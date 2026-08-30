#include "game/options.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include <SDL.h>

#include "game/art.h"
#include "game/combat.h"
#include "game/combatai.h"
#include "game/cycle.h"
#include "game/game.h"
#include "game/gamepad_ui.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/graphlib.h"
#include "game/gsound.h"
#include "game/loadsave.h"
#include "game/message.h"
#include "game/scripts.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/crt_shader.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define PREFERENCES_WINDOW_WIDTH 640
#define PREFERENCES_WINDOW_HEIGHT 480

#define OPTIONS_WINDOW_BUTTONS_COUNT 10
#define PRIMARY_OPTION_VALUE_COUNT 4
#define SECONDARY_OPTION_VALUE_COUNT 2

// Label for our CRT checkbox. options.msg is Interplay's and comes out of MASTER.DAT, so
// there is no message id to add one under -- this string is literal on purpose.
#define CRT_PREF_LABEL "CRT FILTER"

#define GAMMA_MIN 1.0
#define GAMMA_MAX 1.17999267578125
#define GAMMA_STEP 0.01124954223632812

typedef enum Preference {
    PREF_GAME_DIFFICULTY,
    PREF_COMBAT_DIFFICULTY,
    PREF_VIOLENCE_LEVEL,
    PREF_TARGET_HIGHLIGHT,
    PREF_RUNNING_BURNING_GUY,
    PREF_COMBAT_MESSAGES,
    PREF_COMBAT_TAUNTS,
    PREF_LANGUAGE_FILTER,
    PREF_RUNNING,
    PREF_SUBTITLES,
    PREF_ITEM_HIGHLIGHT,
    PREF_COMBAT_SPEED,
    PREF_TEXT_BASE_DELAY,
    PREF_MASTER_VOLUME,
    PREF_MUSIC_VOLUME,
    PREF_SFX_VOLUME,
    PREF_SPEECH_VOLUME,
    PREF_BRIGHTNESS,
    PREF_MOUSE_SENSITIVIY,
    PREF_COUNT,
    FIRST_PRIMARY_PREF = PREF_GAME_DIFFICULTY,
    LAST_PRIMARY_PREF = PREF_RUNNING_BURNING_GUY,
    PRIMARY_PREF_COUNT = LAST_PRIMARY_PREF - FIRST_PRIMARY_PREF + 1,
    FIRST_SECONDARY_PREF = PREF_COMBAT_MESSAGES,
    LAST_SECONDARY_PREF = PREF_ITEM_HIGHLIGHT,
    SECONDARY_PREF_COUNT = LAST_SECONDARY_PREF - FIRST_SECONDARY_PREF + 1,
    FIRST_RANGE_PREF = PREF_COMBAT_SPEED,
    LAST_RANGE_PREF = PREF_MOUSE_SENSITIVIY,
    RANGE_PREF_COUNT = LAST_RANGE_PREF - FIRST_RANGE_PREF + 1,
} Preference;

typedef enum PauseWindowFrm {
    PAUSE_WINDOW_FRM_BACKGROUND,
    PAUSE_WINDOW_FRM_DONE_BOX,
    PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_UP,
    PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN,
    PAUSE_WINDOW_FRM_COUNT,
} PauseWindowFrm;

typedef enum OptionsWindowFrm {
    OPTIONS_WINDOW_FRM_BACKGROUND,
    OPTIONS_WINDOW_FRM_BUTTON_ON,
    OPTIONS_WINDOW_FRM_BUTTON_OFF,
    OPTIONS_WINDOW_FRM_COUNT,
} OptionsWindowFrm;

typedef enum PreferencesWindowFrm {
    PREFERENCES_WINDOW_FRM_BACKGROUND,
    // Knob (for range preferences)
    PREFERENCES_WINDOW_FRM_KNOB_OFF,
    // 4-way switch (for primary preferences)
    PREFERENCES_WINDOW_FRM_PRIMARY_SWITCH,
    // 2-way switch (for secondary preferences)
    PREFERENCES_WINDOW_FRM_SECONDARY_SWITCH,
    PREFERENCES_WINDOW_FRM_CHECKBOX_ON,
    PREFERENCES_WINDOW_FRM_CHECKBOX_OFF,
    PREFERENCES_WINDOW_FRM_6,
    // Active knob (for range preferences)
    PREFERENCES_WINDOW_FRM_KNOB_ON,
    PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP,
    PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN,
    PREFERENCES_WINDOW_FRM_COUNT,
} PreferencesWindowFrm;

typedef struct PreferenceDescription {
    // The number of options.
    short valuesCount;

    // Direction of rotation:
    // 0 - clockwise (incrementing value),
    // 1 - counter-clockwise (decrementing value)
    short direction;
    short knobX;
    short knobY;
    // Min x coordinate of the preference control bounding box.
    short minX;
    // Max x coordinate of the preference control bounding box.
    short maxX;
    short labelIds[PRIMARY_OPTION_VALUE_COUNT];
    int btn;
    char name[32];
    double minValue;
    double maxValue;
    int* valuePtr;
} PreferenceDescription;

static int OptnStart();
static int OptnEnd();
static void ShadeScreen(bool a1);
static int do_prefscreen();
static int PrefStart();
static void DoThing(int eventCode);
static void UpdateThing(int index);
static int PrefEnd();
static void SetSystemPrefs();
static int SavePrefs(bool save);
static void SetDefaults(bool a1);
static void SaveSettings();
static void RestoreSettings();
static void JustUpdate();

// 0x4812B0
static const short row1Ytab[PRIMARY_PREF_COUNT] = {
    48,
    125,
    203,
    286,
    363,
};

// 0x4812BA
static const short row2Ytab[SECONDARY_PREF_COUNT] = {
    49,
    116,
    181,
    247,
    313,
    380,
};

// 0x4812C6
static const short row3Ytab[RANGE_PREF_COUNT] = {
    19,
    94,
    165,
    216,
    268,
    319,
    369,
    420,
};

// x offsets for primary preferences from the knob position
// 0x4812D6
static const short bglbx[PRIMARY_OPTION_VALUE_COUNT] = {
    2,
    25,
    46,
    46,
};

// y offsets for primary preference option values from the knob position
// 0x4812DE
static const short bglby[PRIMARY_OPTION_VALUE_COUNT] = {
    10,
    -4,
    10,
    31,
};

// x offsets for secondary prefrence option values from the knob position
// 0x48FC06
static const short smlbx[SECONDARY_OPTION_VALUE_COUNT] = {
    4,
    21,
};

// 0x505D30
static int opgrphs[OPTIONS_WINDOW_FRM_COUNT] = {
    220, // opbase.frm - character editor
    222, // opbtnon.frm - character editor
    221, // opbtnoff.frm - character editor
};

// 0x505D3C
static int prfgrphs[PREFERENCES_WINDOW_FRM_COUNT] = {
    240, // prefscrn.frm - options screen
    241, // prfsldof.frm - options screen
    242, // prfbknbs.frm - options screen
    243, // prflknbs.frm - options screen
    244, // prfxin.frm - options screen
    245, // prfxout.frm - options screen
    246, // prefcvr.frm - options screen
    247, // prfsldon.frm - options screen
    8, // lilredup.frm - little red button up
    9, // lilreddn.frm - little red button down
};

// 0x661D30
static Size ginfo[OPTIONS_WINDOW_FRM_COUNT];

// 0x661D48
static MessageList optn_msgfl;

// 0x661D50
static Size ginfo2[PREFERENCES_WINDOW_FRM_COUNT];

// 0x661DA0
static unsigned char* prfbmp[PREFERENCES_WINDOW_FRM_COUNT];

// 0x661DC8
static unsigned char* opbtns[OPTIONS_WINDOW_BUTTONS_COUNT];

// 0x661DF0
static CacheEntry* grphkey2[PREFERENCES_WINDOW_FRM_COUNT];

// 0x661E18
static double text_delay_back;

// 0x661E20
static double gamma_value_back;

// 0x661E28
static double mouse_sens_back;

// 0x661E30
static double gamma_value;

// 0x661E38
static double text_delay;

// 0x661E40
static double mouse_sens;

// 0x661E48
static unsigned char* winbuf;

// 0x661E4C
static unsigned char* prefbuf;

// 0x661E50
static MessageListItem optnmesg;

// 0x661E5C
static CacheEntry* grphkey[OPTIONS_WINDOW_FRM_COUNT];

// 0x661E68
static bool mouse_3d_was_on;

// 0x661E6C
static int optnwin;

// Button ids for the option rows (Save/Load/Preferences/Exit/Done), so the gamepad
// nav can highlight the selected one by swapping its up-image to the "on" (yellow)
// FRM via win_register_button_image (which survives GNW_button_refresh, unlike drawing
// into the window buffer).
#define OPTIONS_MENU_COUNT (OPTIONS_WINDOW_BUTTONS_COUNT / 2)
static int optnbtn[OPTIONS_MENU_COUNT];

// Show option `sel` highlighted (its "on" FRM as the resting image) and the rest
// normal. Controller-nav only; harmless for mouse users (never called then).
static void options_gamepad_highlight(int sel)
{
    for (int i = 0; i < OPTIONS_MENU_COUNT; i++) {
        if (optnbtn[i] == -1) {
            continue;
        }
        unsigned char* up = (i == sel) ? opbtns[i * 2 + 1] : opbtns[i * 2];
        win_register_button_image(optnbtn[i], up, opbtns[i * 2 + 1], NULL, true);
    }
}

// Auto-hide the options cursor (controller only): hidden by default, revealed while
// the right stick / mouse moves it, re-hidden after idle. Blank MOUSE_CURSOR_NONE
// shape, not mouse_hide() (which would gate the input hook). Same pattern as the
// main menu / load-save screens.
#define OPTIONS_CURSOR_IDLE_MS 2000

static void options_tick_cursor(bool* shown, int* lastX, int* lastY, unsigned int* lastMove)
{
    int x;
    int y;
    mouse_get_position(&x, &y);

    if (x != *lastX || y != *lastY) {
        *lastMove = get_time();
        if (!*shown) {
            gmouse_set_cursor(MOUSE_CURSOR_ARROW);
            *shown = true;
        }
    } else if (*shown && elapsed_time(*lastMove) >= OPTIONS_CURSOR_IDLE_MS) {
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        *shown = false;
    }

    *lastX = x;
    *lastY = y;
}

// 0x661E70
static int prfwin;

// 0x661E74
static unsigned char* opbmp[OPTIONS_WINDOW_FRM_COUNT];

// This array stores backup for only integer-representable prefs. Because
// `player_speedup` is not a part of prefs enum, it's value is stored in
// `PREF_TEXT_BASE_DELAY`.
//
// 0x661E80
static int settings_backup[PREF_COUNT];

// 0x661ECC
static int fontsave;

// 0x661ED0
static int plyrspdbid;

// CRT FILTER checkbox -- the second checkbox on the Preferences screen, sharing the free
// band the "Affect player speed" one sits in. Like player_speedup it is outside the
// Preference enum, so its backup lives in its own variable rather than settings_backup.
static int crtbid = -1;
static int crt_filter;
static int crt_filter_back;

// Write the CRT flag straight through to fallout.cfg the moment it is flipped, instead of only
// when the screen is left with DONE.
//
// It is a LOOK setting: you toggle it to see the screen change, and the natural way out after
// looking is B / ESC -- which restores the value the screen opened with and threw the change away.
// That is what "it forgets whether the filter was on" was. Persisting on the toggle makes it
// impossible to lose; JustUpdate on the cancel path re-persists the restored value, so B still
// undoes it properly.
static void crt_persist()
{
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_KEY, crt_filter);
    gconfig_save();
}

// Where the CRT checkbox ended up (it is placed after the "Affect player speed" label,
// whose width depends on the language) and where the Preferences window sits on screen.
// Both are needed by the controller navigation, which parks the mouse cursor on the
// selected control -- see prefs_nav_move.
static int crt_checkbox_x = 520;
static int prfwin_x;
static int prfwin_y;

// 0x661ED4
static bool changed;

// 0x661ED8
static int sndfx_volume;

// 0x661EDC
static int subtitles;

// 0x661EE0
static int language_filter;

// 0x661EE4
static int speech_volume;

// 0x661EE8
static int master_volume;

// 0x661EEC
static int player_speedup;

// 0x661EF0
static int combat_taunts;

// 0x661EF4
static int combat_messages;

// 0x661EF8
static int target_highlight;

// 0x661EFC
static int music_volume;

// 0x661F00
static bool bk_enable;

// 0x661F04
static int prf_running;

// 0x661F08
static int combat_speed;

// 0x661F0C
static int item_highlight;

// 0x661F10
static int running_burning_guy;

// 0x661F14
static int combat_difficulty;

// 0x661F18
static int violence_level;

// 0x661F1C
static int game_difficulty;

// 0x505D68
static PreferenceDescription btndat[PREF_COUNT] = {
    { 3, 0, 76, 71, 0, 0, { 203, 204, 205, 0 }, 0, GAME_CONFIG_GAME_DIFFICULTY_KEY, 0, 0, &game_difficulty },
    { 3, 0, 76, 149, 0, 0, { 206, 204, 208, 0 }, 0, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, 0, 0, &combat_difficulty },
    { 4, 0, 76, 226, 0, 0, { 214, 215, 204, 216 }, 0, GAME_CONFIG_VIOLENCE_LEVEL_KEY, 0, 0, &violence_level },
    { 3, 0, 76, 309, 0, 0, { 202, 201, 213, 0 }, 0, GAME_CONFIG_TARGET_HIGHLIGHT_KEY, 0, 0, &target_highlight },
    { 2, 0, 76, 387, 0, 0, { 202, 201, 0, 0 }, 0, GAME_CONFIG_RUNNING_BURNING_GUY_KEY, 0, 0, &running_burning_guy },
    { 2, 0, 299, 74, 0, 0, { 211, 212, 0, 0 }, 0, GAME_CONFIG_COMBAT_MESSAGES_KEY, 0, 0, &combat_messages },
    { 2, 0, 299, 141, 0, 0, { 202, 201, 0, 0 }, 0, GAME_CONFIG_COMBAT_TAUNTS_KEY, 0, 0, &combat_taunts },
    { 2, 0, 299, 207, 0, 0, { 202, 201, 0, 0 }, 0, GAME_CONFIG_LANGUAGE_FILTER_KEY, 0, 0, &language_filter },
    { 2, 0, 299, 271, 0, 0, { 209, 219, 0, 0 }, 0, GAME_CONFIG_RUNNING_KEY, 0, 0, &prf_running },
    { 2, 0, 299, 338, 0, 0, { 202, 201, 0, 0 }, 0, GAME_CONFIG_SUBTITLES_KEY, 0, 0, &subtitles },
    { 2, 0, 299, 404, 0, 0, { 202, 201, 0, 0 }, 0, GAME_CONFIG_ITEM_HIGHLIGHT_KEY, 0, 0, &item_highlight },
    { 2, 0, 374, 50, 0, 0, { 207, 210, 0, 0 }, 0, GAME_CONFIG_COMBAT_SPEED_KEY, 0.0, 50.0, &combat_speed },
    { 3, 0, 374, 125, 0, 0, { 217, 209, 218, 0 }, 0, GAME_CONFIG_TEXT_BASE_DELAY_KEY, 1.0, 6.0, NULL },
    { 4, 0, 374, 196, 0, 0, { 202, 221, 209, 222 }, 0, GAME_CONFIG_MASTER_VOLUME_KEY, 0, 32767.0, &master_volume },
    { 4, 0, 374, 247, 0, 0, { 202, 221, 209, 222 }, 0, GAME_CONFIG_MUSIC_VOLUME_KEY, 0, 32767.0, &music_volume },
    { 4, 0, 374, 298, 0, 0, { 202, 221, 209, 222 }, 0, GAME_CONFIG_SNDFX_VOLUME_KEY, 0, 32767.0, &sndfx_volume },
    { 4, 0, 374, 349, 0, 0, { 202, 221, 209, 222 }, 0, GAME_CONFIG_SPEECH_VOLUME_KEY, 0, 32767.0, &speech_volume },
    { 2, 0, 374, 400, 0, 0, { 207, 223, 0, 0 }, 0, GAME_CONFIG_BRIGHTNESS_KEY, 1.0, 1.17999267578125, NULL },
    { 2, 0, 374, 451, 0, 0, { 207, 218, 0, 0 }, 0, GAME_CONFIG_MOUSE_SENSITIVITY_KEY, 1.0, 2.5, NULL },
};

// True only while the options/pause menu (do_options) is up. Lets the controller
// layer toggle it closed with the same button that opened it.
static bool options_open = false;

bool options_is_open()
{
    return options_open;
}

// 0x481328
int do_options()
{
    if (OptnStart() == -1) {
        debug_printf("\nOPTION MENU: Error loading option dialog data!\n");
        return -1;
    }

    options_open = true;

    // Gamepad navigation: highlight the selected option row and move it with the
    // D-pad / left stick, confirm with A, close with B -- the same feel as the main
    // menu, so the in-game menu is controller-driven without the jumpy snap cursor.
    SDL_GameController* optnPad = gamepad_ui_get_controller();
    int optnSel = 0;
    bool optnPrevUp = false;
    bool optnPrevDown = false;
    bool optnPrevA = false;
    bool optnPrevB = false;
    bool optnCursorShown = true;
    int optnCursorX = 0;
    int optnCursorY = 0;
    unsigned int optnCursorMove = get_time();
    if (optnPad != NULL) {
        gamepad_ui_set_self_nav(true);
        options_gamepad_highlight(optnSel);
        win_draw(optnwin);
        mouse_get_position(&optnCursorX, &optnCursorY);
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        optnCursorShown = false;
    }

    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        if (optnPad != NULL) {
            // Re-assert after a sub-screen (Save/Load/Preferences run their own loops
            // and clear self_nav / restore the cursor on exit).
            gamepad_ui_set_self_nav(true);

            SDL_GameControllerUpdate();
            bool upNow = SDL_GameControllerGetButton(optnPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
                || SDL_GameControllerGetAxis(optnPad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
            bool downNow = SDL_GameControllerGetButton(optnPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
                || SDL_GameControllerGetAxis(optnPad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
            bool aNow = SDL_GameControllerGetButton(optnPad, SDL_CONTROLLER_BUTTON_A) != 0;
            bool bNow = SDL_GameControllerGetButton(optnPad, SDL_CONTROLLER_BUTTON_B) != 0;

            if (upNow && !optnPrevUp) {
                optnSel = (optnSel + OPTIONS_MENU_COUNT - 1) % OPTIONS_MENU_COUNT;
                options_gamepad_highlight(optnSel);
                win_draw(optnwin);
                gsound_play_sfx_file("ib1p1xx1");
            } else if (downNow && !optnPrevDown) {
                optnSel = (optnSel + 1) % OPTIONS_MENU_COUNT;
                options_gamepad_highlight(optnSel);
                win_draw(optnwin);
                gsound_play_sfx_file("ib1p1xx1");
            }

            if (aNow && !optnPrevA) {
                // Trigger the highlighted row via its button event code (500 + index),
                // reusing the switch below (Save/Load/Preferences/Exit/Done).
                GNW_add_input_buffer(500 + optnSel);
            } else if (bNow && !optnPrevB) {
                GNW_add_input_buffer(KEY_ESCAPE);
            }

            optnPrevUp = upNow;
            optnPrevDown = downNow;
            optnPrevA = aNow;
            optnPrevB = bNow;

            options_tick_cursor(&optnCursorShown, &optnCursorX, &optnCursorY, &optnCursorMove);
        }

        int keyCode = get_input();
        bool showPreferences = false;

        if (keyCode == KEY_ESCAPE || keyCode == 504 || game_user_wants_to_quit != 0) {
            rc = 0;
        } else {
            switch (keyCode) {
            case KEY_RETURN:
            case KEY_UPPERCASE_O:
            case KEY_LOWERCASE_O:
            case KEY_UPPERCASE_D:
            case KEY_LOWERCASE_D:
                gsound_play_sfx_file("ib1p1xx1");
                rc = 0;
                break;
            case KEY_UPPERCASE_S:
            case KEY_LOWERCASE_S:
            case 500:
                if (SaveGame(LOAD_SAVE_MODE_NORMAL) == 1) {
                    rc = 1;
                }
                break;
            case KEY_UPPERCASE_L:
            case KEY_LOWERCASE_L:
            case 501:
                if (LoadGame(LOAD_SAVE_MODE_NORMAL) == 1) {
                    rc = 1;
                }
                break;
            case KEY_UPPERCASE_P:
            case KEY_LOWERCASE_P:
                gsound_play_sfx_file("ib1p1xx1");
                // FALLTHROUGH
            case 502:
                // PREFERENCES
                showPreferences = true;
                break;
            case KEY_PLUS:
            case KEY_EQUAL:
                IncGamma();
                break;
            case KEY_UNDERSCORE:
            case KEY_MINUS:
                DecGamma();
                break;
            }
        }

        if (showPreferences) {
            do_prefscreen();
        } else {
            switch (keyCode) {
            case KEY_F12:
                dump_screen();
                break;
            case KEY_UPPERCASE_E:
            case KEY_LOWERCASE_E:
            case KEY_CTRL_Q:
            case KEY_CTRL_X:
            case KEY_F10:
            case 503:
                game_quit_with_confirm();
                break;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (optnPad != NULL) {
        gamepad_ui_set_self_nav(false);
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    }

    OptnEnd();

    options_open = false;

    // The pad's button edges are static across this whole nested loop, so the B that closed this
    // screen is still held and would rose/fall-edge into gameplay. Same counter the load/save
    // screen and the confirmation boxes use; gpui_button_shortcuts and gpui_action_buttons both
    // drop their edges on the change. See gamepad_ui_bump_input_epoch.
    gamepad_ui_bump_input_epoch();

    return rc;
}

// 0x4814D8
static int OptnStart()
{
    fontsave = text_curr();

    if (!message_init(&optn_msgfl)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", msg_path, "options.msg");
    if (!message_load(&optn_msgfl, path)) {
        return -1;
    }

    for (int index = 0; index < OPTIONS_WINDOW_FRM_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, opgrphs[index], 0, 0, 0);
        opbmp[index] = art_lock(fid, &(grphkey[index]), &(ginfo[index].width), &(ginfo[index].height));

        if (opbmp[index] == NULL) {
            while (--index >= 0) {
                art_ptr_unlock(grphkey[index]);
            }

            message_exit(&optn_msgfl);

            return -1;
        }
    }

    int cycle = 0;
    for (int index = 0; index < OPTIONS_WINDOW_BUTTONS_COUNT; index++) {
        opbtns[index] = (unsigned char*)mem_malloc(ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width * ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].height + 1024);
        if (opbtns[index] == NULL) {
            while (--index >= 0) {
                mem_free(opbtns[index]);
            }

            for (int index = 0; index < OPTIONS_WINDOW_FRM_COUNT; index++) {
                art_ptr_unlock(grphkey[index]);
            }

            message_exit(&optn_msgfl);

            return -1;
        }

        cycle = cycle ^ 1;

        memcpy(opbtns[index], opbmp[cycle + 1], ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width * ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].height);
    }

    int optionsWindowX = (screenGetWidth() - ginfo[OPTIONS_WINDOW_FRM_BACKGROUND].width) / 2;
    int optionsWindowY = (screenGetHeight() - ginfo[OPTIONS_WINDOW_FRM_BACKGROUND].height) / 2 - 60;
    optnwin = win_add(optionsWindowX,
        optionsWindowY,
        ginfo[0].width,
        ginfo[0].height,
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);

    if (optnwin == -1) {
        for (int index = 0; index < OPTIONS_WINDOW_BUTTONS_COUNT; index++) {
            mem_free(opbtns[index]);
        }

        for (int index = 0; index < OPTIONS_WINDOW_FRM_COUNT; index++) {
            art_ptr_unlock(grphkey[index]);
        }

        message_exit(&optn_msgfl);

        return -1;
    }

    bk_enable = map_disable_bk_processes();

    mouse_3d_was_on = gmouse_3d_is_on();
    if (mouse_3d_was_on) {
        gmouse_3d_off();
    }

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    winbuf = win_get_buf(optnwin);
    memcpy(winbuf, opbmp[OPTIONS_WINDOW_FRM_BACKGROUND], ginfo[OPTIONS_WINDOW_FRM_BACKGROUND].width * ginfo[OPTIONS_WINDOW_FRM_BACKGROUND].height);

    text_font(103);

    int textY = (ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].height - text_height()) / 2 + 1;
    int buttonY = 17;

    for (int index = 0; index < OPTIONS_WINDOW_BUTTONS_COUNT; index += 2) {
        char text[128];

        const char* msg = getmsg(&optn_msgfl, &optnmesg, index / 2);
        strcpy(text, msg);

        int textX = (ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width - text_width(text)) / 2;
        if (textX < 0) {
            textX = 0;
        }

        text_to_buf(opbtns[index] + ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width * textY + textX, text, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width, colorTable[18979]);
        text_to_buf(opbtns[index + 1] + ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width * textY + textX, text, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width, colorTable[14723]);

        int btn = win_register_button(optnwin, 13, buttonY, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].width, ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].height, -1, -1, -1, index / 2 + 500, opbtns[index], opbtns[index + 1], NULL, 32);
        optnbtn[index / 2] = btn;
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_lrg_butt_press, gsound_lrg_butt_release);
        }

        buttonY += ginfo[OPTIONS_WINDOW_FRM_BUTTON_ON].height + 3;
    }

    text_font(101);

    win_draw(optnwin);

    return 0;
}

// 0x481908
static int OptnEnd()
{
    win_delete(optnwin);

    // ⚠️ DRAIN. This menu's pad handler injects 500 + row (500 SAVE, 501 LOAD, 502 PREFERENCES,
    // ...) and those exact numbers mean DONE / PRINT / CANCEL in the character editor, which can
    // open on the very next frame. Codes are unique per WINDOW; the queue is GLOBAL.
    flush_input_buffer();
    text_font(fontsave);
    message_exit(&optn_msgfl);

    for (int index = 0; index < OPTIONS_WINDOW_BUTTONS_COUNT; index++) {
        mem_free(opbtns[index]);
    }

    for (int index = 0; index < OPTIONS_WINDOW_FRM_COUNT; index++) {
        art_ptr_unlock(grphkey[index]);
    }

    if (mouse_3d_was_on) {
        gmouse_3d_on();
    }

    if (bk_enable) {
        map_enable_bk_processes();
    }

    return 0;
}

// 0x481974
int PauseWindow(bool is_world_map)
{
    // 0x4812EC
    static const int graphicIds[PAUSE_WINDOW_FRM_COUNT] = {
        208, // charwin.frm - character editor
        209, // donebox.frm - character editor
        8, // lilredup.frm - little red button up
        9, // lilreddn.frm - little red button down
    };

    unsigned char* frmData[PAUSE_WINDOW_FRM_COUNT];
    CacheEntry* frmHandles[PAUSE_WINDOW_FRM_COUNT];
    Size frmSizes[PAUSE_WINDOW_FRM_COUNT];

    bool gameMouseWasVisible;
    if (!is_world_map) {
        bk_enable = map_disable_bk_processes();
        cycle_disable();

        gameMouseWasVisible = gmouse_3d_is_on();
        if (gameMouseWasVisible) {
            gmouse_3d_off();
        }
    }

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    ShadeScreen(is_world_map);

    for (int index = 0; index < PAUSE_WINDOW_FRM_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, graphicIds[index], 0, 0, 0);
        frmData[index] = art_lock(fid, &(frmHandles[index]), &(frmSizes[index].width), &(frmSizes[index].height));
        if (frmData[index] == NULL) {
            while (--index >= 0) {
                art_ptr_unlock(frmHandles[index]);
            }

            debug_printf("\n** Error loading pause window graphics! **\n");
            return -1;
        }
    }

    if (!message_init(&optn_msgfl)) {
        // FIXME: Leaking graphics.
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", msg_path, "options.msg");
    if (!message_load(&optn_msgfl, path)) {
        // FIXME: Leaking graphics.
        return -1;
    }

    int pauseWindowX = (screenGetWidth() - frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width) / 2;
    int pauseWindowY = (screenGetHeight() - frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].height) / 2;

    if (is_world_map) {
        pauseWindowX -= 65;
        pauseWindowY -= 24;
    } else {
        pauseWindowY -= 54;
    }

    int window = win_add(pauseWindowX,
        pauseWindowY,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].height,
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (window == -1) {
        for (int index = 0; index < PAUSE_WINDOW_FRM_COUNT; index++) {
            art_ptr_unlock(frmHandles[index]);
        }

        message_exit(&optn_msgfl);

        debug_printf("\n** Error opening pause window! **\n");
        return -1;
    }

    unsigned char* windowBuffer = win_get_buf(window);
    memcpy(windowBuffer,
        frmData[PAUSE_WINDOW_FRM_BACKGROUND],
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width * frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].height);

    trans_buf_to_buf(frmData[PAUSE_WINDOW_FRM_DONE_BOX],
        frmSizes[PAUSE_WINDOW_FRM_DONE_BOX].width,
        frmSizes[PAUSE_WINDOW_FRM_DONE_BOX].height,
        frmSizes[PAUSE_WINDOW_FRM_DONE_BOX].width,
        windowBuffer + frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width * 42 + 13,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width);

    fontsave = text_curr();
    text_font(103);

    char* messageItemText;

    messageItemText = getmsg(&optn_msgfl, &optnmesg, 300);
    text_to_buf(windowBuffer + frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width * 45 + 52,
        messageItemText,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width,
        colorTable[18979]);

    text_font(104);

    messageItemText = getmsg(&optn_msgfl, &optnmesg, 301);
    strcpy(path, messageItemText);

    int length = text_width(path);
    text_to_buf(windowBuffer + frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width * 10 + 2 + (frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width - length) / 2,
        path,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width,
        frmSizes[PAUSE_WINDOW_FRM_BACKGROUND].width,
        colorTable[18979]);

    int doneBtn = win_register_button(window,
        26,
        46,
        frmSizes[PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_UP].width,
        frmSizes[PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_UP].height,
        -1,
        -1,
        -1,
        504,
        frmData[PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_UP],
        frmData[PAUSE_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (doneBtn != -1) {
        win_register_button_sound_func(doneBtn, gsound_red_butt_press, gsound_red_butt_release);
    }

    win_draw(window);

    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();
        switch (keyCode) {
        case KEY_PLUS:
        case KEY_EQUAL:
            IncGamma();
            break;
        case KEY_MINUS:
        case KEY_UNDERSCORE:
            DecGamma();
            break;
        default:
            if (keyCode != -1 && keyCode != -2) {
                done = true;
            }

            if (game_user_wants_to_quit != 0) {
                done = true;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (!is_world_map) {
        tile_refresh_display();
    }

    win_delete(window);

    for (int index = 0; index < PAUSE_WINDOW_FRM_COUNT; index++) {
        art_ptr_unlock(frmHandles[index]);
    }

    message_exit(&optn_msgfl);

    if (!is_world_map) {
        if (gameMouseWasVisible) {
            gmouse_3d_on();
        }

        if (bk_enable) {
            map_enable_bk_processes();
        }

        cycle_enable();

        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    }

    text_font(fontsave);

    return 0;
}

// 0x481E0C
static void ShadeScreen(bool is_world_map)
{
    if (is_world_map) {
        mouse_hide();
        grey_buf(win_get_buf(world_win) + 640 * 21 + 22, 450, 442, 640);
        win_draw(world_win);
    } else {
        mouse_hide();
        tile_refresh_display();

        int windowWidth = 640;
        int windowHeight = win_height(display_win);
        unsigned char* windowBuffer = win_get_buf(display_win);
        grey_buf(windowBuffer, windowWidth, windowHeight, windowWidth);

        win_draw(display_win);
    }

    mouse_show();
}

// 0x481E84
// ---------------------------------------------------------------------------
// Controller navigation for the Preferences screen.
//
// The screen has no "buttons" in the sense the options menu does -- every control is
// painted straight into the window buffer by UpdateThing() and hit-tested from the raw
// mouse position inside DoThing(). So there is no image to swap for a highlight, and
// restoring a highlight rect from the clean background would wipe the labels, which are
// drawn ONCE in PrefStart and never repainted.
//
// So the indicator is the CURSOR: selecting a control parks the mouse on it. Same choice
// the world-map list nav made, for the same reason.
//
// The values are changed by our own stepper rather than by synthesising clicks. DoThing's
// range-slider branch is a blocking DRAG loop that only exits on a mouse-button-up event;
// calling it with no button held would hang the game.
//
//   D-pad / left stick  move the selection in ALL FOUR directions, geometrically -- left
//                      and right cross between the three columns and along the button
//                      row, exactly where the eye expects to go
//   LB / RB            change the value, down / up (auto-repeating)
//   A                  cycle the value / toggle the checkbox / press the button
//   B                  CANCEL,  Y  DONE
//   right stick        free cursor, as everywhere else
//
// Left/right steer rather than adjust because this screen is THREE COLUMNS, not a list:
// binding them to the value left the other two columns reachable only by paging through
// two dozen controls with up/down. The value moved to the shoulders, which is where a
// settings screen on a pad usually puts it anyway.
typedef enum PrefNavKind {
    PREF_NAV_PRIMARY,
    PREF_NAV_SECONDARY,
    PREF_NAV_RANGE,
    PREF_NAV_SPEEDUP,
    PREF_NAV_CRT,
    PREF_NAV_DEFAULT,
    PREF_NAV_DONE,
    PREF_NAV_CANCEL,
} PrefNavKind;

typedef struct PrefNavItem {
    short kind;
    // Index into btndat for the three preference kinds, -1 otherwise.
    short pref;
    // Which visual column, so LB/RB can skip to the next one.
    short column;
} PrefNavItem;

// Visual order, not enum order: the two checkboxes live in the right-hand column between
// the first and second slider, which is where they are drawn.
static const PrefNavItem prefNav[] = {
    { PREF_NAV_PRIMARY, PREF_GAME_DIFFICULTY, 0 },
    { PREF_NAV_PRIMARY, PREF_COMBAT_DIFFICULTY, 0 },
    { PREF_NAV_PRIMARY, PREF_VIOLENCE_LEVEL, 0 },
    { PREF_NAV_PRIMARY, PREF_TARGET_HIGHLIGHT, 0 },
    { PREF_NAV_PRIMARY, PREF_RUNNING_BURNING_GUY, 0 },
    { PREF_NAV_SECONDARY, PREF_COMBAT_MESSAGES, 1 },
    { PREF_NAV_SECONDARY, PREF_COMBAT_TAUNTS, 1 },
    { PREF_NAV_SECONDARY, PREF_LANGUAGE_FILTER, 1 },
    { PREF_NAV_SECONDARY, PREF_RUNNING, 1 },
    { PREF_NAV_SECONDARY, PREF_SUBTITLES, 1 },
    { PREF_NAV_SECONDARY, PREF_ITEM_HIGHLIGHT, 1 },
    { PREF_NAV_RANGE, PREF_COMBAT_SPEED, 2 },
    { PREF_NAV_SPEEDUP, -1, 2 },
    { PREF_NAV_CRT, -1, 2 },
    { PREF_NAV_RANGE, PREF_TEXT_BASE_DELAY, 2 },
    { PREF_NAV_RANGE, PREF_MASTER_VOLUME, 2 },
    { PREF_NAV_RANGE, PREF_MUSIC_VOLUME, 2 },
    { PREF_NAV_RANGE, PREF_SFX_VOLUME, 2 },
    { PREF_NAV_RANGE, PREF_SPEECH_VOLUME, 2 },
    { PREF_NAV_RANGE, PREF_BRIGHTNESS, 2 },
    { PREF_NAV_RANGE, PREF_MOUSE_SENSITIVIY, 2 },
    { PREF_NAV_DEFAULT, -1, 3 },
    { PREF_NAV_DONE, -1, 3 },
    { PREF_NAV_CANCEL, -1, 3 },
};

#define PREF_NAV_COUNT ((int)(sizeof(prefNav) / sizeof(prefNav[0])))

// The point the cursor is parked on -- for the rotary controls that is the centre DoThing
// treats as "cycle", so a real click there does the same thing A does.
static void prefs_nav_hotspot(int index, int* x, int* y)
{
    const PrefNavItem* item = &(prefNav[index]);
    const PreferenceDescription* meta = item->pref >= 0 ? &(btndat[item->pref]) : NULL;

    switch (item->kind) {
    case PREF_NAV_PRIMARY:
        *x = meta->knobX + 23;
        *y = meta->knobY + 21;
        break;
    case PREF_NAV_SECONDARY:
        *x = meta->knobX + 11;
        *y = meta->knobY + 12;
        break;
    case PREF_NAV_RANGE:
        // Mid-track: the knob itself slides, the track does not.
        *x = 494;
        *y = meta->knobY + 6;
        break;
    case PREF_NAV_SPEEDUP:
        *x = 383 + 9;
        *y = 68 + 9;
        break;
    case PREF_NAV_CRT:
        *x = crt_checkbox_x + 9;
        *y = 68 + 9;
        break;
    case PREF_NAV_DEFAULT:
        *x = 34;
        *y = 461;
        break;
    case PREF_NAV_DONE:
        *x = 159;
        *y = 461;
        break;
    default:
        *x = 274;
        *y = 461;
        break;
    }
}

// Nearest control in a direction, by geometry rather than by list order. Cost is the
// distance ALONG the direction plus three times the sideways drift, so a step left from a
// slider lands on the switch beside it rather than on whatever happens to be adjacent in
// the array. Geometry also means the bottom button row needs no special case: from the
// last control in a column, down simply finds DONE.
static int prefs_nav_neighbour(int index, int dx, int dy)
{
    int cx;
    int cy;
    prefs_nav_hotspot(index, &cx, &cy);

    int best = -1;
    long bestCost = 0;

    for (int i = 0; i < PREF_NAV_COUNT; i++) {
        if (i == index) {
            continue;
        }

        int x;
        int y;
        prefs_nav_hotspot(i, &x, &y);

        int along = (x - cx) * dx + (y - cy) * dy;
        if (along <= 0) {
            continue;
        }

        int across = abs((x - cx) * dy - (y - cy) * dx);
        if (across > 220) {
            continue;
        }

        long cost = (long)along + (long)across * 3;
        if (best < 0 || cost < bestCost) {
            best = i;
            bestCost = cost;
        }
    }

    return best < 0 ? index : best;
}

static void prefs_nav_move(int index)
{
    int x;
    int y;
    prefs_nav_hotspot(index, &x, &y);
    mouse_set_position(prfwin_x + x, prfwin_y + y);
}

// THE SELECTION INDICATOR IS A DRAWN FRAME, not the mouse cursor.
//
// The cursor was the first attempt, following the world-map list nav. It failed for a
// reason that screen does not have: the options menu HIDES the cursor while a pad is
// connected, and even shown, a small arrow on a busy 640x480 panel does not read as "this
// is the control you are editing". The user's report was exactly that -- "both sticks just
// change the difficulty, I can't get to another setting": the selection WAS moving, there
// was simply nothing on screen saying so.
//
// Drawing it is awkward because the labels beside every control are painted ONCE in
// PrefStart and never repainted, so a frame erased against the clean background bitmap
// would take them with it. The fix is a save-under: keep the four 1px strips the frame
// covers and put them back before anything else touches the window.
//
// That only stays consistent if nothing ever draws while the frame is up, so the loop is
// strictly: erase -> handle input (steppers, DoThing, SetDefaults, whatever) -> draw.
// The frame exists only between the end of one frame and the start of the next.
#define PREF_HL_MAX_W 300
#define PREF_HL_MAX_H 80

static unsigned char pref_hl_top[PREF_HL_MAX_W];
static unsigned char pref_hl_bottom[PREF_HL_MAX_W];
static unsigned char pref_hl_left[PREF_HL_MAX_H];
static unsigned char pref_hl_right[PREF_HL_MAX_H];
static int pref_hl_x = -1;
static int pref_hl_y;
static int pref_hl_w;
static int pref_hl_h;

// The widget's own rectangle, three pixels of air around it. Sizes are the ones
// UpdateThing actually blits: 46x47 rotary switch, 22x25 two-way switch, 21x12 slider knob
// on a 384..624 track, 18x18 checkbox.
static void prefs_nav_bounds(int index, int* x, int* y, int* w, int* h)
{
    const PrefNavItem* item = &(prefNav[index]);
    const PreferenceDescription* meta = item->pref >= 0 ? &(btndat[item->pref]) : NULL;

    switch (item->kind) {
    case PREF_NAV_PRIMARY:
        *x = meta->knobX - 3;
        *y = meta->knobY - 3;
        *w = 52;
        *h = 53;
        break;
    case PREF_NAV_SECONDARY:
        *x = meta->knobX - 3;
        *y = meta->knobY - 3;
        *w = 28;
        *h = 31;
        break;
    case PREF_NAV_RANGE:
        // The whole track, not the knob -- the knob moves and a frame that chased it
        // would read as part of the value rather than as the selection.
        *x = 381;
        *y = meta->knobY - 3;
        *w = 246;
        *h = 18;
        break;
    case PREF_NAV_SPEEDUP:
        *x = 380;
        *y = 65;
        *w = 24;
        *h = 24;
        break;
    case PREF_NAV_CRT:
        *x = crt_checkbox_x - 3;
        *y = 65;
        *w = 24;
        *h = 24;
        break;
    default:
        // DEFAULT / DONE / CANCEL, the little red buttons at y 450.
        *x = (prefNav[index].kind == PREF_NAV_DEFAULT ? 23 : (prefNav[index].kind == PREF_NAV_DONE ? 148 : 263)) - 3;
        *y = 447;
        *w = ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP].width + 6;
        *h = ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP].height + 6;
        break;
    }
}

static void prefs_hl_erase()
{
    if (pref_hl_x < 0) {
        return;
    }

    memcpy(prefbuf + PREFERENCES_WINDOW_WIDTH * pref_hl_y + pref_hl_x, pref_hl_top, pref_hl_w);
    memcpy(prefbuf + PREFERENCES_WINDOW_WIDTH * (pref_hl_y + pref_hl_h - 1) + pref_hl_x, pref_hl_bottom, pref_hl_w);
    for (int i = 0; i < pref_hl_h; i++) {
        unsigned char* row = prefbuf + PREFERENCES_WINDOW_WIDTH * (pref_hl_y + i);
        row[pref_hl_x] = pref_hl_left[i];
        row[pref_hl_x + pref_hl_w - 1] = pref_hl_right[i];
    }

    pref_hl_x = -1;
}

static void prefs_hl_draw(int index)
{
    int x;
    int y;
    int w;
    int h;
    prefs_nav_bounds(index, &x, &y, &w, &h);

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > PREFERENCES_WINDOW_WIDTH) {
        w = PREFERENCES_WINDOW_WIDTH - x;
    }
    if (y + h > PREFERENCES_WINDOW_HEIGHT) {
        h = PREFERENCES_WINDOW_HEIGHT - y;
    }
    if (w < 3 || h < 3 || w > PREF_HL_MAX_W || h > PREF_HL_MAX_H) {
        return;
    }

    // Save the whole frame footprint BEFORE drawing any of it, or the corners come back
    // white: the vertical strips would otherwise record pixels the horizontal ones just
    // overwrote.
    memcpy(pref_hl_top, prefbuf + PREFERENCES_WINDOW_WIDTH * y + x, w);
    memcpy(pref_hl_bottom, prefbuf + PREFERENCES_WINDOW_WIDTH * (y + h - 1) + x, w);
    for (int i = 0; i < h; i++) {
        const unsigned char* row = prefbuf + PREFERENCES_WINDOW_WIDTH * (y + i);
        pref_hl_left[i] = row[x];
        pref_hl_right[i] = row[x + w - 1];
    }

    // Fallout's own selection colour -- the pale yellow the character screen and the
    // save/load list use for the highlighted entry (colorTable[992] being the unselected
    // green). Plain white belonged to no palette on this screen.
    unsigned char color = colorTable[32747];
    memset(prefbuf + PREFERENCES_WINDOW_WIDTH * y + x, color, w);
    memset(prefbuf + PREFERENCES_WINDOW_WIDTH * (y + h - 1) + x, color, w);
    for (int i = 0; i < h; i++) {
        unsigned char* row = prefbuf + PREFERENCES_WINDOW_WIDTH * (y + i);
        row[x] = color;
        row[x + w - 1] = color;
    }

    pref_hl_x = x;
    pref_hl_y = y;
    pref_hl_w = w;
    pref_hl_h = h;
}

// Nudge one control by one step. `wrap` is what A does on the rotary controls (cycle
// round, like clicking their centre); the D-pad clamps instead, so holding a direction
// settles at an end rather than flipping back to the other one.
static void prefs_nav_step(int index, int dir, bool wrap)
{
    const PrefNavItem* item = &(prefNav[index]);
    PreferenceDescription* meta = item->pref >= 0 ? &(btndat[item->pref]) : NULL;

    switch (item->kind) {
    case PREF_NAV_PRIMARY: {
        int value = *meta->valuePtr + dir;
        if (wrap) {
            value = (value + meta->valuesCount) % meta->valuesCount;
        } else {
            value = std::clamp(value, 0, (int)meta->valuesCount - 1);
        }
        if (value == *meta->valuePtr) {
            return;
        }
        *meta->valuePtr = value;
        meta->direction = dir < 0 ? 1 : 0;
        gsound_play_sfx_file("ib3lu1x1");
        break;
    }
    case PREF_NAV_SECONDARY: {
        // Left/right pick a side of the switch rather than toggling, so the direction
        // always means the same thing. COMBAT MESSAGES has its two labels the other way
        // round on screen -- mirror what DoThing does for a click on the same side.
        int value;
        if (wrap) {
            value = *meta->valuePtr ^ 1;
        } else if (dir < 0) {
            value = item->pref == PREF_COMBAT_MESSAGES ? 1 : 0;
        } else {
            value = item->pref == PREF_COMBAT_MESSAGES ? 0 : 1;
        }
        if (value == *meta->valuePtr) {
            return;
        }
        *meta->valuePtr = value;
        gsound_play_sfx_file("ib2lu1x1");
        break;
    }
    case PREF_NAV_RANGE: {
        double value;
        switch (item->pref) {
        case PREF_TEXT_BASE_DELAY:
            value = 6.0 - text_delay + 1.0;
            break;
        case PREF_BRIGHTNESS:
            value = gamma_value;
            break;
        case PREF_MOUSE_SENSITIVIY:
            value = mouse_sens;
            break;
        default:
            value = *meta->valuePtr;
            break;
        }

        // Twenty steps end to end -- the same resolution a slow drag gives.
        double stepped = value + dir * (meta->maxValue - meta->minValue) / 20.0;
        stepped = std::clamp(stepped, meta->minValue, meta->maxValue);
        if (stepped == value) {
            return;
        }

        switch (item->pref) {
        case PREF_TEXT_BASE_DELAY:
            text_delay = 6.0 - stepped + 1.0;
            text_object_set_base_delay(text_delay);
            break;
        case PREF_BRIGHTNESS:
            gamma_value = stepped;
            colorGamma(gamma_value);
            break;
        case PREF_MOUSE_SENSITIVIY:
            mouse_sens = stepped;
            mouse_set_sensitivity(mouse_sens);
            break;
        case PREF_MASTER_VOLUME:
            *meta->valuePtr = (int)stepped;
            gsound_set_master_volume(master_volume);
            break;
        case PREF_MUSIC_VOLUME:
            *meta->valuePtr = (int)stepped;
            gsound_background_volume_set(music_volume);
            break;
        case PREF_SFX_VOLUME:
            *meta->valuePtr = (int)stepped;
            gsound_set_sfx_volume(sndfx_volume);
            gsound_play_sfx_file("butin1");
            break;
        case PREF_SPEECH_VOLUME:
            *meta->valuePtr = (int)stepped;
            gsound_speech_volume_set(speech_volume);
            break;
        default:
            *meta->valuePtr = (int)stepped;
            break;
        }
        break;
    }
    case PREF_NAV_SPEEDUP:
        player_speedup ^= 1;
        win_set_button_rest_state(plyrspdbid, player_speedup, 0);
        gsound_play_sfx_file("ib2lu1x1");
        changed = true;
        return;
    case PREF_NAV_CRT:
        crt_filter ^= 1;
        crt_set_enabled(crt_filter != 0);
        crt_persist();
        win_set_button_rest_state(crtbid, crt_filter, 0);
        gsound_play_sfx_file("ib2lu1x1");
        changed = true;
        return;
    default:
        // The three red buttons have nothing to step.
        return;
    }

    UpdateThing(item->pref);
    changed = true;
}

// A on the current item. The rotary controls cycle; the red buttons go through the same
// event codes the mouse would post, so the loop below handles them in one place.
static void prefs_nav_activate(int index)
{
    switch (prefNav[index].kind) {
    case PREF_NAV_DEFAULT:
        gsound_play_sfx_file("ib1p1xx1");
        GNW_add_input_buffer(527);
        break;
    case PREF_NAV_DONE:
        gsound_play_sfx_file("ib1p1xx1");
        GNW_add_input_buffer(504);
        break;
    case PREF_NAV_CANCEL:
        gsound_play_sfx_file("ib1p1xx1");
        GNW_add_input_buffer(528);
        break;
    default:
        prefs_nav_step(index, 1, true);
        break;
    }
}

// Edge plus hold-to-repeat, so a held direction walks a slider at a steady rate instead
// of needing twenty presses. `state`: 0 released, 1 pressed and waiting out the initial
// delay, 2 repeating.
#define PREF_NAV_REPEAT_DELAY 320
#define PREF_NAV_REPEAT_RATE 90

static bool prefs_nav_edge(bool now, int* state, unsigned int* lastFire, bool repeat)
{
    if (!now) {
        *state = 0;
        return false;
    }

    if (*state == 0) {
        *state = 1;
        *lastFire = get_time();
        return true;
    }

    if (!repeat) {
        return false;
    }

    unsigned int wait = *state == 1 ? PREF_NAV_REPEAT_DELAY : PREF_NAV_REPEAT_RATE;
    if (elapsed_time(*lastFire) >= wait) {
        *state = 2;
        *lastFire = get_time();
        return true;
    }

    return false;
}

static int do_prefscreen()
{
    if (PrefStart() == -1) {
        debug_printf("\nPREFERENCE MENU: Error loading preference dialog data!\n");
        return -1;
    }

    // Latched only as a starting value: it is re-fetched every frame below.
    SDL_GameController* prefPad = gamepad_ui_get_controller();
    int prefSel = 0;
    int prefUpState = 0;
    int prefDownState = 0;
    int prefLeftState = 0;
    int prefRightState = 0;
    int prefLbState = 0;
    int prefRbState = 0;
    int prefAState = 0;
    int prefBState = 0;
    int prefYState = 0;
    unsigned int prefUpAt = 0;
    unsigned int prefDownAt = 0;
    unsigned int prefLeftAt = 0;
    unsigned int prefRightAt = 0;
    unsigned int prefLbAt = 0;
    unsigned int prefRbAt = 0;
    unsigned int prefAAt = 0;
    unsigned int prefBAt = 0;
    unsigned int prefYAt = 0;

    // do_options blanks the cursor whenever a pad is connected; this screen needs it back
    // whether or not the pad is up, because a mouse user arrives here the same way.
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    if (prefPad != NULL) {
        prefs_nav_move(prefSel);

        // Self-arming edges: we may have got here BY pressing A on the options menu, and
        // that press is still down. Seed every state from the pad as it is right now so a
        // carried-over button has to be released before it counts -- otherwise entering the
        // screen would instantly cycle GAME DIFFICULTY.
        SDL_GameControllerUpdate();
        unsigned int now = get_time();
        prefUpAt = prefDownAt = prefLeftAt = prefRightAt = now;
        prefLbAt = prefRbAt = prefAAt = prefBAt = prefYAt = now;
        prefUpState = (SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
            || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTY) < -16000) ? 1 : 0;
        prefDownState = (SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
            || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTY) > 16000) ? 1 : 0;
        prefLeftState = (SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0
            || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTX) < -16000) ? 1 : 0;
        prefRightState = (SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0
            || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTX) > 16000) ? 1 : 0;
        prefLbState = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0 ? 1 : 0;
        prefRbState = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0 ? 1 : 0;
        prefAState = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_A) != 0 ? 1 : 0;
        prefBState = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_B) != 0 ? 1 : 0;
        prefYState = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_Y) != 0 ? 1 : 0;
    }

    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        // Take the frame down before anything can draw: the save-under underneath it is
        // only valid while nothing else has touched those pixels.
        prefs_hl_erase();

        // Re-fetch every frame, never once on entry: gamepad_ui opens the controller LAZILY
        // and is hotplug-aware, and its tick does not run inside a modal loop like this one.
        // A handle latched at entry stays NULL for the whole screen if the pad happened not
        // to be open yet, and then nothing here ever answers.
        prefPad = gamepad_ui_get_controller();

        if (prefPad != NULL) {
            // Tell the shared layer this screen drives itself, so A is not ALSO turned into
            // a synthetic mouse click and the left stick does not snap the cursor around.
            gamepad_ui_set_self_nav(true);

            SDL_GameControllerUpdate();

            // self_nav also switches off the shared right-stick cursor (it exists to stop
            // the stick creeping over buttons on menus that draw their own highlight). This
            // screen wants both -- the D-pad hops between controls and the stick still
            // steers freely, because the cursor IS the indicator -- so drive it here rather
            // than lose it.
            float rx = SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
            float ry = SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
            float rmag = sqrtf(rx * rx + ry * ry);
            if (rmag > 0.25f) {
                float t = (rmag - 0.25f) / 0.75f;
                float speed = t * t * 14.0f;
                int cx;
                int cy;
                mouse_get_position(&cx, &cy);
                mouse_set_position(cx + (int)(rx / rmag * speed), cy + (int)(ry / rmag * speed));
            }

            bool upNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
                || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
            bool downNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
                || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
            bool leftNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0
                || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTX) < -16000;
            bool rightNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0
                || SDL_GameControllerGetAxis(prefPad, SDL_CONTROLLER_AXIS_LEFTX) > 16000;
            bool lbNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
            bool rbNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
            bool aNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_A) != 0;
            bool bNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_B) != 0;
            bool yNow = SDL_GameControllerGetButton(prefPad, SDL_CONTROLLER_BUTTON_Y) != 0;

            // Steering: all four directions, geometrically.
            int navX = 0;
            int navY = 0;
            if (prefs_nav_edge(upNow, &prefUpState, &prefUpAt, true)) {
                navY = -1;
            } else if (prefs_nav_edge(downNow, &prefDownState, &prefDownAt, true)) {
                navY = 1;
            } else if (prefs_nav_edge(leftNow, &prefLeftState, &prefLeftAt, true)) {
                navX = -1;
            } else if (prefs_nav_edge(rightNow, &prefRightState, &prefRightAt, true)) {
                navX = 1;
            }

            if (navX != 0 || navY != 0) {
                int next = prefs_nav_neighbour(prefSel, navX, navY);
                if (next != prefSel) {
                    prefSel = next;
                    prefs_nav_move(prefSel);
                    gsound_play_sfx_file("ib1p1xx1");
                }
            }

            // Value on the shoulders. BOTH edges must be evaluated -- `||` would
            // short-circuit past RB's state update whenever LB fired, and RB would latch.
            bool lbFired = prefs_nav_edge(lbNow, &prefLbState, &prefLbAt, true);
            bool rbFired = prefs_nav_edge(rbNow, &prefRbState, &prefRbAt, true);
            if (lbFired) {
                prefs_nav_step(prefSel, -1, false);
            } else if (rbFired) {
                prefs_nav_step(prefSel, 1, false);
            }

            if (prefs_nav_edge(aNow, &prefAState, &prefAAt, false)) {
                prefs_nav_activate(prefSel);
            }
            if (prefs_nav_edge(bNow, &prefBState, &prefBAt, false)) {
                GNW_add_input_buffer(KEY_ESCAPE);
            }
            // Y, not START: while the options menu is open the shared shortcut layer turns
            // START into ESCAPE, so binding DONE there would post an accept AND a cancel,
            // and the cancel would throw the changes away.
            if (prefs_nav_edge(yNow, &prefYState, &prefYAt, false)) {
                GNW_add_input_buffer(504);
            }
        }

        int eventCode = get_input();

        switch (eventCode) {
        case KEY_RETURN:
        case KEY_UPPERCASE_P:
        case KEY_LOWERCASE_P:
            gsound_play_sfx_file("ib1p1xx1");
            // FALLTHROUGH
        case 504:
            rc = 1;
            break;
        case KEY_CTRL_Q:
        case KEY_CTRL_X:
        case KEY_F10:
            game_quit_with_confirm();
            break;
        case KEY_EQUAL:
        case KEY_PLUS:
            IncGamma();
            break;
        case KEY_MINUS:
        case KEY_UNDERSCORE:
            DecGamma();
            break;
        case KEY_F12:
            dump_screen();
            break;
        case 527:
            SetDefaults(true);
            break;
        default:
            if (eventCode == KEY_ESCAPE || eventCode == 528 || game_user_wants_to_quit != 0) {
                RestoreSettings();
                rc = 0;
            } else if ((eventCode >= 505 && eventCode <= 524) || eventCode == 529) {
                // 529 is the CRT FILTER checkbox; 525..528 belong to the range-knob
                // hover, DEFAULT and CANCEL and are handled above.
                DoThing(eventCode);
            }
            break;
        }

        if (prefPad != NULL && rc == -1) {
            prefs_hl_draw(prefSel);
            win_draw(prfwin);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (prefPad != NULL) {
        // do_options re-asserts its own self_nav on the next frame; drop ours so a screen
        // without controller navigation gets the shared stick-and-click layer back.
        gamepad_ui_set_self_nav(false);
    }

    PrefEnd();

    return rc;
}

// 0x481F8C
static int PrefStart()
{
    int i;
    int fid;
    char* messageItemText;
    int x;
    int y;
    int width;
    int height;
    int messageItemId;
    int btn;
    int button_count;

    SaveSettings();

    for (i = 0; i < PREFERENCES_WINDOW_FRM_COUNT; i++) {
        fid = art_id(OBJ_TYPE_INTERFACE, prfgrphs[i], 0, 0, 0);
        prfbmp[i] = art_lock(fid, &(grphkey2[i]), &(ginfo2[i].width), &(ginfo2[i].height));
        if (prfbmp[i] == NULL) {
            while (--i >= 0) {
                art_ptr_unlock(grphkey2[i - 1]);
            }
            return -1;
        }
    }

    changed = false;

    int preferencesWindowX = (screenGetWidth() - PREFERENCES_WINDOW_WIDTH) / 2;
    int preferencesWindowY = (screenGetHeight() - PREFERENCES_WINDOW_HEIGHT) / 2;
    prfwin_x = preferencesWindowX;
    prfwin_y = preferencesWindowY;
    prfwin = win_add(preferencesWindowX,
        preferencesWindowY,
        PREFERENCES_WINDOW_WIDTH,
        PREFERENCES_WINDOW_HEIGHT,
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (prfwin == -1) {
        for (i = 0; i < PREFERENCES_WINDOW_FRM_COUNT; i++) {
            art_ptr_unlock(grphkey2[i]);
        }
        return -1;
    }

    prefbuf = win_get_buf(prfwin);
    memcpy(prefbuf,
        prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND],
        ginfo2[PREFERENCES_WINDOW_FRM_BACKGROUND].width * ginfo2[PREFERENCES_WINDOW_FRM_BACKGROUND].height);

    text_font(104);

    messageItemText = getmsg(&optn_msgfl, &optnmesg, 100);
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 10 + 74, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    text_font(103);

    if (game_global_vars[GVAR_RUNNING_BURNING_GUY]) {
        button_count = 5;
    } else {
        buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_6],
            ginfo2[PREFERENCES_WINDOW_FRM_6].width,
            ginfo2[PREFERENCES_WINDOW_FRM_6].height,
            ginfo2[PREFERENCES_WINDOW_FRM_6].width,
            prefbuf + PREFERENCES_WINDOW_WIDTH * 356 + 0,
            PREFERENCES_WINDOW_WIDTH);
        button_count = 4;
    }

    messageItemId = 101;
    for (i = 0; i < button_count; i++) {
        messageItemText = getmsg(&optn_msgfl, &optnmesg, messageItemId++);
        x = 99 - text_width(messageItemText) / 2;
        text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * row1Ytab[i] + x, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);
    }

    if (!game_global_vars[GVAR_RUNNING_BURNING_GUY]) {
        messageItemId++;
    }

    for (i = 0; i < SECONDARY_PREF_COUNT; i++) {
        messageItemText = getmsg(&optn_msgfl, &optnmesg, messageItemId++);
        text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * row2Ytab[i] + 206, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);
    }

    for (i = 0; i < RANGE_PREF_COUNT; i++) {
        messageItemText = getmsg(&optn_msgfl, &optnmesg, messageItemId++);
        text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * row3Ytab[i] + 384, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);
    }

    // DEFAULT
    messageItemText = getmsg(&optn_msgfl, &optnmesg, 120);
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 449 + 43, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    // DONE
    messageItemText = getmsg(&optn_msgfl, &optnmesg, 4);
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 449 + 169, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    // CANCEL
    messageItemText = getmsg(&optn_msgfl, &optnmesg, 121);
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 449 + 283, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    // Affect player speed
    text_font(101);
    messageItemText = getmsg(&optn_msgfl, &optnmesg, 122);
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 72 + 405, messageItemText, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    // CRT FILTER -- shares the clear band between the first two sliders (y 64..90) with
    // the checkbox above, starting where that label ends. Not in options.msg (it is ours,
    // not Interplay's), so the string is literal. If the translated player-speed label
    // runs long the pair is pushed back against the panel's right edge at x 624 rather
    // than off it.
    int crtLabelWidth = text_width(CRT_PREF_LABEL);
    crt_checkbox_x = 405 + text_width(messageItemText) + 14;
    if (crt_checkbox_x + 22 + crtLabelWidth > 620) {
        crt_checkbox_x = 620 - 22 - crtLabelWidth;
    }
    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * 72 + crt_checkbox_x + 22, CRT_PREF_LABEL, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);

    for (i = 0; i < PREF_COUNT; i++) {
        UpdateThing(i);
    }

    for (i = 0; i < PREF_COUNT; i++) {
        int mouseEnterEventCode;
        int mouseExitEventCode;
        int mouseDownEventCode;
        int mouseUpEventCode;

        if (i >= FIRST_RANGE_PREF) {
            x = 384;
            y = btndat[i].knobY - 12;
            width = 240;
            height = 23;
            mouseEnterEventCode = 526;
            mouseExitEventCode = 526;
            mouseDownEventCode = 505 + i;
            mouseUpEventCode = 526;

        } else if (i >= FIRST_SECONDARY_PREF) {
            x = btndat[i].minX;
            y = btndat[i].knobY - 5;
            width = btndat[i].maxX - x;
            height = 28;
            mouseEnterEventCode = -1;
            mouseExitEventCode = -1;
            mouseDownEventCode = -1;
            mouseUpEventCode = 505 + i;
        } else {
            x = btndat[i].minX;
            y = btndat[i].knobY - 4;
            width = btndat[i].maxX - x;
            height = 48;
            mouseEnterEventCode = -1;
            mouseExitEventCode = -1;
            mouseDownEventCode = -1;
            mouseUpEventCode = 505 + i;
        }

        btndat[i].btn = win_register_button(prfwin, x, y, width, height, mouseEnterEventCode, mouseExitEventCode, mouseDownEventCode, mouseUpEventCode, NULL, NULL, NULL, 32);
    }

    plyrspdbid = win_register_button(prfwin,
        383,
        68,
        ginfo2[PREFERENCES_WINDOW_FRM_CHECKBOX_OFF].width,
        ginfo2[PREFERENCES_WINDOW_FRM_CHECKBOX_ON].height,
        -1,
        -1,
        524,
        524,
        prfbmp[PREFERENCES_WINDOW_FRM_CHECKBOX_OFF],
        prfbmp[PREFERENCES_WINDOW_FRM_CHECKBOX_ON],
        NULL,
        BUTTON_FLAG_TRANSPARENT | BUTTON_FLAG_0x01 | BUTTON_FLAG_0x02);
    if (plyrspdbid != -1) {
        win_set_button_rest_state(plyrspdbid, player_speedup, 0);
    }

    win_register_button_sound_func(plyrspdbid, gsound_med_butt_press, gsound_med_butt_press);

    // CRT FILTER
    crtbid = win_register_button(prfwin,
        crt_checkbox_x,
        68,
        ginfo2[PREFERENCES_WINDOW_FRM_CHECKBOX_OFF].width,
        ginfo2[PREFERENCES_WINDOW_FRM_CHECKBOX_ON].height,
        -1,
        -1,
        529,
        529,
        prfbmp[PREFERENCES_WINDOW_FRM_CHECKBOX_OFF],
        prfbmp[PREFERENCES_WINDOW_FRM_CHECKBOX_ON],
        NULL,
        BUTTON_FLAG_TRANSPARENT | BUTTON_FLAG_0x01 | BUTTON_FLAG_0x02);
    if (crtbid != -1) {
        win_set_button_rest_state(crtbid, crt_filter, 0);
        win_register_button_sound_func(crtbid, gsound_med_butt_press, gsound_med_butt_press);
    }

    // DEFAULT
    btn = win_register_button(prfwin,
        23,
        450,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP].width,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN].height,
        -1,
        -1,
        -1,
        527,
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP],
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (btn != -1) {
        win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
    }

    // DONE
    btn = win_register_button(prfwin,
        148,
        450,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP].width,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN].height,
        -1,
        -1,
        -1,
        504,
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP],
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (btn != -1) {
        win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
    }

    // CANCEL
    btn = win_register_button(prfwin,
        263,
        450,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP].width,
        ginfo2[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN].height,
        -1,
        -1,
        -1,
        528,
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_UP],
        prfbmp[PREFERENCES_WINDOW_FRM_LITTLE_RED_BUTTON_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (btn != -1) {
        win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
    }

    text_font(101);

    win_draw(prfwin);

    return 0;
}

// 0x4825DC
static void DoThing(int eventCode)
{
    int x;
    int y;
    mouseGetPositionInWindow(prfwin, &x, &y);

    // This preference index also contains out-of-bounds value 19,
    // which is the only preference expressed as checkbox.
    int preferenceIndex = eventCode - 505;

    if (preferenceIndex >= FIRST_PRIMARY_PREF && preferenceIndex <= LAST_PRIMARY_PREF) {
        PreferenceDescription* meta = &(btndat[preferenceIndex]);
        int* valuePtr = meta->valuePtr;
        int value = *valuePtr;
        bool valueChanged = false;

        int v1 = meta->knobX + 23;
        int v2 = meta->knobY + 21;

        if (sqrt(pow((double)x - (double)v1, 2) + pow((double)y - (double)v2, 2)) > 16.0) {
            if (y > meta->knobY) {
                int v14 = meta->knobY + bglby[0];
                if (y >= v14 && y <= v14 + text_height()) {
                    if (x >= meta->minX && x <= meta->knobX) {
                        *valuePtr = 0;
                        meta->direction = 0;
                        valueChanged = true;
                    } else {
                        if (meta->valuesCount >= 3 && x >= meta->knobX + bglbx[2] && x <= meta->maxX) {
                            *valuePtr = 2;
                            meta->direction = 0;
                            valueChanged = true;
                        }
                    }
                }
            } else {
                if (x >= meta->knobX + 9 && x <= meta->knobX + 37) {
                    *valuePtr = 1;
                    if (value != 0) {
                        meta->direction = 1;
                    } else {
                        meta->direction = 0;
                    }
                    valueChanged = true;
                }
            }

            if (meta->valuesCount == 4) {
                int v19 = meta->knobY + bglby[3];
                if (y >= v19 && y <= v19 + 2 * text_height() && x >= meta->knobX + bglbx[3] && x <= meta->maxX) {
                    *valuePtr = 3;
                    meta->direction = 1;
                    valueChanged = true;
                }
            }
        } else {
            if (meta->direction != 0) {
                if (value == 0) {
                    meta->direction = 0;
                }
            } else {
                if (value == meta->valuesCount - 1) {
                    meta->direction = 1;
                }
            }

            if (meta->direction != 0) {
                *valuePtr = value - 1;
            } else {
                *valuePtr = value + 1;
            }

            valueChanged = true;
        }

        if (valueChanged) {
            gsound_play_sfx_file("ib3p1xx1");
            block_for_tocks(70);
            gsound_play_sfx_file("ib3lu1x1");
            UpdateThing(preferenceIndex);
            win_draw(prfwin);
            changed = true;
            return;
        }
    } else if (preferenceIndex >= FIRST_SECONDARY_PREF && preferenceIndex <= LAST_SECONDARY_PREF) {
        PreferenceDescription* meta = &(btndat[preferenceIndex]);
        int* valuePtr = meta->valuePtr;
        int value = *valuePtr;
        bool valueChanged = false;

        int v1 = meta->knobX + 11;
        int v2 = meta->knobY + 12;

        if (sqrt(pow((double)x - (double)v1, 2) + pow((double)y - (double)v2, 2)) > 10.0) {
            int v23 = meta->knobY - 5;
            if (y >= v23 && y <= v23 + text_height() + 2) {
                if (x >= meta->minX && x <= meta->knobX) {
                    *valuePtr = preferenceIndex == PREF_COMBAT_MESSAGES ? 1 : 0;
                    valueChanged = true;
                } else if (x >= meta->knobX + 22.0 && x <= meta->maxX) {
                    *valuePtr = preferenceIndex == PREF_COMBAT_MESSAGES ? 0 : 1;
                    valueChanged = true;
                }
            }
        } else {
            *valuePtr ^= 1;
            valueChanged = true;
        }

        if (valueChanged) {
            gsound_play_sfx_file("ib2p1xx1");
            block_for_tocks(70);
            gsound_play_sfx_file("ib2lu1x1");
            UpdateThing(preferenceIndex);
            win_draw(prfwin);
            changed = true;
            return;
        }
    } else if (preferenceIndex >= FIRST_RANGE_PREF && preferenceIndex <= LAST_RANGE_PREF) {
        PreferenceDescription* meta = &(btndat[preferenceIndex]);
        int* valuePtr = meta->valuePtr;

        gsound_play_sfx_file("ib1p1xx1");

        double value;
        switch (preferenceIndex) {
        case PREF_TEXT_BASE_DELAY:
            value = 6.0 - text_delay + 1.0;
            break;
        case PREF_BRIGHTNESS:
            value = gamma_value;
            break;
        case PREF_MOUSE_SENSITIVIY:
            value = mouse_sens;
            break;
        default:
            value = *valuePtr;
            break;
        }

        int knobX = (int)(219.0 / (meta->maxValue - meta->minValue));
        int v31 = (int)((value - meta->minValue) * (219.0 / (meta->maxValue - meta->minValue)) + 384.0);
        buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + PREFERENCES_WINDOW_WIDTH * meta->knobY + 384, 240, 12, PREFERENCES_WINDOW_WIDTH, prefbuf + PREFERENCES_WINDOW_WIDTH * meta->knobY + 384, PREFERENCES_WINDOW_WIDTH);
        trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_ON], 21, 12, 21, prefbuf + PREFERENCES_WINDOW_WIDTH * meta->knobY + v31, PREFERENCES_WINDOW_WIDTH);

        win_draw(prfwin);

        int sfxVolumeExample = 0;
        int speechVolumeExample = 0;
        while (true) {
            sharedFpsLimiter.mark();

            get_input();

            int tick = get_time();

            mouseGetPositionInWindow(prfwin, &x, &y);

            if (mouse_get_buttons() & 0x10) {
                gsound_play_sfx_file("ib1lu1x1");
                UpdateThing(preferenceIndex);
                win_draw(prfwin);
                renderPresent();
                changed = true;
                return;
            }

            if (v31 + 14 > x) {
                if (v31 + 6 > x) {
                    v31 = x - 6;
                    if (v31 < 384) {
                        v31 = 384;
                    }
                }
            } else {
                v31 = x - 6;
                if (v31 > 603) {
                    v31 = 603;
                }
            }

            double newValue = ((double)v31 - 384.0) / (219.0 / (meta->maxValue - meta->minValue)) + meta->minValue;

            int v52 = 0;

            switch (preferenceIndex) {
            case PREF_COMBAT_SPEED:
                *meta->valuePtr = (int)newValue;
                break;
            case PREF_TEXT_BASE_DELAY:
                text_delay = 6.0 - newValue + 1.0;
                break;
            case PREF_MASTER_VOLUME:
                *meta->valuePtr = (int)newValue;
                gsound_set_master_volume(master_volume);
                v52 = 1;
                break;
            case PREF_MUSIC_VOLUME:
                *meta->valuePtr = (int)newValue;
                gsound_background_volume_set(music_volume);
                v52 = 1;
                break;
            case PREF_SFX_VOLUME:
                *meta->valuePtr = (int)newValue;
                gsound_set_sfx_volume(sndfx_volume);
                v52 = 1;
                if (sfxVolumeExample == 0) {
                    gsound_play_sfx_file("butin1");
                    sfxVolumeExample = 7;
                } else {
                    sfxVolumeExample--;
                }
                break;
            case PREF_SPEECH_VOLUME:
                *meta->valuePtr = (int)newValue;
                gsound_speech_volume_set(speech_volume);
                v52 = 1;
                if (speechVolumeExample == 0) {
                    gsound_speech_play("narrator\\options", 12, 13, 15);
                    speechVolumeExample = 40;
                } else {
                    speechVolumeExample--;
                }
                break;
            case PREF_BRIGHTNESS:
                gamma_value = newValue;
                colorGamma(newValue);
                break;
            case PREF_MOUSE_SENSITIVIY:
                mouse_sens = newValue;
                break;
            }

            if (v52) {
                int off = PREFERENCES_WINDOW_WIDTH * (meta->knobY - 12) + 384;
                buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + off, 240, 24, PREFERENCES_WINDOW_WIDTH, prefbuf + off, PREFERENCES_WINDOW_WIDTH);

                for (int optionIndex = 0; optionIndex < meta->valuesCount; optionIndex++) {
                    const char* str = getmsg(&optn_msgfl, &optnmesg, meta->labelIds[optionIndex]);

                    int x;
                    switch (optionIndex) {
                    case 0:
                        // 0x4926AA
                        x = 384;
                        // TODO: Incomplete.
                        break;
                    case 1:
                        // 0x4926F3
                        switch (meta->valuesCount) {
                        case 2:
                            x = 624 - text_width(str);
                            break;
                        case 3:
                            // This code path does not use floating-point arithmetic
                            x = 504 - text_width(str) / 2 - 2;
                            break;
                        case 4:
                            // Uses floating-point arithmetic
                            x = 444 + text_width(str) / 2 - 8;
                            break;
                        }
                        break;
                    case 2:
                        // 0x492766
                        switch (meta->valuesCount) {
                        case 3:
                            x = 624 - text_width(str);
                            break;
                        case 4:
                            // Uses floating-point arithmetic
                            x = 564 - text_width(str) - 4;
                            break;
                        }
                        break;
                    case 3:
                        // 0x49279E
                        x = 624 - text_width(str);
                        break;
                    }
                    text_to_buf(prefbuf + PREFERENCES_WINDOW_WIDTH * (meta->knobY - 12) + x, str, PREFERENCES_WINDOW_WIDTH, PREFERENCES_WINDOW_WIDTH, colorTable[18979]);
                }
            } else {
                int off = PREFERENCES_WINDOW_WIDTH * meta->knobY + 384;
                buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + off, 240, 12, PREFERENCES_WINDOW_WIDTH, prefbuf + off, PREFERENCES_WINDOW_WIDTH);
            }

            trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_ON], 21, 12, 21, prefbuf + PREFERENCES_WINDOW_WIDTH * meta->knobY + v31, PREFERENCES_WINDOW_WIDTH);
            win_draw(prfwin);

            while (elapsed_time(tick) < 35) {
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    } else if (preferenceIndex == 19) {
        player_speedup ^= 1;
    } else if (preferenceIndex == 24) {
        // CRT FILTER. Applied immediately so the screen behind the dialog changes under
        // the click -- that IS the preview, there is nothing else to show.
        crt_filter ^= 1;
        crt_set_enabled(crt_filter != 0);
        crt_persist();
    }

    changed = true;
}

// 0x483170
static void UpdateThing(int index)
{
    text_font(101);

    PreferenceDescription* meta = &(btndat[index]);

    if (index >= FIRST_PRIMARY_PREF && index <= LAST_PRIMARY_PREF) {
        // 0x4812FC
        static const int offsets[PRIMARY_PREF_COUNT] = {
            66, // game difficulty
            143, // combat difficulty
            222, // violence level
            304, // target highlight
            382, // combat looks
        };

        int primaryOptionIndex = index - FIRST_PRIMARY_PREF;

        if (primaryOptionIndex == PREF_RUNNING_BURNING_GUY && !game_global_vars[GVAR_RUNNING_BURNING_GUY]) {
            return;
        }

        buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + 640 * offsets[primaryOptionIndex] + 23, 160, 54, 640, prefbuf + 640 * offsets[primaryOptionIndex] + 23, 640);

        for (int valueIndex = 0; valueIndex < meta->valuesCount; valueIndex++) {
            const char* text = getmsg(&optn_msgfl, &optnmesg, meta->labelIds[valueIndex]);

            char copy[100]; // TODO: Size is probably wrong.
            strcpy(copy, text);

            int x = meta->knobX + bglbx[valueIndex];
            int len = text_width(copy);
            switch (valueIndex) {
            case 0:
                x -= text_width(copy);
                meta->minX = x;
                break;
            case 1:
                x -= len / 2;
                meta->maxX = x + len;
                break;
            case 2:
            case 3:
                meta->maxX = x + len;
                break;
            }

            char* p = copy;
            while (*p != '\0' && *p != ' ') {
                p++;
            }

            int y = meta->knobY + bglby[valueIndex];
            const char* s;
            if (*p != '\0') {
                *p = '\0';
                text_to_buf(prefbuf + 640 * y + x, copy, 640, 640, colorTable[18979]);
                s = p + 1;
                y += text_height();
            } else {
                s = copy;
            }

            text_to_buf(prefbuf + 640 * y + x, s, 640, 640, colorTable[18979]);
        }

        int value = *(meta->valuePtr);
        trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_PRIMARY_SWITCH] + (46 * 47) * value, 46, 47, 46, prefbuf + 640 * meta->knobY + meta->knobX, 640);
    } else if (index >= FIRST_SECONDARY_PREF && index <= LAST_SECONDARY_PREF) {
        // 0x481310
        static const int offsets[SECONDARY_PREF_COUNT] = {
            66, // combat messages
            133, // combat taunts
            200, // language filter
            264, // running
            331, // subtitles
            397, // item highlight
        };

        int secondaryOptionIndex = index - FIRST_SECONDARY_PREF;

        buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + 640 * offsets[secondaryOptionIndex] + 251, 113, 34, 640, prefbuf + 640 * offsets[secondaryOptionIndex] + 251, 640);

        // Secondary options are booleans, so it's index is also it's value.
        for (int value = 0; value < 2; value++) {
            const char* text = getmsg(&optn_msgfl, &optnmesg, meta->labelIds[value]);

            int x;
            if (value) {
                x = meta->knobX + smlbx[value];
                meta->maxX = x + text_width(text);
            } else {
                x = meta->knobX + smlbx[value] - text_width(text);
                meta->minX = x;
            }
            text_to_buf(prefbuf + 640 * (meta->knobY - 5) + x, text, 640, 640, colorTable[18979]);
        }

        int value = *(meta->valuePtr);
        if (index == PREF_COMBAT_MESSAGES) {
            value ^= 1;
        }
        trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_SECONDARY_SWITCH] + (22 * 25) * value, 22, 25, 22, prefbuf + 640 * meta->knobY + meta->knobX, 640);
    } else if (index >= FIRST_RANGE_PREF && index <= LAST_RANGE_PREF) {
        buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_BACKGROUND] + 640 * (meta->knobY - 12) + 384, 240, 24, 640, prefbuf + 640 * (meta->knobY - 12) + 384, 640);
        switch (index) {
        case PREF_COMBAT_SPEED:
            if (1) {
                double value = *meta->valuePtr;
                value = std::clamp(value, 0.0, 50.0);

                int x = (int)((value - meta->minValue) * 219.0 / (meta->maxValue - meta->minValue) + 384.0);
                trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_OFF], 21, 12, 21, prefbuf + 640 * meta->knobY + x, 640);
            }
            break;
        case PREF_TEXT_BASE_DELAY:
            if (1) {
                text_delay = std::clamp(text_delay, 1.0, 6.0);

                int x = (int)((6.0 - text_delay) * 43.8 + 384.0);
                trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_OFF], 21, 12, 21, prefbuf + 640 * meta->knobY + x, 640);

                double value = (text_delay - 1.0) * 0.2 * 2.0;
                value = std::clamp(value, 0.0, 2.0);

                text_object_set_base_delay(text_delay);
                text_object_set_line_delay(value);
            }
            break;
        case PREF_MASTER_VOLUME:
        case PREF_MUSIC_VOLUME:
        case PREF_SFX_VOLUME:
        case PREF_SPEECH_VOLUME:
            if (1) {
                double value = *meta->valuePtr;
                value = std::clamp(value, meta->minValue, meta->maxValue);

                int x = (int)((value - meta->minValue) * 219.0 / (meta->maxValue - meta->minValue) + 384.0);
                trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_OFF], 21, 12, 21, prefbuf + 640 * meta->knobY + x, 640);

                switch (index) {
                case PREF_MASTER_VOLUME:
                    gsound_set_master_volume(master_volume);
                    break;
                case PREF_MUSIC_VOLUME:
                    gsound_background_volume_set(music_volume);
                    break;
                case PREF_SFX_VOLUME:
                    gsound_set_sfx_volume(sndfx_volume);
                    break;
                case PREF_SPEECH_VOLUME:
                    gsound_speech_volume_set(speech_volume);
                    break;
                }
            }
            break;
        case PREF_BRIGHTNESS:
            if (1) {
                gamma_value = std::clamp(gamma_value, 1.0, 1.17999267578125);

                int x = (int)((gamma_value - meta->minValue) * (219.0 / (meta->maxValue - meta->minValue)) + 384.0);
                trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_OFF], 21, 12, 21, prefbuf + 640 * meta->knobY + x, 640);

                colorGamma(gamma_value);
            }
            break;
        case PREF_MOUSE_SENSITIVIY:
            if (1) {
                mouse_sens = std::clamp(mouse_sens, 1.0, 2.5);

                int x = (int)((mouse_sens - meta->minValue) * (219.0 / (meta->maxValue - meta->minValue)) + 384.0);
                trans_buf_to_buf(prfbmp[PREFERENCES_WINDOW_FRM_KNOB_OFF], 21, 12, 21, prefbuf + 640 * meta->knobY + x, 640);

                mouse_set_sensitivity(mouse_sens);
            }
            break;
        }

        for (int optionIndex = 0; optionIndex < meta->valuesCount; optionIndex++) {
            const char* str = getmsg(&optn_msgfl, &optnmesg, meta->labelIds[optionIndex]);

            int x;
            switch (optionIndex) {
            case 0:
                // 0x4926AA
                x = 384;
                // TODO: Incomplete.
                break;
            case 1:
                // 0x4926F3
                switch (meta->valuesCount) {
                case 2:
                    x = 624 - text_width(str);
                    break;
                case 3:
                    // This code path does not use floating-point arithmetic
                    x = 504 - text_width(str) / 2 - 2;
                    break;
                case 4:
                    // Uses floating-point arithmetic
                    x = 444 + text_width(str) / 2 - 8;
                    break;
                }
                break;
            case 2:
                // 0x492766
                switch (meta->valuesCount) {
                case 3:
                    x = 624 - text_width(str);
                    break;
                case 4:
                    // Uses floating-point arithmetic
                    x = 564 - text_width(str) - 4;
                    break;
                }
                break;
            case 3:
                // 0x49279E
                x = 624 - text_width(str);
                break;
            }
            text_to_buf(prefbuf + 640 * (meta->knobY - 12) + x, str, 640, 640, colorTable[18979]);
        }
    } else {
        // return false;
    }

    // TODO: Incomplete.

    // return true;
}

// 0x483F28
static int PrefEnd()
{
    if (changed) {
        SavePrefs(1);
        JustUpdate();
        combat_highlight_change();
    }

    win_delete(prfwin);

    for (int index = 0; index < PREFERENCES_WINDOW_FRM_COUNT; index++) {
        art_ptr_unlock(grphkey2[index]);
    }

    return 0;
}

// 0x483F70
int init_options_menu()
{
    for (int index = 0; index < 11; index++) {
        btndat[index].direction = 0;
    }

    SetSystemPrefs();

    InitGreyTable(0, 255);

    return 0;
}

// 0x483F9C
void IncGamma()
{
    gamma_value = GAMMA_MIN;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, &gamma_value);

    if (gamma_value < GAMMA_MAX) {
        gamma_value += GAMMA_STEP;

        if (gamma_value >= GAMMA_MIN) {
            if (gamma_value > GAMMA_MAX) {
                gamma_value = GAMMA_MAX;
            }
        } else {
            gamma_value = GAMMA_MIN;
        }

        colorGamma(gamma_value);

        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, gamma_value);

        gconfig_save();
    }
}

// 0x48407C
void DecGamma()
{
    gamma_value = GAMMA_MIN;
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, &gamma_value);

    if (gamma_value > GAMMA_MIN) {
        gamma_value -= GAMMA_STEP;

        if (gamma_value >= GAMMA_MIN) {
            if (gamma_value > GAMMA_MAX) {
                gamma_value = GAMMA_MAX;
            }
        } else {
            gamma_value = GAMMA_MIN;
        }

        colorGamma(gamma_value);

        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, gamma_value);

        gconfig_save();
    }
}

// 0x484158
static void SetSystemPrefs()
{
    SetDefaults(false);

    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, &game_difficulty);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, &combat_difficulty);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, &violence_level);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TARGET_HIGHLIGHT_KEY, &target_highlight);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_MESSAGES_KEY, &combat_messages);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUNNING_BURNING_GUY_KEY, &running_burning_guy);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_TAUNTS_KEY, &combat_taunts);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_LANGUAGE_FILTER_KEY, &language_filter);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUNNING_KEY, &prf_running);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SUBTITLES_KEY, &subtitles);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ITEM_HIGHLIGHT_KEY, &item_highlight);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, &combat_speed);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TEXT_BASE_DELAY_KEY, &text_delay);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, &player_speedup);
    config_get_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MASTER_VOLUME_KEY, &master_volume);
    config_get_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MUSIC_VOLUME_KEY, &music_volume);
    config_get_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_SNDFX_VOLUME_KEY, &sndfx_volume);
    config_get_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_SPEECH_VOLUME_KEY, &speech_volume);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, &gamma_value);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MOUSE_SENSITIVITY_KEY, &mouse_sens);

    // The CRT filter's own cfg read happened back in crt_init(), during svga_init -- it
    // has to, the shader is live long before this screen exists. Take the flag from there
    // instead of parsing the key a second time.
    crt_filter = crt_is_enabled() ? 1 : 0;

    JustUpdate();
}

// 0x484360
static int SavePrefs(bool save)
{
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_GAME_DIFFICULTY_KEY, game_difficulty);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_DIFFICULTY_KEY, combat_difficulty);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VIOLENCE_LEVEL_KEY, violence_level);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TARGET_HIGHLIGHT_KEY, target_highlight);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_MESSAGES_KEY, combat_messages);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_LOOKS_KEY, running_burning_guy);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_TAUNTS_KEY, combat_taunts);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_LANGUAGE_FILTER_KEY, language_filter);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_RUNNING_KEY, prf_running);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SUBTITLES_KEY, subtitles);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_ITEM_HIGHLIGHT_KEY, item_highlight);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_SPEED_KEY, combat_speed);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TEXT_BASE_DELAY_KEY, text_delay);

    double textLineDelay = (text_delay - 1.0) / 5.0 * 2.0;
    if (textLineDelay >= 0.0) {
        if (textLineDelay > 2.0) {
            textLineDelay = 2.0;
        }

        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TEXT_LINE_DELAY_KEY, textLineDelay);
    } else {
        config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TEXT_LINE_DELAY_KEY, 0.0);
    }

    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_PLAYER_SPEEDUP_KEY, player_speedup);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_KEY, crt_filter);
    config_set_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MASTER_VOLUME_KEY, master_volume);
    config_set_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_MUSIC_VOLUME_KEY, music_volume);
    config_set_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_SNDFX_VOLUME_KEY, sndfx_volume);
    config_set_value(&game_config, GAME_CONFIG_SOUND_KEY, GAME_CONFIG_SPEECH_VOLUME_KEY, speech_volume);

    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_BRIGHTNESS_KEY, gamma_value);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_MOUSE_SENSITIVITY_KEY, mouse_sens);

    if (save) {
        gconfig_save();
    }

    return 0;
}

// 0x48460C
static void SetDefaults(bool a1)
{
    combat_difficulty = COMBAT_DIFFICULTY_NORMAL;
    violence_level = VIOLENCE_LEVEL_MAXIMUM_BLOOD;
    target_highlight = TARGET_HIGHLIGHT_TARGETING_ONLY;
    combat_messages = 1;
    running_burning_guy = 1;
    combat_taunts = 1;
    prf_running = 0;
    subtitles = 0;
    item_highlight = 1;
    combat_speed = 0;
    player_speedup = 0;
    crt_filter = 1;
    text_delay = 3.5;
    gamma_value = 1.0;
    mouse_sens = 1.0;
    game_difficulty = 1;
    language_filter = 0;
    master_volume = 22281;
    music_volume = 22281;
    sndfx_volume = 22281;
    speech_volume = 22281;

    if (a1) {
        for (int index = 0; index < PREF_COUNT; index++) {
            UpdateThing(index);
        }
        win_set_button_rest_state(plyrspdbid, player_speedup, 0);
        win_set_button_rest_state(crtbid, crt_filter, 0);
        crt_set_enabled(crt_filter != 0);
        win_draw(prfwin);
        changed = true;
    }
}

// 0x484704
static void SaveSettings()
{
    settings_backup[PREF_GAME_DIFFICULTY] = game_difficulty;
    settings_backup[PREF_COMBAT_DIFFICULTY] = combat_difficulty;
    settings_backup[PREF_VIOLENCE_LEVEL] = violence_level;
    settings_backup[PREF_TARGET_HIGHLIGHT] = target_highlight;
    settings_backup[PREF_RUNNING_BURNING_GUY] = running_burning_guy;
    settings_backup[PREF_COMBAT_MESSAGES] = combat_messages;
    settings_backup[PREF_COMBAT_TAUNTS] = combat_taunts;
    settings_backup[PREF_LANGUAGE_FILTER] = language_filter;
    settings_backup[PREF_RUNNING] = prf_running;
    settings_backup[PREF_SUBTITLES] = subtitles;
    settings_backup[PREF_ITEM_HIGHLIGHT] = item_highlight;
    settings_backup[PREF_COMBAT_SPEED] = combat_speed;
    settings_backup[PREF_TEXT_BASE_DELAY] = player_speedup;
    settings_backup[PREF_MASTER_VOLUME] = master_volume;
    text_delay_back = text_delay;
    settings_backup[PREF_MUSIC_VOLUME] = music_volume;
    gamma_value_back = gamma_value;
    settings_backup[PREF_SFX_VOLUME] = sndfx_volume;
    mouse_sens_back = mouse_sens;
    settings_backup[PREF_SPEECH_VOLUME] = speech_volume;
    crt_filter_back = crt_filter;
}

// 0x4847D4
static void RestoreSettings()
{
    game_difficulty = settings_backup[PREF_GAME_DIFFICULTY];
    combat_difficulty = settings_backup[PREF_COMBAT_DIFFICULTY];
    violence_level = settings_backup[PREF_VIOLENCE_LEVEL];
    target_highlight = settings_backup[PREF_TARGET_HIGHLIGHT];
    running_burning_guy = settings_backup[PREF_RUNNING_BURNING_GUY];
    combat_messages = settings_backup[PREF_COMBAT_MESSAGES];
    combat_taunts = settings_backup[PREF_COMBAT_TAUNTS];
    language_filter = settings_backup[PREF_LANGUAGE_FILTER];
    prf_running = settings_backup[PREF_RUNNING];
    subtitles = settings_backup[PREF_SUBTITLES];
    item_highlight = settings_backup[PREF_ITEM_HIGHLIGHT];
    combat_speed = settings_backup[PREF_COMBAT_SPEED];
    player_speedup = settings_backup[PREF_TEXT_BASE_DELAY];
    master_volume = settings_backup[PREF_MASTER_VOLUME];
    text_delay = text_delay_back;
    music_volume = settings_backup[PREF_MUSIC_VOLUME];
    gamma_value = gamma_value_back;
    sndfx_volume = settings_backup[PREF_SFX_VOLUME];
    mouse_sens = mouse_sens_back;
    speech_volume = settings_backup[PREF_SPEECH_VOLUME];
    crt_filter = crt_filter_back;

    JustUpdate();
}

// 0x4848A4
static void JustUpdate()
{
    game_difficulty = std::clamp(game_difficulty, 0, 2);
    combat_difficulty = std::clamp(combat_difficulty, 0, 2);
    violence_level = std::clamp(violence_level, 0, 3);
    target_highlight = std::clamp(target_highlight, 0, 2);
    combat_messages = std::clamp(combat_messages, 0, 1);
    running_burning_guy = std::clamp(running_burning_guy, 0, 1);
    combat_taunts = std::clamp(combat_taunts, 0, 1);
    language_filter = std::clamp(language_filter, 0, 1);
    prf_running = std::clamp(prf_running, 0, 1);
    subtitles = std::clamp(subtitles, 0, 1);
    item_highlight = std::clamp(item_highlight, 0, 1);
    combat_speed = std::clamp(combat_speed, 0, 50);
    player_speedup = std::clamp(player_speedup, 0, 1);
    crt_filter = std::clamp(crt_filter, 0, 1);
    text_delay = std::clamp(text_delay, 1.0, 6.0);
    master_volume = std::clamp(master_volume, 0, VOLUME_MAX);
    music_volume = std::clamp(music_volume, 0, VOLUME_MAX);
    sndfx_volume = std::clamp(sndfx_volume, 0, VOLUME_MAX);
    speech_volume = std::clamp(speech_volume, 0, VOLUME_MAX);
    gamma_value = std::clamp(gamma_value, 1.0, 1.17999267578125);
    mouse_sens = std::clamp(mouse_sens, 1.0, 2.5);

    text_object_set_base_delay(text_delay);
    gmouse_3d_synch_item_highlight();

    double textLineDelay = (text_delay + (-1.0)) * 0.2 * 2.0;
    textLineDelay = std::clamp(textLineDelay, 0.0, 2.0);

    text_object_set_line_delay(textLineDelay);
    combatai_refresh_messages();
    scr_message_free();
    gsound_set_master_volume(master_volume);
    gsound_background_volume_set(music_volume);
    gsound_set_sfx_volume(sndfx_volume);
    gsound_speech_volume_set(speech_volume);
    mouse_set_sensitivity(mouse_sens);
    colorGamma(gamma_value);
    crt_set_enabled(crt_filter != 0);
    crt_persist();
}

// 0x4848C8
int save_options(DB_FILE* stream)
{
    float textBaseDelay = (float)text_delay;
    float brightness = (float)gamma_value;
    float mouseSensitivity = (float)mouse_sens;

    if (db_fwriteInt(stream, game_difficulty) == -1) goto err;
    if (db_fwriteInt(stream, combat_difficulty) == -1) goto err;
    if (db_fwriteInt(stream, violence_level) == -1) goto err;
    if (db_fwriteInt(stream, target_highlight) == -1) goto err;
    if (db_fwriteInt(stream, running_burning_guy) == -1) goto err;
    if (db_fwriteInt(stream, combat_messages) == -1) goto err;
    if (db_fwriteInt(stream, combat_taunts) == -1) goto err;
    if (db_fwriteInt(stream, language_filter) == -1) goto err;
    if (db_fwriteInt(stream, prf_running) == -1) goto err;
    if (db_fwriteInt(stream, subtitles) == -1) goto err;
    if (db_fwriteInt(stream, item_highlight) == -1) goto err;
    if (db_fwriteInt(stream, combat_speed) == -1) goto err;
    if (db_fwriteInt(stream, player_speedup) == -1) goto err;
    if (db_fwriteFloat(stream, textBaseDelay) == -1) goto err;
    if (db_fwriteInt(stream, master_volume) == -1) goto err;
    if (db_fwriteInt(stream, music_volume) == -1) goto err;
    if (db_fwriteInt(stream, sndfx_volume) == -1) goto err;
    if (db_fwriteInt(stream, speech_volume) == -1) goto err;
    if (db_fwriteFloat(stream, brightness) == -1) goto err;
    if (db_fwriteFloat(stream, mouseSensitivity) == -1) goto err;

    return 0;

err:

    debug_printf("\nOPTION MENU: Error save option data!\n");

    return -1;
}

// 0x484AAC
int load_options(DB_FILE* stream)
{
    float textBaseDelay;
    float brightness;
    float mouseSensitivity;

    SetDefaults(false);

    if (db_freadInt(stream, &game_difficulty) == -1) goto err;
    if (db_freadInt(stream, &combat_difficulty) == -1) goto err;
    if (db_freadInt(stream, &violence_level) == -1) goto err;
    if (db_freadInt(stream, &target_highlight) == -1) goto err;
    if (db_freadInt(stream, &running_burning_guy) == -1) goto err;
    if (db_freadInt(stream, &combat_messages) == -1) goto err;
    if (db_freadInt(stream, &combat_taunts) == -1) goto err;
    if (db_freadInt(stream, &language_filter) == -1) goto err;
    if (db_freadInt(stream, &prf_running) == -1) goto err;
    if (db_freadInt(stream, &subtitles) == -1) goto err;
    if (db_freadInt(stream, &item_highlight) == -1) goto err;
    if (db_freadInt(stream, &combat_speed) == -1) goto err;
    if (db_freadInt(stream, &player_speedup) == -1) goto err;
    if (db_freadFloat(stream, &textBaseDelay) == -1) goto err;
    if (db_freadInt(stream, &master_volume) == -1) goto err;
    if (db_freadInt(stream, &music_volume) == -1) goto err;
    if (db_freadInt(stream, &sndfx_volume) == -1) goto err;
    if (db_freadInt(stream, &speech_volume) == -1) goto err;
    if (db_freadFloat(stream, &brightness) == -1) goto err;
    if (db_freadFloat(stream, &mouseSensitivity) == -1) goto err;

    gamma_value = brightness;
    mouse_sens = mouseSensitivity;
    text_delay = textBaseDelay;

    JustUpdate();
    SavePrefs(0);

    return 0;

err:

    debug_printf("\nOPTION MENU: Error loading option data!, using defaults.\n");

    SetDefaults(false);
    JustUpdate();
    SavePrefs(0);

    return -1;
}

} // namespace fallout
