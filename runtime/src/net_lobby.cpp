#include "galaxy/net_lobby.h"

#include "galaxy/netplay_session.h"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace galaxy {
namespace {

using Clock = std::chrono::steady_clock;

// Lobby datagrams: Header, then a text payload (Accept: the rejoin point; Decline: why). Not the session's
// protocol (netplay_session.cpp), which starts once both PCs have restarted.
constexpr std::uint32_t kLobbyMagic = 0x4C424E4Eu;  // "NNBL"
constexpr std::uint8_t kLobbyVersion = 1u;
enum class Message : std::uint8_t { Ask = 1, Wait = 2, Accept = 3, Decline = 4 };

#pragma pack(push, 1)
struct Header {
    std::uint32_t magic;
    std::uint8_t type;
    std::uint8_t version;
    std::uint16_t reserved;
    std::uint64_t build;
};
#pragma pack(pop)

enum class Role { None, Hosting, Asking };

struct Lobby {
    std::mutex mutex;
    bool started = false;
    bool in_session = false;
    std::uint64_t build = 0u;
    // Hosting (Joinable on): the listening socket and the PC asking, if any.
    SOCKET listen = INVALID_SOCKET;
    unsigned listen_port = 0u;
    bool listen_failed = false;
    bool asked = false;
    sockaddr_in asker{};
    std::string asker_text;
    Clock::time_point asker_seen{};
    bool accept = false;
    bool decline = false;
    // Joining: the host asked, how it answered.
    Role role = Role::None;
    SOCKET ask = INVALID_SOCKET;
    sockaddr_in host{};
    std::string host_text;  // address:port as typed
    Clock::time_point ask_started{};
    Clock::time_point last_ask{};
    bool host_waiting = false;
    std::string message;  // the last outcome, shown until the next action
    // The game thread's answer to "where are we" (net_location_wanted).
    std::atomic<bool> location_wanted{false};
    bool location_fresh = false;
    NetRejoin location;
    std::string this_pc;  // this PC's LAN address, for the host to tell the joiner
};

Lobby& lobby() {
    static Lobby l;
    return l;
}

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

std::string address_text(const sockaddr_in& address) {
    char text[64]{};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return text;
}

std::string lan_address() {
    char name[256]{};
    if (gethostname(name, sizeof(name)) != 0) return {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &result) != 0) return {};
    std::string found;
    for (addrinfo* at = result; at != nullptr && found.empty(); at = at->ai_next) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(at->ai_addr);
        if ((ntohl(address->sin_addr.s_addr) >> 24u) != 127u) found = address_text(*address);
    }
    freeaddrinfo(result);
    return found;
}

SOCKET open_socket(unsigned port) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    u_long nonblocking = 1u;
    ioctlsocket(s, FIONBIO, &nonblocking);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

void send_message(SOCKET s, const sockaddr_in& to, Message type, std::uint64_t build, const std::string& text = {}) {
    if (s == INVALID_SOCKET) return;
    std::vector<char> packet(sizeof(Header) + std::min<std::size_t>(text.size(), 1024u));
    const Header header{kLobbyMagic, static_cast<std::uint8_t>(type), kLobbyVersion, 0u, build};
    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), text.data(), packet.size() - sizeof(header));
    (void)sendto(s, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr*>(&to),
                 sizeof(to));
}

// One datagram, or false.
bool receive_message(SOCKET s, sockaddr_in& from, Header& header, std::string& text) {
    if (s == INVALID_SOCKET) return false;
    char buffer[1500];
    int from_size = sizeof(from);
    const int size = recvfrom(s, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
    if (size < static_cast<int>(sizeof(Header))) return false;
    std::memcpy(&header, buffer, sizeof(header));
    if (header.magic != kLobbyMagic || header.version != kLobbyVersion) return false;
    text.assign(buffer + sizeof(Header), static_cast<std::size_t>(size) - sizeof(Header));
    return true;
}

// Restarts this game into a session: the same program and command line, with the session's variables, once
// this process has gone (GALAXY_NET_WAIT_PID: its instance lease and port must be free first).
[[noreturn]] void restart_into_session(const std::vector<std::pair<std::wstring, std::wstring>>& variables) {
    for (const auto& [name, value] : variables) SetEnvironmentVariableW(name.c_str(), value.c_str());
    SetEnvironmentVariableW(L"GALAXY_TWO_PLAYERS", L"1");
    SetEnvironmentVariableW(L"GALAXY_NET_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::fprintf(stderr, "[lobby] restarting into the session\n");
    std::fflush(stderr);
    if (CreateProcessW(exe, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        std::fprintf(stderr, "[lobby] the session could not start (error %lu)\n", GetLastError());
        std::fflush(stderr);
    }
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) Sleep(1000);
}

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

// The host accepted: ask the game where it is (a second at most), tell the joiner, restart as the host.
void accept_joiner(Lobby& l, std::unique_lock<std::mutex>& lock) {
    l.location_fresh = false;
    l.location_wanted.store(true, std::memory_order_release);
    const auto asked = Clock::now();
    while (!l.location_fresh && Clock::now() - asked < std::chrono::milliseconds(1500)) {
        lock.unlock();
        Sleep(10);
        lock.lock();
    }
    l.location_wanted.store(false, std::memory_order_release);
    const NetRejoin rejoin = l.location_fresh ? l.location : NetRejoin{};
    const std::string text = format_rejoin(rejoin);
    for (int i = 0; i < 5; ++i) {
        send_message(l.listen, l.asker, Message::Accept, l.build, text);
        Sleep(20);
    }
    const unsigned port = l.listen_port;
    close_socket(l.listen);
    std::fprintf(stderr, "[lobby] %s joins; the session starts at %s\n", l.asker_text.c_str(), text.c_str());
    restart_into_session({{L"GALAXY_NET_HOST", std::to_wstring(port)}, {L"GALAXY_NET_REJOIN", widen(text)}});
}

void step(Lobby& l, std::unique_lock<std::mutex>& lock) {
    const RuntimeSettings settings = get_runtime_settings();
    // Hosting: listen while Joinable is on.
    const bool want_listen = settings.net_joinable && l.role != Role::Asking;
    if (want_listen && (l.listen == INVALID_SOCKET || l.listen_port != settings.net_port) && !l.listen_failed) {
        close_socket(l.listen);
        l.listen = open_socket(settings.net_port);
        l.listen_port = settings.net_port;
        l.listen_failed = l.listen == INVALID_SOCKET;
        if (l.this_pc.empty()) l.this_pc = lan_address();
        if (!l.listen_failed) std::fprintf(stderr, "[lobby] joinable on UDP port %u\n", l.listen_port);
    }
    if (!want_listen || l.listen_port != settings.net_port) {
        if (l.listen != INVALID_SOCKET) std::fprintf(stderr, "[lobby] not joinable\n");
        close_socket(l.listen);
        l.listen_failed = false;
        l.asked = false;
    }
    sockaddr_in from{};
    Header header{};
    std::string text;
    while (receive_message(l.listen, from, header, text)) {
        if (static_cast<Message>(header.type) != Message::Ask) continue;
        if (header.build != l.build) {
            send_message(l.listen, from, Message::Decline, l.build, "A DIFFERENT BUILD OF NEBULA");
            continue;
        }
        if (l.asked && (from.sin_addr.s_addr != l.asker.sin_addr.s_addr || from.sin_port != l.asker.sin_port)) {
            send_message(l.listen, from, Message::Decline, l.build, "SOMEONE ELSE IS JOINING");
            continue;
        }
        if (!l.asked) {
            l.asked = true;
            l.asker = from;
            l.asker_text = address_text(from);
            std::fprintf(stderr, "[lobby] %s asks to join\n", l.asker_text.c_str());
        }
        l.asker_seen = Clock::now();
        send_message(l.listen, from, Message::Wait, l.build);
    }
    if (l.asked && Clock::now() - l.asker_seen > std::chrono::seconds(5)) {
        l.asked = false;
        l.message = l.asker_text + " STOPPED ASKING";
    }
    if (l.asked && l.decline) {
        send_message(l.listen, l.asker, Message::Decline, l.build, "THE HOST SAID NO");
        l.asked = false;
        l.message = "DECLINED " + l.asker_text;
    }
    l.decline = false;
    if (l.asked && l.accept) accept_joiner(l, lock);
    l.accept = false;

    // Joining: ask twice a second until the host answers.
    if (l.role == Role::Asking) {
        const auto now = Clock::now();
        if (now - l.last_ask > std::chrono::milliseconds(500)) {
            send_message(l.ask, l.host, Message::Ask, l.build);
            l.last_ask = now;
        }
        while (receive_message(l.ask, from, header, text)) {
            if (from.sin_addr.s_addr != l.host.sin_addr.s_addr || from.sin_port != l.host.sin_port) continue;
            const auto type = static_cast<Message>(header.type);
            if (type == Message::Wait) {
                l.host_waiting = true;
            } else if (type == Message::Decline) {
                l.role = Role::None;
                close_socket(l.ask);
                l.message = text.empty() ? std::string("THE HOST SAID NO") : text;
                return;
            } else if (type == Message::Accept) {
                close_socket(l.ask);
                std::fprintf(stderr, "[lobby] %s accepted; the session starts at %s\n", l.host_text.c_str(),
                             text.c_str());
                restart_into_session({{L"GALAXY_NET_JOIN", widen(l.host_text)}, {L"GALAXY_NET_REJOIN", widen(text)}});
            }
        }
        if (!l.host_waiting && now - l.ask_started > std::chrono::seconds(20)) {
            l.role = Role::None;
            close_socket(l.ask);
            l.message = "NO ANSWER FROM " + l.host_text;
        }
    }
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0u;
    std::string out;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) out = value;
    std::free(value);
    return out;
}

void join(const char* address);

void run() {
    Lobby& l = lobby();
    // Tests (tools/agent): GALAXY_NET_AUTOJOIN=<address> asks that host once the game is up;
    // GALAXY_NET_AUTOACCEPT=1 accepts the first PC that asks.
    const std::string auto_join = env("GALAXY_NET_AUTOJOIN");
    const bool auto_accept = env("GALAXY_NET_AUTOACCEPT") == "1";
    const auto started = Clock::now();
    bool auto_joined = false;
    for (;;) {
        if (!auto_join.empty() && !auto_joined && Clock::now() - started > std::chrono::seconds(10)) {
            auto_joined = true;
            join(auto_join.c_str());
        }
        {
            std::unique_lock lock(l.mutex);
            if (auto_accept && l.asked) l.accept = true;
            if (!l.in_session) step(l, lock);
        }
        Sleep(20);
    }
}

NetLobbyView view() {
    Lobby& l = lobby();
    std::lock_guard lock(l.mutex);
    NetLobbyView v;
    std::string status;
    const RuntimeSettings settings = get_runtime_settings();
    if (l.in_session) {
        v.in_session = true;
        status = net_is_host() ? "IN A SESSION: YOU ARE THE HOST" : "IN A SESSION: YOU JOINED";
    } else if (l.role == Role::Asking) {
        status = l.host_waiting ? "WAITING FOR " + l.host_text + " TO ACCEPT" : "ASKING " + l.host_text + "...";
    } else if (l.asked) {
        v.asking = true;
        (void)strncpy_s(v.asker, sizeof(v.asker), l.asker_text.c_str(), _TRUNCATE);
        status = l.asker_text + " WANTS TO JOIN";
    } else if (!l.message.empty()) {
        status = l.message;
    } else if (settings.net_joinable && l.listen_failed) {
        status = "PORT " + std::to_string(settings.net_port) + " IS IN USE";
    } else if (settings.net_joinable && l.listen != INVALID_SOCKET) {
        status = "JOINABLE AT " + (l.this_pc.empty() ? std::string("THIS PC") : l.this_pc) + ":" +
                 std::to_string(l.listen_port);
    } else {
        status = "NOT JOINABLE";
    }
    (void)strncpy_s(v.status, sizeof(v.status), status.c_str(), _TRUNCATE);
    return v;
}

void join(const char* address) {
    Lobby& l = lobby();
    std::lock_guard lock(l.mutex);
    if (l.in_session || address == nullptr) return;
    std::string text = address;
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    if (text.empty()) {
        l.message = "TYPE THE HOST'S ADDRESS FIRST";
        return;
    }
    const auto colon = text.rfind(':');
    const std::string name = colon == std::string::npos ? text : text.substr(0, colon);
    const std::string port = colon == std::string::npos ? "7777" : text.substr(colon + 1u);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(name.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        l.message = "CANNOT FIND " + text;
        return;
    }
    std::memcpy(&l.host, result->ai_addr, sizeof(l.host));
    freeaddrinfo(result);
    close_socket(l.ask);
    l.ask = open_socket(0u);
    if (l.ask == INVALID_SOCKET) {
        l.message = "CANNOT OPEN A NETWORK SOCKET";
        return;
    }
    l.host_text = name + ":" + port;
    l.role = Role::Asking;
    l.host_waiting = false;
    l.message.clear();
    l.ask_started = Clock::now();
    l.last_ask = Clock::time_point{};
    std::fprintf(stderr, "[lobby] asking %s to join\n", l.host_text.c_str());
}

void answer(bool accept) {
    Lobby& l = lobby();
    std::lock_guard lock(l.mutex);
    if (!l.asked) return;
    (accept ? l.accept : l.decline) = true;
}

void cancel() {
    Lobby& l = lobby();
    std::lock_guard lock(l.mutex);
    if (l.role == Role::Asking) {
        l.role = Role::None;
        close_socket(l.ask);
        l.message = "STOPPED ASKING";
    } else if (l.asked) {
        l.decline = true;
    } else {
        l.message.clear();
    }
}

}  // namespace

std::string format_rejoin(const NetRejoin& rejoin) {
    char text[160]{};
    std::snprintf(text, sizeof(text), "file=%u;brother=%s;stage=%s;mission=%d;pos=%s%.3f,%.3f,%.3f", rejoin.file,
                  rejoin.mario ? "mario" : "luigi", rejoin.stage.c_str(), rejoin.mission, rejoin.placed ? "" : "none:",
                  rejoin.x, rejoin.y, rejoin.z);
    return text;
}

bool parse_rejoin(const std::string& text, NetRejoin& rejoin) {
    NetRejoin parsed;
    std::size_t start = 0u;
    while (start < text.size()) {
        std::size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        const std::string field = text.substr(start, end - start);
        const auto equals = field.find('=');
        if (equals != std::string::npos) {
            const std::string key = field.substr(0, equals);
            std::string value = field.substr(equals + 1u);
            if (key == "file") parsed.file = static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10));
            if (key == "brother") parsed.mario = value != "luigi";
            if (key == "stage") parsed.stage = value;
            if (key == "mission") parsed.mission = std::atoi(value.c_str());
            if (key == "pos") {
                parsed.placed = value.rfind("none:", 0) != 0;
                if (!parsed.placed) value = value.substr(5u);
                if (sscanf_s(value.c_str(), "%f,%f,%f", &parsed.x, &parsed.y, &parsed.z) != 3) parsed.placed = false;
            }
        }
        start = end + 1u;
    }
    if (parsed.file > 6u) return false;
    for (const char c : parsed.stage) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    rejoin = parsed;
    return true;
}

void net_lobby_start() {
    Lobby& l = lobby();
    {
        std::lock_guard lock(l.mutex);
        if (l.started) return;
        l.started = true;
        l.in_session = net_session_requested();
        l.build = build_identity();
    }
    WSADATA wsa{};
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
    gx::set_net_lobby_hooks({&view, &join, &answer, &cancel});
    std::thread(run).detach();
}

bool net_location_wanted() noexcept {
    return lobby().location_wanted.load(std::memory_order_acquire);
}

void net_publish_location(const NetRejoin& location) {
    Lobby& l = lobby();
    std::lock_guard lock(l.mutex);
    l.location = location;
    l.location_fresh = true;
    l.location_wanted.store(false, std::memory_order_release);
}

}  // namespace galaxy
