#include "galaxy/coop_guest.h"

#include "galaxy/coop.h"
#include "galaxy/runtime_settings.h"

#include <array>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

namespace galaxy {
namespace {

// Mario and Luigi co-op on one screen, in one world (docs/MULTIPLAYER.md).
//
// The game has one player: a MarioActor, which the game reaches through the scene's MarioHolder, and whose
// character (gIsLuigi) and controller (Wii Remote channel 0) are global. A second MarioActor is created right
// after the first, as the other character; it is kept out of the game's own movement list and moved by the
// runtime straight after the first player, with the MarioHolder, gIsLuigi and the channel 0 and 1 pads
// swapped to it for that call, so the game's player code runs for it as it does for the first. Its
// controller is channel 1, which the runtime fills from the second controller (KpadSynth).
//
// RMGE01 addresses and layouts are from Petari (symbols.txt, MarioActor.hpp, WPad.hpp and the
// stationed-archive loader); call ids are translate.rs coop_call_id.

namespace call {
constexpr std::uint32_t kKpadRead = 1u;
constexpr std::uint32_t kGameDataHolderIsDataMario = 2u;
constexpr std::uint32_t kGameDataFunctionIsDataMario = 3u;
constexpr std::uint32_t kMarioActorInit2 = 4u;
constexpr std::uint32_t kCategoryListExecuteMovement = 5u;
constexpr std::uint32_t kMountArchive = 6u;
constexpr std::uint32_t kCreateAndAddPlayerArchives = 7u;
constexpr std::uint32_t kPlayerHeapCreateHeap = 8u;
constexpr std::uint32_t kGameSceneDraw3D = 9u;
constexpr std::uint32_t kGxSetViewport = 10u;  // hooked but left alone: viewports stay full-screen
constexpr std::uint32_t kGxSetScissor = 11u;   // likewise: the renderer keeps each half (SplitClip)
constexpr std::uint32_t kClipFrustum = 12u;
constexpr std::uint32_t kShadowFarClipping = 13u;
constexpr std::uint32_t kSetPlayerMode = 14u;
constexpr std::uint32_t kNameObjHolderCtor = 15u;
constexpr std::uint32_t kNameObjHolderAdd = 16u;
}  // namespace call

namespace fn {
constexpr std::uint32_t kCreateMarioActor = 0x80263C34u;     // createNameObj<MarioActor>(const char*)
constexpr std::uint32_t kMarioActorInit2 = 0x802AED64u;      // MarioActor::init2(const TVec3f&, const TVec3f&, s32)
constexpr std::uint32_t kMarioActorMovement = 0x802AFD80u;   // MarioActor::movement()
constexpr std::uint32_t kRequestSuspend = 0x80261278u;       // NameObj::requestSuspend(): out of the movement list
constexpr std::uint32_t kGetMarioHolder = 0x802E1528u;       // MR::getMarioHolder()
constexpr std::uint32_t kSetMarioActor = 0x802E1518u;        // MarioHolder::setMarioActor(MarioActor*)
constexpr std::uint32_t kGetMarioActor = 0x802E1520u;        // MarioHolder::getMarioActor() const
constexpr std::uint32_t kSetCameraTargetToPlayer = 0x803C8ACCu;  // MR::setGameCameraTargetToPlayer()
constexpr std::uint32_t kGetWPadHolder = 0x803AB558u;        // (WPadHolder.cpp) getWPadHolder(): +0 WPad* mPad[2]
constexpr std::uint32_t kSetPlayerPos = 0x803F1F80u;         // MR::setPlayerPos(const TVec3f&)
constexpr std::uint32_t kMountArchive = 0x803CDDD4u;         // MR::mountArchive(const char*, JKRHeap*)
constexpr std::uint32_t kCreateAndAddStationed = 0x803A6A70u;  // ResourceHolderManager::createAndAddStationed(const char*)
// Mario::push(const TVec3f&): added to the velocity on Mario's next move (Mario::tryPushToVelocity), which
// keeps walls and the ground in charge, as when objects shove the player.
constexpr std::uint32_t kMarioPush = 0x8030C330u;
// Split screen (docs/MULTIPLAYER.md): the 3D scene is drawn a second time with the second player's camera.
constexpr std::uint32_t kGameSceneDraw3D = 0x8033EE8Cu;     // GameScene::draw3D() const
constexpr std::uint32_t kCalcViewAndEntry = 0x80343DCCu;    // SceneFunction::executeCalcViewAndEntryList()
constexpr std::uint32_t kLoadViewMtx = 0x803C7394u;         // MR::loadViewMtx(): CameraContext view -> j3dSys
constexpr std::uint32_t kSetDefaultViewportAndScissor = 0x803CA5F4u;  // MR::setDefaultViewportAndScissor()
constexpr std::uint32_t kGetCameraDirector = 0x8009B08Cu;   // MR::getCameraDirector()
constexpr std::uint32_t kIsEventCameraActive = 0x803C799Cu;  // MR::isEventCameraActive()

constexpr std::uint32_t kGetFrameBufferWidth = 0x802B5140u;   // MR::getFrameBufferWidth()
constexpr std::uint32_t kGetFrameBufferHeight = 0x8034AFB0u;  // MR::getFrameBufferHeight()
// MR::getFirstPolyOnLineToMap(TVec3f* hit, Triangle*, const TVec3f& start, const TVec3f& direction): the
// second player's camera collision.
constexpr std::uint32_t kFirstPolyOnLineToMap = 0x803E0874u;
constexpr std::uint32_t kShadowGetDropPos = 0x8016C1B4u;  // ShadowController::getDropPos(TVec3f*) const
constexpr std::uint32_t kIsInAreaObj = 0x803FEA64u;  // MR::isInAreaObj(const char* name, const TVec3f& position)
constexpr std::uint32_t kIsDemoActive = 0x803C9E84u;     // MR::isDemoActive()
constexpr std::uint32_t kIsTimeKeepDemoActive = 0x803C9F80u;  // MR::isTimeKeepDemoActive()
constexpr std::uint32_t kIsInputDisable = 0x802E98B8u;   // MarioModule::isInputDisable() const (on a Mario)
}  // namespace fn

// CameraContext (scene object 0x17), reached as MR::getCameraViewMtx does: *(r13 - 0x3A78) -> +0x24 -> +0xAC ->
// +0x10 -> [0x17].
namespace camera {
constexpr std::uint32_t kView = 0x0Cu;        // 3x4
constexpr std::uint32_t kInverse = 0x3Cu;     // 3x4
constexpr std::uint32_t kProjection = 0x6Cu;  // 4x4; [0][0] is the x scale
constexpr std::uint32_t kAspect = 0xACu;
constexpr std::uint32_t kFarZ = 0xB4u;
constexpr std::uint32_t kFovy = 0xB8u;  // degrees
}  // namespace camera

constexpr std::uint32_t kIsLuigi = 0x806A18C8u;               // gIsLuigi (bool)
constexpr std::uint32_t kResourceHolderManager = 0x806A2480u;  // SingletonHolder<ResourceHolderManager>::sInstance
constexpr std::uint32_t kMovementTypePlayer = 0x25u;          // MR::MovementType_Player
constexpr std::uint32_t kPlayerHeapSize = 0x500000u;          // PlayerHeapHolder's heaps, one character's set
// Room for the second character's model, face and shadow archives next to the first's.
constexpr std::uint32_t kPartnerHeapExtra = 0x200000u;

namespace actor {
constexpr std::uint32_t kPosition = 0x00Cu;     // LiveActor::mPosition
constexpr std::uint32_t kRotation = 0x018u;     // LiveActor::mRotation
constexpr std::uint32_t kMario = 0x230u;        // MarioActor::mMario
constexpr std::uint32_t kCameraTrans = 0x2F4u;  // MarioActor::mCameraTrans: what the game camera looks at
}  // namespace actor
constexpr std::uint32_t kMarioSideVec = 0x310u;  // Mario::mSideVec
constexpr std::uint32_t kMarioUpVec = 0x1FCu;    // Mario::_1FC, Mario::getUpVec()
constexpr std::uint32_t kMarioFrontVec = 0x208u;  // Mario::mFrontVec
constexpr std::uint32_t kMarioState = 0x97Cu;     // Mario: MarioState* of a special state, 0 in ordinary play
// MarioActor::mModels[6] (J3DModelX*): the power-up models, created when their archive is loaded
// (MarioActorDraw.cpp init*Mario).
constexpr std::uint32_t kActorModels = 0xA28u;
constexpr std::uint32_t kActorTeresaParts = 0x9A4u;  // MarioActor::_9A4: the Boo form's parts
// ShadowController::_67 (u8): the shadow is past the far clip from the camera (updateFarClipping).
constexpr std::uint32_t kShadowFarClipped = 0x67u;

bool plausible(std::uint32_t address) {
    return address >= 0x80000000u && address < 0x81800000u && (address & 3u) == 0u;
}

bool trace_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool set = _dupenv_s(&value, &length, "GALAXY_TRACE_COOP") == 0 && value != nullptr &&
                         value[0] != '\0' && value[0] != '0';
        std::free(value);
        return set;
    }();
    return enabled;
}

void trace(const char* format, ...) {
    if (!trace_enabled()) return;
    std::va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[coop] ");
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

class Guest {
public:
    Guest(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc)
        : context_(context), memory_(memory), services_(services), pc_(pc) {}

    std::uint32_t u32(std::uint32_t address) const { return guest_load_u32(memory_, address, services_, pc_); }
    std::uint8_t u8(std::uint32_t address) const { return guest_load_u8(memory_, address, services_, pc_); }
    float f32(std::uint32_t address) const { return std::bit_cast<float>(u32(address)); }
    coop::Vec3 vec(std::uint32_t address) const { return {f32(address), f32(address + 4u), f32(address + 8u)}; }
    void set_u32(std::uint32_t address, std::uint32_t value) const {
        guest_store_u32(memory_, address, value, services_, pc_);
    }
    void set_u8(std::uint32_t address, std::uint8_t value) const {
        const std::uint32_t word = address & ~3u;
        const unsigned shift = (3u - (address & 3u)) * 8u;
        set_u32(word, (u32(word) & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift));
    }
    void set_f32(std::uint32_t address, float value) const { set_u32(address, std::bit_cast<std::uint32_t>(value)); }
    void set_vec(std::uint32_t address, coop::Vec3 v) const {
        set_f32(address, v.x);
        set_f32(address + 4u, v.y);
        set_f32(address + 8u, v.z);
    }

    // A game function with up to four integer arguments; returns r3. Callers save and restore the context.
    std::uint32_t call(std::uint32_t function, std::uint32_t r3 = 0u, std::uint32_t r4 = 0u, std::uint32_t r5 = 0u,
                       std::uint32_t r6 = 0u) const {
        context_->gpr[3] = r3;
        context_->gpr[4] = r4;
        context_->gpr[5] = r5;
        context_->gpr[6] = r6;
        services_->call_guest(services_->user, function, context_, memory_);
        return context_->gpr[3];
    }

    // Guest stack room below the caller's frame for vectors passed by reference; r1 moves under it for the
    // calls made while the frame lives and is restored with the context.
    class Frame {
    public:
        explicit Frame(const Guest& guest) : guest_(guest), sp_(guest.context_->gpr[1]) {
            base_ = (sp_ - 0x140u) & ~0xFu;
            guest_.set_u32(base_, sp_);  // back chain
            guest_.context_->gpr[1] = base_;
        }
        ~Frame() { guest_.context_->gpr[1] = sp_; }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        // Slot 0..3, 16 bytes each.
        std::uint32_t vec(unsigned slot, coop::Vec3 value) const {
            const std::uint32_t at = base_ + 0x10u + slot * 0x10u;
            guest_.set_vec(at, value);
            return at;
        }
        // 0xC0 bytes of scratch after the vector slots (an out structure such as a Triangle).
        std::uint32_t scratch() const { return base_ + 0x60u; }

    private:
        const Guest& guest_;
        std::uint32_t sp_;
        std::uint32_t base_ = 0u;
    };

    std::string string(std::uint32_t address, std::size_t limit = 96u) const {
        std::string text;
        if (!plausible(address & ~3u)) return text;
        for (std::size_t i = 0; i < limit; ++i) {
            const std::uint8_t c = u8(address + static_cast<std::uint32_t>(i));
            if (c == 0u) break;
            text.push_back(static_cast<char>(c));
        }
        return text;
    }

    // The address of a NUL-terminated string in the game's data, or 0.
    std::uint32_t find_string(std::string_view text) const {
        constexpr std::uint32_t kBegin = 0x80004000u;
        constexpr std::uint32_t kEnd = 0x80700000u;
        const auto first = static_cast<std::uint8_t>(text[0]);
        for (std::uint32_t at = kBegin + 1u; at + text.size() + 1u < kEnd; ++at) {
            if (u8(at) != first || u8(at - 1u) != 0u) continue;
            bool same = true;
            for (std::size_t i = 1; i < text.size() && same; ++i) {
                same = u8(at + static_cast<std::uint32_t>(i)) == static_cast<std::uint8_t>(text[i]);
            }
            if (same && u8(at + static_cast<std::uint32_t>(text.size())) == 0u) return at;
        }
        return 0u;
    }

    PpcContext* context() const { return context_; }

private:
    PpcContext* context_;
    GuestMemoryV1* memory_;
    const NativeServicesV1* services_;
    std::uint32_t pc_;
};

// Restores the interrupted game call's registers when a hook has called into the game itself. The timer
// state (time base, DEC) is the machine's, not the call's: the game's own calls move it, and rewinding it
// makes the runtime's DEC tracker diverge from the guest.
class SavedContext {
public:
    explicit SavedContext(PpcContext* context) : context_(context), saved_(*context) {}
    ~SavedContext() {
        const PpcTimerState timer = capture_timer_state(context_);
        *context_ = saved_;
        restore_timer_state(context_, timer);
    }
    SavedContext(const SavedContext&) = delete;
    SavedContext& operator=(const SavedContext&) = delete;

private:
    PpcContext* context_;
    PpcContext saved_;
};


// Guest addresses of the strings the archive calls take, found once.
struct ArchiveNames {
    bool searched = false;
    std::array<std::uint32_t, 3> mario{};  // model, face, shadow paths
    std::array<std::uint32_t, 3> luigi{};
    const std::array<std::uint32_t, 3>& of(coop::Character character) const {
        return character == coop::Character::Luigi ? luigi : mario;
    }
};

struct State {
    // Arguments of the calls in flight, kept from before the call for after it. KPADRead and the archive
    // calls do not nest; the movement categories and init2 calls are not re-entered by the runtime's own
    // calls (guarded below).
    std::uint32_t kpad_channel = 0xFFFFFFFFu;
    std::uint32_t kpad_buffer = 0u;
    std::uint32_t mount_path = 0u;
    std::uint32_t mount_heap = 0u;
    bool mounting_partner = false;
    std::uint32_t player_archives_is_mario = 1u;
    std::array<std::uint32_t, 8> init2_actors{};
    unsigned init2_depth = 0u;
    std::array<std::uint32_t, 8> categories{};
    unsigned category_depth = 0u;

    // The players. `own` is the character of the game's save (the first player); the second player is
    // the other one.
    coop::Character own = coop::Character::Mario;
    std::uint32_t first = 0u;
    std::uint32_t second = 0u;
    std::uint32_t second_mario = 0u;
    bool creating_second = false;
    bool moving_second = false;
    // During the second player's creation the save is asked which character the player is; the answer
    // is the second player's.
    bool answer_partner = false;

    ArchiveNames names;
    // The Observatory's doors and domes (AstroChangeStageCube areas) test the game's player, the first: when
    // the second player walks into one the players swap places, so the game's own check takes them through.
    // `entrance_name` is the area class's name in the game's data; `entrance_wait` frames between swaps.
    coop::Vec3 parked_position{};
    // The scene's NameObjHolder (every NameObj in the scene, a fixed-size array without a bounds check): the
    // second player's MarioActor and its parts add their own, so it is made larger (kNameObjHolderExtra);
    // `holder_count` counts what is added, for the trace.
    std::uint32_t scene_holder = 0u;
    std::uint32_t holder_capacity = 0u;
    std::uint32_t holder_count = 0u;
    bool second_waiting_input = false;
    bool second_waiting_demo = false;
    std::uint32_t entrance_name = 0u;
    bool entrance_searched = false;
    unsigned entrance_wait = 0u;
    coop::KpadSynth kpad;
    coop::LeashTimer leash;
    unsigned frames = 0u;

    // Split screen. `follow_view` is the second player's own camera (follow, steered and kept out of walls),
    // `camera2` what their half draws with (blended in from the first player's as the split eases in);
    // `layout` the line between the halves. `pass` is 1 or 2 while GameScene::draw3D draws the first or
    // second half, else 0.
    coop::SplitState split;
    coop::FollowCamera follow;
    coop::CameraMatrices follow_view;
    coop::CameraMatrices camera2;
    coop::SplitLayout layout;
    std::uint32_t steer_buttons = 0u;
    float screen_width = 640.0f;
    float screen_height = 456.0f;
    unsigned pass = 0u;
    // A camera event the second player set off (a warp, launch star, pipe): the game camera is theirs while
    // it plays, so their half shows it, and the first player's half holds `last_camera1` (the game camera
    // before the event), moved with them, until the game camera is back after it (CameraHold).
    bool event_second = false;
    unsigned event_inputs = 0u;  // for the trace: what camera_event_is_second last saw
    // When the game camera changes hands (a second player's event begins or ends), the half it moves to
    // blends from that half's own camera into it, so the game camera's swing over from the other player is
    // not seen.
    coop::CameraHandover handover;
    coop::CameraHold hold;
    // The game camera's last view that was the first player's, and where they were then.
    coop::CameraMatrices last_camera1;
    std::array<float, 16> last_projection{};
    coop::Vec3 last_origin{};
    // The second player's half, eased from the event's camera into their own when their event ends;
    // `second_view` is what their half drew last frame.
    coop::CameraHandover second_handover;
    // At the start of the second player's event the game camera travels over from the first player: their
    // half keeps its own camera until the game camera has arrived, then eases into it.
    bool second_awaits_event = false;
    // The second player travels with the first (second_player_waits, the first player parked at the origin, a
    // level start until the first has had control a moment); `controllable_frames` counts frames the first
    // player has been in ordinary play.
    bool travelling = false;
    bool sticky_travel = false;
    bool level_start = false;
    unsigned controllable_frames = 0u;
    coop::CameraMatrices second_view;
    // Trace: frames of per-frame camera detail left after a camera change of hands.
    unsigned camera_trace_frames = 0u;
    std::uint32_t shadow_controller = 0u;
    float shadow_clip = 0.0f;
    std::uint32_t draw_scene = 0u;
    std::uint32_t clip_position = 0u;
    float clip_radius = 0.0f;
    // The first player's camera and projection as the game made them, restored after the second half.
    coop::CameraMatrices camera1;
    std::array<float, 16> projection{};
    // The camera's lens, for the second player's clipping.
    float fovy = 45.0f;
    float aspect = 16.0f / 9.0f;
    float far_z = 100000.0f;
};

bool split_screen_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool off = _dupenv_s(&value, &length, "GALAXY_COOP_SPLIT") == 0 && value != nullptr && value[0] == '0';
        std::free(value);
        return !off;
    }();
    return enabled;
}

State& state() {
    static State s;
    return s;
}

// native_runtime.cpp puts split screen's line in the GX stream (coop_set_split_clip_sink).
struct SplitClipSinkSlot {
    SplitClipSink sink = nullptr;
    void* user = nullptr;
};
SplitClipSinkSlot& split_clip_sink() {
    static SplitClipSinkSlot slot;
    return slot;
}
void send_split_clip(const std::array<float, 3>& plane, unsigned pass,
                     const std::array<float, 4>& separator = {0.0f, 0.0f, 0.0f, 0.0f}) {
    const SplitClipSinkSlot& slot = split_clip_sink();
    if (slot.sink != nullptr) slot.sink(slot.user, plane.data(), pass, separator.data());
}

std::uint32_t camera_context(const Guest& guest) {
    std::uint32_t object = guest.u32(guest.context()->gpr[13] - 0x3A78u);
    for (const std::uint32_t offset : {0x24u, 0xACu, 0x10u, 0x17u * 4u}) {
        if (!plausible(object)) return 0u;
        object = guest.u32(object + offset);
    }
    return plausible(object) ? object : 0u;
}

template <std::size_t N>
std::array<float, N> read_floats(const Guest& guest, std::uint32_t address) {
    std::array<float, N> values{};
    for (std::size_t i = 0; i < N; ++i) values[i] = guest.f32(address + static_cast<std::uint32_t>(4u * i));
    return values;
}

template <std::size_t N>
void write_floats(const Guest& guest, std::uint32_t address, const std::array<float, N>& values) {
    for (std::size_t i = 0; i < N; ++i) guest.set_f32(address + static_cast<std::uint32_t>(4u * i), values[i]);
}

void write_camera(const Guest& guest, std::uint32_t context, const coop::CameraMatrices& matrices) {
    write_floats(guest, context + camera::kView, matrices.view);
    write_floats(guest, context + camera::kInverse, matrices.inverse);
}

coop::CameraMatrices read_camera(const Guest& guest, std::uint32_t context) {
    return {read_floats<12>(guest, context + camera::kView), read_floats<12>(guest, context + camera::kInverse)};
}

bool finite(const std::array<float, 12>& values) {
    for (const float value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}


coop::Vec3 player_up(const Guest& guest, std::uint32_t mario) {
    return coop::normalized(guest.vec(mario + kMarioUpVec), {0.0f, 1.0f, 0.0f});
}

std::uint8_t character_byte(coop::Character character) {
    return character == coop::Character::Luigi ? 1u : 0u;
}

void ensure_archive_names(const Guest& guest, State& s) {
    if (s.names.searched) return;
    s.names.searched = true;
    for (const coop::Character character : {coop::Character::Mario, coop::Character::Luigi}) {
        const coop::CharacterArchives archives = coop::archives_of(character);
        auto& names = character == coop::Character::Luigi ? s.names.luigi : s.names.mario;
        names = {guest.find_string(archives.model_path), guest.find_string(archives.face_path),
                 guest.find_string(archives.shadow_path)};
    }
    trace("archive names: Mario %08X %08X %08X, Luigi %08X %08X %08X", s.names.mario[0], s.names.mario[1],
          s.names.mario[2], s.names.luigi[0], s.names.luigi[1], s.names.luigi[2]);
}

// After the game mounts its player's model archive, the other character's model, face and shadow are
// mounted into the same heap (the stationed-archive loader's thread).
void mount_partner_archives(const Guest& guest, State& s) {
    coop::Character own_model = coop::Character::Mario;
    if (!coop::own_model_archive(guest.string(s.mount_path), own_model)) return;
    ensure_archive_names(guest, s);
    const coop::Character partner = coop::partner_of(own_model);
    SavedContext saved(guest.context());
    s.mounting_partner = true;
    for (const std::uint32_t path : s.names.of(partner)) {
        if (path == 0u) continue;
        const std::uint32_t mounted = guest.call(fn::kMountArchive, path, s.mount_heap);
        trace("mounted %s into heap %08X: %08X", guest.string(path).c_str(), s.mount_heap, mounted);
    }
    s.mounting_partner = false;
}

// After the game makes the resource holders of its player's archives, the same for the other character.
void add_partner_resources(const Guest& guest, State& s) {
    ensure_archive_names(guest, s);
    const coop::Character own = s.player_archives_is_mario != 0u ? coop::Character::Mario : coop::Character::Luigi;
    const std::uint32_t manager = guest.u32(kResourceHolderManager);
    if (!plausible(manager)) return;
    SavedContext saved(guest.context());
    for (const std::uint32_t path : s.names.of(coop::partner_of(own))) {
        if (path == 0u) continue;
        const std::uint32_t holder = guest.call(fn::kCreateAndAddStationed, manager, path);
        trace("resource holder for %s: %08X", guest.string(path).c_str(), holder);
    }
}

// Right after the game sets up its player, the second player: the same MarioActor as the other character,
// beside the first, out of the movement list.
void create_second_player(const Guest& guest, State& s, std::uint32_t first) {
    SavedContext saved(guest.context());
    s.first = first;
    s.second = 0u;
    s.second_mario = 0u;
    s.kpad.reset();
    s.leash.reset();
    s.split.reset();
    // Until the first player has landed and has control, the second waits where they were placed.
    s.level_start = true;
    s.travelling = true;
    s.sticky_travel = false;
    s.controllable_frames = 0u;
    s.follow.reset();
    const std::uint32_t first_mario = guest.u32(first + actor::kMario);
    if (!plausible(first_mario)) return;
    const coop::Vec3 position = guest.vec(first + actor::kPosition);
    const coop::Vec3 rotation = guest.vec(first + actor::kRotation);
    const coop::Vec3 side = guest.vec(first_mario + kMarioSideVec);
    const coop::Vec3 beside = position + side * 120.0f;
    s.parked_position = beside;

    // The actor's name: the debug name the game gives its own player is unknown, so the archive path does.
    ensure_archive_names(guest, s);
    const std::uint32_t name = s.names.of(coop::partner_of(s.own))[0];
    s.creating_second = true;
    const std::uint32_t second = guest.call(fn::kCreateMarioActor, name);
    if (plausible(second)) {
        Guest::Frame frame(guest);
        s.answer_partner = true;
        guest.call(fn::kMarioActorInit2, second, frame.vec(0, beside), frame.vec(1, rotation), 1u);  // 1: 基本
        s.answer_partner = false;
    }
    s.creating_second = false;
    // init2 made the new actor the scene's player and set gIsLuigi to its character: back to the first.
    guest.set_u8(kIsLuigi, character_byte(s.own));
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    if (plausible(holder)) guest.call(fn::kSetMarioActor, holder, first);
    guest.call(fn::kSetCameraTargetToPlayer);
    if (!plausible(second)) {
        trace("second player not created (%08X)", second);
        return;
    }
    guest.call(fn::kRequestSuspend, second);
    s.second = second;
    s.second_mario = guest.u32(second + actor::kMario);
    trace("players: first %08X (%s), second %08X (%s)", first, s.own == coop::Character::Luigi ? "Luigi" : "Mario",
          second, s.own == coop::Character::Luigi ? "Mario" : "Luigi");
}

// Swaps WPadHolder's WPads of channels 0 and 1, so the game's player code (which asks MR::getWPad for
// channel 0) reads the second player's controller. Swapping twice restores them.
void swap_pads(const Guest& guest, std::uint32_t pad_holder) {
    const std::uint32_t pad0 = guest.u32(pad_holder);
    guest.set_u32(pad_holder, guest.u32(pad_holder + 4u));
    guest.set_u32(pad_holder + 4u, pad0);
}

// The players do not pass through each other: when they overlap, each is pushed half the way apart.
void bump_players(const Guest& guest, const State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    if (!plausible(first_mario) || !plausible(s.second_mario)) return;
    coop::Vec3 up = guest.vec(first_mario + kMarioUpVec);
    const float up_length = coop::length(up);
    if (!std::isfinite(up_length) || up_length < 0.5f) return;
    up = up * (1.0f / up_length);
    const coop::Vec3 push = coop::body_push(first_position, second_position, up);
    if (push.x == 0.0f && push.y == 0.0f && push.z == 0.0f) return;
    Guest::Frame frame(guest);
    guest.call(fn::kMarioPush, s.second_mario, frame.vec(0, push));
    guest.call(fn::kMarioPush, first_mario, frame.vec(1, push * -1.0f));
}

// The game camera's player: CameraDirector +0x34 CameraTargetHolder, +0xC its CameraTargetPlayer, whose
// MarioActor is at +0x10 (Petari CameraDirector.hpp, CameraTargetHolder.hpp, CameraTargetObj.hpp).
// While the second player moves, the game's player is them, so a warp, launch star, pipe or event camera
// they set off (MR::setCameraTargetToPlayer, CameraTargetArg::setTarget) points the camera at them. While
// that event camera plays it stays theirs (event_second: their half shows it); after, the camera is the
// first player's again.
void keep_camera_on_first_player(const Guest& guest, State& s) {
    const std::uint32_t director = guest.call(fn::kGetCameraDirector);
    if (!plausible(director)) return;
    const std::uint32_t holder = guest.u32(director + 0x34u);
    if (!plausible(holder)) return;
    const std::uint32_t target = guest.u32(holder + 0xCu);
    if (!plausible(target)) return;
    const std::uint32_t actor = guest.u32(target + 0x10u);
    const bool event = (guest.call(fn::kIsEventCameraActive) & 0xFFu) != 0u;
    const bool was = s.event_second;
    // An event set off by the second player's own move points the camera at them; one an object starts for
    // them (a launch star, a warp pad) points it at the first player, but then only the second player is
    // in a special state (flying, warping).
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    const bool first_special = plausible(first_mario) && guest.u32(first_mario + kMarioState) != 0u;
    const bool second_special = plausible(s.second_mario) && guest.u32(s.second_mario + kMarioState) != 0u;
    s.event_second = coop::camera_event_is_second(actor, s.second, event, second_special, first_special);
    const unsigned inputs = (event ? 1u : 0u) | (actor == s.second ? 2u : actor == s.first ? 4u : 0u) |
                            (second_special ? 8u : 0u) | (first_special ? 16u : 0u);
    if (inputs != s.event_inputs) {
        trace("camera event %d, camera on %s, special: second %d first %d (split %.2f)", event ? 1 : 0,
              actor == s.second ? "second" : actor == s.first ? "first" : "other", second_special ? 1 : 0,
              first_special ? 1 : 0, s.split.amount());
        s.event_inputs = inputs;
        s.camera_trace_frames = 120u;
    }
    if (s.event_second != was) {
        trace("second player's camera event %s", s.event_second ? "begins" : "ends");
        s.camera_trace_frames = 120u;
        if (s.event_second && s.split.amount() > 0.0f) {
            // Their half blends from their own camera into the event's; the first player's half holds the
            // game camera's view from before the event.
            // The event camera aims at the first player for its first frames (until it follows the second),
            // so their half keeps its own view a moment before easing in.
            s.second_awaits_event = true;
            s.hold.start(s.last_camera1, s.last_origin, s.last_projection);
        } else if (!s.event_second && s.split.amount() > 0.0f) {
            // Their half eases from the event's camera back into their own.
            s.second_handover.start(s.second_view, true);
        }
    }
    // The first player's half holds on past the event while the second player is still in flight (a launch
    // star's flight outlasts its event camera, and the game camera follows the flight); once they land it
    // waits for the game camera to be back.
    if (!s.event_second && s.hold.active() && !s.hold.returning() && !second_special) {
        s.hold.release();
        s.camera_trace_frames = 120u;
        trace("second player landed: the first player's half waits for the game camera");
    }
    if (s.event_second) {
        // The event's camera follows the second player while it plays.
        if (actor != s.second) guest.set_u32(target + 0x10u, s.second);
        return;
    }
    const std::uint32_t kept = coop::camera_player(actor, s.first, s.second);
    if (kept == actor) return;
    guest.set_u32(target + 0x10u, kept);
    trace("camera target was the second player (%08X): back to the first (%08X)", actor, kept);
}

// While split, after both players moved: the second player's own camera (it trails them, turned and raised
// by their controller, recentred by C, and kept in front of walls), and where the line between the halves
// runs: square to where the second player is from the first on the first player's screen.
void update_second_camera(const Guest& guest, State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t context = camera_context(guest);
    if (context == 0u || !plausible(s.second_mario)) return;
    const coop::CameraMatrices first_camera = read_camera(guest, context);
    if (!finite(first_camera.view) || !finite(first_camera.inverse)) return;
    const coop::SplitTuning tuning{};
    const coop::Vec3 up = player_up(guest, s.second_mario);
    if (!s.follow.started()) {
        // The split begins: the second player's camera starts as the shared one, moved onto them.
        const coop::Vec3 midpoint = first_position + (second_position - first_position) * 0.5f;
        s.follow.start(coop::pose_of(first_camera.inverse).eye, midpoint, second_position, up, tuning);
        trace("split begins");
    }
    const coop::PartnerInput input = coop_partner_input();
    const coop::CameraSteer steer = coop::camera_steer(input, s.steer_buttons);
    s.steer_buttons = input.connected ? input.buttons : 0u;
    if (steer.recentre) s.follow.recentre(second_position, guest.vec(s.second_mario + kMarioFrontVec), tuning);
    s.follow.steer(steer.turn, steer.raise, tuning);
    s.follow_view = s.follow.update(second_position, up, tuning);

    // Camera collision: the first wall between the point the camera looks at and its eye.
    const coop::Vec3 look = s.follow.look();
    const coop::Vec3 eye = s.follow.eye();
    {
        Guest::Frame frame(guest);
        const std::uint32_t hit = frame.vec(0, eye);
        const std::uint32_t found =
            guest.call(fn::kFirstPolyOnLineToMap, hit, frame.scratch(), frame.vec(1, look), frame.vec(2, eye - look));
        if ((found & 0xFFu) != 0u) {
            const coop::Vec3 wall = guest.vec(hit);
            if (std::isfinite(coop::length(wall))) {
                s.follow_view = coop::look_at(coop::camera_before_wall(look, wall, tuning.wall_margin), s.follow.up(), look);
            }
        }
    }

    // The line follows the first player's view as their half shows it: while it is held (the game camera is
    // the second player's event or warp camera) the held view, so the line does not swing with that camera.
    const coop::CameraMatrices layout_camera = s.hold.active() ? s.hold.view(first_position) : first_camera;
    const coop::Vec3 a = coop::transform_point(layout_camera.view, first_position);
    const coop::Vec3 b = coop::transform_point(layout_camera.view, second_position);
    s.layout.update(b.x - a.x, a.y - b.y);  // screen y runs down
}

// How far towards the edge of a shared picture the players would be, with the game camera's turn and
// distance (from the midpoint it looks at while the screen is one, from the first player once split).
float shared_extent(const Guest& guest, const State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return 0.0f;
    const coop::CameraMatrices camera = read_camera(guest, context);
    if (!finite(camera.view) || !finite(camera.inverse)) return 0.0f;
    float fovy = guest.f32(context + camera::kFovy);
    float aspect = guest.f32(context + camera::kAspect);
    if (!std::isfinite(fovy) || fovy <= 1.0f || fovy >= 170.0f) fovy = s.fovy;
    if (!std::isfinite(aspect) || aspect <= 0.1f) aspect = s.aspect;
    const coop::CameraPose pose = coop::pose_of(camera.inverse);
    const coop::Vec3 looked_at =
        s.split.split() ? first_position : first_position + (second_position - first_position) * 0.5f;
    const float depth = coop::dot(pose.forward, looked_at - pose.eye);
    return coop::shared_view_extent(camera, first_position, second_position, depth, fovy, aspect);
}

// One screen again: nothing held.
void remember_first_camera(const Guest& guest, State& s) {
    (void)guest;
    if (s.split.amount() == 0.0f) s.hold.reset();
}

// After the game moves its players, the second player: one MarioActor::movement with the game's player,
// character and controller switched to it. Then the shared camera, and the warp back to the first player.
void move_second_player(const Guest& guest, State& s) {
    SavedContext saved(guest.context());
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    const std::uint32_t player = plausible(holder) ? guest.call(fn::kGetMarioActor, holder) : 0u;
    if (player != s.first) {
        // A different scene's player: this second player went with its scene.
        trace("player changed (%08X): second player dropped", player);
        s.first = s.second = s.second_mario = 0u;
        s.split.reset();
        s.follow.reset();
        return;
    }
    const std::uint32_t pad_holder = guest.call(fn::kGetWPadHolder);
    const bool pads = plausible(pad_holder) && plausible(guest.u32(pad_holder)) && plausible(guest.u32(pad_holder + 4u));

    // While the game has the first player's controls (a dome's Pull Star, the flight into a galaxy) or a
    // cutscene plays, the second player goes with them, beside them: those sequences change the map
    // (collision, gravity) around the first player and dropped the second through the floor.
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    const bool first_held = plausible(first_mario) && (guest.call(fn::kIsInputDisable, first_mario) & 0xFFu) != 0u;
    // A dome's galaxy selector puppets the first player in a time-keep demo (not MR::isDemoActive).
    const bool demo = (guest.call(fn::kIsDemoActive) & 0xFFu) != 0u ||
                      (guest.call(fn::kIsTimeKeepDemoActive) & 0xFFu) != 0u;
    if (first_held != s.second_waiting_input || demo != s.second_waiting_demo) {
        trace("second player %s (first player's controls held %d, cutscene %d)",
              first_held || demo ? "goes beside the first" : "is free again", first_held ? 1 : 0, demo ? 1 : 0);
        s.second_waiting_input = first_held;
        s.second_waiting_demo = demo;
    }
    const coop::Vec3 first_now = guest.vec(s.first + actor::kPosition);
    // A puppeted first player's actor sits at the origin (a dome's galaxy selector).
    const bool first_parked = !(coop::length(first_now) >= 1.0f);
    // Once a dome's galaxy selector has puppeted the first player, the rest of the scene is the flight into the
    // chosen galaxy (the dome floor no longer holds the second player): travel with them until the next scene.
    if (first_parked && !s.level_start) s.sticky_travel = true;
    const bool first_busy = coop::second_player_waits(first_held, demo) || first_parked || s.sticky_travel;
    s.controllable_frames = first_busy ? 0u : s.controllable_frames + 1u;
    bool land_beside = false;
    if (s.level_start && s.controllable_frames >= coop::kLevelStartSettleFrames) {
        // The first player has landed and has control: the second joins them.
        s.level_start = false;
        land_beside = true;
        trace("level start over: the second player joins the first");
    }
    s.travelling = coop::keep_travelling(s.travelling, s.level_start || first_busy, s.controllable_frames);
    const bool beside_first = s.travelling;

    const coop::Character partner = coop::partner_of(s.own);
    s.moving_second = true;
    guest.set_u8(kIsLuigi, character_byte(partner));
    guest.call(fn::kSetMarioActor, holder, s.second);
    if (pads) swap_pads(guest, pad_holder);
    // While split, the second player's stick is relative to their own camera: the player code reads the
    // camera's directions from CameraContext.
    const std::uint32_t camera_ctx =
        s.split.amount() > 0.0f && s.follow.started() && !s.event_second ? camera_context(guest) : 0u;
    const coop::CameraMatrices first_camera = camera_ctx != 0u ? read_camera(guest, camera_ctx) : coop::CameraMatrices{};
    if (camera_ctx != 0u) write_camera(guest, camera_ctx, s.camera2);
    guest.call(fn::kMarioActorMovement, s.second);
    if (camera_ctx != 0u) write_camera(guest, camera_ctx, first_camera);
    if ((beside_first || land_beside) && plausible(first_mario)) {
        // Still the second player's turn: MR::setPlayerPos moves the scene's player. A puppeted first player's
        // actor sits at the origin, and at a level start the first is still flying in: the second player then
        // stays where they were.
        const coop::Vec3 there = s.level_start ? s.parked_position
                                               : coop::travel_position(first_now, guest.vec(first_mario + kMarioSideVec),
                                                                       s.parked_position);
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, there));
    }

    const coop::Vec3 first_position = guest.vec(s.first + actor::kPosition);
    const coop::Vec3 second_position = guest.vec(s.second + actor::kPosition);
    if (!beside_first) s.parked_position = second_position;
    coop::CameraShare share{};
    bool too_far = false;
    if (split_screen_enabled()) {
        // The camera looks at the midpoint until the split and eases to the first player as it splits.
        const coop::SplitTuning tuning{};
        const float distance = coop::length(second_position - first_position);
        // While the second player's camera event plays the game camera is theirs, so it cannot tell whether
        // both fit one picture: the screen stays as it is until the event ends.
        if (!s.event_second) {
            s.split.update(shared_extent(guest, s, first_position, second_position), distance, tuning);
        }
        if (s.split.amount() == 0.0f) {
            s.follow.reset();
            s.layout.reset();
        }
        if (std::isfinite(distance)) share.offset = (second_position - first_position) * (0.5f * (1.0f - s.split.eased()));
        too_far = !std::isfinite(distance) || distance >= tuning.split_warp;
    } else {
        share = coop::shared_camera(first_position, second_position);
        too_far = share.out_of_range;
    }
    // The second player at a door or dome the first player is not at: swap places (MR::setPlayerPos moves the
    // scene's player, the second now and the first below).
    bool swap_places = false;
    if (beside_first) {
        // Travelling with the first player: no doors, and no bringing back (the first may be parked).
        s.leash.reset();
    } else if (s.entrance_wait > 0u) {
        --s.entrance_wait;
    } else {
        if (!s.entrance_searched) {
            s.entrance_searched = true;
            s.entrance_name = guest.find_string("AstroChangeStageCube");
            trace("entrance area name at %08X", s.entrance_name);
        }
        if (s.entrance_name != 0u) {
            Guest::Frame frame(guest);
            const bool second_in = (guest.call(fn::kIsInAreaObj, s.entrance_name, frame.vec(0, second_position)) & 0xFFu) != 0u;
            const bool first_in = second_in &&
                (guest.call(fn::kIsInAreaObj, s.entrance_name, frame.vec(1, first_position)) & 0xFFu) != 0u;
            swap_places = coop::swap_for_entrance(second_in, first_in);
        }
    }
    if (swap_places) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, first_position));
        s.entrance_wait = 120u;
        trace("second player at an entrance: the players swap places");
    } else if (!beside_first && s.leash.update(too_far, coop::CameraTuning{}.warp_frames)) {
        // Still the second player's turn: MR::setPlayerPos moves the scene's player.
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, first_position));
        trace("second player brought back to the first");
    }

    if (pads) swap_pads(guest, pad_holder);
    guest.call(fn::kSetMarioActor, holder, s.first);
    guest.set_u8(kIsLuigi, character_byte(s.own));
    s.moving_second = false;
    if (swap_places) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, second_position));
    }

    keep_camera_on_first_player(guest, s);
    bump_players(guest, s, first_position, second_position);
    if (s.split.amount() > 0.0f) update_second_camera(guest, s, first_position, second_position);
    remember_first_camera(guest, s);

    // The game camera looks at the first player's camera point; move it towards the second player.
    const coop::Vec3 look = guest.vec(s.first + actor::kCameraTrans);
    guest.set_vec(s.first + actor::kCameraTrans, look + share.offset);

    if (trace_enabled() && (++s.frames % 120u) == 0u) {
        trace("first (%.0f %.0f %.0f) second (%.0f %.0f %.0f) camera offset (%.0f %.0f %.0f)%s", first_position.x,
              first_position.y, first_position.z, second_position.x, second_position.y, second_position.z,
              share.offset.x, share.offset.y, share.offset.z, too_far ? " out of range" : "");
        if (s.split.amount() > 0.0f) {
            trace("split %.2f, line at %.0f degrees", s.split.amount(), s.layout.angle() * 57.2958f);
        }
    }
}

// The lens of a player's half: the game's, except the first player's while their view is held (the game's
// lens is the other player's event camera's then) and as it eases back, and the second player's while their
// half waits for the event camera.
std::array<float, 16> half_lens(const State& s, bool second) {
    if (!second && s.hold.active()) return s.hold.projection();
    if (second && s.second_awaits_event && s.hold.active()) return s.hold.projection();
    return s.handover.projection(s.projection, second);
}

// The projection of a player's half: centred on the middle of their side of the line as the split eases in.
std::array<float, 16> half_projection(const State& s, bool second) {
    std::array<float, 16> projection = half_lens(s, second);
    const std::array<float, 2> centre = s.layout.centre(second, s.screen_width, s.screen_height);
    coop::split_projection(projection, centre[0], centre[1], s.split.eased());
    return projection;
}

// While the second player's camera event plays, the halves swap cameras: the game camera (theirs) draws
// first, in their half, and the first player's held view second.
bool events_swapped(const State& s) { return s.event_second && s.hold.active(); }
// Whether the pass being drawn is the second player's half.
bool second_half(const State& s) { return (s.pass == 2u) != events_swapped(s); }

// Before GameScene::draw3D: when split, the first pass, with the game camera: the first player's half (or
// the second's while their camera event plays). The other camera is blended in from the game's as the
// split eases in; the renderer keeps each pass to its side of the line.
void begin_split_draw(const Guest& guest, State& s) {
    s.pass = 0u;
    if (!split_screen_enabled() || s.split.amount() == 0.0f || !plausible(s.second) || !plausible(s.second_mario) ||
        !plausible(s.first) || !s.follow.started() || !s.layout.started()) {
        return;
    }
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return;
    s.camera1 = read_camera(guest, context);
    s.projection = read_floats<16>(guest, context + camera::kProjection);
    if (!finite(s.camera1.view) || !finite(s.camera1.inverse)) return;
    const float fovy = guest.f32(context + camera::kFovy);
    const float aspect = guest.f32(context + camera::kAspect);
    const float far_z = guest.f32(context + camera::kFarZ);
    if (std::isfinite(fovy) && fovy > 1.0f && fovy < 170.0f) s.fovy = fovy;
    if (std::isfinite(aspect) && aspect > 0.1f) s.aspect = aspect;
    if (std::isfinite(far_z) && far_z > 1.0f) s.far_z = far_z;
    {
        SavedContext saved(guest.context());
        const auto width = static_cast<float>(guest.call(fn::kGetFrameBufferWidth) & 0xFFFFu);
        const auto height = static_cast<float>(guest.call(fn::kGetFrameBufferHeight) & 0xFFFFu);
        if (width >= 320.0f && width <= 1024.0f) s.screen_width = width;
        if (height >= 240.0f && height <= 640.0f) s.screen_height = height;
    }
    const coop::Vec3 first_position = guest.vec(s.first + actor::kPosition);
    const coop::Vec3 second_position = guest.vec(s.second + actor::kPosition);
    if (!events_swapped(s)) {
        // A game camera set up at the second player's warp while aiming at the first player is not the first
        // player's: their half holds its last good view until the game camera is back with them.
        if (coop::camera_is_second_players(coop::pose_of(s.camera1.inverse).eye, first_position, second_position)) {
            if (!s.hold.active()) {
                s.hold.start(s.last_camera1, s.last_origin, s.last_projection);
                trace("the game camera is at the second player: the first player's half holds");
            } else {
                s.hold.rearm();
            }
        } else if (!s.hold.active() && !s.handover.active()) {
            s.last_camera1 = s.camera1;
            s.last_projection = s.projection;
            s.last_origin = first_position;
        }
    }
    // Pans from the first player's view at the split to the second player's own.
    s.camera2 = events_swapped(s) ? s.hold.view(first_position)
                                  : coop::blend_cameras(s.camera1, s.follow_view, s.split.eased());
    if (!events_swapped(s) && s.second_handover.active()) {
        s.camera2 = s.second_handover.camera(s.camera2, true);
        s.second_handover.advance();
    }

    s.pass = 1u;
    s.draw_scene = guest.context()->gpr[3];
    // The game camera's half draws with it, except while it changes hands: after the second player's event
    // the first player's half holds its view until the game camera is back, then eases into it; at the start
    // of the event the second player's half eases from its own camera into the event's.
    coop::CameraMatrices game_half = s.camera1;
    if (events_swapped(s) && s.second_awaits_event) {
        const coop::Vec3 eye = coop::pose_of(s.camera1.inverse).eye;
        if (coop::length(eye - second_position) < coop::length(eye - first_position)) {
            const std::array<float, 16> lens = s.hold.projection();
            s.handover.start(s.follow_view, true, &lens);
            s.second_awaits_event = false;
            trace("the event camera has reached the second player");
        } else {
            game_half = s.follow_view;
        }
    }
    if (s.hold.active() && !events_swapped(s)) {
        // The first player's half, held through the second player's flight and until the game camera is back.
        const coop::CameraMatrices held = s.hold.view(first_position);
        if (s.hold.returning() && s.hold.back(s.camera1, first_position)) {
            const std::array<float, 16> held_lens = s.hold.projection();
            s.handover.start(held, false, &held_lens);
            trace("the game camera is back with the first player");
        } else {
            game_half = held;
        }
    }
    if (s.handover.active()) {
        game_half = s.handover.camera(game_half, second_half(s));
        s.handover.advance();
    }
    if (game_half.view != s.camera1.view) {
        SavedContext saved(guest.context());
        write_camera(guest, context, game_half);
        guest.call(fn::kLoadViewMtx);
        guest.call(fn::kCalcViewAndEntry);
    }
    s.second_view = second_half(s) ? game_half : s.camera2;
    if (s.camera_trace_frames > 0u && trace_enabled()) {
        --s.camera_trace_frames;
        const coop::Vec3 game_eye = coop::pose_of(s.camera1.inverse).eye;
        const coop::Vec3 drawn_eye = coop::pose_of(game_half.inverse).eye;
        const coop::Vec3 other_eye = coop::pose_of(s.camera2.inverse).eye;
        const coop::Vec3 drawn_fwd = coop::pose_of(game_half.inverse).forward;
        const coop::Vec3 other_fwd = coop::pose_of(s.camera2.inverse).forward;
        trace("cam f%u pass1=%s ev=%d hold=%d/%d ho=%d ho2=%d line=%.0f game(%.0f %.0f %.0f) "
              "pass1(%.0f %.0f %.0f >%.2f %.2f %.2f) pass2(%.0f %.0f %.0f >%.2f %.2f %.2f) fov00=%.3f",
              120u - s.camera_trace_frames, second_half(s) ? "second" : "first", s.event_second ? 1 : 0,
              s.hold.active() ? 1 : 0, s.hold.returning() ? 1 : 0, s.handover.active() ? 1 : 0,
              s.second_handover.active() ? 1 : 0, s.layout.angle() * 57.2958f, game_eye.x, game_eye.y, game_eye.z,
              drawn_eye.x, drawn_eye.y, drawn_eye.z, drawn_fwd.x, drawn_fwd.y, drawn_fwd.z, other_eye.x, other_eye.y,
              other_eye.z, other_fwd.x, other_fwd.y, other_fwd.z, s.projection[0]);
    }
    write_floats(guest, context + camera::kProjection, half_projection(s, second_half(s)));
    // The renderer keeps EFB copies per view: per player, so each half's effects see only its own picture
    // whichever pass draws it.
    send_split_clip(s.layout.plane(second_half(s), s.screen_width, s.screen_height), second_half(s) ? 2u : 1u);
}

// After the first half: the second, with the second player's camera, then the first player's camera back for
// the 2D layer and the next frame.
void finish_split_draw(const Guest& guest, State& s) {
    if (s.pass != 1u) return;
    SavedContext saved(guest.context());
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) {
        s.pass = 0u;
        return;
    }
    s.pass = 2u;
    write_camera(guest, context, s.camera2);
    write_floats(guest, context + camera::kProjection, half_projection(s, second_half(s)));
    guest.call(fn::kLoadViewMtx);
    guest.call(fn::kCalcViewAndEntry);
    send_split_clip(s.layout.plane(second_half(s), s.screen_width, s.screen_height), second_half(s) ? 2u : 1u);
    guest.call(fn::kGameSceneDraw3D, s.draw_scene);
    // The whole screen again for the 2D layer (HUD), and the black line between the halves, fading in with
    // the split.
    const std::array<float, 3> line = s.layout.plane(true, s.screen_width, s.screen_height);
    send_split_clip({0.0f, 0.0f, 0.0f}, 0u, {line[0], line[1], line[2], s.split.eased()});
    write_camera(guest, context, s.camera1);
    write_floats(guest, context + camera::kProjection, s.projection);
    guest.call(fn::kLoadViewMtx);
    s.pass = 0u;
    guest.call(fn::kSetDefaultViewportAndScissor);
}

double fpr(const PpcContext* context, unsigned index) { return std::bit_cast<double>(context->fpr_bits[index]); }

// After ShadowController::updateFarClipping: a shadow past the far clip from the first player's camera is
// drawn again if it is near enough to the second player's, so their side of a split screen has shadows.
void keep_second_player_shadow(const Guest& guest, State& s) {
    const std::uint32_t controller = s.shadow_controller;
    s.shadow_controller = 0u;
    if (!plausible(controller) || guest.u8(controller + kShadowFarClipped) == 0u) return;
    SavedContext saved(guest.context());
    Guest::Frame frame(guest);
    const std::uint32_t drop = frame.vec(0, {});
    guest.call(fn::kShadowGetDropPos, controller, drop);
    const coop::Vec3 position = guest.vec(drop);
    const float depth = -coop::transform_point(s.camera2.view, position).z;
    if (std::isfinite(depth) && depth < s.shadow_clip) guest.set_u8(controller + kShadowFarClipped, 0u);
}

void fill_partner_kpad(const Guest& guest, State& s) {
    if (!plausible(s.kpad_buffer)) return;
    const coop::KpadStatusBytes bytes = s.kpad.next(coop_partner_input());
    for (std::size_t at = 0; at < bytes.size(); at += 4u) {
        guest.set_u32(s.kpad_buffer + static_cast<std::uint32_t>(at),
                      (static_cast<std::uint32_t>(bytes[at]) << 24u) | (static_cast<std::uint32_t>(bytes[at + 1u]) << 16u) |
                          (static_cast<std::uint32_t>(bytes[at + 2u]) << 8u) | bytes[at + 3u]);
    }
    guest.context()->gpr[3] = 1u;  // one status read
}

}  // namespace

void coop_before_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                      std::uint32_t guest_pc) {
    if (services == nullptr || services->call_guest == nullptr || !coop_for_boot()) return;
    State& s = state();
    (void)memory;
    (void)guest_pc;
    switch (id) {
    case call::kKpadRead:
        s.kpad_channel = context->gpr[3];
        s.kpad_buffer = context->gpr[4];
        return;
    case call::kMarioActorInit2:
        if (s.init2_depth < s.init2_actors.size()) s.init2_actors[s.init2_depth] = context->gpr[3];
        ++s.init2_depth;
        return;
    case call::kCategoryListExecuteMovement:
        if (s.category_depth < s.categories.size()) s.categories[s.category_depth] = context->gpr[3];
        ++s.category_depth;
        return;
    case call::kMountArchive:
        if (!s.mounting_partner) {
            s.mount_path = context->gpr[3];
            s.mount_heap = context->gpr[4];
        }
        return;
    case call::kCreateAndAddPlayerArchives:
        s.player_archives_is_mario = context->gpr[4] & 0xFFu;
        return;
    case call::kPlayerHeapCreateHeap:
        // Both characters' player archives share PlayerHeapHolder's heaps (the stationed-archive loader
        // shrinks them to fit once loaded).
        if (context->gpr[3] == kPlayerHeapSize) context->gpr[3] = kPlayerHeapSize + kPartnerHeapExtra;
        return;
    case call::kGameSceneDraw3D:
        begin_split_draw(Guest(context, memory, services, guest_pc), s);
        return;
    case call::kClipFrustum:
        // ClippingJudge::isJudgedToClipFrustum(this, const TVec3f& position, f32 radius, s32 level).
        s.clip_position = s.split.amount() > 0.0f && s.follow.started() ? context->gpr[4] : 0u;
        s.clip_radius = static_cast<float>(fpr(context, 1));
        return;
    case call::kNameObjHolderCtor:
        // NameObjHolder::NameObjHolder(this, int num): the scene's (num 0x1300) gets room for the second player.
        if (context->gpr[4] >= coop::kSceneNameObjHolderMin) {
            s.scene_holder = context->gpr[3];
            s.holder_capacity = context->gpr[4];
            s.holder_count = 0u;
            context->gpr[4] = coop::scene_name_obj_capacity(context->gpr[4]);
            trace("scene object table: %u entries (game's %u)", context->gpr[4], s.holder_capacity);
        }
        return;
    case call::kNameObjHolderAdd:
        // NameObjHolder::add(this, NameObj*).
        if (context->gpr[3] == s.scene_holder && s.scene_holder != 0u && ++s.holder_count == s.holder_capacity + 1u) {
            trace("scene object table past the game's %u entries (the extra room is in use)", s.holder_capacity);
        }
        return;
    case call::kSetPlayerMode:
        // MarioActor::setPlayerMode(this, u16 mode, bool): the second player stays as they are rather than
        // take a power-up whose model was not loaded for their character (the game loads only the first
        // player's character's power-up archives; the empty model crashed).
        if (plausible(s.second) && context->gpr[3] == s.second) {
            const auto mode = static_cast<std::uint16_t>(context->gpr[4] & 0xFFFFu);
            const int slot = coop::power_up_model_slot(mode);
            const Guest guest(context, memory, services, guest_pc);
            const bool missing = slot == coop::kTeresaParts ? guest.u32(s.second + kActorTeresaParts) == 0u
                                 : slot >= 0 ? guest.u32(s.second + kActorModels + 4u * static_cast<std::uint32_t>(slot)) == 0u
                                             : false;
            if (missing) {
                trace("second player's power-up %u has no model: not taken", mode);
                context->gpr[4] = 0u;
            }
        }
        return;
    case call::kShadowFarClipping:
        // ShadowController::updateFarClipping(this, f32 clip).
        s.shadow_controller = s.split.amount() > 0.0f && s.follow.started() ? context->gpr[3] : 0u;
        s.shadow_clip = static_cast<float>(fpr(context, 1));
        return;
    default:
        return;
    }
}

void coop_after_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                     std::uint32_t guest_pc) {
    if (services == nullptr || services->call_guest == nullptr || !coop_for_boot()) return;
    State& s = state();
    const Guest guest(context, memory, services, guest_pc);
    switch (id) {
    case call::kKpadRead:
        if (s.kpad_channel == 1u) fill_partner_kpad(guest, s);
        s.kpad_channel = 0xFFFFFFFFu;
        return;
    case call::kGameDataHolderIsDataMario:
    case call::kGameDataFunctionIsDataMario:
        if (s.answer_partner) {
            context->gpr[3] = coop::partner_of(s.own) == coop::Character::Mario ? 1u : 0u;
        } else {
            s.own = (context->gpr[3] & 0xFFu) != 0u ? coop::Character::Mario : coop::Character::Luigi;
        }
        return;
    case call::kMarioActorInit2: {
        if (s.init2_depth == 0u) return;
        --s.init2_depth;
        const std::uint32_t actor = s.init2_depth < s.init2_actors.size() ? s.init2_actors[s.init2_depth] : 0u;
        if (s.creating_second || !plausible(actor)) return;
        create_second_player(guest, s, actor);
        return;
    }
    case call::kCategoryListExecuteMovement: {
        if (s.category_depth == 0u) return;
        --s.category_depth;
        const std::uint32_t category = s.category_depth < s.categories.size() ? s.categories[s.category_depth] : 0u;
        if (category != kMovementTypePlayer || s.moving_second || !plausible(s.first) || !plausible(s.second)) return;
        move_second_player(guest, s);
        return;
    }
    case call::kMountArchive:
        if (!s.mounting_partner) mount_partner_archives(guest, s);
        return;
    case call::kCreateAndAddPlayerArchives:
        add_partner_resources(guest, s);
        return;
    case call::kGameSceneDraw3D:
        finish_split_draw(guest, s);
        return;
    case call::kClipFrustum:
        // Out of the first player's view but in the second's: not clipped, so it moves and is drawn.
        if (s.clip_position != 0u && (context->gpr[3] & 0xFFu) != 0u && plausible(s.clip_position)) {
            const coop::Vec3 position = guest.vec(s.clip_position);
            if (coop::sphere_in_view(s.camera2.view, position, s.clip_radius, s.fovy, s.aspect, s.far_z)) {
                context->gpr[3] = 0u;
            }
        }
        s.clip_position = 0u;
        return;
    case call::kShadowFarClipping:
        if (s.shadow_controller != 0u) keep_second_player_shadow(guest, s);
        return;
    default:
        return;
    }
}

unsigned coop_split_pass() noexcept { return state().pass; }

void coop_set_split_clip_sink(SplitClipSink sink, void* user) noexcept {
    split_clip_sink() = {sink, user};
}

bool coop_is_second_player_mario(std::uint32_t mario) noexcept {
    const State& s = state();
    return mario != 0u && mario == s.second_mario;
}


}  // namespace galaxy
