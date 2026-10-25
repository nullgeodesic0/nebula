#include "galaxy/guest_clock.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

namespace galaxy {
namespace {

std::mutex g_lock;
std::optional<std::uint64_t> g_start;

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    std::string out;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) out = value;
    std::free(value);
    return out;
}

constexpr std::int64_t kWiiEpochUnix = 946'684'800;  // 2000-01-01 00:00:00

// Seconds since 2000-01-01 on this PC's wall clock in local time (the Wii's clock is local), or of `unix_seconds`.
std::uint64_t local_seconds_since_2000(std::time_t unix_seconds) {
    std::tm local{};
    if (localtime_s(&local, &unix_seconds) != 0) return 0u;
    const std::time_t as_utc = _mkgmtime(&local);
    return as_utc > kWiiEpochUnix ? static_cast<std::uint64_t>(as_utc - kWiiEpochUnix) : 0u;
}

std::uint64_t first_start() {
    if (const std::string set = env("GALAXY_GUEST_CLOCK"); !set.empty()) {
        const long long seconds = std::atoll(set.c_str());
        return seconds <= 0 ? 0u : local_seconds_since_2000(static_cast<std::time_t>(seconds)) * kGuestTimeBaseHz;
    }
    if (!env("GALAXY_INPUT_RECORD").empty() || !env("GALAXY_INPUT_REPLAY").empty()) return 0u;
    return local_seconds_since_2000(std::time(nullptr)) * kGuestTimeBaseHz;
}

}  // namespace

std::uint64_t guest_clock_start_ticks() noexcept {
    const std::lock_guard lock(g_lock);
    if (!g_start) {
        g_start = first_start();
        std::fprintf(stderr, "[clock] the guest's clock starts %llu s after 2000-01-01\n",
                     static_cast<unsigned long long>(*g_start / kGuestTimeBaseHz));
    }
    return *g_start;
}

void set_guest_clock_start_ticks(std::uint64_t ticks) noexcept {
    const std::lock_guard lock(g_lock);
    g_start = ticks;
    std::fprintf(stderr, "[clock] the host's clock: %llu s after 2000-01-01\n",
                 static_cast<unsigned long long>(ticks / kGuestTimeBaseHz));
}

}  // namespace galaxy
