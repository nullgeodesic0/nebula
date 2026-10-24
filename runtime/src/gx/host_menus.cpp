#include "galaxy/gx/host_menus.h"

#include "galaxy/button_prompts.h"
#include "galaxy/coop_guest.h"
#include "galaxy/game_menu_probe.h"
#include "galaxy/gx/menu_screens.h"
#include "galaxy/gx/renderer_d3d12.h"
#include "galaxy/input_bindings.h"
#include "galaxy/menu_sounds.h"
#include "galaxy/net_lobby.h"
#include "galaxy/netplay_session.h"
#include "galaxy/player_devices.h"
#include "galaxy/runtime_settings.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

namespace galaxy::gx {

namespace {

std::atomic<bool> s_text_focus{false};
std::mutex s_keys_lock;
std::u32string s_keys;

std::u16string widen(const char* text) {
    std::u16string out;
    for (const char* c = text; *c != '\0'; ++c) out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*c)));
    return out;
}

menu::OnlineView online_view() {
    menu::OnlineView o;
    if (!net_lobby_available()) return o;
    const NetLobbyView v = net_lobby_view();
    switch (v.phase) {
    case NetLobbyPhase::Off: o.phase = menu::OnlinePhase::Off; break;
    case NetLobbyPhase::Joinable: o.phase = menu::OnlinePhase::Joinable; break;
    case NetLobbyPhase::PortInUse: o.phase = menu::OnlinePhase::PortInUse; break;
    case NetLobbyPhase::Asking: o.phase = menu::OnlinePhase::Asking; break;
    case NetLobbyPhase::Waiting: o.phase = menu::OnlinePhase::Waiting; break;
    case NetLobbyPhase::AskedToJoin: o.phase = menu::OnlinePhase::AskedToJoin; break;
    case NetLobbyPhase::InSession: o.phase = menu::OnlinePhase::InSession; break;
    case NetLobbyPhase::Outcome: o.phase = menu::OnlinePhase::Outcome; break;
    }
    o.address = widen(v.address);
    o.asker = widen(v.asker);
    o.outcome = widen(v.outcome);
    o.host = v.host;
    const RuntimeSettings settings = get_runtime_settings();
    o.joinable = settings.net_joinable;
    // Joinable since the start runs the game in step from boot (a live join); switched on later, a join restarts.
    o.joinable_live = net_session_requested();
    o.port = settings.net_port;
    o.join_address = widen(settings.net_join_address.data());
    const OnlineMenuState menu_state = online_menu_state();
    o.menu_open = menu_state.open;
    o.menu_row = menu_state.row;
    o.menu_answering = menu_state.answering;
    return o;
}

std::string env_value(const char* name) {
    char value[32]{};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), name) != 0 || length <= 1u) return {};
    return value;
}

// GALAXY_MENU_DEMO=pause|dialog: the kit's pause buttons or Yes/No window over the game, the first button hovering
// in and out every second, to compare with the game's own menus.
void demo(menu::DrawList& list, const menu::Assets& assets, const menu::Space& space, double seconds) {
    static const std::string scene = env_value("GALAXY_MENU_DEMO");
    if (scene.empty()) return;
    static menu::ButtonAnim first;
    static double last = seconds;
    const auto frames = static_cast<float>((seconds - last) * 60.0);
    last = seconds;
    first.hover(static_cast<long long>(seconds) % 2 == 0);
    first.step(frames);
    if (scene == "pause") {
        const menu::PillStyle style;
        menu::pill(list, assets, space, style, 0.0f, 24.0f, u"Back", first.look(), 1.0f);
        menu::pill(list, assets, space, style, 0.0f, -73.0f, u"Return to the\nObservatory", menu::ButtonAnim{}.look(),
                   1.0f);
    } else if (scene == "dialog") {
        menu::window(list, assets, space, 0.0f, 0.0f, 552.0f, 388.0f, 1.0f);
        menu::TextStyle body;
        body.font_w = 22.95f;
        body.font_h = 29.7f;
        body.char_space = 1.0f;
        body.line_space = -1.0f;
        body.color = menu::rgb255(80, 90, 130);
        menu::text(list, assets, space, body, 0.0f, 38.0f, u"Return to the Observatory?", 1.0f);
        const menu::PillStyle yes_no = menu::dialog_pill_style();
        menu::pill(list, assets, space, yes_no, -116.0f, -136.0f, u"No", first.look(menu::kDialogHover), 1.0f);
        menu::pill(list, assets, space, yes_no, 116.0f, -136.0f, u"Yes", menu::ButtonAnim{}.look(menu::kDialogHover),
                   1.0f);
    }
}

menu::GlyphFamily family_of(input::ControlScheme scheme) {
    switch (scheme) {
    case input::ControlScheme::Xbox: return menu::GlyphFamily::Xbox;
    case input::ControlScheme::DualSense: return menu::GlyphFamily::DualSense;
    default: return menu::GlyphFamily::Wii;  // Switch controllers and the keyboard show the Wii's buttons
    }
}

std::u16string device_name(input::ControlScheme scheme) {
    switch (scheme) {
    case input::ControlScheme::Keyboard: return u"Keyboard";
    case input::ControlScheme::Xbox: return u"Xbox";
    case input::ControlScheme::SwitchPro: return u"Pro Controller";
    case input::ControlScheme::SwitchPro2: return u"Pro Controller 2";
    case input::ControlScheme::DualSense: return u"DualSense";
    }
    return u"Controller";
}

// A Wii target's prompt on a device: its own binding's button for the pads, the Wii's otherwise.
menu::GlyphRef slot_glyph(input::ControlScheme scheme, prompts::Slot slot) {
    const auto wii = menu::GlyphRef{menu::GlyphFamily::Wii, prompts::kSlotGlyph[static_cast<std::size_t>(slot)]};
    const auto family = prompts::family_for_scheme(scheme);
    if (!family) return wii;
    const prompts::PromptSet set = prompts::make_prompt_set(*family, input::get_controls_config().table(scheme));
    const prompts::Label label = set.labels[static_cast<std::size_t>(slot)];
    if (label == prompts::Label::None) return wii;
    return {*family == prompts::Family::Xbox ? menu::GlyphFamily::Xbox : menu::GlyphFamily::DualSense,
            static_cast<std::uint16_t>(label)};
}

// The prompt the game's own text shows for a slot now (the controller in use, button_prompts.h published()).
menu::GlyphRef shown_glyph(prompts::Slot slot) {
    const prompts::PromptSet set = prompts::published();
    const prompts::Label label = set.labels[static_cast<std::size_t>(slot)];
    if (set.family == prompts::Family::None || label == prompts::Label::None) {
        return {menu::GlyphFamily::Wii, prompts::kSlotGlyph[static_cast<std::size_t>(slot)]};
    }
    return {set.family == prompts::Family::Xbox ? menu::GlyphFamily::Xbox : menu::GlyphFamily::DualSense,
            static_cast<std::uint16_t>(label)};
}

menu::SeatChoice seat_choice(players::Seat seat) {
    switch (seat) {
    case players::Seat::First: return menu::SeatChoice::First;
    case players::Seat::Second: return menu::SeatChoice::Second;
    case players::Seat::Nobody: return menu::SeatChoice::Off;
    case players::Seat::Auto: break;
    }
    return menu::SeatChoice::Auto;
}

players::Seat seat_of(menu::SeatChoice seat) {
    switch (seat) {
    case menu::SeatChoice::First: return players::Seat::First;
    case menu::SeatChoice::Second: return players::Seat::Second;
    case menu::SeatChoice::Off: return players::Seat::Nobody;
    case menu::SeatChoice::Auto: break;
    }
    return players::Seat::Auto;
}

menu::ScreensInput gather(double seconds, float extent_x, bool settings_open) {
    menu::ScreensInput in;
    in.seconds = seconds;
    in.extent_x = extent_x;
    in.local_coop = coop_available() && !net_session_requested();
    in.in_level = coop_in_stage();
    in.pause_shown = game_menus::shown(game_menus::Menu::Pause);
    in.second_is_luigi = coop_second_is_luigi();
    const RuntimeSettings settings = get_runtime_settings();
    in.file_select = file_select_options_shown() && !settings_open;
    in.two_players = settings.coop;
    in.odyssey_moves = settings.moveset.enabled;
    in.two_players_glyph = shown_glyph(prompts::Slot::One);
    in.odyssey_moves_glyph = shown_glyph(prompts::Slot::Plus);
    const players::Status status = players::status();
    in.second_present = status.second_present;
    in.second_waiting = status.waiting;
    in.grace_left = status.grace_left;
    // Controllers first, the keyboard last; a second controller of a kind is numbered.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < status.device_count; ++i) {
            const players::DeviceStatus& d = status.devices[i];
            const input::ControlScheme scheme = players::device_scheme(d.key);
            const bool keyboard = scheme == input::ControlScheme::Keyboard;
            if (keyboard != (pass == 1)) continue;
            menu::DeviceView view;
            view.key = d.key;
            view.family = family_of(scheme);
            view.keyboard = keyboard;
            view.name = device_name(scheme);
            int same = 0;
            for (std::size_t j = 0; j < status.device_count; ++j) {
                same += players::device_scheme(status.devices[j].key) == scheme ? 1 : 0;
            }
            if (same > 1) view.name += u" " + std::u16string(1, static_cast<char16_t>(u'1' + (d.key & 0xFFu)));
            view.seat = seat_choice(d.seat);
            view.driving = d.driving;
            view.second = d.second;
            view.join_candidate = d.join_candidate;
            view.input = d.input;
            view.join_glyph = slot_glyph(scheme, prompts::Slot::Plus);
            in.devices.push_back(std::move(view));
        }
    }
    for (const players::Event& e : players::take_posted_events()) {
        menu::CardKind kind = menu::CardKind::Joined;
        switch (e.kind) {
        case players::EventKind::Joined: kind = menu::CardKind::Joined; break;
        case players::EventKind::Disconnected: kind = menu::CardKind::Disconnected; break;
        case players::EventKind::Reconnected: kind = menu::CardKind::Reconnected; break;
        case players::EventKind::Left: kind = menu::CardKind::Left; break;
        }
        in.events.push_back({kind, e.device});
    }
    in.online = online_view();
    in.online.plus_glyph = shown_glyph(prompts::Slot::Plus);
    {
        const std::lock_guard lock(s_keys_lock);
        in.keys.swap(s_keys);
    }
    const players::MenuInput input = players::take_menu_input();
    in.held = input.held;
    in.pressed = input.pressed;
    in.pointer_valid = input.pointer_valid;
    in.pointer_x = input.pointer_x;
    in.pointer_y = input.pointer_y;
    return in;
}

}  // namespace

bool host_menu_text_focus() noexcept {
    return s_text_focus.load(std::memory_order_relaxed);
}

void push_host_menu_key(char32_t key) {
    const std::lock_guard lock(s_keys_lock);
    if (s_keys.size() < 64u) s_keys.push_back(key);
}

menu::DrawList& host_menu_list() noexcept {
    static menu::DrawList list;
    return list;
}

void draw_host_menus(RendererD3D12& renderer, const PictureRect& picture, double seconds, bool settings_open) {
    // From the first frame: the assets are prepared off the render thread and arrive a moment later.
    if (!renderer.ensure_menu_kit_loaded()) return;
    const menu::Assets* assets = renderer.menu_kit_assets();
    if (assets == nullptr || picture.width <= 0.0f || picture.height <= 0.0f) {
        host_menu_list().clear();
        return;
    }
    menu::DrawList& list = host_menu_list();
    static menu::Screens screens;
    const menu::Space space = menu::Space::for_picture(picture.x, picture.y, picture.width, picture.height);
    demo(list, *assets, space, seconds);
    const menu::ScreensActions actions = screens.update(gather(seconds, picture.width * 0.5f / space.unit, settings_open), *assets,
                                                        space, list);
    for (const auto& [key, seat] : actions.seats) players::request_seat(key, seat_of(seat));
    if (actions.leave) coop_request_drop_out();
    if (actions.toggle_two_players || actions.toggle_odyssey_moves) {
        // As the save select's own buttons do (coop_guest.cpp file_select_frame): the setting, saved.
        RuntimeSettings settings = get_runtime_settings();
        if (actions.toggle_two_players) settings.coop = !settings.coop;
        if (actions.toggle_odyssey_moves) settings.moveset.enabled = !settings.moveset.enabled;
        set_runtime_settings(settings);
        (void)save_runtime_settings_to_file();
    }
    s_text_focus.store(actions.text_focus, std::memory_order_relaxed);
    players::set_menu_capture(screens.modal_open());
    if (actions.toggle_joinable || actions.set_join_address) {
        // As F1 > Online does: the settings, saved (Joinable takes effect in the lobby at once, and in step
        // with the game from the next start).
        RuntimeSettings settings = get_runtime_settings();
        if (actions.toggle_joinable) settings.net_joinable = !settings.net_joinable;
        if (actions.set_join_address) {
            settings.net_join_address.fill('\0');
            std::size_t i = 0;
            for (const char16_t c : actions.join_address) {
                if (i + 1u >= settings.net_join_address.size()) break;
                if (c < 0x80u) settings.net_join_address[i++] = static_cast<char>(c);
            }
        }
        set_runtime_settings(settings);
        (void)save_runtime_settings_to_file();
    }
    if (actions.stop_asking) net_lobby_cancel();
    if (actions.join) net_lobby_join(get_runtime_settings().net_join_address.data());
    if (actions.cover_pointer) menu::pointer_hand(list, *assets, space, actions.pointer_lx, actions.pointer_ly, 1.0f);
    // The game's own menu sounds, synthesised on the host (menu_sounds.h; loaded with the menus' art).
    for (const menu::MenuSound sound : actions.sounds) {
        using S = menu_sounds::Sound;
        switch (sound) {
        case menu::MenuSound::Cursor: menu_sounds::play(S::Cursor); break;
        case menu::MenuSound::Decide: menu_sounds::play(S::Decide); break;
        case menu::MenuSound::Cancel: menu_sounds::play(S::Cancel); break;
        case menu::MenuSound::WindowOpen: menu_sounds::play(S::WindowOpen); break;
        case menu::MenuSound::WindowClose: menu_sounds::play(S::WindowClose); break;
        case menu::MenuSound::MenuOpen: menu_sounds::play(S::PauseOn); break;
        case menu::MenuSound::MenuClose: menu_sounds::play(S::PauseOff); break;
        case menu::MenuSound::Yes: menu_sounds::play(S::Yes); break;
        case menu::MenuSound::No: menu_sounds::play(S::No); break;
        }
    }
    renderer.draw_menu_kit(list);
    list.clear();
}

}  // namespace galaxy::gx
