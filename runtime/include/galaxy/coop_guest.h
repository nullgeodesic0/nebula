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

// 1 or 2 while split screen draws the first or second half (GameScene::draw3D runs twice), else 0: the scene's
// begin and end markers go around both (native_runtime.cpp).
[[nodiscard]] unsigned coop_split_pass() noexcept;

// Where split screen's line goes into the GX stream: native_runtime.cpp flushes the gather pipe and attaches a
// SplitClip marker (gx_d3d12.h attach_split_clip) with `plane`, in logical EFB pixels, and `pass` (1 or 2);
// zero and pass 0 clear it. `separator`: the black line between the halves (a, b, c as `plane`, opacity),
// drawn by the pass 0 marker after the second half.
using SplitClipSink = void (*)(void* user, const float plane[3], unsigned pass, const float separator[4]);
void coop_set_split_clip_sink(SplitClipSink sink, void* user) noexcept;

// The second player's MarioActor beside `first` (the scene's player), or 0: none, or one left from the last scene
// (the agent's state and commands, agent_guest.cpp).
[[nodiscard]] std::uint32_t coop_second_player(std::uint32_t first) noexcept;
// Done in the second player's own turn after their next move, where MR::setPlayerPos and the player's calls act on
// them: the agent's teleport and power-up for the second player (agent_guest.cpp).
void coop_request_second_player_teleport(coop::Vec3 position) noexcept;
void coop_request_second_player_mode(std::uint16_t mode) noexcept;
// Co-op lives for the agent's state: the second player's lives (-1 before the first stage) and whether a player
// (0 first, 1 second) is down or out of lives.
[[nodiscard]] int coop_second_lives() noexcept;
[[nodiscard]] coop::DownState coop_down_state(unsigned player) noexcept;

// The agent's per-frame step (agent_guest.cpp), at the first player's controller read: with or without co-op.
void agent_frame(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t guest_pc);

// The second player's controller, published by the input thread and read once a game frame.
void coop_publish_partner_input(const coop::PartnerInput& input) noexcept;
[[nodiscard]] coop::PartnerInput coop_partner_input();

}  // namespace galaxy
