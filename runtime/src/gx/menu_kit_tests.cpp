// The menu kit (menu_kit.h): the game's animation curves and timings, the layout-to-screen mapping, hit boxes and
// the shading the renderer shares with the CPU rasteriser.

#include "galaxy/gx/menu_kit.h"
#include "galaxy/gx/menu_kit_raster.h"

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

    std::printf("%s\n", ok ? "menu kit: all passed" : "menu kit: FAILED");
    return ok ? 0 : 1;
}
