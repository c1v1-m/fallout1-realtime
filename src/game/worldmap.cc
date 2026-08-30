#include "game/worldmap.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game/anim.h"
#include "game/art.h"
#include "game/bmpdlog.h"
#include "game/combat.h"
#include "game/combatai.h"
#include "game/config.h"
#include "game/critter.h"

#include "game/cycle.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gamepad_ui.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gmovie.h"
#include "game/graphlib.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/game_trace.h"
#include "game/loadsave.h"
#include "game/item.h"
#include "game/map_defs.h"
#include "game/message.h"
#include "game/object.h"
#include "game/object_types.h"
#include "game/options.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/protinst.h"
#include "game/queue.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/tile.h"
#include "game/worldmap_walkmask.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/db/db.h"
#include "plib/gnw/button.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/memory.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define WM_WINDOW_WIDTH 640
#define WM_WINDOW_HEIGHT 480

#define WM_WORLDMAP_WIDTH 1400
// WorldTerraTable is 30 rows of 50 world units; the click path's max target Y
// works out to the same bound.
#define WM_WORLDMAP_HEIGHT 1500

// How far ahead of the party a left-stick steer projects its travel target.
#define WM_STICK_TRAVEL_LOOKAHEAD 50

#define LOCATION_MARKER_WIDTH 5
#define LOCATION_MARKER_HEIGHT 5

#define DESTINATION_MARKER_WIDTH 11
#define DESTINATION_MARKER_HEIGHT 11

#define RANDOM_ENCOUNTER_ICON_WIDTH 7
#define RANDOM_ENCOUNTER_ICON_HEIGHT 11

#define HOTSPOT_WIDTH 25
#define HOTSPOT_HEIGHT 13

#define DAY_X 487
#define DAY_Y 12
#define MONTH_X 513
#define MONTH_Y 12
#define YEAR_X 548
#define YEAR_Y 12
#define TIME_X 593
#define TIME_Y 12

#define VIEWPORT_MAX_X 950
#define VIEWPORT_MAX_Y 1058

typedef struct CityLocationEntry {
    int column;
    int row;
} CityLocationEntry;

typedef struct TownHotSpotEntry {
    short x;
    short y;
    short map_idx;
    char name[16];
} TownHotSpotEntry;

static void UpdVisualArea();
static int CheckEvents();
static int LoadTownMap(const char* filename, int map_idx);
static void TargetTown(int city);
static int InitWorldMapData();
static void UnInitWorldMapData();
static void UpdateTownStatus();
static int InCity(unsigned int x, unsigned int y);
static void world_move_init();
static int world_move_step();
static void block_map(unsigned int x, unsigned int y, unsigned char* dst);
static void DrawTownLabels(unsigned char* src, unsigned char* dst);
static void DrawMapTime(int is_town_map);
static void map_num(int value, int digits, int x, int y, int is_town_map);
static void HvrOffBtn(int a1, int a2);
static void TMapPadPark(int sel);
static void TMapPadStep(int dirX, int dirY, int count);
static int RegTMAPsels(int win, int city);
static void UnregTMAPsels(int count);
static void DrawTMAPsels(int win, int city);
static void CalcTimeAdder();
static void BlackOut();

// 0x4A9330
static const unsigned char mouse_table1[3][3][2] = {
    {
        { 1, 1 },
        { 0, 1 },
        { 1, 1 },
    },
    {
        { 1, 0 },
        { 0, 0 },
        { 1, 0 },
    },
    {
        { 1, 1 },
        { 0, 1 },
        { 1, 1 },
    },
};

// 0x4A9344
static const int mouse_table2[3][3][4] = {
    {
        { MOUSE_CURSOR_SCROLL_NW, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_N, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_NE, 0, 0, 0 },
    },
    {
        { MOUSE_CURSOR_SCROLL_W, 0, 0, 0 },
        { MOUSE_CURSOR_ARROW, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_E, 0, 0, 0 },
    },
    {
        { MOUSE_CURSOR_SCROLL_SW, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_S, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_SE, 0, 0, 0 },
    },
};

// 0x4A9368
static const int mouse_table3[3][3][4] = {
    {
        { MOUSE_CURSOR_SCROLL_NW_INVALID, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_N_INVALID, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_NE_INVALID, 0, 0, 0 },
    },
    {
        { MOUSE_CURSOR_SCROLL_W_INVALID, 0, 0, 0 },
        { MOUSE_CURSOR_ARROW, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_E_INVALID, 0, 0, 0 },
    },
    {
        { MOUSE_CURSOR_SCROLL_SW_INVALID, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_S_INVALID, 0, 0, 0 },
        { MOUSE_CURSOR_SCROLL_SE_INVALID, 0, 0, 0 },
    },
};

// 0x4A938C
static const int wmapids[WORLDMAP_FRM_COUNT] = {
    /*  LITTLE_RED_BUTTON_NORMAL */ 8,
    /* LITTLE_RED_BUTTON_PRESSED */ 9,
    /*                       BOX */ 136,
    /*                    LABELS */ 137,
    /*           LOCATION_MARKER */ 138,
    /* DESTINATION_MARKER_BRIGHT */ 139,
    /*   DESTINATION_MARKER_DARK */ 153,
    /*   RANDOM_ENCOUNTER_BRIGHT */ 154,
    /*     RANDOM_ENCOUNTER_DARK */ 155,
    /*                  WORLDMAP */ 135,
    /*                    MONTHS */ 129,
    /*                   NUMBERS */ 82,
    /*            HOTSPOT_NORMAL */ 168,
    /*           HOTSPOT_PRESSED */ 223,
};

// 0x4A93C4
static const int tmapids[TOWNMAP_FRM_COUNT] = {
    /*                       BOX */ 136,
    /*                    LABELS */ 137,
    /*           HOTSPOT_PRESSED */ 223,
    /*            HOTSPOT_NORMAL */ 168,
    /*  LITTLE_RED_BUTTON_NORMAL */ 8,
    /* LITTLE_RED_BUTTON_PRESSED */ 9,
    /*                    MONTHS */ 129,
    /*                   NUMBERS */ 82,
};

// 0x4A93E4
static const short BttnYtab[TOWN_COUNT] = {
    /*      VAULT 13 */ 61,
    /*      VAULT 15 */ 88,
    /*   SHADY SANDS */ 115,
    /*      JUNKTOWN */ 143,
    /*       RAIDERS */ 171,
    /*    NECROPOLIS */ 200,
    /*       THE HUB */ 228,
    /*   BROTHERHOOD */ 256,
    /* MILITARY BASE */ 283,
    /*      THE GLOW */ 310,
    /*      BONEYARD */ 338,
    /*     CATHEDRAL */ 367,
};

// 0x4A93FC
static const int OceanSeeXTable[30] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    2,
    3,
    4,
    5,
    5,
    5,
    5,
    8,
    11,
    12,
    14,
    14,
    17,
    18,
    19,
    19,
    20,
    20,
    20,
    21,
};

// 0x4A9474
static const struct {
    short start;
    short end;
} SpclEncRange[6] = {
    { 1, 30 },
    { 31, 50 },
    { 51, 70 },
    { 71, 80 },
    { 81, 90 },
    { 91, 100 },
};

// 0x4A948C
static const unsigned char WorldTerraTable[30][28] = {
    { 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0 },
    { 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0 },
    { 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 2 },
    { 3, 3, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 2, 0 },
    { 3, 3, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 2, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 0, 2, 2, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 3, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 1, 1, 1, 1, 1, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 1, 1, 1, 1, 1, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0, 0, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 1, 0, 0, 0 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0 },
};

// 0x4A97D4
static const unsigned char WorldEcountChanceTable[30][28] = {
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2, 2 },
    { 2, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 1, 2, 1, 1, 1, 1, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
    { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3 },
};

// 0x4A9B1C
static const unsigned char WorldEcounTable[30][28] = {
    // clang-format off
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  2,  2,  2,  2,  2,  2,  4,  4,  4,  2,  2,  5,  5,  5,  0,  0,  0,  0,  0 },
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  2,  2,  2,  2,  2,  2,  4,  4,  4,  4,  5,  5,  5,  5,  5,  0,  0,  0,  0 },
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  0,  2,  2,  2,  2,  2,  4,  4,  4,  4,  2,  5,  5,  5,  5,  2,  0,  0,  0 },
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  0,  0,  2,  2,  2,  2,  2,  4,  4,  4,  2,  0,  0,  6,  6,  2,  0,  0,  0 },
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  0,  0,  0,  2,  2,  2,  2,  4,  4,  2,  2,  0,  0,  6,  6,  2,  0,  0,  0 },
    { 11, 11, 11, 11, 11, 11, 11, 11, 11,  0,  0,  0,  2,  2,  2,  2,  0,  0,  0,  2,  2,  0,  2,  0,  0,  0,  0,  0 },
    { 15,  2,  2,  2,  2,  2,  0,  0,  0,  0,  0,  0,  0,  2,  2,  2,  0,  0,  0,  0,  2,  0,  0,  0,  0,  0,  0,  0 },
    { 15, 15,  2,  2,  2,  0,  0,  0,  0,  0, 10, 10, 10,  2,  2,  2,  2,  0,  0,  0,  2,  0,  2,  0,  0,  0,  0,  0 },
    { 15, 15, 15,  2,  0,  0,  0,  0,  0, 10, 10, 10, 10,  2,  0,  2,  2,  0,  0,  0,  2,  2,  2,  0,  0,  0,  0,  0 },
    { 15, 15, 15,  0,  0,  0,  0,  0,  0, 10, 10, 10, 10,  2,  2,  2,  7,  7,  7,  7,  0,  0,  0,  0,  0,  0,  0,  0 },
    { 15, 15, 15, 15,  0,  0,  0,  0,  0, 10, 10, 10, 10,  2,  2,  2,  7,  7,  7,  7,  0,  0,  0,  0,  0,  0,  0,  0 },
    { 15, 15, 15, 15, 15,  0,  0,  0,  0,  0, 10, 10, 10,  0,  2,  2,  7,  7,  7,  7,  7,  9,  9,  9,  9,  0,  0,  0 },
    { 15, 15, 15, 15, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  2,  0,  8,  8,  0,  9,  9,  9,  9,  9,  0,  0,  0 },
    { 15, 15, 15, 15, 15, 15,  0,  2,  0,  0,  0,  0,  0,  0,  2, 14,  8,  8,  8,  0,  9,  9,  9,  9,  9,  0,  0,  0 },
    { 15, 15, 15, 15, 15, 15,  0,  2,  0,  0,  0,  0,  0,  0, 14, 14,  8,  8,  8,  0,  9,  9,  9,  9,  9,  0,  0,  0 },
    { 15, 15, 15, 15, 15, 15,  1,  1,  1,  3,  1,  1,  1,  1,  1, 14,  8,  8,  8,  8,  8,  9,  9,  9,  9, 10,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15,  1,  1,  3,  3,  3, 13, 13, 13, 13, 13, 13, 13, 13,  1,  9,  9,  9,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  1, 13, 13, 13, 13, 13, 13, 13, 13,  1,  1,  1,  1,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,  3,  1,  1,  1,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 13, 13, 13, 13, 13, 13, 13,  3,  1,  1,  1,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 13, 13, 15, 13, 13, 13,  3,  3,  3,  1,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  1,  3,  3,  3,  3,  3,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  1,  3,  3,  3,  3,  3,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  3,  3,  3,  3,  3,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  3,  3,  3, 12, 12, 12, 12,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3,  3,  3, 12, 12, 12, 12,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3,  3, 12, 12, 12, 12,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3, 12, 12, 12, 12, 12,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1, 12, 12,  3,  1,  1,  1 },
    { 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  1,  1,  1,  1,  1,  1,  1 },
    // clang-format on
};

// 0x4A9E64
static const CityLocationEntry city_location[TOWN_COUNT] = {
    /*      VAULT 13 */ { 16, 1 },
    /*      VAULT 15 */ { 25, 1 },
    /*   SHADY SANDS */ { 21, 1 },
    /*      JUNKTOWN */ { 17, 10 },
    /*       RAIDERS */ { 22, 3 },
    /*    NECROPOLIS */ { 22, 13 },
    /*       THE HUB */ { 17, 14 },
    /*   BROTHERHOOD */ { 12, 9 },
    /* MILITARY BASE */ { 3, 1 },
    /*      THE GLOW */ { 24, 25 },
    /*      BONEYARD */ { 15, 18 },
    /*     CATHEDRAL */ { 15, 20 },
};

// 0x4A9E94
static const short cityXgvar[12] = {
    /*      VAULT 13 */ 67,
    /*      VAULT 15 */ 70,
    /*   SHADY SANDS */ 68,
    /*      JUNKTOWN */ 71,
    /*       RAIDERS */ 69,
    /*    NECROPOLIS */ 72,
    /*       THE HUB */ 73,
    /*   BROTHERHOOD */ 74,
    /* MILITARY BASE */ 78,
    /*      THE GLOW */ 76,
    /*      BONEYARD */ 75,
    /*     CATHEDRAL */ 77,
};

// 0x4A9EAC
static const short ElevXgvar[12][7] = {
    /*      VAULT 13 */ { 558, 559, 560, 561, 0, 0, 0 },
    /*      VAULT 15 */ { 562, 563, 564, 565, 0, 0, 0 },
    /*   SHADY SANDS */ { 566, 567, 568, 0, 0, 0, 0 },
    /*      JUNKTOWN */ { 569, 570, 571, 0, 0, 0, 0 },
    /*       RAIDERS */ { 572, 0, 0, 0, 0, 0, 0 },
    /*    NECROPOLIS */ { 573, 574, 575, 0, 0, 0, 0 },
    /*       THE HUB */ { 576, 577, 578, 579, 580, 581, 0 },
    /*   BROTHERHOOD */ { 582, 583, 584, 585, 586, 0, 0 },
    /* MILITARY BASE */ { 587, 588, 589, 590, 591, 0, 0 },
    /*      THE GLOW */ { 592, 593, 0, 0, 0, 0, 0 },
    /*      BONEYARD */ { 594, 595, 596, 597, 598, 0, 0 },
    /*     CATHEDRAL */ { 599, 600, 0, 0, 0, 0, 0 },
};

// 0x4A9F54
static const short xlate_town_table[MAP_COUNT] = {
    /*  DESERT1 */ TOWN_VAULT_15,
    /*  DESERT2 */ TOWN_VAULT_15,
    /*  DESERT3 */ TOWN_VAULT_15,
    /*  HALLDED */ TOWN_NECROPOLIS,
    /*    HOTEL */ TOWN_NECROPOLIS,
    /*  WATRSHD */ TOWN_NECROPOLIS,
    /*  VAULT13 */ TOWN_VAULT_13,
    /* VAULTENT */ TOWN_VAULT_15,
    /* VAULTBUR */ TOWN_VAULT_15,
    /* VAULTNEC */ TOWN_NECROPOLIS,
    /*  JUNKENT */ TOWN_JUNKTOWN,
    /* JUNKCSNO */ TOWN_JUNKTOWN,
    /* JUNKKILL */ TOWN_JUNKTOWN,
    /* BROHDENT */ TOWN_BROTHERHOOD,
    /*  BROHD12 */ TOWN_BROTHERHOOD,
    /*  BROHD34 */ TOWN_BROTHERHOOD,
    /*    CAVES */ TOWN_SHADY_SANDS,
    /* CHILDRN1 */ TOWN_CATHEDRAL,
    /* CHILDRN2 */ TOWN_CATHEDRAL,
    /*    CITY1 */ TOWN_VAULT_15,
    /*   COAST1 */ TOWN_VAULT_15,
    /*   COAST2 */ TOWN_VAULT_15,
    /* COLATRUK */ TOWN_VAULT_15,
    /*  FSAUSER */ TOWN_VAULT_15,
    /*  RAIDERS */ TOWN_RAIDERS,
    /*   SHADYE */ TOWN_SHADY_SANDS,
    /*   SHADYW */ TOWN_SHADY_SANDS,
    /*  GLOWENT */ TOWN_THE_GLOW,
    /* LAADYTUM */ TOWN_BONEYARD,
    /* LAFOLLWR */ TOWN_BONEYARD,
    /*    MBENT */ TOWN_MILITARY_BASE,
    /* MBSTRG12 */ TOWN_MILITARY_BASE,
    /* MBVATS12 */ TOWN_MILITARY_BASE,
    /* MSTRLR12 */ TOWN_CATHEDRAL,
    /* MSTRLR34 */ TOWN_CATHEDRAL,
    /*   V13ENT */ TOWN_VAULT_13,
    /*   HUBENT */ TOWN_THE_HUB,
    /* DETHCLAW */ TOWN_THE_HUB,
    /* HUBDWNTN */ TOWN_THE_HUB,
    /* HUBHEIGT */ TOWN_THE_HUB,
    /* HUBOLDTN */ TOWN_THE_HUB,
    /* HUBWATER */ TOWN_THE_HUB,
    /*    GLOW1 */ TOWN_THE_GLOW,
    /*    GLOW2 */ TOWN_THE_GLOW,
    /* LABLADES */ TOWN_BONEYARD,
    /* LARIPPER */ TOWN_BONEYARD,
    /* LAGUNRUN */ TOWN_BONEYARD,
    /* CHILDEAD */ TOWN_CATHEDRAL,
    /*   MBDEAD */ TOWN_MILITARY_BASE,
    /*  MOUNTN1 */ TOWN_VAULT_15,
    /*  MOUNTN2 */ TOWN_VAULT_15,
    /*     FOOT */ TOWN_VAULT_15,
    /*   TARDIS */ TOWN_VAULT_15,
    /*  TALKCOW */ TOWN_VAULT_15,
    /*  USEDCAR */ TOWN_VAULT_15,
    /*  BRODEAD */ TOWN_BROTHERHOOD,
    /* DESCRVN1 */ TOWN_VAULT_15,
    /* DESCRVN2 */ TOWN_VAULT_15,
    /* MNTCRVN1 */ TOWN_VAULT_15,
    /* MNTCRVN2 */ TOWN_VAULT_15,
    /*   VIPERS */ TOWN_VAULT_15,
    /* DESCRVN3 */ TOWN_VAULT_15,
    /* MNTCRVN3 */ TOWN_VAULT_15,
    /* DESCRVN4 */ TOWN_VAULT_13,
    /* MNTCRVN4 */ TOWN_VAULT_13,
    /*  HUBMIS1 */ TOWN_VAULT_13,
};

// 0x5392B4
static int bk_enable = 0;

// 0x5392B8
static int reselect = 0;

// 0x5392BC
static unsigned char tbutntgl = 0;

// 0x5392C0
static const char* RandEnctNames[4][3] = {
    {
        "DESERT1.MAP",
        "DESERT2.MAP",
        "DESERT3.MAP",
    },
    {
        "MOUNTN1.MAP",
        "MOUNTN2.MAP",
        NULL,
    },
    {
        "CITY1.MAP",
        NULL,
        NULL,
    },
    {
        "COAST1.MAP",
        "COAST2.MAP",
        NULL,
    }
};

// 0x5392F0
static const char* spcl_map_name[6] = {
    "FOOT.MAP",
    "TALKCOW.MAP",
    "USEDCAR.MAP",
    "TARDIS.MAP",
    "FSAUSER.MAP",
    "COLATRUK.MAP",
};

// 0x539308
static const char* CityMusic[MAP_COUNT] = {
    /*  DESERT1 */ "07DESERT",
    /*  DESERT2 */ "07DESERT",
    /*  DESERT3 */ "07DESERT",
    /*  HALLDED */ "14NECRO",
    /*    HOTEL */ "14NECRO",
    /*  WATRSHD */ "14NECRO",
    /*  VAULT13 */ "06VAULT",
    /* VAULTENT */ "13CARVRN",
    /* VAULTBUR */ "13CARVRN",
    /* VAULTNEC */ "14NECRO",
    /*  JUNKENT */ "12JUNKTN",
    /* JUNKCSNO */ "12JUNKTN",
    /* JUNKKILL */ "12JUNKTN",
    /* BROHDENT */ "04BRTHRH",
    /*  BROHD12 */ "04BRTHRH",
    /*  BROHD34 */ "04BRTHRH",
    /*    CAVES */ "13CARVRN",
    /* CHILDRN1 */ "11CHILRN",
    /* CHILDRN2 */ "11CHILRN",
    /*    CITY1 */ "11CHILRN",
    /*   COAST1 */ "07DESERT",
    /*   COAST2 */ "07DESERT",
    /* COLATRUK */ "07DESERT",
    /*  FSAUSER */ "07DESERT",
    /*  RAIDERS */ "05RAIDER",
    /*   SHADYE */ "15SHADY",
    /*   SHADYW */ "15SHADY",
    /*  GLOWENT */ "09GLOW",
    /* LAADYTUM */ "10LABONE",
    /* LAFOLLWR */ "16FOLLOW",
    /*    MBENT */ "08VATS",
    /* MBSTRG12 */ "08VATS",
    /* MBVATS12 */ "08VATS",
    /* MSTRLR12 */ "02MSTRLR",
    /* MSTRLR34 */ "02MSTRLR",
    /*   V13ENT */ "06VAULT",
    /*   HUBENT */ "01HUB",
    /* DETHCLAW */ "13CARVRN",
    /* HUBDWNTN */ "01HUB",
    /* HUBHEIGT */ "01HUB",
    /* HUBOLDTN */ "01HUB",
    /* HUBWATER */ "01HUB",
    /*    GLOW1 */ "09GLOW",
    /*    GLOW2 */ "09GLOW",
    /* LABLADES */ "10LABONE",
    /* LARIPPER */ "10LABONE",
    /* LAGUNRUN */ "10LABONE",
    /* CHILDEAD */ "08VATS",
    /*   MBDEAD */ "08VATS",
    /*  MOUNTN1 */ "07DESERT",
    /*  MOUNTN2 */ "07DESERT",
    /*     FOOT */ "07DESERT",
    /*   TARDIS */ "07DESERT",
    /*  TALKCOW */ "07DESERT",
    /*  USEDCAR */ "07DESERT",
    /*  BRODEAD */ "08VATS",
    /* DESCRVN1 */ "07DESERT",
    /* DESCRVN2 */ "07DESERT",
    /* MNTCRVN1 */ "07DESERT",
    /* MNTCRVN2 */ "07DESERT",
    /*   VIPERS */ "07DESERT",
    /* DESCRVN3 */ "07DESERT",
    /* MNTCRVN3 */ "07DESERT",
    /* DESCRVN4 */ "07DESERT",
    /* MNTCRVN4 */ "07DESERT",
    /*  HUBMIS1 */ "01HUB",
};

// 0x539410
static TownHotSpotEntry TownHotSpots[15][7] = {
    {
        { 202, 303, 0, "V13ENT.MAP" },
        { 271, 282, 1, "VAULT13.MAP" },
        { 292, 237, 2, "VAULT13.MAP" },
        { 309, 204, 3, "VAULT13.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 68, 250, 0, "VAULTENT.MAP" },
        { 107, 209, 1, "VAULTBUR.MAP" },
        { 298, 187, 2, "VAULTBUR.MAP" },
        { 135, 290, 3, "VAULTBUR.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 158, 192, 1, "SHADYW.MAP" },
        { 270, 253, 2, "SHADYE.MAP" },
        { 314, 217, 3, "SHADYE.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 400, 317, 3, "JUNKENT.MAP" },
        { 304, 257, 2, "JUNKKILL.MAP" },
        { 200, 279, 1, "JUNKCSNO.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 241, 398, 1, "RAIDERS.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 398, 265, 3, "HOTEL.MAP" },
        { 239, 224, 2, "HALLDED.MAP" },
        { 79, 207, 1, "WATRSHD.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 238, 78, 1, "HUBENT.MAP" },
        { 205, 172, 3, "HUBDWNTN.MAP" },
        { 128, 138, 5, "HUBHEIGT.MAP" },
        { 306, 137, 2, "HUBOLDTN.MAP" },
        { 272, 238, 4, "HUBWATER.MAP" },
        { 125, 216, 0, "DETHCLAW.MAP" },
        { 0, 0, 0, "" },
    },
    {
        { 172, 167, 1, "BROHDENT.MAP" },
        { 254, 194, 2, "BROHD12.MAP" },
        { 136, 263, 3, "BROHD12.MAP" },
        { 280, 306, 4, "BROHD34.MAP" },
        { 161, 373, 5, "BROHD34.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 197, 83, 0, "MBENT.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 340, 149, 0, "GLOWENT.MAP" },
        { 334, 195, 1, "GLOW1.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 276, 239, 3, "LAADYTUM.MAP" },
        { 229, 195, 4, "LABLADES.MAP" },
        { 179, 185, 5, "LAFOLLWR.MAP" },
        { 346, 114, 1, "LAGUNRUN.MAP" },
        { 285, 159, 2, "LARIPPER.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 86, 328, 0, "CHILDRN1.MAP" },
        { 229, 313, 1, "CHILDRN1.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 235, 301, 0, "CHILDEAD.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 106, 64, 0, "MBDEAD.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
    {
        { 172, 167, 0, "BRODEAD.MAP" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
        { 0, 0, 0, "" },
    },
};

// 0x66FDE0
static int TMSelBttns[8];

// 0x66FE00
static struct {
    short x;
    short y;
    int bid;
} brnpos[7];

// 0x66FE38
static MessageList wrldmap_mesg_file;

// 0x66FE40
static unsigned char hvrtxtbuf[4096];

// 0x670E40
static int tcode_xref[8];

// 0x670E48
static CacheEntry* wmapidsav[WORLDMAP_FRM_COUNT];

// 0x670E80
static unsigned char* hvrbtn[8];

// 0x670EA0
static int TownBttns[TOWN_COUNT];

// 0x670ED0
static unsigned char* tmapbmp[TOWNMAP_FRM_COUNT];

// 0x670EF4
static MessageListItem mesg;

// 0x670F00
static unsigned char* sea_mask;

// 0x670F04
static unsigned char* world_buf;

// 0x670F08
static unsigned char* line1bit_buf;

// 0x670F0C
static int encounter_specials;

// 0x670F10
static int line_error;

// 0x670F14
static unsigned char* wmapbmp[WORLDMAP_FRM_COUNT];

// 0x670F50
static int time_adder;

// 0x670F54
static unsigned char* tmap_pic;

// 0x670F58
static unsigned char* onbtn;

// 0x670F5C
static int bx_enable;

// 0x670F60
static int first_visit_flag;

// CE: town currently picked by the D-pad in the world map list, -1 = none yet. File scope (not a
// local static) so it is cleared on every entry to the screen -- a stale pick from the last visit
// would put the cursor somewhere the player did not choose.
static int wm_pad_town = -1;

// CE: entrance currently picked by the pad on the TOWN MAP (index into brnpos[], -1 = none yet).
//
// ⚠️ THIS SCREEN USED TO LEAK THE GAMEPLAY A-CLICK -- the third screen in this family, after the
// main menu and Preferences. town_map() never set gpui_self_nav, so gpui_action_buttons ran its
// full GAMEPLAY A-router here: no menu, no dialog, no lock, no hostile in range => a_interacting is
// true, the world interact finds nothing to do on a map screen, and the press falls through as a
// plain synthetic left click at the cursor. The right stick meanwhile creeps that cursor freely, so
// it lands on an entrance hotspot and the town opens by itself -- the user's report was "I could
// not LEAVE, moving the stick opened Shady Sands as if I were pressing A".
//
// The fix is the same shape as every other screen in this family: own the pad (self_nav) and drive
// the selection explicitly. See [[selfnav-click-leak]] for why gpui_action_buttons must still be
// CALLED under self_nav (its press edges have to stay fresh, or the A that closes this screen fires
// into the world on the next frame).
static int tm_pad_sel = -1;


// CE: TRAVELLING WHILE POISONED IS REFUSED OUTRIGHT.
//
// Why, with the numbers (measured 2026-08-24). Each poison tick is exactly -1 HP
// (critter_check_poison) and re-queues itself `505 - 5 * poison` SECONDS later -- about 7.5 game-
// minutes at poison 12. One step of world-map travel advances `864000 / wmap_day` = 12 to 24 game-
// MINUTES. So every single travel step has at least one, usually two or three, poison ticks due,
// and the loop performs one per rendered frame: the player watched 11 HP fall to 5 in a fraction of
// a second ("insta drop, why?"). That is vanilla arithmetic -- it simply never showed, because
// nobody sets off across the map on 11 HP with poison in them.
//
// It got worse from there: at HP <= 5 critter_check_poison returns 1, queue_process passes that up,
// and the world map reads it as "stop travelling" -- but if you happen to be standing on a town it
// pushes you INTO the town instead of just stopping. Leave, tick, get shoved back in. That was the
// "I couldn't leave Shady Sands" report.
//
// Refusing the journey up front replaces both of those with one honest message. The player deals
// with the poison first, which is what the HP<=5 interrupt was clumsily trying to say anyway.
//
// ⚠️ THE REFUSAL MUST DROP YOU INTO A MAP, NEVER JUST STOP YOU. World-map time only advances while
// you are MOVING -- every set_game_time / queue_process in this loop sits inside `if (is_moving)`
// -- and the screen natively offers no Pipboy and no inventory (its keys are A, P=pause, F12, quit,
// gamma). So a poisoned player who is merely stopped out here can never rest, heal, cure, or tick
// the poison down: that is an unwinnable save, not a difficulty spike. Landing him in the town he
// is standing on (or a random terrain map when he is not) puts him somewhere the clock runs and the
// Pipboy exists.
//
// ⚠️ Making the Pipboy itself reachable from the world map was tried first (keyboard Z / pad Y). It
// was blamed for a crash and reverted -- WRONGLY, as it turned out: that crash was display_print
// being handed a string LITERAL by the message below (it word-wraps in place, so it wrote into
// read-only memory). The Pipboy idea was never actually disproven. It is also no longer needed, now
// that the refusal only fires when the trip would be lethal.
// ⚠️ THE TEST IS "WOULD THE TRIP KILL YOU", NOT "ARE YOU POISONED". The first version refused on
// ANY poison at all, which was plainly wrong and he said so at 30 HP: *"normálně bych mohl pak odejít
// a regenil bych se při travellingu jako ve vanilla."* Both halves of that are right --
//
//   * poison is FINITE. Each tick is -2 poison and -1 HP (critter_check_poison), so a poisoning of
//     N costs exactly ceil(N/2) HP in total and then it is gone. At 30 HP that is a scratch.
//   * travelling DOES heal. world_map calls partyMemberRestingHeal(24) once per travelled game day
//     (worldmap.cc:1838) -- I had claimed the world map offers no healing, and that was wrong.
//
// So the refusal is scoped to the case it was actually built for: the trip ENDING at the vanilla
// HP<=5 queue trigger, which stops the journey and shoves you into whatever town you are standing
// on -- the "I can't leave Shady Sands" loop. Refuse exactly those, allow everything else, and the
// healing above is ignored on purpose (it lands in one lump per travelled day, so counting on it
// would be optimistic in precisely the short hops that matter).
static bool WMapPoisonBlocksTravel()
{
    if (obj_dude == NULL) {
        return false;
    }

    int poison = critter_get_poison(obj_dude);
    if (poison <= 0) {
        return false;
    }

    // Total HP the remaining poison still has to take, and where that leaves you.
    int remaining = (poison + 1) / 2;
    return critter_get_hits(obj_dude) - remaining <= 5;
}

// 0x670F64
static int deltaLineY;

// 0x670F68
static int deltaLineX;

// 0x670F6C
static int line_index;

// 0x670F70
static int WrldToggle;

// 0x670F74
static unsigned char* btnmsk;

// 0x670F78
static int y_line_inc;

// 0x670F7C
static int x_line_inc;

// 0x670F80
static unsigned char* offbtn;

// 0x670F88
static CacheEntry* tmapidsav[TOWNMAP_FRM_COUNT];

// 0x670F84
static unsigned int wmap_day;

// 0x670FAC
static int target_xpos;

// 0x670FB0
static int target_ypos;

// 0x670FB4
static unsigned int wmap_mile;

// 0x670FB8
static int old_world_xpos;

// 0x670FBC
static int old_world_ypos;

// 0x670FC0
static int dropbtn;

// 0x670FC4
int our_section;

// 0x670FC8
int our_town;

// 0x670FCC
static int world_xpos;

// 0x670FD0
static int world_ypos;

// 0x670FD4
// ⚠️ -1 WHEN NO WINDOW EXISTS, and that is what KillWorldWin keys off. It used to key off
// a wwin_flag that a LOAD silently cleared -- see KillWorldWin.
int world_win = -1;

// 0x670FD8
static unsigned char TwnSelKnwFlag[15][7];

// 0x671041
static unsigned char WorldGrid[31][29];


// 0x4AA110
int init_world_map()
{
    int column;
    int row;

    for (row = 0; row < 29; row++) {
        for (column = 0; column < 28; column++) {
            WorldGrid[row][column] = 0;
        }
    }

    memset(TwnSelKnwFlag, 0, sizeof(TwnSelKnwFlag));

    encounter_specials = 0;
    first_visit_flag = 0;
    world_xpos = 50 * city_location[TOWN_VAULT_13].column + 50 / 2;
    world_ypos = 50 * city_location[TOWN_VAULT_13].row + 50 / 2;
    our_town = 0;
    our_section = 1;
    first_visit_flag |= 1;
    TwnSelKnwFlag[TOWN_VAULT_13][0] = 1;

    return 0;
}

// 0x4AA1C0
int save_world_map(DB_FILE* stream)
{
    if (db_fwrite(WorldGrid, sizeof(WorldGrid), 1, stream) != 1) return -1;
    if (db_fwrite(TwnSelKnwFlag, sizeof(TwnSelKnwFlag), 1, stream) != 1) return -1;
    if (db_fwriteInt32(stream, first_visit_flag) == -1) return -1;
    if (db_fwriteInt32(stream, encounter_specials) == -1) return -1;
    if (db_fwriteInt32(stream, our_town) == -1) return -1;
    if (db_fwriteInt32(stream, our_section) == -1) return -1;
    if (db_fwriteInt32(stream, world_xpos) == -1) return -1;
    if (db_fwriteInt32(stream, world_ypos) == -1) return -1;

    return 0;
}

// 0x4AA280
int load_world_map(DB_FILE* stream)
{
    if (db_fread(WorldGrid, sizeof(WorldGrid), 1, stream) != 1) return -1;
    if (db_fread(TwnSelKnwFlag, sizeof(TwnSelKnwFlag), 1, stream) != 1) return -1;
    if (db_freadInt32(stream, &first_visit_flag) == -1) return -1;
    if (db_freadInt32(stream, &encounter_specials) == -1) return -1;
    if (db_freadInt32(stream, &our_town) == -1) return -1;
    if (db_freadInt32(stream, &our_section) == -1) return -1;
    if (db_freadInt32(stream, &world_xpos) == -1) return -1;
    if (db_freadInt32(stream, &world_ypos) == -1) return -1;

    return 0;
}

// NOTE: It's the biggest function in Fallout 1 and Fallout 2 containing more
// than 9000 instructions.
//
// 0x4AA360
int world_map(WorldMapContext ctx)
{
    // TEMP DIAG: did we get in at all? STRIP with wm-armed.
    trace_event(TRACE_FLOW, "wm-enter", "state=%d town=%d", ctx.state, ctx.town);
    const char* title;
    const char* text;
    const char* body[1];
    int index;
    int rc;
    int viewport_x;
    int viewport_y;
    unsigned int time;
    int input;
    int abs_mouse_x;
    int abs_mouse_y;
    int mouse_x;
    int mouse_y;
    int mouse_dx;
    int mouse_dy;
    int hover;
    int should_redraw;
    int done;
    int is_entering_townmap;
    int autofollow;
    int is_entering_city;
    int is_entering_random_encounter;
    int is_entering_random_terrain;
    int terrain;
    int map_index;
    int scroll_dx;
    int scroll_dy;
    int scroll_invalid;
    int scroll_invalid_x;
    int scroll_invalid_y;
    int candidate_viewport_x;
    int candidate_viewport_y;
    int is_moving;
    int temp_x;
    int temp_y;
    int temp_town;
    int entering_city;
    int is_moving_to_town;
    int move_counter;
    unsigned int next_event_time;
    unsigned int new_game_time;
    int random_enc_chance;
    int travel_line_cycle;
    int iso_was_disabled;
    int v109;
    int v142;
    int special_enc_chance;
    int special_enc_num;
    int location_name_width;
    int location_name_x;
    int location_name_y;
    int hover_text_x;
    int hover_text_width;

    title = "";
    text = getmsg(&map_msg_file, &mesg, 1000);
    body[0] = text;

    if (map_save_in_game(true) == -1) {
        debug_printf("\nWORLD MAP: ** Error saving map! **\n");
        gmouse_disable(0);
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        gsound_play_sfx_file("iisxxxx1");
        dialog_out(title, body, 1, 169, 116, colorTable[32328], NULL, colorTable[32328], DIALOG_BOX_LARGE);
        gmouse_enable();
        game_user_wants_to_quit = 2;
        return -1;
    }

    iso_was_disabled = 0;

    soundUpdate();

    is_moving = 0;
    // Left stick must read neutral once before it steers, so a stick still held forward from
    // the map we just left (the exit-grid walk) doesn't auto-travel the party the instant the
    // world map opens. Armed below the first frame the stick is in its deadzone.
    int wm_stick_armed = 0;
    reselect = 0;
    dropbtn = 1;
    wmap_mile = 0;
    CalcTimeAdder();
    art_flush();

    should_redraw = 1;
    wm_pad_town = -1; // fresh pick per visit to this screen
    // Which game we came in on. If a save is loaded while this loop is running, everything below
    // is drawing over a world that no longer exists -- see the bail-out beside the quit check.
    unsigned int wm_load_serial = LoadGameSerial();
    done = 0;
    autofollow = 1;
    is_entering_townmap = 0;
    is_entering_city = 0;
    is_entering_random_encounter = 0;
    is_entering_random_terrain = 0;
    entering_city = 0;
    special_enc_num = 0;
    is_moving_to_town = 0;
    move_counter = 0;
    travel_line_cycle = 0;

    if (game_user_wants_to_quit != 0) {
        ctx.state = 1;
    }

    switch (ctx.state) {
    case -1:
        return -1;
    case 0:
    case 3:
        reselect = 0;
        break;
    case 1:
        intface_update_hit_points(false);
        return 0;
    case 2:
        if (InCity(world_xpos, world_ypos) == ctx.town) {
            our_town = ctx.town;
            our_section = ctx.section;
            reselect = 0;

            for (index = 0; index < TOWN_COUNT; index++) {
                win_disable_button(TownBttns[index]);
            }
            win_disable_button(WrldToggle);
            rc = LoadTownMap(TownHotSpots[ctx.town][ctx.section].name, TownHotSpots[ctx.town][ctx.section].map_idx);
            if (rc == -1) {
                return -1;
            }

            intface_update_hit_points(false);

            return 0;
        }

        reselect = 1;
        TargetTown(ctx.town);
        entering_city = ctx.town;
        is_moving_to_town = 1;
        is_moving = 1;
        our_section = ctx.section;
        break;
    }

    while (1) {
        if (InitWorldMapData() == -1) {
            // TEMP DIAG: a failed init leaves the caller with no screen at all, which looks
            // exactly like the black screen being chased. STRIP with wm-armed.
            trace_event(TRACE_WARN, "wm-init-fail", "state=%d town=%d", ctx.state, ctx.town);
            return -1;
        }

        viewport_x = world_xpos - 247;
        viewport_y = world_ypos - 242;

        if (viewport_x < 0) {
            viewport_x = 0;
        } else if (viewport_x > VIEWPORT_MAX_X) {
            viewport_x = VIEWPORT_MAX_X;
        }

        if (viewport_y < 0) {
            viewport_y = 0;
        } else if (viewport_y > VIEWPORT_MAX_Y) {
            viewport_y = VIEWPORT_MAX_Y;
        }

        buf_to_buf(wmapbmp[WORLDMAP_FRM_WORLDMAP] + WM_WORLDMAP_WIDTH * viewport_y + viewport_x,
            450,
            442,
            WM_WORLDMAP_WIDTH,
            world_buf + WM_WINDOW_WIDTH * 21 + 22,
            WM_WINDOW_WIDTH);
        UpdVisualArea();
        block_map(viewport_x, viewport_y, world_buf);
        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX],
            WM_WINDOW_WIDTH,
            WM_WINDOW_HEIGHT,
            WM_WINDOW_WIDTH,
            world_buf,
            WM_WINDOW_WIDTH);
        DrawTownLabels(wmapbmp[WORLDMAP_FRM_LABELS], world_buf);

        temp_x = world_xpos - viewport_x + 10;
        temp_y = world_ypos - viewport_y + 15;
        if (temp_x > -3 && temp_x < 484 && temp_y > 8 && temp_y < 463) {
            trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_HOTSPOT_NORMAL],
                temp_x > 460 ? 485 - temp_x : HOTSPOT_WIDTH,
                HOTSPOT_HEIGHT,
                HOTSPOT_WIDTH,
                world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                WM_WINDOW_WIDTH);
        }

        DrawMapTime(0);
        win_draw(world_win);
        renderPresent();

        if (!iso_was_disabled) {
            // CE: Hide interface.
            intface_hide();
            win_fill(display_win,
                0,
                0,
                win_width(display_win),
                win_height(display_win),
                colorTable[0]);
            win_draw(display_win);

            bk_enable = map_disable_bk_processes();
            cycle_disable();
            iso_was_disabled = 1;
        }

        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        gsound_background_play_level_music("03WRLDMP", 12);

        hover = 0;

        while (!done) {
            sharedFpsLimiter.mark();

            if (is_entering_random_encounter || is_entering_city || is_entering_random_terrain) {
                break;
            }
            time = get_time();
            input = get_input();

            // A SAVE WAS LOADED OUT FROM UNDER US, INSIDE THAT get_input().
            //
            // Vanilla's world map has no menu at all (its key handler knows only A, P, F12, quit
            // and gamma), so Load was unreachable from here -- but our pad's gameplay shortcut
            // layer runs inside this very loop (get_input -> process_bk -> mouse_info -> the hook)
            // and START opens the pause menu. `intface_hide()` sets only intfaceHidden, never
            // intfaceEnabled, so the shortcut layer's !intface_is_enabled() guard does not apply
            // here. Loading replaces the entire game while this loop is still on the stack, and it
            // carried on drawing over it.
            //
            // ⚠️ THE TEST HAS TO BE HERE, NOT AT THE BOTTOM OF THE BODY. Placed after the input
            // handling it still worked, but the rest of the frame ran first -- and by then
            // world_xpos/world_ypos hold the LOADED position, so the world map was redrawn once at
            // the restored spot before it left ("world map ukazovala kde AKTUÁLNĚ jsem"). Bail
            // before anything in this frame draws.
            //
            // Clearing every destination and breaking lands on the same `out:` cleanup the quit
            // path uses; the caller (map_check_state / scripts_check_state) then tears the window
            // down and the main loop resumes on whatever the save restored.
            if (LoadGameSerial() != wm_load_serial) {
                should_redraw = 0;
                is_entering_city = 0;
                is_entering_townmap = 0;
                is_entering_random_encounter = 0;
                is_entering_random_terrain = 0;
                is_moving = 0;
                done = 1;
                break;
            }

            mouseGetPositionInWindow(world_win, &mouse_x, &mouse_y);

            // CE: D-PAD PICKS A TOWN FROM THE LEFT-HAND LIST, A confirms it.
            //
            // ⚠️ THE CONFIRM SYNTHESISES A TAB PRESS (input = 500 + town) RATHER THAN ACTING ITSELF.
            // That branch below already decides correctly between the two things a tab means: travel
            // to the town, or -- when it is the town you are already standing in -- enter it. Doing
            // it here would mean duplicating that decision and having it drift.
            //
            // The cursor is parked on the chosen town's button as the indicator, which is why no
            // custom highlight is drawn: the left panel is painted once outside the redraw loop
            // (the loop only repaints the map area), so anything drawn into it would have to be
            // erased by hand against a background this screen never keeps a clean copy of.
            //
            // Only VISITED towns are reachable (first_visit_flag), matching the tabs themselves --
            // an unvisited town has no tab to click and no name on the list.
            {
                static bool wmPrevA = false;
                static bool wmWasMoving = false;
                static bool wmPrevUp = false;
                static bool wmPrevDown = false;
                static unsigned int wmLastStep = 0;

                SDL_GameController* wmPad = gamepad_ui_get_controller();
                bool wmANow = false;
                bool wmUpNow = false;
                bool wmDownNow = false;
                if (wmPad != NULL) {
                    SDL_GameControllerUpdate();
                    wmANow = SDL_GameControllerGetButton(wmPad, SDL_CONTROLLER_BUTTON_A) != 0;
                    wmUpNow = SDL_GameControllerGetButton(wmPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
                    wmDownNow = SDL_GameControllerGetButton(wmPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
                }

                // Start on the town you are standing in, so the very first A still means "enter
                // here" exactly as it did before this existed.
                if (wm_pad_town == -1) {
                    int wmHere = InCity(world_xpos, world_ypos);
                    if (wmHere != -1 && (first_visit_flag & (1 << wmHere)) != 0) {
                        wm_pad_town = wmHere;
                    }
                }

                // ⚠️ ARRIVED -- re-point the selection at whatever the party is now standing on.
                //
                // The selection was only ever set ONCE, on entering this screen, so after a journey
                // it still named the place you SET OFF FROM: pressing A at your destination sent
                // 500 + <old town>, and the tab handler dutifully turned the party around and walked
                // all the way back. Which is the opposite of the one thing you want on arrival.
                //
                // Only on the moving -> stopped EDGE. Re-syncing every stationary frame would fight
                // the D-pad, and picking a DIFFERENT town from the list is exactly how you set off
                // in the first place. Landing somewhere with no tab (a site you have never visited,
                // or open terrain) clears the selection instead, which drops A through to the
                // "enter whatever is under the party" fallback below -- so a brand new location is
                // entered with A too, tab or no tab.
                if (wmWasMoving && !is_moving) {
                    int wmArrived = InCity(world_xpos, world_ypos);
                    if (wmArrived != -1 && (first_visit_flag & (1 << wmArrived)) != 0) {
                        wm_pad_town = wmArrived;
                        Rect wmWin;
                        if (win_get_rect(world_win, &wmWin) == 0) {
                            mouse_set_position(wmWin.ulx + 508 + 7, wmWin.uly + BttnYtab[wm_pad_town] + 7);
                        }
                    } else {
                        wm_pad_town = -1;
                    }
                }
                wmWasMoving = is_moving != 0;

                int wmDir = 0;
                bool wmRepeat = elapsed_time(wmLastStep) >= 180;
                if ((wmUpNow && !wmPrevUp) || (wmUpNow && wmRepeat)) {
                    wmDir = -1;
                } else if ((wmDownNow && !wmPrevDown) || (wmDownNow && wmRepeat)) {
                    wmDir = 1;
                }

                if (wmDir != 0) {
                    wmLastStep = get_time();

                    // Walk to the next VISITED town, wrapping. Bounded by TOWN_COUNT so a list with
                    // nothing visited (impossible in practice -- Vault 13 is set at game start)
                    // cannot spin here.
                    int wmNext = wm_pad_town;
                    for (int wmStep = 0; wmStep < TOWN_COUNT; wmStep++) {
                        wmNext = (wmNext + wmDir + TOWN_COUNT) % TOWN_COUNT;
                        if ((first_visit_flag & (1 << wmNext)) != 0) {
                            break;
                        }
                    }

                    if (wmNext != wm_pad_town && (first_visit_flag & (1 << wmNext)) != 0) {
                        wm_pad_town = wmNext;
                        gsound_play_sfx_file("ib1p1xx1");
                    }

                    if (wm_pad_town != -1) {
                        Rect wmWin;
                        if (win_get_rect(world_win, &wmWin) == 0) {
                            mouse_set_position(wmWin.ulx + 508 + 7, wmWin.uly + BttnYtab[wm_pad_town] + 7);
                        }
                    }
                }

                if (wmANow && !wmPrevA) {
                    if (wm_pad_town != -1 && (first_visit_flag & (1 << wm_pad_town)) != 0) {
                        input = 500 + wm_pad_town;
                    } else {
                        // Nothing selectable (standing on an unvisited site): keep the original
                        // behaviour of entering whatever is under the party.
                        int wmHere = InCity(world_xpos, world_ypos);
                        if (wmHere != -1) {
                            entering_city = wmHere;
                            is_entering_city = 1;
                            should_redraw = 0;
                            is_moving = 0;
                        }
                    }
                }

                wmPrevA = wmANow;
                wmPrevUp = wmUpNow;
                wmPrevDown = wmDownNow;
            }

            mouse_dx = abs(mouse_x - (world_xpos - viewport_x + 22));
            mouse_dy = abs(mouse_y - (world_ypos - viewport_y + 20));

            if (mouse_dx < mouse_dy) {
                mouse_dx /= 2;
            } else {
                mouse_dy /= 2;
            }

            if (mouse_dx + mouse_dy > 10) {
                if (hover) {
                    should_redraw = 1;
                }
                hover = 0;
            } else {
                if (!hover) {
                    should_redraw = 1;
                    hover = 1;
                }
            }

            if (input >= 500 && input < 512) {
                if ((first_visit_flag & (1 << (input - 500))) != 0) {
                    if (!is_moving_to_town || !is_moving || InCity(target_xpos, target_ypos) != input - 500) {
                        target_xpos = 50 * city_location[input - 500].column + 50 / 2;
                        target_ypos = 50 * city_location[input - 500].row + 50 / 2;

                        temp_town = InCity(target_xpos, target_ypos);
                        if (temp_town != InCity(world_xpos, world_ypos) || temp_town == -1) {
                            autofollow = !is_moving;
                            TargetTown(temp_town);
                            is_moving = 1;
                            is_moving_to_town = 1;
                        } else {
                            entering_city = temp_town;
                            is_entering_city = 1;
                            should_redraw = 0;
                            is_moving = 0;
                        }
                    }
                }
            } else if (input == 512) {
                should_redraw = 0;
                reselect = 0;
                done = 1;
                is_entering_townmap = 1;
            } else if ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                if (mouse_x > 22 && mouse_x < 472 && mouse_y > 21 && mouse_y < 463) {
                    if (dropbtn && hover) {
                        is_entering_city = 0;
                        gsound_play_sfx_file("ib2p1xx1");
                        reselect = 0;
                        is_entering_random_encounter = 0;
                        is_entering_random_terrain = 1;
                        temp_town = InCity(world_xpos, world_ypos);
                        if (temp_town != -1) {
                            entering_city = temp_town;
                            is_entering_city = 1;
                            is_entering_random_terrain = 0;
                        }

                        break;
                    } else {
                        autofollow = !is_moving;
                        gsound_play_sfx_file("ib1p1xx1");
                        target_xpos = viewport_x + mouse_x - 22;
                        target_ypos = viewport_y + mouse_y - 21;
                        is_moving = 1;
                        world_move_init();
                        dropbtn = 0;
                        is_moving_to_town = InCity(target_xpos, target_ypos) != -1;
                    }
                }
            } else {
                switch (input) {
                case KEY_UPPERCASE_A:
                case KEY_LOWERCASE_A:
                    autofollow = !autofollow;
                    break;
                case KEY_CTRL_P:
                case KEY_ALT_P:
                case KEY_UPPERCASE_P:
                case KEY_LOWERCASE_P:
                    PauseWindow(1);
                    break;
                case KEY_F12:
                    dump_screen();
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
                }
            }


            if (game_user_wants_to_quit != 0) {
                should_redraw = 0;
                is_entering_city = 0;
                is_entering_townmap = 0;
                is_entering_random_encounter = 0;
                is_entering_random_terrain = 0;
                done = 1;
            }

            if (input == KEY_HOME || input == KEY_UPPERCASE_C || input == KEY_LOWERCASE_C || autofollow) {
                viewport_x = world_xpos - 247;
                viewport_y = world_ypos - 242;

                if (viewport_x < 0) {
                    viewport_x = 0;
                } else if (viewport_x > VIEWPORT_MAX_X) {
                    viewport_x = VIEWPORT_MAX_X;
                }

                if (viewport_y < 0) {
                    viewport_y = 0;
                } else if (viewport_y > VIEWPORT_MAX_Y) {
                    viewport_y = VIEWPORT_MAX_Y;
                }

                should_redraw = 1;
            }

            mouse_get_position(&abs_mouse_x, &abs_mouse_y);

            scroll_dx = 0;
            scroll_dy = 0;
            if (abs_mouse_x == 0) {
                scroll_dx = -1;
                autofollow = 0;
            } else if (abs_mouse_x == screenGetWidth() - 1) {
                scroll_dx = 1;
                autofollow = 0;
            }

            if (abs_mouse_y == 0) {
                scroll_dy = -1;
                autofollow = 0;
            } else if (abs_mouse_y == screenGetHeight() - 1) {
                scroll_dy = 1;
                autofollow = 0;
            }

            scroll_invalid = 0;
            scroll_invalid_x = 0;
            scroll_invalid_y = 0;

            candidate_viewport_x = viewport_x + 16 * scroll_dx;
            if (candidate_viewport_x < 0 || candidate_viewport_x > VIEWPORT_MAX_X) {
                scroll_invalid_x = 1;
            }

            candidate_viewport_y = viewport_y + 16 * scroll_dy;
            if (candidate_viewport_y < 0 || candidate_viewport_y > VIEWPORT_MAX_Y) {
                scroll_invalid_y = 1;
            }

            if (mouse_table1[scroll_dy + 1][scroll_dx + 1][0] == scroll_invalid_x) {
                scroll_invalid++;
            }

            if (mouse_table1[scroll_dy + 1][scroll_dx + 1][1] == scroll_invalid_y) {
                scroll_invalid++;
            }

            switch (scroll_invalid) {
            case 2:
                gmouse_set_cursor(mouse_table3[scroll_dy + 1][scroll_dx + 1][0]);
                break;
            default:
                gmouse_set_cursor(mouse_table2[scroll_dy + 1][scroll_dx + 1][0]);
                break;
            }

            switch (input) {
            case KEY_ARROW_LEFT:
                if (viewport_x != 0) {
                    scroll_dx = -1;
                    autofollow = 0;
                }
                break;
            case KEY_ARROW_RIGHT:
                if (viewport_x < VIEWPORT_MAX_X) {
                    scroll_dx = 1;
                    autofollow = 0;
                }
                break;
            case KEY_ARROW_UP:
                if (viewport_y != 0) {
                    scroll_dy = -1;
                    autofollow = 0;
                }
                break;
            case KEY_ARROW_DOWN:
                if (viewport_y < VIEWPORT_MAX_Y) {
                    scroll_dy = 1;
                    autofollow = 0;
                }
                break;
            case KEY_HOME:
                scroll_dy = 1;
                viewport_y = 0;
                autofollow = 0;
                break;
            case KEY_END:
                scroll_dy = 1;
                viewport_y = VIEWPORT_MAX_Y;
                autofollow = 0;
                break;
            case -1:
                if ((mouse_get_buttons() & MOUSE_EVENT_WHEEL) != 0) {
                    int wheel_x;
                    int wheel_y;
                    mouseGetWheel(&wheel_x, &wheel_y);

                    if (mouseHitTestInWindow(world_win, 22, 21, 450 + 22, 442 + 21)) {
                        if (wheel_x > 0) {
                            scroll_dx = 1;
                            autofollow = 0;
                        } else if (wheel_x < 0) {
                            scroll_dx = -1;
                            autofollow = 0;
                        }

                        if (wheel_y > 0) {
                            scroll_dy = -1;
                            autofollow = 0;
                        } else if (wheel_y < 0) {
                            scroll_dy = 1;
                            autofollow = 0;
                        }
                    }
                }
                break;
            }

            if (scroll_dx != 0 || scroll_dy != 0) {
                viewport_x += 16 * scroll_dx;
                viewport_y += 16 * scroll_dy;

                if (viewport_x < 0) {
                    viewport_x = 0;
                } else if (viewport_x > VIEWPORT_MAX_X) {
                    viewport_x = VIEWPORT_MAX_X;
                }

                if (viewport_y < 0) {
                    viewport_y = 0;
                } else if (viewport_y > VIEWPORT_MAX_Y) {
                    viewport_y = VIEWPORT_MAX_Y;
                }

                should_redraw = 1;
            }

            // Left stick steers travel like character movement: project a
            // destination a short distance ahead of the party in the stick
            // direction and (re)start movement toward it each frame the stick is
            // held. Releasing the stick lets the party coast to that last target
            // and stop. SDL's stick Y and world Y both point down, so no flip.
            float stick_x;
            float stick_y;
            if (!gamepad_ui_get_left_stick(&stick_x, &stick_y)) {
                // Stick is neutral -- arm it so subsequent pushes steer. This is also what
                // gates out a hold carried over from the previous map (see wm_stick_armed init).
                wm_stick_armed = 1;
            } else if (wm_stick_armed) {
                float stick_mag = sqrtf(stick_x * stick_x + stick_y * stick_y);
                int steer_x = world_xpos + (int)(stick_x / stick_mag * WM_STICK_TRAVEL_LOOKAHEAD);
                int steer_y = world_ypos + (int)(stick_y / stick_mag * WM_STICK_TRAVEL_LOOKAHEAD);

                if (steer_x < 0) {
                    steer_x = 0;
                } else if (steer_x > WM_WORLDMAP_WIDTH - 1) {
                    steer_x = WM_WORLDMAP_WIDTH - 1;
                }

                if (steer_y < 0) {
                    steer_y = 0;
                } else if (steer_y > WM_WORLDMAP_HEIGHT - 1) {
                    steer_y = WM_WORLDMAP_HEIGHT - 1;
                }

                target_xpos = steer_x;
                target_ypos = steer_y;
                autofollow = 1;
                is_moving = 1;
                world_move_init();
                is_moving_to_town = InCity(target_xpos, target_ypos) != -1;
            }

            // CE: single choke point for the poison refusal. Gating HERE rather than at each of the
            // six `is_moving = 1` sites (stick, mouse, autofollow, script jumps) means no start path
            // can slip past it, and an already-running journey is stopped too.
            if (is_moving && WMapPoisonBlocksTravel()) {
                is_moving = 0;
                autofollow = 0;

                gsound_play_sfx_file("iisxxxx1");

                // ⚠️ display_print TAKES A MUTABLE BUFFER AND WRITES INTO IT. It word-wraps in
                // place (display.cc:248 -- `*space = '\0'`, then restores the space), so handing it
                // a STRING LITERAL is a write into read-only .rdata: instant ACCESS_VIOLATION.
                // That is exactly what crashed him -- stack was display_print <- world_map <-
                // map_check_state, "access: write 0x7ff730c3db0d" (trace_2026-08-24_234756). The
                // signature says `char*` rather than `const char*` for precisely this reason, and
                // every vanilla caller feeds it a getmsg() buffer or a local array.
                char poisonMsg[64];
                strcpy(poisonMsg, "You are poisoned, too weak to travel.");
                display_print(poisonMsg);

                // Land the same way the vanilla HP<=5 queue trigger does: the town under your feet
                // if there is one, otherwise a random terrain map. Either way you end up somewhere
                // time flows and the Pipboy and inventory exist, so the poison can be slept off,
                // cured or out-healed -- which the world map cannot offer (its clock only runs
                // while you are moving, and it has no Pipboy).
                is_entering_city = 0;
                is_entering_townmap = 0;
                is_entering_random_encounter = 0;
                is_entering_random_terrain = 0;
                temp_town = InCity(world_xpos, world_ypos);
                if (temp_town != -1) {
                    entering_city = temp_town;
                    is_entering_city = 1;
                } else {
                    is_entering_random_terrain = 1;
                }

                goto out;
            }

            if (is_moving) {
                v109 = 0;
                dropbtn = 0;
                while (v109 < 2) {
                    if (!is_moving || is_entering_random_encounter || is_entering_city || done) {
                        break;
                    }

                    switch (WorldTerraTable[world_ypos / 50][world_xpos / 50]) {
                    case TERRAIN_TYPE_MOUNTAIN:
                        move_counter -= 1;
                        if (move_counter <= 0) {
                            is_moving = world_move_step() == 0;
                            if (world_xpos < 1064 && world_ypos > 0) {
                                if (((128 >> (world_xpos % 8)) & WALKMASK_MASK_DATA[world_ypos][world_xpos / 8]) != 0) {
                                    world_xpos = old_world_xpos;
                                    world_ypos = old_world_ypos;
                                    is_moving = 0;
                                }
                            }
                            move_counter = 2;
                        }

                        next_event_time = queue_next_time();
                        new_game_time = game_time() + time_adder;
                        if (new_game_time >= next_event_time) {
                            set_game_time(next_event_time + 1);
                            if (queue_process() != 0) {
                                debug_printf("\nWORLDMAP: Exiting from Queue trigger...\n");
                                is_entering_city = 0;
                                is_entering_townmap = 0;
                                is_entering_random_encounter = 0;
                                is_entering_random_terrain = 0;
                                temp_town = InCity(world_xpos, world_ypos);
                                if (temp_town != -1) {
                                    entering_city = temp_town;
                                    is_entering_city = 1;
                                } else {
                                    is_entering_random_terrain = 1;
                                }

                                goto out;
                            }
                        }

                        set_game_time(new_game_time);
                        break;
                    case TERRAIN_TYPE_CITY:
                        is_moving = world_move_step() == 0;
                        if (world_xpos < 1064 && world_ypos > 0) {
                            if (((128 >> (world_xpos % 8)) & WALKMASK_MASK_DATA[world_ypos][world_xpos / 8]) != 0) {
                                world_xpos = old_world_xpos;
                                world_ypos = old_world_ypos;
                                is_moving = false;
                            }
                        }

                        move_counter -= 1;
                        if (move_counter <= 0 && is_moving) {
                            is_moving = world_move_step() == 0;
                            if (world_xpos < 1064 && world_ypos > 0) {
                                if (((128 >> (world_xpos % 8)) & WALKMASK_MASK_DATA[world_ypos][world_xpos / 8]) != 0) {
                                    world_xpos = old_world_xpos;
                                    world_ypos = old_world_ypos;
                                    is_moving = false;
                                }
                            }
                            move_counter = 4;
                        } else {
                            next_event_time = queue_next_time();
                            new_game_time = game_time() + time_adder;
                            if (new_game_time >= next_event_time) {
                                set_game_time(next_event_time + 1);
                                if (queue_process() != 0) {
                                    debug_printf("\nWORLDMAP: Exiting from Queue trigger...\n");
                                    is_entering_city = 0;
                                    is_entering_townmap = 0;
                                    is_entering_random_encounter = 0;
                                    is_entering_random_terrain = 0;
                                    temp_town = InCity(world_xpos, world_ypos);
                                    if (temp_town != -1) {
                                        entering_city = temp_town;
                                        is_entering_city = 1;
                                    } else {
                                        is_entering_random_terrain = 1;
                                    }

                                    goto out;
                                }
                            }

                            set_game_time(new_game_time);
                        }
                        break;
                    default:
                        is_moving = world_move_step() == 0;
                        if (world_xpos < 1064 && world_ypos > 0) {
                            if (((128 >> (world_xpos % 8)) & WALKMASK_MASK_DATA[world_ypos][world_xpos / 8]) != 0) {
                                world_xpos = old_world_xpos;
                                world_ypos = old_world_ypos;
                                is_moving = 0;
                            }
                        }

                        move_counter = 0;

                        next_event_time = queue_next_time();
                        new_game_time = game_time() + time_adder;
                        if (new_game_time >= next_event_time) {
                            set_game_time(next_event_time + 1);
                            if (queue_process() != 0) {
                                debug_printf("\nWORLDMAP: Exiting from Queue trigger...\n");
                                is_entering_city = 0;
                                is_entering_townmap = 0;
                                is_entering_random_encounter = 0;
                                is_entering_random_terrain = 0;
                                temp_town = InCity(world_xpos, world_ypos);
                                if (temp_town != -1) {
                                    entering_city = temp_town;
                                    is_entering_city = 1;
                                } else {
                                    is_entering_random_terrain = 1;
                                }

                                goto out;
                            }
                        }

                        set_game_time(new_game_time);
                        break;
                    }

                    if (travel_line_cycle-- <= 0) {
                        travel_line_cycle = 2;
                        index = (WM_WORLDMAP_WIDTH * world_ypos + world_xpos) / 8;
                        line1bit_buf[index] |= 128 >> (world_xpos % 8);
                        UpdVisualArea();
                    }

                    if (++wmap_mile >= wmap_day) {
                        wmap_mile = 0;
                        partyMemberRestingHeal(24);

                        random_enc_chance = roll_random(1, 6);
                        random_enc_chance += roll_random(1, 6);
                        random_enc_chance += roll_random(1, 6);
                        if (InCity(world_xpos, world_ypos) == -1) {
                            switch (WorldEcountChanceTable[world_ypos / 50][world_xpos / 50]) {
                            case 0:
                                if (random_enc_chance < 6) {
                                    is_entering_random_encounter = 1;
                                }
                                break;
                            case 1:
                                if (random_enc_chance < 7) {
                                    is_entering_random_encounter = 1;
                                }
                                break;
                            case 2:
                                if (random_enc_chance < 9) {
                                    is_entering_random_encounter = 1;
                                }
                                break;
                            case 3:
                                if (random_enc_chance < 10) {
                                    is_entering_random_encounter = 1;
                                }
                                break;
                            }
                        }

                        if (is_entering_random_encounter) {
                            v142 = 0;
                            while (v142 == 0) {
                                special_enc_chance = roll_random(1, 6);
                                special_enc_chance += roll_random(1, 6);
                                special_enc_chance += roll_random(1, 6);
                                special_enc_chance -= 5;
                                special_enc_chance += stat_level(obj_dude, STAT_LUCK);
                                special_enc_chance += 2 * perk_level(PERK_EXPLORER);
                                if (special_enc_chance < 18 || encounter_specials == 63) {
                                    v142 = 1;
                                    break;
                                }

                                special_enc_chance = roll_random(1, 100);
                                for (v109 = 0; v109 < 6 && v142 == 0; v109++) {
                                    if (special_enc_chance >= SpclEncRange[v109].start && special_enc_chance <= SpclEncRange[v109].end) {
                                        if ((encounter_specials & (1 << v109)) != 0) {
                                            break;
                                        }

                                        debug_printf("\nWORLD MAP: specail index #%d\n", v109);
                                        encounter_specials |= 1 << v109;
                                        v142 += 1;
                                        special_enc_num = v109 + 1;
                                    }
                                }
                            }
                        }
                    }

                    if (is_moving_to_town) {
                        if (!is_moving && !is_entering_random_encounter) {
                            temp_town = InCity(world_xpos, world_ypos);
                            if (temp_town != -1) {
                                dropbtn = 1;
                                is_entering_city = 0;
                                entering_city = temp_town;
                                reselect = 0;
                                is_moving_to_town = 0;
                                is_entering_random_encounter = 0;
                            }
                        }
                    }

                    v109++;

                    should_redraw = 1;
                }

                if (!is_moving) {
                    memset(line1bit_buf, 0, 262500);

                    if (!is_entering_city && !is_entering_random_encounter) {
                        should_redraw = 1;
                        dropbtn = 1;
                        reselect = 0;
                        is_moving_to_town = 0;
                        done = 0;
                        is_entering_city = 0;
                        is_entering_random_encounter = 0;
                        is_entering_random_terrain = 0;
                        is_moving = 0;
                    }
                }
            }

            switch (CheckEvents()) {
            case -1:
                // FIXME: Typo.
                debug_printf("\n** WORLD MAP: Error running specail event! **\n");
                break;
            case 1:
                should_redraw = 0;
                reselect = 0;
                is_entering_townmap = 0;
                is_entering_city = 0;
                is_entering_random_terrain = 0;
                is_entering_random_encounter = 0;
                done = 1;
                break;
            }

            if (should_redraw) {
                buf_to_buf(wmapbmp[WORLDMAP_FRM_WORLDMAP] + WM_WORLDMAP_WIDTH * viewport_y + viewport_x,
                    450,
                    442,
                    WM_WORLDMAP_WIDTH,
                    world_buf + 640 * 21 + 22,
                    640);
                block_map(viewport_x, viewport_y, world_buf);

                if (dropbtn) {
                    temp_x = world_xpos - viewport_x + 10;
                    temp_y = world_ypos - viewport_y + 15;
                    if (temp_x > -3 && temp_x < 484 && temp_y > 8 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_HOTSPOT_NORMAL],
                            temp_x > 460 ? 485 - temp_x : HOTSPOT_WIDTH,
                            HOTSPOT_HEIGHT,
                            HOTSPOT_WIDTH,
                            world_buf + 640 * temp_y + temp_x,
                            640);
                    }

                    if (hover) {
                        temp_town = InCity(world_xpos, world_ypos);
                        if (temp_town != -1) {
                            if (game_global_vars[cityXgvar[temp_town]] == 1 || (first_visit_flag & (1 << temp_town)) != 0) {
                                text = getmsg(&map_msg_file, &mesg, temp_town + 500);
                            } else {
                                text = getmsg(&wrldmap_mesg_file, &mesg, 1004);
                            }
                        } else {
                            text = getmsg(&wrldmap_mesg_file, &mesg, WorldTerraTable[world_ypos / 50][world_xpos / 50] + 1000);
                        }

                        location_name_width = text_width(text);
                        location_name_x = temp_x + (25 - location_name_width) / 2;
                        location_name_y = temp_y - 11;
                        if (location_name_x > 22 - location_name_width && location_name_x < 472 && location_name_y > 11 && location_name_y < 463) {
                            if (location_name_x < 22) {
                                hover_text_x = 22 - location_name_x;
                                if (hover_text_x < 0) {
                                    hover_text_x = location_name_x - 22;
                                }
                                location_name_x = 22;
                            } else {
                                hover_text_x = 0;
                            }

                            hover_text_width = location_name_width - hover_text_x;
                            if (location_name_x + location_name_width > 472) {
                                hover_text_width = 472 - hover_text_x - location_name_x;
                            }

                            if (hover_text_width != 0) {
                                buf_to_buf(world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                                    hover_text_width,
                                    text_height(),
                                    WM_WINDOW_WIDTH,
                                    hvrtxtbuf + hover_text_x,
                                    256);
                                text_to_buf(hvrtxtbuf, text, 256, 256, colorTable[992] | 0x10000);
                                buf_to_buf(hvrtxtbuf + hover_text_x,
                                    hover_text_width,
                                    text_height(),
                                    256,
                                    world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                                    WM_WINDOW_WIDTH);
                            }
                        }
                    }
                } else {
                    temp_x = world_xpos - viewport_x + 20;
                    temp_y = world_ypos - viewport_y + 19;
                    if (temp_x > 17 && temp_x < 474 && temp_y > 16 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_LOCATION_MARKER],
                            LOCATION_MARKER_WIDTH,
                            LOCATION_MARKER_HEIGHT,
                            LOCATION_MARKER_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }
                }

                if (is_moving) {
                    temp_x = target_xpos - viewport_x + 17;
                    temp_y = target_ypos - viewport_y + 16;
                    if (temp_x > 11 && temp_x < 474 && temp_y > 10 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_DESTINATION_MARKER_BRIGHT],
                            DESTINATION_MARKER_WIDTH,
                            DESTINATION_MARKER_HEIGHT,
                            DESTINATION_MARKER_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }
                    bit1exbit8(viewport_x,
                        viewport_y,
                        viewport_x + 449,
                        viewport_y + 441,
                        22,
                        21,
                        line1bit_buf,
                        world_buf,
                        WM_WORLDMAP_WIDTH,
                        WM_WINDOW_WIDTH,
                        colorTable[27648]);
                }

                trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX],
                    35,
                    WM_WINDOW_HEIGHT,
                    WM_WINDOW_WIDTH,
                    world_buf,
                    WM_WINDOW_WIDTH);
                trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + 455,
                    32,
                    WM_WINDOW_HEIGHT,
                    WM_WINDOW_WIDTH,
                    world_buf + 455,
                    WM_WINDOW_WIDTH);
                buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + 35,
                    422,
                    21,
                    WM_WINDOW_WIDTH,
                    world_buf + 35,
                    WM_WINDOW_WIDTH);
                buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + WM_WINDOW_WIDTH * 463 + 35,
                    422,
                    17,
                    WM_WINDOW_WIDTH,
                    world_buf + WM_WINDOW_WIDTH * 463 + 35,
                    WM_WINDOW_WIDTH);

                DrawMapTime(0);
                win_draw(world_win);

                should_redraw = 0;

                while (elapsed_time(time) < 1000 / 24) {
                }
            } else {
                if (!done) {
                    DrawMapTime(0);
                }
                win_draw(world_win);
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }

        if (!done) {
            if (special_enc_num != 0 || is_entering_random_encounter) {
                viewport_x = world_xpos - 247;
                viewport_y = world_ypos - 242;

                if (viewport_x < 0) {
                    viewport_x = 0;
                } else if (viewport_x > VIEWPORT_MAX_X) {
                    viewport_x = VIEWPORT_MAX_X;
                }

                if (viewport_y < 0) {
                    viewport_y = 0;
                } else if (viewport_y > VIEWPORT_MAX_Y) {
                    viewport_y = VIEWPORT_MAX_Y;
                }
            }

            buf_to_buf(wmapbmp[WORLDMAP_FRM_WORLDMAP] + WM_WORLDMAP_WIDTH * viewport_y + viewport_x,
                450,
                442,
                WM_WORLDMAP_WIDTH,
                world_buf + 21 * WM_WINDOW_WIDTH + 22,
                WM_WINDOW_WIDTH);
            block_map(viewport_x, viewport_y, world_buf);

            trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX],
                35,
                WM_WINDOW_HEIGHT,
                WM_WINDOW_WIDTH,
                world_buf,
                WM_WINDOW_WIDTH);
            trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + 455,
                32,
                WM_WINDOW_HEIGHT,
                WM_WINDOW_WIDTH,
                world_buf + 455,
                WM_WINDOW_WIDTH);
            buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + 35,
                422,
                21,
                WM_WINDOW_WIDTH,
                world_buf + 35,
                WM_WINDOW_WIDTH);
            buf_to_buf(wmapbmp[WORLDMAP_FRM_BOX] + WM_WINDOW_WIDTH * 463 + 35,
                422,
                17,
                WM_WINDOW_WIDTH,
                world_buf + WM_WINDOW_WIDTH * 463 + 35,
                WM_WINDOW_WIDTH);
        }

        if (is_entering_random_encounter) {
            if (special_enc_num != 0) {
                // Entering special encounter - cycle destination marker dark
                // vs. bright.
                temp_x = world_xpos - viewport_x + 17;
                temp_y = world_ypos - viewport_y + 16;

                for (index = 0; index < 7; index++) {
                    if (temp_x > 11 && temp_x < 474 && temp_y > 10 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_DESTINATION_MARKER_DARK],
                            DESTINATION_MARKER_WIDTH,
                            DESTINATION_MARKER_HEIGHT,
                            DESTINATION_MARKER_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }

                    win_draw(world_win);
                    renderPresent();
                    block_for_tocks(199);

                    if (temp_x > 11 && temp_x < 474 && temp_y > 10 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_DESTINATION_MARKER_BRIGHT],
                            DESTINATION_MARKER_WIDTH,
                            DESTINATION_MARKER_HEIGHT,
                            DESTINATION_MARKER_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }

                    win_draw(world_win);
                    renderPresent();
                    block_for_tocks(199);
                }
            } else {
                // Entering random encounter - cycle "fight" marker (lightning)
                // marker dark vs. bright.
                temp_x = world_xpos - viewport_x + 19;
                temp_y = world_ypos - viewport_y + 16;

                for (index = 0; index < 7; index++) {
                    if (temp_x > 15 && temp_x < 472 && temp_y > 10 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_RANDOM_ENCOUNTER_BRIGHT],
                            RANDOM_ENCOUNTER_ICON_WIDTH,
                            RANDOM_ENCOUNTER_ICON_HEIGHT,
                            RANDOM_ENCOUNTER_ICON_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }

                    win_draw(world_win);
                    renderPresent();
                    block_for_tocks(199);

                    if (temp_x > 15 && temp_x < 472 && temp_y > 10 && temp_y < 463) {
                        trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_RANDOM_ENCOUNTER_DARK],
                            RANDOM_ENCOUNTER_ICON_WIDTH,
                            RANDOM_ENCOUNTER_ICON_HEIGHT,
                            RANDOM_ENCOUNTER_ICON_WIDTH,
                            world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                            WM_WINDOW_WIDTH);
                    }

                    win_draw(world_win);
                    renderPresent();
                    block_for_tocks(199);
                }
            }
        } else if (is_entering_city) {
            // Entering city - render final frame (displaying pressed hotspot).
            temp_x = world_xpos - viewport_x + 10;
            temp_y = world_ypos - viewport_y + 15;

            if (temp_x > -3 && temp_x < 484 && temp_y > 8 && temp_y < 463) {
                trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_HOTSPOT_PRESSED],
                    temp_x > 460 ? 485 - temp_x : HOTSPOT_WIDTH,
                    HOTSPOT_HEIGHT,
                    HOTSPOT_WIDTH,
                    world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                    WM_WINDOW_WIDTH);
            }

            if (hover) {
                temp_town = InCity(world_xpos, world_ypos);
                if (temp_town != -1) {
                    if (game_global_vars[cityXgvar[temp_town]] == 1 || (first_visit_flag & (1 << temp_town)) != 0) {
                        text = getmsg(&map_msg_file, &mesg, temp_town + 500);
                    } else {
                        text = getmsg(&wrldmap_mesg_file, &mesg, 1004);
                    }
                } else {
                    text = getmsg(&wrldmap_mesg_file, &mesg, WorldTerraTable[world_ypos / 50][world_xpos / 50] + 1000);
                }

                location_name_width = text_width(text);
                location_name_x = temp_x + (25 - location_name_width) / 2;
                location_name_y = temp_y - 11;
                if (location_name_x > 22 - location_name_width && location_name_x < 472 && location_name_y > 11 && location_name_y < 463) {
                    if (location_name_x < 22) {
                        hover_text_x = 22 - location_name_x;
                        if (hover_text_x < 0) {
                            hover_text_x = location_name_x - 22;
                        }
                        location_name_x = 22;
                    } else {
                        hover_text_x = 0;
                    }

                    hover_text_width = location_name_width - hover_text_x;
                    if (location_name_x + location_name_width > 472) {
                        hover_text_width = 472 - hover_text_x - location_name_x;
                    }

                    if (hover_text_width != 0) {
                        buf_to_buf(world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                            hover_text_width,
                            text_height(),
                            WM_WINDOW_WIDTH,
                            hvrtxtbuf + hover_text_x,
                            256);
                        text_to_buf(hvrtxtbuf, text, 256, 256, colorTable[992] | 0x10000);
                        buf_to_buf(hvrtxtbuf + hover_text_x,
                            hover_text_width,
                            text_height(),
                            256,
                            world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                            WM_WINDOW_WIDTH);
                    }
                }
            }

            win_draw(world_win);
            renderPresent();

            if (!dropbtn) {
                block_for_tocks(500);
            } else {
                while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
                }
                block_for_tocks(300);
            }
        } else if (is_entering_random_terrain) {
            // Entering random terrain - render final frame (displaying pressed
            // hotspot).
            temp_x = world_xpos - viewport_x + 10;
            temp_y = world_ypos - viewport_y + 15;

            if (temp_x > -3 && temp_x < 484 && temp_y > 8 && temp_y < 463) {
                trans_buf_to_buf(wmapbmp[WORLDMAP_FRM_HOTSPOT_PRESSED],
                    temp_x > 460 ? 485 - temp_x : HOTSPOT_WIDTH,
                    HOTSPOT_HEIGHT,
                    HOTSPOT_WIDTH,
                    world_buf + WM_WINDOW_WIDTH * temp_y + temp_x,
                    WM_WINDOW_WIDTH);
            }

            if (hover) {
                temp_town = InCity(world_xpos, world_ypos);
                if (temp_town != -1) {
                    if (game_global_vars[cityXgvar[temp_town]] == 1 || (first_visit_flag & (1 << temp_town)) != 0) {
                        text = getmsg(&map_msg_file, &mesg, temp_town + 500);
                    } else {
                        text = getmsg(&wrldmap_mesg_file, &mesg, 1004);
                    }
                } else {
                    text = getmsg(&wrldmap_mesg_file, &mesg, WorldTerraTable[world_ypos / 50][world_xpos / 50] + 1000);
                }

                location_name_width = text_width(text);
                location_name_x = temp_x + (25 - location_name_width) / 2;
                location_name_y = temp_y - 11;
                if (location_name_x > 22 - location_name_width && location_name_x < 472 && location_name_y > 11 && location_name_y < 463) {
                    if (location_name_x < 22) {
                        hover_text_x = 22 - location_name_x;
                        if (hover_text_x < 0) {
                            hover_text_x = location_name_x - 22;
                        }
                        location_name_x = 22;
                    } else {
                        hover_text_x = 0;
                    }

                    hover_text_width = location_name_width - hover_text_x;
                    if (location_name_x + location_name_width > 472) {
                        hover_text_width = 472 - hover_text_x - location_name_x;
                    }

                    if (hover_text_width != 0) {
                        buf_to_buf(world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                            hover_text_width,
                            text_height(),
                            WM_WINDOW_WIDTH,
                            hvrtxtbuf + hover_text_x,
                            256);
                        text_to_buf(hvrtxtbuf, text, 256, 256, colorTable[992] | 0x10000);
                        buf_to_buf(hvrtxtbuf + hover_text_x,
                            hover_text_width,
                            text_height(),
                            256,
                            world_buf + WM_WINDOW_WIDTH * location_name_y + location_name_x,
                            WM_WINDOW_WIDTH);
                    }
                }
            }

            win_draw(world_win);
            renderPresent();

            while ((mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
            }

            block_for_tocks(300);
        }

    out:

        UnInitWorldMapData();
        art_flush();

        if (is_entering_random_terrain) {
            debug_printf("\nWORLD MAP: Droping out to random terrain area map.\n");
            game_global_vars[GVAR_WORLD_TERRAIN] = WorldEcounTable[world_ypos / 50][world_xpos / 50];

            terrain = WorldTerraTable[world_ypos / 50][world_xpos / 50];
            while (1) {
                map_index = roll_random(0, 2);
                if (RandEnctNames[terrain][map_index] != NULL) {
                    break;
                }
            }

            debug_printf("\nWORLD MAP: Loading rand \"drop down\" map index #%d, name: %s\n",
                map_index,
                RandEnctNames[terrain][map_index]);

            for (index = 0; index < TOWN_COUNT; index++) {
                win_disable_button(TownBttns[index]);
            }

            win_disable_button(WrldToggle);

            if (LoadTownMap(RandEnctNames[terrain][map_index], 1) == -1) {
                return -1;
            }
        } else if (is_entering_random_encounter) {
            if (special_enc_num != 0) {
                // FIXME: Typo.
                debug_printf("\nWORLD MAP: Doing specail map index #%d...\n", special_enc_num - 1);
                game_global_vars[GVAR_WORLD_TERRAIN] = WorldEcounTable[world_ypos / 50][world_xpos / 50];

                for (index = 0; index < TOWN_COUNT; index++) {
                    win_disable_button(TownBttns[index]);
                }

                win_disable_button(WrldToggle);

                if (LoadTownMap(spcl_map_name[special_enc_num - 1], 0) == -1) {
                    return -1;
                }
            } else {
                game_global_vars[GVAR_WORLD_TERRAIN] = WorldEcounTable[world_ypos / 50][world_xpos / 50];

                terrain = WorldTerraTable[world_ypos / 50][world_xpos / 50];
                while (1) {
                    map_index = roll_random(0, 2);
                    if (RandEnctNames[terrain][map_index] != NULL) {
                        break;
                    }
                }

                debug_printf("\nWORLD MAP: Loading rand encounter map index #%d, name: %s\n",
                    map_index,
                    RandEnctNames[terrain][map_index]);

                for (index = 0; index < TOWN_COUNT; index++) {
                    win_disable_button(TownBttns[index]);
                }

                win_disable_button(WrldToggle);

                if (LoadTownMap(RandEnctNames[terrain][map_index], 2) == -1) {
                    return -1;
                }
            }
        } else if (is_entering_city) {
            debug_printf("\nWORLD MAP: Entering into city index #%d.\n", entering_city);

            our_town = entering_city;
            if ((first_visit_flag & (1 << entering_city)) == 0) {
                for (index = 0; index < TOWN_COUNT; index++) {
                    win_disable_button(TownBttns[index]);
                }

                win_disable_button(WrldToggle);

                if (LoadTownMap(TownHotSpots[entering_city][0].name, TownHotSpots[entering_city][0].map_idx) == -1) {
                    return -1;
                }

                reselect = 0;
                first_visit_flag |= 1 << entering_city;
            } else {
                ctx.town = entering_city;
                ctx = town_map(ctx);
                switch (ctx.state) {
                case -1:
                    if (bx_enable) {
                        enable_box_bar_win();
                    }
                    return -1;
                case 3:
                    is_moving = 0;
                    is_entering_city = 0;
                    reselect = 0;
                    dropbtn = 1;
                    is_moving_to_town = 0;
                    is_entering_random_encounter = 0;
                    is_entering_townmap = 0;
                    autofollow = 1;
                    move_counter = 0;
                    should_redraw = 1;
                    travel_line_cycle = 0;
                    continue;
                default:
                    if (ctx.town != entering_city) {
                        TargetTown(ctx.town);
                        is_entering_city = 0;
                        autofollow = 1;
                        is_entering_townmap = 0;
                        should_redraw = 1;
                        our_section = ctx.section;
                        reselect = 1;
                        is_entering_random_encounter = 0;
                        move_counter = 0;
                        is_moving = 1;
                        travel_line_cycle = 0;
                        is_moving_to_town = 1;
                        continue;
                    }

                    for (index = 0; index < TOWN_COUNT; index++) {
                        win_disable_button(TownBttns[index]);
                    }

                    win_disable_button(WrldToggle);

                    if (LoadTownMap(TownHotSpots[ctx.town][ctx.section].name, TownHotSpots[ctx.town][ctx.section].map_idx) == -1) {
                        return -1;
                    }

                    reselect = 0;
                    break;
                }
            }
        } else if (is_entering_townmap) {
            if (reselect) {
                ctx.town = entering_city;
            } else {
                ctx.town = our_town;
            }

            ctx = town_map(ctx);
            switch (ctx.state) {
            case -1:
                return -1;
            case 3:
                is_moving = 0;
                is_entering_random_terrain = 0;
                is_entering_city = 0;
                is_entering_random_encounter = 0;
                entering_city = 0;
                done = 0;
                move_counter = 0;
                travel_line_cycle = 0;
                dropbtn = 1;
                reselect = 0;
                is_entering_townmap = 0;
                should_redraw = 1;
                autofollow = 0;
                continue;
            case 2:
                temp_town = InCity(world_xpos, world_ypos);
                if (temp_town != ctx.town) {
                    entering_city = ctx.town;
                    done = 0;
                    is_entering_townmap = 0;
                    is_entering_random_terrain = 0;
                    is_entering_city = 0;
                    is_entering_random_encounter = 0;
                    autofollow = 1;
                    reselect = 1;
                    should_redraw = 1;
                    TargetTown(ctx.town);
                    is_moving = 1;
                    is_moving_to_town = 1;
                    our_section = ctx.section;
                    continue;
                }

                our_town = temp_town;
                entering_city = temp_town;

                for (index = 0; index < TOWN_COUNT; index++) {
                    win_disable_button(TownBttns[index]);
                }

                win_disable_button(WrldToggle);

                if (LoadTownMap(TownHotSpots[entering_city][ctx.section].name, TownHotSpots[entering_city][ctx.section].map_idx) == -1) {
                    return -1;
                }

                reselect = 0;
                break;
            }
        }

        if (iso_was_disabled) {
            if (bk_enable) {
                map_enable_bk_processes();
            }
            cycle_enable();
        }

        return 0;
    }
}

// 0x4AC860
static void UpdVisualArea()
{
    // 0x4A9FE0
    static const struct {
        int column_offset;
        int row_offset;
    } offsets[3][3] = {
        {
            { -1, -1 },
            { 0, -1 },
            { 1, -1 },
        },
        {
            { -1, 0 },
            { 0, 0 },
            { 1, 0 },
        },
        {
            { -1, 1 },
            { 0, 1 },
            { 1, 1 },
        },
    };

    // 0x4AA028
    static const struct {
        int column_offset;
        int row_offset;
    } scout_offsets[5][5] = {
        {
            { -2, -2 },
            { -1, -2 },
            { 0, -2 },
            { 1, -2 },
            { 2, -2 },
        },
        {
            { -2, -1 },
            { -1, -1 },
            { 0, -1 },
            { 1, -1 },
            { 2, -1 },
        },
        {
            { -2, 0 },
            { -1, 0 },
            { 0, 0 },
            { 1, 0 },
            { 2, 0 },
        },
        {
            { -2, 1 },
            { -1, 1 },
            { 0, 1 },
            { 1, 1 },
            { 2, 1 },
        },
        {
            { -2, 2 },
            { -1, 2 },
            { 0, 2 },
            { 1, 2 },
            { 2, 2 },
        },
    };

    int current_column = world_xpos / 50;
    int current_row = world_ypos / 50;
    int column;
    int row;
    int column_offset_index;
    int row_offset_index;
    int first_column_offset_index = 0;
    int last_column_offset_index;

    WorldGrid[current_row][current_column] = 2;

    if (perk_level(PERK_SCOUT)) {
        last_column_offset_index = 5;
        for (row_offset_index = 0; row_offset_index < 5; row_offset_index++) {
            row = current_row + scout_offsets[row_offset_index][0].row_offset;
            if (row >= 30) {
                break;
            }

            if (row >= 0) {
                for (column_offset_index = first_column_offset_index; column_offset_index < last_column_offset_index; column_offset_index++) {
                    column = current_column + scout_offsets[row_offset_index][column_offset_index].column_offset;
                    if (column < 0) {
                        first_column_offset_index++;
                    } else if (column >= 28) {
                        last_column_offset_index--;
                    } else {
                        if (WorldGrid[row][column] != 2) {
                            WorldGrid[row][column] = 1;
                        }
                    }
                }
            }
        }
    } else {
        last_column_offset_index = 3;
        for (row_offset_index = 0; row_offset_index < 3; row_offset_index++) {
            row = current_row + offsets[row_offset_index][0].row_offset;
            if (row >= 30) {
                break;
            }

            if (row >= 0) {
                for (column_offset_index = first_column_offset_index; column_offset_index < last_column_offset_index; column_offset_index++) {
                    column = current_column + offsets[row_offset_index][column_offset_index].column_offset;
                    if (column < 0) {
                        first_column_offset_index++;
                    } else if (column >= 28) {
                        last_column_offset_index--;
                    } else {
                        if (WorldGrid[row][column] != 2) {
                            WorldGrid[row][column] = 1;
                        }
                    }
                }
            }
        }
    }

    if (current_column <= OceanSeeXTable[current_row]) {
        for (column = 0; column < OceanSeeXTable[current_row]; column++) {
            WorldGrid[current_row][column] = 2;
        }
    }
}

// 0x4ACA5C
static int CheckEvents()
{
    int rc = 0;

    if (game_global_vars[GVAR_VAULT_WATER] == 0 && game_global_vars[GVAR_FIND_WATER_CHIP] != 2) {
        debug_printf("\nWORLD MAP: Vault water time ran out (death).\n");
        BlackOut();
        gmovie_play(MOVIE_BOIL3, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
        game_user_wants_to_quit = 1;
        rc = 1;
    } else {
        if (game_global_vars[GVAR_VATS_COUNTDOWN] != 0) {
            if ((game_time() - game_global_vars[GVAR_VATS_COUNTDOWN]) / 10 > 240) {
                // FIXME: Typo.
                debug_printf("\nWORLD MAP: Doing \"Vats explode\" specail.\n");

                gmovie_play(MOVIE_VEXPLD, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
                game_global_vars[GVAR_DESTROY_MASTER_4] = 2;

                if (game_global_vars[GVAR_MASTER_BLOWN] != 0) {
                    worldmap_script_jump(0, 0);

                    rc = 1;
                    if (LoadTownMap(TownHotSpots[TOWN_VAULT_13][0].name, TownHotSpots[TOWN_VAULT_13][0].map_idx) == -1) {
                        rc = -1;
                    }
                }

                stat_pc_add_experience(10000);
                game_global_vars[GVAR_PLAYER_REPUATION] += 5;

                // NOTE: Looks like min/max macro usage.
                if (game_global_vars[GVAR_PLAYER_REPUATION] < -100) {
                    game_global_vars[GVAR_PLAYER_REPUATION] = -100;
                } else if (game_global_vars[GVAR_PLAYER_REPUATION] > 100) {
                    game_global_vars[GVAR_PLAYER_REPUATION] = 100;
                }

                display_print(getmsg(&wrldmap_mesg_file, &mesg, 500));

                game_global_vars[GVAR_VATS_COUNTDOWN] = 0;
                game_global_vars[GVAR_VATS_BLOWN] = 1;
            }
        }

        if (game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] != 0) {
            if ((game_time() - game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION]) / 10 > 240) {
                // FIXME: Typo.
                debug_printf("\nWORLD MAP: Doing \"Master lair explode\" specail.\n");

                gmovie_play(MOVIE_CATHEXP, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
                game_global_vars[GVAR_DESTROY_MASTER_5] = 2;

                if (game_global_vars[GVAR_VATS_BLOWN] != 0) {
                    worldmap_script_jump(0, 0);

                    rc = 1;
                    if (LoadTownMap(TownHotSpots[TOWN_VAULT_13][0].name, TownHotSpots[TOWN_VAULT_13][0].map_idx) == -1) {
                        rc = -1;
                    }
                }

                stat_pc_add_experience(10000);

                game_global_vars[GVAR_PLAYER_REPUATION] += 10;

                // NOTE: Looks like min/max macro usage.
                if (game_global_vars[GVAR_PLAYER_REPUATION] < -100) {
                    game_global_vars[GVAR_PLAYER_REPUATION] = -100;
                } else if (game_global_vars[GVAR_PLAYER_REPUATION] > 100) {
                    game_global_vars[GVAR_PLAYER_REPUATION] = 100;
                }

                game_global_vars[GVAR_COUNTDOWN_TO_DESTRUCTION] = 0;
                game_global_vars[GVAR_MASTER_BLOWN] = 1;
            }
        }
    }

    return rc;
}

// 0x4ACC94
static int LoadTownMap(const char* filename, int map_idx)
{
    char filename_copy[16];
    char childead_map_filename[16];
    char mbdead_map_filename[16];
    int debug;
    int index;

    strcpy(childead_map_filename, "CHILDEAD.MAP");
    strcpy(mbdead_map_filename, "MBDEAD.MAP");

    debug = 1;
    if (game_global_vars[GVAR_MASTER_BLOWN] != 0 && strcmp(filename, TownHotSpots[TOWN_CATHEDRAL][0].name) == 0) {
        filename = childead_map_filename;
        map_idx = 0;
        debug = 0;
        debug_printf("WORLD MAP: Loading special \"crater\" map, filename: %s, map index#: %d.\n", childead_map_filename, 0);
    } else if (game_global_vars[GVAR_VATS_BLOWN] != 0) {
        for (index = 0; index < 5; index++) {
            if (strcmp(filename, TownHotSpots[TOWN_MILITARY_BASE][index].name) == 0) {
                filename = mbdead_map_filename;
                map_idx = 0;
                debug = 0;
                debug_printf("WORLD MAP: Loading special \"crater\" map, filename: %s, map index#: %d.\n", mbdead_map_filename, 0);
                break;
            }
        }
    }

    if (debug) {
        debug_printf("WORLD MAP: Loading map, filename: %s, map index#: %d.\n", filename, map_idx);
    }

    memset(world_buf, colorTable[0], WM_WINDOW_WIDTH * WM_WINDOW_HEIGHT);

    win_draw(world_win);
    renderPresent();

    game_global_vars[GVAR_LOAD_MAP_INDEX] = map_idx;

    // NOTE: Needed to silence compiler warnings on `const` qualifier as
    // subsequent `map_load` wants filename to be mutable.
    strcpy(filename_copy, filename);

    if (map_load(filename_copy) == -1) {
        debug_printf("\nWORLD MAP: ** Error loading town map! **\n");
        return -1;
    }

    PlayCityMapMusic();

    obj_turn_on(obj_dude, NULL);
    tile_refresh_display();

    if (bx_enable) {
        enable_box_bar_win();
    }

    debug_printf("WORLD MAP: Map load complete.\n\n");

    return 0;
}

// 0x4ACDEC
static void TargetTown(int city)
{
    int offset;

    target_xpos = 50 * city_location[city].column + 50 / 2;
    target_ypos = 50 * city_location[city].row + 50 / 2;

    offset = roll_random(0, 16);
    if (roll_random(0, 1)) {
        target_xpos += offset;
    } else {
        target_xpos -= offset;
    }

    offset = roll_random(0, 16);
    if (roll_random(0, 1)) {
        target_ypos += offset;
    } else {
        target_ypos -= offset;
    }

    world_move_init();
}

// 0x4ACE98
static int InitWorldMapData()
{
    char path[COMPAT_MAX_PATH];
    int index;
    int fid;

    bk_enable = 0;

    if (message_init(&wrldmap_mesg_file) != 1) {
        // FIXME: Message is misleading.
        debug_printf("\n *** WORLD MAP: Error loading world map graphics! ***\n");
        return -1;
    }

    snprintf(path, sizeof(path), "%s%s", msg_path, "worldmap.msg");

    if (message_load(&wrldmap_mesg_file, path) != 1) {
        // FIXME: Message is misleading.
        debug_printf("\n *** WORLD MAP: Error loading world map graphics! ***\n");
        return -1;
    }

    for (index = 0; index < WORLDMAP_FRM_COUNT; index++) {
        fid = art_id(OBJ_TYPE_INTERFACE, wmapids[index], 0, 0, 0);
        wmapbmp[index] = art_ptr_lock_data(fid, 0, 0, &(wmapidsav[index]));
        if (wmapbmp[index] == NULL) {
            break;
        }

        soundUpdate();
    }

    if (index != WORLDMAP_FRM_COUNT) {
        debug_printf("\n *** WORLD MAP: Error loading world map graphics! ***\n");

        while (--index > 0) {
            art_ptr_unlock(wmapidsav[index]);
        }

        message_exit(&wrldmap_mesg_file);

        return -1;
    }

    sea_mask = (unsigned char*)mem_malloc(263524);
    if (sea_mask == NULL) {
        debug_printf("\n *** WORLD MAP: Error loading world map graphics! ***\n");

        for (index = 0; index < WORLDMAP_FRM_COUNT; index++) {
            art_ptr_unlock(wmapidsav[index]);
        }

        message_exit(&wrldmap_mesg_file);

        return -1;
    }

    line1bit_buf = (unsigned char*)mem_malloc(263524);
    if (line1bit_buf == NULL) {
        debug_printf("\n *** WORLD MAP: Error loading world map graphics! ***\n");

        for (index = 0; index < WORLDMAP_FRM_COUNT; index++) {
            art_ptr_unlock(wmapidsav[index]);
        }

        mem_free(sea_mask);
        message_exit(&wrldmap_mesg_file);

        return -1;
    }

    memset(line1bit_buf, 0, 262500);

    if (world_win == -1) {
        world_win = win_add((screenGetWidth() - WM_WINDOW_WIDTH) / 2,
            (screenGetHeight() - WM_WINDOW_HEIGHT) / 2,
            WM_WINDOW_WIDTH,
            WM_WINDOW_HEIGHT,
            256,
            WINDOW_DONT_MOVE_TOP);
        if (world_win == -1) {
            debug_printf("\n *** WORLD MAP: Error adding world map window! ***\n");

            for (index = 0; index < WORLDMAP_FRM_COUNT; index++) {
                art_ptr_unlock(wmapidsav[index]);
            }

            mem_free(sea_mask);
            mem_free(line1bit_buf);
            message_exit(&wrldmap_mesg_file);

            return -1;
        }

    }

    world_buf = win_get_buf(world_win);

    if (tbutntgl) {
        for (index = 0; index < TOWN_COUNT; index++) {
            win_register_button_image(TownBttns[index],
                wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
                wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
                NULL,
                0);
            win_enable_button(TownBttns[index]);
        }

        win_register_button_image(WrldToggle,
            wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
            wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
            NULL,
            0);
        win_enable_button(WrldToggle);
    } else {
        for (index = 0; index < TOWN_COUNT; index++) {
            TownBttns[index] = win_register_button(world_win,
                508,
                BttnYtab[index],
                15,
                15,
                -1,
                -1,
                -1,
                500 + index,
                wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
                wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            win_register_button_sound_func(TownBttns[index], gsound_red_butt_press, gsound_red_butt_release);
        }

        WrldToggle = win_register_button(world_win,
            520,
            439,
            15,
            15,
            -1,
            -1,
            -1,
            512,
            wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
            wmapbmp[WORLDMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
            NULL,
            BUTTON_FLAG_TRANSPARENT);
        win_register_button_sound_func(WrldToggle, gsound_red_butt_press, gsound_red_butt_release);

        tbutntgl = 1;
    }

    soundUpdate();
    UpdateTownStatus();
    text_font(101);

    bx_enable = disable_box_bar_win();

    return 0;
}

// 0x4AD224
static void UnInitWorldMapData()
{
    int index;

    mem_free(line1bit_buf);
    mem_free(sea_mask);

    message_exit(&wrldmap_mesg_file);

    for (index = 0; index < WORLDMAP_FRM_COUNT; index++) {
        art_ptr_unlock(wmapidsav[index]);
    }

    intface_update_hit_points(false);
}

// 0x4AD264
static void UpdateTownStatus()
{
    int city;
    int entrance;
    int gvar;

    for (city = 1; city < TOWN_COUNT; city++) {
        for (entrance = 0; entrance < 7; entrance++) {
            gvar = ElevXgvar[city][entrance];
            if (gvar == 0) {
                break;
            }

            TwnSelKnwFlag[city][entrance] = game_global_vars[gvar];
            if (TwnSelKnwFlag[city][entrance] != 0) {
                TwnSelKnwFlag[city][entrance] = 1;
            }
        }

        if (game_global_vars[cityXgvar[city]] == 1) {
            if (WorldGrid[city_location[city].row][city_location[city].column] == 0) {
                WorldGrid[city_location[city].row][city_location[city].column] = 1;
            }

            first_visit_flag |= 1 << city;
            TwnSelKnwFlag[city][0] = 1;
        }
    }

    if (game_time() / 864000) {
        TwnSelKnwFlag[TOWN_VAULT_13][0] = 1;
        TwnSelKnwFlag[TOWN_VAULT_13][1] = 1;
        TwnSelKnwFlag[TOWN_VAULT_13][2] = 1;
        TwnSelKnwFlag[TOWN_VAULT_13][3] = 1;
    } else {
        TwnSelKnwFlag[TOWN_VAULT_13][0] = 1;
        TwnSelKnwFlag[TOWN_VAULT_13][1] = 0;
        TwnSelKnwFlag[TOWN_VAULT_13][2] = 0;
        TwnSelKnwFlag[TOWN_VAULT_13][3] = 0;
    }

    if (game_global_vars[GVAR_MASTER_BLOWN] != 0
        || TwnSelKnwFlag[TOWN_CATHEDRAL][0]
        || TwnSelKnwFlag[TOWN_CATHEDRAL][1]) {
        TwnSelKnwFlag[TOWN_SPECIAL_12][0] = 1;
    }

    if (game_global_vars[GVAR_VATS_BLOWN] != 0
        || TwnSelKnwFlag[TOWN_MILITARY_BASE][0]
        || TwnSelKnwFlag[TOWN_MILITARY_BASE][1]
        || TwnSelKnwFlag[TOWN_MILITARY_BASE][2]
        || TwnSelKnwFlag[TOWN_MILITARY_BASE][3]
        || TwnSelKnwFlag[TOWN_MILITARY_BASE][4]) {
        TwnSelKnwFlag[TOWN_SPECIAL_13][0] = 1;
    }

    if (TwnSelKnwFlag[TOWN_BROTHERHOOD][1]
        || TwnSelKnwFlag[TOWN_BROTHERHOOD][2]
        || TwnSelKnwFlag[TOWN_BROTHERHOOD][3]
        || TwnSelKnwFlag[TOWN_BROTHERHOOD][4]) {
        TwnSelKnwFlag[TOWN_SPECIAL_14][0] = 1;
    }
}

// 0x4AD48C
static int InCity(unsigned int x, unsigned int y)
{
    int city;
    int column;
    int row;

    column = x / 50;
    row = y / 50;

    for (city = 0; city < TOWN_COUNT; city++) {
        if (city_location[city].column == column && city_location[city].row == row) {
            return city;
        }
    }

    return -1;
}

// 0x4AD4E4
static void world_move_init()
{
    old_world_xpos = world_xpos;
    old_world_ypos = world_ypos;

    deltaLineX = target_xpos - world_xpos;
    deltaLineY = target_ypos - world_ypos;

    line_error = 0;
    line_index = 0;

    if (deltaLineX < 0) {
        x_line_inc = -1;
        deltaLineX = -deltaLineX;
    } else {
        x_line_inc = 1;
    }

    if (deltaLineY < 0) {
        y_line_inc = -1;
        deltaLineY = -deltaLineY;
    } else {
        y_line_inc = 1;
    }
}

// 0x4AD578
static int world_move_step()
{
    old_world_xpos = world_xpos;
    old_world_ypos = world_ypos;

    if (deltaLineX <= deltaLineY) {
        line_index++;
        if (line_index > deltaLineY) {
            return 1;
        }

        line_error += deltaLineX;
        if (line_error > 0) {
            line_error -= deltaLineY;
            world_xpos += x_line_inc;
        }

        world_ypos += y_line_inc;
    } else {
        line_index++;
        if (line_index > deltaLineX) {
            return 1;
        }

        line_error += deltaLineY;
        if (line_error > deltaLineX) {
            line_error -= deltaLineX;
            world_ypos += y_line_inc;
        }

        world_xpos += x_line_inc;
    }

    return 0;
}

// 0x4AD628
static void block_map(unsigned int x, unsigned int y, unsigned char* dst)
{
    unsigned int first_row = y / 50;
    unsigned int first_column = x / 50;
    unsigned int column;
    unsigned int row;
    unsigned int dst_y = 21;
    unsigned int dst_height = 50 - y % 50;
    unsigned int first_dst_width = 50 - x % 50;
    unsigned int dst_x;
    unsigned int dst_width;

    for (row = first_row; row < first_row + 10; row++) {
        dst_x = 22;
        dst_width = first_dst_width;

        for (column = first_column; column < first_column + 9; column++) {
            switch (WorldGrid[row][column]) {
            case 0:
                buf_fill(dst + 640 * dst_y + dst_x,
                    dst_width,
                    dst_height,
                    640,
                    colorTable[0]);
                break;
            case 1:
                dark_trans_buf_to_buf(dst + 640 * dst_y + dst_x,
                    dst_width,
                    dst_height,
                    640,
                    dst,
                    dst_x,
                    dst_y,
                    640,
                    32786);
                break;
            }

            dst_x += dst_width;
            dst_width = 50;
        }

        // Last column which can be only partially visible.
        switch (WorldGrid[row][column]) {
        case 0:
            buf_fill(dst + 640 * dst_y + dst_x,
                x % 50,
                dst_height,
                640,
                colorTable[0]);
            break;
        case 1:
            dark_trans_buf_to_buf(dst + 640 * dst_y + dst_x,
                x % 50,
                dst_height,
                640,
                dst,
                dst_x,
                dst_y,
                640,
                32786);
            break;
        }

        dst_y += dst_height;
        dst_height = 50;

        if (dst_y + 50 > 463) {
            dst_height = 463 - dst_y;

            // NOTE: Signed comparison.
            if ((int)dst_height <= 0) {
                break;
            }
        }
    }
}

// 0x4AD7D4
static void DrawTownLabels(unsigned char* src, unsigned char* dst)
{
    int index;
    int flag = first_visit_flag;

    for (index = 0; index < TOWN_COUNT; index++) {
        if ((flag & 1) != 0) {
            trans_buf_to_buf(src + (82 * 18) * index,
                82,
                18,
                82,
                dst + 640 * BttnYtab[index] + 531,
                640);
        }
        flag >>= 1;
    }
}

// 0x4AD830
static void DrawMapTime(int is_town_map)
{
    int month;
    int day;
    int year;
    unsigned char* src;

    game_time_date(&month, &day, &year);

    // Day.
    map_num(day, 2, DAY_X, DAY_Y, is_town_map);

    // Month.
    src = is_town_map ? tmapbmp[TOWNMAP_FRM_MONTHS] : wmapbmp[WORLDMAP_FRM_MONTHS];
    buf_to_buf(src + 435 * (month - 1),
        29,
        14,
        29,
        world_buf + WM_WINDOW_WIDTH * MONTH_Y + MONTH_X,
        WM_WINDOW_WIDTH);

    // Year.
    map_num(year, 4, YEAR_X, YEAR_Y, is_town_map);

    // Time.
    map_num(game_time_hour(), 4, TIME_X, TIME_Y, is_town_map);
}

// 0x4AD8EC
static void map_num(int value, int digits, int x, int y, int is_town_map)
{
    unsigned char* dst;
    unsigned char* src;

    dst = world_buf + WM_WINDOW_WIDTH * y + x + 9 * (digits - 1);
    src = is_town_map ? tmapbmp[TOWNMAP_FRM_NUMBERS] : wmapbmp[WORLDMAP_FRM_NUMBERS];

    while (digits > 0) {
        buf_to_buf(src + 9 * (value % 10), 9, 17, 360, dst, WM_WINDOW_WIDTH);
        dst -= 9;
        value /= 10;
        digits--;
    }
}

// NOTE: Very rare case - this function accepts structure and returns structure.
// The structure itself passed via stack. Address of return structure is passed
// in artificial parameter via ESI.
//
// 0x4AD988
WorldMapContext town_map(WorldMapContext ctx)
{
    // Which game we came in on -- see the bail-out in the loop below.
    unsigned int tm_load_serial = LoadGameSerial();
    const char* title;
    const char* text;
    const char* body[1];
    int index;
    int fid;
    CacheEntry* tmap_pic_key;
    char path[COMPAT_MAX_PATH];
    int tmap_sels_count;
    unsigned int time;
    int input;
    WorldMapContext new_ctx;

    new_ctx.state = -1;
    new_ctx.section = 0;

    if (ctx.town > TOWN_COUNT) {
        return new_ctx;
    }

    if (game_user_wants_to_quit != 0) {
        return new_ctx;
    }

    title = "";
    text = getmsg(&map_msg_file, &mesg, 1000);
    body[0] = text;

    if (map_save_in_game(true) == -1) {
        debug_printf("\nWORLD MAP: ** Error saving map! **\n");
        gmouse_disable(0);
        gmouse_set_cursor(1);
        gsound_play_sfx_file("iisxxxx1");
        dialog_out(title,
            body,
            1,
            169,
            116,
            colorTable[32328],
            NULL,
            colorTable[32328],
            DIALOG_BOX_LARGE);
        gmouse_enable();
        game_user_wants_to_quit = 2;
        new_ctx.state = 1;
        return new_ctx;
    }

    for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
        fid = art_id(OBJ_TYPE_INTERFACE, tmapids[index], 0, 0, 0);
        tmapbmp[index] = art_ptr_lock_data(fid, 0, 0, &(tmapidsav[index]));
        if (tmapbmp[index] == NULL) {
            break;
        }
    }

    if (index != TOWNMAP_FRM_COUNT) {
        debug_printf("\n *** WORLD MAP: Error loading town map graphics! ***\n");
        while (--index >= 0) {
            art_ptr_unlock(tmapidsav[index]);
        }
        new_ctx.state = 1;
        return new_ctx;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, ctx.town + 156, 0, 0, 0);
    tmap_pic = art_ptr_lock_data(fid, 0, 0, &tmap_pic_key);
    if (tmap_pic == NULL) {
        debug_printf("\n *** WORLD MAP: Error loading town map graphics! ***\n");
        for (index = 0; index < 8; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        return new_ctx;
    }

    text_font(101);

    onbtn = (unsigned char*)mem_malloc(4100);
    if (onbtn == NULL) {
        for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        art_ptr_unlock(tmap_pic_key);
        return new_ctx;
    }

    offbtn = (unsigned char*)mem_malloc(4100);
    if (offbtn == NULL) {
        for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        art_ptr_unlock(tmap_pic_key);
        // FIXME: Leaking `onbtn`.
        return new_ctx;
    }

    btnmsk = (unsigned char*)mem_malloc(4100);
    if (btnmsk == NULL) {
        for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        art_ptr_unlock(tmap_pic_key);
        // FIXME: Leaking `offbtn`.
        // FIXME: Leaking `onbtn`.
        return new_ctx;
    }

    for (index = 0; index < 7; index++) {
        hvrbtn[index] = (unsigned char*)mem_malloc(4100);
        if (hvrbtn[index] == NULL) {
            while (--index >= 0) {
                mem_free(hvrbtn[index]);
            }

            mem_free(onbtn);
            mem_free(offbtn);
            mem_free(btnmsk);

            for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
                art_ptr_unlock(tmapidsav[index]);
            }
            art_ptr_unlock(tmap_pic_key);
            return new_ctx;
        }
    }

    memset(onbtn, 0, 4100);
    buf_to_buf(tmapbmp[TOWNMAP_FRM_HOTSPOT_NORMAL],
        HOTSPOT_WIDTH,
        HOTSPOT_HEIGHT,
        HOTSPOT_WIDTH,
        onbtn + 164 * 12 + 69,
        164);

    memset(offbtn, 0, 4100);
    buf_to_buf(tmapbmp[TOWNMAP_FRM_HOTSPOT_PRESSED],
        HOTSPOT_WIDTH,
        HOTSPOT_HEIGHT,
        HOTSPOT_WIDTH,
        offbtn + 164 * 12 + 69,
        164);

    memset(btnmsk, 0, 4100);
    buf_to_buf(tmapbmp[TOWNMAP_FRM_HOTSPOT_PRESSED],
        HOTSPOT_WIDTH,
        HOTSPOT_HEIGHT,
        HOTSPOT_WIDTH,
        btnmsk + 164 * 12 + 69,
        164);

    trans_buf_to_buf(tmapbmp[TOWNMAP_FRM_HOTSPOT_NORMAL],
        HOTSPOT_WIDTH,
        HOTSPOT_HEIGHT,
        HOTSPOT_WIDTH,
        btnmsk + 164 * 12 + 69,
        164);

    if (message_init(&wrldmap_mesg_file) != 1) {
        for (index = 0; index < 7; index++) {
            mem_free(hvrbtn[index]);
        }

        mem_free(onbtn);
        mem_free(offbtn);
        mem_free(btnmsk);

        for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        art_ptr_unlock(tmap_pic_key);
        return new_ctx;
    }

    snprintf(path, sizeof(path), "%s%s", msg_path, "worldmap.msg");

    if (message_load(&wrldmap_mesg_file, path) != 1) {
        // FIXME: Missing `message_exit`.
        for (index = 0; index < 7; index++) {
            mem_free(hvrbtn[index]);
        }

        mem_free(onbtn);
        mem_free(offbtn);
        mem_free(btnmsk);

        for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
            art_ptr_unlock(tmapidsav[index]);
        }
        art_ptr_unlock(tmap_pic_key);
        return new_ctx;
    }

    if (world_win == -1) {
        world_win = win_add((screenGetWidth() - WM_WINDOW_WIDTH) / 2,
            (screenGetHeight() - WM_WINDOW_HEIGHT) / 2,
            WM_WINDOW_WIDTH,
            WM_WINDOW_HEIGHT,
            256,
            WINDOW_DONT_MOVE_TOP);
        if (world_win == -1) {
            debug_printf("\n *** WORLD MAP: Error adding town map window! ***\n");
            // FIXME: Reuses `index` from `hvrbtn` but releases `tmapidsav`.
            while (--index >= 0) {
                art_ptr_unlock(tmapidsav[index]);
            }
            // FIXME: Leak lots of memory.
            return new_ctx;
        }
    }

    if (tbutntgl) {
        for (index = 0; index < TOWN_COUNT; index++) {
            win_register_button_image(TownBttns[index],
                tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
                tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
                NULL,
                0);
            win_enable_button(TownBttns[index]);
        }

        win_register_button_image(WrldToggle,
            tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
            tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
            NULL,
            0);
        win_enable_button(WrldToggle);
    } else {
        for (index = 0; index < TOWN_COUNT; index++) {
            TownBttns[index] = win_register_button(world_win,
                508,
                BttnYtab[index],
                15,
                15,
                -1,
                -1,
                -1,
                500 + index,
                tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
                tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
                NULL,
                BUTTON_FLAG_TRANSPARENT);
            win_register_button_sound_func(TownBttns[index], gsound_red_butt_press, gsound_red_butt_release);
        }

        WrldToggle = win_register_button(world_win,
            520,
            439,
            15,
            15,
            -1,
            -1,
            -1,
            512,
            tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_NORMAL],
            tmapbmp[TOWNMAP_FRM_LITTLE_RED_BUTTON_PRESSED],
            NULL,
            BUTTON_FLAG_TRANSPARENT);
        win_register_button_sound_func(WrldToggle, gsound_red_butt_press, gsound_red_butt_release);

        tbutntgl = 1;
    }

    UpdateTownStatus();
    tmap_sels_count = RegTMAPsels(world_win, ctx.town);

    // CE: Hide interface.
    intface_hide();
    win_fill(display_win,
        0,
        0,
        win_width(display_win),
        win_height(display_win),
        colorTable[0]);
    win_draw(display_win);

    world_buf = win_get_buf(world_win);
    buf_to_buf(tmap_pic,
        453,
        444,
        453,
        world_buf + 640 * 20 + 20,
        640);
    trans_buf_to_buf(tmapbmp[TOWNMAP_FRM_BOX],
        640,
        480,
        640,
        world_buf,
        640);
    DrawTownLabels(tmapbmp[TOWNMAP_FRM_LABELS], world_buf);
    DrawMapTime(1);
    win_draw(world_win);
    renderPresent();

    bk_enable = map_disable_bk_processes();
    cycle_disable();
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);

    // CE: this screen drives its own pad navigation from here on. Set AFTER every early-return path
    // above, and cleared right after the loop, so no failure exit can leave the flag stuck on and
    // silently kill the pad for the rest of the session.
    gamepad_ui_set_self_nav(true);

    // Start on the first entrance, so the screen opens with something selected and the player can
    // press A immediately -- the same "the obvious choice is already picked" rule the world map's
    // town list follows.
    tm_pad_sel = tmap_sels_count > 0 ? 0 : -1;
    TMapPadPark(tm_pad_sel);

    // Latch the CURRENT button state as already-seen: the A that entered this town from the world
    // map is very likely still physically down on our first frame, and reading it as a fresh press
    // would instantly re-enter the first entrance.
    bool tmPrevUp = true;
    bool tmPrevDown = true;
    bool tmPrevLeft = true;
    bool tmPrevRight = true;
    bool tmPrevA = true;
    bool tmPrevB = true;

    while (new_ctx.state == -1) {
        sharedFpsLimiter.mark();

        time = get_time();
        input = get_input();

        // CE: pad navigation. ⚠️ Re-fetch the controller EVERY frame -- gamepad_ui_get_controller()
        // opens it lazily and gamepad_ui_tick does not run inside this blocking loop, so a handle
        // latched once on entry can be NULL for the whole screen (the trap that made two earlier
        // screens "not respond to the pad at all").
        SDL_GameController* tmPad = gamepad_ui_get_controller();
        if (tmPad != NULL) {
            SDL_GameControllerUpdate();

            // Left stick doubles as a D-pad here: the stick is what the player reaches for to
            // "drive onto" an entrance, which is the whole point of this.
            bool tmUp = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0
                || SDL_GameControllerGetAxis(tmPad, SDL_CONTROLLER_AXIS_LEFTY) < -16000;
            bool tmDown = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0
                || SDL_GameControllerGetAxis(tmPad, SDL_CONTROLLER_AXIS_LEFTY) > 16000;
            bool tmLeft = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0
                || SDL_GameControllerGetAxis(tmPad, SDL_CONTROLLER_AXIS_LEFTX) < -16000;
            bool tmRight = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0
                || SDL_GameControllerGetAxis(tmPad, SDL_CONTROLLER_AXIS_LEFTX) > 16000;
            bool tmA = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_A) != 0;
            bool tmB = SDL_GameControllerGetButton(tmPad, SDL_CONTROLLER_BUTTON_B) != 0;

            if (tmUp && !tmPrevUp) {
                TMapPadStep(0, -1, tmap_sels_count);
            }
            if (tmDown && !tmPrevDown) {
                TMapPadStep(0, 1, tmap_sels_count);
            }
            if (tmLeft && !tmPrevLeft) {
                TMapPadStep(-1, 0, tmap_sels_count);
            }
            if (tmRight && !tmPrevRight) {
                TMapPadStep(1, 0, tmap_sels_count);
            }

            // A enters the picked entrance, B leaves for the world map. Both set the result
            // DIRECTLY instead of synthesising the button's input code (514+i / 512): this loop
            // dispatches on get_input(), and feeding it fabricated codes is the trap that bit the
            // Preferences steppers. The engine's own mouse path is untouched and still works.
            if (tmA && !tmPrevA && tm_pad_sel >= 0 && tm_pad_sel < tmap_sels_count) {
                gsound_play_sfx_file("ib1p1xx1");
                new_ctx.state = 2;
                new_ctx.town = ctx.town;
                new_ctx.section = tcode_xref[tm_pad_sel];
            } else if (tmB && !tmPrevB) {
                new_ctx.state = 3;
            }

            tmPrevUp = tmUp;
            tmPrevDown = tmDown;
            tmPrevLeft = tmLeft;
            tmPrevRight = tmRight;
            tmPrevA = tmA;
            tmPrevB = tmB;
        }

        if (input >= 500 && input < 512) {
            if ((first_visit_flag & (1 << (input - 500))) != 0) {
                ctx.town = input - 500;
                UnregTMAPsels(tmap_sels_count);
                art_ptr_unlock(tmap_pic_key);

                fid = art_id(OBJ_TYPE_INTERFACE, input - 344, 0, 0, 0);
                tmap_pic = art_ptr_lock_data(fid, 0, 0, &tmap_pic_key);
                if (tmap_pic == NULL) {
                    debug_printf("\n *** WORLD MAP: Error loading town map graphic! ***\n");
                    new_ctx.state = -1;
                    break;
                }

                tmap_sels_count = RegTMAPsels(world_win, ctx.town);
                buf_to_buf(tmap_pic,
                    453,
                    444,
                    453,
                    world_buf + 640 * 20 + 20,
                    640);
                trans_buf_to_buf(tmapbmp[TOWNMAP_FRM_BOX],
                    640,
                    480,
                    640,
                    world_buf,
                    640);
                DrawTownLabels(tmapbmp[TOWNMAP_FRM_LABELS], world_buf);
                DrawMapTime(1);
            }
        } else if (input >= 514 && input < 514 + tmap_sels_count) {
            new_ctx.state = 2;
            new_ctx.town = ctx.town;
            new_ctx.section = tcode_xref[input - 514];
            debug_printf("The tparm is %d\n", new_ctx.section);
        } else {
            switch (input) {
            case 512:
                new_ctx.state = 3;
                break;
            case KEY_CTRL_Q:
            case KEY_CTRL_X:
            case KEY_F10:
                game_quit_with_confirm();
                break;
            case KEY_F12:
                dump_screen();
                break;
            case KEY_EQUAL:
            case KEY_PLUS:
                IncGamma();
                break;
            case KEY_MINUS:
            case KEY_UNDERSCORE:
                DecGamma();
                break;
            }
        }

        // Same hole as the world map: the pad can reach Load from in here, and the game the town
        // map is describing is then gone. Leave the way the quit path does.
        if (game_user_wants_to_quit != 0 || LoadGameSerial() != tm_load_serial) {
            new_ctx.state = 1;
            break;
        }

        win_draw(world_win);

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    // CE: hand the pad back before anything else -- see the note on tm_pad_sel. Leaving this set
    // would kill the shared right-stick cursor and A-as-click everywhere after this screen.
    gamepad_ui_set_self_nav(false);
    tm_pad_sel = -1;

    UnregTMAPsels(tmap_sels_count);
    buf_to_buf(tmap_pic, 453, 444, 453, world_buf + 640 * 20 + 20, 640);
    trans_buf_to_buf(tmapbmp[TOWNMAP_FRM_BOX], 640, 480, 640, world_buf, 640);
    DrawTownLabels(tmapbmp[TOWNMAP_FRM_LABELS], world_buf);
    DrawMapTime(1);
    DrawTMAPsels(world_win, ctx.town);
    win_draw(world_win);
    renderPresent();

    for (index = 0; index < TOWNMAP_FRM_COUNT; index++) {
        art_ptr_unlock(tmapidsav[index]);
    }
    art_ptr_unlock(tmap_pic_key);

    mem_free(onbtn);
    mem_free(offbtn);
    mem_free(btnmsk);

    for (index = 0; index < 8; index++) {
        mem_free(hvrbtn[index]);
    }

    message_exit(&wrldmap_mesg_file);

    if (bk_enable) {
        map_enable_bk_processes();
    }

    cycle_enable();

    return new_ctx;
}

// 0x4AE4C4
void KillWorldWin()
{
    // ⚠️ DO NOT GATE THIS ON A SEPARATE "is it open" FLAG (this used to test wwin_flag).
    // `game_reset()` calls `init_world_map()`, which cleared that flag -- and `PrepLoad` calls
    // `game_reset()`. So loading a save while this window is up
    // DISARMS THE WINDOW'S OWN TEARDOWN: the flag says "no window", the window is still there, and
    // it survives painted over the running game for the rest of the session. That is the Shady
    // Sands town map sitting on top of a live map with its interface bar and message log both
    // updating underneath -- the logic to leave the screen was already correct and running, only
    // the teardown had been quietly switched off.
    //
    // Same family as the other load-time wreckage, inverted: not a stale global that survives a
    // load, but a live resource whose bookkeeping a load CLEARS. The window handle is the truth.
    if (world_win != -1) {
        win_delete(world_win);
        world_win = -1;
    }

    tbutntgl = 0;
    dropbtn = 0;
}

// CE: park the mouse on the picked entrance's HOTSPOT MARKER, which is what makes the engine's own
// hover art (hvrbtn -- the marker lighting up plus the location name) appear. Nothing new is drawn
// here; the selection is expressed entirely through the vanilla hover, so it looks exactly like the
// mouse version.
//
// ⚠️ Aim at the MARKER, not at the button's centre. The button is 164x25 but carries a mask
// (win_register_button_mask(.., btnmsk)), so only the pixels the mask covers are live -- and the
// mask is the 25x13 marker blitted at offset (69,12). The geometric centre of the button lands on
// masked-out pixels, where the engine sees no button at all.
static void TMapPadPark(int sel)
{
    Rect tmWin;

    if (sel < 0 || win_get_rect(world_win, &tmWin) != 0) {
        return;
    }

    mouse_set_position(tmWin.ulx + brnpos[sel].x + 69 + HOTSPOT_WIDTH / 2,
        tmWin.uly + brnpos[sel].y + 12 + HOTSPOT_HEIGHT / 2);
}

// CE: step the entrance selection one 4-way direction, spatially -- nearest hotspot AHEAD in the
// pushed direction, scored by travel along that axis plus a perpendicular penalty. Same rule as
// gpui_menu_snap, kept local because this screen owns its cursor under self_nav. Entrances are
// laid out freely over the town picture (not a list), so a plain prev/next would wander.
static void TMapPadStep(int dirX, int dirY, int count)
{
    int best = -1;
    long bestScore = 0;
    int index;

    if (count <= 0) {
        return;
    }

    if (tm_pad_sel < 0) {
        tm_pad_sel = 0;
        TMapPadPark(tm_pad_sel);
        return;
    }

    for (index = 0; index < count; index++) {
        if (index == tm_pad_sel) {
            continue;
        }

        int rx = brnpos[index].x - brnpos[tm_pad_sel].x;
        int ry = brnpos[index].y - brnpos[tm_pad_sel].y;
        int along = dirX != 0 ? dirX * rx : dirY * ry;
        int perp = dirX != 0 ? ry : rx;

        if (along <= 2) {
            continue; // not ahead the way we are pushing
        }

        long score = (long)along + 3L * labs(perp);
        if (best < 0 || score < bestScore) {
            best = index;
            bestScore = score;
        }
    }

    // No wrap: running out of entrances that way just stays put, so the player always knows where
    // the selection is instead of it jumping across the map.
    if (best >= 0) {
        tm_pad_sel = best;
        TMapPadPark(tm_pad_sel);
        gsound_play_sfx_file("ib1p1xx1");
    }
}

// 0x4AE4F0
static void HvrOffBtn(int btn, int input)
{
    int entrance;
    int px;
    unsigned char mask[4100];

    for (entrance = 0; entrance < 7; entrance++) {
        if (brnpos[entrance].bid == btn) {
            break;
        }
    }

    for (px = 0; px < 4100; px++) {
        mask[px] = hvrbtn[entrance][px] | btnmsk[px];
    }

    mask_buf_to_buf(tmap_pic + 453 * (brnpos[entrance].y - 20) + brnpos[entrance].x - 20,
        164,
        25,
        453,
        mask,
        164,
        world_buf + 640 * brnpos[entrance].y + brnpos[entrance].x,
        640);
}

// 0x4AE5B8
static int RegTMAPsels(int win, int city)
{
    int count;
    int color = colorTable[992] | 0x10000;
    int v4 = 0;
    int index;
    char name[64];
    int name_x;
    int button_x;
    int button_y;

    if (city == TOWN_CATHEDRAL && game_global_vars[GVAR_MASTER_BLOWN] != 0) {
        city = TOWN_SPECIAL_12;
        v4 = 1;
    } else if (city == TOWN_MILITARY_BASE && game_global_vars[GVAR_VATS_BLOWN] != 0) {
        city = TOWN_SPECIAL_13;
        v4 = 2;
    }

    count = 0;
    for (index = 0; index < 7; index++) {
        TownHotSpotEntry* entry = &(TownHotSpots[city][index]);
        if (entry->x == 0 || entry->y == 0) {
            break;
        }

        if (TwnSelKnwFlag[city][index]) {
            memset(hvrbtn[count], 0, 164 * 25);
            buf_to_buf(tmapbmp[TOWNMAP_FRM_HOTSPOT_NORMAL],
                HOTSPOT_WIDTH,
                HOTSPOT_HEIGHT,
                HOTSPOT_WIDTH,
                hvrbtn[count] + 164 * 12 + 69,
                164);

            switch (v4) {
            case 0:
                strcpy(name, getmsg(&wrldmap_mesg_file, &mesg, 10 * city + index + 200));
                break;
            case 1:
                strcpy(name, getmsg(&wrldmap_mesg_file, &mesg, 290));
                break;
            case 2:
                strcpy(name, getmsg(&wrldmap_mesg_file, &mesg, 501));
                break;
            }

            name_x = (164 - text_width(name)) / 2;
            if (name_x < 0) {
                name_x = 0;
            }

            text_to_buf(hvrbtn[count] + name_x, name, 164, 164, color);

            button_x = entry->x - 57;
            button_y = entry->y - 6;

            TMSelBttns[count] = win_register_button(win,
                entry->x - 57,
                entry->y - 6,
                164,
                25,
                -1,
                -1,
                -1,
                514 + count,
                onbtn,
                offbtn,
                hvrbtn[count],
                BUTTON_FLAG_TRANSPARENT);
            win_register_button_mask(TMSelBttns[count], btnmsk);
            win_register_button_func(TMSelBttns[count], NULL, HvrOffBtn, NULL, NULL);

            debug_printf("button found count=%d, bcount=%d, btnid=%d\n", index, count, TMSelBttns[count]);

            win_register_button_sound_func(TMSelBttns[count], gsound_med_butt_press, NULL);

            brnpos[count].x = button_x;
            brnpos[count].y = button_y;
            brnpos[count].bid = TMSelBttns[count];

            tcode_xref[count] = index;

            count++;
        }
    }

    debug_printf("button total bcount=%d\n", count);

    return count;
}

// 0x4AE8AC
static void UnregTMAPsels(int count)
{
    int index;

    for (index = 0; index < count; index++) {
        win_delete_button(TMSelBttns[index]);
    }
}

// 0x4AE8D0
static void DrawTMAPsels(int win, int city)
{
    int index;
    TownHotSpotEntry* entry;

    if (city == TOWN_CATHEDRAL && game_global_vars[GVAR_MASTER_BLOWN] != 0) {
        city = TOWN_SPECIAL_12;
    } else if (city == TOWN_MILITARY_BASE && game_global_vars[GVAR_VATS_BLOWN] != 0) {
        city = TOWN_SPECIAL_13;
    }

    for (index = 0; index < 7; index++) {
        entry = &(TownHotSpots[city][index]);
        if (entry->x == 0 || entry->y == 0) {
            break;
        }

        if (TwnSelKnwFlag[city][index]) {
            trans_buf_to_buf(onbtn,
                164,
                25,
                164,
                win_get_buf(win) + entry->x - 57 + 640 * (entry->y - 6),
                640);
        }
    }
}

// 0x4AE980
int worldmap_script_jump(int city, int a2)
{
    int v1;
    int rc;

    if (city < 0 || city >= TOWN_COUNT) {
        return -1;
    }

    if (InCity(world_xpos, world_ypos) != city) {
        rc = 0;
        TargetTown(city);
        world_move_init();
        CalcTimeAdder();

        wmap_mile = 0;
        v1 = 0;

        while (rc == 0) {
            switch (WorldTerraTable[world_ypos / 50][world_xpos / 50]) {
            case 1:
                v1--;
                if (v1 <= 0) {
                    v1 = 2;
                    rc = world_move_step();
                }
                inc_game_time(time_adder);
                break;
            case 2:
                rc = world_move_step();
                v1--;
                if (v1 <= 0 && rc == 0) {
                    v1 = 4;
                    rc = world_move_step();
                }
                break;
            default:
                rc = world_move_step();
                v1 = 0;
                inc_game_time(time_adder);
                break;
            }

            wmap_mile++;
            if (wmap_mile >= wmap_day) {
                wmap_mile = 0;
                partyMemberRestingHeal(24);
            }
        }
    }

    return 0;
}

// 0x4AEA88
static void CalcTimeAdder()
{
    float outdoorsman;

    outdoorsman = (float)skill_level(obj_dude, SKILL_OUTDOORSMAN);
    if (outdoorsman > 100.0f) {
        outdoorsman = 100.0f;
    }

    wmap_day = (unsigned int)((outdoorsman / 100.0f) * 60.0f + 60.0f);
    time_adder = (unsigned int)(864000.0f / (float)wmap_day);

    // TODO: Check, float-double-int mess.
    time_adder = (unsigned int)(time_adder * (1.0 - perk_level(PERK_PATHFINDER) * 0.25f));
}

// 0x0x4AEB4C
int xlate_mapidx_to_town(int map_idx)
{
    return map_idx >= 0 && map_idx < MAP_COUNT ? xlate_town_table[map_idx] : -1;
}

// 0x4AEB68
int PlayCityMapMusic()
{
    int map_idx;

    map_idx = map_get_index_number();
    if (map_idx != -1) {
        return gsound_background_play_level_music(CityMusic[map_idx], 12);
    }

    debug_printf("\nMAP: Failed to find map ID for music!");

    return -1;
}

// 0x4AEBA0
static void BlackOut()
{
    int index;

    for (index = 0; index < TOWN_COUNT; index++) {
        win_disable_button(TownBttns[index]);
    }

    win_disable_button(WrldToggle);

    memset(world_buf, colorTable[0], WM_WINDOW_WIDTH * WM_WINDOW_HEIGHT);
}

} // namespace fallout
