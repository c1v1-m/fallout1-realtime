#include "game/gamepad_ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/autotest.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/art.h"
#include "game/automap.h"
#include "game/combat.h"
#include "plib/gnw/text.h" // body-part label on the aimed-shot marker
#include "game/combat_defs.h"
#include "game/combat_rt.h"
#include "game/editor.h"
#include "game/game_trace.h"
#include "game/gdialog.h"
#include "game/gconfig.h"
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
#include "game/perk.h"
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
#include "plib/db/db.h" // the hand-painted aim masks come out of data\aimmask.dat
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"

namespace fallout {

// Right stick -> cursor tuning.
#define GPUI_AXIS_MAX 32767.0f
#define GPUI_STICK_DEADZONE 0.2f
#define GPUI_CURSOR_MAX_SPEED 14.0f // pixels per frame at full deflection
// Both are cfg (`cursor_speed`, `cursor_deadzone`), read at gamepad_ui_init. Aiming precision is
// pure feel and only shows up in play -- he reported the free cursor as "sometimes hard to hit what
// I want". LOWER speed = finer control (the response is already squared, so small pushes are gentle;
// what a high ceiling costs you is the last inch). Deadzone only sets where movement STARTS.
static float gpui_cursor_max_speed = GPUI_CURSOR_MAX_SPEED;
static float gpui_stick_deadzone = GPUI_STICK_DEADZONE;
// Right trigger -> left click threshold (~0.5 of the 0..32767 trigger range).
#define GPUI_TRIGGER_THRESHOLD 16000

// ⚠️ A TRIGGER DRIVING A TAP/HOLD GESTURE NEEDS HYSTERESIS, AND A SINGLE THRESHOLD IS NOT IT.
// GPUI_TRIGGER_THRESHOLD is one level with no gap, so an analog axis resting anywhere near it
// DITHERS: a few counts of jitter either way produce a stream of phantom press/release edges.
// For the click that was survivable -- the worst case is a flickering mouse button -- but for the
// aimed shot it is fatal, because every phantom edge restamps lbDownTime and the 220 ms hold window
// can then never elapse. That is the "aiming sometimes catches, sometimes not" report: the gesture
// was not being ignored, its clock was being reset out from under it.
//
// The gap between the two levels IS the debounce; they must never be equal. The press level is also
// lower than the click's 50% -- half a trigger pull is a lot to ask for "start aiming", and that
// number was inherited from the click rather than chosen for this.
//
// ⚠️ LOWERED AGAIN 2026-09-08 (12000/6000 -> 8000/4000): "stisknutí LT bylo obtížné". 37 % of the
// travel is fine for a HOLD, where the finger is going all the way down anyway, but this gesture's
// common case is a TAP -- and a tap has to cross the level, stop, and come back inside 220 ms. At
// 37 % that is a long way to move a finger twice. The Schmitt gap keeps its 2:1 ratio, so nothing
// about the dither immunity changes; only the distance your finger has to travel does.
#define GPUI_AIM_TRIGGER_PRESS 8000   // ~24% pull to engage
#define GPUI_AIM_TRIGGER_RELEASE 4000 // ~12% pull to let go

// How long the axis must STAY low before the latch drops.
//
// ⚠️ HYSTERESIS ON THE VALUE IS NOT ENOUGH, BECAUSE THE BAD READINGS ARE ZERO. The axis
// intermittently reports a hard 0 for a frame while the trigger is physically held: measured in
// trace_2026-08-30_234101, 14 of 33 release transitions read exactly 0 and were immediately
// followed by a fresh press at 20000-30000. No value threshold can survive that -- 0 is below any
// release level you pick -- so the latch dropped and re-armed several times a second, each phantom
// press restamping lbDownTime, and the 220 ms hold window never elapsed. Zero aimed shots fired in
// that whole session. This is the second time the same symptom had a different cause; the first
// really was dither around a single threshold.
//
// A release therefore has to persist. 60 ms is about four frames -- long enough to swallow a
// dropped reading, far too short to notice when letting go on purpose.
#define GPUI_TRIGGER_RELEASE_MS 60

// "Not currently low", as a timestamp sentinel. Not 0: get_time() can legitimately BE 0.
#define GPUI_TRIGGER_NOT_LOW 0xFFFFFFFFu

// Schmitt trigger over an analog axis, with a debounced release. `state` and `lowAt` are the
// caller's own, so each gesture keeps its own pair and they cannot interfere.
static bool gpui_trigger_held(SDL_GameController* pad, SDL_GameControllerAxis axis,
    bool* state, unsigned int* lowAt)
{
    int v = SDL_GameControllerGetAxis(pad, axis);

    if (*state) {
        if (v < GPUI_AIM_TRIGGER_RELEASE) {
            if (*lowAt == GPUI_TRIGGER_NOT_LOW) {
                *lowAt = get_time();
            } else if (get_time() - *lowAt >= GPUI_TRIGGER_RELEASE_MS) {
                *state = false;
            }
        } else {
            // Back above the release level -- whatever that dip was, it was not a release.
            *lowAt = GPUI_TRIGGER_NOT_LOW;
        }
    } else if (v > GPUI_AIM_TRIGGER_PRESS) {
        *state = true;
        *lowAt = GPUI_TRIGGER_NOT_LOW;
    }

    return *state;
}

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

// Aimed-shot (VATS) LT tap-vs-hold split: a press+release shorter than this fires an aimed shot at
// the last-used body part IMMEDIATELY (no wheel); holding past it opens the body-part wheel to pick.
#define GPUI_AIM_TAP_MS 220

// How long the stick has to REST on a new body part before the selection actually moves there.
//
// Without it the pick was recomputed every frame straight from the stick angle, so sweeping across
// the body flickered through three or four parts on the way to the one you wanted, and any tremor
// near a boundary bounced the selection between two neighbours. "chci head tak drzim a tocim a
// utika mi to" -- the selection was not running away, it was tracking the stick far too eagerly.
//
// ⚠️ IT DELAYS THE COMMIT, NOT THE FEEDBACK. What you are pointing at is computed every frame as
// before; only the ACT of changing the selection waits. Under the window the marker simply stays
// where it was, so a sweep reads as "nothing happens until I settle", never as input lag on the
// thing you are watching.
#define GPUI_AIM_SETTLE_MS 130

// Stick deflection needed to aim at all, as a fraction of full throw. Below it the stick counts as
// centered, which selects the TORSO (see gpui_aimbody_pick). Higher than the old 0.18: a thumb
// resting on the stick sits well above 0.18 on a worn pad, which meant the picker steered on noise.
//
// ⚠️ THIS IS THE ONLY PLACE MAGNITUDE IS READ. Past it, the part is chosen by ANGLE alone -- see
// gpui_aim_ring_slot. Every earlier layout mixed the two and that is what kept breaking.
#define GPUI_AIM_STICK_MIN 0.22f

#define GPUI_PI 3.14159265358979323846f

// Quick-use wheel B tap-vs-hold split: hold B past this to bring up the drug wheel; a
// shorter press stays the weapon's firing-mode cycle. Same window as the aimed-shot split
// so both holds feel identical in the hand.
#define GPUI_QUICK_HOLD_MS GPUI_AIM_TAP_MS

// Hold threshold for BACK and START (inventory / pause menu on the tap, exit-run / quick
// save on the hold). Deliberately longer than the wheels: these are pressed deliberately
// rather than drummed, and 220 ms would turn a firm press into the hold action.
#define GPUI_MENU_HOLD_MS 450

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
// Window between two A presses for the DOUBLE TAP that loots the body you are standing on during a
// fight. Short on purpose: a deliberate double tap is quick, while attacking taps are paced by the
// weapon's rate of fire and land much further apart.
#define GPUI_LOOT_DOUBLE_TAP_MS 300

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

// ⚠️ "A NESTED MODAL JUST ATE PAD INPUT -- RE-PRIME YOUR EDGES."
//
// Bumped by dialog_out when its box closes. Any loop that keeps its OWN prevA/prevB (the load/save
// slot list, Preferences, the character screens...) is frozen while such a box runs its own input
// loop, so its edge state describes the world from BEFORE the box. The button the player pressed to
// answer the box is usually still physically down when the outer loop resumes -- and reads as a
// fresh press.
//
// That is exactly what ate his save: B answered "no, don't overwrite", the box closed, and the same
// unreleased B rose-edged again in the slot list as ESCAPE, which closed the whole save screen.
// From his side the Cancel button simply did not work and the menu vanished.
//
// Same family as the dialog's own dialog_pad_prime (a box opened by a held A answering itself) and
// the self-nav click leak -- an edge tracker that stops being called does not stop being WRONG.
static unsigned int gpui_input_epoch = 0;

unsigned int gamepad_ui_input_epoch()
{
    return gpui_input_epoch;
}

void gamepad_ui_bump_input_epoch()
{
    gpui_input_epoch++;
}

// An A press whose interaction was held back because a use-on item is in hand, so the press might
// still turn out to be a HOLD (which arms the use-item cursor instead). Set in gpui_action_buttons
// on the press edge, resolved in gpui_useon_arm on the tick: armed -> dropped, released early ->
// the interaction runs then. See the note at its use.
static bool gpui_a_interact_deferred = false;

// True while the ACTIVE HAND holds an item the engine can "use on" something (proto_action_can_use_on).
static bool gpui_useon_item_in_hand();
static bool gpui_targeting_cursor_armed();
// Puts the pointer a few tiles in front of the dude (the use-on crosshair's starting spot).
static void gpui_useon_park_cursor();

// A screen is running its OWN controller-driven navigation and reads the pad
// directly in its own loop -- the main menu (highlight + A) and the load/save slot
// list (left stick / D-pad step, A/B confirm/cancel). While set, the shared hook
// must NOT also left-stick-snap the cursor or map A/Y to a click -- both would fight
// that nav (the snap yanks the cursor onto buttons/scroll arrows). The right stick
// can still move the cursor and RT still clicks, for optional mouse-style use.
// BACK HELD = RUN TO THE NEAREST MAP EXIT (was LB+RB before the 2026-09-16 layout dropped the
// modifier button). Set from the mouse hook (gpui_button_shortcuts), performed on
// the safe tick (gamepad_ui_tick) for the same reason the heal is: it registers an animation.
static bool gpui_exit_nav_pending = false;

static bool gpui_self_nav = false;

// A self-nav screen that STILL WANTS THE FREE POINTER on the right stick -- the world map.
//
// self_nav takes the stick away from the cursor because on a MENU a creeping pointer hovers buttons
// (the hover blip the title menu misread as its own move sound). The world map is not a menu: its
// pointer is how you say WHERE -- a town, a spot on the map -- and A then acts on what it is over
// ("měl jsem kurzor myši na The Hub a áčko nereagovalo"). So it keeps the stick, and only while no
// modal sits on top of it (a dialog box or the pause menu opened from there is a menu again).
static bool gpui_self_nav_cursor = false;

// Suppresses ONLY the left-stick item snap (gpui_menu_snap), leaving A/Y clicks and
// RT alone. The barter/trade window sets this while it drives its own LB/RB cursor
// stepping: without it, the snap grabs the trade window's slot and scroll-arrow
// buttons and fights that stepping. Unlike gpui_self_nav, it must NOT disable the
// A-router, since barter still relies on A to transfer the item under the cursor.
static bool gpui_suppress_snap = false;

// Set by a screen that handles A ITSELF (character editor, character selection). Those screens are
// modal, so gamepad_ui_menu_active() is true and the A-router below would ALSO synthesise a left
// click -- at whatever stale position the pointer was left at, which on the character sheet meant
// A both spent a point AND pressed whatever the cursor happened to be sitting on. He caught it
// hitting PRINT: "kurzor byl dole u zbranÃÂÃÂ­, a kdyÃÂÃÂ¾ jsem pÃÂÃÂidÃÂÃÂ¡val body, maÃÂÃÂkalo to PRINT".
//
// ⚠️ NARROWER THAN gpui_self_nav ON PURPOSE. self_nav also takes the right stick away from the
// cursor, and these screens want it: it is how you reach PRINT / DONE / OPTIONS, which the pad
// nav does not step through. Only the A CLICK is suppressed; RT stays the deliberate click.
static bool gpui_suppress_a_click = false;

bool gamepad_ui_a_click_suppressed()
{
    return gpui_suppress_a_click;
}

bool gamepad_ui_set_suppress_a_click(bool suppress)
{
    bool previous = gpui_suppress_a_click;
    gpui_suppress_a_click = suppress;
    return previous;
}

// Aim-picker cancel handshake. Declared up here rather than beside gpui_aim_body_on because the
// B-button shortcut handler further down the file has to see it: the press that cancels an aim
// must not ALSO switch the weapon's firing mode.
//
// ⚠️ ITS TWIN IS GONE. There used to be a second flag (gpui_aim_canceled) whose only job was to eat
// the trigger RELEASE that followed a cancel, because a release used to mean "fire". In this layout
// a release just closes the picker, so there is nothing left to eat.
static bool gpui_aim_swallow_b = false; // that B press is spent

static bool gpui_on_virtual_pad = false; // the handle is autotest.cc's virtual pad

// Opens the first connected controller if we don't have one yet.
static void gpui_acquire()
{
    if (gpui_controller != NULL) {
        return;
    }

    // The game playing by itself (autotest.cc) uses its virtual pad, never a real one.
    int virtualPad = autotest_controller_index();
    if (virtualPad >= 0) {
        gpui_controller = SDL_GameControllerOpen(virtualPad);
        gpui_on_virtual_pad = gpui_controller != NULL;
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
    if (gpui_controller != NULL && !gpui_on_virtual_pad && autotest_controller_index() >= 0) {
        SDL_GameControllerClose(gpui_controller); // a real pad opened before the virtual one existed
        gpui_controller = NULL;
    }
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

bool gamepad_ui_set_self_nav_cursor(bool active)
{
    bool previous = gpui_self_nav_cursor;
    gpui_self_nav_cursor = active;
    return previous;
}


// Set while a PRE-GAME modal screen with no live session is up -- currently the death scene.
// Sibling of in_main_menu, and it exists for the identical reason: those screens run their own
// get_input() loop, which reaches the pad through the shared mouse hook, so without a gate the
// gameplay shortcuts fire into a game that has already been torn down. On the death screen that
// meant BACK opened the inventory over a reset dude and everything after it was wrong
// ("je mozne otevrit okno inventory a pak je cela hra nejak rozesrana").
static bool gpui_no_session = false;

void gamepad_ui_set_no_session(bool active)
{
    gpui_no_session = active;
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

// LEFT STICK AS A LIST STEPPER for modal screens: -1 up, +1 down, 0 nothing. Edge plus repeat, the
// same cadence the D-pad snap uses, so a screen can offer both without writing the timing twice --
// and so they FEEL the same, which is the part a player notices.
//
// ⚠️ A FIRM PUSH, not the walking deadzone. This is a menu: a stick resting slightly off centre must
// not walk the selection, and there is no analogue speed to recover here the way there is in the
// world. Half deflection is the threshold.
//
// ⚠️ The statics are shared across screens on purpose -- only one modal is ever up -- and they
// self-clear the moment the stick returns to centre, so nothing carries from one screen to the next.
#define GPUI_MENU_STICK_THRESHOLD 16000

int gamepad_ui_menu_stick_step()
{
    static int lastDir = 0;
    static Uint32 lastStepTime = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        lastDir = 0;
        return 0;
    }

    SDL_GameControllerUpdate();

    int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);

    int dir = 0;
    if (y < -GPUI_MENU_STICK_THRESHOLD) {
        dir = -1;
    } else if (y > GPUI_MENU_STICK_THRESHOLD) {
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
                if (inven_normal_cursor_over_list()
                    || (type == INVENTORY_WINDOW_TYPE_NORMAL && inven_normal_cursor_over_armor())) {
                    // Over the item list (or, in the normal inventory, the armor slot -- A there
                    // takes the armor off): defer the quick action to handle_inventory's loop --
                    // the use/equip pipeline is not safe to run inside the input hook (see
                    // gpui_quick_normal).
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
// The quick slots (see the note at gpui_consumed_lb), declared up here for the binding in this hook.
#define GPUI_QS_LB 0
#define GPUI_QS_RB 1
static void gpui_qs_bind_under_cursor(int slot);

static void gpui_action_buttons(SDL_GameController* pad, int* dx, int* dy, int* buttons)
{
    static bool prevA = false;
    static bool prevRB = false;
    static bool prevLB = false;
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
    bool lbHookNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool lbEdge = lbHookNow && !prevLB;
    prevA = aNow;
    prevRB = rbNow;
    prevLB = lbHookNow;

    // ⚠️ A SCREEN JUST RAN ITS OWN INPUT LOOP -- our edges describe the world before it, so adopt
    // the live pad state and emit NOTHING this frame.
    //
    // These statics outlive every modal screen. Confirm a level-up perk by holding A, and the same
    // still-held A rose-edged HERE the moment the window closed: the dude walked over and started
    // looting the corpse the hand icon happened to be sitting on. Every screen with its own loop
    // has the same exit, which is why this is fixed once, here, rather than per screen.
    //
    // Same counter and the same reasoning as the load/save screen and the confirmation boxes
    // (gamepad_ui_bump_input_epoch): entering or leaving a nested loop invalidates button edges.
    static unsigned int lastEpoch = 0;
    unsigned int epoch = gamepad_ui_input_epoch();
    if (epoch != lastEpoch) {
        lastEpoch = epoch;
        gpui_a_interacting = false;
        return;
    }

    // ⚠️ EDGES ARE TRACKED EVEN UNDER self_nav -- only the ACTIONS below are skipped, and
    // that is why this returns HERE rather than the caller skipping the call.
    //
    // A screen that drives its own navigation reads A itself (options menu, Preferences,
    // main menu, load/save). If this function were simply not called while such a screen is
    // up, prevA would still hold whatever it held before it opened -- so the A that is still
    // physically down when the screen closes reads as a FRESH press on the next frame and
    // fires a world click. That is the "pressed DONE and immediately started talking to
    // Ian" bug: the click landed on whoever the cursor happened to be over.
    if (gpui_self_nav) {
        gpui_a_interacting = false;
        return;
    }

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
            // ⚠️ AN ARMED TARGETING CURSOR OWNS A OUTRIGHT, ahead of every other rule here. A skill
            // armed from the L3 wheel (or the use-on crosshair from holding A) is aimed and then
            // CONFIRMED with A, and that confirming press has to reach gmouse as a plain click --
            // it is what runs action_use_skill_on. Without this it fell through to the normal
            // arbitration and the interact prompt answered instead: standing at a locked cabinet
            // with TRAPS armed, A took the container path and printed "It is locked." rather than
            // using the skill ("chtÃÂÃÂl jsem dÃÂÃÂ¡t traps na skÃÂÃÂÃÂÃÂ­ÃÂÃÂ, ale byl jsem blÃÂÃÂ­zko skÃÂÃÂÃÂÃÂ­nÃÂÃÂ").
            //
            // Being CLOSE is what did it: interact_prompt_target_adjacent() below is exactly the
            // override that lets an adjacent object beat a fight, so proximity handed A away. It
            // also fixes the mirror case -- with a hostile in range and nothing adjacent, A would
            // have ATTACKED instead of applying the armed skill.
            bool targetingArmed = gpui_targeting_cursor_armed();

            // ⚠️ A ALWAYS INTERACTS NOW. The whole arbitration this used to run -- "is a hostile in
            // weapon range, is there a lock, is the thing I am standing at adjacent enough to beat
            // a fight" -- existed because A meant BOTH interact and attack, and every one of its
            // branches was a guess about which one you meant. The 2026-09-16 layout moved the
            // attack to RT, so there is nothing left to arbitrate: A opens the door beside the
            // raider, and RT shoots him. The variable stays because the click-suppression and loot
            // paths below read it.
            gpui_a_interacting = true;

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
            // ⚠️ WITH A USE-ON ITEM IN HAND, A's MEANING CANNOT BE DECIDED ON THE PRESS EDGE.
            //
            // Holding A arms the use-item-on cursor (gpui_useon_arm), but this block fires the
            // prompt's interaction the INSTANT A goes down -- so standing at the elevator with the
            // rope in hand, the press rode the elevator before the hold was ever recognised ("the
            // elevator is highlighted and the press-the-elevator action fills up"). Tap and hold
            // have to be told apart first, so the interaction is DEFERRED to the release and only
            // happens if the press never became a hold. Same shape as the B / X / Y tap-vs-hold
            // splits; deferring costs the tap a few frames and nothing else.
            //
            // Only when a use-on item is actually held: with anything else A stays instant.
            bool interactNow = false;
            // ⚠️ NOT once the cursor is already ARMED. The rope is still in hand then, so this test
            // stays true -- but that A is the CONFIRMING press and must reach gmouse as a plain
            // click immediately. Deferring it would suppress the very click that applies the item.
            bool deferForUseOn = gpui_useon_item_in_hand() && !targetingArmed;
            if (targetingArmed) {
                // Nothing to do here, and that is the point: fall through to the plain left click
                // below. No interaction runs, so interactNow and aLooting both stay false and
                // nothing swallows the press.
            } else if (deferForUseOn) {
                gpui_a_interact_deferred = true;
            } else if (gpui_a_interacting && !gamepad_ui_menu_active() && !dialog_active()) {
                // ⚠️ A CORPSE LOOTS ON ONE A, MID-FIGHT TOO (2026-10-17, "áčko na mrtvolu při boji je
                // loot. teď hráč musí klikat double A pro loot"). The post-kill cooldown used to hold a
                // corpse back while anything hostile was in range -- a drawn gun makes that the whole
                // fight -- leaving only the double tap below. Both dated from when A ALSO FIRED: mashing
                // it to kill would pop the fresh body. RT has been the only attack since 2026-09-16, so
                // nothing mashes A any more and a corpse is one press like a door.
                interactNow = interact_prompt_request() || combat_rt_try_world_loot();
            }
            // A deferred press must not drop through as a world walk-click either.
            interactNow = interactNow || deferForUseOn;
            // ⚠️ SWALLOW THE PRESS ONLY IF SOMETHING ACTUALLY HAPPENED, or if this is the genuine
            // post-kill moment. It used to swallow on `postKillCooldown` alone -- and that flag is
            // TRUE FOR THE WHOLE FIGHT, because its anchor is refreshed every frame a hostile sits
            // within weapon range, which a drawn gun makes true across the entire screen. So
            // standing next to a corpse mid-fight, A did NOTHING AT ALL: the loot was refused by
            // the cooldown and the attack was eaten by this line.
            //
            // The guard's real job is the instant the LAST enemy dies -- an A still being mashed
            // must not pop the fresh body. That moment is "cooldown running AND nothing left to
            // fight", which is exactly what this now tests. While enemies are still up, an
            // unhandled A falls through and attacks, like it always should have.
            bool freshKillGuard = postKillCooldown
                && !gpui_hostile_in_range_cached
                && !combat_rt_has_lock();

            aLooting = !targetingArmed
                && gpui_a_interacting
                && !gamepad_ui_menu_active()
                && !dialog_active()
                && (freshKillGuard || interactNow);
        }
        // HOLD A TO LOOT A BODY YOU ARE STANDING ON, mid-fight, gun out. His idea, and the reason
        // it costs nothing here: the TAP is not deferred at all. The attack still fires on the
        // press edge exactly as before -- this only watches whether the finger STAYS down, and
        // acts later. No hold threshold is ever added to the time it takes to shoot.
        //
        // ⚠️ Adjacency alone could not do this job. Letting a neighbouring corpse flip A to
        // "interact" would take attacking away precisely when you need it: kill one enemy in
        // melee, you are standing on it, and every A meant for the NEXT enemy would open the body
        // instead. The hold says "I mean the corpse" without ever making a tap ambiguous.
        //
        // Deliberately narrow: only a CORPSE, only an ADJACENT one, only once per press.
        // DOUBLE-TAP A TO LOOT the body you are standing on, mid-fight, gun out.
        //
        // ⚠️ It is a double tap and not a HOLD because holding A is SUSTAINED FIRE: the attack
        // intent is re-armed every frame the button is down (combat_rt.cc, `if (attack && ...)`,
        // not edge-gated). A hold-to-loot therefore stole the trigger from anyone standing next to
        // a body -- and stepping off the body is not free either, walking costs AP.
        //
        // ⚠️ And it is gated on the attack cooldown, because MASHING A is a real way people fight
        // and it produces double taps on its own. While the dude is inside his own rate-of-fire
        // cooldown he is shooting, so the taps mean shoot. The gesture only lands in a lull --
        // nothing in range, target already dead, weapon between shots -- which is exactly when a
        // player would reach for the corpse.
        static unsigned int aLastTapAt = 0;
        static bool aTapLooted = false;
        if (aEdge) {
            unsigned int now = get_time();
            bool second = now - aLastTapAt <= GPUI_LOOT_DOUBLE_TAP_MS;

            if (second && !aTapLooted
                && !gpui_targeting_cursor_armed()
                && !gamepad_ui_menu_active()
                && !dialog_active()
                && !combat_rt_player_attack_busy()) {
                aTapLooted = combat_rt_try_world_loot();
            } else {
                aTapLooted = false;
            }

            aLastTapAt = now;
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

            // ⚠️ A NEVER FIRES THE GUN. "RT bude jen utok" has an unspoken other half -- nothing
            // ELSE is -- and the combat crosshair broke it silently: in GAME_MOUSE_MODE_CROSSHAIR a
            // left click on a critter goes straight to combat_attack_this (gmouse.cc:1007), and
            // while a lock is held combat_rt GLUES the cursor onto the target. So every A meant as
            // "use that" was a click on an enemy, i.e. a shot -- his report, and he had seen it
            // "even during normal aiming", which is exactly when the mode is up.
            //
            // Scoped to the attack crosshair alone. USE_CROSSHAIR (the rope, a lockpick, dynamite
            // aimed from the hand) is a DIFFERENT mode and still needs A to confirm on the target.
            // Interactions are unaffected either way: A reaches doors, corpses and people through
            // interact_prompt's queued request, never through this synthetic click.
            bool worldAttackClick = gmouse_3d_get_mode() == GAME_MOUSE_MODE_CROSSHAIR
                && !gamepad_ui_menu_active()
                && !dialog_active()
                && !inven_any_open();

            if (worldAttackClick) {
                static unsigned int lastAttackClickVeto = 0;
                if (get_time() - lastAttackClickVeto > 500) {
                    lastAttackClickVeto = get_time();
                    trace_event(TRACE_INPUT, "a-attack-vetoed", "lock=%d",
                        combat_rt_has_lock() ? 1 : 0);
                }
            }
            // Edges above are updated either way -- only the CLICK is dropped. Skipping the
            // whole router instead is what leaked a stale click into the world once (see the
            // note at the top of this function).
            if (!worldClickAtHiddenCursor && !worldAttackClick && !gpui_suppress_a_click) {
                *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
            } else if (aEdge && gpui_targeting_cursor_armed()) {
                // The confirming A of an armed rope / lockpick / skill went nowhere -- say why.
                trace_event(TRACE_INPUT, "a-confirm-dropped", "hidden=%d attack=%d suppress=%d mode=%d",
                    worldClickAtHiddenCursor ? 1 : 0, worldAttackClick ? 1 : 0, gpui_suppress_a_click ? 1 : 0,
                    gmouse_3d_get_mode());
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

    // RB IN THE WORLD BELONGS TO AIMING NOW (tap = fire the last aimed shot again, hold = the ammo
    // wheel), so it no longer fires the engine's right-click there. That right-click only ever
    // cycled the world cursor between move and look/use -- a mode the interact prompt and A have
    // made redundant, and one that silently changed what the next press would do.
    //
    // ⚠️ IT IS KEPT ON THE SCREENS, where a right-click is a real action on a real widget and
    // nothing else wants RB. Same reason it is still refused while a radial wheel is up: there RB
    // PAGES the wheel, and letting the press also right-click would flip the world cursor behind
    // the overlay -- you would put the gun away and find yourself in the wrong cursor.
    //
    // LB+RB (run to the nearest map exit) died with the modifier button; it is a BACK hold now.
    // THE QUICK SLOTS ARE BOUND HERE (2026-10-15): in your own inventory, LB / RB over an item binds
    // it to that slot. RB's right-click there only switched the hand/arrow cursor, which the pad never
    // needed -- a held A opens the action menu.
    bool invNormal = invOpen && inven_current_window_type() == INVENTORY_WINDOW_TYPE_NORMAL;
    if (invNormal && (lbEdge || rbEdge)) {
        gpui_qs_bind_under_cursor(lbEdge ? GPUI_QS_LB : GPUI_QS_RB);
    } else if (rbEdge && !radial_wheel_is_open()
        && (gamepad_ui_menu_active() || inven_any_open() || dialog_active())) {
        *buttons |= MOUSE_STATE_RIGHT_BUTTON_DOWN;
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

    // ⚠️ Take the world prompt down BEFORE the screen paints. The floating hand and the target
    // outline live in their own always-on-top window, and interact_prompt_tick -- the only thing
    // that hides them -- does not run inside these screens' own input loops. So whatever the prompt
    // was pointing at when you pressed the button stayed drawn over the Pip-Boy ("kdyÃÂÃÂ¾ jdu do menu,
    // je vidÃÂÃÂt z in-game hry ruka"). The tick's own suppression list misses them too: it keys on
    // win_get_top_modal, and the Pip-Boy and character sheet are WINDOW_MOVE_ON_TOP, not modal.
    interact_prompt_reset();

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

    // ⚠️ The screen ran its own input loop and the button that CLOSED it is almost certainly still
    // held. Invalidate every button edge the shared layer is holding, or that press rises again the
    // instant we are back in the world -- which is how confirming a level-up perk with A had the
    // dude immediately loot the corpse under the hand icon.
    gamepad_ui_bump_input_epoch();
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

// Each wheel hangs off a tap-vs-hold button, so that button's TAP action has to wait for the
// RELEASE and only fire if the press never became a hold. One "this press is already spent" flag
// per button, set by whatever claimed it -- the wheel, or any gesture that used the same press.
//
// ⚠️ THE 2026-09-16 LAYOUT MOVED EVERY ONE OF THESE, so the names say the BUTTON, never the wheel
// that used to sit on it: B is cancel-only now, the drug wheel moved to Y, the weapon wheel to X,
// the ammo wheel to RB, and the skill wheel off L3 onto LB.
static bool gpui_consumed_b = false;  // B:  tap cancels, hold opens the DRUG wheel
static bool gpui_consumed_y = false;  // Y:  tap swaps hands, hold opens the WEAPON wheel, LT+Y spends it
static bool gpui_consumed_x = false;  // X:  tap switches firing mode, hold RUNS (no wheel any more)
static bool gpui_consumed_lb = false; // LB: tap reloads, hold draws the LB quick slot (2026-10-15)
static bool gpui_consumed_rb = false; // RB: a press already spent (paging a wheel, a screen closing)

// --- THE QUICK SLOTS: LB AND RB (2026-10-15) ---------------------------------------------------------
//
// His design, in three messages: *"když držím LB, dude si prohodí zbraň a bude držet granát a jakmile
// LB pustí, tak granát hodí ... vlevo dole u pásku bude LB s granátem pro quick throw nebo nějaký item
// na bindování, ale primárně throwable. Quick Pockets je tím taky ovlivněn, swap bude stát jeden bod.
// Kliknutí LB je reload"*, then *"RB uděláme jako LB, ale prvně se tam dá stimpack -- prostě taky jako
// quick slot"*. And his picks: the throw goes at the LT lock, else at the nearest enemy (RT without a
// lock); a slot picks its item by itself but can be bound in the inventory; drawing costs 2 AP, 1 with
// Quick Pockets, and putting the old weapon back is free.
//
//   LB  ⚠️ since 2026-10-17 exactly like RB: the slot is used on the PRESS (a drug or a tool), a
//       throwable drawn on the press and thrown on the release. The reload went to X's tap.
//   RB  the RB slot: a drug (the default, a stimpak) is taken on the press, as the heal always was; a
//       throwable is drawn on the press and thrown on the release.
//   B   while one is drawn: put it away, nothing thrown (costs the draw, like any swap).
//   In your own inventory LB / RB over an item binds it to that slot; again on the same item = auto.
//   Keyboard: T is the LB slot, H the RB slot -- a throwable is drawn and thrown in one go there.
//
// Nobody in range at the release: the throwable goes back and the old weapon comes out (buzz).
#define GPUI_QS_COUNT 2 // GPUI_QS_LB / GPUI_QS_RB are defined above gpui_action_buttons
#define GPUI_QS_HOLD_MS 220 // the same tap-vs-hold line the face-button wheels use
static int gpui_qs_bound[GPUI_QS_COUNT] = { -1, -1 };

static Object* gpui_quick_slot_item(int slot, int* count);
static bool gpui_item_is_throwable(Object* item);
static bool gpui_item_is_tool(Object* item);
static void gpui_qs_press(int slot, bool throwOnDraw);
static void gpui_qs_release(int slot);
static bool gpui_qs_cancel();
static bool gpui_qs_drawn(int slot);
// A slot item to TAKE (a drug), asked from the hook and used on the safe tick like every item use.
static int gpui_qs_use_pending_slot = -1;
static unsigned int gpui_qs_use_pending_since = 0;

// Y = DEDICATED HEAL (it was LB+B until the 2026-09-16 layout -- he moved it himself, having
// decided on 2026-08-31 to keep the combo; the modifier button is simply gone now). Set on the Y
// release when that press was a tap, and drained by gamepad_ui_tick, which is the only place it is
// safe to actually use an item: the press is read inside the mouse hook, and gpui_quick_use
// registers an animation. Same deferral the wheels use -- see gpui_use_best_heal.
static bool gpui_heal_pending = false;

// ⚠️ WHEN THE HEAL WAS ASKED FOR (2026-10-11). The press is read in the mouse hook, which runs in EVERY
// loop -- the character editor, the main menu -- but only the gameplay tick drains it. An RB pressed
// while creating the character (seen in a Deck trace: heal-press src=rb at s=38, the game started a
// minute later) would sit there and burn a stimpak on the first frame of the game. A request older
// than this is dropped instead (gpui_drain_requests).
static unsigned int gpui_heal_pending_since = 0;
#define GPUI_HEAL_STALE_MS 1500

// LB-tap reload with MODS "Reload animation" on: requested from the hook, started on the safe tick
// (it registers an animation), and dropped if the dude stays busy past GPUI_RELOAD_WAIT_MS.
static bool gpui_reload_pending = false;
static unsigned int gpui_reload_pending_since = 0;
#define GPUI_RELOAD_WAIT_MS 1000

// The 2 AP of the reload in progress have been taken (at its START, see gpui_reload_anim_start) and
// not yet earned by a finished reload. Cleared by the end callback; if the loop ends any other way,
// the tick hands them back.
static bool gpui_reload_charged = false;

// KEYBOARD & MOUSE (2026-10-10). V, or a right-click on the enemy the mouse is aiming at, opens the
// called-shot window the pad opens on R3. That window is modal, so the key only asks and the safe
// tick opens it (gpui_drain_requests) -- the same deferral every item use here gets.
static bool gpui_called_shot_pending = false;

// When the player last moved the REAL mouse or pressed one of its buttons (see gpui_mouse_hook_impl).
// The mouse only aims and attacks while it is actually being used: a pad player whose cursor merely
// rests over a raider must not have the mouse layer claim that raider -- see gamepad_ui_mouse_in_use.
static unsigned int gpui_real_mouse_time = 0;
#define GPUI_MOUSE_IN_USE_MS 4000

// NOTE: RB fired aimed shots until 2026-10-09 (a tap, then a held repeat). It is the HEAL now, set
// through gpui_heal_pending above; the aimed shot is the weapon slot's AIMED mode, fired by RT.

// RUN IS A HELD BUTTON AGAIN (X), not the toggle this layout briefly had: "lepší je držet běh".
// Read live from the pad rather than latched anywhere, because there is no gesture to arbitrate --
// X's only other job is a TAP (firing mode), and that is settled on the release edge in
// gpui_button_shortcuts by the same 220 ms rule the wheels use.
bool gamepad_ui_run_on()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return false;
    }
    return SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
}

bool gamepad_ui_attack_held()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return false;
    }

    // Its own latch, independent of the one gpui_aim_wheel keeps for the same axis: two
    // readers of one trigger each need their own hysteresis state, and sharing it would
    // make whichever ran second see an already-consumed edge.
    static bool latch = false;
    static unsigned int lowAt = GPUI_TRIGGER_NOT_LOW;
    return gpui_trigger_held(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, &latch, &lowAt);
}

// True while LT is pulled, for a caller OUTSIDE the world layer -- the barter window's split-stack
// gesture (LT + A).
//
// ⚠️ THIS EXISTS BECAUSE THE BARTER SCREEN WAS READING THE RAW AXIS. `SDL_GameControllerGetAxis(..)
// > 8000` looks harmless and is not: a physically HELD trigger intermittently reports a hard 0 --
// measured in trace_2026-08-30_234101, 14 of 33 release transitions read exactly 0 -- so roughly
// four times in ten the split was asked for and the code saw the trigger as released, and moved the
// WHOLE STACK instead of opening the quantity picker. That is his report: "Dal jsem pri tradu add
// one alt plus A ... a jen jeden se nepresune, ale celej stack."
//
// Its own latch pair, kept here rather than in inventry, so the barter gesture cannot steal or be
// stolen by the aim trigger's state -- the same rule every other gesture in this file follows.
// ⚠️ A MODIFIER IS NOT AN EDGE, AND THE SCHMITT LATCH IS THE WRONG TOOL FOR ONE. gpui_trigger_held
// is built to never MISS a pull: once latched it needs the axis to stay low for a continuous 60 ms,
// and ANY single high sample in that window resets the timer. That is right for "did he pull the
// trigger", and wrong here -- this axis is known to report garbage (a held trigger intermittently
// reads a hard 0, measured 14 of 33 samples, which is the same noise in the other direction). One
// spurious high after a genuine LT+A is enough to keep the latch armed, and from then on every
// plain A moves the WHOLE STACK: "furt to předává celej stack áčkem", reported three times.
//
// So the split modifier is filtered by AGREEMENT instead of by edges. Each read moves a small
// counter: a pull adds 2, a release takes 1, and the modifier is on at 3 of 6. A lone spurious high
// from rest reaches 2 and never arms it; a lone dropout while held costs 1 of 6 and never drops it;
// a real release walks it to 0 in six frames (~100 ms). Deliberately NOT applied to the aim/attack
// triggers -- those are edges on their own latches, they have their own history of breaking, and
// nothing about them is reported wrong.
#define GPUI_SPLIT_LEVEL_MAX 6
#define GPUI_SPLIT_LEVEL_ON 3

static int gpui_split_level = 0;

bool gamepad_ui_split_trigger_held()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        gpui_split_level = 0;
        return false;
    }

    int v = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    if (v > GPUI_AIM_TRIGGER_PRESS) {
        gpui_split_level += 2;
        if (gpui_split_level > GPUI_SPLIT_LEVEL_MAX) {
            gpui_split_level = GPUI_SPLIT_LEVEL_MAX;
        }
    } else if (v < GPUI_AIM_TRIGGER_RELEASE) {
        gpui_split_level -= 1;
        if (gpui_split_level < 0) {
            gpui_split_level = 0;
        }
    }

    return gpui_split_level >= GPUI_SPLIT_LEVEL_ON;
}

// Raw axis + filter state, for the barter diagnostic -- so a report of "A moved the whole stack"
// can be answered with what the trigger actually read rather than with a theory about it.
void gamepad_ui_split_trigger_debug(int* raw, int* level)
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    *raw = pad != NULL ? SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) : -1;
    *level = gpui_split_level;
}

bool gamepad_ui_aim_trigger_held()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return false;
    }

    // Own latch, like the attack trigger above: one reader, one piece of hysteresis state.
    static bool latch = false;
    static unsigned int lowAt = GPUI_TRIGGER_NOT_LOW;
    return gpui_trigger_held(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, &latch, &lowAt);
}

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

// THE RELOAD, for both LB (its release, gpui_button_shortcuts) and the keyboard's R. Why every way
// it can say no is audible and traced is explained at the LB call.
static void gpui_request_reload()
{
    if (combat_rt_player_ap() < 2) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "reload-refused", "why=ap ap=%d", combat_rt_player_ap());
    } else if (anim_get_reload_anim()) {
        // MODS "Reload animation": the weapon's handling loop plays first and the magazine
        // fills at its END (gpui_reload_anim_start). Asked here, done on the safe tick --
        // it registers an animation, which the hook must never do. Refused NOW if there is
        // nothing to load, so the buzz still answers the press it belongs to.
        //
        // A press while the reload is already running is NOT a new reload: it used to be
        // read as "the body is only fidgeting", cut the loop and start it over -- so an
        // impatient second tap made the reload take longer, and looked like it.
        if (combat_rt_is_reloading(obj_dude)) {
            // already on it
        } else if (intface_can_reload_weapon()) {
            gpui_reload_pending = true;
            gpui_reload_pending_since = get_time();
        } else {
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "reload-refused", "why=nothing-to-do");
        }
    } else if (intface_reload_weapon() == 0) {
        combat_rt_spend_ap(2);
        trace_event(TRACE_ACTION, "reload", "ap=%d", combat_rt_player_ap());
    } else {
        // intface_reload_weapon says no: no weapon in hand, it takes no ammo, there is
        // none in the pack, or the magazine is already full. It does not tell us which,
        // so neither do we -- but the player hears that nothing happened.
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "reload-refused", "why=nothing-to-do");
    }
}

// THE FIRING MODE (single -> AIMED -> burst), for LT+Y and the keyboard's F. A weapon with nothing
// to switch to buzzes.
static void gpui_cycle_fire_mode(const char* src)
{
    bool can = intface_item_can_toggle_state();
    if (can) {
        gsound_play_sfx_file("ib1p1xx1");
        intface_toggle_item_state();
    } else {
        gsound_play_sfx_file("iisxxxx1");
    }
    trace_event(TRACE_INPUT, "fire-mode", "src=%s ok=%d", src, can ? 1 : 0);
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
    static bool prevRb = false;
    static bool prevLb = false;

    bool startNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
    bool backNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    bool leftNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    bool rightNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
    bool rbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;

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

    // LB is the reload on its release (and the LB+B heal alias)
    // and RB heals on its press, so both are read and stamped HERE with the rest.
    // ⚠️ Every one of these reads belongs at the top, before the first early return: an edge that
    // is only computed on some paths reports a press that already happened as brand new.
    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool lbPressed = lbNow && !prevLb;
    bool lbReleased = !lbNow && prevLb; // tap = the reload, on its release; a hold = the LB quick slot
    prevLb = lbNow;
    static unsigned int lbDownTime = 0;
    if (lbPressed) {
        lbDownTime = get_time();
    }
    bool rbPressed = rbNow && !prevRb;
    bool rbReleased = !rbNow && prevRb;
    // NOTE: no hold timer for RB -- it is a single action on its press (the heal), nothing to split.

    // BACK is a tap-vs-hold now too: a TAP opens the inventory (as it always did) and a HOLD runs
    // to the nearest map exit, which was LB+RB before the modifier button disappeared.
    bool backReleased = !backNow && prevBack;
    bool startReleased = !startNow && prevStart;
    static unsigned int startDownTime = 0;
    static bool startHoldSpent = false;
    if (startPressed) {
        startDownTime = get_time();
        startHoldSpent = false;
    }
    static unsigned int backDownTime = 0;
    static bool backHoldSpent = false;
    if (backPressed) {
        backDownTime = get_time();
        backHoldSpent = false;
    }
    prevStart = startNow;
    prevBack = backNow;
    prevX = xNow;
    prevB = bNow;
    prevY = yNow;
    prevUp = upNow;
    prevDown = downNow;
    prevLeft = leftNow;
    prevRb = rbNow;

    // A FRESH PRESS STARTS CLEAN. Y acts on its RELEASE and LT+Y spends it, so a flag left standing
    // by a release this function never saw (it came during a dialogue or a menu, where it returns
    // early) would otherwise eat the next hand swap -- and now also the next weapon-wheel hold, which
    // checks it. Cleared here, BEFORE the screen-epoch block below, which re-spends a button still
    // held when a screen closes.
    if (yPressed) {
        gpui_consumed_y = false;
    }

    // ⚠️ A SCREEN JUST RAN ITS OWN INPUT LOOP (inventory, pause menu, Pip-Boy, a confirmation
    // box, load/save). Same counter and the same reasoning as gpui_action_buttons: these statics
    // outlive every modal screen, so the edges derived above describe the world BEFORE it.
    //
    // ⚠️ STAMPING THE LIVE STATE IS NOT ENOUGH HERE, and that is the whole reason this needs its
    // own block. B, X and Y act on the RELEASE edge (a hold opens their wheel), so the B that
    // closed the inventory is still physically DOWN when we get here: prevB=true is correct, and
    // its release then arrives as an ordinary bReleased whose bDownTime was taken INSIDE the
    // inventory -- comfortably under the tap threshold -- so it cycled the weapon's firing mode
    // ("when I close the inventory with B, my firing mode switches"). Mark every held release-edge
    // button SPENT, exactly the way each wheel already marks the press that opened it.
    //
    // gpui_dpad_* is deliberately NOT reset: a direction held through a D-pad-opened screen must
    // stay spent until it is genuinely released, or the screen reopens the instant it closes.
    static unsigned int lastShortcutEpoch = 0;
    unsigned int shortcutEpoch = gamepad_ui_input_epoch();
    if (shortcutEpoch != lastShortcutEpoch) {
        lastShortcutEpoch = shortcutEpoch;
        if (bNow) {
            gpui_consumed_b = true;
        }
        if (xNow) {
            gpui_consumed_x = true;
        }
        if (lbNow) {
            gpui_consumed_lb = true;
        }
        if (rbNow) {
            gpui_consumed_rb = true;
        }
        if (backNow) {
            backHoldSpent = true;
        }
        if (startNow) {
            startHoldSpent = true;
        }
        if (yNow) {
            gpui_consumed_y = true;
        }
        return;
    }

    // Every state below that RETURNS before the D-pad arbitration must also forget a hold in
    // progress, or the direction still held when (say) a conversation starts would look like a
    // fresh release once gameplay resumed and fire a phantom action-cycle. Mirrors the guards
    // below deliberately -- one place to keep in step beats six scattered resets.
    //
    // ⚠️ gpui_in_dpad_screen is EXCLUDED, and that exclusion is the whole point of the state. The
    // hold that opened the Pip-Boy is still physically down while the Pip-Boy is up; forgetting it
    // would restart the timer the moment the screen closed and immediately reopen it. Keeping
    // gpui_dpad_handled set means that direction stays spent until it is genuinely released.
    if (in_main_menu || gpui_no_session || (!gpui_in_dpad_screen
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
    // gpui_no_session covers the death scene, which is the same situation one screen later:
    // main_reset_system() has already run by then, so anything these shortcuts open would be
    // operating on a torn-down game.
    if (in_main_menu || gpui_no_session) {
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
        // Pressing the direction that OPENED the screen closes it again -- a toggle, which is the
        // right feel for a screen you only look at.
        //
        // ⚠️ NOT THE CHARACTER SHEET. That one grew real navigation, where Down means "next row",
        // so the toggle was eating the most-used direction on the screen: every step down the skill
        // list also closed it. B / BACK still back out of it, like everywhere else.
        bool sameDirection = (gpui_dpad_screen_open == GPUI_DPAD_PIPBOY && upPressed)
            || (gpui_dpad_screen_open == GPUI_DPAD_AUTOMAP && leftPressed);
        if (bPressed || backPressed || sameDirection) {
            // ⚠️ SPEND THE PRESS THAT CLOSES, or its RELEASE reopens the screen. BACK and
            // START open on the release edge now (they grew a hold -- exit-run and quick
            // save), so a close on the press edge bounced straight back: ESC shut the
            // window and the release of that same press walked into the tap handler.
            // "po opětovném stisku se zase otevře a otevře".
            if (backPressed) {
                backHoldSpent = true;
            }
            GNW_add_input_buffer(KEY_ESCAPE);
        }
        return;
    }

    // Inventory open: BACK toggles it shut; X/B equip the item under the cursor.
    if (inven_is_open()) {
        if (backPressed) {
            backHoldSpent = true;
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
            startHoldSpent = true;
            GNW_add_input_buffer(KEY_ESCAPE);
        }
        return;
    }

    // Neither shortcut menu open: don't stack onto another modal, and require an
    // interactive interface bar. Also bail during a conversation: the interface bar
    // stays "enabled" through a talking-head, but map bk processes are disabled, so a
    // weapon swap (intface_toggle_items -> intface_change_fid_animate) would block
    // forever waiting on a put-away/take-out animation that can never advance = hang.
    // ⚠️ AND NOT WHILE A SCREEN DRIVES ITS OWN PAD (gpui_self_nav). This layer is GAMEPLAY: D-pad
    // holds open the Pip-Boy / character sheet / automap, LB+RB runs for the exit, and so on. On a
    // screen that reads the pad itself those are not shortcuts, they are that screen's own keys --
    // on the WORLD MAP, holding D-pad up to walk down the town list opened the Pip-Boy instead
    // ("když při travellingu držím Dpadem nahoru a dolu tak se zapíná pipboy"), and the buttons it
    // did not recognise buzzed at him. Same leak as the main menu once had, one screen further on.
    if (gamepad_ui_menu_active() || dialog_active() || !intface_is_enabled() || gpui_self_nav) {
        return;
    }

    // BACK and START ARE TAP-vs-HOLD NOW, because the LB modifier they shared their combos with is
    // gone: BACK tap = inventory, BACK hold = run to the nearest map exit (was LB+RB), START tap =
    // pause menu, START hold = QUICK SAVE (was LB+Y).
    //
    // ⚠️ QUICK SAVE HAD TO GO SOMEWHERE, and BACK+START is not it: that combo was abandoned once
    // because Steam Big Picture and the Xbox Game Bar grab BACK, and nothing about that changed.
    // A START hold is a single button this layer sees whole.
    //
    // ⚠️ A LONGER THRESHOLD THAN THE WHEELS (450 ms, not 220). These two are pressed deliberately
    // rather than drummed, and a 220 ms bar turns an ordinary firm press on the inventory button
    // into a sprint for the exit. The hold fires the INSTANT it elapses instead of waiting for the
    // release, so the action is felt while the thumb is still down and a long press cannot be
    // mistaken for a stuck button.
    if (backNow && !backHoldSpent && get_time() - backDownTime >= GPUI_MENU_HOLD_MS) {
        backHoldSpent = true;
        gsound_play_sfx_file("ib1p1xx1");
        gpui_exit_nav_pending = true;
    }
    if (startNow && !startHoldSpent && get_time() - startDownTime >= GPUI_MENU_HOLD_MS) {
        startHoldSpent = true;
        gsound_play_sfx_file("ib1p1xx1");
        QuickSaveGame();
    }
    if (backReleased && !backHoldSpent) {
        gsound_play_sfx_file("ib1p1xx1");
        handle_inventory();
        return;
    }
    if (startReleased && !startHoldSpent) {
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
            //
            // ⚠️ WITH NOTHING LIT UP, UP / DOWN SCROLL THE MESSAGES (2026-10-14, "když dude nebude na nic
            // mířit, tak dpad bude posouvat pipboye; když bude mířit na prompt, tak šipky prohodí akce;
            // podržení jak je teď"). The message window on the bar keeps a history the pad could never
            // reach -- vanilla scrolls it only by clicking its upper or lower half. Up shows the older
            // line, down the newer one, exactly those two halves. Left / right have nothing to do there
            // and still refuse audibly. A target, even a one-action one, keeps the taps for itself.
            if (gpui_dpad_held != -1 && !gpui_dpad_handled) {
                bool back = gpui_dpad_held == GPUI_DPAD_PIPBOY || gpui_dpad_held == GPUI_DPAD_AUTOMAP;
                bool vertical = gpui_dpad_held == GPUI_DPAD_PIPBOY || gpui_dpad_held == GPUI_DPAD_CHAR;
                if (interact_prompt_cycle(back ? -1 : 1)) {
                    gsound_play_sfx_file("ib1p1xx1");
                } else if (interact_prompt_target() == NULL && vertical) {
                    if (gpui_dpad_held == GPUI_DPAD_PIPBOY) {
                        display_scroll_up(-1, -1);
                    } else {
                        display_scroll_down(-1, -1);
                    }
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

    // The 2026-09-16 LAYOUT -- AND THE MODIFIER BUTTON IS GONE. LB+B (heal), LB+Y (quick save)
    // and LB+RB (run to the exit) all died with it, because LB is a wheel button now and a button
    // that opens a radial when held cannot also be a modifier. Every action that lived on a combo
    // got its own tap instead, which is what he asked for -- one gesture, one meaning:
    //
    //   X  tap = RUN on/off   hold = weapon wheel        (run was LB-held, weapons were Y)
    //   Y  tap = heal         hold = drug wheel          (heal was LB+B, drugs were B)
    //   B  tap = cancel       hold = firing mode         (fire mode was the B tap)
    //   LB tap = reload       hold = skill wheel         (reload was X, skills were L3)
    //   RB tap = last aimed shot again   hold = ammo wheel   (ammo was X)
    //
    // Quick save is unaffected: BACK+START already did it. Running to the nearest exit moved to a
    // BACK hold (see the inventory branch above).

    // ⚠️ X IS RUN AND ONLY RUN. The firing mode was on its TAP for exactly one session, and the
    // 220 ms split I called an honest small cost turned out to be nothing of the kind: every short
    // dash -- a step through a doorway, a dodge sideways -- came back under the threshold and
    // silently switched the weapon. With a SPEAR that is not a cosmetic flip, it is thrust becoming
    // THROW: "při běhu občas překliknu režim střelby a místo útoku oštěpem ho hodím", i.e. the
    // weapon leaves your hands because you moved too briefly.
    //
    // A button you hold constantly must not carry an action on its release. The firing mode went
    // to LB+X after that, then LB+Y, then RT+Y for ten minutes, and since 2026-10-10 it is LT+Y (below).

    // ⚠️ LT + Y = FIRING MODE (2026-10-10). The fifth home in two days: RB+Y (would have pushed the
    // heal onto RB's release), LB+Y (*"lb je reload, ne? tak LB a Y"*), RT+Y for ten minutes -- he had
    // written RT and meant LT: *"já myslel LT ne RT"* -- and now LT+Y. With LB declared reload and
    // nothing else, the mode goes on the AIM trigger: hold LT, tap Y, and the shots are aimed /
    // single / burst. Cycles the
    // active weapon the way the keyboard N does: single -> AIMED -> burst
    // (intface_item_action_available). A weapon with nothing to switch to buzzes.
    //
    // The Y is SPENT: no hand swap on its release, no weapon wheel on its hold (gpui_consumed_y,
    // checked in gpui_weapon_wheel). LT is read through the same Schmitt helper the lock reads
    // (gamepad_ui_aim_trigger_held), so "held" here means exactly what "aiming" means there.
    if (yPressed && gamepad_ui_aim_trigger_held() && !radial_wheel_is_open()) {
        gpui_consumed_y = true;
        gpui_cycle_fire_mode("lt+y");
    }
    if (xReleased) {
        // ⚠️ X TAP = RELOAD (2026-10-17, his pick; off LB, which is a plain quick slot now -- "nech mě
        // používat tools ... na LB to není dobré"). The hold is the run (gamepad_ui_run_on reads X
        // live), so only a press shorter than the hold line reloads -- the same tap/hold split as B
        // and Y. The reload itself is unchanged: gpui_request_reload, its AP, its buzz, its trace.
        if (!gpui_consumed_x && get_time() - xDownTime < GPUI_QUICK_HOLD_MS) {
            gpui_request_reload();
        }
        gpui_consumed_x = false;
    }

    // ⚠️ LB: TAP RELOADS, HOLD AIMS. Reload has now been on X, on LB, on LT and back on LB inside
    // one session -- this is where he put it, and LB earns it: the reload and the body-part picker
    // are the two things you reach for with the same finger in the same fight.
    //
    // LT is not involved at all any more: it is the aim/lock (combat_rt), the same job R3 does.
    // THE LB QUICK SLOT (2026-10-15, see gpui_consumed_lb): held past the tap line, LB stops being the
    // reload and becomes the slot -- a throwable is drawn now and thrown on the release, a drug is
    // taken on the release.
    // ⚠️ LB IS A PLAIN QUICK SLOT NOW, PRESS AND RELEASE EXACTLY LIKE RB (2026-10-17, "nech mě používat
    // tools jako ten přístroj, který ukazuje živý na mapě ... chci prostě kliknout LB nebo RB"). The
    // reload it used to carry on a tap went to X's tap; what is in the slot -- a stimpak by default,
    // a drug, a TOOL (motion sensor, Geiger counter, Stealth Boy, flare...) or a throwable -- is used
    // on the press, a throwable drawn on the press and thrown on the release.
    if (lbPressed && !gpui_consumed_lb && !radial_wheel_is_open() && !inven_any_open()) {
        trace_event(TRACE_INPUT, "quick-slot-press", "src=lb");
        gpui_qs_press(GPUI_QS_LB, false);
    }
    if (lbReleased) {
        if (gpui_consumed_lb) {
            // This press was spent: a weapon-wheel page, or a screen that closed on it.
            gpui_consumed_lb = false;
        } else {
            gpui_qs_release(GPUI_QS_LB);
        }
    }

    // ⚠️ RB = THE HEAL (2026-10-09). LB+B never sat right in his hands, and a heal is
    // the one action that must work first time while you are being shot: *"jako quick heal to stojí
    // za hovno, to musí být quick heal ne tam půl hodiny štelovat jestli to neposeru"*. RB came free
    // because the aimed shot it used to repeat is the weapon slot's AIMED mode again (his idea, the
    // original's own UI with its price on it), fired by RT.
    //
    // ON THE PRESS: RB carries nothing else, so there is nothing to wait for (for ten minutes it was
    // the first half of RB+Y and healed on the release; the firing mode moved off RB so the heal could
    // stay instant). NOT while a radial wheel is up -- there RB turns its page -- and DEFERRED to
    // the safe tick like every other item use (gpui_use_best_heal). LB+B still heals too; it costs
    // nothing to leave.
    //
    // ⚠️ RB IS THE RB QUICK SLOT NOW (2026-10-15), whose own pick is that same best heal: a drug in it
    // is taken on the press as before; a throwable bound to it is drawn on the press and thrown on the
    // release, like LB's.
    if (rbPressed && !radial_wheel_is_open() && !inven_any_open()) {
        trace_event(TRACE_INPUT, "quick-slot-press", "src=rb");
        gpui_qs_press(GPUI_QS_RB, false);
    }
    if (rbReleased) {
        gpui_consumed_rb = false;
        gpui_qs_release(GPUI_QS_RB);
    }

    // B: TAP CANCELS, HOLD OPENS THE DRUG WHEEL. The firing mode that briefly lived on this hold
    // moved to X's tap when the wheels were shuffled -- B is "back out of this" everywhere on the
    // pad, and the drugs are back where they were originally built.
    //
    // Deliberately NOT part of a press-edge chain: this is a release edge, so it can legitimately
    // land on the same frame as one, and chaining them would strand the consumed flag set and eat
    // the NEXT B tap.
    if (bReleased && get_time() - bDownTime < GPUI_QUICK_HOLD_MS) {
        if (gpui_aim_swallow_b) {
            // This press already did its job: it cancelled the aim picker.
            gpui_aim_swallow_b = false;
        } else if (gpui_consumed_b) {
            // The press opened (and released out of) the drug wheel.
            gpui_consumed_b = false;
        } else if (gpui_skill_cursor_armed()) {
            // PUT THE SKILL AWAY. A skill armed from the wheel survives being used, so it needs an
            // explicit way out or you would be stuck aiming a lockpick at the world.
            gsound_play_sfx_file("ib1p1xx1");
            gpui_cancel_skill_cursor();
        } else if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_USE_CROSSHAIR) {
            // Same idea for the use-item-on cursor that holding A arms.
            gsound_play_sfx_file("ib1p1xx1");
            gmouse_set_cursor(MOUSE_CURSOR_NONE);
            gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
        } else if (combat_rt_cancel_player_action()) {
            // STOP. A on something across the room registers a walk-there-then-act sequence, and
            // until this existed nothing could call it off -- "I clicked A and the character walked
            // far, I wanted to cancel and couldn't".
            gsound_play_sfx_file("ib1p1xx1");
        }
    } else if (bReleased) {
        // A HOLD that the wheel did not claim (it was blocked, or nothing was aimed at): swallow
        // the flags it would otherwise leave standing, and cancel nothing -- a hold is not a tap.
        gpui_aim_swallow_b = false;
        gpui_consumed_b = false;
    }

    // Y: TAP SWAPS THE ACTIVE HAND, hold opens the weapon wheel. The swap came back on his word --
    // it is the one thing the wheel cannot do as fast, because swapping is a single flick between
    // two guns you have already chosen, not a choice among everything you carry.
    if (yReleased) {
        if (gpui_consumed_y) {
            // Already spent -- this press opened the weapon wheel.
            gpui_consumed_y = false;
        } else if (get_time() - yDownTime < GPUI_QUICK_HOLD_MS) {
            intface_toggle_items(true);
        }
    }

    // ⚠️ LB + B = HEAL, a combo again, and the ONE place in this layout where a modifier survived.
    // LB's own two jobs are a tap (reload, on the release) and a hold (the called-shot window, once
    // it has been down GPUI_QUICK_HOLD_MS).
    //
    // ⚠️ THE HOLD AND THE HEAL COLLIDED AGAIN, AND B IS WHY. In the on-body picker era B did nothing
    // inside the picker, so a held LB could carry both. Since 2026-09-24 the hold opens the VANILLA
    // called-shot window, where B CANCELS -- so holding LB as the heal modifier and mashing B opened
    // the window 220 ms in and the next B shut it instead of healing (trace_2026-10-05_001014: LB
    // down 36.36, heal 36.48, window 36.63, cancelled; again at 39.61). "Oprav i to okno mířené
    // střely při LB+B." So a heal now SPENDS the LB press (gpui_consumed_lb): this hold opens no
    // window and its release reloads nothing. A hold that has not carried a B by the 220 ms mark is
    // still the called shot -- the gesture cannot know a B is coming.
    //
    // Sits on the PRESS edge: the modifier already makes the gesture unambiguous (the drug wheel
    // refuses to open while LB is held), so there is nothing to disambiguate and no reason to make
    // a heal wait for a release. Marks the press spent so the release neither cancels anything nor
    // opens the wheel. DEFERRED to the safe tick like every other item use.
    if (bPressed && (gpui_qs_drawn(GPUI_QS_LB) || gpui_qs_drawn(GPUI_QS_RB))) {
        // A quick-slot throwable is out: B puts it away (B is cancel everywhere on the pad).
        gpui_consumed_b = true;
        if (lbNow) {
            gpui_consumed_lb = true;
        }
        gpui_qs_cancel();
    }
    // (The LB+B heal alias is gone, 2026-10-17: LB's own press takes the slot's stimpak now, and the
    // B under it would have taken a second one.)
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

// UNLOAD gets a DRAWN glyph, not an item picture: a single stroke running from the top left down
// to the bottom right. It used to borrow the art of whatever ammo was in the gun, which made it
// look exactly like the reload entries sitting next to it -- the one cell on the wheel that does
// the opposite of its neighbours was the one that looked most like them.
//
// Built once into a static buffer and handed over as a plain indexed bitmap, the same thing
// art_lock hands back. Index 0 is transparent to the wheel's blitter, so only the stroke shows,
// and there is nothing to unlock afterwards.
#define GPUI_UNLOAD_ICON_W 56
#define GPUI_UNLOAD_ICON_H 40

static unsigned char gpui_unload_icon[GPUI_UNLOAD_ICON_W * GPUI_UNLOAD_ICON_H];
static bool gpui_unload_icon_built = false;

static void gpui_icon_unload_glyph(RadialSegment* segs, int index)
{
    if (index < 0 || index >= RADIAL_WHEEL_MAX) {
        return;
    }

    if (!gpui_unload_icon_built) {
        memset(gpui_unload_icon, 0, sizeof(gpui_unload_icon));

        // Straight line, corner to corner, inset so the stroke never touches the cell box the
        // wheel draws around it. Walked along x with y interpolated, then thickened vertically --
        // the slope here is shallower than 1, so stepping x leaves no gaps.
        const int x0 = 6;
        const int y0 = 7;
        const int x1 = GPUI_UNLOAD_ICON_W - 7;
        const int y1 = GPUI_UNLOAD_ICON_H - 8;
        const int thickness = 3;

        unsigned char ink = colorTable[32767];
        for (int x = x0; x <= x1; x++) {
            int y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
            for (int t = 0; t < thickness; t++) {
                int yy = y + t;
                if (yy >= 0 && yy < GPUI_UNLOAD_ICON_H) {
                    gpui_unload_icon[yy * GPUI_UNLOAD_ICON_W + x] = ink;
                }
            }
        }

        gpui_unload_icon_built = true;
    }

    segs[index].icon = gpui_unload_icon;
    segs[index].iconWidth = GPUI_UNLOAD_ICON_W;
    segs[index].iconHeight = GPUI_UNLOAD_ICON_H;
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

// True while ANY armed targeting cursor is up: one of the seven skill cursors, or the use-item-on
// crosshair that holding A arms from the active hand.
//
// ⚠️ THE RIGHT STICK MUST DRIVE THE CURSOR IN THESE MODES. The interact-prompt picker otherwise
// claims the stick outright whenever two or more interactables are in reach ("the picker OWNS the
// stick while it is available"), so arming a cursor and then pushing the stick just walked the
// prompt's outline from object to object -- the user's report was that he armed the rope and then
// "the stick doesn't move the cursor, it scouts what is around me". The gesture these modes exist
// for is aim-then-click; without the cursor there is nothing to aim.
//
// This very likely also explains the long-open "do the seven skill cursors actually apply?" question
// from the skill wheel: same stick, same picker, same silent theft.
static bool gpui_targeting_cursor_armed()
{
    return gmouse_3d_get_mode() == GAME_MOUSE_MODE_USE_CROSSHAIR || gpui_skill_cursor_armed();
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

// Move the pointer to (sx, sy) and REDRAW it there.
//
// ⚠️ mouse_set_position ALONE ONLY MOVES THE COORDINATES (mouse.cc:744) -- it neither erases the old
// cursor sprite nor blits the new one, so a bare call leaves a ghost behind. This is the same dance
// barter_warp_cursor does: refresh what was under the old rect, then show at the new spot. Clamped
// into the world window, because a cursor parked under the interface bar or off screen is exactly
// the "where did it go" this whole idea exists to end.
static void gpui_park_cursor_at(int sx, int sy)
{
    Rect world;
    if (display_win != -1 && win_get_rect(display_win, &world) == 0) {
        if (sx < world.ulx) {
            sx = world.ulx;
        } else if (sx > world.lrx) {
            sx = world.lrx;
        }
        if (sy < world.uly) {
            sy = world.uly;
        } else if (sy > world.lry) {
            sy = world.lry;
        }
    }

    int curX;
    int curY;
    mouse_get_position(&curX, &curY);
    if (sx == curX && sy == curY) {
        return;
    }

    Rect oldRect;
    mouse_get_rect(&oldRect);
    mouse_set_position(sx, sy);
    win_refresh_all(&oldRect);
    mouse_show();

    // The pointer is hidden by default with a pad connected; having just moved it ON PURPOSE is
    // exactly the moment it has to be visible (and a click at a hidden cursor is dropped anyway).
    gpui_cursor_show_time = get_time();
}

// --- THE RIGHT STICK STEPS AN ARMED CURSOR BETWEEN OBJECTS (2026-10-08) ------------------------
//
// *"Nemohl jsem nasměrovat provaz správně, furt to aimoval něco jiného, musíme to nějak pojistit."*
// With the rope (or a skill) armed, the stick drove a free pointer and the click went to whatever
// was drawn topmost under it (object_under_mouse) -- trace_2026-10-08_213405 s=247-261: nine A
// presses, nine uses, none on what he meant. Now a push picks the use target lying that way from the
// current one (interact_prompt_use_targets: the dude, critters, scripted / interactable things) and
// GLUES the crosshair onto it, and the click goes to THAT object, not to the pixel
// (gamepad_ui_targeting_pick, read by gmouse). Scored like the enemy lock -- the anchor is what was
// picked when the push began, distance plus angle off the push -- so one flick is one step. Where
// nothing lies that way the stick still moves the pointer freely, and a pointer that has left the
// glued spot drops the pick, so the old aiming is never taken away -- only made exact.
#define GPUI_TPICK_MAX 48
#define GPUI_TPICK_CONE_COS 0.4f // ~66 degrees either side of the push
#define GPUI_TPICK_DEG_PER_TILE 30.0f
#define GPUI_TPICK_SWITCH_MARGIN 0.75f

static Object* gpui_tpick = NULL; // compare-only until re-found in a live scan (objects get freed)
static int gpui_tpick_x = -32000; // where the crosshair was glued for it
static int gpui_tpick_y = -32000;
static bool gpui_tpick_pushing = false;
static Object* gpui_tpick_anchor = NULL;
static int gpui_tpick_anchor_x = 0;
static int gpui_tpick_anchor_y = 0;

static void gpui_tpick_centre(Object* obj, int* x, int* y)
{
    Rect b;
    obj_bound(obj, &b);
    *x = (b.ulx + b.lrx) / 2;
    *y = (b.uly + b.lry) / 2;
}

// The current pick, if it is still a live use target on screen; NULL otherwise.
static Object* gpui_tpick_live()
{
    if (gpui_tpick == NULL) {
        return NULL;
    }
    Object* list[GPUI_TPICK_MAX];
    int n = interact_prompt_use_targets(list, GPUI_TPICK_MAX);
    for (int i = 0; i < n; i++) {
        if (list[i] == gpui_tpick) {
            return gpui_tpick;
        }
    }
    gpui_tpick = NULL;
    return NULL;
}

static void gpui_tpick_set(Object* obj)
{
    int x;
    int y;
    gpui_tpick_centre(obj, &x, &y);
    gpui_park_cursor_at(x, y);
    gpui_tpick = obj;
    // Where the pointer really ended up (gpui_park_cursor_at clamps into the world window).
    mouse_get_position(&gpui_tpick_x, &gpui_tpick_y);
}

static void gpui_tpick_reset()
{
    gpui_tpick = NULL;
    gpui_tpick_pushing = false;
    gpui_tpick_anchor = NULL;
}

// Read by gmouse when an armed cursor is clicked: the object to use it on, or NULL to let the pixel
// decide as it always did. Only while the pointer is still exactly where it was glued -- a real
// mouse or the free stick moving it hands the choice straight back to the pixel.
Object* gamepad_ui_targeting_pick()
{
    if (!gpui_targeting_cursor_armed()) {
        return NULL;
    }
    int mx;
    int my;
    mouse_get_position(&mx, &my);
    if (mx != gpui_tpick_x || my != gpui_tpick_y) {
        return NULL;
    }
    return gpui_tpick_live();
}

// One frame of the right stick while a cursor is armed. True = the stick is taken (glued or just
// stepped), false = let it move the pointer freely.
static bool gpui_tpick_stick(float x, float y, float mag)
{
    if (mag <= gpui_stick_deadzone) {
        gpui_tpick_pushing = false;
        gpui_tpick_anchor = NULL;

        // Follow a target that walks: keep the crosshair on it while the stick rests.
        Object* live = gpui_tpick_live();
        if (live != NULL && gamepad_ui_targeting_pick() == live) {
            int cx;
            int cy;
            gpui_tpick_centre(live, &cx, &cy);
            int mx;
            int my;
            mouse_get_position(&mx, &my);
            if (abs(cx - mx) > 2 || abs(cy - my) > 2) {
                gpui_tpick_set(live);
            }
        }
        return false;
    }

    Object* list[GPUI_TPICK_MAX];
    int n = interact_prompt_use_targets(list, GPUI_TPICK_MAX);
    if (n == 0) {
        return false;
    }

    // The anchor holds for the whole push: what was picked when it began, else the pointer itself.
    if (!gpui_tpick_pushing) {
        gpui_tpick_pushing = true;
        Object* live = gamepad_ui_targeting_pick();
        gpui_tpick_anchor = live;
        if (live != NULL) {
            gpui_tpick_centre(live, &gpui_tpick_anchor_x, &gpui_tpick_anchor_y);
        } else {
            mouse_get_position(&gpui_tpick_anchor_x, &gpui_tpick_anchor_y);
        }
    }

    bool glued = gamepad_ui_targeting_pick() != NULL;
    if (mag < GPUI_IP_PICK_THRESHOLD) {
        return glued; // a gentle push neither steps nor creeps a glued crosshair off its target
    }

    float sx = x / mag;
    float sy = y / mag;
    Object* best = NULL;
    float bestScore = 0.0f;
    float currentScore = 0.0f;
    bool haveCurrent = false;

    for (int i = 0; i < n; i++) {
        Object* c = list[i];
        if (c == gpui_tpick_anchor) {
            continue; // pushing away from it means someone else
        }
        int cx;
        int cy;
        gpui_tpick_centre(c, &cx, &cy);
        float ex = (float)(cx - gpui_tpick_anchor_x);
        float ey = (float)(cy - gpui_tpick_anchor_y);
        float elen = sqrtf(ex * ex + ey * ey);
        if (elen < 1.0f) {
            continue;
        }
        float dot = (ex * sx + ey * sy) / elen;
        if (dot < GPUI_TPICK_CONE_COS) {
            continue;
        }
        float clamped = dot > 1.0f ? 1.0f : dot;
        float offDeg = acosf(clamped) * (180.0f / 3.14159265f);
        // Screen pixels over 32 is roughly tiles -- the anchor may be a bare pointer with no tile.
        float score = elen / 32.0f + offDeg / GPUI_TPICK_DEG_PER_TILE; // lower is better

        if (c == gpui_tpick && glued) {
            currentScore = score;
            haveCurrent = true;
        }
        if (best == NULL || score < bestScore) {
            best = c;
            bestScore = score;
        }
    }

    if (best == NULL) {
        // Nothing that way: the pointer moves freely, which also unglues it -- the target he wants
        // may simply not be on the list, and the stick must still be able to reach it.
        return false;
    }

    if (best != gpui_tpick || !glued) {
        if (haveCurrent && currentScore - bestScore < GPUI_TPICK_SWITCH_MARGIN) {
            return true;
        }
        gpui_tpick_set(best);
        gsound_play_sfx_file("ib1p1xx1");
        trace_event(TRACE_INPUT, "tpick", "pid=%d id=%d name=%s tile=%d dist=%d",
            best->pid, best->id, object_name(best), best->tile, obj_dist(obj_dude, best));
    }
    return true;
}

// --- WHERE AN ARMED SKILL CURSOR STARTS ---------------------------------------------------------
//
// His request: first aid arms on the DUDE, the lockpick arms on the locked chest, and so on for the
// rest. The skill cursor used to appear whereever the pointer was left, which out in the world is
// nowhere -- it is hidden and has not moved for minutes -- so every use of the wheel started with a
// hunt for the crosshair.
//
// First aid and Doctor are answered here, because the answer is not on the map: it is the dude, and
// healing yourself is what you reach for the wheel for. Everything else asks interact_prompt, which
// owns the map scan and all of its traps (hidden staging, proto-less objects).
static void gpui_skill_park_cursor(int skill)
{
    if (obj_dude == NULL) {
        return;
    }

    Object* target = (skill == SKILL_FIRST_AID || skill == SKILL_DOCTOR)
        ? obj_dude
        : interact_prompt_skill_target(skill);

    // Nothing sensible in reach: leave the cursor exactly where it is. A crosshair yanked onto
    // something absurd is worse than one that did not move.
    if (target == NULL) {
        return;
    }

    // Parked AND picked: A applies it to exactly this object, not to whatever pixel is topmost there.
    gpui_tpick_set(target);
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
        // ...and put the crosshair on whatever this skill is obviously for.
        gpui_skill_park_cursor(skill);
    }
}

// --- The eight hit locations, and who owns the shared wheel overlay. ---
//
// The order below is CLOCKWISE FROM THE TOP and is load-bearing twice over: the wheels lay their
// segments out in it, and gpui_aim_ring_slot maps stick directions onto it, so a given push means
// the same body part wherever it is read.

// The radial_wheel is a single shared overlay, so only one wheel may own it at a time. This tracks
// which driver opened the current overlay so the others ignore it.
enum {
    GPUI_WHEEL_NONE = 0,
    GPUI_WHEEL_SKILL,
    GPUI_WHEEL_QUICK,
    GPUI_WHEEL_WEAPON,
    GPUI_WHEEL_AMMO,
};
static int gpui_wheel_owner = GPUI_WHEEL_NONE;


// Set for one tick by gamepad_ui_tick when A goes down; read by every wheel as "select".
static bool gpui_wheel_a_edge = false;

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

// ⛔ THERE IS NO AIM WHEEL, AND THAT IS A DECISION. Picking the body part in a radial panel was
// built on 2026-09-02 and rejected on sight -- the part is chosen ON THE TARGET'S BODY (the marks
// further down this file), because that is the feature. What was actually broken was the stick
// mapping, not the display; see gpui_aim_ring_slot. Do not bring the panel back.

// Drive the wheel from the per-frame safe tick (gamepad_ui_tick). Open on the L3 press
// edge, aim while held, execute + close on release.
// How long the right stick must rest in the centre before letting go counts as a pick: longer than
// the frame or two a deliberate swing through the middle spends there, short enough not to be felt.
#define GPUI_SKILL_RELEASE_MS 70

static void gpui_skill_wheel(SDL_GameController* pad)
{
    // ⚠️ BACK ON L3, WHERE IT LIVED FOR MONTHS. It spent about an hour on an LB hold in this
    // layout, until LB was claimed by the body-part picker; L3 was left free by that same move, so
    // the skills go home rather than squatting on a third button. No tap/hold split here: nothing
    // else wants the stick click, so the wheel opens on the PRESS edge, aims while held, and
    // executes on release -- the snappiest of the four, and the one that needs to be.
    //
    // It aims with the RIGHT stick: the left thumb is on the left stick, so the right one is free.
    //
    // ⚠️ IT STAYS OPEN (2026-10-10): *"stisk R3 menu zůstane, stisk L3 menu nezůstane, změň to na R3"*.
    // The wheel used to close and pick on the RELEASE of L3, so it had to be held -- a stick click
    // held down while the other thumb aims. Now it behaves like the R3 called-shot window: a press
    // opens it and it stays; the right stick aims, A or another L3 press takes the highlighted
    // skill, B closes it without one.
    //
    // ⚠️ ...AND LETTING GO OF THE RIGHT STICK ON A SKILL TAKES IT (2026-10-14, "kliknu L3, nasměruji
    // skill a když pustím na tom skillu, tak se ten skill vybere"). The stick springs back to the
    // centre, which clears the highlight, so the pick is the skill that WAS highlighted the moment
    // it went -- confirmed only after the stick has stayed centred for GPUI_SKILL_RELEASE_MS, so
    // swinging from one side of the ring to the other through the middle does not pick on the way.
    // A, L3 and B keep their jobs.
    //
    // ⚠️ AIMED WITH THE LEFT STICK, THE ONE YOU JUST CLICKED (2026-10-17, "L3 se musí vymyslet jiné
    // vybírání na skills"; his pick of three: one thumb). Click L3 and tilt that same stick at the
    // skill; letting it spring back picks it, as before. The right thumb is free. Two things make the
    // walk stick safe to aim with, both already in place for the Y and B wheels: the wheel opens
    // DISARMED (radial_wheel_update ignores the stick until it has been back in the centre once, so a
    // click made while running does not pick the skill you were running towards -- see
    // [[wheel-aim-arming]]), and combat_rt stops the dude's feet while any wheel is open.
    static bool prevL3 = false;
    static bool prevB = false;
    static int releasedOn = -1;                // skill highlighted when the stick went to centre
    static unsigned int releasedSince = 0;
    bool l3Now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
    bool l3Edge = l3Now && !prevL3;
    prevL3 = l3Now;
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool bEdge = bNow && !prevB;
    prevB = bNow;

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
        } else if (bEdge) {
            // Closed without a pick.
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (l3Edge || gpui_wheel_a_edge) {
            int skill = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            releasedOn = -1;
            if (skill != -1) {
                gpui_use_skill(skill);
            }
        } else {
            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / GPUI_AXIS_MAX;
            int before = radial_wheel_highlight_id();
            radial_wheel_update(x, y);
            int now = radial_wheel_highlight_id();

            if (now != -1) {
                releasedOn = -1; // aiming (again): nothing pending
            } else if (before != -1) {
                releasedOn = before; // just went to the centre from this skill
                releasedSince = get_time();
            } else if (releasedOn != -1 && get_time() - releasedSince >= GPUI_SKILL_RELEASE_MS) {
                int skill = releasedOn;
                releasedOn = -1;
                radial_wheel_close();
                gpui_wheel_owner = GPUI_WHEEL_NONE;
                gpui_icons_release();
                trace_event(TRACE_INPUT, "skill-wheel-release", "skill=%d", skill);
                gpui_use_skill(skill);
            }
        }
        return;
    }

    if (l3Edge && !blocked) {
        releasedOn = -1;
        gpui_open_skill_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_SKILL;
        }
    }
}

// --- Y-hold quick-use wheel (a radial_wheel instance). ---
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
// it showed dead segments for drugs he had none of ("kdyÃÂÃÂ¾ drÃÂÃÂ¾ÃÂÃÂ­m B vidÃÂÃÂ­m i drogy kterÃÂÃÂ© nemÃÂÃÂ¡m"), and
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
    if (proto_ptr(pid, &proto) != 0) {
        return;
    }

    // Armour: what it gives, and whether it is the one on him -- picking that one takes it off.
    if (proto->item.type == ITEM_TYPE_ARMOR) {
        Object* worn = obj_dude != NULL ? inven_worn(obj_dude) : NULL;
        snprintf(out, size, "AC %d  DR %d%%  DT %d%s", proto->item.data.armor.armorClass,
            proto->item.data.armor.damageResistance[DAMAGE_TYPE_NORMAL],
            proto->item.data.armor.damageThreshold[DAMAGE_TYPE_NORMAL],
            worn != NULL && worn->pid == pid ? "  - worn, pick to take off" : "");
        return;
    }

    if (proto->item.type != ITEM_TYPE_DRUG) {
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

// Rounds an ammo inventory entry really holds.
//
// ⚠️ A STACK OF AMMO IS NOT "N IDENTICAL BOXES". item_identical (item.cc:538) deliberately
// blanks the round count before comparing, so boxes with DIFFERENT contents merge into one
// entry -- and the surviving Object carries a single count that belongs to the box currently
// being drawn from, not to all of them. item_w_reload takes rounds off exactly that one box.
//
// So `per * boxes` overcounts wildly, and it moves wrong: measured (trace_2026-08-26_201959),
// 4 boxes of 24 read 96, then a reload that took TEN rounds left per=14, boxes=4 -> 56. Forty
// rounds gone from the badge because the ten came off a figure that was then multiplied by four.
//
// The stack is really one partial box plus (boxes - 1) FULL ones, and a full one holds the
// proto's own quantity. Same data, checked against the same trace: 24 + 3*24 = 96 before, and
// 14 + 3*24 = 86 after -- exactly the ten that were spent.
static int gpui_ammo_stack_rounds(Object* ammo, int boxes)
{
    if (ammo == NULL || boxes <= 0) {
        return 0;
    }

    int per = item_w_curr_ammo(ammo);
    if (boxes == 1) {
        return per;
    }

    Proto* proto = NULL;
    int full = 0;
    if (proto_ptr(ammo->pid, &proto) != -1 && proto != NULL) {
        full = proto->item.data.ammo.quantity; // rounds a fresh box of this proto holds
    }
    if (full <= 0) {
        full = per; // unreadable proto: fall back to treating them as equally full
    }

    return per + (boxes - 1) * full;
}

// What one box of ammo actually DOES, for the second line of the AMMO wheel's hub. The weapon and
// drug wheels have carried a detail line for a while and the ammo wheel had none -- it showed a
// name and a round count, which tells you what you are holding but not why you would pick it.
//
// The three numbers that make ammo worth choosing in Fallout are the ones vanilla hides entirely:
// AC modifier (how much of the target's armour class it ignores), DR modifier (same for damage
// resistance) and the damage multiplier. Each part is printed only when it is not the neutral
// value, so plain ball ammo gets a short line instead of a row of zeroes.
static void gpui_ammo_detail(Object* ammo, char* out, size_t size)
{
    out[0] = '\0';
    if (ammo == NULL) {
        return;
    }

    Proto* proto = NULL;
    if (proto_ptr(ammo->pid, &proto) == -1 || proto == NULL) {
        return;
    }

    int ac = proto->item.data.ammo.armorClassModifier;
    int dr = proto->item.data.ammo.damageResistanceModifier;
    int mult = proto->item.data.ammo.damageMultiplier;
    int div = proto->item.data.ammo.damageDivisor;

    // A zero divisor is a broken proto, not a divide-by-zero waiting to happen.
    if (div <= 0) {
        div = 1;
    }

    size_t len = 0;
    if (mult != div) {
        // "x2" reads better than "2/1"; only a proto with a real fraction spends the extra chars.
        if (div == 1) {
            len += (size_t)snprintf(out + len, len < size ? size - len : 0, "dmg x%d", mult);
        } else {
            len += (size_t)snprintf(out + len, len < size ? size - len : 0, "dmg x%d/%d", mult, div);
        }
    }

    if (ac != 0 && len < size) {
        len += (size_t)snprintf(out + len, size - len, "%sAC %+d", len != 0 ? "  " : "", ac);
    }

    if (dr != 0 && len < size) {
        snprintf(out + len, size - len, "%sDR %+d", len != 0 ? "  " : "", dr);
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
            rounds += gpui_ammo_stack_rounds(item, inventory->items[i].quantity);
        }
    }

    snprintf(out + len, size - len, "  %s: %d", proto_name(ammoPid), rounds);
}

// Distinct drug PIDs the dude is carrying loose, in priority-then-inventory order, with the combined
// count of each. Returns how many (at most GPUI_QUICK_MAX). Shared by the B-hold drug wheel and the
// drug page at the end of the Y-hold weapon wheel, so both always show the same list in the same seats.
//
// PIDs, not Object*: unlike a weapon, a drug is consumed the moment you use it, so a pointer
// would dangle. "Use the next one of these I'm carrying" is the right identity here and it
// survives consuming, looting, trading and reloading a save.
static int gpui_collect_drugs(int* pids, int* counts)
{
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

    return count;
}

// Fill drug segments [0, count) of `segs` -- name, count badge, effect line, prose, icon. The detail
// lines live in gpui_detail_buf, indexed by SEGMENT, so the caller's segment numbers must be the
// buffer slots too (both wheels start their drugs at segment 0).
static void gpui_fill_drug_segments(RadialSegment* segs, const int* pids, const int* counts, int count, int idBase)
{
    for (int i = 0; i < count; i++) {
        // idBase < 0 encodes the index (weapon ring, see GPUI_WHEEL_DRUG_ID_BASE); otherwise the
        // segment id IS the pid, as the B-hold wheel has always had it.
        segs[i].id = idBase < 0 ? idBase - i : pids[i];
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
}

static void gpui_open_quick_wheel()
{
    int pids[GPUI_QUICK_MAX];
    int counts[GPUI_QUICK_MAX];
    int count = gpui_collect_drugs(pids, counts);

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
    gpui_fill_drug_segments(segs, pids, counts, count, 0);

    RadialWheelArt art;
    radial_wheel_open(segs, count, "QUICK USE",
        gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    if (!radial_wheel_is_open()) {
        gpui_icons_release();
    }
}

// ⚠️ ONE INJECTION AT A TIME IN A FIGHT (2026-10-05, his pick of four: "Jedno píchnutí naráz").
// The 2 AP alone never limited healing. Next to an enemy at AGI 10 the pool hands them back in
// ~0.7 s, and every press RESTARTED the injection animation (1.38 s in power armour) instead of
// waiting for it: trace_2026-10-05_001014 went 21 -> 84 HP in three seconds of LB+B mashing, six
// stimpaks, while the bar showed "only yellow" -- the second green bulb lit for 0.07-0.17 s before
// the next press took it. "Nemám AP mám jen žluté a i tak se můžu healovat do nekonečna."
//
// So a drug now holds the next one off for as long as its own injection animation lasts. TIMED, not
// "while the animation plays": the animation is cancellable (a single step cuts it) and is not played
// at all while he is down, and either would have reopened the spam -- tap a direction, heal again;
// get knocked over, heal five times on the floor. Out of a fight there is no lock, just as there is
// no AP cost: nothing is racing you there.
static unsigned int gpui_injection_since = 0;
static unsigned int gpui_injection_ms = 0;

// How long the dude's injection animation runs right now: frames x the same ticks-per-frame the
// scheduler will use (compute_tpf -- the global animation speed lever applies to it), so the lock
// and what you see end together. The fid is built the way anim_animate builds it.
static unsigned int gpui_injection_duration()
{
    int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_MAGIC_HANDS_MIDDLE,
        (obj_dude->fid & 0xF000) >> 12, obj_dude->rotation + 1);

    unsigned int ms = 1000;
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art != NULL) {
        ms = (unsigned int)art_frame_max_frame(art) * compute_tpf(obj_dude, fid);
        art_ptr_unlock(handle);
    }
    return ms;
}

// ⚠️ THE JAB FIRST, THEN THE HEAL (2026-10-16, "šlo by že když dám rb tak je prvně animace píchnutí a
// pak heal?"). The drug used to work at the press and the use animation to follow as decoration. Now
// the animation plays first and the drug works at its ACTION FRAME, the moment the needle goes in
// (his pick "Při píchnutí": frame 7 of 11 of the dude's use art, ~0.55 s at AGI/INT/PE 9) -- the rest
// of the animation plays out after. His second pick, "Zruší se, stimpack zůstane": a hit that
// STAGGERS him or a knockdown before that frame ends it (rt_reaction_flinch -> anim_force_clear), the
// stimpak stays in the pack and the AP come back -- the reload's rules. A trigger pull alone does not
// (combat_rt_is_injecting spares him in action_attack, like a reload).
//
// The AP go at the press, as with the reload, and are refunded by the tick on a cut
// (gpui_inject_tick). The pack is only touched at the jab, so a cut jab costs nothing at all.
static bool gpui_inject_pending = false; // a jab is playing and has not reached its frame yet
static int gpui_inject_pid = -1;
static int gpui_inject_ap = 0;
static unsigned int gpui_inject_at = 0;

// ⚠️ HOLD THE SLOT, KEEP HEALING -- LIKE ESTUS (2026-10-19, his: "něco jako souls když pije hráč estus
// flašky. Nejdřív se napije - animace zvednutí ruky a pak další estus je hlt z flašky. Tak v tomto
// případě se zahraje kus animace píchnutí znova a tak dude se znovu healne" -- "a to nebude stát AP,
// když už se to healuje po několikáté"). A drug taken from a quick slot whose button is STILL HELD at
// the jab's action frame is followed, GPUI_CHAIN_DELAY_MS later, by the next one: the hand stays up
// and only the jab plays again -- the injection animation from GPUI_CHAIN_BACK_FRAMES before its
// action frame (anim_set_start_frame). The first one paid the pack's price (gpui_pack_cost); the
// ones after it are free, as everything you take inside the open inventory is. It ends when the
// button comes up, when nothing is left in the slot to take, at full health (hit points plus the heal
// still coming in, combat_rt_heal_left -- holding on would only waste them), or when a hit cuts the
// jab (gpui_inject_tick). The keyboard reports no release, so H never chains.
#define GPUI_CHAIN_BACK_FRAMES 3
#define GPUI_CHAIN_DELAY_MS 120
static int gpui_quick_use_slot = -1;     // the quick slot gpui_quick_use is being called for, else -1
static int gpui_chain_slot = -1;         // the slot whose held button drives the chain; -1 = none
static unsigned int gpui_chain_due_at = 0; // when the next one is asked for; 0 = not now
static int gpui_chain_count = 0;          // follow-ups in this chain
// The explosive put-down (gpui_place_explosive).
static bool gpui_place_pending = false;
static Object* gpui_place_item = NULL; // compared against the pack's pointers only, until dropped
static unsigned int gpui_place_at = 0;

static Object* gpui_carried_drug(int pid)
{
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item != NULL && item->pid == pid && item_get_type(item) == ITEM_TYPE_DRUG) {
            return item;
        }
    }
    return NULL;
}

// One of the dude's drugs, taken NOW: its effect, the item gone, the HP bar redrawn. This is
// deliberately the same sequence combatai.cc:370 runs when an NPC self-medicates -- remove one,
// apply it, destroy it -- so the player's quick-use and the AI's go through identical engine paths.
// False = the drug code refused (robotic body, dead) and the item is back in the pack.
//
// In a fight the hit points it gives do not land at once: item_d_take_drug puts them into the heal
// pool (combat_rt_heal_over_time).
static bool gpui_take_drug_now(Object* item)
{
    if (item_remove_mult(obj_dude, item, 1) != 0) {
        return false;
    }

    if (item_d_take_drug(obj_dude, item) == -1) {
        item_add_force(obj_dude, item, 1);
        return false;
    }

    obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
    obj_destroy(item);

    intface_update_hit_points(true);
    return true;
}

// The jab's action frame (a register_object_call -- NOT a must-call, so a cut jab never gets here).
static int gpui_inject_done(void* a1, void* a2)
{
    (void)a1;
    (void)a2;

    if (!gpui_inject_pending) {
        return 0;
    }
    gpui_inject_pending = false;
    combat_rt_inject_end(obj_dude);

    int hpBefore = critter_get_hits(obj_dude);
    Object* item = gpui_carried_drug(gpui_inject_pid);
    if (item == NULL || !gpui_take_drug_now(item)) {
        // Gone from the pack while the needle was on its way, or refused: nothing taken, nothing paid.
        combat_rt_refund_ap(gpui_inject_ap);
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "quick-use-refused", "pid=%d why=%s ap=%d", gpui_inject_pid,
            item == NULL ? "gone" : "refused", combat_rt_player_ap());
        return 0;
    }

    if (gpui_chain_slot != -1) {
        gpui_chain_due_at = get_time() + GPUI_CHAIN_DELAY_MS; // gpui_heal_chain_tick decides then
        if (gpui_chain_due_at == 0) {
            gpui_chain_due_at = 1;
        }
    }

    trace_event(TRACE_ACTION, "quick-use", "pid=%d hp=%d->%d pool=%d ap=%d cost=%d qp=%d lock=%ums fight=%d jab=%ums",
        gpui_inject_pid, hpBefore, critter_get_hits(obj_dude), combat_rt_heal_left(), combat_rt_player_ap(),
        gpui_inject_ap, perk_level(PERK_QUICK_POCKETS), gpui_injection_ms, combat_rt_active() ? 1 : 0,
        elapsed_time(gpui_inject_at));
    return 0;
}

// Starts the jab. False = it could not register (he is in another live sequence -- a swing, a walk
// segment -- or a transaction is open), and the caller takes the drug at once, as before.
// followUp: the next one of a held-slot chain -- only the jab, from GPUI_CHAIN_BACK_FRAMES before the
// action frame (see gpui_chain_slot).
static bool gpui_inject_start(Object* item, int apCost, bool followUp = false)
{
    int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_MAGIC_HANDS_MIDDLE,
        (obj_dude->fid & 0xF000) >> 12, obj_dude->rotation + 1);
    int actionFrame = 0;
    int frames = 0;
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art != NULL) {
        actionFrame = art_frame_action_frame(art);
        frames = art_frame_max_frame(art);
        art_ptr_unlock(handle);
    }
    int startFrame = 0;
    if (followUp) {
        startFrame = actionFrame - GPUI_CHAIN_BACK_FRAMES;
        if (startFrame < 0) {
            startFrame = 0;
        }
    }

    if (register_begin(ANIMATION_REQUEST_RESERVED) != 0) {
        return false;
    }
    register_object_animate(obj_dude, ANIM_MAGIC_HANDS_MIDDLE, 0);
    // delay = frames of the animation before it: the call runs as the action frame is reached.
    register_object_call(obj_dude, NULL, gpui_inject_done, actionFrame - startFrame);

    // The bracket makes compute_tpf time the jab by (AGI + INT + PE) / 3 (2026-10-06) -- for the
    // animation, which starts inside register_end, and for the lock measured here alike.
    anim_set_injection_timing(obj_dude);
    anim_set_start_frame(obj_dude, startFrame);
    int rc = register_end();
    anim_set_start_frame(NULL, 0);
    unsigned int ms = gpui_injection_duration();
    anim_set_injection_timing(NULL);
    if (startFrame > 0 && frames > 0) {
        ms = ms * (unsigned int)(frames - startFrame) / (unsigned int)frames; // the lock is the part played
    }

    if (rc != 0) {
        return false;
    }

    gpui_injection_since = get_time();
    gpui_injection_ms = ms;

    combat_rt_spend_ap(apCost);
    gpui_inject_pending = true;
    gpui_inject_pid = item->pid;
    gpui_inject_ap = apCost;
    gpui_inject_at = get_time();
    combat_rt_inject_begin(obj_dude);
    if (!followUp) {
        gpui_chain_slot = gpui_quick_use_slot; // a slot's own press may chain; anything else may not
        gpui_chain_count = 0;
    }
    gpui_chain_due_at = 0;

    trace_event(TRACE_ACTION, "inject-start", "pid=%d frame=%d from=%d lock=%ums ap=%d", item->pid, actionFrame,
        startFrame, ms, combat_rt_player_ap());
    return true;
}

// From the safe tick: a jab that left its pose before its frame (a stagger, a knockdown, anim_stop
// for a save or a conversation) gives the AP back and lifts the lock, so the next press can try again.
static void gpui_inject_tick()
{
    if (!gpui_inject_pending || obj_dude == NULL) {
        return;
    }
    if (combat_rt_is_injecting(obj_dude)) {
        return;
    }

    gpui_inject_pending = false;
    combat_rt_refund_ap(gpui_inject_ap);
    gpui_injection_ms = 0;
    gpui_chain_slot = -1;
    gpui_chain_due_at = 0;
    trace_event(TRACE_ACTION, "inject-cut", "pid=%d refund=%d ap=%d anim=%d after=%ums", gpui_inject_pid,
        gpui_inject_ap, combat_rt_player_ap(), FID_ANIM_TYPE(obj_dude->fid), elapsed_time(gpui_inject_at));
}

static Object* gpui_quick_slot_item(int slot, int* count);
static bool gpui_item_is_throwable(Object* item);
static bool gpui_item_is_tool(Object* item);

static void gpui_heal_chain_end(const char* why)
{
    if (gpui_chain_slot != -1) {
        trace_event(TRACE_ACTION, "heal-chain", "end=%s count=%d hp=%d pool=%d", why, gpui_chain_count,
            obj_dude != NULL ? critter_get_hits(obj_dude) : -1, combat_rt_heal_left());
    }
    gpui_chain_slot = -1;
    gpui_chain_due_at = 0;
}

// From the safe tick: the next jab of a held slot (gpui_chain_slot), once its moment has come.
static void gpui_heal_chain_tick()
{
    if (gpui_chain_slot == -1 || gpui_chain_due_at == 0 || obj_dude == NULL) {
        return;
    }
    if ((int)(get_time() - gpui_chain_due_at) < 0) {
        return;
    }
    gpui_chain_due_at = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    SDL_GameControllerButton button = gpui_chain_slot == GPUI_QS_LB
        ? SDL_CONTROLLER_BUTTON_LEFTSHOULDER
        : SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    if (pad == NULL || SDL_GameControllerGetButton(pad, button) == 0) {
        gpui_heal_chain_end("released");
        return;
    }
    if (critter_is_dead(obj_dude) || critter_is_prone(obj_dude) || gpui_inject_pending) {
        gpui_heal_chain_end("busy");
        return;
    }
    if (critter_get_hits(obj_dude) + combat_rt_heal_left() >= stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS)) {
        gpui_heal_chain_end("full");
        return;
    }
    int count;
    Object* item = gpui_quick_slot_item(gpui_chain_slot, &count);
    if (item == NULL || gpui_item_is_throwable(item) || gpui_item_is_tool(item)
        || item_get_type(item) != ITEM_TYPE_DRUG) {
        gpui_heal_chain_end("none");
        return;
    }
    // The jab before is still lowering the hand: that tail is what the next one replaces. Anything
    // else holding the body (a step, a swing) ends the chain instead.
    if (anim_busy(obj_dude) != 0) {
        if (FID_ANIM_TYPE(obj_dude->fid) != ANIM_MAGIC_HANDS_MIDDLE) {
            gpui_heal_chain_end("busy");
            return;
        }
        anim_force_clear(obj_dude);
    }
    if (!gpui_inject_start(item, 0, true)) {
        gpui_heal_chain_end("refused");
        return;
    }
    gpui_chain_count++;
    trace_event(TRACE_ACTION, "heal-chain", "next=%d pid=%d hp=%d pool=%d left=%d", gpui_chain_count, item->pid,
        critter_get_hits(obj_dude), combat_rt_heal_left(), count - 1);
}

// Use one of `pid` on the dude: the jab, and the drug at its frame (see gpui_inject_pending).
// ⚠️ ANYTHING FROM THE PACK COSTS WHAT THE INVENTORY COSTS (2026-10-19, his rule: "prostě je to z
// inventáře. Granát taky stojí 4"). Vanilla reaches into the pack through the inventory screen -- 4 AP
// to open it, one less per rank of Quick Pockets (inven_action_cursor's own formula) -- and every pad
// shortcut that takes something out of it is that reach: a drug or an item from a quick slot, an
// explosive put down, an item used on something, the grenade's quick draw, the weapon wheel, armour.
// They were 2 (1 with Quick Pockets) and the wheel was free. Swapping hands (Y) stays free, and a
// reload is vanilla's own 2.
static int gpui_pack_cost()
{
    int cost = 4 - perk_level(PERK_QUICK_POCKETS);
    return cost > 0 ? cost : 0;
}

int gamepad_ui_pack_ap_cost()
{
    return gpui_pack_cost();
}

static void gpui_quick_use(int pid)
{
    // ⚠️ COSTS 2 AP AND REFUSES WHEN YOU CANNOT PAY (2026-08-29). It used to charge only when
    // the pool could afford it and never refuse -- "running dry should not be what stops you
    // healing" -- and the player caught it: "přišlo mi že můžu stimpackovat, ikdyž nemám body".
    // That exemption contradicted the rule he set for movement, where 0 AP MUST stop you (see
    // the rejected ap_empty_walks): a free action on an empty pool is a free action either way.
    //
    // The test is FIRST because the charge below happens after the item has been consumed and
    // destroyed -- refusing down there would eat the drug and heal nothing. Out of combat the
    // pool sits at maximum, so this never bites outside a fight.
    //
    // QUICK POCKETS (2026-10-11, his idea: "quick pocket by mohl snížit cenu stimpacku o 1") -- and
    // since 2026-10-19 the whole price is the pack's (gpui_pack_cost): 4, less one per rank.
    int apCost = gpui_pack_cost();
    if (combat_rt_player_ap() < apCost) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "quick-use-refused", "pid=%d why=ap ap=%d", pid, combat_rt_player_ap());
        return;
    }

    if (gpui_inject_pending
        || (combat_rt_active() && elapsed_time(gpui_injection_since) < gpui_injection_ms)) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "quick-use-refused", "pid=%d why=injecting left=%ums ap=%d",
            pid, gpui_injection_ms - elapsed_time(gpui_injection_since), combat_rt_player_ap());
        return;
    }

    Object* item = gpui_carried_drug(pid);
    if (item == NULL) {
        gsound_play_sfx_file("iisxxxx1"); // out of stock: refuse audibly, never silently
        return;
    }

    // ⚠️ NOT WHILE HE IS DOWN. ANIMATION_REQUEST_RESERVED means this registration does not ask
    // permission -- it takes obj_dude off whatever he is currently playing. Fired during a
    // knockdown that is the FALL, so the dude snapped upright into the drug-taking pose in the
    // middle of going down: "mel bych padat, nicmene nepadam, slysim jen zvuk" (the knockdown SFX
    // rides the attacker's sequence, so it still played). Traced 2026-08-29_165046 s=37-40 --
    // anim 20 (falling) -> anim 11 (this) -> anim 14, and the standup pass then reported
    // `fall=14`, i.e. it stood him up from a hit-react he should never have been in.
    //
    // The HEAL still happens -- below, with the AP; only the animation is skipped (the same fallback
    // covers a jab that could not register: in the traces that is always the stand-up after a fall).
    // Making the drug itself fail here would be a silent balance change: being knocked down would
    // start costing you the heal.
    //
    // ⚠️ ...BUT IN A FIGHT IT NO LONGER LANDS AT ONCE (2026-10-19). This path was the spam: five
    // stimpaks on the floor and getting up, 36 -> 71 HP in under two seconds (trace_2026-10-19_204959,
    // every one jab=0). It goes into the heal pool like the jab's does (item_d_take_drug).
    if (!critter_is_prone(obj_dude) && gpui_inject_start(item, apCost)) {
        return;
    }

    // On the floor, or the jab could not register: the drug is taken now, with no animation (in a fight
    // its hit points still come in over time).
    int hpBefore = critter_get_hits(obj_dude);
    int apBefore = combat_rt_player_ap();
    if (!gpui_take_drug_now(item)) {
        return;
    }

    // The lock still holds the next one off (gpui_injection_since) -- the prone case is exactly the
    // one it must cover.
    anim_set_injection_timing(obj_dude);
    gpui_injection_since = get_time();
    gpui_injection_ms = gpui_injection_duration();
    anim_set_injection_timing(NULL);

    // Vanilla charges 2 AP for taking a drug. Unconditional now: affordability was settled at
    // the top of this function, before the item was spent.
    combat_rt_spend_ap(apCost);

    trace_event(TRACE_ACTION, "quick-use", "pid=%d hp=%d->%d pool=%d ap=%d->%d cost=%d qp=%d lock=%ums fight=%d jab=0",
        pid, hpBefore, critter_get_hits(obj_dude), combat_rt_heal_left(), apBefore, combat_rt_player_ap(),
        apCost, perk_level(PERK_QUICK_POCKETS), gpui_injection_ms, combat_rt_active() ? 1 : 0);
}

// What the dedicated heal button (LB+B) reaches for, best first. Fallout 1 has no other
// injectable healing: FIRST_AID_KIT and DOCTORS_BAG are ITEM_TYPE_MISC used THROUGH a skill,
// not through item_d_take_drug, so they are not candidates here.
//
// Plain Stimpack first on purpose. The Super heals more but does delayed damage, so spending
// it while an ordinary one is in the pack would be a worse trade made on the player's behalf;
// it is the FALLBACK, for when the ordinary ones have run out.
static const int gpui_heal_order[] = {
    PROTO_ID_STIMPACK,
    PROTO_ID_SUPER_STIMPACK,
};

// The healing item LB+B would reach for NEXT, and how many of it are carried. ONE selection
// rule for the button and for the interface-bar slot that advertises it -- if these two ever
// disagreed, the bar would be showing you an item the button does not use.
static Object* gpui_next_heal(int* count)
{
    *count = 0;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int rank = 0; rank < (int)(sizeof(gpui_heal_order) / sizeof(gpui_heal_order[0])); rank++) {
        for (int i = 0; i < inventory->length; i++) {
            Object* item = inventory->items[i].item;
            // The type test matters: gpui_quick_use refuses anything that is not a DRUG, so a
            // pid match on some other item would burn the press and heal nothing.
            if (item != NULL && item->pid == gpui_heal_order[rank]
                && item_get_type(item) == ITEM_TYPE_DRUG) {
                *count = inventory->items[i].quantity;
                return item;
            }
        }
    }
    return NULL;
}

// --- LB+RB: run to the nearest map exit ---
//
// "Just take me to the next area." Crossing a cleared map on foot is the part of Fallout that is
// pure travel time, and on a pad it is worse than on a mouse: a mouse player clicks the far edge of
// the screen once and the engine paths him there, while the stick player has to steer every tile of
// it himself. This is the pad's version of that click, aimed at the one destination that is always
// what you meant -- the way out.
//
// ⚠️ BOTH SHOULDERS, because it is the only two-button gesture left that costs nothing. LB is the
// modifier already (run, LB+Y quick save, LB+B heal) and its face-button combos are all spoken for;
// worse, LB is HELD CONSTANTLY by anyone who runs everywhere, so hanging this on LB+X would have
// eaten reload-while-running -- the exact collision the quick-save comment calls the layout's
// accepted cost, and one more of them is one too many. RB's own job in the world is toggling the
// cursor mode, a tap worth nothing mid-run, so the combo simply swallows it.
//
// OUT OF COMBAT ONLY, and that is not timidity: auto-pathing a player through a firefight would walk
// him into rifle fire he is not steering away from. (Leaving a map mid-fight works by stepping onto
// the grid yourself -- immediate since 2026-10-06, see combat_rt_arm_map_exit.)
//
// Exit grids are the MISC objects with pids 0x5000010..0x5000017 -- the same range obj_move_to_tile
// tests to fire map_leave_map, and the same one obj_dude_on_exit_grid reads, so "what counts as an
// exit" has exactly one definition in the build.
static void gpui_run_to_exit()
{
    if (obj_dude == NULL || !intface_is_enabled()) {
        return;
    }

    if (combat_rt_enabled() ? combat_rt_active() : (isInCombat() != 0)) {
        gsound_play_sfx_file("iisxxxx1");
        display_print("Not while you're being shot at.");
        return;
    }

    Object** objects;
    int count = obj_create_list(-1, obj_dude->elevation, OBJ_TYPE_MISC, &objects);

    Object* best = NULL;
    int bestDist = INT_MAX;
    for (int i = 0; i < count; i++) {
        Object* obj = objects[i];
        if (obj == NULL || obj->pid < 0x5000010 || obj->pid > 0x5000017) {
            continue;
        }
        // Straight tile distance, not path length. make_path on every candidate would be the
        // honest answer and is far too expensive for a button press; the nearest grid as the crow
        // flies is the right one on essentially every map, and if the path turns out to be longer
        // the engine still walks it correctly -- only the CHOICE would be second-best.
        int d = tile_dist(obj_dude->tile, obj->tile);
        if (d < bestDist) {
            bestDist = d;
            best = obj;
        }
    }

    if (count > 0) {
        obj_delete_list(objects);
    }

    if (best == NULL) {
        gsound_play_sfx_file("iisxxxx1");
        display_print("No way out of here.");
        return;
    }

    if (bestDist <= 1) {
        gsound_play_sfx_file("iisxxxx1");
        display_print("You're standing on it.");
        return;
    }

    // Same shape as dude_run (anim.cc): clear whatever the stick had registered, then one run
    // sequence to the destination. actionPoints -1 = unlimited, which is what out-of-combat
    // movement always passes. Sneaking drops for the same reason vanilla drops it on a run.
    register_clear(obj_dude);

    if (!perk_level(PERK_SILENT_RUNNING)) {
        pc_flag_off(PC_FLAG_SNEAKING);
    }

    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_run_to_tile(obj_dude, best->tile, obj_dude->elevation, -1, 0);
    int rc = register_end();

    trace_event(TRACE_ACTION, "exit-nav", "tile=%d dist=%d rc=%d", best->tile, bestDist, rc);

    if (rc == -1) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    gsound_play_sfx_file("ib1p1xx1");
    display_print("Heading for the exit.");
}

// --- QUICK SLOTS: the items and the throw (see the note at gpui_consumed_lb) ---

int gamepad_ui_quick_slot_get(int slot)
{
    return slot >= 0 && slot < GPUI_QS_COUNT ? gpui_qs_bound[slot] : -1;
}

void gamepad_ui_quick_slot_set(int slot, int pid)
{
    if (slot >= 0 && slot < GPUI_QS_COUNT) {
        gpui_qs_bound[slot] = pid;
    }
}

// Can a quick slot THROW it? ⚠️ Either mode counts (2026-10-17, "i oštěp k hodu na LB nebo RB?"): a
// spear thrusts on its primary and throws on its secondary, and the quick throw fires the THROW mode
// whichever it is (combat_rt_quick_throw_mode). Grenades, rocks and throwing knives throw on the
// primary.
static bool gpui_item_is_throwable(Object* item)
{
    return item != NULL && item_get_type(item) == ITEM_TYPE_WEAPON
        && (item_w_subtype(item, HIT_MODE_RIGHT_WEAPON_PRIMARY) == ATTACK_TYPE_THROW
            || item_w_subtype(item, HIT_MODE_RIGHT_WEAPON_SECONDARY) == ATTACK_TYPE_THROW);
}

// ...but only something made to be thrown is a slot's OWN pick: a spear in the pack is a melee
// weapon first, and RB's automatic choice must never throw away the one you fight with. A spear goes
// in a slot when you bind it there.
static bool gpui_item_throws_first(Object* item)
{
    return item != NULL && item_get_type(item) == ITEM_TYPE_WEAPON
        && item_w_subtype(item, HIT_MODE_RIGHT_WEAPON_PRIMARY) == ATTACK_TYPE_THROW;
}

// A TOOL for a quick slot (2026-10-17): a miscellaneous item the game lets you USE -- the motion
// sensor (the automap with the living on it), the Geiger counter and the Stealth Boy (on/off), a
// flare, an explosive (its timer) -- or use ON something (the crosshair, like the rope). Exactly the
// set the item pages of the Y wheel list (gpui_collect_usable_items), used the same way.
static bool gpui_item_is_tool(Object* item)
{
    return item != NULL && item_get_type(item) == ITEM_TYPE_MISC
        && (proto_action_can_use(item->pid) || proto_action_can_use_on(item->pid));
}

// The LB slot's own pick: the hardest-hitting throwable carried (grenades before knives and rocks).
static Object* gpui_best_throwable()
{
    Object* best = NULL;
    int bestDamage = -1;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (!gpui_item_throws_first(item)) {
            continue;
        }
        int minDamage = 0;
        int damage = 0;
        item_w_damage_min_max(item, &minDamage, &damage);
        if (damage > bestDamage) {
            best = item;
            bestDamage = damage;
        }
    }
    return best;
}

static Object* gpui_next_heal(int* count);

// What a slot would act on now: its bound item while one is carried, else its own pick.
static Object* gpui_quick_slot_item(int slot, int* count)
{
    *count = 0;
    if (obj_dude == NULL) {
        return NULL;
    }
    int pid = gpui_qs_bound[slot];
    if (pid != -1) {
        Object* bound = inven_pid_is_carried_ptr(obj_dude, pid);
        if (bound != NULL) {
            *count = inven_pid_quantity_carried(obj_dude, pid);
            return bound;
        }
    }
    // ⚠️ RB = GRENADES, LB = STIMPAK (2026-10-17, "lb a rb prohoď, rb bude pro granáty a lb stimpack").
    // Swapped from the first layout (LB grenade, RB heal). Only the slots' OWN picks changed: the button
    // mechanics already follow what is in the slot -- a throwable is drawn on the press (LB: on the
    // hold) and thrown on the release, a drug is taken (RB: on the press, LB: on the release of a hold,
    // since a tap of LB is still the reload). A binding made in the inventory overrides either pick.
    if (slot == GPUI_QS_RB) {
        Object* item = gpui_best_throwable();
        if (item != NULL) {
            *count = inven_pid_quantity_carried(obj_dude, item->pid);
        }
        return item;
    }
    return gpui_next_heal(count);
}

// The throw, as a small state machine driven from the safe tick (it wields and fires, both of which
// register animations): press -> DRAW_REQ -> DRAWING (the take-out plays) -> READY (held) -> release
// -> THROW_REQ -> THROWING (the attack runs) -> RESTORE (the old weapon back) -> IDLE.
enum {
    GPUI_QT_IDLE,
    GPUI_QT_DRAW_REQ,
    GPUI_QT_DRAWING,
    GPUI_QT_READY,
    GPUI_QT_THROW_REQ,
    GPUI_QT_THROWING,
    GPUI_QT_RESTORE,
};
static int gpui_qt_state = GPUI_QT_IDLE;
static int gpui_qt_slot = -1;
static int gpui_qt_pid = -1;
static int gpui_qt_hand = 0;
static int gpui_qt_prev_pid = -1; // what the hand held before the draw (-1 = empty)
// ...and WHICH one. ⚠️ The pid alone gave back the wrong gun (2026-10-16, "po granátu v mé pušce
// nebyly náboje"): the save carried two Hunting Rifles, the loaded one in the hand (id 249, 10
// rounds) and an empty spare earlier in the pack (id 2837), and inven_pid_is_carried_ptr returns the
// first. Only ever compared against the pack's own pointers, never dereferenced, so a stale one is
// harmless.
static Object* gpui_qt_prev_obj = NULL;
static bool gpui_qt_release_seen = false; // let go while it was still coming out
static unsigned int gpui_qt_since = 0;

void gamepad_ui_quick_slots_reset()
{
    gpui_qs_bound[GPUI_QS_LB] = -1;
    gpui_qs_bound[GPUI_QS_RB] = -1;
    gpui_qt_state = GPUI_QT_IDLE;
    gpui_qt_slot = -1;
    gpui_qt_prev_obj = NULL;
    gpui_qs_use_pending_slot = -1;
    // A jab from before a load never reaches its frame; its AP belonged to the old pool.
    gpui_inject_pending = false;
    combat_rt_inject_end(obj_dude);
    // Nor does a put-down: the explosive it pointed at belonged to the old world.
    gpui_place_pending = false;
    gpui_place_item = NULL;
}

static void gpui_qt_set(int state)
{
    gpui_qt_state = state;
    gpui_qt_since = get_time();
}

static bool gpui_qs_drawn(int slot)
{
    return gpui_qt_state != GPUI_QT_IDLE && gpui_qt_slot == slot;
}

// From the hook (no animation here): a press of a slot's button, or its hold for LB.
static void gpui_qs_press(int slot, bool throwOnDraw)
{
    int count;
    Object* item = gpui_quick_slot_item(slot, &count);
    if (item == NULL) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "quick-slot-empty", "slot=%d", slot);
        return;
    }
    if (!gpui_item_is_throwable(item)) {
        // A drug: taken on the safe tick (RB on the press, LB on the release -- the caller decides
        // when to call this).
        gpui_qs_use_pending_slot = slot;
        gpui_qs_use_pending_since = get_time();
        trace_event(TRACE_INPUT, "quick-slot-use", "slot=%d pid=%d", slot, item->pid);
        return;
    }
    if (gpui_qt_state != GPUI_QT_IDLE) {
        return; // one throw at a time
    }
    gpui_qt_slot = slot;
    gpui_qt_pid = item->pid;
    gpui_qt_release_seen = throwOnDraw;
    gpui_qt_set(GPUI_QT_DRAW_REQ);
    trace_event(TRACE_INPUT, "quick-draw-press", "slot=%d pid=%d", slot, item->pid);
}

// From the hook: the slot's button came up.
static void gpui_qs_release(int slot)
{
    if (gpui_qt_slot != slot) {
        return;
    }
    if (gpui_qt_state == GPUI_QT_DRAW_REQ || gpui_qt_state == GPUI_QT_DRAWING) {
        gpui_qt_release_seen = true;
    } else if (gpui_qt_state == GPUI_QT_READY) {
        gpui_qt_set(GPUI_QT_THROW_REQ);
    }
}

// B while one is out: put it away. True if there was one to cancel.
static bool gpui_qs_cancel()
{
    if (gpui_qt_state == GPUI_QT_DRAW_REQ) {
        gpui_qt_set(GPUI_QT_IDLE);
        trace_event(TRACE_INPUT, "quick-draw-cancel", "stage=request");
        return true;
    }
    if (gpui_qt_state == GPUI_QT_DRAWING || gpui_qt_state == GPUI_QT_READY) {
        gpui_qt_set(GPUI_QT_RESTORE);
        trace_event(TRACE_INPUT, "quick-draw-cancel", "stage=drawn");
        return true;
    }
    return false;
}

// LB / RB over an item in your own inventory: bind it to that slot (only what a slot can act on -- a
// throwable or a drug); the same press on the bound item gives the slot back to its own pick.
static void gpui_qs_bind_under_cursor(int slot)
{
    Object* item = inven_list_item_under_cursor();
    if (item == NULL) {
        return;
    }
    char msg[120];
    if (!gpui_item_is_throwable(item) && item_get_type(item) != ITEM_TYPE_DRUG && !gpui_item_is_tool(item)) {
        gsound_play_sfx_file("iisxxxx1");
        snprintf(msg, sizeof(msg), "%s: only something to throw, take or use.", slot == GPUI_QS_LB ? "LB" : "RB");
        inven_display_msg(msg);
        return;
    }
    if (gpui_qs_bound[slot] == item->pid) {
        gpui_qs_bound[slot] = -1;
        snprintf(msg, sizeof(msg), "%s quick slot: automatic.", slot == GPUI_QS_LB ? "LB" : "RB");
    } else {
        gpui_qs_bound[slot] = item->pid;
        snprintf(msg, sizeof(msg), "%s quick slot: %s.", slot == GPUI_QS_LB ? "LB" : "RB", item_name(item));
    }
    gsound_play_sfx_file("ib1p1xx1");
    inven_display_msg(msg);
    trace_event(TRACE_INPUT, "quick-slot-bind", "slot=%d pid=%d", slot, gpui_qs_bound[slot]);
}

static Object* gpui_qt_hand_item()
{
    return gpui_qt_hand ? inven_right_hand(obj_dude) : inven_left_hand(obj_dude);
}

// The safe-tick half (gpui_drain_requests): every wield and every throw happens here.
static void gpui_qt_tick()
{
    if (obj_dude == NULL || gpui_qt_state == GPUI_QT_IDLE) {
        return;
    }

    bool screenUp = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled();

    switch (gpui_qt_state) {
    case GPUI_QT_DRAW_REQ: {
        if (screenUp || critter_is_prone(obj_dude) || elapsed_time(gpui_qt_since) > 1000) {
            gpui_qt_set(GPUI_QT_IDLE);
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "quick-draw-refused", "why=%s", screenUp ? "screen" : "busy");
            return;
        }
        if (anim_busy(obj_dude) != 0) {
            return; // a step or a swing tail -- the request waits a moment for the body
        }
        Object* item = inven_pid_is_carried_ptr(obj_dude, gpui_qt_pid);
        if (item == NULL) {
            gpui_qt_set(GPUI_QT_IDLE);
            gsound_play_sfx_file("iisxxxx1");
            return;
        }
        gpui_qt_hand = intface_is_item_right_hand();
        Object* current = gpui_qt_hand_item();
        if (current != NULL && current->pid == gpui_qt_pid) {
            // Already in the hand: nothing to draw, nothing to pay, nothing to put back.
            gpui_qt_prev_pid = gpui_qt_pid;
            gpui_qt_prev_obj = current;
            gpui_qt_set(gpui_qt_release_seen ? GPUI_QT_THROW_REQ : GPUI_QT_READY);
            return;
        }
        // A weapon change, priced like the wheel's (gpui_pack_cost, 2026-10-19 -- it was 2, 1
        // with Quick Pockets). The put-back after the throw stays free: one draw, one price.
        int apCost = gpui_pack_cost();
        if (combat_rt_player_ap() < apCost) {
            gpui_qt_set(GPUI_QT_IDLE);
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "quick-draw-refused", "why=ap ap=%d", combat_rt_player_ap());
            return;
        }
        // The art inven_wield is about to check itself (see gpui_weapon_equip for why first).
        int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF,
            item_w_anim_weap(item, HIT_MODE_RIGHT_WEAPON_PRIMARY), item_w_anim_code(item),
            obj_dude->rotation + 1);
        if (!art_exists(fid)) {
            gpui_qt_set(GPUI_QT_IDLE);
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "quick-draw-refused", "why=art pid=%d", gpui_qt_pid);
            return;
        }
        gpui_qt_prev_pid = current != NULL ? current->pid : -1;
        gpui_qt_prev_obj = current;
        combat_rt_spend_ap(apCost);
        // ⚠️ NO HOLSTER (2026-10-16, his pick "Bez schování" to "ať je přepínání víc smooth"): the
        // put-away of the gun was ~0.6 s of nothing between the press and the throw. The grenade is
        // in the hand at once; the gun comes back with its own take-out after the throw (RESTORE).
        inven_wield_instant(obj_dude, item, gpui_qt_hand);
        intface_update_items(true);
        trace_event(TRACE_ACTION, "quick-draw", "slot=%d pid=%d prev=%d ap=%d", gpui_qt_slot, gpui_qt_pid,
            gpui_qt_prev_pid, combat_rt_player_ap());
        gpui_qt_set(GPUI_QT_DRAWING);
        return;
    }
    case GPUI_QT_DRAWING:
        if (anim_busy(obj_dude) == 0 || elapsed_time(gpui_qt_since) > 1500) {
            gpui_qt_set(gpui_qt_release_seen ? GPUI_QT_THROW_REQ : GPUI_QT_READY);
        }
        return;
    case GPUI_QT_READY:
        // Held. A screen, a fall or a swapped hand ends it; RT throwing it the ordinary way counts as
        // the throw (the release must not throw a second one).
        if (screenUp || critter_is_prone(obj_dude)) {
            gpui_qt_set(GPUI_QT_RESTORE);
        } else if (intface_is_item_right_hand() != gpui_qt_hand) {
            gpui_qt_set(GPUI_QT_IDLE);
        } else if (combat_rt_player_attack_pending()) {
            gpui_qt_set(GPUI_QT_THROWING);
        }
        return;
    case GPUI_QT_THROW_REQ: {
        Object* inHand = gpui_qt_hand_item();
        Object* target = inHand != NULL && inHand->pid == gpui_qt_pid ? combat_rt_quick_throw_target() : NULL;
        if (target == NULL) {
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "quick-throw-none", "slot=%d pid=%d", gpui_qt_slot, gpui_qt_pid);
            gpui_qt_set(GPUI_QT_RESTORE);
            return;
        }
        combat_rt_request_throw(target);
        gpui_qt_set(GPUI_QT_THROWING);
        return;
    }
    case GPUI_QT_THROWING:
        if ((elapsed_time(gpui_qt_since) > 150 && !combat_rt_player_attack_pending() && anim_busy(obj_dude) == 0)
            || elapsed_time(gpui_qt_since) > 4000) {
            gpui_qt_set(GPUI_QT_RESTORE);
        }
        return;
    case GPUI_QT_RESTORE: {
        if (anim_busy(obj_dude) != 0 && elapsed_time(gpui_qt_since) < 2000) {
            return;
        }
        if (critter_is_prone(obj_dude)) {
            return; // never wield on the floor (gpui_weapon_equip) -- back on his feet, then
        }
        gpui_qt_set(GPUI_QT_IDLE);
        if (gpui_qt_prev_pid == gpui_qt_pid) {
            return; // it was in the hand before the draw; it stays
        }
        Object* current = gpui_qt_hand_item();
        if (gpui_qt_prev_pid == -1) {
            if (current != NULL) {
                inven_unwield(obj_dude, gpui_qt_hand);
            }
        } else {
            // The very weapon that was in the hand while it is still in the pack; only then any of its pid.
            Object* previous = NULL;
            Inventory* inventory = &(obj_dude->data.inventory);
            for (int index = 0; index < inventory->length; index++) {
                if (inventory->items[index].item == gpui_qt_prev_obj) {
                    previous = gpui_qt_prev_obj;
                    break;
                }
            }
            if (previous == NULL) {
                previous = inven_pid_is_carried_ptr(obj_dude, gpui_qt_prev_pid);
            }
            if (previous != NULL && previous != current) {
                // Its take-out paced by AGILITY like the throw it follows (compute_tpf, isQuickDraw).
                anim_quick_draw_begin(obj_dude);
                inven_wield(obj_dude, previous, gpui_qt_hand);
                anim_quick_draw_end();
            } else if (previous == NULL && current != NULL) {
                inven_unwield(obj_dude, gpui_qt_hand);
            }
        }
        intface_update_items(true);
        trace_event(TRACE_ACTION, "quick-restore", "slot=%d prev=%d id=%d", gpui_qt_slot, gpui_qt_prev_pid,
            gpui_qt_hand_item() != NULL ? gpui_qt_hand_item()->id : -1);
        return;
    }
    default:
        gpui_qt_set(GPUI_QT_IDLE);
        return;
    }
}

// Uses the best healing item carried. Drained once per press from gamepad_ui_tick.
static void gpui_use_best_heal()
{
    int count;
    Object* item = gpui_next_heal(&count);
    if (item == NULL) {
        gsound_play_sfx_file("iisxxxx1"); // nothing to heal with: refuse audibly, never silently
        return;
    }
    gpui_quick_use(item->pid);
}

// --- THE RELOAD ANIMATION (MODS "Reload animation", cfg reload_anim) ---
//
// His design, in two messages: *"tlačítko na přebíjení provede animaci při přebíjení... čím větší
// agi... tak přebije rychleji"* and *"chci aby se zahrála animace... a rychlost se odvíjí podle
// stats"*. The animation is the weapon's own STAND loop -- the thing he had been seeing as a random
// idle and reading as a reload (see anim_get_reload_anim) -- played ONCE, timed by Agility through the
// same symmetric curve the weapon swap uses (compute_tpf), with the magazine filling at its END.
//
// ⚠️ LOAD AT THE END, NOT AT THE PRESS. Loading first would make the animation decoration and the
// reload exactly as instant as vanilla, which is the opposite of "higher Agility reloads faster".
// And it is the order he asked for with the doors the same evening: "nech udělat animaci a pak se
// odemkne".
//
// ⚠️ EVERYTHING THAT TAKES THE BODY CANCELS IT, AND A CANCEL COSTS NOTHING. Moving (the movers cut a
// busy STAND exactly as they cut the idle fidget -- "when I move I want to move, not wait for an
// animation"), firing the rounds still in the gun (action_attack clears the dude), B, a hit. A cut
// sequence never reaches the callback, so nothing is loaded and no AP is spent -- the AP is charged
// at the end, with the load.

// The END of the reload's loop: this is where the magazine actually fills.
static int gpui_reload_anim_done(void* a1, void* a2)
{
    (void)a1;
    Object* weapon = (Object*)a2;

    // The points were taken at the start; finishing is what earns them, so this reload keeps them
    // whatever happens below (a refusal below gives them back explicitly).
    gpui_reload_charged = false;
    combat_rt_reload_end(obj_dude);

    // Re-checked, not trusted: a reload finishing into a gun no longer in the active hand would be
    // the one silent wrong outcome this path can have.
    if (weapon == NULL || intface_reload_target() != weapon) {
        combat_rt_refund_ap(2);
        trace_event(TRACE_INPUT, "reload-refused", "why=hand-changed");
        return 0;
    }

    if (intface_reload_weapon() == 0) {
        trace_event(TRACE_ACTION, "reload", "ap=%d anim=1", combat_rt_player_ap());
    } else {
        combat_rt_refund_ap(2);
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "reload-refused", "why=nothing-to-do-at-end");
    }

    return 0;
}

// Start the reload's loop. Returns true when the request is finished with (started, or refused for
// good), false to try again on the next tick because the dude is still busy with something real.
static bool gpui_reload_anim_start()
{
    // Already reloading (a press queued just before the loop began): nothing more to start.
    if (combat_rt_is_reloading(obj_dude)) {
        return true;
    }

    Object* weapon = intface_reload_target();
    if (weapon == NULL || !intface_can_reload_weapon()) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "reload-refused", "why=nothing-to-do");
        return true;
    }

    if (combat_rt_player_ap() < 2) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "reload-refused", "why=ap ap=%d", combat_rt_player_ap());
        return true;
    }

    // Wait out what owns the body: a knockdown, the stick (starting under a held stick would only be
    // cut again on the next frame), or a real animation -- a swing, a walk, an action. His idle
    // fidget is not real work; the reload replaces it, with the same pending-only cut the movers use.
    if (critter_is_prone(obj_dude) || combat_rt_stick_walking()) {
        return false;
    }

    if (anim_busy(obj_dude) != 0) {
        if (FID_ANIM_TYPE(obj_dude->fid) != ANIM_STAND || anim_clear_pending(obj_dude) != 0) {
            return false;
        }
    }

    // obj_dude->fid already carries the active weapon's code, so ANIM_STAND resolves to THAT weapon's
    // handling loop -- or, for a pistol, register_object_reload holsters and draws it instead (its
    // loop is a forehead wipe). The call waits for the animation (-1 = after everything before it).
    //
    // ⚠️ register_begin's refusal MUST be honoured: it refuses when ANOTHER transaction is still
    // open, and registering anyway would append the reload to somebody else's sequence.
    if (register_begin(ANIMATION_REQUEST_RESERVED) != 0) {
        return false;
    }
    register_object_reload(obj_dude);
    register_object_call(obj_dude, weapon, gpui_reload_anim_done, -1);

    // compute_tpf runs INSIDE register_end, as the first description starts -- that is the one moment
    // the Agility timing has to be visible to it.
    anim_set_reload_timing(obj_dude);
    int rc = register_end();
    anim_set_reload_timing(NULL);

    if (rc != 0) {
        return false; // the registry would not take him this tick -- try again
    }

    // ⚠️ THE 2 AP GO AT THE START (2026-10-04, said twice and the second time in capitals: "ZMÁČKNU
    // RELOAD A UŽ MI TO BERE 2 BODY, JASNÝ?"). Charging at the end looked like the pool refilling
    // through the reload and then dropping, and read as the price arriving from nowhere. Taken now,
    // and handed back by the tick if the loop never finishes (a knockdown, the player walking or
    // firing out of it) -- a reload that did not happen still costs nothing.
    combat_rt_spend_ap(2);
    gpui_reload_charged = true;
    combat_rt_reload_begin(obj_dude);

    trace_event(TRACE_ACTION, "reload-anim", "pid=%d agi=%d ap=%d",
        weapon->pid, stat_level(obj_dude, STAT_AGILITY), combat_rt_player_ap());
    return true;
}

// Drive from the per-frame safe tick (gamepad_ui_tick). Open once B has been held past the
// tap window, aim while held, execute + close on release.
static void gpui_quick_wheel(SDL_GameController* pad)
{
    static bool prevQb = false;
    static unsigned int qbDownTime = 0;
    static bool qbHandled = false; // this hold already tried to open; don't retry every frame

    bool qbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool qbEdge = qbNow && !prevQb;
    bool qbReleased = !qbNow && prevQb;
    prevQb = qbNow;

    if (qbEdge) {
        qbDownTime = get_time();
        qbHandled = false;
    }

    // Ignore the shared overlay while one of the other wheels owns it.
    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_QUICK) {
        return;
    }

    // Don't bring up (or keep up) the wheel over a modal menu / inventory / a disabled interface
    // bar -- and not while the aimed-shot picker is up.
    //
    // ⚠️ AND NOT WHILE LB IS HELD: LB+B IS THE HEAL AGAIN. Without this guard the same press would
    // heal immediately AND open this wheel 220 ms later, which is exactly how the combo behaved the
    // first time it existed. The guard is on OPENING only -- it must never close a wheel already up.
    bool lbHeld = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()
        || gpui_aim_picker_active();
    bool blockedOpen = blocked || lbHeld;

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
        } else if (qbReleased || gpui_wheel_a_edge) {
            int pid = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (pid != -1) {
                gpui_quick_use(pid);
            }
        } else {
            // LEFT stick, like the other two FACE-button wheels (X ammo, Y weapons); only the L3
            // skill wheel aims with the right, because the left thumb holds that one open. B is a FACE
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

    if (qbNow && !qbHandled && !blockedOpen && get_time() - qbDownTime >= GPUI_QUICK_HOLD_MS) {
        qbHandled = true;
        gpui_open_quick_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_QUICK;
            // Tell the tap handler this B press was a hold, so releasing it does not also
            // run the cancel chain.
            gpui_consumed_b = true;
        }
    }
}

// --- Y-hold weapon wheel (a radial_wheel instance) ---
//
// Y already means "weapon" (tap = swap hands), so a HOLD naturally means "which weapon" -- the
// same tap-vs-hold split B got for quick-use, and for the same reason: no pad button is free.
// LB+Y stays quick save; holding LB suppresses the wheel outright so the two gestures never race.
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

// AMMO IS THE LAST PAGE OF THIS WHEEL (2026-09-17). It used to be its own wheel on RB-hold, and RB
// had to give that up so it could become a HELD fire button like RT -- his call: "RB se musi
// klikat a RT se muze drzet". Folding it in here costs no new button and reuses the paging the
// weapon wheel already had, so the shoulders keep meaning "turn to the next thing in this ring".

// The ring's "dump the magazine back into the pack" entry. Not a slot index, so it cannot collide
// with one -- and NOT -1, which radial_wheel_highlight_id() already returns for "nothing aimed at".
// ⚠️ NOT -2 EITHER any more: bare hands and Unload are on the SAME page now, and they were both
// -2 while they could never meet.
#define GPUI_AMMO_UNLOAD_ID (-3)

// Ammunition segments on the weapon ring: id = BASE - <index into gpui_ammo_slot>. Far below every
// other id the ring uses, so a weapon slot index, bare hands and Unload can never be mistaken for
// a box of ammo.
#define GPUI_WHEEL_AMMO_ID_BASE (-100)

#define GPUI_AMMO_MAX 8

// THE DRUGS ARE THE LAST PAGE OF THIS RING (2026-10-04, "může držení Y quick menu pak pomocí RB nebo
// LB dostat až k itemům? ať je to fakt ez pz"). The B-hold drug wheel stays, but one hold of Y now
// reaches everything you would grab mid-fight: guns forward with RB, and the drugs ONE LB press back
// from the first page, because the paging wraps. Segment ids = BASE - <index into
// gpui_wheel_drug_pid>, far below the ammo range, and tested BEFORE it (every drug id is also
// <= GPUI_WHEEL_AMMO_ID_BASE).
//
// ⚠️ AND THE USABLE ITEMS AFTER THE DRUGS (2026-10-04, "já chci používat i normální itemy jako
// stealthboy, lockpicking v quick menu ffs"). Every MISC item the engine itself lets you Use
// (proto_action_can_use: Stealth Boy, flare, Geiger counter, motion sensor, explosives -- arming them)
// or Use On (proto_action_can_use_on: lockpicks, first aid kit, doctor's bag, rope) joins the list,
// one entry per kind, eight to a page, as many pages as it takes. A Use item works straight away, the
// way the inventory's Use does it; a Use On item arms the use-on crosshair FOR THAT ITEM
// (gmouse_set_use_on_item), so you aim a lockpick at a door without the gun leaving your hand.
#define GPUI_WHEEL_DRUG_ID_BASE (-1000)
#define GPUI_QUICK_ENTRY_MAX 32
static int gpui_wheel_drug_pid[GPUI_QUICK_ENTRY_MAX];
static int gpui_wheel_drug_qty[GPUI_QUICK_ENTRY_MAX];
static int gpui_wheel_drug_count = 0;

// Usable MISC items in the pack (see above), merged by kind, appended after `count` entries already in
// pids/counts. Returns the new total.
static int gpui_collect_usable_items(int* pids, int* counts, int count, int max)
{
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        Object* item = inventory->items[i].item;
        if (item == NULL) {
            continue;
        }
        // ⚠️ NO ARMOUR (2026-10-17, "quick menu dej pryč brnění, docela tam překáží"). It was added the
        // same day ("Quick inventory i pro armour by se hodil") and taken out again; gpui_quick_wear and
        // its dispatch stay, unreachable, should it come back.
        int type = item_get_type(item);
        if (type != ITEM_TYPE_MISC) {
            continue;
        } else if (!proto_action_can_use(item->pid) && !proto_action_can_use_on(item->pid)) {
            continue;
        }

        int at = -1;
        for (int j = 0; j < count; j++) {
            if (pids[j] == item->pid) {
                at = j;
                break;
            }
        }
        if (at != -1) {
            counts[at] += inventory->items[i].quantity;
        } else if (count < max) {
            pids[count] = item->pid;
            counts[count] = inventory->items[i].quantity;
            count++;
        }
    }
    return count;
}

// ⚠️ AN ARMED EXPLOSIVE GOES ON THE GROUND (2026-10-16, "je možné znovu najet na dynamit za účelem ho
// položit?" and "nebo do artu přidat tlačítko nastavit čas a položit?"). Arming one from the wheel
// left it ticking in the pack: picking it again only said "The timer is already ticking"
// (trace_2026-10-16_014510 s=226.32), and the only way to put it down was the inventory's Drop. Now
// both of his ideas work: the timer window's PLACE (X) arms and puts it down in one go, and picking
// the armed one on the wheel puts it down later. The dude bends to the ground (ANIM_MAGIC_HANDS_
// GROUND, frame 4 of 9 is the hand at the floor) and it lands there -- the animation first, as he
// wanted the stimpak. A body taken off that animation drops it anyway, on the spot: a man who gets
// hit while putting a bomb down lets go of it. Then vanilla's "they saw that"
// (inven_explosive_dropped_notice), exactly as the inventory's Drop answers.
//
// Price: picking the armed one again is a new reach into the pack, 2 AP like every wheel item; the
// timer window's PLACE rides on the arming's 2 AP, one action.
// (gpui_place_pending / _item / _at: declared up with the jab state, the load reset clears them.)

static bool gpui_is_armed_explosive(int pid)
{
    return pid == PROTO_ID_DYNAMITE_II || pid == PROTO_ID_PLASTIC_EXPLOSIVES_II;
}

static void gpui_place_now()
{
    gpui_place_pending = false;
    Object* item = gpui_place_item;
    gpui_place_item = NULL;
    if (item == NULL || obj_dude == NULL) {
        return;
    }

    bool carried = false;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        if (inventory->items[i].item == item) {
            carried = true;
            break;
        }
    }
    if (!carried) {
        return; // gone already (it went off, or was dropped some other way)
    }

    obj_drop(obj_dude, item);
    intface_update_items(true);
    trace_event(TRACE_ACTION, "explosive-placed", "pid=%d tile=%d after=%ums", item->pid, obj_dude->tile,
        elapsed_time(gpui_place_at));
    inven_explosive_dropped_notice();
}

// The bend's frame at the floor (a register_object_call: a cut bend is caught by gpui_place_tick).
static int gpui_place_done(void* a1, void* a2)
{
    (void)a1;
    (void)a2;
    if (gpui_place_pending) {
        gpui_place_now();
    }
    return 0;
}

static void gpui_place_explosive(Object* item, bool chargeAp)
{
    if (gpui_place_pending) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }
    if (chargeAp && combat_rt_player_ap() < gpui_pack_cost()) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "explosive-place-refused", "why=ap ap=%d", combat_rt_player_ap());
        return;
    }
    if (chargeAp) {
        combat_rt_spend_ap(gpui_pack_cost());
    }

    gpui_place_item = item;
    gpui_place_pending = true;
    gpui_place_at = get_time();

    if (!critter_is_prone(obj_dude)) {
        int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_MAGIC_HANDS_GROUND,
            (obj_dude->fid & 0xF000) >> 12, obj_dude->rotation + 1);
        int actionFrame = 0;
        CacheEntry* handle;
        Art* art = art_ptr_lock(fid, &handle);
        if (art != NULL) {
            actionFrame = art_frame_action_frame(art);
            art_ptr_unlock(handle);
        }

        if (register_begin(ANIMATION_REQUEST_RESERVED) == 0) {
            register_object_animate(obj_dude, ANIM_MAGIC_HANDS_GROUND, 0);
            register_object_call(obj_dude, NULL, gpui_place_done, actionFrame);
            if (register_end() == 0) {
                trace_event(TRACE_ACTION, "explosive-place-start", "pid=%d frame=%d ap=%d", item->pid, actionFrame,
                    combat_rt_player_ap());
                return;
            }
        }
    }

    gpui_place_now(); // on the floor already, or the bend could not register: put it down at once
}

// From the safe tick: a bend taken off before its frame drops the explosive where he stands.
static void gpui_place_tick()
{
    if (!gpui_place_pending || obj_dude == NULL) {
        return;
    }
    if (elapsed_time(gpui_place_at) < 200 || FID_ANIM_TYPE(obj_dude->fid) == ANIM_MAGIC_HANDS_GROUND) {
        return;
    }
    trace_event(TRACE_ACTION, "explosive-place-cut", "anim=%d", FID_ANIM_TYPE(obj_dude->fid));
    gpui_place_now();
}

// ⚠️ ARMOUR FROM THE WHEEL (2026-10-17, "Quick inventory i pro armour by se hodil"; his price "4 AP jako
// vanilla"). Picking an armour puts it on, picking the one he wears takes it off. The price is vanilla's
// for changing armour in a fight -- opening the inventory, 4 AP, one less per Quick Pockets rank -- and
// out of a fight the pool sits full, so it costs nothing there in effect.
//
// Done the way vanilla changes a critter's armour outside the inventory screen (item_remove_mult's worn
// branch): the WORN flags, adjust_ac (inven_wield skips it for the dude -- the inventory screen does
// it), then the body art. obj_change_fid keeps the pose and frame he is in, so a free-movement stride
// carries on in the new suit; a body owned by another animation is refused rather than repainted
// under it.
static void gpui_quick_wear(int pid)
{
    int apCost = gpui_pack_cost();
    if (combat_rt_player_ap() < apCost) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "quick-wear-refused", "pid=%d why=ap ap=%d cost=%d", pid, combat_rt_player_ap(), apCost);
        return;
    }
    if (critter_is_prone(obj_dude) || anim_busy_blocking(obj_dude) != 0) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_ACTION, "quick-wear-refused", "pid=%d why=busy anim=%d", pid, FID_ANIM_TYPE(obj_dude->fid));
        return;
    }

    Proto* dudeProto;
    if (proto_ptr(obj_dude->pid, &dudeProto) == -1) {
        return;
    }
    int baseFid = dudeProto->fid & 0xFFF; // the suit he wears under everything

    Object* worn = inven_worn(obj_dude);
    Object* item = NULL;
    if (worn != NULL && worn->pid == pid) {
        worn->flags &= ~OBJECT_WORN; // taking it off: back in the pack
        adjust_ac(obj_dude, worn, NULL);
    } else {
        Inventory* inventory = &(obj_dude->data.inventory);
        for (int i = 0; i < inventory->length; i++) {
            Object* candidate = inventory->items[i].item;
            if (candidate != NULL && candidate->pid == pid && (candidate->flags & OBJECT_WORN) == 0) {
                item = candidate;
                break;
            }
        }
        if (item == NULL) {
            gsound_play_sfx_file("iisxxxx1");
            return;
        }
        if (worn != NULL) {
            worn->flags &= ~OBJECT_WORN;
        }
        item->flags |= OBJECT_WORN;
        adjust_ac(obj_dude, worn, item);

        int armorFid = stat_level(obj_dude, STAT_GENDER) == GENDER_FEMALE ? item_ar_female_fid(item) : item_ar_male_fid(item);
        if (armorFid != -1) {
            baseFid = armorFid;
        }
    }

    Rect rect;
    int fid = art_id(OBJ_TYPE_CRITTER, baseFid, FID_ANIM_TYPE(obj_dude->fid), (obj_dude->fid & 0xF000) >> 12,
        (obj_dude->fid & 0x70000000) >> 28);
    obj_change_fid(obj_dude, fid, &rect);
    tile_refresh_rect(&rect, obj_dude->elevation);

    combat_rt_spend_ap(apCost);
    intface_update_ac(false); // the bar's AC box; false = no blocking roll (see intface_update_hit_points)
    gsound_play_sfx_file("iputdown");
    trace_event(TRACE_ACTION, "quick-wear", "pid=%d %s cost=%d ap=%d", pid, item != NULL ? "on" : "off", apCost,
        combat_rt_player_ap());
}

// USE an item on yourself, exactly the inventory's Use (inventry.cc, GAME_MOUSE_ACTION_MENU_ITEM_USE):
// take one out of the stack, protinst_use_item it, destroy it if that consumed it, otherwise put it
// back. The pack's price in a fight (gpui_pack_cost), refused when the pool cannot pay -- the same
// price the drugs have.
static void gpui_quick_use_item(int pid)
{
    int apCost = gpui_pack_cost();
    if (combat_rt_player_ap() < apCost) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    Object* item = NULL;
    Inventory* inventory = &(obj_dude->data.inventory);
    for (int i = 0; i < inventory->length; i++) {
        if (inventory->items[i].item != NULL && inventory->items[i].item->pid == pid) {
            item = inventory->items[i].item;
            break;
        }
    }
    if (item == NULL) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    // Already armed: put it down (see gpui_place_pending).
    if (gpui_is_armed_explosive(pid) && (item->flags & OBJECT_USED) != 0) {
        gpui_place_explosive(item, true);
        return;
    }

    if (item_remove_mult(obj_dude, item, 1) != 0) {
        return;
    }

    int rc = protinst_use_item(obj_dude, item);
    if (rc == 1) {
        obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
        obj_destroy(item);
    } else {
        item_add_force(obj_dude, item, 1);
        // The timer window's PLACE: armed and put down in one action (its AP is the arming's).
        if (inven_set_timer_place_requested() && gpui_is_armed_explosive(item->pid)) {
            gpui_place_explosive(item, false);
        }
    }

    if (rc != -1) {
        combat_rt_spend_ap(apCost);
    }
    trace_event(TRACE_ACTION, "quick-use-item", "pid=%d rc=%d cost=%d ap=%d", pid, rc, apCost, combat_rt_player_ap());
    intface_update_items(true);
}

// USE ON: arm the engine's use-on crosshair for THIS item (not the hand item) and put the crosshair in
// front of the dude, exactly like holding A with the item in hand does (gpui_useon_arm). The A that
// confirms it lands in gmouse.cc, which reads the item back by PID and charges the AP there.
static void gpui_quick_use_on_item(int pid)
{
    gmouse_set_cursor(MOUSE_CURSOR_USE_CROSSHAIR);
    gmouse_3d_set_mode(GAME_MOUSE_MODE_USE_CROSSHAIR);
    if (gmouse_3d_get_mode() != GAME_MOUSE_MODE_USE_CROSSHAIR) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }
    gmouse_set_use_on_item(pid);
    gpui_useon_park_cursor();
    gpui_cursor_show_time = get_time();
    gsound_play_sfx_file("ib1p1xx1");
    trace_event(TRACE_ACTION, "quick-use-on", "pid=%d", pid);
}

// The ammo objects the open wheel is showing -- one per distinct proto, pointing at the first stack
// of that type. Object*, because item_w_reload needs the actual object.
static Object* gpui_ammo_slot[GPUI_AMMO_MAX];
static int gpui_ammo_count = 0;

// TOTAL rounds of each slot's proto across the whole pack -- every box, times every stack. The
// slot Object above is only the one the reload will draw from; it is not what the player is
// carrying, and showing its magazine as the count was the "wtf, that is not how much I have" bug.
static int gpui_ammo_rounds[GPUI_AMMO_MAX];

// The weapon the wheel was opened for, so a hand swap mid-hold cannot reload the wrong gun.
static Object* gpui_ammo_weapon = NULL;

// Defined below -- the weapon ring carries the ammunition entries and the two actions that go
// with them.
static bool gpui_ammo_collect(Object** weaponOut, bool* canUnloadOut);
static void gpui_ammo_unload();
static void gpui_ammo_load(int index);

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
    // appeared twice. Same wheel, same inventory, two different rules -- "ukazujeÃÂÃÂ¡ jen jednu pistol
    // a mÃÂÃÂ¡m 4 v inventory". Collapsing by proto makes it one rule.
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

    // Drugs first (at most GPUI_QUICK_MAX, the B wheel's own list), then every usable item.
    {
        int drugPids[GPUI_QUICK_MAX];
        int drugCounts[GPUI_QUICK_MAX];
        int drugs = gpui_collect_drugs(drugPids, drugCounts);
        for (int i = 0; i < drugs; i++) {
            gpui_wheel_drug_pid[i] = drugPids[i];
            gpui_wheel_drug_qty[i] = drugCounts[i];
        }
        gpui_wheel_drug_count = gpui_collect_usable_items(gpui_wheel_drug_pid, gpui_wheel_drug_qty,
            drugs, GPUI_QUICK_ENTRY_MAX);
    }

    if (gpui_weapon_count == 0 && gpui_wheel_drug_count == 0) {
        // Nothing to choose from -- refuse audibly rather than flashing an empty ring (the empty-gun
        // lesson: never fail silently). Checked before the bare-hands entry is added: a ring whose
        // only option is "put away the nothing you are holding" is not a choice. A carried drug IS
        // one, now that the drugs have a page here.
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    // ⚠️ THE FRONT OF THE RING IS FIXED, AND THAT IS THE POINT. His call: "bude to Y a prvni
    // ikonky vzdycky budou unarmed, unload, a type of ammos". Ammunition used to be a PAGE of its
    // own at the far end -- hold Y, page past every gun you own, then choose -- which is a lot of
    // gestures for something you do with a fight going on. It sits at the front now, and the guns
    // queue up behind it.
    Object* ammoWeapon = NULL;
    bool canUnload = false;
    gpui_ammo_collect(&ammoWeapon, &canUnload);

    // Bare hands and Unload are ALWAYS the first two, whether they apply or not: a radial is only
    // worth something while a given choice sits in the same place every time, so Unload greys out
    // instead of vanishing and shuffling everything behind it one seat along.
    int ammoShown = gpui_ammo_count;
    if (ammoShown > GPUI_WEAPON_PER_PAGE - 3) {
        // Leave at least one weapon seat on the first page. No FO1 weapon takes anywhere near this
        // many kinds of round; the clamp is here so the arithmetic below cannot go negative.
        ammoShown = GPUI_WEAPON_PER_PAGE - 3;
    }

    int head = 2 + ammoShown;
    int gunsOnFirstPage = GPUI_WEAPON_PER_PAGE - head;

    // Page 0 carries that head plus whatever guns still fit; every page after it is guns alone.
    int rest = gpui_weapon_count - gunsOnFirstPage;
    int gunPages = 1;
    if (rest > 0) {
        gunPages += (rest + GPUI_WEAPON_PER_PAGE - 1) / GPUI_WEAPON_PER_PAGE;
    }

    // ...and the drugs and usable items after all of them, eight to a page.
    int quickPages = (gpui_wheel_drug_count + GPUI_WEAPON_PER_PAGE - 1) / GPUI_WEAPON_PER_PAGE;
    gpui_weapon_pages = gunPages + quickPages;

    int totalPages = gpui_weapon_pages;

    if (gpui_weapon_page < 0) {
        gpui_weapon_page = totalPages - 1; // wrap backwards off page 0
    } else if (gpui_weapon_page >= totalPages) {
        gpui_weapon_page = 0; // wrap forwards off the last page
    }

    if (gpui_weapon_page >= gunPages) {
        int firstEntry = (gpui_weapon_page - gunPages) * GPUI_WEAPON_PER_PAGE;
        int entries = gpui_wheel_drug_count - firstEntry;
        if (entries > GPUI_WEAPON_PER_PAGE) {
            entries = GPUI_WEAPON_PER_PAGE;
        }

        RadialSegment quickSegs[GPUI_WEAPON_PER_PAGE];
        memset(quickSegs, 0, sizeof(quickSegs));
        // Same filler for drugs and items: name, count, effect line (empty for a non-drug), prose,
        // icon. Ids keep the ABSOLUTE entry index, so the selection needs no page arithmetic.
        gpui_fill_drug_segments(quickSegs, gpui_wheel_drug_pid + firstEntry, gpui_wheel_drug_qty + firstEntry,
            entries, GPUI_WHEEL_DRUG_ID_BASE - firstEntry);

        char quickTitle[32];
        snprintf(quickTitle, sizeof(quickTitle), "QUICK USE %d/%d", gpui_weapon_page + 1, totalPages);

        RadialWheelArt quickArt;
        radial_wheel_open(quickSegs, entries, quickTitle,
            gpui_wheel_art_for_icons(&quickArt, gpui_icons_held), -1, -1, false);
        radial_wheel_set_side_hints("LB", "RB");

        if (!radial_wheel_is_open()) {
            gpui_icons_release();
        }
        return;
    }

    bool firstPage = gpui_weapon_page == 0;
    int first = firstPage ? 0 : gunsOnFirstPage + (gpui_weapon_page - 1) * GPUI_WEAPON_PER_PAGE;
    int room = firstPage ? gunsOnFirstPage : GPUI_WEAPON_PER_PAGE;

    int shown = gpui_weapon_count - first;
    if (shown > room) {
        shown = room;
    }
    if (shown < 0) {
        shown = 0;
    }

    RadialSegment segs[GPUI_WEAPON_PER_PAGE];
    memset(segs, 0, sizeof(segs));

    int n = 0;

    if (firstPage) {
        // 1. BARE HANDS. Going unarmed had no route on the pad at all before this (Y swaps hands;
        // the wheel could only ever put something INTO one), and it matters more than it sounds:
        // unarmed and melee skills, sneaking past people who react to a drawn gun, talking to
        // someone without a rifle pointed at them.
        // ⚠️ NOT id -1: radial_wheel_highlight_id() returns -1 for "nothing aimed at", so an id of
        // -1 could not be told apart from letting go while aiming at nothing.
        segs[n].id = GPUI_WEAPON_UNARMED_ID;
        segs[n].enabled = true;
        segs[n].label = skill_name(SKILL_UNARMED);
        segs[n].value = -1;
        gpui_icon_from_interface(segs, n, GPUI_UNARMED_ART_NUM);
        n++;

        // 2. UNLOAD, in its seat whether or not there is anything in the gun to dump.
        segs[n].id = GPUI_AMMO_UNLOAD_ID;
        segs[n].enabled = canUnload;
        segs[n].label = "Unload";
        segs[n].value = canUnload ? item_w_curr_ammo(ammoWeapon) : -1;
        gpui_icon_unload_glyph(segs, n);
        n++;

        // 3. ...and then every kind of round the gun in hand takes. The badge is the whole pack's
        // count of that type, not the box this slot happens to point at.
        for (int i = 0; i < ammoShown; i++) {
            Object* ammo = gpui_ammo_slot[i];
            segs[n].id = GPUI_WHEEL_AMMO_ID_BASE - i;
            segs[n].enabled = true;
            segs[n].label = object_name(ammo);
            segs[n].value = gpui_ammo_rounds[i];
            gpui_ammo_detail(ammo, gpui_detail_buf[n], sizeof(gpui_detail_buf[n]));
            segs[n].detail = gpui_detail_buf[n][0] != 0 ? gpui_detail_buf[n] : NULL;
            segs[n].description = object_description(ammo);
            gpui_icon_from_pid(segs, n, ammo->pid);
            n++;
        }
    }

    for (int i = 0; i < shown; i++) {
        Object* item = gpui_weapon_slot[first + i];

        // id is the ABSOLUTE slot index, so gpui_weapon_equip needs no page arithmetic.
        segs[n].id = first + i;
        segs[n].enabled = true;

        segs[n].label = object_name(item);

        // Rounds in the magazine, for weapons that take ammo at all. A melee weapon has no count
        // worth showing, so it gets no badge instead of a meaningless 0.
        segs[n].value = item_w_max_ammo(item) > 0 ? item_w_curr_ammo(item) : -1;

        gpui_weapon_detail(item, gpui_detail_buf[n], sizeof(gpui_detail_buf[n]));
        segs[n].detail = gpui_detail_buf[n][0] != 0 ? gpui_detail_buf[n] : NULL;
        segs[n].description = object_description(item);

        gpui_icon_from_pid(segs, n, item->pid);
        n++;
    }

    // The page number goes back in the title (shown in the hub while nothing is aimed at), and the
    // BUTTONS go to the panel's outer corners, where they stay readable while you choose -- one
    // column of hub text was too much to read at a glance.
    char title[32];
    if (totalPages > 1) {
        snprintf(title, sizeof(title), "WEAPONS %d/%d", gpui_weapon_page + 1, totalPages);
    } else {
        snprintf(title, sizeof(title), "WEAPONS");
    }

    RadialWheelArt art;
    radial_wheel_open(segs, n, title,
        gpui_wheel_art_for_icons(&art, gpui_icons_held), -1, -1, false);

    // LB pages back, RB forward (both wrap). LB spent a while unable to page -- see gpui_weapon_wheel
    // -- and the hint said "RB" alone for exactly that long.
    if (totalPages > 1) {
        radial_wheel_set_side_hints("LB", "RB");
    }

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

    // Not from the floor, like the reload (gpui_reload_anim_start): inven_wield/inven_unwield
    // register the put-away/take-out as a RESERVED sequence, which takes the body off its knockdown
    // and stands it up while the reaction layer still holds it down -- the standup then plays from
    // the lying pose and he goes down a second time (see intface_dude_is_down, 2026-10-13).
    if (critter_is_prone(obj_dude)) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "weapon-equip-refused", "why=down");
        return;
    }

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

    // ⚠️ IN A FIGHT IT COSTS WHAT VANILLA'S WAY TO DO IT COSTS (2026-10-19, his pick "Jako inventář: 4 AP
    // (− QP)"). Vanilla changes weapons through the inventory -- 4 AP to open it, one less per rank of
    // Quick Pockets -- and the wheel is only a quicker way there; it was free, the put-away and the
    // draw were its only price. Swapping hands (Y) stays free, as in vanilla. Out of a fight the pool
    // is full and nothing is taken (combat_rt_spend_ap).
    int apCost = gpui_pack_cost();
    if (combat_rt_player_ap() < apCost) {
        gsound_play_sfx_file("iisxxxx1");
        trace_event(TRACE_INPUT, "weapon-equip-refused", "why=ap ap=%d cost=%d", combat_rt_player_ap(), apCost);
        return;
    }

    if (item == NULL) {
        // The bare-hands entry. inven_unwield plays the put-away animation itself when the hand
        // being emptied is the one the interface bar shows, so the swap reads on screen just like
        // a wield does.
        combat_rt_spend_ap(apCost);
        inven_unwield(obj_dude, hand);
        intface_update_items(true);
        trace_event(TRACE_ACTION, "weapon-equip", "pid=-1 cost=%d ap=%d", apCost, combat_rt_player_ap());
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

    combat_rt_spend_ap(apCost);
    inven_wield(obj_dude, item, hand);
    intface_update_items(true);
    trace_event(TRACE_ACTION, "weapon-equip", "pid=%d cost=%d ap=%d", item->pid, apCost, combat_rt_player_ap());
}

// Drive from the per-frame safe tick (gamepad_ui_tick), exactly like the quick-use wheel.
static void gpui_weapon_wheel(SDL_GameController* pad)
{
    static bool prevWy = false;
    static unsigned int wyDownTime = 0;
    static bool wyHandled = false; // this hold already tried to open; don't retry every frame

    static bool prevRB = false;
    static bool prevLB = false;

    bool wyNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool wyEdge = wyNow && !prevWy;
    bool wyReleased = !wyNow && prevWy;
    prevWy = wyNow;

    // Page step for the open wheel, edge-triggered so one press is exactly one page: RB forward, LB
    // BACK. gpui_open_weapon_wheel wraps the page index at both ends.
    //
    // ⚠️ LB PAGES BACK AGAIN (2026-10-04, "quickmenu nejde LB zpátky jen RB"). It was taken away on
    // 2026-08-29, when LB was the run/modifier shoulder that blocked -- and so CLOSED -- every wheel.
    // Since the 2026-09-16 layout LB is reload (tap) / limb picker (hold), and neither belongs inside
    // an open wheel, so the button is free here. The press is marked spent (gpui_consumed_lb) so that
    // letting go of it does not also reload, and so a hold that outlives the wheel cannot open the
    // limb picker behind it.
    bool rbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    int pageStep = (rbNow && !prevRB) ? 1 : ((lbNow && !prevLB) ? -1 : 0);
    prevRB = rbNow;
    prevLB = lbNow;

    if (wyEdge) {
        wyDownTime = get_time();
        wyHandled = false;
        gpui_weapon_page = 0; // a fresh summon always starts on the first page
    }

    // Ignore the shared overlay while one of the other wheels owns it.
    if (radial_wheel_is_open() && gpui_wheel_owner != GPUI_WHEEL_WEAPON) {
        return;
    }

    // Same blocks as the quick-use wheel, plus LB: LB+Y is quick save, so while LB is held Y is
    // not ours at all.
    // ⚠️ THE MODIFIER GATES *OPENING* ONLY -- IT MUST NOT CLOSE A WHEEL THAT IS ALREADY UP.
    // RB is the modifier now, and RB is also how you PAGE an open wheel. Folding it into `blocked`
    // (which closes the overlay) would have meant that reaching for the next page shut the wheel
    // instead of turning it. The two roles never actually collide: while the wheel is up the page
    // step owns RB, and while it is down RB is free to be half of a shortcut.
    bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()
        || gpui_aim_picker_active();
    // A Y that LT+Y spent on the firing mode (gpui_button_shortcuts) is not a wheel hold.
    bool blockedOpen = blocked || gpui_consumed_y;

    if (radial_wheel_is_open()) {
        if (blocked) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            if (rbNow) {
                gpui_consumed_rb = true;
            }
        } else if (wyReleased || gpui_wheel_a_edge) {
            int index = radial_wheel_highlight_id();
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
            gpui_icons_release();
            // ⚠️ RB MAY STILL BE DOWN -- you page with it. Spend this press so letting go of Y is
            // not read as anything else. (It was the held aimed-shot button until 2026-10-09; it is
            // the heal now, which acts on a fresh PRESS only, but the rule stands: take the state
            // over when a screen closes, do not read it as a new press.)
            if (rbNow) {
                gpui_consumed_rb = true;
            }
            // Backing out with nothing aimed at is just backing out -- a ring you opened to LOOK
            // at what you are carrying must never fall out into an action. The id says which kind
            // of segment it was: Unload and the ammunition sit far below any weapon slot index.
            if (index == GPUI_AMMO_UNLOAD_ID) {
                gpui_ammo_unload();
            } else if (index <= GPUI_WHEEL_DRUG_ID_BASE) {
                // A quick-use entry -- BEFORE the ammo test, which every one of these ids also passes.
                // Drug -> take it; Use item -> use it now; Use On item -> arm the crosshair for it.
                int d = GPUI_WHEEL_DRUG_ID_BASE - index;
                if (d >= 0 && d < gpui_wheel_drug_count) {
                    int pid = gpui_wheel_drug_pid[d];
                    Proto* proto = NULL;
                    bool isDrug = proto_ptr(pid, &proto) != -1 && proto != NULL
                        && proto->item.type == ITEM_TYPE_DRUG;
                    bool isArmor = proto != NULL && proto->item.type == ITEM_TYPE_ARMOR;
                    if (isArmor) {
                        gpui_quick_wear(pid);
                    } else if (isDrug) {
                        gpui_quick_use(pid);
                    } else if (proto_action_can_use(pid)) {
                        gpui_quick_use_item(pid);
                    } else {
                        gpui_quick_use_on_item(pid);
                    }
                }
            } else if (index <= GPUI_WHEEL_AMMO_ID_BASE) {
                gpui_ammo_load(GPUI_WHEEL_AMMO_ID_BASE - index);
            } else if (index != -1) {
                gpui_weapon_equip(index);
            }
        } else if (pageStep != 0) {
            // RB = next page, LB = previous, wrapping at both ends (gpui_open_weapon_wheel wraps).
            // Reopening rebuilds the ring for the new page; the icon slots are released and
            // re-locked by the open itself, so no leak.
            if (pageStep < 0) {
                gpui_consumed_lb = true; // a page turn, not a reload and not the start of a picker hold
            }
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

    if (wyNow && !wyHandled && !blockedOpen && get_time() - wyDownTime >= GPUI_QUICK_HOLD_MS) {
        wyHandled = true;
        gpui_open_weapon_wheel();
        if (radial_wheel_is_open()) {
            gpui_wheel_owner = GPUI_WHEEL_WEAPON;
            // Tell the tap handler this Y press was a hold, so releasing it does not also
            // use a stimpack.
            gpui_consumed_y = true;
        }
    }
}

// --- the ammunition actions, which now live at the front of the WEAPON ring (Y-hold) ---
//
// Fallout 1 has no ammo-type switch of its own: vanilla's only way to choose is to drag a specific
// box onto the weapon in the inventory, and `item_w_try_reload` (what a tap of LB runs) just grabs
// the first compatible box it finds. So on a pad there was no way to answer "how do I pick AP over
// JHP?" at all.
//
// This started as its own wheel on X-hold, became the weapon wheel's last PAGE when RB had to give
// up a button, and is now the first thing in that wheel -- three homes, each time because the ring
// it was in needed the button more than it needed the page. The two actions never changed.



// Gather what the gun in hand can be fed, and whether it has anything in it to dump. Fills the
// three statics below and says nothing out loud: this is one part of a ring that is opening
// anyway, so "no ammunition here" is an absence of segments, not a refusal.
static bool gpui_ammo_collect(Object** weaponOut, bool* canUnloadOut)
{
    gpui_ammo_count = 0;
    gpui_ammo_weapon = NULL;

    if (weaponOut != NULL) {
        *weaponOut = NULL;
    }
    if (canUnloadOut != NULL) {
        *canUnloadOut = false;
    }

    Object* weapon = NULL;
    if (intface_get_current_item(&weapon) != 0 || weapon == NULL
        || item_get_type(weapon) != ITEM_TYPE_WEAPON || item_w_max_ammo(weapon) <= 0) {
        // Bare hands, a melee weapon, or anything that does not take ammo.
        return false;
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

        // ⚠️ COUNT THE ROUNDS, NOT THE BOX YOU HAPPENED TO FIND FIRST. The badge used to show
        // item_w_curr_ammo of this one Object, so a pack holding six full boxes and one with three
        // rounds left announced whichever the inventory listed first -- "3" while you were carrying
        // 123. Ammo also STACKS, so a single entry can be many boxes: rounds are per-box times the
        // stack quantity, and every entry of the same proto adds into the same total. Same sum
        // gpui_weapon_detail already prints for the gun's ammo line, for the same reason.
        int rounds = gpui_ammo_stack_rounds(ammo, inventory->items[i].quantity);

        // TEMP DIAG (2026-08-26, STRIP with ammo-count): kept until the corrected sum is confirmed
        // in play. It prints the raw pieces, so a badge that is still wrong says which one lies.
        trace_event(TRACE_INPUT, "ammo-count", "pid=%d per=%d boxes=%d rounds=%d | gun cur=%d max=%d",
            ammo->pid, item_w_curr_ammo(ammo), inventory->items[i].quantity, rounds,
            item_w_curr_ammo(weapon), item_w_max_ammo(weapon));

        bool dup = false;
        for (int j = 0; j < gpui_ammo_count; j++) {
            if (gpui_ammo_slot[j]->pid == ammo->pid) {
                gpui_ammo_rounds[j] += rounds;
                dup = true;
                break;
            }
        }
        if (!dup) {
            gpui_ammo_rounds[gpui_ammo_count] = rounds;
            gpui_ammo_slot[gpui_ammo_count++] = ammo;
        }
    }

    // UNLOAD is offered whenever the gun actually has rounds in it. Deliberately reported
    // SEPARATELY from the box list: dumping the magazine is exactly what you want when you are
    // carrying no compatible ammunition at all -- wrong caliber picked up, or you want the rounds
    // back before dropping the gun -- so it must not depend on that list having anything in it.
    // Vanilla can only do this by right-clicking the weapon in the inventory.
    if (weaponOut != NULL) {
        *weaponOut = weapon;
    }
    if (canUnloadOut != NULL) {
        *canUnloadOut = item_w_can_unload(weapon) != 0;
    }

    return true;
}

// Dump the magazine back into the pack. Mirrors the inventory's own UNLOAD action (inventry.cc):
// item_w_unload hands back ONE box at a time and leaves any overflow in the weapon, so it has to be
// looped -- a weapon holding more rounds than one box's capacity would otherwise be half-emptied.
static void gpui_ammo_unload()
{
    // ⚠️ AP FIRST, before anything moves. Both of these charge unconditionally at the bottom
    // now, so an empty pool has to bounce here or it would go negative -- and refusing halfway
    // would leave ammo half-moved. Same rule as the tap-X reload and gpui_quick_use.
    if (combat_rt_player_ap() < 2) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

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
    // hand you a cost-free way to stall in a firefight. REFUSES when the pool cannot pay, matching
    // the tap-X reload (which always did) and gpui_quick_use (which stopped being an exception on
    // 2026-08-29). Checked before anything is moved, so a refusal leaves the gun untouched.
    combat_rt_spend_ap(2);
    intface_update_items(true);

    // ⚠️ SAY THAT IT HAPPENED. Every refusal in here has always buzzed, but the case that WORKED
    // made no sound at all ("chybi zvuk pri unload") -- so the only outcome without feedback was
    // the successful one. Vanilla's inventory unload is silent too, which is why it was never
    // noticed there. It is the weapon's own READY sound, the same one a tap of LB plays through
    // intface_reload_weapon: rounds moving into the gun or out of it is the same handling.
    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon,
        HIT_MODE_RIGHT_WEAPON_PRIMARY, NULL);
    gsound_play_sfx_file(sfx);
}

// Load the chosen box. Deliberately the same sequence item_w_try_reload runs (reload, destroy the
// box when it is spent) -- only the CHOICE of box differs, so partial boxes, capacity and the
// "already full" refusal all behave exactly as vanilla.
static void gpui_ammo_load(int index)
{
    // ⚠️ AP FIRST, before anything moves. Both of these charge unconditionally at the bottom
    // now, so an empty pool has to bounce here or it would go negative -- and refusing halfway
    // would leave ammo half-moved. Same rule as the tap-X reload and gpui_quick_use.
    if (combat_rt_player_ap() < 2) {
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

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
    // ⚠️ FILL THE GUN, DO NOT EMPTY ONE BOX. Vanilla's reload draws from exactly ONE box and stops
    // (item.cc:1244) -- so a box worn down to 2 rounds put 2 in the chamber and made you reload
    // again, and again, while 96 rounds sat in the pack. The partial box is not an edge case
    // either: every reload leaves one, because the gun takes what it needs and the remainder stays
    // behind. That is tolerable in an inventory screen where each reload is a deliberate click; on
    // a wheel you open mid-fight it is the whole complaint.
    //
    // So: keep pulling from boxes of this same proto until the gun is FULL or the pack runs out.
    // Still ONE action and one AP charge -- the player asked for a full magazine, not for n boxes.
    int pid = ammo->pid;
    bool any = false;

    for (;;) {
        int rc = item_w_reload(weapon, ammo);
        if (rc == -1) {
            break; // gun already full of this type, or this box will not go in
        }

        any = true;

        if (rc != 0) {
            break; // rounds left over in the box, which means the gun took all it could hold
        }

        // Box consumed. obj_destroy takes the Object with it, so the pointer dies here -- the next
        // box has to be looked up again rather than stepped to.
        obj_destroy(ammo);
        ammo = NULL;

        if (item_w_curr_ammo(weapon) >= item_w_max_ammo(weapon)) {
            break;
        }

        int idx = -1;
        for (;;) {
            Object* next = inven_find_type(obj_dude, ITEM_TYPE_AMMO, &idx);
            if (next == NULL) {
                break;
            }
            if (next->pid == pid && item_w_can_reload(weapon, next)) {
                ammo = next;
                break;
            }
        }

        if (ammo == NULL) {
            break; // no more of this ammo -- a partly filled gun is the right outcome
        }
    }

    if (!any) {
        gsound_play_sfx_file("iisxxxx1"); // already full of this type
        return;
    }

    // Vanilla charges 2 AP for a reload; mirror the tap-X path, refusal included.
    combat_rt_spend_ap(2);
    intface_update_items(true);

    // ⚠️ SAY THAT IT HAPPENED. Every refusal in here has always buzzed, but the case that WORKED
    // made no sound at all ("chybi zvuk pri unload") -- so the only outcome without feedback was
    // the successful one. Vanilla's inventory unload is silent too, which is why it was never
    // noticed there. It is the weapon's own READY sound, the same one a tap of LB plays through
    // intface_reload_weapon: rounds moving into the gun or out of it is the same handling.
    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon,
        HIT_MODE_RIGHT_WEAPON_PRIMARY, NULL);
    gsound_play_sfx_file(sfx);
}

// NOTE: gpui_ammo_wheel() used to sit here -- the RB tap/hold handler that opened the ammo ring.
// It is gone, and with it RB's tap/hold split. The ammo ring lives on as the last PAGE of the
// weapon wheel; RB was a held aimed-shot button, and since 2026-10-09 it is the heal.

// --- On-body aimed-shot picker (green body-part markers on the target) ---
//
// ⚠️ THE 2026-09-16 LAYOUT SPLIT AIMING ACROSS TWO BUTTONS, and that is what finally killed the
// tap-vs-hold problem here. LT now does ONE thing: hold it and the picker is up. GREEN markers are
// drawn directly on the target's on-screen sprite, one per called-shot body part, the selected one
// WHITE; the RIGHT stick moves the selection over the body; RT fires at it, as many times as you
// like while LT is down; releasing LT just puts the picker away, and B cancels out of it.
// REPEATING the last part is RB, on its own button, so nothing here has to tell a tap from a hold
// any more -- no 220 ms window, and no way for a quick press to be promoted into a hold. Since
// 2026-09-17 RB is HELD rather than clicked, the same as RT.
//
// Part positions are APPROXIMATE fractions of the critter's silhouette -- the world sprite has no
// real per-part coordinates -- good enough to read as "aim here", rougher on non-humanoids.

// NO OVERLAY WINDOW. Three attempts at a see-through GNW window all rendered a black rectangle
// over the enemy: WINDOW_TRANSPARENT is honoured only while GNW double-buffers (gnw.cc:869/887),
// turning buffering on made GNW present the never-written all-zero `screen_buffer` instead, and
// seeding that buffer with a full refresh still left it black. So the marks are now painted
// DIRECTLY into the map window's own buffer (display_win) -- the same surface the world is drawn
// on, so there is nothing to make transparent: pixels we don't touch are simply the game. The
// previous frame's marks are wiped by asking the engine to repaint that patch of world
// (tile_refresh_rect), which is exactly how the engine erases its own transient drawing.
static int gpui_aim_last_part = HIT_LOCATION_HEAD; // part the slot's AIMED mode fires at

// ⚠️ THE BOUND PART IS REMEMBERED (2026-10-17, "po každým spuštění mám aimed shots head, já mám
// neustále eyes, ať si to prostě pamatuje"). It lived only in this variable, so every launch started
// over on the head. Now every bind writes it to fallout.cfg on the spot (a crash must not lose it)
// and gamepad_ui_init reads it back -- one choice for the player, whichever save is loaded.
static void gpui_aim_part_store(int part)
{
    gpui_aim_last_part = part;
    int saved = -1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AIM_PART_KEY, &saved);
    if (saved != part) {
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AIM_PART_KEY, part);
        gconfig_save();
    }
}

static void gpui_aim_part_load()
{
    int part = HIT_LOCATION_HEAD;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AIM_PART_KEY, &part);
    if (part >= HIT_LOCATION_HEAD && part < HIT_LOCATION_UNCALLED) {
        gpui_aim_last_part = part;
    }
}

// The part bound in the LB called-shot window. combat_rt reads it for every attack made while the
// weapon slot is on its AIMED mode (rt_player_called_loc).
int gamepad_ui_aim_part()
{
    return gpui_aim_last_part;
}
static bool gpui_aim_body_on = false;              // on-body picker currently up

// True from the instant the aim trigger goes down until it comes up -- i.e. the whole GESTURE,
// not just the part of it where the picker is drawn.
//
// ⚠️ THE GAP BETWEEN THE TWO IS WHERE THE CURSOR WAS ESCAPING. The picker only appears after
// GPUI_AIM_TAP_MS (220 ms) of holding, and the right-stick cursor was gated on gpui_aim_body_on
// alone -- so for that fifth of a second the stick still drove the WORLD cursor. Anyone who starts
// turning the stick as they pull the trigger, which is the natural way to do it, shoved the hex
// cursor across the map first and then wondered why the body markers were not tracking: "objevi se
// kurzor a hybu pak s kurzorem". Claiming the stick for the whole gesture closes it.
static bool gpui_aim_gesture = false;

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

// True for the WHOLE aim gesture -- from the instant LB goes down, not just once the markers
// are drawn 220 ms later. Only the ATTACK reads this: a pull of RT inside that opening window
// would otherwise fire an ordinary unaimed shot at whatever the auto-target picked, which is
// never what someone holding the aim button meant. Movement deliberately does NOT read it --
// gating the feet on the gesture would freeze the dude for 220 ms on every LB tap (reload).
bool gamepad_ui_aim_gesture_active()
{
    return gpui_aim_gesture || gpui_aim_body_on;
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
    // Centres of the bands the masks are cut from (head 0-17, torso to 45, groin to 57, legs below),
    // so the plain ring lands where the outline would have been when a part cannot be masked.
    case HIT_LOCATION_RIGHT_ARM: *fx = 0.15f; *fy = 0.36f; break;
    case HIT_LOCATION_LEFT_ARM:  *fx = 0.85f; *fy = 0.36f; break;
    case HIT_LOCATION_TORSO:     *fx = 0.50f; *fy = 0.31f; break;
    case HIT_LOCATION_GROIN:     *fx = 0.50f; *fy = 0.51f; break;
    case HIT_LOCATION_RIGHT_LEG: *fx = 0.35f; *fy = 0.78f; break;
    case HIT_LOCATION_LEFT_LEG:  *fx = 0.65f; *fy = 0.78f; break;
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
#define GPUI_AIMBODY_MAX_W 256
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
    int headBottom; // last row of the head, i.e. where the neck runs into the chest
    // The head is a COLUMN, not a band across the row: these are its left and right edges. See
    // gpui_find_head -- a band takes the shoulders with it, which is what "nemá označenou celou
    // hlavu" was looking at.
    int headLo;
    int headHi;
    // ⚠️ THE LEGS AND THE TRUNK USED TO LIVE HERE TOO -- crotch row, per-row leg split, groin band,
    // arm reach -- and every one of them was a horizontal stripe. They were deleted on 2026-09-02
    // when the parts started coming out of gpui_aimbody_partition, which decides pixel by pixel
    // instead of row by row. The head stays because it is the one region worth measuring up front:
    // it gates whether the critter is an upright biped at all, and it gives the flood its best seed.
} GpuiBodyRegions;

// Find the neck line: the head is the band from the crown down to the row where the silhouette
// widens into shoulders. Sets headValid=false only for shapes that have no head ABOVE anything --
// a dog, a brahmin, a radscorpion, the Master -- which then keep the plain ring.
//
// ⚠️ THIS USED TO BE A SINGLE-ROW WIDTH JUMP AND THAT IS WHY THE MUTANT HAD NO HEAD.
// The old rule was "the sharpest row-to-row widening in the top 40%, and it must be at least
// 1.4x". Measured over every critter frame in CRITTER.DAT (1370 stand frames: 58 critters x their
// weapon poses x 6 facings) that rule found a head in 68% of frames -- and in a third of THOSE the
// band it returned was not head-shaped at all (it ran from a raised spear tip, or covered half the
// body). Two things break it, and both are properties of the art rather than of any one critter:
//
//   NO SINGLE ROW CARRIES THE JUMP. On a 60-90 px sprite the neck widens over three or four rows,
//   ~1.2x each. MAMTNTLA (super mutant, minigun) facing NE goes 11,13,16,20,23,25,27 px down its
//   first seven rows: a completely ordinary head-into-shoulders, with a best single-row ratio of
//   1.25 -- refused. Same for every robed figure (HAROBEAA peaks at 1.17), which is why the whole
//   robe/peasant family scored 0/6. It was never mutant-specific; the mutant is just what he
//   happened to aim at.
//
//   THE TOP OF THE FRAME IS NOT ALWAYS THE HEAD. A raised spear or a rifle barrel puts a 1-2 px
//   spike above the crown, and the jump then lands where the SHAFT meets the head -- so the
//   "head" outline was drawn around the weapon.
//
// So: skip a thin spike to find the real crown, then walk down for the first row that is 1.5x the
// crown's width, searched inside a band of 10%..24% of the body height -- the anatomical range a
// head occupies on every one of these sprites. Below 10% is the jaw, past 24% is the chest. If no
// such row exists (a gradual robe) the head simply ends at 18%, which is what the art shows.
// Clamping the ANSWER instead of thresholding the evidence is the whole change: 91% of frames now
// get a head, and every band it returns is inside the plausible range by construction.
static void gpui_find_head(const GpuiSilhouette* sil, GpuiBodyRegions* reg)
{
    reg->headValid = false;

    int bodyH = sil->bottom - sil->top + 1;
    if (bodyH < 8) {
        return; // too few pixels to divide into anything
    }

    // Width profile: the widest row, and the MEDIAN row width (a counting histogram -- widths are
    // small integers, so no sort is needed). The median is what makes the upright test below
    // survive a held weapon.
    int maxW = 0;
    int rowsWithPixels = 0;
    int hist[GPUI_AIMBODY_MAX_W + 1];
    memset(hist, 0, sizeof(hist));
    for (int y = sil->top; y <= sil->bottom; y++) {
        if (sil->rowMax[y] < sil->rowMin[y]) {
            continue;
        }
        int w = sil->rowMax[y] - sil->rowMin[y] + 1;
        if (w > maxW) {
            maxW = w;
        }
        int bucket = w > GPUI_AIMBODY_MAX_W ? GPUI_AIMBODY_MAX_W : w;
        hist[bucket]++;
        rowsWithPixels++;
    }
    if (maxW <= 0 || rowsWithPixels <= 0) {
        return;
    }

    int medW = 0;
    for (int w = 0, seen = 0; w <= GPUI_AIMBODY_MAX_W; w++) {
        seen += hist[w];
        if (seen > rowsWithPixels / 2) {
            medW = w;
            break;
        }
    }
    if (medW <= 0) {
        medW = maxW;
    }

    // "A head sits above shoulders" only means something on an UPRIGHT silhouette. A dog, a rat, a
    // brahmin, a radscorpion, the Master are wider than they are tall (or nearly), their head is
    // out at the front rather than on top, and no band of rows is it. They keep the plain ring --
    // an honest refusal, which is what the fallback is for.
    //
    // ⚠️ THE SECOND TEST IS THERE FOR THE MINIGUN. Measured against the frame's WIDEST row alone, a
    // super mutant holding a minigun (62x84) or a laser rifle across its chest is not "tall and
    // narrow" any more and lost its head at three of six facings -- which is half of why he saw the
    // mutant without one. A weapon widens a handful of rows; the MEDIAN row is still the body's own
    // girth, so h >= 1.9 x median recovers exactly those poses. Over all 1370 stand frames the pair
    // finds a head on 99.4% of humanoid frames (up from 97.0%) and wrongly claims one on 35% of
    // non-humanoid frames (up from 26%) -- the extra ones being Mr Handy, a radscorpion facing away
    // and a deathclaw, where the band lands on the top of a shape that has no head anyway.
    if (bodyH * 100 < maxW * 140 && bodyH * 10 < medW * 19) {
        return;
    }

    // The crown: skip a thin spike at the very top (spear held upright, rifle barrel, antenna).
    // Only a spike -- if the "thin" run goes on for a quarter of the body it is the shape itself.
    int thin = maxW * 15 / 100;
    if (thin < 2) {
        thin = 2;
    }
    int crown = sil->top;
    while (crown < sil->bottom) {
        int w = sil->rowMax[crown] >= sil->rowMin[crown] ? sil->rowMax[crown] - sil->rowMin[crown] + 1 : 0;
        if (w >= thin) {
            break;
        }
        crown++;
    }
    if (crown - sil->top > bodyH * 25 / 100) {
        crown = sil->top;
    }

    int crownH = sil->bottom - crown + 1;
    if (crownH < 8) {
        return;
    }

    // Head CENTRE: the middle of the widest run in the crown's first three rows. Everything below
    // is measured against this column, so a head drawn off to one side (every three-quarter facing)
    // is followed instead of averaged away.
    int cx = -1;
    int widestRun = -1;
    for (int y = crown; y <= crown + 2 && y <= sil->bottom; y++) {
        for (int i = 0; i < sil->runCount[y]; i++) {
            int w = sil->runHi[y][i] - sil->runLo[y][i] + 1;
            if (w > widestRun) {
                widestRun = w;
                cx = (sil->runLo[y][i] + sil->runHi[y][i]) / 2;
            }
        }
    }
    if (cx < 0) {
        return;
    }

    // ⚠️ HEIGHT IS A PROPORTION, NOT A DETECTED LINE, and that is deliberate. Every attempt to find
    // the chin from the silhouette lands at the SHOULDER line instead -- on this art the head and
    // the shoulders are one continuous blob, and the row where the silhouette widens is where the
    // shoulder appears BESIDE the head, not where the head ends. Stopping there marks the top third
    // of the head and calls it done: "supermutant nemá označenou celou hlavu". Measured on the art,
    // a figure here is very close to six heads tall across the whole cast (mutant 84 px / head 14,
    // human 64/11, ghoul 61/10, robe 66/11), so 17% of the body IS the head.
    int depth = bodyH * 17 / 100;
    if (depth < 3) {
        depth = 3;
    }
    int bottom = crown + depth - 1;
    if (bottom > sil->bottom) {
        bottom = sil->bottom;
    }

    // Width comes from the sprite -- how wide the run under the head gets before the shoulders merge
    // into it -- but clamped to the proportions the art actually uses (head width 12..22% of body
    // height, measured across the cast). The clamp is what saves the two failure modes of measuring
    // alone: a facing where the shoulders merge on the second row (which would give a 4 px head) and
    // a hood or helmet that keeps widening (which would give a 25 px one).
    int minW = bodyH * 12 / 100;
    if (minW < 3) {
        minW = 3;
    }
    int maxHeadW = bodyH * 22 / 100;
    if (maxHeadW < minW) {
        maxHeadW = minW;
    }

    int headW = 0;
    int prev = 0;
    for (int y = crown; y <= bottom; y++) {
        int w = 0;
        for (int i = 0; i < sil->runCount[y]; i++) {
            if (cx >= sil->runLo[y][i] && cx <= sil->runHi[y][i]) {
                w = sil->runHi[y][i] - sil->runLo[y][i] + 1;
            }
        }
        if (w == 0) {
            break; // the column left the sprite
        }
        if (w > maxHeadW) {
            break; // wider than a head can be: this row is head + shoulder in one run
        }
        if (prev >= minW && w * 100 > prev * 160) {
            break; // and this is that merge happening in one step
        }
        if (w > headW) {
            headW = w;
        }
        prev = w;
    }
    if (headW < minW) {
        headW = minW;
    }
    if (headW > maxHeadW) {
        headW = maxHeadW;
    }

    reg->headTop = crown;
    reg->headBottom = bottom;
    reg->headLo = cx - headW / 2;
    reg->headHi = reg->headLo + headW - 1;
    reg->headValid = reg->headBottom >= reg->headTop;
}


// Are we looking at the critter's BACK? Decides whether its own left limb is drawn on our left or
// our right -- without this the left/right marks are simply mirrored for half of the six facings,
// which is a plain bug in the old fraction-based anchors.
//
// SE/SW face toward the camera (we see the front, so its left is on OUR right); NE/NW face away
// (we see the back, its left is on our left). E and W are the side views: the art is a three-
// quarter pose that reads as front-facing, so they are grouped with the front. That last part is
// the one judgement call here and it is a single line to flip if the screenshot says otherwise.
static bool gpui_rotation_is_back(int rotation)
{
    return rotation == ROTATION_NE || rotation == ROTATION_NW;
}

static bool gpui_viewing_back(Object* obj)
{
    return gpui_rotation_is_back(obj->rotation);
}

// Maps a HIT_LOCATION_*_ARM/LEG onto a screen side. Returns true for "the critter's own left limb
// is on the LEFT of the screen".
static bool gpui_part_on_screen_left(Object* obj, int location)
{
    bool ownLeft = location == HIT_LOCATION_LEFT_ARM || location == HIT_LOCATION_LEFT_LEG;
    return gpui_viewing_back(obj) ? ownLeft : !ownLeft;
}

// --- WHICH PART OWNS WHICH PIXEL: a flood through the body, not a stack of bands. ---
//
// ⚠️ THIS REPLACED THE BAND SEGMENTATION ON 2026-09-02, AND THE REASON IS WORTH KEEPING. Bands ask
// "which horizontal stripe is this row in", so the answer can only ever be a rectangle: the arm was
// a fixed share of each row, the torso a box, a leg half a row. Rendered over the real art (the
// offline part-map harness in [[aim-body-segmentation]]) they read as slabs pasted on a character,
// and no amount of tuning the stripe edges fixes that, because the shape of the answer is wrong.
//
// This asks a different question: WHICH PART CAN REACH THIS PIXEL FIRST, WALKING THROUGH THE BODY.
// Eight seeds are placed on the anatomy (head, two hands, two thighs, torso, groin), then a single
// breadth-first flood runs from all of them at once through opaque pixels only. Every pixel goes to
// whichever seed reaches it first, so a region flows down a limb and stops where the limb does: an
// arm held out keeps its whole arm INCLUDING the hand, a leg mid-stride keeps its whole leg
// including the boot, and the boundary between two parts lands where the body actually narrows --
// the shoulder, the hip -- because that is where the two floods meet.
//
// It is also cheaper than what it replaced: one pass over ~4000 opaque pixels, no per-row run
// analysis, no latch. And it cannot produce a degenerate part: every part that gets a seed gets a
// connected region, and a part whose seed could not be placed simply has none.

// Owner per pixel: a HIT_LOCATION_*, or one of these two.
#define GPUI_OWNER_NONE (-1)
#define GPUI_OWNER_SHADOW (-2)

static signed char gpui_aim_owner[GPUI_AIMBODY_MAX_H * GPUI_AIMBODY_MAX_W];
static int gpui_aim_owner_w = 0;
static int gpui_aim_owner_h = 0;
static bool gpui_aim_owner_valid = false;

// BFS frontier. Two buffers rather than a ring, so the flood stays breadth-first (equal distance
// grows in step) without any modular arithmetic.
static int gpui_aim_bfs_a[GPUI_AIMBODY_MAX_H * GPUI_AIMBODY_MAX_W];
static int gpui_aim_bfs_b[GPUI_AIMBODY_MAX_H * GPUI_AIMBODY_MAX_W];

// Perceived brightness of a palette index, 0..255. cmap is 6-bit per channel, hence the <<2.
static int gpui_pal_luminance(unsigned char index)
{
    int r = cmap[index * 3] << 2;
    int g = cmap[index * 3 + 1] << 2;
    int b = cmap[index * 3 + 2] << 2;
    return (r * 30 + g * 59 + b * 11) / 100;
}

// ⚠️ THE GROUND SHADOW IS PART OF THE SPRITE AND IT IS NOT THE CRITTER. Fallout bakes it into the
// FRM and the engine anchors the frame's BOTTOM ROW to the tile -- so there is no geometry to
// subtract it with. Left alone, the leg flood runs straight out across the ground and the leg
// outline flares into an ellipse on the floor.
//
// It has one property nothing on the body has: it is uniformly very dark, and it is one connected
// mass reaching the bottom of the frame. So: start from the dark pixels on the bottom rows and
// spread through dark pixels only. What that mass reaches is shadow; what it cannot reach is not.
//
// ⚠️ IT IS NOT "THE BOTTOM FIFTH", WHICH IS WHAT THIS TRIED FIRST AND WHY A LEG STILL ATE GROUND.
// The shadow is drawn BEHIND the feet and reaches much higher than the feet do: on the super mutant
// (MAMTNTAA rot 2, 81 rows) it runs from row 55 -- 68 % of the way up -- while the boots are rows
// 72..80, in FRONT of it and brighter. A fixed bottom-fifth cut therefore walled off the part of the
// shadow that was level with the boots and left the ten rows above it to the legs.
//
// The luminance cut stays at 30, which is what separates shadow from footwear: boots sit at 40+ even
// on the darkest critters, and raising it to 40 visibly ate them. Checked by rendering, not reasoning.
#define GPUI_SHADOW_LUM 30
// Only looked for below this fraction of the body -- a safety rail, so a critter dressed head to
// toe in black cannot have its chest declared "ground". The shadow reaches higher than one would
// think (68 % of the sprite on the super mutant), so this cannot be the bottom fifth.
#define GPUI_SHADOW_CEILING_PCT 55

// ⚠️ COLUMN BY COLUMN, FROM THE BOTTOM UP, STOPPING AT THE FIRST LIT PIXEL. Two other rules were
// tried against the art first and both are worse:
//
//   * FOLLOW THE DARKNESS (flood through connected dark pixels from the bottom). The mutant wears
//     dark trousers over dark boots and the shadow touches them, so the flood climbed his legs and
//     declared them ground -- one leg disappeared from the picker entirely.
//   * KEEP EACH ROW'S LIT CORE (dark pixels outside the outermost bright ones are shadow). The
//     ground BETWEEN the two feet is inside that span, so the leg regions kept growing horizontal
//     spikes across the floor.
//
// Walking up a column stops where the critter starts, which is exactly the question being asked. A
// column through a boot hits the boot and stops, so the boot and everything above it is kept, dark
// shading included; a column that only ever passes through ground -- beside the feet, or between
// them -- is ground all the way up. And because the cut is a run from the bottom of each column, it
// can never punch a hole in the middle of a limb, which any per-pixel test can: a hole would be
// traced as an extra outline loop and read as noise.
#define GPUI_SHADOW_BRIGHT_LUM 45

static void gpui_aimbody_wall_off_shadow(const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil)
{
    int bodyH = sil->bottom - sil->top + 1;
    int ceiling = sil->top + bodyH * GPUI_SHADOW_CEILING_PCT / 100;

    for (int x = 0; x < fw; x++) {
        for (int y = fh - 1; y >= ceiling; y--) {
            unsigned char index = fd[(size_t)y * fw + x];
            if (index == 0) {
                continue; // a gap in this column is not the end of the ground
            }
            if (gpui_pal_luminance(index) >= GPUI_SHADOW_BRIGHT_LUM) {
                break; // the critter, standing on it
            }
            gpui_aim_owner[(size_t)y * fw + x] = GPUI_OWNER_SHADOW;
        }
    }
}

// Nearest opaque pixel to (x, y), searched in growing squares. A seed must land ON the body: the
// anatomy says "about here", and this is what turns that into a pixel that exists.
static int gpui_aimbody_seed_at(const unsigned char* fd, int fw, int fh, int x, int y)
{
    int limit = fw > fh ? fw : fh;
    for (int r = 0; r <= limit; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if ((adx > ady ? adx : ady) != r) {
                    continue; // only the ring at radius r
                }
                int px = x + dx;
                int py = y + dy;
                if (px < 0 || px >= fw || py < 0 || py >= fh) {
                    continue;
                }
                int p = py * fw + px;
                if (fd[p] != 0 && gpui_aim_owner[p] == GPUI_OWNER_NONE) {
                    return p;
                }
            }
        }
    }
    return -1;
}

// Row inside the body at `pct` of its height.
static int gpui_aimbody_row_at(const GpuiSilhouette* sil, int pct)
{
    int bodyH = sil->bottom - sil->top + 1;
    int y = sil->top + bodyH * pct / 100;
    if (y < sil->top) {
        y = sil->top;
    }
    if (y > sil->bottom) {
        y = sil->bottom;
    }
    return y;
}

static int gpui_aimbody_row_mid(const GpuiSilhouette* sil, int y)
{
    if (sil->rowMax[y] < sil->rowMin[y]) {
        return 0;
    }
    return (sil->rowMin[y] + sil->rowMax[y]) / 2;
}

// Places the seeds. Everything anatomical this feature knows lives here and nowhere else.
//
// A part may have MORE THAN ONE seed -- `seeds` holds the first of each, `extra` a second. That is
// not a detail: an arm seeded only at the hand has to flood back up the limb, so the torso (whose
// seed sits closer to the shoulder) wins the upper arm on the way, and the mark ends up on a forearm
// while the torso outline bulges over a shoulder -- "trup zabírá hruď a pravou ruku". Seeding the
// SHOULDER as well roots each arm where it attaches, and the two floods then meet along the armpit,
// which is where an arm actually stops.
static void gpui_aimbody_seeds(const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil, const GpuiBodyRegions* reg, Object* target, int* seeds, int* extra)
{
    for (int i = 0; i < 8; i++) {
        seeds[i] = -1;
        extra[i] = -1;
    }

    // HEAD: the detected head column, which is measured from the art (gpui_find_head) and is the one
    // piece of the old segmentation worth keeping. Without it, the crown of the silhouette.
    int headX;
    int headY;
    int eyesY;
    if (reg->headValid) {
        int headH = reg->headBottom - reg->headTop + 1;
        headX = (reg->headLo + reg->headHi) / 2;
        // ⚠️ THE TWO HEAD SEEDS SIT AT OPPOSITE ENDS OF THE HEAD ON PURPOSE. Seeding the eyes at the
        // very top row gave them a three-pixel scrape of hair on the crown, because the head's own
        // seed was only a couple of rows below and won everything else immediately. Putting the eyes
        // a third of the way down and the head at the jaw makes the two floods meet in the middle of
        // the skull: eyes get the face, head gets the jaw and the neck.
        eyesY = reg->headTop + headH / 3;
        headY = reg->headBottom;
    } else {
        headX = gpui_aimbody_row_mid(sil, sil->top);
        headY = gpui_aimbody_row_at(sil, 10);
        eyesY = gpui_aimbody_row_at(sil, 4);
    }
    seeds[HIT_LOCATION_HEAD] = gpui_aimbody_seed_at(fd, fw, fh, headX, headY);
    seeds[HIT_LOCATION_EYES] = gpui_aimbody_seed_at(fd, fw, fh, headX, eyesY);

    // HANDS: the pixel that reaches furthest from the body on each side, anywhere an arm can be.
    // That is a hand, or an elbow, or a shoulder pad -- whatever it is, it is on the arm and on
    // nothing else, which is all a seed has to be.
    int armTop = gpui_aimbody_row_at(sil, 20);
    int armBot = gpui_aimbody_row_at(sil, 55);
    int lx = -1;
    int ly = -1;
    int rx = -1;
    int ry = -1;
    for (int y = armTop; y <= armBot; y++) {
        if (sil->rowMax[y] < sil->rowMin[y]) {
            continue;
        }
        if (lx < 0 || sil->rowMin[y] < lx) {
            lx = sil->rowMin[y];
            ly = y;
        }
        if (rx < 0 || sil->rowMax[y] > rx) {
            rx = sil->rowMax[y];
            ry = y;
        }
    }
    int leftArmLoc = gpui_part_on_screen_left(target, HIT_LOCATION_LEFT_ARM)
        ? HIT_LOCATION_LEFT_ARM
        : HIT_LOCATION_RIGHT_ARM;
    int rightArmLoc = leftArmLoc == HIT_LOCATION_LEFT_ARM
        ? HIT_LOCATION_RIGHT_ARM
        : HIT_LOCATION_LEFT_ARM;
    if (lx >= 0) {
        seeds[leftArmLoc] = gpui_aimbody_seed_at(fd, fw, fh, lx, ly);
    }
    if (rx >= 0) {
        seeds[rightArmLoc] = gpui_aimbody_seed_at(fd, fw, fh, rx, ry);
    }

    // The shoulders: the outer edge of the body a short way below the head, which is where an arm
    // hangs from on every one of these sprites.
    int shoulderY = reg->headValid
        ? reg->headBottom + (sil->bottom - sil->top + 1) * 8 / 100
        : gpui_aimbody_row_at(sil, 25);
    if (shoulderY > sil->bottom) {
        shoulderY = sil->bottom;
    }
    if (sil->rowMax[shoulderY] >= sil->rowMin[shoulderY]) {
        extra[leftArmLoc] = gpui_aimbody_seed_at(fd, fw, fh, sil->rowMin[shoulderY], shoulderY);
        extra[rightArmLoc] = gpui_aimbody_seed_at(fd, fw, fh, sil->rowMax[shoulderY], shoulderY);
    }

    // TORSO and GROIN on the centre line. The groin sits high -- at the belt, not between the
    // thighs: seeded lower it wins the whole pelvis AND both thighs down to the knees, and the leg
    // marks start at the shins. Rendered over the art that reads as "the groin is his trousers".
    int torsoY = gpui_aimbody_row_at(sil, 32);
    int groinY = gpui_aimbody_row_at(sil, 48);
    seeds[HIT_LOCATION_TORSO] = gpui_aimbody_seed_at(fd, fw, fh, gpui_aimbody_row_mid(sil, torsoY), torsoY);
    seeds[HIT_LOCATION_GROIN] = gpui_aimbody_seed_at(fd, fw, fh, gpui_aimbody_row_mid(sil, groinY), groinY);

    // LEGS: mid-THIGH, one either side of the centre line -- not at the feet. Seeding a foot puts
    // the seed at the far end of the leg, so the groin's flood wins everything down to the knee and
    // the leg mark starts halfway down the shin. From the thigh each leg still floods all the way
    // into its own boot, because through the body that is the only way down there.
    int thighY = gpui_aimbody_row_at(sil, 66);
    int thighMid = gpui_aimbody_row_mid(sil, thighY);
    int thighSpan = sil->rowMax[thighY] >= sil->rowMin[thighY]
        ? sil->rowMax[thighY] - sil->rowMin[thighY] + 1
        : 0;
    int spread = thighSpan / 4;
    if (spread < 1) {
        spread = 1;
    }
    {
        int loc = gpui_part_on_screen_left(target, HIT_LOCATION_LEFT_LEG)
            ? HIT_LOCATION_LEFT_LEG
            : HIT_LOCATION_RIGHT_LEG;
        seeds[loc] = gpui_aimbody_seed_at(fd, fw, fh, thighMid - spread, thighY);
    }
    {
        int loc = gpui_part_on_screen_left(target, HIT_LOCATION_LEFT_LEG)
            ? HIT_LOCATION_RIGHT_LEG
            : HIT_LOCATION_LEFT_LEG;
        seeds[loc] = gpui_aimbody_seed_at(fd, fw, fh, thighMid + spread, thighY);
    }
}

// --- HAND-PAINTED PART MASKS (data\aimmask.dat) ---
//
// The flood above infers where a critter's parts are from its silhouette alone, which is as good
// as a guess about anatomy can be. Where it is not good enough, a mask painted BY HAND wins:
// aimmask.dat holds a per-pixel part map for individual stand frames, built out of PNGs painted in
// Aseprite by .claude/tools/f1script/aimmask.js -- that tool carries the painting rules and the
// canvas layout, and is the only thing that writes this file.
//
// ⚠️ A MASK DOES NOT REPLACE THE FLOOD, IT SEEDS IT, and that is not a shortcut -- it is the only
// shape of answer that survives the animation. Every frame of an idle loop is a DIFFERENT SIZE
// (measured: a super mutant breathes between 44x81 and 55x92, a human between 18x60 and 25x60), so
// a mask painted on one frame cannot be stamped onto the frame actually on screen. It is projected
// onto the live silhouette and the flood then fills whatever the projection missed, so the marks
// stay put across the loop instead of snapping between two segmentations several times a second --
// the exact defect that made the previous, latched design unusable.
//
// No file, or no mask for this critter, is the normal case and costs nothing: the flood runs as it
// always did.

#define GPUI_AIMMASK_MAX 128
#define GPUI_AIMMASK_NONE 0xff
// Critter art is named <base><weapon><animation>, and every base in CRITTERS.LST is six characters
// (hfjmps, mamtnt, hmlthr...). That prefix is the critter; the two letters after it are the pose.
#define GPUI_AIMMASK_BASE_LEN 6

typedef struct {
    char name[16]; // FRM base name, uppercase, no extension
    unsigned char rot;
    unsigned short w;
    unsigned short h;
    // Bounds of the PAINTED pixels, which is what the projection maps -- not the frame, whose
    // margins differ from the live frame's.
    unsigned short boxLo;
    unsigned short boxHi;
    unsigned short boxTop;
    unsigned short boxBottom;
    unsigned char* data; // w*h, a HIT_LOCATION_* or GPUI_AIMMASK_NONE
} GpuiAimMask;

static GpuiAimMask gpui_aimmask[GPUI_AIMMASK_MAX];
static int gpui_aimmask_count = 0;
static bool gpui_aimmask_loaded = false;

// The file is little-endian because the tool that writes it is; the DAT formats around it are big-
// endian, so read the bytes rather than reusing db_freadInt and getting it silently backwards.
static unsigned int gpui_aimmask_rd16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int gpui_aimmask_rd32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void gpui_aimmask_load()
{
    if (gpui_aimmask_loaded) {
        return;
    }
    gpui_aimmask_loaded = true; // one attempt per run, hit or miss

    DB_FILE* stream = db_fopen("aimmask.dat", "rb");
    if (stream == NULL) {
        return;
    }

    unsigned char head[16];
    if (db_fread(head, 1, sizeof(head), stream) != sizeof(head)
        || memcmp(head, "AIMM", 4) != 0
        || gpui_aimmask_rd32(head + 4) != 1) {
        db_fclose(stream);
        return;
    }

    int count = (int)gpui_aimmask_rd32(head + 8);
    unsigned int blobStart = gpui_aimmask_rd32(head + 12);
    if (count > GPUI_AIMMASK_MAX) {
        count = GPUI_AIMMASK_MAX;
    }

    for (int i = 0; i < count; i++) {
        unsigned char rec[28];
        if (db_fseek(stream, 16 + i * 28, SEEK_SET) != 0
            || db_fread(rec, 1, sizeof(rec), stream) != sizeof(rec)) {
            break;
        }

        int w = (int)gpui_aimmask_rd16(rec + 18);
        int h = (int)gpui_aimmask_rd16(rec + 20);
        int packedLen = (int)gpui_aimmask_rd16(rec + 22);
        unsigned int blobOff = gpui_aimmask_rd32(rec + 24);
        if (w <= 0 || h <= 0 || w > GPUI_AIMBODY_MAX_W || h > GPUI_AIMBODY_MAX_H || packedLen <= 0) {
            continue;
        }

        unsigned char* packed = (unsigned char*)malloc((size_t)packedLen);
        if (packed == NULL) {
            break;
        }
        if (db_fseek(stream, (long)(blobStart + blobOff), SEEK_SET) != 0
            || db_fread(packed, 1, (size_t)packedLen, stream) != (size_t)packedLen) {
            free(packed);
            break;
        }

        unsigned char* pixels = (unsigned char*)malloc((size_t)w * h);
        if (pixels == NULL) {
            free(packed);
            break;
        }

        // [run][value] pairs, row-major. A short file is a broken file: drop the entry rather
        // than leave half a body mapped.
        size_t out = 0;
        bool ok = true;
        for (int p = 0; p + 1 < packedLen; p += 2) {
            int run = packed[p];
            unsigned char value = packed[p + 1];
            if (out + run > (size_t)w * h) {
                ok = false;
                break;
            }
            memset(pixels + out, value, run);
            out += run;
        }
        free(packed);
        if (!ok || out != (size_t)w * h) {
            free(pixels);
            continue;
        }

        GpuiAimMask* m = &gpui_aimmask[gpui_aimmask_count];
        memcpy(m->name, rec, 16);
        m->name[15] = '\0';
        m->rot = rec[16];
        m->w = (unsigned short)w;
        m->h = (unsigned short)h;
        m->data = pixels;

        int lo = w;
        int hi = -1;
        int top = h;
        int bottom = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (pixels[y * w + x] == GPUI_AIMMASK_NONE) {
                    continue;
                }
                if (x < lo) lo = x;
                if (x > hi) hi = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
        if (hi < lo || bottom < top) {
            free(pixels);
            continue; // nothing painted
        }
        m->boxLo = (unsigned short)lo;
        m->boxHi = (unsigned short)hi;
        m->boxTop = (unsigned short)top;
        m->boxBottom = (unsigned short)bottom;

        gpui_aimmask_count++;
    }

    db_fclose(stream);
    trace_event(TRACE_STATE, "aimmask", "loaded=%d", gpui_aimmask_count);
}

// The art's own file name is the key, so a mask follows the critter's appearance rather than a
// prototype id: every raider in a leather jacket shares one painted mask.
//
// A critter FRM is named <base 6><weapon><animation>, and the match is tried twice: the whole name
// first, then just the BASE. The base match is what makes hand-painting affordable -- one mask for
// a critter covers it standing, walking, running and holding any weapon, instead of one per pose --
// and it is only sound because the projection maps row by row and so re-fits itself to whatever
// silhouette is on screen.
//
// ⚠️ THE LOOSE MATCH IS GATED ON AN UPRIGHT BODY (`upright`, i.e. the caller's headValid), and that
// is not caution for its own sake: the same base also names the KNOCKDOWN and death frames, where
// the critter is lying down. Row-by-row mapping would then confidently paint the head onto whatever
// is at the top of a horizontal body. An exact match is trusted without the gate, because a mask
// painted for that precise frame is a statement about that precise frame.
static const GpuiAimMask* gpui_aimmask_find(Object* target, bool upright)
{
    if (target == NULL) {
        return NULL;
    }
    gpui_aimmask_load();
    if (gpui_aimmask_count == 0) {
        return NULL;
    }

    char* path = art_get_name(target->fid);
    if (path == NULL) {
        return NULL;
    }

    const char* base = path;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    char key[16];
    int n = 0;
    for (const char* p = base; *p != '\0' && *p != '.' && n < 15; p++) {
        key[n++] = (char)toupper((unsigned char)*p);
    }
    key[n] = '\0';

    for (int i = 0; i < gpui_aimmask_count; i++) {
        const GpuiAimMask* m = &gpui_aimmask[i];
        if (m->rot == target->rotation && strcmp(m->name, key) == 0) {
            return m;
        }
    }

    if (!upright || n < GPUI_AIMMASK_BASE_LEN) {
        return NULL;
    }

    // Ranked, best first: this rotation; a rotation seen from the same side (the other front view,
    // or the other back view); then anything painted for this critter. Rendered over all six
    // directions before it was allowed: a front mask carries into the other front view and into the
    // back views cleanly, and in the two PROFILE views it is a compromise -- it lays an arm along
    // both edges of a body where only one arm is really visible -- but still beats the inferred
    // seeds it replaces, which had no idea where anything was.
    const GpuiAimMask* sameView = NULL;
    const GpuiAimMask* anyView = NULL;
    bool targetBack = gpui_rotation_is_back(target->rotation);
    for (int i = 0; i < gpui_aimmask_count; i++) {
        const GpuiAimMask* m = &gpui_aimmask[i];
        if (strncmp(m->name, key, GPUI_AIMMASK_BASE_LEN) != 0) {
            continue;
        }
        if (m->rot == target->rotation) {
            return m;
        }
        if (sameView == NULL && gpui_rotation_is_back(m->rot) == targetBack) {
            sameView = m;
        }
        if (anyView == NULL) {
            anyView = m;
        }
    }
    return sameView != NULL ? sameView : anyView;
}

// True when a mask painted for `maskRot` has to have its left and right limbs exchanged to be used
// at `targetRot`: the sides are stored anatomically, and which anatomical side you SEE on the left
// of the screen flips between a front view and a back one. Same rule as gpui_part_on_screen_left.
static bool gpui_aimmask_swaps_sides(const GpuiAimMask* m, Object* target)
{
    return gpui_rotation_is_back(m->rot) != gpui_rotation_is_back(target->rotation);
}

// Lays a painted mask over the frame being drawn, and returns the pixels it claimed so they can be
// the flood's starting front (anything it could not decide is left to the flood, as before).
//
// ⚠️ ROW BY ROW, MATCHING THE BODY'S WIDTH -- NOT A BOX SCALED ONTO A BOX, AND NOT SEEDS. Both of
// those were built and rendered over the whole idle loop before this one survived:
//
//   * box onto box assumes the pose does not change. It does -- the arm comes out, the weight
//     shifts -- and by the middle of the human female's loop one leg's colour covered both legs
//     while the other leg had none.
//   * eight seed points thrown to the flood keeps nothing of what was painted but its centres of
//     mass, and the flood's own distances then decide everything: the head ran down the shoulder,
//     the groin spread into a belt, the torso shrank to a blob.
//
// A ROW of the live body maps onto the matching row of the painted body and stretches to ITS width.
// That keeps the order across a row -- arm, torso, arm -- while letting each part's share breathe
// with the silhouette, which is exactly what changes between frames.
static int gpui_aimmask_project(const GpuiAimMask* m, const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil, bool swapSides, int* front)
{
    // Reused from a direction that sees the critter from the other side; see gpui_aimmask_swaps_sides.
    signed char relabel[8];
    for (int i = 0; i < 8; i++) {
        relabel[i] = (signed char)i;
    }
    if (swapSides) {
        relabel[HIT_LOCATION_LEFT_ARM] = HIT_LOCATION_RIGHT_ARM;
        relabel[HIT_LOCATION_RIGHT_ARM] = HIT_LOCATION_LEFT_ARM;
        relabel[HIT_LOCATION_LEFT_LEG] = HIT_LOCATION_RIGHT_LEG;
        relabel[HIT_LOCATION_RIGHT_LEG] = HIT_LOCATION_LEFT_LEG;
    }

    // ⚠️ MEASURE THE BODY, NOT THE FRAME. The shadow is baked into the sprite, sticks out to one
    // side and below the feet, and grows and shrinks with the pose, so a box drawn around it
    // stretches the mapping differently on every frame. It has already been walled off by the
    // caller, so the opaque pixels that still own nothing are exactly the critter.
    int liveTop = fh;
    int liveBottom = -1;
    for (int y = sil->top; y <= sil->bottom; y++) {
        for (int x = 0; x < fw; x++) {
            size_t p = (size_t)y * fw + x;
            if (fd[p] == 0 || gpui_aim_owner[p] == GPUI_OWNER_SHADOW) {
                continue;
            }
            if (y < liveTop) liveTop = y;
            if (y > liveBottom) liveBottom = y;
        }
    }
    if (liveBottom < liveTop) {
        return 0;
    }

    int liveH = liveBottom - liveTop + 1;
    int maskH = m->boxBottom - m->boxTop + 1;

    int count = 0;
    for (int y = liveTop; y <= liveBottom; y++) {
        int lo = -1;
        int hi = -1;
        for (int x = 0; x < fw; x++) {
            size_t p = (size_t)y * fw + x;
            if (fd[p] == 0 || gpui_aim_owner[p] == GPUI_OWNER_SHADOW) {
                continue;
            }
            if (lo < 0) lo = x;
            hi = x;
        }
        if (lo < 0) {
            continue;
        }

        int my = maskH > 1
            ? m->boxTop + ((y - liveTop) * (maskH - 1) + (liveH - 1) / 2) / (liveH - 1)
            : m->boxTop;
        if (my < 0 || my >= m->h) {
            continue;
        }

        int mlo = -1;
        int mhi = -1;
        for (int mx = 0; mx < m->w; mx++) {
            if (m->data[my * m->w + mx] >= 8) {
                continue;
            }
            if (mlo < 0) mlo = mx;
            mhi = mx;
        }
        if (mlo < 0) {
            continue; // nothing painted on that row; the flood gets this one
        }

        for (int x = lo; x <= hi; x++) {
            size_t p = (size_t)y * fw + x;
            if (fd[p] == 0 || gpui_aim_owner[p] != GPUI_OWNER_NONE) {
                continue;
            }
            int mx = hi > lo
                ? mlo + ((x - lo) * (mhi - mlo) + (hi - lo) / 2) / (hi - lo)
                : mlo;
            unsigned char part = m->data[my * m->w + mx];
            if (part >= 8) {
                continue;
            }
            gpui_aim_owner[p] = relabel[part];
            front[count++] = (int)p;
        }
    }
    return count;
}

// Builds the owner map for this frame. False = this critter is not a shape we can divide up (no
// head region: a dog, a scorpion, the Master), and the caller falls back to the plain rings.
static bool gpui_aimbody_partition(const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil, const GpuiBodyRegions* reg, Object* target)
{
    gpui_aim_owner_valid = false;

    if (fd == NULL || fw <= 0 || fh <= 0 || fw > GPUI_AIMBODY_MAX_W || fh > GPUI_AIMBODY_MAX_H) {
        return false;
    }
    // A painted mask states where the parts are, so it needs none of the inference below -- not the
    // seeds, and not the upright-biped gate either: a dog can have a mask even though nothing in
    // the silhouette code can find a head on one.
    const GpuiAimMask* mask = gpui_aimmask_find(target, reg->headValid);

    if (mask == NULL && !reg->headValid) {
        // Same gate as before: an upright silhouette is what all of this assumes, and a critter
        // that fails it deserves an honest ring rather than a confident carve-up.
        return false;
    }

    memset(gpui_aim_owner, GPUI_OWNER_NONE, (size_t)fh * fw);
    gpui_aimbody_wall_off_shadow(fd, fw, fh, sil);

    int seeds[8];
    int extra[8];
    for (int i = 0; i < 8; i++) {
        seeds[i] = -1;
        extra[i] = -1;
    }
    if (mask == NULL) {
        gpui_aimbody_seeds(fd, fw, fh, sil, reg, target, seeds, extra);
    }

    // ⚠️ THE HEAD IS THE ONE PART THAT NEEDS A LEASH. Everywhere else the floods meet at a real
    // narrowing, but a hunched critter has no neck to speak of -- on the super mutant the skull sits
    // straight on a shoulder mass twice its width, so the head's flood had nothing to stop it and ran
    // down the back: "hlava stále zabírá záda". A head is a head-sized thing, so it is allowed to
    // spread about a head and a half from its seed and no further; the pixels beyond that fall to
    // whichever part reaches them next, which is the torso. Nothing else is capped -- an arm SHOULD
    // be allowed to run as far as the arm goes.
    int headH = reg->headBottom - reg->headTop + 1;
    int headBudget = headH * 3 / 2;
    if (headBudget < 4) {
        headBudget = 4;
    }
    int budget[8];
    for (int i = 0; i < 8; i++) {
        budget[i] = 1 << 20;
    }
    if (reg->headValid) {
        // Guarded because a mask lets a critter through that has no detected head at all (a dog),
        // and then there is no head height to leash the flood with.
        budget[HIT_LOCATION_HEAD] = headBudget;
        budget[HIT_LOCATION_EYES] = headBudget;
    }

    int* cur = gpui_aim_bfs_a;
    int* next = gpui_aim_bfs_b;
    int curCount = 0;
    if (mask != NULL) {
        curCount = gpui_aimmask_project(mask, fd, fw, fh, sil,
            gpui_aimmask_swaps_sides(mask, target), cur);
    }
    for (int i = 0; i < 8; i++) {
        int p = seeds[i];
        if (p >= 0 && gpui_aim_owner[p] == GPUI_OWNER_NONE) {
            gpui_aim_owner[p] = (signed char)i;
            cur[curCount++] = p;
        }
        p = extra[i];
        if (p >= 0 && gpui_aim_owner[p] == GPUI_OWNER_NONE) {
            gpui_aim_owner[p] = (signed char)i;
            cur[curCount++] = p;
        }
    }
    if (curCount == 0) {
        return false;
    }

    int dist = 0;
    while (curCount > 0) {
        int nextCount = 0;
        dist++;
        for (int i = 0; i < curCount; i++) {
            int p = cur[i];
            int y = p / fw;
            int x = p - y * fw;
            signed char owner = gpui_aim_owner[p];
            if (dist > budget[owner]) {
                continue; // this part has spread as far as it is entitled to
            }

            int nb[4];
            int n = 0;
            if (x > 0) {
                nb[n++] = p - 1;
            }
            if (x < fw - 1) {
                nb[n++] = p + 1;
            }
            if (y > 0) {
                nb[n++] = p - fw;
            }
            if (y < fh - 1) {
                nb[n++] = p + fw;
            }
            for (int k = 0; k < n; k++) {
                int q = nb[k];
                if (fd[q] == 0 || gpui_aim_owner[q] != GPUI_OWNER_NONE) {
                    continue;
                }
                gpui_aim_owner[q] = owner;
                next[nextCount++] = q;
            }
        }
        int* swap = cur;
        cur = next;
        next = swap;
        curCount = nextCount;
    }

    gpui_aim_owner_w = fw;
    gpui_aim_owner_h = fh;
    gpui_aim_owner_valid = true;
    return true;
}

// --- Part mask + outline. ---

// One frame's worth of "this pixel belongs to the selected part". Static rather than stack: this
// runs from the per-frame aim path (and, indirectly, from the mouse hook) where 64K of stack is
// not worth spending.
static unsigned char gpui_aim_mask[GPUI_AIMBODY_MAX_H * GPUI_AIMBODY_MAX_W];

// Marks the opaque pixels belonging to `location` -- now just a read-out of the flood
// (gpui_aimbody_partition), which already decided every pixel's owner. False = this critter could
// not be divided up at all, or nothing reached this part, and the caller falls back to the plain
// ring rather than drawing an outline around a guess.
static bool gpui_aimbody_build_mask(const unsigned char* fd, int fw, int fh,
    const GpuiSilhouette* sil, const GpuiBodyRegions* reg, Object* target, int location)
{
    if (fw > GPUI_AIMBODY_MAX_W || fh > GPUI_AIMBODY_MAX_H) {
        return false;
    }
    if (!gpui_aim_owner_valid || gpui_aim_owner_w != fw || gpui_aim_owner_h != fh) {
        return false;
    }

    memset(gpui_aim_mask, 0, (size_t)fh * fw);

    bool anyPixel = false;
    for (int i = 0; i < fh * fw; i++) {
        if (gpui_aim_owner[i] == (signed char)location) {
            gpui_aim_mask[i] = 1;
            anyPixel = true;
        }
    }
    return anyPixel;
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
        // SEGMENT THE BODY. gpui_find_head still runs, for two things only: it is the upright-biped
        // gate (a dog or a scorpion produces no head region, and such a critter keeps the honest
        // plain rings), and its head column is the best seed the flood can be given. Everything
        // else about the body now comes out of the flood itself.
        gpui_find_head(&sil, &reg);
        bool partitioned = gpui_aimbody_partition(fd, fw, fh, &sil, &reg, target);

        // ⚠️ RECOMPUTED EVERY FRAME, AND THAT IS NOW SAFE -- it was not before. The old band
        // detection had to be latched per (target, fid, rotation) because it swung wildly between
        // idle frames: measured on the same mutant, consecutive frames put the head column at 11 px,
        // then 19 px, then 10 px, and the crotch row at 73, none, 88, none, 66. The flood does not
        // do that: it is driven by the whole silhouette rather than by one row's widest run, so the
        // same pose lands in the same place, and the frame-to-frame breathing moves a boundary by a
        // pixel instead of teleporting it. Recomputing also keeps the mark exactly on the frame
        // being drawn, which the re-projected latch could only approximate.
        if (partitioned) {
            // ANCHORS ARE THE REGIONS' OWN CENTRES OF MASS. The fraction table above placed them by
            // proportion, which is a guess about where a part is; the centroid is where the part
            // that will be outlined actually IS, so the ring (drawn when a part has no pixels) and
            // the label can never sit somewhere the outline is not.
            int sumX[8];
            int sumY[8];
            int count[8];
            for (int i = 0; i < 8; i++) {
                sumX[i] = 0;
                sumY[i] = 0;
                count[i] = 0;
            }
            for (int y = sil.top; y <= sil.bottom; y++) {
                const signed char* own = gpui_aim_owner + (size_t)y * fw;
                for (int x = 0; x < fw; x++) {
                    signed char o = own[x];
                    if (o < 0) {
                        continue;
                    }
                    for (int i = 0; i < 8; i++) {
                        if (gpui_aim_order[i].location == o) {
                            sumX[i] += x;
                            sumY[i] += y;
                            count[i]++;
                            break;
                        }
                    }
                }
            }
            for (int i = 0; i < 8; i++) {
                if (count[i] == 0) {
                    continue; // nothing reached this part: keep the proportional anchor
                }
                int cx = sumX[i] / count[i];
                int cy = sumY[i] / count[i];

                // A centroid can land in a hole -- between two legs, inside the crook of an arm.
                // Pull it onto the nearest pixel the part actually owns, along its own row first.
                if (cx >= 0 && cx < fw && cy >= sil.top && cy <= sil.bottom
                    && gpui_aim_owner[(size_t)cy * fw + cx] != (signed char)gpui_aim_order[i].location) {
                    int best = -1;
                    int bestDist = 1 << 30;
                    for (int y = sil.top; y <= sil.bottom; y++) {
                        const signed char* own = gpui_aim_owner + (size_t)y * fw;
                        for (int x = 0; x < fw; x++) {
                            if (own[x] != (signed char)gpui_aim_order[i].location) {
                                continue;
                            }
                            int dx = x - cx;
                            int dy = y - cy;
                            int d = dx * dx + dy * dy;
                            if (d < bestDist) {
                                bestDist = d;
                                best = y * fw + x;
                            }
                        }
                    }
                    if (best >= 0) {
                        cy = best / fw;
                        cx = best - cy * fw;
                    }
                }

                gpui_aimbody_anchor_x[i] = cx;
                gpui_aimbody_anchor_y[i] = cy;
            }
        }

        gpui_aimbody_anchors_valid = boxHi >= boxLo;

        // TEMPORARY DIAGNOSTIC (remove once the marker placement is settled). The screenshots
        // measure the head ring ~5 px ABOVE the crown and the leg outline ~14 px BELOW the feet on
        // the same critter -- marks cannot be both, so the frame this scan runs on is not the frame
        // on screen, or the rect it is mapped through is not the sprite's. This line prints both
        // sides of that mapping so it can be compared against the sprite instead of guessed at.
        // Coalesced by `sub`, so holding the picker costs one line per changing state, not per frame.
        {
            int selIdxForTrace = -1;
            for (int i = 0; i < 8; i++) {
                if (gpui_aim_order[i].location == selLoc) {
                    selIdxForTrace = i;
                    break;
                }
            }
            static int lastFid = -1;
            static int lastFrame = -1;
            static int lastRot = -1;
            static int lastSel = -1;
            if (target->fid != lastFid || target->frame != lastFrame
                || target->rotation != lastRot || selLoc != lastSel) {
                lastFid = target->fid;
                lastFrame = target->frame;
                lastRot = target->rotation;
                lastSel = selLoc;
                trace_event(TRACE_ACTION, "aim-draw",
                    "who=%s pid=%d tile=%d "
                    "fid=%08x frame=%d rot=%d anim=%d frm=%dx%d bound=(%d,%d)-(%d,%d) %dx%d "
                    "win=(%d,%d)-(%d,%d) sil=%d..%d box=%d..%d head=%d[%d..%d x %d..%d] "
                    "split=%d sel=%d at=(%d,%d)",
                    object_name(target), target->pid, target->tile,
                    target->fid, target->frame, target->rotation, FID_ANIM_TYPE(target->fid),
                    fw, fh,
                    b.ulx, b.uly, b.lrx, b.lry, b.lrx - b.ulx + 1, b.lry - b.uly + 1,
                    winRect.ulx, winRect.uly, winRect.lrx, winRect.lry,
                    sil.top, sil.bottom, boxLo, boxHi,
                    reg.headValid ? 1 : 0, reg.headTop, reg.headBottom, reg.headLo, reg.headHi,
                    partitioned ? 1 : 0, selLoc,
                    selIdxForTrace >= 0 ? gpui_aimbody_anchor_x[selIdxForTrace] : -1,
                    selIdxForTrace >= 0 ? gpui_aimbody_anchor_y[selIdxForTrace] : -1);
            }
        }

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
        // ⚠️ THE HEAD IS TRACED LIKE EVERY OTHER PART NOW, AND THAT IS ONLY TRUE SINCE THE FLOOD.
        // It used to be drawn as an ellipse, for a good reason at the time: the head "region" was a
        // box invented by a rule, so tracing its boundary drew straight lines across the chest -- a
        // green rectangle pasted on the character, and the better the box was measured the more
        // precisely wrong it looked. The flood's head is not a box: on the outside it follows the
        // sprite's own edge, and underneath it stops where the head's flood met the torso's, which
        // is the neck. There is nothing left to apologise for with a reticle.
        bool outlined = false;

        // PREFERRED for the limbs and trunk: trace the part's own outline, where the mask's
        // boundary really does follow the sprite (a leg's edge, an arm clear of the body). Falls
        // through to the ring when this frame's shape did not yield the part.
        if (!outlined && selIndex >= 0 && gpui_aimbody_build_mask(fd, fw, fh, &sil, &reg, target, selLoc)) {
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

// Which body part owns one frame pixel -- now a direct read of the flood, so it can never disagree
// with the outline the player is looking at. -1 = this critter was not divided up, or the pixel is
// background or ground shadow. Unused by the stick (which picks by direction), kept because it is
// the natural way to ask the question and costs one lookup.
static int gpui_aimbody_classify(Object* target, int px, int py)
{
    if (!gpui_aim_owner_valid) {
        return -1;
    }
    if (px < 0 || px >= gpui_aim_owner_w || py < 0 || py >= gpui_aim_owner_h) {
        return -1;
    }
    return gpui_aim_owner[(size_t)py * gpui_aim_owner_w + px];
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
// ⚠️ AND WHY IT IS NOT "POINT AT THE PIXEL" ANY MORE EITHER. Classifying the pixel under the stick
// was self-consistent and measurably unusable, because the eight parts do not get eight comparable
// shares of the stick's travel -- they get whatever share their pixels happen to occupy in the
// sprite's bounding box. Replaying this function over a 49x49 grid of stick positions on the exact
// frame his trace named (MAMTNTAA rot 1) and counting where each stick position landed:
//
//     eyes 112   head 99   left arm 236   TORSO 272   right arm 353   groin 324
//     right leg 164   LEFT LEG 12   (of ~1800 reachable stick positions)
//
// and the torso's 272 sat in a thin ring around the dead zone, i.e. only at one precise stick
// magnitude. His own trace agrees: 61 samples of the picker in use, and `sel=` never once took the
// value 3 (torso) or 5 (left leg). "Nejde mi na něm zaměřit trup" was not a feel, it was arithmetic:
// the arms are wide, the torso is the narrow middle, and one leg swallows the other's half of the
// gap whenever the split sits off-centre.
//
// ⚠️ AND WHY "FIXED ANATOMICAL BANDS" WAS STILL WRONG (2026-09-02). The bands that replaced the
// pixel test were rectangles in (sx, sy), not wedges, so two of the eight parts were unreachable at
// the only stick position a thumb actually holds -- the rim:
//
//     torso: |sx| < 0.40 AND |sy| < 0.34  ->  impossible past magnitude 0.52
//     groin: 0.34 <= sy < 0.56            ->  push straight down to the stop (sy ~ 1.0) and you
//                                             always land in a LEG instead
//
// That is arithmetic, not taste, and it is the same defect as the pixel version wearing a different
// shape: a rule whose answer depends on HOW FAR the stick is pushed, when a thumb only ever knows
// centre or rim. The measurements that kept coming back lopsided (torso 13 % of the disc but only
// in a thin ring; his trace with sel= never once reading 3 or 5) were all reading that.
//
// THE RULE NOW: past the dead zone, ONLY THE ANGLE COUNTS. Eight equal 45-degree wedges, clockwise
// from straight up, in the same order the marks sit on the body and the same order the wheels use:
//
//        N  head          NE  eyes         E   arm on the screen-right
//        SE leg (right)   S   groin        SW  leg (screen-left)
//        W  arm (left)    NW  torso
//
// So every part owns exactly 1/8 of every circle at every magnitude -- provably equal, nothing to
// measure, nothing to starve. Up is the head because that is where a head is; the arms and legs go
// to the side they are DRAWN on (gpui_limb_for_screen_side flips them when you see the critter's
// back), so "push toward the leg you can see" is right whichever way it faces. The eyes are inside
// the head, so they take the spare upper diagonal next to it.
//
// ⚠️ AND THE CENTRE IS THE TORSO -- IT IS NOT "HOLD WHAT YOU HAD". The first version of this ring
// latched the selection inside the dead zone, and that broke the one movement everybody makes:
// "proč sjedu z hlavy na groin místo hlava trup groin?" Dragging the stick from up to down passes
// through the CENTRE, where a latch means nothing changes, so the sweep read head -> groin and the
// torso -- the part in the middle of the body -- was nowhere on the path. The centre of the stick IS
// the middle of the body: releasing it selects the torso, and a vertical drag now walks head, torso,
// groin in that order, which is what the body looks like. The torso keeps its up-left wedge too, so
// it can also be picked without letting go.
//
// The on-body marks still show WHERE each part is; they no longer decide whether it can be picked.

// The pure geometry, kept free of the target so it can be lifted verbatim into the offline harness
// and measured over a grid of stick positions (see [[aim-body-segmentation]]). Returns the ring
// slot 0..7 clockwise from up, or -1 when the stick is at rest.
static int gpui_aim_ring_slot(float sx, float sy)
{
    if (sx * sx + sy * sy < GPUI_AIM_STICK_MIN * GPUI_AIM_STICK_MIN) {
        return -1;
    }

    // atan2(x, -y): 0 at straight up, growing clockwise, because the stick's y axis points DOWN.
    float a = atan2f(sx, -sy);
    if (a < 0.0f) {
        a += 2.0f * GPUI_PI;
    }

    // Wedges are CENTRED on their direction, hence the half-wedge offset: straight up is the middle
    // of slot 0, not its edge.
    int slot = (int)((a + GPUI_PI / 8.0f) / (GPUI_PI / 4.0f)) % 8;
    return slot < 0 ? 0 : slot;
}

static int gpui_aimbody_pick(Object* target, float sx, float sy, int curLoc)
{
    int slot = gpui_aim_ring_slot(sx, sy);
    if (slot < 0) {
        return HIT_LOCATION_TORSO; // stick at rest = the middle of the body
    }
    if (target == NULL) {
        return curLoc;
    }

    switch (slot) {
    case 0:
        return HIT_LOCATION_HEAD;
    case 1:
        return HIT_LOCATION_EYES;
    case 2:
        return gpui_limb_for_screen_side(target, false, true); // arm, screen-right
    case 3:
        return gpui_limb_for_screen_side(target, false, false); // leg, screen-right
    case 4:
        return HIT_LOCATION_GROIN;
    case 5:
        return gpui_limb_for_screen_side(target, true, false); // leg, screen-left
    case 6:
        return gpui_limb_for_screen_side(target, true, true); // arm, screen-left
    default:
        return HIT_LOCATION_TORSO;
    }
}

// Tears the picker down and puts the iso crosshair back. Every exit from the picker goes through
// here so the suppressed cursor can never be left hidden.
static void gpui_aim_picker_close()
{
    // ⚠️ CLEARED BEFORE THE EARLY RETURN, NOT AFTER IT. The gesture flag is set as soon as the
    // trigger goes down, which is long before the picker itself appears -- so "close the picker"
    // is routinely called while gpui_aim_body_on is still false, and leaving the clear below that
    // guard stranded the flag at true with nothing able to reset it.
    gpui_aim_gesture = false;

    if (!gpui_aim_body_on) {
        return;
    }
    gpui_aimbody_hide();
    gpui_aim_body_on = false;
    gmouse_set_cursor_suppressed(false);
}

// LT tap/hold aimed shot. TAP (< GPUI_AIM_TAP_MS) fires instantly at the last-used part; HOLD brings
// up the AIM wheel (right stick picks a part) with the chosen part marked on the target's body;
// release fires it -- or B cancels, which is the only way out that does NOT spend the shot.
// --- "USE THE THING IN MY HAND ON THAT" (hold A) ------------------------------------------------
//
// His design, and it turned out the engine already has the whole flow -- only the pad had no way in.
//
// Put the item in the active hand, HOLD A -> the use-on crosshair arms; aim; press A -> it is used
// on whatever is under the cursor. Rope on the elevator, lockpick on a door, dynamite on a locker.
//
// Both halves are vanilla:
//   * arming is `intface_use_item()` (intface.cc) -- the same call the USE button on the interface
//     bar's item slot makes. For an item with `proto_action_can_use_on` it sets
//     MOUSE_CURSOR_USE_CROSSHAIR + GAME_MOUSE_MODE_USE_CROSSHAIR and stops there.
//   * applying is already wired: a left click in that mode goes to gmouse.cc:987, which reads the
//     ACTIVE HAND item via intface_get_current_item and calls action_use_an_item_on_object -- the
//     same function the action-ring backpack icon ends up in. That branch is already realtime-aware
//     (it charges combat_rt's AP pool), so nothing new is needed for the second A at all.
//
// This is the same shape the skill wheel uses: arm an engine cursor mode, then let the player aim
// and click with A. No new targeting code, no cached Object* to dangle.
//
// ⚠️ GATED ON THE HELD ITEM being use-on-able. With anything else in hand -- a gun, nothing, a
// stimpak -- hold A keeps meaning exactly what it means today (the engine's hold-to-open action
// ring), so no existing gesture is taken away. intface_use_item() is deliberately NOT called in
// those cases: for a WEAPON it would flip the interface into attack/crosshair mode, which is not
// what holding A should ever do.
static bool gpui_useon_item_in_hand()
{
    Object* item = NULL;
    if (intface_get_current_item(&item) == -1 || item == NULL) {
        return false;
    }

    return proto_action_can_use_on(item->pid);
}

// --- WHERE THE USE-ON CROSSHAIR APPEARS ---------------------------------------------------------
//
// ⚠️ IT USED TO ARM WHEREVER THE POINTER HAPPENED TO HAVE BEEN LEFT, and in free play the pointer is
// hidden and has not moved for minutes -- so holding A on a flare armed a crosshair somewhere off in
// the corner of the map, and the throw began with a hunt for it.
//
// His rule: it belongs in front of the dude's eyes. So it parks GPUI_USEON_PARK_TILES tiles along
// the direction he is FACING (obj_dude->rotation -- the same facing the walk/free-movement mover
// writes), at the centre of that tile, using the engine's own tile-centre convention of +16/+15
// (gmouse.cc:2376). Facing west, the crosshair arms to the west of him, already over the ground he
// was looking at.
#define GPUI_USEON_PARK_TILES 3

static void gpui_useon_park_cursor()
{
    if (obj_dude == NULL) {
        return;
    }

    int sx;
    int sy;
    int dest = tile_num_in_direction(obj_dude->tile, obj_dude->rotation, GPUI_USEON_PARK_TILES);

    // Facing off the edge of the map: his own feet are still "in front of him", and they are always
    // a valid place to start aiming from.
    if (dest < 0 || tile_coord(dest, &sx, &sy, obj_dude->elevation) != 0) {
        if (tile_coord(obj_dude->tile, &sx, &sy, obj_dude->elevation) != 0) {
            return;
        }
    }

    gpui_park_cursor_at(sx + 16, sy + 15);
}

static void gpui_useon_arm(SDL_GameController* pad)
{
    static bool prevA = false;
    static unsigned int aDownTime = 0;
    static bool aHandled = false; // this hold already armed; don't retry every frame
    // This press turned the world click OFF -- whether it armed a cursor or not -- and owes turning it
    // back on at the release. See the note at the release.
    static bool aSuppressing = false;

    bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool aEdge = aNow && !prevA;
    bool aReleased = !aNow && prevA;
    prevA = aNow;

    if (aEdge) {
        aDownTime = get_time();
        aHandled = false;
    }

    if (aReleased) {
        // Let A click again. Held down, the press that armed the cursor must not ALSO reach the
        // world -- otherwise the same hold arms the mode and immediately spends it on whatever the
        // cursor happened to be over.
        //
        // ⚠️ WHOEVER TURNED IT OFF TURNS IT BACK ON (2026-10-10). This used to clear it only when the
        // hold had ARMED a cursor (aHandled) -- but a hold with nothing use-on-able in hand switches
        // the click off too (below) and never sets aHandled, so one long A at a door left every later
        // A click dead for the rest of the session. The A that should confirm a lockpick or a rope
        // was swallowed and only RT, which clicks on its own path, still worked:
        // trace_2026-10-10_115753 s=563.81 / 577.03, A twice at the door with the lockpick armed,
        // nothing; RT, used. *"když klikám itemy jako provaz na šachtu, musím zmáčknout R2"*.
        if (aHandled || aSuppressing) {
            gamepad_ui_set_suppress_a_click(false);
        }
        aSuppressing = false;

        // The press never became a hold, so it was a TAP after all: run the interaction that
        // gpui_action_buttons held back. interact_prompt_request only QUEUES the action (it is
        // performed by interact_prompt_tick), so this is safe from the frame tick.
        if (gpui_a_interact_deferred) {
            gpui_a_interact_deferred = false;
            if (!aHandled && !gamepad_ui_menu_active() && !dialog_active() && !inven_any_open()) {
                interact_prompt_request();
            }
        }

        aHandled = false;
        return;
    }

    if (!aNow || aHandled) {
        return;
    }

    // Nothing to arm from, or somewhere A means something else entirely.
    if (radial_wheel_is_open() || gamepad_ui_menu_active() || dialog_active() || inven_any_open()
        || !intface_is_enabled() || gpui_aim_picker_active() || gpui_self_nav) {
        return;
    }

    // Already in a use-on cursor: a second hold would just re-arm it. Leave it be so the player can
    // hold A while lining the shot up without the mode flickering.
    if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_USE_CROSSHAIR) {
        return;
    }

    if (get_time() - aDownTime < GPUI_QUICK_HOLD_MS) {
        return;
    }

    if (!gpui_useon_item_in_hand()) {
        // ⚠️ A HOLD WITH NOTHING TO ARM MUST DO WHAT A TAP DOES, NOT SOMETHING ELSE.
        //
        // Without this the held press fell through to the raw world CLICK that gpui_action_buttons
        // synthesises -- and a raw click lands on whatever the cursor is over, not on the object the
        // interact prompt is offering. In the Glow elevator that is the difference between the floor
        // panel and the door beside it: "když držím Ačko tak to ty dveře zavře, jinak patra". Three
        // rounds of looking inside the elevator missed it, because the elevator was never the
        // problem -- the press simply went somewhere else on its way.
        //
        // Suppressing only the CLICK is deliberate: the interaction the press deferred is still
        // sitting in gpui_a_interact_deferred and fires on release, so a hold now performs exactly
        // the same action a tap would. The suppression is cleared on that release (above).
        gamepad_ui_set_suppress_a_click(true);
        aSuppressing = true;
        return;
    }

    aHandled = true;
    aSuppressing = true;
    // The press turned out to be a hold, so the interaction gpui_action_buttons deferred is
    // cancelled outright -- arming REPLACES it, it does not follow it.
    gpui_a_interact_deferred = false;
    gamepad_ui_set_suppress_a_click(true);
    gsound_play_sfx_file("ib1p1xx1");
    intface_use_item();

    // ...and put it in front of his eyes rather than wherever the pointer was abandoned. Only once
    // the mode is really up: intface_use_item() is the authority on whether this item arms anything.
    if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_USE_CROSSHAIR) {
        gpui_useon_park_cursor();
    }

    // Show the cursor you just armed. It is also what makes the confirming A land at all: a world
    // click at a HIDDEN cursor is dropped on purpose (gpui_action_buttons), so an armed crosshair
    // nobody can see would refuse every press.
    gpui_cursor_show_time = get_time();
}

// AIMING IS LB (hold = the called-shot window, which binds a part AND puts the weapon slot on its
// AIMED mode) + RT (attack -- aimed while the slot shows the bullseye). RB is the heal since
// 2026-10-09. ⚠️ RT IS ONLY EVER THE ATTACK BUTTON: while the picker is up that
// attack simply lands on the part you chose, which is his rule -- "RT bude jen utok".
static void gpui_aim_wheel(SDL_GameController* pad)
{
    static bool prevLb = false;

    static bool prevRt = false;

    // Settle latch: which part the stick is currently ARGUING for, and since when. Statics rather
    // than fields because the picker is a singleton -- one target, one selection, at a time.
    static int aimPendPart = -1;
    static unsigned int aimPendSince = 0;

    // ⚠️ DEFAULTED FALSE HERE, BEFORE ANY EARLY RETURN, AND RE-ASSERTED LOWER DOWN.
    // This function has early returns above the point where the flag used to be computed, so any
    // of them left the previous frame's value standing. Stuck at true it claims the right stick
    // permanently -- and because the guards that read it live in the MOUSE HOOK, which runs inside
    // every modal loop, that killed the cursor everywhere: inventory, barter, dialogue.
    // "joystick ani dpad vubec nereaguje". A per-frame flag must never depend on which branch ran.
    gpui_aim_gesture = false;

    // ⚠️ THE PICKER IS ON LB NOW, NOT LT (his call, 2026-09-16). LB is a DIGITAL button, so there
    // is no Schmitt trigger and no hard-zero debounce to worry about on this side any more -- a
    // shoulder is either down or it is not, which is the whole reason the aiming gesture kept
    // breaking while it lived on an analog axis. RT still needs the analog treatment.
    static bool rtLatch = false;
    static unsigned int rtLatchLowAt = GPUI_TRIGGER_NOT_LOW;
    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool rtNow = gpui_trigger_held(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, &rtLatch, &rtLatchLowAt);
    bool rtEdge = rtNow && !prevRt;
    prevLb = lbNow;
    prevRt = rtNow;

    // R3 = THE CALLED-SHOT WINDOW (2026-10-09). Edge-detected here, above every early return, for
    // the same reason the flag above is: a click read on only some paths is a click from the past.
    static bool prevR3 = false;
    bool r3Now = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    bool r3Edge = r3Now && !prevR3;
    prevR3 = r3Now;


    // Don't run the aim picker while a wheel (skills on LB, drugs on Y, weapons on X, ammo on RB) owns the
    // shared radial overlay.
    if (radial_wheel_is_open()) {
        gpui_aim_picker_close();
        return;
    }

    // Aimed shot possible only in combat, with an aimable weapon and a target, and no modal /
    // inventory / disabled bar over the screen. ⚠️ combat_rt_can_aim is asked ONLY at a press: since
    // R3 also takes a target out of reach (rt_window_target) it runs the lock's facing search, sight
    // lines included, and asked every frame it did that 60 times a second and flooded the trace with
    // lock-pick lines (trace_2026-10-14_204520: 1541 in 2.5 minutes).
    bool uiBlocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled();

    // PICKER UP (LB is down): the right stick moves the mark and RT attacks the chosen part as
    // often as the cooldown allows. LETTING GO OF LB is the way out -- it puts the markers away
    // without firing, which is why looking at a target costs you nothing.
    //
    // ⚠️ B IS NOT A CANCEL HERE ANY MORE: LB+B IS THE HEAL. Two meanings for B on a held LB would
    // be a coin toss in the exact moment you are bleeding, and the release already cancels, so B
    // was the half of that pair with nothing to lose. A B pressed in here falls through to the
    // heal in gpui_button_shortcuts and leaves the picker standing -- deliberately: patching
    // yourself up should not cost you the aim you had lined up.
    if (gpui_aim_body_on) {
        // The picker owns the stick for as long as it is up; this branch returns before the
        // assignment further down, so it has to say so itself.
        gpui_aim_gesture = true;
        Object* target = combat_rt_aim_target();
        if (uiBlocked || !combat_rt_can_aim() || target == NULL || !lbNow) {
            // LB released (or the shot became impossible): put the markers away. NOT a shot.
            gpui_aim_picker_close();
        } else {
            if (rtEdge) {
                trace_event(TRACE_INPUT, "aim-fire", "part=%d", gpui_aim_last_part);
                combat_rt_aimed_attack(gpui_aim_last_part);
            }

            float x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / GPUI_AXIS_MAX;
            float y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / GPUI_AXIS_MAX;

            // SETTLE BEFORE COMMITTING. want = what the stick points at right now; the selection
            // only follows once that same answer has held for GPUI_AIM_SETTLE_MS. Sweeping past a
            // part therefore costs nothing, and a boundary tremor cannot flip anything, because the
            // timer restarts every time the candidate changes.
            int want = gpui_aimbody_pick(target, x, y, gpui_aim_last_part);
            if (want == gpui_aim_last_part) {
                aimPendPart = want; // already there -- nothing pending
            } else if (want != aimPendPart) {
                aimPendPart = want;
                aimPendSince = get_time();
            } else if (get_time() - aimPendSince >= GPUI_AIM_SETTLE_MS) {
                gpui_aim_last_part = want;
            }

            gpui_aimbody_draw(target, gpui_aim_last_part);
        }
        return;
    }

    // ⚠️ R3 = THE CALLED-SHOT WINDOW (2026-10-09, *"R3 bude vybírání končetin"*). It opened on an LB
    // hold until then, and R3 was a lock toggle. For about ten minutes the same evening LB held was
    // the target lock instead -- *"neee LB bude jen reload, srát na zámek"*: LB is the reload and
    // nothing else (its release, gpui_button_shortcuts), and the lock is LT's hold alone.
    // R3: open the called-shot window on the click, or refuse audibly.
    if (r3Edge) {
        // ⚠️ A TARGET IS NOT NEEDED (2026-10-17, "R3 by mělo fungovat i mimo, prostě si
        // předpřipravit aim"): with nobody in reach the window opens anyway, on a stand-in figure and
        // without chances, and binds the part for the shots to come. What still refuses is the WEAPON
        // -- Fast Shot, a burst, a flamer (combat_rt_aim_refusal names which).
        bool blocked = uiBlocked || !combat_rt_weapon_can_aim();
        if (blocked) {
            // ⚠️ THE REFUSAL IS A SOUND, NOT A SENTENCE. It used to print the reason into the
            // message window, because refusing in silence is indistinguishable from a dead button.
            // That was right when the aim was an invisible gesture on a trigger; now a refused hold
            // simply never draws the markers, which says the same thing without a line of text in
            // the console every time a burst weapon is in your hands ("at to nevypisuje takové
            // zbytečnosti do konzole"). The reason still goes to the TRACE, so "why did nothing
            // happen" is still answerable afterwards -- and combat_rt_aim_refusal() stays, because
            // it is the only place that separates Fast Shot from a burst from a flamer.
            const char* why = combat_rt_aim_refusal();
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "aim-refused", "why=%s", why != NULL ? why : "?");
        } else {
            // ⚠️ THE VANILLA CALLED-SHOT WINDOW, NOT OUR ON-BODY PICKER (2026-09-24). His call, said
            // three times in a row so there could be no mistaking it: *"chci jako to je ve vanilla,
            // změníme to úplně"*. So the pad opens the window the game has always had (on R3 since
            // 2026-10-09, on an LB hold before) -- the critter's
            // called-shot picture, eight named locations, the real to-hit chance beside each, Cancel
            // -- with a controller taught to it (combat_called_shot_pick).
            //
            // ⚠️ CHOOSING BINDS, IT DOES NOT FIRE (his design, and it replaced "choosing fires" the
            // same evening): *"držím tlačítko, najedu na tělo a po puštění je vpravo dole nabindován
            // RB na střelbu končetin"*. So the whole thing is ONE gesture -- hold LB, run the
            // selection onto the limb, let go -- and what you get is the part BOUND, printed in the
            // weapon slot beside its bullseye (intface_update_aim_part, fed from gpui_aim_last_part on
            // the tick). Until 2026-10-09 RB fired it; since then binding also puts the weapon slot
            // on its AIMED mode and RT fires it, at the price the slot prints.
            //
            // Nothing in this window pulls a trigger any more, not even A: he had just reported
            // "áčko spustí střelbu" and a picker that fires on confirm is the same complaint one
            // screen further in. A and a second R3 click both mean the same thing here -- take this
            // part -- and B cancels. (Letting go of LB confirmed while LB opened it; see combat.cc.)
            //
            // The on-body picker (gpui_aimbody_*, the hand-painted masks, the stick wedges) is left
            // in the tree but is now unreachable -- ⚠️ DO NOT DELETE IT without asking him: those
            // masks are his own work, painted in Aseprite over several rounds, and "change it
            // completely" was about which UI opens, not about burning that.
            Object* aimTarget = combat_rt_aim_target(); // NULL = nobody in reach: bind ahead
            int part = combat_called_shot_pick(aimTarget, combat_rt_player_hit_mode(), gpui_aim_last_part);
            trace_event(TRACE_INPUT, "aim-picker", "vanilla part=%d tgt=%d", part,
                aimTarget != NULL ? aimTarget->pid : -1);
            if (part >= 0) {
                gpui_aim_part_store(part);
                // ...AND THE SLOT GOES TO ITS AIMED MODE (2026-10-09): binding a part means "aim
                // there", so the next RT does, at the aimed price the slot now prints beside its
                // bullseye. LT+Y steps back out of it. Still nothing is fired here.
                bool aimed = intface_item_set_aiming();
                trace_event(TRACE_INPUT, "aim-bind", "part=%d slot-aimed=%d", part, aimed ? 1 : 0);
                gsound_play_sfx_file("ib1p1xx1");
            }
        }
    }

    // ⚠️ NO STICK CLAIM ON A HELD LB ANY MORE. The picker used to take gpui_aim_gesture for as long
    // as LB was down, and that flag is what stops RT attacking (gamepad_ui_aim_gesture_active) --
    // with LB just the reload, a held LB must not silence RT. The window is modal; it needs no
    // claim from out here.
}

// mouse_info() hook: contributes right-stick cursor movement, the RT-as-left-
// button state, and (when a menu is open) left-stick item snapping. Runs inside
// the engine input pipeline every frame, so movement and clicks pass through the
// same mouse_simulate_input edge detection and win_check_all_buttons cycle as
// the real mouse.
static void gpui_mouse_hook_impl(int* dx, int* dy, int* buttons)
{
    // WHAT THE REAL MOUSE ALREADY SAID THIS FRAME, captured before any emitter below adds to it.
    // Two things need to tell the player's own mouse apart from something we synthesised, and both
    // of them are the STEAM DECK'S RIGHT TRACKPAD: in mouse mode Steam Input feeds it to the game
    // as an ordinary OS-level mouse (motion, and a real left button under the pad click), so it
    // arrives here in *dx/*dy/*buttons exactly like a desktop mouse -- while a controller is
    // connected at the same time, which is the case neither of the two pieces below expected.
    bool realMouseMoved = (*dx != 0 || *dy != 0);
    bool realLeftDown = (*buttons & MOUSE_STATE_LEFT_BUTTON_DOWN) != 0;

    // The player is using the mouse (gamepad_ui_mouse_in_use). Stamped BEFORE the no-pad return
    // below: without a pad the answer is always yes anyway, but the stamp must not depend on it.
    if (realMouseMoved || *buttons != 0) {
        gpui_real_mouse_time = get_time();
    }

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        return;
    }

    // MOVING THE MOUSE SUMMONS THE POINTER, the same way a right-stick nudge does. With a pad
    // connected the cursor is drawn only for GPUI_CURSOR_SHOW_MS after the stick moves it (see
    // gpui_hide_cursor_predicate), and the stamp lived solely in the stick branch below -- so
    // trackpad (or desktop mouse) motion was pushing an INVISIBLE cursor around the world.
    // Draw-only, like the rest of that mechanism: it cannot touch controller input.
    if (realMouseMoved) {
        gpui_cursor_show_time = get_time();
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
    // An ARMED cursor (the rope, a skill) steps between the objects it can be used on, see
    // gpui_tpick_stick; where nothing lies that way it falls through to the free pointer below.
    bool tpickTook = false;
    if (gpui_targeting_cursor_armed() && !lockedGameplay && !radial_wheel_is_open() && !gpui_self_nav
        && !gamepad_ui_menu_active() && !dialog_active() && !inven_any_open()) {
        tpickTook = gpui_tpick_stick(x, y, mag);
    } else if (!gpui_targeting_cursor_armed()) {
        gpui_tpick_reset();
    }

    // Pointing with the stick lights the prompt's target yellow for a moment, one candidate or several
    // (interact_prompt_note_stick, his call 2026-10-17).
    if (mag > gpui_stick_deadzone && !lockedGameplay && !radial_wheel_is_open() && !gpui_aim_body_on
        && !gpui_aim_gesture && !gpui_self_nav && !gamepad_ui_menu_active() && !dialog_active()
        && !inven_any_open() && !gpui_targeting_cursor_armed()) {
        interact_prompt_note_stick();
    }

    bool ipPickTook = false;
    if (!lockedGameplay && !radial_wheel_is_open() && !gpui_aim_body_on && !gpui_aim_gesture && !gpui_self_nav
        && !gamepad_ui_menu_active() && !dialog_active() && !inven_any_open()
        && !gpui_targeting_cursor_armed()
        && interact_prompt_candidate_count() >= 2) {
        // ⚠️ THE PICKER OWNS THE STICK OUTRIGHT while it is available -- at EVERY deflection, not
        // just past the aim threshold. Claiming only the decisive band left every gentle push
        // falling through to the cursor path, which summons the red iso pointer for two full
        // seconds ("stÃÂÃÂ¡le vidÃÂÃÂ­m ÃÂÃÂervenÃÂÃÂ½ kurzor"). Same rule the enemy lock already follows: while
        // something is selected, the right stick selects, full stop.
        ipPickTook = mag > gpui_stick_deadzone;

        // CONTINUOUS, not edge-triggered: re-aimed every frame the stick is out, so circling the
        // stick sweeps the selection round with it. The arm/re-centre ratchet this replaced made
        // you flick-release-flick to walk the outline across a room -- fine for cycling enemies you
        // cannot see, wrong for pointing at a cabinet you can.
        //
        // No refusal sound when nothing lies that way. A stick push is not a button press: it gets
        // nudged constantly while moving and looking around, and buzzing at every one of those is
        // noise. The blip fires only when the selection actually changes (interact_prompt_pick_-
        // direction returns true only then), so a sweep ticks once per object it passes over.
        // fresh = a new flick: the same direction again steps to the next thing that way.
        static bool ipPickOut = false;
        bool ipPickFresh = mag > GPUI_IP_PICK_THRESHOLD && !ipPickOut;
        ipPickOut = mag > GPUI_IP_PICK_THRESHOLD;
        if (mag > GPUI_IP_PICK_THRESHOLD) {
            if (interact_prompt_pick_direction(x / mag, y / mag, ipPickFresh)) {
                gsound_play_sfx_file("ib1p1xx1");
            }
        }
    }

    // ⚠️ IN THE WORLD, THE RIGHT STICK DOES NOT DRIVE THE POINTER. It picks things -- enemies while
    // locked, interactables otherwise -- and when there is nothing to pick it must do NOTHING.
    //
    // It used to fall through to this cursor path instead, which is how a push with no candidate
    // around handed him the iso movement pointer: the stick started steering a walk-here cursor he
    // never asked for, over the top of a game where walking is the LEFT stick's job. Pointing at
    // empty ground is not a request to move there.
    //
    // Screens are the opposite case and keep it: the Pip-Boy, the character sheet, dialogue and the
    // menus have no pad navigation of their own, so there the pointer IS the only way to reach a
    // button. gpui_in_dpad_screen covers the ones that are not modal windows (the Pip-Boy and the
    // char sheet are WINDOW_MOVE_ON_TOP, so the modal test alone misses them).
    // ⚠️ EXCEPT WITH A TARGETING CURSOR ARMED, and that exception is not optional. Holding A with a
    // use-on item (the ROPE, a lockpick, a skill from the L3 wheel) arms the engine's aim cursor,
    // and that cursor is aimed with the POINTER -- there is no other way to say which object you
    // mean. Suppressing the stick there took the rope's aiming away entirely: armed the cursor,
    // then nothing moved ("chtÃÂÃÂl jsem pouÃÂÃÂ¾ÃÂÃÂ­t provaz, ale najednou right joystick nefungoval").
    // The picker already stands aside for the same reason a few lines below.
    bool inWorld = !gamepad_ui_menu_active()
        && !in_main_menu
        && !dialog_active()
        && !inven_any_open()
        && !gpui_in_dpad_screen
        && !gpui_targeting_cursor_armed()
        && select_window_id == -1
        && intface_is_enabled();

    // The world map's exception to self_nav (see gpui_self_nav_cursor). It also stands outside
    // `inWorld`: the interface bar is merely hidden over there, so the world test can read true.
    bool selfNavCursor = gpui_self_nav && gpui_self_nav_cursor && !gamepad_ui_menu_active();

    // ⚠️ THE SPEED IS PIXELS PER CALL, AND THE WORLD MAP CALLS LESS OFTEN. cursor_speed was tuned on
    // screens that run at the shared 60 fps; the world map busy-waits itself down to 24 fps for as
    // long as the party is moving, so a per-call speed would cross the map at 40 % of the speed it
    // has everywhere else -- and the report was exactly "pravá páčka ve world mapě chodí pomaleji".
    // Scaled by the real time since the previous call, relative to a 60 fps frame. Floored at 1, so a
    // screen that runs AT 60 is untouched to the pixel, and capped so a hitch cannot fling the
    // pointer across the screen.
    static unsigned int lastCursorCallMs = 0;
    unsigned int cursorNowMs = get_time();
    float cursorFrameScale = 1.0f;
    if (lastCursorCallMs != 0) {
        cursorFrameScale = (float)(cursorNowMs - lastCursorCallMs) / (1000.0f / 60.0f);
        if (cursorFrameScale < 1.0f) {
            cursorFrameScale = 1.0f;
        } else if (cursorFrameScale > 4.0f) {
            cursorFrameScale = 4.0f;
        }
    }
    lastCursorCallMs = cursorNowMs;

    if (mag > gpui_stick_deadzone && (selfNavCursor || (!inWorld && !gpui_self_nav)) && !ipPickTook && !tpickTook && !lockedGameplay && !radial_wheel_is_open() && !gpui_aim_body_on && !gpui_aim_gesture) {
        float t = (mag - gpui_stick_deadzone) / (1.0f - gpui_stick_deadzone);
        if (t > 1.0f) {
            t = 1.0f;
        }
        float speed = t * t * gpui_cursor_max_speed;
        if (selfNavCursor) {
            // Only here: every other screen that drives the pointer runs at the shared 60 fps, and
            // leaving them alone means none of them can change feel because of this.
            speed *= cursorFrameScale;
        }
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

    // Right trigger -> left mouse button ON THE SCREENS ONLY. RT is what presses the thing under
    // the cursor in menus, the inventory, barter, dialogue, load/save and the character editor.
    //
    // ⚠️ AND NEVER IN THE WORLD, because out there RT IS THE ATTACK (combat_rt reads it directly).
    // Letting the same pull also land a world click meant the engine acted on the cursor -- walk
    // over there, or use whatever it sat on -- instead of, or on top of, the shot. That is "rt má
    // střílet, ne?": the trigger was doing the old A-button's click job while the attack it was
    // supposed to be was fighting it for the same frame.
    //
    // ⚠️ `inWorld` is FALSE while a use-on cursor is armed (gpui_targeting_cursor_armed), which is
    // deliberate: the rope/lockpick crosshair is aimed with the pointer and confirmed with a click,
    // so RT has to reach the world there. That is the one world case that still wants the click.
    if (!inWorld && !gpui_aim_gesture && !gpui_aim_body_on
        && SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GPUI_TRIGGER_THRESHOLD) {
        *buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
    }

    // While a radial wheel (skill on L3) OR the on-body aimed-shot picker (LB) is up, the right
    // stick is selecting and the hold button owns the frame: suppress the face-button
    // click/shortcut actions so a stray A/B/X/Y can't fire a world click or open a menu
    // mid-selection. (Both are driven from gamepad_ui_tick, not here.)
    if (!radial_wheel_is_open() && !gpui_aim_body_on) {
        // A/Y -> left mouse button (interact/attack arbitration) and left-stick
        // navigation of hold-drag action popups. Called UNCONDITIONALLY: it suppresses
        // itself under self_nav, but only after updating its press edges -- see the note
        // at the top of it for why skipping the call outright leaked a click into the
        // world every time a self-nav screen was closed with A.
        gpui_action_buttons(pad, dx, dy, buttons);

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

    // ⚠️ HARD VETO, LAST WORD IN THE HOOK. Gating the individual emitters was not enough: on the
    // character editor A kept pressing whatever GNW button the pointer sat on (PRINT, then DONE),
    // three fixes deep, which means SOME path here sets the left button that I have not found by
    // reading. So a screen that declares "A is mine" now gets the bit STRIPPED after everything
    // has had its say, whoever set it.
    //
    // Scoped so it cannot cost anything else: only while A is actually held, and never while RT is
    // -- RT is the deliberate click, and holding it must still work for the buttons the pad nav
    // does not step through (PRINT / DONE / OPTIONS).
    //
    // ⚠️ AND A REAL MOUSE CLICK IS NOT OURS TO VETO. Only a left button that was synthesised
    // INSIDE this hook is stripped; one that was already down on entry came from the player's own
    // mouse -- on the Steam Deck, the right trackpad's click -- and these are still mouse-driven
    // screens with buttons the pad nav does not step through. Stripping it blindly made the
    // character editor and the New Game selection screen unclickable with a trackpad whenever a
    // controller was plugged in, which on a Deck is always.
    //
    // The trace fires once per veto with a backtrace, so the emitter finally names itself.
    if (gpui_suppress_a_click && !realLeftDown && (*buttons & MOUSE_STATE_LEFT_BUTTON_DOWN) != 0) {
        // NOT conditioned on A being held. An emitter that sets the bit on the RELEASE frame, or
        // one frame late off a latch, would walk straight through such a test -- and after three
        // failed fixes I am not assuming anything about WHEN it is set. The deliberate click is the
        // only one this screen is allowed to produce, so everything else goes.
        bool rtHeldNow = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GPUI_TRIGGER_THRESHOLD;
        if (!rtHeldNow) {
            *buttons &= ~MOUSE_STATE_LEFT_BUTTON_DOWN;
            static unsigned int lastVetoLog = 0;
            if (get_time() - lastVetoLog > 250) {
                lastVetoLog = get_time();
                int mx = 0;
                int my = 0;
                mouse_get_position(&mx, &my);
                trace_event(TRACE_WARN, "a-click-vetoed", "x=%d y=%d", mx, my);
                trace_backtrace("a-click-vetoed");
            }
        }
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
    // ⚠️ AND THE OTHER HALF OF THAT WARNING: A MAP LOAD IS NOT A WORLD TO LOOK AT.
    // The latch below only stops OUR recursion. It does nothing about a FIRST entry that
    // lands inside the engine's own read -- which is what map_load_file's 8 KB mouse pump
    // is, and it crashed the game travelling Shady Sands -> Raiders (trace 2026-09-16,
    // ACCESS_VIOLATION read 0xffffffffffffffff). obj_remove_all had already freed the map
    // and scr_load was mid-way through building the script lists, so the hook's interactable
    // scan (ip_collect -> ip_classify -> scr_ptr) walked a script extent whose length and
    // next pointer were still uninitialised malloc garbage. Nothing the pad does during a
    // load is wanted anyway: there is no world to point at, and the loading cursor is the
    // engine's own. Button edges resolve on the first frame after -- the same as the
    // controller being unplugged for a moment -- so no press survives the load to leak a
    // click into the map you just arrived on.
    if (map_is_loading()) {
        return;
    }

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

    // ⚠️ TRIGGERS ARE NOT BUTTONS AND WERE THEREFORE INVISIBLE. SDL_CONTROLLER_BUTTON_MAX does not
    // cover the two axes, so every LT/RT press was missing from the log -- which is exactly why the
    // "aiming sometimes catches" report could not be answered from a trace and had to be reasoned
    // out of the source instead. Logged with the RAW axis value, not just up/down: a burst of edges
    // whose values all sit near the threshold is the signature of dither, and that is unreadable
    // from a boolean. Uses the same hysteresis as the gesture, so the log shows what it saw.
    {
        static bool prevTrig[2] = { false, false };
        static unsigned int trigLowAt[2] = { GPUI_TRIGGER_NOT_LOW, GPUI_TRIGGER_NOT_LOW };
        const SDL_GameControllerAxis axes[2] = { SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT };
        const char* names[2] = { "LT", "RT" };
        for (int t = 0; t < 2; t++) {
            bool was = prevTrig[t];
            bool now = gpui_trigger_held(pad, axes[t], &prevTrig[t], &trigLowAt[t]);
            if (now != was) {
                trace_event(TRACE_INPUT, "trig", "%s %s raw=%d", names[t], now ? "down" : "up",
                    SDL_GameControllerGetAxis(pad, axes[t]));
            }
        }
    }
}

// The requests the pad's hook and the keyboard leave for the safe tick: the stimpack, the reload with
// its animation, the run to the exit and the called-shot window. All of them use an item, register
// an animation or open a modal window, none of which the input hook may do. Drained with or without
// a controller -- a keyboard player has none.
static void gpui_drain_requests()
{
    // A jab cut before its frame gives its AP back -- before any new press is judged.
    gpui_inject_tick();
    gpui_heal_chain_tick();
    // ...and an explosive whose put-down was cut still lands.
    gpui_place_tick();

    // A quick slot's drug (RB's stimpak, or whatever is bound), then the throw's state machine.
    if (gpui_qs_use_pending_slot != -1) {
        int slot = gpui_qs_use_pending_slot;
        gpui_qs_use_pending_slot = -1;
        int count;
        Object* item = gpui_quick_slot_item(slot, &count);
        if (elapsed_time(gpui_qs_use_pending_since) > GPUI_HEAL_STALE_MS) {
            trace_event(TRACE_INPUT, "quick-slot-stale", "slot=%d", slot);
        } else if (item == NULL || gpui_item_is_throwable(item)) {
            gsound_play_sfx_file("iisxxxx1");
        } else if (gpui_item_is_tool(item)) {
            // A tool goes the Y wheel's way: used outright, or its crosshair armed.
            if (proto_action_can_use(item->pid)) {
                gpui_quick_use_item(item->pid);
            } else {
                gpui_quick_use_on_item(item->pid);
            }
        } else {
            gpui_quick_use_slot = slot; // a held button may chain this one (gpui_chain_slot)
            gpui_quick_use(item->pid);
            gpui_quick_use_slot = -1;
        }
    }
    gpui_qt_tick();

    // The heal, drained before the rest: it is a completed gesture, not an overlay, and doing it here
    // keeps every item use on the same safe tick.
    if (gpui_heal_pending) {
        gpui_heal_pending = false;
        if (elapsed_time(gpui_heal_pending_since) <= GPUI_HEAL_STALE_MS) {
            gpui_use_best_heal();
        } else {
            trace_event(TRACE_INPUT, "heal-stale", "age=%u", elapsed_time(gpui_heal_pending_since));
        }
    }

    // The reload with its animation (MODS "Reload animation"). Waits for the body to come
    // free -- a swing tail, a step -- but not forever: past GPUI_RELOAD_WAIT_MS the press is stale,
    // and acting on it seconds later would read as the game reloading by itself. The drop is AUDIBLE,
    // because a reload that silently never happened is exactly the "Jak je reload?" question again.
    // A reload that took its points and then did not finish -- the loop was replaced by a knockdown,
    // or the player walked or fired out of it -- gives them back. Checked BEFORE a new start below,
    // so a loop registered this very tick is never judged before it has drawn a frame.
    if (gpui_reload_charged && !combat_rt_is_reloading(obj_dude)) {
        gpui_reload_charged = false;
        combat_rt_refund_ap(2);
        trace_event(TRACE_INPUT, "reload-cut", "refund=2 ap=%d anim=%d", combat_rt_player_ap(),
            obj_dude != NULL ? FID_ANIM_TYPE(obj_dude->fid) : -1);
    }

    if (gpui_reload_pending) {
        if (gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled()) {
            gpui_reload_pending = false;
        } else if (gpui_reload_anim_start()) {
            gpui_reload_pending = false;
        } else if (elapsed_time(gpui_reload_pending_since) >= GPUI_RELOAD_WAIT_MS) {
            gpui_reload_pending = false;
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "reload-refused", "why=busy anim=%d stick=%d",
                obj_dude != NULL ? FID_ANIM_TYPE(obj_dude->fid) : -1, combat_rt_stick_walking() ? 1 : 0);
        }
    }

    // RB NO LONGER REPEATS AN AIMED SHOT (2026-10-09). The aimed shot is the weapon slot's AIMED
    // mode now and RT fires it (rt_player_called_loc in combat_rt); RB is the heal, read on its press
    // edge in gpui_button_shortcuts and drained above with the other item uses.

    if (gpui_exit_nav_pending) {
        gpui_exit_nav_pending = false;
        gpui_run_to_exit();
    }

    // The keyboard's called shot (V, or a right-click on an enemy). The same window and the same
    // binding as the pad's R3 in gpui_aim_wheel: choosing a part binds it and puts the weapon slot
    // on AIMED, nothing is fired here. It opens on the enemy the mouse is aiming at, or failing that
    // on whoever the pad's aim would pick.
    if (gpui_called_shot_pending) {
        gpui_called_shot_pending = false;
        Object* target = combat_rt_mouse_target();
        if (target == NULL) {
            target = combat_rt_aim_target();
        }
        // No target is fine here too: the window then binds a part ahead (see the pad's R3).
        bool blocked = gamepad_ui_menu_active() || inven_any_open() || !intface_is_enabled() || !combat_rt_weapon_can_aim();
        if (blocked) {
            const char* why = combat_rt_aim_refusal();
            gsound_play_sfx_file("iisxxxx1");
            trace_event(TRACE_INPUT, "aim-refused", "why=%s src=key", why != NULL ? why : "?");
        } else {
            int part = combat_called_shot_pick(target, combat_rt_player_hit_mode(), gpui_aim_last_part);
            trace_event(TRACE_INPUT, "aim-picker", "vanilla part=%d tgt=%d src=key", part, target != NULL ? target->pid : -1);
            if (part >= 0) {
                gpui_aim_part_store(part);
                bool aimed = intface_item_set_aiming();
                trace_event(TRACE_INPUT, "aim-bind", "part=%d slot-aimed=%d", part, aimed ? 1 : 0);
                gsound_play_sfx_file("ib1p1xx1");
            }
        }
    }
}

bool gamepad_ui_mouse_in_use()
{
    return gamepad_ui_get_controller() == NULL || elapsed_time(gpui_real_mouse_time) < GPUI_MOUSE_IN_USE_MS;
}

void gamepad_ui_request_called_shot()
{
    gpui_called_shot_pending = true;
}

// ⚠️ THE KEYBOARD IS A CONTROLLER (2026-10-10). First *"mouse and keyboard nefunguje bráško, to
// musíme vymyslet pro hráče bez controllerů"*; a twin-stick layout (W/A/S/D + mouse aim) lived for
// one build, then his own design replaced it: *"udělat to stejně jako fallout na controlleru, chůze
// šipkami, akce mezerníkem, ctrl útok, alt běh ... prostě myš vůbec"*. So every key stands in for a
// pad button and goes through the very code that button uses -- the pad itself is not touched:
//
//   arrows  walk (left stick)        Alt (hold)    run (X)          Ctrl (hold)  attack (RT)
//   Shift (hold)  aim lock (LT)      Q / E         lock target left / right (right stick)
//   Space   use what is lit up (A)   H  stimpack (RB)   R  reload (LB)   F  firing mode (LT+Y)
//   V       called shot (R3)         G  run to the nearest exit (BACK held)
//
// The held keys (arrows, Ctrl, Alt, Shift) are read live in combat_rt_poll_gamepad; this function
// takes the presses. Ctrl and Alt are HELD while the others are pressed, so their combined codes
// (Ctrl+R = 18, Alt+R = 275, ...) count as the plain key -- otherwise reloading while attacking
// would do nothing, and Ctrl+Q would ask whether to QUIT the game in the middle of a fight. Ctrl+H
// is the one left out: its code is Backspace's, and Backspace must not burn a stimpack. Arrow codes
// are swallowed too, or vanilla would scroll the map under the walking dude. Runs from the main loop
// only, so a text field (a save name) never gets here. Returns true when the key was used.
bool gamepad_ui_handle_key(int* keyCode)
{
    int key = *keyCode;
    if (key < 0 || obj_dude == NULL || gamepad_ui_menu_active()) {
        return false;
    }

    switch (key) {
    case KEY_ARROW_UP:
    case KEY_ARROW_DOWN:
    case KEY_ARROW_LEFT:
    case KEY_ARROW_RIGHT:
    case KEY_CTRL_ARROW_UP:
    case KEY_CTRL_ARROW_DOWN:
    case KEY_CTRL_ARROW_LEFT:
    case KEY_CTRL_ARROW_RIGHT:
    case KEY_ALT_ARROW_UP:
    case KEY_ALT_ARROW_DOWN:
    case KEY_ALT_ARROW_LEFT:
    case KEY_ALT_ARROW_RIGHT:
        return true;
    case KEY_SPACE: {
        bool queued = interact_prompt_request();
        trace_event(TRACE_INPUT, "kbm-key", "key=SPACE queued=%d", queued ? 1 : 0);
        return true;
    }
    case KEY_LOWERCASE_Q:
    case KEY_UPPERCASE_Q:
    case KEY_CTRL_Q:
    case KEY_ALT_Q:
        combat_rt_request_lock_step(-1);
        return true;
    case KEY_LOWERCASE_E:
    case KEY_UPPERCASE_E:
    case KEY_CTRL_E:
    case KEY_ALT_E:
        combat_rt_request_lock_step(1);
        return true;
    case KEY_LOWERCASE_H:
    case KEY_UPPERCASE_H:
    case KEY_ALT_H:
        // H = the heal: the LB quick slot since the swap (its own pick is the best heal). A throwable
        // bound to it is drawn and thrown in one go: the keyboard reports no release.
        if (!radial_wheel_is_open() && !inven_any_open()) {
            trace_event(TRACE_INPUT, "heal-press", "src=key");
            gpui_qs_press(GPUI_QS_LB, true);
        }
        return true;
    case KEY_LOWERCASE_T:
    case KEY_UPPERCASE_T:
    case KEY_CTRL_T:
    case KEY_ALT_T:
        // T = the grenade: the RB quick slot since the swap. Drawn and thrown in one go (or the drug
        // bound to it taken).
        if (!radial_wheel_is_open() && !inven_any_open()) {
            trace_event(TRACE_INPUT, "kbm-key", "key=T");
            gpui_qs_press(GPUI_QS_RB, true);
        }
        return true;
    case KEY_LOWERCASE_R:
    case KEY_UPPERCASE_R:
    case KEY_CTRL_R:
    case KEY_ALT_R:
        trace_event(TRACE_INPUT, "kbm-key", "key=R");
        gpui_request_reload();
        return true;
    case KEY_LOWERCASE_F:
    case KEY_UPPERCASE_F:
    case KEY_CTRL_F:
    case KEY_ALT_F:
        if (!radial_wheel_is_open()) {
            gpui_cycle_fire_mode("key");
        }
        return true;
    case KEY_LOWERCASE_V:
    case KEY_UPPERCASE_V:
    case KEY_CTRL_V:
    case KEY_ALT_V:
        trace_event(TRACE_INPUT, "kbm-key", "key=V");
        gpui_called_shot_pending = true;
        return true;
    case KEY_LOWERCASE_G:
    case KEY_UPPERCASE_G:
    case KEY_CTRL_G:
    case KEY_ALT_G:
        trace_event(TRACE_INPUT, "kbm-key", "key=G");
        gpui_exit_nav_pending = true;
        return true;
    }

    return false;
}

// ⚠️ THE QUICK SLOTS ANSWER INSIDE THE INVENTORY (2026-10-17, "když dám LB nebo RB na item v inventory,
// tak se to neprojeví hned, až po vypnutí inventory"). The slots beside the bar are only kept in step
// by gamepad_ui_tick, and the inventory runs its own loop, so a binding (or a stimpak used from the
// list) showed only once the screen closed. The binding itself happens in the input hook, which must
// not read protos (gpui_quick_slot_item does -- see gpui_hostile_in_range_cached), so the redraw
// lives here, called by that loop where the quick-action poll already runs. Both draws are no-ops
// unless the item or the count changed.
void gamepad_ui_inventory_tick()
{
    if (obj_dude == NULL) {
        return;
    }
    int count;
    Object* item = gpui_quick_slot_item(GPUI_QS_RB, &count);
    intface_update_heal_slot(item, count, gpui_qs_drawn(GPUI_QS_RB));
    item = gpui_quick_slot_item(GPUI_QS_LB, &count);
    intface_update_quick_slot_lb(item, count, gpui_qs_drawn(GPUI_QS_LB));
}

void gamepad_ui_tick()
{
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        // Controller vanished mid-selection: drop the overlay so it can't linger.
        if (radial_wheel_is_open()) {
            radial_wheel_close();
            gpui_wheel_owner = GPUI_WHEEL_NONE;
        }
        // No controller -> restore the pointer (a mouse/keyboard player needs it).
        gmouse_set_move_cursor_hidden(false);

        // ...and everything a keyboard player asks for still needs this tick: the quick slots beside
        // the bar, the requests (H, T, R, G, V) and the use prompt Space acts on.
        {
            int count;
            Object* item = gpui_quick_slot_item(GPUI_QS_RB, &count);
            intface_update_heal_slot(item, count, gpui_qs_drawn(GPUI_QS_RB));
            item = gpui_quick_slot_item(GPUI_QS_LB, &count);
            intface_update_quick_slot_lb(item, count, gpui_qs_drawn(GPUI_QS_LB));
        }
        intface_update_aim_part(gpui_aim_last_part);
        gpui_drain_requests();
        interact_prompt_tick();
        return;
    }

    SDL_GameControllerUpdate();
    gpui_trace_buttons(pad);

    // Sample the hostile-in-range answer HERE (safe main-loop context), not in the mouse hook --
    // the query reads protos off disk and the hook can run from inside the engine's file I/O.
    gpui_hostile_in_range_cached = combat_rt_hostile_in_attack_range();

    // A = SELECT for every radial wheel, sampled ONCE here so all five wheels share one edge.
    // Sampled every tick whether or not a wheel is open, and that is the point: A doubles as the
    // world attack/use button, so a press carried in from gameplay must already be counted as
    // "down" by the time a wheel opens, or it would select on the wheel's first frame. Releasing
    // the button that holds the wheel open still selects too -- A is the visible alternative, and
    // it is the one the panel advertises (radial_wheel.cc, wheel_draw_hint).
    {
        static bool prevA = false;
        bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
        gpui_wheel_a_edge = aNow && !prevA;
        prevA = aNow;
    }

    // Keep the heal slot (the side panel right of the bar, or the bar itself on a 640-wide screen)
    // in step with the pack. Polled rather than pushed: a
    // stimpak can leave the inventory through looting, bartering, a script or a load, and this
    // is the one place that sees all of them. intface_update_heal_slot is a no-op unless the
    // item or the count actually changed.
    {
        int count;
        Object* item = gpui_quick_slot_item(GPUI_QS_RB, &count);
        intface_update_heal_slot(item, count, gpui_qs_drawn(GPUI_QS_RB));
        item = gpui_quick_slot_item(GPUI_QS_LB, &count);
        intface_update_quick_slot_lb(item, count, gpui_qs_drawn(GPUI_QS_LB));
    }

    // ...and the aimed part for the weapon slot, which prints it beside the bullseye while the slot is
    // AIMED: the remembered part is what RT fires at then.
    // Also a no-op unless it changed, so polling costs a compare.
    intface_update_aim_part(gpui_aim_last_part);

    // The requests from the hook and the keyboard (heal, reload, exit run, called shot).
    gpui_drain_requests();

    gpui_skill_wheel(pad);
    gpui_quick_wheel(pad);
    gpui_weapon_wheel(pad);
    gpui_aim_wheel(pad);
    gpui_useon_arm(pad);
    gpui_wheel_a_edge = false; // consumed by whichever wheel was open

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
    gpui_aim_part_load(); // the remembered aimed part (gpui_aim_part_store)
    // Cursor feel. Clamped so a bad cfg can't make the cursor immovable or uncontrollable, and the
    // deadzone is capped well below 1 so there is always a usable band left above it.
    double cursorSpeed = GPUI_CURSOR_MAX_SPEED;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CURSOR_SPEED_KEY, &cursorSpeed);
    if (cursorSpeed < 1.0) {
        cursorSpeed = 1.0;
    } else if (cursorSpeed > 60.0) {
        cursorSpeed = 60.0;
    }
    gpui_cursor_max_speed = (float)cursorSpeed;

    double cursorDead = GPUI_STICK_DEADZONE;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CURSOR_DEADZONE_KEY, &cursorDead);
    if (cursorDead < 0.0) {
        cursorDead = 0.0;
    } else if (cursorDead > 0.8) {
        cursorDead = 0.8;
    }
    gpui_stick_deadzone = (float)cursorDead;

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
