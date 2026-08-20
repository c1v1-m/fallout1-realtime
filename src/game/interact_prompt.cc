#include "game/interact_prompt.h"

#include <string.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/combat_rt.h"
#include "game/critter.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/radial_wheel.h"
#include "game/scripts.h"
#include "game/skill_defs.h"
#include "game/tile.h"
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"

namespace fallout {

// How near (hex tiles) an interactable must be for the prompt to offer it. The engine walks the
// dude the rest of the way when A fires, so this is just "close enough that you clearly mean it".
#define IP_RANGE_TILES 4

// In combat, an interactable overrides the A-attack only when the dude is standing RIGHT at it
// (<= this many tiles). Keeps doors/corpses openable mid-fight when you walk up to them, while a
// bystander a few tiles off never steals A from firing. Tight on purpose (1 = adjacent).
#define IP_INTERACT_ADJACENT_TILES 1

// Interface FRM numbers of the action-menu icons (see gmouse_3d_action_nums[] in gmouse.cc):
// the "Use/Get" hand, the "Talk" face, the "Look" eye and the "Skill" icon.
#define IP_HAND_ART_NUM 265
#define IP_TALK_ART_NUM 263
#define IP_LOOK_ART_NUM 259
#define IP_SKILL_ART_NUM 304

// What A does with the prompted object -- mirrors the vanilla left-click (ARROW) dispatch.
enum {
    IP_NONE = 0,
    IP_USE,            // usable scenery (door, lever, terminal) -> action_use_an_object
    IP_GET,            // loose item on the ground -> action_get_an_object
    IP_TALK,           // living, talkable critter -> action_talk_to
    IP_LOOT_CORPSE,    // dead critter -> action_loot_container
    IP_LOOT_CONTAINER, // container item -> action_use_an_object (opens it)
    IP_LOCKPICK,       // LOCKED door / container -> action_use_skill_on(SKILL_LOCKPICK)
    IP_LOOK,           // examine -- the vanilla action menu's eye
};

static Object* ip_target = NULL;   // interactable currently highlighted (re-derived each tick)
static Object* ip_pending = NULL;  // queued by the A hook, performed from the tick
static int ip_pending_kind = IP_NONE;

// The actions the D-pad cycles through for ip_target, the DEFAULT one first (index 0), and which
// of them is currently selected. Rebuilt every tick from the live target; the index is reset only
// when the target itself changes, so a chosen action survives while you keep facing the same thing.
#define IP_ACTIONS_MAX 4
static int ip_actions[IP_ACTIONS_MAX];
static int ip_actions_count = 0;
static int ip_action_index = 0;

// How closely a candidate must line up with the stick push to count as "that way" (cosine, so 0.5
// is a 60-degree half-angle = a 120-degree cone). Wide enough that you don't have to aim precisely,
// tight enough that a flick right never grabs something behind you.
#define IP_PICK_CONE_COS 0.5f

// How much distance may pull the choice away from pure alignment. Small on purpose: the stick says
// the DIRECTION, and distance is only here to settle two things sitting along the same line.
#define IP_PICK_DIST_WEIGHT 0.25f

// How much better a rival must score before it steals the selection. Stops two similarly-placed
// objects trading the outline back and forth every frame while the stick rests between them.
#define IP_PICK_SWITCH_MARGIN 0.08f

// A target the PLAYER chose with the right stick, which overrides the automatic nearest-and-highest-
// priority pick. Expires on its own (IP_MANUAL_HOLD_MS since the last flick) so walking away from a
// hand-picked object quietly hands control back to the automatic pick instead of stranding the
// prompt on something behind you.
#define IP_MANUAL_HOLD_MS 4000
static Object* ip_manual_target = NULL;
static unsigned int ip_manual_since = 0;

// How far the right-stick picker reaches, as opposed to IP_RANGE_TILES (4) which is how close
// something must be for the prompt to offer it on its own. Pointing at a thing says what you mean,
// so it does not need the proximity guess -- but it stays bounded so the picker can't grab the far
// side of a warehouse, and everything it offers must additionally be ON SCREEN.
#define IP_PICK_RANGE_TILES 12

static bool ip_on_screen(Object* obj); // defined with the picker below

static Object* ip_outlined = NULL; // object we placed an outline on, to restore it exactly
static int ip_outline_saved = 0;

static int ip_win = -1;            // floating icon overlay window
static int ip_icon_num = 0;        // interface FRM number currently shown (hand vs talk face)
static int ip_hand_w = 0;          // current icon dimensions
static int ip_hand_h = 0;
static int ip_win_x = -32000;
static int ip_win_y = -32000;

// Classify an object into an interaction kind (0 = not interactable).
// True only if the critter's script actually has a TALK procedure -- i.e. pressing A would produce
// real dialogue (or a "moo"). This is the exact thing gdialog runs (exec_script_proc SCRIPT_PROC_TALK,
// gdialog.cc). The engine's obj_action_can_talk_to returns true for EVERY critter, so it can't tell a
// conversational NPC (or a brahmin) apart from a wild rat; the proc table can. procs[] is populated
// when the script's program loads (on map-enter/start), so it's reliable for on-map critters.
static bool ip_critter_can_converse(Object* obj)
{
    if (obj->sid == -1) {
        return false;
    }
    Script* script;
    if (scr_ptr(obj->sid, &script) == -1) {
        return false;
    }
    return script->procs[SCRIPT_PROC_TALK] != SCRIPT_PROC_NO_PROC;
}

static int ip_classify(Object* obj)
{
    if (obj == NULL || obj == obj_dude) {
        return IP_NONE;
    }

    switch (FID_TYPE(obj->fid)) {
    case OBJ_TYPE_ITEM:
        if (item_get_type(obj) == ITEM_TYPE_CONTAINER) {
            // A LOCKED chest offers the pick instead of a loot window that would just refuse.
            return obj_is_locked(obj) ? IP_LOCKPICK : IP_LOOT_CONTAINER;
        }
        return IP_GET;
    case OBJ_TYPE_SCENERY: {
        if (!proto_action_can_use(obj->pid)) {
            return IP_NONE;
        }
        // A LOCKED door: A picks it rather than rattling the handle. Reaching the lockpick any
        // other way on a controller is L3 -> wheel -> Lockpick -> aim the cursor -> A, which is
        // why pressing A at the door (the obvious move) appeared to do nothing. Once it's open
        // (or was never locked) this falls through to the normal use.
        //
        // ⚠️ ONLY A DOOR CAN BE LOCKED, and the type test is load-bearing. obj_is_locked() reads
        // data.scenery.door.openFlags unconditionally (protinst.cc), but that union member ALIASES
        // ladder.destinationBuiltTile / stairs.destinationMap -- so on a ladder it is testing a
        // destination tile for DOOR_FLAG_LOCKED and getting whatever bit 25 of that tile happens to
        // be. Vanilla never noticed because it only ever calls obj_is_locked on doors and
        // containers; we were the first caller to hand it arbitrary scenery. The result was that
        // ladders read as LOCKED, so A offered a lockpick instead of climbing them -- "I can't get
        // down to Doc Morbid and I can't get back up the ladder either" (trace_2026-08-13_155532:
        // 14x `interact-do kind=6` on pid 33555016, a SCENERY_TYPE_LADDER_DOWN, plus 2x on the
        // LADDER_UP pid 33554571). Left obj_is_locked itself alone: it is a vanilla function and
        // script opcodes can reach it.
        Proto* proto;
        bool isDoor = proto_ptr(obj->pid, &proto) != -1 && proto->scenery.type == SCENERY_TYPE_DOOR;
        return (isDoor && obj_is_locked(obj)) ? IP_LOCKPICK : IP_USE;
    }
    case OBJ_TYPE_CRITTER:
        if ((obj->data.critter.combat.results & DAM_DEAD) != 0) {
            // DAM_DEAD is set the instant the kill lands, but the death-fall animation is still
            // playing -- offering the loot hand mid-fall looks premature (the body is still standing
            // as it topples). Wait until the corpse has finished falling (anim done, fully prone)
            // before advertising it as lootable. A stuck/dead-standing corpse is force-reaped
            // elsewhere (combat_rt RT_DEAD_STANDING_TIMEOUT), so this won't hang the prompt forever.
            if (anim_busy(obj) != 0) {
                return IP_NONE;
            }
            return IP_LOOT_CORPSE;
        }
        // A critter that is ACTUALLY FIGHTING YOU is never a talk target -- you fight it back.
        // Since the interact prompt no longer yields to "a hostile is in weapon range" (so doors
        // work with the gun out), an enemy mid-swing would otherwise pop the TALK hand over
        // something you mean to shoot. Excluding it makes interact_prompt_target() NULL for that
        // enemy, so A routes to the attack path.
        //
        // ⚠️ THIS USED TO TEST combat_rt_is_hostile(), THE RAW STICKY FLAG, AND THAT WAS A TRAP.
        // Faction aggro sets `hostile` on every onlooker of the victim's team, the flag is never
        // cleared, and (since the perception-gated re-aggro) such a critter may never actually
        // come at you. The result was a dead end the player walked straight into after clearing
        // Gizmo's guards: a bystander who would not fight, could not be talked to, and offered no
        // prompt of any kind -- permanently. combat_rt_is_engaging() asks whether it is fighting
        // you NOW, so a passive one becomes a person again and its own script decides what it says.
        if (combat_rt_is_engaging(obj)) {
            return IP_NONE;
        }
        // No talking WHILE SOMETHING IS ACTUALLY ON YOU. Dialogue and a live fight are mutually
        // exclusive engine states, so offering a talk hand that gdialog_enter would refuse just
        // advertises an action A cannot perform.
        //
        // This used to test isInCombat(), and that was far too broad in realtime: the warm combat
        // bit stays set while ANY critter anywhere is engaged, so the prompt refused to offer talk
        // to anyone, for minutes at a time. That is what the player hit after clearing Gizmo's
        // guards -- the man was neither hostile nor talkable. combat_rt_under_threat() asks the
        // question this rule actually meant, and gdialog_enter now uses the same one.
        if (combat_rt_under_threat()) {
            return IP_NONE;
        }
        // Offer TALK only when talking would actually do something -- i.e. the critter has a real
        // talk script proc. This attacks the "I can't hit the rats, the dude keeps talking to them"
        // bug (wild animals have no talk proc -> IP_NONE -> A attacks) while keeping talkable BEASTS
        // like a brahmin (which DO have a talk/"moo" proc) selectable. Supersedes the old body-type
        // gate, which wrongly dropped quadruped talkers (the brahmin) along with the beasts.
        if (obj_action_can_talk_to(obj) && ip_critter_can_converse(obj)) {
            return IP_TALK;
        }
        return IP_NONE;
    default:
        return IP_NONE;
    }
}

// The actions a D-pad tap cycles through for `obj`, DEFAULT FIRST. Deliberately short: these are
// the actions that actually exist for the thing you are standing at, not the whole vanilla action
// menu. LOOK is on every entry because "what is this?" applies to everything and had no pad route
// at all -- the floating icon was permanently the hand.
//
// Vanilla's remaining two menu entries (use-an-ITEM-on, use-a-SKILL-on) are left out: the skill
// wheel (LB) already covers skills, and use-item-on opens a modal picker that would want its own
// gamepad pass. Easy to add here later -- the dispatch in interact_prompt_tick is one case each.
static int ip_build_actions(Object* obj, int* out)
{
    int n = 0;
    int def = ip_classify(obj);
    if (def == IP_NONE) {
        return 0;
    }

    out[n++] = def;

    // A LOCKED thing defaults to the pick, but the plain action stays reachable one tap away: the
    // lock may already be open (a script, a key, someone else's pick) while obj_is_locked lags, and
    // rattling a locked door is at least honest feedback. Same for a locked chest.
    if (def == IP_LOCKPICK) {
        out[n++] = FID_TYPE(obj->fid) == OBJ_TYPE_SCENERY ? IP_USE : IP_LOOT_CONTAINER;
    }

    out[n++] = IP_LOOK;
    return n;
}

// The action A would perform right now: the selected entry, or the plain classification when the
// cycle has not been built yet (nothing has ticked since the target appeared).
static int ip_chosen_kind()
{
    if (ip_target == NULL) {
        return IP_NONE;
    }
    if (ip_actions_count > 0 && ip_action_index < ip_actions_count) {
        return ip_actions[ip_action_index];
    }
    return ip_classify(ip_target);
}

// Deliberate-interaction kinds (doors/scenery, containers, corpses, NPCs) rank ABOVE picking up a
// loose ground item, so standing next to a bookcase with a dropped book on the floor targets the
// bookcase, not the book. Within the same rank, nearest wins.
static int ip_priority(int kind)
{
    return kind == IP_GET ? 0 : 1;
}

// Best interactable within range, or NULL. Sets *kindOut to its interaction kind.
static Object* ip_find_nearest(int* kindOut)
{
    // A hand-picked target (right-stick flick) wins over the automatic choice for as long as it
    // stays valid and in reach. Re-validated here every tick rather than trusted, because the
    // object can die, be looted empty, be picked up, or be walked away from between frames.
    if (ip_manual_target != NULL) {
        int kind = ip_classify(ip_manual_target);
        bool stale = get_time() - ip_manual_since > IP_MANUAL_HOLD_MS;
        // Validated against the PICKER's reach, not the automatic one -- a deliberately chosen
        // cabinet across the room would otherwise be discarded on the very next tick.
        if (kind != IP_NONE && !stale && obj_dist(obj_dude, ip_manual_target) <= IP_PICK_RANGE_TILES
            && ip_on_screen(ip_manual_target)) {
            if (kindOut != NULL) {
                *kindOut = kind;
            }
            return ip_manual_target;
        }
        ip_manual_target = NULL;
    }

    Object* best = NULL;
    int bestDist = 0;
    int bestPrio = -1;
    int bestKind = IP_NONE;

    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        int kind = ip_classify(obj);
        if (kind == IP_NONE) {
            continue;
        }
        int d = obj_dist(obj_dude, obj);
        if (d > IP_RANGE_TILES) {
            continue;
        }
        int prio = ip_priority(kind);
        // A following party member is almost always the closest talkable critter, so it would
        // hog the prompt and block talking to the NPC you actually walked up to. Drop companions
        // BELOW everything else so any other interactable in range wins; you can still talk to a
        // companion (to manage/trade) when nothing else is nearby.
        if (kind == IP_TALK && isPartyMember(obj)) {
            prio = -1;
        }
        if (best == NULL || prio > bestPrio || (prio == bestPrio && d < bestDist)) {
            bestPrio = prio;
            bestDist = d;
            best = obj;
            bestKind = kind;
        }
    }

    if (kindOut != NULL) {
        *kindOut = bestKind;
    }
    return best;
}

// Is `obj` inside the map window? MANDATORY for the picker, not a nicety: stepping the selection
// onto something round a corner that the player cannot see is worse than not having the feature.
// Same test the enemy lock uses to drop an off-screen target (combat_rt.cc).
static bool ip_on_screen(Object* obj)
{
    if (obj == NULL || display_win == -1) {
        return true;
    }
    Rect win;
    if (win_get_rect(display_win, &win) == -1) {
        return true;
    }
    Rect b;
    obj_bound(obj, &b);
    return !(b.lrx < win.ulx || b.ulx > win.lrx || b.lry < win.uly || b.uly > win.lry);
}

// Everything the right-stick picker may select. Deliberately a WIDER net than ip_find_nearest's:
// the automatic pick stays close (IP_RANGE_TILES) because it has to guess what you meant, while
// pointing the stick at something is unambiguous, so it may reach across the room. Pressing A then
// walks you there -- exactly what a mouse click has always done.
#define IP_CANDIDATES_MAX 24

static int ip_collect(Object** out, int max)
{
    int n = 0;
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL && n < max; obj = obj_find_next_at()) {
        if (ip_classify(obj) == IP_NONE) {
            continue;
        }
        if (obj_dist(obj_dude, obj) > IP_PICK_RANGE_TILES) {
            continue;
        }
        if (!ip_on_screen(obj)) {
            continue;
        }
        out[n++] = obj;
    }
    return n;
}

int interact_prompt_candidate_count()
{
    if (obj_dude == NULL) {
        return 0;
    }

    // Off entirely while something is in weapon range. A distant pick commits the dude to a long
    // walk, and starting one mid-firefight is never what you meant -- the prompt itself already
    // stands down there for anything non-adjacent, so this just follows it.
    if (combat_rt_hostile_in_attack_range()) {
        return 0;
    }

    Object* cand[IP_CANDIDATES_MAX];
    return ip_collect(cand, IP_CANDIDATES_MAX);
}

// Screen-centre of an object, for direction comparisons.
static void ip_screen_pos(Object* obj, int* x, int* y)
{
    Rect r;
    obj_bound(obj, &r);
    *x = (r.ulx + r.lrx) / 2;
    *y = (r.uly + r.lry) / 2;
}

// ABSOLUTE aim, not a step. The stick direction names WHERE the thing is relative to the dude, and
// this is called every frame the stick is deflected -- so sweeping the stick round sweeps the
// selection round with it, continuously, instead of ratcheting one object per flick.
//
// (The first version stepped to the nearest neighbour of the CURRENT target, edge-triggered with a
// re-centre. That is the right model for cycling ENEMIES, where you rarely know where the next one
// is and just want "the next one that way" -- but for pointing at furniture you already know where
// it is, and having to flick-release-flick to walk the selection over there is the opposite of
// seamless.)
//
// Returns true only when the selection actually CHANGED, so the caller can blip once per change
// rather than once per frame.
bool interact_prompt_pick_direction(float dirX, float dirY)
{
    if (obj_dude == NULL) {
        return false;
    }

    Object* cand[IP_CANDIDATES_MAX];
    int count = ip_collect(cand, IP_CANDIDATES_MAX);
    if (count < 2) {
        return false;
    }

    int fromX;
    int fromY;
    ip_screen_pos(obj_dude, &fromX, &fromY);

    Object* best = NULL;
    float bestScore = 0.0f;
    float currentScore = 0.0f;
    bool haveCurrent = false;

    for (int i = 0; i < count; i++) {
        int tx;
        int ty;
        ip_screen_pos(cand[i], &tx, &ty);

        float vx = (float)(tx - fromX);
        float vy = (float)(ty - fromY);
        float len = sqrtf(vx * vx + vy * vy);
        if (len < 1.0f) {
            continue; // standing on it
        }

        // Alignment with the push comes first; distance only breaks near-ties, so two things in the
        // same direction resolve to the closer one instead of the selection leaping across the room.
        float dot = (vx * dirX + vy * dirY) / len;
        if (dot < IP_PICK_CONE_COS) {
            continue;
        }

        float dist = (float)obj_dist(obj_dude, cand[i]);
        float score = dot - IP_PICK_DIST_WEIGHT * (dist / (float)IP_PICK_RANGE_TILES);

        if (cand[i] == ip_target) {
            currentScore = score;
            haveCurrent = true;
        }

        if (best == NULL || score > bestScore) {
            best = cand[i];
            bestScore = score;
        }
    }

    if (best == NULL || best == ip_target) {
        return false;
    }

    // HYSTERESIS. Without it two objects at nearly the same angle swap back and forth every frame
    // while the stick sits between them -- the outline flickers and the blip machine-guns. The
    // newcomer has to clearly beat what is already selected before it takes over.
    if (haveCurrent && bestScore - currentScore < IP_PICK_SWITCH_MARGIN) {
        return false;
    }

    ip_manual_target = best;
    ip_manual_since = get_time();
    return true;
}

static void ip_clear_outline()
{
    if (ip_outlined == NULL) {
        return;
    }
    Object* o = ip_outlined;
    ip_outlined = NULL;
    o->outline = ip_outline_saved; // restore the exact prior outline (usually none)
    Rect r;
    obj_bound(o, &r);
    tile_refresh_rect(&r, o->elevation);
}

static void ip_set_outline(Object* obj)
{
    if (ip_outlined == obj) {
        return;
    }
    ip_clear_outline();
    ip_outline_saved = obj->outline;
    ip_outlined = obj;
    obj_outline_object(obj, OUTLINE_TYPE_ITEM, NULL);
    Rect r;
    if (obj_turn_on_outline(obj, &r) == 0) {
        tile_refresh_rect(&r, obj->elevation);
    } else {
        tile_refresh_display();
    }
}

static void ip_hide_overlay()
{
    if (ip_win != -1) {
        win_delete(ip_win);
        ip_win = -1;
    }
    ip_win_x = ip_win_y = -32000;
}

// Which action-menu icon floats for a given interaction kind. Same art the vanilla action menu
// uses, so the floating icon and that menu speak the same language.
static int ip_icon_art_num(int kind)
{
    switch (kind) {
    case IP_TALK:
        return IP_TALK_ART_NUM;
    case IP_LOOK:
        return IP_LOOK_ART_NUM;
    case IP_LOCKPICK:
        // The skilldex icon, not the hand: a lockpick attempt is a SKILL roll, and drawing it as
        // the plain "use" hand is what made a mis-classified ladder impossible to spot on screen.
        return IP_SKILL_ART_NUM;
    default:
        return IP_HAND_ART_NUM;
    }
}

// Blit the current icon (frame 0, index-0 transparent) into the overlay window and present it.
static void ip_draw_icon()
{
    if (ip_win == -1) {
        return;
    }
    unsigned char* buf = win_get_buf(ip_win);
    if (buf == NULL) {
        return;
    }
    memset(buf, 0, (size_t)ip_hand_w * ip_hand_h); // index 0 = transparent (WINDOW_TRANSPARENT)

    CacheEntry* handle;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, ip_icon_num, 0, 0, 0), &handle);
    if (art != NULL) {
        unsigned char* src = art_frame_data(art, 0, 0);
        int sw = art_frame_width(art, 0, 0);
        int sh = art_frame_length(art, 0, 0);
        for (int y = 0; y < sh && y < ip_hand_h; y++) {
            for (int x = 0; x < sw && x < ip_hand_w; x++) {
                unsigned char p = src[y * sw + x];
                if (p != 0) {
                    buf[y * ip_hand_w + x] = p;
                }
            }
        }
        art_ptr_unlock(handle);
    }

    win_draw(ip_win);
}

// Float the icon for `kind` just to the RIGHT of the object (vertically centered), tracking it.
static void ip_show_overlay(Object* obj, int kind)
{
    int iconNum = ip_icon_art_num(kind);

    int iw = 0;
    int ih = 0;
    CacheEntry* handle;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, iconNum, 0, 0, 0), &handle);
    if (art != NULL) {
        iw = art_frame_width(art, 0, 0);
        ih = art_frame_length(art, 0, 0);
        art_ptr_unlock(handle);
    }
    if (iw == 0 || ih == 0) {
        return;
    }

    Rect r;
    obj_bound(obj, &r); // object bounding rect in screen coordinates

    int sw = screenGetWidth();
    int sh = screenGetHeight();

    // Just to the RIGHT of the object, vertically centered on it. No room on the right (near the
    // screen edge) -> flip to the left side.
    int x = r.lrx + 2;
    if (x + iw > sw) {
        x = r.ulx - iw - 2;
    }
    if (x < 0) {
        x = 0;
    } else if (x + iw > sw) {
        x = sw - iw;
    }

    // Nudged up from the vertical centre so the icon floats over the object's upper part.
    int y = (r.uly + r.lry) / 2 - ih / 2 - 32;
    if (y < 0) {
        y = 0;
    } else if (y + ih > sh) {
        y = sh - ih;
    }

    // Recreate the window when the icon or its size changes; otherwise move it to track.
    if (ip_win == -1 || iconNum != ip_icon_num || iw != ip_hand_w || ih != ip_hand_h) {
        ip_hide_overlay();
        ip_icon_num = iconNum;
        ip_hand_w = iw;
        ip_hand_h = ih;
        ip_win = win_add(x, y, iw, ih, colorTable[0], WINDOW_MOVE_ON_TOP | WINDOW_TRANSPARENT);
        if (ip_win == -1) {
            return;
        }
        ip_win_x = x;
        ip_win_y = y;
        ip_draw_icon();
    } else if (x != ip_win_x || y != ip_win_y) {
        win_move(ip_win, x, y);
        ip_win_x = x;
        ip_win_y = y;
        ip_draw_icon();
    }
}

void interact_prompt_tick()
{
    // 1) Perform a queued interaction FIRST, from this safe main-loop context (never from the
    // input hook). Tear the overlay down before a talk/loot modal opens over it.
    if (ip_pending != NULL) {
        Object* obj = ip_pending;
        int kind = ip_pending_kind;
        ip_pending = NULL;
        ip_pending_kind = IP_NONE;

        ip_clear_outline();
        ip_hide_overlay();
        ip_target = NULL;

        int rc = -2;
        switch (kind) {
        case IP_USE:
            rc = action_use_an_object(obj_dude, obj);
            break;
        case IP_GET:
            rc = action_get_an_object(obj_dude, obj);
            break;
        case IP_TALK:
            rc = action_talk_to(obj_dude, obj);
            break;
        case IP_LOOT_CORPSE:
            rc = action_loot_container(obj_dude, obj);
            break;
        case IP_LOOT_CONTAINER:
            // A container ITEM opens via action_get_an_object -> obj_use_container (opens the loot
            // window). action_use_an_object -> obj_use is SCENERY-only (returns -1 for an item), so
            // it silently did nothing -- that was the "can't loot containers, only corpses" bug.
            rc = action_get_an_object(obj_dude, obj);
            break;
        case IP_LOCKPICK:
            // The same call the skill cursor makes (gmouse.cc), so the roll, the messages, the
            // lockpick-item bonus and any trap are all the vanilla path -- we only spared the
            // player the wheel-and-aim detour. NOTE it refuses with "you can't do that in combat"
            // while isInCombat() (actions.cc:1379); that vanilla rule is deliberately kept, and
            // realtime holds combat warm longer than the turn-based game did, so expect to meet
            // it more often than in vanilla.
            rc = action_use_skill_on(obj_dude, obj, SKILL_LOCKPICK);
            break;
        case IP_LOOK:
            // Exactly the vanilla action menu's LOOK dispatch (gmouse.cc): the script's own
            // description first, the generic "You see..." only when it has none.
            rc = obj_examine(obj_dude, obj);
            if (rc == -1) {
                rc = obj_look_at(obj_dude, obj);
            }
            break;
        }
        trace_event(TRACE_ACTION, "interact-do", "kind=%d pid=%d objType=%d rc=%d",
            kind, obj->pid, FID_TYPE(obj->fid), rc);
        return;
    }

    // TEMP DIAG (Gizmo's-door hunt): find the nearest USABLE scenery (doors are usable scenery) and,
    // along with WHY the prompt would be suppressed, log it whenever any of those change. Pins down
    // "the door won't highlight / open with A" to the exact cause: suppress != 0 => combat/menu is
    // blocking; dist > ipRange (4) => too far, just a range issue; door_pid == -1 => the door isn't
    // even usable scenery (different object type). STRIP once resolved.
    if (obj_dude != NULL) {
        int reason = 0;
        if (gamepad_ui_menu_active()) reason = 1;        // menu
        else if (dialog_active()) reason = 2;            // dialog
        else if (radial_wheel_is_open()) reason = 3;     // wheel
        else if (inven_any_open()) reason = 4;           // inventory
        else if (combat_rt_has_lock()) reason = 5;       // lock-on held
        else if (combat_rt_hostile_in_attack_range()) reason = 6; // active hostile in range

        Object* nd = NULL;
        int ndDist = 9999;
        int ndPid = -1;
        for (Object* o = obj_find_first_at(obj_dude->elevation); o != NULL; o = obj_find_next_at()) {
            if (FID_TYPE(o->fid) != OBJ_TYPE_SCENERY || !proto_action_can_use(o->pid)) {
                continue;
            }
            int d = obj_dist(obj_dude, o);
            if (d < ndDist) {
                ndDist = d;
                nd = o;
                ndPid = o->pid;
            }
        }

        static int lastReason = -99;
        static int lastPid = -99;
        static int lastDist = -99;
        if (reason != lastReason || ndPid != lastPid || ndDist != lastDist) {
            lastReason = reason;
            lastPid = ndPid;
            lastDist = ndDist;
            trace_event(TRACE_FLOW, "ip-diag", "suppress=%d door_pid=%d dist=%d ipRange=%d",
                reason, ndPid, nd != NULL ? ndDist : -1, IP_RANGE_TILES);
        }
    }

    // 2) Show the prompt whenever an in-reach interactable exists and A is not committed to an
    // attack. An in-reach interactable (a door you're standing at, an adjacent corpse) WINS over a
    // hostile that merely sits within weapon range -- otherwise a drawn ranged weapon (whose range
    // covers most of the map) kept the prompt permanently suppressed, so a door wouldn't highlight
    // or open with A while any enemy was on the map (the "can't open Gizmo's door with my gun out,
    // but it works once I holster" report). A held LOCK (R3) is deliberate attack intent, so it
    // still suppresses the prompt -- that's how you commit A to firing instead of interacting.
    // Also hidden whenever a menu / dialog / radial wheel / inventory owns the screen (so A isn't
    // stolen from a reply).
    //
    // An ARMED SKILL CURSOR suppresses it too, and that one is not optional: while a lockpick /
    // doctor / steal cursor is up, A means "apply this skill where I am pointing", and gmouse turns
    // the raw left click into exactly that. If the prompt kept its target, the A router would queue
    // its own interaction first and swallow the press -- so standing next to the very door you
    // armed the lockpick FOR would be the one place the lockpick could not be used. Now that a
    // skill cursor stays armed after each use (gamepad_ui_skill_cursor_sticky), that overlap would
    // be permanent rather than momentary.
    if (obj_dude == NULL || gamepad_ui_menu_active() || dialog_active()
        || radial_wheel_is_open() || inven_any_open()
        || combat_rt_has_lock()
        || gmouse_3d_get_mode() >= FIRST_GAME_MOUSE_MODE_SKILL) {
        ip_clear_outline();
        ip_hide_overlay();
        ip_target = NULL;
        return;
    }

    // 3) Reflect the nearest interactable: highlight it + float the hand over it.
    int kind = IP_NONE;
    Object* best = ip_find_nearest(&kind);
    bool targetChanged = best != ip_target;
    ip_target = best;
    if (best == NULL) {
        ip_actions_count = 0;
        ip_action_index = 0;
        ip_clear_outline();
        ip_hide_overlay();
        return;
    }

    // Rebuild the D-pad cycle for whatever is highlighted now. The SELECTION only resets when the
    // target changes -- rebuilding every tick keeps the list honest (a door that just got unlocked
    // drops its pick entry) without snapping the icon back to the default under the player's thumb.
    ip_actions_count = ip_build_actions(best, ip_actions);
    if (targetChanged) {
        ip_action_index = 0;
    } else if (ip_action_index >= ip_actions_count) {
        ip_action_index = ip_actions_count > 0 ? ip_actions_count - 1 : 0;
    }
    kind = ip_chosen_kind();

    // In combat (a hostile in weapon range), only advertise the prompt for an interactable you're
    // standing RIGHT AT (a door/corpse adjacent to the dude). A merely-nearby one (a neutral patron
    // or item a few tiles off) must NOT show the hand, because A stays an ATTACK there -- otherwise
    // plain A wouldn't fire when any bystander happened to be within reach, and you'd be forced to
    // lock just to shoot ("plain A doesn't fire when I'm not aiming"). ip_target stays set either
    // way so the A router (gpui) can make the same adjacent-vs-attack decision. Out of combat the
    // hand shows for anything in range, as before.
    if (combat_rt_hostile_in_attack_range() && obj_dist(obj_dude, best) > IP_INTERACT_ADJACENT_TILES) {
        ip_clear_outline();
        ip_hide_overlay();
        return;
    }

    ip_set_outline(best);
    ip_show_overlay(best, kind);
}

bool interact_prompt_request()
{
    if (ip_target == NULL) {
        return false;
    }
    int kind = ip_chosen_kind();
    if (kind == IP_NONE) {
        return false;
    }

    // Don't re-trigger while the dude is still carrying out the previous interaction.
    // A door/scenery use FIRST walks the dude to the object (that walk-then-use is one
    // busy animation sequence), so the door doesn't open until he arrives. Mashing A in
    // the meantime kept re-issuing the action -- each press interrupted/restarted the
    // walk (rc=-1 "busy") so he never reached the door, which read as "first press does
    // nothing, keep pressing". Gate on the dude being idle so ONE press cleanly walks
    // him over and opens it; further presses are ignored until that finishes.
    if (obj_dude != NULL && anim_busy(obj_dude) != 0) {
        return false;
    }

    ip_pending = ip_target;
    ip_pending_kind = kind;
    trace_event(TRACE_ACTION, "interact-queue", "kind=%d pid=%d objType=%d",
        kind, ip_target->pid, FID_TYPE(ip_target->fid));
    return true;
}

Object* interact_prompt_target()
{
    return ip_target;
}

bool interact_prompt_offering_corpse()
{
    // The CHOSEN action, not the default: once you have cycled a corpse to LOOK, A is no longer a
    // loot and must not be held back by the post-kill loot cooldown.
    return ip_target != NULL && ip_chosen_kind() == IP_LOOT_CORPSE;
}

bool interact_prompt_cycle(int delta)
{
    if (ip_target == NULL || ip_actions_count < 2 || delta == 0) {
        return false;
    }
    ip_action_index = (ip_action_index + delta % ip_actions_count + ip_actions_count) % ip_actions_count;
    // Deliberately does NOT repaint here. This runs from the INPUT HOOK, and the redraw would lock
    // art (a possible disk read) and add/move a GNW window from inside button processing -- the
    // exact re-entry this file's tick/queue split exists to avoid. interact_prompt_tick repaints on
    // the very next frame, and it rebuilds the window whenever the icon number changes, which this
    // just made it do.
    return true;
}

bool interact_prompt_target_adjacent()
{
    return ip_target != NULL && obj_dude != NULL
        && obj_dist(obj_dude, ip_target) <= IP_INTERACT_ADJACENT_TILES;
}

void interact_prompt_reset()
{
    ip_clear_outline();
    ip_hide_overlay();
    ip_target = NULL;
    ip_pending = NULL;
    ip_pending_kind = IP_NONE;
    ip_actions_count = 0;
    ip_action_index = 0;
}

} // namespace fallout
