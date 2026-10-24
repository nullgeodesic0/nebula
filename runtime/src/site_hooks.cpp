#include "galaxy/site_hooks.h"

#include "galaxy/button_prompts.h"
#include "galaxy/coop_guest.h"
#include "galaxy/motion_style.h"
#include "galaxy/odyssey_moveset_guest.h"
#include "galaxy/experimental_ultrawide_aspect.h"
#include "galaxy/game_menu_probe.h"
#include "galaxy/layout_aspect.h"
#include "galaxy/layout_aspect_cinema.h"
#include "galaxy/layout_aspect_fits.h"
#include "galaxy/layout_aspect_home.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace galaxy {
namespace {

// The star-select sky (MultiSceneActor named by the DOL literal at 0x805C4411)
// is one quad at z = -1000 in model space, sized for a 16:9 view. Wider views
// scale it about its center so it still covers the screen.
void fit_rmge01_scenario_sky(PpcContext* context, GuestMemoryV1* memory,
                             const NativeServicesV1* services, std::uint32_t pc) {
    if (guest_load_u32(memory, context->gpr[31] + 4u, services, pc) != 0x805C4411u) return;
    const auto policy = experimental_effective_guest_aspect(services);
    if (!policy || native_four_three(*policy)) return;
    const float k = experimental_nw4r_canvas_width(*policy) / 608.0f;
    if (!(k > 1.0f)) return;
    const std::uint32_t model = context->gpr[3];
    const auto f32 = [&](std::uint32_t address) {
        return std::bit_cast<float>(guest_load_u32(memory, address, services, pc));
    };
    const auto store = [&](std::uint32_t address, float value) {
        guest_store_u32(memory, address, std::bit_cast<std::uint32_t>(value), services, pc);
    };
    constexpr float kCenter[3] = {0.0f, 0.0f, -1000.0f};
    float offset[3];
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const float scale = f32(model + 0x18u + i * 4u);
        offset[i] = scale * (1.0f - k) * kCenter[i];
        store(model + 0x18u + i * 4u, scale * k);
    }
    // J3DModel base matrix (+0x24) is rotation and translation only.
    for (std::uint32_t row = 0; row < 3u; ++row) {
        const std::uint32_t base = model + 0x24u + row * 0x10u;
        float shift = 0.0f;
        for (std::uint32_t column = 0; column < 3u; ++column) shift += f32(base + column * 4u) * offset[column];
        store(base + 0xCu, f32(base + 0xCu) + shift);
    }
}

bool rmge01_guest_object(std::uint32_t address) noexcept {
    return ((address >= 0x80000000u && address < 0x81800000u) ||
            (address >= 0x90000000u && address < 0x94000000u)) &&
           (address & 3u) == 0u;
}

// Mario's tuning tables (motion_style.h) hold the values of the style last written; Original is what
// the game loaded. A field holding none of its known values is not the table expected and is skipped.
// Written when the style changes, and checked again every 256 calls in case the game reloaded them.
void apply_rmge01_mario_tables(GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc,
                               MotionStyle style) {
    static std::atomic<std::uint32_t> s_written{static_cast<std::uint32_t>(MotionStyle::Original)};
    static std::atomic<std::uint32_t> s_calls{0u};
    const bool recheck = (s_calls.fetch_add(1u, std::memory_order_relaxed) & 0xFFu) == 0u;
    const auto wanted = static_cast<std::uint32_t>(style);
    if (s_written.load(std::memory_order_relaxed) == wanted &&
        (style == MotionStyle::Original || !recheck)) {
        return;
    }
    for (const std::uint32_t table : kMarioConstTables) {
        for (const MarioTableField& field : kMarioTableFields) {
            const std::uint32_t address = table + field.offset;
            const float current = field.halfword
                ? static_cast<float>(guest_load_u16(memory, address, services, pc))
                : std::bit_cast<float>(guest_load_u32(memory, address, services, pc));
            float value = 0.0f;
            if (!mario_table_value(field, style, current, value) || value == current) continue;
            if (field.halfword) {
                guest_store_u16(memory, address, static_cast<std::uint16_t>(value), services, pc);
            } else {
                guest_store_u32(memory, address, std::bit_cast<std::uint32_t>(value), services, pc);
            }
        }
    }
    s_written.store(wanted, std::memory_order_relaxed);
}

// XanimePlayer (r3 at its updateInterpoleRatio calls): +0x08 the cross-fade ratio (f32), +0x20 the
// current XanimeFrameCtrl, whose +0x14 counts the cross-fade's remaining frames (s32). Layouts from
// Petari's XanimePlayer.hpp. The game's own step then runs on the values stored here. Mario's tables
// follow the style from here too: the game steps some cross-fade every frame of play.
void shape_rmge01_animation_interpole(PpcContext* context, GuestMemoryV1* memory,
                                      const NativeServicesV1* services, std::uint32_t pc) {
    const MotionStyle style = get_runtime_settings().motion_style;
    apply_rmge01_mario_tables(memory, services, pc, style);
    if (style == MotionStyle::Original) return;
    const std::uint32_t player = context->gpr[3];
    if (!rmge01_guest_object(player)) return;
    const std::uint32_t frame_ctrl = guest_load_u32(memory, player + 0x20u, services, pc);
    if (!rmge01_guest_object(frame_ctrl)) return;
    const float ratio = std::bit_cast<float>(guest_load_u32(memory, player + 0x08u, services, pc));
    const auto remaining =
        static_cast<std::int32_t>(guest_load_u32(memory, frame_ctrl + 0x14u, services, pc));
    const AnimationInterpoleStep step = shape_animation_interpole(style, ratio, remaining);
    if (step.remaining != remaining) {
        guest_store_u32(memory, frame_ctrl + 0x14u, static_cast<std::uint32_t>(step.remaining), services, pc);
    }
    if (std::bit_cast<std::uint32_t>(step.ratio) != std::bit_cast<std::uint32_t>(ratio)) {
        guest_store_u32(memory, player + 0x08u, std::bit_cast<std::uint32_t>(step.ratio), services, pc);
    }
}

// ScenarioSelectScene builds the perspective of its own CameraContext once, at
// boot, with the aspect in force then; a later aspect never reaches it, and the
// star select drew 16:9 into every other aspect. Each frame its stars and sky
// animate, the projection's x scale is set from its y scale and the current
// aspect. GameSystem (0x806A1228) +0x24 scene controller, +0xBC
// ScenarioSelectScene, +0x30 CameraContext, projection (row-major 4x4) at +0x6C.
void fit_rmge01_scenario_camera(GuestMemoryV1* memory, const NativeServicesV1* services,
                                std::uint32_t pc) {
    const auto policy = experimental_effective_guest_aspect(services);
    if (policy && native_four_three(*policy)) return;
    const double aspect = policy ? policy->content_aspect : kWiiContentAspect;
    if (!(aspect > 0.0)) return;
    std::uint32_t object = guest_load_u32(memory, 0x806A1228u, services, pc);
    for (const std::uint32_t offset : {0x24u, 0xBCu, 0x30u}) {
        if (!rmge01_guest_object(object)) return;
        object = guest_load_u32(memory, object + offset, services, pc);
    }
    if (!rmge01_guest_object(object)) return;
    const std::uint32_t projection = object + 0x6Cu;
    const auto f32 = [&](std::uint32_t address) {
        return std::bit_cast<float>(guest_load_u32(memory, address, services, pc));
    };
    const float y_scale = f32(projection + 0x14u);
    if (!(y_scale > 0.0f) || f32(projection + 0x38u) != -1.0f) return;
    const float x_scale = static_cast<float>(y_scale / aspect);
    if (f32(projection) != x_scale) {
        guest_store_u32(memory, projection, std::bit_cast<std::uint32_t>(x_scale), services, pc);
    }
}

// The host menus' view of the game's own menus (game_menu_probe.h): the name of every layout drawn and not
// hidden. LayoutManager +0x60 hidden, +0x78 name; a manager keeps its name for life, so names are cached by
// manager (and the name's first word, against a recycled address). Reads only.
void note_rmge01_layout(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services) {
    if (context->gpr[2] != 0x806AB280u) return;
    constexpr std::uint32_t pc = 0x80367E64u;
    const std::uint32_t manager = context->gpr[31];
    if (!rmge01_guest_object(manager) || guest_load_u8(memory, manager + 0x60u, services, pc) != 0u) return;
    const std::uint32_t name = guest_load_u32(memory, manager + 0x78u, services, pc);
    if (!rmge01_guest_object(name)) return;
    struct Slot {
        std::uint32_t manager = 0u, name = 0u, word = 0u;
        std::array<char, 24> text{};
        std::size_t length = 0u;
    };
    static std::array<Slot, 256> slots;
    const std::uint32_t word = guest_load_u32(memory, name, services, pc);
    Slot& slot = slots[((manager >> 3u) * 0x9E3779B1u) >> 24u];
    if (slot.manager != manager || slot.name != name || slot.word != word) {
        slot = Slot{manager, name, word, {}, 0u};
        while (slot.length + 1u < slot.text.size()) {
            const auto byte = guest_load_u8(memory, name + static_cast<std::uint32_t>(slot.length), services, pc);
            if (byte == 0u) break;
            slot.text[slot.length++] = static_cast<char>(byte);
        }
    }
    game_menus::note_layout_drawn({slot.text.data(), slot.length});
}

}  // namespace

void dispatch_site_hook(
    const NativeServicesV1* services,
    std::uint32_t guest_pc,
    std::uint32_t kind,
    std::uint32_t argument,
    PpcContext* context,
    GuestMemoryV1* memory) {
    // Controller button prompts follow the controller in use; hooks run often
    // and on the simulation thread, which is where guest memory may be written.
    prompts::tick(memory, services, guest_pc);
    switch (static_cast<SiteHookKind>(kind)) {
    case SiteHookKind::CinemaAndHud:
        note_rmge01_layout(context, memory, services);
        apply_rmge01_cinema_vertical(context, memory, services);
        apply_rmge01_layout_fits(context, memory, services);
        return;
    case SiteHookKind::LayoutBackings:
        apply_rmge01_layout_backings(context, memory, services);
        return;
    case SiteHookKind::HomeBackings:
        apply_rmge01_home_backings(context, memory, services, guest_pc);
        return;
    case SiteHookKind::HomeVertical:
        apply_rmge01_home_vertical(context, memory, services, guest_pc);
        return;
    case SiteHookKind::NativeScreenWidthEnter:
        enter_experimental_rmge01_native_screen_width();
        return;
    case SiteHookKind::NativeScreenWidthExit:
        exit_experimental_rmge01_native_screen_width();
        return;
    case SiteHookKind::FourThreeAspectRead:
        apply_experimental_rmge01_native_four_three_aspect_read(context, memory, services);
        return;
    case SiteHookKind::LayoutConstant:
        apply_experimental_rmge01_layout_constant(
            context, services, argument & 0xFFu, ((argument >> 8u) & 0xFFu) != 0u);
        return;
    case SiteHookKind::Nw4rCanvasWidth:
        apply_experimental_rmge01_nw4r_canvas_width(context, services);
        return;
    case SiteHookKind::CameraAspect:
        apply_experimental_rmge01_camera_aspect(context, services);
        return;
    case SiteHookKind::UltrawideScreenWidth:
        apply_experimental_rmge01_ultrawide_screen_width(context, services);
        return;
    case SiteHookKind::EfbScreenWidth:
        apply_experimental_rmge01_efb_screen_width(context, services);
        return;
    case SiteHookKind::LayoutVerticalFit:
        apply_experimental_rmge01_layout_vertical_fit(
            context, services, argument & 0xFFu, ((argument >> 8u) & 0xFFu) != 0u);
        return;
    case SiteHookKind::HomeGeometry:
        apply_experimental_rmge01_home_geometry(
            context, services, argument & 0xFFu, (argument >> 8u) & 0xFFu);
        return;
    case SiteHookKind::MultiSceneModel:
        fit_rmge01_scenario_sky(context, memory, services, guest_pc);
        fit_rmge01_scenario_camera(memory, services, guest_pc);
        return;
    case SiteHookKind::TextStep: {
        // Text speed 2x/3x: the same step again while text is still appearing
        // (TalkTextFormer::isTextAppearedAll 0x802A368C, updateTalking 0x802A367C).
        const unsigned extra = std::clamp(get_runtime_settings().text_speed, 1u, 3u) - 1u;
        if (extra == 0u) return;
        const std::uint32_t former = argument == 0u
            ? guest_load_u32(memory, context->gpr[31] + 0x24u, services, guest_pc)
            : context->gpr[30] + 0x24u;
        for (unsigned i = 0; i < extra; ++i) {
            context->gpr[3] = former;
            services->call_guest(services->user, 0x802A368Cu, context, memory);
            if ((context->gpr[3] & 0xFFu) != 0u) return;
            context->gpr[3] = former;
            services->call_guest(services->user, 0x802A367Cu, context, memory);
        }
        return;
    }
    case SiteHookKind::Language:
        // The game, its HOME Menu and its message files all follow SCGetLanguage. The answer is
        // fixed at the first question of the session (game_language_for_boot).
        context->gpr[3] = static_cast<std::uint32_t>(game_language_for_boot());
        return;
    case SiteHookKind::AnimationInterpole:
        shape_rmge01_animation_interpole(context, memory, services, guest_pc);
        return;
    case SiteHookKind::MarioActionMain:
        tick_odyssey_moveset(context, memory, services, guest_pc);
        return;
    case SiteHookKind::CoopBeforeCall:
        coop_before_call(argument, context, memory, services, guest_pc);
        return;
    case SiteHookKind::CoopAfterCall:
        coop_after_call(argument, context, memory, services, guest_pc);
        return;
    case SiteHookKind::LodDistance:
        // LodCtrl::update keeps the game's distances: which model shows is
        // also game state (MR::isHiddenModel).
        return;
    }
    throw std::runtime_error("unknown site hook kind " + std::to_string(kind));
}

}  // namespace galaxy
