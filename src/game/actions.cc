#include "game/actions.h"

#include <limits.h>
#include <string.h>

#include "game/anim.h"
#include "game/game_trace.h"
#include "game/combat.h"
#include "game/combat_rt.h"
#include "game/combatai.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "game/trait.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"

namespace fallout {

static int pick_death(Object* attacker, Object* defender, int damage, int damage_type, int anim, bool isFallingBack);
static int check_death(Object* obj, int anim, int minViolenceLevel, bool isFallingBack);
static int internal_destroy(Object* a1, Object* a2);
static int show_death(Object* obj, int anim);
static int action_melee(Attack* attack, int a2);
static int action_ranged(Attack* attack, int a2);
static int is_next_to(Object* a1, Object* a2);
static int action_climb_ladder(Object* a1, Object* a2);
static int report_explosion(Attack* attack, Object* a2);
static int finished_explosion(Object* a1, Object* a2);
static int compute_explosion_damage(int min, int max, Object* a3, int* a4);
static int can_talk_to(Object* a1, Object* a2);
static int talk_to(Object* a1, Object* a2);
static int report_dmg(Attack* attack, Object* a2);
static int compute_dmg_damage(int min, int max, Object* obj, int* a4, int damage_type);

// 0x4FEA50
static bool action_in_explode = false;

// 0x4FEA54
unsigned int rotation = 0;

// 0x4FEA58
int obj_fid = -1;

// 0x4FEA5C
int obj_pid_old = -1;

// 0x4FEA60
static int death_2[DAMAGE_TYPE_COUNT] = {
    ANIM_DANCING_AUTOFIRE,
    ANIM_SLICED_IN_HALF,
    ANIM_CHARRED_BODY,
    ANIM_CHARRED_BODY,
    ANIM_ELECTRIFY,
    ANIM_FALL_BACK,
    ANIM_BIG_HOLE,
};

// 0x4FEA7C
static int death_3[DAMAGE_TYPE_COUNT] = {
    ANIM_CHUNKS_OF_FLESH,
    ANIM_SLICED_IN_HALF,
    ANIM_FIRE_DANCE,
    ANIM_MELTED_TO_NOTHING,
    ANIM_ELECTRIFIED_TO_NOTHING,
    ANIM_FALL_BACK,
    ANIM_EXPLODED_TO_NOTHING,
};

// 0x410410
void switch_dude()
{
    Object* critter;
    int gender;

    critter = pick_object(OBJ_TYPE_CRITTER, false);
    if (critter != NULL) {
        gender = stat_level(critter, STAT_GENDER);
        stat_set_base(obj_dude, STAT_GENDER, gender);

        obj_dude = critter;
        obj_fid = critter->fid;
        obj_pid_old = critter->pid;
        critter->pid = 0x1000000;
    }
}

// 0x410468
int action_knockback(Object* obj, int* anim, int maxDistance, int rotation, int delay)
{
    if (*anim == ANIM_FALL_FRONT) {
        int fid = art_id(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, *anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (!art_exists(fid)) {
            *anim = ANIM_FALL_BACK;
        }
    }

    int distance;
    int tile;
    for (distance = 1; distance <= maxDistance; distance++) {
        tile = tile_num_in_direction(obj->tile, rotation, distance);
        if (obj_blocking_at(obj, tile, obj->elevation) != NULL) {
            distance--;
            break;
        }
    }

    const char* soundEffectName = gsnd_build_character_sfx_name(obj, *anim, CHARACTER_SOUND_EFFECT_KNOCKDOWN);
    register_object_play_sfx(obj, soundEffectName, delay);

    // TODO: Check, probably step back because we've started with 1?
    distance--;

    if (distance <= 0) {
        tile = obj->tile;
        register_object_animate(obj, *anim, 0);
    } else {
        tile = tile_num_in_direction(obj->tile, rotation, distance);
        register_object_animate_and_move_straight(obj, tile, obj->elevation, *anim, 0);
    }

    return tile;
}

// 0x410548
int action_blood(Object* obj, int anim, int delay)
{

    int violence_level = VIOLENCE_LEVEL_MAXIMUM_BLOOD;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, &violence_level);
    if (violence_level == VIOLENCE_LEVEL_NONE) {
        return anim;
    }

    int bloodyAnim;
    if (anim == ANIM_FALL_BACK) {
        bloodyAnim = ANIM_FALL_BACK_BLOOD;
    } else if (anim == ANIM_FALL_FRONT) {
        bloodyAnim = ANIM_FALL_FRONT_BLOOD;
    } else {
        return anim;
    }

    int fid = art_id(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, bloodyAnim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    if (art_exists(fid)) {
        register_object_animate(obj, bloodyAnim, delay);
    } else {
        bloodyAnim = anim;
    }

    return bloodyAnim;
}

// These critter protos have ONLY explode-to-nothing death art -- every normal death frame is
// missing, so a non-explode death renders as an invisible "blank" corpse. pick_death hardcodes
// their explode; this shared predicate lets the realtime death paths force it consistently
// (standing AND prone) so they always take the valid explode instead of settling into a blank body.
static bool is_explode_death_pid(int pid)
{
    return pid == 16777239 || pid == 16777265 || pid == 16777266;
}

// 0x4105EC
static int pick_death(Object* attacker, Object* defender, int damage, int damage_type, int anim, bool hit_from_front)
{
    int violence_level = VIOLENCE_LEVEL_MAXIMUM_BLOOD;
    bool has_bloody_mess = false;
    int death_anim;

    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, &violence_level);

    if (is_explode_death_pid(defender->pid)) {
        // In realtime, force the explode regardless of the violence-level setting: a low-gore
        // config would otherwise fail the VIOLENCE_LEVEL_NORMAL gate and fall this back to a normal
        // death frame the proto has no art for -- an invisible blank corpse. VIOLENCE_LEVEL_NONE
        // makes check_death take the explode whenever its art exists. Turn-based keeps vanilla.
        return check_death(defender, ANIM_EXPLODED_TO_NOTHING,
            combat_rt_enabled() ? VIOLENCE_LEVEL_NONE : VIOLENCE_LEVEL_NORMAL, hit_from_front);
    }

    if (attacker == obj_dude) {
        if (trait_level(TRAIT_BLOODY_MESS)) {
            has_bloody_mess = true;
        }
    }

    if (anim == ANIM_THROW_PUNCH
        || anim == ANIM_KICK_LEG
        || anim == ANIM_THRUST_ANIM
        || anim == ANIM_SWING_ANIM
        || anim == ANIM_THROW_ANIM) {
        if (violence_level == VIOLENCE_LEVEL_MAXIMUM_BLOOD && has_bloody_mess) {
            death_anim = ANIM_BIG_HOLE;
        } else {
            death_anim = ANIM_FALL_BACK;
        }
    } else if (anim == ANIM_FIRE_SINGLE && damage_type == DAMAGE_TYPE_NORMAL) {
        if (violence_level == VIOLENCE_LEVEL_MAXIMUM_BLOOD && (has_bloody_mess || damage >= 45)) {
            death_anim = ANIM_BIG_HOLE;
        } else {
            death_anim = ANIM_FALL_BACK;
        }
    } else {
        if (violence_level > VIOLENCE_LEVEL_NORMAL && (has_bloody_mess || damage >= 45)) {
            death_anim = death_3[damage_type];
            if (check_death(defender, death_anim, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hit_from_front) != death_anim) {
                death_anim = death_2[damage_type];
            }
        } else if (violence_level > VIOLENCE_LEVEL_MINIMAL && (has_bloody_mess || damage >= 15)) {
            death_anim = death_2[damage_type];
        } else {
            death_anim = ANIM_FALL_BACK;
        }
    }

    if (!hit_from_front && death_anim == ANIM_FALL_BACK) {
        death_anim = ANIM_FALL_FRONT;
    }

    return check_death(defender, death_anim, VIOLENCE_LEVEL_NONE, hit_from_front);
}

// 0x410754
static int check_death(Object* obj, int anim, int min_violence_level, bool hit_from_front)
{
    int fid;
    int violence_level = VIOLENCE_LEVEL_MAXIMUM_BLOOD;

    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, &violence_level);
    if (violence_level >= min_violence_level) {
        fid = art_id(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (art_exists(fid)) {
            return anim;
        }
    }

    if (hit_from_front) {
        return ANIM_FALL_BACK;
    }

    fid = art_id(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, ANIM_FALL_FRONT, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    if (art_exists(fid)) {
        return ANIM_FALL_BACK;
    }

    return ANIM_FALL_FRONT;
}

// 0x410808
static int internal_destroy(Object* a1, Object* a2)
{
    return obj_destroy(a2);
}

// TODO: Check very carefully, lots of conditions and jumps.
//
// 0x410810
void show_damage_to_object(Object* defender, int damage, int flags, Object* weapon, bool hit_from_front, int knockback_distance, int knockback_rotation, int a8, Object* attacker, int delay)
{
    int anim;
    int fid;
    const char* sfx_name;

    anim = FID_ANIM_TYPE(defender->fid);
    if (!critter_is_prone(defender)) {
        if ((flags & DAM_DEAD) != 0) {
            fid = art_id(OBJ_TYPE_MISC, 10, 0, 0, 0);
            if (fid == attacker->fid) {
                anim = check_death(defender, ANIM_EXPLODED_TO_NOTHING, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hit_from_front);
            } else if (attacker->pid == PROTO_ID_0x20001EB) {
                anim = check_death(defender, ANIM_ELECTRIFIED_TO_NOTHING, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hit_from_front);
            } else if (attacker->fid == FID_0x20001F5) {
                anim = check_death(defender, a8, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hit_from_front);
            } else {
                anim = pick_death(attacker, defender, damage, item_w_damage_type(weapon), a8, hit_from_front);
            }

            if (anim != ANIM_FIRE_DANCE) {
                if (knockback_distance != 0 && (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK)) {
                    action_knockback(defender, &anim, knockback_distance, knockback_rotation, delay);
                    anim = action_blood(defender, anim, -1);
                } else {
                    sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_DIE);
                    register_object_play_sfx(defender, sfx_name, delay);

                    anim = pick_fall(defender, anim);
                    register_object_animate(defender, anim, 0);

                    if (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK) {
                        anim = action_blood(defender, anim, -1);
                    }
                }
            } else {
                fid = art_id(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_FIRE_DANCE, (defender->fid & 0xF000) >> 12, defender->rotation + 1);
                if (art_exists(fid)) {
                    sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                    register_object_play_sfx(defender, sfx_name, delay);

                    register_object_animate(defender, anim, 0);

                    int randomDistance = roll_random(2, 5);
                    int randomRotation = roll_random(0, 5);

                    while (randomDistance > 0) {
                        int tile = tile_num_in_direction(defender->tile, randomRotation, randomDistance);
                        Object* v35 = NULL;
                        make_straight_path(defender, defender->tile, tile, NULL, &v35, 4);
                        if (v35 == NULL) {
                            register_object_turn_towards(defender, tile);
                            register_object_move_straight_to_tile(defender, tile, defender->elevation, anim, 0);
                            break;
                        }
                        randomDistance--;
                    }
                }

                anim = ANIM_BURNED_TO_NOTHING;
                sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                register_object_play_sfx(defender, sfx_name, -1);
                register_object_animate(defender, anim, 0);
            }
        } else {
            if ((flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) {
                anim = hit_from_front ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
                sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                register_object_play_sfx(defender, sfx_name, delay);
                if (knockback_distance != 0) {
                    action_knockback(defender, &anim, knockback_distance, knockback_rotation, 0);
                } else {
                    anim = pick_fall(defender, anim);
                    register_object_animate(defender, anim, 0);
                }
            } else if ((flags & DAM_ON_FIRE) != 0 && art_exists(art_id(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_FIRE_DANCE, (defender->fid & 0xF000) >> 12, defender->rotation + 1))) {
                register_object_animate(defender, ANIM_FIRE_DANCE, delay);

                fid = art_id(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_STAND, (defender->fid & 0xF000) >> 12, defender->rotation + 1);
                register_object_change_fid(defender, fid, -1);
            } else {
                if (knockback_distance != 0) {
                    anim = hit_from_front ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
                    action_knockback(defender, &anim, knockback_distance, knockback_rotation, delay);
                    if (anim == ANIM_FALL_BACK) {
                        register_object_animate(defender, ANIM_BACK_TO_STANDING, -1);
                    } else {
                        register_object_animate(defender, ANIM_PRONE_TO_STANDING, -1);
                    }
                } else {
                    if (hit_from_front || !art_exists(art_id(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_HIT_FROM_BACK, (defender->fid & 0xF000) >> 12, defender->rotation + 1))) {
                        anim = ANIM_HIT_FROM_FRONT;
                    } else {
                        anim = ANIM_HIT_FROM_BACK;
                    }

                    sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                    register_object_play_sfx(defender, sfx_name, delay);

                    // Realtime drives the light hit-react as the defender's OWN animation
                    // sequence (combat_rt_update, at commit), decoupled from this attacker's
                    // swing. That lets several attackers stagger one target concurrently
                    // (latest-wins) without clearing a flinch cutting anyone's swing -- the
                    // "my shot had no animation" bug. Knockdown/death/fire above still play
                    // here (they own the whole reaction). Turn-based unchanged.
                    if (!combat_rt_enabled()) {
                        register_object_animate(defender, anim, 0);
                    }
                }
            }
        }
    } else {
        if ((flags & DAM_DEAD) != 0 && (defender->data.critter.combat.results & DAM_DEAD) == 0) {
            if (combat_rt_enabled() && is_explode_death_pid(defender->pid)) {
                // This proto has only explode-to-nothing death art; a prone (settle-in-place) death
                // would leave a blank invisible body. Register its valid explode death here so it
                // vanishes cleanly and drops its items (show_death below handles the item drop for
                // the EXPLODED_TO_NOTHING anim), instead of a blank corpse.
                anim = check_death(defender, ANIM_EXPLODED_TO_NOTHING, VIOLENCE_LEVEL_NONE, hit_from_front);
                sfx_name = gsnd_build_character_sfx_name(defender, anim, CHARACTER_SOUND_EFFECT_DIE);
                register_object_play_sfx(defender, sfx_name, delay);
                register_object_animate(defender, anim, 0);
            } else if (!combat_rt_enabled()) {
                // Realtime concurrent-hit fix: under focus-fire a critter is often knocked
                // down by one hit and killed by the next before it can stand. It is already
                // prone here (the knockdown fall registered above by the earlier commit is on
                // screen), so action_blood -- which registers a fresh FALL_BACK_BLOOD -- would
                // make it fall a SECOND time (~2s of "double fall" jank; trace: anim 20 then 34).
                // Skip the bloody re-fall in realtime and let it die where it lies; show_death
                // (below) still settles the corpse. Turn-based keeps the blood animation.
                anim = action_blood(defender, anim, delay);
            }
        } else {
            return;
        }
    }

    if (weapon != NULL) {
        if ((flags & DAM_EXPLODE) != 0) {
            register_object_must_call(defender, weapon, (AnimationCallback*)obj_drop, -1);
            fid = art_id(OBJ_TYPE_MISC, 10, 0, 0, 0);
            register_object_change_fid(weapon, fid, 0);
            register_object_animate_and_hide(weapon, ANIM_STAND, 0);

            sfx_name = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_HIT, weapon, HIT_MODE_RIGHT_WEAPON_PRIMARY, defender);
            register_object_play_sfx(weapon, sfx_name, 0);

            register_object_must_erase(weapon);
        } else if ((flags & DAM_DESTROY) != 0) {
            register_object_must_call(defender, weapon, (AnimationCallback*)internal_destroy, -1);
        } else if ((flags & DAM_DROP) != 0) {
            register_object_must_call(defender, weapon, (AnimationCallback*)obj_drop, -1);
        }
    }

    if ((flags & DAM_DEAD) != 0) {
        // TODO: Get rid of casts.
        register_object_must_call(defender, (void*)anim, (AnimationCallback*)show_death, -1);
    }
}

// 0x410D50
static int show_death(Object* obj, int anim)
{
    Rect temp_rect;
    Rect dirty_rect;

    obj_bound(obj, &dirty_rect);

    if (obj->pid != 16777266 && obj->pid != 16777265 && obj->pid != 16777224) {
        obj->flags |= OBJECT_NO_BLOCK;
        if (obj_toggle_flat(obj, &temp_rect) == 0) {
            rect_min_bound(&dirty_rect, &temp_rect, &dirty_rect);
        }
    }

    if (obj_turn_off_outline(obj, &temp_rect) == 0) {
        rect_min_bound(&dirty_rect, &temp_rect, &dirty_rect);
    }

    if (anim >= ANIM_ELECTRIFIED_TO_NOTHING && anim <= ANIM_EXPLODED_TO_NOTHING) {
        if (obj->pid != 16777265 && obj->pid != 16777266 && obj->pid != 16777239) {
            item_drop_all(obj, obj->tile);
        }
    }

    tile_refresh_rect(&dirty_rect, obj->elevation);

    return 0;
}

// 0x410E74
int show_damage_target(Attack* attack)
{
    int frontHit;

    if (FID_TYPE(attack->defender->fid) == OBJ_TYPE_CRITTER) {
        // NOTE: Uninline.
        frontHit = is_hit_from_front(attack->attacker, attack->defender);

        register_begin(ANIMATION_REQUEST_RESERVED);
        register_priority(1);
        show_damage_to_object(attack->defender,
            attack->defenderDamage,
            attack->defenderFlags,
            attack->weapon,
            frontHit,
            attack->defenderKnockback,
            tile_dir(attack->attacker->tile, attack->defender->tile),
            item_w_anim(attack->attacker, attack->hitMode),
            attack->attacker,
            0);
        register_end();
    }

    return 0;
}

// 0x410F18
int show_damage_extras(Attack* attack)
{
    int v6;
    int v8;
    int v9;

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* obj = attack->extras[index];
        if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
            int delta = attack->attacker->rotation - obj->rotation;
            if (delta < 0) {
                delta = -delta;
            }

            v6 = delta != 0 && delta != 1 && delta != 5;
            register_begin(ANIMATION_REQUEST_RESERVED);
            register_priority(1);
            v8 = item_w_anim(attack->attacker, attack->hitMode);
            v9 = tile_dir(attack->attacker->tile, obj->tile);
            show_damage_to_object(obj, attack->extrasDamage[index], attack->extrasFlags[index], attack->weapon, v6, attack->extrasKnockback[index], v9, v8, attack->attacker, 0);
            register_end();
        }
    }

    return 0;
}

// --- REALTIME: the defender's reaction runs as its OWN animation sequence. ---
//
// anim_busy(obj) reports "busy" for any live sequence that holds a description OWNED by obj --
// including descriptions that already finished playing -- so an actor stays busy for as long as
// the whole sequence exists, not just his own part of it. Vanilla packs an entire attack into one
// sequence: the attacker's swing, then the defender's hit/death reaction, then the attacker's
// trailing ANIM_UNPOINT. Turn-based doesn't care (the turn is over anyway), but in realtime it
// pins the shooter in place for the whole death fall of whoever he just killed -- "the dude just
// stands there waiting for the death animation to finish". Splitting the defender out ends the
// attacker's sequence with his own swing, so he can walk off and shoot someone else while the
// corpse is still dropping.
//
// Only the melee/ranged ATTACK paths split. The explosion paths leave it alone: their "attacker"
// is the explosion object, not a critter waiting to move.
// DISABLED -- the split breaks HIT TIMING, which is far more noticeable than what it fixed.
//
// An animation sequence plays its descriptions IN ORDER. Vanilla's single sequence is therefore
// also the synchronisation: attacker's swing, then the projectile's flight, and only then the
// defender's reaction -- the target flinches when the bullet ARRIVES. Splitting the defender into
// its own sequence starts that reaction immediately and in parallel, so the target reacts the
// instant the trigger is pulled, while the gunshot SFX (registered inside the ATTACKER's sequence,
// at its action frame) still plays on its own schedule. The player reported exactly both halves of
// that: "they react before I manage to hit them" and "I hit them and THEN the gunshot plays."
// Passing the action frame as a delay does not rescue it -- the delay is relative to the start of
// the new sequence, not to where the attacker's sequence has got to, and projectile flight time
// depends on distance.
//
// The problem it was solving is real (anim_busy keeps the shooter locked for the whole of his
// victim's death fall). The correct fix is to register the defender's reaction from combat_rt's
// COMMIT point, which is already action-frame synchronised, instead of at fire time -- a bigger
// change that needs its own trace verification. The machinery below is deliberately kept intact
// for that follow-up; flipping this to combat_rt_enabled() re-enables it.
// 2026-08-09: RE-ENABLED for realtime, with the timing fixed the way the note above prescribes.
// The split itself was never the problem -- the CALL SITE was. It used to fire right after the
// attacker's register_end(), i.e. at trigger-pull, so the target reacted before the bullet arrived.
// It is now driven from combat_rt's COMMIT point (action_defender_reaction), which is already
// action-frame synchronised: the reaction registers at the instant the blow lands, which is exactly
// what vanilla's single ordered sequence achieved -- only now the defender's fall lives in the
// DEFENDER's own sequence, so the attacker's sequence ends with his swing and he is free to move
// instead of waiting out his victim's death ("I have to wait for the rat to bleed out").
static bool action_split_defender_anim()
{
    return combat_rt_enabled();
}

// The defender half of show_damage, split out so it can be registered either inline (vanilla,
// inside the attacker's sequence) or standalone (realtime, see show_damage_defender_split).
static void show_damage_to_defender(Attack* attack, int a2, int delay)
{
    bool hit_from_front = is_hit_from_front(attack->attacker, attack->defender);

    // fid 33554933 keeps the caller's animation code instead of deriving one from the weapon.
    int weaponAnim = attack->attacker->fid == 33554933
        ? a2
        : item_w_anim(attack->attacker, attack->hitMode);

    show_damage_to_object(attack->defender,
        attack->defenderDamage,
        attack->defenderFlags,
        attack->weapon,
        hit_from_front,
        attack->defenderKnockback,
        tile_dir(attack->attacker->tile, attack->defender->tile),
        weaponAnim,
        attack->attacker,
        delay);
}

// Realtime counterpart of the inline defender block: register the reaction as a standalone
// sequence, exactly the way show_damage_target / show_damage_extras already do. MUST be called
// after the attacker's register_end(); register_begin cannot nest.
//
// Error handling matches those two (the return is ignored): if no sequence slot is free the
// reaction simply doesn't animate. That is not a correctness hole -- combat_rt commits the damage
// from its own in-flight snapshot, not from this callback.
static bool show_damage_defender_split(Attack* attack, int a2, int delay)
{
    if ((attack->attackerFlags & DAM_HIT) == 0
        || attack->defender == NULL
        || FID_TYPE(attack->defender->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    // show_damage zeroes the delay once it has spent it on the extras ping; mirror that so a
    // burst that clips several critters reacts on the same beat it did before the split.
    for (int index = 0; index < attack->extrasLength; index++) {
        if (FID_TYPE(attack->extras[index]->fid) == OBJ_TYPE_CRITTER) {
            delay = 0;
            break;
        }
    }

    register_begin(ANIMATION_REQUEST_RESERVED);
    register_priority(1);
    show_damage_to_defender(attack, a2, delay);
    // Report whether the sequence actually took. It does NOT when the defender is still held by a
    // live sequence of its own (check_registry refuses) -- e.g. killed mid-swing. The caller needs
    // to know: a death whose fall never registered has to fall back to settling a static corpse.
    return register_end() == 0;
}

// CE: the explosive projectile (rocket, grenade) the LAST action_ranged put in the air, and the
// explosion art it turns into when it lands. combat_rt takes it straight after the attack is issued
// (action_take_explosive_projectile) and commits the blast when the projectile wears that art.
// Since 2026-10-16 a thrown knife or rock rides here too, with its own item art as the "explosion".
static Object* action_rt_explosive_projectile = NULL;
static int action_rt_explosion_fid = -1;

Object* action_take_explosive_projectile(int* explosionFid)
{
    Object* projectile = action_rt_explosive_projectile;
    if (explosionFid != NULL) {
        *explosionFid = action_rt_explosion_fid;
    }
    action_rt_explosive_projectile = NULL;
    action_rt_explosion_fid = -1;
    return projectile;
}

// Public entry for combat_rt: register the reactions of every OTHER critter the attack caught (a
// burst's cone, a blast's ring) at the commit, beside action_defender_reaction. Realtime only --
// turn-based registers them from action_melee / action_ranged as it always did.
//
// A victim the blow kills or knocks down gets whatever it was doing cut first, the way the main
// defender does in action_attack: a fall that cannot register on a busy body is exactly how the
// splash victims ended up snapped flat with no animation at all.
void action_extras_reaction(Attack* attack)
{
    if (attack == NULL || !combat_rt_enabled() || attack->extrasLength <= 0) {
        return;
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* extra = attack->extras[index];
        if (extra == NULL || FID_TYPE(extra->fid) != OBJ_TYPE_CRITTER) {
            continue;
        }
        if ((attack->extrasFlags[index] & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) != 0
            && !critter_is_prone(extra)
            && (extra->data.critter.combat.results & DAM_DEAD) == 0) {
            anim_force_clear(extra);
        }
    }

    show_damage_extras(attack);
}

// Public entry for combat_rt: register the defender's damage reaction (its fall / knockdown /
// death, the weapon-drop callbacks and the show_death corpse settle) as the DEFENDER's OWN
// sequence, at the moment the attack COMMITS. Realtime only -- turn-based keeps everything in the
// attacker's one ordered sequence.
//
// Called from the commit pass, which is action-frame synchronised, so this replaces vanilla's
// ordering-by-sequence with ordering-by-commit-time and the reaction still lands with the blow.
// Returns true if the reaction sequence registered. False means the defender was too busy to take
// it (see show_damage_defender_split) and a lethal hit will need the static-corpse fallback.
bool action_defender_reaction(Attack* attack)
{
    if (attack == NULL || !combat_rt_enabled()) {
        return false;
    }

    Object* defender = attack->defender;
    if (defender == NULL || FID_TYPE(defender->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    if ((attack->attackerFlags & DAM_HIT) == 0) {
        // MISS -> the cosmetic dodge, which action_suppress_miss_dodge no longer registers inline.
        // Its own sequence, so it can never block the attacker (or be blocked BY him), and timed
        // from the commit so the target ducks as the blow arrives rather than at the trigger pull.
        // Skipped for a prone or already-busy defender: it is pure decoration, never worth cutting
        // something real (a swing, a fall) to play.
        if (critter_is_prone(defender) || anim_busy(defender) != 0) {
            return false;
        }

        register_begin(ANIMATION_REQUEST_RESERVED);
        register_priority(1);
        const char* sfx = gsnd_build_character_sfx_name(defender, ANIM_DODGE_ANIM, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(defender, sfx, 0);
        register_object_animate(defender, ANIM_DODGE_ANIM, 0);
        return register_end() == 0;
    }

    // a2 only matters for the fid == 33554933 special case (the caller's own animation code);
    // every realtime attack derives the weapon animation from the attacker's hit mode instead.
    return show_damage_defender_split(attack, 0, 0);
}

// 0x410FD8
//
// splitDefender: realtime attack paths pass true to SKIP the defender's reaction here, having it
// registered separately after register_end() instead. See action_split_defender_anim.
void show_damage(Attack* attack, int a2, int delay, bool splitDefender)
{
    for (int index = 0; index < attack->extrasLength; index++) {
        Object* object = attack->extras[index];
        if (FID_TYPE(object->fid) == OBJ_TYPE_CRITTER) {
            register_ping(2, delay);
            delay = 0;
        }
    }

    if ((attack->attackerFlags & DAM_HIT) == 0) {
        if ((attack->attackerFlags & DAM_CRITICAL) != 0) {
            show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, a2, attack->attacker, -1);
        } else if ((attack->attackerFlags & DAM_BACKWASH) != 0) {
            show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, a2, attack->attacker, -1);
        }
    } else {
        // Skipped under splitDefender: the caller registers this on its own sequence right after
        // its register_end(), so the attacker isn't held busy by the defender's death fall.
        if (attack->defender != NULL && !splitDefender) {
            if (FID_TYPE(attack->defender->fid) == OBJ_TYPE_CRITTER) {
                show_damage_to_defender(attack, a2, delay);
            }
        }

        if ((attack->attackerFlags & DAM_DUD) != 0) {
            show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, a2, attack->attacker, -1);
        }
    }
}

// 0x411134
int action_attack(Attack* attack)
{
    // ⚠️ register_clear ends ONE sequence and returns (anim.cc:602 -- `anim_set_end` then
    // `return 0`), but check_registry further down vetoes on EVERY live sequence that lists the
    // object as an owner. An attacker holding TWO of them therefore kept the second, and that
    // second one refused the very first registration inside action_melee/action_ranged; the
    // refusal calls anim_cleanup, which drops the transaction, so register_end returns -1 and
    // the whole attack disappears -- with no reg-clear-refused (nothing was PRIORITIZED) and no
    // reg-begin-fail (the slot was acquired). That is the shape measured in
    // trace_2026-08-23_143356: 285 x `attack-fail rc=-1 atkBusy=0 defBusy=0`, zero of either
    // warning, because a leftover length-1 ANIM_STAND is invisible to anim_busy (anim.cc:839)
    // yet fatal to check_registry (anim.cc:803). So DRAIN them: keep clearing until nothing the
    // attacker owns is left (-1) or one of them is prioritized (-2, the pre-existing refusal).
    for (int cleared = 0;; cleared++) {
        int rc = register_clear(attack->attacker);
        if (rc == -1) {
            break; // no sequence left mentions the attacker
        }
        if (rc == -2) {
            trace_event(TRACE_WARN, "reg-clear-refused", "pid=%d busy=%d blk=%d cleared=%d",
                attack->attacker != NULL ? attack->attacker->pid : -1,
                anim_busy(attack->attacker), anim_busy_blocking(attack->attacker), cleared);
            return -1;
        }
        // Bounded by the sequence table (anim.cc: 64 slots). anim_set_end retires the slot it
        // just cleared, so this always terminates -- the cap is belt-and-braces against a
        // sequence that somehow survives its own end.
        if (cleared >= 64) {
            trace_event(TRACE_WARN, "reg-clear-loop", "pid=%d",
                attack->attacker != NULL ? attack->attacker->pid : -1);
            break;
        }
    }

    // ⚠️ A RELOADING DEFENDER KEEPS RELOADING THROUGH THE TRIGGER PULL (2026-10-06: "ať můžu
    // reloadnout když na mě útočí, takhle jsem stunlocklej"). In realtime nothing of an attack needs
    // the defender's slot at its START -- the dodge and the hit reaction are registered at the commit
    // (action_defender_reaction / rt_reaction_flinch), sized to what actually landed -- yet this clear
    // ended the reload anyway, at the moment the enemy fired, hit or miss: trace_2026-10-06_005347,
    // seven reloads died exactly on an enemy's COMBAT/attack line, two of them to clean misses. His
    // rule since: only a hit that makes you FLINCH (through-damage >= flinch_dt_factor x DT) or a
    // knockdown ends a reload, and both are decided at the commit, where the damage is known.
    bool keepDefenderReload = combat_rt_enabled() && combat_rt_is_reloading(attack->defender);
    if (!keepDefenderReload && register_clear(attack->defender) == -2) {
        // The defender is in a PRIORITIZED sequence -- almost always its OWN attack swing.
        // Turn-based can't reach here (you can't be attacked mid-swing), but realtime can, and
        // failing the attack means "enemies can't hit me while I'm attacking" (and vice versa).
        if (combat_rt_enabled()) {
            // A LIGHT hit registers NO reaction animation on the defender in realtime (the
            // hit-react is suppressed below in show_damage_to_object), so nothing needs the
            // defender's animation slot -- force-clearing its swing anyway made that swing vanish
            // with no anim/sound even though its AP/damage still committed (the "shadow attack").
            // Leave a committed-to swing running; the hit's damage lands regardless (combat_rt
            // snapshots it) and the hit SFX still plays from the attacker's sequence. This matches
            // rt_play_hit_react's swing-protection ("commit to your swing when hit mid-attack").
            // Only a HEAVY reaction (knockdown / death / fire) actually plays a defender animation
            // that must own the slot, so for that we still force-end the swing so the fall plays.
            // BUT never force-clear a defender that is already DYING / knocked down / dead: its
            // prioritized sequence is then its own death/knockdown -- NOT a swing -- and that
            // sequence carries the show_death callback. anim_force_clear flushes pending callbacks,
            // so cutting it runs show_death EARLY, mid-fall, before the body's sub-tile offset
            // settles -- which flats the corpse (drawn as background) and strands it floating in the
            // air (the "dying animation shows as background / corpse in the air" bug, confirmed by
            // the obj-flat backtrace). Let a dying/prone defender's sequence finish on its own; the
            // incoming hit's damage still commits via combat_rt's snapshot.
            if ((attack->defenderFlags & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_ON_FIRE)) != 0
                && !critter_is_prone(attack->defender)
                && (attack->defender->data.critter.combat.results & DAM_DEAD) == 0) {
                anim_force_clear(attack->defender);
            }
        } else {
            return -1;
        }
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        // Same rule for a body caught in the burst: its reload is only a hit's to end, at the commit.
        if (combat_rt_enabled() && combat_rt_is_reloading(attack->extras[index])) {
            continue;
        }
        if (register_clear(attack->extras[index]) == -2) {
            // ⚠️ SAME TRAP AS THE DEFENDER ABOVE, ONE LOOP LOWER -- and it survived the 2026-08-09
            // fix because that fix only touched the defender line. `extras` are the OTHER critters
            // caught in a BURST/cone, so in realtime this read as: "somebody standing near your
            // target is mid-animation, therefore your burst does not happen at all". Silently. In a
            // crowded room that is most of the time.
            //
            // MEASURED (trace_2026-08-15_155257, the Skum Pitt raid): 18 x
            // `WARN/attack-fail DUDE->... rc=-1 atkBusy=0 atkBlk=0 oor=0` in one 18 s fight, in
            // bursts of 5-7 inside a tenth of a second, with a 10mm SMG at dist=6 of range=20 --
            // the attacker free, the target in reach, and the shot dropped anyway. That is the
            // "guards just stood there and hardly anyone was shooting" report from the player's own
            // side of it.
            //
            // Skipping is safe for the same reason it is for the defender: show_damage_extras opens
            // its OWN register_begin/register_end per extra, so an extra that keeps its current
            // sequence simply does not play a reaction -- and its damage still lands, because
            // combat_rt snapshots the whole computed attack rather than reading it back off the
            // animation.
            if (!combat_rt_enabled()) {
                return -1;
            }
        }
    }

    int anim = item_w_anim(attack->attacker, attack->hitMode);

    // Scoped so check_registry names whoever refuses a registration in here -- the one exit
    // that could still drop an attack silently now that the clear above drains the attacker.
    anim_debug_attack_scope(true);
    int rc = (anim < ANIM_FIRE_SINGLE && anim != ANIM_THROW_ANIM)
        ? action_melee(attack, anim)
        : action_ranged(attack, anim);
    anim_debug_attack_scope(false);
    return rc;
}

// True if the defender is mid-locomotion in realtime combat, so its purely-cosmetic
// miss-dodge must be suppressed. Registering ANIM_DODGE_ANIM onto a walking/running
// actor leaves a second competing sad, and when the attack sequence ends anim_set_end
// force-stands the owner (dude_stand) and kills its first sad -- the in-flight WALK
// sad -- freezing locomotion until the next step is issued, for the dude and NPCs
// alike. A miss does no damage or state change, so dropping the dodge for a moving
// target is free; stationary targets still dodge (harmless "you missed" feedback).
static bool action_suppress_miss_dodge(Object* defender)
{
    if (!combat_rt_enabled()) {
        return false;
    }

    // ALWAYS, in realtime. The dodge is the only thing a MISS registers on the defender, and it is
    // registered INSIDE THE ATTACKER'S sequence -- which has two costs, both measured:
    //
    //  1. Registering it needs the defender's animation slot, and check_registry refuses while the
    //     defender is busy. That failure is not local: it aborts the WHOLE attack (register_end ->
    //     action_melee -> combat_attack all return -1), so the attacker never even swings and the
    //     input is silently eaten. 53 of the player's attempts died this way in one fight.
    //  2. Worse in the other direction: it puts the DEFENDER into the attacker's prioritized
    //     sequence, so while a rat's swing at you is still running, YOUR next attack is refused by
    //     action_attack's `register_clear(attacker) == -2`. Measured: 87 of 87 refusals were
    //     exactly that shape (atkBusy=-1 atkBlk=0) -- the block was on the player's side, put there
    //     by the enemy's attack.
    //
    // So the dodge follows the damage reaction out of the attacker's sequence: combat_rt registers
    // it on the DEFENDER's own sequence at the commit point (action_defender_reaction), where it is
    // also correctly timed to the blow instead of to the trigger pull.
    // (The old per-case suppressions -- mid-locomotion, flinch-immunity window, the dude's
    // between-steps backstop -- are all subsumed: the inline dodge is gone entirely, and the
    // split version in action_defender_reaction re-checks prone/busy before it plays.)
    (void)defender;
    return true;
}

// 0x4111C4
static int action_melee(Attack* attack, int anim)
{
    int fid;
    Art* art;
    CacheEntry* cache_entry;
    int v17;
    int v18;
    int delta;
    int flag;
    const char* sfx_name;
    char sfx_name_temp[16];

    bool splitDefender = action_split_defender_anim();

    // The attack's WHOLE failure mode funnels through here. This return value used to be
    // discarded; register_end() then returns -1 purely because no transaction is open, so
    // combat_attack reports -1 with nothing anywhere saying the animation slot was never
    // acquired. Two global reasons: an already-open transaction, or the anim_stop reentry
    // guard -- and both are gone by the time the caller could look at them.
    if (register_begin(ANIMATION_REQUEST_RESERVED) == -1) {
        int openTx = -1;
        int inStop = 0;
        anim_debug_gate(&openTx, &inStop);
        trace_event(TRACE_WARN, "reg-begin-fail", "melee pid=%d openTx=%d inStop=%d",
            attack->attacker != NULL ? attack->attacker->pid : -1, openTx, inStop);
    }
    register_priority(1);

    fid = art_id(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, anim, (attack->attacker->fid & 0xF000) >> 12, attack->attacker->rotation + 1);
    art = art_ptr_lock(fid, &cache_entry);
    if (art != NULL) {
        v17 = art_frame_action_frame(art);
    } else {
        v17 = 0;
    }
    art_ptr_unlock(cache_entry);

    tile_num_in_direction(attack->attacker->tile, attack->attacker->rotation, 1);
    register_object_turn_towards(attack->attacker, attack->defender->tile);

    delta = attack->attacker->rotation - attack->defender->rotation;
    if (delta < 0) {
        delta = -delta;
    }
    flag = delta != 0 && delta != 1 && delta != 5;

    if (anim != ANIM_THROW_PUNCH && anim != ANIM_KICK_LEG) {
        sfx_name = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_ATTACK, attack->weapon, attack->hitMode, attack->defender);
    } else {
        sfx_name = gsnd_build_character_sfx_name(attack->attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    }

    strcpy(sfx_name_temp, sfx_name);

    combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_ATTACK, 0);

    if (attack->attackerFlags & 0x0300) {
        register_object_play_sfx(attack->attacker, sfx_name_temp, 0);
        if (anim != ANIM_THROW_PUNCH && anim != ANIM_KICK_LEG) {
            sfx_name = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_HIT, attack->weapon, attack->hitMode, attack->defender);
        } else {
            sfx_name = gsnd_build_character_sfx_name(attack->attacker, anim, CHARACTER_SOUND_EFFECT_CONTACT);
        }

        strcpy(sfx_name_temp, sfx_name);

        register_object_animate(attack->attacker, anim, 0);
        register_object_play_sfx(attack->attacker, sfx_name_temp, v17);
        show_damage(attack, anim, 0, splitDefender);
    } else {
        // Skip the defender's dodge (play only the attacker's swing) when the
        // defender is already prone, OR -- in realtime combat -- when it's mid-
        // locomotion (dude or NPC): a miss does nothing, so it must not interrupt
        // the walk with a dodge (which the engine's end-of-sequence stand-up would
        // turn into a freeze). Suppressed here at the source (not cancelled after it
        // starts) so repeated misses never stutter the walk.
        if ((attack->defender->data.critter.combat.results & 0x03)
            || action_suppress_miss_dodge(attack->defender)) {
            register_object_play_sfx(attack->attacker, sfx_name_temp, -1);
            register_object_animate(attack->attacker, anim, 0);
        } else {
            fid = art_id(OBJ_TYPE_CRITTER, attack->defender->fid & 0xFFF, ANIM_DODGE_ANIM, (attack->defender->fid & 0xF000) >> 12, attack->defender->rotation + 1);
            art = art_ptr_lock(fid, &cache_entry);
            if (art != NULL) {
                v18 = art_frame_action_frame(art);
                art_ptr_unlock(cache_entry);

                if (v18 <= v17) {
                    register_object_play_sfx(attack->attacker, sfx_name_temp, -1);
                    register_object_animate(attack->attacker, anim, 0);

                    sfx_name = gsnd_build_character_sfx_name(attack->defender, ANIM_DODGE_ANIM, CHARACTER_SOUND_EFFECT_UNUSED);
                    register_object_play_sfx(attack->defender, sfx_name, v17 - v18);
                    register_object_animate(attack->defender, ANIM_DODGE_ANIM, 0);
                } else {
                    sfx_name = gsnd_build_character_sfx_name(attack->defender, ANIM_DODGE_ANIM, CHARACTER_SOUND_EFFECT_UNUSED);
                    register_object_play_sfx(attack->defender, sfx_name, -1);
                    register_object_animate(attack->defender, ANIM_DODGE_ANIM, 0);
                    register_object_play_sfx(attack->attacker, sfx_name_temp, v18 - v17);
                    register_object_animate(attack->attacker, anim, 0);
                }
            }
        }
    }

    if ((attack->attackerFlags & DAM_HIT) != 0) {
        if ((attack->defenderFlags & DAM_DEAD) == 0) {
            combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_HIT, -1);
        }
    } else {
        combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_MISS, -1);
    }

    if (register_end() == -1) {
        return -1;
    }

    // NOT registered here. Firing it now would put the reaction on screen at trigger-pull, ahead
    // of the blow -- the exact bug that got the split disabled. combat_rt calls
    // action_defender_reaction() at its commit point instead. (splitDefender is only ever true
    // under combat_rt, so the turn-based path already registered the reaction inline.)
    // The extras likewise -- combat_rt calls action_extras_reaction() at the same commit.
    if (!splitDefender) {
        show_damage_extras(attack);
    }

    return 0;
}

// 0x4114DC
static int action_ranged(Attack* attack, int anim)
{
    Object* neighboors[6];
    memset(neighboors, 0, sizeof(neighboors));

    bool splitDefender = action_split_defender_anim();

    // The attack's WHOLE failure mode funnels through here. This return value used to be
    // discarded; register_end() then returns -1 purely because no transaction is open, so
    // combat_attack reports -1 with nothing anywhere saying the animation slot was never
    // acquired. Two global reasons: an already-open transaction, or the anim_stop reentry
    // guard -- and both are gone by the time the caller could look at them.
    if (register_begin(ANIMATION_REQUEST_RESERVED) == -1) {
        int openTx = -1;
        int inStop = 0;
        anim_debug_gate(&openTx, &inStop);
        trace_event(TRACE_WARN, "reg-begin-fail", "ranged pid=%d openTx=%d inStop=%d",
            attack->attacker != NULL ? attack->attacker->pid : -1, openTx, inStop);
    }
    register_priority(1);

    Object* projectile = NULL;
    Object* v50 = NULL;
    int weaponFid = -1;

    // Nothing explosive in the air yet from THIS attack (see action_take_explosive_projectile).
    action_rt_explosive_projectile = NULL;
    action_rt_explosion_fid = -1;

    Proto* weaponProto;
    Object* weapon = attack->weapon;
    proto_ptr(weapon->pid, &weaponProto);

    int fid = art_id(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, anim, (attack->attacker->fid & 0xF000) >> 12, attack->attacker->rotation + 1);
    CacheEntry* artHandle;
    Art* art = art_ptr_lock(fid, &artHandle);
    int actionFrame = (art != NULL) ? art_frame_action_frame(art) : 0;
    art_ptr_unlock(artHandle);

    item_w_range(attack->attacker, attack->hitMode);

    int damageType = item_w_damage_type(attack->weapon);

    tile_num_in_direction(attack->attacker->tile, attack->attacker->rotation, 1);

    register_object_turn_towards(attack->attacker, attack->defender->tile);

    bool isGrenade = false;
    if (anim == ANIM_THROW_ANIM) {
        if (damageType == DAMAGE_TYPE_EXPLOSION || damageType == DAMAGE_TYPE_PLASMA || damageType == DAMAGE_TYPE_EMP) {
            isGrenade = true;
        }
    } else if (!combat_rt_enabled() || !combat_rt_weapon_is_up(attack->attacker)) {
        // RAISING the weapon. It is a turn-based ceremony: vanilla fires once per turn, so
        // raise -> fire -> lower reads as one complete action. Fire CONTINUOUSLY and it becomes a
        // per-shot tax -- MEASURED (trace_2026-08-10_183532, 85 attacks) at 0.45 s (10mm Pistol) /
        // 0.56 s (Hunting Rifle) before the swing pose even starts, against 0.02 s for melee. Half
        // a second of nothing visible, and no commit-timing change can fix it because it all
        // happens BEFORE the shot exists.
        //
        // Cut outright on 2026-08-10; restored 2026-08-15 as a PER-BURST cost instead of a
        // per-shot one, once the attack cooldown had grown enough to hide it. combat_rt_weapon_is_-
        // up is true while the shooter has fired recently, so the raise plays on the opening shot
        // of an engagement and is skipped for the follow-ups.
        //
        // ⚠️ THROUGHPUT IS UNAFFECTED EITHER WAY, and that is what made this affordable: the
        // cooldown clock starts when the swing REGISTERS (rt_note_attack_fired, called right after
        // rt_do_attack), so the animation and the cooldown run concurrently. At the player's ~1.98 s
        // cooldown the whole raise/fire/lower sequence fits inside with room to spare. What the
        // raise costs is LATENCY from press to damage, never rate of fire.
        register_object_animate(attack->attacker, ANIM_POINT, -1);
    }

    combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_ATTACK, 0);

    const char* sfx;
    if (((attack->attacker->fid & 0xF000) >> 12) != 0) {
        sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_ATTACK, weapon, attack->hitMode, attack->defender);
    } else {
        sfx = gsnd_build_character_sfx_name(attack->attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    }
    register_object_play_sfx(attack->attacker, sfx, -1);

    register_object_animate(attack->attacker, anim, 0);

    if (anim != ANIM_FIRE_CONTINUOUS) {
        if ((attack->attackerFlags & DAM_HIT) != 0 || (attack->attackerFlags & DAM_CRITICAL) == 0) {
            bool l56 = false;

            int projectilePid = item_w_proj_pid(weapon);
            Proto* projectileProto;
            if (proto_ptr(projectilePid, &projectileProto) != -1 && projectileProto->fid != -1) {
                if (anim == ANIM_THROW_ANIM) {
                    projectile = weapon;
                    weaponFid = weapon->fid;
                    int weaponFlags = weapon->flags;

                    item_remove_mult(attack->attacker, weapon, 1);
                    v50 = item_replace(attack->attacker, weapon, weaponFlags & OBJECT_IN_ANY_HAND);
                    obj_change_fid(projectile, projectileProto->fid, NULL);

                    if (attack->attacker == obj_dude) {
                        intface_update_items(false);
                    }

                    obj_connect(weapon, attack->attacker->tile, attack->attacker->elevation, NULL);
                } else {
                    obj_new(&projectile, projectileProto->fid, -1);
                }

                obj_turn_off(projectile, NULL);

                obj_set_light(projectile, 9, projectile->lightIntensity, NULL);

                int projectileOrigin = combat_bullet_start(attack->attacker, attack->defender);
                obj_move_to_tile(projectile, projectileOrigin, attack->attacker->elevation, NULL);

                int projectileRotation = tile_dir(attack->attacker->tile, attack->defender->tile);
                obj_set_rotation(projectile, projectileRotation, NULL);

                register_object_funset(projectile, OBJECT_HIDDEN, actionFrame);

                const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_AMMO_FLYING, weapon, attack->hitMode, attack->defender);
                register_object_play_sfx(projectile, sfx, 0);

                int v24;
                if ((attack->attackerFlags & DAM_HIT) != 0) {
                    register_object_move_straight_to_tile(projectile, attack->defender->tile, attack->defender->elevation, ANIM_WALK, 0);
                    actionFrame = make_straight_path(projectile, projectileOrigin, attack->defender->tile, NULL, NULL, 32) - 1;
                    v24 = attack->defender->tile;
                } else {
                    register_object_move_straight_to_tile(projectile, attack->tile, attack->defender->elevation, ANIM_WALK, 0);
                    actionFrame = 0;
                    v24 = attack->tile;
                }

                if (isGrenade || damageType == DAMAGE_TYPE_EXPLOSION) {
                    if ((attack->attackerFlags & DAM_DROP) == 0) {
                        int explosionFrmId;
                        if (isGrenade) {
                            switch (damageType) {
                            case DAMAGE_TYPE_EMP:
                                explosionFrmId = 2;
                                break;
                            case DAMAGE_TYPE_PLASMA:
                                explosionFrmId = 31;
                                break;
                            default:
                                explosionFrmId = 29;
                                break;
                            }
                        } else {
                            explosionFrmId = 10;
                        }

                        if (isGrenade) {
                            register_object_change_fid(projectile, weaponFid, -1);
                        }

                        int explosionFid = art_id(OBJ_TYPE_MISC, explosionFrmId, 0, 0, 0);
                        register_object_change_fid(projectile, explosionFid, -1);

                        // For combat_rt: the blast goes off the moment this projectile wears that
                        // art, which is when the attack commits (`landed` in its resolve loop).
                        action_rt_explosive_projectile = projectile;
                        action_rt_explosion_fid = explosionFid;

                        const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_HIT, weapon, attack->hitMode, attack->defender);
                        register_object_play_sfx(projectile, sfx, 0);

                        register_object_animate_and_hide(projectile, ANIM_STAND, 0);

                        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                            if (obj_new(&(neighboors[rotation]), explosionFid, -1) != -1) {
                                obj_turn_off(neighboors[rotation], NULL);

                                int v31 = tile_num_in_direction(v24, rotation, 1);
                                obj_move_to_tile(neighboors[rotation], v31, projectile->elevation, NULL);

                                int delay;
                                if (rotation != ROTATION_NE) {
                                    delay = 0;
                                } else {
                                    if (damageType == DAMAGE_TYPE_PLASMA) {
                                        delay = 4;
                                    } else {
                                        delay = 2;
                                    }
                                }

                                register_object_funset(neighboors[rotation], OBJECT_HIDDEN, delay);
                                register_object_animate_and_hide(neighboors[rotation], ANIM_STAND, 0);
                            }
                        }

                        l56 = true;
                    }
                } else {
                    if (anim != ANIM_THROW_ANIM) {
                        register_object_must_erase(projectile);
                    } else if (combat_rt_enabled()) {
                        // ⚠️ A THROWN KNIFE OR ROCK HITS WHEN IT ARRIVES TOO (2026-10-16). It used to
                        // commit when the throw POSE ended, which was after the landing only while the
                        // flight was quick; thrown weapons now fly at half pace (RT_THROWN_SLOWDOWN in
                        // anim.cc) and a long throw would hurt the target with the knife still in the
                        // air. It lands wearing its own item art again (the change_fid below), so that
                        // fid is its "explosion" for combat_rt's landing test.
                        action_rt_explosive_projectile = projectile;
                        action_rt_explosion_fid = weaponFid;
                    }
                }

                if (!l56) {
                    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_HIT, weapon, attack->hitMode, attack->defender);
                    register_object_play_sfx(weapon, sfx, actionFrame);
                }

                actionFrame = 0;
            } else {
                if ((attack->attackerFlags & DAM_HIT) == 0) {
                    Object* defender = attack->defender;
                    // Same realtime suppression as the melee miss path: don't dodge
                    // (which would stutter the walk) while the defender is moving.
                    if ((defender->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) == 0
                        && !action_suppress_miss_dodge(defender)) {
                        register_object_animate(defender, ANIM_DODGE_ANIM, actionFrame);
                        l56 = true;
                    }
                }
            }
        }
    }

    int defenderDelay = actionFrame;
    show_damage(attack, anim, actionFrame, splitDefender);

    if ((attack->attackerFlags & DAM_HIT) == 0) {
        combatai_msg(attack->defender, attack, AI_MESSAGE_TYPE_MISS, -1);
    } else {
        if ((attack->defenderFlags & DAM_DEAD) == 0) {
            combatai_msg(attack->defender, attack, AI_MESSAGE_TYPE_HIT, -1);
        }
    }

    if (projectile != NULL && (isGrenade || damageType == DAMAGE_TYPE_EXPLOSION)) {
        register_object_must_erase(projectile);
    } else if (anim == ANIM_THROW_ANIM && projectile != NULL) {
        register_object_change_fid(projectile, weaponFid, -1);
    }

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        if (neighboors[rotation] != NULL) {
            register_object_must_erase(neighboors[rotation]);
        }
    }

    if ((attack->attackerFlags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_DEAD)) == 0) {
        if (anim == ANIM_THROW_ANIM) {
            bool l9 = false;
            if (v50 != NULL) {
                int v38 = item_w_anim_code(v50);
                if (v38 != 0) {
                    register_object_take_out(attack->attacker, v38, -1);
                    l9 = true;
                }
            }

            if (!l9) {
                int fid = art_id(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, ANIM_STAND, 0, attack->attacker->rotation + 1);
                register_object_change_fid(attack->attacker, fid, -1);
            }
        } else {
            // ⚠️ RESTORED IN REALTIME 2026-08-10 -- the reason it was dropped has gone stale.
            //
            // It used to be skipped because it is registered AFTER the defender's reaction, so it
            // was the attacker's LAST pending description and kept the shooter owing animation
            // work right through his victim's death fall ("I killed the rat and can't move until
            // it stops twitching"). **That premise died when the defender's reaction moved out of
            // the attacker's sequence** (action_defender_reaction, 2026-08-09): the attacker's
            // sequence now holds only his own shot, so this is a short tail, not a death fall.
            //
            // And it earns its keep: it is the ONLY transition from the fire animation's last
            // frame (weapon up) back to the idle pose. Without it anim_set_end -> dude_stand snaps
            // between the two in one frame, which is the "the animation cuts off oddly at the end"
            // the player reported the moment ANIM_POINT was removed.
            //
            // So the pairing is deliberate and asymmetric: the RAISE stays dropped (half a second
            // of dead time BEFORE the shot, fixing nothing), the LOWER comes back (a tail AFTER
            // the shot, which is exactly where the eye needs the transition). To stop that tail
            // delaying the damage, combat_rt no longer waits for the whole sequence on a
            // projectile attack -- it commits when the FIRING POSE ends (see poseEnded there).
            register_object_animate(attack->attacker, ANIM_UNPOINT, -1);
        }
    }

    if (register_end() == -1) {
        debug_printf("Something went wrong with a ranged attack sequence!\n");
        // No attack, so no blast for combat_rt to wait for -- and the projectile is about to go.
        action_rt_explosive_projectile = NULL;
        action_rt_explosion_fid = -1;
        if (projectile != NULL && (isGrenade || damageType == DAMAGE_TYPE_EXPLOSION || anim != ANIM_THROW_ANIM)) {
            obj_erase_object(projectile, NULL);
        }

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            if (neighboors[rotation] != NULL) {
                obj_erase_object(neighboors[rotation], NULL);
            }
        }

        return -1;
    }

    // Deferred to combat_rt's commit point -- see the matching note in action_melee.
    (void)defenderDelay;

    // The OTHER critters the attack caught (a burst's cone, a blast's ring) react at the commit
    // too, in realtime -- action_extras_reaction, called beside action_defender_reaction. Registered
    // here, at the trigger pull, they fell before anything was decided and mostly did not take at
    // all (busy bodies), leaving the corpse reap to snap them flat with no fall.
    if (!splitDefender) {
        show_damage_extras(attack);
    }

    return 0;
}

// 0x411BCC
int use_an_object(Object* item)
{
    return action_use_an_object(obj_dude, item);
}

// 0x411BE4
static int is_next_to(Object* a1, Object* a2)
{
    MessageListItem messageListItem;

    if (obj_dist(a1, a2) > 1) {
        if (a2 == obj_dude) {
            // You cannot get there.
            messageListItem.num = 2000;
            if (message_search(&misc_message_file, &messageListItem)) {
                display_print(messageListItem.text);
            }
        }
        return -1;
    }

    return 0;
}

// 0x411C30
static int action_climb_ladder(Object* a1, Object* a2)
{
    if (a1 == obj_dude) {
        int anim = FID_ANIM_TYPE(obj_dude->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            register_clear(obj_dude);
        }
    }

    int animationRequestOptions;
    int actionPoints;
    if (isInCombat()) {
        animationRequestOptions = ANIMATION_REQUEST_RESERVED;
        actionPoints = a1->data.critter.combat.ap;
    } else {
        animationRequestOptions = ANIMATION_REQUEST_UNRESERVED;
        actionPoints = -1;
    }

    if (a1 == obj_dude) {
        animationRequestOptions = ANIMATION_REQUEST_RESERVED;
    }

    animationRequestOptions |= ANIMATION_REQUEST_NO_STAND;
    register_begin(animationRequestOptions);

    int tile = tile_num_in_direction(a2->tile, ROTATION_SE, 1);
    if (actionPoints != -1 || obj_dist(a1, a2) < 5) {
        register_object_move_to_tile(a1, tile, a2->elevation, actionPoints, 0);
    } else {
        register_object_run_to_tile(a1, tile, a2->elevation, actionPoints, 0);
    }

    register_object_must_call(a1, a2, (AnimationCallback*)is_next_to, -1);
    register_object_turn_towards(a1, a2->tile);
    register_object_must_call(a1, a2, (AnimationCallback*)check_scenery_ap_cost, -1);

    int weaponAnimationCode = (a1->fid & 0xF000) >> 12;
    if (weaponAnimationCode != 0) {
        const char* puttingAwaySfx = gsnd_build_character_sfx_name(a1, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(a1, puttingAwaySfx, -1);
        register_object_animate(a1, ANIM_PUT_AWAY, 0);
    }

    const char* climbingSfx = gsnd_build_character_sfx_name(a1, ANIM_CLIMB_LADDER, CHARACTER_SOUND_EFFECT_UNUSED);
    register_object_play_sfx(a1, climbingSfx, -1);
    register_object_animate(a1, ANIM_CLIMB_LADDER, 0);
    register_object_call(a1, a2, (AnimationCallback*)obj_use, -1);

    if (weaponAnimationCode != 0) {
        register_object_take_out(a1, weaponAnimationCode, -1);
    }

    return register_end();
}

// 0x411DA8
int a_use_obj(Object* a1, Object* a2, Object* a3)
{
    Proto* scenery_proto;
    int scenery_type = -1;
    int anim_request_options;
    int action_points;
    int anim;
    int weapon_anim_code;

    if (FID_TYPE(a2->fid) == OBJ_TYPE_SCENERY) {
        if (proto_ptr(a2->pid, &scenery_proto) == -1) {
            return -1;
        }

        scenery_type = scenery_proto->scenery.type;
    }

    if (scenery_type == SCENERY_TYPE_LADDER_UP && a3 == NULL) {
        return action_climb_ladder(a1, a2);
    }

    if (a1 == obj_dude) {
        int anim = FID_ANIM_TYPE(obj_dude->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            register_clear(obj_dude);
        }
    }

    if (isInCombat()) {
        anim_request_options = ANIMATION_REQUEST_RESERVED;
        action_points = a1->data.critter.combat.ap;
    } else {
        anim_request_options = ANIMATION_REQUEST_UNRESERVED;
        action_points = -1;
    }

    if (a1 == obj_dude) {
        anim_request_options = ANIMATION_REQUEST_RESERVED;
    }

    register_begin(anim_request_options);

    if (action_points != -1 || obj_dist(a1, a2) < 5) {
        register_object_move_to_object(a1, a2, action_points, 0);
    } else {
        register_object_run_to_object(a1, a2, -1, 0);
    }

    register_object_must_call(a1, a2, (AnimationCallback*)is_next_to, -1);
    register_object_call(a1, a2, (AnimationCallback*)check_scenery_ap_cost, -1);

    weapon_anim_code = (a1->fid & 0xF000) >> 12;
    if (weapon_anim_code != 0) {
        const char* sfx = gsnd_build_character_sfx_name(a1, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(a1, sfx, -1);
        register_object_animate(a1, ANIM_PUT_AWAY, 0);
    }

    if (FID_TYPE(a2->fid) == OBJ_TYPE_CRITTER && critter_is_prone(a2)) {
        anim = ANIM_MAGIC_HANDS_GROUND;
    } else if (FID_TYPE(a2->fid) == OBJ_TYPE_SCENERY && (scenery_proto->scenery.extendedFlags & 0x01) != 0) {
        anim = ANIM_MAGIC_HANDS_GROUND;
    } else {
        anim = ANIM_MAGIC_HANDS_MIDDLE;
    }

    register_object_animate(a1, anim, -1);

    if (a3 != NULL) {
        // TODO: Get rid of cast.
        register_object_call3(a1, a2, a3, (AnimationCallback3*)obj_use_item_on, -1);
    } else {
        register_object_call(a1, a2, (AnimationCallback*)obj_use, -1);
    }

    if (weapon_anim_code != 0) {
        register_object_take_out(a1, weapon_anim_code, -1);
    }

    return register_end();
}

// 0x411F2C
int action_use_an_item_on_object(Object* critter, Object* item, Object* target)
{
    return a_use_obj(critter, item, target);
}

// 0x411F78
int action_use_an_object(Object* critter, Object* item)
{
    return a_use_obj(critter, item, NULL);
}

// 0x411F84
int get_an_object(Object* item)
{
    return action_get_an_object(obj_dude, item);
}

// 0x411F98
int action_get_an_object(Object* critter, Object* item)
{
    if (FID_TYPE(item->fid) != OBJ_TYPE_ITEM) {
        return -1;
    }

    if (critter == obj_dude) {
        int animationCode = FID_ANIM_TYPE(obj_dude->fid);
        if (animationCode == ANIM_WALK || animationCode == ANIM_RUNNING) {
            register_clear(obj_dude);
        }
    }

    if (isInCombat()) {
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_move_to_object(critter, item, critter->data.critter.combat.ap, 0);
    } else {
        register_begin(critter == obj_dude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);
        if (obj_dist(critter, item) >= 5) {
            register_object_run_to_object(critter, item, -1, 0);
        } else {
            register_object_move_to_object(critter, item, -1, 0);
        }
    }

    register_object_must_call(critter, item, (AnimationCallback*)is_next_to, -1);
    register_object_call(critter, item, (AnimationCallback*)check_scenery_ap_cost, -1);

    Proto* itemProto;
    proto_ptr(item->pid, &itemProto);

    if (itemProto->item.type != ITEM_TYPE_CONTAINER || proto_action_can_pickup(item->pid)) {
        register_object_animate(critter, ANIM_MAGIC_HANDS_GROUND, 0);

        int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_MAGIC_HANDS_GROUND, (critter->fid & 0xF000) >> 12, critter->rotation + 1);

        int actionFrame;
        CacheEntry* cacheEntry;
        Art* art = art_ptr_lock(fid, &cacheEntry);
        if (art != NULL) {
            actionFrame = art_frame_action_frame(art);
        } else {
            actionFrame = -1;
        }

        char sfx[16];
        if (art_get_base_name(FID_TYPE(item->fid), item->fid & 0xFFF, sfx) == 0) {
            // NOTE: looks like they copy sfx one more time, what for?
            register_object_play_sfx(item, sfx, actionFrame);
        }

        register_object_call(critter, item, (AnimationCallback*)obj_pickup, actionFrame);
    } else {
        int weaponAnimationCode = (critter->fid & 0xF000) >> 12;
        if (weaponAnimationCode != 0) {
            const char* sfx = gsnd_build_character_sfx_name(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
            register_object_play_sfx(critter, sfx, -1);
            register_object_animate(critter, ANIM_PUT_AWAY, -1);
        }

        // ground vs middle animation
        int anim = (itemProto->item.data.container.openFlags & 0x01) == 0
            ? ANIM_MAGIC_HANDS_MIDDLE
            : ANIM_MAGIC_HANDS_GROUND;
        register_object_animate(critter, anim, 0);

        int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, anim, 0, critter->rotation + 1);

        int actionFrame;
        CacheEntry* cacheEntry;
        Art* art = art_ptr_lock(fid, &cacheEntry);
        if (art == NULL) {
            actionFrame = art_frame_action_frame(art);
            art_ptr_unlock(cacheEntry);
        } else {
            actionFrame = -1;
        }

        if (item->pid != 213) {
            register_object_call(critter, item, (AnimationCallback*)obj_use_container, actionFrame);
        }

        if (weaponAnimationCode != 0) {
            register_object_take_out(critter, weaponAnimationCode, -1);
        }

        if (item->frame == 0 || item->pid == 213) {
            register_object_call(critter, item, (AnimationCallback*)scripts_request_loot_container, -1);
        }
    }

    return register_end();
}

// 0x412254
int action_loot_container(Object* critter, Object* container)
{
    if (FID_TYPE(container->fid) != OBJ_TYPE_CRITTER) {
        return -1;
    }

    if (critter == obj_dude) {
        int anim = FID_ANIM_TYPE(obj_dude->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            register_clear(obj_dude);
        }
    }

    if (isInCombat()) {
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_move_to_object(critter, container, critter->data.critter.combat.ap, 0);
    } else {
        register_begin(critter == obj_dude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);

        if (obj_dist(critter, container) < 5) {
            register_object_move_to_object(critter, container, -1, 0);
        } else {
            register_object_run_to_object(critter, container, -1, 0);
        }
    }

    register_object_must_call(critter, container, (AnimationCallback*)is_next_to, -1);
    register_object_call(critter, container, (AnimationCallback*)check_scenery_ap_cost, -1);
    register_object_call(critter, container, (AnimationCallback*)scripts_request_loot_container, -1);
    return register_end();
}

// 0x41234C
int action_skill_use(int skill)
{
    if (skill == SKILL_SNEAK) {
        register_clear(obj_dude);
        pc_flag_toggle(PC_FLAG_SNEAKING);
        return 0;
    }

    return -1;
}

// 0x41236C
int action_use_skill_in_combat_error(Object* critter)
{
    MessageListItem messageListItem;

    if (critter == obj_dude) {
        messageListItem.num = 902;
        if (message_search(&proto_main_msg_file, &messageListItem) == 1) {
            display_print(messageListItem.text);
        }
    }

    return -1;
}

// 0x4123C8
int action_use_skill_on(Object* a1, Object* a2, int skill)
{
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR:
        if (isInCombat()) {
            // NOTE: Uninline.
            return action_use_skill_in_combat_error(a1);
        }

        if (PID_TYPE(a2->pid) != OBJ_TYPE_CRITTER) {
            return -1;
        }
        break;
    case SKILL_SNEAK:
        pc_flag_toggle(PC_FLAG_SNEAKING);
        return 0;
    case SKILL_LOCKPICK:
        if (isInCombat()) {
            // NOTE: Uninline.
            return action_use_skill_in_combat_error(a1);
        }

        if (PID_TYPE(a2->pid) != OBJ_TYPE_ITEM && PID_TYPE(a2->pid) != OBJ_TYPE_SCENERY) {
            return -1;
        }

        break;
    case SKILL_STEAL:
        if (isInCombat()) {
            // NOTE: Uninline.
            return action_use_skill_in_combat_error(a1);
        }

        if (PID_TYPE(a2->pid) != OBJ_TYPE_ITEM && PID_TYPE(a2->pid) != OBJ_TYPE_CRITTER && a2->pid != 0x2000384) {
            return -1;
        }

        if (a2 == a1) {
            return -1;
        }

        break;
    case SKILL_TRAPS:
        if (isInCombat()) {
            // NOTE: Uninline.
            return action_use_skill_in_combat_error(a1);
        }

        if (PID_TYPE(a2->pid) == OBJ_TYPE_CRITTER) {
            return -1;
        }

        break;
    case SKILL_SCIENCE:
    case SKILL_REPAIR:
        if (isInCombat()) {
            // NOTE: Uninline.
            return action_use_skill_in_combat_error(a1);
        }

        if (PID_TYPE(a2->pid) != OBJ_TYPE_CRITTER) {
            break;
        }

        if (critter_kill_count_type(a2) == KILL_TYPE_ROBOT) {
            break;
        }

        if (critter_kill_count_type(a2) == KILL_TYPE_BRAHMIN
            && skill == SKILL_SCIENCE) {
            break;
        }

        return -1;
    default:
        debug_printf("\nskill_use: invalid skill used.");
        break;
    }

    if (a1 == obj_dude) {
        int anim = FID_ANIM_TYPE(a1->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            register_clear(a1);
        }
    }

    if (isInCombat()) {
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_move_to_object(a1, a2, a1->data.critter.combat.ap, 0);
    } else {
        if (a1 == obj_dude) {
            register_begin(ANIMATION_REQUEST_RESERVED);
        } else {
            register_begin(ANIMATION_REQUEST_UNRESERVED);
        }

        if (a2 != obj_dude) {
            if (obj_dist(a1, a2) >= 5) {
                register_object_run_to_object(a1, a2, -1, 0);
            } else {
                register_object_move_to_object(a1, a2, -1, 0);
            }
        }
    }

    register_object_must_call(a1, a2, (AnimationCallback*)is_next_to, -1);

    int anim = (FID_TYPE(a2->fid) == OBJ_TYPE_CRITTER && critter_is_prone(a2))
        ? ANIM_MAGIC_HANDS_GROUND
        : ANIM_MAGIC_HANDS_MIDDLE;
    int fid = art_id(OBJ_TYPE_CRITTER, a1->fid & 0xFFF, anim, 0, a1->rotation + 1);

    CacheEntry* artHandle;
    Art* art = art_ptr_lock(fid, &artHandle);
    if (art != NULL) {
        art_frame_action_frame(art);
        art_ptr_unlock(artHandle);
    }

    register_object_animate(a1, anim, -1);
    // TODO: Get rid of casts.
    register_object_call3(a1, a2, (void*)skill, (AnimationCallback3*)obj_use_skill_on, -1);
    return register_end();
}

// 0x412758
Object* pick_object(int objectType, bool a2)
{
    Object* foundObject;
    int mouseEvent;
    int keyCode;

    foundObject = NULL;

    do {
        get_input();
    } while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    gmouse_set_cursor(MOUSE_CURSOR_PLUS);
    gmouse_3d_off();

    do {
        if (get_input() == -2) {
            mouseEvent = mouse_get_buttons();
            if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                keyCode = 0;
                foundObject = object_under_mouse(objectType, a2, map_elevation);
                break;
            }

            if ((mouseEvent & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                keyCode = KEY_ESCAPE;
                break;
            }
        }
    } while (game_user_wants_to_quit == 0);

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    gmouse_3d_on();

    if (keyCode == KEY_ESCAPE) {
        return NULL;
    }

    return foundObject;
}

// 0x4127E0
int pick_hex()
{
    int elevation;
    int inputEvent;
    int tile;
    Rect rect;

    elevation = map_elevation;

    while (1) {
        inputEvent = get_input();
        if (inputEvent == -2) {
            break;
        }

        if (inputEvent == KEY_CTRL_ARROW_RIGHT) {
            rotation++;
            if (rotation > 5) {
                rotation = 0;
            }

            obj_set_rotation(obj_mouse, rotation, &rect);
            tile_refresh_rect(&rect, obj_mouse->elevation);
        }

        if (inputEvent == KEY_CTRL_ARROW_LEFT) {
            rotation--;
            if (rotation == -1) {
                rotation = 5;
            }

            obj_set_rotation(obj_mouse, rotation, &rect);
            tile_refresh_rect(&rect, obj_mouse->elevation);
        }

        if (inputEvent == KEY_PAGE_UP || inputEvent == KEY_PAGE_DOWN) {
            if (inputEvent == KEY_PAGE_UP) {
                map_set_elevation(map_elevation + 1);
            } else {
                map_set_elevation(map_elevation - 1);
            }

            rect.ulx = 30;
            rect.uly = 62;
            rect.lrx = 50;
            rect.lry = 88;
        }

        if (game_user_wants_to_quit != 0) {
            return -1;
        }
    }

    if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) == 0) {
        return -1;
    }

    if (!mouse_click_in(0, 0, scr_size.lrx - scr_size.ulx, scr_size.lry - scr_size.uly - 100)) {
        return -1;
    }

    mouse_get_position(&(rect.ulx), &(rect.uly));

    tile = tile_num(rect.ulx, rect.uly, elevation);
    if (tile == -1) {
        return -1;
    }

    return tile;
}

// 0x412950
bool is_hit_from_front(Object* a1, Object* a2)
{
    int diff = a1->rotation - a2->rotation;
    if (diff < 0) {
        diff = -diff;
    }

    return diff != 0 && diff != 1 && diff != 5;
}

// 0x412974
bool can_see(Object* a1, Object* a2)
{
    int diff;

    diff = a1->rotation - tile_dir(a1->tile, a2->tile);
    if (diff < 0) {
        diff = -diff;
    }

    return diff == 0 || diff == 1 || diff == 5;
}

// 0x4129A4
int pick_fall(Object* obj, int anim)
{
    int i;
    int rotation;
    int tile_num;
    int fid;

    if (anim == ANIM_FALL_FRONT) {
        rotation = obj->rotation;
        for (i = 1; i < 3; i++) {
            tile_num = tile_num_in_direction(obj->tile, rotation, i);
            if (obj_blocking_at(obj, tile_num, obj->elevation) != NULL) {
                anim = ANIM_FALL_BACK;
                break;
            }
        }
    } else if (anim == ANIM_FALL_BACK) {
        rotation = (obj->rotation + 3) % ROTATION_COUNT;
        for (i = 1; i < 3; i++) {
            tile_num = tile_num_in_direction(obj->tile, rotation, i);
            if (obj_blocking_at(obj, tile_num, obj->elevation) != NULL) {
                anim = ANIM_FALL_FRONT;
                break;
            }
        }
    }

    if (anim == ANIM_FALL_FRONT) {
        fid = art_id(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, ANIM_FALL_FRONT, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (!art_exists(fid)) {
            anim = ANIM_FALL_BACK;
        }
    }

    return anim;
}

// 0x412A6C
int action_explode(int tile, int elevation, int minDamage, int maxDamage, Object* a5, bool premature)
{
    if (premature && action_in_explode) {
        return -2;
    }

    Attack* attack = (Attack*)mem_malloc(sizeof(*attack));
    if (attack == NULL) {
        return -1;
    }

    Object* explosion;
    int fid = art_id(OBJ_TYPE_MISC, 10, 0, 0, 0);
    if (obj_new(&explosion, fid, -1) == -1) {
        mem_free(attack);
        return -1;
    }

    obj_turn_off(explosion, NULL);
    explosion->flags |= OBJECT_NO_SAVE;

    obj_move_to_tile(explosion, tile, elevation, NULL);

    Object* adjacentExplosions[ROTATION_COUNT];
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int fid = art_id(OBJ_TYPE_MISC, 10, 0, 0, 0);
        if (obj_new(&(adjacentExplosions[rotation]), fid, -1) == -1) {
            while (--rotation >= 0) {
                obj_erase_object(adjacentExplosions[rotation], NULL);
            }

            obj_erase_object(explosion, NULL);
            mem_free(attack);
            return -1;
        }

        obj_turn_off(adjacentExplosions[rotation], NULL);
        adjacentExplosions[rotation]->flags |= OBJECT_NO_SAVE;

        int adjacentTile = tile_num_in_direction(tile, rotation, 1);
        obj_move_to_tile(adjacentExplosions[rotation], adjacentTile, elevation, NULL);
    }

    Object* critter = obj_blocking_at(NULL, tile, elevation);
    if (critter != NULL) {
        if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER || (critter->data.critter.combat.results & DAM_DEAD) != 0) {
            critter = NULL;
        }
    }

    combat_ctd_init(attack, explosion, critter, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);

    attack->tile = tile;
    attack->attackerFlags = DAM_HIT;

    game_ui_disable(1);

    if (critter != NULL) {
        if (register_clear(critter) == -2) {
            debug_printf("Cannot clear target's animation for action_explode!\n");
        }
        attack->defenderDamage = compute_explosion_damage(minDamage, maxDamage, critter, &(attack->defenderKnockback));
    }

    compute_explosion_on_extras(attack, 0, 0, 1);

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* critter = attack->extras[index];
        if (register_clear(critter) == -2) {
            debug_printf("Cannot clear extra's animation for action_explode!\n");
        }

        attack->extrasDamage[index] = compute_explosion_damage(minDamage, maxDamage, critter, &(attack->extrasKnockback[index]));
    }

    death_checks(attack);

    if (premature) {
        action_in_explode = true;

        register_begin(ANIMATION_REQUEST_RESERVED);
        register_priority(1);
        register_object_play_sfx(explosion, "whn1xxx1", 0);
        register_object_funset(explosion, OBJECT_HIDDEN, 0);
        register_object_animate_and_hide(explosion, ANIM_STAND, 0);
        show_damage(attack, 0, 1);

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            register_object_funset(adjacentExplosions[rotation], OBJECT_HIDDEN, 0);
            register_object_animate_and_hide(adjacentExplosions[rotation], ANIM_STAND, 0);
        }

        register_object_must_call(explosion, 0, (AnimationCallback*)combat_explode_scenery, -1);
        register_object_must_erase(explosion);

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            register_object_must_erase(adjacentExplosions[rotation]);
        }

        register_object_must_call(attack, a5, (AnimationCallback*)report_explosion, -1);
        register_object_must_call(NULL, NULL, (AnimationCallback*)finished_explosion, -1);
        if (register_end() == -1) {
            action_in_explode = false;

            obj_erase_object(explosion, NULL);

            for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                obj_erase_object(adjacentExplosions[rotation], NULL);
            }

            mem_free(attack);

            game_ui_enable();
            return -1;
        }

        show_damage_extras(attack);
    } else {
        if (critter != NULL) {
            if ((attack->defenderFlags & DAM_DEAD) != 0) {
                critter_kill(critter, -1, false);
            }
        }

        for (int index = 0; index < attack->extrasLength; index++) {
            if ((attack->extrasFlags[index] & DAM_DEAD) != 0) {
                critter_kill(attack->extras[index], -1, false);
            }
        }

        report_explosion(attack, a5);

        combat_explode_scenery(explosion, NULL);

        obj_erase_object(explosion, NULL);

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            obj_erase_object(adjacentExplosions[rotation], NULL);
        }
    }

    return 0;
}

// 0x412EBC
static int report_explosion(Attack* attack, Object* a2)
{
    bool mainTargetWasDead;
    if (attack->defender != NULL) {
        mainTargetWasDead = (attack->defender->data.critter.combat.results & DAM_DEAD) != 0;
    } else {
        mainTargetWasDead = false;
    }

    bool extrasWasDead[6];
    for (int index = 0; index < attack->extrasLength; index++) {
        extrasWasDead[index] = (attack->extras[index]->data.critter.combat.results & DAM_DEAD) != 0;
    }

    death_checks(attack);
    combat_display(attack);
    apply_damage(attack, false);

    Object* anyDefender = NULL;
    int xp = 0;
    if (a2 != NULL) {
        if (attack->defender != NULL && attack->defender != a2) {
            if ((attack->defender->data.critter.combat.results & DAM_DEAD) != 0) {
                if (a2 == obj_dude && !mainTargetWasDead) {
                    xp += critter_kill_exps(attack->defender);
                }
            } else {
                critter_set_who_hit_me(attack->defender, a2);
                anyDefender = attack->defender;
            }
        }

        for (int index = 0; index < attack->extrasLength; index++) {
            Object* critter = attack->extras[index];
            if (critter != a2) {
                if ((critter->data.critter.combat.results & DAM_DEAD) != 0) {
                    if (a2 == obj_dude && !extrasWasDead[index]) {
                        xp += critter_kill_exps(critter);
                    }
                } else {
                    critter_set_who_hit_me(critter, a2);

                    if (anyDefender == NULL) {
                        anyDefender = critter;
                    }
                }
            }
        }

        if (anyDefender != NULL) {
            if (!isInCombat()) {
                STRUCT_664980 combat;
                combat.attacker = anyDefender;
                combat.defender = a2;
                combat.actionPointsBonus = 0;
                combat.accuracyBonus = 0;
                combat.damageBonus = 0;
                combat.minDamage = 0;
                combat.maxDamage = INT_MAX;
                combat.field_1C = 0;
                scripts_request_combat(&combat);
            }
        }
    }

    mem_free(attack);
    game_ui_enable();

    if (a2 == obj_dude) {
        combat_give_exps(xp);
    }

    return 0;
}

// 0x413038
static int finished_explosion(Object* a1, Object* a2)
{
    action_in_explode = false;
    return 0;
}

// 0x413044
static int compute_explosion_damage(int min, int max, Object* a3, int* a4)
{
    int v5 = roll_random(min, max);
    int v7 = v5 - stat_level(a3, STAT_DAMAGE_THRESHOLD_EXPLOSION);
    if (v7 > 0) {
        v7 -= stat_level(a3, STAT_DAMAGE_RESISTANCE_EXPLOSION) * v7 / 100;
    }

    if (v7 < 0) {
        v7 = 0;
    }

    if (a4 != NULL) {
        if ((a3->flags & OBJECT_MULTIHEX) == 0) {
            *a4 = v7 / 10;
        }
    }

    return v7;
}

// 0x4130A8
int action_talk_to(Object* a1, Object* a2)
{
    if (a1 != obj_dude) {
        return -1;
    }

    if (FID_TYPE(a2->fid) != OBJ_TYPE_CRITTER) {
        return -1;
    }

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
        register_clear(obj_dude);
    }

    if (isInCombat()) {
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_move_to_object(a1, a2, a1->data.critter.combat.ap, 0);
    } else {
        register_begin(a1 == obj_dude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);

        if (obj_dist(a1, a2) >= 9 || combat_is_shot_blocked(a1, a1->tile, a2->tile, a2, NULL)) {
            register_object_run_to_object(a1, a2, -1, 0);
        }
    }

    register_object_must_call(a1, a2, (AnimationCallback*)can_talk_to, -1);
    register_object_call(a1, a2, (AnimationCallback*)talk_to, -1);
    return register_end();
}

// 0x413198
static int can_talk_to(Object* a1, Object* a2)
{
    MessageListItem messageListItem;

    if (combat_is_shot_blocked(a1, a1->tile, a2->tile, a2, NULL) || obj_dist(a1, a2) >= 9) {
        if (a1 == obj_dude) {
            // You cannot get there. (used in actions.c)
            messageListItem.num = 2000;
            if (message_search(&misc_message_file, &messageListItem)) {
                display_print(messageListItem.text);
            }
        }

        return -1;
    }

    return 0;
}

// 0x413200
static int talk_to(Object* a1, Object* a2)
{
    scripts_request_dialog(a2);
    return 0;
}

// 0x41320C
void action_dmg(int tile, int elevation, int minDamage, int maxDamage, int damageType, bool animated, bool bypassArmor)
{
    Attack* attack = (Attack*)mem_malloc(sizeof(*attack));
    if (attack == NULL) {
        return;
    }

    Object* attacker;
    if (obj_new(&attacker, FID_0x20001F5, -1) == -1) {
        mem_free(attack);
        return;
    }

    obj_turn_off(attacker, NULL);

    attacker->flags |= OBJECT_NO_SAVE;

    obj_move_to_tile(attacker, tile, elevation, NULL);

    Object* defender = obj_blocking_at(NULL, tile, elevation);
    combat_ctd_init(attack, attacker, defender, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
    attack->tile = tile;
    attack->attackerFlags = DAM_HIT;
    game_ui_disable(1);

    if (defender != NULL) {
        register_clear(defender);

        int damage;
        if (bypassArmor) {
            damage = maxDamage;
        } else {
            damage = compute_dmg_damage(minDamage, maxDamage, defender, &(attack->defenderKnockback), damageType);
        }

        attack->defenderDamage = damage;
    }

    death_checks(attack);

    if (animated) {
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_play_sfx(attacker, "whc1xxx1", 0);
        show_damage(attack, death_3[damageType], 0);
        register_object_must_call(attack, NULL, (AnimationCallback*)report_dmg, 0);
        register_object_must_erase(attacker);

        if (register_end() == -1) {
            obj_erase_object(attacker, NULL);
            mem_free(attack);
            return;
        }
    } else {
        if (defender != NULL) {
            if ((attack->defenderFlags & DAM_DEAD) != 0) {
                critter_kill(defender, -1, 1);
            }
        }

        // NOTE: Uninline.
        report_dmg(attack, NULL);

        obj_erase_object(attacker, NULL);
    }

    game_ui_enable();
}

// 0x4133B4
static int report_dmg(Attack* attack, Object* a2)
{
    combat_display(attack);
    apply_damage(attack, false);
    mem_free(attack);
    game_ui_enable();
    return 0;
}

// Calculate damage by applying threshold and resistances.
//
// 0x4133D8
static int compute_dmg_damage(int min_damage, int max_damage, Object* obj, int* knockback_distance, int damage_type)
{
    int damage;

    damage = roll_random(min_damage, max_damage) - stat_level(obj, STAT_DAMAGE_THRESHOLD + damage_type);
    if (damage > 0) {
        damage -= stat_level(obj, STAT_DAMAGE_RESISTANCE + damage_type) * damage / 100;
    }

    if (damage < 0) {
        damage = 0;
    }

    if (knockback_distance != NULL) {
        if ((obj->flags & OBJECT_MULTIHEX) == 0 && damage_type != DAMAGE_TYPE_ELECTRICAL) {
            *knockback_distance = damage / 10;
        }
    }

    return damage;
}

} // namespace fallout
