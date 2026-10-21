#pragma once

// Reading, writing and calling the game from a site hook (coop_guest.cpp, agent_guest.cpp).

#include "galaxy/coop.h"
#include "galaxy/native_api.h"

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace galaxy::guest_access {

inline bool plausible(std::uint32_t address) {
    return address >= 0x80000000u && address < 0x81800000u && (address & 3u) == 0u;
}

class Guest {
public:
    Guest(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc)
        : context_(context), memory_(memory), services_(services), pc_(pc) {}

    std::uint32_t u32(std::uint32_t address) const { return guest_load_u32(memory_, address, services_, pc_); }
    std::uint8_t u8(std::uint32_t address) const { return guest_load_u8(memory_, address, services_, pc_); }
    float f32(std::uint32_t address) const { return std::bit_cast<float>(u32(address)); }
    coop::Vec3 vec(std::uint32_t address) const { return {f32(address), f32(address + 4u), f32(address + 8u)}; }
    void set_u32(std::uint32_t address, std::uint32_t value) const {
        guest_store_u32(memory_, address, value, services_, pc_);
    }
    void set_u8(std::uint32_t address, std::uint8_t value) const {
        const std::uint32_t word = address & ~3u;
        const unsigned shift = (3u - (address & 3u)) * 8u;
        set_u32(word, (u32(word) & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift));
    }
    void set_f32(std::uint32_t address, float value) const { set_u32(address, std::bit_cast<std::uint32_t>(value)); }
    void set_vec(std::uint32_t address, coop::Vec3 v) const {
        set_f32(address, v.x);
        set_f32(address + 4u, v.y);
        set_f32(address + 8u, v.z);
    }

    // A game function with up to five integer arguments; returns r3. Callers save and restore the context.
    std::uint32_t call(std::uint32_t function, std::uint32_t r3 = 0u, std::uint32_t r4 = 0u, std::uint32_t r5 = 0u,
                       std::uint32_t r6 = 0u, std::uint32_t r7 = 0u) const {
        context_->gpr[3] = r3;
        context_->gpr[4] = r4;
        context_->gpr[5] = r5;
        context_->gpr[6] = r6;
        context_->gpr[7] = r7;
        services_->call_guest(services_->user, function, context_, memory_);
        return context_->gpr[3];
    }

    // Guest stack room below the caller's frame for vectors passed by reference; r1 moves under it for the
    // calls made while the frame lives and is restored with the context.
    class Frame {
    public:
        explicit Frame(const Guest& guest) : guest_(guest), sp_(guest.context_->gpr[1]) {
            base_ = (sp_ - 0x140u) & ~0xFu;
            guest_.set_u32(base_, sp_);  // back chain
            guest_.context_->gpr[1] = base_;
        }
        ~Frame() { guest_.context_->gpr[1] = sp_; }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        // Slot 0..3, 16 bytes each.
        std::uint32_t vec(unsigned slot, coop::Vec3 value) const {
            const std::uint32_t at = base_ + 0x10u + slot * 0x10u;
            guest_.set_vec(at, value);
            return at;
        }
        // 0xC0 bytes of scratch after the vector slots (an out structure such as a Triangle).
        std::uint32_t scratch() const { return base_ + 0x60u; }

    private:
        const Guest& guest_;
        std::uint32_t sp_;
        std::uint32_t base_ = 0u;
    };

    std::string string(std::uint32_t address, std::size_t limit = 96u) const {
        std::string text;
        if (!plausible(address & ~3u)) return text;
        for (std::size_t i = 0; i < limit; ++i) {
            const std::uint8_t c = u8(address + static_cast<std::uint32_t>(i));
            if (c == 0u) break;
            text.push_back(static_cast<char>(c));
        }
        return text;
    }

    // The address of a NUL-terminated string in the game's data, or 0.
    std::uint32_t find_string(std::string_view text) const {
        constexpr std::uint32_t kBegin = 0x80004000u;
        constexpr std::uint32_t kEnd = 0x80700000u;
        const auto first = static_cast<std::uint8_t>(text[0]);
        for (std::uint32_t at = kBegin + 1u; at + text.size() + 1u < kEnd; ++at) {
            if (u8(at) != first || u8(at - 1u) != 0u) continue;
            bool same = true;
            for (std::size_t i = 1; i < text.size() && same; ++i) {
                same = u8(at + static_cast<std::uint32_t>(i)) == static_cast<std::uint8_t>(text[i]);
            }
            if (same && u8(at + static_cast<std::uint32_t>(text.size())) == 0u) return at;
        }
        return 0u;
    }

    PpcContext* context() const { return context_; }

private:
    PpcContext* context_;
    GuestMemoryV1* memory_;
    const NativeServicesV1* services_;
    std::uint32_t pc_;
};

// Restores the interrupted game call's registers when a hook has called into the game itself. The timer
// state (time base, DEC) is the machine's, not the call's: the game's own calls move it, and rewinding it
// makes the runtime's DEC tracker diverge from the guest.
class SavedContext {
public:
    explicit SavedContext(PpcContext* context) : context_(context), saved_(*context) {}
    ~SavedContext() {
        const PpcTimerState timer = capture_timer_state(context_);
        *context_ = saved_;
        restore_timer_state(context_, timer);
    }
    SavedContext(const SavedContext&) = delete;
    SavedContext& operator=(const SavedContext&) = delete;

private:
    PpcContext* context_;
    PpcContext saved_;
};

}  // namespace galaxy::guest_access
