// The menu kit (menu_kit.h): the game's animation curves and timings, the layout-to-screen mapping, hit boxes and
// the shading the renderer shares with the CPU rasteriser.

#include "galaxy/gx/menu_kit.h"
#include "galaxy/gx/menu_kit_raster.h"
#include "galaxy/gx/menu_screens.h"

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}
bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
    using namespace galaxy::gx::menu;
    bool ok = true;

    // Hermite keys: ends, an eased middle, a repeated frame as a step.
    ok &= expect(near(curve({{0, 0, 0}, {20, 1, 0}}, 10), 0.5f) && near(curve({{0, 0, 0}, {20, 1, 0}}, 5), 0.15625f),
                 "flat slopes give a smoothstep");
    ok &= expect(near(curve({{0, 5, 0}, {4, 9, 0}}, -1), 5) && near(curve({{0, 5, 0}, {4, 9, 0}}, 9), 9),
                 "before and after the keys hold the ends");
    ok &= expect(near(curve({{0, 80, -10}, {6, 20, -10}, {6, 20, 0}}, 6), 20), "a repeated frame steps");

    // The hover: 1 -> 1.15 in 6 frames, the gradient flips in 7, the out curve overshoots to 1.05 at frame 8.
    ButtonAnim button;
    button.hover(true);
    button.step(6.0f);
    ok &= expect(near(button.look().scale, 1.15f) && near(button.look().text_grey, 20.0f), "hovered at frame 6");
    button.step(3.0f);
    ok &= expect(button.phase == ButtonAnim::Phase::Hovered && near(button.look().gradient, 1.0f), "settled hovered");
    button.hover(false);
    button.step(8.0f);
    ok &= expect(near(button.look().scale, 1.05f), "the way out bounces to 1.05 at frame 8");
    button.step(4.0f);
    ok &= expect(button.phase == ButtonAnim::Phase::Idle && near(button.look().scale, 1.0f), "back to rest after 11 frames");
    // Leaving half-way in turns around from the matching point.
    button.hover(true);
    button.step(2.0f);
    button.hover(false);
    ok &= expect(button.phase == ButtonAnim::Phase::Out && near(button.frame, 4.0f), "reversing mid-way mirrors the frame");
    button.decide();
    button.step(2.0f);
    ok &= expect(near(button.look().shine, 1.0f), "the decide shine peaks at frame 2");
    button.step(20.0f);
    ok &= expect(button.decided() && near(button.look().shine, 0.0f), "and is gone by frame 14");

    // The window: 21 frames each way, sliding in from +100 and out to -100.
    WindowAnim win;
    win.open();
    ok &= expect(near(win.slide_x(), 100.0f) && near(win.alpha(), 0.0f), "opening starts 100 to the right, clear");
    win.step(10.0f);
    ok &= expect(win.slide_x() < 50.0f && win.alpha() > 0.5f, "it eases out (more than half way by frame 10)");
    win.step(11.0f);
    ok &= expect(win.phase == WindowAnim::Phase::Shown && near(win.alpha(), 1.0f), "shown after 21 frames");
    win.close();
    win.step(21.0f);
    ok &= expect(win.phase == WindowAnim::Phase::Hidden, "closed after 21 frames");

    // 608x456 layouts centred on a 16:9 picture.
    const Space space = Space::for_picture(0, 0, 854, 480);
    ok &= expect(near(space.sx(0), 427) && near(space.sy(0), 240) && near(space.sx(276) - space.sx(-276), 581.05f, 0.1f),
                 "a 552-wide window spans 581 pixels at 480 high, centred");
    ok &= expect(near(space.lx(space.sx(123)), 123) && near(space.ly(space.sy(-45)), -45), "pixels back to layout units");

    PillStyle style;
    ok &= expect(pill_hit(style, 0, 24, 160, 40) && !pill_hit(style, 0, 24, 0, 70) && !pill_hit(style, 0, 24, 190, 24),
                 "the hit box is the bnd1 box (304x64) at the pane scale");

    // Shading: the pill's material at its middle (white) and rim (cyan), with the picture gain.
    Quad q;
    q.mode = Mode::Material;
    q.c0 = rgb255(0, 160, 255, 0);
    q.c1 = rgb255(255, 255, 255);
    q.scale = 2.0f;
    const Shaded middle = shade(q, rgb255(150, 200, 255), {1, 1, 1, 1});
    const Shaded rim = shade(q, rgb255(150, 200, 255), {0, 0, 0, 1});
    ok &= expect(near(middle.r, 1) && near(middle.g, 1) && near(rim.r, 0) && near(rim.b, 1), "white middle, cyan rim");
    const Shaded bottom = shade(q, rgb255(60, 65, 70), {1, 1, 1, 1});
    ok &= expect(near(bottom.r, 120.0f / 255.0f * kPictureGain), "the gradient's dark end, doubled, with the gain");

    // The screens: a join hint that fades after 10 s, a card that comes and goes, the panel's buttons.
    {
        GlyphAtlas atlas;
        const GlyphRef start = standard_glyph(GlyphFamily::Xbox, GlyphButton::Start);
        atlas.rects[GlyphAtlas::key(start)] = {0, 0, 0.1f, 0.1f, 1.0f};
        Assets assets;
        assets.glyphs = &atlas;
        Screens screens;
        ScreensInput in;
        in.local_coop = true;
        in.in_level = true;
        in.extent_x = 405.0f;
        DeviceView pad;
        pad.key = 0x0101u;
        pad.family = GlyphFamily::Xbox;
        pad.name = u"Xbox 2";
        pad.join_candidate = true;
        pad.join_glyph = start;
        in.devices.push_back(pad);
        const auto glyphs_drawn = [](const DrawList& list) {
            int n = 0;
            for (const Quad& q : list) n += q.tex == Tex::Glyphs && q.v[0].color.a > 0.0f ? 1 : 0;
            return n;
        };
        DrawList list;
        for (double t = 0.0; t <= 1.0; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        ok &= expect(glyphs_drawn(list) == 1, "a free controller shows its Start prompt");
        for (double t = 1.0; t <= 11.0; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        ok &= expect(glyphs_drawn(list) == 0, "and fades after 10 s without input");
        in.devices[0].input = true;
        for (double t = 11.0; t <= 11.5; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        ok &= expect(glyphs_drawn(list) == 1, "touching it brings it back");

        in.devices[0].input = false;
        in.events.push_back({CardKind::Joined, 0x0101u});
        list.clear();
        in.seconds = 12.0;
        screens.update(in, assets, space, list);
        in.events.clear();
        in.second_present = true;
        for (double t = 12.0; t <= 12.5; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        ok &= expect(!list.empty(), "a join card shows");
        for (double t = 12.5; t <= 16.0; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        ok &= expect(list.empty(), "and is gone after its time (the hint is off while he plays)");

        in.pause_shown = true;
        for (double t = 16.0; t <= 17.0; t += 1.0 / 60.0) {
            list.clear();
            in.seconds = t;
            screens.update(in, assets, space, list);
        }
        const auto& buttons = screens.panel_buttons();
        ok &= expect(buttons.size() == 2u && buttons[1].leave, "the pause menu shows the panel: the pad and Leave");
        in.pointer_valid = true;
        in.pointer_x = buttons[0].x / in.extent_x;
        in.pointer_y = -buttons[0].y / 228.0f;
        in.pressed = 1u;  // A
        list.clear();
        in.seconds = 17.02;
        const ScreensActions actions = screens.update(in, assets, space, list);
        ok &= expect(actions.seats.size() == 1u && actions.seats[0].second == SeatChoice::First && actions.cover_pointer,
                     "A on a device's button seats it on the next choice, and the hand is drawn over the panel");
        in.pointer_x = buttons[1].x / in.extent_x;
        in.pointer_y = -buttons[1].y / 228.0f;
        list.clear();
        in.seconds = 17.04;
        ok &= expect(screens.update(in, assets, space, list).leave, "A on Leave asks him to leave");
    }

    ok &= expect(readable_words("RESOLUTION SCALE: 2X") == u"Resolution scale: 2X" &&
                     readable_words("A: SPACE, ENTER, MOUSELEFT") == u"A: Space, Enter, Mouseleft" &&
                     readable_words("ANTI-ALIASING: SMAA ULTRA") == u"Anti-aliasing: SMAA ultra" &&
                     readable_words("TAB/LB/RB PAGE  ESC/B CLOSE") == u"TAB/LB/RB page  ESC/B close",
                 "F1's words as the game writes its own");

    std::printf("%s\n", ok ? "menu kit: all passed" : "menu kit: FAILED");
    return ok ? 0 : 1;
}
