#include "game/pipboy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "game/automap.h"
#include "game/bmpdlog.h"
#include "game/combat.h"
#include "game/config.h"
#include "game/critter.h"
#include "game/cycle.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gmovie.h"
#include "game/gamepad_ui.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/map.h"
#include "game/object.h"
#include "game/queue.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/wordwrap.h"
#include "game/worldmap.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define PIPBOY_RAND_MAX 32767

#define PIPBOY_WINDOW_WIDTH 640
#define PIPBOY_WINDOW_HEIGHT 480

#define PIPBOY_WINDOW_DAY_X 20
#define PIPBOY_WINDOW_DAY_Y 17

#define PIPBOY_WINDOW_MONTH_X 46
#define PIPBOY_WINDOW_MONTH_Y 18

#define PIPBOY_WINDOW_YEAR_X 83
#define PIPBOY_WINDOW_YEAR_Y 17

#define PIPBOY_WINDOW_TIME_X 155
#define PIPBOY_WINDOW_TIME_Y 17

#define PIPBOY_WINDOW_NOTE_X 32
#define PIPBOY_WINDOW_NOTE_Y 83

#define PIPBOY_HOLODISK_LINES_MAX 35

#define PIPBOY_WINDOW_CONTENT_VIEW_X 254
#define PIPBOY_WINDOW_CONTENT_VIEW_Y 46
#define PIPBOY_WINDOW_CONTENT_VIEW_WIDTH 374
#define PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT 410

#define PIPBOY_IDLE_TIMEOUT 120000

#define PIPBOY_BOMB_COUNT 16

#define BACK_BUTTON_INDEX 20

#define QUEST_LOCATION_COUNT 12
#define QUEST_PER_LOCATION_COUNT 9
#define HOLODISK_COUNT 18

typedef enum Holiday {
    HOLIDAY_NEW_YEAR,
    HOLIDAY_VALENTINES_DAY,
    HOLIDAY_FOOLS_DAY,
    HOLIDAY_SHIPPING_DAY,
    HOLIDAY_INDEPENDENCE_DAY,
    HOLIDAY_HALLOWEEN,
    HOLIDAY_THANKSGIVING_DAY,
    HOLIDAY_CRISTMAS,
    HOLIDAY_COUNT,
} Holiday;

// Options used to render Pipboy texts.
typedef enum PipboyTextOptions {
    // Specifies that text should be rendered in the center of the Pipboy
    // monitor.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_CENTER = 0x02,

    // Specifies that text should be rendered in the beginning of the right
    // column in two-column layout.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN = 0x04,

    // Specifies that text should be rendered in the center of the left column.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER = 0x10,

    // Specifies that text should be rendered in the center of the right
    // column.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER = 0x20,

    // Specifies that text should rendered with underline.
    PIPBOY_TEXT_STYLE_UNDERLINE = 0x08,

    // Specifies that text should rendered with strike-through line.
    PIPBOY_TEXT_STYLE_STRIKE_THROUGH = 0x40,

    // Specifies that text should be rendered with no (minimal) indentation.
    PIPBOY_TEXT_NO_INDENT = 0x80,
} PipboyTextOptions;

typedef enum PipboyRestDuration {
    PIPBOY_REST_DURATION_TEN_MINUTES,
    PIPBOY_REST_DURATION_THIRTY_MINUTES,
    PIPBOY_REST_DURATION_ONE_HOUR,
    PIPBOY_REST_DURATION_TWO_HOURS,
    PIPBOY_REST_DURATION_THREE_HOURS,
    PIPBOY_REST_DURATION_FOUR_HOURS,
    PIPBOY_REST_DURATION_FIVE_HOURS,
    PIPBOY_REST_DURATION_SIX_HOURS,
    PIPBOY_REST_DURATION_UNTIL_MORNING,
    PIPBOY_REST_DURATION_UNTIL_NOON,
    PIPBOY_REST_DURATION_UNTIL_EVENING,
    PIPBOY_REST_DURATION_UNTIL_MIDNIGHT,
    PIPBOY_REST_DURATION_UNTIL_HEALED,
    PIPBOY_REST_DURATION_UNTIL_PARTY_HEALED,
    PIPBOY_REST_DURATION_COUNT,
    PIPBOY_REST_DURATION_COUNT_WITHOUT_PARTY = PIPBOY_REST_DURATION_COUNT - 1,
} PipboyRestDuration;

typedef enum PipboyFrm {
    PIPBOY_FRM_LITTLE_RED_BUTTON_UP,
    PIPBOY_FRM_LITTLE_RED_BUTTON_DOWN,
    PIPBOY_FRM_NUMBERS,
    PIPBOY_FRM_BACKGROUND,
    PIPBOY_FRM_NOTE,
    PIPBOY_FRM_MONTHS,
    PIPBOY_FRM_NOTE_NUMBERS,
    PIPBOY_FRM_ALARM_DOWN,
    PIPBOY_FRM_ALARM_UP,
    PIPBOY_FRM_LOGO,
    PIPBOY_FRM_BOMB,
    PIPBOY_FRM_COUNT,
} PipboyFrm;

typedef struct HolidayDescription {
    short month;
    short day;
    short textId;
} HolidayDescription;

typedef struct PipboySortableEntry {
    char* name;
    short value;
    short field_6;
} PipboySortableEntry;

typedef struct PipboyBomb {
    int x;
    int y;
    float field_8;
    float field_C;
    unsigned char field_10;
} PipboyBomb;

// One row of the CONTROLS page in the archives -- see ctrl_lines.
typedef struct PipboyControl {
    const char* button;
    const char* action;
} PipboyControl;

static int StartPipboy(int intent);
static void EndPipboy();
static void pip_days_left(int days);
static void pip_num(int value, int digits, int x, int y);
static void pip_date();
static void pip_print(const char* text, int a2, int a3);
static void pip_back(int a1);
static void PipStatus(int a1);
static void ListStatLines(int a1);
static void ShowHoloDisk();
static int ListHoloDiskTitles(int a1);
static int qscmp(const void* a1, const void* a2);
static void PipAutomaps(int a1);
static int PrintAMelevList(int a1);
static int PrintAMList(int a1);
static void PipArchives(int a1);
static int ListArchive(int a1);
static void ShowControls();
static void ShowControls2();
static void ShowControls3();
static void ShowControls4();
static void ShowControlsPage(const char* title, const PipboyControl* lines, int count);
static void pip_ctrl_line(const char* button, const char* action, int color);
static void PipAlarm(int a1);
static void DrawAlarmText(int a1);
static void DrawAlrmHitPnts();
static void NewFuncDsply();
static void AddHotLines(int start, int count, bool add_back_button);
static void NixHotLines();
static bool TimedRest(int hours, int minutes, int kind);
static bool Check4Health(int a1);
static bool AddHealth();
static void ClacTime(int* hours, int* minutes, int wakeUpHour);
static int ScreenSaver();
static void pip_note();

// 0x486A20
static const Rect pip_rect = {
    PIPBOY_WINDOW_CONTENT_VIEW_X,
    PIPBOY_WINDOW_CONTENT_VIEW_Y,
    PIPBOY_WINDOW_CONTENT_VIEW_X + PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
    PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
};

// 0x486A30
static const int pipgrphs[PIPBOY_FRM_COUNT] = {
    8,
    9,
    82,
    127,
    128,
    129,
    130,
    131,
    132,
    133,
    226,
};

// 0x486A5C
static const short holodisks[HOLODISK_COUNT] = {
    GVAR_FEV_DISK,
    GVAR_SECURITY_DISK,
    GVAR_ARTIFACT_DISK,
    GVAR_ALPHA_DISK,
    GVAR_DELTA_DISK,
    GVAR_VREE_DISK,
    GVAR_HONOR_DISK,
    GVAR_MUTANT_DISK,
    GVAR_BROTHER_HISTORY,
    GVAR_SOPHIA_DISK,
    GVAR_MAXSON_DISK,
    GVAR_MASTER_FILLER_7,
    GVAR_MASTER_FILLER_8,
    GVAR_WATER_CHIP_1,
    GVAR_WATER_CHIP_2,
    GVAR_WATER_CHIP_3,
    GVAR_MASTER_FILLER_10,
    GVAR_DESTROY_MASTER_7,
};

// 0x507224
static bool bk_enable = false;

// NOTE: Quest location indexes match town, I'm not sure if that was intentional
// or just a coincedence.
//
// 0x507228
static short sthreads[QUEST_LOCATION_COUNT][QUEST_PER_LOCATION_COUNT] = {
    // Vault 13
    {
        GVAR_CALM_REBELS,
        GVAR_DESTROY_MASTER_5,
        GVAR_DESTROY_MASTER_4,
        GVAR_FIND_WATER_CHIP,
        GVAR_WATER_THIEF,
        0,
        0,
        0,
        0,
    },
    // Buried Vault
    {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Shady Sands
    {
        GVAR_CURE_JARVIS,
        GVAR_MAKE_ANTIDOTE,
        GVAR_RESCUE_TANDI,
        GVAR_RADSCORPION_SEED,
        0,
        0,
        0,
        0,
        0,
    },
    // Junktown
    {
        GVAR_SAUL_QUEST,
        GVAR_KILL_KILLIAN,
        GVAR_SAVE_SINTHIA,
        GVAR_TRISH_QUEST,
        GVAR_CAPTURE_GIZMO,
        GVAR_BUST_SKULZ,
        0,
        0,
        0,
    },
    // Raiders
    {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Necropolis
    {
        GVAR_NECROP_MUTANTS_KILLED,
        GVAR_NECROP_WATER_PUMP_FIXED,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Hub
    {
        GVAR_KILL_DEATHCLAW,
        GVAR_KILL_JAIN,
        GVAR_KILL_MERCHANT,
        GVAR_MISSING_CARAVAN,
        GVAR_STEAL_NECKLACE,
        0,
        0,
        0,
        0,
    },
    // Brotherhood
    {
        GVAR_BECOME_AN_INITIATE,
        GVAR_FIND_LOST_INITIATE,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Military Base
    {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Glow
    {
        GVAR_WEAPONS_ARMED,
        GVAR_START_POWER,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
    // Boneyard
    {
        GVAR_BECOME_BLADE,
        GVAR_ROMEO_JULIET,
        GVAR_DESTROY_FOLLOWERS,
        GVAR_FIND_AGENT,
        GVAR_FIX_FARM,
        GVAR_LOST_BROTHER,
        GVAR_GANG_WAR,
        0,
        0,
    },
    // Cathedral
    {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    },
};

// 0x507300
static HolidayDescription SpclDate[HOLIDAY_COUNT] = {
    { 1, 1, 100 },
    { 2, 14, 101 },
    { 4, 1, 102 },
    { 7, 4, 104 },
    { 10, 6, 103 },
    { 10, 31, 105 },
    { 11, 28, 106 },
    { 12, 25, 107 },
};

// 0x507330
static PipboyRenderProc* PipFnctn[5] = {
    PipStatus,
    PipAutomaps,
    PipArchives,
    PipAlarm,
    PipAlarm,
};

// The gamepad layout, listed on its own pages of the Pip-Boy archives.
//
// Hardcoded rather than added to pipboy.msg: that file lives inside MASTER.DAT, and adding a
// handful of message ids would mean shipping a loose override of the WHOLE file, which would
// then fight every localised or patched pipboy.msg the player might install.
//
//   button == NULL, action != NULL -> section heading (underlined)
//   both NULL                      -> blank spacer, dropped first if the page runs long
//
// ⚠️ EACH PAGE HAS A HARD 40-LINE BUDGET, and it is not a style preference -- pip_ctrl_line piles
// every row past `bottom_line` onto the bottom line, so an overlong table does not scroll, it
// smears. The budget is 1 (title) + as many entries as fit; blank spacers are dropped wholesale
// first (`spacious` in ShowControlsPage), which buys back one line per section boundary. That is
// why there are THREE pages: the world/combat table alone spends its whole 40.
static const PipboyControl ctrl_lines[] = {
    { NULL, "MOVEMENT" },
    { "Left stick", "Walk" },
    { "X (hold)", "Run" },
    { "Right stick", "Cursor, pick a target" },
    { "LT (hold)", "Aim: lock the nearest enemy" },
    { NULL, NULL },
    { NULL, "FIGHTING" },
    { "RT", "Attack (hold to keep attacking)" },
    // ⚠️ 2026-10-09: the aimed shot is the weapon slot's AIMED mode (bullseye + its AP price), RT
    // fires it; RB is the heal, LT + Y the firing mode. LB + B still heals but is not listed -- one row, one gesture.
    { "R3", "Called shot: pick a body part" },
    { "In that window", "A or R3: slot goes AIMED there" },
    { "LB", "Reload" },
    { "LB (hold)", "Quick slot: draw the grenade, let go = throw" },
    { "RB", "Quick slot: stimpack (a grenade: hold, let go)" },
    { "LT + Y", "Firing mode: single / aimed / burst" },
    { NULL, NULL },
    { NULL, "ACTION" },
    { "A", "Use what is lit up / confirm" },
    { "A twice", "Loot the body you stand on" },
    { "A (hold)", "Aim held item at something" },
    { "B", "Cancel / stop what you ordered" },
    { "Y", "Swap active hand" },
    { NULL, NULL },
    // ⚠️ THE STICK EACH WHEEL READS IS PART OF ITS ENTRY. A wheel is aimed with the thumb that is
    // NOT holding it open, so the face-button wheels (Y, B) read the LEFT stick and L3 reads the
    // RIGHT one. Naming the wrong stick is worse than naming none: it sends the player to the walk
    // stick, which is the input the wheels arm against.
    //
    // ⚠️ AMMUNITION LOST ITS OWN ROW (2026-09-17) because it lost its own button -- RB had to be
    // freed to become a held fire button. Since 2026-09-20 it is not a page you have to go looking
    // for either: bare hands, Unload and every kind of round the gun takes are the FIRST segments
    // of the weapon ring, so the entry says where they are rather than how to page to them.
    { NULL, "WHEELS - hold the button (L3: press), aim, release to pick" },
    // Since 2026-10-04 the drugs are this ring's LAST page too, and the paging wraps -- so from the
    // first page they are a single LB away. That is the whole reason the entry spells LB out.
    { "Y", "Weapons + ammo; LB = drugs, items, armour" },
    { "B", "Drugs and quick items - aim LEFT" },
    { "L3 (press)", "Skills: stays open, RIGHT stick, let go or A picks" },
    { "A", "Take what is highlighted" },
    { NULL, NULL },
    { NULL, "D-PAD - HOLD each of these; a TAP cycles the action" },
    { "Up", "Pip-Boy" },
    { "Down", "Character sheet" },
    { "Left", "Automap" },
    { "Right", "End combat (refused if enemies still want you)" },
    { "Tap", "Next action (on a follower: push past)" },
    { "Tap Up / Down", "Nothing lit up: scroll the messages" },
    { NULL, NULL },
    { NULL, "OTHER" },
    { "BACK", "Inventory" },
    { "BACK (hold)", "Run to the nearest way out" },
    { "START", "Pause menu" },
    { "START (hold)", "Quick save" },
};

// SECOND CONTROLS PAGE: the screens you act IN -- talking, the character sheet, inventory, barter.
//
// ⚠️ A CONTROL THAT IS NOT WRITTEN DOWN DOES NOT EXIST. Barter's "even out with caps" and the
// split-stack picker sat unused for weeks for exactly this reason, and the end-combat hold was
// pressed once and never found again. The inventory's A -- a tap uses or equips, a hold opens the
// whole Use / Drop / Look menu -- was the last big one still missing.
static const PipboyControl ctrl2_lines[] = {
    { NULL, "TALKING" },
    { "Stick / D-pad", "Pick a reply" },
    { "A", "Say it" },
    { "X", "Ask me about" },
    { "Y", "Barter" },
    { "B", "Leave" },
    { NULL, NULL },
    { NULL, "CHARACTER SHEET" },
    { "Stick / D-pad", "Move the cursor" },
    // Not "column": the shoulders hop to the next GROUP of rows (stats, traits, skills...), which
    // is what ce_focus_jump actually walks.
    { "LB / RB", "Jump to the next section" },
    { "A", "Spend a point" },
    { "START", "Done (A held works too)" },
    { "X", "Take a point back" },
    { "Y", "Spend them all" },
    { "B", "Cancel" },
    // The name box (2026-10-11): it took only Enter until a Deck player got stuck in it.
    { "Name box", "Steam+X types it, A confirms" },
    { "Age / sex box", "Left / Right changes, A confirms" },
    { NULL, NULL },
    { NULL, "INVENTORY AND LOOT" },
    { "A", "Use / equip to A (loot: take one)" },
    { "A on worn armour", "Take it off" },
    { "A (hold)", "Use / Drop / Look menu" },
    { "X", "Put it in the left hand" },
    { "B", "Right hand (loot: close)" },
    { "Y", "Take the whole stack (loot)" },
    { "LB / RB", "Put the item in that quick slot (again: auto)" },
    { "BACK", "Close" },
    { NULL, NULL },
    { NULL, "BARTER" },
    { "LB / RB", "Step between columns" },
    { "D-pad", "Move between items" },
    { "A", "Move one of them" },
    { "A (hold)", "Look at the item" },
    { "LT + A", "Move the whole stack" },
    { "RT", "Choose how many" },
    { "X", "Even out the deal with caps" },
    { "Y", "Offer the trade" },
    { "B", "Leave" },
};

// THIRD CONTROLS PAGE: the screens you only LOOK at or pick from. Split off the screens page
// because that one had filled up, and for the same reason the second page exists at all -- a page
// that overflows does not scroll, it stacks every extra row onto the bottom line.
static const PipboyControl ctrl3_lines[] = {
    { NULL, "PIP-BOY AND AUTOMAP" },
    { "Stick", "Move the cursor" },
    { "A", "Press what it is on" },
    { "B or BACK", "Close" },
    { "Up / Left", "Closes the one it opened" },
    { NULL, NULL },
    { NULL, "WORLD MAP" },
    { "Up / Down", "Pick where to go" },
    { "A", "Travel, or enter this place" },
    { NULL, NULL },
    { NULL, "TOWN MAP" },
    { "Stick / D-pad", "Pick a way in" },
    { "A", "Go in there" },
    { "B", "Back to the world map" },
    { NULL, NULL },
    { NULL, "SAVE AND LOAD" },
    { "Stick / D-pad", "Pick a slot" },
    { "A", "Confirm" },
    { "B", "Cancel" },
    { NULL, NULL },
    { NULL, "PREFERENCES" },
    { "X or M", "MODS - this build's own settings" },
    { "D-pad", "Move between settings" },
    { "LB / RB", "Change the value" },
    { "A", "Press the button" },
    { "Y", "Done" },
    { "B", "Cancel" },
    // 2026-10-16: PLACE on the timer window, and an armed explosive picked again on a wheel goes on
    // the ground (gpui_place_explosive). Eight rows tip this page into its tight layout (37 of 40).
    { NULL, NULL },
    { NULL, "SET TIMER (dynamite, plastic explosive)" },
    { "D-pad", "Longer / shorter" },
    { "Y", "Longest" },
    { "A", "Set it" },
    { "X", "Set it and put it down" },
    { "B", "Cancel" },
    { "Armed, on a wheel", "Pick it again: put it down" },
    { NULL, NULL },
    { NULL, "ANYWHERE ELSE" },
    { "Stick / D-pad", "Move the highlight" },
    { "A", "Pick it, or yes in a prompt" },
    { "B", "Back out, or no in a prompt" },
    { "RT", "Click wherever the cursor is" },
    { "A / B / START", "Skip the death scene" },
};

// FOURTH CONTROLS PAGE: the keyboard (2026-10-10). The keyboard is a controller -- his design,
// *"chůze šipkami, akce mezerníkem, ctrl útok, alt běh ... prostě myš vůbec"* -- so this page is the
// world page again with a key where each pad button was (gamepad_ui_handle_key, combat_rt_poll_gamepad).
// The vanilla keys that still work are listed too, because a keyboard player has no other way to find
// them, and so is the optional mouse aim.
static const PipboyControl ctrl4_lines[] = {
    { NULL, "MOVE AND FIGHT" },
    { "Arrows", "Walk" },
    { "Alt (hold)", "Run" },
    { "Ctrl (hold)", "Attack (keeps attacking)" },
    { "Shift (hold)", "Aim: lock the enemy in front" },
    { "Q / E", "Lock the next enemy left / right" },
    { "F", "Firing mode: single / aimed / burst" },
    { "V", "Called shot: pick a body part" },
    { "R", "Reload" },
    { "T", "Quick slot LB: throw the grenade" },
    { "H", "Quick slot RB: stimpack" },
    { "B", "Swap active hand" },
    { NULL, NULL },
    { NULL, "AROUND YOU" },
    { "Space", "Use what is lit up" },
    { "G", "Run to the nearest way out" },
    { "S", "Skilldex" },
    { "1 - 8", "Skills, as in the original" },
    { "Mouse right", "Switch the cursor: hex (click = walk) / arrow" },
    { "Mouse left (hold)", "Arrow: walk to the cursor, far = run; enemy: attack" },
    { NULL, NULL },
    { NULL, "SCREENS" },
    { "I", "Inventory" },
    { "C", "Character sheet" },
    { "P", "Pip-Boy" },
    { "Tab", "Automap" },
    { "Esc", "Pause menu" },
    { "Enter", "End combat" },
    { "F6 / F7", "Quick save / quick load" },
};

// Titles of the CONTROLS pages, and the archive entries that open them -- one string each so the
// list and the page it leads to can never drift apart.
static const char* ctrl_entry = "CONTROLS - WORLD";
static const char* ctrl2_entry = "CONTROLS - SCREENS";
static const char* ctrl3_entry = "CONTROLS - MENUS";
static const char* ctrl4_entry = "CONTROLS - KEYBOARD";

// 0x662A70
static Size ginfo[PIPBOY_FRM_COUNT];

// 0x662B90
static MessageListItem pipmesg;

// pipboy.msg
//
// 0x662B88
static MessageList pipboy_message_file;

// 0x662AC8
static PipboySortableEntry sortlist[24];

// 0x662BF8
static int statcount;

// 0x662BFC
static unsigned char* scrn_buf;

// 0x662B9C
static unsigned char* pipbmp[PIPBOY_FRM_COUNT];

// 0x662C0C
static int holocount;

// 0x662C18
static int mouse_y;

// 0x662C1C
static int mouse_x;

// 0x662BC8
static unsigned int wait_time;

// Index of the last page when rendering holodisk content.
//
// 0x662C28
static int holopages;

// 0x662C2C
static int HotLines[21];

// 0x662C80
static int old_mouse_x;

// 0x662C84
static int old_mouse_y;

// 0x662C88
static int pip_win;

// 0x662BCC
static CacheEntry* grphkey[PIPBOY_FRM_COUNT];

// 0x662C8C
static int holodisk;

// 0x662C00
static int hot_line_count;

// 0x662C04
static int savefont;

// 0x662C08
static bool proc_bail_flag;

// 0x662C10
static int amlst_mode;

// 0x662C14
static int crnt_func;

// 0x662C20
static int actcnt;

// 0x662C24
static int hot_line_start;

// 0x662C90
static int cursor_line;

// 0x662C94
static int rest_time;

// 0x662C98
static int amcty_indx;

// 0x662C9C
static int view_page;

// 0x662CA0
static int bottom_line;

// 0x662CA4
static unsigned char hot_back_line;

// 0x662CA5
static unsigned char holo_flag;

// 0x662CA6
static unsigned char stat_flag;

// Set while the CONTROLS page (the pad layout, reachable from the archives) is showing, so
// PipArchives routes BACK to the list instead of treating hot-line codes as movies. Same
// shape as holo_flag above, which does the job for the holodisk reader.
static unsigned char ctrl_flag;

// x of the CONTROLS page's second column, measured from the widest button label in
// ShowControls -- see pip_ctrl_line.
static int ctrl_col;

// GAMEPAD NAVIGATION FOR THE PIP-BOY.
//
// ⚠️ IT HAD NONE. Every page here is a list of HOT LINES -- registered buttons 350 px wide that
// report 505 + index when clicked -- and the only way a pad ever reached one was to leave the world
// cursor lying on top of it. Reading a holodisk or a quest log therefore meant nudging a cursor
// around a screen that shows no cursor ("chci si přečíst logs, blbě se to ovládá").
//
// The shape is the one he asked for and the one the inventory already uses: start on the FIRST
// entry, D-pad walks down them one at a time, A opens the one you are on. The selection is shown by
// PARKING THE CURSOR on it -- the Pip-Boy repaints its content area wholesale on every page change
// and keeps no clean copy to erase a highlight against, so a drawn marker would have to be undone
// by hand against a background that no longer exists.
static int pip_pad_sel = 0;

// SELECTION FRAME. ⚠️ The cursor alone does not do this job -- the elevator proved it the same
// evening ("ted se při výběru patra neukazuje kurzor"), and Preferences proved it before that: a
// pointer is not a statement about which entry you are on, and on these screens it is not reliably
// visible either. So the selected hot line gets a frame drawn round it.
//
// ⚠️ SAVE-UNDER, the same as Preferences and the elevator: the page under it was painted by
// whichever PipFnctn drew it and there is no clean copy to erase against. Keep the four 1px strips
// and put them back before anything else draws -- which means the loop is strictly
// ERASE -> input (any of which may repaint the whole content area) -> DRAW.
#define PIP_HL_MAX_W 380
#define PIP_HL_MAX_H 32

static unsigned char pip_hl_top[PIP_HL_MAX_W];
static unsigned char pip_hl_bottom[PIP_HL_MAX_W];
static unsigned char pip_hl_left[PIP_HL_MAX_H];
static unsigned char pip_hl_right[PIP_HL_MAX_H];
static int pip_hl_x = -1;
static int pip_hl_y;
static int pip_hl_w;
static int pip_hl_h;

static void pip_hl_erase()
{
    if (pip_hl_x < 0) {
        return;
    }

    memcpy(scrn_buf + PIPBOY_WINDOW_WIDTH * pip_hl_y + pip_hl_x, pip_hl_top, pip_hl_w);
    memcpy(scrn_buf + PIPBOY_WINDOW_WIDTH * (pip_hl_y + pip_hl_h - 1) + pip_hl_x, pip_hl_bottom, pip_hl_w);
    for (int i = 0; i < pip_hl_h; i++) {
        unsigned char* row = scrn_buf + PIPBOY_WINDOW_WIDTH * (pip_hl_y + i);
        row[pip_hl_x] = pip_hl_left[i];
        row[pip_hl_x + pip_hl_w - 1] = pip_hl_right[i];
    }

    pip_hl_x = -1;
}

static void pip_hl_draw(int k)
{
    if (hot_line_count <= 0 || k < 0 || k >= hot_line_count) {
        return;
    }

    text_font(101);
    int height = text_height();

    // The hot line's own rectangle (x 254, 350 wide, one text line tall), two pixels of air.
    int x = 252;
    int y = PIPBOY_WINDOW_CONTENT_VIEW_Y + (hot_line_start + 2 * k) * height - 2;
    int w = 354;
    int h = height + 4;

    if (x < 0 || y < 0 || x + w > PIPBOY_WINDOW_WIDTH || y + h > PIPBOY_WINDOW_HEIGHT) {
        return;
    }
    if (w < 3 || h < 3 || w > PIP_HL_MAX_W || h > PIP_HL_MAX_H) {
        return;
    }

    // Save the whole footprint BEFORE drawing any of it, or the corners come back coloured.
    memcpy(pip_hl_top, scrn_buf + PIPBOY_WINDOW_WIDTH * y + x, w);
    memcpy(pip_hl_bottom, scrn_buf + PIPBOY_WINDOW_WIDTH * (y + h - 1) + x, w);
    for (int i = 0; i < h; i++) {
        const unsigned char* row = scrn_buf + PIPBOY_WINDOW_WIDTH * (y + i);
        pip_hl_left[i] = row[x];
        pip_hl_right[i] = row[x + w - 1];
    }

    // colorTable[992] is the Pip-Boy's own green -- the colour every line on this screen is already
    // printed in, so the frame belongs to the display rather than sitting on top of it.
    unsigned char color = colorTable[992];
    memset(scrn_buf + PIPBOY_WINDOW_WIDTH * y + x, color, w);
    memset(scrn_buf + PIPBOY_WINDOW_WIDTH * (y + h - 1) + x, color, w);
    for (int i = 0; i < h; i++) {
        unsigned char* row = scrn_buf + PIPBOY_WINDOW_WIDTH * (y + i);
        row[x] = color;
        row[x + w - 1] = color;
    }

    pip_hl_x = x;
    pip_hl_y = y;
    pip_hl_w = w;
    pip_hl_h = h;
}

// Where hot line k sits. AddHotLines registers them at x 254, first at hot_line_start * height,
// then every SECOND text line (y += height * 2).
static void pip_pad_point(int k)
{
    if (hot_line_count <= 0) {
        return;
    }

    Rect winRect;
    if (win_get_rect(pip_win, &winRect) != 0) {
        return;
    }

    text_font(101);
    int height = text_height();

    mouse_set_position(winRect.ulx + 254 + 8,
        winRect.uly + PIPBOY_WINDOW_CONTENT_VIEW_Y + (hot_line_start + 2 * k) * height + height / 2);
}

// 0x486A80
int pipboy(int intent)
{
    intent = StartPipboy(intent);
    if (intent == -1) {
        return -1;
    }

    mouseGetPositionInWindow(pip_win, &old_mouse_x, &old_mouse_y);
    wait_time = get_time();

    // ⚠️ Every button must be seen RELEASED once before it acts in here, and these are LOCALS, not
    // statics: a screen that opens while a button is physically down reads it as a fresh press on
    // its first frame -- which on the elevator meant the panel confirmed the floor you were already
    // standing on and vanished before you saw it. Statics would arm themselves permanently after
    // the first visit and the guard would only work once.
    bool pipArmedA = false;
    bool pipArmedB = false;

    while (true) {
        sharedFpsLimiter.mark();

        // ⚠️ The frame comes down before anything can draw -- input below may repaint the whole
        // content area, and the save-under is only valid until it does.
        pip_hl_erase();

        int keyCode = get_input();

        // --- gamepad -------------------------------------------------------------------------
        // Injects the very same event codes the mouse buttons report, so everything below this
        // point stays exactly as it was and there is no second dispatch to keep in step.
        {
            static bool pipPrevUp = false;
            static bool pipPrevDown = false;
            static bool pipPrevA = false;
            static bool pipPrevB = false;
            static bool pipPrevLB = false;
            static bool pipPrevRB = false;
            static unsigned int pipLastStep = 0;


            SDL_GameController* pipPad = gamepad_ui_get_controller();
            if (pipPad != NULL) {
                // ⚠️ NO self_nav HERE, AND THAT IS THE WHOLE CORRECTION. Every other modal wants the
                // gameplay layer gone, but this screen was ALREADY fully playable with the stick
                // cursor -- "pipboy se ovládal v pohodě s levým joystickem" -- and self_nav is
                // exactly what takes that cursor away. Turning it on traded a screen where you could
                // click anything for one where you could only reach the hot lines: not the tabs, not
                // the sub-buttons, not WAIT. That is a net loss however good the list stepping is.
                //
                // So the pad only ADDS here: the D-pad and stick PARK THE CURSOR on entries (A then
                // clicks them exactly as it always did, no injection, nothing to conflict with), and
                // LB/RB switch pages.
                SDL_GameControllerUpdate();
                bool upNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
                bool downNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
                bool aNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_A) != 0;
                bool bNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_B) != 0;
                bool lbNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
                bool rbNow = SDL_GameControllerGetButton(pipPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;

                if (!aNow) {
                    pipArmedA = true;
                }
                if (!bNow) {
                    pipArmedB = true;
                }

                bool repeat = (upNow || downNow) && elapsed_time(pipLastStep) >= 220;

                int step = 0;
                if ((upNow && !pipPrevUp) || (upNow && repeat)) {
                    step = -1;
                } else if ((downNow && !pipPrevDown) || (downNow && repeat)) {
                    step = 1;
                } else {
                    // The left stick does it too -- it is the thing your thumb is already on.
                    step = gamepad_ui_menu_stick_step();
                }

                if (step != 0 && hot_line_count > 0) {
                    pipLastStep = get_time();
                    int next = pip_pad_sel + step;
                    if (next >= 0 && next < hot_line_count) {
                        pip_pad_sel = next;
                        pip_pad_point(pip_pad_sel);
                    }
                }

                // LB / RB step between the PAGES, which is the other half of "blbě se to ovládá":
                // the tabs are buttons too, and a pad could not press them either. 503 is CLOSE and
                // is deliberately not in the ring -- you leave with B.
                static const int pipTabs[] = { 500, 501, 502, 504 };
                int tabStep = 0;
                if (rbNow && !pipPrevRB) {
                    tabStep = 1;
                } else if (lbNow && !pipPrevLB) {
                    tabStep = -1;
                }

                if (tabStep != 0) {
                    int here = 0;
                    for (int i = 0; i < 4; i++) {
                        if (pipTabs[i] - 500 == crnt_func) {
                            here = i;
                            break;
                        }
                    }
                    keyCode = pipTabs[(here + tabStep + 4) % 4];
                } else if (pipArmedB && bNow && !pipPrevB) {
                    // Back out of a sub-page if there is one to back out of, otherwise close.
                    // ⚠️ A is deliberately NOT injected: the cursor is already parked on the
                    // selected entry, so the ordinary A-click lands on it. Injecting as well would
                    // fire the entry twice, and would also make A mean something different here
                    // from everywhere else on this screen.
                    keyCode = HotLines[BACK_BUTTON_INDEX] != -1 ? 528 : KEY_ESCAPE;
                }

                pipPrevUp = upNow;
                pipPrevDown = downNow;
                pipPrevA = aNow;
                pipPrevB = bNow;
                pipPrevLB = lbNow;
                pipPrevRB = rbNow;
            }
        }

        if (intent == PIPBOY_OPEN_INTENT_REST) {
            keyCode = 504;
            intent = PIPBOY_OPEN_INTENT_UNSPECIFIED;
        }

        mouseGetPositionInWindow(pip_win, &mouse_x, &mouse_y);

        if (keyCode != -1 || mouse_x != old_mouse_x || mouse_y != old_mouse_y) {
            wait_time = get_time();
            old_mouse_x = mouse_x;
            old_mouse_y = mouse_y;
        } else {
            if (get_time() - wait_time > PIPBOY_IDLE_TIMEOUT) {
                ScreenSaver();

                wait_time = get_time();
                mouseGetPositionInWindow(pip_win, &old_mouse_x, &old_mouse_y);
            }
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            game_quit_with_confirm();
            break;
        }

        if (keyCode == 503 || keyCode == KEY_ESCAPE || keyCode == KEY_RETURN || keyCode == KEY_UPPERCASE_P || keyCode == KEY_LOWERCASE_P || keyCode == KEY_UPPERCASE_Z || keyCode == KEY_LOWERCASE_Z || game_user_wants_to_quit != 0) {
            break;
        }

        if (keyCode == KEY_F12) {
            dump_screen();
        } else if (keyCode >= 500 && keyCode <= 504) {
            crnt_func = keyCode - 500;
            PipFnctn[crnt_func](1024);
        } else if (keyCode >= 505 && keyCode <= 527) {
            PipFnctn[crnt_func](keyCode - 506);
        } else if (keyCode == 528) {
            PipFnctn[crnt_func](1025);
        } else if (keyCode == KEY_PAGE_DOWN) {
            PipFnctn[crnt_func](1026);
        } else if (keyCode == KEY_PAGE_UP) {
            PipFnctn[crnt_func](1027);
        }

        if (proc_bail_flag) {
            break;
        }

        // ...and back up at the bottom, over whatever the page looks like now.
        if (gamepad_ui_get_controller() != NULL) {
            pip_hl_draw(pip_pad_sel);
            win_draw(pip_win);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    pip_hl_erase();

    EndPipboy();

    return 0;
}

// 0x486C5C
static int StartPipboy(int intent)
{
    bk_enable = map_disable_bk_processes();

    cycle_disable();
    gmouse_3d_off();
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    savefont = text_curr();
    text_font(101);

    proc_bail_flag = 0;
    rest_time = 0;
    cursor_line = 0;
    hot_line_count = 0;
    bottom_line = PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT / text_height() - 1;
    hot_line_start = 0;
    hot_back_line = 0;
    ctrl_flag = 0;

    if (!message_init(&pipboy_message_file)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", msg_path, "pipboy.msg");

    if (!(message_load(&pipboy_message_file, path))) {
        return -1;
    }

    int index;
    for (index = 0; index < PIPBOY_FRM_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, pipgrphs[index], 0, 0, 0);
        pipbmp[index] = art_lock(fid, &(grphkey[index]), &(ginfo[index].width), &(ginfo[index].height));
        if (pipbmp[index] == NULL) {
            break;
        }
    }

    if (index != PIPBOY_FRM_COUNT) {
        debug_printf("\n** Error loading pipboy graphics! **\n");

        while (--index >= 0) {
            art_ptr_unlock(grphkey[index]);
        }

        return -1;
    }

    int pipboyWindowX = (screenGetWidth() - PIPBOY_WINDOW_WIDTH) / 2;
    int pipboyWindowY = (screenGetHeight() - PIPBOY_WINDOW_HEIGHT) / 2;
    pip_win = win_add(pipboyWindowX, pipboyWindowY, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_HEIGHT, colorTable[0], WINDOW_MODAL);
    if (pip_win == -1) {
        debug_printf("\n** Error opening pipboy window! **\n");
        for (int index = 0; index < PIPBOY_FRM_COUNT; index++) {
            art_ptr_unlock(grphkey[index]);
        }
        return -1;
    }

    scrn_buf = win_get_buf(pip_win);
    memcpy(scrn_buf, pipbmp[PIPBOY_FRM_BACKGROUND], PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_HEIGHT);

    pip_note();
    pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
    pip_date();

    int alarmButton = win_register_button(pip_win,
        124,
        13,
        ginfo[PIPBOY_FRM_ALARM_UP].width,
        ginfo[PIPBOY_FRM_ALARM_UP].height,
        -1,
        -1,
        -1,
        504,
        pipbmp[PIPBOY_FRM_ALARM_UP],
        pipbmp[PIPBOY_FRM_ALARM_DOWN],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (alarmButton != -1) {
        win_register_button_sound_func(alarmButton, gsound_med_butt_press, gsound_med_butt_release);
    }

    int y = 341;
    int eventCode = 500;
    for (int index = 0; index < 5; index += 1) {
        if (index != 1) {
            int btn = win_register_button(pip_win,
                53,
                y,
                ginfo[PIPBOY_FRM_LITTLE_RED_BUTTON_UP].width,
                ginfo[PIPBOY_FRM_LITTLE_RED_BUTTON_UP].height,
                -1,
                -1,
                -1,
                eventCode,
                pipbmp[PIPBOY_FRM_LITTLE_RED_BUTTON_UP],
                pipbmp[PIPBOY_FRM_LITTLE_RED_BUTTON_DOWN],
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            if (btn != -1) {
                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);
            }

            eventCode += 1;
        }

        y += 27;
    }

    if (intent == PIPBOY_OPEN_INTENT_REST) {
        if (!critter_can_obj_dude_rest()) {
            trans_buf_to_buf(
                pipbmp[PIPBOY_FRM_LOGO],
                ginfo[PIPBOY_FRM_LOGO].width,
                ginfo[PIPBOY_FRM_LOGO].height,
                ginfo[PIPBOY_FRM_LOGO].width,
                scrn_buf + PIPBOY_WINDOW_WIDTH * 156 + 323,
                PIPBOY_WINDOW_WIDTH);

            int month;
            int day;
            int year;
            game_time_date(&month, &day, &year);

            int holiday = 0;
            for (; holiday < HOLIDAY_COUNT; holiday += 1) {
                const HolidayDescription* holidayDescription = &(SpclDate[holiday]);
                if (holidayDescription->month == month && holidayDescription->day == day) {
                    break;
                }
            }

            if (holiday != HOLIDAY_COUNT) {
                const HolidayDescription* holidayDescription = &(SpclDate[holiday]);
                const char* holidayName = getmsg(&pipboy_message_file, &pipmesg, holidayDescription->textId);
                char holidayNameCopy[256];
                strcpy(holidayNameCopy, holidayName);

                int len = text_width(holidayNameCopy);
                text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * (ginfo[PIPBOY_FRM_LOGO].height + 174) + 6 + ginfo[PIPBOY_FRM_LOGO].width / 2 + 323 - len / 2,
                    holidayNameCopy,
                    350,
                    PIPBOY_WINDOW_WIDTH,
                    colorTable[992]);
            }

            win_draw(pip_win);

            gsound_play_sfx_file("iisxxxx1");

            const char* text = getmsg(&pipboy_message_file, &pipmesg, 215);
            dialog_out(text, NULL, 0, 192, 135, colorTable[32328], 0, colorTable[32328], DIALOG_BOX_LARGE);

            intent = PIPBOY_OPEN_INTENT_UNSPECIFIED;
        }
    } else {
        trans_buf_to_buf(
            pipbmp[PIPBOY_FRM_LOGO],
            ginfo[PIPBOY_FRM_LOGO].width,
            ginfo[PIPBOY_FRM_LOGO].height,
            ginfo[PIPBOY_FRM_LOGO].width,
            scrn_buf + PIPBOY_WINDOW_WIDTH * 156 + 323,
            PIPBOY_WINDOW_WIDTH);

        int month;
        int day;
        int year;
        game_time_date(&month, &day, &year);

        int holiday;
        for (holiday = 0; holiday < HOLIDAY_COUNT; holiday += 1) {
            const HolidayDescription* holidayDescription = &(SpclDate[holiday]);
            if (holidayDescription->month == month && holidayDescription->day == day) {
                break;
            }
        }

        if (holiday != HOLIDAY_COUNT) {
            const HolidayDescription* holidayDescription = &(SpclDate[holiday]);
            const char* holidayName = getmsg(&pipboy_message_file, &pipmesg, holidayDescription->textId);
            char holidayNameCopy[256];
            strcpy(holidayNameCopy, holidayName);

            int length = text_width(holidayNameCopy);
            text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * (ginfo[PIPBOY_FRM_LOGO].height + 174) + 6 + ginfo[PIPBOY_FRM_LOGO].width / 2 + 323 - length / 2,
                holidayNameCopy,
                350,
                PIPBOY_WINDOW_WIDTH,
                colorTable[992]);
        }

        win_draw(pip_win);
    }

    gsound_play_sfx_file("pipon");
    win_draw(pip_win);

    return intent;
}

// 0x487218
static void EndPipboy()
{
    bool showScriptMessages = false;
    configGetBool(&game_config, GAME_CONFIG_DEBUG_KEY, GAME_CONFIG_SHOW_SCRIPT_MESSAGES_KEY, &showScriptMessages);

    if (showScriptMessages) {
        debug_printf("\nScript <Map Update>");
    }

    scr_exec_map_update_scripts();

    win_delete(pip_win);

    message_exit(&pipboy_message_file);

    for (int index = 0; index < PIPBOY_FRM_COUNT; index++) {
        art_ptr_unlock(grphkey[index]);
    }

    NixHotLines();

    text_font(savefont);

    if (bk_enable) {
        map_enable_bk_processes();
    }

    cycle_enable();
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    intface_redraw();
}

// 0x4872B4
void pip_init()
{
}

// 0x4872B8
static void pip_days_left(int days)
{
    int x = 92;
    int y = PIPBOY_WINDOW_WIDTH * 180;

    while (days != 0) {
        trans_buf_to_buf(pipbmp[PIPBOY_FRM_NOTE_NUMBERS] + 12 * (days % 10),
            12,
            ginfo[PIPBOY_FRM_NOTE_NUMBERS].height,
            ginfo[PIPBOY_FRM_NOTE_NUMBERS].width,
            scrn_buf + y + x,
            PIPBOY_WINDOW_WIDTH);

        if (days % 10 == 1) {
            x += 6;
        }

        days /= 10;

        x -= 12;
        y += PIPBOY_WINDOW_WIDTH * 2;
    }
}

// 0x487340
static void pip_num(int value, int digits, int x, int y)
{
    int offset = PIPBOY_WINDOW_WIDTH * y + x + 9 * (digits - 1);

    for (int index = 0; index < digits; index++) {
        buf_to_buf(pipbmp[PIPBOY_FRM_NUMBERS] + 9 * (value % 10), 9, 17, 360, scrn_buf + offset, PIPBOY_WINDOW_WIDTH);
        offset -= 9;
        value /= 10;
    }
}

// 0x4873D8
static void pip_date()
{
    int day;
    int month;
    int year;

    game_time_date(&month, &day, &year);
    pip_num(day, 2, PIPBOY_WINDOW_DAY_X, PIPBOY_WINDOW_DAY_Y);

    buf_to_buf(pipbmp[PIPBOY_FRM_MONTHS] + 435 * (month - 1), 29, 14, 29, scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_MONTH_Y + PIPBOY_WINDOW_MONTH_X, PIPBOY_WINDOW_WIDTH);

    pip_num(year, 4, PIPBOY_WINDOW_YEAR_X, PIPBOY_WINDOW_YEAR_Y);
}

// 0x487464
static void pip_print(const char* text, int flags, int color)
{
    if ((flags & PIPBOY_TEXT_STYLE_UNDERLINE) != 0) {
        color |= FONT_UNDERLINE;
    }

    int left = 8;
    if ((flags & PIPBOY_TEXT_NO_INDENT) != 0) {
        left -= 7;
    }

    int length = text_width(text);

    if ((flags & PIPBOY_TEXT_ALIGNMENT_CENTER) != 0) {
        left = (350 - length) / 2;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN) != 0) {
        left += 175;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER) != 0) {
        left += 86 - length + 16;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER) != 0) {
        left += 260 - length;
    }

    text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * (cursor_line * text_height() + PIPBOY_WINDOW_CONTENT_VIEW_Y) + PIPBOY_WINDOW_CONTENT_VIEW_X + left, text, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_WIDTH, color);

    if ((flags & PIPBOY_TEXT_STYLE_STRIKE_THROUGH) != 0) {
        int top = cursor_line * text_height() + 49;
        draw_line(scrn_buf, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_CONTENT_VIEW_X + left, top, PIPBOY_WINDOW_CONTENT_VIEW_X + left + length, top, color);
    }

    if (cursor_line < bottom_line) {
        cursor_line += 1;
    }
}

// 0x487588
static void pip_back(int color)
{
    if (bottom_line >= 0) {
        cursor_line = bottom_line;
    }

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, scrn_buf + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

    // BACK
    const char* text = getmsg(&pipboy_message_file, &pipmesg, 201);
    pip_print(text, PIPBOY_TEXT_ALIGNMENT_CENTER, color);
}

// 0x4875F8.
int save_pipboy(DB_FILE* stream)
{
    return 0;
}

// 0x4875F8.
int load_pipboy(DB_FILE* stream)
{
    return 0;
}

// FNV-1a hash of every tracked quest's global-var value. Callers (the autosave trigger in
// the main loop) poll this and save when it changes -- i.e. on any quest progress/completion
// -- without having to know per-quest completion thresholds (FO1 has no quest-done event).
// Keeps the sthreads/holodisk tables encapsulated here. Returns 0 before a game is loaded.
unsigned int pipboy_quest_state_hash()
{
    if (game_global_vars == NULL || num_game_global_vars <= 0) {
        return 0;
    }

    unsigned int h = 2166136261u;
    for (int location = 0; location < QUEST_LOCATION_COUNT; location++) {
        for (int quest = 0; quest < QUEST_PER_LOCATION_COUNT; quest++) {
            int gvar = sthreads[location][quest];
            if (gvar <= 0 || gvar >= num_game_global_vars) {
                continue;
            }
            h = (h ^ (unsigned int)game_global_vars[gvar]) * 16777619u;
        }
    }
    return h;
}

// 0x4875FC
static void PipStatus(int a1)
{
    if (a1 == 1024) {
        NixHotLines();
        buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);
        if (bottom_line >= 0) {
            cursor_line = 0;
        }

        holo_flag = 0;
        holodisk = -1;
        holocount = 0;
        view_page = 0;
        stat_flag = 0;

        for (int index = 0; index < HOLODISK_COUNT; index += 1) {
            if (game_global_vars[holodisks[index]] != 0) {
                holocount += 1;
                break;
            }
        }

        ListStatLines(-1);

        if (statcount == 0) {
            const char* text = getmsg(&pipboy_message_file, &pipmesg, 203);
            pip_print(text, 0, colorTable[992]);
        }

        holocount = ListHoloDiskTitles(-1);

        win_draw_rect(pip_win, &pip_rect);
        AddHotLines(2, statcount + holocount + 1, false);
        win_draw(pip_win);
        return;
    }

    if (stat_flag == 0 && holo_flag == 0) {
        if (statcount != 0 && mouse_x < 429) {
            gsound_play_sfx_file("ib1p1xx1");
            buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                PIPBOY_WINDOW_WIDTH,
                scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_WIDTH);
            ListStatLines(a1);
            ListHoloDiskTitles(-1);
            win_draw_rect(pip_win, &pip_rect);
            pause_for_tocks(200);
            stat_flag = 1;
        } else {
            if (holocount != 0 && holocount >= a1 && mouse_x > 429) {
                gsound_play_sfx_file("ib1p1xx1");
                holodisk = 0;

                int index = 0;
                for (; index < HOLODISK_COUNT; index += 1) {
                    if (game_global_vars[holodisks[index]] > 0) {
                        if (a1 - 1 == holodisk) {
                            break;
                        }
                        holodisk += 1;
                    }
                }
                holodisk = index;

                buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                    PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                    PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                    PIPBOY_WINDOW_WIDTH,
                    scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                    PIPBOY_WINDOW_WIDTH);
                ListHoloDiskTitles(holodisk);
                ListStatLines(-1);
                win_draw_rect(pip_win, &pip_rect);
                pause_for_tocks(200);
                NixHotLines();
                ShowHoloDisk();
                AddHotLines(0, 0, true);
                holo_flag = 1;
            }
        }
    }

    if (stat_flag == 0) {
        if (holo_flag == 0 || a1 < 1025 || a1 > 1027) {
            return;
        }

        if ((mouse_x > 459 && a1 != 1027) || a1 == 1026) {
            if (holopages <= view_page) {
                if (a1 != 1026) {
                    gsound_play_sfx_file("ib1p1xx1");
                    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, scrn_buf + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

                    if (bottom_line >= 0) {
                        cursor_line = bottom_line;
                    }

                    // Back
                    const char* text1 = getmsg(&pipboy_message_file, &pipmesg, 201);
                    pip_print(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

                    if (bottom_line >= 0) {
                        cursor_line = bottom_line;
                    }

                    // Done
                    const char* text2 = getmsg(&pipboy_message_file, &pipmesg, 214);
                    pip_print(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, colorTable[992]);

                    win_draw_rect(pip_win, &pip_rect);
                    pause_for_tocks(200);
                    PipStatus(1024);
                }
            } else {
                gsound_play_sfx_file("ib1p1xx1");
                buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, scrn_buf + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

                if (bottom_line >= 0) {
                    cursor_line = bottom_line;
                }

                // Back
                const char* text1 = getmsg(&pipboy_message_file, &pipmesg, 201);
                pip_print(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

                if (bottom_line >= 0) {
                    cursor_line = bottom_line;
                }

                // More
                const char* text2 = getmsg(&pipboy_message_file, &pipmesg, 200);
                pip_print(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, colorTable[992]);

                win_draw_rect(pip_win, &pip_rect);
                pause_for_tocks(200);

                view_page += 1;

                ShowHoloDisk();
            }
            return;
        }

        if (a1 == 1027) {
            gsound_play_sfx_file("ib1p1xx1");
            buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, scrn_buf + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

            if (bottom_line >= 0) {
                cursor_line = bottom_line;
            }

            // Back
            const char* text1 = getmsg(&pipboy_message_file, &pipmesg, 201);
            pip_print(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

            if (bottom_line >= 0) {
                cursor_line = bottom_line;
            }

            // More
            const char* text2 = getmsg(&pipboy_message_file, &pipmesg, 200);
            pip_print(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, colorTable[992]);

            win_draw_rect(pip_win, &pip_rect);
            pause_for_tocks(200);

            view_page -= 1;

            if (view_page < 0) {
                PipStatus(1024);
                return;
            }
        } else {
            if (mouse_x > 395) {
                return;
            }

            gsound_play_sfx_file("ib1p1xx1");
            buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, scrn_buf + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

            if (bottom_line >= 0) {
                cursor_line = bottom_line;
            }

            // Back
            const char* text1 = getmsg(&pipboy_message_file, &pipmesg, 201);
            pip_print(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

            if (bottom_line >= 0) {
                cursor_line = bottom_line;
            }

            // More
            const char* text2 = getmsg(&pipboy_message_file, &pipmesg, 200);
            pip_print(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, colorTable[992]);

            win_draw_rect(pip_win, &pip_rect);
            pause_for_tocks(200);

            if (view_page <= 0) {
                PipStatus(1024);
                return;
            }

            view_page -= 1;
        }

        ShowHoloDisk();
        return;
    }

    if (a1 == 1025) {
        gsound_play_sfx_file("ib1p1xx1");
        pip_back(colorTable[32747]);
        win_draw_rect(pip_win, &pip_rect);
        pause_for_tocks(200);
        PipStatus(1024);
    }

    if (a1 <= statcount) {
        gsound_play_sfx_file("ib1p1xx1");

        int v13 = 0;
        int location = 0;
        for (; location < QUEST_LOCATION_COUNT && v13 != -1; location++) {
            for (int quest = 0; quest < QUEST_PER_LOCATION_COUNT; quest++) {
                if (sthreads[location][quest] == 0) {
                    break;
                }

                int value = game_global_vars[sthreads[location][quest]];
                if (sthreads[location][quest] == GVAR_FIND_WATER_CHIP) {
                    value = 1;
                }

                if (value > 0) {
                    if (v13 == a1 - 1) {
                        v13 = -1;
                    } else {
                        v13++;
                    }
                    break;
                }
            }
        }

        NewFuncDsply();

        location -= 1;

        if (bottom_line >= 1) {
            cursor_line = 1;
        }

        AddHotLines(0, 0, true);

        const char* text1 = getmsg(&pipboy_message_file, &pipmesg, 210);
        const char* text2 = getmsg(&pipboy_message_file, &pipmesg, 700 + 10 * location);
        char formattedText[1024];
        snprintf(formattedText, sizeof(formattedText), "%s %s", text2, text1);
        pip_print(formattedText, PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

        if (bottom_line >= 3) {
            cursor_line = 3;
        }

        int number = 1;
        for (int quest = 0; quest < QUEST_PER_LOCATION_COUNT; quest++) {
            if (sthreads[location][quest] == 0) {
                break;
            }

            int value = game_global_vars[sthreads[location][quest]];
            if (sthreads[location][quest] == GVAR_FIND_WATER_CHIP) {
                if (value > 1) {
                    value = 2;
                } else {
                    value = 1;
                }
            }

            if (value > 0) {
                const char* text = getmsg(&pipboy_message_file, &pipmesg, 701 + 10 * location + quest);
                char formattedText[1024];
                snprintf(formattedText, sizeof(formattedText), "%d. %s", number, text);
                number += 1;

                short beginnings[WORD_WRAP_MAX_COUNT];
                short count;
                if (word_wrap(formattedText, 350, beginnings, &count) == 0) {
                    for (int line = 0; line < count - 1; line += 1) {
                        char* beginning = formattedText + beginnings[line];
                        char* ending = formattedText + beginnings[line + 1];
                        char c = *ending;
                        *ending = '\0';

                        int flags;
                        int color;
                        if (value == 1) {
                            flags = 0;
                            color = colorTable[992];
                        } else {
                            flags = PIPBOY_TEXT_STYLE_STRIKE_THROUGH;
                            color = colorTable[8804];
                        }

                        pip_print(beginning, flags, color);

                        *ending = c;
                        cursor_line += 1;
                    }
                } else {
                    debug_printf("\n ** Word wrap error in pipboy! **\n");
                }
            }
        }

        pip_back(colorTable[992]);
        win_draw_rect(pip_win, &pip_rect);
        stat_flag = 1;
    }
}

// [a1] is likely selected location, or -1 if nothing is selected
//
// 0x4880FC
static void ListStatLines(int a1)
{
    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    int flags = holocount != 0 ? PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER : PIPBOY_TEXT_ALIGNMENT_CENTER;
    flags |= PIPBOY_TEXT_STYLE_UNDERLINE;

    // STATUS
    const char* statusText = getmsg(&pipboy_message_file, &pipmesg, 202);
    pip_print(statusText, flags, colorTable[992]);

    if (bottom_line >= 2) {
        cursor_line = 2;
    }

    statcount = 0;

    for (int location = 0; location < QUEST_LOCATION_COUNT; location += 1) {
        for (int quest = 0; quest < 9; quest++) {
            int value = game_global_vars[sthreads[location][quest]];
            if (sthreads[location][quest] == GVAR_FIND_WATER_CHIP) {
                value = 1;
            }

            if (value > 0) {
                int color = (cursor_line - 1) / 2 == (a1 - 1) ? colorTable[32747] : colorTable[992];

                // Render location.
                const char* questLocation = getmsg(&pipboy_message_file, &pipmesg, 700 + 10 * location);
                pip_print(questLocation, 0, color);

                cursor_line += 1;
                statcount += 1;

                break;
            }
        }
    }
}

// 0x488254
static void ShowHoloDisk()
{
    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    int holodiskTextId;
    int linesCount = 0;

    holopages = 0;

    for (holodiskTextId = 1000 * holodisk + 1000; holodiskTextId < 1000 * holodisk + 1500; holodiskTextId += 1) {
        const char* text = getmsg(&pipboy_message_file, &pipmesg, holodiskTextId);
        if (strcmp(text, "**END-DISK**") == 0) {
            break;
        }

        linesCount += 1;
        if (linesCount >= PIPBOY_HOLODISK_LINES_MAX) {
            linesCount = 0;
            holopages += 1;
        }
    }

    if (holodiskTextId >= 1000 * holodisk + 1500) {
        debug_printf("\nPIPBOY: #1 Holodisk text end not found!\n");
    }

    holodiskTextId = 1000 * holodisk + 1000;

    if (view_page != 0) {
        int page = 0;
        int numberOfLines = 0;
        for (; holodiskTextId < 1000 * holodisk + 1500; holodiskTextId += 1) {
            const char* line = getmsg(&pipboy_message_file, &pipmesg, holodiskTextId);
            if (strcmp(line, "**END-DISK**") == 0) {
                debug_printf("\nPIPBOY: Premature page end in holodisk page search!\n");
                break;
            }

            numberOfLines += 1;
            if (numberOfLines >= PIPBOY_HOLODISK_LINES_MAX) {
                page += 1;
                if (page >= view_page) {
                    break;
                }

                numberOfLines = 0;
            }
        }

        holodiskTextId += 1;

        if (holodiskTextId >= 1000 * holodisk + 1500) {
            debug_printf("\nPIPBOY: #2 Holodisk text end not found!\n");
        }
    } else {
        const char* name = getmsg(&pipboy_message_file, &pipmesg, holodisk + 400);
        pip_print(name, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);
    }

    if (holopages != 0) {
        // of
        const char* of = getmsg(&pipboy_message_file, &pipmesg, 212);
        char formattedText[60]; // TODO: Size is probably wrong.
        snprintf(formattedText, sizeof(formattedText), "%d %s %d", view_page + 1, of, holopages + 1);

        int len = text_width(of);
        text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * 47 + 616 + 604 - len, formattedText, 350, PIPBOY_WINDOW_WIDTH, colorTable[992]);
    }

    if (bottom_line >= 3) {
        cursor_line = 3;
    }

    for (int line = 0; line < PIPBOY_HOLODISK_LINES_MAX; line += 1) {
        const char* text = getmsg(&pipboy_message_file, &pipmesg, holodiskTextId);
        if (strcmp(text, "**END-DISK**") == 0) {
            break;
        }

        if (strcmp(text, "**END-PAR**") == 0) {
            cursor_line += 1;
        } else {
            pip_print(text, PIPBOY_TEXT_NO_INDENT, colorTable[992]);
        }

        holodiskTextId += 1;
    }

    int moreOrDoneTextId;
    if (holopages <= view_page) {
        if (bottom_line >= 0) {
            cursor_line = bottom_line;
        }

        const char* back = getmsg(&pipboy_message_file, &pipmesg, 201);
        pip_print(back, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

        if (bottom_line >= 0) {
            cursor_line = bottom_line;
        }

        moreOrDoneTextId = 214;
    } else {
        if (bottom_line >= 0) {
            cursor_line = bottom_line;
        }

        const char* back = getmsg(&pipboy_message_file, &pipmesg, 201);
        pip_print(back, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, colorTable[992]);

        if (bottom_line >= 0) {
            cursor_line = bottom_line;
        }

        moreOrDoneTextId = 200;
    }

    const char* moreOrDoneText = getmsg(&pipboy_message_file, &pipmesg, moreOrDoneTextId);
    pip_print(moreOrDoneText, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, colorTable[992]);
    win_draw(pip_win);
}

// 0x4885E0
static int ListHoloDiskTitles(int a1)
{
    if (bottom_line >= 2) {
        cursor_line = 2;
    }

    int knownHolodisksCount = 0;
    for (int index = 0; index < HOLODISK_COUNT; index++) {
        if (game_global_vars[holodisks[index]] != 0) {
            int color;
            if ((cursor_line - 2) / 2 == a1) {
                color = colorTable[32747];
            } else {
                color = colorTable[992];
            }

            const char* text = getmsg(&pipboy_message_file, &pipmesg, 400 + index);
            pip_print(text, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN, color);

            cursor_line++;
            knownHolodisksCount++;
        }
    }

    if (knownHolodisksCount != 0) {
        if (bottom_line >= 0) {
            cursor_line = 0;
        }

        const char* text = getmsg(&pipboy_message_file, &pipmesg, 211); // DATA
        pip_print(text, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);
    }

    return knownHolodisksCount;
}

// 0x4886C4
static int qscmp(const void* a1, const void* a2)
{
    PipboySortableEntry* v1 = (PipboySortableEntry*)a1;
    PipboySortableEntry* v2 = (PipboySortableEntry*)a2;

    return strcmp(v1->name, v2->name);
}

// 0x4886D0
static void PipAutomaps(int a1)
{
    if (a1 == 1024) {
        NixHotLines();
        buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);

        if (bottom_line >= 0) {
            cursor_line = 0;
        }

        const char* title = getmsg(&pipboy_message_file, &pipmesg, 205);
        pip_print(title, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

        actcnt = PrintAMList(-1);

        AddHotLines(2, actcnt, 0);

        win_draw_rect(pip_win, &pip_rect);
        amlst_mode = 0;
        return;
    }

    if (amlst_mode != 0) {
        if (a1 == 1025 || a1 <= -1) {
            PipAutomaps(1024);
            gsound_play_sfx_file("ib1p1xx1");
        }

        if (a1 >= 1 && a1 <= actcnt + 3) {
            gsound_play_sfx_file("ib1p1xx1");
            PrintAMelevList(a1);
            draw_top_down_map_pipboy(pip_win, sortlist[a1 - 1].field_6, sortlist[a1 - 1].value);
            win_draw_rect(pip_win, &pip_rect);
        }

        return;
    }

    if (a1 > 0 && a1 <= actcnt) {
        gsound_play_sfx_file("ib1p1xx1");
        NixHotLines();
        PrintAMList(a1);
        win_draw_rect(pip_win, &pip_rect);
        amcty_indx = sortlist[a1 - 1].value;
        actcnt = PrintAMelevList(1);
        AddHotLines(0, actcnt + 2, 1);
        draw_top_down_map_pipboy(pip_win, sortlist[0].field_6, sortlist[0].value);
        win_draw_rect(pip_win, &pip_rect);
        amlst_mode = 1;
    }
}

// 0x4888C0
static int PrintAMelevList(int a1)
{
    AutomapHeader* automapHeader;
    if (ReadAMList(&automapHeader) == -1) {
        return -1;
    }

    int line = 0;
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if (automapHeader->offsets[amcty_indx][elevation] > 0) {
            sortlist[line].name = map_get_elev_idx(amcty_indx, elevation);
            sortlist[line].value = elevation;
            sortlist[line].field_6 = amcty_indx;
            line++;
        }
    }

    for (int index = 0; index < 5; index++) {
        int map = get_map_idx_same(amcty_indx, index);
        if (map != -1) {
            for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
                if (automapHeader->offsets[map][elevation] > 0) {
                    sortlist[line].name = map_get_elev_idx(map, elevation);
                    sortlist[line].value = elevation;
                    sortlist[line].field_6 = map;
                    line++;
                }
            }
        }
    }

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    const char* msg = getmsg(&pipboy_message_file, &pipmesg, 205);
    pip_print(msg, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

    if (bottom_line >= 2) {
        cursor_line = 2;
    }

    const char* name = map_get_description_idx(amcty_indx);
    pip_print(name, PIPBOY_TEXT_ALIGNMENT_CENTER, colorTable[992]);

    if (bottom_line >= 4) {
        cursor_line = 4;
    }

    int selectedPipboyLine = (a1 - 1) * 2;

    for (int index = 0; index < line; index++) {
        int color;
        if (cursor_line - 4 == selectedPipboyLine) {
            color = colorTable[32747];
        } else {
            color = colorTable[992];
        }

        pip_print(sortlist[index].name, 0, color);
        cursor_line++;
    }

    pip_back(colorTable[992]);

    return line;
}

// 0x488AE4
static int PrintAMList(int a1)
{
    AutomapHeader* automapHeader;
    if (ReadAMList(&automapHeader) == -1) {
        return -1;
    }

    int count = 0;
    int index = 0;

    for (int map = 0; map < MAP_COUNT; map++) {
        int elevation;
        for (elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            if (automapHeader->offsets[map][elevation] > 0) {
                break;
            }
        }

        if (elevation < ELEVATION_COUNT) {
            int v7;
            if (count != 0) {
                v7 = 0;
                for (int index = 0; index < count; index++) {
                    if (is_map_idx_same(map, sortlist[index].value)) {
                        break;
                    }

                    v7++;
                }
            } else {
                v7 = 0;
            }

            if (v7 == count) {
                sortlist[count].name = map_get_short_name(map);
                sortlist[count].value = map;
                count++;
            }
        }
    }

    if (count != 0) {
        if (count > 1) {
            qsort(sortlist, count, sizeof(*sortlist), qscmp);
        }

        buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);

        if (bottom_line >= 0) {
            cursor_line = 0;
        }

        const char* msg = getmsg(&pipboy_message_file, &pipmesg, 205);
        pip_print(msg, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

        if (bottom_line >= 2) {
            cursor_line = 2;
        }

        for (int index = 0; index < count; index++) {
            int color;
            if (cursor_line - 1 == a1) {
                color = colorTable[32747];
            } else {
                color = colorTable[992];
            }

            pip_print(sortlist[index].name, 0, color);
            cursor_line++;
        }
    }

    return count;
}

// 0x488CAC
static void PipArchives(int a1)
{
    if (a1 == 1024) {
        ctrl_flag = 0;
        NixHotLines();
        view_page = ListArchive(-1);
        AddHotLines(2, view_page, false);
        return;
    }

    // The CONTROLS page is up. It has no hot lines of its own -- only the BACK button
    // (event 528, arriving here as 1025), which returns to the archive list. Everything
    // else is inert while it shows.
    if (ctrl_flag) {
        if (a1 == 1025) {
            gsound_play_sfx_file("ib1p1xx1");
            pip_back(colorTable[32747]);
            win_draw_rect(pip_win, &pip_rect);
            pause_for_tocks(200);
            PipArchives(1024);
        }
        return;
    }

    if (a1 >= 0 && a1 <= view_page) {
        gsound_play_sfx_file("ib1p1xx1");

        ListArchive(a1);

        // The four CONTROLS pages are the last entries of the list, and none of them is a movie.
        // view_page is the line count ListArchive returned, so KEYBOARD sits at view_page, MENUS one
        // line above it, SCREENS one above that and WORLD above SCREENS -- the same order ListArchive
        // prints them in.
        if (a1 >= view_page - 3) {
            pause_for_tocks(200);
            NixHotLines();
            if (a1 == view_page) {
                ShowControls4();
            } else if (a1 == view_page - 1) {
                ShowControls3();
            } else if (a1 == view_page - 2) {
                ShowControls2();
            } else {
                ShowControls();
            }
            AddHotLines(0, 0, true);
            // ⚠️ Vanilla never sets this, so its own BACK button (the holodisk reader) leaks
            // one registered button per open. Ours is claimed properly, so NixHotLines takes
            // it down again on the way out -- the archive list re-registers over that row.
            hot_back_line = 1;
            ctrl_flag = 1;
            return;
        }

        int movie;
        for (movie = MOVIE_VEXPLD; movie < MOVIE_COUNT; movie++) {
            if (gmovie_has_been_played(movie)) {
                a1--;
                if (a1 <= 0) {
                    break;
                }
            }
        }

        if (movie <= MOVIE_COUNT) {
            gmovie_play(movie, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
        } else {
            debug_printf("\n ** Selected movie not found in list! **\n");
        }

        text_font(101);

        wait_time = get_time();
        ListArchive(-1);
    }
}

// 0x488D5C
static int ListArchive(int a1)
{
    const char* text;
    int movie;
    int line;
    int color;

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    // VIDEO ARCHIVES
    text = getmsg(&pipboy_message_file, &pipmesg, 206);
    pip_print(text, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

    if (bottom_line >= 2) {
        cursor_line = 2;
    }

    line = 0;

    for (movie = MOVIE_VEXPLD; movie < MOVIE_COUNT; movie++) {
        if (gmovie_has_been_played(movie)) {
            if (line++ == a1 - 1) {
                color = colorTable[32747];
            } else {
                color = colorTable[992];
            }

            text = getmsg(&pipboy_message_file, &pipmesg, 500 + movie);
            pip_print(text, 0, color);

            cursor_line++;
        }
    }

    // CONTROLS -- the pad layout, always the LAST entry. Last rather than first because the
    // archive fills up as the game is played and this must not shuffle around; and it is the
    // only entry that exists at the start of a game, which is when it is actually wanted.
    if (line++ == a1 - 1) {
        color = colorTable[32747];
    } else {
        color = colorTable[992];
    }

    pip_print(ctrl_entry, 0, color);
    cursor_line++;

    // ...and the screens page right behind it, so the two read as one pair in the list.
    if (line++ == a1 - 1) {
        color = colorTable[32747];
    } else {
        color = colorTable[992];
    }

    pip_print(ctrl2_entry, 0, color);
    cursor_line++;

    // ...and the menus page last. The order here IS the dispatch order in PipArchives (which reads
    // them off the end of the list as view_page, -1, -2), so the two must be changed together.
    if (line++ == a1 - 1) {
        color = colorTable[32747];
    } else {
        color = colorTable[992];
    }

    pip_print(ctrl3_entry, 0, color);
    cursor_line++;

    // ...and the keyboard page after it (2026-10-10). Same rule: this order is PipArchives' dispatch.
    if (line++ == a1 - 1) {
        color = colorTable[32747];
    } else {
        color = colorTable[992];
    }

    pip_print(ctrl4_entry, 0, color);
    cursor_line++;

    win_draw_rect(pip_win, &pip_rect);

    return line;
}

// One row of the CONTROLS page: the pad button on the left, what it does on the right.
// pip_print's column stops (8 and +175) are the wrong shape here -- the labels run to
// "Tap a direction" and the descriptions are longer than half the page -- so the split is
// measured from the widest label instead (ctrl_col, computed once per page in ShowControls).
static void pip_ctrl_line(const char* button, const char* action, int color)
{
    int top = cursor_line * text_height() + PIPBOY_WINDOW_CONTENT_VIEW_Y;

    if (button != NULL) {
        text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * top + PIPBOY_WINDOW_CONTENT_VIEW_X + 8,
            button,
            PIPBOY_WINDOW_WIDTH,
            PIPBOY_WINDOW_WIDTH,
            color);
    }

    if (action != NULL) {
        text_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * top + PIPBOY_WINDOW_CONTENT_VIEW_X + ctrl_col,
            action,
            PIPBOY_WINDOW_WIDTH,
            PIPBOY_WINDOW_WIDTH,
            color);
    }

    if (cursor_line < bottom_line) {
        cursor_line += 1;
    }
}

// 0x000000 (not vanilla)
static void ShowControls()
{
    ShowControlsPage(ctrl_entry, ctrl_lines, (int)(sizeof(ctrl_lines) / sizeof(ctrl_lines[0])));
}

static void ShowControls2()
{
    ShowControlsPage(ctrl2_entry, ctrl2_lines, (int)(sizeof(ctrl2_lines) / sizeof(ctrl2_lines[0])));
}

static void ShowControls3()
{
    ShowControlsPage(ctrl3_entry, ctrl3_lines, (int)(sizeof(ctrl3_lines) / sizeof(ctrl3_lines[0])));
}

static void ShowControls4()
{
    ShowControlsPage(ctrl4_entry, ctrl4_lines, (int)(sizeof(ctrl4_lines) / sizeof(ctrl4_lines[0])));
}

// All three pages render through here: same layout, only the title and the table differ.
static void ShowControlsPage(const char* title, const PipboyControl* lines, int count)
{
    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    // Where the second column starts: past the widest button label, never so far right that a
    // description would run out of the 350px text area. Measured rather than hardcoded so the
    // page survives a different font (the Pip-Boy font is a data file, not ours).
    ctrl_col = 0;
    for (int index = 0; index < count; index++) {
        if (lines[index].button != NULL) {
            int width = text_width(lines[index].button);
            if (width > ctrl_col) {
                ctrl_col = width;
            }
        }
    }
    ctrl_col += 8 + 12;
    if (ctrl_col > 160) {
        ctrl_col = 160;
    }

    // The blank separator lines are the first thing to go if the page does not fit -- one line
    // of headroom is worth more than the spacing, and pip_print/pip_ctrl_line would otherwise
    // pile every overflowing row onto the bottom line.
    int needed = 1 + count;

    bool spacious = needed <= bottom_line;

    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    pip_print(title, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

    for (int index = 0; index < count; index++) {
        const PipboyControl* entry = &(lines[index]);

        if (entry->button == NULL && entry->action == NULL) {
            if (spacious) {
                pip_ctrl_line(NULL, NULL, colorTable[992]);
            }
        } else if (entry->button == NULL) {
            pip_print(entry->action, PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[32747]);
        } else {
            pip_ctrl_line(entry->button, entry->action, colorTable[992]);
        }
    }

    pip_back(colorTable[992]);

    win_draw_rect(pip_win, &pip_rect);
}

// 0x488E94
static void PipAlarm(int a1)
{
    if (a1 == 1024) {
        if (critter_can_obj_dude_rest()) {
            NixHotLines();
            DrawAlarmText(0);
            AddHotLines(5, PIPBOY_REST_DURATION_COUNT_WITHOUT_PARTY, false);
        } else {
            gsound_play_sfx_file("iisxxxx1");

            // You cannot rest at this location!
            const char* text = getmsg(&pipboy_message_file, &pipmesg, 215);
            dialog_out(text, NULL, 0, 192, 135, colorTable[32328], 0, colorTable[32328], DIALOG_BOX_LARGE);
        }
    } else if (a1 >= 4 && a1 <= 17) {
        gsound_play_sfx_file("ib1p1xx1");

        DrawAlarmText(a1 - 3);

        int duration = a1 - 4;
        int minutes = 0;
        int hours = 0;

        switch (duration) {
        case PIPBOY_REST_DURATION_TEN_MINUTES:
            TimedRest(0, 10, 0);
            break;
        case PIPBOY_REST_DURATION_THIRTY_MINUTES:
            TimedRest(0, 30, 0);
            break;
        case PIPBOY_REST_DURATION_ONE_HOUR:
        case PIPBOY_REST_DURATION_TWO_HOURS:
        case PIPBOY_REST_DURATION_THREE_HOURS:
        case PIPBOY_REST_DURATION_FOUR_HOURS:
        case PIPBOY_REST_DURATION_FIVE_HOURS:
        case PIPBOY_REST_DURATION_SIX_HOURS:
            TimedRest(duration - 1, 0, 0);
            break;
        case PIPBOY_REST_DURATION_UNTIL_MORNING:
            ClacTime(&hours, &minutes, 6);
            TimedRest(hours, minutes, 0);
            break;
        case PIPBOY_REST_DURATION_UNTIL_NOON:
            ClacTime(&hours, &minutes, 12);
            TimedRest(hours, minutes, 0);
            break;
        case PIPBOY_REST_DURATION_UNTIL_EVENING:
            ClacTime(&hours, &minutes, 18);
            TimedRest(hours, minutes, 0);
            break;
        case PIPBOY_REST_DURATION_UNTIL_MIDNIGHT:
            ClacTime(&hours, &minutes, 0);
            if (TimedRest(hours, minutes, 0) == 0) {
                pip_num(0, 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
            }
            win_draw(pip_win);
            break;
        case PIPBOY_REST_DURATION_UNTIL_HEALED:
            TimedRest(0, 0, duration);
            break;
        }

        gsound_play_sfx_file("ib2lu1x1");

        DrawAlarmText(0);
    }
}

// 0x489028
static void DrawAlarmText(int a1)
{
    const char* text;

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (bottom_line >= 0) {
        cursor_line = 0;
    }

    // ALARM CLOCK
    text = getmsg(&pipboy_message_file, &pipmesg, 300);
    pip_print(text, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, colorTable[992]);

    if (bottom_line >= 5) {
        cursor_line = 5;
    }

    DrawAlrmHitPnts();

    // NOTE: I don't know if this +1 was a result of compiler optimization or it
    // was written like this in the first place.
    for (int option = 1; option < PIPBOY_REST_DURATION_COUNT_WITHOUT_PARTY + 1; option++) {
        // 302 - Rest for ten minutes
        // ...
        // 315 - Rest until party is healed
        text = getmsg(&pipboy_message_file, &pipmesg, 302 + option - 1);
        int color = option == a1 ? colorTable[32747] : colorTable[992];

        pip_print(text, 0, color);

        cursor_line++;
    }

    win_draw_rect(pip_win, &pip_rect);
}

// 0x489124
static void DrawAlrmHitPnts()
{
    int max_hp;
    int cur_hp;
    char* text;
    char msg[64];
    int len;

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + 66 * PIPBOY_WINDOW_WIDTH + 254,
        350,
        10,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + 66 * PIPBOY_WINDOW_WIDTH + 254,
        PIPBOY_WINDOW_WIDTH);

    max_hp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    cur_hp = critter_get_hits(obj_dude);
    text = getmsg(&pipboy_message_file, &pipmesg, 301); // Hit Points
    snprintf(msg, sizeof(msg), "%s %d/%d", text, cur_hp, max_hp);
    len = text_width(msg);
    text_to_buf(scrn_buf + 66 * PIPBOY_WINDOW_WIDTH + 254 + (350 - len) / 2, msg, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_WIDTH, colorTable[992]);
}

// 0x4891DC
static void NewFuncDsply()
{
    NixHotLines();
    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (bottom_line >= 0) {
        cursor_line = 0;
    }
}

// 0x48922C
static void AddHotLines(int start, int count, bool add_back_button)
{
    text_font(101);

    int height = text_height();

    hot_line_start = start;
    hot_line_count = count;

    // A new list always starts on its first entry -- that is the whole gesture: open a page, the
    // cursor is already on the first thing you could read.
    pip_pad_sel = 0;

    if (count != 0) {
        int y = start * height + PIPBOY_WINDOW_CONTENT_VIEW_Y;
        int eventCode = start + 505;
        for (int index = start; index < hot_line_count + hot_line_start && index < 20; index++) {
            HotLines[index] = win_register_button(pip_win,
                254,
                y,
                350,
                height,
                -1,
                -1,
                -1,
                eventCode,
                NULL,
                NULL,
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            y += height * 2;
            eventCode += 1;
        }
    }

    if (add_back_button) {
        HotLines[BACK_BUTTON_INDEX] = win_register_button(pip_win,
            254,
            height * bottom_line + PIPBOY_WINDOW_CONTENT_VIEW_Y,
            350,
            height,
            -1,
            -1,
            -1,
            528,
            NULL,
            NULL,
            NULL,
            BUTTON_FLAG_TRANSPARENT);
    }
}

// 0x48932C
static void NixHotLines()
{
    if (hot_line_count != 0) {
        int end = hot_line_start + hot_line_count;
        if (end > 20) {
            end = 20;
        }

        for (int index = hot_line_start; index < end; index++) {
            win_delete_button(HotLines[index]);
        }
    }

    if (hot_back_line) {
        win_delete_button(HotLines[BACK_BUTTON_INDEX]);
    }

    hot_line_count = 0;
    hot_back_line = 0;
}

// 0x48938C
static bool TimedRest(int hours, int minutes, int duration)
{
    gmouse_set_cursor(MOUSE_CURSOR_WAIT_WATCH);

    bool rc = false;

    if (duration == 0) {
        int hoursInMinutes = hours * 60;
        double v1 = (double)hoursInMinutes + (double)minutes;
        double v2 = v1 * (1.0 / 1440.0) * 3.5 + 0.25;
        double v3 = (double)minutes / v1 * v2;
        if (minutes != 0) {
            int gameTime = game_time();

            double v4 = v3 * 20.0;
            int v5 = 0;
            for (int v5 = 0; v5 < (int)v4; v5++) {
                sharedFpsLimiter.mark();

                if (rc) {
                    break;
                }

                unsigned int start = get_time();

                unsigned int v6 = (unsigned int)((double)v5 / v4 * ((double)minutes * 600.0) + (double)gameTime);
                unsigned int nextEventTime = queue_next_time();
                if (v6 >= nextEventTime) {
                    set_game_time(nextEventTime + 1);
                    if (queue_process()) {
                        rc = true;
                        debug_printf("PIPBOY: Returning from Queue trigger...\n");
                        proc_bail_flag = 1;
                        break;
                    }

                    if (game_user_wants_to_quit != 0) {
                        rc = true;
                    }
                }

                if (!rc) {
                    set_game_time(v6);
                    if (get_input() == KEY_ESCAPE || game_user_wants_to_quit != 0) {
                        rc = true;
                    }

                    pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
                    pip_date();
                    pip_note();
                    win_draw(pip_win);

                    while (elapsed_time(start) < 50) {
                    }
                }

                renderPresent();
                sharedFpsLimiter.throttle();
            }

            if (!rc) {
                set_game_time(gameTime + 600 * minutes);

                if (Check4Health(minutes)) {
                    // NOTE: Uninline.
                    AddHealth();
                }
            }

            pip_note();
            pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
            pip_date();
            DrawAlrmHitPnts();
            win_draw(pip_win);
        }

        if (hours != 0 && !rc) {
            int gameTime = game_time();
            double v7 = (v2 - v3) * 20.0;

            for (int hour = 0; hour < (int)v7; hour++) {
                sharedFpsLimiter.mark();

                if (rc) {
                    break;
                }

                unsigned int start = get_time();

                if (get_input() == KEY_ESCAPE || game_user_wants_to_quit != 0) {
                    rc = true;
                }

                unsigned int v8 = (unsigned int)((double)hour / v7 * (hours * GAME_TIME_TICKS_PER_HOUR) + gameTime);
                unsigned int nextEventTime = queue_next_time();
                if (!rc && v8 >= nextEventTime) {
                    set_game_time(nextEventTime + 1);

                    if (queue_process()) {
                        rc = true;
                        debug_printf("PIPBOY: Returning from Queue trigger...\n");
                        proc_bail_flag = 1;
                        break;
                    }

                    if (game_user_wants_to_quit != 0) {
                        rc = true;
                    }
                }

                if (!rc) {
                    set_game_time(v8);

                    int healthToAdd = (int)((double)hoursInMinutes / v7);
                    if (Check4Health(healthToAdd)) {
                        // NOTE: Uninline.
                        AddHealth();
                    }

                    pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
                    pip_date();
                    pip_note();
                    DrawAlrmHitPnts();
                    win_draw(pip_win);

                    while (elapsed_time(start) < 50) {
                    }
                }

                renderPresent();
                sharedFpsLimiter.throttle();
            }

            if (!rc) {
                set_game_time(gameTime + GAME_TIME_TICKS_PER_HOUR * hours);
            }

            pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
            pip_date();
            pip_note();
            DrawAlrmHitPnts();
            win_draw(pip_win);
        }
    } else if (duration == PIPBOY_REST_DURATION_UNTIL_HEALED) {
        int currentHp = critter_get_hits(obj_dude);
        int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
        if (currentHp != maxHp) {
            // First pass - healing dude is the top priority.
            int hpToHeal = maxHp - currentHp;
            int healingRate = stat_level(obj_dude, STAT_HEALING_RATE);
            int hoursToHeal = (int)((double)hpToHeal / (double)healingRate * 3.0);
            while (!rc && hoursToHeal != 0) {
                if (hoursToHeal <= 24) {
                    rc = TimedRest(hoursToHeal, 0, 0);
                    hoursToHeal = 0;
                } else {
                    rc = TimedRest(24, 0, 0);
                    hoursToHeal -= 24;
                }
            }

            // Second pass - attempt to heal delayed damage to dude (via poison
            // or radiation), and remaining party members. This process is
            // performed in 3 hour increments.
            currentHp = critter_get_hits(obj_dude);
            maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
            hpToHeal = maxHp - currentHp;

            while (!rc && hpToHeal != 0) {
                currentHp = critter_get_hits(obj_dude);
                maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
                hpToHeal = maxHp - currentHp;

                rc = TimedRest(3, 0, 0);
            }
        } else {
            // No one needs healing.
            gmouse_set_cursor(MOUSE_CURSOR_ARROW);
            return rc;
        }
    }

    int gameTime = game_time();
    int nextEventGameTime = queue_next_time();
    if (gameTime > nextEventGameTime) {
        if (queue_process()) {
            debug_printf("PIPBOY: Returning from Queue trigger...\n");
            proc_bail_flag = 1;
            rc = true;
        }
    }

    pip_num(game_time_hour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
    pip_date();
    pip_note();
    win_draw(pip_win);

    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    return rc;
}

// 0x4898E8
static bool Check4Health(int a1)
{
    rest_time += a1;

    if (rest_time < 180) {
        return false;
    }

    debug_printf("\n health added!\n");
    rest_time = 0;

    return true;
}

// 0x489924
static bool AddHealth()
{
    partyMemberRestingHeal(3);

    int currentHp = critter_get_hits(obj_dude);
    int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    return currentHp == maxHp;
}

// Returns [hours] and [minutes] needed to rest until [wakeUpHour].
//
// 0x489958
static void ClacTime(int* hours, int* minutes, int wakeUpHour)
{
    int gameTimeHour = game_time_hour();

    *hours = gameTimeHour / 100;
    *minutes = gameTimeHour % 100;

    if (*hours != wakeUpHour || *minutes != 0) {
        *hours = wakeUpHour - *hours;
        if (*hours < 0) {
            *hours += 24;
            if (*minutes != 0) {
                *hours -= 1;
                *minutes = 60 - *minutes;
            }
        } else {
            if (*minutes != 0) {
                *hours -= 1;
                *minutes = 60 - *minutes;
                if (*hours < 0) {
                    *hours = 23;
                }
            }
        }
    } else {
        *hours = 24;
    }
}

// 0x4899E4
static int ScreenSaver()
{
    PipboyBomb bombs[PIPBOY_BOMB_COUNT];

    mouseGetPositionInWindow(pip_win, &old_mouse_x, &old_mouse_y);

    for (int index = 0; index < PIPBOY_BOMB_COUNT; index += 1) {
        bombs[index].field_10 = 0;
    }

    gmouse_disable(0);

    unsigned char* buf = (unsigned char*)mem_malloc(412 * 374);
    if (buf == NULL) {
        return -1;
    }

    buf_to_buf(scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        buf,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH);

    buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    int v31 = 50;
    while (true) {
        sharedFpsLimiter.mark();

        unsigned int time = get_time();

        mouseGetPositionInWindow(pip_win, &mouse_x, &mouse_y);
        if (get_input() != -1 || old_mouse_x != mouse_x || old_mouse_y != mouse_y) {
            break;
        }

        double random = roll_random(0, PIPBOY_RAND_MAX);

        // TODO: Figure out what this constant means. Probably somehow related
        // to PIPBOY_RAND_MAX.
        if (random < 3047.3311) {
            int index = 0;
            for (; index < PIPBOY_BOMB_COUNT; index += 1) {
                if (bombs[index].field_10 == 0) {
                    break;
                }
            }

            if (index < PIPBOY_BOMB_COUNT) {
                PipboyBomb* bomb = &(bombs[index]);
                int v27 = (350 - ginfo[PIPBOY_FRM_BOMB].width / 4) + (406 - ginfo[PIPBOY_FRM_BOMB].height / 4);
                int v5 = (int)((double)roll_random(0, PIPBOY_RAND_MAX) / (double)PIPBOY_RAND_MAX * (double)v27);
                int v6 = ginfo[PIPBOY_FRM_BOMB].height / 4;
                if (PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT - v6 >= v5) {
                    bomb->x = 602;
                    bomb->y = v5 + 48;
                } else {
                    bomb->x = v5 - (PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT - v6) + PIPBOY_WINDOW_CONTENT_VIEW_X + ginfo[PIPBOY_FRM_BOMB].width / 4;
                    bomb->y = PIPBOY_WINDOW_CONTENT_VIEW_Y - ginfo[PIPBOY_FRM_BOMB].height + 2;
                }

                bomb->field_10 = 1;
                bomb->field_8 = (float)((double)roll_random(0, PIPBOY_RAND_MAX) * (2.75 / PIPBOY_RAND_MAX) + 0.15);
                bomb->field_C = 0;
            }
        }

        if (v31 == 0) {
            buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                PIPBOY_WINDOW_WIDTH,
                scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_WIDTH);
        }

        for (int index = 0; index < PIPBOY_BOMB_COUNT; index++) {
            PipboyBomb* bomb = &(bombs[index]);
            if (bomb->field_10 != 1) {
                continue;
            }

            int srcWidth = ginfo[PIPBOY_FRM_BOMB].width;
            int srcHeight = ginfo[PIPBOY_FRM_BOMB].height;
            int destX = bomb->x;
            int destY = bomb->y;
            int srcY = 0;
            int srcX = 0;

            if (destX >= PIPBOY_WINDOW_CONTENT_VIEW_X) {
                if (destX + ginfo[PIPBOY_FRM_BOMB].width >= 604) {
                    srcWidth = 604 - destX;
                    if (srcWidth < 1) {
                        bomb->field_10 = 0;
                    }
                }
            } else {
                srcX = PIPBOY_WINDOW_CONTENT_VIEW_X - destX;
                if (srcX >= ginfo[PIPBOY_FRM_BOMB].width) {
                    bomb->field_10 = 0;
                }
                destX = PIPBOY_WINDOW_CONTENT_VIEW_X;
                srcWidth = ginfo[PIPBOY_FRM_BOMB].width - srcX;
            }

            if (destY >= PIPBOY_WINDOW_CONTENT_VIEW_Y) {
                if (destY + ginfo[PIPBOY_FRM_BOMB].height >= 452) {
                    srcHeight = 452 - destY;
                    if (srcHeight < 1) {
                        bomb->field_10 = 0;
                    }
                }
            } else {
                if (destY + ginfo[PIPBOY_FRM_BOMB].height < PIPBOY_WINDOW_CONTENT_VIEW_Y) {
                    bomb->field_10 = 0;
                }

                srcY = PIPBOY_WINDOW_CONTENT_VIEW_Y - destY;
                srcHeight = ginfo[PIPBOY_FRM_BOMB].height - srcY;
                destY = PIPBOY_WINDOW_CONTENT_VIEW_Y;
            }

            if (bomb->field_10 == 1 && v31 == 0) {
                trans_buf_to_buf(
                    pipbmp[PIPBOY_FRM_BOMB] + ginfo[PIPBOY_FRM_BOMB].width * srcY + srcX,
                    srcWidth,
                    srcHeight,
                    ginfo[PIPBOY_FRM_BOMB].width,
                    scrn_buf + PIPBOY_WINDOW_WIDTH * destY + destX,
                    PIPBOY_WINDOW_WIDTH);
            }

            bomb->field_C += bomb->field_8;
            if (bomb->field_C >= 1.0) {
                bomb->x = (int)((float)bomb->x - bomb->field_C);
                bomb->y = (int)((float)bomb->y + bomb->field_C);
                bomb->field_C = 0.0;
            }
        }

        if (v31 != 0) {
            v31 -= 1;
        } else {
            win_draw_rect(pip_win, &pip_rect);
            while (elapsed_time(time) < 50) {
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    buf_to_buf(buf,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    mem_free(buf);

    win_draw_rect(pip_win, &pip_rect);
    gmouse_enable();

    return 0;
}

// 0x489EC8
static void pip_note()
{
    if (game_global_vars[GVAR_FIND_WATER_CHIP] == 2 || game_global_vars[GVAR_VAULT_WATER] == 0) {
        buf_to_buf(pipbmp[PIPBOY_FRM_BACKGROUND] + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_NOTE_Y + PIPBOY_WINDOW_NOTE_X,
            ginfo[PIPBOY_FRM_NOTE].width,
            ginfo[PIPBOY_FRM_NOTE].height,
            PIPBOY_WINDOW_WIDTH,
            scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_NOTE_Y + PIPBOY_WINDOW_NOTE_X,
            PIPBOY_WINDOW_WIDTH);
    } else {
        buf_to_buf(pipbmp[PIPBOY_FRM_NOTE],
            ginfo[PIPBOY_FRM_NOTE].width,
            ginfo[PIPBOY_FRM_NOTE].height,
            ginfo[PIPBOY_FRM_NOTE].width,
            scrn_buf + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_NOTE_Y + PIPBOY_WINDOW_NOTE_X,
            PIPBOY_WINDOW_WIDTH);

        pip_days_left(game_global_vars[GVAR_VAULT_WATER]);
    }
}

} // namespace fallout
