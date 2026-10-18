#include "game/mods_page.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game/anim.h"
#include "game/combat_rt.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/gamepad_ui.h"
#include "game/gdialog.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/svga.h"
#include "plib/db/db.h"
#include "plib/gnw/text.h"

namespace fallout {

// The window SIZES ITSELF to the screen (mods_layout). It used to be a fixed 420x300, which was
// right for the three rows it opened with and cramped by the time there were eight: the help band
// had one line to explain a setting in and the sentences were being cut off mid-clause to fit.
#define MODS_W_MAX 620
#define MODS_W_MIN 420
#define MODS_H_MAX 430
#define MODS_H_MIN 300

// Left edge of the value column, and the row geometry. One row = label on the left, the chosen
// value on the right, and the explanation of the CURRENT value in a band at the bottom -- the same
// shape the radial wheels use, for the same reason: a name alone never says what a setting does.
#define MODS_PAD 16
#define MODS_VALUE_GAP 200 // value column, measured back from the right edge

static int mods_w = MODS_W_MIN;
static int mods_h = MODS_H_MIN;
static int mods_value_x = MODS_W_MIN - MODS_VALUE_GAP;
static int mods_row_h = 22;
static int mods_first_row = 52;

// One setting. `values` are the choices in display order; `apply` pushes the chosen one into the
// live game AND into the cfg, so a setting takes effect the moment you move off it -- no "restart
// to apply", which for a comfort toggle would be its own kind of broken. `help2` is an optional
// second line for the settings a single line genuinely cannot explain.
struct ModsOption {
    const char* label;
    const char* values[4];
    const char* help[4];
    const char* help2[4];
    int count;
};

// ⚠️ THIS ENUM IS THE ROW INDEX into mods_options below -- the two lists are one list kept in
// two places, and nothing in the language complains when they drift apart. They did: Language sat
// at index 7 in the enum and at index 5 in the table, so the row labelled Language toggled free
// movement and the row labelled Free movement switched the language. The static_asserts under the
// table are the guard -- add a row in ONE place and the build stops.
enum {
    MODS_TRAVEL_SPEED,
    MODS_DOOR_MAGNET,
    MODS_NPC_SLIDE,
    MODS_KNOCKDOWN_AGILITY,
    MODS_CAM_SMOOTH,
    MODS_LANGUAGE,
    MODS_RADAWAY,
    MODS_RELOAD_ANIM,
    MODS_LOOK_AROUND,
    MODS_SCRIPT_TURNS,
    MODS_FREE_MOVE,
    MODS_SMOOTH_MOTION,
    MODS_VSYNC,
    MODS_COMBAT_BOX_SOUND,
    MODS_CAM_LEAN,
    MODS_SCRIPT_FLOAT,
    MODS_HIT_STAGGER,
    MODS_MOUSE,
    MODS_FORGIVE_STRAY,
    MODS_CROWD_ROOM,
    MODS_TACTICAL_AI,
    MODS_PROJECTILES,
    MODS_SCREEN_RES,
    MODS_INTERNAL_RES,
    MODS_COUNT,
};

static constexpr ModsOption mods_options[MODS_COUNT] = {
    {
        "Travel speed",
        { "Normal", "Brisk", "Fast", "Very fast" },
        {
            "The game as it shipped.",
            "About a third quicker on foot when nothing is fighting.",
            "Half again as quick. Combat is never affected.",
            "Twice the pace. Crossing a cleared map stops being a chore.",
        },
        {
            "Combat pacing is never touched by this row.",
            "World map journeys shrink the same way.",
            "World map travel too, at the same game-time cost.",
            "World map too: same days, same encounters, half the wait.",
        },
        4,
    },
    {
        "Doorway assist",
        { "Off", "On" },
        {
            "The stick points exactly where you push it.",
            "Bends your heading toward a doorway you are aiming at, so you",
        },
        {
            NULL,
            "go through the gap instead of scraping along the wall.",
        },
        2,
    },
    {
        "Walk around people",
        { "Off", "On" },
        {
            "Bumping a bystander stops you, as in the original.",
            "You step around a neutral standing in your way instead of",
        },
        {
            NULL,
            "jamming to a halt. Never while a fight is live.",
        },
        2,
    },
    {
        "Agility gets you up",
        { "Off", "On" },
        {
            "Everyone spends the same time on the ground after a knockdown.",
            "Nimble characters get back on their feet sooner, clumsy ones",
        },
        {
            NULL,
            "slower. Agility 1 lies there about twice as long as Agility 10.",
        },
        2,
    },
    {
        "Smooth camera",
        { "Off", "On" },
        {
            "You are welded to the exact centre of the screen at all times.",
            "The view eases after you instead of snapping, so a swing or a",
        },
        {
            NULL,
            "running step cannot jolt the whole screen.",
        },
        2,
    },
    {
        "Language",
        { "English", "Cestina" },
        {
            "The game's own text.",
            "Czech, from text\\CZECH. Takes effect when you restart the game.",
        },
        {
            NULL,
            "Not installed? Put the translation in data\\TEXT\\CZECH, not over english.",
        },
        2,
    },
    {
        "RadAway clears dose",
        { "Off", "On" },
        {
            "As shipped: a dose already on its way lands whatever you drink.",
            "RadAway also cancels the radiation dose that is already queued.",
        },
        {
            "Rads only decide how bad the NEXT one is.",
            "Being irradiated schedules the damage 4-18 hours ahead.",
        },
        2,
    },
    {
        "Reload animation",
        { "Off", "On" },
        {
            "As shipped: the magazine is full the instant anyone reloads.",
            "Everyone works the weapon first (pistols: holster and draw),",
        },
        {
            "Your character also fidgets with the gun now and then.",
            "full at the end. Agility+Perception speed it; moving cancels.",
        },
        2,
    },
    {
        "Enemies look around",
        { "Off", "On" },
        {
            "As shipped: an enemy with its back to you sees you only up close.",
            "Hostiles standing guard turn their heads every few seconds,",
        },
        {
            "Facing you they see Perception x5 tiles; behind them, only x1.",
            "and toward a gunshot they hear. Sneaking up behind still works.",
        },
        2,
    },
    {
        "Script combat turns",
        { "Off", "On" },
        {
            "Scripts get no combat turns: the Master never calls his guards,",
            "As shipped: scripts get their combat turns. The Master calls",
        },
        {
            "mutant bases never sound the alarm, towns never turn on you.",
            "his horde, bases raise the alarm. One turn = a full AP refill.",
        },
        2,
    },
    {
        "Free movement",
        { "Off", "On" },
        {
            "The original movement: one whole tile at a time, six directions.",
            "You move off the grid entirely; the tile under you just follows.",
        },
        {
            NULL,
            "Any angle, instant turns, and you slide along walls.",
        },
        2,
    },
    {
        "Smooth movement",
        { "Off", "On" },
        {
            "As shipped: walkers and bullets hop once per animation frame.",
            "Everyone who walks, runs or flies glides between the steps of",
        },
        {
            "That is 10-20 hops a second, next to a camera that glides.",
            "their animation instead of hopping. Drawing only; rules untouched.",
        },
        2,
    },
    {
        "VSync",
        { "Off", "On" },
        {
            "As shipped: frames go out whenever they are ready. Fast panels",
            "Each frame waits for your screen's refresh, so motion is even",
        },
        {
            "show them unevenly, and the picture can tear.",
            "and never tears. Still ~60 fps on a 120/144/240 Hz panel.",
        },
        2,
    },
    {
        "Combat box sound",
        { "Off", "On" },
        {
            "The combat box on the bar opens and closes without a sound.",
            "As shipped: the box clunks open when a fight starts, chimes",
        },
        {
            NULL,
            "as its lights come on, and clunks shut when the fight is over.",
        },
        2,
    },
    {
        "Camera leans to target",
        { "Off", "On" },
        {
            "As shipped: the camera stays on you, even when the enemy you",
            "Aim at an enemy off the screen and the camera eases a little",
        },
        {
            "aim at is off the edge of the screen.",
            "toward it -- just enough to show it -- and back when you stop.",
        },
        2,
    },
    {
        "Script lines as floating text",
        { "Off", "On" },
        {
            "As shipped: a script's one-line shout, like a guard's \"Intruders!\",",
            "That shout floats over the speaker like a combat taunt, and the",
        },
        {
            "comes up as bare grey boxes and stops the game until [Done].",
            "game carries on. Longer lines still open the dialogue box.",
        },
        2,
    },
    {
        "Hits stagger",
        { "Always", "By hit strength" },
        {
            "Every hit that gets through armour makes the target flinch",
            "A hit through armour makes the target flinch only by chance,",
        },
        {
            "and stop for a moment -- a walking enemy stops on every bullet.",
            "the bigger the hit the likelier. Small hits: it keeps walking.",
        },
        2,
    },
    {
        "Mouse",
        { "Vanilla", "Arrow walks" },
        {
            "Click a spot to walk there, as in the original.",
            "Right-click to the arrow: hold left to walk, far = run.",
        },
        {
            "Right-click switches the cursor.",
            "The hex cursor stays vanilla. Left on an enemy attacks.",
        },
        2,
    },
    {
        "Stray hits on allies",
        { "Vanilla", "Forgive once" },
        {
            "A stray bullet in anyone turns them, and their friends, on you.",
            "A stray bullet in someone fighting your target is forgiven once.",
        },
        {
            "A burst beside Killian can set the whole town on you.",
            "Your target, and a second stray hit, still count.",
        },
        2,
    },
    {
        "Crowds make room",
        { "Off", "On" },
        {
            "The fighter beside the target keeps its spot; the rest walk round.",
            "After striking, a fighter slides aside or steps back so a waiting",
        },
        {
            NULL,
            "teammate gets in. Everyone does it, your companions too.",
        },
        2,
    },
    {
        "Tactical AI",
        { "Off", "On" },
        {
            "Fighters follow only the original game's rules.",
            "Fighters dodge grenades, reload in cover, search where they last",
        },
        {
            NULL,
            "saw you, take nearer foes and try not to shoot their friends.",
        },
        2,
    },
    {
        "Projectiles",
        { "Vanilla", "Flying" },
        {
            "A shot hits or misses the moment it is fired.",
            "Bullets, bolts and plasma fly: step out of their line and",
        },
        {
            NULL,
            "they miss. Lasers are almost instant, rockets slow.",
        },
        2,
    },
    {
        "Screen resolution",
        { "1280x720", "1920x1080", "2560x1440", "Desktop" },
        {
            "The window size. Fullscreen always fills your whole screen.",
            "Restart to apply.",
            "Restart to apply.",
            "Whatever your desktop is set to -- 1280x800 on a Steam Deck.",
        },
        {
            "How BIG things look is the row below, not this one.",
            NULL,
            NULL,
            "Restart to apply.",
        },
        4,
    },
    {
        "Internal resolution",
        { "Original", "Sharper", "Native" },
        {
            "480 lines, the size everything was in 1997. Restart to apply.",
            "720 lines: elements two thirds the size, more map on screen.",
            "Your screen's own height. Smallest elements, the most map.",
        },
        {
            "The width follows your screen's shape, so nothing is squashed.",
            "Restart to apply.",
            "Restart to apply.",
        },
        3,
    },
};

// The guard for the comment above the enum. A row is named in two places -- the enum that the
// apply switch reads and the table the screen draws -- so the build checks they are the same row.
// A label that no longer matches its enum name is the bug that shipped: silent, and visible only
// as "this setting does nothing".
static constexpr bool mods_label_is(int index, const char* expected)
{
    const char* a = mods_options[index].label;
    while (*a != '\0' && *a == *expected) {
        a++;
        expected++;
    }
    return *a == *expected;
}

static_assert(mods_label_is(MODS_TRAVEL_SPEED, "Travel speed"), "MODS row order drifted");
static_assert(mods_label_is(MODS_DOOR_MAGNET, "Doorway assist"), "MODS row order drifted");
static_assert(mods_label_is(MODS_NPC_SLIDE, "Walk around people"), "MODS row order drifted");
static_assert(mods_label_is(MODS_KNOCKDOWN_AGILITY, "Agility gets you up"), "MODS row order drifted");
static_assert(mods_label_is(MODS_CAM_SMOOTH, "Smooth camera"), "MODS row order drifted");
static_assert(mods_label_is(MODS_LANGUAGE, "Language"), "MODS row order drifted");
static_assert(mods_label_is(MODS_RADAWAY, "RadAway clears dose"), "MODS row order drifted");
static_assert(mods_label_is(MODS_RELOAD_ANIM, "Reload animation"), "MODS row order drifted");
static_assert(mods_label_is(MODS_LOOK_AROUND, "Enemies look around"), "MODS row order drifted");
static_assert(mods_label_is(MODS_SCRIPT_TURNS, "Script combat turns"), "MODS row order drifted");
static_assert(mods_label_is(MODS_FREE_MOVE, "Free movement"), "MODS row order drifted");
static_assert(mods_label_is(MODS_SMOOTH_MOTION, "Smooth movement"), "MODS row order drifted");
static_assert(mods_label_is(MODS_VSYNC, "VSync"), "MODS row order drifted");
static_assert(mods_label_is(MODS_COMBAT_BOX_SOUND, "Combat box sound"), "MODS row order drifted");
static_assert(mods_label_is(MODS_CAM_LEAN, "Camera leans to target"), "MODS row order drifted");
static_assert(mods_label_is(MODS_SCRIPT_FLOAT, "Script lines as floating text"), "MODS row order drifted");
static_assert(mods_label_is(MODS_HIT_STAGGER, "Hits stagger"), "MODS row order drifted");
static_assert(mods_label_is(MODS_MOUSE, "Mouse"), "MODS row order drifted");
static_assert(mods_label_is(MODS_FORGIVE_STRAY, "Stray hits on allies"), "MODS row order drifted");
static_assert(mods_label_is(MODS_CROWD_ROOM, "Crowds make room"), "MODS row order drifted");
static_assert(mods_label_is(MODS_TACTICAL_AI, "Tactical AI"), "MODS row order drifted");
static_assert(mods_label_is(MODS_PROJECTILES, "Projectiles"), "MODS row order drifted");
static_assert(mods_label_is(MODS_SCREEN_RES, "Screen resolution"), "MODS row order drifted");
static_assert(mods_label_is(MODS_INTERNAL_RES, "Internal resolution"), "MODS row order drifted");

// travel_speed is a MULTIPLIER on how long a frame lasts, so LOWER is faster. The screen speaks in
// speeds and this table is the only place the two ever meet.
static const float mods_travel_values[4] = { 1.0f, 0.75f, 0.67f, 0.5f };

static int mods_sel = 0;
static int mods_value[MODS_COUNT];

// --- THE INTERNAL RESOLUTION ROW ----------------------------------------------------------------
//
// The heights the row offers. 0 means "this screen's own", resolved against SCR_HEIGHT when it is
// written. 480 is the one that matters: the game shipped at 640x480, so 480 internal lines put
// every element back at the size it was, whatever the panel is (see f1_res.ini's own notes).
static const int mods_res_heights[3] = { 480, 720, 0 };

// The OUTPUT sizes the screen-resolution row offers. { 0, 0 } is "whatever the desktop is", which
// is the only sane entry for a handheld -- a Steam Deck is 1280x800 and a Vita 960x544, and neither
// belongs in a list of desktop sizes.
static const int mods_screen_sizes[4][2] = {
    { 1280, 720 },
    { 1920, 1080 },
    { 2560, 1440 },
    { 0, 0 },
};

// ⚠️ WRITTEN BY HAND, LINE BY LINE, INSTEAD OF THROUGH Config. config_save() rewrites the file from
// the parsed key/values -- which would throw away every comment in f1_res.ini, and those comments
// ARE the documentation for a file you edit with a text editor on a Steam Deck. So: replace the
// key's line in [MAIN] if there is one (commented out or not), add it to [MAIN] if there is not, and
// leave every other byte exactly where it was.
//
// ⚠️ TWO THINGS THE STEAM INSTALL TAUGHT (2026-10-10, the release check). Steam ships the High
// Resolution Patch's f1_res.ini: 228 lines and TEN sections, [MAIN] first. (1) The buffer held 128
// lines, so the overflow guard fired and both resolution rows silently never stuck -- now 1024,
// static rather than 256 KB of stack. (2) A missing key was APPENDED AT THE END OF THE FILE, i.e.
// into [OTHER_SETTINGS], where game.cc (which reads "MAIN") never looks. Keys are now matched only
// inside [MAIN] and a new one goes in right after [MAIN]'s last line.
#define MODS_RES_MAX_LINES 1024

// "[MAIN]" (any case, any leading blanks) -> true; any section header sets *isHeader.
static bool mods_res_line_is_main(const char* line, bool* isHeader)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    *isHeader = *line == '[';
    return *isHeader && SDL_strncasecmp(line, "[MAIN]", 6) == 0;
}

static void mods_write_res_keys(const char* key1, int value1, const char* key2, int value2)
{
    const char* keys[2] = { key1, key2 };
    int values[2] = { value1, value2 };
    static char lines[MODS_RES_MAX_LINES][256];
    int count = 0;

    FILE* in = fopen("f1_res.ini", "rt");
    if (in != NULL) {
        while (count < MODS_RES_MAX_LINES && fgets(lines[count], sizeof(lines[0]), in) != NULL) {
            count++;
        }

        // ⚠️ A FILE BIGGER THAN THE BUFFER IS LEFT ALONE, not written back half-length. Rewriting
        // what we managed to read would silently delete the rest of someone's config, and a
        // setting that does not stick is a complaint; a truncated file is a disaster.
        char overflow[8];
        bool tooLong = (count == MODS_RES_MAX_LINES && fgets(overflow, sizeof(overflow), in) != NULL);
        fclose(in);

        if (tooLong) {
            return;
        }
    }

    // Where [MAIN] is: its header line, and the line a new key goes before (just past its last
    // non-blank line, so it does not land after the blank gap in front of the next section).
    int mainHeader = -1;
    int mainInsert = -1;
    bool inMain = false;
    for (int i = 0; i < count; i++) {
        bool isHeader;
        bool isMain = mods_res_line_is_main(lines[i], &isHeader);
        if (isHeader) {
            if (inMain) {
                break; // [MAIN] ended; the parser keeps the first [MAIN], so a later one is moot
            }
            if (isMain) {
                mainHeader = i;
                mainInsert = i + 1;
                inMain = true;
            }
            continue;
        }
        if (inMain) {
            const char* p = lines[i];
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
                p++;
            }
            if (*p != '\0') {
                mainInsert = i + 1;
            }
        }
    }

    bool replaced[2] = { false, false };
    for (int k = 0; k < 2; k++) {
        if (keys[k] == NULL) {
            replaced[k] = true; // nothing asked for, nothing to append
            continue;
        }
        if (mainHeader == -1) {
            continue; // no [MAIN]: nothing to replace, it gets written fresh below
        }

        size_t keyLen = strlen(keys[k]);
        for (int i = mainHeader + 1; i < count; i++) {
            bool isHeader;
            mods_res_line_is_main(lines[i], &isHeader);
            if (isHeader) {
                break;
            }

            const char* p = lines[i];
            while (*p == ' ' || *p == '\t' || *p == ';') {
                p++;
            }
            if (strncmp(p, keys[k], keyLen) == 0 && (p[keyLen] == '=' || p[keyLen] == ' ')) {
                snprintf(lines[i], sizeof(lines[0]), "%s=%d\n", keys[k], values[k]);
                replaced[k] = true;
                break; // a second occurrence would be dead anyway: the parser keeps the first
            }
        }
    }

    FILE* out = fopen("f1_res.ini", "wt");
    if (out == NULL) {
        return; // read-only install: the setting simply does not stick, and nothing is damaged
    }

    if (count == 0) {
        // No file at all (the game then runs on its built-in default, 480 lines on the desktop's
        // size): write one that still says what the numbers mean.
        fprintf(out, "[MAIN]\n; SCR_* is the output; INTERNAL_HEIGHT is what the game renders at,\n"
                     "; and 480 is the element size the game shipped with.\n");
    }

    for (int i = 0; i <= count; i++) {
        if (i == mainInsert) {
            // A line with no newline of its own (the last one of a file) would otherwise get the
            // first added key glued onto the end of it.
            if (i > 0 && lines[i - 1][0] != '\0' && strchr(lines[i - 1], '\n') == NULL) {
                fputc('\n', out);
            }
            for (int k = 0; k < 2; k++) {
                if (!replaced[k]) {
                    fprintf(out, "%s=%d\n", keys[k], values[k]);
                    replaced[k] = true;
                }
            }
        }
        if (i < count) {
            fputs(lines[i], out);
        }
    }

    // No [MAIN] anywhere (a file with only other sections, or the fresh one above): the keys go at
    // the end, under a [MAIN] of their own when the file had other sections for them to fall into.
    bool needMain = false;
    for (int k = 0; k < 2; k++) {
        needMain = needMain || !replaced[k];
    }
    if (needMain) {
        if (count > 0) {
            if (lines[count - 1][0] != '\0' && strchr(lines[count - 1], '\n') == NULL) {
                fputc('\n', out);
            }
            fprintf(out, "\n[MAIN]\n");
        }
        for (int k = 0; k < 2; k++) {
            if (!replaced[k]) {
                fprintf(out, "%s=%d\n", keys[k], values[k]);
            }
        }
    }

    fclose(out);
}

// What the NEXT start will use: the file's own answer, not the running one, because that is what
// the row is about. No key (or a nonsense one) means whatever we are rendering at now.
static int mods_read_internal_height()
{
    int height = 0;

    Config res;
    if (config_init(&res)) {
        config_load(&res, "f1_res.ini", false);
        if (!config_get_value(&res, "MAIN", "INTERNAL_HEIGHT", &height)) {
            height = 0;
        }
        config_exit(&res);
    }

    if (height < 480) {
        height = screenGetHeight();
    }

    return height;
}
static int mods_win = -1;

// Read the live values, not the cfg: another screen (or a future console command) may have moved
// them since the file was written, and the row must show what the game is actually doing.
static void mods_load()
{
    float travel = anim_get_travel_speed();
    mods_value[MODS_TRAVEL_SPEED] = 0;
    for (int i = 0; i < 4; i++) {
        // Nearest match rather than equality: the cfg is a free-form number and a hand-edited
        // 0.72 has to land on SOME row rather than silently reading as "Normal".
        float d = travel - mods_travel_values[i];
        float best = travel - mods_travel_values[mods_value[MODS_TRAVEL_SPEED]];
        if (d < 0.0f) d = -d;
        if (best < 0.0f) best = -best;
        if (d < best) {
            mods_value[MODS_TRAVEL_SPEED] = i;
        }
    }

    mods_value[MODS_DOOR_MAGNET] = combat_rt_get_door_magnet() > 0.0f ? 1 : 0;
    mods_value[MODS_NPC_SLIDE] = combat_rt_get_npc_slide() ? 1 : 0;
    mods_value[MODS_KNOCKDOWN_AGILITY] = combat_rt_get_knockdown_agility() > 0.0f ? 1 : 0;
    mods_value[MODS_CAM_SMOOTH] = combat_rt_get_cam_smooth() > 0.0f ? 1 : 0;
    mods_value[MODS_FREE_MOVE] = combat_rt_get_free_move() ? 1 : 0;
    mods_value[MODS_RADAWAY] = critter_get_radaway_cancels() ? 1 : 0;
    mods_value[MODS_RELOAD_ANIM] = anim_get_reload_anim() ? 1 : 0;
    mods_value[MODS_LOOK_AROUND] = combat_rt_get_look_around() ? 1 : 0;
    mods_value[MODS_SCRIPT_TURNS] = combat_rt_get_script_turns() ? 1 : 0;
    mods_value[MODS_SMOOTH_MOTION] = anim_get_smooth_motion() ? 1 : 0;
    mods_value[MODS_VSYNC] = svga_get_vsync() ? 1 : 0;
    mods_value[MODS_COMBAT_BOX_SOUND] = intface_get_combat_box_sound() ? 1 : 0;
    mods_value[MODS_CAM_LEAN] = combat_rt_get_cam_lean() ? 1 : 0;
    mods_value[MODS_SCRIPT_FLOAT] = gdialog_get_script_float() ? 1 : 0;
    mods_value[MODS_HIT_STAGGER] = combat_rt_get_flinch_chance() > 0.0f ? 1 : 0;
    mods_value[MODS_MOUSE] = combat_rt_get_mouse_hold_walk() ? 1 : 0;
    mods_value[MODS_FORGIVE_STRAY] = combat_rt_get_forgive_stray() ? 1 : 0;
    mods_value[MODS_CROWD_ROOM] = combat_rt_get_crowd_make_room() ? 1 : 0;
    mods_value[MODS_TACTICAL_AI] = combat_rt_get_tactical_ai() ? 1 : 0;
    mods_value[MODS_PROJECTILES] = combat_rt_get_projectiles() ? 1 : 0;

    {
        // The OUTPUT the file asks for, matched against the sizes the row offers; anything else
        // (a Deck, a Vita, a hand-typed size) reads as "Desktop", which is what it behaves like.
        int outW = 0;
        int outH = 0;
        Config res;
        if (config_init(&res)) {
            config_load(&res, "f1_res.ini", false);
            config_get_value(&res, "MAIN", "SCR_WIDTH", &outW);
            config_get_value(&res, "MAIN", "SCR_HEIGHT", &outH);
            config_exit(&res);
        }

        mods_value[MODS_SCREEN_RES] = 3;
        for (int i = 0; i < 3; i++) {
            if (outW == mods_screen_sizes[i][0] && outH == mods_screen_sizes[i][1]) {
                mods_value[MODS_SCREEN_RES] = i;
                break;
            }
        }
    }

    {
        int height = mods_read_internal_height();
        mods_value[MODS_INTERNAL_RES] = 2; // anything unrecognised reads as "Native"
        for (int i = 0; i < 2; i++) {
            if (height == mods_res_heights[i]) {
                mods_value[MODS_INTERNAL_RES] = i;
                break;
            }
        }
    }

    char* lang = NULL;
    mods_value[MODS_LANGUAGE] = 0;
    if (config_get_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY, &lang)
        && lang != NULL && strcmp(lang, CZECH) == 0) {
        mods_value[MODS_LANGUAGE] = 1;
    }
}

// Apply one row: live value first, then the cfg. The cfg write is what survives the session; the
// live write is what makes the setting mean something before you close the screen.
static void mods_apply(int index)
{
    switch (index) {
    case MODS_TRAVEL_SPEED: {
        float v = mods_travel_values[mods_value[index]];
        anim_set_travel_speed(v);
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "travel_speed", v);
        break;
    }
    case MODS_DOOR_MAGNET: {
        // 0.7 is the strength the cfg default and the play-tests used; the screen only offers
        // on/off because "how strongly" is not a question a player can answer without a number.
        float v = mods_value[index] != 0 ? 0.7f : 0.0f;
        combat_rt_set_door_magnet(v);
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "door_magnet", v);
        break;
    }
    case MODS_NPC_SLIDE: {
        bool v = mods_value[index] != 0;
        combat_rt_set_npc_slide(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "npc_slide", v ? 1 : 0);
        break;
    }
    case MODS_KNOCKDOWN_AGILITY: {
        // 0.06 is the tuned strength -- roughly 2.1 s on the ground at Agility 1 against 1.2 s at
        // Agility 10, with the average character unchanged at the flat 1.7 s. On/off only, for the
        // same reason the doorway assist is: "how strongly" is not a question a player can answer
        // without being shown the number.
        float v = mods_value[index] != 0 ? 0.06f : 0.0f;
        combat_rt_set_knockdown_agility(v);
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "knockdown_agility_factor", v);
        break;
    }
    case MODS_CAM_SMOOTH: {
        // 10 is the tuned time constant -- about 100 ms, which is long enough to swallow the
        // per-frame jitter an attack swing or an unsmoothed run frame puts into the view, and
        // short enough that the camera is back on him the moment he stops. Off = the hard lock.
        float v = mods_value[index] != 0 ? 10.0f : 0.0f;
        combat_rt_set_cam_smooth(v);
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, "cam_smooth", v);
        break;
    }
    case MODS_FREE_MOVE: {
        bool v = mods_value[index] != 0;
        combat_rt_set_free_move(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "free_move", v ? 1 : 0);
        break;
    }
    case MODS_SCREEN_RES: {
        // ⚠️ THE OUTPUT, not what the game renders at -- that is the row below. Same file, same
        // "restart to apply", because game.cc reads f1_res.ini once before anything exists.
        int w = mods_screen_sizes[mods_value[index]][0];
        int h = mods_screen_sizes[mods_value[index]][1];

        if (w == 0 || h == 0) {
            // "Desktop": ask SDL what the display actually is, so a handheld gets its own size
            // (1280x800 on a Steam Deck) without anyone having to type it.
            SDL_DisplayMode mode;
            if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w >= 640 && mode.h >= 480) {
                w = mode.w;
                h = mode.h;
            } else {
                w = 640;
                h = 480;
            }
        }

        mods_write_res_keys("SCR_WIDTH", w, "SCR_HEIGHT", h);
        break;
    }
    case MODS_INTERNAL_RES: {
        // ⚠️ NOT fallout.cfg. The renderer's size is read ONCE, out of f1_res.ini, before anything
        // else exists (game.cc) -- so this writes there, and it is the one row on this screen that
        // cannot take effect until the game is restarted. The row says so on every value.
        int height = mods_res_heights[mods_value[index]];

        if (height == 0) {
            // "Native" -- the screen's own height, which only f1_res.ini knows.
            Config res;
            if (config_init(&res)) {
                config_load(&res, "f1_res.ini", false);
                if (!config_get_value(&res, "MAIN", "SCR_HEIGHT", &height) || height < 480) {
                    height = screenGetHeight();
                }
                config_exit(&res);
            } else {
                height = screenGetHeight();
            }
        }

        mods_write_res_keys("INTERNAL_HEIGHT", height, NULL, 0);
        break;
    }
    case MODS_LANGUAGE: {
        // â ï¸ REFUSE A LANGUAGE THAT IS NOT THERE. Every .msg lookup is text\<language>\<file>, so
        // pointing this at a folder that does not exist does not fall back to English -- it fails
        // every message load, and the game comes up with no text at all. Check one file the engine
        // itself always reads before letting the setting through.
        bool czech = mods_value[index] != 0;

        if (czech) {
            DB_FILE* probe = db_fopen("text\\czech\\game\\misc.msg", "rt");
            if (probe != NULL) {
                db_fclose(probe);
            } else {
                mods_value[index] = 0;
                gsound_play_sfx_file("iisxxxx1");
                break;
            }
        }

        // â ï¸ NOT LIVE. Message files are read once at startup and at map load, so this only writes
        // the cfg -- the row says so. Reloading them under a running game would mean re-reading
        // every message list in the engine and re-laying out every screen that cached a string.
        config_set_string(&game_config, GAME_CONFIG_SYSTEM_KEY, GAME_CONFIG_LANGUAGE_KEY,
            czech ? CZECH : ENGLISH);
        break;
    }
    case MODS_RADAWAY: {
        bool v = mods_value[index] != 0;
        critter_set_radaway_cancels(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "radaway_cancels", v ? 1 : 0);
        break;
    }
    case MODS_LOOK_AROUND: {
        // Default OFF like every row here: vanilla's fixed stare is how the game shipped. Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_look_around(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "enemies_look_around", v ? 1 : 0);
        break;
    }
    case MODS_SCRIPT_TURNS: {
        // ⚠️ DEFAULT ON, the one row that is: here ON is the vanilla behaviour (scripts get their
        // combat turns) and OFF is what realtime did by accident until 2026-10-05. Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_script_turns(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "script_turns", v ? 1 : 0);
        break;
    }
    case MODS_SMOOTH_MOTION: {
        // Default OFF like every row here: hopping once per frame is how the game shipped. Live --
        // switching off lands every glide in flight on its real spot at the next tick.
        bool v = mods_value[index] != 0;
        anim_set_smooth_motion(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SMOOTH_MOTION_KEY, v ? 1 : 0);
        break;
    }
    case MODS_VSYNC: {
        // Default OFF like every row here. Live: the next present switches the swap interval.
        bool v = mods_value[index] != 0;
        svga_set_vsync(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VSYNC_KEY, v ? 1 : 0);
        break;
    }
    case MODS_CAM_LEAN: {
        // Default OFF like every row here: the camera never left the dude in the shipped game. Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_cam_lean(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CAM_LOCK_LEAN_KEY, v ? 1 : 0);
        break;
    }
    case MODS_MOUSE: {
        // DEFAULT "Arrow walks" -- his design (2026-10-14, moved into the arrow cursor 2026-10-15), ON. Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_mouse_hold_walk(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MOUSE_HOLD_WALK_KEY, v ? 1 : 0);
        break;
    }
    case MODS_FORGIVE_STRAY: {
        // DEFAULT "Forgive once" -- his call (2026-10-15), ON without being vanilla. Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_forgive_stray(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_FORGIVE_STRAY_KEY, v ? 1 : 0);
        break;
    }
    case MODS_CROWD_ROOM: {
        // DEFAULT ON -- his call (2026-10-17, "posun + vystřídání, na všechny"). Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_crowd_make_room(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CROWD_MAKE_ROOM_KEY, v ? 1 : 0);
        break;
    }
    case MODS_TACTICAL_AI: {
        // DEFAULT ON -- his call (2026-10-17, "jdeme implementovat vše"). Live.
        bool v = mods_value[index] != 0;
        combat_rt_set_tactical_ai(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TACTICAL_AI_KEY, v ? 1 : 0);
        break;
    }
    case MODS_PROJECTILES: {
        // DEFAULT ON -- his design ("mít prostě projektily", 2026-10-18). Live: the next shot reads it.
        bool v = mods_value[index] != 0;
        combat_rt_set_projectiles(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PROJECTILES_KEY, v ? 1 : 0);
        break;
    }
    case MODS_HIT_STAGGER: {
        // ⚠️ DEFAULT "By hit strength" -- his pick of three ("brnění + šance", 2026-10-10), so like
        // Script lines as floating text it is ON without being vanilla. "Always" writes 0, which is
        // exactly the old every-hit-through-armour stagger. Live: the next hit reads it.
        float factor = mods_value[index] != 0 ? 2.5f : 0.0f;
        combat_rt_set_flinch_chance(factor);
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_FLINCH_CHANCE_FACTOR_KEY, factor);
        break;
    }
    case MODS_SCRIPT_FLOAT: {
        // ⚠️ DEFAULT ON -- his explicit call, the one exception to "defaults are what shipped" that
        // is not vanilla itself (see gdialog_script_float). Live: the next such line reads it.
        bool v = mods_value[index] != 0;
        gdialog_set_script_float(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SCRIPT_FLOAT_KEY, v ? 1 : 0);
        break;
    }
    case MODS_COMBAT_BOX_SOUND: {
        // ⚠️ DEFAULT ON, like Script combat turns: ON is how the game shipped. Live.
        bool v = mods_value[index] != 0;
        intface_set_combat_box_sound(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_BOX_SOUND_KEY, v ? 1 : 0);
        break;
    }
    case MODS_RELOAD_ANIM: {
        // Default OFF like every row here: the instant reload is how the game shipped. Live -- the
        // next LB tap and the next idle roll both read it.
        bool v = mods_value[index] != 0;
        anim_set_reload_anim(v);
        config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, "reload_anim", v ? 1 : 0);
        break;
    }
    }
}

// Fit the window to the screen, then fit the rows to the window. â ï¸ THE ROWS ARE LAID OUT FROM THE
// SPACE THAT IS LEFT, not from a constant: this screen has grown from three settings to eight
// without anyone touching the geometry, and a fixed row pitch is exactly how the ninth would have
// been drawn off the bottom edge with nothing complaining.
static void mods_layout()
{
    mods_w = screenGetWidth() - 80;
    if (mods_w > MODS_W_MAX) {
        mods_w = MODS_W_MAX;
    } else if (mods_w < MODS_W_MIN) {
        mods_w = MODS_W_MIN;
    }

    mods_value_x = mods_w - MODS_VALUE_GAP;

    text_font(101);
    int lineH = text_height();

    int headH = 16 + lineH * 2 + 16; // title, subtitle, rule
    int footH = lineH * 3 + 34; // two help lines and the key hint

    // The HEIGHT FOLLOWS THE CONTENT and is only then capped by the screen -- a fixed height either
    // leaves a hole under a short list or runs a long one off the bottom, and this screen has
    // already grown from three rows to eight without anyone revisiting the number.
    int pitch = lineH + 16;
    int want = headH + MODS_COUNT * pitch + footH;

    int cap = screenGetHeight() - 40;
    if (cap > MODS_H_MAX) {
        cap = MODS_H_MAX;
    }
    if (cap < MODS_H_MIN) {
        cap = MODS_H_MIN;
    }

    if (want > cap) {
        // More rows than the screen can seat: tighten the spacing instead of drawing past the edge.
        pitch = (cap - headH - footH) / MODS_COUNT;
        if (pitch < lineH + 2) {
            pitch = lineH + 2;
        }
        want = headH + MODS_COUNT * pitch + footH;
    }

    mods_h = want < MODS_H_MIN ? MODS_H_MIN : want;
    mods_row_h = pitch;
    mods_first_row = headH + (mods_h - headH - footH - MODS_COUNT * pitch) / 2;
}

static void mods_draw()
{
    if (mods_win == -1) {
        return;
    }

    unsigned char* buf = win_get_buf(mods_win);
    int pitch = mods_w;

    buf_fill(buf, mods_w, mods_h, pitch, colorTable[0]);
    draw_box(buf, pitch, 0, 0, mods_w - 1, mods_h - 1, colorTable[992]);

    text_font(101);
    int lineH = text_height();

    const char* title = "MODS";
    text_to_buf(buf + 16 * pitch + (mods_w - text_width(title)) / 2, title, pitch, pitch,
        colorTable[32767]);

    const char* sub = "Extras this build added. A change applies the moment you make it.";
    text_to_buf(buf + (16 + lineH + 2) * pitch + (mods_w - text_width(sub)) / 2, sub, pitch,
        pitch, colorTable[992]);

    draw_line(buf, pitch, MODS_PAD, mods_first_row - 10, mods_w - MODS_PAD, mods_first_row - 10,
        colorTable[992]);

    for (int i = 0; i < MODS_COUNT; i++) {
        int y = mods_first_row + i * mods_row_h;
        bool on = i == mods_sel;

        // The selected row is the one you can change, so it is the one that gets the arrows --
        // drawing them on every row would say every row is live at once.
        text_to_buf(buf + y * pitch + MODS_PAD, mods_options[i].label, pitch, pitch,
            on ? colorTable[32767] : colorTable[992]);

        char value[64];
        if (on) {
            snprintf(value, sizeof(value), "< %s >", mods_options[i].values[mods_value[i]]);
        } else {
            snprintf(value, sizeof(value), "%s", mods_options[i].values[mods_value[i]]);
        }
        text_to_buf(buf + y * pitch + mods_value_x, value, pitch, pitch,
            on ? colorTable[32767] : colorTable[992]);
    }

    // Help band for the CURRENT value of the CURRENT row, anchored to the BOTTOM of the window so
    // it does not wander up and down as the row count changes.
    int hintY = mods_h - lineH - 12;
    int help2Y = hintY - lineH - 12;
    int help1Y = help2Y - lineH - 2;

    draw_line(buf, pitch, MODS_PAD, help1Y - 10, mods_w - MODS_PAD, help1Y - 10, colorTable[992]);

    const char* help = mods_options[mods_sel].help[mods_value[mods_sel]];
    if (help != NULL) {
        text_to_buf(buf + help1Y * pitch + MODS_PAD, help, pitch, pitch, colorTable[992]);
    }

    const char* help2 = mods_options[mods_sel].help2[mods_value[mods_sel]];
    if (help2 != NULL) {
        text_to_buf(buf + help2Y * pitch + MODS_PAD, help2, pitch, pitch, colorTable[992]);
    }

    const char* hint = "D-pad / arrows move    LB RB / left right change    B or ESC done";
    text_to_buf(buf + hintY * pitch + MODS_PAD, hint, pitch, pitch, colorTable[992]);

    win_draw(mods_win);
}

void mods_page()
{
    mods_layout();

    int x = (screenGetWidth() - mods_w) / 2;
    int y = (screenGetHeight() - mods_h) / 2;

    mods_win = win_add(x, y, mods_w, mods_h, colorTable[0], WINDOW_MOVE_ON_TOP);
    if (mods_win == -1) {
        return;
    }

    mods_sel = 0;
    mods_load();

    // Same deal every modal screen on this pad makes: tell the gamepad layer that this screen
    // drives its own navigation, so A is not synthesised into a world click behind it and the
    // right stick does not go on steering a cursor we are not showing.
    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad != NULL) {
        gamepad_ui_set_self_nav(true);
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
    }

    mods_draw();

    bool prevUp = false;
    bool prevDown = false;
    bool prevLeft = false;
    bool prevRight = false;
    bool prevB = false;
    bool prevLB = false;
    bool prevRB = false;

    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();
        int move = 0;
        int step = 0;

        switch (keyCode) {
        case KEY_ESCAPE:
        case KEY_RETURN:
            done = true;
            break;
        case KEY_ARROW_UP:
            move = -1;
            break;
        case KEY_ARROW_DOWN:
            move = 1;
            break;
        case KEY_ARROW_LEFT:
            step = -1;
            break;
        case KEY_ARROW_RIGHT:
            step = 1;
            break;
        }

        // Read the pad directly. gamepad_ui_tick does not run inside a modal loop like this one --
        // the same reason the dialogue and inventory screens read SDL themselves.
        if (pad != NULL) {
            SDL_GameControllerUpdate();

            bool up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
            bool down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
            bool left = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
            bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
            bool lb = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            bool rb = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
            bool b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;

            if (up && !prevUp) move = -1;
            if (down && !prevDown) move = 1;
            // ...and so does the left stick, which is where the thumb already is.
            if (move == 0) move = gamepad_ui_menu_stick_step();
            // Both pairs change the value: LB/RB is what the character sheet and barter already use
            // for "step this", and left/right is what anyone will try first.
            if ((left && !prevLeft) || (lb && !prevLB)) step = -1;
            if ((right && !prevRight) || (rb && !prevRB)) step = 1;
            if (b && !prevB) done = true;

            prevUp = up;
            prevDown = down;
            prevLeft = left;
            prevRight = right;
            prevLB = lb;
            prevRB = rb;
            prevB = b;
        }

        if (move != 0) {
            mods_sel += move;
            if (mods_sel < 0) {
                mods_sel = MODS_COUNT - 1;
            } else if (mods_sel >= MODS_COUNT) {
                mods_sel = 0;
            }
            gsound_play_sfx_file("ib1p1xx1");
            mods_draw();
        }

        if (step != 0) {
            int count = mods_options[mods_sel].count;
            mods_value[mods_sel] = (mods_value[mods_sel] + step + count) % count;
            mods_apply(mods_sel);
            gsound_play_sfx_file("ib1p1xx1");
            mods_draw();
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    // Persist. Every change is already live and already in the in-memory config; this is the one
    // write to disk, done once on the way out rather than on every keypress.
    gconfig_save();

    win_delete(mods_win);
    mods_win = -1;

    if (pad != NULL) {
        gamepad_ui_set_self_nav(false);
    }
}

} // namespace fallout
