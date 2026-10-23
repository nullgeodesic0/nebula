#include "galaxy/coop_guest.h"

#include "galaxy/agent.h"

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

// The agent's controllers (agent.h), set on the game thread and read by the input thread.
struct AgentPadSlots {
    std::mutex mutex;
    agent::Pad pads[2];
};

AgentPadSlots& agent_pad_slots() {
    static AgentPadSlots slots;
    return slots;
}

}  // namespace

void coop_publish_partner_input(const coop::PartnerInput& input) noexcept {
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    slot.input = input;
}

namespace {
CoopPartnerInputSource g_partner_input_source = nullptr;
}  // namespace

void coop_set_partner_input_source(CoopPartnerInputSource source) noexcept {
    g_partner_input_source = source;
}

coop::PartnerInput coop_partner_input() {
    if (g_partner_input_source != nullptr) {
        coop::PartnerInput input;
        if (g_partner_input_source(input)) return input;
    }
    const agent::Pad pad = agent_pad(1u);
    if (pad.active) {
        // The agent has the second player's controller (agent.h).
        coop::PartnerInput input;
        input.connected = true;
        input.buttons = pad.buttons;
        input.stick_x = pad.stick_x;
        input.stick_y = pad.stick_y;
        input.shake = pad.shake;
        input.pointer_valid = pad.has_pointer;
        input.pointer_x = pad.pointer_x;
        input.pointer_y = pad.pointer_y;
        return input;
    }
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    return slot.input;
}

namespace {
PartnerInputSlot& local_input_slot() {
    static PartnerInputSlot slot;
    return slot;
}
}  // namespace

void coop_publish_local_input(const coop::PartnerInput& input) noexcept {
    PartnerInputSlot& slot = local_input_slot();
    std::lock_guard lock(slot.mutex);
    slot.input = input;
}

coop::PartnerInput coop_local_input() {
    PartnerInputSlot& slot = local_input_slot();
    std::lock_guard lock(slot.mutex);
    return slot.input;
}

void agent_set_pad(unsigned player, const agent::Pad& pad) noexcept {
    if (player > 1u) return;
    AgentPadSlots& slots = agent_pad_slots();
    std::lock_guard lock(slots.mutex);
    slots.pads[player] = pad;
}

agent::Pad agent_pad(unsigned player) noexcept {
    if (player > 1u) return {};
    AgentPadSlots& slots = agent_pad_slots();
    std::lock_guard lock(slots.mutex);
    return slots.pads[player];
}

}  // namespace galaxy
