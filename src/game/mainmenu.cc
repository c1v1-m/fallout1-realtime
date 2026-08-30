#include "game/mainmenu.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "game/art.h"
#include "game/game.h"
#include "game/gamepad_ui.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/options.h"
#include "game/palette.h"
#include "game/version.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define MAIN_MENU_WINDOW_WIDTH 640
#define MAIN_MENU_WINDOW_HEIGHT 480

typedef enum MainMenuButton {
    MAIN_MENU_BUTTON_INTRO,
    MAIN_MENU_BUTTON_CONTINUE,
    MAIN_MENU_BUTTON_NEW_GAME,
    MAIN_MENU_BUTTON_LOAD_GAME,
    MAIN_MENU_BUTTON_CREDITS,
    MAIN_MENU_BUTTON_EXIT,
    MAIN_MENU_BUTTON_COUNT,
} MainMenuButton;

static int main_menu_fatal_error();
static void main_menu_extend_plate();
static const char* main_menu_label(int index);
static void main_menu_play_sound(const char* fileName);
static void main_menu_draw_option(int index, int color);
static void main_menu_gamepad_highlight(int sel);
static void main_menu_tick_cursor(bool* shown, int* lastX, int* lastY, unsigned int* lastMove);

// 0x505A84
static int main_window = -1;

// 0x505A88
static unsigned char* main_window_buf = NULL;

// 0x505A8C
static unsigned char* background_data = NULL;

// 0x505A90
static unsigned char* button_up_data = NULL;

// 0x505A94
static unsigned char* button_down_data = NULL;

// 0x505A98
bool in_main_menu = false;

// 0x505A9C
static bool main_menu_created = false;

// 0x505AA0
static unsigned int main_menu_timeout = 120000;

// 0x505AA4
static int button_values[MAIN_MENU_BUTTON_COUNT] = {
    KEY_LOWERCASE_I,
    KEY_LOWERCASE_O, // c is taken by CREDITS, so cOntinue
    KEY_LOWERCASE_N,
    KEY_LOWERCASE_L,
    KEY_LOWERCASE_C,
    KEY_LOWERCASE_E,
};

// 0x505AB8
static int return_values[MAIN_MENU_BUTTON_COUNT] = {
    MAIN_MENU_INTRO,
    MAIN_MENU_CONTINUE,
    MAIN_MENU_NEW_GAME,
    MAIN_MENU_LOAD_GAME,
    MAIN_MENU_CREDITS,
    MAIN_MENU_EXIT,
};

// 0x612DC8
static int buttons[MAIN_MENU_BUTTON_COUNT];

// 0x612DC0
static bool main_menu_is_hidden;

// 0x612DC4
static CacheEntry* button_up_key;

// 0x612DDC
static CacheEntry* button_down_key;

// 0x612DE0
static CacheEntry* background_key;

// 0x472F80
int main_menu_create()
{
    int fid;
    MessageListItem msg;
    int len;

    if (main_menu_created) {
        return 0;
    }

    loadColorTable("color.pal");

    int mainMenuWindowX = (screenGetWidth() - MAIN_MENU_WINDOW_WIDTH) / 2;
    int mainMenuWindowY = (screenGetHeight() - MAIN_MENU_WINDOW_HEIGHT) / 2;
    main_window = win_add(mainMenuWindowX,
        mainMenuWindowY,
        MAIN_MENU_WINDOW_WIDTH,
        MAIN_MENU_WINDOW_HEIGHT,
        0,
        WINDOW_HIDDEN | WINDOW_MOVE_ON_TOP);
    if (main_window == -1) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }

    main_window_buf = win_get_buf(main_window);

    // mainmenu.frm
    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 140, 0, 0, 0);
    background_data = art_ptr_lock_data(backgroundFid, 0, 0, &background_key);
    if (background_data == NULL) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }

    buf_to_buf(background_data, MAIN_MENU_WINDOW_WIDTH,
        MAIN_MENU_WINDOW_HEIGHT,
        MAIN_MENU_WINDOW_WIDTH,
        main_window_buf,
        MAIN_MENU_WINDOW_WIDTH);
    art_ptr_unlock(background_key);

    // Six options need a six-row plate; the art only draws five. See the block above.
    main_menu_extend_plate();

    int oldFont = text_curr();
    text_font(100);

    // Copyright.
    msg.num = 14;
    if (message_search(&misc_message_file, &msg)) {
        win_print(main_window, msg.text, 0, 15, 460, colorTable[21204] | 0x4000000 | 0x2000000);
    }

    // Version.
    char version[VERSION_MAX];
    getverstr(version, sizeof(version));
    len = text_width(version);
    win_print(main_window, version, 0, 615 - len, 460, colorTable[21204] | 0x4000000 | 0x2000000);

    // menuup.frm
    fid = art_id(OBJ_TYPE_INTERFACE, 299, 0, 0, 0);
    button_up_data = art_ptr_lock_data(fid, 0, 0, &button_up_key);
    if (button_up_data == NULL) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }

    // menudown.frm
    fid = art_id(OBJ_TYPE_INTERFACE, 300, 0, 0, 0);
    button_down_data = art_ptr_lock_data(fid, 0, 0, &button_down_key);
    if (button_down_data == NULL) {
        // NOTE: Uninline.
        return main_menu_fatal_error();
    }

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        buttons[index] = -1;
    }

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        buttons[index] = win_register_button(main_window,
            425,
            index * 42 - index + 45,
            26,
            26,
            -1,
            -1,
            1111,
            button_values[index],
            button_up_data,
            button_down_data,
            NULL,
            BUTTON_FLAG_TRANSPARENT);
        if (buttons[index] == -1) {
            // NOTE: Uninline.
            return main_menu_fatal_error();
        }

        win_register_button_mask(buttons[index], button_up_data);
    }

    text_font(104);

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        const char* label = main_menu_label(index);
        if (label != NULL) {
            len = text_width((char*)label);
            text_to_buf(main_window_buf + MAIN_MENU_WINDOW_WIDTH * (42 * index - index + 46) + 520 - (len / 2),
                (char*)label,
                MAIN_MENU_WINDOW_WIDTH - (520 - (len / 2)) - 1,
                MAIN_MENU_WINDOW_WIDTH,
                colorTable[21091]);
        }
    }

    text_font(oldFont);

    main_menu_created = true;
    main_menu_is_hidden = true;

    return 0;
}

// 0x473298
void main_menu_destroy()
{
    if (!main_menu_created) {
        return;
    }

    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        // FIXME: Why it tries to free only invalid buttons?
        if (buttons[index] == -1) {
            win_delete_button(buttons[index]);
        }
    }

    if (button_down_data) {
        art_ptr_unlock(button_down_key);
        button_down_key = NULL;
        button_down_data = NULL;
    }

    if (button_up_data) {
        art_ptr_unlock(button_up_key);
        button_up_key = NULL;
        button_up_data = NULL;
    }

    if (main_window != -1) {
        win_delete(main_window);
    }

    main_menu_created = false;
}

// 0x473330
void main_menu_hide(bool animate)
{
    if (!main_menu_created) {
        return;
    }

    if (main_menu_is_hidden) {
        return;
    }

    soundUpdate();

    if (animate) {
        palette_fade_to(black_palette);
        soundUpdate();
    }

    win_hide(main_window);

    main_menu_is_hidden = true;
}

// 0x473378
void main_menu_show(bool animate)
{
    if (!main_menu_created) {
        return;
    }

    if (!main_menu_is_hidden) {
        return;
    }

    win_show(main_window);

    if (animate) {
        loadColorTable("color.pal");
        palette_fade_to(cmap);
    }

    main_menu_is_hidden = false;
}

// 0x4733BC
int main_menu_is_shown()
{
    return main_menu_created ? main_menu_is_hidden == 0 : 0;
}

// 0x4733D8
int main_menu_is_enabled()
{
    return 1;
}

// 0x4733E0
void main_menu_set_timeout(unsigned int timeout)
{
    main_menu_timeout = 60000 * timeout;
}

// 0x473400
unsigned int main_menu_get_timeout()
{
    return main_menu_timeout / 1000 / 60;
}

// 0x47341C
int main_menu_loop()
{
    in_main_menu = true;

    // Suppress the shared gamepad LEFT-STICK cursor snapping for the WHOLE title menu, right now --
    // before the first get_input() pumps the mouse hook. This menu drives its own D-pad/left-stick
    // highlight nav below, so the snap (which moves the mouse cursor to buttons + plays the button
    // hover sound) must never run here: otherwise pushing the stick showed the cursor and played an
    // "action" sound instead of cleanly moving the highlight. Was previously set only inside an
    // `if (pad != NULL)` block (and later lazily), so if the controller wasn't ready at entry the
    // snap ran unsuppressed the whole time. Set unconditionally; harmless with no controller (the
    // hook early-returns when no pad) and cleared at the end.
    gamepad_ui_set_self_nav(true);

    bool oldCursorIsHidden = mouse_hidden();
    if (oldCursorIsHidden) {
        mouse_show();
    }

    // Gamepad menu navigation: highlight the selected option and move it with the
    // D-pad / left stick, confirm with A. Only when a controller is present, so
    // keyboard/mouse keeps the vanilla (no-highlight) look. The world cursor is
    // hidden in-game, so this is how the pad drives the main menu without a cursor.
    SDL_GameController* pad = gamepad_ui_get_controller();
    int selection = MAIN_MENU_BUTTON_NEW_GAME;
    bool padPrevUp = false;
    bool padPrevDown = false;
    bool padPrevA = false;
    // Cursor auto-hide (controller only): start hidden, reveal on movement. Also tell
    // the shared gamepad hook to stop left-stick-snapping the cursor to buttons while
    // in_main_menu -- otherwise it fights the highlight navigation (jumpy cursor).
    bool cursorShown = true;
    int cursorX = 0;
    int cursorY = 0;
    unsigned int cursorMove = get_time();
    // The controller "view" (self-nav highlight + hidden cursor) is set up LAZILY, the first loop
    // frame a pad is actually present -- gamepad_ui_get_controller() is re-polled every iteration
    // below. Previously pad was latched once here at entry: if a controller wasn't ready yet (a
    // wireless pad still waking, or connected during the intro movies), pad stayed NULL for the
    // whole menu and the D-pad did nothing ("D-pad doesn't work in the main menu"). Re-polling picks
    // it up whenever it appears.
    bool padViewInit = false;

    unsigned int tick = get_time();

    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();

        pad = gamepad_ui_get_controller();

        if (pad != NULL) {
            // Re-assert every frame (like the options screen): keeps the shared gamepad hook's
            // left-stick cursor snap + A/Y-as-click suppressed for the whole menu, so pushing the
            // stick/D-pad drives the highlight below instead of creeping/teleporting the mouse
            // cursor (which left a ghost = "two cursors" on screen).
            gamepad_ui_set_self_nav(true);

            if (!padViewInit) {
                padViewInit = true;
                main_menu_gamepad_highlight(selection);
                win_draw(main_window);
                // Hide the pointer (blank shape, not mouse_hide() -- that gates the input hook).
                // Do NOT teleport the mouse: mouse_set_position() left the old cursor sprite behind
                // as a ghost because nothing redrew under it, so two arrows appeared. Just record
                // where it is so main_menu_tick_cursor sees no motion and keeps it hidden.
                mouse_get_position(&cursorX, &cursorY);
                gmouse_set_cursor(MOUSE_CURSOR_NONE);
                cursorShown = false;
            }

            SDL_GameControllerUpdate();

            bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
                || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
            bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
                || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
            bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;

            if (upNow && !padPrevUp) {
                selection = (selection + MAIN_MENU_BUTTON_COUNT - 1) % MAIN_MENU_BUTTON_COUNT;
                main_menu_gamepad_highlight(selection);
                win_draw(main_window);
                // NOTE: Uninline.
                main_menu_play_sound("nmselec0");
            } else if (downNow && !padPrevDown) {
                selection = (selection + 1) % MAIN_MENU_BUTTON_COUNT;
                main_menu_gamepad_highlight(selection);
                win_draw(main_window);
                // NOTE: Uninline.
                main_menu_play_sound("nmselec0");
            }

            if (aNow && !padPrevA) {
                // NOTE: Uninline.
                main_menu_play_sound("nmselec1");
                rc = return_values[selection];
            }

            padPrevUp = upNow;
            padPrevDown = downNow;
            padPrevA = aNow;

            main_menu_tick_cursor(&cursorShown, &cursorX, &cursorY, &cursorMove);
        }

        for (int buttonIndex = 0; buttonIndex < MAIN_MENU_BUTTON_COUNT; buttonIndex++) {
            if (keyCode == button_values[buttonIndex] || keyCode == toupper(button_values[buttonIndex])) {
                // NOTE: Uninline.
                main_menu_play_sound("nmselec1");

                rc = return_values[buttonIndex];

                if (buttonIndex == MAIN_MENU_BUTTON_CREDITS && (keys[SDL_SCANCODE_RSHIFT] != KEY_STATE_UP || keys[SDL_SCANCODE_LSHIFT] != KEY_STATE_UP)) {
                    rc = MAIN_MENU_QUOTES;
                }

                break;
            }
        }

        if (rc == -1) {
            if (keyCode == KEY_CTRL_R) {
                rc = MAIN_MENU_SELFRUN;
                continue;
            } else if (keyCode == KEY_PLUS || keyCode == KEY_EQUAL) {
                IncGamma();
            } else if (keyCode == KEY_MINUS || keyCode == KEY_UNDERSCORE) {
                DecGamma();
            } else if (keyCode == KEY_UPPERCASE_D || keyCode == KEY_LOWERCASE_D) {
                rc = MAIN_MENU_SCREENSAVER;
                continue;
            } else if (keyCode == 1111) {
                if (!(mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT)) {
                    // NOTE: Uninline.
                    main_menu_play_sound("nmselec0");
                }
                continue;
            }
        }

        if (keyCode == KEY_ESCAPE || game_user_wants_to_quit == 3) {
            rc = MAIN_MENU_EXIT;

            // NOTE: Uninline.
            main_menu_play_sound("nmselec1");
            break;
        } else if (game_user_wants_to_quit == 2) {
            game_user_wants_to_quit = 0;
        } else {
            if (elapsed_time(tick) >= main_menu_timeout) {
                rc = MAIN_MENU_TIMEOUT;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    // Always clear self-nav (set unconditionally at entry) so it never leaks into gameplay and
    // suppresses the in-game gamepad hook. The cursor restore only matters if we hid it (pad path).
    gamepad_ui_set_self_nav(false);
    if (pad != NULL) {
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    }

    if (oldCursorIsHidden) {
        mouse_hide();
    }

    in_main_menu = false;

    return rc;
}

// 0x4735B8
static int main_menu_fatal_error()
{
    main_menu_destroy();

    return -1;
}

// 0x4735C4
static void main_menu_play_sound(const char* fileName)
{
    gsound_play_sfx_file(fileName);
}

// Redraw a single menu option's label in the given color. The labels are solid
// (non-antialiased) glyph text at fixed positions, so redrawing the same string
// at the same spot in a new color cleanly overwrites the old glyphs — this is how
// the gamepad selection highlight moves without touching the background.

// ⚠️ THE PLATE ONLY HAS FIVE SLOTS. mainmenu.frm draws the metal option plate with its divider
// lines baked in: measured off the art, they sit at y 74 / 115 / 156 / 197 / 238 -- a 41 px step
// -- and the frame's bottom edge follows immediately after. Vanilla's five buttons fill it
// exactly, so the sixth (CONTINUE) would have hung in mid-air below the panel.
//
// So GROW the plate by one row, in the WINDOW buffer: lift the real bottom border out of the
// way, duplicate one clean interior band (a row plus its divider and rivets) into the space it
// occupied, then put the border back underneath. The plate texture is uniform and the rivets
// repeat per row, so the seam does not read.
//
// ⚠️ In the WINDOW, never in background_data -- that is the shared art cache and writing to it
// would corrupt the plate for anything else that locks the same fid.
#define MENU_PLATE_X0 398
#define MENU_PLATE_X1 620
#define MENU_PLATE_ROW_H 41
#define MENU_PLATE_BAND_Y 159 // a clean interior row (its divider included) to clone
#define MENU_PLATE_BOTTOM_Y 238
#define MENU_PLATE_BOTTOM_H 14


// ⚠️ THE LABEL IS NO LONGER `9 + index`. misc.msg ids 9..13 are INTRO / NEW GAME / LOAD GAME /
// CREDITS / EXIT in exactly that order, so inserting CONTINUE at position 1 would have made every
// row below it draw its neighbour's name. The mapping is explicit now.
//
// ⚠️ CONTINUE's text is HARDCODED rather than given a new msg id. misc.msg lives inside
// MASTER.DAT: adding an id there would mean shipping a loose copy of the WHOLE file beside it,
// which then fights every localisation and every patch. Same call, same reason, as the Pip-Boy
// CONTROLS page.
static const int menu_label_msg[MAIN_MENU_BUTTON_COUNT] = { 9, -1, 10, 11, 12, 13 };
static const char* menu_label_literal[MAIN_MENU_BUTTON_COUNT] = { NULL, "CONTINUE", NULL, NULL, NULL, NULL };

// The text for one row, or NULL if the message is missing. The returned pointer is the message
// list's own storage (the same thing getmsg hands out), so it outlives this call.
static const char* main_menu_label(int index)
{
    if (index < 0 || index >= MAIN_MENU_BUTTON_COUNT) {
        return NULL;
    }
    if (menu_label_literal[index] != NULL) {
        return menu_label_literal[index];
    }

    MessageListItem msg;
    msg.num = menu_label_msg[index];
    if (message_search(&misc_message_file, &msg)) {
        return msg.text;
    }
    return NULL;
}

static void main_menu_extend_plate()
{
    const int w = MENU_PLATE_X1 - MENU_PLATE_X0;
    unsigned char bottom[(MENU_PLATE_X1 - MENU_PLATE_X0) * MENU_PLATE_BOTTOM_H];

    // 1. Stash the plate's real bottom border.
    for (int y = 0; y < MENU_PLATE_BOTTOM_H; y++) {
        memcpy(&bottom[y * w],
            main_window_buf + MAIN_MENU_WINDOW_WIDTH * (MENU_PLATE_BOTTOM_Y + y) + MENU_PLATE_X0,
            w);
    }

    // 2. Clone one interior band over it, which is the new sixth row.
    for (int y = 0; y < MENU_PLATE_ROW_H; y++) {
        memcpy(main_window_buf + MAIN_MENU_WINDOW_WIDTH * (MENU_PLATE_BOTTOM_Y + y) + MENU_PLATE_X0,
            main_window_buf + MAIN_MENU_WINDOW_WIDTH * (MENU_PLATE_BAND_Y + y) + MENU_PLATE_X0,
            w);
    }

    // 3. Put the border back, now one row lower.
    for (int y = 0; y < MENU_PLATE_BOTTOM_H; y++) {
        memcpy(main_window_buf + MAIN_MENU_WINDOW_WIDTH * (MENU_PLATE_BOTTOM_Y + MENU_PLATE_ROW_H + y) + MENU_PLATE_X0,
            &bottom[y * w],
            w);
    }
}

static void main_menu_draw_option(int index, int color)
{
    const char* label = main_menu_label(index);
    if (label != NULL) {
        int oldFont = text_curr();
        text_font(104);
        int len = text_width((char*)label);
        text_to_buf(main_window_buf + MAIN_MENU_WINDOW_WIDTH * (42 * index - index + 46) + 520 - (len / 2),
            (char*)label,
            MAIN_MENU_WINDOW_WIDTH - (520 - (len / 2)) - 1,
            MAIN_MENU_WINDOW_WIDTH,
            color);
        text_font(oldFont);
    }
}

// Move the controller selection highlight to `sel`. Two visible cues, because the text
// re-color alone was NOT showing on screen (the input + nav worked -- selection changed and
// the blip played -- but nothing appeared to move, so the player couldn't tell where they
// were and mis-selected): (1) recolor every label, the chosen one red; (2) show the chosen
// row's button in its PRESSED (menudown) image, the rest un-pressed. The button-image swap
// is the same proven-visible technique the options screen uses -- it goes through the button
// system's own draw, so it can't be silently overwritten like the raw text_to_buf was.
static void main_menu_gamepad_highlight(int sel)
{
    for (int index = 0; index < MAIN_MENU_BUTTON_COUNT; index++) {
        main_menu_draw_option(index, index == sel ? colorTable[32074] : colorTable[21091]);
        if (buttons[index] != -1) {
            win_register_button_image(buttons[index],
                index == sel ? button_down_data : button_up_data,
                button_down_data,
                NULL,
                true);
        }
    }
}

// Auto-hide the menu cursor (controller only): hidden by default, revealed while the
// right stick / real mouse moves it, re-hidden after MAIN_MENU_CURSOR_IDLE_MS of no
// motion. Blank MOUSE_CURSOR_NONE shape, not mouse_hide() (which would gate the
// mouse_info hook off and stop the right stick from ever revealing it). Navigation is
// by the highlighted option + A, so the cursor isn't needed.
#define MAIN_MENU_CURSOR_IDLE_MS 2000

static void main_menu_tick_cursor(bool* shown, int* lastX, int* lastY, unsigned int* lastMove)
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
    } else if (*shown && elapsed_time(*lastMove) >= MAIN_MENU_CURSOR_IDLE_MS) {
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        *shown = false;
    }

    *lastX = x;
    *lastY = y;
}

} // namespace fallout
