#include "game/combat_rt.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <SDL.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/combat_defs.h"
#include "game/combatai.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/trait.h"
#include "game/game.h"
#include "game/game_trace.h"
#include "game/gdialog.h" // dialog_active, for the post-conversation NPC hold
#include "game/gamepad_ui.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/perk_defs.h"
#include "game/proto.h" // proto_ptr, for the doorway magnet's scenery test
#include "game/proto_types.h" // SCENERY_TYPE_DOOR
#include "game/protinst.h" // obj_destroy, for spent healing items; obj_is_locked
#include "game/radial_wheel.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/skill.h" // skill_result, for the medic beat's First Aid / Doctor roll
#include "game/stat.h"
#include "game/stat_defs.h"
#include "game/tile.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"

namespace fallout {

// Realtime combat — design notes
// ================================
// This module replaces the blocking, turn-based combat() encounter with a
// per-frame, per-actor state machine that runs inside the existing realtime
// game loop (main_game_loop -> combat_rt_update).
//
// There is no combat "mode": NPCs aggro when the player enters their perception
// range, chase, and attack whenever their regenerating action-point pool can
// afford the attack's AP cost. The pool is capped at the actor's original Fallout
// max AP (Agility scales the cap) and refills one whole point at a fixed interval
// (RT_AP_REGEN_SECONDS) that is the same for everyone, so Agility governs how
// many actions an actor can stack, not how fast points come back. Movement is
// free. The player moves with WASD/stick and attacks by clicking a target.
//
// Damage/hit resolution is NOT reimplemented — it reuses combat_attack() (which
// in turn drives compute_attack/apply_damage/roll.cc) and the SPECIAL system via
// stat_level(). Because combat_attack() fills the single shared global Attack
// (main_ctd) and applies damage asynchronously through the animation queue, only
// one attack may be resolving at a time; rt_resolving enforces that (it is the
// "per-actor attack context" boundary — every actor owns its FSM/target/timers,
// and the one global hit-resolution slot is leased serially). The system never
// blocks: it only ever checks anim_busy() and returns, letting the main loop's
// process_bk() advance animations.
//
// The combat subsystem is kept "warm" (COMBAT_STATE_0x01 set) while hostiles are
// engaged so the reused attack/animation/damage code paths engage; the turn
// scheduler, action-point bar, and end-turn flow are never run.

#define RT_MAX_ACTORS 256

// Regenerating action-point pool (Variant A). Each actor has an AP pool capped
// at its original Fallout max AP (stat_level STAT_MAXIMUM_ACTION_POINTS, which
// already includes Agility). Actions spend their normal AP cost; the pool refills
// one whole point at a time on a FIXED interval that is the same for everyone --
// Agility scales the pool SIZE, never the regen rate. An action that the actor
// can't afford simply doesn't happen (no queueing). Movement is free (it never
// touches this pool; see the combat.ap top-ups, which stay only to keep anim.cc
// from truncating in-combat moves).
// Default seconds per +1 AP, fixed for all actors. Overridable from config
// (GAME_CONFIG_AP_REGEN_KEY) into rt_ap_regen_seconds at combat_rt_init, so the
// firing cadence can be retuned in the cfg without a rebuild. 0.7 => ~1.4 AP/sec
// => ~3.5s between 5-AP shots once the pool is drained.
#define RT_AP_REGEN_SECONDS 0.666667f

// How long a latched player attack intent stays live (seconds). A held attack
// button refreshes it every frame so it never expires while held; a one-shot
// mouse click sets it once and it survives this many seconds of locked/flinch
// frames -- so the click isn't lost just because the frame it landed on was busy.
#define RT_INTENT_TTL 0.4f

// Hard cap (seconds) on how long the hit-resolution slot stays leased to one
// attack. Normally the slot frees the instant the attacker's swing animation
// finishes (anim_busy == 0) and the damage commits then. But an attacker can
// chain straight into a hit-react, knockdown, or movement animation so anim_busy
// never returns to 0 -- which is exactly what wedged the lock permanently (see
// the watchdog log). Past this cap the attack commits and the slot releases
// regardless, so the damage always lands and the lock can never stick.
#define RT_RESOLVE_MAX_SECONDS 1.5f

// Minimum seconds an attack must animate before it may commit via swing-done / connect.
// Without this, an attack commits the same frame it's snapshotted whenever its defender is
// ALREADY mid-reaction (from a prior hit under focus-fire) -- connected reads true instantly --
// or its swing anim is momentarily read as done, so the attack resolves (damage + AP) with no
// visible swing or sound: the "shadow attack" the player reported. Holding the commit this long
// guarantees the swing plays and its SFX sounds first. The hard cap above still overrides it.
#define RT_MIN_SWING_SECONDS 0.3f

// Which frame an attack animation that has NO authored action frame discharges on, as a
// fraction of its own length. See the long note in rt_attack_action_frame_reached for why it
// is a quarter and not a half, and what it was measured against.
#define RT_UNAUTHORED_ACTION_FRAME(frames) ((frames) / 4 < 1 ? 1 : (frames) / 4)

// Backstop: how long the player may hold a movement input and be refused before combat_rt cuts
// whatever animation is holding him. Being unable to move with no way out is always a bug (a
// swing that never ends: traced at anim=16 stuck on its last frame for 6 s), so this is a
// safety net, not a balance lever. Comfortably longer than a real swing (~0.5 s) so it never
// cancels a legitimate attack. Every trip logs WARN/move-unstick -- if that shows up in normal
// play, the underlying sequence leak is still there and this is only papering over it.
#define RT_MOVE_STUCK_SECONDS 1.0f

// A dead critter that is still standing (its death fall never finished / got stuck) is
// force-laid-down after this long, so a corpse can't stand "halfway" forever and keep
// counting as a live hostile. A real death fall completes well under this.
#define RT_DEAD_STANDING_TIMEOUT 2.0f
// Largest sub-tile offset a completed DEATH FALL can legitimately leave behind. The fall art adds
// a fixed per-rotation hot-spot displacement and all of them measured <= 11 px (2026-08-10), so a
// corpse past this did not get there by falling -- it kept a walk offset from being killed
// mid-step, which renders as a body hanging above its tile.
#define RT_CORPSE_FALL_OFFSET_MAX 12

// How long a knocked-down critter stays on the ground after its fall animation finishes before it
// starts to stand up. Without it, rt_needs_standup fires the frame the fall ends, so the fall flows
// straight into the standup with no time down and the knockdown reads as "fell and instantly got
// up" -- it disables nothing. This dwell gives the knockdown a readable, meaningful beat. Symmetric
// (dude and NPCs). Kept short so a downed critter isn't helpless for long (with the 2026-07-16
// change, a downed critter is attackable the whole time).
// 2026-08-13: 0.6 -> 2.0, and made a cfg lever (`knockdown_dwell`). 0.6 was chosen when a swing
// came every second; at the tactical pace (attack_cooldown_base 2.4) it was over before the player
// could read it -- "spadla na zem a po výstřelu hned vstala". Roughly one attack cycle on the
// ground makes a knockdown cost the victim a turn, which is what it is supposed to mean.
// Scaled WITH attack_cooldown_base -- the point of the dwell is that a knockdown costs its victim
// roughly one attack cycle, so it has to move whenever the pacing scheme does. 1.7 belongs to the
// NORMAL scheme (base 2.6); the SLOW scheme used 2.0 against base 3.1.
#define RT_KNOCKDOWN_DWELL_SECONDS 1.7f
static float rt_knockdown_dwell = RT_KNOCKDOWN_DWELL_SECONDS;

// AGILITY shortens the ground dwell: mult = 1 - (AGI - 5) * factor, the same idiom as the run
// speed and the attack cooldown. The dwell above is the AGI 5 value, so an average character is
// unchanged and the stat moves the rest around it.
//
// ⚠️ AGILITY, NOT ENDURANCE, AND THE REASON IS ALREADY WRITTEN DOWN IN THIS PROJECT. run_speed_scale
// states the split: "Endurance already governs how LONG you can sprint (the AP drain), so the pair
// reads as AGI = speed, EN = stamina, with no overlap." Hauling yourself off the floor is speed.
// Endurance is not idle either -- it already owns three knockdown-adjacent jobs: the sprint drain,
// the anti-stunlock flinch threshold and its recovery, and in VANILLA the crit tables roll
// STAT_ENDURANCE to decide whether a knockdown becomes a KNOCKOUT (combat.cc:131, :165, :885...).
// So the pair reads: EN decides whether you go down, AGI decides how fast you are back up.
//
// ⚠️ THE ANTI-PIN CAP (rt_knockdown_ground_cap) IS DELIBERATELY NOT SCALED. It is a safety backstop,
// not a character trait -- a ceiling a stat can raise is not a ceiling.
#define RT_KNOCKDOWN_AGILITY_FACTOR 0.06f
static float rt_knockdown_agility_factor = RT_KNOCKDOWN_AGILITY_FACTOR;

// HARD CAP (seconds) on the TOTAL time a critter spends resting on the ground, measured from the
// moment its fall animation finishes. Past this it stands up no matter what is still being done to
// it.
//
// Why it exists (trace_2026-08-24_185614, the dude): rt_bump_prone_dwell restarts the dwell on
// every DAMAGING hit that lands on a downed critter -- deliberately, so shooting a downed enemy
// keeps it down. But that restart had no ceiling, so any group whose combined hit cadence is
// shorter than the dwell pins its victim on the ground FOREVER. Measured: the dude fell at
// s=63.21, his fall finished at s=64.21, and he was still prone at s=70.26 -- six seconds against a
// 1.7s dwell, with ZERO [FLOW/standup] in the log -- while two rats alternating at ~1s intervals
// took him from 35 HP to 4. His input is blocked the whole time he is prone
// (rt_dude_incapacitated), so that is a total, unbounded lockout ending in death.
//
// Note the EN-scaled flinch immunity (flinch_accum / immune_until) does NOT cover this: it was
// built for the light hit-react stagger only. This cap is the knockdown's equivalent.
//
// Symmetric (dude and NPCs) -- an enemy pinned down by focus-fire has exactly the same problem.
// Roughly two dwells, so a downed critter can still be kept there for a meaningful beat by
// sustained fire; it just can't be kept there indefinitely. cfg lever `knockdown_max_ground`;
// 0 = no cap (the pre-2026-08-24 behaviour). Scale it with knockdown_dwell.
#define RT_KNOCKDOWN_GROUND_CAP 3.4f
static float rt_knockdown_ground_cap = RT_KNOCKDOWN_GROUND_CAP;

// A chasing critter whose walk animation is playing but whose TILE hasn't changed for this
// long is stuck (crowd gridlock: everyone stepping into everyone else at once) -- its walk is
// force-cleared so the FSM re-plans. A legit walk crosses a tile boundary well under this
// (it resets the timer each tile), so this only ever fires on a genuinely frozen mover.
#define RT_STUCK_MOVE_TIMEOUT 1.25f

// Hard cap (seconds) on how long the dude is left in a hit-react flinch before
// combat_rt force-cancels it, so a hit never freezes the player's movement for
// long even if they give no movement input. A movement input cancels it sooner
// (rt_cancel_flinch_for_move); this is the no-input backstop. Scoped to the
// standing flinch only -- knockdowns are a real disabling state and are left to
// run their course / the standup pass.
#define RT_FLINCH_MAX_SECONDS 0.4f

// Window (seconds) after the last movement input during which the dude counts as
// "moving" for combat_rt_dude_moving(). Suppresses the enemy-miss dodge at its
// source while a direction is held, and bridges the gap between the input poll and
// the enemy-attack registration (both happen within one combat_rt_update tick).
#define RT_MOVE_INPUT_GRACE 0.2f

// How often the actor registry is rebuilt from the world (seconds). Rebuilding
// picks up spawned/removed critters and map changes while carrying over the FSM
// state of critters that persist.
#define RT_REFRESH_INTERVAL 0.5f

// Aggro radius (tiles) = max(PERCEPTION * scale, floor). obj_dist and PERCEPTION
// are both in tiles, but raw PE is a short radius for realtime play, so scale it
// up and apply a floor so even low-perception critters engage at a usable range.
#define RT_AGGRO_PERCEPTION_SCALE 2
#define RT_AGGRO_MIN_TILES 5

// R3 AUTO-LOCK RANGE (tiles) = max(PERCEPTION * scale, floor). Same shape as the aggro radius above,
// deliberately: "how far can this character pick a target out of the scene" is the same question in
// both directions, so it should read the same way.
//
// It used to be UNBOUNDED -- rt_nearest_lockable simply took the nearest lockable critter in the
// registry, anywhere. That made Perception worth nothing to the one gesture that is entirely about
// noticing someone.
//
// Numbers at 2.0 / 5: PE 1 -> 5, **PE 4 -> 8**, PE 7 -> 14, PE 10 -> 20. Identical to the aggro
// radius on purpose, so the range at which NPCs notice YOU and the range at which you can single one
// of THEM out are the same number.
//
// ⚠️ HOW THAT NUMBER WAS ARRIVED AT, because the first attempt was wrong. It shipped at 1.0 / 3 --
// PE 4 -> 4 tiles -- from the user's "melee range plus a bit", which I took literally as 1+3 without
// checking what "range" means in this game's data. Measured from his own traces: melee/unarmed is
// range 1 (3393 samples), spears 2, pistols and SMGs 14-25, rifles 40-45; and roughly 12-13 tiles
// fit on screen at 853x480. So 4 tiles left enemies plainly VISIBLE but unlockable, which reads as a
// bug rather than as a stat. Anchor a reach on what the data says, not on what the phrase suggests.
//
// A low-PE character with a long gun still out-ranges his lock (SMG 25 vs 8 at PE 4). That is the
// intended cost of a dull-eyed brawler build; both numbers are cfg (`lock_range_pe_scale`,
// `lock_range_min`) if it ever needs widening for a sniper.
#define RT_LOCK_PERCEPTION_SCALE 2.0f
#define RT_LOCK_MIN_TILES 5
static float rt_lock_pe_scale = RT_LOCK_PERCEPTION_SCALE;
static int rt_lock_min_tiles = RT_LOCK_MIN_TILES;

// Extra tiles beyond the chase range before a chasing critter gives up (returns to
// IDLE). Keeps the engage/disengage from flickering at the boundary.
#define RT_DEAGGRO_MARGIN 3

// Pursuit (chase) range for an already-HOSTILE critter, in tiles. This is deliberately
// much larger than the aggro radius: aggro_range is the short "notice an unaware target"
// radius, but once a critter is hostile (it has been shot, or its faction was), it must
// pursue across the whole engagement -- otherwise a ranged player simply shoots from past
// the (small) aggro radius and strolls away while nothing ever closes in (the broken-kiting
// bug). Set comfortably beyond typical weapon ranges so "if it can be shot, it gives chase";
// the player still breaks contact by getting this far away (or leaving the map).
//
// NOTE: since the vanilla stop rule below was ported this is only the CHEAP OUTER BOUND on a
// pursuit -- an early-out that saves a line-of-sight trace at absurd distances. What actually
// ends a chase now is perception (rt_wants_to_stop), which bites long before 50 tiles.
#define RT_CHASE_LEASH_TILES 50

// How close a follower keeps to the dude while a fight is live and it has no target of its
// own. Only used by the in-combat follow in RT_IDLE -- out of combat the vanilla party script
// owns following and this is not consulted. Four tiles is inside the dude's own screen and
// close enough that the follower's next look around covers what he is fighting.
#define RT_FOLLOW_TILES 4

// VANILLA COMBAT-END RULE -- see rt_wants_to_stop. A fight ends on PERCEPTION, not on distance,
// so these two levers only pace HOW OFTEN the (line-of-sight tracing) test runs and how long the
// "I can't perceive anyone" condition must hold before the chase is actually dropped.
//
// The grace exists because perception flickers: one step behind a doorframe drops can_see and
// collapses the radius from PE*5 to PE*2, and without a hold a critter would give up the instant
// you rounded a corner and re-acquire a step later, thrashing rt_engaged (and with it the
// proximity-regen multiplier) several times a second.
#define RT_STOP_CHECK_SECONDS 0.5f // interval between perception tests for one actor
#define RT_STOP_GRACE_SECONDS 3.0f // how long it must perceive nobody before it disengages

// How long an actor that gave up on an UNREACHABLE target refuses to engage again, while it is
// still standing on the tile it gave up from. See RtActor::unreachable_tile.
//
// EVIDENCE (trace_2026-08-07_174713.log): dropping the target to RT_IDLE was not enough, because
// the RT_IDLE branch re-acquired it on the very next frame. id=687, rooted on tile 16523 and
// unable to path anywhere, logged FLOW/unreachable every 2.02 SECONDS like a metronome (13 times)
// and 1575 of the log's 1575 failed pathfinds -- and, being permanently non-IDLE, it alone kept
// rt_engaged true from t=47.5 to the end of the session. That is what the player felt as "I never
// cool down from combat". Long enough that the retry is rare; the tile check is what actually
// releases it, so a critter that gets pushed/moves at all tries again immediately.
#define RT_UNREACHABLE_RETRY_SECONDS 30.0f

// Preferred engagement distance (tiles). An NPC closes to within this before it opens fire,
// instead of attacking the instant the target is within max WEAPON RANGE. A ranged weapon can
// reach 20-40 tiles, but Fallout's to-hit has a steep per-tile distance penalty, so a critter
// that stopped and shot from max range just plinked away missing forever (the "distant guy
// always misses" complaint). Capped against the weapon's own range, so melee (range 1) and
// already-short weapons are unaffected -- only long-range shooters are pulled in closer where
// their shots actually connect.
#define RT_PREFERRED_ENGAGE_TILES 8

// Tiles a ranged NPC advances per hop when it has distance on its target but NO line
// of fire (target behind a wall). Kept small so line-of-sight is re-checked after
// each short pathfound hop and the NPC opens fire the instant it rounds the corner,
// rather than marching all the way into melee. See rt_advance_to_target.
#define RT_REPOSITION_STEP_TILES 4

// Gunshots carry. Vanilla Fallout only aggros onlookers who can SEE the fight (plus a
// tiny non-LoS awareness of ~Perception tiles), so guards behind a wall never react to
// gunfire. This extends the aggro-spread so a same-team critter also "hears" a FIREARM
// fight within (its Perception * this multiplier) tiles with NO line of sight required
// -- scaling the earshot with the listener's Perception, as it should. At PER 5 that's
// ~15 tiles (a building + just outside); a distant patrol still won't hear. Melee is
// unaffected (a knife scuffle isn't loud). Deviation from vanilla, by design.
#define RT_GUNSHOT_HEAR_PER_MULT 3

typedef enum RtState {
    RT_IDLE, // Not engaged.
    RT_CHASE, // Has a target, closing to weapon range.
    RT_ATTACK, // In range; attack whenever the AP pool can afford it.
} RtState;

typedef struct RtActor {
    Object* obj;
    RtState state;
    Object* target;
    int ap; // Current action-point pool (whole points, capped at max AP).
    float ap_regen; // Seconds accumulated toward the next +1 AP tick.
    // Fractional AP burned by SPRINTING, mirroring the dude's rt_run_ap_drain. A chasing NPC pays
    // the same run cost the player does, but stops short of its attack reserve -- rt_npc_sprint_ok.
    float run_ap_drain;
    // Fractional AP burned by WALKING, the NPC half of the dude's rt_walk_ap_drain. Same attack
    // reserve as the sprint drain, so a walking chaser also always arrives able to swing.
    float walk_ap_drain;
    // rt_clock at which this actor FIRST failed to find a path to its current target, or -1 when
    // the last routing attempt succeeded. Once the failure has persisted for
    // RT_NO_PATH_GIVEUP_SECONDS the target is unreachable and gets dropped -- see RT_CHASE.
    float no_path_since;
    // Vanilla stop-rule bookkeeping (see rt_wants_to_stop). stop_check_at is the rt_clock at which
    // this actor may run the next perception test -- the test walks a line of sight, so it is
    // throttled to RT_STOP_CHECK_SECONDS rather than run every frame. unperceived_since is the
    // rt_clock at which it FIRST perceived nothing worth fighting, or -1 while it still does; once
    // that has held for RT_STOP_GRACE_SECONDS the chase is dropped.
    float stop_check_at;
    float unperceived_since;
    // "I already proved I cannot get anywhere from here." Set when rt_target_unreachable drops a
    // target: the tile the actor was standing on at the time, and the rt_clock after which it may
    // try again. Re-aggro is refused while the latch holds AND the actor is still on that tile --
    // without it a rooted critter re-acquires the same unreachable target the very next frame and
    // loops forever (see the RT_IDLE re-aggro branch).
    int unreachable_tile;
    float unreachable_until;
    // Earliest rt_clock at which the in-combat FOLLOW may ask the pathfinder for a route to the dude
    // again after one failed. A follower is never told to give up on its leader (it is not fed to
    // the unreachable latch), so without this a follower with no way through re-ran a full path
    // search every frame -- and since 2026-10-04 that search reaches six times further.
    float follow_retry_at;
    // LOOK AROUND (rt_idle_look_around): when an idle hostile next turns its head on its own; a turn
    // toward a gunshot it heard, queued for a beat after the shot (-1 = none) and aimed at that
    // tile; and which shot it has already reacted to, so one shot turns it once.
    float look_next_at;
    float look_noise_at;
    int look_noise_tile;
    float look_noise_seen;
    // Latched once the sprint has drained the pool down to the attack reserve; cleared only after
    // the pool has recovered by RT_NPC_RUN_RESUME_MARGIN. Without this latch the reserve became a
    // free-sprint equilibrium -- see rt_npc_sprint_ok.
    bool run_winded;

    // BREATHER. Set when the pool has drained to the attack reserve, cleared once it has
    // recovered by RT_NPC_BREATHER_MARGIN -- the NPC half of the dude's rt_move_blocked. While
    // set, CHASE movement is suppressed: the critter stands and catches its breath instead of
    // walking at you for ever on a pool that can never empty.
    //
    // ⚠️ breather_since IS THE SAFETY VALVE, not bookkeeping. The reason NPCs never had a stop
    // is the "enemy runs up and just looks at me" bug, and an unbounded latch is exactly how
    // that comes back. RT_NPC_BREATHER_MAX_SECONDS caps every pause, so the worst case a bug
    // here can produce is a critter that hesitates -- never one that is stuck.
    bool breather;
    float breather_since;

    bool hostile; // Genuinely hostile to the dude; see rt_refresh_registry.
    bool friendly; // A party member / follower -- fights the dude's enemies (see rt_update_actor).
    int last_results; // combat.results last frame, for watchdog event detection.

    // Engine-assigned combat target that is ANOTHER non-player critter (a scripted
    // op_attack -- e.g. Gizmo's assassin sent at Killian). Derived from this critter's
    // whoHitMe each refresh (op_attack / apply_damage overload it as "my current target"),
    // and drives the same CHASE/ATTACK states used against the dude, so NPC-vs-NPC fights
    // actually play out in realtime. NULL when the target is the dude/party (that's the
    // `hostile` path) or there is none.
    Object* engine_target;

    // Per-frame attack intent. The FSM records (not fires) an attack here when the
    // actor is in range with AP to spare; the central arbiter (rt_resolve_attacks)
    // then grants the single shared hit-resolution slot to the highest-Sequence
    // intent. Rebuilt every frame (cleared at the top of rt_update_actor).
    bool wants_attack;
    Object* attack_target;
    int attack_mode;
    // Earliest rt_clock at which this critter may swing again -- its own physical rate of fire,
    // set when it attacks. See rt_attack_cooldown. Nobody else can extend it, which is the whole
    // point: a cooldown cannot be used to lock somebody down.
    float next_attack_at;

    // Have we already tried to draw this NPC's best weapon for the current
    // engagement? Set the first time it acquires a target (see rt_ensure_weapon_-
    // drawn), cleared when it drops back to RT_IDLE. Prevents re-running the
    // inventory search / take-out animation every frame while it holds the weapon.
    bool weapon_checked;

    // rt_clock until which this critter's weapon counts as ALREADY SHOULDERED, so the ANIM_POINT
    // raise is skipped. Stamped on every shot; see combat_rt_weapon_is_up.
    float weapon_up_until;

    // Earliest rt_clock at which this critter may use a healing item again -- the medic beat's own
    // rate limit, separate from next_attack_at because healing and swinging are different actions
    // and sharing one timer would let a heal buy a free attack (or the reverse). See rt_try_medic.
    float heal_next_at;

    // rt_clock when this critter was first seen dead (HP <= 0) but still standing. Lets
    // a corpse whose death animation got stuck (killed mid-walk -> competing sads ->
    // anim_busy never returns to 0) be force-reaped after RT_DEAD_STANDING_TIMEOUT,
    // instead of standing "halfway" forever and still counting as a live hostile.
    // -1 = not currently dead-standing.
    float dead_since;

    // The death frame dead_since was last reset on. ⚠️ "Stuck" means NOT ADVANCING, not "longer
    // than 2 s" (2026-10-09): the Master's death is 61 frames, ~6 s, and the old wall-clock timeout
    // reaped it four times while it played -- register_clear + critter_kill in the middle of a death
    // that was going perfectly well, and he was seen twice ("master byl taky rozdvojen, když umřel").
    int dead_fid;
    int dead_frame;

    // Where this critter's body sits on its tile while it STANDS, sampled while alive and idle. Most
    // critters stand at 0,0; the Master is PLACED at -34,-48 by his map (vanilla). Corpse grounding
    // measures a "stray" offset against this and grounds to it -- grounding to 0,0 threw his corpse
    // 34 px right and 48 px down the moment his death ended (trace_2026-10-09_120546 s=743).
    int rest_x;
    int rest_y;

    // NOTE: the knockdown clocks that used to live here (prone_since / prone_fall_end) belong to
    // the critter's RtReaction entry now -- see the REACTION OWNERSHIP block. They were per-actor,
    // so the dude needed his own duplicate pair and bystanders had none at all.

    // Flinch-immunity (anti-stunlock) state. flinch_accum = seconds of CONTINUOUS hit-
    // reacting (builds while flinching, decays otherwise); when it crosses the EN-scaled
    // threshold the critter gets a brief immunity window (immune_until = rt_clock + EN-
    // scaled recovery) during which hits still damage it but DON'T stagger it, so it can
    // act instead of being flinch-locked to death under focus-fire. See rt_play_hit_react.
    float flinch_accum;
    float immune_until;

    // Stuck-move watchdog. In a crowd, many critters path to the same tile (the dude)
    // at once and step into each other's tiles simultaneously (realtime has no turns to
    // serialise them), so a walk animation plays but the body never translates -- it
    // "walks in place" forever, anim_busy stays set, and the FSM (gated on anim_busy)
    // never re-issues. stuck_tile / stuck_since track the last tile the critter actually
    // occupied and when it got there; if it stays mid-walk on the same tile past
    // RT_STUCK_MOVE_TIMEOUT, the walk is force-cleared so the FSM re-plans. See rt_update_actor.
    int stuck_tile;
    float stuck_since;

    // The SUB-TILE offset we last saw. A critter creeping across a tile has not moved `tile` yet,
    // but these are changing every frame -- which is the difference between slow and stuck.
    int stuck_x;
    int stuck_y;

    // TEMP DIAG state for the "chase-why" trace line: last logged decision inputs, so the line is
    // emitted on change instead of every frame. Strip with the diagnostic.
    int diag_mode;
    int diag_range;
    int diag_dist;
    // TEMP DIAG (walk sem-a-tam, 2026-08-31): which MOVER ran last, so the chase line is emitted on
    // a change of branch instead of every frame. -1 = nothing logged yet. STRIP with chase-move.
    int diag_mover;

    // TEMP DIAG for the ping-pong (2026-08-28): pid of whatever refused the STRAIGHT step, so the
    // blocker line is emitted on change instead of every frame. -2 = nothing logged yet.
    int diag_blocker;

    // TEMP DIAG for the "běhalo sem a tam jako pták uličník" report: detects a critter REVERSING --
    // stepping back onto the tile it just left. One reversal is ordinary (the player moved, a path
    // re-planned); a run of them while the DUDE IS STANDING STILL is the pathological jitter, and
    // that distinction is the whole point of the counter. Strip once the cause is found.
    int osc_tile;      // tile it occupied before the current one
    int osc_prev_tile; // the one before that -- a return to it is a reversal
    float osc_since;   // when the current run of reversals started
    int osc_count;     // reversals in this run

    // BALANCE TELEMETRY. Seconds this actor spent in each "why am I not swinging" bucket, sampled
    // once a frame -- see rt_balance_sample. The point is to answer the one question every balance
    // lever depends on and which no amount of watching can settle: WHAT IS ACTUALLY THROTTLING THE
    // FIGHT? An actor starved of AP and an actor sitting on cooldown look identical on screen, and
    // they need opposite fixes. Reported per combat by rt_balance_report.
    float bal_busy;     // playing an animation (its own swing, a hit react, a fall)
    float bal_cooldown; // ready and in range, but inside its own rate-of-fire
    float bal_ap;       // ready and in range, but cannot afford the attack
    float bal_approach; // chasing / not in range yet
    float bal_ready;    // nothing stopping it -- should be attacking this frame
    float bal_breath;   // seconds held in the breather (a SUBSET of bal_approach)
    int bal_attacks;    // attacks it actually committed
    float bal_seen;     // rt_clock when it first entered the fight

    // SCRIPT TURNS (MODS "Script combat turns", cfg script_turns) -- see rt_script_turn_tick.
    float script_turn_at;    // rt_clock of its next script event in this fight; < 0 = none yet
    float script_hold_until; // its script took the turn (script_overrides): the AI stands down until then
    bool script_fighting;    // was it in the fight at its last script event (4 = turn, 5 = join check)
    bool script_combatant;   // joined this fight -- sticky until it ends, as vanilla's combat list is
    int script_turns;        // events its script has had in this fight, for the trace

    // FLEEING (rt_flee_step): cornered last time it looked -- the next look is a turn later, as in vanilla.
    float flee_retry_at;
    bool fleeing;      // rt_flee_step owns it: it pays for its legs the way the DUDE pays (no reserve)
    bool flee_blocked; // ran its pool dry: stands, like the dude's rt_move_blocked, until resume_ap
    bool wipe_pending; // entered its breather mid-step: wipe its forehead once the body is free
} RtActor;

static RtActor rt_actors[RT_MAX_ACTORS];

// --- BALANCE TELEMETRY ---
//
// A per-combat report, dumped when the fight ends, that says what the numbers ACTUALLY produced
// rather than what they were supposed to produce. Every lever we have (ap_regen, the proximity
// multipliers, the attack cooldown, the AGILITY coupling) changes the same visible thing -- how
// often anyone swings -- so watching a fight cannot tell them apart. This can: it splits every
// actor's time into WHY it was not attacking, and the biggest bucket names the lever that matters.
//
// Read it as: cooldown% high -> attack_cooldown_base is the throttle, AP changes will do nothing.
// ap% high -> the regen levers are the throttle. approach% high -> it is a movement/positioning
// problem and neither. ready% should be near zero; anything else means the arbiter is refusing
// attacks nobody asked it to refuse.
static float rt_bal_dude_busy = 0.0f;
static float rt_bal_dude_cooldown = 0.0f;
static float rt_bal_dude_ap = 0.0f;
static float rt_bal_dude_approach = 0.0f;
static float rt_bal_dude_ready = 0.0f;
static int rt_bal_dude_attacks = 0;
static float rt_bal_start = -1.0f; // rt_clock when the current fight began

// TEMP DIAG (invisible-caravan hunt, 2026-08-13). Reports any CRITTER on the dude's elevation whose
// current FID has no art: the object is alive, scripted and blocking its tile, but nothing is drawn
// -- exactly the shape of the invisible-children bug, where the proto and the map entry were fine
// and only the FRMs were missing. Distinguishes "the art is absent" from "the object is hidden /
// somewhere else", which is the fork we cannot guess from a screenshot.
//
// Throttled and reported ONCE PER PID: art_exists is a cache probe that can reach the disk, so this
// must never run per critter per frame. Strip once the caravan is explained.
#define RT_ART_SCAN_SECONDS 2.0f
#define RT_ART_SEEN_MAX 32
static float rt_art_scan_at = 0.0f;
static int rt_art_seen[RT_ART_SEEN_MAX];
static int rt_art_seen_count = 0;
static void rt_missing_art_scan(); // defined below rt_clock

// TEMP DIAG (oscillation hunt): how long a run of reversals may span before it is treated as a new
// run, and how many reversals inside that span are worth a trace line. Two is normal (out and
// back); three or more in five seconds is not.
#define RT_OSC_WINDOW_SECONDS 5.0f
#define RT_OSC_REPORT_AT 3

// The dude's tile when the current oscillation run started, so the report can say whether the
// PLAYER was the one moving. Single global: only one run is ever being reported at a time.
static int rt_osc_dude_tile = -1;
static int rt_actor_count = 0;
static bool rt_engaged = false;
static float rt_refresh_timer = 0.0f;

// Flinch-immunity (anti-stunlock) for the dude -- the RtActor.flinch_accum/immune_until
// equivalents (he isn't in the actor registry). Reset in rt_reset_session_state.
static float rt_dude_flinch_accum = 0.0f;
static float rt_dude_immune_until = 0.0f;

// Flinch-immunity tuning (hardcoded for now; could be cfg levers later). A critter that
// has been CONTINUOUSLY hit-reacting for ~threshold seconds gets a ~recovery-second
// window where hits don't stagger it, so heavy focus-fire can't flinch-lock it to death
// without it ever acting. Endurance shifts it: high EN breaks out sooner and stays out
// longer (a tank shrugs off suppression); clamped so even max EN still visibly flinches
// and even min EN gets some breathing room.
#define RT_FLINCH_LOCK_THRESHOLD 1.0f   // base seconds of continuous flinching -> immunity
#define RT_FLINCH_RECOVERY 0.5f         // base immunity-window seconds
#define RT_FLINCH_EN_THRESH_FACTOR 0.07f // per EN point below/above 5: shortens threshold
#define RT_FLINCH_EN_RECOV_FACTOR 0.05f  // per EN point: lengthens recovery
#define RT_FLINCH_DECAY 2.0f            // accum decays this-x faster than it builds, in a gap

// Damage-Threshold gate on the hit STAGGER (the light flinch that interrupts a walk). A hit only
// staggers the defender if the damage that got THROUGH its armor is at least this fraction of the
// defender's Damage Threshold for that damage type. So a well-armored critter (high DT) shrugs off
// glancing hits and keeps advancing, only flinching on a solid blow; an unarmored one (DT 0)
// flinches on any damaging hit exactly as before. Only gates the cosmetic flinch -- damage/death/
// knockdown are unaffected. 0 = every damaging hit flinches (old behavior). cfg flinch_dt_factor.
#define RT_FLINCH_DT_FACTOR 2.0f

// True once combat_rt owns combat. While set, the turn-based combat() scheduler
// refuses to run, so the old combat mode can never take over.
static bool rt_enabled = false;

// Player action-point pool, same model as the NPC pool (RtActor.ap). Spent on
// attacks, refilled one point per rt_ap_regen_seconds. Starts "full" via INT_MAX
// clamped down to the dude's max AP on the first valid update frame.
static int rt_player_ap = INT_MAX;
static float rt_player_ap_regen = 0.0f;

// BONUS MOVE: a second pool that ONLY movement may spend. Vanilla grants 2 free move points per
// rank at the start of every turn (combat_free_move, combat.cc) -- and that turn setup never runs
// under realtime, so the perk gave nothing at all. With no turns, "every turn" becomes "while you
// stand": the reserve refills at the dude's normal regen rate whenever he is neither walking nor
// running, and never while he is.
//   * Movement spends it FIRST (rt_take_move_point), so a burst of walking leaves the attack pool
//     alone for 2 points per rank. Attacks, reloads and items never touch it.
//   * It can be walked on at 0 regular AP -- vanilla's "shoot, then step back" -- but it is small
//     and does not refill on the move, so it cannot feed kiting: once it is spent the empty-pool
//     stop applies exactly as before, and that stop still lifts only on the REGULAR pool.
// Same INT_MAX "start full" sentinel as rt_player_ap; out of a fight it is simply held full.
static int rt_bonus_move_ap = INT_MAX;
static float rt_bonus_move_regen = 0.0f;

// Live AP-regen interval (seconds per +1 AP), loaded from config at combat_rt_init
// (default RT_AP_REGEN_SECONDS). Tunable in the cfg for firing cadence without a
// rebuild. Clamped to a sane floor so a bad value can't make AP regen instant.
static float rt_ap_regen_seconds = RT_AP_REGEN_SECONDS;
// How hard AGILITY scales the AP refill interval (cfg ap_regen_agility_factor). See
// rt_regen_interval. Clamps keep a silly cfg from making anyone instant or catatonic.
// 0.05 = a 1.6x refill spread across AGI 1..10. Deliberately BELOW the 0.09 (2.5x) it shipped at
// this morning: Agility already caps the pool via vanilla's own 5 + AGI/2, so the rate stacks on
// top of a 2x advantage the stat had anyway -- exactly the double-dip flagged when this was
// designed and then talked out of. 1.6x on the rate plus 2x on the pool lands total throughput
// near vanilla's 2x, instead of well past it.
static float rt_ap_regen_agility_factor = 0.05f;

// Minimum seconds between one critter's own attacks (cfg attack_cooldown_base; 0 = disabled, which
// is exactly the old behaviour). Scaled per critter by STR/PE/AGI -- see rt_attack_cooldown.
// 2026-08-13: raised 1.0 -> 2.4 for a deliberately SLOWER, more tactical exchange (the user's
// reference is Dragon Age: Origins -- one swing every couple of seconds, decisions between them
// rather than per-swing button work). This is the right lever for "combat is too fast" because the
// balance telemetry showed the cooldown, not AP, is what actually gates a fight in contact range.
// THE "NORMAL" PACING SCHEME (2026-08-14). Base and factor are ONE curve and must move together:
// the factor is symmetric about stat 5, so widening the spread SPEEDS UP everyone above average --
// at base 2.9 with 0.12 the player's own stat 7 came out at 2.20 s, barely slower than the 1.97 s
// he was trying to get away from. The base pays that back.
//
// 2.6 is derived from play-tested verdicts, not guessed: base 1.0 gave him 0.82 s ("too fast"),
// 2.4/0.09 gave 1.97 s ("much better, the right note"), 3.1/0.12 gave 2.36 s ("very slow"). 2.6/0.12
// puts him back at 1.98 s -- the tempo he confirmed -- while KEEPING the wider spread he asked for
// afterwards. Sized against the enemies actually in his game (PE clusters at 4-6): stat 4 = 2.91 s
// against his 1.98 s, so he lands about three swings to a typical thug's two.
//
// stat 1 = 3.85 s, 5 = 2.60 s, 7 = 1.98 s, 10 = 1.04 s. Spread 3.7x (was 2.5x at factor 0.09).
// A SLOW scheme (3.1 / 0.12 / knockdown_dwell 2.0) is recorded in the balance-schemes note.
static float rt_attack_cooldown_base = 2.6f;
static float rt_attack_cooldown_stat_factor = 0.12f;
// How hard WEAPON SKILL bends the same cooldown -- the second, weaker term. Spans roughly +-this
// much across the whole 0-200 skill range. Kept far below the SPECIAL factor on purpose; see the
// long note in rt_attack_cooldown. 0 = skill has no effect on speed, which is vanilla.
#define RT_ATTACK_COOLDOWN_SKILL_FACTOR 0.15f
static float rt_attack_cooldown_skill_factor = RT_ATTACK_COOLDOWN_SKILL_FACTOR;
// ⚠️ 1.0 = SYMMETRIC ON PURPOSE, and the reason is worth keeping (2026-08-10).
// The attack ANIMATION curve is deliberately asymmetric (ANIM_ATTACK_STAT_HIGH_BIAS 1.5), and the
// justification written there is explicit: it is safe to exaggerate the top end BECAUSE animation
// speed is cosmetic -- the AP pool paces the attacks, so a dramatic curve buys a look, not power.
// I then copied that same bias onto THIS lever, which is the exact opposite: a cooldown IS
// throughput. At 1.5 it made STR 10 cost 0.325 s -- barely a cooldown at all -- and the player
// caught it as "attacks at Strength 10 feel maybe too fast" while the trace showed his shortest
// gaps at 0.36 s, i.e. precisely this value. Symmetric gives STR 1 -> 1.36 s, 5 -> 1.00, 10 ->
// 0.55: still a 2.5x reward for the stat, without handing the top end a free pass.
// LESSON: a curve shape is not portable between a cosmetic lever and a balance lever.
// 2026-08-23: LOWERED TO 0.6 and made cfg-tunable, on his report that his own attacks felt too
// fast. Measured first: his balance row read cd=1.05s with cool=50% ap=0% -- the cooldown was the
// only thing pacing him, and AP was not gating him at all, so no AP lever could have helped. The
// 1.05 s was entirely the stat term: STR 10 is five points over average and at a symmetric 0.12
// that is a 60% cut off the 2.6 s base. The BASE was not wrong (a STR 1 cave rat sat at a perfectly
// reasonable 4.04 s) -- only the reward at the top of the curve was. Damping just that side takes
// him to ~1.68 s and leaves every weaker critter untouched.
// 2026-08-24: 0.6 -> 0.83, bracketed by his own two verdicts (1.05 s "too fast", 1.59 s "STR 10
// should hit fast"); 0.83 gives the 1.25 s he asked to try. Synced here 2026-10-10 -- until then
// it lived only in his fallout.cfg and the release build fell back to 0.6.
#define RT_ATTACK_COOLDOWN_HIGH_BIAS 0.83f
static float rt_attack_cooldown_high_bias = RT_ATTACK_COOLDOWN_HIGH_BIAS;
#define RT_ATTACK_COOLDOWN_MIN 0.25f
// The dude has no RtActor, so his cooldown lives here. Reset with the rest of the session state.
static float rt_player_next_attack_at = 0.0f;
#define RT_AP_REGEN_STAT_MIN 0.35f
#define RT_AP_REGEN_STAT_MAX 2.0f

// Run-cost (Variant A nerf): running IN COMBAT drains the shared AP pool, so
// sprinting and shooting compete -- you can run to dodge OR shoot, not both for
// free. Walking and out-of-combat running stay free. Both loaded from config at
// combat_rt_init (defaults below), tunable without a rebuild.
#define RT_RUN_AP_COST 3.0f   // AP/sec drained while running in combat (base, before EN)
#define RT_RUN_AP_ENDURANCE_FACTOR 0.1f // AP/sec the run cost drops per point of Endurance
#define RT_RUN_AP_MIN_COST 0.2f // floor for the EN-reduced run cost (running always costs)
#define RT_RUN_RESUME_AP 3    // walk until AP recovers to this, then sprint again
static float rt_run_ap_cost = RT_RUN_AP_COST;
static float rt_run_ap_endurance_factor = RT_RUN_AP_ENDURANCE_FACTOR;
static int rt_run_resume_ap = RT_RUN_RESUME_AP;

// DT gate on the hit stagger (see RT_FLINCH_DT_FACTOR). Loaded from cfg at combat_rt_init.
static float rt_flinch_dt_factor = RT_FLINCH_DT_FACTOR;

// ⚠️ A HIT STAGGERS BY CHANCE, BY HOW HARD IT WAS (2026-10-10). His words: *"když vystřelím, tak se
// mutant zastaví a čeká… bylo by možné, že by chodil, stále chodil, když ho zasáhnu, a poté se třeba
// cuknul či necuknul? … fakt z toho udělat real time"*. Measured on trace_2026-10-10_155027: every
// hit that got through the DT gate staggered (39 of them), because super mutants and animals have NO
// Damage Threshold and DT 0 passes everything -- a mutant walking at you stopped for the 0.4 s flinch
// on every single bullet, while the dude in armour rarely did. He chose "brnění + šance" from three
// options: the DT gate stays the first sieve, and a hit through it staggers with chance
// RT_FLINCH_CHANCE_FACTOR x (damage / max HP). For everyone, the dude included. 12 damage into a
// 130-HP mutant ~23%, 25 damage ~48%, a rat nearly always. 0 = no roll (the old "always").
#define RT_FLINCH_CHANCE_FACTOR 2.5f
static float rt_flinch_chance_factor = RT_FLINCH_CHANCE_FACTOR;

// Exponent on SEQUENCE in the strike-first roll (see GAME_CONFIG_SEQ_INITIATIVE_POWER_KEY).
// Loaded from cfg at combat_rt_init.
#define RT_SEQ_INITIATIVE_POWER 2.0f
static float rt_seq_initiative_power = RT_SEQ_INITIATIVE_POWER;

// How long the SLOWEST possible critter waits before its opening swing, in seconds (cfg
// `initiative_span`). 0 disables initiative entirely and restores the old "everyone ready at
// once" opening. See rt_initiative_delay.
// Directional-lock aiming (see rt_directional_lock). The cone is the half-angle, as a cosine: it
// decides WHO IS ELIGIBLE when you push the stick. Inside it, the NEAREST one wins.
//
// ⚠️ IT USED TO BE THE OTHER WAY AROUND -- a single score, `dot - distance * 0.0015`, in which
// alignment could outweigh being close: an enemy 400 px away and dead-on scored 0.40 while one
// 100 px away and slightly off scored 0.25, so pushing left at the man beside you handed the lock
// to somebody across the room. "zamirim a chci vybrat toho vlevo, ale hra mi vybere uplne nejakeho
// z prdele." Two quantities in different units were being added up, which is the shape of the bug.
// ⚠️ A HALF-PLANE, NOT A NARROW CONE (0.35 -> 0.0). 0.35 was +-70 degrees, which sounds
// generous and is not: in an isometric view a critter standing BESIDE you is, on screen, almost
// exactly sideways, so pushing "down" at a dog two steps away scored 0.18 against a 0.35 gate and
// the push did nothing. Measured off his own screenshot: dude at (405,230), dog at (485,245).
// "proste zase nemuzu zamerit toho psa, i kdyz dam dolu joystick."
//
// At 0.0 the test becomes "is it on that side of me at all", and the NEAREST-first ranking below
// does the actual choosing -- which is the thing a player can predict: push roughly that way, get
// the closest one over there. The cone is a filter for the wrong half of the screen, nothing more.
#define RT_LOCK_AIM_CONE_COS 0.0f

// ⚠️ THE SCORE IS IN TILES MEASURED FROM THE TARGET YOU HAD, PLUS A CHARGE FOR BEING OFF THE PUSH.
// History, because this is the fourteenth shape and each earlier one was wrong for a reason:
//   * alignment alone (`dot - dist * 0.0015`, a cosine plus a pixel count) handed the lock to
//     whoever lined up best anywhere on the map: "chci vybrat toho vlevo, ale hra mi vybere uplne
//     nejakeho z prdele";
//   * distance alone, measured from the DUDE, made the nearest enemy on that side unbeatable, so one
//     further out could never be picked however precisely you pointed: "mirim ale proste nezamerim
//     to npc";
//   * degrees plus 2 deg/tile from the DUDE (2026-09-21, "hurraaaa") let a well-aligned enemy across
//     the room win -- and on 2026-10-04 that is exactly what he hit: locked on a Patient 5 tiles away
//     in the Cathedral, one push jumped to another Patient 11 tiles away. What he wanted, in his
//     words: "aby to zaměřilo toho nejblíž od toho posledního".
// So distance is measured from the ANCHOR (the target locked when the push began, see
// rt_directional_lock), nearest wins, and every RT_LOCK_STEP_DEG_PER_TILE degrees off the push count
// as one tile further away -- enough that a neighbour almost at right angles to your push loses to
// one nearly as close that is actually in that direction. The old "can't reach the far one" case is
// answered by stepping: each push starts from where the last one landed.
#define RT_LOCK_STEP_DEG_PER_TILE 30.0f

// How much better (in the same tiles) a rival must score before it takes the lock off the current
// target during a sweep. Pure hysteresis: without it two equal candidates trade the lock every frame.
#define RT_LOCK_STEP_SWITCH_MARGIN 0.5f

#define RT_INITIATIVE_SPAN 1.0f
static float rt_initiative_span = RT_INITIATIVE_SPAN;

// Seconds after a conversation closes during which NPCs may not open fire (cfg
// `post_dialog_grace`). A talk that ends in a fight -- Gizmo -- dumped the player straight from
// reading text into being shot at, with no beat to see what had changed.
//
// ⚠️ ONE-SIDED ON PURPOSE, and that is what makes it safe: only the NPCs are held. The player may
// shoot the instant the window closes, so nobody who wants to open fire immediately is stopped --
// which was the objection to a real freeze here ("zase někdo bude chtít i vystřelit").
#define RT_POST_DIALOG_GRACE 1.5f
static float rt_post_dialog_grace = RT_POST_DIALOG_GRACE;
static float rt_npc_hold_until = -1.0f;

// Does the dude win the strike-first roll against a foe swinging back at him this same frame?
// Weighted by SEQUENCE (Perception x 2), raised to rt_seq_initiative_power: the chance is
// dudeSeq^p / (dudeSeq^p + foeSeq^p). p = 1 is the flat weighting this started as; p = 2 makes a
// Perception edge actually felt without ever making it absolute -- a low-Sequence character still
// gets his share of first blows, which is what stops either side stagger-locking the other.
static bool rt_wins_initiative(int dudeSeq, int foeSeq)
{
    float dw = (float)(dudeSeq > 0 ? dudeSeq : 1);
    float fw = (float)(foeSeq > 0 ? foeSeq : 1);

    if (rt_seq_initiative_power > 0.0f) {
        dw = powf(dw, rt_seq_initiative_power);
        fw = powf(fw, rt_seq_initiative_power);
    } else {
        dw = fw = 1.0f; // initiative disabled -> coin flip
    }

    float total = dw + fw;
    if (total <= 0.0f) {
        return true;
    }

    // roll_random is inclusive on both ends, so scale over 0..9999 for a clean fraction.
    return (float)roll_random(0, 9999) < 10000.0f * (dw / total);
}

// Walk cost (Variant C2 kiting nerf): WALKING IN COMBAT also drains the shared AP pool (at a
// smaller rate than running), so moving spends the attack pool -- kiting dries up your offense.
// Loaded from config at combat_rt_init, tunable without a rebuild. Out-of-combat walking stays
// free (gated on rt_engaged via the dudeWalking check at the drain site).
//
// Walking no longer PAUSES regen; it switches the critter to the moving proximity curve instead
// (see RT_REGEN_NEAR_MULT_MOVING), which is what makes this a full burn in contact and a partly
// offset one at range.
#define RT_WALK_AP_COST 2.0f  // AP/sec drained while walking in combat (0 = free).
static float rt_walk_ap_cost = RT_WALK_AP_COST;

// PROXIMITY REGEN -- supersedes the old "stand still to regen faster" bonus (Variant C1).
//
// WHY IT CHANGED: tying the regen bonus to STANDING STILL quietly made the game a ranged
// game. A ranged attacker can stand and collect the bonus while shooting; a melee attacker
// must close the distance, so he was taxed TWICE -- he spent AP walking AND forfeited the
// bonus the whole way. Vanilla had the movement tax too, but a turn refilled the pool
// completely, so you could never fall permanently behind; realtime has no such reset.
//
// The fix keeps the original ANTI-KITING intent and drops the anti-melee side effect, by
// switching the axis from stance to DISTANCE: regen is fastest in contact and slowest far
// away. Kiting still costs you (you kite at range), but CLOSING no longer does -- a melee
// attacker actually charges his pool as he charges you. A ranged build fighting at 12+ tiles
// regenerates at rt_regen_far_mult, which is deliberately BELOW 1.0: the user asked for a
// pronounced ranged nerf here, so holding the far end is now an active cost, not just the
// absence of a bonus. REVISIT THIS NUMBER at the next balance pass.
//
// Symmetric: the dude measures to his nearest live hostile, an NPC to its current target.
// 2.0 made AP effectively free at point-blank: 0.21 s per point at AGI 8, so the pool refilled
// faster than the attack cooldown would let you spend it and the player read it as "AP reload
// instantly when anyone fires". 1.4 puts the brake back in contact range without touching ranged
// exchanges, which were not the complaint. (2026-08-13, user picked this lever.)
#define RT_REGEN_NEAR_MULT 1.4f   // multiplier in contact (dist <= 1)
#define RT_REGEN_FAR_MULT 0.6f    // multiplier at/beyond the falloff distance
#define RT_REGEN_FALLOFF_TILES 12 // tiles at which the multiplier reaches the far value
static float rt_regen_near_mult = RT_REGEN_NEAR_MULT;
static float rt_regen_far_mult = RT_REGEN_FAR_MULT;
static int rt_regen_falloff_tiles = RT_REGEN_FALLOFF_TILES;

// THE MOVING CURVE -- a SECOND proximity curve that applies while the critter is walking or
// running, replacing the old "floor the standing curve at 1.0 while moving" rule.
//
// WHY: the standing curve alone could not express what the fight actually needs. Its far end had
// to be floored while moving, or walking to a fight in the next room was punished exactly like
// sniping from it; but that floor also handed a moving critter the full contact BONUS, so at
// point-blank range you regenerated +0.86 AP/s while walking and nothing about closing distance
// ever competed with shooting. Vanilla's tension is "either you move or you attack", and it got
// that for free because a turn's AP paid for BOTH out of one pool.
//
// So: two curves, both linear over the same falloff, selected by whether the critter is moving.
//   standing: near 2.0  -> far 0.6   (hold ground at contact = fastest refill; snipe = penalised)
//   moving:   near 0.0  -> far 1.0   (moving INTO contact spends, approaching from range is free)
// The moving curve is monotonic the other way ON PURPOSE: crossing open ground toward a distant
// enemy costs you nothing extra, and the closer you get the more moving competes with attacking.
// ⚠️ WAS 0.0f UNTIL 2026-08-29, AND THAT ZERO IS WHAT MADE CAMPING THE DOMINANT STRATEGY.
// Standing in contact pays 1.4x -- the best regen in the game -- while MOVING in contact paid
// nothing at all against a full 2.0 AP/s walk cost. The economy was not suggesting you hold still,
// it was insisting: "vlastne je to docela lehcejsi, staci se zacampit". At 0.4 a repositioning
// player still loses ground (net about -1.25 AP/s, ~7 s of walking instead of ~4.5) but is no
// longer earning literally zero, and the stand:move ratio falls from infinity to about 3.5:1.
// Deliberately partial -- standing is still the better play, this only stops it being the ONLY one.
#define RT_REGEN_NEAR_MULT_MOVING 0.4f // multiplier in contact WHILE MOVING
// ⚠️ 0.7, NOT 1.0, SINCE 2026-10-08 (his pick: "pohyb v boji nikdy plně nedobíjí"). At 1.0 walking
// away from anything 12+ tiles off was all but free -- 2.0 AP/s walk cost against ~1.7 AP/s regen, so
// he walked off from three radscorpions for ever with the pool idling at 1-3 (trace_2026-10-08_142814,
// s=153-158). At 0.7 the same walk nets about -0.8 AP/s: ~7 s from a full pool, then the 0 AP stop,
// and a walking pursuer gains on him the way it would in vanilla. Kiting stays possible, not free.
// It also makes the long walk IN toward a distant enemy cost a little; the near end is untouched.
#define RT_REGEN_FAR_MULT_MOVING 0.7f  // multiplier at/beyond the falloff WHILE MOVING
static float rt_regen_near_mult_moving = RT_REGEN_NEAR_MULT_MOVING;
static float rt_regen_far_mult_moving = RT_REGEN_FAR_MULT_MOVING;

// THE NPC MOVING CURVE. Same shape as the dude's pair above, deliberately gentler, and it is what
// replaced the flat clamp-to-1.0 this file carried for one afternoon on 2026-08-29.
//
// The problem it fixes: an NPC in motion used to be handed a FLAT 1.0 -- which is the dude's BEST
// case (his far end) applied at every distance, including standing on top of him. Measured against
// walk_ap_cost 2.0, a Deathclaw (AGI 10, regen 2.00 AP/s) came out at EXACTLY zero net, so its pool
// never moved off 8-10 of 10 for a whole fight and the breather could not physically trigger
// (breath=0%, trace_2026-08-29_151004). That is the "enemy chodi za mnou jako zombici" report.
//
// So give them the same SHAPE the dude has -- cheap far away, expensive in contact -- just less
// steep: 0.5 where his is 0.0. Contact now runs about -1.0 AP/s for a fast critter, so it drains to
// its reserve in ~6 s and has to stop and breathe.
//
// ⚠️ THE FAR END MUST STAY 1.0. That is not a spare tuning knob, it is the guard on
// trace_2026-08-03_222723: a build that charged NPCs for the WHOLE approach pinned chasing melee
// critters at their reserve before they ever arrived. Only the near end may be tightened.
#define RT_NPC_REGEN_NEAR_MULT_MOVING 0.5f // multiplier in contact WHILE MOVING
#define RT_NPC_REGEN_FAR_MULT_MOVING 1.0f  // at/beyond the falloff -- approach is FREE, keep it so
static float rt_npc_regen_near_mult_moving = RT_NPC_REGEN_NEAR_MULT_MOVING;
static float rt_npc_regen_far_mult_moving = RT_NPC_REGEN_FAR_MULT_MOVING;

// Does the moving curve apply to NPCs too? DEFAULT OFF, and that asymmetry is evidence-based,
// not an oversight -- see the long note at the NPC regen site in rt_update_actor. A build that
// zeroed NPC regen while moving was killed by trace_2026-08-03_222723: chasing melee critters
// (id=1756, id=1323) sat pinned at their one-attack reserve for the whole chase because their
// walk drain ate everything they earned, and the ranged hostiles took 22 of 25 ATTACK samples.
// The dude escapes that trap only because rt_move_blocked STOPS him at 0 AP so he is forced to
// stand and recover; an NPC deliberately has no such stop (a chaser frozen mid-pursuit is the
// "enemy runs up and just looks at me" bug). Flip this to 1 in cfg to try full symmetry.
static bool rt_regen_moving_npc = false;

// Fractional AP-drain accumulators for running / walking (rt_player_ap is a whole-point pool).
static float rt_run_ap_drain = 0.0f;
static float rt_walk_ap_drain = 0.0f;

// COMBAT SPEED SLIDER -> AP TEMPO. The vanilla preferences screen's "Combat Speed" slider
// (cfg key combat_speed, 0..50) now drives the whole realtime AP economy: regen interval
// DIVIDED by the multiplier, walk/run drains MULTIPLIED by it. Because both sides scale
// together the balance between moving and shooting is untouched -- the same sprint/recover
// rhythm simply plays out faster in wall-clock time, and attacks come round sooner because
// the pool refills sooner. Applied to the dude AND every NPC (rt_regen_ap and
// rt_effective_run_cost are shared), so the slider is the tempo of the FIGHT, not a
// difficulty cheat.
//
// WHY THIS SLIDER: in vanilla it only adds fps to in-combat WALK anims, and only for NPCs
// (compute_tpf, anim.cc). In this build that does nothing -- walk_dir_slowdown normalises an
// NPC's tile-crossing time against the dude THROUGH compute_tpf, so it folds the very same
// bonus in and cancels it back out. The slider was dead UI; this gives it a real job.
//
// rt_speed_mult is re-sampled once per frame at the top of combat_rt_update (not cached at
// init) so moving the slider takes effect immediately, without a restart.
//
// ⚠️ 2026-10-11: THE SLIDER NOW SPEEDS UP THE WHOLE FIGHT, not just the AP economy. On a Deck at
// slider 50 with vanilla's "Affect player speed" ticked the dude ran about six times faster than
// everyone else ("dude běhá během combatu rychle"): vanilla adds the slider to the dude's walk fps,
// and only the NPCs' copy of that bonus was being cancelled by walk_dir_slowdown. His pick of the
// options offered: the slider speeds up EVERYTHING in a fight, for everyone alike, so faster never
// means unfair. It is now a time scale (rt_game_speed, applied in combat_rt_time_scale) -- the same
// mechanism the aimed-shot slow motion uses, run the other way: the combat clock (AP regen, drains,
// cooldowns, every timer) and the animation clock (compute_tpf: walking, running, swings,
// projectiles) both run faster by the same factor, so nothing goes out of step. rt_speed_mult is
// therefore 1.0 for good -- scaling the AP economy on top would count the slider twice -- and the
// vanilla walk-fps bonus is skipped in realtime (compute_tpf). Only while a fight is on: exploring
// is untouched. combat_speed_ap_max is the top speed (2.0 = twice as fast at slider 50).
// AIMED-SHOT SLOW MOTION. While the on-body limb picker is up (LB held), the world runs at this
// fraction of normal speed.
//
// ⚠️ SLOW, NOT STOPPED, AND THE DIFFERENCE IS THE DESIGN. Every RADIAL wheel -- stimpak, weapon,
// ammo -- freezes combat outright (see the early return in combat_rt_update): those are inventory
// decisions, and a pausable tactical game lets you make them with the world held still. Picking a
// limb is not that. It is an ATTACK being aimed, the shot is about to happen, and holding LB must
// not become a free tactical timeout you can open whenever the fight turns. So the clock keeps
// running -- just slowly enough that eight body parts is a real choice instead of a panic.
//
// It is also a nerf to what shipped: the freeze checks radial_wheel_is_open() only, and the limb
// picker is NOT a radial wheel (it is gpui_aim_body_on, drawn on the target itself), so until now
// aiming ran at FULL speed and you got mauled while choosing.
#define RT_AIM_SLOWMO 0.25f
static float rt_aim_slowmo = RT_AIM_SLOWMO;

#define RT_COMBAT_SPEED_AP_MAX 2.0f // AP tempo at slider 50 (1.0 = disable the coupling)
#define RT_COMBAT_SPEED_SLIDER_MAX 50.0f
static float rt_combat_speed_ap_max = RT_COMBAT_SPEED_AP_MAX;
static float rt_speed_mult = 1.0f; // stays 1.0 since the slider became a time scale (see above)
static float rt_game_speed = 1.0f;  // the Combat Speed slider as a time scale, sampled per frame

// LEAVING A MAP MID-FIGHT. In realtime you can walk onto a map-exit grid while something is still
// fighting you; map_leave_map arms this (combat_rt_arm_map_exit) and rt_update_map_exit tears the
// fight down so the queued transition can fire. ⚠️ IT IS IMMEDIATE (2026-10-06: "víš jak máme
// pravidlo 10 sekund zůstat na místě při odchodu, dej to pryč"), AND THAT IS VANILLA: in turn-based
// an exit grid reached in your turn ends the fight and changes the map at once (map_leave_map sets
// game_user_wants_to_quit, combat_input returns -1, combat_over, map_check_state). There used to be
// a 10 s "hold position" on the grid -- our own anti-abuse, so a losing fight could not be walked
// out of -- reset by any movement; vanilla never had one.
static bool rt_map_exit_armed = false;
// Set true once combat is torn down for the queued map transition. While set,
// combat_rt_update must NOT refresh the registry or re-arm the warm
// bit: the lingering hostiles would re-engage and re-set COMBAT_STATE_0x01 within the same
// frame, before map_check_state (gated on !isInCombat()) could fire -- stranding the player
// on the exit grid forever (the "waited 10s and nothing happened" bug). Cleared when the map
// actually unloads (combat_rt_notify_objects_freed) or combat is reset.
static bool rt_map_exit_committed = false;
// ⚠️ A TRANSITION THAT DID NOT COME FROM AN EXIT GRID (2026-10-08). Stairs, ladders and elevators in
// these maps are SCRIPTED: you use them and their script calls load_map / the elevator calls
// map_leave_map, with the dude standing next to them, not on a grid. The armed exit used to wait for
// "on an exit grid" before tearing the fight down, so a scripted one waited for the fight to end by
// itself -- *"zabil jsem konečně mastera a při útěku nemohu použít schody"*: the stairs out of the
// vault level, used at s=742.6 of trace_2026-10-08_215600 with Nightkin on his heels, fired only
// when the fight went cold 30 s later (the elevator one floor down had waited 2.4 s the same way).
// Vanilla leaves at once either way (map_leave_map -> game_user_wants_to_quit -> combat over), so a
// scripted exit goes on the next frame, grid or no grid.
static bool rt_map_exit_scripted = false;
static void rt_update_map_exit(float dt); // defined below combat_rt_update; called from it

// Hysteresis latch: set when the pool empties from running, so the dude WALKS (free)
// instead of stutter-flickering run/walk at the low end. Cleared once AP regenerates
// back to rt_run_resume_ap -- giving the sprint / walk-recover / sprint burst rhythm.
static bool rt_run_blocked = false;

// Movement stop-latch: when the AP pool empties from walking OR running IN COMBAT, ALL
// player movement stops (the dude stands still) until AP regenerates back to
// rt_run_resume_ap. No deadlock: a stopped dude is STANDING, and regen only pauses while
// RUNNING, so the pool always climbs back while stopped (and faster via the C1 standing
// bonus). Distinct from rt_run_blocked, which only gates the run GAIT; this gates motion
// entirely. Combat-only (the pool sits full out of combat, so it never latches there).
static bool rt_move_blocked = false;

// cfg `ap_empty_walks`: what rt_move_blocked MEANS. false (default) = the shipped behaviour, an
// empty pool stops the dude dead. true = it drops him to a free walk instead, so a fight always
// has an exit. See the header note on the key, and the deadlock guard at the regen call.
static bool rt_ap_empty_walks = false;

// Last AP value pushed to the interface bar's AP indicator, so the bulbs are
// redrawn only when a point is actually spent or regenerated (-1 = never drawn).
static int rt_last_hud_ap = -1;

// Same, for the movement-blocked latch that turns those bulbs red. It is a SECOND redraw
// trigger because the latch flips without the AP count changing -- it is set the instant a
// walk drains the pool to 0 (already 0, so no AP change) and drops the instant the fight
// ends -- and either edge would otherwise leave the bar the wrong colour until the next
// regen tick. -1 = never drawn.
static int rt_last_hud_blocked = -1;

// Same, for the Bonus Move reserve, which the bar draws as yellow bulbs after the green ones.
// -1 = never drawn.
static int rt_last_hud_bonus = -1;

// Same, for the ARMOR CLASS readout. Vanilla only refreshes it on intface_redraw/show or on
// leaving the inventory, which was fine while the AC bonus was a per-turn constant. Now that the
// bonus rides on the REAL realtime pool (see combat_rt_critter_ap / stat_level) it moves with
// every attack and every regen tick, so the number on the bar would be stale nearly all the time.
// Pushed from here on change only -- same rule as the AP bulbs above. -1 = never drawn.
static int rt_last_hud_ac = -1;

// Follow-cam: the dude is pinned to screen center by centering on his tile and
// shifting the view by the negative of his sub-tile walk offset (obj->x/y), so
// the world scrolls under him pixel-for-pixel with no per-tile jump. These
// remember the last applied state so a full redraw is forced only when
// something actually moved (idle/attacking -> no redraw). See rt_follow_cam.
static int rt_cam_last_tile = -1;
static int rt_cam_last_off_x = 0;
static int rt_cam_last_off_y = 0;

// SMOOTHED FOLLOW-CAM. rt_cam_lag_x/y is how far the dude is currently allowed to sit AHEAD of the
// camera centre, in pixels -- the camera's error, decaying toward 0 whenever he stops moving. The
// two prev_ fields are last frame's sub-tile offset, so the per-frame motion can be measured.
//
// Exponential smoothing with the time constant below: alpha = 1 - exp(-k*dt). NOT a fixed
// per-frame factor -- this game's frame rate is not fixed, and a per-frame lerp silently changes
// its own strength with the frame rate.
#define RT_CAM_SMOOTH_K 10.0f   // ~100 ms time constant; 0 disables (hard lock, the old behaviour)
#define RT_CAM_SMOOTH_MAX 24.0f // px the dude may drift off centre -- 3/4 of a tile
#define RT_CAM_SMOOTH_SNAP 1.0f // under a pixel of error, settle exactly (see the redraw note)
static float rt_cam_smooth_k = RT_CAM_SMOOTH_K;
static float rt_cam_lag_x = 0.0f;
static float rt_cam_lag_y = 0.0f;
static int rt_cam_prev_x = 0;
static int rt_cam_prev_y = 0;
static bool rt_cam_have_prev = false;

// LEANING TOWARD AN AIMED TARGET OFF THE SCREEN (2026-10-09): *"když zamířím někoho mimo hrací pov,
// ... by se lehce šoupnul pov k tomu enemy, ke kterému mířím? fakt lehce"*. The lock reaches by
// Perception, which is further than the screen, so you could hold an enemy you could not see.
// While a lock is up and its target sits off the view (or within RT_CAM_LEAN_MARGIN of an edge),
// the camera eases toward it just far enough to bring it in -- capped at RT_CAM_LEAN_FRACTION of
// the half-view, so the dude never leaves the middle of the screen -- and eases back when the lock
// drops or the target comes into view. Measured from the DUDE, not from the leaned camera, so the
// lean cannot chase itself. MODS "Camera leans to target" (cfg cam_lock_lean), default OFF.
#define RT_CAM_LEAN_MARGIN 40.0f
#define RT_CAM_LEAN_FRACTION 0.25f
#define RT_CAM_LEAN_K 3.0f // ~1/3 s in and out: a lean, not a cut
static bool rt_cam_lean_on = false;
static float rt_cam_lean_x = 0.0f;
static float rt_cam_lean_y = 0.0f;

// How far the view must lean (px, capped) for `t` to be on screen, and whether that cap is enough
// to show it at all. False = no lean applies (off, no target, other elevation, no view).
static bool rt_cam_lean_want(Object* t, float* wantX, float* wantY, bool* fits)
{
    *wantX = 0.0f;
    *wantY = 0.0f;
    *fits = false;

    if (!rt_cam_lean_on || t == NULL || obj_dude == NULL || display_win == -1
        || t->tile == -1 || t->elevation != obj_dude->elevation) {
        return false;
    }

    int tx;
    int ty;
    int dx;
    int dy;
    if (tile_coord(t->tile, &tx, &ty, obj_dude->elevation) != 0
        || tile_coord(obj_dude->tile, &dx, &dy, obj_dude->elevation) != 0) {
        return false;
    }

    float vx = (float)((tx + t->x) - (dx + obj_dude->x));
    float vy = (float)((ty + t->y) - (dy + obj_dude->y));
    float halfW = win_width(display_win) * 0.5f;
    float halfH = win_height(display_win) * 0.5f;
    float edgeX = halfW - RT_CAM_LEAN_MARGIN;
    float edgeY = halfH - RT_CAM_LEAN_MARGIN;

    float needX = vx > edgeX ? vx - edgeX : (vx < -edgeX ? vx + edgeX : 0.0f);
    float needY = vy > edgeY ? vy - edgeY : (vy < -edgeY ? vy + edgeY : 0.0f);
    float capX = halfW * RT_CAM_LEAN_FRACTION;
    float capY = halfH * RT_CAM_LEAN_FRACTION;

    *fits = fabsf(needX) <= capX && fabsf(needY) <= capY;
    *wantX = needX > capX ? capX : (needX < -capX ? -capX : needX);
    *wantY = needY > capY ? capY : (needY < -capY ? -capY : needY);
    return true;
}

// Gamepad (SDL_GameController) for testing. Left stick = movement, right trigger
// / A = attack, B = cancel.
#define RT_GAMEPAD_AXIS_MAX 32767.0f
#define RT_GAMEPAD_DEADZONE 0.3f
#define RT_GAMEPAD_TRIGGER_THRESHOLD 16000 // ~0.5 of the 0..32767 trigger range.
// Right-stick horizontal flick to cycle the lock-on target: push past THRESHOLD
// to step, then return inside RECENTER to re-arm (one flick = one target).
#define RT_LOCK_CYCLE_THRESHOLD 0.5f
#define RT_LOCK_CYCLE_RECENTER 0.3f
// A held lock drops once the target gets this far (tiles) -- it fled the area.
#define RT_LOCK_DROP_DIST 40
// ...and, long before that, once it is simply no longer ON SCREEN. 40 tiles is far wider than the
// view, so distance alone let the lock survive on a critter the player could not see: the crosshair
// stayed glued to something off the edge of the world window and the aim UI kept reporting on it.
// Held briefly rather than instantly so a target hovering on the screen edge, or one frame of view
// scrolling, does not flicker the lock off and on.
#define RT_LOCK_OFFSCREEN_SECONDS 0.4f
// When the locked target dies, the lock auto-hops to the nearest enemy within THIS many tiles
// (a "nearby" radius, smaller than the drop distance) so it only re-targets close threats, not a
// straggler across the map.
#define RT_LOCK_HOP_DIST 12
// Fine-tune knob (degrees) added to the raw stick angle before mapping to a hex
// direction. The screen<->hex alignment itself lives in sectorToRotation; this
// just lets you nudge the feel. 0 = aligned.
#define RT_GAMEPAD_ANGLE_OFFSET 0.0f

// DOORWAY MAGNET. Six hex directions vs a 360-degree stick: aim one sector wrong and you slide
// along the wall instead of through the gap. Doorways are the one place that really hurts, because
// the opening is a SINGLE tile and there is no second chance -- you scrape past and have to back up.
//
// The assist bends the stick ANGLE toward a nearby doorway before it is bucketed into a sector. It
// never moves the dude and never picks a destination: if the bent angle still lands in the sector
// the player asked for, nothing at all changes. All it does is decide which side of a sector
// boundary a near-miss falls on -- which is exactly the failure.
#define RT_DOOR_MAGNET_TILES 4    // beyond this a door does not pull at all
#define RT_DOOR_MAGNET_CONE 45.0f // and only within this many degrees of where the stick points
#define RT_DOOR_MAGNET_MAX 4      // nearby doorways tracked at once

// Strength, 0..1, from cfg door_magnet. 0 = off, 1 = the stick snaps straight at the doorway.
static float rt_door_magnet = 0.7f;

// Crowd assist, from cfg npc_slide. See rt_bystander_blocks / the slide block in
// combat_rt_poll_gamepad.
static bool rt_npc_slide = true;

// Idle hostiles look around, from cfg enemies_look_around (MODS "Enemies look around", default off
// = vanilla). See rt_idle_look_around.
static bool rt_look_around = false;

// Scripts get their combat turns, from cfg script_turns (MODS "Script combat turns"). Default ON,
// because ON is what vanilla does -- see rt_script_turn_tick.
static bool rt_script_turns = true;

// The last GUNSHOT from the player's side (the dude or a follower): when, where, on which floor.
// What an idle hostile within earshot turns toward. Tile, not the shooter's pointer -- a noise is a
// place, and a stored Object* would be one more thing to invalidate on every teardown.
static float rt_noise_clock = -1000.0f;
static int rt_noise_tile = -1;
static int rt_noise_elev = -1;

// BODIES IN THE MIDDLE OF A RELOAD (MODS "Reload animation") -- the dude's (gamepad_ui) and every
// NPC's (rt_npc_start_reload_anim). What lets a hit land WITHOUT cutting the reload ("střelba od
// enemyho zruší reload? I dunno about that"), lets a second press be ignored instead of restarting
// the loop, and tells the dude's side that a reload ended without finishing (refund).
//
// A body counts only while it is STILL in its STAND loop: anything that replaced the loop (a
// knockdown, a walk, an attack) ended the reload, and the entry is dropped the first time anyone
// asks. Pointers are cleared on every teardown (rt_abandon_encounter, notify_critter_removed).
#define RT_RELOADING_MAX 16
static Object* rt_reloading[RT_RELOADING_MAX];
static int rt_reloading_count = 0;

static void rt_reloading_drop(Object* critter)
{
    for (int i = 0; i < rt_reloading_count; i++) {
        if (rt_reloading[i] == critter) {
            rt_reloading[i] = rt_reloading[--rt_reloading_count];
            return;
        }
    }
}

void combat_rt_reload_begin(Object* critter)
{
    if (critter == NULL) {
        return;
    }
    rt_reloading_drop(critter);
    if (rt_reloading_count < RT_RELOADING_MAX) {
        rt_reloading[rt_reloading_count++] = critter;
    }
}

void combat_rt_reload_end(Object* critter)
{
    rt_reloading_drop(critter);
}

bool combat_rt_is_reloading(Object* critter)
{
    if (critter == NULL) {
        return false;
    }
    for (int i = 0; i < rt_reloading_count; i++) {
        if (rt_reloading[i] == critter) {
            // STAND, or a pistol's put-away / take-out (register_object_reload).
            if (anim_is_reload_pose(critter)) {
                return true;
            }
            rt_reloading_drop(critter);
            return false;
        }
    }
    return false;
}


// Doorway tiles near the dude. Rebuilt only when he changes tile -- the set cannot change while he
// stands still, and obj_create_list over every scenery object is not something to run per frame.
static int rt_door_tiles[RT_DOOR_MAGNET_MAX];
static int rt_door_count = 0;
static int rt_door_cache_tile = -1;
static int rt_door_cache_elev = -1;

// Stick movement registers walk paths of this many tiles and seamlessly chains
// the next segment one tile before the current one runs out, so holding the
// stick walks continuously (like mouse movement) instead of step-pause-step.
// Kept SHORT (2, not 4) so a brief stick nudge commits only a step or two and the
// dude STOPS promptly on release instead of coasting several tiles on -- the "I let
// go but the character keeps walking by itself after a shot" report. Holding still
// stays smooth: rt_stick_move appends another 2-tile chunk to the live animation one
// tile before it runs out, so the walk/run frame cycle never resets.
#define RT_STICK_SEGMENT_TILES 2

// In-flight stick-walk segment. rt_stick_active is true only while an
// animation WE registered is playing, which is what makes it safe to
// register_clear(obj_dude): the dude's other animations (attacks) must never
// be cancelled. rt_stick_short marks a 1-tile fallback segment (blocked far
// tile) that must finish naturally instead of being extended. rt_stick_running
// remembers the segment's gait (LT held) so a gait change forces a restart.
static bool rt_stick_active = false;
// rt_clock when the player started continuously asking to move and being refused; -1 = not
// currently refused. Drives the RT_MOVE_STUCK_SECONDS backstop.
static float rt_move_stuck_since = -1.0f;
static bool rt_stick_short = false;
static bool rt_stick_running = false;
static int rt_stick_rotation = -1;
static int rt_stick_dest = -1;

// Last stick sector, for hysteresis at sector boundaries (-1 = stick idle).
static int rt_stick_last_sector = -1;

// The six hex directions in CCW screen order, with the angle each one actually walks at. Derived
// from the engine's own off_tile rather than hardcoded -- that table IS where a tile step lands.
//
// ⚠️ THEY ARE NOT 60 DEGREES APART: E 0, NE 36.9, NW 143.1, W 180, SW 216.9, SE 323.1. The
// isometric squash pulls every diagonal toward the horizontal, so one gap is 36.9 degrees wide and
// the next is 106. Anything that picks "the nearest facing" has to use these numbers and not a
// sector index, or it is wrong by up to 53 degrees on the vertical.
static const int rt_hex_ccw[6] = { ROTATION_E, ROTATION_NE, ROTATION_NW, ROTATION_W, ROTATION_SW, ROTATION_SE };
static float rt_hex_angle[6];
static bool rt_hex_ready = false;

static void rt_hex_table_init()
{
    if (rt_hex_ready) {
        return;
    }

    for (int i = 0; i < 6; i++) {
        int r = rt_hex_ccw[i];
        // off_tile is in SCREEN pixels with y growing DOWN; the angle is math space (y up).
        float a = atan2f(-(float)off_tile[1][r], (float)off_tile[0][r]) * 57.2957795f;
        if (a < 0.0f) {
            a += 360.0f;
        }
        rt_hex_angle[i] = a;
    }

    rt_hex_ready = true;
}

// Right-stick lock-on target (gamepad aiming). When set, the A button attacks
// this critter instead of the nearest one; NULL = auto-target nearest. Acquired
// by sweeping the aim cursor over a hostile, cleared by R3 or when the target
// dies / leaves. Validated against the actor registry before any dereference so
// a freed object can never be touched.
static Object* rt_lock_target = NULL;

// The target that was locked when the current right-stick PUSH began. Target switching measures from
// it (rt_directional_lock) and it stays put until the stick comes back home, so one push is one step
// away from where you were. NULL while the stick is home or nothing is locked. Cleared with the lock
// in every teardown path, and when its critter is freed.
static Object* rt_lock_push_anchor = NULL;

// The critter the lock was last on, kept after the lock lets go so the NEXT aim starts there: "ať si
// hra pamatuje, kdo byl naposledy zamířen, když dám zase aiming" (2026-10-04). Refreshed every frame
// a lock exists; forgotten with the encounter and when its critter is freed.
static Object* rt_lock_last_target = NULL;

// CONCURRENT ATTACK RESOLUTION. combat_attack() computes one attack into the single global
// main_ctd; the classic flow holds that global until the damage commits, which serializes
// attacks to one-at-a-time (a turn-based feel under realtime). Instead, the instant combat_attack
// succeeds we SNAPSHOT main_ctd into a private RtInFlight record and free the global, so many
// attacks can be in flight at once -- each commits from its own copy at swing-connect / the cap.
// The only inter-attack rule kept is ONE attack per DEFENDER at a time (rt_defender_in_flight),
// which preserves the original "a critter can't be asked to react twice at once" invariant (the
// anim-sequence / death-crash protection) while letting DIFFERENT attacker->defender pairs run
// in parallel -- so the dude strikes an enemy at the same instant an enemy strikes him.
struct RtInFlight {
    Attack ctd;          // private copy of the computed attack (POD; safe to memcpy/assign)
    Object* attacker;    // == ctd.attacker, cached for liveness/identity checks
    Object* defender;    // == ctd.defender
    bool callDisplay;    // snapshot of combat_call_display for the combat-log line
    float started;       // rt_clock when the attack was issued, for the RT_RESOLVE_MAX_SECONDS cap
    int playerApCost;    // >0 only for the DUDE's attack: AP to charge at commit, not at fire (so
                         // the pool drops when the swing lands ~half-anim in, not on button press)

    // Edges of the attacker's swing pose, in seconds since started (-1 = not reached yet).
    // Load-bearing for THROWN weapons: their commit waits for the firing pose to END (poseEnded in
    // the resolve loop), and tAnimStart must have been seen first, or a throw whose animation never
    // registered would read as "pose already over" on frame one.
    float tAnimStart;    // the attacker's fid first became a real ATTACK pose
    float tAnimEnd;      // it left the attack pose (recoil/UNPOINT, or the projectile's flight)

    // An EXPLOSIVE projectile (rocket, grenade) this attack put in the air, and the explosion art it
    // turns into on arrival (action_take_explosive_projectile). Such an attack commits when the blast
    // goes off, not when the trigger is pulled -- see `landed` in the resolve loop. NULLed by the
    // erase hook if the projectile is freed first; explosionFid stays, so that still reads as landed.
    Object* projectile;
    int explosionFid;    // -1 = not an explosive attack
};
#define RT_MAX_INFLIGHT (RT_MAX_ACTORS + 2)
static RtInFlight rt_inflight[RT_MAX_INFLIGHT];
static int rt_inflight_count = 0;

// True if some in-flight attack already targets `d` (enforces one-reaction-per-defender).
static bool rt_defender_in_flight(Object* d)
{
    for (int i = 0; i < rt_inflight_count; i++) {
        if (rt_inflight[i].defender == d) {
            return true;
        }
    }
    return false;
}

// True if `a` already has an attack of its own in flight (not yet committed). Used to stop the
// player from firing a SECOND attack in the window where his swing anim has ended but the commit
// is still held by RT_MIN_SWING_SECONDS -- anim_busy alone no longer covers that gap, and firing
// again there would double-spend AP (the pending charge commits later) and stack shadow attacks.
static bool rt_attacker_in_flight(Object* a)
{
    for (int i = 0; i < rt_inflight_count; i++) {
        if (rt_inflight[i].attacker == a) {
            return true;
        }
    }
    return false;
}

// Damage already in flight against `defender` (see the header). combat.cc's check_for_death calls
// this at FIRE time so an attack's lethality is decided against the HP the defender will actually
// have when the blow lands, not the HP it has while several earlier attacks are still animating.
int combat_rt_pending_damage_on(Object* defender, bool* killClaimed)
{
    if (killClaimed != NULL) {
        *killClaimed = false;
    }

    if (!rt_enabled || defender == NULL) {
        return 0;
    }

    int total = 0;
    for (int i = 0; i < rt_inflight_count; i++) {
        if (rt_inflight[i].defender != defender) {
            continue;
        }

        int dmg = rt_inflight[i].ctd.defenderDamage;
        if (dmg > 0) {
            total += dmg;
        }

        if (killClaimed != NULL && (rt_inflight[i].ctd.defenderFlags & DAM_DEAD) != 0) {
            *killClaimed = true;
        }
    }

    return total;
}

// Drop any in-flight attack referencing `o` (its attacker or defender) -- called when an object
// is freed so a snapshot never commits against dangling memory.
static void rt_inflight_drop_obj(Object* o)
{
    for (int i = 0; i < rt_inflight_count;) {
        if (rt_inflight[i].attacker == o || rt_inflight[i].defender == o) {
            rt_inflight[i] = rt_inflight[--rt_inflight_count];
        } else {
            i++;
        }
    }
}

// NOTE: the dude's flinch timer and his knockdown clocks used to live here, as a hand-kept copy of
// the per-actor pair. Both copies are gone -- the reaction layer tracks him like any other body.

// Seconds since the last movement input. Reset to 0 each time the dude is fed a
// direction (rt_player_move / rt_stick_move); incremented by dt each update. Under
// RT_MOVE_INPUT_GRACE means "moving" -- see combat_rt_dude_moving(). Starts large
// so the dude isn't considered moving before any input.
static float rt_move_input_age = 1000.0f;

// Latched player attack intent. Input sets this (instead of firing immediately)
// so a press/click survives flinch and locked frames; the arbiter fires it at the
// next eligible moment. ttl decays each frame (a held button refreshes it).
static Object* rt_player_intent_target = NULL;
static int rt_player_intent_mode = 0;
static float rt_player_intent_ttl = 0.0f;
// The called-shot body part for the latched intent, or HIT_LOCATION_UNCALLED for a
// normal (non-aimed) attack. Set by the LT aimed-shot picker (and its RB repeat) via combat_rt_aimed_attack.
static int rt_player_intent_loc = HIT_LOCATION_UNCALLED;

// Pending world-loot target (tap-A on an adjacent corpse/container), set from the
// input hook via combat_rt_try_world_loot() and opened in combat_rt_update -- the
// loot window runs its own modal loop, so it must NOT open from inside the hook.
static Object* rt_pending_loot = NULL;
static int rt_pending_loot_kind = 0; // 1 = corpse, 2 = container item

// --- Stuck-lock watchdog ---------------------------------------------------
// The attack lock (rt_attack_locked) can wedge permanently ON if an animation
// sequence is leaked/orphaned: the engine's combat_cleanup_enabled never clears
// (apply_damage runs only when combat_turn_running drains to 0, which a
// never-ending sequence prevents) AND the 21-slot anim table saturates, so
// nothing -- not even movement -- can register. The game looks frozen. This
// watchdog measures how long the lock has been continuously held and, past
// RT_WATCHDOG_SECONDS, logs a diagnostic snapshot and force-recovers (flushes the
// anim table + clears the stuck pending flag) so the game keeps running. It is a
// recovery valve only and does NOT touch normal serialization (the main_ctd
// death-crash fix is preserved). The log line is meant to pin the orphaned
// sequence's first firing: held time, pending/resolving/anim state, actor/hostile
// counts, and the last death/gib/knockdown seen before the lock stuck.
#define RT_WATCHDOG_SECONDS 2.5f
static float rt_lock_held = 0.0f;

// Monotonic seconds since combat_rt_init, for event timestamps.
static float rt_clock = 0.0f;

// Critters whose death reaction was registered in the last instant. A fall registered at the
// commit point has NOT started yet, so critter_is_prone() is still false for it -- and every
// safety net in here that uses "not prone but animating" as its test for a STRAY animation would
// otherwise cut the very fall that was just queued. Short-lived, tiny, ring-buffered.
#define RT_DEATH_GRACE_SLOTS 8
#define RT_DEATH_GRACE_SECONDS 0.5f
static Object* rt_death_grace_obj[RT_DEATH_GRACE_SLOTS];
static float rt_death_grace_time[RT_DEATH_GRACE_SLOTS];
static int rt_death_grace_next = 0;

static void rt_mark_death_registered(Object* obj)
{
    if (obj == NULL) {
        return;
    }
    rt_death_grace_obj[rt_death_grace_next] = obj;
    rt_death_grace_time[rt_death_grace_next] = rt_clock;
    rt_death_grace_next = (rt_death_grace_next + 1) % RT_DEATH_GRACE_SLOTS;
}

static bool rt_death_just_registered(Object* obj)
{
    if (obj == NULL) {
        return false;
    }
    for (int i = 0; i < RT_DEATH_GRACE_SLOTS; i++) {
        if (rt_death_grace_obj[i] == obj && rt_clock - rt_death_grace_time[i] < RT_DEATH_GRACE_SECONDS) {
            return true;
        }
    }
    return false;
}

// Most recent lethal/disabling result transition seen on any combatant (the dude
// included), so the watchdog can name WHAT just happened right before the lock
// wedged -- the prime suspect for the orphaned sequence (death vs gib vs
// knockdown). rt_dude_last_results tracks the dude (not in rt_actors).
static char rt_last_event[48] = "none";
static int rt_last_event_pid = -1;
static float rt_last_event_time = -1000.0f;
static int rt_dude_last_results = 0;

// Records the most-recent significant combat.results transition (highest-severity
// bit newly set this frame) so the watchdog log can name what happened just before
// the lock wedged. Death/gib/knockdown are the suspected orphaned-sequence
// triggers, so they are distinguished explicitly.
static void rt_note_result_change(Object* obj, int prev, int cur)
{
    int gained = cur & ~prev;
    if (gained == 0) {
        return;
    }

    // CRIPPLES AND BLINDNESS, traced the instant they land. They are NOT part of the rt_last_event
    // digest below -- that names the one headline thing that happened to a critter, and a lost limb
    // must not shadow the death that follows it. This is its own line for its own question.
    //
    // ⚠️ WITHOUT THIS THE WHOLE CRIPPLE SYSTEM IS INVISIBLE. The player asked the obvious question
    // after shooting a Deathclaw's leg -- did it actually walk slower? -- and there was no way to
    // answer it: STATE/critter carries no cripple bits, and rt_walk.log is emitted inside
    // walk_dir_slowdown, which runs BEFORE the limp multiplier is applied at the call site. Two
    // separate blind spots over the same mechanic. This closes the first; the crip= column added to
    // rt_walk.log closes the second.
    {
        int crip = gained & (DAM_CRIP_LEG_LEFT | DAM_CRIP_LEG_RIGHT | DAM_CRIP_ARM_LEFT | DAM_CRIP_ARM_RIGHT | DAM_BLIND);
        if (crip != 0 && obj != NULL) {
            int nowFlags = cur;
            int legs = (((nowFlags & DAM_CRIP_LEG_LEFT) != 0) ? 1 : 0) + (((nowFlags & DAM_CRIP_LEG_RIGHT) != 0) ? 1 : 0);
            // prev/now in full: a cripple that arrives with NO hit (the dude's blindness,
            // 2026-10-08) is either a script (script-injure), a load, or a corrupted word -- and
            // only the whole field tells those apart.
            trace_event(TRACE_COMBAT, "cripple",
                "pid=%d id=%d dude=%d legL=%d legR=%d armL=%d armR=%d blind=%d legsNow=%d prev=0x%x now=0x%x t=%.2f",
                obj->pid, obj->id, obj == obj_dude ? 1 : 0,
                (crip & DAM_CRIP_LEG_LEFT) != 0 ? 1 : 0, (crip & DAM_CRIP_LEG_RIGHT) != 0 ? 1 : 0,
                (crip & DAM_CRIP_ARM_LEFT) != 0 ? 1 : 0, (crip & DAM_CRIP_ARM_RIGHT) != 0 ? 1 : 0,
                (crip & DAM_BLIND) != 0 ? 1 : 0, legs, prev, cur, rt_clock);
        }
    }

    const char* what = NULL;
    if ((gained & DAM_DESTROY) != 0) {
        what = "GIB";
    } else if ((gained & DAM_DEAD) != 0) {
        what = "DEATH";
    } else if ((gained & DAM_EXPLODE) != 0) {
        what = "EXPLODE";
    } else if ((gained & DAM_ON_FIRE) != 0) {
        what = "FIRE";
    } else if ((gained & DAM_KNOCKED_OUT) != 0) {
        what = "KNOCKOUT";
    } else if ((gained & DAM_KNOCKED_DOWN) != 0) {
        what = "KNOCKDOWN";
    }

    if (what == NULL) {
        return;
    }

    snprintf(rt_last_event, sizeof(rt_last_event), "%s", what);
    rt_last_event_pid = obj != NULL ? obj->pid : -1;
    rt_last_event_time = rt_clock;
}

// 0x000000
static bool rt_is_critter(Object* obj)
{
    return obj != NULL && PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER;
}

// A living critter that can still act/be targeted.
static bool rt_is_combatant(Object* obj)
{
    if (!rt_is_critter(obj)) {
        return false;
    }

    if ((obj->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
        return false;
    }

    return stat_level(obj, STAT_CURRENT_HIT_POINTS) > 0;
}

// Like rt_is_combatant but ALSO includes a KNOCKED-OUT (unconscious) critter. rt_is_combatant
// excludes the knocked-out so the AI never acts while unconscious -- but the PLAYER must still be
// able to lock onto and finish a downed enemy before it comes to (the "couldn't shoot Gizmo while
// he was knocked out" bug). Used only on the player's lock / aim / attack-target paths; a dead
// critter is still never targetable.
static bool rt_is_targetable(Object* obj)
{
    if (!rt_is_critter(obj)) {
        return false;
    }
    if ((obj->data.critter.combat.results & DAM_DEAD) != 0) {
        return false;
    }
    return stat_level(obj, STAT_CURRENT_HIT_POINTS) > 0;
}

// Aggro radius for a critter, in tiles. Used both to start a chase (RT_IDLE) and,
// widened by RT_DEAGGRO_MARGIN, to decide when a chase gives up.
static int rt_aggro_range(Object* npc)
{
    int range = stat_level(npc, STAT_PERCEPTION) * RT_AGGRO_PERCEPTION_SCALE;
    return range < RT_AGGRO_MIN_TILES ? RT_AGGRO_MIN_TILES : range;
}

// Pursuit range for a hostile critter: at least its aggro radius, but never less than the
// generous chase leash so a ranged kiter is actually followed. (See RT_CHASE_LEASH_TILES.)
static int rt_chase_range(Object* npc)
{
    int aggro = rt_aggro_range(npc);
    return aggro > RT_CHASE_LEASH_TILES ? aggro : RT_CHASE_LEASH_TILES;
}

// VANILLA'S COMBAT-END RULE, ported to the realtime FSM.
//
// Fallout does NOT end a fight on distance -- it ends it on PERCEPTION. The manual path,
// combat_end (combat.cc), only goes through when every enemy passes combatai_want_to_stop
// (combatai.cc), and that is true when the critter is disengaging, fleeing, downed, has no
// ai_danger_source at all, or can no longer PERCEIVE the danger it does have. The automatic
// path, combat_should_end, is purely a team head-count with no distance term either. Our old
// leash (obj_dist > RT_CHASE_LEASH_TILES + margin = 53 tiles) was therefore not a port of
// anything, and on any normal map it never fired: a hostile pursued forever, rt_engaged never
// went false, and combat stayed permanently "warm" -- which is what the player was feeling as
// "the fight never cools down".
//
// We call the two cheap pieces (ai_danger_source + is_within_perception) DIRECTLY and never
// combatai_want_to_stop itself: that function opens with process_bk(), which pumps the whole
// game loop, and must not be re-entered from inside combat_rt_update.
//
// Vanilla's last clause -- "ap == max action points" -- is deliberately NOT ported. It is a
// turn-based artifact meaning "this critter spent nothing on its turn"; in realtime a full pool
// is the normal resting state, so porting it would end essentially every fight on sight.
//
// Perception radius, for reference (is_within_perception): PE*5 tiles with line of sight (halved
// through glass, quartered vs. a sneaking player), PE*2 without it while combat is warm, PE*1
// once it has gone cold. So a PE 5 guard holds you at 25 tiles in the open and 10 through a wall.
static bool rt_wants_to_stop(Object* npc, Object* target)
{
    int maneuver = npc->data.critter.combat.maneuver;
    if ((maneuver & (CRITTER_MANEUVER_DISENGAGING | CRITTER_MANUEVER_FLEEING)) != 0) {
        return true;
    }

    // ⚠️ HURT SINCE ITS LAST TURN, IT KEEPS COMING (2026-10-06, his call: "ano, jako vanilla").
    // Vanilla's ai_danger_source returns whoHitMe with NO perception test -- a critter you shoot goes
    // after you whether it can see you or not; only combatai_want_to_stop ("may the whole COMBAT
    // end?") asks about perception, and that is the question this function borrowed. So a nearly
    // blind animal dropped the chase while being shot: trace_2026-10-06_003930, a radscorpion (PE 2,
    // 10 tiles at best) "disengaged" five times in 35 s at 11-14 tiles, standing a second each time,
    // and kiting it cost nothing ("mohl jsem je fakt lehce kiteovat"). Now a hit since its last turn
    // (damageLastTurn -- cleared at each of its turns, see rt_script_turn_tick) keeps it on you; a
    // turn without one, out of sight, and it lets go -- so fights still cool down.
    Object* hurtBy = npc->data.critter.combat.whoHitMe;
    if (npc->data.critter.combat.damageLastTurn > 0 && hurtBy != NULL && rt_is_combatant(hurtBy)) {
        return false;
    }

    // Still perceives the thing it is actually fighting -> stay on it. Checked before
    // ai_danger_source because our target and vanilla's "danger" need not be the same object
    // (we let a pack split across the dude and his followers).
    //
    // ⚠️ WITHOUT THE PLAYER'S SNEAK (2026-10-09): "když dám sneak, tak i skončil combat" -- Sneak
    // quarters a critter's perception of him, so switching it on mid-fight dropped every enemy
    // more than a few tiles away within seconds (trace_2026-10-09_160503: L3 at s=46.6, the Master
    // and both robots disengaged at 50-51, the fight was over at 55.6), and the mutants simply
    // stopped. Sneak keeps you from being FOUND; a critter already fighting you keeps you at its
    // full perception and loses you by distance and walls, as anyone would.
    if (rt_is_combatant(target) && is_within_perception_unsneaked(npc, target)) {
        return false;
    }

    // Otherwise ask vanilla's own question: is there ANY danger left that it can perceive?
    Object* danger = ai_danger_source(npc);
    return danger == NULL || !is_within_perception_unsneaked(npc, danger);
}

// True if `x` is ACTIVELY a threat to the player's side: it has hit the dude or a follower
// (their whoHitMe points at x), or the dude is personally fighting it (lock / attack intent).
// Companions use this so they only join REAL fights -- the `hostile` flag alone is too loose
// (it stays set, and can be raised transiently), which made a follower maul a peaceful Junktown
// NPC that was flagged hostile but never actually attacking anyone.
static bool rt_threatens_player_side(Object* x)
{
    if (x == NULL) {
        return false;
    }
    if (x == rt_lock_target || x == rt_player_intent_target) {
        return true; // the dude is fighting it -> follower assists
    }
    if (obj_dude != NULL && obj_dude->data.critter.combat.whoHitMe == x) {
        return true; // it hit the dude
    }
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].friendly && rt_is_combatant(rt_actors[i].obj)
            && rt_actors[i].obj->data.critter.combat.whoHitMe == x) {
            return true; // it hit a follower
        }
    }
    return false;
}

// Nearest enemy a follower should attack: hostile, in chase range, AND actually fighting the
// player's side (rt_threatens_player_side). NULL if none qualify. The threat gate is what keeps
// a follower from attacking a critter that's merely flagged hostile but not engaging us.
static Object* rt_nearest_hostile(Object* from)
{
    Object* best = NULL;
    int bestDist = INT_MAX;
    int range = rt_chase_range(from);
    for (int i = 0; i < rt_actor_count; i++) {
        Object* e = rt_actors[i].obj;
        // Target a live sticky-hostile that is EITHER actively threatening the player's
        // side OR simply fighting SOMEONE (whoHitMe != NULL -- e.g. it retargeted onto
        // Killian). The old code required rt_threatens_player_side ALONE, so a follower
        // ABANDONED an enemy the instant it stopped hitting us (retargeted, or our victim
        // died) even with live enemies still around -- "Ian suddenly stops attacking".
        // The `whoHitMe != NULL` clause keeps followers on real brawlers while still
        // excluding a PEACEFUL NPC that was only transiently mis-flagged hostile (its
        // whoHitMe is NULL) -- the Junktown false-positive that rt_threatens_player_side
        // was added to prevent.
        if (!rt_actors[i].hostile || !rt_is_combatant(e)) {
            continue;
        }
        if (!rt_threatens_player_side(e) && e->data.critter.combat.whoHitMe == NULL) {
            continue;
        }
        int d = obj_dist(from, e);
        if (d <= range && d < bestDist) {
            bestDist = d;
            best = e;
        }
    }
    return best;
}

// Nearest target on the PLAYER's side -- the dude OR a party follower -- within npc's chase
// range, for a hostile to attack. Lets enemies engage companions, not only the dude.
static Object* rt_nearest_player_side(Object* npc)
{
    Object* best = NULL;
    int bestDist = INT_MAX;
    int range = rt_chase_range(npc);

    if (rt_is_combatant(obj_dude)) {
        int d = obj_dist(npc, obj_dude);
        if (d <= range) {
            best = obj_dude;
            bestDist = d;
        }
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (!rt_actors[i].friendly || !rt_is_combatant(rt_actors[i].obj)) {
            continue;
        }
        int d = obj_dist(npc, rt_actors[i].obj);
        if (d <= range && d < bestDist) {
            bestDist = d;
            best = rt_actors[i].obj;
        }
    }
    return best;
}

// Whether an object is currently a tracked actor flagged hostile to the dude.
// False for anything not in the registry. Used to gate click-to-attack so only
// hostiles are attacked and peaceful NPCs route to normal interaction.
static bool rt_is_hostile(Object* obj)
{
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == obj) {
            return rt_actors[i].hostile;
        }
    }
    return false;
}

// Public: is this critter currently flagged hostile to the dude? (The interact prompt
// uses it to suppress the "talk" face on a critter you're fighting -- e.g. a rat.)
bool combat_rt_is_hostile(Object* obj)
{
    return rt_is_hostile(obj);
}

// True while the engine's single global main_ctd still holds an UNCOMMITTED attack -- i.e. a
// combat_attack() ran but combat_rt hasn't snapshotted+freed it yet. With concurrency this is
// transient: rt_do_attack snapshots and clears it the same call, and a scripted orphan is
// adopted at the top of the next update, so it should never persist beyond a frame. Kept only as
// the watchdog's "did the global wedge" signal. (Per-attack progress is tracked by the in-flight
// list, not this flag.)
static bool rt_attack_locked()
{
    return combat_is_attack_pending();
}

// True when the critter's right hand holds something it can actually FIGHT with.
//
// ⚠️ item_hit_with returns whatever is in the hand -- ANY item, not necessarily a weapon. A hand
// full of First Aid Kit is a perfectly normal thing for a proto to ship with, and the engine knows
// it: vanilla's own item_mp_cost (item.cc) guards on item_get_type before touching the weapon
// fields. Everything downstream of the hit mode reads the proto's WEAPON union member
// (item_w_range -> data.weapon.maxRange1, item_w_mp_cost -> primary AP cost), so handing it a
// non-weapon does not fail loudly -- it reads whatever bytes another item type stored in that union
// and carries on with a nonsense number.
static bool rt_hand_holds_weapon(Object* critter)
{
    Object* item = item_hit_with(critter, HIT_MODE_RIGHT_WEAPON_PRIMARY);
    return item != NULL && item_get_type(item) == ITEM_TYPE_WEAPON;
}

// Picks the hit mode an NPC will attack with: its right-hand weapon if it has
// one, otherwise an unarmed punch.
//
// ⚠️ THE NON-WEAPON TRAP (2026-08-15, "Morbid just stands there and never attacks"). This used to
// accept any non-NULL right hand. Doc Morbid carries a First Aid Kit (pid 47, ITEM_TYPE_MISC), so
// he resolved to HIT_MODE_RIGHT_WEAPON_PRIMARY and item_w_range read maxRange1 off a MISC union:
// **0**. rt_update_actor's chase gate is `engage = min(range, PREFERRED)` and `inRange = dist <=
// engage`, and dist is never below 1 -- so inRange was false at every distance and he could never
// leave RT_CHASE. trace_2026-08-15_141434: `chase-why ... range=0 engage=0 dist=1 inRange=0
// rhand=47`, and his balance row is `atk=0 approach=85%` over a 33 s fight. The AP cost read off
// the same union is the other half of it -- that is the `atk=0 ap=82%` row from 2026-08-14, the
// same critter, the same cause, a different garbage field.
//
// Falling back to PUNCH is the right answer and not a consolation prize: rt_ensure_weapon_drawn
// runs first and will have pulled a real weapon out of the pack if he owns one, so reaching here
// means he genuinely has nothing but his fists -- which is exactly what vanilla's AI does with him.
// True if this critter's CURRENT fid can actually play the attack animation for `hitMode`.
//
// ⚠️ A critter's fid carries a weapon animation code in bits 12-15 (art.h: 1 = knife 'd',
// 6 = SMG 'i', ...) and the attack art file is built from THAT code, not from the weapon in
// the hand -- both action_melee and register_object_animate use `(owner->fid & 0xF000) >> 12`.
// When the two disagree the engine asks for an art that was never drawn: art_ptr_lock fails,
// anim_preload calls anim_cleanup, the whole transaction is torn down, register_end returns -1
// and combat_attack reports -1 with NOTHING logged anywhere. Silently, forever, every frame.
//
// MEASURED (trace_2026-08-23_155712, Lorenzo, proto 120): fid 0100602d = weapon code 6 = SMG,
// art letter 'i', while item_w_range on his actual weapon is 1 (a melee item). NMMAXX has
// IA/IB/IC/ID/IE/IH/II/IJ/IK but NO **IF** (thrust) and no **IG** (swing) -- an SMG does not
// stab -- so all 642 of his attacks in that fight died in anim_preload. He walked up to the
// player, failed to swing, stepped back, and did it again: the "he just paces" report.
//
// Vanilla asks the same question in ai_can_use_weapon (combatai.cc) before it lets the AI pick
// a weapon at all, and refusing the weapon (-> PUNCH) is exactly what it does on a miss.
static bool rt_attack_art_ok(Object* critter, int hitMode)
{
    Object* weapon = item_hit_with(critter, hitMode);
    if (weapon == NULL) {
        return false;
    }

    int fid = art_id(OBJ_TYPE_CRITTER,
        critter->fid & 0xFFF,
        item_w_anim_weap(weapon, hitMode),
        (critter->fid & 0xF000) >> 12,
        critter->rotation + 1);

    return art_exists(fid);
}

static int rt_hit_mode(Object* critter)
{
    if (rt_hand_holds_weapon(critter) && rt_attack_art_ok(critter, HIT_MODE_RIGHT_WEAPON_PRIMARY)) {
        return HIT_MODE_RIGHT_WEAPON_PRIMARY;
    }

    // Either nothing to fight with, or the weapon it holds has no attack art for the body it
    // is wearing. Fists always do (ANIM_THROW_PUNCH resolves to the 'a' art set every biped
    // ships with), so punching is the difference between a fight and a critter pacing forever.
    return HIT_MODE_PUNCH;
}

// Make an NPC draw its best weapon before it engages, the same way the turn-based
// AI does (ai_switch_weapons). Without this rt_hit_mode falls back to PUNCH for any
// critter that spawns with its weapon stowed in inventory rather than pre-wielded in
// the right hand (e.g. Gizmo's assassin) -- so it would walk up and melee instead of
// drawing and shooting. We only run the inventory search + take-out animation once
// per engagement (weapon_checked latch), and only while the NPC is idle so the
// wield's register_begin/register_end doesn't stomp an in-progress animation.
// Re-stamps the critter's weapon POSE (fid bits 12-15) from the weapon it is actually holding.
//
// The engine keeps those in sync through inven_wield -> register_object_take_out, which is a
// REGISTERED animation: it only lands if a transaction is open and survives to play. Anything
// that puts an item in a hand without that -- a script, a load, a wield whose sequence was
// cleared -- leaves the pose behind, and a pose that disagrees with the hand means every attack
// animation is looked up under the wrong art letter (see rt_attack_art_ok).
//
// Deliberately conservative: only while the critter is idle (changing the fid under a running
// animation swaps the art out from under it), only for the STAND pose, and only if the new fid
// and its attack art BOTH exist -- otherwise leave the pose alone and let rt_hit_mode fall back
// to fists rather than trade a broken attack for an invisible critter.
// Defined further down (next to the other animation helpers); needed here for the wield.
static bool rt_register_begin(int requestOptions, const char* where);

// Returns TRUE once the pose is settled -- either it already matched, or it was just
// re-stamped. FALSE means "ask again next frame": the critter was mid-animation, which is most
// of a chase, and latching on that would leave the pose wrong for the whole fight.
static bool rt_sync_weapon_fid(Object* npc)
{
    if (npc == NULL) {
        return true;
    }

    // Changing the fid under a running animation swaps the art out from under it, and during a
    // chase the anim type is WALK/RUN rather than STAND. Retry, do not give up.
    if (anim_busy(npc) != 0 || FID_ANIM_TYPE(npc->fid) != ANIM_STAND) {
        return false;
    }

    Object* weapon = inven_right_hand(npc);
    int want = (weapon != NULL && item_get_type(weapon) == ITEM_TYPE_WEAPON)
        ? item_w_anim_code(weapon)
        : 0;
    int have = (npc->fid & 0xF000) >> 12;
    if (want == have) {
        return true; // already holding the pose its weapon belongs to
    }

    int standFid = art_id(OBJ_TYPE_CRITTER, npc->fid & 0xFFF, ANIM_STAND, want, npc->rotation + 1);
    if (!art_exists(standFid)) {
        return true; // nothing better exists -- settle for what it has, PUNCH covers the attack
    }

    if (want != 0) {
        int attackFid = art_id(OBJ_TYPE_CRITTER,
            npc->fid & 0xFFF,
            item_w_anim_weap(weapon, HIT_MODE_RIGHT_WEAPON_PRIMARY),
            want,
            npc->rotation + 1);
        if (!art_exists(attackFid)) {
            return true; // the pose it wants can't swing either -- rt_hit_mode will use fists
        }
    }

    Rect rect;
    if (obj_change_fid(npc, standFid, &rect) != 0) {
        return false; // retry next frame
    }

    tile_refresh_rect(&rect, npc->elevation);
    trace_event(TRACE_FLOW, "weapon-pose", "id=%d pid=%d have=%d want=%d wpn=%d t=%.2f",
        npc->id, npc->pid, have, want, weapon != NULL ? weapon->pid : -1, rt_clock);
    return true;
}

static void rt_ensure_weapon_drawn(RtActor* a)
{
    if (a->weapon_checked) {
        return;
    }

    Object* npc = a->obj;

    // Already holding a weapon in the right hand -- nothing to draw. Uses the TYPE-checked test:
    // with a plain non-NULL check a critter holding a non-weapon (Morbid's First Aid Kit) latched
    // weapon_checked here on frame one, so the inventory search below never ran and he was left
    // "armed" with an item that has no range and no AP cost. See rt_hit_mode.
    // Don't cut into a running animation; retry next frame once idle. Checked BEFORE the
    // hand test, because everything below (the wield and the pose stamp alike) needs an idle
    // critter, and latching while it walks would settle for whatever it happened to hold.
    if (anim_busy(npc) != 0) {
        return;
    }

    // ⚠️ HOLDING *A* WEAPON IS NOT THE SAME AS HOLDING ITS *BEST* ONE, and the old code stopped
    // at the former. Lorenzo (proto 120) carries a 10mm SMG **and** a Knife, and the map puts the
    // KNIFE in his hand -- so he closed to melee range and stabbed, with a submachine gun in his
    // pack, because the check was only "is there a weapon in the hand?". Vanilla never settles
    // for that: every AI turn runs ai_switch_weapons -> ai_search_inven -> ai_best_weapon, which
    // prefers RANGED over melee and the pricier of two of a kind.
    //
    // ai_search_inven skips whatever is already held, so it returns the best OTHER weapon; the
    // ai_best_weapon comparison against the held one decides whether swapping is actually an
    // upgrade. check_action_points = 0: combat_rt runs its own AP pool, so the engine AP field
    // must not veto the choice.
    Object* held = rt_hand_holds_weapon(npc) ? inven_right_hand(npc) : NULL;
    Object* candidate = ai_search_inven(npc, 0);

    if (candidate != NULL && ai_best_weapon(held, candidate) == candidate) {
        // Inside a transaction of our own: inven_wield stamps the new weapon POSE through
        // register_object_take_out, which is a registered animation and silently does nothing
        // when no transaction is open -- that leaves the item in the hand and the body still
        // posed for the old one, which is exactly the mismatch that kills every attack
        // (see rt_attack_art_ok). Bare inven_wield was how this code called it before.
        if (rt_register_begin(ANIMATION_REQUEST_RESERVED, "wield")) {
            inven_wield(npc, candidate, 1);
            register_end();
            trace_event(TRACE_FLOW, "draw-weapon", "npc pid=%d wpn=%d held=%d t=%.2f",
                npc->pid, candidate->pid, held != NULL ? held->pid : -1, rt_clock);
        }
        return; // let the take-out play; the pose is re-checked next frame
    }

    // Nothing better to switch to. The hand may still disagree with the BODY, though -- the pose
    // carries its own weapon code and a stale one asks for art nobody drew. Latch only once that
    // is settled.
    a->weapon_checked = rt_sync_weapon_fid(npc);
}

// The AP an actor must pay to attack in the given hit mode (its original Fallout
// move-point cost, floored at 1 so a zero-cost weapon still gates).
static int rt_ap_cost(Object* actor, int hitMode, bool aiming = false)
{
    int apCost = item_w_mp_cost(actor, hitMode, aiming);
    if (apCost < 1) {
        apCost = 1;
    }
    return apCost;
}

// Minimum seconds between this critter's own attacks -- its PHYSICAL rate of fire, as opposed to
// AP, which is the RESOURCE ("can I afford it" vs "how fast can I actually do it"). Scaled by the
// stat that owns the attack category, the same mapping and the same asymmetric curve the attack
// ANIMATIONS use: melee by STRENGTH, ranged by PERCEPTION, thrown by AGILITY.
//
// ⚠️ WHY A COOLDOWN AND NOT A STAT-SCALED AP COST (2026-08-10, the user's proposal, and it is the
// better one). Two reasons:
//   * It CANNOT be used to lock anyone down. Your cooldown depends only on YOUR stat and YOUR last
//     swing -- no one else can extend it. The alternative on the table was suppression ("a critter
//     you just hit loses its next attack window"), which is the June melee-starvation bug by
//     design and would have needed a whole immunity system to be safe. This needs none.
//   * It lives entirely inside combat_rt. Scaling `item_w_mp_cost` instead would have to satisfy
//     its ~10 readers across combat.cc / combatai.cc / gmouse.cc / intface.cc / anim.cc, or the
//     "can you afford it" checks would disagree with what is actually charged.
//
// The measurement that justified it: real gaps between one attacker's own swings were 1.1-2.1 s
// average with a **0.05 s** minimum (trace 2026-08-10), because `rt_attacker_in_flight` guards
// only the dude -- an NPC could open a second attack while its first was still in flight. That
// double-tap is what read as "the enemy spams attacks".
//
// Returns 0 when disabled by config, which restores the old behaviour exactly.
// Which SPECIAL owns an attack category: melee (and a bare hand) STRENGTH, ranged PERCEPTION,
// thrown AGILITY. Split out of rt_attack_cooldown so the character sheet can ask the same question
// about a skill that has no weapon in hand -- see combat_rt_skill_attack_speed.
static int rt_cooldown_stat_for_type(int attackType)
{
    if (attackType == ATTACK_TYPE_RANGED) {
        return STAT_PERCEPTION;
    }
    if (attackType == ATTACK_TYPE_THROW) {
        return STAT_AGILITY;
    }
    return STAT_STRENGTH;
}

// The two multipliers the cooldown is made of, as pure functions of a LEVEL. They exist so that
// nothing outside this file has to restate the formula to display it -- the character sheet prints
// exactly what the fight will use, and a change here cannot leave the readout lying.
//
// Same shape as ANIM_ATTACK_STAT_HIGH_BIAS: points above average are worth more, points below
// average keep the plain factor so a weak character is not punished twice over.
static float rt_cooldown_stat_mult(int statLevel)
{
    int delta = statLevel - 5;
    float factor = delta > 0 ? rt_attack_cooldown_stat_factor * rt_attack_cooldown_high_bias
                             : rt_attack_cooldown_stat_factor;
    return 1.0f - delta * factor;
}

static float rt_cooldown_skill_mult(int skillLevel)
{
    if (rt_attack_cooldown_skill_factor <= 0.0f) {
        return 1.0f;
    }

    float skillMult = 1.0f - ((float)skillLevel - 100.0f) / 100.0f * rt_attack_cooldown_skill_factor;

    // Clamp the MULTIPLIER, not the skill: a modded/console skill far outside 0-200 must not be
    // able to invert the term (a negative multiplier would hand back a negative cooldown).
    if (skillMult < 1.0f - rt_attack_cooldown_skill_factor) {
        skillMult = 1.0f - rt_attack_cooldown_skill_factor;
    } else if (skillMult > 1.0f + rt_attack_cooldown_skill_factor) {
        skillMult = 1.0f + rt_attack_cooldown_skill_factor;
    }
    return skillMult;
}

static float rt_attack_cooldown(Object* attacker, int hitMode)
{
    if (attacker == NULL || rt_attack_cooldown_base <= 0.0f) {
        return 0.0f;
    }

    int attackType = ATTACK_TYPE_UNARMED;
    Object* weapon = item_hit_with(attacker, hitMode);
    if (weapon != NULL) {
        attackType = item_w_subtype(weapon, hitMode);
    }
    int stat = rt_cooldown_stat_for_type(attackType);

    float cd = rt_attack_cooldown_base * rt_cooldown_stat_mult(stat_level(attacker, stat));

    // WEAPON SKILL, a SECOND and deliberately much weaker term (the user's proposal, 2026-08-15).
    //
    // It belongs on the cooldown and nowhere else, and the balance telemetry is what settled that:
    // fights run at `cool=65-70% ap=0-4%`, so the cooldown is the binding constraint. Skill on the
    // AP COST would fit the "cooldown = rhythm, AP = resource" split more neatly but would be
    // invisible at those numbers, and skill on ANIMATION SPEED would be nearly invisible too -- an
    // attack animation is ~0.5 s hidden inside a ~2 s cooldown.
    //
    // ⚠️ WHY IT MUST STAY SMALL, and why it is NOT shaped like the SPECIAL term above.
    //   * SPECIAL IS FIXED, SKILL GROWS. A stat is chosen at creation and barely moves; a skill runs
    //     0-200 and climbs all game. Given equal weight, the player's rate of fire would accelerate
    //     for the whole campaign -- a power treadmill SPECIAL cannot produce, and one that would
    //     quietly invalidate a pacing scheme calibrated at a single moment in that curve.
    //   * IT DOUBLE-DIPS. Skill already buys TO-HIT (item_w_skill_level, combat.cc:3293). Letting it
    //     buy throughput as well makes one investment compound, which is the classic way a balance
    //     lever gets away from you.
    // At the default 0.15 the whole 0-200 range spans about +-15% (1.15x), against SPECIAL's 3.7x.
    // Stats stay what defines the character; skill is a progression bonus you can feel.
    //
    // Vanilla gives weapon skill NO effect on speed at all -- only on to-hit -- so this is a
    // deliberate deviation, like the medic beat treating allies.
    // item_w_skill_level resolves the weapon's own skill, and falls back to SKILL_UNARMED when
    // the hand is empty -- so this is correct for guns, melee and fists without a table here.
    cd *= rt_cooldown_skill_mult(item_w_skill_level(attacker, hitMode));

    if (cd < RT_ATTACK_COOLDOWN_MIN) {
        cd = RT_ATTACK_COOLDOWN_MIN;
    }
    return cd;
}

// --- WHAT THE CHARACTER SHEET PRINTS ------------------------------------------------------------
//
// Both queries hand back the SAME numbers the fight uses, because they go through the same two
// helpers above. Nothing here recomputes anything; a change to the formula or to the cfg levers
// moves the readout with it, which is the only way a displayed number stays true.
//
// The attack category is taken from the SKILL rather than from a weapon, so the card is correct
// with an empty hand and during character creation: the three gun skills are ranged, unarmed and
// melee are melee, throwing is thrown -- exactly the mapping item_w_subtype would produce for a
// weapon of that skill.
static int rt_attack_type_for_skill(int skill)
{
    switch (skill) {
    case SKILL_SMALL_GUNS:
    case SKILL_BIG_GUNS:
    case SKILL_ENERGY_WEAPONS:
        return ATTACK_TYPE_RANGED;
    case SKILL_THROWING:
        return ATTACK_TYPE_THROW;
    case SKILL_MELEE_WEAPONS:
        return ATTACK_TYPE_MELEE;
    case SKILL_UNARMED:
        return ATTACK_TYPE_UNARMED;
    default:
        return ATTACK_TYPE_NONE;
    }
}

bool combat_rt_skill_attack_speed(Object* critter, int skill, int skillLevel,
    float* seconds, float* statMult, float* skillMult)
{
    if (critter == NULL || !combat_rt_enabled() || rt_attack_cooldown_base <= 0.0f) {
        return false;
    }

    int attackType = rt_attack_type_for_skill(skill);
    if (attackType == ATTACK_TYPE_NONE) {
        return false; // not a combat skill -- it buys no rate of fire at all
    }

    float sm = rt_cooldown_stat_mult(stat_level(critter, rt_cooldown_stat_for_type(attackType)));
    float km = rt_cooldown_skill_mult(skillLevel);

    float cd = rt_attack_cooldown_base * sm * km;
    if (cd < RT_ATTACK_COOLDOWN_MIN) {
        cd = RT_ATTACK_COOLDOWN_MIN;
    }

    if (seconds != NULL) {
        *seconds = cd;
    }
    if (statMult != NULL) {
        *statMult = sm;
    }
    if (skillMult != NULL) {
        *skillMult = km;
    }
    return true;
}

bool combat_rt_stat_attack_speed(int stat, int statLevel, float* mult, int* attackType)
{
    if (!combat_rt_enabled() || rt_attack_cooldown_base <= 0.0f) {
        return false;
    }

    int type;
    switch (stat) {
    case STAT_STRENGTH:
        type = ATTACK_TYPE_MELEE;
        break;
    case STAT_PERCEPTION:
        type = ATTACK_TYPE_RANGED;
        break;
    case STAT_AGILITY:
        type = ATTACK_TYPE_THROW;
        break;
    default:
        return false; // this stat owns no attack category
    }

    if (mult != NULL) {
        *mult = rt_cooldown_stat_mult(statLevel);
    }
    if (attackType != NULL) {
        *attackType = type;
    }
    return true;
}

float combat_rt_attack_cooldown_base()
{
    return rt_attack_cooldown_base;
}

// --- THE WEAPON RAISE (ANIM_POINT) -------------------------------------------------------------
//
// Seconds after a shot during which the weapon still counts as SHOULDERED, so the next shot skips
// the ANIM_POINT raise. Special values: < 0 raises never, 0 raises on every shot (plain vanilla).
//
// WHY A HOLD AND NOT A FLAT ON/OFF. The raise is a turn-based ceremony: vanilla fires once per
// turn, so raise -> fire -> lower is one complete action. Fire continuously and it becomes a
// PER-SHOT tax -- measured at 0.43 s of nothing visible before every round (trace_2026-08-10_-
// 183532), which took damage-on-a-10mm from 0.53 s to 0.89 s. That was cut on 2026-08-10 and the
// cost was the reported "animace se tak zvláštně usekává ke konci".
//
// The hold pays it ONCE PER BURST instead: you shoulder the gun, fire as long as you keep firing,
// and only a real lull makes you raise it again. So the opening shot of an engagement gets its
// wind-up and the follow-ups keep the 0.53 s.
//
// ⚠️ WHY THIS DOES NOT ALSO LATCH ANIM_UNPOINT, i.e. why the pair is deliberately ASYMMETRIC.
// UNPOINT is the only transition from the last frame of firing (gun up) back to the idle pose --
// without it anim_set_end -> dude_stand snaps between them in one frame, which is exactly the
// report that got it restored. Holding it for the whole burst would need a separate "the fight
// lulled, lower your weapon" trigger that nothing currently owns. The visible consequence is that
// follow-up shots fire from the lowered pose without re-raising -- which is precisely how EVERY
// shot has looked since 2026-08-10, so it is a pose the player has already accepted.
//
// ⚠️ MOVEMENT DELIBERATELY DOES NOT CLEAR THE HOLD. Lowering the gun to step would be the more
// literal fiction, but it would charge the raise again on every strafe -- and shoot-step-shoot is
// the common pattern, so that quietly rebuilds the per-shot tax this exists to remove. The rule is
// one sentence on purpose: "have you fired recently?"
#define RT_POINT_HOLD_SECONDS 2.0f
// The hold is really "one and a half of YOUR OWN attack cycles" -- see the measurement note in
// rt_note_attack_fired for why a flat number could not work. 1.5 means you keep the weapon up as
// long as you never skip more than half a beat; miss a whole one and you re-shoulder it.
#define RT_POINT_HOLD_CD_MULT 1.5f
static float rt_point_hold = RT_POINT_HOLD_SECONDS;

// Player half of weapon_up_until (the dude is not in rt_actors).
static float rt_player_weapon_up_until = 0.0f;

bool combat_rt_weapon_is_up(Object* attacker)
{
    if (rt_point_hold < 0.0f) {
        return true; // raise disabled entirely -- the 2026-08-10 behaviour
    }
    if (rt_point_hold == 0.0f || attacker == NULL) {
        return false; // raise on every shot -- plain vanilla
    }

    if (attacker == obj_dude) {
        return rt_clock < rt_player_weapon_up_until;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == attacker) {
            return rt_clock < rt_actors[i].weapon_up_until;
        }
    }

    return false;
}

// Stamps the attacker's next allowed swing. Called at FIRE time (not commit) so the gap measured
// is swing-to-swing, which is what the trace measures and what the player feels.
static void rt_note_attack_fired(Object* attacker, int hitMode)
{
    // THE NOISE an idle hostile turns toward (rt_idle_look_around): a gun going off on the player's
    // side. Guns only -- a punch or a knife is not heard two rooms away, and the aggro spread draws
    // the same line (rt_hears_gunfire). Type-checked: a hand can hold a non-weapon.
    if (attacker != NULL && (attacker == obj_dude || isPartyMember(attacker))) {
        Object* weapon = item_hit_with(attacker, hitMode);
        if (weapon != NULL && item_get_type(weapon) == ITEM_TYPE_WEAPON && item_w_max_ammo(weapon) > 0
            && item_w_subtype(weapon, hitMode) == ATTACK_TYPE_RANGED) {
            rt_noise_clock = rt_clock;
            rt_noise_tile = attacker->tile;
            rt_noise_elev = attacker->elevation;
        }
    }

    float cd = rt_attack_cooldown(attacker, hitMode);

    // WEAPON IS NOW SHOULDERED, and the hold is measured against THIS ATTACKER'S OWN CADENCE.
    //
    // ⚠️ IT WAS A FLAT 2.0 s FOR ONE BUILD AND THAT WAS WRONG -- measured, not guessed
    // (trace_2026-08-15_155257, 24 player shots): the rule came out perfectly sharp, every gap
    // <= 2.0 s kept the weapon up and every gap > 2.0 s re-raised. The player's cooldown is 1.98 s,
    // so the hold cleared his cadence by 0.02 s: any hesitation at all -- releasing the attack
    // button for a beat, a step, switching target -- pushed him over and he re-aimed. 8 of 24 shots
    // (33 %) raised again, which is exactly the "it feels like I aim every single time" report.
    // For NPCs it never worked AT ALL: their cooldowns are 2.29-2.91 s, all above the flat hold, so
    // every enemy shot in that log re-raised.
    //
    // Scaling by the attacker's own cooldown makes it hold BY CONSTRUCTION for everyone, whatever
    // their Perception or whatever the scheme's cooldown base is set to. The cfg value survives as
    // a FLOOR, which is what keeps it meaningful when the cooldown is disabled entirely.
    float hold = rt_point_hold;
    if (hold > 0.0f) {
        float byCadence = cd * RT_POINT_HOLD_CD_MULT;
        if (byCadence > hold) {
            hold = byCadence;
        }
    }

    // ⚠️ Stamped BEFORE the cooldown early-outs below: with attack_cooldown_base=0 that `cd <= 0`
    // return fires on every shot, and stamping after it would silently leave the raise playing
    // every time for anyone running without a cooldown.
    if (attacker == obj_dude) {
        rt_player_weapon_up_until = rt_clock + hold;
    } else {
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].obj == attacker) {
                rt_actors[i].weapon_up_until = rt_clock + hold;
                break;
            }
        }
    }

    // Balance telemetry counts attacks HERE, where one is definitively committed, rather than at
    // the intent stage -- intents are rebuilt every frame and most of them never become a swing.
    if (attacker == obj_dude) {
        rt_bal_dude_attacks++;
    } else {
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].obj == attacker) {
                rt_actors[i].bal_attacks++;
                break;
            }
        }
    }

    if (cd <= 0.0f) {
        return;
    }

    if (attacker == obj_dude) {
        rt_player_next_attack_at = rt_clock + cd;
        return;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == attacker) {
            rt_actors[i].next_attack_at = rt_clock + cd;
            return;
        }
    }
}

static bool rt_attack_on_cooldown(Object* attacker);
static float rt_regen_interval(Object* critter);
static int rt_player_hit_mode();
static int rt_player_called_loc(int hitMode);

// Seconds a critter waits before its FIRST swing of an engagement -- its initiative.
//
// Measured against the DUDE's Sequence rather than on an absolute scale, because "who shoots first"
// is only ever a question about the two of you: a critter at least as quick as you gets none (it
// really can open on you), a slower one waits in proportion to how far behind it is, capped at the
// full span. Sequence is Perception x 2, so this is where PE buys the drop on somebody.
//
// The jitter is not decoration. Without it two identical raiders share a Sequence, get an identical
// delay, and swing in unison forever -- which is the same lock-step this exists to break, just with
// more people in it.
static float rt_initiative_delay(Object* critter)
{
    if (rt_initiative_span <= 0.0f || critter == NULL || obj_dude == NULL) {
        return 0.0f;
    }

    int dudeSeq = stat_level(obj_dude, STAT_SEQUENCE);
    int seq = stat_level(critter, STAT_SEQUENCE);
    if (dudeSeq < 1) {
        dudeSeq = 1;
    }

    float behind = (float)(dudeSeq - seq) / (float)dudeSeq;
    if (behind < 0.0f) {
        behind = 0.0f; // as quick as the dude, or quicker -- swings at once
    } else if (behind > 1.0f) {
        behind = 1.0f;
    }

    float jitter = (float)roll_random(0, 250) / 1000.0f;
    return rt_initiative_span * behind + jitter;
}

static void rt_missing_art_scan()
{
    if (obj_dude == NULL || rt_clock < rt_art_scan_at) {
        return;
    }
    rt_art_scan_at = rt_clock + RT_ART_SCAN_SECONDS;

    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER || obj == obj_dude) {
            continue;
        }
        // Staged content is not missing art, it is simply not shown yet -- warning about it would
        // be pure noise.
        if ((obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }
        if (art_exists(obj->fid)) {
            continue;
        }

        bool known = false;
        for (int i = 0; i < rt_art_seen_count; i++) {
            if (rt_art_seen[i] == obj->pid) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }
        if (rt_art_seen_count < RT_ART_SEEN_MAX) {
            rt_art_seen[rt_art_seen_count++] = obj->pid;
        }

        trace_event(TRACE_WARN, "missing-art",
            "pid=%d id=%d name=%s fid=%08x anim=%d tile=%d dist=%d t=%.2f",
            obj->pid, obj->id, object_name(obj), obj->fid, FID_ANIM_TYPE(obj->fid),
            obj->tile, obj_dist(obj_dude, obj), rt_clock);
    }
}

// Split one frame of one actor into the reason it is not swinging. Order matters: it is a
// PRIORITY, not a set -- an actor mid-animation is also technically on cooldown, and reporting it
// as cooldown would blame the wrong lever.
static void rt_balance_bucket(Object* obj, RtState state, int ap, int apCost, float dt,
    float* busy, float* cooldown, float* apStarved, float* approach, float* ready)
{
    if (anim_busy(obj) != 0) {
        // MOVING counts as approach, not busy. anim_busy is true for a walk too, so without this
        // split every step the dude took landed in `busy` alongside his swings -- and `busy` is
        // supposed to mean "the animation system owes me time", which is an argument for changing
        // animation speeds, whereas time spent walking is an argument about positioning and range.
        int at = FID_ANIM_TYPE(obj->fid);
        if (at == ANIM_WALK || at == ANIM_RUNNING) {
            *approach += dt;
        } else {
            *busy += dt;
        }
    } else if (state == RT_CHASE) {
        *approach += dt;
    } else if (rt_attack_on_cooldown(obj)) {
        *cooldown += dt;
    } else if (ap < apCost) {
        *apStarved += dt;
    } else {
        *ready += dt;
    }
}

static void rt_balance_begin()
{
    rt_bal_start = rt_clock;
    rt_bal_dude_busy = rt_bal_dude_cooldown = rt_bal_dude_ap = 0.0f;
    rt_bal_dude_approach = rt_bal_dude_ready = 0.0f;
    rt_bal_dude_attacks = 0;

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        a->bal_busy = a->bal_cooldown = a->bal_ap = a->bal_approach = a->bal_ready = 0.0f;
        a->bal_breath = 0.0f;
        a->bal_attacks = 0;
        a->bal_seen = rt_clock;
    }
}

static void rt_balance_sample(float dt)
{
    if (obj_dude != NULL) {
        int hm = rt_player_hit_mode();
        int cost = rt_ap_cost(obj_dude, hm, rt_player_called_loc(hm) != HIT_LOCATION_UNCALLED);
        rt_balance_bucket(obj_dude, RT_ATTACK, rt_player_ap, cost, dt,
            &rt_bal_dude_busy, &rt_bal_dude_cooldown, &rt_bal_dude_ap,
            &rt_bal_dude_approach, &rt_bal_dude_ready);
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (a->obj == NULL || a->state == RT_IDLE || !rt_is_combatant(a->obj)) {
            continue; // bystanders are not part of the fight's tempo
        }
        int cost = rt_ap_cost(a->obj, rt_hit_mode(a->obj));
        rt_balance_bucket(a->obj, a->state, a->ap, cost, dt,
            &a->bal_busy, &a->bal_cooldown, &a->bal_ap, &a->bal_approach, &a->bal_ready);
    }
}

// One block per fight. Percentages, not seconds, so fights of different lengths compare directly.
// Defined with the reaction layer, far below; the fight report rides this one.
static void rt_reaction_report();

static void rt_balance_report()
{
    if (rt_bal_start < 0.0f) {
        return;
    }

    float span = rt_clock - rt_bal_start;
    if (span < 1.0f) {
        rt_bal_start = -1.0f;
        return; // too short to mean anything
    }

    rt_reaction_report();

    trace_event(TRACE_COMBAT, "balance", "=== fight %.1fs | levers: ap_regen=%.2f agiF=%.2f near=%.2f far=%.2f nearMov=%.2f farMov=%.2f npcNearMov=%.2f npcFarMov=%.2f cd=%.2f cdF=%.2f cdSkillF=%.2f",
        span, rt_ap_regen_seconds, rt_ap_regen_agility_factor,
        rt_regen_near_mult, rt_regen_far_mult,
        rt_regen_near_mult_moving, rt_regen_far_mult_moving,
        rt_npc_regen_near_mult_moving, rt_npc_regen_far_mult_moving,
        rt_attack_cooldown_base, rt_attack_cooldown_stat_factor,
        rt_attack_cooldown_skill_factor);

    if (obj_dude != NULL) {
        float t = rt_bal_dude_busy + rt_bal_dude_cooldown + rt_bal_dude_ap
            + rt_bal_dude_approach + rt_bal_dude_ready;
        if (t < 0.01f) {
            t = 0.01f;
        }
        trace_event(TRACE_COMBAT, "balance",
            "DUDE str=%d per=%d agi=%d seq=%d skill=%d maxAp=%d regen=%.2fs cd=%.2fs atk=%d | busy=%.0f%% cool=%.0f%% ap=%.0f%% approach=%.0f%% ready=%.0f%%",
            stat_level(obj_dude, STAT_STRENGTH), stat_level(obj_dude, STAT_PERCEPTION),
            stat_level(obj_dude, STAT_AGILITY),
            stat_level(obj_dude, STAT_SEQUENCE),
            item_w_skill_level(obj_dude, rt_player_hit_mode()),
            stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS),
            rt_regen_interval(obj_dude), rt_attack_cooldown(obj_dude, rt_player_hit_mode()),
            rt_bal_dude_attacks,
            100.0f * rt_bal_dude_busy / t, 100.0f * rt_bal_dude_cooldown / t,
            100.0f * rt_bal_dude_ap / t, 100.0f * rt_bal_dude_approach / t,
            100.0f * rt_bal_dude_ready / t);
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        float t = a->bal_busy + a->bal_cooldown + a->bal_ap + a->bal_approach + a->bal_ready;
        if (a->obj == NULL || t < 1.0f) {
            continue; // never really joined in
        }
        trace_event(TRACE_COMBAT, "balance",
            "NPC pid=%d %s str=%d per=%d agi=%d seq=%d skill=%d maxAp=%d regen=%.2fs cd=%.2fs atk=%d hp=%d in=%.0fs | busy=%.0f%% cool=%.0f%% ap=%.0f%% approach=%.0f%% ready=%.0f%% breath=%.0f%%",
            a->obj->pid, object_name(a->obj),
            stat_level(a->obj, STAT_STRENGTH), stat_level(a->obj, STAT_PERCEPTION),
            stat_level(a->obj, STAT_AGILITY),
            stat_level(a->obj, STAT_SEQUENCE),
            item_w_skill_level(a->obj, rt_hit_mode(a->obj)),
            stat_level(a->obj, STAT_MAXIMUM_ACTION_POINTS),
            rt_regen_interval(a->obj), rt_attack_cooldown(a->obj, rt_hit_mode(a->obj)),
            a->bal_attacks, a->obj->data.critter.hp, t,
            100.0f * a->bal_busy / t, 100.0f * a->bal_cooldown / t,
            100.0f * a->bal_ap / t, 100.0f * a->bal_approach / t, 100.0f * a->bal_ready / t,
            100.0f * a->bal_breath / t);
    }

    rt_bal_start = -1.0f;
}

// True while `attacker` is still inside its own cooldown.
static bool rt_attack_on_cooldown(Object* attacker)
{
    if (rt_attack_cooldown_base <= 0.0f) {
        return false;
    }

    if (attacker == obj_dude) {
        return rt_clock < rt_player_next_attack_at;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == attacker) {
            return rt_clock < rt_actors[i].next_attack_at;
        }
    }
    return false;
}

// Regenerates a discrete AP pool toward maxAp at the fixed rate: whole points
// only, one per RT_AP_REGEN_SECONDS. When the pool is full the accumulator is
// cleared so refill time doesn't bank while idle. Ticked every frame (the pool,
// not a recovery timer, is what paces attacks).
// Seconds per AP point for THIS critter. Agility scales it -- the change that gives the stat back
// the job it has in vanilla (2026-08-10).
//
// ⚠️ WHY THIS EXISTS. Vanilla's core combat stat is Agility, because AP per TURN is literally how
// much you get to do: 5 + AGI/2, i.e. AGI 1 acts half as much as AGI 10. Realtime kept only the
// POOL SIZE (that same vanilla formula caps it) while the refill ran at one flat rate for
// everybody -- so a high-AGI character got a bigger tank and then waited exactly as long as
// everyone else. Agility had quietly been demoted from "how much you do" to "how much you can do
// at once". This restores the sustained rate as the thing the stat drives.
//
// Same curve shape as anim_attack_stat_factor deliberately: mult = 1 - (stat - 5) * factor. At the
// default 0.09 that is AGI 1 -> 0.95 s/point, AGI 5 -> 0.70, AGI 10 -> 0.385 -- a 2.5x spread,
// close to vanilla's 2x in AP per turn. Symmetric by construction: `critter` is whoever is
// regenerating, so a nimble enemy presses you harder, which is what makes the stat real rather
// than a player buff. NULL critter (defensive) keeps the flat rate.
static float rt_regen_interval(Object* critter)
{
    float interval = rt_ap_regen_seconds;

    if (critter != NULL && rt_ap_regen_agility_factor > 0.0f) {
        float mult = 1.0f - (stat_level(critter, STAT_AGILITY) - 5) * rt_ap_regen_agility_factor;
        if (mult < RT_AP_REGEN_STAT_MIN) {
            mult = RT_AP_REGEN_STAT_MIN;
        } else if (mult > RT_AP_REGEN_STAT_MAX) {
            mult = RT_AP_REGEN_STAT_MAX;
        }
        interval *= mult;
    }

    return interval;
}

static void rt_regen_ap(Object* critter, int* ap, float* accum, int maxAp, float dt)
{
    if (*ap >= maxAp) {
        *ap = maxAp;
        *accum = 0.0f;
        return;
    }

    // rt_speed_mult is 1.0 now (the Combat Speed slider scales dt instead). Floored so a
    // pathological cfg can never produce a zero/negative interval = infinite loop below.
    float interval = rt_regen_interval(critter) / rt_speed_mult;
    if (interval < 0.01f) {
        interval = 0.01f;
    }

    *accum += dt;
    while (*accum >= interval && *ap < maxAp) {
        *accum -= interval;
        (*ap)++;
    }

    if (*ap >= maxAp) {
        *accum = 0.0f;
    }
}

// Size of the Bonus Move reserve -- vanilla's own formula from the turn setup (combat.cc).
static int rt_bonus_move_max()
{
    return obj_dude != NULL ? 2 * perk_level(PERK_BONUS_MOVE) : 0;
}

// Pay ONE whole point of movement cost: the Bonus Move reserve first, then the regular pool.
// False when both are empty -- the caller's stop-latch takes it from there.
static bool rt_take_move_point()
{
    if (rt_bonus_move_ap > 0) {
        rt_bonus_move_ap--;
        if (rt_bonus_move_ap == 0) {
            trace_event(TRACE_ACTION, "bonus-move-spent", "max=%d ap=%d t=%.2f",
                rt_bonus_move_max(), rt_player_ap, rt_clock);
        }
        return true;
    }

    if (rt_player_ap > 0) {
        rt_player_ap--;
        return true;
    }

    return false;
}

// Maps a distance-to-enemy (in tiles) onto the AP regen multiplier: near_mult at contact,
// interpolating linearly to far_mult at rt_regen_falloff_tiles and beyond. Which pair of
// endpoints is used depends on whether the critter is moving AND whether it is on the moving
// curve (movingCurve) -- see RT_REGEN_NEAR_MULT_MOVING.
//
// dist < 0 means "no enemy to measure against" -- the caller uses that for out-of-combat and
// for idle NPCs, and gets a neutral 1.0 so nothing outside a fight is penalised.
static float rt_proximity_regen_mult(int dist, bool moving, bool movingCurve)
{
    if (dist < 0) {
        return 1.0f;
    }

    // THREE curves, not two. `movingCurve` says whether THIS critter uses the dude's moving pair
    // (the dude always; NPCs only under cfg regen_moving_npc=1). Anyone else who is MOVING now gets
    // the NPC moving pair rather than the standing one -- that is the whole fix: a walking NPC used
    // to be scored on the STANDING curve and then floored at 1.0, which handed it the contact BONUS
    // for walking circles around the player.
    bool useMovingCurve = moving && movingCurve;
    bool useNpcMovingCurve = moving && !movingCurve;

    float nearMult = useMovingCurve ? rt_regen_near_mult_moving
        : (useNpcMovingCurve ? rt_npc_regen_near_mult_moving : rt_regen_near_mult);
    float farMult = useMovingCurve ? rt_regen_far_mult_moving
        : (useNpcMovingCurve ? rt_npc_regen_far_mult_moving : rt_regen_far_mult);

    float mult;
    float span = (float)rt_regen_falloff_tiles - 1.0f;
    if (span <= 0.0f) {
        mult = dist <= 1 ? nearMult : farMult;
    } else {
        float t = ((float)rt_regen_falloff_tiles - (float)dist) / span;
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
        mult = farMult + (nearMult - farMult) * t;
    }

    // NO FLOOR AND NO CLAMP ANY MORE -- the NPC moving curve above expresses both intents directly.
    //
    // What used to be here: a one-sided floor pushing a moving NPC's multiplier UP to 1.0, so that
    // crossing a room to reach the fight was never punished (the first proximity-regen build made
    // approach cost -1.14 AP/s net and produced ~8 s of walking followed by a ~6 s forced stop, on
    // repeat). That intent is now the FAR END of the NPC moving curve, which is 1.0 by definition.
    //
    // What was briefly bolted on beside it: a matching clamp DOWN to 1.0, because the floor left a
    // moving critter still being scored on the STANDING curve, collecting the 1.4x contact bonus
    // for walking circles around the player -- 2.1 AP/s earned against 2.0 spent, a net GAIN. That
    // intent is now the NEAR END of the same curve, and at 0.5 it goes further than the clamp could:
    // the clamp could only reach break-even, which still means "never runs out".
    //
    // One curve, two ends, both tunable -- instead of a curve plus two corrections pulling against
    // each other.
    return mult;
}

// Distance in tiles from the dude to his NEAREST LIVE hostile, or -1 if there is none.
// rt_is_combatant is what keeps CORPSES out of it -- dead critters linger in the registry
// with a sticky hostile flag, and a body at your feet must not hand you the contact bonus.
static int rt_dude_nearest_hostile_dist()
{
    int best = -1;
    for (int i = 0; i < rt_actor_count; i++) {
        if (!rt_actors[i].hostile || !rt_is_combatant(rt_actors[i].obj)) {
            continue;
        }

        int dist = obj_dist(obj_dude, rt_actors[i].obj);
        if (best < 0 || dist < best) {
            best = dist;
        }
    }
    return best;
}

// Effective per-second AP cost of running for a given runner. Endurance discounts the
// base run cost (rt_run_ap_cost - EN * rt_run_ap_endurance_factor), so a tougher runner
// sprints longer; floored at RT_RUN_AP_MIN_COST so running is never free and the pool
// always eventually empties (defaults keep even EN 10 above regen).
//
// SYMMETRIC: the dude and every NPC are charged from this one formula (call sites: the drain
// block in combat_rt_update for obj_dude, and rt_update_actor for NPCs). The asymmetries that
// remain all FAVOUR the NPC -- it never spends below its one-attack reserve, and it has the
// run_winded latch -- so a chaser can always still swing on arrival.
//
// (rt_speed_mult is 1.0 since 2026-10-11: the Combat Speed slider is a time scale on the whole fight
// now, which already runs this drain and the regen it is netted against faster together.)
static float rt_effective_run_cost(Object* runner)
{
    float cost = rt_run_ap_cost - stat_level(runner, STAT_ENDURANCE) * rt_run_ap_endurance_factor;
    if (cost < RT_RUN_AP_MIN_COST) {
        cost = RT_RUN_AP_MIN_COST;
    }
    return cost * rt_speed_mult;
}

// Per-second walk cost, shared by the dude and NPCs exactly like the run cost above. No
// Endurance discount (Endurance is the SPRINT stat; a plain walk costs everyone the same).
static float rt_effective_walk_cost()
{
    return rt_walk_ap_cost * rt_speed_mult;
}

// Re-reads the "Combat Speed" preference and maps it onto rt_speed_mult. Called once per
// frame from the top of combat_rt_update -- BEFORE any actor update or regen, so every
// consumer in the frame sees the same value -- which is what lets the slider take effect
// the moment the player closes the options screen, with no restart.
static void rt_sample_speed_mult()
{
    int combatSpeed = 0;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, &combatSpeed);

    if (combatSpeed < 0) {
        combatSpeed = 0;
    } else if (combatSpeed > (int)RT_COMBAT_SPEED_SLIDER_MAX) {
        combatSpeed = (int)RT_COMBAT_SPEED_SLIDER_MAX;
    }

    // Linear from 1.0 at slider 0 (stock tempo -- the vanilla default, so an untouched
    // install behaves exactly as before) to rt_combat_speed_ap_max at slider 50. A TIME SCALE now
    // (combat_rt_time_scale), so rt_speed_mult is left at 1.0.
    rt_game_speed = 1.0f + (combatSpeed / RT_COMBAT_SPEED_SLIDER_MAX) * (rt_combat_speed_ap_max - 1.0f);
    if (rt_game_speed < 1.0f) {
        rt_game_speed = 1.0f;
    }
}

// AP a critter must keep in hand to still be able to swing/shoot when it arrives.
static int rt_attack_reserve_ap(Object* npc)
{
    return rt_ap_cost(npc, rt_hit_mode(npc));
}

// How far above the attack reserve an NPC's pool must climb before it may sprint AGAIN. This is
// the NPC counterpart of the dude's rt_run_resume_ap and it is what makes the sprint a BURST.
#define RT_NPC_RUN_RESUME_MARGIN 3

// BREATHER: the NPC counterpart of the dude's rt_move_blocked. When the pool is down to the
// one-attack reserve the critter STANDS instead of walking, until it has banked this much AP
// on top of the reserve. That is the whole point -- it stops with something in hand, so a
// shooter that pauses can open fire the moment you step out.
#define RT_NPC_BREATHER_MARGIN 3
static int rt_npc_breather_margin = RT_NPC_BREATHER_MARGIN;

// ⚠️ THE HARD CAP, AND THE REASON THIS IS SAFE TO SHIP AT ALL. Every previous attempt at
// stopping an NPC re-created the "enemy runs up and just looks at me" bug, so no pause here is
// allowed to be open-ended: past this many seconds the latch drops whatever the pool says. The
// worst a mistake in the latch can then cause is hesitation, never a frozen chaser.
#define RT_NPC_BREATHER_MAX_SECONDS 3.0f
static float rt_npc_breather_max = RT_NPC_BREATHER_MAX_SECONDS;

// ⚠️ HOW CLOSE YOU MUST BE TO BE ALLOWED TO STOP. Was RT_PREFERRED_ENGAGE_TILES (8) and that
// was far too generous. Measured on a Ghoul (AGI 6, maxAp 8, reserve 5) at 6 tiles: walking nets
// -0.85 AP/s, so it drains 8->5 in 3.5 s, then stands 1.8 s to climb back -- a third of the
// approach spent standing still, in a visible walk/stop/walk stutter. "skubali se a nepshlo mi
// ze by se zas tak moc hejbali."
//
// And the pause bought nothing. A critter in CHASE cannot attack by definition (if it could it
// would be in RT_ATTACK), so banking AP mid-field only delays its arrival -- and the ATTACK
// RESERVE already guarantees it turns up able to swing. At 2 tiles the pause finally means
// something readable: it is gathering itself right before it reaches you, not stalling in the
// open. See the open question in the notes -- this may want to be 0 (feature off) instead.
#define RT_NPC_BREATHER_TILES 2
static int rt_npc_breather_tiles = RT_NPC_BREATHER_TILES;

// True while a chasing NPC can still afford to SPRINT.
//
// Running costs an NPC the same AP it costs the dude (rt_effective_run_cost, Endurance discount
// included) -- but with one guarantee the player does not get: an NPC never spends its way out of
// a swing. It stops sprinting -- and the chase movers fall back to a WALK -- as soon as its pool
// is down to what ONE attack costs. Without that reserve the AI, which unlike the player sprints
// unconditionally, would arrive empty and stand there -- the "enemy runs up and just looks at me"
// bug we already chased down once. Creatures with no ANIM_RUNNING art walk either way
// (register_object_run_to_* self-falls back), so this only governs humanoids.
//
// The `run_winded` LATCH is what makes the reserve mean anything. The reserve alone stops the
// DRAIN at the floor, but regen keeps trickling in (~1.4 AP/s) and the gait is re-decided every
// segment -- so a critter parked at the floor gained a point, sprinted, spent it, gained another,
// and sprinted FOREVER on regen alone. A trace caught exactly that: id=1756 ran for 18 seconds
// straight with its pool oscillating 4<->6 ("she ran back and forth and never stopped, like she
// had infinite AP"). So hitting the floor now latches the critter as winded and it must recover
// RT_NPC_RUN_RESUME_MARGIN points above the reserve before it can sprint again -- the same
// sprint / recover-walk / sprint rhythm the player has via rt_run_resume_ap.
static bool rt_npc_sprint_ok(Object* npc)
{
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == npc) {
            return !rt_actors[i].run_winded && rt_actors[i].ap > rt_attack_reserve_ap(npc);
        }
    }
    return true; // untracked mover -- don't cripple it
}

// register_begin wrapper that fails safely when the animation table is full. The
// table has only ANIMATION_SEQUENCE_LIST_CAPACITY (21) slots; many simultaneous
// realtime combatants (e.g. a whole rat nest aggroed via the spread) can exhaust
// it. On failure register_begin returns -1 but leaves curr_anim_set pointing at
// ANOTHER object's in-flight sequence -- so a caller that ignores the result and
// proceeds to register_object_*/register_end appends to / overflows that sequence
// and corrupts memory (the radscorpion/rat-fight crash). Callers MUST gate their
// register_object_*/register_end on this returning true.
static bool rt_register_begin(int requestOptions, const char* where)
{
    if (register_begin(requestOptions) == -1) {
        trace_event(TRACE_ANIM, "register-full", "at=%s t=%.2f", where, rt_clock);
        return false;
    }
    return true;
}

// Stands a knocked-down critter back up. Realtime combat has no turns, so the
// engine's "stand up at the start of your next turn" never fires and a knocked-
// down critter would lie prone forever -- and if it was the resolving attacker,
// its anim_busy never returns to 0 so the hit-resolution slot leaks (a chronic
// lock wedge seen in the watchdog log). Mirrors dude_standup, but routed through
// the table-full-safe register wrapper (dude_standup's raw register_begin would
// corrupt a saturated anim table, the very condition combat_rt can hit). The
// knocked-down flag is cleared only once the animation actually registers, so a
// table-full frame simply retries next frame.
static void rt_stand_up(Object* critter)
{
    // NOTE: the dude used to WAIT prone until his AP pool could afford the 3-AP standup. Combined
    // with sprint exhaustion (rt_move_blocked, which also waits on the same AP), that locked the
    // player out of ALL control for seconds after every knockdown -- the "input locked / movement
    // all over the place" report (log: dude prone+blocked, stick ignored, for ~4s). So the dude now
    // stands up as soon as the fall animation finishes, regardless of AP: the standup still SPENDS
    // 3 AP if he has it (a cost), but never BLOCKS on it. NPCs stand for free as before.
    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "standup")) {
        return;
    }

    int anim = FID_ANIM_TYPE(critter->fid) == ANIM_FALL_BACK
        ? ANIM_BACK_TO_STANDING
        : ANIM_PRONE_TO_STANDING;

    // DIAG (standup-no-animation hunt): capture whether the standup animation actually REGISTERS.
    // register_object_animate returns -1 when check_registry(owner) fails (the critter still owns
    // another anim sequence) -- and rt_stand_up clears DAM_KNOCKED_DOWN below REGARDLESS, so a failed
    // register = no standup anim + critter marked up = snap. rcA = register_object_animate rc,
    // rcE = register_end rc, busyAfter = anim_busy after. Strip once resolved.
    int rcA = register_object_animate(critter, anim, 0);
    int rcE = register_end();
    trace_event(TRACE_FLOW, "standup", "id=%d pid=%d fall=%d su=%d rcA=%d rcE=%d busyAfter=%d t=%.2f",
        critter->id, critter->pid, FID_ANIM_TYPE(critter->fid), anim, rcA, rcE,
        anim_busy(critter), rt_clock);

    // Also clear DAM_LOSE_TURN: it rode in with the knockdown and, with no turn scheduler to
    // clear it in realtime, would otherwise linger and keep the critter flagged "loses its turn"
    // in the shared combatai / can't-act checks (combatai.cc, critter.cc) after it stood up.
    critter->data.critter.combat.results &= ~(DAM_KNOCKED_DOWN | DAM_LOSE_TURN);

    if (critter == obj_dude) {
        combat_rt_spend_ap(3);
        // A knockdown recovery is a fresh start: clear sprint-exhaustion so the player can move
        // again immediately after getting up, instead of being double-locked (down THEN blocked).
        rt_move_blocked = false;
    }
}

// True while a critter is playing a damage-REACTION animation: a hit-react
// flinch, a knockdown fall, or standing back up. These are the states where the
// critter's animation sequence is occupied by a reaction, so a fresh attack
// can't register its flinch into it -- the one-reaction-at-a-time guard.
//
// Crucially this checks the animation TYPE (FID_ANIM_TYPE) instead of
// anim_busy(): a WALKING / RUNNING target is busy but is NOT reacting, and must
// stay attackable (walking is not flinching). Keying the attack gate on
// anim_busy() instead made enemies unable to attack a moving player at all,
// because his walk read as "busy." So this returns false for movement, standing,
// and attack animations -- true only for the reaction animations registered by
// show_damage_to_object (actions.cc).
static bool rt_is_reacting(Object* critter)
{
    if (critter == NULL) {
        return false;
    }

    int anim = FID_ANIM_TYPE(critter->fid);
    return anim == ANIM_HIT_FROM_FRONT
        || anim == ANIM_HIT_FROM_BACK
        || anim == ANIM_FALL_BACK
        || anim == ANIM_FALL_FRONT
        || anim == ANIM_FALL_BACK_BLOOD
        || anim == ANIM_FALL_FRONT_BLOOD
        || anim == ANIM_PRONE_TO_STANDING
        || anim == ANIM_BACK_TO_STANDING;
}

// True once the attacker's CURRENT attack animation has reached its ACTION FRAME --
// the frame at which the weapon actually discharges / the melee blow connects
// (art->actionFrame, the exact frame the turn-based path launches the projectile and
// times show_damage on). Committing the hit here syncs the defender's reaction to the
// VISIBLE shot instead of waiting for the whole fire animation (raise + recoil + lower)
// to finish -- that trailing recoil/lower was the perceived "delay before the enemy
// reacts." Symmetric: applies to whichever critter is the attacker (dude and NPCs).
// Returns false when the art or a meaningful action frame is unavailable, so callers
// fall back to the swing-done / connect triggers unchanged.
// Is this animation type a critter ACTUALLY SWINGING/FIRING? (Not "is it in an attack" -- POINT,
// UNPOINT, the recoil tail and the projectile's own flight are all outside this.)
static bool rt_is_attack_anim(int anim)
{
    return anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG || anim == ANIM_THROW_ANIM
        || anim == ANIM_THRUST_ANIM || anim == ANIM_SWING_ANIM || anim == ANIM_FIRE_SINGLE
        || anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS;
}

static bool rt_attack_action_frame_reached(Object* attacker)
{
    if (attacker == NULL) {
        return false;
    }

    CacheEntry* handle;
    Art* art = art_ptr_lock(attacker->fid, &handle);
    if (art == NULL) {
        return false;
    }

    int actionFrame = art_frame_action_frame(art);
    art_ptr_unlock(handle);

    // ⚠️ ONLY MEANINGFUL WHILE AN ATTACK ANIMATION IS ACTUALLY PLAYING (fixed 2026-08-10).
    // art->actionFrame is 0 on every NON-attack FRM (stand, walk -- nothing ever set one), and
    // `frame >= 0` is trivially true, so this used to return TRUE for a critter who was still
    // STANDING: the fire animation is registered a tick or two before playback swaps the fid, and
    // in that window the test read the stand art and said "already fired". The commit then landed
    // on the RT_MIN_SWING_SECONDS floor alone -- 0.3 s flat, before the shot was ever visible.
    // Traced: two DUDE resolves, `elapsed=0.32 action=1` and `elapsed=0.55 action=1`, the first one
    // sitting exactly on the floor. Require a real attack pose AND a real (non-zero) action frame.
    // The attack POSE is the real guard: art->actionFrame is 0 on every non-attack FRM, which is
    // what used to make a STANDING critter read as "already fired".
    if (!rt_is_attack_anim(FID_ANIM_TYPE(attacker->fid))) {
        return false;
    }

    // ⚠️ actionFrame 0 on an ATTACK animation means "nobody authored one", NOT "it fires on frame
    // zero" (2026-08-10, second pass). The Hunting Rifle is such a proto: it logged action=-1 on
    // 3/3 and 7/7 shots while the guard rejected 0 outright, and then -- once 0 was accepted --
    // discharged on frame 0, i.e. INSTANTLY, which made a rifle quicker off the mark than a
    // pistol. Neither reading is right, so an unauthored animation gets a fraction of its own
    // length instead. The pistols are unaffected either way -- they have real action frames.
    //
    // ⚠️ THAT FRACTION WAS 1/2 AND IT WAS TOO SLOW, measured rather than argued (2026-09-24):
    // "vystřelil jsem a čekal jsem delší odezvu, než radscorp reaguje na kulku". Eleven rifle shots
    // in trace_2026-09-24_213943 resolved at 0.67-0.84 s, against 0.31-0.41 s for his fists and
    // ~0.10-0.20 s for an authored pistol -- so the slowest weapon to hurt anything in the game was
    // slow purely because nobody drew it an action frame, which is not a balance decision anyone
    // made. A QUARTER lands it beside the fists (~0.35 s) and still behind the pistols, which is
    // the order the weapons ought to be in. His call, offered against a fixed-time cap and against
    // speeding the animation itself up.
    int frames = 0;
    {
        CacheEntry* h2;
        Art* a2 = art_ptr_lock(attacker->fid, &h2);
        if (a2 != NULL) {
            frames = art_frame_max_frame(a2);
            art_ptr_unlock(h2);
        }
    }

    // DIAG (shot feel, 2026-09-24): ONE line the first time each attack pose is seen -- how many
    // frames it has, whether the artist authored an action frame at all, which frame we are
    // therefore waiting for, and how long a frame lasts after the SPECIAL scaling. That product is
    // the whole answer to "I fired and waited for the scorpion to notice", and it was until now
    // only inferable from the resolve timestamps. Strip with `move-diag`.
    if (trace_enabled()) {
        static Object* lastAnimLogged = NULL; // compared only, never dereferenced
        static int lastAnimLoggedFid = -1;
        if (attacker != lastAnimLogged || attacker->fid != lastAnimLoggedFid) {
            lastAnimLogged = attacker;
            lastAnimLoggedFid = attacker->fid;
            unsigned int tpf = compute_tpf(attacker, attacker->fid);
            trace_event(TRACE_COMBAT, "attack-anim",
                "%s fid=%08x anim=%d frames=%d authored=%d wait=%d tpf=%u est=%.2f t=%.2f",
                attacker == obj_dude ? "DUDE" : "NPC", attacker->fid,
                FID_ANIM_TYPE(attacker->fid), frames, actionFrame,
                actionFrame > 0 ? actionFrame : (frames > 1 ? RT_UNAUTHORED_ACTION_FRAME(frames) : -1),
                tpf,
                (actionFrame > 0 ? actionFrame : (frames > 1 ? RT_UNAUTHORED_ACTION_FRAME(frames) : 0))
                    * (float)tpf / 1000.0f,
                rt_clock);
        }
    }

    if (actionFrame <= 0) {
        if (frames <= 1) {
            return false;
        }
        actionFrame = RT_UNAUTHORED_ACTION_FRAME(frames);
    }

    return attacker->frame >= actionFrame;
}

// Does this attack put something in the AIR between attacker and defender? Guns and thrown weapons
// do; punches, kicks and melee swings do not.
//
// It matters because a projectile attack is NOT resolved when the weapon discharges -- the bullet
// or spear still has to travel. actions.cc registers that flight into the ATTACKER's own sequence
// (register_object_move_straight_to_tile on the projectile, action_ranged), so anim_busy(attacker)
// stays set until it lands: `swingDone` IS "the projectile arrived", which is vanilla's own sync
// point. Committing on the action frame instead made the target react while the spear was still in
// the air. Melee has no travel, so there the action frame is exactly the moment of contact.
static int rt_attack_type(const Attack* attack)
{
    if (attack == NULL || attack->attacker == NULL) {
        return ATTACK_TYPE_NONE;
    }

    Object* weapon = item_hit_with(attack->attacker, attack->hitMode);
    if (weapon == NULL) {
        return ATTACK_TYPE_UNARMED;
    }

    return item_w_subtype(weapon, attack->hitMode);
}

// Was this corpse's death fall CUT before it finished?
//
// The fall moves the body by adding each frame's hot-spot offset as it plays, so a fall stopped
// half way leaves the body drawn in its death pose at a FRACTION of the distance it should have
// travelled -- which on the isometric grid reads as hovering above the ground. The frame is the
// tell: a completed fall rests on its LAST frame, identically for every critter (measured
// 2026-08-10: anim 34 -> frame 3, anim 23 -> frame 21), so a corpse sitting on an earlier frame
// with nothing animating it any more was interrupted.
//
// ⚠️ The interrupter is real, reproducible and NOT ours: gdialog's `scr_dialogue_init` calls
// `anim_stop()`, which runs `anim_set_end()` over EVERY sequence and force-flattens whoever was
// mid-death. Caught by backtrace: obj_toggle_flat <- show_death <- anim_set_end <- anim_stop <-
// scr_dialogue_init <- op_start_gdialog. In other words: kill something, start talking to Ian
// while it is still falling, and it stays in the air. Vanilla never noticed because you cannot
// start a conversation mid-combat there.
// Lay a finished corpse down: what show_death (actions.cc) does at the end of a death sequence.
//
// ⚠️ ONLY show_death EVER FLATTENS A BODY, and it is a `register_object_must_call` callback riding
// on the death animation's sequence -- so any death that does not carry that sequence to its end
// leaves a corpse standing upright and BLOCKING, on an otherwise perfectly correct final frame.
// That is the "one of them is still standing" report: trace_2026-08-15_181907 has several critters
// parked at `anim=23 frame=21 flat=0` for hundreds of samples, and frame 21 of 22 is the FINISHED
// pose. Nothing was stuck -- the animation had played out and simply nobody put the body down.
//
// Mirrors show_death, including its two vanilla quirks:
//   * THE PID EXCLUSIONS. Three protos (Gizmo 16777265 among them) are deliberately never flattened
//     by vanilla and their corpses are SUPPOSED to remain upright objects -- so a standing Gizmo is
//     not this bug and must not be "fixed".
//   * OBJECT_NO_BLOCK together with OBJECT_FLAT, so the tile stops blocking movement.
// Guarded on OBJECT_FLAT because obj_toggle_flat TOGGLES -- calling it on a flat corpse stands it
// back up. Callers must ensure the death animation is actually finished (anim_busy == 0): flagging
// a critter flat mid-fall draws it with the FLOOR, which is the "dying animation shows as
// background" bug the obj-flat diagnostic exists to catch.
static bool rt_settle_corpse_flat(Object* npc)
{
    if (npc == NULL || (npc->flags & OBJECT_FLAT) != 0) {
        return false;
    }
    if (npc->pid == 16777266 || npc->pid == 16777265 || npc->pid == 16777224) {
        return false;
    }

    Rect flatRect;
    npc->flags |= OBJECT_NO_BLOCK;
    if (obj_toggle_flat(npc, &flatRect) == 0) {
        tile_refresh_rect(&flatRect, npc->elevation);
    }
    obj_turn_off_outline(npc, NULL);
    return true;
}

static bool rt_death_fall_was_cut(Object* npc)
{
    if (npc == NULL || anim_busy(npc) != 0) {
        return false;
    }

    // ⚠️ THE POSE MUST ACTUALLY BE A FALL. Without this the test read "any multi-frame art not on
    // its last frame", which ANIM_STAND satisfies permanently -- an idle loop is multi-frame and
    // parks wherever it stopped. So a critter that died and NEVER STARTED a death animation (still
    // ANIM_STAND) was treated as a fall to finish: obj_set_frame jumped it to the last frame of the
    // STANDING art and the reap flattened it on the spot. That is the "I shot a second NPC while
    // the first was dying, both animations cut, both bodies instantly on the ground" report --
    // trace_2026-08-13_181943.log has 14x `death-finish ... anim=0`, and anim 0 is ANIM_STAND.
    //
    // Same shape of mistake as the corpse-float diagnostic (2026-08-10): a condition that looked
    // like a discriminator but was true almost always. A cut fall can only be a fall.
    int anim = FID_ANIM_TYPE(npc->fid);
    if (anim < FIRST_KNOCKDOWN_AND_DEATH_ANIM) {
        return false;
    }

    CacheEntry* handle;
    Art* art = art_ptr_lock(npc->fid, &handle);
    if (art == NULL) {
        return false;
    }

    int frameCount = art_frame_max_frame(art);
    art_ptr_unlock(handle);

    return frameCount > 1 && npc->frame < frameCount - 1;
}

// Narrow form of rt_is_reacting: true for a brief cosmetic reaction that's safe
// to cut short for movement -- a standing HIT-react flinch (ANIM_HIT_FROM_FRONT/
// BACK) or a MISS dodge (ANIM_DODGE_ANIM), but NOT a knockdown fall or standup.
// Used for the player's reaction-cancel and reaction-cap: a flinch is safe to cut
// once its damage has committed (commits at swing-connect), and a dodge is a pure
// miss reaction with no damage or state change at all, so it's always safe to cut;
// whereas a knockdown is a real disabling state the player must actually sit in
// until the standup pass lifts it -- so those must never be cancelled by movement.
static bool rt_is_cancelable_reaction(Object* critter)
{
    if (critter == NULL) {
        return false;
    }

    int anim = FID_ANIM_TYPE(critter->fid);
    return anim == ANIM_HIT_FROM_FRONT
        || anim == ANIM_HIT_FROM_BACK
        || anim == ANIM_DODGE_ANIM;
}

// Cancels the dude's in-progress hit-react flinch so a movement input can take
// over immediately, instead of the player being frozen for the flinch's full
// duration. Safe because the attack's damage already committed at swing-connect
// (combat_rt_update's commit block), so the flinch tail is purely cosmetic.
// register_clear ends the (reserved, non-prioritized) sequence the flinch lives
// in; the attacker's swing already played, so only the cosmetic tail is dropped.
// Scoped to cancelable reactions (rt_is_cancelable_reaction: hit-react or miss
// dodge) so a knockdown is never cut.
static void rt_cancel_flinch_for_move(Object* critter)
{
    if (rt_is_cancelable_reaction(critter)) {
        // anim_force_clear, NOT register_clear. THE cancel never actually fired: a defender's
        // dodge / hit-react is registered INSIDE the attacker's sequence, and both attack paths
        // mark that sequence PRIORITIZED (actions.cc:750 melee, :859 ranged). register_clear
        // refuses a prioritized sequence -- it returns -2 and does nothing, and this caller
        // ignored the return -- so the player stayed frozen for the FULL dodge every single time
        // something swung at him. Traced: three of four refused movement inputs were the dude at
        // anim=13 (ANIM_DODGE_ANIM) with his dodge pending inside the rat's prioritized attack.
        // anim_force_clear exists for exactly this and is already used on attack defenders; the
        // cut swing's damage is safe because combat_rt commits from its own snapshot.
        anim_force_clear(critter);
    }
}

// A DISABLING reaction (knockdown fall / standup) -- as opposed to a brief cosmetic hit-react
// or miss dodge (rt_is_cancelable_reaction).
//
// This BLOCKS NOTHING. Its sole use is rt_play_hit_react, which skips the cosmetic stagger for a
// critter in one, because the fall/standup owns its own animation sequence and a stagger would
// override it. Attacks on such a defender are allowed and land normally -- the arbiter
// (rt_resolve_attacks) has no reaction gate at all.
static bool rt_is_heavy_reaction(Object* critter)
{
    // ⚠️ THE FID IS NOT ENOUGH ON ITS OWN. rt_is_reacting reads FID_ANIM_TYPE, so it only sees a
    // knockdown once the fall is actually PLAYING -- and action_defender_reaction registers the fall
    // a few frames before its first frame renders. In that window the critter is flagged
    // DAM_KNOCKED_DOWN but still standing, and this predicate answered "no heavy reaction".
    //
    // Its one caller is rt_play_hit_react, whose next act is anim_force_clear(defender). So a second
    // hit landing inside that window CUT THE QUEUED FALL and played a standing flinch instead: the
    // knockdown SFX still fired (it rides the attacker's sequence, actions.cc:374) but the fall was
    // never seen -- "mel bych padat, nicmene nepadam, slysim jen zvuk".
    //
    // The evidence is in the standup diag: trace_2026-08-29_163935 logged `fall=14 su=36` twice,
    // i.e. the standup pass found the dude's fid on ANIM_HIT_FROM_FRONT while he was still flagged
    // prone, so it stood him up from a pose he should never have been in.
    //
    // rt_death_just_registered is exactly the shield for this window and already covers knockdowns:
    // it is armed for every `heavyReaction`, which is DAM_DEAD | KNOCKED_DOWN | KNOCKED_OUT |
    // ON_FIRE, not deaths alone (the name predates the knockdown case). Reusing it here rather than
    // adding a second grace list keeps one source of truth for "a fall is queued but not started".
    return (rt_is_reacting(critter) && !rt_is_cancelable_reaction(critter))
        || rt_death_just_registered(critter);
}

// True for an animation that ONLY plays when a critter DIES -- the gib/blood/burned/
// electrified/melted death animations and their static-frame corpse versions. Excludes
// the recoverable knockdowns ANIM_FALL_BACK/FRONT (20/21), which also occur on a
// non-lethal knockdown. Used to catch an "undead" critter: one playing a terminal death
// anim yet not flagged dead (a death whose damage never committed -- typically a scripted
// NPC-vs-NPC kill), which combat_rt would otherwise keep treating as a live combatant,
// leaving it frozen upright in its death pose.
//
// ⚠️ NOT ANIM_FALL_BACK_SF / ANIM_FALL_FRONT_SF (48/49) EITHER, for the same reason 20/21 are out:
// they are the single frames a KNOCKED-DOWN or KNOCKED-OUT critter LIES in, alive, until it gets up.
// Counting them made undead-kill finish off anyone saved lying on the ground: a Shady Sands man
// (id=1249, tile 22728) went flat and died at the very first frame of every load of that save
// (trace_2026-10-08_143759 s=11.38 and 37.38, obj-flat <- critter_kill <- rt_update_actor).
static bool rt_is_terminal_death_anim(int anim)
{
    if (anim == ANIM_FALL_BACK_SF || anim == ANIM_FALL_FRONT_SF) {
        return false;
    }
    return (anim >= ANIM_BAD_LANDING && anim <= ANIM_FALL_FRONT_BLOOD)
        || (anim >= FIRST_SF_DEATH_ANIM && anim <= LAST_SF_DEATH_ANIM);
}

// EN-scaled flinch-immunity threshold / recovery (Endurance = stagger resistance). Higher
// EN -> shorter threshold (shrugs off suppression sooner) + longer recovery (acts longer).
static float rt_flinch_threshold(Object* o)
{
    float t = RT_FLINCH_LOCK_THRESHOLD - (stat_level(o, STAT_ENDURANCE) - 5) * RT_FLINCH_EN_THRESH_FACTOR;
    if (t < 0.4f) t = 0.4f;
    if (t > 1.5f) t = 1.5f;
    return t;
}

static float rt_flinch_recovery(Object* o)
{
    float r = RT_FLINCH_RECOVERY + (stat_level(o, STAT_ENDURANCE) - 5) * RT_FLINCH_EN_RECOV_FACTOR;
    if (r < 0.2f) r = 0.2f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

// Point at a critter's flinch-immunity state -- the dude's statics, or its RtActor's
// fields. NULL out-params if the critter isn't tracked (treated as never immune).
static void rt_flinch_state(Object* o, float** accum, float** immuneUntil)
{
    if (o == obj_dude) {
        *accum = &rt_dude_flinch_accum;
        *immuneUntil = &rt_dude_immune_until;
        return;
    }
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == o) {
            *accum = &rt_actors[i].flinch_accum;
            *immuneUntil = &rt_actors[i].immune_until;
            return;
        }
    }
    *accum = NULL;
    *immuneUntil = NULL;
}

// True while a critter is in an anti-stunlock immunity window (hits damage but don't stagger).
static bool rt_flinch_immune(Object* o)
{
    float* accum;
    float* immuneUntil;
    rt_flinch_state(o, &accum, &immuneUntil);
    return immuneUntil != NULL && rt_clock < *immuneUntil;
}

// Public: true while the critter is in its anti-stunlock immunity window. Used by actions.cc to
// also drop the MISS dodge during the window (the hit-react is already suppressed in combat_rt).
bool combat_rt_flinch_immune(Object* o)
{
    return o != NULL && rt_flinch_immune(o);
}

// Per-frame: build up continuous-hit-react time; at the EN-scaled threshold open an
// immunity window, then reset. Decays RT_FLINCH_DECAY-x faster in a gap, so only sustained
// (concurrent) fire ever trips it -- a lone AP-gated attacker never reaches the threshold,
// so ordinary fights still flinch on every hit. Symmetric: runs for the dude and every NPC.
static void rt_update_flinch_state(Object* o, float dt)
{
    float* accum;
    float* immuneUntil;
    rt_flinch_state(o, &accum, &immuneUntil);
    if (accum == NULL) {
        return;
    }
    // Count the MISS dodge (ANIM_DODGE_ANIM) toward the lock timer as well as the HIT flinch:
    // under focus-fire most incoming shots MISS, and each miss restarts the dude's dodge, which
    // was the actual stunlock (he stood there dodging 29 attackers, never reaching the hit-only
    // threshold -- 0 immunity windows in the trace). Dodge is a pure cosmetic miss reaction, so
    // once the window opens action_suppress_miss_dodge drops it too and the player can act.
    int anim = FID_ANIM_TYPE(o->fid);
    if (anim == ANIM_HIT_FROM_FRONT || anim == ANIM_HIT_FROM_BACK || anim == ANIM_DODGE_ANIM) {
        *accum += dt;
        if (*accum >= rt_flinch_threshold(o)) {
            float recov = rt_flinch_recovery(o);
            *immuneUntil = rt_clock + recov;
            *accum = 0.0f;
            // Proof the anti-stunlock fired: which critter got a breather, its EN, and how
            // long the window is. dude=1 tags the player so his own immunity is visible too.
            trace_event(TRACE_FLOW, "flinch-immune", "id=%d dude=%d en=%d recov=%.2f t=%.2f",
                o->id, o == obj_dude ? 1 : 0, stat_level(o, STAT_ENDURANCE), recov, rt_clock);
        }
    } else {
        *accum -= RT_FLINCH_DECAY * dt;
        if (*accum < 0.0f) {
            *accum = 0.0f;
        }
    }
}

// DT gate on the stagger: does a hit doing `dmg` (already post-DT/DR) THROUGH `defender`'s armor
// stagger it (interrupt its walk with a flinch), or does the armor let it shrug the hit off and keep
// moving? A hit staggers only if its through-damage reaches rt_flinch_dt_factor x the defender's
// Damage Threshold for the weapon's damage type. DT 0 (unarmored) -> threshold 0 -> any damaging hit
// staggers, exactly as before. Gates ONLY the cosmetic flinch, never damage/knockdown/death.
static bool rt_hit_staggers(Object* defender, Object* weapon, int dmg)
{
    if (dmg <= 0) {
        return false;
    }
    if (rt_flinch_dt_factor <= 0.0f || defender == NULL) {
        return true; // gate disabled -> preserve the old "every damaging hit flinches"
    }

    int damageType = item_w_damage_type(weapon);
    int dt = stat_level(defender, STAT_DAMAGE_THRESHOLD + damageType);
    if (dt > 0 && (float)dmg < dt * rt_flinch_dt_factor) {
        return false; // the armour shrugged it off (no armor of this type -> any damage passes)
    }

    // ...and through the armour, a roll scaled by how much of the bar the hit took (see
    // RT_FLINCH_CHANCE_FACTOR). Rolled exactly once per hit: this has one caller, the commit pass.
    if (rt_flinch_chance_factor <= 0.0f) {
        return true;
    }
    int maxHp = stat_level(defender, STAT_MAXIMUM_HIT_POINTS);
    float chance = maxHp > 0 ? rt_flinch_chance_factor * (float)dmg / (float)maxHp : 1.0f;
    if (chance > 1.0f) {
        chance = 1.0f;
    }
    int roll = roll_random(0, 9999);
    bool staggers = roll < (int)(chance * 10000.0f);
    trace_event(TRACE_COMBAT, "stagger-roll", "id=%d dmg=%d maxhp=%d chance=%.2f staggers=%d t=%.2f",
        defender->id, dmg, maxHp, chance, staggers ? 1 : 0, rt_clock);
    return staggers;
}

void combat_rt_set_flinch_chance(float factor)
{
    rt_flinch_chance_factor = factor < 0.0f ? 0.0f : factor;
}

float combat_rt_get_flinch_chance()
{
    return rt_flinch_chance_factor;
}

// E: drive a defender's LIGHT hit-react as ITS OWN animation sequence, decoupled from the
// attacker's swing (the flinch used to live in the attacker's swing sequence, so any
// clear/refresh of the flinch also cut the swing -- the "my shot had no animation" bug and
// the reason attacks on one target had to serialize). As the defender's own sequence, a
// latest-wins refresh here never touches any swing, so attackers fire TRULY concurrently on
// one target and every hit still visibly staggers it. Called from the commit pass for a
// light hit; the flinch itself is suppressed at its old site (actions.cc show_damage_to_-
// object) under combat_rt. Skips a gone/dead defender or one in a HEAVY reaction (a
// knockdown fall owns the animation -- don't override it with a stagger).
// Defined with the reaction layer below -- the cut that gets a stagger going needs it too.
static void rt_keep_offset(Object* o, int keepX, int keepY);

static void rt_play_hit_react(Object* attacker, Object* defender)
{
    (void)attacker; // direction TODO: front-only for now (always-present art)
    if (defender == NULL || !rt_is_combatant(defender) || rt_is_heavy_reaction(defender)) {
        return;
    }

    // Anti-stunlock: while in the immunity window (it's been continuously flinched past its
    // EN-scaled threshold), the hit still lands (damage already committed) but does NOT
    // stagger -- so the target gets to ACT instead of being flinch-locked to death under
    // focus-fire. Knockdown/death above still play; only the light stagger is suppressed.
    if (rt_flinch_immune(defender)) {
        return;
    }

    // Don't cut the defender's OWN attack swing to play a cosmetic stagger. If this critter
    // has an attack of its own still in flight, cutting it makes that attack vanish with NO
    // animation or sound even though its AP/damage already committed -- the "I fired but nothing
    // played, yet points were spent" bug (the SFX plays a few frames into the swing, so an early
    // cut eats it too). Let the swing finish; a light stagger is cosmetic and the hit's damage
    // landed regardless. (A heavy reaction -- knockdown/death -- was already handled above and
    // still interrupts.) Committing to your swing when hit mid-attack is also better feel.
    for (int i = 0; i < rt_inflight_count; i++) {
        if (rt_inflight[i].attacker == defender) {
            return;
        }
    }

    // Latest-wins: interrupt whatever the defender is mid-doing (an older hit-react, a
    // move) so this hit staggers it -- getting hit suppresses you. The sub-tile offset survives
    // the cut (see rt_keep_offset): a stagger must not teleport the body back down its own path.
    int keepX = defender->x;
    int keepY = defender->y;
    anim_force_clear(defender);
    rt_keep_offset(defender, keepX, keepY);

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "hit-react")) {
        return;
    }
    register_object_animate(defender, ANIM_HIT_FROM_FRONT, 0);
    register_end();
}

// NOTE: rt_needs_standup() and rt_bump_prone_dwell() used to sit here and they are gone.
//
// Both answered a question by INSPECTING the body -- "is this critter resting prone and idle?",
// "which of my two timer copies belongs to this critter?" -- and both got it wrong the moment a
// body was frozen on a stale fid, because critter_is_prone() reads the fid. The reaction layer
// below knows the answer instead of deriving it: see rt_reaction_tick's PRONE branch (the standup)
// and rt_reaction_note_damage (the dwell restart).

// --- REACTION OWNERSHIP -------------------------------------------------------------------------
//
// ONE place decides what a body is doing, and one place fixes it when that stops being true.
//
// WHY THIS EXISTS. Vanilla guarantees that a critter is in AT MOST ONE reaction at a time, because
// it is turn-based: one attack resolves, its reaction plays, the turn ends. Realtime broke that
// guarantee and it was never replaced -- instead every site that discovered the breakage grew its
// own guard, and each guard GUESSED the answer by reading the critter's fid. That is the whole bug
// family, and it always looks the same to the player: a body frozen part-way through an animation.
//
// The one that finally forced this: two ghouls swung at the dude 20 ms apart (trace
// 2026-09-17_142140, s=29.09 / s=29.11). Commit A knocked him down and registered the fall; commit
// B force-cleared him UNCONDITIONALLY and then registered nothing, because vanilla's
// show_damage_to_object opens with `if (!critter_is_prone(defender))` and he was already prone. Net
// result: ANIM_FALL_BACK stopped dead on frame 1, busy=0, for the full 1.7 s knockdown dwell -- the
// dude hanging in mid-topple, off the ground, until the standup pass snapped him upright.
//
// AND THE GUESSING IS SELF-DEFEATING. critter_is_prone() reads the FID. A body frozen on a fall fid
// therefore answers "I am falling" for ever, so any guard built on that predicate protects exactly
// the broken state it was written to detect. The fix cannot be another fid test.
//
// SO: an explicit kind per critter, a transition rule that says which hit may take the body, and a
// reconciler that compares intent against reality every frame and REPAIRS the difference. Nothing
// outside this layer clears a defender's animation.
//
// SCOPE (deliberate, phase 1). This replaces the DECISION layer, not the EFFECT layer. The corpse
// reap, the settle, action_defender_reaction and rt_play_hit_react all still do the actual work --
// they are years of hard-won special cases and they are not what was broken. What was broken is
// that four different places decided, independently and by guesswork, when to run them.

typedef enum RtReactionKind {
    RT_REACT_NONE = 0, // free to act
    RT_REACT_FLINCH,   // light stagger / miss dodge -- cosmetic, cancelable
    RT_REACT_FALLING,  // knockdown fall playing
    RT_REACT_PRONE,    // fall finished, lying on the ground (dwell)
    RT_REACT_STANDING, // standup playing
    RT_REACT_DYING,    // death animation playing
    RT_REACT_DEAD,     // settled corpse -- terminal
    RT_REACT_KIND_COUNT,
} RtReactionKind;

static const char* rt_react_name(int kind)
{
    switch (kind) {
    case RT_REACT_NONE:
        return "none";
    case RT_REACT_FLINCH:
        return "flinch";
    case RT_REACT_FALLING:
        return "falling";
    case RT_REACT_PRONE:
        return "prone";
    case RT_REACT_STANDING:
        return "standing";
    case RT_REACT_DYING:
        return "dying";
    case RT_REACT_DEAD:
        return "dead";
    default:
        return "unknown";
    }
}

// One entry per critter currently in (or recovering from) a reaction. Sized well above the anim
// table's 21 sequences: a blast can knock down more bodies than can animate at once, and a body
// that cannot animate is exactly the one that must not be forgotten.
#define RT_REACTION_SLOTS 64

// How long a FLINCH may hold the body before the reconciler takes it back. Deliberately the SAME
// number the dude-only cap already used (RT_FLINCH_MAX_SECONDS) rather than a new one -- this pass
// changes WHO the cap applies to, not how long a stagger lasts. It now covers every critter,
// because an NPC frozen in a flinch is the same bug and nothing was watching for it.

// How long a STANDING may run before it is considered cut. The animation itself is about 0.5 s.
#define RT_REACT_STANDUP_MAX 2.0f

// A queued reaction has not started animating yet (anim_busy is still 0 for a beat). Inside this
// window the reconciler must NOT read "not animating" as "finished or cut" -- that would tear down
// the very fall it just registered. Replaces the old rt_death_grace ring.
#define RT_REACT_START_GRACE 0.5f

// Held longer than this in any non-terminal kind and something is wrong that this layer did not
// cause. Reported once per entry, with everything needed to name the culprit.
#define RT_REACT_STUCK_SECONDS 6.0f

typedef struct RtReaction {
    Object* obj;
    unsigned char kind;
    bool adopted; // entered without a request -- a script/explosion/turn-based leftover
    bool stuckLogged; // the stuck warning is once per entry, not once per frame
    bool artSeen; // the body has actually started animating this reaction (see react-start)
    float tEnter; // rt_clock when this kind was entered
    float dwellUntil; // PRONE: when the ground dwell expires (restarted by every damaging hit)
    float groundAnchor; // PRONE: when the fall ENDED -- never restarted, so the anti-pin cap is real
    int repairs; // how many times the reconciler had to fix this body
} RtReaction;

static RtReaction rt_reactions[RT_REACTION_SLOTS];
static int rt_reaction_count = 0;

// Fight counters -- see rt_reaction_report. These are the "did the layer have to do work it should
// not have had to" numbers, and they are the point of the whole instrumentation: a play-test either
// leaves them at zero or hands over the exact line that explains why not.
static int rt_react_enters[RT_REACT_KIND_COUNT];
static int rt_react_refusals = 0;
static int rt_react_repairs = 0;
static int rt_react_stuck = 0;
static int rt_react_adoptions = 0;
static int rt_react_overflow = 0;

static RtReaction* rt_reaction_find(Object* obj)
{
    if (obj == NULL) {
        return NULL;
    }
    for (int i = 0; i < rt_reaction_count; i++) {
        if (rt_reactions[i].obj == obj) {
            return &rt_reactions[i];
        }
    }
    return NULL;
}

// Find, or take a free slot. Returns NULL only when the table is genuinely full, which is traced --
// silently dropping a reaction is how a body gets forgotten on the ground.
static RtReaction* rt_reaction_slot(Object* obj)
{
    RtReaction* r = rt_reaction_find(obj);
    if (r != NULL) {
        return r;
    }
    if (rt_reaction_count >= RT_REACTION_SLOTS) {
        rt_react_overflow++;
        trace_event(TRACE_WARN, "react-overflow", "pid=%d slots=%d t=%.2f",
            obj->pid, RT_REACTION_SLOTS, rt_clock);
        return NULL;
    }
    r = &rt_reactions[rt_reaction_count++];
    r->obj = obj;
    r->kind = RT_REACT_NONE;
    r->adopted = false;
    r->stuckLogged = false;
    r->tEnter = rt_clock;
    r->dwellUntil = -1.0f;
    r->groundAnchor = -1.0f;
    r->repairs = 0;
    return r;
}

static void rt_reaction_drop(Object* obj)
{
    for (int i = 0; i < rt_reaction_count;) {
        if (rt_reactions[i].obj == obj) {
            rt_reactions[i] = rt_reactions[--rt_reaction_count];
        } else {
            i++;
        }
    }
}

static void rt_reaction_clear_all()
{
    rt_reaction_count = 0;
    for (int i = 0; i < RT_REACT_KIND_COUNT; i++) {
        rt_react_enters[i] = 0;
    }
    rt_react_refusals = 0;
    rt_react_repairs = 0;
    rt_react_stuck = 0;
    rt_react_adoptions = 0;
    rt_react_overflow = 0;
}

// THE query. Everything that used to read the fid to work out "is this body busy reacting" asks
// here instead. A critter with no entry is simply free.
static int rt_reaction_kind(Object* obj)
{
    RtReaction* r = rt_reaction_find(obj);
    return r != NULL ? (int)r->kind : RT_REACT_NONE;
}

// True while a reaction has been registered but its first frame has not rendered yet. In that
// window anim_busy is still 0 and the fid is still the OLD pose, so every "not animating = stray"
// test in this file would otherwise tear down the reaction that was just queued.
static bool rt_reaction_starting(const RtReaction* r)
{
    return r != NULL && rt_clock - r->tEnter < RT_REACT_START_GRACE;
}

static void rt_reaction_enter(RtReaction* r, int kind, const char* why)
{
    if (r == NULL) {
        return;
    }
    int from = r->kind;
    r->kind = (unsigned char)kind;
    r->tEnter = rt_clock;
    r->stuckLogged = false;
    r->artSeen = false;
    if (kind >= 0 && kind < RT_REACT_KIND_COUNT) {
        rt_react_enters[kind]++;
    }

    // The old start-grace ring stays, and this is now its ONLY arming point. Every consumer of
    // rt_death_just_registered (rt_is_heavy_reaction, the reap's stranded-anim test) keeps working
    // unchanged, but it is fed from here instead of from the commit pass directly -- one place
    // decides that a reaction was just queued.
    if (kind == RT_REACT_FALLING || kind == RT_REACT_DYING) {
        rt_mark_death_registered(r->obj);
    }

    Object* o = r->obj;
    trace_event(TRACE_COMBAT, "react", "pid=%d id=%d %s->%s why=%s anim=%d busy=%d prone=%d t=%.2f",
        o->pid, o->id, rt_react_name(from), rt_react_name(kind), why != NULL ? why : "unknown",
        FID_ANIM_TYPE(o->fid), anim_busy(o), critter_is_prone(o) ? 1 : 0, rt_clock);
}

// THE TRANSITION RULE -- the whole fix, stated once.
//
// The question every hit asks is "may I take this body?", and until now nobody asked it: the commit
// pass force-cleared whatever the defender was doing and hoped the engine would register something
// new. When it did not (vanilla refuses a second fall on an already-prone body) the body was left
// holding the corpse of an animation.
//
// DEATH always wins -- a body dying mid-fall genuinely has to switch to its death animation, and
// show_damage_to_object's prone branch knows how to do that.
// A KNOCKDOWN wins over standing, over a flinch and over a standup (knocked down while getting up =
// back down) but NOT over a fall already in progress or a body already on the ground. That last
// clause is the bug.
// A FLINCH wins only over nothing, or over another flinch (latest-wins stagger under focus fire).
static bool rt_reaction_supersedes(int cur, int req)
{
    if (cur == RT_REACT_DEAD) {
        return false; // nothing disturbs a settled corpse
    }
    if (req == RT_REACT_DYING) {
        return cur != RT_REACT_DYING;
    }
    switch (req) {
    case RT_REACT_FALLING:
        return cur == RT_REACT_NONE || cur == RT_REACT_FLINCH || cur == RT_REACT_STANDING;
    case RT_REACT_FLINCH:
        return cur == RT_REACT_NONE || cur == RT_REACT_FLINCH;
    default:
        return false;
    }
}

// Cuts every sequence still holding `critter`. THE ONLY PLACE IN THE REACTION PATH THAT CLEARS
// ANYTHING, and it runs exclusively on an ACCEPTED transition.
//
// ALL of them, not one: anim_force_clear ends a single sequence and a critter killed mid-swing can
// be held by more than one (its own attack plus whatever else). One clear left it still owned,
// check_registry then refused the death fall, and the salvage flattened it into a blood pool with
// no fall at all (traced: id=560 killed at anim=16 frame=0, death-settle in the same instant).
// ⚠️ KEEP THE BODY WHERE IT IS WHEN A CUT LANDS ON IT. Ending a walk goes through
// obj_move_to_tile, whose obj_connect_to_tile sets obj->x = obj->y = 0 -- so a critter caught
// mid-step is SNAPPED back to the centre of the tile it was crossing, up to 32 px, in the same
// instant it is hit: "vystrelim a NPC se jakoby cukne a posune zpet". The tile is right either
// way; what is wrong is throwing away the part of the step the body had already walked.
//
// The dude got this fixed twice over (anim_move_repoint for steering, rt_cut_walk_in_place for
// stopping) and the note in anim.cc names obj_connect_to_tile as the cause -- but nobody had ever
// done it for the thing being SHOT AT, which is where it is most visible: you are looking straight
// at it when it happens.
static void rt_keep_offset(Object* o, int keepX, int keepY)
{
    if (o == NULL || (keepX == 0 && keepY == 0)) {
        return;
    }

    // Only when the cut actually zeroed it. A reaction that MOVES the body on purpose (knockback)
    // has already put it somewhere it means to be, and must not be dragged back here.
    if (o->x != 0 || o->y != 0) {
        return;
    }

    Rect r;
    obj_offset(o, keepX, keepY, &r);
    tile_refresh_rect(&r, o->elevation);
}

static void rt_reaction_take_body(Object* critter)
{
    int keepX = critter != NULL ? critter->x : 0;
    int keepY = critter != NULL ? critter->y : 0;

    for (int guard = 0; guard < 4 && anim_force_clear(critter) == 0; guard++) {
    }

    rt_keep_offset(critter, keepX, keepY);
}

// --- THE SWING MUST NOT MOVE THE BODY -----------------------------------------------------------
//
// ⚠️ ATTACK ART TRANSLATES THE BODY, AND THE ENGINE THEN THROWS THAT TRANSLATION AWAY BY RE-CENTRING
// ON THE TILE. Both halves are vanilla, and neither could be seen in turn-based, where a body only
// ever stood on a tile centre. Free movement made them visible in the worst possible place: melee.
//
//   1. Registering the swing cuts whatever the attacker was doing, and every cut runs through
//      obj_move_to_tile -> obj_connect_to_tile, which sets obj->x = obj->y = 0 (anim.cc:4702).
//   2. The animation then steps the body along the art's own hot spots (anim.cc:3609, :3636) --
//      the lunge is drawn into the frames -- and at the end of the sequence dude_stand / anim_stand
//      re-centre on the tile all over again (anim.cc:3924).
//
// In melee the dude stands pressed against his victim at the edge of his own cell, which is the
// LARGEST offset free movement can hold (half a tile, clamped in rt_fm_drive), so every single
// swing threw away half a tile and he re-closed the gap the moment the art was ours to write
// again: *"při melee se teleportuji k těm postavám"*.
//
// So the attacker's body is HELD across his own swing: the sub-tile offset is captured before the
// attack is registered, and put back whenever something zeroes it while the latch is live. The
// lunge still plays -- it is drawn into the art and nothing here touches the frames -- the body
// just ends the swing where it began it.
//
// The latch disarms itself on everything that means "this body is somewhere else on purpose": a
// tile change (a real step), a reaction taking the body (knockdown / death / flinch -- that path
// has its own rt_keep_offset), a walk or run animation (the movers own obj->x/y and seed their own
// continuity from it), and a hard deadline so nothing can hold a body for ever.
//
// This is the ATTACKER's half of the fix rt_keep_offset made for the DEFENDER. Same cause, same
// cure, opposite end of the blow.
#define RT_SWING_KEEP_MAX 16
#define RT_SWING_KEEP_SECONDS 2.0f

struct RtSwingKeep {
    Object* obj;
    int tile;
    int x;
    int y;
    float armed; // rt_clock when the swing started
    int restores; // how many times the offset actually had to be put back
};

static RtSwingKeep rt_swing_keep[RT_SWING_KEEP_MAX];

static void rt_swing_keep_clear_all()
{
    for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
        rt_swing_keep[i].obj = NULL;
    }
}

// A single body is being freed (a corpse cleaned up, a critter removed by a script): the latch
// holds a raw Object* that rt_swing_keep_tick dereferences every frame. Same hole the reaction
// table and the in-flight list each had to close here.
static void rt_swing_keep_drop(Object* o)
{
    for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
        if (rt_swing_keep[i].obj == o) {
            rt_swing_keep[i].obj = NULL;
        }
    }
}

// Captured BEFORE combat_attack registers anything (the registration is what zeroes the offset),
// armed only once the engine reports that the swing actually started.
static void rt_swing_keep_arm(Object* attacker, int tile, int keepX, int keepY)
{
    if (attacker == NULL || (keepX == 0 && keepY == 0)) {
        return; // already on its tile centre: nothing to preserve, nothing to fix
    }

    RtSwingKeep* slot = NULL;
    for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
        if (rt_swing_keep[i].obj == attacker) {
            slot = &rt_swing_keep[i];
            break;
        }
    }
    if (slot == NULL) {
        for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
            if (rt_swing_keep[i].obj == NULL) {
                slot = &rt_swing_keep[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        return; // table full: this swing simply behaves the way every swing used to
    }

    slot->obj = attacker;
    slot->tile = tile;
    slot->x = keepX;
    slot->y = keepY;
    slot->armed = rt_clock;
    slot->restores = 0;

    trace_event(TRACE_COMBAT, "swing-keep", "arm %s pid=%d tile=%d x=%d y=%d t=%.2f",
        attacker == obj_dude ? "DUDE" : "NPC", attacker->pid, tile, keepX, keepY, rt_clock);
}

// One pass per frame, ahead of the movement handlers -- so a restored offset is what free movement
// re-syncs from, instead of something it has already overwritten.
//
// ⚠️ IT REPORTS THE QUIET CASE TOO. `restores=0` on the disarm line means that swing never moved the
// body, which is the only way to tell a fixed bug from a blind spot: a measure that prints only when
// it fires can never say "this is not happening any more".
static void rt_swing_keep_tick()
{
    for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
        RtSwingKeep* slot = &rt_swing_keep[i];
        Object* o = slot->obj;
        if (o == NULL) {
            continue;
        }

        const char* done = NULL;

        if (!rt_is_critter(o) || critter_is_dead(o) || critter_is_prone(o)) {
            done = "body-gone";
        } else if (rt_reaction_kind(o) != RT_REACT_NONE) {
            done = "reaction"; // that path owns the body, and rt_keep_offset already guards it
        } else if (o->tile != slot->tile) {
            done = "moved"; // a real step happened: wherever the body is now, it means it
        } else {
            int anim = FID_ANIM_TYPE(o->fid);
            if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
                done = "walking"; // the movers own obj->x/y from here (walk_smooth_seed et al)
            } else {
                if (o->x == 0 && o->y == 0) {
                    Rect r;
                    obj_offset(o, slot->x, slot->y, &r);
                    tile_refresh_rect(&r, o->elevation);
                    slot->restores++;
                }

                if (rt_clock - slot->armed >= RT_SWING_KEEP_SECONDS) {
                    done = "timeout";
                }
            }
        }

        if (done != NULL) {
            trace_event(TRACE_COMBAT, "swing-keep",
                "done %s pid=%d why=%s restores=%d tile=%d->%d x=%d y=%d kept=%d,%d held=%.2f t=%.2f",
                o == obj_dude ? "DUDE" : "NPC", o->pid, done, slot->restores,
                slot->tile, o->tile, o->x, o->y, slot->x, slot->y,
                rt_clock - slot->armed, rt_clock);
            slot->obj = NULL;
        }
    }
}

// Is one of the dude's own swings being held right now? Read ONLY by the cam-jump diagnostic, so a
// logged world-jump says whether an attack was in progress when it happened. ⚠️ It must never steer
// the camera -- see the note in rt_follow_cam for what that cost.
#define RT_CAM_JUMP_PX 6

static bool rt_swing_keep_live()
{
    if (obj_dude == NULL) {
        return false;
    }

    for (int i = 0; i < RT_SWING_KEEP_MAX; i++) {
        if (rt_swing_keep[i].obj == obj_dude) {
            return true;
        }
    }

    return false;
}

// --- A POSE NOBODY OWNS ANY MORE ----------------------------------------------------------------
//
// ⚠️ HE CLIMBED A LADDER AND COULD NOT MOVE AGAIN. Ladders (and stairs, and anything else that goes
// through action_climb_ladder) register their sequence with ANIMATION_REQUEST_NO_STAND, which means
// exactly what it says: when the sequence ends, nothing puts the critter back to the stand pose. In
// vanilla that is harmless -- the climb is usually followed by a map change, and the next thing the
// player does re-poses him anyway. Free movement is not so forgiving: it refuses to drive a body
// whose art is not STAND / WALK / RUNNING (the art is the ownership signal, see rt_fm_drive), so a
// dude left standing in the CLIMB_LADDER pose can never walk again.
//
// ⚠️ AND IT ONLY BITES WITH EMPTY HANDS. action_climb_ladder ends with register_object_take_out when
// a weapon is drawn, and that re-poses him for free -- which is why this was never seen until a
// ladder was climbed bare-handed, and why "it works for me" would have been a real answer.
//
// So: one owner of the idle pose. A body that is idle by EVERY test we have -- nothing animating,
// not dead, not prone, no reaction pending -- and yet is standing in an art that is none of the
// three we drive, is a body nobody owns, and it goes back to standing. The sub-tile offset is
// carried across (the reset runs through obj_move_to_tile, which zeroes it) so the camera does not
// lurch at the same moment.
//
// Deliberately NOT while a conversation is up: a script that poses the dude for a cutscene has every
// right to, and the pose is the point.
#define RT_POSE_STUCK_SECONDS 0.30f

static void rt_dude_pose_reconcile(float dt)
{
    static float stuckFor = 0.0f;

    if (obj_dude == NULL || !rt_enabled || dialog_active()) {
        stuckFor = 0.0f;
        return;
    }

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    if (anim == ANIM_STAND || anim == ANIM_WALK || anim == ANIM_RUNNING
        || anim_busy(obj_dude) != 0
        || critter_is_dead(obj_dude)
        || critter_is_prone(obj_dude)
        || rt_reaction_kind(obj_dude) != RT_REACT_NONE) {
        stuckFor = 0.0f;
        return;
    }

    stuckFor += dt;
    if (stuckFor < RT_POSE_STUCK_SECONDS) {
        return;
    }
    stuckFor = 0.0f;

    trace_event(TRACE_WARN, "pose-stuck", "anim=%d fid=%08x tile=%d x=%d y=%d -> stand t=%.2f",
        anim, obj_dude->fid, obj_dude->tile, obj_dude->x, obj_dude->y, rt_clock);

    int keepX = obj_dude->x;
    int keepY = obj_dude->y;
    dude_stand(obj_dude, obj_dude->rotation, -1);
    rt_keep_offset(obj_dude, keepX, keepY);
}

// A HEAVY hit (death / knockdown / knockout / fire) asks for the body, at the commit point.
// Returns true when the transition was accepted, which is also the only case in which anything was
// cleared. The caller still runs action_defender_reaction either way -- it carries the weapon
// drop/destroy callbacks and the show_death settle, not just the animation.
static bool rt_reaction_heavy(Object* defender, int flags)
{
    if (defender == NULL || !rt_is_critter(defender)) {
        return false;
    }

    int req = (flags & DAM_DEAD) != 0 ? RT_REACT_DYING : RT_REACT_FALLING;

    RtReaction* r = rt_reaction_slot(defender);
    if (r == NULL) {
        // Table full: fall back to the old unconditional behaviour rather than silently doing
        // nothing. Worse than the new rule, but never "the hit had no effect at all".
        rt_reaction_take_body(defender);
        return true;
    }

    if (!rt_reaction_supersedes(r->kind, req)) {
        rt_react_refusals++;
        trace_event(TRACE_COMBAT, "react-refuse", "pid=%d id=%d req=%s cur=%s held=%.2f anim=%d t=%.2f",
            defender->pid, defender->id, rt_react_name(req), rt_react_name(r->kind),
            rt_clock - r->tEnter, FID_ANIM_TYPE(defender->fid), rt_clock);
        return false;
    }

    rt_reaction_take_body(defender);
    rt_reaction_enter(r, req, (flags & DAM_DEAD) != 0 ? "lethal" : "knockdown");
    return true;
}

// A LIGHT hit asks for a cosmetic stagger. Every refusal says WHY out loud -- these used to be
// silent early-returns scattered through rt_play_hit_react, and a stagger that never plays is
// indistinguishable from a hit that never landed.
static void rt_reaction_flinch(Object* attacker, Object* defender)
{
    if (defender == NULL || !rt_is_critter(defender)) {
        return;
    }

    const char* refuse = NULL;
    if (critter_is_dead(defender)) {
        refuse = "dead";
    } else if (rt_flinch_immune(defender)) {
        // Anti-stunlock: the hit still lands (damage already committed) but does not stagger, so
        // the target gets to ACT instead of being flinch-locked to death under focus fire.
        refuse = "immune";
    } else {
        // ⚠️ A RELOAD NO LONGER REFUSES THE FLINCH (2026-10-06, his pick "jen zásah, po kterém se
        // cukneš"). It used to (2026-10-04, "střelba od enemyho zruší reload? I dunno about that"),
        // but that protection never held: action_attack cleared the reloading defender at every
        // enemy's trigger pull, misses included -- the stunlock. That clear now spares a reload, so
        // the line is drawn HERE instead, where the damage is known: this function is only reached
        // for a hit that staggers (through-damage >= flinch_dt_factor x DT, rt_hit_staggers), and
        // such a hit takes the body -- the reload ends and the dude's tick refunds its 2 AP. A miss
        // or a hit the armour shrugs off never gets here, and the reload carries on.
        if (combat_rt_is_reloading(defender)) {
            trace_event(TRACE_COMBAT, "reload-broken", "pid=%d id=%d by=%d t=%.2f",
                defender->pid, defender == obj_dude ? 0 : defender->id,
                attacker != NULL ? attacker->pid : -1, rt_clock);
        }
    }

    if (refuse == NULL) {
        // Do not cut the defender's OWN swing to play a cosmetic stagger: that attack's AP and
        // damage already committed, so cutting it makes the attack vanish with no animation or
        // sound -- the "I fired but nothing played, yet points were spent" bug.
        for (int i = 0; i < rt_inflight_count; i++) {
            if (rt_inflight[i].attacker == defender) {
                refuse = "own-swing";
                break;
            }
        }
    }

    RtReaction* r = refuse == NULL ? rt_reaction_slot(defender) : rt_reaction_find(defender);
    if (refuse == NULL && r == NULL) {
        refuse = "no-slot";
    }
    if (refuse == NULL && !rt_reaction_supersedes(r->kind, RT_REACT_FLINCH)) {
        refuse = rt_react_name(r->kind);
    }

    if (refuse != NULL) {
        rt_react_refusals++;
        trace_event(TRACE_COMBAT, "react-refuse", "pid=%d id=%d req=flinch cur=%s why=%s t=%.2f",
            defender->pid, defender->id,
            rt_react_name(r != NULL ? (int)r->kind : RT_REACT_NONE), refuse, rt_clock);
        return;
    }

    rt_play_hit_react(attacker, defender);
    rt_reaction_enter(r, RT_REACT_FLINCH, "stagger");
}

// This critter's own ground dwell, in seconds. Clamped on the same 0.6..1.6 band the run-speed
// scale uses, so a silly cfg cannot make a knockdown either instant or eternal.
static float rt_knockdown_dwell_for(Object* critter)
{
    if (critter == NULL || rt_knockdown_agility_factor <= 0.0f) {
        return rt_knockdown_dwell;
    }
    float mult = 1.0f - (stat_level(critter, STAT_AGILITY) - 5) * rt_knockdown_agility_factor;
    if (mult < 0.6f) {
        mult = 0.6f;
    }
    if (mult > 1.6f) {
        mult = 1.6f;
    }
    return rt_knockdown_dwell * mult;
}

// A damaging hit on a body that is already down restarts its ground dwell, so it stays put past
// this hit instead of standing up on the old timer ("I shot him while he was on the ground and he
// got straight back up"). The anti-pin anchor deliberately does NOT move -- see groundAnchor.
static void rt_reaction_note_damage(Object* defender)
{
    RtReaction* r = rt_reaction_find(defender);
    if (r == NULL || (r->kind != RT_REACT_PRONE && r->kind != RT_REACT_FALLING)) {
        return;
    }
    r->dwellUntil = rt_clock + rt_knockdown_dwell_for(defender);
}

// Adopt a body that got into a reaction without asking us: a script, an explosion, a turn-based
// leftover, or a knockdown that arrived through a path that never reaches the commit pass. Without
// this the layer would simply not know about it and nothing would ever stand it up -- the exact bug
// rt_needs_standup was widened to cover ("anyone alive and knocked down gets back up").
static void rt_reaction_adopt(Object* critter)
{
    if (critter == NULL || !rt_is_critter(critter)) {
        return;
    }
    if (rt_reaction_find(critter) != NULL) {
        return; // already tracked
    }

    int want = RT_REACT_NONE;
    if (critter_is_dead(critter)) {
        // ⚠️ A CORPSE AT REST IS NOT DYING. Dead used to mean "adopt as DYING" unconditionally, so
        // every corpse looped for the rest of the session: the tick settled it (DYING->DEAD), the
        // retire rule dropped the DEAD entry 2 s later, the very next rt_update_actor re-adopted it,
        // and round again every ~2.5 s -- each lap re-arming the death grace ring for a body that
        // was going nowhere. trace_2026-10-08_142023: rat id=489 died at s=18.00, settled at
        // s=19.24, then `none->dying why=adopted anim=34 busy=0 prone=1` / `dying->dead
        // why=settled` every 2.5 s until the log ended; trace_2026-10-06_210502 has 1097 of them,
        // and not ONE was a body that was still animating.
        //
        // The test is exactly the DYING branch's own settle rule (not animating, prone pose), so
        // adopt can never create an entry the tick would close on its next look -- which is the
        // whole cycle. What adoption is FOR still gets in: a death registered outside this layer
        // (a script kill, an explosion) is either animating already or still in its upright pose.
        if (anim_busy(critter) == 0 && critter_is_prone(critter)) {
            return;
        }
        want = RT_REACT_DYING;
    } else if ((critter->data.critter.combat.results & DAM_KNOCKED_DOWN) != 0) {
        // Still animating -> it is the fall; already at rest -> it is lying there.
        want = anim_busy(critter) != 0 ? RT_REACT_FALLING : RT_REACT_PRONE;
    }

    if (want == RT_REACT_NONE) {
        return;
    }

    RtReaction* r = rt_reaction_slot(critter);
    if (r == NULL) {
        return;
    }
    r->adopted = true;
    rt_react_adoptions++;
    if (want == RT_REACT_PRONE) {
        r->groundAnchor = rt_clock;
        r->dwellUntil = rt_clock + rt_knockdown_dwell_for(critter);
    }
    rt_reaction_enter(r, want, "adopted");
}

// THE REPAIR. A fall that was CUT stops on a middle frame with anim_busy back at 0, and the body
// hangs there part-way through toppling over -- the "in the air" the player sees. Finish it: jump
// to the last frame, which is exactly where the fall would have ended, keeping the art's own
// landing displacement instead of snapping the body to its tile centre.
//
// This is the net, and it is on purpose. Even if some future caller outside this layer cuts a fall,
// the body still ends up lying down -- and the WARN names the body, the pose and the frame it was
// abandoned on, so the culprit is findable WITHOUT the player having to notice the glitch.
static bool rt_reaction_repair_fall(RtReaction* r)
{
    Object* o = r->obj;
    int anim = FID_ANIM_TYPE(o->fid);
    if (anim != ANIM_FALL_BACK && anim != ANIM_FALL_FRONT) {
        return false;
    }

    CacheEntry* handle;
    Art* art = art_ptr_lock(o->fid, &handle);
    if (art == NULL) {
        return false;
    }
    int frameCount = art_frame_max_frame(art);
    art_ptr_unlock(handle);

    int last = frameCount - 1;
    if (last <= 0 || o->frame >= last) {
        return false; // the fall really did finish
    }

    Rect rect;
    obj_set_frame(o, last, &rect);
    tile_refresh_rect(&rect, o->elevation);

    r->repairs++;
    rt_react_repairs++;
    trace_event(TRACE_COMBAT, "react-repair", "pid=%d id=%d kind=%s anim=%d frame=%d/%d held=%.2f t=%.2f",
        o->pid, o->id, rt_react_name(r->kind), anim, o->frame, last, rt_clock - r->tEnter, rt_clock);
    return true;
}

// Per-frame reconciler: compare what each body is SUPPOSED to be doing against what it IS doing,
// and close the gap. Replaces the two duplicated standup passes (NPC + dude) and the dude-only
// flinch cap -- one pass, everybody, including bystanders the old passes could not see.
static void rt_reaction_tick(float dt)
{
    (void)dt;

    for (int i = 0; i < rt_reaction_count;) {
        RtReaction* r = &rt_reactions[i];
        Object* o = r->obj;

        if (o == NULL) {
            rt_reactions[i] = rt_reactions[--rt_reaction_count];
            continue;
        }

        // Death overrules whatever we thought: a body can die while falling, flinching or prone.
        if (critter_is_dead(o) && r->kind != RT_REACT_DYING && r->kind != RT_REACT_DEAD) {
            rt_reaction_enter(r, RT_REACT_DYING, "died");
        }

        bool animating = anim_busy(o) != 0;
        bool starting = rt_reaction_starting(r);
        float held = rt_clock - r->tEnter;

        // ⚠️ THE GAP BETWEEN BEING SHOT AND VISIBLY REACTING, which nothing measured until now.
        // Damage commits on the frame the gun fires, but the body does not move on that frame: the
        // reaction is REGISTERED and the animation system starts it on a later tick, and this
        // layer already knew that ("anim_busy is still 0 for a beat" -- RT_REACT_START_GRACE).
        // "Strelim ho a prijde mi ze az za 0.2 sek reaguje" is a claim about exactly that beat, so
        // print it once per reaction rather than argue about it. Cosmetic kinds only: DEAD and
        // NONE are not animations anyone waits to see.
        if (!r->artSeen && animating && r->kind != RT_REACT_NONE && r->kind != RT_REACT_DEAD) {
            r->artSeen = true;
            // x/y is the sub-tile offset: a body cut mid-step used to read 0,0 here because the
            // cut zeroed it (see rt_keep_offset). Non-zero now means the step it had walked was
            // kept, which is the thing to check in play.
            trace_event(TRACE_COMBAT, "react-start", "pid=%d id=%d kind=%s delay=%.3f anim=%d x=%d y=%d t=%.2f",
                o->pid, o->id, rt_react_name(r->kind), held, FID_ANIM_TYPE(o->fid), o->x, o->y, rt_clock);
        }

        switch (r->kind) {
        case RT_REACT_FLINCH:
            // Cosmetic and bounded. Its damage committed at swing-connect, so cutting the tail
            // costs nothing, and never cutting it is what froze the player mid-stagger.
            if (!animating && !starting) {
                rt_reaction_enter(r, RT_REACT_NONE, "flinch-done");
            } else if (held >= RT_FLINCH_MAX_SECONDS) {
                anim_clear_pending(o);
                rt_reaction_enter(r, RT_REACT_NONE, "flinch-cap");
            }
            break;

        case RT_REACT_FALLING:
            if (!animating && !starting) {
                // Finished -- or cut. rt_reaction_repair_fall decides which and lands the body
                // either way, so PRONE always means "actually lying down".
                rt_reaction_repair_fall(r);
                r->groundAnchor = rt_clock;
                r->dwellUntil = rt_clock + rt_knockdown_dwell_for(o);
                // The Agility lever is invisible by nature -- you cannot see a number you did not
                // roll. Print what this body actually got, so a play-test CHECKS it instead of
                // believing it: two criters of different AGI knocked down in one fight should show
                // two different dwells here.
                trace_event(TRACE_COMBAT, "prone-dwell", "pid=%d id=%d agi=%d dwell=%.2f base=%.2f t=%.2f",
                    o->pid, o->id, stat_level(o, STAT_AGILITY),
                    r->dwellUntil - rt_clock, rt_knockdown_dwell, rt_clock);
                rt_reaction_enter(r, RT_REACT_PRONE, "landed");
            }
            break;

        case RT_REACT_PRONE: {
            // Stood up by something else (a script, a load, the engine) -- believe the world.
            if ((o->data.critter.combat.results & DAM_KNOCKED_DOWN) == 0) {
                rt_reaction_enter(r, RT_REACT_NONE, "no-longer-down");
                break;
            }
            // Knocked OUT is a different state with its own game-time recovery event
            // (EVENT_TYPE_KNOCKOUT -> critter_wake_up), so leave an NPC down and let the queue wake
            // it. The DUDE is the exception and always recovers here -- see the knockout fold in
            // combat_rt_update; a save made while he was out must never reload into a frozen dude.
            if ((o->data.critter.combat.results & DAM_KNOCKED_OUT) != 0 && o != obj_dude) {
                break;
            }
            bool dwellDone = r->dwellUntil >= 0.0f && rt_clock >= r->dwellUntil;
            bool capped = rt_knockdown_ground_cap > 0.0f && r->groundAnchor >= 0.0f
                && rt_clock - r->groundAnchor >= rt_knockdown_ground_cap;

            // THE CAP OUTRANKS anim_busy, and that is the whole point of it. The old pass waited
            // for "not animating" before standing anyone up, so a body holding a sequence that
            // never ended was pinned on the ground for ever -- and the harder you were focus-fired
            // the more reliably it happened, because every incoming hit-react re-armed the wait.
            // An anti-pin backstop that a stuck animation can veto is not a backstop. Past the cap
            // the sequence is cut and the body gets up regardless.
            if (capped && animating) {
                anim_clear_pending(o);
                animating = anim_busy(o) != 0;
                trace_event(TRACE_COMBAT, "react-repair", "pid=%d id=%d kind=prone why=cap-vs-anim anim=%d busy=%d t=%.2f",
                    o->pid, o->id, FID_ANIM_TYPE(o->fid), anim_busy(o), rt_clock);
                r->repairs++;
                rt_react_repairs++;
            }

            if ((dwellDone || capped) && !animating) {
                if (capped && !dwellDone) {
                    trace_event(TRACE_FLOW, "prone-cap", "pid=%d id=%d ground=%.2f dwellLeft=%.2f t=%.2f",
                        o->pid, o->id, rt_clock - r->groundAnchor,
                        r->dwellUntil - rt_clock, rt_clock);
                }
                rt_stand_up(o);
                rt_reaction_enter(r, RT_REACT_STANDING, capped && !dwellDone ? "cap" : "dwell");
            }
            break;
        }

        case RT_REACT_STANDING:
            if (!animating && !starting) {
                rt_reaction_enter(r, RT_REACT_NONE, "up");
            } else if (held >= RT_REACT_STANDUP_MAX) {
                // The standup was cut or never rendered. DAM_KNOCKED_DOWN is already cleared by
                // rt_stand_up, so the body is logically upright; make sure it is not left holding a
                // dead sequence, and hand control back.
                anim_clear_pending(o);
                rt_reaction_enter(r, RT_REACT_NONE, "standup-cap");
            }
            break;

        case RT_REACT_DYING:
            // The corpse reap (rt_update_actor) owns the settle -- deliberately untouched. This
            // only follows along, so the layer knows when the body is finally done.
            if (!animating && !starting && critter_is_prone(o)) {
                rt_reaction_enter(r, RT_REACT_DEAD, "settled");
            }
            break;

        case RT_REACT_DEAD:
        case RT_REACT_NONE:
        default:
            break;
        }

        // Anything still held past the bound is not this layer's doing. Say so, once, with enough
        // to find the culprit -- this is the line that means "something outside the reaction layer
        // is still touching bodies".
        if (r->kind != RT_REACT_NONE && r->kind != RT_REACT_DEAD
            && !r->stuckLogged && rt_clock - r->tEnter >= RT_REACT_STUCK_SECONDS) {
            r->stuckLogged = true;
            rt_react_stuck++;
            trace_event(TRACE_COMBAT, "react-stuck", "pid=%d id=%d kind=%s held=%.2f anim=%d busy=%d frame=%d prone=%d dead=%d t=%.2f",
                o->pid, o->id, rt_react_name(r->kind), rt_clock - r->tEnter,
                FID_ANIM_TYPE(o->fid), anim_busy(o), o->frame,
                critter_is_prone(o) ? 1 : 0, critter_is_dead(o) ? 1 : 0, rt_clock);
        }

        // Retire a settled entry so the table stays short. NONE is kept for one grace window so a
        // rapid second hit still sees the history.
        if ((r->kind == RT_REACT_NONE && rt_clock - r->tEnter > RT_REACT_START_GRACE)
            || (r->kind == RT_REACT_DEAD && rt_clock - r->tEnter > 2.0f)) {
            rt_reactions[i] = rt_reactions[--rt_reaction_count];
            continue;
        }

        i++;
    }
}

// End-of-fight summary. THIS IS THE POINT OF THE INSTRUMENTATION: the player does not have to
// notice anything. If `repairs` or `stuck` is anything but 0, a body was cut by something outside
// this layer and the WARN lines above name it. Rides rt_balance_report, which already fires once
// per fight.
static void rt_reaction_report()
{
    // WARN when this fight needed repairs, a body got stuck, or the table overflowed -- so a single
    // `grep WARN` over a whole evening's play lists exactly the fights worth looking at, and the
    // full-detail react-repair / react-stuck lines are sitting right next to each one. Otherwise
    // it is an ordinary COMBAT line: proof the layer ran and had nothing to fix.
    bool trouble = rt_react_repairs > 0 || rt_react_stuck > 0 || rt_react_overflow > 0;
    trace_event(trouble ? TRACE_WARN : TRACE_COMBAT, "react-summary",
        "session totals: flinch=%d fall=%d prone=%d stand=%d dying=%d dead=%d | refused=%d repairs=%d stuck=%d adopted=%d overflow=%d live=%d",
        rt_react_enters[RT_REACT_FLINCH], rt_react_enters[RT_REACT_FALLING],
        rt_react_enters[RT_REACT_PRONE], rt_react_enters[RT_REACT_STANDING],
        rt_react_enters[RT_REACT_DYING], rt_react_enters[RT_REACT_DEAD],
        rt_react_refusals, rt_react_repairs, rt_react_stuck, rt_react_adoptions,
        rt_react_overflow, rt_reaction_count);
}

// Tiles per continuous NPC walk segment. A single-tile move restarts the walk sad every tile,
// which drops the tail of the leg cycle (choppy) and reads as "NPCs walk weird / fast / not like
// me". Moving several clear tiles in one sad keeps the walk cycle running unbroken across tiles --
// exactly like the dude's stick movement -- so the NPC walk matches the player's.
#define RT_NPC_WALK_SEGMENT 4

// ⚠️ A CRITTER WITH NO WALK ART CANNOT MOVE, SO DO NOT ASK IT TO (2026-10-08). The Master (MAMSTR)
// has stand, attack and hit art and no walk (xB) or run (xT) at all, yet the chase FSM asked him to
// move every frame: `walk-seg rc=-1` and `move-fail no-path` ~7000 times in one ten-minute session
// (trace_2026-10-08_192731). Every ask looked up two FRMs that do not exist -- art_exists for the
// run, anim_preload for the walk -- each a miss through the patch folder and then the DAT, and on
// the Deck (Proton) a missed file is not free: the seconds he was doing it hitched 7-8x as often.
// The outcome is unchanged -- he still does not move, and the movers below report exactly what the
// failed registration reported -- it just costs one cached answer per art id instead of four disk
// misses a frame. Walk art under the bare-handed code OR the weapon it holds counts.
static bool rt_can_walk(Object* critter)
{
    static int cachedKey[64];
    static bool cachedResult[64];
    static int cachedCount = 0;

    int key = critter->fid & 0xFFFF; // art index + weapon code
    for (int i = 0; i < cachedCount; i++) {
        if (cachedKey[i] == key) {
            return cachedResult[i];
        }
    }

    int base = critter->fid & 0xFFF;
    int weaponCode = (critter->fid & 0xF000) >> 12;
    bool result = art_exists(art_id(OBJ_TYPE_CRITTER, base, ANIM_WALK, 0, critter->rotation + 1))
        || (weaponCode != 0 && art_exists(art_id(OBJ_TYPE_CRITTER, base, ANIM_WALK, weaponCode, critter->rotation + 1)));

    if (cachedCount < 64) {
        cachedKey[cachedCount] = key;
        cachedResult[cachedCount] = result;
        cachedCount++;
    }
    if (!result) {
        trace_event(TRACE_FLOW, "immobile", "id=%d pid=%d fid=%08x -- no walk art, never asked to move t=%.2f",
            critter->id, critter->pid, critter->fid, rt_clock);
    }
    return result;
}

// Queues a CONTINUOUS multi-tile chase segment from mover straight toward target (as far as the
// straight run is clear, capped at RT_NPC_WALK_SEGMENT, stopping one tile short of the target). The
// FSM re-plans when the segment ends, so it still tracks a moving target while moving smoothly.
static void rt_step_toward(Object* mover, Object* target)
{
    if (!rt_can_walk(mover)) {
        return;
    }

    int rotation = tile_dir(mover->tile, target->tile);

    // ⚠️ THE LEG STOPS WHERE IT STOPS GETTING CLOSER -- never just "on the target's tile". The run is
    // along ONE of six directions and the target is rarely exactly on that line: two tiles off in
    // the same row is a zig-zag, not a straight hop. Testing only `next == target->tile` let such a
    // leg run on for all four tiles, straight past the target and out the other side, then come back
    // the same way -- and a critter mid-leg is busy, so it cannot attack while it brushes past.
    // trace_2026-10-08_142023, rat id=560: 19507 -> 19903 beside the dude at 19505 (dist 2 -> 1 ->
    // 1 -> 3), then 19903 -> 19507 back; three hits taken, no bite. *"Krysa šla kolem mě, místo toho
    // aby na mě zaútočila."* Now a leg ends beside the target, or on the last tile that brought it
    // closer, and the FSM plans the next one from there.
    int dist = 0;
    int dest = mover->tile;
    int destDist = tile_dist(dest, target->tile);
    for (int i = 0; i < RT_NPC_WALK_SEGMENT; i++) {
        if (destDist <= 1) {
            break; // beside it: the attack decision takes over from here
        }
        int next = tile_num_in_direction(dest, rotation, 1);
        if (next == target->tile) {
            break; // don't step onto the target's tile
        }
        int nextDist = tile_dist(next, target->tile);
        if (nextDist >= destDist) {
            break; // drawn level with it: one more step on this line only goes past
        }
        if (obj_blocking_at(mover, next, mover->elevation) != NULL) {
            break; // straight run blocked here -- CHASE re-plans / pathfinds around next frame
        }
        dest = next;
        destDist = nextDist;
        dist++;
    }

    if (dist == 0) {
        // TEMP DIAG (strip once the frozen-chaser case is solved). A critter that is in CHASE,
        // not busy, and yet never changes tile for tens of seconds -- trace_2026-08-03_222723
        // id=687, 30+s on tile 16523 with a target 40 tiles away -- has to be failing HERE or in
        // rt_advance_to_target, and the two need opposite fixes. This arm means the very first
        // tile toward the target is blocked (or we are already adjacent).
        trace_event(TRACE_FLOW, "move-fail", "id=%d pid=%d reason=step-blocked tile=%d tgt=%d rot=%d t=%.2f",
            mover->id, mover->pid, mover->tile, target->tile, rotation, rt_clock);
        return; // adjacent to target or immediately blocked -- nothing to move this frame
    }

    // ⚠️ DEFER_STAND: this leg is one of many, and the FSM registers the next one the frame this one
    // ends. Standing the body in between flashed the STAND pose and restarted the stride from frame 0
    // every four tiles (anim_defer_stand in anim.cc holds it in stride instead).
    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED | ANIMATION_REQUEST_DEFER_STAND, "step")) {
        return;
    }
    // Chasing critters RUN -- while they can afford it. register_object_run_to_tile self-falls-back
    // to WALK for a critter with no ANIM_RUNNING art (radscorpions / most creatures) or a crippled
    // leg, so "humanoids sprint, animals keep walking" comes for free; a melee enemy can run down a
    // kiting player instead of walking at the player's exact pace forever (equal walk speed = the
    // gap never closed). rt_npc_sprint_ok adds the AP side: the sprint burns the same pool the
    // player's does, and drops to a walk once the pool is down to one attack's worth, so a long
    // chase tires the chaser too -- without ever leaving it unable to swing on arrival.
    // (dist mirrors the dude's own run segment -- the actionPoints arg is the straight-run length.)
    bool running = rt_npc_sprint_ok(mover);
    int rc = running
        ? register_object_run_to_tile(mover, dest, mover->elevation, dist, 0)
        : register_object_move_to_tile(mover, dest, mover->elevation, dist, 0);
    register_end();

    // TEMP DIAG (walk-in-place hunt, 2026-08-10). id=1798 sat in CHASE with anim_busy = -1 and its
    // tile UNCHANGED, taking a `unstick-move` cut every 1.25 s on a perfect metronome -- i.e. it was
    // animating a walk that never crossed a tile -- yet it logged NO move-fail at all, so routing
    // was not refusing it. That leaves this registration: either `dest` comes back as the tile the
    // critter is already standing on, or the register call fails and nothing actually moves.
    // Logging from/to/dist/rc answers which, and `run=` catches a gait that has no art.
    // STRIP once the walk-in-place case is understood.
    trace_event(TRACE_FLOW, "walk-seg", "id=%d pid=%d from=%d to=%d dist=%d run=%d rc=%d t=%.2f",
        mover->id, mover->pid, mover->tile, dest, dist, running ? 1 : 0, rc, rt_clock);
}

// Maneuvers an in-range attacker that can't strike this instant (its target is
// mid-reaction, so the arbiter would skip its attack) instead of letting it stand
// frozen -- which is what made a melee read as "everything stops" while the player
// flinched. If not yet adjacent it closes in; if adjacent it strafes one tile
// tangentially around the target, staying within weapon range, so the pack circles
// the player and keeps pressing. Side is fixed per-actor (by object id) so the
// group splits around the player rather than all sliding the same way. Falls back
// to the other side, then to a straight approach, if the preferred tile is blocked.

static void rt_reposition(Object* npc, Object* target)
{
    if (!rt_can_walk(npc)) {
        return; // see rt_can_walk -- a strafe would only ask for walk art that is not there
    }

    int range = item_w_range(npc, rt_hit_mode(npc));

    // Movement spends the engine combat.ap per tile while the warm bit is set;
    // top it up (movement is free in combat_rt's model) so the step isn't truncated.
    npc->data.critter.combat.ap = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);

    if (obj_dist(npc, target) > 1) {
        // Not adjacent yet -- close the gap (reads as an aggressive push-in).
        rt_step_toward(npc, target);
        return;
    }

    // Adjacent: strafe tangentially (60 deg off the line to the target) to circle
    // it. Try the actor's preferred side first, then the other.
    int dir = tile_dir(npc->tile, target->tile);
    int side = (npc->id & 1) != 0 ? 1 : 5; // +1 or -1 (mod 6)
    int dirs[2] = { (dir + side) % 6, (dir + 6 - side) % 6 };

    for (int i = 0; i < 2; i++) {
        int dest = tile_num_in_direction(npc->tile, dirs[i], 1);
        if (dest == target->tile) {
            continue; // never step onto the target
        }
        if (obj_blocking_at(npc, dest, npc->elevation) != NULL) {
            continue; // tile occupied/blocked
        }
        if (tile_dist(dest, target->tile) > range) {
            continue; // would drift out of weapon range
        }

        if (!rt_register_begin(ANIMATION_REQUEST_RESERVED | ANIMATION_REQUEST_DEFER_STAND, "strafe")) {
            return;
        }
        register_object_move_to_tile(npc, dest, npc->elevation, 1, 0);
        register_end();
        return;
    }
}

// True when a RANGED / THROWN attack from npc to target has no line of fire -- the
// shot would be blocked by a wall or obstacle (the same check combat_check_bad_shot
// makes before rejecting a shot as AIM_BLOCKED). Melee / unarmed always returns false
// (adjacent strikes have no LOS gate), so a melee pack is never told to "reposition".
// AP an NPC pays to reload, matching the player's gamepad-X reload (gamepad_ui.cc) and
// vanilla's reload action cost.
#define RT_RELOAD_AP_COST 2

// How long an actor may fail to route to its target before that target is written off as
// UNREACHABLE. Generous on purpose: a path can fail transiently while a doorway is briefly
// plugged by other critters, and de-aggroing on one bad frame would make packs give up.
#define RT_NO_PATH_GIVEUP_SECONDS 2.0f

// Records the outcome of a routing attempt on the actor. Success clears the timer; the first
// failure stamps it. rt_target_unreachable() then decides when to let go.
static void rt_note_path_result(RtActor* a, bool routed)
{
    if (routed) {
        a->no_path_since = -1.0f;
    } else if (a->no_path_since < 0.0f) {
        a->no_path_since = rt_clock;
    }
}

// True once routing to the current target has failed continuously for long enough that the
// target must be considered unreachable (different building, across a chasm, or simply beyond
// what Fallout's pathfinder will solve).
static bool rt_target_unreachable(RtActor* a)
{
    return a->no_path_since >= 0.0f && rt_clock - a->no_path_since >= RT_NO_PATH_GIVEUP_SECONDS;
}

// True if this critter's chosen weapon is a magazine weapon that is EMPTY. Only an empty
// magazine counts -- a partially loaded gun keeps shooting, exactly like the player's, or
// the AI would top up after every round and never actually run dry.
static bool rt_needs_reload(Object* npc, int hitMode)
{
    Object* weapon = item_hit_with(npc, hitMode);
    if (weapon == NULL || item_get_type(weapon) != ITEM_TYPE_WEAPON) {
        return false;
    }
    if (item_w_max_ammo(weapon) <= 0) {
        return false; // no magazine (melee, unarmed, thrown) -- nothing to reload
    }
    return item_w_curr_ammo(weapon) <= 0;
}

// --- NPC RELOAD ANIMATION (MODS "Reload animation", the same switch as the dude's) ---
//
// "budou všichni přebíjet?" -- yes. With the row on, a dry NPC plays its weapon's STAND loop exactly
// like the player does (gamepad_ui.cc, gpui_reload_anim_start), timed by ITS Agility through the same
// curve (compute_tpf, anim_set_reload_timing), and the magazine fills and the 2 AP are charged at the
// loop's END. A slow reload only the player had to sit through would be a handicap, not a mechanic.
//
// A cut loop -- a hit-react, a death, the chase taking the body -- never reaches the callback, so it
// loads nothing and costs nothing; the gun is still empty and the FSM simply starts it again.

// True if `npc` carries a box that fits `weapon` -- the same question item_w_try_reload answers by
// doing it, asked without loading anything.
static bool rt_npc_has_ammo_for(Object* npc, Object* weapon)
{
    int index = -1;
    Object* ammo;
    while ((ammo = inven_find_type(npc, ITEM_TYPE_AMMO, &index)) != NULL) {
        if (item_w_can_reload(weapon, ammo)) {
            return true;
        }
    }
    return false;
}

// The END of an NPC's reload loop.
static int rt_npc_reload_done(void* a1, void* a2)
{
    Object* npc = static_cast<Object*>(a1);
    Object* weapon = static_cast<Object*>(a2);
    if (npc == NULL || weapon == NULL || critter_is_dead(npc)) {
        return 0;
    }

    // Still the gun in its hands? A swap that ran in between would otherwise get loaded by proxy.
    if (inven_right_hand(npc) != weapon && inven_left_hand(npc) != weapon) {
        return 0;
    }

    combat_rt_reload_end(npc);

    if (item_w_try_reload(npc, weapon) == -1) {
        return 0;
    }

    trace_event(TRACE_FLOW, "npc-reload", "id=%d pid=%d anim=1 t=%.2f", npc->id, npc->pid, rt_clock);
    return 0;
}

// Start the loop. False when the body or the animation table is not free this frame -- the caller
// spends the beat waiting and asks again. The AP go at the START, as the dude's do.
static bool rt_npc_start_reload_anim(RtActor* a, Object* npc, Object* weapon)
{
    if (anim_busy(npc) != 0) {
        return false;
    }

    // ⚠️ register_begin's refusal must be honoured: another transaction is open, and registering
    // anyway would append this reload to somebody else's sequence.
    if (register_begin(ANIMATION_REQUEST_RESERVED) != 0) {
        return false;
    }
    register_object_reload(npc); // the weapon's STAND loop, or a pistol's holster-and-draw
    register_object_call(npc, weapon, rt_npc_reload_done, -1);

    anim_set_reload_timing(npc);
    int rc = register_end();
    anim_set_reload_timing(NULL);

    if (rc == 0) {
        a->ap -= RT_RELOAD_AP_COST;
        if (a->ap < 0) {
            a->ap = 0;
        }
        combat_rt_reload_begin(npc);
        trace_event(TRACE_FLOW, "npc-reload-anim", "id=%d pid=%d agi=%d ap=%d t=%.2f",
            npc->id, npc->pid, stat_level(npc, STAT_AGILITY), a->ap, rt_clock);
    }
    return rc == 0;
}

// THE DRY-GUN ESCAPE HATCH.
//
// rt_needs_reload + item_w_try_reload only rescue a critter that still HAS ammo. When it has none,
// nothing in realtime ever fixed the cause: vanilla's answer is ai_switch_weapons() inside
// combat_ai(), and that whole path is dead here. The comment at the reload site used to claim
// rt_hit_mode "falls through to melee on a later pass" -- it does NOT, and trace_2026-08-07_233608
// proves it: 63 WARN/shot-blocked reason=1 (COMBAT_BAD_SHOT_NO_AMMO), every one of them NPC->dude,
// and not a single reload. From the player's side that is a guard standing in front of him pulling
// the trigger of an empty gun forever -- "guard při konci boje jen koukal a nic nedělal".
//
// So do what vanilla would: take the next best weapon out of the pack FIRST, and only ball your
// fists when there is genuinely nothing left. ai_search_inven already skips the item in hand and
// any ranged weapon that is empty with no ammo to reload it, so it can never hand back another dry
// gun -- which is also what stops this from cycling.
static bool rt_swap_dry_weapon(RtActor* a, Object* npc, Object* dry)
{
    if (anim_busy(npc) != 0) {
        return false; // mid-animation; the swap can wait a frame
    }

    Object* replacement = ai_search_inven(npc, 0);
    if (replacement != NULL) {
        inven_wield(npc, replacement, 1);
        a->weapon_checked = true;
        trace_event(TRACE_FLOW, "npc-swap-weapon", "id=%d pid=%d from=%d to=%d t=%.2f",
            npc->id, npc->pid, dry != NULL ? dry->pid : -1, replacement->pid, rt_clock);
        return true;
    }

    if (dry != NULL) {
        inven_unwield(npc, 1);
        a->weapon_checked = true;
        trace_event(TRACE_FLOW, "npc-unarmed", "id=%d pid=%d dropped=%d t=%.2f",
            npc->id, npc->pid, dry->pid, rt_clock);
        return true;
    }

    return false;
}

// --- GUARDS JOIN A FIGHT THEY WITNESS ---
//
// A neutral critter had NO WAY INTO A FIGHT. rt_update_actor's RT_IDLE branch offers exactly three
// doors -- `hostile` (it is attacking us), `friendly` (it is in our party), and `engine_target` (its
// OWN whoHitMe is a live critter) -- and a town guard watching a brawl is none of them: nobody has
// hit it, so whoHitMe is NULL. It stands there. Measured in the Skum Pitt raid
// (trace_2026-08-15_155257, s=331..349): 15 critters on the map, and Killian plus every guard sat
// at `st=0 host=0 frnd=0 tgt=0` for the whole 17.8 s fight while the Skulz killed Neal.
//
// The aggro spread does not cover it either -- it only pulls in the SAME TEAM as its anchor (five
// `aggro-spread ... team=14` lines, all Skulz), which is about reinforcing a side, not about
// bystanders taking one.
//
// ⚠️ WHY NOT JUST CALL ai_danger_source(). It is exported and it is exactly this decision -- but its
// useful half, ai_find_attackers, walks `curr_crit_list` / `curr_crit_num`, the TURN-BASED
// participant array, which realtime never fills (`if (curr_crit_num == 0) return 0`). Called here it
// degenerates to "return whoHitMe", i.e. the very thing that is already NULL. So the RULE is lifted
// and re-run over combat_rt's own registry, which is realtime's equivalent of that list.
//
// The rule is vanilla's, verbatim -- ai_find_attackers' third candidate (`*a3`): a critter on MY
// team is being attacked by someone who is NOT on my team. That is "defend the one being attacked",
// which is exactly the scope chosen for this: guards protect their own, and a barman with a shotgun
// still keeps his head down.
//
// The GUARD test leans on vanilla data rather than a list of our own: AI.TXT aggression separates
// cleanly (Loser 1, Cow 5, Careful/Peasant 10, Guard 20 | Guard Humorous 33, Guard Quiet 55, Shady
// Sands Guard 60, Bouncer 70), and it is paired with "actually carrying a weapon" so an aggressive
// packet on an unarmed civilian still stays out of it.
#define RT_GUARD_JOIN_AGGRESSION 20
static int rt_guard_join_aggression = RT_GUARD_JOIN_AGGRESSION;

static bool rt_is_guard_type(Object* critter)
{
    if (rt_guard_join_aggression <= 0 || critter == NULL) {
        return false;
    }
    if (combatai_aggression(critter) < rt_guard_join_aggression) {
        return false;
    }

    // Armed, in hand or in the pack -- rt_ensure_weapon_drawn will draw it once it engages.
    return rt_hand_holds_weapon(critter) || ai_search_inven(critter, 0) != NULL;
}

// Who this guard should go after, or NULL. One perception test at most: the candidate scan is plain
// field reads, and only the single attacker it settles on is checked for line of sight.
// True if `suspect` has itself attacked someone on `team` -- i.e. it is the aggressor in this
// fight, not a victim worth avenging. Scans whoHitMe across the team, DEAD MEMBERS INCLUDED: the
// most important case is precisely the one where the aggressor already killed someone.
//
// Pointer comparisons only, never a dereference of whoHitMe, so a stale value cannot hurt us
// (rt_refresh_registry sanitises those every refresh anyway).
static bool rt_attacked_team(Object* suspect, int team)
{
    for (int i = 0; i <= rt_actor_count; i++) {
        Object* member = i == rt_actor_count ? obj_dude : rt_actors[i].obj;
        if (member == NULL || member == suspect || !rt_is_critter(member)) {
            continue;
        }
        if (member->data.critter.combat.team != team) {
            continue;
        }
        if (member->data.critter.combat.whoHitMe == suspect) {
            return true;
        }
    }
    return false;
}

static Object* rt_guard_danger_source(Object* guard)
{
    int team = guard->data.critter.combat.team;

    for (int i = 0; i <= rt_actor_count; i++) {
        // The dude is deliberately included: he is not in rt_actors (rt_refresh_registry skips
        // him), and "the player is beating up a townsperson" is the case guards most need to see.
        Object* victim = i == rt_actor_count ? obj_dude : rt_actors[i].obj;
        if (victim == NULL || victim == guard || !rt_is_combatant(victim)) {
            continue;
        }
        if (victim->data.critter.combat.team != team) {
            continue; // not one of ours
        }

        // ⚠️ DO NOT AVENGE THE AGGRESSOR. Team membership alone cannot tell "the player murdered a
        // shopkeeper" from "the player shot the man who was murdering one" -- and in Junktown those
        // two are the SAME TEAM. Gizmo's assassin walks in on Killian's side of the roster, so the
        // moment the player put him down, every guard read it as an attack on one of their own and
        // came for the player ("proč když jsem zabil assassina tak najednou po mě utočí guards").
        //
        // Both facts are on the table at once -- Killian was hit by the assassin AND the assassin
        // was hit by the player -- and the scan simply returned whichever came first in the actor
        // list. Asking whether the victim has itself attacked the team settles it in the right
        // order every time: the assassin is skipped, the next pass finds Killian, and the guards go
        // after the assassin, which is what a guard is for.
        if (rt_attacked_team(victim, team)) {
            continue;
        }

        Object* attacker = victim->data.critter.combat.whoHitMe;
        if (attacker == NULL || attacker == guard || !rt_is_combatant(attacker)) {
            continue;
        }
        if (attacker->data.critter.combat.team == team) {
            continue; // an inside scuffle -- not ours to police
        }

        if (is_within_perception(guard, attacker)) {
            trace_event(TRACE_FLOW, "guard-threat", "guard=%d victim=%d attacker=%d team=%d t=%.2f",
                guard->id, victim->id, attacker->id, team, rt_clock);
            return attacker;
        }
    }

    return NULL;
}

// --- THE MEDIC BEAT ----------------------------------------------------------------------------
//
// Vanilla's combat AI already heals: ai_check_drugs (combatai.cc:354) makes any biped below HALF
// its hit points stim ITSELF for 2 AP, plays a use-item animation and consumes the drug. Like the
// reload path, it lives inside combat_ai() -- which realtime never calls -- so until now NOTHING in
// a realtime fight ever used a healing item. Enemies and companions alike died with a pack full of
// Stimpaks. Same shape of hole as the empty-gun one rt_needs_reload had to fill.
//
// This restores that, and takes the one step vanilla never did: a critter may also treat a WOUNDED
// ALLY beside it. That is what Doc Morbid's First Aid Kit is finally for -- the same kit that used
// to break him outright (see rt_hit_mode).
//
// ⚠️ WHY THIS DOES NOT CALL protinst_use_item_on / skill_use, which already implement exactly this
// and even name PROTO_ID_FIRST_AID_KIT. Three blockers, each fatal in realtime:
//   * skill_use FADES THE SCREEN TO BLACK and back around the roll (palette_fade_to, skill.cc:493
//     and :537). Fine between turns; not while a firefight is running.
//   * skill_use_slot_available is a GLOBAL per-day budget (timesSkillUsed[skill][...]), not a
//     per-critter one -- every NPC use would eat the PLAYER's First Aid uses for the day.
//   * protinst_use_item_on returns -1 outright under isInCombat() ("You cannot do that in combat").
// It also runs scr_exec_map_update_scripts() mid-fight and prints its failure text ungated by
// `obj == obj_dude`. So we take the RULES from there -- First Aid / Doctor roll, +20 crit chance,
// the 1-in-10 chance the kit is used up -- and leave the turn-based presentation behind.
//
// The heal amount deliberately matches what skill_use would have produced for a non-dude healer:
// the Healer perk block there is gated on `obj == obj_dude`, so min/max stay 0 and the roll reduces
// to roll_random(1, 5). Identical numbers, none of the side effects.
#define RT_MEDIC_HP_FRACTION 0.5f // vanilla's `bloodied` -- max HP / 2
#define RT_MEDIC_ALLY_TILES 3
#define RT_MEDIC_COOLDOWN 6.0f
#define RT_MEDIC_AP_COST 2 // what ai_check_drugs charges
#define RT_MEDIC_KIT_SPENT_CHANCE 10 // 1-in-N the kit runs dry, as obj_use_item_on

static bool rt_medic_enabled = true;
static float rt_medic_hp_fraction = RT_MEDIC_HP_FRACTION;
static int rt_medic_ally_tiles = RT_MEDIC_ALLY_TILES;
static float rt_medic_cooldown = RT_MEDIC_COOLDOWN;

// --- "STOP PATCHING ME UP" ----------------------------------------------------------------------
//
// Per-companion opt-out, set from the dialogue (see gdialog.cc). It stops one named follower from
// spending ITS OWN supplies and 2 AP on the PLAYER; the same follower still doses itself and still
// treats other allies, because neither of those is what anyone complains about. "kokot ian furt
// healuje" -- Ian burning his stimpaks on a scratch, six seconds at a time.
//
// ⚠️ KEYED BY object->id, NOT BY THE POINTER. Objects are rebuilt on every map change and load, so
// a pointer is meaningless the moment you walk through a door; `id` is what partyMemberSave itself
// writes to identify a follower across saves, so it is the identity the rest of the game already
// trusts.
//
// The set is small on purpose -- the party caps at 20 in partyMemberList -- and a linear scan of at
// most 32 ints is nothing against a beat that already scans the whole actor registry.
#define RT_MEDIC_OPTOUT_MAX 32
static int rt_medic_optout[RT_MEDIC_OPTOUT_MAX];
static int rt_medic_optout_count = 0;

static int rt_medic_optout_slot(int id)
{
    for (int i = 0; i < rt_medic_optout_count; i++) {
        if (rt_medic_optout[i] == id) {
            return i;
        }
    }
    return -1;
}

bool combat_rt_medic_treats_player(Object* npc)
{
    return npc == NULL || rt_medic_optout_slot(npc->id) < 0;
}

void combat_rt_medic_set_treats_player(Object* npc, bool treats)
{
    if (npc == NULL) {
        return;
    }

    int slot = rt_medic_optout_slot(npc->id);
    if (treats) {
        if (slot >= 0) {
            // Swap the last entry down rather than shuffle: the set has no meaningful order.
            rt_medic_optout[slot] = rt_medic_optout[--rt_medic_optout_count];
        }
    } else if (slot < 0 && rt_medic_optout_count < RT_MEDIC_OPTOUT_MAX) {
        rt_medic_optout[rt_medic_optout_count++] = npc->id;
    }

    trace_event(TRACE_FLOW, "medic-optout", "id=%d treats=%d count=%d",
        npc->id, treats ? 1 : 0, rt_medic_optout_count);
}

void combat_rt_medic_optout_clear()
{
    rt_medic_optout_count = 0;
}

// Is there a medic beat at all? The dialogue asks before offering to switch it off for one
// follower -- an order that could not change anything is worse than no order.
bool combat_rt_medic_beat_enabled()
{
    return rt_medic_enabled && combat_rt_enabled();
}

// --- "GIVE ME SOME ROOM" ------------------------------------------------------------------------
//
// A follower WALKS a few tiles away from the dude, on the same dialogue line the medic opt-out uses.
//
// ⚠️ IT IS A WALK, NOT A PLACEMENT, AND THAT IS THE WHOLE REQUEST. interact_prompt's IP_PUSH already
// swaps the two of them with obj_move_to_tile -- an instant change of tiles, which he called what it
// is: *"takovej teleport a to je meh"*. So this registers a real move animation and lets him watch
// it happen. Nothing else about IP_PUSH changes; a shove past someone in a doorway still wants to be
// instant.
#define RT_STEP_BACK_TILES 4

// Requested from the dialogue, performed later. ⚠️ THE DIALOGUE IS MODAL: while it is up the main
// loop is not running, so an animation registered there would sit in the tables doing nothing until
// the conversation ended anyway -- and worse, gdialog's own exit path and map.cc's
// partyMemberSyncPosition both touch follower positions around that moment. Deferring to the first
// combat_rt_update after the screen closes means the walk starts in a world that is actually
// ticking, with the party already settled.
static Object* rt_step_back_pending = NULL;

void combat_rt_request_step_back(Object* npc)
{
    rt_step_back_pending = npc;
}

// Walk `npc` away from the dude. Returns true when a move was actually registered.
static bool rt_do_step_back(Object* npc)
{
    if (npc == NULL || obj_dude == NULL || npc->elevation != obj_dude->elevation) {
        return false;
    }
    if (critter_is_dead(npc) || critter_is_prone(npc)) {
        return false;
    }
    if (npc->tile == obj_dude->tile) {
        return false; // no direction to be "away"
    }

    // Straight out along the line the dude is already pushing them off, then fanning outwards: in a
    // corridor -- which is exactly where a follower is in the way -- the direct line is usually the
    // one blocked, and the two neighbours are the doorway you actually want them to back through.
    int away = tile_dir(obj_dude->tile, npc->tile);
    static const int fan[5] = { 0, 1, -1, 2, -2 };

    for (int f = 0; f < 5; f++) {
        int dir = (away + fan[f] + ROTATION_COUNT) % ROTATION_COUNT;

        // Longest first: "a few steps" is the point, and a single tile reads as a twitch rather
        // than as getting out of the way.
        for (int dist = RT_STEP_BACK_TILES; dist >= 1; dist--) {
            int dest = tile_num_in_direction(npc->tile, dir, dist);
            if (dest == npc->tile || dest < 0) {
                continue;
            }
            if (obj_blocking_at(npc, dest, npc->elevation) != NULL) {
                continue;
            }

            // Drop whatever it had queued, or an in-flight follow step finishes from the old tile
            // and drags it straight back -- the same trap ip_push_companion documents.
            anim_clear_pending(npc);

            if (!rt_register_begin(ANIMATION_REQUEST_UNRESERVED | ANIMATION_REQUEST_DEFER_STAND, "step-back")) {
                return false;
            }
            // ⚠️ register_end() ON THE FAILURE PATH TOO, NEVER register_clear. That function
            // refuses on short sequences (see the ANIM_STAND deadlock this project already paid
            // for), and every other mover in this file simply ends the sequence whatever the
            // registration returned. An empty sequence is harmless; a half-open one is not.
            bool queued = register_object_move_to_tile(npc, dest, npc->elevation, -1, 0) != -1;
            register_end();
            if (!queued) {
                return false;
            }

            trace_event(TRACE_FLOW, "step-back", "id=%d dir=%d dist=%d dest=%d",
                npc->id, dir, dist, dest);
            return true;
        }
    }

    trace_event(TRACE_FLOW, "step-back", "id=%d refused=nowhere-to-go", npc->id);
    return false;
}

int combat_rt_medic_optout_save(DB_FILE* stream)
{
    if (db_fwriteInt(stream, rt_medic_optout_count) == -1) {
        return -1;
    }
    for (int i = 0; i < rt_medic_optout_count; i++) {
        if (db_fwriteInt(stream, rt_medic_optout[i]) == -1) {
            return -1;
        }
    }
    return 0;
}

// ⚠️ A SHORT READ IS "NOT IN THIS SAVE", NOT AN ERROR. This chunk was appended to the save format,
// so every save written before it simply ends where the interface chunk ends. Returning -1 there
// would refuse to load every existing save in the game. Defaults are already correct (an empty
// set = everyone heals you), so a save with no chunk needs nothing but a clean exit.
int combat_rt_medic_optout_load(DB_FILE* stream)
{
    rt_medic_optout_count = 0;

    int count;
    if (db_freadInt(stream, &count) == -1) {
        return 0; // older save: no chunk
    }
    if (count < 0 || count > RT_MEDIC_OPTOUT_MAX) {
        return 0; // not ours / corrupt -- fall back to the default rather than refuse the load
    }

    for (int i = 0; i < count; i++) {
        int id;
        if (db_freadInt(stream, &id) == -1) {
            rt_medic_optout_count = 0;
            return 0;
        }
        rt_medic_optout[i] = id;
    }
    rt_medic_optout_count = count;
    return 0;
}

typedef enum RtHealKind {
    RT_HEAL_NONE,
    RT_HEAL_DRUG, // Stimpak / Super Stimpak -- applied via item_d_take_drug, always consumed
    RT_HEAL_KIT, // First Aid Kit / Doctor's Bag -- a skill roll, consumed only on the 1-in-10
} RtHealKind;

// Hurt enough to be worth a dose? Mirrors ai_check_drugs' `bloodied` test, plus the two refusals
// skill_use makes for real (a corpse, and a ROBOTIC body -- which it scores as an automatic
// failure rather than a heal, so treating one would just burn the item).
static bool rt_medic_needs_care(Object* c)
{
    if (c == NULL || !rt_is_critter(c) || critter_is_dead(c)) {
        return false;
    }
    if (critter_body_type(c) == BODY_TYPE_ROBOTIC) {
        return false;
    }

    int maxHp = stat_level(c, STAT_MAXIMUM_HIT_POINTS);
    if (maxHp < 1) {
        return false;
    }

    return (float)stat_level(c, STAT_CURRENT_HIT_POINTS) < rt_medic_hp_fraction * (float)maxHp;
}

// The best healing item this critter carries, or NULL. Drugs are preferred over kits: a Stimpak is
// a guaranteed heal, a kit is a skill roll that can fail outright.
//
// inven_find_type walks the whole inventory list, and a WIELDED item is still in that list (see
// inven_right_hand, which finds the hand by scanning it for OBJECT_IN_RIGHT_HAND) -- so this
// correctly finds the kit Morbid is holding, not just one stowed in his pack.
static Object* rt_find_healing_item(Object* medic, RtHealKind* kind)
{
    *kind = RT_HEAL_NONE;

    int index = -1;
    Object* item;
    while ((item = inven_find_type(medic, ITEM_TYPE_DRUG, &index)) != NULL) {
        if (item->pid == PROTO_ID_STIMPACK || item->pid == PROTO_ID_SUPER_STIMPACK) {
            *kind = RT_HEAL_DRUG;
            return item;
        }
    }

    index = -1;
    while ((item = inven_find_type(medic, ITEM_TYPE_MISC, &index)) != NULL) {
        if (item->pid == PROTO_ID_FIRST_AID_KIT || item->pid == PROTO_ID_DOCTORS_BAG) {
            *kind = RT_HEAL_KIT;
            return item;
        }
    }

    return NULL;
}

// Who should this medic treat? Itself first -- your own bleeding out is more urgent than a
// squadmate's, and it is also the vanilla behaviour we are restoring -- then the WORST-hurt ally
// standing within rt_medic_ally_tiles.
//
// ⚠️ NO WALKING TO THE PATIENT, deliberately. Reaching an ally would need a destination that isn't
// the combat target, i.e. a whole new state in the FSM, and "NPC walks somewhere instead of
// fighting" is the exact failure mode that has cost this project two regressions already
// (rt_target_unreachable, the crowd special-case). A medic treats who is already next to it; if
// nobody is, it fights. Widen rt_medic_ally_tiles before adding movement.
static Object* rt_medic_patient(Object* medic)
{
    if (rt_medic_needs_care(medic)) {
        return medic;
    }

    Object* worst = NULL;
    float worstFrac = 1.0f;

    // ⚠️ THE DUDE IS NOT IN rt_actors -- rt_refresh_registry skips him by design. Iterating the
    // registry alone would therefore quietly exclude the single most interesting patient in the
    // game: a companion patching YOU up is the visible half of this feature. So he is considered
    // explicitly, on the same team test as everyone else (party followers share his team).
    for (int i = 0; i <= rt_actor_count; i++) {
        Object* other = i == rt_actor_count ? obj_dude : rt_actors[i].obj;
        if (other == NULL || other == medic) {
            continue;
        }
        if (other->data.critter.combat.team != medic->data.critter.combat.team) {
            continue;
        }
        if (obj_dist(medic, other) > rt_medic_ally_tiles || !rt_medic_needs_care(other)) {
            continue;
        }
        // Told to stop, in dialogue. Only the PLAYER is refused: this medic still doses itself
        // (that is the branch above) and still treats other allies, neither of which costs the
        // player anything he asked to stop.
        if (other == obj_dude && !combat_rt_medic_treats_player(medic)) {
            continue;
        }

        int maxHp = stat_level(other, STAT_MAXIMUM_HIT_POINTS);
        float frac = (float)stat_level(other, STAT_CURRENT_HIT_POINTS) / (float)maxHp;
        if (worst == NULL || frac < worstFrac) {
            worst = other;
            worstFrac = frac;
        }
    }

    return worst;
}

// Consumes the item, taking it out of the hand first if that is where it was. Skipping the unwield
// would leave inven_right_hand pointing at a destroyed object -- and rt_hit_mode reads that hand
// every frame.
static void rt_medic_consume(Object* medic, Object* item)
{
    if (item == inven_right_hand(medic)) {
        inven_unwield(medic, 1);
    }

    if (item_remove_mult(medic, item, 1) == 0) {
        obj_connect(item, medic->tile, medic->elevation, NULL);
        obj_destroy(item);
    }
}

// One healing action. Returns true when it consumed this actor's beat (so the caller skips the
// attack FSM this frame), false when there was nothing to do.
//
// Costs the same RT_MEDIC_AP_COST vanilla charges, and pays it out of the same pool attacks spend --
// so patching someone up genuinely trades against shooting, which is the whole reason it is
// interesting rather than free healing.
static bool rt_try_medic(RtActor* a, Object* npc)
{
    if (!rt_medic_enabled || a->ap < RT_MEDIC_AP_COST || rt_clock < a->heal_next_at) {
        return false;
    }
    if (critter_body_type(npc) != BODY_TYPE_BIPED) {
        return false; // ai_check_drugs' own gate: no rat is opening a first aid kit
    }

    // Item first: most critters carry nothing, and bailing here skips the patient search entirely.
    // rt_medic_patient scans every registered actor, and this runs per actor per frame.
    RtHealKind kind;
    Object* item = rt_find_healing_item(npc, &kind);
    if (item == NULL) {
        return false;
    }

    Object* patient = rt_medic_patient(npc);
    if (patient == NULL) {
        return false;
    }

    int healed = 0;
    bool consumed = false;

    if (kind == RT_HEAL_DRUG) {
        // item_d_take_drug applies the drug's whole stat package to the PATIENT (it takes the
        // critter as its first argument), so this works on an ally exactly as it does on self.
        // It is removed first because take_drug can queue delayed effects against the object.
        if (item_remove_mult(npc, item, 1) != 0) {
            return false;
        }
        if (item_d_take_drug(patient, item) == -1) {
            item_add_force(npc, item, 1); // refused -- give it back rather than lose it
            return false;
        }
        obj_connect(item, npc->tile, npc->elevation, NULL);
        obj_destroy(item);
        consumed = true;
        healed = -1; // the drug's own tables decide; nothing meaningful to report here
    } else {
        int skill = item->pid == PROTO_ID_DOCTORS_BAG ? SKILL_DOCTOR : SKILL_FIRST_AID;
        int howMuch = 0;
        int roll = skill_result(npc, skill,
            stat_level(npc, STAT_CRITICAL_CHANCE) + 20, &howMuch);

        if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
            healed = roll_random(1, 5);
            critter_adjust_hits(patient, healed);
        }

        // The supplies run out on the same 1-in-10 obj_use_item_on rolls, hit or miss.
        if (roll_random(1, RT_MEDIC_KIT_SPENT_CHANCE) == 1) {
            rt_medic_consume(npc, item);
            consumed = true;
        }
    }

    if (patient == obj_dude) {
        intface_update_hit_points(true); // the bar only ever shows HIS hit points
    }

    a->ap -= RT_MEDIC_AP_COST;
    a->heal_next_at = rt_clock + rt_medic_cooldown;

    // Show it. Without an animation a heal is invisible -- the critter simply skips a beat and the
    // player reads it as the AI stalling, which is precisely the complaint that started this.
    // ANIM_MAGIC_HANDS_MIDDLE is what ai_magic_hands plays; routed through the table-full-safe
    // wrapper because ai_magic_hands' raw register_begin is unsafe under a saturated anim table.
    // Bracketed like the dude's quick-use: an NPC's injection is timed by its own (AGI + INT + PE) / 3
    // (compute_tpf, 2026-10-06), so a dim, clumsy medic is slower with the needle too.
    if (rt_register_begin(ANIMATION_REQUEST_RESERVED, "medic")) {
        register_object_animate(npc, ANIM_MAGIC_HANDS_MIDDLE, 0);
        anim_set_injection_timing(npc);
        register_end();
        anim_set_injection_timing(NULL);
    }

    trace_event(TRACE_FLOW, "medic",
        "id=%d pid=%d patient=%d self=%d kind=%d item=%d healed=%d consumed=%d hp=%d/%d ap=%d t=%.2f",
        npc->id, npc->pid, patient->id, patient == npc ? 1 : 0, (int)kind, item->pid,
        healed, consumed ? 1 : 0,
        stat_level(patient, STAT_CURRENT_HIT_POINTS), stat_level(patient, STAT_MAXIMUM_HIT_POINTS),
        a->ap, rt_clock);

    return true;
}

static bool rt_shot_blocked(Object* npc, Object* target, int hitMode)
{
    Object* weapon = item_hit_with(npc, hitMode);
    if (weapon == NULL) {
        return false;
    }
    int attackType = item_w_subtype(weapon, hitMode);
    if (attackType != ATTACK_TYPE_RANGED && attackType != ATTACK_TYPE_THROW) {
        return false;
    }
    return combat_is_shot_blocked(npc, npc->tile, target->tile, target, NULL);
}

// Pathfinding advance toward the target OBJECT (routes AROUND walls/buildings and
// stops adjacent), capped at a few tiles so line-of-sight is re-evaluated after each
// short hop. Used when a ranged NPC has the distance but no line of fire: it must
// reposition to get an angle, which rt_step_toward (a single straight step toward the
// target) can't do around a building -- it would just bump the wall and stand there
// re-attempting a blocked shot (the "guards stood outside and never came" bug). This
// mirrors the engine's ai_move_closer, which register_object_move_to_object's toward
// the target on a COMBAT_BAD_SHOT_AIM_BLOCKED.
// Returns TRUE if a move was actually registered, FALSE if the engine could not build a path
// to the target. The caller uses that to give up on an UNREACHABLE target -- see the
// no_path_since handling in RT_CHASE.
// TEMP DIAG (STRIP with chase-move). WHICH of the two chase movers ran, emitted only when the
// branch CHANGES.
//
// ⚠️ THE ONE FACT THE PING-PONG TRACES NEVER CARRIED. step-blocker already says WHAT refused the
// straight step and chase-why says the distance and range, but neither says which mover actually
// executed -- and the whole symptom is the two disagreeing frame to frame. The player sees a
// critter close, back off, close again; the log shows make-path alternating between a long route to
// him and a short one sideways, with no way to tell which branch produced which.
//
// Three previous attempts to fix the movement layer were reverted for making engagement worse, each
// one guessing at the cause. This is the measurement that has to come first.
//
// mover: 1 = rt_step_toward (straight, one hop), 2 = rt_advance_to_target because the SHOT is
// blocked, 3 = rt_advance_to_target because the straight STEP is blocked.
static void rt_note_chase_move(RtActor* a, Object* npc, int mover, int blockerPid, int stepDest)
{
    if (a->diag_mover == mover) {
        return;
    }
    a->diag_mover = mover;

    trace_event(TRACE_FLOW, "chase-move",
        "id=%d pid=%d mover=%d tile=%d step=%d blk=%d tgt=%d dist=%d t=%.2f",
        npc->id, npc->pid, mover, npc->tile, stepDest, blockerPid,
        a->target != NULL ? a->target->tile : -1,
        a->target != NULL ? obj_dist(npc, a->target) : -1, rt_clock);
}

static bool rt_advance_to_target(Object* npc, Object* target)
{
    // No walk art = no route, exactly what the failed registration below used to report, minus
    // the two missed file lookups and the move-fail line every frame (see rt_can_walk).
    if (!rt_can_walk(npc)) {
        return false;
    }

    // Movement spends the engine combat.ap per tile while the warm bit is set; top it
    // up (movement is free in combat_rt's model) so the hop isn't truncated short.
    npc->data.critter.combat.ap = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED | ANIMATION_REQUEST_DEFER_STAND, "reposition")) {
        return true; // table full, not a routing failure -- don't count it against the target
    }
    // RUN (self-falls-back to WALK with no run art), same as rt_step_toward, and gated on the same
    // attack reserve. This is the mover used when the straight step is blocked -- i.e. pathfinding
    // AROUND obstacles / through a melee scrum, which is most of a real chase -- so without running
    // here a chasing humanoid kept walking whenever it had to route around anything and never
    // actually ran down the kiting player.
    int rc = rt_npc_sprint_ok(npc)
        ? register_object_run_to_object(npc, target, RT_REPOSITION_STEP_TILES, 0)
        : register_object_move_to_object(npc, target, RT_REPOSITION_STEP_TILES, 0);
    anim_reset_last_path();
    register_end();

    // ⚠️ AND THE ROUTE ITSELF. rc only says the move was QUEUED; make_path runs as it starts, inside
    // register_end, and a 0-step result kills it there without a word (see anim_last_path_steps).
    // Reading rc alone, a critter boxed in a room counted every doomed attempt as "routed", so the
    // unreachable give-up below never armed and it re-ran a full path search every frame, forever.
    if (rc == 0 && anim_last_path_steps(npc) == 0) {
        rc = -1;
    }

    // rc != 0 means the engine could not build a PATH to the target. CONFIRMED as the frozen
    // chaser's cause: trace_2026-08-03_235230 logged 3277 of these (rc=-1, id=687 pinned on
    // tile 16523 with its target 39 tiles off) and ZERO step-blocked. Fallout's pathfinder
    // gives up over long distances / unreachable areas, so the critter stood in CHASE forever
    // re-attempting a route that can never exist -- once per frame, which is also why this
    // fired 3277 times in one session.
    if (rc != 0) {
        trace_event(TRACE_FLOW, "move-fail", "id=%d pid=%d reason=no-path rc=%d tile=%d tgt=%d dist=%d t=%.2f",
            npc->id, npc->pid, rc, npc->tile, target->tile, obj_dist(npc, target), rt_clock);
        return false;
    }
    return true;
}

// True if the critter has a FIREARM in its right hand -- a ranged weapon that consumes
// ammo. Thrown weapons (rocks/spears/grenades), melee, and unarmed are excluded: they
// aren't loud enough to alert onlookers through a wall (see rt_hears_gunfire).
static bool rt_wields_firearm(Object* c)
{
    if (c == NULL) {
        return false;
    }
    Object* w = inven_right_hand(c);
    if (w == NULL || item_get_type(w) != ITEM_TYPE_WEAPON) {
        return false;
    }
    if (item_w_max_ammo(w) <= 0) {
        return false; // no magazine -> not a firearm
    }
    return item_w_subtype(w, HIT_MODE_RIGHT_WEAPON_PRIMARY) == ATTACK_TYPE_RANGED;
}

// True if a same-team onlooker should be pulled into the fight by the SOUND of gunfire
// -- i.e. a firearm is involved in the anchor's fight (the anchor or whoever it is
// fighting is shooting) and the onlooker is within earshot, scaled by its Perception,
// with NO line of sight required. This is what makes guards behind a wall respond to
// shooting; a silent melee fight does not trigger it. Complements is_within_perception
// (the vanilla sight-primary check) in the aggro spread.
static bool rt_hears_gunfire(Object* onlooker, Object* anchor)
{
    if (!rt_wields_firearm(anchor)
        && !rt_wields_firearm(anchor->data.critter.combat.whoHitMe)) {
        return false;
    }
    int per = stat_level(onlooker, STAT_PERCEPTION);
    return obj_dist(onlooker, anchor) <= per * RT_GUNSHOT_HEAR_PER_MULT;
}

// Turn a standing critter to a new facing and repaint it. Only ever called on a body nothing is
// animating (rt_idle_look_around checks), so there is no sequence to fight over the rotation.
static void rt_look_set_rotation(Object* npc, int rotation)
{
    if (rotation < 0 || rotation >= ROTATION_COUNT || rotation == npc->rotation) {
        return;
    }

    Rect rect;
    if (obj_set_rotation(npc, rotation, &rect) == 0) {
        tile_refresh_rect(&rect, npc->elevation);
    }
}

// IDLE HOSTILES LOOK AROUND (2026-10-04 -- the user's pick of four options: "A, ať se rozhlížejí").
//
// Vanilla's sight is a FACING test. can_see (actions.cc) is nothing but "within one hex-direction of
// where I am turned" -- no walls, no distance -- and is_within_perception grants PE*5 tiles inside
// that cone but only PE outside it (PE*2 while combat is warm). A critter that stands still never
// turns, so an enemy with its back to you stayed blind to you until you were nearly touching it,
// however long you stood in plain view: "protivníci reagují až když na ně někdo začne střílet...
// hráč k nim musí dojít". Turn-based hid it -- once a fight started, every critter joined through
// the AI whichever way it faced.
//
// Two things now turn an idle HOSTILE (never anyone else -- the townsfolk keep staring where they
// stare, and their scripts see what they always saw):
//   - a GUNSHOT from the player's side within earshot (PE * RT_GUNSHOT_HEAR_PER_MULT, the radius the
//     aggro spread already uses) turns it toward the shot, a beat later;
//   - otherwise it looks around on its own every 2.5-5 s, one or two facings at a time.
// The perception rule itself is untouched. Creeping up behind one still works, for a few seconds at
// a time; the blind spot just stops being permanent.
static void rt_idle_look_around(RtActor* a, Object* npc, Object* tgt)
{
    if (!rt_look_around || tgt == NULL || npc->elevation != tgt->elevation) {
        return;
    }

    // Never over anything the animation system owns -- a fidget, a reaction, a knockdown.
    if (anim_busy(npc) != 0 || critter_is_prone(npc)) {
        return;
    }

    int pe = stat_level(npc, STAT_PERCEPTION);

    // 1. A SHOT IT HEARD. Queued, not instant: a head snapping round on the very frame of the bang
    // reads as the game cheating, a beat later reads as hearing.
    if (rt_noise_clock > a->look_noise_seen && rt_noise_elev == npc->elevation && rt_noise_tile != -1) {
        a->look_noise_seen = rt_noise_clock;
        if (tile_dist(npc->tile, rt_noise_tile) <= pe * RT_GUNSHOT_HEAR_PER_MULT) {
            a->look_noise_tile = rt_noise_tile;
            a->look_noise_at = rt_clock + 0.3f + roll_random(0, 4) * 0.1f;
        }
    }

    if (a->look_noise_at >= 0.0f) {
        if (rt_clock >= a->look_noise_at) {
            int toward = tile_dir(npc->tile, a->look_noise_tile);
            trace_event(TRACE_FLOW, "look-noise", "id=%d pid=%d rot=%d->%d dist=%d t=%.2f",
                npc->id, npc->pid, npc->rotation, toward, tile_dist(npc->tile, a->look_noise_tile), rt_clock);
            rt_look_set_rotation(npc, toward);
            a->look_noise_at = -1.0f;
            // Hold that look before scanning on: it turned to see something, let it see.
            a->look_next_at = rt_clock + 3.0f + roll_random(0, 20) * 0.1f;
        }
        return;
    }

    // 2. LOOKING AROUND -- only while somebody of ours is close enough to be seen at all. Even
    // facing straight at us it sees no further than PE*5, so turning beyond that changes nothing.
    if (obj_dist(npc, tgt) > pe * 5) {
        return;
    }

    if (rt_clock < a->look_next_at) {
        return;
    }

    static const int steps[4] = { -2, -1, 1, 2 };
    int rotation = (npc->rotation + steps[roll_random(0, 3)] + ROTATION_COUNT) % ROTATION_COUNT;
    rt_look_set_rotation(npc, rotation);
    a->look_next_at = rt_clock + 2.5f + roll_random(0, 25) * 0.1f;
}

// What stands in the way FOR GOOD: walls, scenery, a door it cannot open -- never another body. A
// doorway jammed by the critter's own side for a second is not "unreachable", and the latch an
// unreachable verdict arms holds for RT_UNREACHABLE_RETRY_SECONDS.
static Object* rt_static_blocking_at(Object* obj, int tile, int elevation)
{
    Object* blocker = obj_blocking_at(obj, tile, elevation);
    if (blocker != NULL && FID_TYPE(blocker->fid) == OBJ_TYPE_CRITTER) {
        return NULL;
    }
    return blocker;
}

// ⚠️ SEEING SOMEONE IS NOT ENOUGH TO GO AFTER THEM: the critter must be able to hit them from where
// it stands, or a route must exist (2026-10-05, "supermutanti jsou mimo mapu, když se koukneš").
//
// Fallout's maps keep reinforcements in holding pens out in the black, off the playable area, and
// rely on vanilla never moving them: vanilla AI moves only along make_path, and out of a pen there
// is no path. Ours chases with a STRAIGHT step first and asks the pathfinder only once that step is
// blocked -- and the black between the rooms has nothing in it to block a step. In MSTRLR34 six
// Nightkin in the pen north of the throne room heard the shooting (the aggro spread hears through
// rock), turned toward it (look-around) and "saw" the dude through it (vanilla sight is a facing
// test, no walls), then walked straight out across the void until the throne room's outer wall
// stopped them -- and stood there in the dark, outside the map, re-running a failing route every
// frame. trace_2026-10-05_001014, ids 33-48.
//
// The route is asked for once, against static geometry only (rt_static_blocking_at). A critter that
// fails it is latched exactly like one whose route vanished mid-chase, so it does not ask again
// every frame -- and it stays where the map put it, which is what vanilla would have done.
static bool rt_can_reach_or_shoot(Object* npc, Object* tgt)
{
    int mode = rt_hit_mode(npc);
    if (obj_dist(npc, tgt) <= item_w_range(npc, mode) && !rt_shot_blocked(npc, tgt, mode)) {
        return true;
    }

    return make_path_func(npc, npc->tile, tgt->tile, NULL, 0, rt_static_blocking_at) > 0;
}

// ⚠️ SCRIPT COMBAT TURNS (2026-10-05, his call: "Ano, všem"). Vanilla hands every critter's script a
// combat event each turn, from combat_turn and combat_add_noncoms -- neither of which realtime runs,
// so for the whole life of this mode those events never came:
//   - fixed_param 4, THE START OF ITS TURN, to every critter in the fight (combat.cc, combat_turn).
//     37 vanilla scripts act on it. The Master calls one Nightkin of his horde (Hord.int) every
//     other turn of his; super mutants in the Vats and the Military Base raise the base alert
//     after three turns (GVAR 146); townsfolk flip their town's state and cost karma after a few;
//     the Overseer restocks his ammo; and some take the turn away from the AI (script_overrides) --
//     the Glow's sentry robots while the power is off, Mr. Handy in the Vats, Jarvis, Morbid.
//   - fixed_param 5, "DO YOU JOIN?", to every critter NOT in the fight (combatai_want_to_join, each
//     round). No script tests that value, but an old single-entry script (all of it in `start`)
//     runs whole on any event, and that is how the horde ever hears the Master: Hord.int checks
//     map_var 4 whenever it runs, and during a fight this is the only thing that runs it.
//
// A "turn" is the time a critter takes to refill a full AP pool at its own Agility -- the realtime
// meaning of a turn's worth of action points, ~5 s for the Master and his Nightkin. Someone who
// JOINS the fight gets a turn at once, as combat_add_noncoms gives one.
//
// ONE SCRIPT PER FRAME, on purpose: a script can move, create or kill critters, and removing one
// compacts rt_actors (combat_rt_notify_critter_removed), so nothing here may touch a second actor
// after a script has run. Joiners first, then whoever is most overdue; at 60 fps that drains a
// whole town in well under a second.
#define RT_SCRIPT_TURN_MIN_SECONDS 2.0f
#define RT_SCRIPT_TURN_MAX_SECONDS 12.0f

static float rt_script_turn_period(Object* critter)
{
    float ap = (float)stat_level(critter, STAT_MAXIMUM_ACTION_POINTS);
    if (ap < 1.0f) {
        ap = 1.0f;
    }

    float period = ap * rt_regen_interval(critter) / (rt_speed_mult > 0.01f ? rt_speed_mult : 1.0f);
    if (period < RT_SCRIPT_TURN_MIN_SECONDS) {
        period = RT_SCRIPT_TURN_MIN_SECONDS;
    } else if (period > RT_SCRIPT_TURN_MAX_SECONDS) {
        period = RT_SCRIPT_TURN_MAX_SECONDS;
    }
    return period;
}

// THE SCRIPT HAS THIS TURN -- the AI waits. Both cases are vanilla's order of events:
//  - it has just joined the fight and its script has not had its turn start yet: vanilla runs
//    combat_p_proc BEFORE the AI decides anything, so ours waits the frame or two that takes;
//  - its script took the turn (script_overrides): vanilla skips the AI for that turn, so ours
//    stands down until the script's next turn says otherwise.
//
// Keyed on script_combatant, which only the tick sets -- and the tick always fires a joiner first,
// so this hold resolves within frames. (Keying it on the raw state instead could hold a critter the
// tick will never call a joiner: one flagged DISENGAGING still chasing out its grace period would
// have stood frozen in CHASE, holding the whole fight warm.)
static bool rt_script_turn_holds(RtActor* a)
{
    if (!rt_script_turns || !rt_engaged || a->obj == NULL || a->obj->sid == -1) {
        return false;
    }

    if (a->script_hold_until > rt_clock) {
        return true;
    }

    return a->script_combatant && !a->script_fighting;
}

static void rt_script_turn_tick()
{
    if (!rt_engaged || !rt_script_turns) {
        // Between fights: the next one starts from its first turn again, like a new vanilla combat.
        for (int i = 0; i < rt_actor_count; i++) {
            rt_actors[i].script_turn_at = -1.0f;
            rt_actors[i].script_hold_until = 0.0f;
            rt_actors[i].script_fighting = false;
            rt_actors[i].script_combatant = false;
            rt_actors[i].script_turns = 0;
        }
        return;
    }

    // TURN BOUNDARIES FOR CRITTERS WITHOUT A SCRIPT. They get no script event, but they still have
    // turns in vanilla, and damageLastTurn ("hurt since my last turn") is cleared at the end of each
    // of them (combat_turn). rt_wants_to_stop and the re-join rules read it, so without this an
    // unscripted critter hit once would count as "just hit" for the rest of the fight. No script
    // runs here, so it costs none of the one-per-frame budget below.
    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* c = a->obj;
        if (c == NULL || c->sid != -1 || a->state == RT_IDLE
            || (c->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
            continue;
        }
        if (a->script_turn_at < 0.0f) {
            a->script_turn_at = rt_clock + rt_script_turn_period(c);
        } else if (rt_clock >= a->script_turn_at) {
            a->script_turn_at = rt_clock + rt_script_turn_period(c);
            c->data.critter.combat.damageLastTurn = 0;
        }
    }

    // WHO IS IN THE FIGHT, as vanilla's combat list has it: whoever has fought in it, until it ends.
    // A combatant that loses sight of you still takes its turns in vanilla (combat_sequence only drops
    // the unconscious and the DISENGAGING), so it keeps its script turns here too -- the town that
    // counts four turns of fighting counts them the same way. Exactly `== DISENGAGING` drops out, the
    // same test combat_sequence makes; that is what a script's critter_stop_attacking leaves behind.
    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (a->obj == NULL) {
            continue;
        }
        if (a->state != RT_IDLE) {
            a->script_combatant = true;
        }
        if (a->obj->data.critter.combat.maneuver == CRITTER_MANEUVER_DISENGAGING) {
            a->script_combatant = false;
        }
    }

    RtActor* due = NULL;
    bool dueJoined = false;
    float dueAt = 0.0f;
    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* npc = a->obj;

        // Who vanilla's combat list would hold: a scripted, visible critter on the player's
        // elevation that is neither dead nor unconscious (combatai_want_to_join's own early-outs).
        if (npc == NULL || npc->sid == -1 || (npc->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }
        if (obj_dude == NULL || npc->elevation != obj_dude->elevation) {
            continue;
        }
        if ((npc->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
            continue;
        }

        bool joined = a->script_combatant && !a->script_fighting;
        float at = a->script_turn_at;
        if (!joined && at > rt_clock) {
            continue;
        }

        if (due == NULL || (joined && !dueJoined) || (joined == dueJoined && at < dueAt)) {
            due = a;
            dueJoined = joined;
            dueAt = at;
        }
    }

    if (due == NULL) {
        return;
    }

    // Everything the trace and the bookkeeping need is taken BEFORE the script runs: it may destroy
    // its own critter, and after that `npc` may only be compared, never read.
    Object* npc = due->obj;
    int sid = npc->sid;
    int id = npc->id;
    int pid = npc->pid;
    bool fighting = due->script_combatant;
    int param = fighting ? 4 : 5;
    float period = rt_script_turn_period(npc);
    due->script_fighting = fighting;
    due->script_turn_at = rt_clock + period;
    due->script_turns += 1;
    int turns = due->script_turns;
    float nextAt = due->script_turn_at;

    scr_set_objs(sid, NULL, NULL);
    scr_set_ext_param(sid, param);
    exec_script_proc(sid, SCRIPT_PROC_COMBAT);

    bool overrides = false;
    Script* script;
    if (scr_ptr(sid, &script) != -1) {
        overrides = script->scriptOverrides != 0;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == npc) {
            rt_actors[i].script_hold_until = (param == 4 && overrides) ? nextAt : 0.0f;

            // damageLastTurn is "hurt since the end of my last turn" in vanilla -- combat_turn clears
            // it at the end of every combatant's turn, and nothing clears a non-combatant's until the
            // fight ends. Same here: a combatant's turn boundary is this event. The flee and re-join
            // rules read it (toldToStop, wouldOnlyLeave in RT_IDLE).
            if (param == 4) {
                npc->data.critter.combat.damageLastTurn = 0;
            }
            break;
        }
    }

    trace_event(TRACE_FLOW, "script-turn", "id=%d pid=%d param=%d n=%d overrides=%d next=%.1fs t=%.2f",
        id, pid, param, turns, overrides ? 1 : 0, period, rt_clock);
}

// ⚠️ OUT OF BREATH, IT WIPES ITS FOREHEAD (2026-10-06). The gesture lives in the STAND loop of three
// animation sets -- bare hands, pistol, spear -- and the pistol's was the "reload" until that became a
// holster-and-draw ("pistole 10mm má jen otírání hlavy jako reload?"). Too good to throw away
// ("přijde mi to strašná škoda, to nevyužít"), so it is now what being winded LOOKS like, his pick:
// the dude when his pool runs dry and the lamps go red (rt_move_blocked), a fleeing critter that ran
// itself dry (flee_blocked), a chaser in its breather. It is an ordinary STAND, so the movers cut it
// the moment the dude may walk again (rt_dude_is_fidgeting) -- it never holds anyone up.
//
// Only bipeds: an animal's or a robot's STAND is not a gesture of a hand to a head. Returns 1 when it
// started, 0 when the body is busy or down (ask again), -1 when this critter has no wipe to play.
static int rt_wipe_forehead(Object* critter, const char* why)
{
    int code = (critter->fid & 0xF000) >> 12;
    if ((code != WEAPON_ANIMATION_NONE && code != WEAPON_ANIMATION_PISTOL && code != WEAPON_ANIMATION_SPEAR)
        || critter_body_type(critter) != BODY_TYPE_BIPED) {
        return -1;
    }

    if (critter_is_prone(critter) || anim_busy(critter) != 0) {
        return 0;
    }

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "wipe")) {
        return 0;
    }
    register_object_animate(critter, ANIM_STAND, 0);
    if (register_end() != 0) {
        return 0;
    }

    trace_event(TRACE_FLOW, "wipe", "id=%d pid=%d why=%s code=%d t=%.2f",
        critter == obj_dude ? 0 : critter->id, critter->pid, why, code, rt_clock);
    return 1;
}

// ⚠️ FLEEING, AS VANILLA DOES IT (2026-10-05: "udělej i ten útěk jako ve vanille"). Realtime had no
// flight at all: a FLEEING critter only dropped out of the chase (rt_wants_to_stop) and stood where
// it was, and the next frame's perception picked it straight back up.
//
// Vanilla, every turn, before anything else (combat_ai): a critter that is FLEEING, hurt the way its
// AI packet cannot stand, or below the packet's min_hp RUNS -- straight away from the dude, up to a
// turn's worth of AP in tiles (ai_run_away) -- and never attacks while it does (ai_attack refuses).
// Once it is max_dist from the dude it stops and leaves the fight (DISENGAGING; it stays out until
// it is hit again or the fight ends -- see toldToStop and the bookends in combat_rt_update).
// Cornered, it does nothing that turn and looks again the next.
//
// The decisions are vanilla's own code (combatai_flee_reason / combatai_flee_tile); only the timing
// is realtime: the next leg starts the moment the last one ends (this only runs on a free body), at
// realtime running speed -- the speed it chased you with -- and a cornered critter looks again one
// turn later (rt_script_turn_period), not every frame. Returns true when this critter is fleeing,
// i.e. its AI must do nothing else this frame.
static bool rt_flee_step(RtActor* a, Object* npc)
{
    int why = combatai_flee_reason(npc);
    if (why == 0) {
        a->fleeing = false;
        a->flee_blocked = false;
        return false;
    }

    a->fleeing = true;

    if (rt_clock < a->flee_retry_at) {
        return true; // cornered, or the last leg would not route: holds, and never attacks
    }

    static const char* reasons[] = { "none", "fleeing", "hurt", "min-hp" };
    int from = npc->tile;
    int dist = obj_dist(npc, obj_dude);

    // ⚠️ THE SAME BUDGET AS THE DUDE (2026-10-05, "ať je to férové jako ve vanille"). A leg is as many
    // tiles as it has AP RIGHT NOW -- vanilla's ai_run_away runs combatData->ap tiles, what the turn
    // has left, not a fresh maximum -- and the drain in rt_update_actor bills the run on the dude's
    // rates down to 0 (no attack reserve: it is not going to attack). Dry, it stands where the leg
    // ended, exactly as the dude stops on rt_move_blocked, and goes again once it has regenerated to
    // the dude's resume_ap (clamped to its own maximum, the dude's clamp too). Legs are never cut
    // mid-run: cutting snaps the body back to its tile, and a leg sized by the pool overruns it by
    // only a step or two.
    int resumeAp = rt_run_resume_ap;
    int maxAp = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);
    if (resumeAp > maxAp) {
        resumeAp = maxAp;
    }

    if (a->flee_blocked) {
        if (a->ap < resumeAp) {
            return true; // catching its breath -- and still not attacking
        }
        a->flee_blocked = false;
        trace_event(TRACE_FLOW, "flee-recovered", "id=%d pid=%d ap=%d dist=%d t=%.2f",
            npc->id, npc->pid, a->ap, dist, rt_clock);
    }

    int ap = a->ap;
    if (ap < 1) {
        a->flee_blocked = true;
        trace_event(TRACE_FLOW, "flee-winded", "id=%d pid=%d resume=%d dist=%d t=%.2f",
            npc->id, npc->pid, resumeAp, dist, rt_clock);
        rt_wipe_forehead(npc, "flee-winded"); // the body is free here: rt_flee_step only runs on one
        return true;
    }

    int dest = combatai_flee_tile(npc, ap);

    if (dest == -2) {
        trace_event(TRACE_FLOW, "flee-done", "id=%d pid=%d dist=%d maxDist=%d t=%.2f",
            npc->id, npc->pid, dist, combatai_max_dist(npc), rt_clock);
        a->state = RT_IDLE;
        a->target = NULL;
        a->no_path_since = -1.0f;
        a->unperceived_since = -1.0f;
        a->fleeing = false;
        a->flee_blocked = false;
        return true;
    }

    if (dest == -1) {
        // Truly cornered only with a FULL pool -- vanilla only ever asked at the start of a turn,
        // with every AP in hand; that one waits a turn. With a part pool the nearer tiles may simply
        // be all walled in, so it looks again in half a second, with more AP to reach further.
        a->flee_retry_at = rt_clock + (ap >= maxAp ? rt_script_turn_period(npc) : 0.5f);
        trace_event(TRACE_FLOW, "flee-cornered", "id=%d pid=%d why=%s tile=%d dist=%d t=%.2f",
            npc->id, npc->pid, reasons[why], from, dist, rt_clock);
        return true;
    }

    // anim.cc bills the ENGINE combat.ap one per tile while the combat bit is set and truncates the
    // run at 0 -- vanilla's own per-tile rule. Set to the leg's AP, it caps the leg at exactly the
    // budget it was sized from; the realtime pool pays through the drain in rt_update_actor.
    npc->data.critter.combat.ap = ap;

    // Same sequence as ai_run_away: the "run" taunt (a chance roll; when it lands it is the first
    // description, so the run is not necessarily started inside register_end), then the run. The
    // route itself was already proven by combatai_flee_tile's make_path, so only a refused
    // registration is a failure -- and it must not be re-planned on the very next frame, thirty
    // candidate searches at a time.
    // DEFER_STAND: legs follow legs, and a body stood between two of them flashed its STAND pose
    // (anim_defer_stand in anim.cc).
    register_begin(ANIMATION_REQUEST_RESERVED | ANIMATION_REQUEST_DEFER_STAND);
    combatai_msg(npc, NULL, AI_MESSAGE_TYPE_RUN, 0);
    register_object_run_to_tile(npc, dest, npc->elevation, ap, 0);
    int rc = register_end();

    if (rc == -1) {
        a->flee_retry_at = rt_clock + 0.5f;
    }

    trace_event(TRACE_FLOW, "flee", "id=%d pid=%d why=%s from=%d to=%d ap=%d dist=%d hp=%d rc=%d t=%.2f",
        npc->id, npc->pid, reasons[why], from, dest, ap, dist,
        stat_level(npc, STAT_CURRENT_HIT_POINTS), rc, rt_clock);
    return true;
}

// Starts a single attack through the reused combat pipeline, then SNAPSHOTS the computed attack
// into the in-flight list and frees the global main_ctd immediately so other attacks can start
// the same/next frame (concurrency).
// Returns true only if the engine actually STARTED the attack. combat_attack() returns -1 when
// it can't (compute_attack failed, or action_attack couldn't register the swing -- e.g. the
// defender's own animation was occupying the anim table). On that failure no damage/AP was
// applied and combat_cleanup_enabled was NOT set, so the caller must NOT charge its AP pool; it
// keeps its intent and retries next frame.
static bool rt_do_attack(Object* attacker, Object* defender, int hitMode, int hitLocation)
{
    if (rt_inflight_count >= RT_MAX_INFLIGHT) {
        return false; // in-flight list full -- caller keeps intent, retries next frame
    }

    if (defender == NULL) {
        return false;
    }

    combat_state |= COMBAT_STATE_0x01;

    // Realtime has no "initiating attack" context; clear the turn-based one so
    // combat_attack() doesn't apply stale damage/AP bonuses.
    gcsd = NULL;

    // Keep the engine AP maxed so the AP sub-checks inside combat_check_bad_shot /
    // combat_attack never truncate -- combat_rt gates AP with its OWN pool (rt_player_ap /
    // RtActor.ap), not this field.
    attacker->data.critter.combat.ap = stat_level(attacker, STAT_MAXIMUM_ACTION_POINTS);

    // Refuse an invalid shot the same way turn-based does: no line-of-sight to the target
    // (shooting THROUGH A WALL), an empty gun (NO AMMO), out of range, target already dead, or
    // crippled arms. The realtime fire path only distance-checked before, so you could shoot
    // through walls and with no ammo. Melee is unaffected (no ammo / LOS gates for adjacent
    // strikes). On a bad shot, don't fire and don't charge AP -- the caller keeps its intent.
    bool aiming = hitLocation != HIT_LOCATION_UNCALLED;
    int badShot = combat_check_bad_shot(attacker, defender, hitMode, aiming);
    if (badShot != COMBAT_BAD_SHOT_OK) {
        trace_event(TRACE_WARN, "shot-blocked", "%s->pid=%d reason=%d t=%.2f",
            attacker == obj_dude ? "DUDE" : "NPC", defender->pid, badShot, rt_clock);

        // An empty weapon silently refuses to fire every frame, so the player can't
        // tell why their shots do nothing (the target never reacts because nothing is
        // actually fired). Nudge them to reload -- throttled so it doesn't spam.
        if (attacker == obj_dude && badShot == COMBAT_BAD_SHOT_NO_AMMO) {
            static float rt_last_no_ammo_msg = -1000.0f;
            if (rt_clock - rt_last_no_ammo_msg >= 1.0f) {
                rt_last_no_ammo_msg = rt_clock;
                // Play the weapon's empty "click" so the player HEARS they're dry --
                // a combat-log line alone is easy to miss mid-fight.
                // ⚠️ NO CONSOLE LINE HERE. The dry-fire SOUND is the signal -- it is vanilla's own,
                // it is instant, and it cannot be missed mid-fight; a sentence in the message window
                // on top of it was just noise ("at to nevypisuje takové zbytečnosti do konzole").
                // The trace still records it, so "why did nothing happen" stays answerable.
                Object* emptyWeapon = item_hit_with(attacker, hitMode);
                if (emptyWeapon != NULL) {
                    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_OUT_OF_AMMO, emptyWeapon, hitMode, NULL);
                    gsound_play_sfx_file(sfx);
                }
                trace_event(TRACE_COMBAT, "out-of-ammo", "pid=%d hm=%d t=%.2f",
                    attacker->pid, hitMode, rt_clock);
            }
        }

        return false;
    }

    // Attacks animate at normal engine speed: the AP pool alone paces them (a
    // costlier attack drains more of the pool and waits longer to refill), so
    // there's no cooldown-derived slow-down to apply.

    // Where the attacker's body stands BEFORE anything is registered. The registration itself cuts
    // his current sequence, and that cut zeroes obj->x/y, so this is the last moment at which the
    // sub-tile offset still exists. See rt_swing_keep_arm.
    int swingKeepTile = attacker != NULL ? attacker->tile : -1;
    int swingKeepX = attacker != NULL ? attacker->x : 0;
    int swingKeepY = attacker != NULL ? attacker->y : 0;
    int rc = combat_attack(attacker, defender, hitMode, hitLocation);

    // THE "ENEMY NEVER ACTUALLY ATTACKS" DEADLOCK.
    //
    // combat_attack's first act is action_attack -> `register_clear(attacker) == -2 -> -1`, and
    // register_clear refuses on ANY live PRIORITIZED sequence that merely MENTIONS the attacker.
    // anim_busy() does not agree with it: anim.cc:800 makes anim_busy skip a length-1 ANIM_STAND
    // sequence outright. So an actor can read "not busy" by every test we have and still be
    // refused by register_clear, every single frame, with nothing to show for it.
    //
    // Traced live (trace_2026-08-22_204421): a hostile next to the dude produced 54 consecutive
    // `attack-fail rc=-1 defBusy=0 atkBusy=0 atkBlk=0 dist=1 range=1 oor=0` over 17 seconds --
    // combat running, enemy adjacent, and not one swing, sound or hit. From the player's side the
    // fight simply does not happen.
    //
    // So: if the engine refused while every busy-test says the attacker is idle, the blocker is a
    // sequence he does not actually owe anything to. Force it out and try once more. Guarded to an
    // attacker that is genuinely idle and neither prone nor dead -- a dying critter's prioritized
    // sequence is its OWN death and carries the show_death callback, which anim_force_clear would
    // flush early (that is the "corpse floating in the air" bug, see rt_settle_corpse_flat).
    if (rc != 0 && anim_busy(attacker) == 0 && !critter_is_prone(attacker) && !critter_is_dead(attacker)) {
        // Two different things can be wrong, and they need opposite fixes -- so check the
        // GLOBAL gate first. register_begin refuses outright while a register_begin
        // transaction is still open, and that state is not per-actor: it kills every attack
        // in the game at once, for the dude and every NPC alike. Which is exactly what the
        // trace showed after a mid-session load -- 196 DUDE and 544 NPC failures with the
        // animation table nearly EMPTY (animOcc 0..2/64) and every busy-test at zero.
        // loadsave.cc calls neither anim_stop() nor anim_reset(), so an open transaction
        // survives the load and nothing ever closes it.
        int openTx = -1;
        int inStop = 0;
        anim_debug_gate(&openTx, &inStop);

        if (openTx != -1 && anim_discard_open_transaction()) {
            trace_event(TRACE_WARN, "anim-tx-stuck", "discarded set=%d inStop=%d t=%.2f",
                openTx, inStop, rt_clock);
        } else {
            // Per-actor blocker instead: a sequence the attacker owes nothing to. anim_busy
            // skips a length-1 ANIM_STAND sequence (anim.cc) while register_clear does not,
            // so an actor can read idle and still be refused.
            anim_force_clear(attacker);
        }

        rc = combat_attack(attacker, defender, hitMode, hitLocation);
        trace_event(TRACE_WARN, "attack-unblock", "%s->pid=%d rc=%d openTx=%d inStop=%d t=%.2f",
            attacker == obj_dude ? "DUDE" : "NPC",
            defender != NULL ? defender->pid : -1, rc, openTx, inStop, rt_clock);
    }

    if (rc == 0) {
        // The swing started: hold the body where it stood, for as long as it lasts (rt_swing_keep_arm).
        rt_swing_keep_arm(attacker, swingKeepTile, swingKeepX, swingKeepY);

        trace_event(TRACE_COMBAT, "attack", "%s aid=%d ->pid=%d did=%d hm=%d t=%.2f",
            attacker == obj_dude ? "DUDE" : "NPC", attacker != NULL ? attacker->id : -1,
            defender != NULL ? defender->pid : -1, defender != NULL ? defender->id : -1,
            hitMode, rt_clock);
    } else {
        // combat_attack returns -1 from TWO different places and they mean opposite things:
        // compute_attack() refusing because the target is out of weapon range, or action_attack()
        // failing to register the animation (the "defender is busy" case). defBusy alone cannot
        // tell them apart -- it is just whatever the defender happened to be doing -- so log the
        // range check too: dist > range means it was REACH, not busy-ness. Without this the two
        // are indistinguishable and it is easy to blame the wrong one.
        int dfDist = defender != NULL ? obj_dist(attacker, defender) : -1;
        int dfRange = item_w_range(attacker, hitMode);
        // ATTACKER state too. action_attack's very first line is
        // `register_clear(attacker) == -2 -> return -1`, so the attacker's OWN lingering
        // prioritized sequence refuses the attack just as readily as the defender's -- and logging
        // only defBusy makes that invisible and pins the blame on the defender. atkBlk is
        // anim_busy_blocking: 0 there with atkBusy != 0 means the attacker's own work is spent but
        // his old sequence is still alive, which is exactly the case register_clear rejects.
        // ⚠️ NAME THE SEQUENCE THAT HOLDS HIM. atkBusy=-1 with atkBlk=0 says "something still
        // mentions the attacker but none of the remaining work is his" -- which is the whole story
        // except for the one thing that decides what to do about it: WHICH sequence, and what is
        // left in it. The dump prints the cursor and every description as #index owner/kind/anim,
        // with D for the attacker and o for anyone else. Measured 2026-09-19: three bursts of
        // refusals, 0.3-1.5 s each, and no way to tell from the log whether the holder was the
        // victim's death, its flinch, or something with no business being there at all.
        char seqDump[220];
        anim_debug_blocking_dump(attacker, seqDump, sizeof(seqDump));

        trace_event(TRACE_WARN, "attack-fail",
            "%s->pid=%d rc=%d defBusy=%d atkBusy=%d atkBlk=%d dist=%d range=%d oor=%d %s t=%.2f",
            attacker == obj_dude ? "DUDE" : "NPC", defender != NULL ? defender->pid : -1,
            rc, defender != NULL ? anim_busy(defender) : -9,
            anim_busy(attacker), anim_busy_blocking(attacker),
            dfDist, dfRange, (dfDist >= 0 && dfDist > dfRange) ? 1 : 0, seqDump, rt_clock);
    }

    if (rc != 0) {
        // The attack never started -- nothing pending to snapshot.
        return false;
    }

    // Snapshot the computed attack into the in-flight list and free the global pending slot
    // immediately, so the next attacker (incl. the player vs an enemy) can compute this very
    // frame. The snapshot commits later, from its own copy, at swing-connect / the cap.
    RtInFlight* f = &rt_inflight[rt_inflight_count++];
    combat_take_pending_snapshot(&f->ctd, &f->callDisplay);
    f->attacker = attacker;
    f->defender = defender;
    f->started = rt_clock;
    f->playerApCost = 0; // set by the player-fire path; NPC attacks charge at fire as before
    f->tAnimStart = -1.0f;
    f->tAnimEnd = -1.0f;
    f->projectile = action_take_explosive_projectile(&f->explosionFid);
    combat_clear_pending_attack();

    // TEMP DIAG (phantom-attack hunt): combat_attack returned success but the attacker's swing
    // animation never started (anim_busy still 0) -> it was dropped at registration (e.g. sad[]
    // table saturation). That commits immediately (swingDone) with no visible anim/sound. Strip
    // once the "fired with no anim/sound but AP spent" report is resolved.
    if (attacker == obj_dude && anim_busy(attacker) == 0) {
        int occ = -1, nr = -1, curr = -9;
        anim_debug_table(&occ, &nr, &curr);
        trace_event(TRACE_WARN, "phantom-fire", "DUDE anim didn't start animOcc=%d t=%.2f",
            occ, rt_clock);
    }
    return true;
}

// The dude's current hit mode (selected weapon mode, falling back to the primary
// right-hand attack).
static int rt_player_hit_mode()
{
    int hitMode;
    bool aiming;
    if (intface_get_attack(&hitMode, &aiming) == -1) {
        hitMode = HIT_MODE_RIGHT_WEAPON_PRIMARY;
    }
    return hitMode;
}

// The body part a plain attack goes to: the part bound in the LB called-shot window while the
// weapon slot shows its AIMED mode (vanilla's bullseye), otherwise none.
//
// ⚠️ THE AIMED SHOT IS THE SLOT'S MODE AGAIN (2026-10-09), his idea: *"ta hra má mířivé střely, ne?
// jako v tom políčku u zbraní a hráč i vidí kolik stojí, tím získáme navíc jeden button RB"*. It
// was RB's own held button; now it is a firing mode the way the original shows and prices it
// (LT+Y cycles single -> aimed -> burst, binding a part in the LB window switches to it), RT fires
// it, and RB became the one-press heal. The cost is item_w_mp_cost(.., aiming) on both sides: the
// slot prints it, the arbiter charges it (`aiming = loc != HIT_LOCATION_UNCALLED`).
static int rt_player_called_loc(int hitMode)
{
    int mode;
    bool aiming;
    if (intface_get_attack(&mode, &aiming) == -1 || !aiming) {
        return HIT_LOCATION_UNCALLED;
    }
    if (!item_w_called_shot(obj_dude, hitMode)) {
        return HIT_LOCATION_UNCALLED;
    }
    return gamepad_ui_aim_part();
}

// Steps the dude one tile in the given rotation. Used by WASD (key events have
// no reliable release signal, so keyboard stays on single steps). Tops up AP
// first (anim.cc truncates moves at ap <= 0 while in combat) and never starts a
// move while one is already animating.
// True when the dude is "busy" only with its idle fidget. dude_fidget plays
// register_object_animate(ANIM_STAND) and, for the dude specifically, pairs it with a
// character SFX -- a length-2 sequence, which anim_busy reports as busy (a normal length-1
// ANIM_STAND is skipped there, which is why other critters' fidgets don't block). Without
// this the player has to wait out the whole scratch/shift idle before he can move or act.
// Real animations (attack, walk, hit-react) have a non-STAND FID_ANIM_TYPE, so they're never
// mistaken for the fidget and never cut here.
static bool rt_dude_is_fidgeting()
{
    return obj_dude != NULL
        && anim_busy(obj_dude) != 0
        && FID_ANIM_TYPE(obj_dude->fid) == ANIM_STAND;
}

// A knocked-down (or still-falling) dude must finish the knockdown -- fall, ground dwell, then the
// standup animation -- before he can act again. Without this the player's own attack/move input
// registers a fire/step animation the instant the fall ends, which pre-empts the standup pass
// (rt_needs_standup gates on anim_busy==0): the dude snapped from the fall straight into firing with
// NO standup animation, still flagged KNOCKED_DOWN (log: anim 20 -> anim 45 fire, prone=1 throughout,
// no FLOW/standup ever emitted). Blocking his input here lets rt_stand_up run unopposed so the
// getting-up animation actually plays. True through fall + ground dwell; false once the standup
// registers (rt_stand_up clears the flag and moves him to anim 36/37, outside the prone range).
static bool rt_dude_incapacitated()
{
    if (obj_dude == NULL) {
        return false;
    }

    if (critter_is_prone(obj_dude)) {
        return true;
    }

    // ⚠️ THE STANDUP ANIMATION ITSELF IS NOT COVERED BY critter_is_prone (2026-08-24).
    //
    // critter_is_prone is `DAM_KNOCKED_* flag || anim in FIRST..LAST_KNOCKDOWN_AND_DEATH_ANIM
    // (20..35)`. rt_stand_up clears DAM_KNOCKED_DOWN on the SAME frame it registers the animation,
    // and ANIM_BACK_TO_STANDING (37) / ANIM_PRONE_TO_STANDING (36) sit OUTSIDE that range -- so the
    // gate fell open the instant the getting-up started, while it still had its whole run to play.
    //
    // Measured (trace_2026-08-24_194502): standup registered at s=19.78 (rcA=0, anim 37 frame 0 at
    // s=19.80), the player nudged the stick at s=19.83, and by s=20.20 he was at anim=0 with no
    // sequence at all. The walk segment replaced the standup and the stick-stop then cancelled the
    // walk, so the animation never played -- his report was "no getting-up animation, he was just
    // suddenly standing". Same hole the note in rt_resolve_attacks describes for ENEMIES not
    // waiting a standup out; this is its other half, on the player's own input.
    //
    // Keeping the gate shut for the ~0.9 s of the animation is also what the fall/dwell block above
    // is for: the point of a knockdown is that you cannot act through it.
    int anim = FID_ANIM_TYPE(obj_dude->fid);
    return (anim == ANIM_BACK_TO_STANDING || anim == ANIM_PRONE_TO_STANDING)
        && anim_busy(obj_dude) != 0;
}

static void rt_player_move(int rotation)
{
    // Flight-recorder: keyboard move intent (held keys coalesce). Traced before the AP-block
    // return so a "pressed move but stood still" case shows blocked=1 in the log.
    trace_event(TRACE_ACTION, "move-key", "dir=%d blocked=%d t=%.2f",
        rotation, rt_move_blocked ? 1 : 0, rt_clock);

    // Knocked down: no movement until he stands up (so the standup isn't pre-empted).
    if (rt_dude_incapacitated()) {
        return;
    }

    // Out of AP: movement is stopped until the pool regenerates to the resume threshold
    // (rt_move_blocked, set in the drain block). Standing recharges it. Combat-only.
    //
    // Under ap_empty_walks the latch means "walk only" instead of "stop", and the keyboard path is
    // walk-only and free to begin with -- so there is nothing left to refuse here and the input
    // goes through.
    if (rt_move_blocked && !rt_ap_empty_walks) {
        return;
    }

    // Mark movement input present (suppresses the enemy-miss dodge at its source).
    rt_move_input_age = 0.0f;

    // A movement input cancels an in-progress flinch so the player isn't frozen
    // for its full duration (the hit already committed, so the tail is cosmetic).
    rt_cancel_flinch_for_move(obj_dude);

    // anim_busy_blocking, not anim_busy: once the dude's own swing has played out he is free,
    // even though the sequence he fired from is still animating his victim's death. See
    // anim_busy_blocking() in anim.cc.
    if (anim_busy_blocking(obj_dude) != 0) {
        // An idle fidget must not lock out movement -- cut it and move now. Anything else
        // (attack / walk / hit-react) legitimately owns the dude, so wait it out.
        // anim_clear_pending, not anim_force_clear: a plain clear ends the first sequence
        // MENTIONING the dude, which may be an older attack sequence now animating a corpse.
        if (rt_dude_is_fidgeting()) {
            anim_clear_pending(obj_dude);
        } else {
            return;
        }
    }

    obj_dude->data.critter.combat.ap = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);

    int dest = tile_num_in_direction(obj_dude->tile, rotation, 1);
    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "pmove")) {
        return;
    }
    register_object_move_to_tile(obj_dude, dest, map_elevation, 1, 0);
    register_end();
}

// Registers a fresh stick-move segment of up to RT_STICK_SEGMENT_TILES tiles
// toward `rotation`, running if `run` else walking. make_path runs when the
// sequence STARTS (register_end -> anim_set_continue, same frame), so a blocked
// destination kills the sequence instantly and anim_busy stays 0 -- that's how
// failure is detected, and why a 1-tile fallback is tried before giving up.
// Caller must ensure anim_busy(obj_dude) == 0. Returns true if movement started.
static bool rt_stick_segment_start(int rotation, bool run)
{
    rt_stick_active = false;

    obj_dude->data.critter.combat.ap = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);

    // Running while sneaking would silently fall back to walk (see
    // register_object_run_to_tile); drop sneak first so LT actually runs, same
    // as the engine's dude_run.
    if (run && !perk_level(PERK_SILENT_RUNNING)) {
        pc_flag_off(PC_FLAG_SNEAKING);
    }

    // Straight directional walk: step along `rotation` only as far as the tiles are actually
    // clear, then register a move to THAT tile. This keeps stick movement going in a straight line
    // and stopping AT a wall, instead of register_object_move_to_tile's make_path routing the dude
    // AROUND the obstacle on its own -- which reads as "I aimed at the wall but the dude walked off
    // on a path I didn't ask for". 0 clear tiles == immediately blocked, so we don't move and the
    // dude just stands against the wall (the append path extends straight).
    int dist = 0;
    int dest = obj_dude->tile;
    for (int i = 0; i < RT_STICK_SEGMENT_TILES; i++) {
        int next = tile_num_in_direction(dest, rotation, 1);
        if (obj_blocking_at(obj_dude, next, map_elevation) != NULL) {
            break;
        }
        dest = next;
        dist++;
    }

    // TEMP DIAG: separate "refused because something is standing in that direction" (normal --
    // you cannot walk through an enemy you are toe to toe with) from "the registration itself
    // was refused", which is a bug. Rate-limited. STRIP with the rest of the move diags.
    static float rt_seg_fail_last = -1.0f;
    if (dist == 0) {
        if (rt_clock - rt_seg_fail_last >= 0.25f) {
            rt_seg_fail_last = rt_clock;
            Object* blocker = obj_blocking_at(obj_dude, tile_num_in_direction(obj_dude->tile, rotation, 1), map_elevation);
            trace_event(TRACE_FLOW, "seg-fail", "reason=blocked dir=%d blockerPid=%d t=%.2f",
                rotation, blocker != NULL ? blocker->pid : -1, rt_clock);
        }
        return false;
    }

    bool registered = false;
    if (rt_register_begin(ANIMATION_REQUEST_RESERVED, "stick")) {
        if (run) {
            register_object_run_to_tile(obj_dude, dest, map_elevation, dist, 0);
        } else {
            register_object_move_to_tile(obj_dude, dest, map_elevation, dist, 0);
        }
        registered = register_end() == 0;
    }

    // Report whether the walk ACTUALLY registered. This used to infer it from anim_busy != 0 --
    // which is equally true when something ELSE owns the dude (his own attack sequence, which is
    // PRIORITIZED), so a walk that never began still latched rt_stick_active = true. With that
    // flag latched, rt_stick_move takes its "segment already in flight" path and, on a steady
    // stick direction, silently does NOTHING: the player holds the stick, stands still, and not
    // one refusal is logged. That was the real "I can't move after killing something" -- the
    // wait just happened to end when the corpse settled, because that is when the attack
    // sequence finally released him.
    if (!registered) {
        if (rt_clock - rt_seg_fail_last >= 0.25f) {
            rt_seg_fail_last = rt_clock;
            trace_event(TRACE_WARN, "seg-fail", "reason=register dir=%d dist=%d anim=%d busyBlk=%d t=%.2f",
                rotation, dist, FID_ANIM_TYPE(obj_dude->fid), anim_busy_blocking(obj_dude), rt_clock);
        }
        return false;
    }

    rt_stick_active = true;
    rt_stick_short = dist == 1;
    rt_stick_running = run;
    rt_stick_rotation = rotation;
    rt_stick_dest = dest;
    return true;
}

static void rt_cut_walk_in_place();

// Called every poll frame while the stick is held. Starts a segment when the
// dude is idle. While our own segment is in flight and the stick still points
// the same way at the same gait, it APPENDS more steps to the live animation
// (anim_extend_move) one tile before it runs out -- this keeps the walk/run
// frame cycle advancing instead of restarting (which a clear+re-register
// would). A gait change, or a heading swung further than RT_HEADING_RESTART_DEG
// from the one the queued steps were planned at, still does clear+restart (the
// art changes there anyway, so a fresh cycle looks natural).
//
// ⚠️ A CHANGE OF DIRECTION REWRITES THE QUEUED STEP, it does not cut the walk. Cutting means
// anim_clear_pending + register, and the first thing object_move does on a fresh sad is
// obj_move_to_tile, whose obj_connect_to_tile zeroes obj->x/y -- so every turn used to SNAP the
// dude back to the centre of the tile he was half-way across, reset the walk cycle to frame 0 and
// run make_path again. Measured: 17 make-paths in a 10 s sweep of the stick, two of them 70 ms
// apart from the same tile. The step in progress cannot be changed (he is physically between two
// tiles); the one queued behind it can.
static void rt_stick_move(int rotation, bool run)
{
    // Knocked down: no movement until he stands up (so the standup isn't pre-empted).
    if (rt_dude_incapacitated()) {
        return;
    }

    // Mark movement input present (suppresses the enemy-miss dodge at its source).
    rt_move_input_age = 0.0f;

    // Holding the stick during a flinch cancels it so movement takes over right
    // away rather than waiting out the hit-react (damage already committed).
    rt_cancel_flinch_for_move(obj_dude);

    // anim_busy_blocking, not anim_busy: the dude walks off as soon as his own swing is spent,
    // without waiting out the victim's death fall registered in the same sequence.
    // Only clear the stuck timer when a walk genuinely STARTED -- otherwise a segment_start that
    // fails every frame keeps resetting it and the backstop below can never trip.
    if (anim_busy_blocking(obj_dude) == 0) {
        if (rt_stick_segment_start(rotation, run)) {
            rt_move_stuck_since = -1.0f;
            return;
        }
    } else if (rt_dude_is_fidgeting()) {
        // Idle fidget owns the dude -- cut it and start walking now instead of waiting it out.
        // Pending-only clear so it can't end an older sequence that is still dropping a corpse.
        //
        // The clear REFUSES a prioritized sequence (-2), and the dude also reads as "standing but
        // busy" during the tail of his own attack, which rt_dude_is_fidgeting cannot tell from a
        // real idle fidget. So a refused clear means this was never a fidget: fall through to the
        // blocked path instead of resetting the timer and asking for a move that cannot start.
        if (anim_clear_pending(obj_dude) == 0 && rt_stick_segment_start(rotation, run)) {
            rt_move_stuck_since = -1.0f;
            return;
        }
    }

    // Second guard on the same latch (rt_stick_segment_start now reports honestly, but a segment
    // can also die under us): our segment is only really in flight while the dude is actually in
    // a locomotion animation. If he isn't, the flag is stale -- and with a steady stick direction
    // neither branch below fires (`changed` false, `nearEnd` false), so the function would
    // silently do NOTHING, forever. Traced: 6 s stuck on one tile, stick held throughout, not one
    // refusal logged.
    int gait = FID_ANIM_TYPE(obj_dude->fid);
    if (rt_stick_active && gait != ANIM_WALK && gait != ANIM_RUNNING) {
        rt_stick_active = false;
    }

    if (!rt_stick_active) {
        // Some other animation (attack, hit react) owns the dude; never clear it.
        //
        // ...unless it has owned him for too long. An attack sequence that never ends (the swing
        // holding on its LAST frame is the traced signature: anim=16 frame=10 for seconds) leaves
        // the player unable to move with no way out, which is always a bug, never a design. This
        // is the same backstop idea as the flinch cap, just for the "stuck busy" case: past
        // RT_MOVE_STUCK_SECONDS of continuously asking to move, cut whatever is holding him.
        // Deliberately longer than a normal swing (~0.5 s) so it never cancels a real attack.
        // Only while something really is holding him -- a walk refused because he is facing a
        // wall must not trip the backstop (there is nothing to cut, and it would log forever).
        if (anim_busy_blocking(obj_dude) == 0) {
            rt_move_stuck_since = -1.0f;
        } else if (rt_move_stuck_since < 0.0f) {
            rt_move_stuck_since = rt_clock;
        } else if (rt_clock - rt_move_stuck_since >= RT_MOVE_STUCK_SECONDS) {
            trace_event(TRACE_WARN, "move-unstick", "anim=%d frame=%d held=%.2f t=%.2f",
                gait, obj_dude->frame, rt_clock - rt_move_stuck_since, rt_clock);
            rt_move_stuck_since = -1.0f;
            anim_force_clear(obj_dude);
            rt_stick_segment_start(rotation, run);
            return;
        }
        //
        // TEMP DIAG (move-why): the player reports "I kill a rat and can't move until its death
        // animation finishes", but the STATE heartbeat shows the dude busy=0 throughout, so
        // anim_busy is NOT what is holding him. This fires on every REFUSED movement input and
        // records who is actually saying no. Rate-limited to one line per 0.25 s so a held stick
        // doesn't flood the log. STRIP once the cause is found.
        static float rt_move_why_last = -1.0f;
        if (rt_clock - rt_move_why_last >= 0.25f) {
            rt_move_why_last = rt_clock;
            char seqDump[220];
            anim_debug_blocking_dump(obj_dude, seqDump, sizeof(seqDump));
            trace_event(TRACE_FLOW, "move-why",
                "busy=%d busyBlk=%d anim=%d frame=%d moveBlk=%d ap=%d inflight=%d %s t=%.2f",
                anim_busy(obj_dude),
                anim_busy_blocking(obj_dude),
                FID_ANIM_TYPE(obj_dude->fid),
                obj_dude->frame,
                rt_move_blocked ? 1 : 0,
                rt_player_ap,
                rt_inflight_count,
                seqDump,
                rt_clock);
        }
        return;
    }

    // ⚠️ A CHANGE OF DIRECTION IS NOT A RESTART ANY MORE -- NOT EVEN A BIG ONE. The gait is the only
    // thing left that has to cut the walk, because the art itself changes there.
    //
    // WHY: a restart is `anim_clear_pending` + register, and the first thing object_move does on a
    // fresh sad is obj_move_to_tile(object, object->tile), whose obj_connect_to_tile sets
    // obj->x = obj->y = 0. So every cut SNAPPED the dude back to the centre of the tile he was
    // half-way across -- up to 32 px thrown away, the walk cycle reset to frame 0, make_path run
    // again, and (now) the smooth camera yanked along with it. Measured in
    // trace_2026-09-19_155521: 17 dude make-paths in a 10 s sweep of the stick, two of them 70 ms
    // apart from the SAME tile. That is the "it is not seamless, it is jerky when I go a different
    // way" report, and it was never about the hex grid at all.
    //
    // Instead the queued step is REWRITTEN in place (anim_move_repoint) and the one in progress is
    // left alone -- it cannot be changed anyway, the dude is physically between two tiles. A turn
    // therefore costs exactly the tile he is already crossing, which is the soonest a hex walk
    // could ever honour it, and costs nothing at all in dropped pixels or restarted cycles.
    bool changed = run != rt_stick_running;

    bool nearEnd = !rt_stick_short && tile_dist(obj_dude->tile, rt_stick_dest) <= 1;

    if (!changed && nearEnd) {
        // Still walking the same way: append to the live sad so the cycle never resets.
        //
        // ONE tile at a time, so the step that is queued behind the one in progress is never more
        // than a tile of committed direction -- that is what keeps a turn cheap enough to absorb by
        // rewriting it (below) instead of cutting the walk.
        int rc = anim_extend_move(obj_dude, rotation, 1);
        if (rc == 0) {
            rt_stick_dest = tile_num_in_direction(rt_stick_dest, rotation, 1);
            rt_stick_rotation = rotation;
            return;
        }
        // Append failed (sad ended between frames, or capacity); restart below.
        changed = true;
    }

    if (!changed) {
        // STEER: rewrite the one queued step to the new direction, in place. Nothing is cut and
        // nothing snaps; the turn costs exactly the tile he is already crossing, which is the
        // soonest a walk on a grid could honour it.
        if (rotation != rt_stick_rotation && anim_move_pending_steps(obj_dude) == 1) {
            int newDest = rt_stick_dest;
            if (anim_move_repoint(obj_dude, &rotation, 1, &newDest) == 0) {
                rt_stick_rotation = rotation;
                rt_stick_dest = newDest;

                static float rt_steer_traced = -1.0f;
                if (rt_clock - rt_steer_traced >= 0.25f) {
                    rt_steer_traced = rt_clock;
                    trace_event(TRACE_INPUT, "steer", "rot=%d t=%.2f", rotation, rt_clock);
                }
            }
        }
        return;
    }

    // GAIT CHANGE (or an append that failed). The art itself changes here, so this one really does
    // need a fresh sequence -- immediately, not at the next tile: switching to a run has to answer
    // the button, not the grid. The offset survives the cut and the new walk resumes from it, so
    // the swap costs no ground and no jump.
    rt_stick_active = false;
    rt_cut_walk_in_place();
    rt_stick_segment_start(rotation, run);
}

// END THE DUDE'S WALK RIGHT NOW, without moving him.
//
// ⚠️ ENDING A WALK SEQUENCE RUNS dude_stand, WHICH ZEROES obj->x/y. He is normally between two
// tiles when a walk is cut, so that alone teleports him back onto the tile he came from -- up to a
// whole tile of ground, given back, in one frame. This puts the offset straight back afterwards, so
// he stands exactly where he was standing. The next walk picks the offset up again (object_move
// hands it to the smoother before re-centring), so nothing snaps at either end.
//
// Returns the offset it preserved, in case the caller registers a new walk immediately.
static void rt_cut_walk_in_place()
{
    if (anim_busy_blocking(obj_dude) == 0) {
        return;
    }

    int keepX = obj_dude->x;
    int keepY = obj_dude->y;

    // Pending-only: this must end the dude's live WALK, never an older attack sequence that is
    // still animating the corpse he just made (that froze bodies mid-fall).
    if (anim_clear_pending(obj_dude) != 0) {
        return;
    }

    if ((keepX != 0 || keepY != 0) && obj_dude->x == 0 && obj_dude->y == 0) {
        Rect r;
        obj_offset(obj_dude, keepX, keepY, &r);
        tile_refresh_rect(&r, obj_dude->elevation);
    }
}

// ===================== FREE MOVEMENT (the body leaves the grid) =====================
//
// His friend's design, and it is the right one: *"player's body, soul and spirit bych splitnul --
// sprite by se mohl hejbat jakkoliv, kamera by followovala sprite, a engine by videl playera na
// nejblizsim miste kde muze bejt."*
//
// The step mover asks "which tile next?" and the body follows it. This one asks "where is the
// body?" and the TILE follows: obj->x/y is integrated from a velocity, and obj->tile is re-anchored
// to whichever cell the body has moved into. Everything else in the engine goes on reading
// obj->tile -- line of sight, scripts, attacks, doors, exit grids -- and never learns the
// difference, because obj->tile never stops being a real, walkable tile.
//
// ⚠️ WHAT THIS DOES NOT FIX: THE SIX FACINGS. The body can travel at any angle; it still has to be
// drawn as one of six sprites. Free movement fixes the PATH; only new art fixes the facing.
//
// THE LATTICE. The hex grid is generated by a = E = (32,0) and b = SE = (16,12): every neighbour is
// a whole combination of those two (NE = a - b). So a pixel offset converts to cell coordinates
// with two divisions, and "which cell am I in" is two roundings -- and the cell that falls out is
// the diamond the tiles are actually drawn as, which is what makes this exact rather than a
// polygon-in-hex approximation.
#define RT_FM_MAX_STEP_PX 12.0f // never cross more than one cell in a frame, whatever dt says
#define RT_FM_FACING_HYST 12.0f // degrees a new facing must beat the current one by

static bool rt_free_move = false; // cfg free_move / MODS row "Free movement"
// This frame's dt. combat_rt_poll_gamepad runs from inside combat_rt_update, which has it; the
// free mover integrates a velocity and cannot work without it.
static float rt_frame_dt = 0.0f;
static bool rt_fm_active = false; // we are driving the body this frame
static float rt_fm_x = 0.0f; // sub-pixel position, of which obj->x/y is the rounded shadow
static float rt_fm_y = 0.0f;
static float rt_fm_frame = 0.0f; // walk-cycle accumulator, in frames
static int rt_fm_facing = -1; // index into rt_hex_ccw, not a rotation
// The tile rt_fm_x/y are measured from -- what combat_rt_dude_rest_offset checks before handing them
// to dude_stand. -1 = no rest spot (map load, reset).
static int rt_fm_rest_tile = -1;

// Pixel offset -> cell coordinates and back. Both are one line because the lattice is sheared, not
// rotated: b contributes to x as well as y, so j comes out of y alone and i out of what is left.
static void rt_fm_cell(float x, float y, float* ci, float* cj)
{
    *cj = y / 12.0f;
    *ci = (x - 16.0f * *cj) / 32.0f;
}

static void rt_fm_pixels(float ci, float cj, float* x, float* y)
{
    *x = 32.0f * ci + 16.0f * cj;
    *y = 12.0f * cj;
}

// The single neighbour named by a whole cell step, or -1 if it is not one of the six.
static int rt_fm_rot(int di, int dj)
{
    if (di == 1 && dj == 0) return ROTATION_E;
    if (di == -1 && dj == 0) return ROTATION_W;
    if (di == 0 && dj == 1) return ROTATION_SE;
    if (di == 0 && dj == -1) return ROTATION_NW;
    if (di == 1 && dj == -1) return ROTATION_NE;
    if (di == -1 && dj == 1) return ROTATION_SW;
    return -1;
}

// Nearest of the six to the heading, held through a hysteresis band. ⚠️ There is no alternation to
// defend against here -- the heading is continuous now -- so this is the plain nearest-angle rule
// that the step mover cannot use -- its heading alternates between two legs by construction.
static int rt_fm_facing_for(float angle)
{
    rt_hex_table_init();

    int best = 0;
    float bestDev = 1000.0f;
    for (int i = 0; i < 6; i++) {
        float d = fmodf(angle - rt_hex_angle[i] + 540.0f, 360.0f) - 180.0f;
        if (d < 0.0f) {
            d = -d;
        }
        if (d < bestDev) {
            bestDev = d;
            best = i;
        }
    }

    if (rt_fm_facing >= 0 && rt_fm_facing < 6 && rt_fm_facing != best) {
        float cur = fmodf(angle - rt_hex_angle[rt_fm_facing] + 540.0f, 360.0f) - 180.0f;
        if (cur < 0.0f) {
            cur = -cur;
        }
        if (cur <= bestDev + RT_FM_FACING_HYST) {
            best = rt_fm_facing;
        }
    }

    rt_fm_facing = best;
    return rt_hex_ccw[best];
}

// Stop driving and stand where the body stands. ⚠️ NOT dude_stand: that re-centres him on his tile,
// which is the whole class of snap this mode exists to remove.
static void rt_fm_release()
{
    if (!rt_fm_active) {
        return;
    }

    rt_fm_active = false;
    rt_stick_active = false;
    rt_fm_frame = 0.0f;
    rt_move_stuck_since = -1.0f; // input released: nothing is being refused any more

    if (obj_dude == NULL) {
        return;
    }

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    if (anim != ANIM_WALK && anim != ANIM_RUNNING) {
        return;
    }

    Rect r;
    int fid = art_id(FID_TYPE(obj_dude->fid), obj_dude->fid & 0xFFF, ANIM_STAND,
        (obj_dude->fid & 0xF000) >> 12, obj_dude->rotation + 1);
    obj_change_fid(obj_dude, fid, &r);
    tile_refresh_rect(&r, obj_dude->elevation);
    obj_set_frame(obj_dude, 0, &r);
    tile_refresh_rect(&r, obj_dude->elevation);
}

// One frame of free movement along `angle`.
static void rt_fm_drive(float angle, bool run, float dt)
{
    if (obj_dude == NULL || dt <= 0.0f) {
        return;
    }

    // ⚠️ IT NEVER FIGHTS THE ANIMATION SYSTEM. An attack, a hit react, a knockdown, or the walk an
    // A-press registered: all of those own the body, and while any of them is registered this mode
    // simply is not driving. That is also what keeps it out of the way of every feature built on
    // the step mover instead of having to re-implement them.
    if (anim_busy_blocking(obj_dude) != 0 || rt_dude_incapacitated()) {
        // ...WITH ONE EXCEPTION, AND IT IS THE ONE HE HIT: THE IDLE FIDGET.
        //
        // "typek se škrabe na hlavě a musím počkat, až se dokončí animace, než se budu moct pohnout,
        // wtf". The engine plays a little scratch-your-head loop when the dude stands still, and it
        // is a registered animation like any other -- so this mode stood off it and the player was
        // locked out of moving for its whole length. The step mover has cut fidgets since the day
        // it was written (rt_stick_move); free movement simply never inherited it, because it
        // inherited nothing from the animation system on purpose.
        //
        // A fidget is "busy, but the art is ANIM_STAND" -- nothing is owed to anyone, so cutting it
        // costs nothing. anim_clear_pending, not force: it must never end an older sequence that is
        // still dropping a corpse.
        if (rt_dude_is_fidgeting() && !rt_dude_incapacitated()) {
            anim_clear_pending(obj_dude);
        }

        if (anim_busy_blocking(obj_dude) != 0 || rt_dude_incapacitated()) {
            if (rt_fm_active) {
                rt_fm_active = false;
                rt_stick_active = false;
            }
            return;
        }
    }

    // ⚠️ AND THE ART HAS TO BE OURS TO WRITE. anim_busy_blocking drops to 0 the moment the dude's
    // own work in a sequence is SPENT -- which, for an attack, is before the swing has finished
    // DRAWING. Taking over there gives obj->fid two writers: this mode slams it to WALK every frame
    // while the attack's own sad goes on stepping its frames, and what you see is the swing tearing
    // in half ("zaútočím a hýbu se tak je to celé ta animace fucked up").
    //
    // The ART is the ownership signal, because the art is the thing this mode writes. Stand, walk
    // and run are ours; anything else belongs to whoever registered it. That is still far tighter
    // than waiting for the whole sequence to end -- the corpse half of an attack sequence never
    // touches the dude's art, so it no longer holds him.
    int curAnim = FID_ANIM_TYPE(obj_dude->fid);
    if (curAnim != ANIM_STAND && curAnim != ANIM_WALK && curAnim != ANIM_RUNNING) {
        // Backstop, the same one the step mover has (RT_MOVE_STUCK_SECONDS): an animation that
        // never ends leaves the player unable to move with no way out, which is always a bug and
        // never a design. Deliberately longer than any real swing.
        if (rt_move_stuck_since < 0.0f) {
            rt_move_stuck_since = rt_clock;
        } else if (rt_clock - rt_move_stuck_since >= RT_MOVE_STUCK_SECONDS) {
            trace_event(TRACE_WARN, "fm-unstick", "anim=%d frame=%d held=%.2f t=%.2f",
                curAnim, obj_dude->frame, rt_clock - rt_move_stuck_since, rt_clock);
            rt_move_stuck_since = -1.0f;
            anim_force_clear(obj_dude);
        }

        if (rt_fm_active) {
            rt_fm_active = false;
            rt_stick_active = false;
        }
        return;
    }

    rt_move_stuck_since = -1.0f;

    rt_cancel_flinch_for_move(obj_dude);
    rt_move_input_age = 0.0f;

    int anim = run ? ANIM_RUNNING : ANIM_WALK;

    float tps = 0.0f;
    float fps = 0.0f;
    anim_move_speed(obj_dude, anim, &tps, &fps);
    if (tps <= 0.0f) {
        return; // art we cannot measure: leave the body alone rather than guess a speed
    }

    // Re-sync if anything else moved him (a cut walk, a knockback, a map load). Our float position
    // is the master, but only while nobody else has touched the int it shadows.
    if ((int)lroundf(rt_fm_x) != obj_dude->x || (int)lroundf(rt_fm_y) != obj_dude->y) {
        rt_fm_x = (float)obj_dude->x;
        rt_fm_y = (float)obj_dude->y;
    }
    rt_fm_rest_tile = obj_dude->tile;

    float rad = angle * 0.0174532925f;
    float dx = cosf(rad);
    float dy = -sinf(rad); // screen y grows downward

    // ⚠️ SPEED IS IN TILES PER SECOND, NOT PIXELS PER SECOND. The projection squashes the vertical,
    // so a fixed pixel speed would cover ground faster going east than going north -- and every
    // number in the combat model (the AP drain, chase pacing, how far you get between two shots) is
    // denominated in tiles. Converting needs the length of this screen direction measured in the
    // lattice's own metric, where the two basis steps are both exactly one tile.
    float ci;
    float cj;
    rt_fm_cell(dx, dy, &ci, &cj);
    float worldLen = sqrtf(ci * ci + cj * cj + ci * cj);
    if (worldLen < 0.00001f) {
        return;
    }

    float pxPerSec = tps / worldLen;
    float mx = dx * pxPerSec * dt;
    float my = dy * pxPerSec * dt;

    // One cell per frame, whatever dt says. A frame spike (a load, a menu, a breakpoint) must not
    // teleport the body through a wall the crossing test below never gets to see.
    float step = sqrtf(mx * mx + my * my);
    if (step > RT_FM_MAX_STEP_PX) {
        mx *= RT_FM_MAX_STEP_PX / step;
        my *= RT_FM_MAX_STEP_PX / step;
    }

    float nx = rt_fm_x + mx;
    float ny = rt_fm_y + my;

    Rect dirty;
    bool haveDirty = false;
    Rect r;

    // ⚠️⚠️ SPATIAL SCRIPTS. Set when this frame moves him to a new tile, and fired at the very END of
    // this function -- see the note there. THE BUG IT FIXES IS NOT SMALL: spatial scripts are
    // triggered from exactly ONE place in the engine, anim_bk, immediately after object_move changes
    // a tile. Free movement never goes through object_move -- it re-anchors obj->tile itself -- so
    // from the moment it was switched on, NOT ONE SPATIAL SCRIPT IN THE GAME HAD FIRED. Triggers,
    // ambushes, cutscene hexes, and the Glow elevator, which is a spatial script and not a door at
    // all: the script (ELEV6.INT, disassembled) is "if script_action == 2 (SPATIAL) and source is
    // the dude, metarule(ELEVATOR)". Pressing A on the elevator door only ever animated the door,
    // which is exactly what he saw, over and over: "zase zavře dveře".
    int spatialTile = -1;

    rt_fm_cell(nx, ny, &ci, &cj);

    // ⚠️ FLOOR, NOT ROUND. The cell is [-0.5, +0.5): a crossing leaves a residual of exactly -0.5 on
    // that axis, and rounding (which goes away from zero) would read that as "you are a cell to the
    // WEST now" and send the body straight back -- a ping-pong across every boundary it touches.
    int di = (int)floorf(ci + 0.5f);
    int dj = (int)floorf(cj + 0.5f);

    if (di != 0 || dj != 0) {
        // The exact neighbour first -- a diagonal is ONE step, and walking it as E-then-NW would
        // test the corner tile and refuse a gap that is actually open. Then each axis ALONE, and
        // that fallback is the whole of wall sliding: push south-west into a wall that only blocks
        // the south-east axis and the west half of the push still goes through, so you slide along
        // the wall instead of sticking to it.
        int tries[3][2] = { { di, dj }, { di, 0 }, { 0, dj } };
        bool crossed = false;

        for (int t = 0; t < 3 && !crossed; t++) {
            int ai = tries[t][0];
            int aj = tries[t][1];
            if (ai == 0 && aj == 0) {
                continue;
            }

            int rot = rt_fm_rot(ai, aj);
            if (rot < 0) {
                continue;
            }

            int dest = tile_num_in_direction(obj_dude->tile, rot, 1);
            if (!hexGridTileIsValid(dest) || obj_blocking_at(obj_dude, dest, map_elevation) != NULL) {
                continue;
            }

            obj_move_to_tile(obj_dude, dest, map_elevation, &r);
            dirty = r;
            haveDirty = true;

            nx -= (float)off_tile[0][rot];
            ny -= (float)off_tile[1][rot];
            crossed = true;
            spatialTile = dest;
        }

        // Nothing at all got through: the body is jammed into a corner. Rate-limited, because a
        // held stick would otherwise log it sixty times a second -- and it is the signature to
        // grep for if he reports being stuck on something he should have slid past.
        if (!crossed) {
            static float rt_fm_blocked_traced = -1.0f;
            if (rt_clock - rt_fm_blocked_traced >= 0.5f) {
                rt_fm_blocked_traced = rt_clock;
                int rot = rt_fm_rot(di, dj);
                Object* blocker = rot >= 0
                    ? obj_blocking_at(obj_dude, tile_num_in_direction(obj_dude->tile, rot, 1), map_elevation)
                    : NULL;
                trace_event(TRACE_INPUT, "fm-stuck", "di=%d dj=%d rot=%d tile=%d blockerPid=%d ang=%.0f t=%.2f",
                    di, dj, rot, obj_dude->tile, blocker != NULL ? blocker->pid : -1, angle, rt_clock);
            }
        }
    }

    // Always: the body may never end a frame outside its own cell, whether it crossed, slid or was
    // stopped dead. Pressed against a wall it sits at the boundary -- half a tile out, which is the
    // sprite standing against the wall rather than inside it.
    rt_fm_cell(nx, ny, &ci, &cj);
    if (ci > 0.4999f) {
        ci = 0.4999f;
    } else if (ci < -0.5f) {
        ci = -0.5f;
    }
    if (cj > 0.4999f) {
        cj = 0.4999f;
    } else if (cj < -0.5f) {
        cj = -0.5f;
    }
    rt_fm_pixels(ci, cj, &nx, &ny);

    rt_fm_x = nx;
    rt_fm_y = ny;
    rt_fm_rest_tile = obj_dude->tile;

    int px = (int)lroundf(nx);
    int py = (int)lroundf(ny);
    if (px != obj_dude->x || py != obj_dude->y) {
        obj_offset(obj_dude, px - obj_dude->x, py - obj_dude->y, &r);
        if (haveDirty) {
            rect_min_bound(&dirty, &r, &dirty);
        } else {
            dirty = r;
            haveDirty = true;
        }
    }

    int facing = rt_fm_facing_for(angle);
    if (facing != obj_dude->rotation) {
        obj_set_rotation(obj_dude, facing, &r);
        if (haveDirty) {
            rect_min_bound(&dirty, &r, &dirty);
        } else {
            dirty = r;
            haveDirty = true;
        }
    }

    if (FID_ANIM_TYPE(obj_dude->fid) != anim) {
        int fid = art_id(FID_TYPE(obj_dude->fid), obj_dude->fid & 0xFFF, anim,
            (obj_dude->fid & 0xF000) >> 12, obj_dude->rotation + 1);
        obj_change_fid(obj_dude, fid, &r);
        if (haveDirty) {
            rect_min_bound(&dirty, &r, &dirty);
        } else {
            dirty = r;
            haveDirty = true;
        }
        obj_set_frame(obj_dude, 0, &r);
        rect_min_bound(&dirty, &r, &dirty);
        rt_fm_frame = 0.0f;
    }

    // The legs run off the DISTANCE COVERED, not off a timer: that is the same relationship the
    // step mover has (its tile lasts exactly frames-per-tile frames), so the walk cycle stays in
    // step with the ground at any speed and in any direction.
    rt_fm_frame += fps * dt;
    while (rt_fm_frame >= 1.0f) {
        rt_fm_frame -= 1.0f;
        obj_inc_frame(obj_dude, &r);
        if (haveDirty) {
            rect_min_bound(&dirty, &r, &dirty);
        } else {
            dirty = r;
            haveDirty = true;
        }
    }

    if (haveDirty) {
        tile_refresh_rect(&dirty, obj_dude->elevation);
    }

    // Tell the rest of the model what is going on: the run AP drain and every "is he walking"
    // question are keyed on these two, and they are the only thing this mode shares with the step
    // mover's state.
    rt_fm_active = true;
    rt_stick_active = true;
    rt_stick_running = run;

    // ⚠️ LAST, AND NOTHING OF OURS AFTER IT. A spatial script can do anything -- open the elevator
    // panel, start a fight, change the map -- and it runs to completion inside this call, so every
    // piece of our own state has to be settled before it goes. This is the same relationship the
    // step mover has with it (anim_bk fires it straight after object_move and touches nothing
    // afterwards).
    if (spatialTile >= 0) {
        trace_event(TRACE_FLOW, "fm-spatial", "tile=%d t=%.2f", spatialTile, rt_clock);
        scr_chk_spatials_in(obj_dude, spatialTile, map_elevation);
    }
}

// Called when the stick returns to the deadzone: STOP. Not "stop at the next tile" -- stop.
//
// ⚠️ IT USED TO COAST OUT THE STEP IN PROGRESS (one tile, ~0.4 s) to avoid the snap described in
// rt_cut_walk_in_place. He played it and the delay is what he felt: "zastavím movement a hráč ještě
// dochází... když zastavím joystick, tak se zastaví a nebude docházet". The snap is worth avoiding,
// the wait is not -- so the walk is cut on the spot and the offset is kept instead.
static void rt_stick_stop()
{
    rt_move_stuck_since = -1.0f; // stick released: no longer being refused
    if (!rt_stick_active) {
        return;
    }

    rt_stick_active = false;
    rt_cut_walk_in_place();
}

// CANCEL WHATEVER THE PLAYER IS PART-WAY THROUGH. Returns true if there was something to cancel,
// so the caller can swallow the button's other meaning only when it actually did something.
//
// The case it exists for: A on a distant object registers a WALK-THERE-THEN-ACT sequence (that is
// how every vanilla action works -- action_use_an_object and friends walk you into reach first), and
// once it is running there was no way to change your mind. The user clicked A, watched the dude set
// off across the map, and had no brake -- "chtěl jsem zrušit akci a nešlo to".
//
// anim_clear_pending kills the whole registered sequence, trailing action included, which is exactly
// right: cancelling the walk must also cancel the thing it was walking to do. Nothing else is
// touched -- an attack in flight is combat_rt's business and has its own commit rules.
bool combat_rt_cancel_player_action()
{
    if (obj_dude == NULL) {
        return false;
    }

    // ⚠️ NOT WHILE THE STICK IS STEERING HIM. B cancels a COMMITTED action -- a walk-to-click, an
    // attack wind-up -- something you asked for once and now want to take back. Stick movement is
    // not that: it is continuous, you cancel it by letting go, and there is nothing queued to
    // abort. Cancelling it anyway cut the step short AND ate the press, so B never reached its real
    // job of toggling the firing mode ("dude neudělá celej krok a nepřepne se režim střílení").
    if (combat_rt_stick_walking()) {
        return false;
    }

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    bool moving = anim == ANIM_WALK || anim == ANIM_RUNNING;
    if (!moving && anim_busy_blocking(obj_dude) == 0) {
        return false;
    }

    rt_stick_active = false;
    rt_move_stuck_since = -1.0f;
    anim_clear_pending(obj_dude);

    trace_event(TRACE_ACTION, "cancel", "anim=%d t=%.2f", anim, rt_clock);
    return true;
}

// True if obj is one of the critters currently tracked in the actor registry.
// Pointer-compare only (no dereference), so it is safe to call on a possibly
// stale lock-on pointer: the registry is rebuilt from live world objects, so a
// freed/despawned/cross-map critter won't be found, and the caller can null the
// lock before any field access.
static bool rt_in_registry(Object* obj)
{
    if (obj == NULL) {
        return false;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == obj) {
            return true;
        }
    }

    return false;
}

// Nearest hostile combatant within the dude's weapon range, or NULL. Used by the
// gamepad attack (which has no mouse cursor to pick a target).
static Object* rt_nearest_hostile_in_range(int hitMode)
{
    int range = item_w_range(obj_dude, hitMode);
    Object* best = NULL;
    int bestDist = range + 1;

    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        // Only ACTIVE hostiles here -- rt_is_combatant, NOT rt_is_targetable. Note precisely what
        // that excludes: DEAD and KNOCKED-OUT (unconscious). A merely KNOCKED-DOWN critter still
        // counts as active, which is right -- it stands back up in a beat and keeps fighting.
        // This drives combat_rt_hostile_in_attack_range, which gates BOTH the A-button attack-vs-
        // interact split AND the interact-prompt suppression. Counting an unconscious enemy as
        // "in range" kept the player stuck "in combat" next to a body -- so the interact prompt stayed
        // off and a door/loot wouldn't highlight or open with A (the "can't open Gizmo's door, I'm in
        // combat" report). A knocked-out enemy is not an active threat: let combat wind down so A
        // interacts and the prompt returns. You can still FINISH a knocked-out enemy by locking it
        // (R3) -- rt_is_lockable + the lock/aim/fire paths use rt_is_targetable, so a held lock
        // still fires.
        if (!rt_is_combatant(critter) || !rt_actors[i].hostile) {
            continue;
        }

        int dist = obj_dist(obj_dude, critter);
        if (dist <= range && dist < bestDist) {
            bestDist = dist;
            best = critter;
        }
    }

    return best;
}

// Whether a critter can be lock-on targeted. Broader than rt_is_hostile so the
// player can lock an enemy that hasn't aggroed yet: a living non-dude combatant
// that is either already hostile, or on a different team AND perceivable by the
// dude (line-of-sight/perception gate, so we don't lock unseen or cross-map
// critters). Locking does not aggro -- only the A-attack does -- so a different-
// team neutral being lockable is harmless.
static bool rt_is_lockable(Object* obj)
{
    if (obj == NULL || obj == obj_dude || !rt_is_targetable(obj)) {
        return false;
    }

    if (rt_is_hostile(obj)) {
        return true;
    }

    return obj->data.critter.combat.team != obj_dude->data.critter.combat.team
        && is_within_perception(obj_dude, obj);
}

// Nearest lockable critter on the map regardless of weapon range, or NULL. Used
// to engage the R3 lock-on (you can lock a distant target and then close on it).
// How far the dude can lock, from his own Perception. See RT_LOCK_PERCEPTION_SCALE.
static int rt_lock_range()
{
    if (obj_dude == NULL) {
        return rt_lock_min_tiles;
    }

    int range = (int)(stat_level(obj_dude, STAT_PERCEPTION) * rt_lock_pe_scale);
    return range < rt_lock_min_tiles ? rt_lock_min_tiles : range;
}

static bool rt_hostile_by_nature(Object* critter);

// What a fresh aim prefers: 2 = fighting you, or a mutated animal (the same red outline), 1 = any
// other lockable critter. Any 2 beats every 1.
static int rt_lock_class(Object* critter)
{
    return rt_is_hostile(critter) || rt_hostile_by_nature(critter) ? 2 : 1;
}

// ⚠️ WHAT YOU FACE FIRST, THEN ENEMIES, THEN NEAREST (2026-10-10). The order the fresh aim ranks by:
//   1. IN FRONT beats BEHIND: anyone within RT_LOCK_FRONT_DEG of your facing beats anyone behind you;
//   2. inside that half, anyone fighting you beats any neutral (rt_lock_class);
//   3. inside that, tiles away plus the angle off your facing, in the right stick's own units
//      (RT_LOCK_STEP_DEG_PER_TILE), so a target straight ahead at 10 beats one at your shoulder at 6.
//
// Rule 2 alone was the 2026-10-08 fix for *"koukám postavou na mastera a i tak se mi LT aimne na
// neutrálního mutanta"* (a neutral at the shoulder beat the Master ahead -- both are in front, so it
// still holds). Rule 1 is new: on its own, rule 2 let an enemy BEHIND you outrank a neutral in front.
// A neutral is still picked when nobody is fighting you in front -- that is how you start a fight on
// purpose -- and the right stick still steps onto anyone.
#define RT_LOCK_FRONT_DEG 90.0f

// The dude's facing as a screen direction, and his on-screen centre. `valid` is false at a map edge,
// where there is no tile "ahead": every angle is then 0 and distance alone decides.
typedef struct RtLockFacing {
    bool valid;
    float fx;
    float fy;
    float ax;
    float ay;
} RtLockFacing;

static RtLockFacing rt_lock_facing()
{
    RtLockFacing f;
    f.valid = false;
    f.fx = 0.0f;
    f.fy = 0.0f;

    int ahead = tile_num_in_direction(obj_dude->tile, obj_dude->rotation, 1);
    if (ahead >= 0 && ahead != obj_dude->tile) {
        int x0;
        int y0;
        int x1;
        int y1;
        tile_coord(obj_dude->tile, &x0, &y0, obj_dude->elevation);
        tile_coord(ahead, &x1, &y1, obj_dude->elevation);
        float fx = (float)(x1 - x0);
        float fy = (float)(y1 - y0);
        float flen = sqrtf(fx * fx + fy * fy);
        if (flen > 0.0001f) {
            f.valid = true;
            f.fx = fx / flen;
            f.fy = fy / flen;
        }
    }

    Rect db;
    obj_bound(obj_dude, &db);
    f.ax = (db.ulx + db.lrx) * 0.5f;
    f.ay = (db.uly + db.lry) * 0.5f;
    return f;
}

// Degrees between the facing and the direction to `critter`: 0 straight ahead, 180 straight behind.
static float rt_lock_off_facing(const RtLockFacing* f, Object* critter)
{
    if (!f->valid) {
        return 0.0f;
    }
    Rect b;
    obj_bound(critter, &b);
    float ex = (b.ulx + b.lrx) * 0.5f - f->ax;
    float ey = (b.uly + b.lry) * 0.5f - f->ay;
    float elen = sqrtf(ex * ex + ey * ey);
    if (elen <= 0.0001f) {
        return 0.0f;
    }
    float dot = (ex / elen) * f->fx + (ey / elen) * f->fy;
    dot = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
    return acosf(dot) * (180.0f / 3.14159265f);
}

// ⚠️ ONLY WHAT YOU COULD SHOOT (2026-10-10): *"držel jsem AIM a aimovalo to krysu ve vedlejší místnosti
// skrze zeď"* -- trace_2026-10-10_115753 s=1068.44 locked id=1497 next door. A hostile was lockable by
// range and hostility alone, walls never asked. Now a fresh pick, a remembered target and a right-
// stick switch all need a clear line of fire: the same test a shot makes (combat_is_shot_blocked --
// walls and scenery stop it, critters in between do not). A lock already held is NOT dropped when
// its target steps behind a pillar; this only decides what you can lock onto.
static bool rt_lock_in_sight(Object* critter)
{
    return !combat_is_shot_blocked(obj_dude, obj_dude->tile, critter->tile, critter, NULL);
}

// Lower is better: tiles away plus the angle off the facing in tiles.
static float rt_lock_score(int dist, float offDeg)
{
    return dist + offDeg / RT_LOCK_STEP_DEG_PER_TILE;
}

static Object* rt_lock_pick_facing()
{
    RtLockFacing facing = rt_lock_facing();

    Object* best = NULL;
    bool bestFront = false;
    int bestClass = 0;
    float bestScore = 0.0f;
    int maxDist = rt_lock_range();

    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        if (!rt_is_lockable(critter)) {
            continue;
        }

        int dist = obj_dist(obj_dude, critter);
        if (dist > maxDist) {
            continue;
        }
        if (!rt_lock_in_sight(critter)) {
            continue;
        }

        float offDeg = rt_lock_off_facing(&facing, critter);
        bool front = offDeg <= RT_LOCK_FRONT_DEG;
        int cls = rt_lock_class(critter);
        float score = rt_lock_score(dist, offDeg);
        bool better;
        if (best == NULL) {
            better = true;
        } else if (front != bestFront) {
            better = front;
        } else if (cls != bestClass) {
            better = cls > bestClass;
        } else {
            better = score < bestScore;
        }
        if (better) {
            best = critter;
            bestFront = front;
            bestClass = cls;
            bestScore = score;
        }
    }

    return best;
}

// Who a fresh aim (LT) locks onto. The critter you last had wins back the lock -- "ať si hra pamatuje,
// kdo byl naposledy zamířen, když dám zase aiming" (2026-10-04) -- only while it could still be locked
// by every rule a new lock obeys (in the registry, lockable, within your Perception reach), is no
// less an enemy than the fresh pick (a remembered neutral must not outrank the Master), AND:
//
// ⚠️ IS IN FRONT OF YOU, AND NOT A MUCH WORSE CHOICE THAN WHAT IS (2026-10-10). *"Otočil jsem se zády
// masterovi, že zamířím před sebou na mutanta, a furt to aimovalo mastera"* -- trace_2026-10-10_015527:
// at s=54.39 the Master stood 152 degrees behind him, a hostile 13 degrees ahead at 6 tiles, and the
// lock went to the Master; again at 129.5 (111 degrees), 163.7 (148), 172.8 (105). The memory had no
// notion of where you look, so the last target simply always came back. Now a remembered target
// behind you is forgotten, and one in front is kept only within RT_LOCK_KEEP_MARGIN (in the score's
// tile units) of the fresh pick: turning onto a mutant beside you takes the mutant, re-aiming at the
// same fight takes back the one you had.
#define RT_LOCK_KEEP_MARGIN 4.0f

static Object* rt_lock_pick_initial()
{
    Object* fresh = rt_lock_pick_facing();
    Object* remembered = rt_lock_last_target;
    bool keep = false;
    float rememberedOff = -1.0f;

    if (remembered != NULL
        && rt_in_registry(remembered)
        && rt_is_lockable(remembered)
        && obj_dist(obj_dude, remembered) <= rt_lock_range()
        && rt_lock_in_sight(remembered)) {
        RtLockFacing facing = rt_lock_facing();
        rememberedOff = rt_lock_off_facing(&facing, remembered);
        keep = rememberedOff <= RT_LOCK_FRONT_DEG;
        if (keep && fresh != NULL && fresh != remembered) {
            float freshScore = rt_lock_score(obj_dist(obj_dude, fresh), rt_lock_off_facing(&facing, fresh));
            float keptScore = rt_lock_score(obj_dist(obj_dude, remembered), rememberedOff);
            keep = rt_lock_class(remembered) >= rt_lock_class(fresh) && keptScore <= freshScore + RT_LOCK_KEEP_MARGIN;
        }
    } else {
        remembered = NULL;
    }

    Object* chosen = keep ? remembered : fresh;
    // Why this one -- the question every "it aimed at the wrong one" report asks. Only when something
    // was chosen: a held LT with nothing in sight asks again every frame, and logged each one.
    if (chosen != NULL) {
        trace_event(TRACE_ACTION, "lock-pick", "chosen=%d fresh=%d remembered=%d remOff=%.0f kept=%d t=%.2f",
            chosen->id, fresh != NULL ? fresh->id : -1,
            remembered != NULL ? remembered->id : -1, rememberedOff, keep ? 1 : 0, rt_clock);
    }
    return chosen;
}

// --- AIM OUTLINES (2026-10-04) ------------------------------------------------------------------
//
// While you aim -- a lock is up, from LT or R3 -- every living critter on screen wears an outline that
// says what it is TO YOU: RED hostile, YELLOW neutral, GREEN your own team. His words: "když mířím,
// červení jsou enemy, žlutí budou neutral a zelení allies, ať nejsem během combatu zmatený. Někdy jsou
// highlighted, někdy ne."
//
// The "někdy ano, někdy ne" was vanilla: combat_outline_on outlines everybody only OUTSIDE a fight --
// its in-fight branch walks the turn-based combat_list, which realtime never fills -- and
// combat_outline_off then left those outlines stuck on whoever they were on when a fight began. In
// realtime this code owns critter outlines while aiming, and combat_outline_on/off stand down
// (combat.cc). Rebuilt every RT_AIM_OUTLINE_REFRESH seconds as a diff, so a critter that turns
// hostile changes colour without anything flickering.
#define RT_AIM_OUTLINE_MAX 64
#define RT_AIM_OUTLINE_REFRESH 0.2f

static Object* rt_aim_outlined[RT_AIM_OUTLINE_MAX];
static int rt_aim_outlined_type[RT_AIM_OUTLINE_MAX];
static int rt_aim_outlined_count = 0;
static float rt_aim_outline_next = 0.0f;

static bool rt_obj_on_screen(Object* obj)
{
    if (obj == NULL || display_win == -1) {
        return false;
    }
    Rect win;
    if (win_get_rect(display_win, &win) == -1) {
        return false;
    }
    Rect b;
    obj_bound(obj, &b);
    return !(b.lrx < win.ulx || b.ulx > win.lrx || b.lry < win.uly || b.uly > win.lry);
}

// MUTATED ANIMALS ARE ENEMIES BY NATURE (2026-10-08). The engine only flags a rat hostile once it
// has seen you and gone for you, so until then the aim outline painted it YELLOW, neutral -- and a
// player reads yellow as "leave it alone": "krysy jsou žluté ... vždycky budou k hráči hostile,
// zmutovaná zvířata taky. Takhle to může mást hráče." The proto's KILL TYPE is the game's own
// answer to "what species is this", so it decides. Brahmin and dogs are not on the list -- cattle are
// peaceful and a dog may be Dogmeat. ⚠️ COLOUR ONLY: what counts as hostile for the A button, the
// interact prompt and the lock's auto-hop is still rt_is_hostile, i.e. whoever is actually fighting.
static bool rt_hostile_by_nature(Object* critter)
{
    switch (critter_kill_count_type(critter)) {
    case KILL_TYPE_RADSCORPION:
    case KILL_TYPE_RAT:
    case KILL_TYPE_FLOATER:
    case KILL_TYPE_CENTAUR:
    case KILL_TYPE_MANTIS:
    case KILL_TYPE_DEATH_CLAW:
    case KILL_TYPE_PLANT:
        return true;
    default:
        return false;
    }
}

static int rt_aim_outline_type_for(Object* critter)
{
    if (rt_is_hostile(critter)) {
        return OUTLINE_TYPE_HOSTILE;
    }
    // Your own team first, so an animal that has joined you stays green -- the exception he allowed for.
    if (critter->data.critter.combat.team == obj_dude->data.critter.combat.team) {
        return OUTLINE_TYPE_FRIENDLY;
    }
    if (rt_hostile_by_nature(critter)) {
        return OUTLINE_TYPE_HOSTILE;
    }
    return OUTLINE_TYPE_ITEM; // the engine's yellow
}

static void rt_aim_outline_drop(Object* obj)
{
    Rect r;
    if (obj_remove_outline(obj, &r) == 0) {
        tile_refresh_rect(&r, obj->elevation);
    }
}

static void rt_aim_outlines_clear()
{
    for (int i = 0; i < rt_aim_outlined_count; i++) {
        rt_aim_outline_drop(rt_aim_outlined[i]);
    }
    rt_aim_outlined_count = 0;
}

static void rt_aim_outlines_update()
{
    if (rt_lock_target == NULL || obj_dude == NULL) {
        if (rt_aim_outlined_count > 0) {
            rt_aim_outlines_clear();
        }
        rt_aim_outline_next = 0.0f;
        return;
    }

    if (rt_clock < rt_aim_outline_next) {
        return;
    }
    rt_aim_outline_next = rt_clock + RT_AIM_OUTLINE_REFRESH;

    Object* keep[RT_AIM_OUTLINE_MAX];
    int keepType[RT_AIM_OUTLINE_MAX];
    int keepCount = 0;

    for (Object* o = obj_find_first_at(obj_dude->elevation); o != NULL && keepCount < RT_AIM_OUTLINE_MAX; o = obj_find_next_at()) {
        if (o == obj_dude || FID_TYPE(o->fid) != OBJ_TYPE_CRITTER) {
            continue;
        }
        if ((o->flags & OBJECT_HIDDEN) != 0 || critter_is_dead(o) || !rt_obj_on_screen(o)) {
            continue;
        }

        int type = rt_aim_outline_type_for(o);

        bool same = false;
        for (int i = 0; i < rt_aim_outlined_count; i++) {
            if (rt_aim_outlined[i] == o) {
                same = rt_aim_outlined_type[i] == type && (o->outline & OUTLINE_TYPE_MASK) == type;
                break;
            }
        }

        if (!same) {
            obj_remove_outline(o, NULL);
            Rect r;
            if (obj_outline_object(o, type, &r) != 0) {
                continue; // OBJECT_NO_HIGHLIGHT -- the map says this one is never outlined
            }
            obj_turn_on_outline(o, &r);
            tile_refresh_rect(&r, o->elevation);
        }

        keep[keepCount] = o;
        keepType[keepCount] = type;
        keepCount++;
    }

    // Anyone outlined last time who is not in this set any more -- dead, off screen -- loses it.
    for (int i = 0; i < rt_aim_outlined_count; i++) {
        bool still = false;
        for (int j = 0; j < keepCount; j++) {
            if (keep[j] == rt_aim_outlined[i]) {
                still = true;
                break;
            }
        }
        if (!still) {
            rt_aim_outline_drop(rt_aim_outlined[i]);
        }
    }

    for (int i = 0; i < keepCount; i++) {
        rt_aim_outlined[i] = keep[i];
        rt_aim_outlined_type[i] = keepType[i];
    }
    rt_aim_outlined_count = keepCount;
}

// Forget an outlined critter that is about to be freed (pointer only -- nothing to restore).
static void rt_aim_outlines_forget(Object* obj)
{
    for (int i = 0; i < rt_aim_outlined_count; i++) {
        if (rt_aim_outlined[i] == obj) {
            rt_aim_outlined[i] = rt_aim_outlined[rt_aim_outlined_count - 1];
            rt_aim_outlined_type[i] = rt_aim_outlined_type[rt_aim_outlined_count - 1];
            rt_aim_outlined_count--;
            return;
        }
    }
}

// Like rt_nearest_lockable but STRICT: only critters actually flagged HOSTILE to the
// dude (rt_is_lockable also allows different-team neutrals so you can deliberately
// pre-aim them). Used for the auto-hop when a locked enemy dies -- the lock must jump
// ONLY to another enemy, never snap onto a bystander/neutral/ally standing nearby.
static Object* rt_nearest_hostile_lockable()
{
    Object* best = NULL;
    int bestDist = INT_MAX;

    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        if (critter == obj_dude || !rt_actors[i].hostile || !rt_is_combatant(critter)) {
            continue;
        }

        int dist = obj_dist(obj_dude, critter);
        if (dist < bestDist) {
            bestDist = dist;
            best = critter;
        }
    }

    return best;
}

// Re-points the lock to the next (dir > 0) or previous (dir < 0) hostile in
// left-to-right screen order, wrapping at the ends. No-op with fewer than two
// candidates. Drives the Dark-Souls-style target switch on a right-stick flick.
static void rt_cycle_lock(int dir)
{
    Object* list[RT_MAX_ACTORS];
    int centerX[RT_MAX_ACTORS];
    int count = 0;

    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        if (!rt_is_lockable(critter)) {
            continue;
        }

        Rect bounds;
        obj_bound(critter, &bounds);
        list[count] = critter;
        centerX[count] = (bounds.ulx + bounds.lrx) / 2;
        count++;
    }

    if (count < 2) {
        return;
    }

    // Insertion sort by on-screen center X (left -> right).
    for (int i = 1; i < count; i++) {
        Object* obj = list[i];
        int x = centerX[i];
        int j = i - 1;
        while (j >= 0 && centerX[j] > x) {
            list[j + 1] = list[j];
            centerX[j + 1] = centerX[j];
            j--;
        }
        list[j + 1] = obj;
        centerX[j + 1] = x;
    }

    int cur = -1;
    for (int i = 0; i < count; i++) {
        if (list[i] == rt_lock_target) {
            cur = i;
            break;
        }
    }

    int next = (cur < 0)
        ? (dir > 0 ? 0 : count - 1)
        : (cur + dir + count) % count;
    rt_lock_target = list[next];
}

// The anchor a push measures from: the target locked when the push began, or the current one if no
// push is under way. Both are registry critters, freed-pointer safe via notify_critter_removed.
static Object* rt_lock_step_anchor()
{
    return rt_lock_push_anchor != NULL ? rt_lock_push_anchor : rt_lock_target;
}

// Where `c` lies relative to the anchor's on-screen centre (ax, ay), as seen along the push (sx, sy,
// unit length). Returns false when it is on the wrong side of the anchor; otherwise the angle off
// the push in degrees. Screen space, because that is where the thumb points -- the distance that
// ranks the candidates is measured in TILES (obj_dist) by the caller, because the projection
// squashes the vertical axis and "how far" is a question about the world.
static bool rt_lock_step_angle(float ax, float ay, Object* c, float sx, float sy, float* offDegOut)
{
    Rect b;
    obj_bound(c, &b);
    float ex = (b.ulx + b.lrx) * 0.5f - ax;
    float ey = (b.uly + b.lry) * 0.5f - ay;
    float elen = sqrtf(ex * ex + ey * ey);
    if (elen < 0.0001f) {
        return false;
    }

    float dot = (ex / elen) * sx + (ey / elen) * sy; // cos of the angle to the pushed direction
    if (dot < RT_LOCK_AIM_CONE_COS) {
        return false;
    }

    float clamped = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
    *offDegOut = acosf(clamped) * (180.0f / 3.14159265f);
    return true;
}

// Switches the lock with the RIGHT STICK: push toward a side and the lock moves to the enemy NEAREST
// TO THE ONE YOU HAD, on that side of it. SDL stick (sx,sy) and screen coords are both +x right /
// +y down, so the stick vector maps straight to a screen direction. Called EVERY FRAME the stick is
// out; returns true only when the lock actually moved, so the caller blips once per change.
//
// ⚠️ ANCHORED ON THE TARGET, NOT ON THE DUDE (2026-10-04). See RT_LOCK_STEP_DEG_PER_TILE for the
// history and the scoring. The anchor is the target that was locked when the push BEGAN
// (rt_lock_push_anchor) and it holds for the whole push, which gives both feels that were asked for
// over the months:
//   * a flick is one step to the nearest neighbour on that side, and the next flick steps on from
//     where it landed -- what he described today, and what the 2026-08-10 step model was;
//   * holding the stick and sweeping still moves the lock round (the "seamless" wish), choosing among
//     the neighbours of where you started. An anchor that FOLLOWED the lock would instead chain away
//     one enemy per frame for as long as the stick was held.
// The anchor itself is never a candidate inside its own push: pushing away from someone means you
// want someone else. Nothing on that side = nothing happens.
static bool rt_directional_lock(float sx, float sy)
{
    if (rt_lock_target == NULL || obj_dude == NULL) {
        return false;
    }

    float slen = sqrtf(sx * sx + sy * sy);
    if (slen < 0.0001f) {
        return false;
    }
    sx /= slen;
    sy /= slen;

    Object* anchor = rt_lock_step_anchor();
    Rect ab;
    obj_bound(anchor, &ab);
    float ax = (ab.ulx + ab.lrx) * 0.5f;
    float ay = (ab.uly + ab.lry) * 0.5f;

    Object* best = NULL;
    float bestScore = 0.0f;

    // The current target only counts as "current" while it is on the pushed side of the anchor.
    // Sweep away from it and it stops being a contender at all, which makes turning round immediate.
    float currentScore = 0.0f;
    bool haveCurrent = false;

    // Same Perception reach as R3 itself, measured from the DUDE -- that is a question about what
    // this character can pick out, not about the step. Without it the stick could flick the lock onto
    // someone R3 would refuse to lock in the first place.
    int switchMaxDist = rt_lock_range();

    for (int i = 0; i < rt_actor_count; i++) {
        Object* c = rt_actors[i].obj;
        if (c == anchor || !rt_is_lockable(c) || !rt_lock_in_sight(c)) {
            continue;
        }

        // The CURRENT target is exempt: it must always be scoreable, or a target that drifted out
        // of range mid-fight could never be compared against and the lock would judder.
        if (c != rt_lock_target && obj_dist(obj_dude, c) > switchMaxDist) {
            continue;
        }

        float offDeg;
        if (!rt_lock_step_angle(ax, ay, c, sx, sy, &offDeg)) {
            continue;
        }

        float score = obj_dist(anchor, c) + offDeg / RT_LOCK_STEP_DEG_PER_TILE; // lower is better

        if (c == rt_lock_target) {
            currentScore = score;
            haveCurrent = true;
        }

        if (best == NULL || score < bestScore) {
            best = c;
            bestScore = score;
        }
    }

    if (best == NULL || best == rt_lock_target) {
        return false;
    }

    if (haveCurrent && currentScore - bestScore < RT_LOCK_STEP_SWITCH_MARGIN) {
        return false;
    }

    rt_lock_target = best;
    return true;
}

// WHY THE STICK WILL NOT GO THERE. "muzu vybirat jenom mezi lidmi, toho dogga proste neaimnu" --
// and there are four separate gates that can produce exactly that, with nothing on screen to tell
// them apart: the critter is not targetable, it is not LOCKABLE (same team, or a different team we
// cannot perceive), it is outside the Perception reach, or it is simply not in the direction being
// pushed. Rather than guess which one the dog trips, ask the engine to name it.
//
// Runs only when a push changed nothing, and at most once a second, so holding the stick cannot
// flood the log.
static void rt_lock_diag(float sx, float sy)
{
    static float lastAt = -1000.0f;
    if (rt_clock - lastAt < 1.0f) {
        return;
    }
    lastAt = rt_clock;

    float slen = sqrtf(sx * sx + sy * sy);
    if (slen < 0.0001f || obj_dude == NULL) {
        return;
    }
    sx /= slen;
    sy /= slen;

    // Measured exactly the way rt_directional_lock measures: from the push's anchor, so a candidate
    // line here carries the very score it competed with.
    Object* anchor = rt_lock_step_anchor();
    if (anchor == NULL) {
        return;
    }
    Rect ab;
    obj_bound(anchor, &ab);
    float ax = (ab.ulx + ab.lrx) * 0.5f;
    float ay = (ab.uly + ab.lry) * 0.5f;

    int reach = rt_lock_range();

    for (int i = 0; i < rt_actor_count; i++) {
        Object* c = rt_actors[i].obj;
        if (c == NULL || c == obj_dude || c == rt_lock_target || c == anchor) {
            continue;
        }

        int dist = obj_dist(obj_dude, c);
        if (dist > reach + 4) {
            continue; // far outside the question being asked
        }

        const char* why = NULL;
        if (!rt_is_targetable(c)) {
            why = "not-targetable";
        } else if (!rt_is_lockable(c)) {
            why = c->data.critter.combat.team == obj_dude->data.critter.combat.team
                ? "same-team"
                : "unperceived";
        } else if (dist > reach) {
            why = "out-of-reach";
        } else {
            float offDeg;
            if (!rt_lock_step_angle(ax, ay, c, sx, sy, &offDeg)) {
                why = "off-direction";
            } else {
                // ⚠️ A CANDIDATE THAT PASSES EVERY GATE AND STILL DOES NOT GET PICKED LEFT NO TRACE
                // AT ALL -- twice now that was exactly the case being complained about. Log it with
                // the score it lost by, in the same units the chooser uses (`step` = tiles from the
                // anchor; `dist` = tiles from you, for the reach rule).
                int step = obj_dist(anchor, c);
                trace_event(TRACE_INPUT, "aim-cand",
                    "pid=%d id=%d name=%s dist=%d step=%d off=%.0fdeg score=%.1f anchor=%d t=%.2f",
                    c->pid, c->id, object_name(c), dist, step, offDeg,
                    step + offDeg / RT_LOCK_STEP_DEG_PER_TILE, anchor->id, rt_clock);
            }
        }

        if (why != NULL) {
            trace_event(TRACE_INPUT, "aim-skip", "pid=%d id=%d name=%s dist=%d hostile=%d team=%d why=%s t=%.2f",
                c->pid, c->id, object_name(c), dist, rt_is_hostile(c) ? 1 : 0,
                c->data.critter.combat.team, why, rt_clock);
        }
    }

    // ⚠⚠ AND THE OTHER DIRECTION: A CRITTER THE WORLD HAS AND THIS LAYER DOES NOT.
    // "vzdyt tam je cokl" -- a dog standing in the open, outlined by the interact prompt, and not
    // one of the nine actors the registry held. The two enumerations are not the same enumeration:
    // the prompt walks obj_find_first_at, the actor refresh walks obj_create_list, and those differ
    // on OBJECT_HIDDEN and on FID_TYPE-vs-PID_TYPE (obj_create_list filters on the ART type of the
    // current frame, everything here tests the PROTO type). A critter that falls in that gap is
    // invisible to combat entirely: cannot be locked, cannot be shot at, does not fight back.
    //
    // So list them. pid/fid/flags name the reason on sight.
    for (Object* o = obj_find_first_at(obj_dude->elevation); o != NULL; o = obj_find_next_at()) {
        if (o == obj_dude || !rt_is_critter(o)) {
            continue;
        }

        if (obj_dist(obj_dude, o) > reach + 4) {
            continue;
        }

        bool known = false;
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].obj == o) {
                known = true;
                break;
            }
        }

        if (!known) {
            trace_event(TRACE_WARN, "aim-ghost",
                "pid=%d id=%d name=%s dist=%d fid=%08x fidType=%d flags=%08x hidden=%d elev=%d mapElev=%d t=%.2f",
                o->pid, o->id, object_name(o), obj_dist(obj_dude, o), o->fid, FID_TYPE(o->fid),
                o->flags, (o->flags & OBJECT_HIDDEN) != 0 ? 1 : 0, o->elevation, map_elevation, rt_clock);
        }
    }
}

// Latches a player attack intent (target + hit mode) instead of firing it
// outright. Shared by the mouse-click and gamepad paths. The central arbiter
// (rt_resolve_attacks) decides when it actually fires -- weighing it against the
// NPCs by Sequence -- and only then spends AP. Latching is what stops the
// player's input being dropped during a flinch or a locked frame: the intent
// persists (RT_INTENT_TTL) until it fires or expires.
static void rt_set_player_intent(Object* target, int hitMode, int hitLocation = HIT_LOCATION_UNCALLED)
{
    rt_player_intent_target = target;
    rt_player_intent_mode = hitMode;
    rt_player_intent_loc = hitLocation;
    rt_player_intent_ttl = RT_INTENT_TTL;
}

// Rebuilds the actor registry from the critters on the current elevation,
// carrying over the FSM state of any critter already tracked.
static void rt_refresh_registry()
{
    Object** list = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &list);

    // SANITIZE whoHitMe across ALL live critters FIRST, before anything below reads it. A
    // critter's whoHitMe can be a dangling pointer (a mid-fight free) OR union garbage like
    // 0x00000000FFFFFFFF -- the engine writes the 4-byte whoHitMeCid=-1 into the 8-byte whoHitMe
    // union (object_types.h), leaving a non-NULL TRASH pointer. The hostility check + engine-
    // target derivation below DEREFERENCE whoHitMe (isPartyMember(w) / rt_is_combatant(w)); doing
    // that on trash crashes (ACCESS_VIOLATION read 0xffffffff in isPartyMember). apply_damage's
    // retaliation reads it too. So clear it up front: any whoHitMe that isn't the dude and isn't a
    // live critter on this map (membership in `list` -- a pointer compare that never dereferences
    // the candidate, so comparing a trash value is safe) becomes NULL. list[] holds every live
    // critter incl. obj_dude. MUST run BEFORE the actor loop: it used to run after, so a whoHitMe
    // corrupted since the previous refresh was dereferenced by the hostility check before this
    // sweep could clean it -- the recurring dangling/garbage-whoHitMe crash.
    for (int i = 0; i < count; i++) {
        Object* c = list[i];
        if (c == NULL || FID_TYPE(c->fid) != OBJ_TYPE_CRITTER) {
            continue;
        }
        Object* w = c->data.critter.combat.whoHitMe;
        if (w == NULL || w == obj_dude) {
            continue;
        }
        bool liveW = false;
        for (int j = 0; j < count; j++) {
            if (list[j] == w) {
                liveW = true;
                break;
            }
        }
        if (!liveW) {
            c->data.critter.combat.whoHitMe = NULL;
        }
    }

    RtActor fresh[RT_MAX_ACTORS];
    int freshCount = 0;

    for (int i = 0; i < count && freshCount < RT_MAX_ACTORS; i++) {
        Object* obj = list[i];
        if (obj == obj_dude || !rt_is_critter(obj)) {
            continue;
        }

        RtActor* prev = NULL;
        for (int j = 0; j < rt_actor_count; j++) {
            if (rt_actors[j].obj == obj) {
                prev = &rt_actors[j];
                break;
            }
        }

        RtActor* a = &fresh[freshCount++];
        a->obj = obj;
        a->wants_attack = false;
        a->engine_target = NULL;
        bool prevHostile = false;
        if (prev != NULL) {
            a->state = prev->state;
            a->target = prev->target;
            a->ap = prev->ap;
            a->ap_regen = prev->ap_regen;
            a->run_ap_drain = prev->run_ap_drain;
            a->walk_ap_drain = prev->walk_ap_drain;
            a->no_path_since = prev->no_path_since;
            a->stop_check_at = prev->stop_check_at;
            a->unperceived_since = prev->unperceived_since;
            a->unreachable_tile = prev->unreachable_tile;
            a->unreachable_until = prev->unreachable_until;
            a->follow_retry_at = prev->follow_retry_at;
            a->look_next_at = prev->look_next_at;
            a->look_noise_at = prev->look_noise_at;
            a->look_noise_tile = prev->look_noise_tile;
            a->look_noise_seen = prev->look_noise_seen;
            a->run_winded = prev->run_winded;
            a->breather = prev->breather;
            a->breather_since = prev->breather_since;
            a->last_results = prev->last_results;
            a->weapon_checked = prev->weapon_checked;
            a->dead_since = prev->dead_since;
            a->dead_fid = prev->dead_fid;
            a->dead_frame = prev->dead_frame;
            a->rest_x = prev->rest_x;
            a->rest_y = prev->rest_y;
            a->flinch_accum = prev->flinch_accum;
            a->immune_until = prev->immune_until;
            a->stuck_tile = prev->stuck_tile;
            a->stuck_since = prev->stuck_since;
            a->stuck_x = prev->stuck_x;
            a->stuck_y = prev->stuck_y;
            a->next_attack_at = prev->next_attack_at;
            a->heal_next_at = prev->heal_next_at;
            a->weapon_up_until = prev->weapon_up_until;
            a->diag_mode = prev->diag_mode;
            a->diag_range = prev->diag_range;
            a->diag_dist = prev->diag_dist;
            a->diag_blocker = prev->diag_blocker;
            a->diag_mover = prev->diag_mover;
            a->osc_tile = prev->osc_tile;
            a->osc_prev_tile = prev->osc_prev_tile;
            a->osc_since = prev->osc_since;
            a->osc_count = prev->osc_count;
            a->bal_busy = prev->bal_busy;
            a->bal_cooldown = prev->bal_cooldown;
            a->bal_ap = prev->bal_ap;
            a->bal_approach = prev->bal_approach;
            a->bal_ready = prev->bal_ready;
            a->bal_breath = prev->bal_breath;
            a->bal_attacks = prev->bal_attacks;
            a->bal_seen = prev->bal_seen;
            a->script_turn_at = prev->script_turn_at;
            a->script_hold_until = prev->script_hold_until;
            a->script_fighting = prev->script_fighting;
            a->script_combatant = prev->script_combatant;
            a->script_turns = prev->script_turns;
            a->flee_retry_at = prev->flee_retry_at;
            a->fleeing = prev->fleeing;
            a->flee_blocked = prev->flee_blocked;
            a->wipe_pending = prev->wipe_pending;
            prevHostile = prev->hostile;
        } else {
            a->state = RT_IDLE;
            a->target = NULL;
            // Newly tracked critters start with a full pool.
            a->ap = stat_level(obj, STAT_MAXIMUM_ACTION_POINTS);
            a->ap_regen = 0.0f;
            a->run_ap_drain = 0.0f;
            a->walk_ap_drain = 0.0f;
            a->no_path_since = -1.0f;
            a->stop_check_at = 0.0f;
            a->unperceived_since = -1.0f;
            a->unreachable_tile = -1;
            a->unreachable_until = 0.0f;
            a->follow_retry_at = 0.0f;
            // Staggered, so a camp that comes into the registry together does not turn in unison;
            // and the shot that is already in the air is not news to it.
            a->look_next_at = rt_clock + 1.0f + roll_random(0, 30) * 0.1f;
            a->look_noise_at = -1.0f;
            a->look_noise_tile = -1;
            a->look_noise_seen = rt_noise_clock;
            a->script_turn_at = -1.0f;
            a->script_hold_until = 0.0f;
            a->script_fighting = false;
            a->script_combatant = false;
            a->script_turns = 0;
            a->flee_retry_at = 0.0f;
            a->fleeing = false;
            a->flee_blocked = false;
            a->wipe_pending = false;
            a->run_winded = false;
            a->breather = false;
            a->breather_since = -1.0f;
            // Seed from the current results so a critter first seen already
            // dead/downed doesn't register a phantom event.
            a->last_results = obj->data.critter.combat.results;
            a->weapon_checked = false;
            a->dead_since = -1.0f;
            a->dead_fid = -1;
            a->dead_frame = -1;
            a->rest_x = obj->x;
            a->rest_y = obj->y;
            a->flinch_accum = 0.0f;
            a->immune_until = 0.0f;
            a->stuck_tile = obj->tile;
            a->stuck_x = obj->x;
            a->stuck_y = obj->y;
            a->stuck_since = rt_clock;
            // INITIATIVE. Not 0 -- that made every actor ready to swing on the same frame it was
            // first seen, so a fight opened with a simultaneous volley and the cooldowns then kept
            // everyone in phase for the rest of it. That lock-step is what reads as "I hit him and
            // he hits me straight back": nobody is out of step with anybody.
            //
            // The delay is SEQUENCE (Perception x 2) measured against the DUDE's: a critter at
            // least as quick as you swings immediately and can genuinely get the drop on you, a
            // sluggish one waits. Plus a little jitter so two identical raiders don't march in
            // unison either. After this one seed, the differing cooldowns keep them drifting apart
            // on their own -- the exchange interleaves instead of alternating.
            // DIAG (initiative, the OTHER half): rt_wins_initiative already logs the contested
            // roll, but that only fires on a mutual trade and is rare. THIS is the half that runs
            // on every critter that joins a fight, and it had no number at all. `behind` is the
            // fraction of the span it waits (0 = as quick as the dude or quicker, swings at once;
            // 1 = fully outclassed), so a run of these says whether Sequence is doing anything at
            // your stats or whether every enemy is simply pinned at the ceiling.
            float initDelay = rt_initiative_delay(obj);
            {
                int foeSeq = stat_level(obj, STAT_SEQUENCE);
                int dSeq = obj_dude != NULL ? stat_level(obj_dude, STAT_SEQUENCE) : 0;
                trace_event(TRACE_COMBAT, "init-delay",
                    "pid=%d seq=%d dudeSeq=%d behind=%.2f delay=%.2fs span=%.2f t=%.2f",
                    obj->pid, foeSeq, dSeq,
                    dSeq > 0 ? (float)(dSeq - foeSeq) / (float)dSeq : 0.0f,
                    initDelay, rt_initiative_span, rt_clock);
            }
            a->next_attack_at = rt_clock + initDelay;
            // No opening delay on healing: a critter that walks into the fight already bloodied
            // should be able to patch itself immediately. The AP cost is the only opening gate.
            a->heal_next_at = 0.0f;
            // Weapon starts DOWN: a critter's opening shot of a fight gets the full raise, which is
            // the one place the animation actually reads as something (it draws a bead on you).
            a->weapon_up_until = 0.0f;
            a->diag_mode = -1;
            a->diag_range = -1;
            a->diag_dist = -1;
            a->diag_blocker = -2;
            a->diag_mover = -1;
            // ⚠️ `fresh` is an UNINITIALISED stack array and every field is assigned by hand, so a
            // field added to RtActor without a line here holds garbage. That is exactly what
            // happened to the balance counters: an NPC that joined after the fight started never
            // ran through rt_balance_begin, its buckets were stack junk, and the report printed
            // `-nan%` for every NPC while the DUDE line was fine.
            a->osc_tile = obj->tile;
            a->osc_prev_tile = 0;
            a->osc_since = rt_clock;
            a->osc_count = 0;
            a->bal_busy = 0.0f;
            a->bal_cooldown = 0.0f;
            a->bal_ap = 0.0f;
            a->bal_approach = 0.0f;
            a->bal_ready = 0.0f;
            a->bal_breath = 0.0f;
            a->bal_attacks = 0;
            a->bal_seen = rt_clock;
        }

        // Decide hostility TO THE DUDE from real intent. Hostility is STICKY -- combat_rt has no
        // de-aggro, so once genuinely hostile a critter stays hostile until it or the dude is
        // gone. A not-yet-hostile combatant becomes hostile when its combat target is on the
        // PLAYER'S SIDE -- whoHitMe is the dude, or a party follower:
        //   - op_attack(target) sets the attacker's whoHitMe = target (intextra.cc), so an NPC
        //     scripted to attack the dude/a follower reads as hostile; and the dude hitting an NPC
        //     sets that NPC's whoHitMe = dude (apply_damage) -- both real aggression at us.
        // We do NOT flag on the raw CRITTER_MANEUVER_ENGAGING bit anymore: op_attack sets ENGAGING
        // for ANY target, so a scripted NPC-vs-NPC brawl (or a guard attacking a thief) in a town
        // set it and combat_rt mis-read those critters as hostile to the dude -> phantom "combat
        // started, nothing happens" + a follower piling onto a neutral. Genuine on-sight aggro
        // still works: it routes through op_attack(dude) (whoHitMe=dude above) and combat_rt_-
        // notify_attack / the aggro spread set the cached hostile flag directly (carried via
        // prevHostile), neither of which needs the raw ENGAGING read.
        a->hostile = prevHostile;
        if (rt_is_combatant(obj)) {
            Object* w = obj->data.critter.combat.whoHitMe;
            if (!a->hostile && (w == obj_dude || (w != NULL && isPartyMember(w)))) {
                a->hostile = true;
            } else if (!a->hostile && w != NULL && w != obj_dude && rt_is_combatant(w)) {
                // NPC-vs-NPC: whoHitMe is a live non-player critter, so this critter has a
                // scripted/engine combat target off the player's side (op_attack:intextra.cc:1518
                // and apply_damage both overload whoHitMe as "my current fight target"). Drive it
                // to attack that critter via the normal CHASE/ATTACK path -- this is what makes
                // Gizmo's assassin actually attack Killian (and Killian retaliate) in realtime.
                a->engine_target = w;
            }
        }

        // TEMP DIAG: log the moment a critter first reads as hostile + WHY, to pin the Junktown
        // phantom-combat seed. STRIP once diagnosed.
        if (a->hostile && !prevHostile) {
            Object* w = obj->data.critter.combat.whoHitMe;
            trace_event(TRACE_FLOW, "newhostile",
                "pid=%d whoDude=%d whoParty=%d maneuver=%d prevCached=%d t=%.2f",
                obj->pid, w == obj_dude ? 1 : 0,
                (w != NULL && w != obj_dude && isPartyMember(w)) ? 1 : 0,
                obj->data.critter.combat.maneuver, prevHostile ? 1 : 0, rt_clock);
        }

        // Party members / followers fight FOR the dude (target the dude's enemies in
        // rt_update_actor) instead of just standing there. Stable per object, so recompute
        // fresh each refresh. A critter is never both friendly and hostile-to-the-dude.
        a->friendly = !a->hostile && isPartyMember(obj);
    }

    if (list != NULL) {
        obj_delete_list(list);
    }

    // Allied aggro spread (onlooker model, mirroring combatai_notify_onlookers):
    // a critter hostile to the dude pulls nearby SAME-TEAM combatants who can
    // witness it into the fight, so a whole faction/camp turns on the dude rather
    // than only the one that was attacked. The original raises the ENGAGING
    // maneuver on perceivers from curr_crit_list, but that list exists only during
    // turn-based combat (combat_rt never builds it) -- which is why aggro stopped
    // spreading here. Iterated to a fixpoint so it cascades across a spread-out
    // group (a Khan who sees a fighting Khan joins, and so on). Bounded: the
    // is_within_perception LoS check runs only for hostile x not-yet-hostile
    // same-team pairs, and it converges monotonically. Map-local by construction:
    // fresh[] holds only this elevation's critters, so a faction member elsewhere
    // is never in the set and never learns of it.
    bool spread = true;
    while (spread) {
        spread = false;
        for (int i = 0; i < freshCount; i++) {
            if (!fresh[i].hostile || !rt_is_combatant(fresh[i].obj)) {
                continue;
            }

            Object* anchor = fresh[i].obj;
            // Only a SEED hostile -- one genuinely fighting the player's side (its whoHitMe is the
            // dude or a follower) -- may recruit onlookers, whether by SIGHT or SOUND. A critter
            // that itself only got pulled in by the spread (whoHitMe not on our side) must NOT
            // re-broadcast: otherwise the recruitment CHAINS transitively to a fixpoint and, in an
            // open town where everyone can see everyone (Shady Sands), a single shot aggros the
            // entire same-team map in one pass -- which then gridlocks, 30 bodies all pathing to
            // the dude through each other and "walking in place". Gating both senses on the seed
            // bounds the crowd to those who can actually perceive a real player-fight. Quest-driven
            // "faction turns on you" is unaffected: scripted hostility goes through op_attack /
            // whoHitMe / team directly, not this onlooker spread.
            Object* anchorWho = anchor->data.critter.combat.whoHitMe;
            bool anchorIsSeed = anchorWho == obj_dude
                || (anchorWho != NULL && isPartyMember(anchorWho));
            if (!anchorIsSeed) {
                continue;
            }
            for (int j = 0; j < freshCount; j++) {
                if (fresh[j].hostile || !rt_is_combatant(fresh[j].obj)) {
                    continue;
                }

                Object* onlooker = fresh[j].obj;
                // Same-team critter joins if it can SEE the seed fight (vanilla, sight-primary
                // is_within_perception) OR HEAR its gunfire through a wall (earshot scaled by
                // Perception). Both only from a seed anchor (guarded above), so aggro doesn't
                // relay town-wide through freshly-recruited onlookers.
                if (onlooker->data.critter.combat.team == anchor->data.critter.combat.team
                    && (is_within_perception(onlooker, anchor)
                        || rt_hears_gunfire(onlooker, anchor))) {
                    fresh[j].hostile = true;
                    onlooker->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
                    spread = true;
                    // Trace the aggro CHAIN: who got pulled in by whom (team + how). Lets a
                    // "the whole town turned on me" report be traced back to the seed hostile.
                    trace_event(TRACE_FLOW, "aggro-spread",
                        "onlooker=%d(pid=%d) <- anchor=%d(pid=%d) team=%d saw=%d heard=%d t=%.2f",
                        onlooker->id, onlooker->pid, anchor->id, anchor->pid,
                        anchor->data.critter.combat.team,
                        is_within_perception(onlooker, anchor) ? 1 : 0,
                        rt_hears_gunfire(onlooker, anchor) ? 1 : 0, rt_clock);
                }
            }
        }
    }

    for (int i = 0; i < freshCount; i++) {
        rt_actors[i] = fresh[i];
    }
    rt_actor_count = freshCount;
}

// Advances one NPC's state machine for this frame.
static void rt_update_actor(RtActor* a, float dt)
{
    Object* npc = a->obj;

    // Clear last frame's attack intent; the RT_ATTACK branch re-records it if this
    // actor is still eligible to swing. The arbiter (run after this loop) reads it.
    a->wants_attack = false;

    // Anti-stunlock: accumulate/decay this NPC's continuous-flinch timer + open its
    // immunity window when focus-fire has been staggering it too long (see rt_play_hit_react).
    rt_update_flinch_state(npc, dt);

    // Watch for death/gib/knockdown transitions before any early return below, so
    // the stuck-lock watchdog can name what just happened (these are the suspected
    // orphaned-sequence triggers).
    rt_note_result_change(npc, a->last_results, npc->data.critter.combat.results);
    a->last_results = npc->data.critter.combat.results;

    // Defensive death reap: if the critter's HP is gone but it's still standing
    // (the normal attack -> apply_damage death sequence didn't switch it to a
    // corpse), kill it now. Normally gated on !anim_busy so a death fall in progress
    // is never interrupted; BUT a critter killed mid-walk can get its fall animation
    // stuck (competing walk/death sads -> anim_busy never returns to 0), leaving the
    // corpse standing "halfway" forever and still counted as a live hostile. So also
    // force the reap once it has been dead-and-standing past RT_DEAD_STANDING_TIMEOUT,
    // regardless of anim_busy. A real death fall finishes well within that window.
    // "Undead" catch: a critter playing a TERMINAL death anim (gib/blood/burned/etc.) but
    // NOT flagged dead -- a death whose damage never committed (e.g. a scripted NPC-vs-NPC
    // kill that bypassed the normal commit path). combat_rt keeps driving it as a live
    // combatant (it showed up st=ATTACK, frozen mid-death), so commit the death now:
    // critter_kill sets hp=0 + DAM_DEAD and lays it down as a corpse.
    // Skip a critter whose killing blow is still IN FLIGHT: its death animation was registered at
    // fire time but DAM_DEAD is committed a bit later (combat_commit_snapshot), so for that brief
    // window it shows a death anim while critter_is_dead is still false. Force-killing it here RACED
    // the commit -- critter_kill cut the death anim the instant it started (snap to a corpse, no
    // fall) and corrupted the sequence into the stuck busy=-1 freeze. Let the commit finish the
    // kill; the dead block below then plays/settles it normally. Only a GENUINELY orphaned death
    // (no pending commit -- e.g. a scripted NPC-vs-NPC kill that bypassed rt_do_attack) is force-
    // committed here, which is the case undead-kill was actually meant for.
    // ...and never one the engine itself says is only down or out: that body is alive by definition.
    if (rt_is_critter(npc) && !critter_is_dead(npc) && !rt_defender_in_flight(npc)
        && (npc->data.critter.combat.results & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) == 0
        && rt_is_terminal_death_anim(FID_ANIM_TYPE(npc->fid))) {
        critter_kill(npc, -1, true);
        trace_event(TRACE_FLOW, "undead-kill", "pid=%d anim=%d t=%.2f",
            npc->pid, FID_ANIM_TYPE(npc->fid), rt_clock);
    }

    // A corpse is "at rest" only when it's prone AND its fall animation has finished.
    // Anything else for a dead critter is broken: still STANDING (death anim never
    // played) or stuck HALFWAY through the fall (competing walk/death sads froze
    // anim_busy != 0) -- both leave a body upright, still counted as a live hostile.
    // NOTE the earlier bug: gating on !critter_is_prone MISSED the stuck-mid-fall case,
    // because a frozen fall frame's FID_ANIM_TYPE is a death anim, so critter_is_prone
    // returned true and the reap skipped exactly the "corpse stood halfway" body.
    // The standing offset, for the corpse grounding below (see RtActor.rest_x).
    if (rt_is_critter(npc) && !critter_is_dead(npc) && anim_busy(npc) == 0
        && FID_ANIM_TYPE(npc->fid) == ANIM_STAND) {
        a->rest_x = npc->x;
        a->rest_y = npc->y;
    }

    if (rt_is_critter(npc) && critter_is_dead(npc)) {
        int deadAnim = FID_ANIM_TYPE(npc->fid);
        bool stillMoving = deadAnim == ANIM_WALK || deadAnim == ANIM_RUNNING;

        // DIAG (vanishing-sprite hunt): a dead critter whose current frame has NO ART renders as
        // nothing -- "the body just isn't there / empty." This catches e.g. a critter forced into
        // a death animation its proto doesn't have art for. Coalesces per pid; strip once resolved.
        if (!art_exists(npc->fid)) {
            trace_event(TRACE_WARN, "blank-corpse", "pid=%d anim=%d fid=%08x t=%.2f",
                npc->pid, deadAnim, npc->fid, rt_clock);
        }

        // A dead critter should only ever be playing its DEATH FALL, which keeps
        // critter_is_prone() true for the whole animation. If it is instead upright in some
        // OTHER live animation, that anim will never end on its own and the death is stranded
        // until the 2s dead-standing timeout below:
        //   * killed MID-STEP -> its WALK/RUN sad keeps running; object_move advances the
        //     sub-tile offset (obj->x/y) every frame so the body slides/floats above its tile
        //     (and just zeroing the offset does NOT hold -- object_move re-applies it next frame);
        //   * killed mid its OWN ATTACK swing -> it stands there finishing the swing = the "died
        //     but kept attacking" body (its attack was registered a beat before the lethal hit
        //     committed, so combat_rt let it swing and the death anim was lost to that swing).
        // Cut the stray animation NOW so grounding the offset sticks and the reap below settles
        // the corpse THIS frame. A genuine death fall is never cut (critter_is_prone is true while
        // it plays, so strandedDeadAnim is false).
        // ⚠️ ...EXCEPT a fall that was only just queued. Since the reaction moved to the commit
        // point, the death fall is registered but has NOT started, so critter_is_prone() is still
        // false while anim_busy() is already true -- exactly the shape this test calls "stranded".
        // Cutting it ran anim_set_end, which fires the FORCED show_death callback, and the body
        // went flat in its standing pose the same instant the blow landed: "insta smrt". Traced
        // straight off the obj-flat backtrace (show_death <- anim_set_end <- anim_force_clear <-
        // rt_update_actor). The grace window lets the fall actually begin, after which
        // critter_is_prone() is true and this test correctly leaves it alone.
        bool strandedDeadAnim = !critter_is_prone(npc) && anim_busy(npc) != 0
            && !rt_death_just_registered(npc);
        if (strandedDeadAnim) {
            anim_force_clear(npc);
        }

        // Ground the body: zero the sub-tile offset left over by the animation we JUST CUT above.
        // anim.cc adds each frame's hot-spot offset to obj->x/y as an animation plays
        // (object_animate), so a critter killed mid-step keeps the walk's accumulated offset once
        // the cut stops it, and the body hangs above its tile -- that is the real float. Gated on
        // anim_busy too: a live animation owns the offset, and subtracting while it still runs just
        // fights it frame-by-frame (we subtract, it re-adds next frame -- the trace showed x/y
        // swinging -17..+9 with busy=-1 throughout). The cut above drops anim_busy to 0, so the
        // grounding lands on this same pass.
        //
        // ⚠️ ONLY for the cut case -- do NOT ground a body that died normally. A COMPLETED death
        // fall also ends on a non-zero offset, and that offset is the art's own per-rotation
        // displacement: it is where the fall art puts the body, not a bug. Trace 2026-08-09 checked
        // this: 20 deaths, 20 hits, every one anim=ANIM_FALL_BACK_BLOOD busy=0 flat=1, and the
        // offset was a pure function of the fid's rotation nibble (six rotations -> six fixed
        // values, e.g. rot 4 -> 2,4; rot 1 -> -4,11). Grounding those snapped every corpse back to
        // its tile centre and threw the fall displacement away -- exactly what the settle fix was
        // written NOT to do (corpses are legitimately off-tile).
        // ...and a SECOND way in, added 2026-08-10 after one body was still seen hanging: an
        // offset far bigger than a fall can produce. The fall's own displacement is the art's
        // per-rotation constant and we measured every one of them at <= 11 px; the leftovers from
        // a critter killed MID-STEP are a different order (traced that session: x=-27 y=17,
        // x=23 y=30, x=-18 y=32). Those did not come from a fall, so grounding them takes nothing
        // vanilla away. The stranded-anim test above misses them whenever the body ends up prone
        // or its animation has already stopped, which is exactly when nobody else will fix it.
        // Measured from where this body STANDS, not from the tile centre (RtActor.rest_x).
        int offX = npc->x - a->rest_x;
        int offY = npc->y - a->rest_y;
        bool strayOffset = offX > RT_CORPSE_FALL_OFFSET_MAX || offX < -RT_CORPSE_FALL_OFFSET_MAX
            || offY > RT_CORPSE_FALL_OFFSET_MAX || offY < -RT_CORPSE_FALL_OFFSET_MAX;

        // ...and a THIRD: a fall that was cut before its last frame (see rt_death_fall_was_cut --
        // the dialogue-interrupts-a-death case). Its leftover offset can be well UNDER the stray
        // threshold above, because the body simply never travelled the rest of the way, so
        // neither of the other two tests sees it. Grounding it puts the body on its tile instead
        // of part-way through a fall it will never finish.
        if ((strandedDeadAnim || strayOffset || rt_death_fall_was_cut(npc))
            && (offX != 0 || offY != 0) && anim_busy(npc) == 0) {
            Rect off;
            obj_offset(npc, -offX, -offY, &off);
            tile_refresh_rect(&off, npc->elevation);
        }

        // DIED WHILE ALREADY ON THE GROUND -- the "corpse stuck in the air / never turns into a
        // body" case. A critter that is already prone gets NO death animation registered (there is
        // nothing to play; it is lying down already), so it simply keeps the last frame of its
        // KNOCKDOWN. The at-rest test below then reads "prone and not animating" as "already a
        // corpse" and the reap is skipped forever. Trace 2026-08-02: id=1224 was knocked down at
        // s=121.7, finished its fall at s=123.8, died at s=129.8 -- and sat on anim=20 (ANIM_FALL_
        // BACK) frame=10 for the remaining 20+ seconds of the log, never converted.
        //
        // ANIM_FALL_BACK / ANIM_FALL_FRONT are precisely the two poses critter_kill's own prone
        // branch knows how to settle (-> FALL_BACK_SF / FALL_FRONT_SF), so treating them as NOT at
        // rest routes the body into the existing reap and it converts on the next pass. Self-
        // terminating: once converted the anim is an SF death frame, this is false, and it rests.
        int restAnim = FID_ANIM_TYPE(npc->fid);
        bool onKnockdownPose = restAnim == ANIM_FALL_BACK || restAnim == ANIM_FALL_FRONT;

        // A CUT fall is not at rest either, however still it looks. Grounding its offset (above)
        // only fixes WHERE the body is; the sprite is still frozen on a middle frame of its death
        // animation and will never advance, so the corpse lies there mid-collapse forever.
        // Traced 2026-08-10 after the first half of this fix shipped: id=1652 sat on
        // `anim=23 frame=5 flat=1 x=0 y=0` for 36 straight seconds -- on the tile (grounded, so the
        // offset half worked) but stuck at frame 5 of a 22-frame death. Routing it into the reap
        // below lets critter_kill settle it onto the proper static death frame.
        // Self-terminating: an _SF corpse frame is a single-frame art, so rt_death_fall_was_cut's
        // `frameCount > 1` guard is false afterwards and it rests.
        bool atRest = critter_is_prone(npc) && anim_busy(npc) == 0 && !onKnockdownPose
            && !rt_death_fall_was_cut(npc);
        if (!atRest) {
            // The clock restarts every time the death animation ADVANCES, so only a frozen one
            // times out (see RtActor.dead_frame).
            if (a->dead_since < 0.0f || npc->fid != a->dead_fid || npc->frame != a->dead_frame) {
                a->dead_since = rt_clock;
                a->dead_fid = npc->fid;
                a->dead_frame = npc->frame;
            }
            // Immediately if there's no animation to wait on (already standing, or the walk was
            // just cut above); after the timeout if a fall animation is genuinely stuck.
            bool stuck = rt_clock - a->dead_since >= RT_DEAD_STANDING_TIMEOUT;
            // ⚠️ A CUT FALL CANNOT BE SETTLED BY critter_kill (found 2026-08-10, second pass).
            // Its prone branch only knows how to convert ANIM_FALL_BACK / ANIM_FALL_FRONT into
            // their _SF frames -- for any OTHER death pose (ANIM_BIG_HOLE and friends) it changes
            // nothing at all, so routing a cut fall into the reap ran a no-op and the body stayed
            // frozen mid-collapse. Traced: id=2471 held `anim=23 frame=13 flat=1 x=0 y=0` for 72
            // seconds -- grounded (so the offset half of the fix worked) but stuck on frame 13 of
            // a 22-frame death. Finish the animation ourselves instead: jump to its LAST frame,
            // which is exactly where the fall would have ended, and keeps the proto's own death
            // art (blood pool and all) rather than substituting a generic corpse.
            if (rt_death_fall_was_cut(npc)) {
                CacheEntry* handle;
                Art* art = art_ptr_lock(npc->fid, &handle);
                if (art != NULL) {
                    int last = art_frame_max_frame(art) - 1;
                    art_ptr_unlock(handle);
                    if (last > 0) {
                        Rect r;
                        obj_set_frame(npc, last, &r);
                        tile_refresh_rect(&r, npc->elevation);
                    }
                }
                // ⚠️ AND LAY THE BODY DOWN -- the half this was missing since 2026-08-10, and the
                // "one of them is still standing" report. Jumping to the last frame fixes the POSE;
                // it does nothing about the object still being an upright, blocking one, because
                // the only thing that ever flattens a corpse is show_death (actions.cc), and that
                // is a `register_object_must_call` callback on a death sequence we just bypassed.
                // So the body sat on its correct final death frame and still stood there, in the
                // way. Traced: `anim=23 frame=21 flat=0` held for hundreds of samples across
                // several critters (trace_2026-08-15_181907) -- frame 21 of 22 IS the finished
                // pose, so this was never a stuck animation, it was an unflattened one.
                //
                rt_settle_corpse_flat(npc);
                a->dead_since = -1.0f;
                trace_event(TRACE_FLOW, "death-finish", "pid=%d anim=%d flat=%d t=%.2f",
                    npc->pid, FID_ANIM_TYPE(npc->fid),
                    (npc->flags & OBJECT_FLAT) != 0 ? 1 : 0, rt_clock);
            } else if (anim_busy(npc) == 0 || stuck) {
                // Clear only THIS critter's stuck/standing sequence (not a global anim_stop,
                // which would interrupt every other actor) so critter_kill snaps it to the
                // final static death frame cleanly.
                register_clear(npc);
                critter_kill(npc, -1, true);
                trace_event(TRACE_FLOW, "corpse-reap", "pid=%d stuck=%d moving=%d animWas=%d t=%.2f",
                    npc->pid, stuck ? 1 : 0, stillMoving ? 1 : 0, anim_busy(npc), rt_clock);
                a->dead_since = -1.0f;
            }
        } else {
            // AT REST -- but resting is not the same as settled. This branch is reached by a death
            // that played out completely and correctly, which is exactly the case where nobody
            // notices the body is still an upright, blocking object: show_death is the only thing
            // that flattens a corpse, and it only runs if the death sequence carried its callback
            // to the end. When it did not, we land here with a perfect final frame and flat=0.
            //
            // ⚠️ THIS, NOT THE CUT-FALL PATH, IS WHAT THE "one of them is still standing" REPORT WAS.
            // The cut-fall branch above already flattens, and the trace proves those corpses never
            // went through it: trace_2026-08-15_181907 has only 3 `death-finish` lines and none of
            // them is for the standing bodies. They were `atRest` the whole time.
            //
            // Safe here by construction: atRest already requires anim_busy == 0 and a prone pose,
            // so there is no animation left to render behind the floor.
            if (rt_settle_corpse_flat(npc)) {
                trace_event(TRACE_FLOW, "corpse-settle", "pid=%d id=%d anim=%d frame=%d t=%.2f",
                    npc->pid, npc->id, FID_ANIM_TYPE(npc->fid), npc->frame, rt_clock);
            }
            a->dead_since = -1.0f;
        }
    } else {
        a->dead_since = -1.0f;
    }

    // The standup pass that used to live here is GONE -- it is rt_reaction_tick's PRONE branch
    // now, for everybody at once. All this does is make sure the layer KNOWS about a body that got
    // knocked down without asking it (a script, an explosion, a turn-based leftover, a reload).
    //
    // The old pair of hand-rolled timers (a->prone_since / a->prone_fall_end) and their duplicate
    // in combat_rt_update are what this replaces. They were duplicated per-critter and per-dude,
    // they were re-derived from the fid every frame, and the long comment that used to sit here
    // explained how clearing the wrong one let a knockdown pin the player indefinitely. The
    // reaction entry owns both clocks now (dwellUntil / groundAnchor) and nothing else writes them.
    rt_reaction_adopt(npc);

    if (!rt_is_combatant(npc)) {
        a->state = RT_IDLE;
        a->target = NULL;
        return;
    }

    // Leash / de-aggro: a chasing critter gives up and returns to IDLE once its target is dead,
    // has slipped past the (very wide) outer bound, or -- the rule that actually does the work --
    // it can no longer PERCEIVE anything to fight. Without this it chases forever, combat never
    // goes cold, and the warm combat bit stays stuck. Hostility is sticky, so RT_IDLE below
    // re-aggros the moment it perceives a target on the player's side again.
    if (a->state != RT_IDLE) {
        // ⚠️ THE ONLY DE-AGGRO PATH THAT USED TO BE SILENT. The other two (disengage,
        // unreachable) each trace; this one dropped the target with nothing in the log, so a
        // chaser that kept losing its target and re-acquiring it -- running to the player,
        // forgetting them, walking back, aggroing again -- looked like a movement bug and could
        // not be told apart from one. Measured in trace_2026-08-23_003610: id=2295 went
        // st=2/tgt=18000/dist=1 to st=0/tgt=0 with NO disengage and NO unreachable line, while
        // walk-seg showed it shuttling 23309<->22913 every ~0.46s ("furt chodím sem a tam").
        bool targetGone = !rt_is_combatant(a->target);
        int leash = rt_chase_range(npc) + RT_DEAGGRO_MARGIN;
        int tgtDist = obj_dist(npc, a->target);
        bool giveUp = targetGone || tgtDist > leash;

        if (giveUp) {
            trace_event(TRACE_FLOW, "deaggro", "id=%d pid=%d why=%s dist=%d leash=%d tgtDead=%d tgtKO=%d tgtHp=%d t=%.2f",
                npc->id, npc->pid, targetGone ? "target-not-combatant" : "out-of-leash",
                tgtDist, leash,
                (a->target != NULL && (a->target->data.critter.combat.results & DAM_DEAD) != 0) ? 1 : 0,
                (a->target != NULL && (a->target->data.critter.combat.results & DAM_KNOCKED_OUT) != 0) ? 1 : 0,
                a->target != NULL ? stat_level(a->target, STAT_CURRENT_HIT_POINTS) : -999,
                rt_clock);
        }

        // VANILLA STOP RULE. Throttled (it traces line of sight) and held for a grace window, so
        // stepping behind a doorframe doesn't disengage half the map for one frame.
        if (!giveUp && rt_clock >= a->stop_check_at) {
            a->stop_check_at = rt_clock + RT_STOP_CHECK_SECONDS;
            if (rt_wants_to_stop(npc, a->target)) {
                if (a->unperceived_since < 0.0f) {
                    a->unperceived_since = rt_clock;
                }
            } else {
                a->unperceived_since = -1.0f;
            }
        }

        if (!giveUp && a->unperceived_since >= 0.0f
            && rt_clock - a->unperceived_since >= RT_STOP_GRACE_SECONDS) {
            trace_event(TRACE_FLOW, "disengage", "id=%d pid=%d tile=%d dist=%d t=%.2f",
                npc->id, npc->pid, npc->tile,
                rt_is_combatant(a->target) ? obj_dist(npc, a->target) : -1, rt_clock);
            giveUp = true;
        }

        if (giveUp) {
            a->state = RT_IDLE;
            a->target = NULL;
            a->no_path_since = -1.0f;
            a->unperceived_since = -1.0f;
        } else if (rt_target_unreachable(a)) {
            // UNREACHABLE TARGET -- the third de-aggro condition, added once the trace proved
            // this was the frozen chaser's cause (3277 no-path failures, id=687 rooted on one
            // tile for the whole session with its target 39 tiles away in another area). The
            // distance leash above cannot catch it: the target was well inside chase range, it
            // simply could not be walked to. Without this the actor stands in CHASE forever,
            // re-running a doomed pathfind EVERY FRAME. Dropping to IDLE also stops that burn;
            // hostility stays sticky, so it re-aggros the moment something reachable shows up.
            trace_event(TRACE_FLOW, "unreachable", "id=%d pid=%d tile=%d tgt=%d dist=%d t=%.2f",
                npc->id, npc->pid, npc->tile, a->target->tile, obj_dist(npc, a->target), rt_clock);
            a->state = RT_IDLE;
            a->target = NULL;
            a->no_path_since = -1.0f;
            a->unperceived_since = -1.0f;
            // Latch it so the RT_IDLE branch cannot re-acquire on the next frame and spin.
            a->unreachable_tile = npc->tile;
            a->unreachable_until = rt_clock + RT_UNREACHABLE_RETRY_SECONDS;
        }
    }

    // Regenerate this actor's AP pool every frame (it's a pool, not a recovery
    // timer, so it ticks even mid-animation -- refill time naturally overlaps the
    // attack swing). Done before the anim_busy guard so AP keeps coming back
    // while the critter is animating.
    //
    // NPC regen deliberately does NOT pause while moving, unlike the dude's (combat_rt_update).
    //
    // It DID for one build, mirroring him exactly, and the trace killed it: with regen at 0
    // during the walk and the walk drain floored at the attack reserve, a chasing melee critter
    // parked on the floor and could never leave it -- trace_2026-08-03_222723 shows id=1756 and
    // id=1323 (both unarmed, punch = 3 AP) pinned at ap=3 for their entire chase, while the
    // ranged hostiles sat at ap=8-10 and took 22 of the 25 ATTACK samples. The dude escapes the
    // same trap only because rt_move_blocked STOPS him at 0 AP so he is forced to stand and
    // recover; an NPC has no such stop by design (an AI frozen mid-chase is the "enemy runs up
    // and just looks at me" bug), so for NPCs the pause had no exit.
    //
    // PROXIMITY REGEN, the NPC half of the dude's: measured to this actor's own target, so a
    // chaser's pool fills FASTER the closer it gets and it arrives ready to swing instead of
    // scraping the reserve. That is the whole point of the switch away from the standing bonus
    // -- see rt_proximity_regen_mult. An idle critter has no target, gets the neutral 1.0.
    int npcAnimType = FID_ANIM_TYPE(npc->fid);
    bool npcInFight = a->state != RT_IDLE;
    int npcEnemyDist = (npcInFight && rt_is_combatant(a->target)) ? obj_dist(npc, a->target) : -1;
    bool npcMoving = npcAnimType == ANIM_WALK || npcAnimType == ANIM_RUNNING;

    // ⚠️ A FLEEING CRITTER IS BUDGETED EXACTLY LIKE THE DUDE (2026-10-05, his call: "ať je to férové
    // jako ve vanille"). In vanilla the chase is fair because both sides move on the same coin -- AP,
    // tile for tile, turn for turn -- so you can run it down or shoot it. Here the NPC rules were
    // written for CHASERS (never below an attack's reserve, never stopped -- see below), and a
    // fleeing critter on them never tired: past the reserve its legs were free, while the dude
    // stops dead at 0. So while it flees it regenerates on the DUDE's curves (movingCurve = true),
    // measured to the dude it is running from, pays the same drain all the way to 0, and stands
    // when it is dry until resume_ap (rt_flee_step).
    if (a->fleeing && obj_dude != NULL) {
        npcEnemyDist = obj_dist(npc, obj_dude);
    }
    rt_regen_ap(npc, &a->ap, &a->ap_regen, stat_level(npc, STAT_MAXIMUM_ACTION_POINTS),
        dt * rt_proximity_regen_mult(npcEnemyDist, npcMoving, a->fleeing ? true : rt_regen_moving_npc));

    // Movement drain, the NPC half of the dude's run/walk cost (combat_rt_update). Keyed on the
    // live animation exactly like his, so it burns only while the cycle is actually playing.
    // WALKING COSTS TOO now (the user asked for full symmetry) -- it used to be free for NPCs.
    //
    // The reserve is the one thing the player does NOT get: an NPC never spends below one
    // attack's worth (rt_attack_reserve_ap), whichever gait it is in, so a chaser always
    // arrives able to hit. At the floor rt_npc_sprint_ok drops the sprint to a walk, and the
    // walk drain simply stops -- movement itself is never blocked for an NPC (no equivalent of
    // the dude's rt_move_blocked), because an AI frozen mid-chase is the "enemy runs up and
    // just looks at me" bug we have already chased down twice. It keeps closing, it just
    // arrives with exactly one swing in hand and has to stand still to earn the next.
    //
    // Only combatants pay (a->state != RT_IDLE), matching the regen gate above.
    //
    // A FLEEING critter keeps no reserve: it will not attack while it runs (vanilla's ai_attack
    // refuses), so it pays down to 0 like the dude -- see the regen note above.
    {
        int reserve = a->fleeing ? 0 : rt_attack_reserve_ap(npc);
        if (npcInFight && npcAnimType == ANIM_RUNNING) {
            a->run_ap_drain += rt_effective_run_cost(npc) * dt;
            while (a->run_ap_drain >= 1.0f && a->ap > reserve) {
                a->ap--;
                a->run_ap_drain -= 1.0f;
            }
            if (a->ap <= reserve) {
                a->run_ap_drain = 0.0f; // at the floor: stop banking drain it can't spend
                a->run_winded = true;   // must recover a margin before sprinting again
            }
            a->walk_ap_drain = 0.0f;
        } else if (npcInFight && npcAnimType == ANIM_WALK) {
            a->walk_ap_drain += rt_effective_walk_cost() * dt;
            while (a->walk_ap_drain >= 1.0f && a->ap > reserve) {
                a->ap--;
                a->walk_ap_drain -= 1.0f;
            }
            if (a->ap <= reserve) {
                a->walk_ap_drain = 0.0f; // at the floor: stop banking drain it can't spend
            }
            a->run_ap_drain = 0.0f;
        } else {
            a->run_ap_drain = 0.0f;
            a->walk_ap_drain = 0.0f;
        }
        // Recovery is checked whatever the gait, so a winded critter can catch its breath while
        // walking after you (or standing). Without the margin, one regen tick would re-enable the
        // sprint immediately and the pool would just sit at the floor forever.
        if (a->run_winded && a->ap >= reserve + RT_NPC_RUN_RESUME_MARGIN) {
            a->run_winded = false;
        }

        // BREATHER LATCH. run_winded drops the SPRINT; this drops movement altogether, which is
        // what the dude's rt_move_blocked does to him. Without it the reserve made the pool a
        // floor the critter could sit on for ever while still walking -- "enemy chodi za mnou
        // jako zombici", and the reason camping beat manoeuvring: only one side ever had to stop.
        //
        // The critter stops with `reserve` in hand and leaves with `reserve + margin`, so a pause
        // is never wasted time -- it is a shooter banking a shot for the moment you step out.
        //
        // Evaluated for BOTH gaits and while standing, like the recovery above: a critter that
        // walks itself down to the reserve must latch on the step that gets it there, not on the
        // next frame it happens to be running.
        // ⚠️ THE DISTANCE GATE IS LOAD-BEARING -- it shipped missing and immediately reproduced
        // the exact bug this feature was designed around. Walking costs AP at EVERY distance, so a
        // critter crossing a map trends down to its reserve long before it arrives; ungated, the
        // latch then stopped it out there. Measured in trace_2026-08-29_170410: 21 of 75
        // activations fired at 16+ tiles, and the Hub Police Officers came out of a 12 s fight at
        // `approach=98% atk=0 breath=66%` -- two thirds of it standing still, 16 tiles away, never
        // arriving, never attacking. That is "enemy runs up and just looks at me" with an extra
        // step, and it is the one outcome this whole feature was not allowed to produce.
        //
        // So a pause is only allowed INSIDE the bubble, where standing still is a tactic rather
        // than a failure to arrive: a melee critter a few tiles off is in the fight, one 25 tiles
        // away is still travelling to it. Beyond it the old guarantee holds in full -- movement is
        // never blocked and the critter arrives with its one-attack reserve in hand.
        //
        // Gated HERE rather than at the movement branch so `breath=` in the balance line keeps
        // meaning "seconds actually spent stopped". A latch that set but suppressed nothing would
        // report time the critter spent walking normally.
        // ⚠️ AND ONLY WHILE CHASING. The breather's sole effect is suppressing CHASE movement;
        // in RT_ATTACK it latches, changes nothing, and then cannot recover either -- an attacking
        // critter keeps spending its pool on attacks, so it sits below the reserve until the 3 s
        // cap expires and immediately re-latches. Measured in trace_2026-08-30_185109: exits ran
        // 7 timeout to 4 recovered, and the timeouts were attackers, not walkers. That inverted
        // the one ratio I use to judge whether the margin is reachable, and it made `breath=`
        // report seconds during which nothing was being suppressed at all.
        // Never for a FLEEING critter: the breather is a chaser's tactical pause, and one that flees
        // already stops on an empty pool (flee_blocked, rt_flee_step).
        bool breatherAllowed = a->state == RT_CHASE && !a->fleeing
            && npcEnemyDist >= 0 && npcEnemyDist <= rt_npc_breather_tiles;

        if (npcInFight && breatherAllowed && !a->breather && a->ap <= reserve) {
            a->breather = true;
            a->breather_since = rt_clock;
            a->wipe_pending = true; // out of breath: wipes its forehead once its step ends (rt_wipe_forehead)
            trace_event(TRACE_COMBAT, "breather", "enter id=%d pid=%d ap=%d reserve=%d dist=%d t=%.2f",
                npc->id, npc->pid, a->ap, reserve, npcEnemyDist, rt_clock);
        } else if (a->breather) {
            // ⚠️ THE TARGET IS CLAMPED TO THE CRITTER'S OWN MAXIMUM. reserve + margin is not
            // reachable for everyone: a 5 AP weapon on an 8 AP critter needs 8, and one point more
            // of margin would put the target above anything it can ever hold. Unclamped, those
            // critters could only ever leave the breather by TIMEOUT -- a 3 s pause every single
            // time, for ever. Same clamp, same reason, as the dude's resume_ap against his max.
            int breatherTarget = reserve + rt_npc_breather_margin;
            int npcMaxAp = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);
            if (breatherTarget > npcMaxAp) {
                breatherTarget = npcMaxAp;
            }

            // Three ways out, and the ORDER is only for the trace label -- any one ends it.
            const char* why = NULL;
            if (!npcInFight) {
                why = "fight-over";
            } else if (!breatherAllowed) {
                // Target moved away (or was lost): stop resting and start travelling again.
                why = "out-of-bubble";
            } else if (a->ap >= breatherTarget) {
                why = "recovered";
            } else if (rt_clock - a->breather_since >= rt_npc_breather_max) {
                // The safety valve. If this fires often the margin is unreachable for that critter
                // (regen too slow, or something is still draining it) -- which is exactly the
                // frozen-chaser shape, caught before it can be seen on screen.
                why = "timeout";
            }

            if (why != NULL) {
                trace_event(TRACE_COMBAT, "breather", "exit id=%d pid=%d why=%s ap=%d reserve=%d held=%.2f t=%.2f",
                    npc->id, npc->pid, why, a->ap, reserve, rt_clock - a->breather_since, rt_clock);
                a->breather = false;
                a->breather_since = -1.0f;
            }
        }

        if (a->breather) {
            a->bal_breath += dt;
        }
    }

    // Stuck-move watchdog. Whenever the critter actually changes tile, note the new tile and
    // the time -- a moving body resets this constantly. If it's mid-WALK/RUN but has held the
    // same tile past RT_STUCK_MOVE_TIMEOUT, it's frozen (crowd gridlock: its next tile is
    // permanently occupied by another chaser), so force-clear the walk. That drops anim_busy,
    // and the FSM below re-plans next frame (repathing around, or de-aggroing via the leash).
    // Only movement anims are cut -- an attack swing / hit-react / standup stays on its tile
    // legitimately and must not be interrupted here. Gated to CHASE/ATTACK actors (a->state !=
    // RT_IDLE): an IDLE townsperson's walk is script-driven wandering, not combat_rt's doing,
    // and cutting it would make the town jerky (the log showed idle NPCs being unstuck at eng=0).
    // ⚠️ THE REVERSAL DETECTOR NEEDS A REAL TILE CHANGE, and it is now its own branch because
    // of that. It used to live inside the progress test below -- fine while that test only fired on
    // a tile change, fatal once the test was widened to sub-tile movement: the history then shifted
    // every frame the critter crept forward, so osc_tile and osc_prev_tile collapsed onto the
    // CURRENT tile and `npc->tile == a->osc_prev_tile` became trivially true. 1364 phantom
    // reversals in one session, every one of them logging a=b (the same tile twice), which is the
    // tell: a real reversal is between two DIFFERENT tiles.
    if (npc->tile != a->osc_tile) {
    // TEMP DIAG (oscillation hunt). The critter just changed tile: if the tile it moved ONTO is
    // the one it occupied two steps ago, that is a REVERSAL -- it went back where it came from.
    // Counted in a run, and reported with whether the DUDE moved during that run, because a
    // reversal while the player is walking around is simply following, and only a run of them
    // against a STATIONARY player is the bug. dudeStill=1 is the case to chase.
    if (npc->tile == a->osc_prev_tile && a->osc_prev_tile != 0) {
        if (a->osc_count == 0 || rt_clock - a->osc_since > RT_OSC_WINDOW_SECONDS) {
            a->osc_since = rt_clock;
            a->osc_count = 0;
            rt_osc_dude_tile = obj_dude != NULL ? obj_dude->tile : -1;
        }
        a->osc_count++;
        if (a->osc_count >= RT_OSC_REPORT_AT) {
            trace_event(TRACE_FLOW, "oscillate",
                "id=%d pid=%d n=%d span=%.2f a=%d b=%d dudeStill=%d st=%d dist=%d t=%.2f",
                npc->id, npc->pid, a->osc_count, rt_clock - a->osc_since,
                npc->tile, a->osc_tile,
                (obj_dude != NULL && obj_dude->tile == rt_osc_dude_tile) ? 1 : 0,
                a->state, obj_dude != NULL ? obj_dist(npc, obj_dude) : -1, rt_clock);
            a->osc_count = 0; // one line per run, not per step
        }
    } else if (rt_clock - a->osc_since > RT_OSC_WINDOW_SECONDS) {
        a->osc_count = 0;
    }
    a->osc_prev_tile = a->osc_tile;
    a->osc_tile = npc->tile;
    }

    if (npc->tile != a->stuck_tile || npc->x != a->stuck_x || npc->y != a->stuck_y) {

        a->stuck_tile = npc->tile;
        a->stuck_x = npc->x;
        a->stuck_y = npc->y;
        a->stuck_since = rt_clock;
    } else if (anim_busy(npc) == 0) {
        // ⚠️ STANDING STILL IS NOT BEING STUCK, AND THE TIMER MUST NOT RUN WHILE IT IS.
        //
        // Without this branch stuck_since only ever reset on a TILE CHANGE, so it kept counting
        // through every legitimate pause -- an attack cooldown, the initiative delay, a breather,
        // simply standing in RT_ATTACK. Any critter that held its tile for over
        // RT_STUCK_MOVE_TIMEOUT arrived at its NEXT walk with the budget already spent, and the
        // watchdog cut that walk on its first frame.
        //
        // anim_force_clear then drops the part-crossed sub-tile offset, so the body SNAPS BACK to
        // the tile it started from: lurch forward, jump back, walk again. That is the rubberbanding
        // -- "priblizuje se k hraci, ale pokazde se vrati jakoby zpatky". Measured in
        // trace_2026-08-30_220545: unstick-move fired on 40 of 44 walk segments, every one of them
        // about 0.01 s after the segment was issued against a 1.25 s timeout, and with
        // FLOW/oscillate at ZERO -- so nothing was re-pathing backwards, the moves were being killed
        // before they could start.
        //
        // The watchdog asks "is a move in flight and failing to make progress". Time spent with no
        // move in flight is not an answer to that question.
        a->stuck_since = rt_clock;
    } else if (a->state != RT_IDLE) {
        // ⚠️ PROGRESS IS THE SUB-TILE OFFSET, NOT THE TILE. This used to reset only when `tile`
        // changed, and a tile change is a rare event: measured in trace_2026-08-30_225022, a Ghoul
        // needs about 2.5 s to cross one tile (npcCross reports 760 ms of animation, but the actual
        // frame cadence is slower), against a 1.25 s timeout. So the watchdog fired EVERY crossing,
        // roughly halfway over -- id=1979 reached sub-tile (14,11) and was cut 0.06 s later, four
        // times running at a metronomic 2.46 s.
        //
        // anim_force_clear then discards the part-crossed offset, so the body jumps back to where
        // the step began: creep forward, snap back, creep forward. "priblizuje se k hraci, ale
        // pokazde se vrati jakoby zpatky." It was never gridlock -- the watchdog was mistaking slow
        // for stuck and then causing the very symptom it exists to clear.
        //
        // ⚠️ AND IT IS NOT THE SHOOTING. The unstick bursts looked like they followed the player's
        // attacks, but per critter they are periodic and evenly spaced, and they continue with no
        // attack anywhere near them. rt_play_hit_react does clear a defender's walk by design, but
        // that is not what this was.
        int at = FID_ANIM_TYPE(npc->fid);
        if ((at == ANIM_WALK || at == ANIM_RUNNING)
            && rt_clock - a->stuck_since >= RT_STUCK_MOVE_TIMEOUT) {
            anim_force_clear(npc);
            a->stuck_since = rt_clock;
            a->stuck_x = npc->x;
            a->stuck_y = npc->y;
            trace_event(TRACE_FLOW, "unstick-move", "id=%d pid=%d tile=%d t=%.2f",
                npc->id, npc->pid, npc->tile, rt_clock);
        }
    }

    // Never issue a new action while this actor is mid-animation OR knocked down. The knockdown
    // clause is what makes the standup animation actually play: during the ground dwell the fall is
    // done so anim_busy==0, and without this the FSM below would fire an attack that pre-empts the
    // pending standup (rt_stand_up runs from the standup pass above) -- the critter snapped straight
    // from the fall into shooting with no getting-up animation. Prone clears once rt_stand_up
    // registers the standup (flag dropped, anim moves to 36/37), so the FSM resumes right after.
    if (anim_busy(npc) != 0 || critter_is_prone(npc)) {
        return;
    }

    // VANILLA'S ORDER FOR A TURN, top to bottom: the script's combat_p_proc first -- if it takes the
    // turn (script_overrides) combat_ai never runs -- then combat_ai, which flees before anything
    // else, then uses drugs, then attacks.

    // Its script has this turn (rt_script_turn_holds): no decision until it is done with it.
    if (rt_script_turn_holds(a)) {
        return;
    }

    // FLEEING (rt_flee_step): FLEEING, hurt_too_much or below min_hp runs, and does nothing else.
    // Out of the fight it is not fleeing -- and not billed like the dude -- any more.
    if (a->state != RT_IDLE) {
        if (rt_flee_step(a, npc)) {
            return;
        }
    } else {
        a->fleeing = false;
        a->flee_blocked = false;
    }

    // HEAL BEFORE FIGHTING, which is where vanilla puts it too: ai_check_drugs runs inside combat_ai,
    // before the target/attack decision. Sits OUTSIDE the state switch on purpose -- a critter
    // bleeding out mid-CHASE should stop and patch itself, not have to reach weapon range first.
    // Returning here consumes the beat, exactly like the reload branch inside RT_ATTACK: the
    // animation it just registered will hold anim_busy next frame anyway.
    if (a->state != RT_IDLE && rt_try_medic(a, npc)) {
        return;
    }

    // A chaser that went into its breather mid-step wipes its forehead now that the body is free --
    // still winded, or not at all (rt_wipe_forehead). 0 = not this frame; anything else settles it.
    if (a->wipe_pending) {
        if (!a->breather || rt_wipe_forehead(npc, "breather") != 0) {
            a->wipe_pending = false;
        }
        if (anim_busy(npc) != 0) {
            return;
        }
    }

    switch (a->state) {
    case RT_IDLE: {
        // RE-AGGRO IS GATED ON PERCEPTION, and it has to be: the stop rule above drops a chase
        // once the critter perceives nobody, and if this branch could re-acquire on distance
        // alone (rt_nearest_player_side reaches the 50-tile leash) it would re-engage on the very
        // next frame and the disengage would be a no-op flicker. Perceiving it again is exactly
        // what re-starts the fight -- which is also how vanilla behaves after combat_end.
        //
        // CONSEQUENCE, deliberate: an alerted-but-blind hostile (faction aggro from hearing
        // gunfire two rooms away) now HOLDS instead of sprinting across the map at you. It stays
        // sticky-hostile and comes the moment it perceives someone.
        //
        // The unreachable latch is the SECOND gate, and it is what stops a rooted critter from
        // re-acquiring the target it just proved it cannot path to (see RT_UNREACHABLE_RETRY_-
        // SECONDS -- one such critter held the whole session in combat). It releases as soon as
        // the actor is standing somewhere else, so anything that can actually move retries at once.
        if (a->unreachable_until > rt_clock && npc->tile == a->unreachable_tile) {
            // ...UNLESS IT CAN SHOOT FROM WHERE IT STANDS. The latch exists to stop a rooted critter
            // re-running a doomed CHASE every frame; it was never meant to stop one that has you in
            // range with a clear line from going straight back to firing. Without this a critter
            // that cannot move at all (the Master) stayed silent for the full 30 s even with the
            // player standing in plain sight in front of it.
            Object* near = a->hostile ? rt_nearest_player_side(npc) : NULL;
            int nearMode = near != NULL ? rt_hit_mode(npc) : 0;
            bool canShootNow = near != NULL
                && obj_dist(npc, near) <= item_w_range(npc, nearMode)
                && !rt_shot_blocked(npc, near, nearMode)
                && is_within_perception(npc, near);
            if (!canShootNow) {
                break;
            }
            a->unreachable_until = 0.0f; // it is back in the fight; the latch has done its job
        }

        // A GUARD WITNESSING AN ATTACK ON ITS OWN -- the door a neutral critter never had, and
        // without which a whole town watches a murder happen (see rt_guard_danger_source).
        // Resolved ONCE into a local: the scan ends in a line-of-sight test, which is far too
        // expensive to run twice per idle guard per frame just to keep an else-if chain tidy.
        Object* guardThreat = NULL;
        if (!a->hostile && !a->friendly && rt_is_guard_type(npc)) {
            guardThreat = rt_guard_danger_source(npc);
        }

        if (guardThreat != NULL) {
            a->target = guardThreat;
            a->state = RT_CHASE;
            a->unperceived_since = -1.0f;
            // If the threat is the PLAYER or his party, set the hostile flag rather than leaving a
            // targeted-but-not-hostile actor behind: everything downstream -- the aggro spread,
            // rt_nearest_player_side, the disengage rules -- is written against `hostile`, and a
            // guard coming for you because you shot a townsperson IS hostile to you.
            if (guardThreat == obj_dude || isPartyMember(guardThreat)) {
                a->hostile = true;
            }
            trace_event(TRACE_FLOW, "guard-join",
                "id=%d pid=%d aggr=%d threat=%d threatPid=%d dude=%d t=%.2f",
                npc->id, npc->pid, combatai_aggression(npc), guardThreat->id, guardThreat->pid,
                guardThreat == obj_dude ? 1 : 0, rt_clock);
        } else if (a->hostile) {
            // Engage the nearest target on the player's side (the dude OR a companion), out to
            // the full chase leash -- so a pack splits between the dude and his followers instead
            // of everyone funnelling onto the dude, and companions actually draw fire.
            Object* tgt = rt_nearest_player_side(npc);

            // ⚠️ BEING SHOT COUNTS AS PERCEIVING, whatever the eyes say. is_within_perception is a
            // SIGHT test -- PE * 5 tiles, halved through glass, quartered against a sneaking dude --
            // and a radscorpion's PERCEPTION of 2 buys it ten. Shoot one from further out and it
            // went hostile and then just STOOD THERE: trace_2026-08-26_194846, id=1516, host=1 with
            // st=IDLE and tgt=0 for six seconds while the player emptied a magazine into it, moving
            // exactly one tile before it died. "Radscorpion po výstřelu nechodil za mnou."
            //
            // whoHitMe is the engine's OWN record of who last hurt this critter (apply_damage sets
            // it), so it answers "do I have a reason to come after you" with no new state and no
            // new bookkeeping to keep in sync. A bullet out of the dark is exactly the case vanilla
            // sight checks were never meant to arbitrate.
            //
            // Deliberately NOT widened to "any hostile chases from anywhere": the perception gate is
            // what keeps an unaware critter across the map from marching at you, and that stays. The
            // only thing added is that being HIT by this particular target is its own evidence.
            Object* hurtBy = npc->data.critter.combat.whoHitMe;

            // Turn its head first (MODS "Enemies look around"), so the perception test below sees
            // the facing it has NOW -- a turn toward a shot takes effect on this very frame.
            rt_idle_look_around(a, npc, tgt);

            // ⚠️ TOLD TO STOP, IT STAYS OUT OF THIS FIGHT -- vanilla's combatai_want_to_join refuses a
            // DISENGAGING critter (a script's critter_stop_attacking, or one that ran far enough --
            // rt_flee_step), and the flag lasts until the fight ends (the bookends in combat_rt_update
            // clear it). The one thing that brings it back is being HURT since its last turn:
            // want_to_join tests damageLastTurn before the flag. (whoHitMe cannot say that -- it stays
            // set long after the hit.) Without this, rt_wants_to_stop dropped it out of the chase and
            // the next frame picked it straight back up, round and round.
            bool hurtSinceItsTurn = npc->data.critter.combat.damageLastTurn > 0;
            bool toldToStop = (npc->data.critter.combat.maneuver & CRITTER_MANEUVER_DISENGAGING) != 0
                && !hurtSinceItsTurn;

            // ⚠️ IT WOULD ONLY LEAVE AGAIN. A critter vanilla would run away on its very first turn
            // (combatai_flee_reason: still FLEEING, hurt too much, under min_hp) and that is already
            // max_dist from the dude would spend that turn leaving the fight (combatai_flee_tile ->
            // DISENGAGING). So it does not start one -- or a wounded critter standing at the edge of
            // its max_dist started a fight, left it, and started the next, every frame once the
            // bookends had cleared its flags. Hurt, it comes in (and runs again), as in vanilla.
            bool wouldOnlyLeave = !hurtSinceItsTurn
                && combatai_flee_reason(npc) != 0
                && obj_dist(npc, obj_dude) >= combatai_max_dist(npc);

            if (tgt != NULL && !toldToStop && !wouldOnlyLeave
                && (is_within_perception(npc, tgt) || hurtBy == tgt)) {
                if (rt_can_reach_or_shoot(npc, tgt)) {
                    a->target = tgt;
                    a->state = RT_CHASE;
                    a->unperceived_since = -1.0f;
                } else {
                    // No way to it and no shot from here: hold, and do not re-ask every frame.
                    trace_event(TRACE_FLOW, "engage-unreachable", "id=%d pid=%d tile=%d tgt=%d dist=%d t=%.2f",
                        npc->id, npc->pid, npc->tile, tgt->tile, obj_dist(npc, tgt), rt_clock);
                    a->unreachable_tile = npc->tile;
                    a->unreachable_until = rt_clock + RT_UNREACHABLE_RETRY_SECONDS;
                }
            }
        } else if (a->friendly) {
            // Follower: pick the nearest of the dude's enemies and engage it. The same
            // CHASE/ATTACK states + arbiter then drive the attack (target-agnostic), and the
            // concurrent resolver lets the follower fight an enemy while the dude fights another.
            //
            // A companion that RAN from this fight (DISENGAGING, rt_flee_step -- vanilla runs party
            // members away too) sits the rest of it out, as one dropped from vanilla's combat list
            // does: no re-engaging and no following until the fight ends and the bookends in
            // combat_rt_update clear the flag. Following would walk it straight back into the
            // fight it just fled, round and round.
            bool ranAway = (npc->data.critter.combat.maneuver & CRITTER_MANEUVER_DISENGAGING) != 0;
            Object* enemy = ranAway ? NULL : rt_nearest_hostile(npc);
            if (enemy != NULL && is_within_perception(npc, enemy)) {
                a->target = enemy;
                a->state = RT_CHASE;
                a->unperceived_since = -1.0f;
            } else if (!ranAway && rt_engaged && rt_is_combatant(obj_dude)
                && obj_dist(npc, obj_dude) > RT_FOLLOW_TILES) {
                // ⚠️ A FOLLOWER IN A FIGHT MUST NEVER JUST STAND THERE -- and before this it did:
                // "dogmeat jen čumí a nepříjde na pomoc, když je daleko". TWO THINGS LOCKED TOGETHER.
                // The branch above only engages what the follower PERCEIVES, and is_within_perception
                // is a SIGHT test: PE * 5 while it can see, but PE * 2 (in combat) the moment a wall,
                // a corner or another critter breaks the line -- a handful of tiles for a dog. And
                // while a fight is live combat_rt raises the engine's warm combat bit
                // (combat_state |= COMBAT_STATE_0x01, in combat_rt_update), which is exactly what
                // stops the VANILLA party-follow script from towing companions after the dude. So a
                // follower that fell behind could not see the fight, and nothing moved it anywhere it
                // could: it had to walk to see, and only walked once it saw.
                //
                // Following the DUDE breaks that deadlock without touching the perception rule, which
                // is load-bearing everywhere else (see the re-aggro note at the top of this case --
                // widening it is how an alerted-but-blind critter starts sprinting at you across the
                // map). The follower closes to RT_FOLLOW_TILES and then engages on its own the moment
                // something comes into view -- his call, over charging the dude's target blind:
                // "nejdřív doběhnout k tobě, pak teprve bojovat".
                //
                // Out of combat this branch is dead (rt_engaged), so the vanilla follow script keeps
                // its job and the two can never both be steering the same critter.
                int dist = obj_dist(npc, obj_dude);

                // anim.cc bills the mover's ENGINE combat.ap per tile while the combat bit is set and
                // truncates the walk at 0. That field is not combat_rt's pool (movement never spends
                // the pool), so it is topped up here for the same reason the chase mover tops it up.
                npc->data.critter.combat.ap = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);

                // Same two-way move the chase uses: a straight step is cheap and smooth, but it
                // cannot route around a building, so hand a blocked step to the pathfinder instead.
                // The result is deliberately NOT fed to rt_note_path_result -- that arms the
                // unreachable latch, which exists to stop an actor re-acquiring a TARGET it cannot
                // path to, and a follower must keep trying to reach its leader.
                int stepRot = tile_dir(npc->tile, obj_dude->tile);
                int stepDest = tile_num_in_direction(npc->tile, stepRot, 1);
                Object* blocker = stepDest != obj_dude->tile
                    ? obj_blocking_at(npc, stepDest, npc->elevation)
                    : NULL;

                if (a->diag_dist != dist) {
                    a->diag_dist = dist;
                    trace_event(TRACE_FLOW, "follow-step",
                        "id=%d pid=%d dist=%d blk=%d t=%.2f",
                        npc->id, npc->pid, dist, blocker != NULL ? blocker->pid : -1, rt_clock);
                }

                if (blocker != NULL) {
                    // Ask the pathfinder at most once a second while it keeps saying no -- see
                    // RtActor::follow_retry_at. A success clears nothing because nothing waits.
                    if (rt_clock >= a->follow_retry_at && !rt_advance_to_target(npc, obj_dude)) {
                        a->follow_retry_at = rt_clock + 1.0f;
                    }
                } else {
                    rt_step_toward(npc, obj_dude);
                }
            }
        } else if (rt_is_combatant(a->engine_target)) {
            // Scripted NPC-vs-NPC fight (e.g. Gizmo's assassin sent at Killian): engage the
            // engine-assigned target with the same CHASE/ATTACK states used against the dude.
            a->target = a->engine_target;
            a->state = RT_CHASE;
        }
        break;
    }

    case RT_CHASE:
        if (!rt_is_combatant(a->target)) {
            a->state = RT_IDLE;
            a->target = NULL;
            a->weapon_checked = false;
            break;
        }

        // Draw the best weapon before closing in, so rt_hit_mode below reports the
        // weapon's real range (ranged shooters keep their distance) instead of the
        // unarmed melee range that would drag them into punching distance. If this
        // starts a take-out animation the anim_busy guard holds movement until it
        // finishes, so the NPC visibly draws, then advances.
        rt_ensure_weapon_drawn(a);

        {
            // Close to the PREFERRED engagement distance before opening fire (capped at the
            // weapon's own range), not just to max weapon range -- a ranged shooter that stops
            // at max range eats Fallout's per-tile to-hit penalty and misses forever. Melee /
            // short weapons: min(range, PREFERRED) == range, so they still close to contact.
            int mode = rt_hit_mode(npc);
            int range = item_w_range(npc, mode);
            int engage = range < RT_PREFERRED_ENGAGE_TILES ? range : RT_PREFERRED_ENGAGE_TILES;
            // ⚠️ A CRITTER THAT CANNOT WALK FIRES FROM WHERE IT IS (2026-10-09). The preferred
            // distance is something to close TO; the Master cannot close at all (rt_can_walk), so
            // with you past 8 tiles he sat in CHASE asking for a route, was marked unreachable and
            // dropped to IDLE -- and then just stood there flinching while you shot him ("Master
            // se zacyklil", trace_2026-10-09_160503 s=52.97 onward). His own weapon reaches 40.
            if (!rt_can_walk(npc)) {
                engage = range;
            }
            bool inRange = obj_dist(npc, a->target) <= engage;

            // TEMP DIAG -- "the guard never shot at me" / hostiles sitting in CHASE forever.
            // The state trace shows an actor stuck in CHASE but not WHY it fails to qualify for
            // RT_ATTACK, and the two candidate causes need opposite fixes: either it genuinely
            // can't close the distance (a movement problem), or `range` is 1 because rt_hit_mode
            // resolved to a PUNCH for a critter that is actually carrying a gun (a weapon-selection
            // problem -- an armed guard would then behave like a melee attacker and try to walk
            // into contact instead of firing). One line per actor whenever the decision inputs
            // change; STRIP once the cause is known.
            {
                int dist = obj_dist(npc, a->target);
                bool blockedShot = rt_shot_blocked(npc, a->target, mode);
                Object* rw = inven_right_hand(npc);
                if (a->diag_mode != mode || a->diag_range != range || a->diag_dist != dist) {
                    a->diag_mode = mode;
                    a->diag_range = range;
                    a->diag_dist = dist;
                    trace_event(TRACE_FLOW, "chase-why",
                        "id=%d pid=%d mode=%d range=%d engage=%d dist=%d inRange=%d shotBlocked=%d rhand=%d ap=%d t=%.2f",
                        npc->id, npc->pid, mode, range, engage, dist, inRange ? 1 : 0,
                        blockedShot ? 1 : 0, rw != NULL ? rw->pid : -1, a->ap, rt_clock);
                }
            }

            if (inRange && !rt_shot_blocked(npc, a->target, mode)) {
                a->state = RT_ATTACK;
            } else if (a->breather) {
                // STAND AND RECOVER. The latch lives in the drain block above; this is the only
                // place it does anything, and what it suppresses is MOVEMENT ONLY.
                //
                // Only ever set inside the engagement bubble -- see the gate on the latch.
                //
                // ⚠️ IT IS DELIBERATELY BELOW THE RT_ATTACK BRANCH. A breathing critter that can
                // see and reach you still attacks -- exactly like the dude, whose rt_move_blocked
                // stops his feet and never his trigger. Putting this test first would have made a
                // pause mean "cannot fight", which is the frozen-chaser bug wearing a new name.
                //
                // An in-flight walk segment is left alone rather than force-cleared: it finishes
                // its current tile and then nothing new is issued, so the stop reads as slowing to
                // a halt instead of snapping mid-stride.
            } else if (inRange) {
                // In range but no LINE OF FIRE (target behind a wall/building): don't
                // just stand -- path AROUND the obstacle to get an angle. A plain step
                // toward can't route around a building, so a ranged NPC would otherwise
                // sit there forever re-attempting a blocked shot.
                rt_note_chase_move(a, npc, 2, -1, -1);

                // ⚠️ BUT A FAILED ROUTE HERE IS NOT "UNREACHABLE" (2026-10-04, "master střílel a
                // pak neútočil"). The target is INSIDE weapon range; all that is missing is a clear
                // line, which the target itself restores the moment it steps out from behind the
                // obstacle. Feeding the failure to the unreachable give-up dropped such a critter to
                // IDLE and then latched it for RT_UNREACHABLE_RETRY_SECONDS "while it stands on that
                // tile" -- which for the Master, who never moves, meant: fire three times, then go
                // quiet for good. It only bit once rt_advance_to_target started reporting real route
                // failures (anim_last_path_steps); before that this branch could never fail at all.
                //
                // So: try to reposition at most once a second, never arm the give-up from here, and
                // otherwise hold -- the RT_ATTACK test above re-checks the line every frame.
                if (rt_clock >= a->follow_retry_at && !rt_advance_to_target(npc, a->target)) {
                    a->follow_retry_at = rt_clock + 1.0f;
                }
                a->no_path_since = -1.0f;
            } else {
                // While the in-combat bit is set, anim.cc deducts the mover's engine
                // combat.ap per tile and truncates the move at ap <= 0. That field is
                // NOT combat_rt's AP pool (movement is free and never spends the
                // pool), so top combat.ap up before each step or the NPC never closes
                // distance.
                npc->data.critter.combat.ap = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);

                // A plain step toward the target is cheap and smooth, but it CAN'T route
                // around a wall/building: rt_step_toward just tries the one tile straight
                // at the target and, if that's blocked, bumps it and stays put forever
                // (the "guard stuck behind a wall never engages / doesn't react" case). So
                // when the immediate step is blocked, pathfind AROUND the obstacle instead
                // -- same routing rt_advance_to_target uses for a blocked line of fire.
                int stepRot = tile_dir(npc->tile, a->target->tile);
                int stepDest = tile_num_in_direction(npc->tile, stepRot, 1);
                Object* blocker = stepDest != a->target->tile
                    ? obj_blocking_at(npc, stepDest, npc->elevation)
                    : NULL;

                // TEMP DIAG (ping-pong, 2026-08-28). trace_2026-08-28_225016 caught id=669 and
                // id=579 cycling between two tiles with dudeStill=1 -- 13 identical
                // `walk-seg from=19495 to=19099 dist=4` and NO move-fail. The straight step and the
                // detour disagree and each undoes the other every frame. Two earlier fixes here were
                // REVERTED for making engagement worse, and both guessed at what was in the way, so
                // measure it: WHAT refuses the straight step decides the fix. A wall means commit to
                // the detour; a live critter means the queue case the comment below describes; a
                // CORPSE means bodies are blocking tiles they should not. Emitted on change only.
                // STRIP with diag_blocker.
                {
                    int bpid = blocker != NULL ? blocker->pid : -1;
                    if (a->diag_blocker != bpid) {
                        a->diag_blocker = bpid;
                        bool bcritter = blocker != NULL && FID_TYPE(blocker->fid) == OBJ_TYPE_CRITTER;
                        trace_event(TRACE_FLOW, "step-blocker",
                            "id=%d pid=%d tile=%d step=%d blk=%d critter=%d dead=%d tgt=%d dist=%d t=%.2f",
                            npc->id, npc->pid, npc->tile, stepDest, bpid, bcritter ? 1 : 0,
                            (bcritter && (blocker->data.critter.combat.results & DAM_DEAD) != 0) ? 1 : 0,
                            a->target->tile, obj_dist(npc, a->target), rt_clock);
                    }
                }

                if (blocker != NULL) {
                    // WHAT is in the way decides what to do about it. A wall or a crate is
                    // permanent -- route around it. ANOTHER CRITTER pressing on the same target
                    // is not: routing around a body that is itself moving produced the crowd
                    // ping-pong the player reported ("she ran back and forth and never stopped",
                    // trace 235618: id=1756 held dist=2 for 18 seconds, oscillating between two
                    // tiles). Each segment re-pathed around the ally, arrived no closer, and
                    // re-pathed again. Standing your ground for a beat is both calmer to watch
                    // and what a real queue behind a doorway looks like -- the blocker is another
                    // attacker, so it will move or die shortly and the way opens by itself.
                    //
                    // The first version of this gated on DISTANCE (hold only within a few tiles of
                    // engagement range, route around otherwise) and the player still reported the
                    // ping-pong. That window was far too narrow: it only ever covered dist in
                    // (engage, engage+3], because anything at or inside `engage` goes to RT_ATTACK
                    // and never reaches here. A pack chasing the player is mostly OUTSIDE it, so
                    // the whole back of the queue kept re-pathing around the front of it -- which
                    // is precisely the crowd this was meant to fix. Distance was the wrong axis.
                    // TIME is the right one: wait behind another critter for real, and only path
                    // around once the wait proves fruitless, which also breaks the one case a pure
                    // hold can't survive (two critters blocking each other symmetrically).
                    // REVERTED TO BASELINE, TWICE BURNED. Two attempts to special-case a CRITTER in
                    // the way both made engagement measurably WORSE, each in its own way, and each
                    // was caught by the next trace:
                    //   * a distance-gated "hold and wait": id=1756 stood still at dist=2 for six
                    //     unbroken seconds; hostiles were 575 samples in CHASE to 40 in ATTACK.
                    //   * "go for an open slot on the ring, else orbit": 103 to 4 -- 96% of hostile
                    //     time spent NOT attacking, and the player reported guards that never fired
                    //     at all.
                    // Pathing around is at least a behaviour that ends in an attack, so it stands
                    // until the CHASE diagnostic below explains why actors sit in CHASE at all --
                    // guessing at the movement layer three times running is how this got worse.
                    rt_note_chase_move(a, npc, 3, blocker != NULL ? blocker->pid : -1, stepDest);
                    rt_note_path_result(a, rt_advance_to_target(npc, a->target));
                } else {
                    rt_note_chase_move(a, npc, 1, -1, stepDest);
                    rt_step_toward(npc, a->target);
                }
            }
        }
        break;

    case RT_ATTACK: {
        if (!rt_is_combatant(a->target)) {
            a->state = RT_IDLE;
            a->target = NULL;
            a->weapon_checked = false;
            break;
        }

        int hitMode = rt_hit_mode(npc);
        if (obj_dist(npc, a->target) > item_w_range(npc, hitMode)) {
            a->state = RT_CHASE;
            break;
        }

        // RELOAD AN EMPTY GUN. Vanilla's turn-based AI reloads inside combat_ai(), which is
        // dead under realtime -- so an NPC that ran dry simply kept pulling the trigger on an
        // empty weapon forever. combat_check_bad_shot rejects the shot every single frame with
        // COMBAT_BAD_SHOT_NO_AMMO and nothing ever fixes the cause: trace_2026-08-03_222723 has
        // 95 such rejections, in bursts of 18 inside 0.28s, all from NPCs. Costs the same 2 AP
        // the player's own reload does (gamepad X), so a dry critter pauses to reload instead of
        // shooting that beat.
        //
        // If the reload FAILS there is no ammo anywhere in the pack, and the critter must not be
        // left holding a weapon it can never fire -- see rt_swap_dry_weapon for what that looked
        // like in the trace. Swapping costs the same beat and the same AP as reloading would have.
        if (a->ap >= RT_RELOAD_AP_COST && rt_needs_reload(npc, hitMode)) {
            Object* weapon = item_hit_with(npc, hitMode);
            // MODS "Reload animation": the loop first, the load and the AP at its end (see
            // rt_npc_start_reload_anim). The beat is the reload either way -- started now, or asked
            // again next frame because the body was not free yet; the gun is empty regardless.
            if (anim_get_reload_anim() && weapon != NULL && rt_npc_has_ammo_for(npc, weapon)) {
                if (rt_npc_start_reload_anim(a, npc, weapon) || anim_busy(npc) != 0) {
                    break;
                }
                // A free body that still would not take the loop has no STAND art for this weapon
                // (the registry refuses missing art). Waiting would wait forever with an empty gun
                // -- reload the old instant way below instead.
            }
            if (weapon != NULL && item_w_try_reload(npc, weapon) != -1) {
                a->ap -= RT_RELOAD_AP_COST;
                trace_event(TRACE_FLOW, "npc-reload", "id=%d pid=%d ap=%d t=%.2f",
                    npc->id, npc->pid, a->ap, rt_clock);
                break; // reloading IS this beat's action
            }
            if (rt_swap_dry_weapon(a, npc, weapon)) {
                a->ap -= RT_RELOAD_AP_COST;
                break; // swapping IS this beat's action
            }
        }

        // Line of fire lost (target stepped behind cover, or we were pulled here with
        // no LOS): drop back to CHASE, which repositions around the obstacle. Without
        // this a ranged NPC stands and machine-guns a blocked shot (rejected every
        // frame by combat_check_bad_shot) instead of moving to get an angle.
        if (rt_shot_blocked(npc, a->target, hitMode)) {
            a->state = RT_CHASE;
            break;
        }

        // While the target is in a brief COSMETIC reaction (a hit-react flinch or a miss dodge),
        // just HOLD position and wait this frame -- don't strafe/circle it. The tangential
        // reposition read as jerky "darting" back and forth next to the player (and fed the crowd
        // gridlock the unstick-move watchdog then had to keep breaking); the flinch clears in a
        // moment and the arbiter lets this NPC swing then, so standing still for that beat is far
        // smoother than circling.
        //
        // A KNOCKDOWN is deliberately NOT waited out -- neither the fall nor the standup. It's a
        // real disabling state the critter has to sit in, and a downed enemy is fair game.
        // This used to gate on `rt_is_reacting(t) && !critter_is_prone(t)`, which covered only the
        // FALL: rt_stand_up clears DAM_KNOCKED_DOWN the instant the standup registers, and
        // ANIM_PRONE/BACK_TO_STANDING (36/37) sit OUTSIDE FIRST..LAST_KNOCKDOWN_AND_DEATH_ANIM
        // (20..35), so critter_is_prone read false for the whole standup and every enemy politely
        // waited it out. Since rt_needs_standup fires the frame the fall ends, there is no idle
        // prone beat at all -- which made the standup a total free pass ("they don't attack me
        // while I'm knocked down"), and an ASYMMETRIC one: the player's own attack path
        // (playerEligible) has no reaction gate and can freely hit a critter that's getting up.
        // rt_is_cancelable_reaction is true only for the flinch/dodge, so it draws the line where
        // the comment above always claimed it was.
        if (rt_is_cancelable_reaction(a->target)) {
            break;
        }

        // Record an attack intent if the pool can afford it; the arbiter decides
        // (by Sequence) who actually gets the single free hit-resolution slot this
        // frame and spends the AP. Can't afford -> stay in RT_ATTACK and wait for
        // AP to refill (no queueing). The anim_busy(npc) guard above already kept
        // an actor that's mid-swing from getting here.
        int cost = rt_ap_cost(npc, hitMode);
        if (a->ap >= cost) {
            a->wants_attack = true;
            a->attack_target = a->target;
            a->attack_mode = hitMode;
        }
        break;
    }
    }
}

// Central attack arbiter. At most one attack may resolve at a time (the shared
// main_ctd lock -- see rt_attack_locked / the death-crash fix), so when that slot
// is free this grants it to ONE actor. Every actor -- the player and every NPC --
// that has a live, still-eligible attack intent this frame competes on equal
// footing: the highest Sequence (= Perception x 2, the original Fallout initiative)
// wins, so a high-PE build strikes first and a low-PE one genuinely struggles
// against fast enemies. There is no player favouritism -- the player is arbitrated
// by Sequence like everyone else. Equal-Sequence actors share via a rotating
// cursor so none monopolizes. The winner pays its AP only here, on the actual
// fire; losers keep their AP and re-contend next frame. Eligibility is
// re-validated at grant time, so a target that died/fled or an attacker that
// started animating since recording its intent is dropped.
static void rt_resolve_attacks(float dt)
{
    // Decay the latched player intent so a one-shot mouse click eventually
    // expires; a held attack button refreshes it every frame and keeps it live.
    if (rt_player_intent_ttl > 0.0f) {
        rt_player_intent_ttl -= dt;
        if (rt_player_intent_ttl <= 0.0f) {
            rt_player_intent_ttl = 0.0f;
            rt_player_intent_target = NULL;
            rt_player_intent_loc = HIT_LOCATION_UNCALLED;
        }
    }

    // Player attack eligibility, computed up front. The gates: the target is a live
    // combatant, the dude is idle (not mid-flinch/animation -- the latch keeps the press
    // alive across flinch frames until this is true; a cosmetic flinch is cut so he can
    // fire), in range, and the dude has the AP. NOTE (E): the target being mid-reaction NO
    // LONGER blocks the attack -- concurrent focus-fire is allowed and the target's stagger
    // is its own decoupled sequence (rt_play_hit_react), latest-wins.
    int dudeSeq = stat_level(obj_dude, STAT_SEQUENCE);
    bool playerEligible = false;
    int dudeCost = 0;
    if (rt_player_intent_target != NULL && rt_player_intent_ttl > 0.0f
        && rt_in_registry(rt_player_intent_target)
        && rt_is_targetable(rt_player_intent_target)
        && !rt_attacker_in_flight(obj_dude)
        && !rt_attack_on_cooldown(obj_dude) // his own rate of fire, symmetric with the NPCs'
        && !rt_dude_incapacitated()) {
        bool aiming = rt_player_intent_loc != HIT_LOCATION_UNCALLED;
        dudeCost = rt_ap_cost(obj_dude, rt_player_intent_mode, aiming);
        int dudeDist = obj_dist(obj_dude, rt_player_intent_target);
        int dudeRange = item_w_range(obj_dude, rt_player_intent_mode);
        if (rt_player_ap >= dudeCost && dudeDist <= dudeRange) {
            // anim_busy_blocking, not anim_busy: since check_registry stopped letting a SPENT
            // description veto a new animation, the dude legitimately appears in an old sequence
            // (his swing, still dropping his victim) while acting in a new one. Gating the right
            // to attack on plain anim_busy therefore locked him out for the whole of his victim's
            // death -- "the rat walks around and I can't hit it".
            if (anim_busy_blocking(obj_dude) == 0) {
                playerEligible = true;
            } else if (rt_is_cancelable_reaction(obj_dude)) {
                // FLINCH-LOCK FIX: the dude is "busy" only with a cosmetic hit-react / dodge
                // flinch (its damage already committed at swing-connect). Under multi-enemy fire he
                // is re-flinched (and even near-misses register a blocking DODGE) almost every
                // frame, so he essentially never reached anim_busy==0 and could not attack at all
                // ("can't attack while they shoot at me"). CUT the flinch now: register_clear ->
                // anim_set_end frees the sequence, so anim_busy reads 0 immediately (unless the
                // reaction is PRIORITIZED) -- re-test and let the attack fire THIS frame when it
                // cleared, instead of waiting a frame in which a fresh flinch re-locks him. If it
                // didn't clear, rt_do_attack tolerates a failed register (no AP charged, intent
                // kept), so a same-frame attempt is safe. A KNOCKDOWN is not cancelable, so a
                // downed dude still can't attack.
                rt_cancel_flinch_for_move(obj_dude);
                if (anim_busy_blocking(obj_dude) == 0) {
                    playerEligible = true;
                }
            } else if (rt_dude_is_fidgeting() && !combat_rt_is_reloading(obj_dude)) {
                // An idle STAND -- the out-of-breath forehead wipe (rt_wipe_forehead) or a vanilla
                // fidget -- owes nobody anything: cut it and fire, exactly as the movers do. Never a
                // reload's STAND, whose cut has its own refund rules (gamepad_ui reload tick).
                anim_clear_pending(obj_dude);
                if (anim_busy_blocking(obj_dude) == 0) {
                    playerEligible = true;
                }
            }
        }
    }

    // --- Build NPC attack candidates FIRST (the player's Sequence-initiative check below needs to
    // know whether the player's target is itself swinging at the dude this frame). One attack per
    // DEFENDER (skip a target mid-reaction or already in flight) keeps attacks on the dude
    // serialized while different attacker->defender pairs run concurrently.
    struct RtCandidate {
        Object* attacker;
        Object* target;
        int mode;
        int cost;
        int seq;
        int* ap;
    };
    RtCandidate cand[RT_MAX_ACTORS];
    int candCount = 0;

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (!a->wants_attack) {
            continue;
        }

        Object* npc = a->obj;
        if (!rt_is_combatant(npc) || anim_busy(npc) != 0 || !rt_is_combatant(a->attack_target)) {
            continue;
        }

        // E: no per-defender reaction/in-flight block anymore. Attacks on one target run
        // truly concurrently (the flinch is the defender's own sequence, driven at commit,
        // so nothing here conflicts). Attacking a knocked-down target is fine too --
        // rt_play_hit_react skips the stagger for a heavy reaction, so the knockdown plays.
        // Its own rate of fire. This is the gate that kills the double-tap: without it an NPC could
        // open a second attack while its first was still in flight (rt_attacker_in_flight guards
        // only the dude), which the trace caught at 0.05 s between two swings of one critter.
        if (rt_attack_on_cooldown(npc)) {
            continue;
        }

        int cost = rt_ap_cost(npc, a->attack_mode);
        if (a->ap < cost
            || obj_dist(npc, a->attack_target) > item_w_range(npc, a->attack_mode)) {
            continue;
        }

        RtCandidate* c = &cand[candCount++];
        c->attacker = npc;
        c->target = a->attack_target;
        c->mode = a->attack_mode;
        c->cost = cost;
        c->seq = stat_level(npc, STAT_SEQUENCE);
        c->ap = &a->ap;
    }

    // --- Player attack: concurrent, BUT governed by SEQUENCE INITIATIVE when the target is
    // fighting back. If the dude's target is itself a candidate to strike the dude this frame (a
    // mutual trade), roll initiative weighted by Sequence (dudeSeq vs the foe's Seq). The dude
    // fires only if he WINS; if he loses he holds this frame (keeps his intent) so he doesn't hit
    // the foe -- which lets the foe recover from its hit-react and get its own swing in via the NPC
    // pick. So a higher-Sequence enemy (or ANY enemy when the dude's PE is low) gets to strike
    // first sometimes, instead of the dude always pre-empting and click-stunlocking it to death.
    // If the target isn't swinging back (fleeing, or a non-aggressor), the dude keeps priority.
    if (playerEligible) {
        bool dudeWins = true;
        int foeSeq = -1;
        for (int i = 0; i < candCount; i++) {
            if (cand[i].attacker == rt_player_intent_target) {
                foeSeq = cand[i].seq;
                dudeWins = rt_wins_initiative(dudeSeq, foeSeq);

                // DIAG (initiative): the roll is invisible otherwise -- you can only infer it from
                // who happened to swing first. `pct` is the modelled chance the dude wins, so a run
                // of these lines can be checked against the outcomes: the win rate should converge
                // on pct. Fires only on a MUTUAL trade, so it is naturally sparse.
                {
                    float dw = powf((float)(dudeSeq > 0 ? dudeSeq : 1), rt_seq_initiative_power);
                    float fw = powf((float)(foeSeq > 0 ? foeSeq : 1), rt_seq_initiative_power);
                    trace_event(TRACE_COMBAT, "initiative",
                        "dudeSeq=%d foeSeq=%d p=%.2f pct=%.0f win=%d t=%.2f",
                        dudeSeq, foeSeq, rt_seq_initiative_power,
                        (dw + fw) > 0.0f ? 100.0f * dw / (dw + fw) : 100.0f,
                        dudeWins ? 1 : 0, rt_clock);
                }
                break;
            }
        }

        if (dudeWins) {
            bool fired = rt_do_attack(obj_dude, rt_player_intent_target,
                rt_player_intent_mode, rt_player_intent_loc);
            if (fired) {
                rt_note_attack_fired(obj_dude, rt_player_intent_mode);
                // Defer the AP charge to swing-connect (the commit pass) rather than debiting
                // it the instant the button is pressed: the pool visibly drops when the swing
                // actually lands, ~halfway through the animation. No double-spend risk -- the
                // dude is anim_busy for his whole swing, so playerEligible stays false until the
                // swing ends, by which point the in-flight entry has committed and charged. The
                // just-pushed in-flight entry is this attack (rt_do_attack pushes exactly one).
                if (rt_inflight_count > 0) {
                    rt_inflight[rt_inflight_count - 1].playerApCost = dudeCost;
                }
                rt_player_intent_target = NULL;
                rt_player_intent_ttl = 0.0f;
                rt_player_intent_loc = HIT_LOCATION_UNCALLED;
            }
        }
        // else: the dude lost the Sequence roll -- hold this frame (keep intent) so the
        // foe recovers from its hit-react and gets its own swing in via the NPC pick.
    }

    if (candCount == 0) {
        return;
    }

    // Post-conversation grace: NPCs hold fire for a beat after a talk closes. Placed AFTER the
    // player's attack above, so the hold is one-sided -- you can shoot the moment the talk ends,
    // they cannot. Candidates keep their intent and fire normally once it lapses.
    if (rt_clock < rt_npc_hold_until) {
        return;
    }

    // --- NPC pick: one new NPC attack begins per frame, Sequence-weighted (higher PE attacks
    // proportionally more often; a slower actor still gets a fair share). Sequence floored at 1.
    int totalWeight = 0;
    for (int i = 0; i < candCount; i++) {
        totalWeight += cand[i].seq > 0 ? cand[i].seq : 1;
    }
    int roll = roll_random(0, totalWeight - 1);
    int winIdx = candCount - 1;
    for (int i = 0; i < candCount; i++) {
        int w = cand[i].seq > 0 ? cand[i].seq : 1;
        if (roll < w) {
            winIdx = i;
            break;
        }
        roll -= w;
    }
    RtCandidate* win = &cand[winIdx];

    // E: concurrent focus-fire -- no in-flight/reaction skip. This NPC can strike a target
    // the player (or another NPC) is also hitting; the decoupled hit-react (rt_play_hit_-
    // react at commit) handles the stagger latest-wins without cutting any swing.

    // Charge AP only if the attack actually started (rt_do_attack false = engine couldn't
    // register, e.g. the in-flight list is full or the table was busy; keep the AP and retry).
    bool fired = rt_do_attack(win->attacker, win->target, win->mode, HIT_LOCATION_UNCALLED);
    if (fired) {
        *win->ap -= win->cost;
        rt_note_attack_fired(win->attacker, win->mode);
    }
}

// Forget the current combat encounter: drop all tracked actors, in-flight attack
// snapshots, lock-on / attack-intent / world-loot targets, any armed map-exit, and the
// in-progress stick move. Leaves the AP pool, clock, config and rt_enabled alone -- used
// both by the map-teardown hook (the same session continues on the new map) and as the
// first half of a full session reset. Pointer/index resets only (the referenced objects
// are being freed by the caller), so it never touches the animation tables.
static void rt_abandon_encounter()
{
    rt_inflight_count = 0;
    rt_actor_count = 0;
    rt_engaged = false;
    // A shot on the map being left is not a noise on the next one (the tile would be meaningless
    // there), and the clock it was stamped with may restart.
    rt_noise_clock = -1000.0f;
    rt_noise_tile = -1;
    rt_noise_elev = -1;
    rt_reloading_count = 0; // pointers only -- the bodies are being freed by the caller
    rt_lock_target = NULL;
    rt_lock_push_anchor = NULL;
    rt_lock_last_target = NULL;
    rt_aim_outlined_count = 0; // pointers only: the objects are being freed by the caller
    rt_aim_outline_next = 0.0f;
    rt_lock_held = 0.0f;
    rt_player_intent_target = NULL;
    rt_player_intent_mode = 0;
    rt_player_intent_ttl = 0.0f;
    rt_player_intent_loc = HIT_LOCATION_UNCALLED;
    rt_pending_loot = NULL;
    rt_pending_loot_kind = 0;
    rt_map_exit_armed = false;
    rt_map_exit_committed = false;
    rt_map_exit_scripted = false;
    rt_stick_active = false;
    rt_stick_short = false;
    rt_stick_running = false;
    rt_stick_rotation = -1;
    rt_fm_active = false;
    rt_fm_x = 0.0f;
    rt_fm_y = 0.0f;
    rt_fm_rest_tile = -1;
    rt_fm_frame = 0.0f;
    rt_fm_facing = -1;
    rt_stick_dest = -1;
    rt_stick_last_sector = -1;

    // The held attacker offsets point at objects the caller is about to free.
    rt_swing_keep_clear_all();
}

// Full per-session state reset, shared by init / exit / game-reset so every teardown path
// clears the SAME runtime state (the old hand-maintained per-function lists had drifted --
// exit forgot rt_refresh_timer, and none reset the stick/camera/flinch state, which was the
// source of the teardown/reload bug family). Does NOT load config (init does that after),
// set rt_enabled (each caller does), or touch combat_state (game-reset clears the warm bit).
static void rt_reset_session_state()
{
    // The reaction table is session state like everything else here: a load must not resurrect a
    // knockdown from the game that was running a second ago.
    rt_reaction_clear_all();

    // ⚠️ And the camera must not carry a lag (or a previous sub-tile offset) across a load or a map
    // change: the dude is somewhere else entirely, and the first delta would be measured against a
    // tile on the map we just left.
    rt_cam_lag_x = 0.0f;
    rt_cam_lag_y = 0.0f;
    rt_cam_have_prev = false;
    rt_cam_lean_x = 0.0f;
    rt_cam_lean_y = 0.0f;

    rt_abandon_encounter();

    // A roll in flight is animating the PREVIOUS session's hit points; nothing ticks it once
    // combat_rt is torn down, so it would sit on a stale number over the new game's bar.
    intface_hp_roll_cancel();

    rt_refresh_timer = 0.0f;
    rt_dude_flinch_accum = 0.0f;
    rt_dude_immune_until = 0.0f;
    rt_player_ap = INT_MAX; // clamped to the dude's max AP on the first update
    rt_player_ap_regen = 0.0f;
    rt_bonus_move_ap = INT_MAX; // likewise clamped to 2 x Bonus Move rank
    rt_bonus_move_regen = 0.0f;
    rt_player_next_attack_at = 0.0f;
    rt_player_weapon_up_until = 0.0f;
    rt_run_ap_drain = 0.0f;
    rt_walk_ap_drain = 0.0f;
    rt_run_blocked = false;
    rt_move_blocked = false;
    rt_last_hud_ap = -1;
    rt_last_hud_blocked = -1;
    rt_last_hud_bonus = -1;
    rt_last_hud_ac = -1;
    rt_cam_last_tile = -1;
    rt_cam_last_off_x = 0;
    rt_cam_last_off_y = 0;
    rt_clock = 0.0f;
    rt_move_input_age = 1000.0f;
    snprintf(rt_last_event, sizeof(rt_last_event), "none");
    rt_last_event_pid = -1;
    rt_last_event_time = -1000.0f;
    rt_dude_last_results = 0;
}

void combat_rt_init()
{
    rt_reset_session_state();
    rt_enabled = true;

    // Firing cadence: seconds per +1 AP, from config so it's retunable without a
    // rebuild (default RT_AP_REGEN_SECONDS). Floored so a tiny/zero value can't make
    // regen effectively instant (which would un-gate attacks entirely).
    double apRegen = RT_AP_REGEN_SECONDS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AP_REGEN_KEY, &apRegen);
    rt_ap_regen_seconds = apRegen < 0.05 ? 0.05f : (float)apRegen;

    // Doorway magnet strength (0 = off). Clamped to 0..1: past 1 the assist would overshoot the
    // doorway and bend the stick past it, which reads as the controls fighting back.
    double doorMagnet = 0.7;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_DOOR_MAGNET_KEY, &doorMagnet);
    rt_door_magnet = doorMagnet < 0.0 ? 0.0f : (doorMagnet > 1.0 ? 1.0f : (float)doorMagnet);
    rt_door_cache_tile = -1; // force a rebuild on the new map

    // Crowd assist: step around a bystander instead of jamming into them (0 = off).
    int npcSlide = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_SLIDE_KEY, &npcSlide);
    rt_npc_slide = npcSlide != 0;

    // Idle hostiles look around (MODS row; 0 = off = vanilla's fixed stare). See rt_idle_look_around.
    // Default ON since the release (his setting).
    int lookAround = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "enemies_look_around", &lookAround);
    rt_look_around = lookAround != 0;

    // Script combat turns (MODS row; 1 = on = vanilla's per-turn script events). See rt_script_turn_tick.
    int scriptTurns = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "script_turns", &scriptTurns);
    rt_script_turns = scriptTurns != 0;

    // Free movement: the body off the grid entirely. See FREE MOVEMENT.
    int freeMove = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_FREE_MOVE_KEY, &freeMove);
    rt_free_move = freeMove != 0;


    // How hard AGILITY bends that cadence per critter (0 = the old flat rate for everyone).
    // Capped at 0.3 so the spread stays inside the clamps in rt_regen_interval.
    double apRegenAgi = 0.05; // = the gconfig_init default
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AP_REGEN_AGILITY_FACTOR_KEY, &apRegenAgi);
    if (apRegenAgi < 0.0) {
        apRegenAgi = 0.0;
    } else if (apRegenAgi > 0.3) {
        apRegenAgi = 0.3;
    }
    rt_ap_regen_agility_factor = (float)apRegenAgi;

    // Physical rate of fire. 0 = disabled (previous behaviour exactly). Capped well above any sane
    // value so a typo can't leave a critter unable to attack for a whole fight.
    double atkCd = 2.6;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ATTACK_COOLDOWN_BASE_KEY, &atkCd);
    if (atkCd < 0.0) {
        atkCd = 0.0;
    } else if (atkCd > 5.0) {
        atkCd = 5.0;
    }
    rt_attack_cooldown_base = (float)atkCd;

    double atkCdStat = 0.12;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ATTACK_COOLDOWN_STAT_FACTOR_KEY, &atkCdStat);
    if (atkCdStat < 0.0) {
        atkCdStat = 0.0;
    } else if (atkCdStat > 0.3) {
        atkCdStat = 0.3;
    }
    rt_attack_cooldown_stat_factor = (float)atkCdStat;

    // Same term, but only for stats ABOVE average. Capped at 1.0 = symmetric; there is no case for
    // rewarding the top end MORE than the bottom is punished, which is how it got too fast.
    double atkCdBias = RT_ATTACK_COOLDOWN_HIGH_BIAS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ATTACK_COOLDOWN_HIGH_BIAS_KEY, &atkCdBias);
    if (atkCdBias < 0.0) {
        atkCdBias = 0.0;
    } else if (atkCdBias > 1.0) {
        atkCdBias = 1.0;
    }
    rt_attack_cooldown_high_bias = (float)atkCdBias;

    // Weapon skill's contribution to the same cooldown. Capped at 0.5 because the term is applied
    // as (1 +- factor) and anything approaching 1.0 would let a low skill DOUBLE the cooldown while
    // a high one drove it to nothing -- far past the point where the stat term still leads.
    double atkCdSkill = RT_ATTACK_COOLDOWN_SKILL_FACTOR;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ATTACK_COOLDOWN_SKILL_FACTOR_KEY, &atkCdSkill);
    if (atkCdSkill < 0.0) {
        atkCdSkill = 0.0;
    } else if (atkCdSkill > 0.5) {
        atkCdSkill = 0.5;
    }
    rt_attack_cooldown_skill_factor = (float)atkCdSkill;

    // The weapon-raise hold. Negatives are meaningful (never raise), so only the upper end is
    // capped -- past a few seconds "raise again after a lull" and "raise once per fight" are the
    // same thing, and a huge value would just be a confusing way to write the latter.
    double pointHold = RT_POINT_HOLD_SECONDS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RANGED_POINT_HOLD_KEY, &pointHold);
    if (pointHold > 30.0) {
        pointHold = 30.0;
    }
    rt_point_hold = (float)pointHold;

    // Guards joining a fight they witness. 0 (or below) disables it entirely; no upper clamp is
    // needed because aggression tops out at 100 in AI.TXT, so anything higher is just "off" again.
    int guardJoin = RT_GUARD_JOIN_AGGRESSION;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GUARD_JOIN_AGGRESSION_KEY, &guardJoin);
    rt_guard_join_aggression = guardJoin;

    // The medic beat. Clamps: the HP threshold stays inside (0, 1] because "heal above full" and
    // "never heal" are both just the off switch spelled badly; the ally reach is floored at 1 (0
    // would silently make it self-only, which is what medic_enabled=0 is for) and capped so it
    // can't reach across a room; the cooldown is floored at 0 (0 = as fast as AP allows).
    int medicEnabled = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MEDIC_ENABLED_KEY, &medicEnabled);
    rt_medic_enabled = medicEnabled != 0;

    double medicHpFraction = RT_MEDIC_HP_FRACTION;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MEDIC_HP_FRACTION_KEY, &medicHpFraction);
    if (medicHpFraction <= 0.0) {
        medicHpFraction = 0.05;
    } else if (medicHpFraction > 1.0) {
        medicHpFraction = 1.0;
    }
    rt_medic_hp_fraction = (float)medicHpFraction;

    int medicAllyTiles = RT_MEDIC_ALLY_TILES;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MEDIC_ALLY_TILES_KEY, &medicAllyTiles);
    if (medicAllyTiles < 1) {
        medicAllyTiles = 1;
    } else if (medicAllyTiles > 15) {
        medicAllyTiles = 15;
    }
    rt_medic_ally_tiles = medicAllyTiles;

    double medicCooldown = RT_MEDIC_COOLDOWN;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MEDIC_COOLDOWN_KEY, &medicCooldown);
    rt_medic_cooldown = medicCooldown < 0.0 ? 0.0f : (float)medicCooldown;

    // Run cost + resume threshold (the kiting nerf), both cfg-tunable. run_ap_cost
    // floored at 0 (0 = free running); resume_ap floored at 1 so there's always some
    // recovery walk before sprinting again.
    double runCost = RT_RUN_AP_COST;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUN_AP_COST_KEY, &runCost);
    rt_run_ap_cost = runCost < 0.0 ? 0.0f : (float)runCost;

    // Endurance scaling of the run cost: higher EN = cheaper sprint = runs longer.
    // Floored at 0 so a negative cfg can't make high EN cost MORE (the floor on the
    // final effective cost, RT_RUN_AP_MIN_COST, still guarantees running isn't free).
    double runEnduranceFactor = RT_RUN_AP_ENDURANCE_FACTOR;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUN_AP_ENDURANCE_FACTOR_KEY, &runEnduranceFactor);
    rt_run_ap_endurance_factor = runEnduranceFactor < 0.0 ? 0.0f : (float)runEnduranceFactor;

    int resumeAp = RT_RUN_RESUME_AP;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUN_RESUME_AP_KEY, &resumeAp);
    rt_run_resume_ap = resumeAp < 1 ? 1 : resumeAp;

    // Breather margin. Floored at 1: a margin of 0 would let one regen tick end the pause, which
    // is the stutter the run_winded latch exists to prevent, in a place where it would be worse
    // (a critter twitching between standing and stepping every frame).
    int breatherMargin = RT_NPC_BREATHER_MARGIN;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_BREATHER_MARGIN_KEY, &breatherMargin);
    rt_npc_breather_margin = breatherMargin < 1 ? 1 : breatherMargin;

    // Breather cap. 0 effectively DISABLES the breather: the latch still sets, but the very next
    // frame times it out, so nothing is ever suppressed for a length anyone can see. That is the
    // one-number rollback to the previous behaviour if this turns out to feel wrong.
    double breatherMax = RT_NPC_BREATHER_MAX_SECONDS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_BREATHER_MAX_KEY, &breatherMax);
    rt_npc_breather_max = breatherMax < 0.0 ? 0.0f : (float)breatherMax;

    // 0 disables the pause outright (the latch can then never arm), which is the honest rollback
    // if it turns out a chaser should simply never stop.
    int breatherTiles = RT_NPC_BREATHER_TILES;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_BREATHER_TILES_KEY, &breatherTiles);
    rt_npc_breather_tiles = breatherTiles < 0 ? 0 : breatherTiles;

    // Does an empty pool stop him or slow him? Defaults to 0 = the shipped stop, so the
    // experiment is opt-in and rolling it back is one number in fallout.cfg, not a rebuild.
    int emptyWalks = 0;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AP_EMPTY_WALKS_KEY, &emptyWalks);
    rt_ap_empty_walks = emptyWalks != 0;

    // Walk cost (Variant C2): AP/sec drained while walking in combat. Floored at 0
    // (0 = free walking). Smaller than the run cost so kiting bleeds slowly.
    double walkCost = RT_WALK_AP_COST;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_WALK_AP_COST_KEY, &walkCost);
    rt_walk_ap_cost = walkCost < 0.0 ? 0.0f : (float)walkCost;

    // Proximity regen curve. near is floored at the far value (the curve must never run
    // backwards, i.e. reward being far), far is floored at a small positive number so regen
    // can never stop dead at range, and the falloff distance at 2 tiles so there is always a
    // real ramp between the two ends.
    double regenNearMult = RT_REGEN_NEAR_MULT;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_NEAR_MULT_KEY, &regenNearMult);
    double regenFarMult = RT_REGEN_FAR_MULT;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_FAR_MULT_KEY, &regenFarMult);
    rt_regen_far_mult = regenFarMult < 0.1 ? 0.1f : (float)regenFarMult;
    rt_regen_near_mult = (float)regenNearMult < rt_regen_far_mult ? rt_regen_far_mult : (float)regenNearMult;

    int regenFalloff = RT_REGEN_FALLOFF_TILES;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_FALLOFF_TILES_KEY, &regenFalloff);
    rt_regen_falloff_tiles = regenFalloff < 2 ? 2 : regenFalloff;

    // The MOVING curve. Unlike the standing pair above, the near end is NOT floored above zero:
    // zero regen at contact while moving is the whole point of it (see RT_REGEN_NEAR_MULT_MOVING),
    // and it is safe only because rt_move_blocked stops the dude at 0 AP -- stopping puts him back
    // on the standing curve, which at contact is the fastest refill there is. Only negatives are
    // clamped away. This curve is allowed to run "backwards" (near < far) -- that is its design.
    double regenNearMoving = RT_REGEN_NEAR_MULT_MOVING;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_NEAR_MULT_MOVING_KEY, &regenNearMoving);
    rt_regen_near_mult_moving = regenNearMoving < 0.0 ? 0.0f : (float)regenNearMoving;
    double regenFarMoving = RT_REGEN_FAR_MULT_MOVING;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_FAR_MULT_MOVING_KEY, &regenFarMoving);
    rt_regen_far_mult_moving = regenFarMoving < 0.0 ? 0.0f : (float)regenFarMoving;

    // The NPC moving pair. Same rules as the dude's above: negatives clamped away, nothing else --
    // this curve is MEANT to run backwards (near < far), so the "near = max(near, far)" guard the
    // STANDING curve carries must never be copied here.
    double npcRegenNearMoving = RT_NPC_REGEN_NEAR_MULT_MOVING;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_REGEN_NEAR_MULT_MOVING_KEY, &npcRegenNearMoving);
    rt_npc_regen_near_mult_moving = npcRegenNearMoving < 0.0 ? 0.0f : (float)npcRegenNearMoving;
    double npcRegenFarMoving = RT_NPC_REGEN_FAR_MULT_MOVING;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_NPC_REGEN_FAR_MULT_MOVING_KEY, &npcRegenFarMoving);
    rt_npc_regen_far_mult_moving = npcRegenFarMoving < 0.0 ? 0.0f : (float)npcRegenFarMoving;

    // Aimed-shot slow motion. 1.0 disables it (full-speed combat while aiming, the pre-2026-08-29
    // behaviour); the load floor matches the one in combat_rt_time_scale so cfg cannot produce a
    // freeze by writing 0.
    double aimSlowmo = RT_AIM_SLOWMO;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AIM_SLOWMO_KEY, &aimSlowmo);
    rt_aim_slowmo = aimSlowmo < 0.05 ? 0.05f : (float)aimSlowmo;

    int regenMovingNpc = 0;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_REGEN_MOVING_NPC_KEY, &regenMovingNpc);
    rt_regen_moving_npc = regenMovingNpc != 0;

    // Ceiling of the Combat Speed slider's AP-tempo coupling (its value at slider 50).
    // Floored at 1.0: the slider may only ever speed the fight UP, never slow it below stock.
    double combatSpeedApMax = RT_COMBAT_SPEED_AP_MAX;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_AP_MAX_KEY, &combatSpeedApMax);
    rt_combat_speed_ap_max = combatSpeedApMax < 1.0 ? 1.0f : (float)combatSpeedApMax;

    // DT gate on the hit stagger (armored critters shrug off glancing hits and keep moving).
    // Floored at 0 (0 = disabled, every damaging hit flinches as before).
    double flinchDtFactor = RT_FLINCH_DT_FACTOR;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_FLINCH_DT_FACTOR_KEY, &flinchDtFactor);
    rt_flinch_dt_factor = flinchDtFactor < 0.0 ? 0.0f : (float)flinchDtFactor;

    // The stagger chance past the DT gate (MODS "Hits stagger"). Floored at 0 (0 = always staggers).
    double flinchChance = RT_FLINCH_CHANCE_FACTOR;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_FLINCH_CHANCE_FACTOR_KEY, &flinchChance);
    rt_flinch_chance_factor = flinchChance < 0.0 ? 0.0f : (float)flinchChance;

    // Sequence-initiative sharpness. Floored at 0 (0 = initiative off, a coin flip) and capped so
    // a wild cfg value can't turn it into the hard "highest Sequence always wins" that stagger-locks.
    double seqPower = RT_SEQ_INITIATIVE_POWER;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SEQ_INITIATIVE_POWER_KEY, &seqPower);
    if (seqPower < 0.0) {
        seqPower = 0.0;
    } else if (seqPower > 6.0) {
        seqPower = 6.0;
    }
    rt_seq_initiative_power = (float)seqPower;

    // Opening-swing initiative span. 0 = off (everyone ready at once, the old behaviour); capped so
    // a wild value can't leave a slow critter standing there for most of the fight.
    double initSpan = RT_INITIATIVE_SPAN;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_INITIATIVE_SPAN_KEY, &initSpan);
    if (initSpan < 0.0) {
        initSpan = 0.0;
    } else if (initSpan > 5.0) {
        initSpan = 5.0;
    }
    rt_initiative_span = (float)initSpan;

    // Post-conversation NPC hold. 0 = off; capped so a wild value can't leave a room full of
    // enemies politely waiting.
    double dialogGrace = RT_POST_DIALOG_GRACE;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_POST_DIALOG_GRACE_KEY, &dialogGrace);
    if (dialogGrace < 0.0) {
        dialogGrace = 0.0;
    } else if (dialogGrace > 10.0) {
        dialogGrace = 10.0;
    }
    rt_post_dialog_grace = (float)dialogGrace;

    // Knockdown ground dwell. Floored at 0 (0 = stand up as soon as the fall ends, the old
    // instant-recovery behaviour) and capped so a downed critter is never helpless for a whole
    // fight -- it is attackable the entire time it is down.
    double knockDwell = RT_KNOCKDOWN_DWELL_SECONDS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_KNOCKDOWN_DWELL_KEY, &knockDwell);
    if (knockDwell < 0.0) {
        knockDwell = 0.0;
    } else if (knockDwell > 10.0) {
        knockDwell = 10.0;
    }
    rt_knockdown_dwell = (float)knockDwell;

    // Hard ceiling on total ground time (anti-pin). 0 = off. Floored at the dwell itself: a cap
    // BELOW the dwell would fire first every single time and silently replace it.
    double knockCap = RT_KNOCKDOWN_GROUND_CAP;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_KNOCKDOWN_MAX_GROUND_KEY, &knockCap);
    if (knockCap <= 0.0) {
        knockCap = 0.0;
    } else {
        if (knockCap < knockDwell) {
            knockCap = knockDwell;
        }
        if (knockCap > 20.0) {
            knockCap = 20.0;
        }
    }
    rt_knockdown_ground_cap = (float)knockCap;

    // How hard Agility pulls on that dwell. 0 disables the stat influence and leaves every
    // character on the flat number above.
    double knockAgi = RT_KNOCKDOWN_AGILITY_FACTOR;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_KNOCKDOWN_AGILITY_KEY, &knockAgi);
    rt_knockdown_agility_factor = knockAgi < 0.0 ? 0.0f : (knockAgi > 0.15 ? 0.15f : (float)knockAgi);

    // Follow-cam smoothing time constant. 0 = the old hard lock (dude welded to screen centre).
    double camK = RT_CAM_SMOOTH_K;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CAM_SMOOTH_KEY, &camK);
    rt_cam_smooth_k = camK < 0.0 ? 0.0f : (camK > 40.0 ? 40.0f : (float)camK);

    // Lean toward an aimed target off the screen (MODS). Default ON since the release (his setting).
    int camLean = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CAM_LOCK_LEAN_KEY, &camLean);
    rt_cam_lean_on = camLean != 0;

    // R3 auto-lock reach from Perception. Floors keep a silly cfg from making the lock useless
    // (0 = nothing is ever lockable) or unbounded again.
    double lockScale = RT_LOCK_PERCEPTION_SCALE;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_LOCK_RANGE_PE_SCALE_KEY, &lockScale);
    if (lockScale < 0.0) {
        lockScale = 0.0;
    } else if (lockScale > 10.0) {
        lockScale = 10.0;
    }
    rt_lock_pe_scale = (float)lockScale;

    int lockMin = RT_LOCK_MIN_TILES;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_LOCK_RANGE_MIN_KEY, &lockMin);
    if (lockMin < 1) {
        lockMin = 1;
    } else if (lockMin > 50) {
        lockMin = 50;
    }
    rt_lock_min_tiles = lockMin;

    // The controller is owned by gamepad_ui (it must work outside a game
    // session too); combat_rt just borrows the handle.
}

void combat_rt_exit()
{
    rt_reset_session_state();
    rt_enabled = false;
}

void combat_rt_reset()
{
    rt_reset_session_state();
    combat_state &= ~COMBAT_STATE_0x01;

    // ⚠️ CLEARED HERE SO IT CANNOT SURVIVE A MID-SESSION LOAD -- the exact shape of bug that has
    // bitten this project before. game_reset() runs from PrepLoad, which is save handler #0, long
    // before our chunk at #26, so the order is always: clear, then fill from the save being
    // loaded. A new game gets the same clear and therefore the default -- everyone heals you
    // until you tell them not to.
    combat_rt_medic_optout_clear();

    // ⚠️ AND THE PENDING STEP-BACK, WHICH IS A RAW Object*. Objects are destroyed and rebuilt on
    // every map change and load, so a request left standing across one points at freed memory and
    // would be dereferenced on the next dialogue close. Exactly the class of stale global that has
    // bitten this project before.
    combat_rt_request_step_back(NULL);
}

// Refresh rt_door_tiles for the dude's current tile. Usable, UNLOCKED doors only: pulling the
// player toward a door he cannot open is worse than no assist at all.
static void rt_door_cache_refresh()
{
    rt_door_count = 0;
    rt_door_cache_tile = obj_dude->tile;
    rt_door_cache_elev = obj_dude->elevation;

    Object** list = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_SCENERY, &list);

    for (int i = 0; i < count && rt_door_count < RT_DOOR_MAGNET_MAX; i++) {
        Object* obj = list[i];
        if (obj == NULL || obj->elevation != obj_dude->elevation) {
            continue;
        }

        Proto* proto;
        if (proto_ptr(obj->pid, &proto) == -1 || proto->scenery.type != SCENERY_TYPE_DOOR) {
            continue;
        }

        if (obj_is_locked(obj)) {
            continue;
        }

        int dist = tile_dist(obj_dude->tile, obj->tile);
        if (dist <= 0 || dist > RT_DOOR_MAGNET_TILES) {
            continue;
        }

        rt_door_tiles[rt_door_count++] = obj->tile;
    }

    if (list != NULL) {
        obj_delete_list(list);
    }
}

// Bend a stick angle (degrees, screen convention: right = 0, up = +90) toward the nearest doorway
// that the player is already roughly pointing at. Returns the angle unchanged when no doorway
// qualifies, which is the overwhelmingly common case.
static float rt_door_magnet_apply(float angle)
{
    if (rt_door_magnet <= 0.0f || obj_dude == NULL) {
        return angle;
    }

    if (obj_dude->tile != rt_door_cache_tile || obj_dude->elevation != rt_door_cache_elev) {
        rt_door_cache_refresh();
    }

    if (rt_door_count == 0) {
        return angle;
    }

    int dudeX;
    int dudeY;
    if (tile_coord(obj_dude->tile, &dudeX, &dudeY, obj_dude->elevation) == -1) {
        return angle;
    }

    int bestDist = RT_DOOR_MAGNET_TILES + 1;
    float bestDev = 0.0f;

    for (int i = 0; i < rt_door_count; i++) {
        int doorX;
        int doorY;
        if (tile_coord(rt_door_tiles[i], &doorX, &doorY, obj_dude->elevation) == -1) {
            continue;
        }

        // Same convention as the stick: screen Y is +down, so negate it to make up positive.
        float doorAngle = atan2f(-(float)(doorY - dudeY), (float)(doorX - dudeX)) * 57.2957795f;

        // Signed shortest difference, -180..180.
        float dev = fmodf(doorAngle - angle + 540.0f, 360.0f) - 180.0f;
        if (dev < -RT_DOOR_MAGNET_CONE || dev > RT_DOOR_MAGNET_CONE) {
            continue;
        }

        int dist = tile_dist(obj_dude->tile, rt_door_tiles[i]);
        if (dist < bestDist) {
            bestDist = dist;
            bestDev = dev;
        }
    }

    if (bestDist > RT_DOOR_MAGNET_TILES) {
        return angle;
    }

    // Taper with distance: full strength on the doorstep, nothing at the edge of range. Without the
    // taper a door four tiles away tugs as hard as the one you are stepping through, and walking
    // PAST a doorway -- which is a thing players do constantly -- starts to feel sticky.
    float taper = 1.0f - (float)(bestDist - 1) / (float)RT_DOOR_MAGNET_TILES;
    if (taper < 0.0f) {
        taper = 0.0f;
    }

    return angle + bestDev * rt_door_magnet * taper;
}

// Who is standing in the first tile along `rotation`, if it is someone we are willing to walk
// AROUND rather than into? NULL when nothing blocks, when the blocker is scenery or a wall, or
// when it is a hostile.
//
// Bystanders only. A HOSTILE keeps blocking exactly as before: closing to melee, holding a firing
// angle and backing off a step all stay predictable, and the assist can never reposition you in a
// fight you did not ask it to. rt_is_hostile answers false for anyone outside the combat registry,
// which is what makes every peaceful townsfolk slide-able without a second test.
//
// ⚠️ THAT SAME "false for anyone outside the registry" IS ALSO THE HOLE, which is why the caller
// gates this on `!rt_engaged`. rt_is_hostile reads the actor's sticky flag, so a critter you
// attacked first (and who therefore never aggroed onto you) reads as a peaceful bystander -- and
// the assist then refuses to let you walk into the very person you are fighting, deflecting a held
// stick by a whole 60-degree rotation, every frame, for as long as you hold it. Traced in
// trace_2026-08-24_234756: `want=3 took=4` five times across 1.5 s, each within 2 s of an attack,
// which is exactly the report -- "držel jsem směr dolů, po útoku jsem ztratil kontrolu nad dudem a
// šel jsem směrem, kam jsem nechtěl jít".
static Object* rt_bystander_blocking(int rotation)
{
    Object* blocker = obj_blocking_at(obj_dude, tile_num_in_direction(obj_dude->tile, rotation, 1), map_elevation);
    if (blocker == NULL || !rt_is_critter(blocker) || rt_is_hostile(blocker)) {
        return NULL;
    }

    return blocker;
}

// Is the first step that way clear of ANYTHING -- critter, wall, scenery? A slide may only land on
// a genuinely open tile; rounding a companion by walking into a wall is not an improvement.
static bool rt_step_is_free(int rotation)
{
    return obj_blocking_at(obj_dude, tile_num_in_direction(obj_dude->tile, rotation, 1), map_elevation) == NULL;
}

// THE ARROWS AS THE LEFT STICK (the keyboard as a controller, 2026-10-10 -- the layout is in
// gamepad_ui_handle_key): the held arrows as a full-deflection vector in SDL stick space (+y is
// down), eight ways with two held. NOT gated on Ctrl/Alt/Shift: those are attack, run and the aim
// lock, all of which are held WHILE walking.
static bool rt_keyboard_walk(float* outX, float* outY)
{
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    if (keys == NULL) {
        return false;
    }

    float x = 0.0f;
    float y = 0.0f;
    if (keys[SDL_SCANCODE_UP]) {
        y -= 1.0f;
    }
    if (keys[SDL_SCANCODE_DOWN]) {
        y += 1.0f;
    }
    if (keys[SDL_SCANCODE_LEFT]) {
        x -= 1.0f;
    }
    if (keys[SDL_SCANCODE_RIGHT]) {
        x += 1.0f;
    }
    if (x == 0.0f && y == 0.0f) {
        return false;
    }

    float len = sqrtf(x * x + y * y);
    *outX = x / len;
    *outY = y / len;
    return true;
}

// --- KEYBOARD & MOUSE: THE MOUSE AIMS, ITS LEFT BUTTON ATTACKS (2026-10-10) ------------------------
//
// His pick of two designs, "2", twin-stick (the keys are in gamepad_ui_handle_key). The enemy under
// the cursor -- or the nearest one within RT_MOUSE_AIM_RADIUS of it, because hitting a walking sprite
// pixel-exactly is a test of the hand, not of the character -- is the mouse's target: its to-hit is
// drawn over it, a held left button keeps attacking it exactly like a held RT (the cooldown paces
// it), and a right-click opens the called-shot window on it (combat_rt_handle_input).
//
// HOSTILES ONLY: a click on a peaceful townsman stays vanilla's -- talk, look, use -- and is never an
// attack. Only while the mouse is actually in use (gamepad_ui_mouse_in_use), so a pad player's resting
// cursor claims nothing, and never while the pad holds a lock, which owns the cursor and glues it to
// its own target.
static Object* rt_mouse_target = NULL;
static bool rt_mouse_prev_lmb = false;
static bool rt_mouse_armed = false; // the current press began over the world, not over the bar
// This press has attacked. Until the button comes up, every left-button event stays ours, even with
// no target under the cursor any more: a raider who drops mid-hold would otherwise hand the still-held
// button to vanilla, which reads a held click on the ground as "walk there" -- the dude would set off
// for the corpse on his own.
static bool rt_mouse_hold_claimed = false;
#define RT_MOUSE_AIM_RADIUS 24 // game pixels from a sprite's box that still count as "on" it

static bool rt_point_in_world(int x, int y)
{
    return display_win != -1 && win_get_top_win(x, y) == display_win;
}

static Object* rt_pick_mouse_target(int mx, int my)
{
    Object* under = object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (under != NULL && under != obj_dude && rt_in_registry(under) && rt_is_hostile(under) && rt_is_targetable(under)) {
        return under;
    }

    // Nearest by distance to the sprite's BOX (0 anywhere on it), ties broken by the box centre --
    // two raiders overlapping on screen go to the one the cursor is nearer the middle of.
    Object* best = NULL;
    long bestScore = (long)(RT_MOUSE_AIM_RADIUS * RT_MOUSE_AIM_RADIUS + 1) * 100000L;
    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        if (critter == NULL || critter == obj_dude || critter->elevation != map_elevation
            || (critter->flags & OBJECT_HIDDEN) != 0 || !rt_is_hostile(critter) || !rt_is_targetable(critter)) {
            continue;
        }

        Rect b;
        obj_bound(critter, &b);
        int dx = mx < b.ulx ? b.ulx - mx : (mx > b.lrx ? mx - b.lrx : 0);
        int dy = my < b.uly ? b.uly - my : (my > b.lry ? my - b.lry : 0);
        int cx = (b.ulx + b.lrx) / 2 - mx;
        int cy = (b.uly + b.lry) / 2 - my;
        long score = (long)(dx * dx + dy * dy) * 100000L + (long)(cx * cx + cy * cy);
        if (score < bestScore) {
            bestScore = score;
            best = critter;
        }
    }
    return best;
}

static void rt_mouse_update()
{
    // REAL left button only (SDL's own state, not the game's): the pad's A arrives in the game's mouse
    // state as a synthesised click, and A must never become an attack. Sampled before any return so
    // the edge is never stale.
    bool lmb = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0;
    bool lmbEdge = lmb && !rt_mouse_prev_lmb;
    rt_mouse_prev_lmb = lmb;
    if (!lmb) {
        rt_mouse_armed = false;
        rt_mouse_hold_claimed = false;
    }

    Object* previous = rt_mouse_target;
    rt_mouse_target = NULL;

    if (rt_lock_target != NULL || !gamepad_ui_mouse_in_use()) {
        return;
    }

    // A skill or "use item on" cursor (the 1-8 keys, a rope, a lockpick) means the next click is that
    // skill's, even on an enemy -- steal from him, patch him up. Only the plain cursors aim.
    int cursorMode = gmouse_3d_get_mode();
    if (cursorMode != GAME_MOUSE_MODE_MOVE && cursorMode != GAME_MOUSE_MODE_ARROW && cursorMode != GAME_MOUSE_MODE_CROSSHAIR) {
        return;
    }

    int mx;
    int my;
    mouse_get_position(&mx, &my);
    bool inWorld = rt_point_in_world(mx, my);
    if (lmbEdge) {
        rt_mouse_armed = inWorld;
    }
    if (!inWorld) {
        return;
    }

    rt_mouse_target = rt_pick_mouse_target(mx, my);
    if (rt_mouse_target == NULL) {
        return;
    }

    if (rt_mouse_target != previous) {
        trace_event(TRACE_INPUT, "mouse-aim", "target=%d id=%d dist=%d t=%.2f", rt_mouse_target->pid,
            rt_mouse_target->id, obj_dist(obj_dude, rt_mouse_target), rt_clock);
    }

    // The to-hit over the target every frame -- the same indicator the pad's lock drives, and for the
    // same reason: the engine's own only paints after the cursor has rested ~250 ms.
    gmouse_3d_show_to_hit(rt_mouse_target);

    // A press that began over the bar and slid into the world is not a shot.
    if (lmb && rt_mouse_armed) {
        rt_mouse_hold_claimed = true;
        int hitMode = rt_player_hit_mode();
        // AIMED MODE in the weapon slot = this click goes to the bound part, as RT's does.
        int loc = rt_player_called_loc(hitMode);
        if (obj_dist(obj_dude, rt_mouse_target) <= item_w_range(obj_dude, hitMode)) {
            rt_set_player_intent(rt_mouse_target, hitMode, loc);
            if (lmbEdge) {
                trace_event(TRACE_ACTION, "attack", "target=%d hm=%d loc=%d locked=0 src=mouse t=%.2f",
                    rt_mouse_target->pid, hitMode, loc, rt_clock);
            }
        } else if (lmbEdge) {
            trace_event(TRACE_INPUT, "attack-far", "target=%d dist=%d range=%d src=mouse t=%.2f",
                rt_mouse_target->pid, obj_dist(obj_dude, rt_mouse_target), item_w_range(obj_dude, hitMode), rt_clock);
        }
    }
}

Object* combat_rt_mouse_target()
{
    return rt_mouse_target;
}

// Q / E on the keyboard step the lock one enemy left / right of the current one -- the right stick's
// job (rt_directional_lock), one press per step. Asked from the key handler, done in the poll.
static int rt_kb_lock_step = 0;

void combat_rt_request_lock_step(int dir)
{
    rt_kb_lock_step = dir;
}

// The held keyboard modifiers that stand in for pad buttons (gamepad_ui_handle_key has the layout).
static bool rt_kb_mod_held(SDL_Keymod mask)
{
    return (SDL_GetModState() & mask) != 0;
}

void combat_rt_poll_gamepad()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return;
    }

    // While a modal menu/dialog has control, the sticks belong to the UI layer
    // (cursor + item snapping), not the player. Halt any in-flight stick walk
    // and skip all gameplay gamepad input.
    if (gamepad_ui_menu_active()) {
        rt_fm_release();
        rt_stick_stop();
        rt_stick_last_sector = -1;
        return;
    }

    // While a radial wheel (skills on LB, drugs Y, weapons X, ammo RB) OR the on-body aimed-shot
    // picker (LT held) is up, the sticks drive the selection (right = pick) and the player should
    // not walk or lock-switch; halt any in-flight step and skip gameplay input (including the
    // right-stick lock cycling further below).
    if (radial_wheel_is_open() || gamepad_ui_aim_body_active()) {
        rt_fm_release();
        rt_stick_stop();
        rt_stick_last_sector = -1;
        return;
    }

    // Borrow the shared controller handle owned by gamepad_ui. NULL is fine since the keyboard &
    // mouse layer (2026-10-10): W/A/S/D walk and the mouse aims without one, and every read of the
    // pad below checks for it.
    SDL_GameController* rt_gamepad = gamepad_ui_get_controller();
    if (rt_gamepad != NULL) {
        SDL_GameControllerUpdate();
    }

    // Left stick -> continuous walking in chained multi-tile segments while
    // held. The analog angle becomes a HEADING (see FREE WALK ANGLE), which the
    // step planner follows by mixing the two hex directions bracketing it; with
    // the free angle switched off it falls back to bucketing the angle into 6
    // sectors of 60 degrees, one per hex rotation. SDL's Y axis is +down, so -y
    // makes "up" positive; atan2 then gives up = +90 deg, right = 0 deg.
    float x = rt_gamepad != NULL ? SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_LEFTX) / RT_GAMEPAD_AXIS_MAX : 0.0f;
    float y = rt_gamepad != NULL ? SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_LEFTY) / RT_GAMEPAD_AXIS_MAX : 0.0f;

    // THE ARROWS WALK EXACTLY LIKE THE STICK (the keyboard as a controller, 2026-10-10): eight ways at
    // full deflection, into the same heading code below -- doorway assist, crowd assist, free movement
    // and the AP model all apply unchanged. Only while the stick is idle, so a pad player resting a
    // hand on the keyboard is never fought.
    bool kbWalk = false;
    if (x * x + y * y <= RT_GAMEPAD_DEADZONE * RT_GAMEPAD_DEADZONE) {
        float kx;
        float ky;
        if (rt_keyboard_walk(&kx, &ky)) {
            x = kx;
            y = ky;
            kbWalk = true;
        }
    }

    // RUN instead of walk. IN COMBAT running drains the AP
    // pool (rt_run_ap_cost), so when it's empty -- or recovering while rt_run_blocked
    // -- the dude drops to a free walk. Feeding this effective gait into rt_stick_move
    // makes its gait-change path do the seamless run->walk restart (no stop/freeze:
    // movement itself is never gated, only the run/walk choice). Out of combat,
    // running is always free.
    // ⚠️ RUN IS A TOGGLE ON X, NOT A HELD SHOULDER (2026-09-16 layout). It was LB-held, and before
    // that LT-held; X could not stay a hold because holding X opens the weapon wheel. The local
    // keeps its name because every gait test below reads it as "the run key", but there is no key
    // being held any more -- gamepad_ui owns the state and this just asks for it.
    // ...and on the keyboard Alt is the run, held like X.
    bool ltHeld = gamepad_ui_run_on() || (kbWalk && rt_kb_mod_held(KMOD_ALT));
    // The Bonus Move reserve pays for running as well -- it is movement, which is all it is for.
    bool run = ltHeld && (!rt_engaged || ((rt_player_ap > 0 || rt_bonus_move_ap > 0) && !rt_run_blocked));

    // Flight-recorder movement input: emit on a heading/gait EDGE (a held stick coalesces
    // to nothing after the first line). TRACE_INPUT reflects what the stick ASKED for even
    // when AP-blocking stops the actual step, so a "why didn't I move" report is visible.
    //
    // ⚠️ THE EDGE IS THE ANGLE, NOT THE ROTATION. Under a free heading the rotation alternates on
    // purpose from tile to tile, so an edge on it would put a line in the log for every tile walked.
    static float rt_traced_stick_ang = 0.0f;
    static bool rt_traced_stick_run = false;
    static bool rt_traced_stick_on = false;

    if (x * x + y * y > RT_GAMEPAD_DEADZONE * RT_GAMEPAD_DEADZONE) {
        // Map the analog angle to the hex rotation whose on-screen walk
        // direction is nearest. Per off_tile (tile.cc), the actual screen walk
        // angles are E 0, NE 36.9, NW 143.1, W 180, SW 216.9, SE 323.1 deg --
        // not evenly spaced, because the iso squash pulls the diagonals toward
        // horizontal. Equal 60-deg sectors centered at 0/60/120/180/240/300
        // keep each true walk direction inside its own sector with symmetric
        // error (exact for E/W, 23 deg off-center for the diagonals).
        //
        // ⚠️ SYMMETRIC ERROR IS STILL ERROR, AND IT IS NOT 30 DEGREES. The sectors are even but the
        // directions they stand for are not, so the gap between what you ask for and what you get
        // peaks at 53 deg on the vertical -- push straight up, walk up-right. That is what the free
        // angle above removes; this bucketing is now the fallback, and the sector is also what the
        // crowd assist measures its lean against.
        static const int sectorToRotation[6] = {
            ROTATION_E, //  0  centered at   0 deg -> walks right-screen
            ROTATION_NE, // 1  centered at  60 deg -> walks up-right (36.9)
            ROTATION_NW, // 2  centered at 120 deg -> walks up-left (143.1)
            ROTATION_W, //  3  centered at 180 deg -> walks left-screen
            ROTATION_SW, // 4  centered at 240 deg -> walks down-left (216.9)
            ROTATION_SE, // 5  centered at 300 deg -> walks down-right (323.1)
        };

        // atan2(-y, x): SDL Y is +down, so -y makes up positive (up = +90 deg,
        // right = 0 deg). RT_GAMEPAD_ANGLE_OFFSET is a feel knob on top.
        float angle = atan2f(-y, x) * 57.2957795f + RT_GAMEPAD_ANGLE_OFFSET;

        // Doorway assist, applied BEFORE the sector bucketing below so it can only ever change
        // which sector a near-miss falls into. See rt_door_magnet_apply.
        angle = rt_door_magnet_apply(angle);

        angle = fmodf(angle, 360.0f);
        if (angle < 0.0f) {
            angle += 360.0f;
        }

        int sector = ((int)((angle + 30.0f) / 60.0f)) % 6;

        // Hysteresis: stay in the previous sector unless the stick is more
        // than 5 deg past its edge. Without it, hand tremor on a sector
        // boundary redirects (clear + re-register) the walk every frame, and
        // the dude marches in place.
        if (rt_stick_last_sector != -1 && sector != rt_stick_last_sector) {
            float dev = fmodf(angle - 60.0f * rt_stick_last_sector + 540.0f, 360.0f) - 180.0f;
            if (dev > -35.0f && dev < 35.0f) {
                sector = rt_stick_last_sector;
            }
        }
        rt_stick_last_sector = sector;

        int rotation = sectorToRotation[sector];

        // CROWD ASSIST -- step AROUND a bystander instead of jamming into them. Same seam as the
        // doorway magnet, opposite sense: the magnet bends the angle TOWARD a target, this bends it
        // AWAY from an obstacle. Only the FIRST tile is tested because only the first tile decides:
        // rt_stick_segment_start refuses the whole segment when that one is blocked (dist == 0),
        // which is the stop the player feels.
        //
        // The side is chosen by where inside the 60-degree sector the stick actually points -- lean
        // left of centre and you round them on the left. That makes it read as "he took the gap I
        // was already leaning into" instead of a coin flip. The far side is the fallback when the
        // near one is occupied too; if both are, nothing changes and you stop exactly as before.
        //
        // ⚠️ Deflects the ROTATION, not the angle, and does it AFTER rt_stick_last_sector was set
        // from the undeflected sector -- the hysteresis belongs to the stick, not to the assist.
        // Latching the deflected direction would hand it back as "the previous sector" next frame
        // and keep the dude walking sideways after the obstacle had already cleared.
        // ⚠️ OFF WHILE A FIGHT IS LIVE. The assist is crowd navigation -- get past Ian in a doorway
        // -- and in a fight the player is aiming their movement, so a 60-degree "helpful" deflection
        // is not help, it is losing the character. It fired repeatedly in the seconds right after
        // attacks (see rt_bystander_blocking for the traced runs and why a critter you struck first
        // reads as a bystander). Without it you simply stop against the blocker, which is vanilla,
        // predictable, and recoverable by steering one notch yourself.
        Object* bystander = (rt_npc_slide && !rt_engaged) ? rt_bystander_blocking(rotation) : NULL;
        if (bystander != NULL) {
            float dev = fmodf(angle - 60.0f * sector + 540.0f, 360.0f) - 180.0f;
            int nearSector = dev >= 0.0f ? (sector + 1) % 6 : (sector + 5) % 6;
            int farSector = dev >= 0.0f ? (sector + 5) % 6 : (sector + 1) % 6;

            int slid = rotation;
            if (rt_step_is_free(sectorToRotation[nearSector])) {
                slid = sectorToRotation[nearSector];
            } else if (rt_step_is_free(sectorToRotation[farSector])) {
                slid = sectorToRotation[farSector];
            }

            if (slid != rotation) {
                rotation = slid;

                static float rt_slide_traced = -1.0f;
                if (rt_clock - rt_slide_traced >= 0.25f) {
                    rt_slide_traced = rt_clock;
                    trace_event(TRACE_INPUT, "npc-slide", "want=%d took=%d dev=%.1f blocker=%d dead=%d t=%.2f",
                        sectorToRotation[sector], rotation, dev, bystander->pid,
                        critter_is_dead(bystander) ? 1 : 0, rt_clock);
                }
            }
        }

        float angDev = fmodf(angle - rt_traced_stick_ang + 540.0f, 360.0f) - 180.0f;
        if (angDev < 0.0f) {
            angDev = -angDev;
        }

        if (!rt_traced_stick_on || angDev > 15.0f || run != rt_traced_stick_run) {
            trace_event(TRACE_INPUT, "stick", "ang=%.0f dir=%d gait=%s blocked=%d free=%d t=%.2f",
                angle, rotation, run ? "run" : "walk", rt_move_blocked ? 1 : 0,
                rt_free_move ? 1 : 0, rt_clock);
            rt_traced_stick_ang = angle;
            rt_traced_stick_run = run;
            rt_traced_stick_on = true;
        }

        // Out of AP (walked/ran the pool dry): stop until it regenerates to the resume
        // threshold. Standing lets the pool climb back (see rt_move_blocked).
        if (rt_free_move) {
            // The body leaves the grid. Out of AP it stops (or walks, under ap_empty_walks) for
            // exactly the same reasons the step mover does -- the AP model is not what changed.
            if (rt_move_blocked && !rt_ap_empty_walks) {
                rt_fm_release();
            } else {
                rt_fm_drive(angle, run && !rt_move_blocked, rt_frame_dt);
            }
        } else if (rt_move_blocked && !rt_ap_empty_walks) {
            rt_stick_stop();
        } else if (rt_move_blocked) {
            // Out of AP, but this mode leaves him mobile: force the WALK gait whatever LT says.
            // He can still leave a fight, just slower than the thing chasing him -- which is what
            // makes it an exit and not an escape hatch.
            rt_stick_move(rotation, false);
        } else {
            rt_stick_move(rotation, run);
        }
    } else {
        rt_stick_last_sector = -1;
        rt_fm_release();
        rt_stick_stop();
        if (rt_traced_stick_on) {
            trace_event(TRACE_INPUT, "stick-stop", "t=%.2f", rt_clock);
            rt_traced_stick_on = false;
        }
    }

    // --- Right-stick lock-on aiming -------------------------------------------
    // Does the target's sprite still overlap the world window at all? Defined here so both the
    // lock-drop test and the cursor glue below use one answer. Anything that stops us knowing
    // (no window) counts as ON screen -- a lock must never be dropped on a technicality.
    struct OnScreen {
        static bool test(Object* obj)
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
    };

    // Maintain a held lock. If the locked target DIED or despawned, AUTO-HOP the lock to the
    // nearest remaining enemy (within lock range) so you keep fighting through a crowd without
    // re-locking after every kill -- only fall back to no-lock if nothing's left nearby. If the
    // target merely FLED past RT_LOCK_DROP_DIST, just drop the lock (don't chase a runner).
    // Registry membership is checked first (pointer-only) so a freed object is never dereferenced.
    if (rt_lock_target != NULL) {
        if (!rt_in_registry(rt_lock_target) || !rt_is_targetable(rt_lock_target)) {
            // Auto-hop ONLY to another genuine enemy -- never onto a neutral/ally that
            // happens to be nearest (rt_nearest_lockable would include perceivable
            // different-team neutrals). No enemy in range -> just drop the lock.
            Object* next = rt_nearest_hostile_lockable();
            rt_lock_target = (next != NULL && obj_dist(obj_dude, next) <= RT_LOCK_HOP_DIST)
                ? next
                : NULL;
        } else if (obj_dist(obj_dude, rt_lock_target) > RT_LOCK_DROP_DIST) {
            rt_lock_target = NULL;
        }
    }

    // OFF-SCREEN DROP. Kept separate from the distance leash above because it is a different
    // question: RT_LOCK_DROP_DIST asks "has it fled the area", this asks "can the player still see
    // what he is aiming at". The second is what actually bounds the lock in practice -- the view is
    // far narrower than 40 tiles.
    static float rt_lock_offscreen_since = -1.0f;
    // ...unless the camera lean can bring it into view: then the lean is about to show it, and
    // dropping the lock in the 0.4 s it takes to get there would defeat the lean entirely.
    float leanWantX;
    float leanWantY;
    bool leanFits = false;
    rt_cam_lean_want(rt_lock_target, &leanWantX, &leanWantY, &leanFits);
    if (rt_lock_target != NULL && !leanFits && !OnScreen::test(rt_lock_target)) {
        if (rt_lock_offscreen_since < 0.0f) {
            rt_lock_offscreen_since = rt_clock;
        } else if (rt_clock - rt_lock_offscreen_since >= RT_LOCK_OFFSCREEN_SECONDS) {
            trace_event(TRACE_ACTION, "lock-offscreen", "target=%d t=%.2f",
                rt_lock_target->pid, rt_clock);
            rt_lock_target = NULL;
            rt_lock_offscreen_since = -1.0f;
        }
    } else {
        rt_lock_offscreen_since = -1.0f;
    }

    // ⚠️ R3 NO LONGER TOGGLES THE LOCK (2026-10-09). It is the called-shot window now (gamepad_ui,
    // "R3 bude vybírání končetin"), and the lock is LT's HOLD alone (LB held was tried for a few
    // minutes and dropped: "LB bude jen reload, srát na zámek"). The toggle's lesson stays true for
    // the hold: locking and letting go sound different ("zamířil na npc R3 a stisknul áčko a místo
    // střelby se dveře zavřely" was a silent unlock).

    // ⚠️ LT AIMS THE SAME WAY R3 DOES -- his words, "LT JE AIMING JAKO JE R3". Two ways into one
    // state, for two different grips: R3 is a TOGGLE you set and forget, LT is a HOLD you keep your
    // finger on, the way an aim button works everywhere else. The lock they drive is the same
    // rt_lock_target, so the right stick still switches targets, the crosshair still tracks, and
    // the LB picker still aims at whatever is locked.
    //
    // ⚠️ LT ONLY DROPS A LOCK IT TOOK ITSELF. Releasing it must not cancel a lock the player set
    // deliberately with R3 -- that would make the toggle useless the moment a finger brushed the
    // trigger. `rt_lt_owns_lock` is that distinction, and it is cleared whenever the lock changes
    // hands (R3 toggling, the target dying, combat ending -- all of which go through rt_lock_target
    // becoming NULL or being reassigned above).
    //
    // Read through the Schmitt helper, never a bare threshold: a held trigger reports occasional
    // hard zeros, and each one would read as "let go" and drop the aim for a frame.
    static bool rt_lt_owns_lock = false;
    // ...or Shift held on the keyboard, which is LT there.
    bool ltAim = gamepad_ui_aim_trigger_held() || rt_kb_mod_held(KMOD_SHIFT);

    if (ltAim && rt_lock_target == NULL) {
        rt_lock_target = rt_lock_pick_initial();
        if (rt_lock_target != NULL) {
            rt_lt_owns_lock = true;
            trace_event(TRACE_ACTION, "lock-on", "target=%d src=LT t=%.2f",
                rt_lock_target->pid, rt_clock);
            gsound_play_sfx_file("ib1p1xx1");
        }
    } else if (!ltAim && rt_lt_owns_lock) {
        rt_lt_owns_lock = false;
        if (rt_lock_target != NULL) {
            trace_event(TRACE_ACTION, "lock-off", "target=%d src=LT t=%.2f",
                rt_lock_target->pid, rt_clock);
            gsound_play_sfx_file("iisxxxx1");
            rt_lock_target = NULL;
        }
    }
    if (rt_lock_target == NULL) {
        rt_lt_owns_lock = false;
    } else {
        rt_lock_last_target = rt_lock_target; // the next aim starts here (rt_lock_pick_initial)
    }

    // While locked: aim the RIGHT STICK toward the enemy you want and the lock switches to the
    // best-matching enemy in that screen direction (rt_directional_lock) -- just point at who you
    // want. The cursor is glued to the locked critter's on-screen center every frame so the aim
    // point tracks it (the stick no longer free-aims while locked). Edge-triggered with a
    // re-center arm: one push steps exactly one target and a held stick doesn't spin.
    // Tracks whether WE forced the crosshair cursor for a lock, so we only revert
    // it on unlock (and don't clobber a crosshair the player set themselves).
    static bool rt_lock_set_crosshair = false;

    // The keyboard's Q / E: one step left / right of the current target, anchored on it exactly the
    // way a fresh right-stick push is. Nothing to step from without a lock -- hold Shift first.
    if (rt_kb_lock_step != 0) {
        int dir = rt_kb_lock_step;
        rt_kb_lock_step = 0;
        if (rt_lock_target != NULL) {
            rt_lock_push_anchor = rt_lock_target;
            if (rt_directional_lock((float)dir, 0.0f)) {
                gsound_play_sfx_file("ib1p1xx1");
                trace_event(TRACE_INPUT, "aim", "target=%d id=%d dist=%d src=key dir=%d t=%.2f",
                    rt_lock_target != NULL ? rt_lock_target->pid : -1,
                    rt_lock_target != NULL ? rt_lock_target->id : -1,
                    rt_lock_target != NULL ? obj_dist(obj_dude, rt_lock_target) : -1, dir, rt_clock);
            }
            rt_lock_push_anchor = NULL;
        }
    }

    if (rt_lock_target != NULL) {
        float rx = rt_gamepad != NULL ? SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_RIGHTX) / RT_GAMEPAD_AXIS_MAX : 0.0f;
        float ry = rt_gamepad != NULL ? SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_RIGHTY) / RT_GAMEPAD_AXIS_MAX : 0.0f;
        float rmag = sqrtf(rx * rx + ry * ry);
        // CONTINUOUS aiming -- no arm/re-centre ratchet. Sweep the stick and the lock sweeps with
        // it, matching the interact picker exactly so both selections feel like one gesture.
        // rt_directional_lock reports only real changes, so the blip and the trace line fire once
        // per switch rather than every frame the stick is held.
        if (rmag > RT_LOCK_CYCLE_THRESHOLD) {
            // The push's anchor is whoever is locked at the moment the push BEGINS, and it holds
            // until the stick comes home (below) -- see rt_directional_lock.
            if (rt_lock_push_anchor == NULL) {
                rt_lock_push_anchor = rt_lock_target;
            }
            if (rt_directional_lock(rx, ry)) {
                gsound_play_sfx_file("ib1p1xx1");
                // `step` is the whole point of the rule now -- a switch that lands on something far
                // from the target you had, when a nearer one was on that side, is the bug, and this
                // is where it would show. `dist` is from you, as before.
                Object* anchor = rt_lock_step_anchor();
                trace_event(TRACE_INPUT, "aim", "target=%d id=%d dist=%d step=%d anchor=%d t=%.2f",
                    rt_lock_target != NULL ? rt_lock_target->pid : -1,
                    rt_lock_target != NULL ? rt_lock_target->id : -1,
                    rt_lock_target != NULL ? obj_dist(obj_dude, rt_lock_target) : -1,
                    rt_lock_target != NULL && anchor != NULL ? obj_dist(anchor, rt_lock_target) : -1,
                    anchor != NULL ? anchor->id : -1, rt_clock);
            }

            // ⚠️ ON EVERY PUSH, not only on a refusal. The complaint that started this was "I want
            // that one and it picks another" -- a SUCCESSFUL switch onto the wrong target -- and the
            // first version of this diagnostic only ran when nothing had moved, so it could not see
            // the case it was built for. Rate-limited inside.
            rt_lock_diag(rx, ry);
        } else if (rmag < RT_LOCK_CYCLE_THRESHOLD * 0.5f) {
            // The stick is home: the next push starts from whoever is locked NOW. Half the push
            // threshold, so a thumb hovering on the edge cannot re-anchor in the middle of a push.
            rt_lock_push_anchor = NULL;
        }

        // Glue the cursor to the target's centre -- but NOT while it is off screen, or we would be
        // parking the mouse outside the world window during the brief grace before the lock drops.
        if (OnScreen::test(rt_lock_target)) {
            Rect bounds;
            obj_bound(rt_lock_target, &bounds);
            mouse_set_position((bounds.ulx + bounds.lrx) / 2, (bounds.uly + bounds.lry) / 2);
        }

        // Put the game mouse in CROSSHAIR mode so left-click attacks and hostiles
        // get the red combat outline; the cursor is already glued onto the target.
        // Reverted on unlock.
        if (gmouse_3d_get_mode() != GAME_MOUSE_MODE_CROSSHAIR) {
            gmouse_3d_set_mode(GAME_MOUSE_MODE_CROSSHAIR);
            rt_lock_set_crosshair = true;
        }

        // Draw the to-hit % over the target every frame. The engine's own
        // crosshair display only paints after the cursor sits still ~250ms, which
        // never happens here (the glue tracks a moving target / scrolling view),
        // so drive the indicator directly instead.
        //
        // NOT while the body-part picker is up: it owns the target's presentation, draws the
        // per-part chance into its own label, and the crosshair art would sit right on the body
        // it is asking the player to read. (The cursor object is hidden then anyway -- this just
        // stops us rebuilding a frame nobody can see.)
        if (!gamepad_ui_aim_body_active()) {
            gmouse_3d_show_to_hit(rt_lock_target);
        }
    } else {
        // No lock, no push to measure from -- whatever the stick does next starts fresh.
        rt_lock_push_anchor = NULL;

        if (rt_lock_set_crosshair) {
            if (gmouse_3d_get_mode() == GAME_MOUSE_MODE_CROSSHAIR) {
                gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
            }
            rt_lock_set_crosshair = false;
        }
    }

    // A button -> attack. With a lock, A always targets the locked critter and
    // simply doesn't fire if it's out of weapon range (never falls through to a
    // UI click -- see combat_rt_has_lock + the gamepad_ui A arbitration). With no
    // lock, A auto-targets the nearest hostile in range, as before. The attack is
    // suppressed while A is acting as a UI interact (no lock and no hostile in range).
    //
    // HOLD TO KEEP ATTACKING: this reads the HELD state, not the press edge, so an intent is
    // re-recorded every frame and a swing goes off whenever the gates open again. Since the attack
    // cooldown was raised to a deliberately slow, tactical value the pacing is now the COOLDOWN,
    // not AP -- hold A and the dude swings once every rt_attack_cooldown, hands free for
    // positioning and the wheels in between. (The comment here used to say "at the AP cadence";
    // that stopped being true the moment the cooldown became the binding constraint.)
    // ⚠️ ATTACK IS THE RIGHT TRIGGER (2026-09-16 layout), not A. A is action-and-confirm only now,
    // which is what finally ends the interact-vs-attack arbitration this code spent months on: a
    // door next to a raider used to be a coin toss between opening it and shooting him, and the
    // fix was always another guess about which one the player meant. Two buttons, two meanings.
    //
    // Read through the Schmitt helper, never as a bare threshold: a held trigger reports an
    // occasional HARD ZERO, and while a dropped frame costs a held attack nothing (the intent is
    // re-armed the next frame), it would restamp the press edge and spray the trace with phantom
    // attacks -- the same dropped-sample fault that once ate an entire session of aimed shots.
    //
    // HOLD TO KEEP ATTACKING: this reads the HELD state, not the press edge, so an intent is
    // re-recorded every frame and a swing goes off whenever the gates open again. Since the attack
    // cooldown was raised to a deliberately slow, tactical value the pacing is now the COOLDOWN,
    // not AP -- hold RT and the dude swings once every rt_attack_cooldown, hands free for
    // positioning and the wheels in between.
    //
    // ⚠️ NOT WHILE THE AIMED-SHOT PICKER IS UP: there RT means "fire at the part I chose", and
    // gamepad_ui commits that shot itself. Without this gate the same pull would also start an
    // ordinary attack at whatever the auto-target picked.
    bool attack = (gamepad_ui_attack_held() && !gamepad_ui_aim_gesture_active())
        // ...or Ctrl held on the keyboard, which is RT there.
        || rt_kb_mod_held(KMOD_CTRL);
    // Only trace the attack intent on the RT PRESS EDGE, not every held frame -- each actual fire
    // is already logged as [COMBAT/attack], so a per-frame ACTION line would be pure spam.
    static bool rt_prev_attack_btn = false;
    bool attackEdge = attack && !rt_prev_attack_btn;
    rt_prev_attack_btn = attack;
    // ⚠️ THE A-INTERACT GATE IS GONE WITH A'S ATTACK ROLE. It existed to stop one button meaning
    // two things; RT means one thing, and gating it on what A happens to be doing would only
    // reintroduce the coin toss from the other side.
    if (attack) {
        int hitMode = rt_player_hit_mode();
        // AIMED MODE in the weapon slot = this pull goes to the bound part (rt_player_called_loc).
        int loc = rt_player_called_loc(hitMode);
        if (rt_lock_target != NULL) {
            if (obj_dist(obj_dude, rt_lock_target) <= item_w_range(obj_dude, hitMode)) {
                rt_set_player_intent(rt_lock_target, hitMode, loc);
                if (attackEdge) {
                    trace_event(TRACE_ACTION, "attack", "target=%d hm=%d loc=%d locked=1 t=%.2f",
                        rt_lock_target->pid, hitMode, loc, rt_clock);
                }
            }
        } else {
            Object* target = rt_nearest_hostile_in_range(hitMode);
            if (target != NULL) {
                rt_set_player_intent(target, hitMode, loc);
                if (attackEdge) {
                    trace_event(TRACE_ACTION, "attack", "target=%d hm=%d loc=%d locked=0 t=%.2f",
                        target->pid, hitMode, loc, rt_clock);
                }
            }
        }
    }

    // The mouse: aim at what it points at, attack with its left button (keyboard & mouse).
    rt_mouse_update();

    // B button: cancel / no action for now (reserved).
}

bool combat_rt_hostile_in_attack_range()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return false;
    }

    int hitMode = rt_player_hit_mode();
    return rt_nearest_hostile_in_range(hitMode) != NULL;
}

bool combat_rt_has_lock()
{
    return rt_lock_target != NULL;
}

// Is the player currently walking under LEFT-STICK control (as opposed to standing, or walking out
// a click-to-move path)? Used by gmouse to keep the hover hex off the ground while he is already
// moving -- see the guard in gmouse_bk_process.
bool combat_rt_stick_walking()
{
    return rt_stick_active;
}

// The critter an aimed shot would hit: the lock-on target if it's a live combatant,
// otherwise the nearest hostile within current-weapon range. NULL if neither exists.
// Nearest critter in WEAPON RANGE that the player is allowed to shoot at all -- hostile or not.
// Same membership rule as the R3 lock (rt_is_lockable: alive, not you, and either hostile or simply
// on another team and perceivable), but ranged by the weapon rather than by Perception, because
// this answers "can I shoot that" and not "can I see that".
static Object* rt_nearest_aimable_in_range(int hitMode)
{
    int range = item_w_range(obj_dude, hitMode);
    Object* best = NULL;
    int bestDist = range + 1;

    for (int i = 0; i < rt_actor_count; i++) {
        Object* critter = rt_actors[i].obj;
        if (!rt_is_lockable(critter)) {
            continue;
        }

        int dist = obj_dist(obj_dude, critter);
        if (dist <= range && dist < bestDist) {
            bestDist = dist;
            best = critter;
        }
    }

    return best;
}

static Object* rt_aim_target()
{
    if (rt_lock_target != NULL && rt_is_targetable(rt_lock_target)) {
        return rt_lock_target;
    }

    // HOSTILES FIRST, and that ordering is the whole safety of the fallback below. In a fight the
    // answer is unchanged -- the nearest active enemy -- so nothing about combat aiming moves.
    Object* hostile = rt_nearest_hostile_in_range(rt_player_hit_mode());
    if (hostile != NULL) {
        return hostile;
    }

    // ⚠️ AND ONLY THEN ANYONE ELSE. Without this the aim gesture was hostiles-only unless you had
    // first locked the target with R3: `combat_rt_can_aim` ends in `rt_aim_target() != NULL`, so
    // walking up to a peaceful NPC and holding the aim button did nothing at all, with no feedback
    // explaining why. Nothing downstream ever objected -- rt_is_lockable already admits anyone on
    // another team, and neither combat_rt_can_aim nor combat_rt_aimed_attack tests hostility -- so
    // the restriction was an accident of this one fallback, not a rule anybody chose.
    //
    // ⚠️ IT DOES MEAN THE AIM BUTTON CAN OPEN FIRE ON A TOWNSPERSON. A tap fires immediately at
    // the last-used body part (GPUI_AIM_TAP_MS), so in a peaceful map it is now possible to start a
    // fight with one stray press. That is deliberate -- you can always shoot anyone in Fallout, and
    // the alternative is a targeting system that silently refuses on exactly the target the player
    // is pointing at -- but it is a foot-gun, and B cancels the picker if the shot was not intended.
    return rt_nearest_aimable_in_range(rt_player_hit_mode());
}

bool combat_rt_can_aim()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return false;
    }
    // The active weapon/attack must support called shots (same gate the engine and the
    // AI use), and there must be a target to aim at.
    if (!item_w_called_shot(obj_dude, rt_player_hit_mode())) {
        return false;
    }
    return rt_aim_target() != NULL;
}

Object* combat_rt_aim_target()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return NULL;
    }
    return rt_aim_target();
}

// The critter currently green-highlighted for aiming, and the outline state it had before
// (restored on highlight-off so we don't leave a stray outline behind).
static Object* rt_aim_hl_obj = NULL;
static int rt_aim_hl_saved = 0;

void combat_rt_aim_highlight_on()
{
    combat_rt_aim_highlight_off();

    Object* target = rt_aim_target();
    if (target == NULL) {
        return;
    }

    rt_aim_hl_obj = target;
    rt_aim_hl_saved = target->outline; // remember so we can restore the exact prior state

    // FRIENDLY = green (vs the combat HOSTILE red), so the aimed target reads as "selected"
    // -- a Fallout 3 VATS-style highlight on the actual enemy.
    obj_outline_object(target, OUTLINE_TYPE_FRIENDLY, NULL);

    Rect r;
    if (obj_turn_on_outline(target, &r) == 0) {
        tile_refresh_rect(&r, map_elevation);
    } else {
        tile_refresh_display();
    }
}

void combat_rt_aim_highlight_off()
{
    if (rt_aim_hl_obj == NULL) {
        return;
    }

    Object* target = rt_aim_hl_obj;
    rt_aim_hl_obj = NULL;

    // Restore the exact outline the object had before (red combat outline, or none).
    target->outline = rt_aim_hl_saved;

    Rect r;
    obj_bound(target, &r);
    tile_refresh_rect(&r, map_elevation);
}

int combat_rt_aim_chance(int hitLocation)
{
    Object* target = rt_aim_target();
    if (target == NULL) {
        return 0;
    }
    int chance = determine_to_hit(obj_dude, target, hitLocation, rt_player_hit_mode());
    if (chance < 0) {
        chance = 0;
    } else if (chance > 999) {
        chance = 999;
    }
    return chance;
}

// See the header. Ordered so the most specific answer wins: a weapon that can never be aimed is
// worth saying before "no target", because the player can act on it.
const char* combat_rt_aim_refusal()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return "You can't aim right now.";
    }

    int hitMode = rt_player_hit_mode();

    if (!item_w_called_shot(obj_dude, hitMode)) {
        // ⚠️ THREE VERY DIFFERENT CAUSES LAND HERE and the player can fix two of them, so do not
        // collapse them into one message. Fast Shot is permanent (a character trait), the firing
        // MODE is one tap of X away, and the damage type is a matter of picking another weapon.
        if (trait_level(TRAIT_FAST_SHOT)) {
            return "Fast Shot: you can never make a called shot.";
        }

        int anim = item_w_anim(obj_dude, hitMode);
        if (anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS) {
            return "You can't aim a burst. Switch firing mode with LT+Y.";
        }

        return "This weapon can't be aimed.";
    }

    if (rt_aim_target() == NULL) {
        return "Nothing in range to aim at.";
    }

    return NULL;
}

// The hit mode the player's next attack would use (which hand, single or burst). Exported for the
// vanilla called-shot window: the to-hit percentage it prints beside every body part is computed
// per hit mode, so a burst and a single shot must not show the same number.
int combat_rt_player_hit_mode()
{
    return rt_player_hit_mode();
}

bool combat_rt_aimed_attack(int hitLocation)
{
    if (hitLocation < 0 || hitLocation >= HIT_LOCATION_SPECIFIC_COUNT) {
        return false;
    }
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return false;
    }
    int hitMode = rt_player_hit_mode();
    if (!item_w_called_shot(obj_dude, hitMode)) {
        return false;
    }
    Object* target = rt_aim_target();
    if (target == NULL) {
        return false;
    }
    // Latch the intent with the chosen body part; the arbiter fires it (paying the
    // aimed AP cost) at the next eligible frame, exactly like a normal attack.
    rt_set_player_intent(target, hitMode, hitLocation);
    return true;
}

// Scans for the nearest object adjacent to the dude (hex distance <= 1, same
// elevation) that can be looted: a corpse (dead critter) or a container item. If
// one is found it is stashed for combat_rt_update to open, and true is returned so
// the gamepad layer routes tap-A to looting instead of a walk-click. Detection
// only -- the loot window must open from the main loop, never the input hook.
// Same scan with NO side effect: is there anything adjacent worth looting right now? Split out so
// the pad can ask the question at the moment A goes down without queuing anything -- see
// combat_rt_world_loot_available below.
static Object* rt_find_world_loot(int* kindOut)
{
    if (obj_dude == NULL) {
        return NULL;
    }

    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (obj == obj_dude || obj_dist(obj_dude, obj) > 1) {
            continue;
        }
        // Not drawn, not lootable. Same trap as the invisible caravan (see ip_classify): staged
        // content is a real object until a script reveals it, and obj_find_first_at hands it over.
        if ((obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }

        int fidType = FID_TYPE(obj->fid);
        if (fidType == OBJ_TYPE_CRITTER && (obj->data.critter.combat.results & DAM_DEAD) != 0) {
            *kindOut = 1;
            return obj;
        }
        if (fidType == OBJ_TYPE_ITEM && item_get_type(obj) == ITEM_TYPE_CONTAINER) {
            *kindOut = 2;
            return obj;
        }
    }

    return NULL;
}

// Is the dude mid-shooting? True while his own rate-of-fire cooldown is still running, i.e. a
// swing or shot went out moments ago. The pad uses it to tell a DELIBERATE double-tap from plain
// mashing at an enemy: if he is on cooldown he is attacking, and the taps mean attack.
bool combat_rt_player_attack_busy()
{
    return obj_dude != NULL && rt_attack_on_cooldown(obj_dude);
}

// True if something adjacent could be looted this instant. Asks, never queues.
bool combat_rt_world_loot_available()
{
    int kind = 0;
    return rt_find_world_loot(&kind) != NULL;
}

bool combat_rt_try_world_loot()
{
    if (obj_dude == NULL) {
        return false;
    }

    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (obj == obj_dude || obj_dist(obj_dude, obj) > 1) {
            continue;
        }
        // Not drawn, not lootable. Same trap as the invisible caravan (see ip_classify): staged content is a
        // real object until a script reveals it, and obj_find_first_at hands it over -- obj_create_list,
        // which the combat registry uses, would not.
        if ((obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }


        int fidType = FID_TYPE(obj->fid);
        if (fidType == OBJ_TYPE_CRITTER && (obj->data.critter.combat.results & DAM_DEAD) != 0) {
            rt_pending_loot = obj;
            rt_pending_loot_kind = 1;
            return true;
        }
        if (fidType == OBJ_TYPE_ITEM && item_get_type(obj) == ITEM_TYPE_CONTAINER) {
            rt_pending_loot = obj;
            rt_pending_loot_kind = 2;
            return true;
        }
    }

    return false;
}

// Smooth follow-cam. An object is drawn at tile_coord(tile) + (obj->x, obj->y),
// where obj->x/y is the sub-tile walk offset that grows across a tile and resets
// on each crossing (object.cc obj_render_object). Centering on the dude's tile
// puts tile_coord(tile) at the window center, so shifting the view by the
// negative of that walk offset pins the dude exactly to center every frame --
// the world scrolls under him pixel-for-pixel, with no per-tile jump and nothing
// to ease (the old residual-easing approach ignored obj->x/y and so oscillated
// against it, which was the jitter). The crossing is seamless because
// center(A) + (-tileVector) and center(B) + 0 render identically.
// Advance the camera's lag by this frame's dude motion and let it decay. Returns false when the
// camera should simply snap (smoothing off, or we have no previous frame to measure against).
//
// ⚠️ MEASURED AS A DELTA, NEVER AS AN ABSOLUTE POSITION. The dude's absolute pixel position is
// tile_coord(tile) + obj->x/y, and tile_coord depends on tile_offx -- which is the very thing this
// camera sets, so reading it back would be circular. Both tile_coord calls below happen in the SAME
// frame and therefore carry the SAME tile_offx, which cancels in the subtraction. What is left is
// the true map-space movement.
//
// ⚠️ AND THIS IS WHY THE OLD EASING FAILED. It smoothed the TILE while obj->x/y went on snapping
// underneath it -- two systems pulling against each other, which is the oscillation the comment on
// this function has warned about ever since. Tile motion and sub-tile motion are ONE quantity here,
// summed before anything is smoothed.
static bool rt_cam_advance_lag(float dt)
{
    if (rt_cam_smooth_k <= 0.0f || dt <= 0.0f) {
        return false;
    }

    if (!rt_cam_have_prev || rt_cam_last_tile < 0) {
        rt_cam_prev_x = obj_dude->x;
        rt_cam_prev_y = obj_dude->y;
        rt_cam_have_prev = true;
        rt_cam_lag_x = 0.0f;
        rt_cam_lag_y = 0.0f;
        return false; // nothing to measure against yet -- snap this frame
    }

    int nx, ny, ox, oy;
    if (tile_coord(obj_dude->tile, &nx, &ny, obj_dude->elevation) != 0
        || tile_coord(rt_cam_last_tile, &ox, &oy, obj_dude->elevation) != 0) {
        // Off-screen or invalid (a teleport, a fresh map): no meaningful delta, so snap rather
        // than smear the camera across half the map.
        rt_cam_lag_x = 0.0f;
        rt_cam_lag_y = 0.0f;
        rt_cam_prev_x = obj_dude->x;
        rt_cam_prev_y = obj_dude->y;
        return false;
    }

    float dx = (float)((nx - ox) + (obj_dude->x - rt_cam_prev_x));
    float dy = (float)((ny - oy) + (obj_dude->y - rt_cam_prev_y));
    rt_cam_prev_x = obj_dude->x;
    rt_cam_prev_y = obj_dude->y;

    // A jump this big is not walking -- a map change, an elevator, a script teleport. Smoothing it
    // would drag the camera across the screen for a second; snap instead.
    if (dx > 200.0f || dx < -200.0f || dy > 200.0f || dy < -200.0f) {
        rt_cam_lag_x = 0.0f;
        rt_cam_lag_y = 0.0f;
        return false;
    }

    // E_new = (how far he moved) + E * (1 - alpha). Straight out of C += (D - C) * alpha, written
    // in terms of the error so no absolute position is ever needed.
    float alpha = 1.0f - expf(-rt_cam_smooth_k * dt);

    // ⚠️ AN ANIMATION THAT IS NOT LOCOMOTION MOVES THE BODY WITHOUT MOVING THE MAN. A shot is
    // POINT -> FIRE -> UNPOINT and every one of those poses sits at its own hot spot, so obj->x
    // wanders ~15 px over the shot; a punch or a fall does the same. The camera is `-x + lag`, so
    // with the decay running it CHASES that wander one tenth of a second behind and the whole world
    // shivers for the length of the animation -- "druhý výstřel z pušky je trhaný".
    //
    // Freezing the DECAY (not the position -- freezing the position breaks the cancellation and
    // snaps twice, which is what the first attempt at this did) leaves lag += dx with nothing taken
    // out, so `-x + lag` is algebraically CONSTANT for as long as the animation runs: the body
    // lunges, recoils and settles against a world that does not move at all. The instant the art is
    // ours again the decay resumes and the camera eases onto wherever the body ended up. Nothing is
    // discontinuous at either end -- only the rate changes -- and the existing clamp still bounds
    // how far he may drift off centre.
    int camAnim = FID_ANIM_TYPE(obj_dude->fid);
    if (camAnim != ANIM_STAND && camAnim != ANIM_WALK && camAnim != ANIM_RUNNING) {
        alpha = 0.0f;
    }
    rt_cam_lag_x = dx + rt_cam_lag_x * (1.0f - alpha);
    rt_cam_lag_y = dy + rt_cam_lag_y * (1.0f - alpha);

    // Bound the drift so a long sprint cannot walk him toward the screen edge. Steady-state lag is
    // velocity / k, and a sprint is ~300 px/s, so without this he would sit a whole tile back.
    if (rt_cam_lag_x > RT_CAM_SMOOTH_MAX) rt_cam_lag_x = RT_CAM_SMOOTH_MAX;
    if (rt_cam_lag_x < -RT_CAM_SMOOTH_MAX) rt_cam_lag_x = -RT_CAM_SMOOTH_MAX;
    if (rt_cam_lag_y > RT_CAM_SMOOTH_MAX) rt_cam_lag_y = RT_CAM_SMOOTH_MAX;
    if (rt_cam_lag_y < -RT_CAM_SMOOTH_MAX) rt_cam_lag_y = -RT_CAM_SMOOTH_MAX;

    // ⚠️ SETTLE EXACTLY, DO NOT CREEP. An exponential never actually reaches zero, and a camera
    // that is still moving by a hundredth of a pixel re-rounds to the same integer while never
    // comparing equal -- which would cost a FULL-SCREEN REDRAW every frame for ever, because the
    // skip below is what keeps a standing dude from repainting the map. Under a pixel, stop.
    if (rt_cam_lag_x < RT_CAM_SMOOTH_SNAP && rt_cam_lag_x > -RT_CAM_SMOOTH_SNAP) {
        rt_cam_lag_x = 0.0f;
    }
    if (rt_cam_lag_y < RT_CAM_SMOOTH_SNAP && rt_cam_lag_y > -RT_CAM_SMOOTH_SNAP) {
        rt_cam_lag_y = 0.0f;
    }

    return true;
}

// Eases rt_cam_lean_x/y toward how far the view must move for the locked target to be on screen.
static void rt_cam_advance_lean(float dt)
{
    float wantX = 0.0f;
    float wantY = 0.0f;
    bool fits = false;
    rt_cam_lean_want(rt_lock_target, &wantX, &wantY, &fits);

    if (dt <= 0.0f) {
        return;
    }
    float alpha = 1.0f - expf(-RT_CAM_LEAN_K * dt);
    rt_cam_lean_x += (wantX - rt_cam_lean_x) * alpha;
    rt_cam_lean_y += (wantY - rt_cam_lean_y) * alpha;

    // Settle exactly, or the last fraction of a pixel would keep the view redrawing for ever.
    if (fabsf(wantX - rt_cam_lean_x) < 0.5f) {
        rt_cam_lean_x = wantX;
    }
    if (fabsf(wantY - rt_cam_lean_y) < 0.5f) {
        rt_cam_lean_y = wantY;
    }
}

void combat_rt_set_cam_lean(bool on)
{
    rt_cam_lean_on = on;
}

bool combat_rt_get_cam_lean()
{
    return rt_cam_lean_on;
}

static void rt_follow_cam(float dt)
{
    // The lag has to be advanced BEFORE the offsets are built, and before rt_cam_last_tile is
    // overwritten below -- it is measured against that tile.
    bool smoothed = rt_cam_advance_lag(dt);
    rt_cam_advance_lean(dt);

    // ⚠️ DO NOT "HOLD" THE BODY POSITION HERE. The camera is the dude's sub-tile offset negated, so
    // freezing that offset while an attack plays LOOKS like the right way to stop the screen lurching
    // with the swing -- and it is exactly backwards, because rt_cam_advance_lag above already cancels
    // the body's movement: it adds this frame's delta to the lag, so `-x + lag` stays put and then
    // eases to the new position. Holding x while the lag keeps tracking the LIVE body breaks that
    // cancellation and turns every attack into two hard jumps, one when the hold starts and one when
    // it ends. Measured, trace_2026-09-24_211536: a rifle shot logged `cam-jump dx=-14 held=1` at the
    // shot and `dx=+14 held=0` when the hold expired 2 s later -- "druhý výstřel z pušky je trhaný".
    //
    // Adding L to the offset shifts the whole drawing origin by L, so the dude renders L pixels
    // further along -- i.e. he sits AHEAD of the camera centre by exactly the lag, in the direction
    // he is travelling, and eases back to dead centre when he stops. That is the entire feature.
    int offX = -obj_dude->x + (smoothed ? (int)lroundf(rt_cam_lag_x) : 0);
    int offY = -obj_dude->y + (smoothed ? (int)lroundf(rt_cam_lag_y) : 0);

    // The lean moves the VIEW toward the target, i.e. the drawing origin the other way.
    offX -= (int)lroundf(rt_cam_lean_x);
    offY -= (int)lroundf(rt_cam_lean_y);

    // DIAG (the "jumpy / big distance" hunt, 2026-09-24): the world moving by more than a few
    // pixels in ONE frame WITHOUT a tile change is the symptom itself, measured rather than
    // guessed at -- and the line says what the body was doing when it happened, so a swing, a
    // knockdown and a walk-cut are told apart instead of argued about. Strip with `move-diag`.
    if (trace_enabled() && obj_dude->tile == rt_cam_last_tile) {
        int jx = offX - rt_cam_last_off_x;
        int jy = offY - rt_cam_last_off_y;
        if (abs(jx) + abs(jy) >= RT_CAM_JUMP_PX) {
            trace_event(TRACE_FLOW, "cam-jump", "dx=%d dy=%d anim=%d x=%d y=%d held=%d t=%.2f",
                jx, jy, FID_ANIM_TYPE(obj_dude->fid), obj_dude->x, obj_dude->y,
                rt_swing_keep_live() ? 1 : 0, rt_clock);
        }
    }

    // Only force a full redraw when the framing actually changed; while the dude
    // stands or plays a non-moving animation (attack), obj->x/y stay put and we
    // skip the redraw entirely.
    if (obj_dude->tile == rt_cam_last_tile
        && offX == rt_cam_last_off_x
        && offY == rt_cam_last_off_y) {
        return;
    }
    rt_cam_last_tile = obj_dude->tile;
    rt_cam_last_off_x = offX;
    rt_cam_last_off_y = offY;

    // No IGNORE_SCROLL_RESTRICTIONS, so the camera honors the map edge like
    // normal walking (it simply stops following at the boundary).
    tile_set_center_offset(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW, offX, offY);
}

// Live setter for the MODS screen + its reader. The cfg holds the time constant so it stays
// tunable without a rebuild; the screen only offers on/off.
void combat_rt_set_cam_smooth(float k)
{
    rt_cam_smooth_k = k < 0.0f ? 0.0f : (k > 40.0f ? 40.0f : k);
    if (rt_cam_smooth_k <= 0.0f) {
        rt_cam_lag_x = 0.0f;
        rt_cam_lag_y = 0.0f;
    }
}

float combat_rt_get_cam_smooth()
{
    return rt_cam_smooth_k;
}

float combat_rt_time_scale()
{
    if (!rt_enabled) {
        return 1.0f;
    }

    // The Combat Speed slider: the whole fight runs this much faster (see rt_game_speed). Only
    // while one is on -- exploring and talking keep their normal pace.
    float scale = rt_engaged ? rt_game_speed : 1.0f;

    // Floored at a small positive value rather than allowed to reach 0. A zero scale would stop
    // rt_clock dead while the picker is up, which is the full freeze this feature deliberately is
    // not -- and it would divide by zero in compute_tpf's caller.
    if (gamepad_ui_aim_body_active()) {
        scale *= rt_aim_slowmo < 0.05f ? 0.05f : rt_aim_slowmo;
    }
    return scale;
}

void combat_rt_update(float dt)
{
    if (obj_dude == NULL) {
        return;
    }

    // A RADIAL WHEEL FREEZES COMBAT. Picking a stimpak, a weapon or an ammo type is a decision, not
    // an action -- in a pausable tactical game you make it with the world held still. Returning
    // BEFORE rt_clock advances is what makes the freeze total: cooldowns, AP regen, initiative and
    // every in-flight commit are all keyed off rt_clock, so none of them tick while you choose.
    // (gamepad_ui additionally suspends the engine's own background processes, which is what stops
    // the animations; this half stops the combat model.)
    if (radial_wheel_is_open()) {
        return;
    }

    // ...but the LIMB PICKER only SLOWS it. Scaling dt here rather than returning is what makes it
    // a slow-motion: rt_clock, AP regen, the movement drains, attack cooldowns, initiative and the
    // in-flight commit timers are all keyed off this one number, so every one of them stretches by
    // the same factor and nothing goes out of step with anything else. The Combat Speed slider
    // rides the same number the other way (rt_game_speed), so it is sampled first.
    rt_sample_speed_mult();
    dt *= combat_rt_time_scale();

    rt_clock += dt;

    // Step the dude's HP counter. Deliberately here, below the radial-wheel return and above
    // every other early-out: the counter is part of the combat model's presentation, so it
    // holds still exactly when the world does.
    intface_hp_roll_tick(dt);

    // ...and so are the combat curtains on the bar's end-turn box: open while a fight is live,
    // shut once it is over (the held D-pad right, or nobody left to fight). See intface.cc.
    intface_rt_combat_box_tick(rt_engaged, dt);

    // Conversation just closed -> hold the NPCs for a beat (see rt_npc_hold_until). Detected as a
    // true->false edge on dialog_active() rather than hooked into gdialog, so it covers every way a
    // talk can end -- a reply, a script closing it, walking out of one.
    {
        static bool wasInDialog = false;
        bool inDialog = dialog_active();
        if (wasInDialog && !inDialog) {
            if (rt_post_dialog_grace > 0.0f) {
                rt_npc_hold_until = rt_clock + rt_post_dialog_grace;
            }

            // "Give me some room", asked for during the conversation that just ended. This is the
            // first frame in which the world is ticking again, which is what the order was waiting
            // for. Consumed unconditionally -- a refusal (nowhere to go) must not leave the request
            // standing to fire after some later, unrelated conversation.
            if (rt_step_back_pending != NULL) {
                Object* who = rt_step_back_pending;
                rt_step_back_pending = NULL;
                rt_do_step_back(who);
            }
        }
        wasInDialog = inDialog;
    }

    // Mid-combat map exit: with the dude on an exit grid it ends combat this frame so the queued
    // transition fires (immediately, as in vanilla). Runs before the commit/registry work so the
    // teardown is clean.
    rt_update_map_exit(dt);

    // Escape granted: combat was torn down this frame for the queued map transition. Bail out
    // BEFORE the registry refresh / engagement recompute below -- otherwise the hostiles still
    // on this map instantly re-register and re-set COMBAT_STATE_0x01, so map_check_state (run
    // later this same frame, gated on !isInCombat()) never sees combat as ended and the
    // transition is missed ("map-exit-go" logs, but the map never changes).
    // Hold combat torn down until the destination map actually loads
    // (combat_rt_notify_objects_freed clears the flag from obj_remove_all).
    if (rt_map_exit_committed) {
        combat_state &= ~COMBAT_STATE_0x01;
        return;
    }

    // (The Combat Speed slider is sampled at the top of this function now, before dt is scaled.)

    // Age the movement-input timer; movement handlers reset it to 0 this frame
    // (keyboard via handle_input before this tick, stick via poll_gamepad below).
    rt_move_input_age += dt;

    // Track the dude's own death/gib/knockdown for the watchdog (he isn't in the
    // actor registry).
    rt_note_result_change(obj_dude, rt_dude_last_results, obj_dude->data.critter.combat.results);
    rt_dude_last_results = obj_dude->data.critter.combat.results;

    // Anti-stunlock for the dude (symmetric with NPCs): accumulate/decay his continuous-
    // flinch timer + open his immunity window, so heavy enemy focus-fire can't flinch-lock
    // him to death without a chance to act. Endurance scales how fast he shrugs it off.
    rt_update_flinch_state(obj_dude, dt);

    rt_frame_dt = dt;

    // Put back any sub-tile offset an attack's registration or its end-of-sequence re-centre threw
    // away -- BEFORE the movement handlers below read obj->x/y. See rt_swing_keep_tick.
    rt_swing_keep_tick();

    // ...and put a body back on its feet when an animation left it standing in a pose nothing owns
    // any more (a ladder, a flight of stairs). See rt_dude_pose_reconcile.
    rt_dude_pose_reconcile(dt);

    combat_rt_poll_gamepad();

    // Open a queued tap-A world-loot target here (main-loop context). These just
    // register an approach animation + open the loot window via callback -- safe
    // from here, but NOT from the input hook that queued it (modal re-entrancy).
    if (rt_pending_loot != NULL) {
        Object* target = rt_pending_loot;
        int kind = rt_pending_loot_kind;
        rt_pending_loot = NULL;
        rt_pending_loot_kind = 0;
        if (kind == 1) {
            action_loot_container(obj_dude, target);
        } else if (kind == 2) {
            // A container ITEM opens via action_get_an_object -> obj_use_container. action_use_an_-
            // object -> obj_use is scenery-only (returns -1 for an item), so containers never opened.
            action_get_an_object(obj_dude, target);
        }
    }

    if (rt_enabled) {
        rt_follow_cam(dt);
    }

    rt_refresh_timer -= dt;
    if (rt_refresh_timer <= 0.0f || rt_actor_count == 0) {
        rt_refresh_timer = RT_REFRESH_INTERVAL;
        rt_refresh_registry();
    }

    // Adopt any ORPHAN engine-pending attack into the in-flight list. rt_do_attack snapshots and
    // frees the global the same call, so if combat_cleanup_enabled is still set at the top of this
    // pass, a path that bypassed rt_do_attack set it -- a scripted critter attack
    // (op_attack -> scripts_request_combat -> combat() -> combat_attack). combat_attack already
    // registered its animation (incl. a death/bloody-mess anim on a lethal blow); snapshot it so
    // it commits like the rest instead of hanging until the watchdog dropped it (which left a
    // "dead" critter alive -- death anim played, DAM_DEAD never set).
    if (combat_is_attack_pending() && rt_inflight_count < RT_MAX_INFLIGHT) {
        RtInFlight* f = &rt_inflight[rt_inflight_count++];
        combat_take_pending_snapshot(&f->ctd, &f->callDisplay);
        f->attacker = f->ctd.attacker;
        f->defender = f->ctd.defender;
        f->started = rt_clock;
        f->playerApCost = 0; // scripted/orphan attack -- not the player's AP-tracked swing
        f->tAnimStart = -1.0f;
        f->tAnimEnd = -1.0f;
        f->projectile = action_take_explosive_projectile(&f->explosionFid);
        combat_clear_pending_attack();
        trace_event(TRACE_COMBAT, "inflight-adopt", "t=%.2f", rt_clock);
    }

    // Commit each in-flight attack at SWING-CONNECT (its defender begins a damage reaction), when
    // the attacker's swing animation finishes, or at the hard cap -- whichever is first. Each
    // commits from its OWN snapshot via combat_commit_snapshot (same apply_damage / display /
    // whoHitMe-guard path), so MANY attacks resolve concurrently: different attacker->defender
    // pairs land in the same window, instead of the single global main_ctd serializing them
    // one-at-a-time. The one-reaction-per-defender invariant (the anim-sequence / death-crash
    // protection) is preserved by the arbiter refusing to start a second attack on a defender
    // that's already in flight or reacting -- so a critter is never asked to react twice at once.
    // "connected" uses rt_is_reacting (the reaction anim TYPE), NOT raw anim_busy, so a merely
    // walking/running defender doesn't read as hit (which would commit before the swing played).
    for (int i = 0; i < rt_inflight_count;) {
        RtInFlight* f = &rt_inflight[i];

        // ⚠️ A DEAD ATTACKER'S BLOW NEVER LANDS. The snapshot is taken when the swing STARTS and
        // commits up to a second and a half later, so a critter that dies in between would still
        // deal its damage from beyond the grave -- "a rat hit me after it died", and he is right
        // that it should not. Turn-based could never produce this (you cannot act on a turn you
        // did not live to take), so there is no vanilla behaviour to preserve here.
        //
        // Checked at the TOP, before any commit condition, so it holds for every trigger --
        // action frame, swing-done, connect and the hard cap alike. Applies to the dude too: if
        // he dies mid-swing, his blow dies with him.
        if (f->attacker != NULL && critter_is_dead(f->attacker)) {
            trace_event(TRACE_COMBAT, "attack-voided",
                "%s pid=%d ->pid=%d why=attacker-dead elapsed=%.2f t=%.2f",
                f->attacker == obj_dude ? "DUDE" : "NPC", f->attacker->pid,
                f->defender != NULL ? f->defender->pid : -1, rt_clock - f->started, rt_clock);
            rt_inflight[i] = rt_inflight[--rt_inflight_count];
            continue;
        }

        float elapsed = rt_clock - f->started;
        bool swingDone = anim_busy(f->attacker) == 0;
        bool connected = rt_is_reacting(f->defender);

        // Latch the edges of the attacker's swing pose. poseEnded below reads them to commit a
        // THROWN weapon once the throw itself is over. Edge-latched: two float compares a frame.
        if (f->attacker != NULL) {
            bool inAttackPose = rt_is_attack_anim(FID_ANIM_TYPE(f->attacker->fid));
            if (inAttackPose) {
                if (f->tAnimStart < 0.0f) {
                    f->tAnimStart = elapsed;
                }
            } else if (f->tAnimStart >= 0.0f && f->tAnimEnd < 0.0f) {
                // Left the swing pose: from here on the sequence is the recoil/UNPOINT tail and,
                // for a gun or a thrown weapon, the projectile still travelling.
                f->tAnimEnd = elapsed;
            }
        }
        // Reached the frame the weapon actually fires / the blow lands -- normally the EARLIEST of
        // the three, so the defender's reaction syncs to the visible shot instead of waiting for the
        // whole fire anim (raise + recoil + lower) to finish. That trailing recoil was the delay the
        // player felt between firing and the enemy reacting.
        //
        // ONLY for a LIGHT hit.
        //
        // ⚠️ NOTE (2026-08-09): the ORIGINAL reason for this exclusion is now STALE. It was: a
        // lethal/knockdown/fire hit registered its full reaction at FIRE time via
        // show_damage_to_object, queued after the attacker's remaining swing frames, so at the
        // action frame that fall hadn't STARTED -- committing there set DAM_DEAD on an upright,
        // un-animating body and the death reap flattened it with no fall ("they drop like flies").
        // Since the reaction moved to the commit point (action_defender_reaction, below), the fall
        // now starts exactly WHEN we commit, so that failure mode is gone and heavy reactions could
        // commit on the action frame too. Deliberately NOT changed in the same pass as the move --
        // it is a separate feel change (deaths would land earlier in the swing) and wants its own
        // trace check. Keeping swing-done here is safe: the attacker's sequence no longer carries
        // the death, so he is free either way.
        bool heavyReaction = (f->ctd.defenderFlags & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_ON_FIRE)) != 0;
        // Applies to EVERY hit now, heavy ones included. Heavy reactions used to be excluded: their
        // fall was registered at FIRE time, queued after the attacker's remaining swing frames, so
        // at the action frame it had not STARTED -- committing there set DAM_DEAD on an upright,
        // un-animating body and the death reap flattened it with no fall ("they drop like flies").
        // Moving the reaction to the commit point (action_defender_reaction below) removed that:
        // the fall now begins exactly WHEN we commit.
        //
        // Keeping the exclusion after that move was a bad bug. `connected` means "the defender
        // began reacting", which for a heavy hit is now registered BY the commit itself -- so it
        // could never become true; with actionReached excluded as well, a LETHAL blow satisfied NO
        // commit condition and only landed on the 1.5 s hard cap. Traced: killing blow at s=17.53,
        // then `resolve-capped elapsed=1.51 done=0 conn=0 action=0` at s=19.05, with the rat
        // getting four more attacks in before it finally died.
        // ⚠️ MELEE ONLY (2026-08-10). For a gun or a thrown weapon the action frame is when the
        // weapon DISCHARGES, not when the target is struck -- the projectile still has to fly, and
        // that flight is registered into this same sequence, so `swingDone` is the arrival. Using
        // the action frame there made the defender flinch mid-flight ("the spear reacted just
        // before it landed") and, together with the stand-art bug fixed in
        // rt_attack_action_frame_reached, made a rifle's hit land before the shot was visible.
        // ⚠️ THROWN vs SHOT is the line that matters here, not "has a projectile" (2026-08-10).
        // Both put something in the air, but only one of them is something you can WATCH fly:
        //   * A GUN's bullet crosses the gap in 0-6 path steps (measured) -- invisible.
        //     Its bang IS the hit, so the commit belongs on the ACTION FRAME, where the shot fires
        //     and the weapon SFX plays. Waiting past it puts ~0.4 s between the bang and the target
        //     flinching, which is exactly the "I pictured the hit sound and the hit together" the
        //     player asked for.
        //   * A THROWN spear or grenade is watched all the way across. Committing at the throw's
        //     action frame made the target react in mid-air -- his original complaint. Those keep
        //     waiting for the firing pose to end.
        int attackType = rt_attack_type(&f->ctd);
        bool thrown = attackType == ATTACK_TYPE_THROW;
        bool projectileAttack = thrown || attackType == ATTACK_TYPE_RANGED;

        // ⚠️ AN EXPLOSIVE PROJECTILE COMMITS WHEN IT GOES OFF (2026-10-04, "zabil jsem shluk
        // raketometem... bylo to takové useklé, nebyla vidět raketa ani exploze"). The gun rule
        // below -- "the bang IS the hit, commit on the action frame" -- holds for a bullet nobody
        // can see fly, and is exactly wrong for a rocket: the trace had the whole cluster dying and
        // snapping flat 0.06 s after launch while the rocket was still in the air, and the blast
        // landing half a second later on a pile of corpses. `landed` is the moment action_ranged's
        // own sequence swaps the projectile for its explosion art (or the projectile is gone);
        // nothing else commits such an attack except the (longer) cap. The splash victims' falls
        // are registered at that same commit (action_extras_reaction), so they go down WITH the blast.
        bool explosive = f->explosionFid != -1;
        bool landed = explosive && (f->projectile == NULL || f->projectile->fid == f->explosionFid);

        bool actionReached = !thrown && !explosive && rt_attack_action_frame_reached(f->attacker);

        // For a projectile attack, commit when the FIRING POSE ENDS rather than when the whole
        // sequence does. The sequence also carries the projectile's flight and, since 2026-08-10,
        // the ANIM_UNPOINT tail that stops the shot ending in a visible snap -- waiting for all of
        // it would hand the unpoint's whole length back as damage delay, undoing the win from
        // dropping ANIM_POINT. Leaving the fire pose is the end of the VISIBLE shot, which is the
        // sync point the eye actually uses, and it keeps vanilla's ordering (damage before the
        // weapon is lowered, never after). Measured flight is 0-6 steps, so the
        // remaining error is small and the cap still bounds everything.
        // Needs tAnimStart to have been seen, or a shot whose animation never registered would
        // read as "pose already over" on frame one.
        bool poseEnded = thrown && f->tAnimStart >= 0.0f && f->tAnimEnd >= 0.0f;

        // A rocket can fly further than a swing lasts, so its safety cap is longer.
        bool capped = elapsed >= (explosive ? RT_RESOLVE_MAX_SECONDS * 2.0f : RT_RESOLVE_MAX_SECONDS);
        // Don't let action-frame / swing-done / connect commit before the swing has had time to play
        // (and its SFX to sound) -- that produced "shadow attacks" (damage + AP, no visible swing)
        // when the defender was already reacting or the swing read as done instantly. Cap still wins.
        bool ready = elapsed >= RT_MIN_SWING_SECONDS;
        bool trigger = explosive ? landed : (swingDone || connected || actionReached || poseEnded);
        if ((trigger && ready) || capped) {
            // Settle lethality against the defender's hp as it is NOW, before anything downstream
            // reads the flags. Fire-time lethality is measured against the hp the defender had when
            // the trigger was pulled, so under focus-fire the blow that lands first can be one that
            // measured "survivable" against hp a sibling attack has since eaten. If we register the
            // reaction from those stale flags the victim gets a light stagger instead of a death
            // fall, and the commit two lines down kills it with nothing queued -- the corpse reap
            // then snaps it flat in the same frame ("he died without falling"; traced on Ian
            // 2026-08-10 under three attackers). heavyReaction has to be re-read after this, since
            // it is what selects the fall AND shields it from the stray-anim tests.
            if (combat_late_lethal_upgrade(&f->ctd)) {
                heavyReaction = true;
            }

            // Capture before the commit + slot-compaction below invalidate f, so the
            // decoupled hit-react (E) can fire from the right attacker/defender/result.
            Object* rAttacker = f->ctd.attacker;
            Object* rDefender = f->ctd.defender;
            Object* rWeapon = f->ctd.weapon;
            int rFlags = f->ctd.defenderFlags;
            int rDmg = f->ctd.defenderDamage;
            int rPlayerApCost = f->playerApCost;

            // Charge the player's deferred attack AP now the swing has connected/finished
            // (~half-anim in), not at button press. The dude couldn't act mid-swing, so the pool
            // only grew (regen) since fire and this never drives it negative.
            if (rPlayerApCost > 0) {
                rt_player_ap -= rPlayerApCost;
            }

            // Register the defender's reaction (its fall / knockdown / death + the corpse settle)
            // as the DEFENDER's OWN sequence, HERE -- at the action-frame synchronised commit --
            // instead of inside the attacker's sequence at fire time. That is what frees the
            // attacker: his sequence now holds only his own swing, so it ends when the swing does
            // and he can move immediately, instead of being pinned for the whole of his victim's
            // death fall ("I killed the rat and have to wait for it to bleed out").
            //
            // BEFORE combat_commit_snapshot, matching vanilla's order: show_damage runs while the
            // defender still has its pre-hit state (show_damage_to_object's prone branch tests
            // results & DAM_DEAD, which the commit is about to set).
            //
            // Latest-wins, but ONLY for a heavy reaction: a critter mid-walk cannot register its
            // own death otherwise (check_registry refuses) and the death reap would then flatten
            // it with no fall at all. A LIGHT hit is deliberately left alone here -- its stagger
            // is rt_play_hit_react's job further down, and that already does its own latest-wins
            // clear; clearing twice would only fight it.
            // ASK THE REACTION LAYER, do not just take the body. This used to force-clear the
            // defender unconditionally, which is how a second knockdown landing 20 ms after the
            // first cut the fall it had just started and left the body frozen mid-topple (see
            // the REACTION OWNERSHIP block). rt_reaction_heavy applies the transition rule and
            // clears ONLY when the new reaction actually supersedes the current one; a refusal is
            // traced. action_defender_reaction still runs either way -- it carries the weapon
            // drop/destroy callbacks and the show_death settle, not just the animation, and on a
            // refused transition its own prone branch correctly registers no second fall.
            if (heavyReaction && (f->ctd.attackerFlags & DAM_HIT) != 0) {
                rt_reaction_heavy(f->ctd.defender, f->ctd.defenderFlags);
            }
            // Called for EVERY hit, not just heavy ones: with the split on, the whole defender
            // block is skipped at fire time, so this is also what registers the weapon
            // drop/destroy callbacks and the show_death corpse settle.
            bool reactionRegistered = action_defender_reaction(&f->ctd);

            // ...and every critter the blow caught beside it (a burst's cone, a blast's ring), on
            // the same beat. They used to be registered at FIRE time, before anything was decided,
            // and against bodies that were often busy -- so they never took, and the corpse reap
            // snapped the victims flat with no fall at all.
            action_extras_reaction(&f->ctd);


            // TEMP DIAG: a heavy reaction that fails to register means the defender is still owned
            // by a live sequence (check_registry refused) and the salvage below will settle it to a
            // static corpse with NO fall. That is the "skipped its death animation" report, so it
            // must be visible. STRIP with the other move diags once this stays quiet.
            if (heavyReaction && !reactionRegistered && (f->ctd.attackerFlags & DAM_HIT) != 0
                && f->ctd.defender != NULL) {
                trace_event(TRACE_WARN, "react-fail", "pid=%d busy=%d anim=%d prone=%d t=%.2f",
                    f->ctd.defender->pid, anim_busy(f->ctd.defender),
                    FID_ANIM_TYPE(f->ctd.defender->fid),
                    critter_is_prone(f->ctd.defender) ? 1 : 0, rt_clock);
            }

            combat_commit_snapshot(&f->ctd, f->callDisplay);
            trace_event(capped ? TRACE_WARN : TRACE_COMBAT, capped ? "resolve-capped" : "resolve",
                "%s elapsed=%.2f done=%d conn=%d action=%d pose=%d proj=%d capped=%d inflight=%d t=%.2f",
                f->attacker == obj_dude ? "DUDE" : "NPC", rt_clock - f->started,
                swingDone, connected, actionReached, poseEnded, projectileAttack, capped,
                rt_inflight_count - 1, rt_clock);

            // E: play the defender's stagger as its own sequence (decoupled from the
            // attacker's swing) for a LIGHT hit only -- knockdown/death/fire play their
            // own full reaction via the engine and must not be overridden by a flinch.
            // rt_hit_staggers is the DT gate: an armored critter shrugs off a hit whose
            // through-damage doesn't reach a fraction of its Damage Threshold and keeps
            // walking, instead of stutter-flinching on every glancing shot.
            if (rDmg > 0 && (rFlags & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_ON_FIRE)) == 0
                && rt_hit_staggers(rDefender, rWeapon, rDmg)) {
                rt_reaction_flinch(rAttacker, rDefender);
            }

            // A DAMAGING hit on a critter that's already down restarts its ground dwell, so it
            // stays prone RT_KNOCKDOWN_DWELL_SECONDS past this hit instead of standing up on the
            // old timer the instant the damage lands. A miss doesn't (rDmg > 0). There is no
            // visible reaction while prone -- one sprite can't lie down and flinch at once -- so
            // the hit reads through HP/damage text and the extended time on the ground.
            if (rDmg > 0) {
                rt_reaction_note_damage(rDefender);
            }

            // Killed mid-animation -> corpse frozen on a live frame. Under focus-fire a critter can
            // take a light hit (-> a HIT_FROM_FRONT/BACK/DODGE flinch) or be mid its OWN swing/fire
            // when the KILLING blow lands in the same beat. The engine's death fall is registered at
            // the lethal shot's fire time, but it can't register OVER the animation the critter is
            // already playing, so the fall is LOST and the corpse freezes upright on a live frame --
            // dead but not prone (anim 14 flinch / an attack pose), and the reap misses it (it drops
            // from the registry once dead). That live anim never resolves into a fall on its own, so
            // settle it to a clean static corpse right now. A genuine death fall keeps critter_is_prone
            // TRUE for its whole run, so a normally-animating death (id already falling, anim 20-35) is
            // left untouched. This is the "enemies drop like flies / freeze mid-hit" death bug.
            //
            // ⚠️ ONLY when the fall did NOT register. Since the reaction moved to this commit
            // point, a normal death registers its fall a few lines above -- but it has not STARTED
            // yet, so critter_is_prone() is still false right here and this salvage would fire on
            // EVERY death, snapping away the fall it was meant to rescue ("the rat skipped its
            // death animation and was instantly lying in a pool of blood"). reactionRegistered is
            // the discriminator: true means a fall is queued and will play, so leave it alone.
            if (!reactionRegistered && rDefender != NULL && rt_is_critter(rDefender)
                && critter_is_dead(rDefender) && !critter_is_prone(rDefender)) {
                register_clear(rDefender);
                critter_kill(rDefender, -1, true);
                trace_event(TRACE_FLOW, "death-settle", "pid=%d anim=%d t=%.2f",
                    rDefender->pid, FID_ANIM_TYPE(rDefender->fid), rt_clock);
            }

            rt_inflight[i] = rt_inflight[--rt_inflight_count];
        } else {
            i++;
        }
    }

    for (int i = 0; i < rt_actor_count; i++) {
        rt_update_actor(&rt_actors[i], dt);
    }

    // The dude must NEVER be left unable to act. Unlike an NPC -- which stays unconscious until
    // its queued knockout event wakes it -- a KNOCKED-OUT dude has to recover on his own, because
    // otherwise a save made while he was knocked down/out leaves him frozen in place after a
    // reload ("stuck after reload"): PrepLoad's game_reset() resets combat_rt BEFORE the dude is
    // restored, so the reset can't clear his serialized DAM_KNOCKED_* flags, and rt_needs_standup
    // skips a knocked-OUT critter. So for the dude specifically, drop the knockout here and fold it
    // into a knockdown, which the standup pass below then gets him up from (paying the standup AP).
    if (obj_dude != NULL && rt_is_critter(obj_dude)
        && (obj_dude->data.critter.combat.results & DAM_KNOCKED_OUT) != 0) {
        obj_dude->data.critter.combat.results &= ~(DAM_KNOCKED_OUT | DAM_LOSE_TURN);
        obj_dude->data.critter.combat.results |= DAM_KNOCKED_DOWN;
    }

    // The dude is adopted exactly like any other body -- and THAT is the point of the rewrite. He
    // used to need his own copy of the standup pass (he is not in rt_actors) and his own copy of
    // the flinch cap, and the two copies drifted: the long note that used to sit here recorded how
    // clearing the wrong timer let a knockdown pin him indefinitely, measured at 5+ seconds frozen
    // on a finished fall with not one FLOW/standup in a 56-minute log.
    //
    // One pass now runs both, so a bug found in either is fixed in both. Bystanders knocked down by
    // a stray shot are covered too, which neither old pass could see.
    rt_reaction_adopt(obj_dude);

    // RECONCILE. Everything that decides a body has finished falling, may get up, has flinched long
    // enough, or is frozen on a cut animation happens in here and nowhere else.
    rt_reaction_tick(dt);

    // Start new attacks (player + NPCs) for this frame. Multiple may start: the player and any
    // NPC whose target isn't already in flight resolve concurrently. Runs after the actor loop
    // so every intent recorded this frame -- plus the latched player intent -- is in the running.
    rt_resolve_attacks(dt);

    // Pending-flag safety valve. With concurrency the global pending flag is snapshotted+cleared
    // the same frame it's set (rt_do_attack) or adopted at the top of the commit pass, so it
    // should never linger. If it somehow does (an unexpected path leaving it set), clear it after
    // a grace so the engine doesn't think an attack is forever mid-resolve. NOTE: deliberately NO
    // anim_stop() here -- that global flush would kill the legitimately-concurrent in-flight
    // attack animations; in-flight entries self-drain via the per-attack cap instead.
    if (rt_attack_locked()) {
        rt_lock_held += dt;
        if (rt_lock_held >= RT_WATCHDOG_SECONDS) {
            trace_event(TRACE_WARN, "pending-stuck",
                "cleared lingering pending after %.2fs inflight=%d t=%.2f",
                rt_lock_held, rt_inflight_count, rt_clock);
            combat_clear_pending_attack();
            rt_lock_held = 0.0f;
        }
    } else {
        rt_lock_held = 0.0f;
    }

    // Engagement = any tracked critter actively pursuing/attacking. Keep combat
    // "warm" while engaged so the reused damage pipeline works; never run the
    // turn scheduler.
    bool engaged = false;
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].state != RT_IDLE && rt_is_combatant(rt_actors[i].obj)) {
            engaged = true;
            break;
        }
    }

    if (engaged != rt_engaged) {
        int hostiles = 0;
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].hostile) {
                hostiles++;
            }
        }
        trace_event(TRACE_FLOW, engaged ? "combat-enter" : "combat-exit",
            "actors=%d host=%d t=%.2f", rt_actor_count, hostiles, rt_clock);

        // VANILLA'S BOOKENDS FOR THE MANEUVER FLAGS, which realtime never ran: combat_begin keeps only
        // ENGAGING when a fight starts, combat_over clears every flag (and damageLastTurn) when it
        // ends. Without the end, one critter_stop_attacking from a script turn -- VTEAM3 runs it on
        // every combat event in which the mutant cannot see you -- would have left that critter
        // DISENGAGING, i.e. out of every fight, for the rest of the game. whoHitMe is NOT cleared,
        // although combat_over clears it: realtime keeps hostility sticky on purpose.
        for (int i = 0; i < rt_actor_count; i++) {
            Object* c = rt_actors[i].obj;
            if (c == NULL) {
                continue;
            }
            if (engaged) {
                c->data.critter.combat.maneuver &= CRITTER_MANEUVER_ENGAGING;
            } else {
                c->data.critter.combat.maneuver = CRITTER_MANEUVER_NONE;
                c->data.critter.combat.damageLastTurn = 0;
            }
        }

        if (engaged) {
            rt_balance_begin();
        } else {
            rt_balance_report();
        }
    }

    // Sample BEFORE rt_engaged is updated, so the frame the fight ends is not counted.
    if (engaged) {
        rt_balance_sample(dt);
    }

    // Runs in and out of combat -- the caravan is not a fight.
    rt_missing_art_scan();

    rt_engaged = engaged;
    if (engaged) {
        combat_state |= COMBAT_STATE_0x01;

        // While the in-combat bit is set, anim.cc deducts the dude's engine
        // combat.ap per tile moved and truncates the move at ap <= 0. That field
        // is NOT combat_rt's AP pool (movement is free), so keep combat.ap topped
        // up to guarantee WASD/stick movement always works during a fight.
        obj_dude->data.critter.combat.ap = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);
    } else {
        combat_state &= ~COMBAT_STATE_0x01;
    }

    // The scripts' combat events -- the turn starts and "do you join?" checks vanilla gives every
    // round. After the actor loop and the attack pass, so a script that moves or removes a critter
    // only ever affects the NEXT frame's registry.
    rt_script_turn_tick();

    // Is the dude actively running this frame? (Drives both the regen pause below and
    // the drain block. FID_ANIM_TYPE == ANIM_RUNNING is the true "visually sprinting"
    // signal -- see the drain comment.)
    bool dudeRunning = rt_engaged && FID_ANIM_TYPE(obj_dude->fid) == ANIM_RUNNING;

    // Variant C2: walking in combat also spends the pool. Detected the same way as
    // running (the live anim type), so post-attack flinch/swing anims read as neither
    // run nor walk = "standing" = full regen (an attack is not kiting).
    bool dudeWalking = rt_engaged && FID_ANIM_TYPE(obj_dude->fid) == ANIM_WALK;

    // PROXIMITY REGEN: the multiplier comes from the DISTANCE to the nearest live hostile, and
    // from whether the dude is moving -- the two together, not from stance alone. The dude is on
    // the MOVING CURVE (the `true` argument): standing at contact refills fastest (2.0x), moving
    // at contact refills at 0, and the far end is 0.6x standing / 1.0x moving. So closing on an
    // enemy from across the room is free, and once you are ON him you choose between moving and
    // shooting -- vanilla's trade, restored. Movement still COSTS on top (the drain block below).
    //
    // Out of combat there is no hostile to measure, so the multiplier is a neutral 1.0 and
    // the pool refills at the plain rate.
    //
    // ⚠️ HISTORY THAT MUST NOT BE FORGOTTEN: an early build paused regen while WALKING and
    // DEADLOCKED at 0 AP -- movement was free at an empty pool, so kiting pinned the pool at 0
    // forever. The moving curve reintroduces a zero-regen state (in contact), so the same trap is
    // live again in principle, and exactly one thing keeps it closed: rt_move_blocked (below)
    // STOPS the dude at 0 AP. Stopped means not moving, which means the standing curve, which at
    // contact is the FASTEST refill in the game -- so an empty pool always recovers. NEVER make
    // movement free at 0 AP again without also reverting the moving curve to a floor of 1.0.
    //
    // GATED ON rt_engaged, and that gate is half of "the fight cools down". rt_dude_nearest_-
    // hostile_dist() measures to the nearest live STICKY-hostile anywhere on the map, and
    // hostility is never cleared -- so without this gate one survivor who lost track of you three
    // rooms away pinned the dude at the far multiplier (0.6x) permanently, everywhere, until it
    // died. Disengaging then achieved nothing the player could feel. Out of a fight there is
    // nothing to measure against, so the multiplier is a neutral 1.0 -- exactly what the NPC half
    // already does via npcInFight.
    int dudeEnemyDist = rt_engaged ? rt_dude_nearest_hostile_dist() : -1;

    // ⚠️ THE DEADLOCK GUARD FOR ap_empty_walks, AND IT IS NOT OPTIONAL. Under that mode the dude
    // keeps moving at 0 AP for free, which is EXACTLY the shape that deadlocked an early build:
    // free movement plus a zero-regen moving curve pins the pool at 0 forever. The stop-dead
    // behaviour closed it by forcing him onto the standing curve; this mode closes it by putting
    // him on the standing curve DIRECTLY while the latch is up, moving or not. The pool then always
    // climbs, the latch always clears, and the walk is a penalty rather than a trap.
    bool dudeOnMovingCurve = (dudeWalking || dudeRunning)
        && !(rt_move_blocked && rt_ap_empty_walks);
    float regenDt = dt * rt_proximity_regen_mult(dudeEnemyDist, dudeOnMovingCurve, true);

    // The dt=0 run call still clamps the INT_MAX startup sentinel to the dude's current
    // max on frame 1.
    rt_regen_ap(obj_dude, &rt_player_ap, &rt_player_ap_regen,
        stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS), regenDt);

    // The Bonus Move reserve refills on the same clock, but ONLY while he is not moving -- that is
    // the realtime "start of your next turn". Refilling on the move would turn it into a flat
    // discount on the walk drain, and a big enough discount makes walking free, i.e. kiting. Out of
    // a fight it is held full, so every fight opens with it available, as every vanilla turn did.
    int bonusMoveMax = rt_bonus_move_max();
    if (!rt_engaged) {
        rt_bonus_move_ap = bonusMoveMax;
        rt_bonus_move_regen = 0.0f;
    } else {
        int bonusMoveBefore = rt_bonus_move_ap;
        rt_regen_ap(obj_dude, &rt_bonus_move_ap, &rt_bonus_move_regen, bonusMoveMax,
            (dudeWalking || dudeRunning) ? 0.0f : regenDt);
        if (bonusMoveMax > 0 && bonusMoveBefore < bonusMoveMax && rt_bonus_move_ap == bonusMoveMax) {
            trace_event(TRACE_ACTION, "bonus-move-full", "max=%d ap=%d t=%.2f",
                bonusMoveMax, rt_player_ap, rt_clock);
        }
    }

    // Run cost (kiting nerf): running IN COMBAT drains the same pool attacks spend, so
    // sprinting and shooting compete -- net negative while running (drain > regen) makes
    // the pool fall, dropping the dude to a free walk after a few seconds.
    //
    // Keyed on the dude actually being in the RUN animation (FID_ANIM_TYPE == ANIM_RUNNING)
    // rather than the stick-segment bookkeeping. The segment flag (rt_stick_active) blinks
    // off for a frame between segments and on direction changes, and the OLD code reset the
    // fractional accumulator on every such gap -- discarding partial drain and roughly
    // halving the effective burn (an EN-1 sprint lasted ~6s instead of the intended ~3s).
    // The anim-type gate burns continuously while the run cycle plays and is zero for
    // WALK/STAND/knockdown/flinch/attack, so walking and being disabled stay free and
    // standing with LT held (no run anim) never drains. The accumulator is only reset once
    // the player genuinely stops feeding movement (past the input grace), so transient
    // inter-segment gaps no longer throw away progress. Runs AFTER regen so the two net out;
    // the latch + resume threshold give the sprint / walk-recover / sprint burst rhythm.
    if (dudeRunning) {
        rt_run_ap_drain += rt_effective_run_cost(obj_dude) * dt;
        while (rt_run_ap_drain >= 1.0f && rt_take_move_point()) {
            rt_run_ap_drain -= 1.0f;
        }
        if (rt_player_ap <= 0 && rt_bonus_move_ap <= 0) {
            rt_player_ap = 0;
            rt_run_ap_drain = 0.0f;
            rt_run_blocked = true; // gait latch (now moot: rt_move_blocked stops motion)
            if (!rt_move_blocked) {
                trace_event(TRACE_ACTION, "move-blocked", "gait=run resume_ap=%d t=%.2f", rt_run_resume_ap, rt_clock);
            }
            rt_move_blocked = true; // STOP until AP recovers to rt_run_resume_ap
        }
    } else if (dudeWalking) {
        // Variant C2 walk drain: smaller flat rate, no run_blocked latch (walking is
        // already the slowest gait, so there's nothing to fall back TO -- emptying the
        // pool sets rt_move_blocked and stops the dude outright until he has regenerated
        // to resume_ap). Regen is paused for the whole walk (see the regen block above),
        // so this rate is the FULL burn, not a net figure. A zero rt_walk_ap_cost makes
        // this a no-op and walking is free again (but still non-regenerating).
        // Under ap_empty_walks the recovery walk is FREE: he has already paid for running dry by
        // being reduced to a walk, and charging for it as well would hold the pool at 0 whenever
        // the walk cost exceeds the regen (it does, at any distance from the enemy) -- a walk he
        // could never walk out of.
        if (!(rt_move_blocked && rt_ap_empty_walks)) {
            rt_walk_ap_drain += rt_effective_walk_cost() * dt;
        }
        while (rt_walk_ap_drain >= 1.0f && rt_take_move_point()) {
            rt_walk_ap_drain -= 1.0f;
        }
        if (rt_player_ap <= 0 && rt_bonus_move_ap <= 0) {
            rt_player_ap = 0;
            rt_walk_ap_drain = 0.0f;
            if (!rt_move_blocked) {
                trace_event(TRACE_ACTION, "move-blocked", "gait=walk resume_ap=%d t=%.2f", rt_run_resume_ap, rt_clock);
            }
            rt_move_blocked = true; // STOP until AP recovers to rt_run_resume_ap
        }
    } else if (rt_move_input_age >= RT_MOVE_INPUT_GRACE) {
        rt_run_ap_drain = 0.0f;
        rt_walk_ap_drain = 0.0f;
    }

    // Resume threshold, clamped to the dude's max AP so a low-AGI critter (or a small
    // cfg max) can never sit below it forever = no freeze-forever even if maxAP < resume.
    int rt_resume_ap = rt_run_resume_ap;
    int rt_dude_max_ap = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);
    if (rt_resume_ap > rt_dude_max_ap) {
        rt_resume_ap = rt_dude_max_ap;
    }
    if (rt_run_blocked && rt_player_ap >= rt_resume_ap) {
        rt_run_blocked = false;
    }
    if (rt_move_blocked && rt_player_ap >= rt_resume_ap) {
        rt_move_blocked = false;
        trace_event(TRACE_ACTION, "move-unblocked", "ap=%d/%d t=%.2f", rt_player_ap, rt_dude_max_ap, rt_clock);
    }

    // OUT OF BREATH (rt_wipe_forehead, his pick 2026-10-06: "když mám ty žárovičky červené"). The
    // lamps went red, so he wipes his forehead -- once he has actually stopped: the step that ran him
    // dry finishes first, and a stick still held means he is trying to move, not resting. The latch
    // clearing ends the wish; once he may walk again, walking cuts the gesture like any fidget.
    {
        static bool wipePending = false;
        static bool wasBlocked = false;
        bool blockedNow = combat_rt_move_blocked();
        if (blockedNow && !wasBlocked) {
            wipePending = true;
        }
        if (!blockedNow) {
            wipePending = false;
        }
        wasBlocked = blockedNow;

        if (wipePending && !combat_rt_stick_walking() && rt_is_combatant(obj_dude)) {
            if (rt_wipe_forehead(obj_dude, "winded") != 0) {
                wipePending = false;
            }
        }
    }

    // Drive the interface bar's AP indicator from the real pool so the bulbs
    // light up one at a time as AP regenerates (and drop the instant an attack
    // spends them). Redraw only when the count changes to avoid per-frame blits.
    // intface_update_move_points itself reads combat_rt_player_ap() while
    // combat_rt is enabled, so the engine combat.ap (kept maxed for movement)
    // never reaches the bar.
    // The dude's sprite must hold what his hand holds. The swap animation that normally keeps
    // those in step is cancellable in realtime, so it is verified here instead of trusted.
    intface_reconcile_weapon_fid();

    // Friend / neutral / foe outlines while aiming (rt_aim_outlines_update).
    rt_aim_outlines_update();

    int hudAp = combat_rt_player_ap();
    int hudBlocked = combat_rt_move_blocked() ? 1 : 0;
    int hudBonus = combat_rt_bonus_move_ap();
    if (hudAp != rt_last_hud_ap || hudBlocked != rt_last_hud_blocked || hudBonus != rt_last_hud_bonus) {
        rt_last_hud_ap = hudAp;
        rt_last_hud_blocked = hudBlocked;
        rt_last_hud_bonus = hudBonus;
        intface_update_move_points(hudAp, hudBonus);
    }

    // Keep the ARMOR CLASS readout live for the same reason. animate=false on purpose: the
    // animated path spins the digits with block_for_tocks + renderPresent, which would stall the
    // realtime loop once per changed point. stat_level is the single source of truth here, so
    // this also covers armour swaps and stat buffs, not just the AP term.
    if (obj_dude != NULL) {
        int hudAc = stat_level(obj_dude, STAT_ARMOR_CLASS);
        if (hudAc != rt_last_hud_ac) {
            rt_last_hud_ac = hudAc;
            intface_update_ac(false);
        }
    }

    // Un-stick the UI. combat_anim_begin disables the interface + shows the wait
    // cursor when obj_dude == main_ctd.attacker, and combat_anim_finished only
    // re-enables if that's still true at completion. Under combat_rt main_ctd is
    // reused across actors, so a concurrent NPC attack can leave the interface
    // stuck disabled with the hourglass (the lockout bug). Whenever the dude is
    // idle, re-enable it and restore the cursor.
    // anim_busy_blocking, not anim_busy -- same reason as the attack-eligibility gate above.
    // Plain anim_busy stays set while an old sequence merely still LISTS the dude's spent swing,
    // so this un-stick stopped firing and the interface stayed disabled with the hourglass up.
    if (anim_busy_blocking(obj_dude) == 0 && !intface_is_enabled()) {
        game_ui_enable();
        gmouse_3d_refresh();
    }

    // Kills accumulate experience into the global combat_exps (in damage_object);
    // the turn-based pipeline only awards it in combat_over(), which combat_rt
    // never calls. Award and clear it here so the player gets kill XP promptly.
    if (combat_exps != 0) {
        combat_give_exps(combat_exps);
        combat_exps = 0;
    }
}

bool combat_rt_enabled()
{
    return rt_enabled;
}

bool combat_rt_dude_moving()
{
    return rt_move_input_age < RT_MOVE_INPUT_GRACE;
}

int combat_rt_player_max_ap()
{
    if (obj_dude == NULL) {
        return 0;
    }
    return stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);
}

// The REAL action-point pool of any critter under realtime combat, or -1 when combat_rt is not
// the authority on it (disabled, no critter, or an NPC that isn't tracked in the registry).
//
// Exists because the engine's `combat.critter.ap` field is NOT the pool any more: combat_rt pins
// it to max every frame for both the dude (so anim.cc won't truncate walking) and every NPC, and
// only combat_attack briefly subtracts a weapon cost from it. Anything that wants to ask "how
// many points does this critter actually have left" must come here -- see STAT_ARMOR_CLASS in
// stat.cc, where vanilla's "unused AP make you harder to hit" rule was reading the pinned field
// and therefore handing out a flat max-AP bonus for the whole fight.
int combat_rt_critter_ap(Object* critter)
{
    if (!rt_enabled || critter == NULL) {
        return -1;
    }

    if (critter == obj_dude) {
        return combat_rt_player_ap();
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == critter) {
            int ap = rt_actors[i].ap;
            return ap < 0 ? 0 : ap;
        }
    }

    return -1;
}

int combat_rt_player_ap()
{
    // Clamp: rt_player_ap is the INT_MAX sentinel until the first update frame
    // clamps it down, so report the cap until then.
    int maxAp = combat_rt_player_max_ap();
    if (rt_player_ap > maxAp) {
        return maxAp;
    }
    if (rt_player_ap < 0) {
        return 0;
    }
    return rt_player_ap;
}

// Is movement currently stopped on an empty pool? -- see the header. Reports the LATCH, not
// "AP is below the threshold", and the difference is the whole point: the pool also sits
// under the threshold right after a shot, and there the dude is perfectly free to walk until
// the drain runs him down to 0. Only the latch means he has actually lost the ability to
// move, which is the one state the bar has any business colouring red.
//
// False out of a fight ON PURPOSE. The drain block is gated on rt_engaged, so outside combat
// nothing empties the pool and no threshold is enforced; without this gate a fight that ended
// on an empty pool would leave the bar red for the seconds it takes regen to clear the latch,
// while the dude is already walking around freely.
bool combat_rt_move_blocked()
{
    return rt_enabled && rt_engaged && rt_move_blocked;
}

// The Bonus Move reserve -- see rt_bonus_move_ap. 0 without the perk, and 0 out of a fight ON
// PURPOSE, for the same reason as the latch above: movement costs nothing there, so there is
// nothing for the reserve to pay for, and bulbs lit for it would claim otherwise.
int combat_rt_bonus_move_ap()
{
    if (!rt_enabled || !rt_engaged) {
        return 0;
    }

    int maxBonus = rt_bonus_move_max();
    if (rt_bonus_move_ap > maxBonus) {
        return maxBonus;
    }
    if (rt_bonus_move_ap < 0) {
        return 0;
    }
    return rt_bonus_move_ap;
}

// Empty a critter's realtime AP pool outright -- the realtime meaning of vanilla's "lose your
// turn". A critical failure sets `combat.ap = 0` on the attacker (combat.cc, DAM_LOSE_TURN), but
// combat_rt keeps that engine field pinned at max so movement stays free, so the punishment landed
// on a number nothing reads and a critical miss cost you NOTHING beyond the miss itself. Draining
// the real pool is the honest translation: no turns to lose, so you lose the points and wait out
// the regen -- which is the same tempo cost, paid in the currency this combat model actually uses.
//
// The accumulator goes with it, or a fraction of a point banked before the failure would hand back
// the first point almost immediately.
void combat_rt_zero_ap(Object* critter)
{
    if (!rt_enabled || critter == NULL) {
        return;
    }

    if (critter == obj_dude) {
        rt_player_ap = 0;
        rt_player_ap_regen = 0.0f;
        // A lost vanilla turn skips the turn setup that grants Bonus Move too (combat.cc).
        rt_bonus_move_ap = 0;
        rt_bonus_move_regen = 0.0f;
        return;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == critter) {
            rt_actors[i].ap = 0;
            rt_actors[i].ap_regen = 0.0f;
            return;
        }
    }
}

// Spend AP from the realtime pool (e.g. a weapon reload = 2). Free and always
// succeeds outside combat (the pool only matters while engaged). While engaged it
// deducts only if the pool has enough, returning false otherwise so the caller can
// refuse the action -- same tension as the turn-based AP economy.
// Give back points an action took up front and then never did (the reload that a knockdown or the
// player's own move cut short). Out of a fight the pool is held full anyway, so nothing to return.
void combat_rt_refund_ap(int amount)
{
    if (!rt_engaged || amount <= 0) {
        return;
    }

    rt_player_ap += amount;
    int maxAp = combat_rt_player_max_ap();
    if (rt_player_ap > maxAp) {
        rt_player_ap = maxAp;
    }
}

bool combat_rt_spend_ap(int cost)
{
    if (!rt_engaged) {
        return true;
    }

    int maxAp = combat_rt_player_max_ap();
    if (rt_player_ap > maxAp) {
        rt_player_ap = maxAp;
    }

    if (rt_player_ap < cost) {
        return false;
    }

    rt_player_ap -= cost;
    return true;
}

bool combat_rt_handle_input(int keyCode)
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return false;
    }

    // Top up the dude's combat AP before processing any movement/attack input.
    // While the in-combat bit is set, anim.cc truncates moves at ap <= 0; doing
    // this here (before input) guarantees a WASD move never starts AP-starved.
    obj_dude->data.critter.combat.ap = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);

    // (W/A/S/D used to step single tiles here, four of the six directions. Walking on the keyboard is
    // the arrows now, read as a stick in combat_rt_poll_gamepad; see gamepad_ui_handle_key.)

    // KEYBOARD & MOUSE: a REAL click on the enemy the mouse is aiming at belongs to the attack, which
    // rt_mouse_update fires, tapped or held -- never to vanilla, which would try to talk to him or
    // open its turn-based combat. A real right-click on him opens the called-shot window (R3 on the
    // pad). rt_mouse_target is only ever set while the mouse is in use, so a pad's synthesised click
    // never gets here with one.
    if (keyCode == -2 && rt_mouse_hold_claimed && (mouse_get_buttons() & (MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT | MOUSE_EVENT_LEFT_BUTTON_UP)) != 0) {
        return true;
    }
    if (keyCode == -2 && rt_mouse_target != NULL) {
        Uint32 real = SDL_GetMouseState(NULL, NULL);
        int buttons = mouse_get_buttons();
        if ((real & SDL_BUTTON_RMASK) != 0 && (buttons & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
            trace_event(TRACE_INPUT, "kbm-key", "key=RMB target=%d", rt_mouse_target->pid);
            gamepad_ui_request_called_shot();
            return true;
        }
        if ((buttons & MOUSE_EVENT_LEFT_BUTTON_UP) != 0
            || ((buttons & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) != 0 && (real & SDL_BUTTON_LMASK) != 0)) {
            return true;
        }
    }

    // Left-click on a HOSTILE critter in range -> attack it. A click on a
    // non-hostile critter (or a hostile out of range) is left unconsumed so it
    // falls through to game_handle_input's normal explore-mode action (talk,
    // examine, use) -- otherwise clicking a peaceful NPC would attack it.
    if (keyCode == -2 && (mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
        Object* target = object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
        if (target != NULL && target != obj_dude && rt_is_combatant(target) && rt_is_hostile(target)) {
            int hitMode = rt_player_hit_mode();
            if (obj_dist(obj_dude, target) <= item_w_range(obj_dude, hitMode)) {
                rt_set_player_intent(target, hitMode, rt_player_called_loc(hitMode));
                return true;
            }
        }
    }

    return false;
}

bool combat_rt_active()
{
    return rt_engaged;
}

// "Is something actually coming for me RIGHT NOW?" -- the narrow question vanilla's isInCombat()
// used to answer by accident, and no longer does.
//
// WHY THIS EXISTS. Vanilla combat is a bounded encounter: you enter it, it lasts a few turns, you
// leave. Dozens of rules hang off isInCombat() on that assumption -- no talking, no script timers,
// no ambient critter scripts -- and they are all fine when "combat" means twenty seconds. Realtime
// turned combat into a CONTINUOUS state that can stay warm for minutes because some critter three
// rooms away is still in RT_CHASE, and every one of those rules silently became a much bigger
// restriction than it was ever written to be. The player met this as "I killed the guards and then
// couldn't talk to Gizmo at all" -- nobody designed that, we inherited it.
//
// So: engaged, hostile, live, actually after the player's side, and close enough to perceive him.
// Anything less than that is not a fight the player is in, whatever the warm combat bit says.
// Falls back to isInCombat() when the turn-based system owns combat, where the old meaning holds.
// Is THIS critter actually fighting the player right now? The per-critter half of the question
// above, and the answer to a nastier version of the same trap: combat_rt's `hostile` flag is
// STICKY and never cleared, so a bystander swept up by faction aggro carries it for the rest of
// the map's life. Anything gated on the raw flag therefore treats a man who never threw a punch
// exactly like an enemy mid-swing -- forever.
bool combat_rt_is_engaging(Object* obj)
{
    if (obj == NULL || obj_dude == NULL) {
        return false;
    }
    if (!rt_enabled) {
        return isInCombat() && rt_is_hostile(obj);
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (a->obj != obj) {
            continue;
        }
        if (!a->hostile || a->state == RT_IDLE || !rt_is_combatant(obj)) {
            return false;
        }
        if (a->target != obj_dude && !(a->target != NULL && isPartyMember(a->target))) {
            return false;
        }
        return is_within_perception(obj, obj_dude);
    }
    return false;
}

// Is anyone still playing a death animation right now?
//
// Exists for ONE caller and one reason: saving the game runs map_save_in_game -> anim_stop(), which
// ends EVERY animation sequence and force-flattens whoever was mid-fall (show_death via
// anim_set_end). An autosave that lands in the second after a kill therefore slams the corpses
// straight to the ground -- "koukni jak obě npc spadly jako švestky ze stromu, žádná animace".
// Confirmed from the backtrace in trace_2026-08-13_190113.log: combat-exit at s=17.22, AutoSaveQuest
// at s=17.23, both bodies flat in the same frame. Identical mechanism to the dialogue-interrupts-a-
// death bug (2026-08-10); only the caller is new, which is why it survived that fix.
//
// Deliberately NOT restricted to hostiles or to combat: any critter dying anywhere on the dude's
// elevation has a fall worth not destroying.
bool combat_rt_death_anim_pending()
{
    if (obj_dude == NULL) {
        return false;
    }

    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
            continue;
        }
        // A hidden body is not on screen, so nothing is waiting for its animation to finish.
        if ((obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }
        if ((obj->data.critter.combat.results & DAM_DEAD) == 0) {
            continue;
        }
        if (anim_busy(obj) != 0) {
            return true;
        }
    }

    return false;
}

bool combat_rt_under_threat()
{
    if (!rt_enabled) {
        return isInCombat();
    }

    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return false;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (!a->hostile || a->state == RT_IDLE || !rt_is_combatant(a->obj)) {
            continue;
        }
        // After US specifically -- a critter busy with a scripted brawl against another NPC is
        // not a threat to the player and must not lock him out of talking or of his own scripts.
        if (a->target != obj_dude && !(a->target != NULL && isPartyMember(a->target))) {
            continue;
        }
        if (!is_within_perception(a->obj, obj_dude)) {
            continue;
        }
        return true;
    }

    return false;
}

// True while a fight is going on WITHIN SIGHT of the player -- whoever it is between.
//
// Vanilla has ONE global combat state: if anybody on the map is fighting, isInCombat() is true and
// dialogue is refused outright. combat_rt_under_threat() deliberately narrowed that to "is something
// after ME", which fixed the map-wide, minutes-long talk lockout (the "Gizmo's guards are dead and
// the man still won't talk" report). But it also opened a hole vanilla never had: a brawl the player
// is merely a BYSTANDER to leaves him free to stroll up and start a conversation mid-fight.
//
// That is exactly how the Skulz ambush got staged twice: Vinnie reads host=0 tgt=<another NPC> for
// the whole fight, so nothing was ever "after" the player, so the talk gate opened -- and his
// dialogue branch runs load_map, which re-spawns the ambush (see the memory note; the duplication
// itself is vanilla script design, this gate is what let it be reached).
//
// NOTE the deliberate absence of an a->hostile test. The brawlers are NOT hostile to the dude --
// that is the whole point. What matters is that a fight is happening where he can see it.
//
// Used ONLY by the player-initiated talk gates. Scripts, timed events and NPC behaviour keep using
// combat_rt_under_threat(); widening those is what caused the lockouts in the first place.
//
// The player always has vanilla's own way out: combat_rt_end_combat() (Enter, or the D-pad entry)
// sets every actor RT_IDLE and drops rt_engaged, so this goes false the moment he ends the fight.
bool combat_rt_fight_in_view()
{
    if (!rt_enabled) {
        return isInCombat();
    }

    if (!rt_engaged || obj_dude == NULL) {
        return false;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        if (a->state == RT_IDLE || a->target == NULL || !rt_is_combatant(a->obj)) {
            continue;
        }
        if (!is_within_perception(a->obj, obj_dude)) {
            continue;
        }
        return true;
    }

    return false;
}

// Manual END COMBAT. Vanilla bound this to Enter and to the interface bar's END COMBAT button,
// both of which route through combat_input() -> combat_end() -- the dormant turn-based path, so
// in realtime the player had no way at all to break off, and no way to be TOLD why he could not.
//
// The rule is vanilla's, unchanged: combat_end() walks the enemy list and refuses the moment one
// critter fails combatai_want_to_stop(). We ask the same question through rt_wants_to_stop (the
// direct, process_bk-free port) and print the same message on refusal, so this is a query of the
// disengage rule rather than a second, softer way out of a fight.
//
// Hostility is deliberately NOT cleared on success -- vanilla's combat_end() pacifies nobody
// either. Everyone that passed the test perceives nothing to fight, so the perception-gated
// re-aggro in RT_IDLE will not immediately re-engage; walk back into view and the fight resumes.
bool combat_rt_end_combat()
{
    if (!rt_engaged) {
        return true;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* npc = a->obj;
        if (!a->hostile || !rt_is_combatant(npc)) {
            continue;
        }

        // Ask about whatever it is actually after. With no target of its own, ask about the
        // nearest of us -- the same object the re-aggro branch would pick -- so a "yes" here
        // cannot be undone by that branch on the next frame.
        Object* tgt = a->target != NULL ? a->target : rt_nearest_player_side(npc);
        if (!rt_wants_to_stop(npc, tgt)) {
            trace_event(TRACE_FLOW, "end-combat-refused", "id=%d pid=%d st=%d dist=%d t=%.2f",
                npc->id, npc->pid, a->state,
                tgt != NULL ? obj_dist(npc, tgt) : -1, rt_clock);
            combat_display_cant_end_message();
            return false;
        }
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        a->state = RT_IDLE;
        a->target = NULL;
        a->no_path_since = -1.0f;
        a->unperceived_since = -1.0f;
        a->weapon_checked = false;
    }

    rt_engaged = false;
    combat_state &= ~COMBAT_STATE_0x01;

    trace_event(TRACE_FLOW, "end-combat", "manual actors=%d t=%.2f", rt_actor_count, rt_clock);
    return true;
}

// A SCRIPT ended the fight (op_terminate_combat). Vanilla's version is unconditional -- the script
// has decided the fight is over and nobody is asked -- so this is the second half of the manual
// end above without its refusal test. Hostility stays exactly as vanilla leaves it: anyone still
// hostile who can see you re-engages through the normal perception-gated re-aggro, so a script
// that called it too early cannot hand you a free escape, only a fresh start of the fight.
//
// Shots already in the air are left to land: they were committed before the script ran.
void combat_rt_terminate_from_script()
{
    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        a->state = RT_IDLE;
        a->target = NULL;
        a->no_path_since = -1.0f;
        a->unperceived_since = -1.0f;
        a->weapon_checked = false;
    }

    rt_engaged = false;
    combat_state &= ~COMBAT_STATE_0x01;

    trace_event(TRACE_FLOW, "end-combat", "script actors=%d t=%.2f", rt_actor_count, rt_clock);
}

// Onlooker notify for a victim that can no longer act as an aggro SEED -- i.e. one the hit
// killed or knocked out outright.
//
// Vanilla calls combatai_notify_onlookers(defender) from apply_damage for EVERY damaged critter,
// including one that dies from that very hit. combat_rt replaced that with the seed-anchored
// spread in rt_refresh_registry, which recruits same-team onlookers around a LIVE hostile whose
// whoHitMe is on the player's side. An instantly-killed victim never becomes that seed:
// rt_is_combatant() is false for a corpse, so the victim is dropped both as an actor and as a
// spread anchor, and the critter standing right next to it learns NOTHING. A one-shot kill
// therefore read as a silent takedown, while the same shot that merely WOUNDED the victim
// aggroed its whole camp one refresh later. Confirmed in trace_2026-08-27_170457: three player
// attacks, two instant kills, and ZERO [FLOW/newhostile] / [FLOW/aggro-spread] in the whole log.
//
// So notify here, at the damage commit, exactly where vanilla did -- but ONLY for the case the
// spread structurally cannot cover, and with the SAME recruitment bounds it uses (same team as
// the victim, and must SEE the victim or HEAR the gunfire). Nothing is aggroed that a surviving
// victim would not have aggroed a frame later; this only closes the death hole.
void combat_rt_notify_victim(Object* attacker, Object* victim)
{
    if (!rt_enabled || attacker == NULL || victim == NULL) {
        return;
    }

    if (!rt_is_critter(attacker) || !rt_is_critter(victim)) {
        return;
    }

    // Only the PLAYER'S side recruits. The flag means "hostile to the dude", so an NPC-vs-NPC
    // kill (a guard executing a thief) must not turn the victim's faction on US.
    if (attacker != obj_dude && !isPartyMember(attacker)) {
        return;
    }

    // Still standing -> the normal spread will seed off it on the next refresh. Leave it alone,
    // so the surviving-victim path keeps its existing, play-tested behaviour.
    if (rt_is_combatant(victim)) {
        return;
    }

    int team = victim->data.critter.combat.team;

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* onlooker = a->obj;
        if (a->hostile || onlooker == victim || !rt_is_combatant(onlooker)) {
            continue;
        }

        if (onlooker->data.critter.combat.team != team) {
            continue;
        }

        // Sight is checked against the VICTIM (that is what there is to witness); earshot is
        // checked against the ATTACKER, because rt_hears_gunfire derives "a firearm is involved"
        // from the anchor's own hands, and a corpse's whoHitMe is not a reliable second source.
        bool saw = is_within_perception(onlooker, victim);
        bool heard = rt_hears_gunfire(onlooker, attacker);
        if (!saw && !heard) {
            continue;
        }

        a->hostile = true;
        onlooker->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        trace_event(TRACE_FLOW, "witness-kill",
            "onlooker=%d(pid=%d) victim=%d(pid=%d) team=%d saw=%d heard=%d dist=%d t=%.2f",
            onlooker->id, onlooker->pid, victim->id, victim->pid, team,
            saw ? 1 : 0, heard ? 1 : 0, obj_dist(onlooker, victim), rt_clock);
    }
}

void combat_rt_notify_attack(Object* attacker, Object* defender)
{
    if (!rt_enabled || attacker == NULL || defender == NULL) {
        return;
    }

    // The dude's own attacks are driven by the input paths, not this hook.
    if (attacker == obj_dude || !rt_is_critter(attacker)) {
        return;
    }

    // Raise the attacker's ENGAGING maneuver so the fight persists across refreshes.
    attacker->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;

    if (defender == obj_dude) {
        // Attack aimed at the PLAYER: the authoritative scripted / on-sight aggro signal.
        //
        // ⚠️ whoHitMe CARRIES IT, so an attacker that is not tracked yet is not lost. The old
        // comment promised "the next registry refresh picks it up", but the refresh reads hostility
        // from whoHitMe and nothing here set it -- an untracked attacker's order simply vanished.
        // whoHitMe as "my fight target" is vanilla's own overload (op_attack's combat branch).
        attacker->data.critter.combat.whoHitMe = obj_dude;

        RtActor* tracked = NULL;
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].obj == attacker) {
                tracked = &rt_actors[i];
                break;
            }
        }

        // Scripts repeat the order every think tick for as long as they see you; log only the one
        // that changes something, or this line buries the fight it is meant to explain.
        if (tracked != NULL && tracked->hostile) {
            return;
        }

        trace_event(TRACE_FLOW, "notify-attack", "atk pid=%d team=%d party=%d dudeTeam=%d dist=%d tracked=%d t=%.2f",
            attacker->pid, attacker->data.critter.combat.team, isPartyMember(attacker) ? 1 : 0,
            obj_dude != NULL ? obj_dude->data.critter.combat.team : -1,
            obj_dude != NULL ? obj_dist(attacker, obj_dude) : -1, tracked != NULL ? 1 : 0, rt_clock);

        if (tracked != NULL) {
            tracked->hostile = true;
        }
        return;
    }

    // NPC-vs-NPC scripted attack (e.g. Gizmo's assassin sent at Killian). combat() is inert in
    // realtime, so without this the fight would never happen. Persist the target on the attacker
    // via whoHitMe -- op_attack's own overload (intextra.cc:1518) -- so the next registry refresh
    // derives engine_target from it and drives the fight; also set it on the tracked actor now
    // for an immediate reaction. A struck defender's own whoHitMe (set by apply_damage) makes it
    // retaliate through the same path, so the fight is two-sided. Live critter targets only.
    if (!rt_is_critter(defender)) {
        return;
    }
    attacker->data.critter.combat.whoHitMe = defender;
    trace_event(TRACE_FLOW, "notify-npc-attack", "atk=%d def=%d t=%.2f",
        attacker->pid, defender->pid, rt_clock);
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == attacker) {
            rt_actors[i].engine_target = defender;
            if (rt_actors[i].state == RT_IDLE) {
                rt_actors[i].target = defender;
                rt_actors[i].state = RT_CHASE;
            }
            return;
        }
    }
}

// Called from obj_remove_all() (object.cc) the instant before a map tears down and frees
// every critter. Realtime (unlike turn-based) lets the player walk to a map exit MID-COMBAT,
// so when the objects are freed, combat_rt is left holding dangling pointers: rt_resolving /
// rt_resolving_defender, the actor registry, the lock/intent/loot targets, AND the engine's
// pending main_ctd. The next combat_rt_update would commit that pending attack -> apply_damage
// -> combatai_check_retaliation -> combatai_rating(defender->whoHitMe) on freed memory =
// ACCESS_VIOLATION (the "leave map during combat -> crash" repro caught by [[trace-system]]).
// obj_remove_all is THE universal teardown signal (every map unload + new-game path routes
// through it), so hooking it here is far more reliable than watching map_get_index_number
// (which does NOT change for a same-map reload, and missed the teardown entirely in testing).
// Abandons the pending attack and drops every object pointer; rt_refresh_registry rebuilds
// for the new map. Safe/idempotent when nothing is in flight.
void combat_rt_notify_objects_freed()
{
    if (combat_is_attack_pending() || rt_inflight_count > 0 || rt_actor_count > 0) {
        trace_event(TRACE_FLOW, "objects-freed",
            "abandon pend=%d inflight=%d actors=%d t=%.2f",
            combat_is_attack_pending() ? 1 : 0, rt_inflight_count, rt_actor_count, rt_clock);
    }

    combat_clear_pending_attack();
    rt_abandon_encounter(); // map has unloaded -- also clears any queued escape transition

    // Every body in the table is about to be freed. obj_remove_all calls us at its TOP, so this
    // runs while the pointers are still valid -- and after it the table is empty, which is what
    // stops the reconciler sweeping freed memory on the first frame of the next map.
    rt_reaction_clear_all();

    // Snap the HP counter to the truth before the teardown. Safe here specifically because
    // obj_remove_all calls us at its TOP, before a single object is freed.
    intface_hp_roll_cancel();
}

// Hook from obj_erase_object(): a SINGLE object is about to be freed (script destroy, a
// gibbed/cleaned-up corpse, etc.). Unlike obj_remove_all (whole-map teardown), other live
// critters survive -- and any of them holding a whoHitMe pointer to the critter being freed
// would be left dangling. apply_damage's retaliation check dereferences exactly that
// (apply_damage -> combatai_check_retaliation -> combatai_rating(defender->whoHitMe)), so a
// freed-mid-fight attacker = an ACCESS_VIOLATION on the next hit against any survivor that it
// had hit. Clear every reference to `removed` here, at the one chokepoint where individual
// objects are freed, so no whoHitMe ever dangles. No-op for non-critters / outside realtime.
void combat_rt_notify_critter_removed(Object* removed)
{
    // An explosive projectile being freed (its sequence ended, or the map is going): the attack that
    // fired it must not read its fid again. Its explosionFid stays, so it now counts as landed.
    if (removed != NULL) {
        for (int i = 0; i < rt_inflight_count; i++) {
            if (rt_inflight[i].projectile == removed) {
                rt_inflight[i].projectile = NULL;
            }
        }
    }

    if (removed == NULL || !combat_rt_enabled() || FID_TYPE(removed->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    // Drop any in-flight attack snapshot whose attacker OR defender is the critter being freed,
    // so its deferred commit can't run apply_damage against freed memory.
    rt_inflight_drop_obj(removed);

    // ...and its reaction entry, which holds a raw Object* the reconciler dereferences every frame.
    // Same class as the interact-prompt hole this hook already had to close once.
    rt_reaction_drop(removed);

    // ...and a reload it was in the middle of (combat_rt_is_reloading reads the fid).
    rt_reloading_drop(removed);

    // ...and any held attack offset (rt_swing_keep_tick dereferences it every frame).
    rt_swing_keep_drop(removed);

    // Drop combat_rt's own cached pointers.
    if (rt_lock_target == removed) {
        rt_lock_target = NULL;
    }
    if (rt_lock_push_anchor == removed) {
        rt_lock_push_anchor = NULL;
    }
    if (rt_lock_last_target == removed) {
        rt_lock_last_target = NULL;
    }
    rt_aim_outlines_forget(removed);
    if (rt_player_intent_target == removed) {
        rt_player_intent_target = NULL;
        rt_player_intent_ttl = 0.0f;
    }
    if (rt_pending_loot == removed) {
        rt_pending_loot = NULL;
        rt_pending_loot_kind = 0;
    }

    // Remove it from the actor registry and clear any actor that was targeting it (both the
    // active FSM target and the engine-assigned NPC-vs-NPC target -- either would dangle and be
    // dereferenced by rt_is_combatant on the next tick before a refresh could re-derive it).
    for (int i = 0; i < rt_actor_count;) {
        if (rt_actors[i].obj == removed) {
            rt_actors[i] = rt_actors[--rt_actor_count];
        } else {
            if (rt_actors[i].target == removed) {
                rt_actors[i].target = NULL;
            }
            if (rt_actors[i].engine_target == removed) {
                rt_actors[i].engine_target = NULL;
            }
            i++;
        }
    }

    // Clear whoHitMe in every other critter on this elevation (and the dude) that points at the
    // critter being freed. obj_create_list enumerates the live world critters; the freed object
    // is still linked at this point (we run before obj_remove), so skip it explicitly.
    Object** list = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &list);
    for (int i = 0; i < count; i++) {
        Object* c = list[i];
        if (c != NULL && c != removed && FID_TYPE(c->fid) == OBJ_TYPE_CRITTER
            && c->data.critter.combat.whoHitMe == removed) {
            c->data.critter.combat.whoHitMe = NULL;
        }
    }
    if (list != NULL) {
        obj_delete_list(list);
    }

    // obj_dude is normally in the list above, but clear defensively in case it isn't.
    if (obj_dude != NULL && obj_dude->data.critter.combat.whoHitMe == removed) {
        obj_dude->data.critter.combat.whoHitMe = NULL;
    }
}

// Airtight guard run at the commit point (combat_commit_pending_attack), where apply_damage's
// retaliation check dereferences defender->whoHitMe. The obj_erase_object hook above clears
// references when an object is freed, but a free can slip through (a path that doesn't route
// through obj_erase_object, or a same-frame free before the next refresh), leaving a dangling
// whoHitMe that crashes combatai_rating (ACCESS_VIOLATION reading a freed Object's fid). Here we
// re-validate the whoHitMe of both attack parties by membership in the live critter list --
// obj_create_list scans objectTable, it never dereferences the candidate pointer, so reading a
// stale whoHitMe value to compare it is safe -- and NULL any that no longer points at a live
// critter. The party objects themselves (a, b) are validated separately by apply_damage.
void combat_rt_validate_who_hit_me(Object* a, Object* b)
{
    if (!combat_rt_enabled()) {
        return;
    }

    Object* parties[2] = { a, b };

    // Anything worth a (relatively costly) world scan?
    bool need = false;
    for (int i = 0; i < 2; i++) {
        Object* c = parties[i];
        if (c != NULL && FID_TYPE(c->fid) == OBJ_TYPE_CRITTER) {
            Object* w = c->data.critter.combat.whoHitMe;
            if (w != NULL && w != obj_dude) {
                need = true;
            }
        }
    }
    if (!need) {
        return;
    }

    Object** list = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &list);
    for (int i = 0; i < 2; i++) {
        Object* c = parties[i];
        if (c == NULL || FID_TYPE(c->fid) != OBJ_TYPE_CRITTER) {
            continue;
        }
        Object* w = c->data.critter.combat.whoHitMe;
        if (w == NULL || w == obj_dude) {
            continue; // NULL is fine; the dude is always live.
        }
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (list[j] == w) {
                live = true;
                break;
            }
        }
        if (!live) {
            c->data.critter.combat.whoHitMe = NULL;
            trace_event(TRACE_WARN, "whohitme-stale", "cleared (%s) t=%.2f",
                c == obj_dude ? "DUDE" : "NPC", rt_clock);
        }
    }
    if (list != NULL) {
        obj_delete_list(list);
    }
}

// Called from map_leave_map() / the elevation-change path when the player walks off a map
// MID-COMBAT (realtime only). Vanilla Fallout can't reach an exit grid during combat, so its
// guard there (`if (isInCombat()) game_user_wants_to_quit = 1;`) is a never-fires safety --
// but realtime lets you flee mid-fight, which tripped it and BOOTED THE PLAYER TO THE MAIN
// MENU ("crashed to menu"). It also blocks the destination load (map_check_state gates the
// whole transition on !isInCombat()). So here we END combat cleanly: clear the engine warm
// bit (COMBAT_STATE_0x01) so isInCombat() goes false and the transition proceeds, and abandon
// any in-flight attack + drop actor pointers. If the destination map has hostiles, combat_rt
// re-engages there normally. obj_remove_all's objects-freed hook will also fire on load.
void combat_rt_end_for_map_exit()
{
    trace_event(TRACE_FLOW, "map-exit-combat-end", "actors=%d eng=%d t=%.2f",
        rt_actor_count, rt_engaged ? 1 : 0, rt_clock);

    combat_state &= ~COMBAT_STATE_0x01;

    combat_clear_pending_attack();
    rt_inflight_count = 0;
    rt_actor_count = 0;
    rt_engaged = false;
    rt_lock_target = NULL;
    rt_lock_push_anchor = NULL;
    rt_lock_last_target = NULL;
    rt_player_intent_target = NULL;
    rt_player_intent_ttl = 0.0f;
    rt_pending_loot = NULL;
    rt_pending_loot_kind = 0;
    rt_lock_held = 0.0f;
}

// Arm the mid-combat map exit. Called from map_leave_map when the dude steps onto an exit grid IN
// COMBAT (realtime). map_state already holds the destination; rt_update_map_exit (next frame) ends
// the fight so map_check_state can fire it. Idempotent.
void combat_rt_arm_map_exit(bool scripted)
{
    if (scripted && !rt_map_exit_scripted) {
        rt_map_exit_scripted = true;
        if (rt_map_exit_armed) {
            trace_event(TRACE_FLOW, "map-exit-armed", "scripted=1 (upgraded) t=%.2f", rt_clock);
        }
    }
    if (rt_map_exit_armed) {
        return;
    }
    rt_map_exit_armed = true;
    trace_event(TRACE_FLOW, "map-exit-armed", "scripted=%d t=%.2f", scripted ? 1 : 0, rt_clock);
}

// Per-frame map-exit tick (called from combat_rt_update): the dude is on an exit grid in a fight
// -> end the fight so map_check_state performs the queued transition (map_state already holds the
// destination). No hold any more (see rt_map_exit_armed). Stepping off the grid before the frame
// came round keeps it armed -- it fires the moment he is back on one -- and the destination only
// clears when the fight ends away from the exit, so a fight won elsewhere can't teleport the dude
// to a stale exit.
static void rt_update_map_exit(float dt)
{
    (void)dt;

    // SELF-ARM (the "sometimes the countdown never starts" fix, from the days of the 10 s hold;
    // the race is the same without it -- the exit simply never fires). The exit is normally armed by
    // map_leave_map at the instant the dude STEPS ONTO an exit grid -- but only if isInCombat()
    // was already true right then. In realtime the engaged flag flickers (it follows the hostile
    // registry, and combat can go cold for a beat and re-engage), so this race exists: step onto
    // the grid while combat reads COLD -> map_leave_map only STORES the destination and does not
    // arm -> combat re-engages before map_check_state (which is gated on !isInCombat) can run ->
    // the transition sits queued and never fires, and the only way out was to step off the grid
    // and back on while combat was hot. Arming from state rather than from the entry EVENT closes
    // it: engaged + standing on a grid + a destination already queued = arm (and go, below).
    // Skipped once committed (combat torn down, waiting for the map to load).
    if (!rt_map_exit_armed && !rt_map_exit_committed && rt_engaged
        && map_has_pending_transition() && obj_dude_on_exit_grid()) {
        combat_rt_arm_map_exit();
    }

    if (!rt_map_exit_armed) {
        return;
    }

    // A scripted exit (stairs, ladder, elevator) is "on the grid" by definition -- see
    // rt_map_exit_scripted. It must neither wait for one nor be dropped for lacking one.
    bool onGrid = rt_map_exit_scripted || obj_dude_on_exit_grid();

    if (!rt_engaged) {
        // Combat ended. On the exit grid -> "leave at once": disarm and let the warm bit clear
        // so map_check_state fires the queued transition. Away from the grid -> the dude won
        // the fight elsewhere; drop the queued destination so it can't teleport him.
        rt_map_exit_armed = false;
        rt_map_exit_scripted = false;
        if (!onGrid) {
            map_clear_pending_transition();
            trace_event(TRACE_FLOW, "map-exit-clear", "reason=combat-ended-off-grid t=%.2f", rt_clock);
        } else {
            trace_event(TRACE_FLOW, "map-exit-clear", "reason=combat-ended-on-grid t=%.2f", rt_clock);
        }
        return;
    }

    // On the grid: go now. Off it (he stepped off before this frame): stay armed and wait.
    if (!onGrid) {
        return;
    }

    rt_map_exit_armed = false;
    rt_map_exit_committed = true; // hold combat torn down until the transition actually fires
    trace_event(TRACE_FLOW, "map-exit-go", "scripted=%d t=%.2f", rt_map_exit_scripted ? 1 : 0, rt_clock);
    rt_map_exit_scripted = false;
    combat_rt_end_for_map_exit(); // clears warm bit -> map_check_state loads the destination
}

// Route a legacy player-attack request (combat_attack_this -> the crosshair mouse path,
// or the RT-trigger raw click) into the realtime intent system instead of letting it
// reach combat_attack(). combat_attack() sets the engine's pending flag
// (combat_cleanup_enabled) WITHOUT combat_rt's rt_resolving slot, so the commit/cap block
// (gated on rt_resolving != NULL) never fires and the attack lock wedges ON until the 2.5s
// watchdog force-clears it -- a frozen, damage-less swing. Routing through rt_set_player_intent
// makes the attack resolve through rt_do_attack like every other realtime attack, so the slot
// and pending flag stay paired. Mirrors the hostile/range gate of combat_rt_handle_input's
// left-click path: only a hostile combatant in weapon range is attacked; anything else is a
// no-op (the caller just returns, so combat_attack() is never reached). Returns true if intent
// was set.
bool combat_rt_player_attack(Object* target)
{
    if (!rt_enabled || obj_dude == NULL || target == NULL || target == obj_dude) {
        return false;
    }
    if (!rt_is_targetable(target) || !rt_is_hostile(target)) {
        return false;
    }
    int hitMode = rt_player_hit_mode();
    if (obj_dist(obj_dude, target) > item_w_range(obj_dude, hitMode)) {
        return false;
    }
    rt_set_player_intent(target, hitMode, rt_player_called_loc(hitMode));
    return true;
}

// Flight-recorder heartbeat: emit one TRACE_STATE snapshot of the realtime combat
// world + run cheap invariant health-checks (TRACE_WARN on a mismatch). Registered as
// the game_trace snapshot callback in main_init_system, fired ~1/sec by trace_tick.
// Mirrors anim.cc's ANIMATION_SAD_LIST_CAPACITY (not exported) for the anim-table gauge.
void combat_rt_trace_snapshot()
{
    if (!trace_enabled()) {
        return;
    }

    const int RT_ANIM_TABLE_CAP = 64; // keep in sync with anim.cc ANIMATION_SAD_LIST_CAPACITY

    trace_set_clock(rt_clock);

    int hostiles = 0;
    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].hostile) {
            hostiles++;
        }
    }

    int occ = -1, nr = -1, curr = -9;
    anim_debug_table(&occ, &nr, &curr);

    bool pending = combat_is_attack_pending();
    int dudeHp = obj_dude != NULL ? stat_level(obj_dude, STAT_CURRENT_HIT_POINTS) : -1;
    int dudeAnim = obj_dude != NULL ? FID_ANIM_TYPE(obj_dude->fid) : -1;

    // TEMP DIAG (STRIP with ac-diag): ac= is the LIVE armor class and eap= the engine combat.ap
    // it rides on. The interface readout only refreshes on intface_redraw/show, so the bar can
    // sit on a stale value; this once-a-second sample says what the number actually was.
    int dudeAc = obj_dude != NULL ? stat_level(obj_dude, STAT_ARMOR_CLASS) : -1;
    int dudeEap = obj_dude != NULL ? obj_dude->data.critter.combat.ap : -1;

    trace_event(TRACE_STATE, "hb",
        "eng=%d act=%d host=%d hp=%d ap=%d/%d ac=%d eap=%d lock=%d pend=%d inflight=%d anim=%d animOcc=%d/%d camlag=%d,%d",
        rt_engaged ? 1 : 0, rt_actor_count, hostiles, dudeHp,
        combat_rt_player_ap(), combat_rt_player_max_ap(),
        dudeAc, dudeEap,
        rt_lock_target != NULL ? 1 : 0, pending ? 1 : 0, rt_inflight_count,
        dudeAnim, occ, RT_ANIM_TABLE_CAP,
        (int)lroundf(rt_cam_lag_x), (int)lroundf(rt_cam_lag_y));

    // Invariant health-checks.
    if (occ >= 0 && occ >= RT_ANIM_TABLE_CAP - 8) {
        // Approaching the sad[] saturation that silently drops attack/death anims.
        trace_event(TRACE_WARN, "anim-near-full", "animOcc=%d/%d host=%d", occ, RT_ANIM_TABLE_CAP, hostiles);
    }

    // COMPREHENSIVE per-critter dump: everything relevant to the "floating / drawn-as-background /
    // fast walk / vanishing" reports, one line each. Fields:
    //   tile        = map tile (iso depth-sort key). Wrong tile => wrong z-order.
    //   x,y         = SUB-TILE pixel offset. Non-zero on a resting/dead body => "floating in air".
    //   fid         = full art id; anim = anim type; frame = current frame index.
    //   flat        = OBJECT_FLAT: drawn with the FLOOR (as background). 1 on a live/dying critter
    //                 => "dying animation / critter shows as background".
    //   hid         = OBJECT_HIDDEN: not drawn at all.
    //   dead/prone/busy = death + reaction + anim-busy state.
    //   st/tgt/dist/ap  = FSM state (0 IDLE 1 CHASE 2 ATTACK), target, range, AP.
    // The dude is dumped first (row DUDE) so his walk/render state can be compared directly against
    // the NPCs -- if the NPCs' fields match the dude's, they ARE moving/rendering like him.
    if (obj_dude != NULL) {
        trace_event(TRACE_STATE, "critter",
            "DUDE pid=%d tile=%d x=%d y=%d fid=%08x anim=%d frame=%d flat=%d hid=%d dead=%d prone=%d busy=%d rx=%s",
            obj_dude->pid, obj_dude->tile, obj_dude->x, obj_dude->y, obj_dude->fid,
            FID_ANIM_TYPE(obj_dude->fid), obj_dude->frame,
            (obj_dude->flags & OBJECT_FLAT) != 0 ? 1 : 0,
            (obj_dude->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            critter_is_dead(obj_dude) ? 1 : 0, critter_is_prone(obj_dude) ? 1 : 0,
            anim_busy(obj_dude), rt_react_name(rt_reaction_kind(obj_dude)));
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* o = a->obj;
        if (o == NULL) {
            continue;
        }
        Object* tgt = a->target;
        trace_event(TRACE_STATE, "critter",
            "id=%d pid=%d tile=%d x=%d y=%d fid=%08x anim=%d frame=%d flat=%d hid=%d dead=%d prone=%d busy=%d rx=%s "
            "st=%d host=%d frnd=%d tgt=%d dist=%d ap=%d",
            o->id, o->pid, o->tile, o->x, o->y, o->fid,
            FID_ANIM_TYPE(o->fid), o->frame,
            (o->flags & OBJECT_FLAT) != 0 ? 1 : 0,
            (o->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            critter_is_dead(o) ? 1 : 0, critter_is_prone(o) ? 1 : 0, anim_busy(o),
            rt_react_name(rt_reaction_kind(o)),
            a->state, a->hostile ? 1 : 0, a->friendly ? 1 : 0,
            tgt != NULL ? tgt->id : 0,
            tgt != NULL ? obj_dist(o, tgt) : -1, a->ap);
    }
}


// --- MODS page live setters ---
//
// Both levers are read from cfg once at init into the statics above. The MODS screen changes them
// while the game is running, so it needs a way in that does NOT go through a re-init (which would
// also reset the fight in progress). Deliberately plain assignments with the same clamps the cfg
// loader applies -- the screen owns persistence (it writes the cfg and saves), these own the live
// value, and nothing here can be reached before combat_rt_init has run.
void combat_rt_set_knockdown_agility(float factor)
{
    rt_knockdown_agility_factor = factor < 0.0f ? 0.0f : (factor > 0.15f ? 0.15f : factor);
}

float combat_rt_get_knockdown_agility()
{
    return rt_knockdown_agility_factor;
}

void combat_rt_set_door_magnet(float strength)
{
    rt_door_magnet = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
}

float combat_rt_get_door_magnet()
{
    return rt_door_magnet;
}

void combat_rt_set_free_move(bool enabled)
{
    if (!enabled) {
        rt_fm_release();
    }
    rt_free_move = enabled;
}

// WHERE THE DUDE STANDS under free movement: the offset the free mover last left him at, on the tile
// it was measured from. dude_stand (anim.cc) puts him back there instead of on the tile centre.
//
// ⚠️ EVERY ONE OF HIS ANIMATIONS ENDS IN dude_stand, AND dude_stand RE-CENTRES ON THE TILE. Walking
// never hit it (rt_fm_release stands him in place on purpose) and his swings had rt_swing_keep, but a
// weapon or hand swap, a reload, an injection, a pickup all did: he stood half a tile off-centre --
// that is what free movement is -- and the end of the animation threw him back onto the centre, up
// to ~22 px in one frame. "Prohodil jsem zbraň a postava se pohnula" (2026-10-08): in his trace he
// stood at x=22 when Y swapped hands.
//
// False -- vanilla behaviour -- when free movement is off, or the dude is on another tile than the one
// the offset was measured on (knocked back, moved by a script, a fresh map).
bool combat_rt_dude_rest_offset(int* x, int* y)
{
    if (!rt_free_move || !combat_rt_enabled() || obj_dude == NULL
        || rt_fm_rest_tile == -1 || obj_dude->tile != rt_fm_rest_tile) {
        return false;
    }

    *x = (int)lroundf(rt_fm_x);
    *y = (int)lroundf(rt_fm_y);
    return true;
}

bool combat_rt_get_free_move()
{
    return rt_free_move;
}

void combat_rt_set_npc_slide(bool enabled)
{
    rt_npc_slide = enabled;
}

bool combat_rt_get_npc_slide()
{
    return rt_npc_slide;
}

void combat_rt_set_look_around(bool enabled)
{
    rt_look_around = enabled;
}

bool combat_rt_get_look_around()
{
    return rt_look_around;
}

void combat_rt_set_script_turns(bool enabled)
{
    rt_script_turns = enabled;
}

bool combat_rt_get_script_turns()
{
    return rt_script_turns;
}

} // namespace fallout
