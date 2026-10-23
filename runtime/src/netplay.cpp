#include "galaxy/netplay.h"
#include "galaxy/netplay_session.h"

#include "galaxy/coop.h"
#include "galaxy/coop_guest.h"
#include "guest_access.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace galaxy {

// Groundwork for rollback netcode (docs/NETPLAY.md): the game's per-frame controller data recorded and replayed
// at the one place both channels are final, and hashes of guest memory, for the determinism probe.
//
// The frame is the game's own: WPadHolder::updateReadDataOnly calls KPADRead for each channel once a game frame
// from kFrameReadSite (JUTException::readPad's call, a diagnostic, is not one). Channel 0's synthetic answer
// (native_runtime.cpp) and channel 1's (coop_guest.cpp fill_partner_kpad) are both written by the time the
// after-hook runs: the KPADStatus at the buffer and the sample count in r3.
//
// Probe switches: GALAXY_INPUT_RECORD / GALAXY_INPUT_REPLAY=<file>; GALAXY_STATE_HASH=<file> with _EVERY=<n>,
// _FROM=<frame> and _CHUNKS=1 (per-megabyte hashes); GALAXY_STATE_DUMP=<frame>,<file>; GALAXY_STATE_EXIT_FRAME=<n>.
// tools/agent/determinism.py drives them.

namespace {

constexpr std::uint32_t kFrameReadSite = 0x803AB2B0u;  // WPadHolder::updateReadDataOnly's KPADRead
constexpr std::uint32_t kStatusSize = 0x84u;           // KPADStatus
constexpr std::uint32_t kChannels = 2u;                // the two players

struct Sample {
    std::uint32_t count = 0u;
    std::array<std::uint8_t, kStatusSize> status{};
};

struct Netplay {
    bool initialized = false;
    std::FILE* record = nullptr;
    std::FILE* hashes = nullptr;
    std::uint32_t hash_every = 60u;
    std::uint32_t hash_from = 0u;              // GALAXY_STATE_HASH_FROM: no hashes before this frame
    std::uint32_t exit_frame = 0xFFFFFFFFu;    // GALAXY_STATE_EXIT_FRAME: quit when this frame starts
    bool hash_chunks = false;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Sample> replay;  // (frame, channel)
    std::uint32_t frame = 0u;    // game frames so far (channel 0 reads at kFrameReadSite)
    std::uint32_t peek_sequence = 0u;  // EFB peeks so far this frame (netplay_efb_peek)
    std::uint32_t pending_command = 0u;  // a NetCommand from this frame's inputs (netplay_take_command)
    std::uint32_t channel = 0xFFFFFFFFu;
    std::uint32_t buffer = 0u;
    std::uint32_t other_channel = 0xFFFFFFFFu;  // a KPADRead off the frame site, online
    std::uint32_t other_buffer = 0u;
    std::uint32_t dump_frame = 0xFFFFFFFFu;
    std::string dump_path;
    std::uint32_t rate_every = 0u;   // GALAXY_NETPLAY_RATE=<frames>: log simulated frames per second
    std::uint64_t rate_start_qpc = 0u;
    NetplaySnapshotHooks snapshot_hooks{};
    void (*digest_trace)(void* user) = nullptr;
    void* digest_user = nullptr;
    std::uint32_t snapshot_save_frame = 0xFFFFFFFFu;     // GALAXY_SNAPSHOT_TEST
    std::uint32_t snapshot_restore_frame = 0xFFFFFFFFu;
    bool snapshot_saved = false;
    bool snapshot_restored = false;
    bool resimulating = false;           // between the rollback and the frame it was rolled back from
    std::uint32_t write_frame = 0xFFFFFFFFu;  // GALAXY_SNAPSHOT_WRITE (hot join)
    std::string write_path;
    std::uint32_t load_frame = 0xFFFFFFFFu;   // GALAXY_SNAPSHOT_LOAD
    std::string load_path;
    std::uint64_t resim_start_qpc = 0u;
};

Netplay& netplay() {
    static Netplay n;
    return n;
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0u;
    std::string out;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        out = value;
        std::free(value);
    }
    return out;
}

void initialize(Netplay& n) {
    n.initialized = true;
    if (const std::string path = env("GALAXY_INPUT_RECORD"); !path.empty()) {
        if (fopen_s(&n.record, path.c_str(), "wb") != 0) n.record = nullptr;
        std::fprintf(stderr, "[netplay] recording input to %s%s\n", path.c_str(), n.record ? "" : " (cannot open)");
    }
    if (const std::string path = env("GALAXY_INPUT_REPLAY"); !path.empty()) {
        std::FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "rb") == 0 && file != nullptr) {
            std::uint32_t head[3];
            Sample sample;
            while (std::fread(head, sizeof(head), 1, file) == 1 &&
                   std::fread(sample.status.data(), kStatusSize, 1, file) == 1) {
                sample.count = head[2];
                n.replay[{head[0], head[1]}] = sample;
            }
            std::fclose(file);
        }
        std::fprintf(stderr, "[netplay] replaying %zu channel frames from %s\n", n.replay.size(), path.c_str());
    }
    if (const std::string path = env("GALAXY_STATE_HASH"); !path.empty()) {
        if (fopen_s(&n.hashes, path.c_str(), "w") != 0) n.hashes = nullptr;
        if (const std::string every = env("GALAXY_STATE_HASH_EVERY"); !every.empty()) {
            n.hash_every = static_cast<std::uint32_t>(std::max(1, std::atoi(every.c_str())));
        }
        n.hash_chunks = !env("GALAXY_STATE_HASH_CHUNKS").empty();
        if (const std::string from = env("GALAXY_STATE_HASH_FROM"); !from.empty()) {
            n.hash_from = static_cast<std::uint32_t>(std::strtoul(from.c_str(), nullptr, 10));
        }
    }
    if (const std::string test = env("GALAXY_SNAPSHOT_TEST"); !test.empty()) {
        if (const std::size_t comma = test.find(','); comma != std::string::npos) {
            n.snapshot_save_frame = static_cast<std::uint32_t>(std::strtoul(test.substr(0, comma).c_str(), nullptr, 10));
            n.snapshot_restore_frame =
                static_cast<std::uint32_t>(std::strtoul(test.substr(comma + 1u).c_str(), nullptr, 10));
            std::fprintf(stderr, "[netplay] snapshot test: save at frame %u, roll back to it at frame %u\n",
                         n.snapshot_save_frame, n.snapshot_restore_frame);
        }
    }
    for (const auto& [name, frame, path] : {std::tuple{"GALAXY_SNAPSHOT_WRITE", &n.write_frame, &n.write_path},
                                            std::tuple{"GALAXY_SNAPSHOT_LOAD", &n.load_frame, &n.load_path}}) {
        const std::string value = env(name);
        if (const std::size_t comma = value.find(','); comma != std::string::npos) {
            *frame = static_cast<std::uint32_t>(std::strtoul(value.substr(0, comma).c_str(), nullptr, 10));
            *path = value.substr(comma + 1u);
        }
    }
    if (const std::string rate = env("GALAXY_NETPLAY_RATE"); !rate.empty()) {
        n.rate_every = static_cast<std::uint32_t>(std::strtoul(rate.c_str(), nullptr, 10));
    }
    if (const std::string exit_at = env("GALAXY_STATE_EXIT_FRAME"); !exit_at.empty()) {
        n.exit_frame = static_cast<std::uint32_t>(std::strtoul(exit_at.c_str(), nullptr, 10));
    }
    if (const std::string dump = env("GALAXY_STATE_DUMP"); !dump.empty()) {
        if (const std::size_t comma = dump.find(','); comma != std::string::npos) {
            n.dump_frame = static_cast<std::uint32_t>(std::strtoul(dump.substr(0, comma).c_str(), nullptr, 10));
            n.dump_path = dump.substr(comma + 1u);
        }
    }
}

std::uint64_t fnv1a(const std::byte* data, std::size_t size, std::uint64_t h = 0xCBF29CE484222325ull) {
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint8_t>(data[i]);
        h *= 0x100000001B3ull;
    }
    return h;
}

// MEM1, the 24 MB the game's state lives in (heaps, actors, the stage), as one hash and per-megabyte hashes.
void write_hash(Netplay& n, const GuestMemoryV1* memory) {
    if (n.hashes == nullptr || memory == nullptr) return;
    for (std::uint32_t r = 0; r < memory->region_count; ++r) {
        const GuestMemoryRegionV1& region = memory->regions[r];
        if (region.host_base == nullptr || region.size != 0x01800000u) continue;
        std::fprintf(n.hashes, "%u %016llx", n.frame,
                     static_cast<unsigned long long>(fnv1a(region.host_base, region.size)));
        if (n.hash_chunks) {
            for (std::uint32_t at = 0; at < region.size; at += 0x100000u) {
                std::fprintf(n.hashes, " %08llx",
                             static_cast<unsigned long long>(fnv1a(region.host_base + at, 0x100000u) & 0xFFFFFFFFull));
            }
        }
        std::fprintf(n.hashes, "\n");
        std::fflush(n.hashes);
        return;
    }
}

// GALAXY_STATE_DUMP="<frame>,<path>": MEM1 as it is when that frame starts, raw, to diff two runs' state.
void write_dump(Netplay& n, const GuestMemoryV1* memory) {
    // MEM1 to the path, MEM2 (64 MB) beside it as <path>.mem2.
    if (memory == nullptr) return;
    for (std::uint32_t r = 0; r < memory->region_count; ++r) {
        const GuestMemoryRegionV1& region = memory->regions[r];
        if (region.host_base == nullptr || (region.size != 0x01800000u && region.size != 0x04000000u)) continue;
        const std::string path = region.size == 0x01800000u ? n.dump_path : n.dump_path + ".mem2";
        std::FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") == 0 && file != nullptr) {
            std::fwrite(region.host_base, 1, region.size, file);
            std::fclose(file);
            std::fprintf(stderr, "[netplay] guest memory at frame %u dumped to %s\n", n.frame, path.c_str());
        }
    }
}

}  // namespace

void netplay_kpad_before(PpcContext* context, const GuestMemoryV1* memory, std::uint32_t guest_pc) {
    Netplay& n = netplay();
    if (!n.initialized) initialize(n);
    n.channel = 0xFFFFFFFFu;
    if (guest_pc != kFrameReadSite) {
        // Another KPADRead (JUTException::readPad's, a diagnostic): online it must see the synchronised input
        // too, never this PC's own controller.
        if (net_session_requested() && context->gpr[3] < kChannels) {
            n.other_channel = context->gpr[3];
            n.other_buffer = context->gpr[4];
        }
        return;
    }
    n.channel = context->gpr[3];
    n.buffer = context->gpr[4];
    if (n.channel == 0u) {
        n.peek_sequence = 0u;
        net_frame_begin(n.frame, memory);
        // A frame starts: its state before this frame's input.
        if (n.hashes != nullptr && n.frame >= n.hash_from && n.frame % n.hash_every == 0u) write_hash(n, memory);
        if (n.dump_frame == n.frame && !n.dump_path.empty()) write_dump(n, memory);
        if (n.digest_trace != nullptr && netplay_tracing()) n.digest_trace(n.digest_user);
        if (n.rate_every != 0u && n.frame % n.rate_every == 0u) {
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const auto ticks = static_cast<std::uint64_t>(now.QuadPart);
            if (n.rate_start_qpc != 0u) {
                const double seconds = static_cast<double>(ticks - n.rate_start_qpc) /
                                       static_cast<double>(frequency.QuadPart);
                std::fprintf(stderr, "[netplay-rate] frame %u: %.1f frames/s (%.2fx real time)\n", n.frame,
                             n.rate_every / seconds, n.rate_every / seconds / 60.0);
            }
            n.rate_start_qpc = ticks;
        }
        // GALAXY_SIM_PROFILE_WINDOW=<from>,<to> (with GALAXY_SIM_PROFILE=1 GALAXY_SIM_PROFILE_RESIM=1): a profile of
        // those frames alone, unrendered as a re-simulation is.
        static const std::pair<std::uint32_t, std::uint32_t> profile_window = [] {
            const std::string value = env("GALAXY_SIM_PROFILE_WINDOW");
            const std::size_t comma = value.find(',');
            if (comma == std::string::npos) return std::pair<std::uint32_t, std::uint32_t>{0u, 0u};
            return std::pair<std::uint32_t, std::uint32_t>{
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)),
                static_cast<std::uint32_t>(std::strtoul(value.c_str() + comma + 1u, nullptr, 10))};
        }();
        if (profile_window.second != 0u && n.snapshot_hooks.resimulating != nullptr &&
            (n.frame == profile_window.first || n.frame == profile_window.second)) {
            n.snapshot_hooks.resimulating(n.snapshot_hooks.user, n.frame == profile_window.first);
        }
        if (n.resimulating && n.frame == n.snapshot_restore_frame) {
            n.resimulating = false;
            if (n.snapshot_hooks.resimulating != nullptr) n.snapshot_hooks.resimulating(n.snapshot_hooks.user, false);
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const double ms = 1000.0 * static_cast<double>(static_cast<std::uint64_t>(now.QuadPart) - n.resim_start_qpc) /
                              static_cast<double>(frequency.QuadPart);
            const std::uint32_t frames = n.snapshot_restore_frame - n.snapshot_save_frame;
            std::fprintf(stderr, "[netplay] re-simulated %u frames in %.1f ms (%.2f ms a frame)\n", frames, ms,
                         ms / frames);
        }
        if (n.snapshot_hooks.save != nullptr) {
            if (!n.snapshot_saved && n.frame == n.snapshot_save_frame) {
                n.snapshot_saved = true;
                const std::uint32_t frame = n.frame;
                const std::uint32_t channel = n.channel;
                const std::uint32_t buffer = n.buffer;
                if (n.snapshot_hooks.save(n.snapshot_hooks.user)) {
                    // Rolled back: this is frame `frame` again, in this read.
                    n.frame = frame;
                    n.channel = channel;
                    n.buffer = buffer;
                    std::fprintf(stderr, "[netplay] rolled back to frame %u\n", frame);
                    n.resimulating = true;
                    LARGE_INTEGER now{};
                    QueryPerformanceCounter(&now);
                    n.resim_start_qpc = static_cast<std::uint64_t>(now.QuadPart);
                    if (n.snapshot_hooks.resimulating != nullptr && env("GALAXY_SNAPSHOT_RENDER_RESIM").empty()) {
                        n.snapshot_hooks.resimulating(n.snapshot_hooks.user, true);
                    }
                } else {
                    std::fprintf(stderr, "[netplay] saved at frame %u\n", frame);
                }
            } else if (n.snapshot_saved && !n.snapshot_restored && n.frame == n.snapshot_restore_frame) {
                n.snapshot_restored = true;
                std::fprintf(stderr, "[netplay] restoring at frame %u\n", n.frame);
                std::fflush(stderr);
                n.snapshot_hooks.restore(n.snapshot_hooks.user);
            }
        }
        if (n.snapshot_hooks.write != nullptr && n.frame == n.write_frame) {
            n.write_frame = 0xFFFFFFFFu;
            const std::uint32_t frame = n.frame;
            const std::uint32_t channel = n.channel;
            const std::uint32_t buffer = n.buffer;
            if (n.snapshot_hooks.write(n.snapshot_hooks.user, n.write_path.c_str(), frame)) {
                // Another process loaded the file and resumed here: from now on its frames are the writer's.
                n.frame = frame;
                n.channel = channel;
                n.buffer = buffer;
                n.write_frame = 0xFFFFFFFFu;
                n.load_frame = 0xFFFFFFFFu;
                std::fprintf(stderr, "[netplay] joined: resumed at frame %u from a snapshot file\n", frame);
            }
        }
        if (n.snapshot_hooks.load != nullptr && n.frame == n.load_frame) {
            n.load_frame = 0xFFFFFFFFu;
            std::fprintf(stderr, "[netplay] frame %u: loading %s\n", n.frame, n.load_path.c_str());
            std::fflush(stderr);
            n.snapshot_hooks.load(n.snapshot_hooks.user, n.load_path.c_str());
        }
        if (n.frame >= n.exit_frame) {
            // A probe run is over: its files are flushed and nothing else of it matters.
            if (n.hashes != nullptr) std::fclose(n.hashes);
            if (n.record != nullptr) std::fclose(n.record);
            std::fprintf(stderr, "[netplay] frame %u reached: exiting\n", n.frame);
            std::fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

void netplay_kpad_after(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                        std::uint32_t guest_pc) {
    Netplay& n = netplay();
    if (guest_pc != kFrameReadSite && n.other_channel < kChannels && services != nullptr) {
        // Online, off the frame read site: the input this frame applied for that channel (the frame counter has
        // moved past it once the second channel was read).
        const std::uint32_t channel = n.other_channel;
        n.other_channel = 0xFFFFFFFFu;
        const std::uint32_t applied = n.frame == 0u ? 0u : n.frame - 1u;
        const NetInput& input = net_input(applied, channel == 0u ? 0u : 1u);
        const guest_access::Guest guest(context, memory, services, guest_pc);
        for (std::uint32_t i = 0; i < kStatusSize; i += 4u) {
            guest.set_u32(n.other_buffer + i, (static_cast<std::uint32_t>(input.status[i]) << 24u) |
                                                  (static_cast<std::uint32_t>(input.status[i + 1u]) << 16u) |
                                                  (static_cast<std::uint32_t>(input.status[i + 2u]) << 8u) |
                                                  input.status[i + 3u]);
        }
        context->gpr[3] = input.count;
        return;
    }
    if (guest_pc != kFrameReadSite || n.channel >= kChannels || services == nullptr) {
        n.channel = 0xFFFFFFFFu;
        return;
    }
    const std::uint32_t channel = n.channel;
    n.channel = 0xFFFFFFFFu;
    const guest_access::Guest guest(context, memory, services, guest_pc);
    // KPADStatus is word aligned: whole words, big-endian as the guest has them.
    auto read = [&](std::uint32_t address, std::uint8_t* out, std::uint32_t size) {
        for (std::uint32_t i = 0; i < size; i += 4u) {
            const std::uint32_t word = guest.u32(address + i);
            out[i] = static_cast<std::uint8_t>(word >> 24u);
            out[i + 1u] = static_cast<std::uint8_t>(word >> 16u);
            out[i + 2u] = static_cast<std::uint8_t>(word >> 8u);
            out[i + 3u] = static_cast<std::uint8_t>(word);
        }
    };
    auto write = [&](std::uint32_t address, const std::uint8_t* in, std::uint32_t size) {
        for (std::uint32_t i = 0; i < size; i += 4u) {
            guest.set_u32(address + i, (static_cast<std::uint32_t>(in[i]) << 24u) |
                                           (static_cast<std::uint32_t>(in[i + 1u]) << 16u) |
                                           (static_cast<std::uint32_t>(in[i + 2u]) << 8u) | in[i + 3u]);
        }
    };
    if (net_session_requested()) {
        // Online: this PC's controller (the local first-player read) is its player's input `delay` frames on;
        // the first player plays the host's input and the second the joiner's, on both PCs.
        if (channel == 0u) {
            // The game's Remote is neutral in deterministic mode (native_host.cpp): this PC's player is its
            // controller as the input thread published it, a KPAD status a frame.
            static coop::KpadSynth s_local_kpad;
            const coop::PartnerInput controller = coop_local_input();
            const coop::KpadStatusBytes local = s_local_kpad.next(controller);
            if (n.rate_every != 0u && n.frame % n.rate_every == 0u) {
                std::fprintf(stderr, "[netplay-input] frame %u: buttons=%04x stick=(%.2f,%.2f) pointer=%d (%.2f,%.2f)\n",
                             n.frame, controller.buttons, controller.stick_x, controller.stick_y,
                             controller.pointer_valid ? 1 : 0, controller.pointer_x, controller.pointer_y);
            }
            net_local_input(n.frame, 1u, local.data());
        }
        const NetInput& input = net_input(n.frame, channel == 0u ? 0u : 1u);
        if (channel == 0u) {
            // A player's online-menu command in this frame's inputs (either's): carried out at the next frame's
            // start on both PCs (agent_guest.cpp rejoin_frame).
            const std::uint32_t command = input.command | net_input(n.frame, 1u).command;
            if (command != 0u) n.pending_command = command;
        }
        if (channel == 0u) {
            // A rejoin passing the logo (agent_guest.cpp): A for the first player, on both PCs alike.
            static bool s_rejoin_a = false;
            const bool press = netplay_rejoin_press_a();
            if (press || s_rejoin_a) {
                std::array<std::uint8_t, kStatusSize> status = input.status;
                const auto get = [&](std::size_t at) {
                    return (static_cast<std::uint32_t>(status[at]) << 24u) | (static_cast<std::uint32_t>(status[at + 1u]) << 16u) |
                           (static_cast<std::uint32_t>(status[at + 2u]) << 8u) | status[at + 3u];
                };
                const auto put = [&](std::size_t at, std::uint32_t value) {
                    for (int i = 0; i < 4; ++i) status[at + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
                };
                const std::uint32_t a = coop::button::kA;
                put(coop::kpad::kHold, press ? get(coop::kpad::kHold) | a : get(coop::kpad::kHold));
                if (press && !s_rejoin_a) put(coop::kpad::kTrig, get(coop::kpad::kTrig) | a);
                if (!press && s_rejoin_a) put(coop::kpad::kRelease, get(coop::kpad::kRelease) | a);
                s_rejoin_a = press;
                write(n.buffer, status.data(), kStatusSize);
                context->gpr[3] = std::max<std::uint32_t>(input.count, 1u);
                if (channel == kChannels - 1u) ++n.frame;
                return;
            }
        }
        write(n.buffer, input.status.data(), kStatusSize);
        context->gpr[3] = input.count;
    } else if (auto it = n.replay.find({n.frame, channel}); it != n.replay.end()) {
        write(n.buffer, it->second.status.data(), kStatusSize);
        context->gpr[3] = it->second.count;
    }
    if (n.record != nullptr) {
        Sample sample;
        sample.count = context->gpr[3];
        if (static_cast<std::int32_t>(sample.count) > 0) read(n.buffer, sample.status.data(), kStatusSize);
        const std::uint32_t head[3] = {n.frame, channel, sample.count};
        std::fwrite(head, sizeof(head), 1, n.record);
        std::fwrite(sample.status.data(), kStatusSize, 1, n.record);
    }
    if (channel == kChannels - 1u) ++n.frame;
}

std::uint32_t netplay_pending_channel() noexcept {
    return netplay().channel;
}

std::uint32_t netplay_frame() noexcept {
    return netplay().frame;
}

std::uint32_t netplay_take_command() noexcept {
    const std::uint32_t command = netplay().pending_command;
    netplay().pending_command = 0u;
    return command;
}

namespace {
std::FILE* trace_file() noexcept {
    static std::FILE* file = [] {
        std::FILE* f = nullptr;
        const std::string path = env("GALAXY_DETERMINISTIC_TRACE");
        if (!path.empty() && fopen_s(&f, path.c_str(), "w") != 0) f = nullptr;
        return f;
    }();
    return file;
}
}  // namespace

std::uint32_t netplay_efb_peek(bool depth, std::uint32_t local_value) noexcept {
    Netplay& n = netplay();
    if (!n.initialized) initialize(n);
    // Peek answers ride in the input stream as channel kPeekChannel + their order in the frame (count = value).
    constexpr std::uint32_t kPeekChannel = 0x100u;
    if (net_session_requested()) return net_peek(n.frame, n.peek_sequence++, local_value);
    const std::uint32_t channel = kPeekChannel + (n.peek_sequence++ & 0xFFFFu) + (depth ? 0u : 0x10000u);
    if (!n.replay.empty()) {
        const auto it = n.replay.find({n.frame, channel});
        if (it != n.replay.end()) return it->second.count;
    }
    if (n.record != nullptr) {
        const std::uint32_t head[3] = {n.frame, channel, local_value};
        static const std::array<std::uint8_t, kStatusSize> kEmpty{};
        std::fwrite(head, sizeof(head), 1, n.record);
        std::fwrite(kEmpty.data(), kStatusSize, 1, n.record);
    }
    return local_value;
}

void netplay_set_snapshot_hooks(const NetplaySnapshotHooks& hooks) noexcept {
    netplay().snapshot_hooks = hooks;
}

void netplay_set_digest_hook(void (*trace)(void* user), void* user) noexcept {
    netplay().digest_trace = trace;
    netplay().digest_user = user;
}

bool netplay_tracing() noexcept {
    return trace_file() != nullptr;
}

void netplay_trace(const char* format, ...) noexcept {
    std::FILE* file = trace_file();
    if (file == nullptr) return;
    std::fprintf(file, "%u ", netplay().frame);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

}  // namespace galaxy
