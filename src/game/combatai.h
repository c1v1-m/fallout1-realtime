#ifndef FALLOUT_GAME_COMBATAI_H_
#define FALLOUT_GAME_COMBATAI_H_

#include <stddef.h>

#include "game/combat_defs.h"
#include "game/message.h"
#include "game/object_types.h"
#include "game/party.h"
#include "plib/db/db.h"

namespace fallout {

typedef enum AiMessageType {
    AI_MESSAGE_TYPE_RUN,
    AI_MESSAGE_TYPE_MOVE,
    AI_MESSAGE_TYPE_ATTACK,
    AI_MESSAGE_TYPE_MISS,
    AI_MESSAGE_TYPE_HIT,
} AiMessageType;

typedef struct AiPacket {
    char* name;
    int packet_num;
    int max_dist;
    int min_to_hit;
    int min_hp;
    int aggression;
    int hurt_too_much;
    int secondary_freq;
    int called_freq;
    int font;
    int color;
    int outline_color;
    int chance;
    int run_start;
    int move_start;
    int attack_start;
    int miss_start;
    int hit_start[HIT_LOCATION_SPECIFIC_COUNT];
    int last_msg;
} AiPacket;

int combat_ai_init();
void combat_ai_reset();
int combat_ai_exit();
int combat_ai_load(DB_FILE* stream);
int combat_ai_save(DB_FILE* stream);
int combat_ai_num();
char* combat_ai_name(int packetNum);
Object* ai_danger_source(Object* critter);
// Aggression from this critter's AI packet (AI.TXT). Realtime combat uses it to tell a guard from
// a civilian; see rt_guard_danger_source in combat_rt.cc.
int combatai_aggression(Object* critter);
Object* ai_search_inven(Object* critter, int check_action_points);
// Ranks two weapons the way the turn-based AI does: RANGED beats THROW beats melee, ties broken
// by item cost. NULL-tolerant, so it doubles as "is the candidate better than what is held?".
Object* ai_best_weapon(Object* weapon1, Object* weapon2);
// The same, for THIS critter: the weapon whose skill it is clearly better at wins first (see
// AI_WEAPON_SKILL_MARGIN), and only between near-equal skills does ai_best_weapon's order decide.
Object* ai_best_weapon_for(Object* critter, Object* weapon1, Object* weapon2);
void combat_ai_begin(int critters_count, Object** critters);
void combat_ai_over();
Object* combat_ai(Object* critter, Object* target);
// Vanilla's flee decision and flee tile, for the realtime AI (combat_rt rt_flee_step) -- see
// combatai.cc. Reason: 0 none, 1 FLEEING, 2 hurt_too_much, 3 below min_hp. Tile: -1 cornered,
// -2 far enough (DISENGAGING set).
int combatai_flee_reason(Object* critter);
int combatai_flee_tile(Object* critter, int actionPoints);
int combatai_max_dist(Object* critter);
int combatai_min_to_hit(Object* critter);
int combatai_pick_hit_mode(Object* critter, Object* weapon);
int combatai_called_shot(Object* critter, Object* target, int hitMode);
bool combatai_want_to_join(Object* critter);
bool combatai_want_to_stop(Object* critter);
int combatai_switch_team(Object* critter, int team);
int combatai_msg(Object* critter, Attack* attack, int message_type, int delay);
// The same with the hit location given instead of an Attack, for a bark registered after the attack
// struct is gone (a realtime projectile flight, actions.cc).
int combatai_msg_at(Object* critter, int hitLocation, int message_type, int delay);
// Floats `text` over `critter` in its AI packet's font and colours -- the look of a combat taunt.
int combatai_float_line(Object* critter, const char* text);
bool combatai_msg_text(int num, char* out, size_t size);
Object* combat_ai_random_target(Attack* attack);
void combatai_check_retaliation(Object* critter, Object* candidate);
bool is_within_perception(Object* critter1, Object* critter2);
bool is_within_perception_unsneaked(Object* critter1, Object* critter2);
void combatai_refresh_messages();
void combatai_notify_onlookers(Object* critter);
void combatai_delete_critter(Object* critter);

} // namespace fallout

#endif /* FALLOUT_GAME_COMBATAI_H_ */
