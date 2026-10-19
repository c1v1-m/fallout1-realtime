#include "game/anim.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h> // TEMP: walk-speed diagnosis logger

#include "game/art.h"
#include "game/combat.h"
#include "game/combat_defs.h"
#include "game/combat_rt.h"
#include "game/combatai.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/game_trace.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/proto.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "game/trait.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/vcr.h"

namespace fallout {

// Realtime-combat capacity bump: the originals (21 / 16) were sized for turn-based
// play, where only a handful of objects animate at once. Realtime combat (combat_rt)
// runs every actor concurrently -- a 30+ critter brawl has all of them moving/chasing
// at the same time, each consuming one anim_set sequence AND one sad (active-playback)
// slot, plus per-attack swing/fire, projectile, and death sequences. The 16-slot sad
// list was the tighter bottleneck: once full, anim_animate() returns -1 so the engine
// SILENTLY drops attack/death animations (combat_attack still commits the damage), which
// looked like "enemies hit me with no animation / corpses appear without a death fall".
// 64 comfortably exceeds the ~32-actor fights seen in play-tests with headroom. These
// are runtime-only arrays (never serialized), so enlarging them is safe.
#define ANIMATION_SEQUENCE_LIST_CAPACITY 64
#define ANIMATION_DESCRIPTION_LIST_CAPACITY 40
#define ANIMATION_SAD_LIST_CAPACITY 64

#define ANIMATION_SEQUENCE_FORCED 0x01

typedef enum AnimationKind {
    ANIM_KIND_MOVE_TO_OBJECT = 0,
    ANIM_KIND_MOVE_TO_TILE = 1,
    ANIM_KIND_MOVE_TO_TILE_STRAIGHT = 2,
    ANIM_KIND_MOVE_TO_TILE_STRAIGHT_AND_WAIT_FOR_COMPLETE = 3,
    ANIM_KIND_ANIMATE = 4,
    ANIM_KIND_ANIMATE_REVERSED = 5,
    ANIM_KIND_ANIMATE_AND_HIDE = 6,
    ANIM_KIND_ROTATE_TO_TILE = 7,
    ANIM_KIND_ROTATE_CLOCKWISE = 8,
    ANIM_KIND_ROTATE_COUNTER_CLOCKWISE = 9,
    ANIM_KIND_HIDE = 10,
    ANIM_KIND_CALLBACK = 11,
    ANIM_KIND_CALLBACK3 = 12,
    ANIM_KIND_SET_FLAG = 14,
    ANIM_KIND_UNSET_FLAG = 15,
    ANIM_KIND_TOGGLE_FLAT = 16,
    ANIM_KIND_SET_FID = 17,
    ANIM_KIND_TAKE_OUT_WEAPON = 18,
    ANIM_KIND_SET_LIGHT_DISTANCE = 19,
    ANIM_KIND_MOVE_ON_STAIRS = 20,
    ANIM_KIND_CHECK_FALLING = 23,
    ANIM_KIND_TOGGLE_OUTLINE = 24,
    ANIM_KIND_ANIMATE_FOREVER = 25,
    ANIM_KIND_26 = 26,
    ANIM_KIND_27 = 27,
    ANIM_KIND_NOOP = 28,
} AnimationKind;

typedef enum AnimationSequenceFlags {
    // Specifies that the animation sequence has high priority, it cannot be
    // cleared.
    ANIM_SEQ_PRIORITIZED = 0x01,

    // Specifies that the animation sequence started combat animation mode and
    // therefore should balance it with appropriate finish call.
    ANIM_SEQ_COMBAT_ANIM_STARTED = 0x02,

    // Specifies that the animation sequence is reserved (TODO: explain what it
    // actually means).
    ANIM_SEQ_RESERVED = 0x04,

    // Specifies that the animation sequence is in the process of adding actions
    // to it (that is in the middle of begin/end calls).
    ANIM_SEQ_ACCUMULATING = 0x08,

    // TODO: Add description.
    ANIM_SEQ_0x10 = 0x10,

    // TODO: Add description.
    ANIM_SEQ_0x20 = 0x20,

    // Specifies that the animation sequence is negligible and will be end
    // immediately when a new animation sequence is requested for the same
    // object.
    ANIM_SEQ_INSIGNIFICANT = 0x40,

    // Specifies that the animation sequence should not return to ANIM_STAND
    // when it's completed.
    ANIM_SEQ_NO_STAND = 0x80,

    // CE: ANIMATION_REQUEST_DEFER_STAND -- one leg of a longer move. Its end holds the body in its
    // stride for a beat (anim_defer_stand) instead of standing it at once.
    ANIM_SEQ_DEFER_STAND = 0x100,
} AnimationSequenceFlags;

typedef enum AnimationSadFlags {
    // Specifies that the animation should play from end to start.
    ANIM_SAD_REVERSE = 0x01,

    // Specifies that the animation should use straight movement mode (as
    // opposed to normal movement mode).
    ANIM_SAD_STRAIGHT = 0x02,

    // Specifies that no frame change should occur during animation.
    ANIM_SAD_NO_ANIM = 0x04,

    // Specifies that the animation should be played fully from start to finish.
    //
    // NOTE: This flag is only used together with straight movement mode to
    // implement knockdown. Without this flag when the knockdown distance is
    // short, say 1 or 2 tiles, knockdown animation might not be completed by
    // the time critter reached it's destination. With this flag set animation
    // will be played to it's final frame.
    ANIM_SAD_WAIT_FOR_COMPLETION = 0x10,

    // Unknown, set once, never read.
    ANIM_SAD_0x20 = 0x20,

    // Specifies that the owner of the animation should be hidden when animation
    // is completed.
    ANIM_SAD_HIDE_ON_END = 0x40,

    // Specifies that the animation should never end.
    ANIM_SAD_FOREVER = 0x80,
} AnimationSadFlags;

typedef struct AnimationDescription {
    int kind;
    union {
        Object* owner;

        // - ANIM_KIND_CALLBACK
        // - ANIM_KIND_CALLBACK3
        void* param2;
    };

    union {
        // - ANIM_KIND_MOVE_TO_OBJECT
        Object* destination;

        // - ANIM_KIND_CALLBACK
        void* param1;
    };
    union {
        // - ANIM_KIND_MOVE_TO_TILE
        // - ANIM_KIND_ANIMATE_AND_MOVE_TO_TILE_STRAIGHT
        // - ANIM_KIND_MOVE_TO_TILE_STRAIGHT
        struct {
            int tile;
            int elevation;
        };

        // ANIM_KIND_SET_FID
        int fid;

        // ANIM_KIND_TAKE_OUT_WEAPON
        int weaponAnimationCode;

        // ANIM_KIND_SET_LIGHT_DISTANCE
        int lightDistance;

        // ANIM_KIND_TOGGLE_OUTLINE
        bool outline;
    };
    int anim;
    int delay;

    // ANIM_KIND_CALLBACK
    AnimationCallback* callback;

    // ANIM_KIND_CALLBACK3
    AnimationCallback3* callback3;

    union {
        // - ANIM_KIND_SET_FLAG
        // - ANIM_KIND_UNSET_FLAG
        unsigned int objectFlag;

        // - ANIM_KIND_HIDE
        // - ANIM_KIND_CALLBACK
        unsigned int extendedFlags;
    };

    union {
        // - ANIM_KIND_MOVE_TO_TILE
        // - ANIM_KIND_MOVE_TO_OBJECT
        int actionPoints;

        // ANIM_KIND_26
        int animationSequenceIndex;

        // ANIM_KIND_CALLBACK3
        void* param3;
    };
    CacheEntry* artCacheKey;
} AnimationDescription;

typedef struct AnimationSequence {
    int field_0;
    // Index of current animation in [animations] array or -1 if animations in
    // this sequence is not playing.
    int animationIndex;
    // Number of scheduled animations in [animations] array.
    int length;
    unsigned int flags;
    AnimationDescription animations[ANIMATION_DESCRIPTION_LIST_CAPACITY];
} AnimationSequence;

typedef struct PathNode {
    int tile;
    int from;
    // actual type is likely char
    int rotation;
    int field_C;
    int field_10;
} PathNode;

// TODO: I don't know what `sad` means, but it's definitely better than
// `STRUCT_530014`. Find a better name.
typedef struct AnimationSad {
    unsigned int flags;
    Object* obj;
    int fid; // fid
    int anim;

    // Timestamp (in game ticks) when animation last occurred.
    unsigned int animationTimestamp;

    // Number of ticks per frame (taking art's fps and overall animation speed
    // settings into account).
    unsigned int ticksPerFrame;

    int animationSequenceIndex;
    int field_1C; // length of field_28
    int field_20; // current index in field_28
    int field_24;
    int startFrame; // > 0: the animation's first tick jumps here (anim_set_start_frame)
    union {
        unsigned char rotations[3200];
        StraightPathNode field_28[200];
    };
} AnimationSad;

static int anim_free_slot(int a1);
static int anim_preload(Object* object, int fid, CacheEntry** cacheEntryPtr);
static void anim_cleanup();
static int anim_set_check(int a1);
static int anim_set_continue(int a1, int a2);
static int anim_set_end(int a1);
static bool anim_can_use_door(Object* critter, Object* door);
static int anim_move_to_object(Object* from, Object* to, int a3, int anim, int animationSequenceIndex);
static int make_stair_path(Object* object, int from, int fromElevation, int to, int toElevation, StraightPathNode* a6, Object** obstaclePtr);
static int anim_move_to_tile(Object* obj, int tile_num, int elev, int a4, int anim, int animationSequenceIndex);
static int anim_move(Object* obj, int tile, int elev, int a3, int anim, int a5, int animationSequenceIndex);
static int anim_move_straight_to_tile(Object* obj, int tile, int elevation, int anim, int animationSequenceIndex, int flags);
static void object_move(int index);
static void object_straight_move(int index);
static float walk_dir_slowdown(Object* object, int rotation);
static float crippled_move_scale(Object* object);
static float travel_speed_scale(Object* object);
float anim_get_travel_speed();
static float run_speed_scale(Object* object, int rotation);
static int walk_smooth_offset(Object* object, int rotation, int anim, int* frameX, int* frameY);
static void walk_smooth_seed(Object* object, int x, int y);
static int anim_animate(Object* obj, int anim, int animationSequenceIndex, int flags);
static void object_anim_compact();
static int anim_turn_towards(Object* obj, int delta, int animationSequenceIndex);
static int check_gravity(int tile, int elevation);
static bool anim_smooth_begin(Object* obj, float* drawnX, float* drawnY);
static void anim_smooth_end(Object* obj, float drawnX, float drawnY, unsigned int stepMs, Rect* dirty);
static void anim_smooth_forget(Object* obj);
static void anim_smooth_settle(Object* obj, const Rect* before);
static bool anim_defer_stand(Object* obj);
static void anim_deferred_stands(bool now);
static void anim_note_move_step(Object* obj, int anim, unsigned int time, unsigned int stepMs);
static bool anim_move_cadence_hold(Object* obj, int anim, unsigned int time);
static int make_path_func_impl(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback);

// 0x4FEA98
static int curr_sad = 0;

// 0x4FEA9C
static int curr_anim_set = -1;

// 0x4FEAA0
static bool anim_in_init = false;

// 0x4FEAA4
static bool anim_in_anim_stop = false;

// 0x4FEAA8
static bool anim_in_bk = false;

// 0x540014
static AnimationSad sad[ANIMATION_SAD_LIST_CAPACITY];

// CE: make_path_func's working set (it replaced the vanilla dad[2000] / child[2000] lists -- see
// the note on that function). Indexed by TILE, so a node's parent, entering rotation and cost are a
// single load instead of a scan of the closed list.
typedef struct PathHeapEntry {
    int f; // cost so far + estimate to the goal, the vanilla scoring exactly
    int seq; // insertion order: among equal scores the oldest node goes first
    int tile;
} PathHeapEntry;

static PathHeapEntry path_heap[HEX_GRID_SIZE];
static int path_parent[HEX_GRID_SIZE];
static unsigned char path_rotation[HEX_GRID_SIZE];
static int path_cost[HEX_GRID_SIZE];
static unsigned char path_scratch[HEX_GRID_SIZE];

// 0x560314
static AnimationSequence anim_set[ANIMATION_SEQUENCE_LIST_CAPACITY];

// Which sequences are CURRENTLY inside anim_set_end. Not a status flag -- a re-entrancy guard,
// and the fix for a runaway recursion that overflowed the stack (Junktown, 2026-08-28: the
// bartender killing the Skulz who killed the girl). The cycle, straight out of the crash digest:
//
//     obj_erase_object -> anim_notify_object_freed -> anim_force_clear -> anim_set_end
//         -> obj_erase_object (its ANIM_KIND_HIDE owners)  -> ...and round again, for ever.
//
// The loop closes because anim_set_end marks the sequence dead (field_0 = -1000) only at its very
// END, after the loop that erases the HIDE owners -- so the nested call sails past the dead check,
// finds the SAME sequence, erases the SAME owner, and recurses. (The free at the bottom of
// obj_erase_object is never reached, so nothing ever runs out except the stack.)
//
// ⚠️ Realtime-only in practice, like the hook that exposed it: turn-based erases objects between
// turns, when no sequence is mid-teardown.
static bool anim_set_ending[ANIMATION_SEQUENCE_LIST_CAPACITY];

// 0x56A1E4
static unsigned char seen[5000];

// 0x56B56C
static int curr_anim_counter;

// 0x4134B0
// Per-stat scaling of ATTACK animation speed (realtime combat only). Loaded once from
// config at anim_init. Non-attack anims use anim_speed_scale; attack anims instead use a
// multiplier derived purely from the attacker's SPECIAL stat: melee by STRENGTH, ranged
// by PERCEPTION. multiplier = 1 - (stat - 5) * anim_attack_stat_factor, clamped to
// ANIM_ATTACK_SPEED_MIN..MAX. See compute_tpf().
static float anim_attack_stat_factor = 0.09f;
#define ANIM_ATTACK_SPEED_MIN 0.30f
#define ANIM_ATTACK_SPEED_MAX 2.0f

// The curve is ASYMMETRIC around the average (2026-08-10, on request: "bigger differences between
// 5 and 10"). Every stat curve in this build is linear and centred on 5, so each point buys the
// same amount and pushing a stat from 5 to 10 feels like nothing in particular.
//
// Attack-animation speed is the one place we can afford to exaggerate that, precisely BECAUSE it
// is cosmetic: the AP pool paces the attacks (the animation is ~14% of a cycle, measured), so a
// dramatic curve here buys a visibly faster gunslinger WITHOUT touching throughput or balance.
// Above-average is steepened by this; below-average keeps the plain factor, so a weak character is
// not punished twice over. At the default 0.09 that is stat 5 -> 1.00, 7 -> 0.73, 10 -> 0.325 --
// a 3.1x spread from average to maxed, where the symmetric curve gave only 1.8x.
// ANIM_ATTACK_SPEED_MIN was lowered from 0.4 to make room: at 0.4 the clamp bit from stat ~8 up
// and 8/9/10 all animated identically, which is exactly the flat top this is meant to remove.
//
// ⚠️ 2026-08-24: 1.5 -> 1.0 (SYMMETRIC), and promoted from a #define to a cfg key.
//
// This constant and `attack_cooldown_high_bias` shape the SAME half of the SAME curve, and they
// had silently DRIFTED APART pointing in opposite directions: this one AMPLIFIES above-average
// stats (1.5), while the cooldown's was damped to 0.6 on 2026-08-23. They started life equal (both
// 1.5); the cooldown then went 1.5 -> 1.0 -> 0.6 across three balance rounds and nobody moved this
// one with it.
//
// The visible result at STR 10 / unarmed 112: the punch played in 0.35 s (11 frames at 32 ms, i.e.
// 3.1x its authored 10 fps) while attacks only came 1.56x more often -- so the DUTY CYCLE, flat at
// 40-43% for stats 1..5, collapsed to 22%. The character stood motionless four fifths of the fight
// and twitched. The animation was promising a rate the cooldown no longer delivered.
//
// At 1.0 the whole 1..10 range lands on 37-43%, i.e. the same band stats 1..5 already occupied.
// STR 10 still animates visibly faster than STR 5 (0.60 s vs 1.10 s), it just isn't a flicker.
//
// ⚠️ The old note above -- "we can afford to exaggerate BECAUSE it is cosmetic" -- is still true
// about BALANCE (this touches no throughput, hit chance or damage) but was wrong about READABILITY.
// A cosmetic lever cannot be tuned in isolation from the pacing lever it renders.
#define ANIM_ATTACK_STAT_HIGH_BIAS 1.0f

// How much faster a shot's own projectile flies in realtime (see compute_tpf).
#define RT_PROJECTILE_SPEEDUP 4.0f
// ...and how much SLOWER a thrown weapon flies (a grenade: 10 -> 5 tiles a second), 2026-10-16.
#define RT_THROWN_SLOWDOWN 2.0f
static float anim_attack_stat_high_bias = ANIM_ATTACK_STAT_HIGH_BIAS;
static float anim_projectile_speedup = -1.0f; // > 0: anim_set_projectile_speedup (autotest) overrides the table

// ⚠️ EACH KIND OF SHOT AT ITS OWN SPEED (2026-10-18, the projectiles -- his words: "laser bude ještě
// rychlejší, skoro insta. Rakety, plazma, granáty a plamen pomalejší"; his pick after the self-played
// rocket-dodge measurements: rockets and plasma at the art's own pace, ~10 tiles a second, so a player
// can see one coming and step out of it). A projectile's own art walks a tile a frame; these are
// multipliers on that. Every other created projectile keeps RT_PROJECTILE_SPEEDUP.
#define RT_ROCKET_SPEEDUP 1.0f
#define RT_PLASMA_SPEEDUP 1.0f
#define RT_LASER_SPEEDUP 100.0f // light: as fast as the frame allows (2026-10-18, "jako v realu")

// The projectile art of the rocket (misc proto 1), the plasma ball (2) and the laser bolt (9), looked
// up once from the protos a weapon names as its projectile.
static float anim_projectile_kind_speedup(int fid)
{
    static int rocketFid = -2;
    static int plasmaFid = -2;
    static int laserFid = -2;
    if (rocketFid == -2) {
        Proto* proto;
        rocketFid = proto_ptr(0x05000001, &proto) != -1 && proto != NULL ? proto->fid : -1;
        plasmaFid = proto_ptr(0x05000002, &proto) != -1 && proto != NULL ? proto->fid : -1;
        laserFid = proto_ptr(0x05000009, &proto) != -1 && proto != NULL ? proto->fid : -1;
    }
    if (anim_projectile_speedup > 0.0f) {
        return anim_projectile_speedup;
    }
    int artId = fid & 0xFFF;
    if (rocketFid != -1 && artId == (rocketFid & 0xFFF)) {
        return RT_ROCKET_SPEEDUP;
    }
    if (plasmaFid != -1 && artId == (plasmaFid & 0xFFF)) {
        return RT_PLASMA_SPEEDUP;
    }
    if (laserFid != -1 && artId == (laserFid & 0xFFF)) {
        return RT_LASER_SPEEDUP;
    }
    return RT_PROJECTILE_SPEEDUP;
}

void anim_init()
{
    anim_in_init = true;
    anim_reset();
    anim_in_init = false;

    // Global animation-speed multiplier from config (gconfig_init ran first). This
    // is the one snappiness lever: compute_tpf scales every animation's tpf by it,
    // symmetrically for the dude and all NPCs (no obj_dude exclusion). Clamped so a
    // bad cfg value can't stall (too high) or near-freeze (~0) the animation system.
    double animSpeed = 1.0;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ANIM_SPEED_KEY, &animSpeed);
    if (animSpeed < 0.1) {
        animSpeed = 0.1;
    } else if (animSpeed > 3.0) {
        animSpeed = 3.0;
    }
    anim_set_speed_scale((float)animSpeed);

    // Movement-only speed (WALK / RUN). A SPEED, so higher = faster. Clamped either side of 1.0:
    // below ~0.3 a chase would never close, above ~3 the sprite skates across tiles faster than the
    // art can read.
    double moveSpeed = 1.3;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MOVE_SPEED_KEY, &moveSpeed);
    if (moveSpeed < 0.3) {
        moveSpeed = 0.3;
    } else if (moveSpeed > 3.0) {
        moveSpeed = 3.0;
    }
    anim_set_move_speed_scale((float)moveSpeed);

    // Weapon-swap speed by AGILITY. Same clamp range as the attack curve; 0 disables.
    double swapFactor = 0.09;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_WEAPON_SWAP_STAT_FACTOR_KEY, &swapFactor);
    if (swapFactor < 0.0) {
        swapFactor = 0.0;
    } else if (swapFactor > 0.3) {
        swapFactor = 0.3;
    }
    anim_set_weapon_swap_stat_factor((float)swapFactor);

    // Per-stat attack-animation speed factor (realtime only). Attack anims ignore the
    // global anim_speed above and are driven purely by the attacker's STR (melee) /
    // PE (ranged). Floored at 0 (no negative scaling) and capped so the spread stays
    // sane; the final per-attack multiplier is additionally clamped in compute_tpf.
    double attackStatFactor = 0.09;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ANIM_ATTACK_STAT_FACTOR_KEY, &attackStatFactor);
    if (attackStatFactor < 0.0) {
        attackStatFactor = 0.0;
    } else if (attackStatFactor > 0.3) {
        attackStatFactor = 0.3;
    }
    anim_attack_stat_factor = (float)attackStatFactor;

    // How much of that factor applies ABOVE the average stat of 5 (below 5 always uses the plain
    // factor). 1.0 = symmetric. Keep it in step with `attack_cooldown_high_bias` -- see the note at
    // ANIM_ATTACK_STAT_HIGH_BIAS for what drifting apart looked like. Capped at 2.0 so the clamp in
    // compute_tpf stays the exception rather than the rule.
    double attackStatHighBias = ANIM_ATTACK_STAT_HIGH_BIAS;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ANIM_ATTACK_STAT_HIGH_BIAS_KEY, &attackStatHighBias);
    if (attackStatHighBias < 0.0) {
        attackStatHighBias = 0.0;
    } else if (attackStatHighBias > 2.0) {
        attackStatHighBias = 2.0;
    }
    anim_attack_stat_high_bias = (float)attackStatHighBias;

    // MODS "Smooth movement" -- default ON since the release (his setting); 0 = the original
    // step-per-frame motion.
    int smoothMotion = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SMOOTH_MOTION_KEY, &smoothMotion);
    anim_set_smooth_motion(smoothMotion != 0);
}

// 0x4134D0
void anim_reset()
{
    int index;

    if (!anim_in_init) {
        // NOTE: Uninline.
        anim_stop();
    }

    curr_sad = 0;
    curr_anim_set = -1;

    for (index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        anim_set[index].field_0 = -1000;
        anim_set[index].flags = 0;
        anim_set_ending[index] = false;
    }
}

// 0x413548
void anim_exit()
{
    // NOTE: Uninline.
    anim_stop();
}

// 0x413584
int register_begin(int requestOptions)
{
    if (curr_anim_set != -1) {
        return -1;
    }

    if (anim_in_anim_stop) {
        return -1;
    }

    int v1 = anim_free_slot(requestOptions);
    if (v1 == -1) {
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[v1]);
    animationSequence->flags |= ANIM_SEQ_ACCUMULATING;

    if ((requestOptions & ANIMATION_REQUEST_RESERVED) != 0) {
        animationSequence->flags |= ANIM_SEQ_RESERVED;
    }

    if ((requestOptions & ANIMATION_REQUEST_INSIGNIFICANT) != 0) {
        animationSequence->flags |= ANIM_SEQ_INSIGNIFICANT;
    }

    if ((requestOptions & ANIMATION_REQUEST_NO_STAND) != 0) {
        animationSequence->flags |= ANIM_SEQ_NO_STAND;
    }

    if ((requestOptions & ANIMATION_REQUEST_DEFER_STAND) != 0) {
        animationSequence->flags |= ANIM_SEQ_DEFER_STAND;
    }

    curr_anim_set = v1;

    curr_anim_counter = 0;

    return 0;
}

// TEMP DIAG (realtime attack-not-animating): report the animation table state so
// combat_rt can log, at the instant an attack registers, whether it hit a FULL table
// (occupied == ANIMATION_SEQUENCE_LIST_CAPACITY -> reserved register_begin returns -1)
// or registered over a LEAKED open transaction (curr != -1 -> register_begin returns -1
// for a different reason). occupied/nonReserved mirror anim_free_slot's occupancy test.
// STRIP with the rest of the temp loggers.
void anim_debug_table(int* occupied, int* nonReserved, int* curr)
{
    int occ = 0;
    int nr = 0;
    for (int i = 0; i < ANIMATION_SEQUENCE_LIST_CAPACITY; i++) {
        AnimationSequence* s = &(anim_set[i]);
        if (s->field_0 != -1000 || (s->flags & ANIM_SEQ_ACCUMULATING) != 0 || (s->flags & ANIM_SEQ_0x20) != 0) {
            occ++;
            if (!(s->flags & ANIM_SEQ_RESERVED)) {
                nr++;
            }
        }
    }
    if (occupied != NULL) *occupied = occ;
    if (nonReserved != NULL) *nonReserved = nr;
    if (curr != NULL) *curr = curr_anim_set;
}

// RECOVERY for a register_begin transaction nobody closed.
//
// register_begin refuses outright while `curr_anim_set != -1` -- it is a one-at-a-time
// transaction and an unclosed one poisons EVERY later attempt, for every actor. The symptom
// is total and silent: attacks return -1 forever, the dude and every NPC alike, with the
// animation table nearly empty and all the busy-tests reading zero.
//
// Vanilla never had to care: register_begin/register_end always ran to completion inside one
// turn. Realtime and a mid-session LoadGame can both leave one dangling -- loadsave.cc calls
// neither anim_stop() nor anim_reset(), so whatever was open at save time survives the load.
//
// Abandoning the slot is safe precisely BECAUSE it never started: an unclosed transaction has
// no running animation and no callbacks pending -- register_end is what would have made it
// live. Returns true if one was actually discarded, so the caller can log it.
bool anim_discard_open_transaction()
{
    if (curr_anim_set == -1) {
        return false;
    }

    anim_set[curr_anim_set].field_0 = -1000;
    anim_set[curr_anim_set].flags = 0;
    curr_anim_set = -1;
    curr_anim_counter = 0;
    return true;
}

// Reports the two GLOBAL reasons register_begin can refuse, so a caller that just had an
// attack rejected can say which one it was instead of guessing.
void anim_debug_gate(int* openTransaction, int* inAnimStop)
{
    if (openTransaction != NULL) {
        *openTransaction = curr_anim_set;
    }
    if (inAnimStop != NULL) {
        *inAnimStop = anim_in_anim_stop ? 1 : 0;
    }
}

// Set for the duration of an attack's registration (action_attack wraps action_melee /
// action_ranged in it). check_registry refuses far more often than anim_busy reports -- it
// vetoes on ANY live sequence that merely lists the object as an owner, including a length-1
// ANIM_STAND that anim_busy skips outright -- and that refusal tears the transaction down via
// anim_cleanup, so register_end returns -1 and the whole attack is dropped with every busy
// test reading zero. Scoped, because check_registry runs for every walk in the game and an
// unconditional log would drown the recorder.
static bool anim_dbg_attack_scope = false;

void anim_debug_attack_scope(bool on)
{
    anim_dbg_attack_scope = on;
}

// 0x413630
static int anim_free_slot(int requestOptions)
{
    int v1 = -1;
    int v2 = 0;
    for (int index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        AnimationSequence* animationSequence = &(anim_set[index]);
        if (animationSequence->field_0 != -1000 || (animationSequence->flags & ANIM_SEQ_ACCUMULATING) != 0 || (animationSequence->flags & ANIM_SEQ_0x20) != 0) {
            if (!(animationSequence->flags & ANIM_SEQ_RESERVED)) {
                v2++;
            }
        } else if (v1 == -1 && ((requestOptions & ANIMATION_REQUEST_0x100) == 0 || (animationSequence->flags & ANIM_SEQ_0x10) == 0)) {
            v1 = index;
        }
    }

    if (v1 == -1) {
        if ((requestOptions & ANIMATION_REQUEST_RESERVED) != 0) {
            debug_printf("Unable to begin reserved animation!\n");
        }

        return -1;
    } else if ((requestOptions & ANIMATION_REQUEST_RESERVED) != 0 || v2 < 13) {
        return v1;
    }

    return -1;
}

// 0x4136D0
int register_priority(int a1)
{
    if (curr_anim_set == -1) {
        return -1;
    }

    if (a1 == 0) {
        return -1;
    }

    anim_set[curr_anim_set].flags |= ANIM_SEQ_PRIORITIZED;

    return 0;
}

// 0x413708
int register_clear(Object* a1)
{
    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequence->field_0 == -1000) {
            continue;
        }

        int animationDescriptionIndex;
        for (animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
            AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
            if (a1 != animationDescription->owner || animationDescription->kind == 11) {
                continue;
            }

            break;
        }

        if (animationDescriptionIndex == animationSequence->length) {
            continue;
        }

        if ((animationSequence->flags & ANIM_SEQ_PRIORITIZED) != 0) {
            return -2;
        }

        anim_set_end(animationSequenceIndex);

        return 0;
    }

    return -1;
}

// register_clear() for callers that are only allowed to interrupt work `a1` still OWES.
//
// ⚠️ WHY THIS EXISTS. register_clear above ends the FIRST live sequence that mentions a1 at
// all -- including one where a1's only description has ALREADY played and the sequence has
// moved on to animating somebody else. In vanilla that is harmless: nothing runs while a
// sequence is playing. In realtime it is a trap, because anim_busy_blocking() deliberately
// frees the player to walk while his victim's death is still running in the sequence that
// also contains his (spent) swing. A plain register_clear on the next direction change would
// then end THAT sequence and freeze the corpse mid-fall -- the "body stuck in the air" bug.
//
// So this variant applies the same already-played skip as anim_busy_blocking and only ever
// ends a sequence where a1 genuinely still has something left to animate.
static int anim_clear_pending_ex(Object* a1, bool evenPrioritized);

int anim_clear_pending(Object* a1)
{
    return anim_clear_pending_ex(a1, false);
}

// anim_clear_pending that also ends a PRIORITIZED sequence -- every attack is one. For combat_rt's
// shoot-and-move (rt_shot_tail_cut): once the shot has left the gun, the rest of the shooter's own
// attack sequence (the fire pose's last frames, the lowering) is cut so the body can move. Same
// already-played skip as anim_clear_pending, so it can only ever end a sequence where a1 still has
// something left to animate -- never one that is only dropping somebody else's corpse.
int anim_cut_pending(Object* a1)
{
    return anim_clear_pending_ex(a1, true);
}

static int anim_clear_pending_ex(Object* a1, bool evenPrioritized)
{
    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequence->field_0 == -1000) {
            continue;
        }

        int animationDescriptionIndex;
        for (animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
            if (animationSequence->animationIndex >= 0 && animationDescriptionIndex < animationSequence->animationIndex) {
                continue;
            }

            AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
            if (a1 != animationDescription->owner || animationDescription->kind == 11) {
                continue;
            }

            break;
        }

        if (animationDescriptionIndex == animationSequence->length) {
            continue;
        }

        if ((animationSequence->flags & ANIM_SEQ_PRIORITIZED) != 0 && !evenPrioritized) {
            return -2;
        }

        anim_set_end(animationSequenceIndex);

        return 0;
    }

    return -1;
}

// ⚠️ A LONE STAND (2026-10-16, "animace běhu se zasekávala"). A sequence holding nothing but one
// ANIM_STAND for `obj` -- what a script's anim(obj, ANIM_STAND, 0) registers; Junktown's ladder
// JTDN2UP.INT does it to the dude after its move_to -- is invisible to anim_busy and
// anim_busy_blocking alike (both skip "length == 1 && ANIM_STAND"), so a mover that asks either of
// them takes the body while the STAND's sad still lives. Free movement writes the walk pose every
// frame; the sad finds a fid not its own on each of its ticks and stands the body again at frame 0
// (object_animate's fid-change branch), and since it never sees its own fid it never finishes: the
// stride restarted every step for as long as he walked (trace_2026-10-16_004402, s=9.26..10.00 at
// the cap of the fm-pose-cut diag). Standing is moot for a body about to walk, so the movers end
// these first. ANIM_SEQ_NO_STAND keeps anim_set_end from dude_stand-ing him -- that would snap a
// free-moving body back onto its tile centre. Returns how many it ended.
int anim_end_lone_stand(Object* obj)
{
    if (obj == NULL) {
        return 0;
    }

    int ended = 0;
    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequenceIndex == curr_anim_set || animationSequence->field_0 == -1000) {
            continue;
        }

        if (animationSequence->length != 1 || (animationSequence->flags & ANIM_SEQ_PRIORITIZED) != 0) {
            continue;
        }

        AnimationDescription* animationDescription = &(animationSequence->animations[0]);
        if (animationDescription->owner != obj
            || animationDescription->kind != ANIM_KIND_ANIMATE
            || animationDescription->anim != ANIM_STAND) {
            continue;
        }

        animationSequence->flags |= ANIM_SEQ_NO_STAND;
        anim_set_end(animationSequenceIndex);
        ended++;
    }

    return ended;
}

// Like register_clear, but ends the owning sequence even when it is PRIORITIZED (which
// register_clear refuses with -2). Realtime combat uses this on an attack's DEFENDER: in
// turn-based you can't be attacked mid-swing, but in realtime an attacker (incl. an enemy
// striking the busy player) must be able to register its hit on a target that is in its own
// prioritized attack swing -- otherwise the attack just fails and "enemies never hit me while
// I'm attacking." The cut swing's damage is already safe (combat_rt snapshots it before the
// animation), so only the animation is interrupted -- a hit staggers your swing, as it should.
// Returns 0 if a sequence was ended, -1 if the object owned none.
int anim_force_clear(Object* a1)
{
    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequence->field_0 == -1000) {
            continue;
        }

        int animationDescriptionIndex;
        for (animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
            // NO already-played skip here, deliberately. This function's job is "free `a1` so a new
            // animation can be registered for it", and the thing that decides that is
            // check_registry -- which counts SPENT descriptions too. A skip here made the two
            // disagree: a defender holding only a spent description could not be cleared, yet
            // check_registry still refused its death fall, so 6 of 7 kills settled straight to a
            // static corpse with no fall. Keep these two in lockstep.
            AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
            if (a1 != animationDescription->owner || animationDescription->kind == 11) {
                continue;
            }

            break;
        }

        if (animationDescriptionIndex == animationSequence->length) {
            continue;
        }

        anim_set_end(animationSequenceIndex);

        return 0;
    }

    return -1;
}

// ⚠️ A DYING BODY DOES NOT KEEP WALKING (2026-10-14, "když ten týpek umírá ... v tom klubu ten Skulz":
// the death animation looped and the body slid away). A script sent a Skulz ganger on a 111-step
// walk to the off-map pen (tile 7000) and he was killed half a second later by a scripted hit, so his
// death animation started as a SECOND sequence while the walk's was still live. A walk step animates
// whatever fid the body has -- now the death art -- so it ran those 22 frames round and round and
// took their hot-spot shifts for steps: the corpse-to-be cycled its fall and drifted 144 px down the
// screen until the reaper settled it (trace_2026-10-14_210449 s=34.46-37.10, id=1242). Ends every
// live sequence of the object that WALKS or RUNS it somewhere and leaves the
// rest -- the death animation itself -- running. Returns how many were ended.
int anim_end_moves(Object* obj)
{
    if (obj == NULL) {
        return 0;
    }

    int ended = 0;
    for (int index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        AnimationSequence* sequence = &(anim_set[index]);
        if (sequence->field_0 == -1000) {
            continue;
        }
        bool moves = false;
        for (int at = 0; at < sequence->length; at++) {
            AnimationDescription* description = &(sequence->animations[at]);
            if (description->owner != obj) {
                continue;
            }
            // Walking or running only: a KNOCKBACK death is a straight move too, but it moves the body
            // with the fall animation (action_knockback), and that one IS the death.
            switch (description->kind) {
            case ANIM_KIND_MOVE_TO_OBJECT:
            case ANIM_KIND_MOVE_TO_TILE:
            case ANIM_KIND_MOVE_TO_TILE_STRAIGHT:
            case ANIM_KIND_MOVE_TO_TILE_STRAIGHT_AND_WAIT_FOR_COMPLETE:
                if (description->anim == ANIM_WALK || description->anim == ANIM_RUNNING) {
                    moves = true;
                }
                break;
            default:
                break;
            }
        }
        if (moves) {
            anim_set_end(index);
            ended++;
        }
    }
    return ended;
}

// Called from obj_erase_object() (object.cc) in the instant before an Object is freed.
//
// anim.cc holds RAW Object pointers in TWO tables and obj_remove() cleans NEITHER -- it frees the
// Object and returns. The frame stepper then dereferences the dangling pointer on the very next
// frame: object_animate() reads `object->rotation` to feed art_frame_offset(), so a garbage
// rotation indexes art->xOffsets[] far out of bounds. That is the ACCESS_VIOLATION in
// trace_2026-08-27_215442 (read 0x21a40981bfe):
//     art_frame_offset (art.cc:796) <- object_animate (anim.cc:3454) <- process_bk
// The free came from op_destroy_object inside a critter script during doBkProcesses -- ordinary
// vanilla cleanup after the Junktown fight (eight critters erased in half a second).
//
// ⚠️ WHY THIS IS A REALTIME BUG. Turn-based never opened this window: scripts destroy objects
// between turns, when no animation is in flight. Realtime runs scripts and animations in the same
// frames, so a script can erase a critter that is mid-walk or mid-death. Same family as
// combat_rt_notify_critter_removed (whoHitMe / the actor registry) -- a different holder of the
// same freed pointer.
//
// sad[] is silenced FIRST because it is pure data with no side effects; the sequence teardown
// after it can register follow-up animations, and by then nothing points at the dead object.
void anim_notify_object_freed(Object* obj)
{
    if (obj == NULL) {
        return;
    }

    // field_20 == -1000 is this file's established "entry is dead" marker (see anim_set_end);
    // every dereference of sad_entry->obj sits behind that check, and the two places that only
    // COMPARE the pointer (anim_set_end, anim_extend_move) read NULL correctly. Clearing obj as
    // well matters because the allocator can hand the same address to a later object, which would
    // otherwise alias this dead entry.
    for (int i = 0; i < curr_sad; i++) {
        if (sad[i].obj == obj) {
            sad[i].field_20 = -1000;
            sad[i].obj = NULL;
        }
    }

    // The sequence table holds the object as an AnimationDescription owner; left alone,
    // anim_set_continue would run the NEXT description for a freed owner -- the second crash in
    // the same window. One object can own descriptions in several live sequences and
    // anim_force_clear ends one per call, so loop, bounded by the table size.
    for (int i = 0; i < ANIMATION_SEQUENCE_LIST_CAPACITY; i++) {
        if (anim_force_clear(obj) != 0) {
            break;
        }
    }

    // ...and the smoothing tables (a glide, a held stride, the last step's cadence) hold it too.
    anim_smooth_forget(obj);
}

// 0x413788
int register_end()
{
    if (curr_anim_set == -1) {
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);

    // TEMP DIAG (the stride restart, see combat_rt_fm_foreign_touch): who registers an animation on
    // the dude while free movement is walking him -- its end runs dude_stand.
    if (trace_enabled() && combat_rt_fm_foreign_touch()) {
        for (int index = 0; index < curr_anim_counter; index++) {
            AnimationDescription* description = &(animationSequence->animations[index]);
            if (description->owner == obj_dude) {
                static int traced = 0;
                if (traced < 8) {
                    traced++;
                    trace_event(TRACE_WARN, "fm-anim-registered", "kind=%d anim=%d length=%d",
                        description->kind, description->anim, curr_anim_counter);
                    trace_backtrace("fm-anim-registered");
                }
                break;
            }
        }
    }

    animationSequence->field_0 = 0;
    animationSequence->length = curr_anim_counter;
    animationSequence->animationIndex = -1;
    animationSequence->flags &= ~ANIM_SEQ_ACCUMULATING;
    animationSequence->animations[0].delay = 0;
    if (isInCombat()) {
        combat_anim_begin();
        animationSequence->flags |= ANIM_SEQ_COMBAT_ANIM_STARTED;
    }

    int v1 = curr_anim_set;
    curr_anim_set = -1;

    if (!(animationSequence->flags & ANIM_SEQ_0x10)) {
        anim_set_continue(v1, 1);
    }

    return 0;
}

// 0x41384C
static int anim_preload(Object* object, int fid, CacheEntry** cacheEntryPtr)
{
    *cacheEntryPtr = NULL;

    if (art_ptr_lock(fid, cacheEntryPtr) != NULL) {
        art_ptr_unlock(*cacheEntryPtr);
        *cacheEntryPtr = NULL;
        return 0;
    }

    return -1;
}

// 0x413878
static void anim_cleanup()
{
    if (curr_anim_set == -1) {
        return;
    }

    for (int index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        anim_set[index].flags &= ~(ANIM_SEQ_ACCUMULATING | ANIM_SEQ_0x10);
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    for (int index = 0; index < curr_anim_counter; index++) {
        AnimationDescription* animationDescription = &(animationSequence->animations[index]);
        if (animationDescription->artCacheKey != NULL) {
            art_ptr_unlock(animationDescription->artCacheKey);
        }

        if (animationDescription->kind == ANIM_KIND_CALLBACK && animationDescription->callback == (AnimationCallback*)gsnd_anim_sound) {
            gsound_delete_sfx((Sound*)animationDescription->param1);
        }
    }

    curr_anim_set = -1;
}

// 0x41390C
int check_registry(Object* obj)
{
    if (curr_anim_set == -1) {
        if (anim_dbg_attack_scope) {
            trace_event(TRACE_WARN, "reg-conflict", "why=no-transaction pid=%d",
                obj != NULL ? obj->pid : -1);
        }
        return -1;
    }

    if (curr_anim_counter >= ANIMATION_DESCRIPTION_LIST_CAPACITY) {
        if (anim_dbg_attack_scope) {
            trace_event(TRACE_WARN, "reg-conflict", "why=descriptions-full pid=%d count=%d",
                obj != NULL ? obj->pid : -1, curr_anim_counter);
        }
        return -1;
    }

    if (obj == NULL) {
        return 0;
    }

    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);

        if (animationSequenceIndex != curr_anim_set && animationSequence->field_0 != -1000) {
            for (int animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
                // NOTE (2026-08-09): an "already played descriptions don't veto a new registration"
                // skip was tried here to fix "I kill a rat and can't move until it stops twitching".
                // It DOES fix that -- the player owns a spent description for as long as the corpse
                // is falling, and every new animation he registers, above all a walk, is refused
                // right here. But it REVERTED: allowing an object into two live sequences at once
                // breaks invariants all over this module. It produced, in order, the interface stuck
                // disabled with the hourglass, the player unable to attack, and finally enemies
                // frozen mid-dodge and therefore unhittable (traced: rat at anim=13 frame=3 for 5 s,
                // its dodge description started and never completing, 45 x attack-fail defBusy=-1).
                // Do not re-apply without solving the sad/sequence ownership model first.
                AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
                if (obj == animationDescription->owner && animationDescription->kind != 11) {
                    if ((animationSequence->flags & ANIM_SEQ_INSIGNIFICANT) == 0) {
                        if (anim_dbg_attack_scope) {
                            // Everything needed to name the blocker: whose slot, which sequence,
                            // how long it is, what it is, and whether anim_busy can even see it
                            // (len==1 && anim==ANIM_STAND is the one it skips).
                            trace_event(TRACE_WARN, "reg-conflict",
                                "why=owner-busy pid=%d seq=%d len=%d idx=%d kind=%d anim=%d flags=%#x prio=%d busy=%d",
                                obj != NULL ? obj->pid : -1,
                                animationSequenceIndex,
                                animationSequence->length,
                                animationDescriptionIndex,
                                animationDescription->kind,
                                animationDescription->anim,
                                animationSequence->flags,
                                (animationSequence->flags & ANIM_SEQ_PRIORITIZED) != 0 ? 1 : 0,
                                anim_busy(obj));
                        }
                        return -1;
                    }

                    anim_set_end(animationSequenceIndex);
                }
            }
        }
    }

    return 0;
}

// Returns -1 if object is playing some animation.
//
// 0x4139A8
int anim_busy(Object* a1)
{
    if (curr_anim_counter >= ANIMATION_DESCRIPTION_LIST_CAPACITY || a1 == NULL) {
        return 0;
    }

    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequenceIndex != curr_anim_set && animationSequence->field_0 != -1000) {
            for (int animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
                AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
                if (a1 != animationDescription->owner) {
                    continue;
                }

                if (animationDescription->kind == ANIM_KIND_CALLBACK) {
                    continue;
                }

                if (animationSequence->length == 1 && animationDescription->anim == ANIM_STAND) {
                    continue;
                }

                return -1;
            }
        }
    }

    return 0;
}

// True (-1) only while `obj` still has UNPLAYED work in a live sequence.
//
// anim_busy() answers the wider question "does any live sequence mention obj at all", and it
// counts descriptions that have ALREADY been played. Vanilla packs a whole attack into ONE
// sequence -- attacker's swing, projectile flight, defender's reaction, attacker's trailing
// cleanup -- so once the player kills something, anim_busy keeps reporting him busy for the
// entire death fall of his victim, long after his own swing finished. Turn-based never notices
// (the turn is over anyway); under realtime it reads as "I killed the rat and now I have to
// stand still until it finishes bleeding out before I can move".
//
// Descriptions before the sequence's animationIndex have already played and so cannot be what
// obj is still waiting on. animationIndex == -1 means the sequence has not started yet, so
// everything in it is still pending.
//
// This is ONLY safe to gate realtime player movement on when paired with anim_clear_pending()
// -- see the warning there.
//
// exceptSequenceIndex lets a sequence ask "is this object busy ELSEWHERE?" while excluding
// itself; pass -1 for the plain question. anim_set_end needs it (see its dude_stand guard).
static int anim_busy_blocking_ex(Object* a1, int exceptSequenceIndex)
{
    if (curr_anim_counter >= ANIMATION_DESCRIPTION_LIST_CAPACITY || a1 == NULL) {
        return 0;
    }

    for (int animationSequenceIndex = 0; animationSequenceIndex < ANIMATION_SEQUENCE_LIST_CAPACITY; animationSequenceIndex++) {
        AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
        if (animationSequenceIndex == curr_anim_set || animationSequence->field_0 == -1000) {
            continue;
        }

        if (animationSequenceIndex == exceptSequenceIndex) {
            continue;
        }

        for (int animationDescriptionIndex = 0; animationDescriptionIndex < animationSequence->length; animationDescriptionIndex++) {
            // Already played -- not something obj is waiting on.
            if (animationSequence->animationIndex >= 0 && animationDescriptionIndex < animationSequence->animationIndex) {
                continue;
            }

            AnimationDescription* animationDescription = &(animationSequence->animations[animationDescriptionIndex]);
            if (a1 != animationDescription->owner) {
                continue;
            }

            if (animationDescription->kind == ANIM_KIND_CALLBACK) {
                continue;
            }

            if (animationSequence->length == 1 && animationDescription->anim == ANIM_STAND) {
                continue;
            }

            return -1;
        }
    }

    return 0;
}

int anim_busy_blocking(Object* a1)
{
    return anim_busy_blocking_ex(a1, -1);
}

// TEMP DIAG. Dump the first live sequence that reports `a1` busy, so a "why can't I move"
// report can name the exact description holding him. Format per description:
//   #<idx><D|o><kind>/<anim>/d<delay>   -- D = owned by a1, o = owned by someone else
// plus the sequence's two cursors: ai = animationIndex (descriptions COMPLETED), f0 = field_0
// (next description to START). STRIP once the stall is understood.
void anim_debug_blocking_dump(Object* a1, char* buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (a1 == NULL) {
        return;
    }

    for (int s = 0; s < ANIMATION_SEQUENCE_LIST_CAPACITY; s++) {
        AnimationSequence* seq = &(anim_set[s]);
        if (s == curr_anim_set || seq->field_0 == -1000) {
            continue;
        }

        bool mentionsA1 = false;
        for (int i = 0; i < seq->length; i++) {
            if (seq->animations[i].owner == a1) {
                mentionsA1 = true;
                break;
            }
        }
        if (!mentionsA1) {
            continue;
        }

        size_t used = (size_t)snprintf(buf, size, "seq=%d ai=%d f0=%d len=%d",
            s, seq->animationIndex, seq->field_0, seq->length);
        for (int i = 0; i < seq->length && used < size - 1; i++) {
            AnimationDescription* d = &(seq->animations[i]);
            used += (size_t)snprintf(buf + used, size - used, " #%d%c%d/%d/d%d",
                i, d->owner == a1 ? 'D' : 'o', d->kind, d->anim, d->delay);
        }
        return;
    }

    snprintf(buf, size, "no-seq");
}

// 0x413A3C
int register_object_move_to_object(Object* owner, Object* destination, int actionPoints, int delay)
{
    if (check_registry(owner) == -1 || actionPoints == 0) {
        anim_cleanup();
        return -1;
    }

    if (owner->tile == destination->tile && owner->elevation == destination->elevation) {
        return 0;
    }

    AnimationDescription* animationDescription = &(anim_set[curr_anim_set].animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_OBJECT;
    animationDescription->anim = ANIM_WALK;
    animationDescription->owner = owner;
    animationDescription->destination = destination;
    animationDescription->actionPoints = actionPoints;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return register_object_turn_towards(owner, destination->tile);
}

// 0x413B48
int register_object_run_to_object(Object* owner, Object* destination, int actionPoints, int delay)
{
    if (check_registry(owner) == -1 || actionPoints == 0) {
        anim_cleanup();
        return -1;
    }

    if (owner->tile == destination->tile && owner->elevation == destination->elevation) {
        return 0;
    }

    AnimationDescription* animationDescription = &(anim_set[curr_anim_set].animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_OBJECT;
    animationDescription->owner = owner;
    animationDescription->destination = destination;

    if ((FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER && (owner->data.critter.combat.results & DAM_CRIP_LEG_ANY) != 0)
        || (owner == obj_dude && is_pc_flag(PC_FLAG_SNEAKING) && !perk_level(PERK_SILENT_RUNNING))
        || (!art_exists(art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, ANIM_RUNNING, 0, owner->rotation + 1)))) {
        animationDescription->anim = ANIM_WALK;
    } else {
        animationDescription->anim = ANIM_RUNNING;
    }

    animationDescription->actionPoints = actionPoints;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;
    return register_object_turn_towards(owner, destination->tile);
}

// 0x413CDC
int register_object_move_to_tile(Object* owner, int tile, int elevation, int actionPoints, int delay)
{
    if (check_registry(owner) == -1 || actionPoints == 0) {
        anim_cleanup();
        return -1;
    }

    if (tile == owner->tile && elevation == owner->elevation) {
        return 0;
    }

    AnimationDescription* animationDescription = &(anim_set[curr_anim_set].animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_TILE;
    animationDescription->anim = ANIM_WALK;
    animationDescription->owner = owner;
    animationDescription->tile = tile;
    animationDescription->elevation = elevation;
    animationDescription->actionPoints = actionPoints;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x413DE8
int register_object_run_to_tile(Object* owner, int tile, int elevation, int actionPoints, int delay)
{
    if (check_registry(owner) == -1 || actionPoints == 0) {
        anim_cleanup();
        return -1;
    }

    if (tile == owner->tile && elevation == owner->elevation) {
        return 0;
    }

    AnimationDescription* animationDescription = &(anim_set[curr_anim_set].animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_TILE;
    animationDescription->owner = owner;
    animationDescription->tile = tile;
    animationDescription->elevation = elevation;

    if ((FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER && (owner->data.critter.combat.results & DAM_CRIP_LEG_ANY) != 0)
        || (owner == obj_dude && is_pc_flag(PC_FLAG_SNEAKING) && !perk_level(PERK_SILENT_RUNNING))
        || (!art_exists(art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, ANIM_RUNNING, 0, owner->rotation + 1)))) {
        animationDescription->anim = ANIM_WALK;
    } else {
        animationDescription->anim = ANIM_RUNNING;
    }

    animationDescription->actionPoints = actionPoints;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x413F60
int register_object_move_straight_to_tile(Object* object, int tile, int elevation, int anim, int delay)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    if (tile == object->tile && elevation == object->elevation) {
        return 0;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);

    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_TILE_STRAIGHT;
    animationDescription->owner = object;
    animationDescription->tile = tile;
    animationDescription->elevation = elevation;
    animationDescription->anim = anim;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, animationDescription->anim, (object->fid & 0xF000) >> 12, object->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(object, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414060
int register_object_animate_and_move_straight(Object* owner, int tile, int elevation, int anim, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    if (tile == owner->tile && elevation == owner->elevation) {
        return 0;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_TO_TILE_STRAIGHT_AND_WAIT_FOR_COMPLETE;
    animationDescription->owner = owner;
    animationDescription->tile = tile;
    animationDescription->elevation = elevation;
    animationDescription->anim = anim;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414160
int register_object_move_on_stairs(Object* owner, Object* stairs, int delay)
{
    int anim;
    int destTile;
    int destElevation;
    AnimationSequence* animationSequence;
    AnimationDescription* animationDescription;
    int fid;

    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    if (owner->elevation == stairs->elevation) {
        anim = ANIM_UP_STAIRS_LEFT;
        destTile = stairs->tile + 4;
        destElevation = stairs->elevation + 1;
    } else {
        anim = ANIM_DOWN_STAIRS_RIGHT;
        destTile = stairs->tile + 200;
        destElevation = stairs->elevation;
    }

    if (destTile == owner->tile && destElevation == owner->elevation) {
        return 0;
    }

    animationSequence = &(anim_set[curr_anim_set]);
    animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_MOVE_ON_STAIRS;
    animationDescription->owner = owner;
    animationDescription->tile = destTile;
    animationDescription->elevation = destElevation;
    animationDescription->anim = anim;
    animationDescription->delay = delay;

    fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414298
int register_object_check_falling(Object* owner, int delay)
{
    AnimationSequence* animationSequence;
    AnimationDescription* animationDescription;
    int fid;

    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    animationSequence = &(anim_set[curr_anim_set]);
    animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_CHECK_FALLING;
    animationDescription->anim = ANIM_FALLING;
    animationDescription->owner = owner;
    animationDescription->delay = delay;

    fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414380
int register_object_animate(Object* owner, int anim, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ANIMATE;
    animationDescription->owner = owner;
    animationDescription->anim = anim;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        if (anim_dbg_attack_scope) {
            // The OTHER way an attack dies without a word: not a busy slot but a MISSING ART.
            // anim_cleanup below drops the whole transaction, so register_end -- and with it
            // combat_attack -- returns -1 while every busy test reads zero. The weapon code in
            // bits 12-15 of the owner's fid is the usual culprit (see rt_attack_art_ok).
            trace_event(TRACE_WARN, "reg-art-missing",
                "pid=%d ownerFid=%#x anim=%d wpnCode=%d rot=%d artFid=%#x",
                owner != NULL ? owner->pid : -1,
                owner != NULL ? owner->fid : 0,
                animationDescription->anim,
                owner != NULL ? (owner->fid & 0xF000) >> 12 : -1,
                owner != NULL ? owner->rotation : -1,
                fid);
        }
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414460
int register_object_animate_reverse(Object* owner, int anim, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ANIMATE_REVERSED;
    animationDescription->owner = owner;
    animationDescription->anim = anim;
    animationDescription->delay = delay;
    animationDescription->artCacheKey = NULL;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, animationDescription->anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414544
int register_object_animate_and_hide(Object* owner, int anim, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ANIMATE_AND_HIDE;
    animationDescription->owner = owner;
    animationDescription->anim = anim;
    animationDescription->delay = delay;
    animationDescription->artCacheKey = NULL;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414628
int register_object_turn_towards(Object* owner, int tile)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ROTATE_TO_TILE;
    animationDescription->delay = -1;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = owner;
    animationDescription->tile = tile;

    curr_anim_counter++;

    return 0;
}

// 0x4146AC
int register_object_inc_rotation(Object* owner)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ROTATE_CLOCKWISE;
    animationDescription->delay = -1;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = owner;

    curr_anim_counter++;

    return 0;
}

// 0x41472C
int register_object_dec_rotation(Object* owner)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ROTATE_COUNTER_CLOCKWISE;
    animationDescription->delay = -1;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = owner;

    curr_anim_counter++;

    return 0;
}

// 0x4147AC
int register_object_erase(Object* object)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_HIDE;
    animationDescription->delay = -1;
    animationDescription->artCacheKey = NULL;
    animationDescription->extendedFlags = 0;
    animationDescription->owner = object;
    curr_anim_counter++;

    return 0;
}

// 0x414834
int register_object_must_erase(Object* object)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_HIDE;
    animationDescription->delay = -1;
    animationDescription->artCacheKey = NULL;
    animationDescription->extendedFlags = ANIMATION_SEQUENCE_FORCED;
    animationDescription->owner = object;
    curr_anim_counter++;

    return 0;
}

// 0x4148BC
int register_object_call(void* a1, void* a2, AnimationCallback* proc, int delay)
{
    if (check_registry(NULL) == -1 || proc == NULL) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_CALLBACK;
    animationDescription->extendedFlags = 0;
    animationDescription->artCacheKey = NULL;
    animationDescription->param2 = a2;
    animationDescription->param1 = a1;
    animationDescription->callback = proc;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// Same as `register_object_call` but accepting 3 parameters.
//
// 0x414950
int register_object_call3(void* a1, void* a2, void* a3, AnimationCallback3* proc, int delay)
{
    if (check_registry(NULL) == -1 || proc == NULL) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_CALLBACK3;
    animationDescription->extendedFlags = 0;
    animationDescription->artCacheKey = NULL;
    animationDescription->param2 = a2;
    animationDescription->param1 = a1;
    animationDescription->callback3 = proc;
    animationDescription->param3 = a3;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x4149E4
int register_object_must_call(void* a1, void* a2, AnimationCallback* proc, int delay)
{
    if (check_registry(NULL) == -1 || proc == NULL) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_CALLBACK;
    animationDescription->extendedFlags = ANIMATION_SEQUENCE_FORCED;
    animationDescription->artCacheKey = NULL;
    animationDescription->param2 = a2;
    animationDescription->param1 = a1;
    animationDescription->callback = proc;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// The [flag] parameter should be one of OBJECT_* flags. The way it's handled
// down the road implies it should not be a group of flags (joined with bitwise
// OR), but a one particular flag.
//
// 0x414A78
int register_object_fset(Object* object, int flag, int delay)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_SET_FLAG;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = object;
    animationDescription->objectFlag = flag;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// The [flag] parameter should be one of OBJECT_* flags. The way it's handled
// down the road implies it should not be a group of flags (joined with bitwise
// OR), but a one particular flag.
//
// 0x414AF8
int register_object_funset(Object* object, int flag, int delay)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_UNSET_FLAG;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = object;
    animationDescription->objectFlag = flag;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x414B78
int register_object_flatten(Object* object, int delay)
{
    AnimationSequence* animationSequence;
    AnimationDescription* animationDescription;

    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    animationSequence = &(anim_set[curr_anim_set]);
    animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_TOGGLE_FLAT;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = object;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x414BF4
int register_object_change_fid(Object* owner, int fid, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_SET_FID;
    animationDescription->owner = owner;
    animationDescription->fid = fid;
    animationDescription->delay = delay;

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414CAC
int register_object_take_out(Object* owner, int weaponAnimationCode, int delay)
{
    const char* sfx = gsnd_build_character_sfx_name(owner, ANIM_TAKE_OUT, weaponAnimationCode);
    if (register_object_play_sfx(owner, sfx, delay) == -1) {
        return -1;
    }

    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_TAKE_OUT_WEAPON;
    animationDescription->anim = ANIM_TAKE_OUT;
    animationDescription->delay = 0;
    animationDescription->owner = owner;
    animationDescription->weaponAnimationCode = weaponAnimationCode;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, ANIM_TAKE_OUT, weaponAnimationCode, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x414DB4
int register_object_light(Object* owner, int lightDistance, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_SET_LIGHT_DISTANCE;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = owner;
    animationDescription->lightDistance = lightDistance;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x414E34
int register_object_outline(Object* object, bool outline, int delay)
{
    if (check_registry(object) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_TOGGLE_OUTLINE;
    animationDescription->artCacheKey = NULL;
    animationDescription->owner = object;
    animationDescription->outline = outline;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x414EB4
int register_object_play_sfx(Object* owner, const char* soundEffectName, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_CALLBACK;
    animationDescription->owner = owner;
    if (soundEffectName != NULL) {
        int volume = gsound_compute_relative_volume(owner);
        animationDescription->param1 = gsound_load_sound_volume(soundEffectName, owner, volume);
        if (animationDescription->param1 != NULL) {
            animationDescription->callback = (AnimationCallback*)gsnd_anim_sound;
        } else {
            animationDescription->kind = ANIM_KIND_NOOP;
        }
    } else {
        animationDescription->kind = ANIM_KIND_NOOP;
    }

    animationDescription->artCacheKey = NULL;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x414F68
int register_object_animate_forever(Object* owner, int anim, int delay)
{
    if (check_registry(owner) == -1) {
        anim_cleanup();
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->kind = ANIM_KIND_ANIMATE_FOREVER;
    animationDescription->owner = owner;
    animationDescription->anim = anim;
    animationDescription->delay = delay;

    int fid = art_id(FID_TYPE(owner->fid), owner->fid & 0xFFF, anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1);

    // NOTE: Uninline.
    if (anim_preload(owner, fid, &(animationDescription->artCacheKey)) == -1) {
        anim_cleanup();
        return -1;
    }

    curr_anim_counter++;

    return 0;
}

// 0x41504C
int register_ping(int a1, int delay)
{
    if (check_registry(NULL) == -1) {
        anim_cleanup();
        return -1;
    }

    int animationSequenceIndex = anim_free_slot(a1 | ANIMATION_REQUEST_0x100);
    if (animationSequenceIndex == -1) {
        return -1;
    }

    anim_set[animationSequenceIndex].flags = ANIM_SEQ_0x10;

    AnimationSequence* animationSequence = &(anim_set[curr_anim_set]);
    AnimationDescription* animationDescription = &(animationSequence->animations[curr_anim_counter]);
    animationDescription->owner = NULL;
    animationDescription->kind = ANIM_KIND_26;
    animationDescription->artCacheKey = NULL;
    animationDescription->animationSequenceIndex = animationSequenceIndex;
    animationDescription->delay = delay;

    curr_anim_counter++;

    return 0;
}

// 0x415170
static int anim_set_check(int animationSequenceIndex)
{
    if (animationSequenceIndex == -1) {
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
    if (animationSequence->field_0 == -1000) {
        return -1;
    }

    while (1) {
        if (animationSequence->field_0 >= animationSequence->length) {
            return 0;
        }

        if (animationSequence->field_0 > animationSequence->animationIndex) {
            AnimationDescription* animationDescription = &(animationSequence->animations[animationSequence->field_0]);
            if (animationDescription->delay < 0) {
                return 0;
            }

            if (animationDescription->delay > 0) {
                animationDescription->delay--;
                return 0;
            }
        }

        AnimationDescription* animationDescription = &(animationSequence->animations[animationSequence->field_0++]);

        int rc;
        Rect rect;
        switch (animationDescription->kind) {
        case ANIM_KIND_MOVE_TO_OBJECT:
            rc = anim_move_to_object(animationDescription->owner, animationDescription->destination, animationDescription->actionPoints, animationDescription->anim, animationSequenceIndex);
            break;
        case ANIM_KIND_MOVE_TO_TILE:
            rc = anim_move_to_tile(animationDescription->owner, animationDescription->tile, animationDescription->elevation, animationDescription->actionPoints, animationDescription->anim, animationSequenceIndex);
            break;
        case ANIM_KIND_MOVE_TO_TILE_STRAIGHT:
            rc = anim_move_straight_to_tile(animationDescription->owner, animationDescription->tile, animationDescription->elevation, animationDescription->anim, animationSequenceIndex, 0);
            break;
        case ANIM_KIND_MOVE_TO_TILE_STRAIGHT_AND_WAIT_FOR_COMPLETE:
            rc = anim_move_straight_to_tile(animationDescription->owner, animationDescription->tile, animationDescription->elevation, animationDescription->anim, animationSequenceIndex, ANIM_SAD_WAIT_FOR_COMPLETION);
            break;
        case ANIM_KIND_ANIMATE:
            rc = anim_animate(animationDescription->owner, animationDescription->anim, animationSequenceIndex, 0);
            break;
        case ANIM_KIND_ANIMATE_REVERSED:
            rc = anim_animate(animationDescription->owner, animationDescription->anim, animationSequenceIndex, ANIM_SAD_REVERSE);
            break;
        case ANIM_KIND_ANIMATE_AND_HIDE:
            rc = anim_animate(animationDescription->owner, animationDescription->anim, animationSequenceIndex, ANIM_SAD_HIDE_ON_END);
            if (rc == -1) {
                // NOTE: Uninline.
                rc = anim_hide(animationDescription->owner, animationSequenceIndex);
            }
            break;
        case ANIM_KIND_ANIMATE_FOREVER:
            rc = anim_animate(animationDescription->owner, animationDescription->anim, animationSequenceIndex, ANIM_SAD_FOREVER);
            break;
        case ANIM_KIND_ROTATE_TO_TILE:
            if (!critter_is_prone(animationDescription->owner)) {
                int rotation = tile_dir(animationDescription->owner->tile, animationDescription->tile);
                dude_stand(animationDescription->owner, rotation, -1);
            }
            anim_set_continue(animationSequenceIndex, 0);
            rc = 0;
            break;
        case ANIM_KIND_ROTATE_CLOCKWISE:
            rc = anim_turn_towards(animationDescription->owner, 1, animationSequenceIndex);
            break;
        case ANIM_KIND_ROTATE_COUNTER_CLOCKWISE:
            rc = anim_turn_towards(animationDescription->owner, -1, animationSequenceIndex);
            break;
        case ANIM_KIND_HIDE:
            // NOTE: Uninline.
            rc = anim_hide(animationDescription->owner, animationSequenceIndex);
            break;
        case ANIM_KIND_CALLBACK:
            rc = animationDescription->callback(animationDescription->param1, animationDescription->param2);
            if (rc == 0) {
                rc = anim_set_continue(animationSequenceIndex, 0);
            }
            break;
        case ANIM_KIND_CALLBACK3:
            rc = animationDescription->callback3(animationDescription->param1, animationDescription->param2, animationDescription->param3);
            if (rc == 0) {
                rc = anim_set_continue(animationSequenceIndex, 0);
            }
            break;
        case ANIM_KIND_SET_FLAG:
            if (animationDescription->objectFlag == OBJECT_LIGHTING) {
                if (obj_turn_on_light(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            } else if (animationDescription->objectFlag == OBJECT_HIDDEN) {
                if (obj_turn_off(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            } else {
                animationDescription->owner->flags |= animationDescription->objectFlag;
            }

            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        case ANIM_KIND_UNSET_FLAG:
            if (animationDescription->objectFlag == OBJECT_LIGHTING) {
                if (obj_turn_off_light(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            } else if (animationDescription->objectFlag == OBJECT_HIDDEN) {
                if (obj_turn_on(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            } else {
                animationDescription->owner->flags &= ~animationDescription->objectFlag;
            }

            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        case ANIM_KIND_TOGGLE_FLAT:
            if (obj_toggle_flat(animationDescription->owner, &rect) == 0) {
                tile_refresh_rect(&rect, animationDescription->owner->elevation);
            }
            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        case ANIM_KIND_SET_FID:
            rc = anim_change_fid(animationDescription->owner, animationSequenceIndex, animationDescription->fid);
            break;
        case ANIM_KIND_TAKE_OUT_WEAPON:
            rc = anim_animate(animationDescription->owner, ANIM_TAKE_OUT, animationSequenceIndex, animationDescription->tile);
            break;
        case ANIM_KIND_SET_LIGHT_DISTANCE:
            obj_set_light(animationDescription->owner, animationDescription->lightDistance, animationDescription->owner->lightIntensity, &rect);
            tile_refresh_rect(&rect, animationDescription->owner->elevation);
            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        case ANIM_KIND_MOVE_ON_STAIRS:
            rc = anim_move_on_stairs(animationDescription->owner, animationDescription->tile, animationDescription->elevation, animationDescription->anim, animationSequenceIndex);
            break;
        case ANIM_KIND_CHECK_FALLING:
            rc = check_for_falling(animationDescription->owner, animationDescription->anim, animationSequenceIndex);
            break;
        case ANIM_KIND_TOGGLE_OUTLINE:
            if (animationDescription->outline) {
                if (obj_turn_on_outline(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            } else {
                if (obj_turn_off_outline(animationDescription->owner, &rect) == 0) {
                    tile_refresh_rect(&rect, animationDescription->owner->elevation);
                }
            }
            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        case ANIM_KIND_26:
            anim_set[animationDescription->animationSequenceIndex].flags &= ~ANIM_SEQ_0x10;
            rc = anim_set_continue(animationDescription->animationSequenceIndex, 1);
            if (rc != -1) {
                rc = anim_set_continue(animationSequenceIndex, 0);
            }
            break;
        case ANIM_KIND_NOOP:
            rc = anim_set_continue(animationSequenceIndex, 0);
            break;
        default:
            rc = -1;
            break;
        }

        if (rc == -1) {
            anim_set_end(animationSequenceIndex);
        }

        if (animationSequence->field_0 == -1000) {
            return -1;
        }
    }
}

// 0x415620
static int anim_set_continue(int animationSequenceIndex, int a2)
{
    if (animationSequenceIndex == -1) {
        return -1;
    }

    AnimationSequence* animationSequence = &(anim_set[animationSequenceIndex]);
    if (animationSequence->field_0 == -1000) {
        return -1;
    }

    animationSequence->animationIndex++;
    if (animationSequence->animationIndex == animationSequence->length) {
        return anim_set_end(animationSequenceIndex);
    } else {
        if (a2) {
            return anim_set_check(animationSequenceIndex);
        }
    }

    return 0;
}

// 0x41568C
static int anim_set_end(int animationSequenceIndex)
{
    AnimationSequence* animationSequence;
    AnimationDescription* animationDescription;
    int i;

    if (animationSequenceIndex == -1) {
        return -1;
    }

    animationSequence = &(anim_set[animationSequenceIndex]);
    if (animationSequence->field_0 == -1000) {
        return -1;
    }

    // Already tearing this sequence down one level up the stack -- see anim_set_ending. Bail out
    // instead of ending it a second time; the outer call finishes the job and marks it dead.
    if (anim_set_ending[animationSequenceIndex]) {
        return -1;
    }
    anim_set_ending[animationSequenceIndex] = true;

    for (i = 0; i < curr_sad; i++) {
        AnimationSad* sad_entry = &(sad[i]);
        if (sad_entry->animationSequenceIndex == animationSequenceIndex) {
            sad_entry->field_20 = -1000;
        }
    }

    for (i = 0; i < animationSequence->length; i++) {
        animationDescription = &(animationSequence->animations[i]);
        if (animationDescription->kind == ANIM_KIND_HIDE && ((i < animationSequence->animationIndex) || (animationDescription->extendedFlags & ANIMATION_SEQUENCE_FORCED))) {
            Rect rect;
            int elevation = animationDescription->owner->elevation;
            obj_erase_object(animationDescription->owner, &rect);
            tile_refresh_rect(&rect, elevation);
        }
    }

    for (i = 0; i < animationSequence->length; i++) {
        animationDescription = &(animationSequence->animations[i]);
        if (animationDescription->artCacheKey) {
            art_ptr_unlock(animationDescription->artCacheKey);
        }

        if (animationDescription->kind != 11 && animationDescription->kind != 12) {
            // TODO: Check.
            if (animationDescription->kind != ANIM_KIND_26) {
                Object* owner = animationDescription->owner;
                if (FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER) {
                    int j = 0;
                    for (; j < i; j++) {
                        AnimationDescription* ad = &(animationSequence->animations[j]);
                        if (owner == ad->owner) {
                            if (ad->kind != ANIM_KIND_CALLBACK && ad->kind != ANIM_KIND_CALLBACK3) {
                                break;
                            }
                        }
                    }

                    if (i == j) {
                        int k = 0;
                        for (; k < animationSequence->animationIndex; k++) {
                            AnimationDescription* ad = &(animationSequence->animations[k]);
                            if (ad->kind == ANIM_KIND_HIDE && ad->owner == owner) {
                                break;
                            }
                        }

                        if (k == animationSequence->animationIndex) {
                            // Is another live sequence driving this owner right now? Then this
                            // ending sequence must keep its hands off him entirely -- both the sad
                            // kill just below and the force-stand after it would tear down the
                            // animation that OTHER sequence is running.
                            bool ownerBusyElsewhere = anim_busy_blocking_ex(owner, animationSequenceIndex) != 0;

                            if (!ownerBusyElsewhere) {
                                for (int m = 0; m < curr_sad; m++) {
                                    if (sad[m].obj == owner) {
                                        sad[m].field_20 = -1000;
                                        break;
                                    }
                                }
                            }

                            // ...but NOT if another live sequence is currently driving this
                            // owner. dude_stand force-stands the object and kills its first
                            // sad -- an in-flight WALK sad -- freezing locomotion until the
                            // next step is issued (the same trap action_suppress_miss_dodge
                            // documents). That used to be unreachable for an attacker, who
                            // could not act until his own attack sequence ended; now that
                            // anim_busy_blocking lets the realtime player walk off as soon as
                            // his swing is spent, his victim's death sequence ends WHILE he is
                            // walking and would otherwise stop him dead in his tracks.
                            if ((animationSequence->flags & ANIM_SEQ_NO_STAND) == 0
                                && !critter_is_prone(owner)
                                && !ownerBusyElsewhere) {
                                // ⚠️ ONE LEG OF A LONGER MOVE DOES NOT STAND. A chase is a string
                                // of short legs, each its own sequence, and standing the body at
                                // the end of every one flashed the STAND pose for a frame and
                                // restarted the stride from frame 0 -- a hitch every four tiles on
                                // every chaser. The next leg is registered within the frame; if it
                                // never comes, anim_deferred_stands stands the body after all.
                                int pose = FID_ANIM_TYPE(owner->fid);
                                bool held = (animationSequence->flags & ANIM_SEQ_DEFER_STAND) != 0
                                    && !anim_in_anim_stop
                                    && (pose == ANIM_WALK || pose == ANIM_RUNNING)
                                    && anim_defer_stand(owner);
                                if (!held) {
                                    dude_stand(owner, owner->rotation, -1);
                                }
                            }
                        }
                    }
                }
            }
        } else if (i >= animationSequence->field_0) {
            if (animationDescription->extendedFlags & ANIMATION_SEQUENCE_FORCED) {
                animationDescription->callback(animationDescription->param1, animationDescription->param2);
            } else {
                if (animationDescription->kind == ANIM_KIND_CALLBACK && animationDescription->callback == (AnimationCallback*)gsnd_anim_sound) {
                    gsound_delete_sfx((Sound*)animationDescription->param1);
                }
            }
        }
    }

    animationSequence->animationIndex = -1;
    animationSequence->field_0 = -1000;
    if ((animationSequence->flags & ANIM_SEQ_COMBAT_ANIM_STARTED) != 0) {
        combat_anim_finished();
    }

    if (anim_in_bk) {
        animationSequence->flags = ANIM_SEQ_0x20;
    } else {
        animationSequence->flags = 0;
    }

    anim_set_ending[animationSequenceIndex] = false;

    return 0;
}

// 0x415940
static bool anim_can_use_door(Object* critter, Object* door)
{
    int body_type;
    Proto* door_proto;

    if (critter == obj_dude) {
        return false;
    }

    if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    body_type = critter_body_type(critter);
    if (body_type != BODY_TYPE_BIPED && body_type != BODY_TYPE_ROBOTIC) {
        return false;
    }

    if (FID_TYPE(door->fid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    if (proto_ptr(door->pid, &door_proto) == -1) {
        return false;
    }

    if (door_proto->scenery.type != SCENERY_TYPE_DOOR) {
        return false;
    }

    if (obj_is_locked(door)) {
        return false;
    }

    return true;
}

// 0x4159D4
int make_path(Object* object, int from, int to, unsigned char* rotations, int a5)
{
    return make_path_func(object, from, to, rotations, a5, obj_blocking_at);
}

// 0x4159E8
//
// CE: THE SAME SEARCH WITH SIX TIMES THE REACH (2026-10-04).
//
// Vanilla kept its open and closed lists in two flat arrays of 2000 PathNodes, found the best open
// node by scanning every slot, and walked the closed list linearly to rebuild the route -- and it
// simply GAVE UP when either list filled. 2000 closed nodes is a disc of ~25 tiles around the
// start: any route that has to swing out further than that to get round a wall does not exist as
// far as Fallout is concerned. Measured on the real map (LAADYTUM, the trace's Adytum guard): from
// 20914 to 18715 the shortest route is 54 steps around the compound wall, and the vanilla search
// dies at 2000 closed nodes; it needs ~2900. That is "NPCs want to come after me but cannot get out
// of the tent or the room" -- the way out exists, the search just never walks far enough to see it.
//
// ⚠️ SAME SCORING, SAME RULES. Step cost 50 (+10 for a turn out of combat), the screen-distance
// estimate (idist), doors a biped can open, the destination exempt from the blocking test, a5's
// "refuse a blocked destination" and the 800-step route cap: all exactly as before. What changed is
// only the bookkeeping -- a binary heap instead of the slot scan, per-tile parent/rotation/cost
// arrays instead of the closed-list walk -- which is also what pays for the bigger budget: one pop
// is O(log n) where the scan was O(n), so 12000 nodes now cost about what 2000 used to.
//
// Among nodes of EQUAL score the oldest is expanded first. Vanilla's tie-break was "lowest free
// slot", which shifted as slots were reused; routes can differ from vanilla in such ties, never in
// length class.
#define PATH_SEARCH_MAX_CLOSED 12000

// The budget make_path_func searches with: PATH_SEARCH_MAX_CLOSED, except inside make_path_capped.
static int path_closed_cap = PATH_SEARCH_MAX_CLOSED;

// make_path with a smaller search budget -- VANILLA'S 2000 for a question vanilla asked with it,
// such as "can I get to that tile a few steps away?" (the flee choice, combatai_flee_tile). A route
// to a tile that close which needs more nodes than that is no route for a critter running for its
// life, and asking with the full budget turns every walled-off candidate into a 12000-node failure.
int make_path_capped(Object* object, int from, int to, unsigned char* rotations, int a5, int maxClosed)
{
    int saved = path_closed_cap;
    path_closed_cap = maxClosed;
    int steps = make_path_func(object, from, to, rotations, a5, obj_blocking_at);
    path_closed_cap = saved;
    return steps;
}

static void path_heap_push(int* size, int f, int seq, int tile)
{
    int i = (*size)++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        PathHeapEntry* p = &(path_heap[parent]);
        if (p->f < f || (p->f == f && p->seq < seq)) {
            break;
        }
        path_heap[i] = *p;
        i = parent;
    }
    path_heap[i].f = f;
    path_heap[i].seq = seq;
    path_heap[i].tile = tile;
}

static PathHeapEntry path_heap_pop(int* size)
{
    PathHeapEntry top = path_heap[0];
    PathHeapEntry last = path_heap[--(*size)];
    int n = *size;
    int i = 0;
    while (true) {
        int child = 2 * i + 1;
        if (child >= n) {
            break;
        }
        if (child + 1 < n) {
            PathHeapEntry* l = &(path_heap[child]);
            PathHeapEntry* r = &(path_heap[child + 1]);
            if (r->f < l->f || (r->f == l->f && r->seq < l->seq)) {
                child++;
            }
        }
        PathHeapEntry* c = &(path_heap[child]);
        if (last.f < c->f || (last.f == c->f && last.seq < c->seq)) {
            break;
        }
        path_heap[i] = *c;
        i = child;
    }
    if (n > 0) {
        path_heap[i] = last;
    }
    return top;
}

// The search above, timed for the frame-pacing meter. A failed search walks its whole budget, and
// several in one frame are the likeliest thing to make a frame late -- which is a hitch in every
// animation on screen, not just the one that asked.
static int anim_stat_path_searches = 0;
static double anim_stat_path_ms = 0.0;
static double anim_stat_path_max_ms = 0.0;

int make_path_func(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback)
{
    Uint64 start = SDL_GetPerformanceCounter();
    int steps = make_path_func_impl(object, from, to, rotations, a5, callback);
    double ms = (double)(SDL_GetPerformanceCounter() - start) * 1000.0 / (double)SDL_GetPerformanceFrequency();

    anim_stat_path_searches++;
    anim_stat_path_ms += ms;
    if (ms > anim_stat_path_max_ms) {
        anim_stat_path_max_ms = ms;
    }

    return steps;
}

static int make_path_func_impl(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback)
{
    if (a5) {
        if (callback(object, to, object->elevation) != NULL) {
            return 0;
        }
    }

    // Vanilla indexed seen[] with whatever it was handed; an invalid tile would read and write
    // outside it.
    if (!hexGridTileIsValid(from) || !hexGridTileIsValid(to)) {
        return 0;
    }

    bool isNotInCombat = !isInCombat();

    memset(seen, 0, sizeof(seen));
    seen[from / 8] |= 1 << (from & 7);

    int toScreenX;
    int toScreenY;
    tile_coord(to, &toScreenX, &toScreenY, object->elevation);

    path_parent[from] = -1;
    path_rotation[from] = 0;
    path_cost[from] = 0;

    int heapSize = 0;
    int seq = 0;
    path_heap_push(&heapSize, EST(from, to), seq++, from);

    int closed = 0;
    bool found = false;

    while (heapSize > 0) {
        PathHeapEntry curr = path_heap_pop(&heapSize);

        if (curr.tile == to) {
            found = true;
            break;
        }

        closed += 1;
        if (closed >= path_closed_cap) {
            return 0;
        }

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tile_num_in_direction(curr.tile, rotation, 1);
            int bit = 1 << (tile & 7);
            if ((seen[tile / 8] & bit) != 0) {
                continue;
            }

            if (tile != to) {
                Object* blocker = callback(object, tile, object->elevation);
                if (blocker != NULL) {
                    if (!anim_can_use_door(object, blocker)) {
                        continue;
                    }
                }
            }

            seen[tile / 8] |= bit;

            int cost = path_cost[curr.tile] + 50;
            if (isNotInCombat && path_rotation[curr.tile] != rotation) {
                cost += 10;
            }

            path_parent[tile] = curr.tile;
            path_rotation[tile] = (unsigned char)rotation;
            path_cost[tile] = cost;

            int newX;
            int newY;
            tile_coord(tile, &newX, &newY, object->elevation);

            path_heap_push(&heapSize, idist(newX, newY, toScreenX, toScreenY) + cost, seq++, tile);
        }
    }

    if (!found) {
        return 0;
    }

    // Walk back from the goal into the scratch buffer (it comes out goal-first), then hand the
    // caller the FIRST 800 steps in walking order. Vanilla copied only the 800 steps nearest the GOAL
    // and reversed those, which on a longer route would have started the walker in the middle of
    // nowhere; with the old budget no route ever got that long, with this one it can.
    int length = 0;
    for (int tile = to; tile != from; tile = path_parent[tile]) {
        path_scratch[length++] = path_rotation[tile];
    }

    int steps = length < 800 ? length : 800;
    if (rotations != NULL) {
        for (int index = 0; index < steps; index++) {
            rotations[index] = path_scratch[length - 1 - index];
        }
    }

    return steps;
}

// 0x415D9C
int idist(int x1, int y1, int x2, int y2)
{
    int dx = x2 - x1;
    if (dx < 0) {
        dx = -dx;
    }

    int dy = y2 - y1;
    if (dy < 0) {
        dy = -dy;
    }

    int dm = (dx <= dy) ? dx : dy;

    return dx + dy - (dm / 2);
}

// 0x415DC0
int EST(int tile1, int tile2)
{
    int x1;
    int y1;
    tile_coord(tile1, &x1, &y1, map_elevation);

    int x2;
    int y2;
    tile_coord(tile2, &x2, &y2, map_elevation);

    return idist(x1, y1, x2, y2);
}

// 0x415E0C
int make_straight_path(Object* a1, int from, int to, StraightPathNode* pathNodes, Object** a5, int a6)
{
    return make_straight_path_func(a1, from, to, pathNodes, a5, a6, obj_blocking_at);
}

// TODO: Rather complex, but understandable, needs testing.
//
// 0x415E28
int make_straight_path_func(Object* a1, int from, int to, StraightPathNode* pathNodes, Object** a5, int a6, PathBuilderCallback* callback)
{
    if (a5 != NULL) {
        Object* v11 = callback(a1, from, a1->elevation);
        if (v11 != NULL) {
            if (v11 != *a5 && (a6 != 32 || (v11->flags & OBJECT_SHOOT_THRU) == 0)) {
                *a5 = v11;
                return 0;
            }
        }
    }

    int fromX;
    int fromY;
    tile_coord(from, &fromX, &fromY, a1->elevation);
    fromX += 16;
    fromY += 8;

    int toX;
    int toY;
    tile_coord(to, &toX, &toY, a1->elevation);
    toX += 16;
    toY += 8;

    int stepX;
    int deltaX = toX - fromX;
    if (deltaX > 0)
        stepX = 1;
    else if (deltaX < 0)
        stepX = -1;
    else
        stepX = 0;

    int stepY;
    int deltaY = toY - fromY;
    if (deltaY > 0)
        stepY = 1;
    else if (deltaY < 0)
        stepY = -1;
    else
        stepY = 0;

    int v48 = 2 * abs(toX - fromX);
    int v47 = 2 * abs(toY - fromY);

    int tileX = fromX;
    int tileY = fromY;

    int pathNodeIndex = 0;
    int prevTile = from;
    int v22 = 0;
    int tile;

    if (v48 <= v47) {
        int middle = v48 - v47 / 2;
        while (true) {
            tile = tile_num(tileX, tileY, a1->elevation);

            v22 += 1;
            if (v22 == a6) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (pathNodes != NULL) {
                    StraightPathNode* pathNode = &(pathNodes[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = a1->elevation;

                    tile_coord(tile, &fromX, &fromY, a1->elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                v22 = 0;
                pathNodeIndex++;
            }

            if (tileY == toY) {
                if (a5 != NULL) {
                    *a5 = NULL;
                }
                break;
            }

            if (middle >= 0) {
                tileX += stepX;
                middle -= v47;
            }

            tileY += stepY;
            middle += v48;

            if (tile != prevTile) {
                if (a5 != NULL) {
                    Object* obj = callback(a1, tile, a1->elevation);
                    if (obj != NULL) {
                        if (obj != *a5 && (a6 != 32 || (obj->flags & OBJECT_SHOOT_THRU) == 0)) {
                            *a5 = obj;
                            break;
                        }
                    }
                }
                prevTile = tile;
            }
        }
    } else {
        int middle = v47 - v48 / 2;
        while (true) {
            tile = tile_num(tileX, tileY, a1->elevation);

            v22 += 1;
            if (v22 == a6) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (pathNodes != NULL) {
                    StraightPathNode* pathNode = &(pathNodes[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = a1->elevation;

                    tile_coord(tile, &fromX, &fromY, a1->elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                v22 = 0;
                pathNodeIndex++;
            }

            if (tileX == toX) {
                if (a5 != NULL) {
                    *a5 = NULL;
                }
                break;
            }

            if (middle >= 0) {
                tileY += stepY;
                middle -= v48;
            }

            tileX += stepX;
            middle += v47;

            if (tile != prevTile) {
                if (a5 != NULL) {
                    Object* obj = callback(a1, tile, a1->elevation);
                    if (obj != NULL) {
                        if (obj != *a5 && (a6 != 32 || (obj->flags & OBJECT_SHOOT_THRU) == 0)) {
                            *a5 = obj;
                            break;
                        }
                    }
                }
                prevTile = tile;
            }
        }
    }

    if (v22 != 0) {
        if (pathNodeIndex >= 200) {
            return 0;
        }

        if (pathNodes != NULL) {
            StraightPathNode* pathNode = &(pathNodes[pathNodeIndex]);
            pathNode->tile = tile;
            pathNode->elevation = a1->elevation;

            tile_coord(tile, &fromX, &fromY, a1->elevation);
            pathNode->x = tileX - fromX - 16;
            pathNode->y = tileY - fromY - 8;
        }

        pathNodeIndex += 1;
    } else {
        if (pathNodeIndex > 0 && pathNodes != NULL) {
            pathNodes[pathNodeIndex - 1].elevation = a1->elevation;
        }
    }

    return pathNodeIndex;
}

// 0x416258
static int anim_move_to_object(Object* from, Object* to, int a3, int anim, int animationSequenceIndex)
{
    bool hidden = (to->flags & OBJECT_HIDDEN);
    to->flags |= OBJECT_HIDDEN;

    int moveSadIndex = anim_move(from, to->tile, to->elevation, -1, anim, 0, animationSequenceIndex);

    if (!hidden) {
        to->flags &= ~OBJECT_HIDDEN;
    }

    if (moveSadIndex == -1) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[moveSadIndex]);
    // NOTE: Original code is somewhat different. Due to some kind of
    // optimization this value is either 1 or 2, which is later used in
    // subsequent calculations and rotations array lookup.
    bool isMultihex = (from->flags & OBJECT_MULTIHEX);
    sad_entry->field_1C -= (isMultihex ? 2 : 1);
    if (sad_entry->field_1C <= 0) {
        sad_entry->field_20 = -1000;
        anim_set_continue(animationSequenceIndex, 0);
    }

    sad_entry->field_24 = tile_num_in_direction(to->tile, sad_entry->rotations[isMultihex ? sad_entry->field_1C + 1 : sad_entry->field_1C], 1);

    if (isMultihex) {
        sad_entry->field_24 = tile_num_in_direction(sad_entry->field_24, sad_entry->rotations[sad_entry->field_1C], 1);
    }

    if (a3 != -1 && a3 < sad_entry->field_1C) {
        sad_entry->field_1C = a3;
    }

    return 0;
}

// 0x4163B4
static int make_stair_path(Object* object, int from, int fromElevation, int to, int toElevation, StraightPathNode* a6, Object** obstaclePtr)
{
    int elevation = fromElevation;
    if (elevation > toElevation) {
        elevation = toElevation;
    }

    int fromX;
    int fromY;
    tile_coord(from, &fromX, &fromY, fromElevation);
    fromX += 16;
    fromY += 8;

    int toX;
    int toY;
    tile_coord(to, &toX, &toY, toElevation);
    toX += 16;
    toY += 8;

    if (obstaclePtr != NULL) {
        *obstaclePtr = NULL;
    }

    int ddx = 2 * abs(toX - fromX);

    int stepX;
    int deltaX = toX - fromX;
    if (deltaX > 0) {
        stepX = 1;
    } else if (deltaX < 0) {
        stepX = -1;
    } else {
        stepX = 0;
    }

    int ddy = 2 * abs(toY - fromY);

    int stepY;
    int deltaY = toY - fromY;
    if (deltaY > 0) {
        stepY = 1;
    } else if (deltaY < 0) {
        stepY = -1;
    } else {
        stepY = 0;
    }

    int tileX = fromX;
    int tileY = fromY;

    int pathNodeIndex = 0;
    int prevTile = from;
    int iteration = 0;
    int tile;

    if (ddx > ddy) {
        int middle = ddy - ddx / 2;
        while (true) {
            tile = tile_num(tileX, tileY, elevation);

            iteration += 1;
            if (iteration == 16) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (a6 != NULL) {
                    StraightPathNode* pathNode = &(a6[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = elevation;

                    tile_coord(tile, &fromX, &fromY, elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                iteration = 0;
                pathNodeIndex++;
            }

            if (tileX == toX) {
                break;
            }

            if (middle >= 0) {
                tileY += stepY;
                middle -= ddx;
            }

            tileX += stepX;
            middle += ddy;

            if (tile != prevTile) {
                if (obstaclePtr != NULL) {
                    *obstaclePtr = obj_blocking_at(object, tile, object->elevation);
                    if (*obstaclePtr != NULL) {
                        break;
                    }
                }
                prevTile = tile;
            }
        }
    } else {
        int middle = ddx - ddy / 2;
        while (true) {
            tile = tile_num(tileX, tileY, elevation);

            iteration += 1;
            if (iteration == 16) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (a6 != NULL) {
                    StraightPathNode* pathNode = &(a6[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = elevation;

                    tile_coord(tile, &fromX, &fromY, elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                iteration = 0;
                pathNodeIndex++;
            }

            if (tileY == toY) {
                break;
            }

            if (middle >= 0) {
                tileX += stepX;
                middle -= ddy;
            }

            tileY += stepY;
            middle += ddx;

            if (tile != prevTile) {
                if (obstaclePtr != NULL) {
                    *obstaclePtr = obj_blocking_at(object, tile, object->elevation);
                    if (*obstaclePtr != NULL) {
                        break;
                    }
                }
                prevTile = tile;
            }
        }
    }

    if (iteration != 0) {
        if (pathNodeIndex >= 200) {
            return 0;
        }

        if (a6 != NULL) {
            StraightPathNode* pathNode = &(a6[pathNodeIndex]);
            pathNode->tile = tile;
            pathNode->elevation = elevation;

            tile_coord(tile, &fromX, &fromY, elevation);
            pathNode->x = tileX - fromX - 16;
            pathNode->y = tileY - fromY - 8;
        }

        pathNodeIndex++;
    } else {
        if (pathNodeIndex > 0) {
            if (a6 != NULL) {
                a6[pathNodeIndex - 1].elevation = toElevation;
            }
        }
    }

    return pathNodeIndex;
}

// 0x416754
static int anim_move_to_tile(Object* obj, int tile, int elev, int a4, int anim, int animationSequenceIndex)
{
    int v1;

    v1 = anim_move(obj, tile, elev, -1, anim, 0, animationSequenceIndex);
    if (v1 == -1) {
        return -1;
    }

    if (obj_blocking_at(obj, tile, elev)) {
        AnimationSad* sad_entry = &(sad[v1]);
        sad_entry->field_1C--;
        if (sad_entry->field_1C <= 0) {
            sad_entry->field_20 = -1000;
            anim_set_continue(animationSequenceIndex, 0);
        }

        sad_entry->field_24 = tile_num_in_direction(tile, sad_entry->rotations[sad_entry->field_1C], 1);
        if (a4 != -1 && a4 < sad_entry->field_1C) {
            sad_entry->field_1C = a4;
        }
    }

    return 0;
}

// CE: WHAT THE LAST ROUTED MOVE ACTUALLY GOT.
//
// ⚠️ register_object_move_to_* / run_to_* only QUEUE a move, so the register_end that follows says
// "queued", never "reachable" -- the routing happens here, as the move description starts, and a
// 0-step make_path kills the move silently. combat_rt's chase read rc == 0 as "routed", so a
// critter with no way out (trace_2026-10-04_153226: an Adytum guard, id=2105, ~60 failed make_path
// calls a second for minutes) was never declared unreachable and never gave up: it stood there
// "wanting to come", burning a full path search every frame ("npc chtějí jít za mnou, ale nemůžou
// se dostat ze stanu nebo z místnosti").
//
// The first description of a sequence starts INSIDE register_end, so a caller can reset this,
// register, and read the verdict straight after.
static Object* anim_last_path_obj = NULL;
static int anim_last_path_steps_value = -1;

void anim_reset_last_path()
{
    anim_last_path_obj = NULL;
    anim_last_path_steps_value = -1;
}

int anim_last_path_steps(Object* obj)
{
    return obj != NULL && obj == anim_last_path_obj ? anim_last_path_steps_value : -1;
}

// 0x416778
static int anim_move(Object* obj, int tile, int elev, int a3, int anim, int a5, int animationSequenceIndex)
{
    if (curr_sad == ANIMATION_SAD_LIST_CAPACITY) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[curr_sad]);
    sad_entry->obj = obj;

    if (a5) {
        sad_entry->flags = ANIM_SAD_0x20;
    } else {
        sad_entry->flags = 0;
    }

    sad_entry->field_20 = -2000;
    sad_entry->fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    sad_entry->animationTimestamp = 0;
    sad_entry->ticksPerFrame = compute_tpf(obj, sad_entry->fid);
    sad_entry->field_24 = tile;
    sad_entry->animationSequenceIndex = animationSequenceIndex;
    sad_entry->anim = anim;

    sad_entry->field_1C = make_path(obj, obj->tile, tile, sad_entry->rotations, a5);
    anim_last_path_obj = obj;
    anim_last_path_steps_value = sad_entry->field_1C;

    // TEMP DIAG (walk-in-place hunt, 2026-08-10). combat_rt's [FLOW/walk-seg] measures the wrong
    // stage: register_object_move_to_tile only QUEUES the move, so its rc=0 says "queued", not
    // "reachable" -- the real routing is this make_path, and a 0-step result kills the move right
    // here. That is the last unknown in the id=1798 case (animates a walk, tile never changes,
    // alone on the tile, destination a valid 3 tiles away, registration fine).
    // Critters in realtime only, so the town's script-driven wandering doesn't drown the log.
    // STRIP with the other walk diags once this is understood.
    if (combat_rt_enabled() && FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
        trace_event(TRACE_FLOW, "make-path", "id=%d pid=%d from=%d to=%d steps=%d anim=%d",
            obj->id, obj->pid, obj->tile, tile, sad_entry->field_1C, anim);
    }

    if (sad_entry->field_1C == 0) {
        sad_entry->field_20 = -1000;
        return -1;
    }

    if (a3 != -1 && sad_entry->field_1C > a3) {
        sad_entry->field_1C = a3;
    }

    return curr_sad++;
}

// 0x4168D0
static int anim_move_straight_to_tile(Object* obj, int tile, int elevation, int anim, int animationSequenceIndex, int flags)
{
    if (curr_sad == ANIMATION_SAD_LIST_CAPACITY) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[curr_sad]);
    sad_entry->obj = obj;
    sad_entry->flags = flags | ANIM_SAD_STRAIGHT;
    if (anim == -1) {
        sad_entry->fid = obj->fid;
        sad_entry->flags |= ANIM_SAD_NO_ANIM;
    } else {
        sad_entry->fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    }
    sad_entry->field_20 = -2000;
    sad_entry->animationTimestamp = 0;
    sad_entry->ticksPerFrame = compute_tpf(obj, sad_entry->fid);
    sad_entry->animationSequenceIndex = animationSequenceIndex;

    int v15;
    if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
        if (FID_ANIM_TYPE(obj->fid) == ANIM_JUMP_BEGIN)
            v15 = 16;
        else
            v15 = 4;
    } else {
        v15 = 32;
    }

    sad_entry->field_1C = make_straight_path(obj, obj->tile, tile, sad_entry->field_28, NULL, v15);
    if (sad_entry->field_1C == 0) {
        sad_entry->field_20 = -1000;
        return -1;
    }

    curr_sad++;

    return 0;
}

// 0x416AA8
int anim_move_on_stairs(Object* obj, int tile, int elevation, int anim, int animationSequenceIndex)
{
    if (curr_sad == ANIMATION_SAD_LIST_CAPACITY) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[curr_sad]);
    sad_entry->flags = ANIM_SAD_STRAIGHT;
    sad_entry->obj = obj;
    if (anim == -1) {
        sad_entry->fid = obj->fid;
        sad_entry->flags |= ANIM_SAD_NO_ANIM;
    } else {
        sad_entry->fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    }
    sad_entry->field_20 = -2000;
    sad_entry->animationTimestamp = 0;
    sad_entry->ticksPerFrame = compute_tpf(obj, sad_entry->fid);
    sad_entry->animationSequenceIndex = animationSequenceIndex;
    sad_entry->field_1C = make_stair_path(obj, obj->tile, obj->elevation, tile, elevation, sad_entry->field_28, NULL);
    if (sad_entry->field_1C == 0) {
        sad_entry->field_20 = -1000;
        return -1;
    }

    curr_sad++;

    return 0;
}

// 0x416BC4
int check_for_falling(Object* obj, int anim, int a3)
{
    if (curr_sad == ANIMATION_SAD_LIST_CAPACITY) {
        return -1;
    }

    if (check_gravity(obj->tile, obj->elevation) == obj->elevation) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[curr_sad]);
    sad_entry->flags = ANIM_SAD_STRAIGHT;
    sad_entry->obj = obj;
    if (anim == -1) {
        sad_entry->fid = obj->fid;
        sad_entry->flags |= ANIM_SAD_NO_ANIM;
    } else {
        sad_entry->fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    }
    sad_entry->field_20 = -2000;
    sad_entry->animationTimestamp = 0;
    sad_entry->ticksPerFrame = compute_tpf(obj, sad_entry->fid);
    sad_entry->animationSequenceIndex = a3;
    sad_entry->field_1C = make_straight_path_func(obj, obj->tile, obj->tile, sad_entry->field_28, 0, 16, obj_blocking_at);
    if (sad_entry->field_1C == 0) {
        sad_entry->field_20 = -1000;
        return -1;
    }

    curr_sad++;

    return 0;
}

// 0x416CDC
static void object_move(int index)
{
    AnimationSad* sad_entry = &(sad[index]);
    Object* object = sad_entry->obj;

    Rect dirty;
    Rect temp;

    // Smooth movement: where the body is DRAWN before this step moves it. The glide to the new
    // position starts from there (anim_smooth_end, at the bottom).
    float smoothX;
    float smoothY;
    bool smoothing = anim_smooth_begin(object, &smoothX, &smoothY);

    if (sad_entry->field_20 == -2000) {
        // Where the body stands BEFORE obj_move_to_tile zeroes obj->x/y. A walk that starts after
        // a stop mid-tile begins from here instead of from the tile centre.
        int seedX = object->x;
        int seedY = object->y;

        // The gait the body is in right now -- a leg that follows a leg of the same gait carries
        // on with its stride (rtWalkContinuous below).
        int prevAnim = FID_ANIM_TYPE(object->fid);

        obj_move_to_tile(object, object->tile, object->elevation, &dirty);

        // Realtime NPC walk continuity: a chasing NPC steps ONE tile at a time (rt_step_toward),
        // so each tile begins a FRESH walk sad. Resetting the leg frame to 0 every tile drops the
        // tail of the cycle whenever the art crosses a tile in fewer frames than it has -- the
        // "NPC walk skips frames / looks choppy (and reads as faster)" report. Keep the current
        // leg frame instead so the cycle runs continuously across tiles. Safe because the body
        // translation is handled independently by walk_smooth_offset (below), decoupled from the
        // frame; and all of a critter's walk facings share a frame count, so the preserved index
        // stays valid (clamped after the fid swap just in case). The dude already moves
        // continuously via anim_extend_move, and turn-based is untouched: both reset to 0 as before.
        // ⚠️ RUNNING TOO, when the body is already running: a chase leg is usually a run, and its
        // stride restarted at frame 0 at the start of every leg. Only from a run, though -- the run
        // cycle has its own frame count, and a run that starts from a stand should start at its top.
        bool rtWalkContinuous = combat_rt_enabled() && object != obj_dude
            && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
            && (sad_entry->anim == ANIM_WALK
                || (sad_entry->anim == ANIM_RUNNING && prevAnim == ANIM_RUNNING));

        if (!rtWalkContinuous) {
            obj_set_frame(object, 0, &temp);
            rect_min_bound(&dirty, &temp, &dirty);
        }

        obj_set_rotation(object, sad_entry->rotations[0], &temp);
        rect_min_bound(&dirty, &temp, &dirty);

        int fid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, sad_entry->anim, (object->fid & 0xF000) >> 12, object->rotation + 1);
        obj_change_fid(object, fid, &temp);
        rect_min_bound(&dirty, &temp, &dirty);

        if (rtWalkContinuous) {
            CacheEntry* walkHandle;
            Art* walkArt = art_ptr_lock(object->fid, &walkHandle);
            if (walkArt != NULL) {
                int frameCount = art_frame_max_frame(walkArt);
                art_ptr_unlock(walkHandle);
                if (frameCount > 0 && object->frame >= frameCount) {
                    obj_set_frame(object, object->frame % frameCount, &temp);
                    rect_min_bound(&dirty, &temp, &dirty);
                }
            }
        }

        if (combat_rt_enabled() && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
            && (sad_entry->anim == ANIM_WALK || sad_entry->anim == ANIM_RUNNING)) {
            walk_smooth_seed(object, seedX, seedY);
        }

        sad_entry->field_20 = 0;
    } else {
        obj_inc_frame(object, &dirty);
    }

    int rotation = sad_entry->rotations[sad_entry->field_20];

    int frameX;
    int frameY;

    CacheEntry* cacheHandle;
    Art* art = art_ptr_lock(object->fid, &cacheHandle);
    if (art != NULL) {
        art_frame_hot(art, object->frame, object->rotation, &frameX, &frameY);
        art_ptr_unlock(cacheHandle);
    } else {
        frameX = 0;
        frameY = 0;
    }

    // Realtime combat: smooth the PLAYER's walk so the per-frame sprite advance is
    // constant within each tile, instead of the uneven per-frame offsets baked into
    // the walk art. The female walk art has uneven X-deltas on the E/W axis, and the
    // realtime follow-cam (it scrolls the world by -obj->x) turns that into a full-
    // screen horizontal judder. We override the raw delta with a constant-velocity
    // step toward off_tile, so obj->x marches linearly to the tile boundary and lands
    // exactly on it (tile crossings stay seamless, walk speed per tile is unchanged
    // because the ramp spans the art's own frames-per-tile). The walk frames still
    // cycle for the leg animation -- only the body translation is evened out. Applied to
    // EVERY critter in realtime (not just the dude): NPC walk art lurches the same way --
    // the radscorpion's NW facing front-loads its motion in big jumps and reads as "fast"
    // even though its per-tile time is normal -- so smoothing the within-tile motion fixes
    // that. Turn-based keeps the original art-driven motion.
    // ⚠️ RUNNING IS IN TOO. It never was: the gate said ANIM_WALK, so a run crossed a tile in 2.5
    // art frames of raw hot-spot offsets. There is nothing walk-specific about a constant-velocity
    // ramp.
    int stepDone = -1;
    if (combat_rt_enabled() && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
        && (sad_entry->anim == ANIM_WALK || sad_entry->anim == ANIM_RUNNING)) {
        stepDone = walk_smooth_offset(object, rotation, sad_entry->anim, &frameX, &frameY);
    }

    obj_offset(object, frameX, frameY, &temp);
    rect_min_bound(&dirty, &temp, &dirty);

    // Realtime combat: normalize this NPC walk step so every facing crosses a tile
    // at the same (slowest) cadence, correcting per-direction art speed asymmetry.
    // compute_tpf can't do this (it runs before the path/direction is known), so it
    // is applied here per step where the actual rotation is set. The scheduler re-
    // reads ticksPerFrame each tick, so updating it mid-sad takes effect. Player and
    // turn-based combat are untouched.
    // MOVEMENT RE-TIMING, all of it in one place: art normalisation, the Agility run curve and the
    // crippled-leg limp, applied to a FRESHLY COMPUTED base every step.
    //
    // ⚠️ NEVER MULTIPLY sad_entry->ticksPerFrame BY ANYTHING. It is state that survives between
    // steps, and this code runs on EVERY step, so scaling the stored value compounds. The limp
    // shipped that way for one evening and the arithmetic is brutal: the NPC walk branch only
    // re-times when its own scale exceeds 1.0, so on every other step nothing reset the field and
    // the x1.6 stacked -- 1.6, 2.56, 4.1, 6.6, 10.5 -- until a single frame lasted seconds. The
    // critter froze mid-tile with the walk animation still "playing", never reached the tile
    // boundary that triggers obj_move_to_tile, and the 1.25 s stuck-move watchdog then cut and
    // re-issued the walk for ever. On screen: a ghoul standing on one spot, twitching.
    //
    // Measured, trace_2026-08-30_192334: id=2115 (the critter the cripple trace names, legsNow=1)
    // re-issued `from=22894` for 28 seconds without ever leaving it, sub-tile offset stuck at (6,5)
    // and frame stuck at 3, with nothing blocking it and the animation table at 5/64.
    //
    // Deriving from compute_tpf every time makes the whole block idempotent: run it once or a
    // hundred times on the same step and the answer is identical.
    if (combat_rt_enabled()
        && (sad_entry->anim == ANIM_WALK || sad_entry->anim == ANIM_RUNNING)) {
        float scale = 1.0f;
        bool retime = false;

        if (sad_entry->anim == ANIM_RUNNING) {
            // Run speed: art variance out, Agility in -- and unlike the walk branch this DOES
            // include the dude, so the rule is the same for everyone. Both directions allowed (the
            // stat has to be able to make you faster, not just slower), bounded by run_speed_scale's
            // own clamps. Always re-timed, as before.
            scale = run_speed_scale(object, rotation);
            retime = true;
        } else if (object != obj_dude) {
            // NPC walk: equalise this facing's tile-crossing time against the reference. Only ever
            // SLOWS (the > 1.0 test), which is why the dude's walk is left on its registration tpf.
            float dirScale = walk_dir_slowdown(object, rotation);
            if (dirScale > 1.0f) {
                scale = dirScale;
                retime = true;
            }
        }

        // ⚠️ THE LIMP IS A FACTOR HERE, NOT A SEPARATE PASS -- and it must stay OUT of compute_tpf
        // itself, because walk_dir_slowdown builds its scale from compute_tpf on both sides, so a
        // factor hidden in there cancels out of the quotient exactly. It also has to stay out of the
        // dude's reference, or shooting your own leg would slow every NPC on the map to match you.
        float crip = crippled_move_scale(object);
        if (crip > 1.0f) {
            retime = true;
        }

        // Out-of-combat travel speed, folded in as one more factor for the same reason the limp is:
        // it must NOT live inside compute_tpf, or it cancels out of walk_dir_slowdown's quotient and
        // drags the dude's reference around with it.
        //
        // ⚠️ RE-TIMED UNCONDITIONALLY for the player's side, even when the factor is 1.0. tpf is
        // STATE that outlives the step, so a factor that merely stops being applied leaves the last
        // FAST value in the field for the rest of the sequence -- i.e. the dude would keep travel
        // speed through the opening seconds of the fight that just started. Re-timing always is free
        // and exact: the whole block derives from a fresh compute_tpf and assigns once, so running it
        // with travel == 1.0 reproduces the registration value bit for bit.
        float travel = travel_speed_scale(object);
        if (travel != 1.0f || object == obj_dude || isPartyMember(object)) {
            retime = true;
        }

        if (retime) {
            unsigned int tpf = (unsigned int)(compute_tpf(object, sad_entry->fid) * scale * crip * travel);
            sad_entry->ticksPerFrame = tpf > 0 ? tpf : 1;
        }
    }

    int y = off_tile[1][rotation];
    int x = off_tile[0][rotation];

    // Has the step covered its tile? The ramp answers for itself when it ran, because a step may
    // begin somewhere other than a tile centre and obj->x/y is then no longer a measure of progress
    // on its own. For a step that starts at the centre its answer is identical to the test below.
    bool crossed = stepDone >= 0
        ? stepDone != 0
        : ((x > 0 && x <= object->x) || (x < 0 && x >= object->x) || (y > 0 && y <= object->y) || (y < 0 && y >= object->y));

    if (crossed) {
        x = object->x - x;
        y = object->y - y;

        int v10 = tile_num_in_direction(object->tile, rotation, 1);
        Object* v12 = obj_blocking_at(object, v10, object->elevation);
        if (v12 != NULL) {
            if (!anim_can_use_door(object, v12)) {
                sad_entry->field_1C = make_path(object, object->tile, sad_entry->field_24, sad_entry->rotations, 1);
                if (sad_entry->field_1C != 0) {
                    obj_move_to_tile(object, object->tile, object->elevation, &temp);
                    rect_min_bound(&dirty, &temp, &dirty);

                    obj_set_frame(object, 0, &temp);
                    rect_min_bound(&dirty, &temp, &dirty);

                    obj_set_rotation(object, sad_entry->rotations[0], &temp);
                    rect_min_bound(&dirty, &temp, &dirty);

                    sad_entry->field_20 = 0;
                } else {
                    sad_entry->field_20 = -1000;
                }
                v10 = -1;
            } else {
                obj_use_door(object, v12, 0);
            }
        }

        if (v10 != -1) {
            obj_move_to_tile(object, v10, object->elevation, &temp);
            rect_min_bound(&dirty, &temp, &dirty);

            int v17 = 0;
            // In realtime combat, movement is free (combat_rt runs its own AP pool for attacks
            // and never charges movement to it). The turn-based per-tile combat.ap deduction below
            // would otherwise truncate any move once combat.ap hits 0 -- which combat_rt hides for
            // the hostiles it manages by topping their combat.ap up each step, but NOT for
            // bystanders it doesn't drive: a fleeing/wandering Shady Sands townsperson has ap 0, so
            // its script-issued walk gets cut to a single tile and it "walks in place" forever.
            // Skipping the deduction entirely under combat_rt lets every critter's move play out.
            if (isInCombat() && !combat_rt_enabled() && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER) {
                int v18 = critter_compute_ap_from_distance(object, 1);
                if (combat_free_move < v18) {
                    int ap = object->data.critter.combat.ap;
                    int v20 = v18 - combat_free_move;
                    combat_free_move = 0;
                    if (v20 > ap) {
                        object->data.critter.combat.ap = 0;
                    } else {
                        object->data.critter.combat.ap = ap - v20;
                    }
                } else {
                    combat_free_move -= v18;
                }

                if (object == obj_dude) {
                    intface_update_move_points(obj_dude->data.critter.combat.ap, combat_free_move);
                }

                v17 = (object->data.critter.combat.ap + combat_free_move) <= 0;
            }

            sad_entry->field_20 += 1;

            if (sad_entry->field_20 == sad_entry->field_1C || v17) {
                sad_entry->field_20 = -1000;
            } else {
                obj_set_rotation(object, sad_entry->rotations[sad_entry->field_20], &temp);
                rect_min_bound(&dirty, &temp, &dirty);

                obj_offset(object, x, y, &temp);
                rect_min_bound(&dirty, &temp, &dirty);
            }
        }
    }

    // The step's own clock, for the leg that follows this one (anim_move_cadence_hold).
    anim_note_move_step(object, sad_entry->anim, sad_entry->animationTimestamp, sad_entry->ticksPerFrame);

    if (smoothing) {
        anim_smooth_end(object, smoothX, smoothY, sad_entry->ticksPerFrame, &dirty);
    }

    tile_refresh_rect(&dirty, object->elevation);
    if (sad_entry->field_20 == -1000) {
        anim_set_continue(sad_entry->animationSequenceIndex, 1);
    }
}

// 0x417128
static void object_straight_move(int index)
{
    AnimationSad* sad_entry = &(sad[index]);
    Object* object = sad_entry->obj;

    Rect dirtyRect;
    Rect temp;

    // Smooth movement for the straight movers too: a projectile hops 32 px per frame of its art, a
    // body knocked back 4 px. The dude's own knockback is left alone -- the camera is welded to him.
    float smoothX;
    float smoothY;
    bool smoothing = anim_smooth_begin(object, &smoothX, &smoothY);

    if (sad_entry->field_20 == -2000) {
        obj_change_fid(object, sad_entry->fid, &dirtyRect);
        sad_entry->field_20 = 0;
    } else {
        obj_bound(object, &dirtyRect);
    }

    CacheEntry* cacheHandle;
    Art* art = art_ptr_lock(object->fid, &cacheHandle);
    if (art != NULL) {
        int lastFrame = art_frame_max_frame(art) - 1;
        art_ptr_unlock(cacheHandle);

        if ((sad_entry->flags & ANIM_SAD_NO_ANIM) == 0) {
            if ((sad_entry->flags & ANIM_SAD_WAIT_FOR_COMPLETION) == 0 || object->frame < lastFrame) {
                obj_inc_frame(object, &temp);
                rect_min_bound(&dirtyRect, &temp, &dirtyRect);
            }
        }

        if (sad_entry->field_20 < sad_entry->field_1C) {
            StraightPathNode* v12 = &(sad_entry->field_28[sad_entry->field_20]);

            obj_move_to_tile(object, v12->tile, v12->elevation, &temp);
            rect_min_bound(&dirtyRect, &temp, &dirtyRect);

            obj_offset(object, v12->x, v12->y, &temp);
            rect_min_bound(&dirtyRect, &temp, &dirtyRect);

            sad_entry->field_20++;
        }

        if (sad_entry->field_20 == sad_entry->field_1C) {
            if ((sad_entry->flags & ANIM_SAD_WAIT_FOR_COMPLETION) == 0 || object->frame == lastFrame) {
                sad_entry->field_20 = -1000;
            }
        }

        if (smoothing) {
            anim_smooth_end(object, smoothX, smoothY, sad_entry->ticksPerFrame, &dirtyRect);
        }

        tile_refresh_rect(&dirtyRect, sad_entry->obj->elevation);

        if (sad_entry->field_20 == -1000) {
            anim_set_continue(sad_entry->animationSequenceIndex, 1);
        }
    }
}

// 0x417320
// The start-frame bracket (anim_set_start_frame, below).
static Object* anim_start_frame_obj = NULL;
static int anim_start_frame_value = 0;

static int anim_animate(Object* obj, int anim, int animationSequenceIndex, int flags)
{
    if (curr_sad == ANIMATION_SAD_LIST_CAPACITY) {
        return -1;
    }

    AnimationSad* sad_entry = &(sad[curr_sad]);

    int fid;
    if (anim == ANIM_TAKE_OUT) {
        sad_entry->flags = 0;
        fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, ANIM_TAKE_OUT, flags, obj->rotation + 1);
    } else {
        sad_entry->flags = flags;
        fid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    }

    if (!art_exists(fid)) {
        return -1;
    }

    sad_entry->obj = obj;
    sad_entry->fid = fid;
    sad_entry->animationSequenceIndex = animationSequenceIndex;
    sad_entry->animationTimestamp = 0;
    sad_entry->ticksPerFrame = compute_tpf(obj, sad_entry->fid);
    sad_entry->field_20 = 0;
    sad_entry->field_1C = 0;
    sad_entry->startFrame = obj == anim_start_frame_obj && (flags & ANIM_SAD_REVERSE) == 0 ? anim_start_frame_value : 0;

    curr_sad++;

    return 0;
}

// 0x417498
void object_animate()
{
    // Bodies held in their stride by a leg that ended (anim_defer_stand) and that nothing followed.
    // Before the early return: the table can be empty by then.
    anim_deferred_stands(false);

    if (curr_sad == 0) {
        return;
    }

    anim_in_bk = 1;

    for (int index = 0; index < curr_sad; index++) {
        AnimationSad* sad_entry = &(sad[index]);
        if (sad_entry->field_20 == -1000) {
            continue;
        }

        Object* object = sad_entry->obj;

        unsigned int time = get_time();
        if (elapsed_tocks(time, sad_entry->animationTimestamp) < sad_entry->ticksPerFrame) {
            continue;
        }

        // A LEG THAT FOLLOWS A LEG KEEPS THE STRIDE'S BEAT. A new move sad is due at once (its
        // timestamp is 0), so the first step of a chase leg landed a frame after the last step of
        // the one before -- two steps in 16 ms, a lurch every four tiles. Wait out the step that
        // is still running instead.
        if (sad_entry->field_20 == -2000 && sad_entry->field_1C > 0
            && (sad_entry->flags & ANIM_SAD_STRAIGHT) == 0
            && anim_move_cadence_hold(object, sad_entry->anim, time)) {
            continue;
        }

        sad_entry->animationTimestamp = time;

        if (anim_set_check(sad_entry->animationSequenceIndex) == -1) {
            continue;
        }

        if (sad_entry->field_1C > 0) {
            if ((sad_entry->flags & ANIM_SAD_STRAIGHT) != 0) {
                object_straight_move(index);
            } else {
                int savedTile = object->tile;
                object_move(index);
                if (savedTile != object->tile) {
                    scr_chk_spatials_in(object, object->tile, object->elevation);
                }
            }
            continue;
        }

        if (sad_entry->field_20 == 0) {
            for (int index = 0; index < curr_sad; index++) {
                AnimationSad* otherSad = &(sad[index]);
                if (object == otherSad->obj && otherSad->field_20 == -2000) {
                    otherSad->field_20 = -1000;
                    anim_set_continue(otherSad->animationSequenceIndex, 1);
                }
            }
            sad_entry->field_20 = -2000;
        }

        Rect dirtyRect;
        Rect tempRect;

        obj_bound(object, &dirtyRect);

        if (object->fid == sad_entry->fid) {
            if ((sad_entry->flags & ANIM_SAD_REVERSE) == 0) {
                CacheEntry* cacheHandle;
                Art* art = art_ptr_lock(object->fid, &cacheHandle);
                if (art != NULL) {
                    if ((sad_entry->flags & ANIM_SAD_FOREVER) == 0 && object->frame == art_frame_max_frame(art) - 1) {
                        sad_entry->field_20 = -1000;
                        art_ptr_unlock(cacheHandle);

                        if ((sad_entry->flags & ANIM_SAD_HIDE_ON_END) != 0) {
                            // NOTE: Uninline.
                            anim_hide(object, -1);
                        }

                        anim_set_continue(sad_entry->animationSequenceIndex, 1);
                        continue;
                    } else {
                        obj_inc_frame(object, &tempRect);
                        rect_min_bound(&dirtyRect, &tempRect, &dirtyRect);

                        int frameX;
                        int frameY;
                        art_frame_hot(art, object->frame, object->rotation, &frameX, &frameY);

                        obj_offset(object, frameX, frameY, &tempRect);
                        rect_min_bound(&dirtyRect, &tempRect, &dirtyRect);

                        art_ptr_unlock(cacheHandle);
                    }
                }

                tile_refresh_rect(&dirtyRect, map_elevation);

                continue;
            }

            if ((sad_entry->flags & ANIM_SAD_FOREVER) != 0 || object->frame != 0) {
                int x;
                int y;

                CacheEntry* cacheHandle;
                Art* art = art_ptr_lock(object->fid, &cacheHandle);
                if (art != NULL) {
                    art_frame_hot(art, object->frame, object->rotation, &x, &y);
                    art_ptr_unlock(cacheHandle);
                }

                obj_dec_frame(object, &tempRect);
                rect_min_bound(&dirtyRect, &tempRect, &dirtyRect);

                obj_offset(object, -x, -y, &tempRect);
                rect_min_bound(&dirtyRect, &tempRect, &dirtyRect);

                tile_refresh_rect(&dirtyRect, map_elevation);
                continue;
            }

            sad_entry->field_20 = -1000;
            anim_set_continue(sad_entry->animationSequenceIndex, 1);
        } else {
            int x;
            int y;

            CacheEntry* cacheHandle;
            Art* art = art_ptr_lock(object->fid, &cacheHandle);
            if (art != NULL) {
                art_frame_offset(art, object->rotation, &x, &y);
                art_ptr_unlock(cacheHandle);
            } else {
                x = 0;
                y = 0;
            }

            Rect v29;
            obj_change_fid(object, sad_entry->fid, &v29);
            rect_min_bound(&dirtyRect, &v29, &dirtyRect);

            art = art_ptr_lock(object->fid, &cacheHandle);
            if (art != NULL) {
                int frame;
                if ((sad_entry->flags & ANIM_SAD_REVERSE) != 0) {
                    frame = art_frame_max_frame(art) - 1;
                } else {
                    frame = 0;
                }

                obj_set_frame(object, frame, &v29);
                rect_min_bound(&dirtyRect, &v29, &dirtyRect);

                int frameX;
                int frameY;
                art_frame_hot(art, object->frame, object->rotation, &frameX, &frameY);

                Rect v19;
                obj_offset(object, x + frameX, y + frameY, &v19);
                rect_min_bound(&dirtyRect, &v19, &dirtyRect);

                // Started part way in (anim_set_start_frame): through the skipped frames the way the
                // forward ticks go, one frame and its hot-spot offset at a time.
                if (sad_entry->startFrame > 0 && (sad_entry->flags & ANIM_SAD_REVERSE) == 0) {
                    int last = art_frame_max_frame(art) - 1;
                    int target = sad_entry->startFrame < last ? sad_entry->startFrame : last;
                    while (object->frame < target) {
                        obj_inc_frame(object, &v19);
                        rect_min_bound(&dirtyRect, &v19, &dirtyRect);
                        art_frame_hot(art, object->frame, object->rotation, &frameX, &frameY);
                        obj_offset(object, frameX, frameY, &v19);
                        rect_min_bound(&dirtyRect, &v19, &dirtyRect);
                    }
                    sad_entry->startFrame = 0;
                }

                art_ptr_unlock(cacheHandle);
            } else {
                obj_set_frame(object, 0, &v29);
                rect_min_bound(&dirtyRect, &v29, &dirtyRect);
            }

            tile_refresh_rect(&dirtyRect, map_elevation);
        }
    }

    anim_in_bk = 0;

    object_anim_compact();
}

// 0x417880
static void object_anim_compact()
{
    for (int index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        AnimationSequence* animationSequence = &(anim_set[index]);
        if ((animationSequence->flags & ANIM_SEQ_0x20) != 0) {
            animationSequence->flags = 0;
        }
    }

    int index = 0;
    for (; index < curr_sad; index++) {
        if (sad[index].field_20 == -1000) {
            int v2 = index + 1;
            for (; v2 < curr_sad; v2++) {
                if (sad[v2].field_20 != -1000) {
                    break;
                }
            }

            if (v2 == curr_sad) {
                break;
            }

            if (index != v2) {
                memcpy(&(sad[index]), &(sad[v2]), sizeof(AnimationSad));
                sad[v2].field_20 = -1000;
                sad[v2].flags = 0;
            }
        }
    }
    curr_sad = index;
}

// 0x417964
int check_move(int* a1)
{
    int x;
    int y;
    mouse_get_position(&x, &y);

    int tile = tile_num(x, y, map_elevation);
    if (tile == -1) {
        return -1;
    }

    if (isInCombat()) {
        if (*a1 != -1) {
            if (keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_LCTRL]) {
                int hitMode;
                bool aiming;
                intface_get_attack(&hitMode, &aiming);

                int v6 = item_mp_cost(obj_dude, hitMode, aiming);
                *a1 = *a1 - v6;
                if (*a1 <= 0) {
                    return -1;
                }
            }
        }
    } else {
        bool interruptWalk;
        configGetBool(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_INTERRUPT_WALK_KEY, &interruptWalk);
        if (interruptWalk) {
            register_clear(obj_dude);
        }
    }

    return tile;
}

// 0x417A1C
int dude_move(int action_points)
{
    static int lastDestination = -2;

    int dest;

    dest = check_move(&action_points);
    if (dest == -1) {
        return -1;
    }

    if (lastDestination == dest) {
        return dude_run(action_points);
    }

    lastDestination = dest;

    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_move_to_tile(obj_dude, dest, obj_dude->elevation, action_points, 0);
    return register_end();
}

// 0x417A5C
int dude_run(int action_points)
{
    int dest;

    dest = check_move(&action_points);
    if (dest == -1) {
        return -1;
    }

    if (!perk_level(PERK_SILENT_RUNNING)) {
        pc_flag_off(PC_FLAG_SNEAKING);
    }

    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_run_to_tile(obj_dude, dest, obj_dude->elevation, action_points, 0);
    return register_end();
}

// 0x417AB0
void dude_fidget()
{
    // 0x4FEAAC
    static unsigned int last_time = 0;

    // 0x4FEAB0
    static unsigned int next_time = 0;

    // 0x56B570
    static Object* fidget_ptr[100];

    if (game_user_wants_to_quit != 0) {
        return;
    }

    if (isInCombat()) {
        return;
    }

    if (vcr_status() != VCR_STATE_TURNED_OFF) {
        return;
    }

    if ((obj_dude->flags & OBJECT_HIDDEN) != 0) {
        return;
    }

    unsigned int now = get_bk_time();
    if (elapsed_tocks(now, last_time) <= next_time) {
        return;
    }

    last_time = now;

    int count = 0;
    Object* object = obj_find_first_at(obj_dude->elevation);
    while (object != NULL) {
        if (count >= 100) {
            break;
        }

        // CE: with the reload animation on, the dude's STAND loop is his RELOAD and nothing else --
        // left in the random idle pool it would play "reloading" at random and say nothing true
        // (see anim_get_reload_anim). Everyone else keeps fidgeting.
        if (object == obj_dude && anim_get_reload_anim()) {
            object = obj_find_next_at();
            continue;
        }

        if ((object->flags & OBJECT_HIDDEN) == 0 && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER && FID_ANIM_TYPE(object->fid) == ANIM_STAND && !critter_is_dead(object)) {
            Rect rect;
            obj_bound(object, &rect);

            Rect intersection;
            if (rect_inside_bound(&rect, &scr_size, &intersection) == 0) {
                fidget_ptr[count++] = object;
            }
        }

        object = obj_find_next_at();
    }

    int v13;
    if (count != 0) {
        int r = roll_random(0, count - 1);
        Object* object = fidget_ptr[r];

        register_begin(ANIMATION_REQUEST_UNRESERVED | ANIMATION_REQUEST_INSIGNIFICANT);

        bool v8 = false;
        if (object == obj_dude) {
            v8 = true;
        } else {
            char v15[16];
            v15[0] = '\0';
            art_get_base_name(1, object->fid & 0xFFF, v15);
            if (v15[0] == 'm' || v15[0] == 'M') {
                if (obj_dist(object, obj_dude) < stat_level(obj_dude, STAT_PERCEPTION) * 2) {
                    v8 = true;
                }
            }
        }

        if (v8) {
            const char* sfx = gsnd_build_character_sfx_name(object, ANIM_STAND, CHARACTER_SOUND_EFFECT_UNUSED);
            register_object_play_sfx(object, sfx, 0);
        }

        register_object_animate(object, ANIM_STAND, 0);
        register_end();

        v13 = 20 / count;
    } else {
        v13 = 7;
    }

    if (v13 < 1) {
        v13 = 1;
    } else if (v13 > 7) {
        v13 = 7;
    }

    next_time = roll_random(0, 3000) + 1000 * v13;
}

// 0x417CB0
void dude_stand(Object* obj, int rotation, int fid)
{
    Rect rect;

    // Where the body is DRAWN before the pose changes, for anim_smooth_settle at the bottom.
    Rect drawnBefore;
    bool settle = anim_get_smooth_motion() && obj != NULL && obj->tile != -1 && obj->elevation == map_elevation;
    if (settle) {
        obj_bound(obj, &drawnBefore);
    }

    obj_set_rotation(obj, rotation, &rect);

    int x = 0;
    int y = 0;

    // ⚠️ FREE MOVEMENT: THE DUDE STANDS WHERE HE STOOD, NOT ON THE TILE CENTRE. Every animation of
    // his ends here (a weapon or hand swap, a reload, an injection, a pickup), and re-centring threw
    // him up to ~22 px across the floor at the end of each one. The take-out alignment below is the
    // tile-centre convention and is skipped with it -- rt_fm_release stands him the same way.
    int restX = 0;
    int restY = 0;
    bool rest = obj == obj_dude && combat_rt_dude_rest_offset(&restX, &restY);
    if (rest) {
        if (obj->x != restX || obj->y != restY) {
            trace_event(TRACE_FLOW, "dude-rest", "anim=%d x=%d,%d -> %d,%d", FID_ANIM_TYPE(obj->fid),
                obj->x, obj->y, restX, restY);
        }
        x = restX;
        y = restY;
    }

    int weaponAnimationCode = (obj->fid & 0xF000) >> 12;
    if (weaponAnimationCode != 0 && !rest) {
        if (fid == -1) {
            int takeOutFid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, ANIM_TAKE_OUT, weaponAnimationCode, obj->rotation + 1);
            CacheEntry* takeOutFrmHandle;
            Art* takeOutFrm = art_ptr_lock(takeOutFid, &takeOutFrmHandle);
            if (takeOutFrm != NULL) {
                int frameCount = art_frame_max_frame(takeOutFrm);
                for (int frame = 0; frame < frameCount; frame++) {
                    int offsetX;
                    int offsetY;
                    art_frame_hot(takeOutFrm, frame, obj->rotation, &offsetX, &offsetY);
                    x += offsetX;
                    y += offsetY;
                }
                art_ptr_unlock(takeOutFrmHandle);

                CacheEntry* standFrmHandle;
                int standFid = art_id(FID_TYPE(obj->fid), obj->fid & 0xFFF, ANIM_STAND, 0, obj->rotation + 1);
                Art* standFrm = art_ptr_lock(standFid, &standFrmHandle);
                if (standFrm != NULL) {
                    int offsetX;
                    int offsetY;
                    if (art_frame_offset(standFrm, obj->rotation, &offsetX, &offsetY) == 0) {
                        x += offsetX;
                        y += offsetY;
                    }
                    art_ptr_unlock(standFrmHandle);
                }
            }
        }
    }

    if (fid == -1) {
        int anim;
        if (FID_ANIM_TYPE(obj->fid) == ANIM_FIRE_DANCE) {
            anim = ANIM_FIRE_DANCE;
        } else {
            anim = ANIM_STAND;
        }
        fid = art_id(FID_TYPE(obj->fid), (obj->fid & 0xFFF), anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    }

    Rect temp;
    obj_change_fid(obj, fid, &temp);
    rect_min_bound(&rect, &temp, &rect);

    obj_move_to_tile(obj, obj->tile, obj->elevation, &temp);
    rect_min_bound(&rect, &temp, &rect);

    obj_set_frame(obj, 0, &temp);
    rect_min_bound(&rect, &temp, &rect);

    obj_offset(obj, x, y, &temp);
    rect_min_bound(&rect, &temp, &rect);

    tile_refresh_rect(&rect, obj->elevation);

    if (settle) {
        anim_smooth_settle(obj, &drawnBefore);
    }
}

// 0x417EAC
void dude_standup(Object* a1)
{
    register_begin(ANIMATION_REQUEST_RESERVED);

    int anim;
    if (FID_ANIM_TYPE(a1->fid) == ANIM_FALL_BACK) {
        anim = ANIM_BACK_TO_STANDING;
    } else {
        anim = ANIM_PRONE_TO_STANDING;
    }

    register_object_animate(a1, anim, 0);
    register_end();
    a1->data.critter.combat.results &= ~DAM_KNOCKED_DOWN;
}

// 0x417EF0
static int anim_turn_towards(Object* obj, int delta, int animationSequenceIndex)
{
    if (!critter_is_prone(obj)) {
        int rotation = obj->rotation + delta;
        if (rotation >= ROTATION_COUNT) {
            rotation = ROTATION_NE;
        } else if (rotation < 0) {
            rotation = ROTATION_NW;
        }

        dude_stand(obj, rotation, -1);
    }

    anim_set_continue(animationSequenceIndex, 0);

    return 0;
}

// 0x417F64
int anim_hide(Object* object, int animationSequenceIndex)
{
    Rect rect;

    if (obj_turn_off(object, &rect) == 0) {
        tile_refresh_rect(&rect, object->elevation);
    }

    if (animationSequenceIndex != -1) {
        anim_set_continue(animationSequenceIndex, 0);
    }

    return 0;
}

// 0x417F98
int anim_change_fid(Object* obj, int animationSequenceIndex, int fid)
{
    Rect rect;
    Rect v7;

    if (FID_ANIM_TYPE(fid)) {
        obj_change_fid(obj, fid, &rect);
        obj_set_frame(obj, 0, &v7);
        rect_min_bound(&rect, &v7, &rect);
        tile_refresh_rect(&rect, obj->elevation);
    } else {
        dude_stand(obj, obj->rotation, fid);
    }

    anim_set_continue(animationSequenceIndex, 0);

    return 0;
}

// 0x418004
void anim_stop()
{
    int index;

    anim_in_anim_stop = true;
    curr_anim_set = -1;

    for (index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY; index++) {
        anim_set_end(index);
    }

    // Everything stops standing up -- including a body held in its stride by a leg that ended a
    // moment ago (a save lands here mid-chase).
    anim_deferred_stands(true);

    anim_in_anim_stop = false;
    curr_sad = 0;
}

// ⚠️ DECORATIVE LOOPS SURVIVE A SAVE AND A CONVERSATION (2026-10-14, "hele netočí se billboard";
// first reported as "gizmovo bilboard se první netočí a po odchodu a příchodu k němu zase jo").
// Gizmo's sign and every other ANIMFRVR.INT prop loops on an ANIMATE_FOREVER sequence started at
// map_enter. anim_stop() ends every sequence, and two everyday things call it: saving
// (map_save_in_game) and opening a conversation (scr_dialogue_init). Our autosave on entering a NEW
// place is exactly the first visit -- so the sign stopped the moment you arrived, and a return visit
// (no autosave) kept it turning. Vanilla only got it back through ANIMFRVR's map_update_p_proc, at
// most every 30 s and only while combat_is_initialized is 0, which realtime's warm combat bit often
// is not. So the loops running before such a stop are noted here and put back after it. A body that
// is playing something by then is left alone.
int anim_forever_snapshot(Object** owners, int* anims, int max)
{
    int count = 0;
    for (int index = 0; index < ANIMATION_SEQUENCE_LIST_CAPACITY && count < max; index++) {
        AnimationSequence* sequence = &(anim_set[index]);
        if (sequence->field_0 == -1000) {
            continue;
        }
        for (int at = 0; at < sequence->length && count < max; at++) {
            AnimationDescription* description = &(sequence->animations[at]);
            if (description->kind == ANIM_KIND_ANIMATE_FOREVER && description->owner != NULL) {
                owners[count] = description->owner;
                anims[count] = description->anim;
                count++;
            }
        }
    }
    return count;
}

void anim_forever_restore(Object** owners, int* anims, int count)
{
    for (int index = 0; index < count; index++) {
        Object* owner = owners[index];
        if (owner == NULL || owner->tile == -1 || anim_busy(owner) != 0) {
            continue;
        }
        if (register_begin(ANIMATION_REQUEST_UNRESERVED) == -1) {
            continue;
        }
        // On a refusal register_object_animate_forever closes the registration itself.
        if (register_object_animate_forever(owner, anims[index], -1) == 0) {
            register_end();
        }
    }
    if (count > 0) {
        trace_event(TRACE_ANIM, "forever-restore", "loops=%d", count);
    }
}

// 0x418040
static int check_gravity(int tile, int elevation)
{
    for (; elevation > 0; elevation--) {
        int x;
        int y;
        tile_coord(tile, &x, &y, elevation);

        int squareTile = square_num(x + 2, y + 8, elevation);
        int fid = art_id(OBJ_TYPE_TILE, square[elevation]->field_0[squareTile] & 0xFFF, 0, 0, 0);
        if (fid != art_id(OBJ_TYPE_TILE, 1, 0, 0, 0)) {
            break;
        }
    }
    return elevation;
}

// 0x4180CC
// Global animation-speed multiplier applied to ticks-per-frame (compute_tpf). tpf is a per-frame
// DURATION, so this is inverted: 1.0 = unscaled, LOWER = faster (0.8 plays ~25% quicker).
//
// Set ONCE at startup from the cfg `anim_speed` preference (anim_init) and never varied at
// runtime -- anim_set_speed_scale has that one call site. Applies to every NON-attack animation
// (movement, idle, hit-reacts, knockdown, standup, death), symmetrically for the dude and NPCs.
// ATTACK animations never see this: they take the earlier branch in compute_tpf and are driven by
// the attacker's SPECIAL instead (melee=STR, ranged=PE, throw=AGI, via anim_attack_stat_factor).
static float anim_speed_scale = 1.0f;

void anim_set_speed_scale(float scale)
{
    anim_speed_scale = scale;
}

// MOVEMENT-only speed, applied in compute_tpf on top of anim_speed_scale and expressed as a SPEED
// (1.0 = stock, 1.5 = half again as fast) rather than a duration, so the cfg number reads the way
// the request does. Only WALK and RUN see it -- idle, reactions, knockdown and death keep whatever
// anim_speed says. Symmetric for the dude and NPCs, which is what stops it from silently becoming a
// kiting buff; see the note at the call site.
static float move_speed_scale = 1.0f;

void anim_set_move_speed_scale(float scale)
{
    move_speed_scale = scale;
}

// Per-point AGILITY scaling of the weapon put-away / take-out animations (realtime only).
// Duration multiplier = 1 - (AGI - 5) * this, SYMMETRIC -- see the note at the call site in
// compute_tpf for why this one must not get the asymmetric treatment the attack anims have.
// 0 = off.
static float anim_weapon_swap_stat_factor = 0.0f;

void anim_set_weapon_swap_stat_factor(float factor)
{
    anim_weapon_swap_stat_factor = factor;
}

// RELOAD ANIMATION (2026-10-04, MODS "Reload animation", cfg [preferences] reload_anim, 0 = off =
// the instant vanilla reload).
//
// Fallout has no reload animation -- but every weapon's STAND art is a 9..18-frame loop of the dude
// handling that weapon (16 frames for the SMG, 15 for the shotgun), and the engine plays it as his
// random idle fidget. That is the "he reloads his gun and I can't move" the player kept seeing. With
// this on, that loop IS the reload: the reload button plays it once and the magazine fills at its
// end (gamepad_ui.cc), and the random idle is switched off for the dude, so the animation only ever
// means one thing.
static int anim_reload_anim = -1;

bool anim_get_reload_anim()
{
    if (anim_reload_anim < 0) {
        int v = 1; // default ON since the release (his setting)
        config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "reload_anim", &v);
        anim_reload_anim = v != 0 ? 1 : 0;
    }
    return anim_reload_anim != 0;
}

void anim_set_reload_anim(bool on)
{
    anim_reload_anim = on ? 1 : 0;
}

// The critter whose reload is being registered right now, NULL otherwise. Set around the
// register_end that starts its STAND animation, so compute_tpf -- which runs right there,
// synchronously, as the first description begins -- times THAT animation by the critter's Agility.
// A plain "is it a STAND" test cannot tell a reload from a standing frame or somebody's idle.
//
// ⚠️ ANY CRITTER, not just the dude (2026-10-04, "budou všichni přebíjet?"): NPCs reload through the
// same loop when the MODS row is on (combat_rt.cc rt_npc_start_reload_anim), so a slow reload is a
// cost everyone pays, never one only the player does.
static Object* anim_reload_timing_obj = NULL;

void anim_set_reload_timing(Object* critter)
{
    anim_reload_timing_obj = critter;
}

// The critter whose DRUG INJECTION (ANIM_MAGIC_HANDS_MIDDLE) is being registered or measured right
// now, NULL otherwise -- the same kind of bracket as anim_set_reload_timing. compute_tpf times that
// animation by the average of Agility, Intelligence and Perception (2026-10-06); outside the bracket
// the same animation (picking things up, using objects) keeps its plain timing.
static Object* anim_injection_timing_obj = NULL;

void anim_set_injection_timing(Object* critter)
{
    anim_injection_timing_obj = critter;
}

// START AN ANIMATION PART WAY IN (2026-10-19, the held-LB heal chain: "kus animace píchnutí znova").
// A bracket like the timing ones: set around the register_end that starts the animation, and the
// ANIMATE description that begins for this critter inside it starts at that frame instead of 0 --
// object_animate's first tick steps the body through the skipped frames exactly as the forward ticks
// would (frame + its hot-spot offset), so the pose lands where the full animation would have had it.
// A call registered after it counts its delay from there. Out of the bracket: (NULL, 0).
void anim_set_start_frame(Object* critter, int frame)
{
    anim_start_frame_obj = critter;
    anim_start_frame_value = frame;
}

// THE RELOAD ITSELF, registered inside an open register_begin..register_end: the weapon's STAND
// loop -- EXCEPT FOR PISTOLS (2026-10-06, "pistole 10mm má jen otírání hlavy jako reload?"). Every
// pistol shares one animation set, and its STAND loop is the dude wiping his forehead, nothing to do
// with the gun. So a pistol holsters and draws again instead -- the swap's own put-away + take-out,
// the same two descriptions intface_change_fid_animate registers -- which reads as reaching to the
// belt for a magazine. Agility times both through the swap curve in compute_tpf; dude_stand at the
// end of the sequence rebuilds the pistol stand pose from the take-out art, exactly as after a swap.
int register_object_reload(Object* critter)
{
    int weaponCode = (critter->fid & 0xF000) >> 12;
    if (weaponCode == WEAPON_ANIMATION_PISTOL) {
        const char* sfx = gsnd_build_character_sfx_name(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(critter, sfx, 0);
        if (register_object_animate(critter, ANIM_PUT_AWAY, 0) == -1) {
            return -1;
        }
        return register_object_take_out(critter, weaponCode, -1);
    }

    return register_object_animate(critter, ANIM_STAND, 0);
}

// Is this body still playing its reload? STAND for most weapons, PUT_AWAY then TAKE_OUT for a pistol
// (register_object_reload). Anything else means the reload was replaced -- cut.
bool anim_is_reload_pose(Object* critter)
{
    int anim = FID_ANIM_TYPE(critter->fid);
    return anim_busy(critter) != 0
        && (anim == ANIM_STAND || anim == ANIM_PUT_AWAY || anim == ANIM_TAKE_OUT);
}

// --- Realtime-combat per-direction NPC walk-speed normalization. ---
//
// Some critter walk .frm assets advance the sprite a different number of pixels
// per frame depending on facing, so at a fixed ticks-per-frame the critter
// crosses a tile faster in some directions than others (the radscorpion's NW art
// is ~2.5x faster than its E/W art). In turn-based combat this is invisible, but
// under realtime combat_rt it lets a critter outrun the player on one diagonal.
// We normalize every NPC walk direction to that critter's OWN slowest direction
// so it walks at a single, even, slow cadence -- realtime only; the turn-based
// path and the player are left untouched.
//
// The per-direction "frames to cross one tile" is derived from the art at runtime
// (mirroring object_move's pixel-accumulation crossing test against off_tile), so
// this auto-corrects ANY critter with the same art asymmetry, not just
// radscorpions. The 6-direction result is cached per walk-art set.

struct WalkSpeedProfile {
    int key; // walk-art identity (type/frmId/weapon), rotation-independent; -1 = empty
    float framesPerTile[ROTATION_COUNT];
    float maxFramesPerTile;
};

#define WALK_PROFILE_CACHE_SIZE 32
static WalkSpeedProfile walk_profile_cache[WALK_PROFILE_CACHE_SIZE];
static int walk_profile_cache_next = 0;
static bool walk_profile_cache_inited = false;

// Frames the walk art takes to advance one tile in `rotation`, by replaying
// object_move's per-frame hot-offset accumulation against off_tile. Returns 0 if
// the art is missing or yields no net movement (nothing to normalize).
static float walk_frames_per_tile(Object* object, int rotation, int anim)
{
    int fid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, anim, (object->fid & 0xF000) >> 12, rotation + 1);
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art == NULL) {
        return 0.0f;
    }

    int frameCount = art_frame_max_frame(art);
    int targetX = off_tile[0][rotation];
    int targetY = off_tile[1][rotation];

    int ax = 0;
    int ay = 0;
    int crossings = 0;
    int frames = 0;
    int maxFrames = frameCount > 0 ? frameCount * 16 : 0;

    while (frames < maxFrames && crossings < 8) {
        int hx = 0;
        int hy = 0;
        art_frame_hot(art, frames % frameCount, rotation, &hx, &hy);
        ax += hx;
        ay += hy;
        frames++;

        if ((targetX > 0 && targetX <= ax) || (targetX < 0 && targetX >= ax)
            || (targetY > 0 && targetY <= ay) || (targetY < 0 && targetY >= ay)) {
            ax -= targetX;
            ay -= targetY;
            crossings++;
        }
    }

    art_ptr_unlock(handle);

    if (crossings == 0) {
        return 0.0f;
    }

    return (float)frames / (float)crossings;
}

// `anim` selects which locomotion art is profiled: ANIM_WALK or ANIM_RUNNING. It is part of the
// cache key, so a critter's walk and run profiles coexist.
static WalkSpeedProfile* walk_profile_get(Object* object, int anim)
{
    if (!walk_profile_cache_inited) {
        for (int i = 0; i < WALK_PROFILE_CACHE_SIZE; i++) {
            walk_profile_cache[i].key = -1;
        }
        walk_profile_cache_inited = true;
    }

    int type = FID_TYPE(object->fid);
    int frmId = object->fid & 0xFFF;
    int weapon = (object->fid & 0xF000) >> 12;
    int key = (anim << 26) | (type << 22) | (weapon << 16) | frmId;

    for (int i = 0; i < WALK_PROFILE_CACHE_SIZE; i++) {
        if (walk_profile_cache[i].key == key) {
            return &walk_profile_cache[i];
        }
    }

    // Miss: derive the 6-direction profile from the art and cache it (ring buffer).
    WalkSpeedProfile* p = &walk_profile_cache[walk_profile_cache_next];
    walk_profile_cache_next = (walk_profile_cache_next + 1) % WALK_PROFILE_CACHE_SIZE;
    p->key = key;
    p->maxFramesPerTile = 0.0f;
    for (int d = 0; d < ROTATION_COUNT; d++) {
        float fpt = walk_frames_per_tile(object, d, anim);
        p->framesPerTile[d] = fpt;
        if (fpt > p->maxFramesPerTile) {
            p->maxFramesPerTile = fpt;
        }
    }
    return p;
}

// Multiplier (>= 1.0) that slows an NPC's walk step in `rotation` so that one tile
// takes the SAME wall-clock time as the PLAYER's walk -- not just the critter's own
// slowest facing. Equalizing only a critter's own directions (the old behavior) left
// a critter whose walk art is uniformly fast (no per-facing asymmetry) untouched, so
// in realtime combat it out-walked the player chasing him. Now we match the dude's
// tile-crossing time directly: no enemy can out-walk you. 1.0 = no change (and we only
// ever SLOW, never speed a slow enemy up -- see the >1.0 guard at the call site).
//
// Tile-crossing time ~ framesPerTile * ticksPerFrame, where ticksPerFrame (compute_tpf)
// folds in the art fps, the realtime anim_speed lever, and the combat-speed bonus that
// vanilla adds to NPC (but not player) walks -- the very thing that made enemies fast.
// Taking the ratio against the dude cancels the common factors and corrects the rest.
// TEMP walk-speed diagnosis: logs one line per (object,rotation) change to rt_walk.log
// next to the exe (truncate per run). STRIP once the radscorpion NW speed is confirmed.
static void walk_log(const char* fmt, ...)
{
#ifdef FALLOUT_RELEASE
    // Not in the public build: it writes rt_walk.log next to the exe on every run.
    (void)fmt;
    return;
#endif
    static FILE* f = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        char* base = SDL_GetBasePath();
        char path[1024];
        snprintf(path, sizeof(path), "%srt_walk.log", base != NULL ? base : "");
        if (base != NULL) {
            SDL_free(base);
        }
        f = fopen(path, "w");
    }
    if (f == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

// Crippled legs, as a tile-crossing-time multiplier (1.0 = unhurt). Realtime replacement for the
// vanilla rule, which is pure ACTION POINTS: critter_compute_ap_from_distance charges 4x per tile
// for one bad leg and 8x for two. That rule is dead here -- the per-tile deduction it feeds is
// explicitly skipped under combat_rt (see the guard in object_animate), because charging movement
// AP would cut every script-driven walk to one tile. So until now a crippled leg did NOTHING in
// realtime: you could shoot both of someone's legs off and they kept pace with you.
//
// ⚠️ NOT 4x / 8x. Translating the vanilla numbers straight across would be near-immobility --
// in turn-based they cost you ground per TURN, with the fight politely waiting; in realtime they
// would be four to eight times the wall-clock seconds to cross one tile, which is a stunlock
// wearing a different name. These are the numbers that make it a real wound you can still fight
// through, and both are cfg-tunable.
//
// Applies to the DUDE as well as NPCs, deliberately: he is the one who mostly does the crippling,
// and a consequence he can never suffer is not a mechanic, it is a cheat code.
// TRAVEL SPEED: how fast the player's side moves WHEN NOBODY IS FIGHTING.
//
// The complaint this answers is "walking bores me, I just hold run, and it is still meh" -- and the
// numbers back it up. Movement had exactly one lever for the player and it only touched the RUN:
// run_speed_scale normalises the art and then applies Agility (run_agility_factor 0.04, so AGI 10 =
// 0.80). The WALK has none at all -- walk_dir_slowdown deliberately never rescales the dude, so his
// walk is raw vanilla art times the global anim_speed, and Agility buys him nothing on foot. Crossing
// an empty map is therefore exactly as slow as it was in 1997, which is what he actually noticed.
//
// ⚠️ ONLY OUT OF COMBAT, and that boundary is the whole design. Speed IS the kiting exploit: the
// hostile runners are normalised against the dude's own run cadence (run_speed_scale's refCross
// takes the SLOWER of dude/own), so making the dude faster does NOT drag them along -- he simply
// outruns everything on the map, which is the hole the AP-drain on running was built to close.
// Out of combat none of that exists: running is already free, nothing is chasing, and the only
// thing the speed changes is how long the boring part lasts. So this is free to crank, and the
// combat pacing that was play-tested into shape is untouched.
//
// Applies to the dude AND his party -- companions follow him, so leaving them behind would just
// trade a slow walk for a string of teammates teleporting to catch up.
//
// ⚠️ DEFAULT IS 1.0 = OFF. His call: a normal player should get the game as it was authored, and
// this is an accessibility/comfort knob you go and switch ON. So the shipped behaviour is vanilla
// speed and the cfg (or the Preferences row) is what opts in.
//
// Lever: cfg [preferences] travel_speed (1.0 = off, 0.7 ~ 40% faster, floor 0.35).
static float travel_speed_scale(Object* object)
{
    if (object == NULL || FID_TYPE(object->fid) != OBJ_TYPE_CRITTER) {
        return 1.0f;
    }

    if (object != obj_dude && !isPartyMember(object)) {
        return 1.0f;
    }

    // Any live fight, whoever it is between, puts everyone back on combat cadence. Deliberately the
    // BROAD test (the warm combat bit) rather than "is something after ME": a bystander sprinting
    // through someone else's firefight at travel speed would read as a bug, and being conservative
    // here can only ever cost speed, never balance.
    if (combat_rt_enabled() ? combat_rt_active() : (isInCombat() != 0)) {
        return 1.0f;
    }

    return anim_get_travel_speed();
}

// The live value, -1 until the first read pulls it out of the cfg. Kept at file scope rather than
// inside travel_speed_scale so the MODS screen can move it without a re-init.
static float anim_travel_speed = -1.0f;

// The travel-speed factor as it is ACTUALLY being applied to the dude right now -- combat test
// and all. ⚠️ Callers outside this file must ask THIS, not anim_get_travel_speed: the raw setting
// says what you chose, this says what the legs are doing, and the game clock has to follow the legs
// (scr_process_all). Reading the setting instead would speed time up in the combat cases where the
// movement itself is deliberately not sped up.
float anim_travel_speed_in_effect()
{
    return travel_speed_scale(obj_dude);
}

float anim_get_travel_speed()
{
    if (anim_travel_speed < 0.0f) {
        double v = 1.0;
        config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "travel_speed", &v);
        anim_travel_speed = v < 0.35 ? 0.35f : (v > 1.0 ? 1.0f : (float)v);
    }
    return anim_travel_speed;
}

// MODS page: change the travel speed while the game runs. Same clamp as the loader, so the screen
// cannot feed in a value the cfg path would have rejected.
// Overrides how much faster than its art EVERY created projectile flies (the per-kind table above
// otherwise). Only the self-played tests use it (scenario "projectile_speedup"), to measure dodging.
void anim_set_projectile_speedup(float speedup)
{
    anim_projectile_speedup = speedup > 0.1f ? speedup : 0.1f;
}

void anim_set_travel_speed(float scale)
{
    anim_travel_speed = scale < 0.35f ? 0.35f : (scale > 1.0f ? 1.0f : scale);
}

static float crippled_move_scale(Object* object)
{
    if (object == NULL || FID_TYPE(object->fid) != OBJ_TYPE_CRITTER) {
        return 1.0f;
    }

    static float crip_one = -1.0f;
    static float crip_both = -1.0f;
    if (crip_one < 0.0f) {
        double one = 1.6;
        double both = 2.5;
        config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRIP_LEG_ONE_KEY, &one);
        config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRIP_LEG_BOTH_KEY, &both);
        // Floored at 1.0: this may only ever SLOW. A value below 1 would turn a shot-off leg into
        // a speed boost, which is the kind of sign error a cfg typo makes very easy.
        crip_one = one < 1.0 ? 1.0f : (float)one;
        crip_both = both < 1.0 ? 1.0f : (float)both;
        if (crip_both < crip_one) {
            crip_both = crip_one; // two bad legs can never be better than one
        }
    }

    int flags = object->data.critter.combat.results;
    if ((flags & DAM_CRIP_LEG_LEFT) != 0 && (flags & DAM_CRIP_LEG_RIGHT) != 0) {
        return crip_both;
    }
    if ((flags & DAM_CRIP_LEG_ANY) != 0) {
        return crip_one;
    }
    return 1.0f;
}

static float walk_dir_slowdown(Object* object, int rotation)
{
    if (rotation < 0 || rotation >= ROTATION_COUNT) {
        return 1.0f;
    }
    if (object == obj_dude || obj_dude == NULL) {
        return 1.0f; // never rescale the player against himself
    }

    WalkSpeedProfile* np = walk_profile_get(object, ANIM_WALK);
    float nfpt = np->framesPerTile[rotation];
    if (nfpt <= 0.0f) {
        return 1.0f;
    }

    // Player reference: his SLOWEST facing = the base walk-speed feel to match.
    WalkSpeedProfile* dp = walk_profile_get(obj_dude, ANIM_WALK);
    float dfpt = dp->maxFramesPerTile;
    if (dfpt <= 0.0f) {
        return 1.0f;
    }

    int npcFid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, ANIM_WALK,
        (object->fid & 0xF000) >> 12, rotation + 1);
    int dudeFid = art_id(FID_TYPE(obj_dude->fid), obj_dude->fid & 0xFFF, ANIM_WALK,
        (obj_dude->fid & 0xF000) >> 12, rotation + 1);
    float npcTpf = (float)compute_tpf(object, npcFid);
    float dudeTpf = (float)compute_tpf(obj_dude, dudeFid);
    if (npcTpf <= 0.0f) {
        return 1.0f;
    }

    float npcCross = nfpt * npcTpf;
    float dudeCross = dfpt * dudeTpf;
    if (npcCross <= 0.0f) {
        return 1.0f;
    }

    // Reference tile-crossing time to slow THIS facing toward: the SLOWER of the player's
    // slowest facing and THIS critter's OWN slowest facing. The dude term keeps a uniformly-
    // fast critter from out-walking the player; the own-slowest term evens the critter's own
    // directions so no single facing (e.g. a humanoid's SE) darts ahead of its other facings
    // -- the per-direction asymmetry that the player-only reference left uncorrected when a
    // critter's facings straddle the dude's speed (some faster, some slower). We still only
    // ever SLOW (the >1.0 guard at the call site), so the slower reference is always safe.
    float ownCross = np->maxFramesPerTile * npcTpf;
    float refCross = dudeCross > ownCross ? dudeCross : ownCross;

    // USER LEVER for "how fast NPCs walk": cfg [preferences] key rt_npc_walk_speed. It scales the
    // NPC's target tile-crossing time relative to the player: 1.0 = match the player (default),
    // 1.3 = 30% slower (so you can outrun/kite them), etc. Read once and cached. Clamped to >= 1.0
    // because the >1.0 guard at the call site only ever SLOWS an NPC (it never speeds a slow one
    // up), and capped at 4.0 for sanity. Set it in fallout.cfg to tune enemy walk speed live.
    static float rt_npc_walk_speed = -1.0f;
    if (rt_npc_walk_speed < 0.0f) {
        double v = 1.0;
        config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "rt_npc_walk_speed", &v);
        rt_npc_walk_speed = v < 1.0 ? 1.0f : (v > 4.0 ? 4.0f : (float)v);
    }
    refCross *= rt_npc_walk_speed;

    float scale = refCross / npcCross;

    // TEMP: log each new (object,rotation) so we can see whether NW gets normalized.
    static int wl_pid = -1;
    static int wl_rot = -1;
    if (object->pid != wl_pid || rotation != wl_rot) {
        wl_pid = object->pid;
        wl_rot = rotation;
        // crip= is reported here but APPLIED at the call site, after this function returns -- so
        // finalTpf below does NOT include it. Multiply the two to get the real tile time. Kept
        // this way because moving the limp in here would let the normalisation cancel it (see
        // the long note at the call site); reporting it is enough to make the log answerable.
        walk_log("pid=%d rot=%d | nfpt=%.1f npcCross=%.0f | dudeCross=%.0f ownMaxFpt=%.1f ownCross=%.0f refCross=%.0f | scale=%.2f finalTpf=%.1f crip=%.2f",
            object->pid, rotation, nfpt, npcCross, dudeCross, np->maxFramesPerTile, ownCross,
            refCross, scale, npcTpf * (scale > 1.0f ? scale : 1.0f), crippled_move_scale(object));
    }

    return scale;
}

// Multiplier on a RUN step's ticks-per-frame: < 1.0 runs faster, > 1.0 slower. Applies to EVERY
// runner including the dude (unlike walk_dir_slowdown, which never rescales the player).
//
// WHY THIS EXISTS: until NPCs started running, nothing normalized ANIM_RUNNING -- walk_dir_slowdown
// and the smoothing are both gated on ANIM_WALK. So run speed was decided purely by how many frames
// the artist happened to draw per tile in that critter's run art: measured from a trace, most
// humanoids ran ~5.1 tiles/s (matching the dude) while one art managed ~8.5, i.e. 70% faster than
// the player for no reason the player could see or counter. Two layers fix that:
//
//   1. ART NORMALIZATION -- take the dude's run cadence (his slowest facing) as the neutral
//      baseline and even out each critter's own facings too, exactly like the walk path. This
//      strips the accidental variance without touching anything intentional.
//   2. AGILITY -- then scale that baseline by the runner's AGI, so speed says something about the
//      CHARACTER: `mult = 1 - (AGI - 5) * factor`, the same idiom as the SPECIAL-driven attack
//      animation speed (anim_attack_stat_factor). AGI 5 = baseline, higher = faster, lower =
//      slower. Endurance already governs how LONG you can sprint (the AP drain), so the pair reads
//      as AGI = speed, EN = stamina, with no overlap.
//
// Lever: cfg [preferences] run_agility_factor (default 0.04 => roughly +-20% across AGI 1..10).
// Set it to 0 to keep the art normalization but drop the stat influence entirely.
static float run_speed_scale(Object* object, int rotation)
{
    if (rotation < 0 || rotation >= ROTATION_COUNT || object == NULL || obj_dude == NULL) {
        return 1.0f;
    }

    WalkSpeedProfile* np = walk_profile_get(object, ANIM_RUNNING);
    float nfpt = np->framesPerTile[rotation];
    if (nfpt <= 0.0f) {
        return 1.0f; // no run art / no net movement -- nothing to scale
    }

    int npcFid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, ANIM_RUNNING,
        (object->fid & 0xF000) >> 12, rotation + 1);
    float npcTpf = (float)compute_tpf(object, npcFid);
    if (npcTpf <= 0.0f) {
        return 1.0f;
    }

    float npcCross = nfpt * npcTpf; // this facing's tile-crossing time
    if (npcCross <= 0.0f) {
        return 1.0f;
    }

    // Baseline: the slower of {the dude's slowest run facing, this critter's own slowest}. The
    // dude term stops a fast art from outrunning the player for free; the own term evens the
    // critter's own directions. For the dude himself the two are the same profile, so he simply
    // gets his own facings evened -- then the stat below moves him as a whole.
    WalkSpeedProfile* dp = walk_profile_get(obj_dude, ANIM_RUNNING);
    int dudeFid = art_id(FID_TYPE(obj_dude->fid), obj_dude->fid & 0xFFF, ANIM_RUNNING,
        (obj_dude->fid & 0xF000) >> 12, rotation + 1);
    float dudeTpf = (float)compute_tpf(obj_dude, dudeFid);
    float dudeCross = dp->maxFramesPerTile * dudeTpf;
    float ownCross = np->maxFramesPerTile * npcTpf;
    float refCross = dudeCross > ownCross ? dudeCross : ownCross;
    if (refCross <= 0.0f) {
        return 1.0f;
    }

    static float run_agility_factor = -1.0f;
    if (run_agility_factor < 0.0f) {
        double v = 0.04;
        config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "run_agility_factor", &v);
        run_agility_factor = v < 0.0 ? 0.0f : (v > 0.15 ? 0.15f : (float)v);
    }

    float statMult = 1.0f - (stat_level(object, STAT_AGILITY) - 5) * run_agility_factor;
    if (statMult < 0.6f) statMult = 0.6f;
    if (statMult > 1.6f) statMult = 1.6f;

    float scale = refCross * statMult / npcCross;
    if (scale < 0.5f) scale = 0.5f; // sanity rails; the stat clamp above is the real limit
    if (scale > 4.0f) scale = 4.0f;

    // TEMP DIAG ("who runs how fast"): one line per (pid, rotation) change. Reading run speed out
    // of the STATE heartbeat is hopeless -- runs are short bursts, so a 1/s sample gives 1-2 data
    // points per critter and the fast arts may not even appear. This prints the ACTUAL numbers the
    // scaler used: crossMs is the tile-crossing time BEFORE scaling and finalMs after, so
    // finalMs is directly comparable BETWEEN critters (lower = faster). STRIP once run speed is
    // confirmed.
    static int rl_pid = -1;
    static int rl_rot = -1;
    if (object->pid != rl_pid || rotation != rl_rot) {
        rl_pid = object->pid;
        rl_rot = rotation;
        trace_event(TRACE_ANIM, "run-speed",
            "pid=%d%s rot=%d agi=%d fpt=%.1f crossMs=%.0f dudeCross=%.0f ownCross=%.0f statMult=%.2f scale=%.2f finalMs=%.0f",
            object->pid, object == obj_dude ? " DUDE" : "", rotation,
            stat_level(object, STAT_AGILITY), nfpt, npcCross, dudeCross, ownCross,
            statMult, scale, npcCross * scale);
    }

    return scale;
}

// --- Realtime walk smoothing (variant A), per-object for all critters. ---
//
// Each walking critter (the dude AND chasing NPCs) gets a linear within-tile ramp so
// obj->x/y advance at constant velocity across each tile instead of following the art's
// uneven per-frame hot offsets. Those raw offsets make some critters lurch in big jumps on
// certain facings (the radscorpion's NW art front-loads its motion), which reads as "fast"
// even when the per-tile time is normal. A small pointer-keyed cache holds the few
// concurrent walkers; a slot self-resets on a new tile / direction, so a slot reused after
// its object was freed can't carry stale state.
struct WalkSmoothState {
    Object* obj;
    int rot;
    float fracX;
    float fracY;

    // How far the ramp advances per frame. Stored rather than recomputed because a step may start
    // from somewhere other than the tile centre (see the seed below), and then it is not delta / n.
    float stepX;
    float stepY;

    // Where the body already stands when a walk STARTS, handed over by object_move before it
    // re-centres the object. The first step then ramps FROM there instead of from the tile centre,
    // which is what lets a walk begin after a stop that left the body between two tiles without
    // teleporting it onto the tile first. Consumed by the next reset.
    float seedX;
    float seedY;
    bool hasSeed;

    // Set on the frame the ramp finished a tile, so the NEXT call restarts it. ⚠️ Inferring that
    // from obj->x/y being back at zero (as this once did) stops working the moment a step can start
    // anywhere other than a tile centre -- the ramp would run on into a second tile's worth of
    // offset.
    bool crossed;
};
#define WALK_SMOOTH_SLOTS 16
static WalkSmoothState walk_smooth_state[WALK_SMOOTH_SLOTS];

static WalkSmoothState* walk_smooth_slot(Object* object)
{
    for (int i = 0; i < WALK_SMOOTH_SLOTS; i++) {
        if (walk_smooth_state[i].obj == object) {
            return &walk_smooth_state[i];
        }
    }
    for (int i = 0; i < WALK_SMOOTH_SLOTS; i++) {
        if (walk_smooth_state[i].obj == NULL) {
            WalkSmoothState* s = &walk_smooth_state[i];
            memset(s, 0, sizeof(*s));
            s->obj = object;
            s->rot = -1;
            return s;
        }
    }
    static int ev = 0; // all slots busy: evict via a rotating cursor
    WalkSmoothState* s = &walk_smooth_state[ev];
    ev = (ev + 1) % WALK_SMOOTH_SLOTS;
    memset(s, 0, sizeof(*s)); // ⚠️ a reused slot must not inherit the last object's ramp
    s->obj = object;
    s->rot = -1;
    return s;
}

// Tell the smoother where the body already is, before object_move re-centres it on its tile.
// ⚠️ obj_move_to_tile zeroes obj->x/y, so without this a walk that begins after a stop mid-tile
// would start by teleporting the body onto the tile centre.
static void walk_smooth_seed(Object* object, int x, int y)
{
    if (x == 0 && y == 0) {
        return;
    }

    WalkSmoothState* s = walk_smooth_slot(object);
    s->seedX = (float)x;
    s->seedY = (float)y;
    s->hasSeed = true;
}

// CONSTANT-VELOCITY RAMP. Overrides this frame's (frameX, frameY) walk delta so obj->x/y marches
// linearly to the tile boundary, instead of following the art's uneven per-frame hot offsets --
// those lurch on some facings (the radscorpion's NW art front-loads its motion in big jumps) and
// read as "fast" even when the per-tile time is normal. The ramp spans the art's own
// frames-per-tile, so the walk speed per tile is unchanged; only the motion WITHIN a tile is
// evened. Applied to every critter in realtime; turn-based keeps the art-driven motion.
//
// Returns 1 if this step has now covered its whole tile, 0 if not, -1 if the ramp did not run (bad
// rotation, unmeasurable art) and the caller must fall back to its own test.
//
// ⚠️ IT OWNS THE TILE-CROSSING TEST. object_move's own test asks whether obj->x/y has reached the
// tile delta -- which stops being the same question once a step can BEGIN somewhere other than the
// tile centre (the seed above). For a step that starts at the centre the answer is identical,
// because obj->x was never anything but this ramp, rounded.
static int walk_smooth_offset(Object* object, int rotation, int anim, int* frameX, int* frameY)
{
    if (rotation < 0 || rotation >= ROTATION_COUNT) {
        return -1;
    }

    float fpt = walk_frames_per_tile(object, rotation, anim);
    if (fpt < 1.0f) {
        return -1;
    }

    int n = (int)(fpt + 0.5f);
    if (n < 1) {
        n = 1;
    }

    int ox = off_tile[0][rotation];
    int oy = off_tile[1][rotation];

    WalkSmoothState* s = walk_smooth_slot(object);

    if (s->crossed || rotation != s->rot || (object->x == 0 && object->y == 0)) {
        // Where this step's ramp begins. Normally the tile centre; after a stop that left the body
        // between tiles, wherever it was left -- so the step covers the real remaining ground and
        // still lands exactly on the next tile.
        float startX = 0.0f;
        float startY = 0.0f;
        if (s->hasSeed) {
            startX = s->seedX;
            startY = s->seedY;
            s->hasSeed = false;

            // ⚠️ A seed already at or past the target would finish the step on its first frame (the
            // ramp would run backwards and the done test would fire at once). Nothing sensible can
            // be interpolated from there, so fall back to the tile centre and accept the one snap.
            if ((ox > 0 && startX >= (float)ox) || (ox < 0 && startX <= (float)ox)) {
                startX = 0.0f;
                startY = 0.0f;
            }
            if (oy != 0 && ((oy > 0 && startY >= (float)oy) || (oy < 0 && startY <= (float)oy))) {
                startX = 0.0f;
                startY = 0.0f;
            }
        }

        s->crossed = false;
        s->rot = rotation;
        s->fracX = startX;
        s->fracY = startY;
        s->stepX = ((float)ox - startX) / n;
        s->stepY = ((float)oy - startY) / n;
    }

    s->fracX += s->stepX;
    s->fracY += s->stepY;

    *frameX = (int)lroundf(s->fracX) - object->x;
    *frameY = (int)lroundf(s->fracY) - object->y;

    bool done = (ox > 0 ? s->fracX >= (float)ox : s->fracX <= (float)ox)
        || (oy != 0 && (oy > 0 ? s->fracY >= (float)oy : s->fracY <= (float)oy));

    s->crossed = done;

    return done ? 1 : 0;
}

// Extends an in-flight normal (non-straight) move animation for `obj` by
// appending `count` more steps in `rotation`, without restarting the sad. The
// walk cycle keeps advancing from its current frame because no new sad is
// created and field_20 (the current step index) is untouched -- only the path
// (rotations[]/field_1C) and destination (field_24) grow. Returns 0 on
// success, or a negative reason code:
//   -1  no sad belongs to obj
//   -2  obj's sad is already finished (field_20 == -1000)
//   -3  obj's sad is a straight move (knockback etc.), not a walk/run
//   -4  rotations[] capacity would overflow
// How many steps of obj's live walk have NOT started yet. The step in progress is excluded --
// the critter is physically between two tiles and that one can no longer be changed.
//
// WHY THIS EXISTS: steering used to mean cutting the walk and registering a new one, and
// obj_connect_to_tile zeroes obj->x/y, so every such cut SNAPPED the critter back to the centre of
// the tile he was crossing -- up to 32 px of progress thrown away, the walk cycle reset to frame 0,
// and make_path run again. That is the "it is not seamless, it is jerky when I change direction"
// report. With per-step rotations the queued step can simply be rewritten instead.
int anim_move_pending_steps(Object* obj)
{
    for (int i = 0; i < curr_sad; i++) {
        AnimationSad* sad_entry = &(sad[i]);
        if (sad_entry->obj != obj) {
            continue;
        }

        if ((sad_entry->flags & ANIM_SAD_STRAIGHT) != 0) {
            return 0;
        }

        // -2000 (not started) and -1000 (finished) are both "nothing to steer".
        if (sad_entry->field_20 < 0) {
            return 0;
        }

        int pending = sad_entry->field_1C - (sad_entry->field_20 + 1);
        return pending > 0 ? pending : 0;
    }

    return 0;
}

// Replace the not-yet-started steps of obj's live walk with `rots`, in place. `count` must equal
// anim_move_pending_steps(obj). The destination tile is recomputed (object_move re-paths to it when
// something blocks the way), and `newDest` receives it. Returns 0 on success.
//
// ⚠️ IT NEVER TOUCHES THE STEP IN PROGRESS OR THE FRAME. That is the whole point: the walk cycle
// keeps running, obj->x/y keeps its sub-tile progress, and the new heading takes effect at the next
// tile boundary -- which is the soonest a hex walk could honour it anyway.
int anim_move_repoint(Object* obj, const int* rots, int count, int* newDest)
{
    if (rots == NULL || count <= 0) {
        return -6;
    }

    for (int i = 0; i < curr_sad; i++) {
        AnimationSad* sad_entry = &(sad[i]);
        if (sad_entry->obj != obj) {
            continue;
        }

        if ((sad_entry->flags & ANIM_SAD_STRAIGHT) != 0) {
            return -3;
        }

        if (sad_entry->field_20 < 0) {
            return -2;
        }

        int start = sad_entry->field_20 + 1;
        if (sad_entry->field_1C - start != count) {
            return -5;
        }

        // Rebuild the destination FORWARD from where the object actually is, rather than rewinding
        // field_24. ⚠️ field_24 is not always the end of the rotations array -- anim_move truncates
        // field_1C to the action-point budget without touching it -- so rewinding would quietly
        // drift. obj->tile is the tile the step in progress LEAVES (object_move advances the tile
        // and the index together), so this is exact whatever field_24 held.
        int dest = tile_num_in_direction(obj->tile, sad_entry->rotations[sad_entry->field_20], 1);

        for (int j = 0; j < count; j++) {
            sad_entry->rotations[start + j] = (unsigned char)rots[j];
            dest = tile_num_in_direction(dest, rots[j], 1);
        }

        sad_entry->field_24 = dest;
        if (newDest != NULL) {
            *newDest = dest;
        }

        return 0;
    }

    return -1;
}

int anim_extend_move(Object* obj, int rotation, int count)
{
    for (int i = 0; i < curr_sad; i++) {
        AnimationSad* sad_entry = &(sad[i]);
        if (sad_entry->obj != obj) {
            continue;
        }

        if (sad_entry->field_20 == -1000) {
            return -2;
        }

        if ((sad_entry->flags & ANIM_SAD_STRAIGHT) != 0) {
            return -3;
        }

        if (sad_entry->field_1C + count > 3200) {
            return -4;
        }

        for (int j = 0; j < count; j++) {
            sad_entry->rotations[sad_entry->field_1C + j] = (unsigned char)rotation;
        }
        sad_entry->field_1C += count;
        sad_entry->field_24 = tile_num_in_direction(sad_entry->field_24, rotation, count);
        return 0;
    }

    return -1;
}

// How fast this critter's WALK or RUNNING art moves it, as the step-based mover plays it:
// tiles per second, and animation frames per second. ⚠️ THE FREE MOVER HAS TO MATCH THIS EXACTLY.
// Two movement modes that pace differently would make every number tuned against one of them wrong
// under the other -- chase distances, the AP drain per second, how far you get between two shots.
//
// Measured on the EAST facing, which is the one whose tile is a whole 32 px; the art's other
// facings differ slightly and picking one reference is what keeps a continuous heading from
// changing speed as it sweeps. The two multipliers that live at object_move's call site rather than
// inside compute_tpf (travel speed and the crippled-leg limp) are applied here for the same reason
// they are applied there: they are tile-TIME factors, so they divide the speed.
void anim_move_speed(Object* object, int anim, float* tilesPerSec, float* framesPerSec)
{
    *tilesPerSec = 0.0f;
    *framesPerSec = 0.0f;

    if (object == NULL) {
        return;
    }

    float fpt = walk_frames_per_tile(object, ROTATION_E, anim);
    if (fpt < 1.0f) {
        return;
    }

    int fid = art_id(FID_TYPE(object->fid), object->fid & 0xFFF, anim,
        (object->fid & 0xF000) >> 12, object->rotation + 1);
    float tpf = (float)compute_tpf(object, fid);
    if (tpf < 1.0f) {
        tpf = 1.0f;
    }

    tpf *= travel_speed_scale(object) * crippled_move_scale(object);
    if (tpf < 1.0f) {
        tpf = 1.0f;
    }

    *framesPerSec = 1000.0f / tpf;
    *tilesPerSec = 1000.0f / (fpt * tpf);
}

// See isQuickDraw in compute_tpf. Set around the one inven_wield whose take-out it paces: the
// take-out is the sequence's first description, so it starts -- and is timed -- inside that call.
static Object* anim_quick_draw_obj = NULL;

void anim_quick_draw_begin(Object* obj)
{
    anim_quick_draw_obj = obj;
}

void anim_quick_draw_end()
{
    anim_quick_draw_obj = NULL;
}

unsigned int compute_tpf(Object* object, int fid)
{
    int fps;

    CacheEntry* handle;
    Art* frm = art_ptr_lock(fid, &handle);
    if (frm != NULL) {
        fps = art_frame_fps(frm);
        art_ptr_unlock(handle);
    } else {
        fps = 10;
    }

    // Vanilla's Combat Speed bonus: extra walk fps in combat, for NPCs, and for the dude only with
    // "Affect player speed" ticked. NOT in realtime (2026-10-11): there the slider is a time scale
    // for the whole fight (combat_rt_time_scale, applied below), and this bonus on top made the dude
    // alone run about six times faster at slider 50.
    if (isInCombat() && !combat_rt_enabled()) {
        if (FID_ANIM_TYPE(fid) == ANIM_WALK) {
            int playerSpeedup = 0;
            config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, &playerSpeedup);

            if (object != obj_dude || playerSpeedup == 1) {
                int combatSpeed = 0;
                config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, &combatSpeed);
                fps += combatSpeed;
            }
        }
    }

    if (fps <= 0) {
        fps = 10;
    }

    unsigned int tpf = 1000 / fps;

    // Realtime combat: ATTACK animations are excluded from the global anim_speed_scale
    // and driven purely by the attacker's SPECIAL -- each attack category by its own stat:
    // melee (swing/thrust/punch/kick) by STRENGTH, ranged (fire) by PERCEPTION, throw
    // (grenade/spear/knife) by AGILITY. These anim types only occur during an attack, so
    // FID_ANIM_TYPE alone classifies them with no weapon lookup. Symmetric: object is the
    // attacker, so a high-PE enemy fires fast and a strong enemy swings fast too. This is
    // purely animation duration -- hit chance, AP cost, and damage are untouched.
    int animType = FID_ANIM_TYPE(fid);
    bool isMeleeAttack = animType == ANIM_THROW_PUNCH || animType == ANIM_KICK_LEG
        || animType == ANIM_THRUST_ANIM || animType == ANIM_SWING_ANIM;
    bool isRangedAttack = animType == ANIM_FIRE_SINGLE || animType == ANIM_FIRE_BURST
        || animType == ANIM_FIRE_CONTINUOUS;
    bool isThrowAttack = animType == ANIM_THROW_ANIM;
    // The quick slots' weapon draw after a throw is paced like the throw itself, by AGILITY
    // (anim_quick_draw_begin; his pick 2026-10-16). Only that one take-out -- every other weapon
    // switch keeps the global lever.
    bool isQuickDraw = animType == ANIM_TAKE_OUT && object != NULL && object == anim_quick_draw_obj;

    if (combat_rt_enabled() && object != NULL && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
        && (isMeleeAttack || isRangedAttack || isThrowAttack || isQuickDraw)) {
        int stat = isMeleeAttack ? STAT_STRENGTH : ((isThrowAttack || isQuickDraw) ? STAT_AGILITY : STAT_PERCEPTION);
        // Asymmetric: above-average points are worth more than below-average ones, so investing
        // past 5 actually shows. See ANIM_ATTACK_STAT_HIGH_BIAS.
        int delta = stat_level(object, stat) - 5;
        float factor = delta > 0 ? anim_attack_stat_factor * anim_attack_stat_high_bias
                                 : anim_attack_stat_factor;
        float mult = 1.0f - delta * factor;
        if (mult < ANIM_ATTACK_SPEED_MIN) {
            mult = ANIM_ATTACK_SPEED_MIN;
        } else if (mult > ANIM_ATTACK_SPEED_MAX) {
            mult = ANIM_ATTACK_SPEED_MAX;
        }
        tpf = (unsigned int)(tpf * mult);
        if (tpf < 1) {
            tpf = 1;
        }
    } else if (anim_speed_scale != 1.0f) {
        // Non-attack anims (movement, idle, reactions, death) keep the global lever.
        tpf = (unsigned int)(tpf * anim_speed_scale);
        if (tpf < 1) {
            tpf = 1;
        }
    }

    // WALK ONLY, on top of whatever the above decided. anim_speed is the whole-game snappiness lever
    // and moves idle, reactions and death falls with it; this one touches nothing but the walk.
    //
    // Expressed as a SPEED (1.5 = half again as fast), not a duration, because that is how the
    // request is phrased -- so it divides rather than multiplies.
    //
    // ⚠️ RUN IS DELIBERATELY EXCLUDED. It was in here at first and the run came out "super fast":
    // the run art is already roughly twice the walk, so the same multiplier lands on a much bigger
    // number and reads as a sprint. Walking is what wanted the nudge; running was already fine.
    //
    // ⚠️ SYMMETRIC ON PURPOSE. It applies to the dude AND every NPC, which is what keeps
    // walk_dir_slowdown honest: that function equalises an NPC's tile-crossing time against the
    // DUDE's and only ever SLOWS, never speeds up. Speeding the player alone would put every enemy
    // permanently behind him and quietly turn kiting into a free win.
    if (move_speed_scale != 1.0f && animType == ANIM_WALK) {
        tpf = (unsigned int)(tpf / move_speed_scale);
        if (tpf < 1) {
            tpf = 1;
        }
    }

    // AIMED-SHOT SLOW MOTION, applied LAST so it scales whatever every lever above arrived at.
    // combat_rt scales its own dt by the same number, so the combat model and the animation clock
    // stretch together; without this half the enemies would decide at quarter speed while still
    // walking and swinging at full speed.
    //
    // tpf is a DURATION, so slowing divides: scale 0.25 makes each frame last four times as long.
    //
    // ⚠️ IN-FLIGHT ANIMATIONS ONLY PARTLY FOLLOW. ticksPerFrame is baked into the sad entry when
    // an animation is registered, and the scheduler re-reads it each tick but nothing recomputes it
    // -- except the WALK and RUN branches in object_animate, which call compute_tpf again on every
    // step and therefore pick this up immediately. Movement is what you actually watch during an
    // aim, so the effect reads correctly; a swing already in the air finishes at its original
    // speed. Re-timing every live sad entry on entry/exit would be the complete fix and is not
    // worth the reach into the animation tables for a fraction of a second of mismatch.
    //
    // ⚠️ RATIO-SAFE. walk_dir_slowdown and run_speed_scale compare an NPC's compute_tpf against
    // the DUDE's; a factor applied to both cancels in the quotient, so normalisation is unaffected.
    //
    // ⚠️ AND THE COMBAT SPEED SLIDER (2026-10-11) is the same scale above 1: a duration divided by 2
    // runs twice as fast. Rounded, never below one tick.
    //
    // ⚠️ ENERGY BOLTS AND ROCKETS FLY FASTER IN REALTIME (2026-10-11, his pick "1" of three). A visible
    // projectile crosses the gap at its art's own walking pace, one tile per frame -- a beat you never
    // notice between turns and a lag in a realtime fight. The ones the game CREATES for the shot (laser,
    // plasma, the Alien Blaster, a rocket: obj_new with no pid, MISC art) now fly RT_PROJECTILE_SPEEDUP
    // times faster; a THROWN weapon is the item itself (it keeps its pid) and keeps its pace, because
    // watching the spear or the grenade arc across is the point. Bullet guns have no projectile at all.
    //
    // ⚠️ THE FLIGHT ONLY (2026-10-16). A projectile flies as ANIM_WALK (action_ranged's straight move);
    // the blast it turns into plays ANIM_STAND on the SAME object, and the eight ring sprites around it
    // are obj_new'd with no pid too -- so without the walk test a grenade's ring flashed four times
    // faster than its own centre (0.18 s against 0.7 s) and a rocket's whole blast was over in 0.18 s.
    // An explosion plays at its art's pace again.
    bool projectileFlight = combat_rt_enabled() && object != NULL && animType == ANIM_WALK
        && FID_TYPE(object->fid) == OBJ_TYPE_MISC;
    if (projectileFlight && object->pid == -1) {
        tpf = (unsigned int)(tpf / anim_projectile_kind_speedup(object->fid) + 0.5f);
        if (tpf < 1) {
            tpf = 1;
        }
    } else if (projectileFlight && PID_TYPE(object->pid) == OBJ_TYPE_ITEM) {
        // ⚠️ ...AND A THROWN ONE FLIES SLOWER (2026-10-16, "ať je vidět projektil granátu, přišel mi hod
        // granátu fakt fast"). A thrown weapon is the item itself, wearing its projectile art for the
        // flight -- only then is an ITEM's fid MISC -- and a grenade's is GRNDTRO.FRM, a 7x7 px tumble
        // at 10 fps that hops a tile per frame: 10 tiles a second, a three-tile throw over in 0.3 s,
        // a dot nobody sees. RT_THROWN_SLOWDOWN times its pace; the quick slots made throwing a thing
        // you do every fight.
        //
        // ⚠️ THE THROWER WAITS FOR IT. The flight and the blast sit in the thrower's own attack
        // sequence, ahead of his take-out tail, and check_registry refuses him anything new while
        // that sequence lives -- so a longer flight is a longer stand after a LONG throw (his throw
        // pose, ~1.5 s at AGI 9, already outlasts a short one). combat_rt's explosive commit cap
        // (RT_RESOLVE_MAX_SECONDS x3) is sized to outlast a full-range throw at this pace.
        tpf = (unsigned int)(tpf * RT_THROWN_SLOWDOWN + 0.5f);
    }
    // ⚠️ A FLINCH PLAYS WHOLE (2026-10-13, "ne bez cuknutí, ale ať to vypadá plynule, staggering tam
    // chci mít jak mám"). A light stagger holds the body for combat_rt_flinch_seconds() (0.4 s) --
    // that length is gameplay and stays -- but the hit art is longer: every moving NPC's flinch in
    // trace_2026-10-11_215834 (14 of 14) ended by the cap, i.e. the recoil was cut off partway and
    // the body jumped from a mid-recoil frame straight to standing and walking. That jump is the
    // "uncanny" look. So the whole hit animation is fitted into 90 % of the window: it recoils and
    // recovers on its own last frame, and the stagger lasts exactly as long as before. Only ever
    // faster than the art's own rate, never slower; before the time scale below, which then scales
    // the animation and the window alike.
    if (combat_rt_enabled() && object != NULL && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER
        && (animType == ANIM_HIT_FROM_FRONT || animType == ANIM_HIT_FROM_BACK)) {
        CacheEntry* hitHandle;
        Art* hitArt = art_ptr_lock(fid, &hitHandle);
        if (hitArt != NULL) {
            int frames = art_frame_max_frame(hitArt);
            art_ptr_unlock(hitHandle);
            if (frames > 0) {
                unsigned int fitTpf = (unsigned int)(combat_rt_flinch_seconds() * 1000.0f * 0.9f / frames);
                if (fitTpf >= 1 && fitTpf < tpf) {
                    tpf = fitTpf;
                }
            }
        }
    }

    {
        float timeScale = combat_rt_time_scale();
        if (timeScale != 1.0f) {
            tpf = (unsigned int)(tpf / timeScale + 0.5f);
            if (tpf < 1) {
                tpf = 1;
            }
        }
    }

    // WEAPON SWAP by AGILITY (realtime only). Putting a weapon away and drawing another is hand
    // work, so Agility owns it -- the same stat that already sets the AP pool and its refill rate.
    // Together those are the three faces of "how fast do you operate".
    //
    // ⚠️ SYMMETRIC (no high bias), unlike the attack-animation curve just above, and the difference
    // is the whole point. An attack animation is COSMETIC -- the cooldown paces the fight, so
    // exaggerating the top end buys a look and nothing else. A swap animation holds anim_busy, so
    // for its whole duration you cannot attack: it IS throughput, and an exaggerated curve here
    // would be hidden combat power. That is exactly the mistake made on 2026-08-10, when the
    // animation bias was copied onto the attack cooldown and the player felt it as "attacks at
    // Strength 10 are maybe too fast".
    //
    // ⚠️ THE RELOAD IS THE SAME KIND OF WORK AND GETS THE SAME CURVE (2026-10-04, "rychlost se odvíjí
    // podle stats"). It holds the dude for its whole length exactly like a swap does -- you cannot
    // fire through it -- so it is throughput too, and it takes this symmetric factor rather than the
    // attack animations' biased one. Only a reload's own STAND (anim_reload_timing_obj), never an
    // ordinary standing frame or anyone's idle.
    //
    // ⚠️ THREE KINDS OF HAND WORK NOW, THREE STATS (2026-10-06, his calls), all on this one symmetric
    // curve -- anyone who does the work is timed by their own stats, the dude and NPCs alike:
    //   - a WEAPON SWAP: Agility, as before;
    //   - a RELOAD: the average of Agility and Perception ("můžeme kdyžtak spojit PE a AGI") -- hands
    //     and eyes. Its STAND loop starts inside the bracketed register_end; a PISTOL's reload is a
    //     put-away + take-out (register_object_reload) whose take-out starts later, outside the
    //     bracket, so the reloading registry identifies that half;
    //   - a DRUG INJECTION (ANIM_MAGIC_HANDS_MIDDLE inside anim_set_injection_timing): the average of
    //     Agility, Intelligence and Perception -- "nesmíš být dement a vědět jak to píchnout... abys
    //     věděl kam to píchnout". Its length is also the dude's one-injection-at-a-time lock.
    if (anim_weapon_swap_stat_factor > 0.0f && combat_rt_enabled()
        && object != NULL && FID_TYPE(object->fid) == OBJ_TYPE_CRITTER) {
        bool swapType = animType == ANIM_TAKE_OUT || animType == ANIM_PUT_AWAY;
        bool reloading = (animType == ANIM_STAND || swapType)
            && (object == anim_reload_timing_obj || (swapType && combat_rt_is_reloading(object)));

        float stat = -1.0f;
        if (reloading) {
            stat = (stat_level(object, STAT_AGILITY) + stat_level(object, STAT_PERCEPTION)) / 2.0f;
        } else if (swapType) {
            stat = (float)stat_level(object, STAT_AGILITY);
        } else if (animType == ANIM_MAGIC_HANDS_MIDDLE && object == anim_injection_timing_obj) {
            stat = (stat_level(object, STAT_AGILITY) + stat_level(object, STAT_INTELLIGENCE)
                       + stat_level(object, STAT_PERCEPTION))
                / 3.0f;
        }

        if (stat >= 0.0f) {
            float mult = 1.0f - (stat - 5.0f) * anim_weapon_swap_stat_factor;
            if (mult < ANIM_ATTACK_SPEED_MIN) {
                mult = ANIM_ATTACK_SPEED_MIN;
            } else if (mult > ANIM_ATTACK_SPEED_MAX) {
                mult = ANIM_ATTACK_SPEED_MAX;
            }
            tpf = (unsigned int)(tpf * mult);
            if (tpf < 1) {
                tpf = 1;
            }
        }
    }

    return tpf;
}

// =================================================================================================
// SMOOTH MOVEMENT, HELD STRIDES AND THE STRIDE'S BEAT (2026-10-08: "potřebuji aby všechny animace
// nevypadaly přerušovaně ... aby to bylo plynulé, polish prostě")
// =================================================================================================
//
// Three things made every moving body look broken up, and none of them is the art:
//
// 1. A MOVE ONLY MOVES WHEN ITS FRAME CHANGES. object_move and object_straight_move step the body
//    once per animation frame -- 10 to 20 times a second -- and it stands still in between. On a
//    853x480 picture stretched x3 to 1440p at 240 Hz a walking NPC jumps ~24 screen pixels at a
//    time, while the dude (free movement) and the camera glide every frame, so next to them
//    everything else stutters. The fix is DRAWING ONLY: the logical position (tile, obj->x/y) still
//    jumps exactly as before and every rule that reads it is untouched, but the sprite is drawn
//    gliding from where it was DRAWN to where it now IS, over the step's own frame time -- so the
//    drawn spot trails the real one by at most a step. MODS "Smooth movement", cfg smooth_motion.
//
// 2. EVERY CHASE LEG ENDED STANDING. anim_set_end stands a critter whose sequence finishes, and a
//    chase is a string of short legs (rt_step_toward: up to four tiles each): STAND for a frame,
//    then the next leg's stride from frame 0 -- a hitch every four tiles on every chaser. A leg
//    registered with ANIMATION_REQUEST_DEFER_STAND is held in its stride instead (anim_defer_stand)
//    and only stood if no new leg turns up within ANIM_DEFER_STAND_MS.
//
// 3. EVERY LEG STARTED A FRAME AFTER THE LAST ONE ENDED. A new move sad is due the moment it
//    exists, so the first step of a leg landed ~16 ms after the last step of the leg before: two
//    steps at once, a lurch. anim_move_cadence_hold makes it wait for the beat it would have had.
//
// Measure, don't guess: the main loop's frame-pacing meter reports all of it (anim_smooth_take_stats).

#define ANIM_SMOOTH_SLOTS 64

// Further than this between where a body is drawn and where it really is, and it is not a step: a
// teleport, a script moving it, a door path recompute. Snap rather than glide across the room.
#define ANIM_SMOOTH_MAX_PX 48.0f

typedef struct AnimSmooth {
    Object* obj;
    int elevation;

    // WORLD pixels (anim_smooth_world) the body was DRAWN at when its latest step landed, the
    // moment that step started, and how long the glide from there to the live position takes.
    float fromX;
    float fromY;
    unsigned int t0;
    float dur;

    // Drawn = logical + (dx, dy), as of the last redraw. Every draw, bound and hit test reads THIS
    // value -- never a fresh one -- so the screen and the dirty rects can never disagree about
    // where the body is, which is what keeps the glide from leaving smears behind it.
    int dx;
    int dy;
} AnimSmooth;

static AnimSmooth anim_smooth[ANIM_SMOOTH_SLOTS];
static int anim_smooth_count = 0;
static bool anim_smooth_on = false;

// Rects still showing a body that left the world while it was drawn off its logical spot. The
// eraser redraws the LOGICAL spot only; the next tick redraws these.
#define ANIM_SMOOTH_OWED_SLOTS 16
static Rect anim_smooth_owed[ANIM_SMOOTH_OWED_SLOTS];
static int anim_smooth_owed_elevation[ANIM_SMOOTH_OWED_SLOTS];
static int anim_smooth_owed_count = 0;

// HELD STRIDES (anim_defer_stand). Longer than one frame of the slowest walk art, shorter than the
// eye reads as "it stopped mid-step".
#define ANIM_DEFER_STAND_SLOTS 32
#define ANIM_DEFER_STAND_MS 120

typedef struct AnimDeferredStand {
    Object* obj;
    unsigned int since;
} AnimDeferredStand;

static AnimDeferredStand anim_deferred[ANIM_DEFER_STAND_SLOTS];
static int anim_deferred_count = 0;

// THE STRIDE'S BEAT (anim_move_cadence_hold): when each NPC's last move step landed, in which gait,
// and how long that step lasts.
#define ANIM_LAST_STEP_SLOTS 64

typedef struct AnimLastStep {
    Object* obj;
    int anim;
    unsigned int time;
    unsigned int stepMs;
    bool counted;
} AnimLastStep;

static AnimLastStep anim_last_step[ANIM_LAST_STEP_SLOTS];
static int anim_last_step_count = 0;
static int anim_last_step_cursor = 0;

static int anim_stat_glides = 0;
static int anim_stat_glide_snaps = 0;
static int anim_stat_stride_holds = 0;
static int anim_stat_stride_stands = 0;
static int anim_stat_cadence_holds = 0;

bool anim_get_smooth_motion()
{
    return anim_smooth_on;
}

// Off takes effect at the next tick, which lands every glide in flight on its real spot.
void anim_set_smooth_motion(bool on)
{
    anim_smooth_on = on;
}

// Where obj is, in pixels that do not move with the camera: its tile's screen spot minus tile 0's,
// both taken now, so the scroll cancels out. (tile_set_center keeps the camera's column even, and
// that is what makes tile_coord's differences exact wherever the camera is.)
static bool anim_smooth_world(Object* obj, float* x, float* y)
{
    if (obj->tile == -1) {
        return false;
    }

    int sx;
    int sy;
    int ox;
    int oy;
    if (tile_coord(obj->tile, &sx, &sy, obj->elevation) != 0 || tile_coord(0, &ox, &oy, obj->elevation) != 0) {
        return false;
    }

    *x = (float)(sx - ox + obj->x);
    *y = (float)(sy - oy + obj->y);
    return true;
}

static int anim_smooth_index(Object* obj)
{
    for (int i = 0; i < anim_smooth_count; i++) {
        if (anim_smooth[i].obj == obj) {
            return i;
        }
    }
    return -1;
}

static void anim_smooth_remove(int index)
{
    anim_smooth[index] = anim_smooth[--anim_smooth_count];
}

bool anim_smooth_offset(Object* obj, int* dx, int* dy)
{
    if (anim_smooth_count != 0) {
        int i = anim_smooth_index(obj);
        if (i != -1 && (anim_smooth[i].dx != 0 || anim_smooth[i].dy != 0)) {
            *dx = anim_smooth[i].dx;
            *dy = anim_smooth[i].dy;
            return true;
        }
    }

    *dx = 0;
    *dy = 0;
    return false;
}

// Before a move step: where the body is drawn right now. ⚠️ NEVER THE DUDE -- the camera follows his
// LOGICAL spot (and free movement already glides him), so drawing him anywhere else would knock him
// off the centre of his own view.
static bool anim_smooth_begin(Object* obj, float* drawnX, float* drawnY)
{
    if (!anim_smooth_on || obj == NULL || obj == obj_dude) {
        return false;
    }

    if (!anim_smooth_world(obj, drawnX, drawnY)) {
        return false;
    }

    int i = anim_smooth_index(obj);
    if (i != -1) {
        *drawnX += (float)anim_smooth[i].dx;
        *drawnY += (float)anim_smooth[i].dy;
    }

    return true;
}

// After the step, before object_move redraws `dirty`: start the glide from where the body was drawn
// to where it now is. The body therefore does not move on screen at the instant of the step at all
// -- it starts gliding. `dirty` already covers the old drawn spot (the step bounded the body before
// it moved it); the new one is added here.
static void anim_smooth_end(Object* obj, float drawnX, float drawnY, unsigned int stepMs, Rect* dirty)
{
    int i = anim_smooth_index(obj);

    float wx = 0.0f;
    float wy = 0.0f;
    bool placed = anim_smooth_world(obj, &wx, &wy);

    float ox = placed ? drawnX - wx : 0.0f;
    float oy = placed ? drawnY - wy : 0.0f;
    bool tooFar = fabsf(ox) > ANIM_SMOOTH_MAX_PX || fabsf(oy) > ANIM_SMOOTH_MAX_PX;

    if (!placed || tooFar || (ox == 0.0f && oy == 0.0f)) {
        if (i != -1) {
            if (tooFar) {
                anim_stat_glide_snaps++;
            }
            anim_smooth_remove(i);
            if (placed) {
                Rect r;
                obj_bound(obj, &r);
                rect_min_bound(dirty, &r, dirty);
            }
        }
        return;
    }

    if (i == -1) {
        if (anim_smooth_count >= ANIM_SMOOTH_SLOTS) {
            return; // no slot: this one steps the way it always did
        }
        i = anim_smooth_count++;
        anim_smooth[i].obj = obj;
        anim_stat_glides++;
    }

    AnimSmooth* s = &(anim_smooth[i]);
    s->elevation = obj->elevation;
    s->fromX = drawnX;
    s->fromY = drawnY;
    s->t0 = get_time();

    // A hair LONGER than the step itself, so the glide is still under way when the next step lands
    // and the motion never pauses on a step boundary. The lag that builds settles at a few percent
    // of a step, and the speed on screen is exactly the step's.
    s->dur = (float)stepMs * 1.08f + 4.0f;
    s->dx = (int)lroundf(ox);
    s->dy = (int)lroundf(oy);

    Rect r;
    obj_bound(obj, &r);
    rect_min_bound(dirty, &r, dirty);
}

// ⚠️ A POSE CHANGE THAT MOVES THE FEET, EASED (2026-10-08). Every animation ends in dude_stand, which
// puts the body back on its standing spot in one frame -- and a shot does not end where it began:
// POINT -> FIRE -> UNPOINT drift the body by their frame offsets, so lowering the gun hopped the dude
// 7-12 px sideways (trace_2026-10-08_215600: 30 of them in 13 minutes, `dude-rest anim=44`). He
// asked for it gone ("udělej to"). This measures the jump where the eye sees it -- the FEET, the
// bottom centre of the drawn sprite, before and after the stand -- so whatever the art's own anchors
// already cancel is left alone, and glides only what is left, over ANIM_SETTLE_MS. Drawing only, like
// every glide here: the logical spot is the stand spot at once. The camera already eases on its own
// over the same tenth of a second (rt_cam_advance_lag), so the two settle together.
#define ANIM_SETTLE_MS 110.0f
#define ANIM_SETTLE_MIN_PX 3.0f

static void anim_smooth_settle(Object* obj, const Rect* before)
{
    if (!anim_smooth_on || obj == NULL || obj->tile == -1 || obj->elevation != map_elevation) {
        return;
    }

    int i = anim_smooth_index(obj);
    int curDx = i != -1 ? anim_smooth[i].dx : 0;
    int curDy = i != -1 ? anim_smooth[i].dy : 0;

    // obj_bound includes any glide already in flight; take it out to get the LOGICAL feet.
    Rect after;
    obj_bound(obj, &after);
    float ox = (float)((before->ulx + before->lrx) / 2 - ((after.ulx + after.lrx) / 2 - curDx));
    float oy = (float)(before->lry - (after.lry - curDy));

    if (fabsf(ox) + fabsf(oy) < ANIM_SETTLE_MIN_PX) {
        return; // the art lines up (or nearly): nothing to ease, and a glide in flight carries on
    }
    if (fabsf(ox) > ANIM_SMOOTH_MAX_PX || fabsf(oy) > ANIM_SMOOTH_MAX_PX) {
        return; // not a pose: a teleport or a map move -- the stand snaps as it always did
    }

    float wx;
    float wy;
    if (!anim_smooth_world(obj, &wx, &wy)) {
        return;
    }

    if (i == -1) {
        if (anim_smooth_count >= ANIM_SMOOTH_SLOTS) {
            return;
        }
        i = anim_smooth_count++;
        anim_smooth[i].obj = obj;
        anim_stat_glides++;
    }

    AnimSmooth* sm = &(anim_smooth[i]);
    sm->elevation = obj->elevation;
    sm->fromX = wx + ox;
    sm->fromY = wy + oy;
    sm->t0 = get_time();
    sm->dur = ANIM_SETTLE_MS;
    sm->dx = (int)lroundf(ox);
    sm->dy = (int)lroundf(oy);

    Rect r;
    obj_bound(obj, &r);
    tile_refresh_rect(&r, obj->elevation);
}

void anim_smooth_tick()
{
    for (int i = 0; i < anim_smooth_owed_count; i++) {
        if (anim_smooth_owed_elevation[i] == map_elevation) {
            tile_refresh_rect(&(anim_smooth_owed[i]), map_elevation);
        }
    }
    anim_smooth_owed_count = 0;

    if (anim_smooth_count == 0) {
        return;
    }

    unsigned int now = get_time();

    for (int i = 0; i < anim_smooth_count;) {
        AnimSmooth* s = &(anim_smooth[i]);
        Object* obj = s->obj;

        // Left the map's drawing altogether (picked up, hidden, gone up the stairs): whoever did
        // that redrew where it was. Just forget it.
        if (obj->tile == -1 || obj->elevation != s->elevation || (obj->flags & OBJECT_HIDDEN) != 0) {
            anim_smooth_remove(i);
            continue;
        }

        // Switched off (or finished) lands it on its real spot: offset 0, redrawn below.
        int ndx = 0;
        int ndy = 0;
        bool done = !anim_smooth_on;

        float wx;
        float wy;
        if (!done && anim_smooth_world(obj, &wx, &wy)) {
            unsigned int elapsed = elapsed_tocks(now, s->t0);
            float p = (float)elapsed >= s->dur ? 1.0f : (float)elapsed / s->dur;
            if (p >= 1.0f) {
                done = true;
            } else {
                float ox = (s->fromX - wx) * (1.0f - p);
                float oy = (s->fromY - wy) * (1.0f - p);
                if (fabsf(ox) > ANIM_SMOOTH_MAX_PX || fabsf(oy) > ANIM_SMOOTH_MAX_PX) {
                    // Something other than a step moved it a long way mid-glide.
                    done = true;
                    anim_stat_glide_snaps++;
                } else {
                    ndx = (int)lroundf(ox);
                    ndy = (int)lroundf(oy);
                }
            }
        } else {
            done = true;
        }

        if (ndx != s->dx || ndy != s->dy) {
            Rect before;
            Rect after;
            obj_bound(obj, &before);
            s->dx = ndx;
            s->dy = ndy;
            obj_bound(obj, &after);
            rect_min_bound(&before, &after, &before);
            if (obj->elevation == map_elevation) {
                tile_refresh_rect(&before, obj->elevation);
            }
        }

        if (done) {
            anim_smooth_remove(i);
            continue;
        }

        i++;
    }
}

// A leg of a longer move ended (anim_set_end, ANIM_SEQ_DEFER_STAND): leave the body in its stride
// for now. False when the table is full -- the caller then stands it the old way.
static bool anim_defer_stand(Object* obj)
{
    for (int i = 0; i < anim_deferred_count; i++) {
        if (anim_deferred[i].obj == obj) {
            anim_deferred[i].since = get_time();
            return true;
        }
    }

    if (anim_deferred_count >= ANIM_DEFER_STAND_SLOTS) {
        return false;
    }

    anim_deferred[anim_deferred_count].obj = obj;
    anim_deferred[anim_deferred_count].since = get_time();
    anim_deferred_count++;
    anim_stat_stride_holds++;
    return true;
}

// Every frame (object_animate): a held body that something has taken over since (the next leg, an
// attack, a flinch) is that animation's business now; one that nothing claimed within
// ANIM_DEFER_STAND_MS is stood after all, exactly as anim_set_end would have. `now` stands every
// held body at once (anim_stop: a save or a map change must not leave anyone mid-stride).
static void anim_deferred_stands(bool now)
{
    if (anim_deferred_count == 0) {
        return;
    }

    unsigned int time = get_time();

    for (int i = 0; i < anim_deferred_count;) {
        Object* obj = anim_deferred[i].obj;
        bool drop = false;

        if (!now && anim_busy(obj) != 0) {
            drop = true;
        } else if (now || elapsed_tocks(time, anim_deferred[i].since) >= ANIM_DEFER_STAND_MS) {
            int pose = FID_ANIM_TYPE(obj->fid);
            if ((pose == ANIM_WALK || pose == ANIM_RUNNING) && obj->tile != -1
                && !critter_is_prone(obj) && !critter_is_dead(obj)) {
                dude_stand(obj, obj->rotation, -1);
                anim_stat_stride_stands++;
            }
            drop = true;
        }

        if (drop) {
            anim_deferred[i] = anim_deferred[--anim_deferred_count];
            continue;
        }

        i++;
    }
}

static AnimLastStep* anim_last_step_find(Object* obj)
{
    for (int i = 0; i < anim_last_step_count; i++) {
        if (anim_last_step[i].obj == obj) {
            return &(anim_last_step[i]);
        }
    }
    return NULL;
}

// Every move step of an NPC: when it landed, in which gait, and how long it lasts.
static void anim_note_move_step(Object* obj, int anim, unsigned int time, unsigned int stepMs)
{
    if (!combat_rt_enabled() || obj == NULL || obj == obj_dude || FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    AnimLastStep* r = anim_last_step_find(obj);
    if (r == NULL) {
        if (anim_last_step_count < ANIM_LAST_STEP_SLOTS) {
            r = &(anim_last_step[anim_last_step_count++]);
        } else {
            // Full: recycle round-robin. Losing one critter's beat costs one lurch, nothing more.
            r = &(anim_last_step[anim_last_step_cursor]);
            anim_last_step_cursor = (anim_last_step_cursor + 1) % ANIM_LAST_STEP_SLOTS;
        }
        r->obj = obj;
    }

    r->anim = anim;
    r->time = time;
    r->stepMs = stepMs;
    r->counted = false;
}

// A new leg in the same gait waits until the step the last leg ended on has had its time.
static bool anim_move_cadence_hold(Object* obj, int anim, unsigned int time)
{
    if (!combat_rt_enabled() || obj == NULL || obj == obj_dude) {
        return false;
    }

    AnimLastStep* r = anim_last_step_find(obj);
    if (r == NULL || r->anim != anim) {
        return false;
    }

    unsigned int since = elapsed_tocks(time, r->time);
    if (since >= r->stepMs || since > 600) {
        return false;
    }

    if (!r->counted) {
        r->counted = true;
        anim_stat_cadence_holds++;
    }
    return true;
}

// The object is about to be freed (anim_notify_object_freed): out of every table. If it was drawn
// off its logical spot, that spot is owed a redraw once it is gone (see anim_smooth_owed).
static void anim_smooth_forget(Object* obj)
{
    int i = anim_smooth_index(obj);
    if (i != -1) {
        if ((anim_smooth[i].dx != 0 || anim_smooth[i].dy != 0)
            && anim_smooth_owed_count < ANIM_SMOOTH_OWED_SLOTS && obj->tile != -1) {
            obj_bound(obj, &(anim_smooth_owed[anim_smooth_owed_count]));
            anim_smooth_owed_elevation[anim_smooth_owed_count] = obj->elevation;
            anim_smooth_owed_count++;
        }
        anim_smooth_remove(i);
    }

    for (int j = 0; j < anim_deferred_count;) {
        if (anim_deferred[j].obj == obj) {
            anim_deferred[j] = anim_deferred[--anim_deferred_count];
            continue;
        }
        j++;
    }

    for (int j = 0; j < anim_last_step_count; j++) {
        if (anim_last_step[j].obj == obj) {
            anim_last_step[j] = anim_last_step[--anim_last_step_count];
            break;
        }
    }
}

void anim_smooth_reset()
{
    anim_smooth_count = 0;
    anim_smooth_owed_count = 0;
    anim_deferred_count = 0;
    anim_last_step_count = 0;
    anim_last_step_cursor = 0;
}

void anim_smooth_take_stats(AnimSmoothStats* out)
{
    out->pathSearches = anim_stat_path_searches;
    out->pathMs = anim_stat_path_ms;
    out->pathMaxMs = anim_stat_path_max_ms;
    out->glides = anim_stat_glides;
    out->glideSnaps = anim_stat_glide_snaps;
    out->liveGlides = anim_smooth_count;
    out->strideHolds = anim_stat_stride_holds;
    out->strideStands = anim_stat_stride_stands;
    out->cadenceHolds = anim_stat_cadence_holds;

    anim_stat_path_searches = 0;
    anim_stat_path_ms = 0.0;
    anim_stat_path_max_ms = 0.0;
    anim_stat_glides = 0;
    anim_stat_glide_snaps = 0;
    anim_stat_stride_holds = 0;
    anim_stat_stride_stands = 0;
    anim_stat_cadence_holds = 0;
}

} // namespace fallout
