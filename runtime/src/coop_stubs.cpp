// Neutral co-op and online play for the executables that link the GX layer or the native host without the game
// runtime (the tests, NebulaNative): one player, no session. NebulaRuntime's own objects (coop_guest.cpp,
// netplay.cpp, netplay_session.cpp) define these, so the linker never takes this library's copy for it.

#include "galaxy/coop_guest.h"
#include "galaxy/netplay.h"
#include "galaxy/netplay_session.h"

namespace galaxy {

void coop_request_drop_out() noexcept {}

bool coop_second_is_luigi() noexcept {
    return false;
}

bool coop_in_stage() noexcept {
    return false;
}

bool netplay_tracing() noexcept {
    return false;
}

void netplay_trace(const char*, ...) noexcept {}

bool net_session_requested() noexcept {
    return false;
}

}  // namespace galaxy
