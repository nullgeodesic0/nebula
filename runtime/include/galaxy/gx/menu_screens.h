#pragma once

// The host menus' screens (docs/MULTIPLAYER_UX.md), built with the menu kit each frame from a snapshot of the game
// and the devices, returning what the player chose. host_menus.cpp feeds them and carries out the actions.
// - Join hints: while the second player is out, each free controller shows "[Start] Join as Luigi" at the bottom,
//   fading after 10 s without input and back when touched.
// - Status cards: joins, disconnects (with the grace countdown) and leaves, top centre.
// - The Players panel: beside the game's pause menu, each device and the brother it plays, and Leave.
// - The save select's options: two players from the start and the Odyssey moves, bottom right.

#include "galaxy/gx/menu_kit.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace galaxy::gx::menu {

// The seat chosen for a device: the first player's brother (the save's), the second's, or nobody's.
enum class SeatChoice : std::uint8_t { Auto = 0, First = 1, Second = 2, Off = 3 };

struct DeviceView {
    std::uint32_t key = 0u;
    GlyphFamily family = GlyphFamily::Wii;
    std::u16string name;          // "Xbox 1", "DualSense", "Keyboard"
    bool keyboard = false;
    SeatChoice seat = SeatChoice::Auto;
    bool driving = false;         // it plays the first player now
    bool second = false;          // it plays the second player now
    bool join_candidate = false;
    bool input = false;           // it had input this frame
    GlyphRef join_glyph;          // its button for the Wii + (the join button), as bound
};

enum class CardKind : std::uint8_t { Joined, Disconnected, Reconnected, Left };
struct CardEvent {
    CardKind kind;
    std::uint32_t device;
};

enum class MenuSound : std::uint8_t { Cursor, Decide, Cancel };

// Online co-op as the lobby has it (net_lobby.h NetLobbyPhase, plus Unavailable: no lobby).
enum class OnlinePhase : std::uint8_t { Unavailable, Off, Joinable, PortInUse, Asking, Waiting, AskedToJoin, InSession,
                                         Outcome };
struct OnlineView {
    OnlinePhase phase = OnlinePhase::Unavailable;
    std::u16string address;        // Joinable: this PC's; Asking/Waiting: the host's
    std::u16string asker;          // AskedToJoin
    std::u16string outcome;        // Outcome, as the lobby words it (upper case)
    bool host = false;             // in a session as its host
    bool joinable = false;         // the setting
    bool joinable_live = false;    // Joinable since the start: a joiner comes in without a restart
    unsigned port = 7777u;
    std::u16string join_address;   // the address box (settings net_join_address)
    // This PC's online menu (net_lobby.h online_menu_poll): + opens it in a session or to answer a request.
    bool menu_open = false;
    int menu_row = 0;
    bool menu_answering = false;
    GlyphRef plus_glyph;           // + as the controller in use has it bound
};

struct ScreensInput {
    double seconds = 0.0;
    float extent_x = 304.0f;      // half the picture's width in layout units (304 at 4:3, 405 at 16:9)
    bool local_coop = false;      // drop-in on this PC (co-op available, not online)
    bool in_level = false;        // a stage is playing
    bool pause_shown = false;     // the game's pause menu is up
    bool second_present = false;
    bool second_waiting = false;  // his controller is gone, inside the grace
    float grace_left = 0.0f;
    bool second_is_luigi = true;
    std::vector<DeviceView> devices;
    std::vector<CardEvent> events;
    // The save select's options (coop_guest.cpp file_select_frame): two players from the start, the Odyssey
    // moves, and the buttons that switch them (the Wii 1 and +, as the controller in use has them bound).
    bool file_select = false;
    bool two_players = false, odyssey_moves = false;
    OnlineView online;
    // Keys typed while the address box has the focus (host_menus.h push_host_menu_key).
    std::u32string keys;
    GlyphRef two_players_glyph, odyssey_moves_glyph;
    // The first player's buttons (players::MenuInput bits) and pointer (-1..1, y down).
    std::uint32_t held = 0u, pressed = 0u;
    bool pointer_valid = false;
    float pointer_x = 0.0f, pointer_y = 0.0f;
    // Set by update: the pointer is on screen (pointer_valid then says whether it is pointing, moved lately).
    bool pointer_shown = false;
};

struct ScreensActions {
    std::vector<std::pair<std::uint32_t, SeatChoice>> seats;
    bool leave = false;
    bool toggle_two_players = false, toggle_odyssey_moves = false;
    // Online: switch Joinable, ask the address box's host, stop asking, the box's new text, whether it wants the
    // keyboard.
    bool toggle_joinable = false, join = false, stop_asking = false;
    bool set_join_address = false;
    std::u16string join_address;
    bool text_focus = false;
    std::vector<MenuSound> sounds;
    // The pointer is over a menu, which covers the game's own hand: draw it again on top (pointer_hand) at
    // (pointer_lx, pointer_ly), layout units.
    bool cover_pointer = false;
    float pointer_lx = 0.0f, pointer_ly = 0.0f;
};

class Screens {
public:
    ScreensActions update(const ScreensInput& raw, const Assets& assets, const Space& space, DrawList& list);

    // The Players panel's buttons this frame, for tests: their centres in layout units.
    struct Button {
        float x, y;
        std::uint32_t device;  // 0 for Leave
        bool leave;
    };
    [[nodiscard]] const std::vector<Button>& panel_buttons() const noexcept { return buttons_; }
    // The Online window is up (it covers the game's pause menu, so the game must not read the buttons).
    [[nodiscard]] bool online_window_open() const noexcept { return online_open_; }

private:
    void hints(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames);
    void cards(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames);
    void panel(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames,
               ScreensActions& actions);
    void file_select(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames,
                     ScreensActions& actions);
    void online_window(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames,
                       ScreensActions& actions);
    void online_menu(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames);
    void online_cards(const ScreensInput& in, const Assets& assets, const Space& space, DrawList& list, float frames);

    double last_seconds_ = -1.0;
    // The pointer counts on the menus only while someone points with it (it moved in the last 2 s): a resting
    // pointer that happens to sit on a button never chooses it.
    float pointer_x_ = 0.0f, pointer_y_ = 0.0f;
    double pointer_moved_ = -100.0;
    // Join hints: when each device last had input, and the hint's fade.
    std::unordered_map<std::uint32_t, double> last_input_;
    std::unordered_map<std::uint32_t, float> hint_alpha_;
    // Cards: the one showing and those waiting.
    struct Card {
        CardKind kind;
        std::u16string device;
        double until = 0.0;
        WindowAnim anim;
    };
    std::vector<Card> cards_;
    // The panel.
    WindowAnim panel_anim_;
    std::unordered_map<std::uint32_t, ButtonAnim> button_anims_;  // by device, 0 = Leave
    std::vector<Button> buttons_;
    int focus_ = -1;          // the button the D-pad points at (-1: the pointer's)
    int hovered_ = -1;
    // The save select's options.
    WindowAnim options_anim_;
    std::array<ButtonAnim, 3> option_anims_;
    int option_hovered_ = -1;
    // The Online window, opened from the Players panel or the save select's options.
    bool online_open_ = false;
    WindowAnim online_anim_;
    std::array<ButtonAnim, 4> online_anims_;  // Joinable, the address box, Join (or Stop), Back
    int online_hovered_ = -1, online_focus_ = -1;
    bool typing_ = false;
    bool typed_fresh_ = false;  // the first character typed replaces the saved address
    std::u16string typed_;
    // The online menu and the lobby's cards.
    WindowAnim menu_anim_;
    std::array<ButtonAnim, 3> menu_anims_;
    WindowAnim lobby_card_anim_;
    OnlinePhase lobby_card_phase_ = OnlinePhase::Unavailable;
    std::u16string lobby_card_words_[2];
    double lobby_card_until_ = 0.0;
    OnlinePhase last_phase_ = OnlinePhase::Unavailable;
    std::u16string last_outcome_;
};

// The seat's label: the brother it plays ("Luigi"/"Mario", the second being the partner of the save's), "Off", or
// "Auto" with what it plays now.
[[nodiscard]] std::u16string seat_label(SeatChoice seat, bool driving, bool second, bool second_is_luigi);

}  // namespace galaxy::gx::menu
