#include "galaxy/netplay_session.h"

#include "galaxy/gx_d3d12.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace galaxy {
namespace {

// Wire format (little-endian, one UDP datagram each):
//   Header, then by type:
//   Hello / HelloAck: u32 delay, u64 build id
//   Inputs: u32 first frame, u32 frame count (<= kInputsPerPacket), u32 newest frame received from the peer,
//           then per frame: u32 count, u32 peek count, kNetMaxPeeks x u32 peeks, kNetStatusSize status bytes
//   Hash: u32 frame, u64 MEM1 hash
constexpr std::uint32_t kMagic = 0x504E424Eu;  // "NBNP"
constexpr std::uint8_t kVersion = 1u;
enum class Packet : std::uint8_t { Hello = 1, HelloAck = 2, Inputs = 3, Hash = 4 };
constexpr std::uint32_t kInputsPerPacket = 8u;  // every packet repeats the last frames: a lost one costs nothing
constexpr std::uint32_t kRing = 1024u;          // frames of inputs kept
constexpr std::uint32_t kHashEvery = 120u;
constexpr double kFramePeriodSeconds = 1001.0 / 60000.0;  // NTSC 59.94 Hz

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic;
    std::uint8_t type;
    std::uint8_t version;
    std::uint16_t reserved;
};
struct WireInput {
    std::uint32_t count;
    std::uint32_t peek_count;
    std::uint32_t peeks[kNetMaxPeeks];
    std::uint8_t status[kNetStatusSize];
};
struct WireHello {
    Header header;
    std::uint32_t delay;
    std::uint64_t build;
};
struct WireHash {
    Header header;
    std::uint32_t frame;
    std::uint64_t hash;
};
#pragma pack(pop)
static_assert(sizeof(WireHello) == sizeof(Header) + 12u && sizeof(WireHash) == sizeof(Header) + 12u);

struct Slot {
    std::uint32_t frame = 0xFFFFFFFFu;  // which frame this slot holds
    NetInput input;
};

struct Session {
    bool initialized = false;
    bool requested = false;
    bool host = false;
    std::uint32_t delay = 3u;
    std::uint64_t build_id = 0u;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in peer{};
    bool peer_known = false;
    bool connected = false;
    std::vector<Slot> inputs[2];                  // per player, ring by frame
    std::vector<std::array<std::uint32_t, kNetMaxPeeks>> local_peeks;  // host: answers by frame
    std::vector<std::uint32_t> local_peek_counts;
    std::uint32_t newest_local_frame = 0u;        // newest frame this PC has an input for
    std::uint32_t newest_remote_frame = 0u;       // newest frame received from the peer (0: none)
    bool any_remote = false;
    std::uint32_t peer_newest_of_ours = 0u;       // what the peer says it has of ours
    LARGE_INTEGER frequency{};
    std::int64_t pace_origin = 0;                 // QPC at pace_origin_frame
    std::uint32_t pace_origin_frame = 0u;
    bool paced = false;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> my_hashes;  // (frame, hash), recent
    std::uint64_t stalls = 0u;
    double stalled_ms = 0.0;
};

Session& session() {
    static Session s;
    return s;
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

// Size and write time of the executable and the game module: two PCs must run the same build.
std::uint64_t build_identity() {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const wchar_t* path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return;
        for (const std::uint32_t word : {data.nFileSizeLow, data.nFileSizeHigh, data.ftLastWriteTime.dwLowDateTime,
                                         data.ftLastWriteTime.dwHighDateTime}) {
            h = (h ^ word) * 1099511628211ull;
        }
    };
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    mix(exe);
    std::wstring module(exe);
    if (const auto slash = module.find_last_of(L"\\/"); slash != std::wstring::npos) module.resize(slash + 1u);
    module += L"RMGE01_game.dll";
    mix(module.c_str());
    return h;
}

void send_raw(Session& s, const void* data, int size) {
    if (s.socket == INVALID_SOCKET || !s.peer_known) return;
    (void)sendto(s.socket, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&s.peer),
                 sizeof(s.peer));
}

void send_hello(Session& s, Packet type) {
    const WireHello packet{{kMagic, static_cast<std::uint8_t>(type), kVersion, 0u}, s.delay, s.build_id};
    send_raw(s, &packet, sizeof(packet));
}

Slot& slot(Session& s, unsigned player, std::uint32_t frame) {
    return s.inputs[player][frame % kRing];
}

unsigned me(const Session& s) { return s.host ? 0u : 1u; }

void send_inputs(Session& s) {
    if (!s.connected) return;
    std::vector<std::uint8_t> packet(sizeof(Header) + 12u + kInputsPerPacket * sizeof(WireInput));
    const std::uint32_t last = s.newest_local_frame;
    const std::uint32_t first = last + 1u >= kInputsPerPacket ? last + 1u - kInputsPerPacket : 0u;
    std::uint32_t count = 0u;
    std::uint8_t* out = packet.data() + sizeof(Header) + 12u;
    for (std::uint32_t frame = first; frame <= last; ++frame) {
        const Slot& from = slot(s, me(s), frame);
        if (from.frame != frame) continue;
        WireInput wire{};
        wire.count = from.input.count;
        wire.peek_count = from.input.peek_count;
        std::memcpy(wire.peeks, from.input.peeks.data(), sizeof(wire.peeks));
        std::memcpy(wire.status, from.input.status.data(), kNetStatusSize);
        std::memcpy(out + count * sizeof(WireInput), &wire, sizeof(wire));
        ++count;
    }
    if (count == 0u) return;
    const Header header{kMagic, static_cast<std::uint8_t>(Packet::Inputs), kVersion, 0u};
    std::memcpy(packet.data(), &header, sizeof(header));
    const std::uint32_t fields[3] = {last + 1u - count, count, s.any_remote ? s.newest_remote_frame : 0u};
    std::memcpy(packet.data() + sizeof(Header), fields, sizeof(fields));
    send_raw(s, packet.data(), static_cast<int>(sizeof(Header) + 12u + count * sizeof(WireInput)));
}

void handle_packet(Session& s, const std::uint8_t* data, int size, const sockaddr_in& from) {
    if (size < static_cast<int>(sizeof(Header))) return;
    Header header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != kMagic || header.version != kVersion) return;
    const auto type = static_cast<Packet>(header.type);
    if (type == Packet::Hello || type == Packet::HelloAck) {
        if (size < static_cast<int>(sizeof(Header) + 12u)) return;
        std::uint32_t delay = 0u;
        std::uint64_t build = 0u;
        std::memcpy(&delay, data + sizeof(Header), 4u);
        std::memcpy(&build, data + sizeof(Header) + 4u, 8u);
        if (build != s.build_id || delay != s.delay) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                std::fprintf(stderr, "[net] refused a peer with a different %s\n",
                             build != s.build_id ? "build" : "input delay");
            }
            return;
        }
        if (s.host && type == Packet::Hello) {
            s.peer = from;
            s.peer_known = true;
            send_hello(s, Packet::HelloAck);
        }
        if (!s.connected) {
            s.connected = true;
            char text[64]{};
            inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
            std::fprintf(stderr, "[net] connected to %s:%u (input delay %u frames)\n", text, ntohs(from.sin_port),
                         s.delay);
        }
        return;
    }
    if (!s.connected) return;
    if (type == Packet::Inputs) {
        if (size < static_cast<int>(sizeof(Header) + 12u)) return;
        std::uint32_t fields[3]{};
        std::memcpy(fields, data + sizeof(Header), sizeof(fields));
        const std::uint32_t first = fields[0];
        const std::uint32_t count = std::min(fields[1], kInputsPerPacket);
        if (size < static_cast<int>(sizeof(Header) + 12u + count * sizeof(WireInput))) return;
        s.peer_newest_of_ours = std::max(s.peer_newest_of_ours, fields[2]);
        const unsigned peer = 1u - me(s);
        for (std::uint32_t i = 0; i < count; ++i) {
            WireInput wire{};
            std::memcpy(&wire, data + sizeof(Header) + 12u + i * sizeof(WireInput), sizeof(wire));
            const std::uint32_t frame = first + i;
            Slot& to = slot(s, peer, frame);
            to.frame = frame;
            to.input.count = wire.count;
            to.input.peek_count = std::min(wire.peek_count, kNetMaxPeeks);
            std::memcpy(to.input.peeks.data(), wire.peeks, sizeof(wire.peeks));
            std::memcpy(to.input.status.data(), wire.status, kNetStatusSize);
            if (!s.any_remote || frame > s.newest_remote_frame) s.newest_remote_frame = frame;
            s.any_remote = true;
        }
        return;
    }
    if (type == Packet::Hash) {
        if (size < static_cast<int>(sizeof(Header) + 12u)) return;
        std::uint32_t frame = 0u;
        std::uint64_t hash = 0u;
        std::memcpy(&frame, data + sizeof(Header), 4u);
        std::memcpy(&hash, data + sizeof(Header) + 4u, 8u);
        for (const auto& [mine_frame, mine] : s.my_hashes) {
            if (mine_frame != frame) continue;
            if (mine != hash) {
                std::fprintf(stderr, "[net] DESYNC: MEM1 differs from the peer's at frame %u\n", frame);
            }
            break;
        }
    }
}

void receive(Session& s) {
    if (s.socket == INVALID_SOCKET) return;
    std::uint8_t buffer[2048];
    for (;;) {
        sockaddr_in from{};
        int from_size = sizeof(from);
        const int size = recvfrom(s.socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_size);
        if (size <= 0) return;
        handle_packet(s, buffer, size, from);
    }
}

void initialize(Session& s) {
    s.initialized = true;
    const std::string host_port = env("GALAXY_NET_HOST");
    const std::string join = env("GALAXY_NET_JOIN");
    if (host_port.empty() && join.empty()) return;
    s.requested = true;
    s.host = !host_port.empty();
    if (const std::string delay = env("GALAXY_NET_DELAY"); !delay.empty()) {
        s.delay = static_cast<std::uint32_t>(std::clamp(std::atoi(delay.c_str()), 1, 30));
    }
    s.build_id = build_identity();
    for (auto& ring : s.inputs) ring.assign(kRing, Slot{});
    s.local_peeks.assign(kRing, {});
    s.local_peek_counts.assign(kRing, 0u);
    QueryPerformanceFrequency(&s.frequency);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[net] Winsock did not start\n");
        return;
    }
    s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.socket == INVALID_SOCKET) {
        std::fprintf(stderr, "[net] cannot open a UDP socket\n");
        return;
    }
    u_long nonblocking = 1u;
    ioctlsocket(s.socket, FIONBIO, &nonblocking);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s.host) {
        local.sin_port = htons(static_cast<u_short>(std::atoi(host_port.c_str())));
        if (bind(s.socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            std::fprintf(stderr, "[net] cannot listen on UDP port %s\n", host_port.c_str());
            return;
        }
        std::fprintf(stderr, "[net] hosting on UDP port %s, input delay %u frames\n", host_port.c_str(), s.delay);
    } else {
        const auto colon = join.rfind(':');
        const std::string address = colon == std::string::npos ? join : join.substr(0, colon);
        const std::string port = colon == std::string::npos ? "7777" : join.substr(colon + 1u);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(address.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
            std::fprintf(stderr, "[net] cannot resolve %s\n", join.c_str());
            return;
        }
        std::memcpy(&s.peer, result->ai_addr, sizeof(s.peer));
        freeaddrinfo(result);
        s.peer_known = true;
        (void)bind(s.socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        std::fprintf(stderr, "[net] joining %s, input delay %u frames\n", join.c_str(), s.delay);
    }
}

std::uint64_t mem1_hash(const GuestMemoryV1* memory) {
    if (memory == nullptr) return 0u;
    for (std::uint32_t r = 0; r < memory->region_count; ++r) {
        const GuestMemoryRegionV1& region = memory->regions[r];
        if (region.host_base == nullptr || region.size != 0x01800000u) continue;
        // Eight bytes a step: a few milliseconds for 24 MB.
        std::uint64_t h = 0x9E3779B97F4A7C15ull;
        const auto* words = reinterpret_cast<const std::uint64_t*>(region.host_base);
        for (std::size_t i = 0; i < region.size / 8u; ++i) h = (h ^ words[i]) * 0x100000001B3ull + (h >> 29u);
        return h;
    }
    return 0u;
}

std::int64_t now_qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

}  // namespace

bool net_session_requested() noexcept {
    Session& s = session();
    if (!s.initialized) initialize(s);
    return s.requested;
}

bool net_is_host() noexcept {
    return net_session_requested() && session().host;
}

void net_frame_begin(std::uint32_t frame, const GuestMemoryV1* memory) {
    Session& s = session();
    if (!s.requested) return;
    receive(s);
    if (!s.connected && !s.host) send_hello(s, Packet::Hello);
    s.local_peek_counts[frame % kRing] = 0u;
    // 59.94 Hz: each frame starts at its own deadline from the pacing origin. A stall (waiting for the peer)
    // moves the origin instead of making later frames rush to catch up.
    const std::int64_t period = static_cast<std::int64_t>(kFramePeriodSeconds * static_cast<double>(s.frequency.QuadPart));
    std::int64_t now = now_qpc();
    if (!s.paced) {
        s.paced = true;
        s.pace_origin = now;
        s.pace_origin_frame = frame;
    }
    std::int64_t deadline = s.pace_origin + static_cast<std::int64_t>(frame - s.pace_origin_frame) * period;
    // Running ahead of the peer (it has fewer of our inputs than we have of its): give it a millisecond.
    if (s.connected && s.any_remote && s.newest_local_frame > s.peer_newest_of_ours + s.delay) {
        deadline += s.frequency.QuadPart / 1000;
    }
    if (now > deadline + 6 * period) {
        s.pace_origin = now;
        s.pace_origin_frame = frame;
        deadline = now;
    }
    while (now < deadline) {
        const double remaining_ms = 1000.0 * static_cast<double>(deadline - now) / static_cast<double>(s.frequency.QuadPart);
        if (remaining_ms > 1.5) {
            Sleep(1);
        } else {
            SwitchToThread();
        }
        receive(s);
        now = now_qpc();
    }
    if (frame % kHashEvery == 0u && s.connected) {
        const std::uint64_t hash = mem1_hash(memory);
        s.my_hashes.emplace_back(frame, hash);
        if (s.my_hashes.size() > 64u) s.my_hashes.erase(s.my_hashes.begin());
        const WireHash packet{{kMagic, static_cast<std::uint8_t>(Packet::Hash), kVersion, 0u}, frame, hash};
        send_raw(s, &packet, sizeof(packet));
    }
    if (frame % 600u == 0u && s.stalls != 0u) {
        std::fprintf(stderr, "[net] frame %u: %llu stalls, %.0f ms waiting for the peer\n", frame,
                     static_cast<unsigned long long>(s.stalls), s.stalled_ms);
    }
}

void net_local_input(std::uint32_t frame, std::uint32_t count, const std::uint8_t* status) {
    Session& s = session();
    if (!s.requested) return;
    const std::uint32_t target = frame + s.delay;
    Slot& to = slot(s, me(s), target);
    to.frame = target;
    to.input = NetInput{};
    to.input.count = count;
    if (status != nullptr) std::memcpy(to.input.status.data(), status, kNetStatusSize);
    if (s.host && frame >= 1u) {
        // The host's peek answers of the frame just finished go with this input: both PCs use them at `target`.
        const std::uint32_t previous = frame - 1u;
        to.input.peek_count = std::min(s.local_peek_counts[previous % kRing], kNetMaxPeeks);
        to.input.peeks = s.local_peeks[previous % kRing];
    }
    s.newest_local_frame = target;
    send_inputs(s);
}

const NetInput& net_input(std::uint32_t frame, unsigned player) {
    Session& s = session();
    static const NetInput kNeutral{};
    if (frame < s.delay) return kNeutral;  // before any input could arrive: the same on both PCs
    Slot& from = slot(s, player, frame);
    if (from.frame == frame) return from.input;
    // The peer's input for this frame has not arrived: wait for it (lockstep), resending ours.
    const std::int64_t started = now_qpc();
    std::int64_t last_send = started;
    std::int64_t last_log = started;
    ++s.stalls;
    while (from.frame != frame) {
        receive(s);
        if (from.frame == frame) break;
        const std::int64_t now = now_qpc();
        if (now - last_send > s.frequency.QuadPart / 200) {
            if (!s.connected && !s.host) send_hello(s, Packet::Hello);
            send_inputs(s);
            last_send = now;
        }
        if (now - last_log > s.frequency.QuadPart * 5) {
            std::fprintf(stderr, "[net] frame %u: waiting for the %s\n", frame,
                         s.connected ? "peer's input" : (s.host ? "joiner to connect" : "host to answer"));
            last_log = now;
        }
        galaxy::gx::pump_messages();
        Sleep(0);
    }
    s.stalled_ms += 1000.0 * static_cast<double>(now_qpc() - started) / static_cast<double>(s.frequency.QuadPart);
    // A wait moves the pacing origin: the frames after it run at their normal rate, not in a burst.
    s.paced = false;
    return from.input;
}

std::uint32_t net_peek(std::uint32_t frame, std::uint32_t sequence, std::uint32_t local) noexcept {
    Session& s = session();
    if (s.host && sequence < kNetMaxPeeks) {
        s.local_peeks[frame % kRing][sequence] = local;
        s.local_peek_counts[frame % kRing] = sequence + 1u;
    }
    // Both PCs answer with the host's value carried by the host's input for this frame.
    const NetInput& host_input = net_input(frame, 0u);
    if (host_input.peek_count == 0u) return 0x00FFFFFFu;
    return host_input.peeks[std::min(sequence, host_input.peek_count - 1u)];
}

}  // namespace galaxy
