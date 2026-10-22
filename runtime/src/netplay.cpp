#include "galaxy/netplay.h"

#include "guest_access.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace galaxy {

// Groundwork for rollback netcode (docs/NETPLAY.md): the game's per-frame controller data recorded and replayed
// at the one place both channels are final, and hashes of guest memory, for the determinism probe.
//
// The frame is the game's own: WPadHolder::updateReadDataOnly calls KPADRead for each channel once a game frame
// from kFrameReadSite (JUTException::readPad's call, a diagnostic, is not one). Channel 0's synthetic answer
// (native_runtime.cpp) and channel 1's (coop_guest.cpp fill_partner_kpad) are both written by the time the
// after-hook runs: the KPADStatus at the buffer and the sample count in r3.

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
    bool hash_chunks = false;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Sample> replay;  // (frame, channel)
    std::uint32_t frame = 0u;    // game frames so far (channel 0 reads at kFrameReadSite)
    std::uint32_t channel = 0xFFFFFFFFu;
    std::uint32_t buffer = 0u;
    std::uint32_t dump_frame = 0xFFFFFFFFu;
    std::string dump_path;
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
    if (memory == nullptr) return;
    for (std::uint32_t r = 0; r < memory->region_count; ++r) {
        const GuestMemoryRegionV1& region = memory->regions[r];
        if (region.host_base == nullptr || region.size != 0x01800000u) continue;
        std::FILE* file = nullptr;
        if (fopen_s(&file, n.dump_path.c_str(), "wb") == 0 && file != nullptr) {
            std::fwrite(region.host_base, 1, region.size, file);
            std::fclose(file);
            std::fprintf(stderr, "[netplay] MEM1 at frame %u dumped to %s\n", n.frame, n.dump_path.c_str());
        }
        return;
    }
}

}  // namespace

void netplay_kpad_before(PpcContext* context, const GuestMemoryV1* memory, std::uint32_t guest_pc) {
    Netplay& n = netplay();
    if (!n.initialized) initialize(n);
    n.channel = 0xFFFFFFFFu;
    if (guest_pc != kFrameReadSite) return;
    n.channel = context->gpr[3];
    n.buffer = context->gpr[4];
    if (n.channel == 0u) {
        // A frame starts: its state before this frame's input.
        if (n.hashes != nullptr && n.frame % n.hash_every == 0u) write_hash(n, memory);
        if (n.dump_frame == n.frame && !n.dump_path.empty()) write_dump(n, memory);
    }
}

void netplay_kpad_after(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                        std::uint32_t guest_pc) {
    Netplay& n = netplay();
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
    if (auto it = n.replay.find({n.frame, channel}); it != n.replay.end()) {
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

}  // namespace galaxy
