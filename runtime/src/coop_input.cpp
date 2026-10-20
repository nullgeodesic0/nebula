#include "galaxy/coop_guest.h"

#include <mutex>

namespace galaxy {

namespace {

// Written by the input thread (native_host.cpp), read once a game frame by coop_guest.cpp. Kept apart from
// coop_guest.cpp so the host library links without the guest-side hooks.
struct PartnerInputSlot {
    std::mutex mutex;
    coop::PartnerInput input;
};

PartnerInputSlot& partner_input_slot() {
    static PartnerInputSlot slot;
    return slot;
}

}  // namespace

void coop_publish_partner_input(const coop::PartnerInput& input) noexcept {
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    slot.input = input;
}

coop::PartnerInput coop_partner_input() {
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    return slot.input;
}

}  // namespace galaxy
