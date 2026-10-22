#pragma once

#include "galaxy/native_api.h"

#include <cstdint>

namespace galaxy {

// Groundwork for rollback netcode (docs/NETPLAY.md), around every KPADRead (coop_guest.cpp's call hook, id 1):
// GALAXY_INPUT_RECORD=<file> records both players' controller data per game frame as the game receives it,
// GALAXY_INPUT_REPLAY=<file> gives the game a recording's instead, and GALAXY_STATE_HASH=<file> writes a hash of
// MEM1 every GALAXY_STATE_HASH_EVERY frames (60), with per-megabyte hashes when GALAXY_STATE_HASH_CHUNKS is set.
void netplay_kpad_before(PpcContext* context, const GuestMemoryV1* memory, std::uint32_t guest_pc);
void netplay_kpad_after(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                        std::uint32_t guest_pc);
// The channel of the KPADRead in flight at the per-frame read site (its before-hook seen), else 0xFFFFFFFF.
[[nodiscard]] std::uint32_t netplay_pending_channel() noexcept;
// Game frames so far (the first player's per-frame controller reads).
[[nodiscard]] std::uint32_t netplay_frame() noexcept;

}  // namespace galaxy
