#ifndef FALLOUT_GAME_COMBAT_RT_H_
#define FALLOUT_GAME_COMBAT_RT_H_

#include "game/object_types.h"
#include "plib/db/db.h" // DB_FILE, for the medic opt-out save chunk

namespace fallout {

// Realtime combat system.
//
// Replaces the turn-based combat() interrupt with a per-frame, per-actor state
// machine. There is no "combat mode" the player enters or leaves: enemies aggro
// when the player comes within their perception range, act on cooldowns derived
// from their action points (Agility), and the player moves/attacks freely in
// realtime. See combat_rt.cc for the design notes.
//
// This system REUSES the existing damage pipeline (combat_attack, apply_damage,
// roll.cc, stat_level) rather than reimplementing it. It keeps the combat
// subsystem "warm" (the COMBAT_STATE_0x01 in-combat bit set while hostiles are
// near) so the shared attack/animation/damage code engages, but it never runs
// the turn scheduler, action-point UI, or end-turn flow.

// Lifecycle.
void combat_rt_init();
void combat_rt_exit();
void combat_rt_reset();

// Per-frame tick. dt is wall-clock seconds since the previous frame. Drives
// aggro, the NPC state machines, and cooldowns. Safe to call every frame even
// when no hostiles are present (it early-outs cheaply).
void combat_rt_update(float dt);

// Routes a player input event (a get_input() key code) to the realtime system:
// WASD tile movement and mouse-click-to-attack. Returns true if the event was
// consumed by combat_rt and should not be processed further.
bool combat_rt_handle_input(int keyCode);

// Polls the gamepad (SDL_GameController) once per frame: left stick -> tile
// movement, right trigger / A -> attack nearest hostile in range, B -> cancel.
// Called from combat_rt_update.
void combat_rt_poll_gamepad();

// True while at least one hostile is engaged (for music/UI hooks later).
bool combat_rt_active();

// "Is something actually coming for me right now?" -- an engaged, live hostile that is after the
// player's side AND can perceive him. Use this INSTEAD of isInCombat() for any vanilla rule that
// meant "you are in a turn-based encounter" (no talking, no script timers, ...): realtime keeps the
// warm combat bit set for as long as anything anywhere is chasing, which turns those short,
// bounded restrictions into permanent ones. Falls back to isInCombat() outside realtime.
bool combat_rt_under_threat();
// True while a fight is going on within sight of the player, whoever it is between -- including a
// brawl he is only a bystander to. Restores vanilla's "no chatting mid-fight" for the PLAYER-INITIATED
// talk gates only, without the map-wide lockout that a raw isInCombat() brings back. Cleared at once
// by combat_rt_end_combat(). See the note above the definition before widening its use.
bool combat_rt_fight_in_view();

// True while any critter on the dude's elevation is still playing its death animation. Saving calls
// anim_stop(), which force-ends every sequence and slams a mid-fall body flat, so an autosave must
// wait for this to clear.
bool combat_rt_death_anim_pending();

// Per-critter version of the above: is THIS critter actually fighting the player right now?
// Use it instead of combat_rt_is_hostile() wherever the question is "should I treat this as an
// enemy in front of me" -- the hostile flag itself is STICKY and never cleared, so a bystander
// caught by faction aggro carries it for the rest of the map's life even if it never acts.
bool combat_rt_is_engaging(Object* obj);

// Manual END COMBAT -- the realtime replacement for vanilla's Enter key / the interface bar's
// END COMBAT button, both of which live in the dormant turn-based path and are unreachable here.
// It is NOT an escape hatch: exactly like vanilla's combat_end(), it only succeeds when every
// live hostile already wants to stop (disengaging, fleeing, or perceiving nobody to fight), and
// otherwise refuses and prints combat.msg #103 so the player is told WHY. Returns true if combat
// ended (or was not running).
bool combat_rt_end_combat();

// True when a hostile the player could hit is within current-weapon range. The
// gamepad layer uses this to arbitrate the A button: A attacks when this is true,
// otherwise A is a world/UI interact (left click). Cheap; safe every frame.
bool combat_rt_hostile_in_attack_range();

// True if this specific critter is currently flagged hostile to the dude.
bool combat_rt_is_hostile(Object* obj);

// True while a right-stick lock-on target is set. The gamepad layer also treats
// A as a committed attack (never a UI interact) whenever this is true, so a lock
// on an out-of-range target still consumes A (it just doesn't fire) instead of
// falling through to a world click.
bool combat_rt_has_lock();
// True while the player is walking under left-stick control (not standing, not walking out a
// click-to-move path).
bool combat_rt_stick_walking();

// MODS page: live read/write of the two gamepad movement assists. The cfg is still where they are
// LOADED from at init; these are how the settings screen changes them without a re-init.
void combat_rt_set_cam_smooth(float k);
// MODS "Camera leans to target": ease the view toward an aimed target that is off the screen.
void combat_rt_set_cam_lean(bool on);
bool combat_rt_get_cam_lean();
// MODS "Hits stagger": the stagger chance factor (0 = every hit through armor staggers).
void combat_rt_set_flinch_chance(float factor);
float combat_rt_get_flinch_chance();
float combat_rt_get_cam_smooth();
void combat_rt_set_knockdown_agility(float factor);
float combat_rt_get_knockdown_agility();
void combat_rt_set_door_magnet(float strength);
float combat_rt_get_door_magnet();
void combat_rt_set_npc_slide(bool enabled);
bool combat_rt_get_npc_slide();
void combat_rt_set_mouse_hold_walk(bool enabled);
bool combat_rt_get_mouse_hold_walk();
void combat_rt_set_forgive_stray(bool enabled);
bool combat_rt_get_forgive_stray();
bool combat_rt_weapon_amnesty();
bool combat_rt_forgive_stray(Object* attacker, Object* bystander, Object* intended);
// MODS "Enemies look around" (cfg enemies_look_around): idle hostiles turn toward gunshots and
// scan around, so a back turned to you is not a permanent blind spot. See rt_idle_look_around.
void combat_rt_set_look_around(bool enabled);
bool combat_rt_get_look_around();

// MODS "Script combat turns" (cfg script_turns, default on = vanilla): every scripted critter on the
// map gets the combat event vanilla gives it each turn -- the start of its turn while it fights, the
// "do you join?" check while it does not. See rt_script_turn_tick.
void combat_rt_set_script_turns(bool enabled);
bool combat_rt_get_script_turns();
void combat_rt_set_free_move(bool enabled);
bool combat_rt_get_free_move();
// Free movement: the offset the dude stands at on his tile, for dude_stand to keep instead of
// re-centring him. False = re-centre as vanilla (free movement off, or he is on another tile).
bool combat_rt_dude_rest_offset(int* x, int* y);

// --- L3 aimed-shot wheel support (gamepad_ui drives the body-part overlay). ---

// Whether an aimed shot is possible right now: the dude is in combat, the active
// weapon supports called shots, and there is a target (the lock-on, else the nearest
// hostile in range). The gamepad layer gates the body-part wheel on this.
bool combat_rt_can_aim();

// The critter an aimed shot would currently hit (the lock-on target, else the nearest
// hostile in range), or NULL. The gamepad layer uses it to show the target's body in the
// wheel hub.
Object* combat_rt_aim_target();

// Keyboard & mouse (2026-10-10): the hostile the mouse is aiming at, or NULL (combat_rt.cc), and the
// keyboard's Q / E lock step (-1 left, +1 right).
Object* combat_rt_mouse_target();
void combat_rt_request_lock_step(int dir);

// To-hit % for a called shot at `hitLocation` (a HIT_LOCATION_* specific value)
// against the current aim target, clamped to 0..999. 0 if there is no target. Used to
// label each wheel segment.
int combat_rt_aim_chance(int hitLocation);

// Queue an aimed shot at the current target hitting `hitLocation`. Returns true if it
// was latched (the arbiter fires it on the next eligible frame, paying the aimed AP
// cost), false if no valid target / the weapon can't aim / the location is invalid.
bool combat_rt_aimed_attack(int hitLocation);

// The hit mode the player's next attack would use. The called-shot window needs it to print the
// right to-hit percentage per body part.
int combat_rt_player_hit_mode();

// Why an aimed shot is impossible right now, as a short player-facing phrase -- or NULL when it is
// possible. Several of the reasons are VANILLA rules the player cannot see from the interface bar
// (Fast Shot forbids called shots outright; burst and continuous modes cannot be aimed; neither can
// flame, explosive or EMP damage), so a silent refusal is indistinguishable from a broken button.
const char* combat_rt_aim_refusal();

// Green-highlight (Fallout 3 VATS style) the current aim target while the body-part wheel
// is up, replacing its combat red outline; the off call restores the exact prior outline.
// Called by the gamepad layer on aim-wheel open/close. No-op if there is no target.
void combat_rt_aim_highlight_on();
void combat_rt_aim_highlight_off();

// Per-companion "stop patching me up", set from the dialogue. Keyed by object id so it survives
// map changes and loads. Only the PLAYER is refused -- the medic still doses itself and other
// allies. combat_rt_medic_optout_load treats a missing chunk as "not in this save" and defaults
// to everyone healing you, which is what makes the save format change backward compatible.
// "Give me some room": the follower WALKS a few tiles away from the dude. Queued rather than done
// on the spot, because the dialogue that asks for it is modal -- it fires on the first update after
// the conversation closes. Pass NULL to cancel a standing request.
void combat_rt_request_step_back(Object* npc);

bool combat_rt_medic_beat_enabled();
bool combat_rt_medic_treats_player(Object* npc);
void combat_rt_medic_set_treats_player(Object* npc, bool treats);
void combat_rt_medic_optout_clear();
int combat_rt_medic_optout_save(DB_FILE* stream);
int combat_rt_medic_optout_load(DB_FILE* stream);

// Rate of fire, for the character sheet to PRINT. Both go through the same helpers the fight
// uses, so the card can never disagree with the combat model.
//
// combat_rt_skill_attack_speed: seconds between attacks for one of the six combat skills, at
//   `skillLevel`, with `critter`'s current SPECIAL folded in -- plus the two multipliers that
//   made it, so the card can say how much of the number is the skill's doing. The attack
//   category comes from the skill, so no weapon needs to be in hand.
// combat_rt_stat_attack_speed: the multiplier a SPECIAL level buys, for the one attack category
//   that stat owns (STR melee, PE ranged, AGI thrown).
//
// Both return false when there is nothing true to print: turn-based mode, the cooldown disabled
// by config, or a stat/skill that owns no attack category.
bool combat_rt_skill_attack_speed(Object* critter, int skill, int skillLevel,
    float* seconds, float* statMult, float* skillMult);
bool combat_rt_stat_attack_speed(int stat, int statLevel, float* mult, int* attackType);
float combat_rt_attack_cooldown_base();

// Tap-A world loot: if an adjacent corpse or container is next to the dude, stash
// it (opened by combat_rt_update) and return true so the gamepad layer routes A to
// looting instead of a walk-click. Safe to call from the input hook (detection
// only; the loot window is opened later from the main loop).
bool combat_rt_try_world_loot();

// Hook from combat(): when a critter's script routes an attack on the dude
// through the turn-based entry point (op_attack -> scripts_request_combat ->
// combat()), combat() forwards the attacker/defender here before refusing to
// run. combat_rt uses it as the authoritative "this critter is hostile to the
// player" signal (genuine hostiles request combat; peaceful NPCs never do).
void combat_rt_notify_attack(Object* attacker, Object* defender);

// Hook from the realtime damage committer (combat_commit_pending_attack) for a critter that the
// hit KILLED or knocked out. Vanilla's combatai_notify_onlookers ran on dying defenders too;
// combat_rt's aggro spread cannot, because it needs a live seed hostile -- so without this a
// one-shot kill was invisible to the critter standing right next to the victim.
void combat_rt_notify_victim(Object* attacker, Object* victim);

// Hook from obj_remove_all() (object.cc) right before a map teardown frees every object.
// Abandons any in-flight realtime attack and drops all object pointers combat_rt holds
// (rt_resolving, the actor registry, lock/intent/loot, the engine's pending main_ctd) so a
// stale attack can't commit against freed memory after a mid-combat map exit. Idempotent.
void combat_rt_notify_objects_freed();
// Cancel a queued walk-there-then-act the player started with A. True if something was cancelled.
bool combat_rt_cancel_player_action();

// Hook from obj_erase_object() (object.cc) right before a SINGLE object is freed (a script
// destroy, a gibbed/cleaned-up corpse, etc.). If a critter is being freed mid-realtime-combat,
// clears every dangling whoHitMe pointer that references it (so apply_damage's retaliation
// check can't dereference freed memory) and drops combat_rt's own pointers to it. No-op outside
// realtime combat / for non-critter objects.
void combat_rt_notify_critter_removed(Object* removed);

// Total damage from attacks that are already IN FLIGHT against `defender` (computed and
// snapshotted, but not yet committed), plus whether one of them is already flagged lethal.
// combat.cc's check_for_death() calls this so that an attack's DAM_DEAD flag -- which is decided
// at FIRE time -- accounts for the damage that will land before this attack does. Without it,
// concurrent attacks each compute "not lethal" against the same stale HP, nobody registers a
// death animation, and the kill falls into critter_adjust_hits' emergency critter_kill() path.
// `killClaimed` (may be NULL) reports that another in-flight attack already owns the death, so
// this one must not register a second death animation. Returns 0 outside realtime.
int combat_rt_pending_damage_on(Object* defender, bool* killClaimed);

// Defensive guard called from combat_commit_pending_attack() right before apply_damage(): clears
// a dangling whoHitMe on either attack party so apply_damage's retaliation check
// (combatai_check_retaliation -> combatai_rating(defender->whoHitMe)) can't dereference a freed
// Object. Validates the whoHitMe pointer by membership in the live critter list (a scan that
// never dereferences the candidate) and NULLs it if the referenced critter is gone. Covers any
// free path that the obj_erase_object hook misses. No-op outside realtime combat.
void combat_rt_validate_who_hit_me(Object* a, Object* b);

// Called from map_leave_map() / the elevation-change guard when the player leaves a map
// MID-COMBAT in realtime. Ends realtime combat cleanly (clears the engine warm bit so
// isInCombat() becomes false and the map transition proceeds) instead of the vanilla
// "in combat -> game_user_wants_to_quit = 1" path, which boots the player to the main menu.
void combat_rt_end_for_map_exit();

// op_terminate_combat in realtime: end the engagement unconditionally (the script decided), instead
// of vanilla's game_user_wants_to_quit = 1, which only the dormant turn-based loop ever consumed --
// under combat_rt it dropped the player to the main menu as if they had died.
void combat_rt_terminate_from_script();

// Called from map_leave_map() when the player steps onto a map-exit grid IN realtime combat.
// Arms the exit: on its next frame combat_rt ends the fight so the queued transition fires --
// immediately (the 10 s hold-position delay was removed 2026-10-06 at his request).
// scripted = the transition did not come from stepping onto an exit grid (stairs, ladders and
// elevators are scripted): it goes on the next frame instead of waiting for the dude to stand on one.
void combat_rt_arm_map_exit(bool scripted = false);

// Route a legacy player-attack request (combat_attack_this / the crosshair mouse path)
// into the realtime intent system. Calling combat_attack() directly in realtime sets the
// engine's pending flag without combat_rt's rt_resolving slot, wedging the attack lock
// until the 2.5s watchdog (a frozen, damage-less swing); this resolves the attack through
// rt_do_attack instead. Only a hostile combatant in weapon range is attacked (else no-op).
// Returns true if attack intent was set.
bool combat_rt_player_attack(Object* target);

// game_trace heartbeat callback: emits one TRACE_STATE snapshot of the realtime combat
// world (actors, hostiles, dude HP/AP, lock, pending/resolving, anim-table occupancy)
// plus invariant health-check TRACE_WARNs. Registered with trace_set_snapshot_fn.
void combat_rt_trace_snapshot();

// True once combat_rt owns combat (between combat_rt_init and combat_rt_exit).
// The turn-based combat() scheduler checks this and refuses to activate while
// it is set, so the old combat mode can never take over.
bool combat_rt_enabled();

// True while the player is actively feeding movement input (within a short grace
// window of the last WASD/stick step). actions.cc uses this to suppress the dude's
// enemy-miss dodge while he's moving, so a miss never interrupts his movement.
bool combat_rt_dude_moving();

// True while the critter is in its anti-stunlock flinch-immunity window (see combat_rt.cc).
bool combat_rt_flinch_immune(Object* obj);

// True when this attacker's weapon still counts as SHOULDERED from a recent shot, so action_ranged
// should SKIP the ANIM_POINT raise. The raise is a per-shot 0.43 s tax when you fire continuously,
// so it is paid once per burst rather than once per round -- see the long note in combat_rt.cc.
// Governed by cfg `ranged_point_hold` (< 0 never raises, 0 raises every shot, > 0 = the hold).
bool combat_rt_weapon_is_up(Object* attacker);

// The player's realtime action-point pool and its cap (the dude's max AP). The
// interface bar's AP indicator reads these so the bulbs reflect the real pool
// (which paces attacks) instead of the engine combat.ap that combat_rt keeps
// maxed for free movement. Both clamp safely before the first update frame.
int combat_rt_player_ap();
// The REAL remaining action points of any critter in a realtime fight, or -1 when combat_rt is not
// the authority (disabled / untracked). The engine's combat.critter.ap is pinned to max and is NOT
// this number -- see the definition in combat_rt.cc.
int combat_rt_critter_ap(Object* critter);
int combat_rt_player_max_ap();
bool combat_rt_spend_ap(int cost);
// Return points an action took up front and then never did (a cut reload). No-op out of a fight.
void combat_rt_refund_ap(int amount);
// MODS "Reload animation": who is in the middle of a reload loop. begin/end bracket it; the query
// also drops a body whose loop was replaced by something else (knockdown, walk, attack).
void combat_rt_reload_begin(Object* critter);
void combat_rt_reload_end(Object* critter);
bool combat_rt_is_reloading(Object* critter);

// Empty a critter's realtime pool. The realtime stand-in for vanilla's "lose your turn" critical
// failure: there are no turns, so the cost is paid as points plus the wait for them to come back.
// Safe on anyone -- the dude, a tracked NPC, or a critter the registry has never heard of.
void combat_rt_zero_ap(Object* critter);

// Is something adjacent lootable RIGHT NOW (a corpse or a container)? Pure query -- unlike
// combat_rt_try_world_loot it queues nothing. The pad asks this the instant A goes down, to tell a
// press that MEANT to loot from the press that merely happened to kill something.
bool combat_rt_world_loot_available();

// True while the dude's own attack cooldown is still running -- he shot or swung a moment ago.
// Distinguishes "mashing A at an enemy" from "deliberately double-tapping to loot".
bool combat_rt_player_attack_busy();

// True while movement is STOPPED on an empty pool and has not yet earned its way back: the
// latch runs from the moment walking/running drains the pool to 0 until AP has climbed back
// to the reserve threshold (cfg `resume_ap`, clamped to the dude's max). False out of a
// fight, where nothing drains the pool and no threshold is enforced. The interface bar
// paints the AP indicator RED for exactly this window, so the "you stop at 0, you move
// again at 3" rule is on screen instead of being a silent loss of control.
bool combat_rt_move_blocked();

// The Bonus Move perk's movement-only reserve (2 points per rank): spent by walking and running
// before the regular pool, refilled while the dude stands, never touched by anything else. 0
// without the perk and 0 out of a fight. The interface bar draws it as the vanilla yellow bulbs.
int combat_rt_bonus_move_ap();

// How fast the world is currently running, as a fraction of normal. 1.0 nearly always; less than
// that while the on-body limb picker is up, which slows the fight instead of pausing it.
//
// Read by anim.cc's compute_tpf as well as by combat_rt itself, so the animation clock and the
// combat model slow together -- a critter that decides at quarter speed but walks at full speed
// looks broken. Never returns 0: this is a slow-down, never a stop.
float combat_rt_time_scale();
float combat_rt_flinch_seconds();

} // namespace fallout

#endif /* FALLOUT_GAME_COMBAT_RT_H_ */
