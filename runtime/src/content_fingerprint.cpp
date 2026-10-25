#include "galaxy/content_fingerprint.h"

#include <atomic>
#include <mutex>

namespace galaxy {

namespace {
std::atomic<std::uint64_t> s_fingerprint{0u};
std::mutex s_lock;
std::string s_mods;
}  // namespace

void set_content_fingerprint(std::uint64_t value, std::string mods) noexcept {
    {
        const std::lock_guard lock(s_lock);
        s_mods = std::move(mods);
    }
    s_fingerprint.store(value, std::memory_order_release);
}

std::uint64_t content_fingerprint() noexcept {
    return s_fingerprint.load(std::memory_order_acquire);
}

std::string content_mods() {
    const std::lock_guard lock(s_lock);
    return s_mods;
}

}  // namespace galaxy
