#include "galaxy/gx/menu_screens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace galaxy::gx::menu {

namespace {

// players::MenuInput's bits (kept apart from that header so the kit has no runtime dependencies).
constexpr std::uint32_t kA = 1u << 0, kB = 1u << 1, kUp = 1u << 2, kDown = 1u << 3, kLeft = 1u << 4, kRight = 1u << 5;

constexpr float kHintSeconds = 10.0f;
constexpr double kCardSeconds = 2.5;
constexpr float kFadeFrames = 20.0f;
// The Players panel's Online button, among its device buttons.
constexpr std::uint32_t kOnlineButton = 0xFFFFFFF0u;

std::u16string brother(bool luigi) { return luigi ? u"Luigi" : u"Mario"; }

// The dialogs' body text (SysInfoWindow TxtConfirm).
TextStyle body_text(float size = 1.0f) {
    TextStyle style;
    style.font_w = 22.95f * size;
    style.font_h = 29.7f * size;
    style.char_space = 1.0f;
    style.line_space = -1.0f;
    style.color = rgb255(80, 90, 130);
    return style;
}

// White with the HUD's dark shadow, over the game.
void hud_text(DrawList& list, const Assets& assets, const Space& space, float x, float y, std::u16string_view str,
              float alpha, TextStyle::Align align) {
    TextStyle style;
    style.font_w = 22.0f;
    style.font_h = 27.0f;
    style.char_space = 1.0f;
    style.align = align;
    style.color = rgb255(0, 0, 0, 150);
    text(list, assets, space, style, x + 2.0f, y - 2.0f, str, alpha);
    style.color = rgb255(255, 255, 255);
    text(list, assets, space, style, x, y, str, alpha);
}

// The panel's small buttons: the dialogs' pill, smaller.
PillStyle panel_pill() {
    PillStyle style = dialog_pill_style();
    style.width = 86.0f;
    style.height = 40.0f;
    style.cap = 20.0f;
    style.pane_scale = 1.0f;
    style.font_w = 19.0f;
    style.font_h = 24.0f;
    style.char_space = 1.0f;
    style.label_shadow_y = -3.0f;
    style.hit_w = 126.0f;
    style.hit_h = 40.0f;
    style.hit_scale = 1.0f;
    return style;
}

SeatChoice next_seat(SeatChoice seat, int step) {
    const int value = (static_cast<int>(seat) + step + 4) % 4;
    return static_cast<SeatChoice>(value);
}

}  // namespace

std::u16string seat_label(SeatChoice seat, bool driving, bool second, bool second_is_luigi) {
    const std::u16string first_name = brother(!second_is_luigi);
    const std::u16string second_name = brother(second_is_luigi);
    switch (seat) {
    case SeatChoice::First: return first_name;
    case SeatChoice::Second: return second_name;
    case SeatChoice::Off: return u"Off";
    case SeatChoice::Auto: break;
    }
    return second ? second_name : driving ? first_name : u"Auto";
}

ScreensActions Screens::update(const ScreensInput& raw, const Assets& assets, const Space& space, DrawList& list) {
    ScreensActions actions;
    const float frames = last_seconds_ < 0.0 ? 0.0f : static_cast<float>((raw.seconds - last_seconds_) * 60.0);
    last_seconds_ = raw.seconds;
    if (raw.pointer_valid &&
        std::fabs(raw.pointer_x - pointer_x_) + std::fabs(raw.pointer_y - pointer_y_) > 0.004f) {
        pointer_moved_ = raw.seconds;
    }
    pointer_x_ = raw.pointer_x;
    pointer_y_ = raw.pointer_y;
    ScreensInput in = raw;
    in.pointer_shown = raw.pointer_valid;
    if (raw.seconds - pointer_moved_ > 2.0) in.pointer_valid = false;
    hints(in, assets, space, list, frames);
    cards(in, assets, space, list, frames);
    online_cards(in, assets, space, list, frames);
    // The Online window is modal: what is under it sees no input.
    ScreensInput under = in;
    if (online_open_) {
        under.pressed = 0u;
        under.pointer_valid = false;
    }
    panel(under, assets, space, list, frames, actions);
    file_select(under, assets, space, list, frames, actions);
    online_window(in, assets, space, list, frames, actions);
    online_menu(in, assets, space, list, frames);
    return actions;
}

void Screens::hints(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames) {
    const bool showing = in.local_coop && in.in_level && !in.pause_shown && !in.second_present;
    float y = -200.0f;
    for (const DeviceView& device : in.devices) {
        auto [it, fresh] = last_input_.try_emplace(device.key, in.seconds);
        if (device.input) it->second = in.seconds;
        const bool want = showing && device.join_candidate && in.seconds - it->second < kHintSeconds;
        float& alpha = hint_alpha_[device.key];
        alpha = std::clamp(alpha + (want ? 1.0f : -1.0f) * frames / kFadeFrames, 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;
        // "[Start] Join as Luigi", centred at the bottom; one line per waiting controller, upwards.
        const std::u16string words = u"Join as " + brother(in.second_is_luigi);
        TextStyle measure;
        measure.font_w = 22.0f;
        measure.font_h = 27.0f;
        measure.char_space = 1.0f;
        const float glyph_h = 30.0f;
        const GlyphRect* rect = assets.glyphs ? assets.glyphs->find(device.join_glyph) : nullptr;
        const float glyph_w = rect ? glyph_h * rect->aspect : 0.0f;
        const float width = glyph_w + 8.0f + text_width(assets, measure, words);
        const float left = -width * 0.5f;
        glyph(list, assets, space, device.join_glyph, left + glyph_w * 0.5f, y, glyph_h, alpha);
        hud_text(list, assets, space, left + glyph_w + 8.0f, y, words, alpha, TextStyle::Align::Left);
        y += 36.0f;
    }
}

void Screens::cards(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames) {
    for (const CardEvent& event : in.events) {
        std::u16string device;
        for (const DeviceView& d : in.devices) {
            if (d.key == event.device) device = d.name;
        }
        // A newer card takes over at once; the one showing leaves.
        for (Card& card : cards_) card.until = std::min(card.until, in.seconds);
        Card card{event.kind, device, in.seconds + kCardSeconds, {}};
        card.anim.open();
        cards_.push_back(std::move(card));
    }
    float y = 176.0f;
    for (Card& card : cards_) {
        // The disconnect card stays while he waits for a controller.
        const bool hold = card.kind == CardKind::Disconnected && in.second_waiting && &card == &cards_.back();
        if (!hold && in.seconds >= card.until) card.anim.close();
        card.anim.step(frames);
        if (!card.anim.visible()) continue;
        const std::u16string name = brother(in.second_is_luigi);
        std::u16string line;
        switch (card.kind) {
        case CardKind::Joined: line = name + u" joins!"; break;
        case CardKind::Reconnected: line = name + u" is back!"; break;
        case CardKind::Left: line = name + u" left."; break;
        case CardKind::Disconnected: line = name + u"'s controller disconnected."; break;
        }
        std::u16string second_line;
        if (card.kind == CardKind::Joined && !card.device.empty()) second_line = card.device;
        if (card.kind == CardKind::Disconnected && in.second_waiting) {
            char seconds[8];
            std::snprintf(seconds, sizeof(seconds), "%d", static_cast<int>(std::ceil(in.grace_left)));
            second_line = u"Press Start to rejoin (";
            for (const char* c = seconds; *c != '\0'; ++c) second_line.push_back(static_cast<char16_t>(*c));
            second_line += u")";
        }
        const TextStyle style = body_text(0.9f);
        TextStyle small = body_text(0.75f);
        const float width = std::max(text_width(assets, style, line), text_width(assets, small, second_line)) + 64.0f;
        const float height = second_line.empty() ? 64.0f : 92.0f;
        const float alpha = card.anim.alpha();
        const float x = card.anim.slide_x();
        window(list, assets, space, x, y, width, height, alpha);
        if (second_line.empty()) {
            text(list, assets, space, style, x, y, line, alpha);
        } else {
            text(list, assets, space, style, x, y + 14.0f, line, alpha);
            text(list, assets, space, small, x, y - 20.0f, second_line, alpha);
        }
    }
    std::erase_if(cards_, [](const Card& c) { return !c.anim.visible(); });
}

void Screens::panel(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames,
                    ScreensActions& actions) {
    // The game stops the stage while paused, so the pause menu alone says a stage is on.
    const bool want = in.local_coop && in.pause_shown;
    if (want) {
        panel_anim_.open();
    } else {
        panel_anim_.close();
    }
    panel_anim_.step(frames);
    buttons_.clear();
    if (!panel_anim_.visible()) {
        focus_ = hovered_ = -1;
        return;
    }
    const float alpha = panel_anim_.alpha();
    const PillStyle pill_style = panel_pill();
    // Right of the pause menu's buttons, which reach 171 units out: 216 wide fits a 16:9 picture (405 units each
    // side). Each row: the controller's picture, its name above the button with the brother it plays.
    constexpr float kWidth = 216.0f, kRow = 72.0f;
    const std::size_t rows = in.devices.size();
    const bool leave = in.second_present;
    const bool online = in.online.phase != OnlinePhase::Unavailable;
    const float height = 56.0f + static_cast<float>(rows) * kRow + (leave ? 56.0f : 0.0f) + (online ? 56.0f : 0.0f) + 12.0f;
    const float x = in.extent_x - 10.0f - kWidth * 0.5f + panel_anim_.slide_x() * 0.4f;
    const float top = std::min(height * 0.5f, 150.0f);
    const float y = top - height * 0.5f;
    const float left = x - kWidth * 0.5f;
    window(list, assets, space, x, y, kWidth, height, alpha);
    text(list, assets, space, body_text(), x, top - 30.0f, u"Players", alpha);

    float row_y = top - 56.0f - kRow * 0.5f;
    for (const DeviceView& device : in.devices) {
        const float column = device.keyboard ? x : left + 128.0f;
        if (!device.keyboard) {
            glyph(list, assets, space, standard_glyph(device.family, GlyphButton::Controller), left + 28.0f, row_y, 38.0f,
                  alpha);
        }
        text(list, assets, space, body_text(0.62f), column, row_y + 17.0f, device.name, alpha);
        buttons_.push_back({column, row_y - 13.0f, device.key, false});
        row_y -= kRow;
    }
    if (leave) {
        buttons_.push_back({x, row_y + kRow * 0.5f - 30.0f, 0u, true});
        row_y -= 56.0f;
    }
    if (online) {
        buttons_.push_back({x, row_y + kRow * 0.5f - 30.0f, kOnlineButton, false});
    }
    // The pointer (the game's own Star Pointer) hovers; the D-pad moves a focus when it is not over the panel.
    const float px = in.pointer_x * in.extent_x, py = -in.pointer_y * 228.0f;
    if (in.pointer_shown && std::fabs(px - x) <= kWidth * 0.5f + 30.0f && std::fabs(py - y) <= height * 0.5f + 40.0f) {
        actions.cover_pointer = true;
        actions.pointer_lx = px;
        actions.pointer_ly = py;
    }
    int hovered = -1;
    const bool settled = panel_anim_.settled();
    for (std::size_t i = 0; i < buttons_.size() && in.pointer_valid && settled; ++i) {
        PillStyle hit = pill_style;
        if (buttons_[i].leave || buttons_[i].device == kOnlineButton) hit.hit_w = 196.0f;
        if (pill_hit(hit, buttons_[i].x, buttons_[i].y, px, py)) hovered = static_cast<int>(i);
    }
    if (hovered >= 0) focus_ = -1;
    const int count = static_cast<int>(buttons_.size());
    if (settled && count > 0 && (in.pressed & (kUp | kDown)) != 0u) {
        const int from = focus_ >= 0 ? focus_ : (hovered >= 0 ? hovered : -1);
        focus_ = from < 0 ? 0 : std::clamp(from + ((in.pressed & kDown) != 0u ? 1 : -1), 0, count - 1);
    }
    if (focus_ >= count) focus_ = count - 1;
    const int active = focus_ >= 0 ? focus_ : hovered;
    if (active != hovered_ && active >= 0) actions.sounds.push_back(MenuSound::Cursor);
    hovered_ = active;

    // A on the pointer's button, or on the focus unless the pointer is on the game's own buttons (A is theirs then).
    const bool over_game_buttons = in.pointer_valid && std::fabs(px) <= 182.0f && py <= 66.0f && py >= -116.0f;
    const bool may_choose = active >= 0 && (focus_ < 0 || !over_game_buttons);
    int step = 0;
    if (may_choose && (in.pressed & kA) != 0u) step = 1;
    if (active >= 0 && focus_ >= 0 && (in.pressed & kRight) != 0u) step = 1;
    if (active >= 0 && focus_ >= 0 && (in.pressed & kLeft) != 0u) step = -1;
    if (step != 0) {
        const Button& button = buttons_[static_cast<std::size_t>(active)];
        button_anims_[button.device].decide();
        if (button.leave) {
            actions.leave = true;
            actions.sounds.push_back(MenuSound::Cancel);
        } else if (button.device == kOnlineButton) {
            if (step == 1 && (in.pressed & kA) != 0u) {
                online_open_ = true;
                actions.sounds.push_back(MenuSound::Decide);
            }
        } else {
            for (const DeviceView& device : in.devices) {
                if (device.key == button.device) actions.seats.emplace_back(device.key, next_seat(device.seat, step));
            }
            actions.sounds.push_back(MenuSound::Decide);
        }
    }

    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const Button& button = buttons_[i];
        ButtonAnim& anim = button_anims_[button.device];
        if (anim.decided()) anim.phase = ButtonAnim::Phase::Hovered;
        anim.hover(static_cast<int>(i) == active);
        anim.step(frames);
        std::u16string label;
        PillStyle style = pill_style;
        if (button.leave) {
            label = brother(in.second_is_luigi) + u" leaves";
            style.width = 150.0f;
            style.hit_w = 196.0f;
        } else if (button.device == kOnlineButton) {
            label = u"Online";
            style.width = 150.0f;
            style.hit_w = 196.0f;
        } else {
            for (const DeviceView& device : in.devices) {
                if (device.key == button.device) {
                    label = seat_label(device.seat, device.driving, device.second, in.second_is_luigi);
                }
            }
        }
        pill(list, assets, space, style, button.x, button.y, label, anim.look(kDialogHover), alpha);
    }
}

void Screens::file_select(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list,
                          float frames, ScreensActions& actions) {
    if (in.file_select) {
        options_anim_.open();
    } else {
        options_anim_.close();
    }
    options_anim_.step(frames);
    if (!options_anim_.visible()) {
        option_hovered_ = -1;
        return;
    }
    const float alpha = options_anim_.alpha();
    // Bottom right, in the game's info window: a row per option, its button, its name and a switch.
    constexpr float kWidth = 300.0f, kRow = 50.0f;
    const bool online = in.online.phase != OnlinePhase::Unavailable;
    const float height = 24.0f + (online ? 3.0f : 2.0f) * kRow;
    const float x = in.extent_x - 12.0f - kWidth * 0.5f + options_anim_.slide_x() * 0.4f;
    const float y = -228.0f + 14.0f + height * 0.5f;
    const float left = x - kWidth * 0.5f;
    window(list, assets, space, x, y, kWidth, height, alpha);
    PillStyle style = panel_pill();
    style.width = 56.0f;
    style.cap = 18.0f;
    style.hit_w = 92.0f;
    const struct {
        const char16_t* name;
        bool on;
        GlyphRef glyph;
    } rows[2] = {{u"Two players", in.two_players, in.two_players_glyph},
                 {u"Odyssey moves", in.odyssey_moves, in.odyssey_moves_glyph}};
    const float px = in.pointer_x * in.extent_x, py = -in.pointer_y * 228.0f;
    if (in.pointer_shown && std::fabs(px - x) <= kWidth * 0.5f + 30.0f && std::fabs(py - y) <= height * 0.5f + 40.0f) {
        actions.cover_pointer = true;
        actions.pointer_lx = px;
        actions.pointer_ly = py;
    }
    int hovered = -1;
    float row_y = y + height * 0.5f - 12.0f - kRow * 0.5f;
    for (int i = 0; i < 2; ++i) {
        const float pill_x = left + kWidth - 14.0f - 46.0f;
        glyph(list, assets, space, rows[i].glyph, left + 30.0f, row_y, 30.0f, alpha);
        TextStyle name = body_text(0.75f);
        name.align = TextStyle::Align::Left;
        text(list, assets, space, name, left + 54.0f, row_y, rows[i].name, alpha);
        if (in.pointer_valid && options_anim_.settled() && pill_hit(style, pill_x, row_y, px, py)) hovered = i;
        ButtonAnim& anim = option_anims_[static_cast<std::size_t>(i)];
        if (anim.decided()) anim.phase = ButtonAnim::Phase::Hovered;
        anim.hover(hovered == i);
        anim.step(frames);
        pill(list, assets, space, style, pill_x, row_y, rows[i].on ? u"On" : u"Off", anim.look(kDialogHover), alpha);
        row_y -= kRow;
    }
    if (online) {
        PillStyle wide = style;
        wide.width = 150.0f;
        wide.hit_w = 186.0f;
        if (in.pointer_valid && options_anim_.settled() && pill_hit(wide, x, row_y, px, py)) hovered = 2;
        ButtonAnim& anim = option_anims_[2];
        if (anim.decided()) anim.phase = ButtonAnim::Phase::Hovered;
        anim.hover(hovered == 2);
        anim.step(frames);
        pill(list, assets, space, wide, x, row_y, u"Online", anim.look(kDialogHover), alpha);
    }
    if (hovered != option_hovered_ && hovered >= 0) actions.sounds.push_back(MenuSound::Cursor);
    option_hovered_ = hovered;
    if (hovered >= 0 && (in.pressed & kA) != 0u) {
        option_anims_[static_cast<std::size_t>(hovered)].decide();
        if (hovered == 2) {
            online_open_ = true;
        } else {
            (hovered == 0 ? actions.toggle_two_players : actions.toggle_odyssey_moves) = true;
        }
        actions.sounds.push_back(MenuSound::Decide);
    }
}

namespace {

// The lobby's upper-case words as a sentence: "THE HOST SAID NO" -> "The host said no."
std::u16string sentence(std::u16string_view words) {
    std::u16string out;
    bool first = true;
    for (const char16_t c : words) {
        char16_t d = c;
        if (c >= u'A' && c <= u'Z' && !first) d = static_cast<char16_t>(c - u'A' + u'a');
        if (c != u' ') first = false;
        out.push_back(d);
    }
    if (!out.empty() && out.back() != u'.' && out.back() != u'!' && out.back() != u'?') out.push_back(u'.');
    return out;
}

}  // namespace

void Screens::online_window(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list,
                            float frames, ScreensActions& actions) {
    const OnlineView& o = in.online;
    if (o.phase == OnlinePhase::Unavailable || o.phase == OnlinePhase::InSession || o.menu_open ||
        (!in.pause_shown && !in.file_select)) {
        online_open_ = false;
    }
    if (online_open_) {
        online_anim_.open();
    } else {
        online_anim_.close();
        typing_ = false;
    }
    online_anim_.step(frames);
    if (!online_anim_.visible()) {
        online_hovered_ = online_focus_ = -1;
        return;
    }
    const float alpha = online_anim_.alpha();
    const float x = online_anim_.slide_x();
    constexpr float kWidth = 440.0f, kHeight = 300.0f;
    window(list, assets, space, x, 0.0f, kWidth, kHeight, alpha);
    text(list, assets, space, body_text(), x, 112.0f, u"Online", alpha);

    // Joinable: the switch and what it means now.
    TextStyle label = body_text(0.8f);
    label.align = TextStyle::Align::Left;
    text(list, assets, space, label, x - 190.0f, 64.0f, u"Joinable", alpha);
    std::u16string note;
    switch (o.phase) {
    case OnlinePhase::Joinable: note = u"Friends can join at " + o.address; break;
    default: note = o.joinable ? u"" : u"No one can join this game"; break;
    }
    if (o.phase == OnlinePhase::PortInUse) {
        note = u"Port ";
        for (const char c : std::to_string(o.port)) note.push_back(static_cast<char16_t>(c));
        note += u" is in use";
    }
    if (o.joinable && !o.joinable_live && o.phase == OnlinePhase::Joinable) note = u"Joining restarts both games until the next start";
    TextStyle small = body_text(0.6f);
    small.align = TextStyle::Align::Left;
    text(list, assets, space, small, x - 190.0f, 34.0f, note, alpha);

    // Join a friend: the address box and Join (Stop while asking).
    const bool asking = o.phase == OnlinePhase::Asking || o.phase == OnlinePhase::Waiting;
    text(list, assets, space, label, x - 190.0f, -12.0f, u"Join a friend", alpha);

    struct Item {
        float x, y, width;
        std::u16string label;
    };
    PillStyle style = panel_pill();
    const std::u16string shown_address = typing_ ? typed_ : o.join_address;
    std::u16string box = asking ? (o.phase == OnlinePhase::Asking ? u"Asking " + o.address + u"..."
                                                                   : o.address + u" is choosing...")
                                : (shown_address.empty() && !typing_ ? u"Type an address" : shown_address);
    if (typing_ && static_cast<long long>(in.seconds * 2.0) % 2 == 0) box += u"|";
    const Item items[4] = {
        {x + 150.0f, 64.0f, 70.0f, o.joinable ? u"On" : u"Off"},
        {x - 46.0f, -54.0f, 230.0f, box},
        {x + 150.0f, -54.0f, 70.0f, asking ? u"Stop" : u"Join"},
        {x, -112.0f, 110.0f, u"Back"},
    };

    const float px = in.pointer_x * in.extent_x, py = -in.pointer_y * 228.0f;
    if (in.pointer_shown && std::fabs(px - x) <= kWidth * 0.5f + 30.0f && std::fabs(py) <= kHeight * 0.5f + 40.0f) {
        actions.cover_pointer = true;
        actions.pointer_lx = px;
        actions.pointer_ly = py;
    }
    int hovered = -1;
    for (int i = 0; i < 4 && in.pointer_valid && online_anim_.settled(); ++i) {
        PillStyle hit = style;
        hit.hit_w = items[i].width + 40.0f;
        if (pill_hit(hit, items[i].x, items[i].y, px, py)) hovered = i;
    }
    if (hovered >= 0) online_focus_ = -1;
    if (online_anim_.settled() && !typing_ && (in.pressed & (kUp | kDown | kLeft | kRight)) != 0u) {
        const int from = online_focus_ >= 0 ? online_focus_ : (hovered >= 0 ? hovered : -1);
        int to = from < 0 ? 0 : from;
        if (from >= 0) {
            if ((in.pressed & kDown) != 0u) to = from == 0 ? 1 : 3;
            if ((in.pressed & kUp) != 0u) to = from == 3 ? 1 : 0;
            if ((in.pressed & kRight) != 0u && from == 1) to = 2;
            if ((in.pressed & kLeft) != 0u && from == 2) to = 1;
        }
        online_focus_ = to;
    }
    const int active = online_focus_ >= 0 ? online_focus_ : hovered;
    if (active != online_hovered_ && active >= 0) actions.sounds.push_back(MenuSound::Cursor);
    online_hovered_ = active;

    // Typing: the keys edit the box; Enter asks, Escape stops typing.
    if (typing_) {
        for (const char32_t key : in.keys) {
            if (key == U'\b') {
                if (!typed_.empty()) typed_.pop_back();
            } else if (key == U'\r') {
                typing_ = false;
                actions.join = !typed_.empty();
            } else if (key == U'\x1B') {
                typing_ = false;
            } else if (key < 0x7Fu && typed_.size() < 60u) {
                if (typed_fresh_) typed_.clear();
                typed_.push_back(static_cast<char16_t>(key));
            }
            typed_fresh_ = false;
            actions.set_join_address = true;
            actions.join_address = typed_;
        }
    }
    if (online_anim_.settled() && (in.pressed & kA) != 0u && active >= 0 && !typing_) {
        online_anims_[static_cast<std::size_t>(active)].decide();
        actions.sounds.push_back(MenuSound::Decide);
        switch (active) {
        case 0: actions.toggle_joinable = true; break;
        case 1:
            if (!asking) {
                typing_ = true;
                typed_fresh_ = true;
                typed_ = o.join_address;
            }
            break;
        case 2:
            if (asking) {
                actions.stop_asking = true;
            } else {
                actions.set_join_address = typing_;
                actions.join_address = typed_;
                actions.join = true;
                typing_ = false;
            }
            break;
        case 3: online_open_ = false; break;
        }
    }
    if (online_anim_.settled() && (in.pressed & kB) != 0u && !typing_) {
        online_open_ = false;
        actions.sounds.push_back(MenuSound::Cancel);
    }
    actions.text_focus = typing_;

    for (int i = 0; i < 4; ++i) {
        ButtonAnim& anim = online_anims_[static_cast<std::size_t>(i)];
        if (anim.decided()) anim.phase = ButtonAnim::Phase::Hovered;
        anim.hover(i == active || (i == 1 && typing_));
        anim.step(frames);
        PillStyle item = style;
        item.width = items[i].width;
        item.hit_w = items[i].width + 40.0f;
        if (i == 1) item.font_w = item.font_h * 0.7f;  // addresses are long
        pill(list, assets, space, item, items[i].x, items[i].y, items[i].label, anim.look(kDialogHover), alpha);
    }
}

void Screens::online_menu(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list,
                          float frames) {
    const OnlineView& o = in.online;
    if (o.menu_open) {
        menu_anim_.open();
    } else {
        menu_anim_.close();
    }
    menu_anim_.step(frames);
    if (!menu_anim_.visible()) return;
    const float alpha = menu_anim_.alpha();
    const float x = menu_anim_.slide_x();
    // This PC's own menu while the game plays on (no pause online), or the answer to a request.
    const std::u16string session_rows[3] = {u"Resume", u"Exit the galaxy", u"Leave"};
    const std::u16string answer_rows[2] = {u"Accept", u"Not now"};
    const int rows = o.menu_answering ? 2 : 3;
    const float height = 110.0f + static_cast<float>(rows) * 84.0f;
    window(list, assets, space, x, 0.0f, 400.0f, height, alpha);
    const std::u16string title = o.menu_answering ? o.asker + u" wants to join" : u"Online";
    text(list, assets, space, body_text(), x, height * 0.5f - 44.0f, title, alpha);
    PillStyle style = dialog_pill_style();
    style.width = 170.0f;
    style.hit_w = 230.0f;
    float y = height * 0.5f - 104.0f;
    for (int i = 0; i < rows; ++i) {
        ButtonAnim& anim = menu_anims_[static_cast<std::size_t>(i)];
        anim.hover(i == o.menu_row);
        anim.step(frames);
        pill(list, assets, space, style, x, y, o.menu_answering ? answer_rows[i] : session_rows[i],
             anim.look(kDialogHover), alpha);
        y -= 84.0f;
    }
}

void Screens::online_cards(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list,
                           float frames) {
    const OnlineView& o = in.online;
    const std::u16string name = brother(in.second_is_luigi);
    // What the lobby card says now: a request (with +), asking, or an outcome for a few seconds.
    OnlinePhase show = OnlinePhase::Unavailable;
    std::u16string words[2];
    if (o.phase == OnlinePhase::AskedToJoin && !o.menu_open) {
        show = o.phase;
        words[0] = o.asker + u" wants to join as " + name + u".";
        words[1] = u"Answer";
    } else if ((o.phase == OnlinePhase::Asking || o.phase == OnlinePhase::Waiting) && !online_open_) {
        show = o.phase;
        words[0] = o.phase == OnlinePhase::Asking ? u"Asking " + o.address + u"..." : o.address + u" is choosing...";
    } else if (o.phase == OnlinePhase::Outcome && (last_phase_ != OnlinePhase::Outcome || o.outcome != last_outcome_)) {
        lobby_card_until_ = in.seconds + 4.0;
    }
    if (o.phase == OnlinePhase::Outcome && in.seconds < lobby_card_until_ && !online_open_) {
        show = OnlinePhase::Outcome;
        words[0] = sentence(o.outcome);
    }
    last_phase_ = o.phase;
    last_outcome_ = o.outcome;
    if (show != OnlinePhase::Unavailable) {
        lobby_card_phase_ = show;
        lobby_card_words_[0] = words[0];
        lobby_card_words_[1] = words[1];
        lobby_card_anim_.open();
    } else {
        lobby_card_anim_.close();
    }
    lobby_card_anim_.step(frames);
    if (!lobby_card_anim_.visible()) return;
    const float alpha = lobby_card_anim_.alpha();
    const float x = lobby_card_anim_.slide_x();
    const float y = 110.0f;
    const TextStyle style = body_text(0.9f);
    const bool prompt = lobby_card_phase_ == OnlinePhase::AskedToJoin;
    const float width = text_width(assets, style, lobby_card_words_[0]) + 64.0f;
    const float height = prompt ? 96.0f : 64.0f;
    window(list, assets, space, x, y, width, height, alpha);
    if (!prompt) {
        text(list, assets, space, style, x, y, lobby_card_words_[0], alpha);
        return;
    }
    text(list, assets, space, style, x, y + 16.0f, lobby_card_words_[0], alpha);
    // "[+] Answer": this PC's + opens the answer (the online menu).
    TextStyle small = body_text(0.75f);
    small.align = TextStyle::Align::Left;
    const float words_w = text_width(assets, small, lobby_card_words_[1]);
    const float glyph_h = 28.0f;
    const GlyphRect* rect = assets.glyphs ? assets.glyphs->find(o.plus_glyph) : nullptr;
    const float glyph_w = rect ? glyph_h * rect->aspect : 0.0f;
    const float left = x - (glyph_w + 8.0f + words_w) * 0.5f;
    glyph(list, assets, space, o.plus_glyph, left + glyph_w * 0.5f, y - 20.0f, glyph_h, alpha);
    text(list, assets, space, small, left + glyph_w + 8.0f, y - 20.0f, lobby_card_words_[1], alpha);
}

}  // namespace galaxy::gx::menu