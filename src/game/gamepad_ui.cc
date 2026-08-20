#include "game/gamepad_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/automap.h"
#include "game/combat.h"
#include "plib/gnw/text.h" // body-part label on the aimed-shot marker
#include "game/combat_defs.h"
#include "game/combat_rt.h"
#include "game/editor.h"
#include "game/game_trace.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/interact_prompt.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/protinst.h" // obj_destroy
#include "game/loadsave.h"
#include "game/mainmenu.h"
#include "game/map.h"
#include "game/tile.h"
#include "game/object.h"
#include "game/options.h"
#include "game/pipboy.h"
#include "game/radial_wheel.h"
#include "game/select.h"
#include "game/skill.h"
#include "game/stat.h" // stat_name, for the drug wheel's effect line
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"

namespace fallout {

// Right stick -> cursor tuning.
#define GPUI_AXIS_MAX 32767.0f
#define GPUI_STICK_DEADZONE 0.2f
#define GPUI_CURSOR_MAX_SPEED 14.0f // pixels per frame at full deflection
// Right trigger -> left click threshold (~0.5 of the 0..32767 trigger range).
#define GPUI_TRIGGER_THRESHOLD 16000

// Left-stick menu snapping tuning.
#define GPUI_SNAP_AXIS_THRESHOLD 0.5f   // how far past center counts as a press
#define GPUI_SNAP_REPEAT_MS 220         // hold-to-repeat interval
#define GPUI_SNAP_MAX_ITEMS 48          // cap on enumerated menu items
#define GPUI_SNAP_PERP_PENALTY 3        // off-axis cost when picking a target

// Left-stick -> cursor dy speed while a hold-drag action popup is open.
#define GPUI_POPUP_NAV_SPEED 14.0f

// How long A must be held (ms) inside an inventory before it becomes the context
// popup instead of the quick-action tap.
#define GPUI_A_HOLD_MS 250

// Aimed-shot (VATS) LB tap-vs-hold split: a press+release shorter than this fires an aimed shot at
// the last-used body part IMMEDIATELY (no wheel); holding past it opens the body-part wheel to pick.
#define GPUI_AIM_TAP_MS 220

// Quick-use wheel B tap-vs-hold split: hold B past this to bring up the drug wheel; a
// shorter press stays the weapon's firing-mode cycle. Same window as the aimed-shot split
// so both holds feel identical in the hand.
#define GPUI_QUICK_HOLD_MS GPUI_AIM_TAP_MS

// D-pad tap-vs-hold split: a HOLD past this opens the standalone screen for that direction
// (Pip-Boy / character sheet / automap) or ends combat; a shorter TAP steps the action the
// interact prompt is offering on the highlighted object. Deliberately LONGER than the B/Y window:
// those two split a rare action off a common one, whereas here the common action is the TAP, and a
// 220 ms threshold made brisk taps open the Pip-Boy by accident.
#define GPUI_DPAD_HOLD_MS 350

// Right-stick deflection past which the interact-prompt picker starts aiming. Lower than a flick
// threshold would be, because the aim is CONTINUOUS rather than a step: this is just "the stick is
// meaningfully pointing somewhere", not "commit to a move".
#define GPUI_IP_PICK_THRESHOLD 0.45f

// After a hostile was last in attack range (or a lock was held), A won't auto-loot /
// interact for this long. Spamming A to attack means a tap often lands the instant the
// last enemy dies -- without this it would immediately loot the fresh corpse (or fire a
// world interaction) mid-fight. The window absorbs that spam tail; then A loots normally.
#define GPUI_POST_KILL_LOOT_MS 500

// How long the pointer stays visible after the last right-stick nudge before it hides
// again. The stick keeps it fresh while held, then it fades out this long after release.
#define GPUI_CURSOR_SHOW_MS 2000

// The cursor is only auto-hidden during live iso GAMEPLAY. gamepad_ui_tick runs only in
// the main game loop, so if it stamped within this window we're in gameplay; any menu /
// dialogue / world-map loop lets it go stale and the pointer stays visible for
// navigation. See gpui_hide_cursor_predicate.
#define GPUI_TICK_FRESH_MS 100

static SDL_GameController* gpui_controller = NULL;

// get_time() of the last right-stick movement that drove the cursor. Within gameplay the
// cursor shows for GPUI_CURSOR_SHOW_MS after this; hidden otherwise.
static unsigned int gpui_cursor_show_time = 0;

// get_time() of the last gamepad_ui_tick (last main-game-loop frame). 0 until first tick.
static unsigned int gpui_last_tick_time = 0;

// A is acting as a world/UI interact (left click) for the current press; latched
// at the press edge so a hostile entering range mid-press can't flip it back to
// attack and snap a hold-drag popup shut. See gpui_action_buttons.
static bool gpui_a_interacting = false;

// Pending loot quick-take request (0 none, 1 = A single unit, 2 = Y whole stack),
// set by the A/Y buttons in a loot window and consumed by the loot_container loop
// via gamepad_ui_take_loot_quick().
static int gpui_loot_quick = 0;

// Pending normal-inventory quick action (tap-A): set in the input hook but run
// from handle_inventory's own loop via gamepad_ui_take_quick_normal(). It must
// NOT run inside the hook -- the use/equip pipeline can refresh the map and the
// message monitor (tile_refresh_rect / display_print), which re-enters the
// windowing system mid button-processing and crashes (esp. in combat).
static bool gpui_quick_normal = false;

// An engine hold-drag action popup (world/inventory) is mid-navigation.
static bool gpui_action_popup_active = false;

// A screen is running its OWN controller-driven navigation and reads the pad
// directly in its own loop -- the main menu (highlight + A) and the load/save slot
// list (left stick / D-pad step, A/B confirm/cancel). While set, the shared hook
// must NOT also left-stick-snap the cursor or map A/Y to a click -- both would fight
// that nav (the snap yanks the cursor onto buttons/scroll arrows). The right stick
// can still move the cursor and RT still clicks, for optional mouse-style use.
static bool gpui_self_nav = false;

// Suppresses ONLY the left-stick item snap (gpui_menu_snap), leaving A/Y clicks and
// RT alone. The barter/trade window sets this while it drives its own LB/RB cursor
// stepping: without it, the snap grabs the trade window's slot and scroll-arrow
// buttons and fights that stepping. Unlike gpui_self_nav, it must NOT disable the
// A-router, since barter still relies on A to transfer the item under the cursor.
static bool gpui_suppress_snap = false;

// Aim-picker cancel handshake. Declared up here rather than beside gpui_aim_body_on because the
// B-button shortcut handler further down the file has to see the swallow flag: the press that
// cancels an aim must not ALSO cycle the weapon's firing mode.
static bool gpui_aim_canceled = false;  // B cancelled this aim; eat the LB release still to come
static bool gpui_aim_swallow_b = false; // that same B press is spent

// Opens the first connected controller if we don't have one yet.
static void gpui_acquire()
{
    if (gpui_controller != NULL) {
        return;
    }

    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (SDL_IsGameController(i)) {
            gpui_controller = SDL_GameControllerOpen(i);
            if (gpui_controller != NULL) {
                break;
            }
        }
    }
}

SDL_GameController* gamepad_ui_get_controller()
{
    // Drop a handle that's no longer attached FIRST, then (re)acquire, so a
    // disconnect is recovered within the same call instead of returning NULL for
    // a frame (and so a freshly opened handle is never closed by this same call).
    if (gpui_controller != NULL && !SDL_GameControllerGetAttached(gpui_controller)) {
        SDL_GameControllerClose(gpui_controller);
        gpui_controller = NULL;
    }

    gpui_acquire();

    return gpui_controller;
}

bool gamepad_ui_menu_active()
{
    return win_get_top_modal() != -1;
}

bool gamepad_ui_a_interacting()
{
    return gpui_a_interacting;
}

void gamepad_ui_set_action_popup_active(bool active)
{
    gpui_action_popup_active = active;
}

void gamepad_ui_set_self_nav(bool active)
{
    gpui_self_nav = active;
}

void gamepad_ui_set_suppress_snap(bool active)
{
    gpui_suppress_snap = active;
}

int gamepad_ui_take_loot_quick()
{
    int q = gpui_loot_quick;
    gpui_loot_quick = 0;
    return q;
}

bool gamepad_ui_take_quick_normal()
{
    bool q = gpui_quick_normal;
    gpui_quick_normal = false;
    return q;
}

int gamepad_ui_action_popup_dpad()
{
    static int lastDir = 0;
    static Uint32 lastStepTime = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        lastDir = 0;
        return 0;
    }

    SDL_GameControllerUpdate();

    int dir = 0;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0) {
        dir = -1;
    } else if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0) {
        dir = 1;
    }

    if (dir == 0) {
        lastDir = 0;
        return 0;
    }

    Uint32 now = SDL_GetTicks();
    bool changed = dir != lastDir;
    if (!changed && now - lastStepTime < GPUI_SNAP_REPEAT_MS) {
        return 0;
    }

    lastDir = dir;
    lastStepTime = now;
    return dir;
}

bool gamepad_ui_skip_pressed()
{
    // Persists across movies; self-corrects because any all-released frame
    // re-arms it. Starts disarmed so a button held into the first movie can't
    // skip until it is released once.
    static bool armed = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return false;
    }

    SDL_GameControllerUpdate();

    bool anyDown = false;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b) != 0) {
            anyDown = true;
            break;
        }
    }
    if (!anyDown
        && (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GPUI_TRIGGER_THRESHOLD
            || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GPUI_TRIGGER_THRESHOLD)) {
        anyDown = true;
    }

    if (!anyDown) {
        armed = true;
        return false;
    }

    if (armed) {
        armed = false;
        return true;
    }

    return false;
}

bool gamepad_ui_get_left_stick(float* x, float* y)
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return false;
    }

    SDL_GameControllerUpdate();

    float lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
    float ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
    if (lx * lx + ly * ly < GPUI_STICK_DEADZONE * GPUI_STICK_DEADZONE) {
        return false;
    }

    *x = lx;
    *y = ly;
    return true;
}

// Left stick snaps the cursor between menu/dialog items (button centers) when a
// menu is open. 2D and direction-aware: the dominant stick axis picks one of
// up/down/left/right, and the step lands on the enumerated button that is
// nearest IN THAT DIRECTION from the cursor (travel along the axis plus a
// perpendicular penalty so the most in-line item wins). This reaches multi-
// column layouts -- e.g. the inventory's list, armor slot, and hand slots --
// not just a single vertical list. Edge-triggered with hold-to-repeat. When a
// step fires it OVERRIDES dx/dy with the jump to the chosen item's center;
// otherwise it leaves them alone so the right stick can still free-move.
static void gpui_menu_snap(SDL_GameController* pad, int* dx, int* dy)
{
    static int lastDirX = 0;
    static int lastDirY = 0;
    static Uint32 lastStepTime = 0;

    float lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
    float ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;

    // Dominant axis -> a single 4-way direction (D-pad style), so a diagonal
    // push resolves to one clean step rather than fighting between axes.
    int dirX = 0;
    int dirY = 0;
    if (fabsf(lx) > fabsf(ly)) {
        if (lx < -GPUI_SNAP_AXIS_THRESHOLD) {
            dirX = -1;
        } else if (lx > GPUI_SNAP_AXIS_THRESHOLD) {
            dirX = 1;
        }
    } else {
        if (ly < -GPUI_SNAP_AXIS_THRESHOLD) {
            dirY = -1;
        } else if (ly > GPUI_SNAP_AXIS_THRESHOLD) {
            dirY = 1;
        }
    }

    if (dirX == 0 && dirY == 0) {
        lastDirX = 0;
        lastDirY = 0;
        return;
    }

    Uint32 now = SDL_GetTicks();
    bool changed = dirX != lastDirX || dirY != lastDirY;
    bool fire = changed || now - lastStepTime >= GPUI_SNAP_REPEAT_MS;
    if (!fire) {
        return;
    }
    lastDirX = dirX;
    lastDirY = dirY;
    lastStepTime = now;

    int xs[GPUI_SNAP_MAX_ITEMS];
    int ys[GPUI_SNAP_MAX_ITEMS];
    int count = win_get_top_button_centers(xs, ys, GPUI_SNAP_MAX_ITEMS);
    if (count < 2) {
        return; // nothing to navigate
    }

    int cursorX;
    int cursorY;
    mouse_get_position(&cursorX, &cursorY);

    // Pick the button best matching "nearest in the pushed direction": positive
    // travel along the direction axis (beyond a small tolerance so the current
    // item isn't re-picked), scored by that travel plus a perpendicular penalty.
    int best = -1;
    long bestScore = 0;
    for (int i = 0; i < count; i++) {
        int rx = xs[i] - cursorX;
        int ry = ys[i] - cursorY;
        int along = dirX != 0 ? dirX * rx : dirY * ry;
        int perp = dirX != 0 ? ry : rx;
        if (along <= 2) {
            continue; // not ahead in the pushed direction
        }
        long score = (long)along + (long)GPUI_SNAP_PERP_PENALTY * labs(perp);
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }

    if (best < 0) {
        return; // nothing further that way -> stay put (no wrap)
    }

    *dx = xs[best] - cursorX;
    *dy = ys[best] - cursorY;
}

// A-button router while an inventory window is open. A quick TAP performs the
// one-shot quick action; HOLDING A opens the full Use/Drop/Look context popup.
// The tap-vs-hold decision is delayed: nothing is injected on the press edge, so
// the same press can resolve either way.
//
//   * Tap in the player's own inventory (normal / use-on): handled directly by
//     inven_gamepad_quick_normal (use drugs/usables, else equip to a free hand).
//     Its HAND-mode mouse click is a pick-up-drag, so it can't be reused here.
//   * Tap in loot/barter: drive the engine's existing HAND-mode click (which is
//     already a cross-panel transfer) by forcing HAND mode and pulsing one
//     synthetic left click over the hovered item.
//   * Hold (any window): force ARROW mode and hold the synthetic left button
//     down so the engine's hold-drag menu opens; releasing A raises the button,
//     which the popup loop reads as the confirm. D-pad/stick navigate it.
//
// Forcing the mode via inven_set_mouse (rather than a parallel override) keeps
// the engine's own click dispatch untouched. State resets whenever no inventory
// is open so a press that straddles open/close can't get stuck.
static void gpui_inventory_a(SDL_GameController* pad, int* buttons)
{
    // 0 idle, 1 pending (held, undecided), 2 hold-popup, 3 tap-click release.
    static int phase = 0;
    static Uint32 pressTime = 0;
    static bool prevA = false;
    static bool prevY = false;

    // Read the live button state up front so it can be tracked even while no window is
    // open. That baseline is what stops the press that OPENED a window from being seen as
    // a fresh press edge on the window's first frame: without it, prevA reset to false while
    // closed, so a still-held A (e.g. looting an adjacent corpse, which opens with no
    // approach walk to release the button) rose-edged instantly and tapped Done -- "pressed
    // loot and it immediately clicked Done." Tracking the real state means the carried-over
    // hold is a no-edge; a genuine release + re-press inside the window still taps normally.
    bool aHeld = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool yHeld = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;

    if (!inven_any_open()) {
        phase = 0;
        prevA = aHeld;
        prevY = yHeld;
        gpui_loot_quick = 0;
        gpui_quick_normal = false;
        return;
    }

    // The barter/trade window drives its own A transfer (barter_gamepad_input calls
    // barter_transfer_slot directly). Don't also fire the synthetic HAND-mode click
    // here -- it would double-transfer, or (with a stale cursor) misfire.
    if (inven_current_window_type() == INVENTORY_WINDOW_TYPE_TRADE) {
        phase = 0;
        prevA = aHeld;
        prevY = yHeld;
        return;
    }

    bool aNow = aHeld;
    bool aEdge = aNow && !prevA;
    prevA = aNow;

    // Y in a loot window grabs the whole stack under the cursor (edge-triggered);
    // the loot_container loop consumes the request. Elsewhere Y stays a raw left
    // click (handled in gpui_action_buttons).
    bool yNow = yHeld;
    bool yEdge = yNow && !prevY;
    prevY = yNow;
    if (yEdge && inven_current_window_type() == INVENTORY_WINDOW_TYPE_LOOT) {
        gpui_loot_quick = 2;
    }

    Uint32 now = SDL_GetTicks();

    switch (phase) {
    case 0:
        if (aEdge) {
            phase = 1;
            pressTime = now;
        }
        break;

    case 1:
        if (!aNow) {
            // Released before the hold threshold -> a tap (quick action).
            int type = inven_current_window_type();
            if (type == INVENTORY_WINDOW_TYPE_NORMAL || type == INVENTORY_WINDOW_TYPE_USE_ITEM_ON) {
                if (inven_normal_cursor_over_list()) {
                    // Over the item list: defer the quick equip/use to
                    // handle_inventory's loop -- the use/equip pipeline is not
                    // safe to run inside the input hook (see gpui_quick_normal).
                    gpui_quick_normal = true;
                    phase = 0;
                } else {
                    // Over Done or another GNW button: a plain synthetic click so
                    // the button fires normally (Done closes the inventory).
                    *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
                    phase = 3;
                }
            } else if (type == INVENTORY_WINDOW_TYPE_LOOT) {
                if (inven_loot_cursor_over_panel()) {
                    // Over an item panel: request a single-unit quick-take; the
                    // loot_container loop does the transfer directly (no synthetic
                    // click, which would trigger move_inventory's drag).
                    gpui_loot_quick = 1;
                    phase = 0;
                } else {
                    // Over Done or another GNW button: a plain synthetic click so
                    // the button fires normally (Done closes the loot window).
                    *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
                    phase = 3;
                }
            } else {
                inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
                *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
                phase = 3;
            }
        } else if (now - pressTime >= GPUI_A_HOLD_MS) {
            // Held past the threshold -> open the context popup.
            inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
            *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
            phase = 2;
        }
        break;

    case 2:
        if (aNow) {
            *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN; // keep the popup open
        } else {
            phase = 0; // release -> button up -> popup confirms the highlight
        }
        break;

    case 3:
        // The single synthetic down was issued last frame; releasing it now
        // makes a one-click down+up the loot/barter loop turns into a transfer.
        phase = 0;
        break;
    }
}

// "Is a hostile inside the dude's weapon range", sampled ONCE per frame in gamepad_ui_tick and
// read from the mouse hook. It is NOT queried from the hook directly, because the answer needs
// item_w_range -> proto_ptr, which can hit the DISK, and the engine pumps the mouse from inside
// its file reads (db_fgetc -> gameMouseRefreshImmediately) -- so a hook-side proto read either
// re-enters the hook or nests inside the engine's own read. That's what crashed the game on map
// exit (see the guard on gpui_mouse_hook). One frame of staleness is irrelevant here: this only
// picks A's attack-vs-interact role and refreshes the post-kill loot cooldown anchor.
static bool gpui_hostile_in_range_cached = false;

// Set while the mouse hook is running, to stop it re-entering ITSELF through engine file I/O
// (see gpui_mouse_hook). Deliberately CLEARED again before the hook opens a modal screen: those
// run their own input loop, which must keep receiving gamepad input.
static bool gpui_in_mouse_hook = false;

// A/Y -> left mouse button, RB -> right mouse button, and left-stick navigation
// of hold-drag action popups. A's role is decided at the press edge and latched
// for the press: in a modal menu A always clicks; in gameplay A interacts (left
// click) only when no hostile is in attack range, otherwise it is left to
// combat_rt as an attack. Holding A (or Y) keeps the left button down, so the
// engine's own hold-to-open / drag / release-to-confirm model drives the world
// and inventory action popups; a quick tap is a single click (default action /
// take). Y is the same interact but ungated, so it works even with a hostile in
// range.
//
// RB is the engine's right-click: it toggles the cursor between move and action
// mode in the world (gmouse_handle_event) and between hand and arrow in the
// inventory (handle_inventory). The "action"/"arrow" mode is what makes a left
// click talk/loot/use an object or open the inventory item popup, so RB is how a
// controller reaches all of those. Injected on the press edge only (a single
// RIGHT_BUTTON_DOWN, no auto-repeat) so one press is exactly one mode toggle.
static void gpui_action_buttons(SDL_GameController* pad, int* dx, int* dy, int* buttons)
{
    static bool prevA = false;
    static bool prevRB = false;
    // Set on the A press edge when the tap is routed to looting an adjacent
    // corpse/container; suppresses the walk-click for that press.
    static bool aLooting = false;
    // Last time (ms) a hostile was in attack range or a lock was held -- the anchor for
    // the post-kill loot cooldown that keeps an A-spam tail from looting a fresh corpse.
    static unsigned int gpui_combat_a_time = 0;

    bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool rbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    bool aEdge = aNow && !prevA;
    bool rbEdge = rbNow && !prevRB;
    prevA = aNow;
    prevRB = rbNow;

    // While an inventory window is open, A is the quick-action/popup router
    // (gpui_inventory_a), not a raw left click -- so the generic A arbitration is
    // skipped and A never injects a plain left button here.
    bool invOpen = inven_any_open();
    if (invOpen) {
        gpui_a_interacting = false;
    } else {
        // Keep the combat-recency anchor fresh every frame there's something to fight,
        // so the loot cooldown measures time since the LAST enemy left range / died.
        if (gpui_hostile_in_range_cached || combat_rt_has_lock()) {
            gpui_combat_a_time = get_time();
        }
        bool postKillCooldown = get_time() - gpui_combat_a_time < GPUI_POST_KILL_LOOT_MS;

        if (aEdge) {
            // A-button arbitration: attack vs. world interact.
            //  - Menu / dialog ALWAYS interact (so A picks a button / reply; a stale lock or
            //    hostile-in-range left over from combat must not swallow the click).
            //  - A held LOCK (R3) always ATTACKS (the deliberate "shoot this" gesture; the fire
            //    simply no-ops if the locked target is out of range).
            //  - Otherwise: A INTERACTS when there's no hostile in weapon range (peaceful) OR when
            //    the dude is standing RIGHT AT an interactable (interact_prompt_target_adjacent --
            //    a door/corpse you walked up to overrides the fight). But if a hostile IS in range
            //    and the interactable is only NEARBY (not adjacent), A ATTACKS -- so plain A fires
            //    without needing a lock just because a neutral bystander/item sits a few tiles off
            //    ("plain A doesn't fire when I'm not aiming"). A hostile enemy is never itself an
            //    interact target (ip_classify returns IP_NONE for it), so it never routes to talk.
            gpui_a_interacting = gamepad_ui_menu_active()
                || dialog_active()
                || (!combat_rt_has_lock()
                    && (!gpui_hostile_in_range_cached
                        || interact_prompt_target_adjacent()));

            // In the world (not a menu, not a conversation), if A is interacting, route it to a
            // world interaction and suppress this press's walk-click. Do WHAT THE PROMPT HIGHLIGHTS
            // FIRST (interact_prompt_request -- door / scenery / item / talk / loot, already picked
            // by proximity+priority), and only fall back to the combat adjacent-corpse loot
            // (combat_rt_try_world_loot) when the prompt has nothing (it clears its target in
            // combat). The old order tried the corpse loot FIRST, so a highlighted DOOR opened a
            // nearby corpse instead -- the "I had a door highlighted but it looted an enemy" bug.
            // interact_prompt_request() returns false when nothing is highlighted, so the fallback
            // still works. Both just QUEUE the action -- it runs from the next tick.
            // The dialog_active() guard is critical: a conversation window is NOT modal (menu_active
            // is false), so without it, pressing A to pick a REPLY would instead interact/loot and
            // swallow the click -- the "can't select a dialogue answer" bug.
            // During the post-kill cooldown, swallow the press (aLooting = true suppresses the
            // walk-click) but DON'T loot/interact: short-circuit before either path so an A-spam tap
            // the instant an enemy dies can't open the corpse mid-fight. After GPUI_POST_KILL_LOOT_MS
            // with nothing to fight, A interacts/loots normally.
            // A non-corpse interactable in reach (a door / scenery / item / talkable NPC) is a
            // DELIBERATE world action and BYPASSES the post-kill loot cooldown -- otherwise a drawn
            // ranged weapon keeps a hostile "in range" all over the map, so gpui_combat_a_time is
            // refreshed every frame, the cooldown never ends, and it short-circuits before
            // interact_prompt_request() -- the "can't open a door with my gun out" bug. The cooldown
            // still guards CORPSE looting (its actual purpose: an A-spam tap the instant the last
            // enemy dies mustn't pop the fresh body), and the adjacent-corpse loot fallback.
            bool interactNow = false;
            if (gpui_a_interacting && !gamepad_ui_menu_active() && !dialog_active()) {
                if (interact_prompt_target() != NULL && !interact_prompt_offering_corpse()) {
                    interactNow = interact_prompt_request();
                } else if (!postKillCooldown) {
                    interactNow = interact_prompt_request() || combat_rt_try_world_loot();
                }
            }
            aLooting = gpui_a_interacting
                && !gamepad_ui_menu_active()
                && !dialog_active()
                && (postKillCooldown || interactNow);
        }
        if (!aNow) {
            gpui_a_interacting = false;
            aLooting = false;
        }

        if (aNow && gpui_a_interacting && !aLooting) {
            // Don't fire a WORLD walk-click at a hidden (idle) cursor -- the dude would
            // trek to an invisible tile. Reveal the cursor (right stick / RB) to click a
            // spot. Menus and dialogue still click normally: their pointer is the separate
            // low-level cursor, always visible, and A there selects a button / reply.
            bool worldClickAtHiddenCursor = gmouse_move_cursor_is_hidden()
                && !gamepad_ui_menu_active()
                && !dialog_active();
            if (!worldClickAtHiddenCursor) {
                *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
            }
        }
    }

    // Inside an inventory window Y is a manual raw left click alongside the A
    // router -- EXCEPT a loot window, where the A-router repurposes Y as the
    // whole-stack quick-take (a raw click there would re-trigger move_inventory's
    // drag). In gameplay Y instead swaps the active weapon (gpui_button_shortcuts),
    // so it does not click there; RT remains the gameplay raw-click escape hatch.
    bool yLootGrab = invOpen && inven_current_window_type() == INVENTORY_WINDOW_TYPE_LOOT;
    // In a barter window Y is the "offer" shortcut (barter_gamepad_input injects 'm'),
    // not a raw click -- otherwise it would also transfer the item under the cursor.
    bool yBarter = invOpen && inven_current_window_type() == INVENTORY_WINDOW_TYPE_TRADE;
    if (yNow && invOpen && !yLootGrab && !yBarter) {
        *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
    }

    // Inventory A router (no-op / self-resetting when no inventory is open).
    gpui_inventory_a(pad, buttons);

    // NOT while a radial wheel owns the screen: RB pages the weapon wheel there, and letting the
    // same press ALSO fire the engine's right-click would silently flip the world cursor's mode
    // behind the wheel -- you would put the gun away and find yourself in the wrong cursor.
    if (rbEdge && !radial_wheel_is_open()) {
        *buttons |= MOUSE_STATE_RIGHT_BUTTON_DOWN;
        // RB toggles which cursor is active -- reveal it briefly (like a right-stick
        // nudge) so the player sees the cursor they just switched to; it auto-hides again.
        gpui_cursor_show_time = get_time();
    }

    // Those popups are not GNW windows (no enumerable buttons); they read raw
    // mouse-Y motion, so feed the left stick in as cursor dy to navigate them.
    if (gpui_action_popup_active) {
        float ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
        if (ly > GPUI_STICK_DEADZONE || ly < -GPUI_STICK_DEADZONE) {
            *dy += (int)(ly * GPUI_POPUP_NAV_SPEED);
        }
    }
}

// Edge-detected face-button shortcuts for the engine's own modal screens:
// START toggles do_options() (the ESC pause menu: Save/Load/Preferences/Exit),
// The read-only fullscreen screens reachable from the D-pad during play: Pip-Boy (Up),
// character sheet (Down), automap (Left). They register WINDOW_MODAL and run their own
// get_input() loop; opening one sets gpui_in_dpad_screen so the nested gpui_button_shortcuts
// (which fires via the mouse hook inside that loop) routes B/BACK to the ESC it closes on.
// They have no controller navigation of their own -- open + back-out is the whole interaction.
enum {
    GPUI_DPAD_PIPBOY,
    GPUI_DPAD_CHAR,
    GPUI_DPAD_AUTOMAP,
    // Not a screen -- Right has no window to open, but it shares the same tap-vs-hold arbitration,
    // so it lives in the same direction space. gpui_open_dpad_screen is never called with it.
    GPUI_DPAD_END_COMBAT,
};

static bool gpui_in_dpad_screen = false;

// Which of the three is currently up, so the SAME D-pad direction closes it again (toggle).
// Only meaningful while gpui_in_dpad_screen is true.
static int gpui_dpad_screen_open = -1;

static void gpui_open_dpad_screen(int which)
{
    gpui_in_dpad_screen = true;
    gpui_dpad_screen_open = which;
    switch (which) {
    case GPUI_DPAD_PIPBOY:
        pipboy(false);
        break;
    case GPUI_DPAD_CHAR: {
        // Same iso-suspend the keyboard 'C' path uses around the character sheet.
        bool isoWasEnabled = map_disable_bk_processes();
        editor_design(false);
        if (isoWasEnabled) {
            map_enable_bk_processes();
        }
        break;
    }
    case GPUI_DPAD_AUTOMAP:
        automap(true, false);
        break;
    }
    gpui_in_dpad_screen = false;
    gpui_dpad_screen_open = -1;
}

// BACK/SELECT toggles handle_inventory(), and while the inventory is open X/B
// equip the item under the cursor into the left/right hand. All of these run
// their own get_input() modal loop, so this poll lives in the per-frame mouse
// hook (which fires inside every loop, including those nested screens).
//
// The D-pad opens the Pip-Boy (Up) / character sheet (Down) / automap (Left); B or BACK
// backs out of whichever is open. See gpui_open_dpad_screen above.
//
// Toggle: when the menu a button opens is already up, the same button injects
// KEY_ESCAPE into the input buffer, which the menu's own loop reads to close --
// no stacking. Opening is edge-triggered (one press = one open) and gated by
// intface_is_enabled() (the same guard the keyboard 'O'/'I' keys use) plus a
// no-other-modal check, so START/BACK do nothing in the main menu or sessions
// without an interface bar. The open-menu branches run BEFORE that gate because
// these screens disable the interface bar while up.
//
// Opening a screen synchronously re-enters mouse_info() from within
// mouse_info(); the engine's modal windows register WINDOW_MODAL before their
// loop starts, so the already-open checks are true for every nested frame and
// prevent recursion.
// Set by the quick-use wheel (gpui_quick_wheel) when a B HOLD opened it, so the tap handler
// below knows that press was a hold and must not also cycle the weapon's firing mode.
// Declared here because the tap handler is compiled before the wheel that sets it.
static bool gpui_quick_consumed_b = false;

// The same arrangement for Y: a HOLD opens the weapon wheel (gpui_weapon_wheel), so the hand-swap
// has to wait for the release. Set by ANYTHING that has already spent this Y press -- the wheel, or
// an LT+Y quick save -- so the release doesn't also swap hands. (Testing "is LT still down?" at
// release time is not enough: letting go of LT first would leak the swap through.)
static bool gpui_weapon_consumed_y = false;

// Same again for X: a HOLD opens the ammo wheel, so the plain reload has to wait for the release
// and only fire if the press was a tap.
static bool gpui_ammo_consumed_x = false;

// Defined with the skill wheel further down; B is the cancel for a sticky skill cursor.
static bool gpui_skill_cursor_armed();
static void gpui_cancel_skill_cursor();

// D-pad tap-vs-hold state (see the arbitration at the end of gpui_button_shortcuts). At FILE scope,
// not inside that block, because every path that bails out of the shortcuts early has to be able to
// FORGET a hold in progress: gpui_button_shortcuts returns before the D-pad block whenever a menu,
// a conversation or an inventory owns the screen, so a direction still held when one of those opens
// would otherwise look like a fresh release the moment gameplay resumed and fire a phantom tap.
static int gpui_dpad_held = -1;
static unsigned int gpui_dpad_down_time = 0;
static bool gpui_dpad_handled = false; // this hold already opened its screen

static void gpui_dpad_forget()
{
    gpui_dpad_held = -1;
    gpui_dpad_handled = false;
}

static void gpui_button_shortcuts(SDL_GameController* pad)
{
    static bool prevStart = false;
    static bool prevBack = false;
    static bool prevX = false;
    static bool prevB = false;
    static bool prevY = false;
    static bool prevUp = false;
    static bool prevDown = false;
    static bool prevLeft = false;

    bool startNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
    bool backNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    bool leftNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    bool rightNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;

    bool startPressed = startNow && !prevStart;
    bool backPressed = backNow && !prevBack;
    bool xPressed = xNow && !prevX;
    // X's reload moved to the RELEASE edge (tap-vs-hold: a hold opens the ammo wheel), so it needs
    // the press time to tell a tap from a hold -- same shape as B and Y below.
    bool xReleased = !xNow && prevX;
    static unsigned int xDownTime = 0;
    if (xPressed) {
        xDownTime = get_time();
    }
    bool bPressed = bNow && !prevB;
    bool bReleased = !bNow && prevB;
    bool yPressed = yNow && !prevY;

    // B's gameplay action moved to the RELEASE edge (tap-vs-hold: a hold opens the quick-use
    // wheel), so it needs the press time to tell a tap from a hold.
    static unsigned int bDownTime = 0;
    if (bPressed) {
        bDownTime = get_time();
    }

    // Y is now the same shape: hold = weapon wheel, tap = swap hands.
    bool yReleased = !yNow && prevY;
    static unsigned int yDownTime = 0;
    if (yPressed) {
        yDownTime = get_time();
    }
    // Only the three SCREEN directions still need a press edge (they close their own screen again);
    // Right and the tap-vs-hold arbitration below work off the live held state instead.
    bool upPressed = upNow && !prevUp;
    bool downPressed = downNow && !prevDown;
    bool leftPressed = leftNow && !prevLeft;
    prevStart = startNow;
    prevBack = backNow;
    prevX = xNow;
    prevB = bNow;
    prevY = yNow;
    prevUp = upNow;
    prevDown = downNow;
    prevLeft = leftNow;

    // Every state below that RETURNS before the D-pad arbitration must also forget a hold in
    // progress, or the direction still held when (say) a conversation starts would look like a
    // fresh release once gameplay resumed and fire a phantom action-cycle. Mirrors the guards
    // below deliberately -- one place to keep in step beats six scattered resets.
    //
    // ⚠️ gpui_in_dpad_screen is EXCLUDED, and that exclusion is the whole point of the state. The
    // hold that opened the Pip-Boy is still physically down while the Pip-Boy is up; forgetting it
    // would restart the timer the moment the screen closed and immediately reopen it. Keeping
    // gpui_dpad_handled set means that direction stays spent until it is genuinely released.
    if (in_main_menu || (!gpui_in_dpad_screen
            && (inven_any_open() || options_is_open() || gamepad_ui_menu_active()
                || dialog_active() || !intface_is_enabled() || gpui_action_popup_active))) {
        gpui_dpad_forget();
    }

    // The title menu drives its OWN D-pad highlight nav and has no game session, so the gameplay
    // shortcuts must NOT run here: D-pad Down would open the character sheet, Up the Pip-Boy, Left
    // the automap, BACK the inventory, START the pause menu -- "with the D-pad the char screen (and
    // other in-game windows) open in the main menu." The interface-bar guard further down does not
    // catch this because intface can still read enabled after returning from a game, and the title
    // window is not modal (so gamepad_ui_menu_active() is false too). Edges are tracked above, so a
    // button still held when gameplay actually starts is not misread as a fresh press.
    if (in_main_menu) {
        return;
    }

    // Inside a D-pad-opened read-only screen (Pip-Boy / char sheet / automap): B or BACK backs
    // out (injects the ESC their loop closes on). Handled first so nothing else fires there.
    //
    // The D-pad direction that OPENED the screen also closes it, so each one is a true toggle --
    // "pressing the D-pad again doesn't close the windows". Safe against the opening press being
    // read twice: prevUp/prevDown/prevLeft are stamped below (before the open call), and the
    // screen's nested loop reaches this function through the same statics, so the button must be
    // physically released and pressed again to produce an edge in here. The other two directions
    // deliberately do nothing -- these screens run nested modal loops, so switching between them
    // would stack rather than swap.
    if (gpui_in_dpad_screen) {
        bool sameDirection = (gpui_dpad_screen_open == GPUI_DPAD_PIPBOY && upPressed)
            || (gpui_dpad_screen_open == GPUI_DPAD_CHAR && downPressed)
            || (gpui_dpad_screen_open == GPUI_DPAD_AUTOMAP && leftPressed);
        if (bPressed || backPressed || sameDirection) {
            GNW_add_input_buffer(KEY_ESCAPE);
        }
        return;
    }

    // Inventory open: BACK toggles it shut; X/B equip the item under the cursor.
    if (inven_is_open()) {
        if (backPressed) {
            GNW_add_input_buffer(KEY_ESCAPE);
        } else if (xPressed) {
            inven_equip_under_cursor(HAND_LEFT);
        } else if (bPressed) {
            inven_equip_under_cursor(HAND_RIGHT);
        }
        return;
    }

    // A loot/container window is modal (so inven_is_open(), which is the normal
    // inventory only, is false here). B closes it: its Done button emits
    // KEY_ESCAPE, which the loot loop breaks on -- same as the BACK escape hatch.
    if (inven_any_open() && inven_current_window_type() == INVENTORY_WINDOW_TYPE_LOOT) {
        if (bPressed) {
            GNW_add_input_buffer(KEY_ESCAPE);
        }
        return;
    }

    // A barter/trade window runs its own controller loop (barter_gamepad_input handles
    // LB/RB cursor, D-pad scroll, Y offer, B/BACK leave). Bail so BACK doesn't open the
    // normal inventory and the D-pad doesn't open Pip-Boy/char/automap over the trade.
    if (inven_any_open() && inven_current_window_type() == INVENTORY_WINDOW_TYPE_TRADE) {
        return;
    }

    // Options/pause menu open: START toggles it shut.
    if (options_is_open()) {
        if (startPressed) {
            GNW_add_input_buffer(KEY_ESCAPE);
        }
        return;
    }

    // Neither shortcut menu open: don't stack onto another modal, and require an
    // interactive interface bar. Also bail during a conversation: the interface bar
    // stays "enabled" through a talking-head, but map bk processes are disabled, so a
    // weapon swap (intface_toggle_items -> intface_change_fid_animate) would block
    // forever waiting on a put-away/take-out animation that can never advance = hang.
    if (gamepad_ui_menu_active() || dialog_active() || !intface_is_enabled()) {
        return;
    }

    // BACK/START open inventory/options directly on press. (They used to be deferred to
    // release so a BACK+START quick-save combo could pre-empt them, but that combo collided
    // with Steam Big Picture / Xbox Game Bar grabbing BACK -- quick save now lives on LT+Y.)
    if (backPressed) {
        gsound_play_sfx_file("ib1p1xx1");
        handle_inventory();
        return;
    }
    if (startPressed) {
        gsound_play_sfx_file("ib1p1xx1");
        do_options();
        return;
    }

    // D-pad is TAP-vs-HOLD (not while a hold-drag action popup is navigating, which also owns the
    // D-pad):
    //
    //   HOLD a direction -> the standalone screen it always opened. Up = Pip-Boy (blocked in
    //     combat, like keyboard 'P'), Down = character sheet, Left = automap, Right = END COMBAT
    //     (the pad's half of vanilla's Enter; combat_rt applies vanilla's "does every enemy want to
    //     stop?" test and prints the refusal message itself, so the buzz just marks the refusal).
    //   TAP a direction -> step which action the interact prompt offers on the highlighted object,
    //     which is what the floating icon shows. Before this, that icon was permanently the hand
    //     and LOOK had no controller route at all.
    //
    // The screens move to the HOLD side rather than the tap side because the tap is the frequent
    // action here, and a hold that opens a full-screen modal is far more forgiving of a slip than a
    // tap that does. Opening runs a NESTED modal loop from inside this call; prevUp/prevDown/... are
    // stamped above (before the open), and that loop re-enters this function through the same
    // statics, so the still-held button produces no further edge -- see the gpui_in_dpad_screen
    // branch, which keeps the same-direction PRESS as the close toggle.
    if (!gpui_action_popup_active) {
        // Which direction is being held, and since when -- see gpui_dpad_held. Only one direction at
        // a time: a diagonal roll would otherwise arm two screens at once.
        int dirNow = -1;
        if (upNow) {
            dirNow = GPUI_DPAD_PIPBOY;
        } else if (downNow) {
            dirNow = GPUI_DPAD_CHAR;
        } else if (leftNow) {
            dirNow = GPUI_DPAD_AUTOMAP;
        } else if (rightNow) {
            dirNow = GPUI_DPAD_END_COMBAT;
        }

        if (dirNow != gpui_dpad_held) {
            // A RELEASE (or a slide to another direction) that never reached the hold threshold is
            // the tap: cycle the prompt's action. Up/Left step backwards, Down/Right forwards, so
            // both axes read the way the ring is laid out. Refuse audibly when there is nothing
            // highlighted or it has only one action, so the D-pad never just silently does nothing.
            if (gpui_dpad_held != -1 && !gpui_dpad_handled) {
                bool back = gpui_dpad_held == GPUI_DPAD_PIPBOY || gpui_dpad_held == GPUI_DPAD_AUTOMAP;
                if (interact_prompt_cycle(back ? -1 : 1)) {
                    gsound_play_sfx_file("ib1p1xx1");
                } else {
                    gsound_play_sfx_file("iisxxxx1");
                }
            }
            gpui_dpad_held = dirNow;
            gpui_dpad_down_time = get_time();
            gpui_dpad_handled = false;
        }

        if (dirNow != -1 && !gpui_dpad_handled && get_time() - gpui_dpad_down_time >= GPUI_DPAD_HOLD_MS) {
            gpui_dpad_handled = true;
            switch (dirNow) {
            case GPUI_DPAD_PIPBOY:
                if (isInCombat()) {
                    gsound_play_sfx_file("iisxxxx1"); // Pip-Boy not available in combat
                } else {
                    gsound_play_sfx_file("ib1p1xx1");
                    gpui_open_dpad_screen(GPUI_DPAD_PIPBOY);
                }
                break;
            case GPUI_DPAD_CHAR:
                gsound_play_sfx_file("ib1p1xx1");
                gpui_open_dpad_screen(GPUI_DPAD_CHAR);
                break;
            case GPUI_DPAD_AUTOMAP:
                gsound_play_sfx_file("ib1p1xx1");
                gpui_open_dpad_screen(GPUI_DPAD_AUTOMAP);
                break;
            case GPUI_DPAD_END_COMBAT:
                if (combat_rt_end_combat()) {
                    gsound_play_sfx_file("ib1p1xx1");
                } else {
                    gsound_play_sfx_file("iisxxxx1");
                }
                break;
            }
            return;
        }
    }

    // Quick save = hold LT (left trigger) + press Y (emulator-style). LT is otherwise
    // unused in gameplay and -- unlike BACK/START -- is not intercepted by Steam/Xbox
    // overlays. The LT modifier suppresses Y's normal weapon swap for that press.
    bool ltNow = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GPUI_TRIGGER_THRESHOLD;

    // Quick save stays on the PRESS edge -- the LT modifier already makes the gesture unambiguous
    // (the weapon wheel refuses to open while LT is held), so there is nothing to disambiguate and
    // no reason to make a save wait for the release. Mark the press spent so the release below
    // doesn't also swap hands.
    if (yPressed && ltNow) {
        gsound_play_sfx_file("ib1p1xx1");
        gpui_weapon_consumed_y = true;
        QuickSaveGame();
    }

    // X is TAP-vs-HOLD like B and Y: a hold opens the ammo wheel (gpui_ammo_wheel), so the plain
    // reload waits for the release and only fires if this was a tap. Release edge, so deliberately
    // outside the press chain above.
    if (xReleased) {
        if (gpui_ammo_consumed_x) {
            // This press opened (and released out of) the ammo wheel.
            gpui_ammo_consumed_x = false;
        } else if (get_time() - xDownTime < GPUI_QUICK_HOLD_MS) {
            // Reload the active-hand weapon from inventory ammo. In realtime combat this
            // costs AP (vanilla reload = 2) from combat_rt's pool -- you can't reload on an
            // empty pool; out of combat the pool is full so it's effectively free. Charge
            // only when a reload actually happened (had ammo / wasn't already full).
            if (combat_rt_player_ap() >= 2) {
                if (intface_reload_weapon() == 0) {
                    combat_rt_spend_ap(2);
                }
            }
        }
    }

    // B is TAP-vs-HOLD: a hold brings up the quick-use wheel (gpui_quick_wheel), so the
    // firing-mode cycle has to wait for the release and only fire if this was a tap. Every
    // other pad button was already taken; firing mode is the least time-critical of them,
    // which is why it is the one that got split.
    //
    // Deliberately NOT part of the y/x else-if chain above: those are press edges and this
    // is a release edge, so they can legitimately land on the same frame. Chaining them
    // would let a simultaneous X/Y press swallow the release and strand
    // gpui_quick_consumed_b set, which would then eat the NEXT B tap.
    if (bReleased) {
        if (gpui_aim_swallow_b) {
            // This press already did its job: it cancelled the aim picker. Letting it fall
            // through would silently change the weapon's firing mode as well.
            gpui_aim_swallow_b = false;
        } else if (gpui_quick_consumed_b) {
            // The press opened (and released out of) the quick-use wheel.
            gpui_quick_consumed_b = false;
        } else if (gpui_skill_cursor_armed()) {
            // PUT THE SKILL AWAY. A skill armed from the wheel now survives being used, so it
            // needs an explicit way out or you would be stuck aiming a lockpick at the world. B is
            // the natural one -- it is already "back out of this" everywhere else on the pad -- and
            // the firing-mode cycle it normally does is meaningless while a skill cursor is up.
            gsound_play_sfx_file("ib1p1xx1");
            gpui_cancel_skill_cursor();
        } else if (get_time() - bDownTime < GPUI_QUICK_HOLD_MS) {
            // Cycle the active weapon's firing mode, mirroring the keyboard 'N'. B's inventory
            // role (equip right hand) is handled in the inven_is_open() branch above, so this is
            // gameplay-only. In realtime the cycle is only single/burst -- the aiming and reload
            // entries are skipped as inert (see intface_item_action_available), so a single-mode
            // weapon has nothing to switch to and says so instead of beeping a confirmation at a
            // cycle that changes nothing.
            if (intface_item_can_toggle_state()) {
                gsound_play_sfx_file("ib1p1xx1");
                intface_toggle_item_state();
            } else {
                gsound_play_sfx_file("iisxxxx1");
            }
        }
    }

    // Y, same shape as B: the hand swap is a TAP, because a HOLD belongs to the weapon wheel.
    // Also a release edge, so deliberately outside the y/x press chain above.
    if (yReleased) {
        if (gpui_weapon_consumed_y) {
            // Already spent -- this press opened the weapon wheel, or saved the game.
            gpui_weapon_consumed_y = false;
        } else if (get_time() - yDownTime < GPUI_QUICK_HOLD_MS) {
            // Swap the active weapon hand (left <-> right).
            intface_toggle_items(true);
        }
    }
}

// --- L3 skill wheel (a radial_wheel instance). ---
//
// Hold L3 (left-stick click) to bring up a radial menu of the eight active skills; aim a
// segment with the RIGHT stick; release L3 to pick it. Sneak is a toggle (applied
// immediately, no target); the other seven arm the engine's skill cursor so the player
// then aims and presses A on the target (reusing gmouse's existing skill-on-target path).
// The wheel touches a GNW window, so it is driven from gamepad_ui_tick() (the main loop),
// NEVER from the mouse hook.

// The eight skills on the wheel, clockwise from the top.
static const int gpui_skill_wheel_order[8] = {
    SKILL_LOCKPICK,
    SKILL_STEAL,
    SKILL_FIRST_AID,
    SKILL_DOCTOR,
    SKILL_SCIENCE,
    SKILL_REPAIR,
    SKILL_TRAPS,
    SKILL_SNEAK,
};

// Each skill DOES have its own picture -- `skill_pic(skill)` (skill.cc, the `art_num` column of
// skill_data[]) is the OBJ_TYPE_SKILLDEX frame the CHARACTER SHEET draws on its info card. It is
// the SKILLDEX SCREEN that has no icons, which is a different thing; the wheel uses the card art.

// Reused skilldex art so the wheel segments look like real Fallout buttons and show each
// skill's level: FRM 120 = unpressed button (normal), 119 = pressed button (highlighted),
// 170 = the big-number digit strip (glyph d at x = d*14, see skilldex.cc). There are NO
// per-skill icons in FO1 -- the skilldex identifies skills by name, which the wheel label
// already shows. Locked once (first wheel open, when the art cache is ready) and kept
// locked for the wheel's lifetime; released in gamepad_ui_exit. On failure the wheel
// silently falls back to its plain text look.
static bool gpui_skill_art_loaded = false;
static CacheEntry* gpui_btn_up_key = NULL;
static CacheEntry* gpui_btn_down_key = NULL;
static CacheEntry* gpui_digits_key = NULL;
static RadialWheelArt gpui_skill_art;

static void gpui_load_skill_art()
{
    if (gpui_skill_art_loaded) {
        return;
    }

    memset(&gpui_skill_art, 0, sizeof(gpui_skill_art));

    int w;
    int h;
    unsigned char* up = art_lock(art_id(OBJ_TYPE_INTERFACE, 120, 0, 0, 0), &gpui_btn_up_key, &w, &h);
    if (up == NULL) {
        // Art cache not ready yet -- leave unloaded so a later open retries.
        return;
    }
    gpui_skill_art.buttonUp = up;
    gpui_skill_art.buttonWidth = w;
    gpui_skill_art.buttonHeight = h;

    int w2;
    int h2;
    unsigned char* down = art_lock(art_id(OBJ_TYPE_INTERFACE, 119, 0, 0, 0), &gpui_btn_down_key, &w2, &h2);
    if (down != NULL && w2 == w && h2 == h) {
        gpui_skill_art.buttonDown = down;
    } else if (down != NULL) {
        art_ptr_unlock(gpui_btn_down_key);
        gpui_btn_down_key = NULL;
    }

    int dw;
    int dh;
    unsigned char* digits = art_lock(art_id(OBJ_TYPE_INTERFACE, 170, 0, 0, 0), &gpui_digits_key, &dw, &dh);
    if (digits != NULL) {
        gpui_skill_art.digits = digits;
        gpui_skill_art.digitWidth = 14; // glyph cell width in the strip (skilldex constant)
        gpui_skill_art.digitHeight = dh;
        gpui_skill_art.digitsPitch = dw;
    }

    gpui_skill_art_loaded = true;
}

static void gpui_free_skill_art()
{
    if (gpui_btn_up_key != NULL) {
        art_ptr_unlock(gpui_btn_up_key);
        gpui_btn_up_key = NULL;
    }
    if (gpui_btn_down_key != NULL) {
        art_ptr_unlock(gpui_btn_down_key);
        gpui_btn_down_key = NULL;
    }
    if (gpui_digits_key != NULL) {
        art_ptr_unlock(gpui_digits_key);
        gpui_digits_key = NULL;
    }
    memset(&gpui_skill_art, 0, sizeof(gpui_skill_art));
    gpui_skill_art_loaded = false;
}

// --- Per-segment inventory-art icons, shared by every wheel that shows items ---
//
// The radial_wheel is a SINGLE shared overlay (gpui_wheel_owner), so at most one wheel can be
// showing icons at any moment and one slot array is enough for all of them -- which also makes
// that invariant load-bearing instead of merely true today. Locked per wheel OPENING rather than
// once at startup: these are full-size FRMs and the art cache is free to evict them in between.
static CacheEntry* gpui_icon_key[RADIAL_WHEEL_MAX];
// A slot holds EITHER a locked cache entry above (art used at 1:1) OR a malloc'd scaled copy here,
// never both -- art that has to be resized cannot stay a cache pointer.
static unsigned char* gpui_icon_owned[RADIAL_WHEEL_MAX];
static bool gpui_icons_held = false;

static void gpui_icons_release()
{
    if (!gpui_icons_held) {
        return;
    }
    for (int i = 0; i < RADIAL_WHEEL_MAX; i++) {
        if (gpui_icon_key[i] != NULL) {
            art_ptr_unlock(gpui_icon_key[i]);
            gpui_icon_key[i] = NULL;
        }
        if (gpui_icon_owned[i] != NULL) {
            free(gpui_icon_owned[i]);
            gpui_icon_owned[i] = NULL;
        }
    }
    gpui_icons_held = false;
}

// Give segment `index` the inventory sprite of item proto `pid`. A missing proto or missing art
// just leaves that segment icon-less -- the wheel falls back to drawing its label, never blits a
// dangling pointer (RadialSegment must be zero-initialised by the caller, which every caller is).
static void gpui_icon_from_pid(RadialSegment* segs, int index, int pid)
{
    if (index < 0 || index >= RADIAL_WHEEL_MAX) {
        return;
    }

    Proto* proto;
    if (proto_ptr(pid, &proto) != 0) {
        return;
    }

    int w;
    int h;
    unsigned char* icon = art_lock(proto->item.inventoryFid, &(gpui_icon_key[index]), &w, &h);
    if (icon != NULL) {
        segs[index].icon = icon;
        segs[index].iconWidth = w;
        segs[index].iconHeight = h;
        gpui_icons_held = true;
    }
}

// Give segment `index` an interface FRM (the action-menu icon set: hand, eye, talk face...), used
// at 1:1 -- those are already icon-sized. Same failure contract as gpui_icon_from_pid.
static void gpui_icon_from_interface(RadialSegment* segs, int index, int frmId)
{
    if (index < 0 || index >= RADIAL_WHEEL_MAX) {
        return;
    }

    int w;
    int h;
    unsigned char* icon = art_lock(art_id(OBJ_TYPE_INTERFACE, frmId, 0, 0, 0), &(gpui_icon_key[index]), &w, &h);
    if (icon != NULL) {
        segs[index].icon = icon;
        segs[index].iconWidth = w;
        segs[index].iconHeight = h;
        gpui_icons_held = true;
    }
}

// A skill's own picture -- the one the CHARACTER SHEET shows on its info card (editor.cc DrawCard,
// `art_id(OBJ_TYPE_SKILLDEX, skill_pic(skill), ...)`). Those cards are drawn into a 156x171 slot,
// far too big for eight of them in a ring, so each is scaled into a copy WE own. Cap matches the
// drug wheel's inventory FRMs (~88x59), where the ring already reads well.
#define GPUI_CARD_ICON_MAX_W 88
#define GPUI_CARD_ICON_MAX_H 64

static void gpui_icon_from_skilldex(RadialSegment* segs, int index, int frmId)
{
    if (index < 0 || index >= RADIAL_WHEEL_MAX) {
        return;
    }

    int w;
    int h;
    CacheEntry* key;
    unsigned char* src = art_lock(art_id(OBJ_TYPE_SKILLDEX, frmId, 0, 0, 0), &key, &w, &h);
    if (src == NULL) {
        return;
    }
    if (w <= 0 || h <= 0) {
        art_ptr_unlock(key);
        return;
    }

    // Fit the cell with the aspect kept. Never scale UP: a card already smaller than the cap is
    // the size the artist drew, and stretching it only softens it.
    int dw = w;
    int dh = h;
    if (dw > GPUI_CARD_ICON_MAX_W) {
        dh = dh * GPUI_CARD_ICON_MAX_W / dw;
        dw = GPUI_CARD_ICON_MAX_W;
    }
    if (dh > GPUI_CARD_ICON_MAX_H) {
        dw = dw * GPUI_CARD_ICON_MAX_H / dh;
        dh = GPUI_CARD_ICON_MAX_H;
    }
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }

    unsigned char* dst = (unsigned char*)malloc((size_t)dw * dh);
    if (dst == NULL) {
        art_ptr_unlock(key);
        return;
    }

    // ⚠️ trans_cscale only WRITES non-zero source pixels (that is what makes it transparent-aware),
    // so the destination must START as index 0 or every gap in the picture keeps whatever malloc
    // happened to leave there. Same discipline as the aimed-shot figure.
    memset(dst, 0, (size_t)dw * dh);
    trans_cscale(src, w, h, w, dst, dw, dh, dw);

    // We own a scaled copy now, so the cache entry is free to go immediately.
    art_ptr_unlock(key);

    gpui_icon_owned[index] = dst;
    segs[index].icon = dst;
    segs[index].iconWidth = dw;
    segs[index].iconHeight = dh;
    gpui_icons_held = true;
}

// Icons are far bigger than the 86x33 skilldex button and would swallow it, so a wheel that has
// them keeps ONLY the digit strip (the number under each cell) and drops the button frame.
// Returns the art to hand radial_wheel_open, or NULL when nothing loaded at all.
static const RadialWheelArt* gpui_wheel_art_for_icons(RadialWheelArt* out, bool haveIcons)
{
    memset(out, 0, sizeof(*out));
    if (!gpui_skill_art_loaded) {
        return NULL;
    }

    *out = gpui_skill_art;
    if (haveIcons) {
        out->buttonUp = NULL;
        out->buttonDown = NULL;
        out->buttonWidth = 0;
        out->buttonHeight = 0;
    }
    return out;
}

// The cursor mode that arms a given skill, or -1 if it has none (Sneak).
static int gpui_skill_cursor_mode(int skill)
{
    switch (skill) {
    case SKILL_FIRST_AID:
        return GAME_MOUSE_MODE_USE_FIRST_AID;
    case SKILL_DOCTOR:
        return GAME_MOUSE_MODE_USE_DOCTOR;
    case SKILL_LOCKPICK:
        return GAME_MOUSE_MODE_USE_LOCKPICK;
    case SKILL_STEAL:
        return GAME_MOUSE_MODE_USE_STEAL;
    case SKILL_TRAPS:
        return GAME_MOUSE_MODE_USE_TRAPS;
    case SKILL_SCIENCE:
        return GAME_MOUSE_MODE_USE_SCIENCE;
    case SKILL_REPAIR:
        return GAME_MOUSE_MODE_USE_REPAIR;
    default:
        return -1;
    }
}

static void gpui_open_skill_wheel()
{
    // Targeted skills are refused by action_use_skill_on() while isInCombat(); only
    // Sneak (a toggle) works mid-fight, so disable the rest when engaged (drawn red).
    bool inCombat = isInCombat();

    gpui_icons_release();
    gpui_load_skill_art();

    // Zeroed, not just filled in: RadialSegment carries optional fields (the per-segment icon
    // pointer among them) and a stack array left uninitialised would hand the wheel garbage
    // to blit from.
    RadialSegment segs[8];
    memset(segs, 0, sizeof(segs));
    for (int i = 0; i < 8; i++) {
        int skill = gpui_skill_wheel_order[i];
        segs[i].label = skill_name(skill);
        segs[i].id = skill;
        segs[i].enabled = (skill == SKILL_SNEAK) || !inCombat;
        int level = skill_level(obj_dude, skill);
        segs[i].value = level < 0 ? 0 : level;
        gpui_icon_from_skilldex(segs, i, skill_pic(skill));
    }

    // With icons the wheel switches to its icon look: the label of the segment you are aiming at
    // moves to the hub (eight names spread round a ring overlap into mush) while the skill % keeps
    // drawing under each cell. If no icon art loaded this is exactly the old button look.
    RadialWheelArt art;
    radial_wheel_open(segs, 8, "SKILLS", gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    if (!radial_wheel_is_open()) {
        gpui_icons_release();
    }
}

// A skill cursor armed from the wheel STAYS armed after it is applied -- see
// gamepad_ui_skill_cursor_sticky() and the gmouse dispatch that reads it.
static bool gpui_skill_sticky = false;

bool gamepad_ui_skill_cursor_sticky()
{
    return gpui_skill_sticky;
}

// Is one of the seven targeted skill cursors currently armed? (Sneak is a toggle and has no
// cursor, so it never shows up here.)
static bool gpui_skill_cursor_armed()
{
    int mode = gmouse_3d_get_mode();
    return mode >= FIRST_GAME_MOUSE_MODE_SKILL && mode < GAME_MOUSE_MODE_COUNT;
}

// Put the cursor back to plain MOVE and stop it re-arming. The explicit way out of a sticky skill.
static void gpui_cancel_skill_cursor()
{
    gpui_skill_sticky = false;
    if (gpui_skill_cursor_armed()) {
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
    }
}

static void gpui_use_skill(int skill)
{
    if (skill == SKILL_SNEAK) {
        // Toggle PC_FLAG_SNEAKING -- lightweight, no target, safe from the frame tick.
        action_skill_use(SKILL_SNEAK);
        return;
    }

    // Arm the engine's skill cursor. The player aims (right stick -> cursor) and presses
    // A (a left click), which gmouse applies via action_use_skill_on() -- the whole targeting path
    // is reused, no new targeting code. gmouse_3d_set_mode is a no-op unless 3d modes are enabled,
    // so make sure they are first (they normally are during exploration, but this guards the case
    // where they've been turned off).
    int mode = gpui_skill_cursor_mode(skill);
    if (mode != -1) {
        if (gmouse_3d_modes_are_enabled() == 0) {
            gmouse_3d_enable_modes();
        }
        // Picking the SAME skill that is already armed puts it away again -- the wheel is then its
        // own toggle, which is the shortest cancel when your thumb is already on L3.
        if (gpui_skill_cursor_armed() && gmouse_3d_get_mode() == mode) {
            gpui_cancel_skill_cursor();
            return;
        }
        gmouse_3d_set_mode(mode);
        // STICKY from here: gmouse will not revert to MOVE after applying it, so the skill can be
        // used again (another lock, the next trap, a second patient) without re-opening the wheel.
        gpui_skill_sticky = true;
    }
}

// --- LB aimed-shot wheel (a second radial_wheel instance). ---
//
// In combat, hold LB to bring up a wheel of the eight body parts; aim a segment with the
// RIGHT stick; release LB to fire an aimed shot at the lock-on target (or nearest hostile
// in range). Each segment shows the to-hit % for that part. Combat-only and weapon-gated
// via combat_rt_can_aim(). Reuses the same skilldex button art as the skill wheel.

// The radial_wheel is a single shared overlay, so only one of the two wheels may own it
// at a time. This tracks which driver opened the current overlay so the other ignores it.
enum {
    GPUI_WHEEL_NONE = 0,
    GPUI_WHEEL_SKILL,
    GPUI_WHEEL_AIM,
    GPUI_WHEEL_QUICK,
    GPUI_WHEEL_WEAPON,
    GPUI_WHEEL_AMMO,
};
static int gpui_wheel_owner = GPUI_WHEEL_NONE;

// The eight body parts, clockwise from the top (head up, groin down, limbs on the sides).
static const struct {
    const char* label;
    int location;
} gpui_aim_order[8] = {
    { "Head", HIT_LOCATION_HEAD },
    { "Eyes", HIT_LOCATION_EYES },
    { "R.Arm", HIT_LOCATION_RIGHT_ARM },
    { "R.Leg", HIT_LOCATION_RIGHT_LEG },
    { "Groin", HIT_LOCATION_GROIN },
    { "L.Leg", HIT_LOCATION_LEFT_LEG },
    { "L.Arm", HIT_LOCATION_LEFT_ARM },
    { "Torso", HIT_LOCATION_TORSO },
};

// The target's body picture, scaled to sit in the wheel hub. Reuses the engine's
// called-shot critter art (ANIM_CALLED_SHOT_PIC, the same figure the turn-based aimed-shot
// screen shows). Built per open from the current target and freed on close. Scaled down so
// the body-part labels around it have room.
#define GPUI_AIM_FIG_MAX_W 120
#define GPUI_AIM_FIG_MAX_H 150
static unsigned char* gpui_aim_fig = NULL; // malloc'd scaled copy (transparent, index 0 = clear)
static int gpui_aim_fig_w = 0;
static int gpui_aim_fig_h = 0;

static void gpui_release_aim_figure()
{
    if (gpui_aim_fig != NULL) {
        mem_free(gpui_aim_fig);
        gpui_aim_fig = NULL;
    }
    gpui_aim_fig_w = 0;
    gpui_aim_fig_h = 0;
}

static void gpui_load_aim_figure(Object* target)
{
    gpui_release_aim_figure();
    if (target == NULL) {
        return;
    }

    CacheEntry* key;
    int w;
    int h;
    int fid = art_id(OBJ_TYPE_CRITTER, target->fid & 0xFFF, ANIM_CALLED_SHOT_PIC, 0, 0);
    unsigned char* src = art_lock(fid, &key, &w, &h);
    if (src == NULL) {
        return; // this critter has no called-shot picture -- hub just stays empty
    }

    // Fit within the hub box, preserving aspect, never upscaling.
    float scale = 1.0f;
    if (w > GPUI_AIM_FIG_MAX_W) {
        scale = (float)GPUI_AIM_FIG_MAX_W / w;
    }
    if ((float)h * scale > GPUI_AIM_FIG_MAX_H) {
        scale = (float)GPUI_AIM_FIG_MAX_H / h;
    }
    int sw = (int)(w * scale);
    int sh = (int)(h * scale);
    if (sw < 1) {
        sw = 1;
    }
    if (sh < 1) {
        sh = 1;
    }

    gpui_aim_fig = (unsigned char*)mem_malloc(sw * sh);
    if (gpui_aim_fig != NULL) {
        memset(gpui_aim_fig, 0, sw * sh); // transparent background for the trans scale
        trans_cscale(src, w, h, w, gpui_aim_fig, sw, sh, sw);
        gpui_aim_fig_w = sw;
        gpui_aim_fig_h = sh;
    }

    art_ptr_unlock(key);
}

static void gpui_open_aim_wheel()
{
    gpui_load_skill_art(); // same button/digit art as the skill wheel

    Object* target = combat_rt_aim_target();
    gpui_load_aim_figure(target);

    // Anchor a SMALL opaque panel flush to the LEFT edge, vertically centered: passing x=0
    // makes win_move clamp the (off-screen-left) panel against the left margin. This keeps it
    // off the enemy you're aiming at -- the enemy stays green-outlined out on the battlefield,
    // clearly visible -- instead of a big black block sitting on top of it.
    int atX = 0;
    int atY = screenGetHeight() / 2;

    RadialSegment segs[8];
    memset(segs, 0, sizeof(segs)); // see gpui_open_skill_wheel: optional fields must be zeroed
    for (int i = 0; i < 8; i++) {
        segs[i].label = gpui_aim_order[i].label;
        segs[i].id = gpui_aim_order[i].location;
        segs[i].enabled = true;
        segs[i].value = combat_rt_aim_chance(gpui_aim_order[i].location); // to-hit %
    }

    // Plain compact text look (no skilldex buttons / hub figure): each label shows its
    // to-hit % inline, so the panel stays small and readable at the screen edge.
    radial_wheel_open(segs, 8, "AIM", NULL, atX, atY, false);
}

// Drive the wheel from the per-frame safe tick (gamepad_ui_tick). Open on the L3 press
// edge, aim while held, execute + close on release.
static void gpui_skill_wheel(SDL_GameController* pad)
{
    static bool prevL3 = false;
    bool l3Now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
    bool l3Edge = l3Now && !prevL3;
    bool l3Released = !l3Now && prevL3;
    prevL3 = l3Now;

    // Ignore the shared overlay while the aim wheel owns it.
    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_SKILL) {
        return;
    }

    // Don't bring up (or keep up) the wheel over a modal menu / inventory / a disabled
    // interface bar.
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled();

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (l3Released) {
            int skill = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (skill != -1) {
                gpui_use_skill(skill);
            }
        } else {
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / GPUI_AXIS_MAX;
            radial_wheel_update(x, y);
        }
        return;
    }

    if (l3Edge && !blocked) {
        gpui_open_skill_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_SKILL;
        }
    }
}

// --- B-hold quick-use wheel (a third radial_wheel instance). ---
//
// Hold B to bring up a wheel of the healing/combat drugs; aim a segment with the LEFT
// stick (B is a face button -- the right thumb is busy holding the wheel open and cannot
// work the right stick as well); release B to use it. A short TAP of B keeps its old job (cycling the
// weapon's firing mode) -- see the release handling in gpui_button_shortcuts. Every button
// on the pad was already spoken for, and firing mode is the least time-critical of them,
// so it is the one that got split into tap-vs-hold.
//
// SLOTS ARE PIDs, NEVER Object*. A pointer would dangle the instant the item is consumed,
// and load/save reallocates the whole object graph. Holding a PID means "use the next one
// of these I'm carrying", which survives consuming, looting, trading and reloading.
//
// The list is BUILT FROM THE INVENTORY: only drugs you are actually carrying get a segment.
//
// ⚠️ THIS REPLACED A FIXED EIGHT-SLOT LIST, and the trade is deliberate. The fixed list bought
// muscle memory (the same drug always on the same segment) at two prices the player hit in play:
// it showed dead segments for drugs he had none of ("když držím B vidím i drogy které nemám"), and
// it could never show a drug that wasn't one of its eight -- Rad-X, Jet, Healing Powder were
// unreachable no matter how many you carried. The weapon wheel already made the opposite choice for
// the same reason; this now matches it.
//
// What survives of the old design is the ORDER: gpui_quick_order is still the priority table, so
// among the drugs you carry the healing ones still come first and stay in the same relative places.
// Anything not in that table is appended in inventory order. So positions shift only when you run
// out of something or pick up a new kind, not on every use.

#define GPUI_QUICK_MAX 8

// Clockwise from the top. Healing first (what you actually reach for under fire), then the
// rad meds, then the combat chems. Drugs outside this table still appear, just after it.
static const int gpui_quick_order[] = {
    PROTO_ID_STIMPACK,
    PROTO_ID_SUPER_STIMPACK,
    PROTO_ID_RADAWAY,
    PROTO_ID_MENTATS,
    PROTO_ID_BUFF_OUT,
    PROTO_ID_PSYCHO,
    PROTO_ID_NUKA_COLA,
    PROTO_ID_BEER,
};

#define GPUI_QUICK_ORDER_COUNT ((int)(sizeof(gpui_quick_order) / sizeof(gpui_quick_order[0])))

// Rank of `pid` in the priority table, or the table size for anything not listed (so unknown drugs
// sort after the curated ones, keeping their inventory order among themselves).
static int gpui_quick_rank(int pid)
{
    for (int i = 0; i < GPUI_QUICK_ORDER_COUNT; i++) {
        if (gpui_quick_order[i] == pid) {
            return i;
        }
    }
    return GPUI_QUICK_ORDER_COUNT;
}

// The aimed-shot picker owns B (as cancel) while it is up; defined further down.
static bool gpui_aim_picker_active();

// Icon art is locked for the lifetime of one wheel opening and released on close -- see the
// shared gpui_icons_* helpers above, which this wheel, the skill wheel and the weapon wheel
// all share (only one of them can be on screen at a time).

// Text buffers for the wheels' hub detail lines. One per ring slot, static because RadialSegment
// only borrows the pointer and the wheel outlives this function.
static char gpui_detail_buf[RADIAL_WHEEL_MAX][64];

// "what does this drug do", built from the PROTO's own effect table rather than from the item's
// description text -- the description is flavour prose ("A chem that..."), which is the wrong thing
// to read while picking under fire. stat[i] == -1 marks an unused slot.
//
// Only the IMMEDIATE effects are listed. Fallout drugs also carry two delayed waves (duration1 /
// duration2 with their own amounts); spelling those out needs three times the room and the answer
// to "what happens if I take this NOW" is the first wave.
static void gpui_drug_detail(int pid, char* out, size_t size)
{
    out[0] = '\0';

    Proto* proto;
    if (proto_ptr(pid, &proto) != 0 || proto->item.type != ITEM_TYPE_DRUG) {
        return;
    }

    size_t len = 0;
    for (int i = 0; i < 3; i++) {
        int stat = proto->item.data.drug.stat[i];
        int amount = proto->item.data.drug.amount[i];
        if (stat < 0 || amount == 0) {
            continue;
        }

        const char* name = stat_name(stat);
        if (name == NULL) {
            continue;
        }

        int written = snprintf(out + len, size - len, "%s%+d %s", len > 0 ? "  " : "", amount, name);
        if (written < 0 || (size_t)written >= size - len) {
            break; // out of room -- keep what fits rather than truncating mid-word
        }
        len += written;
    }
}

// "damage, what it eats, and how much of that I am carrying". The ammo count is the whole point of
// the line: a gun's magazine is already on its badge, but whether there is a second magazine in
// your pack is what actually decides which weapon you pull.
static void gpui_weapon_detail(Object* weapon, char* out, size_t size)
{
    out[0] = '\0';
    if (weapon == NULL) {
        return;
    }

    int minDam = 0;
    int maxDam = 0;
    item_w_damage_min_max(weapon, &minDam, &maxDam);

    size_t len = (size_t)snprintf(out, size, "%d-%d dmg", minDam, maxDam);
    if (len >= size) {
        return;
    }

    int ammoPid = item_w_ammo_pid(weapon);
    if (ammoPid == -1) {
        return; // melee / unarmed -- no ammo line to add
    }

    // Loose stacks only, matching the reach the wheels use everywhere else. Sums the ROUNDS in
    // every box of that proto, not the number of boxes: "38" answers the question, "2 boxes" does
    // not without knowing how full they are.
    int rounds = 0;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item != NULL && item->pid == ammoPid) {
            int per = item_w_curr_ammo(item);
            rounds += (per > 0 ? per : 1) * inventory->items[i].quantity;
        }
    }

    snprintf(out + len, size - len, "  %s: %d", proto_name(ammoPid), rounds);
}

static void gpui_open_quick_wheel()
{
    // Distinct drug PIDs the dude is carrying loose, in priority-then-inventory order.
    //
    // PIDs, not Object*: unlike a weapon, a drug is consumed the moment you use it, so a pointer
    // would dangle. "Use the next one of these I'm carrying" is the right identity here and it
    // survives consuming, looting, trading and reloading a save.
    int pids[GPUI_QUICK_MAX];
    int counts[GPUI_QUICK_MAX];
    int count = 0;

    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item == NULL || item_get_type(item) != ITEM_TYPE_DRUG) {
            // Only ITEM_TYPE_DRUG: gpui_quick_use goes through item_d_take_drug, which is the drug
            // path only. (A first aid kit is MISC and belongs on the skill wheel.)
            continue;
        }

        int pid = item->pid;

        // Same proto in two stacks is ONE segment carrying the combined count.
        int at = -1;
        for (int j = 0; j < count; j++) {
            if (pids[j] == pid) {
                at = j;
                break;
            }
        }
        if (at != -1) {
            counts[at] += inventory->items[i].quantity;
            continue;
        }

        if (count >= GPUI_QUICK_MAX) {
            continue; // ring is full; the priority sort below already put the useful ones first
        }

        // Insertion sort by priority rank, so the curated healing-first order is preserved among
        // whatever subset you happen to be carrying.
        int rank = gpui_quick_rank(pid);
        int pos = count;
        while (pos > 0 && gpui_quick_rank(pids[pos - 1]) > rank) {
            pids[pos] = pids[pos - 1];
            counts[pos] = counts[pos - 1];
            pos--;
        }
        pids[pos] = pid;
        counts[pos] = inventory->items[i].quantity;
        count++;
    }

    if (count == 0) {
        // Carrying nothing usable -- refuse audibly rather than flashing an empty ring (the
        // empty-gun lesson: never fail silently). Same as the weapon wheel with no weapons.
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    gpui_icons_release();
    gpui_load_skill_art(); // only for the BIGNUM digit strip that draws the counts

    RadialSegment segs[GPUI_QUICK_MAX];
    memset(segs, 0, sizeof(segs));

    for (int i = 0; i < count; i++) {
        segs[i].id = pids[i];
        segs[i].label = proto_name(pids[i]);
        segs[i].value = counts[i];
        segs[i].enabled = true; // everything on the ring is something you have
        gpui_drug_detail(pids[i], gpui_detail_buf[i], sizeof(gpui_detail_buf[i]));
        segs[i].detail = gpui_detail_buf[i][0] != '\0' ? gpui_detail_buf[i] : NULL;
        // Fallout's own prose, straight from the proto message file -- no copy, the proto text
        // outlives the wheel.
        segs[i].description = proto_description(pids[i]);
        gpui_icon_from_pid(segs, i, pids[i]);
    }

    RadialWheelArt art;
    radial_wheel_open(segs, count, "QUICK USE",
        gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    if (!radial_wheel_is_open()) {
        gpui_icons_release();
    }
}

// Use one of `pid` on the dude. This is deliberately the same sequence combatai.cc:370 runs
// when an NPC self-medicates -- remove one, apply it, play the use animation, destroy it --
// so the player's quick-use and the AI's go through identical engine paths.
static void gpui_quick_use(int pid)
{
    Object* item = NULL;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        if (inventory->items[i].item != NULL && inventory->items[i].item->pid == pid) {
            item = inventory->items[i].item;
            break;
        }
    }

    if (item == NULL || item_get_type(item) != ITEM_TYPE_DRUG) {
        gsound_play_sfx_file("iisxxxx1"); // out of stock: refuse audibly, never silently
        return;
    }

    if (item_remove_mult(obj_dude, item, 1) != 0) {
        return;
    }

    if (item_d_take_drug(obj_dude, item) == -1) {
        // Refused (robotic body, dead) -- put it back rather than eating the item.
        item_add_force(obj_dude, item, 1);
        return;
    }

    // The "using something" animation. This DOES tie up obj_dude until it finishes, which is
    // intended. NOTE: combatai follows a failed register_end() with combat_turn_run(); that
    // pumps the game loop and must never be called from this frame tick, so it is omitted --
    // a refused registration just means no animation plays.
    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_animate(obj_dude, ANIM_MAGIC_HANDS_MIDDLE, 0);
    register_end();

    obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
    obj_destroy(item);

    intface_update_hit_points(true);

    // Vanilla charges 2 AP for taking a drug. Charge the same from the realtime pool when
    // it can be paid, but never REFUSE the use over it -- running dry should not be what
    // stops you healing.
    if (combat_rt_player_ap() >= 2) {
        combat_rt_spend_ap(2);
    }
}

// Drive from the per-frame safe tick (gamepad_ui_tick). Open once B has been held past the
// tap window, aim while held, execute + close on release.
static void gpui_quick_wheel(SDL_GameController* pad)
{
    static bool prevB = false;
    static unsigned int bDownTime = 0;
    static bool bHandled = false; // this hold already tried to open; don't retry every frame

    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool bEdge = bNow && !prevB;
    bool bReleased = !bNow && prevB;
    prevB = bNow;

    if (bEdge) {
        bDownTime = get_time();
        bHandled = false;
    }

    // Ignore the shared overlay while one of the other wheels owns it.
    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_QUICK) {
        return;
    }

    // Don't bring up (or keep up) the wheel over a modal menu / inventory / a disabled
    // interface bar -- and not while the aimed-shot picker is up, where B means cancel.
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()
        || gpui_aim_picker_active();

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (bReleased) {
            int pid = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (pid != -1) {
                gpui_quick_use(pid);
            }
        } else {
            // LEFT stick, unlike the other two wheels, which aim with the right. B is a FACE
            // button: the right thumb is already holding the wheel open and physically cannot
            // reach the right stick at the same time, so the aim moves to the other hand.
            // Walking is not a conflict -- rt_gamepad_input bails out entirely while any
            // radial wheel is open (the radial_wheel_is_open() guard in combat_rt.cc), so
            // the left stick is free while this is up.
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
            radial_wheel_update(x, y);
        }
        return;
    }

    if (bNow && !bHandled && !blocked && get_time() - bDownTime >= GPUI_QUICK_HOLD_MS) {
        bHandled = true;
        gpui_open_quick_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_QUICK;
            // Tell the tap handler this B press was a hold, so releasing it does not also
            // cycle the weapon's firing mode.
            gpui_quick_consumed_b = true;
        }
    }
}

// --- Y-hold weapon wheel (a radial_wheel instance) ---
//
// Y already means "weapon" (tap = swap hands), so a HOLD naturally means "which weapon" -- the
// same tap-vs-hold split B got for quick-use, and for the same reason: no pad button is free.
// LT+Y stays quick save; holding LT suppresses the wheel outright so the two gestures never race.
//
// ⚠️ Unlike the drug wheel this list is BUILT FROM THE INVENTORY, not fixed. That breaks the
// muscle-memory property the drug wheel was designed around -- but weapons ARE whatever you happen
// to be carrying, so there is no honest fixed slot to give them. Inventory order at least only
// shifts when you pick something up or drop it, not mid-fight.

// Eight segments on screen at once: seven weapons plus the bare-hands entry, which holds the LAST
// position on EVERY page so going unarmed is always the same flick and never needs paging.
//
// More weapons than that PAGE with RB (next) / LB (previous) rather than growing the ring. A ring of
// twenty is unaimable and destroys the one thing a radial menu is good for -- fixed positions you
// stop having to read. Eight stays learnable; the page number is the only thing you track.
#define GPUI_WEAPON_PER_PAGE 8
#define GPUI_WEAPON_GUNS_PER_PAGE (GPUI_WEAPON_PER_PAGE - 1)

// Hard ceiling on weapons the wheel will page through, i.e. 8 pages' worth. Anything past this is
// unreachable from the wheel and has to come out of the inventory screen.
#define GPUI_WEAPON_MAX 56

// Interface FRM for the "bare hands" segment: the action menu's Use/Get HAND (gmouse.cc's
// gmouse_3d_action_nums[]). FO1 has no dedicated "no weapon" art, and a hand is the honest picture
// of what you would be fighting with.
#define GPUI_UNARMED_ART_NUM 265

// Segment id of the bare-hands entry. Must not be -1, which radial_wheel_highlight_id() already
// uses for "nothing aimed at".
#define GPUI_WEAPON_UNARMED_ID (-2)

// The weapons the open wheel is showing. Object pointers, NOT pids: two rifles of the same proto
// are different guns with different loads, and the wheel must equip the exact one you aimed at.
// Only meaningful while the wheel is up -- and nothing can move items underneath it, since the
// wheel blocks while the inventory screen is open.
//
// A NULL slot is the deliberate "bare hands" entry -- see gpui_open_weapon_wheel.
static Object* gpui_weapon_slot[GPUI_WEAPON_MAX];
static int gpui_weapon_count = 0;

// Which page the wheel is showing, and how many there are. Kept across a single wheel OPENING (LB/RB
// step it and reopen); reset to 0 each time the wheel is freshly summoned, so a Y-hold always starts
// on the page your first seven weapons live on.
static int gpui_weapon_page = 0;
static int gpui_weapon_pages = 1;

// Collect every loose weapon, then show one page of it. Object pointers, NOT pids: two rifles of the
// same proto are different guns with different loads, and the wheel must equip the exact one aimed
// at. Nothing can move items underneath this, since the wheel blocks while the inventory is open.
static void gpui_open_weapon_wheel()
{
    gpui_icons_release();
    gpui_load_skill_art(); // only for the BIGNUM digit strip that draws the ammo counts

    gpui_weapon_count = 0;

    // Loose inventory only, same reach as the drug wheel. The weapons currently in your hands are
    // in here too (equipping does not remove them from the inventory, it only flags the hand), so
    // the wheel shows what you are holding as well as what you could hold.
    // ONE SEGMENT PER WEAPON TYPE.
    //
    // ⚠️ Showing every weapon OBJECT was inconsistent in a way that looked like a bug: Fallout
    // stacks identical items into a single inventory entry, so four identical 10mm Pistols are ONE
    // entry and appeared once, while two shotguns with different magazines are two entries and
    // appeared twice. Same wheel, same inventory, two different rules -- "ukazuješ jen jednu pistol
    // a mám 4 v inventory". Collapsing by proto makes it one rule.
    //
    // When a type has several distinct objects, keep the FULLEST one: the reason to distinguish
    // them at all was to grab the loaded gun rather than the empty one, and that survives here
    // without spending a segment on each.
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item == NULL || item_get_type(item) != ITEM_TYPE_WEAPON) {
            continue;
        }

        int at = -1;
        for (int j = 0; j < gpui_weapon_count; j++) {
            if (gpui_weapon_slot[j]->pid == item->pid) {
                at = j;
                break;
            }
        }

        if (at != -1) {
            if (item_w_curr_ammo(item) > item_w_curr_ammo(gpui_weapon_slot[at])) {
                gpui_weapon_slot[at] = item;
            }
            continue;
        }

        if (gpui_weapon_count < GPUI_WEAPON_MAX) {
            gpui_weapon_slot[gpui_weapon_count++] = item;
        }
    }

    if (gpui_weapon_count == 0) {
        // Nothing to choose from -- refuse audibly rather than flashing an empty ring (the empty-gun
        // lesson: never fail silently). Checked before the bare-hands entry is added: a ring whose
        // only option is "put away the nothing you are holding" is not a choice.
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    gpui_weapon_pages = (gpui_weapon_count + GPUI_WEAPON_GUNS_PER_PAGE - 1) / GPUI_WEAPON_GUNS_PER_PAGE;
    if (gpui_weapon_page < 0) {
        gpui_weapon_page = gpui_weapon_pages - 1; // wrap backwards off page 0
    } else if (gpui_weapon_page >= gpui_weapon_pages) {
        gpui_weapon_page = 0; // wrap forwards off the last page
    }

    int first = gpui_weapon_page * GPUI_WEAPON_GUNS_PER_PAGE;
    int shown = gpui_weapon_count - first;
    if (shown > GPUI_WEAPON_GUNS_PER_PAGE) {
        shown = GPUI_WEAPON_GUNS_PER_PAGE;
    }

    RadialSegment segs[GPUI_WEAPON_PER_PAGE];
    memset(segs, 0, sizeof(segs));

    int n = 0;
    for (int i = 0; i < shown; i++) {
        Object* item = gpui_weapon_slot[first + i];

        // id is the ABSOLUTE slot index, so gpui_weapon_equip needs no page arithmetic.
        segs[n].id = first + i;
        segs[n].enabled = true;

        segs[n].label = object_name(item);

        // Rounds in the magazine, for weapons that take ammo at all. A melee weapon has no
        // count worth showing, so it gets no badge instead of a meaningless 0.
        segs[n].value = item_w_max_ammo(item) > 0 ? item_w_curr_ammo(item) : -1;

        gpui_weapon_detail(item, gpui_detail_buf[n], sizeof(gpui_detail_buf[n]));
        segs[n].detail = gpui_detail_buf[n][0] != '\0' ? gpui_detail_buf[n] : NULL;
        segs[n].description = object_description(item);

        gpui_icon_from_pid(segs, n, item->pid);
        n++;
    }

    // Bare hands, LAST on EVERY page -- id -1 rather than a slot index, since it is not one of the
    // collected weapons. Going unarmed had no route on the pad at all (Y swaps hands; the wheel
    // could only ever put something INTO one), and it matters more than it sounds: unarmed/melee
    // skills, sneaking past people who react to a drawn gun, talking to someone without a rifle
    // pointed at them.
    // ⚠️ NOT -1: radial_wheel_highlight_id() already returns -1 for "nothing aimed at", so a
    // bare-hands id of -1 would be indistinguishable from an unaimed release.
    segs[n].id = GPUI_WEAPON_UNARMED_ID;
    segs[n].enabled = true;
    segs[n].label = skill_name(SKILL_UNARMED);
    segs[n].value = -1;
    gpui_icon_from_interface(segs, n, GPUI_UNARMED_ART_NUM);
    n++;

    // Title carries the page number only when there IS more than one page, so the common case
    // stays the plain word.
    char title[32];
    if (gpui_weapon_pages > 1) {
        snprintf(title, sizeof(title), "WEAPONS %d/%d", gpui_weapon_page + 1, gpui_weapon_pages);
    } else {
        snprintf(title, sizeof(title), "WEAPONS");
    }

    RadialWheelArt art;
    radial_wheel_open(segs, n, title,
        gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    if (!radial_wheel_is_open()) {
        gpui_icons_release();
    }
}

static void gpui_weapon_equip(int index)
{
    if (index != GPUI_WEAPON_UNARMED_ID && (index < 0 || index >= gpui_weapon_count)) {
        return;
    }

    Object* item = index == GPUI_WEAPON_UNARMED_ID ? NULL : gpui_weapon_slot[index];

    // Into the ACTIVE hand. inven_wield only plays the put-away/take-out animation when the target
    // hand is the one the interface bar is showing (its internal `hand == a3` test), and that
    // animation is the point -- the swap should read on screen, not appear in your fist.
    int hand = intface_is_item_right_hand();
    Object* current = hand ? inven_right_hand(obj_dude) : inven_left_hand(obj_dude);
    if (current == item) {
        // Covers both "already holding that gun" and "already empty-handed" (item == current ==
        // NULL), which is exactly the same nothing-to-do.
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    if (item == NULL) {
        // The bare-hands entry. inven_unwield plays the put-away animation itself when the hand
        // being emptied is the one the interface bar shows, so the swap reads on screen just like
        // a wield does.
        inven_unwield(obj_dude, hand);
        intface_update_items(true);
        return;
    }

    // ⚠️ Pre-check the art inven_wield is about to check itself. Vanilla's version returns -1 on a
    // missing take-out frame AFTER it has already called register_begin(), leaving the animation
    // registration open -- and a dangling begin wedges the next one. Refusing here means that path
    // is never entered. Formula copied from inventry.cc:3269 deliberately; keep them in step.
    int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF,
        item_w_anim_weap(item, HIT_MODE_RIGHT_WEAPON_PRIMARY), item_w_anim_code(item),
        obj_dude->rotation + 1);
    if (!art_exists(fid)) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    inven_wield(obj_dude, item, hand);
    intface_update_items(true);
}

// Drive from the per-frame safe tick (gamepad_ui_tick), exactly like the quick-use wheel.
static void gpui_weapon_wheel(SDL_GameController* pad)
{
    static bool prevY = false;
    static unsigned int yDownTime = 0;
    static bool yHandled = false; // this hold already tried to open; don't retry every frame

    static bool prevLB = false;
    static bool prevRB = false;

    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool yEdge = yNow && !prevY;
    bool yReleased = !yNow && prevY;
    prevY = yNow;

    // Page step for the open wheel, edge-triggered so one press is exactly one page.
    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool rbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    int pageStep = 0;
    if (rbNow && !prevRB) {
        pageStep = 1;
    } else if (lbNow && !prevLB) {
        pageStep = -1;
    }
    prevLB = lbNow;
    prevRB = rbNow;

    if (yEdge) {
        yDownTime = get_time();
        yHandled = false;
        gpui_weapon_page = 0; // a fresh summon always starts on the first page
    }

    // Ignore the shared overlay while one of the other wheels owns it.
    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_WEAPON) {
        return;
    }

    // Same blocks as the quick-use wheel, plus LT: LT+Y is quick save, so while LT is held Y is
    // not ours at all.
    bool ltNow = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GPUI_TRIGGER_THRESHOLD;
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()
        || gpui_aim_picker_active() || ltNow;

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (yReleased) {
            int index = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (index != -1) {
                gpui_weapon_equip(index);
            }
        } else if (pageStep != 0) {
            // RB = next page, LB = previous, wrapping at both ends (gpui_open_weapon_wheel clamps).
            // The shoulders are free while Y is held -- LT is suppressed entirely (it means quick
            // save with Y) and the aimed-shot picker cannot be up at the same time as this wheel.
            // Reopening rebuilds the ring for the new page; the icon slots are released and
            // re-locked by the open itself, so no leak.
            gpui_weapon_page += pageStep;
            radial_wheel_close();
            gpui_icons_release();
            gsound_play_sfx_file("ib1p1xx1");
            gpui_open_weapon_wheel();
            if (radial_wheel_is_open()) {
                gpui_wheel_owner = GPUI_WHEEL_WEAPON;
            } else {
                gpui_wheel_owner = GPUI_WHEEL_NONE;
            }
        } else {
            // LEFT stick -- Y is a FACE button, so the right thumb is holding the wheel open and
            // cannot work the right stick at the same time. Same call the quick-use wheel makes.
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
            radial_wheel_update(x, y);
        }
        return;
    }

    if (yNow && !yHandled && !blocked && get_time() - yDownTime >= GPUI_QUICK_HOLD_MS) {
        yHandled = true;
        gpui_open_weapon_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_WEAPON;
            // Tell the tap handler this Y press was a hold, so releasing it does not also
            // swap the active hand.
            gpui_weapon_consumed_y = true;
        }
    }
}

// --- X-hold ammo wheel ---
//
// X already means "reload"; a HOLD naturally means "reload with WHICH". Fallout 1 has no ammo-type
// switch of its own -- vanilla's only way to choose is to drag a specific box onto the weapon in the
// inventory, and `item_w_try_reload` (what a tap of X runs) just grabs the first compatible box it
// finds. So on a pad there was no way to answer "how do I pick AP over JHP?" at all.
//
// Same tap-vs-hold shape as B and Y, and aimed with the LEFT stick for the same reason: a face
// button ties up the right thumb, so the aim has to move to the other hand.

#define GPUI_AMMO_MAX 8

// Segment id of the UNLOAD entry. Not an index into gpui_ammo_slot, so it must be outside 0..count
// -- and NOT -1, which radial_wheel_highlight_id() already uses for "not pointing at anything".
// Same convention as the weapon wheel's "no weapon" slot.
#define GPUI_AMMO_UNLOAD_ID (-2)

// The ammo objects the open wheel is showing -- one per distinct proto, pointing at the first stack
// of that type. Object*, because item_w_reload needs the actual object.
static Object* gpui_ammo_slot[GPUI_AMMO_MAX];
static int gpui_ammo_count = 0;

// The weapon the wheel was opened for, so a hand swap mid-hold cannot reload the wrong gun.
static Object* gpui_ammo_weapon = NULL;

static void gpui_open_ammo_wheel()
{
    Object* weapon = NULL;
    if (intface_get_current_item(&weapon) != 0 || weapon == NULL
        || item_get_type(weapon) != ITEM_TYPE_WEAPON || item_w_max_ammo(weapon) <= 0) {
        // Bare hands, a melee weapon, or anything that does not take ammo.
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    gpui_ammo_count = 0;
    gpui_ammo_weapon = weapon;

    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length && gpui_ammo_count < GPUI_AMMO_MAX; i++) {
        Object* ammo = inventory->items[i].item;
        if (ammo == NULL || item_get_type(ammo) != ITEM_TYPE_AMMO) {
            continue;
        }
        // The engine's own compatibility test, so the wheel can never offer a box the reload
        // would then refuse.
        if (!item_w_can_reload(weapon, ammo)) {
            continue;
        }

        bool dup = false;
        for (int j = 0; j < gpui_ammo_count; j++) {
            if (gpui_ammo_slot[j]->pid == ammo->pid) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            gpui_ammo_slot[gpui_ammo_count++] = ammo;
        }
    }

    // UNLOAD is offered whenever the gun actually has rounds in it. Deliberately checked SEPARATELY
    // from the box list: dumping the magazine is exactly what you want when you are carrying no
    // compatible ammo at all (wrong caliber picked up, or you want the rounds back before dropping
    // the gun), so refusing the whole wheel on an empty box list would hide the one entry that
    // still applies. Vanilla can only do this by right-clicking the weapon in the inventory.
    bool canUnload = item_w_can_unload(weapon) != 0;

    if (gpui_ammo_count == 0 && !canUnload) {
        gsound_play_sfx_file("iisxxxx1"); // nothing this gun can take, and nothing in it either
        return;
    }

    gpui_icons_release();
    gpui_load_skill_art(); // BIGNUM digit strip for the box counts

    RadialSegment segs[GPUI_AMMO_MAX + 1];
    memset(segs, 0, sizeof(segs));

    for (int i = 0; i < gpui_ammo_count; i++) {
        Object* ammo = gpui_ammo_slot[i];
        segs[i].id = i;
        segs[i].label = object_name(ammo);
        segs[i].enabled = true;
        segs[i].value = item_w_curr_ammo(ammo); // rounds left in this box
        gpui_icon_from_pid(segs, i, ammo->pid);
    }

    int segCount = gpui_ammo_count;
    if (canUnload) {
        int slot = segCount++;
        segs[slot].id = GPUI_AMMO_UNLOAD_ID;
        segs[slot].label = "Unload";
        segs[slot].enabled = true;
        segs[slot].value = item_w_curr_ammo(weapon); // rounds currently IN the gun
        // Icon of the ammo that is loaded right now -- "take THIS back out" reads immediately,
        // where a generic symbol would need learning.
        gpui_icon_from_pid(segs, slot, item_w_ammo_pid(weapon));
    }

    RadialWheelArt art;
    radial_wheel_open(segs, segCount, "AMMO",
        gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    if (!radial_wheel_is_open()) {
        gpui_icons_release();
    }
}

// Dump the magazine back into the pack. Mirrors the inventory's own UNLOAD action (inventry.cc):
// item_w_unload hands back ONE box at a time and leaves any overflow in the weapon, so it has to be
// looped -- a weapon holding more rounds than one box's capacity would otherwise be half-emptied.
static void gpui_ammo_unload()
{
    // Re-check: the wheel was open for a while and a hand swap could have moved the gun.
    Object* weapon = NULL;
    if (intface_get_current_item(&weapon) != 0 || weapon != gpui_ammo_weapon) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    bool any = false;
    for (;;) {
        Object* ammo = item_w_unload(weapon);
        if (ammo == NULL) {
            break;
        }

        Rect rect;
        obj_disconnect(ammo, &rect);
        item_add_force(obj_dude, ammo, 1);
        any = true;
    }

    if (!any) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    // Same 2 AP a reload costs -- clearing the gun is the same handling, and making it free would
    // hand you a cost-free way to stall in a firefight.
    if (combat_rt_player_ap() >= 2) {
        combat_rt_spend_ap(2);
    }
    intface_update_items(true);
}

// Load the chosen box. Deliberately the same sequence item_w_try_reload runs (reload, destroy the
// box when it is spent) -- only the CHOICE of box differs, so partial boxes, capacity and the
// "already full" refusal all behave exactly as vanilla.
static void gpui_ammo_load(int index)
{
    if (index < 0 || index >= gpui_ammo_count || gpui_ammo_weapon == NULL) {
        return;
    }

    Object* ammo = gpui_ammo_slot[index];

    // Re-check: the wheel was open for a while and a hand swap could have moved the gun.
    Object* weapon = NULL;
    if (intface_get_current_item(&weapon) != 0 || weapon != gpui_ammo_weapon) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    // Swapping to a DIFFERENT ammo type means dumping what is loaded first; vanilla's reload
    // refuses a mismatched type outright rather than mixing. item_w_reload handles the same-type
    // top-up; for a different type the engine drops the old rounds itself.
    int rc = item_w_reload(weapon, ammo);
    if (rc == 0) {
        obj_destroy(ammo); // box fully consumed
    }

    if (rc == -1) {
        gsound_play_sfx_file("iisxxxx1"); // already full of this type
        return;
    }

    // Vanilla charges 2 AP for a reload; mirror the tap-X path.
    if (combat_rt_player_ap() >= 2) {
        combat_rt_spend_ap(2);
    }
    intface_update_items(true);
}

// Driven from the per-frame safe tick, exactly like the quick-use and weapon wheels.
static void gpui_ammo_wheel(SDL_GameController* pad)
{
    static bool prevX = false;
    static unsigned int xDownTime = 0;
    static bool xHandled = false;

    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
    bool xEdge = xNow && !prevX;
    bool xReleased = !xNow && prevX;
    prevX = xNow;

    if (xEdge) {
        xDownTime = get_time();
        xHandled = false;
    }

    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_AMMO) {
        return;
    }

    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()
        || gpui_aim_picker_active();

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (xReleased) {
            int index = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (index == GPUI_AMMO_UNLOAD_ID) {
                gpui_ammo_unload();
            } else if (index != -1) {
                gpui_ammo_load(index);
            }
        } else {
            // LEFT stick, exactly like the B and Y wheels. X is a FACE button: the right thumb is
            // holding the wheel open and cannot work the right stick at the same time. Which corner
            // of the diamond X sits in is irrelevant -- one thumb serves the whole diamond.
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
            radial_wheel_update(x, y);
        }
        return;
    }

    if (xNow && !xHandled && !blocked && get_time() - xDownTime >= GPUI_QUICK_HOLD_MS) {
        xHandled = true;
        gpui_open_ammo_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_AMMO;
            // Tell the tap handler this X press was a hold, so releasing it does not also reload.
            gpui_ammo_consumed_x = true;
        }
    }
}

// --- On-body aimed-shot picker (green body-part markers on the locked enemy) ---
//
// Holding LB brings up the aimed-shot picker. Instead of an abstract radial wheel, GREEN markers are
// drawn directly on the target's on-screen sprite, one per called-shot body part, the selected one
// WHITE. The RIGHT stick moves the selection over the body; releasing LB fires an aimed shot at it
// and remembers it, so a quick TAP of LB repeats that part instantly (the fast path). Part positions
// are APPROXIMATE fractions of the critter's bounding box -- the world sprite has no real per-part
// coordinates -- good enough to read as "aim here", rougher on non-humanoids.

// NO OVERLAY WINDOW. Three attempts at a see-through GNW window all rendered a black rectangle
// over the enemy: WINDOW_TRANSPARENT is honoured only while GNW double-buffers (gnw.cc:869/887),
// turning buffering on made GNW present the never-written all-zero `screen_buffer` instead, and
// seeding that buffer with a full refresh still left it black. So the marks are now painted
// DIRECTLY into the map window's own buffer (display_win) -- the same surface the world is drawn
// on, so there is nothing to make transparent: pixels we don't touch are simply the game. The
// previous frame's marks are wiped by asking the engine to repaint that patch of world
// (tile_refresh_rect), which is exactly how the engine erases its own transient drawing.
static int gpui_aim_last_part = HIT_LOCATION_HEAD; // part a quick tap / release fires at
static bool gpui_aim_body_on = false;              // on-body picker currently up

// Forward-declared up with the quick-use wheel, which must not steal B while this is up.
static bool gpui_aim_picker_active()
{
    return gpui_aim_body_on;
}

// Screen rect we last painted marks into, so it can be repainted clean.
// Colour of the aim reticle and its part label. GREEN, not white: white reads as chrome pasted
// over the scene -- it matches nothing else on screen and sat right under the red floating speech
// text. Green is the game's own selection colour (colorTable[992] is what the automap draws walls
// and labels in), and it is already the language this feature speaks: the aim highlight puts an
// OUTLINE_TYPE_FRIENDLY green outline on the target for exactly the same "this one is selected"
// reason. It also separates cleanly from the red hostile outline underneath it.
//
// NOTE the to-hit "88%" beside the target is NOT this -- that is vanilla's own crosshair readout
// (gmouse_3d_show_to_hit), white in the original game, and it is left alone deliberately.
#define GPUI_AIM_COLOR colorTable[992]

static Rect gpui_aimbody_dirty;
static bool gpui_aimbody_dirty_valid = false;

bool gamepad_ui_aim_body_active()
{
    return gpui_aim_body_on;
}

// Mark size per part, as a percentage of the common base radius.
//
// These were roughly halved after a play-test screenshot: at the previous sizes the arm, torso and
// groin marks all touched and read as one solid green mass covering the whole body, which defeats
// the point -- you couldn't see WHICH part was selected, only that the enemy had gone green. The
// marks now have to stay small enough to leave clear sprite between them; the selected one is
// separated by density (see the dither in gpui_aimbody_draw), not by being huge.
static int gpui_aimbody_radius_pct(int location)
{
    switch (location) {
    case HIT_LOCATION_EYES:      return 40;
    case HIT_LOCATION_HEAD:      return 60;
    case HIT_LOCATION_RIGHT_ARM: return 55;
    case HIT_LOCATION_LEFT_ARM:  return 55;
    case HIT_LOCATION_TORSO:     return 85;
    case HIT_LOCATION_GROIN:     return 50;
    case HIT_LOCATION_RIGHT_LEG: return 60;
    case HIT_LOCATION_LEFT_LEG:  return 60;
    default:                     return 100;
    }
}

// Approximate on-sprite position of a body part, as a fraction of the critter's bounding box. The
// world sprite carries no real per-part coordinates, so this is anatomy by eye and is rougher on
// non-humanoids. Arms sit further out to the sides and the torso a little higher than before, to
// stop the three of them piling into the same patch of chest.
static void gpui_aimbody_frac(int location, float* fx, float* fy)
{
    switch (location) {
    case HIT_LOCATION_EYES:      *fx = 0.50f; *fy = 0.09f; break;
    case HIT_LOCATION_HEAD:      *fx = 0.50f; *fy = 0.18f; break;
    case HIT_LOCATION_RIGHT_ARM: *fx = 0.22f; *fy = 0.40f; break;
    case HIT_LOCATION_LEFT_ARM:  *fx = 0.78f; *fy = 0.40f; break;
    case HIT_LOCATION_TORSO:     *fx = 0.50f; *fy = 0.40f; break;
    case HIT_LOCATION_GROIN:     *fx = 0.50f; *fy = 0.62f; break;
    case HIT_LOCATION_RIGHT_LEG: *fx = 0.40f; *fy = 0.86f; break;
    case HIT_LOCATION_LEFT_LEG:  *fx = 0.60f; *fy = 0.86f; break;
    default:                     *fx = 0.50f; *fy = 0.50f; break;
    }
}

// --- Silhouette-relative part placement. ---
//
// The fractions above used to be taken against the sprite's BOUNDING BOX, and that is why the
// marks never quite sat on the body ("the positions still don't line up on the characters"). An
// FRM frame is a padded rectangle: a standing humanoid leaves transparent margin above the head,
// below the feet and, depending on facing, a lot on one side. Measuring 0.18 of the frame height
// therefore lands somewhere above the head, and 0.22 of the frame width can miss the arm entirely
// -- and the error changes with every frame of every animation, which is exactly the "sometimes it
// fits, sometimes it doesn't" the player saw.
//
// So measure the actual OPAQUE pixels instead: the vertical fraction is taken against the real top
// and bottom of the body, and the horizontal fraction against the width of the body ON THAT ROW.
// Arms then land on the shoulders' real span rather than on frame padding, and it self-corrects
// for facing, pose, crouch and non-humanoid shapes.
#define GPUI_AIMBODY_MAX_H 256
#define GPUI_AIMBODY_MAX_RUNS 8 // opaque segments per row we bother recording

typedef struct GpuiSilhouette {
    int top;    // first row with any opaque pixel, -1 if the frame is empty
    int bottom; // last such row
    int rowMin[GPUI_AIMBODY_MAX_H];
    int rowMax[GPUI_AIMBODY_MAX_H]; // < rowMin marks an empty row
    // RUNS: the contiguous opaque segments of each row, not merely its outer edges. This is the
    // whole basis of real per-part regions -- rowMin/rowMax alone throws away everything BETWEEN
    // the edges, and it is precisely the gaps in between (between the two legs, between an arm and
    // the torso) that tell one body part from another. Rows with more runs than we store are
    // clamped; nothing downstream assumes the list is exhaustive.
    short runCount[GPUI_AIMBODY_MAX_H];
    short runLo[GPUI_AIMBODY_MAX_H][GPUI_AIMBODY_MAX_RUNS];
    short runHi[GPUI_AIMBODY_MAX_H][GPUI_AIMBODY_MAX_RUNS];
} GpuiSilhouette;

// Scan the frame once: per row, the leftmost/rightmost opaque pixel AND the individual opaque runs.
// Returns false if the frame is empty or taller than we're willing to scan (callers then skip).
static bool gpui_aimbody_scan(const unsigned char* fd, int fw, int fh, GpuiSilhouette* sil)
{
    if (fh > GPUI_AIMBODY_MAX_H) {
        return false;
    }

    sil->top = -1;
    sil->bottom = -1;

    for (int y = 0; y < fh; y++) {
        int lo = fw;
        int hi = -1;
        int runs = 0;
        int runStart = -1;
        const unsigned char* row = fd + (size_t)y * fw;

        for (int x = 0; x < fw; x++) {
            if (row[x] != 0) {
                if (x < lo) {
                    lo = x;
                }
                hi = x;
                if (runStart < 0) {
                    runStart = x;
                }
            } else if (runStart >= 0) {
                if (runs < GPUI_AIMBODY_MAX_RUNS) {
                    sil->runLo[y][runs] = (short)runStart;
                    sil->runHi[y][runs] = (short)(x - 1);
                    runs++;
                }
                runStart = -1;
            }
        }
        if (runStart >= 0 && runs < GPUI_AIMBODY_MAX_RUNS) {
            sil->runLo[y][runs] = (short)runStart;
            sil->runHi[y][runs] = (short)(fw - 1);
            runs++;
        }

        sil->runCount[y] = (short)runs;
        sil->rowMin[y] = lo;
        sil->rowMax[y] = hi;
        if (hi >= 0) {
            if (sil->top < 0) {
                sil->top = y;
            }
            sil->bottom = y;
        }
    }

    return sil->top >= 0;
}

// --- Body-part REGIONS, carved out of the silhouette. ---
//
// WHAT IS AND IS NOT POSSIBLE HERE, because it governs every heuristic below: an FRM frame is a
// flat indexed bitmap (index 0 = transparent) and there is NO limb data anywhere in the art or the
// engine -- HIT_LOCATION appears only in gameplay code, never in art.cc/proto. So a body part can
// only ever be inferred from the SHAPE of the opaque pixels. Two inferences are genuinely reliable
// and one is not:
//   HEAD  - reliable. Going down from the crown the head is a narrow column and the shoulders are
//           a sharp width jump; that jump is the neck line, per frame and per facing.
//   LEGS  - reliable WHILE THEY ARE APART. A row containing two runs is the gap between the legs,
//           so the left and right runs are the legs themselves. When the legs overlap (feet
//           together, mid-stride) there is no gap and the split has to be guessed at the midline.
//   ARMS  - NOT reliable, and no algorithm can make it so. An arm held clear of the body shows up
//           as its own run; an arm resting against the torso shares a solid block of pixels with
//           it and there is simply no boundary in the image to find. (Stage 2.)
// Non-humanoids (radscorpion, dog, brahmin) satisfy none of these assumptions, so every detector
// reports failure rather than a wrong answer, and the caller falls back to the plain ring.

typedef struct GpuiBodyRegions {
    bool headValid;
    int headTop;
    int headBottom; // last row of the head, i.e. the row above the shoulders
    bool legsValid;
    int legsTop; // first row at which the legs are separated
    int legSplit[GPUI_AIMBODY_MAX_H]; // x of the gap centre per row, from legsTop down
    // The TRUNK: everything between the neck and the crotch, which is then split vertically into
    // torso / groin and horizontally into torso / arms. Needs the head (the neck line is its top
    // edge), so trunkValid implies headValid.
    bool trunkValid;
    int trunkTop;
    int trunkBottom;
    int groinTop;  // first row counted as groin rather than torso
    int armBottom; // last row an arm reaches (hands hang to about hip height)
} GpuiBodyRegions;

// Find the neck: the row in the upper part of the body where the silhouette's width jumps most
// sharply. Sets headValid=false when nothing convincing is there (a shape with no distinct head).
static void gpui_find_head(const GpuiSilhouette* sil, GpuiBodyRegions* reg)
{
    reg->headValid = false;

    int bodyH = sil->bottom - sil->top + 1;
    if (bodyH < 8) {
        return; // too few pixels for the jump to mean anything
    }

    // Only look in the top 40%: below that a width jump is a belt, a coat, or the legs parting.
    int limit = sil->top + bodyH * 40 / 100;
    if (limit > sil->bottom) {
        limit = sil->bottom;
    }

    int bestRow = -1;
    float bestRatio = 1.0f;
    for (int y = sil->top + 2; y <= limit; y++) {
        if (sil->rowMax[y] < sil->rowMin[y] || sil->rowMax[y - 1] < sil->rowMin[y - 1]) {
            continue;
        }
        int wPrev = sil->rowMax[y - 1] - sil->rowMin[y - 1] + 1;
        int wHere = sil->rowMax[y] - sil->rowMin[y] + 1;
        if (wPrev <= 0) {
            continue;
        }
        float ratio = (float)wHere / (float)wPrev;
        if (ratio > bestRatio) {
            bestRatio = ratio;
            bestRow = y;
        }
    }

    // 1.4x is the shoulders. A head is roughly a third of the shoulder width, so the real jump is
    // far larger than this; the threshold only has to exclude ordinary taper.
    if (bestRow < 0 || bestRatio < 1.4f) {
        return;
    }

    reg->headTop = sil->top;
    reg->headBottom = bestRow - 1;
    reg->headValid = reg->headBottom >= reg->headTop;
}

// Walk up from the feet for as long as the row is split in two: that gap is between the legs.
static void gpui_find_legs(const GpuiSilhouette* sil, GpuiBodyRegions* reg)
{
    reg->legsValid = false;

    int bodyH = sil->bottom - sil->top + 1;
    if (bodyH < 8) {
        return;
    }

    // The split may not reach the very bottom (feet can touch), so allow a few solid rows at the
    // bottom before giving up on finding it at all.
    int firstSplit = -1;
    int floorLimit = sil->bottom - bodyH * 55 / 100; // legs never start above mid-body
    for (int y = sil->bottom; y >= sil->top && y >= floorLimit; y--) {
        if (sil->runCount[y] >= 2) {
            firstSplit = y;
            break;
        }
    }
    if (firstSplit < 0) {
        return;
    }

    // Extend upward while the row stays split; that top row is the crotch.
    int topRow = firstSplit;
    for (int y = firstSplit - 1; y >= sil->top && y >= floorLimit; y--) {
        if (sil->runCount[y] < 2) {
            break;
        }
        topRow = y;
    }

    // Record the gap centre per row. Below the split (feet merged) the last known centre carries
    // down, which is what keeps the two feet assigned to the leg they belong to.
    int lastCentre = -1;
    for (int y = topRow; y <= sil->bottom; y++) {
        if (sil->runCount[y] >= 2) {
            // Widest gap in the row: with a weapon or a tail there can be more than two runs.
            int bestGap = -1;
            int centre = -1;
            for (int i = 1; i < sil->runCount[y]; i++) {
                int gap = sil->runLo[y][i] - sil->runHi[y][i - 1];
                if (gap > bestGap) {
                    bestGap = gap;
                    centre = (sil->runHi[y][i - 1] + sil->runLo[y][i]) / 2;
                }
            }
            lastCentre = centre;
        }
        reg->legSplit[y] = lastCentre;
    }

    reg->legsTop = topRow;
    reg->legsValid = lastCentre >= 0;
}

// The trunk is what is left once the head and the legs are accounted for. Its top edge is the neck
// line (so a head is required); its bottom edge is the crotch when the legs were found, otherwise
// the bottom of the sprite.
static void gpui_find_trunk(const GpuiSilhouette* sil, GpuiBodyRegions* reg)
{
    reg->trunkValid = false;

    if (!reg->headValid) {
        return; // without a neck line there is nothing to measure the trunk from
    }

    reg->trunkTop = reg->headBottom + 1;
    reg->trunkBottom = reg->legsValid ? reg->legsTop - 1 : sil->bottom;

    int trunkH = reg->trunkBottom - reg->trunkTop + 1;
    if (trunkH < 4) {
        return; // too short to divide into torso / groin without the two colliding
    }

    // The groin is the bottom quarter of the trunk -- the band that sits directly above where the
    // legs part -- and the arms reach down to roughly hip height.
    reg->groinTop = reg->trunkBottom - trunkH * 25 / 100;
    if (reg->groinTop <= reg->trunkTop) {
        reg->groinTop = reg->trunkTop + 1;
    }
    reg->armBottom = reg->trunkTop + trunkH * 70 / 100;
    if (reg->armBottom > reg->trunkBottom) {
        reg->armBottom = reg->trunkBottom;
    }
    reg->trunkValid = true;
}

// Splits ONE trunk row into torso and (optional) arm zones. This is where the arm problem is
// actually confronted, and it has two genuinely different cases:
//
//   ARM CLEAR OF THE BODY -- the row has several runs. The WIDEST run is the torso (a torso is
//   always broader than an arm), so whatever lies to its left and right is arm, and a weapon held
//   out in that hand comes along with it, which is correct: it is in the hand.
//
//   ARM AGAINST THE BODY -- one solid run, and the boundary the eye "sees" is not in the image at
//   all; the pixels are one continuous block. No amount of scanning recovers it. So the row is
//   divided proportionally: the outer 30% each side is arm, the middle is torso. That is an
//   ESTIMATE and is the one part of this whole system that cannot be made exact. It still hugs the
//   silhouette vertically, so it tracks the pose; only the left/right cut is assumed.
//
// Returns false for a row too narrow to divide at all (the caller then treats it as pure torso).
static bool gpui_trunk_row_split(const GpuiSilhouette* sil, int y, int* torsoLo, int* torsoHi)
{
    if (sil->rowMax[y] < sil->rowMin[y]) {
        return false;
    }

    if (sil->runCount[y] >= 2) {
        int bestW = -1;
        for (int i = 0; i < sil->runCount[y]; i++) {
            int w = sil->runHi[y][i] - sil->runLo[y][i] + 1;
            if (w > bestW) {
                bestW = w;
                *torsoLo = sil->runLo[y][i];
                *torsoHi = sil->runHi[y][i];
            }
        }
        return true;
    }

    int lo = sil->rowMin[y];
    int hi = sil->rowMax[y];
    int w = hi - lo + 1;
    if (w < 6) {
        *torsoLo = lo;
        *torsoHi = hi;
        return true; // nothing to carve an arm out of
    }

    int armW = w * 30 / 100;
    if (armW < 2) {
        armW = 2;
    }
    *torsoLo = lo + armW;
    *torsoHi = hi - armW;
    return true;
}

// Are we looking at the critter's BACK? Decides whether its own left limb is drawn on our left or
// our right -- without this the left/right marks are simply mirrored for half of the six facings,
// which is a plain bug in the old fraction-based anchors.
//
// SE/SW face toward the camera (we see the front, so its left is on OUR right); NE/NW face away
// (we see the back, its left is on our left). E and W are the side views: the art is a three-
// quarter pose that reads as front-facing, so they are grouped with the front. That last part is
// the one judgement call here and it is a single line to flip if the screenshot says otherwise.
static bool gpui_viewing_back(Object* obj)
{
    return obj->rotation == ROTATION_NE || obj->rotation == ROTATION_NW;
}

// Maps a HIT_LOCATION_*_ARM/LEG onto a screen side. Returns true for "the critter's own left limb
// is on the LEFT of the screen".
static bool gpui_part_on_screen_left(Object* obj, int location)
{
    bool ownLeft = location == HIT_LOCATION_LEFT_ARM || location == HIT_LOCATION_LEFT_LEG;
    return gpui_viewing_back(obj) ? ownLeft : !ownLeft;
}

// --- Part mask + outline. ---

#define GPUI_AIMBODY_MAX_W 256

// One frame's worth of "this pixel belongs to the selected part". Static rather than stack: this
// runs from the per-frame aim path (and, indirectly, from the mouse hook) where 64K of stack is
// not worth spending.
static unsigned char gpui_aim_mask[GPUI_AIMBODY_MAX_H * GPUI_AIMBODY_MAX_W];

// Marks the opaque pixels belonging to `location`. False = this part cannot be resolved from this
// frame (no detected region, or a part stage 1 does not segment yet), and the caller falls back to
// the plain ring rather than drawing an outline around a guess.
static bool gpui_aimbody_build_mask(const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil, const GpuiBodyRegions* reg, Object* target, int location)
{
    if (fw > GPUI_AIMBODY_MAX_W || fh > GPUI_AIMBODY_MAX_H) {
        return false;
    }

    bool legs = location == HIT_LOCATION_LEFT_LEG || location == HIT_LOCATION_RIGHT_LEG;
    bool arms = location == HIT_LOCATION_LEFT_ARM || location == HIT_LOCATION_RIGHT_ARM;
    bool trunk = arms || location == HIT_LOCATION_TORSO || location == HIT_LOCATION_GROIN;
    if (location == HIT_LOCATION_HEAD) {
        if (!reg->headValid) {
            return false;
        }
    } else if (legs) {
        if (!reg->legsValid) {
            return false;
        }
    } else if (trunk) {
        if (!reg->trunkValid) {
            return false;
        }
    } else {
        return false; // eyes have nothing to outline -- the caller draws a dot
    }

    memset(gpui_aim_mask, 0, (size_t)fh * fw);

    if (trunk) {
        int y0 = location == HIT_LOCATION_GROIN ? reg->groinTop : reg->trunkTop;
        int y1 = location == HIT_LOCATION_GROIN ? reg->trunkBottom
            : (arms ? reg->armBottom : reg->groinTop - 1);

        bool wantLeft = arms && gpui_part_on_screen_left(target, location);
        bool anyPixel = false;

        for (int y = y0; y <= y1; y++) {
            int tLo;
            int tHi;
            if (y < 0 || y >= fh || !gpui_trunk_row_split(sil, y, &tLo, &tHi)) {
                continue;
            }

            // Torso and groin ARE the middle run; an arm is everything outside it on its side.
            int lo;
            int hi;
            if (arms) {
                lo = wantLeft ? sil->rowMin[y] : tHi + 1;
                hi = wantLeft ? tLo - 1 : sil->rowMax[y];
            } else {
                lo = tLo;
                hi = tHi;
            }
            if (lo > hi) {
                continue; // that arm is not visible on this row (turned away, or fully overlapped)
            }

            const unsigned char* row = fd + (size_t)y * fw;
            unsigned char* m = gpui_aim_mask + (size_t)y * fw;
            for (int x = lo; x <= hi; x++) {
                if (x >= 0 && x < fw && row[x] != 0) {
                    m[x] = 1;
                    anyPixel = true;
                }
            }
        }

        // An arm can end up with nothing at all (side-on pose, arm entirely behind the body).
        // Report failure so the caller rings it rather than drawing an empty outline.
        return anyPixel;
    }

    if (location == HIT_LOCATION_HEAD) {
        for (int y = reg->headTop; y <= reg->headBottom; y++) {
            const unsigned char* row = fd + (size_t)y * fw;
            unsigned char* m = gpui_aim_mask + (size_t)y * fw;
            for (int x = sil->rowMin[y]; x <= sil->rowMax[y]; x++) {
                if (row[x] != 0) {
                    m[x] = 1;
                }
            }
        }
        return true;
    }

    // A leg is everything opaque on its side of the gap, from the crotch down. Taking the side of
    // the GAP rather than of the body's midline is what keeps a leg swung out mid-stride whole.
    bool wantLeft = gpui_part_on_screen_left(target, location);
    for (int y = reg->legsTop; y <= sil->bottom; y++) {
        int split = reg->legSplit[y];
        if (split < 0 || sil->rowMax[y] < sil->rowMin[y]) {
            continue;
        }
        const unsigned char* row = fd + (size_t)y * fw;
        unsigned char* m = gpui_aim_mask + (size_t)y * fw;
        for (int x = sil->rowMin[y]; x <= sil->rowMax[y]; x++) {
            if (row[x] == 0) {
                continue;
            }
            if (wantLeft ? (x <= split) : (x > split)) {
                m[x] = 1;
            }
        }
    }
    return true;
}

// The mask's boundary: a masked pixel with at least one unmasked (or off-frame) 4-neighbour. That
// traces the limb's real silhouette edge, so the outline follows the pose instead of approximating
// it with a shape.
static bool gpui_aimbody_mask_edge(int x, int y, int fw, int fh)
{
    if (gpui_aim_mask[(size_t)y * fw + x] == 0) {
        return false;
    }
    if (x == 0 || y == 0 || x == fw - 1 || y == fh - 1) {
        return true;
    }
    return gpui_aim_mask[(size_t)y * fw + (x - 1)] == 0
        || gpui_aim_mask[(size_t)y * fw + (x + 1)] == 0
        || gpui_aim_mask[(size_t)(y - 1) * fw + x] == 0
        || gpui_aim_mask[(size_t)(y + 1) * fw + x] == 0;
}

// Resolve a body part to a pixel inside the silhouette.
static void gpui_aimbody_anchor(const GpuiSilhouette* sil, int location, int* cx, int* cy)
{
    float fx, fy;
    gpui_aimbody_frac(location, &fx, &fy);

    int bodyH = sil->bottom - sil->top + 1;
    int y = sil->top + (int)(fy * bodyH);
    if (y < sil->top) {
        y = sil->top;
    }
    if (y > sil->bottom) {
        y = sil->bottom;
    }

    // A row can be empty even between top and bottom (the gap between two legs, a weapon held out
    // clear of the body). Walk outward to the nearest row that has pixels so the mark still lands
    // on the critter rather than on thin air.
    int use = y;
    for (int d = 0; d <= bodyH; d++) {
        if (y - d >= sil->top && sil->rowMax[y - d] >= sil->rowMin[y - d]) {
            use = y - d;
            break;
        }
        if (y + d <= sil->bottom && sil->rowMax[y + d] >= sil->rowMin[y + d]) {
            use = y + d;
            break;
        }
    }

    int lo = sil->rowMin[use];
    int hi = sil->rowMax[use];
    *cy = use;
    *cx = lo + (int)(fx * (hi - lo));
}

// Anchors from the most recent gpui_aimbody_draw, in FRAME pixel coordinates, plus the silhouette
// box they were measured in. The right-stick picker selects against these so it is choosing from
// exactly the marks that are on screen -- with the anchors now silhouette-derived, a picker still
// working off bounding-box fractions would disagree with what the player can see. One frame stale
// (the picker runs just before the draw), which is invisible.
static int gpui_aimbody_anchor_x[8];
static int gpui_aimbody_anchor_y[8];
static Rect gpui_aimbody_box;
static bool gpui_aimbody_anchors_valid = false;

// The scan and the segmentation from that same draw. Published so the picker can classify the
// stick's aim point through the EXACT regions the player is looking at.
static GpuiSilhouette gpui_aim_sil;
static GpuiBodyRegions gpui_aim_reg;

// Erase whatever we painted last: ask the engine to redraw that patch of the world, which puts
// the untouched sprite and floor back. Cheap (one small rect) and it's the engine's own way of
// undoing transient drawing.
static void gpui_aimbody_hide()
{
    if (!gpui_aimbody_dirty_valid) {
        return;
    }
    Rect r = gpui_aimbody_dirty;
    gpui_aimbody_dirty_valid = false;
    tile_refresh_rect(&r, map_elevation);
}

// Paint the target's body-part marks straight onto the map window: the part whose HIT_LOCATION ==
// selLoc white, the rest green, and ONLY on the sprite's own opaque pixels so the marks sit on the
// body. No overlay window and therefore no transparency problem -- every pixel we don't write stays
// exactly the game. Called every frame while aiming; each call first repaints the previous frame's
// patch (gpui_aimbody_hide) so nothing smears.
static void gpui_aimbody_draw(Object* target, int selLoc)
{
    gpui_aimbody_hide();

    // Only true once this frame's scan has actually produced anchors, so the picker can never
    // select against a stale target's body.
    gpui_aimbody_anchors_valid = false;

    if (target == NULL || display_win == -1) {
        return;
    }

    Rect b;
    obj_bound(target, &b); // screen coordinates of the sprite's frame
    int bw = b.lrx - b.ulx + 1;
    int bh = b.lry - b.uly + 1;
    if (bw < 4 || bh < 4) {
        return;
    }

    Rect winRect;
    if (win_get_rect(display_win, &winRect) == -1) {
        return;
    }
    unsigned char* buf = win_get_buf(display_win);
    if (buf == NULL) {
        return;
    }
    int winW = winRect.lrx - winRect.ulx + 1;
    int winH = winRect.lry - winRect.uly + 1;

    CacheEntry* key;
    Art* art = art_ptr_lock(target->fid, &key);
    if (art == NULL) {
        return;
    }

    unsigned char* fd = art_frame_data(art, target->frame, target->rotation);
    int fw = art_frame_width(art, target->frame, target->rotation);
    int fh = art_frame_length(art, target->frame, target->rotation);
    // File-scope, not stack: the run table makes this ~11K (not worth spending on a path reached
    // from the mouse hook), and the picker below needs the very same scan and regions the outlines
    // were drawn from -- that shared state is what lets the stick select by REGION instead of by
    // nearest anchor point.
    GpuiSilhouette& sil = gpui_aim_sil;
    GpuiBodyRegions& reg = gpui_aim_reg;
    if (fd != NULL && fw > 0 && fh > 0 && gpui_aimbody_scan(fd, fw, fh, &sil)) {
        // Base mark radius, scaled off the sprite so it works for a rat and a super mutant alike.
        // SIZED FOR ONE MARK. The old 8% (clamped 3..8) was set when all EIGHT parts were painted
        // and the only thing that mattered was that they not merge into a single blob -- at that
        // size the head ring came out radius 2 (floored to 3) with a 1px hole, i.e. a speck sitting
        // INSIDE the head rather than a reticle around it, which is what the player kept reading as
        // "out of place". Only the selected part is drawn now, so nothing can merge with anything
        // and the mark can finally be the size it needs to be to read as a ring.
        //
        // Measured against the BODY's real height, not the padded frame, for the same reason the
        // anchors are -- otherwise a frame with a lot of margin gets a mark sized for a bigger
        // critter. At a ~50px humanoid: base 9, so the head ring is r=6 and the torso r=10.
        int bodyH = sil.bottom - sil.top + 1;
        int base = bodyH * 18 / 100;
        if (base < 4) base = 4;
        if (base > 18) base = 18;

        // Resolve every part once and publish it for the picker (and so the two draw passes below
        // share the work). Also record the silhouette's box, which is the space the picker maps the
        // right stick into.
        int boxLo = fw;
        int boxHi = -1;
        for (int y = sil.top; y <= sil.bottom; y++) {
            if (sil.rowMax[y] < sil.rowMin[y]) {
                continue;
            }
            if (sil.rowMin[y] < boxLo) {
                boxLo = sil.rowMin[y];
            }
            if (sil.rowMax[y] > boxHi) {
                boxHi = sil.rowMax[y];
            }
        }
        gpui_aimbody_box.ulx = boxLo;
        gpui_aimbody_box.lrx = boxHi;
        gpui_aimbody_box.uly = sil.top;
        gpui_aimbody_box.lry = sil.bottom;
        for (int i = 0; i < 8; i++) {
            gpui_aimbody_anchor(&sil, gpui_aim_order[i].location,
                &gpui_aimbody_anchor_x[i], &gpui_aimbody_anchor_y[i]);
        }
        // SEGMENT THE BODY. Head and legs are read out of the silhouette's real shape (see the
        // long note at gpui_find_head); everything else still falls back to the fraction anchors
        // above. Detection failing is a normal outcome -- a radscorpion has no neck line and no leg
        // gap -- and it simply means this frame is drawn the old way.
        gpui_find_head(&sil, &reg);
        gpui_find_legs(&sil, &reg);
        gpui_find_trunk(&sil, &reg); // needs both of the above

        // Re-anchor the parts we DID detect, so the right stick picks against the same geometry the
        // player can see. Without this the outline and the hit box drift apart.
        if (reg.headValid) {
            int hMid = (reg.headTop + reg.headBottom) / 2;
            int hy = sil.rowMax[hMid] >= sil.rowMin[hMid] ? hMid : reg.headTop;
            for (int i = 0; i < 8; i++) {
                int loc = gpui_aim_order[i].location;
                if (loc != HIT_LOCATION_HEAD && loc != HIT_LOCATION_EYES) {
                    continue;
                }
                // Eyes sit in the upper part of the detected head, the head at its centre.
                int y = loc == HIT_LOCATION_EYES
                    ? reg.headTop + (reg.headBottom - reg.headTop) * 40 / 100
                    : hy;
                if (sil.rowMax[y] < sil.rowMin[y]) {
                    y = hy;
                }
                gpui_aimbody_anchor_y[i] = y;
                gpui_aimbody_anchor_x[i] = (sil.rowMin[y] + sil.rowMax[y]) / 2;
            }
        }
        if (reg.legsValid) {
            int ly = (reg.legsTop + sil.bottom) / 2;
            if (reg.legSplit[ly] < 0) {
                ly = reg.legsTop;
            }
            for (int i = 0; i < 8; i++) {
                int loc = gpui_aim_order[i].location;
                if (loc != HIT_LOCATION_LEFT_LEG && loc != HIT_LOCATION_RIGHT_LEG) {
                    continue;
                }
                int split = reg.legSplit[ly];
                if (split < 0) {
                    continue;
                }
                gpui_aimbody_anchor_y[i] = ly;
                gpui_aimbody_anchor_x[i] = gpui_part_on_screen_left(target, loc)
                    ? (sil.rowMin[ly] + split) / 2
                    : (split + sil.rowMax[ly]) / 2;
            }
        }

        if (reg.trunkValid) {
            int torsoY = (reg.trunkTop + reg.groinTop) / 2;
            int groinY = (reg.groinTop + reg.trunkBottom) / 2;
            int armY = (reg.trunkTop + reg.armBottom) / 2;
            for (int i = 0; i < 8; i++) {
                int loc = gpui_aim_order[i].location;
                int y;
                if (loc == HIT_LOCATION_TORSO) {
                    y = torsoY;
                } else if (loc == HIT_LOCATION_GROIN) {
                    y = groinY;
                } else if (loc == HIT_LOCATION_LEFT_ARM || loc == HIT_LOCATION_RIGHT_ARM) {
                    y = armY;
                } else {
                    continue;
                }

                int tLo;
                int tHi;
                if (y < 0 || y >= fh || !gpui_trunk_row_split(&sil, y, &tLo, &tHi)) {
                    continue;
                }

                gpui_aimbody_anchor_y[i] = y;
                if (loc == HIT_LOCATION_LEFT_ARM || loc == HIT_LOCATION_RIGHT_ARM) {
                    // Middle of that arm's own zone, so the two arms never resolve to the same
                    // point and the stick can actually tell them apart.
                    gpui_aimbody_anchor_x[i] = gpui_part_on_screen_left(target, loc)
                        ? (sil.rowMin[y] + tLo) / 2
                        : (tHi + sil.rowMax[y]) / 2;
                } else {
                    gpui_aimbody_anchor_x[i] = (tLo + tHi) / 2;
                }
            }
        }

        gpui_aimbody_anchors_valid = boxHi >= boxLo;

        // ONLY THE SELECTED PART IS MARKED.
        //
        // Painting all eight was tried twice (big blobs, then small dithered ones) and the player's
        // verdict both times was that you cannot tell what you are aiming at -- which is right:
        // eight marks on a ~30px humanoid cannot be told apart at any size, and nothing on screen
        // ever NAMED the part. So the other seven are simply not drawn; the stick moves this one
        // marker around the body and the label below says what it is.
        int selIndex = -1;
        for (int i = 0; i < 8; i++) {
            if (gpui_aim_order[i].location == selLoc) {
                selIndex = i;
                break;
            }
        }

        // PREFERRED: trace the part's own outline. Falls through to the ring when this frame's
        // shape did not yield the part (see gpui_aimbody_build_mask).
        bool outlined = false;
        if (selIndex >= 0 && gpui_aimbody_build_mask(fd, fw, fh, &sil, &reg, target, selLoc)) {
            for (int py = sil.top; py <= sil.bottom; py++) {
                int wy = b.uly + py - winRect.uly;
                if (wy < 0 || wy >= winH) {
                    continue;
                }
                for (int px = 0; px < fw; px++) {
                    if (!gpui_aimbody_mask_edge(px, py, fw, fh)) {
                        continue;
                    }
                    int wx = b.ulx + px - winRect.ulx;
                    if (wx < 0 || wx >= winW) {
                        continue;
                    }
                    buf[wy * winW + wx] = GPUI_AIM_COLOR;
                }
            }
            outlined = true;
        }

        // EYES get a dot, not an outline -- there is nothing in the art to outline, and a dot on
        // the detected head's upper half is both honest about that and easy to read.
        if (!outlined && selIndex >= 0 && selLoc == HIT_LOCATION_EYES && reg.headValid) {
            int cx = gpui_aimbody_anchor_x[selIndex];
            int cy = gpui_aimbody_anchor_y[selIndex];
            int headH = reg.headBottom - reg.headTop + 1;
            int rDot = headH >= 10 ? 2 : 1;
            for (int dy = -rDot; dy <= rDot; dy++) {
                int wy = b.uly + cy + dy - winRect.uly;
                if (cy + dy < 0 || cy + dy >= fh || wy < 0 || wy >= winH) {
                    continue;
                }
                for (int dx = -rDot; dx <= rDot; dx++) {
                    int wx = b.ulx + cx + dx - winRect.ulx;
                    if (cx + dx < 0 || cx + dx >= fw || wx < 0 || wx >= winW) {
                        continue;
                    }
                    buf[wy * winW + wx] = GPUI_AIM_COLOR;
                }
            }
            outlined = true;
        }

        if (!outlined && selIndex >= 0) {
            int cx = gpui_aimbody_anchor_x[selIndex];
            int cy = gpui_aimbody_anchor_y[selIndex];
            int r = base * gpui_aimbody_radius_pct(selLoc) / 100;
            if (r < 3) r = 3;

            // A RING, not a filled patch: the outline reads as a reticle and leaves the part itself
            // visible inside it, so you can see what you have picked rather than covering it up.
            int rInner = r - 2;
            if (rInner < 1) rInner = 1;
            int rSq = r * r;
            int rInnerSq = rInner * rInner;

            for (int dy = -r; dy <= r; dy++) {
                int py = cy + dy;
                if (py < 0 || py >= fh) {
                    continue;
                }
                // frame pixel -> screen -> map-window buffer
                int wy = b.uly + py - winRect.uly;
                if (wy < 0 || wy >= winH) {
                    continue;
                }
                for (int dx = -r; dx <= r; dx++) {
                    int d2 = dx * dx + dy * dy;
                    if (d2 > rSq || d2 < rInnerSq) {
                        continue; // ring band only
                    }
                    int px = cx + dx;
                    if (px < 0 || px >= fw) {
                        continue;
                    }
                    int wx = b.ulx + px - winRect.ulx;
                    if (wx < 0 || wx >= winW) {
                        continue;
                    }
                    // Drawn over the WORLD, not just the sprite's opaque pixels: a ring that
                    // vanished wherever it left the silhouette (an arm held away from the body,
                    // the gap between the legs) broke up into disconnected specks and stopped
                    // reading as a circle at all.
                    buf[wy * winW + wx] = GPUI_AIM_COLOR;
                }
            }
        }
    }
    art_ptr_unlock(key);

    // NAME THE PART. Without this the marker still only says "somewhere around here" -- the eyes
    // and the head, or an arm and the torso, sit a few pixels apart on a 30px sprite and no marker
    // can distinguish them on its own. The string comes from the game's own message file via
    // combat_get_loc_name, so it is the same wording (and language) the vanilla called-shot window
    // uses, and it varies correctly per critter type.
    // The CHANCE goes on the same line. The crosshair's own "95%" is suppressed while the picker
    // is up (it is a red reticle sitting on the body), and it was misleading anyway: it comes from
    // combat_to_hit(), the UNAIMED chance, while the player is looking at a called shot that
    // carries a per-location penalty. combat_rt_aim_chance() answers for the part actually selected.
    Rect painted = b;
    char label[80];
    const char* partName = combat_get_loc_name(target, selLoc);
    if (partName != NULL && partName[0] != '\0') {
        snprintf(label, sizeof(label), "%s  %d%%", partName, combat_rt_aim_chance(selLoc));
        partName = label;
    }
    if (partName != NULL && partName[0] != '\0') {
        int oldFont = text_curr();
        text_font(101); // the small interface font, as used by the to-hit readout
        int tw = text_width(partName);
        int th = text_height();

        // Centred just above the sprite; flipped below it when there is no room at the top.
        int tx = b.ulx + ((b.lrx - b.ulx + 1) - tw) / 2;
        int ty = b.uly - th - 2;
        if (ty < winRect.uly) {
            ty = b.lry + 2;
        }

        int lx = tx - winRect.ulx;
        int ly = ty - winRect.uly;
        if (lx < 0) lx = 0;
        if (lx + tw > winW) lx = winW - tw;
        if (lx >= 0 && ly >= 0 && ly + th <= winH && tw > 0 && tw <= winW) {
            text_to_buf(buf + (size_t)ly * winW + lx, partName, tw, winW, GPUI_AIM_COLOR);

            // Grow the repaint rect to cover the text too, or it smears as the target moves.
            int sx0 = lx + winRect.ulx;
            int sy0 = ly + winRect.uly;
            if (sx0 < painted.ulx) painted.ulx = sx0;
            if (sy0 < painted.uly) painted.uly = sy0;
            if (sx0 + tw - 1 > painted.lrx) painted.lrx = sx0 + tw - 1;
            if (sy0 + th - 1 > painted.lry) painted.lry = sy0 + th - 1;
        }
        text_font(oldFont);
    }

    // Push just the painted patch, in window-relative coordinates (win_draw_rect offsets by the
    // window's origin itself), and remember it in SCREEN coordinates for the next erase.
    Rect local;
    local.ulx = painted.ulx - winRect.ulx;
    local.uly = painted.uly - winRect.uly;
    local.lrx = painted.lrx - winRect.ulx;
    local.lry = painted.lry - winRect.uly;
    if (local.ulx < 0) local.ulx = 0;
    if (local.uly < 0) local.uly = 0;
    if (local.lrx > winW - 1) local.lrx = winW - 1;
    if (local.lry > winH - 1) local.lry = winH - 1;
    if (local.ulx > local.lrx || local.uly > local.lry) {
        return;
    }
    win_draw_rect(display_win, &local);

    gpui_aimbody_dirty = painted;
    gpui_aimbody_dirty_valid = true;
}

// Right-stick part selection: map the stick to a reticle over the body and pick the nearest part.
// A small deflection (deadzone) keeps the current selection.
//
// Works in the SAME frame-pixel space as the drawn marks (gpui_aimbody_anchor_*), so what the stick
// snaps to is what the player sees lit up. It used to compare bounding-box fractions against each
// other, which was self-consistent but no longer matches the silhouette-derived marks: aiming at a
// visibly outlying arm could select the torso.
// Which HIT_LOCATION is a limb on the given side of the SCREEN? The inverse of
// gpui_part_on_screen_left, for going from a picked pixel back to a body part.
static int gpui_limb_for_screen_side(Object* obj, bool screenLeft, bool arm)
{
    bool ownLeft = gpui_viewing_back(obj) ? screenLeft : !screenLeft;
    if (arm) {
        return ownLeft ? HIT_LOCATION_LEFT_ARM : HIT_LOCATION_RIGHT_ARM;
    }
    return ownLeft ? HIT_LOCATION_LEFT_LEG : HIT_LOCATION_RIGHT_LEG;
}

// Classify one frame pixel into a body part using the SAME regions the outlines are drawn from.
// -1 when the pixel falls outside every detected region (the caller then falls back).
static int gpui_aimbody_classify(Object* target, int px, int py)
{
    const GpuiSilhouette* sil = &gpui_aim_sil;
    const GpuiBodyRegions* reg = &gpui_aim_reg;

    if (reg->headValid && py <= reg->headBottom) {
        int headH = reg->headBottom - reg->headTop + 1;
        return py <= reg->headTop + headH / 2 ? HIT_LOCATION_EYES : HIT_LOCATION_HEAD;
    }

    if (reg->trunkValid && py <= reg->trunkBottom) {
        if (py >= reg->groinTop) {
            return HIT_LOCATION_GROIN;
        }
        int tLo;
        int tHi;
        if (gpui_trunk_row_split(sil, py, &tLo, &tHi) && py <= reg->armBottom) {
            if (px < tLo) {
                return gpui_limb_for_screen_side(target, true, true);
            }
            if (px > tHi) {
                return gpui_limb_for_screen_side(target, false, true);
            }
        }
        return HIT_LOCATION_TORSO;
    }

    if (reg->legsValid && py >= reg->legsTop && py <= sil->bottom) {
        int split = reg->legSplit[py];
        if (split >= 0) {
            return gpui_limb_for_screen_side(target, px <= split, false);
        }
    }

    return -1;
}

// Right stick -> body part.
//
// WHY THIS IS NOT "NEAREST ANCHOR" ANY MORE. It used to map the stick to a point and take the
// closest of the eight anchors, and that made the two CENTRAL parts nearly unreachable, which is
// exactly what the player hit ("it's hard to get onto chest and groin"). Two compounding reasons:
// the dead zone was 0.35, and the aim point was then pushed out by a further 1.2x -- so the moment
// the stick registered at all, the aim point was already 40%+ of the way to the edge and the torso,
// whose anchor sits at the dead centre, could never win. The groin, one step below centre, was
// squeezed between the torso and the legs in the same way.
//
// Now the stick picks a PIXEL on the body and that pixel is classified by the same segmentation
// that drew the outline. Every part is therefore as easy to hit as it looks: the torso is a big
// area in the middle, so pointing anywhere near the middle selects it. The dead zone drops to a
// plain noise floor, and the overshoot is nearly gone (1.05, just enough to reach the feet and the
// crown). Nearest-anchor survives only as the fallback for a critter whose body could not be
// segmented -- and even then in NORMALISED space, so a tall narrow sprite no longer makes the
// vertical parts disproportionately hard to reach.
static int gpui_aimbody_pick(Object* target, float sx, float sy, int curLoc)
{
    if (sx * sx + sy * sy < 0.18f * 0.18f) {
        return curLoc; // stick at rest: hold the selection
    }
    if (target == NULL || !gpui_aimbody_anchors_valid) {
        return curLoc;
    }

    float halfW = (gpui_aimbody_box.lrx - gpui_aimbody_box.ulx) * 0.5f;
    float halfH = (gpui_aimbody_box.lry - gpui_aimbody_box.uly) * 0.5f;
    float cx = (gpui_aimbody_box.ulx + gpui_aimbody_box.lrx) * 0.5f;
    float cy = (gpui_aimbody_box.uly + gpui_aimbody_box.lry) * 0.5f;

    float aimX = cx + sx * halfW * 1.05f;
    float aimY = cy + sy * halfH * 1.05f;

    // Clamp into the body box so full deflection lands ON the extreme part rather than past it.
    if (aimX < gpui_aimbody_box.ulx) aimX = (float)gpui_aimbody_box.ulx;
    if (aimX > gpui_aimbody_box.lrx) aimX = (float)gpui_aimbody_box.lrx;
    if (aimY < gpui_aimbody_box.uly) aimY = (float)gpui_aimbody_box.uly;
    if (aimY > gpui_aimbody_box.lry) aimY = (float)gpui_aimbody_box.lry;

    int hit = gpui_aimbody_classify(target, (int)(aimX + 0.5f), (int)(aimY + 0.5f));
    if (hit >= 0) {
        return hit;
    }

    // Fallback: nearest anchor, measured in box-normalised units so the box's aspect ratio does
    // not weight one axis against the other.
    if (halfW < 1.0f) halfW = 1.0f;
    if (halfH < 1.0f) halfH = 1.0f;

    int best = curLoc;
    float bestD = -1.0f;
    for (int i = 0; i < 8; i++) {
        float dx = (aimX - gpui_aimbody_anchor_x[i]) / halfW;
        float dy = (aimY - gpui_aimbody_anchor_y[i]) / halfH;
        float d = dx * dx + dy * dy;
        if (bestD < 0.0f || d < bestD) {
            bestD = d;
            best = gpui_aim_order[i].location;
        }
    }
    return best;
}

// Tears the picker down and puts the iso crosshair back. Every exit from the picker goes through
// here so the suppressed cursor can never be left hidden.
static void gpui_aim_picker_close()
{
    if (!gpui_aim_body_on) {
        return;
    }
    gpui_aimbody_hide();
    gpui_aim_body_on = false;
    gmouse_set_cursor_suppressed(false);
}

// LB tap/hold aimed shot. TAP (< GPUI_AIM_TAP_MS) fires instantly at the last-used part; HOLD brings
// up the on-body green markers (right stick selects), release fires the selected part -- or B
// cancels, which is the only way out that does NOT spend the shot.
static void gpui_aim_wheel(SDL_GameController* pad)
{
    static bool prevLb = false;
    static bool prevB = false;
    static unsigned int lbDownTime = 0;
    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool lbEdge = lbNow && !prevLb;
    bool lbReleased = !lbNow && prevLb;
    prevLb = lbNow;

    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool bPressed = bNow && !prevB;
    prevB = bNow;

    // Don't run the aim picker while the skill wheel (L3) owns the shared radial overlay.
    if (radial_wheel_is_open()) {
        gpui_aim_picker_close();
        return;
    }

    // Aimed shot possible only in combat, with an aimable weapon and a target, and no modal /
    // inventory / disabled bar over the screen.
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled() || !combat_rt_can_aim();

    // Picker already up (LB held past the tap threshold): move the selection with the right stick and
    // redraw the markers; on release fire the selected part. Blocked / target gone -> tear down.
    if (gpui_aim_body_on) {
        Object* target = combat_rt_aim_target();
        if (blocked || target == NULL) {
            gpui_aim_picker_close();
        } else if (bPressed) {
            // CANCEL. Before this the picker had no exit but the shot: releasing LB always fired,
            // so bringing the markers up to LOOK at a target -- or changing your mind about the
            // part -- cost you an aimed attack. B is the cancel everywhere else in this UI.
            // gpui_aim_swallow_b keeps the same press from also cycling the weapon's firing mode
            // in gpui_button_shortcuts, and gpui_aim_canceled eats the LB release still to come.
            gpui_aim_picker_close();
            gpui_aim_canceled = true;
            gpui_aim_swallow_b = true;
        } else if (lbReleased) {
            gpui_aim_picker_close();
            combat_rt_aimed_attack(gpui_aim_last_part);
        } else {
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / GPUI_AXIS_MAX;
            gpui_aim_last_part = gpui_aimbody_pick(target, x, y, gpui_aim_last_part);
            gpui_aimbody_draw(target, gpui_aim_last_part);
        }
        return;
    }

    // Picker not up yet -- TAP vs HOLD on LB:
    //  - TAP (press+release under GPUI_AIM_TAP_MS): fire immediately at the last-used part.
    //  - HOLD (past the threshold): bring up the on-body markers to pick a different part.
    if (lbEdge) {
        lbDownTime = get_time();
        gpui_aim_canceled = false; // a fresh press starts a fresh aim
    }
    if (lbNow && !blocked && !gpui_aim_canceled && get_time() - lbDownTime >= GPUI_AIM_TAP_MS) {
        gpui_aim_body_on = true;
        // The crosshair art is a big red reticle painted over the middle of the body -- exactly
        // where the outline and label we are about to draw need to be readable.
        gmouse_set_cursor_suppressed(true);
        gpui_aimbody_draw(combat_rt_aim_target(), gpui_aim_last_part);
    } else if (lbReleased && !blocked) {
        // A cancelled aim must not fire on the release that follows it.
        if (!gpui_aim_canceled) {
            combat_rt_aimed_attack(gpui_aim_last_part);
        }
        gpui_aim_canceled = false;
    }
}

// mouse_info() hook: contributes right-stick cursor movement, the RT-as-left-
// button state, and (when a menu is open) left-stick item snapping. Runs inside
// the engine input pipeline every frame, so movement and clicks pass through the
// same mouse_simulate_input edge detection and win_check_all_buttons cycle as
// the real mouse.
static void gpui_mouse_hook_impl(int* dx, int* dy, int* buttons)
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return;
    }

    SDL_GameControllerUpdate();

    // While a lock-on is held during gameplay, the right stick belongs to the
    // lock (cycle targets) and the cursor is glued to the target by combat_rt, so
    // don't also drive a free cursor delta here -- that would fight the glue. In
    // menus/inventory the lock may persist but combat_rt isn't gluing, so the
    // cursor stays free there.
    bool lockedGameplay = combat_rt_has_lock()
        && !gamepad_ui_menu_active()
        && !inven_any_open();

    // Right stick -> cursor delta. Magnitude past the deadzone is rescaled to
    // 0..1 and squared so small pushes give fine control and full deflection is
    // fast. NOT while a screen drives its own nav (gpui_self_nav: main menu, options,
    // load/save, dialogue): there the stick must not creep the mouse cursor onto buttons --
    // that showed the cursor and fired the button hover SOUND, which the title menu misreads
    // as its move blip ("I hear a blip, cursor shows, but the highlight doesn't move").
    float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / GPUI_AXIS_MAX;
    float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / GPUI_AXIS_MAX;
    float mag = sqrtf(x * x + y * y);

    // RIGHT STICK PICKS THE INTERACTABLE, mirroring what it already does for enemies while locked:
    // point at the thing you mean. Without it the prompt only ever offered the nearest-and-highest-
    // priority object, so a shelf beside a door, or one of three corpses, was simply unreachable.
    //
    // ⚠️ It only takes the stick when there are at least TWO candidates, and only while the flick is
    // armed. The prompt has a target almost everywhere in the world, so claiming the stick whenever
    // one exists would cost the player cursor control across most of the game for a feature that
    // does nothing with a single candidate.
    //
    // Edge-triggered with a re-centre arm (same shape as rt_directional_lock): one push steps
    // exactly one object and a held stick does not spin through the room.
    bool ipPickTook = false;
    if (!lockedGameplay && !radial_wheel_is_open() && !gpui_aim_body_on && !gpui_self_nav
        && !gamepad_ui_menu_active() && !dialog_active() && !inven_any_open()
        && interact_prompt_candidate_count() >= 2) {
        // ⚠️ THE PICKER OWNS THE STICK OUTRIGHT while it is available -- at EVERY deflection, not
        // just past the aim threshold. Claiming only the decisive band left every gentle push
        // falling through to the cursor path, which summons the red iso pointer for two full
        // seconds ("stále vidím červený kurzor"). Same rule the enemy lock already follows: while
        // something is selected, the right stick selects, full stop.
        ipPickTook = mag > GPUI_STICK_DEADZONE;

        // CONTINUOUS, not edge-triggered: re-aimed every frame the stick is out, so circling the
        // stick sweeps the selection round with it. The arm/re-centre ratchet this replaced made
        // you flick-release-flick to walk the outline across a room -- fine for cycling enemies you
        // cannot see, wrong for pointing at a cabinet you can.
        //
        // No refusal sound when nothing lies that way. A stick push is not a button press: it gets
        // nudged constantly while moving and looking around, and buzzing at every one of those is
        // noise. The blip fires only when the selection actually changes (interact_prompt_pick_-
        // direction returns true only then), so a sweep ticks once per object it passes over.
        if (mag > GPUI_IP_PICK_THRESHOLD) {
            if (interact_prompt_pick_direction(x / mag, y / mag)) {
                gsound_play_sfx_file("ib1p1xx1");
            }
        }
    }

    if (mag > GPUI_STICK_DEADZONE && !ipPickTook && !lockedGameplay && !radial_wheel_is_open() && !gpui_aim_body_on && !gpui_self_nav) {
        float t = (mag - GPUI_STICK_DEADZONE) / (1.0f - GPUI_STICK_DEADZONE);
        if (t > 1.0f) {
            t = 1.0f;
        }
        float speed = t * t * GPUI_CURSOR_MAX_SPEED;
        *dx += (int)(x / mag * speed);
        *dy += (int)(y / mag * speed);

        // Summon the pointer: right-stick movement is the ONLY thing that shows the
        // cursor (it stays visible GPUI_CURSOR_SHOW_MS after the last nudge, then hides).
        gpui_cursor_show_time = get_time();
    }

    // Left stick -> item snapping while a menu/dialog is open (overrides the
    // right-stick delta on the frames a step fires). The main menu, the New Game
    // character-selection screen, and conversation are included explicitly: their
    // windows are WINDOW_HIDDEN / non-modal (WINDOW_MOVE_ON_TOP) rather than
    // WINDOW_MODAL, so the modal check misses them even though their buttons are
    // standard GNW buttons. (The character editor itself is modal and is covered
    // by the menu check.) For dialogue the topmost buttoned window is gOptionWin
    // -- the reply sentences (created after the reply/control windows) -- so the
    // snap lands on the player's options. Suppressed while a hold-drag action
    // popup is up: that popup runs over a modal inventory window, so the modal
    // check is true, but the left stick must drive the popup's mouse-Y nav (done
    // in gpui_action_buttons), not snap to the underlying window's buttons.
    if ((gamepad_ui_menu_active() || in_main_menu || select_window_id != -1 || dialog_active())
        && !gpui_action_popup_active && !gpui_self_nav && !gpui_suppress_snap) {
        gpui_menu_snap(pad, dx, dy);
    }

    // Right trigger -> left mouse button. Level-based: held = button down, so a
    // press-release is exactly one click (no auto-repeat) and holding lets you
    // drag.
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GPUI_TRIGGER_THRESHOLD) {
        *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
    }

    // While a radial wheel (skill on L3) OR the on-body aimed-shot picker (LB) is up, the right
    // stick is selecting and the hold button owns the frame: suppress the face-button
    // click/shortcut actions so a stray A/B/X/Y can't fire a world click or open a menu
    // mid-selection. (Both are driven from gamepad_ui_tick, not here.)
    if (!radial_wheel_is_open() && !gpui_aim_body_on) {
        // A/Y -> left mouse button (interact/attack arbitration) and left-stick
        // navigation of hold-drag action popups. Suppressed under self_nav: a screen
        // driving its own nav (main menu, options, load/save) reads A directly and must
        // not also get a synthetic click.
        if (!gpui_self_nav) {
            gpui_action_buttons(pad, dx, dy, buttons);
        }

        // START/BACK shortcuts last: these may open a modal screen (and re-enter this
        // hook), so let this frame's cursor/click deltas resolve first. NOT gated on
        // self_nav -- these are how START closes the options menu it opened (the toggle),
        // and gpui_button_shortcuts has its own per-screen guards.
        //
        // Drop the reentrancy guard first: the inventory / Pip-Boy / char sheet / pause menu
        // open from HERE (handle_inventory, gpui_open_dpad_screen) and run their OWN input loop
        // inside this call, which reaches the pad through this very hook. Leaving the guard up
        // froze the pad in every one of those screens -- "I can't control the inventory and can't
        // get back out of it." Re-entry from a nested modal loop is legitimate; only re-entry
        // through a file read is not, and nothing below this point reads from disk.
        gpui_in_mouse_hook = false;
        gpui_button_shortcuts(pad);
    }
}

// REENTRANCY GUARD -- this hook must never run inside itself.
//
// The engine pumps the mouse from inside FILE I/O: db_fgetc calls gameMouseRefreshImmediately
// (the "keep the cursor alive while loading" hack) -> gmouse_bk_process -> mouse_info -> THIS
// hook. So any disk read the hook itself triggers re-enters it, and the recursion is unbounded.
// That is exactly the map-exit crash: leaving the map, gpui_action_buttons asked
// combat_rt_hostile_in_attack_range() -> item_w_range() -> proto_ptr() -> proto_list_str() ->
// db_fgets() -> mouse refresh -> the hook again -> item_w_range() ... The nested proto read ran
// on top of the outer one, proto_ptr failed, and item_w_range (item.cc:1353) dereferenced the
// NULL proto -> ACCESS_VIOLATION reading 0x34. The latch breaks the cycle at the second entry;
// the normal per-frame call from get_input is untouched. Keep any new hook work I/O-free anyway
// (see gpui_hostile_in_range_cached) -- the latch stops OUR recursion, but a first-entry hook
// call can still land in the middle of the ENGINE's own read.
//
// The latch guards THIS function's own work only. Any modal screen the hook opens (inventory,
// Pip-Boy, char sheet, pause menu) runs a nested input loop that legitimately needs the hook, so
// gpui_mouse_hook_impl clears the flag before gpui_button_shortcuts -- see the note there.
static void gpui_mouse_hook(int* dx, int* dy, int* buttons)
{
    if (gpui_in_mouse_hook) {
        return;
    }

    gpui_in_mouse_hook = true;
    gpui_mouse_hook_impl(dx, dy, buttons);
    gpui_in_mouse_hook = false;
}

// Flight-recorder: log every gamepad button up/down edge in ONE place (TRACE_INPUT), so
// the trace shows exactly what the player pressed regardless of which handler consumes it
// (A/B/X/Y/LB/RB/L3/R3/Start/Back/DPad). Analog sticks/triggers are logged separately by
// combat_rt's movement/aim code. Edges are naturally sparse, so no coalescing is needed.
static void gpui_trace_buttons(SDL_GameController* pad)
{
    if (!trace_enabled()) {
        return;
    }
    static bool prev[SDL_CONTROLLER_BUTTON_MAX] = { false };
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        bool now = SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b) != 0;
        if (now != prev[b]) {
            const char* name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
            trace_event(TRACE_INPUT, "btn", "%s %s", name != NULL ? name : "?", now ? "down" : "up");
            prev[b] = now;
        }
    }
}

void gamepad_ui_tick()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        // Controller vanished mid-selection: drop the overlay so it can't linger.
        if (radial_wheel_is_open()) {
            radial_wheel_close();
            gpui_release_aim_figure();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
        }
        // No controller -> restore the pointer (a mouse/keyboard player needs it).
        gmouse_set_move_cursor_hidden(false);
        return;
    }

    SDL_GameControllerUpdate();
    gpui_trace_buttons(pad);

    // Sample the hostile-in-range answer HERE (safe main-loop context), not in the mouse hook --
    // the query reads protos off disk and the hook can run from inside the engine's file I/O.
    gpui_hostile_in_range_cached = combat_rt_hostile_in_attack_range();

    gpui_skill_wheel(pad);
    gpui_quick_wheel(pad);
    gpui_weapon_wheel(pad);
    gpui_ammo_wheel(pad);
    gpui_aim_wheel(pad);

    // PAUSE THE WORLD WHILE A WHEEL IS UP. combat_rt freezes its own clock (see combat_rt_update),
    // but critter animations and scripts run off the engine's background processes, so without this
    // the enemies would keep walking and swinging behind the panel while you picked a stimpak.
    //
    // Edge-triggered against the previous frame, and it must be, because these calls are a COUNTER
    // inside the engine, not a flag: calling disable every frame the wheel is up would sink it
    // arbitrarily deep and one enable would never bring it back. Toggled after the wheel drivers
    // above so an open and its pause land in the same frame.
    static bool gpui_wheel_paused = false;
    bool wheelUp = radial_wheel_is_open();
    if (wheelUp != gpui_wheel_paused) {
        gpui_wheel_paused = wheelUp;
        if (wheelUp) {
            map_disable_bk_processes();
        } else {
            map_enable_bk_processes();
        }
    }

    // Contextual world "use" prompt (nearest interactable + floating hand + A to use). Safe main-
    // loop context, so it also performs any interaction the A hook queued this frame.
    interact_prompt_tick();

    // Hide the red iso MOVE cursor during free-play, show it briefly after a right-stick
    // nudge (gpui_cursor_show_time is stamped in gpui_mouse_hook when the stick moves it).
    // This drives the game's cursor OBJECT (gmouse), NOT the low-level mouse pipeline, so
    // it can't affect controller input. Runs only in gameplay (this tick), so menus /
    // dialogue keep their pointer. Only the MOVE cursor is hidden -- aim/skill cursors show.
    int cursorMode = gmouse_3d_get_mode();
    bool rightStickRecent = (get_time() - gpui_cursor_show_time) < GPUI_CURSOR_SHOW_MS;
    bool hideCursor = pad != NULL
        && (cursorMode == GAME_MOUSE_MODE_MOVE || cursorMode == GAME_MOUSE_MODE_ARROW)
        && !rightStickRecent;
    gmouse_set_move_cursor_hidden(hideCursor);
}

// Draw-suppress predicate (installed on the mouse module, consulted every frame in every
// loop). Dead simple: with a controller connected the pointer is HIDDEN by default,
// everywhere, and SHOWN only for a short window after the player nudges the RIGHT stick
// (which is also what moves it). No context detection -- and because suppression is
// draw-only (mouse.cc never touches mouse_is_hidden), this can only ever affect whether
// the cursor is drawn, never the controller input. Menus/dialogue convey selection via
// the highlighted item under the snapped cursor, so the pointer isn't needed to navigate.
static bool gpui_hide_cursor_predicate()
{
    if (gpui_controller == NULL) {
        return false;
    }
    // Only auto-hide during live iso GAMEPLAY. In any menu / dialogue / world-map loop
    // gamepad_ui_tick doesn't run, so freshness goes stale and the cursor stays VISIBLE
    // for navigation. Also always show it over a modal window or an open inventory (in
    // case one is driven from the main loop) -- biasing toward "shown" can never strand
    // the player, only leave a cursor up. Within gameplay, show it briefly after a
    // right-stick nudge.
    bool inGameplay = (get_time() - gpui_last_tick_time) < GPUI_TICK_FRESH_MS
        && win_get_top_modal() == -1
        && !inven_any_open();
    bool rightStickRecent = (get_time() - gpui_cursor_show_time) < GPUI_CURSOR_SHOW_MS;
    return inGameplay && !rightStickRecent;
}

void gamepad_ui_init()
{
    // Keep delivering controller input when the window isn't focused. SDL gates
    // joystick/controller input on focus by default, so an overlay or a brief
    // focus blink mid-fight (Steam/Discord/Xbox Game Bar, a notification) would
    // otherwise kill the gamepad until a physical replug forced a device event.
    // Must be set before the gamecontroller (joystick) subsystem initializes.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        gpui_acquire();
    }
    mouse_set_input_hook(gpui_mouse_hook);
    // Cursor-hiding TEMPORARILY DISABLED: it kept stranding controller input. With the
    // hook not installed the mouse is 100% vanilla (cursor always drawn, input tracked
    // everywhere). Re-enable by restoring this call once hiding is proven safe.
    // mouse_set_draw_suppress_hook(gpui_hide_cursor_predicate);
}

void gamepad_ui_exit()
{
    gpui_release_aim_figure();
    gpui_free_skill_art();
    gpui_icons_release();

    mouse_set_input_hook(NULL);
    mouse_set_draw_suppress_hook(NULL);

    if (gpui_controller != NULL) {
        SDL_GameControllerClose(gpui_controller);
        gpui_controller = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

} // namespace fallout
