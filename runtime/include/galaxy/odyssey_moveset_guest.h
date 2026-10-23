#pragma once

#include "galaxy/native_api.h"

#include <memory>

namespace galaxy {

// Rollback: both players' moveset state.
struct OdysseyRollbackState;
[[nodiscard]] std::shared_ptr<OdysseyRollbackState> odyssey_rollback_save();
void odyssey_rollback_restore(const OdysseyRollbackState& state);

// Runtime side of SiteHookKind::MarioActionMain (r3 the Mario, before Mario::actionMain runs):
// reads Mario and the player's buttons, and applies the Odyssey moveset (odyssey_moveset.h) when the
// setting is on. Simulation thread.
void tick_odyssey_moveset(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                          std::uint32_t guest_pc);

}  // namespace galaxy
