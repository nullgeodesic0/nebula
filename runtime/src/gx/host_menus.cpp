#include "galaxy/gx/host_menus.h"

#include "galaxy/gx/renderer_d3d12.h"

#include <cstdlib>
#include <string>

namespace galaxy::gx {

namespace {

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

}  // namespace

void draw_host_menus(RendererD3D12& renderer, const PictureRect& picture, double seconds) {
    // From the first frame: the assets are prepared off the render thread and arrive a moment later.
    if (!renderer.ensure_menu_kit_loaded()) return;
    static const bool wanted = !env_value("GALAXY_MENU_DEMO").empty();
    if (!wanted || picture.width <= 0.0f || picture.height <= 0.0f) return;
    const menu::Assets* assets = renderer.menu_kit_assets();
    if (assets == nullptr) return;
    static menu::DrawList list;
    list.clear();
    const menu::Space space = menu::Space::for_picture(picture.x, picture.y, picture.width, picture.height);
    demo(list, *assets, space, seconds);
    renderer.draw_menu_kit(list);
}

}  // namespace galaxy::gx
