// Draws the menu kit's widgets over a background picture, to compare with screenshots of the game's own menus.
//   galaxy_menu_kit_preview <game.pak> <background.rgba> <width> <height> <out.ppm> <scene> [frame]
// background.rgba is raw RGBA8. scene: pause (the two pause buttons, the first hovered), dialog (the Yes/No
// window, No hovered), card (a status card), players (a Players panel sketch). `frame` steps the hover animation.

#include "galaxy/gx/font_sdf.h"
#include "galaxy/gx/game_disc.h"
#include "galaxy/gx/game_font.h"
#include "galaxy/gx/menu_kit.h"
#include "galaxy/gx/menu_kit_raster.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace galaxy::gx;
using namespace galaxy::gx::menu;

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: galaxy_menu_kit_preview <game.pak> <bg.rgba> <w> <h> <out.ppm> <scene> [frame]\n");
        return 2;
    }
    auto disc = GameDisc::open(argv[1]);
    if (!disc) return std::fprintf(stderr, "no disc\n"), 1;
    const auto font_bytes = disc->read_arc_entry("LayoutData/Font.arc", "messagefont26.brfnt");
    auto font = font_bytes ? parse_brfnt(*font_bytes) : std::nullopt;
    const auto cap_bytes = disc->read_arc_entry("LayoutData/PauseMenu.arc", "mybuttonatypeside.tpl");
    const auto corner_bytes = disc->read_arc_entry("LayoutData/SysInfoWindow.arc", "mysysinfowindow.tpl");
    auto cap_quarter = cap_bytes ? decode_tpl(*cap_bytes) : std::nullopt;
    auto corner = corner_bytes ? decode_tpl(*corner_bytes) : std::nullopt;
    if (!font || !cap_quarter || !corner) return std::fprintf(stderr, "assets missing\n"), 1;
    const DecodedTexture cap = mirror_cap_vertically(*cap_quarter);

    Image image;
    image.width = static_cast<unsigned>(std::atoi(argv[3]));
    image.height = static_cast<unsigned>(std::atoi(argv[4]));
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4u);
    std::ifstream bg(argv[2], std::ios::binary);
    bg.read(reinterpret_cast<char*>(image.rgba.data()), static_cast<std::streamsize>(image.rgba.size()));

    std::array<const DecodedTexture*, kMaxTextures> textures{};
    textures[static_cast<std::size_t>(Tex::ButtonCap)] = &cap;
    textures[static_cast<std::size_t>(Tex::WindowCorner)] = &*corner;
    // As the renderer binds them: distance fields.
    std::vector<DecodedTexture> sdf;
    for (const DecodedTexture& sheet : font->sheets) sdf.push_back(coverage_to_sdf(sheet));
    for (std::size_t i = 0; i < sdf.size() && static_cast<std::size_t>(Tex::Font) + i < kMaxTextures; ++i) {
        textures[static_cast<std::size_t>(Tex::Font) + i] = &sdf[i];
    }
    Assets assets;
    assets.font = &*font;
    assets.button_cap = true;
    assets.window_corner = true;
    assets.font_sdf = true;
    const Space space = Space::for_picture(0, 0, static_cast<float>(image.width), static_cast<float>(image.height));
    const std::string scene = argv[6];
    const float frame = argc > 7 ? static_cast<float>(std::atof(argv[7])) : 20.0f;

    ButtonAnim hovered;
    hovered.hover(true);
    hovered.step(frame);
    DrawList list;
    if (scene == "pause") {
        // NBackUp / NBackDown (pane y 0 and -97, BackUp at +12 inside): the game's two pause buttons.
        PillStyle style;
        pill(list, assets, space, style, 0.0f, 24.0f, u"Back", hovered.look(), 1.0f);
        pill(list, assets, space, style, 0.0f, -73.0f, u"Return to the\nObservatory", ButtonAnim{}.look(), 1.0f);
    } else if (scene == "dialog") {
        window(list, assets, space, 0.0f, 0.0f, 552.0f, 388.0f, 1.0f);
        TextStyle body;
        body.font_w = 22.95f;
        body.font_h = 29.7f;
        body.char_space = 1.0f;
        body.line_space = -1.0f;
        body.color = rgb255(80, 90, 130);
        text(list, assets, space, body, 0.0f, 38.0f, u"Return to the Observatory?", 1.0f);
        const PillStyle yes_no = dialog_pill_style();
        pill(list, assets, space, yes_no, -116.0f, -136.0f, u"No", hovered.look(kDialogHover), 1.0f);
        pill(list, assets, space, yes_no, 116.0f, -136.0f, u"Yes", ButtonAnim{}.look(kDialogHover), 1.0f);
    }
    rasterize(image, list, textures);

    std::ofstream out(argv[5], std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(&image.rgba[i]), 3);
    std::printf("%zu quads; font height %d width %d line feed %d ascent %d cell %dx%d baseline %d\n", list.size(),
                font->height, font->width, font->line_feed, font->ascent, font->cell_width, font->cell_height,
                font->baseline);
    return 0;
}
