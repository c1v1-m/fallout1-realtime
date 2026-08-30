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
#define RT_AP_REGEN_SECONDS 0.7f

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

    // rt_clock when this critter's knockdown FALL finished (it's down and idle, ready to stand).
    // A short dwell (RT_KNOCKDOWN_DWELL_SECONDS) is held from here before rt_stand_up fires, so a
    // knockdown actually leaves the critter on the ground for a readable beat instead of the fall
    // flowing straight into the standup with no time down. -1 = not waiting to stand.
    float prone_since;

    // rt_clock when the CURRENT knockdown's fall finished -- the same instant prone_since is first
    // armed, but NEVER restarted by rt_bump_prone_dwell. This is the clock the hard ground cap
    // (RT_KNOCKDOWN_GROUND_CAP) runs on, so focus-fire can extend the dwell but can't pin the
    // critter down forever. -1 = not down.
    float prone_fall_end;

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
#define RT_ATTACK_COOLDOWN_HIGH_BIAS 0.6f
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

// Exponent on SEQUENCE in the strike-first roll (see GAME_CONFIG_SEQ_INITIATIVE_POWER_KEY).
// Loaded from cfg at combat_rt_init.
#define RT_SEQ_INITIATIVE_POWER 2.0f
static float rt_seq_initiative_power = RT_SEQ_INITIATIVE_POWER;

// How long the SLOWEST possible critter waits before its opening swing, in seconds (cfg
// `initiative_span`). 0 disables initiative entirely and restores the old "everyone ready at
// once" opening. See rt_initiative_delay.
// Directional-lock aiming (see rt_directional_lock). Cone half-angle as a cosine, how much distance
// may pull the choice off pure alignment, and how much better a rival must score before it takes the
// lock. The distance weight is per SCREEN PIXEL, hence the small number.
#define RT_LOCK_AIM_CONE_COS 0.35f
#define RT_LOCK_AIM_DIST_WEIGHT 0.0015f
#define RT_LOCK_AIM_SWITCH_MARGIN 0.08f

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
#define RT_REGEN_FAR_MULT_MOVING 1.0f  // multiplier at/beyond the falloff WHILE MOVING
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
static float rt_speed_mult = 1.0f;

// Escape-delay: in realtime you can walk onto a map-exit grid mid-combat, but to stop you
// instantly fleeing a losing fight you must HOLD POSITION on the exit grid for this many
// seconds of live combat before the map changes. Stepping off the grid cancels it; combat
// ending early lets you leave at once. Armed by combat_rt_arm_map_exit (from map_leave_map).
#define RT_MAP_EXIT_DELAY 10.0f
static bool rt_map_exit_armed = false;
static float rt_map_exit_timer = 0.0f;
static int rt_map_exit_last_shown = -1; // last whole-second countdown printed (-1 = none)
// Set true once the escape timer expires and combat is torn down for the queued map
// transition. While set, combat_rt_update must NOT refresh the registry or re-arm the warm
// bit: the lingering hostiles would re-engage and re-set COMBAT_STATE_0x01 within the same
// frame, before map_check_state (gated on !isInCombat()) could fire -- stranding the player
// on the exit grid forever (the "waited 10s and nothing happened" bug). Cleared when the map
// actually unloads (combat_rt_notify_objects_freed) or combat is reset.
static bool rt_map_exit_committed = false;
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

// Right-stick lock-on target (gamepad aiming). When set, the A button attacks
// this critter instead of the nearest one; NULL = auto-target nearest. Acquired
// by sweeping the aim cursor over a hostile, cleared by R3 or when the target
// dies / leaves. Validated against the actor registry before any dereference so
// a freed object can never be touched.
static Object* rt_lock_target = NULL;

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

// Seconds the dude has been in an uninterrupted hit-react flinch, for the
// RT_FLINCH_MAX_SECONDS auto-cancel backstop. Reset whenever he isn't flinching.
static float rt_dude_flinch_time = 0.0f;

// rt_clock when the dude's knockdown fall finished (down and idle). Holds the same
// RT_KNOCKDOWN_DWELL_SECONDS beat as the per-NPC prone_since before he stands up. -1 = not waiting.
static float rt_dude_prone_since = -1.0f;

// The dude's counterpart to prone_fall_end: when his fall finished, never restarted by a hit. Runs
// the hard ground cap. -1 = not down.
static float rt_dude_prone_fall_end = -1.0f;

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
// normal (non-aimed) attack. Set by the L3 aimed-shot wheel via combat_rt_aimed_attack.
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
            trace_event(TRACE_COMBAT, "cripple",
                "pid=%d id=%d dude=%d legL=%d legR=%d armL=%d armR=%d blind=%d legsNow=%d t=%.2f",
                obj->pid, obj->id, obj == obj_dude ? 1 : 0,
                (crip & DAM_CRIP_LEG_LEFT) != 0 ? 1 : 0, (crip & DAM_CRIP_LEG_RIGHT) != 0 ? 1 : 0,
                (crip & DAM_CRIP_ARM_LEFT) != 0 ? 1 : 0, (crip & DAM_CRIP_ARM_RIGHT) != 0 ? 1 : 0,
                (crip & DAM_BLIND) != 0 ? 1 : 0, legs, rt_clock);
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

    // Still perceives the thing it is actually fighting -> stay on it. Checked before
    // ai_danger_source because our target and vanilla's "danger" need not be the same object
    // (we let a pack split across the dude and his followers).
    if (rt_is_combatant(target) && is_within_perception(npc, target)) {
        return false;
    }

    // Otherwise ask vanilla's own question: is there ANY danger left that it can perceive?
    Object* danger = ai_danger_source(npc);
    return danger == NULL || !is_within_perception(npc, danger);
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
static float rt_attack_cooldown(Object* attacker, int hitMode)
{
    if (attacker == NULL || rt_attack_cooldown_base <= 0.0f) {
        return 0.0f;
    }

    int stat = STAT_STRENGTH;
    Object* weapon = item_hit_with(attacker, hitMode);
    if (weapon != NULL) {
        int type = item_w_subtype(weapon, hitMode);
        if (type == ATTACK_TYPE_RANGED) {
            stat = STAT_PERCEPTION;
        } else if (type == ATTACK_TYPE_THROW) {
            stat = STAT_AGILITY;
        }
    }

    // Same shape as ANIM_ATTACK_STAT_HIGH_BIAS: points above average are worth more, points below
    // average keep the plain factor so a weak character is not punished twice over.
    int delta = stat_level(attacker, stat) - 5;
    float factor = delta > 0 ? rt_attack_cooldown_stat_factor * rt_attack_cooldown_high_bias
                             : rt_attack_cooldown_stat_factor;
    float cd = rt_attack_cooldown_base * (1.0f - delta * factor);

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
    if (rt_attack_cooldown_skill_factor > 0.0f) {
        // item_w_skill_level resolves the weapon's own skill, and falls back to SKILL_UNARMED when
        // the hand is empty -- so this is correct for guns, melee and fists without a table here.
        int skill = item_w_skill_level(attacker, hitMode);
        float skillMult = 1.0f - ((float)skill - 100.0f) / 100.0f * rt_attack_cooldown_skill_factor;
        // Clamp the MULTIPLIER, not the skill: a modded/console skill far outside 0-200 must not be
        // able to invert the term (a negative multiplier would hand back a negative cooldown).
        if (skillMult < 1.0f - rt_attack_cooldown_skill_factor) {
            skillMult = 1.0f - rt_attack_cooldown_skill_factor;
        } else if (skillMult > 1.0f + rt_attack_cooldown_skill_factor) {
            skillMult = 1.0f + rt_attack_cooldown_skill_factor;
        }
        cd *= skillMult;
    }

    if (cd < RT_ATTACK_COOLDOWN_MIN) {
        cd = RT_ATTACK_COOLDOWN_MIN;
    }
    return cd;
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
        int cost = rt_ap_cost(obj_dude, rt_player_hit_mode());
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

    // The Combat Speed slider shortens the interval (see rt_speed_mult). Floored so a
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
// The Combat Speed slider scales the cost in step with the regen it is netted against, so the
// sprint lasts the same number of AP but a proportionally shorter wall-clock time.
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
    // install behaves exactly as before) to rt_combat_speed_ap_max at slider 50.
    rt_speed_mult = 1.0f + (combatSpeed / RT_COMBAT_SPEED_SLIDER_MAX) * (rt_combat_speed_ap_max - 1.0f);
    if (rt_speed_mult < 1.0f) {
        rt_speed_mult = 1.0f;
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
    // pistol. Neither reading is right. Fall back to the middle of the animation instead: the shot
    // lands mid-swing like every authored one does, and a rifle stops being the fastest weapon in
    // the game by accident. The pistols are unaffected -- they have real action frames (~0.10 s).
    if (actionFrame <= 0) {
        CacheEntry* h2;
        Art* a2 = art_ptr_lock(attacker->fid, &h2);
        if (a2 == NULL) {
            return false;
        }
        int frames = art_frame_max_frame(a2);
        art_ptr_unlock(h2);
        if (frames <= 1) {
            return false;
        }
        actionFrame = frames / 2;
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
static bool rt_is_terminal_death_anim(int anim)
{
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
    if (dt <= 0) {
        return true; // no armor of this type -> flinch on any damage
    }

    return (float)dmg >= dt * rt_flinch_dt_factor;
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
    // move) so this hit staggers it -- getting hit suppresses you.
    anim_force_clear(defender);

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "hit-react")) {
        return;
    }
    register_object_animate(defender, ANIM_HIT_FROM_FRONT, 0);
    register_end();
}

// True if a knocked-down critter is resting prone and idle, so it can be stood
// back up (its fall animation has finished and it isn't dead/out/skipping). Used
// by the defensive-standup passes for both the NPCs and the dude.
// True when a critter resting prone has waited long enough to get up: either its dwell ran out (the
// normal path) or the hard ground cap expired (the anti-pin backstop, see RT_KNOCKDOWN_GROUND_CAP).
// `proneSince` is the dwell clock, restarted by every damaging hit; `fallEnd` is when the fall
// finished and is NEVER restarted, which is what makes the cap a real ceiling. `capped` reports
// which of the two fired, for the trace.
static bool rt_prone_ready_to_stand(float proneSince, float fallEnd, bool* capped)
{
    *capped = false;

    if (rt_clock - proneSince >= rt_knockdown_dwell) {
        return true;
    }

    if (rt_knockdown_ground_cap > 0.0f && fallEnd >= 0.0f
        && rt_clock - fallEnd >= rt_knockdown_ground_cap) {
        *capped = true;
        return true;
    }

    return false;
}

static bool rt_needs_standup(Object* critter)
{
    // NOTE: intentionally NOT gated on DAM_LOSE_TURN. Every knockdown sets that flag
    // alongside DAM_KNOCKED_DOWN (set_new_results), and it is cleared ONLY by combat_turn
    // (the turn scheduler, combat.cc:2225) -- which never runs under combat_rt. Gating on it
    // left a knocked-DOWN critter prone FOREVER in realtime: it looked dead, couldn't be
    // looted, and stopped fighting. A knocked-OUT critter is different -- it's genuinely
    // unconscious with a game-time recovery event (queue EVENT_TYPE_KNOCKOUT -> critter_wake_up),
    // so leave it down and let the queue wake it; hence the DAM_KNOCKED_OUT guard instead.
    // Deliberately NOT gated on rt_is_combatant any more. That required the critter to be an active
    // hostile/follower, so a BYSTANDER knocked down by a stray shot or a blast was never told to get
    // up: nothing else in the engine stands a critter up in realtime, so it just lay there. Trace
    // 2026-08-02 caught it exactly -- id=1224 (st=RT_IDLE, host=0, frnd=0, ap=7) was knocked down at
    // s=121.7, its fall finished at s=123.8, and it was still on the ground at s=129.8 when it died:
    // six seconds against a 0.6s design dwell, and the ONLY FLOW/standup in the whole log belonged to
    // an engaged follower. Anyone alive and knocked down gets back up now.
    return rt_is_critter(critter)
        && !critter_is_dead(critter)
        && (critter->data.critter.combat.results & DAM_KNOCKED_DOWN) != 0
        && (critter->data.critter.combat.results & DAM_KNOCKED_OUT) == 0
        && anim_busy(critter) == 0;
}

// Restarts the ground dwell of a critter that is ALREADY knocked down, so the knockdown lasts
// RT_KNOCKDOWN_DWELL_SECONDS from the LAST hit instead of from the fall. Without it the standup
// timer ran on regardless of what was being done to the body: the trace had a shot commit at
// t=10.41 and the guard stand up at t=10.42 -- "I shot him while he was on the ground and he got
// straight back up." Shooting a downed enemy now keeps him down. Note a hit landing DURING the
// fall changes nothing (the standup pass holds prone_since at -1 until the fall ends, and the
// fall's own duration already covers that window) -- this only extends the resting-prone beat.
static void rt_bump_prone_dwell(Object* critter)
{
    if (critter == NULL || !rt_is_critter(critter) || critter_is_dead(critter)) {
        return;
    }

    if ((critter->data.critter.combat.results & DAM_KNOCKED_DOWN) == 0) {
        return;
    }

    if (critter == obj_dude) {
        rt_dude_prone_since = rt_clock;
        return;
    }

    for (int i = 0; i < rt_actor_count; i++) {
        if (rt_actors[i].obj == critter) {
            rt_actors[i].prone_since = rt_clock;
            return;
        }
    }
}

// Tiles per continuous NPC walk segment. A single-tile move restarts the walk sad every tile,
// which drops the tail of the leg cycle (choppy) and reads as "NPCs walk weird / fast / not like
// me". Moving several clear tiles in one sad keeps the walk cycle running unbroken across tiles --
// exactly like the dude's stick movement -- so the NPC walk matches the player's.
#define RT_NPC_WALK_SEGMENT 4

// Queues a CONTINUOUS multi-tile chase segment from mover straight toward target (as far as the
// straight run is clear, capped at RT_NPC_WALK_SEGMENT, stopping one tile short of the target). The
// FSM re-plans when the segment ends, so it still tracks a moving target while moving smoothly.
static void rt_step_toward(Object* mover, Object* target)
{
    int rotation = tile_dir(mover->tile, target->tile);

    int dist = 0;
    int dest = mover->tile;
    for (int i = 0; i < RT_NPC_WALK_SEGMENT; i++) {
        int next = tile_num_in_direction(dest, rotation, 1);
        if (next == target->tile) {
            break; // don't step onto the target's tile
        }
        if (obj_blocking_at(mover, next, mover->elevation) != NULL) {
            break; // straight run blocked here -- CHASE re-plans / pathfinds around next frame
        }
        dest = next;
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

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "step")) {
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

        if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "strafe")) {
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

        Object* attacker = victim->data.critter.combat.whoHitMe;
        if (attacker == NULL || attacker == guard || !rt_is_combatant(attacker)) {
            continue;
        }
        if (attacker->data.critter.combat.team == team) {
            continue; // an inside scuffle -- not ours to police
        }

        if (is_within_perception(guard, attacker)) {
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
    if (rt_register_begin(ANIMATION_REQUEST_RESERVED, "medic")) {
        register_object_animate(npc, ANIM_MAGIC_HANDS_MIDDLE, 0);
        register_end();
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
static bool rt_advance_to_target(Object* npc, Object* target)
{
    // Movement spends the engine combat.ap per tile while the warm bit is set; top it
    // up (movement is free in combat_rt's model) so the hop isn't truncated short.
    npc->data.critter.combat.ap = stat_level(npc, STAT_MAXIMUM_ACTION_POINTS);

    if (!rt_register_begin(ANIMATION_REQUEST_RESERVED, "reposition")) {
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
    register_end();

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
                Object* emptyWeapon = item_hit_with(attacker, hitMode);
                if (emptyWeapon != NULL) {
                    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_OUT_OF_AMMO, emptyWeapon, hitMode, NULL);
                    gsound_play_sfx_file(sfx);
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "%s", "Out of ammo -- press X to reload!");
                display_print(buf);
            }
        }

        return false;
    }

    // Attacks animate at normal engine speed: the AP pool alone paces them (a
    // costlier attack drains more of the pool and waits longer to refill), so
    // there's no cooldown-derived slow-down to apply.
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
        trace_event(TRACE_WARN, "attack-fail",
            "%s->pid=%d rc=%d defBusy=%d atkBusy=%d atkBlk=%d dist=%d range=%d oor=%d t=%.2f",
            attacker == obj_dude ? "DUDE" : "NPC", defender != NULL ? defender->pid : -1,
            rc, defender != NULL ? anim_busy(defender) : -9,
            anim_busy(attacker), anim_busy_blocking(attacker),
            dfDist, dfRange, (dfDist >= 0 && dfDist > dfRange) ? 1 : 0, rt_clock);
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
// toward rotation, running if `run` else walking. make_path runs when the
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
    // clear, then register a move to THAT tile. This keeps stick movement going in a straight
    // line and stopping AT a wall, instead of register_object_move_to_tile's make_path routing
    // the dude AROUND the obstacle on its own -- which reads as "I aimed at the wall but the
    // dude walked off on a path I didn't ask for". 0 clear tiles == immediately blocked, so we
    // don't move and the dude just stands against the wall (the append path extends straight).
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

// Called every poll frame while the stick is held. Starts a segment when the
// dude is idle. While our own segment is in flight and the stick still points
// the same way at the same gait, it APPENDS more steps to the live animation
// (anim_extend_move) one tile before it runs out -- this keeps the walk/run
// frame cycle advancing instead of restarting (which a clear+re-register
// would). A direction or gait change still does clear+restart (the art changes
// there anyway, so a fresh cycle looks natural).
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

    bool changed = rotation != rt_stick_rotation || run != rt_stick_running;
    bool nearEnd = !rt_stick_short && tile_dist(obj_dude->tile, rt_stick_dest) <= 1;

    if (!changed && nearEnd) {
        // Same heading: append to the live sad so the cycle never resets.
        int rc = anim_extend_move(obj_dude, rotation, RT_STICK_SEGMENT_TILES);
        if (rc == 0) {
            rt_stick_dest = tile_num_in_direction(rt_stick_dest, rotation, RT_STICK_SEGMENT_TILES);
            return;
        }
        // Append failed (sad ended between frames, or capacity); restart below.
        changed = true;
    }

    if (changed) {
        rt_stick_active = false;
        // Pending-only: this must end the dude's live WALK, never an older attack sequence
        // that is still animating the corpse he just made (that froze bodies mid-fall).
        if (anim_clear_pending(obj_dude) == 0) {
            rt_stick_segment_start(rotation, run);
        }
    }
}

// Called when the stick returns to the deadzone: stop at the current tile
// instead of walking out the rest of the segment.
static void rt_stick_stop()
{
    rt_move_stuck_since = -1.0f; // stick released: no longer being refused
    if (rt_stick_active) {
        rt_stick_active = false;
        if (anim_busy_blocking(obj_dude) != 0) {
            anim_clear_pending(obj_dude);
        }
    }
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

static Object* rt_nearest_lockable()
{
    Object* best = NULL;
    int bestDist = INT_MAX;
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
        if (dist < bestDist) {
            bestDist = dist;
            best = critter;
        }
    }

    return best;
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

// Switches the lock to the lockable enemy that best matches the pushed right-stick DIRECTION on
// screen, measured from the DUDE (so "up" = the enemy above your character, regardless of who is
// currently locked). SDL stick (sx,sy) and screen coords are both +x right / +y down, so the
// stick vector maps straight to screen direction. Picks the enemy with the highest direction
// alignment (within a cone), lightly preferring closer ones. No-op if nothing suitable lies that
// way.
// ABSOLUTE aim, called EVERY FRAME the stick is out: the direction names where the enemy is
// relative to YOU, so sweeping the stick sweeps the lock round with it. Returns true only when the
// lock actually moved, so the caller blips once per change.
//
// ⚠️ THIS REPLACED A STEP MODEL (flick -> one neighbour -> re-centre), anchored on the LOCKED
// TARGET rather than the dude. That anchoring was itself a fix (2026-08-10): from the dude, a group
// bunched 20 tiles away sits at nearly one angle, the cone cannot separate them, and most pushes
// silently found nothing. Going back to a dude anchor re-opens exactly that case -- so the two
// guards below are not decoration, they are what makes it survivable:
//
//   * the distance term is SMALL, so alignment still decides and it only settles near-ties (the
//     nearest of a co-linear pair wins rather than the selection jumping to the back of the group);
//   * hysteresis stops two similarly-placed enemies trading the lock every frame.
//
// Genuinely co-linear enemies are still hard to separate this way -- one sidestep changes the
// angles. That is the price of matching the interact picker's feel, which is what was asked for.
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

    Rect cb;
    obj_bound(obj_dude, &cb);
    float cx = (cb.ulx + cb.lrx) * 0.5f;
    float cy = (cb.uly + cb.lry) * 0.5f;

    Object* best = NULL;
    float bestScore = -1.0e9f;
    float currentScore = 0.0f;
    bool haveCurrent = false;

    // Same Perception reach as R3 itself. Without it the stick could flick the lock onto someone
    // R3 would refuse to lock in the first place -- two different answers to "can this character
    // pick that one out", which is the sort of split that reads as a bug.
    int switchMaxDist = rt_lock_range();

    for (int i = 0; i < rt_actor_count; i++) {
        Object* c = rt_actors[i].obj;
        if (!rt_is_lockable(c)) {
            continue;
        }

        // The CURRENT target is exempt: it must always be scoreable, or a target that drifted out
        // of range mid-fight could never be compared against and the lock would judder.
        if (c != rt_lock_target && obj_dist(obj_dude, c) > switchMaxDist) {
            continue;
        }

        Rect b;
        obj_bound(c, &b);
        float ex = (b.ulx + b.lrx) * 0.5f - cx;
        float ey = (b.uly + b.lry) * 0.5f - cy;
        float elen = sqrtf(ex * ex + ey * ey);
        if (elen < 0.0001f) {
            continue;
        }

        float dot = (ex / elen) * sx + (ey / elen) * sy; // cos of angle to the pushed direction
        if (dot < RT_LOCK_AIM_CONE_COS) {
            continue;
        }

        float score = dot - elen * RT_LOCK_AIM_DIST_WEIGHT;

        if (c == rt_lock_target) {
            currentScore = score;
            haveCurrent = true;
        }

        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }

    if (best == NULL || best == rt_lock_target) {
        return false;
    }

    if (haveCurrent && bestScore - currentScore < RT_LOCK_AIM_SWITCH_MARGIN) {
        return false;
    }

    rt_lock_target = best;
    return true;
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
            a->run_winded = prev->run_winded;
            a->breather = prev->breather;
            a->breather_since = prev->breather_since;
            a->last_results = prev->last_results;
            a->weapon_checked = prev->weapon_checked;
            a->dead_since = prev->dead_since;
            a->prone_since = prev->prone_since;
            a->prone_fall_end = prev->prone_fall_end;
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
            a->run_winded = false;
            a->breather = false;
            a->breather_since = -1.0f;
            // Seed from the current results so a critter first seen already
            // dead/downed doesn't register a phantom event.
            a->last_results = obj->data.critter.combat.results;
            a->weapon_checked = false;
            a->dead_since = -1.0f;
            a->prone_since = -1.0f;
            a->prone_fall_end = -1.0f;
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
    if (rt_is_critter(npc) && !critter_is_dead(npc) && !rt_defender_in_flight(npc)
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
        bool strayOffset = npc->x > RT_CORPSE_FALL_OFFSET_MAX || npc->x < -RT_CORPSE_FALL_OFFSET_MAX
            || npc->y > RT_CORPSE_FALL_OFFSET_MAX || npc->y < -RT_CORPSE_FALL_OFFSET_MAX;

        // ...and a THIRD: a fall that was cut before its last frame (see rt_death_fall_was_cut --
        // the dialogue-interrupts-a-death case). Its leftover offset can be well UNDER the stray
        // threshold above, because the body simply never travelled the rest of the way, so
        // neither of the other two tests sees it. Grounding it puts the body on its tile instead
        // of part-way through a fall it will never finish.
        if ((strandedDeadAnim || strayOffset || rt_death_fall_was_cut(npc))
            && (npc->x != 0 || npc->y != 0) && anim_busy(npc) == 0) {
            Rect off;
            obj_offset(npc, -npc->x, -npc->y, &off);
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
            if (a->dead_since < 0.0f) {
                a->dead_since = rt_clock;
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

    // Defensive standup: with no turns to stand it up on, a knocked-down critter
    // would stay prone forever (and leak the hit-resolution slot if it was the
    // resolving attacker). Once its fall animation has finished, get it back up --
    // but hold a short dwell first (RT_KNOCKDOWN_DWELL_SECONDS) so the knockdown
    // actually leaves it on the ground for a readable beat instead of the fall
    // flowing straight into the standup. prone_since is armed the frame the fall
    // ends (rt_needs_standup requires anim_busy==0) and reset whenever it's no
    // longer waiting to stand (stood up, or a new anim/knockdown restarted the wait).
    if (rt_needs_standup(npc)) {
        if (a->prone_since < 0.0f) {
            a->prone_since = rt_clock;
        }
        // Stamped SEPARATELY from prone_since, and only once per knockdown -- see the note on the
        // hold branch below. prone_since is restarted by every hit (rt_bump_prone_dwell); this is
        // the anchor the anti-pin cap measures from and it must survive that.
        if (a->prone_fall_end < 0.0f) {
            a->prone_fall_end = rt_clock;
        }
        bool capped = false;
        if (rt_prone_ready_to_stand(a->prone_since, a->prone_fall_end, &capped)) {
            if (capped) {
                trace_event(TRACE_FLOW, "prone-cap", "id=%d pid=%d ground=%.2f dwellLeft=%.2f t=%.2f",
                    npc->id, npc->pid, rt_clock - a->prone_fall_end,
                    rt_knockdown_dwell - (rt_clock - a->prone_since), rt_clock);
            }
            rt_stand_up(npc);
            a->prone_since = -1.0f;
            a->prone_fall_end = -1.0f;
        }
    } else if (!critter_is_prone(npc)) {
        a->prone_since = -1.0f;
        a->prone_fall_end = -1.0f;
    }
    // ...otherwise STILL DOWN but momentarily not standup-eligible -- HOLD both timers. See the
    // long note at the dude's copy of this pass; the bug and the fix are identical.

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
    rt_regen_ap(npc, &a->ap, &a->ap_regen, stat_level(npc, STAT_MAXIMUM_ACTION_POINTS),
        dt * rt_proximity_regen_mult(npcEnemyDist, npcMoving, rt_regen_moving_npc));

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
    {
        int reserve = rt_attack_reserve_ap(npc);
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
        bool breatherAllowed = a->state == RT_CHASE
            && npcEnemyDist >= 0 && npcEnemyDist <= rt_npc_breather_tiles;

        if (npcInFight && breatherAllowed && !a->breather && a->ap <= reserve) {
            a->breather = true;
            a->breather_since = rt_clock;
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

    // HEAL BEFORE FIGHTING, which is where vanilla puts it too: ai_check_drugs runs at the top of
    // combat_ai (combatai.cc:1207), before the target/attack decision. Sits OUTSIDE the state
    // switch on purpose -- a critter bleeding out mid-CHASE should stop and patch itself, not have
    // to reach weapon range first. Returning here consumes the beat, exactly like the reload branch
    // inside RT_ATTACK: the animation it just registered will hold anim_busy next frame anyway.
    if (a->state != RT_IDLE && rt_try_medic(a, npc)) {
        return;
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
            break;
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
            if (tgt != NULL && (is_within_perception(npc, tgt) || hurtBy == tgt)) {
                a->target = tgt;
                a->state = RT_CHASE;
                a->unperceived_since = -1.0f;
            }
        } else if (a->friendly) {
            // Follower: pick the nearest of the dude's enemies and engage it. The same
            // CHASE/ATTACK states + arbiter then drive the attack (target-agnostic), and the
            // concurrent resolver lets the follower fight an enemy while the dude fights another.
            Object* enemy = rt_nearest_hostile(npc);
            if (enemy != NULL && is_within_perception(npc, enemy)) {
                a->target = enemy;
                a->state = RT_CHASE;
                a->unperceived_since = -1.0f;
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
                rt_note_path_result(a, rt_advance_to_target(npc, a->target));
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
                    rt_note_path_result(a, rt_advance_to_target(npc, a->target));
                } else {
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
    rt_lock_target = NULL;
    rt_lock_held = 0.0f;
    rt_player_intent_target = NULL;
    rt_player_intent_mode = 0;
    rt_player_intent_ttl = 0.0f;
    rt_player_intent_loc = HIT_LOCATION_UNCALLED;
    rt_pending_loot = NULL;
    rt_pending_loot_kind = 0;
    rt_map_exit_armed = false;
    rt_map_exit_timer = 0.0f;
    rt_map_exit_last_shown = -1;
    rt_map_exit_committed = false;
    rt_stick_active = false;
    rt_stick_short = false;
    rt_stick_running = false;
    rt_stick_rotation = -1;
    rt_stick_dest = -1;
    rt_stick_last_sector = -1;
}

// Full per-session state reset, shared by init / exit / game-reset so every teardown path
// clears the SAME runtime state (the old hand-maintained per-function lists had drifted --
// exit forgot rt_refresh_timer, and none reset the stick/camera/flinch state, which was the
// source of the teardown/reload bug family). Does NOT load config (init does that after),
// set rt_enabled (each caller does), or touch combat_state (game-reset clears the warm bit).
static void rt_reset_session_state()
{
    rt_abandon_encounter();

    // A roll in flight is animating the PREVIOUS session's hit points; nothing ticks it once
    // combat_rt is torn down, so it would sit on a stale number over the new game's bar.
    intface_hp_roll_cancel();

    rt_refresh_timer = 0.0f;
    rt_dude_flinch_accum = 0.0f;
    rt_dude_immune_until = 0.0f;
    rt_player_ap = INT_MAX; // clamped to the dude's max AP on the first update
    rt_player_ap_regen = 0.0f;
    rt_player_next_attack_at = 0.0f;
    rt_player_weapon_up_until = 0.0f;
    rt_run_ap_drain = 0.0f;
    rt_walk_ap_drain = 0.0f;
    rt_run_blocked = false;
    rt_move_blocked = false;
    rt_last_hud_ap = -1;
    rt_last_hud_blocked = -1;
    rt_last_hud_ac = -1;
    rt_cam_last_tile = -1;
    rt_cam_last_off_x = 0;
    rt_cam_last_off_y = 0;
    rt_clock = 0.0f;
    rt_dude_flinch_time = 0.0f;
    rt_dude_prone_since = -1.0f;
    rt_dude_prone_fall_end = -1.0f;
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


    // How hard AGILITY bends that cadence per critter (0 = the old flat rate for everyone).
    // Capped at 0.3 so the spread stays inside the clamps in rt_regen_interval.
    double apRegenAgi = 0.09;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_AP_REGEN_AGILITY_FACTOR_KEY, &apRegenAgi);
    if (apRegenAgi < 0.0) {
        apRegenAgi = 0.0;
    } else if (apRegenAgi > 0.3) {
        apRegenAgi = 0.3;
    }
    rt_ap_regen_agility_factor = (float)apRegenAgi;

    // Physical rate of fire. 0 = disabled (previous behaviour exactly). Capped well above any sane
    // value so a typo can't leave a critter unable to attack for a whole fight.
    double atkCd = 1.0;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ATTACK_COOLDOWN_BASE_KEY, &atkCd);
    if (atkCd < 0.0) {
        atkCd = 0.0;
    } else if (atkCd > 5.0) {
        atkCd = 5.0;
    }
    rt_attack_cooldown_base = (float)atkCd;

    double atkCdStat = 0.09;
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

void combat_rt_poll_gamepad()
{
    if (obj_dude == NULL || !rt_is_combatant(obj_dude)) {
        return;
    }

    // While a modal menu/dialog has control, the sticks belong to the UI layer
    // (cursor + item snapping), not the player. Halt any in-flight stick walk
    // and skip all gameplay gamepad input.
    if (gamepad_ui_menu_active()) {
        rt_stick_stop();
        rt_stick_last_sector = -1;
        return;
    }

    // While the skill wheel (L3) OR the on-body aimed-shot picker (LB held) is up, the sticks drive
    // the selection (right = pick) and the player shouldn't walk or lock-switch; halt any in-flight
    // step and skip gameplay input (incl. the right-stick lock cycling further below).
    if (radial_wheel_is_open() || gamepad_ui_aim_body_active()) {
        rt_stick_stop();
        rt_stick_last_sector = -1;
        return;
    }

    // Borrow the shared controller handle owned by gamepad_ui.
    SDL_GameController* rt_gamepad = gamepad_ui_get_controller();
    if (rt_gamepad == NULL) {
        return;
    }

    SDL_GameControllerUpdate();

    // Left stick -> continuous walking in chained multi-tile segments while
    // held. The analog angle is bucketed into 6 sectors of 60 degrees, one per
    // hex rotation. SDL's Y axis is +down, so -y makes "up" positive; atan2
    // then gives up = +90 deg, right = 0 deg.
    float x = SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_LEFTX) / RT_GAMEPAD_AXIS_MAX;
    float y = SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_LEFTY) / RT_GAMEPAD_AXIS_MAX;

    // Left trigger held -> run instead of walk. IN COMBAT running drains the AP
    // pool (rt_run_ap_cost), so when it's empty -- or recovering while rt_run_blocked
    // -- the dude drops to a free walk. Feeding this effective gait into rt_stick_move
    // makes its gait-change path do the seamless run->walk restart (no stop/freeze:
    // movement itself is never gated, only the run/walk choice). Out of combat,
    // running is always free.
    // ⚠️ RUN IS ON LB, NOT LT (swapped with the aimed shot, 2026-08-29). LB is a digital button,
    // so there is no threshold to cross -- and that is the one behavioural difference the swap
    // brought: the trigger had a dead zone you could rest a finger inside, the shoulder does not.
    // The local stays named ltHeld because every gait test below reads it as "the run key".
    bool ltHeld = SDL_GameControllerGetButton(rt_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool run = ltHeld && (!rt_engaged || (rt_player_ap > 0 && !rt_run_blocked));

    // Flight-recorder movement input: emit on a direction/gait EDGE (a held stick coalesces
    // to nothing after the first line). TRACE_INPUT reflects what the stick ASKED for even
    // when AP-blocking stops the actual step, so a "why didn't I move" report is visible.
    static int rt_traced_stick_rot = -1;
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

            if (rt_step_is_free(sectorToRotation[nearSector])) {
                rotation = sectorToRotation[nearSector];
            } else if (rt_step_is_free(sectorToRotation[farSector])) {
                rotation = sectorToRotation[farSector];
            }

            if (rotation != sectorToRotation[sector]) {
                static float rt_slide_traced = -1.0f;
                if (rt_clock - rt_slide_traced >= 0.25f) {
                    rt_slide_traced = rt_clock;
                    trace_event(TRACE_INPUT, "npc-slide", "want=%d took=%d dev=%.1f blocker=%d dead=%d t=%.2f",
                        sectorToRotation[sector], rotation, dev, bystander->pid,
                        critter_is_dead(bystander) ? 1 : 0, rt_clock);
                }
            }
        }

        if (!rt_traced_stick_on || rotation != rt_traced_stick_rot || run != rt_traced_stick_run) {
            trace_event(TRACE_INPUT, "stick", "dir=%d gait=%s blocked=%d t=%.2f",
                rotation, run ? "run" : "walk", rt_move_blocked ? 1 : 0, rt_clock);
            rt_traced_stick_rot = rotation;
            rt_traced_stick_run = run;
            rt_traced_stick_on = true;
        }

        // Out of AP (walked/ran the pool dry): stop until it regenerates to the resume
        // threshold. Standing lets the pool climb back (see rt_move_blocked).
        if (rt_move_blocked && !rt_ap_empty_walks) {
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
    if (rt_lock_target != NULL && !OnScreen::test(rt_lock_target)) {
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

    // R3 (right-stick click) TOGGLES the lock: engage the nearest lockable enemy when unlocked,
    // and RELEASE the lock when already locked. Switching between enemies while locked is done by
    // flicking the RIGHT STICK toward the one you want (rt_directional_lock) -- R3 is a clean
    // lock/unlock, not a cycle. Edge-triggered = one press, one toggle. (The lock also drops on
    // its own when the target dies, flees past RT_LOCK_DROP_DIST, or combat ends.)
    static bool rt_prev_r3 = false;
    bool r3 = SDL_GameControllerGetButton(rt_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    if (r3 && !rt_prev_r3) {
        bool wasLocked = rt_lock_target != NULL;
        rt_lock_target = wasLocked ? NULL : rt_nearest_lockable();
        trace_event(TRACE_ACTION, wasLocked ? "lock-off" : "lock-on", "target=%d t=%.2f",
            rt_lock_target != NULL ? rt_lock_target->pid : -1, rt_clock);

        // ⚠️ SAY WHICH WAY THE TOGGLE WENT. Locking already announces itself (crosshair, outline,
        // the cursor snapping to the target); UNLOCKING was silent, so a press meant as "aim at
        // him" when you were already locked simply stopped aiming -- and the next A, no longer
        // committed to an attack, fell through to whatever the interact prompt was offering.
        // That is the "zamířil na npc R3 a stisknul áčko a místo střelby se dveře zavřely" report:
        // trace_2026-08-13_181943.log has lock-off at s=850.18 on the very frame of `rightstick
        // down`, and the door opening 0.33 s later. The arbitration was right; the player simply
        // could not tell he had disarmed it. Two different sounds make the state audible.
        gsound_play_sfx_file(wasLocked ? "iisxxxx1" : "ib1p1xx1");
    }
    rt_prev_r3 = r3;

    // While locked: aim the RIGHT STICK toward the enemy you want and the lock switches to the
    // best-matching enemy in that screen direction (rt_directional_lock) -- just point at who you
    // want. The cursor is glued to the locked critter's on-screen center every frame so the aim
    // point tracks it (the stick no longer free-aims while locked). Edge-triggered with a
    // re-center arm: one push steps exactly one target and a held stick doesn't spin.
    // Tracks whether WE forced the crosshair cursor for a lock, so we only revert
    // it on unlock (and don't clobber a crosshair the player set themselves).
    static bool rt_lock_set_crosshair = false;
    if (rt_lock_target != NULL) {
        float rx = SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_RIGHTX) / RT_GAMEPAD_AXIS_MAX;
        float ry = SDL_GameControllerGetAxis(rt_gamepad, SDL_CONTROLLER_AXIS_RIGHTY) / RT_GAMEPAD_AXIS_MAX;
        float rmag = sqrtf(rx * rx + ry * ry);
        // CONTINUOUS aiming -- no arm/re-centre ratchet. Sweep the stick and the lock sweeps with
        // it, matching the interact picker exactly so both selections feel like one gesture.
        // rt_directional_lock reports only real changes, so the blip and the trace line fire once
        // per switch rather than every frame the stick is held.
        if (rmag > RT_LOCK_CYCLE_THRESHOLD) {
            if (rt_directional_lock(rx, ry)) {
                gsound_play_sfx_file("ib1p1xx1");
                trace_event(TRACE_INPUT, "aim", "target=%d t=%.2f",
                    rt_lock_target != NULL ? rt_lock_target->pid : -1, rt_clock);
            }
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
    bool attack = SDL_GameControllerGetButton(rt_gamepad, SDL_CONTROLLER_BUTTON_A) != 0;
    // Only trace the attack intent on the A PRESS EDGE, not every held frame -- each actual fire is
    // already logged as [COMBAT/attack], so a per-frame ACTION line would be pure spam (the raw A
    // edge is in [INPUT/btn] too).
    static bool rt_prev_attack_btn = false;
    bool attackEdge = attack && !rt_prev_attack_btn;
    rt_prev_attack_btn = attack;
    if (attack && !gamepad_ui_a_interacting()) {
        int hitMode = rt_player_hit_mode();
        if (rt_lock_target != NULL) {
            if (obj_dist(obj_dude, rt_lock_target) <= item_w_range(obj_dude, hitMode)) {
                rt_set_player_intent(rt_lock_target, hitMode);
                if (attackEdge) {
                    trace_event(TRACE_ACTION, "attack", "target=%d hm=%d locked=1 t=%.2f",
                        rt_lock_target->pid, hitMode, rt_clock);
                }
            }
        } else {
            Object* target = rt_nearest_hostile_in_range(hitMode);
            if (target != NULL) {
                rt_set_player_intent(target, hitMode);
                if (attackEdge) {
                    trace_event(TRACE_ACTION, "attack", "target=%d hm=%d locked=0 t=%.2f",
                        target->pid, hitMode, rt_clock);
                }
            }
        }
    }

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
        // MODE is one press of B away, and the damage type is a matter of picking another weapon.
        if (trait_level(TRAIT_FAST_SHOT)) {
            return "Fast Shot: you can never make a called shot.";
        }

        int anim = item_w_anim(obj_dude, hitMode);
        if (anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS) {
            return "You can't aim a burst. Switch firing mode with B.";
        }

        return "This weapon can't be aimed.";
    }

    if (rt_aim_target() == NULL) {
        return "Nothing in range to aim at.";
    }

    return NULL;
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
static void rt_follow_cam()
{
    int offX = -obj_dude->x;
    int offY = -obj_dude->y;

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

float combat_rt_time_scale()
{
    // Floored at a small positive value rather than allowed to reach 0. A zero scale would stop
    // rt_clock dead while the picker is up, which is the full freeze this feature deliberately is
    // not -- and it would divide by zero in compute_tpf's caller.
    if (rt_enabled && gamepad_ui_aim_body_active()) {
        return rt_aim_slowmo < 0.05f ? 0.05f : rt_aim_slowmo;
    }
    return 1.0f;
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
    // the same factor and nothing goes out of step with anything else.
    dt *= combat_rt_time_scale();

    rt_clock += dt;

    // Step the dude's HP counter. Deliberately here, below the radial-wheel return and above
    // every other early-out: the counter is part of the combat model's presentation, so it
    // holds still exactly when the world does.
    intface_hp_roll_tick(dt);

    // Conversation just closed -> hold the NPCs for a beat (see rt_npc_hold_until). Detected as a
    // true->false edge on dialog_active() rather than hooked into gdialog, so it covers every way a
    // talk can end -- a reply, a script closing it, walking out of one.
    {
        static bool wasInDialog = false;
        bool inDialog = dialog_active();
        if (wasInDialog && !inDialog && rt_post_dialog_grace > 0.0f) {
            rt_npc_hold_until = rt_clock + rt_post_dialog_grace;
        }
        wasInDialog = inDialog;
    }

    // Mid-combat map-escape hold timer (anti-abuse): may end combat this frame so the queued
    // transition fires. Runs before the commit/registry work so a completed escape tears down
    // cleanly. While armed it keeps the player held via the normal engaged path.
    rt_update_map_exit(dt);

    // Escape granted: combat was torn down this frame for the queued map transition. Bail out
    // BEFORE the registry refresh / engagement recompute below -- otherwise the hostiles still
    // on this map instantly re-register and re-set COMBAT_STATE_0x01, so map_check_state (run
    // later this same frame, gated on !isInCombat()) never sees combat as ended and the
    // transition is missed (timer reaches 0, "map-exit-go" logs, but the map never changes).
    // Hold combat torn down until the destination map actually loads
    // (combat_rt_notify_objects_freed clears the flag from obj_remove_all).
    if (rt_map_exit_committed) {
        combat_state &= ~COMBAT_STATE_0x01;
        return;
    }

    // Sample the Combat Speed slider into rt_speed_mult once for the whole frame, ahead of
    // every AP regen and drain below (both the player's and, via rt_update_actor, the NPCs').
    rt_sample_speed_mult();

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
        rt_follow_cam();
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

        bool actionReached = !thrown && rt_attack_action_frame_reached(f->attacker);

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

        bool capped = elapsed >= RT_RESOLVE_MAX_SECONDS;
        // Don't let action-frame / swing-done / connect commit before the swing has had time to play
        // (and its SFX to sound) -- that produced "shadow attacks" (damage + AP, no visible swing)
        // when the defender was already reacting or the swing read as done instantly. Cap still wins.
        bool ready = elapsed >= RT_MIN_SWING_SECONDS;
        if (((swingDone || connected || actionReached || poseEnded) && ready) || capped) {
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
            if (heavyReaction && (f->ctd.attackerFlags & DAM_HIT) != 0 && f->ctd.defender != NULL) {
                // ALL of them, not one. anim_force_clear ends a single sequence, and a critter
                // killed mid-swing can be held by more than one -- its own attack plus whatever
                // else. One clear left it still owned, check_registry then refused the death fall,
                // and the salvage below flattened it into a blood pool with no fall at all
                // (traced: id=560 killed at anim=16 frame=0, death-settle in the same instant).
                for (int guard = 0; guard < 4 && anim_force_clear(f->ctd.defender) == 0; guard++) {
                }
            }
            // Called for EVERY hit, not just heavy ones: with the split on, the whole defender
            // block is skipped at fire time, so this is also what registers the weapon
            // drop/destroy callbacks and the show_death corpse settle.
            bool reactionRegistered = action_defender_reaction(&f->ctd);

            // Shield the freshly queued fall from every "not prone but animating = stray" test in
            // here until it has actually started (see rt_death_just_registered).
            if (reactionRegistered && heavyReaction) {
                rt_mark_death_registered(f->ctd.defender);
            }

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
                rt_play_hit_react(rAttacker, rDefender);
            }

            // A DAMAGING hit on a critter that's already down restarts its ground dwell, so it
            // stays prone RT_KNOCKDOWN_DWELL_SECONDS past this hit instead of standing up on the
            // old timer the instant the damage lands. A miss doesn't (rDmg > 0). There is no
            // visible reaction while prone -- one sprite can't lie down and flinch at once -- so
            // the hit reads through HP/damage text and the extended time on the ground.
            if (rDmg > 0) {
                rt_bump_prone_dwell(rDefender);
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

    // Defensive standup for the dude (same rationale as the per-actor pass in
    // rt_update_actor; the dude isn't in the registry). A knocked-down dude with no
    // turn to stand up on would stay prone forever -- and if he was the resolving
    // attacker his anim_busy never returns to 0, leaking the slot. This was the
    // run-1 freeze trigger in the watchdog log (player KNOCKDOWN before the wedge).
    if (rt_needs_standup(obj_dude)) {
        if (rt_dude_prone_since < 0.0f) {
            rt_dude_prone_since = rt_clock;
        }
        if (rt_dude_prone_fall_end < 0.0f) {
            rt_dude_prone_fall_end = rt_clock;
        }
        bool capped = false;
        if (rt_prone_ready_to_stand(rt_dude_prone_since, rt_dude_prone_fall_end, &capped)) {
            if (capped) {
                trace_event(TRACE_FLOW, "prone-cap", "DUDE ground=%.2f dwellLeft=%.2f t=%.2f",
                    rt_clock - rt_dude_prone_fall_end,
                    rt_knockdown_dwell - (rt_clock - rt_dude_prone_since), rt_clock);
            }
            rt_stand_up(obj_dude);
            rt_dude_prone_since = -1.0f;
            rt_dude_prone_fall_end = -1.0f;
        }
    } else if (!critter_is_prone(obj_dude)) {
        // Genuinely back on his feet (or never down): both timers are meaningless, clear them.
        rt_dude_prone_since = -1.0f;
        rt_dude_prone_fall_end = -1.0f;
    }
    // ⚠️ STILL DOWN, JUST NOT ELIGIBLE THIS FRAME -> HOLD BOTH TIMERS. This `else` used to be
    // unconditional, and clearing prone_fall_end here is what let a knockdown pin the player
    // INDEFINITELY. rt_needs_standup requires anim_busy == 0, so every hit-react taken while on
    // the ground flickered it false for a frame or two, and the reset moved the anchor the 3.4 s
    // RT_KNOCKDOWN_GROUND_CAP is measured from. The cap is the ANTI-PIN backstop -- and it was
    // being reset by exactly the thing it exists to bound, so the harder you were focus-fired the
    // more reliably it failed to save you.
    //
    // Measured, trace_2026-08-29_131744 s=3227.9: anim=20 held on frame 10 (the fall finished --
    // busy=0, so nothing was playing, the sprite was simply frozen) for 5+ seconds, with ZERO
    // FLOW/standup and ZERO prone-cap in the entire 56-minute log. It ended only because the map
    // changed. That is the "zasekne se mi jedna animace" report.
    //
    // The dwell restarting per hit is DELIBERATE (rt_bump_prone_dwell -- shooting a downed body
    // keeps it down). The cap is what makes that bounded, so the two must not share an anchor:
    // prone_since is the per-hit timer, prone_fall_end is stamped once and left alone.

    // Flinch cap backstop: even with no movement input, don't leave the dude
    // frozen in a hit-react or miss-dodge for its full length. Past
    // RT_FLINCH_MAX_SECONDS cancel it so he's free to act again quickly (a hit's
    // damage already committed at swing-connect and a dodge has none, so the tail
    // is cosmetic). Only those brief reactions are capped; knockdowns run their
    // course and are lifted by the standup pass above.
    if (rt_is_cancelable_reaction(obj_dude)) {
        rt_dude_flinch_time += dt;
        if (rt_dude_flinch_time >= RT_FLINCH_MAX_SECONDS) {
            // Pending-only: cancel the flinch the dude is actually stuck in, not an older
            // sequence that merely still lists his spent swing while a corpse drops.
            anim_clear_pending(obj_dude);
            rt_dude_flinch_time = 0.0f;
        }
    } else {
        rt_dude_flinch_time = 0.0f;
    }

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
        while (rt_run_ap_drain >= 1.0f && rt_player_ap > 0) {
            rt_player_ap--;
            rt_run_ap_drain -= 1.0f;
        }
        if (rt_player_ap <= 0) {
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
        while (rt_walk_ap_drain >= 1.0f && rt_player_ap > 0) {
            rt_player_ap--;
            rt_walk_ap_drain -= 1.0f;
        }
        if (rt_player_ap <= 0) {
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

    // Drive the interface bar's AP indicator from the real pool so the bulbs
    // light up one at a time as AP regenerates (and drop the instant an attack
    // spends them). Redraw only when the count changes to avoid per-frame blits.
    // intface_update_move_points itself reads combat_rt_player_ap() while
    // combat_rt is enabled, so the engine combat.ap (kept maxed for movement)
    // never reaches the bar.
    // The dude's sprite must hold what his hand holds. The swap animation that normally keeps
    // those in step is cancellable in realtime, so it is verified here instead of trusted.
    intface_reconcile_weapon_fid();

    int hudAp = combat_rt_player_ap();
    int hudBlocked = combat_rt_move_blocked() ? 1 : 0;
    if (hudAp != rt_last_hud_ap || hudBlocked != rt_last_hud_blocked) {
        rt_last_hud_ap = hudAp;
        rt_last_hud_blocked = hudBlocked;
        intface_update_move_points(hudAp, 0);
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

    // WASD tile movement (4 of the 6 iso directions for now).
    int rotation = -1;
    switch (keyCode) {
    case KEY_LOWERCASE_W:
    case KEY_UPPERCASE_W:
        rotation = ROTATION_NE;
        break;
    case KEY_LOWERCASE_S:
    case KEY_UPPERCASE_S:
        rotation = ROTATION_SW;
        break;
    case KEY_LOWERCASE_A:
    case KEY_UPPERCASE_A:
        rotation = ROTATION_W;
        break;
    case KEY_LOWERCASE_D:
    case KEY_UPPERCASE_D:
        rotation = ROTATION_E;
        break;
    }

    if (rotation != -1) {
        rt_player_move(rotation);
        return true;
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
                rt_set_player_intent(target, hitMode);
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
        // Flag the attacker hostile-to-dude now if it's already tracked (sticky); otherwise
        // the next registry refresh picks it up.
        trace_event(TRACE_FLOW, "notify-attack", "atk pid=%d team=%d party=%d dudeTeam=%d dist=%d t=%.2f",
            attacker->pid, attacker->data.critter.combat.team, isPartyMember(attacker) ? 1 : 0,
            obj_dude != NULL ? obj_dude->data.critter.combat.team : -1,
            obj_dude != NULL ? obj_dist(attacker, obj_dude) : -1, rt_clock);
        for (int i = 0; i < rt_actor_count; i++) {
            if (rt_actors[i].obj == attacker) {
                rt_actors[i].hostile = true;
                return;
            }
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
    if (removed == NULL || !combat_rt_enabled() || FID_TYPE(removed->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    // Drop any in-flight attack snapshot whose attacker OR defender is the critter being freed,
    // so its deferred commit can't run apply_damage against freed memory.
    rt_inflight_drop_obj(removed);

    // Drop combat_rt's own cached pointers.
    if (rt_lock_target == removed) {
        rt_lock_target = NULL;
    }
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
    rt_player_intent_target = NULL;
    rt_player_intent_ttl = 0.0f;
    rt_pending_loot = NULL;
    rt_pending_loot_kind = 0;
    rt_lock_held = 0.0f;
}

// Print a short combat-log line (escape-delay feedback). display_print takes a non-const
// char*, so format into a local buffer.
static void rt_escape_message(const char* fmt, int arg)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, arg);
    display_print(buf);
}

// Arm (or keep armed) the mid-combat map-exit hold timer. Called from map_leave_map when the
// dude steps onto an exit grid IN COMBAT (realtime). map_state already holds the destination;
// we just delay the transition by keeping the engine warm bit set (via the normal engaged
// path) until the timer elapses in combat_rt_update. Idempotent: re-stepping onto exit tiles
// won't reset an already-running countdown.
void combat_rt_arm_map_exit()
{
    if (rt_map_exit_armed) {
        return;
    }
    rt_map_exit_armed = true;
    rt_map_exit_timer = RT_MAP_EXIT_DELAY;
    rt_map_exit_last_shown = -1;
    trace_event(TRACE_FLOW, "map-exit-armed", "delay=%.0f t=%.2f", RT_MAP_EXIT_DELAY, rt_clock);
    rt_escape_message("Escaping... hold position (%d).", (int)RT_MAP_EXIT_DELAY);
}

// Per-frame escape-delay tick (called from combat_rt_update). The dude must HOLD STILL on an
// exit grid in combat for RT_MAP_EXIT_DELAY seconds; on expiry combat ends so map_check_state
// performs the queued transition (map_state already holds the destination).
//
// The escape stays ARMED across movement/stepping off -- it does NOT cancel on the first
// off-grid frame the way it used to. That one-shot cancel meant any nudge dropped the escape
// and required physically walking out of the zone and back in (a fresh map_leave_map trigger)
// to restart it -- exactly the "resets and never resumes" bug. Now: moving, or stepping off
// the grid, just RESETS the hold timer (anti-abuse: you must hold position) while keeping the
// escape armed, so it resumes on its own the moment the dude is standing still on the grid
// again. The destination only clears when combat ends away from the exit (so a fight won
// elsewhere can't teleport the dude to a stale exit).
static void rt_update_map_exit(float dt)
{
    // SELF-ARM (the "sometimes the countdown never starts" fix). The escape is normally armed by
    // map_leave_map at the instant the dude STEPS ONTO an exit grid -- but only if isInCombat()
    // was already true right then. In realtime the engaged flag flickers (it follows the hostile
    // registry, and combat can go cold for a beat and re-engage), so this race exists: step onto
    // the grid while combat reads COLD -> map_leave_map only STORES the destination and does not
    // arm -> combat re-engages before map_check_state (which is gated on !isInCombat) can run ->
    // the transition sits queued with no timer and no exit, and the only way out was to step off
    // the grid and back on while combat was hot. Arming from state rather than from the entry
    // EVENT closes it: engaged + standing on a grid + a destination already queued = start the
    // hold. Skipped once committed (the timer expired and we're waiting for the map to load).
    if (!rt_map_exit_armed && !rt_map_exit_committed && rt_engaged
        && map_has_pending_transition() && obj_dude_on_exit_grid()) {
        combat_rt_arm_map_exit();
    }

    if (!rt_map_exit_armed) {
        return;
    }

    bool onGrid = obj_dude_on_exit_grid();

    if (!rt_engaged) {
        // Combat ended. On the exit grid -> "leave at once": disarm and let the warm bit clear
        // so map_check_state fires the queued transition. Away from the grid -> the dude won
        // the fight elsewhere; drop the queued destination so it can't teleport him.
        rt_map_exit_armed = false;
        if (!onGrid) {
            map_clear_pending_transition();
            trace_event(TRACE_FLOW, "map-exit-clear", "reason=combat-ended-off-grid t=%.2f", rt_clock);
        } else {
            trace_event(TRACE_FLOW, "map-exit-clear", "reason=combat-ended-on-grid t=%.2f", rt_clock);
        }
        return;
    }

    // Must be standing still ON the exit grid for the timer to advance. Moving (walk/run) or
    // stepping off the grid resets the countdown but keeps the escape armed -- it resumes
    // automatically without re-entering the zone. (FID_ANIM_TYPE is the live anim, same signal
    // the AP-drain block uses below.)
    int animType = FID_ANIM_TYPE(obj_dude->fid);
    bool moving = animType == ANIM_WALK || animType == ANIM_RUNNING;
    if (!onGrid || moving) {
        if (rt_map_exit_last_shown != -1) {
            // Was counting; show feedback that the hold restarted.
            rt_escape_message("Escaping... hold position (%d).", (int)RT_MAP_EXIT_DELAY);
        }
        rt_map_exit_timer = RT_MAP_EXIT_DELAY;
        rt_map_exit_last_shown = -1;
        return;
    }

    rt_map_exit_timer -= dt;

    int secs = (int)rt_map_exit_timer + 1;
    if (secs != rt_map_exit_last_shown && secs > 0) {
        rt_map_exit_last_shown = secs;
        rt_escape_message("Escaping... %d", secs);
    }

    if (rt_map_exit_timer <= 0.0f) {
        rt_map_exit_armed = false;
        rt_map_exit_committed = true; // hold combat torn down until the transition actually fires
        trace_event(TRACE_FLOW, "map-exit-go", "t=%.2f", rt_clock);
        combat_rt_end_for_map_exit(); // clears warm bit -> map_check_state loads the destination
    }
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
    rt_set_player_intent(target, hitMode);
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
        "eng=%d act=%d host=%d hp=%d ap=%d/%d ac=%d eap=%d lock=%d pend=%d inflight=%d anim=%d animOcc=%d/%d",
        rt_engaged ? 1 : 0, rt_actor_count, hostiles, dudeHp,
        combat_rt_player_ap(), combat_rt_player_max_ap(),
        dudeAc, dudeEap,
        rt_lock_target != NULL ? 1 : 0, pending ? 1 : 0, rt_inflight_count,
        dudeAnim, occ, RT_ANIM_TABLE_CAP);

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
            "DUDE pid=%d tile=%d x=%d y=%d fid=%08x anim=%d frame=%d flat=%d hid=%d dead=%d prone=%d busy=%d",
            obj_dude->pid, obj_dude->tile, obj_dude->x, obj_dude->y, obj_dude->fid,
            FID_ANIM_TYPE(obj_dude->fid), obj_dude->frame,
            (obj_dude->flags & OBJECT_FLAT) != 0 ? 1 : 0,
            (obj_dude->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            critter_is_dead(obj_dude) ? 1 : 0, critter_is_prone(obj_dude) ? 1 : 0,
            anim_busy(obj_dude));
    }

    for (int i = 0; i < rt_actor_count; i++) {
        RtActor* a = &rt_actors[i];
        Object* o = a->obj;
        if (o == NULL) {
            continue;
        }
        Object* tgt = a->target;
        trace_event(TRACE_STATE, "critter",
            "id=%d pid=%d tile=%d x=%d y=%d fid=%08x anim=%d frame=%d flat=%d hid=%d dead=%d prone=%d busy=%d "
            "st=%d host=%d frnd=%d tgt=%d dist=%d ap=%d",
            o->id, o->pid, o->tile, o->x, o->y, o->fid,
            FID_ANIM_TYPE(o->fid), o->frame,
            (o->flags & OBJECT_FLAT) != 0 ? 1 : 0,
            (o->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            critter_is_dead(o) ? 1 : 0, critter_is_prone(o) ? 1 : 0, anim_busy(o),
            a->state, a->hostile ? 1 : 0, a->friendly ? 1 : 0,
            tgt != NULL ? tgt->id : 0,
            tgt != NULL ? obj_dist(o, tgt) : -1, a->ap);
    }
}

} // namespace fallout
