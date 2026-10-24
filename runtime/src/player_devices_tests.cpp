// The device router (player_devices.h): who drives the first player, who joins as the second, the disconnect
// grace and the seats a menu chooses.

#include "galaxy/player_devices.h"

#include <cstdio>
#include <vector>

namespace {

using namespace galaxy::players;
using galaxy::input::ControlScheme;

bool expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

const DeviceKey kPadA = device_key(ControlScheme::Xbox, 0);
const DeviceKey kPadB = device_key(ControlScheme::PlayStation, 0);
const DeviceKey kPadC = device_key(ControlScheme::SwitchPro, 1);

DeviceFrame idle(DeviceKey key) { return {key, false, false}; }
DeviceFrame moving(DeviceKey key) { return {key, true, false}; }
DeviceFrame join(DeviceKey key) { return {key, true, true}; }

bool has_event(const std::vector<Event>& events, EventKind kind, DeviceKey key) {
    for (const Event& e : events) {
        if (e.kind == kind && e.device == key) return true;
    }
    return false;
}

}  // namespace

int main() {
    bool ok = true;

    {
        // One controller and the keyboard: the controller is the first player's, its + pauses.
        Router router;
        const DeviceFrame frames[] = {idle(kKeyboard), join(kPadA)};
        const Route route = router.step(frames, false, 0.0);
        ok &= expect(route.second == kNoDevice && !route.is_join_candidate(kPadA) && route.driving == kPadA,
                     "a lone controller pauses, it never asks for the second player");
    }
    {
        // Two controllers: A plays, B's + drops the second player in.
        Router router;
        DeviceFrame frames[] = {moving(kPadA), idle(kPadB)};
        Route route = router.step(frames, false, 0.0);
        ok &= expect(route.driving == kPadA && route.is_join_candidate(kPadB) && !route.is_join_candidate(kPadA),
                     "the free controller is the join candidate");
        frames[1] = join(kPadB);
        route = router.step(frames, false, 0.1);
        ok &= expect(route.second == kPadB && route.driving == kPadA, "its + goes to the second player");
        frames[1] = moving(kPadB);
        route = router.step(frames, false, 0.5);
        ok &= expect(route.second == kPadB, "and keeps going to him while the game takes it");
        route = router.step(frames, true, 0.6);
        ok &= expect(route.second == kPadB && router.second_device() == kPadB &&
                         has_event(router.take_events(), EventKind::Joined, kPadB),
                     "he is in: the controller is his");
        ok &= expect(!route.feeds_first(kPadB) && route.feeds_first(kPadA), "his input is not the first player's");

        // Disconnect: he waits with no input, then the same controller comes back.
        const DeviceFrame a_only[] = {moving(kPadA)};
        route = router.step(a_only, true, 1.0);
        ok &= expect(route.waiting && route.second == kNoDevice &&
                         has_event(router.take_events(), EventKind::Disconnected, kPadB),
                     "his controller disconnects: he waits");
        route = router.step(a_only, true, 6.0);
        ok &= expect(route.waiting && route.grace_left > 4.0f && route.grace_left < 6.0f, "the grace counts down");
        route = router.step(frames, true, 7.0);
        ok &= expect(route.second == kPadB && !route.waiting &&
                         has_event(router.take_events(), EventKind::Reconnected, kPadB),
                     "it comes back inside the grace: he plays on");

        // Disconnect past the grace: his input reads disconnected.
        route = router.step(a_only, true, 8.0);
        route = router.step(a_only, true, 18.5);
        ok &= expect(!route.waiting && route.second == kNoDevice, "past the grace he is sat out");
        route = router.step(a_only, false, 18.6);
        ok &= expect(router.second_device() == kNoDevice && has_event(router.take_events(), EventKind::Left, kPadB),
                     "and has left");
    }
    {
        // While he waits, another free controller's + takes his place.
        Router router;
        DeviceFrame frames[] = {moving(kPadA), join(kPadB), idle(kPadC)};
        router.step(frames, false, 0.0);
        router.step(frames, true, 0.1);
        const DeviceFrame without_b[] = {moving(kPadA), join(kPadC)};
        router.step(without_b, true, 1.0);
        const Route route = router.step(without_b, true, 1.1);
        ok &= expect(route.second == kPadC && router.second_device() == kPadC, "another controller's + takes over");
    }
    {
        // Seated First: never joins. The keyboard seated Second joins.
        Router router;
        router.set_seat(kPadB, Seat::First);
        router.set_seat(kKeyboard, Seat::Second);
        const DeviceFrame frames[] = {moving(kPadA), join(kPadB), idle(kKeyboard)};
        Route route = router.step(frames, false, 0.0);
        ok &= expect(!route.is_join_candidate(kPadB) && route.second == kNoDevice, "a controller seated First never joins");
        ok &= expect(route.is_join_candidate(kKeyboard) && route.is_excluded(kKeyboard),
                     "the keyboard seated Second waits for him, feeding nobody");
        const DeviceFrame keys_join[] = {moving(kPadA), idle(kPadB), join(kKeyboard)};
        route = router.step(keys_join, false, 0.2);
        ok &= expect(route.second == kKeyboard, "the keyboard's + drops him in");
    }
    {
        // A save started with two: the second controller, or the only one with the keyboard as the first player's.
        Router two;
        const DeviceFrame pads[] = {idle(kKeyboard), moving(kPadA), idle(kPadB)};
        Route route = two.step(pads, true, 0.0);
        ok &= expect(route.second == kPadB, "started with two: the second controller is his");
        Router one;
        const DeviceFrame pad_and_keys[] = {idle(kKeyboard), idle(kPadA)};
        route = one.step(pad_and_keys, true, 0.0);
        ok &= expect(route.second == kPadA && route.feeds_first(kKeyboard), "the only controller is his, the keyboard the first's");
    }
    {
        // Seating a controller Second while he plays hands him that controller; seating his away makes him wait.
        Router router;
        DeviceFrame frames[] = {moving(kPadA), join(kPadB), idle(kPadC)};
        router.step(frames, false, 0.0);
        frames[1] = moving(kPadB);
        router.step(frames, true, 0.1);
        router.set_seat(kPadC, Seat::Second);
        Route route = router.step(frames, true, 0.2);
        ok &= expect(route.second == kPadC && route.feeds_first(kPadB), "seated Second: his controller now");
        router.set_seat(kPadC, Seat::Nobody);
        route = router.step(frames, true, 0.3);
        ok &= expect(route.waiting && route.second == kNoDevice && route.is_excluded(kPadC),
                     "his controller seated away: he waits for another");
    }
    {
        Router router;
        router.set_seat(kPadB, Seat::Second);
        router.set_seat(kKeyboard, Seat::First);
        router.set_seat(kPadC, Seat::Nobody);
        const std::string text = router.format_seats();
        Router back;
        back.parse_seats(text);
        ok &= expect(back.seat(kPadB) == Seat::Second && back.seat(kKeyboard) == Seat::First &&
                         back.seat(kPadC) == Seat::Nobody && back.format_seats() == text,
                     "the seats save and load");
        back.set_seat(kPadA, Seat::Second);
        ok &= expect(back.seat(kPadB) == Seat::Auto && back.seat(kPadA) == Seat::Second, "only one device sits Second");
        back.parse_seats("bogus,xbox.x=second,dualsense.2=nobody,xbox.1=sideways");
        ok &= expect(back.seat(device_key(ControlScheme::PlayStation, 2)) == Seat::Nobody && back.format_seats() == "playstation.2=nobody",
                     "bad entries are skipped");
    }

    std::printf("%s\n", ok ? "player devices: all passed" : "player devices: FAILED");
    return ok ? 0 : 1;
}
