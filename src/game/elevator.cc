#include "game/elevator.h"

#include <ctype.h>
#include <string.h>

#include "game/art.h"
#include "game/cycle.h"
#include "game/gmouse.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gsound.h"
#include "game/interact_prompt.h"
#include "game/intface.h"
#include "game/map.h"
#include "game/pipboy.h"
#include "game/scripts.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"

namespace fallout {

// The maximum number of elevator levels.
#define ELEVATOR_LEVEL_MAX 4

// NOTE: There are two variables which hold background data used in the
// elevator window - [grphbmp[ELEVATOR_FRM_BACKGROUND]] and [grphbmp[ELEVATOR_FRM_PANEL]].
// For unknown reason they are using -1 to denote that they are not set
// (instead of using NULL).
#define ELEVATOR_BACKGROUND_NULL ((unsigned char*)(-1))

// NOTE: This enum is a little bit unsual. It contains two types of members:
// static and dynamic. Static part of enum is always accessed with array
// operations as commonly seen in other UI setup/teardown loops. Background and
// panel are always accessed separately without using loops, but they are a part
// of globals holding state (buffers, cache keys, dimensions).
typedef enum ElevatorFrm {
    ELEVATOR_FRM_BUTTON_DOWN,
    ELEVATOR_FRM_BUTTON_UP,
    ELEVATOR_FRM_GAUGE,
    ELEVATOR_FRM_BACKGROUND,
    ELEVATOR_FRM_PANEL,
    ELEVATOR_FRM_COUNT,
    ELEVATOR_FRM_STATIC_COUNT = 3,
} ElevatorFrm;

typedef struct ElevatorBackground {
    int backgroundFrmId;
    int panelFrmId;
} ElevatorBackground;

typedef struct ElevatorDescription {
    int map;
    int elevation;
    int tile;
} ElevatorDescription;

static int elevator_start(int elevator);
static void elevator_end();
static int Check4Keys(int elevator, int keyCode);

// 0x437B80
static const int grph_id[ELEVATOR_FRM_STATIC_COUNT] = {
    141, // ebut_in.frm - map elevator screen
    142, // ebut_out.frm - map elevator screen
    149, // gaj000.frm - map elevator screen
};

// 0x437B8C
static const ElevatorBackground intotal[ELEVATOR_COUNT] = {
    /*    BROTHERHOOD_OF_STEEL_MAIN */ { 143, -1 },
    /* BROTHERHOOD_OF_STEEL_SURFACE */ { 143, 150 },
    /*                 MASTER_UPPER */ { 144, -1 },
    /*                 MASTER_LOWER */ { 144, 145 },
    /*          MILITARY_BASE_UPPER */ { 146, -1 },
    /*          MILITARY_BASE_LOWER */ { 146, 147 },
    /*                   GLOW_UPPER */ { 146, -1 },
    /*                   GLOW_LOWER */ { 146, 151 },
    /*                     VAULT_13 */ { 148, -1 },
    /*                   NECROPOLIS */ { 148, -1 },
    /*                     SIERRA_1 */ { 148, -1 },
    /*                     SIERRA_2 */ { 146, 152 },
};

// Number of levels for eleveators.
//
// 0x437BEC
static const int btncnt[ELEVATOR_COUNT] = {
    /*    BROTHERHOOD_OF_STEEL_MAIN */ 4,
    /* BROTHERHOOD_OF_STEEL_SURFACE */ 2,
    /*                 MASTER_UPPER */ 3,
    /*                 MASTER_LOWER */ 2,
    /*          MILITARY_BASE_UPPER */ 3,
    /*          MILITARY_BASE_LOWER */ 2,
    /*                   GLOW_UPPER */ 3,
    /*                   GLOW_LOWER */ 3,
    /*                     VAULT_13 */ 3,
    /*                   NECROPOLIS */ 3,
    /*                     SIERRA_1 */ 3,
    /*                     SIERRA_2 */ 3,
};

// 0x437C1C
static const ElevatorDescription retvals[ELEVATOR_COUNT][ELEVATOR_LEVEL_MAX] = {
    {
        { 14, 0, 18940 },
        { 14, 1, 18936 },
        { 15, 0, 21340 },
        { 15, 1, 21340 },
    },
    {
        { 13, 0, 20502 },
        { 14, 0, 14912 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 33, 0, 12498 },
        { 33, 1, 20094 },
        { 34, 0, 17312 },
        { 0, 0, -1 },
    },
    {
        { 34, 0, 16140 },
        { 34, 1, 16140 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 31, 0, 14920 },
        { 31, 1, 14920 },
        { 32, 0, 12944 },
        { 0, 0, -1 },
    },
    {
        { 32, 0, 24520 },
        { 32, 1, 24520 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 42, 0, 22526 },
        { 42, 1, 22526 },
        { 42, 2, 22526 },
        { 0, 0, -1 },
    },
    {
        { 42, 2, 14086 },
        { 43, 0, 14086 },
        { 43, 2, 14086 },
        { 0, 0, -1 },
    },
    {
        { 6, 0, 14104 },
        { 6, 1, 22504 },
        { 6, 2, 17312 },
        { 0, 0, -1 },
    },
    {
        { 9, 0, 13704 },
        { 9, 1, 23302 },
        { 9, 2, 17308 },
        { 0, 0, -1 },
    },
    {
        { 9, 0, 13704 },
        { 9, 1, 23302 },
        { 9, 2, 17308 },
        { 0, 0, -1 },
    },
    {
        { 43, 0, 14130 },
        { 43, 1, 14130 },
        { 43, 2, 14130 },
        { 0, 0, -1 },
    },
};

// NOTE: These values are also used as key bindings.
//
// 0x437E5C
static const char keytable[ELEVATOR_COUNT][ELEVATOR_LEVEL_MAX] = {
    { '1', '2', '3', '4' },
    { 'G', '1', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '6', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '4', '5', '6', '\0' },
};

// 0x504F3C
static const char* sfxtable[ELEVATOR_LEVEL_MAX - 1][ELEVATOR_LEVEL_MAX] = {
    {
        "ELV1_1",
        "ELV1_1",
        "ERROR",
        "ERROR",
    },
    {
        "ELV1_2",
        "ELV1_2",
        "ELV1_1",
        "ERROR",
    },
    {
        "ELV1_3",
        "ELV1_3",
        "ELV2_3",
        "ELV1_1",
    },
};

// 0x56ED30
static Size GInfo[ELEVATOR_FRM_COUNT];

// 0x56ED58
static int elev_win;

// 0x56ED5C
static unsigned char* win_buf;

// 0x56ED60
static bool bk_enable;

// 0x56ED64
static CacheEntry* grph_key[ELEVATOR_FRM_COUNT];

// 0x56ED78
static unsigned char* grphbmp[ELEVATOR_FRM_COUNT];

// Presents elevator dialog for player to pick a desired level.
//
// 0x437E8C
// SELECTION FRAME. ⚠️ Parking the cursor on the chosen button was not enough -- "ted se při výběru
// patra neukazuje kurzor" -- and it is the same lesson Preferences already learned: a cursor is a
// pointer, not a statement about which control you are editing, and on this screen it is not even
// reliably visible. So the selected floor button is marked on the panel itself (since 2026-10-09 with
// its own pressed-button art, not a frame -- see below).
//
// ⚠️ SAVE-UNDER, because this window is painted ONCE (background + panel art in elevator_start) and
// keeps no clean copy to erase against: rubbing the frame out with background colour would take the
// panel art with it. Keep the pixels the mark covers and put them back before anything
// else draws. That only stays consistent if nothing draws while the frame is up, so the loop is
// strictly ERASE -> input -> DRAW.
#define ELEV_HL_MAX 64

// ⚠️ NO YELLOW FRAME ANY MORE (2026-10-09): *"highlighted žlutě floors u výtahu, I fucking hate that"*
// -- the same verdict as the yellow interact outline on 2026-10-04. The selected floor now shows the
// panel's OWN pressed-button art (ELEVATOR_FRM_BUTTON_DOWN), the frame the button shows under a
// mouse click: the selection is visible, and it is the game's art, not ours. Same save-under rule as
// the frame had: the whole button rect is saved, the pressed art drawn, and the saved pixels put
// back before anything else can draw.
static unsigned char elev_hl_save[ELEV_HL_MAX * ELEV_HL_MAX];
static int elev_hl_x = -1;
static int elev_hl_y;
static int elev_hl_w;
static int elev_hl_h;

static void elevator_hl_erase()
{
    if (elev_hl_x < 0 || win_buf == NULL) {
        return;
    }

    int pitch = GInfo[ELEVATOR_FRM_BACKGROUND].width;
    for (int i = 0; i < elev_hl_h; i++) {
        memcpy(win_buf + pitch * (elev_hl_y + i) + elev_hl_x, elev_hl_save + elev_hl_w * i, elev_hl_w);
    }

    elev_hl_x = -1;
}

static void elevator_hl_draw(int level)
{
    if (win_buf == NULL || grphbmp[ELEVATOR_FRM_BUTTON_DOWN] == NULL) {
        return;
    }

    // Exactly where elevator_start registers the button: x 13, y 40 + 60 per level.
    int x = 13;
    int y = 40 + 60 * level;
    int w = GInfo[ELEVATOR_FRM_BUTTON_DOWN].width;
    int h = GInfo[ELEVATOR_FRM_BUTTON_DOWN].height;

    int pitch = GInfo[ELEVATOR_FRM_BACKGROUND].width;
    if (x < 0 || y < 0 || x + w > pitch || y + h > GInfo[ELEVATOR_FRM_BACKGROUND].height) {
        return;
    }
    if (w < 1 || h < 1 || w > ELEV_HL_MAX || h > ELEV_HL_MAX) {
        return;
    }

    for (int i = 0; i < h; i++) {
        memcpy(elev_hl_save + w * i, win_buf + pitch * (y + i) + x, w);
    }

    trans_buf_to_buf(grphbmp[ELEVATOR_FRM_BUTTON_DOWN], w, h, w, win_buf + pitch * y + x, pitch);

    elev_hl_x = x;
    elev_hl_y = y;
    elev_hl_w = w;
    elev_hl_h = h;
}

// Park the cursor on floor button `level`. The buttons are registered at x 13, y 40 + 60 per
// level (elevator_start), so the centre is that plus half the button art.
static void elevator_pad_point(int level)
{
    Rect winRect;
    if (win_get_rect(elev_win, &winRect) != 0) {
        return;
    }

    mouse_set_position(winRect.ulx + 13 + GInfo[ELEVATOR_FRM_BUTTON_DOWN].width / 2,
        winRect.uly + 40 + 60 * level + GInfo[ELEVATOR_FRM_BUTTON_DOWN].height / 2);
}

int elevator_select(int elevator, int* mapPtr, int* elevationPtr, int* tilePtr)
{
    if (elevator < 0 || elevator >= ELEVATOR_COUNT) {
        trace_event(TRACE_WARN, "elev-refused", "why=type type=%d count=%d", elevator, ELEVATOR_COUNT);
        return -1;
    }

    // ⚠️ Take the world prompt down BEFORE the panel paints -- the same rule the Pip-Boy / character
    // sheet / automap already follow (gpui_open_dpad_screen). The floating hand and the target
    // outline live in their OWN always-on-top window, and interact_prompt_tick -- the only thing
    // that ever hides them -- runs from gamepad_ui_tick, which does not run inside this screen's own
    // get_input loop. So the hand that was highlighting the elevator when you pressed A stayed
    // painted over the floor buttons for as long as the panel was up ("ta ruka mě sere při výtahu").
    // The tick's own suppression list cannot help here either: it keys on menus, dialogue, wheels
    // and the inventory, and the elevator is none of those.
    interact_prompt_reset();

    if (elevator_start(elevator) == -1) {
        trace_event(TRACE_WARN, "elev-refused", "why=start type=%d", elevator);
        return -1;
    }

    const ElevatorDescription* elevatorDescription = retvals[elevator];

    int index;
    for (index = 0; index < ELEVATOR_LEVEL_MAX; index++) {
        if (elevatorDescription[index].map == *mapPtr) {
            break;
        }
    }

    if (index < ELEVATOR_LEVEL_MAX) {
        if (elevatorDescription[*elevationPtr + index].tile != -1) {
            *elevationPtr += index;
        }
    }

    if (elevator == ELEVATOR_GLOW_LOWER && *mapPtr == 42) {
        *elevationPtr -= 2;
    }

    debug_printf("\n the start elev level %d\n", *elevationPtr);

    int v18 = (GInfo[ELEVATOR_FRM_GAUGE].width * GInfo[ELEVATOR_FRM_GAUGE].height) / 13;
    float v42 = 12.0f / (float)(btncnt[elevator] - 1);
    buf_to_buf(
        grphbmp[ELEVATOR_FRM_GAUGE] + v18 * (int)((float)(*elevationPtr) * v42),
        GInfo[ELEVATOR_FRM_GAUGE].width,
        GInfo[ELEVATOR_FRM_GAUGE].height / 13,
        GInfo[ELEVATOR_FRM_GAUGE].width,
        win_buf + GInfo[ELEVATOR_FRM_BACKGROUND].width * 41 + 121,
        GInfo[ELEVATOR_FRM_BACKGROUND].width);
    win_draw(elev_win);

    // ⚠️ THE PANEL HAD NO GAMEPAD AT ALL. Its only inputs were ESC and the four floor BUTTONS,
    // which a pad can reach solely by happening to leave the world cursor on top of one -- so on a
    // controller the elevator opened, refused every button, and closed again: "PROSTE TEN VYTAH
    // NEJDE NIKAM JENOM SE ZAVIRAJI VETSINU CASU DVERE". The doors were the only part of it that
    // ever worked.
    //
    // Same shape as every other modal this pad drives: self_nav so the gameplay layer stops
    // synthesising world clicks behind the panel, D-pad picks, A confirms, B cancels -- and the
    // SELECTION IS SHOWN BY PARKING THE CURSOR on the chosen button, exactly as the world map's
    // town list does, because this window is painted once and keeps no clean background copy to
    // erase a highlight against.
    int sel = *elevationPtr;
    if (sel < 0 || sel >= btncnt[elevator]) {
        sel = 0;
    }

    SDL_GameController* elevPad = gamepad_ui_get_controller();
    if (elevPad != NULL) {
        gamepad_ui_set_self_nav(true);
        elevator_pad_point(sel);
        elevator_hl_draw(sel);
        win_draw(elev_win);
    }

    bool prevUp = false;
    bool prevDown = false;
    bool prevA = false;
    bool prevB = false;
    bool aArmed = false; // both buttons must be seen RELEASED before they count -- see below
    bool bArmed = false;
    unsigned int lastStep = 0;

    bool done = false;
    int keyCode;
    while (!done) {
        sharedFpsLimiter.mark();

        // ⚠️ The frame comes down BEFORE anything can draw, and goes back up at the bottom of the
        // frame: the save-under underneath it is only valid while nothing else has touched those
        // pixels.
        elevator_hl_erase();

        if (elevPad != NULL) {
            gamepad_ui_set_self_nav(true); // re-assert: nothing else may take the pad while we are up

            SDL_GameControllerUpdate();
            bool upNow = SDL_GameControllerGetButton(elevPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
            bool downNow = SDL_GameControllerGetButton(elevPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
            bool aNow = SDL_GameControllerGetButton(elevPad, SDL_CONTROLLER_BUTTON_A) != 0;
            bool bNow = SDL_GameControllerGetButton(elevPad, SDL_CONTROLLER_BUTTON_B) != 0;

            // Held D-pad repeats after a beat, like every other list on this pad.
            bool repeat = (upNow || downNow) && elapsed_time(lastStep) >= 220;

            int step = 0;
            if ((upNow && !prevUp) || (upNow && repeat)) {
                step = -1;
            } else if ((downNow && !prevDown) || (downNow && repeat)) {
                step = 1;
            } else {
                // The left stick does it too -- it is the thing your thumb is already on.
                step = gamepad_ui_menu_stick_step();
            }

            if (step != 0) {
                lastStep = get_time();
                int next = sel + step;
                if (next >= 0 && next < btncnt[elevator]) {
                    sel = next;
                    gsound_play_sfx_file("ib1p1xx1");
                    elevator_pad_point(sel);
                }
            }

            // ⚠️⚠️ THE PRESS THAT OPENED THIS SCREEN IS STILL DOWN, AND IT USED TO CONFIRM.
            //
            // This is the whole bug behind "aktivuji vytah a misto toho, abych se dostal k ikonce s
            // patry, se zavrou dvere", and why it was intermittent: A is physically held from the
            // world action that ran the elevator script, the panel opens in the same frame, prevA
            // is a LOCAL that starts false -- so the very first iteration reads a fresh press edge,
            // confirms the floor you are already on, and closes the panel again. All you ever see
            // is the door animation. Let go fast enough and you keep the panel; hold a moment
            // longer and it vanishes.
            //
            // (An earlier comment here argued this could not happen because "the press was consumed
            // by the world action". It is not -- nothing consumes a physical button, and the edge
            // is measured against a local that knows nothing about the world frame.)
            //
            // So each button must be RELEASED ONCE before it means anything on this screen. Same
            // rule every screen that opens on a held button already follows.
            if (!aNow) {
                aArmed = true;
            }
            if (!bNow) {
                bArmed = true;
            }

            if (aArmed && aNow && !prevA) {
                keyCode = 500 + sel;
                done = true;
            } else if (bArmed && bNow && !prevB) {
                keyCode = KEY_ESCAPE;
                done = true;
            }

            prevUp = upNow;
            prevDown = downNow;
            prevA = aNow;
            prevB = bNow;

            if (done) {
                renderPresent();
                sharedFpsLimiter.throttle();
                break;
            }
        }

        keyCode = get_input();
        if (keyCode == KEY_ESCAPE) {
            done = true;
        }

        if (keyCode >= 500 && keyCode < 504) {
            done = true;
        }

        if (keyCode > 0 && keyCode < 500) {
            int level = Check4Keys(elevator, keyCode);
            if (level != 0) {
                keyCode = 500 + level - 1;
                done = true;
            }
        }

        if (!done && elevPad != NULL) {
            elevator_hl_draw(sel);
            win_draw(elev_win);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    // Down for good: the travel animation below repaints the gauge and must not find a frame it
    // knows nothing about sitting in the buffer.
    elevator_hl_erase();
    win_draw(elev_win);

    // How the panel was left: which floor, and whether it was chosen or escaped. A picked= that
    // equals the floor you were already on, one frame after it opened, is the held-A signature.
    trace_event(TRACE_FLOW, "elev-closed", "picked=%d from=%d escaped=%d pad=%d",
        keyCode == KEY_ESCAPE ? -1 : keyCode - 500, *elevationPtr, keyCode == KEY_ESCAPE ? 1 : 0,
        elevPad != NULL ? 1 : 0);

    if (keyCode != KEY_ESCAPE) {
        keyCode -= 500;

        if (*elevationPtr != keyCode) {
            float v43 = (float)(btncnt[elevator] - 1) / 12.0f;

            unsigned int delay = (unsigned int)(v43 * 276.92307);

            if (keyCode < *elevationPtr) {
                v43 = -v43;
            }

            int numberOfLevelsTravelled = keyCode - *elevationPtr;
            if (numberOfLevelsTravelled < 0) {
                numberOfLevelsTravelled = -numberOfLevelsTravelled;
            }

            gsound_play_sfx_file(sfxtable[btncnt[elevator] - 2][numberOfLevelsTravelled]);

            float v41 = (float)keyCode * v42;
            float v44 = (float)(*elevationPtr) * v42;
            do {
                sharedFpsLimiter.mark();

                unsigned int tick = get_time();
                v44 += v43;
                buf_to_buf(
                    grphbmp[ELEVATOR_FRM_GAUGE] + v18 * (int)v44,
                    GInfo[ELEVATOR_FRM_GAUGE].width,
                    GInfo[ELEVATOR_FRM_GAUGE].height / 13,
                    GInfo[ELEVATOR_FRM_GAUGE].width,
                    win_buf + GInfo[ELEVATOR_FRM_BACKGROUND].width * 41 + 121,
                    GInfo[ELEVATOR_FRM_BACKGROUND].width);

                win_draw(elev_win);

                while (elapsed_time(tick) < delay) {
                }

                renderPresent();
                sharedFpsLimiter.throttle();
            } while ((v43 <= 0.0 || v44 < v41) && (v43 > 0.0 || v44 > v41));

            pause_for_tocks(200);
        }
    }

    elevator_end();

    // Hand the pad back. ⚠️ A leaked self_nav means the controller is dead in the game world, so it
    // is cleared on the ONE exit this function has past the panel.
    if (elevPad != NULL) {
        gamepad_ui_set_self_nav(false);
    }

    if (keyCode != KEY_ESCAPE) {
        const ElevatorDescription* description = &(elevatorDescription[keyCode]);
        *mapPtr = description->map;
        *elevationPtr = description->elevation;
        *tilePtr = description->tile;
    } else {
        if (elevator == ELEVATOR_GLOW_LOWER) {
            *elevationPtr += 2;
        }
    }

    return 0;
}

// 0x43821C
static int elevator_start(int elevator)
{
    bk_enable = map_disable_bk_processes();
    cycle_disable();

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    gmouse_3d_off();

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    int index;
    for (index = 0; index < ELEVATOR_FRM_STATIC_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, grph_id[index], 0, 0, 0);
        grphbmp[index] = art_lock(fid, &(grph_key[index]), &(GInfo[index].width), &(GInfo[index].height));
        if (grphbmp[index] == NULL) {
            break;
        }
    }

    if (index != ELEVATOR_FRM_STATIC_COUNT) {
        for (int reversedIndex = index - 1; reversedIndex >= 0; reversedIndex--) {
            art_ptr_unlock(grph_key[reversedIndex]);
        }

        if (bk_enable) {
            map_enable_bk_processes();
        }

        cycle_enable();
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    grphbmp[ELEVATOR_FRM_PANEL] = ELEVATOR_BACKGROUND_NULL;
    grphbmp[ELEVATOR_FRM_BACKGROUND] = ELEVATOR_BACKGROUND_NULL;

    const ElevatorBackground* elevatorBackground = &(intotal[elevator]);
    bool backgroundsLoaded = true;

    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, elevatorBackground->backgroundFrmId, 0, 0, 0);
    grphbmp[ELEVATOR_FRM_BACKGROUND] = art_lock(backgroundFid, &grph_key[ELEVATOR_FRM_BACKGROUND], &(GInfo[ELEVATOR_FRM_BACKGROUND].width), &(GInfo[ELEVATOR_FRM_BACKGROUND].height));
    if (grphbmp[ELEVATOR_FRM_BACKGROUND] != NULL) {
        if (elevatorBackground->panelFrmId != -1) {
            int panelFid = art_id(OBJ_TYPE_INTERFACE, elevatorBackground->panelFrmId, 0, 0, 0);
            grphbmp[ELEVATOR_FRM_PANEL] = art_lock(panelFid, &grph_key[ELEVATOR_FRM_PANEL], &(GInfo[ELEVATOR_FRM_PANEL].width), &(GInfo[ELEVATOR_FRM_PANEL].height));
            if (grphbmp[ELEVATOR_FRM_PANEL] == NULL) {
                grphbmp[ELEVATOR_FRM_PANEL] = ELEVATOR_BACKGROUND_NULL;
                backgroundsLoaded = false;
            }
        }
    } else {
        grphbmp[ELEVATOR_FRM_BACKGROUND] = ELEVATOR_BACKGROUND_NULL;
        backgroundsLoaded = false;
    }

    if (!backgroundsLoaded) {
        if (grphbmp[ELEVATOR_FRM_BACKGROUND] != ELEVATOR_BACKGROUND_NULL) {
            art_ptr_unlock(grph_key[ELEVATOR_FRM_BACKGROUND]);
        }

        if (grphbmp[ELEVATOR_FRM_PANEL] != ELEVATOR_BACKGROUND_NULL) {
            art_ptr_unlock(grph_key[ELEVATOR_FRM_PANEL]);
        }

        for (int index = 0; index < ELEVATOR_FRM_STATIC_COUNT; index++) {
            art_ptr_unlock(grph_key[index]);
        }

        if (bk_enable) {
            map_enable_bk_processes();
        }

        cycle_enable();
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    int elevatorWindowX = (screenGetWidth() - GInfo[ELEVATOR_FRM_BACKGROUND].width) / 2;
    int elevatorWindowY = (screenGetHeight() - INTERFACE_BAR_HEIGHT - 1 - GInfo[ELEVATOR_FRM_BACKGROUND].height) / 2;
    elev_win = win_add(
        elevatorWindowX,
        elevatorWindowY,
        GInfo[ELEVATOR_FRM_BACKGROUND].width,
        GInfo[ELEVATOR_FRM_BACKGROUND].height,
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (elev_win == -1) {
        if (grphbmp[ELEVATOR_FRM_BACKGROUND] != ELEVATOR_BACKGROUND_NULL) {
            art_ptr_unlock(grph_key[ELEVATOR_FRM_BACKGROUND]);
        }

        if (grphbmp[ELEVATOR_FRM_PANEL] != ELEVATOR_BACKGROUND_NULL) {
            art_ptr_unlock(grph_key[ELEVATOR_FRM_PANEL]);
        }

        for (int index = 0; index < ELEVATOR_FRM_STATIC_COUNT; index++) {
            art_ptr_unlock(grph_key[index]);
        }

        if (bk_enable) {
            map_enable_bk_processes();
        }

        cycle_enable();
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    win_buf = win_get_buf(elev_win);
    memcpy(win_buf, (unsigned char*)grphbmp[ELEVATOR_FRM_BACKGROUND], GInfo[ELEVATOR_FRM_BACKGROUND].width * GInfo[ELEVATOR_FRM_BACKGROUND].height);

    if (grphbmp[ELEVATOR_FRM_PANEL] != ELEVATOR_BACKGROUND_NULL) {
        buf_to_buf((unsigned char*)grphbmp[ELEVATOR_FRM_PANEL],
            GInfo[ELEVATOR_FRM_PANEL].width,
            GInfo[ELEVATOR_FRM_PANEL].height,
            GInfo[ELEVATOR_FRM_PANEL].width,
            win_buf + GInfo[ELEVATOR_FRM_BACKGROUND].width * (GInfo[ELEVATOR_FRM_BACKGROUND].height - GInfo[ELEVATOR_FRM_PANEL].height),
            GInfo[ELEVATOR_FRM_BACKGROUND].width);
    }

    int y = 40;
    for (int level = 0; level < btncnt[elevator]; level++) {
        int btn = win_register_button(elev_win,
            13,
            y,
            GInfo[ELEVATOR_FRM_BUTTON_DOWN].width,
            GInfo[ELEVATOR_FRM_BUTTON_DOWN].height,
            -1,
            -1,
            -1,
            500 + level,
            grphbmp[ELEVATOR_FRM_BUTTON_UP],
            grphbmp[ELEVATOR_FRM_BUTTON_DOWN],
            NULL,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, NULL);
        }
        y += 60;
    }

    return 0;
}

// 0x4385C4
static void elevator_end()
{
    win_delete(elev_win);

    if (grphbmp[ELEVATOR_FRM_BACKGROUND] != ELEVATOR_BACKGROUND_NULL) {
        art_ptr_unlock(grph_key[ELEVATOR_FRM_BACKGROUND]);
    }

    if (grphbmp[ELEVATOR_FRM_PANEL] != ELEVATOR_BACKGROUND_NULL) {
        art_ptr_unlock(grph_key[ELEVATOR_FRM_PANEL]);
    }

    for (int index = 0; index < ELEVATOR_FRM_STATIC_COUNT; index++) {
        art_ptr_unlock(grph_key[index]);
    }

    if (bk_enable) {
        map_enable_bk_processes();
    }

    cycle_enable();

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
}

// 0x43862C
static int Check4Keys(int elevator, int keyCode)
{
    // TODO: Check if result is really unused?
    toupper(keyCode);

    for (int index = 0; index < ELEVATOR_LEVEL_MAX; index++) {
        char c = keytable[elevator][index];
        if (c == '\0') {
            break;
        }

        if (c == (char)(keyCode & 0xFF)) {
            return index + 1;
        }
    }
    return 0;
}

} // namespace fallout
