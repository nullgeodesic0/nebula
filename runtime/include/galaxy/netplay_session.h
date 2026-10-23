#pragma once

#include "galaxy/native_api.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaxy {

// Online co-op over UDP: deterministic lockstep with input delay (docs/NETPLAY.md). Both PCs simulate the whole
// game in deterministic mode from the same save; each frame the host's controller drives the first player and the
// joiner's the second, each input applied `delay` frames after it was read so it reaches the other PC in time.
// A frame whose remote input has not arrived waits for it. The host's EFB peek answers (the Star Pointer's depth,
// which depends on its GPU) travel with its input. MEM1 hashes are exchanged to catch a desync.
//
// GALAXY_NET_HOST=<port> hosts; GALAXY_NET_JOIN=<address>:<port> joins; GALAXY_NET_DELAY=<frames> (default 3).
// Either implies GALAXY_DETERMINISTIC=1.

inline constexpr std::uint32_t kNetStatusSize = 0x84u;  // KPADStatus
inline constexpr std::uint32_t kNetMaxPeeks = 8u;        // EFB peek answers a frame carries

struct NetInput {
    std::uint32_t count = 0u;                         // KPADRead's sample count (r3)
    std::array<std::uint8_t, kNetStatusSize> status{};  // big-endian, as the guest has it
    std::uint32_t peek_count = 0u;                    // host only: its peek answers from an earlier frame
    std::array<std::uint32_t, kNetMaxPeeks> peeks{};
    std::uint32_t command = 0u;                       // NetCommand: both PCs carry it out at this frame
};

// Session-wide actions from a player's online menu (net_lobby.cpp), carried by their input so both PCs act on
// the same frame (agent_guest.cpp netplay_apply_command).
// TestStarAppear (tests only, tools/agent/live_join.py): a stage's Power Star appears, with its cutscene.
enum class NetCommand : std::uint32_t { None = 0, ExitGalaxy = 1, Join = 2, TestStarAppear = 0x100 };

// The next local input carries `command` (net_local_input).
void net_request_command(NetCommand command) noexcept;
// Ends the session on both PCs: tells the peer, then this game restarts on its own (net_lobby.cpp).
[[noreturn]] void net_leave();
// The peer said it left (or has not been heard from for a while): this game should restart on its own.
[[nodiscard]] bool net_peer_left() noexcept;

// Whether a session was requested (environment read once).
[[nodiscard]] bool net_session_requested() noexcept;
[[nodiscard]] bool net_is_host() noexcept;

// At the start of frame `frame` (the first player's per-frame KPAD read): paces to 59.94 Hz, receives, sends the
// periodic hash. `memory` for the hash.
void net_frame_begin(std::uint32_t frame, const GuestMemoryV1* memory);

// This PC's controller as read for `frame` (the local first-player channel): becomes this PC's input for
// frame + delay, sent to the peer.
void net_local_input(std::uint32_t frame, std::uint32_t count, const std::uint8_t* status);

// The input of `player` (0 host, 1 joiner) for `frame`; waits for the peer's when it has not arrived.
[[nodiscard]] const NetInput& net_input(std::uint32_t frame, unsigned player);

// Live join (docs/HOT_JOIN.md): a joinable game is deterministic from boot and runs as a session host on its own
// (the solo phase: its player's input is its controller's at once, the second player's neutral, Luigi sitting
// out). Accepting a joiner snapshots the game at the next frame F and keeps playing; the snapshot, then every frame's
// input from F on, go to the joiner as one stream. The joiner (restarted once into GALAXY_NET_JOIN with
// GALAXY_NET_HOTJOIN=1, with the host's saves) boots to its title, loads the snapshot, replays the host's inputs
// unpaced and unrendered and says when it has caught up; the host then names the join frame J, whose input carries
// NetCommand::Join (Luigi drops in on both PCs) and from which the session is lockstep with input delay.
[[nodiscard]] bool net_live() noexcept;
// Host: in the solo phase with nobody joining, so a joiner can be taken live.
[[nodiscard]] bool net_live_can_accept() noexcept;
// Host (the lobby's thread, its listening socket closed): the session opens on `port` and the snapshot is taken
// at the next frame.
void net_live_accept(unsigned port);
// netplay.cpp, at a frame's start (the first player's KPAD read, before its input):
[[nodiscard]] bool net_live_snapshot_due() noexcept;                                   // host: capture now
void net_live_snapshot_taken(std::uint32_t frame, std::vector<std::uint8_t>&& image);  // host: the capture's image
// Joiner: the snapshot's image when this frame is the one to load it at (it waits for the image there), else null.
// The session keeps it until net_live_resumed.
[[nodiscard]] const std::vector<std::uint8_t>* net_live_load_due(std::uint32_t frame);
void net_live_resumed(std::uint32_t frame);  // joiner: running the host's game from its frame `frame`

// Rollback (GALAXY_NET_ROLLBACK=1; GALAXY_NET_ROLLBACK_FRAMES, default 8, how far ahead of the peer's newest input a
// PC may run). net_rollback_target, at a frame's start: the oldest frame that ran on a prediction now known wrong
// (netplay.cpp restores it and simulates forward again), or 0xFFFFFFFF.
[[nodiscard]] bool net_rollback_enabled() noexcept;
[[nodiscard]] std::uint32_t net_rollback_target(std::uint32_t frame);
// Frames being simulated again: nothing of this PC (controller, peek answers) is sampled or sent anew.
void net_set_resimulating(bool on) noexcept;

// EFB peeks: this PC's answer for the `sequence`-th peek of `frame`, and the answer both PCs use (the host's,
// from `delay + 1` frames earlier, or `local` before there is one).
[[nodiscard]] std::uint32_t net_peek(std::uint32_t frame, std::uint32_t sequence, std::uint32_t local) noexcept;

}  // namespace galaxy
