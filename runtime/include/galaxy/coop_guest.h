#pragma once

#include "galaxy/coop.h"
#include "galaxy/native_api.h"

#include <cstdint>

namespace galaxy {

// Mario and Luigi co-op (docs/MULTIPLAYER.md). The runtime side of SiteHookKind::CoopBeforeCall and
// CoopAfterCall: `id` names the game call (coop_call_id in translate.rs). Simulation thread, or the
// game's archive loader thread for the archive calls.
void coop_before_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                      std::uint32_t guest_pc);
void coop_after_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                     std::uint32_t guest_pc);

// Whether `mario` (a Mario, MarioActor + 0x230) is the second player's, which the Odyssey moveset leaves alone.
[[nodiscard]] bool coop_is_second_player_mario(std::uint32_t mario) noexcept;

// The second player's controller, published by the input thread and read once a game frame.
void coop_publish_partner_input(const coop::PartnerInput& input) noexcept;

}  // namespace galaxy
