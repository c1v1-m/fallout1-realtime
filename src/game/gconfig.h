#ifndef FALLOUT_GAME_GCONFIG_H_
#define FALLOUT_GAME_GCONFIG_H_

#include "game/config.h"

namespace fallout {

// The file name of the main config file.
#define GAME_CONFIG_FILE_NAME "fallout.cfg"

#define GAME_CONFIG_SYSTEM_KEY "system"
#define GAME_CONFIG_PREFERENCES_KEY "preferences"
#define GAME_CONFIG_SOUND_KEY "sound"
#define GAME_CONFIG_MAPPER_KEY "mapper"
#define GAME_CONFIG_DEBUG_KEY "debug"

#define GAME_CONFIG_EXECUTABLE_KEY "executable"
#define GAME_CONFIG_MASTER_DAT_KEY "master_dat"
#define GAME_CONFIG_MASTER_PATCHES_KEY "master_patches"
#define GAME_CONFIG_CRITTER_DAT_KEY "critter_dat"
#define GAME_CONFIG_CRITTER_PATCHES_KEY "critter_patches"
#define GAME_CONFIG_PATCHES_KEY "patches"
#define GAME_CONFIG_LANGUAGE_KEY "language"
#define GAME_CONFIG_SCROLL_LOCK_KEY "scroll_lock"
#define GAME_CONFIG_INTERRUPT_WALK_KEY "interrupt_walk"
#define GAME_CONFIG_ART_CACHE_SIZE_KEY "art_cache_size"
#define GAME_CONFIG_COLOR_CYCLING_KEY "color_cycling"
#define GAME_CONFIG_CYCLE_SPEED_FACTOR_KEY "cycle_speed_factor"
#define GAME_CONFIG_HASHING_KEY "hashing"
#define GAME_CONFIG_SPLASH_KEY "splash"
#define GAME_CONFIG_FREE_SPACE_KEY "free_space"
#define GAME_CONFIG_TIMES_RUN_KEY "times_run"
#define GAME_CONFIG_GAME_DIFFICULTY_KEY "game_difficulty"
#define GAME_CONFIG_RUNNING_BURNING_GUY_KEY "running_burning_guy"
#define GAME_CONFIG_COMBAT_DIFFICULTY_KEY "combat_difficulty"
#define GAME_CONFIG_VIOLENCE_LEVEL_KEY "violence_level"
#define GAME_CONFIG_TARGET_HIGHLIGHT_KEY "target_highlight"
#define GAME_CONFIG_ITEM_HIGHLIGHT_KEY "item_highlight"
#define GAME_CONFIG_COMBAT_LOOKS_KEY "combat_looks"
#define GAME_CONFIG_COMBAT_MESSAGES_KEY "combat_messages"
#define GAME_CONFIG_COMBAT_TAUNTS_KEY "combat_taunts"
#define GAME_CONFIG_LANGUAGE_FILTER_KEY "language_filter"
#define GAME_CONFIG_RUNNING_KEY "running"
#define GAME_CONFIG_SUBTITLES_KEY "subtitles"
#define GAME_CONFIG_COMBAT_SPEED_KEY "combat_speed"
#define GAME_CONFIG_PLAYER_SPEED_KEY "player_speed"
// Global animation-speed multiplier (anim_speed_scale): scales every critter/object
// animation's per-frame duration symmetrically (dude and NPCs alike). < 1 = faster
// (0.8 ~= 20% snappier); 1.0 = stock. Read once into anim_speed_scale at anim_init.
#define GAME_CONFIG_ANIM_SPEED_KEY "anim_speed"
// WALK speed multiplier (move_speed_scale), applied to the walk animation only, on top of
// anim_speed above. RUN is deliberately NOT affected -- the run art is already about twice the
// walk, so the same multiplier there reads as a sprint. Expressed as a SPEED: 1.0 = stock, 1.5 =
// half again as fast, 0.8 = slower. Clamped 0.3..3.0 at anim_init. Symmetric for the dude and NPCs
// -- see the note in compute_tpf about why speeding only the player would break walk_dir_slowdown.
#define GAME_CONFIG_MOVE_SPEED_KEY "move_speed"
// Per-point AGILITY scaling of the weapon put-away / take-out animations (realtime only) ->
// anim_weapon_swap_stat_factor. Duration multiplier = 1 - (AGI - 5) * this. SYMMETRIC by design:
// the swap holds anim_busy, so it is throughput, not decoration -- see compute_tpf. 0 = off.
#define GAME_CONFIG_WEAPON_SWAP_STAT_FACTOR_KEY "weapon_swap_stat_factor"
// Realtime-combat per-stat scaling of ATTACK animation speed -> anim_attack_stat_factor
// at anim_init. Attack anims are excluded from anim_speed above and driven purely by the
// attacker's SPECIAL: melee (swing/thrust/punch/kick) by STRENGTH, ranged (fire) by
// PERCEPTION. Duration multiplier = 1 - (stat - 5) * this (clamped 0.4..2.0); higher
// factor = bigger spread. 0.09: stat 1 = 1.36x (slow), 5 = 1.0x, 10 = 0.55x (fast).
#define GAME_CONFIG_ANIM_ATTACK_STAT_FACTOR_KEY "anim_attack_stat_factor"
// Realtime-combat AP regen interval (seconds per +1 AP) -> rt_ap_regen_seconds at
// combat_rt_init. Lower = faster firing cadence (0.7 ~= 3.5s between 5-AP shots).
#define GAME_CONFIG_AP_REGEN_KEY "ap_regen"
// How hard AGILITY scales the AP refill rate in realtime combat. interval = ap_regen *
// (1 - (AGI - 5) * factor). 0 = the old flat rate for everyone; 0.09 puts AGI 1 vs 10 at ~2.5x,
// which is roughly vanilla's spread in action points per TURN. See rt_regen_interval.
#define GAME_CONFIG_AP_REGEN_AGILITY_FACTOR_KEY "ap_regen_agility_factor"
// Minimum seconds between one critter's OWN attacks -- its physical rate of fire, as opposed to AP
// (the resource). 0 disables it entirely, restoring the previous behaviour. Scaled per critter by
// the stat that owns the attack: STR melee, PE ranged, AGI thrown. See rt_attack_cooldown.
#define GAME_CONFIG_ATTACK_COOLDOWN_BASE_KEY "attack_cooldown_base"
#define GAME_CONFIG_ATTACK_COOLDOWN_STAT_FACTOR_KEY "attack_cooldown_stat_factor"
// Realtime-combat run cost: AP drained per second while running IN COMBAT (walking
// and out-of-combat running stay free) -> rt_run_ap_cost. Higher = sprint depletes
// faster, so running and shooting compete harder for the shared pool. 0 disables.
#define GAME_CONFIG_RUN_AP_COST_KEY "run_ap_cost"
// Realtime-combat Endurance scaling of the run cost: effective run cost = run_ap_cost
// - (Endurance * this), floored at a small minimum so running always costs something
// -> rt_run_ap_endurance_factor. Higher EN = cheaper sprint = runs longer before the
// pool empties, but a sprinter still eventually depletes (cost stays above regen).
#define GAME_CONFIG_RUN_AP_ENDURANCE_FACTOR_KEY "run_ap_endurance_factor"
// Realtime-combat run resume threshold: after the pool empties from running, the
// dude walks (free) until AP regenerates back to this many points, then can sprint
// again -> rt_run_resume_ap. Higher = longer walk between sprints (burst rhythm).
#define GAME_CONFIG_RUN_RESUME_AP_KEY "resume_ap"
// Realtime-combat walk cost (kiting nerf, Variant C2): AP drained per second while
// WALKING IN COMBAT -> rt_walk_ap_cost, in AP per second. Walking spends the same pool
// attacks come out of, so moving competes with shooting and kiting dries up your offense.
// Regen is NOT paused by walking -- what it does instead is switch to the moving proximity
// curve below. Out-of-combat walking stays free. 0 disables (walk free).
#define GAME_CONFIG_WALK_AP_COST_KEY "walk_ap_cost"
// Realtime-combat PROXIMITY REGEN (replaces the old stand_regen_mult standing bonus).
// AP regen is multiplied by a factor that depends on how far the critter is from its
// nearest enemy: regen_near_mult at contact, falling linearly to regen_far_mult at
// regen_falloff_tiles and beyond. Rewards fighting up close and still punishes kiting
// (you kite far away), WITHOUT taxing the act of closing distance -- which is what the
// standing bonus did, and why it structurally favoured ranged builds over melee.
// Applies to the dude and every NPC alike. Out of combat the multiplier is 1.0.
#define GAME_CONFIG_REGEN_NEAR_MULT_KEY "regen_near_mult"
#define GAME_CONFIG_REGEN_FAR_MULT_KEY "regen_far_mult"
#define GAME_CONFIG_REGEN_FALLOFF_TILES_KEY "regen_falloff_tiles"
// The SECOND proximity curve, used while the critter is WALKING or RUNNING (the pair above
// then applies only while standing). Same falloff distance, different endpoints: at contact
// moving regen is regen_near_mult_moving (default 0 = none at all, so in a real fight you
// either move or you attack -- vanilla's trade), while at range it is regen_far_mult_moving
// (default 1.0, so crossing open ground toward a distant enemy is not punished).
#define GAME_CONFIG_REGEN_NEAR_MULT_MOVING_KEY "regen_near_mult_moving"
#define GAME_CONFIG_REGEN_FAR_MULT_MOVING_KEY "regen_far_mult_moving"
// Does the moving curve apply to NPCs as well (1) or only to the dude (0, default)? The
// asymmetry is deliberate: a chasing melee NPC with zero regen while moving gets pinned at
// its one-attack reserve for the whole chase (proven by trace_2026-08-03_222723), and unlike
// the dude it has no rt_move_blocked stop to force it to stand and recover. Set to 1 to try
// full symmetry -- and watch for chasers that arrive and barely swing.
#define GAME_CONFIG_REGEN_MOVING_NPC_KEY "regen_moving_npc"
// How far the in-game "Combat Speed" slider (combat_speed, 0..50) may speed up the whole
// realtime AP economy -> rt_combat_speed_ap_max. At slider 0 the multiplier is 1.0 (stock);
// at slider 50 it is this value. It scales AP regen AND the walk/run drains by the SAME
// factor, for the dude and every NPC alike, so the balance between moving and shooting is
// unchanged -- only the wall-clock tempo of the fight. 1.0 disables the coupling entirely.
#define GAME_CONFIG_COMBAT_SPEED_AP_MAX_KEY "combat_speed_ap_max"
// Realtime-combat DT gate on the hit STAGGER: a hit interrupts a walking critter with a flinch
// only if its through-armor damage reaches this fraction of the defender's Damage Threshold for the
// weapon's damage type -> rt_flinch_dt_factor. Higher = armored critters keep advancing through more
// hits (only solid blows stagger them); 0 = disabled, every damaging hit flinches. Damage/knockdown/
// death are never gated. Unarmored critters (DT 0) always flinch on any damage.
#define GAME_CONFIG_FLINCH_DT_FACTOR_KEY "flinch_dt_factor"

// Realtime combat: how sharply SEQUENCE (= Perception x 2, Fallout's initiative) decides who lands
// the blow first when you and your target swing at each other in the same frame -> the exponent in
// rt_seq_initiative_power. The chance you strike first is seq^p / (seq^p + foeSeq^p).
//   1.0 = flat odds (10 vs 8 -> 56%)   2.0 = squared, the default (10 vs 8 -> 61%, 14 vs 6 -> 84%)
//   higher = Perception matters more; 0 = initiative off, a coin flip.
// Deliberately NOT a hard "highest Sequence always goes first": that let whoever had the edge
// stagger-lock the other out of ever swinging back.
#define GAME_CONFIG_SEQ_INITIATIVE_POWER_KEY "seq_initiative_power"
// Seconds the SLOWEST critter waits before its opening swing of an engagement, scaled by how far
// its SEQUENCE trails the dude's (a critter as quick as you swings at once). Breaks the lock-step
// opening volley that made every exchange alternate. 0 = off. See rt_initiative_delay.
#define GAME_CONFIG_INITIATIVE_SPAN_KEY "initiative_span"
// Seconds after a conversation closes during which NPCs may not open fire. ONE-SIDED: the player
// can shoot immediately, only the NPCs are held, so it buys a beat to read the room without taking
// the opening shot away from anyone who wants it. 0 = off.
#define GAME_CONFIG_POST_DIALOG_GRACE_KEY "post_dialog_grace"
// Seconds a knocked-down critter stays on the ground after its fall animation finishes, before it
// starts getting up. A DAMAGING hit while it is down restarts this, so shooting a downed enemy
// keeps it down. 0 = stand up the instant the fall ends. Scale it with attack_cooldown_base: the
// point is that a knockdown costs its victim roughly an attack cycle.
#define GAME_CONFIG_KNOCKDOWN_DWELL_KEY "knockdown_dwell"
// FLOOR (seconds) on how long a shooter's weapon still counts as SHOULDERED after a shot, during
// which the next shot skips the ANIM_POINT raise. The raise costs ~0.43 s of nothing-visible BEFORE
// the shot, so firing continuously pays it every round; this pays it once per burst instead.
//   < 0  never raise (the 2026-08-10 behaviour)
//     0  raise on every shot (plain vanilla)
//   > 0  the FLOOR, default 2.0
// ⚠️ The real hold is max(this, 1.5 x the attacker's OWN attack cooldown), so it tracks each
// critter's cadence. A flat value cannot work: measured, a 2.0 s hold cleared the player's 1.98 s
// cadence by 0.02 s and sat BELOW every NPC's, so any hesitation re-aimed and enemies re-aimed
// always. Raise this only to make the weapon stay up through longer LULLS than that.
// Affects LATENCY only, never rate of fire -- the cooldown clock starts at the swing, so the
// animation runs inside it. See combat_rt_weapon_is_up.
#define GAME_CONFIG_RANGED_POINT_HOLD_KEY "ranged_point_hold"
// How hard the attacker's WEAPON SKILL bends its attack cooldown -- the second, deliberately weaker
// term next to attack_cooldown_stat_factor. Spans about +-this much across the whole 0-200 skill
// range (0.15 => skill 200 is ~15% faster, skill 0 ~15% slower). 0 = off, which is vanilla: there,
// weapon skill affects to-hit only and never speed.
// ⚠️ KEEP IT WELL BELOW THE STAT FACTOR. SPECIAL is fixed at creation; skill climbs all game, so an
// equal weight would accelerate the player's rate of fire for the whole campaign and invalidate any
// pacing scheme calibrated at one point on that curve. Skill also already buys to-hit, so speed on
// top of it compounds.
#define GAME_CONFIG_ATTACK_COOLDOWN_SKILL_FACTOR_KEY "attack_cooldown_skill_factor"
// Strength of the concurrent-sound-effect limiter (see gsound_sfx_limit). Fallout plays every SFX
// at full volume because turn-based resolves one attack at a time; realtime stacks several within a
// few frames and they SUM. 0 = off (vanilla mixing), 0.25 default, 1.0 = heavy.
// At 0.25 three overlapping sounds play at 0.80 volume, five at 0.57, eight at 0.40. The first two
// overlapping sounds are always full volume, so a single shot is never quieter than before.
#define GAME_CONFIG_SFX_BURST_DUCK_KEY "sfx_burst_duck"
// Minimum AI.TXT AGGRESSION at which an armed neutral critter will join a fight it witnesses, to
// defend someone on its own team (see rt_guard_danger_source). 0 = off, and neutrals stay inert as
// they were -- which is what let a whole bar watch the Skulz murder Neal.
// Vanilla's own values separate cleanly: Loser 1, Cow 5, Careful/Peasant 10, Guard 20 | Guard
// Humorous 33, Guard Quiet 55, Shady Sands Guard 60, Bouncer 70, Raider 80. Default 20 = guards
// and tougher. Raise it to make only the really aggressive types wade in.
#define GAME_CONFIG_GUARD_JOIN_AGGRESSION_KEY "guard_join_aggression"
// THE MEDIC BEAT (see rt_try_medic). Restores vanilla's ai_check_drugs -- which realtime never
// runs, so no NPC ever used a Stimpak -- and extends it to treating a wounded ally in reach.
// 1 = on, 0 = nobody heals (previous behaviour exactly).
#define GAME_CONFIG_MEDIC_ENABLED_KEY "medic_enabled"
// Fraction of MAX hit points below which a critter counts as worth treating. Vanilla's threshold
// is 0.5 (ai_check_drugs' `bloodied`). Raise it and medics burn supplies on scratches; lower it and
// they leave it too late.
#define GAME_CONFIG_MEDIC_HP_FRACTION_KEY "medic_hp_fraction"
// How far (tiles) a medic will reach to treat an ALLY. It never WALKS to a patient -- see the note
// in rt_medic_patient -- so this is the whole of its reach.
#define GAME_CONFIG_MEDIC_ALLY_TILES_KEY "medic_ally_tiles"
// Seconds between one critter's own heals. The AP cost alone is not much of a brake at contact
// regen rates, and this is what stops a stocked medic from turning a fight into a stalemate.
#define GAME_CONFIG_MEDIC_COOLDOWN_KEY "medic_cooldown"
// Flight-recorder trace log (game_trace): 1 = write trace_<date>.log next to the exe
// with input/combat/anim/state events + a crash digest; 0 = disabled (all trace calls
// become no-ops). On by default during the realtime test phase.
#define GAME_CONFIG_TRACE_LOG_KEY "trace_log"
#define GAME_CONFIG_TEXT_BASE_DELAY_KEY "text_base_delay"
#define GAME_CONFIG_TEXT_LINE_DELAY_KEY "text_line_delay"
#define GAME_CONFIG_BRIGHTNESS_KEY "brightness"
#define GAME_CONFIG_MOUSE_SENSITIVITY_KEY "mouse_sensitivity"
#define GAME_CONFIG_INITIALIZE_KEY "initialize"
#define GAME_CONFIG_DEVICE_KEY "device"
#define GAME_CONFIG_PORT_KEY "port"
#define GAME_CONFIG_IRQ_KEY "irq"
#define GAME_CONFIG_DMA_KEY "dma"
#define GAME_CONFIG_SOUNDS_KEY "sounds"
#define GAME_CONFIG_MUSIC_KEY "music"
#define GAME_CONFIG_SPEECH_KEY "speech"
#define GAME_CONFIG_MASTER_VOLUME_KEY "master_volume"
#define GAME_CONFIG_MUSIC_VOLUME_KEY "music_volume"
#define GAME_CONFIG_SNDFX_VOLUME_KEY "sndfx_volume"
#define GAME_CONFIG_SPEECH_VOLUME_KEY "speech_volume"
#define GAME_CONFIG_CACHE_SIZE_KEY "cache_size"
#define GAME_CONFIG_MUSIC_PATH1_KEY "music_path1"
#define GAME_CONFIG_MUSIC_PATH2_KEY "music_path2"
#define GAME_CONFIG_DEBUG_SFXC_KEY "debug_sfxc"
#define GAME_CONFIG_MODE_KEY "mode"
#define GAME_CONFIG_SHOW_TILE_NUM_KEY "show_tile_num"
#define GAME_CONFIG_SHOW_SCRIPT_MESSAGES_KEY "show_script_messages"
#define GAME_CONFIG_SHOW_LOAD_INFO_KEY "show_load_info"
#define GAME_CONFIG_OUTPUT_MAP_DATA_INFO_KEY "output_map_data_info"
#define GAME_CONFIG_EXECUTABLE_KEY "executable"
#define GAME_CONFIG_OVERRIDE_LIBRARIAN_KEY "override_librarian"
#define GAME_CONFIG_USE_ART_NOT_PROTOS_KEY "use_art_not_protos"
#define GAME_CONFIG_REBUILD_PROTOS_KEY "rebuild_protos"
#define GAME_CONFIG_FIX_MAP_OBJECTS_KEY "fix_map_objects"
#define GAME_CONFIG_FIX_MAP_INVENTORY_KEY "fix_map_inventory"
#define GAME_CONFIG_IGNORE_REBUILD_ERRORS_KEY "ignore_rebuild_errors"
#define GAME_CONFIG_SHOW_PID_NUMBERS_KEY "show_pid_numbers"
#define GAME_CONFIG_SAVE_TEXT_MAPS_KEY "save_text_maps"
#define GAME_CONFIG_RUN_MAPPER_AS_GAME_KEY "run_mapper_as_game"
#define GAME_CONFIG_DEFAULT_F8_AS_GAME_KEY "default_f8_as_game"
#define GAME_CONFIG_PLAYER_SPEEDUP_KEY "player_speedup"

#define ENGLISH "english"
#define FRENCH "french"
#define GERMAN "german"
#define ITALIAN "italian"
#define SPANISH "spanish"

typedef enum GameDifficulty {
    GAME_DIFFICULTY_EASY,
    GAME_DIFFICULTY_NORMAL,
    GAME_DIFFICULTY_HARD,
} GameDifficulty;

typedef enum CombatDifficulty {
    COMBAT_DIFFICULTY_EASY,
    COMBAT_DIFFICULTY_NORMAL,
    COMBAT_DIFFICULTY_HARD,
} CombatDifficulty;

typedef enum ViolenceLevel {
    VIOLENCE_LEVEL_NONE,
    VIOLENCE_LEVEL_MINIMAL,
    VIOLENCE_LEVEL_NORMAL,
    VIOLENCE_LEVEL_MAXIMUM_BLOOD,
} ViolenceLevel;

typedef enum TargetHighlight {
    TARGET_HIGHLIGHT_OFF,
    TARGET_HIGHLIGHT_ON,
    TARGET_HIGHLIGHT_TARGETING_ONLY,
} TargetHighlight;

extern Config game_config;

bool gconfig_init(bool isMapper, int argc, char** argv);
bool gconfig_save();
bool gconfig_exit(bool shouldSave);

} // namespace fallout

#endif /* FALLOUT_GAME_GCONFIG_H_ */
