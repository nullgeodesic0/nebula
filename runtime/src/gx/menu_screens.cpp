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

ScreensActions Screens::update(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list) {
    ScreensActions actions;
    const float frames = last_seconds_ < 0.0 ? 0.0f : static_cast<float>((in.seconds - last_seconds_) * 60.0);
    last_seconds_ = in.seconds;
    hints(in, assets, space, list, frames);
    cards(in, assets, space, list, frames);
    panel(in, assets, space, list, frames, actions);
    file_select(in, assets, space, list, frames, actions);
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
    const float height = 56.0f + static_cast<float>(rows) * kRow + (leave ? 56.0f : 0.0f) + 12.0f;
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
    }
    // The pointer (the game's own Star Pointer) hovers; the D-pad moves a focus when it is not over the panel.
    const float px = in.pointer_x * in.extent_x, py = -in.pointer_y * 228.0f;
    if (in.pointer_valid && std::fabs(px - x) <= kWidth * 0.5f + 30.0f && std::fabs(py - y) <= height * 0.5f + 40.0f) {
        actions.cover_pointer = true;
        actions.pointer_lx = px;
        actions.pointer_ly = py;
    }
    int hovered = -1;
    const bool settled = panel_anim_.settled();
    for (std::size_t i = 0; i < buttons_.size() && in.pointer_valid && settled; ++i) {
        PillStyle hit = pill_style;
        if (buttons_[i].leave) hit.hit_w = 196.0f;
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
    const float height = 24.0f + 2.0f * kRow;
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
    if (in.pointer_valid && std::fabs(px - x) <= kWidth * 0.5f + 30.0f && std::fabs(py - y) <= height * 0.5f + 40.0f) {
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
    if (hovered != option_hovered_ && hovered >= 0) actions.sounds.push_back(MenuSound::Cursor);
    option_hovered_ = hovered;
    if (hovered >= 0 && (in.pressed & kA) != 0u) {
        option_anims_[static_cast<std::size_t>(hovered)].decide();
        (hovered == 0 ? actions.toggle_two_players : actions.toggle_odyssey_moves) = true;
        actions.sounds.push_back(MenuSound::Decide);
    }
}

}  // namespace galaxy::gx::menu
