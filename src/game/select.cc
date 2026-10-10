#include "game/select.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game/art.h"
#include "game/critter.h"
#include "game/editor.h"
#include "game/game.h"
#include "game/gamepad_ui.h"
#include "game/gsound.h"
#include "game/message.h"
#include "game/object.h"
#include "game/options.h"
#include "game/palette.h"
#include "game/proto.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/trait.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/db/db.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define CS_WINDOW_WIDTH 640
#define CS_WINDOW_HEIGHT 480

#define CS_WINDOW_BACKGROUND_X 40
#define CS_WINDOW_BACKGROUND_Y 30
#define CS_WINDOW_BACKGROUND_WIDTH 560
#define CS_WINDOW_BACKGROUND_HEIGHT 300

#define CS_WINDOW_PREVIOUS_BUTTON_X 292
#define CS_WINDOW_PREVIOUS_BUTTON_Y 320

#define CS_WINDOW_NEXT_BUTTON_X 318
#define CS_WINDOW_NEXT_BUTTON_Y 320

#define CS_WINDOW_TAKE_BUTTON_X 81
#define CS_WINDOW_TAKE_BUTTON_Y 323

#define CS_WINDOW_MODIFY_BUTTON_X 435
#define CS_WINDOW_MODIFY_BUTTON_Y 320

#define CS_WINDOW_CREATE_BUTTON_X 80
#define CS_WINDOW_CREATE_BUTTON_Y 425

#define CS_WINDOW_BACK_BUTTON_X 461
#define CS_WINDOW_BACK_BUTTON_Y 425

#define CS_WINDOW_NAME_MID_X 318
#define CS_WINDOW_PRIMARY_STAT_MID_X 348
#define CS_WINDOW_SECONDARY_STAT_MID_X 365
#define CS_WINDOW_BIO_X 420

typedef enum PremadeCharacter {
    PREMADE_CHARACTER_NARG,
    PREMADE_CHARACTER_CHITSA,
    PREMADE_CHARACTER_MINGUN,
    PREMADE_CHARACTER_COUNT,
} PremadeCharacter;

typedef struct PremadeCharacterDescription {
    char fileName[20];
    int face;
    char vid[20];
} PremadeCharacterDescription;

static void select_exit();
static bool select_update_display();
static bool select_display_portrait();
static bool select_display_stats();
static bool select_display_bio();
static bool select_fatal_error(bool rc);

// 0x50797C
int select_window_id = -1;

// 0x507980
static unsigned char* select_window_buffer = NULL;

// 0x507984
static unsigned char* monitor = NULL;

// 0x507988
static Rect monitor_rect = { 40, 30, 599, 329 };

// 0x507998
static int previous_button = -1;

// 0x50799C
static CacheEntry* previous_button_up_key = NULL;

// 0x5079A0
static CacheEntry* previous_button_down_key = NULL;

// 0x5079A4
static int next_button = -1;

// 0x5079A8
static CacheEntry* next_button_up_key = NULL;

// 0x5079AC
static CacheEntry* next_button_down_key = NULL;

// 0x5079B0
static int take_button = -1;

// 0x5079B4
static CacheEntry* take_button_up_key = NULL;

// 0x5079B8
static CacheEntry* take_button_down_key = NULL;

// 0x5079BC
static int modify_button = -1;

// 0x5079C0
static CacheEntry* modify_button_up_key = NULL;

// 0x5079C4
static CacheEntry* modify_button_down_key = NULL;

// 0x5079C8
static int create_button = -1;

// 0x5079CC
static CacheEntry* create_button_up_key = NULL;

// 0x5079D0
static CacheEntry* create_button_down_key = NULL;

// 0x5079D4
static int back_button = -1;

// 0x5079D8
static CacheEntry* back_button_up_key = NULL;

// 0x5079DC
static CacheEntry* back_button_down_key = NULL;

// 0x5079E0
static int premade_index = PREMADE_CHARACTER_NARG;

// 0x5079E4
static PremadeCharacterDescription premade_characters[PREMADE_CHARACTER_COUNT] = {
    { "premade\\combat", 201, "VID 208-197-88-125" },
    { "premade\\stealth", 202, "VID 208-206-49-229" },
    { "premade\\diplomat", 203, "VID 208-206-49-227" },
};

// 0x507A68
static int premade_total = PREMADE_CHARACTER_COUNT;

// 0x664FCC
static unsigned char* take_button_up;

// 0x664FD4
static unsigned char* modify_button_down;

// 0x664FBC
static unsigned char* back_button_up;

// 0x664FE0
static unsigned char* create_button_up;

// 0x664FDC
static unsigned char* modify_button_up;

// 0x664FC0
static unsigned char* back_button_down;

// 0x664FE0
static unsigned char* create_button_down;

// 0x664FE4
static unsigned char* take_button_down;

// 0x664FC4
static unsigned char* next_button_down;

// 0x664FC8
static unsigned char* next_button_up;

// 0x664FD0
static unsigned char* previous_button_up;

// 0x664FE8
static unsigned char* previous_button_down;

// 0x495260
// CONTROLLER NAVIGATION for the character SELECTION screen -- the first thing New Game shows, and
// until now the one pre-game screen the pad could not touch at all: the three premade characters
// and all four buttons were mouse-only.
//
// Same idiom as Preferences, deliberately: D-pad UP/DOWN picks the control, LEFT/RIGHT changes the
// VALUE -- and on this screen the value IS which premade character you are looking at, so the arrows
// under the portrait get driven directly instead of being focusable stops of their own. A presses
// the focused button, B backs out.
//
// The indicator is the same 1px frame in Fallout's selection yellow. It has to be a frame here for
// the same reason as Preferences: the labels are painted once by select_update_display and never
// repainted, so anything erased against the background would eat them. Hence a SAVE-UNDER and the
// strict erase -> input -> draw order in the loop below.
// Every clickable thing on the screen, the arrows included. They are ordinary stops like anything
// else -- an earlier version drove them off D-pad left/right instead, which stole the two obvious
// \"move sideways\" directions and left the layout only half navigable.
struct SelFocusItem {
    int x;
    int y;
    int w;
    int h;
    int code; // what pressing A sends -- the same code the on-screen button emits
};

static const SelFocusItem sel_items[] = {
    { CS_WINDOW_TAKE_BUTTON_X, CS_WINDOW_TAKE_BUTTON_Y, 15, 16, KEY_LOWERCASE_T },
    { CS_WINDOW_PREVIOUS_BUTTON_X, CS_WINDOW_PREVIOUS_BUTTON_Y, 20, 18, 500 },
    { CS_WINDOW_NEXT_BUTTON_X, CS_WINDOW_NEXT_BUTTON_Y, 20, 18, 501 },
    { CS_WINDOW_MODIFY_BUTTON_X, CS_WINDOW_MODIFY_BUTTON_Y, 15, 16, KEY_LOWERCASE_M },
    { CS_WINDOW_CREATE_BUTTON_X, CS_WINDOW_CREATE_BUTTON_Y, 15, 16, KEY_LOWERCASE_C },
    { CS_WINDOW_BACK_BUTTON_X, CS_WINDOW_BACK_BUTTON_Y, 15, 16, KEY_ESCAPE },
};

#define SEL_FOCUS_COUNT ((int)(sizeof(sel_items) / sizeof(sel_items[0])))
#define SEL_FOCUS_TAKE 0

#define SEL_HL_MAX_W 40
#define SEL_HL_MAX_H 40

static unsigned char sel_hl_top[SEL_HL_MAX_W];
static unsigned char sel_hl_bottom[SEL_HL_MAX_W];
static unsigned char sel_hl_left[SEL_HL_MAX_H];
static unsigned char sel_hl_right[SEL_HL_MAX_H];
static int sel_hl_x = -1;
static int sel_hl_y;
static int sel_hl_w;
static int sel_hl_h;
static int sel_focus = SEL_FOCUS_TAKE;

// Three pixels of air around the focused item.
static void sel_focus_bounds(int* x, int* y, int* w, int* h)
{
    const SelFocusItem* it = &(sel_items[sel_focus]);
    *x = it->x - 3;
    *y = it->y - 3;
    *w = it->w + 6;
    *h = it->h + 6;
}

// Drag the POINTER along with the frame. The two are different indicators of the same thing and
// the player reads whichever catches the eye first, so they must never disagree -- a frame on TAKE
// while the arrow sits over BACK is worse than having no frame at all. Screen coordinates: the
// window is centred, so the item's window-relative position has to be offset by the window origin.
static void sel_park_cursor()
{
    if (select_window_id == -1) {
        return;
    }

    const SelFocusItem* it = &(sel_items[sel_focus]);
    int originX = (screenGetWidth() - CS_WINDOW_WIDTH) / 2;
    int originY = (screenGetHeight() - CS_WINDOW_HEIGHT) / 2;
    mouse_set_position(originX + it->x + it->w / 2, originY + it->y + it->h / 2);
}

// SPATIAL step: from the focused item, the nearest one that genuinely lies in the direction pushed.
//
// Two rules, and both are needed because this panel is not a tidy grid -- the top row sits at
// y 320/323 and the two bottom buttons at x 80/461, so a naive "is it further along this axis"
// test made LEFT off TAKE jump down to CREATE (one pixel to the left of it) and UP off TAKE land
// on an arrow three pixels higher.
//
//  1. The candidate must move at least SEL_STEP_MIN along the pushed axis -- a few pixels of
//     sloppy layout is not a direction.
//  2. Candidates that OVERLAP the current item on the other axis win outright (a real row or
//     column neighbour); only if none do, items merely in line with it (within SEL_STEP_SLOP)
//     are considered. Anything further off to the side is not "that way" at all, and the focus
//     stays put rather than teleporting across the panel.
#define SEL_STEP_MIN 10
#define SEL_STEP_SLOP 40

static void sel_focus_step(int dx, int dy)
{
    const SelFocusItem* cur = &(sel_items[sel_focus]);
    int cx = cur->x + cur->w / 2;
    int cy = cur->y + cur->h / 2;

    int best = -1;
    int bestScore = 0;
    bool bestOverlaps = false;

    for (int i = 0; i < SEL_FOCUS_COUNT; i++) {
        if (i == sel_focus) {
            continue;
        }

        const SelFocusItem* it = &(sel_items[i]);
        int ix = it->x + it->w / 2;
        int iy = it->y + it->h / 2;

        int along = dx != 0 ? (ix - cx) * dx : (iy - cy) * dy;
        if (along < SEL_STEP_MIN) {
            continue;
        }

        int across = dx != 0 ? (iy - cy) : (ix - cx);
        if (across < 0) {
            across = -across;
        }

        bool overlaps = dx != 0
            ? (it->y < cur->y + cur->h && it->y + it->h > cur->y)
            : (it->x < cur->x + cur->w && it->x + it->w > cur->x);

        if (!overlaps && across > SEL_STEP_SLOP) {
            continue;
        }

        int score = along + 2 * across;

        // A row/column neighbour always beats one that is merely in line with us.
        if (best == -1 || (overlaps && !bestOverlaps)
            || (overlaps == bestOverlaps && score < bestScore)) {
            best = i;
            bestScore = score;
            bestOverlaps = overlaps;
        }
    }

    if (best != -1) {
        sel_focus = best;
        sel_park_cursor();
        gsound_play_sfx_file("ib1p1xx1");
    }
}

static void sel_hl_erase()
{
    if (sel_hl_x < 0 || select_window_id == -1) {
        return;
    }

    unsigned char* buf = win_get_buf(select_window_id);
    if (buf == NULL) {
        sel_hl_x = -1;
        return;
    }

    memcpy(buf + CS_WINDOW_WIDTH * sel_hl_y + sel_hl_x, sel_hl_top, sel_hl_w);
    memcpy(buf + CS_WINDOW_WIDTH * (sel_hl_y + sel_hl_h - 1) + sel_hl_x, sel_hl_bottom, sel_hl_w);
    for (int i = 0; i < sel_hl_h; i++) {
        unsigned char* row = buf + CS_WINDOW_WIDTH * (sel_hl_y + i);
        row[sel_hl_x] = sel_hl_left[i];
        row[sel_hl_x + sel_hl_w - 1] = sel_hl_right[i];
    }

    sel_hl_x = -1;
}

static void sel_hl_draw()
{
    if (select_window_id == -1) {
        return;
    }

    unsigned char* buf = win_get_buf(select_window_id);
    if (buf == NULL) {
        return;
    }

    int x;
    int y;
    int w;
    int h;
    sel_focus_bounds(&x, &y, &w, &h);

    if (x < 0 || y < 0 || w < 3 || h < 3 || w > SEL_HL_MAX_W || h > SEL_HL_MAX_H
        || x + w > CS_WINDOW_WIDTH || y + h > CS_WINDOW_HEIGHT) {
        return;
    }

    // Whole footprint saved before any of it is drawn, or the corners come back yellow.
    memcpy(sel_hl_top, buf + CS_WINDOW_WIDTH * y + x, w);
    memcpy(sel_hl_bottom, buf + CS_WINDOW_WIDTH * (y + h - 1) + x, w);
    for (int i = 0; i < h; i++) {
        const unsigned char* row = buf + CS_WINDOW_WIDTH * (y + i);
        sel_hl_left[i] = row[x];
        sel_hl_right[i] = row[x + w - 1];
    }

    unsigned char color = colorTable[32747];
    memset(buf + CS_WINDOW_WIDTH * y + x, color, w);
    memset(buf + CS_WINDOW_WIDTH * (y + h - 1) + x, color, w);
    for (int i = 0; i < h; i++) {
        unsigned char* row = buf + CS_WINDOW_WIDTH * (y + i);
        row[x] = color;
        row[x + w - 1] = color;
    }

    sel_hl_x = x;
    sel_hl_y = y;
    sel_hl_w = w;
    sel_hl_h = h;
}

static bool sel_prev_up;
static bool sel_prev_down;
static bool sel_prev_left;
static bool sel_prev_right;
static bool sel_prev_a;
static bool sel_prev_b;

// Latch the CURRENT state as "already seen", so the press that opened this screen (A on the main
// menu) is not read as a fresh press on the first frame and does not instantly hit TAKE.
static void sel_pad_prime(SDL_GameController* pad)
{
    SDL_GameControllerUpdate();
    sel_prev_up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
    sel_prev_down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
    sel_prev_left = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) < -16000;
    sel_prev_right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) > 16000;
    sel_prev_a = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    sel_prev_b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
}

static void sel_pad_poll(SDL_GameController* pad)
{
    SDL_GameControllerUpdate();

    bool up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
    bool down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
    bool left = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) < -16000;
    bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0
        || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) > 16000;
    bool a = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;

    // All four directions move the FOCUS. Nothing else -- the arrows are stops of their own, so
    // switching character is just A on one of them.
    if (up && !sel_prev_up) {
        sel_focus_step(0, -1);
    }
    if (down && !sel_prev_down) {
        sel_focus_step(0, 1);
    }
    if (left && !sel_prev_left) {
        sel_focus_step(-1, 0);
    }
    if (right && !sel_prev_right) {
        sel_focus_step(1, 0);
    }

    if (a && !sel_prev_a) {
        GNW_add_input_buffer(sel_items[sel_focus].code);
    }
    if (b && !sel_prev_b) {
        GNW_add_input_buffer(KEY_ESCAPE);
    }

    sel_prev_up = up;
    sel_prev_down = down;
    sel_prev_left = left;
    sel_prev_right = right;
    sel_prev_a = a;
    sel_prev_b = b;
}

int select_character()
{
    if (!select_init()) {
        return 0;
    }

    bool cursorWasHidden = mouse_hidden();
    if (cursorWasHidden) {
        mouse_show();
    }

    // Same reason as the character editor: this screen injects its own button codes and must not
    // inherit anyone else's (see the note in editor_design).
    flush_input_buffer();

    loadColorTable("color.pal");
    palette_fade_to(cmap);

    // Re-fetched every frame, never latched once on entry: gamepad_ui opens the controller LAZILY
    // and its tick does not run inside this loop, so a handle taken here could be NULL for the whole
    // screen -- the trap the Preferences screen was built on.
    sel_focus = SEL_FOCUS_TAKE;
    sel_hl_x = -1;
    {
        SDL_GameController* pad = gamepad_ui_get_controller();
        if (pad != NULL) {
            sel_pad_prime(pad);
            sel_park_cursor(); // start with the pointer already on the framed item
            // A is ours here (it sends the focused item's own key code); stop the shared router
            // from also firing a raw click at the pointer.
            gamepad_ui_set_suppress_a_click(true);
        }
    }

    int rc = 0;
    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        if (game_user_wants_to_quit != 0) {
            break;
        }

        // Erase -> input -> draw, once per frame. The frame is the only thing that touches those
        // pixels between the two, which is what keeps the save-under honest.
        SDL_GameController* selPad = gamepad_ui_get_controller();
        if (selPad != NULL) {
            sel_hl_erase();
            sel_pad_poll(selPad);
        }

        int keyCode = get_input();

        switch (keyCode) {
        case KEY_MINUS:
        case KEY_UNDERSCORE:
            DecGamma();
            break;
        case KEY_EQUAL:
        case KEY_PLUS:
            IncGamma();
            break;
        case KEY_UPPERCASE_B:
        case KEY_LOWERCASE_B:
        case KEY_ESCAPE:
            rc = 3;
            done = true;
            break;
        case KEY_UPPERCASE_C:
        case KEY_LOWERCASE_C:
            ResetPlayer();
            if (editor_design(1) == 0) {
                rc = 2;
                done = true;
            } else {
                select_update_display();
            }

            break;
        case KEY_UPPERCASE_M:
        case KEY_LOWERCASE_M:
            if (!editor_design(1)) {
                rc = 2;
                done = true;
            } else {
                select_update_display();
            }

            break;
        case KEY_UPPERCASE_T:
        case KEY_LOWERCASE_T:
            rc = 2;
            done = true;

            break;
        case KEY_F10:
            game_quit_with_confirm();
            break;
        case KEY_ARROW_LEFT:
            gsound_play_sfx_file("ib2p1xx1");
            // FALLTHROUGH
        case 500:
            premade_index -= 1;
            if (premade_index < 0) {
                premade_index = premade_total - 1;
            }

            select_update_display();
            break;
        case KEY_ARROW_RIGHT:
            gsound_play_sfx_file("ib2p1xx1");
            // FALLTHROUGH
        case 501:
            premade_index += 1;
            if (premade_index >= premade_total) {
                premade_index = 0;
            }

            select_update_display();
            break;
        }

        if (selPad != NULL && !done) {
            sel_hl_draw();
            win_draw(select_window_id);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    sel_hl_erase();
    flush_input_buffer();
    gamepad_ui_set_suppress_a_click(false);

    palette_fade_to(black_palette);
    select_exit();

    if (cursorWasHidden) {
        mouse_hide();
    }

    return rc;
}

// 0x4954F8
bool select_init()
{
    int backgroundFid;
    unsigned char* backgroundFrmData;

    if (select_window_id != -1) {
        return false;
    }

    int characterSelectorWindowX = (screenGetWidth() - CS_WINDOW_WIDTH) / 2;
    int characterSelectorWindowY = (screenGetHeight() - CS_WINDOW_HEIGHT) / 2;
    select_window_id = win_add(characterSelectorWindowX, characterSelectorWindowY, CS_WINDOW_WIDTH, CS_WINDOW_HEIGHT, colorTable[0], 0);
    if (select_window_id == -1) {
        return select_fatal_error(false);
    }

    select_window_buffer = win_get_buf(select_window_id);
    if (select_window_buffer == NULL) {
        return select_fatal_error(false);
    }

    CacheEntry* backgroundFrmHandle;
    backgroundFid = art_id(OBJ_TYPE_INTERFACE, 174, 0, 0, 0);
    backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
    if (backgroundFrmData == NULL) {
        return select_fatal_error(false);
    }

    buf_to_buf(backgroundFrmData,
        CS_WINDOW_WIDTH,
        CS_WINDOW_HEIGHT,
        CS_WINDOW_WIDTH,
        select_window_buffer,
        CS_WINDOW_WIDTH);

    monitor = (unsigned char*)mem_malloc(CS_WINDOW_BACKGROUND_WIDTH * CS_WINDOW_BACKGROUND_HEIGHT);
    if (monitor == NULL)
        return select_fatal_error(false);

    buf_to_buf(backgroundFrmData + CS_WINDOW_WIDTH * CS_WINDOW_BACKGROUND_Y + CS_WINDOW_BACKGROUND_X,
        CS_WINDOW_BACKGROUND_WIDTH,
        CS_WINDOW_BACKGROUND_HEIGHT,
        CS_WINDOW_WIDTH,
        monitor,
        CS_WINDOW_BACKGROUND_WIDTH);

    art_ptr_unlock(backgroundFrmHandle);

    int fid;

    // Setup "Previous" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 122, 0, 0, 0);
    previous_button_up = art_ptr_lock_data(fid, 0, 0, &previous_button_up_key);
    if (previous_button_up == NULL) {
        return select_fatal_error(false);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 123, 0, 0, 0);
    previous_button_down = art_ptr_lock_data(fid, 0, 0, &previous_button_down_key);
    if (previous_button_down == NULL) {
        return select_fatal_error(false);
    }

    previous_button = win_register_button(select_window_id,
        CS_WINDOW_PREVIOUS_BUTTON_X,
        CS_WINDOW_PREVIOUS_BUTTON_Y,
        20,
        18,
        -1,
        -1,
        -1,
        500,
        previous_button_up,
        previous_button_down,
        NULL,
        0);
    if (previous_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(previous_button, gsound_med_butt_press, gsound_med_butt_release);

    // Setup "Next" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 124, 0, 0, 0);
    next_button_up = art_ptr_lock_data(fid, 0, 0, &next_button_up_key);
    if (next_button_up == NULL) {
        return select_fatal_error(false);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 125, 0, 0, 0);
    next_button_down = art_ptr_lock_data(fid, 0, 0, &next_button_down_key);
    if (next_button_down == NULL) {
        return select_fatal_error(false);
    }

    next_button = win_register_button(select_window_id,
        CS_WINDOW_NEXT_BUTTON_X,
        CS_WINDOW_NEXT_BUTTON_Y,
        20,
        18,
        -1,
        -1,
        -1,
        501,
        next_button_up,
        next_button_down,
        NULL,
        0);
    if (next_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(next_button, gsound_med_butt_press, gsound_med_butt_release);

    // Setup "Take" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    take_button_up = art_ptr_lock_data(fid, 0, 0, &take_button_up_key);
    if (take_button_up == NULL) {
        return select_fatal_error(false);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    take_button_down = art_ptr_lock_data(fid, 0, 0, &take_button_down_key);
    if (take_button_down == NULL) {
        return select_fatal_error(false);
    }

    take_button = win_register_button(select_window_id,
        CS_WINDOW_TAKE_BUTTON_X,
        CS_WINDOW_TAKE_BUTTON_Y,
        15,
        16,
        -1,
        -1,
        -1,
        KEY_LOWERCASE_T,
        take_button_up,
        take_button_down,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (take_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(take_button, gsound_red_butt_press, gsound_red_butt_release);

    // Setup "Modify" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    modify_button_up = art_ptr_lock_data(fid, 0, 0, &modify_button_up_key);
    if (modify_button_up == NULL)
        return select_fatal_error(false);

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    modify_button_down = art_ptr_lock_data(fid, 0, 0, &modify_button_down_key);
    if (modify_button_down == NULL) {
        return select_fatal_error(false);
    }

    modify_button = win_register_button(select_window_id,
        CS_WINDOW_MODIFY_BUTTON_X,
        CS_WINDOW_MODIFY_BUTTON_Y,
        15,
        16,
        -1,
        -1,
        -1,
        KEY_LOWERCASE_M,
        modify_button_up,
        modify_button_down,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (modify_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(modify_button, gsound_red_butt_press, gsound_red_butt_release);

    // Setup "Create" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    create_button_up = art_ptr_lock_data(fid, 0, 0, &create_button_up_key);
    if (create_button_up == NULL) {
        return select_fatal_error(false);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    create_button_down = art_ptr_lock_data(fid, 0, 0, &create_button_down_key);
    if (create_button_down == NULL) {
        return select_fatal_error(false);
    }

    create_button = win_register_button(select_window_id,
        CS_WINDOW_CREATE_BUTTON_X,
        CS_WINDOW_CREATE_BUTTON_Y,
        15,
        16,
        -1,
        -1,
        -1,
        KEY_LOWERCASE_C,
        create_button_up,
        create_button_down,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (create_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(create_button, gsound_red_butt_press, gsound_red_butt_release);

    // Setup "Back" button.
    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    back_button_up = art_ptr_lock_data(fid, 0, 0, &back_button_up_key);
    if (back_button_up == NULL) {
        return select_fatal_error(false);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    back_button_down = art_ptr_lock_data(fid, 0, 0, &back_button_down_key);
    if (back_button_down == NULL) {
        return select_fatal_error(false);
    }

    back_button = win_register_button(select_window_id,
        CS_WINDOW_BACK_BUTTON_X,
        CS_WINDOW_BACK_BUTTON_Y,
        15,
        16,
        -1,
        -1,
        -1,
        KEY_ESCAPE,
        back_button_up,
        back_button_down,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (back_button == -1) {
        return select_fatal_error(false);
    }

    win_register_button_sound_func(back_button, gsound_red_butt_press, gsound_red_butt_release);

    premade_index = PREMADE_CHARACTER_NARG;

    win_draw(select_window_id);

    if (!select_update_display()) {
        return select_fatal_error(false);
    }

    return true;
}

// 0x495B64
static void select_exit()
{
    if (select_window_id == -1) {
        return;
    }

    if (previous_button != -1) {
        win_delete_button(previous_button);
        previous_button = -1;
    }

    if (previous_button_down != NULL) {
        art_ptr_unlock(previous_button_down_key);
        previous_button_down_key = NULL;
        previous_button_down = NULL;
    }

    if (previous_button_up != NULL) {
        art_ptr_unlock(previous_button_up_key);
        previous_button_up_key = NULL;
        previous_button_up = NULL;
    }

    if (next_button != -1) {
        win_delete_button(next_button);
        next_button = -1;
    }

    if (next_button_down != NULL) {
        art_ptr_unlock(next_button_down_key);
        next_button_down_key = NULL;
        next_button_down = NULL;
    }

    if (next_button_up != NULL) {
        art_ptr_unlock(next_button_up_key);
        next_button_up_key = NULL;
        next_button_up = NULL;
    }

    if (take_button != -1) {
        win_delete_button(take_button);
        take_button = -1;
    }

    if (take_button_down != NULL) {
        art_ptr_unlock(take_button_down_key);
        take_button_down_key = NULL;
        take_button_down = NULL;
    }

    if (take_button_up != NULL) {
        art_ptr_unlock(take_button_up_key);
        take_button_up_key = NULL;
        take_button_up = NULL;
    }

    if (modify_button != -1) {
        win_delete_button(modify_button);
        modify_button = -1;
    }

    if (modify_button_down != NULL) {
        art_ptr_unlock(modify_button_down_key);
        modify_button_down_key = NULL;
        modify_button_down = NULL;
    }

    if (modify_button_up != NULL) {
        art_ptr_unlock(modify_button_up_key);
        modify_button_up_key = NULL;
        modify_button_up = NULL;
    }

    if (create_button != -1) {
        win_delete_button(create_button);
        create_button = -1;
    }

    if (create_button_down != NULL) {
        art_ptr_unlock(create_button_down_key);
        create_button_down_key = NULL;
        create_button_down = NULL;
    }

    if (create_button_up != NULL) {
        art_ptr_unlock(create_button_up_key);
        create_button_up_key = NULL;
        create_button_up = NULL;
    }

    if (back_button != -1) {
        win_delete_button(back_button);
        back_button = -1;
    }

    if (back_button_down != NULL) {
        art_ptr_unlock(back_button_down_key);
        back_button_down_key = NULL;
        back_button_down = NULL;
    }

    if (back_button_up != NULL) {
        art_ptr_unlock(back_button_up_key);
        back_button_up_key = NULL;
        back_button_up = NULL;
    }

    if (monitor != NULL) {
        mem_free(monitor);
        monitor = NULL;
    }

    win_delete(select_window_id);
    select_window_id = -1;
}

// 0x495DE8
static bool select_update_display()
{
    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s.gcd", premade_characters[premade_index].fileName);
    if (proto_dude_init(path) == -1) {
        debug_printf("\n ** Error in dude init! **\n");
        return false;
    }

    buf_to_buf(monitor,
        CS_WINDOW_BACKGROUND_WIDTH,
        CS_WINDOW_BACKGROUND_HEIGHT,
        CS_WINDOW_BACKGROUND_WIDTH,
        select_window_buffer + CS_WINDOW_WIDTH * CS_WINDOW_BACKGROUND_Y + CS_WINDOW_BACKGROUND_X,
        CS_WINDOW_WIDTH);

    bool success = false;
    if (select_display_portrait()) {
        if (select_display_stats()) {
            success = select_display_bio();
        }
    }

    win_draw_rect(select_window_id, &monitor_rect);

    return success;
}

// 0x495E9C
static bool select_display_portrait()
{
    int old_font;
    int id_width;
    bool success = false;

    CacheEntry* faceFrmHandle;
    int faceFid = art_id(OBJ_TYPE_INTERFACE, premade_characters[premade_index].face, 0, 0, 0);
    Art* frm = art_ptr_lock(faceFid, &faceFrmHandle);
    if (frm != NULL) {
        unsigned char* data = art_frame_data(frm, 0, 0);
        if (data != NULL) {
            int width = art_frame_width(frm, 0, 0);
            int height = art_frame_length(frm, 0, 0);
            int y;

            for (y = 1; y < height; y += 2) {
                memset(data + y * width, 0, width);
            }

            trans_buf_to_buf(data,
                width,
                height,
                width,
                select_window_buffer + CS_WINDOW_WIDTH * (240 - height) + 150 - (width / 2),
                CS_WINDOW_WIDTH);

            old_font = text_curr();
            text_font(101);

            id_width = text_width(premade_characters[premade_index].vid);
            text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * 252 + 150 - id_width / 2,
                premade_characters[premade_index].vid,
                id_width,
                CS_WINDOW_WIDTH,
                colorTable[992]);

            text_font(old_font);

            success = true;
        }
        art_ptr_unlock(faceFrmHandle);
    }

    return success;
}

// 0x49602C
static bool select_display_stats()
{
    char* str;
    char text[260];
    int length;
    int value;
    MessageListItem messageListItem;

    int oldFont = text_curr();
    text_font(101);

    text_char_width(0x20);

    int vh = text_height();
    int y = 40;

    // NAME
    str = object_name(obj_dude);
    strcpy(text, str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_NAME_MID_X - (length / 2), text, 160, CS_WINDOW_WIDTH, colorTable[992]);

    // STRENGTH
    y += vh + vh + vh;

    value = stat_level(obj_dude, STAT_STRENGTH);
    str = stat_name(STAT_STRENGTH);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // PERCEPTION
    y += vh;

    value = stat_level(obj_dude, STAT_PERCEPTION);
    str = stat_name(STAT_PERCEPTION);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // ENDURANCE
    y += vh;

    value = stat_level(obj_dude, STAT_ENDURANCE);
    str = stat_name(STAT_ENDURANCE);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // CHARISMA
    y += vh;

    value = stat_level(obj_dude, STAT_CHARISMA);
    str = stat_name(STAT_CHARISMA);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // INTELLIGENCE
    y += vh;

    value = stat_level(obj_dude, STAT_INTELLIGENCE);
    str = stat_name(STAT_INTELLIGENCE);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // AGILITY
    y += vh;

    value = stat_level(obj_dude, STAT_AGILITY);
    str = stat_name(STAT_AGILITY);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // LUCK
    y += vh;

    value = stat_level(obj_dude, STAT_LUCK);
    str = stat_name(STAT_LUCK);

    snprintf(text, sizeof(text), "%s %02d", str, value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    str = stat_level_description(value);
    snprintf(text, sizeof(text), "  %s", str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_PRIMARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    y += vh; // blank line

    // HIT POINTS
    y += vh;

    messageListItem.num = 16;
    text[0] = '\0';
    if (message_search(&misc_message_file, &messageListItem)) {
        strcpy(text, messageListItem.text);
    }

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    value = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    snprintf(text, sizeof(text), " %d/%d", critter_get_hits(obj_dude), value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // ARMOR CLASS
    y += vh;

    str = stat_name(STAT_ARMOR_CLASS);
    strcpy(text, str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    value = stat_level(obj_dude, STAT_ARMOR_CLASS);
    snprintf(text, sizeof(text), " %d", value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // ACTION POINTS
    y += vh;

    messageListItem.num = 15;
    text[0] = '\0';
    if (message_search(&misc_message_file, &messageListItem)) {
        strcpy(text, messageListItem.text);
    }

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    value = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS);
    snprintf(text, sizeof(text), " %d", value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    // MELEE DAMAGE
    y += vh;

    str = stat_name(STAT_MELEE_DAMAGE);
    strcpy(text, str);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    value = stat_level(obj_dude, STAT_MELEE_DAMAGE);
    snprintf(text, sizeof(text), " %d", value);

    length = text_width(text);
    text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);

    y += vh; // blank line

    // SKILLS
    int skills[DEFAULT_TAGGED_SKILLS];
    skill_get_tags(skills, DEFAULT_TAGGED_SKILLS);

    for (int index = 0; index < DEFAULT_TAGGED_SKILLS; index++) {
        y += vh;

        str = skill_name(skills[index]);
        strcpy(text, str);

        length = text_width(text);
        text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);

        value = skill_level(obj_dude, skills[index]);
        snprintf(text, sizeof(text), " %d%%", value);

        length = text_width(text);
        text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X, text, length, CS_WINDOW_WIDTH, colorTable[992]);
    }

    // TRAITS
    int traits[PC_TRAIT_MAX];
    trait_get(&(traits[0]), &(traits[1]));

    for (int index = 0; index < PC_TRAIT_MAX; index++) {
        y += vh;

        str = trait_name(traits[index]);
        strcpy(text, str);

        length = text_width(text);
        text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_SECONDARY_STAT_MID_X - length, text, length, CS_WINDOW_WIDTH, colorTable[992]);
    }

    text_font(oldFont);

    return true;
}

// 0x496C60
static bool select_display_bio()
{
    int oldFont = text_curr();
    text_font(101);

    // ⚠️ A TRANSLATION'S BIOS BELONG TO THE TRANSLATION (2026-10-06): "destripction postav je v
    // češtině, hraju v eng". The Czech pack's premade\*.bio sat loose in the patch folder, where they
    // answer to the bare path for EVERY language -- the same trap its fonts and interface art fell
    // into. They now live in text\<language>\premade\, asked for first; the bare path is the
    // fallback, which is master.dat's own English. Between the two, master.dat's OWN localisation
    // layout -- it ships premade\french\*.bio and premade\spanish\*.bio, which nothing here ever read.
    char path[COMPAT_MAX_PATH];
    DB_FILE* stream = NULL;
    const char* language = message_language();
    if (language != NULL) {
        snprintf(path, sizeof(path), "text\\%s\\%s.bio", language, premade_characters[premade_index].fileName);
        stream = db_fopen(path, "rt");
        if (stream == NULL) {
            // fileName is "premade\\combat"; the vanilla layout puts the language between the two.
            const char* base = strrchr(premade_characters[premade_index].fileName, '\\');
            base = base != NULL ? base + 1 : premade_characters[premade_index].fileName;
            snprintf(path, sizeof(path), "premade\\%s\\%s.bio", language, base);
            stream = db_fopen(path, "rt");
        }
    }
    if (stream == NULL) {
        snprintf(path, sizeof(path), "%s.bio", premade_characters[premade_index].fileName);
        stream = db_fopen(path, "rt");
    }

    if (stream != NULL) {
        int y = 40;
        int lineHeight = text_height();

        char string[256];
        while (db_fgets(string, 256, stream) && y < 260) {
            text_to_buf(select_window_buffer + CS_WINDOW_WIDTH * y + CS_WINDOW_BIO_X,
                string,
                CS_WINDOW_WIDTH - CS_WINDOW_BIO_X,
                CS_WINDOW_WIDTH,
                colorTable[992]);
            y += lineHeight;
        }

        db_fclose(stream);
    }

    text_font(oldFont);

    return true;
}

// 0x496D4C
static bool select_fatal_error(bool rc)
{
    select_exit();
    return rc;
}

} // namespace fallout
