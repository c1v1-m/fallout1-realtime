#include "game/intface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/combat_rt.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/cycle.h"
#include "game/display.h"
#include "game/endgame.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/game_trace.h"
#include "game/gsound.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/object.h"
#include "game/protinst.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

// The width of connectors in the indicator box.
//
// There are male connectors on the left, and female connectors on the right.
// When displaying series of boxes they appear to be plugged into a chain.
#define INDICATOR_BOX_CONNECTOR_WIDTH 3

// The values of it's members are offsets to beginning of numbers in
// numbers.frm.
typedef enum InterfaceNumbersColor {
    INTERFACE_NUMBERS_COLOR_WHITE = 0,
    INTERFACE_NUMBERS_COLOR_YELLOW = 120,
    INTERFACE_NUMBERS_COLOR_RED = 240,
} InterfaceNumbersColor;

#define INDICATOR_BOX_WIDTH 130
#define INDICATOR_BOX_HEIGHT 21

// The maximum number of indicator boxes the indicator bar can display.
#define INDICATOR_SLOTS_COUNT 4

// Available indicators.
//
// Indicator boxes in the bar are displayed according to the order of this enum.
typedef enum Indicator {
    INDICATOR_ADDICT,
    INDICATOR_SNEAK,
    INDICATOR_LEVEL,
    INDICATOR_COUNT,
} Indicator;

// Provides metadata about indicator boxes.
typedef struct IndicatorDescription {
    // An identifier of title in `intrface.msg`.
    int title;

    // A flag denoting this box represents something harmful to the player. It
    // affects color of the title.
    bool isBad;

    // Prerendered indicator data.
    //
    // This value is provided at runtime during indicator box initialization.
    // It includes indicator box background with it's title positioned in the
    // center and is green colored if indicator is good, or red otherwise, as
    // denoted by [isBad] property.
    unsigned char* data;
} IndicatorDescription;

typedef struct InterfaceItemState {
    Object* item;
    unsigned char isDisabled;
    unsigned char isWeapon;
    int primaryHitMode;
    int secondaryHitMode;
    int action;
    int itemFid;
} InterfaceItemState;

static int intface_init_items();
static int intface_redraw_items();
static int intface_redraw_items_callback(Object* a1, Object* a2);
static int intface_change_fid_callback(Object* a1, Object* a2);
static void intface_change_fid_animate(int previousWeaponAnimationCode, int weaponAnimationCode);
static int intface_create_end_turn_button();
static int intface_destroy_end_turn_button();
static int intface_create_end_combat_button();
static int intface_destroy_end_combat_button();
static void intface_draw_ammo_lights(int x, int ratio);
static int intface_item_reload();
static void intface_rotate_numbers(int x, int y, int previousValue, int value, int offset, int delay);
static int intface_fatal_error(int rc);
static int construct_box_bar_win();
static void deconstruct_box_bar_win();
static void reset_box_bar_win();
static int bbox_comp(const void* a, const void* b);
static void draw_bboxes(int count);
static bool add_bar_box(int indicator);

// 0x505508
static bool insideInit = false;

// 0x50550C
static bool intface_fid_is_changing = false;

// 0x505510
static bool intfaceEnabled = false;

// 0x505514
static bool intfaceHidden = false;

// 0x505518
static int inventoryButton = -1;

// 0x50551C
static CacheEntry* inventoryButtonUpKey = NULL;

// 0x505520
static CacheEntry* inventoryButtonDownKey = NULL;

// 0x505524
static int optionsButton = -1;

// 0x505528
static CacheEntry* optionsButtonUpKey = NULL;

// 0x50552C
static CacheEntry* optionsButtonDownKey = NULL;

// 0x505530
static int skilldexButton = -1;

// 0x505534
static CacheEntry* skilldexButtonUpKey = NULL;

// 0x505538
static CacheEntry* skilldexButtonDownKey = NULL;

// 0x50553C
static CacheEntry* skilldexButtonMaskKey = NULL;

// 0x505540
static int automapButton = -1;

// 0x505544
static CacheEntry* automapButtonUpKey = NULL;

// 0x505548
static CacheEntry* automapButtonDownKey = NULL;

// 0x50554C
static CacheEntry* automapButtonMaskKey = NULL;

// 0x505550
static int pipboyButton = -1;

// 0x505554
static CacheEntry* pipboyButtonUpKey = NULL;

// 0x505558
static CacheEntry* pipboyButtonDownKey = NULL;

// 0x50555C
static int characterButton = -1;

// 0x505560
static CacheEntry* characterButtonUpKey = NULL;

// 0x505564
static CacheEntry* characterButtonDownKey = NULL;

// 0x505568
static int itemButton = -1;

// 0x50556C
static CacheEntry* itemButtonUpKey = NULL;

// 0x505570
static CacheEntry* itemButtonDownKey = NULL;

// 0x505574
static CacheEntry* itemButtonDisabledKey = NULL;

// 0x505578
static int itemCurrentItem = HAND_LEFT;

// 0x50557C
static Rect itemButtonRect = { 267, 26, 455, 93 };

// 0x50558C
static int toggleButton = -1;

// 0x505590
static CacheEntry* toggleButtonUpKey = NULL;

// 0x505594
static CacheEntry* toggleButtonDownKey = NULL;

// 0x505598
static CacheEntry* toggleButtonMaskKey = NULL;

// 0x50559C
static bool endWindowOpen = false;

// Combat mode curtains rect.
//
// 0x5055A0
static Rect endWindowRect = { 580, 38, 637, 96 };

// THE SIDE PANEL (2026-10-08). On a screen wider than 640 the bar sits centred with black on both
// sides of it -- no window at all lives there. The realtime heal slot and the aimed-part readout
// move into the strip on its RIGHT, which hands the bar its two boxes back: the SKILLDEX plate, and
// the end-turn box, which shows the combat curtains again (intface_rt_combat_box_tick). -1 = the
// screen is only 640 wide (or too narrow a strip): both widgets stay where they were on the bar.
#define SIDE_PANEL_MIN_WIDTH 60
static int sidePanelWindow = -1;
static unsigned char* sidePanelBuffer = NULL;
static int sidePanelWidth = 0;

// THE SIDE ART (2026-10-08): *"prostě nahraď černý pruhy tímhle, nic nepřidávej a neměň"*. Mash's
// High Resolution Patch -- shipped with the Steam release -- carries a metal strip for each side of
// the 640 bar, art\intrface\HR_IFACELFT1.FRM / HR_IFACERHT1.FRM, 640x100 apiece, the Vault emblem at
// the end that meets the bar. Laid the way fallout2-ce's sidePanelsInit lays them: each piece butts
// against the bar and the screen edge crops its far end. The right one goes UNDER the side panel's
// heal box and readout, which only ever paint their own rects. They are not in the game's own data:
// without the files the strips stay black, exactly as before -- so there is no switch for it.
static int sideArtLeftWindow = -1;
static int sideArtRightWindow = -1;

// Copies the end of a side piece nearest the bar into a strip `width` wide. `barOnRight` = this is
// the LEFT strip, so the image's right edge meets the bar and its left end is what gets cropped.
static void intface_side_art_paint(Art* art, bool barOnRight, unsigned char* dest, int destPitch, int width)
{
    int artW;
    int artH;
    if (art_frame_width_length(art, 0, 0, &artW, &artH) != 0) {
        return;
    }
    unsigned char* data = art_frame_data(art, 0, 0);
    if (data == NULL) {
        return;
    }

    int copyW = std::min(width, artW);
    int copyH = std::min(artH, INTERFACE_BAR_HEIGHT);
    int srcX = barOnRight ? artW - copyW : 0;
    int destX = barOnRight ? width - copyW : 0;
    buf_to_buf(data + srcX, copyW, copyH, artW, dest + destX, destPitch);
}

// Fills the strips either side of the bar from the HRP pieces, when they are there. The right one
// paints into the side panel if it exists, else into a window of its own; both follow the bar's
// show/hide (intface_show / intface_hide) and die with it (intface_exit).
static void intface_side_art_init(int barX, int barY)
{
    if (barX > 0) {
        Art* art = load_frame("art\\intrface\\HR_IFACELFT1.FRM");
        if (art != NULL) {
            sideArtLeftWindow = win_add(0, barY, barX, INTERFACE_BAR_HEIGHT, colorTable[0], WINDOW_HIDDEN);
            unsigned char* buf = sideArtLeftWindow != -1 ? win_get_buf(sideArtLeftWindow) : NULL;
            if (buf != NULL) {
                intface_side_art_paint(art, true, buf, barX, barX);
            }
            mem_free(art);
        }
    }

    int rightX = barX + INTERFACE_BAR_WIDTH;
    int rightW = screenGetWidth() - rightX;
    if (rightW > 0) {
        Art* art = load_frame("art\\intrface\\HR_IFACERHT1.FRM");
        if (art != NULL) {
            if (sidePanelWindow != -1) {
                intface_side_art_paint(art, false, sidePanelBuffer, sidePanelWidth, sidePanelWidth);
            } else {
                sideArtRightWindow = win_add(rightX, barY, rightW, INTERFACE_BAR_HEIGHT, colorTable[0], WINDOW_HIDDEN);
                unsigned char* buf = sideArtRightWindow != -1 ? win_get_buf(sideArtRightWindow) : NULL;
                if (buf != NULL) {
                    intface_side_art_paint(art, false, buf, rightW, rightW);
                }
            }
            mem_free(art);
        }
    }
}

// 0x5055B0
static int endTurnButton = -1;

// 0x5055B4
static CacheEntry* endTurnButtonUpKey = NULL;

// 0x5055B8
static CacheEntry* endTurnButtonDownKey = NULL;

// 0x5055BC
static int endCombatButton = -1;

// 0x5055C0
static CacheEntry* endCombatButtonUpKey = NULL;

// 0x5055C4
static CacheEntry* endCombatButtonDownKey = NULL;

// 0x5055C8
static unsigned char* moveLightGreen = NULL;

// 0x5055CC
static unsigned char* moveLightYellow = NULL;

// The yellow AP bulb actually drawn for Bonus Move points: the GREEN bulb re-tinted, built once in
// intface_init by intface_build_yellow_light. Fallout 1's own HLYEL.FRM is a dull olive blob with
// none of the green bulb's shading -- its left half is near-brown (72,56,4 / 112,120,32) -- and it
// had to sit right beside the green ones now that Bonus Move works. His ask was the cleaner yellow
// Fallout 2 shows ("inspiruj se žlutou z fallout 2"); F2's data is not on this machine, so this is
// that look rebuilt from the green bulb itself: same shape, same highlight, yellow.
static unsigned char moveLightYellowLit[5 * 5];

// The LIGHT-GREEN AP bulb: one bulb worth TWO action points, for the AP a character has beyond the
// ten the strip can show (AGI 10 + Action Boy and the like). His pick, 2026-10-04 ("udělej dvojku,
// světle zelená za dva AP"), over leaving AP past ten invisible as both games did -- vanilla Fallout 2
// clamps the count to ten -- and over sfall's widened 16-bulb bar, which needs new interface art.
// Built from the green bulb like the yellow one; see intface_update_move_points for the layout.
static unsigned char moveLightGreenLight[5 * 5];

// Re-tint the green AP bulb into the two bulbs we add. The green bulb carries its shading in the G
// channel and its whitish highlight in R and B.
//   * YELLOW (moveLightYellowLit): G becomes both red AND green -- lifted by a third, because yellow
//     at the same level reads muddier than green -- and the highlight becomes its blue.
//   * LIGHT GREEN (moveLightGreenLight): every channel lifted toward white (+12 on red and blue, +8
//     on green), so it stays green but reads clearly paler and brighter than its neighbours.
// colorTable is Interplay's own nearest-colour table (the tail of COLOR.PAL), so every shade lands on
// a real palette entry. On the stock palette: yellow 144,120,36 / 204,184,28 / 228,216,12 /
// 252,252,124; light green 120,152,92 / 100,228,100 / 216,252,156 -- none of them a cycling colour.
static void intface_build_ap_lights()
{
    for (int index = 0; index < 5 * 5; index++) {
        unsigned char pixel = moveLightGreen[index];
        if (pixel == 0) {
            // Transparent stays transparent: both bulbs keep the green one's shape.
            moveLightYellowLit[index] = 0;
            moveLightGreenLight[index] = 0;
            continue;
        }

        int rgb = Color2RGB(pixel);
        int r = (rgb >> 10) & 0x1F;
        int g = (rgb >> 5) & 0x1F;
        int b = rgb & 0x1F;

        int level = g + g / 3;
        if (level > 0x1F) {
            level = 0x1F;
        }
        int highlight = (r + b) / 2;
        moveLightYellowLit[index] = colorTable[(level << 10) | (level << 5) | highlight];

        int lr = r + 12 > 0x1F ? 0x1F : r + 12;
        int lg = g + 8 > 0x1F ? 0x1F : g + 8;
        int lb = b + 12 > 0x1F ? 0x1F : b + 12;
        moveLightGreenLight[index] = colorTable[(lr << 10) | (lg << 5) | lb];
    }
}

// 0x5055D0
static unsigned char* moveLightRed = NULL;

// 0x5055D4
static Rect movePointRect = { 316, 14, 406, 19 };

// 0x5055E4
static unsigned char* numbersBuffer = NULL;

// 0x5055E8
static IndicatorDescription bbox[INDICATOR_COUNT] = {
    { 102, true, NULL }, // ADDICT
    { 100, false, NULL }, // SNEAK
    { 101, false, NULL }, // LEVEL
};

// 0x50560C
int interfaceWindow = -1;

// 0x505610
int bar_window = -1;

// Each slot contains one of indicators or -1 if slot is empty.
//
// 0x595670
static int bboxslot[INDICATOR_SLOTS_COUNT];

// 0x595680
static InterfaceItemState itemButtonItems[HAND_COUNT];

// 0x5956CC
static CacheEntry* moveLightYellowKey;

// 0x5956D0
static CacheEntry* moveLightRedKey;

// 0x5956B0
static CacheEntry* numbersKey;

// 0x5956B8
static bool box_status_flag;

// 0x5956BC
static unsigned char* toggleButtonUp;

// 0x5956C0
static CacheEntry* moveLightGreenKey;

// 0x5956C4
static unsigned char* endCombatButtonUp;

// 0x5956C8
static unsigned char* endCombatButtonDown;

// 0x5956D4
static unsigned char* toggleButtonDown;

// 0x5956D8
static unsigned char* endTurnButtonDown;

// 0x595700
static unsigned char itemButtonDown[188 * 67];

// 0x5956DC
static unsigned char* endTurnButtonUp;

// 0x5956E0
static unsigned char* toggleButtonMask;

// 0x5956E4
static unsigned char* characterButtonUp;

// 0x5956E8
static unsigned char* itemButtonUpBlank;

// 0x5956EC
static unsigned char* itemButtonDisabled;

// 0x5956F0
static unsigned char* automapButtonDown;

// 0x5956F4
static unsigned char* pipboyButtonUp;

// 0x5956F8
static unsigned char* characterButtonDown;

// 0x5956FC
static unsigned char* itemButtonDownBlank;

// 0x598834
static unsigned char* pipboyButtonDown;

// 0x598838
static unsigned char* automapButtonMask;

// 0x59883C
static unsigned char itemButtonUp[188 * 67];

// 0x59B990
static unsigned char* automapButtonUp;

// 0x59B970
static unsigned char* skilldexButtonMask;

// 0x59B974
static unsigned char* skilldexButtonDown;

// 0x59B978
static unsigned char* interfaceBuffer;

// 0x59B97C
static unsigned char* inventoryButtonUp;

// 0x59B980
static unsigned char* optionsButtonUp;

// 0x59B984
static unsigned char* optionsButtonDown;

// 0x59B988
static unsigned char* skilldexButtonUp;

// 0x59B98C
static unsigned char* inventoryButtonDown;

// A slice of main interface background containing 10 shadowed action point
// dots. In combat mode individual colored dots are rendered on top of this
// background.
//
// This buffer is initialized once and does not change throughout the game.
//
// 0x59B994
static unsigned char movePointBackground[90 * 5];

// 0x4531F0
int intface_init()
{
    int fid;
    CacheEntry* backgroundFrmHandle;
    unsigned char* backgroundFrmData;

    if (interfaceWindow != -1) {
        return -1;
    }

    insideInit = 1;

    int interfaceBarWindowX = (screenGetWidth() - INTERFACE_BAR_WIDTH) / 2;
    int interfaceBarWindowY = screenGetHeight() - INTERFACE_BAR_HEIGHT;

    interfaceWindow = win_add(interfaceBarWindowX, interfaceBarWindowY, INTERFACE_BAR_WIDTH, INTERFACE_BAR_HEIGHT, colorTable[0], WINDOW_HIDDEN);
    if (interfaceWindow == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    interfaceBuffer = win_get_buf(interfaceWindow);
    if (interfaceBuffer == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    // MODS "Combat box sound" -- default ON, the vanilla clunk and chime.
    int boxSound = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_COMBAT_BOX_SOUND_KEY, &boxSound);
    intface_set_combat_box_sound(boxSound != 0);

    // The strip right of the bar, if there is room for the heal slot in it. Hidden with the bar.
    {
        int sideX = interfaceBarWindowX + INTERFACE_BAR_WIDTH;
        int sideW = screenGetWidth() - sideX;
        if (sideW >= SIDE_PANEL_MIN_WIDTH) {
            sidePanelWindow = win_add(sideX, interfaceBarWindowY, sideW, INTERFACE_BAR_HEIGHT, colorTable[0], WINDOW_HIDDEN);
            if (sidePanelWindow != -1) {
                sidePanelBuffer = win_get_buf(sidePanelWindow);
                sidePanelWidth = sideW;
                if (sidePanelBuffer == NULL) {
                    win_delete(sidePanelWindow);
                    sidePanelWindow = -1;
                }
            }
        }
    }

    intface_side_art_init(interfaceBarWindowX, interfaceBarWindowY);

    fid = art_id(OBJ_TYPE_INTERFACE, 16, 0, 0, 0);
    backgroundFrmData = art_ptr_lock_data(fid, 0, 0, &backgroundFrmHandle);
    if (backgroundFrmData == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    buf_to_buf(backgroundFrmData, INTERFACE_BAR_WIDTH, INTERFACE_BAR_HEIGHT, INTERFACE_BAR_WIDTH, interfaceBuffer, 640);
    art_ptr_unlock(backgroundFrmHandle);

    fid = art_id(OBJ_TYPE_INTERFACE, 47, 0, 0, 0);
    inventoryButtonUp = art_ptr_lock_data(fid, 0, 0, &inventoryButtonUpKey);
    if (inventoryButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 46, 0, 0, 0);
    inventoryButtonDown = art_ptr_lock_data(fid, 0, 0, &inventoryButtonDownKey);
    if (inventoryButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    inventoryButton = win_register_button(interfaceWindow, 211, 41, 32, 21, -1, -1, -1, KEY_LOWERCASE_I, inventoryButtonUp, inventoryButtonDown, NULL, 0);
    if (inventoryButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_sound_func(inventoryButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 18, 0, 0, 0);
    optionsButtonUp = art_ptr_lock_data(fid, 0, 0, &optionsButtonUpKey);
    if (optionsButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 17, 0, 0, 0);
    optionsButtonDown = art_ptr_lock_data(fid, 0, 0, &optionsButtonDownKey);
    if (optionsButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    optionsButton = win_register_button(interfaceWindow, 210, 62, 34, 34, -1, -1, -1, KEY_LOWERCASE_O, optionsButtonUp, optionsButtonDown, NULL, 0);
    if (optionsButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_sound_func(optionsButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 6, 0, 0, 0);
    skilldexButtonUp = art_ptr_lock_data(fid, 0, 0, &skilldexButtonUpKey);
    if (skilldexButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 7, 0, 0, 0);
    skilldexButtonDown = art_ptr_lock_data(fid, 0, 0, &skilldexButtonDownKey);
    if (skilldexButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 6, 0, 0, 0);
    skilldexButtonMask = art_ptr_lock_data(fid, 0, 0, &skilldexButtonMaskKey);
    if (skilldexButtonMask == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    skilldexButton = win_register_button(interfaceWindow, 523, 7, 22, 21, -1, -1, -1, KEY_LOWERCASE_S, skilldexButtonUp, skilldexButtonDown, NULL, BUTTON_FLAG_TRANSPARENT);
    if (skilldexButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_mask(skilldexButton, skilldexButtonMask);
    win_register_button_sound_func(skilldexButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 13, 0, 0, 0);
    automapButtonUp = art_ptr_lock_data(fid, 0, 0, &automapButtonUpKey);
    if (automapButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 10, 0, 0, 0);
    automapButtonDown = art_ptr_lock_data(fid, 0, 0, &automapButtonDownKey);
    if (automapButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 13, 0, 0, 0);
    automapButtonMask = art_ptr_lock_data(fid, 0, 0, &automapButtonMaskKey);
    if (automapButtonMask == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    automapButton = win_register_button(interfaceWindow, 526, 40, 41, 19, -1, -1, -1, KEY_TAB, automapButtonUp, automapButtonDown, NULL, BUTTON_FLAG_TRANSPARENT);
    if (automapButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_mask(automapButton, automapButtonMask);
    win_register_button_sound_func(automapButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 59, 0, 0, 0);
    pipboyButtonUp = art_ptr_lock_data(fid, 0, 0, &pipboyButtonUpKey);
    if (pipboyButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 58, 0, 0, 0);
    pipboyButtonDown = art_ptr_lock_data(fid, 0, 0, &pipboyButtonDownKey);
    if (pipboyButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    pipboyButton = win_register_button(interfaceWindow, 526, 78, 41, 19, -1, -1, -1, KEY_LOWERCASE_P, pipboyButtonUp, pipboyButtonDown, NULL, 0);
    if (pipboyButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_mask(pipboyButton, automapButtonMask);
    win_register_button_sound_func(pipboyButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 57, 0, 0, 0);
    characterButtonUp = art_ptr_lock_data(fid, 0, 0, &characterButtonUpKey);
    if (characterButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 56, 0, 0, 0);
    characterButtonDown = art_ptr_lock_data(fid, 0, 0, &characterButtonDownKey);
    if (characterButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    characterButton = win_register_button(interfaceWindow, 526, 59, 41, 19, -1, -1, -1, KEY_LOWERCASE_C, characterButtonUp, characterButtonDown, NULL, 0);
    if (characterButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_mask(characterButton, automapButtonMask);
    win_register_button_sound_func(characterButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 32, 0, 0, 0);
    itemButtonUpBlank = art_ptr_lock_data(fid, 0, 0, &itemButtonUpKey);
    if (itemButtonUpBlank == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 31, 0, 0, 0);
    itemButtonDownBlank = art_ptr_lock_data(fid, 0, 0, &itemButtonDownKey);
    if (itemButtonDownBlank == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 73, 0, 0, 0);
    itemButtonDisabled = art_ptr_lock_data(fid, 0, 0, &itemButtonDisabledKey);
    if (itemButtonDisabled == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    memcpy(itemButtonUp, itemButtonUpBlank, sizeof(itemButtonUp));
    memcpy(itemButtonDown, itemButtonDownBlank, sizeof(itemButtonDown));

    itemButton = win_register_button(interfaceWindow, 267, 26, 188, 67, -1, -1, -1, -20, itemButtonUp, itemButtonDown, NULL, BUTTON_FLAG_TRANSPARENT);
    if (itemButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_right_button(itemButton, -1, KEY_LOWERCASE_N, NULL, NULL);
    win_register_button_sound_func(itemButton, gsound_lrg_butt_press, gsound_lrg_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 6, 0, 0, 0);
    toggleButtonUp = art_ptr_lock_data(fid, 0, 0, &toggleButtonUpKey);
    if (toggleButtonUp == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 7, 0, 0, 0);
    toggleButtonDown = art_ptr_lock_data(fid, 0, 0, &toggleButtonDownKey);
    if (toggleButtonDown == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 6, 0, 0, 0);
    toggleButtonMask = art_ptr_lock_data(fid, 0, 0, &toggleButtonMaskKey);
    if (toggleButtonMask == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    // Swap hands button
    toggleButton = win_register_button(interfaceWindow, 218, 6, 22, 21, -1, -1, -1, KEY_LOWERCASE_B, toggleButtonUp, toggleButtonDown, NULL, BUTTON_FLAG_TRANSPARENT);
    if (toggleButton == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    win_register_button_mask(toggleButton, toggleButtonMask);
    win_register_button_sound_func(toggleButton, gsound_med_butt_press, gsound_med_butt_release);

    fid = art_id(OBJ_TYPE_INTERFACE, 82, 0, 0, 0);
    numbersBuffer = art_ptr_lock_data(fid, 0, 0, &numbersKey);
    if (numbersBuffer == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 83, 0, 0, 0);
    moveLightGreen = art_ptr_lock_data(fid, 0, 0, &moveLightGreenKey);
    if (moveLightGreen == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 84, 0, 0, 0);
    moveLightYellow = art_ptr_lock_data(fid, 0, 0, &moveLightYellowKey);
    if (moveLightYellow == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 85, 0, 0, 0);
    moveLightRed = art_ptr_lock_data(fid, 0, 0, &moveLightRedKey);
    if (moveLightRed == NULL) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    // The Bonus Move bulb and the two-AP bulb, both built from the green one.
    intface_build_ap_lights();

    buf_to_buf(interfaceBuffer + 640 * 14 + 316, 90, 5, 640, movePointBackground, 90);

    if (construct_box_bar_win() == -1) {
        // NOTE: Uninline.
        return intface_fatal_error(-1);
    }

    itemCurrentItem = HAND_LEFT;

    // NOTE: Uninline.
    intface_init_items();

    display_init();

    intfaceEnabled = true;
    insideInit = false;
    intfaceHidden = 1;

    return 0;
}

// 0x453D40
void intface_reset()
{
    intface_enable();

    // NOTE: Uninline.
    intface_hide();

    display_reset();

    // NOTE: Uninline.
    reset_box_bar_win();

    itemCurrentItem = 0;

    // A new game or a load: no fight is live, so the combat box shuts without its animation.
    intface_rt_combat_box_reset();
}

// 0x453DB0
void intface_exit()
{
    if (interfaceWindow != -1) {
        display_exit();

        if (moveLightRed != NULL) {
            art_ptr_unlock(moveLightRedKey);
            moveLightRed = NULL;
        }

        if (moveLightYellow != NULL) {
            art_ptr_unlock(moveLightYellowKey);
            moveLightYellow = NULL;
        }

        if (moveLightGreen != NULL) {
            art_ptr_unlock(moveLightGreenKey);
            moveLightGreen = NULL;
        }

        if (numbersBuffer != NULL) {
            art_ptr_unlock(numbersKey);
            numbersBuffer = NULL;
        }

        if (toggleButton != -1) {
            win_delete_button(toggleButton);
            toggleButton = -1;
        }

        if (toggleButtonMask != NULL) {
            art_ptr_unlock(toggleButtonMaskKey);
            toggleButtonMaskKey = NULL;
            toggleButtonMask = NULL;
        }

        if (toggleButtonDown != NULL) {
            art_ptr_unlock(toggleButtonDownKey);
            toggleButtonDownKey = NULL;
            toggleButtonDown = NULL;
        }

        if (toggleButtonUp != NULL) {
            art_ptr_unlock(toggleButtonUpKey);
            toggleButtonUpKey = NULL;
            toggleButtonUp = NULL;
        }

        if (itemButton != -1) {
            win_delete_button(itemButton);
            itemButton = -1;
        }

        if (itemButtonDisabled != NULL) {
            art_ptr_unlock(itemButtonDisabledKey);
            itemButtonDisabledKey = NULL;
            itemButtonDisabled = NULL;
        }

        if (itemButtonDownBlank != NULL) {
            art_ptr_unlock(itemButtonDownKey);
            itemButtonDownKey = NULL;
            itemButtonDownBlank = NULL;
        }

        if (itemButtonUpBlank != NULL) {
            art_ptr_unlock(itemButtonUpKey);
            itemButtonUpKey = NULL;
            itemButtonUpBlank = NULL;
        }

        if (characterButton != -1) {
            win_delete_button(characterButton);
            characterButton = -1;
        }

        if (characterButtonDown != NULL) {
            art_ptr_unlock(characterButtonDownKey);
            characterButtonDownKey = NULL;
            characterButtonDown = NULL;
        }

        if (characterButtonUp != NULL) {
            art_ptr_unlock(characterButtonUpKey);
            characterButtonUpKey = NULL;
            characterButtonUp = NULL;
        }

        if (pipboyButton != -1) {
            win_delete_button(pipboyButton);
            pipboyButton = -1;
        }

        if (pipboyButtonDown != NULL) {
            art_ptr_unlock(pipboyButtonDownKey);
            pipboyButtonDownKey = NULL;
            pipboyButtonDown = NULL;
        }

        if (pipboyButtonUp != NULL) {
            art_ptr_unlock(pipboyButtonUpKey);
            pipboyButtonUpKey = NULL;
            pipboyButtonUp = NULL;
        }

        if (automapButton != -1) {
            win_delete_button(automapButton);
            automapButton = -1;
        }

        if (automapButtonMask != NULL) {
            art_ptr_unlock(automapButtonMaskKey);
            automapButtonMaskKey = NULL;
            automapButtonMask = NULL;
        }

        if (automapButtonDown != NULL) {
            art_ptr_unlock(automapButtonDownKey);
            automapButtonDownKey = NULL;
            automapButtonDown = NULL;
        }

        if (automapButtonUp != NULL) {
            art_ptr_unlock(automapButtonUpKey);
            automapButtonUpKey = NULL;
            automapButtonUp = NULL;
        }

        if (skilldexButton != -1) {
            win_delete_button(skilldexButton);
            skilldexButton = -1;
        }

        if (skilldexButtonMask != NULL) {
            art_ptr_unlock(skilldexButtonMaskKey);
            skilldexButtonMaskKey = NULL;
            skilldexButtonMask = NULL;
        }

        if (skilldexButtonDown != NULL) {
            art_ptr_unlock(skilldexButtonDownKey);
            skilldexButtonDownKey = NULL;
            skilldexButtonDown = NULL;
        }

        if (skilldexButtonUp != NULL) {
            art_ptr_unlock(skilldexButtonUpKey);
            skilldexButtonUpKey = NULL;
            skilldexButtonUp = NULL;
        }

        if (optionsButton != -1) {
            win_delete_button(optionsButton);
            optionsButton = -1;
        }

        if (optionsButtonDown != NULL) {
            art_ptr_unlock(optionsButtonDownKey);
            optionsButtonDownKey = NULL;
            optionsButtonDown = NULL;
        }

        if (optionsButtonUp != NULL) {
            art_ptr_unlock(optionsButtonUpKey);
            optionsButtonUpKey = NULL;
            optionsButtonUp = NULL;
        }

        if (inventoryButton != -1) {
            win_delete_button(inventoryButton);
            inventoryButton = -1;
        }

        if (inventoryButtonDown != NULL) {
            art_ptr_unlock(inventoryButtonDownKey);
            inventoryButtonDownKey = NULL;
            inventoryButtonDown = NULL;
        }

        if (inventoryButtonUp != NULL) {
            art_ptr_unlock(inventoryButtonUpKey);
            inventoryButtonUpKey = NULL;
            inventoryButtonUp = NULL;
        }

        if (interfaceWindow != -1) {
            win_delete(interfaceWindow);
            interfaceWindow = -1;
        }

        if (sidePanelWindow != -1) {
            win_delete(sidePanelWindow);
            sidePanelWindow = -1;
            sidePanelBuffer = NULL;
            sidePanelWidth = 0;
        }

        if (sideArtLeftWindow != -1) {
            win_delete(sideArtLeftWindow);
            sideArtLeftWindow = -1;
        }

        if (sideArtRightWindow != -1) {
            win_delete(sideArtRightWindow);
            sideArtRightWindow = -1;
        }
    }

    deconstruct_box_bar_win();
}

// 0x4541D0
int intface_load(DB_FILE* stream)
{
    if (interfaceWindow == -1) {
        if (intface_init() == -1) {
            return -1;
        }
    }

    bool enabled;
    if (db_freadBool(stream, &enabled) == -1) return -1;

    bool hidden;
    if (db_freadBool(stream, &hidden) == -1) return -1;

    int hand;
    if (db_freadInt32(stream, &hand) == -1) return -1;

    bool endButtonsVisible;
    if (db_freadBool(stream, &endButtonsVisible) == -1) return -1;

    if (!intfaceEnabled) {
        intface_enable();
    }

    if (hidden) {
        // NOTE: Uninline.
        intface_hide();
    } else {
        intface_show();
    }

    intface_update_hit_points(false);
    intface_update_ac(false);

    itemCurrentItem = hand;

    intface_update_items(false);

    if (endButtonsVisible != endWindowOpen) {
        if (endButtonsVisible) {
            intface_end_window_open(false);
        } else {
            intface_end_window_close(false);
        }
    }

    if (!enabled) {
        intface_disable();
    }

    refresh_box_bar_win();

    win_draw(interfaceWindow);

    return 0;
}

// 0x454320
int intface_save(DB_FILE* stream)
{
    if (interfaceWindow == -1) {
        return -1;
    }

    if (db_fwriteBool(stream, intfaceEnabled) == -1) return -1;
    if (db_fwriteBool(stream, intfaceHidden) == -1) return -1;
    if (db_fwriteInt32(stream, itemCurrentItem) == -1) return -1;
    if (db_fwriteBool(stream, endWindowOpen) == -1) return -1;

    return 0;
}

// 0x454378
void intface_hide()
{
    if (interfaceWindow != -1) {
        if (!intfaceHidden) {
            win_hide(interfaceWindow);
            if (sidePanelWindow != -1) {
                win_hide(sidePanelWindow);
            }
            if (sideArtLeftWindow != -1) {
                win_hide(sideArtLeftWindow);
            }
            if (sideArtRightWindow != -1) {
                win_hide(sideArtRightWindow);
            }
            intfaceHidden = 1;
        }
    }
    refresh_box_bar_win();
}

// 0x4543A8
void intface_show()
{
    if (interfaceWindow != -1) {
        if (intfaceHidden) {
            intface_update_items(false);
            intface_update_hit_points(false);
            intface_update_ac(false);
            win_show(interfaceWindow);
            if (sidePanelWindow != -1) {
                win_show(sidePanelWindow);
            }
            if (sideArtLeftWindow != -1) {
                win_show(sideArtLeftWindow);
            }
            if (sideArtRightWindow != -1) {
                win_show(sideArtRightWindow);
            }
            intfaceHidden = false;
        }
    }
    refresh_box_bar_win();
}

// 0x4543EC
int intface_is_hidden()
{
    return intfaceHidden;
}

// 0x4543F4
void intface_enable()
{
    if (!intfaceEnabled) {
        win_enable_button(inventoryButton);
        win_enable_button(optionsButton);
        win_enable_button(skilldexButton);
        win_enable_button(automapButton);
        win_enable_button(pipboyButton);
        win_enable_button(characterButton);

        if (itemButtonItems[itemCurrentItem].isDisabled == 0) {
            win_enable_button(itemButton);
        }

        win_enable_button(endTurnButton);
        win_enable_button(endCombatButton);
        display_enable();

        intfaceEnabled = true;
    }
}

// 0x45448C
void intface_disable()
{
    if (intfaceEnabled) {
        display_disable();
        win_disable_button(inventoryButton);
        win_disable_button(optionsButton);
        win_disable_button(skilldexButton);
        win_disable_button(automapButton);
        win_disable_button(pipboyButton);
        win_disable_button(characterButton);
        if (itemButtonItems[itemCurrentItem].isDisabled == 0) {
            win_disable_button(itemButton);
        }
        win_disable_button(endTurnButton);
        win_disable_button(endCombatButton);
        intfaceEnabled = false;
    }
}

// 0x454520
bool intface_is_enabled()
{
    return intfaceEnabled;
}


// --- REALTIME HEAL SLOT ----------------------------------------------------
//
// Shows the healing item RB would use NEXT, with its count (RB since 2026-10-09; it was LB+B).
//
// ⚠️ WHERE IT LIVES (2026-10-08, his layout): in the SIDE PANEL, the black strip right of the bar,
// with the aimed-part readout above it -- "dej ho vpravo vedle displaye, jak je ta černá část". That
// gives the end-turn box back to the combat curtains it was made for (intface_rt_combat_box_tick).
// On a screen only 640 wide there is no strip, and it falls back to its old spot ON that box: the
// bar is otherwise full -- the left panel is the message console, the middle is the weapon slot
// with the AP dots above it, and the right column is HP/AC plus the four screen buttons.
#define HEAL_SLOT_X 580
#define HEAL_SLOT_Y 38
#define HEAL_SLOT_W 57
#define HEAL_SLOT_H 58

// Where one of the two widgets is painted: which window, its buffer and pitch, and the top-left
// corner in it. The side panel when there is one, the bar otherwise.
typedef struct IntfaceSlot {
    int window;
    unsigned char* buffer;
    int pitch;
    int x;
    int y;
    bool side;
} IntfaceSlot;

static IntfaceSlot intface_slot_at(int barX, int y, int width)
{
    IntfaceSlot slot;
    if (sidePanelWindow != -1) {
        slot.window = sidePanelWindow;
        slot.buffer = sidePanelBuffer;
        slot.pitch = sidePanelWidth;
        // ⚠️ A COLUMN BESIDE THE BAR, NOT THE MIDDLE OF THE STRIP. Centred, the widgets drifted off to
        // the screen edge on any wide picture -- 1280 internal pixels wide leaves a 320 px strip, and
        // on the Deck they sat alone in the corner, far from the bar they belong to. Both widgets
        // share one column hugging the bar (the readout's width), each centred in it.
        const int readoutW = 78; // the column width the aimed-part readout set (that readout is gone)
        int columnW = readoutW < sidePanelWidth - 4 ? readoutW : sidePanelWidth - 4;
        int columnX = sidePanelWidth >= columnW + 8 ? 4 : (sidePanelWidth - columnW) / 2;
        slot.x = columnX + (columnW - width) / 2;
        if (slot.x < 0) {
            slot.x = 0;
        }
        slot.side = true;
    } else {
        slot.window = interfaceWindow;
        slot.buffer = interfaceBuffer;
        slot.pitch = 640;
        slot.x = barX;
        slot.side = false;
    }
    slot.y = y;
    return slot;
}

// What is currently PAINTED, so the per-frame poll is free when nothing changed. -2 means
// "never drawn", which is also what intface_redraw resets it to -- a full bar repaint wipes
// our pixels, and without that reset the cache would suppress the redraw that puts them back.
static int heal_slot_pid = -2;
static int heal_slot_count = -1;

void intface_invalidate_heal_slot()
{
    heal_slot_pid = -2;
    heal_slot_count = -1;
}

void intface_update_heal_slot(Object* item, int count)
{
    // Turn-based still owns the end-turn box (it is where END TURN / END COMBAT appear), so stay off.
    if (interfaceWindow == -1 || !combat_rt_enabled()) {
        return;
    }

    int pid = item != NULL ? item->pid : -1;
    if (pid == heal_slot_pid && count == heal_slot_count) {
        return;
    }
    heal_slot_pid = pid;
    heal_slot_count = count;

    IntfaceSlot slot = intface_slot_at(HEAL_SLOT_X, HEAL_SLOT_Y, HEAL_SLOT_W);
    unsigned char* base = slot.buffer + slot.pitch * slot.y + slot.x;

    if (slot.side) {
        // Out in the black there is no bar art to frame it, so it brings its own: the end-turn
        // box's closed frame, the very art it used to sit on -- it looks the same as it always did.
        CacheEntry* frameHandle;
        unsigned char* frame = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 104, 0, 0, 0), 0, 0, &frameHandle);
        if (frame != NULL) {
            buf_to_buf(frame, HEAL_SLOT_W, HEAL_SLOT_H, HEAL_SLOT_W, base, slot.pitch);
            art_ptr_unlock(frameHandle);
        }
    }

    // Flat black plate, inset 2px so the box's own bevel still frames it. The weapon slot does
    // the same thing for its item: a plain ground, or the striped art reads through the icon.
    buf_fill(base + slot.pitch * 2 + 2, HEAL_SLOT_W - 4, HEAL_SLOT_H - 4, slot.pitch, colorTable[0]);

    int oldFont = text_curr();
    text_font(101);

    int lineH = text_height();
    int iconH = HEAL_SLOT_H - 8 - lineH * 2;

    if (item != NULL) {
        // scale_art fits the art inside the box preserving aspect, through trans_cscale, so the
        // 88x59 inventory frame lands centred with index 0 left alone -- which is why the plate
        // is drawn first rather than after.
        scale_art(item_inv_fid(item), base + slot.pitch * 4 + 4, HEAL_SLOT_W - 8, iconH, slot.pitch);
    }

    // The binding, then the count -- same reading order as the weapon slot's mode label above
    // its ammo counter. Yellow for the label, green for a usable count, red for empty, matching
    // the colours the rest of the bar already uses for those three meanings.
    const char* label = "RB";
    int labelY = 4 + iconH;
    int labelX = (HEAL_SLOT_W - text_width((char*)label)) / 2;
    text_to_buf(base + slot.pitch * labelY + labelX, (char*)label, slot.pitch, slot.pitch, colorTable[32747]);

    char countText[8];
    snprintf(countText, sizeof(countText), "%d", count);
    int countY = labelY + lineH;
    int countX = (HEAL_SLOT_W - text_width(countText)) / 2;
    text_to_buf(base + slot.pitch * countY + countX, countText, slot.pitch, slot.pitch,
        count > 0 ? colorTable[992] : colorTable[31744]);

    text_font(oldFont);

    Rect rect = { slot.x, slot.y, slot.x + HEAL_SLOT_W - 1, slot.y + HEAL_SLOT_H - 1 };
    win_draw_rect(slot.window, &rect);
}

// --- AIMED-SHOT PART, IN THE WEAPON SLOT -----------------------------------
//
// Where an aimed RT goes: the part bound in the LB called-shot window (gpui_aim_last_part, default
// HEAD), printed in the WEAPON SLOT left of its bullseye while the slot is on its AIMED mode
// ("dej ty eyes a to kam mířím vedle toho terčíku… vlevo teda vedle toho terčíku", 2026-10-09).
//
// ⚠️ THE SIDE-PANEL READOUT IS GONE. From 2026-10-08 it printed "RB <part>" (later "AIM <part>")
// above the heal slot; once the part sat next to the bullseye that was the same fact twice, and he
// had it removed ("a zruš ho… tu duplicitu"). The slot is the only place it is shown now.

// The part the slot prints; UNCALLED = nothing. Fed by intface_update_aim_part from the pad tick.
static int aim_slot_weapon_part = HIT_LOCATION_UNCALLED;

// Short enough to sit between the AP cost and the bullseye in the worn letters below.
static const char* intface_aim_part_name(int hitLocation)
{
    switch (hitLocation) {
    case HIT_LOCATION_HEAD:
        return "HEAD";
    case HIT_LOCATION_EYES:
        return "EYES";
    case HIT_LOCATION_TORSO:
        return "TORSO";
    case HIT_LOCATION_GROIN:
        return "GROIN";
    case HIT_LOCATION_LEFT_ARM:
        return "L ARM";
    case HIT_LOCATION_RIGHT_ARM:
        return "R ARM";
    case HIT_LOCATION_LEFT_LEG:
        return "L LEG";
    case HIT_LOCATION_RIGHT_LEG:
        return "R LEG";
    default:
        return "BODY";
    }
}

void intface_update_aim_part(int hitLocation)
{
    // Turn-based asks for the part on every shot; there is nothing bound to print.
    if (interfaceWindow == -1 || !combat_rt_enabled()) {
        return;
    }

    // Repaint the slot when the part changes, but only while it is AIMED -- otherwise it shows no
    // part, and the next switch into the aimed mode repaints it anyway (intface_toggle_item_state /
    // intface_item_set_aiming).
    if (hitLocation != aim_slot_weapon_part) {
        aim_slot_weapon_part = hitLocation;
        int slotAction = itemButtonItems[itemCurrentItem].action;
        if (slotAction == INTERFACE_ITEM_ACTION_PRIMARY_AIMING
            || slotAction == INTERFACE_ITEM_ACTION_SECONDARY_AIMING) {
            intface_redraw_items();
        }
    }
}

// --- THE WORN LETTERS ----------------------------------------------------------
//
// *"Můžeme to udělat, ať je to taky tak ošuntělé jako celej text ve hře?"* The words on the bar
// (AP, SINGLE, BURST, THROW...) are not a font: they are hand-drawn art with thick strokes, each
// stroke shaded darker at its edges and lighter down its middle, the pixels varying from stroke to
// stroke. This label is drawn the same way, and it took five rounds to get there (2026-10-09/10):
//   1. font 101 in the art's colour -- read as pasted on;
//   2. letters cut from the word art, colours picked at random -- "pixelated, jiný styl": the
//      bevel IS the style, noise destroys it;
//   3. letters cut from the art with their own colours -- right, but 11 px, as big as SINGLE;
//      then "ten text zmenši": shrunk to 3/4 with area averaging, which no pixel art survives --
//   4. "je to blurry, proč je to blurry": every output pixel was a mix of up to four;
//   5. NOW: small letters DRAWN AT SIZE, 8 rows with 2-px strokes in the bar's blocky hand, nothing
//      resampled, so every pixel is a whole palette entry and the edges stay hard.
//
// The shading still comes from the original: every stroke row takes the colours of a real 3-px
// vertical stroke of the word art at the matching row -- left edge / middle / right edge along a
// longer run, the MIDDLE for both pixels of a 2-px stroke. Seven real strokes are used, one per
// stroke of the label, so neighbouring strokes differ the way they do on the bar.
//
// ⚠️ ONLY BRIGHT STROKES, AND NO DARK EDGE ON A 2-PX STROKE (2026-10-10): *"torso poslední o vypadá
// zvláštně"*. Giving a 2-px stroke the middle + RIGHT EDGE made every round letter's right side
// darker than its left, and the sources first chosen included SINGLE's N right stem, ten of whose
// eleven pixels are grey -- whichever letter drew that stroke came out half grey (TORSO's last O,
// GROIN's O and I). Measured per column, the strokes kept are the ones with no grey middle. Then the
// colours are moved onto the AP digits' (intface_worn_build_remap), because the word art is dimmer
// than the AP cost the label sits next to: "moc tmavé oproti AP 4".
//
// Checked in renders from MASTER.DAT beside AP 4 under six guns before shipping
// (.claude/tools/sideart_preview/slotpreview.js, env SMALL=1 MULTI=1 BRIGHT=1 TWO_MID=1 REMAP=1).
//
// ⚠️ NO SHADOW: the black one tried under it on a gun was "strašná".
#define WORN_H 8
#define WORN_SPACE 3
#define WORN_GAP 1
#define WORN_MAX_W 64

typedef struct IntfaceWornGlyph {
    char ch;
    const char* rows[WORN_H];
} IntfaceWornGlyph;

static const IntfaceWornGlyph intface_worn_glyphs[] = {
    { 'H', { "##...##", "##...##", "##...##", "#######", "#######", "##...##", "##...##", "##...##" } },
    { 'E', { "######", "######", "##....", "#####.", "#####.", "##....", "######", "######" } },
    { 'A', { "..###..", ".#####.", "##...##", "##...##", "#######", "#######", "##...##", "##...##" } },
    { 'D', { "#####..", "######.", "##..###", "##...##", "##...##", "##..###", "######.", "#####.." } },
    { 'Y', { "##..##", "##..##", "##..##", ".####.", "..##..", "..##..", "..##..", "..##.." } },
    { 'S', { ".#####", "######", "##....", "#####.", ".#####", "....##", "######", "#####." } },
    { 'T', { "######", "######", "..##..", "..##..", "..##..", "..##..", "..##..", "..##.." } },
    { 'O', { ".#####.", "#######", "##...##", "##...##", "##...##", "##...##", "#######", ".#####." } },
    { 'R', { "######.", "#######", "##...##", "#######", "######.", "##.###.", "##..###", "##...##" } },
    { 'G', { ".#####.", "#######", "##.....", "##.....", "##..###", "##...##", "#######", ".#####." } },
    { 'I', { "##", "##", "##", "##", "##", "##", "##", "##" } },
    { 'N', { "##...##", "###..##", "###..##", "####.##", "##.####", "##..###", "##..###", "##...##" } },
    { 'L', { "##....", "##....", "##....", "##....", "##....", "##....", "######", "######" } },
    { 'M', { "##....##", "###..###", "########", "##.##.##", "##.##.##", "##....##", "##....##", "##....##" } },
};

// Real 3-px vertical strokes of the word art, 11 rows tall: { intrface.lst index, left column }.
// SINGLE (43): E's stem, L's stem, N's left stem. THRUST (45): H's two stems. BURST (40): B's stem,
// R's stem. (SINGLE's I at column 9 stays the fallback for a source row with a gap.)
#define WORN_STROKE_ROWS 11
#define WORN_STROKE_ARTS 3
static const int intface_worn_stroke_arts[WORN_STROKE_ARTS] = { 43, 45, 40 };
static const int intface_worn_strokes[][2] = {
    { 43, 44 },
    { 45, 10 },
    { 45, 18 },
    { 40, 0 },
    { 40, 22 },
    { 43, 36 },
    { 43, 13 },
};

static const IntfaceWornGlyph* intface_worn_glyph(char ch)
{
    for (size_t i = 0; i < sizeof(intface_worn_glyphs) / sizeof(intface_worn_glyphs[0]); i++) {
        if (intface_worn_glyphs[i].ch == ch) {
            return &(intface_worn_glyphs[i]);
        }
    }
    return NULL;
}

// Width in pixels; -1 when the text has a letter the set does not carry (the caller draws nothing
// rather than a word with a hole in it).
static int intface_worn_text_width(const char* text)
{
    int width = 0;
    for (const char* p = text; *p != '\0'; p++) {
        if (p != text) {
            width += WORN_GAP;
        }
        if (*p == ' ') {
            width += WORN_SPACE;
            continue;
        }
        const IntfaceWornGlyph* glyph = intface_worn_glyph(*p);
        if (glyph == NULL) {
            return -1;
        }
        width += (int)strlen(glyph->rows[0]);
    }
    return width;
}

// ⚠️ RECOLOURED ONTO THE AP DIGITS' COLOURS (2026-10-10): *"moc tmavé oproti AP 4"*. The word art
// the shading comes from (SINGLE, BURST...) is a dimmer mix with greys and browns in it; the digits
// are a brighter yellow. Every pixel keeps its place in the bevel but takes the digits' colour at the
// same BRIGHTNESS RANK: the darkest word-art pixels become the darkest digit pixels, the brightest the
// brightest. Measured from the art at first use, not hard-coded -- a replaced interface FRM brings its
// own colours along.
static unsigned char intface_worn_remap[256];
static bool intface_worn_remap_ready = false;

static float intface_worn_luma(int index)
{
    return 0.299f * cmap[index * 3] + 0.587f * cmap[index * 3 + 1] + 0.114f * cmap[index * 3 + 2];
}

static void intface_worn_count(int artNum, unsigned int* counts)
{
    CacheEntry* handle;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, artNum, 0, 0, 0), &handle);
    if (art == NULL) {
        return;
    }
    int size = art_frame_width(art, 0, 0) * art_frame_length(art, 0, 0);
    unsigned char* data = art_frame_data(art, 0, 0);
    for (int i = 0; i < size; i++) {
        if (data[i] != 0) {
            counts[data[i]]++;
        }
    }
    art_ptr_unlock(handle);
}

static void intface_worn_build_remap()
{
    for (int i = 0; i < 256; i++) {
        intface_worn_remap[i] = (unsigned char)i;
    }
    intface_worn_remap_ready = true;

    // Source: the bar's words (SINGLE, UNARMED, THRUST, BURST, THROW). Target: the ten AP digits
    // (art 290), the brightest text on the slot.
    unsigned int from[256] = { 0 };
    unsigned int to[256] = { 0 };
    static const int sourceArts[] = { 43, 287, 45, 40, 117 };
    for (size_t i = 0; i < sizeof(sourceArts) / sizeof(sourceArts[0]); i++) {
        intface_worn_count(sourceArts[i], from);
    }
    intface_worn_count(290, to);

    unsigned int fromTotal = 0;
    unsigned int toTotal = 0;
    for (int i = 0; i < 256; i++) {
        fromTotal += from[i];
        toTotal += to[i];
    }
    if (fromTotal == 0 || toTotal == 0) {
        return; // art missing: leave the colours as they are
    }

    // The target's palette entries from darkest to brightest (a 256-entry insertion sort, once).
    int order[256];
    int orderCount = 0;
    for (int i = 0; i < 256; i++) {
        if (to[i] == 0) {
            continue;
        }
        int at = orderCount++;
        while (at > 0 && intface_worn_luma(order[at - 1]) > intface_worn_luma(i)) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }

    for (int v = 1; v < 256; v++) {
        if (from[v] == 0) {
            continue;
        }
        // v's rank among the source pixels: those darker, plus half of those exactly as bright.
        float luma = intface_worn_luma(v);
        unsigned int darker = 0;
        unsigned int same = 0;
        for (int u = 0; u < 256; u++) {
            if (from[u] == 0) {
                continue;
            }
            float l = intface_worn_luma(u);
            if (l < luma) {
                darker += from[u];
            } else if (l == luma) {
                same += from[u];
            }
        }
        float rank = (darker + same * 0.5f) / fromTotal;

        // ...and the target pixel at that rank.
        unsigned int want = (unsigned int)(rank * toTotal);
        if (want >= toTotal) {
            want = toTotal - 1;
        }
        unsigned int seen = 0;
        for (int k = 0; k < orderCount; k++) {
            seen += to[order[k]];
            if (seen > want) {
                intface_worn_remap[v] = (unsigned char)order[k];
                break;
            }
        }
    }
}

// Paints `text` into `dest` (width x WORN_H, pitch == width, already zeroed: 0 is transparent, the
// same convention as FRM data, so the result goes through trans_buf_to_buf like any art). False if
// the stroke art could not be loaded -- the caller then draws nothing.
static bool intface_worn_text_compose(const char* text, unsigned char* dest, int width)
{
    if (!intface_worn_remap_ready) {
        intface_worn_build_remap();
    }

    // The words the strokes come from, locked once for the whole label. [0] is SINGLE, the fallback.
    CacheEntry* handles[WORN_STROKE_ARTS];
    Art* arts[WORN_STROKE_ARTS];
    for (int i = 0; i < WORN_STROKE_ARTS; i++) {
        arts[i] = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, intface_worn_stroke_arts[i], 0, 0, 0), &(handles[i]));
        if (arts[i] == NULL) {
            for (int j = 0; j < i; j++) {
                art_ptr_unlock(handles[j]);
            }
            return false;
        }
    }
    Art* single = arts[0];

    bool ok = true;
    int x = 0;
    for (const char* p = text; *p != '\0'; p++) {
        int index = (int)(p - text);
        if (p != text) {
            x += WORN_GAP;
        }
        if (*p == ' ') {
            x += WORN_SPACE;
            continue;
        }
        const IntfaceWornGlyph* glyph = intface_worn_glyph(*p);
        if (glyph == NULL) {
            ok = false;
            break;
        }

        int glyphW = (int)strlen(glyph->rows[0]);
        for (int y = 0; y < WORN_H; y++) {
            const char* row = glyph->rows[y];
            int sy = y * WORN_STROKE_ROWS / WORN_H;
            for (int gx = 0; gx < glyphW && x + gx < width; gx++) {
                if (row[gx] != '#') {
                    continue;
                }

                // The run of stroke pixels this one sits in, and where in it.
                int runStart = gx;
                while (runStart > 0 && row[runStart - 1] == '#') {
                    runStart--;
                }
                int runEnd = gx;
                while (runEnd < glyphW - 1 && row[runEnd + 1] == '#') {
                    runEnd++;
                }
                int column;
                if (runEnd - runStart <= 1) {
                    column = 1; // a 1- or 2-px stroke is all middle (see above)
                } else {
                    column = gx == runStart ? 0 : (gx == runEnd ? 2 : 1);
                }

                // One real stroke per stroke of the label (letter index and where the run starts).
                const int* stroke = intface_worn_strokes[(index * 3 + runStart * 2) % (int)(sizeof(intface_worn_strokes) / sizeof(intface_worn_strokes[0]))];
                Art* art = single;
                for (int i = 0; i < WORN_STROKE_ARTS; i++) {
                    if (intface_worn_stroke_arts[i] == stroke[0]) {
                        art = arts[i];
                    }
                }
                int artW = art_frame_width(art, 0, 0);
                unsigned char* data = art_frame_data(art, 0, 0);
                unsigned char v = data[sy * artW + stroke[1] + column];
                if (v == 0) {
                    // A gap in that stroke at this row: SINGLE's I is solid top to bottom.
                    unsigned char* iData = art_frame_data(single, 0, 0);
                    v = iData[sy * art_frame_width(single, 0, 0) + 9 + column];
                }
                if (v != 0) {
                    dest[y * width + x + gx] = intface_worn_remap[v];
                }
            }
        }
        x += glyphW;
    }

    for (int i = 0; i < WORN_STROKE_ARTS; i++) {
        art_ptr_unlock(handles[i]);
    }
    return ok;
}

// --- THE COMBAT BOX, BACK IN REALTIME (2026-10-08) -------------------------
//
// "Když budu v kombatu, tak se animace, že jsem v combatu, ukáže tam vpravo dole. Ta animace mě
// nezpomalí ... jakmile podržím ukončit combat, pravý dpad, tak se ta animace změní zase na normál
// -- prostě to políčko tak fungovalo ve Falloutu 1."
//
// Vanilla opens these curtains in combat_begin (intface_end_window_open), spinning its own loop until
// the five frames are drawn while the world stands still. Here the same frames are stepped one per
// tick from combat_rt_update: the same art, sounds and lights -- iciboxx1 as it opens, the GREEN
// lights with icombat2 once it is open (in realtime it is always the dude's turn), icibcxx1 as it
// closes. No END TURN / END COMBAT buttons are created: a fight ends on the held D-pad right, and
// END TURN has nothing to end.
//
// ⚠️ ONLY WITH A SIDE PANEL: without one the heal slot still lives on this box.
// ⚠️ CLOSING WAITS RT_BOX_CLOSE_DELAY after the fight goes cold. Realtime engagement can drop for a
// beat and come straight back, and the curtains must not flap -- or chime -- with it.
#define RT_BOX_CLOSE_DELAY 0.75f

static int rt_box_frame = 0; // endanim frame painted now; 0 = closed, which is the bar's own art
static int rt_box_dir = 0; // +1 opening, -1 closing, 0 settled
static bool rt_box_want = false;
static bool rt_box_lit = false;
static bool rt_box_dirty = false; // a bar repaint may have wiped it: paint the current state again
static float rt_box_accum = 0.0f;
static float rt_box_cold_for = 0.0f;

// MODS "Combat box sound" (cfg combat_box_sound): the open/close clunk and the lights chime. ON by
// default because ON is vanilla -- "je dost otravnej, ale někdo si ho určitě rád nechá".
static bool rt_box_sound = true;

bool intface_get_combat_box_sound()
{
    return rt_box_sound;
}

void intface_set_combat_box_sound(bool on)
{
    rt_box_sound = on;
}

static void intface_rt_box_paint(Art* art, int frame, bool lights)
{
    unsigned char* src = art_frame_data(art, frame, 0);
    if (src != NULL) {
        buf_to_buf(src, 57, 58, 57, interfaceBuffer + 640 * 38 + 580, 640);
    }

    if (lights) {
        // endltgrn.frm -- the green lights vanilla shows while it is your turn.
        CacheEntry* lightsHandle;
        unsigned char* lights = art_ptr_lock_data(art_id(OBJ_TYPE_INTERFACE, 109, 0, 0, 0), 0, 0, &lightsHandle);
        if (lights != NULL) {
            trans_buf_to_buf(lights, 57, 58, 57, interfaceBuffer + 640 * 38 + 580, 640);
            art_ptr_unlock(lightsHandle);
        }
    }

    win_draw_rect(interfaceWindow, &endWindowRect);
}

void intface_rt_combat_box_tick(bool engaged, float dt)
{
    if (interfaceWindow == -1 || sidePanelWindow == -1 || !combat_rt_enabled()) {
        return;
    }

    if (engaged) {
        rt_box_want = true;
        rt_box_cold_for = 0.0f;
    } else if (rt_box_want) {
        rt_box_cold_for += dt;
        if (rt_box_cold_for >= RT_BOX_CLOSE_DELAY) {
            rt_box_want = false;
        }
    }

    // Settled and untouched is every frame of the game but a few: leave before touching the art.
    bool settled = rt_box_dir == 0 && (rt_box_want ? rt_box_lit : rt_box_frame == 0);
    if (settled && !rt_box_dirty) {
        return;
    }

    CacheEntry* handle;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, 104, 0, 0, 0), &handle);
    if (art == NULL) {
        return;
    }

    int last = art_frame_max_frame(art) - 1;
    int target = rt_box_want ? last : 0;

    if (rt_box_dirty) {
        rt_box_dirty = false;
        intface_rt_box_paint(art, rt_box_frame, rt_box_lit);
    }

    if (rt_box_frame != target) {
        int dir = target > rt_box_frame ? 1 : -1;
        if (dir != rt_box_dir) {
            rt_box_dir = dir;
            rt_box_accum = 0.0f;
            if (rt_box_sound) {
                gsound_play_sfx_file(dir > 0 ? "iciboxx1" : "icibcxx1");
            }
        }

        int fps = art_frame_fps(art);
        if (fps <= 0) {
            fps = 10;
        }

        rt_box_accum += dt;
        if (rt_box_accum >= 1.0f / (float)fps) {
            // One frame per tick at most: a long frame never skips the curtains.
            rt_box_accum = 0.0f;
            rt_box_frame += dir;
            rt_box_lit = false;
            intface_rt_box_paint(art, rt_box_frame, false);
        }
    } else {
        rt_box_dir = 0;
        if (rt_box_want && !rt_box_lit) {
            rt_box_lit = true;
            if (rt_box_sound) {
                gsound_play_sfx_file("icombat2");
            }
            intface_rt_box_paint(art, rt_box_frame, true);
        }
    }

    art_ptr_unlock(handle);
}

// A new game or a load: shut at once, no animation, no sound.
void intface_rt_combat_box_reset()
{
    bool painted = rt_box_frame != 0 || rt_box_lit;

    rt_box_frame = 0;
    rt_box_dir = 0;
    rt_box_want = false;
    rt_box_lit = false;
    rt_box_dirty = false;
    rt_box_accum = 0.0f;
    rt_box_cold_for = 0.0f;

    if (painted && interfaceWindow != -1) {
        CacheEntry* handle;
        Art* art = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, 104, 0, 0, 0), &handle);
        if (art != NULL) {
            intface_rt_box_paint(art, 0, false);
            art_ptr_unlock(handle);
        }
    }
}

void intface_rt_combat_box_invalidate()
{
    if (rt_box_frame != 0 || rt_box_lit) {
        rt_box_dirty = true;
    }
}

// 0x454528
void intface_redraw()
{
    if (interfaceWindow != -1) {
        intface_update_items(false);
        intface_update_hit_points(false);
        intface_update_ac(false);
        refresh_box_bar_win();
        // The repaint above wiped our pixels; let the next poll put them back.
        intface_invalidate_heal_slot();
        intface_rt_combat_box_invalidate();
        win_draw(interfaceWindow);
    }
    refresh_box_bar_win();
}

// Last hit points rendered in interface.
//
// Used to animate changes. Also the value the non-blocking roll below is currently
// showing, which is why it is file scope and not a function-local static like vanilla.
//
// 0x505614
static int last_points = 0;

// Last color used to render hit points in interface.
//
// Used to animate changes.
//
// 0x505618
static int last_points_color = INTERFACE_NUMBERS_COLOR_RED;

// NON-BLOCKING HIT-POINT COUNTER (realtime combat).
//
// Vanilla's animated counter rolls the digits inside intface_rotate_numbers' own
// `while (previousValue != value)` loop, which calls block_for_tocks() and pumps the mouse
// but NEVER process_bk -- so the entire world (AI, movement, projectiles, combat_rt) stops
// for ~250ms on every hit the dude takes. Harmless in turn-based, where nothing else is
// moving; in realtime it was the "time freezes when I get hit, smooth on a miss" freeze,
// and the roll was simply switched off there.
//
// This is the same animation stepped from the frame loop instead: one point per tick, each
// step drawn through the *static* (delay 0) path of intface_rotate_numbers. Same look,
// nothing blocks.
//
// Vanilla spends ~250ms on the whole roll no matter how big the change is
// (delay = 250 / (diff + 1) per step); keeping that means a scratch and a shotgun blast
// take the same time to read.
#define HP_ROLL_TOTAL_SECONDS (0.25f)

// Vanilla's counter is an ODOMETER, not a sequence of numbers. Every single point costs three
// drawn frames (see intface_rotate_numbers): the digit rolls OUT upwards (upSrc), the value is
// incremented, the digit rolls IN from below (downSrc), and only then is the new digit drawn.
// Carries cascade the same way into tens and hundreds. Drawing only the third of those -- which
// is what intface_rotate_numbers(..., delay = 0) does -- gives a counter that snaps from number
// to number and reads nothing like the original. So the phases are stepped here instead.
#define HP_ROLL_PHASE_OUT 0    // upSrc over every digit that is about to change
#define HP_ROLL_PHASE_IN 1     // value stepped, downSrc over the digits that changed
#define HP_ROLL_PHASE_SETTLE 2 // the new digits, clean
#define HP_ROLL_PHASES 3

// AT MOST ONE PHASE PER TICK. A frame loop cannot show more sub-frames than it draws frames, so
// burning several phases in one tick would silently drop exactly the frames that ARE the animation
// -- the snap we are fixing. A big hit therefore takes longer than vanilla's fixed ~250 ms, which
// is the right trade in realtime: the roll stays visible instead of being compressed into nothing.
// It also makes a hitch harmless: a long frame advances one phase, never a burst.

static bool hp_roll_active = false;
static int hp_roll_target = 0;
static float hp_roll_step_seconds = 0.0f; // per PHASE, not per point
static float hp_roll_accum = 0.0f;
static int hp_roll_phase = HP_ROLL_PHASE_OUT;

// The color thresholds vanilla applies at 25% / 50% of max HP. Evaluated per intermediate
// value, which reproduces for free what vanilla does by splitting the roll into colored
// segments (the v1[]/v2[] pairs in intface_update_hit_points).
static int intface_hp_points_color(int hp)
{
    int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);

    if (hp < (int)((double)maxHp * 0.25)) {
        return INTERFACE_NUMBERS_COLOR_RED;
    } else if (hp < (int)((double)maxHp * 0.5)) {
        return INTERFACE_NUMBERS_COLOR_YELLOW;
    } else {
        return INTERFACE_NUMBERS_COLOR_WHITE;
    }
}

// Which decimal digits differ between two values: bit 0 ones, bit 1 tens, bit 2 hundreds. Drives
// the carry cascade -- vanilla rolls the tens only on the step where the tens actually turn over.
static int intface_hp_digit_mask(int a, int b)
{
    int aa = abs(a);
    int bb = abs(b);
    int mask = 0;

    if (aa % 10 != bb % 10) {
        mask |= 1;
    }
    if ((aa / 10) % 10 != (bb / 10) % 10) {
        mask |= 2;
    }
    if (aa / 100 != bb / 100) {
        mask |= 4;
    }

    return mask;
}

// One frame of the roll. Same buffer arithmetic as intface_rotate_numbers (9x17 digits in a
// 360-wide sheet, sign 6 wide, at 473,40), but it draws a single frame and returns instead of
// spinning on block_for_tocks. transition: 0 none, +1 upSrc (rolling out), -1 downSrc (rolling in);
// digitMask selects which digit positions the transition sprite covers.
static void intface_hp_roll_draw(int value, int digitMask, int transition)
{
    if (value > 999) {
        value = 999;
    } else if (value < -999) {
        value = -999;
    }

    unsigned char* numbers = numbersBuffer + last_points_color;
    unsigned char* dest = interfaceBuffer + 640 * 40;

    unsigned char* downSrc = numbers + 90;
    unsigned char* upSrc = numbers + 99;
    unsigned char* minusSrc = numbers + 108;
    unsigned char* plusSrc = numbers + 114;

    unsigned char* signDest = dest + 473;
    unsigned char* hundredsDest = dest + 473 + 6;
    unsigned char* tensDest = dest + 473 + 6 + 9;
    unsigned char* onesDest = dest + 473 + 6 + 9 * 2;

    int sign = value >= 0 ? 1 : -1;
    int magnitude = abs(value);

    int ones = magnitude % 10;
    int tens = (magnitude / 10) % 10;
    int hundreds = magnitude / 100;

    buf_to_buf(numbers + 9 * hundreds, 9, 17, 360, hundredsDest, 640);
    buf_to_buf(numbers + 9 * tens, 9, 17, 360, tensDest, 640);
    buf_to_buf(numbers + 9 * ones, 9, 17, 360, onesDest, 640);
    buf_to_buf(sign >= 0 ? plusSrc : minusSrc, 6, 17, 360, signDest, 640);

    if (transition != 0) {
        unsigned char* transitionSrc = transition > 0 ? upSrc : downSrc;

        if ((digitMask & 1) != 0) {
            buf_to_buf(transitionSrc, 9, 17, 360, onesDest, 640);
        }
        if ((digitMask & 2) != 0) {
            buf_to_buf(transitionSrc, 9, 17, 360, tensDest, 640);
        }
        if ((digitMask & 4) != 0) {
            buf_to_buf(transitionSrc, 9, 17, 360, hundredsDest, 640);
        }
    }

    Rect numbersRect = { 473, 40, 473 + 33, 40 + 17 };
    win_draw_rect(interfaceWindow, &numbersRect);
}

// Start -- or re-target -- the non-blocking roll. Re-targeting mid-roll is the common case
// in realtime (a second hit lands while the first is still rolling) and must NOT restart
// from the old value, or the counter would visibly jump backwards.
static void intface_hp_roll_start()
{
    if (interfaceWindow == -1 || obj_dude == NULL) {
        return;
    }

    hp_roll_target = critter_get_hits(obj_dude);

    if (hp_roll_target == last_points) {
        // Nothing to roll (damage fully absorbed, heal for 0). Any roll already in flight is
        // left alone -- it is heading here anyway.
        return;
    }

    // Vanilla budgets the WHOLE roll at ~250 ms regardless of size; keep that intent by dividing it
    // across every phase. The one-phase-per-tick rule stretches a big roll past it rather than
    // dropping frames -- see the note at HP_ROLL_PHASES.
    hp_roll_step_seconds = HP_ROLL_TOTAL_SECONDS
        / (float)((abs(hp_roll_target - last_points) + 1) * HP_ROLL_PHASES);

    if (!hp_roll_active) {
        hp_roll_accum = 0.0f;
        hp_roll_phase = HP_ROLL_PHASE_OUT;
        hp_roll_active = true;
    }
}

// Frame-loop pump for the roll above. Called from combat_rt_update, so it inherits the same
// pause rules as the rest of the combat model (a radial wheel holds it still too).
void intface_hp_roll_tick(float dt)
{
    if (!hp_roll_active) {
        return;
    }

    if (interfaceWindow == -1 || obj_dude == NULL) {
        hp_roll_active = false;
        return;
    }

    hp_roll_accum += dt;

    if (hp_roll_accum < hp_roll_step_seconds) {
        return;
    }

    // Exactly one phase per tick; drop whatever time is left over so a hitch cannot bank up a burst.
    hp_roll_accum = 0.0f;

    int step = hp_roll_target > last_points ? 1 : -1;

    switch (hp_roll_phase) {
    case HP_ROLL_PHASE_OUT:
        // The digits about to turn over roll upwards, still showing the OLD value.
        last_points_color = intface_hp_points_color(last_points);
        intface_hp_roll_draw(last_points, intface_hp_digit_mask(last_points, last_points + step), 1);
        hp_roll_phase = HP_ROLL_PHASE_IN;
        break;

    case HP_ROLL_PHASE_IN: {
        // Step the value, then show those same digits arriving from below.
        int previous = last_points;
        last_points += step;
        last_points_color = intface_hp_points_color(last_points);
        intface_hp_roll_draw(last_points, intface_hp_digit_mask(previous, last_points), -1);
        hp_roll_phase = HP_ROLL_PHASE_SETTLE;
        break;
    }

    case HP_ROLL_PHASE_SETTLE:
    default:
        // The new number, clean. One point is now complete.
        last_points_color = intface_hp_points_color(last_points);
        intface_hp_roll_draw(last_points, 0, 0);
        hp_roll_phase = HP_ROLL_PHASE_OUT;

        if (last_points == hp_roll_target) {
            hp_roll_active = false;
        }
        break;
    }
}

// Drop a roll in flight and snap the counter to the truth. Anything that invalidates the
// value underneath it -- a load, a map teardown, a new game -- must land here, or a stale
// target would keep painting over the real hit points.
void intface_hp_roll_cancel()
{
    if (!hp_roll_active) {
        return;
    }

    hp_roll_active = false;
    hp_roll_accum = 0.0f;
    hp_roll_phase = HP_ROLL_PHASE_OUT;

    if (interfaceWindow != -1 && obj_dude != NULL) {
        intface_update_hit_points(false);
    }
}

// Render hit points.
//
// 0x4545A8
void intface_update_hit_points(bool animate)
{
    if (interfaceWindow == -1) {
        return;
    }

    // Realtime combat: hand the animation to the frame-loop roll above instead of the
    // blocking one below. Gated on combat_rt_active() (a fight is actually live) rather than
    // on the realtime mode flag, because the roll only ticks from combat_rt_update -- a
    // stimpak taken inside the inventory's own modal loop would otherwise sit frozen
    // mid-roll until that screen closed.
    if (animate && combat_rt_active()) {
        intface_hp_roll_start();
        return;
    }

    // Something authoritative is setting the value (a load, a level-up, the cancel above):
    // whatever was in flight is stale.
    hp_roll_active = false;
    hp_roll_accum = 0.0f;
    hp_roll_phase = HP_ROLL_PHASE_OUT;

    int hp = critter_get_hits(obj_dude);
    int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);

    int red = (int)((double)maxHp * 0.25);
    int yellow = (int)((double)maxHp * 0.5);

    int color = intface_hp_points_color(hp);

    int v1[4];
    int v2[3];
    int count = 1;

    v1[0] = last_points;
    v2[0] = last_points_color;

    if (last_points_color != color) {
        if (hp >= last_points) {
            if (last_points < red && hp >= red) {
                v1[count] = red;
                v2[count] = INTERFACE_NUMBERS_COLOR_YELLOW;
                count += 1;
            }

            if (last_points < yellow && hp >= yellow) {
                v1[count] = yellow;
                v2[count] = INTERFACE_NUMBERS_COLOR_WHITE;
                count += 1;
            }
        } else {
            if (last_points >= yellow && hp < yellow) {
                v1[count] = yellow;
                v2[count] = INTERFACE_NUMBERS_COLOR_YELLOW;
                count += 1;
            }

            if (last_points >= red && hp < red) {
                v1[count] = red;
                v2[count] = INTERFACE_NUMBERS_COLOR_RED;
                count += 1;
            }
        }
    }

    v1[count] = hp;

    if (animate) {
        int delay = 250 / (abs(last_points - hp) + 1);
        for (int index = 0; index < count; index++) {
            intface_rotate_numbers(473, 40, v1[index], v1[index + 1], v2[index], delay);
        }
    } else {
        intface_rotate_numbers(473, 40, last_points, hp, color, 0);
    }

    last_points = hp;
    last_points_color = color;
}

// Render armor class.
//
// 0x454770
void intface_update_ac(bool animate)
{
    // Last armor class rendered in interface.
    //
    // Used to animate changes.
    //
    // 0x50561C
    static int last_ac = 0;

    // The same guard the other indicators carry (intface_update_move_points et al). It matters
    // now that combat_rt pushes this every frame the value moves: with no window there is no
    // interfaceBuffer to blit into.
    if (interfaceWindow == -1) {
        return;
    }

    int armorClass = stat_level(obj_dude, STAT_ARMOR_CLASS);

    // TEMP DIAG (STRIP with ac-diag): why the AC readout moves. Vanilla stat_level adds the
    // critter's ENGINE combat.ap to STAT_ARMOR_CLASS while isInCombat() and it is not its turn
    // (stat.cc:199) -- "unused AP make you harder to hit". In realtime BOTH conditions hold for
    // the whole fight (combat_rt owns COMBAT_STATE_0x01; combat_begin/combat_turn never run, so
    // combat_turn_obj stays NULL), and combat.ap is NOT the real pool -- combat_rt pins it to max
    // every frame so anim.cc will not truncate walking, while combat_attack keeps subtracting the
    // weapon cost from it. Log the decomposition on every CHANGE so we can say what actually
    // moved: `armor` is the AC without the AP term, `eap` the term itself, `rtap` the real pool.
    if (armorClass != last_ac) {
        int armorOnly = stat_get_base(obj_dude, STAT_ARMOR_CLASS)
            + stat_get_bonus(obj_dude, STAT_ARMOR_CLASS);
        trace_event(TRACE_STATE, "ac",
            "%d->%d armor=%d eap=%d rtap=%d/%d inCombat=%d whoseTurn=%d anim=%d",
            last_ac, armorClass, armorOnly,
            obj_dude->data.critter.combat.ap,
            combat_rt_player_ap(), combat_rt_player_max_ap(),
            isInCombat() ? 1 : 0,
            combat_whose_turn() == obj_dude ? 1 : 0,
            animate ? 1 : 0);
    }

    int delay = 0;
    if (animate) {
        delay = 250 / (abs(last_ac - armorClass) + 1);
    }

    intface_rotate_numbers(473, 75, last_ac, armorClass, 0, delay);

    last_ac = armorClass;
}

// One bulb of the AP strip at position `slot` (0-9).
static void intface_draw_ap_bulb(unsigned char* art, int slot)
{
    buf_to_buf(art, 5, 5, 5, interfaceBuffer + 14 * 640 + 316 + slot * 9, 640);
}

// 0x4547D4
void intface_update_move_points(int actionPoints, int bonusMove)
{
    unsigned char* frmData;

    if (interfaceWindow == -1) {
        return;
    }

    // Under realtime combat the engine combat.ap is held maxed so movement stays
    // free, so it can't drive the bar. Override every caller with the real RT pool
    // so the indicator shows AP being spent/regenerated -- and the Bonus Move reserve
    // as vanilla's own yellow bulbs, instead of the turn-based combat_free_move that
    // realtime never sets. Hidden while the dude is stopped on an empty pool: the reserve
    // keeps refilling there, but it cannot move him until the red countdown below ends,
    // and yellow lights beside the reds would say it could.
    if (combat_rt_enabled()) {
        actionPoints = combat_rt_player_ap();
        bonusMove = combat_rt_move_blocked() ? 0 : combat_rt_bonus_move_ap();
    }

    buf_to_buf(movePointBackground, 90, 5, 90, interfaceBuffer + 14 * 640 + 316, 640);

    if (actionPoints == -1) {
        frmData = moveLightRed;
        actionPoints = 10;
        bonusMove = 0;
    } else {
        frmData = moveLightGreen;

        // Realtime combat: an empty pool STOPS the dude outright, and he may only walk
        // again once AP has climbed back to the resume threshold (combat_rt.cc,
        // rt_move_blocked). Nothing on screen used to say so -- the player just lost
        // control mid-step. So for exactly that window the points refilling the pool
        // light up RED instead of green, and the strip snaps back to green on the point
        // that frees him: watching the reds go 1-2-3 IS the countdown to moving again.
        //
        // Keyed on the BLOCKED LATCH, not on "AP is below the threshold". The pool also
        // sits below it after a shot, and there the dude can still walk -- painting that
        // red would be claiming a lockout that is not happening. So the reds only ever
        // appear while the pool is climbing back from a zero that actually stopped him.
        //
        // It is the same red light vanilla puts on this strip via actionPoints == -1
        // (the branch right above) to mean "you cannot act right now" -- the art and the
        // meaning are the game's own, only the trigger is ours. Nothing else changes:
        // the fill loop below is untouched, so the count of lit bulbs is still the pool.
        if (combat_rt_move_blocked()) {
            frmData = moveLightRed;
        }
    }

    // ⚠️ THE STRIP HAS TEN BULBS AND NOW SHOWS UP TO TWENTY POINTS (2026-10-04). Vanilla -- Fallout 1
    // AND Fallout 2 (fallout2-ce's interfaceRenderActionPoints clamps the AP to ten and trims the
    // yellow to whatever room is left) -- simply dropped what did not fit, so a 10-AP character never
    // saw his Bonus Move points and nobody ever saw an AP past the tenth. His rules, in order:
    //   "11 a 12 budou prostě žluté od 1 a 2" / "žlutý bod se počítá za dva" -- overflow goes onto the
    //     front of the strip, and such a bulb is worth two;
    //   "sjednoť to" -- a plain wrap made yellow mean one point at the end of the strip and two at its
    //     start (9 AP + 2 lit bulb 10 AND bulb 1), so: all-or-nothing;
    //   "udělej dvojku, světle zelená za dva AP" -- AP past ten get the same treatment in light green.
    //
    // So, with A = action points and M = Bonus Move points:
    //   * A + M <= 10 (it fits): A green, then M yellow -- ONE point per bulb, both games' own layout.
    //   * otherwise, from the front of the strip:
    //       M yellow      -- each worth TWO: the AP underneath plus the movement point on top;
    //       A - 10 light  -- each worth TWO action points (only when A itself is past ten);
    //       the rest green, one AP each; any bulbs left over stay dark.
    //     10 + 2 = YY + 8 green.  9 + 2 = YY + 7 green + one dark.  11 + 2 = YY, one light, 7 green.
    //     12 + 0 = two light + 8 green.  At any one moment every bulb of a colour means the same.
    //   Capped at ten double bulbs (twenty points); 5 AP + 6 movement points, the one combination the
    //   rule cannot fit, shows ten of the eleven -- vanilla showed five.
    //
    // While the dude is stopped on an empty pool the bonus is hidden (bonusMove == 0 here) and his AP
    // are at most a couple of points, so the red countdown strip is never mixed with these bulbs.
    int apUnits = actionPoints > 0 ? actionPoints : 0;
    int moveUnits = bonusMove > 0 ? bonusMove : 0;

    int yellowDouble = 0;
    int lightDouble = 0;
    int greenSingle;
    int yellowSingle = 0;

    if (apUnits + moveUnits <= 10) {
        greenSingle = apUnits;
        yellowSingle = moveUnits;
    } else {
        // Every yellow needs an AP to sit on, and there are only ten bulbs.
        yellowDouble = moveUnits;
        if (yellowDouble > apUnits) {
            yellowDouble = apUnits;
        }
        if (yellowDouble > 10) {
            yellowDouble = 10;
        }

        lightDouble = apUnits > 10 ? apUnits - 10 : 0;
        if (lightDouble > 10 - yellowDouble) {
            lightDouble = 10 - yellowDouble;
        }

        greenSingle = apUnits - yellowDouble - 2 * lightDouble;
        if (greenSingle > 10 - yellowDouble - lightDouble) {
            greenSingle = 10 - yellowDouble - lightDouble;
        }
        if (greenSingle < 0) {
            greenSingle = 0;
        }
    }

    int slot = 0;
    for (int index = 0; index < yellowDouble && slot < 10; index++) {
        intface_draw_ap_bulb(moveLightYellowLit, slot++);
    }
    for (int index = 0; index < lightDouble && slot < 10; index++) {
        intface_draw_ap_bulb(moveLightGreenLight, slot++);
    }
    for (int index = 0; index < greenSingle && slot < 10; index++) {
        intface_draw_ap_bulb(frmData, slot++);
    }
    for (int index = 0; index < yellowSingle && slot < 10; index++) {
        intface_draw_ap_bulb(moveLightYellowLit, slot++);
    }

    if (!insideInit) {
        win_draw_rect(interfaceWindow, &movePointRect);
    }
}

// 0x454898
int intface_get_attack(int* hitMode, bool* aiming)
{
    if (interfaceWindow == -1) {
        return -1;
    }

    *aiming = false;

    switch (itemButtonItems[itemCurrentItem].action) {
    case INTERFACE_ITEM_ACTION_PRIMARY_AIMING:
        *aiming = true;
        // FALLTHROUGH
    case INTERFACE_ITEM_ACTION_PRIMARY:
        *hitMode = itemButtonItems[itemCurrentItem].primaryHitMode;
        return 0;
    case INTERFACE_ITEM_ACTION_SECONDARY_AIMING:
        *aiming = true;
        // FALLTHROUGH
    case INTERFACE_ITEM_ACTION_SECONDARY:
        *hitMode = itemButtonItems[itemCurrentItem].secondaryHitMode;
        return 0;
    }

    return -1;
}

// 0x454918
int intface_update_items(bool animated)
{
    if (interfaceWindow == -1) {
        return -1;
    }

    Object* oldCurrentItem = itemButtonItems[itemCurrentItem].item;

    InterfaceItemState* leftItemState = &(itemButtonItems[HAND_LEFT]);
    Object* item1 = inven_left_hand(obj_dude);
    if (item1 == leftItemState->item && leftItemState->item != NULL) {
        if (leftItemState->item != NULL) {
            leftItemState->isDisabled = item_grey(item1);
            leftItemState->itemFid = item_inv_fid(item1);
        }
    } else {
        leftItemState->item = item1;
        if (item1 != NULL) {
            leftItemState->isDisabled = item_grey(item1);
            leftItemState->primaryHitMode = HIT_MODE_LEFT_WEAPON_PRIMARY;
            leftItemState->secondaryHitMode = HIT_MODE_LEFT_WEAPON_SECONDARY;
            leftItemState->isWeapon = item_get_type(item1) == ITEM_TYPE_WEAPON;
            leftItemState->action = leftItemState->isWeapon != 0
                ? INTERFACE_ITEM_ACTION_PRIMARY
                : INTERFACE_ITEM_ACTION_USE;
            leftItemState->itemFid = item_inv_fid(item1);
        } else {
            leftItemState->isDisabled = 0;
            leftItemState->isWeapon = 1;
            leftItemState->primaryHitMode = HIT_MODE_PUNCH;
            leftItemState->secondaryHitMode = HIT_MODE_PUNCH;
            leftItemState->action = INTERFACE_ITEM_ACTION_PRIMARY;
            leftItemState->itemFid = -1;
        }
    }

    InterfaceItemState* rightItemState = &(itemButtonItems[HAND_RIGHT]);

    Object* item2 = inven_right_hand(obj_dude);
    if (item2 == rightItemState->item && rightItemState->item != NULL) {
        if (rightItemState->item != NULL) {
            rightItemState->isDisabled = item_grey(rightItemState->item);
            rightItemState->itemFid = item_inv_fid(rightItemState->item);
        }
    } else {
        rightItemState->item = item2;

        if (item2 != NULL) {
            rightItemState->isDisabled = item_grey(item2);
            rightItemState->primaryHitMode = HIT_MODE_RIGHT_WEAPON_PRIMARY;
            rightItemState->secondaryHitMode = HIT_MODE_RIGHT_WEAPON_SECONDARY;
            rightItemState->isWeapon = item_get_type(item2) == ITEM_TYPE_WEAPON;
            rightItemState->action = rightItemState->isWeapon != 0
                ? INTERFACE_ITEM_ACTION_PRIMARY
                : INTERFACE_ITEM_ACTION_USE;
            rightItemState->itemFid = item_inv_fid(item2);
        } else {
            rightItemState->isDisabled = 0;
            rightItemState->isWeapon = 1;
            rightItemState->primaryHitMode = HIT_MODE_PUNCH;
            rightItemState->secondaryHitMode = HIT_MODE_PUNCH;
            rightItemState->action = INTERFACE_ITEM_ACTION_PRIMARY;
            rightItemState->itemFid = -1;
        }
    }

    if (animated) {
        Object* newCurrentItem = itemButtonItems[itemCurrentItem].item;
        if (newCurrentItem != oldCurrentItem) {
            int animationCode = 0;
            if (newCurrentItem != NULL) {
                if (item_get_type(newCurrentItem) == ITEM_TYPE_WEAPON) {
                    animationCode = item_w_anim_code(newCurrentItem);
                }
            }

            intface_change_fid_animate((obj_dude->fid & 0xF000) >> 12, animationCode);

            return 0;
        }
    }

    intface_redraw_items();

    return 0;
}

// 0x454C28
int intface_toggle_items(bool animated)
{
    if (interfaceWindow == -1) {
        return -1;
    }

    itemCurrentItem = 1 - itemCurrentItem;

    if (animated) {
        Object* item = itemButtonItems[itemCurrentItem].item;
        int animationCode = 0;
        if (item != NULL) {
            if (item_get_type(item) == ITEM_TYPE_WEAPON) {
                animationCode = item_w_anim_code(item);
            }
        }

        intface_change_fid_animate((obj_dude->fid & 0xF000) >> 12, animationCode);
    } else {
        intface_redraw_items();
    }

    int mode = gmouse_3d_get_mode();
    if (mode == GAME_MOUSE_MODE_CROSSHAIR || mode == GAME_MOUSE_MODE_USE_CROSSHAIR) {
        gmouse_3d_set_mode(GAME_MOUSE_MODE_MOVE);
    }

    return 0;
}

// 0x454C28
// Is `action` a mode this weapon can actually be switched INTO?
//
// ⚠️ REALTIME DROPS THE RELOAD ENTRY: intface_get_attack() returns -1 for it, so combat_rt falls
// back to the primary attack and you simply keep firing. Reloading has its own binding (LB tap).
//
// ⚠️ THE AIMING ENTRIES ARE BACK IN REALTIME (2026-10-09). They were dropped on 2026-08-10 as a
// dead twin of the picker, because nothing read the flag. Now combat_rt does
// (rt_player_called_loc): with the slot on its bullseye, RT fires at the part bound in the LB
// called-shot window, at the aimed price the slot prints. His idea -- it is how the original shows
// and prices an aimed shot, and it freed RB for the heal.
static bool intface_item_action_available(InterfaceItemState* itemState, int action)
{
    bool realtime = combat_rt_enabled();

    switch (action) {
    case INTERFACE_ITEM_ACTION_PRIMARY:
        // Always reachable -- this is what makes the cycle below terminate.
        return true;
    case INTERFACE_ITEM_ACTION_PRIMARY_AIMING:
        return item_w_called_shot(obj_dude, itemState->primaryHitMode);
    case INTERFACE_ITEM_ACTION_SECONDARY:
        return itemState->secondaryHitMode != HIT_MODE_PUNCH
            && itemState->secondaryHitMode != HIT_MODE_KICK
            && item_w_subtype(itemState->item, itemState->secondaryHitMode) != ATTACK_TYPE_NONE;
    case INTERFACE_ITEM_ACTION_SECONDARY_AIMING:
        return itemState->secondaryHitMode != HIT_MODE_PUNCH
            && itemState->secondaryHitMode != HIT_MODE_KICK
            && item_w_subtype(itemState->item, itemState->secondaryHitMode) != ATTACK_TYPE_NONE
            && item_w_called_shot(obj_dude, itemState->secondaryHitMode);
    case INTERFACE_ITEM_ACTION_RELOAD:
        return !realtime && item_w_max_ammo(itemState->item) != item_w_curr_ammo(itemState->item);
    default:
        return false;
    }
}

// Would a toggle actually land on a DIFFERENT mode? Realtime drops the reload entry, so a weapon with
// one mode and no called shot (a flamer, Fast Shot) has nothing to switch to -- the caller can refuse audibly instead of
// playing a confirmation for a cycle that changes nothing.
bool intface_item_can_toggle_state()
{
    if (interfaceWindow == -1) {
        return false;
    }

    InterfaceItemState* itemState = &(itemButtonItems[itemCurrentItem]);
    if (itemState->isWeapon == 0) {
        return false;
    }

    for (int action = INTERFACE_ITEM_ACTION_USE; action < INTERFACE_ITEM_ACTION_COUNT; action++) {
        if (action != itemState->action && intface_item_action_available(itemState, action)) {
            return true;
        }
    }

    return false;
}

int intface_toggle_item_state()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    InterfaceItemState* itemState = &(itemButtonItems[itemCurrentItem]);

    int oldAction = itemState->action;
    if (itemState->isWeapon != 0) {
        bool done = false;
        while (!done) {
            itemState->action++;
            if (itemState->action == INTERFACE_ITEM_ACTION_COUNT) {
                // Wrap: ++ on the next pass takes USE to PRIMARY, which is always available, so
                // the loop can never spin forever however many entries are skipped.
                itemState->action = INTERFACE_ITEM_ACTION_USE;
                continue;
            }
            done = intface_item_action_available(itemState, itemState->action);
        }
    }

    if (oldAction != itemState->action) {
        intface_redraw_items();
    }

    return 0;
}

// CE: put the active hand on its AIMED mode -- what binding a part in the LB called-shot window
// does, so the next RT goes there (see rt_player_called_loc). True when the slot shows the
// bullseye afterwards. A mode that cannot be aimed (a burst, Fast Shot) is left alone.
bool intface_item_set_aiming()
{
    if (interfaceWindow == -1) {
        return false;
    }

    InterfaceItemState* itemState = &(itemButtonItems[itemCurrentItem]);
    if (itemState->isWeapon == 0) {
        return false;
    }

    int want;
    switch (itemState->action) {
    case INTERFACE_ITEM_ACTION_PRIMARY_AIMING:
    case INTERFACE_ITEM_ACTION_SECONDARY_AIMING:
        return true;
    case INTERFACE_ITEM_ACTION_PRIMARY:
        want = INTERFACE_ITEM_ACTION_PRIMARY_AIMING;
        break;
    case INTERFACE_ITEM_ACTION_SECONDARY:
        want = INTERFACE_ITEM_ACTION_SECONDARY_AIMING;
        break;
    default:
        return false;
    }

    if (!intface_item_action_available(itemState, want)) {
        return false;
    }

    itemState->action = want;
    intface_redraw_items();
    return true;
}

// 0x454D20
void intface_use_item()
{
    if (interfaceWindow == -1) {
        return;
    }

    InterfaceItemState* ptr = &(itemButtonItems[itemCurrentItem]);

    if (ptr->isWeapon != 0) {
        if (ptr->action == INTERFACE_ITEM_ACTION_RELOAD) {
            if (isInCombat()) {
                int hitMode = itemCurrentItem == HAND_LEFT
                    ? HIT_MODE_LEFT_WEAPON_RELOAD
                    : HIT_MODE_RIGHT_WEAPON_RELOAD;

                int actionPointsRequired = item_mp_cost(obj_dude, hitMode, false);
                if (actionPointsRequired <= obj_dude->data.critter.combat.ap) {
                    if (intface_item_reload() == 0) {
                        if (actionPointsRequired > obj_dude->data.critter.combat.ap) {
                            obj_dude->data.critter.combat.ap = 0;
                        } else {
                            obj_dude->data.critter.combat.ap -= actionPointsRequired;
                        }
                        intface_update_move_points(obj_dude->data.critter.combat.ap, combat_free_move);
                    }
                }
            } else {
                intface_item_reload();
            }
        } else {
            gmouse_set_cursor(MOUSE_CURSOR_CROSSHAIR);
            gmouse_3d_set_mode(GAME_MOUSE_MODE_CROSSHAIR);
            if (!isInCombat()) {
                combat(NULL);
            }
        }
    } else if (proto_action_can_use_on(ptr->item->pid)) {
        gmouse_set_cursor(MOUSE_CURSOR_USE_CROSSHAIR);
        gmouse_3d_set_mode(GAME_MOUSE_MODE_USE_CROSSHAIR);
    } else if (proto_action_can_use(ptr->item->pid)) {
        if (isInCombat()) {
            int actionPointsRequired = item_mp_cost(obj_dude, ptr->secondaryHitMode, false);
            // Realtime holds the engine combat.ap maxed, so charge combat_rt's pool
            // instead (using an item in a fight costs AP just like vanilla). Free out
            // of combat; refuses when the pool can't afford it.
            if (combat_rt_enabled()) {
                if (combat_rt_spend_ap(actionPointsRequired)) {
                    obj_use_item(obj_dude, ptr->item);
                    intface_update_items(false);
                }
            } else if (actionPointsRequired <= obj_dude->data.critter.combat.ap) {
                obj_use_item(obj_dude, ptr->item);
                intface_update_items(false);
                if (actionPointsRequired > obj_dude->data.critter.combat.ap) {
                    obj_dude->data.critter.combat.ap = 0;
                } else {
                    obj_dude->data.critter.combat.ap -= actionPointsRequired;
                }

                intface_update_move_points(obj_dude->data.critter.combat.ap, combat_free_move);
            }
        } else {
            obj_use_item(obj_dude, ptr->item);
            intface_update_items(false);
        }
    }
}

// 0x454F14
int intface_is_item_right_hand()
{
    return itemCurrentItem;
}

// 0x454F1C
int intface_get_current_item(Object** itemPtr)
{
    if (interfaceWindow == -1) {
        return -1;
    }

    // ⚠️ THE "NO ITEM" SENTINEL HERE IS (Object*)-1, NOT NULL. intface_init_items seeds both
    // hands with it (see the FIXME there -- it is vanilla's own oddity) and nothing in vanilla
    // ever tests for it, because in vanilla this getter is only ever reached in-game, where
    // intface_update_items has long since replaced the sentinel with a real item or a NULL.
    //
    // The gamepad layer broke that assumption: its mouse hook runs inside PRE-GAME modal loops
    // too, so pressing A on the credits screen reached here before any game had touched the
    // hands. The sentinel is not NULL, so a caller's NULL check waves it through -- and the
    // next dereference reads 0xFFFF...FFFF + offsetof(Object, pid) = 0x77. That was the credits
    // crash. (The title menu was safe only by accident: it runs under gpui_self_nav, which
    // returns out of gpui_action_buttons before this path.)
    //
    // Answer "no item" instead. It is the truth, and it fixes every caller at once -- ours and
    // vanilla's -- rather than bolting a second sentinel check onto each one.
    Object* item = itemButtonItems[itemCurrentItem].item;
    if (item == (Object*)-1) {
        return -1;
    }

    *itemPtr = item;

    return 0;
}

// 0x454F50
int intface_update_ammo_lights()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    InterfaceItemState* p = &(itemButtonItems[itemCurrentItem]);

    int ratio = 0;

    if (p->isWeapon != 0) {
        // calls sub_478674 twice, probably because if min/max kind macro
        int maximum = item_w_max_ammo(p->item);
        if (maximum > 0) {
            int current = item_w_curr_ammo(p->item);
            ratio = (int)((double)current / (double)maximum * 70.0);
        }
    } else {
        if (item_get_type(p->item) == ITEM_TYPE_MISC) {
            // calls sub_4793D0 twice, probably because if min/max kind macro
            int maximum = item_m_max_charges(p->item);
            if (maximum > 0) {
                int current = item_m_curr_charges(p->item);
                ratio = (int)((double)current / (double)maximum * 70.0);
            }
        }
    }

    intface_draw_ammo_lights(463, ratio);

    return 0;
}

// 0x455084
void intface_end_window_open(bool animated)
{
    if (interfaceWindow == -1) {
        return;
    }

    if (endWindowOpen) {
        return;
    }

    int fid = art_id(OBJ_TYPE_INTERFACE, 104, 0, 0, 0);
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art == NULL) {
        return;
    }

    int frameCount = art_frame_max_frame(art);
    gsound_play_sfx_file("iciboxx1");

    if (animated) {
        unsigned int delay = 1000 / art_frame_fps(art);
        int time = 0;
        int frame = 0;
        while (frame < frameCount) {
            sharedFpsLimiter.mark();

            if (elapsed_time(time) >= delay) {
                unsigned char* src = art_frame_data(art, frame, 0);
                if (src != NULL) {
                    buf_to_buf(src, 57, 58, 57, interfaceBuffer + 640 * 38 + 580, 640);
                    win_draw_rect(interfaceWindow, &endWindowRect);
                }

                time = get_time();
                frame++;
            }
            gmouse_bk_process();

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    } else {
        unsigned char* src = art_frame_data(art, frameCount - 1, 0);
        buf_to_buf(src, 57, 58, 57, interfaceBuffer + 640 * 38 + 580, 640);
        win_draw_rect(interfaceWindow, &endWindowRect);
    }

    art_ptr_unlock(handle);

    endWindowOpen = true;
    intface_create_end_turn_button();
    intface_create_end_combat_button();
    intface_end_buttons_disable();
}

// 0x4551D8
void intface_end_window_close(bool animated)
{
    if (interfaceWindow == -1) {
        return;
    }

    if (!endWindowOpen) {
        return;
    }

    int fid = art_id(OBJ_TYPE_INTERFACE, 104, 0, 0, 0);
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art == NULL) {
        return;
    }

    intface_destroy_end_turn_button();
    intface_destroy_end_combat_button();
    gsound_play_sfx_file("icibcxx1");

    if (animated) {
        unsigned int delay = 1000 / art_frame_fps(art);
        unsigned int time = 0;
        int frame = art_frame_max_frame(art);

        while (frame != 0) {
            sharedFpsLimiter.mark();

            if (elapsed_time(time) >= delay) {
                unsigned char* src = art_frame_data(art, frame - 1, 0);
                unsigned char* dest = interfaceBuffer + 640 * 38 + 580;
                if (src != NULL) {
                    buf_to_buf(src, 57, 58, 57, dest, 640);
                    win_draw_rect(interfaceWindow, &endWindowRect);
                }

                time = get_time();
                frame--;
            }
            gmouse_bk_process();

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    } else {
        unsigned char* dest = interfaceBuffer + 640 * 38 + 580;
        unsigned char* src = art_frame_data(art, 0, 0);
        buf_to_buf(src, 57, 58, 57, dest, 640);
        win_draw_rect(interfaceWindow, &endWindowRect);
    }

    art_ptr_unlock(handle);
    endWindowOpen = false;
}

// 0x45531C
void intface_end_buttons_enable()
{
    if (endWindowOpen) {
        win_enable_button(endTurnButton);
        win_enable_button(endCombatButton);

        // endltgrn.frm - green lights around end turn/combat window
        int lightsFid = art_id(OBJ_TYPE_INTERFACE, 109, 0, 0, 0);
        CacheEntry* lightsFrmHandle;
        unsigned char* lightsFrmData = art_ptr_lock_data(lightsFid, 0, 0, &lightsFrmHandle);
        if (lightsFrmData == NULL) {
            return;
        }

        gsound_play_sfx_file("icombat2");
        trans_buf_to_buf(lightsFrmData, 57, 58, 57, interfaceBuffer + 38 * 640 + 580, 640);
        win_draw_rect(interfaceWindow, &endWindowRect);

        art_ptr_unlock(lightsFrmHandle);
    }
}

// 0x4553B0
void intface_end_buttons_disable()
{
    if (endWindowOpen) {
        win_disable_button(endTurnButton);
        win_disable_button(endCombatButton);

        CacheEntry* lightsFrmHandle;
        // endltred.frm - red lights around end turn/combat window
        int lightsFid = art_id(OBJ_TYPE_INTERFACE, 110, 0, 0, 0);
        unsigned char* lightsFrmData = art_ptr_lock_data(lightsFid, 0, 0, &lightsFrmHandle);
        if (lightsFrmData == NULL) {
            return;
        }

        gsound_play_sfx_file("icombat1");
        trans_buf_to_buf(lightsFrmData, 57, 58, 57, interfaceBuffer + 38 * 640 + 580, 640);
        win_draw_rect(interfaceWindow, &endWindowRect);

        art_ptr_unlock(lightsFrmHandle);
    }
}

// 0x455444
static int intface_init_items()
{
    // FIXME: For unknown reason these values initialized with -1. It's never
    // checked for -1, so I have no explanation for this.
    itemButtonItems[HAND_LEFT].item = (Object*)-1;
    itemButtonItems[HAND_RIGHT].item = (Object*)-1;

    return 0;
}

// 0x455470
static int intface_redraw_items()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    win_enable_button(itemButton);

    InterfaceItemState* itemState = &(itemButtonItems[itemCurrentItem]);
    int actionPoints = -1;
    int aimPartBullseyeW = -1; // set when the bullseye is drawn: the slot is on its AIMED mode
    int aimPartApRight = 7 + 17 + 7 + 10; // right edge of the AP cost's digit (MVEPNT.FRM + one digit)
    int aimPartApH = 12;

    if (itemState->isDisabled == 0) {
        memcpy(itemButtonUp, itemButtonUpBlank, sizeof(itemButtonUp));
        memcpy(itemButtonDown, itemButtonDownBlank, sizeof(itemButtonDown));

        if (itemState->isWeapon == 0) {
            int fid;
            if (proto_action_can_use_on(itemState->item->pid)) {
                // USE ON
                fid = art_id(OBJ_TYPE_INTERFACE, 294, 0, 0, 0);
            } else if (proto_action_can_use(itemState->item->pid)) {
                // USE
                fid = art_id(OBJ_TYPE_INTERFACE, 292, 0, 0, 0);
            } else {
                fid = -1;
            }

            if (fid != -1) {
                CacheEntry* useTextFrmHandle;
                Art* useTextFrm = art_ptr_lock(fid, &useTextFrmHandle);
                if (useTextFrm != NULL) {
                    int width = art_frame_width(useTextFrm, 0, 0);
                    int height = art_frame_length(useTextFrm, 0, 0);
                    unsigned char* data = art_frame_data(useTextFrm, 0, 0);
                    trans_buf_to_buf(data, width, height, width, itemButtonUp + 188 * 7 + 181 - width, 188);
                    dark_trans_buf_to_buf(data, width, height, width, itemButtonDown, 181 - width + 1, 5, 188, 59641);
                    art_ptr_unlock(useTextFrmHandle);
                }

                actionPoints = item_mp_cost(obj_dude, itemState->primaryHitMode, false);
            }
        } else {
            int primaryFid = -1;
            int bullseyeFid = -1;
            int hitMode = -1;

            // NOTE: This value is decremented at 0x45FEAC, probably to build
            // jump table.
            switch (itemState->action) {
            case INTERFACE_ITEM_ACTION_PRIMARY_AIMING:
                bullseyeFid = art_id(OBJ_TYPE_INTERFACE, 288, 0, 0, 0);
                // FALLTHROUGH
            case INTERFACE_ITEM_ACTION_PRIMARY:
                hitMode = itemState->primaryHitMode;
                break;
            case INTERFACE_ITEM_ACTION_SECONDARY_AIMING:
                bullseyeFid = art_id(OBJ_TYPE_INTERFACE, 288, 0, 0, 0);
                // FALLTHROUGH
            case INTERFACE_ITEM_ACTION_SECONDARY:
                hitMode = itemState->secondaryHitMode;
                break;
            case INTERFACE_ITEM_ACTION_RELOAD:
                actionPoints = item_mp_cost(obj_dude, itemCurrentItem == HAND_LEFT ? HIT_MODE_LEFT_WEAPON_RELOAD : HIT_MODE_RIGHT_WEAPON_RELOAD, false);
                primaryFid = art_id(OBJ_TYPE_INTERFACE, 291, 0, 0, 0);
                break;
            }

            if (bullseyeFid != -1) {
                CacheEntry* bullseyeFrmHandle;
                Art* bullseyeFrm = art_ptr_lock(bullseyeFid, &bullseyeFrmHandle);
                if (bullseyeFrm != NULL) {
                    int width = art_frame_width(bullseyeFrm, 0, 0);
                    int height = art_frame_length(bullseyeFrm, 0, 0);
                    unsigned char* data = art_frame_data(bullseyeFrm, 0, 0);
                    trans_buf_to_buf(data, width, height, width, itemButtonUp + 188 * (60 - height) + (181 - width), 188);

                    int v9 = 60 - height - 2;
                    if (v9 < 0) {
                        v9 = 0;
                        height -= 2;
                    }

                    dark_trans_buf_to_buf(data, width, height, width, itemButtonDown, 181 - width + 1, v9, 188, 59641);
                    art_ptr_unlock(bullseyeFrmHandle);

                    aimPartBullseyeW = width; // the part name goes up against its left edge, below
                }
            }

            if (hitMode != -1) {
                actionPoints = item_w_mp_cost(obj_dude, hitMode, bullseyeFid != -1);

                int id;
                int anim = item_w_anim(obj_dude, hitMode);
                switch (anim) {
                case ANIM_THROW_PUNCH:
                case ANIM_KICK_LEG:
                    id = 287;
                    break;
                case ANIM_THROW_ANIM:
                    id = 117;
                    break;
                case ANIM_THRUST_ANIM:
                    id = 45;
                    break;
                case ANIM_SWING_ANIM:
                    id = 44;
                    break;
                case ANIM_FIRE_SINGLE:
                    id = 43;
                    break;
                case ANIM_FIRE_BURST:
                case ANIM_FIRE_CONTINUOUS:
                    id = 40;
                    break;
                }

                primaryFid = art_id(OBJ_TYPE_INTERFACE, id, 0, 0, 0);
            }

            if (primaryFid != -1) {
                CacheEntry* primaryFrmHandle;
                Art* primaryFrm = art_ptr_lock(primaryFid, &primaryFrmHandle);
                if (primaryFrm != NULL) {
                    int width = art_frame_width(primaryFrm, 0, 0);
                    int height = art_frame_length(primaryFrm, 0, 0);
                    unsigned char* data = art_frame_data(primaryFrm, 0, 0);
                    trans_buf_to_buf(data, width, height, width, itemButtonUp + 188 * 7 + 181 - width, 188);
                    dark_trans_buf_to_buf(data, width, height, width, itemButtonDown, 181 - width + 1, 5, 188, 59641);
                    art_ptr_unlock(primaryFrmHandle);
                }
            }
        }
    }

    if (actionPoints >= 0 && actionPoints < 10) {
        // movement point text
        int fid = art_id(OBJ_TYPE_INTERFACE, 289, 0, 0, 0);

        CacheEntry* handle;
        Art* art = art_ptr_lock(fid, &handle);
        if (art != NULL) {
            int width = art_frame_width(art, 0, 0);
            int height = art_frame_length(art, 0, 0);
            unsigned char* data = art_frame_data(art, 0, 0);

            trans_buf_to_buf(data, width, height, width, itemButtonUp + 188 * (60 - height) + 7, 188);
            aimPartApRight = 7 + width + 7 + 10; // "AP", the gap, one 10-px digit (see below)
            aimPartApH = height; // before the pressed frame trims it

            int v29 = 60 - height - 2;
            if (v29 < 0) {
                v29 = 0;
                height -= 2;
            }

            dark_trans_buf_to_buf(data, width, height, width, itemButtonDown, 7 + 1, v29, 188, 59641);
            art_ptr_unlock(handle);

            int offset = width + 7;

            // movement point numbers - ten numbers 0 to 9, each 10 pixels wide.
            fid = art_id(OBJ_TYPE_INTERFACE, 290, 0, 0, 0);
            art = art_ptr_lock(fid, &handle);
            if (art != NULL) {
                width = art_frame_width(art, 0, 0);
                height = art_frame_length(art, 0, 0);
                data = art_frame_data(art, 0, 0);

                trans_buf_to_buf(data + actionPoints * 10, 10, height, width, itemButtonUp + 188 * (60 - height) + 7 + offset, 188);

                int v40 = 60 - height - 2;
                if (v40 < 0) {
                    v40 = 0;
                    height -= 2;
                }
                dark_trans_buf_to_buf(data + actionPoints * 10, 10, height, width, itemButtonDown, offset + 7 + 1, v40, 188, 59641);

                art_ptr_unlock(handle);
            }
        }
    } else {
        memcpy(itemButtonUp, itemButtonDisabled, sizeof(itemButtonUp));
        memcpy(itemButtonDown, itemButtonDisabled, sizeof(itemButtonDown));
    }

    if (itemState->itemFid != -1) {
        CacheEntry* itemFrmHandle;
        Art* itemFrm = art_ptr_lock(itemState->itemFid, &itemFrmHandle);
        if (itemFrm != NULL) {
            int width = art_frame_width(itemFrm, 0, 0);
            int height = art_frame_length(itemFrm, 0, 0);
            unsigned char* data = art_frame_data(itemFrm, 0, 0);

            int v46 = (188 - width) / 2;
            int v47 = (67 - height) / 2 - 2;

            trans_buf_to_buf(data, width, height, width, itemButtonUp + 188 * ((67 - height) / 2) + v46, 188);

            if (v47 < 0) {
                v47 = 0;
                height -= 2;
            }

            dark_trans_buf_to_buf(data, width, height, width, itemButtonDown, v46 + 1, v47, 188, 63571);
            art_ptr_unlock(itemFrmHandle);
        }
    }

    // CE: realtime names the part an AIMED shot goes to, in small worn letters drawn at size
    // (intface_worn_text_compose), only while the slot shows its bullseye.
    // Turn-based has no bound part -- it asks every shot -- so nothing is printed there.
    //
    // ⚠️ RIGHT AFTER THE AP COST, centred on its height ("co to dát vedle číslice AP", 2026-10-10).
    // It went beside the bullseye first ("vlevo vedle toho terčíku", then "výš… na stejnou řádku"),
    // but that corner is exactly where a long gun's stock lies: rendered from the original art, a
    // sniper rifle covered both the bullseye and the label there. The bottom-left corner stayed clear
    // under every gun rendered (inventory art pistol, sniper, grifle, smgpstle, hrifle, shotgun6);
    // the pistol's grip just touches the last letter.
    //
    // ON TOP OF THE ITEM, no shadow (see the worn letters). The pressed frame is drawn the way every
    // word on the bar is: dark_trans_buf_to_buf at (+1, -2).
    if (aimPartBullseyeW >= 0 && combat_rt_enabled() && aim_slot_weapon_part != HIT_LOCATION_UNCALLED) {
        const char* partName = intface_aim_part_name(aim_slot_weapon_part);
        int labelW = intface_worn_text_width(partName);
        int labelX = aimPartApRight + 5;
        int labelY = (60 - aimPartApH) + (aimPartApH - WORN_H + 1) / 2; // centred; an odd half-pixel goes down
        bool fits = labelX + labelW <= 181 - aimPartBullseyeW - 3; // never under the bullseye
        if (labelW > 0 && labelW <= WORN_MAX_W && fits && labelY >= 2 && labelY + WORN_H <= 67) {
            unsigned char label[WORN_MAX_W * WORN_H];
            memset(label, 0, sizeof(label));
            if (intface_worn_text_compose(partName, label, labelW)) {
                trans_buf_to_buf(label, labelW, WORN_H, labelW, itemButtonUp + 188 * labelY + labelX, 188);
                dark_trans_buf_to_buf(label, labelW, WORN_H, labelW, itemButtonDown, labelX + 1, labelY - 2, 188, 59641);
            }
        }
    }

    if (!insideInit) {
        intface_update_ammo_lights();

        win_draw_rect(interfaceWindow, &itemButtonRect);

        if (itemState->isDisabled != 0) {
            win_disable_button(itemButton);
        } else {
            win_enable_button(itemButton);
        }
    }

    return 0;
}

// 0x455C60
static int intface_redraw_items_callback(Object* a1, Object* a2)
{
    intface_redraw_items();
    return 0;
}

// 0x455C68
static int intface_change_fid_callback(Object* a1, Object* a2)
{
    intface_fid_is_changing = false;
    return 0;
}

// SELF-HEALING WEAPON FID. The dude's sprite carries the weapon he is holding in bits 12-15 of his
// fid, and the ONLY thing that normally updates it is the swap animation registered below -- via a
// callback that fires at the end of that sequence.
//
// ⚠️ In realtime that sequence is not guaranteed to finish, or even to start. register_clear can
// refuse on a dude who is mid-swing, register_end can fail, and combat_rt cancels pending sequences
// on its own (anim_clear_pending) whenever it needs the dude's animation slot. Every one of those
// leaves the hand holding one weapon and the SPRITE holding the previous one, silently and
// permanently: "vystřelil jsem z pistole, ale přehrála se animace pušky", "vyměnil jsem nůž za
// sledgehammer a stále jsem v animacích držel nůž" -- both after swapping during a fight.
//
// So the fid is reconciled against the hand every frame instead of being trusted to one callback.
// This is a REPAIR path, not the normal one: it only acts when the dude is idle and no swap is in
// flight, and it corrects the fid outright rather than replaying a put-away/take-out, because by
// the time we get here the animation that should have done it is already gone.
void intface_reconcile_weapon_fid()
{
    if (interfaceWindow == -1 || obj_dude == NULL || intface_fid_is_changing) {
        return;
    }

    // Never fight a running animation -- that is how the fid got out of step in the first place.
    if (anim_busy(obj_dude) != 0) {
        return;
    }

    Object* item = itemButtonItems[itemCurrentItem].item;
    if (item == (Object*)-1) {
        return; // hands not initialised yet -- see intface_get_current_item
    }

    int wantCode = 0;
    if (item != NULL && item_get_type(item) == ITEM_TYPE_WEAPON) {
        wantCode = item_w_anim_code(item);
    }

    int haveCode = (obj_dude->fid & 0xF000) >> 12;
    if (haveCode == wantCode) {
        return;
    }

    // Keep the pose and facing the dude already has; only the weapon code changes.
    int fid = art_id(OBJ_TYPE_CRITTER,
        obj_dude->fid & 0xFFF,
        FID_ANIM_TYPE(obj_dude->fid),
        wantCode,
        obj_dude->rotation + 1);

    Rect rect;
    if (obj_change_fid(obj_dude, fid, &rect) == 0) {
        tile_refresh_rect(&rect, obj_dude->elevation);
    }
}

// 0x455C74
static void intface_change_fid_animate(int previousWeaponAnimationCode, int weaponAnimationCode)
{
    intface_fid_is_changing = true;

    register_clear(obj_dude);
    register_begin(ANIMATION_REQUEST_RESERVED);
    register_object_light(obj_dude, 4, 0);

    if (previousWeaponAnimationCode != 0) {
        const char* sfx = gsnd_build_character_sfx_name(obj_dude, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        register_object_play_sfx(obj_dude, sfx, 0);
        register_object_animate(obj_dude, ANIM_PUT_AWAY, 0);
    }

    register_object_must_call(NULL, NULL, (AnimationCallback*)intface_redraw_items_callback, -1);

    if (weaponAnimationCode != 0) {
        register_object_take_out(obj_dude, weaponAnimationCode, -1);
    } else {
        int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_STAND, 0, obj_dude->rotation + 1);
        register_object_change_fid(obj_dude, fid, -1);
    }

    register_object_must_call(NULL, NULL, (AnimationCallback*)intface_change_fid_callback, -1);

    if (register_end() == -1) {
        return;
    }

    // REALTIME: DO NOT BLOCK. The loop below is vanilla's -- it pumps get_input() and renderPresent()
    // until the put-away/take-out animation finishes, but it never runs the game update, so the
    // whole world stands still for the length of the swap ("při přepínání zbraní je svět zamrzlej").
    // Turn-based could afford that; realtime cannot -- enemies stop mid-stride and stop shooting
    // while you change guns.
    //
    // Returning here just leaves the animation registered: the main loop plays it, and the two
    // registered callbacks (intface_redraw_items_callback / intface_change_fid_callback) still fire
    // at their points in the sequence, the second one clearing intface_fid_is_changing. Nothing
    // reads that flag except this loop, so dropping the loop costs nothing.
    //
    // The interface/mouse disable and the wait cursor are skipped WITH the loop on purpose: they
    // exist to make the frozen frames safe to look at. Un-frozen there is nothing to protect
    // against, and combat_rt already refuses to fire while anim_busy(obj_dude) holds -- which it
    // does for the whole swap.
    if (combat_rt_enabled()) {
        return;
    }

    bool interfaceBarWasEnabled = intfaceEnabled;

    intface_disable();
    gmouse_disable(0);

    gmouse_set_cursor(MOUSE_CURSOR_WAIT_WATCH);

    while (intface_fid_is_changing) {
        sharedFpsLimiter.mark();

        if (game_user_wants_to_quit) {
            break;
        }

        get_input();

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    gmouse_set_cursor(MOUSE_CURSOR_NONE);

    gmouse_enable();

    if (interfaceBarWasEnabled) {
        intface_enable();
    }
}

// 0x455DA4
static int intface_create_end_turn_button()
{
    int fid;

    if (interfaceWindow == -1) {
        return -1;
    }

    if (!endWindowOpen) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 105, 0, 0, 0);
    endTurnButtonUp = art_ptr_lock_data(fid, 0, 0, &endTurnButtonUpKey);
    if (endTurnButtonUp == NULL) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 106, 0, 0, 0);
    endTurnButtonDown = art_ptr_lock_data(fid, 0, 0, &endTurnButtonDownKey);
    if (endTurnButtonDown == NULL) {
        return -1;
    }

    endTurnButton = win_register_button(interfaceWindow, 590, 43, 38, 22, -1, -1, -1, 32, endTurnButtonUp, endTurnButtonDown, NULL, 0);
    if (endTurnButton == -1) {
        return -1;
    }

    win_register_button_disable(endTurnButton, endTurnButtonUp, endTurnButtonUp, endTurnButtonUp);
    win_register_button_sound_func(endTurnButton, gsound_med_butt_press, gsound_med_butt_release);

    return 0;
}

// 0x455E88
static int intface_destroy_end_turn_button()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    if (endTurnButton != -1) {
        win_delete_button(endTurnButton);
        endTurnButton = -1;
    }

    if (endTurnButtonDown) {
        art_ptr_unlock(endTurnButtonDownKey);
        endTurnButtonDownKey = NULL;
        endTurnButtonDown = NULL;
    }

    if (endTurnButtonUp) {
        art_ptr_unlock(endTurnButtonUpKey);
        endTurnButtonUpKey = NULL;
        endTurnButtonUp = NULL;
    }

    return 0;
}

// 0x455F04
static int intface_create_end_combat_button()
{
    int fid;

    if (interfaceWindow == -1) {
        return -1;
    }

    if (!endWindowOpen) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 107, 0, 0, 0);
    endCombatButtonUp = art_ptr_lock_data(fid, 0, 0, &endCombatButtonUpKey);
    if (endCombatButtonUp == NULL) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 108, 0, 0, 0);
    endCombatButtonDown = art_ptr_lock_data(fid, 0, 0, &endCombatButtonDownKey);
    if (endCombatButtonDown == NULL) {
        return -1;
    }

    endCombatButton = win_register_button(interfaceWindow, 590, 65, 38, 22, -1, -1, -1, 13, endCombatButtonUp, endCombatButtonDown, NULL, 0);
    if (endCombatButton == -1) {
        return -1;
    }

    win_register_button_disable(endCombatButton, endCombatButtonUp, endCombatButtonUp, endCombatButtonUp);
    win_register_button_sound_func(endCombatButton, gsound_med_butt_press, gsound_med_butt_release);

    return 0;
}

// 0x455FE8
static int intface_destroy_end_combat_button()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    if (endCombatButton != -1) {
        win_delete_button(endCombatButton);
        endCombatButton = -1;
    }

    if (endCombatButtonDown != NULL) {
        art_ptr_unlock(endCombatButtonDownKey);
        endCombatButtonDownKey = NULL;
        endCombatButtonDown = NULL;
    }

    if (endCombatButtonUp != NULL) {
        art_ptr_unlock(endCombatButtonUpKey);
        endCombatButtonUpKey = NULL;
        endCombatButtonUp = NULL;
    }

    return 0;
}

// 0x456064
static void intface_draw_ammo_lights(int x, int ratio)
{
    // The ammo-light bar is 70 pixels tall, so `ratio` is a 0..70 fill level. If a caller passes
    // an out-of-range value -- e.g. a stale/garbage weapon in itemButtonItems yielding a bad
    // current/max ratio -- the fill loops below step `dest += 640` per row past the end (ratio<0
    // makes the first loop run >70 times; ratio>70 makes the second run long), writing off the
    // interface window buffer and corrupting the heap (an ACCESS_VIOLATION caught under page heap
    // during a gamepad weapon-swap mid-combat). Clamp defensively -- ratio is always meant to be
    // within [0, 70], so this can never alter a legitimate draw.
    if (ratio < 0) {
        ratio = 0;
    } else if (ratio > 70) {
        ratio = 70;
    }

    if ((ratio & 1) != 0) {
        ratio -= 1;
    }

    unsigned char* dest = interfaceBuffer + 640 * 26 + x;

    for (int index = 70; index > ratio; index--) {
        *dest = 14;
        dest += 640;
    }

    while (ratio > 0) {
        *dest = 196;
        dest += 640;

        *dest = 14;
        dest += 640;

        ratio -= 2;
    }

    if (!insideInit) {
        Rect rect;
        rect.ulx = x;
        rect.uly = 26;
        rect.lrx = x + 1;
        rect.lry = 26 + 70;
        win_draw_rect(interfaceWindow, &rect);
    }
}

// 0x4560E4
static int intface_item_reload()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    bool v0 = false;
    while (item_w_try_reload(obj_dude, itemButtonItems[itemCurrentItem].item) != -1) {
        v0 = true;
    }

    intface_toggle_item_state();
    intface_update_items(false);

    if (!v0) {
        return -1;
    }

    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, itemButtonItems[itemCurrentItem].item, HIT_MODE_RIGHT_WEAPON_PRIMARY, NULL);
    gsound_play_sfx_file(sfx);

    return 0;
}

// Public entry for the controller reload button: reloads the active-hand weapon
// from available inventory ammo (no-op if it isn't a reloadable weapon or there
// is no matching ammo). Returns 0 if it reloaded, -1 otherwise.
//
// Deliberately does NOT call intface_toggle_item_state() the way intface_item_reload
// does: that engine path is entered with the item's action already cycled onto
// RELOAD and uses the toggle to step back off it, but a direct reload would have
// the toggle just advance the firing mode (single/burst/aimed) as a side effect.
// Reloading must not change the selected attack mode.
// Would intface_reload_weapon do anything right now? The same three questions it answers by trying
// -- a weapon in the active hand that takes ammo, a magazine that is not full, and a box in the pack
// that fits -- asked WITHOUT loading anything. The reload animation needs the answer before it starts:
// it plays first and loads at the end, and an animation for a reload that then refuses is a lie.
bool intface_can_reload_weapon()
{
    if (interfaceWindow == -1 || obj_dude == NULL) {
        return false;
    }

    Object* weapon = itemButtonItems[itemCurrentItem].item;
    // ⚠️ The empty hand is (Object*)-1, not NULL (see intface_init_items), and a hand can hold a
    // non-weapon -- every item_w_* below reads the weapon union blindly.
    if (weapon == NULL || weapon == (Object*)-1 || item_get_type(weapon) != ITEM_TYPE_WEAPON) {
        return false;
    }

    int capacity = item_w_max_ammo(weapon);
    if (capacity <= 0 || item_w_curr_ammo(weapon) >= capacity) {
        return false;
    }

    int index = -1;
    Object* ammo;
    while ((ammo = inven_find_type(obj_dude, ITEM_TYPE_AMMO, &index)) != NULL) {
        if (item_w_can_reload(weapon, ammo)) {
            return true;
        }
    }

    return false;
}

// The object intface_reload_weapon would reload (the active hand's), or NULL when that hand is empty.
Object* intface_reload_target()
{
    if (interfaceWindow == -1) {
        return NULL;
    }

    Object* weapon = itemButtonItems[itemCurrentItem].item;
    return weapon == (Object*)-1 ? NULL : weapon;
}

int intface_reload_weapon()
{
    if (interfaceWindow == -1) {
        return -1;
    }

    bool reloaded = false;
    while (item_w_try_reload(obj_dude, itemButtonItems[itemCurrentItem].item) != -1) {
        reloaded = true;
    }

    intface_update_items(false);

    if (!reloaded) {
        return -1;
    }

    const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, itemButtonItems[itemCurrentItem].item, HIT_MODE_RIGHT_WEAPON_PRIMARY, NULL);
    gsound_play_sfx_file(sfx);

    return 0;
}

// 0x456160
static void intface_rotate_numbers(int x, int y, int previousValue, int value, int offset, int delay)
{
    if (value > 999) {
        value = 999;
    } else if (value < -999) {
        value = -999;
    }

    unsigned char* numbers = numbersBuffer + offset;
    unsigned char* dest = interfaceBuffer + 640 * y;

    unsigned char* downSrc = numbers + 90;
    unsigned char* upSrc = numbers + 99;
    unsigned char* minusSrc = numbers + 108;
    unsigned char* plusSrc = numbers + 114;

    unsigned char* signDest = dest + x;
    unsigned char* hundredsDest = dest + x + 6;
    unsigned char* tensDest = dest + x + 6 + 9;
    unsigned char* onesDest = dest + x + 6 + 9 * 2;

    int normalizedSign;
    int normalizedValue;
    if (insideInit || delay == 0) {
        normalizedSign = value >= 0 ? 1 : -1;
        normalizedValue = abs(value);
    } else {
        normalizedSign = previousValue >= 0 ? 1 : -1;
        normalizedValue = previousValue;
    }

    int ones = normalizedValue % 10;
    int tens = (normalizedValue / 10) % 10;
    int hundreds = normalizedValue / 100;

    buf_to_buf(numbers + 9 * hundreds, 9, 17, 360, hundredsDest, 640);
    buf_to_buf(numbers + 9 * tens, 9, 17, 360, tensDest, 640);
    buf_to_buf(numbers + 9 * ones, 9, 17, 360, onesDest, 640);
    buf_to_buf(normalizedSign >= 0 ? plusSrc : minusSrc, 6, 17, 360, signDest, 640);

    if (!insideInit) {
        Rect numbersRect = { x, y, x + 33, y + 17 };
        win_draw_rect(interfaceWindow, &numbersRect);
        if (delay != 0) {
            int change = value - previousValue >= 0 ? 1 : -1;
            int v14 = previousValue >= 0 ? 1 : -1;
            int v49 = change * v14;
            while (previousValue != value) {
                if ((hundreds | tens | ones) == 0) {
                    v49 = 1;
                }

                buf_to_buf(upSrc, 9, 17, 360, onesDest, 640);
                mouse_info();
                gmouse_bk_process();
                renderPresent();
                block_for_tocks(delay);
                win_draw_rect(interfaceWindow, &numbersRect);

                ones += v49;

                if (ones > 9 || ones < 0) {
                    buf_to_buf(upSrc, 9, 17, 360, tensDest, 640);
                    mouse_info();
                    gmouse_bk_process();
                    renderPresent();
                    block_for_tocks(delay);
                    win_draw_rect(interfaceWindow, &numbersRect);

                    tens += v49;
                    ones -= 10 * v49;
                    if (tens == 10 || tens == -1) {
                        buf_to_buf(upSrc, 9, 17, 360, hundredsDest, 640);
                        mouse_info();
                        gmouse_bk_process();
                        block_for_tocks(delay);
                        win_draw_rect(interfaceWindow, &numbersRect);

                        hundreds += v49;
                        tens -= 10 * v49;
                        if (hundreds == 10 || hundreds == -1) {
                            hundreds -= 10 * v49;
                        }

                        buf_to_buf(downSrc, 9, 17, 360, hundredsDest, 640);
                        mouse_info();
                        gmouse_bk_process();
                        renderPresent();
                        block_for_tocks(delay);
                        win_draw_rect(interfaceWindow, &numbersRect);
                    }

                    buf_to_buf(downSrc, 9, 17, 360, tensDest, 640);
                    block_for_tocks(delay);
                    win_draw_rect(interfaceWindow, &numbersRect);
                }

                buf_to_buf(downSrc, 9, 17, 360, onesDest, 640);
                mouse_info();
                gmouse_bk_process();
                renderPresent();
                block_for_tocks(delay);
                win_draw_rect(interfaceWindow, &numbersRect);

                previousValue += change;

                buf_to_buf(numbers + 9 * hundreds, 9, 17, 360, hundredsDest, 640);
                buf_to_buf(numbers + 9 * tens, 9, 17, 360, tensDest, 640);
                buf_to_buf(numbers + 9 * ones, 9, 17, 360, onesDest, 640);

                buf_to_buf(previousValue >= 0 ? plusSrc : minusSrc, 6, 17, 360, signDest, 640);
                mouse_info();
                gmouse_bk_process();
                renderPresent();
                block_for_tocks(delay);
                win_draw_rect(interfaceWindow, &numbersRect);
            }
        }
    }
}

// 0x4566E8
static int intface_fatal_error(int rc)
{
    intface_exit();

    return rc;
}

// 0x4566F4
static int construct_box_bar_win()
{
    int oldFont = text_curr();

    if (bar_window != -1) {
        return 0;
    }

    MessageList messageList;
    MessageListItem messageListItem;
    int rc = 0;
    if (!message_init(&messageList)) {
        rc = -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", msg_path, "intrface.msg");

    if (rc != -1) {
        if (!message_load(&messageList, path)) {
            rc = -1;
        }
    }

    if (rc == -1) {
        debug_printf("\nINTRFACE: Error indicator box messages! **\n");
        return -1;
    }

    CacheEntry* indicatorBoxFrmHandle;
    int width;
    int height;
    int indicatorBoxFid = art_id(OBJ_TYPE_INTERFACE, 126, 0, 0, 0);
    unsigned char* indicatorBoxFrmData = art_lock(indicatorBoxFid, &indicatorBoxFrmHandle, &width, &height);
    if (indicatorBoxFrmData == NULL) {
        debug_printf("\nINTRFACE: Error initializing indicator box graphics! **\n");
        message_exit(&messageList);
        return -1;
    }

    for (int index = 0; index < INDICATOR_COUNT; index++) {
        IndicatorDescription* indicatorDescription = &(bbox[index]);

        indicatorDescription->data = (unsigned char*)mem_malloc(INDICATOR_BOX_WIDTH * INDICATOR_BOX_HEIGHT);
        if (indicatorDescription->data == NULL) {
            debug_printf("\nINTRFACE: Error initializing indicator box graphics! **");

            while (--index >= 0) {
                mem_free(bbox[index].data);
            }

            message_exit(&messageList);
            art_ptr_unlock(indicatorBoxFrmHandle);

            return -1;
        }
    }

    text_font(101);

    for (int index = 0; index < INDICATOR_COUNT; index++) {
        IndicatorDescription* indicator = &(bbox[index]);

        char text[1024];
        strcpy(text, getmsg(&messageList, &messageListItem, indicator->title));

        int color = indicator->isBad ? colorTable[31744] : colorTable[992];

        memcpy(indicator->data, indicatorBoxFrmData, INDICATOR_BOX_WIDTH * INDICATOR_BOX_HEIGHT);

        // NOTE: For unknown reason it uses 24 as a height of the box to center
        // the title. One explanation is that these boxes were redesigned, but
        // this value was not changed. On the other hand 24 is
        // [INDICATOR_BOX_HEIGHT] + [INDICATOR_BOX_CONNECTOR_WIDTH]. Maybe just
        // a coincidence. I guess we'll never find out.
        int y = (24 - text_height()) / 2;
        int x = (INDICATOR_BOX_WIDTH - text_width(text)) / 2;
        text_to_buf(indicator->data + INDICATOR_BOX_WIDTH * y + x, text, INDICATOR_BOX_WIDTH, INDICATOR_BOX_WIDTH, color);
    }

    box_status_flag = true;
    refresh_box_bar_win();

    message_exit(&messageList);
    art_ptr_unlock(indicatorBoxFrmHandle);
    text_font(oldFont);

    return 0;
}

// 0x456A14
static void deconstruct_box_bar_win()
{
    if (bar_window != -1) {
        win_delete(bar_window);
        bar_window = -1;
    }

    for (int index = 0; index < INDICATOR_COUNT; index++) {
        IndicatorDescription* indicatorBoxDescription = &(bbox[index]);
        if (indicatorBoxDescription->data != NULL) {
            mem_free(indicatorBoxDescription->data);
            indicatorBoxDescription->data = NULL;
        }
    }
}

// 0x456A60
static void reset_box_bar_win()
{
    if (bar_window != -1) {
        win_delete(bar_window);
        bar_window = -1;
    }

    box_status_flag = true;
}

// Updates indicator bar.
//
// 0x45455C
int refresh_box_bar_win()
{
    if (interfaceWindow != -1 && box_status_flag && !intfaceHidden) {
        for (int index = 0; index < INDICATOR_SLOTS_COUNT; index++) {
            bboxslot[index] = -1;
        }

        int count = 0;

        if (is_pc_flag(PC_FLAG_SNEAKING)) {
            if (add_bar_box(INDICATOR_SNEAK)) {
                ++count;
            }
        }

        if (is_pc_flag(PC_FLAG_LEVEL_UP_AVAILABLE)) {
            if (add_bar_box(INDICATOR_LEVEL)) {
                ++count;
            }
        }

        if (is_pc_flag(PC_FLAG_ADDICTED)) {
            if (add_bar_box(INDICATOR_ADDICT)) {
                ++count;
            }
        }

        if (count > 1) {
            qsort(bboxslot, count, sizeof(*bboxslot), bbox_comp);
        }

        if (bar_window != -1) {
            win_delete(bar_window);
            bar_window = -1;
        }

        if (count != 0) {
            Rect interfaceWindowRect;
            win_get_rect(interfaceWindow, &interfaceWindowRect);

            bar_window = win_add(interfaceWindowRect.ulx,
                interfaceWindowRect.uly - INDICATOR_BOX_HEIGHT,
                (INDICATOR_BOX_WIDTH - INDICATOR_BOX_CONNECTOR_WIDTH) * count,
                INDICATOR_BOX_HEIGHT,
                colorTable[0],
                0);
            draw_bboxes(count);
            win_draw(bar_window);
        }

        return count;
    }

    if (bar_window != -1) {
        win_delete(bar_window);
        bar_window = -1;
    }

    return 0;
}

// 0x456B68
static int bbox_comp(const void* a, const void* b)
{
    int indicatorBox1 = *(int*)a;
    int indicatorBox2 = *(int*)b;

    if (indicatorBox1 == indicatorBox2) {
        return 0;
    } else if (indicatorBox1 < indicatorBox2) {
        return -1;
    } else {
        return 1;
    }
}

// Renders indicator boxes into the indicator bar window.
//
// 0x456B8C
static void draw_bboxes(int count)
{
    if (bar_window == -1) {
        return;
    }

    if (count == 0) {
        return;
    }

    int windowWidth = win_width(bar_window);
    unsigned char* windowBuffer = win_get_buf(bar_window);

    // The initial number of connections is 2 - one is first box to the screen
    // boundary, the other is female socket (initially empty). Every displayed
    // box adds one more connection (it is "plugged" into previous box and
    // exposes it's own empty female socket).
    int connections = 2;

    // The width of displayed indicator boxes as if there were no connections.
    int unconnectedIndicatorsWidth = 0;

    // The X offset to display next box.
    int x = 0;

    // The first box is connected to the screen boundary, so we have to clamp
    // male connectors on the left.
    int connectorWidthCompensation = INDICATOR_BOX_CONNECTOR_WIDTH;

    for (int index = 0; index < count; index++) {
        int indicator = bboxslot[index];
        IndicatorDescription* indicatorDescription = &(bbox[indicator]);

        trans_buf_to_buf(indicatorDescription->data + connectorWidthCompensation,
            INDICATOR_BOX_WIDTH - connectorWidthCompensation,
            INDICATOR_BOX_HEIGHT,
            INDICATOR_BOX_WIDTH,
            windowBuffer + x, windowWidth);

        connectorWidthCompensation = 0;

        unconnectedIndicatorsWidth += INDICATOR_BOX_WIDTH;
        x = unconnectedIndicatorsWidth - INDICATOR_BOX_CONNECTOR_WIDTH * connections;
        connections++;
    }
}

// Adds indicator to the indicator bar.
//
// Returns `true` if indicator was added, or `false` if there is no available
// space in the indicator bar.
//
// 0x456C34
static bool add_bar_box(int indicator)
{
    for (int index = 0; index < INDICATOR_SLOTS_COUNT; index++) {
        if (bboxslot[index] == -1) {
            bboxslot[index] = indicator;
            return true;
        }
    }

    debug_printf("\nINTRFACE: no free bar box slots!\n");

    return false;
}

// 0x456C84
bool enable_box_bar_win()
{
    bool oldIsVisible = box_status_flag;
    box_status_flag = true;

    refresh_box_bar_win();

    return oldIsVisible;
}

// 0x456CA4
bool disable_box_bar_win()
{
    bool oldIsVisible = box_status_flag;
    box_status_flag = false;

    refresh_box_bar_win();

    return oldIsVisible;
}

} // namespace fallout
