#include "game/interact_prompt.h"

#include <math.h>
#include <string.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/combat_rt.h"
#include "game/critter.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gconfig.h"
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
// 4 -> 2 at his request (cfg interact_range); synced into the default 2026-10-10.
#define IP_RANGE_TILES 2

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
    IP_PUSH,           // swap places with a PARTY MEMBER -- see ip_push_companion
};

static Object* ip_target = NULL;   // interactable currently highlighted (re-derived each tick)

// ⚠️ THE CORPSE YOU JUST LOOTED STEPS ASIDE (2026-10-17, "mám dvě mrtvoly na lootování vedle sebe a
// nemůžu vybrat tu druhou"). trace_2026-10-17_213818: two raiders on 26088 and 26089, the dude on
// 26087 -- A always took the nearer one, and the farther lay in the same direction, where the
// stick's aim chose the nearer one again. So for IP_RECENT_LOOT_MS after a loot, the automatic pick
// ranks that body as if it lay IP_RECENT_LOOT_PENALTY tiles further: the next A opens the other one.
#define IP_RECENT_LOOT_MS 30000
#define IP_RECENT_LOOT_PENALTY 3
static Object* ip_recent_loot = NULL;
static unsigned int ip_recent_loot_at = 0;

// ⚠️ A LOCKED DOOR IS USED FIRST, PICKED SECOND (2026-10-17, his pick, after the Mariposa entrance:
// he had the code from the Mutant Transmissions tape, and A on the door kept rolling a -60 lockpick
// instead of using it -- the door's use_p_proc is what enters the code). So the first A on a locked
// door USES it, as a click always did in vanilla: a code, a password, any script gets its say, or the
// door just says it is locked. If it is still locked after that try, the prompt turns to the pick
// and the next A picks it. Doors only -- a locked chest has no script to ask and keeps the pick.
#define IP_DOOR_TRIED_MS 120000
static Object* ip_door_tried = NULL;
static unsigned int ip_door_tried_at = 0;
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

// How far the right-stick picker reaches, as opposed to IP_RANGE_TILES (2) which is how close
// something must be for the prompt to offer it on its own. Pointing at a thing says what you mean,
// so it does not need the proximity guess -- but it stays bounded so the picker can't grab the far
// side of a warehouse, and everything it offers must additionally be ON SCREEN.
// 12 -> 6 at his request (cfg interact_pick_range); synced into the default 2026-10-10.
#define IP_PICK_RANGE_TILES 6

// Both ranges are cfg levers (`interact_range` / `interact_pick_range`), read once on first tick.
// They are pure feel and the right value only shows up in play, so they must be tunable without a
// rebuild. The #defines above stay as the compiled-in defaults.
static int ip_range_tiles = IP_RANGE_TILES;
static int ip_pick_range_tiles = IP_PICK_RANGE_TILES;
static bool ip_config_loaded = false;

static void ip_load_config()
{
    if (ip_config_loaded) {
        return;
    }
    ip_config_loaded = true;

    int value = ip_range_tiles;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_INTERACT_RANGE_KEY, &value);
    // Floored at 1: zero would mean nothing is ever offered, which reads as "the prompt is broken".
    ip_range_tiles = value < 1 ? 1 : (value > 20 ? 20 : value);

    value = ip_pick_range_tiles;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_INTERACT_PICK_RANGE_KEY, &value);
    ip_pick_range_tiles = value < 1 ? 1 : (value > 40 ? 40 : value);

    // Pointing is a statement of intent, so the picker must never reach LESS far than the automatic
    // proximity offer -- otherwise something the prompt lights up on its own could not be pointed at.
    if (ip_pick_range_tiles < ip_range_tiles) {
        ip_pick_range_tiles = ip_range_tiles;
    }
}

static bool ip_on_screen(Object* obj); // defined with the picker below

static Object* ip_outlined = NULL; // object we placed an outline on, to restore it exactly
static int ip_outline_saved = 0;

static int ip_win = -1;            // floating icon overlay window
static int ip_icon_num = 0;        // interface FRM number currently shown (hand vs talk face)
static int ip_hand_w = 0;          // current icon dimensions
static int ip_hand_h = 0;
static int ip_win_x = -32000;
static int ip_win_y = -32000;

// Can this critter actually hold a conversation?
//
// ⚠️ A DECLARED `talk_p_proc` IS NOT REQUIRED, and assuming it was is what silently made Aradesh
// and Tandi unspeakable while Killian was fine ("nemůžu mluvit jak s Tandi tak ani s jejím otcem
// ... s Killianem šlo v pořádku"). `exec_script_proc` (scripts.cc:1125) falls back to PROCEDURE 1
// -- `start` -- whenever procs[action] is NO_PROC, so a script whose dialogue hangs off `start`
// converses perfectly well without ever declaring talk_p_proc. Read straight out of the shipped
// data: KILLIAN.INT declares talk_p_proc, TANDI.INT declares only map_enter / map_update /
// critter_p_proc, and ARADESH.INT declares NOTHING but `start`. All three talk in vanilla, and
// all three have a DIALOG/*.MSG.
//
// The question this gate actually means is "is this a person or is it wildlife", and it takes two
// steps to answer:
//   1. A declared talk_p_proc. This is what keeps QUADRUPED talkers -- the brahmin declares one,
//      which is exactly why the original body-type-only gate was wrong.
//   2. Failing that, a BIPED with a script: every humanoid NPC, and none of the rats / molerats /
//      radscorpions / dogs this gate exists for ("I can't hit the rats, the dude keeps talking to
//      them"). A biped with nothing to say (a deathclaw) is already excluded a line above by
//      combat_rt_is_engaging, and if one ever slips through, A runs its `start` and nothing
//      happens -- the same no-op vanilla gives you for clicking the talk cursor on it.
//
// Step 2 also covers a hazard step 1 has on its own: procs[] is built by scr_build_lookup_table
// only when the script's PROGRAM is first loaded, so before that it reads all-zero -- i.e. the
// proc test can answer "no talk proc" for a critter that plainly has one.
static bool ip_critter_can_converse(Object* obj)
{
    if (obj->sid == -1) {
        return false;
    }

    Script* script;
    if (scr_ptr(obj->sid, &script) == -1) {
        return false;
    }

    if (script->procs[SCRIPT_PROC_TALK] != SCRIPT_PROC_NO_PROC) {
        return true;
    }

    return critter_body_type(obj) == BODY_TYPE_BIPED;
}

// Classify an object into an interaction kind (0 = not interactable).
static int ip_classify(Object* obj)
{
    if (obj == NULL || obj == obj_dude) {
        return IP_NONE;
    }

    // ⚠️ NOT DRAWN => NOT INTERACTABLE. Maps stage content that does not exist yet -- a caravan that
    // arrives later, an ambush waiting for its trigger -- as real objects flagged OBJECT_HIDDEN, and
    // a script clears the flag when their moment comes. Vanilla never had to think about it: the
    // mouse picks targets out of what is RENDERED, so an undrawn object simply cannot be clicked.
    //
    // Our prompt enumerates with obj_find_first_at, which returns hidden objects too (obj_create_list
    // filters them, obj_find_first_at does not), so it happily offered the talk hand to thin air --
    // the player talked to an invisible caravan merchant outside Junktown (proto 180 "Merchant",
    // trace_2026-08-21_231322 `interact-do kind=3 pid=16777396`, and note it appears in NO state
    // heartbeat, because that walks obj_create_list). Talking to staged content early can also run
    // its script out of order, which is a worse problem than the ghost itself.
    if ((obj->flags & OBJECT_HIDDEN) != 0) {
        return IP_NONE;
    }

    switch (FID_TYPE(obj->fid)) {
    case OBJ_TYPE_ITEM: {
        // ⚠️ RESOLVE THE PROTO OURSELVES. `item_get_type()` is vanilla and calls proto_ptr WITHOUT
        // checking it (item.cc:585): on failure proto_ptr leaves the pointer NULL and the very next
        // line reads `proto->item.type` -- NULL + 0x20. That is the crash he hit one tenth of a
        // second after Continue loaded JUNKKILL.sav (trace_2026-09-16_214225, ACCESS_VIOLATION
        // read 0x20 in item_get_type <- ip_classify).
        //
        // ⚠️ AND IT IS NOT A LOAD-TIMING FLUKE -- an object can simply HAVE NO PROTO. Scripts (and
        // map_load_file itself) create objects with `obj_new(&obj, fid, -1)`, and pid -1 is the
        // first thing proto_ptr refuses. The mouse never had to care, because vanilla picks targets
        // out of what is rendered and reaches items through paths that check first; our prompt
        // enumerates the map with obj_find_first_at and hands whatever it finds straight to a
        // helper that assumes a proto. Same shape as the scenery branch below, which already does
        // this correctly -- this branch was simply written trusting a function that cannot be.
        //
        // No proto = nothing we can say about the thing = not interactable.
        Proto* itemProto = NULL;
        if (proto_ptr(obj->pid, &itemProto) == -1 || itemProto == NULL) {
            // DIAGNOSTIC, once per distinct pid: the guard above stops the crash either way, but
            // the two causes want different follow-ups. A pid that shows up here EVERY session is a
            // protoless object that simply exists on that map (nothing more to do). One that
            // appears only in the seconds after a load would mean proto_ptr is failing TRANSIENTLY
            // while the proto lists rebuild -- and that would be hiding a real bug, because the
            // same item would be silently un-interactable for those frames. Say which it is.
            static int reportedPid = -1;
            if (obj->pid != reportedPid) {
                reportedPid = obj->pid;
                trace_event(TRACE_WARN, "ip-no-proto", "pid=%d fid=%08x tile=%d",
                    obj->pid, obj->fid, obj->tile);
            }
            return IP_NONE;
        }
        if (itemProto->item.type == ITEM_TYPE_CONTAINER) {
            // A LOCKED chest offers the pick instead of a loot window that would just refuse.
            return obj_is_locked(obj) ? IP_LOCKPICK : IP_LOOT_CONTAINER;
        }
        return IP_GET;
    }
    case OBJ_TYPE_SCENERY: {
        if (!proto_action_can_use(obj->pid)) {
            return IP_NONE;
        }
        // A LOCKED door: A picks it rather than rattling the handle. Reaching the lockpick any
        // other way on a controller is LB -> wheel -> Lockpick -> aim the cursor -> A, which is
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
        // Widened 2026-08-20 from combat_rt_under_threat(): a fight the player merely WATCHES also
        // blocks talking, as it does in vanilla. under_threat only asks "is something after ME",
        // which let him walk into the middle of the Skulz brawl and talk to Vinnie -- nobody there
        // was ever hostile to HIM. END COMBAT (Enter / D-pad) clears this immediately.
        if (combat_rt_fight_in_view()) {
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
// LOOK-ONLY: scenery that has nothing to DO but plenty to say -- a signpost, a poster, a terminal
// with no use action. 808 of the game's 908 scenery protos carry a description, which is exactly why
// these are NOT in the automatic prompt: it would light up on every table, door and barrel in the
// room and steal A from the things you actually meant.
//
// ⚠️ REACHABLE ONLY BY DELIBERATELY POINTING AT IT with the right stick. Pointing IS the statement
// of intent, so the automatic nearest-pick stays exactly as clean as it was, and the cost is bounded
// to the moments you are already sweeping the stick.
//
// Scenery only, on purpose: walls also carry descriptions and there are hundreds of segments in any
// building, so including them would drown the picker in the one place signs actually hang.
static bool ip_look_only(Object* obj)
{
    if (obj == NULL || obj == obj_dude || (obj->flags & OBJECT_HIDDEN) != 0) {
        return false;
    }

    if (FID_TYPE(obj->fid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    // ⚠️ NO FLAT SCENERY. Flat objects are drawn WITH THE FLOOR (obj_toggle_flat, set from proto
    // flag 0x08), which is the map designer's marker layer: floor decals and, above all, the
    // invisible blockers used to fence the player off. Proto 67 "Secret Blocking Hex" is exactly
    // that -- flags 0xa0000008 = FLAT | LIGHT_THRU | SHOOT_THRU -- and its vanilla description is
    // the placeholder joke "If you can see this, blame the technical designer." Sweeping the right
    // stick kept picking those out of thin air and printing it, which is how the user found this.
    //
    // Nothing worth reading is flat: signs, posters, terminals and lockers all stand up in the
    // world. So this costs the picker nothing real and removes the whole marker layer at once --
    // no pid blacklist, which would have missed every other map's markers.
    //
    // Only the LOOK-only path is filtered. ip_classify's usable scenery (doors, ladders, elevators)
    // is untouched: a usable object stays usable even if a map flags it flat.
    if ((obj->flags & OBJECT_FLAT) != 0) {
        return false;
    }

    // Anything ip_classify already accepts is a normal target and must not be duplicated here.
    return ip_classify(obj) == IP_NONE;
}

static int ip_build_actions(Object* obj, int* out)
{
    int n = 0;
    int def = ip_classify(obj);
    if (def == IP_NONE) {
        // A pointed-at sign has exactly one thing it can do, and the cycle must not be empty or the
        // prompt would have a target with no action to show.
        if (ip_look_only(obj)) {
            out[n++] = IP_LOOK;
            return n;
        }
        return 0;
    }

    out[n++] = def;

    // A LOCKED thing defaults to the pick, but the plain action stays reachable one tap away: the
    // lock may already be open (a script, a key, someone else's pick) while obj_is_locked lags, and
    // rattling a locked door is at least honest feedback. Same for a locked chest.
    if (def == IP_LOCKPICK) {
        bool door = FID_TYPE(obj->fid) == OBJ_TYPE_SCENERY;
        bool tried = obj == ip_door_tried && get_time() - ip_door_tried_at < IP_DOOR_TRIED_MS;
        if (door && !tried) {
            out[0] = IP_USE; // used first (ip_door_tried); the pick one tap away
            out[n++] = IP_LOCKPICK;
        } else {
            out[n++] = door ? IP_USE : IP_LOOT_CONTAINER;
        }
    }

    // PUSH PAST YOUR OWN FOLLOWER -- the Fallout 2 move, which F1 only ever had through mods.
    //
    // ⚠️ It is an ACTION, not a movement rule, and that was his correction. The first attempt made
    // walking INTO a companion swap you automatically ("ne takhle ne"), which silently overloads the
    // one input you use constantly and gives no way to just stand next to Ian. As a D-pad entry the
    // shove is something you ASK for, sitting right beside TALK on the follower you are already
    // pointing at.
    //
    // Party members only, so it can never become a way to reposition strangers or enemies.
    if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER && isPartyMember(obj) && !critter_is_dead(obj)) {
        out[n++] = IP_PUSH;
    }

    out[n++] = IP_LOOK;
    return n;
}

// Swap tiles with a party member. Returns 0 on success, -1 when it could not be done.
//
// ⚠️ The dude MOVES TOO, unlike the walk-into version -- there the player was already stepping, so
// only the companion had to give way. Asking for a push while standing still means both of them
// have to change places or nothing visibly happens.
static int ip_push_companion(Object* companion)
{
    if (companion == NULL || obj_dude == NULL) {
        return -1;
    }

    // Never tear a companion out of something it is committed to (an attack, a death, a knockdown).
    // Walking is fine to interrupt -- it is only following you anyway.
    if (critter_is_prone(companion) || critter_is_dead(companion)) {
        return -1;
    }

    int anim = FID_ANIM_TYPE(companion->fid);
    if (anim != ANIM_STAND && anim != ANIM_WALK && anim != ANIM_RUNNING) {
        return -1;
    }

    int dudeTile = obj_dude->tile;
    int theirTile = companion->tile;
    if (dudeTile == theirTile) {
        return -1;
    }

    // Drop whatever either of them had queued first -- otherwise an in-flight walk finishes from the
    // OLD tile and drags them straight back.
    anim_clear_pending(companion);
    anim_clear_pending(obj_dude);

    Rect r;
    if (obj_move_to_tile(companion, dudeTile, map_elevation, &r) == 0) {
        tile_refresh_rect(&r, map_elevation);
    }
    if (obj_move_to_tile(obj_dude, theirTile, map_elevation, &r) == 0) {
        tile_refresh_rect(&r, map_elevation);
    }

    return 0;
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

// ⚠️ NOTHING THROUGH A WALL (2026-10-13, "mohu interagovat s trezorem přes zeď v posledním patře ve
// vaultu 15"). Every scan here measured tiles and nothing else, so a safe two tiles away on the far
// side of a wall was as good a target as one in front of you. Each one now asks whether he can get
// NEXT TO the thing (the tile under a loose item counts) within a few walking steps.
//
// Not a line of sight, which is wrong both ways at this range: walking along a wall towards a door,
// the line to the door clips the wall tile beside it and the door would vanish, while a gap one tile
// wide can pass the line with the object in another room. Reaching it is what "it is right here"
// means. A breadth-first search over the hexes around him, with a generation-stamped visited map so
// a step costs O(1); the picker's reach of 6 is a couple of hundred tiles at most.
#define IP_REACH_MAX_TILES 1024

static unsigned short ip_reach_seen[HEX_GRID_SIZE];
static unsigned short ip_reach_gen = 0;

static bool ip_reachable_within(Object* obj, int maxSteps)
{
    if (obj_dude == NULL || obj == NULL) {
        return false;
    }

    // ⚠️ NOT FOR PEOPLE. A conversation never needed you beside anyone -- vanilla talks across a
    // counter, and gdialog_enter makes its own "can you see there" check -- and a shopkeeper behind
    // one has no free tile next to him at all, so this test would have taken the TALK hand off
    // Killian and every merchant like him. Corpses lie where people stood, so they go with them.
    if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
        return true;
    }

    int goalDist = (obj->flags & OBJECT_MULTIHEX) != 0 ? 2 : 1;
    if (tile_dist(obj_dude->tile, obj->tile) <= goalDist) {
        return true;
    }

    // A new stamp per search; on wrap-around the map is wiped so an old stamp can never read as seen.
    ip_reach_gen++;
    if (ip_reach_gen == 0) {
        memset(ip_reach_seen, 0, sizeof(ip_reach_seen));
        ip_reach_gen = 1;
    }

    static int queue[IP_REACH_MAX_TILES];
    static unsigned char depth[IP_REACH_MAX_TILES];
    int head = 0;
    int tail = 0;
    queue[tail] = obj_dude->tile;
    depth[tail] = 0;
    tail++;
    ip_reach_seen[obj_dude->tile] = ip_reach_gen;

    while (head < tail) {
        int tile = queue[head];
        int d = depth[head];
        head++;

        if (tile_dist(tile, obj->tile) <= goalDist) {
            return true;
        }
        if (d >= maxSteps) {
            continue;
        }

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int next = tile_num_in_direction(tile, rotation, 1);
            if (next < 0 || next >= HEX_GRID_SIZE || ip_reach_seen[next] == ip_reach_gen) {
                continue;
            }
            ip_reach_seen[next] = ip_reach_gen;

            if (next != obj->tile && obj_blocking_at(obj_dude, next, obj_dude->elevation) != NULL) {
                continue;
            }

            if (tail >= IP_REACH_MAX_TILES) {
                return false; // far beyond any reach used here; a bound, not a verdict
            }
            queue[tail] = next;
            depth[tail] = (unsigned char)(d + 1);
            tail++;
        }
    }

    return false;
}

// ⚠️ AND NOTHING BEHIND A WALL, LITERALLY (2026-10-18, "byl jsem u skříněk a mohl jsem zaměřit i další
// za zdí. Ne! To se nesmí stávat"). The reach test alone let the stick pick a locker in the NEXT
// alcove of Mariposa's barracks (MBSTRG12, elevation 1: three lockers per alcove, 4-tile partitions
// between them): round the partition is ~10 steps for a locker 6 tiles off, inside the picker's
// budget of dist x 2 + 2. So the straight line from him to the thing -- the engine's own, the one a
// projectile flies (make_straight_path_func) -- must not cross a WALL or a CLOSED DOOR
// (obj_wall_blocking_at; furniture does not hide anything). The line stops at the thing itself, so
// the wall a locker stands against never counts. A door, or anything set into a wall, is not hidden
// by the wall tiles beside it: walking along a wall towards a door, the line clips them -- the very
// reason this was not a sight test the first time (2026-10-13).
static Object* ip_sight_goal = NULL;
static bool ip_sight_goal_in_wall = false;

static Object* ip_sight_blocking_at(Object* self, int tile, int elevation)
{
    Object* blocker = obj_wall_blocking_at(self, tile, elevation);
    if (blocker == NULL || blocker == ip_sight_goal) {
        return NULL;
    }
    if (ip_sight_goal_in_wall && ip_sight_goal != NULL && tile_dist(tile, ip_sight_goal->tile) <= 1) {
        return NULL;
    }
    return blocker;
}

static bool ip_sees(Object* obj)
{
    if (obj_dude == NULL || obj == NULL || obj == obj_dude) {
        return true;
    }
    if (obj->elevation != obj_dude->elevation || tile_dist(obj_dude->tile, obj->tile) <= 1) {
        return true;
    }

    ip_sight_goal = obj;
    ip_sight_goal_in_wall = FID_TYPE(obj->fid) == OBJ_TYPE_WALL;
    if (FID_TYPE(obj->fid) == OBJ_TYPE_SCENERY) {
        Proto* proto;
        if (proto_ptr(obj->pid, &proto) != -1 && proto->scenery.type == SCENERY_TYPE_DOOR) {
            ip_sight_goal_in_wall = true;
        }
    }
    Object* blocker = NULL;
    make_straight_path_func(obj_dude, obj_dude->tile, obj->tile, NULL, &blocker, 0, ip_sight_blocking_at);
    bool seen = blocker == NULL;

    // A DOOR IS SEEN WHEN THE FLOOR IN FRONT OF IT IS. Seen along its own wall the line to the door
    // runs down that wall and clips wall tiles well past the ones beside it (self-played at the
    // barracks door 26880: hidden from 4 of 5 spots beside its wall). So for a door, or anything set
    // into a wall, any free tile next to it that he can see counts.
    if (!seen && ip_sight_goal_in_wall) {
        for (int rotation = 0; rotation < ROTATION_COUNT && !seen; rotation++) {
            int front = tile_num_in_direction(obj->tile, rotation, 1);
            if (front < 0 || front >= HEX_GRID_SIZE || obj_wall_blocking_at(NULL, front, obj->elevation) != NULL) {
                continue;
            }
            if (front == obj_dude->tile) {
                seen = true;
                break;
            }
            blocker = NULL;
            make_straight_path_func(obj_dude, obj_dude->tile, front, NULL, &blocker, 0, ip_sight_blocking_at);
            seen = blocker == NULL;
        }
    }

    ip_sight_goal = NULL;
    ip_sight_goal_in_wall = false;
    return seen;
}

// The automatic prompt guesses, so it only takes what is right there: a detour of two steps at most.
// Pointing the stick (the picker, a skill cursor) is a decision and may cost a short walk round
// something, but never the trip into another room that a wall between you would mean.
static bool ip_auto_reach(Object* obj)
{
    return ip_sees(obj) && ip_reachable_within(obj, tile_dist(obj_dude->tile, obj->tile) + 2);
}

static bool ip_pick_reach(Object* obj)
{
    return ip_sees(obj) && ip_reachable_within(obj, tile_dist(obj_dude->tile, obj->tile) * 2 + 2);
}

// Best interactable within range, or NULL. Sets *kindOut to its interaction kind.
static Object* ip_find_nearest(int* kindOut)
{
    // A hand-picked target (right-stick flick) wins over the automatic choice for as long as it
    // stays valid and in reach. Re-validated here every tick rather than trusted, because the
    // object can die, be looted empty, be picked up, or be walked away from between frames.
    if (ip_manual_target != NULL) {
        int kind = ip_classify(ip_manual_target);
        // A look-only target only ever exists because it was pointed at, so it is validated here
        // and nowhere else -- the automatic scan below must never pick one up on its own.
        if (kind == IP_NONE && ip_look_only(ip_manual_target)) {
            kind = IP_LOOK;
        }
        bool stale = get_time() - ip_manual_since > IP_MANUAL_HOLD_MS;
        // Validated against the PICKER's reach, not the automatic one -- a deliberately chosen
        // cabinet across the room would otherwise be discarded on the very next tick.
        if (kind != IP_NONE && !stale && obj_dist(obj_dude, ip_manual_target) <= ip_pick_range_tiles
            && ip_on_screen(ip_manual_target) && ip_pick_reach(ip_manual_target)) {
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
        if (d > ip_range_tiles) {
            continue;
        }
        if (!ip_auto_reach(obj)) {
            continue;
        }
        int prio = ip_priority(kind);
        // Just looted (see ip_recent_loot): ranked as if further off, so another body of the same
        // kind in reach goes first.
        if (obj == ip_recent_loot && (kind == IP_LOOT_CORPSE || kind == IP_LOOT_CONTAINER)
            && get_time() - ip_recent_loot_at < IP_RECENT_LOOT_MS) {
            d += IP_RECENT_LOOT_PENALTY;
        }
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
// the automatic pick stays close (ip_range_tiles) because it has to guess what you meant, while
// pointing the stick at something is unambiguous, so it may reach across the room. Pressing A then
// walks you there -- exactly what a mouse click has always done.
#define IP_CANDIDATES_MAX 24

static int ip_collect(Object** out, int max)
{
    int n = 0;
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL && n < max; obj = obj_find_next_at()) {
        // The picker's set includes LOOK-ONLY scenery (signs and the like); the automatic prompt's
        // does not. That difference is the whole design -- see ip_look_only.
        if (ip_classify(obj) == IP_NONE && !ip_look_only(obj)) {
            continue;
        }
        if (obj_dist(obj_dude, obj) > ip_pick_range_tiles) {
            continue;
        }
        if (!ip_on_screen(obj)) {
            continue;
        }
        if (!ip_pick_reach(obj)) {
            continue;
        }
        out[n++] = obj;
    }
    return n;
}

// Every interactable within the picker's range, and which test lets it through or keeps it out -- for
// the self-played runs (autotest "ipdump") and any "I could pick that through a wall" report.
void interact_prompt_trace_candidates()
{
    if (obj_dude == NULL) {
        return;
    }
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (ip_classify(obj) == IP_NONE) {
            continue;
        }
        int d = obj_dist(obj_dude, obj);
        if (d > ip_pick_range_tiles) {
            continue;
        }
        trace_event(TRACE_FLOW, "ip-candidate", "pid=%d tile=%d dist=%d sees=%d reach=%d onScreen=%d pick=%d auto=%d dude=%d",
            obj->pid, obj->tile, d, ip_sees(obj) ? 1 : 0,
            ip_reachable_within(obj, tile_dist(obj_dude->tile, obj->tile) * 2 + 2) ? 1 : 0,
            ip_on_screen(obj) ? 1 : 0, ip_pick_reach(obj) ? 1 : 0,
            d <= ip_range_tiles && ip_auto_reach(obj) ? 1 : 0, obj_dude->tile);
    }
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

// --- WHERE A SKILL CURSOR STARTS ----------------------------------------------------------------
//
// His request: *"při výběru first aid dej kurzor na hráče, při odemkání na locknutý container a to
// samé u dalších skillů"*. An armed skill cursor used to appear wherever the pointer was abandoned,
// which in free play is nowhere near anything -- the pointer is hidden and has not moved for
// minutes -- so every use of the wheel began by hunting for the crosshair and dragging it back.
//
// This answers "what would he obviously be pointing at", per skill, out of the same scan the prompt
// itself uses -- which matters, because that scan is where the traps live: HIDDEN objects are
// staged map content that must never be offered, and an object can have NO PROTO at all. Both are
// handled inside ip_classify, so this goes through it rather than around it.
//
// Nothing suitable nearby returns NULL and the caller simply leaves the cursor where it is: a
// crosshair parked on something absurd is worse than one that did not move.
Object* interact_prompt_skill_target(int skill)
{
    if (obj_dude == NULL) {
        return NULL;
    }

    // Two passes: what the skill is FOR, then what it will settle for. Traps is the reason -- a
    // trapped chest is not necessarily a LOCKED one, so "locked things" is the right first guess
    // and "containers and doors" the right second one.
    for (int pass = 0; pass < 2; pass++) {
        Object* best = NULL;
        int bestDist = 0;

        for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
            if (obj == obj_dude) {
                continue;
            }

            int kind = ip_classify(obj); // HIDDEN / proto-less / non-interactable all fall out here
            bool critter = FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER && !critter_is_dead(obj);
            if (kind == IP_NONE && !critter) {
                continue;
            }

            bool want = false;
            switch (skill) {
            case SKILL_LOCKPICK:
                want = kind == IP_LOCKPICK
                    || (pass == 1 && (kind == IP_LOOT_CONTAINER || kind == IP_USE));
                break;
            case SKILL_TRAPS:
                want = kind == IP_LOCKPICK
                    || (pass == 1 && (kind == IP_LOOT_CONTAINER || kind == IP_USE));
                break;
            case SKILL_STEAL:
                // Only a living critter can be stolen from, and only someone ELSE.
                want = critter;
                break;
            case SKILL_SCIENCE:
                // Terminals and the like first; a robot or a critter is the fallback.
                want = kind == IP_USE || (pass == 1 && critter);
                break;
            case SKILL_REPAIR:
                want = kind == IP_USE || (pass == 1 && kind == IP_LOOT_CONTAINER);
                break;
            case SKILL_FIRST_AID:
            case SKILL_DOCTOR:
                // The caller parks these on the DUDE himself -- this is only reached if it asks for
                // a patient other than him.
                want = critter;
                break;
            default:
                return NULL;
            }

            if (!want) {
                continue;
            }

            if (!ip_on_screen(obj)) {
                continue; // off-screen: the cursor cannot be put there at all
            }

            int dist = obj_dist(obj_dude, obj);
            if (dist > ip_pick_range_tiles) {
                continue;
            }

            if (!ip_pick_reach(obj)) {
                continue;
            }

            if (best == NULL || dist < bestDist) {
                best = obj;
                bestDist = dist;
            }
        }

        if (best != NULL) {
            return best;
        }
    }

    return NULL;
}

// --- WHAT A USE-ON OR SKILL CURSOR CAN BE PUT ON (2026-10-08) ----------------------------------
//
// *"Nemohl jsem nasměrovat provaz správně, furt to aimoval něco jiného, musíme to nějak pojistit."*
// The crosshair the rope arms was a free pointer, and the click resolves to whatever is drawn
// topmost at that pixel (object_under_mouse) -- a wall, a rat, a crate in front of the hole -- so
// aiming a stick at a flat hole in the floor was a matter of luck. The pad now steps the crosshair
// between the objects a use-on can mean and the click goes to the one it sits on
// (gamepad_ui_targeting_pick). These are them: the dude himself (first aid, a doctor's bag),
// every critter, and any item, scenery or wall that is interactable or carries a SCRIPT -- a script
// is what answers "use rope on me"; an unscripted rock would only say nothing happens. HIDDEN staged
// content is never one (see ip_classify).
// Anything the prompt would offer an action on -- a door, a container, an item, a person -- reach
// aside. The arrow cursor's walk (combat_rt, the Diablo mouse) leaves a click on one of these to
// vanilla, so it still opens, talks and picks up.
bool interact_prompt_object_interactable(Object* obj)
{
    return ip_classify(obj) != IP_NONE;
}

bool interact_prompt_is_use_target(Object* obj)
{
    if (obj == NULL || (obj->flags & OBJECT_HIDDEN) != 0) {
        return false;
    }
    if (obj == obj_dude) {
        return true;
    }
    switch (FID_TYPE(obj->fid)) {
    case OBJ_TYPE_CRITTER:
        return true;
    case OBJ_TYPE_ITEM:
    case OBJ_TYPE_SCENERY:
    case OBJ_TYPE_WALL:
        return obj->sid != -1 || ip_classify(obj) != IP_NONE;
    default:
        return false;
    }
}

// Every use target on screen within the picker's reach, the dude included.
int interact_prompt_use_targets(Object** out, int max)
{
    int n = 0;
    if (obj_dude == NULL) {
        return 0;
    }
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL && n < max; obj = obj_find_next_at()) {
        if (!interact_prompt_is_use_target(obj)) {
            continue;
        }
        if (obj != obj_dude && obj_dist(obj_dude, obj) > ip_pick_range_tiles) {
            continue;
        }
        if (!ip_on_screen(obj)) {
            continue;
        }
        if (!ip_sees(obj)) {
            continue; // nothing behind a wall (ip_sees)
        }
        out[n++] = obj;
    }
    return n;
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
bool interact_prompt_pick_direction(float dirX, float dirY, bool fresh)
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
        float score = dot - IP_PICK_DIST_WEIGHT * (dist / (float)ip_pick_range_tiles);

        if (cand[i] == ip_target) {
            currentScore = score;
            haveCurrent = true;
        }

        if (best == NULL || score > bestScore) {
            best = cand[i];
            bestScore = score;
        }
    }

    // ⚠️ A SECOND FLICK THE SAME WAY STEPS ON (2026-10-17, the two raiders in a line: the farther one
    // could never win, the nearer always scored higher in the same direction). On a FRESH flick whose
    // best answer is what is already selected, take the next candidate that way instead -- ordered by
    // score, wrapping round -- so pushing the stick twice reaches the second thing in a line.
    if (fresh && best != NULL && best == ip_target) {
        Object* next = NULL;
        float nextScore = 0.0f;
        Object* top = NULL;
        float topScore = 0.0f;
        for (int i = 0; i < count; i++) {
            if (cand[i] == ip_target) {
                continue;
            }
            int tx;
            int ty;
            ip_screen_pos(cand[i], &tx, &ty);
            float vx = (float)(tx - fromX);
            float vy = (float)(ty - fromY);
            float len = sqrtf(vx * vx + vy * vy);
            if (len < 1.0f) {
                continue;
            }
            float dot = (vx * dirX + vy * dirY) / len;
            if (dot < IP_PICK_CONE_COS) {
                continue;
            }
            float score = dot - IP_PICK_DIST_WEIGHT * ((float)obj_dist(obj_dude, cand[i]) / (float)ip_pick_range_tiles);
            if (score <= bestScore && (next == NULL || score > nextScore)) {
                next = cand[i]; // the best one ranked below the current
                nextScore = score;
            }
            if (top == NULL || score > topScore) {
                top = cand[i];
                topScore = score;
            }
        }
        Object* chosen = next != NULL ? next : top;
        if (chosen != NULL) {
            ip_manual_target = chosen;
            ip_manual_since = get_time();
            return true;
        }
        return false;
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

// ⚠️ ...EXCEPT WHILE THE RIGHT STICK IS POINTING (2026-10-17, "když hýbu pravým joystickem, že highlight
// na co mířím by byl fajn"; asked how, he said "ať jsou žluté jak byly"). The original yellow outline
// is back, but only as the answer to a gesture: from the push until IP_STICK_HIGHLIGHT_MS after the
// stick comes home. What he threw out on 2026-10-04 was the outline that never went away.
#define IP_STICK_HIGHLIGHT_MS 1500
static unsigned int ip_stick_last = 0;

void interact_prompt_note_stick()
{
    ip_stick_last = get_time();
    if (ip_stick_last == 0) {
        ip_stick_last = 1; // 0 means "never"
    }
}

static void ip_set_outline(Object* obj)
{
    // ⚠️ THE PROMPT NO LONGER OUTLINES ANYTHING ON ITS OWN (2026-10-04): "highlighty žluté u výtahů dej
    // pryč i u pipbye když čekám, celkově je nechci". The floating action icon is the prompt; the yellow
    // outline sat on every elevator panel and stayed lit over the Pip-Boy while he waited. Only the
    // stick's pointing (above) lights one now, and ip_clear_outline puts back whatever was there.
    bool pointing = obj != NULL && ip_stick_last != 0 && elapsed_time(ip_stick_last) < IP_STICK_HIGHLIGHT_MS;
    if (!pointing) {
        if (ip_outlined != NULL) {
            ip_clear_outline();
        }
        return;
    }
    if (ip_outlined == obj) {
        return;
    }
    ip_clear_outline();
    ip_outline_saved = obj->outline;
    ip_outlined = obj;
    obj_outline_object(obj, OUTLINE_TYPE_ITEM, NULL); // refuses if it already wears one (an aim outline)
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

// ⚠️ THE HAND OUTLIVES THE WORLD IT POINTS AT -- THIRD TIME (2026-10-08). Its window is
// WINDOW_MOVE_ON_TOP, so every screen opened over the world shows it straight through, and the only
// thing that ever takes it down is interact_prompt_tick, which runs from the MAIN loop and not from
// a screen's own input loop. The Pip-Boy (gpui_open_dpad_screen) and the elevator panel each got an
// explicit interact_prompt_reset; then the called-shot window showed it too: *"když jsem vybíral
// střelbu končetin, tak ruka na loot byla skrz to vidět"*. The explicit resets stay (they are
// instant); this is the net under the next screen nobody has thought of yet. It is a BACKGROUND
// process, so it runs inside every screen's input loop too: once the main loop has not ticked the
// prompt for a quarter of a second -- i.e. some screen owns the frame -- it hides the hand. The
// tick puts it back the moment the world runs again (it recreates a missing window).
static unsigned int ip_last_tick_ms = 0;

static void ip_overlay_guard()
{
    if (ip_win != -1 && get_time() - ip_last_tick_ms > 250) {
        ip_hide_overlay();
    }
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
    case IP_PUSH:
        // PROVISIONAL ART, on his instruction ("zkus tam dát ikonku ruky provizorně"): the plain
        // USE hand. It is wrong in the long run -- the hand already means "use this" on every other
        // entry, so a shove reads as a use -- but no vanilla action-menu icon means "move aside",
        // and inventing one is a separate job. Replace when a better glyph exists.
        return IP_HAND_ART_NUM;
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
    ip_last_tick_ms = get_time();
    static bool guardInstalled = false;
    if (!guardInstalled) {
        add_bk_process(ip_overlay_guard, "prompt-guard");
        guardInstalled = true;
    }

    ip_load_config();

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

        // The mouse hover's side effect, which this pad action has to supply itself: a script that
        // has never run gets its start body now, so the action below reaches its own procedure.
        // See scr_load_before_first_event (the roped shaft that answered USE with "You see").
        scr_load_before_first_event(obj);

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
        case IP_PUSH:
            rc = ip_push_companion(obj);
            break;
        }
        trace_event(TRACE_ACTION, "interact-do", "kind=%d pid=%d objType=%d rc=%d",
            kind, obj->pid, FID_TYPE(obj->fid), rc);
        if (kind == IP_LOOT_CORPSE || kind == IP_LOOT_CONTAINER) {
            ip_recent_loot = obj;
            ip_recent_loot_at = get_time();
        }
        // A locked door has been tried with USE: if it stays locked, the next A picks it.
        if (kind == IP_USE && FID_TYPE(obj->fid) == OBJ_TYPE_SCENERY && ip_classify(obj) == IP_LOCKPICK) {
            ip_door_tried = obj;
            ip_door_tried_at = get_time();
        }
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
                reason, ndPid, nd != NULL ? ndDist : -1, ip_range_tiles);
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
    // ⚠️ THE USE-ITEM-ON CROSSHAIR COUNTS TOO, and it is NOT covered by the skill test above:
    // GAME_MOUSE_MODE_USE_CROSSHAIR is 3, FIRST_GAME_MOUSE_MODE_SKILL is 4, so it slipped under the
    // threshold. Everything the paragraph above says about a lockpick applies to it word for word:
    // holding A with a rope in hand arms this cursor to use the rope ON something, and if the prompt
    // kept highlighting the elevator, the A router queued "use the elevator" and swallowed the
    // press -- so the one object you armed the rope FOR was the one it could not be used on. That
    // was the user's report, twice.
    int ipCursorMode = gmouse_3d_get_mode();
    if (obj_dude == NULL || gamepad_ui_menu_active() || dialog_active()
        || radial_wheel_is_open() || inven_any_open()
        || combat_rt_has_lock()
        || ipCursorMode == GAME_MOUSE_MODE_USE_CROSSHAIR
        || ipCursorMode >= FIRST_GAME_MOUSE_MODE_SKILL) {
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

    // Rebuild the D-pad cycle for whatever is highlighted now. Rebuilding every tick keeps the list
    // honest (a door that just got unlocked drops its pick entry) without snapping the icon back to
    // the default under the player's thumb.
    //
    // ⚠️ THE SELECTION FOLLOWS THE ACTION, NOT THE SLOT (2026-10-04, "když odemknu dveře nebo stash,
    // tak ať je tam hned ruka, ať prostě zmáčknu jen A"). It used to be kept by INDEX, so when a lock
    // opened and the list shrank from [pick, use, look] to [use, look], whatever sat in slot 1 before
    // -- the hand -- was now the eye. Two rules instead:
    //   * the object's MAIN action changed (the pick became the hand: the lock just opened) -> back
    //     to that new default, so the very next A opens it;
    //   * otherwise keep the same ACTION selected, wherever it now sits in the list.
    static int lastDefault = IP_NONE;
    int prevChosen = (ip_actions_count > 0 && ip_action_index < ip_actions_count)
        ? ip_actions[ip_action_index]
        : IP_NONE;
    ip_actions_count = ip_build_actions(best, ip_actions);
    int newDefault = ip_actions_count > 0 ? ip_actions[0] : IP_NONE;
    if (targetChanged || newDefault != lastDefault) {
        ip_action_index = 0;
    } else {
        int found = 0;
        for (int i = 0; i < ip_actions_count; i++) {
            if (ip_actions[i] == prevChosen) {
                found = i;
                break;
            }
        }
        ip_action_index = found;
    }
    lastDefault = newDefault;
    kind = ip_chosen_kind();

    // ⚠️ SAY WHICH ACTION IS ON SCREEN, on every change. Without it the log cannot answer the one
    // question that matters about this prompt -- "what did it offer me?" -- and his report that a
    // just-picked lock still showed the pick instead of the hand was undecidable from a trace that
    // recorded the lockpick going out and nothing about what came back. The list is rebuilt every
    // tick and the index survives a rebuild, so a door that loses its IP_LOCKPICK entry should show
    // this flip to IP_USE (1) by itself; if it ever does not, this line is the proof.
    {
        static Object* lastObj = NULL;
        static int lastKind = -1;
        static int lastCount = -1;
        if (best != lastObj || kind != lastKind || ip_actions_count != lastCount) {
            lastObj = best;
            lastKind = kind;
            lastCount = ip_actions_count;
            trace_event(TRACE_FLOW, "ip-show", "pid=%d kind=%d idx=%d of=%d locked=%d",
                best->pid, kind, ip_action_index, ip_actions_count,
                FID_TYPE(best->fid) == OBJ_TYPE_SCENERY && obj_is_locked(best) ? 1 : 0);
        }
    }

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

// The mouse's way into the same queue (combat_rt, the right button outside a fight): the object the
// cursor is on, with its default action -- what A does when that object is the one lit up. Refused
// for anything the prompt itself would not offer, behind a wall (ip_pick_reach), or while the dude is
// still busy with the last one. The walk there is the action's own, as for A.
bool interact_prompt_request_object(Object* obj)
{
    if (obj == NULL || obj == obj_dude || ip_pending != NULL) {
        return false;
    }
    if (obj_dude != NULL && anim_busy(obj_dude) != 0) {
        return false;
    }
    int actions[IP_ACTIONS_MAX];
    if (ip_build_actions(obj, actions) <= 0 || !ip_pick_reach(obj)) {
        return false;
    }

    ip_pending = obj;
    ip_pending_kind = actions[0];
    trace_event(TRACE_ACTION, "interact-queue", "kind=%d pid=%d objType=%d src=mouse",
        actions[0], obj->pid, FID_TYPE(obj->fid));
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

// A SINGLE object is about to be freed (obj_erase_object): a scripted destroy, a corpse cleanup,
// or the OLD DUDE that obj_load_dude erases on its way in. The prompt caches raw Object* in
// ip_target / ip_pending and dereferences them (ip_classify, the overlay, the queued action), so a
// pointer that survives the free is a crash waiting for the next frame. Same hook and the same
// reasoning as combat_rt_notify_critter_removed and anim_notify_object_freed next to it.
//
// Only forgets the object being freed -- everything else the prompt knows is still valid, and a
// full reset here would drop a target the player is standing at for no reason.
void interact_prompt_notify_object_freed(Object* obj)
{
    if (obj == NULL) {
        return;
    }
    if (ip_target == obj) {
        ip_clear_outline();
        ip_hide_overlay();
        ip_target = NULL;
        ip_actions_count = 0;
        ip_action_index = 0;
    }
    if (ip_pending == obj) {
        ip_pending = NULL;
        ip_pending_kind = IP_NONE;
    }
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
