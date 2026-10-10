#ifndef FALLOUT_GAME_ANIMATION_H_
#define FALLOUT_GAME_ANIMATION_H_

#include "game/object_types.h"

namespace fallout {

typedef enum AnimationRequestOptions {
    ANIMATION_REQUEST_UNRESERVED = 0x01,
    ANIMATION_REQUEST_RESERVED = 0x02,
    ANIMATION_REQUEST_NO_STAND = 0x04,
    ANIMATION_REQUEST_0x100 = 0x100,
    ANIMATION_REQUEST_INSIGNIFICANT = 0x200,
    // A move that is one leg of a longer one (a chase segment, a flee leg): when it ends, hold the
    // body in its stride for a beat instead of snapping it to ANIM_STAND, because the next leg is
    // about to be registered. Stood after all if nothing follows. See anim_defer_stand in anim.cc.
    ANIMATION_REQUEST_DEFER_STAND = 0x400,
} AnimationRequestOptions;

// Basic animations: 0-19
// Knockdown and death: 20-35
// Change positions: 36-37
// Weapon: 38-47
// Single-frame death animations (the last frame of knockdown and death animations): 48-63
typedef enum AnimationType {
    ANIM_STAND = 0,
    ANIM_WALK = 1,
    ANIM_JUMP_BEGIN = 2,
    ANIM_JUMP_END = 3,
    ANIM_CLIMB_LADDER = 4,
    ANIM_FALLING = 5,
    ANIM_UP_STAIRS_RIGHT = 6,
    ANIM_UP_STAIRS_LEFT = 7,
    ANIM_DOWN_STAIRS_RIGHT = 8,
    ANIM_DOWN_STAIRS_LEFT = 9,
    ANIM_MAGIC_HANDS_GROUND = 10,
    ANIM_MAGIC_HANDS_MIDDLE = 11,
    ANIM_MAGIC_HANDS_UP = 12,
    ANIM_DODGE_ANIM = 13,
    ANIM_HIT_FROM_FRONT = 14,
    ANIM_HIT_FROM_BACK = 15,
    ANIM_THROW_PUNCH = 16,
    ANIM_KICK_LEG = 17,
    ANIM_THROW_ANIM = 18,
    ANIM_RUNNING = 19,
    ANIM_FALL_BACK = 20,
    ANIM_FALL_FRONT = 21,
    ANIM_BAD_LANDING = 22,
    ANIM_BIG_HOLE = 23,
    ANIM_CHARRED_BODY = 24,
    ANIM_CHUNKS_OF_FLESH = 25,
    ANIM_DANCING_AUTOFIRE = 26,
    ANIM_ELECTRIFY = 27,
    ANIM_SLICED_IN_HALF = 28,
    ANIM_BURNED_TO_NOTHING = 29,
    ANIM_ELECTRIFIED_TO_NOTHING = 30,
    ANIM_EXPLODED_TO_NOTHING = 31,
    ANIM_MELTED_TO_NOTHING = 32,
    ANIM_FIRE_DANCE = 33,
    ANIM_FALL_BACK_BLOOD = 34,
    ANIM_FALL_FRONT_BLOOD = 35,
    ANIM_PRONE_TO_STANDING = 36,
    ANIM_BACK_TO_STANDING = 37,
    ANIM_TAKE_OUT = 38,
    ANIM_PUT_AWAY = 39,
    ANIM_PARRY_ANIM = 40,
    ANIM_THRUST_ANIM = 41,
    ANIM_SWING_ANIM = 42,
    ANIM_POINT = 43,
    ANIM_UNPOINT = 44,
    ANIM_FIRE_SINGLE = 45,
    ANIM_FIRE_BURST = 46,
    ANIM_FIRE_CONTINUOUS = 47,
    ANIM_FALL_BACK_SF = 48,
    ANIM_FALL_FRONT_SF = 49,
    ANIM_BAD_LANDING_SF = 50,
    ANIM_BIG_HOLE_SF = 51,
    ANIM_CHARRED_BODY_SF = 52,
    ANIM_CHUNKS_OF_FLESH_SF = 53,
    ANIM_DANCING_AUTOFIRE_SF = 54,
    ANIM_ELECTRIFY_SF = 55,
    ANIM_SLICED_IN_HALF_SF = 56,
    ANIM_BURNED_TO_NOTHING_SF = 57,
    ANIM_ELECTRIFIED_TO_NOTHING_SF = 58,
    ANIM_EXPLODED_TO_NOTHING_SF = 59,
    ANIM_MELTED_TO_NOTHING_SF = 60,
    ANIM_FIRE_DANCE_SF = 61,
    ANIM_FALL_BACK_BLOOD_SF = 62,
    ANIM_FALL_FRONT_BLOOD_SF = 63,
    ANIM_CALLED_SHOT_PIC = 64,
    ANIM_COUNT = 65,
    FIRST_KNOCKDOWN_AND_DEATH_ANIM = ANIM_FALL_BACK,
    LAST_KNOCKDOWN_AND_DEATH_ANIM = ANIM_FALL_FRONT_BLOOD,
    FIRST_SF_DEATH_ANIM = ANIM_FALL_BACK_SF,
    LAST_SF_DEATH_ANIM = ANIM_FALL_FRONT_BLOOD_SF,
} AnimationType;

#define FID_ANIM_TYPE(value) ((value) & 0xFF0000) >> 16

// Signature of animation callback accepting 2 parameters.
typedef int AnimationCallback(void*, void*);

// Signature of animation callback accepting 3 parameters.
typedef int AnimationCallback3(void*, void*, void*);

typedef Object* PathBuilderCallback(Object* object, int tile, int elevation);

typedef struct StraightPathNode {
    int tile;
    int elevation;
    int x;
    int y;
} StraightPathNode;

void anim_init();
void anim_reset();
void anim_exit();
void anim_debug_table(int* occupied, int* nonReserved, int* curr); // TEMP DIAG

// Discards a register_begin transaction nobody closed. Returns true if there was one.
// See the comment in anim.cc -- an unclosed transaction disables ALL attacks game-wide.
bool anim_discard_open_transaction();

// The two global reasons register_begin refuses: an open transaction (-1 = none) and the
// anim_stop reentry guard.
void anim_debug_gate(int* openTransaction, int* inAnimStop);
// Turns on the [WARN/reg-conflict] log inside check_registry for the duration of an attack's
// registration -- the refusal that silently aborts the whole attack via anim_cleanup.
void anim_debug_attack_scope(bool on);
int register_begin(int a1);
int register_priority(int a1);
int register_clear(Object* a1);
// register_clear variant that ends even a PRIORITIZED sequence (realtime combat; see anim.cc).
int anim_force_clear(Object* a1);
// Hook from obj_erase_object(): drop every raw pointer anim.cc holds to an object about to be
// freed (the sad[] frame stepper + the sequence table). Without it object_animate dereferences
// the dangling pointer next frame -- the art_frame_offset ACCESS_VIOLATION. See anim.cc.
void anim_notify_object_freed(Object* obj);
// register_clear variant that only ends a sequence in which `a1` still has UNPLAYED work.
// MUST be used instead of register_clear by anything gated on anim_busy_blocking (see anim.cc).
int anim_clear_pending(Object* a1);
int register_end();
int check_registry(Object* obj);
int anim_busy(Object* a1);
// Narrower anim_busy: true only while `a1` still OWES animation work, ignoring descriptions
// that have already played. Lets the realtime player move on once his own swing is done
// instead of waiting out his victim's death in the same sequence. See anim.cc.
int anim_busy_blocking(Object* a1);
// TEMP DIAG: describe the live sequence holding `a1` busy (see anim.cc). STRIP when done.
void anim_debug_blocking_dump(Object* a1, char* buf, size_t size);
int register_object_move_to_object(Object* owner, Object* destination, int actionPoints, int delay);
int register_object_run_to_object(Object* owner, Object* destination, int actionPoints, int delay);
int register_object_move_to_tile(Object* owner, int tile, int elevation, int actionPoints, int delay);
int register_object_run_to_tile(Object* owner, int tile, int elevation, int actionPoints, int delay);
int register_object_move_straight_to_tile(Object* object, int tile, int elevation, int anim, int delay);
int register_object_animate_and_move_straight(Object* owner, int tile, int elev, int anim, int delay);
int register_object_move_on_stairs(Object* owner, Object* stairs, int delay);
int register_object_check_falling(Object* owner, int delay);
int register_object_animate(Object* owner, int anim, int delay);
int register_object_animate_reverse(Object* owner, int anim, int delay);
int register_object_animate_and_hide(Object* owner, int anim, int delay);
int register_object_turn_towards(Object* owner, int tile);
int register_object_inc_rotation(Object* owner);
int register_object_dec_rotation(Object* owner);
int register_object_erase(Object* object);
int register_object_must_erase(Object* object);
int register_object_call(void* a1, void* a2, AnimationCallback* proc, int delay);
int register_object_call3(void* a1, void* a2, void* a3, AnimationCallback3* proc, int delay);
int register_object_must_call(void* a1, void* a2, AnimationCallback* proc, int delay);
int register_object_fset(Object* object, int flag, int delay);
int register_object_funset(Object* object, int flag, int delay);
int register_object_flatten(Object* object, int delay);
int register_object_change_fid(Object* owner, int fid, int delay);
int register_object_take_out(Object* owner, int weaponAnimationCode, int delay);
int register_object_light(Object* owner, int lightDistance, int delay);
int register_object_outline(Object* object, bool outline, int delay);
int register_object_play_sfx(Object* owner, const char* soundEffectName, int delay);
int register_object_animate_forever(Object* owner, int anim, int delay);
int register_ping(int a1, int a2);
int make_path(Object* object, int from, int to, unsigned char* a4, int a5);
// make_path with its own search budget (vanilla's was 2000 closed nodes) -- see anim.cc.
int make_path_capped(Object* object, int from, int to, unsigned char* rotations, int a5, int maxClosed);
int make_path_func(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback);
int idist(int a1, int a2, int a3, int a4);
int EST(int tile1, int tile2);
int make_straight_path(Object* a1, int from, int to, StraightPathNode* pathNodes, Object** a5, int a6);
int make_straight_path_func(Object* a1, int from, int to, StraightPathNode* pathNodes, Object** a5, int a6, PathBuilderCallback* callback);
int anim_move_on_stairs(Object* obj, int tile, int elevation, int anim, int animationSequenceIndex);
int check_for_falling(Object* obj, int anim, int a3);
void object_animate();
int check_move(int* a1);
// MODS page: live travel-speed lever (see travel_speed_scale in anim.cc). 1.0 = vanilla speed.
float anim_get_travel_speed();
float anim_travel_speed_in_effect();
void anim_set_travel_speed(float scale);
// MODS "Reload animation" (cfg reload_anim): a reload plays the weapon's STAND loop (dude and NPCs)
// and the dude stops idling. anim_set_reload_timing brackets the register_end that starts one.
bool anim_get_reload_anim();
void anim_set_reload_anim(bool on);
void anim_set_reload_timing(Object* critter);
// Brackets the registration (and the lock measurement) of a DRUG INJECTION: compute_tpf times it by
// (AGI + INT + PE) / 3 on the reload/swap curve. See anim.cc.
void anim_set_injection_timing(Object* critter);
// The reload animation (inside register_begin/end): the weapon's STAND loop, or for a pistol a
// holster-and-draw (put-away + take-out). anim_is_reload_pose: is the body still in it?
int register_object_reload(Object* critter);
bool anim_is_reload_pose(Object* critter);
// The routing verdict of the last move that STARTED (make_path steps; 0 = no route, -1 = no move
// for that object since the reset). A queued move's register_end cannot tell you this.
void anim_reset_last_path();
int anim_last_path_steps(Object* obj);

int dude_move(int a1);
int dude_run(int a1);
void dude_fidget();
void dude_stand(Object* obj, int rotation, int fid);
void dude_standup(Object* a1);
int anim_hide(Object* object, int animationSequenceIndex);
int anim_change_fid(Object* obj, int animationSequenceIndex, int fid);
void anim_stop();
unsigned int compute_tpf(Object* object, int fid);

// Realtime combat: multiplies the ticks-per-frame of subsequently-registered
// animations (set just before registering an attack, reset to 1.0 right after)
// so an attack's animation duration tracks its cooldown. 1.0 = unscaled.
void anim_set_speed_scale(float scale);

// Movement-only (WALK / RUN) speed multiplier, as a SPEED: 1.0 = stock, higher = faster. Applied on
// top of anim_set_speed_scale and only to those two animation types.
void anim_set_move_speed_scale(float scale);

// Per-point AGILITY scaling of the weapon put-away / take-out animations (realtime only).
// Symmetric curve; 0 = off.
void anim_set_weapon_swap_stat_factor(float factor);

// Realtime combat: appends `count` steps in `rotation` to obj's in-flight walk/
// run animation so it keeps walking without restarting the frame cycle. Returns
// 0 on success or a negative reason code (see definition in anim.cc).
int anim_extend_move(Object* obj, int rotation, int count);
int anim_move_pending_steps(Object* obj);
int anim_move_repoint(Object* obj, const int* rots, int count, int* newDest);
void anim_move_speed(Object* object, int anim, float* tilesPerSec, float* framesPerSec);

// SMOOTH MOVEMENT (MODS row, cfg smooth_motion). A walking, running or flying object is DRAWN
// gliding between the animation's own steps instead of jumping 10-20 times a second; its logical
// position (tile, obj->x/y) is untouched. object.cc adds the offset wherever it draws, bounds or
// hit-tests an object -- returns false (and 0,0) for anything that is not gliding.
bool anim_smooth_offset(Object* obj, int* dx, int* dy);
bool anim_get_smooth_motion();
void anim_set_smooth_motion(bool on);
// Every frame (a background process, after object_animate): advances the glides and redraws them.
void anim_smooth_tick();
// The map is being torn down: forget every glide and every held stride (no redraw).
void anim_smooth_reset();

// FRAME-PACING METER (main loop). Counters since the last call, then zeroed: path searches and
// their cost, glides started/abandoned, strides held/stood, leg starts held for the cadence.
typedef struct AnimSmoothStats {
    int pathSearches;
    double pathMs;
    double pathMaxMs;
    int glides;
    int glideSnaps;
    int liveGlides;
    int strideHolds;
    int strideStands;
    int cadenceHolds;
} AnimSmoothStats;
void anim_smooth_take_stats(AnimSmoothStats* out);

} // namespace fallout

#endif /* FALLOUT_GAME_ANIMATION_H_ */
