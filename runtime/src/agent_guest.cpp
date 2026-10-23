#include "galaxy/agent.h"
#include "galaxy/coop_guest.h"
#include "galaxy/gx/gx_backend.h"
#include "galaxy/netplay.h"

#include "guest_access.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <deque>
#include <optional>
#include <string>

namespace galaxy {
namespace {

using guest_access::Guest;
using guest_access::plausible;
using guest_access::SavedContext;

// Agent play testing (agent.h, docs/AGENT_TESTING.md): the game's side of the command folder. Runs on the game
// thread at the first player's controller read (KPADRead channel 0), once a frame, in every scene.
//
// RMGE01 addresses and layouts are from Petari (symbols.txt, GameSystem.hpp, GameSystemSceneController.hpp,
// MarioActor.hpp).

namespace fn {
constexpr std::uint32_t kGetMarioHolder = 0x802E1528u;   // MR::getMarioHolder()
constexpr std::uint32_t kGetMarioActor = 0x802E1520u;    // MarioHolder::getMarioActor() const
constexpr std::uint32_t kSetPlayerPos = 0x803F1F80u;     // MR::setPlayerPos(const TVec3f&)
constexpr std::uint32_t kGetAllLiveActorGroup = 0x8015CDB8u;  // MR::getAllLiveActorGroup()
constexpr std::uint32_t kIsDead = 0x803DD748u;                // MR::isDead(const LiveActor*)
constexpr std::uint32_t kSetPlayerMode = 0x802C42C0u;    // MarioActor::setPlayerMode(u16, bool)
constexpr std::uint32_t kForceGameOver = 0x802B4EB8u;    // MarioActor::forceGameOver()
constexpr std::uint32_t kGetPlayerLeft = 0x803CC3CCu;    // MR::getPlayerLeft()
constexpr std::uint32_t kIsOpeningCamera = 0x8033F9F4u;  // GameSceneFunction::isExecScenarioOpeningCamera()
constexpr std::uint32_t kIsDemoActive = 0x803C9E84u;     // MR::isDemoActive()
constexpr std::uint32_t kIsInputDisable = 0x802E98B8u;   // MarioModule::isInputDisable() const (on a Mario)
constexpr std::uint32_t kIsPlayerDead = 0x803F1EA4u;     // MR::isPlayerDead()
constexpr std::uint32_t kIsEventCameraActive = 0x803C799Cu;  // MR::isEventCameraActive(): intros, cutscenes
constexpr std::uint32_t kGetFreeSize = 0x80409794u;      // JKRHeap::getFreeSize()
// MR::requestChangeStageInGameMoving(const char* stage, s32 scenario): the game copies the name
// (SceneControlInfo::setStage), so a string on the guest stack will do.
constexpr std::uint32_t kRequestChangeStage = 0x803F8328u;
// A save file as the file select starts it: the save files read from NAND (startPreLoadSaveDataSequence, which the
// file select runs after the title; before it every file is empty and loading one starts a new game), then
// GameSequenceFunction::startGameDataLoadSequence(int file, bool mario),
// then, once isActiveSaveDataHandleSequence() is over and isSuccessSaveDataHandleSequence(),
// MR::requestChangeStageInGameAfterLoadingGameData().
constexpr std::uint32_t kStartPreLoadSaveData = 0x803B4D3Cu;  // GameSequenceFunction::startPreLoadSaveDataSequence()
constexpr std::uint32_t kStartGameDataLoad = 0x803B4D84u;
constexpr std::uint32_t kIsSaveSequenceActive = 0x803B4F84u;
constexpr std::uint32_t kIsSaveSequenceSuccess = 0x803B4FA8u;
constexpr std::uint32_t kChangeStageAfterLoading = 0x803F81E8u;
// MR::onGameEventFlag*MarioAtFirst() for modes 1-7 (rainbow, fire, ice, bee, spring, boo, fly): a power-up the save
// has not had yet plays its first-time demo, which turns the player to the item they touched (MarioActor::_4A4,
// a HitSensor); a power-up given without an item had none and crashed there.
constexpr std::array<std::uint32_t, 8> kPowerUpSeen = {0u,          0x803CC5FCu, 0x803CC5D8u, 0x803CC5E4u,
                                                      0x803CC5B4u, 0x803CC5CCu, 0x803CC5C0u, 0x803CC5F0u};
}  // namespace fn

// SingletonHolder<HeapMemoryWatcher>::sInstance: its heaps, stationed (+0, +4), game (+8, +0xC) and the stage's
// (+0x14, +0x18), each MEM1 then MEM2.
constexpr std::uint32_t kHeapMemoryWatcher = 0x806A245Cu;
constexpr std::uint32_t kGameSystem = 0x806A1228u;  // SingletonHolder<GameSystem>::sInstance
constexpr std::uint32_t kGameSystemSceneController = 0x24u;
// GameSystemSceneController: mCurrSceneControlInfo (mScene[32], mStage[32], mScenarioNo) and the scene's
// initialisation state (SceneInitializeState_End = 6 once the scene runs).
constexpr std::uint32_t kSceneName = 0x00u;
constexpr std::uint32_t kStageName = 0x20u;
constexpr std::uint32_t kScenarioNo = 0x40u;
constexpr std::uint32_t kInitializeState = 0xB0u;
constexpr std::uint32_t kInitializeEnd = 6u;

namespace actor {
constexpr std::uint32_t kPosition = 0x00Cu;     // LiveActor::mPosition
constexpr std::uint32_t kMario = 0x230u;        // MarioActor::mMario
constexpr std::uint32_t kHealth = 0x380u;       // MarioActor::mHealth (u32)
constexpr std::uint32_t kPlayerMode = 0x3D4u;   // MarioActor::mPlayerMode (u16)
constexpr std::uint32_t kTeresaParts = 0x9A4u;  // MarioActor::_9A4: the Boo form's parts
constexpr std::uint32_t kModels = 0xA28u;       // MarioActor::mModels[6]: the power-up models
}  // namespace actor

struct Scene {
    std::string scene;
    std::string stage;
    std::int32_t scenario = 0;
    std::uint32_t initialize = 0u;
    bool in_game = false;  // the Game scene, fully set up: its player can be asked for
};

struct Player {
    std::uint32_t actor = 0u;
    coop::Vec3 position{};
    std::uint32_t mode = 0u;
    std::uint32_t health = 0u;
    bool input_disabled = false;
};

struct PadTimer {
    agent::Pad pad;
    std::uint64_t buttons_until = 0u;
    std::uint64_t stick_until = 0u;
};

struct Agent {
    bool enabled = false;
    std::string directory;
    std::uint64_t frame = 0u;
    long long read_offset = 0;
    std::string partial;
    std::deque<agent::Command> queue;
    // A command the queue waits for: `wait`, or a `press`/`stick` without "nowait".
    bool blocked = false;
    std::uint64_t blocked_until = 0u;
    std::uint64_t blocked_id = 0u;
    std::array<PadTimer, 2> pads{};
    // A `file` command in progress: waiting for the file select (phase 0), the save files to be read (1), the
    // file to load (2).
    bool file_pending = false;
    std::uint64_t file_id = 0u;
    std::uint32_t file_index = 0u;
    bool file_mario = true;
    int file_phase = 0;
    std::uint64_t file_started = 0u;
    std::uint64_t file_settle = 0u;
    std::uint64_t last_status_ms = 0u;
};

Agent& agent_state() {
    static Agent a = [] {
        Agent made;
        char* value = nullptr;
        std::size_t length = 0u;
        if (_dupenv_s(&value, &length, "GALAXY_AGENT_DIR") == 0 && value != nullptr && value[0] != '\0') {
            made.enabled = true;
            made.directory = value;
            std::fprintf(stderr, "[agent] commands from %s\\commands.txt\n", value);
        }
        std::free(value);
        return made;
    }();
    return a;
}

std::string path_in(const Agent& a, const char* name) {
    return a.directory + "\\" + name;
}

void reply(const Agent& a, std::uint64_t id, bool ok, const std::string& fields) {
    const std::string path = path_in(a, "replies.jsonl");
    // Shared (fopen_s opens without sharing): the driver may be reading the file just then, and a reply that
    // cannot be written is lost. A few tries for any other holder.
    FILE* file = nullptr;
    for (int attempt = 0; attempt < 20 && file == nullptr; ++attempt) {
        file = _fsopen(path.c_str(), "ab", _SH_DENYNO);
        if (file == nullptr) Sleep(1);
    }
    if (file == nullptr) return;
    std::fprintf(file, "{\"id\":%llu,\"ok\":%s,\"frame\":%llu%s%s}\n", static_cast<unsigned long long>(id),
                 ok ? "true" : "false", static_cast<unsigned long long>(a.frame), fields.empty() ? "" : ",",
                 fields.c_str());
    std::fclose(file);
}

void fail(const Agent& a, const agent::Command& command, const char* why) {
    reply(a, command.id, false, std::string("\"error\":\"") + agent::json_escape(why) + "\"");
}

// New complete lines of commands.txt, which the driver only appends to.
void read_commands(Agent& a) {
    const std::string path = path_in(a, "commands.txt");
    FILE* file = _fsopen(path.c_str(), "rb", _SH_DENYNO);
    if (file == nullptr) return;
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return;
    }
    const long long size = _ftelli64(file);
    if (size < a.read_offset) a.read_offset = 0;  // a new file
    if (size > a.read_offset && _fseeki64(file, a.read_offset, SEEK_SET) == 0) {
        std::string text(static_cast<std::size_t>(size - a.read_offset), '\0');
        const std::size_t got = std::fread(text.data(), 1u, text.size(), file);
        text.resize(got);
        a.read_offset += static_cast<long long>(got);
        a.partial += text;
        std::size_t start = 0u;
        for (std::size_t end = a.partial.find('\n'); end != std::string::npos; end = a.partial.find('\n', start)) {
            if (auto command = agent::parse_command(std::string_view(a.partial).substr(start, end - start))) {
                a.queue.push_back(std::move(*command));
            }
            start = end + 1u;
        }
        a.partial.erase(0u, start);
    }
    std::fclose(file);
}

Scene read_scene(const Guest& guest) {
    Scene scene;
    const std::uint32_t system = guest.u32(kGameSystem);
    if (!plausible(system)) return scene;
    const std::uint32_t controller = guest.u32(system + kGameSystemSceneController);
    if (!plausible(controller)) return scene;
    scene.scene = guest.string(controller + kSceneName, 31u);
    scene.stage = guest.string(controller + kStageName, 31u);
    scene.scenario = static_cast<std::int32_t>(guest.u32(controller + kScenarioNo));
    scene.initialize = guest.u32(controller + kInitializeState);
    scene.in_game = scene.scene == "Game" && scene.initialize == kInitializeEnd;
    return scene;
}

std::uint32_t first_player(const Guest& guest) {
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    return plausible(holder) ? guest.call(fn::kGetMarioActor, holder) : 0u;
}

Player read_player(const Guest& guest, std::uint32_t actor_address) {
    Player player;
    if (!plausible(actor_address)) return player;
    player.actor = actor_address;
    player.position = guest.vec(actor_address + actor::kPosition);
    player.mode = guest.u32(actor_address + actor::kPlayerMode) >> 16u;
    player.health = guest.u32(actor_address + actor::kHealth);
    const std::uint32_t mario = guest.u32(actor_address + actor::kMario);
    player.input_disabled = plausible(mario) && (guest.call(fn::kIsInputDisable, mario) & 0xFFu) != 0u;
    return player;
}

std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", std::isfinite(value) ? value : 0.0f);
    return text;
}

std::string player_json(const Player& player, unsigned index, int lives) {
    if (player.actor == 0u) return "null";
    const coop::DownState down = coop_down_state(index);
    return std::string("{\"x\":") + number(player.position.x) + ",\"y\":" + number(player.position.y) +
           ",\"z\":" + number(player.position.z) + ",\"powerup\":\"" + agent::power_up_name(player.mode) +
           "\",\"health\":" + std::to_string(player.health) +
           ",\"input_disabled\":" + (player.input_disabled ? "true" : "false") + ",\"lives\":" +
           std::to_string(lives) + ",\"down\":" + (down.down ? "true" : "false") + ",\"out\":" +
           (down.out ? "true" : "false") + "}";
}

// Free space in one of HeapMemoryWatcher's heaps (`field`: its offset there), in KB; -1 without one.
std::string heap_free_kb(const Guest& guest, std::uint32_t field) {
    const std::uint32_t watcher = guest.u32(kHeapMemoryWatcher);
    const std::uint32_t heap = plausible(watcher) ? guest.u32(watcher + field) : 0u;
    if (heap == 0u || !(plausible(heap) || (heap >= 0x90000000u && heap < 0x94000000u))) return "-1";
    return std::to_string(guest.call(fn::kGetFreeSize, heap) / 1024u);
}

// The scene and, in a game scene, the players: the `state` reply and status.json.
std::string state_json(const Guest& guest) {
    const Scene scene = read_scene(guest);
    std::string out = "\"scene\":\"" + agent::json_escape(scene.scene) + "\",\"stage\":\"" +
                      agent::json_escape(scene.stage) + "\",\"mission\":" + std::to_string(scene.scenario) +
                      ",\"in_game\":" + (scene.in_game ? "true" : "false");
    if (!scene.in_game) return out;
    SavedContext saved(guest.context());
    const std::uint32_t first_actor = first_player(guest);
    const Player first = read_player(guest, first_actor);
    const Player second = read_player(guest, coop_second_player(first_actor));
    const bool demo = (guest.call(fn::kIsDemoActive) & 0xFFu) != 0u;
    const bool dead = first.actor != 0u && (guest.call(fn::kIsPlayerDead) & 0xFFu) != 0u;
    const bool event_camera = (guest.call(fn::kIsEventCameraActive) & 0xFFu) != 0u;
    const bool intro = (guest.call(fn::kIsOpeningCamera) & 0xFFu) != 0u;
    out += ",\"demo\":" + std::string(demo ? "true" : "false") + ",\"dead\":" + (dead ? "true" : "false") +
           ",\"event_camera\":" + (event_camera ? "true" : "false") + ",\"intro\":" + (intro ? "true" : "false") +
           ",\"heap_free_kb\":{\"stationed_mem1\":" + heap_free_kb(guest, 0x00u) + ",\"stationed_mem2\":" +
           heap_free_kb(guest, 0x04u) + ",\"game_mem1\":" + heap_free_kb(guest, 0x08u) + ",\"game_mem2\":" +
           heap_free_kb(guest, 0x0Cu) + ",\"scene_mem1\":" + heap_free_kb(guest, 0x14u) + ",\"scene_mem2\":" +
           heap_free_kb(guest, 0x18u) + "}" +
           ",\"players\":[" + player_json(first, 0u, static_cast<int>(guest.call(fn::kGetPlayerLeft))) + "," +
           player_json(second, 1u, coop_second_lives()) + "]";
    return out;
}

void write_status(const Agent& a, const Guest& guest) {
    const std::string body = "{\"frame\":" + std::to_string(a.frame) + "," + state_json(guest) + "}\n";
    const std::string path = path_in(a, "status.json");
    const std::string temporary = path + ".tmp";
    FILE* file = nullptr;
    if (fopen_s(&file, temporary.c_str(), "wb") != 0 || file == nullptr) return;
    std::fwrite(body.data(), 1u, body.size(), file);
    std::fclose(file);
    MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool has_power_up_model(const Guest& guest, std::uint32_t actor_address, std::uint16_t mode) {
    const int slot = coop::power_up_model_slot(mode);
    if (slot == coop::kTeresaParts) return guest.u32(actor_address + actor::kTeresaParts) != 0u;
    if (slot >= 0) return guest.u32(actor_address + actor::kModels + 4u * static_cast<std::uint32_t>(slot)) != 0u;
    return true;
}

bool has_arg(const agent::Command& command, const char* word) {
    for (const auto& arg : command.args) {
        if (arg == word) return true;
    }
    return false;
}

// Runs one command and answers it, or blocks the queue until it is done (wait, press, stick).
void run(Agent& a, const Guest& guest, const agent::Command& command) {
    using agent::Verb;
    const auto& args = command.args;
    switch (command.verb) {
    case Verb::Ping:
        reply(a, command.id, true, "");
        return;
    case Verb::State:
        reply(a, command.id, true, state_json(guest));
        return;
    case Verb::Wait: {
        const auto frames = args.size() == 1u ? agent::parse_int(args[0]) : std::nullopt;
        if (!frames || *frames < 0) return fail(a, command, "wait <frames>");
        a.blocked = true;
        a.blocked_until = a.frame + static_cast<std::uint64_t>(*frames);
        a.blocked_id = command.id;
        return;
    }
    case Verb::Press:
    case Verb::Stick: {
        const bool press = command.verb == Verb::Press;
        const std::size_t needed = press ? 3u : 4u;
        const auto player = args.size() >= needed ? agent::parse_player(args[0]) : std::nullopt;
        if (!player) return fail(a, command, press ? "press <1|2> <A+B..> <frames> [nowait]"
                                                   : "stick <1|2> <x> <y> <frames> [nowait]");
        PadTimer& timer = a.pads[*player];
        const auto frames = agent::parse_int(args[needed - 1u]);
        if (!frames || *frames <= 0) return fail(a, command, "frames must be a positive number");
        if (press) {
            const auto buttons = agent::parse_buttons(args[1]);
            if (!buttons) return fail(a, command, "buttons: A B 1 2 PLUS MINUS UP DOWN LEFT RIGHT C Z HOME SHAKE joined with +");
            timer.pad.buttons = *buttons & ~agent::kShake;
            timer.pad.shake = (*buttons & agent::kShake) != 0u;
            timer.buttons_until = a.frame + static_cast<std::uint64_t>(*frames);
        } else {
            const auto x = agent::parse_float(args[1]);
            const auto y = agent::parse_float(args[2]);
            if (!x || !y) return fail(a, command, "stick axes from -1 to 1");
            timer.pad.stick_x = std::clamp(*x, -1.0f, 1.0f);
            timer.pad.stick_y = std::clamp(*y, -1.0f, 1.0f);
            timer.stick_until = a.frame + static_cast<std::uint64_t>(*frames);
        }
        timer.pad.active = true;
        agent_set_pad(*player, timer.pad);
        if (has_arg(command, "nowait")) {
            reply(a, command.id, true, "");
        } else {
            a.blocked = true;
            a.blocked_until = a.frame + static_cast<std::uint64_t>(*frames);
            a.blocked_id = command.id;
        }
        return;
    }
    case Verb::Pointer: {
        // "pointer [2] <x> <y>" | "pointer [2] off": the second player's is their orange Star Pointer.
        std::vector<std::string> rest(args.begin(), args.end());
        unsigned player = 0u;
        if ((rest.size() == 3u || (rest.size() == 2u && rest[1] == "off")) && (rest[0] == "1" || rest[0] == "2")) {
            player = rest[0] == "2" ? 1u : 0u;
            rest.erase(rest.begin());
        }
        PadTimer& timer = a.pads[player];
        if (rest.size() == 1u && rest[0] == "off") {
            timer.pad.has_pointer = false;
        } else {
            const auto x = rest.size() == 2u ? agent::parse_float(rest[0]) : std::nullopt;
            const auto y = rest.size() == 2u ? agent::parse_float(rest[1]) : std::nullopt;
            if (!x || !y) return fail(a, command, "pointer [1|2] <x> <y> (-1..1) | pointer [1|2] off");
            timer.pad.has_pointer = true;
            timer.pad.pointer_x = std::clamp(*x, -1.0f, 1.0f);
            timer.pad.pointer_y = std::clamp(*y, -1.0f, 1.0f);
        }
        timer.pad.active = true;
        agent_set_pad(player, timer.pad);
        reply(a, command.id, true, "");
        return;
    }
    case Verb::Release: {
        const auto player = args.size() == 1u ? agent::parse_player(args[0]) : std::nullopt;
        if (!player) return fail(a, command, "release <1|2>");
        a.pads[*player] = PadTimer{};
        agent_set_pad(*player, agent::Pad{});
        reply(a, command.id, true, "");
        return;
    }
    case Verb::File: {
        const auto file = !args.empty() ? agent::parse_int(args[0]) : std::nullopt;
        if (!file || *file < 1 || *file > 6 || args.size() > 2u || (args.size() == 2u && args[1] != "luigi")) {
            return fail(a, command, "file <1-6> [luigi]");
        }
        a.file_pending = true;
        a.file_id = command.id;
        a.file_index = static_cast<std::uint32_t>(*file);  // 1-based, as FileSelector passes it
        a.file_mario = args.size() < 2u;
        a.file_phase = 0;
        a.file_started = a.frame;
        a.file_settle = 0u;
        a.blocked = true;  // answered by update_file
        a.blocked_until = ~std::uint64_t{0};
        a.blocked_id = 0u;
        return;
    }
    case Verb::Stage: {
        if (args.size() != 2u) return fail(a, command, "stage <StageName> <mission>");
        const std::string& stage = args[0];
        const auto mission = agent::parse_int(args[1]);
        bool name_ok = !stage.empty() && stage.size() < 32u;
        for (const char c : stage) name_ok = name_ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (!name_ok || !mission || *mission < 1 || *mission > 8) {
            return fail(a, command, "stage <StageName> <mission 1-8>, e.g. stage StarDustGalaxy 1");
        }
        if (!read_scene(guest).in_game) return fail(a, command, "not in a game scene yet (boot first)");
        SavedContext saved(guest.context());
        Guest::Frame frame(guest);
        const std::uint32_t text = frame.scratch();
        for (std::size_t i = 0; i <= stage.size(); ++i) {
            guest.set_u8(text + static_cast<std::uint32_t>(i), i < stage.size() ? static_cast<std::uint8_t>(stage[i]) : 0u);
        }
        guest.call(fn::kRequestChangeStage, text, static_cast<std::uint32_t>(*mission));
        std::fprintf(stderr, "[agent] stage %s mission %lld requested\n", stage.c_str(), *mission);
        reply(a, command.id, true, "");
        return;
    }
    case Verb::PowerUp: {
        const auto player = !args.empty() ? agent::parse_player(args[0]) : std::nullopt;
        const auto mode = args.size() >= 2u ? agent::parse_power_up(args[1]) : std::nullopt;
        if (!player || !mode) return fail(a, command, "powerup <1|2> <normal|rainbow|fire|ice|bee|spring|boo|fly> [force]");
        if (!read_scene(guest).in_game) return fail(a, command, "not in a game scene");
        SavedContext saved(guest.context());
        const std::uint32_t first = first_player(guest);
        const std::uint32_t target = *player == 0u ? first : coop_second_player(first);
        if (!plausible(target)) return fail(a, command, "no such player");
        if (!has_arg(command, "force") && !has_power_up_model(guest, target, *mode)) {
            return fail(a, command, "that power-up's model is not loaded in this stage (add force to try anyway)");
        }
        // As in play (coop_guest.cpp): never both players Boos.
        const std::uint32_t other = *player == 0u ? coop_second_player(first) : first;
        if (*mode == 6u && plausible(other) && (guest.u32(other + actor::kPlayerMode) >> 16u) == 6u) {
            return fail(a, command, "the other player is a Boo");
        }
        if (*mode < fn::kPowerUpSeen.size() && fn::kPowerUpSeen[*mode] != 0u) guest.call(fn::kPowerUpSeen[*mode]);
        if (*player == 0u) {
            guest.call(fn::kSetPlayerMode, target, *mode, 1u);
        } else {
            coop_request_second_player_mode(*mode);
        }
        reply(a, command.id, true, "");
        return;
    }
    case Verb::Teleport: {
        const auto player = args.size() == 4u ? agent::parse_player(args[0]) : std::nullopt;
        const auto x = args.size() == 4u ? agent::parse_float(args[1]) : std::nullopt;
        const auto y = args.size() == 4u ? agent::parse_float(args[2]) : std::nullopt;
        const auto z = args.size() == 4u ? agent::parse_float(args[3]) : std::nullopt;
        if (!player || !x || !y || !z) return fail(a, command, "teleport <1|2> <x> <y> <z>");
        if (!read_scene(guest).in_game) return fail(a, command, "not in a game scene");
        const coop::Vec3 there{*x, *y, *z};
        if (*player == 1u) {
            SavedContext saved(guest.context());
            if (coop_second_player(first_player(guest)) == 0u) return fail(a, command, "no second player");
            coop_request_second_player_teleport(there);
        } else {
            SavedContext saved(guest.context());
            Guest::Frame frame(guest);
            guest.call(fn::kSetPlayerPos, frame.vec(0, there));
        }
        reply(a, command.id, true, "");
        return;
    }
    case Verb::Kill: {
        const auto player = args.size() == 1u ? agent::parse_player(args[0]) : std::nullopt;
        if (!player) return fail(a, command, "kill <1|2>");
        if (!read_scene(guest).in_game) return fail(a, command, "not in a game scene");
        SavedContext saved(guest.context());
        const std::uint32_t first = first_player(guest);
        const std::uint32_t target = *player == 0u ? first : coop_second_player(first);
        if (!plausible(target)) return fail(a, command, "no such player");
        guest.set_u32(target + actor::kHealth, 0u);
        guest.call(fn::kForceGameOver, target);
        reply(a, command.id, true, "");
        return;
    }
    case Verb::Peek: {
        const auto address = !args.empty() ? agent::parse_word(args[0]) : std::nullopt;
        const auto count = args.size() >= 2u ? agent::parse_word(args[1]) : std::optional<std::uint32_t>{1u};
        if (!address || !count || *count == 0u || *count > 256u || (*address & 3u) != 0u ||
            !(plausible(*address) || (*address >= 0x90000000u && *address < 0x94000000u))) {
            return fail(a, command, "peek <aligned guest address> [words 1-256]");
        }
        std::string words;
        char text[16];
        for (std::uint32_t i = 0; i < *count; ++i) {
            std::snprintf(text, sizeof(text), "%s\"%08X\"", i == 0u ? "" : ",", guest.u32(*address + 4u * i));
            words += text;
        }
        reply(a, command.id, true, "\"words\":[" + words + "]");
        return;
    }
    case Verb::Actors: {
        // MR::getAllLiveActorGroup(): NameObjGroup +0x10 count, +0x14 NameObj**; NameObj +0x4 name,
        // LiveActor +0xC position. Names are the game's (Shift-JIS shows as '?').
        if (args.empty()) return fail(a, command, "actors <name part> [max]");
        const auto most = args.size() >= 2u ? agent::parse_word(args[1]) : std::optional<std::uint32_t>{40u};
        if (!most) return fail(a, command, "actors <name part> [max]");
        SavedContext saved(guest.context());
        const std::uint32_t group = guest.call(fn::kGetAllLiveActorGroup);
        if (!plausible(group)) return fail(a, command, "no actor group (not in a stage?)");
        const std::uint32_t count = std::min(guest.u32(group + 0x10u), 4096u);
        const std::uint32_t objects = guest.u32(group + 0x14u);
        if (!plausible(objects)) return fail(a, command, "no actor list");
        std::string list;
        std::uint32_t listed = 0u;
        for (std::uint32_t i = 0u; i < count && listed < *most; ++i) {
            const std::uint32_t actor = guest.u32(objects + 4u * i);
            if (!plausible(actor)) continue;
            const std::uint32_t name_at = guest.u32(actor + 4u);
            std::string name = plausible(name_at) ? guest.string(name_at, 47u) : std::string{};
            for (char& c : name) {
                if (static_cast<unsigned char>(c) >= 0x80u) c = '?';
            }
            if (args[0] != "." && name.find(args[0]) == std::string::npos) continue;
            const coop::Vec3 at = guest.vec(actor + 0xCu);
            char text[64];
            std::snprintf(text, sizeof(text), "\",\"address\":\"%08X\",\"vtable\":\"%08X\",\"x\":", actor,
                          guest.u32(actor));
            list += std::string(listed == 0u ? "" : ",") + "{\"name\":\"" + agent::json_escape(name) + text +
                    number(at.x) + ",\"y\":" + number(at.y) + ",\"z\":" + number(at.z) + ",\"dead\":" +
                    ((guest.call(fn::kIsDead, actor) & 0xFFu) != 0u ? "true" : "false") + "}";
            ++listed;
        }
        reply(a, command.id, true, "\"actors\":[" + list + "]");
        return;
    }
    case Verb::Poke: {
        const auto address = args.size() == 2u ? agent::parse_word(args[0]) : std::nullopt;
        const auto value = args.size() == 2u ? agent::parse_word(args[1]) : std::nullopt;
        if (!address || !value || (*address & 3u) != 0u ||
            !(plausible(*address) || (*address >= 0x90000000u && *address < 0x94000000u))) {
            return fail(a, command, "poke <aligned guest address> <word>");
        }
        guest.set_u32(*address, *value);
        reply(a, command.id, true, "");
        return;
    }
    case Verb::Call: {
        std::array<std::uint32_t, 5> words{};
        bool ok = !args.empty() && args.size() <= 5u;
        for (std::size_t i = 0; ok && i < args.size(); ++i) {
            const auto word = agent::parse_word(args[i]);
            ok = word.has_value();
            if (ok) words[i] = *word;
        }
        if (!ok || words[0] < 0x80004000u || words[0] >= 0x80600000u || (words[0] & 3u) != 0u) {
            return fail(a, command, "call <function address> [r3 r4 r5 r6]");
        }
        SavedContext saved(guest.context());
        const std::uint32_t result = guest.call(words[0], words[1], words[2], words[3], words[4]);
        char text[32];
        std::snprintf(text, sizeof(text), "\"r3\":\"%08X\"", result);
        reply(a, command.id, true, text);
        return;
    }
    case Verb::Screenshot: {
        if (args.size() != 1u) return fail(a, command, "screenshot <path.ppm>");
        gx::request_present_snapshot(args[0]);
        reply(a, command.id, true, "\"path\":\"" + agent::json_escape(args[0]) + "\"");
        return;
    }
    case Verb::Quit:
        reply(a, command.id, true, "");
        std::fprintf(stderr, "[agent] quit\n");
        std::fflush(stderr);
        std::exit(0);
    case Verb::Unknown:
    default:
        fail(a, command, "unknown command");
        return;
    }
}

// Timed inputs that have run out: buttons or stick back to rest.
void update_pads(Agent& a) {
    for (unsigned player = 0u; player < 2u; ++player) {
        PadTimer& timer = a.pads[player];
        if (!timer.pad.active) continue;
        bool changed = false;
        if (timer.buttons_until != 0u && a.frame >= timer.buttons_until) {
            timer.pad.buttons = 0u;
            timer.pad.shake = false;
            timer.buttons_until = 0u;
            changed = true;
        }
        if (timer.stick_until != 0u && a.frame >= timer.stick_until) {
            timer.pad.stick_x = timer.pad.stick_y = 0.0f;
            timer.stick_until = 0u;
            changed = true;
        }
        if (changed) agent_set_pad(player, timer.pad);
    }
}

// The `file` command: the wrist-strap screen tapped through, the file select waited for, then the save loaded
// and its first stage requested; answered once the request is made (or on failure).
void update_file(Agent& a, const Guest& guest) {
    if (!a.file_pending) return;
    const auto finish = [&](bool ok, const char* error) {
        a.file_pending = false;
        a.blocked = false;
        a.pads[0] = PadTimer{};  // the A taps' controller back
        agent_set_pad(0u, agent::Pad{});
        reply(a, a.file_id, ok, ok ? std::string{} : std::string("\"error\":\"") + error + "\"");
    };
    if (a.frame - a.file_started > 60u * 120u) return finish(false, "timed out");
    const Scene scene = read_scene(guest);
    SavedContext saved(guest.context());
    if (a.file_phase == 0) {
        if (scene.scene == "Logo" && (a.frame % 40u) == 0u) {
            PadTimer& timer = a.pads[0];
            timer.pad.active = true;
            timer.pad.buttons = coop::button::kA;
            timer.buttons_until = a.frame + 4u;
            agent_set_pad(0u, timer.pad);
        }
        // The file select up for a second, with no save data handling under way (it preloads the files).
        const bool ready = scene.in_game && scene.stage == "FileSelect" &&
                           (guest.call(fn::kIsSaveSequenceActive) & 0xFFu) == 0u;
        a.file_settle = ready ? a.file_settle + 1u : 0u;
        if (a.file_settle >= 60u) {
            guest.call(fn::kStartPreLoadSaveData);
            a.file_phase = 1;
            a.file_settle = 0u;
        }
        return;
    }
    if (a.file_phase == 1) {
        if (++a.file_settle < 2u || (guest.call(fn::kIsSaveSequenceActive) & 0xFFu) != 0u) return;
        if ((guest.call(fn::kIsSaveSequenceSuccess) & 0xFFu) == 0u) return finish(false, "the save files could not be read");
        guest.call(fn::kStartGameDataLoad, a.file_index, a.file_mario ? 1u : 0u);
        std::fprintf(stderr, "[agent] loading save file %u\n", a.file_index);
        a.file_phase = 2;
        a.file_settle = 0u;
        return;
    }
    if (++a.file_settle < 2u || (guest.call(fn::kIsSaveSequenceActive) & 0xFFu) != 0u) return;
    if ((guest.call(fn::kIsSaveSequenceSuccess) & 0xFFu) == 0u) return finish(false, "the save file did not load");
    guest.call(fn::kChangeStageAfterLoading);
    finish(true, nullptr);
}

}  // namespace

void agent_frame(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t guest_pc) {
    Agent& a = agent_state();
    if (!a.enabled) return;
    ++a.frame;
    const Guest guest(context, memory, services, guest_pc);
    update_pads(a);
    update_file(a, guest);
    if (a.blocked && !a.file_pending && a.blocked_id != 0u && a.frame >= a.blocked_until) {
        a.blocked = false;
        reply(a, a.blocked_id, true, "");
    }
    if ((a.frame % 2u) == 0u) read_commands(a);
    // Commands up to the next one that waits.
    while (!a.blocked && !a.queue.empty()) {
        const agent::Command command = std::move(a.queue.front());
        a.queue.pop_front();
        run(a, guest, command);
    }
    // Writing the status calls the game. In deterministic mode (netplay) it goes by game frames, not the host
    // clock, so every run makes the same calls at the same frames.
    static const bool deterministic = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool on = _dupenv_s(&value, &length, "GALAXY_DETERMINISTIC") == 0 && value != nullptr &&
                        value[0] != '\0' && value[0] != '0';
        std::free(value);
        return on;
    }();
    const std::uint64_t now = GetTickCount64();
    // By the netplay frame, which a rollback puts back, so a replayed frame makes the same calls again.
    if (deterministic ? netplay_frame() % 30u == 0u : now - a.last_status_ms >= 500u) {
        a.last_status_ms = now;
        write_status(a, guest);
    }
}

}  // namespace galaxy
