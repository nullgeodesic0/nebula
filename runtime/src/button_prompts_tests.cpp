// Button prompts: which icon each binding gets, the redrawn cells, and the
// in-memory patch of the picture font (applied and restored).
//
// `galaxy_button_prompts_tests <out.bmp>` also writes every prompt on a grey
// sheet (Xbox row, DualSense row) for a look.

#include "galaxy/button_prompts.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace galaxy;
using namespace galaxy::prompts;
using input::ControlScheme;
using input::WiiTarget;

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

void write_bmp(const char* path, const std::vector<std::uint8_t>& rgba, int w, int h);

void defaults() {
    const input::BindingTable xbox = input::default_binding_table(ControlScheme::Xbox);
    const PromptSet x = make_prompt_set(Family::Xbox, xbox);
    const auto at = [](const PromptSet& s, Slot slot) { return s.labels[static_cast<std::size_t>(slot)]; };
    check(at(x, Slot::A) == Label::South, "xbox A is the south face button");
    check(at(x, Slot::B) == Label::East, "xbox B follows jump/back");
    check(at(x, Slot::C) == Label::L3, "xbox C (camera reset) is the left stick click");
    check(at(x, Slot::Z) == Label::L2, "xbox Z is the left trigger");
    check(at(x, Slot::Plus) == Label::Start, "xbox + is Menu");
    check(at(x, Slot::Minus) == Label::Back, "xbox - is View");
    check(at(x, Slot::One) == Label::West, "xbox 1 is X, the moveset's cap button");
    check(at(x, Slot::Two) == Label::Digit2, "unbound 2 draws its digit");
    check(at(x, Slot::Home) == Label::Home, "unbound Home draws the home icon");
    check(at(x, Slot::Nunchuk) == Label::LeftStick, "nunchuk is the left stick");
    check(at(x, Slot::Dpad) == Label::Dpad && at(x, Slot::Remote) == Label::Controller, "dpad and controller");

    const PromptSet d = make_prompt_set(Family::DualSense, input::default_binding_table(ControlScheme::PlayStation));
    check(at(d, Slot::A) == Label::South && at(d, Slot::B) == Label::East, "dualsense faces");
    check(std::strcmp(label_name(Family::DualSense, Label::South), "Cross") == 0, "dualsense south is Cross");
    check(std::strcmp(label_name(Family::Xbox, Label::Start), "Menu") == 0, "xbox start is Menu");

    // Remapped: Wii A on the north button, Z on the right bumper, B (and no jump/back) on RT.
    input::BindingTable custom = xbox;
    custom[WiiTarget::A] = {};
    input::add_binding(custom[WiiTarget::A], {input::SourceKind::Button, input::pad::kNorthBit});
    custom[WiiTarget::Z] = {};
    input::add_binding(custom[WiiTarget::Z], {input::SourceKind::Key, 'Z'});
    input::add_binding(custom[WiiTarget::Z], {input::SourceKind::Button, input::pad::kRightShoulderBit});
    custom[WiiTarget::JumpBack] = {};
    custom[WiiTarget::B] = {};
    input::add_binding(custom[WiiTarget::B], {input::SourceKind::AxisPositive,
                                              static_cast<std::uint8_t>(input::PadAxis::RightTrigger)});
    const PromptSet c = make_prompt_set(Family::Xbox, custom);
    check(at(c, Slot::A) == Label::North, "remapped A shows its button");
    check(at(c, Slot::Z) == Label::R1, "a key before the pad button is skipped");
    check(at(c, Slot::B) == Label::R2, "B falls back to Wii B's own binding");
    check(!family_for_scheme(ControlScheme::Keyboard) && !family_for_scheme(ControlScheme::SwitchPro),
          "only Xbox and DualSense get prompts");
}

void rendering(const char* bmp_path) {
    std::vector<std::uint8_t> rgba(kCellW * kCellH * 4);
    for (Family family : {Family::Xbox, Family::DualSense}) {
        for (int l = 1; l <= static_cast<int>(Label::Digit2); ++l) {
            std::fill(rgba.begin(), rgba.end(), std::uint8_t{0});
            render_glyph(family, static_cast<Label>(l), rgba.data());
            int opaque = 0, minx = kCellW, maxx = -1, miny = kCellH, maxy = -1;
            for (int y = 0; y < kCellH; ++y) {
                for (int x = 0; x < kCellW; ++x) {
                    if (rgba[(y * kCellW + x) * 4 + 3] > 40) {
                        ++opaque;
                        minx = std::min(minx, x); maxx = std::max(maxx, x);
                        miny = std::min(miny, y); maxy = std::max(maxy, y);
                    }
                }
            }
            char what[96];
            std::snprintf(what, sizeof(what), "%s %s draws inside its cell", family == Family::Xbox ? "xbox" : "ds",
                          label_name(family, static_cast<Label>(l)));
            check(opaque > 80 && minx >= 1 && miny >= 2 && maxx <= kCellW - 2 && maxy <= kCellH - 1, what);
        }
    }
    {
        // Tutorial pictures: every frame draws, the pose changes the picture.
        int size = 0;
        std::vector<std::uint8_t> a, b;
        render_guidance(Family::Xbox, 0, 1, 4.0f, size, a);
        check(size == 128 && a.size() == 128u * 128u * 4u, "guidance picture size");
        render_guidance(Family::Xbox, 2, 3, 4.0f, size, b);
        check(a != b, "guidance poses differ");
        render_guidance(Family::DualSense, 0, 1, 4.0f, size, b);
        check(a != b, "guidance families differ");
        char path[260]{};
        std::size_t length = 0;
        if (getenv_s(&length, path, sizeof(path), "PROMPTS_GUIDE_BMP") == 0 && length > 1) {
            std::vector<std::uint8_t> sheet(static_cast<std::size_t>(128 * 8) * 256 * 4, 255);
            int column = 0;
            for (Family family : {Family::Xbox, Family::DualSense}) {
                column = 0;
                const int frames[][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {1, 7}, {1, 8}, {1, 9}, {2, 1}};
                for (const auto& f : frames) {
                    render_guidance(family, f[0], f[1], 4.0f, size, a);
                    for (int y = 0; y < 128; ++y) {
                        std::memcpy(&sheet[((static_cast<std::size_t>(y) + (family == Family::Xbox ? 0 : 128)) * 1024 +
                                            static_cast<std::size_t>(column) * 128) * 4],
                                    &a[static_cast<std::size_t>(y) * 128 * 4], 128u * 4u);
                    }
                    ++column;
                }
            }
            write_bmp(path, sheet, 1024, 256);
        }
    }
    check(encode_rgb5a3(255, 255, 255, 255) == 0xFFFF, "opaque white");
    check(encode_rgb5a3(0, 0, 0, 255) == 0x8000, "opaque black");
    check(encode_rgb5a3(10, 10, 10, 0) == 0x0FFF, "transparent");
    if (bmp_path == nullptr) return;

    constexpr int labels = static_cast<int>(Label::Digit2);
    constexpr int scale = 3, cell_w = (kCellW + 2) * scale, cell_h = (kCellH + 2) * scale;
    const int width = labels * cell_w, height = 2 * cell_h;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t v = ((x / 12 + y / 12) & 1) ? 120 : 108;
            const std::size_t i = (static_cast<std::size_t>(height - 1 - y) * width + x) * 3;
            bgr[i] = bgr[i + 1] = bgr[i + 2] = v;
        }
    }
    int row = 0;
    for (Family family : {Family::Xbox, Family::DualSense}) {
        for (int l = 1; l <= labels; ++l) {
            std::fill(rgba.begin(), rgba.end(), std::uint8_t{0});
            render_glyph(family, static_cast<Label>(l), rgba.data());
            for (int y = 0; y < kCellH * scale; ++y) {
                for (int x = 0; x < kCellW * scale; ++x) {
                    const std::uint8_t* p = &rgba[((y / scale) * kCellW + x / scale) * 4];
                    const int dx = (l - 1) * cell_w + scale + x, dy = row * cell_h + scale + y;
                    const std::size_t i = (static_cast<std::size_t>(height - 1 - dy) * width + dx) * 3;
                    for (int ch = 0; ch < 3; ++ch) {
                        const int src = p[2 - ch];
                        bgr[i + ch] = static_cast<std::uint8_t>((src * p[3] + bgr[i + ch] * (255 - p[3])) / 255);
                    }
                }
            }
        }
        ++row;
    }
    std::FILE* f = nullptr;
    if (fopen_s(&f, bmp_path, "wb") != 0 || f == nullptr) return;
    std::uint8_t header[54]{'B', 'M'};
    const std::uint32_t row_bytes = static_cast<std::uint32_t>(width) * 3u;  // width*3 padded below
    const std::uint32_t padded = (row_bytes + 3u) & ~3u;
    const std::uint32_t size = 54u + padded * static_cast<std::uint32_t>(height);
    std::memcpy(header + 2, &size, 4);
    const std::uint32_t off = 54, dib = 40;
    std::memcpy(header + 10, &off, 4);
    std::memcpy(header + 14, &dib, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    header[26] = 1;
    header[28] = 24;
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<std::uint8_t> line(padded);
    for (int y = 0; y < height; ++y) {
        std::memcpy(line.data(), &bgr[static_cast<std::size_t>(y) * width * 3], row_bytes);
        std::fwrite(line.data(), 1, padded, f);
    }
    std::fclose(f);
}

// Opaque-over-grey BMP of straight-alpha RGBA.
void write_bmp(const char* path, const std::vector<std::uint8_t>& rgba, int w, int h) {
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) return;
    const std::uint32_t row = (static_cast<std::uint32_t>(w) * 3u + 3u) & ~3u;
    const std::uint32_t size = 54u + row * static_cast<std::uint32_t>(h);
    std::uint8_t header[54]{'B', 'M'};
    std::memcpy(header + 2, &size, 4);
    const std::uint32_t off = 54, dib = 40;
    std::memcpy(header + 10, &off, 4);
    std::memcpy(header + 14, &dib, 4);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    header[26] = 1;
    header[28] = 24;
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<std::uint8_t> line(row);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            const int grey = ((x / 16 + y / 16) & 1) ? 120 : 104;
            for (int c = 0; c < 3; ++c) line[x * 3 + c] = static_cast<std::uint8_t>((p[2 - c] * p[3] + grey * (255 - p[3])) / 255);
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

struct Ram {
    std::vector<std::byte> mem1 = std::vector<std::byte>(0x01800000u);
    std::vector<std::byte> mem2 = std::vector<std::byte>(0x04000000u);
    GuestMemoryV1 memory{};
    Ram() {
        memory.fast_regions[8] = {0x01800000u, mem1.data()};
        memory.fast_regions[9] = {0x04000000u, mem2.data()};
    }
};

void font_patch() {
    constexpr std::uint32_t base = 0x80001000u;
    Ram ram;
    std::byte* font = ram.mem1.data() + (base & 0x0FFFFFFFu);
    std::memcpy(font, "RFNT", 4);
    const std::uint8_t tglp[32] = {'T', 'G', 'L', 'P', 0, 3, 0, 0x30, 0x22, 0x23, 0x1A, 0x1E, 0, 0, 0x80, 0, 0, 6, 0, 5, 0, 3,
                                   0, 3, 0, 0x80, 0, 0x80, 0, 0, 0, 0x60};
    std::memcpy(font + 48, tglp, sizeof(tglp));
    // Stand-in art: a recognisable pattern in every sheet (not the game's).
    std::byte* data = font + 0x60;
    for (std::size_t i = 0; i < 0x8000u * 6u; i += 2) {
        data[i] = static_cast<std::byte>(0x8C);
        data[i + 1] = static_cast<std::byte>(i >> 6);
    }
    std::vector<std::byte> before(data, data + 0x8000u * 6u);

    NativeServicesV1 services{};
    publish({});
    tick(&ram.memory, &services, 0);
    check(std::memcmp(before.data(), data, before.size()) == 0, "no controller: font untouched");

    const PromptSet set = make_prompt_set(Family::Xbox, input::default_binding_table(ControlScheme::Xbox));
    publish(set);
    tick(&ram.memory, &services, 0);
    check(std::memcmp(before.data(), data, before.size()) != 0, "xbox: font cells redrawn");
    // Cell of glyph 1 (the A icon): sheet 0, column 1, row 0 -> a pixel in its middle changed.
    const std::uint32_t px = 35u + 17u, py = 19u;
    const std::uint32_t off = ((py >> 2) * 32u + (px >> 2)) * 32u + ((py & 3u) * 4u + (px & 3u)) * 2u;
    check(std::memcmp(before.data() + off, data + off, 2) != 0, "the A icon's centre changed");
    // Glyph 0 (blank) and glyph 8+ (stars) are not slots: untouched.
    bool outside_changed = false;
    for (std::uint32_t y = 0; y < 36; ++y) {
        for (std::uint32_t x = 0; x < 35; ++x) {
            const std::uint32_t o = ((y >> 2) * 32u + (x >> 2)) * 32u + ((y & 3u) * 4u + (x & 3u)) * 2u;
            if (std::memcmp(before.data() + o, data + o, 2) != 0) outside_changed = true;
        }
    }
    check(!outside_changed, "the blank cell is untouched");

    // HD: the texture cache asks for sheet 0 (A, B, C, remote, nunchuk, 1, 2) and sheet 2.
    {
        HdImage hd;
        const auto* sheet0 = reinterpret_cast<const std::uint8_t*>(data);
        check(hd_replacement(base + 0x60u, 128, 128, 5, sheet0, 0x8000u, hd), "xbox: HD sheet 0");
        check(hd.width == 128u * kHdScale && hd.height == hd.width && hd.rgba.size() == hd.width * hd.height * 4u,
              "HD sheet size");
        check(hd.key.rfind("prompts_font0_", 0) == 0, "HD sheet key");
        HdImage again;
        check(hd_replacement(base + 0x60u, 128, 128, 5, sheet0, 0x8000u, again) && again.key == hd.key, "HD key is stable");
        check(!hd_replacement(base + 0x60u + 0x8000u, 128, 128, 5, sheet0, 0x8000u, again), "sheet 1 has no prompts");
        check(!hd_replacement(base + 0x60u, 64, 64, 5, sheet0, 0x2000u, again), "wrong size is not a sheet");
        char path[260]{};
        std::size_t length = 0;
        if (getenv_s(&length, path, sizeof(path), "PROMPTS_HD_BMP") == 0 && length > 1) {
            write_bmp(path, hd.rgba, static_cast<int>(hd.width), static_cast<int>(hd.height));
        }
    }

    publish({});
    tick(&ram.memory, &services, 0);
    {
        HdImage none;
        check(!hd_replacement(base + 0x60u, 128, 128, 5, reinterpret_cast<const std::uint8_t*>(data), 0x8000u, none),
              "wii remote prompts: no HD replacement");
    }
    check(std::memcmp(before.data(), data, before.size()) == 0, "back to a keyboard: the original bytes return");

    publish(make_prompt_set(Family::DualSense, input::default_binding_table(ControlScheme::PlayStation)));
    tick(&ram.memory, &services, 0);
    check(std::memcmp(before.data(), data, before.size()) != 0, "dualsense: font cells redrawn");
    publish({});
    tick(&ram.memory, &services, 0);
    check(std::memcmp(before.data(), data, before.size()) == 0, "restored again");
}

}  // namespace

int main(int argc, char** argv) {
    defaults();
    rendering(argc > 1 ? argv[1] : nullptr);
    font_patch();
    if (failures == 0) std::cout << "button prompts: ok\n";
    return failures == 0 ? 0 : 1;
}
