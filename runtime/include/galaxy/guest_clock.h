#pragma once

// Where the guest's time base starts. A Wii's time base counts from 2000-01-01 in local time (the IPL sets it from
// the real-time clock; Dolphin does the same), and the game dates its saves from it (OSGetTime ->
// OSTicksToCalendarTime). Starting at 0 dated every save 01/01/2000.
//
// The start is this PC's local time at boot, fixed once read. Online both PCs must agree: the host sends its start in
// the session bundle and a joiner that boots with the host takes it (netplay_session.cpp); a live joiner gets it with
// the snapshot's registers. A run that records or replays input (GALAXY_INPUT_RECORD / GALAXY_INPUT_REPLAY) starts
// at 0, so a replay matches its recording. GALAXY_GUEST_CLOCK=<seconds since 1970, local> sets it (0: 2000-01-01).

#include <cstdint>

namespace galaxy {

inline constexpr std::uint64_t kGuestTimeBaseHz = 60'750'000ull;

// The time base's start in ticks since 2000-01-01, local time.
[[nodiscard]] std::uint64_t guest_clock_start_ticks() noexcept;
// A joiner, before the game starts: the host's start.
void set_guest_clock_start_ticks(std::uint64_t ticks) noexcept;

}  // namespace galaxy
