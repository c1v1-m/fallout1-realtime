#include "game/select.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game/art.h"
#include "game/critter.h"
#include "game/editor.h"
#include "game/game.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
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

// THE PAD ON CHARACTER SELECTION (from Fallout 2 Realtime, 2026-11-14). His layout there, „upravíme char select, lb a rb
// a bude u toho posouvá výběr A bude u Take, X bude u create character, Y bude modify B u back“, and for this game the
// same day „dodáme to i do F1“. Every action of the screen has a button of its own -- LB / RB the previous / next
// character, A TAKE, X CREATE, Y MODIFY, B BACK -- and a press sends the code that action already has here, so the
// loop's own dispatch below stays the only one. It replaces August's focus frame, which the D-pad / stick stepped
// between the buttons and A pressed: with a button for everything there is nothing left to step to.
//
// ⚠️ THE SCREEN OWNS THE PAD (self_nav, from its first input frame to its last). It is not modal and only ever set the
// A-click suppression, so the WORLD's shortcuts ran in it through the shared mouse hook: LB / RB fired the quick slots,
// and Y swapped hands on a bar whose hands still held the init sentinel -- item_get_type read (Object*)-1 and the game
// was gone (Fallout 2's „hra spadla v char selectu po zmáčknutí Y“; this screen had the same hole). The pad layer also
// refuses every world shortcut while no map is up now (gpui_no_world in gamepad_ui.cc), whatever the screen, and the
// hand swap refuses the sentinel (intface_toggle_items).
//
// ⚠️ EVERY BUTTON ARMS ON ITS RELEASE. The A that pressed NEW GAME on the title menu is still down as this opens, and the
// button that closed the editor (MODIFY / CREATE) or the quit box is still down as they come back: a press counts only
// once the button has been seen up.
typedef struct SelPadKey {
    SDL_GameControllerButton button;
    int code; // what the press sends -- the code this screen already acts on
    const char* sound; // the click its on-screen button makes under the mouse, or NULL when the code plays it
    const char* name; // for the trace
} SelPadKey;

static const SelPadKey sel_pad_keys[] = {
    { SDL_CONTROLLER_BUTTON_A, KEY_LOWERCASE_T, "ib1p1xx1", "A" }, // TAKE
    { SDL_CONTROLLER_BUTTON_X, KEY_LOWERCASE_C, "ib1p1xx1", "X" }, // CREATE
    { SDL_CONTROLLER_BUTTON_Y, KEY_LOWERCASE_M, "ib1p1xx1", "Y" }, // MODIFY
    { SDL_CONTROLLER_BUTTON_B, KEY_ESCAPE, "ib1p1xx1", "B" }, // BACK
    // The arrows as the keyboard's arrow keys step them: the very 500 / 501 the arrow buttons send, with the arrow
    // button's own click in front.
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, KEY_ARROW_LEFT, NULL, "LB" },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, KEY_ARROW_RIGHT, NULL, "RB" },
};

#define SEL_PAD_KEY_COUNT ((int)(sizeof(sel_pad_keys) / sizeof(sel_pad_keys[0])))

// Every button counts as held until it is seen up: on entry, and after a screen opened from here comes back.
static void sel_pad_rearm(bool* held)
{
    for (int i = 0; i < SEL_PAD_KEY_COUNT; i++) {
        held[i] = true;
    }
}

// One frame of the pad: the first button down since the last frame sends its code (one action a frame).
static void sel_pad_poll(SDL_GameController* pad, bool* held)
{
    SDL_GameControllerUpdate();

    bool sent = false;
    for (int i = 0; i < SEL_PAD_KEY_COUNT; i++) {
        bool down = SDL_GameControllerGetButton(pad, sel_pad_keys[i].button) != 0;
        if (down && !held[i] && !sent) {
            if (sel_pad_keys[i].sound != NULL) {
                gsound_play_sfx_file(sel_pad_keys[i].sound);
            }
            GNW_add_input_buffer(sel_pad_keys[i].code);
            trace_event(TRACE_INPUT, "charsel-pad", "button=%s code=%d", sel_pad_keys[i].name, sel_pad_keys[i].code);
            sent = true;
        }
        held[i] = down;
    }
}

// THE POINTER: hidden while a pad is in, as on the title menu, Preferences, load / save and the editor -- the pad does
// everything here, and the arrow only sat over the screen („při new game vidím myšítko, teď je zbytečné“). The real mouse
// brings it back while it moves, and SEL_CURSOR_IDLE_MS of rest hides it again. The blank shape, never mouse_hide(),
// which would gate the input hook off; hidden where it stands, never moved (mouse_set_position left a ghost arrow on the
// title menu). Without a pad it stays the arrow.
#define SEL_CURSOR_IDLE_MS 2000

static bool sel_cursor_pad = false; // kept for a pad (hidden at rest); false = nothing kept, hidden on the next tick
static bool sel_cursor_shown = false;
static int sel_cursor_x;
static int sel_cursor_y;
static unsigned int sel_cursor_move;

// Once a frame, and once before the screen fades in.
static void sel_tick_cursor(bool pad)
{
    if (!pad) {
        if (sel_cursor_pad) { // the pad went: the pointer is the mouse's again
            gmouse_set_cursor(MOUSE_CURSOR_ARROW);
            sel_cursor_pad = false;
        }
        return;
    }

    int x;
    int y;
    mouse_get_position(&x, &y);

    if (!sel_cursor_pad) {
        sel_cursor_pad = true;
        sel_cursor_shown = false;
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        trace_event(TRACE_FLOW, "charsel-cursor", "hidden");
    } else if (x != sel_cursor_x || y != sel_cursor_y) {
        sel_cursor_move = get_time();
        if (!sel_cursor_shown) {
            gmouse_set_cursor(MOUSE_CURSOR_ARROW);
            sel_cursor_shown = true;
        }
    } else if (sel_cursor_shown && elapsed_time(sel_cursor_move) >= SEL_CURSOR_IDLE_MS) {
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        sel_cursor_shown = false;
    }

    sel_cursor_x = x;
    sel_cursor_y = y;
}

// On the way out: the shape the pointer had when the screen opened (the title menu's arrow), if a pad took it.
static void sel_cursor_restore(int shape)
{
    if (sel_cursor_pad) {
        gmouse_set_cursor(shape);
        sel_cursor_pad = false;
    }
}

// THE BADGES: with a pad in, each of those buttons is named on the screen by the bar's own badge (intface_pad_face /
// intface_pad_pill: a dark disc or pill, the letters gold). No words: the labels are the art's.
//   - LB and RB: pills on the window art either side of the arrows, clear of the arrows' rects (a press repaints those)
//     and of every line of text; what was under each is kept, so a pad pulled out takes them with it. Their top half
//     lies in the monitor's rows (30..329), which a new character repaints (select_update_display): that paints them
//     again.
//   - A, Y, X and B: ON the red buttons themselves, the whole badge in place of the red dot (sel_red_*, below) -- his
//     pick of three mock-ups in Fallout 2 („Celá značka“; the others were the badge beside the button and the gold
//     letter on the red dot).
typedef struct SelPadBadge {
    int cx;
    int cy;
    const char* key; // two letters: a shoulder's pill
} SelPadBadge;

static const SelPadBadge sel_pad_badges[] = {
    { 270, 329, "LB" }, // the arrows: 292..311 and 318..337
    { 361, 329, "RB" },
};

#define SEL_BADGE_COUNT ((int)(sizeof(sel_pad_badges) / sizeof(sel_pad_badges[0])))

// The box kept under each badge: as wide as the pill (17), as tall as the disc (15).
#define SEL_BADGE_HALF_W 8
#define SEL_BADGE_HALF_H 7
#define SEL_BADGE_W (2 * SEL_BADGE_HALF_W + 1)
#define SEL_BADGE_H (2 * SEL_BADGE_HALF_H + 1)

static unsigned char sel_badge_under[SEL_BADGE_COUNT][SEL_BADGE_W * SEL_BADGE_H];
static bool sel_badge_under_kept = false;
static bool sel_badges_shown = false;

// Once, on the bare art, before any badge is drawn.
static void sel_badges_keep_under()
{
    for (int i = 0; i < SEL_BADGE_COUNT; i++) {
        const SelPadBadge* badge = &(sel_pad_badges[i]);
        const unsigned char* src = select_window_buffer
            + CS_WINDOW_WIDTH * (badge->cy - SEL_BADGE_HALF_H) + badge->cx - SEL_BADGE_HALF_W;
        for (int y = 0; y < SEL_BADGE_H; y++) {
            memcpy(sel_badge_under[i] + SEL_BADGE_W * y, src + CS_WINDOW_WIDTH * y, SEL_BADGE_W);
        }
    }
    sel_badge_under_kept = true;
    sel_badges_shown = false;
}

// The badges with a pad in, the art they covered without one -- each put on the screen at once, because this window is
// only ever drawn in parts (the monitor's rect, a button's) and the badges stick out of the monitor's.
static void sel_badges_paint(bool pad)
{
    if (select_window_buffer == NULL || !sel_badge_under_kept) {
        return;
    }

    for (int i = 0; i < SEL_BADGE_COUNT; i++) {
        const SelPadBadge* badge = &(sel_pad_badges[i]);
        Rect rect;
        rect.ulx = badge->cx - SEL_BADGE_HALF_W;
        rect.uly = badge->cy - SEL_BADGE_HALF_H;
        rect.lrx = rect.ulx + SEL_BADGE_W - 1;
        rect.lry = rect.uly + SEL_BADGE_H - 1;
        if (pad) {
            intface_pad_pill(select_window_buffer, CS_WINDOW_WIDTH, CS_WINDOW_WIDTH, CS_WINDOW_HEIGHT,
                badge->cx, badge->cy, badge->key);
        } else if (sel_badges_shown) {
            unsigned char* dest = select_window_buffer + CS_WINDOW_WIDTH * rect.uly + rect.ulx;
            for (int y = 0; y < SEL_BADGE_H; y++) {
                memcpy(dest + CS_WINDOW_WIDTH * y, sel_badge_under[i] + SEL_BADGE_W * y, SEL_BADGE_W);
            }
        } else {
            continue;
        }
        win_draw_rect(select_window_id, &rect);
    }
    sel_badges_shown = pad;
}

// THE RED BUTTONS WEAR THEIR PAD BUTTON (see THE BADGES above). Drawn into COPIES OF THE BUTTONS' OWN IMAGES, not on the
// window: a GNW button blits its image into the window buffer at every window refresh (GNW_button_refresh), so a badge
// painted on the art under a red button is simply covered by it -- Fallout 2's first try. Each copy is the whole 15x16
// tile made opaque: the bare art that lies under the button, the button's own image over it (its transparent pixels
// kept), then the badge. Opaque, because the buttons are BUTTON_FLAG_TRANSPARENT and blit only their non-zero pixels: a
// copy with holes would leave whatever the previous image drew there. The pressed copy is the same badge over the
// pressed art, so a mouse press on a badged button changes nothing a player could misread (the press still clicks).
// Without a pad the buttons get the art's own images back, over the bare art put back first.
#define SEL_RED_W 15
#define SEL_RED_H 16

enum {
    SEL_RED_TAKE,
    SEL_RED_MODIFY,
    SEL_RED_CREATE,
    SEL_RED_BACK,
    SEL_RED_COUNT,
};

static const char sel_red_keys[SEL_RED_COUNT] = { 'A', 'Y', 'X', 'B' };
static const int sel_red_at[SEL_RED_COUNT][2] = {
    { CS_WINDOW_TAKE_BUTTON_X, CS_WINDOW_TAKE_BUTTON_Y },
    { CS_WINDOW_MODIFY_BUTTON_X, CS_WINDOW_MODIFY_BUTTON_Y },
    { CS_WINDOW_CREATE_BUTTON_X, CS_WINDOW_CREATE_BUTTON_Y },
    { CS_WINDOW_BACK_BUTTON_X, CS_WINDOW_BACK_BUTTON_Y },
};
static unsigned char sel_red_under[SEL_RED_COUNT][SEL_RED_W * SEL_RED_H]; // the bare art under each red button
static unsigned char sel_red_badged[SEL_RED_COUNT][2][SEL_RED_W * SEL_RED_H]; // [up, down], opaque
static bool sel_red_ready = false;
static bool sel_red_shown = false;

static int sel_red_button(int i)
{
    switch (i) {
    case SEL_RED_TAKE:
        return take_button;
    case SEL_RED_MODIFY:
        return modify_button;
    case SEL_RED_CREATE:
        return create_button;
    case SEL_RED_BACK:
        return back_button;
    }
    return -1;
}

static unsigned char* sel_red_art(int i, bool down)
{
    switch (i) {
    case SEL_RED_TAKE:
        return down ? take_button_down : take_button_up;
    case SEL_RED_MODIFY:
        return down ? modify_button_down : modify_button_up;
    case SEL_RED_CREATE:
        return down ? create_button_down : create_button_up;
    case SEL_RED_BACK:
        return down ? back_button_down : back_button_up;
    }
    return NULL;
}

// Is the red button's art (interface 8 up, 9 down) the buttons' own 15x16? Art of another size -- a mod's -- gets no
// badge, and the plain buttons stay.
static bool sel_red_art_fits(int index)
{
    CacheEntry* key;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, index, 0, 0, 0), &key);
    if (art == NULL) {
        return false;
    }
    bool fits = art_frame_width(art, 0, 0) == SEL_RED_W && art_frame_length(art, 0, 0) == SEL_RED_H;
    art_ptr_unlock(key);
    return fits;
}

// Once, on the bare art, before the buttons exist (they draw themselves into the window when made).
static void sel_red_keep_under()
{
    for (int i = 0; i < SEL_RED_COUNT; i++) {
        const unsigned char* src = select_window_buffer + CS_WINDOW_WIDTH * sel_red_at[i][1] + sel_red_at[i][0];
        for (int y = 0; y < SEL_RED_H; y++) {
            memcpy(sel_red_under[i] + SEL_RED_W * y, src + CS_WINDOW_WIDTH * y, SEL_RED_W);
        }
    }
    sel_red_ready = false;
    sel_red_shown = false;
}

// Once the four buttons and their art exist: the badged tiles.
static void sel_red_make()
{
    sel_red_ready = false;
    if (!sel_red_art_fits(8) || !sel_red_art_fits(9)) {
        return;
    }
    for (int i = 0; i < SEL_RED_COUNT; i++) {
        for (int p = 0; p < 2; p++) {
            const unsigned char* img = sel_red_art(i, p != 0);
            if (img == NULL) {
                return;
            }
            unsigned char* tile = sel_red_badged[i][p];
            memcpy(tile, sel_red_under[i], SEL_RED_W * SEL_RED_H);
            for (int k = 0; k < SEL_RED_W * SEL_RED_H; k++) {
                if (img[k] != 0) {
                    tile[k] = img[k];
                }
            }
            intface_pad_face(tile, SEL_RED_W, SEL_RED_W, SEL_RED_H, SEL_RED_W / 2, SEL_RED_H / 2, sel_red_keys[i]);
        }
    }
    sel_red_ready = true;
}

// The badged tiles with a pad in, the art's own images without one; nothing when that is already so. The button swap
// puts each on the screen itself.
static void sel_red_show(bool pad)
{
    if (!sel_red_ready || pad == sel_red_shown) {
        return;
    }
    for (int i = 0; i < SEL_RED_COUNT; i++) {
        int btn = sel_red_button(i);
        if (btn == -1) {
            continue;
        }
        // The bare art back under the button first: a transparent image only covers its own non-zero pixels.
        unsigned char* dest = select_window_buffer + CS_WINDOW_WIDTH * sel_red_at[i][1] + sel_red_at[i][0];
        for (int y = 0; y < SEL_RED_H; y++) {
            memcpy(dest + CS_WINDOW_WIDTH * y, sel_red_under[i] + SEL_RED_W * y, SEL_RED_W);
        }
        if (pad) {
            win_register_button_image(btn, sel_red_badged[i][0], sel_red_badged[i][1], NULL, true);
        } else {
            win_register_button_image(btn, sel_red_art(i, false), sel_red_art(i, true), NULL, true);
        }
    }
    sel_red_shown = pad;
    trace_event(TRACE_FLOW, "charsel-red", "badged=%d", pad ? 1 : 0);
}

// 0x495260
int select_character()
{
    if (!select_init()) {
        return 0;
    }

    // The pad is this screen's (see sel_pad_keys) -- with no pad as well, which costs nothing, as on the title menu.
    gamepad_ui_set_self_nav(true);

    bool cursorWasHidden = mouse_hidden();
    if (cursorWasHidden) {
        mouse_show();
    }

    // Same reason as the character editor: this screen injects its own button codes and must not
    // inherit anyone else's (see the note in editor_design).
    flush_input_buffer();

    // A pad hides the pointer before the screen fades in (sel_tick_cursor); the shape it had comes back on the way out.
    int cursorWas = gmouse_get_cursor();
    sel_tick_cursor(gamepad_ui_get_controller() != NULL);

    loadColorTable("color.pal");
    palette_fade_to(cmap);

    bool padHeld[SEL_PAD_KEY_COUNT];
    sel_pad_rearm(padHeld);
    trace_event(TRACE_FLOW, "charsel", "open premades=%d pad=%d", premade_total,
        gamepad_ui_get_controller() != NULL ? 1 : 0);

    int rc = 0;
    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        if (game_user_wants_to_quit != 0) {
            break;
        }

        // Re-fetched every frame, never latched on entry: gamepad_ui opens the controller LAZILY and its tick does not
        // run inside this loop, and one can come or go while the screen is up -- its badges with it. Read before the
        // input, so a press is answered this frame.
        SDL_GameController* pad = gamepad_ui_get_controller();
        if (pad != NULL) {
            sel_pad_poll(pad, padHeld);
        }
        if ((pad != NULL) != sel_badges_shown) {
            sel_badges_paint(pad != NULL);
        }
        sel_red_show(pad != NULL); // the red buttons' badges: nothing unless the pad came or went
        sel_tick_cursor(pad != NULL);

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
            // The editor wants the right stick's pointer, which self_nav takes away (it reaches PRINT / DONE / OPTIONS
            // with it); the pad is this screen's again when it comes back, every button once let go.
            gamepad_ui_set_self_nav(false);
            if (editor_design(1) == 0) {
                rc = 2;
                done = true;
            } else {
                select_update_display();
            }
            gamepad_ui_set_self_nav(true);
            sel_pad_rearm(padHeld);
            sel_cursor_pad = false; // the editor put the arrow back as it closed: hidden again on the next frame

            break;
        case KEY_UPPERCASE_M:
        case KEY_LOWERCASE_M:
            gamepad_ui_set_self_nav(false); // as CREATE's, above
            if (!editor_design(1)) {
                rc = 2;
                done = true;
            } else {
                select_update_display();
            }
            gamepad_ui_set_self_nav(true);
            sel_pad_rearm(padHeld);
            sel_cursor_pad = false; // as CREATE's

            break;
        case KEY_UPPERCASE_T:
        case KEY_LOWERCASE_T:
            rc = 2;
            done = true;

            break;
        case KEY_F10:
            game_quit_with_confirm();
            sel_pad_rearm(padHeld); // its A / B are still down as it closes
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

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    flush_input_buffer();
    // Every way out comes through here. The button that closed the screen is still down, and the title menu or the game
    // after it must not read it as a fresh press (the pad layer re-primes its edges on the epoch).
    gamepad_ui_set_self_nav(false);
    gamepad_ui_bump_input_epoch();
    trace_event(TRACE_FLOW, "charsel", "close rc=%d", rc);

    palette_fade_to(black_palette);
    select_exit();
    sel_cursor_restore(cursorWas); // after the fade, so the arrow does not show on the screen going out

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

    // Realtime build: the bare art under the pad's badges and under the red buttons, before anything is drawn over it.
    sel_badges_keep_under();
    sel_red_keep_under();

    monitor =(unsigned char*)mem_malloc(CS_WINDOW_BACKGROUND_WIDTH * CS_WINDOW_BACKGROUND_HEIGHT);
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

    // Realtime build: the red buttons' badged tiles (sel_red_show puts them on with a pad in).
    sel_red_make();

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

    // Realtime build: the next opening keeps its own bare art and makes its own badged tiles.
    sel_badge_under_kept = false;
    sel_badges_shown = false;
    sel_red_ready = false;
    sel_red_shown = false;
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

    // Realtime build: the badges the monitor's blit above just painted over (sel_pad_badges), and on the first display
    // the red buttons' (the draw below repaints every button in its rect from its image anyway).
    bool pad = gamepad_ui_get_controller() != NULL;
    sel_badges_paint(pad);
    sel_red_show(pad);
    trace_event(TRACE_FLOW, "charsel-show", "premade=%d name=%s", premade_index, object_name(obj_dude));

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
