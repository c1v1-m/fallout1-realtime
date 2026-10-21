#include "game/autotest.h"

#include "game/anim.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <SDL.h>

#include "game/combat_rt.h"
#include "game/interact_prompt.h"
#include "game/critter.h"
#include "game/editor.h"
#include "game/game.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gdialog.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/scripts.h"
#include "game/worldmap.h"
#include "game/object.h"
#include "game/options.h"
#include "game/proto.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/winmain.h"

namespace fallout {

// --- THE SCENARIO FILE -----------------------------------------------------------------------------
//
// One command per line, `#` starts a comment. Tiles are map tiles (200 x 200 hex grid).
//
//   name <label>                       shown in the trace
//   map <FILE.MAP>                     required; an empty random-encounter map makes a clean arena
//   seed <n>                           the dice (default 1)
//   mapscript on                       keep the map's own script (off by default: an encounter map's
//                                      script spawns its own critters into the arena)
//   gvar <n> <value>                   a global variable set before the run (a quest part way, a code known)
//   mapvar <n> <value>                 one of the loaded map's own variables (its scripts' map_var)
//   cleararea <tiles>                  every other critter within that many tiles of the dude is removed
//   duration <seconds>                 hard stop (default 60)
//   dude tile <t> [rot <0-5>]
//   dude elevation <0-2>               the floor of the map (default: the map's entrance floor); the
//                                      enemies are spawned on the same floor
//   dude stat <ST|PE|EN|CH|IN|AG|LK> <value>
//   dude hp <n>                        maximum HP, as a higher-level character would have
//   dude skill <small_guns|big_guns|energy|unarmed|melee|throwing|first_aid|sneak|lockpick|steal|
//              traps|science|repair|speech|doctor> <level>
//   dude item <pid> [qty]
//   dude wield <pid>                   added and put in the active hand, loaded
//   dude armor <pid>
//   dude curhp <n>                     start with n hit points (after the maximum is set)
//   team <n>                           the team every hostile spawn joins (default: the first one's)
//   spawn <pid> <tile> [rot <r>] [wield <pid>] [item <pid> <qty>]... [hostile] [team <n>] [hp <n>] [empty]
//                                      `hostile` = an enemy that still has to SEE you before it comes
//   at <seconds> <policy> [tile]       the bot's plan; policies:
//        idle                          do nothing
//        fight                         hold LT, attack the locked enemy when it is in weapon range,
//                                      reload when empty, stimpak (LB) under 35 % HP
//        advance                       walk at the nearest enemy until in range, then fight
//        kite                          back away from anyone within 4 tiles, otherwise fight
//        goto <tile>                   walk there and wait (hide behind a wall, say)
//        shoot                         fight until one attack has gone off, then idle
//        throw                         hold LT for a lock, then press and let go of RB once (the RB
//                                      quick slot: a grenade by default)
//        throwfree                     press and let go of RB once with no lock
//        strafe                        run (X held) sideways to the nearest enemy, turning every 1.5 s,
//                                      never attacking -- for measuring what can be dodged
//        dodge                         stand (saving AP) and, the moment an attack at him is under way,
//                                      run sideways for 1.2 s -- a player who sees the shot coming
//        shootmove                     fight, and the moment his own attack is under way push the stick
//                                      sideways for 0.8 s (turning each time) -- shoot and move
//        heal                          press LB (the heal slot) once, then stand
//        use                           press A (the interact prompt's use) once, then stand
//        healhold                      hold LB down for the whole phase
//        rthold                        hold RT down for the whole phase with NO lock (no LT): the
//                                      unlocked attack picks its own target
//        aimother [tile]               hold LT and RT, and flick the RIGHT stick from the locked critter
//                                      toward the tile (or the nearest other enemy) every 0.6 s
//                                      (pre-aiming at one peeking round a corner)
//   reaction <seconds>                 the dodger's reaction time (default 0.3)
//   provoke                            every critter already on the dude's floor turns hostile (as a
//                                      base alarm does) -- it still has to see him before it comes
//   tactical on|off                    MODS "Tactical AI" for this run (default: as the cfg says)
//   projectiles on|off                 MODS "Projectiles" for this run
//   tracer <hexes> <r> <g> <b> [underline]   how a bullet looks (r g b 0-31) -- to photograph variants
//   ipdump <seconds>                   list every interactable in the picker's range and why it is (not) pickable
//   shots <n>                          up to n screenshots (screenshots/scrNNNNN.bmp beside the exe),
//                                      each taken while a bullet tracer is in the air, 1 s apart
//   projectile_speedup <x>             how much faster than its art a rocket / plasma / laser flies
//                                      (the game's own is 4)
//
// The run ends at `duration`, 2 s after the dude dies, or 3 s after the last spawned enemy does.
//
// SCREENS: THE PAD TIMELINE. Any of the lines below make the scenario a SCREEN test: the bot is off
// (its `at` plan is ignored -- one owner of the pad) and the virtual pad is pressed on a timeline
// instead. The timeline keeps running INSIDE modal screens (character editor, its age / sex / options
// windows, dialog boxes, inventory, Pip-Boy, file dialogs), which the bot cannot: it is a GNW
// background process, run by every get_input() on the main thread, not a tick of the game loop. Its
// clock is the WALL clock from the `start` line, in ms; `+<ms>` means that long after the previous
// timeline line. `duration` is a hard stop on the same clock, kept inside any screen.
//
//   pad <ms> <button> <0|1>            a button down (1) or up (0)
//   tap <ms> <button> [hold_ms]        down, and up again hold_ms later (default 120)
//   axis <ms> <axis> <value>           -32768..32767; a TRIGGER is released at -32768 and fully
//                                      pulled at 32767 (the pad starts centred, triggers released)
//   open <ms> <screen>                 open a screen THE WAY THE PAD DOES, from the world:
//        sheet                         D-pad down held 500 ms (the pad's hold is 350)
//        pipboy                        D-pad up held 500 ms (refused in combat, as for the player)
//        automap                       D-pad left held 500 ms
//        inventory                     BACK tapped (a hold past 450 ms runs for the exit instead)
//        options                       START tapped (a hold quick-saves instead)
//        creation                      character CREATION as the new-game selector's MODIFY opens it:
//                                      the menus are skipped here, so there is no pad route to it.
//                                      Opened by the game loop: the timeline waits until the world is
//                                      back on top, and the lines after it are timed from the open.
//   note <ms> <text...>                a marker in the trace
//   walkto <ms> <tile>                 the dude runs there along the pathfinder's route (a click's walk),
//                                      for the long way across a level; traced with its step count
//
//   buttons: a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown
//            dpleft dpright, or A B X Y BACK START L3 R3 LB RB UP DOWN LEFT RIGHT
//   axes:    leftx lefty rightx righty lefttrigger righttrigger, or LX LY RX RY LT RT
//
// Traced as [FLOW/autotest] pad / axis / open / opened / closed / note lines, and a `modal` line
// whenever the topmost modal window changes (its id, position and size -- which screen is up). The
// run.js report checks `expect <regex>` / `expect-not <regex>` / `expect-count <n> <regex>` lines
// against the trace (this parser ignores them; a regex cannot contain `#`, the comment sign).

#define AT_MAX_SPAWNS 32
#define AT_MAX_ITEMS 8
#define AT_MAX_PHASES 24

enum {
    AT_IDLE,
    AT_FIGHT,
    AT_ADVANCE,
    AT_KITE,
    AT_GOTO,
    AT_SHOOT,
    AT_THROW,
    AT_THROW_FREE,
    AT_STRAFE,
    AT_DODGE,
    AT_SHOOT_MOVE,
    AT_HEAL,
    AT_USE,
    AT_HEAL_HOLD,
    AT_RT_HOLD,
    AT_AIM_OTHER,
};

static const char* const at_policy_names[] = { "idle", "fight", "advance", "kite", "goto", "shoot", "throw", "throwfree", "strafe", "dodge", "shootmove", "heal", "use", "healhold", "rthold", "aimother" };

typedef struct AtItem {
    int pid;
    int qty;
} AtItem;

typedef struct AtSpawn {
    int pid;
    int tile;
    int rotation;
    int wield;
    AtItem items[AT_MAX_ITEMS];
    int itemCount;
    bool hostile;
    int team;
    int curhp; // "hp <n>": start wounded (0 = full)
    bool empty; // "empty": the wielded gun starts with no rounds in it
    // "stat <ST..LK> <n>": ⚠️ sets the PROTO's base stat (stat_set_base on a critter writes its proto),
    // so every critter of that pid gets it -- give the one under test a pid of its own.
    int statIndex[4];
    int statValue[4];
    int statCount;
    Object* obj;
} AtSpawn;

typedef struct AtPhase {
    float at;
    int policy;
    int tile;
} AtPhase;

static bool at_active = false;
static char at_name[64] = "unnamed";
static char at_map_name[64] = "";
static char at_file[260] = "";
static int at_seed = 1;
static bool at_map_script = false;
static float at_projectile_speedup = -1.0f;
static int at_tactical = -1;
static int at_projectiles = -1;
static int at_shots_wanted = 0;
static float at_ipdump_at = -1.0f; // scenario "ipdump": when to list the interact candidates
static int at_shots_taken = 0;
static float at_shot_next = 0.0f;
static float at_duration = 60.0f;
static int at_dude_tile = -1;
static int at_dude_rotation = -1;
static int at_dude_elevation = -1;
// "gvar <n> <value>": global variables set before the run starts (a quest's state, a code the
// character already knows), so a scenario can start partway through a quest.
#define AT_MAX_GVARS 16
static int at_gvar_index[AT_MAX_GVARS];
static int at_gvar_value[AT_MAX_GVARS];
static int at_gvar_count = 0;
// "mapvar <n> <value>": the loaded map's own variables (map_var in its scripts), e.g. a town's grudge.
static int at_mvar_index[AT_MAX_GVARS];
static int at_mvar_value[AT_MAX_GVARS];
static int at_mvar_count = 0;
// "cleararea <tiles>": every other critter that close to the dude is taken off the map at the start
// (a quest step tested on its own: the guards at a door are a fight, not the door).
static int at_clear_radius = -1;
static bool at_provoke = false;
static int at_dude_stats[7] = { -1, -1, -1, -1, -1, -1, -1 };
static int at_dude_skills[SKILL_COUNT];
static AtItem at_dude_items[AT_MAX_ITEMS];
static int at_dude_item_count = 0;
static int at_dude_wield = -1;
static int at_dude_hp = -1;
static int at_dude_curhp = -1; // "dude curhp": start wounded
static int at_dude_armor = -1;
static AtSpawn at_spawns[AT_MAX_SPAWNS];
static int at_spawn_count = 0;
static int at_team = -1; // the team every hostile spawn joins (-1 = the first hostile spawn's own)
static AtPhase at_phases[AT_MAX_PHASES];
static int at_phase_count = 0;

// The pad timeline (see SCREENS above).
#define AT_MAX_STEPS 512
#define AT_MAX_BAD_LINES 8

enum {
    AT_STEP_BUTTON, // index = button, value = 0/1
    AT_STEP_AXIS, // index = axis, value = raw
    AT_STEP_PAD_OPEN, // a pad-route open: traces the open, then presses `index` (its release is a BUTTON step)
    AT_STEP_OPEN, // index = AT_SCREEN_*: a screen the game loop opens itself (no pad route to it)
    AT_STEP_NOTE,
    AT_STEP_WALKTO, // index = tile: the dude is sent there along the pathfinder's route, as a click sends him
};

enum {
    AT_SCREEN_SHEET,
    AT_SCREEN_PIPBOY,
    AT_SCREEN_AUTOMAP,
    AT_SCREEN_INVENTORY,
    AT_SCREEN_OPTIONS,
    AT_SCREEN_CREATION,
};

// How the pad opens each screen from the world (gamepad_ui.cc: the D-pad holds, BACK / START taps).
// ⚠️ IF THE LAYOUT MOVES ONE OF THESE, THIS TABLE MOVES WITH IT -- the trace's `modal` line is what
// says the screen really came up.
static const struct {
    const char* name;
    int button; // -1: no pad route from the world, autotest_tick opens it
    int holdMs;
} at_screens[] = {
    { "sheet", SDL_CONTROLLER_BUTTON_DPAD_DOWN, 500 },
    { "pipboy", SDL_CONTROLLER_BUTTON_DPAD_UP, 500 },
    { "automap", SDL_CONTROLLER_BUTTON_DPAD_LEFT, 500 },
    { "inventory", SDL_CONTROLLER_BUTTON_BACK, 120 },
    { "options", SDL_CONTROLLER_BUTTON_START, 120 },
    { "creation", -1, 0 },
};

typedef struct AtStep {
    int ms; // timeline time
    int order; // line order, so equal times keep the file's order
    int kind;
    int index;
    int value;
    char text[64]; // a note's text
} AtStep;

static AtStep at_steps[AT_MAX_STEPS];
static int at_step_count = 0;
static int at_step_last_ms = 0; // the previous timeline line's time, for `+<ms>`
static char at_bad_lines[AT_MAX_BAD_LINES][80]; // reported once the trace is up
static int at_bad_line_count = 0;

// run state
static SDL_Joystick* at_joy = NULL;
static int at_joy_index = -1;
static float at_elapsed = 0.0f;
static int at_phase = -1;
static float at_end_at = -1.0f;
static const char* at_end_why = NULL;
static bool at_done = false;
static int at_tap_frames[SDL_CONTROLLER_BUTTON_MAX];
static int at_tap_rest[SDL_CONTROLLER_BUTTON_MAX];
static int at_rt_frames = 0;
static int at_rt_rest = 0;
static float at_rt_next = 0.0f;
static float at_relock_next = 0.0f;
static int at_lt_release = 0;
static float at_reload_next = 0.0f;
static float at_heal_next = 0.0f;
static int at_shots = 0;
static int at_goto_best = 9999;
static float at_goto_since = 0.0f;
static bool at_goto_done = false;
static float at_throw_at = -1.0f;
static float at_strafe_flip = 0.0f;
static int at_strafe_dir = 1;
static bool at_was_targeted = false;
static float at_dodge_until = 0.0f;
static float at_dodge_from = 0.0f;
static float at_reaction = 0.3f; // a player notices the wind-up this late (scenario "reaction")
static bool at_was_attacking = false;
static float at_move_until = 0.0f;

// timeline run state
static bool at_tl_running = false;
static Uint32 at_tl_t0 = 0; // SDL_GetTicks() at the start line
static int at_tl_shift = 0; // ms the timeline waited for a game-loop open
static int at_tl_next = 0;
static int at_tl_open = -1; // the AT_STEP_OPEN step waiting for autotest_tick, or -1
static int at_tl_modal = -2; // the topmost modal window last traced (-2 = none traced yet)

static void at_trim(char* s)
{
    char* p = strchr(s, '#');
    if (p != NULL) {
        *p = '\0';
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static int at_stat_index(const char* s)
{
    static const char* const names[7] = { "ST", "PE", "EN", "CH", "IN", "AG", "LK" };
    for (int i = 0; i < 7; i++) {
        if (SDL_strcasecmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int at_skill_index(const char* s)
{
    static const struct {
        const char* name;
        int skill;
    } names[] = {
        { "small_guns", SKILL_SMALL_GUNS },
        { "big_guns", SKILL_BIG_GUNS },
        { "energy", SKILL_ENERGY_WEAPONS },
        { "unarmed", SKILL_UNARMED },
        { "melee", SKILL_MELEE_WEAPONS },
        { "throwing", SKILL_THROWING },
        { "first_aid", SKILL_FIRST_AID },
        { "sneak", SKILL_SNEAK },
        { "lockpick", SKILL_LOCKPICK },
        { "steal", SKILL_STEAL },
        { "traps", SKILL_TRAPS },
        { "science", SKILL_SCIENCE },
        { "repair", SKILL_REPAIR },
        { "speech", SKILL_SPEECH },
        { "doctor", SKILL_DOCTOR },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (SDL_strcasecmp(s, names[i].name) == 0) {
            return names[i].skill;
        }
    }
    return -1;
}

static int at_policy_index(const char* s)
{
    for (int i = 0; i < (int)(sizeof(at_policy_names) / sizeof(at_policy_names[0])); i++) {
        if (SDL_strcasecmp(s, at_policy_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// The F2 pad test's short names first (rt_f2_hooks.cc), then SDL's own.
static int at_button_index(const char* s)
{
    static const struct {
        const char* name;
        int button;
    } names[] = {
        { "A", SDL_CONTROLLER_BUTTON_A },
        { "B", SDL_CONTROLLER_BUTTON_B },
        { "X", SDL_CONTROLLER_BUTTON_X },
        { "Y", SDL_CONTROLLER_BUTTON_Y },
        { "BACK", SDL_CONTROLLER_BUTTON_BACK },
        { "START", SDL_CONTROLLER_BUTTON_START },
        { "L3", SDL_CONTROLLER_BUTTON_LEFTSTICK },
        { "R3", SDL_CONTROLLER_BUTTON_RIGHTSTICK },
        { "LB", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
        { "RB", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
        { "UP", SDL_CONTROLLER_BUTTON_DPAD_UP },
        { "DOWN", SDL_CONTROLLER_BUTTON_DPAD_DOWN },
        { "LEFT", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
        { "RIGHT", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (SDL_strcasecmp(s, names[i].name) == 0) {
            return names[i].button;
        }
    }
    int button = SDL_GameControllerGetButtonFromString(s);
    // The virtual pad has buttons up to the D-pad only (see autotest_setup_world).
    return button >= 0 && button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? button : -1;
}

static int at_axis_index(const char* s)
{
    static const struct {
        const char* name;
        int axis;
    } names[] = {
        { "LX", SDL_CONTROLLER_AXIS_LEFTX },
        { "LY", SDL_CONTROLLER_AXIS_LEFTY },
        { "RX", SDL_CONTROLLER_AXIS_RIGHTX },
        { "RY", SDL_CONTROLLER_AXIS_RIGHTY },
        { "LT", SDL_CONTROLLER_AXIS_TRIGGERLEFT },
        { "RT", SDL_CONTROLLER_AXIS_TRIGGERRIGHT },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (SDL_strcasecmp(s, names[i].name) == 0) {
            return names[i].axis;
        }
    }
    return SDL_GameControllerGetAxisFromString(s);
}

static int at_screen_index(const char* s)
{
    for (int i = 0; i < (int)(sizeof(at_screens) / sizeof(at_screens[0])); i++) {
        if (SDL_strcasecmp(s, at_screens[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

// "1500" = 1500 ms after the start line, "+500" = 500 ms after the previous timeline line.
static int at_step_time(const char* s)
{
    int ms = s[0] == '+' ? at_step_last_ms + atoi(s + 1) : atoi(s);
    at_step_last_ms = ms;
    return ms;
}

static void at_bad_line(const char* what, const char* line)
{
    if (at_bad_line_count < AT_MAX_BAD_LINES) {
        snprintf(at_bad_lines[at_bad_line_count++], sizeof(at_bad_lines[0]), "%s: %s", what, line);
    }
}

static AtStep* at_add_step(int ms, int kind, int index, int value)
{
    if (at_step_count >= AT_MAX_STEPS) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            at_bad_line("timeline full", "the rest of the file is dropped");
        }
        return NULL;
    }
    AtStep* st = &(at_steps[at_step_count]);
    memset(st, 0, sizeof(*st));
    st->ms = ms;
    st->order = at_step_count;
    st->kind = kind;
    st->index = index;
    st->value = value;
    at_step_count++;
    return st;
}

static int at_step_compare(const void* a, const void* b)
{
    const AtStep* sa = (const AtStep*)a;
    const AtStep* sb = (const AtStep*)b;
    if (sa->ms != sb->ms) {
        return sa->ms < sb->ms ? -1 : 1;
    }
    return sa->order - sb->order;
}

// The route a player could walk once he has opened the doors on it: walls and scenery block, a door
// or a critter does not (walkto's "steps without doors").
static Object* at_block_but_doors(Object* obj, int tile, int elevation)
{
    Object* blocker = obj_blocking_at(obj, tile, elevation);
    if (blocker == NULL || FID_TYPE(blocker->fid) == OBJ_TYPE_CRITTER) {
        return NULL;
    }
    if (FID_TYPE(blocker->fid) == OBJ_TYPE_SCENERY) {
        Proto* proto;
        if (proto_ptr(blocker->pid, &proto) == 0 && proto->scenery.type == SCENERY_TYPE_DOOR) {
            return NULL;
        }
    }
    return blocker;
}

// One timeline line, already split into tokens. False when it is not a timeline command at all.
static bool at_parse_step(char** tok, int n)
{
    bool button = strcmp(tok[0], "pad") == 0;
    bool tap = strcmp(tok[0], "tap") == 0;
    bool axis = strcmp(tok[0], "axis") == 0;
    bool open = strcmp(tok[0], "open") == 0;
    bool note = strcmp(tok[0], "note") == 0;
    bool walkto = strcmp(tok[0], "walkto") == 0;
    if (!button && !tap && !axis && !open && !note && !walkto) {
        return false;
    }
    if (n < 3 || ((button || axis) && n < 4)) {
        at_bad_line("too short", tok[0]);
        return true;
    }

    int ms = at_step_time(tok[1]);
    if (walkto) {
        at_add_step(ms, AT_STEP_WALKTO, atoi(tok[2]), 0);
        return true;
    }
    if (button || tap) {
        int index = at_button_index(tok[2]);
        if (index < 0) {
            at_bad_line("unknown button", tok[2]);
        } else if (button) {
            at_add_step(ms, AT_STEP_BUTTON, index, atoi(tok[3]) != 0 ? 1 : 0);
        } else {
            int hold = n >= 4 ? atoi(tok[3]) : 120;
            at_add_step(ms, AT_STEP_BUTTON, index, 1);
            at_add_step(ms + (hold > 0 ? hold : 120), AT_STEP_BUTTON, index, 0);
        }
    } else if (axis) {
        int index = at_axis_index(tok[2]);
        if (index < 0) {
            at_bad_line("unknown axis", tok[2]);
        } else {
            int value = atoi(tok[3]);
            at_add_step(ms, AT_STEP_AXIS, index, value < -32768 ? -32768 : (value > 32767 ? 32767 : value));
        }
    } else if (open) {
        int screen = at_screen_index(tok[2]);
        if (screen < 0) {
            at_bad_line("unknown screen", tok[2]);
        } else if (at_screens[screen].button < 0) {
            at_add_step(ms, AT_STEP_OPEN, screen, 0);
        } else {
            at_add_step(ms, AT_STEP_PAD_OPEN, screen, 0);
            at_add_step(ms + at_screens[screen].holdMs, AT_STEP_BUTTON, at_screens[screen].button, 0);
        }
    } else {
        AtStep* st = at_add_step(ms, AT_STEP_NOTE, 0, 0);
        if (st != NULL) {
            for (int i = 2; i < n; i++) {
                if (i > 2) {
                    SDL_strlcat(st->text, " ", sizeof(st->text));
                }
                SDL_strlcat(st->text, tok[i], sizeof(st->text));
            }
        }
    }
    return true;
}

static bool at_parse(const char* path)
{
    FILE* f = fopen(path, "rt");
    if (f == NULL) {
        return false;
    }
    for (int i = 0; i < SKILL_COUNT; i++) {
        at_dude_skills[i] = -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        at_trim(line);
        char* tok[24];
        int n = 0;
        for (char* t = strtok(line, " \t\r\n"); t != NULL && n < 24; t = strtok(NULL, " \t\r\n")) {
            tok[n++] = t;
        }
        if (n == 0) {
            continue;
        }

        if (strcmp(tok[0], "name") == 0 && n >= 2) {
            SDL_strlcpy(at_name, tok[1], sizeof(at_name));
        } else if (strcmp(tok[0], "map") == 0 && n >= 2) {
            SDL_strlcpy(at_map_name, tok[1], sizeof(at_map_name));
        } else if (strcmp(tok[0], "team") == 0 && n >= 2) {
            at_team = atoi(tok[1]);
        } else if (strcmp(tok[0], "reaction") == 0 && n >= 2) {
            at_reaction = (float)atof(tok[1]);
        } else if (strcmp(tok[0], "provoke") == 0) {
            at_provoke = true;
        } else if (strcmp(tok[0], "tactical") == 0 && n >= 2) {
            at_tactical = strcmp(tok[1], "off") == 0 ? 0 : 1;
        } else if (strcmp(tok[0], "projectiles") == 0 && n >= 2) {
            at_projectiles = strcmp(tok[1], "off") == 0 ? 0 : 1;
        } else if (strcmp(tok[0], "ipdump") == 0 && n >= 2) {
            at_ipdump_at = (float)atof(tok[1]);
        } else if (strcmp(tok[0], "tracer") == 0 && n >= 5) {
            combat_rt_set_tracer_look((float)atof(tok[1]), atoi(tok[2]), atoi(tok[3]), atoi(tok[4]), n >= 6 && strcmp(tok[5], "underline") == 0);
        } else if (strcmp(tok[0], "shots") == 0 && n >= 2) {
            at_shots_wanted = atoi(tok[1]);
        } else if (strcmp(tok[0], "projectile_speedup") == 0 && n >= 2) {
            at_projectile_speedup = (float)atof(tok[1]);
        } else if (strcmp(tok[0], "mapscript") == 0 && n >= 2) {
            at_map_script = strcmp(tok[1], "on") == 0;
        } else if (strcmp(tok[0], "cleararea") == 0 && n >= 2) {
            at_clear_radius = atoi(tok[1]);
        } else if (strcmp(tok[0], "mapvar") == 0 && n >= 3 && at_mvar_count < AT_MAX_GVARS) {
            at_mvar_index[at_mvar_count] = atoi(tok[1]);
            at_mvar_value[at_mvar_count] = atoi(tok[2]);
            at_mvar_count++;
        } else if (strcmp(tok[0], "gvar") == 0 && n >= 3 && at_gvar_count < AT_MAX_GVARS) {
            at_gvar_index[at_gvar_count] = atoi(tok[1]);
            at_gvar_value[at_gvar_count] = atoi(tok[2]);
            at_gvar_count++;
        } else if (strcmp(tok[0], "seed") == 0 && n >= 2) {
            at_seed = atoi(tok[1]);
        } else if (strcmp(tok[0], "duration") == 0 && n >= 2) {
            at_duration = (float)atof(tok[1]);
        } else if (strcmp(tok[0], "dude") == 0 && n >= 3) {
            if (strcmp(tok[1], "elevation") == 0) {
                at_dude_elevation = atoi(tok[2]);
            } else if (strcmp(tok[1], "tile") == 0) {
                at_dude_tile = atoi(tok[2]);
                if (n >= 5 && strcmp(tok[3], "rot") == 0) {
                    at_dude_rotation = atoi(tok[4]);
                }
            } else if (strcmp(tok[1], "stat") == 0 && n >= 4) {
                int s = at_stat_index(tok[2]);
                if (s >= 0) {
                    at_dude_stats[s] = atoi(tok[3]);
                }
            } else if (strcmp(tok[1], "skill") == 0 && n >= 4) {
                int s = at_skill_index(tok[2]);
                if (s >= 0) {
                    at_dude_skills[s] = atoi(tok[3]);
                }
            } else if (strcmp(tok[1], "item") == 0 && at_dude_item_count < AT_MAX_ITEMS) {
                at_dude_items[at_dude_item_count].pid = atoi(tok[2]);
                at_dude_items[at_dude_item_count].qty = n >= 4 ? atoi(tok[3]) : 1;
                at_dude_item_count++;
            } else if (strcmp(tok[1], "hp") == 0) {
                at_dude_hp = atoi(tok[2]);
            } else if (strcmp(tok[1], "curhp") == 0) {
                at_dude_curhp = atoi(tok[2]);
            } else if (strcmp(tok[1], "wield") == 0) {
                at_dude_wield = atoi(tok[2]);
            } else if (strcmp(tok[1], "armor") == 0) {
                at_dude_armor = atoi(tok[2]);
            }
        } else if (strcmp(tok[0], "spawn") == 0 && n >= 3 && at_spawn_count < AT_MAX_SPAWNS) {
            AtSpawn* sp = &(at_spawns[at_spawn_count++]);
            memset(sp, 0, sizeof(*sp));
            sp->pid = atoi(tok[1]);
            sp->tile = atoi(tok[2]);
            sp->rotation = -1;
            sp->wield = -1;
            sp->team = -1;
            for (int i = 3; i < n; i++) {
                if (strcmp(tok[i], "rot") == 0 && i + 1 < n) {
                    sp->rotation = atoi(tok[++i]);
                } else if (strcmp(tok[i], "wield") == 0 && i + 1 < n) {
                    sp->wield = atoi(tok[++i]);
                } else if (strcmp(tok[i], "item") == 0 && i + 2 < n && sp->itemCount < AT_MAX_ITEMS) {
                    sp->items[sp->itemCount].pid = atoi(tok[i + 1]);
                    sp->items[sp->itemCount].qty = atoi(tok[i + 2]);
                    sp->itemCount++;
                    i += 2;
                } else if (strcmp(tok[i], "hostile") == 0) {
                    sp->hostile = true;
                } else if (strcmp(tok[i], "team") == 0 && i + 1 < n) {
                    sp->team = atoi(tok[++i]);
                } else if (strcmp(tok[i], "hp") == 0 && i + 1 < n) {
                    sp->curhp = atoi(tok[++i]);
                } else if (strcmp(tok[i], "empty") == 0) {
                    sp->empty = true;
                } else if (strcmp(tok[i], "stat") == 0 && i + 2 < n && sp->statCount < 4) {
                    int st = at_stat_index(tok[i + 1]);
                    if (st >= 0) {
                        sp->statIndex[sp->statCount] = st;
                        sp->statValue[sp->statCount] = atoi(tok[i + 2]);
                        sp->statCount++;
                    }
                    i += 2;
                }
            }
        } else if (at_parse_step(tok, n)) {
            // a timeline line
        } else if (strcmp(tok[0], "at") == 0 && n >= 3 && at_phase_count < AT_MAX_PHASES) {
            int policy = at_policy_index(tok[2]);
            if (policy >= 0) {
                AtPhase* ph = &(at_phases[at_phase_count++]);
                ph->at = (float)atof(tok[1]);
                ph->policy = policy;
                ph->tile = n >= 4 ? atoi(tok[3]) : -1;
            }
        }
    }
    fclose(f);
    qsort(at_steps, at_step_count, sizeof(at_steps[0]), at_step_compare);
    return at_map_name[0] != '\0';
}

bool autotest_init(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autotest") == 0 && i + 1 < argc) {
            SDL_strlcpy(at_file, argv[i + 1], sizeof(at_file));
            if (!at_parse(at_file)) {
                fprintf(stderr, "autotest: cannot read scenario %s\n", at_file);
                exit(2);
            }
            at_active = true;
        } else if (strcmp(argv[i], "--autotest-visible") == 0) {
            gnw_visible_override = true;
        }
    }
    if (at_active) {
        gnw_headless = !gnw_visible_override;
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    return at_active;
}

bool autotest_active()
{
    return at_active;
}

char* autotest_map()
{
    return at_map_name;
}

int autotest_seed()
{
    return at_seed;
}

bool autotest_map_script_allowed()
{
    return !at_active || at_map_script;
}

int autotest_controller_index()
{
    return at_active ? at_joy_index : -1;
}

static Object* at_give(Object* owner, int pid, int qty)
{
    Object* item;
    if (obj_pid_new(&item, pid) != 0) {
        trace_event(TRACE_WARN, "autotest", "cannot create pid=%d", pid);
        return NULL;
    }
    if (item_add_force(owner, item, qty > 0 ? qty : 1) != 0) {
        trace_event(TRACE_WARN, "autotest", "cannot give pid=%d to %d", pid, owner->pid);
        return NULL;
    }
    return item;
}

static void at_wield(Object* critter, int pid, int hand)
{
    Object* item = at_give(critter, pid, 1);
    if (item == NULL) {
        return;
    }
    if (item_get_type(item) == ITEM_TYPE_WEAPON) {
        int max = item_w_max_ammo(item);
        if (max > 0) {
            item_w_set_curr_ammo(item, max);
        }
    }
    inven_wield(critter, item, hand);

    // A critter whose art has no animation for that weapon cannot hold it (a super mutant and a
    // hunting rifle): the scenario is wrong, and the trace has to say so loudly.
    Object* held = hand == HAND_RIGHT ? inven_right_hand(critter) : inven_left_hand(critter);
    if (held != item) {
        trace_event(TRACE_WARN, "autotest", "wield-failed critter=%d pid=%d weapon=%d -- this critter cannot hold it",
            critter->id, critter->pid, pid);
    }
}

static void at_tl_start();

void autotest_setup_world()
{
    if (!at_active || obj_dude == NULL) {
        return;
    }

    // The virtual pad. gamepad_ui opens this index instead of any real controller.
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.name = "Autotest pad";
    at_joy_index = SDL_JoystickAttachVirtualEx(&desc);
    at_joy = at_joy_index >= 0 ? SDL_JoystickOpen(at_joy_index) : NULL;
    if (at_joy == NULL) {
        trace_event(TRACE_ERR, "autotest", "virtual pad failed: %s", SDL_GetError());
    }

    if (at_dude_elevation >= 0 && at_dude_elevation != map_elevation) {
        map_set_elevation(at_dude_elevation);
    }
    int elevation = at_dude_elevation >= 0 ? at_dude_elevation : map_elevation;

    if (at_projectile_speedup > 0.0f) {
        anim_set_projectile_speedup(at_projectile_speedup);
    }
    if (at_tactical >= 0) {
        combat_rt_set_tactical_ai(at_tactical != 0);
    }
    if (at_projectiles >= 0) {
        combat_rt_set_projectiles(at_projectiles != 0);
    }

    // The dude.
    for (int s = 0; s < 7; s++) {
        if (at_dude_stats[s] >= 0) {
            stat_set_base(obj_dude, STAT_STRENGTH + s, at_dude_stats[s]);
        }
    }
    stat_recalc_derived(obj_dude);

    // ⚠️ THE PARTY STANDS WHERE THIS MAP IS ON THE WORLD MAP, as it would after walking there. A map
    // change (an elevator, a door into the next map) calls worldmap_script_jump, which otherwise
    // "travels" from the new game's start and adds the weeks of the journey to the clock: on the
    // first elevator ride in Mariposa 15 days passed and the self-destruct timer was long gone.
    {
        int savedTime = game_time();
        worldmap_script_jump(xlate_mapidx_to_town(map_data.field_34), 0);
        set_game_time(savedTime);
    }

    for (int g = 0; g < at_gvar_count; g++) {
        game_set_global_var(at_gvar_index[g], at_gvar_value[g]);
        trace_event(TRACE_FLOW, "autotest", "gvar %d=%d", at_gvar_index[g], at_gvar_value[g]);
    }
    for (int m = 0; m < at_mvar_count; m++) {
        ProgramValue value;
        value.opcode = VALUE_TYPE_INT;
        value.integerValue = at_mvar_value[m];
        map_set_global_var(at_mvar_index[m], value);
        trace_event(TRACE_FLOW, "autotest", "mapvar %d=%d", at_mvar_index[m], at_mvar_value[m]);
    }
    Proto* proto;
    if (proto_ptr(obj_dude->pid, &proto) == 0) {
        for (int s = 0; s < SKILL_COUNT; s++) {
            if (at_dude_skills[s] < 0) {
                continue;
            }
            for (int guard = 0; guard < 300 && skill_level(obj_dude, s) < at_dude_skills[s]; guard++) {
                proto->critter.data.skills[s] += 1;
            }
        }
    }
    for (int i = 0; i < at_dude_item_count; i++) {
        at_give(obj_dude, at_dude_items[i].pid, at_dude_items[i].qty);
    }
    if (at_dude_armor >= 0) {
        Object* armor = at_give(obj_dude, at_dude_armor, 1);
        if (armor != NULL) {
            inven_wield(obj_dude, armor, 0);
        }
    }
    if (at_dude_wield >= 0) {
        at_wield(obj_dude, at_dude_wield, intface_is_item_right_hand() ? HAND_RIGHT : HAND_LEFT);
    }
    // Last: putting on the armour recalculates the derived stats, maximum HP among them.
    if (at_dude_hp > 0) {
        // A higher-level character's pool, without levelling one up.
        int now = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
        stat_set_bonus(obj_dude, STAT_MAXIMUM_HIT_POINTS, stat_get_bonus(obj_dude, STAT_MAXIMUM_HIT_POINTS) + at_dude_hp - now);
    }
    obj_dude->data.critter.hp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    if (at_dude_curhp > 0 && at_dude_curhp < obj_dude->data.critter.hp) {
        obj_dude->data.critter.hp = at_dude_curhp;
    }
    if (at_dude_tile >= 0) {
        obj_move_to_tile(obj_dude, at_dude_tile, elevation, NULL);
    }
    if (at_dude_rotation >= 0) {
        obj_set_rotation(obj_dude, at_dude_rotation, NULL);
    }
    tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
    intface_update_hit_points(false);
    intface_update_items(false);
    if (at_clear_radius >= 0) {
        Object** list = NULL;
        int count = obj_create_list(-1, obj_dude->elevation, OBJ_TYPE_CRITTER, &list);
        int cleared = 0;
        for (int i = 0; i < count; i++) {
            Object* c = list[i];
            if (c != obj_dude && tile_dist(c->tile, obj_dude->tile) <= at_clear_radius) {
                obj_erase_object(c, NULL);
                cleared++;
            }
        }
        if (list != NULL) {
            obj_delete_list(list);
        }
        trace_event(TRACE_FLOW, "autotest", "cleararea radius=%d cleared=%d", at_clear_radius, cleared);
    }

    // The enemies.
    for (int i = 0; i < at_spawn_count; i++) {
        AtSpawn* sp = &(at_spawns[i]);
        Object* critter;
        if (obj_pid_new(&critter, sp->pid) != 0) {
            trace_event(TRACE_WARN, "autotest", "cannot create critter pid=%d", sp->pid);
            continue;
        }
        obj_move_to_tile(critter, sp->tile, elevation, NULL);
        if (sp->rotation >= 0) {
            obj_set_rotation(critter, sp->rotation, NULL);
        }
        for (int k = 0; k < sp->statCount; k++) {
            stat_set_base(critter, STAT_STRENGTH + sp->statIndex[k], sp->statValue[k]);
        }
        critter->data.critter.hp = stat_level(critter, STAT_MAXIMUM_HIT_POINTS);
        for (int k = 0; k < sp->itemCount; k++) {
            at_give(critter, sp->items[k].pid, sp->items[k].qty);
        }
        if (sp->wield >= 0) {
            at_wield(critter, sp->wield, HAND_RIGHT);
            Object* gun = inven_right_hand(critter);
            if (sp->empty && gun != NULL && item_get_type(gun) == ITEM_TYPE_WEAPON && item_w_max_ammo(gun) > 0) {
                item_w_set_curr_ammo(gun, 0);
            }
        }
        sp->obj = critter;
        if (sp->curhp > 0 && sp->curhp < critter->data.critter.hp) {
            critter->data.critter.hp = sp->curhp;
        }

        // ONE SIDE: raiders (team 1) and a super mutant (team 6) spawned together are not allies to the
        // engine, so friendly-fire care, guard joins and the squad rules would not see each other. A real
        // map keeps one place's fighters in one team; the scenario does the same unless told otherwise.
        if (sp->team >= 0) {
            critter->data.critter.combat.team = sp->team;
        } else if (sp->hostile) {
            if (at_team < 0) {
                at_team = critter->data.critter.combat.team;
            }
            critter->data.critter.combat.team = at_team;
        }
        if (sp->hostile) {
            // As a script's "attack the player" does: hostile, but it still has to perceive you.
            combat_rt_notify_attack(critter, obj_dude);
        }
        trace_event(TRACE_FLOW, "autotest-spawn", "id=%d pid=%d tile=%d wield=%d hostile=%d team=%d hp=%d",
            critter->id, critter->pid, critter->tile, sp->wield, sp->hostile ? 1 : 0, critter->data.critter.combat.team,
            critter->data.critter.hp);
    }

    if (at_provoke) {
        Object** list = NULL;
        int count = obj_create_list(-1, elevation, OBJ_TYPE_CRITTER, &list);
        int provoked = 0;
        for (int i = 0; i < count; i++) {
            Object* c = list[i];
            if (c == obj_dude || critter_is_dead(c)) {
                continue;
            }
            combat_rt_notify_attack(c, obj_dude);
            provoked++;
        }
        if (list != NULL) {
            obj_delete_list(list);
        }
        trace_event(TRACE_FLOW, "autotest", "provoked %d critters", provoked);
    }

    for (int i = 0; i < at_bad_line_count; i++) {
        trace_event(TRACE_WARN, "autotest", "bad timeline line -- %s", at_bad_lines[i]);
    }
    if (at_step_count > 0 && at_phase_count > 0) {
        trace_event(TRACE_WARN, "autotest", "the `at` plan is ignored -- the timeline owns the pad");
    }

    trace_event(TRACE_FLOW, "autotest", "start scenario=%s file=%s map=%s seed=%d duration=%.0f dude=%d spawns=%d pad=%d steps=%d",
        at_name, at_file, at_map_name, at_seed, at_duration, obj_dude->tile, at_spawn_count, at_joy_index, at_step_count);

    if (at_step_count > 0) {
        at_tl_start();
    }
}

// --- THE BOT ---------------------------------------------------------------------------------------

static bool at_is_enemy(Object* c)
{
    if (c == NULL || c == obj_dude || critter_is_dead(c)) {
        return false;
    }
    if (combat_rt_is_hostile(c)) {
        return true;
    }
    for (int i = 0; i < at_spawn_count; i++) {
        if (at_spawns[i].obj == c && at_spawns[i].hostile) {
            return true;
        }
    }
    return false;
}

static Object* at_nearest_enemy(int* distOut)
{
    Object** list = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &list);
    Object* best = NULL;
    int bestDist = 9999;
    for (int i = 0; i < count; i++) {
        Object* c = list[i];
        if (!at_is_enemy(c)) {
            continue;
        }
        int d = obj_dist(obj_dude, c);
        if (d < bestDist) {
            best = c;
            bestDist = d;
        }
    }
    if (list != NULL) {
        obj_delete_list(list);
    }
    *distOut = bestDist;
    return best;
}

static void at_axis(SDL_GameControllerAxis axis, Sint16 value)
{
    if (at_joy != NULL) {
        SDL_JoystickSetVirtualAxis(at_joy, axis, value);
    }
}

static void at_button(SDL_GameControllerButton button, bool down)
{
    if (at_joy != NULL) {
        SDL_JoystickSetVirtualButton(at_joy, button, down ? 1 : 0);
    }
}

// A press long enough for every edge detector (3 frames down, then 3 up before the next).
static void at_tap(SDL_GameControllerButton button)
{
    if (at_tap_frames[button] == 0 && at_tap_rest[button] == 0) {
        at_tap_frames[button] = 3;
    }
}

static void at_tap_rt()
{
    if (at_rt_frames == 0 && at_rt_rest == 0) {
        at_rt_frames = 3;
        at_shots++;
    }
}

// The left stick, pointed from the dude at a tile (screen space: what the player's thumb does).
static void at_stick_toward(int tile, bool away)
{
    int x1, y1, x2, y2;
    tile_coord(obj_dude->tile, &x1, &y1, obj_dude->elevation);
    tile_coord(tile, &x2, &y2, obj_dude->elevation);
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        at_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
        at_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
        return;
    }
    if (away) {
        dx = -dx;
        dy = -dy;
    }
    at_axis(SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(32000.0f * dx / len));
    at_axis(SDL_CONTROLLER_AXIS_LEFTY, (Sint16)(32000.0f * dy / len));
}

static void at_stick_still()
{
    at_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    at_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
}

static int at_weapon_range()
{
    int hitMode = intface_is_item_right_hand() ? HIT_MODE_RIGHT_WEAPON_PRIMARY : HIT_MODE_LEFT_WEAPON_PRIMARY;
    int range = item_w_range(obj_dude, hitMode);
    return range > 0 ? range : 1;
}

static bool at_weapon_empty()
{
    Object* item = NULL;
    intface_get_current_item(&item);
    if (item == NULL || item == (Object*)-1 || item_get_type(item) != ITEM_TYPE_WEAPON) {
        return false;
    }
    return item_w_max_ammo(item) > 0 && item_w_curr_ammo(item) == 0;
}

// Hold LT, keep a lock, fire at it when in reach, look after the gun and the HP.
static void at_fight(Object* enemy, int dist)
{
    at_stick_still();
    if (enemy == NULL) {
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        return;
    }
    if (at_lt_release > 0) {
        at_lt_release--;
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    } else {
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
        if (!combat_rt_has_lock() && at_elapsed >= at_relock_next) {
            at_lt_release = 3; // let go and press again: a fresh lock picks again
            at_relock_next = at_elapsed + 0.5f;
        }
    }
    if (at_weapon_empty()) {
        if (at_elapsed >= at_reload_next) {
            at_tap(SDL_CONTROLLER_BUTTON_X);
            at_reload_next = at_elapsed + 2.0f;
        }
        return;
    }
    int hp = critter_get_hits(obj_dude);
    int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    if (maxHp > 0 && hp * 100 < maxHp * 35 && at_elapsed >= at_heal_next) {
        at_tap(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        at_heal_next = at_elapsed + 3.0f;
        return;
    }
    if (combat_rt_has_lock() && dist <= at_weapon_range() && at_elapsed >= at_rt_next) {
        at_tap_rt();
        at_rt_next = at_elapsed + 0.3f;
    }
}

static void at_end(const char* why, float delay)
{
    if (at_end_at < 0.0f) {
        at_end_at = at_elapsed + delay;
        at_end_why = why;
    }
}

// --- THE PAD TIMELINE ------------------------------------------------------------------------------

static const char* at_button_name(int button)
{
    const char* name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)button);
    return name != NULL ? name : "?";
}

static const char* at_axis_name(int axis)
{
    const char* name = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
    return name != NULL ? name : "?";
}

// Which screen is up: the topmost modal window, traced whenever it changes. Every screen the pad
// reaches registers WINDOW_MODAL -- the editor and its age / sex / options windows, dialog_out, the
// inventory, the Pip-Boy, the automap, the options menu -- so its id, place and size say which.
static void at_tl_watch_modal(int wall)
{
    int top = win_get_top_modal();
    if (top == at_tl_modal) {
        return;
    }
    at_tl_modal = top;
    Rect rect;
    if (top != -1 && win_get_rect(top, &rect) == 0) {
        trace_event(TRACE_FLOW, "autotest", "modal win=%d at=%d,%d size=%dx%d inv=%d options=%d talk=%d t=%d",
            top, rect.ulx, rect.uly, rect.lrx - rect.ulx + 1, rect.lry - rect.uly + 1,
            inven_any_open() ? 1 : 0, options_is_open() ? 1 : 0, dialog_active() ? 1 : 0, wall);
    } else {
        trace_event(TRACE_FLOW, "autotest", "modal none t=%d", wall);
    }
}

// `duration` on the wall clock -- from inside a screen too, where the game loop never gets to end it.
[[noreturn]] static void at_tl_end(int wall)
{
    at_tl_running = false;
    if (!at_done) {
        at_done = true;
        trace_event(TRACE_FLOW, "autotest", "end why=time elapsed=%.1f dudeHp=%d dudeDead=%d modal=%d",
            wall / 1000.0f, critter_get_hits(obj_dude), critter_is_dead(obj_dude) ? 1 : 0, win_get_top_modal());
    }
    autotest_finish();
}

// The GNW background process: every get_input() of every loop runs it, the world's and each screen's.
static void at_tl_bk()
{
    if (!at_tl_running) {
        return;
    }
    int wall = (int)(SDL_GetTicks() - at_tl_t0);
    at_tl_watch_modal(wall);
    if (wall >= (int)(at_duration * 1000.0f)) {
        at_tl_end(wall);
    }

    int t = wall - at_tl_shift;
    while (at_tl_next < at_step_count && at_tl_open < 0) {
        AtStep* st = &(at_steps[at_tl_next]);
        if (t < st->ms) {
            break;
        }
        switch (st->kind) {
        case AT_STEP_BUTTON:
            at_button((SDL_GameControllerButton)st->index, st->value != 0);
            trace_event(TRACE_FLOW, "autotest", "pad t=%d %s=%d", t, at_button_name(st->index), st->value);
            break;
        case AT_STEP_AXIS:
            at_axis((SDL_GameControllerAxis)st->index, (Sint16)st->value);
            trace_event(TRACE_FLOW, "autotest", "axis t=%d %s=%d", t, at_axis_name(st->index), st->value);
            break;
        case AT_STEP_PAD_OPEN: {
            int button = at_screens[st->index].button;
            trace_event(TRACE_FLOW, "autotest", "open screen=%s via=%s hold=%d t=%d",
                at_screens[st->index].name, at_button_name(button), at_screens[st->index].holdMs, t);
            at_button((SDL_GameControllerButton)button, true);
            break;
        }
        case AT_STEP_OPEN:
            // Only the game loop may open a screen (this runs INSIDE get_input), so autotest_tick takes
            // it -- once the world is back on top -- and the timeline stands still until it has.
            at_tl_open = at_tl_next;
            trace_event(TRACE_FLOW, "autotest", "open screen=%s via=game-loop t=%d", at_screens[st->index].name, t);
            return;
        case AT_STEP_NOTE:
            trace_event(TRACE_FLOW, "autotest", "note t=%d %s", t, st->text);
            break;
        case AT_STEP_WALKTO: {
            // The long way through a level, whose turns no stick timeline can be written for: the route is
            // the pathfinder's, as a click would walk it. 0 steps = no way there (a force field, a door).
            int steps = make_path(obj_dude, obj_dude->tile, st->index, NULL, 0);
            bool ok = false;
            if (steps > 0 && register_begin(ANIMATION_REQUEST_RESERVED) == 0) {
                ok = register_object_run_to_tile(obj_dude, st->index, obj_dude->elevation, -1, 0) == 0;
                register_end();
            }
            int open = make_path_func(obj_dude, obj_dude->tile, st->index, NULL, 0, at_block_but_doors);
            trace_event(TRACE_FLOW, "autotest", "walkto t=%d from=%d to=%d elev=%d steps=%d ok=%d doorsOpen=%d", t,
                obj_dude->tile, st->index, obj_dude->elevation, steps, ok ? 1 : 0, open);
            break;
        }
        }
        at_tl_next++;
    }
}

static int at_open_screen(int screen)
{
    int rc = -1;
    switch (screen) {
    case AT_SCREEN_CREATION: {
        // As the new-game selector runs it (select.cc): a flushed queue, the selector's A-click
        // suppression on, no world animating behind it.
        flush_input_buffer();
        bool wasSuppressed = gamepad_ui_set_suppress_a_click(true);
        bool isoWasEnabled = map_disable_bk_processes();
        rc = editor_design(true);
        if (isoWasEnabled) {
            map_enable_bk_processes();
        }
        flush_input_buffer();
        gamepad_ui_set_suppress_a_click(wasSuppressed);
        // The button that closed it is still down, and the world is not where creation returns to.
        gamepad_ui_bump_input_epoch();
        break;
    }
    }
    return rc;
}

// From autotest_tick, the game loop: a game-loop open the timeline is waiting for.
static void at_tl_take_open()
{
    if (at_tl_open < 0) {
        return;
    }
    const AtStep* st = &(at_steps[at_tl_open]);
    int wall = (int)(SDL_GetTicks() - at_tl_t0);
    int late = wall - at_tl_shift - st->ms;
    // From here on the timeline counts from the moment the screen opened, as if it had been on time.
    at_tl_shift = wall - st->ms;
    at_tl_next = at_tl_open + 1;
    at_tl_open = -1;
    trace_event(TRACE_FLOW, "autotest", "opened screen=%s t=%d late=%d", at_screens[st->index].name, st->ms, late);
    int rc = at_open_screen(st->index);
    trace_event(TRACE_FLOW, "autotest", "closed screen=%s rc=%d t=%d", at_screens[st->index].name, rc,
        (int)(SDL_GetTicks() - at_tl_t0) - at_tl_shift);
}

static void at_tl_start()
{
    // The pad at rest: sticks centred, triggers RELEASED (-32768 on this pad -- a trigger left at 0
    // reads half pulled), every button up. The bot used to keep it there every frame.
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) {
        bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        at_axis((SDL_GameControllerAxis)axis, trigger ? -32768 : 0);
    }
    for (int button = 0; button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; button++) {
        at_button((SDL_GameControllerButton)button, false);
    }
    at_tl_t0 = SDL_GetTicks();
    at_tl_running = true;
    add_bk_process(at_tl_bk, "autotest");
}

void autotest_tick(float dt)
{
    if (!at_active || at_done || obj_dude == NULL) {
        return;
    }
    at_elapsed += dt;

    // A screen test: the timeline owns the pad (and ends the run); the game loop only opens the screens
    // it asks for.
    if (at_step_count > 0) {
        at_tl_take_open();
        return;
    }

    // The plan.
    int next = at_phase;
    while (next + 1 < at_phase_count && at_elapsed >= at_phases[next + 1].at) {
        next++;
    }
    if (next != at_phase) {
        at_phase = next;
        at_goto_best = 9999;
        at_goto_since = at_elapsed;
        at_goto_done = false;
        at_shots = 0;
        at_throw_at = -1.0f;
        trace_event(TRACE_FLOW, "autotest", "phase at=%.1f policy=%s tile=%d dude=%d",
            at_elapsed, at_policy_names[at_phases[at_phase].policy], at_phases[at_phase].tile, obj_dude->tile);
    }
    if (at_ipdump_at >= 0.0f && at_elapsed >= at_ipdump_at) {
        at_ipdump_at = -1.0f;
        interact_prompt_trace_candidates();
    }
    int policy = at_phase >= 0 ? at_phases[at_phase].policy : AT_IDLE;
    int phaseTile = at_phase >= 0 ? at_phases[at_phase].tile : -1;

    // Taps in progress.
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (at_tap_frames[b] > 0) {
            at_button((SDL_GameControllerButton)b, true);
            if (--at_tap_frames[b] == 0) {
                at_tap_rest[b] = 3;
            }
        } else {
            at_button((SDL_GameControllerButton)b, false);
            if (at_tap_rest[b] > 0) {
                at_tap_rest[b]--;
            }
        }
    }
    if (at_rt_frames > 0) {
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        if (--at_rt_frames == 0) {
            at_rt_rest = 3;
        }
    } else {
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        if (at_rt_rest > 0) {
            at_rt_rest--;
        }
    }

    bool dudeDead = critter_is_dead(obj_dude);
    int dist = 9999;
    Object* enemy = dudeDead ? NULL : at_nearest_enemy(&dist);

    if (!dudeDead) {
        switch (policy) {
        case AT_IDLE:
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            break;
        case AT_FIGHT:
            at_fight(enemy, dist);
            break;
        case AT_ADVANCE:
            if (enemy != NULL && dist > at_weapon_range() - 1) {
                at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
                at_stick_toward(enemy->tile, false);
            } else {
                at_fight(enemy, dist);
            }
            break;
        case AT_KITE:
            if (enemy != NULL && dist <= 4) {
                at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
                at_stick_toward(enemy->tile, true);
            } else {
                at_fight(enemy, dist);
            }
            break;
        case AT_GOTO:
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            if (!at_goto_done && phaseTile >= 0) {
                int d = tile_dist(obj_dude->tile, phaseTile);
                if (d < at_goto_best) {
                    at_goto_best = d;
                    at_goto_since = at_elapsed;
                }
                // There, or no closer for 3 s (a wall the stick cannot get round): stop.
                if (d == 0 || at_elapsed - at_goto_since > 3.0f) {
                    at_goto_done = true;
                    trace_event(TRACE_FLOW, "autotest", "goto-done at=%.1f tile=%d want=%d dist=%d",
                        at_elapsed, obj_dude->tile, phaseTile, d);
                    at_stick_still();
                } else {
                    at_stick_toward(phaseTile, false);
                }
            } else {
                at_stick_still();
            }
            break;
        case AT_THROW:
        case AT_THROW_FREE:
            // LT first (a lock needs a beat), then RB: down for 6 frames, up -- the quick slot draws on
            // the press and throws on the release.
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, policy == AT_THROW ? 32767 : -32768);
            if (at_throw_at < 0.0f) {
                at_throw_at = at_elapsed + (policy == AT_THROW ? 0.6f : 0.1f);
            } else if (at_elapsed >= at_throw_at && at_shots == 0) {
                at_tap_frames[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 6;
                at_shots = 1;
                trace_event(TRACE_FLOW, "autotest", "rb-tap at=%.1f lock=%d", at_elapsed, combat_rt_has_lock() ? 1 : 0);
            }
            break;
        case AT_STRAFE:
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            if (enemy == NULL) {
                at_stick_still();
                at_button(SDL_CONTROLLER_BUTTON_X, false);
                break;
            }
            if (at_elapsed >= at_strafe_flip) {
                at_strafe_dir = -at_strafe_dir;
                at_strafe_flip = at_elapsed + 1.5f;
            }
            {
                // Perpendicular to the line to the enemy, in screen space (what a thumb does).
                int x1, y1, x2, y2;
                tile_coord(obj_dude->tile, &x1, &y1, obj_dude->elevation);
                tile_coord(enemy->tile, &x2, &y2, obj_dude->elevation);
                float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
                float len = sqrtf(dx * dx + dy * dy);
                if (len < 1.0f) {
                    len = 1.0f;
                }
                float px = -dy / len * at_strafe_dir, py = dx / len * at_strafe_dir;
                at_axis(SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(32000.0f * px));
                at_axis(SDL_CONTROLLER_AXIS_LEFTY, (Sint16)(32000.0f * py));
                at_button(SDL_CONTROLLER_BUTTON_X, true); // run
            }
            break;
        case AT_DODGE: {
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            bool targeted = combat_rt_dude_targeted();
            if (targeted && !at_was_targeted) {
                at_dodge_from = at_elapsed + at_reaction;
                at_dodge_until = at_dodge_from + 1.2f;
                at_strafe_dir = -at_strafe_dir;
                trace_event(TRACE_FLOW, "autotest", "dodge at=%.1f dir=%d ap=%d", at_elapsed, at_strafe_dir, combat_rt_player_ap());
            }
            at_was_targeted = targeted;
            if (enemy == NULL || at_elapsed < at_dodge_from || at_elapsed >= at_dodge_until) {
                at_stick_still();
                at_button(SDL_CONTROLLER_BUTTON_X, false);
                break;
            }
            int x1, y1, x2, y2;
            tile_coord(obj_dude->tile, &x1, &y1, obj_dude->elevation);
            tile_coord(enemy->tile, &x2, &y2, obj_dude->elevation);
            float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1.0f) {
                len = 1.0f;
            }
            at_axis(SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(-32000.0f * dy / len * at_strafe_dir));
            at_axis(SDL_CONTROLLER_AXIS_LEFTY, (Sint16)(32000.0f * dx / len * at_strafe_dir));
            at_button(SDL_CONTROLLER_BUTTON_X, true);
            break;
        }
        case AT_SHOOT_MOVE: {
            bool attacking = combat_rt_dude_attacking();
            if (attacking && !at_was_attacking) {
                at_move_until = at_elapsed + 0.8f;
                at_strafe_dir = -at_strafe_dir;
                trace_event(TRACE_FLOW, "autotest", "shootmove at=%.1f dir=%d ap=%d", at_elapsed, at_strafe_dir, combat_rt_player_ap());
            }
            at_was_attacking = attacking;
            if (enemy == NULL || at_elapsed >= at_move_until) {
                at_fight(enemy, dist);
                break;
            }
            // Keep the lock (LT held) while the stick walks him sideways.
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
            int x1, y1, x2, y2;
            tile_coord(obj_dude->tile, &x1, &y1, obj_dude->elevation);
            tile_coord(enemy->tile, &x2, &y2, obj_dude->elevation);
            float dx = (float)(x2 - x1), dy = (float)(y2 - y1);
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1.0f) {
                len = 1.0f;
            }
            at_axis(SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(-32000.0f * dy / len * at_strafe_dir));
            at_axis(SDL_CONTROLLER_AXIS_LEFTY, (Sint16)(32000.0f * dx / len * at_strafe_dir));
            break;
        }
        case AT_AIM_OTHER: {
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
            at_rt_frames = 3;
            at_rt_rest = 0;
            Object* lock = combat_rt_lock_target();
            Object* other = NULL;
            int otherDist = 9999;
            Object** list = NULL;
            int count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &list);
            for (int i = 0; i < count; i++) {
                if (list[i] != lock && at_is_enemy(list[i]) && obj_dist(obj_dude, list[i]) < otherDist) {
                    other = list[i];
                    otherDist = obj_dist(obj_dude, list[i]);
                }
            }
            if (list != NULL) {
                obj_delete_list(list);
            }
            int toTile = phaseTile >= 0 ? phaseTile : (other != NULL ? other->tile : -1);
            // A flick: 0.25 s pushed, then home until the next one.
            float cycle = fmodf(at_elapsed, 0.6f);
            if (lock != NULL && toTile >= 0 && cycle < 0.25f) {
                int x1, y1, x2, y2;
                tile_coord(lock->tile, &x1, &y1, obj_dude->elevation);
                tile_coord(toTile, &x2, &y2, obj_dude->elevation);
                float dx = (float)(x2 - x1);
                float dy = (float)(y2 - y1);
                float len = sqrtf(dx * dx + dy * dy);
                if (len >= 1.0f) {
                    at_axis(SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)(32000.0f * dx / len));
                    at_axis(SDL_CONTROLLER_AXIS_RIGHTY, (Sint16)(32000.0f * dy / len));
                }
            } else {
                at_axis(SDL_CONTROLLER_AXIS_RIGHTX, 0);
                at_axis(SDL_CONTROLLER_AXIS_RIGHTY, 0);
            }
            break;
        }
        case AT_RT_HOLD:
            // RT held for as long as this phase lasts, LT never touched (rt_unlocked_attack_target).
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            at_rt_frames = 3;
            at_rt_rest = 0;
            break;
        case AT_HEAL_HOLD:
            // LB held down for as long as this phase lasts (the release comes with the next phase).
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            at_tap_frames[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 3;
            if (at_shots == 0) {
                at_shots = 1;
                trace_event(TRACE_FLOW, "autotest", "heal-hold at=%.2f hp=%d ap=%d", at_elapsed, critter_get_hits(obj_dude), combat_rt_player_ap());
            }
            break;
        case AT_USE:
            // One press of A (use what the interact prompt points at), then stand.
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            if (at_shots == 0) {
                at_tap(SDL_CONTROLLER_BUTTON_A);
                at_shots = 1;
                trace_event(TRACE_FLOW, "autotest", "use-tap at=%.2f ap=%d", at_elapsed, combat_rt_player_ap());
            }
            break;
        case AT_HEAL:
            // One press of LB (the heal slot), then stand.
            at_stick_still();
            at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            if (at_shots == 0) {
                at_tap(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
                at_shots = 1;
                trace_event(TRACE_FLOW, "autotest", "heal-tap at=%.2f hp=%d", at_elapsed, critter_get_hits(obj_dude));
            }
            break;
        case AT_SHOOT:
            if (at_shots == 0) {
                at_fight(enemy, dist);
            } else {
                at_stick_still();
                at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            }
            break;
        }
    } else {
        at_stick_still();
        at_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    }

    // A photograph of the bullets (scenario "shots"): EVERY frame a tracer is in the air, up to six, so
    // a real-speed bullet (on screen ~0.05 s) is caught all the way across -- a single photo missed the
    // .223 and the buckshot (2026-10-19).
    static int at_seq_frames = 0;
    if (at_shots_taken < at_shots_wanted && at_elapsed >= at_shot_next && combat_rt_tracers_in_air() > 0) {
        dump_screen();
        at_seq_frames++;
        trace_event(TRACE_FLOW, "autotest", "shot n=%d frame=%d at=%.3f tracers=%d", at_shots_taken, at_seq_frames, at_elapsed, combat_rt_tracers_in_air());
        if (at_seq_frames >= 6) {
            at_shots_taken++;
            at_shot_next = at_elapsed + 1.0f;
            at_seq_frames = 0;
        }
    } else if (at_seq_frames > 0) {
        at_shots_taken++;
        at_shot_next = at_elapsed + 1.0f;
        at_seq_frames = 0;
    }

    // The end.
    if (at_elapsed >= at_duration) {
        at_end("time", 0.0f);
    } else if (dudeDead) {
        at_end("dude-dead", 2.0f);
    } else if (at_spawn_count > 0) {
        bool anyAlive = false;
        for (int i = 0; i < at_spawn_count; i++) {
            if (at_spawns[i].hostile && at_spawns[i].obj != NULL && !critter_is_dead(at_spawns[i].obj)) {
                anyAlive = true;
                break;
            }
        }
        if (!anyAlive) {
            at_end("all-dead", 3.0f);
        }
    }
    if (at_end_at >= 0.0f && at_elapsed >= at_end_at) {
        at_done = true;
        trace_event(TRACE_FLOW, "autotest", "end why=%s elapsed=%.1f dudeHp=%d dudeDead=%d",
            at_end_why, at_elapsed, critter_get_hits(obj_dude), dudeDead ? 1 : 0);
        game_user_wants_to_quit = 2;
    }
}

void autotest_finish()
{
    if (!at_done) {
        bool dudeDead = obj_dude != NULL && critter_is_dead(obj_dude);
        trace_event(TRACE_FLOW, "autotest", "end why=%s elapsed=%.1f dudeHp=%d dudeDead=%d",
            dudeDead ? "dude-dead" : "loop-ended", at_elapsed, obj_dude != NULL ? critter_get_hits(obj_dude) : -1,
            dudeDead ? 1 : 0);
        at_done = true;
    }
    trace_shutdown();
    fflush(NULL);
    _exit(0);
}

} // namespace fallout
