#ifndef FALLOUT_GAME_COMBAT_H_
#define FALLOUT_GAME_COMBAT_H_

#include "game/anim.h"
#include "game/combat_defs.h"
#include "game/message.h"
#include "game/object_types.h"
#include "game/party.h"
#include "game/proto_types.h"
#include "plib/db/db.h"

namespace fallout {

extern unsigned int combat_state;
extern STRUCT_664980* gcsd;
extern bool combat_call_display;
extern int cf_table[WEAPON_CRITICAL_FAILURE_TYPE_COUNT][WEAPON_CRITICAL_FAILURE_EFFECT_COUNT];

extern MessageList combat_message_file;
extern Object* combat_turn_obj;
extern int combat_exps;
extern int combat_free_move;

int combat_init();
void combat_reset();
void combat_exit();
int find_cid(int start, int cid, Object** critterList, int critterListLength);
int combat_load(DB_FILE* stream);
int combat_save(DB_FILE* stream);
Object* combat_whose_turn();
void combat_data_init(Object* obj);
void combat_over_from_load();
void combat_give_exps(int exp_points);
int combat_in_range(Object* critter);
void combat_end();
// Displays combat.msg #103 ("can't stop fighting yet"). Used by combat_rt's realtime END
// COMBAT action so it gives the same feedback vanilla's END COMBAT button did.
void combat_display_cant_end_message();
void combat_turn_run();
void combat_end_turn();
void combat(STRUCT_664980* attack);
void combat_ctd_init(Attack* attack, Object* attacker, Object* defender, int hitMode, int hitLocation);
int combat_attack(Object* a1, Object* a2, int hitMode, int location);
int combat_bullet_start(const Object* a1, const Object* a2);
void compute_explosion_on_extras(Attack* attack, int a2, bool isGrenade, int a4);
int determine_to_hit(Object* a1, Object* a2, int hitLocation, int hitMode);
int determine_to_hit_no_range(Object* a1, Object* a2, int hitLocation, int hitMode);
void death_checks(Attack* attack);
void apply_damage(Attack* attack, bool animated);
void combat_display(Attack* attack);
void combat_anim_begin();
void combat_anim_finished();
// True while an attack's damage is pending commit (issued but apply_damage not
// yet run). Used by realtime combat to serialize attacks through main_ctd.
bool combat_is_attack_pending();
// Recovery valve for realtime combat's stuck-lock watchdog: forcibly abandons a
// pending attack whose animation was orphaned, so the lock can release. Never
// called in the normal attack flow (damage commits through combat_anim_finished).
void combat_clear_pending_attack();
// Commits a pending attack's damage now (ammo/display/apply_damage/scr_end_combat),
// returning true if one was committed. Realtime combat drives this per-attack when
// the swing animation finishes, since it keeps the warm bit set so the engine's
// combat_turn_running==0 commit never fires. Does NOT stand the attacker up.
bool combat_commit_pending_attack();
// Realtime concurrency: copy the just-computed pending attack out (so the global can be freed
// immediately and many attacks run at once), and later commit that private copy.
void combat_take_pending_snapshot(Attack* out, bool* outCallDisplay);
bool combat_commit_snapshot(const Attack* snap, bool callDisplay);
// Flag a snapshot lethal when its damage will empty the defender's CURRENT hp but fire-time
// lethality said otherwise (concurrent attackers each measured against the same stale hp).
// Returns true if it upgraded the snapshot. Call it BEFORE the defender's reaction is registered,
// or the death fall is picked from the stale flags and the corpse snaps flat with no fall.
bool combat_late_lethal_upgrade(Attack* attack);
int combat_check_bad_shot(Object* attacker, Object* defender, int hitMode, bool aiming);

// The vanilla called-shot window, opened for the pad: returns the hit location the player chose, or
// -1 on cancel. `initialLocation` is where its selection starts.
int combat_called_shot_pick(Object* critter, int hitMode, int initialLocation);
bool combat_to_hit(Object* target, int* accuracy);

// THROWN AS FAR AS IT GOES (2026-10-18, realtime only -- combat_rt.cc sets it around one attack): when
// >= 0, the next THROW attack ignores its range, misses on purpose and lands on this tile (or on the
// first thing in the way). His pick: a quick-slot grenade at a target out of reach is not put away.
extern int combat_forced_throw_tile;
void combat_attack_this(Object* a1);
void combat_outline_on();
void combat_outline_off();
void combat_highlight_change();
bool combat_is_shot_blocked(Object* a1, int from, int to, Object* a4, int* a5);
// Localized name of a body part for a given critter (the strings the vanilla called-shot window
// uses). NULL if the message file has no entry. Used by the gamepad aimed-shot readout.
char* combat_get_loc_name(Object* critter, int hitLocation);
int combat_player_knocked_out_by();
int combat_explode_scenery(Object* a1, Object* a2);
void combat_delete_critter(Object* obj);

static inline bool isInCombat()
{
    return (combat_state & COMBAT_STATE_0x01) != 0;
}

} // namespace fallout

#endif /* FALLOUT_GAME_COMBAT_H_ */
