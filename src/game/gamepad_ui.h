#ifndef FALLOUT_GAME_GAMEPAD_UI_H_
#define FALLOUT_GAME_GAMEPAD_UI_H_

#include <SDL.h>

namespace fallout {

// Always-on game-controller UI layer. Independent of any game session, so it
// works in the main menu, character creation, dialogs, and gameplay alike.
//
// Phase 1: right stick drives the mouse cursor (variable speed) and the right
// trigger acts as the left mouse button, both injected into the engine mouse
// pipeline via a mouse_info() hook so clicks register on real UI buttons.
//
// Later phases add modal-menu detection and left-stick item snapping; combat_rt
// shares this module's controller handle rather than opening its own.
void gamepad_ui_init();
void gamepad_ui_exit();

// Per-frame safe-tick update, called from the main game loop (NOT the input hook).
// Drives the gamepad overlays that touch GNW windows -- currently the LB skill wheel:
// opens it on the LB press edge, aims it with the right stick, and on release applies
// the chosen skill (Sneak toggles immediately; the rest arm the engine skill cursor).
void gamepad_ui_tick();

// The active controller handle, lazily opened and hotplug-aware. NULL when no
// controller is connected. Shared with combat_rt so there is a single owner.
SDL_GameController* gamepad_ui_get_controller();

// True when a modal menu/dialog has control (so the left stick should navigate
// the UI rather than move the player). Backed by the window manager's modal
// window check.
bool gamepad_ui_menu_active();

// What using an item from a quick slot costs in a fight: vanilla's 2 (see gpui_pack_cost for the
// drugs and weapons, which cost the inventory's price).
int gamepad_ui_item_use_ap_cost();

// True while the on-body aimed-shot picker is up (LB held, green body-part markers drawn on the
// locked enemy, right stick selecting a part). combat_rt checks this to halt walking and keep the
// right stick on part-selection instead of lock-switching, like it does for the radial wheels.
bool gamepad_ui_aim_body_active();

// True for the whole aim gesture (LB down), not only while the markers are up. The attack
// gate uses this so RT inside the opening window cannot fire an unaimed shot.
bool gamepad_ui_aim_gesture_active();

// True while the A button is acting as a world/UI interact (left click) for the
// current press, rather than as an attack. combat_rt checks this so it does not
// also fire an attack on the same press when a hostile drifts into range.
bool gamepad_ui_a_interacting();

// RUN IS A TOGGLE ON X (2026-09-16 layout), not a held button. X could not stay a hold:
// holding X opens the weapon wheel, and one button cannot be both. combat_rt reads this
// instead of the shoulder it used to read -- it is the only run input there is.
bool gamepad_ui_run_on();

// True while the ATTACK trigger (RT) is pulled. Exported so combat_rt does not have to
// re-implement the Schmitt trigger + hard-zero debounce that every analog read in this
// layer needs -- see gpui_trigger_held for why a bare threshold is not good enough.
bool gamepad_ui_attack_held();

// The body part bound in the LB called-shot window (a HIT_LOCATION_*). An attack made while the
// weapon slot is on its AIMED mode goes there -- see rt_player_called_loc in combat_rt.
int gamepad_ui_aim_part();

// True while the AIM trigger (LT) is pulled. LT aims the same way R3 does -- see the lock
// block in combat_rt -- and like every analog read in this layer it needs the Schmitt
// trigger plus hard-zero debounce, which is why it is exported rather than re-read there.
bool gamepad_ui_aim_trigger_held();

// The keyboard as a controller (2026-10-10): the main loop offers every key here first (gamepad_ui.cc
// has the layout); the mouse, still able to aim, counts only while it is actually being used.
bool gamepad_ui_handle_key(int* keyCode);
bool gamepad_ui_mouse_in_use();
void gamepad_ui_request_called_shot();

// True while LT is pulled, for the barter window's split-stack gesture (LT + A). Exported for the
// same reason as the two above: a bare threshold on the raw axis loses a held trigger to its
// intermittent hard-zero readings, which is what silently turned "move one" into "move the lot".
bool gamepad_ui_split_trigger_held();
void gamepad_ui_split_trigger_debug(int* raw, int* level);

// Set by the engine's hold-drag action popups (world action menu in gmouse.cc,
// inventory action menu in inventry.cc) while their navigation loop runs. Those
// popups are not GNW windows -- they read raw mouse-Y motion -- so while one is
// active the gamepad layer feeds the left stick in as cursor dy to navigate it.
void gamepad_ui_set_action_popup_active(bool active);

// Set by a screen that runs its OWN controller-driven navigation and reads the pad
// directly in its own loop -- the main menu (highlight + A) and the load/save slot
// list. While set, the shared mouse hook suppresses left-stick cursor snapping and
// A/Y-as-click so they don't fight that navigation; the right stick and RT still work.
void gamepad_ui_set_self_nav(bool active);
// Under self_nav, give the right stick back to the pointer (the world map, where the pointer is how
// you say WHERE). Ignored while a modal sits on top. Returns the PREVIOUS state, so a screen opened
// from inside (the town map) can switch it off and put it back.
bool gamepad_ui_set_self_nav_cursor(bool active);
// Drop the A -> left-click synthesis while a screen handles A itself. Narrower than self_nav:
// the right stick keeps driving the cursor, and RT keeps clicking. Returns the PREVIOUS state, so
// a screen opened from another one (the editor from character selection) can restore rather than
// clear -- clearing would hand the click back to the screen underneath, which still handles A.
bool gamepad_ui_set_suppress_a_click(bool suppress);
// Edge-tracking generation. A loop keeping its own prevA/prevB must re-prime when this changes:
// a nested modal (dialog_out) has run its own input loop and the outer edges are stale.
unsigned int gamepad_ui_input_epoch();
void gamepad_ui_bump_input_epoch();
bool gamepad_ui_a_click_suppressed();
void gamepad_ui_set_suppress_snap(bool active);
// Set while a pre-game modal screen with no live session is up (the death scene): the gameplay
// shortcuts must not fire into a game that main_reset_system has already torn down.
void gamepad_ui_set_no_session(bool active);

// Consumes a pending loot/container quick-take request set by the A/Y buttons:
// returns 0 (none), 1 (A = move one unit of the item under the cursor), or 2
// (Y = move the whole stack), and clears it. Polled by the loot_container loop.
int gamepad_ui_take_loot_quick();

// Consumes a pending normal-inventory quick action (tap-A), returning true once
// per request and clearing it. Polled by handle_inventory's loop, which then
// calls inven_gamepad_quick_normal() -- this must run from that loop and not the
// input hook, where the use/equip pipeline's map/monitor redraw would re-enter
// the windowing system and crash.
bool gamepad_ui_take_quick_normal();
// Per frame from the inventory's own loop (outside the input hook): keeps the LB / RB quick slots
// beside the bar in step while the inventory is open -- see gamepad_ui.cc.
void gamepad_ui_inventory_tick();

// One discrete D-pad step for navigating an action popup: -1 for up (previous
// option), +1 for down (next option), 0 for no step this frame. Edge-triggered
// with hold-to-repeat, so a held D-pad walks the list at a steady cadence. The
// popup loops call this each frame as a precise alternative to the analog stick;
// the highlighted option is still confirmed by releasing A (left-button up).
int gamepad_ui_action_popup_dpad();

// True on a fresh controller button/trigger press that should skip the current
// movie/cutscene. Self-arming edge detection, so a button still held from the
// previous screen does not instantly skip. Polled directly by gmovie_play:
// movies hide the mouse, which disables the mouse_info hook, so the controller
// can't reach the movie loop through the normal cursor pipeline.
bool gamepad_ui_skip_pressed();
int gamepad_ui_menu_stick_step();

// Reads the left stick as a normalized vector (SDL axes / 32767, Y positive
// down). Returns false and leaves x/y untouched when inside the deadzone. Used
// by the worldmap to steer travel with the stick.
bool gamepad_ui_get_left_stick(float* x, float* y);

// True while the armed skill cursor came from the pad's skill wheel and should SURVIVE being
// applied, so the same skill can be used again without walking back through L3 -> ring -> aim.
// Read by gmouse's skill-cursor dispatch, which otherwise reverts to MOVE after one use. B cancels
// it (gpui_button_shortcuts), so a sticky cursor is never a state you are stuck in.
bool gamepad_ui_skill_cursor_sticky();

// The object the pad has the armed use-on / skill crosshair glued on, or NULL to let the pixel under
// the pointer decide (gmouse reads it on the click). See gpui_tpick_stick.
struct Object;
Object* gamepad_ui_targeting_pick();

// THE QUICK SLOTS (2026-10-15): 0 = LB, 1 = RB. A slot holds a bound item pid, or -1 = it picks by
// itself (LB the best throwable, RB the best heal). Saved in combat_rt's save chunk; reset to auto on
// a new game and on every load before the chunk is read.
int gamepad_ui_quick_slot_get(int slot);
void gamepad_ui_quick_slot_set(int slot, int pid);
void gamepad_ui_quick_slots_reset();

} // namespace fallout

#endif /* FALLOUT_GAME_GAMEPAD_UI_H_ */
