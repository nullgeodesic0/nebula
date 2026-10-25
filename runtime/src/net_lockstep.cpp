#include "galaxy/net_lockstep.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
static void close_socket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
static void close_socket(Socket s) { ::close(s); }
#endif

namespace galaxy::net {

static_assert(std::is_trivially_copyable_v<input::WiimoteInputSnapshot>, "inputs go over the wire as bytes");

namespace {

// Messages: a type byte, a 32-bit length, the payload (little-endian, as both PCs are x86-64).
enum MessageType : std::uint8_t {
    kHello = 1,    // joiner: protocol, build, mods
    kWelcome = 2,  // host: accepted (settings, save) or refused (reason)
    kInput = 3,    // frame, snapshot
    kHash = 4,     // scene frame, hash
    kBye = 5,
    kState = 6,  // live join: total size, offset, a piece of the game image
};

constexpr std::uint32_t kMagic = 0x324E424Eu;  // "NBN2"

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_string(std::vector<std::uint8_t>& out, const std::string& s) {
    put32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (at + 4 > data.size()) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        return lo | static_cast<std::uint64_t>(u32()) << 32;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || at + n > data.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data.data() + at), n);
        at += n;
        return s;
    }
    std::vector<std::uint8_t> bytes() {
        const std::uint32_t n = u32();
        if (!ok || at + n > data.size()) { ok = false; return {}; }
        std::vector<std::uint8_t> v(data.begin() + static_cast<std::ptrdiff_t>(at),
                                    data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return v;
    }
};

bool send_all(Socket s, const std::uint8_t* data, std::size_t size) {
    while (size != 0) {
        const int sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool recv_all(Socket s, std::uint8_t* data, std::size_t size) {
    while (size != 0) {
        const int got = ::recv(s, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
        if (got <= 0) return false;
        data += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

bool send_message(Socket s, std::mutex& lock, MessageType type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 5);
    frame.push_back(type);
    put32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    std::lock_guard<std::mutex> guard(lock);
    return send_all(s, frame.data(), frame.size());
}

bool recv_message(Socket s, MessageType& type, std::vector<std::uint8_t>& payload) {
    std::uint8_t header[5];
    if (!recv_all(s, header, sizeof(header))) return false;
    type = static_cast<MessageType>(header[0]);
    std::uint32_t size = 0;
    std::memcpy(&size, header + 1, 4);
    if (size > (64u << 20)) return false;
    payload.resize(size);
    return size == 0 || recv_all(s, payload.data(), size);
}

void configure(Socket s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void start_sockets() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

std::vector<std::uint8_t> encode_settings(const SessionSettings& s) {
    std::vector<std::uint8_t> out;
    for (const std::uint32_t v :
         {s.calendar, s.coop, s.moveset, s.motion_style, s.text_speed, s.language, s.delay, s.rollback, s.live}) {
        put32(out, v);
    }
    return out;
}

SessionSettings decode_settings(Reader& r) {
    SessionSettings s;
    s.calendar = r.u32();
    s.coop = r.u32();
    s.moveset = r.u32();
    s.motion_style = r.u32();
    s.text_speed = r.u32();
    s.language = r.u32();
    s.delay = r.u32();
    s.rollback = r.u32();
    s.live = r.u32();
    return s;
}

}  // namespace

struct Session::Impl {
    Socket socket = kNoSocket;
    std::mutex send_lock;
    std::thread receiver;
    std::mutex lock;
    std::condition_variable changed;
    std::map<std::uint32_t, FrameInput> inputs[2];  // by channel, by frame
    std::map<std::uint32_t, std::uint64_t> local_hashes;
    std::map<std::uint32_t, std::uint64_t> remote_hashes;
    std::atomic<std::uint32_t> desyncs{0};
    bool ended = false;
    unsigned remote_channel = 1;
    std::uint32_t remote_confirmed = 0;  // every remote input up to here has come
    std::vector<std::uint8_t> state;     // live join: the game image as it comes
    std::size_t state_received = 0;
    bool state_ready = false;
    // GALAXY_NET_TEST_LAG=<ms>: the other PC's inputs count only that long after they came (a slow link, on one PC).
    std::chrono::milliseconds test_lag{0};
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::pair<std::uint32_t, FrameInput>>> lagged;

    // With `lock` held: a remote input counts.
    void take_input(std::uint32_t frame, const FrameInput& input) {
        inputs[remote_channel][frame] = input;
        while (inputs[remote_channel].count(remote_confirmed + 1) != 0) ++remote_confirmed;
        changed.notify_all();
    }
    // With `lock` held: the lagged inputs whose time has come count.
    void take_lagged() {
        const auto now = std::chrono::steady_clock::now();
        while (!lagged.empty() && lagged.front().first <= now) {
            take_input(lagged.front().second.first, lagged.front().second.second);
            lagged.pop_front();
        }
    }

    void check_hash(std::uint32_t frame) {
        const auto local = local_hashes.find(frame);
        const auto remote = remote_hashes.find(frame);
        if (local == local_hashes.end() || remote == remote_hashes.end()) return;
        if (local->second != remote->second) {
            if (desyncs.fetch_add(1) == 0) {
                std::fprintf(stderr, "[netplay] desync at scene frame %u: %016llx here, %016llx there\n", frame,
                             static_cast<unsigned long long>(local->second),
                             static_cast<unsigned long long>(remote->second));
            }
        } else if (frame % (kHashEvery * 10u) == 0) {
            std::fprintf(stderr, "[netplay] in step at scene frame %u\n", frame);
        }
        local_hashes.erase(local_hashes.begin(), std::next(local));
        remote_hashes.erase(remote_hashes.begin(), std::next(remote));
    }

    void receive_loop() {
        MessageType type;
        std::vector<std::uint8_t> payload;
        while (recv_message(socket, type, payload)) {
            Reader r{payload};
            if (type == kInput) {
                const std::uint32_t frame = r.u32();
                FrameInput input;
                if (payload.size() == 4 + sizeof(input.snapshot)) {
                    std::memcpy(&input.snapshot, payload.data() + 4, sizeof(input.snapshot));
                    std::lock_guard<std::mutex> guard(lock);
                    if (test_lag.count() > 0) {
                        lagged.push_back({std::chrono::steady_clock::now() + test_lag, {frame, input}});
                    } else {
                        take_input(frame, input);
                    }
                }
            } else if (type == kHash) {
                const std::uint32_t frame = r.u32();
                const std::uint64_t hash = r.u64();
                std::lock_guard<std::mutex> guard(lock);
                remote_hashes[frame] = hash;
                check_hash(frame);
            } else if (type == kState) {
                const std::uint32_t total = r.u32();
                const std::uint32_t offset = r.u32();
                const std::size_t size = payload.size() - 8;
                std::lock_guard<std::mutex> guard(lock);
                if (r.ok && offset <= total && size <= total - offset) {
                    state.resize(total);
                    std::memcpy(state.data() + offset, payload.data() + 8, size);
                    state_received += size;
                    if (state_received == total) {
                        state_ready = true;
                        changed.notify_all();
                    }
                }
            } else if (type == kBye) {
                break;
            }
        }
        std::lock_guard<std::mutex> guard(lock);
        ended = true;
        changed.notify_all();
        std::fprintf(stderr, "[netplay] the other PC left\n");
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {
#if defined(_WIN32)
    char lag[16] = {};
    std::size_t length = 0;
    if (getenv_s(&length, lag, sizeof(lag), "GALAXY_NET_TEST_LAG") == 0 && length > 1) {
#else
    if (const char* lag = std::getenv("GALAXY_NET_TEST_LAG"); lag != nullptr) {
#endif
        impl_->test_lag = std::chrono::milliseconds(std::strtoul(lag, nullptr, 10));
    }
}

Session::~Session() {
    close();
}

bool Session::host(unsigned port, const std::string& build_id, const std::string& mods, const SessionSettings& settings,
                   const std::vector<SaveFile>& save, std::string& error, void (*ready)(unsigned port)) {
    start_sockets();
    const Socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kNoSocket) {
        error = "no socket";
        return false;
    }
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 1) != 0) {
        error = "cannot listen on port " + std::to_string(port);
        close_socket(listener);
        return false;
    }
    socklen_t length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    std::fprintf(stderr, "[netplay] hosting on port %u, waiting for the other player\n", port_);
    if (ready != nullptr) ready(port_);
    for (;;) {
        const Socket s = ::accept(listener, nullptr, nullptr);
        if (s == kNoSocket) {
            error = "accept failed";
            close_socket(listener);
            return false;
        }
        configure(s);
        MessageType type;
        std::vector<std::uint8_t> payload;
        if (!recv_message(s, type, payload) || type != kHello) {
            close_socket(s);
            continue;
        }
        Reader r{payload};
        const std::uint32_t magic = r.u32();
        const std::uint32_t protocol = r.u32();
        const std::string their_build = r.str();
        const std::string their_mods = r.str();
        std::string refusal;
        if (!r.ok || magic != kMagic || protocol != kProtocol) {
            refusal = "a different Nebula version";
        } else if (their_build != build_id) {
            refusal = "a different game build (both PCs need the same Nebula)";
        } else if (their_mods != mods) {
            refusal = "different mods (both PCs need the same mods on: " + mods + ")";
        }
        std::vector<std::uint8_t> answer;
        put32(answer, refusal.empty() ? 1u : 0u);
        if (!refusal.empty()) {
            put_string(answer, refusal);
            send_message(s, impl_->send_lock, kWelcome, answer);
            std::fprintf(stderr, "[netplay] refused a joiner: %s\n", refusal.c_str());
            close_socket(s);
            continue;
        }
        const std::vector<std::uint8_t> encoded = encode_settings(settings);
        answer.insert(answer.end(), encoded.begin(), encoded.end());
        put32(answer, static_cast<std::uint32_t>(save.size()));
        for (const SaveFile& file : save) {
            put_string(answer, file.path);
            put32(answer, static_cast<std::uint32_t>(file.data.size()));
            answer.insert(answer.end(), file.data.begin(), file.data.end());
        }
        if (!send_message(s, impl_->send_lock, kWelcome, answer)) {
            close_socket(s);
            continue;
        }
        close_socket(listener);
        impl_->socket = s;
        impl_->remote_channel = 1;
        impl_->remote_confirmed = settings.delay;
        settings_ = settings;
        if (settings.live != 0) {
            pending_role_ = Role::Host;
        } else {
            role_ = Role::Host;
        }
        impl_->receiver = std::thread([this] { impl_->receive_loop(); });
        std::fprintf(stderr, "[netplay] the other player joined (input delay %u frames)\n", settings.delay);
        return true;
    }
}

bool Session::join(const std::string& where, const std::string& build_id, const std::string& mods,
                   SessionSettings& settings, std::vector<SaveFile>& save, std::string& error) {
    start_sockets();
    std::string hostname = where;
    std::string service = "7777";
    if (const auto colon = where.rfind(':'); colon != std::string::npos) {
        hostname = where.substr(0, colon);
        service = where.substr(colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(hostname.c_str(), service.c_str(), &hints, &found) != 0 || found == nullptr) {
        error = "cannot find " + where;
        return false;
    }
    Socket s = kNoSocket;
    // The host may still be starting: try for 30 seconds.
    for (int attempt = 0; attempt < 60 && s == kNoSocket; ++attempt) {
        s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        if (s != kNoSocket && ::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
            close_socket(s);
            s = kNoSocket;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    freeaddrinfo(found);
    if (s == kNoSocket) {
        error = "cannot connect to " + where;
        return false;
    }
    configure(s);
    std::vector<std::uint8_t> hello;
    put32(hello, kMagic);
    put32(hello, kProtocol);
    put_string(hello, build_id);
    put_string(hello, mods);
    MessageType type;
    std::vector<std::uint8_t> payload;
    if (!send_message(s, impl_->send_lock, kHello, hello) || !recv_message(s, type, payload) || type != kWelcome) {
        error = "the host did not answer";
        close_socket(s);
        return false;
    }
    Reader r{payload};
    if (r.u32() != 1u) {
        error = "the host refused: " + r.str();
        close_socket(s);
        return false;
    }
    settings = decode_settings(r);
    const std::uint32_t files = r.u32();
    save.clear();
    for (std::uint32_t i = 0; i < files && r.ok; ++i) {
        SaveFile file;
        file.path = r.str();
        file.data = r.bytes();
        save.push_back(std::move(file));
    }
    if (!r.ok) {
        error = "the host's answer was cut short";
        close_socket(s);
        return false;
    }
    impl_->socket = s;
    impl_->remote_channel = 0;
    impl_->remote_confirmed = settings.delay;
    settings_ = settings;
    if (settings.live != 0) {
        pending_role_ = Role::Join;
    } else {
        role_ = Role::Join;
    }
    impl_->receiver = std::thread([this] { impl_->receive_loop(); });
    std::fprintf(stderr, "[netplay] joined %s (input delay %u frames, %u save files)\n", where.c_str(), settings.delay,
                 files);
    return true;
}

void Session::send_input(std::uint32_t frame, const FrameInput& input) {
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        impl_->inputs[local_channel()][frame] = input;
    }
    std::vector<std::uint8_t> payload;
    put32(payload, frame);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&input.snapshot);
    payload.insert(payload.end(), bytes, bytes + sizeof(input.snapshot));
    send_message(impl_->socket, impl_->send_lock, kInput, payload);
}

void Session::activate(std::uint32_t base) {
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        base_ = base;
        impl_->remote_confirmed = (std::max)(impl_->remote_confirmed, base + settings_.delay);
        while (impl_->inputs[impl_->remote_channel].count(impl_->remote_confirmed + 1) != 0) ++impl_->remote_confirmed;
    }
    role_ = pending_role_;
    std::fprintf(stderr, "[netplay] the session starts at frame %u\n", base);
}

void Session::send_state(const std::vector<std::uint8_t>& image) {
    constexpr std::size_t kPiece = 8u << 20;
    for (std::size_t at = 0; at < image.size(); at += kPiece) {
        const std::size_t size = (std::min)(kPiece, image.size() - at);
        std::vector<std::uint8_t> payload;
        payload.reserve(size + 8);
        put32(payload, static_cast<std::uint32_t>(image.size()));
        put32(payload, static_cast<std::uint32_t>(at));
        payload.insert(payload.end(), image.begin() + static_cast<std::ptrdiff_t>(at),
                       image.begin() + static_cast<std::ptrdiff_t>(at + size));
        if (!send_message(impl_->socket, impl_->send_lock, kState, payload)) return;
    }
}

bool Session::take_state(std::vector<std::uint8_t>& image) {
    std::unique_lock<std::mutex> guard(impl_->lock);
    impl_->changed.wait(guard, [&] { return impl_->state_ready || impl_->ended; });
    if (!impl_->state_ready) return false;
    image = std::move(impl_->state);
    impl_->state = {};
    impl_->state_ready = false;
    impl_->state_received = 0;
    return true;
}

bool Session::frame_inputs(std::uint32_t frame, FrameInput (&inputs)[2]) {
    if (frame <= base_ + settings_.delay) {
        inputs[0] = {};
        inputs[1] = {};
        return true;
    }
    std::unique_lock<std::mutex> guard(impl_->lock);
    for (;;) {
        impl_->take_lagged();
        if (impl_->ended || (impl_->inputs[0].count(frame) != 0 && impl_->inputs[1].count(frame) != 0)) break;
        impl_->changed.wait_for(guard, std::chrono::milliseconds(impl_->lagged.empty() ? 50 : 1));
    }
    if (impl_->inputs[0].count(frame) == 0 || impl_->inputs[1].count(frame) == 0) return false;
    for (unsigned channel = 0; channel < 2; ++channel) {
        auto& by_frame = impl_->inputs[channel];
        inputs[channel] = by_frame[frame];
        by_frame.erase(by_frame.begin(), by_frame.find(frame));
    }
    return true;
}

bool Session::remote_input(std::uint32_t frame, FrameInput& input) {
    if (frame <= base_ + settings_.delay) {
        input = {};
        return true;
    }
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->take_lagged();
    // Long-confirmed frames are not asked for again (a rollback goes back at most a few frames).
    if (frame > 128) {
        for (auto& by_frame : impl_->inputs) {
            by_frame.erase(by_frame.begin(), by_frame.lower_bound(frame - 128));
        }
    }
    const auto& by_frame = impl_->inputs[impl_->remote_channel];
    const auto it = by_frame.find(frame);
    if (it == by_frame.end()) return false;
    input = it->second;
    return true;
}

std::uint32_t Session::remote_confirmed() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    impl_->take_lagged();
    return (std::max)(impl_->remote_confirmed, base_ + settings_.delay);
}

FrameInput Session::local_input(std::uint32_t frame) {
    if (frame <= base_ + settings_.delay) return {};
    std::lock_guard<std::mutex> guard(impl_->lock);
    const auto& by_frame = impl_->inputs[local_channel()];
    const auto it = by_frame.find(frame);
    return it != by_frame.end() ? it->second : FrameInput{};
}

bool Session::ended() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->ended;
}

void Session::report_hash(std::uint32_t scene_frame, std::uint64_t hash) {
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        impl_->local_hashes[scene_frame] = hash;
        impl_->check_hash(scene_frame);
    }
    std::vector<std::uint8_t> payload;
    put32(payload, scene_frame);
    put64(payload, hash);
    send_message(impl_->socket, impl_->send_lock, kHash, payload);
}

std::uint32_t Session::desyncs() const noexcept {
    return impl_->desyncs.load();
}

void Session::close() {
    if (impl_ == nullptr || impl_->socket == kNoSocket) return;
    send_message(impl_->socket, impl_->send_lock, kBye, {});
    // Closing the socket wakes the receiver's blocking recv (a shutdown alone does not, on Windows).
#if defined(_WIN32)
    shutdown(impl_->socket, SD_BOTH);
#else
    shutdown(impl_->socket, SHUT_RDWR);
#endif
    close_socket(impl_->socket);
    if (impl_->receiver.joinable()) impl_->receiver.join();
    impl_->socket = kNoSocket;
}

Session& session() {
    static Session instance;
    return instance;
}

}  // namespace galaxy::net
