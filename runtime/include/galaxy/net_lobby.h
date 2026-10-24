#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace galaxy {

// Online co-op's lobby (net_lobby.cpp, docs/NETPLAY.md): what happens before a session. With F1 > Online >
// Joinable on, this PC listens on its port; another PC's Join asks it, and its player accepts or declines. On
// accept both PCs restart into a session (GALAXY_NET_HOST / GALAXY_NET_JOIN) that boots from the host's save and
// takes both players back to where the host was (NetRejoin): the same file, galaxy, mission and spot, the joiner
// as the other brother beside them.

// Where the host is, as the session's start point. file 0: not in a loaded save (the title or file select).
struct NetRejoin {
    unsigned file = 0u;  // 1-6, as the file select passes it
    bool mario = true;   // a Mario file (the joiner plays Luigi) or a Luigi file
    std::string stage;   // the game's stage name, e.g. AstroGalaxy
    int mission = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool placed = false;  // the position is known
};
[[nodiscard]] std::string format_rejoin(const NetRejoin& rejoin);
[[nodiscard]] bool parse_rejoin(const std::string& text, NetRejoin& rejoin);

// Starts the lobby (not in a session). Safe to call more than once.
void net_lobby_start();

// The game thread (agent_guest.cpp): whether the lobby wants a fresh location, and the answer.
[[nodiscard]] bool net_location_wanted() noexcept;
void net_publish_location(const NetRejoin& location);

// The menu's view and actions (renderer_d3d12.cpp, F1 > Online; host_menus.cpp, the game-style menus).
enum class NetLobbyPhase : std::uint8_t {
    Off,          // not joinable, not joining
    Joinable,     // listening at `address`
    PortInUse,    // Joinable, but the port is taken
    Asking,       // joining: asking `address`
    Waiting,      // joining: `address` has the request, its player is choosing
    AskedToJoin,  // hosting: `asker` asks to join
    InSession,
    Outcome,      // the last outcome (`outcome`), until the next action
};
struct NetLobbyView {
    char status[96]{};    // one line, upper case
    bool asking = false;  // host: a PC is asking to join; accept or decline
    char asker[48]{};
    bool in_session = false;
    NetLobbyPhase phase = NetLobbyPhase::Off;
    char address[64]{};   // Joinable: this PC's address:port; Asking/Waiting: the host's
    char outcome[96]{};   // upper case, as `status` shows it
    bool host = false;    // in a session as its host
};
using NetLobbyViewFn = NetLobbyView (*)();
using NetLobbyJoinFn = void (*)(const char* address);  // "address[:port]"
using NetLobbyAnswerFn = void (*)(bool accept);
using NetLobbyCancelFn = void (*)();
using NetLobbyMenuFn = void (*)(int row);  // the online menu's row chosen: 0 resume, 1 exit galaxy, 2 leave
struct NetLobbyHooks {
    NetLobbyViewFn view = nullptr;
    NetLobbyJoinFn join = nullptr;
    NetLobbyAnswerFn answer = nullptr;
    NetLobbyCancelFn cancel = nullptr;
    NetLobbyMenuFn menu = nullptr;
};

// Out of a session: this game restarts on its own, with this PC's own saves and settings.
[[noreturn]] void net_restart_alone();

namespace gx {
// The renderer's copy of the lobby's hooks (none: the Online page shows the lobby as unavailable).
void set_net_lobby_hooks(const NetLobbyHooks& hooks) noexcept;
// The online menu (in a session, on this PC only, the game running on): + or HOME opens and closes it, the
// D-pad or stick moves, A chooses, B closes. One controller poll's edges from the input thread; returns whether it
// is open, when this PC's player's input must stay neutral.
bool online_menu_poll(bool toggle, bool up, bool down, bool choose, bool back) noexcept;
// The online menu as the host menus draw it: open, its row, and whether it answers a join request (rows Accept,
// Not now) rather than offering the session's (Resume, Exit the galaxy, Leave).
struct OnlineMenuState {
    bool open = false;
    int row = 0;
    bool answering = false;
};
[[nodiscard]] OnlineMenuState online_menu_state() noexcept;
// The lobby through the renderer's hooks, for the host menus (an empty view and no-ops without a lobby).
[[nodiscard]] NetLobbyView net_lobby_view() noexcept;
[[nodiscard]] bool net_lobby_available() noexcept;
void net_lobby_join(const char* address);
void net_lobby_cancel();
}  // namespace gx

}  // namespace galaxy
