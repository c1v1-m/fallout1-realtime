#ifndef FALLOUT_GAME_ACTIONS_H_
#define FALLOUT_GAME_ACTIONS_H_

#include "game/combat_defs.h"
#include "game/object_types.h"
#include "game/proto_types.h"

namespace fallout {

extern unsigned int rotation;
extern int obj_fid;
extern int obj_pid_old;

void switch_dude();
int action_knockback(Object* obj, int* anim, int maxDistance, int rotation, int delay);
int action_blood(Object* obj, int anim, int delay);
void show_damage_to_object(Object* a1, int damage, int flags, Object* weapon, bool isFallingBack, int knockbackDistance, int knockbackRotation, int a8, Object* a9, int a10);
int show_damage_target(Attack* attack);
int show_damage_extras(Attack* attack);
void show_damage(Attack* attack, int a2, int a3, bool splitDefender = false);
// Realtime: register the defender's damage reaction as its OWN sequence, from a committed attack
// snapshot. Called by combat_rt at its (action-frame synchronised) commit point, so the attacker's
// sequence never carries his victim's death and ends with his own swing. See actions.cc.
bool action_defender_reaction(Attack* attack);
// Realtime: the splash victims' reactions, at the same commit (see actions.cc).
void action_extras_reaction(Attack* attack);
// Realtime: take the explosive projectile the last ranged attack launched (NULL = none) and the
// explosion art it becomes on arrival; combat_rt commits the blast then.
Object* action_take_explosive_projectile(int* explosionFid);

// The "explosion fid" handed over for a projectile that has no blast and is simply erased when it
// arrives (a laser bolt, a plasma ball): combat_rt reads the erase as the landing.
#define ACTION_RT_LANDS_ON_ERASE (-2)
// obj_erase_object, just before the free: a realtime flight waiting for its launch drops `obj`.
void action_notify_object_freed(Object* obj);
int action_attack(Attack* attack);
int use_an_object(Object* item);
int a_use_obj(Object* a1, Object* a2, Object* a3);
int action_use_an_item_on_object(Object* a1, Object* a2, Object* a3);
int action_use_an_object(Object* a1, Object* a2);
int get_an_object(Object* item);
int action_get_an_object(Object* critter, Object* item);
int action_loot_container(Object* critter, Object* container);
int action_skill_use(int a1);
int action_use_skill_in_combat_error(Object* critter);
int action_use_skill_on(Object* a1, Object* a2, int skill);
Object* pick_object(int objectType, bool a2);
int pick_hex();
bool is_hit_from_front(Object* a1, Object* a2);
bool can_see(Object* a1, Object* a2);
int pick_fall(Object* obj, int anim);
int action_explode(int tile, int elevation, int minDamage, int maxDamage, Object* a5, bool a6);
int action_talk_to(Object* a1, Object* a2);
void action_dmg(int tile, int elevation, int minDamage, int maxDamage, int damageType, bool animated, bool bypassArmor);

} // namespace fallout

#endif /* FALLOUT_GAME_ACTIONS_H_ */
