#pragma once

// Which device plays which brother on this PC (docs/MULTIPLAYER_UX.md "Player flows"). The input poll
// (native_host.cpp) tells the router which devices are connected and which had input or pressed the join
// button (the Wii + : Start, Menu or Options); the router says whose input is the second player's.
//
// - Every device drives the first player unless it is seated elsewhere. The one that moved last is "driving"
//   (its prompts, tilt and vibration).
// - While the second player is out, a controller that is not driving and not seated First may press its join
//   button: its input goes to him (coop_guest.cpp drops him in on that +) and the controller is his.
//   The keyboard joins only when it is seated Second.
// - If his controller disconnects (or is seated away), he stays in for kGraceSeconds with no input
//   (`waiting`): the same controller coming back, or any free controller's join button, takes over.
//   After the grace his input reads disconnected and coop_guest sits him out.
// - Seats (First, Second, Nobody; Auto by default) are this PC's choice, saved in runtime_settings.ini as
//   `player_devices=xbox.0=second,keyboard.0=first`. Only one device may sit Second.
//
// Online each PC is one player and the router is not used (every device here is that player's).

#include "galaxy/input_bindings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galaxy::players {

// (ControlScheme << 8) | device slot: the XInput user index, or the HID worker's slot.
using DeviceKey = std::uint32_t;
inline constexpr DeviceKey kNoDevice = 0xFFFFFFFFu;
// The keyboard and mouse, as one device.
inline constexpr DeviceKey kKeyboard = static_cast<DeviceKey>(input::ControlScheme::Keyboard) << 8u;

[[nodiscard]] constexpr DeviceKey device_key(input::ControlScheme scheme, std::uint8_t slot) noexcept {
    return (static_cast<DeviceKey>(scheme) << 8u) | slot;
}
[[nodiscard]] constexpr input::ControlScheme device_scheme(DeviceKey key) noexcept {
    return static_cast<input::ControlScheme>((key >> 8u) & 0xFFu);
}

enum class Seat : std::uint8_t { Auto, First, Second, Nobody };
[[nodiscard]] const char* seat_id(Seat seat) noexcept;

// One connected device, this poll.
struct DeviceFrame {
    DeviceKey key = kNoDevice;
    bool input = false;          // any button, stick, trigger, gyro turn or shake
    bool join_pressed = false;   // its join button (+) went down
};

struct Route {
    // The device whose input is the first player's driving device (kKeyboard, or kNoDevice before any input).
    DeviceKey driving = kNoDevice;
    // The device whose input is the second player's this poll (kNoDevice: none).
    DeviceKey second = kNoDevice;
    // He is in but his device is gone: publish him connected with no input until the grace ends.
    bool waiting = false;
    float grace_left = 0.0f;
    // Devices whose join button goes to the second player, never to the first player's pause: the free
    // controllers while he is out (or waiting for a device).
    std::array<DeviceKey, 8> join_candidates{};
    std::size_t join_candidate_count = 0u;
    [[nodiscard]] bool is_join_candidate(DeviceKey key) const noexcept;
    // Devices whose input is nobody's: seated Nobody, or seated Second while he is out.
    std::array<DeviceKey, 8> excluded{};
    std::size_t excluded_count = 0u;
    [[nodiscard]] bool is_excluded(DeviceKey key) const noexcept;
    // Whether the device's input goes to the first player.
    [[nodiscard]] bool feeds_first(DeviceKey key) const noexcept { return key != second && !is_excluded(key); }
};

enum class EventKind : std::uint8_t { Joined, Disconnected, Reconnected, Left };
struct Event {
    EventKind kind = EventKind::Joined;
    DeviceKey device = kNoDevice;
};

class Router {
public:
    static constexpr double kGraceSeconds = 10.0;
    // How long a join press keeps feeding the second player, so the game's frame sees the +.
    static constexpr double kJoinFeedSeconds = 1.0;

    // `second_present`: the second player is in (coop_second_present()). `now`: seconds, any steady origin.
    // `coop` false (co-op off, or online): no second player, only the driving device.
    Route step(std::span<const DeviceFrame> devices, bool second_present, double now, bool coop = true);

    void set_seat(DeviceKey key, Seat seat);
    [[nodiscard]] Seat seat(DeviceKey key) const noexcept;
    // `xbox.0=second,keyboard.0=first` (Auto seats are not written).
    [[nodiscard]] std::string format_seats() const;
    void parse_seats(std::string_view text);
    // The second player's device (kNoDevice: none bound).
    [[nodiscard]] DeviceKey second_device() const noexcept { return bound_; }

    std::vector<Event> take_events();

private:
    struct SeatEntry {
        DeviceKey key;
        Seat seat;
    };
    std::vector<SeatEntry> seats_;
    DeviceKey driving_ = kNoDevice;
    DeviceKey bound_ = kNoDevice;     // his device while he is in
    DeviceKey pending_ = kNoDevice;   // a join press waiting for him to appear
    double pending_until_ = 0.0;
    bool orphaned_ = false;           // his device was seated away: wait for another, never pick by default
    bool grace_ = false;
    double grace_start_ = 0.0;
    bool was_present_ = false;
    std::vector<Event> events_;
};

// The router the input poll owns, with a copy of its state for the menus (any thread).
struct DeviceStatus {
    DeviceKey key = kNoDevice;
    Seat seat = Seat::Auto;
    bool driving = false;
    bool second = false;
    bool join_candidate = false;  // its + would drop the second player in
    bool input = false;           // it had input in the last poll
};
struct Status {
    std::array<DeviceStatus, 12> devices{};
    std::size_t device_count = 0u;
    bool second_present = false;
    bool waiting = false;
    float grace_left = 0.0f;
    std::uint64_t generation = 0u;
};
void publish_status(const Status& status);
[[nodiscard]] Status status();
// A seat change from a menu, applied by the next poll (and saved).
void request_seat(DeviceKey key, Seat seat);
// Taken by the input poll.
[[nodiscard]] std::vector<std::pair<DeviceKey, Seat>> take_seat_requests();
// The first player's buttons and pointer for the host menus (host_menus.cpp), which sit beside the game's own and
// read the same input without taking it from the game. Presses between two takes are kept (OR'd).
struct MenuInput {
    enum Button : std::uint32_t {
        kA = 1u << 0, kB = 1u << 1, kUp = 1u << 2, kDown = 1u << 3, kLeft = 1u << 4, kRight = 1u << 5,
        kPlus = 1u << 6, kMinus = 1u << 7,
    };
    std::uint32_t held = 0u;
    std::uint32_t pressed = 0u;
    bool pointer_valid = false;
    float pointer_x = 0.0f, pointer_y = 0.0f;  // -1..1 across the picture, y down
};
void publish_menu_input(std::uint32_t held, bool pointer_valid, float pointer_x, float pointer_y);
[[nodiscard]] MenuInput take_menu_input();
// A host window that covers the game's own menu (the Online window) takes the first player's buttons: the game
// reads none while it is up, nor those still held as it closes (the input poll blanks them).
void set_menu_capture(bool capture) noexcept;
[[nodiscard]] bool menu_capture() noexcept;

// Joins, disconnects and leaves for the status cards (the menus take them).
void post_events(std::vector<Event> events);
[[nodiscard]] std::vector<Event> take_posted_events();

}  // namespace galaxy::players
