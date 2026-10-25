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
}  // namespace fn

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
            base_ = (sp_ - 0x60u) & ~0xFu;
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

// Restores the interrupted game call's registers when a hook has called into the game itself.
class SavedContext {
public:
    explicit SavedContext(PpcContext* context) : context_(context), saved_(*context) {}
    ~SavedContext() { *context_ = saved_; }
    SavedContext(const SavedContext&) = delete;
    SavedContext& operator=(const SavedContext&) = delete;

private:
    PpcContext* context_;
    PpcContext saved_;
};

struct PartnerInputSlot {
    std::mutex mutex;
    coop::PartnerInput input;
};

PartnerInputSlot& partner_input_slot() {
    static PartnerInputSlot slot;
    return slot;
}

coop::PartnerInput partner_input() {
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    return slot.input;
}

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
    coop::KpadSynth kpad;
    coop::LeashTimer leash;
    unsigned frames = 0u;
};

State& state() {
    static State s;
    return s;
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
    const std::uint32_t first_mario = guest.u32(first + actor::kMario);
    if (!plausible(first_mario)) return;
    const coop::Vec3 position = guest.vec(first + actor::kPosition);
    const coop::Vec3 rotation = guest.vec(first + actor::kRotation);
    const coop::Vec3 side = guest.vec(first_mario + kMarioSideVec);
    const coop::Vec3 beside = position + side * 120.0f;

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
        return;
    }
    const std::uint32_t pad_holder = guest.call(fn::kGetWPadHolder);
    const bool pads = plausible(pad_holder) && plausible(guest.u32(pad_holder)) && plausible(guest.u32(pad_holder + 4u));

    const coop::Character partner = coop::partner_of(s.own);
    s.moving_second = true;
    guest.set_u8(kIsLuigi, character_byte(partner));
    guest.call(fn::kSetMarioActor, holder, s.second);
    if (pads) swap_pads(guest, pad_holder);
    guest.call(fn::kMarioActorMovement, s.second);

    const coop::Vec3 first_position = guest.vec(s.first + actor::kPosition);
    const coop::Vec3 second_position = guest.vec(s.second + actor::kPosition);
    const coop::CameraShare share = coop::shared_camera(first_position, second_position);
    if (s.leash.update(share.out_of_range, coop::CameraTuning{}.warp_frames)) {
        // Still the second player's turn: MR::setPlayerPos moves the scene's player.
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, first_position));
        trace("second player brought back to the first");
    }

    if (pads) swap_pads(guest, pad_holder);
    guest.call(fn::kSetMarioActor, holder, s.first);
    guest.set_u8(kIsLuigi, character_byte(s.own));
    s.moving_second = false;

    // The game camera looks at the first player's camera point; move it towards the second player.
    const coop::Vec3 look = guest.vec(s.first + actor::kCameraTrans);
    guest.set_vec(s.first + actor::kCameraTrans, look + share.offset);

    if (trace_enabled() && (++s.frames % 120u) == 0u) {
        trace("first (%.0f %.0f %.0f) second (%.0f %.0f %.0f) camera offset (%.0f %.0f %.0f)%s", first_position.x,
              first_position.y, first_position.z, second_position.x, second_position.y, second_position.z,
              share.offset.x, share.offset.y, share.offset.z, share.out_of_range ? " out of range" : "");
    }
}

void fill_partner_kpad(const Guest& guest, State& s) {
    if (!plausible(s.kpad_buffer)) return;
    const coop::KpadStatusBytes bytes = s.kpad.next(partner_input());
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
    default:
        return;
    }
}

bool coop_is_second_player_mario(std::uint32_t mario) noexcept {
    const State& s = state();
    return mario != 0u && mario == s.second_mario;
}

void coop_publish_partner_input(const coop::PartnerInput& input) noexcept {
    PartnerInputSlot& slot = partner_input_slot();
    std::lock_guard lock(slot.mutex);
    slot.input = input;
}

}  // namespace galaxy
