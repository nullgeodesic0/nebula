#include "galaxy/netplay_session.h"

#include "galaxy/coop.h"
#include "galaxy/coop_guest.h"
#include "galaxy/gx_d3d12.h"
#include "galaxy/net_lobby.h"
#include "galaxy/netplay.h"
#include "galaxy/hot_join.h"
#include "galaxy/runtime_settings.h"

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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
constexpr std::uint8_t kVersion = 3u;
// Bye: the sender has left the session (its online menu). StreamRequest / StreamChunk / Ready: a live join
// (docs/HOT_JOIN.md).
enum class Packet : std::uint8_t {
    Hello = 1, HelloAck = 2, Inputs = 3, Hash = 4, BundleRequest = 5, BundleChunk = 6, Bye = 7,
    StreamRequest = 8, StreamChunk = 9, Ready = 10,
};
constexpr std::uint32_t kChunkBytes = 1024u;
constexpr const wchar_t* kNandMetaStream = L":galaxy.isfs.meta";  // native_host.cpp's per-file NAND metadata
constexpr std::uint32_t kChunkBurst = 32u;

// Settings that change what the game computes: the joiner plays with the host's. Its own controls stay.
constexpr const char* kHostSettingKeys[] = {
    "text_speed", "language", "motion_style", "custom_animations", "custom_model", "custom_voice", "odyssey_moveset",
    "moveset_", "coop", "audio_",
};
constexpr std::uint32_t kInputsPerPacket = 8u;  // every packet repeats the last frames: a lost one costs nothing
constexpr std::uint32_t kRing = 1024u;          // frames of inputs kept
constexpr std::uint32_t kHashEvery = 120u;
// Live join: the stream's chunks, how many the joiner may ask for at once, the joiner's own frame that loads the
// snapshot (its title is up), and how far ahead of the host the join frame is put once the joiner has caught up.
constexpr std::uint32_t kStreamChunk = 1200u;
constexpr std::uint32_t kStreamWindowMax = 512u;
constexpr std::uint32_t kStreamWindowMin = 16u;
constexpr std::uint32_t kLoadFrame = 30u;
constexpr std::uint32_t kJoinMargin = 60u;
constexpr std::uint32_t kStreamMagic = 0x4A4C424Eu;  // "NBLJ"
constexpr std::uint32_t kNone = 0xFFFFFFFFu;

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
    std::uint32_t command;
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
struct WireBundleRequest {
    Header header;
    std::uint32_t first_chunk;
};
struct WireBundleChunk {
    Header header;
    std::uint32_t index;
    std::uint32_t total_size;
    std::uint16_t length;
    std::uint8_t data[kChunkBytes];
};
// Live join. The stream: u32 magic, u32 snapshot frame F, u32 compressed size, the compressed snapshot image
// (hot_join::make_image), then a WireRecord per frame from F on: the host's input as that frame used it.
struct WireStreamRequest {
    Header header;
    std::uint32_t first_chunk;
    std::uint32_t window;
};
struct WireStreamChunk {
    Header header;
    std::uint32_t index;
    std::uint16_t length;
    std::uint8_t data[kStreamChunk];
};
struct WireReady {
    Header header;
    std::uint32_t frame;
};
struct WireRecord {
    std::uint32_t frame;
    std::uint32_t join_frame;  // kNone until the host has named it
    WireInput input;
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
    std::vector<std::pair<std::uint32_t, std::uint64_t>> my_hashes;  // (frame, hash), recent
    std::uint64_t stalls = 0u;
    double stalled_ms = 0.0;
    // Rollback (GALAXY_NET_ROLLBACK=1): a frame whose peer input has not arrived runs on a prediction (the peer's
    // newest input, held) instead of waiting, up to max_ahead frames past the newest that arrived; when the real one
    // differs, netplay.cpp restores the frame and simulates forward again (net_rollback_target).
    bool rollback = false;
    std::uint32_t max_ahead = 8u;
    std::vector<NetInput> predicted;               // by frame, the input a prediction used
    std::vector<std::uint8_t> was_predicted;
    std::uint32_t unconfirmed_from = kNone;        // the oldest frame run on a prediction not yet checked
    std::uint64_t rollbacks = 0u;
    std::uint64_t rolled_frames = 0u;
    std::uint64_t predictions = 0u;
    std::uint32_t last_hashed_frame = kNone;
    bool resimulating = false;  // netplay.cpp re-running frames after a rollback: nothing local is sampled again
    bool must_wait = false;     // the next net_input waits rather than predicts (an EFB peek's input)
    std::atomic<bool> peer_left{false};                 // set on the game thread, read by the lobby's
    std::atomic<std::uint32_t> pending_command{0u};     // the next local input's NetCommand (the menu's thread)
    // The session bundle: the host's save files and game settings (host: built on the first request; joiner:
    // the chunks received so far).
    std::vector<std::uint8_t> bundle;
    bool bundle_built = false;
    std::vector<bool> bundle_have;
    std::uint32_t bundle_size = 0u;
    bool bundle_size_known = false;
    // Live join (docs/HOT_JOIN.md).
    bool live = false;
    enum class Live : std::uint8_t { Solo, Starting, Streaming, Joined, Booting, CatchUp, Edge };
    Live phase = Live::Solo;
    std::atomic<bool> accept_pending{false};  // the lobby accepted (its thread); the game thread opens the session
    unsigned live_port = 0u;
    bool snapshot_wanted = false;
    std::uint32_t snapshot_frame = kNone;  // F
    std::uint32_t join_frame = kNone;      // J
    std::uint32_t current_frame = 0u;
    // Host: the stream as built so far (the head once the snapshot is compressed) and its compressor.
    std::mutex stream_mutex;
    std::vector<std::uint8_t> stream_head;
    std::vector<std::uint8_t> records;
    bool stream_ready = false;
    std::thread compressor;
    std::int64_t last_request = 0;
    // Joiner: the stream received so far, by chunk, and what is made of it.
    std::vector<std::uint8_t> stream_in;
    std::vector<std::uint16_t> chunk_length;
    std::uint32_t first_missing = 0u;
    std::uint32_t window = 32u;
    std::uint32_t asked_first = 0u, asked_window = 0u;
    std::int64_t last_ask = 0;
    std::size_t parsed = 0u;  // stream bytes taken into the image or the history
    std::uint32_t blob_size = kNone;
    std::vector<std::uint8_t> image;
    bool image_ready = false;
    std::vector<NetInput> history;  // the host's inputs from F on (history[t - F])
    std::int64_t last_ready = 0;
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
    // GALAXY_NET_BUILD_ID names the build by its content (Nebula Plus: source commit, module key, mods), so two installs
    // compiled on different PCs from the same source match; without it, the files' size and write time.
    wchar_t named[256]{};
    if (const DWORD length = GetEnvironmentVariableW(L"GALAXY_NET_BUILD_ID", named, 256); length != 0u && length < 256u) {
        for (DWORD i = 0; i < length; ++i) h = (h ^ static_cast<std::uint64_t>(named[i])) * 1099511628211ull;
        return h;
    }
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

// GALAXY_NET_TEST_LATENCY_MS=<ms>: every datagram this PC sends leaves that much later (tests of rollback on one
// PC, where the network is instant). Held ones go out as later sends and receives pass by.
struct HeldDatagram {
    std::int64_t due = 0;
    std::vector<char> bytes;
};

std::int64_t qpc_now() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::deque<HeldDatagram>& held_datagrams() {
    static std::deque<HeldDatagram> held;
    return held;
}

std::int64_t test_latency_ticks(const Session& s) {
    static const std::int64_t ticks = [&] {
        const int ms = std::atoi(env("GALAXY_NET_TEST_LATENCY_MS").c_str());
        return ms > 0 ? s.frequency.QuadPart * ms / 1000 : 0;
    }();
    return ticks;
}

void flush_held(Session& s) {
    auto& held = held_datagrams();
    const std::int64_t now = qpc_now();
    while (!held.empty() && held.front().due <= now) {
        (void)sendto(s.socket, held.front().bytes.data(), static_cast<int>(held.front().bytes.size()), 0,
                     reinterpret_cast<const sockaddr*>(&s.peer), sizeof(s.peer));
        held.pop_front();
    }
}

void send_raw(Session& s, const void* data, int size) {
    if (s.socket == INVALID_SOCKET || !s.peer_known) return;
    if (const std::int64_t latency = test_latency_ticks(s); latency > 0) {
        const auto* bytes = static_cast<const char*>(data);
        held_datagrams().push_back({qpc_now() + latency, std::vector<char>(bytes, bytes + size)});
        flush_held(s);
        return;
    }
    (void)sendto(s.socket, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&s.peer),
                 sizeof(s.peer));
}

void send_hello(Session& s, Packet type) {
    // The delay word carries rollback's window too: both PCs must play with the same (or neither with it).
    const std::uint32_t timing = s.delay | (s.rollback ? s.max_ahead << 8u : 0u);
    const WireHello packet{{kMagic, static_cast<std::uint8_t>(type), kVersion, 0u}, timing, s.build_id};
    send_raw(s, &packet, sizeof(packet));
}

Slot& slot(Session& s, unsigned player, std::uint32_t frame) {
    return s.inputs[player][frame % kRing];
}

unsigned me(const Session& s) { return s.host ? 0u : 1u; }

void send_inputs(Session& s) {
    if (!s.connected) return;
    // Live: inputs before the join frame travel in the stream (with their final peek answers).
    if (s.live && s.join_frame == kNone) return;
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
        wire.command = from.input.command;
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

// The game's save directory (GALAXY_NAND_ROOT or %LOCALAPPDATA%\SuperMarioGalaxy\nand) and settings file
// (GALAXY_RUNTIME_SETTINGS_PATH or %LOCALAPPDATA%\Nebula\runtime_settings.ini), as the runtime resolves them.
std::filesystem::path env_path(const wchar_t* name) {
    wchar_t value[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    return length != 0u && length < MAX_PATH ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path nand_root() {
    if (auto root = env_path(L"GALAXY_NAND_ROOT"); !root.empty()) return root;
    return env_path(L"LOCALAPPDATA") / L"SuperMarioGalaxy" / L"nand";
}

std::filesystem::path settings_file() {
    if (auto path = env_path(L"GALAXY_RUNTIME_SETTINGS_PATH"); !path.empty()) return path;
    return env_path(L"LOCALAPPDATA") / L"Nebula" / L"runtime_settings.ini";
}

std::FILE* open_file(const std::filesystem::path& path, const wchar_t* mode) {
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::filesystem::path from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return std::filesystem::path(wide);
}

bool is_host_setting(const std::string& line) {
    for (const char* key : kHostSettingKeys) {
        if (line.rfind(key, 0) == 0) return true;
    }
    return false;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    if (std::FILE* file = open_file(path, L"rb")) {
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
            std::string line(buffer);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            lines.push_back(line);
        }
        std::fclose(file);
    }
    return lines;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& in, std::size_t& at) {
    if (at + 4u > in.size()) throw std::runtime_error("truncated session bundle");
    std::uint32_t value = 0u;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in[at + i]) << (8 * i);
    at += 4u;
    return value;
}

// Whole contents of a file or of a named stream (a directory's too), through CreateFileW.
bool read_all(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    out.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::uint8_t buffer[65536];
    DWORD n = 0u;
    while (ReadFile(file, buffer, sizeof(buffer), &n, nullptr) && n != 0u) out.insert(out.end(), buffer, buffer + n);
    CloseHandle(file);
    return true;
}

bool write_all(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    const bool ok = size == 0u || (WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size);
    CloseHandle(file);
    return ok;
}

// Bundle: u32 entry count, per entry (u32 kind: 0 directory, 1 file; u32 path length, path in UTF-8 relative to the
// save directory, "" for the directory itself; a file: u32 size and its bytes; u32 metadata size and the entry's
// NAND metadata stream), then u32 settings length and the host's game settings lines. Directories come before
// what they hold. The emulated NAND keeps each entry's owner, permissions and a check in that stream; without it
// the joiner's NAND layer would rebuild the save as damaged.
std::vector<std::uint8_t> build_bundle() {
    std::vector<std::uint8_t> out;
    const std::filesystem::path root = nand_root();
    std::vector<std::pair<bool, std::filesystem::path>> entries{{false, root}};
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_directory(error)) {
            entries.emplace_back(false, it->path());
        } else if (it->is_regular_file(error)) {
            entries.emplace_back(true, it->path());
        }
    }
    put_u32(out, static_cast<std::uint32_t>(entries.size()));
    std::size_t files = 0u;
    std::vector<std::uint8_t> bytes;
    for (const auto& [is_file, path] : entries) {
        std::string relative = path == root ? std::string{} : to_utf8(std::filesystem::relative(path, root, error));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        put_u32(out, is_file ? 1u : 0u);
        put_u32(out, static_cast<std::uint32_t>(relative.size()));
        out.insert(out.end(), relative.begin(), relative.end());
        if (is_file) {
            ++files;
            (void)read_all(path, bytes);
            put_u32(out, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        std::filesystem::path stream = path;
        stream += kNandMetaStream;
        if (!read_all(stream, bytes)) bytes.clear();
        put_u32(out, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    std::string settings;
    for (const std::string& line : read_lines(settings_file())) {
        if (is_host_setting(line)) settings += line + "\n";
    }
    put_u32(out, static_cast<std::uint32_t>(settings.size()));
    out.insert(out.end(), settings.begin(), settings.end());
    std::fprintf(stderr, "[net] session bundle: %zu save files from %s, %zu bytes\n", files,
                 to_utf8(root).c_str(), out.size());
    return out;
}

void send_bundle_chunks(Session& s, std::uint32_t first) {
    if (!s.bundle_built) {
        if (s.live) return;  // built at the snapshot (net_live_snapshot_taken): the save as of that frame
        s.bundle = build_bundle();
        s.bundle_built = true;
    }
    const auto total = static_cast<std::uint32_t>(s.bundle.size());
    for (std::uint32_t index = first; index < first + kChunkBurst && index * kChunkBytes < total + (total == 0u ? 1u : 0u);
         ++index) {
        WireBundleChunk chunk{};
        chunk.header = {kMagic, static_cast<std::uint8_t>(Packet::BundleChunk), kVersion, 0u};
        chunk.index = index;
        chunk.total_size = total;
        const std::uint32_t at = index * kChunkBytes;
        chunk.length = static_cast<std::uint16_t>(std::min(kChunkBytes, total - std::min(at, total)));
        if (chunk.length != 0u) std::memcpy(chunk.data, s.bundle.data() + at, chunk.length);
        send_raw(s, &chunk, static_cast<int>(offsetof(WireBundleChunk, data) + chunk.length));
    }
}

// ---- Live join's stream (docs/HOT_JOIN.md) ----

std::int64_t now_qpc();

WireInput to_wire(const NetInput& input) {
    WireInput wire{};
    wire.count = input.count;
    wire.peek_count = input.peek_count;
    std::memcpy(wire.peeks, input.peeks.data(), sizeof(wire.peeks));
    std::memcpy(wire.status, input.status.data(), kNetStatusSize);
    wire.command = input.command;
    return wire;
}

NetInput from_wire(const WireInput& wire) {
    NetInput input;
    input.count = wire.count;
    input.peek_count = std::min(wire.peek_count, kNetMaxPeeks);
    std::memcpy(input.peeks.data(), wire.peeks, sizeof(wire.peeks));
    std::memcpy(input.status.data(), wire.status, kNetStatusSize);
    input.command = wire.command;
    return input;
}

// Host: a frame's input, final, goes on the stream.
void append_record(Session& s, std::uint32_t frame, const NetInput& input) {
    WireRecord record{};
    record.frame = frame;
    record.join_frame = s.join_frame;
    record.input = to_wire(input);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&record);
    std::lock_guard lock(s.stream_mutex);
    s.records.insert(s.records.end(), bytes, bytes + sizeof(record));
}

// Host: a joiner's request. It names the first chunk it lacks and how many it can take from there: the chunk it
// lacks goes again (it may have been the stream's end, since grown), then those not sent yet up to its window.
// A request that has not moved for three in a row means chunks were lost: everything from there goes again.
void send_stream_chunks(Session& s, std::uint32_t first, std::uint32_t window) {
    static std::uint32_t s_sent_to = 0u, s_last_first = kNone, s_same = 0u;
    s_same = first == s_last_first ? s_same + 1u : 0u;
    s_last_first = first;
    if (s_same >= 3u) {
        s_sent_to = first;
        s_same = 0u;
    }
    std::lock_guard lock(s.stream_mutex);
    if (!s.stream_ready) return;
    const std::size_t head = s.stream_head.size();
    const std::size_t total = head + s.records.size();
    const std::uint32_t limit = first + std::min(window, kStreamWindowMax);
    WireStreamChunk chunk{};
    chunk.header = {kMagic, static_cast<std::uint8_t>(Packet::StreamChunk), kVersion, 0u};
    const auto send = [&](std::uint32_t index) {
        const std::size_t at = static_cast<std::size_t>(index) * kStreamChunk;
        if (at >= total) return false;
        const std::size_t length = std::min<std::size_t>(kStreamChunk, total - at);
        const std::size_t from_head = at < head ? std::min(length, head - at) : 0u;
        if (from_head != 0u) std::memcpy(chunk.data, s.stream_head.data() + at, from_head);
        if (length > from_head) {
            std::memcpy(chunk.data + from_head, s.records.data() + (at + from_head - head), length - from_head);
        }
        chunk.index = index;
        chunk.length = static_cast<std::uint16_t>(length);
        send_raw(s, &chunk, static_cast<int>(offsetof(WireStreamChunk, data) + length));
        return true;
    };
    (void)send(first);
    for (std::uint32_t index = std::max(first + 1u, s_sent_to); index < limit; ++index) {
        if (!send(index)) break;
        s_sent_to = index + 1u;
    }
}

std::size_t contiguous(const Session& s) {
    std::size_t bytes = static_cast<std::size_t>(s.first_missing) * kStreamChunk;
    if (s.first_missing < s.chunk_length.size()) bytes += s.chunk_length[s.first_missing];
    return bytes;
}

// Joiner: what has come contiguously becomes the snapshot's image, then the host's inputs frame by frame.
void parse_stream(Session& s) {
    const std::size_t have = contiguous(s);
    if (s.blob_size == kNone) {
        if (have < 12u) return;
        std::uint32_t head[3]{};
        std::memcpy(head, s.stream_in.data(), sizeof(head));
        if (head[0] != kStreamMagic) {
            std::fprintf(stderr, "[net] the host's stream is not a snapshot\n");
            s.peer_left = true;
            return;
        }
        s.snapshot_frame = head[1];
        s.blob_size = head[2];
        s.parsed = 12u;
    }
    if (s.parsed == 12u) {
        if (have < 12u + s.blob_size) return;
        const std::int64_t started = now_qpc();
        if (!hot_join::decompress(s.stream_in.data() + 12u, s.blob_size, s.image)) {
            std::fprintf(stderr, "[net] the host's snapshot does not decompress\n");
            s.peer_left = true;
            return;
        }
        s.image_ready = true;
        s.parsed = 12u + s.blob_size;
        std::fprintf(stderr, "[net] the snapshot of frame %u is here: %u bytes, %zu unpacked in %.0f ms\n",
                     s.snapshot_frame, s.blob_size, s.image.size(),
                     1000.0 * static_cast<double>(now_qpc() - started) / static_cast<double>(s.frequency.QuadPart));
    }
    while (have >= s.parsed + sizeof(WireRecord)) {
        WireRecord record{};
        std::memcpy(&record, s.stream_in.data() + s.parsed, sizeof(record));
        s.parsed += sizeof(record);
        if (record.frame != s.snapshot_frame + s.history.size()) {
            std::fprintf(stderr, "[net] the host's inputs skip from frame %zu to %u\n",
                         s.snapshot_frame + s.history.size(), record.frame);
            s.peer_left = true;
            return;
        }
        s.history.push_back(from_wire(record.input));
        if (record.join_frame != kNone && s.join_frame == kNone) {
            s.join_frame = record.join_frame;
            std::fprintf(stderr, "[net] the host takes us in at frame %u\n", s.join_frame);
        }
    }
}

void take_stream_chunk(Session& s, const std::uint8_t* data, int size) {
    if (size < static_cast<int>(offsetof(WireStreamChunk, data))) return;
    WireStreamChunk chunk{};
    std::memcpy(&chunk, data, std::min<std::size_t>(sizeof(chunk), static_cast<std::size_t>(size)));
    if (chunk.length > kStreamChunk || size < static_cast<int>(offsetof(WireStreamChunk, data) + chunk.length)) return;
    if (chunk.index >= s.chunk_length.size()) s.chunk_length.resize(chunk.index + 1u, 0u);
    if (chunk.length <= s.chunk_length[chunk.index]) return;
    const std::size_t at = static_cast<std::size_t>(chunk.index) * kStreamChunk;
    if (s.stream_in.size() < at + chunk.length) s.stream_in.resize(at + chunk.length);
    std::memcpy(s.stream_in.data() + at, chunk.data, chunk.length);
    s.chunk_length[chunk.index] = chunk.length;
    while (s.first_missing < s.chunk_length.size() && s.chunk_length[s.first_missing] == kStreamChunk) ++s.first_missing;
}

// Joiner: asks for the stream from the first chunk it lacks, every 10 ms. The window doubles while it is used up
// between asks and halves when nothing moved.
void ask_stream(Session& s) {
    if (!s.live || s.host || !s.connected) return;
    const std::int64_t now = now_qpc();
    if (now - s.last_ask < s.frequency.QuadPart / 100) return;
    if (s.asked_window != 0u) {
        if (s.first_missing >= s.asked_first + s.asked_window / 2u) {
            s.window = std::min(s.window * 2u, kStreamWindowMax);
        } else if (s.first_missing == s.asked_first) {
            s.window = std::max(s.window / 2u, kStreamWindowMin);
        }
    }
    s.asked_first = s.first_missing;
    s.asked_window = s.window;
    s.last_ask = now;
    const WireStreamRequest request{{kMagic, static_cast<std::uint8_t>(Packet::StreamRequest), kVersion, 0u},
                                    s.first_missing, s.window};
    send_raw(s, &request, sizeof(request));
}

void set_buffers(SOCKET socket) {
    const int bytes = 8 * 1024 * 1024;
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
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
        if (build != s.build_id || delay != (s.delay | (s.rollback ? s.max_ahead << 8u : 0u))) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                std::fprintf(stderr, "[net] refused a peer with a different %s\n",
                             build != s.build_id ? "build" : "input delay or rollback setting");
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
    if (type == Packet::Bye) {
        if (!s.peer_left) std::fprintf(stderr, "[net] the other player left the session\n");
        s.peer_left = true;
        return;
    }
    if (type == Packet::BundleRequest && s.host) {
        if (size < static_cast<int>(sizeof(WireBundleRequest))) return;
        WireBundleRequest request{};
        std::memcpy(&request, data, sizeof(request));
        send_bundle_chunks(s, request.first_chunk);
        return;
    }
    if (type == Packet::BundleChunk && !s.host) {
        if (size < static_cast<int>(offsetof(WireBundleChunk, data))) return;
        WireBundleChunk chunk{};
        std::memcpy(&chunk, data, std::min<std::size_t>(sizeof(chunk), static_cast<std::size_t>(size)));
        if (!s.bundle_size_known) {
            s.bundle_size = chunk.total_size;
            s.bundle_size_known = true;
            s.bundle.assign(s.bundle_size, 0u);
            s.bundle_have.assign((s.bundle_size + kChunkBytes - 1u) / kChunkBytes, false);
        }
        if (chunk.index < s.bundle_have.size() && !s.bundle_have[chunk.index]) {
            const std::uint32_t at = chunk.index * kChunkBytes;
            if (at + chunk.length <= s.bundle.size()) {
                std::memcpy(s.bundle.data() + at, chunk.data, chunk.length);
                s.bundle_have[chunk.index] = true;
            }
        }
        return;
    }
    if (type == Packet::StreamRequest && s.host && s.live) {
        if (size < static_cast<int>(sizeof(WireStreamRequest))) return;
        WireStreamRequest request{};
        std::memcpy(&request, data, sizeof(request));
        s.last_request = now_qpc();
        send_stream_chunks(s, request.first_chunk, request.window);
        return;
    }
    if (type == Packet::StreamChunk && !s.host && s.live) {
        take_stream_chunk(s, data, size);
        return;
    }
    if (type == Packet::Ready && s.host && s.live) {
        if (size < static_cast<int>(sizeof(WireReady))) return;
        if (s.join_frame == kNone && s.phase == Session::Live::Streaming) {
            WireReady ready{};
            std::memcpy(&ready, data, sizeof(ready));
            s.join_frame = s.current_frame + kJoinMargin;
            std::fprintf(stderr, "[net] the joiner has caught up (its frame %u, ours %u): it drops in at frame %u\n",
                         ready.frame, s.current_frame, s.join_frame);
        }
        return;
    }
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
            to.input.command = wire.command;
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
    if (test_latency_ticks(s) > 0 && s.peer_known) flush_held(s);
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

void join_session(Session& s, const std::string& host_label);
std::int64_t now_qpc();

std::uint32_t be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) | (static_cast<std::uint32_t>(bytes[1]) << 16u) |
           (static_cast<std::uint32_t>(bytes[2]) << 8u) | bytes[3];
}

float bef32(const std::uint8_t* bytes) noexcept {
    const std::uint32_t bits = be32(bytes);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The second player's controller as the co-op code reads it directly (their split-screen camera), from the
// joiner's input of the frame just applied: the same on both PCs, never a controller of this PC's.
bool synchronised_partner_input(coop::PartnerInput& partner) {
    const std::uint32_t frame = netplay_frame();
    const NetInput& input = net_input(frame == 0u ? 0u : frame - 1u, 1u);
    const std::uint8_t* status = input.status.data();
    partner = coop::PartnerInput{};
    partner.connected = true;
    partner.buttons = be32(status + coop::kpad::kHold);
    partner.stick_x = bef32(status + coop::kpad::kFsStick);
    partner.stick_y = bef32(status + coop::kpad::kFsStick + 4u);
    partner.pointer_valid = status[coop::kpad::kDpdValid] != 0u;
    partner.pointer_x = bef32(status + coop::kpad::kPos);
    partner.pointer_y = bef32(status + coop::kpad::kPos + 4u);
    return true;
}

void initialize(Session& s) {
    s.initialized = true;
    // Restarted from the lobby (net_lobby.cpp): the old process must be gone (its lease and port) first.
    if (const std::string pid = env("GALAXY_NET_WAIT_PID"); !pid.empty()) {
        if (const HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::strtoul(pid.c_str(), nullptr, 10)))) {
            (void)WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
        SetEnvironmentVariableW(L"GALAXY_NET_WAIT_PID", nullptr);
    }
    const std::string host_port = env("GALAXY_NET_HOST");
    const std::string join = env("GALAXY_NET_JOIN");
    // Live join (docs/HOT_JOIN.md): a game booted Joinable is a session host on its own until someone joins (its
    // lobby listens meanwhile); GALAXY_NET_HOTJOIN=1 makes a joiner take the host's running game.
    // The settings are not loaded yet this early (a joiner changes their path first): the file says.
    const auto joinable_setting = [] {
        for (const std::string& line : read_lines(settings_file())) {
            if (line == "net_joinable=on") return true;
        }
        return false;
    };
    const bool joinable = host_port.empty() && join.empty() &&
                          (env("GALAXY_NET_JOINABLE") == "1" || (env("GALAXY_NET_JOINABLE") != "0" && joinable_setting()));
    if (host_port.empty() && join.empty() && !joinable) return;
    s.requested = true;
    s.host = !host_port.empty() || joinable;
    s.live = joinable || (!join.empty() && env("GALAXY_NET_HOTJOIN") == "1");
    s.phase = s.host ? Session::Live::Solo : Session::Live::Booting;
    coop_set_partner_input_source(&synchronised_partner_input);
    // The other PC waits on this one's every frame: no power throttling (EcoQoS slowing a background process,
    // or its timer resolution being ignored) even when this window is behind another or minimised.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0u;
    (void)SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    // Rollback is on unless GALAXY_NET_ROLLBACK=0; with it the input delay is 2 frames, without it 3.
    s.rollback = env("GALAXY_NET_ROLLBACK") != "0";
    s.delay = s.rollback ? 2u : 3u;
    if (const std::string delay = env("GALAXY_NET_DELAY"); !delay.empty()) {
        s.delay = static_cast<std::uint32_t>(std::clamp(std::atoi(delay.c_str()), 1, 30));
    }
    s.build_id = build_identity();
    for (auto& ring : s.inputs) ring.assign(kRing, Slot{});
    if (const std::string ahead = env("GALAXY_NET_ROLLBACK_FRAMES"); !ahead.empty()) {
        s.max_ahead = static_cast<std::uint32_t>(std::clamp(std::atoi(ahead.c_str()), 1, 12));
    }
    s.predicted.assign(kRing, NetInput{});
    s.was_predicted.assign(kRing, 0u);
    if (s.rollback) std::fprintf(stderr, "[net] rollback on: up to %u frames predicted\n", s.max_ahead);
    s.local_peeks.assign(kRing, {});
    s.local_peek_counts.assign(kRing, 0u);
    QueryPerformanceFrequency(&s.frequency);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[net] Winsock did not start\n");
        return;
    }
    if (joinable) {
        // The socket opens when a joiner is accepted (net_live_accept), on the lobby's port.
        std::fprintf(stderr, "[net] joinable: deterministic from boot, input delay %u frames once joined\n", s.delay);
        return;
    }
    s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.socket == INVALID_SOCKET) {
        std::fprintf(stderr, "[net] cannot open a UDP socket\n");
        return;
    }
    if (s.live) set_buffers(s.socket);
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
        // The save as this PC boots it, before the game writes to it (it does at once: the play record): the
        // joiner, however late it connects, starts from exactly these files.
        s.bundle = build_bundle();
        s.bundle_built = true;
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
        join_session(s, join);
    }
}

// Joiner, at startup (before the game reads its save or settings): fetch the host's bundle and play from it.
// The save goes to %LOCALAPPDATA%\Nebula\netplay\<host>\saves, or GALAXY_NETPLAY_DIR\<host>\saves (the rejoin
// cache; never the joiner's own saves),
// the settings to ...\runtime_settings.ini: the joiner's own file with the host's game settings in place of its.
void join_session(Session& s, const std::string& host_label) {
    const std::int64_t started = now_qpc();
    std::int64_t last_send = 0;
    std::int64_t last_log = started;
    for (;;) {
        receive(s);
        if (s.bundle_size_known &&
            std::all_of(s.bundle_have.begin(), s.bundle_have.end(), [](bool have) { return have; })) {
            break;
        }
        const std::int64_t now = now_qpc();
        if (now - last_send > s.frequency.QuadPart / 20) {
            if (!s.connected) {
                send_hello(s, Packet::Hello);
            } else {
                std::uint32_t first = 0u;
                while (first < s.bundle_have.size() && s.bundle_have[first]) ++first;
                const WireBundleRequest request{{kMagic, static_cast<std::uint8_t>(Packet::BundleRequest), kVersion, 0u},
                                                first};
                send_raw(s, &request, sizeof(request));
            }
            last_send = now;
        }
        if (now - last_log > s.frequency.QuadPart * 5) {
            std::fprintf(stderr, "[net] waiting for the host's %s\n", s.connected ? "save" : "answer");
            last_log = now;
        }
        Sleep(1);
    }
    std::string label = host_label;
    for (char& c : label) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') c = '_';
    }
    // GALAXY_NETPLAY_DIR keeps the cache with a separate install's own data (Nebula Plus: <root>\data\netplay).
    std::filesystem::path netplay_dir = env_path(L"GALAXY_NETPLAY_DIR");
    if (netplay_dir.empty()) netplay_dir = env_path(L"LOCALAPPDATA") / L"Nebula" / L"netplay";
    const std::filesystem::path session_dir = netplay_dir / label;
    const std::filesystem::path saves = session_dir / L"saves";
    std::error_code error;
    std::filesystem::remove_all(saves, error);
    std::filesystem::create_directories(saves, error);
    std::size_t at = 0u;
    const std::uint32_t entries = get_u32(s.bundle, at);
    std::uint32_t files = 0u;
    for (std::uint32_t i = 0; i < entries; ++i) {
        const std::uint32_t kind = get_u32(s.bundle, at);
        const std::uint32_t path_length = get_u32(s.bundle, at);
        if (at + path_length > s.bundle.size()) throw std::runtime_error("truncated session bundle");
        const std::string relative(reinterpret_cast<const char*>(s.bundle.data() + at), path_length);
        at += path_length;
        const std::filesystem::path target = relative.empty() ? saves : saves / from_utf8(relative);
        // A path from the peer stays inside the session's save directory.
        const auto normal = target.lexically_normal().native();
        if (normal.rfind(saves.lexically_normal().native(), 0) != 0 || relative.find("..") != std::string::npos ||
            relative.find(':') != std::string::npos) {
            throw std::runtime_error("session bundle names a file outside its save directory");
        }
        if (kind == 1u) {
            const std::uint32_t size = get_u32(s.bundle, at);
            if (at + size > s.bundle.size()) throw std::runtime_error("truncated session bundle");
            std::filesystem::create_directories(target.parent_path(), error);
            (void)write_all(target, s.bundle.data() + at, size);
            at += size;
            ++files;
        } else {
            std::filesystem::create_directories(target, error);
        }
        const std::uint32_t meta_size = get_u32(s.bundle, at);
        if (at + meta_size > s.bundle.size()) throw std::runtime_error("truncated session bundle");
        if (meta_size != 0u) {
            std::filesystem::path stream = target;
            stream += kNandMetaStream;
            (void)write_all(stream, s.bundle.data() + at, meta_size);
        }
        at += meta_size;
    }
    const std::uint32_t settings_length = get_u32(s.bundle, at);
    if (at + settings_length > s.bundle.size()) throw std::runtime_error("truncated session bundle");
    const std::string host_settings(reinterpret_cast<const char*>(s.bundle.data() + at), settings_length);
    std::string merged;
    for (const std::string& line : read_lines(settings_file())) {
        if (!is_host_setting(line)) merged += line + "\n";
    }
    merged += host_settings;
    const std::filesystem::path session_settings = session_dir / L"runtime_settings.ini";
    if (std::FILE* out = open_file(session_settings, L"wb")) {
        std::fwrite(merged.data(), 1, merged.size(), out);
        std::fclose(out);
    }
    // This PC's own save directory and settings, for when the session ends (net_restart_alone): kept once, so a
    // second session keeps the first's originals.
    if (env_path(L"GALAXY_NET_HOME_SET").empty()) {
        SetEnvironmentVariableW(L"GALAXY_NET_HOME_SET", L"1");
        SetEnvironmentVariableW(L"GALAXY_NET_HOME_NAND_ROOT", env_path(L"GALAXY_NAND_ROOT").c_str());
        SetEnvironmentVariableW(L"GALAXY_NET_HOME_SETTINGS", env_path(L"GALAXY_RUNTIME_SETTINGS_PATH").c_str());
    }
    // Both environments: the save directory is read through the Win32 one, the settings path through the CRT's.
    SetEnvironmentVariableW(L"GALAXY_NAND_ROOT", saves.c_str());
    _wputenv_s(L"GALAXY_NAND_ROOT", saves.c_str());
    SetEnvironmentVariableW(L"GALAXY_RUNTIME_SETTINGS_PATH", session_settings.c_str());
    _wputenv_s(L"GALAXY_RUNTIME_SETTINGS_PATH", session_settings.c_str());
    std::fprintf(stderr, "[net] playing the host's save: %u files, %zu bytes, in %s\n", files, s.bundle.size(),
                 to_utf8(session_dir).c_str());
    s.bundle.clear();
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
    if (!s.initialized) {
        initialize(s);
        // The lobby (F1 > Online) runs in every game, in a session only to show it.
        net_lobby_start();
    }
    return s.requested;
}

bool net_is_host() noexcept {
    return net_session_requested() && session().host;
}

void net_request_command(NetCommand command) noexcept {
    session().pending_command = static_cast<std::uint32_t>(command);
}

bool net_peer_left() noexcept {
    return session().peer_left;
}

void net_leave() {
    Session& s = session();
    const Header bye{kMagic, static_cast<std::uint8_t>(Packet::Bye), kVersion, 0u};
    for (int i = 0; i < 5; ++i) {
        send_raw(s, &bye, sizeof(bye));
        Sleep(10);
    }
    if (s.live && s.host) {
        // The host plays on alone (live_host_frame takes it back to Solo at the next frame).
        std::fprintf(stderr, "[net] ending the session: playing on alone\n");
        s.peer_left = true;
        return;
    }
    std::fprintf(stderr, "[net] leaving the session\n");
    net_restart_alone();
}

namespace {

// Live host, after its read of the frame before the join frame: that frame's input goes on the stream, and the
// inputs of J .. J + delay - 1, which no read reaches (the host's reads go `delay` frames ahead from J on), are
// J - 1's held, J's carrying Join. Here, not at J's start: the game's EFB peeks of a frame come before it starts
// (in the previous frame's drawing, the frame counter already past both reads), and they read J's input.
void begin_lockstep(Session& s, std::uint32_t previous) {
    if (previous >= s.snapshot_frame) append_record(s, previous, slot(s, 0u, previous).input);
    const NetInput held = slot(s, 0u, previous).input;
    const std::uint32_t join = previous + 1u;
    for (std::uint32_t t = join; t < join + s.delay; ++t) {
        Slot& to = slot(s, 0u, t);
        to.frame = t;
        to.input = held;
        to.input.command = t == join ? static_cast<std::uint32_t>(NetCommand::Join) : 0u;
        // The answers a frame from J on uses: the host's of `delay + 1` frames earlier, as in lockstep.
        const std::uint32_t source = t - s.delay - 1u;
        to.input.peek_count = std::min(s.local_peek_counts[source % kRing], kNetMaxPeeks);
        to.input.peeks = s.local_peeks[source % kRing];
        append_record(s, t, to.input);
    }
    s.newest_local_frame = join + s.delay - 1u;
}

// Live host, at a frame's start: an accept from the lobby opens the session; the previous frame's input (final
// now: its EFB peek answers are in) goes on the stream until begin_lockstep takes over.
// A live host on its own again: no peer, no stream, the next joiner welcome (live_host_frame).
void back_to_solo(Session& s) {
    if (s.socket != INVALID_SOCKET) closesocket(s.socket);
    s.socket = INVALID_SOCKET;
    s.connected = false;
    s.peer_known = false;
    s.peer_left = false;
    s.phase = Session::Live::Solo;
    s.snapshot_frame = kNone;
    s.join_frame = kNone;
    s.any_remote = false;
    s.unconfirmed_from = kNone;
    if (s.compressor.joinable()) s.compressor.join();
    std::lock_guard lock(s.stream_mutex);
    s.stream_head.clear();
    s.records.clear();
    s.stream_ready = false;
    s.bundle.clear();
    s.bundle_built = false;
}
void live_host_frame(Session& s, std::uint32_t frame) {
    // The joiner left (its Leave, its game closed, its network gone): play on alone, the second player sitting out,
    // and the lobby listens for the next one (no restart).
    if (s.peer_left && s.phase == Session::Live::Joined) {
        std::fprintf(stderr, "[net] frame %u: the other player left: playing on alone\n", frame);
        back_to_solo(s);
        coop_request_drop_out();
    }
    if (s.accept_pending.exchange(false)) {
        s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long nonblocking = 1u;
        ioctlsocket(s.socket, FIONBIO, &nonblocking);
        set_buffers(s.socket);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(static_cast<u_short>(s.live_port));
        if (s.socket == INVALID_SOCKET || bind(s.socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            std::fprintf(stderr, "[net] cannot open the session on UDP port %u\n", s.live_port);
            if (s.socket != INVALID_SOCKET) closesocket(s.socket);
            s.socket = INVALID_SOCKET;
            s.phase = Session::Live::Solo;
            return;
        }
        s.phase = Session::Live::Starting;
        s.snapshot_wanted = true;
        s.last_request = now_qpc();
        std::fprintf(stderr, "[net] a joiner was accepted: the session is on UDP port %u\n", s.live_port);
    }
    if (s.snapshot_frame == kNone || frame == 0u) return;
    const std::uint32_t previous = frame - 1u;
    if (previous >= s.snapshot_frame && (s.join_frame == kNone || previous + 1u < s.join_frame)) {
        append_record(s, previous, slot(s, 0u, previous).input);
    }
    if (frame == s.join_frame) {
        s.phase = Session::Live::Joined;
        std::fprintf(stderr, "[net] frame %u: the joiner drops in\n", frame);
    }
    // Tests (tools/agent/live_join.py): GALAXY_NET_TEST_EXIT_AFTER_JOIN=<frames> sends the online menu's Exit
    // Galaxy that many frames after the join.
    static const auto s_exit_after =
        static_cast<std::uint32_t>(std::strtoul(env("GALAXY_NET_TEST_EXIT_AFTER_JOIN").c_str(), nullptr, 10));
    if (s_exit_after != 0u && s.join_frame != kNone && frame == s.join_frame + s_exit_after) {
        std::fprintf(stderr, "[net] test: exit galaxy at frame %u\n", frame);
        s.pending_command = static_cast<std::uint32_t>(NetCommand::ExitGalaxy);
    }
    // GALAXY_NET_TEST_STAR_AFTER_JOIN=<frames>: a Power Star appears (its cutscene on both PCs) that many frames on.
    static const auto s_star_after =
        static_cast<std::uint32_t>(std::strtoul(env("GALAXY_NET_TEST_STAR_AFTER_JOIN").c_str(), nullptr, 10));
    if (s_star_after != 0u && s.join_frame != kNone && frame == s.join_frame + s_star_after) {
        std::fprintf(stderr, "[net] test: a Power Star appears at frame %u\n", frame);
        s.pending_command = static_cast<std::uint32_t>(NetCommand::TestStarAppear);
    }
    // A joiner that stopped asking before it joined is gone: back to playing alone (the lobby listens again).
    if (s.phase == Session::Live::Streaming && now_qpc() - s.last_request > s.frequency.QuadPart * 15) {
        std::fprintf(stderr, "[net] the joiner went away before joining: playing on alone\n");
        back_to_solo(s);
    }
}

// Live joiner, at a frame's start: the stream keeps coming; caught up with the host (its newest input a frame or
// two ahead), rendering and pacing come back and the host is told, until it names the join frame.
void live_joiner_frame(Session& s, std::uint32_t frame) {
    ask_stream(s);
    parse_stream(s);
    const NetplaySnapshotHooks& hooks = netplay_snapshot_hooks();
    const std::uint32_t newest = s.snapshot_frame + static_cast<std::uint32_t>(s.history.size());
    if (s.phase == Session::Live::CatchUp && frame + 2u >= newest) {
        s.phase = Session::Live::Edge;
        if (hooks.catch_up != nullptr) hooks.catch_up(hooks.user, false);
        std::fprintf(stderr, "[net] frame %u: caught up with the host\n", frame);
    }
    if (s.phase == Session::Live::Edge && s.join_frame == kNone && now_qpc() - s.last_ready > s.frequency.QuadPart / 10) {
        const WireReady ready{{kMagic, static_cast<std::uint8_t>(Packet::Ready), kVersion, 0u}, frame};
        send_raw(s, &ready, sizeof(ready));
        s.last_ready = now_qpc();
    }
    if (s.join_frame != kNone && frame >= s.join_frame && s.phase != Session::Live::Joined) {
        s.phase = Session::Live::Joined;
        std::fprintf(stderr, "[net] frame %u: in the game\n", frame);
    }
}

// Joiner: waits until the stream has the host's input for `frame`.
const NetInput& history_input(Session& s, std::uint32_t frame) {
    const std::int64_t started = now_qpc();
    std::int64_t last_log = started;
    while (frame - s.snapshot_frame >= s.history.size()) {
        receive(s);
        ask_stream(s);
        parse_stream(s);
        if (frame - s.snapshot_frame < s.history.size()) break;
        const std::int64_t now = now_qpc();
        if (now - last_log > s.frequency.QuadPart * 5) {
            std::fprintf(stderr, "[net] frame %u: waiting for the host's input\n", frame);
            last_log = now;
        }
        if (now - started > s.frequency.QuadPart * 15 && !s.peer_left) {
            std::fprintf(stderr, "[net] nothing from the host for 15 s: the session is over\n");
            s.peer_left = true;
        }
        galaxy::gx::pump_messages();
        Sleep(1);
    }
    return s.history[frame - s.snapshot_frame];
}

}  // namespace

void net_frame_begin(std::uint32_t frame, const GuestMemoryV1* memory) {
    Session& s = session();
    if (!s.requested) return;
    s.current_frame = frame;
    if (s.live && s.host) live_host_frame(s, frame);
    receive(s);
    if (!s.connected && !s.host) send_hello(s, Packet::Hello);
    if (s.live && !s.host) live_joiner_frame(s, frame);
    s.local_peek_counts[frame % kRing] = 0u;
    // No pacing here: the guest's virtual time is kept to real time (native_runtime.cpp
    // deterministic_advance_to), as a game frame may span several VI frames.
    const bool booting = s.live && !s.host && s.phase == Session::Live::Booting;
    // A frame's hash only once, and only when every input before it is real (not a prediction a rollback may undo).
    const bool settled = s.unconfirmed_from == kNone && (s.last_hashed_frame == kNone || frame > s.last_hashed_frame);
    if (frame % kHashEvery == 0u && s.connected && !booting && settled && !s.resimulating) {
        s.last_hashed_frame = frame;
        const std::uint64_t hash = mem1_hash(memory);
        s.my_hashes.emplace_back(frame, hash);
        if (s.my_hashes.size() > 64u) s.my_hashes.erase(s.my_hashes.begin());
        const WireHash packet{{kMagic, static_cast<std::uint8_t>(Packet::Hash), kVersion, 0u}, frame, hash};
        send_raw(s, &packet, sizeof(packet));
    }
    if (frame % 600u == 0u && s.stalls != 0u && !s.resimulating) {
        std::fprintf(stderr, "[net] frame %u: %llu stalls, %.0f ms waiting for the peer\n", frame,
                     static_cast<unsigned long long>(s.stalls), s.stalled_ms);
    }
    if (frame % 600u == 0u && s.rollback && !s.resimulating) {
        std::fprintf(stderr, "[net] frame %u: %llu predictions, %llu rollbacks, %llu frames simulated again\n", frame,
                     static_cast<unsigned long long>(s.predictions), static_cast<unsigned long long>(s.rollbacks),
                     static_cast<unsigned long long>(s.rolled_frames));
    }
}

void net_local_input(std::uint32_t frame, std::uint32_t count, const std::uint8_t* status) {
    Session& s = session();
    if (!s.requested) return;
    const bool before_join = s.live && (s.join_frame == kNone || frame < s.join_frame);
    if (before_join && !s.host) return;  // the joiner plays from the join frame on
    if (before_join) {
        // The host on its own: its controller is this frame's input (a peek earlier in the frame made the slot).
        Slot& to = slot(s, 0u, frame);
        if (to.frame != frame) {
            to.frame = frame;
            to.input = NetInput{};
        }
        to.input.count = count;
        if (status != nullptr) std::memcpy(to.input.status.data(), status, kNetStatusSize);
        to.input.command = 0u;
        s.newest_local_frame = frame;
        if (s.join_frame != kNone && frame + 1u == s.join_frame) begin_lockstep(s, frame);
        return;
    }
    const std::uint32_t target = frame + s.delay;
    Slot& to = slot(s, me(s), target);
    to.frame = target;
    to.input = NetInput{};
    to.input.count = count;
    if (status != nullptr) std::memcpy(to.input.status.data(), status, kNetStatusSize);
    to.input.command = s.pending_command.exchange(0u);
    if (s.host && frame >= 1u) {
        // The host's peek answers of the frame just finished go with this input: both PCs use them at `target`.
        const std::uint32_t previous = frame - 1u;
        to.input.peek_count = std::min(s.local_peek_counts[previous % kRing], kNetMaxPeeks);
        to.input.peeks = s.local_peeks[previous % kRing];
    }
    s.newest_local_frame = target;
    send_inputs(s);
}

// The frames rollback may predict: a session's lockstep part (not a live session's solo phase, nor before the
// joiner's first input applies), and not while a rejoin plays its way back (its state is outside the snapshot).
bool rollback_frame(const Session& s, std::uint32_t frame) {
    if (netplay_rejoin_active()) return false;
    if (!s.live) return frame >= s.delay;
    return s.join_frame != kNone && frame >= s.join_frame + s.delay;
}

bool same_input(const NetInput& a, const NetInput& b) {
    // Peeks are not compared: a frame's peek answers are read from an earlier, real input (net_peek).
    return a.count == b.count && a.status == b.status && a.command == b.command;
}

std::uint32_t net_rollback_target(std::uint32_t frame) {
    Session& s = session();
    if (!s.rollback || s.unconfirmed_from == kNone) return kNone;
    receive(s);
    const unsigned peer = 1u - me(s);
    for (std::uint32_t f = s.unconfirmed_from; f < frame; ++f) {
        if (!s.was_predicted[f % kRing]) continue;
        const Slot& real = slot(s, peer, f);
        if (real.frame != f) {
            s.unconfirmed_from = f;  // not here yet: checked again next frame
            return kNone;
        }
        if (!same_input(real.input, s.predicted[f % kRing])) {
            for (std::uint32_t g = f; g < frame; ++g) s.was_predicted[g % kRing] = 0u;
            s.unconfirmed_from = kNone;
            ++s.rollbacks;
            s.rolled_frames += frame - f;
            return f;
        }
        s.was_predicted[f % kRing] = 0u;
    }
    s.unconfirmed_from = kNone;
    return kNone;
}

void net_set_resimulating(bool on) noexcept {
    session().resimulating = on;
}

bool net_rollback_enabled() noexcept {
    return session().rollback;
}

bool net_rollback_frame(std::uint32_t frame) noexcept {
    const Session& s = session();
    return s.rollback && s.connected && rollback_frame(s, frame);
}

const NetInput& net_input(std::uint32_t frame, unsigned player) {
    Session& s = session();
    static const NetInput kNeutral{};
    if (s.live) {
        // Live: the second player is neutral until he is in (J + delay, when the joiner's first input applies);
        // a joiner still booting its own title plays nobody's input.
        if (!s.host && s.phase == Session::Live::Booting) return kNeutral;
        const bool before_lockstep = s.join_frame == kNone || frame < s.join_frame + s.delay;
        if (player == 1u && before_lockstep) return kNeutral;
        if (player == 0u && s.host) {
            const Slot& own = slot(s, 0u, frame);
            if (own.frame == frame) return own.input;
        }
        if (player == 0u && !s.host && before_lockstep) return history_input(s, frame);
    } else if (frame < s.delay) {
        return kNeutral;  // before any input could arrive: the same on both PCs
    }
    Slot& from = slot(s, player, frame);
    if (from.frame == frame) return from.input;
    if (s.rollback && player != me(s) && s.connected && rollback_frame(s, frame) && !s.must_wait) {
        // Not here yet: run on a prediction, the peer's newest input held, within the window.
        const std::uint32_t newest = s.any_remote ? s.newest_remote_frame : 0u;
        if (frame <= newest + s.max_ahead) {
            NetInput& guess = s.predicted[frame % kRing];
            if (!s.was_predicted[frame % kRing]) {
                const Slot& last = slot(s, player, newest);
                guess = s.any_remote && last.frame == newest ? last.input : NetInput{};
                // Held, not pressed again: no presses or releases, no command.
                for (const std::size_t at : {coop::kpad::kTrig, coop::kpad::kRelease}) {
                    std::fill_n(guess.status.begin() + at, 4u, std::uint8_t{0});
                }
                guess.command = 0u;
                s.was_predicted[frame % kRing] = 1u;
                if (s.unconfirmed_from == kNone || frame < s.unconfirmed_from) s.unconfirmed_from = frame;
                ++s.predictions;
            }
            return guess;
        }
    }
    // The peer's input for this frame has not arrived: wait for it (lockstep), resending ours.
    const std::int64_t started = now_qpc();
    std::int64_t last_send = started;
    std::int64_t last_log = started;
    ++s.stalls;
    while (from.frame != frame) {
        receive(s);
        if (from.frame == frame) break;
        // A live host whose joiner left plays on alone: the second player's input is neutral until he sits out.
        if (s.live && s.host && s.peer_left) return kNeutral;
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
        // A peer silent for 15 s once connected is gone (its game closed or its network dropped): the lobby
        // restarts this game on its own (net_lobby.cpp).
        if (s.connected && now - started > s.frequency.QuadPart * 15 && !s.peer_left) {
            std::fprintf(stderr, "[net] nothing from the other player for 15 s: the session is over\n");
            s.peer_left = true;
        }
        galaxy::gx::pump_messages();
        Sleep(0);
    }
    s.stalled_ms += 1000.0 * static_cast<double>(now_qpc() - started) / static_cast<double>(s.frequency.QuadPart);
    return from.input;
}

std::uint32_t net_peek(std::uint32_t frame, std::uint32_t sequence, std::uint32_t local) noexcept {
    Session& s = session();
    if (s.host && sequence < kNetMaxPeeks && !s.resimulating) {
        s.local_peeks[frame % kRing][sequence] = local;
        s.local_peek_counts[frame % kRing] = sequence + 1u;
    }
    if (s.live && s.host && (s.join_frame == kNone || frame < s.join_frame)) {
        // The host on its own answers with its own value at once; the frame's input carries it to a joiner.
        Slot& to = slot(s, 0u, frame);
        if (to.frame != frame) {
            to.frame = frame;
            to.input = NetInput{};
        }
        if (sequence < kNetMaxPeeks) {
            to.input.peeks[sequence] = local;
            to.input.peek_count = std::max(to.input.peek_count, sequence + 1u);
        }
        return to.input.peeks[std::min(sequence, to.input.peek_count - 1u)];
    }
    // Both PCs answer with the host's value carried by the host's input for this frame. With rollback, the input of
    // max_ahead frames earlier, which has arrived whenever this frame may run (a peek is never predicted).
    const bool shifted = s.rollback && rollback_frame(s, frame);
    s.must_wait = shifted;
    const NetInput& host_input = net_input(shifted ? frame - s.max_ahead : frame, 0u);
    s.must_wait = false;
    if (host_input.peek_count == 0u) return 0x00FFFFFFu;
    return host_input.peeks[std::min(sequence, host_input.peek_count - 1u)];
}

bool net_live() noexcept {
    return net_session_requested() && session().live;
}

bool net_live_host() noexcept {
    const Session& s = session();
    return net_session_requested() && s.live && s.host;
}

bool net_live_can_accept() noexcept {
    Session& s = session();
    return net_session_requested() && s.live && s.host && s.phase == Session::Live::Solo && !s.accept_pending;
}

void net_live_accept(unsigned port) {
    Session& s = session();
    s.live_port = port;
    s.accept_pending = true;
}

bool net_live_snapshot_due() noexcept {
    Session& s = session();
    return s.live && s.host && s.snapshot_wanted;
}

void net_live_snapshot_taken(std::uint32_t frame, std::vector<std::uint8_t>&& image) {
    Session& s = session();
    s.snapshot_wanted = false;
    if (image.empty()) {
        std::fprintf(stderr, "[net] the snapshot could not be taken: playing on alone\n");
        closesocket(s.socket);
        s.socket = INVALID_SOCKET;
        s.phase = Session::Live::Solo;
        return;
    }
    s.snapshot_frame = frame;
    s.phase = Session::Live::Streaming;
    s.last_request = now_qpc();
    // The save as of this frame: the joiner's game reads and writes these files from here on.
    s.bundle = build_bundle();
    s.bundle_built = true;
    if (s.compressor.joinable()) s.compressor.join();
    s.compressor = std::thread([&s, frame, image = std::move(image)] {
        const std::int64_t started = now_qpc();
        const std::vector<std::uint8_t> blob = hot_join::compress(image.data(), image.size());
        std::vector<std::uint8_t> head(12u);
        const std::uint32_t fields[3] = {kStreamMagic, frame, static_cast<std::uint32_t>(blob.size())};
        std::memcpy(head.data(), fields, sizeof(fields));
        head.insert(head.end(), blob.begin(), blob.end());
        std::lock_guard lock(s.stream_mutex);
        s.stream_head = std::move(head);
        s.stream_ready = !blob.empty();
        std::fprintf(stderr, "[net] snapshot of frame %u: %zu bytes, %zu compressed in %.0f ms\n", frame, image.size(),
                     blob.size(),
                     1000.0 * static_cast<double>(now_qpc() - started) / static_cast<double>(s.frequency.QuadPart));
    });
}

const std::vector<std::uint8_t>* net_live_load_due(std::uint32_t frame) {
    Session& s = session();
    if (!s.live || s.host || s.phase != Session::Live::Booting || frame < kLoadFrame) return nullptr;
    const std::int64_t started = now_qpc();
    std::int64_t last_log = started;
    while (!s.image_ready && !s.peer_left) {
        receive(s);
        ask_stream(s);
        parse_stream(s);
        const std::int64_t now = now_qpc();
        if (now - last_log > s.frequency.QuadPart * 2) {
            std::fprintf(stderr, "[net] receiving the host's game: %zu bytes\n", contiguous(s));
            last_log = now;
        }
        galaxy::gx::pump_messages();
        Sleep(1);
    }
    if (!s.image_ready) return nullptr;
    std::fprintf(stderr, "[net] the host's game arrived in %.1f s\n",
                 static_cast<double>(now_qpc() - started) / static_cast<double>(s.frequency.QuadPart));
    return &s.image;
}

void net_live_resumed(std::uint32_t frame) {
    Session& s = session();
    s.phase = Session::Live::CatchUp;
    s.current_frame = frame;
    s.image = std::vector<std::uint8_t>{};
    s.image_ready = false;
    std::fprintf(stderr, "[net] running the host's game from its frame %u; catching up\n", frame);
    const NetplaySnapshotHooks& hooks = netplay_snapshot_hooks();
    if (hooks.catch_up != nullptr) hooks.catch_up(hooks.user, true);
}

}  // namespace galaxy
