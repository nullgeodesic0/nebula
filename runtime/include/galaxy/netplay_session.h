#pragma once

#include "galaxy/native_api.h"

#include <array>
#include <cstdint>

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
};

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

// EFB peeks: this PC's answer for the `sequence`-th peek of `frame`, and the answer both PCs use (the host's,
// from `delay + 1` frames earlier, or `local` before there is one).
[[nodiscard]] std::uint32_t net_peek(std::uint32_t frame, std::uint32_t sequence, std::uint32_t local) noexcept;

}  // namespace galaxy
