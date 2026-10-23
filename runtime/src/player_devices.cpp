#include "galaxy/player_devices.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace galaxy::players {

namespace {

template <std::size_t N>
void add(std::array<DeviceKey, N>& keys, std::size_t& count, DeviceKey key) {
    if (count < N) keys[count++] = key;
}

template <std::size_t N>
bool contains(const std::array<DeviceKey, N>& keys, std::size_t count, DeviceKey key) {
    return std::find(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(count), key) !=
           keys.begin() + static_cast<std::ptrdiff_t>(count);
}

}  // namespace

const char* seat_id(Seat seat) noexcept {
    switch (seat) {
    case Seat::Auto: return "auto";
    case Seat::First: return "first";
    case Seat::Second: return "second";
    case Seat::Nobody: return "nobody";
    }
    return "auto";
}

bool Route::is_join_candidate(DeviceKey key) const noexcept {
    return contains(join_candidates, join_candidate_count, key);
}

bool Route::is_excluded(DeviceKey key) const noexcept {
    return contains(excluded, excluded_count, key);
}

Route Router::step(std::span<const DeviceFrame> devices, bool second_present, double now, bool coop) {
    Route route;
    const auto connected = [&](DeviceKey key) {
        return key != kNoDevice &&
               std::any_of(devices.begin(), devices.end(), [key](const DeviceFrame& d) { return d.key == key; });
    };
    if (!coop) {
        // No second player here (co-op off, or online): every device is the first player's.
        for (const DeviceFrame& d : devices) {
            if (d.input) driving_ = d.key;
        }
        if (!connected(driving_)) {
            driving_ = kNoDevice;
            for (const DeviceFrame& d : devices) {
                if (driving_ == kNoDevice || driving_ == kKeyboard) driving_ = d.key;
            }
        }
        route.driving = driving_;
        return route;
    }

    // He came in (on a join press, or the save started with two) or went out.
    if (second_present && !was_present_) {
        if (pending_ != kNoDevice) {
            bound_ = pending_;
            events_.push_back({EventKind::Joined, bound_});
        }
        pending_ = kNoDevice;
        grace_ = false;
        orphaned_ = false;
    } else if (!second_present && was_present_) {
        events_.push_back({EventKind::Left, bound_});
        bound_ = kNoDevice;
        grace_ = false;
        orphaned_ = false;
    }
    was_present_ = second_present;

    // Before anything has moved, the first player's is the first controller he may use (a lone controller's
    // + then pauses rather than asking for the second player), else the keyboard.
    const auto first_players = [&](const DeviceFrame& d) {
        const Seat s = seat(d.key);
        return s != Seat::Second && s != Seat::Nobody && d.key != bound_ && d.key != pending_;
    };
    if (!connected(driving_)) {
        driving_ = kNoDevice;
        for (const DeviceFrame& d : devices) {
            if (first_players(d) && (driving_ == kNoDevice || driving_ == kKeyboard)) driving_ = d.key;
        }
    }
    const DeviceKey driving_before = driving_;
    // A controller free to join (or take over his place): not driving, not his, not kept for the first player.
    // The keyboard only when seated Second.
    const auto free_to_join = [&](const DeviceFrame& d) {
        const Seat s = seat(d.key);
        if (s == Seat::First || s == Seat::Nobody || d.key == bound_) return false;
        if (s == Seat::Second) return true;
        return d.key != kKeyboard && d.key != driving_before;
    };

    if (second_present) {
        if (bound_ == kNoDevice && !orphaned_) {
            // The save started with two: the device seated Second, else the second connected controller, or the
            // only one (the keyboard and mouse are then the first player's).
            for (const DeviceFrame& d : devices) {
                if (seat(d.key) == Seat::Second) bound_ = d.key;
            }
            if (bound_ == kNoDevice) {
                std::size_t count = 0u;
                for (const DeviceFrame& d : devices) {
                    const Seat s = seat(d.key);
                    if (d.key == kKeyboard || s == Seat::First || s == Seat::Nobody) continue;
                    if (count < 2u) bound_ = d.key;
                    ++count;
                }
            }
        }
        if (connected(bound_)) {
            route.second = bound_;
            if (grace_) events_.push_back({EventKind::Reconnected, bound_});
            grace_ = false;
        } else {
            if (!grace_) {
                grace_ = true;
                grace_start_ = now;
                events_.push_back({EventKind::Disconnected, bound_});
            }
            for (const DeviceFrame& d : devices) {
                if (!free_to_join(d)) continue;
                add(route.join_candidates, route.join_candidate_count, d.key);
                if (d.join_pressed && route.second == kNoDevice) {
                    bound_ = d.key;
                    orphaned_ = false;
                    grace_ = false;
                    route.second = d.key;
                    events_.push_back({EventKind::Reconnected, d.key});
                }
            }
            if (route.second == kNoDevice) {
                const double left = kGraceSeconds - (now - grace_start_);
                // After the grace his input reads disconnected and coop_guest sits him out.
                route.waiting = left > 0.0;
                route.grace_left = route.waiting ? static_cast<float>(left) : 0.0f;
            }
        }
    } else {
        for (const DeviceFrame& d : devices) {
            if (!free_to_join(d)) continue;
            add(route.join_candidates, route.join_candidate_count, d.key);
            if (d.join_pressed) {
                pending_ = d.key;
                pending_until_ = now + kJoinFeedSeconds;
            }
        }
        if (pending_ != kNoDevice && (now > pending_until_ || !connected(pending_))) pending_ = kNoDevice;
        // His input while the + is pressed, so the game sees it and drops him in.
        route.second = pending_;
    }

    for (const DeviceFrame& d : devices) {
        const Seat s = seat(d.key);
        if (d.key == route.second) continue;
        if (s == Seat::Nobody || (s == Seat::Second && d.key != bound_)) {
            add(route.excluded, route.excluded_count, d.key);
        }
    }
    // The first player's driving device: the last one of his with input.
    for (const DeviceFrame& d : devices) {
        if (d.input && route.feeds_first(d.key) && !(d.join_pressed && route.is_join_candidate(d.key))) {
            driving_ = d.key;
        }
    }
    if (!connected(driving_) || !route.feeds_first(driving_)) driving_ = kNoDevice;
    route.driving = driving_;
    return route;
}

void Router::set_seat(DeviceKey key, Seat seat) {
    if (seat == Seat::Second) {
        for (SeatEntry& entry : seats_) {
            if (entry.seat == Seat::Second) entry.seat = Seat::Auto;
        }
    }
    const auto it = std::find_if(seats_.begin(), seats_.end(), [key](const SeatEntry& e) { return e.key == key; });
    if (it != seats_.end()) {
        it->seat = seat;
    } else {
        seats_.push_back({key, seat});
    }
    std::erase_if(seats_, [](const SeatEntry& e) { return e.seat == Seat::Auto; });
    if (!was_present_) return;
    if (seat == Seat::Second && bound_ != key) {
        // His controller from now on.
        bound_ = key;
        orphaned_ = false;
        grace_ = false;
    } else if (seat != Seat::Second && seat != Seat::Auto && key == bound_) {
        // Seated away: he waits for another controller as if it had disconnected.
        bound_ = kNoDevice;
        orphaned_ = true;
    }
}

Seat Router::seat(DeviceKey key) const noexcept {
    for (const SeatEntry& entry : seats_) {
        if (entry.key == key) return entry.seat;
    }
    return Seat::Auto;
}

std::string Router::format_seats() const {
    std::string text;
    for (const SeatEntry& entry : seats_) {
        if (entry.seat == Seat::Auto) continue;
        if (!text.empty()) text += ',';
        text += input::control_scheme_id(device_scheme(entry.key));
        text += '.';
        text += std::to_string(entry.key & 0xFFu);
        text += '=';
        text += seat_id(entry.seat);
    }
    return text;
}

void Router::parse_seats(std::string_view text) {
    seats_.clear();
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1u);
        const std::size_t dot = item.find('.');
        const std::size_t equals = item.find('=');
        if (dot == std::string_view::npos || equals == std::string_view::npos || equals < dot) continue;
        const auto scheme = input::parse_control_scheme(item.substr(0, dot));
        if (!scheme) continue;
        unsigned slot = 0u;
        const std::string_view digits = item.substr(dot + 1u, equals - dot - 1u);
        if (digits.empty() || digits.size() > 3u) continue;
        bool ok = true;
        for (const char c : digits) {
            if (c < '0' || c > '9') ok = false;
            slot = slot * 10u + static_cast<unsigned>(c - '0');
        }
        if (!ok || slot > 255u) continue;
        const std::string_view name = item.substr(equals + 1u);
        Seat seat = Seat::Auto;
        if (name == "first") seat = Seat::First;
        else if (name == "second") seat = Seat::Second;
        else if (name == "nobody") seat = Seat::Nobody;
        else continue;
        set_seat(device_key(*scheme, static_cast<std::uint8_t>(slot)), seat);
    }
}

std::vector<Event> Router::take_events() {
    std::vector<Event> taken;
    taken.swap(events_);
    return taken;
}

namespace {
std::mutex s_lock;
Status s_status;
std::vector<std::pair<DeviceKey, Seat>> s_seat_requests;
std::vector<Event> s_events;
}  // namespace

void publish_status(const Status& status) {
    std::lock_guard lock(s_lock);
    const std::uint64_t generation = s_status.generation + 1u;
    s_status = status;
    s_status.generation = generation;
}

Status status() {
    std::lock_guard lock(s_lock);
    return s_status;
}

void request_seat(DeviceKey key, Seat seat) {
    std::lock_guard lock(s_lock);
    s_seat_requests.emplace_back(key, seat);
}

std::vector<std::pair<DeviceKey, Seat>> take_seat_requests() {
    std::lock_guard lock(s_lock);
    std::vector<std::pair<DeviceKey, Seat>> taken;
    taken.swap(s_seat_requests);
    return taken;
}

void post_events(std::vector<Event> events) {
    if (events.empty()) return;
    std::lock_guard lock(s_lock);
    s_events.insert(s_events.end(), events.begin(), events.end());
    if (s_events.size() > 32u) s_events.erase(s_events.begin(), s_events.end() - 32);
}

std::vector<Event> take_posted_events() {
    std::lock_guard lock(s_lock);
    std::vector<Event> taken;
    taken.swap(s_events);
    return taken;
}

}  // namespace galaxy::players
