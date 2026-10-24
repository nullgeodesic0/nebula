# Multiplayer UX: drop-in play, player devices, in-game menus

Status: built (multi-player, October 2026). The design below is kept as written; **As built** at the end says where
the result differs and where each piece lives.

## Goal

Single player is the default. Load a save as always and play alone. A second player can join at any time,
on this PC (a second controller) or online (a friend's PC), and leave again. The game does not restart for any of
this, and no setting has to be chosen at boot. Who plays with which device can be chosen on the file select and
from the pause menu, and changed at any time. Every menu looks like the game's own.

## What exists today (the current build)

- **Local drop-in/drop-out** *(exists)*: the second brother is created, hidden, at every level start whatever the
  Multiplayer switch says. A second controller's + drops him in beside the first player; holding − for a second,
  or unplugging that controller, drops him out. The Multiplayer switch only means "start with two".
- **Online** *(exists)*: F1 > Online > Joinable, Join (an address), Accept. With Joinable on from boot, the joiner
  loads the host's running game and drops in (live join, no restart). With Joinable switched on later, a join
  restarts both games and they return to the host's spot (the fallback). Rollback, the online pause menu
  (Resume, Exit the galaxy, Leave) and per-PC full-screen views exist too.
- **Device routing** *(exists, implicit)*: on one PC, the first controller with input drives the first player and
  a second connected controller is the second player's. The keyboard and mouse drive the first player. There is
  no way to choose.

## Player flows

1. **Local drop-in.** One join button on every controller: Start on the Wii layout (+), Menu on Xbox, Options on
   DualSense. While a controller is connected but not playing, a small prompt sits in the corner: "[glyph] Join as
   Luigi", with that controller's own glyph (one line per waiting controller, fading out after 10 s of no input
   and back when the controller is touched). Pressing it on that controller drops the player in. A small
   game-style card appears for about 2 s: "Luigi joins! (DualSense)". Luigi appears beside Mario and the split
   screen opens when they part. On a Luigi save, the joiner is Mario.
2. **Local drop-out.** From the pause menu's Players panel, choose "Leave", or hold − for 1 s. If the
   controller disconnects, a card says "Luigi's controller disconnected: Start to rejoin", and Luigi steps out
   after a 10 s grace period (rejoining within it keeps him in place).
3. **Choosing devices.** The Players panel lists the devices that are connected (keyboard and mouse, each
   controller with its glyph family), each with a slot: Mario, Luigi or none. Left and right move a device between
   slots. Changes apply at once, and a device can have only one slot. Defaults: the first device with input is
   the first player, and the next one to press Start is the second.
4. **Online, the host.** Joinable on (a toggle in the Players panel and in the file select's online card). A
   friend's request shows as a game-style dialogue: "Tyler wants to join as Luigi. Accept / Deny" (a name from
   the joiner's settings, or the address). Accept: Luigi drops in once the friend has caught up (a few seconds,
   with a "Connecting…" card). Deny: the friend is told.
5. **Online, the joiner.** File select or pause menu, then Online, then Join, then the address (or a recent host).
   A card shows "Asking…" and then "Joining Tyler's game…". On accept, the joiner's window shows the host's
   game with Luigi in it. Leave (pause menu): the joiner goes back to their own game and the host plays on alone.
6. **Online disconnect.** The host plays on alone: Luigi steps out and a card says "Your friend left". *Today the
   host restarts; changing that is part of this work.* The joiner returns to their own save.

## Where each menu lives, and how it looks

| Menu | Where | Contents |
| --- | --- | --- |
| Players panel | File select (beside the save files; replaces the current Multiplayer/Odyssey text panel), and the pause menu (a panel beside the game's own pause window) | Device slots, Joinable, Online (Join, recent hosts), Leave |
| Join request | Over the game, at the top, while play continues | Name, Accept/Deny with this PC's button glyphs |
| Join prompt | Bottom corner, while a controller waits unassigned | "[join glyph] Join as Luigi" per waiting controller |
| Status cards | Top centre, short-lived | Joins, leaves, disconnects, connecting |
| Settings (F1) | As now, rethemed | All current pages |

**The look.** The game's own window frame (the file select's rounded frame is already extracted from the disc at
runtime for the F1 corners), its font (the disc's BRFNT, rendered as glyph textures by our renderer), its colours
(the deep blue panel with the yellow selection), and the HD button prompts that already exist (Wii, Xbox,
DualSense glyphs). Sounds: the game's own menu cursor, decide and cancel sounds, played on the host side so they
never touch game state. Nothing from the disc is committed; everything is read from the user's disc at startup.

**How it is drawn.** Our renderer's UI layer (where F1 and the file-select panel draw now), styled with the
game's assets. It is not the game's own layout system (LayoutActor, BRLYT):

- Online, a menu shown on one PC must never change game memory, or the two PCs desync. A host-side layer cannot
  change it. The game's layout system lives in game memory.
- Driving the game's layout system from outside means building layouts in its formats and hooking its scene
  graph per screen. That is weeks of work per menu, and fragile.

The cost is that our menus only *look* like the game's: the frame, font, colours, sounds and prompts are the
game's, and the animation is ours (a short ease in and out, matching the game's timing).

## What the game state needs

- **Boot:** nothing to choose. The second brother's files and his hidden actor at each level start already
  happen whenever drop-in is enabled (default on), so single player pays only the hidden actor (about 600 KB of
  stage memory, which already falls back to MEM2 when needed). There is no frame-rate cost.
- **Device assignment** is host-side only (which device feeds which player's input), so it never touches game
  state, online or local. Online, each PC has one player, so the panel shows only this PC's devices for its own
  brother.
- **Online determinism:** joins, leaves and drop-in happen through the synchronised input (NetCommand, as Exit
  Galaxy and Join do now). Menus and cards are host-side only.
- **Joinable at boot:** live join needs the game deterministic from startup. Proposal: make "Joinable" a setting
  that takes effect at the next start (saved), with the rest as now. Switched on mid-game, it offers the restart
  join, saying so in the card.

## Order of work

1. **Input router.** An explicit device-to-player table, replacing the implicit "second connected controller"
   rule: defaults, Start on an unassigned device drops in, disconnect grace, persistence per PC. No UI yet; driven
   by the agent in tests.
2. **UI kit.** The game's frame, font and colours, and the HD prompts, in the renderer's UI layer: panels, lists,
   cards and a dialogue, with keyboard, controller and mouse navigation, and the game's menu sounds host-side.
3. **Players panel on the file select** (replacing today's text panel), then the same panel beside the pause
   menu.
4. **Online in the panel:** Joinable, Join with recent hosts, the in-game request dialogue (Accept/Deny), and the
   status cards.
5. **Online disconnect without a restart:** the host leaves the session and plays on alone (out of lockstep,
   still deterministic, still Joinable).
6. **The F1 menu rethemed** with the same kit.
7. A test pass: local drop-in and out with two real controllers, device swaps, online join, deny, leave and
   disconnect, both PCs.

Steps 1 and 2 can run in parallel (different code). 3 and 4 need 2.

## Risks

- **The font:** the disc's BRFNT must be decoded (sheets plus the character map) and drawn at HD sizes. It is
  well documented, but there is work in it. Fallback: the existing UI font with the game's colours.
- **The pause menu:** the game pauses its own scene, and our panel sits beside it. Knowing when the pause menu is
  open needs a hook on the game's pause scene (the co-op code already watches scenes). Input while paused must go
  to our panel or to the game's menu, never both.
- **Sounds:** the game's sound effects live in its sound archive (BRSAR, ADPCM). Playing them host-side needs a
  small decoder. Fallback: silent menus, or calling the game's sound function, which local play allows but online
  play does not.
- **Device identity across reconnects:** XInput slots and HID paths can change. We match by path and then by
  kind, and ask again if unsure.
- **Live join needs Joinable at boot:** without it, a restart is unavoidable. The card says so.
- **Effort:** steps 1 and 2 are about a day each, 3 to 5 about a day each, and 6 about half a day. The font and
  sounds are the uncertain parts.

## As built

**Devices** (`runtime/include/galaxy/player_devices.h`): the router replaces the "second connected controller"
rule. A free controller's + (its own binding of the Wii +) drops the second player in; a lone controller's + pauses.
His controller going away leaves him in, still, for 10 s ("Press Start to rejoin"), then he sits out. Seats (Auto,
the save's brother, the partner, Off) are saved as `player_devices=` in runtime_settings.ini; the keyboard can be
the second player's. Device identity is the XInput or HID slot (a controller that comes back in another slot is a
new device).

**The menu kit** (`runtime/include/galaxy/gx/menu_kit.h`): the game's pill buttons, info window, MessageFont26 and
animation keys, read from game.pak at startup (`game_disc.h`, `game_font.h`) and drawn by our renderer in one draw a
frame. Checked against screenshots of the game's pause menu and Yes/No dialog: same pixels and colours (labels within
one grey level). The font is drawn from distance fields (`font_sdf.h`), sharp at 1440p and 4K with the game's weight.
The buttons follow the game's own hover, bounce and decide animations; the pointer (the game's Star Pointer) hovers
and A chooses as in the game's menus, the D-pad moves a focus as an extra, and the Star Pointer's hand is drawn again
over our windows (they cover the game's). A resting pointer never presses our buttons (it must have moved in the
last 2 s).

**Sounds** (`runtime/include/galaxy/menu_sounds.h`): the game's own system sounds (cursor, decide, cancel, window
open and close, pause on and off, Yes, No) synthesised on the host from the disc's JAudio data; checked against
captures of the game's output (6 of 15 bit-identical, the rest 65-92 dB, exact lengths and peaks). No game code runs,
so online menus cannot desync.

**Where the menus are** (`runtime/src/gx/menu_screens.cpp`, `host_menus.cpp`):
- Join prompts at the bottom, cards at the top (joins, disconnects with the countdown, leaves; online requests,
  asking, outcomes, "Your friend left").
- The Players panel beside the game's pause menu (right of its buttons, 216 units wide; on a 4:3 picture it
  overlaps their edge).
- The save select: the game's screens fill it, so the options are one small **Options** button between Back and Play
  This File, opening a window (Two players, Odyssey moves, Online, Back). A window there covered Play This File.
- The Online window (from Options or the Players panel): Joinable, and Join a friend with an address box typed on the
  keyboard. There is no list of recent hosts yet; the box keeps the last address.
- The online menu (+ online): Resume, Exit the galaxy, Leave; on a join request, Accept and Not now.
- F1 in the game's style; the old look remains the fallback when the game's art cannot be read.

**Input while our windows are up**: a window covering the game's menu (Online, Options) takes the first player's
buttons (`players::set_menu_capture`); typing in the address box takes the keyboard. Out of a session, a join request
gives + to the answer rather than the pause.

**Online disconnect**: a live host (Joinable since the start) plays on alone when its friend leaves or drops, and
takes the next joiner (`netplay_session.cpp back_to_solo`). A joiner, and a session started by a restart, still
restart on their own.
