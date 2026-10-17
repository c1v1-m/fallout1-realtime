#include "game/inventry.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include <SDL.h>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/bmpdlog.h"
#include "game/combat.h"
#include "game/combat_rt.h"
#include "game/combatai.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gdialog.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/item.h"
#include "game/light.h"
#include "game/map.h"
#include "game/message.h"
#include "game/object.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/reaction.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "int/dialog.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define INVENTORY_WINDOW_X 80
#define INVENTORY_WINDOW_Y 0

#define INVENTORY_TRADE_WINDOW_X 80
#define INVENTORY_TRADE_WINDOW_Y 290
#define INVENTORY_TRADE_WINDOW_WIDTH 480
#define INVENTORY_TRADE_WINDOW_HEIGHT 180

#define INVENTORY_LARGE_SLOT_WIDTH 90
#define INVENTORY_LARGE_SLOT_HEIGHT 61

#define INVENTORY_SLOT_WIDTH 64
#define INVENTORY_SLOT_HEIGHT 48

#define INVENTORY_LEFT_HAND_SLOT_X 154
#define INVENTORY_LEFT_HAND_SLOT_Y 286
#define INVENTORY_LEFT_HAND_SLOT_MAX_X (INVENTORY_LEFT_HAND_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_LEFT_HAND_SLOT_MAX_Y (INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_RIGHT_HAND_SLOT_X 245
#define INVENTORY_RIGHT_HAND_SLOT_Y 286
#define INVENTORY_RIGHT_HAND_SLOT_MAX_X (INVENTORY_RIGHT_HAND_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_RIGHT_HAND_SLOT_MAX_Y (INVENTORY_RIGHT_HAND_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_ARMOR_SLOT_X 154
#define INVENTORY_ARMOR_SLOT_Y 183
#define INVENTORY_ARMOR_SLOT_MAX_X (INVENTORY_ARMOR_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_ARMOR_SLOT_MAX_Y (INVENTORY_ARMOR_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_TRADE_SCROLLER_Y 30

// How long A must be held on a barter slot before it describes the item instead of moving it.
// Same 220 ms the pad wheels use, so every tap-vs-hold on the controller feels identical.
#define BARTER_LOOK_HOLD_MS 220
#define INVENTORY_TRADE_INNER_SCROLLER_Y 20

#define INVENTORY_TRADE_LEFT_SCROLLER_X 29
#define INVENTORY_TRADE_LEFT_SCROLLER_Y INVENTORY_TRADE_SCROLLER_Y

#define INVENTORY_TRADE_RIGHT_SCROLLER_X 388
#define INVENTORY_TRADE_RIGHT_SCROLLER_Y INVENTORY_TRADE_SCROLLER_Y

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_X 165
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y INVENTORY_TRADE_INNER_SCROLLER_Y

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X 250
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y INVENTORY_TRADE_INNER_SCROLLER_Y

#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X 0
#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X 165
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X 250
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X 395
#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_LOOT_LEFT_SCROLLER_X 46
#define INVENTORY_LOOT_LEFT_SCROLLER_Y 35
#define INVENTORY_LOOT_LEFT_SCROLLER_MAX_X (INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_LOOT_RIGHT_SCROLLER_X 424
#define INVENTORY_LOOT_RIGHT_SCROLLER_Y 35
#define INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X (INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_SCROLLER_X 46
#define INVENTORY_SCROLLER_Y 35
#define INVENTORY_SCROLLER_MAX_X (INVENTORY_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_BODY_VIEW_WIDTH 60
#define INVENTORY_BODY_VIEW_HEIGHT 100

#define INVENTORY_PC_BODY_VIEW_X 176
#define INVENTORY_PC_BODY_VIEW_Y 37
#define INVENTORY_PC_BODY_VIEW_MAX_X (INVENTORY_PC_BODY_VIEW_X + INVENTORY_BODY_VIEW_WIDTH)
#define INVENTORY_PC_BODY_VIEW_MAX_Y (INVENTORY_PC_BODY_VIEW_Y + INVENTORY_BODY_VIEW_HEIGHT)

#define INVENTORY_LOOT_RIGHT_BODY_VIEW_X 297
#define INVENTORY_LOOT_RIGHT_BODY_VIEW_Y 37

#define INVENTORY_LOOT_LEFT_BODY_VIEW_X 176
#define INVENTORY_LOOT_LEFT_BODY_VIEW_Y 37

#define INVENTORY_SUMMARY_X 297
#define INVENTORY_SUMMARY_Y 44
#define INVENTORY_SUMMARY_MAX_X 440

#define INVENTORY_WINDOW_WIDTH 499
#define INVENTORY_USE_ON_WINDOW_WIDTH 292
#define INVENTORY_LOOT_WINDOW_WIDTH 537
#define INVENTORY_TRADE_WINDOW_WIDTH 480
#define INVENTORY_TIMER_WINDOW_WIDTH 259

#define INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH 640
#define INVENTORY_TRADE_BACKGROUND_WINDOW_HEIGHT 480
#define INVENTORY_TRADE_WINDOW_OFFSET ((INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH - INVENTORY_TRADE_WINDOW_WIDTH) / 2)

#define INVENTORY_SLOT_PADDING 4

#define INVENTORY_SCROLLER_X_PAD (INVENTORY_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_SCROLLER_Y_PAD (INVENTORY_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_LOOT_LEFT_SCROLLER_X_PAD (INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD (INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_LOOT_RIGHT_SCROLLER_X_PAD (INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_LOOT_RIGHT_SCROLLER_Y_PAD (INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_LEFT_SCROLLER_X_PAD (INVENTORY_TRADE_LEFT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD (INVENTORY_TRADE_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_RIGHT_SCROLLER_X_PAD (INVENTORY_TRADE_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_RIGHT_SCROLLER_Y_PAD (INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD (INVENTORY_TRADE_INNER_LEFT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD (INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_SLOT_WIDTH_PAD (INVENTORY_SLOT_WIDTH - INVENTORY_SLOT_PADDING * 2)
#define INVENTORY_SLOT_HEIGHT_PAD (INVENTORY_SLOT_HEIGHT - INVENTORY_SLOT_PADDING * 2)

#define INVENTORY_NORMAL_WINDOW_PC_ROTATION_DELAY (1000U / ROTATION_COUNT)

typedef void(InventoryPrintItemDescriptionHandler)(char* string);

typedef enum InventoryArrowFrm {
    INVENTORY_ARROW_FRM_LEFT_ARROW_UP,
    INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN,
    INVENTORY_ARROW_FRM_RIGHT_ARROW_UP,
    INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN,
    INVENTORY_ARROW_FRM_COUNT,
} InventoryArrowFrm;

typedef struct InventoryWindowConfiguration {
    int field_0; // artId
    int width;
    int height;
    int x;
    int y;
} InventoryWindowDescription;

typedef struct InventoryCursorData {
    Art* frm;
    unsigned char* frmData;
    int width;
    int height;
    int offsetX;
    int offsetY;
    CacheEntry* frmHandle;
} InventoryCursorData;

static int inventry_msg_load();
static int inventry_msg_unload();
static void display_inventory_info(Object* item, int quantity, unsigned char* dest, int pitch, bool a5);
static void inven_update_lighting(Object* a1);
static int barter_compute_value(Object* buyer, Object* seller);
static int barter_attempt_transaction(Object* a1, Object* a2, Object* a3, Object* a4);
static void barter_move_inventory(Object* a1, int quantity, int a3, int a4, Object* a5, Object* a6, bool a7);
static void barter_move_from_table_inventory(Object* a1, int quantity, int a3, Object* a4, Object* a5, bool a6);
static void display_table_inventories(int win, Object* a2, Object* a3, int a4);
static int do_move_timer(int inventoryWindowType, Object* item, int a3);
static void inven_place_if_requested(Object* item, Object* owner, Object** handSlot);
static int setup_move_timer_win(int inventoryWindowType, Object* item);
static int exit_move_timer_win(int inventoryWindowType);

// The number of items to show in scroller.
//
// 0x50563C
static int inven_cur_disp = 6;

// 0x505640
static Object* inven_dude = NULL;

// Probably fid of armor to display in inventory dialog.
//
// 0x505644
static int inven_pid = -1;

// 0x505648
static bool inven_is_initialized = false;

// 0x50564C
static int inven_display_msg_line = 1;

// 0x505650
static InventoryWindowDescription iscr_data[INVENTORY_WINDOW_TYPE_COUNT] = {
    { 48, INVENTORY_WINDOW_WIDTH, 377, 80, 0 },
    { 113, INVENTORY_USE_ON_WINDOW_WIDTH, 376, 80, 0 },
    { 114, INVENTORY_LOOT_WINDOW_WIDTH, 376, 80, 0 },
    { 111, INVENTORY_TRADE_WINDOW_WIDTH, 180, 80, 290 },
    { 305, INVENTORY_TIMER_WINDOW_WIDTH, 162, 140, 80 },
    { 305, INVENTORY_TIMER_WINDOW_WIDTH, 162, 140, 80 },
};

// 0x5056C8
static bool dropped_explosive = false;

// ⚠️ THE TIMER WINDOW CAN PUT THE EXPLOSIVE DOWN TOO (2026-10-16, "nebo do artu přidat tlačítko nastavit
// čas a položit?"). Arming dynamite left it ticking in the pack, and putting it down meant opening
// the inventory again and finding Drop -- on a pad, with the fuse running. The window's PLACE (the
// plate under the clock, X on the pad) sets the timer exactly as DONE does and also asks for the
// explosive to go on the ground; the one who armed it reads the request back
// (inven_set_timer_place_requested) and drops it. The plate is vanilla's own ALL button from the
// move-items mode of this same window, with the label drawn into a COPY (mt_place_up/down):
// vanilla draws "ALL" straight into the cached art, and drawing PLACE there would leave it under
// every later ALL.
#define MT_KEY_PLACE 5001
#define MT_PLACE_W 94
#define MT_PLACE_H 33
static bool mt_place_requested = false;
static unsigned char mt_place_up[MT_PLACE_W * MT_PLACE_H];
static unsigned char mt_place_down[MT_PLACE_W * MT_PLACE_H];

// 0x59CD2C
static CacheEntry* mt_key[8];

// 0x59CD4C
CacheEntry* ikey[OFF_59E7BC_COUNT];

// 0x59CD9C
static int target_stack_offset[10];

// inventory.msg
//
// 0x059CDC4
static MessageList inventry_message_file;

// 0x59CDF4
static Object* target_stack[10];

// 0x59CD74
static int stack_offset[10];

// 0x59CDCC
static Object* stack[10];

// 0x59CE1C
static int mt_wid;

// 0x59CE24
static int barter_mod;

// 0x59CE28
static int btable_offset;

// 0x59CE2C
static int ptable_offset;

// 0x59CE34
static Inventory* ptable_pud;

// 0x59CE38
static InventoryCursorData imdata[INVENTORY_WINDOW_CURSOR_COUNT];

// 0x59CEC4
static Object* ptable;

// 0x59CE20
static InventoryPrintItemDescriptionHandler* display_msg;

// 0x59CECC
static int im_value;

// 0x59CED0
static int immode;

// 0x59CED8
static Object* btable;

// 0x59CEDC
static int target_curr_stack;

// 0x59CEE0
static Inventory* btable_pud;

// 0x59CEE8
static bool inven_ui_was_disabled;

// 0x59CEEC
static Object* i_worn;

// 0x59CEF0
static Object* i_lhand;

// Rotating character's fid.
//
// 0x59CEF4
static int i_fid;

// 0x59CE30
static Inventory* pud;

// 0x59CEF8
static int i_wid;

// 0x059CEFC
static Object* i_rhand;

// 0x59CEC8
static int curr_stack;

// 0x59CF00
static int i_wid_max_y;

// 0x59CF04
static int i_wid_max_x;

// 0x59CED4
static Inventory* target_pud;

// 0x59CEE4
static int barter_back_win;

// 0x4623E8
void inven_set_dude(Object* obj, int pid)
{
    inven_dude = obj;
    inven_pid = pid;
}

// 0x4623F4
void inven_reset_dude()
{
    inven_dude = obj_dude;
    inven_pid = 0x1000000;
}

// 0x46240C
static int inventry_msg_load()
{
    char path[COMPAT_MAX_PATH];

    if (!message_init(&inventry_message_file))
        return -1;

    snprintf(path, sizeof(path), "%s%s", msg_path, "inventry.msg");
    if (!message_load(&inventry_message_file, path))
        return -1;

    return 0;
}

// 0x462470
static int inventry_msg_unload()
{
    message_exit(&inventry_message_file);
    return 0;
}

// True only while the normal inventory screen (handle_inventory) is up. Lets the
// controller layer offer equip/close shortcuts without misfiring inside loot,
// trade, or use-on screens that reuse the same window statics.
static bool normal_inventory_open = false;

bool inven_is_open()
{
    return normal_inventory_open;
}

// Stack of currently-open inventory window types (LIFO; the loot/barter/use
// screens can nest a container inside another inventory). Maintained by
// setup_inventory/exit_inventory so the gamepad layer can tell whether ANY
// inventory is open and which window type is on top, to route the A button.
static int inv_window_type_stack[8];
static int inv_window_type_depth = 0;

bool inven_any_open()
{
    return inv_window_type_depth > 0;
}

int inven_current_window_type()
{
    return inv_window_type_depth > 0
        ? inv_window_type_stack[inv_window_type_depth - 1]
        : -1;
}

// True if the mouse cursor is over either loot item panel (the player's or the
// container's). The gamepad layer uses this so that tap-A over a panel does a
// quick-take, while tap-A elsewhere (Done and other GNW buttons) does a normal
// click instead.
bool inven_loot_cursor_over_panel()
{
    if (i_wid == -1) {
        return false;
    }

    if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
        return true;
    }

    if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
        return true;
    }

    return false;
}

// True if the cursor is over the normal/use-on inventory item list. The gamepad
// layer uses this so tap-A over the list does a quick equip/use, while tap-A
// elsewhere (Done and other GNW buttons) does a normal click instead.
bool inven_normal_cursor_over_list()
{
    if (i_wid == -1) {
        return false;
    }

    return mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y);
}

// True if the cursor is over the body-armor slot of the normal inventory. The gamepad layer routes a
// tap of A there to inven_gamepad_quick_normal, which takes the armor off (2026-10-10).
bool inven_normal_cursor_over_armor()
{
    if (i_wid == -1) {
        return false;
    }

    return mouseHitTestInWindow(i_wid, INVENTORY_ARMOR_SLOT_X, INVENTORY_ARMOR_SLOT_Y, INVENTORY_ARMOR_SLOT_MAX_X, INVENTORY_ARMOR_SLOT_MAX_Y);
}

// How long a D-pad direction must be held before the inventory list starts auto-scrolling, and how
// fast it steps once it does. The delay has to clear a normal deliberate tap; the rate is roughly
// the Windows list-repeat cadence, brisk without overshooting.
#define INVEN_PAD_REPEAT_DELAY_MS 350
#define INVEN_PAD_REPEAT_RATE_MS 70

// Gamepad D-pad up/down -> inventory list scroll. One step on the press, then HOLD TO REPEAT;
// returns KEY_ARROW_UP / KEY_ARROW_DOWN (which the inventory loop already handles as scroll) or -1.
// D-pad ONLY on purpose: the LEFT STICK already drives the cursor (gpui_menu_snap) in this modal
// window, so using it here too would fight that. Read directly because gamepad_ui_tick doesn't run
// inside this modal loop (same pattern as the load/save slot list).
static int inven_gamepad_scroll_key()
{
    static bool prevUp = false;
    static bool prevDown = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevUp = false;
        prevDown = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;

    // HOLD TO REPEAT. It used to be edge-only, so a long inventory meant one press per row --
    // "zatÃ­m jde pouze klikat". Now a held direction keeps stepping: one row on the press, then a
    // pause, then a steady stream, which is how every list on a controller behaves.
    static unsigned int heldSince = 0;
    static unsigned int lastRepeat = 0;

    int keyCode = -1;
    bool edge = (upNow && !prevUp) || (downNow && !prevDown);
    unsigned int now = get_time();

    if (edge) {
        heldSince = now;
        lastRepeat = now;
        keyCode = upNow && !prevUp ? KEY_ARROW_UP : KEY_ARROW_DOWN;
    } else if (upNow || downNow) {
        // Delay before the stream starts, so a deliberate single step never runs away, then a fixed
        // interval between repeats.
        if (now - heldSince >= INVEN_PAD_REPEAT_DELAY_MS
            && now - lastRepeat >= INVEN_PAD_REPEAT_RATE_MS) {
            lastRepeat = now;
            keyCode = upNow ? KEY_ARROW_UP : KEY_ARROW_DOWN;
        }
    }

    prevUp = upNow;
    prevDown = downNow;
    return keyCode;
}

// 0x462480
void handle_inventory()
{
    // In realtime combat there is no turn scheduler (combat_rt keeps the combat "warm" bit set,
    // so isInCombat() is true, but combat_whose_turn() never becomes the dude) -- so the vanilla
    // turn-owner gate would permanently refuse the inventory during a fight. Realtime is always
    // "the dude's turn", so skip the check there; the AP gate below still applies.
    if (isInCombat() && !combat_rt_enabled()) {
        if (combat_whose_turn() != inven_dude) {
            return;
        }
    }

    if (inven_init() == -1) {
        return;
    }

    if (isInCombat()) {
        if (inven_dude == obj_dude) {
            int actionPointsRequired = 4 - perk_level(PERK_QUICK_POCKETS);

            // Realtime keeps the engine combat.ap maxed (movement is free), so the vanilla
            // deduction below would be a no-op -- charge combat_rt's own pool instead so
            // opening the inventory mid-fight costs AP just like vanilla (4 - Quick Pockets).
            // combat_rt_spend_ap is free out of combat and refuses (returns false) when the
            // pool can't afford it.
            bool affordable;
            if (combat_rt_enabled()) {
                affordable = actionPointsRequired <= 0 || combat_rt_spend_ap(actionPointsRequired);
            } else {
                affordable = actionPointsRequired <= 0 || actionPointsRequired <= obj_dude->data.critter.combat.ap;
            }

            if (!affordable) {
                // You don't have enough action points to use inventory
                MessageListItem messageListItem;
                messageListItem.num = 19;
                if (message_search(&inventry_message_file, &messageListItem)) {
                    display_print(messageListItem.text);
                }

                // NOTE: Uninline.
                inven_exit();

                return;
            }

            if (!combat_rt_enabled() && actionPointsRequired > 0) {
                obj_dude->data.critter.combat.ap -= actionPointsRequired;
                intface_update_move_points(obj_dude->data.critter.combat.ap, combat_free_move);
            }
        }
    }

    Object* oldArmor = inven_worn(inven_dude);
    bool isoWasEnabled = setup_inventory(INVENTORY_WINDOW_TYPE_NORMAL);
    register_clear(inven_dude);
    display_stats();
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);

    normal_inventory_open = true;

    for (;;) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();

        // Gamepad D-pad / left stick scrolls the item list (injects arrow keys the loop
        // below already handles). Only when there's no other input this frame.
        if (keyCode == -1) {
            int padKey = inven_gamepad_scroll_key();
            if (padKey != -1) {
                keyCode = padKey;
            }
        }

        if (keyCode == KEY_ESCAPE || keyCode == KEY_UPPERCASE_I || keyCode == KEY_LOWERCASE_I) {
            break;
        }

        if (game_user_wants_to_quit != 0) {
            break;
        }

        // Controller tap-A quick action: the gamepad layer sets a pending request
        // from the input hook; run it here (outside the hook) where the use/equip
        // pipeline's map/monitor redraw is safe. See gamepad_ui_take_quick_normal.
        if (gamepad_ui_take_quick_normal()) {
            inven_gamepad_quick_normal();
        }

        display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);

        if (game_state() == GAME_STATE_5) {
            break;
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X) {
            game_quit_with_confirm();
        } else if (keyCode == KEY_ARROW_UP) {
            if (stack_offset[curr_stack] > 0) {
                stack_offset[curr_stack] -= 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (inven_cur_disp + stack_offset[curr_stack] < pud->length) {
                stack_offset[curr_stack] += 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            }
        } else if (keyCode == 2500) {
            container_exit(keyCode, INVENTORY_WINDOW_TYPE_NORMAL);
        } else {
            if ((mouse_get_buttons() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (immode == INVENTORY_WINDOW_CURSOR_HAND) {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
                } else if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
                    display_stats();
                    win_draw(i_wid);
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1008) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_NORMAL);
                    } else {
                        inven_pickup(keyCode, stack_offset[curr_stack]);
                    }
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (stack_offset[curr_stack] > 0) {
                            stack_offset[curr_stack] -= 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
                        }
                    } else if (wheelY < 0) {
                        if (inven_cur_disp + stack_offset[curr_stack] < pud->length) {
                            stack_offset[curr_stack] += 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
                        }
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    normal_inventory_open = false;

    inven_dude = stack[0];
    adjust_fid();

    if (inven_dude == obj_dude) {
        Rect rect;
        obj_change_fid(inven_dude, i_fid, &rect);
        tile_refresh_rect(&rect, inven_dude->elevation);
    }

    Object* newArmor = inven_worn(inven_dude);
    if (inven_dude == obj_dude) {
        if (oldArmor != newArmor) {
            intface_update_ac(true);
        }
    }

    exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inven_exit();

    if (inven_dude == obj_dude) {
        intface_update_items(false);
    }
}

// 0x462818
bool setup_inventory(int inventoryWindowType)
{
    // Track the open window type for the gamepad A router (balanced with the pop
    // in exit_inventory; the two are always called as a pair).
    if (inv_window_type_depth < (int)(sizeof(inv_window_type_stack) / sizeof(inv_window_type_stack[0]))) {
        inv_window_type_stack[inv_window_type_depth] = inventoryWindowType;
    }
    inv_window_type_depth++;

    dropped_explosive = 0;
    curr_stack = 0;
    stack_offset[0] = 0;
    inven_cur_disp = 6;
    pud = &(inven_dude->data.inventory);
    stack[0] = inven_dude;

    if (inventoryWindowType <= INVENTORY_WINDOW_TYPE_LOOT) {
        InventoryWindowDescription* windowDescription = &(iscr_data[inventoryWindowType]);

        // Maintain original position in original resolution, otherwise center it.
        int inventoryWindowX = screenGetWidth() != 640
            ? (screenGetWidth() - windowDescription->width) / 2
            : INVENTORY_WINDOW_X;
        int inventoryWindowY = screenGetHeight() != 480
            ? (screenGetHeight() - windowDescription->height) / 2
            : INVENTORY_WINDOW_Y;
        i_wid = win_add(inventoryWindowX,
            inventoryWindowY,
            windowDescription->width,
            windowDescription->height,
            257,
            WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        i_wid_max_x = windowDescription->width + inventoryWindowX;
        i_wid_max_y = windowDescription->height + inventoryWindowY;

        unsigned char* dest = win_get_buf(i_wid);
        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, windowDescription->field_0, 0, 0, 0);

        CacheEntry* backgroundFrmHandle;
        unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
        if (backgroundFrmData != NULL) {
            buf_to_buf(backgroundFrmData, windowDescription->width, windowDescription->height, windowDescription->width, dest, windowDescription->width);
            art_ptr_unlock(backgroundFrmHandle);
        }

        display_msg = display_print;
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        if (barter_back_win == -1) {
            exit(1);
        }

        inven_cur_disp = 3;

        // Trade inventory window is a part of game dialog, which is 640x480.
        int tradeWindowX = (screenGetWidth() - INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH) / 2 + INVENTORY_TRADE_WINDOW_X;
        int tradeWindowY = (screenGetHeight() - INVENTORY_TRADE_BACKGROUND_WINDOW_HEIGHT) / 2 + INVENTORY_TRADE_WINDOW_Y;
        i_wid = win_add(tradeWindowX, tradeWindowY, INVENTORY_TRADE_WINDOW_WIDTH, INVENTORY_TRADE_WINDOW_HEIGHT, 257, 0);
        i_wid_max_x = tradeWindowX + INVENTORY_TRADE_WINDOW_WIDTH;
        i_wid_max_y = tradeWindowY + INVENTORY_TRADE_WINDOW_HEIGHT;

        unsigned char* dest = win_get_buf(i_wid);
        unsigned char* src = win_get_buf(barter_back_win);
        buf_to_buf(src + INVENTORY_TRADE_WINDOW_X,
            INVENTORY_TRADE_WINDOW_WIDTH,
            INVENTORY_TRADE_WINDOW_HEIGHT,
            INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH,
            dest,
            INVENTORY_TRADE_WINDOW_WIDTH);

        display_msg = gdialog_display_msg;
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        // Create invsibile buttons representing character's inventory item
        // slots.
        for (int index = 0; index < inven_cur_disp; index++) {
            int btn = win_register_button(i_wid,
                INVENTORY_LOOT_LEFT_SCROLLER_X,
                INVENTORY_LOOT_LEFT_SCROLLER_Y + index * INVENTORY_SLOT_HEIGHT,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                1000 + index,
                -1,
                1000 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }
        }

        for (int index = 0; index < 6; index++) {
            int btn = win_register_button(i_wid,
                INVENTORY_LOOT_RIGHT_SCROLLER_X,
                INVENTORY_LOOT_RIGHT_SCROLLER_Y + index * INVENTORY_SLOT_HEIGHT,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2000 + index,
                -1,
                2000 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        int y1 = INVENTORY_TRADE_SCROLLER_Y;
        int y2 = INVENTORY_TRADE_INNER_SCROLLER_Y;

        for (int index = 0; index < inven_cur_disp; index++) {
            int btn;

            // Invsibile button representing left inventory slot.
            btn = win_register_button(i_wid,
                INVENTORY_TRADE_LEFT_SCROLLER_X,
                y1,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                1000 + index,
                -1,
                1000 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }

            // Invisible button representing right inventory slot.
            btn = win_register_button(i_wid,
                INVENTORY_TRADE_RIGHT_SCROLLER_X,
                y1,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2000 + index,
                -1,
                2000 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }

            // Invisible button representing left suggested slot.
            btn = win_register_button(i_wid,
                INVENTORY_TRADE_INNER_LEFT_SCROLLER_X,
                y2,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2300 + index,
                -1,
                2300 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }

            // Invisible button representing right suggested slot.
            btn = win_register_button(i_wid,
                INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X,
                y2,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2400 + index,
                -1,
                2400 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }

            y1 += INVENTORY_SLOT_HEIGHT;
            y2 += INVENTORY_SLOT_HEIGHT;
        }
    } else {
        // Create invisible buttons representing item slots.
        for (int index = 0; index < inven_cur_disp; index++) {
            int btn = win_register_button(i_wid,
                INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_HEIGHT * index + INVENTORY_SCROLLER_Y,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                1000 + index,
                -1,
                1000 + index,
                -1,
                NULL,
                NULL,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
            }
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        int btn;

        // Item2 slot
        btn = win_register_button(i_wid,
            INVENTORY_RIGHT_HAND_SLOT_X,
            INVENTORY_RIGHT_HAND_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1006,
            -1,
            1006,
            -1,
            NULL,
            NULL,
            NULL,
            0);
        if (btn != -1) {
            win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
        }

        // Item1 slot
        btn = win_register_button(i_wid,
            INVENTORY_LEFT_HAND_SLOT_X,
            INVENTORY_LEFT_HAND_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1007,
            -1,
            1007,
            -1,
            NULL,
            NULL,
            NULL,
            0);
        if (btn != -1) {
            win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
        }

        // Armor slot
        btn = win_register_button(i_wid,
            INVENTORY_ARMOR_SLOT_X,
            INVENTORY_ARMOR_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1008,
            -1,
            1008,
            -1,
            NULL,
            NULL,
            NULL,
            0);
        if (btn != -1) {
            win_register_button_func(btn, inven_hover_on, inven_hover_off, NULL, NULL);
        }
    }

    memset(ikey, 0, sizeof(ikey));

    int fid;
    int btn;
    unsigned char* buttonUpData;
    unsigned char* buttonDownData;

    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[0]));

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[1]));

    if (buttonUpData != NULL && buttonDownData != NULL) {
        btn = -1;
        switch (inventoryWindowType) {
        case INVENTORY_WINDOW_TYPE_NORMAL:
            // Done button
            btn = win_register_button(i_wid,
                437,
                329,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                buttonUpData,
                buttonDownData,
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            break;
        case INVENTORY_WINDOW_TYPE_USE_ITEM_ON:
            // Cancel button
            btn = win_register_button(i_wid,
                233,
                328,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                buttonUpData,
                buttonDownData,
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            break;
        case INVENTORY_WINDOW_TYPE_LOOT:
            // Done button
            btn = win_register_button(i_wid,
                288,
                328,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                buttonUpData,
                buttonDownData,
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            break;
        }

        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        // TODO: Figure out why it building fid in chain.

        // Large arrow up (normal).
        fid = art_id(OBJ_TYPE_INTERFACE, 100, 0, 0, 0);
        fid = art_id(OBJ_TYPE_INTERFACE, fid, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[2]));

        // Large arrow up (pressed).
        fid = art_id(OBJ_TYPE_INTERFACE, 101, 0, 0, 0);
        fid = art_id(OBJ_TYPE_INTERFACE, fid, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[3]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // Left inventory up button.
            btn = win_register_button(i_wid,
                109,
                56,
                23,
                24,
                -1,
                -1,
                KEY_ARROW_UP,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            // Right inventory up button.
            btn = win_register_button(i_wid,
                342,
                56,
                23,
                24,
                -1,
                -1,
                KEY_CTRL_ARROW_UP,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }
        }
    } else {
        // Large up arrow (normal).
        fid = art_id(OBJ_TYPE_INTERFACE, 49, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[2]));

        // Large up arrow (pressed).
        fid = art_id(OBJ_TYPE_INTERFACE, 50, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[3]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            if (inventoryWindowType != INVENTORY_WINDOW_TYPE_TRADE) {
                // Left inventory up button.
                btn = win_register_button(i_wid,
                    128,
                    39,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_ARROW_UP,
                    -1,
                    buttonUpData,
                    buttonDownData,
                    NULL,
                    0);
                if (btn != -1) {
                    win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                }
            }

            if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                // Right inventory up button.
                btn = win_register_button(i_wid,
                    379,
                    39,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_CTRL_ARROW_UP,
                    -1,
                    buttonUpData,
                    buttonDownData,
                    NULL,
                    0);
                if (btn != -1) {
                    win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                }
            }
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        // Large dialog down button (normal)
        fid = art_id(OBJ_TYPE_INTERFACE, 93, 0, 0, 0);
        fid = art_id(OBJ_TYPE_INTERFACE, fid, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[5]));

        // Dialog down button (pressed)
        fid = art_id(OBJ_TYPE_INTERFACE, 94, 0, 0, 0);
        fid = art_id(OBJ_TYPE_INTERFACE, fid, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[6]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // Left inventory down button.
            btn = win_register_button(i_wid,
                109,
                82,
                24,
                25,
                -1,
                -1,
                KEY_ARROW_DOWN,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            // Right inventory down button
            btn = win_register_button(i_wid,
                342,
                82,
                24,
                25,
                -1,
                -1,
                KEY_CTRL_ARROW_DOWN,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            // Invisible button representing left character.
            win_register_button(barter_back_win, 15, 25, INVENTORY_BODY_VIEW_WIDTH, INVENTORY_BODY_VIEW_HEIGHT, -1, -1, 2500, -1, NULL, NULL, NULL, 0);

            // Invisible button representing right character.
            win_register_button(barter_back_win, 560, 25, INVENTORY_BODY_VIEW_WIDTH, INVENTORY_BODY_VIEW_HEIGHT, -1, -1, 2501, -1, NULL, NULL, NULL, 0);
        }
    } else {
        // Large arrow down (normal).
        fid = art_id(OBJ_TYPE_INTERFACE, 51, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[5]));

        // Large arrow down (pressed).
        fid = art_id(OBJ_TYPE_INTERFACE, 52, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[6]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // Left inventory down button.
            btn = win_register_button(i_wid,
                128,
                62,
                22,
                23,
                -1,
                -1,
                KEY_ARROW_DOWN,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);

            if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                // Invisible button representing left character.
                win_register_button(i_wid, INVENTORY_LOOT_LEFT_BODY_VIEW_X, INVENTORY_LOOT_LEFT_BODY_VIEW_Y, INVENTORY_BODY_VIEW_WIDTH, INVENTORY_BODY_VIEW_HEIGHT, -1, -1, 2500, -1, NULL, NULL, NULL, 0);

                // Right inventory down button.
                btn = win_register_button(i_wid,
                    379,
                    62,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_CTRL_ARROW_DOWN,
                    -1,
                    buttonUpData,
                    buttonDownData,
                    0,
                    0);
                if (btn != -1) {
                    win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                }

                // Invisible button representing right character.
                win_register_button(i_wid,
                    INVENTORY_LOOT_RIGHT_BODY_VIEW_X,
                    INVENTORY_LOOT_RIGHT_BODY_VIEW_Y,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    -1,
                    -1,
                    2501,
                    -1,
                    NULL,
                    NULL,
                    NULL,
                    0);
            } else {
                // Invisible button representing character (in inventory and use on dialogs).
                win_register_button(i_wid,
                    INVENTORY_PC_BODY_VIEW_X,
                    INVENTORY_PC_BODY_VIEW_Y,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    -1,
                    -1,
                    2500,
                    -1,
                    NULL,
                    NULL,
                    NULL,
                    0);
            }
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        // Inventory button up (normal)
        fid = art_id(OBJ_TYPE_INTERFACE, 49, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[8]));

        // Inventory button up (pressed)
        fid = art_id(OBJ_TYPE_INTERFACE, 50, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[9]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // Left offered inventory up button.
            btn = win_register_button(i_wid,
                128,
                113,
                22,
                23,
                -1,
                -1,
                KEY_PAGE_UP,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            // Right offered inventory up button.
            btn = win_register_button(i_wid,
                333,
                113,
                22,
                23,
                -1,
                -1,
                KEY_CTRL_PAGE_UP,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }
        }

        // Inventory button down (normal)
        fid = art_id(OBJ_TYPE_INTERFACE, 51, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(ikey[8]));

        // Inventory button down (pressed).
        fid = art_id(OBJ_TYPE_INTERFACE, 52, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(ikey[9]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // Left offered inventory down button.
            btn = win_register_button(i_wid,
                128,
                136,
                22,
                23,
                -1,
                -1,
                KEY_PAGE_DOWN,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            // Right offered inventory down button.
            btn = win_register_button(i_wid,
                333,
                136,
                22,
                23,
                -1,
                -1,
                KEY_CTRL_PAGE_DOWN,
                -1,
                buttonUpData,
                buttonDownData,
                NULL,
                0);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }
        }
    }

    i_rhand = NULL;
    i_worn = NULL;
    i_lhand = NULL;

    for (int index = 0; index < pud->length; index++) {
        InventoryItem* inventoryItem = &(pud->items[index]);
        Object* item = inventoryItem->item;
        if ((item->flags & OBJECT_IN_LEFT_HAND) != 0) {
            if ((item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
                i_rhand = item;
            }
            i_lhand = item;
        } else if ((item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
            i_rhand = item;
        } else if ((item->flags & OBJECT_WORN) != 0) {
            i_worn = item;
        }
    }

    if (i_lhand != NULL) {
        item_remove_mult(inven_dude, i_lhand, 1);
    }

    if (i_rhand != NULL && i_rhand != i_lhand) {
        item_remove_mult(inven_dude, i_rhand, 1);
    }

    if (i_worn != NULL) {
        item_remove_mult(inven_dude, i_worn, 1);
    }

    adjust_fid();

    bool isoWasEnabled = map_disable_bk_processes();

    gmouse_disable(0);

    return isoWasEnabled;
}

// 0x46353C
void exit_inventory(bool shouldEnableIso)
{
    // Balanced with the push in setup_inventory.
    if (inv_window_type_depth > 0) {
        inv_window_type_depth--;
    }

    inven_dude = stack[0];

    if (i_lhand != NULL) {
        i_lhand->flags |= OBJECT_IN_LEFT_HAND;
        if (i_lhand == i_rhand) {
            i_lhand->flags |= OBJECT_IN_RIGHT_HAND;
        }

        item_add_force(inven_dude, i_lhand, 1);
    }

    if (i_rhand != NULL && i_rhand != i_lhand) {
        i_rhand->flags |= OBJECT_IN_RIGHT_HAND;
        item_add_force(inven_dude, i_rhand, 1);
    }

    if (i_worn != NULL) {
        i_worn->flags |= OBJECT_WORN;
        item_add_force(inven_dude, i_worn, 1);
    }

    i_rhand = NULL;
    i_worn = NULL;
    i_lhand = NULL;

    for (int index = 0; index < OFF_59E7BC_COUNT; index++) {
        art_ptr_unlock(ikey[index]);
    }

    if (shouldEnableIso) {
        map_enable_bk_processes();
    }

    win_delete(i_wid);

    gmouse_enable();

    if (dropped_explosive) {
        inven_explosive_dropped_notice();
        dropped_explosive = false;
    }

    // ⚠️ THE BUMP LIVES HERE, NOT AT THE CALL SITES. Every inventory-family modal screen runs its
    // own nested input loop while the pad's button edges sit in statics, so the button that closed
    // the screen is still physically DOWN when gameplay resumes and its edge fires there. This was
    // fixed once for handle_inventory and the other three screens were simply forgotten: closing a
    // LOOT window with B cycled the weapon's firing mode, because B acts on the RELEASE edge and
    // that release arrived in gameplay with a bDownTime taken inside the loot window.
    //
    // exit_inventory has exactly four callers -- handle_inventory, use_inventory_on,
    // loot_container, barter_inventory -- and every one of them passes through here exactly once
    // on the way out, which is why the counter belongs here. See gamepad_ui_bump_input_epoch.
    gamepad_ui_bump_input_epoch();
}

// 0x463758
void display_inventory(int first_item_index, int selected_index, int inventoryWindowType)
{
    unsigned char* windowBuffer = win_get_buf(i_wid);
    int pitch;

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        pitch = INVENTORY_WINDOW_WIDTH;

        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);

        CacheEntry* backgroundFrmHandle;
        unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
        if (backgroundFrmData != NULL) {
            // Clear scroll view background.
            buf_to_buf(backgroundFrmData + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                inven_cur_disp * INVENTORY_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                pitch);

            // Clear armor button background.
            buf_to_buf(backgroundFrmData + pitch * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X,
                INVENTORY_LARGE_SLOT_WIDTH,
                INVENTORY_LARGE_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X,
                pitch);

            if (i_lhand != NULL && i_lhand == i_rhand) {
                // Clear item1.
                int itemBackgroundFid = art_id(OBJ_TYPE_INTERFACE, 32, 0, 0, 0);

                CacheEntry* itemBackgroundFrmHandle;
                Art* itemBackgroundFrm = art_ptr_lock(itemBackgroundFid, &itemBackgroundFrmHandle);
                if (itemBackgroundFrm != NULL) {
                    unsigned char* data = art_frame_data(itemBackgroundFrm, 0, 0);
                    int width = art_frame_width(itemBackgroundFrm, 0, 0);
                    int height = art_frame_length(itemBackgroundFrm, 0, 0);
                    buf_to_buf(data,
                        width,
                        height,
                        width,
                        windowBuffer + pitch * 284 + 152,
                        pitch);
                    art_ptr_unlock(itemBackgroundFrmHandle);
                }
            } else {
                // Clear both items in one go.
                buf_to_buf(backgroundFrmData + pitch * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X,
                    INVENTORY_LARGE_SLOT_WIDTH * 2,
                    INVENTORY_LARGE_SLOT_HEIGHT,
                    pitch,
                    windowBuffer + pitch * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X,
                    pitch);
            }

            art_ptr_unlock(backgroundFrmHandle);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_USE_ITEM_ON) {
        pitch = INVENTORY_USE_ON_WINDOW_WIDTH;

        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 113, 0, 0, 0);

        CacheEntry* backgroundFrmHandle;
        unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
        if (backgroundFrmData != NULL) {
            // Clear scroll view background.
            buf_to_buf(backgroundFrmData + pitch * 35 + 44, 64, inven_cur_disp * 48, pitch, windowBuffer + pitch * 35 + 44, pitch);
            art_ptr_unlock(backgroundFrmHandle);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        pitch = INVENTORY_LOOT_WINDOW_WIDTH;

        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);

        CacheEntry* backgroundFrmHandle;
        unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
        if (backgroundFrmData != NULL) {
            // Clear scroll view background.
            buf_to_buf(backgroundFrmData + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                inven_cur_disp * INVENTORY_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                pitch);
            art_ptr_unlock(backgroundFrmHandle);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        pitch = INVENTORY_TRADE_WINDOW_WIDTH;

        windowBuffer = win_get_buf(i_wid);

        buf_to_buf(win_get_buf(barter_back_win) + INVENTORY_TRADE_LEFT_SCROLLER_Y * INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH + INVENTORY_TRADE_LEFT_SCROLLER_X + INVENTORY_TRADE_WINDOW_OFFSET,
            INVENTORY_SLOT_WIDTH,
            INVENTORY_SLOT_HEIGHT * inven_cur_disp,
            INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH,
            windowBuffer + pitch * INVENTORY_TRADE_LEFT_SCROLLER_Y + INVENTORY_TRADE_LEFT_SCROLLER_X,
            pitch);
    } else {
        assert(false && "Should be unreachable");
    }

    int y = 0;
    for (int index = 0; index + first_item_index < pud->length && index < inven_cur_disp; index += 1) {
        int offset;
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            offset = pitch * (y + INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_LEFT_SCROLLER_X_PAD;
        } else {
            offset = pitch * (y + INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD) + INVENTORY_SCROLLER_X_PAD;
        }

        InventoryItem* inventoryItem = &(pud->items[index + first_item_index]);

        int inventoryFid = item_inv_fid(inventoryItem->item);
        scale_art(inventoryFid, windowBuffer + offset, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, pitch);

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
            offset = pitch * (y + INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD) + INVENTORY_LOOT_LEFT_SCROLLER_X_PAD;
        } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            offset = pitch * (y + INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_LEFT_SCROLLER_X_PAD;
        } else {
            offset = pitch * (y + INVENTORY_SCROLLER_Y_PAD) + INVENTORY_SCROLLER_X_PAD;
        }

        display_inventory_info(inventoryItem->item, inventoryItem->quantity, windowBuffer + offset, pitch, index == selected_index);

        y += INVENTORY_SLOT_HEIGHT;
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        if (i_rhand != NULL) {
            int width = i_rhand == i_lhand ? INVENTORY_LARGE_SLOT_WIDTH * 2 : INVENTORY_LARGE_SLOT_WIDTH;
            int inventoryFid = item_inv_fid(i_rhand);
            scale_art(inventoryFid, windowBuffer + 499 * INVENTORY_RIGHT_HAND_SLOT_Y + INVENTORY_RIGHT_HAND_SLOT_X, width, INVENTORY_LARGE_SLOT_HEIGHT, 499);
        }

        if (i_lhand != NULL && i_lhand != i_rhand) {
            int inventoryFid = item_inv_fid(i_lhand);
            scale_art(inventoryFid, windowBuffer + 499 * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, 499);
        }

        if (i_worn != NULL) {
            int inventoryFid = item_inv_fid(i_worn);
            scale_art(inventoryFid, windowBuffer + 499 * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, 499);
        }

        // CE: Gamepad button hints in the bottom-left corner of each weapon slot so the
        // player knows how to equip the highlighted item -- A into ITEM 1 (left hand),
        // B into ITEM 2 (right hand). Controller only; drawn on top of the slot so it
        // stays visible whether or not a weapon occupies it. ⚠️ These letters are a
        // PROMISE: inven_gamepad_quick_normal must put an A-tapped item in the A box.
        if (gamepad_ui_get_controller() != NULL) {
            int oldFont = text_curr();
            text_font(101);

            int hintY = INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT - text_height() - 2;

            text_to_buf(windowBuffer + 499 * hintY + (INVENTORY_LEFT_HAND_SLOT_X + 3),
                "A", INVENTORY_LARGE_SLOT_WIDTH, 499, colorTable[32747]);
            text_to_buf(windowBuffer + 499 * hintY + (INVENTORY_RIGHT_HAND_SLOT_X + 3),
                "B", INVENTORY_LARGE_SLOT_WIDTH, 499, colorTable[32747]);

            text_font(oldFont);
        }
    }

    // CE: Show items weight.
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        char formattedText[20];

        int oldFont = text_curr();
        text_font(101);

        CacheEntry* key;
        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        unsigned char* data = art_ptr_lock_data(backgroundFid, 0, 0, &key);
        if (data != NULL) {
            int x = INVENTORY_LOOT_LEFT_SCROLLER_X;
            int y = INVENTORY_LOOT_LEFT_SCROLLER_Y + inven_cur_disp * INVENTORY_SLOT_HEIGHT + 2;
            buf_to_buf(data + pitch * y + x,
                INVENTORY_SLOT_WIDTH,
                text_height(),
                pitch,
                windowBuffer + pitch * y + x,
                pitch);
            art_ptr_unlock(key);
        }

        Object* object = stack[0];

        int color = colorTable[992];
        if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            int carryWeight = stat_level(object, STAT_CARRY_WEIGHT);
            int inventoryWeight = item_total_weight(object);
            snprintf(formattedText, sizeof(formattedText), "%d/%d", inventoryWeight, carryWeight);
        } else {
            int inventoryWeight = item_total_weight(object);
            snprintf(formattedText, sizeof(formattedText), "%d", inventoryWeight);
        }

        int width = text_width(formattedText);
        int x = INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2 - width / 2;
        int y = INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * inven_cur_disp + 2;
        text_to_buf(windowBuffer + pitch * y + x, formattedText, width, pitch, color);

        text_font(oldFont);
    }

    win_draw(i_wid);
}

// Render inventory item.
//
// 0x463C00
void display_target_inventory(int first_item_index, int selected_index, Inventory* inventory, int inventoryWindowType)
{
    unsigned char* windowBuffer = win_get_buf(i_wid);

    int pitch;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        pitch = INVENTORY_LOOT_WINDOW_WIDTH;

        int fid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);

        CacheEntry* handle;
        unsigned char* data = art_ptr_lock_data(fid, 0, 0, &handle);
        if (data != NULL) {
            buf_to_buf(data + pitch * 35 + 422, 64, 48 * inven_cur_disp, pitch, windowBuffer + pitch * 35 + 422, pitch);
            art_ptr_unlock(handle);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        pitch = INVENTORY_TRADE_WINDOW_WIDTH;

        unsigned char* src = win_get_buf(barter_back_win);
        buf_to_buf(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_TRADE_RIGHT_SCROLLER_X + INVENTORY_TRADE_WINDOW_OFFSET,
            INVENTORY_SLOT_WIDTH,
            INVENTORY_SLOT_HEIGHT * inven_cur_disp,
            INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH,
            windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_TRADE_RIGHT_SCROLLER_X,
            INVENTORY_TRADE_WINDOW_WIDTH);
    } else {
        assert(false && "Should be unreachable");
    }

    int y = 0;
    for (int index = 0; index < inven_cur_disp && first_item_index + index < inventory->length; index++) {
        int offset;
        switch (inventoryWindowType) {
        case INVENTORY_WINDOW_TYPE_LOOT:
            offset = pitch * (y + INVENTORY_LOOT_RIGHT_SCROLLER_Y_PAD) + INVENTORY_LOOT_RIGHT_SCROLLER_X_PAD;
            break;
        case INVENTORY_WINDOW_TYPE_TRADE:
            offset = pitch * (y + INVENTORY_TRADE_RIGHT_SCROLLER_Y_PAD) + INVENTORY_TRADE_RIGHT_SCROLLER_X_PAD;
            break;
        }

        InventoryItem* inventoryItem = &(inventory->items[first_item_index + index]);
        int inventoryFid = item_inv_fid(inventoryItem->item);
        scale_art(inventoryFid, windowBuffer + offset, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, pitch);
        display_inventory_info(inventoryItem->item, inventoryItem->quantity, windowBuffer + offset, pitch, index == selected_index);

        y += INVENTORY_SLOT_HEIGHT;
    }

    // CE: Show items weight.
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        char formattedText[20];
        formattedText[0] = '\0';

        int oldFont = text_curr();
        text_font(101);

        CacheEntry* key;
        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        unsigned char* data = art_ptr_lock_data(backgroundFid, 0, 0, &key);
        if (data != NULL) {
            int x = INVENTORY_LOOT_RIGHT_SCROLLER_X;
            int y = INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * inven_cur_disp + 2;
            buf_to_buf(data + pitch * y + x,
                INVENTORY_SLOT_WIDTH,
                text_height(),
                pitch,
                windowBuffer + pitch * y + x,
                pitch);
            art_ptr_unlock(key);
        }

        Object* object = target_stack[target_curr_stack];

        int color = colorTable[992];
        if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            int currentWeight = item_total_weight(object);
            int maxWeight = stat_level(object, STAT_CARRY_WEIGHT);
            snprintf(formattedText, sizeof(formattedText), "%d/%d", currentWeight, maxWeight);
        } else if (PID_TYPE(object->pid) == OBJ_TYPE_ITEM) {
            if (item_get_type(object) == ITEM_TYPE_CONTAINER) {
                int currentSize = item_c_curr_size(object);
                int maxSize = item_c_max_size(object);
                snprintf(formattedText, sizeof(formattedText), "%d/%d", currentSize, maxSize);
            }
        } else {
            int inventoryWeight = item_total_weight(object);
            snprintf(formattedText, sizeof(formattedText), "%d", inventoryWeight);
        }

        int width = text_width(formattedText);
        int x = INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2 - width / 2;
        int y = INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * inven_cur_disp + 2;
        text_to_buf(windowBuffer + pitch * y + x, formattedText, width, pitch, color);

        text_font(oldFont);
    }
}

// Renders inventory item quantity.
//
// 0x463E00
static void display_inventory_info(Object* item, int quantity, unsigned char* dest, int pitch, bool a5)
{
    int oldFont = text_curr();
    text_font(101);

    char formattedText[12];

    // NOTE: Original code is slightly different and probably used goto.
    bool draw = false;

    if (item_get_type(item) == ITEM_TYPE_AMMO) {
        int ammoQuantity = item_w_max_ammo(item) * (quantity - 1);

        if (!a5) {
            ammoQuantity += item_w_curr_ammo(item);
        }

        if (ammoQuantity > 99999) {
            ammoQuantity = 99999;
        }

        snprintf(formattedText, sizeof(formattedText), "x%d", ammoQuantity);
        draw = true;
    } else {
        if (quantity > 1) {
            int v9 = quantity;
            if (a5) {
                v9 -= 1;
            }

            // NOTE: Checking for quantity twice probably means inlined function
            // or some macro expansion.
            if (quantity > 1) {
                if (v9 > 99999) {
                    v9 = 99999;
                }

                snprintf(formattedText, sizeof(formattedText), "x%d", v9);
                draw = true;
            }
        }
    }

    if (draw) {
        text_to_buf(dest, formattedText, 80, pitch, colorTable[32767]);
    }

    text_font(oldFont);
}

// 0x463EB0
void display_body(int fid, int inventoryWindowType)
{
    // 0x5056CC
    static unsigned int ticker = 0;

    // 0x5056D0
    static int curr_rot = 0;

    if (elapsed_time(ticker) < INVENTORY_NORMAL_WINDOW_PC_ROTATION_DELAY) {
        return;
    }

    curr_rot += 1;

    if (curr_rot == ROTATION_COUNT) {
        curr_rot = 0;
    }

    int rotations[2];
    if (fid == -1) {
        rotations[0] = curr_rot;
        rotations[1] = ROTATION_SE;
    } else {
        rotations[0] = ROTATION_SW;
        rotations[1] = target_stack[target_curr_stack]->rotation;
    }

    int fids[2] = {
        i_fid,
        fid,
    };

    for (int index = 0; index < 2; index += 1) {
        int fid = fids[index];
        if (fid == -1) {
            continue;
        }

        CacheEntry* handle;
        Art* art = art_ptr_lock(fid, &handle);
        if (art == NULL) {
            continue;
        }

        int frame = 0;
        if (index == 1) {
            frame = art_frame_max_frame(art) - 1;
        }

        int rotation = rotations[index];

        unsigned char* frameData = art_frame_data(art, frame, rotation);

        int framePitch = art_frame_width(art, frame, rotation);
        int frameWidth = std::min(framePitch, INVENTORY_BODY_VIEW_WIDTH);

        int frameHeight = art_frame_length(art, frame, rotation);
        if (frameHeight > INVENTORY_BODY_VIEW_HEIGHT) {
            frameHeight = INVENTORY_BODY_VIEW_HEIGHT;
        }

        Rect rect;
        CacheEntry* backrgroundFrmHandle;
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            unsigned char* windowBuffer = win_get_buf(barter_back_win);
            int windowPitch = win_width(barter_back_win);

            if (index == 1) {
                rect.ulx = 560;
                rect.uly = 25;
            } else {
                rect.ulx = 15;
                rect.uly = 25;
            }

            rect.lrx = rect.ulx + INVENTORY_BODY_VIEW_WIDTH - 1;
            rect.lry = rect.uly + INVENTORY_BODY_VIEW_HEIGHT - 1;

            int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 111, 0, 0, 0);

            unsigned char* src = art_ptr_lock_data(backgroundFid, 0, 0, &backrgroundFrmHandle);
            if (src != NULL) {
                buf_to_buf(src + rect.uly * 640 + rect.ulx,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    640,
                    windowBuffer + windowPitch * rect.uly + rect.ulx,
                    windowPitch);
            }

            trans_buf_to_buf(frameData, frameWidth, frameHeight, framePitch,
                windowBuffer + windowPitch * (rect.uly + (INVENTORY_BODY_VIEW_HEIGHT - frameHeight) / 2) + (INVENTORY_BODY_VIEW_WIDTH - frameWidth) / 2 + rect.ulx,
                windowPitch);

            win_draw_rect(barter_back_win, &rect);
        } else {
            unsigned char* windowBuffer = win_get_buf(i_wid);
            int windowPitch = win_width(i_wid);

            if (index == 1) {
                rect.ulx = 297;
                rect.uly = 37;
            } else {
                rect.ulx = 176;
                rect.uly = 37;
            }

            rect.lrx = rect.ulx + INVENTORY_BODY_VIEW_WIDTH - 1;
            rect.lry = rect.uly + INVENTORY_BODY_VIEW_HEIGHT - 1;

            int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
            unsigned char* src = art_ptr_lock_data(backgroundFid, 0, 0, &backrgroundFrmHandle);
            if (src != NULL) {
                buf_to_buf(src + INVENTORY_LOOT_WINDOW_WIDTH * rect.uly + rect.ulx,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    INVENTORY_LOOT_WINDOW_WIDTH,
                    windowBuffer + windowPitch * rect.uly + rect.ulx,
                    windowPitch);
            }

            trans_buf_to_buf(frameData, frameWidth, frameHeight, framePitch,
                windowBuffer + windowPitch * (rect.uly + (INVENTORY_BODY_VIEW_HEIGHT - frameHeight) / 2) + (INVENTORY_BODY_VIEW_WIDTH - frameWidth) / 2 + rect.ulx,
                windowPitch);

            win_draw_rect(i_wid, &rect);
        }

        art_ptr_unlock(backrgroundFrmHandle);
        art_ptr_unlock(handle);
    }

    ticker = get_time();
}

// 0x46424C
int inven_init()
{
    // 0x5056D4
    static int num[INVENTORY_WINDOW_CURSOR_COUNT] = {
        286, // pointing hand
        250, // action arrow
        282, // action pick
        283, // action menu
        266, // blank
    };

    if (inventry_msg_load() == -1) {
        return -1;
    }

    inven_ui_was_disabled = game_ui_is_disabled();

    if (inven_ui_was_disabled) {
        game_ui_enable();
    }

    gmouse_3d_off();

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    int index;
    for (index = 0; index < INVENTORY_WINDOW_CURSOR_COUNT; index++) {
        InventoryCursorData* cursorData = &(imdata[index]);

        int fid = art_id(OBJ_TYPE_INTERFACE, num[index], 0, 0, 0);
        Art* frm = art_ptr_lock(fid, &(cursorData->frmHandle));
        if (frm == NULL) {
            break;
        }

        cursorData->frm = frm;
        cursorData->frmData = art_frame_data(frm, 0, 0);
        cursorData->width = art_frame_width(frm, 0, 0);
        cursorData->height = art_frame_length(frm, 0, 0);
        art_frame_hot(frm, 0, 0, &(cursorData->offsetX), &(cursorData->offsetY));
    }

    if (index != INVENTORY_WINDOW_CURSOR_COUNT) {
        for (; index >= 0; index--) {
            art_ptr_unlock(imdata[index].frmHandle);
        }

        if (inven_ui_was_disabled) {
            game_ui_disable(0);
        }

        message_exit(&inventry_message_file);

        return -1;
    }

    inven_is_initialized = true;
    im_value = -1;

    return 0;
}

// 0x4643AC
void inven_exit()
{
    for (int index = 0; index < INVENTORY_WINDOW_CURSOR_COUNT; index++) {
        art_ptr_unlock(imdata[index].frmHandle);
    }

    if (inven_ui_was_disabled) {
        game_ui_disable(0);
    }

    // NOTE: Uninline.
    inventry_msg_unload();

    inven_is_initialized = 0;
}

// 0x4643EC
void inven_set_mouse(int cursor)
{
    immode = cursor;

    if (cursor != INVENTORY_WINDOW_CURSOR_ARROW || im_value == -1) {
        InventoryCursorData* cursorData = &(imdata[cursor]);
        mouse_set_shape(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
    } else {
        inven_hover_on(-1, im_value);
    }
}

// 0x46444C
void inven_hover_on(int btn, int keyCode)
{
    // 0x5056E8
    static Object* last_target = NULL;

    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
        int x;
        int y;
        mouseGetPositionInWindow(i_wid, &x, &y);

        Object* a2a = NULL;
        if (inven_from_button(keyCode, &a2a, NULL, NULL) != 0) {
            gmouse_3d_build_pick_frame(x, y, 3, i_wid_max_x, i_wid_max_y);

            int v5 = 0;
            int v6 = 0;
            gmouse_3d_pick_frame_hot(&v5, &v6);

            InventoryCursorData* cursorData = &(imdata[INVENTORY_WINDOW_CURSOR_PICK]);
            mouse_set_shape(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, v5, v6, 0);

            if (a2a != last_target) {
                obj_look_at_func(stack[0], a2a, display_msg);
            }
        } else {
            InventoryCursorData* cursorData = &(imdata[INVENTORY_WINDOW_CURSOR_ARROW]);
            mouse_set_shape(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
        }

        last_target = a2a;
    }

    im_value = keyCode;
}

// 0x46453C
void inven_hover_off(int btn, int keyCode)
{
    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
        InventoryCursorData* cursorData = &(imdata[INVENTORY_WINDOW_CURSOR_ARROW]);
        mouse_set_shape(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
    }

    im_value = -1;
}

// 0x46457C
void inven_pickup(int keyCode, int first_item_index)
{
    Object* a1a;
    Object** v29 = NULL;
    int count = inven_from_button(keyCode, &a1a, &v29, NULL);
    if (count == 0) {
        return;
    }

    int v3 = -1;
    Rect rect;

    switch (keyCode) {
    case 1006:
        rect.ulx = 245;
        rect.uly = 286;
        break;
    case 1007:
        rect.ulx = 154;
        rect.uly = 286;
        break;
    case 1008:
        rect.ulx = 154;
        rect.uly = 183;
        break;
    default:
        // NOTE: Original code a little bit different, this code path
        // is only for key codes below 1006.
        v3 = keyCode - 1000;
        rect.ulx = 44;
        rect.uly = 48 * v3 + 35;
        break;
    }

    if (v3 == -1 || pud->items[first_item_index + v3].quantity <= 1) {
        unsigned char* windowBuffer = win_get_buf(i_wid);
        if (i_rhand != i_lhand || a1a != i_lhand) {
            int height;
            int width;
            if (v3 == -1) {
                height = INVENTORY_LARGE_SLOT_HEIGHT;
                width = INVENTORY_LARGE_SLOT_WIDTH;
            } else {
                height = INVENTORY_SLOT_HEIGHT;
                width = INVENTORY_SLOT_WIDTH;
            }

            CacheEntry* backgroundFrmHandle;
            int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
            unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
            if (backgroundFrmData != NULL) {
                buf_to_buf(backgroundFrmData + 499 * rect.uly + rect.ulx, width, height, 499, windowBuffer + 499 * rect.uly + rect.ulx, 499);
                art_ptr_unlock(backgroundFrmHandle);
            }

            rect.lrx = rect.ulx + width - 1;
            rect.lry = rect.uly + height - 1;
        } else {
            CacheEntry* backgroundFrmHandle;
            int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
            unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
            if (backgroundFrmData != NULL) {
                buf_to_buf(backgroundFrmData + 499 * 286 + 154, 180, 61, 499, windowBuffer + 499 * 286 + 154, 499);
                art_ptr_unlock(backgroundFrmHandle);
            }

            rect.ulx = 154;
            rect.uly = 286;
            rect.lrx = rect.ulx + 180 - 1;
            rect.lry = rect.uly + 61 - 1;
        }
        win_draw_rect(i_wid, &rect);
    } else {
        display_inventory(first_item_index, v3, INVENTORY_WINDOW_TYPE_NORMAL);
    }

    CacheEntry* itemInventoryFrmHandle;
    int itemInventoryFid = item_inv_fid(a1a);
    Art* itemInventoryFrm = art_ptr_lock(itemInventoryFid, &itemInventoryFrmHandle);
    if (itemInventoryFrm != NULL) {
        int width = art_frame_width(itemInventoryFrm, 0, 0);
        int height = art_frame_length(itemInventoryFrm, 0, 0);
        unsigned char* itemInventoryFrmData = art_frame_data(itemInventoryFrm, 0, 0);
        mouse_set_shape(itemInventoryFrmData, width, height, width, width / 2, height / 2, 0);
        gsound_play_sfx_file("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        get_input();
        display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (itemInventoryFrm != NULL) {
        art_ptr_unlock(itemInventoryFrmHandle);
        gsound_play_sfx_file("iputdown");
    }

    if (mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
        int x;
        int y;
        mouseGetPositionInWindow(i_wid, &x, &y);

        int index = (y - 39) / 48;
        if (index + first_item_index < pud->length) {
            Object* v19 = pud->items[index + first_item_index].item;
            if (v19 != a1a) {
                // TODO: Needs checking usage of v19
                if (item_get_type(v19) == ITEM_TYPE_CONTAINER) {
                    if (drop_into_container(v19, a1a, v3, v29, count) == 0) {
                        v3 = 0;
                    }
                } else {
                    if (drop_ammo_into_weapon(v19, a1a, v29, count, keyCode) == 0) {
                        v3 = 0;
                    }
                }
            }
        }

        if (v3 == -1) {
            // TODO: Holy shit, needs refactoring.
            *v29 = NULL;
            if (item_add_force(inven_dude, a1a, 1)) {
                *v29 = a1a;
            } else if (v29 == &i_worn) {
                adjust_ac(stack[0], a1a, NULL);
            } else if (i_rhand == i_lhand) {
                i_lhand = NULL;
                i_rhand = NULL;
            }
        }
    } else if (mouseHitTestInWindow(i_wid, INVENTORY_LEFT_HAND_SLOT_X, INVENTORY_LEFT_HAND_SLOT_Y, INVENTORY_LEFT_HAND_SLOT_MAX_X, INVENTORY_LEFT_HAND_SLOT_MAX_Y)) {
        if (i_lhand != NULL && item_get_type(i_lhand) == ITEM_TYPE_CONTAINER && i_lhand != a1a) {
            drop_into_container(i_lhand, a1a, v3, v29, count);
        } else if (i_lhand == NULL || drop_ammo_into_weapon(i_lhand, a1a, v29, count, keyCode)) {
            switch_hand(a1a, &i_lhand, v29, keyCode);
        }
    } else if (mouseHitTestInWindow(i_wid, INVENTORY_RIGHT_HAND_SLOT_X, INVENTORY_RIGHT_HAND_SLOT_Y, INVENTORY_RIGHT_HAND_SLOT_MAX_X, INVENTORY_RIGHT_HAND_SLOT_MAX_Y)) {
        if (i_rhand != NULL && item_get_type(i_rhand) == ITEM_TYPE_CONTAINER && i_rhand != a1a) {
            drop_into_container(i_rhand, a1a, v3, v29, count);
        } else if (i_rhand == NULL || drop_ammo_into_weapon(i_rhand, a1a, v29, count, keyCode)) {
            switch_hand(a1a, &i_rhand, v29, v3);
        }
    } else if (mouseHitTestInWindow(i_wid, INVENTORY_ARMOR_SLOT_X, INVENTORY_ARMOR_SLOT_Y, INVENTORY_ARMOR_SLOT_MAX_X, INVENTORY_ARMOR_SLOT_MAX_Y)) {
        if (item_get_type(a1a) == ITEM_TYPE_ARMOR) {
            Object* v21 = i_worn;
            int v22 = 0;
            if (v3 != -1) {
                item_remove_mult(inven_dude, a1a, 1);
            }

            if (i_worn != NULL) {
                if (v29 != NULL) {
                    *v29 = i_worn;
                } else {
                    i_worn = NULL;
                    v22 = item_add_force(inven_dude, v21, 1);
                }
            } else {
                if (v29 != NULL) {
                    *v29 = i_worn;
                }
            }

            if (v22 != 0) {
                i_worn = v21;
                if (v3 != -1) {
                    item_add_force(inven_dude, a1a, 1);
                }
            } else {
                adjust_ac(stack[0], v21, a1a);
                i_worn = a1a;
            }
        }
    } else if (mouseHitTestInWindow(i_wid, INVENTORY_PC_BODY_VIEW_X, INVENTORY_PC_BODY_VIEW_Y, INVENTORY_PC_BODY_VIEW_MAX_X, INVENTORY_PC_BODY_VIEW_MAX_Y)) {
        if (curr_stack != 0) {
            // TODO: Check this curr_stack - 1, not sure.
            drop_into_container(stack[curr_stack - 1], a1a, v3, v29, count);
        }
    }

    adjust_fid();
    display_stats();
    display_inventory(first_item_index, -1, INVENTORY_WINDOW_TYPE_NORMAL);
    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
}

// Controller shortcut: equip the inventory-list item under the cursor into a
// hand slot (hand == HAND_LEFT or HAND_RIGHT). Mirrors the list-item -> hand-slot
// drop in inven_pickup, but is driven by a button press instead of a mouse drag.
// Returns false (no-op) unless the normal inventory is open and the cursor is
// over a populated list slot.
bool inven_equip_under_cursor(int hand)
{
    if (!normal_inventory_open || i_wid == -1) {
        return false;
    }

    // OVER A HAND SLOT = TAKE THAT HAND'S ITEM BACK OUT, rather than equip. Without this there was
    // no way to empty a hand from the inventory at all: X/B only ever worked over the item LIST, so
    // the slots were write-only and you could swap weapons forever but never end up empty-handed
    // ("v inventory mi nejdou odebrat itemy z A a B"). Which hand is decided by the SLOT under the
    // cursor, not by which button was pressed -- pointing at a thing and pressing either button
    // should do what you are pointing at.
    bool overLeft = mouseHitTestInWindow(i_wid, INVENTORY_LEFT_HAND_SLOT_X, INVENTORY_LEFT_HAND_SLOT_Y,
        INVENTORY_LEFT_HAND_SLOT_MAX_X, INVENTORY_LEFT_HAND_SLOT_MAX_Y);
    bool overRight = mouseHitTestInWindow(i_wid, INVENTORY_RIGHT_HAND_SLOT_X, INVENTORY_RIGHT_HAND_SLOT_Y,
        INVENTORY_RIGHT_HAND_SLOT_MAX_X, INVENTORY_RIGHT_HAND_SLOT_MAX_Y);

    if (overLeft || overRight) {
        Object** handSlot = overLeft ? &i_lhand : &i_rhand;
        if (*handSlot == NULL) {
            return false; // already empty -- let the caller refuse audibly
        }

        // Back into the inventory working copy, then clear the slot. item_add_force is what
        // switch_hand uses to return a displaced hand item, so the bookkeeping matches the
        // engine's own path and the OK button commits it normally.
        if (item_add_force(inven_dude, *handSlot, 1) != 0) {
            return false;
        }
        *handSlot = NULL;

        adjust_fid();
        display_stats();
        display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
        win_draw(i_wid);
        gsound_play_sfx_file("iputdown");
        return true;
    }

    // Same list region inven_pickup uses to decide a drop targets the list.
    if (!mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
        return false;
    }

    int x;
    int y;
    mouseGetPositionInWindow(i_wid, &x, &y);

    // Row under the cursor (matches inven_pickup's (y - 39) / 48 mapping).
    int displayIndex = (y - 39) / INVENTORY_SLOT_HEIGHT;
    if (displayIndex < 0 || displayIndex >= inven_cur_disp) {
        return false;
    }

    int itemIndex = stack_offset[curr_stack] + displayIndex;
    if (itemIndex >= pud->length) {
        return false;
    }

    Object* item = pud->items[itemIndex].item;
    if (item == NULL) {
        return false;
    }

    // a3 (source slot) is NULL for a list item; a4 != -1 makes switch_hand pull
    // the item out of the inventory and return any displaced hand item to it.
    Object** handSlot = (hand == HAND_RIGHT) ? &i_rhand : &i_lhand;
    switch_hand(item, handSlot, NULL, 1000 + displayIndex);

    adjust_fid();
    display_stats();
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
    win_draw(i_wid);
    gsound_play_sfx_file("iputdown");
    return true;
}

// Controller shortcut: WEAR the inventory-list armor under the cursor (into the body
// armor slot), instead of dropping it into a hand. Mirrors the list-item -> armor-slot
// drop in inven_pickup (the ITEM_TYPE_ARMOR branch over INVENTORY_ARMOR_SLOT), driven
// by a button press rather than a mouse drag: pull the item from the list, return any
// currently-worn armor to the list, run adjust_ac, and set it as worn. If the displaced
// armor can't fit back in the inventory the swap is rolled back (item stays in the list).
// Returns false (no-op) unless the normal inventory is open and ARMOR is under the cursor.
Object* inven_list_item_under_cursor()
{
    if (!normal_inventory_open || i_wid == -1) {
        return NULL;
    }
    // Same list region and row mapping as inven_equip_under_cursor.
    if (!mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
        return NULL;
    }
    int x;
    int y;
    mouseGetPositionInWindow(i_wid, &x, &y);
    int displayIndex = (y - 39) / INVENTORY_SLOT_HEIGHT;
    if (displayIndex < 0 || displayIndex >= inven_cur_disp) {
        return NULL;
    }
    int itemIndex = stack_offset[curr_stack] + displayIndex;
    if (itemIndex >= pud->length) {
        return NULL;
    }
    return pud->items[itemIndex].item;
}

bool inven_wear_under_cursor()
{
    if (!normal_inventory_open || i_wid == -1) {
        return false;
    }

    if (!mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
        return false;
    }

    int x;
    int y;
    mouseGetPositionInWindow(i_wid, &x, &y);

    int displayIndex = (y - 39) / INVENTORY_SLOT_HEIGHT;
    if (displayIndex < 0 || displayIndex >= inven_cur_disp) {
        return false;
    }

    int itemIndex = stack_offset[curr_stack] + displayIndex;
    if (itemIndex >= pud->length) {
        return false;
    }

    Object* item = pud->items[itemIndex].item;
    if (item == NULL || item_get_type(item) != ITEM_TYPE_ARMOR) {
        return false;
    }

    // Already worn (cursor over the same object) -- nothing to do.
    if (item == i_worn) {
        return false;
    }

    Object* oldArmor = i_worn;

    // Pull the to-wear item out of the inventory list.
    item_remove_mult(inven_dude, item, 1);

    // Return any currently-worn armor to the inventory list. If it won't fit, roll back.
    int addFailed = 0;
    if (oldArmor != NULL) {
        i_worn = NULL;
        addFailed = item_add_force(inven_dude, oldArmor, 1);
    }

    if (addFailed != 0) {
        i_worn = oldArmor;
        item_add_force(inven_dude, item, 1);
        return false;
    }

    adjust_ac(stack[0], oldArmor, item);
    i_worn = item;

    adjust_fid();
    display_stats();
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
    win_draw(i_wid);
    gsound_play_sfx_file("iputdown");
    return true;
}

// Controller quick-action for the player's own (normal) inventory list: a tap of
// A on a list item does the most useful one-shot action without any popup --
// drugs and use-on-self items are used, everything else is equipped into the
// hand box labelled "A" (the left hand). Mirrors inven_action_cursor's NORMAL-window
// USE branch for the use case and inven_equip_under_cursor for the equip case,
// so it respects the inventory screen's working-copy bookkeeping. Hold A / RB->A
// still opens the full Use/Drop/Look popup. Returns true if it acted.
bool inven_gamepad_quick_normal()
{
    if (!normal_inventory_open || i_wid == -1) {
        return false;
    }

    // ⚠️ A ON THE ARMOR SLOT TAKES THE ARMOR OFF (2026-10-10): *"body armour mi nejde sundat"*. Vanilla
    // only undresses by DRAGGING the armor from its slot onto the list, and the pad's A is one click --
    // the drag ended on the slot it started from, so the armor went straight back on and the pad had
    // no way to take it off at all (A on the list already PUTS it on: inven_wear_under_cursor). This
    // mirrors the drag's drop-onto-list branch in inven_pickup: back into the bag, adjust_ac, and if
    // the bag cannot take it the armor stays worn.
    if (inven_normal_cursor_over_armor()) {
        if (i_worn == NULL) {
            return false;
        }

        Object* armor = i_worn;
        i_worn = NULL;
        if (item_add_force(inven_dude, armor, 1) != 0) {
            i_worn = armor;
            gsound_play_sfx_file("iisxxxx1");
            return true;
        }

        adjust_ac(stack[0], armor, NULL);
        adjust_fid();
        display_stats();
        display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
        win_draw(i_wid);
        gsound_play_sfx_file("iputdown");
        return true;
    }

    // Same list region inven_pickup/inven_equip_under_cursor use.
    if (!mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
        return false;
    }

    int x;
    int y;
    mouseGetPositionInWindow(i_wid, &x, &y);

    int displayIndex = (y - 39) / INVENTORY_SLOT_HEIGHT;
    if (displayIndex < 0 || displayIndex >= inven_cur_disp) {
        return false;
    }

    int itemIndex = stack_offset[curr_stack] + displayIndex;
    if (itemIndex >= pud->length) {
        return false;
    }

    int keyCode = 1000 + displayIndex;
    Object* item;
    Object** v43;
    Object* v41;
    if (inven_from_button(keyCode, &item, &v43, &v41) == 0) {
        return false;
    }

    int itemType = item_get_type(item);

    // Ammo loads into a compatible equipped weapon (right hand preferred) rather
    // than going to a hand slot, reusing the engine's drag-ammo-onto-weapon path
    // (loads one ammo object, no quantity prompt). item_w_can_reload also rejects
    // a weapon already holding a DIFFERENT ammo type (it must be unloaded first
    // via the popup), in which case no weapon matches and this is a no-op.
    if (itemType == ITEM_TYPE_AMMO) {
        // Same "active hand first" rule as the equip fallback below: reload the weapon the
        // interface bar is showing, and only fall back to the other hand when that one cannot
        // take this ammo. Only reachable with a reloadable weapon in BOTH hands, but the old
        // hardcoded right-hand preference would then load the gun you are not holding.
        int ammoActive = intface_is_item_right_hand() != 0 ? HAND_RIGHT : HAND_LEFT;
        Object* first = ammoActive == HAND_RIGHT ? i_rhand : i_lhand;
        Object* second = ammoActive == HAND_RIGHT ? i_lhand : i_rhand;

        Object* weapon = NULL;
        if (first != NULL && item_w_can_reload(first, item)) {
            weapon = first;
        } else if (second != NULL && item_w_can_reload(second, item)) {
            weapon = second;
        }

        if (weapon != NULL) {
            drop_ammo_into_weapon(weapon, item, v43, 1, keyCode);
            adjust_fid();
            display_stats();
            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            win_draw(i_wid);
        }
        return true;
    }

    // Armor is WORN (body slot), not dropped into a hand -- a tap of A on armor should
    // put it on. (The hand-equip fallback below is for weapons/everything else.)
    if (itemType == ITEM_TYPE_ARMOR) {
        return inven_wear_under_cursor();
    }

    // Drugs and items you can use ON YOURSELF get used; weapons (and anything else) are equipped
    // into a free hand.
    //
    // ⚠️ proto_action_can_use_ON is deliberately NOT part of this test, though it used to be. A
    // use-ON item -- the rope, a lockpick, a doctor's bag -- cannot do anything from an inventory
    // slot: it needs a TARGET, and the only way to give it one is to hold it and aim the engine's
    // targeting cursor at something. Treating it as "use" meant A on the rope tried to use it on
    // nothing instead of putting it in hand, so there was no way to arm it at all ("provaz se
    // nechce dÃ¡t do Ã¡Äka, protoÅ¾e to bere jako pouÅ¾Ã­t"). can_use (use on SELF) still gets used --
    // that one genuinely works from the list.
    bool useIt = itemType == ITEM_TYPE_DRUG
        || (itemType != ITEM_TYPE_WEAPON && proto_action_can_use(item->pid));

    // HISTORY (report 1 below). A's target used to be "right if it is free, else left", so with
    // BOTH hands empty the item always landed in the right one -- the slot B equips -- no matter
    // which hand the interface bar was actually showing ("mÃ¡m prÃ¡zdnÃ© ruce a kdyÅ¾ kliknu Ã¡Äkem na
    // item, tak se dÃ¡ do BÄka slotu"). It only showed up with empty hands because with one hand
    // occupied the fallback happened to pick the free one, which is what you wanted anyway.
    //
    // ⚠️ WHICH HAND A FILLS: THE ONE WITH "A" PRINTED ON IT. The two hand boxes carry the pad's own
    // hints -- "A" on the LEFT hand, "B" on the RIGHT (display_inventory, the CE hint block) -- and
    // B already equips into the B box. A has to do the same with its box, always, replacing what is
    // in it (that item goes back to the list).
    //
    // Every one of his three reports about this was the same thing, and it took three rounds to see
    // it because the second one was misread:
    //   1. empty hands, A on an item -> it landed in B: the rule was "right if free" (the paragraph
    //      above, kept for its quote);
    //   2. "mám zbraň v A a kliknu na jinou zbraň A a hodí se B! ať nahradí A" -- which I took to mean
    //      "the hand the interface bar shows", and A was made to fill the ACTIVE hand;
    //   3. 2026-10-04, with the bar showing the right hand: "kliknu na zbraň áčkem a dá se mi do
    //      Béčka a to nemám v áčku žádné zbraně".
    // A = the A box. Which hand is ACTIVE is Y's business on the interface bar, not A's.
    if (!useIt) {
        return inven_equip_under_cursor(HAND_LEFT);
    }

    if (itemType == ITEM_TYPE_DRUG) {
        if (item_d_take_drug(stack[0], item)) {
            if (v43 != NULL) {
                *v43 = NULL;
            } else {
                item_remove_mult(v41, item, 1);
            }

            obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
            obj_destroy(item);
        }
        intface_update_hit_points(true);
    } else {
        if (v43 == NULL) {
            item_remove_mult(v41, item, 1);
        }

        int rc;
        if (proto_action_can_use(item->pid)) {
            rc = protinst_use_item(stack[0], item);
        } else {
            rc = protinst_use_item_on(stack[0], stack[0], item);
        }

        if (rc == 1) {
            if (v43 != NULL) {
                *v43 = NULL;
            }

            obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
            obj_destroy(item);
        } else {
            if (v43 == NULL) {
                item_add_force(v41, item, 1);
            }
            inven_place_if_requested(item, v41, v43);
        }
    }

    adjust_fid();
    display_stats();
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
    win_draw(i_wid);
    return true;
}

// 0x464BFC
void switch_hand(Object* a1, Object** a2, Object** a3, int a4)
{
    if (*a2 != NULL) {
        if (item_get_type(*a2) == ITEM_TYPE_WEAPON && item_get_type(a1) == ITEM_TYPE_AMMO) {
            return;
        }

        if (a3 != NULL && (a3 != &i_worn || item_get_type(*a2) == ITEM_TYPE_ARMOR)) {
            if (a3 == &i_worn) {
                adjust_ac(stack[0], i_worn, *a2);
            }
            *a3 = *a2;
        } else {
            if (a4 != -1) {
                item_remove_mult(inven_dude, a1, 1);
            }

            Object* itemToAdd = *a2;
            *a2 = NULL;
            if (item_add_force(inven_dude, itemToAdd, 1) != 0) {
                item_add_force(inven_dude, a1, 1);
                return;
            }

            a4 = -1;

            if (a3 != NULL) {
                if (a3 == &i_worn) {
                    adjust_ac(stack[0], i_worn, NULL);
                }
                *a3 = NULL;
            }
        }
    } else {
        if (a3 != NULL) {
            if (a3 == &i_worn) {
                adjust_ac(stack[0], i_worn, NULL);
            }
            *a3 = NULL;
        }
    }

    *a2 = a1;

    if (a4 != -1) {
        item_remove_mult(inven_dude, a1, 1);
    }
}

// This function removes armor bonuses and effects granted by [oldArmor] and
// adds appropriate bonuses and effects granted by [newArmor]. Both [oldArmor]
// and [newArmor] can be NULL.
//
// 0x464D14
void adjust_ac(Object* critter, Object* oldArmor, Object* newArmor)
{
    if (critter == obj_dude) {
        int armorClassBonus = stat_get_bonus(critter, STAT_ARMOR_CLASS);
        int oldArmorClass = item_ar_ac(oldArmor);
        int newArmorClass = item_ar_ac(newArmor);
        stat_set_bonus(critter, STAT_ARMOR_CLASS, armorClassBonus - oldArmorClass + newArmorClass);

        int damageResistanceStat = STAT_DAMAGE_RESISTANCE;
        int damageThresholdStat = STAT_DAMAGE_THRESHOLD;
        for (int damageType = 0; damageType < DAMAGE_TYPE_COUNT; damageType += 1) {
            int damageResistanceBonus = stat_get_bonus(critter, damageResistanceStat);
            int oldArmorDamageResistance = item_ar_dr(oldArmor, damageType);
            int newArmorDamageResistance = item_ar_dr(newArmor, damageType);
            stat_set_bonus(critter, damageResistanceStat, damageResistanceBonus - oldArmorDamageResistance + newArmorDamageResistance);

            int damageThresholdBonus = stat_get_bonus(critter, damageThresholdStat);
            int oldArmorDamageThreshold = item_ar_dt(oldArmor, damageType);
            int newArmorDamageThreshold = item_ar_dt(newArmor, damageType);
            stat_set_bonus(critter, damageThresholdStat, damageThresholdBonus - oldArmorDamageThreshold + newArmorDamageThreshold);

            damageResistanceStat += 1;
            damageThresholdStat += 1;
        }

        if (oldArmor != NULL) {
            int perk = item_ar_perk(oldArmor);
            perk_remove_effect(critter, perk);
        }

        if (newArmor != NULL) {
            int perk = item_ar_perk(newArmor);
            perk_add_effect(critter, perk);
        }
    }
}

// 0x464E04
void adjust_fid()
{
    int fid;
    if (FID_TYPE(inven_dude->fid) == OBJ_TYPE_CRITTER) {
        Proto* proto;

        int v0 = art_vault_guy_num;

        if (proto_ptr(inven_pid, &proto) == -1) {
            v0 = proto->fid & 0xFFF;
        }

        if (i_worn != NULL) {
            proto_ptr(i_worn->pid, &proto);
            if (stat_level(inven_dude, STAT_GENDER) == GENDER_FEMALE) {
                v0 = proto->item.data.armor.femaleFid;
            } else {
                v0 = proto->item.data.armor.maleFid;
            }

            if (v0 == -1) {
                v0 = art_vault_guy_num;
            }
        }

        int animationCode = 0;
        if (intface_is_item_right_hand()) {
            if (i_rhand != NULL) {
                proto_ptr(i_rhand->pid, &proto);
                if (proto->item.type == ITEM_TYPE_WEAPON) {
                    animationCode = proto->item.data.weapon.animationCode;
                }
            }
        } else {
            if (i_lhand != NULL) {
                proto_ptr(i_lhand->pid, &proto);
                if (proto->item.type == ITEM_TYPE_WEAPON) {
                    animationCode = proto->item.data.weapon.animationCode;
                }
            }
        }

        fid = art_id(OBJ_TYPE_CRITTER, v0, 0, animationCode, 0);
    } else {
        fid = inven_dude->fid;
    }

    i_fid = fid;
}

// 0x464F00
void use_inventory_on(Object* a1)
{
    if (inven_init() == -1) {
        return;
    }

    bool isoWasEnabled = setup_inventory(INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
    for (;;) {
        sharedFpsLimiter.mark();

        if (game_user_wants_to_quit != 0) {
            break;
        }

        display_body(-1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);

        int keyCode = get_input();
        switch (keyCode) {
        case KEY_HOME:
            stack_offset[curr_stack] = 0;
            display_inventory(0, -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        case KEY_ARROW_UP:
            if (stack_offset[curr_stack] > 0) {
                stack_offset[curr_stack] -= 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            }
            break;
        case KEY_PAGE_UP:
            stack_offset[curr_stack] -= inven_cur_disp;
            if (stack_offset[curr_stack] < 0) {
                stack_offset[curr_stack] = 0;
                display_inventory(stack_offset[curr_stack], -1, 1);
            }
            break;
        case KEY_END:
            stack_offset[curr_stack] = pud->length - inven_cur_disp;
            if (stack_offset[curr_stack] < 0) {
                stack_offset[curr_stack] = 0;
            }
            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        case KEY_ARROW_DOWN:
            if (stack_offset[curr_stack] + inven_cur_disp < pud->length) {
                stack_offset[curr_stack] += 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            }
            break;
        case KEY_PAGE_DOWN:
            stack_offset[curr_stack] += inven_cur_disp;
            if (stack_offset[curr_stack] + inven_cur_disp >= pud->length) {
                stack_offset[curr_stack] = pud->length - inven_cur_disp;
                if (stack_offset[curr_stack] < 0) {
                    stack_offset[curr_stack] = 0;
                }
            }
            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        case 2500:
            container_exit(keyCode, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        default:
            if ((mouse_get_buttons() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (immode == INVENTORY_WINDOW_CURSOR_HAND) {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode < 1000 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                    } else {
                        int inventoryItemIndex = stack_offset[curr_stack] + keyCode - 1000;
                        if (inventoryItemIndex < pud->length) {
                            InventoryItem* inventoryItem = &(pud->items[inventoryItemIndex]);
                            action_use_an_item_on_object(stack[0], a1, inventoryItem->item);
                            keyCode = KEY_ESCAPE;
                        } else {
                            keyCode = -1;
                        }
                    }
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(i_wid, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (stack_offset[curr_stack] > 0) {
                            stack_offset[curr_stack] -= 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                        }
                    } else if (wheelY < 0) {
                        if (inven_cur_disp + stack_offset[curr_stack] < pud->length) {
                            stack_offset[curr_stack] += 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                        }
                    }
                }
            }
        }

        if (keyCode == KEY_ESCAPE) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inven_exit();
}

// 0x4650E8
Object* inven_right_hand(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (i_rhand != NULL && critter == inven_dude) {
        return i_rhand;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_IN_RIGHT_HAND) {
            return item;
        }
    }

    return NULL;
}

// 0x465128
Object* inven_left_hand(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (i_lhand != NULL && critter == inven_dude) {
        return i_lhand;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_IN_LEFT_HAND) {
            return item;
        }
    }

    return NULL;
}

// 0x465168
Object* inven_worn(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (i_worn != NULL && critter == inven_dude) {
        return i_worn;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_WORN) {
            return item;
        }
    }

    return NULL;
}

// 0x4651A8
int inven_pid_is_carried(Object* obj, int pid)
{
    Inventory* inventory = &(obj->data.inventory);

    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (inventoryItem->item->pid == pid) {
            return 1;
        }
    }

    return 0;
}

// 0x4651A8
Object* inven_pid_is_carried_ptr(Object* obj, int pid)
{
    Inventory* inventory = &(obj->data.inventory);

    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (inventoryItem->item->pid == pid) {
            return inventoryItem->item;
        }
    }

    return NULL;
}

// 0x465208
int inven_pid_quantity_carried(Object* object, int pid)
{
    int quantity = 0;

    Inventory* inventory = &(object->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (inventoryItem->item->pid == pid) {
            quantity += inventoryItem->quantity;
        }
    }

    return quantity;
}

// Renders character's summary of SPECIAL stats, equipped armor bonuses,
// and weapon's damage/range.
//
// 0x465264
void display_stats()
{
    // 0x4623A0
    static const int stats1[7] = {
        STAT_CURRENT_HIT_POINTS,
        STAT_ARMOR_CLASS,
        STAT_DAMAGE_THRESHOLD,
        STAT_DAMAGE_THRESHOLD_LASER,
        STAT_DAMAGE_THRESHOLD_FIRE,
        STAT_DAMAGE_THRESHOLD_PLASMA,
        STAT_DAMAGE_THRESHOLD_EXPLOSION,
    };

    // 0x4623BC
    static const int stats2[7] = {
        STAT_MAXIMUM_HIT_POINTS,
        -1,
        STAT_DAMAGE_RESISTANCE,
        STAT_DAMAGE_RESISTANCE_LASER,
        STAT_DAMAGE_RESISTANCE_FIRE,
        STAT_DAMAGE_RESISTANCE_PLASMA,
        STAT_DAMAGE_RESISTANCE_EXPLOSION,
    };

    char formattedText[80];

    int oldFont = text_curr();
    text_font(101);

    unsigned char* windowBuffer = win_get_buf(i_wid);

    int fid = art_id(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);

    CacheEntry* backgroundHandle;
    unsigned char* backgroundData = art_ptr_lock_data(fid, 0, 0, &backgroundHandle);
    if (backgroundData != NULL) {
        buf_to_buf(backgroundData + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            152,
            188,
            INVENTORY_WINDOW_WIDTH,
            windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            INVENTORY_WINDOW_WIDTH);
    }
    art_ptr_unlock(backgroundHandle);

    // Render character name.
    const char* critterName = critter_name(stack[0]);
    text_to_buf(windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X, critterName, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);

    draw_line(windowBuffer,
        INVENTORY_WINDOW_WIDTH,
        INVENTORY_SUMMARY_X,
        3 * text_height() / 2 + INVENTORY_SUMMARY_Y,
        INVENTORY_SUMMARY_MAX_X,
        3 * text_height() / 2 + INVENTORY_SUMMARY_Y,
        colorTable[992]);

    MessageListItem messageListItem;

    int offset = INVENTORY_WINDOW_WIDTH * 2 * text_height() + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X;
    for (int stat = 0; stat < 7; stat++) {
        messageListItem.num = stat;
        if (message_search(&inventry_message_file, &messageListItem)) {
            text_to_buf(windowBuffer + offset, messageListItem.text, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        }

        int value = stat_level(stack[0], stat);
        snprintf(formattedText, sizeof(formattedText), "%d", value);
        text_to_buf(windowBuffer + offset + 24, formattedText, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * text_height();
    }

    offset -= INVENTORY_WINDOW_WIDTH * 7 * text_height();

    for (int index = 0; index < 7; index += 1) {
        messageListItem.num = 7 + index;
        if (message_search(&inventry_message_file, &messageListItem)) {
            text_to_buf(windowBuffer + offset + 40, messageListItem.text, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        }

        if (stats2[index] == -1) {
            int value = stat_level(stack[0], stats1[index]);
            snprintf(formattedText, sizeof(formattedText), "   %d", value);
        } else {
            int value1 = stat_level(stack[0], stats1[index]);
            int value2 = stat_level(stack[0], stats2[index]);
            const char* format = index != 0 ? "%d/%d%%" : "%d/%d";
            snprintf(formattedText, sizeof(formattedText), format, value1, value2);
        }

        text_to_buf(windowBuffer + offset + 104, formattedText, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * text_height();
    }

    draw_line(windowBuffer, INVENTORY_WINDOW_WIDTH, INVENTORY_SUMMARY_X, 18 * text_height() / 2 + 48, INVENTORY_SUMMARY_MAX_X, 18 * text_height() / 2 + 48, colorTable[992]);
    draw_line(windowBuffer, INVENTORY_WINDOW_WIDTH, INVENTORY_SUMMARY_X, 26 * text_height() / 2 + 48, INVENTORY_SUMMARY_MAX_X, 26 * text_height() / 2 + 48, colorTable[992]);

    Object* itemsInHands[2] = {
        i_lhand,
        i_rhand,
    };

    const int hitModes[2] = {
        HIT_MODE_LEFT_WEAPON_PRIMARY,
        HIT_MODE_RIGHT_WEAPON_PRIMARY,
    };

    offset += INVENTORY_WINDOW_WIDTH * text_height();

    for (int index = 0; index < 2; index += 1) {
        Object* item = itemsInHands[index];
        if (item == NULL) {
            formattedText[0] = '\0';

            // No item
            messageListItem.num = 14;
            if (message_search(&inventry_message_file, &messageListItem)) {
                text_to_buf(windowBuffer + offset, messageListItem.text, 120, INVENTORY_WINDOW_WIDTH, colorTable[992]);
            }

            offset += INVENTORY_WINDOW_WIDTH * text_height();

            // Unarmed dmg:
            messageListItem.num = 24;
            if (message_search(&inventry_message_file, &messageListItem)) {
                // TODO: Figure out why it uses STAT_MELEE_DAMAGE instead of
                // STAT_UNARMED_DAMAGE.
                int damage = stat_level(stack[0], STAT_MELEE_DAMAGE) + 2;
                snprintf(formattedText, sizeof(formattedText), "%s 1-%d", messageListItem.text, damage);
            }

            text_to_buf(windowBuffer + offset, formattedText, 120, INVENTORY_WINDOW_WIDTH, colorTable[992]);

            offset += 3 * INVENTORY_WINDOW_WIDTH * text_height();
            continue;
        }

        const char* itemName = item_name(item);
        text_to_buf(windowBuffer + offset, itemName, 140, INVENTORY_WINDOW_WIDTH, colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * text_height();

        int itemType = item_get_type(item);
        if (itemType != ITEM_TYPE_WEAPON) {
            if (itemType == ITEM_TYPE_ARMOR) {
                // (Not worn)
                messageListItem.num = 18;
                if (message_search(&inventry_message_file, &messageListItem)) {
                    text_to_buf(windowBuffer + offset, messageListItem.text, 120, INVENTORY_WINDOW_WIDTH, colorTable[992]);
                }
            }

            offset += 3 * INVENTORY_WINDOW_WIDTH * text_height();
            continue;
        }

        int range = item_w_range(stack[0], hitModes[index]);

        int damageMin;
        int damageMax;
        item_w_damage_min_max(item, &damageMin, &damageMax);

        int attackType = item_w_subtype(item, hitModes[index]);

        formattedText[0] = '\0';

        int meleeDamage;
        if (attackType == ATTACK_TYPE_MELEE || attackType == ATTACK_TYPE_UNARMED) {
            meleeDamage = stat_level(stack[0], STAT_MELEE_DAMAGE);
        } else {
            meleeDamage = 0;
        }

        messageListItem.num = 15; // Dmg:
        if (message_search(&inventry_message_file, &messageListItem)) {
            if (attackType == ATTACK_TYPE_RANGED || range > 1) {
                MessageListItem rangeMessageListItem;
                rangeMessageListItem.num = 16; // Rng:
                if (message_search(&inventry_message_file, &rangeMessageListItem)) {
                    snprintf(formattedText, sizeof(formattedText), "%s %d-%d   %s %d", messageListItem.text, damageMin, damageMax + meleeDamage, rangeMessageListItem.text, range);
                }
            } else {
                snprintf(formattedText, sizeof(formattedText), "%s %d-%d", messageListItem.text, damageMin, damageMax + meleeDamage);
            }

            text_to_buf(windowBuffer + offset, formattedText, 140, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        }

        offset += INVENTORY_WINDOW_WIDTH * text_height();

        if (item_w_max_ammo(item) > 0) {
            int ammoTypePid = item_w_ammo_pid(item);

            formattedText[0] = '\0';

            messageListItem.num = 17; // Ammo:
            if (message_search(&inventry_message_file, &messageListItem)) {
                if (ammoTypePid != 0) {
                    const char* ammoName = proto_name(ammoTypePid);
                    int capacity = item_w_max_ammo(item);
                    int quantity = item_w_curr_ammo(item);
                    snprintf(formattedText, sizeof(formattedText), "%s %d/%d %s", messageListItem.text, quantity, capacity, ammoName);
                } else {
                    int capacity = item_w_max_ammo(item);
                    int quantity = item_w_curr_ammo(item);
                    snprintf(formattedText, sizeof(formattedText), "%s %d/%d", messageListItem.text, quantity, capacity);
                }
            }

            text_to_buf(windowBuffer + offset, formattedText, 140, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        }

        offset += 2 * INVENTORY_WINDOW_WIDTH * text_height();
    }

    // Total wt:
    messageListItem.num = 20;
    if (message_search(&inventry_message_file, &messageListItem)) {
        if (PID_TYPE(stack[0]->pid) == OBJ_TYPE_CRITTER) {
            int carryWeight = stat_level(stack[0], STAT_CARRY_WEIGHT);
            int inventoryWeight = item_total_weight(stack[0]);
            snprintf(formattedText, sizeof(formattedText), "%s %d/%d", messageListItem.text, inventoryWeight, carryWeight);

            text_to_buf(windowBuffer + offset + 15, formattedText, 120, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        } else {
            int inventoryWeight = item_total_weight(stack[0]);
            snprintf(formattedText, sizeof(formattedText), "%s %d", messageListItem.text, inventoryWeight);

            text_to_buf(windowBuffer + offset + 30, formattedText, 80, INVENTORY_WINDOW_WIDTH, colorTable[992]);
        }
    }

    text_font(oldFont);
}

// Finds next item of given [itemType] (can be -1 which means any type of
// item).
//
// The [index] is used to control where to continue the search from, -1 - from
// the beginning.
//
// 0x465AF0
Object* inven_find_type(Object* obj, int itemType, int* indexPtr)
{
    int dummy = -1;
    if (indexPtr == NULL) {
        indexPtr = &dummy;
    }

    *indexPtr += 1;

    Inventory* inventory = &(obj->data.inventory);

    // TODO: Refactor with for loop.
    if (*indexPtr >= inventory->length) {
        return NULL;
    }

    while (itemType != -1 && item_get_type(inventory->items[*indexPtr].item) != itemType) {
        *indexPtr += 1;

        if (*indexPtr >= inventory->length) {
            return NULL;
        }
    }

    return inventory->items[*indexPtr].item;
}

// 0x465B44
Object* inven_find_id(Object* obj, int id)
{
    if (obj->id == id) {
        return obj;
    }

    Inventory* inventory = &(obj->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        Object* item = inventoryItem->item;
        if (item->id == id) {
            return item;
        }

        if (item_get_type(item) == ITEM_TYPE_CONTAINER) {
            item = inven_find_id(item, id);
            if (item != NULL) {
                return item;
            }
        }
    }

    return NULL;
}

// 0x465B98
// Vanilla's answer to an armed explosive put down in front of people -- it ran as the inventory
// closed after one was dropped: everyone of another team inside the blast who notices it (a
// Perception roll) takes it as an attack by the dude, and the first of them starts the fight. Out
// here so the quick menu's "put it down" (gamepad_ui, 2026-10-16) answers the same way.
void inven_explosive_dropped_notice()
{
    Attack v1;
    combat_ctd_init(&v1, obj_dude, NULL, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
    v1.attackerFlags = DAM_HIT;
    v1.tile = obj_dude->tile;
    compute_explosion_on_extras(&v1, 0, 0, 1);

    Object* v2 = NULL;
    for (int index = 0; index < v1.extrasLength; index++) {
        Object* critter = v1.extras[index];
        if (critter != obj_dude
            && critter->data.critter.combat.team != obj_dude->data.critter.combat.team
            && stat_result(critter, STAT_PERCEPTION, 0, NULL) >= ROLL_SUCCESS) {
            critter_set_who_hit_me(critter, obj_dude);

            if (v2 == NULL) {
                v2 = critter;
            }
        }
    }

    if (v2 != NULL) {
        if (!isInCombat()) {
            STRUCT_664980 v3;
            v3.attacker = v2;
            v3.defender = obj_dude;
            v3.actionPointsBonus = 0;
            v3.accuracyBonus = 0;
            v3.damageBonus = 0;
            v3.minDamage = 0;
            v3.maxDamage = INT_MAX;
            v3.field_1C = 0;
            scripts_request_combat(&v3);
        }
    }

}

static int inven_wield_func(Object* critter, Object* item, int a3, bool animate);

int inven_wield(Object* critter, Object* item, int a3)
{
    return inven_wield_func(critter, item, a3, true);
}

// inven_wield without the ceremony: no holstering of what the hand held, no take-out of the new one
// -- the stance changes at once. For the quick slots' grenade draw (2026-10-16, his pick "Bez
// schování": the put-away was ~0.6 s of nothing between the press and the throw).
int inven_wield_instant(Object* critter, Object* item, int a3)
{
    return inven_wield_func(critter, item, a3, false);
}

static int inven_wield_func(Object* critter, Object* item, int a3, bool animate)
{
    register_begin(ANIMATION_REQUEST_RESERVED);

    int itemType = item_get_type(item);
    if (itemType == ITEM_TYPE_ARMOR) {
        Object* armor = inven_worn(critter);
        if (armor != NULL) {
            armor->flags &= ~OBJECT_WORN;
        }

        item->flags |= OBJECT_WORN;

        int baseFrmId;
        if (stat_level(critter, STAT_GENDER) == GENDER_FEMALE) {
            baseFrmId = item_ar_female_fid(item);
        } else {
            baseFrmId = item_ar_male_fid(item);
        }

        if (baseFrmId == -1) {
            baseFrmId = 1;
        }

        if (critter == obj_dude) {
            int fid = art_id(OBJ_TYPE_CRITTER, baseFrmId, 0, (critter->fid & 0xF000) >> 12, critter->rotation + 1);
            register_object_change_fid(critter, fid, 0);
        } else {
            adjust_ac(critter, armor, item);
        }
    } else {
        int hand;
        if (critter == obj_dude) {
            hand = intface_is_item_right_hand();
        } else {
            hand = HAND_RIGHT;
        }

        int weaponAnimationCode = item_w_anim_code(item);
        int hitModeAnimationCode = item_w_anim_weap(item, HIT_MODE_RIGHT_WEAPON_PRIMARY);
        int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, hitModeAnimationCode, weaponAnimationCode, critter->rotation + 1);
        if (!art_exists(fid)) {
            debug_printf("\ninven_wield failed!  ERROR ERROR ERROR!");
            return -1;
        }

        Object* v17;
        if (a3) {
            v17 = inven_right_hand(critter);
            item->flags |= OBJECT_IN_RIGHT_HAND;
        } else {
            v17 = inven_left_hand(critter);
            item->flags |= OBJECT_IN_LEFT_HAND;
        }

        Rect rect;
        if (v17 != NULL) {
            v17->flags &= ~OBJECT_IN_ANY_HAND;

            if (v17->pid == PROTO_ID_LIT_FLARE) {
                int lightIntensity;
                int lightDistance;
                if (critter == obj_dude) {
                    lightIntensity = LIGHT_LEVEL_MAX;
                    lightDistance = 4;
                } else {
                    Proto* proto;
                    if (proto_ptr(critter->pid, &proto) == -1) {
                        return -1;
                    }

                    lightDistance = proto->lightDistance;
                    lightIntensity = proto->lightIntensity;
                }

                obj_set_light(critter, lightDistance, lightIntensity, &rect);
            }
        }

        if (item->pid == PROTO_ID_LIT_FLARE) {
            int lightDistance = item->lightDistance;
            if (lightDistance < critter->lightDistance) {
                lightDistance = critter->lightDistance;
            }

            int lightIntensity = item->lightIntensity;
            if (lightIntensity < critter->lightIntensity) {
                lightIntensity = critter->lightIntensity;
            }

            obj_set_light(critter, lightDistance, lightIntensity, &rect);
            tile_refresh_rect(&rect, map_elevation);
        }

        if (item_get_type(item) == ITEM_TYPE_WEAPON) {
            weaponAnimationCode = item_w_anim_code(item);
        } else {
            weaponAnimationCode = 0;
        }

        if (hand == a3) {
            if (animate && (critter->fid & 0xF000) >> 12 != 0) {
                const char* soundEffectName = gsnd_build_character_sfx_name(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
                register_object_play_sfx(critter, soundEffectName, 0);
                register_object_animate(critter, ANIM_PUT_AWAY, 0);
            }

            if (!animate) {
                int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_STAND, weaponAnimationCode, critter->rotation + 1);
                register_object_change_fid(critter, fid, -1);
            } else if (weaponAnimationCode != 0) {
                register_object_take_out(critter, weaponAnimationCode, -1);
            } else {
                int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, 0, critter->rotation + 1);
                register_object_change_fid(critter, fid, -1);
            }
        }
    }

    return register_end();
}

// 0x465D10
int inven_unwield(Object* critter, int a2)
{
    int hand;
    Object* item;
    int fid;

    if (critter == obj_dude) {
        hand = intface_is_item_right_hand();
    } else {
        hand = 1;
    }

    if (a2) {
        item = inven_right_hand(critter);
    } else {
        item = inven_left_hand(critter);
    }

    if (item) {
        item->flags &= ~OBJECT_IN_ANY_HAND;
    }

    if (hand == a2 && ((critter->fid & 0xF000) >> 12) != 0) {
        register_begin(ANIMATION_REQUEST_RESERVED);

        const char* sfx = gsnd_build_character_sfx_name(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(critter, sfx, 0);

        register_object_animate(critter, ANIM_PUT_AWAY, 0);

        fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, 0, critter->rotation + 1);
        register_object_change_fid(critter, fid, -1);

        return register_end();
    }

    return 0;
}

// 0x465DC4
int inven_from_button(int keyCode, Object** a2, Object*** a3, Object** a4)
{
    Object** slot_ptr;
    Object* owner;
    Object* item;
    int quantity = 0;
    int index;
    InventoryItem* inventoryItem;

    switch (keyCode) {
    case 1006:
        slot_ptr = &i_rhand;
        owner = stack[0];
        item = i_rhand;
        break;
    case 1007:
        slot_ptr = &i_lhand;
        owner = stack[0];
        item = i_lhand;
        break;
    case 1008:
        slot_ptr = &i_worn;
        owner = stack[0];
        item = i_worn;
        break;
    default:
        slot_ptr = NULL;
        owner = NULL;
        item = NULL;

        if (keyCode < 2000) {
            index = stack_offset[curr_stack] + keyCode - 1000;
            if (index >= pud->length) {
                break;
            }

            inventoryItem = &(pud->items[index]);
            item = inventoryItem->item;
            owner = stack[curr_stack];
        } else if (keyCode < 2300) {
            index = target_stack_offset[target_curr_stack] + keyCode - 2000;
            if (index >= target_pud->length) {
                break;
            }

            inventoryItem = &(target_pud->items[index]);
            item = inventoryItem->item;
            owner = target_stack[target_curr_stack];
        } else if (keyCode < 2400) {
            index = ptable_offset + keyCode - 2300;
            if (index >= ptable_pud->length) {
                break;
            }

            inventoryItem = &(ptable_pud->items[index]);
            item = inventoryItem->item;
            owner = ptable;
        } else {
            index = btable_offset + keyCode - 2400;
            if (index >= btable_pud->length) {
                break;
            }

            inventoryItem = &(btable_pud->items[index]);
            item = inventoryItem->item;
            owner = btable;
        }

        quantity = inventoryItem->quantity;
    }

    if (a3 != NULL) {
        *a3 = slot_ptr;
    }

    if (a2 != NULL) {
        *a2 = item;
    }

    if (a4 != NULL) {
        *a4 = owner;
    }

    if (quantity == 0 && item != NULL) {
        quantity = 1;
    }

    return quantity;
}

// Displays item description.
//
// The [string] is mutated in the process replacing spaces back and forth
// for word wrapping purposes.
//
// 0x465F74
void inven_display_msg(char* string)
{
    int oldFont = text_curr();
    text_font(101);

    unsigned char* windowBuffer = win_get_buf(i_wid);
    windowBuffer += 499 * 44 + 297;

    char* c = string;
    while (c != NULL && *c != '\0') {
        inven_display_msg_line += 1;
        if (inven_display_msg_line > 17) {
            debug_printf("\nError: inven_display_msg: out of bounds!");
            return;
        }

        char* space = NULL;
        if (text_width(c) > 152) {
            // Look for next space.
            space = c + 1;
            while (*space != '\0' && *space != ' ') {
                space += 1;
            }

            if (*space == '\0') {
                // This was the last line containing very long word. Text
                // drawing routine will silently truncate it after reaching
                // desired length.
                text_to_buf(windowBuffer + 499 * inven_display_msg_line * text_height(), c, 152, 499, colorTable[992]);
                return;
            }

            char* nextSpace = space + 1;
            while (true) {
                while (*nextSpace != '\0' && *nextSpace != ' ') {
                    nextSpace += 1;
                }

                if (*nextSpace == '\0') {
                    break;
                }

                // Break string and measure it.
                *nextSpace = '\0';
                if (text_width(c) >= 152) {
                    // Next space is too far to fit in one line. Restore next
                    // space's character and stop.
                    *nextSpace = ' ';
                    break;
                }

                space = nextSpace;

                // Restore next space's character and continue looping from the
                // next character.
                *nextSpace = ' ';
                nextSpace += 1;
            }

            if (*space == ' ') {
                *space = '\0';
            }
        }

        if (text_width(c) > 152) {
            debug_printf("\nError: inven_display_msg: word too long!");
            return;
        }

        text_to_buf(windowBuffer + 499 * inven_display_msg_line * text_height(), c, 152, 499, colorTable[992]);

        if (space != NULL) {
            c = space + 1;
            if (*space == '\0') {
                *space = ' ';
            }
        } else {
            c = NULL;
        }
    }

    text_font(oldFont);
}

// Examines inventory item.
//
// 0x466108
void inven_obj_examine_func(Object* critter, Object* item)
{
    int oldFont = text_curr();
    text_font(101);

    unsigned char* windowBuffer = win_get_buf(i_wid);

    // Clear item description area.
    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);

    CacheEntry* handle;
    unsigned char* backgroundData = art_ptr_lock_data(backgroundFid, 0, 0, &handle);
    if (backgroundData != NULL) {
        buf_to_buf(backgroundData + 499 * 44 + 297, 152, 188, 499, windowBuffer + 499 * 44 + 297, 499);
    }
    art_ptr_unlock(handle);

    // Reset item description lines counter.
    inven_display_msg_line = 0;

    // Render item's name.
    char* itemName = object_name(item);
    inven_display_msg(itemName);

    // Increment line counter to accomodate separator below.
    inven_display_msg_line += 1;

    int lineHeight = text_height();

    // Draw separator.
    draw_line(windowBuffer,
        499,
        297,
        3 * lineHeight / 2 + 49,
        440,
        3 * lineHeight / 2 + 49,
        colorTable[992]);

    // Examine item.
    obj_examine_func(critter, item, inven_display_msg);

    // Add weight if neccessary.
    int weight = item_weight(item);
    if (weight != 0) {
        MessageListItem messageListItem;
        messageListItem.num = 540;

        if (weight == 1) {
            messageListItem.num = 541;
        }

        if (!message_search(&proto_main_msg_file, &messageListItem)) {
            debug_printf("\nError: Couldn't find message!");
        }

        char formattedText[40];
        snprintf(formattedText, sizeof(formattedText), messageListItem.text, weight);
        inven_display_msg(formattedText);
    }

    text_font(oldFont);
}

// 0x46629C
void inven_action_cursor(int keyCode, int inventoryWindowType)
{
    // 0x5056EC
    static int act_use[4] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_USE,
        GAME_MOUSE_ACTION_MENU_ITEM_DROP,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    // 0x5056FC
    static int act_no_use[3] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_DROP,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    // 0x505708
    static int act_just_use[3] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_USE,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    // 0x505714
    static int act_nothing[2] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    // 0x50571C
    static int act_weap[4] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD,
        GAME_MOUSE_ACTION_MENU_ITEM_DROP,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    // 0x50572C
    static int act_weap2[3] = {
        GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
        GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD,
        GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
    };

    Object* item;
    Object** v43;
    Object* v41;

    int v56 = inven_from_button(keyCode, &item, &v43, &v41);
    if (v56 == 0) {
        return;
    }

    int itemType = item_get_type(item);

    int mouseState;
    do {
        sharedFpsLimiter.mark();

        get_input();

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
            display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);
        }

        mouseState = mouse_get_buttons();
        if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
            if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
                obj_look_at_func(stack[0], item, display_msg);
            } else {
                inven_obj_examine_func(stack[0], item);
            }
            win_draw(i_wid);
            return;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseState & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) != MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT);

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_BLANK);

    unsigned char* windowBuffer = win_get_buf(i_wid);

    int x;
    int y;
    mouse_get_position(&x, &y);

    int actionMenuItemsLength;
    const int* actionMenuItems;
    if (itemType == ITEM_TYPE_WEAPON && item_w_can_unload(item)) {
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL && obj_top_environment(item) != obj_dude) {
            actionMenuItemsLength = 3;
            actionMenuItems = act_weap2;
        } else {
            actionMenuItemsLength = 4;
            actionMenuItems = act_weap;
        }
    } else {
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
            if (obj_top_environment(item) != obj_dude) {
                if (itemType == ITEM_TYPE_CONTAINER) {
                    actionMenuItemsLength = 3;
                    actionMenuItems = act_just_use;
                } else {
                    actionMenuItemsLength = 2;
                    actionMenuItems = act_nothing;
                }
            } else {
                if (itemType == ITEM_TYPE_CONTAINER) {
                    actionMenuItemsLength = 4;
                    actionMenuItems = act_use;
                } else {
                    actionMenuItemsLength = 3;
                    actionMenuItems = act_no_use;
                }
            }
        } else {
            if (itemType == ITEM_TYPE_CONTAINER && v43 != NULL) {
                actionMenuItemsLength = 3;
                actionMenuItems = act_no_use;
            } else {
                if (proto_action_can_use(item->pid) || proto_action_can_use_on(item->pid)) {
                    actionMenuItemsLength = 4;
                    actionMenuItems = act_use;
                } else {
                    actionMenuItemsLength = 3;
                    actionMenuItems = act_no_use;
                }
            }
        }
    }

    InventoryWindowDescription* windowDescription = &(iscr_data[inventoryWindowType]);

    Rect windowRect;
    win_get_rect(i_wid, &windowRect);
    int inventoryWindowX = windowRect.ulx;
    int inventoryWindowY = windowRect.uly;

    gmouse_3d_build_menu_frame(x, y, actionMenuItems, actionMenuItemsLength,
        windowDescription->width + inventoryWindowX,
        windowDescription->height + inventoryWindowY);

    InventoryCursorData* cursorData = &(imdata[INVENTORY_WINDOW_CURSOR_MENU]);

    int offsetX;
    int offsetY;
    art_frame_offset(cursorData->frm, 0, &offsetX, &offsetY);

    Rect rect;
    rect.ulx = x - inventoryWindowX - cursorData->width / 2 + offsetX;
    rect.uly = y - inventoryWindowY - cursorData->height + 1 + offsetY;
    rect.lrx = rect.ulx + cursorData->width - 1;
    rect.lry = rect.uly + cursorData->height - 1;

    int menuButtonHeight = cursorData->height;
    if (rect.uly + menuButtonHeight > windowDescription->height) {
        menuButtonHeight = windowDescription->height - rect.uly;
    }

    int btn = win_register_button(i_wid,
        rect.ulx,
        rect.uly,
        cursorData->width,
        menuButtonHeight,
        -1,
        -1,
        -1,
        -1,
        cursorData->frmData,
        cursorData->frmData,
        0,
        BUTTON_FLAG_TRANSPARENT);
    win_draw_rect(i_wid, &rect);

    int menuItemIndex = 0;
    int previousMouseY = y;
    // Tell the gamepad layer this hold-drag popup is up so the left stick feeds
    // mouse-Y motion (the only way to navigate this non-GNW menu).
    gamepad_ui_set_action_popup_active(true);
    while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_UP) == 0) {
        sharedFpsLimiter.mark();

        get_input();

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
            display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);
        }

        // D-pad up/down steps the highlighted option directly -- more precise
        // than the analog stick for a short menu list. Confirm is still A
        // (left-button up, which exits this loop).
        int dpad = gamepad_ui_action_popup_dpad();
        if (dpad < 0 && menuItemIndex > 0) {
            menuItemIndex--;
            gmouse_3d_highlight_menu_frame(menuItemIndex);
            win_draw_rect(i_wid, &rect);
        } else if (dpad > 0 && menuItemIndex < actionMenuItemsLength - 1) {
            menuItemIndex++;
            gmouse_3d_highlight_menu_frame(menuItemIndex);
            win_draw_rect(i_wid, &rect);
        }

        int x;
        int y;
        mouse_get_position(&x, &y);
        if (y - previousMouseY > 10 || previousMouseY - y > 10) {
            if (y >= previousMouseY || menuItemIndex <= 0) {
                if (previousMouseY < y && menuItemIndex < actionMenuItemsLength - 1) {
                    menuItemIndex++;
                }
            } else {
                menuItemIndex--;
            }
            gmouse_3d_highlight_menu_frame(menuItemIndex);
            win_draw_rect(i_wid, &rect);
            previousMouseY = y;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }
    gamepad_ui_set_action_popup_active(false);

    win_delete_button(btn);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        unsigned char* src = win_get_buf(barter_back_win);
        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        buf_to_buf(src + pitch * rect.uly + rect.ulx + INVENTORY_TRADE_WINDOW_OFFSET,
            cursorData->width,
            menuButtonHeight,
            pitch,
            windowBuffer + windowDescription->width * rect.uly + rect.ulx,
            windowDescription->width);
    } else {
        int backgroundFid = art_id(OBJ_TYPE_INTERFACE, windowDescription->field_0, 0, 0, 0);
        CacheEntry* backgroundFrmHandle;
        unsigned char* backgroundFrmData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundFrmHandle);
        buf_to_buf(backgroundFrmData + windowDescription->width * rect.uly + rect.ulx,
            cursorData->width,
            menuButtonHeight,
            windowDescription->width,
            windowBuffer + windowDescription->width * rect.uly + rect.ulx,
            windowDescription->width);
        art_ptr_unlock(backgroundFrmHandle);
    }

    mouse_set_position(x, y);

    display_inventory(stack_offset[curr_stack], -1, inventoryWindowType);

    int actionMenuItem = actionMenuItems[menuItemIndex];
    switch (actionMenuItem) {
    case GAME_MOUSE_ACTION_MENU_ITEM_DROP:
        if (v43 != NULL) {
            if (v43 == &i_worn) {
                adjust_ac(stack[0], item, NULL);
            }
            item_add_force(v41, item, 1);
            v56 = 1;
            *v43 = NULL;
        }

        if (item->pid == PROTO_ID_MONEY) {
            if (v56 > 1) {
                v56 = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, v56);
            } else {
                v56 = 1;
            }

            if (v56 > 0) {
                if (v56 == 1) {
                    item_caps_set_amount(item, 1);
                    obj_drop(v41, item);
                } else {
                    if (item_remove_mult(v41, item, v56 - 1) == 0) {
                        Object* a2;
                        if (inven_from_button(keyCode, &a2, &v43, &v41) != 0) {
                            item_caps_set_amount(a2, v56);
                            obj_drop(v41, a2);
                        } else {
                            item_add_force(v41, item, v56 - 1);
                        }
                    }
                }
            }
        } else if (item->pid == PROTO_ID_DYNAMITE_II || item->pid == PROTO_ID_PLASTIC_EXPLOSIVES_II) {
            dropped_explosive = 1;
            obj_drop(v41, item);
        } else {
            if (v56 > 1) {
                v56 = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, v56);

                for (int index = 0; index < v56; index++) {
                    if (inven_from_button(keyCode, &item, &v43, &v41) != 0) {
                        obj_drop(v41, item);
                    }
                }
            } else {
                obj_drop(v41, item);
            }
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_LOOK:
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
            obj_examine_func(stack[0], item, display_msg);
        } else {
            inven_obj_examine_func(stack[0], item);
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_USE:
        switch (itemType) {
        case ITEM_TYPE_CONTAINER:
            container_enter(keyCode, inventoryWindowType);
            break;
        case ITEM_TYPE_DRUG:
            if (item_d_take_drug(stack[0], item)) {
                if (v43 != NULL) {
                    *v43 = NULL;
                } else {
                    item_remove_mult(v41, item, 1);
                }

                obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
                obj_destroy(item);
            }
            intface_update_hit_points(true);
            break;
        case ITEM_TYPE_WEAPON:
        case ITEM_TYPE_MISC:
            if (v43 == NULL) {
                item_remove_mult(v41, item, 1);
            }

            int v21;
            if (proto_action_can_use(item->pid)) {
                v21 = protinst_use_item(stack[0], item);
            } else {
                v21 = protinst_use_item_on(stack[0], stack[0], item);
            }

            if (v21 == 1) {
                if (v43 != NULL) {
                    *v43 = NULL;
                }

                obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
                obj_destroy(item);
            } else {
                if (v43 == NULL) {
                    item_add_force(v41, item, 1);
                }
                inven_place_if_requested(item, v41, v43);
            }
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD:
        if (v43 == NULL) {
            item_remove_mult(v41, item, 1);
        }

        for (;;) {
            Object* ammo = item_w_unload(item);
            if (ammo == NULL) {
                break;
            }

            Rect rect;
            obj_disconnect(ammo, &rect);
            item_add_force(v41, ammo, 1);
        }

        if (v43 == NULL) {
            item_add_force(v41, item, 1);
        }
        break;
    default:
        break;
    }

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL && actionMenuItem != GAME_MOUSE_ACTION_MENU_ITEM_LOOK) {
        display_stats();
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT
        || inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, inventoryWindowType);
    }

    display_inventory(stack_offset[curr_stack], -1, inventoryWindowType);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        display_table_inventories(barter_back_win, ptable, btable, -1);
    }

    adjust_fid();
}

static void barter_warp_cursor(int sx, int sy);
static void barter_draw_deal();

// CE: loot-window panel jump. Pushing the left stick / D-pad LEFT snaps the pointer to the
// first (top) slot of YOUR inventory (left panel); RIGHT snaps it to the first slot of the
// container/corpse (right panel). Up/down within a panel is still the shared cursor snap
// (gpui_menu_snap); this only overrides the left/right cross-panel jump so entering a side
// always lands on its first item instead of whatever row was nearest. Read SDL directly --
// gamepad_ui_tick doesn't run inside loot_container's modal loop. Runs after get_input, so its
// warp wins over gpui's same-frame horizontal snap.
static void loot_gamepad_panel_jump(int i_wid)
{
    static bool prevL = false;
    static bool prevR = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevL = false;
        prevR = false;
        return;
    }

    SDL_GameControllerUpdate();
    Sint16 lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    bool lNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0 || lx < -16000;
    bool rNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0 || lx > 16000;

    Rect winRect;
    win_get_rect(i_wid, &winRect);

    if (lNow && !prevL) {
        barter_warp_cursor(winRect.ulx + INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2,
            winRect.uly + INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT / 2);
    } else if (rNow && !prevR) {
        barter_warp_cursor(winRect.ulx + INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2,
            winRect.uly + INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT / 2);
    }

    prevL = lNow;
    prevR = rNow;
}

// 0x466B10
int loot_container(Object* a1, Object* a2)
{
    // 0x46E708
    static const int arrowFrmIds[INVENTORY_ARROW_FRM_COUNT] = {
        122, // left arrow up
        123, // left arrow down
        124, // right arrow up
        125, // right arrow down
    };

    CacheEntry* arrowFrmHandles[INVENTORY_ARROW_FRM_COUNT];
    MessageListItem messageListItem;

    if (a1 != inven_dude) {
        return 0;
    }

    if (FID_TYPE(a2->fid) == OBJ_TYPE_ITEM) {
        if (item_get_type(a2) == ITEM_TYPE_CONTAINER) {
            if (a2->frame == 0) {
                CacheEntry* handle;
                Art* frm = art_ptr_lock(a2->fid, &handle);
                if (frm != NULL) {
                    int frameCount = art_frame_max_frame(frm);
                    art_ptr_unlock(handle);
                    if (frameCount > 1) {
                        return 0;
                    }
                }
            }
        }
    }

    int sid = -1;
    if (!gIsSteal) {
        if (obj_sid(a2, &sid) != -1) {
            scr_set_objs(sid, a1, NULL);
            exec_script_proc(sid, SCRIPT_PROC_PICKUP);

            Script* script;
            if (scr_ptr(sid, &script) != -1) {
                if (script->scriptOverrides) {
                    return 0;
                }
            }
        }
    }

    if (inven_init() == -1) {
        return 0;
    }

    target_pud = &(a2->data.inventory);
    target_curr_stack = 0;
    target_stack_offset[0] = 0;
    target_stack[0] = a2;

    Object* a1a = NULL;
    if (obj_new(&a1a, 0, 467) == -1) {
        return 0;
    }

    Object* item1 = NULL;
    Object* item2 = NULL;
    Object* armor = NULL;

    if (gIsSteal) {
        item1 = inven_left_hand(a2);
        if (item1 != NULL) {
            item_remove_mult(a2, item1, 1);
        }

        item2 = inven_right_hand(a2);
        if (item2 != NULL) {
            item_remove_mult(a2, item2, 1);
        }

        armor = inven_worn(a2);
        if (armor != NULL) {
            item_remove_mult(a2, armor, 1);
        }
    }

    bool isoWasEnabled = setup_inventory(INVENTORY_WINDOW_TYPE_LOOT);

    Object** critters = NULL;
    int critterCount = 0;
    int critterIndex = 0;
    if (!gIsSteal) {
        if (FID_TYPE(a2->fid) == OBJ_TYPE_CRITTER) {
            critterCount = obj_create_list(a2->tile, a2->elevation, OBJ_TYPE_CRITTER, &critters);
            int endIndex = critterCount - 1;
            for (int index = 0; index < critterCount; index++) {
                Object* critter = critters[index];
                if ((critter->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) == 0) {
                    critters[index] = critters[endIndex];
                    critters[endIndex] = critter;
                    critterCount--;
                    index--;
                    endIndex--;
                } else {
                    critterIndex++;
                }
            }

            if (critterCount == 1) {
                obj_delete_list(critters);
                critterCount = 0;
            }

            if (critterCount > 1) {
                int fid;
                unsigned char* buttonUpData;
                unsigned char* buttonDownData;
                int btn;

                for (int index = 0; index < INVENTORY_ARROW_FRM_COUNT; index++) {
                    arrowFrmHandles[index] = INVALID_CACHE_ENTRY;
                }

                // Setup left arrow button.
                fid = art_id(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_LEFT_ARROW_UP], 0, 0, 0);
                buttonUpData = art_ptr_lock_data(fid, 0, 0, &(arrowFrmHandles[INVENTORY_ARROW_FRM_LEFT_ARROW_UP]));

                fid = art_id(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN], 0, 0, 0);
                buttonDownData = art_ptr_lock_data(fid, 0, 0, &(arrowFrmHandles[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN]));

                if (buttonUpData != NULL && buttonDownData != NULL) {
                    btn = win_register_button(i_wid,
                        307,
                        149,
                        20,
                        18,
                        -1,
                        -1,
                        KEY_PAGE_UP,
                        -1,
                        buttonUpData,
                        buttonDownData,
                        NULL,
                        0);
                    if (btn != -1) {
                        win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                    }
                }

                // Setup right arrow button.
                fid = art_id(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP], 0, 0, 0);
                buttonUpData = art_ptr_lock_data(fid, 0, 0, &(arrowFrmHandles[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP]));

                fid = art_id(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN], 0, 0, 0);
                buttonDownData = art_ptr_lock_data(fid, 0, 0, &(arrowFrmHandles[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN]));

                if (buttonUpData != NULL && buttonDownData != NULL) {
                    btn = win_register_button(i_wid,
                        327,
                        149,
                        20,
                        18,
                        -1,
                        -1,
                        KEY_PAGE_DOWN,
                        -1,
                        buttonUpData,
                        buttonDownData,
                        NULL,
                        0);
                    if (btn != -1) {
                        win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                    }
                }

                for (int index = 0; index < critterCount; index++) {
                    if (a2 == critters[index]) {
                        critterIndex = index;
                    }
                }
            }
        }
    }

    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
    display_body(a2->fid, INVENTORY_WINDOW_TYPE_LOOT);
    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);

    // CE: ON A CONTROLLER, START ON THE LOOT ITSELF. The window used to open with the pointer
    // wherever the world had left it -- usually over the corpse in the middle of the screen, or on
    // a button -- so every single looting began with the player steering the cursor onto the right
    // panel before anything could be taken. The first slot of the CONTAINER side is the only thing
    // anyone opens this window for, so that is where the hand goes.
    //
    // Guarded on a controller being present: warping the pointer under a mouse player's hand would
    // be rude, and they are already holding the thing that aims it.
    if (gamepad_ui_get_controller() != NULL) {
        Rect lootRect;
        win_get_rect(i_wid, &lootRect);
        barter_warp_cursor(lootRect.ulx + INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2,
            lootRect.uly + INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT / 2);
    }

    bool isCaughtStealing = false;
    int stealingXp = 0;
    int stealingXpBonus = 10;
    for (;;) {
        sharedFpsLimiter.mark();

        if (game_user_wants_to_quit != 0) {
            break;
        }

        if (isCaughtStealing) {
            break;
        }

        int keyCode = get_input();

        // Gamepad D-pad up/down scrolls the LOOT list, which it could not do at all before --
        // "containers nelzou hÃ½bat dpadem". handle_inventory wired inven_gamepad_scroll_key into
        // its own loop; this window runs a separate one and never got it, so anything past the
        // visible rows was unreachable on a controller.
        //
        // Two panels, so the direction has to pick one: scroll whichever the cursor is over. Both
        // already have keyboard handlers below (plain arrows = your side, CTRL+arrows = the
        // container's), so this only translates the pad into those and changes no scroll logic.
        // Left stick is deliberately NOT used -- it drives the cursor here, same as in the normal
        // inventory (see inven_gamepad_scroll_key).
        if (keyCode == -1) {
            int padKey = inven_gamepad_scroll_key();
            if (padKey != -1) {
                bool overTarget = mouseHitTestInWindow(i_wid,
                    INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y,
                    INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X,
                    INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_RIGHT_SCROLLER_Y);

                // ⚠️ STEP THE ITEMS FIRST, SCROLL ONLY AT THE ENDS. The D-pad used to scroll the
                // list and nothing else, which meant the cursor sat on the top slot for ever: you
                // could bring any item into view and still only ever take THAT one. It is the same
                // shape he asked for everywhere else and the barter window already has -- "začne se
                // na prvním, a pak postupně druhý třetí čtvrtý, a pak d-pad posouvá na další" --
                // and it was missing in both columns, the container's most visibly.
                //
                // The cursor IS the selection here (this window paints its slots itself and keeps no
                // clean copy to erase a frame against), so stepping means parking it one slot on.
                int panelX = overTarget ? INVENTORY_LOOT_RIGHT_SCROLLER_X : INVENTORY_LOOT_LEFT_SCROLLER_X;
                int panelY = overTarget ? INVENTORY_LOOT_RIGHT_SCROLLER_Y : INVENTORY_LOOT_LEFT_SCROLLER_Y;

                int listLength = overTarget ? target_pud->length : pud->length;
                int listOffset = overTarget ? target_stack_offset[target_curr_stack] : stack_offset[curr_stack];

                int shown = listLength - listOffset;
                if (shown > inven_cur_disp) {
                    shown = inven_cur_disp;
                }

                int mx;
                int my;
                mouseGetPositionInWindow(i_wid, &mx, &my);

                int sel = (my - panelY) / INVENTORY_SLOT_HEIGHT;
                if (sel < 0) {
                    sel = 0;
                } else if (sel > inven_cur_disp - 1) {
                    sel = inven_cur_disp - 1;
                }

                int next = sel + (padKey == KEY_ARROW_UP ? -1 : 1);

                if (next >= 0 && next < shown) {
                    Rect lootRect;
                    win_get_rect(i_wid, &lootRect);
                    barter_warp_cursor(lootRect.ulx + panelX + INVENTORY_SLOT_WIDTH / 2,
                        lootRect.uly + panelY + next * INVENTORY_SLOT_HEIGHT + INVENTORY_SLOT_HEIGHT / 2);
                    // Handled: no key, so nothing else in this loop reacts to the press.
                } else if (overTarget) {
                    keyCode = padKey == KEY_ARROW_UP ? KEY_CTRL_ARROW_UP : KEY_CTRL_ARROW_DOWN;
                } else {
                    keyCode = padKey;
                }
            }
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            game_quit_with_confirm();
        }

        if (game_user_wants_to_quit != 0) {
            break;
        }

        // Controller: left/right jumps the pointer to the first item of your / the container's
        // panel (runs after get_input so it wins over the shared horizontal cursor snap).
        loot_gamepad_panel_jump(i_wid);

        // Controller quick-take: the gamepad layer signals 1 (A = one unit) or 2
        // (Y = whole stack) for the item under the cursor. Transfer it directly
        // via move_inventory's quick (no-drag) mode, in whichever panel the cursor
        // is over, with the steal bookkeeping handled exactly like the mouse path.
        int lootQuick = gamepad_ui_take_loot_quick();
        if (lootQuick != 0) {
            int mx;
            int my;
            mouseGetPositionInWindow(i_wid, &mx, &my);

            if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
                int index = (my - INVENTORY_LOOT_LEFT_SCROLLER_Y) / INVENTORY_SLOT_HEIGHT;
                if (index >= 0 && index + stack_offset[curr_stack] < pud->length) {
                    InventoryItem* inventoryItem = &(pud->items[index + stack_offset[curr_stack]]);
                    int amount = lootQuick == 2 ? inventoryItem->quantity : 1;
                    gStealCount += 1;
                    gStealSize += item_size(inventoryItem->item);
                    int rc = move_inventory(inventoryItem->item, index, target_stack[target_curr_stack], true, amount);
                    if (rc == 1) {
                        isCaughtStealing = true;
                    } else if (rc == 2) {
                        stealingXp += stealingXpBonus;
                        stealingXpBonus += 10;
                    }
                    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                }
            } else if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
                int index = (my - INVENTORY_LOOT_RIGHT_SCROLLER_Y) / INVENTORY_SLOT_HEIGHT;
                if (index >= 0 && index + target_stack_offset[target_curr_stack] < target_pud->length) {
                    InventoryItem* inventoryItem = &(target_pud->items[index + target_stack_offset[target_curr_stack]]);
                    int amount = lootQuick == 2 ? inventoryItem->quantity : 1;
                    gStealCount += 1;
                    gStealSize += item_size(inventoryItem->item);
                    int rc = move_inventory(inventoryItem->item, index, target_stack[target_curr_stack], false, amount);
                    if (rc == 1) {
                        isCaughtStealing = true;
                    } else if (rc == 2) {
                        stealingXp += stealingXpBonus;
                        stealingXpBonus += 10;
                    }
                    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                }
            }

            keyCode = -1;
        }

        if (keyCode == KEY_UPPERCASE_A) {
            if (!gIsSteal) {
                int maxCarryWeight = stat_level(a1, STAT_CARRY_WEIGHT);
                int currentWeight = item_total_weight(a1);
                int newInventoryWeight = item_total_weight(a2);
                if (newInventoryWeight <= maxCarryWeight - currentWeight) {
                    item_move_all(a2, a1);
                    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                } else {
                    // Sorry, you cannot carry that much.
                    messageListItem.num = 31;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        dialog_out(messageListItem.text, NULL, 0, 169, 117, colorTable[32328], NULL, colorTable[32328], 0);
                    }
                }
            }
        } else if (keyCode == KEY_ARROW_UP) {
            if (stack_offset[curr_stack] > 0) {
                stack_offset[curr_stack] -= 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_PAGE_UP) {
            if (critterCount != 0) {
                if (critterIndex > 0) {
                    critterIndex -= 1;
                } else {
                    critterIndex = critterCount - 1;
                }

                a2 = critters[critterIndex];
                target_pud = &(a2->data.inventory);
                target_stack[0] = a2;
                target_curr_stack = 0;
                target_stack_offset[0] = 0;
                display_target_inventory(0, -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                display_body(a2->fid, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (stack_offset[curr_stack] + inven_cur_disp < pud->length) {
                stack_offset[curr_stack] += 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_PAGE_DOWN) {
            if (critterCount != 0) {
                if (critterIndex < critterCount - 1) {
                    critterIndex += 1;
                } else {
                    critterIndex = 0;
                }

                a2 = critters[critterIndex];
                target_pud = &(a2->data.inventory);
                target_stack[0] = a2;
                target_curr_stack = 0;
                target_stack_offset[0] = 0;
                display_target_inventory(0, -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                display_body(a2->fid, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_CTRL_ARROW_UP) {
            if (target_stack_offset[target_curr_stack] > 0) {
                target_stack_offset[target_curr_stack] -= 1;
                display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                win_draw(i_wid);
            }
        } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
            if (target_stack_offset[target_curr_stack] + inven_cur_disp < target_pud->length) {
                target_stack_offset[target_curr_stack] += 1;
                display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                win_draw(i_wid);
            }
        } else if (keyCode >= 2500 && keyCode <= 2501) {
            container_exit(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
        } else {
            if ((mouse_get_buttons() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (immode == INVENTORY_WINDOW_CURSOR_HAND) {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1000 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
                    } else {
                        int index = keyCode - 1000;
                        if (index + stack_offset[curr_stack] < pud->length) {
                            gStealCount += 1;
                            gStealSize += item_size(stack[curr_stack]);

                            InventoryItem* inventoryItem = &(pud->items[index + stack_offset[curr_stack]]);
                            int rc = move_inventory(inventoryItem->item, index, target_stack[target_curr_stack], true);
                            if (rc == 1) {
                                isCaughtStealing = true;
                            } else if (rc == 2) {
                                stealingXp += stealingXpBonus;
                                stealingXpBonus += 10;
                            }

                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }

                        keyCode = -1;
                    }
                } else if (keyCode >= 2000 && keyCode <= 2000 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
                    } else {
                        int index = keyCode - 2000;
                        if (index + target_stack_offset[target_curr_stack] < target_pud->length) {
                            gStealCount += 1;
                            gStealSize += item_size(stack[curr_stack]);

                            InventoryItem* inventoryItem = &(target_pud->items[index + target_stack_offset[target_curr_stack]]);
                            int rc = move_inventory(inventoryItem->item, index, target_stack[target_curr_stack], false);
                            if (rc == 1) {
                                isCaughtStealing = true;
                            } else if (rc == 2) {
                                stealingXp += stealingXpBonus;
                                stealingXpBonus += 10;
                            }

                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    }
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (stack_offset[curr_stack] > 0) {
                            stack_offset[curr_stack] -= 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    } else if (wheelY < 0) {
                        if (stack_offset[curr_stack] + inven_cur_disp < pud->length) {
                            stack_offset[curr_stack] += 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    }
                } else if (mouseHitTestInWindow(i_wid, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (target_stack_offset[target_curr_stack] > 0) {
                            target_stack_offset[target_curr_stack] -= 1;
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            win_draw(i_wid);
                        }
                    } else if (wheelY < 0) {
                        if (target_stack_offset[target_curr_stack] + inven_cur_disp < target_pud->length) {
                            target_stack_offset[target_curr_stack] += 1;
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            win_draw(i_wid);
                        }
                    }
                }
            }
        }

        if (keyCode == KEY_ESCAPE) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (critterCount != 0) {
        obj_delete_list(critters);

        for (int index = 0; index < INVENTORY_ARROW_FRM_COUNT; index++) {
            art_ptr_unlock(arrowFrmHandles[index]);
        }
    }

    if (gIsSteal) {
        if (item1 != NULL) {
            item1->flags |= OBJECT_IN_LEFT_HAND;
            item_add_force(a2, item1, 1);
        }

        if (item2 != NULL) {
            item2->flags |= OBJECT_IN_RIGHT_HAND;
            item_add_force(a2, item2, 1);
        }

        if (armor != NULL) {
            armor->flags |= OBJECT_WORN;
            item_add_force(a2, armor, 1);
        }
    }

    item_move_all(a1a, a2);
    obj_erase_object(a1a, NULL);

    if (gIsSteal) {
        if (!isCaughtStealing) {
            if (stealingXp > 0) {
                if (!isPartyMember(a2)) {
                    stealingXp = std::min(300 - skill_level(a1, SKILL_STEAL), stealingXp);
                    debug_printf("\n[[[%d]]]", 300 - skill_level(a1, SKILL_STEAL));

                    // You gain %d experience points for successfully using your Steal skill.
                    messageListItem.num = 29;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        char formattedText[200];
                        snprintf(formattedText, sizeof(formattedText), messageListItem.text, stealingXp);
                        display_print(formattedText);
                    }

                    stat_pc_add_experience(stealingXp);
                }
            }
        }
    }

    exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inven_exit();

    if (gIsSteal) {
        if (isCaughtStealing) {
            if (gStealCount > 0) {
                if (obj_sid(a2, &sid) != -1) {
                    scr_set_objs(sid, a1, NULL);
                    exec_script_proc(sid, SCRIPT_PROC_PICKUP);

                    // TODO: Looks like inlining, script is not used.
                    Script* script;
                    scr_ptr(sid, &script);
                }
            }
        }
    }

    return 0;
}

// 0x467658
int inven_steal_container(Object* a1, Object* a2)
{
    if (a1 == a2) {
        return -1;
    }

    gIsSteal = PID_TYPE(a1->pid) == OBJ_TYPE_CRITTER && critter_is_active(a2);
    gStealCount = 0;
    gStealSize = 0;

    int rc = loot_container(a1, a2);

    gIsSteal = 0;
    gStealCount = 0;
    gStealSize = 0;

    return rc;
}

// 0x4676C0
int move_inventory(Object* a1, int a2, Object* a3, bool a4, int gpQuantity)
{
    // Controller quick transfer: skip the pick-up/drag/release gesture and the
    // destination hit-test, and move exactly gpQuantity units to the other panel.
    bool gp = gpQuantity != 0;

    bool v38 = true;

    Rect rect;

    int quantity;
    if (a4) {
        rect.ulx = INVENTORY_LOOT_LEFT_SCROLLER_X;
        rect.uly = INVENTORY_SLOT_HEIGHT * a2 + INVENTORY_LOOT_LEFT_SCROLLER_Y;

        InventoryItem* inventoryItem = &(pud->items[a2 + stack_offset[curr_stack]]);
        quantity = inventoryItem->quantity;
        if (quantity > 1) {
            display_inventory(stack_offset[curr_stack], a2, INVENTORY_WINDOW_TYPE_LOOT);
            v38 = false;
        }
    } else {
        rect.ulx = INVENTORY_LOOT_RIGHT_SCROLLER_X;
        rect.uly = INVENTORY_SLOT_HEIGHT * a2 + INVENTORY_LOOT_RIGHT_SCROLLER_Y;

        InventoryItem* inventoryItem = &(target_pud->items[a2 + target_stack_offset[target_curr_stack]]);
        quantity = inventoryItem->quantity;
        if (quantity > 1) {
            display_target_inventory(target_stack_offset[target_curr_stack], a2, target_pud, INVENTORY_WINDOW_TYPE_LOOT);
            win_draw(i_wid);
            v38 = false;
        }
    }

    if (!gp) {
        if (v38) {
            unsigned char* windowBuffer = win_get_buf(i_wid);

            CacheEntry* handle;
            int fid = art_id(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
            unsigned char* data = art_ptr_lock_data(fid, 0, 0, &handle);
            if (data != NULL) {
                buf_to_buf(data + INVENTORY_LOOT_WINDOW_WIDTH * rect.uly + rect.ulx,
                    INVENTORY_SLOT_WIDTH,
                    INVENTORY_SLOT_HEIGHT,
                    INVENTORY_LOOT_WINDOW_WIDTH,
                    windowBuffer + INVENTORY_LOOT_WINDOW_WIDTH * rect.uly + rect.ulx,
                    INVENTORY_LOOT_WINDOW_WIDTH);
                art_ptr_unlock(handle);
            }

            rect.lrx = rect.ulx + INVENTORY_SLOT_WIDTH - 1;
            rect.lry = rect.uly + INVENTORY_SLOT_HEIGHT - 1;
            win_draw_rect(i_wid, &rect);
        }

        CacheEntry* inventoryFrmHandle;
        int inventoryFid = item_inv_fid(a1);
        Art* inventoryFrm = art_ptr_lock(inventoryFid, &inventoryFrmHandle);
        if (inventoryFrm != NULL) {
            int width = art_frame_width(inventoryFrm, 0, 0);
            int height = art_frame_length(inventoryFrm, 0, 0);
            unsigned char* data = art_frame_data(inventoryFrm, 0, 0);
            mouse_set_shape(data, width, height, width, width / 2, height / 2, 0);
            gsound_play_sfx_file("ipickup1");
        }

        do {
            sharedFpsLimiter.mark();

            get_input();

            renderPresent();
            sharedFpsLimiter.throttle();
        } while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

        if (inventoryFrm != NULL) {
            art_ptr_unlock(inventoryFrmHandle);
            gsound_play_sfx_file("iputdown");
        }
    }

    int rc = 0;
    MessageListItem messageListItem;

    if (a4) {
        if (gp || mouseHitTestInWindow(i_wid, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
            int quantityToMove;
            if (gp) {
                quantityToMove = gpQuantity;
            } else if (quantity > 1) {
                quantityToMove = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity);
            } else {
                quantityToMove = 1;
            }

            if (quantityToMove != -1) {
                if (gIsSteal) {
                    if (skill_check_stealing(inven_dude, a3, a1, true) == 0) {
                        rc = 1;
                    }
                }

                if (rc != 1) {
                    if (item_move(inven_dude, a3, a1, quantityToMove) != -1) {
                        rc = 2;
                    } else {
                        // There is no space left for that item.
                        messageListItem.num = 26;
                        if (message_search(&inventry_message_file, &messageListItem)) {
                            display_print(messageListItem.text);
                        }
                    }
                }
            }
        }
    } else {
        if (gp || mouseHitTestInWindow(i_wid, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
            int quantityToMove;
            if (gp) {
                quantityToMove = gpQuantity;
            } else if (quantity > 1) {
                quantityToMove = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity);
            } else {
                quantityToMove = 1;
            }

            if (quantityToMove != -1) {
                if (gIsSteal) {
                    if (skill_check_stealing(inven_dude, a3, a1, false) == 0) {
                        rc = 1;
                    }
                }

                if (rc != 1) {
                    if (item_move(a3, inven_dude, a1, quantityToMove) == 0) {
                        if ((a1->flags & OBJECT_IN_RIGHT_HAND) != 0) {
                            a3->fid = art_id(FID_TYPE(a3->fid), a3->fid & 0xFFF, FID_ANIM_TYPE(a3->fid), 0, a3->rotation + 1);
                        }

                        a3->flags &= ~OBJECT_EQUIPPED;

                        rc = 2;
                    } else {
                        // You cannot pick that up. You are at your maximum weight capacity.
                        messageListItem.num = 25;
                        if (message_search(&inventry_message_file, &messageListItem)) {
                            display_print(messageListItem.text);
                        }
                    }
                }
            }
        }
    }

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);

    return rc;
}

// 0x467AD0
static int barter_compute_value(Object* buyer, Object* seller)
{
    int mod = 100;

    if (buyer == obj_dude) {
        if (perk_level(PERK_MASTER_TRADER)) {
            mod += 25;
        }
    }

    int buyer_mod = skill_level(buyer, SKILL_BARTER);
    int seller_mod = skill_level(seller, SKILL_BARTER);
    mod += buyer_mod - seller_mod + barter_mod;

    mod = std::clamp(mod, 10, 300);

    int cost = item_total_cost(btable);
    int caps = item_caps_total(btable);

    // Exclude caps before applying modifier (since it only affect items).
    return 100 * (cost - caps) / mod + caps;
}

// 0x467B70
static int barter_attempt_transaction(Object* a1, Object* a2, Object* a3, Object* a4)
{
    if (a2->data.inventory.length == 0) {
        return -1;
    }

    if (item_queued(a2)) {
        return -1;
    }

    if (barter_compute_value(a1, a3) > item_total_cost(a2)) {
        return -1;
    }

    item_move_all(a4, a1);
    item_move_all(a2, a3);
    return 0;
}

// 0x467BD4
static void barter_move_inventory(Object* a1, int quantity, int a3, int a4, Object* a5, Object* a6, bool a7)
{
    Rect rect;
    if (a7) {
        rect.ulx = 23;
        rect.uly = 48 * a3 + 34;
    } else {
        rect.ulx = 395;
        rect.uly = 48 * a3 + 31;
    }

    if (quantity > 1) {
        if (a7) {
            display_inventory(a4, a3, INVENTORY_WINDOW_TYPE_TRADE);
        } else {
            display_target_inventory(a4, a3, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
        }
    } else {
        unsigned char* dest = win_get_buf(i_wid);
        unsigned char* src = win_get_buf(barter_back_win);

        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        buf_to_buf(src + pitch * rect.uly + rect.ulx + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, INVENTORY_SLOT_HEIGHT, pitch, dest + INVENTORY_TRADE_WINDOW_WIDTH * rect.uly + rect.ulx, INVENTORY_TRADE_WINDOW_WIDTH);

        rect.lrx = rect.ulx + INVENTORY_SLOT_WIDTH - 1;
        rect.lry = rect.uly + INVENTORY_SLOT_HEIGHT - 1;
        win_draw_rect(i_wid, &rect);
    }

    CacheEntry* inventoryFrmHandle;
    int inventoryFid = item_inv_fid(a1);
    Art* inventoryFrm = art_ptr_lock(inventoryFid, &inventoryFrmHandle);
    if (inventoryFrm != NULL) {
        int width = art_frame_width(inventoryFrm, 0, 0);
        int height = art_frame_length(inventoryFrm, 0, 0);
        unsigned char* data = art_frame_data(inventoryFrm, 0, 0);
        mouse_set_shape(data, width, height, width, width / 2, height / 2, 0);
        gsound_play_sfx_file("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        get_input();

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (inventoryFrm != NULL) {
        art_ptr_unlock(inventoryFrmHandle);
        gsound_play_sfx_file("iputdown");
    }

    MessageListItem messageListItem;

    if (a7) {
        if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity) : 1;
            if (quantityToMove != -1) {
                if (item_move_force(inven_dude, a6, a1, quantityToMove) == -1) {
                    // There is no space left for that item.
                    messageListItem.num = 26;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        display_print(messageListItem.text);
                    }
                }
            }
        }
    } else {
        if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity) : 1;
            if (quantityToMove != -1) {
                if (item_move_force(a5, a6, a1, quantityToMove) == -1) {
                    // You cannot pick that up. You are at your maximum weight capacity.
                    messageListItem.num = 25;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        display_print(messageListItem.text);
                    }
                }
            }
        }
    }

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
}

// 0x467E84
static void barter_move_from_table_inventory(Object* a1, int quantity, int a3, Object* a4, Object* a5, bool a6)
{
    Rect rect;
    if (a6) {
        rect.ulx = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        rect.uly = INVENTORY_SLOT_HEIGHT * a3 + INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD;
    } else {
        rect.ulx = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        rect.uly = INVENTORY_SLOT_HEIGHT * a3 + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD;
    }

    if (quantity > 1) {
        if (a6) {
            display_table_inventories(barter_back_win, a5, NULL, a3);
        } else {
            display_table_inventories(barter_back_win, NULL, a5, a3);
        }
    } else {
        unsigned char* dest = win_get_buf(i_wid);
        unsigned char* src = win_get_buf(barter_back_win);

        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        buf_to_buf(src + pitch * rect.uly + rect.ulx + INVENTORY_TRADE_WINDOW_OFFSET,
            INVENTORY_SLOT_WIDTH,
            INVENTORY_SLOT_HEIGHT,
            pitch,
            dest + INVENTORY_TRADE_WINDOW_WIDTH * rect.uly + rect.ulx,
            INVENTORY_TRADE_WINDOW_WIDTH);

        rect.lrx = rect.ulx + INVENTORY_SLOT_WIDTH - 1;
        rect.lry = rect.uly + INVENTORY_SLOT_HEIGHT - 1;
        win_draw_rect(i_wid, &rect);
    }

    CacheEntry* inventoryFrmHandle;
    int inventoryFid = item_inv_fid(a1);
    Art* inventoryFrm = art_ptr_lock(inventoryFid, &inventoryFrmHandle);
    if (inventoryFrm != NULL) {
        int width = art_frame_width(inventoryFrm, 0, 0);
        int height = art_frame_length(inventoryFrm, 0, 0);
        unsigned char* data = art_frame_data(inventoryFrm, 0, 0);
        mouse_set_shape(data, width, height, width, width / 2, height / 2, 0);
        gsound_play_sfx_file("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        get_input();

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (inventoryFrm != NULL) {
        art_ptr_unlock(inventoryFrmHandle);
        gsound_play_sfx_file("iputdown");
    }

    MessageListItem messageListItem;

    if (a6) {
        if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity) : 1;
            if (quantityToMove != -1) {
                if (item_move_force(a5, inven_dude, a1, quantityToMove) == -1) {
                    // There is no space left for that item.
                    messageListItem.num = 26;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        display_print(messageListItem.text);
                    }
                }
            }
        }
    } else {
        if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a1, quantity) : 1;
            if (quantityToMove != -1) {
                if (item_move_force(a5, a4, a1, quantityToMove) == -1) {
                    // You cannot pick that up. You are at your maximum weight capacity.
                    messageListItem.num = 25;
                    if (message_search(&inventry_message_file, &messageListItem)) {
                        display_print(messageListItem.text);
                    }
                }
            }
        }
    }

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
}

// 0x468138
static void display_table_inventories(int win, Object* a2, Object* a3, int a4)
{
    unsigned char* windowBuffer = win_get_buf(i_wid);

    int oldFont = text_curr();
    text_font(101);

    char formattedText[80];
    int v45 = text_height() + INVENTORY_SLOT_HEIGHT * inven_cur_disp;

    if (a2 != NULL) {
        unsigned char* src = win_get_buf(win);
        buf_to_buf(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD + INVENTORY_TRADE_WINDOW_OFFSET,
            INVENTORY_SLOT_WIDTH,
            v45 + 1,
            INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH,
            windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD,
            INVENTORY_TRADE_WINDOW_WIDTH);

        unsigned char* dest = windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        Inventory* inventory = &(a2->data.inventory);
        for (int index = 0; index < inven_cur_disp && index + ptable_offset < inventory->length; index++) {
            InventoryItem* inventoryItem = &(inventory->items[index + ptable_offset]);
            int inventoryFid = item_inv_fid(inventoryItem->item);
            scale_art(inventoryFid,
                dest,
                INVENTORY_SLOT_WIDTH_PAD,
                INVENTORY_SLOT_HEIGHT_PAD,
                INVENTORY_TRADE_WINDOW_WIDTH);
            display_inventory_info(inventoryItem->item,
                inventoryItem->quantity,
                dest,
                INVENTORY_TRADE_WINDOW_WIDTH,
                index == a4);

            dest += INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_SLOT_HEIGHT;
        }

        int cost = item_total_cost(a2);
        snprintf(formattedText, sizeof(formattedText), "$%d", cost);

        text_to_buf(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD,
            formattedText,
            80,
            INVENTORY_TRADE_WINDOW_WIDTH,
            colorTable[32767]);

        Rect rect;
        rect.ulx = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        rect.uly = INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD;
        // NOTE: Odd math, the only way to get 223 is to subtract 2.
        rect.lrx = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD + INVENTORY_SLOT_WIDTH_PAD - 2;
        rect.lry = rect.uly + v45;
        win_draw_rect(i_wid, &rect);
    }

    if (a3 != NULL) {
        unsigned char* src = win_get_buf(win);
        buf_to_buf(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD + INVENTORY_TRADE_WINDOW_OFFSET,
            INVENTORY_SLOT_WIDTH,
            v45 + 1,
            INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH,
            windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD,
            INVENTORY_TRADE_WINDOW_WIDTH);

        unsigned char* dest = windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        Inventory* inventory = &(a3->data.inventory);
        for (int index = 0; index < inven_cur_disp && index + btable_offset < inventory->length; index++) {
            InventoryItem* inventoryItem = &(inventory->items[index + btable_offset]);
            int inventoryFid = item_inv_fid(inventoryItem->item);
            scale_art(inventoryFid,
                dest,
                INVENTORY_SLOT_WIDTH_PAD,
                INVENTORY_SLOT_HEIGHT_PAD,
                INVENTORY_TRADE_WINDOW_WIDTH);
            display_inventory_info(inventoryItem->item,
                inventoryItem->quantity,
                dest,
                INVENTORY_TRADE_WINDOW_WIDTH,
                index == a4);

            dest += INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_SLOT_HEIGHT;
        }

        int cost = barter_compute_value(obj_dude, target_stack[0]);
        snprintf(formattedText, sizeof(formattedText), "$%d", cost);

        text_to_buf(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (INVENTORY_SLOT_HEIGHT * inven_cur_disp + 24) + 254, formattedText, 80, INVENTORY_TRADE_WINDOW_WIDTH, colorTable[32767]);

        Rect rect;
        rect.ulx = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        rect.uly = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD;
        // NOTE: Odd math, likely should be `INVENTORY_SLOT_WIDTH_PAD`.
        rect.lrx = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD + INVENTORY_SLOT_WIDTH;
        rect.lry = rect.uly + v45;
        win_draw_rect(i_wid, &rect);
    }

    text_font(oldFont);

    // The deal readout rides along with every table repaint. Hooked HERE rather than at the call
    // sites because there are more than twenty of them across the vanilla barter loop, and one of
    // them being missed is exactly how a stale number ends up on screen. Self-guarding: it returns
    // immediately when there is no barter in progress.
    barter_draw_deal();
}

// One occupied, visible barter slot: its screen rect + which list (0..3) and visible
// row it maps to, so a cursor over it can be transferred with the right move call.
struct BarterSlot {
    int x0; // screen top-left
    int y0;
    int listId; // 0 player inv, 1 player offer, 2 NPC offer, 3 NPC inv
    int row; // visible row within the list (== slot button keyCode base offset)
};

// Rebuilds the flat left-to-right list of the four trade lists' occupied visible slots
// (player inventory -> player offer -> NPC offer -> NPC inventory). Returns the count;
// fills slots[] (cap 4*inven_cur_disp) and the trade window's screen rect.
static int barter_collect_slots(BarterSlot* slots, int cap, Rect* winRect)
{
    if (win_get_rect(i_wid, winRect) != 0) {
        return 0;
    }

    struct {
        int x;
        int y0;
        int count;
    } cols[4] = {
        { INVENTORY_TRADE_LEFT_SCROLLER_X, INVENTORY_TRADE_SCROLLER_Y, pud->length - stack_offset[curr_stack] },
        { INVENTORY_TRADE_INNER_LEFT_SCROLLER_X, INVENTORY_TRADE_INNER_SCROLLER_Y, ptable_pud->length - ptable_offset },
        { INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X, INVENTORY_TRADE_INNER_SCROLLER_Y, btable_pud->length - btable_offset },
        { INVENTORY_TRADE_RIGHT_SCROLLER_X, INVENTORY_TRADE_SCROLLER_Y, target_pud->length - target_stack_offset[target_curr_stack] },
    };

    int n = 0;
    for (int c = 0; c < 4; c++) {
        int count = cols[c].count;
        if (count > inven_cur_disp) {
            count = inven_cur_disp;
        }
        for (int i = 0; i < count && n < cap; i++) {
            slots[n].x0 = winRect->ulx + cols[c].x;
            slots[n].y0 = winRect->uly + cols[c].y0 + i * INVENTORY_SLOT_HEIGHT;
            slots[n].listId = c;
            slots[n].row = i;
            n++;
        }
    }
    return n;
}

// Moves the mouse cursor to (sx, sy) AND redraws it: mouse_set_position alone only
// updates coordinates (mouse.cc:744) -- it does NOT erase the old cursor sprite or
// blit the new one, which leaves a ghost trail. Replicate what mouse_simulate_input
// does on a real move: refresh the window under the old rect, then re-show at the new.
static void barter_warp_cursor(int sx, int sy)
{
    int curX;
    int curY;
    mouse_get_position(&curX, &curY);
    if (sx == curX && sy == curY) {
        return;
    }

    Rect oldRect;
    mouse_get_rect(&oldRect);
    mouse_set_position(sx, sy);
    win_refresh_all(&oldRect);
    mouse_show();
}

// Transfers the item in (listId, row): player inventory / NPC inventory slots move the
// item into that side's offer table; an offer-table slot moves it back.
//
// This calls item_move_force DIRECTLY rather than the mouse handlers
// barter_move_inventory / barter_move_from_table_inventory: those are DRAG-and-DROP
// (they pick the item up, wait for the mouse button to release, then require the cursor
// to be over the DESTINATION offer-table drop zone). Driven from a controller the cursor
// stays on the SOURCE slot, so that drop-zone hit-test failed and nothing moved -- the
// "A does nothing" bug. Moving the WHOLE stack in one press also kills the "click caps 60
// times" grind for stacked items. The src/dst pairs mirror those handlers' final
// item_move_force calls exactly.
static void barter_redraw_tables()
{
    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
    display_table_inventories(barter_back_win, ptable, btable, -1);
}

// The item shown in (listId, visible row), or NULL if that slot is empty. Same four-list mapping
// barter_transfer_slot uses -- kept beside it so the two can never drift about which list is which.
static Object* barter_slot_item(int listId, int row)
{
    switch (listId) {
    case 0: { // player inventory
        int idx = row + stack_offset[curr_stack];
        return idx < pud->length ? pud->items[idx].item : NULL;
    }
    case 1: { // player's offer table
        int idx = row + ptable_offset;
        return idx < ptable_pud->length ? ptable_pud->items[idx].item : NULL;
    }
    case 2: { // NPC's offer table
        int idx = row + btable_offset;
        return idx < btable_pud->length ? btable_pud->items[idx].item : NULL;
    }
    case 3: { // NPC inventory
        int idx = row + target_stack_offset[target_curr_stack];
        return idx < target_pud->length ? target_pud->items[idx].item : NULL;
    }
    default:
        return NULL;
    }
}

// How much of the stack under the cursor one press moves.
enum BarterAmount {
    BARTER_AMOUNT_ONE, // A
    BARTER_AMOUNT_ALL, // LT + A
    BARTER_AMOUNT_PICK, // LT + A held -- the quantity picker
};

// Put the cursor back on the nearest occupied slot after an item has moved lists, so repeated
// A / LB / RB keep flowing instead of stranding it on a slot that is now empty.
static void barter_reanchor_cursor(int mouseX, int mouseY)
{
    Rect winRect;
    BarterSlot after[4 * 3];
    int nAfter = barter_collect_slots(after, 4 * 3, &winRect);
    if (nAfter <= 0) {
        return;
    }

    int nearest = 0;
    long bestDist = LONG_MAX;
    for (int i = 0; i < nAfter; i++) {
        long dx = after[i].x0 + INVENTORY_SLOT_WIDTH / 2 - mouseX;
        long dy = after[i].y0 + INVENTORY_SLOT_HEIGHT / 2 - mouseY;
        long d = dx * dx + dy * dy;
        if (d < bestDist) {
            bestDist = d;
            nearest = i;
        }
    }

    barter_warp_cursor(after[nearest].x0 + INVENTORY_SLOT_WIDTH / 2,
        after[nearest].y0 + INVENTORY_SLOT_HEIGHT / 2);
}

static void barter_transfer_slot(int listId, int row, BarterAmount amount)
{
    Object* npc = target_stack[0]; // the NPC we are trading with

    Object* item = NULL;
    int qty = 0;
    Object* src = NULL;
    Object* dst = NULL;

    switch (listId) {
    case 0: { // player inventory -> player's offer
        int idx = row + stack_offset[curr_stack];
        if (idx >= pud->length) {
            return;
        }
        item = pud->items[idx].item;
        qty = pud->items[idx].quantity;
        src = inven_dude;
        dst = ptable;
        break;
    }
    case 1: { // player's offer -> back to player inventory
        int idx = row + ptable_offset;
        if (idx >= ptable_pud->length) {
            return;
        }
        item = ptable_pud->items[idx].item;
        qty = ptable_pud->items[idx].quantity;
        src = ptable;
        dst = inven_dude;
        break;
    }
    case 2: { // NPC's offer -> back to NPC inventory
        int idx = row + btable_offset;
        if (idx >= btable_pud->length) {
            return;
        }
        item = btable_pud->items[idx].item;
        qty = btable_pud->items[idx].quantity;
        src = btable;
        dst = npc;
        break;
    }
    case 3: { // NPC inventory -> NPC's offer
        int idx = row + target_stack_offset[target_curr_stack];
        if (idx >= target_pud->length) {
            return;
        }
        item = target_pud->items[idx].item;
        qty = target_pud->items[idx].quantity;
        src = npc;
        dst = btable;
        break;
    }
    default:
        return;
    }

    if (item == NULL || qty <= 0) {
        return;
    }

    // ⚠️ A MOVES ONE, LT+A MOVES THE LOT, LT+A HELD ASKS. His call: "Ačko přidá jeden item a LT
    // všechny", then "a jak přidám some, třeba kdybych chtěl přidat 3 stimpacks?".
    //
    // Three stimpaks are three taps, which is fewer presses than the picker used to cost to open,
    // fill in and confirm -- and that is why the picker is no longer on the tap. But thirty out of
    // fifty is not thirty taps, so it did not simply go away either: it moved onto the HOLD, where
    // it is there when you ask for it and never in the way when you do not. ⚠️ The same shape the
    // rest of this screen uses: LT decides how much, tap versus hold decides how you say it.
    if (qty > 1) {
        if (amount == BARTER_AMOUNT_ONE) {
            qty = 1;
        } else if (amount == BARTER_AMOUNT_PICK) {
            int chosen = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, qty);
            if (chosen < 1) {
                return; // cancelled
            }
            qty = chosen;
        }
    }

    if (item_move_force(src, dst, item, qty) == -1) {
        MessageListItem messageListItem;
        messageListItem.num = 25; // at maximum weight / no space
        if (message_search(&inventry_message_file, &messageListItem)) {
            display_print(messageListItem.text);
        }
        return;
    }

    gsound_play_sfx_file("iputdown");
    barter_redraw_tables();
}

// CE: "Auto-match offer" (gamepad X). Tops up the PLAYER's offer table with exactly
// enough caps to meet the NPC's current asking price for whatever is on their offer
// table -- so buying an item priced at 60 is one button, not 60 cap clicks. The price
// is barter_compute_value (the same value barter_attempt_transaction checks, so after
// this the trade will be accepted); the shortfall is filled from the player's spare
// caps (capped at what they actually carry).
// Finds the top-level caps (money) stack in obj's inventory. Returns the item (or NULL) and
// its quantity via *qtyOut.
static Object* barter_find_money(Object* obj, int* qtyOut)
{
    Inventory* inv = &(obj->data.inventory);
    for (int i = 0; i < inv->length; i++) {
        if (inv->items[i].item->pid == PROTO_ID_MONEY) {
            *qtyOut = inv->items[i].quantity;
            return inv->items[i].item;
        }
    }
    *qtyOut = 0;
    return NULL;
}

// X in the barter window: even the deal out with caps, so a single press finishes it instead of
// clicking caps one by one. Bidirectional (see barter_auto_offer for the scale it measures on):
//   SHORT    -- the player owes for the NPC's offer -> the trader's caps come off ITS table first,
//               then the DUDE's caps go onto the player's table.
//   OVERPAY  -- the player's offer is worth more than the trader's -> the player's own caps come
//               back off the player's table first, then the TRADER's caps go onto its table.
// Each side is capped at the caps that side actually carries.
// THE DEAL READOUT -- the number the game decides on, finally on screen.
//
// ⚠️ VANILLA SHOWS YOU TWO NUMBERS AND JUDGES YOU ON A THIRD. The $N under each table is the RAW
// item_total_cost of that table, but barter_attempt_transaction refuses unless
// barter_compute_value(...) <= item_total_cost(ptable) -- and barter_compute_value applies the
// Barter-skill / Master Trader / reputation modifier to the NPC's side. So two piles reading $100
// and $100 are routinely rejected, because what the trader actually wants is $130, and nothing
// anywhere told you that. That is the whole "trading is bad" experience: not a control problem, an
// information problem.
//
// This draws what the check really compares -- what you are GIVING against what they WANT after the
// markup -- plus the exact shortfall, which is also precisely the number X (barter_auto_offer)
// covers with caps. The two features finally describe the same transaction.
static void barter_draw_deal()
{
    if (ptable == NULL || btable == NULL || target_stack[0] == NULL) {
        return;
    }

    unsigned char* windowBuffer = win_get_buf(i_wid);
    if (windowBuffer == NULL) {
        return;
    }

    int give = item_total_cost(ptable);                           // raw value of the player's pile
    int want = barter_compute_value(inven_dude, target_stack[0]);  // adjusted price of the NPC's pile
    int shortBy = want - give;

    int oldFont = text_curr();
    text_font(101);
    int th = text_height();

    // ⚠️ THE LAYOUT IS DICTATED BY FOUR 64px COLUMNS AT x 29 / 165 / 250 / 388. The first attempt
    // put this in the gap between the two inner columns, which measures 250 - (165+64) = 17 PIXELS --
    // enough for nothing. The usable space is the strip to the RIGHT of the NPC's $total (drawn at
    // x 254) and before the NPC's inventory column starts at 388.
    const int dealX = 300;
    const int dealW = INVENTORY_TRADE_RIGHT_SCROLLER_X - dealX - 2;
    const int dealY = INVENTORY_SLOT_HEIGHT * inven_cur_disp + 24;

    // Both strips are FILLED first: this is background art underneath, not a flat panel, so text
    // drawn straight onto it would sit on whatever the frame happens to show there.
    if (dealW > 8) {
        buf_fill(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * dealY + dealX,
            dealW, th + 2, INVENTORY_TRADE_WINDOW_WIDTH, colorTable[0]);

        // OVERPAY is a deal the trader accepts but you are losing on -- exactly what X evens out, so
        // it is shown rather than hidden under "DEAL OK" (which is how a lopsided trade looked fine).
        char text[32];
        int color;
        if (shortBy > 0) {
            snprintf(text, sizeof(text), "SHORT %d", shortBy);
            color = colorTable[31744];
        } else if (shortBy < 0) {
            snprintf(text, sizeof(text), "OVERPAY %d", -shortBy);
            color = colorTable[32747];
        } else {
            snprintf(text, sizeof(text), "DEAL OK");
            color = colorTable[992];
        }

        int tw = text_width(text);
        int tx = dealX + (dealW - tw) / 2;
        if (tx < dealX) {
            tx = dealX;
        }
        text_to_buf(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (dealY + 1) + tx,
            text, dealW, INVENTORY_TRADE_WINDOW_WIDTH, color);
    }

    // THE CONTROL LEGEND, on screen where the controls are actually used. The Pip-Boy CONTROLS page
    // lists these too, but a page you have to go looking for is how "even out with caps" and the
    // split-stack picker sat unused for weeks -- if a control is not visible where it applies, it
    // does not exist. The top strip is free: the inner item columns start at y 20 and the outer at
    // y 30, so nothing is drawn across this band.
    // Full window width, not just the gap between the outer columns: at y 4 NOTHING is drawn
    // yet -- the outer item columns start at y 30 and the inner ones at y 20 -- so the whole
    // 480px strip is free, and the legend needs it once the navigation keys are in there too.
    const int legX = 6;
    const int legW = INVENTORY_TRADE_WINDOW_WIDTH - 12;
    const int legY = 4;

    if (legW > 40) {
        buf_fill(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * legY + legX,
            legW, th + 2, INVENTORY_TRADE_WINDOW_WIDTH, colorTable[0]);

        // ⚠️ THE LEGEND MUST NAME THE ACTUAL AXES. It read "LB/RB item" back when the shoulders
        // walked the flat left-to-right list of every occupied slot; they now step COLUMN (empty
        // ones skipped) and the D-pad steps ITEM within one column. Wording and binding change
        // together -- a legend promising a jump the buttons do not make is worse than none.
        const char* legend = "LB/RB column   D-pad item   A move   LT+A some   X even   Y offer   B exit";
        int lw = text_width(legend);
        int lx = legX + (legW - lw) / 2;
        if (lx < legX) {
            lx = legX;
        }
        text_to_buf(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (legY + 1) + lx,
            legend, legW, INVENTORY_TRADE_WINDOW_WIDTH, colorTable[32767]);
    }

    text_font(oldFont);

    Rect r;
    r.ulx = legX < dealX ? legX : dealX;
    r.uly = legY;
    r.lrx = (legX + legW) > (dealX + dealW) ? (legX + legW) : (dealX + dealW);
    r.lry = dealY + th + 2;
    win_draw_rect(i_wid, &r);
}

// ⚠️ "EVEN" IS MEASURED ON THE SCALE THE TRADER JUDGES WITH, AND THAT SCALE IS NOT THE PRICE TAG.
//
// The deal check is want <= give, where want = barter_compute_value (the trader's pile with the
// Barter-skill markup -- or markdown: a good trader pays well under the tag) and give = the RAW value
// of the player's pile. The SELL side used to balance against the trader's RAW total instead, which
// only agrees with `want` while the trader's pile is nothing but caps. Put a few items on BOTH sides
// and the two scales part: measured in trace_2026-10-04_153226, want 2131 against give 5022 -- the
// player overpaying by 2891 -- was "evened" with 822 caps, and every X after that refused as
// "already even" while the deal was still 2065 in the trader's favour ("even nefunguje správně s
// více itemy").
//
// Caps are the one thing BOTH scales count 1:1 (barter_compute_value takes them out before the
// modifier), so moving N caps moves the gap by exactly N. Each direction first takes back caps
// from the side that is ahead -- the player's own caps off the player's table, or the trader's off
// the trader's -- and only then adds new ones, so a press never shuffles caps both ways at once.
static void barter_auto_offer()
{
    Object* npc = target_stack[0];

    int want = barter_compute_value(inven_dude, npc); // what the trader asks for its pile
    int give = item_total_cost(ptable);               // what the player's pile is worth to it
    int gap = want - give;                            // > 0 player is short, < 0 player overpays

    int dudeCaps = 0;
    barter_find_money(inven_dude, &dudeCaps);
    int npcCaps = 0;
    barter_find_money(npc, &npcCaps);
    int ptableCaps = 0;
    barter_find_money(ptable, &ptableCaps);
    int btableCaps = 0;
    barter_find_money(btable, &btableCaps);

    // TEMP DIAG (strip after diagnosing barter auto-caps): flight-recorder ONLY.
    // NOTE: display_print CRASHES inside the barter/dialog modal (message monitor buffer
    // invalid), so diagnostics MUST go to the trace, never the screen.
    trace_event(TRACE_FLOW, "barter-auto",
        "want=%d give=%d gap=%d dudeCaps=%d npcCaps=%d ptableCaps=%d btableCaps=%d",
        want, give, gap, dudeCaps, npcCaps, ptableCaps, btableCaps);

    // Move up to `amount` caps from `from` to `to`; returns how many actually went. The money object
    // is looked up again for every move -- a move can merge or empty a stack, and a pointer fetched
    // before it may no longer be the stack it was.
    auto moveCaps = [](Object* from, Object* to, int amount) -> int {
        int have = 0;
        Object* money = barter_find_money(from, &have);
        if (money == NULL || have <= 0 || amount <= 0) {
            return 0;
        }
        int n = amount < have ? amount : have;
        return item_move_force(from, to, money, n) == 0 ? n : 0;
    };

    const char* why = NULL;
    int moved = 0;

    if (gap > 0) {
        // SHORT: the trader's own caps off its table first (each one off its pile is one off the
        // price), then the rest out of the player's pocket.
        int left = gap;
        int n = moveCaps(btable, npc, left);
        moved += n;
        left -= n;
        if (left > 0) {
            n = moveCaps(inven_dude, ptable, left);
            moved += n;
            left -= n;
        }
        if (moved == 0) {
            why = dudeCaps <= 0 ? "no-caps-dude" : "move-failed";
        }
    } else if (gap < 0) {
        // OVERPAYING: the player's own caps back off the player's table first, then the trader pays
        // the rest into its offer.
        int left = -gap;
        int n = moveCaps(ptable, inven_dude, left);
        moved += n;
        left -= n;
        if (left > 0) {
            n = moveCaps(npc, btable, left);
            moved += n;
            left -= n;
        }
        if (moved == 0) {
            why = npcCaps <= 0 ? "no-caps-npc" : "move-failed";
        }
    } else {
        why = "already-even";
    }

    if (moved > 0) {
        gsound_play_sfx_file("iputdown");
        barter_redraw_tables();
        trace_event(TRACE_INPUT, "barter-auto-moved", "caps=%d gap=%d", moved, gap);
        return;
    }

    // ⚠️ IT MUST NEVER REFUSE IN SILENCE. "even xko mi někdy nefungovalo" -- and it had not failed,
    // it had DECLINED, three different ways, with no sound and no message: nobody is short of
    // anything (the deal is already even), the buyer has no caps, the seller has no caps. A button
    // that does nothing and says nothing reads as broken every time. The refusal buzzer is the same
    // one the reload and every other refused action on this pad uses.
    //
    // ⚠️ And it has to be a SOUND, not text: display_print crashes inside the barter/dialog modal
    // (invalid message-monitor buffer), which is why the reason goes to the trace instead.
    gsound_play_sfx_file("iisxxxx1");
    trace_event(TRACE_INPUT, "barter-auto-refused", "why=%s gap=%d dudeCaps=%d npcCaps=%d",
        why != NULL ? why : "?", gap, dudeCaps, npcCaps);
}

// Total item count and current scroll offset of trade column c (0 player inv, 1 player
// offer, 2 NPC offer, 3 NPC inv). Used by the D-pad navigation to know how many items are
// below/above the visible window so it can scroll only at the edge.
static void barter_col_range(int c, int* length, int* offset)
{
    switch (c) {
    case 0:
        *length = pud->length;
        *offset = stack_offset[curr_stack];
        break;
    case 1:
        *length = ptable_pud->length;
        *offset = ptable_offset;
        break;
    case 2:
        *length = btable_pud->length;
        *offset = btable_offset;
        break;
    default:
        *length = target_pud->length;
        *offset = target_stack_offset[target_curr_stack];
        break;
    }
}

// Which of the four lists is the cursor nearest, by x alone? Used when the cursor is not over
// an occupied slot, so the shoulders and the D-pad anchor onto the same column.
static int barter_nearest_column(int rx)
{
    static const int colsX[4] = {
        INVENTORY_TRADE_LEFT_SCROLLER_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_X,
        INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X, INVENTORY_TRADE_RIGHT_SCROLLER_X
    };

    int c = 0;
    int bestDx = INT_MAX;
    for (int i = 0; i < 4; i++) {
        int cx = colsX[i] + INVENTORY_SLOT_WIDTH / 2;
        int dx = rx < cx ? cx - rx : rx - cx;
        if (dx < bestDx) {
            bestDx = dx;
            c = i;
        }
    }
    return c;
}

// How many occupied visible rows does column c have in slots[]? barter_collect_slots fills each
// column top-down from row 0, so this doubles as "the row past the last one".
static int barter_col_rows(const BarterSlot* slots, int nSlots, int c)
{
    int rows = 0;
    for (int i = 0; i < nSlots; i++) {
        if (slots[i].listId == c) {
            rows++;
        }
    }
    return rows;
}

// Warp the cursor onto column c's visible row `row`, if that slot is present in slots[].
static void barter_warp_to_slot(const BarterSlot* slots, int nSlots, int c, int row)
{
    for (int i = 0; i < nSlots; i++) {
        if (slots[i].listId == c && slots[i].row == row) {
            barter_warp_cursor(slots[i].x0 + INVENTORY_SLOT_WIDTH / 2,
                slots[i].y0 + INVENTORY_SLOT_HEIGHT / 2);
            return;
        }
    }
}

// CE: Controller navigation for the barter/trade window. The four item lists --
// player inventory, player's offer table, NPC's offer table, NPC inventory -- are
// walked as one flat left-to-right order. LB/RB step the cursor to the previous/next
// COLUMN (wrapping, and skipping any column with nothing occupied in it); the D-pad moves
// between the items inside one column. A transfers the WHOLE stack in the
// slot UNDER the cursor directly (barter_transfer_slot), so it does not depend on the
// hook's synthetic click (disabled for TRADE in gpui_inventory_a). X auto-fills the
// offer with exactly the caps the NPC is asking (barter_auto_offer). D-pad up/down
// moves the cursor between items in the current column and only scrolls at the top/
// bottom edge, so every item (incl. the last visible one) is reachable. Y offers the
// trade (same as 'm'); B / BACK leave (KEY_ESCAPE). Read SDL directly -- gamepad_ui_-
// tick does not run inside barter's own modal loop (same pattern as inven_gamepad_scroll_key).
//
// Returns an injected keyCode (or -1). Cursor moves and the A transfer are side
// effects, so this must be called every frame regardless of the return value.
// Stick deflection that counts as a direction, and how fast a held direction repeats. 160 ms is
// brisk enough to run a long list in one push without overshooting a three-row column.
#define BARTER_STICK_THRESHOLD 0.5f
#define BARTER_NAV_REPEAT_MS 160

static int barter_gamepad_input()
{
    static bool prevLB = false;
    static bool prevRB = false;
    static bool prevRT = false;
    static bool prevA = false;
    static bool prevX = false;
    static bool prevY = false;
    static bool prevB = false;
    static bool prevBack = false;
    static bool prevUp = false;
    static bool prevDown = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevLB = prevRB = prevRT = prevA = prevX = prevY = prevB = prevBack = prevUp = prevDown = false;
        return -1;
    }

    SDL_GameControllerUpdate();

    bool lbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    bool rbNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool backNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
    // RT asks how many. ⚠️ Read through gamepad_ui_split_trigger_held's sibling rule -- a held
    // trigger intermittently reports a hard 0 (measured: 14 of 33 readings), so a bare threshold
    // drops presses. Here it is only an EDGE on a press, so the Schmitt low bound is enough.
    bool rtNow = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;

    // ⚠️ SAMPLED EVERY FRAME, AND THAT IS NOT OPTIONAL. The split modifier is filtered by agreement
    // across readings (gamepad_ui_split_trigger_held), because this axis reports garbage in both
    // directions; asking it once, at the moment A is released, hands the filter a single sample and
    // it can then neither confirm a real pull nor out-vote a spurious one. This loop runs every
    // frame, so it is what feeds it.
    bool splitNow = gamepad_ui_split_trigger_held();

    // ⚠️ THE LEFT STICK STEPS TOO, AND EVERYTHING REPEATS WHEN HELD. His two asks in one breath:
    // *"Proč nepoužíváme i joystick? Můžu držet dpad aby se rychleji posouvalo?"*. The left stick is
    // free on this screen -- barter_inventory suppresses the cursor snap for its whole length -- so
    // it drives exactly the same four steps the D-pad does, and both of them now act on the first
    // frame and then repeat while held. The right stick stays the free pointer it always was.
    float navX = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    float navY = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    float navAbsX = navX < 0.0f ? -navX : navX;
    float navAbsY = navY < 0.0f ? -navY : navY;

    // The dominant axis wins outright: a diagonal must not step a row AND a column at once.
    bool stickUp = navAbsY >= navAbsX && navY < -BARTER_STICK_THRESHOLD;
    bool stickDown = navAbsY >= navAbsX && navY > BARTER_STICK_THRESHOLD;
    bool stickLeft = navAbsX > navAbsY && navX < -BARTER_STICK_THRESHOLD;
    bool stickRight = navAbsX > navAbsY && navX > BARTER_STICK_THRESHOLD;

    upNow = upNow || stickUp;
    downNow = downNow || stickDown;
    bool colLeftNow = lbNow || stickLeft;
    bool colRightNow = rbNow || stickRight;

    // One repeat clock for all four directions: the newest direction owns it, so sliding the stick
    // from down to left does not inherit the old timer and fire instantly.
    static int navHeld = 0; // 0 none, 1 up, 2 down, 3 left, 4 right
    static unsigned int navAt = 0;
    int navNow = upNow ? 1 : (downNow ? 2 : (colLeftNow ? 3 : (colRightNow ? 4 : 0)));
    bool navRepeat = false;
    if (navNow == 0) {
        navHeld = 0;
    } else if (navNow != navHeld) {
        navHeld = navNow;
        navAt = get_time();
    } else if (elapsed_time(navAt) >= BARTER_NAV_REPEAT_MS) {
        navAt = get_time();
        navRepeat = true;
    }

    bool lbEdge = (colLeftNow && !prevLB) || (navRepeat && navNow == 3);
    bool rbEdge = (colRightNow && !prevRB) || (navRepeat && navNow == 4);
    bool aEdge = aNow && !prevA;
    bool aReleased = !aNow && prevA;
    bool xEdge = xNow && !prevX;
    bool yEdge = yNow && !prevY;
    bool bEdge = bNow && !prevB;
    bool backEdge = backNow && !prevBack;
    bool rtEdge = rtNow && !prevRT;
    bool upEdge = (upNow && !prevUp) || (navRepeat && navNow == 1);
    bool downEdge = (downNow && !prevDown) || (navRepeat && navNow == 2);

    prevLB = colLeftNow;
    prevRB = colRightNow;
    prevRT = rtNow;
    prevA = aNow;
    prevX = xNow;
    prevY = yNow;
    prevB = bNow;
    prevBack = backNow;
    prevUp = upNow;
    prevDown = downNow;

    // X: auto-fill the player's offer with exactly the caps the NPC is asking for.
    if (xEdge) {
        barter_auto_offer();
        return -1;
    }

    Rect winRect;
    BarterSlot slots[4 * 3];
    int nSlots = barter_collect_slots(slots, 4 * 3, &winRect);

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);

    // Index of the occupied slot the cursor is over (fully inside its 64x48 rect), or -1.
    int over = -1;
    for (int i = 0; i < nSlots; i++) {
        if (mouseX >= slots[i].x0 && mouseX < slots[i].x0 + INVENTORY_SLOT_WIDTH
            && mouseY >= slots[i].y0 && mouseY < slots[i].y0 + INVENTORY_SLOT_HEIGHT) {
            over = i;
            break;
        }
    }

    // ⚠️ HOLD A TO LOOK AT AN ITEM. A trade screen without a description is guesswork -- you cannot
    // tell two similar guns apart, and there was no way at all to read one on a controller ("potřebuji
    // se podívat na item v shopu, nevím jak toho na controlleru dosáhnout"). The mouse reaches it
    // through the hold-drag action menu; the pad now reaches it the same way the INVENTORY already
    // does, by holding A, so it is one habit and not two.
    //
    // Printed with obj_examine_func(..., display_msg) -- exactly what vanilla's own LOOK does in
    // this window (see GAME_MOUSE_ACTION_MENU_ITEM_LOOK: every non-NORMAL window uses display_msg,
    // because the inventory's description panel does not exist here).
    // DIAG (the "I could not get at the items in my own offer" report, 2026-09-25): one line per
    // navigation press, saying which slot the cursor is on, how many slots were collected at all,
    // and the length + scroll offset of all four lists. Between them those numbers say whether a
    // column was never collected, never scrolled, or simply never pointed at -- three different
    // bugs that I could not tell apart by reading the code. Strip with `move-diag`.
    if (trace_enabled() && (upEdge || downEdge || lbEdge || rbEdge)) {
        trace_event(TRACE_INPUT, "barter-nav",
            "over=%d col=%d row=%d slots=%d mine=%d/%d offer=%d/%d theirs=%d/%d npc=%d/%d m=%d,%d",
            over, over >= 0 ? slots[over].listId : -1, over >= 0 ? slots[over].row : -1, nSlots,
            pud->length, stack_offset[curr_stack],
            ptable_pud->length, ptable_offset,
            btable_pud->length, btable_offset,
            target_pud->length, target_stack_offset[target_curr_stack],
            mouseX - winRect.ulx, mouseY - winRect.uly);
    }

    static unsigned int barterADownTime = 0;
    static bool barterAHandled = false;
    if (aEdge) {
        barterADownTime = get_time();
        barterAHandled = false;
    }
    if (aNow && !barterAHandled && over >= 0
        && elapsed_time(barterADownTime) >= BARTER_LOOK_HOLD_MS) {
        barterAHandled = true;
        Object* looked = barter_slot_item(slots[over].listId, slots[over].row);
        if (looked != NULL) {
            obj_examine_func(inven_dude, looked, display_msg);
        }
        return -1;
    }

    // RT: HOW MANY? Opens the quantity picker for the item under the cursor.
    //
    // ⚠️ IT IS ITS OWN BUTTON BECAUSE THE COMBINATION WAS NOT WORTH IT. It lived on LT+A-held for
    // about ten minutes and he was right about it: "není to zbytečně komplikované? prostě přidej
    // jiný button". Three amounts on one button meant a tap, a modifier, and a hold length all
    // carrying meaning at once, and you had to get the order right. RT had no job on this screen at
    // all, and it sits beside LT, which is the other amount key. A = one, LT+A = all, RT = ask.
    if (rtEdge && over >= 0) {
        barter_transfer_slot(slots[over].listId, slots[over].row, BARTER_AMOUNT_PICK);
        barter_reanchor_cursor(mouseX, mouseY);
        return -1;
    }

    // A transfers the item under the cursor: ONE of it, or the WHOLE stack with LT held. On the
    // RELEASE edge, because the hold above owns the press: a tap moves, a hold describes.
    //
    // ⚠️ THE SPLIT STAYS ON LT WHILE EVERY OTHER COMBO MOVED TO LB (2026-08-29). The tempting move
    // is consistency -- LB is the modifier everywhere else now -- but LB is NOT free on this screen:
    // lbEdge steps the cursor to the previous column (see the rbEdge || lbEdge branch below).
    // Holding LB to split would jump the cursor off the item you were pointing at before A was ever
    // pressed, which breaks the gesture outright. LT has no job in a barter window, so it keeps it.
    if (aReleased && !barterAHandled && over >= 0) {
        // ⚠️ NEVER A BARE THRESHOLD ON A TRIGGER. A held trigger intermittently reports a hard 0
        // (measured: 14 of 33 readings), so `axis > 8000` dropped the split request about four
        // times in ten and moved the whole stack. gamepad_ui_split_trigger_held is the Schmitt
        // trigger + 60 ms release debounce every other analog read in this codebase goes through.
        int listId = slots[over].listId;
        int row = slots[over].row;

        // DIAG (the "A keeps moving the whole stack" report, 2026-09-24): one line per press,
        // naming the slot, what the trigger ACTUALLY read, what the filter made of it, and which
        // amount that chose. Reported three times and never once measured -- this is what tells a
        // stuck modifier apart from a stuck stack. Strip with `move-diag`.
        if (trace_enabled()) {
            int rawLt = 0;
            int level = 0;
            gamepad_ui_split_trigger_debug(&rawLt, &level);
            Object* it = barter_slot_item(listId, row);
            trace_event(TRACE_INPUT, "barter-a", "list=%d row=%d pid=%d lt=%d lvl=%d amount=%s",
                listId, row, it != NULL ? it->pid : -1, rawLt, level, splitNow ? "ALL" : "ONE");
        }

        barter_transfer_slot(listId, row, splitNow ? BARTER_AMOUNT_ALL : BARTER_AMOUNT_ONE);

        barter_reanchor_cursor(mouseX, mouseY);
        return -1;
    }

    // LB/RB: step to the previous/next COLUMN. The shoulders are the column axis, the D-pad is
    // the item axis -- one gesture, one meaning. They used to walk the flat left-to-right list of
    // every occupied slot, crossing into the next column only once the current one ran out, so
    // "go to the other pile" cost as many presses as that pile had items.
    //
    // Empty columns are SKIPPED, not landed on: with no occupied slot there is nothing to point
    // at and A would do nothing, so stopping there would just strand the cursor. The row carries
    // across (clamped to what the target column has), so a sideways step lands level.
    if ((rbEdge || lbEdge) && nSlots > 0) {
        int c;
        int row;
        if (over >= 0) {
            c = slots[over].listId;
            row = slots[over].row;
        } else {
            c = barter_nearest_column(mouseX - winRect.ulx);
            row = 0;
        }

        // ⚠️ STEP INTO THE NEXT COLUMN EVEN WHEN IT IS EMPTY. Empty columns used to be skipped --
        // "there is nothing to point at and A would do nothing there" -- and that reasoning was
        // wrong in the hand: an offer table is empty exactly at the start of every trade, so the
        // shoulder button silently jumped PAST the table you were trying to reach and the player
        // could not tell whether he had pressed it at all ("když nejsou itemy v columnu, tak na to
        // neskáče LB nebo RB"). A cursor sitting on an empty slot is honest -- it shows where you
        // are -- and the actions there simply do nothing, which is the same answer the mouse gives.
        //
        // One step, one column, always: no searching, no wrapping past anything.
        int dir = rbEdge ? 1 : -1;
        int target = (c + dir + 4) % 4;
        int rows = barter_col_rows(slots, nSlots, target);
        if (rows > 0) {
            barter_warp_to_slot(slots, nSlots, target, row < rows ? row : rows - 1);
        } else {
            // No slots collected for that column -- it is empty, so put the cursor on where its
            // first row WOULD be. The geometry is fixed by the window art, not by the contents.
            static const int colsX[4] = {
                INVENTORY_TRADE_LEFT_SCROLLER_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_X,
                INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X, INVENTORY_TRADE_RIGHT_SCROLLER_X
            };
            barter_warp_cursor(winRect.ulx + colsX[target] + INVENTORY_SLOT_WIDTH / 2,
                winRect.uly + INVENTORY_TRADE_SCROLLER_Y + INVENTORY_SLOT_HEIGHT / 2);
        }
        return -1;
    }

    // Y offers the trade; B / BACK leave.
    if (yEdge) {
        return KEY_LOWERCASE_M;
    }
    if (bEdge || backEdge) {
        return KEY_ESCAPE;
    }

    // D-pad up/down: move the cursor between items in one column. It advances the SELECTION
    // through the visible slots first and only SCROLLS the list once the cursor is already on
    // the top/bottom visible slot and there are more items off-screen. This is what lets you
    // reach the middle and last items -- the old "scroll only" behaviour left the cursor stuck
    // on the first visible slot, so the 2nd/3rd items were unreachable once scrolling maxed out.
    if (upEdge || downEdge) {
        static const int upKeys[4] = { KEY_ARROW_UP, KEY_PAGE_UP, KEY_CTRL_PAGE_UP, KEY_CTRL_ARROW_UP };
        static const int downKeys[4] = { KEY_ARROW_DOWN, KEY_PAGE_DOWN, KEY_CTRL_PAGE_DOWN, KEY_CTRL_ARROW_DOWN };

        // Which column, and which visible row within it, is the cursor on? If it isn't over a
        // slot yet, anchor onto the nearest column (first slot on a down press, last on up).
        int c;
        int row;
        if (over >= 0) {
            c = slots[over].listId;
            row = slots[over].row;
        } else {
            c = barter_nearest_column(mouseX - winRect.ulx);
            row = -1;
        }

        int length;
        int offset;
        barter_col_range(c, &length, &offset);
        int remaining = length - offset;
        int visible = remaining < inven_cur_disp ? remaining : inven_cur_disp;
        if (visible <= 0) {
            return -1;
        }

        if (row < 0) {
            // Not anchored yet: land on the first (down) / last (up) visible item.
            barter_warp_to_slot(slots, nSlots, c, downEdge ? 0 : visible - 1);
            return -1;
        }

        if (downEdge) {
            if (row + 1 < visible) {
                barter_warp_to_slot(slots, nSlots, c, row + 1); // next item down
                return -1;
            }
            if (remaining > inven_cur_disp) {
                return downKeys[c]; // at bottom edge, more below -> scroll (cursor stays on last row)
            }
            return -1; // already on the last item
        }

        // upEdge
        if (row - 1 >= 0) {
            barter_warp_to_slot(slots, nSlots, c, row - 1); // next item up
            return -1;
        }
        if (offset > 0) {
            return upKeys[c]; // at top edge, more above -> scroll (cursor stays on first row)
        }
        return -1; // already on the first item
    }

    return -1;
}

// 0x4684E4
void barter_inventory(int win, Object* a2, Object* a3, Object* a4, int a5)
{
    barter_mod = a5;

    if (inven_init() == -1) {
        return;
    }

    Object* armor = inven_worn(a2);
    if (armor != NULL) {
        item_remove_mult(a2, armor, 1);
    }

    Object* item1 = NULL;
    Object* item2 = inven_right_hand(a2);
    if (item2 != NULL) {
        item_remove_mult(a2, item2, 1);
    } else {
        item1 = inven_find_type(a2, ITEM_TYPE_WEAPON, NULL);
        if (item1 != NULL) {
            item_remove_mult(a2, item1, 1);
        }
    }

    Object* a1a = NULL;
    if (obj_new(&a1a, 0, 467) == -1) {
        return;
    }

    pud = &(inven_dude->data.inventory);
    btable = a4;
    ptable = a3;

    ptable_offset = 0;
    btable_offset = 0;

    ptable_pud = &(a3->data.inventory);
    btable_pud = &(a4->data.inventory);

    barter_back_win = win;
    target_curr_stack = 0;
    target_pud = &(a2->data.inventory);

    target_stack[0] = a2;
    target_stack_offset[0] = 0;

    bool isoWasEnabled = setup_inventory(INVENTORY_WINDOW_TYPE_TRADE);
    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
    display_inventory(stack_offset[0], -1, INVENTORY_WINDOW_TYPE_TRADE);
    display_body(a2->fid, INVENTORY_WINDOW_TYPE_TRADE);
    win_draw(barter_back_win);
    display_table_inventories(win, a3, a4, -1);

    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);

    int modifier;
    int npcReactionValue = reaction_get(a2);
    int npcReactionType = reaction_to_level(npcReactionValue);
    switch (npcReactionType) {
    case NPC_REACTION_BAD:
        modifier = -25;
        break;
    case NPC_REACTION_NEUTRAL:
        modifier = 0;
        break;
    case NPC_REACTION_GOOD:
        modifier = 50;
        break;
    default:
        assert(false && "Should be unreachable");
    }

    // CE: While the barter window owns the screen, suppress the left-stick item snap
    // (gpui_menu_snap would grab i_wid's slot/arrow buttons and fight the LB/RB cursor
    // stepping). Cleared right after the loop so menus afterwards snap normally.
    gamepad_ui_set_suppress_snap(true);

    // CE: START ON YOUR OWN FIRST ITEM. The window used to open with the pointer wherever the world
    // had left it, and LB/RB step RELATIVE to the column it happens to sit in -- so on a controller
    // the first move was always a guess, and getting back to your own stack was the part that felt
    // worst ("mám problém se dostat do mého column při tradeu"). Column 0 is the player's
    // inventory; row 0 of it is where every trade actually begins.
    //
    // Guarded on a controller being present, exactly like the loot window's opening warp: never
    // move a mouse player's pointer out from under their hand.
    if (gamepad_ui_get_controller() != NULL) {
        Rect bWinRect;
        BarterSlot bSlots[4 * 3];
        int bCount = barter_collect_slots(bSlots, 4 * 3, &bWinRect);
        if (bCount > 0) {
            barter_warp_to_slot(bSlots, bCount, 0, 0);
        }
    }

    int keyCode = -1;
    for (;;) {
        sharedFpsLimiter.mark();

        if (keyCode == KEY_ESCAPE || game_user_wants_to_quit != 0) {
            break;
        }

        keyCode = get_input();

        // CE: Controller input for the trade window. Runs every frame (LB/RB move the
        // cursor as a side effect); its injected keyCode only applies when nothing else did.
        int padKey = barter_gamepad_input();
        if (keyCode == -1 && padKey != -1) {
            keyCode = padKey;
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            game_quit_with_confirm();
        }

        if (game_user_wants_to_quit != 0) {
            break;
        }

        barter_mod = a5 + modifier;

        if (keyCode == KEY_LOWERCASE_T || modifier <= -30) {
            item_move_all(a4, a2);
            item_move_all(a3, obj_dude);
            barter_end_to_talk_to();
            break;
        } else if (keyCode == KEY_LOWERCASE_M) {
            if (a3->data.inventory.length != 0 || btable->data.inventory.length != 0) {
                MessageListItem messageListItem;
                if (barter_attempt_transaction(inven_dude, a3, a2, a4) == 0) {
                    display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                    display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                    display_table_inventories(win, a3, a4, -1);

                    // Ok, that's a good trade.
                    messageListItem.num = 27;
                } else {
                    //  No, your offer is not good enough.
                    messageListItem.num = 28;
                }

                if (message_search(&inventry_message_file, &messageListItem)) {
                    gdialog_display_msg(messageListItem.text);
                }
            }
        } else if (keyCode == KEY_ARROW_UP) {
            if (stack_offset[curr_stack] > 0) {
                stack_offset[curr_stack] -= 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
            }
        } else if (keyCode == KEY_PAGE_UP) {
            if (ptable_offset > 0) {
                ptable_offset -= 1;
                display_table_inventories(win, a3, a4, -1);
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (stack_offset[curr_stack] + inven_cur_disp < pud->length) {
                stack_offset[curr_stack] += 1;
                display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
            }
        } else if (keyCode == KEY_PAGE_DOWN) {
            if (ptable_offset + inven_cur_disp < ptable_pud->length) {
                ptable_offset += 1;
                display_table_inventories(win, a3, a4, -1);
            }
        } else if (keyCode == KEY_CTRL_PAGE_DOWN) {
            if (btable_offset + inven_cur_disp < btable_pud->length) {
                btable_offset++;
                display_table_inventories(win, a3, a4, -1);
            }
        } else if (keyCode == KEY_CTRL_PAGE_UP) {
            if (btable_offset > 0) {
                btable_offset -= 1;
                display_table_inventories(win, a3, a4, -1);
            }
        } else if (keyCode == KEY_CTRL_ARROW_UP) {
            if (target_stack_offset[target_curr_stack] > 0) {
                target_stack_offset[target_curr_stack] -= 1;
                display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                win_draw(i_wid);
            }
        } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
            if (target_stack_offset[target_curr_stack] + inven_cur_disp < target_pud->length) {
                target_stack_offset[target_curr_stack] += 1;
                display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                win_draw(i_wid);
            }
        } else if (keyCode >= 2500 && keyCode <= 2501) {
            container_exit(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
        } else {
            if ((mouse_get_buttons() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (immode == INVENTORY_WINDOW_CURSOR_HAND) {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inven_set_mouse(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1000 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        display_table_inventories(win, a3, NULL, -1);
                    } else {
                        int index = keyCode - 1000;
                        if (index + stack_offset[curr_stack] < pud->length) {
                            InventoryItem* inventoryItem = &(pud->items[index + stack_offset[curr_stack]]);
                            barter_move_inventory(inventoryItem->item, inventoryItem->quantity, index, stack_offset[curr_stack], a2, a3, true);
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            display_table_inventories(win, a3, NULL, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2000 && keyCode <= 2000 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        display_table_inventories(win, NULL, a4, -1);
                    } else {
                        int index = keyCode - 2000;
                        if (index + target_stack_offset[target_curr_stack] < target_pud->length) {
                            InventoryItem* inventoryItem = &(target_pud->items[index + target_stack_offset[target_curr_stack]]);
                            barter_move_inventory(inventoryItem->item, inventoryItem->quantity, index, target_stack_offset[target_curr_stack], a2, a4, false);
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            display_table_inventories(win, NULL, a4, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2300 && keyCode <= 2300 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        display_table_inventories(win, a3, NULL, -1);
                    } else {
                        int index = keyCode - 2300;
                        if (index < ptable_pud->length) {
                            InventoryItem* inventoryItem = &(ptable_pud->items[index + ptable_offset]);
                            barter_move_from_table_inventory(inventoryItem->item, inventoryItem->quantity, index, a2, a3, true);
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            display_table_inventories(win, a3, NULL, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2400 && keyCode <= 2400 + inven_cur_disp) {
                    if (immode == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inven_action_cursor(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        display_table_inventories(win, NULL, a4, -1);
                    } else {
                        int index = keyCode - 2400;
                        if (index < btable_pud->length) {
                            InventoryItem* inventoryItem = &(btable_pud->items[index + btable_offset]);
                            barter_move_from_table_inventory(inventoryItem->item, inventoryItem->quantity, index, a2, a4, false);
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            display_table_inventories(win, NULL, a4, -1);
                        }
                    }

                    keyCode = -1;
                }
            } else if ((mouse_get_buttons() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (stack_offset[curr_stack] > 0) {
                            stack_offset[curr_stack] -= 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                        }
                    } else if (wheelY < 0) {
                        if (stack_offset[curr_stack] + inven_cur_disp < pud->length) {
                            stack_offset[curr_stack] += 1;
                            display_inventory(stack_offset[curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                        }
                    }
                } else if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (ptable_offset > 0) {
                            ptable_offset -= 1;
                            display_table_inventories(win, a3, a4, -1);
                        }
                    } else if (wheelY < 0) {
                        if (ptable_offset + inven_cur_disp < ptable_pud->length) {
                            ptable_offset += 1;
                            display_table_inventories(win, a3, a4, -1);
                        }
                    }
                } else if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (target_stack_offset[target_curr_stack] > 0) {
                            target_stack_offset[target_curr_stack] -= 1;
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            win_draw(i_wid);
                        }
                    } else if (wheelY < 0) {
                        if (target_stack_offset[target_curr_stack] + inven_cur_disp < target_pud->length) {
                            target_stack_offset[target_curr_stack] += 1;
                            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            win_draw(i_wid);
                        }
                    }
                } else if (mouseHitTestInWindow(i_wid, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * inven_cur_disp + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (btable_offset > 0) {
                            btable_offset -= 1;
                            display_table_inventories(win, a3, a4, -1);
                        }
                    } else if (wheelY < 0) {
                        if (btable_offset + inven_cur_disp < btable_pud->length) {
                            btable_offset += 1;
                            display_table_inventories(win, a3, a4, -1);
                        }
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    gamepad_ui_set_suppress_snap(false);

    item_move_all(a1a, a2);
    obj_erase_object(a1a, NULL);

    if (armor != NULL) {
        armor->flags |= OBJECT_WORN;
        item_add_force(a2, armor, 1);
    }

    if (item2 != NULL) {
        item2->flags |= OBJECT_IN_RIGHT_HAND;
        item_add_force(a2, item2, 1);
    }

    if (item1 != NULL) {
        item_add_force(a2, item1, 1);
    }

    exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inven_exit();
}

// 0x468E58
void container_enter(int keyCode, int inventoryWindowType)
{
    if (keyCode >= 2000) {
        int index = target_stack_offset[target_curr_stack] + keyCode - 2000;
        if (index < target_pud->length && target_curr_stack < 9) {
            InventoryItem* inventoryItem = &(target_pud->items[index]);
            Object* item = inventoryItem->item;
            if (item_get_type(item) == ITEM_TYPE_CONTAINER) {
                target_curr_stack += 1;
                target_stack[target_curr_stack] = item;
                target_stack_offset[target_curr_stack] = 0;

                target_pud = &(item->data.inventory);

                display_body(item->fid, inventoryWindowType);
                display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, inventoryWindowType);
                win_draw(i_wid);
            }
        }
    } else {
        int index = stack_offset[curr_stack] + keyCode - 1000;
        if (index < pud->length && curr_stack < 9) {
            InventoryItem* inventoryItem = &(pud->items[index]);
            Object* item = inventoryItem->item;
            if (item_get_type(item) == ITEM_TYPE_CONTAINER) {
                curr_stack += 1;

                stack[curr_stack] = item;
                stack_offset[curr_stack] = 0;

                inven_dude = stack[curr_stack];
                pud = &(item->data.inventory);

                adjust_fid();
                display_body(-1, inventoryWindowType);
                display_inventory(stack_offset[curr_stack], -1, inventoryWindowType);
            }
        }
    }
}

// 0x468FC0
void container_exit(int keyCode, int inventoryWindowType)
{
    if (keyCode == 2500) {
        if (curr_stack > 0) {
            curr_stack -= 1;
            inven_dude = stack[curr_stack];
            pud = &inven_dude->data.inventory;
            adjust_fid();
            display_body(-1, inventoryWindowType);
            display_inventory(stack_offset[curr_stack], -1, inventoryWindowType);
        }
    } else if (keyCode == 2501) {
        if (target_curr_stack > 0) {
            target_curr_stack -= 1;
            Object* v5 = target_stack[target_curr_stack];
            target_pud = &(v5->data.inventory);
            display_body(v5->fid, inventoryWindowType);
            display_target_inventory(target_stack_offset[target_curr_stack], -1, target_pud, inventoryWindowType);
            win_draw(i_wid);
        }
    }
}

// 0x469090
int drop_into_container(Object* a1, Object* a2, int a3, Object** a4, int quantity)
{
    int quantityToMove;
    if (quantity > 1) {
        quantityToMove = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, a2, quantity);
    } else {
        quantityToMove = 1;
    }

    if (quantityToMove == -1) {
        return -1;
    }

    if (a3 != -1) {
        if (item_remove_mult(inven_dude, a2, quantityToMove) == -1) {
            return -1;
        }
    }

    int rc = item_add_mult(a1, a2, quantityToMove);
    if (rc != 0) {
        if (a3 != -1) {
            item_add_mult(inven_dude, a2, quantityToMove);
        }
    } else {
        if (a4 != NULL) {
            if (a4 == &i_worn) {
                adjust_ac(stack[0], i_worn, NULL);
            }
            *a4 = NULL;
        }
    }

    return rc;
}

// 0x469130
int drop_ammo_into_weapon(Object* weapon, Object* ammo, Object** a3, int quantity, int keyCode)
{
    if (item_get_type(weapon) != ITEM_TYPE_WEAPON) {
        return -1;
    }

    if (item_get_type(ammo) != ITEM_TYPE_AMMO) {
        return -1;
    }

    if (!item_w_can_reload(weapon, ammo)) {
        return -1;
    }

    int quantityToMove;
    if (quantity > 1) {
        quantityToMove = do_move_timer(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, ammo, quantity);
    } else {
        quantityToMove = 1;
    }

    if (quantityToMove == -1) {
        return -1;
    }

    Object* v14 = ammo;
    bool v17 = false;
    int rc = item_remove_mult(inven_dude, weapon, 1);
    for (int index = 0; index < quantityToMove; index++) {
        int v11 = item_w_reload(weapon, v14);
        if (v11 == 0) {
            if (a3 != NULL) {
                *a3 = NULL;
            }

            obj_destroy(v14);

            v17 = true;
            if (inven_from_button(keyCode, &v14, NULL, NULL) == 0) {
                break;
            }
        }
        if (v11 != -1) {
            v17 = true;
        }
        if (v11 != 0) {
            break;
        }
    }

    if (rc != -1) {
        item_add_force(inven_dude, weapon, 1);
    }

    if (!v17) {
        return -1;
    }

    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon, HIT_MODE_RIGHT_WEAPON_PRIMARY, NULL);
    gsound_play_sfx_file(sfx);

    return 0;
}

// 0x469258
void draw_amount(int value, int inventoryWindowType)
{
    // BIGNUM.frm
    CacheEntry* handle;
    int fid = art_id(OBJ_TYPE_INTERFACE, 170, 0, 0, 0);
    unsigned char* data = art_ptr_lock_data(fid, 0, 0, &handle);
    if (data == NULL) {
        return;
    }

    Rect rect;

    int windowWidth = win_width(mt_wid);
    unsigned char* windowBuffer = win_get_buf(mt_wid);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        rect.ulx = 153;
        rect.uly = 45;
        rect.lrx = 195;
        rect.lry = 69;

        int ranks[3];
        ranks[2] = value % 10;
        ranks[1] = value / 10 % 10;
        ranks[0] = value / 100 % 10;

        windowBuffer += rect.uly * windowWidth + rect.ulx;

        for (int index = 0; index < 3; index++) {
            unsigned char* src = data + 14 * ranks[index];
            buf_to_buf(src, 14, 24, 336, windowBuffer, windowWidth);
            windowBuffer += 14;
        }
    } else {
        rect.ulx = 133;
        rect.uly = 64;
        rect.lrx = 189;
        rect.lry = 88;

        windowBuffer += windowWidth * rect.uly + rect.ulx;
        buf_to_buf(data + 14 * (value / 60), 14, 24, 336, windowBuffer, windowWidth);
        buf_to_buf(data + 14 * (value % 60 / 10), 14, 24, 336, windowBuffer + 14 * 2, windowWidth);
        buf_to_buf(data + 14 * (value % 10), 14, 24, 336, windowBuffer + 14 * 3, windowWidth);
    }

    art_ptr_unlock(handle);
    win_draw_rect(mt_wid, &rect);
}

static bool mt_pad_arm = true;

// Re-arm the picker's gamepad edge detection so a button still held from whatever opened
// the picker (e.g. LT+A in barter) doesn't instantly fire on the first frame.
static void move_timer_gamepad_reset()
{
    mt_pad_arm = true;
}

// CE: Gamepad control for the quantity picker (do_move_timer). A = confirm (KEY_RETURN),
// B = cancel (KEY_ESCAPE), Y = set to max (5000), D-pad Up/Right = +1 (6000), D-pad
// Down/Left = -1 (7000). The +/- steps auto-repeat while held so you can ramp the count
// up quickly. Read SDL directly -- gamepad_ui_tick doesn't run in this modal loop.
static int move_timer_gamepad_keycode()
{
    static bool prevA = false;
    static bool prevB = false;
    static bool prevY = false;
    static bool prevX = false;
    static bool prevInc = false;
    static bool prevDec = false;
    static Uint32 lastStep = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevA = prevB = prevY = prevX = prevInc = prevDec = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;
    bool incNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
        || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
    bool decNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
        || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;

    // First frame of a picker session: adopt the current button states as the baseline so
    // a held A/LT (used to OPEN the picker) isn't read as a confirm edge.
    if (mt_pad_arm) {
        prevA = aNow;
        prevB = bNow;
        prevY = yNow;
        prevX = xNow;
        prevInc = incNow;
        prevDec = decNow;
        mt_pad_arm = false;
        return -1;
    }

    int keyCode = -1;
    Uint32 now = SDL_GetTicks();
    if (aNow && !prevA) {
        keyCode = KEY_RETURN;
    } else if (bNow && !prevB) {
        keyCode = KEY_ESCAPE;
    } else if (yNow && !prevY) {
        keyCode = 5000; // set to max
    } else if (xNow && !prevX) {
        keyCode = MT_KEY_PLACE; // set the timer and put it down (SET TIMER only)
    } else if (incNow && (!prevInc || now - lastStep >= 120)) {
        keyCode = 6000; // +1
        lastStep = now;
    } else if (decNow && (!prevDec || now - lastStep >= 120)) {
        keyCode = 7000; // -1
        lastStep = now;
    }

    prevA = aNow;
    prevB = bNow;
    prevY = yNow;
    prevX = xNow;
    prevInc = incNow;
    prevDec = decNow;
    return keyCode;
}

// 0x469458
static int do_move_timer(int inventoryWindowType, Object* item, int max)
{
    setup_move_timer_win(inventoryWindowType, item);
    move_timer_gamepad_reset();
    mt_place_requested = false;

    int value;
    int min;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        value = 1;
        if (max > 999) {
            max = 999;
        }
        min = 1;
    } else {
        value = 60;
        min = 10;
    }

    draw_amount(value, inventoryWindowType);

    bool v5 = false;
    for (;;) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();

        // CE: fold in gamepad control (A/B/Y + D-pad) when no real key fired.
        if (keyCode == -1) {
            keyCode = move_timer_gamepad_keycode();
        }

        if (keyCode == KEY_ESCAPE) {
            exit_move_timer_win(inventoryWindowType);
            return -1;
        }

        bool place = keyCode == MT_KEY_PLACE && inventoryWindowType == INVENTORY_WINDOW_TYPE_SET_TIMER;
        if (keyCode == KEY_RETURN || place) {
            if (value >= min && value <= max) {
                if (inventoryWindowType != INVENTORY_WINDOW_TYPE_SET_TIMER || value % 10 == 0) {
                    gsound_play_sfx_file("ib1p1xx1");
                    mt_place_requested = place;
                    break;
                }
            }

            gsound_play_sfx_file("iisxxxx1");
        } else if (keyCode == 5000) {
            v5 = false;
            value = max;
            draw_amount(value, inventoryWindowType);
        } else if (keyCode == 6000) {
            v5 = false;
            if (value < max) {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
                    value += 1;
                } else {
                    value += 10;
                }

                draw_amount(value, inventoryWindowType);
                continue;
            }
        } else if (keyCode == 7000) {
            v5 = false;
            if (value > min) {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
                    value -= 1;
                } else {
                    value -= 10;
                }

                draw_amount(value, inventoryWindowType);
                continue;
            }
        }

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
            if (keyCode >= KEY_0 && keyCode <= KEY_9) {
                int number = keyCode - KEY_0;
                if (!v5) {
                    value = 0;
                }

                value = 10 * value % 1000 + number;
                v5 = true;

                draw_amount(value, inventoryWindowType);
                continue;
            } else if (keyCode == KEY_BACKSPACE) {
                if (!v5) {
                    value = 0;
                }

                value /= 10;
                v5 = true;

                draw_amount(value, inventoryWindowType);
                continue;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    exit_move_timer_win(inventoryWindowType);

    return value;
}

// Creates move items/set timer interface.
//
// 0x4695E4
static int setup_move_timer_win(int inventoryWindowType, Object* item)
{
    const int oldFont = text_curr();
    text_font(103);

    for (int index = 0; index < 8; index++) {
        mt_key[index] = NULL;
    }

    InventoryWindowDescription* windowDescription = &(iscr_data[inventoryWindowType]);

    // Maintain original position in original resolution, otherwise center it.
    int quantityWindowX = screenGetWidth() != 640
        ? (screenGetWidth() - windowDescription->width) / 2
        : windowDescription->x;
    int quantityWindowY = screenGetHeight() != 480
        ? (screenGetHeight() - windowDescription->height) / 2
        : windowDescription->y;
    mt_wid = win_add(quantityWindowX, quantityWindowY, windowDescription->width, windowDescription->height, 257, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    unsigned char* windowBuffer = win_get_buf(mt_wid);

    CacheEntry* backgroundHandle;
    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, windowDescription->field_0, 0, 0, 0);
    unsigned char* backgroundData = art_ptr_lock_data(backgroundFid, 0, 0, &backgroundHandle);
    if (backgroundData != NULL) {
        buf_to_buf(backgroundData, windowDescription->width, windowDescription->height, windowDescription->width, windowBuffer, windowDescription->width);
        art_ptr_unlock(backgroundHandle);
    }

    MessageListItem messageListItem;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        // MOVE ITEMS
        messageListItem.num = 21;
        if (message_search(&inventry_message_file, &messageListItem)) {
            int length = text_width(messageListItem.text);
            text_to_buf(windowBuffer + windowDescription->width * 9 + (windowDescription->width - length) / 2, messageListItem.text, 200, windowDescription->width, colorTable[21091]);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_SET_TIMER) {
        // SET TIMER
        messageListItem.num = 23;
        if (message_search(&inventry_message_file, &messageListItem)) {
            int length = text_width(messageListItem.text);
            text_to_buf(windowBuffer + windowDescription->width * 9 + (windowDescription->width - length) / 2, messageListItem.text, 200, windowDescription->width, colorTable[21091]);
        }

        // Timer overlay
        CacheEntry* overlayFrmHandle;
        int overlayFid = art_id(OBJ_TYPE_INTERFACE, 306, 0, 0, 0);
        unsigned char* overlayFrmData = art_ptr_lock_data(overlayFid, 0, 0, &overlayFrmHandle);
        if (overlayFrmData != NULL) {
            buf_to_buf(overlayFrmData, 105, 81, 105, windowBuffer + 34 * windowDescription->width + 113, windowDescription->width);
            art_ptr_unlock(overlayFrmHandle);
        }

        // PLACE -- the ALL plate, 6 px lower than ALL sits in the move-items mode: the clock reaches
        // further down than the counter, and at y 80 the plate covered its bottom edge and the minus
        // button (looked at side by side, 2026-10-16). At 86 its shadow lands on the panel border.
        CacheEntry* plateUpHandle;
        CacheEntry* plateDownHandle;
        unsigned char* plateUp = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 307, 0, 0, 0), 0, 0, &plateUpHandle);
        unsigned char* plateDown = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 308, 0, 0, 0), 0, 0, &plateDownHandle);
        if (plateUp != NULL && plateDown != NULL) {
            memcpy(mt_place_up, plateUp, sizeof(mt_place_up));
            memcpy(mt_place_down, plateDown, sizeof(mt_place_down));

            const char* label = "PLACE";
            int length = text_width(label);
            text_to_buf(mt_place_up + (MT_PLACE_W - length) / 2 + 4 * MT_PLACE_W, label, 200, MT_PLACE_W, colorTable[21091]);
            text_to_buf(mt_place_down + (MT_PLACE_W - length) / 2 + 4 * MT_PLACE_W, label, 200, MT_PLACE_W, colorTable[18977]);

            int btn = win_register_button(mt_wid, 120, 86, MT_PLACE_W, MT_PLACE_H, -1, -1, -1, MT_KEY_PLACE,
                mt_place_up, mt_place_down, NULL, BUTTON_FLAG_TRANSPARENT);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }
        }
        if (plateUp != NULL) {
            art_ptr_unlock(plateUpHandle);
        }
        if (plateDown != NULL) {
            art_ptr_unlock(plateDownHandle);
        }
    }

    int inventoryFid = item_inv_fid(item);
    scale_art(inventoryFid, windowBuffer + windowDescription->width * 46 + 16, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, windowDescription->width);

    int x;
    int y;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        x = 200;
        y = 46;
    } else {
        x = 194;
        y = 64;
    }

    int fid;
    unsigned char* buttonUpData;
    unsigned char* buttonDownData;
    int btn;

    // Plus button
    fid = art_id(OBJ_TYPE_INTERFACE, 193, 0, 0, 0);
    buttonUpData = art_ptr_lock_data(fid, 0, 0, &(mt_key[0]));

    fid = art_id(OBJ_TYPE_INTERFACE, 194, 0, 0, 0);
    buttonDownData = art_ptr_lock_data(fid, 0, 0, &(mt_key[1]));

    if (buttonUpData != NULL && buttonDownData != NULL) {
        btn = win_register_button(mt_wid, x, y, 16, 12, -1, -1, 6000, -1, buttonUpData, buttonDownData, NULL, BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }
    }

    // Minus button
    fid = art_id(OBJ_TYPE_INTERFACE, 191, 0, 0, 0);
    buttonUpData = art_ptr_lock_data(fid, 0, 0, &(mt_key[2]));

    fid = art_id(OBJ_TYPE_INTERFACE, 192, 0, 0, 0);
    buttonDownData = art_ptr_lock_data(fid, 0, 0, &(mt_key[3]));

    if (buttonUpData != NULL && buttonDownData != NULL) {
        btn = win_register_button(mt_wid, x, y + 12, 17, 12, -1, -1, 7000, -1, buttonUpData, buttonDownData, NULL, BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    buttonUpData = art_ptr_lock_data(fid, 0, 0, &(mt_key[4]));

    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    buttonDownData = art_ptr_lock_data(fid, 0, 0, &(mt_key[5]));

    if (buttonUpData != NULL && buttonDownData != NULL) {
        // Done
        btn = win_register_button(mt_wid, 98, 128, 15, 16, -1, -1, -1, KEY_RETURN, buttonUpData, buttonDownData, NULL, BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }

        // Cancel
        btn = win_register_button(mt_wid, 148, 128, 15, 16, -1, -1, -1, KEY_ESCAPE, buttonUpData, buttonDownData, NULL, BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        fid = art_id(OBJ_TYPE_INTERFACE, 307, 0, 0, 0);
        buttonUpData = art_ptr_lock_data(fid, 0, 0, &(mt_key[6]));

        fid = art_id(OBJ_TYPE_INTERFACE, 308, 0, 0, 0);
        buttonDownData = art_ptr_lock_data(fid, 0, 0, &(mt_key[7]));

        if (buttonUpData != NULL && buttonDownData != NULL) {
            // ALL
            messageListItem.num = 22;
            if (message_search(&inventry_message_file, &messageListItem)) {
                int length = text_width(messageListItem.text);

                // TODO: Where is y? Is it hardcoded in to 376?
                text_to_buf(buttonUpData + (94 - length) / 2 + 376, messageListItem.text, 200, 94, colorTable[21091]);
                text_to_buf(buttonDownData + (94 - length) / 2 + 376, messageListItem.text, 200, 94, colorTable[18977]);

                btn = win_register_button(mt_wid, 120, 80, 94, 33, -1, -1, -1, 5000, buttonUpData, buttonDownData, NULL, BUTTON_FLAG_TRANSPARENT);
                if (btn != -1) {
                    win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
                }
            }
        }
    }

    win_draw(mt_wid);
    inven_set_mouse(INVENTORY_WINDOW_CURSOR_ARROW);
    text_font(oldFont);

    return 0;
}

// 0x469B5C
static int exit_move_timer_win(int inventoryWindowType)
{
    int count = inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS ? 8 : 6;

    for (int index = 0; index < count; index++) {
        art_ptr_unlock(mt_key[index]);
    }

    win_delete(mt_wid);

    return 0;
}

// Did the last timer window end on PLACE? Read once -- the answer is cleared.
bool inven_set_timer_place_requested()
{
    bool requested = mt_place_requested;
    mt_place_requested = false;
    return requested;
}

// PLACE from the inventory's own Use (both paths): the freshly armed explosive goes on the ground the
// way the Drop action puts it there -- back from a hand box into the pack first -- and the screen's
// close runs vanilla's "they saw that" (dropped_explosive).
static void inven_place_if_requested(Object* item, Object* owner, Object** handSlot)
{
    if (!inven_set_timer_place_requested()) {
        return;
    }
    if (item->pid != PROTO_ID_DYNAMITE_II && item->pid != PROTO_ID_PLASTIC_EXPLOSIVES_II) {
        return;
    }
    if (handSlot != NULL) {
        item_add_force(owner, item, 1);
        *handSlot = NULL;
    }
    obj_drop(owner, item);
    dropped_explosive = true;
}

// 0x469BA0
int inven_set_timer(Object* a1)
{
    bool v1 = inven_is_initialized;

    if (!v1) {
        if (inven_init() == -1) {
            return -1;
        }
    }

    int seconds = do_move_timer(INVENTORY_WINDOW_TYPE_SET_TIMER, a1, 180);

    if (!v1) {
        // NOTE: Uninline.
        inven_exit();
    }

    return seconds;
}

} // namespace fallout
