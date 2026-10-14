#include "game/gdialog.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game/actions.h"
#include "game/combat.h"
#include "game/game_trace.h"
#include "game/light.h"
#include "game/combat_rt.h"
#include "game/combatai.h"
#include "game/config.h"
#include "game/gconfig.h"
#include "game/critter.h"
#include "game/cycle.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gamepad_ui.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/interact_prompt.h"
#include "game/item.h"
#include "game/lip_sync.h"
#include "game/message.h"
#include "game/object.h"
#include "game/party.h" // isPartyMember, for the follower-only dialogue extras
#include "game/perk.h"
#include "game/proto.h"
#include "game/reaction.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/skill.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "int/dialog.h"
#include "int/window.h"
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

#define GAME_DIALOG_WINDOW_WIDTH 640
#define GAME_DIALOG_WINDOW_HEIGHT 480

#define GAME_DIALOG_REPLY_WINDOW_X 135
#define GAME_DIALOG_REPLY_WINDOW_Y 225
#define GAME_DIALOG_REPLY_WINDOW_WIDTH 379
#define GAME_DIALOG_REPLY_WINDOW_HEIGHT 58

#define GAME_DIALOG_OPTIONS_WINDOW_X 127
#define GAME_DIALOG_OPTIONS_WINDOW_Y 335
#define GAME_DIALOG_OPTIONS_WINDOW_WIDTH 393
#define GAME_DIALOG_OPTIONS_WINDOW_HEIGHT 117

#define GAME_DIALOG_REVIEW_WINDOW_WIDTH 640
#define GAME_DIALOG_REVIEW_WINDOW_HEIGHT 480

#define DIALOG_REVIEW_ENTRIES_CAPACITY 80

#define DIALOG_OPTION_ENTRIES_CAPACITY 30

typedef enum GameDialogReviewWindowButton {
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT,
} GameDialogReviewWindowButton;

typedef enum GameDialogReviewWindowButtonFrm {
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT,
} GameDialogReviewWindowButtonFrm;

typedef enum GameDialogReaction {
    GAME_DIALOG_REACTION_GOOD = 49,
    GAME_DIALOG_REACTION_NEUTRAL = 50,
    GAME_DIALOG_REACTION_BAD = 51,
} GameDialogReaction;

typedef struct GameDialogReviewEntry {
    int replyMessageListId;
    int replyMessageId;
    // Can be NULL.
    char* replyText;
    int optionMessageListId;
    int optionMessageId;
    char* optionText;
} GameDialogReviewEntry;

typedef struct GameDialogOptionEntry {
    int messageListId;
    int messageId;
    int reaction;
    int proc;
    int btn;
    int field_14;
    char text[900];
} GameDialogOptionEntry;

typedef struct GameDialogBlock {
    Program* program;
    int replyMessageListId;
    int replyMessageId;
    int offset;

    // NOTE: The is something odd about next two members. There are 2700 bytes,
    // which is 3 x 900, but anywhere in the app only 900 characters is used.
    // The length of text in [DialogOptionEntry] is definitely 900 bytes. There
    // are two possible explanations:
    // - it's an array of 3 elements.
    // - there are three separate elements, two of which are not used, therefore
    // they are not referenced anywhere, but they take up their space.
    //
    // See `gDialogProcessChoice` for more info how this unreferenced range plays
    // important role.
    char replyText[900];
    char field_394[1800];
    GameDialogOptionEntry options[DIALOG_OPTION_ENTRIES_CAPACITY];
} GameDialogBlock;

static int gdialog_hide();
static int gdialog_unhide();
static int gdialog_unhide_reply();
static int gdAddOption(int messageListId, int messageId, int reaction);
static int gdAddOptionStr(int messageListId, const char* text, int reaction);
static void gdReviewFree();
static int gdAddReviewReply(int messageListId, int messageId);
static int gdAddReviewReplyStr(const char* string);
static int gdAddReviewOptionChosen(int messageListId, int messageId);
static int gdAddReviewOptionChosenStr(const char* string);
static int gDialogProcess();
static void gDialogProcessCleanup();
static int gDialogProcessChoice(int a1);
static int gDialogProcessInit();
static void reply_arrow_up(int btn, int keyCode);
static void reply_arrow_down(int btn, int keyCode);
static void reply_arrow_restore(int btn, int keyCode);
static void gDialogProcessHighlight(int index);
static void gDialogProcessUnHighlight(int index);
static void gDialogProcessReply();
static void gDialogProcessUpdate();
static int gDialogProcessExit();
static void demo_copy_title(int win);
static void demo_copy_options(int win);
static void gDialogRefreshOptionsRect(int win, Rect* drawRect);
static void head_bk();
static void talk_to_scroll_subwin(int win, int a2, unsigned char* a3, unsigned char* a4, unsigned char* a5, int a6, int a7);
static int gdialog_review();
static int gdialog_review_init(int* win);
static int gdialog_review_exit(int* win);
static void gdialog_review_display(int win, int origin);
static int text_to_rect_wrapped(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color);
static int text_to_rect_func(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color, int a7);
static int talk_to_create_barter_win();
static void talk_to_destroy_barter_win();
static void dialogue_barter_cleanup_tables();
static void talk_to_pressed_barter(int btn, int keyCode);
static void talk_to_pressed_about(int btn, int keyCode);
static void talk_to_pressed_review(int btn, int keyCode);
static int talk_to_create_dialogue_win();
static void talk_to_destroy_dialogue_win();
static int talk_to_create_background_window();
static int talk_to_refresh_background_window();
static int talk_to_create_head_window();
static void talk_to_destroy_head_window();
static void talk_to_set_up_fidget(int headFrmId, int reaction);
static void talk_to_wait_for_fidget();
static void talk_to_play_transition(int anim);
static void talk_to_translucent_trans_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, unsigned char* a9, unsigned char* a10);
static void talk_to_display_frame(Art* headFrm, int frame);
static void talk_to_blend_table_init();
static void talk_to_blend_table_exit();
static int about_init();
static void about_exit();
static void about_loop();
static int about_process_input(int input);
static void about_update_display(unsigned char should_redraw);
static void about_clear_display(unsigned char should_redraw);
static void about_reset_string();
static void about_process_string();
static int about_lookup_word(const char* search);
static int about_lookup_name(const char* search);

// 0x504FDC
static int fidgetFID = 0;

// 0x504FE0
static CacheEntry* fidgetKey = NULL;

// 0x504FE4
static Art* fidgetFp = NULL;

// 0x504FE8
static int backgroundIndex = 2;

// 0x504FEC
static int lipsFID = 0;

// 0x504FF0
static CacheEntry* lipsKey = NULL;

// 0x504FF4
static Art* lipsFp = NULL;

// 0x504FF8
static bool gdialog_speech_playing = false;

// 0x504FFC
static unsigned char* headWindowBuffer = NULL;

// 0x505000
static int dialogue_state = 0;

// 0x505004
static int dialogue_switch_mode = 0;

// 0x505008
static int gdialog_state = -1;

// 0x50500C
static bool gdDialogWentOff = false;

// 0x505010
static bool gdDialogTurnMouseOff = false;

// 0x505014
Object* dialog_target = NULL;

// The decorative ANIMATE_FOREVER loops a conversation's anim_stop ended, put back when it closes
// (anim_forever_snapshot / anim_forever_restore).
#define GDIALOG_FOREVER_MAX 32
static Object* gdialog_forever_owners[GDIALOG_FOREVER_MAX];
static int gdialog_forever_anims[GDIALOG_FOREVER_MAX];
static int gdialog_forever_count = 0;

// 0x505018
int dialogue_scr_id = -1;

// 0x50501C
static Object* peon_table_obj = NULL;

// 0x505020
static Object* barterer_table_obj = NULL;

// 0x505024
static Object* barterer_temp_obj = NULL;

// 0x505028
static int gdBarterMod = 0;

// 0x50502C
static int dialogueBackWindow = -1;

// 0x505030
static int dialogueWindow = -1;

// 0x505034
static Rect backgrndRects[8] = {
    { 126, 14, 152, 40 },
    { 488, 14, 514, 40 },
    { 126, 188, 152, 214 },
    { 488, 188, 514, 214 },
    { 152, 14, 488, 24 },
    { 152, 204, 488, 214 },
    { 126, 40, 136, 188 },
    { 504, 40, 514, 188 },
};

// 0x5050B4
static int talk_need_to_center = 1;

// 0x5050B8
static bool can_start_new_fidget = false;

// 0x5050BC
static int gd_replyWin = -1;

// 0x5050C0
static int gd_optionsWin = -1;

// 0x5050C4
static int gDialogMusicVol = -1;

// 0x5050C8
static int gdCenterTile = -1;

// 0x5050CC
static int gdPlayerTile = -1;

// 0x5050D0
int dialogue_head = 0;

// 0x5050D4
unsigned char* light_BlendTable = NULL;

// 0x5050D8
unsigned char* dark_BlendTable = NULL;

// 0x5050DC
static int dialogue_just_started = 0;

// 0x5050E0
static int dialogue_seconds_since_last_input = 0;

// Maps phoneme to talking head frame.
//
// 0x5050E4
static int head_phoneme_lookup[PHONEME_COUNT] = {
    0,
    3,
    1,
    1,
    3,
    1,
    1,
    1,
    7,
    8,
    7,
    3,
    1,
    8,
    1,
    7,
    7,
    6,
    6,
    2,
    2,
    2,
    2,
    4,
    4,
    5,
    5,
    2,
    2,
    2,
    2,
    2,
    6,
    2,
    2,
    5,
    8,
    2,
    2,
    2,
    2,
    8,
};

// 0x50518C
static int dialog_state_fix = 0;

// 0x505190
static int boxesWereDisabled = 0;

// 0x505194
static int curReviewSlot = 0;

// 0x505198
static int gdNumOptions = 0;

// CE: bumped every time the reply list is rebuilt (gDialogProcessUpdate). The controller reply
// nav in gDialogProcess watches it and re-seeds its highlight whenever it changes, so a NEW set
// of options always comes up with option 0 highlighted. Without it only the FIRST set of the
// conversation was seeded: after every answer the new replies were drawn with nothing highlighted
// and the selection index still pointed at the option just answered -- you had to nudge up/down
// to see (and correct) where the cursor actually was. Monotonic and global on purpose: nested
// dialogue levels each keep their own last-seen value, so returning to an outer level re-seeds
// there too.
static unsigned int gdOptionGen = 0;

// 0x50519C
static int gReplyWin = -1;

// 0x5051A0
static int gOptionWin = -1;

// 0x5051A4
static int gdReenterLevel = 0;

// 0x5051A8
static bool gdReplyTooBig = false;

// 0x5051B4
static const char* react_strs[3] = {
    "Said Good",
    "Said Neutral",
    "Said Bad",
};

// 0x5051C0
static int dialogue_subwin_len = 0;

// 0x5051C4
static CacheEntry* reviewKeys[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT] = {
    INVALID_CACHE_ENTRY,
    INVALID_CACHE_ENTRY,
    INVALID_CACHE_ENTRY,
    INVALID_CACHE_ENTRY,
    INVALID_CACHE_ENTRY,
    INVALID_CACHE_ENTRY,
};

// 0x5051DC
static CacheEntry* reviewBackKey = INVALID_CACHE_ENTRY;

// 0x5051E0
static CacheEntry* reviewDispBackKey = INVALID_CACHE_ENTRY;

// 0x5051E4
static unsigned char* reviewDispBuf = NULL;

// 0x5051E8
static int reviewFidWids[GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT] = {
    35,
    35,
    82,
};

// 0x5051F4
static int reviewFidLens[GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT] = {
    35,
    37,
    46,
};

// 0x505200
static int reviewFids[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT] = {
    89, // di_bgdn1.frm - dialog big down arrow
    90, // di_bgdn2.frm - dialog big down arrow
    87, // di_bgup1.frm - dialog big up arrow
    88, // di_bgup2.frm - dialog big up arrow
    91, // di_done1.frm - dialog big done button up
    92, // di_done2.frm - dialog big done button down
};

// 0x505218
static int dgAboutWinKey = -1;

// 0x50521C
static int gdAboutRebuildButtons = 1;

// 0x505220
static unsigned char* gdAboutWinBuf = NULL;

// 0x505224
static bool dial_win_created = false;

// 0x505230
static int about_win = -1;

// 0x505234
static unsigned char* about_win_buf = NULL;

// 0x505238
static CacheEntry* about_button_up_key = NULL;

// 0x50523C
static CacheEntry* about_button_down_key = NULL;

// 0x505240
static char* about_input_string = NULL;

// 0x505244
static char about_input_cursor = '_';

// 0x505248
static Rect about_input_rect = { 22, 32, 265, 45 };

// 0x58CD50
static Rect optionRect;

// 0x58CD60
static Rect replyRect;

// 0x58CD70
static GameDialogReviewEntry reviewList[DIALOG_REVIEW_ENTRIES_CAPACITY];

// 0x58D4F0
unsigned char light_GrayTable[256];

// 0x58D5F0
unsigned char dark_GrayTable[256];

// 0x58D6F0
static unsigned char* backgrndBufs[8];

// 0x58D710
static unsigned int about_last_time;

// 0x58D714
static char about_restore_string[900];

// 0x58DA98
static int about_win_width;

// 0x58DA9C
static int about_input_index;

// 0x58DAA0
static int about_old_font;

// 0x58DAA4
static int reviewOldFont;

// 0x58DAA8
static CacheEntry* dialogue_rest_Key1;

// 0x58DAAC
static CacheEntry* dialogue_rest_Key2;

// 0x58DAB0
static CacheEntry* dialogue_redbut_Key2;

// 0x58DAB4
static CacheEntry* dialogue_redbut_Key1;

// 0x58DAB8
static int dialogue_bids[3];

// CE: private copies of the small red button art with a "Y" / "X" hint baked in, used
// for the dialogue barter / ask-about buttons when a controller is present (the letter
// must live in the button image -- GNW_button_refresh re-blits it every frame -- so we
// never draw on the shared art). Freed in talk_to_destroy_dialogue_win.
static unsigned char* gd_barter_hint_up = NULL;
static unsigned char* gd_ask_hint_up = NULL;

// 0x58DAC4
static int talkOldFont;

// 0x58DAC8
static GameDialogBlock dialogBlock;

// 0x5951AC
static CacheEntry* upper_hi_key;

// 0x5951B0
static int lower_hi_len;

// 0x5951B4
static int lower_hi_wid;

// 0x5951B8
static int upper_hi_wid;

// Yellow highlight blick effect.
//
// 0x5951BC
static Art* lower_hi_fp;

// 0x5951C0
static int upper_hi_len;

// 0x5951C4
static CacheEntry* lower_hi_key;

// White highlight blick effect.
//
// This effect appears at the top-right corner on dialog display. Together with
// [gDialogLowerHighlight] it gives an effect of depth of the monitor.
//
// 0x5951C8
static Art* upper_hi_fp;

// 0x5951CC
static int oldFont;

// 0x5951D0
static int fidgetAnim;

// 0x5951D4
static unsigned int fidgetTocksPerFrame;

// 0x5951D8
static unsigned int fidgetLastTime;

// 0x5951DC
static int fidgetFrameCounter;

// MODS "Script lines as floating text" (cfg script_float_lines) -- see gDialogSayMessage.
// ⚠️ DEFAULT ON, AND THAT IS HIS CALL, NOT THE MODS HOUSE RULE (every other row defaults to what
// shipped): *"rovnou zapnuté a 1"* (2026-10-10), having seen what the vanilla path looks like.
static bool gdialog_script_float = true;

bool gdialog_get_script_float()
{
    return gdialog_script_float;
}

void gdialog_set_script_float(bool on)
{
    gdialog_script_float = on;
}

// 0x43DE08
int gdialog_init()
{
    int scriptFloat = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_SCRIPT_FLOAT_KEY, &scriptFloat);
    gdialog_script_float = scriptFloat != 0;
    return 0;
}

// 0x43DE0C
int gdialog_reset()
{
    gdialog_free_speech();
    return 0;
}

// 0x43DE0C.
int gdialog_exit()
{
    gdialog_free_speech();
    return 0;
}

// 0x43DE18
bool dialog_active()
{
    return dialog_state_fix != 0;
}

// True while the dialogue is in BARTER (trade) mode -- dialogue_state 4, set when
// barter_inventory opens. The controller UI hides the pointer during ordinary reply
// selection but must keep it here, since bartering means pointing at items to trade.
bool dialog_is_bartering()
{
    return dialogue_state == 4;
}

// 0x43DE28
// CE: Re-arm the conversation's own pad edge detection. A dialogue can open while a face button is
// already HELD -- the commonest case by far is X, because X is RUN and scripts stop you mid-stride
// ("držím běh xko a zastaví mě npc, že mi chce něco říct, tak se objeví okno, zda se ho chci na
// něco zeptat"). The edge statics in gdialog_gamepad_keycode persist between conversations, so a
// button that never went up still read as a fresh press on the first frame of the new one, and the
// "ask me about" window opened before the NPC had said a word.
//
// Stamping the LIVE state at entry is the whole fix: a button held from the world is already spent,
// and only a genuine press made INSIDE the conversation counts. Same rule the gameplay shortcuts
// apply after every modal screen (gamepad_ui_bump_input_epoch), just local to this loop.
static bool gdialog_pad_rearm = false;
static bool gdialog_pad_rearm_cancel = false;

void gdialog_enter(Object* target, int a2)
{
    gdDialogWentOff = false;
    gdialog_pad_rearm = true;
    gdialog_pad_rearm_cancel = true;
    // And invalidate the GAMEPLAY shortcut edges too, so a button held from the world cannot fire
    // one of those on the frame the conversation opens.
    gamepad_ui_bump_input_epoch();

    // Vanilla refused a conversation during a turn-based encounter -- you cannot start talking in
    // the middle of someone's turn. Realtime has no turns, and its combat bit stays warm for as
    // long as ANY critter anywhere is chasing, so this guard had quietly become "you may never
    // talk to anyone while a fight is going on somewhere on the map" -- silently, with no message.
    // Ask the narrow question instead: is somebody actually coming for me?
    if (combat_rt_under_threat()) {
        // A guard's "who goes there / intruder" conversation skipped because a fight is on: the
        // other half of "they say nothing, they just shoot" (2026-10-09). Traced so it can be told
        // apart from a script that never asked to talk at all.
        trace_event(TRACE_FLOW, "dialog-refused", "pid=%d id=%d name=%s why=under-threat",
            target->pid, target->id, object_name(target));
        return;
    }

    if (target->sid == -1) {
        return;
    }

    if (PID_TYPE(target->pid) != OBJ_TYPE_ITEM && SID_TYPE(target->sid) != SCRIPT_TYPE_SPATIAL) {
        MessageListItem messageListItem;

        if (make_path_func(obj_dude, obj_dude->tile, target->tile, NULL, 0, obj_sight_blocking_at) == 0) {
            // A script's request (a2 == 0) used to die here without a word anywhere; see op_dialogue_system_enter.
            trace_event(TRACE_FLOW, "dialog-refused", "pid=%d id=%d name=%s why=no-sight-path",
                target->pid, target->id, object_name(target));
            // You can't see there.
            messageListItem.num = 660;
            if (message_search(&proto_main_msg_file, &messageListItem)) {
                if (a2) {
                    display_print(messageListItem.text);
                } else {
                    debug_printf(messageListItem.text);
                }
            } else {
                debug_printf("\nError: gdialog: Can't find message!");
            }
            return;
        }

        if (tile_dist(obj_dude->tile, target->tile) > 12) {
            trace_event(TRACE_FLOW, "dialog-refused", "pid=%d id=%d name=%s why=too-far dist=%d",
                target->pid, target->id, object_name(target), tile_dist(obj_dude->tile, target->tile));
            // Too far away.
            messageListItem.num = 661;
            if (message_search(&proto_main_msg_file, &messageListItem)) {
                if (a2) {
                    display_print(messageListItem.text);
                } else {
                    debug_printf(messageListItem.text);
                }
            } else {
                debug_printf("\nError: gdialog: Can't find message!");
            }
            return;
        }
    }

    // Tear down the gamepad interact prompt (floating "talk" hand + outline on a nearby
    // NPC like a companion). It's driven from gamepad_ui_tick, which stops running once
    // this conversation takes over its own modal loop -- so without this its last frame
    // would linger on screen over the talking head. (Idempotent when nothing is shown.)
    interact_prompt_reset();

    gdCenterTile = tile_center_tile;
    gdBarterMod = 0;
    gdPlayerTile = obj_dude->tile;
    map_disable_bk_processes();

    dialog_state_fix = 1;
    dialog_target = target;
    dialogue_just_started = 1;

    if (target->sid != -1) {
        exec_script_proc(target->sid, SCRIPT_PROC_TALK);
    }

    Script* script;
    if (scr_ptr(target->sid, &script) == -1) {
        gmouse_3d_on();
        map_enable_bk_processes();
        scr_exec_map_update_scripts();
        dialog_state_fix = 0;
        return;
    }

    if (script->scriptOverrides || dialogue_state != 4) {
        dialogue_just_started = 0;
        map_enable_bk_processes();
        scr_exec_map_update_scripts();
        dialog_state_fix = 0;
        return;
    }

    gdialog_free_speech();

    if (gdialog_state == 1) {
        switch (dialogue_switch_mode) {
        case 2:
            talk_to_destroy_dialogue_win();
            break;
        case 1:
            talk_to_destroy_barter_win();
            break;
        default:
            switch (dialogue_state) {
            case 1:
                talk_to_destroy_dialogue_win();
                break;
            case 4:
                talk_to_destroy_barter_win();
                break;
            }
            break;
        }
        scr_dialogue_exit();
    }

    gdialog_state = 0;
    dialogue_state = 0;

    int tile = obj_dude->tile;
    if (gdPlayerTile != tile) {
        gdCenterTile = tile;
    }

    if (gdDialogWentOff) {
        tile_scroll_to(gdCenterTile, 2);
    }

    map_enable_bk_processes();
    scr_exec_map_update_scripts();

    // The decorative loops the conversation stopped (scr_dialogue_init) -- after map_update, so a
    // prop its own script already restarted is busy and skipped.
    anim_forever_restore(gdialog_forever_owners, gdialog_forever_anims, gdialog_forever_count);
    gdialog_forever_count = 0;

    dialog_state_fix = 0;
}

// 0x43E0A0
void dialogue_system_enter()
{
    game_state_update();

    gdDialogTurnMouseOff = true;

    soundUpdate();
    gdialog_enter(dialog_target, 0);
    soundUpdate();

    if (gdPlayerTile != obj_dude->tile) {
        gdCenterTile = obj_dude->tile;
    }

    if (gdDialogWentOff) {
        tile_scroll_to(gdCenterTile, 2);
    }

    game_state_request(GAME_STATE_2);

    game_state_update();
}

// 0x43E10C
void gdialog_setup_speech(const char* audioFileName)
{
    char name[16];
    if (art_get_base_name(OBJ_TYPE_HEAD, dialogue_head & 0xFFF, name) == -1) {
        return;
    }

    if (lips_load_file(audioFileName, name) == -1) {
        return;
    }

    gdialog_speech_playing = true;

    lips_play_speech();

    debug_printf("Starting lipsynch speech");
}

// 0x43E164
void gdialog_free_speech()
{
    if (gdialog_speech_playing) {
        debug_printf("Ending lipsynch system");
        gdialog_speech_playing = false;

        lips_free_speech();
    }
}

// 0x43E18C
int gDialogEnableBK()
{
    add_bk_process(head_bk, "head");
    return 0;
}

// 0x43E19C
int gDialogDisableBK()
{
    remove_bk_process(head_bk);
    return 0;
}

// 0x43E1AC
int scr_dialogue_init(int headFid, int reaction)
{
    if (dialogue_state == 1) {
        return -1;
    }

    if (gdialog_state == 1) {
        return 0;
    }

    // Decorative loops (Gizmo's sign...) stopped below come back when the conversation ends.
    gdialog_forever_count = anim_forever_snapshot(gdialog_forever_owners, gdialog_forever_anims, GDIALOG_FOREVER_MAX);
    anim_stop();

    // Every conversation, named: who, which head (-1 = no head: the top window shows the world
    // around the speaker instead), and whether a fight was on. 2026-10-09 "guards stop me while I
    // run, but the whole dialogue window is grey and the art is missing" -- and no line in the trace
    // said a conversation had even happened.
    trace_event(TRACE_FLOW, "dialog-start", "pid=%d id=%d name=%s head=%d reaction=%d inCombat=%d light=%d display=%dx%d",
        dialog_target != NULL ? dialog_target->pid : -1, dialog_target != NULL ? dialog_target->id : -1,
        dialog_target != NULL ? object_name(dialog_target) : "-", headFid, reaction, isInCombat() ? 1 : 0,
        light_get_ambient(), display_win != -1 ? win_width(display_win) : -1, display_win != -1 ? win_height(display_win) : -1);

    boxesWereDisabled = disable_box_bar_win();
    oldFont = text_curr();
    text_font(101);
    dialogSetReplyWindow(135, 225, 379, 58, NULL);
    dialogSetReplyColor(0.3f, 0.3f, 0.3f);
    dialogSetOptionWindow(127, 335, 393, 117, NULL);
    dialogSetOptionColor(0.2f, 0.2f, 0.2f);
    dialogTitle(NULL);
    dialogRegisterWinDrawCallbacks(demo_copy_title, demo_copy_options);
    talk_to_blend_table_init();
    cycle_disable();
    if (gdDialogTurnMouseOff) {
        gmouse_disable(0);
    }
    gmouse_3d_off();
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
    text_object_reset();

    if (PID_TYPE(dialog_target->pid) != OBJ_TYPE_ITEM) {
        tile_scroll_to(dialog_target->tile, 2);
    }

    talk_need_to_center = 1;

    talk_to_create_head_window();
    add_bk_process(head_bk, "head");
    talk_to_set_up_fidget(headFid, reaction);
    gdialog_state = 1;
    gmouse_disable_scrolling();

    if (headFid == -1) {
        gDialogMusicVol = gsound_background_volume_get_set(gDialogMusicVol / 2);
    } else {
        gDialogMusicVol = -1;
        gsound_background_stop();
    }

    gdDialogWentOff = true;

    return 0;
}

// 0x43E32C
int scr_dialogue_exit()
{
    if (dialogue_switch_mode == 2) {
        return -1;
    }

    if (gdialog_state == 0) {
        return 0;
    }

    gdialog_free_speech();
    gdReviewFree();
    remove_bk_process(head_bk);

    if (PID_TYPE(dialog_target->pid) != OBJ_TYPE_ITEM) {
        if (gdPlayerTile != obj_dude->tile) {
            gdCenterTile = obj_dude->tile;
        }
        tile_scroll_to(gdCenterTile, 2);
    }

    talk_to_destroy_head_window();

    text_font(oldFont);

    if (fidgetFp != NULL) {
        art_ptr_unlock(fidgetKey);
        fidgetFp = NULL;
    }

    if (lipsKey != NULL) {
        if (art_ptr_unlock(lipsKey) == -1) {
            debug_printf("Failure unlocking lips frame!\n");
        }
        lipsKey = NULL;
        lipsFp = NULL;
        lipsFID = 0;
    }

    // NOTE: Uninline.
    talk_to_blend_table_exit();

    gdialog_state = 0;
    dialogue_state = 0;

    cycle_enable();

    gmouse_enable_scrolling();

    if (gDialogMusicVol == -1) {
        gsound_background_restart_last(11);
    } else {
        gsound_background_volume_set(gDialogMusicVol);
    }

    if (boxesWereDisabled) {
        enable_box_bar_win();
    }

    boxesWereDisabled = 0;

    if (gdDialogTurnMouseOff) {
        gmouse_enable();

        gdDialogTurnMouseOff = 0;
    }

    gmouse_3d_on();

    gdDialogWentOff = true;

    return 0;
}

// 0x43E4A4
void gdialog_set_background(int a1)
{
    if (a1 != -1) {
        backgroundIndex = a1;
    }
}

// 0x43E4B4
static int gdialog_hide()
{
    if (gd_replyWin != -1) {
        win_hide(gd_replyWin);
    }

    if (gd_optionsWin != -1) {
        win_hide(gd_optionsWin);
    }

    return 0;
}

// 0x43E4E0
static int gdialog_unhide()
{
    if (gd_replyWin != -1) {
        win_show(gd_replyWin);
    }

    if (gd_optionsWin != -1) {
        win_show(gd_optionsWin);
    }

    return 0;
}

// 0x43E50C
static int gdialog_unhide_reply()
{
    if (gd_replyWin != -1) {
        win_show(gd_replyWin);
    }

    return 0;
}

// Renders supplementary message in reply area of the dialog.
//
// 0x43E524
void gdialog_display_msg(char* msg)
{
    if (gd_replyWin == -1) {
        debug_printf("\nError: Reply window doesn't exist!");
    }

    replyRect.ulx = 5;
    replyRect.uly = 10;
    replyRect.lrx = 374;
    replyRect.lry = 58;

    // NOTE: Result is unused. Looks like the intention was to change color.
    perk_level(PERK_EMPATHY);

    demo_copy_title(gReplyWin);

    int a4 = 0;

    // NOTE: Uninline.
    text_to_rect_wrapped(win_get_buf(gReplyWin),
        &replyRect,
        msg,
        &a4,
        text_height(),
        379,
        colorTable[992] | 0x2000000);

    win_show(gd_replyWin);
    win_draw(gReplyWin);
}

// 0x43E5E4
int gDialogStart()
{
    curReviewSlot = 0;
    gdNumOptions = 0;
    return 0;
}

// 0x43E5F8
static int gdAddOption(int messageListId, int messageId, int reaction)
{
    if (gdNumOptions >= DIALOG_OPTION_ENTRIES_CAPACITY) {
        debug_printf("\nError: dialog: Ran out of options!");
        return -1;
    }

    GameDialogOptionEntry* optionEntry = &(dialogBlock.options[gdNumOptions]);
    optionEntry->messageListId = messageListId;
    optionEntry->messageId = messageId;
    optionEntry->reaction = reaction;
    optionEntry->btn = -1;
    optionEntry->text[0] = '\0';

    gdNumOptions++;

    return 0;
}

// 0x43E65C
static int gdAddOptionStr(int messageListId, const char* text, int reaction)
{
    if (gdNumOptions >= DIALOG_OPTION_ENTRIES_CAPACITY) {
        debug_printf("\nError: dialog: Ran out of options!");
        return -1;
    }

    GameDialogOptionEntry* optionEntry = &(dialogBlock.options[gdNumOptions]);
    optionEntry->messageListId = -4;
    optionEntry->messageId = -4;
    optionEntry->reaction = reaction;
    optionEntry->btn = -1;
    snprintf(optionEntry->text, sizeof(optionEntry->text), "%c %s", '\x95', text);

    gdNumOptions++;

    return 0;
}

// NOTE: If you look at the scripts handlers, my best guess that their intention
// was to allow scripters to specify proc names instead of proc addresses. They
// dropped this idea, probably because they've updated their own compiler, or
// maybe there was not enough time to complete it. Any way, [procedure] is the
// identifier of the procedure in the script, but it is silently ignored.
//
// 0x43E6D8
int gDialogOption(int messageListId, int messageId, const char* proc, int reaction)
{
    dialogBlock.options[gdNumOptions].proc = 0;

    return gdAddOption(messageListId, messageId, reaction);
}

// NOTE: If you look at the script handlers, my best guess that their intention
// was to allow scripters to specify proc names instead of proc addresses. They
// dropped this idea, probably because they've updated their own compiler, or
// maybe there was not enough time to complete it. Any way, [procedure] is the
// identifier of the procedure in the script, but it is silently ignored.
//
// 0x43E718
int gDialogOptionStr(int messageListId, const char* text, const char* proc, int reaction)
{
    dialogBlock.options[gdNumOptions].proc = 0;

    return gdAddOptionStr(messageListId, text, reaction);
}

// 0x43E758
int gDialogOptionProc(int messageListId, int messageId, int proc, int reaction)
{
    dialogBlock.options[gdNumOptions].proc = proc;

    return gdAddOption(messageListId, messageId, reaction);
}

// 0x43E79C
int gDialogOptionProcStr(int messageListId, const char* text, int proc, int reaction)
{
    dialogBlock.options[gdNumOptions].proc = proc;

    return gdAddOptionStr(messageListId, text, reaction);
}

// 0x43E7E0
int gDialogReply(Program* program, int messageListId, int messageId)
{
    gdAddReviewReply(messageListId, messageId);

    // What was said to the player, paired with dialog-choice for what he answered (see there).
    {
        const char* said = scr_get_msg_str(messageListId, messageId);
        trace_event(TRACE_FLOW, "dialog-reply", "msg=%d text=%.80s", messageId, said != NULL ? said : "-");
    }

    dialogBlock.program = program;
    dialogBlock.replyMessageListId = messageListId;
    dialogBlock.replyMessageId = messageId;
    dialogBlock.offset = 0;
    dialogBlock.replyText[0] = '\0';
    gdNumOptions = 0;

    return 0;
}

// 0x43E81C
int gDialogReplyStr(Program* program, int messageListId, const char* text)
{
    gdAddReviewReplyStr(text);

    dialogBlock.program = program;
    dialogBlock.offset = 0;
    dialogBlock.replyMessageListId = -4;
    dialogBlock.replyMessageId = -4;

    strcpy(dialogBlock.replyText, text);

    gdNumOptions = 0;

    return 0;
}

// --- FOLLOWER ORDERS BOLTED ONTO A VANILLA CONVERSATION -----------------------------------------
//
// One extra reply line on a party member's dialogue: "stop patching me up" / "you can patch me up
// again", toggling combat_rt's per-companion medic opt-out.
//
// ⚠️ NO SCRIPT IS TOUCHED, AND THAT IS THE POINT. Ian's dialogue lives in a COMPILED IAN.INT inside
// MASTER.DAT; this project has a decompiler for those and no compiler, so adding a real script node
// is not on the table. gDialogGo is the seam that makes it unnecessary: the script has finished
// adding its own options by the time it runs, and gdAddOptionStr already exists for free-text
// options (it is what the barter and "tell me about" paths use). The vanilla script never learns
// the line was there, so nothing it does can be broken by it.
//
// ⚠️ MARKED IN THE ENTRY ITSELF, NOT BY REMEMBERING AN INDEX. Dialogue is reentrant -- a script
// procedure can open a whole nested reply node (gdReenterLevel) -- so a static "our option is at
// index N" would describe the wrong option set the moment a nested level returned. `messageId` is
// dead storage for a free-text option: gdAddOptionStr writes -4 into it, and the only reader
// (gDialogProcessUpdate) consults it solely when messageListId >= 0. So the marker travels with
// the option, and nesting cannot confuse it.
#define GD_OPTION_MEDIC_TOGGLE (-1001)
#define GD_OPTION_STEP_BACK (-1002)

static bool gd_is_extra_option(const GameDialogOptionEntry* entry, int marker)
{
    return entry != NULL && entry->messageListId == -4 && entry->messageId == marker;
}

// Append one free-text option of ours and stamp it with its marker. Silently does nothing when the
// script has already filled the list -- its own options matter more than ours.
static void gd_add_extra_option(const char* text, int marker)
{
    if (gdAddOptionStr(-4, text, GAME_DIALOG_REACTION_NEUTRAL) == -1) {
        return;
    }

    GameDialogOptionEntry* entry = &(dialogBlock.options[gdNumOptions - 1]);
    entry->messageId = marker;
    entry->proc = 0; // ours to handle; there is no script procedure to run
}

static void gd_add_follower_options()
{
    // Followers only: this is an ORDER, and there is nobody else it would mean anything to.
    if (dialog_target == NULL || !isPartyMember(dialog_target) || dialog_target == obj_dude) {
        return;
    }

    // "Back off a few paces." Always offered -- unlike the medic line there is no feature switch
    // it depends on, and being in the way is not a combat-only problem.
    gd_add_extra_option("(Back off a few paces. You're in my way.)", GD_OPTION_STEP_BACK);

    // Nothing to turn off when the beat that does the healing is off game-wide.
    if (!combat_rt_medic_beat_enabled()) {
        return;
    }

    gd_add_extra_option(combat_rt_medic_treats_player(dialog_target)
            ? "(Stop patching me up. Save your supplies.)"
            : "(You can patch me up again.)",
        GD_OPTION_MEDIC_TOGGLE);
}

// Chosen. Flip it, then say so and leave a single way out -- gDialogProcessChoice ends the
// conversation when an option set comes back empty, so the acknowledgement needs one option of its
// own or the screen would simply vanish with no confirmation that anything happened.
static void gd_run_follower_medic_option()
{
    bool treats = combat_rt_medic_treats_player(dialog_target);
    combat_rt_medic_set_treats_player(dialog_target, !treats);

    char reply[256];
    snprintf(reply, sizeof(reply), treats
            ? "%s shrugs, and puts the kit away."
            : "%s nods, and checks what is left in the kit.",
        object_name(dialog_target));

    gDialogReplyStr(dialogBlock.program, -4, reply);
    gdAddOptionStr(-4, "(Good.)", GAME_DIALOG_REACTION_NEUTRAL);
    dialogBlock.options[gdNumOptions - 1].proc = 0;
}

// "Back off a few paces." The walk itself cannot happen here -- this runs inside the modal dialogue
// loop, where the world is not ticking -- so it is queued and combat_rt starts it on the first
// update after the conversation closes.
static void gd_run_follower_step_back_option()
{
    combat_rt_request_step_back(dialog_target);

    char reply[256];
    snprintf(reply, sizeof(reply), "%s steps back to give you room.", object_name(dialog_target));

    gDialogReplyStr(dialogBlock.program, -4, reply);
    gdAddOptionStr(-4, "(Thanks.)", GAME_DIALOG_REACTION_NEUTRAL);
    dialogBlock.options[gdNumOptions - 1].proc = 0;
}

// 0x43E878
int gDialogGo()
{
    if (dialogBlock.replyMessageListId == -1) {
        return 0;
    }

    int rc = 0;

    // Appended AFTER the script's own options and BEFORE the empty-list fallback below, so ours
    // never becomes the reason a node counts as having options.
    gd_add_follower_options();

    if (gdNumOptions < 1) {
        dialogBlock.options[gdNumOptions].proc = 0;

        if (gDialogOption(-1, -1, NULL, 50) == -1) {
            interpretError("Error setting option.");
            rc = -1;
        }
    }

    if (rc != -1) {
        rc = gDialogProcess();
    }

    gdNumOptions = 0;

    return rc;
}

// 0x43E904
static void gdReviewFree()
{
    for (int index = 0; index < curReviewSlot; index++) {
        GameDialogReviewEntry* entry = &(reviewList[index]);
        entry->replyMessageListId = 0;
        entry->replyMessageId = 0;

        if (entry->replyText != NULL) {
            mem_free(entry->replyText);
            entry->replyText = NULL;
        }

        entry->optionMessageListId = 0;
        entry->optionMessageId = 0;
    }
}

// 0x43E968
static int gdAddReviewReply(int messageListId, int messageId)
{
    if (curReviewSlot >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debug_printf("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(reviewList[curReviewSlot]);
    entry->replyMessageListId = messageListId;
    entry->replyMessageId = messageId;

    // NOTE: I'm not sure why there are two consequtive assignments.
    entry->optionMessageListId = -1;
    entry->optionMessageId = -1;

    entry->optionMessageListId = -3;
    entry->optionMessageId = -3;

    curReviewSlot++;

    return 0;
}

// 0x43E9DC
static int gdAddReviewReplyStr(const char* string)
{
    if (curReviewSlot >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debug_printf("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(reviewList[curReviewSlot]);
    entry->replyMessageListId = -4;
    entry->replyMessageId = -4;

    if (entry->replyText != NULL) {
        mem_free(entry->replyText);
        entry->replyText = NULL;
    }

    entry->replyText = (char*)mem_malloc(strlen(string) + 1);
    strcpy(entry->replyText, string);

    entry->optionMessageListId = -3;
    entry->optionMessageId = -3;
    entry->optionText = NULL;

    curReviewSlot++;

    return 0;
}

// 0x43EACC
static int gdAddReviewOptionChosen(int messageListId, int messageId)
{
    if (curReviewSlot >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debug_printf("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(reviewList[curReviewSlot - 1]);
    entry->optionMessageListId = messageListId;
    entry->optionMessageId = messageId;
    entry->optionText = NULL;

    return 0;
}

// 0x43EB18
static int gdAddReviewOptionChosenStr(const char* string)
{
    if (curReviewSlot >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debug_printf("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(reviewList[curReviewSlot - 1]);
    entry->optionMessageListId = -4;
    entry->optionMessageId = -4;

    entry->optionText = (char*)mem_malloc(strlen(string) + 1);
    strcpy(entry->optionText, string);

    return 0;
}

// ⚠️ A MESSAGE WITH NO CONVERSATION OPEN IS FLOATED OVER THE SPEAKER (2026-10-10). Some vanilla scripts
// say one line through gsay_message straight from talk_p_proc, without start_gdialog -- the Master's
// vent guards (AIRGRD.INT, proc airgrd15) shout "Intruders!" that way and then attack. With no
// start_gdialog, scr_dialogue_init never builds the dialogue frame, so the engine showed only the bare
// reply and option windows in the window manager's default grey over the map, and stopped the game
// until [Done]: *"šedý dialogue box intruder"*, screenshots 2026-10-10, sneaking past two of them.
// That is how the original's code draws it; a shortcut in one script branch, not a designed look.
// He chose to float such a line over the speaker like a combat taunt instead (MODS "Script lines as
// floating text", default ON at his request): no box, no pause, the script runs on into its attack.
//
// Only a SHORT line is floated: a longer one is something to read, not a shout, and keeps the
// vanilla box so it cannot scroll away unread.
#define GDIALOG_FLOAT_MAX_CHARS 80

static bool gdialog_float_say_message()
{
    if (!gdialog_script_float || gdialog_state == 1 || dialog_target == NULL) {
        return false;
    }
    if (FID_TYPE(dialog_target->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    const char* text = NULL;
    if (dialogBlock.replyMessageListId == -4) {
        text = dialogBlock.replyText;
    } else if (dialogBlock.replyMessageListId >= 0) {
        text = scr_get_msg_str(dialogBlock.replyMessageListId, dialogBlock.replyMessageId);
    }
    if (text == NULL || text[0] == '\0' || strlen(text) > GDIALOG_FLOAT_MAX_CHARS) {
        return false;
    }

    if (combatai_float_line(dialog_target, text) != 0) {
        return false;
    }
    trace_event(TRACE_FLOW, "script-float", "pid=%d id=%d name=%s text=%s",
        dialog_target->pid, dialog_target->id, object_name(dialog_target), text);
    return true;
}

// 0x43EBB0
int gDialogSayMessage()
{
    if (gdialog_float_say_message()) {
        gdNumOptions = 0;
        dialogBlock.replyMessageListId = -1;
        return 0;
    }

    mouse_show();
    gDialogGo();

    gdNumOptions = 0;
    dialogBlock.replyMessageListId = -1;

    return 0;
}

// 0x43EBD8
// CE: Gamepad shortcuts inside a conversation. The dialogue loop already has keyboard
// shortcuts B = barter and A = ask-about (talk_to_pressed_barter / _about), but the on-
// screen buttons are mouse-only (keyCode -1), so a controller player couldn't reach
// trade easily. Map Y -> B (barter) and X -> A (ask about) by injecting those keycodes.
// Read SDL directly with edge detection -- gamepad_ui_tick doesn't run in this modal
// loop. (Y/X are otherwise free during a conversation: gpui_button_shortcuts bails on
// dialog_active(), and the A-router only clicks the selected reply.)
static int gdialog_gamepad_keycode()
{
    static bool prevY = false;
    static bool prevX = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevY = false;
        prevX = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    bool yNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y) != 0;
    bool xNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) != 0;

    // ⚠️ FIRST FRAME OF A NEW CONVERSATION: adopt whatever is held instead of reading it as a
    // press. See gdialog_pad_rearm -- X is the run button, so being stopped mid-run by a script is
    // the normal way into a dialogue with X already down.
    if (gdialog_pad_rearm) {
        gdialog_pad_rearm = false;
        prevY = yNow;
        prevX = xNow;
        return -1;
    }

    int keyCode = -1;
    if (yNow && !prevY) {
        keyCode = KEY_LOWERCASE_B; // barter / trade
    } else if (xNow && !prevX) {
        keyCode = KEY_LOWERCASE_A; // ask me about
    }

    prevY = yNow;
    prevX = xNow;
    return keyCode;
}

// CE: Universal gamepad cancel for the dialogue-family sub-screens (barter has its own;
// this covers the "ask about" and review windows, both of which close on KEY_ESCAPE).
// B or BACK -> ESCAPE. Separate edge state from gdialog_gamepad_keycode so a Y/X press
// that opened a sub-screen can't also register as a cancel there.
static int gdialog_gamepad_cancel_key()
{
    static bool prevB = false;
    static bool prevBack = false;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevB = false;
        prevBack = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    bool bNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) != 0;
    bool backNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) != 0;

    // Same first-frame adoption as gdialog_gamepad_keycode, for the same reason: a B held on the
    // way into a conversation would otherwise close the sub-screen it just opened.
    if (gdialog_pad_rearm_cancel) {
        gdialog_pad_rearm_cancel = false;
        prevB = bNow;
        prevBack = backNow;
        return -1;
    }

    int keyCode = -1;
    if ((bNow && !prevB) || (backNow && !prevBack)) {
        keyCode = KEY_ESCAPE;
    }

    prevB = bNow;
    prevBack = backNow;
    return keyCode;
}

// CE: console-style reply selection with a controller -- the highlighted option (tracked by
// the left-stick snap, gpui_menu_snap, whose cursor motion fires each option button's hover ->
// gDialogProcessHighlight) already reads like the main menu; the only jarring part is the visible
// pointer. So blank it (MOUSE_CURSOR_NONE, NOT mouse_hide() -- that would gate mouse_info off and
// stop the right stick from ever revealing it), and bring it back only while the mouse/right stick
// is actively moving it, fading out again after GD_CURSOR_IDLE_MS. Same pattern as load/save's
// ls_tick_cursor. Selection is unaffected: the snap still moves the (invisible) cursor and A still
// clicks the hovered option.
#define GD_CURSOR_IDLE_MS 2000

static void gdialog_tick_cursor(bool* shown, int* lastX, int* lastY, unsigned int* lastMove)
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
    } else if (*shown && elapsed_time(*lastMove) >= GD_CURSOR_IDLE_MS) {
        gmouse_set_cursor(MOUSE_CURSOR_NONE);
        *shown = false;
    }

    *lastX = x;
    *lastY = y;
}

// CE: the pad's reply selection is ENGAGED -- self_nav on, pointer blanked -- by SOME level of the
// conversation. File scope rather than per gDialogProcess level, because the levels nest and engage
// lazily (see the note at the top of gDialogProcess), and only the outermost exit may hand the pad
// back; it has to know whether anybody took it.
static bool gdPadNavEngaged = false;

// Minimum gap between two accepted reply selections. Guards against one press being read as two
// answers across consecutive reply sets -- a mistake with no undo, since the conversation has
// already advanced. Well under deliberate reply pacing.
#define GDIALOG_REPLY_LOCKOUT_MS 250

// CE: index-based reply navigation for a controller -- the "like the main menu" selection.
// D-pad / left-stick up/down moves the highlighted option (gDialogProcessHighlight/UnHighlight),
// with NO pointer involved: the caller engages gamepad_ui_set_self_nav, which suppresses the
// shared left-stick cursor snap and A-as-click so they don't fight this. A confirms the current
// option by returning its select keycode (49 + index -- exactly what clicking the option button
// yields, decoded as option = keyCode - 49 in gDialogProcess). Edge + auto-repeat; SDL read
// directly since gamepad_ui_tick doesn't run in this modal loop. `*sel` is owned by the caller
// (reset to 0 per option set so the first reply starts highlighted).
static int gdialog_gamepad_reply_nav(int* sel)
{
    static bool prevUp = false;
    static bool prevDown = false;
    static bool prevA = false;
    static bool aArmed = false;
    static bool navArmed = false;
    static unsigned int lastStep = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL || gdNumOptions <= 0) {
        prevUp = false;
        prevDown = false;
        prevA = false;
        aArmed = false;
        navArmed = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    Sint16 ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    bool upNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0 || ly < -16000;
    bool downNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0 || ly > 16000;
    bool aNow = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0;

    // A nested modal loop (the trade window, a dialog box) ran in between and these edges describe
    // the pad from before it. Take the pad as it is NOW: whatever is still held is spent, and must
    // be seen released before it can act -- the same neutral-first rule as the arming below.
    static unsigned int seenEpoch = 0;
    if (seenEpoch != gamepad_ui_input_epoch()) {
        seenEpoch = gamepad_ui_input_epoch();
        prevUp = upNow;
        prevDown = downNow;
        prevA = aNow;
        aArmed = !aNow;
        navArmed = !upNow && !downNow;
    }

    // Neutral-first latch, same idea as the world map's wm_stick_armed. This loop is entered with
    // whatever the player was physically holding when the conversation opened -- A from the interact
    // press (or a swing), the stick from running up to the NPC -- and the prev* edge state starts
    // false, so a HELD input reads as a fresh press: A instantly confirmed reply 0 and a held stick
    // scrolled the highlight on entry. Whether it fired depended purely on what was down in that one
    // frame, which is what made it intermittent. Require each control to be seen released once
    // before it may act; re-armed per option set via the gdNumOptions reset above, so a held A can
    // still never chain through two replies.
    if (!aNow) {
        aArmed = true;
    }

    if (!upNow && !downNow) {
        navArmed = true;
    }

    unsigned int now = get_time();

    // Step on the press edge immediately, then auto-repeat while held.
    int dir = 0;
    bool repeat = elapsed_time(lastStep) >= 130;
    if (!navArmed) {
        dir = 0;
    } else if ((upNow && !prevUp) || (upNow && repeat)) {
        dir = -1;
    } else if ((downNow && !prevDown) || (downNow && repeat)) {
        dir = 1;
    }

    if (dir != 0) {
        int newSel = *sel + dir;
        if (newSel < 0) {
            newSel = gdNumOptions - 1; // wrap
        } else if (newSel >= gdNumOptions) {
            newSel = 0;
        }
        if (newSel != *sel) {
            gDialogProcessUnHighlight(*sel);
            gDialogProcessHighlight(newSel);
            *sel = newSel;
        }
        lastStep = now;
    }

    // ⚠️ HARD LOCKOUT AFTER A SELECTION, on top of the arming above. The arming only guarantees A
    // was released between two presses -- it cannot stop the NEXT reply set from being answered by
    // a press that arrives milliseconds later, whether that is a fast double-tap, controller switch
    // bounce, or any other path that injects a select. The player saw one press take two replies
    // ("zmáčknul jsem áčko a vybrala se i další odpověď"), which is unrecoverable: the conversation
    // has already moved on and there is no undo. A quarter second is far below deliberate reply
    // pacing and far above any of those.
    static unsigned int lastSelect = 0;

    int keyCode = -1;
    if (aArmed && aNow && !prevA && *sel >= 0 && *sel < gdNumOptions
        && elapsed_time(lastSelect) >= GDIALOG_REPLY_LOCKOUT_MS) {
        keyCode = 49 + *sel; // select this option (gDialogProcess: option = keyCode - 49)
        lastSelect = get_time();
        // Force a fresh release before anything can be picked again, so the very press that
        // answered this node cannot also answer the next one.
        aArmed = false;
    }

    prevUp = upNow;
    prevDown = downNow;
    prevA = aNow;
    return keyCode;
}

// CE: RIGHT stick scrolls the NPC's reply when it does not fit in the box (gdReplyTooBig). The
// engine already pages that text with the arrow keys, so this only has to synthesise them -- which
// keeps one implementation of the paging and its page-offset bookkeeping.
//
// ⚠️ THE RIGHT STICK, NOT THE LEFT. The left stick and the D-pad move the reply SELECTION
// (gdialog_gamepad_reply_nav), and the two must not fight over one axis: scrolling the NPC's text
// and choosing your answer are different jobs and the player does them in the same moment.
//
// Edge-triggered with hold-to-repeat, same shape as the reply nav, plus a neutral-first arm so a
// stick held from before the conversation opened cannot page on the first frame.
static int gdialog_gamepad_reply_scroll()
{
    static bool prevUp = false;
    static bool prevDown = false;
    static bool armed = false;
    static unsigned int lastStep = 0;

    SDL_GameController* pad = gamepad_ui_get_controller();
    if (pad == NULL) {
        prevUp = false;
        prevDown = false;
        armed = false;
        return -1;
    }

    SDL_GameControllerUpdate();
    Sint16 ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    bool upNow = ry < -16000;
    bool downNow = ry > 16000;

    if (!upNow && !downNow) {
        armed = true;
    }

    int keyCode = -1;
    if (armed) {
        bool repeat = elapsed_time(lastStep) >= 200;
        if ((upNow && !prevUp) || (upNow && repeat)) {
            keyCode = KEY_ARROW_UP;
            lastStep = get_time();
        } else if ((downNow && !prevDown) || (downNow && repeat)) {
            keyCode = KEY_ARROW_DOWN;
            lastStep = get_time();
        }
    }

    prevUp = upNow;
    prevDown = downNow;
    return keyCode;
}

static int gDialogProcess()
{
    if (gdReenterLevel == 0) {
        if (gDialogProcessInit() == -1) {
            return -1;
        }
    }

    gdReenterLevel += 1;

    gDialogProcessUpdate();

    int v18 = 0;
    if (dialogBlock.offset != 0) {
        v18 = 1;
        gdReplyTooBig = 1;
    }

    unsigned int tick = get_time();
    int pageCount = 0;
    int pageIndex = 0;
    int pageOffsets[10];
    pageOffsets[0] = 0;

    // Controller reply selection: drive it like the main menu -- the pointer is hidden and a
    // highlighted option is moved by the D-pad/stick (gdialog_gamepad_reply_nav). self_nav tells
    // the shared mouse hook to stop snapping the cursor / clicking on A so it doesn't fight that.
    // Barter keeps its pointer (it means pointing at items to trade).
    //
    // ⚠️ ENGAGED AT THE FIRST FRAME THAT IS NOT A TRADE, NOT ONCE AT ENTRY. A merchant's "let's
    // trade" node calls gdialog_mod_barter BEFORE its gsay_end, so the reply level that node opens
    // is entered already bartering. Deciding at entry left that whole level without the pad: after
    // the trade its "[DONE]" sat there with nothing lit, A did nothing (self_nav from the level
    // below still swallowed the click), and only RT on a pointer that happened to rest on it got
    // you out ("když dokončím obchod, tak áčko nejde, musím zmáčknout RT").
    bool gdCursorController = false;
    bool gdCursorShown = true;
    int gdCursorX = -1;
    int gdCursorY = -1;
    unsigned int gdCursorMove = get_time();
    int gdReplySel = 0;
    // Deliberately NOT gdOptionGen: the first pass through the loop must see a mismatch and seed
    // the highlight, exactly as entering a nested level must re-seed for its own option set.
    unsigned int gdSeenOptionGen = gdOptionGen - 1;

    for (;;) {
        sharedFpsLimiter.mark();

        // Before get_input, so the hook already runs under self_nav on the very frame this engages.
        if (!gdCursorController && gamepad_ui_get_controller() != NULL && !dialog_is_bartering()) {
            gdCursorController = true;
            gdCursorShown = false;
            mouse_get_position(&gdCursorX, &gdCursorY);
            gdCursorMove = get_time();
            gdSeenOptionGen = gdOptionGen - 1; // light this level's list the moment it is ours
            gmouse_set_cursor(MOUSE_CURSOR_NONE);
            gamepad_ui_set_self_nav(true);
            gdPadNavEngaged = true;
        }

        int keyCode = get_input();

        if (gdCursorController) {
            // A rebuilt reply list arrives with nothing highlighted and gdReplySel still on the
            // option we just answered -- invisible AND wrong, since A would then confirm whatever
            // sits at that stale index. Re-seed on option 0, the same state the conversation opens
            // in. Runs before the nav read so the highlight is already up when the player pushes.
            if (gdSeenOptionGen != gdOptionGen) {
                gdSeenOptionGen = gdOptionGen;
                gdReplySel = 0;
                if (gdNumOptions > 0) {
                    gDialogProcessHighlight(0);
                }
            }

            gdialog_tick_cursor(&gdCursorShown, &gdCursorX, &gdCursorY, &gdCursorMove);
            int navKey = gdialog_gamepad_reply_nav(&gdReplySel);
            if (keyCode == -1 && navKey != -1) {
                keyCode = navKey;
            }

            // Right stick -> page the reply text. After the nav read, so a genuine keyboard key or a
            // reply selection always wins the frame; scrolling is the one that can wait a frame.
            if (keyCode == -1) {
                keyCode = gdialog_gamepad_reply_scroll();
            }
        }

        // CE: fold in the gamepad barter/ask shortcuts (Y/X) when no real key fired.
        if (keyCode == -1) {
            int padKey = gdialog_gamepad_keycode();
            if (padKey != -1) {
                keyCode = padKey;
            }
        }

        convertMouseWheelToArrowKey(&keyCode);

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            game_quit_with_confirm();
        }

        if (game_user_wants_to_quit != 0) {
            break;
        }

        if (keyCode == KEY_CTRL_B && !mouse_click_in(135, 225, 514, 283)) {
            if (gmouse_get_cursor() != MOUSE_CURSOR_ARROW) {
                gmouse_set_cursor(MOUSE_CURSOR_ARROW);
            }
        } else {
            if (dialogue_switch_mode == 3) {
                dialogue_state = 4;
                barter_inventory(dialogueWindow, dialog_target, peon_table_obj, barterer_table_obj, gdBarterMod);
                dialogue_barter_cleanup_tables();

                // The trade ran its own input loop for as long as it lasted, so the reply nav's edge
                // state is from BEFORE it -- typically "A down", from the press that picked "let's
                // trade" -- and the first A back in the conversation was read as still held and
                // swallowed. exit_inventory already bumps the input epoch on the way out; the reply
                // nav now re-primes on it (gdialog_gamepad_reply_nav).
                //
                // And light the list again: the dialogue window is rebuilt around the trade, which
                // repaints the options plain while gdReplySel still names one.
                gdSeenOptionGen = gdOptionGen - 1;

                int v5 = dialogue_state;
                talk_to_destroy_barter_win();
                dialogue_state = v5;

                if (v5 == 4) {
                    dialogue_switch_mode = 1;
                    dialogue_state = 1;
                }
                continue;
            }

            if (dialogue_switch_mode == 6) {
                about_loop();
            } else if (keyCode == KEY_LOWERCASE_B) {
                talk_to_pressed_barter(-1, -1);
            } else if (keyCode == KEY_LOWERCASE_A) {
                talk_to_pressed_about(-1, -1);
            }
        }

        if (gdReplyTooBig) {
            unsigned int v6 = get_bk_time();
            if (v18) {
                if (elapsed_tocks(v6, tick) >= 10000 || keyCode == KEY_SPACE) {
                    pageCount++;
                    pageIndex++;
                    pageOffsets[pageCount] = dialogBlock.offset;
                    gDialogProcessReply();
                    tick = v6;
                    if (!dialogBlock.offset) {
                        v18 = 0;
                    }
                }
            }

            if (keyCode == KEY_ARROW_UP) {
                if (pageIndex > 0) {
                    pageIndex--;
                    dialogBlock.offset = pageOffsets[pageIndex];
                    v18 = 0;
                    gDialogProcessReply();
                }
            } else if (keyCode == KEY_ARROW_DOWN) {
                if (pageIndex < pageCount) {
                    pageIndex++;
                    dialogBlock.offset = pageOffsets[pageIndex];
                    v18 = 0;
                    gDialogProcessReply();
                } else {
                    if (dialogBlock.offset != 0) {
                        tick = v6;
                        pageIndex++;
                        pageCount++;
                        pageOffsets[pageCount] = dialogBlock.offset;
                        v18 = 0;
                        gDialogProcessReply();
                    }
                }
            }
        }

        if (keyCode != -1) {
            // ⚠️ WITH THE PAD DRIVING THERE IS ONE HIGHLIGHT, AND IT IS THE SELECTION.
            //
            // The option buttons still raise their mouse hover events (1200+i in, 1300+i out) under
            // the BLANKED pointer -- and a fresh reply list that appears under a pointer resting
            // where it was last left gets hovered without anything moving. That lit the option
            // under the invisible pointer on top of the pad's own, two yellow lines at once ("neviditelný
            // kurzor myši občas highlituje text a z toho má dva texty žluté"), and A then answered
            // the one that was not the pointer's.
            //
            // So under the pad: a hover counts only while the pointer is really in use (shown --
            // the mouse or a trackpad just moved it), and then it TAKES the selection instead of
            // adding a second one. Leaving an option never unlights the selection: the pointer
            // wandering off is not the player moving off it.
            if (keyCode >= 1200 && keyCode <= 1250) {
                int hovered = keyCode - 1200;
                if (!gdCursorController) {
                    gDialogProcessHighlight(hovered);
                } else if (gdCursorShown && hovered < gdNumOptions && hovered != gdReplySel) {
                    if (gdReplySel >= 0 && gdReplySel < gdNumOptions) {
                        gDialogProcessUnHighlight(gdReplySel);
                    }
                    gDialogProcessHighlight(hovered);
                    gdReplySel = hovered;
                }
            } else if (keyCode >= 1300 && keyCode <= 1330) {
                int left = keyCode - 1300;
                if (!gdCursorController || (left != gdReplySel && left < gdNumOptions)) {
                    gDialogProcessUnHighlight(left);
                }
            } else if (keyCode >= 48 && keyCode <= 57) {
                int option = keyCode - 49;
                if (option < gdNumOptions) {
                    pageCount = 0;
                    pageIndex = 0;
                    pageOffsets[0] = 0;
                    gdReplyTooBig = 0;

                    if (gDialogProcessChoice(option) == -1) {
                        break;
                    }

                    tick = get_time();

                    if (dialogBlock.offset) {
                        v18 = 1;
                        gdReplyTooBig = 1;
                    } else {
                        v18 = 0;
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    // Release the controller reply-nav state, but only at the OUTERMOST dialogue level
    // (gdReenterLevel is still 1 here, decremented just below): a nested reply set must
    // leave self_nav / the hidden pointer engaged for the level that entered it, or the
    // outer loop would resume with the cursor snapping and A clicking again. Leaving the
    // pointer visible afterwards matters -- a blanked cursor must never outlive the dialogue.
    //
    // ⚠️ KEYED ON THE FILE-SCOPE gdPadNavEngaged, NOT THIS LEVEL'S OWN FLAG. Engaging is lazy now, so
    // a nested level can engage while the outermost never did (the outer one was entered mid-trade
    // and left straight after the nested one returned); testing the local flag here would then leak
    // self_nav out of the conversation -- and a leaked self_nav is a dead pad in the whole world.
    if (gdPadNavEngaged && gdReenterLevel == 1) {
        gamepad_ui_set_self_nav(false);
        gmouse_set_cursor(MOUSE_CURSOR_ARROW);
        gdPadNavEngaged = false;
    }

    gdReenterLevel -= 1;

    if (gdReenterLevel == 0) {
        if (gDialogProcessExit() == -1) {
            return -1;
        }
    }

    return 0;
}

// 0x43EEEC
static void gDialogProcessCleanup()
{
    for (int index = 0; index < gdNumOptions; index++) {
        GameDialogOptionEntry* optionEntry = &(dialogBlock.options[index]);

        if (optionEntry->btn != -1) {
            win_delete_button(optionEntry->btn);
            optionEntry->btn = -1;
        }
    }
}

// 0x43EF30
static int gDialogProcessChoice(int a1)
{
    // FIXME: There is a buffer underread bug when `a1` is -1 (pressing 0 on the
    // keyboard, see `gDialogProcess`). When it happens the game looks into unused
    // continuation of `dialogBlock.replyText` (within 0x58F868-0x58FF70 range) which
    // is initialized to 0 according to C spec. I was not able to replicate the
    // same behaviour by extending dialogBlock.replyText to 2700 bytes or introduce
    // new 1800 bytes buffer in between, at least not in debug builds. In order
    // to preserve original behaviour this dummy dialog option entry is used.
    //
    // TODO: Recheck behaviour after introducing |GameDialogBlock|.
    GameDialogOptionEntry dummy;
    memset(&dummy, 0, sizeof(dummy));

    mouse_hide();
    gDialogProcessCleanup();

    GameDialogOptionEntry* dialogOptionEntry = a1 != -1 ? &(dialogBlock.options[a1]) : &dummy;
    if (dialogOptionEntry->messageListId == -4) {
        gdAddReviewOptionChosenStr(dialogOptionEntry->text);
    } else {
        gdAddReviewOptionChosen(dialogOptionEntry->messageListId, dialogOptionEntry->messageId);
    }

    // WHICH ANSWER WAS GIVEN (2026-10-13). "Killian turned on me after I saved him" could be a bug or
    // simply a rude line picked in his night-trespass talk, and the trace could not tell the two
    // apart: it named the conversation and nothing that was said in it. The line as the player read it.
    {
        const char* chosen = dialogOptionEntry->messageListId == -4
            ? dialogOptionEntry->text
            : (dialogOptionEntry->messageListId >= 0
                    ? scr_get_msg_str(dialogOptionEntry->messageListId, dialogOptionEntry->messageId)
                    : NULL);
        trace_event(TRACE_INPUT, "dialog-choice", "idx=%d msg=%d text=%.80s", a1,
            dialogOptionEntry->messageId, chosen != NULL ? chosen : "-");
    }

    can_start_new_fidget = false;

    gdialog_free_speech();

    int v1 = GAME_DIALOG_REACTION_NEUTRAL;
    switch (dialogOptionEntry->reaction) {
    case GAME_DIALOG_REACTION_GOOD:
        v1 = -1;
        break;
    case GAME_DIALOG_REACTION_NEUTRAL:
        v1 = 0;
        break;
    case GAME_DIALOG_REACTION_BAD:
        v1 = 1;
        break;
    default:
        // See 0x446907 in ecx but this branch should be unreachable. Due to the
        // bug described above, this code is reachable.
        v1 = GAME_DIALOG_REACTION_NEUTRAL;
        debug_printf("\nError: dialog: Empathy Perk: invalid reaction!");
        break;
    }

    demo_copy_title(gReplyWin);
    demo_copy_options(gOptionWin);
    win_draw(gReplyWin);
    win_draw(gOptionWin);

    gDialogProcessHighlight(a1);
    talk_to_critter_reacts(v1);

    bool extraMedic = gd_is_extra_option(dialogOptionEntry, GD_OPTION_MEDIC_TOGGLE);
    bool extraStepBack = gd_is_extra_option(dialogOptionEntry, GD_OPTION_STEP_BACK);

    gdNumOptions = 0;

    // Our own lines: no script procedure exists for either. They run after the gdNumOptions reset
    // above because each builds a fresh reply and option of its own, and that count is what tells
    // the caller the conversation is still alive.
    if (extraMedic) {
        gd_run_follower_medic_option();
    } else if (extraStepBack) {
        gd_run_follower_step_back_option();
    } else if (gdReenterLevel < 2) {
        if (dialogOptionEntry->proc != 0) {
            executeProcedure(dialogBlock.program, dialogOptionEntry->proc);
        }
    }

    mouse_show();

    if (gdNumOptions == 0) {
        return -1;
    }

    gDialogProcessUpdate();

    return 0;
}

// Creates dialog interface.
//
// 0x43F070
static int gDialogProcessInit()
{
    int upBtn;
    int downBtn;
    int replyWindowX;
    int replyWindowY;
    int optionsWindowX;
    int optionsWindowY;

    talkOldFont = text_curr();
    text_font(101);

    replyWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2 + GAME_DIALOG_REPLY_WINDOW_X;
    replyWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_REPLY_WINDOW_Y;
    gReplyWin = win_add(replyWindowX,
        replyWindowY,
        GAME_DIALOG_REPLY_WINDOW_WIDTH,
        GAME_DIALOG_REPLY_WINDOW_HEIGHT,
        256,
        WINDOW_MOVE_ON_TOP);
    if (gReplyWin == -1) {
        return -1;
    }

    // Top part of the reply window - scroll up.
    upBtn = win_register_button(gReplyWin,
        1,
        1,
        377,
        28,
        -1,
        -1,
        KEY_ARROW_UP,
        -1,
        NULL,
        NULL,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (upBtn != -1) {
        win_register_button_sound_func(upBtn, gsound_red_butt_press, gsound_red_butt_release);
        win_register_button_func(upBtn, reply_arrow_up, reply_arrow_restore, 0, 0);
    }

    // Bottom part of the reply window - scroll down.
    downBtn = win_register_button(gReplyWin,
        1,
        29,
        377,
        28,
        -1,
        -1,
        KEY_ARROW_DOWN,
        -1,
        NULL,
        NULL,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (downBtn != -1) {
        win_register_button_sound_func(downBtn, gsound_red_butt_press, gsound_red_butt_release);
        win_register_button_func(downBtn, reply_arrow_down, reply_arrow_restore, 0, 0);
    }

    optionsWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2 + GAME_DIALOG_OPTIONS_WINDOW_X;
    optionsWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_OPTIONS_WINDOW_Y;
    gOptionWin = win_add(optionsWindowX,
        optionsWindowY,
        GAME_DIALOG_OPTIONS_WINDOW_WIDTH,
        GAME_DIALOG_OPTIONS_WINDOW_HEIGHT,
        256,
        WINDOW_MOVE_ON_TOP);
    if (gOptionWin == -1) {
        return -1;
    }

    return 0;
}

// 0x43F194
static void reply_arrow_up(int btn, int keyCode)
{
    if (gdReplyTooBig) {
        gmouse_set_cursor(MOUSE_CURSOR_SMALL_ARROW_UP);
    }
}

// 0x43F1A8
static void reply_arrow_down(int btn, int keyCode)
{
    if (gdReplyTooBig) {
        gmouse_set_cursor(MOUSE_CURSOR_SMALL_ARROW_DOWN);
    }
}

// 0x43F1BC
static void reply_arrow_restore(int btn, int keyCode)
{
    gmouse_set_cursor(MOUSE_CURSOR_ARROW);
}

// 0x43F1C8
static void gDialogProcessHighlight(int index)
{
    // FIXME: See explanation in `gDialogProcessChoice`.
    GameDialogOptionEntry dummy;
    memset(&dummy, 0, sizeof(dummy));

    GameDialogOptionEntry* dialogOptionEntry = index != -1 ? &(dialogBlock.options[index]) : &dummy;
    if (dialogOptionEntry->btn == 0) {
        return;
    }

    optionRect.ulx = 0;
    optionRect.uly = dialogOptionEntry->field_14;
    optionRect.lrx = 391;
    if (index < gdNumOptions - 1) {
        optionRect.lry = dialogBlock.options[index + 1].field_14 - 1;
    } else {
        optionRect.lry = 111;
    }
    gDialogRefreshOptionsRect(gOptionWin, &optionRect);

    optionRect.ulx = 5;
    optionRect.lrx = 388;

    int color = colorTable[32747] | 0x2000000;
    if (perk_level(PERK_EMPATHY)) {
        color = colorTable[32747] | 0x2000000;
        switch (dialogOptionEntry->reaction) {
        case GAME_DIALOG_REACTION_GOOD:
            color = colorTable[31775] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_NEUTRAL:
            break;
        case GAME_DIALOG_REACTION_BAD:
            color = colorTable[32074] | 0x2000000;
            break;
        default:
            debug_printf("\nError: dialog: Empathy Perk: invalid reaction!");
            break;
        }
    }

    // NOTE: Uninline.
    text_to_rect_wrapped(win_get_buf(gOptionWin),
        &optionRect,
        dialogOptionEntry->text,
        NULL,
        text_height(),
        393,
        color);

    optionRect.ulx = 0;
    optionRect.lrx = 391;
    optionRect.uly = dialogOptionEntry->field_14;
    win_draw_rect(gOptionWin, &optionRect);
}

// 0x43F328
static void gDialogProcessUnHighlight(int index)
{
    GameDialogOptionEntry* dialogOptionEntry = &(dialogBlock.options[index]);

    optionRect.ulx = 0;
    optionRect.uly = dialogOptionEntry->field_14;
    optionRect.lrx = 391;
    if (index < gdNumOptions - 1) {
        optionRect.lry = dialogBlock.options[index + 1].field_14 - 1;
    } else {
        optionRect.lry = 111;
    }
    gDialogRefreshOptionsRect(gOptionWin, &optionRect);

    int color = colorTable[992] | 0x2000000;
    if (perk_level(PERK_EMPATHY) != 0) {
        color = colorTable[32747] | 0x2000000;
        switch (dialogOptionEntry->reaction) {
        case GAME_DIALOG_REACTION_GOOD:
            color = colorTable[31] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_NEUTRAL:
            color = colorTable[992] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_BAD:
            color = colorTable[31744] | 0x2000000;
            break;
        default:
            debug_printf("\nError: dialog: Empathy Perk: invalid reaction!");
            break;
        }
    }

    optionRect.ulx = 5;
    optionRect.lrx = 388;

    // NOTE: Uninline.
    text_to_rect_wrapped(win_get_buf(gOptionWin),
        &optionRect,
        dialogOptionEntry->text,
        NULL,
        text_height(),
        393,
        color);

    optionRect.lrx = 391;
    optionRect.uly = dialogOptionEntry->field_14;
    optionRect.ulx = 0;
    win_draw_rect(gOptionWin, &optionRect);
}

// 0x43F488
static void gDialogProcessReply()
{
    replyRect.ulx = 5;
    replyRect.uly = 10;
    replyRect.lrx = 374;
    replyRect.lry = 58;

    // NOTE: There is an unused if condition.
    perk_level(PERK_EMPATHY);

    demo_copy_title(gReplyWin);

    // NOTE: Uninline.
    text_to_rect_wrapped(win_get_buf(gReplyWin),
        &replyRect,
        dialogBlock.replyText,
        &(dialogBlock.offset),
        text_height(),
        379,
        colorTable[992] | 0x2000000);
    win_draw(gReplyWin);
}

// 0x43F51C
static void gDialogProcessUpdate()
{
    // CE: a fresh reply list starts here. Bumped at the TOP, not the bottom -- the option loop
    // below has error returns, and a half-built list still needs re-seeding.
    gdOptionGen++;

    replyRect.ulx = 5;
    replyRect.uly = 10;
    replyRect.lrx = 374;
    replyRect.lry = 58;

    optionRect.ulx = 5;
    optionRect.uly = 5;
    optionRect.lrx = 388;
    optionRect.lry = 112;

    demo_copy_title(gReplyWin);
    demo_copy_options(gOptionWin);

    if (dialogBlock.replyMessageListId > 0) {
        char* s = scr_get_msg_str_speech(dialogBlock.replyMessageListId, dialogBlock.replyMessageId, 1);
        strncpy(dialogBlock.replyText, s, sizeof(dialogBlock.replyText) - 1);
        *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
    }

    gDialogProcessReply();

    int color = colorTable[992] | 0x2000000;

    bool hasEmpathy = perk_level(PERK_EMPATHY) != 0;

    int width = optionRect.lrx - optionRect.ulx - 4;

    MessageListItem messageListItem;

    int v21 = 0;

    for (int index = 0; index < gdNumOptions; index++) {
        GameDialogOptionEntry* dialogOptionEntry = &(dialogBlock.options[index]);

        if (hasEmpathy) {
            switch (dialogOptionEntry->reaction) {
            case GAME_DIALOG_REACTION_GOOD:
                color = colorTable[31] | 0x2000000;
                break;
            case GAME_DIALOG_REACTION_NEUTRAL:
                color = colorTable[992] | 0x2000000;
                break;
            case GAME_DIALOG_REACTION_BAD:
                color = colorTable[31744] | 0x2000000;
                break;
            default:
                debug_printf("\nError: dialog: Empathy Perk: invalid reaction!");
                break;
            }
        }

        if (dialogOptionEntry->messageListId >= 0) {
            char* text = scr_get_msg_str_speech(dialogOptionEntry->messageListId, dialogOptionEntry->messageId, 0);
            snprintf(dialogOptionEntry->text, sizeof(dialogOptionEntry->text), "%c %s", '\x95', text);
        } else if (dialogOptionEntry->messageListId == -1) {
            if (index == 0) {
                // Go on
                messageListItem.num = 655;
                if (stat_level(obj_dude, STAT_INTELLIGENCE) < 4) {
                    if (message_search(&proto_main_msg_file, &messageListItem)) {
                        strcpy(dialogOptionEntry->text, messageListItem.text);
                    } else {
                        debug_printf("\nError...can't find message!");
                        return;
                    }
                }
            } else {
                // TODO: Why only space?
                strcpy(dialogOptionEntry->text, " ");
            }
        } else if (dialogOptionEntry->messageListId == -2) {
            // [Done]
            messageListItem.num = 650;
            if (message_search(&proto_main_msg_file, &messageListItem)) {
                snprintf(dialogOptionEntry->text, sizeof(dialogOptionEntry->text), "%c %s", '\x95', messageListItem.text);
            } else {
                debug_printf("\nError...can't find message!");
                return;
            }
        }

        if (optionRect.uly < optionRect.lry) {
            int y = optionRect.uly;

            dialogOptionEntry->field_14 = y;

            if (index == 0) {
                y = 0;
            }

            // NOTE: Uninline.
            text_to_rect_wrapped(win_get_buf(gOptionWin),
                &optionRect,
                dialogOptionEntry->text,
                NULL,
                text_height(),
                393,
                color);

            optionRect.uly += 2;

            int max_y;
            if (index < gdNumOptions - 1) {
                max_y = optionRect.uly;
            } else {
                max_y = optionRect.lry - 1;
            }

            dialogOptionEntry->btn = win_register_button(gOptionWin,
                2,
                y,
                width,
                max_y - y - 4,
                1200 + index,
                1300 + index,
                -1,
                49 + index,
                NULL,
                NULL,
                NULL,
                0);
            if (dialogOptionEntry->btn != -1) {
                win_register_button_sound_func(dialogOptionEntry->btn, gsound_red_butt_press, gsound_red_butt_release);
            } else {
                debug_printf("\nError: Can't create button!");
            }
        }
    }

    win_draw(gReplyWin);
    win_draw(gOptionWin);
}

// 0x43F8D4
static int gDialogProcessExit()
{
    gDialogProcessCleanup();

    win_delete(gReplyWin);
    gReplyWin = -1;

    win_delete(gOptionWin);
    gOptionWin = -1;

    text_font(talkOldFont);

    return 0;
}

// 0x43F910
static void demo_copy_title(int win)
{
    gd_replyWin = win;

    if (win == -1) {
        debug_printf("\nError: demo_copy_title: win invalid!");
        return;
    }

    int width = win_width(win);
    if (width < 1) {
        debug_printf("\nError: demo_copy_title: width invalid!");
        return;
    }

    int height = win_height(win);
    if (height < 1) {
        debug_printf("\nError: demo_copy_title: length invalid!");
        return;
    }

    if (dialogueBackWindow == -1) {
        debug_printf("\nError: demo_copy_title: dialogueBackWindow wasn't created!");
        return;
    }

    unsigned char* src = win_get_buf(dialogueBackWindow);
    if (src == NULL) {
        debug_printf("\nError: demo_copy_title: couldn't get buffer!");
        return;
    }

    unsigned char* dest = win_get_buf(win);

    buf_to_buf(src + 640 * 225 + 135, width, height, 640, dest, width);
}

// 0x43F9D0
static void demo_copy_options(int win)
{
    gd_optionsWin = win;

    if (win == -1) {
        debug_printf("\nError: demo_copy_options: win invalid!");
        return;
    }

    int width = win_width(win);
    if (width < 1) {
        debug_printf("\nError: demo_copy_options: width invalid!");
        return;
    }

    int height = win_height(win);
    if (height < 1) {
        debug_printf("\nError: demo_copy_options: length invalid!");
        return;
    }

    if (dialogueBackWindow == -1) {
        debug_printf("\nError: demo_copy_options: dialogueBackWindow wasn't created!");
        return;
    }

    Rect windowRect;
    win_get_rect(dialogueWindow, &windowRect);
    windowRect.ulx -= (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    windowRect.uly -= (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;

    unsigned char* src = win_get_buf(dialogueWindow);
    if (src == NULL) {
        debug_printf("\nError: demo_copy_options: couldn't get buffer!");
        return;
    }

    unsigned char* dest = win_get_buf(win);
    buf_to_buf(src + 640 * (335 - windowRect.uly) + 127, width, height, 640, dest, width);
}

// 0x43FACC
static void gDialogRefreshOptionsRect(int win, Rect* drawRect)
{
    if (drawRect == NULL) {
        debug_printf("\nError: gDialogRefreshOptionsRect: drawRect NULL!");
        return;
    }

    if (win == -1) {
        debug_printf("\nError: gDialogRefreshOptionsRect: win invalid!");
        return;
    }

    if (dialogueBackWindow == -1) {
        debug_printf("\nError: gDialogRefreshOptionsRect: dialogueBackWindow wasn't created!");
        return;
    }

    Rect windowRect;
    win_get_rect(dialogueWindow, &windowRect);
    windowRect.ulx -= (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    windowRect.uly -= (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;

    unsigned char* src = win_get_buf(dialogueWindow);
    if (src == NULL) {
        debug_printf("\nError: gDialogRefreshOptionsRect: couldn't get buffer!");
        return;
    }

    if (drawRect->uly >= drawRect->lry) {
        debug_printf("\nError: gDialogRefreshOptionsRect: Invalid Rect (too many options)!");
        return;
    }

    if (drawRect->ulx >= drawRect->lrx) {
        debug_printf("\nError: gDialogRefreshOptionsRect: Invalid Rect (too many options)!");
        return;
    }

    int destWidth = win_width(win);
    unsigned char* dest = win_get_buf(win);

    buf_to_buf(
        src + (640 * (335 - windowRect.uly) + 127) + (640 * drawRect->uly + drawRect->ulx),
        drawRect->lrx - drawRect->ulx,
        drawRect->lry - drawRect->uly,
        640,
        dest + destWidth * drawRect->uly,
        destWidth);
}

// 0x43FC14
static void head_bk()
{
    // 0x5051AC
    static int loop_cnt = -1;

    // 0x5051B0
    static unsigned int tocksWaiting = 10000;

    switch (dialogue_switch_mode) {
    case 2:
        loop_cnt = -1;
        dialogue_switch_mode = 3;
        talk_to_destroy_dialogue_win();
        talk_to_create_barter_win();
        break;
    case 5:
        loop_cnt = -1;
        dialogue_switch_mode = 6;
        break;
    case 1:
        loop_cnt = -1;
        dialogue_switch_mode = 0;
        talk_to_destroy_barter_win();
        talk_to_create_dialogue_win();

        // NOTE: Uninline.
        gdialog_unhide();

        break;
    }

    if (fidgetFp == NULL) {
        return;
    }

    if (gdialog_speech_playing) {
        lips_bkg_proc();

        if (lips_draw_head) {
            talk_to_display_frame(lipsFp, head_phoneme_lookup[head_phoneme_current]);
            lips_draw_head = false;
        }

        if (!soundPlaying(lip_info.sound)) {
            gdialog_free_speech();
            talk_to_display_frame(lipsFp, 0);
            can_start_new_fidget = true;
            dialogue_seconds_since_last_input = 3;
            fidgetFrameCounter = 0;
        }
        return;
    }

    if (can_start_new_fidget) {
        if (elapsed_time(fidgetLastTime) >= tocksWaiting) {
            can_start_new_fidget = false;
            dialogue_seconds_since_last_input += tocksWaiting / 1000;
            tocksWaiting = 1000 * (roll_random(0, 3) + 4);
            talk_to_set_up_fidget(fidgetFID & 0xFFF, (fidgetFID & 0xFF0000) >> 16);
        }
        return;
    }

    if (elapsed_time(fidgetLastTime) >= fidgetTocksPerFrame) {
        if (art_frame_max_frame(fidgetFp) <= fidgetFrameCounter) {
            talk_to_display_frame(fidgetFp, 0);
            can_start_new_fidget = true;
        } else {
            talk_to_display_frame(fidgetFp, fidgetFrameCounter);
            fidgetLastTime = get_time();
            fidgetFrameCounter += 1;
        }
    }
}

// FIXME: Due to the bug in `gDialogProcessChoice` this function can receive invalid
// reaction value (50 instead of expected -1, 0, 1). It's handled gracefully by
// the game.
//
// 0x43FE3C
void talk_to_critter_reacts(int a1)
{
    int v1 = a1 + 1;

    debug_printf("Dialogue Reaction: ");
    if (v1 < 3) {
        debug_printf("%s\n", react_strs[v1]);
    }

    int v3 = a1 + 50;
    dialogue_seconds_since_last_input = 0;

    switch (v3) {
    case GAME_DIALOG_REACTION_GOOD:
        switch (fidgetAnim) {
        case FIDGET_GOOD:
            talk_to_play_transition(HEAD_ANIMATION_VERY_GOOD_REACTION);
            talk_to_set_up_fidget(dialogue_head, FIDGET_GOOD);
            break;
        case FIDGET_NEUTRAL:
            talk_to_play_transition(HEAD_ANIMATION_NEUTRAL_TO_GOOD);
            talk_to_set_up_fidget(dialogue_head, FIDGET_GOOD);
            break;
        case FIDGET_BAD:
            talk_to_play_transition(HEAD_ANIMATION_BAD_TO_NEUTRAL);
            talk_to_set_up_fidget(dialogue_head, FIDGET_NEUTRAL);
            break;
        }
        break;
    case GAME_DIALOG_REACTION_NEUTRAL:
        break;
    case GAME_DIALOG_REACTION_BAD:
        switch (fidgetAnim) {
        case FIDGET_GOOD:
            talk_to_play_transition(HEAD_ANIMATION_GOOD_TO_NEUTRAL);
            talk_to_set_up_fidget(dialogue_head, FIDGET_NEUTRAL);
            break;
        case FIDGET_NEUTRAL:
            talk_to_play_transition(HEAD_ANIMATION_NEUTRAL_TO_BAD);
            talk_to_set_up_fidget(dialogue_head, FIDGET_BAD);
            break;
        case FIDGET_BAD:
            talk_to_play_transition(HEAD_ANIMATION_VERY_BAD_REACTION);
            talk_to_set_up_fidget(dialogue_head, FIDGET_BAD);
            break;
        }
        break;
    }
}

// 0x43FF34
static void talk_to_scroll_subwin(int win, int a2, unsigned char* a3, unsigned char* a4, unsigned char* a5, int a6, int a7)
{
    int v7;
    unsigned char* v9;
    Rect rect;
    unsigned int tick;

    v7 = a6;
    v9 = a4;

    if (a2 == 1) {
        rect.ulx = 0;
        rect.lrx = GAME_DIALOG_WINDOW_WIDTH - 1;
        rect.lry = a6 - 1;

        int v18 = a6 / 10;
        if (a7 == -1) {
            rect.uly = 10;
            v18 = 0;
        } else {
            rect.uly = v18 * 10;
            v7 = a6 % 10;
            v9 += GAME_DIALOG_WINDOW_WIDTH * rect.uly;
        }

        for (; v18 >= 0; v18--) {
            sharedFpsLimiter.mark();

            soundUpdate();
            buf_to_buf(a3,
                GAME_DIALOG_WINDOW_WIDTH,
                v7,
                GAME_DIALOG_WINDOW_WIDTH,
                v9,
                GAME_DIALOG_WINDOW_WIDTH);
            rect.uly -= 10;
            win_draw_rect(win, &rect);
            v7 += 10;
            v9 -= 10 * (GAME_DIALOG_WINDOW_WIDTH);

            tick = get_time();
            while (elapsed_time(tick) < 33) {
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    } else {
        rect.lrx = GAME_DIALOG_WINDOW_WIDTH - 1;
        rect.lry = a6 - 1;
        rect.ulx = 0;
        rect.uly = 0;

        for (int index = a6 / 10; index > 0; index--) {
            sharedFpsLimiter.mark();

            soundUpdate();

            buf_to_buf(a5,
                GAME_DIALOG_WINDOW_WIDTH,
                10,
                GAME_DIALOG_WINDOW_WIDTH,
                v9,
                GAME_DIALOG_WINDOW_WIDTH);

            v9 += 10 * (GAME_DIALOG_WINDOW_WIDTH);
            v7 -= 10;
            a5 += 10 * (GAME_DIALOG_WINDOW_WIDTH);

            buf_to_buf(a3,
                GAME_DIALOG_WINDOW_WIDTH,
                v7,
                GAME_DIALOG_WINDOW_WIDTH,
                v9,
                GAME_DIALOG_WINDOW_WIDTH);

            win_draw_rect(win, &rect);

            rect.uly += 10;

            tick = get_time();
            while (elapsed_time(tick) < 33) {
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    }
}

// 0x440100
static int gdialog_review()
{
    int win;

    if (gdialog_review_init(&win) == -1) {
        debug_printf("\nError initializing review window!");
        return -1;
    }

    int top_line = 0;
    gdialog_review_display(win, top_line);

    while (true) {
        sharedFpsLimiter.mark();

        int keyCode = get_input();

        // CE: B / BACK cancels the review window.
        if (keyCode == -1) {
            keyCode = gdialog_gamepad_cancel_key();
        }

        if (keyCode == KEY_ESCAPE) {
            break;
        }

        if (keyCode == KEY_ARROW_UP) {
            top_line -= 1;
            if (top_line >= 0) {
                gdialog_review_display(win, top_line);
            } else {
                top_line = 0;
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            top_line += 1;
            if (top_line <= curReviewSlot - 1) {
                gdialog_review_display(win, top_line);
            } else {
                top_line = curReviewSlot - 1;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (gdialog_review_exit(&win) == -1) {
        return -1;
    }

    return 0;
}

// 0x44017C
static int gdialog_review_init(int* win)
{
    if (gdialog_speech_playing) {
        if (soundPlaying(lip_info.sound)) {
            gdialog_free_speech();
        }
    }

    reviewOldFont = text_curr();

    if (win == NULL) {
        return -1;
    }

    int reviewWindowX = (screenGetWidth() - GAME_DIALOG_REVIEW_WINDOW_WIDTH) / 2;
    int reviewWindowY = (screenGetHeight() - GAME_DIALOG_REVIEW_WINDOW_HEIGHT) / 2;
    *win = win_add(reviewWindowX,
        reviewWindowY,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        GAME_DIALOG_REVIEW_WINDOW_HEIGHT,
        256,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (*win == -1) {
        return -1;
    }

    int fid = art_id(OBJ_TYPE_INTERFACE, 102, 0, 0, 0);
    unsigned char* backgroundFrmData = art_ptr_lock_data(fid, 0, 0, &reviewBackKey);
    if (backgroundFrmData == NULL) {
        win_delete(*win);
        *win = -1;
        return -1;
    }

    unsigned char* windowBuffer = win_get_buf(*win);
    buf_to_buf(backgroundFrmData,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        GAME_DIALOG_REVIEW_WINDOW_HEIGHT,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        windowBuffer,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH);

    art_ptr_unlock(reviewBackKey);
    reviewBackKey = INVALID_CACHE_ENTRY;

    unsigned char* buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT];

    int index;
    for (index = 0; index < GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT; index++) {
        int fid = art_id(OBJ_TYPE_INTERFACE, reviewFids[index], 0, 0, 0);
        buttonFrmData[index] = art_ptr_lock_data(fid, 0, 0, &(reviewKeys[index]));
        if (buttonFrmData[index] == NULL) {
            break;
        }
    }

    if (index != GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT) {
        gdialog_review_exit(win);
        return -1;
    }

    int upBtn = win_register_button(*win,
        475,
        152,
        reviewFidWids[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP],
        reviewFidLens[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP],
        -1,
        -1,
        -1,
        KEY_ARROW_UP,
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_NORMAL],
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_PRESSED],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (upBtn == -1) {
        gdialog_review_exit(win);
        return -1;
    }

    win_register_button_sound_func(upBtn, gsound_med_butt_press, gsound_med_butt_release);

    int downBtn = win_register_button(*win,
        475,
        191,
        reviewFidWids[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN],
        reviewFidLens[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN],
        -1,
        -1,
        -1,
        KEY_ARROW_DOWN,
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_NORMAL],
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_PRESSED],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (downBtn == -1) {
        gdialog_review_exit(win);
        return -1;
    }

    win_register_button_sound_func(downBtn, gsound_med_butt_press, gsound_med_butt_release);

    int doneBtn = win_register_button(*win,
        499,
        398,
        reviewFidWids[GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE],
        reviewFidLens[GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE],
        -1,
        -1,
        -1,
        KEY_ESCAPE,
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_NORMAL],
        buttonFrmData[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_PRESSED],
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (doneBtn == -1) {
        gdialog_review_exit(win);
        return -1;
    }

    win_register_button_sound_func(doneBtn, gsound_red_butt_press, gsound_red_butt_release);

    text_font(101);

    win_draw(*win);

    remove_bk_process(head_bk);

    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 102, 0, 0, 0);
    reviewDispBuf = art_ptr_lock_data(backgroundFid, 0, 0, &reviewDispBackKey);
    if (reviewDispBuf == NULL) {
        gdialog_review_exit(win);
        return -1;
    }

    return 0;
}

// 0x44045C
static int gdialog_review_exit(int* win)
{
    add_bk_process(head_bk, "head");

    for (int index = 0; index < GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT; index++) {
        if (reviewKeys[index] != INVALID_CACHE_ENTRY) {
            art_ptr_unlock(reviewKeys[index]);
            reviewKeys[index] = INVALID_CACHE_ENTRY;
        }
    }

    if (reviewDispBackKey != INVALID_CACHE_ENTRY) {
        art_ptr_unlock(reviewDispBackKey);
        reviewDispBackKey = INVALID_CACHE_ENTRY;
        reviewDispBuf = NULL;
    }

    text_font(reviewOldFont);

    if (win == NULL) {
        return -1;
    }

    win_delete(*win);
    *win = -1;

    return 0;
}

// 0x4404E4
static void gdialog_review_display(int win, int origin)
{
    Rect entriesRect;
    entriesRect.ulx = 113;
    entriesRect.uly = 76;
    entriesRect.lrx = 422;
    entriesRect.lry = 418;

    int v20 = text_height() + 2;
    unsigned char* windowBuffer = win_get_buf(win);
    if (windowBuffer == NULL) {
        debug_printf("\nError: gdialog: review: can't find buffer!");
        return;
    }

    int width = GAME_DIALOG_WINDOW_WIDTH;
    buf_to_buf(
        reviewDispBuf + width * entriesRect.uly + entriesRect.ulx,
        width,
        entriesRect.lry - entriesRect.uly + 15,
        width,
        windowBuffer + width * entriesRect.uly + entriesRect.ulx,
        width);

    int y = 76;
    for (int index = origin; index < curReviewSlot; index++) {
        GameDialogReviewEntry* dialogReviewEntry = &(reviewList[index]);

        char name[60];
        snprintf(name, sizeof(name), "%s:", object_name(dialog_target));
        win_print(win, name, 180, 88, y, colorTable[992] | 0x2000000);
        entriesRect.uly += v20;

        char* replyText;
        if (dialogReviewEntry->replyMessageListId <= -3) {
            replyText = dialogReviewEntry->replyText;
        } else {
            replyText = scr_get_msg_str(dialogReviewEntry->replyMessageListId, dialogReviewEntry->replyMessageId);
        }

        if (replyText == NULL) {
            GNWSystemError("\nGDialog::Error Grabbing text message!");
            exit(1);
        }

        // NOTE: Uninline.
        y = text_to_rect_wrapped(windowBuffer + 113,
            &entriesRect,
            replyText,
            NULL,
            text_height(),
            640,
            colorTable[768] | 0x2000000);

        if (dialogReviewEntry->optionMessageListId != -3) {
            snprintf(name, sizeof(name), "%s:", object_name(obj_dude));
            win_print(win, name, 180, 88, y, colorTable[21140] | 0x2000000);
            entriesRect.uly += v20;

            char* optionText;
            if (dialogReviewEntry->optionMessageListId <= -3) {
                optionText = dialogReviewEntry->optionText;
            } else {
                optionText = scr_get_msg_str(dialogReviewEntry->optionMessageListId, dialogReviewEntry->optionMessageId);
            }

            if (optionText == NULL) {
                GNWSystemError("\nGDialog::Error Grabbing text message!");
                exit(1);
            }

            // NOTE: Uninline.
            y = text_to_rect_wrapped(windowBuffer + 113,
                &entriesRect,
                optionText,
                NULL,
                text_height(),
                640,
                colorTable[15855] | 0x2000000);
        }

        if (y >= 407) {
            break;
        }
    }

    entriesRect.ulx = 88;
    entriesRect.uly = 76;
    entriesRect.lry += 14;
    entriesRect.lrx = 434;
    win_draw_rect(win, &entriesRect);
}

// 0x440748
static int text_to_rect_wrapped(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color)
{
    return text_to_rect_func(buffer, rect, string, a4, height, pitch, color, 1);
}

// 0x440768
static int text_to_rect_func(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color, int a7)
{
    char* start;
    if (a4 != NULL) {
        start = string + *a4;
    } else {
        start = string;
    }

    int maxWidth = rect->lrx - rect->ulx;
    char* end = NULL;
    while (start != NULL && *start != '\0') {
        if (text_width(start) > maxWidth) {
            end = start + 1;
            while (*end != '\0' && *end != ' ') {
                end++;
            }

            if (*end != '\0') {
                char* lookahead = end + 1;
                while (lookahead != NULL) {
                    while (*lookahead != '\0' && *lookahead != ' ') {
                        lookahead++;
                    }

                    if (*lookahead == '\0') {
                        lookahead = NULL;
                    } else {
                        *lookahead = '\0';
                        if (text_width(start) >= maxWidth) {
                            *lookahead = ' ';
                            lookahead = NULL;
                        } else {
                            end = lookahead;
                            *lookahead = ' ';
                            lookahead++;
                        }
                    }
                }

                if (*end == ' ') {
                    *end = '\0';
                }
            } else {
                if (rect->lry - text_height() < rect->uly) {
                    return rect->uly;
                }

                if (a7 != 1 || start == string) {
                    text_to_buf(buffer + pitch * rect->uly + 10, start, maxWidth, pitch, color);
                } else {
                    text_to_buf(buffer + pitch * rect->uly, start, maxWidth, pitch, color);
                }

                if (a4 != NULL) {
                    *a4 += strlen(start) + 1;
                }

                rect->uly += height;
                return rect->uly;
            }
        }

        if (text_width(start) > maxWidth) {
            debug_printf("\nError: display_msg: word too long!");
            break;
        }

        if (a7 != 0) {
            if (rect->lry - text_height() < rect->uly) {
                if (end != NULL && *end == '\0') {
                    *end = ' ';
                }
                return rect->uly;
            }

            unsigned char* dest;
            if (a7 != 1 || start == string) {
                dest = buffer + 10;
            } else {
                dest = buffer;
            }
            text_to_buf(dest + pitch * rect->uly, start, maxWidth, pitch, color);
        }

        if (a4 != NULL && end != NULL) {
            *a4 += strlen(start) + 1;
        }

        rect->uly += height;

        if (end != NULL) {
            start = end + 1;
            if (*end == '\0') {
                *end = ' ';
            }
            end = NULL;
        } else {
            start = NULL;
        }
    }

    if (a4 != NULL) {
        *a4 = 0;
    }

    return rect->uly;
}

// 0x4409DC
void gdialogSetBarterMod(int modifier)
{
    gdBarterMod = modifier;
}

// 0x4409E4
int gdActivateBarter(int modifier)
{
    if (!dialog_state_fix) {
        return -1;
    }

    gdBarterMod = modifier;
    talk_to_pressed_barter(-1, -1);
    dialogue_state = 4;
    dialogue_switch_mode = 2;

    return 0;
}

// 0x440A30
void barter_end_to_talk_to()
{
    dialogQuit();
    dialogClose();
    updatePrograms();
    updateWindows();
    dialogue_state = 1;
    dialogue_switch_mode = 1;
}

// 0x440A58
static int talk_to_create_barter_win()
{
    int fid;
    unsigned char* normal;
    unsigned char* pressed;

    dialogue_state = 4;

    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 111, 0, 0, 0);
    CacheEntry* backgroundHandle;
    Art* backgroundFrm = art_ptr_lock(backgroundFid, &backgroundHandle);
    if (backgroundFrm == NULL) {
        return -1;
    }

    unsigned char* backgroundData = art_frame_data(backgroundFrm, 0, 0);
    if (backgroundData == NULL) {
        art_ptr_unlock(backgroundHandle);
        return -1;
    }

    dialogue_subwin_len = art_frame_length(backgroundFrm, 0, 0);

    int barterWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int barterWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - dialogue_subwin_len;
    dialogueWindow = win_add(barterWindowX,
        barterWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        dialogue_subwin_len,
        256,
        WINDOW_DONT_MOVE_TOP);
    if (dialogueWindow == -1) {
        art_ptr_unlock(backgroundHandle);
        return -1;
    }

    int width = GAME_DIALOG_WINDOW_WIDTH;

    unsigned char* windowBuffer = win_get_buf(dialogueWindow);
    unsigned char* backgroundWindowBuffer = win_get_buf(dialogueBackWindow);
    buf_to_buf(backgroundWindowBuffer + width * (480 - dialogue_subwin_len), width, dialogue_subwin_len, width, windowBuffer, width);

    talk_to_scroll_subwin(dialogueWindow, 1, backgroundData, windowBuffer, NULL, dialogue_subwin_len, 0);

    art_ptr_unlock(backgroundHandle);

    fid = art_id(OBJ_TYPE_INTERFACE, 96, 0, 0, 0);
    normal = art_ptr_lock_data(fid, 0, 0, &dialogue_redbut_Key1);
    if (normal == NULL) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 95, 0, 0, 0);
    pressed = art_ptr_lock_data(fid, 0, 0, &dialogue_redbut_Key2);
    if (pressed == NULL) {
        return -1;
    }

    // TRADE
    dialogue_bids[0] = win_register_button(dialogueWindow,
        41,
        163,
        14,
        14,
        -1,
        -1,
        -1,
        KEY_LOWERCASE_M,
        normal,
        pressed,
        0,
        BUTTON_FLAG_TRANSPARENT);
    if (dialogue_bids[0] != -1) {
        win_register_button_sound_func(dialogue_bids[0], gsound_med_butt_press, gsound_med_butt_release);
    }

    // TALK
    dialogue_bids[1] = win_register_button(dialogueWindow,
        584,
        162,
        14,
        14,
        -1,
        -1,
        -1,
        KEY_LOWERCASE_T,
        normal,
        pressed,
        0,
        BUTTON_FLAG_TRANSPARENT);
    if (dialogue_bids[1] != -1) {
        win_register_button_sound_func(dialogue_bids[1], gsound_med_butt_press, gsound_med_butt_release);
    }

    if (obj_new(&peon_table_obj, -1, -1) == -1) {
        return -1;
    }

    peon_table_obj->flags |= OBJECT_HIDDEN;

    if (obj_new(&barterer_table_obj, -1, -1) == -1) {
        return -1;
    }

    barterer_table_obj->flags |= OBJECT_HIDDEN;

    if (obj_new(&barterer_temp_obj, dialog_target->fid, -1) == -1) {
        return -1;
    }

    barterer_temp_obj->flags |= OBJECT_HIDDEN | OBJECT_NO_SAVE;
    barterer_temp_obj->sid = -1;
    return 0;
}

// 0x440CEC
static void talk_to_destroy_barter_win()
{
    if (dialogueWindow == -1) {
        return;
    }

    obj_erase_object(barterer_temp_obj, 0);
    obj_erase_object(barterer_table_obj, 0);
    obj_erase_object(peon_table_obj, 0);

    for (int index = 0; index < 2; index++) {
        win_delete_button(dialogue_bids[index]);
    }

    art_ptr_unlock(dialogue_redbut_Key1);
    art_ptr_unlock(dialogue_redbut_Key2);

    unsigned char* backgroundWindowBuffer = win_get_buf(dialogueBackWindow);
    backgroundWindowBuffer += (GAME_DIALOG_WINDOW_WIDTH) * (480 - dialogue_subwin_len);

    CacheEntry* backgroundFrmHandle;
    int fid = art_id(OBJ_TYPE_INTERFACE, 111, 0, 0, 0);
    unsigned char* backgroundFrmData = art_ptr_lock_data(fid, 0, 0, &backgroundFrmHandle);
    if (backgroundFrmData != NULL) {
        unsigned char* windowBuffer = win_get_buf(dialogueWindow);
        talk_to_scroll_subwin(dialogueWindow, 0, backgroundFrmData, windowBuffer, backgroundWindowBuffer, dialogue_subwin_len, 0);
        art_ptr_unlock(backgroundFrmHandle);
        win_delete(dialogueWindow);
        dialogueWindow = -1;
    }
}

// 0x440DE4
static void dialogue_barter_cleanup_tables()
{
    Inventory* inventory;
    int length;

    inventory = &(peon_table_obj->data.inventory);
    length = inventory->length;
    for (int index = 0; index < length; index++) {
        Object* item = inventory->items->item;
        int quantity = item_count(peon_table_obj, item);
        item_move_force(peon_table_obj, obj_dude, item, quantity);
    }

    inventory = &(barterer_table_obj->data.inventory);
    length = inventory->length;
    for (int index = 0; index < length; index++) {
        Object* item = inventory->items->item;
        int quantity = item_count(barterer_table_obj, item);
        item_move_force(barterer_table_obj, dialog_target, item, quantity);
    }

    if (barterer_temp_obj != NULL) {
        inventory = &(barterer_temp_obj->data.inventory);
        length = inventory->length;
        for (int index = 0; index < length; index++) {
            Object* item = inventory->items->item;
            int quantity = item_count(barterer_temp_obj, item);
            item_move_force(barterer_temp_obj, dialog_target, item, quantity);
        }
    }
}

// 0x440EC4
static void talk_to_pressed_barter(int btn, int keyCode)
{
    if (PID_TYPE(dialog_target->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    Script* script;
    if (scr_ptr(dialog_target->sid, &script) == -1) {
        return;
    }

    Proto* proto;
    proto_ptr(dialog_target->pid, &proto);
    if ((proto->critter.data.flags & CRITTER_BARTER) != 0) {
        if (gdialog_speech_playing) {
            if (soundPlaying(lip_info.sound)) {
                gdialog_free_speech();
            }
        }

        dialogue_switch_mode = 2;
        dialogue_state = 4;

        // NOTE: Uninline.
        gdialog_hide();
    } else {
        MessageListItem messageListItem;
        // This person will not barter with you.
        messageListItem.num = 903;

        if (message_search(&proto_main_msg_file, &messageListItem)) {
            gdialog_display_msg(messageListItem.text);
        } else {
            debug_printf("\nError: gdialog: Can't find message!");
        }
    }
}

// 0x440FB4
static void talk_to_pressed_about(int btn, int keyCode)
{
    MessageListItem mesg;
    int reaction;
    int reaction_level;

    if (PID_TYPE(dialog_target->pid) == OBJ_TYPE_CRITTER) {
        reaction = reaction_get(dialog_target);
        reaction_level = reaction_to_level(reaction);
        if (reaction_level != 0) {
            if (map_data.field_34 != 35) {
                if (gdialog_speech_playing == 1) {
                    if (soundPlay(lip_info.sound)) {
                        gdialog_free_speech();
                    }
                }

                dialogue_switch_mode = 5;
                gdialog_hide();
            } else {
                mesg.num = 904;
                if (message_search(&proto_main_msg_file, &mesg) != 1) {
                    debug_printf("\nError: gdialog: Can't find message!");
                }
                gdialog_display_msg(mesg.text);
            }
        } else {
            mesg.num = 904;
            if (message_search(&proto_main_msg_file, &mesg) != 1) {
                debug_printf("\nError: gdialog: Can't find message!");
            }
            // NOTE: Message is not used.
        }
    } else {
        mesg.num = 904;
        if (message_search(&proto_main_msg_file, &mesg) != 1) {
            debug_printf("\nError: gdialog: Can't find message!");
        }
        // NOTE: Message is not used.
    }
}

// NOTE: Uncollapsed 0x445CA0 with different signature.
static void talk_to_pressed_review(int btn, int keyCode)
{
    gdialog_review();
}

// 0x4410D8
static int talk_to_create_dialogue_win()
{
    int fid;
    unsigned char* normal;
    unsigned char* pressed;
    const int screenWidth = GAME_DIALOG_WINDOW_WIDTH;

    if (dial_win_created) {
        return -1;
    }

    dial_win_created = true;

    CacheEntry* backgroundFrmHandle;
    int backgroundFid = art_id(OBJ_TYPE_INTERFACE, 99, 0, 0, 0);
    Art* backgroundFrm = art_ptr_lock(backgroundFid, &backgroundFrmHandle);
    if (backgroundFrm == NULL) {
        return -1;
    }

    unsigned char* backgroundFrmData = art_frame_data(backgroundFrm, 0, 0);
    if (backgroundFrmData == NULL) {
        return -1;
    }

    dialogue_subwin_len = art_frame_length(backgroundFrm, 0, 0);

    int dialogSubwindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int dialogSubwindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - dialogue_subwin_len;
    dialogueWindow = win_add(dialogSubwindowX,
        dialogSubwindowY,
        screenWidth,
        dialogue_subwin_len,
        256,
        WINDOW_DONT_MOVE_TOP);
    if (dialogueWindow == -1) {
        return -1;
    }

    unsigned char* v10 = win_get_buf(dialogueWindow);
    unsigned char* v14 = win_get_buf(dialogueBackWindow);
    buf_to_buf(v14 + screenWidth * (GAME_DIALOG_WINDOW_HEIGHT - dialogue_subwin_len),
        screenWidth,
        dialogue_subwin_len,
        screenWidth,
        v10,
        screenWidth);

    if (dialogue_just_started) {
        win_draw(dialogueBackWindow);
        talk_to_scroll_subwin(dialogueWindow, 1, backgroundFrmData, v10, 0, dialogue_subwin_len, -1);
        dialogue_just_started = 0;
    } else {
        talk_to_scroll_subwin(dialogueWindow, 1, backgroundFrmData, v10, 0, dialogue_subwin_len, 0);
    }

    art_ptr_unlock(backgroundFrmHandle);

    fid = art_id(OBJ_TYPE_INTERFACE, 96, 0, 0, 0);
    normal = art_ptr_lock_data(fid, 0, 0, &dialogue_redbut_Key1);
    if (normal == NULL) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 95, 0, 0, 0);
    pressed = art_ptr_lock_data(fid, 0, 0, &dialogue_redbut_Key2);
    if (pressed == NULL) {
        return -1;
    }

    // CE: with a controller, bake "Y" (barter) / "X" (ask about) into private copies of
    // the button art so the player sees which pad button opens each (matches the pad
    // shortcuts in gDialogProcess). Free any stale copies first (create is paired with a
    // destroy, so no live button references them here).
    unsigned char* barterUp = normal;
    unsigned char* askUp = normal;
    if (gamepad_ui_get_controller() != NULL) {
        if (gd_barter_hint_up != NULL) {
            mem_free(gd_barter_hint_up);
            gd_barter_hint_up = NULL;
        }
        if (gd_ask_hint_up != NULL) {
            mem_free(gd_ask_hint_up);
            gd_ask_hint_up = NULL;
        }

        CacheEntry* redKey;
        Art* redArt = art_ptr_lock(art_id(OBJ_TYPE_INTERFACE, 96, 0, 0, 0), &redKey);
        if (redArt != NULL) {
            int redW = art_frame_width(redArt, 0, 0);
            int redH = art_frame_length(redArt, 0, 0);
            unsigned char* redData = art_frame_data(redArt, 0, 0);
            int redSize = redW * redH;

            gd_barter_hint_up = (unsigned char*)mem_malloc(redSize);
            gd_ask_hint_up = (unsigned char*)mem_malloc(redSize);
            if (gd_barter_hint_up != NULL && gd_ask_hint_up != NULL && redData != NULL) {
                memcpy(gd_barter_hint_up, redData, redSize);
                memcpy(gd_ask_hint_up, redData, redSize);

                int oldFont = text_curr();
                text_font(101);
                int hintY = (redH - text_height()) / 2;
                if (hintY < 0) {
                    hintY = 0;
                }
                text_to_buf(gd_barter_hint_up + redW * hintY + (redW - text_width("Y")) / 2,
                    "Y", redW, redW, colorTable[32767]);
                text_to_buf(gd_ask_hint_up + redW * hintY + (redW - text_width("X")) / 2,
                    "X", redW, redW, colorTable[32767]);
                text_font(oldFont);

                barterUp = gd_barter_hint_up;
                askUp = gd_ask_hint_up;
            }
            art_ptr_unlock(redKey);
        }
    }

    // BARTER/TRADE
    dialogue_bids[0] = win_register_button(dialogueWindow,
        593,
        41,
        14,
        14,
        -1,
        -1,
        -1,
        -1,
        barterUp,
        pressed,
        NULL,
        BUTTON_FLAG_TRANSPARENT);
    if (dialogue_bids[0] != -1) {
        win_register_button_func(dialogue_bids[0], NULL, NULL, NULL, talk_to_pressed_barter);
        win_register_button_sound_func(dialogue_bids[0], gsound_med_butt_press, gsound_med_butt_release);
    }

    // ASK ABOUT
    dialogue_bids[1] = win_register_button(dialogueWindow,
        593,
        116,
        14,
        14,
        -1,
        -1,
        -1,
        -1,
        askUp,
        pressed,
        0,
        BUTTON_FLAG_TRANSPARENT);
    if (dialogue_bids[1] != -1) {
        win_register_button_func(dialogue_bids[1], NULL, NULL, NULL, talk_to_pressed_about);
        win_register_button_sound_func(dialogue_bids[1], gsound_med_butt_press, gsound_med_butt_release);
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 97, 0, 0, 0);
    normal = art_ptr_lock_data(fid, 0, 0, &dialogue_rest_Key1);
    if (normal == NULL) {
        return -1;
    }

    fid = art_id(OBJ_TYPE_INTERFACE, 98, 0, 0, 0);
    pressed = art_ptr_lock_data(fid, 0, 0, &dialogue_rest_Key2);

    // REVIEW
    dialogue_bids[2] = win_register_button(dialogueWindow,
        13,
        154,
        51,
        29,
        -1,
        -1,
        -1,
        -1,
        normal,
        pressed,
        NULL,
        0);
    if (dialogue_bids[2] != -1) {
        win_register_button_func(dialogue_bids[2], NULL, NULL, NULL, talk_to_pressed_review);
        win_register_button_sound_func(dialogue_bids[2], gsound_red_butt_press, gsound_red_butt_release);
    }

    return 0;
}

// 0x441430
static void talk_to_destroy_dialogue_win()
{
    if (dialogueWindow == -1) {
        return;
    }

    for (int index = 0; index < 3; index++) {
        win_delete_button(dialogue_bids[index]);
    }

    // CE: free the controller Y/X hint art copies now the buttons using them are gone.
    if (gd_barter_hint_up != NULL) {
        mem_free(gd_barter_hint_up);
        gd_barter_hint_up = NULL;
    }
    if (gd_ask_hint_up != NULL) {
        mem_free(gd_ask_hint_up);
        gd_ask_hint_up = NULL;
    }

    art_ptr_unlock(dialogue_redbut_Key1);
    art_ptr_unlock(dialogue_redbut_Key2);
    art_ptr_unlock(dialogue_rest_Key1);
    art_ptr_unlock(dialogue_rest_Key2);

    int offset = (GAME_DIALOG_WINDOW_WIDTH) * (480 - dialogue_subwin_len);
    unsigned char* backgroundWindowBuffer = win_get_buf(dialogueBackWindow) + offset;

    CacheEntry* backgroundFrmHandle;
    int fid = art_id(OBJ_TYPE_INTERFACE, 99, 0, 0, 0);
    unsigned char* backgroundFrmData = art_ptr_lock_data(fid, 0, 0, &backgroundFrmHandle);
    if (backgroundFrmData != NULL) {
        unsigned char* windowBuffer = win_get_buf(dialogueWindow);
        talk_to_scroll_subwin(dialogueWindow, 0, backgroundFrmData, windowBuffer, backgroundWindowBuffer, dialogue_subwin_len, 0);
        art_ptr_unlock(backgroundFrmHandle);
        win_delete(dialogueWindow);
        dial_win_created = 0;
        dialogueWindow = -1;
    }
}

// 0x441524
static int talk_to_create_background_window()
{
    int backgroundWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int backgroundWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;
    dialogueBackWindow = win_add(backgroundWindowX,
        backgroundWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        GAME_DIALOG_WINDOW_HEIGHT,
        256,
        WINDOW_DONT_MOVE_TOP);

    if (dialogueBackWindow != -1) {
        return 0;
    }

    return -1;
}

// 0x441564
static int talk_to_refresh_background_window()
{
    CacheEntry* backgroundFrmHandle;
    // alltlk.frm - dialog screen background
    int fid = art_id(OBJ_TYPE_INTERFACE, 103, 0, 0, 0);
    unsigned char* backgroundFrmData = art_ptr_lock_data(fid, 0, 0, &backgroundFrmHandle);
    if (backgroundFrmData == NULL) {
        return -1;
    }

    int windowWidth = GAME_DIALOG_WINDOW_WIDTH;
    unsigned char* windowBuffer = win_get_buf(dialogueBackWindow);
    buf_to_buf(backgroundFrmData, windowWidth, 480, windowWidth, windowBuffer, windowWidth);
    art_ptr_unlock(backgroundFrmHandle);

    if (!dialogue_just_started) {
        win_draw(dialogueBackWindow);
    }

    return 0;
}

// 0x4415F4
static int talk_to_create_head_window()
{
    dialogue_state = 1;

    int windowWidth = GAME_DIALOG_WINDOW_WIDTH;

    // NOTE: Uninline.
    talk_to_create_background_window();
    talk_to_refresh_background_window();

    unsigned char* buf = win_get_buf(dialogueBackWindow);

    for (int index = 0; index < 8; index++) {
        soundUpdate();

        Rect* rect = &(backgrndRects[index]);
        int width = rect->lrx - rect->ulx;
        int height = rect->lry - rect->uly;
        backgrndBufs[index] = (unsigned char*)mem_malloc(width * height);
        if (backgrndBufs[index] == NULL) {
            return -1;
        }

        unsigned char* src = buf;
        src += windowWidth * rect->uly + rect->ulx;

        buf_to_buf(src, width, height, windowWidth, backgrndBufs[index], width);
    }

    talk_to_create_dialogue_win();

    headWindowBuffer = win_get_buf(dialogueBackWindow) + windowWidth * 14 + 126;

    if (headWindowBuffer == NULL) {
        talk_to_destroy_head_window();
        return -1;
    }

    return 0;
}

// 0x44172C
static void talk_to_destroy_head_window()
{
    if (dialogueWindow != -1) {
        headWindowBuffer = NULL;
    }

    if (dialogue_state == 1) {
        talk_to_destroy_dialogue_win();
    } else if (dialogue_state == 4) {
        talk_to_destroy_barter_win();
    }

    if (dialogueBackWindow != -1) {
        win_delete(dialogueBackWindow);
        dialogueBackWindow = -1;
    }

    for (int index = 0; index < 8; index++) {
        mem_free(backgrndBufs[index]);
    }
}

// 0x441798
static void talk_to_set_up_fidget(int headFrmId, int reaction)
{
    // 0x505228
    static int phone_anim = 0;

    fidgetFrameCounter = 0;

    if (headFrmId == -1) {
        fidgetFID = -1;
        fidgetFp = NULL;
        fidgetKey = INVALID_CACHE_ENTRY;
        fidgetAnim = -1;
        fidgetTocksPerFrame = 0;
        fidgetLastTime = 0;
        talk_to_display_frame(NULL, 0);
        lipsFID = 0;
        lipsKey = NULL;
        lipsFp = 0;
        return;
    }

    int anim = HEAD_ANIMATION_NEUTRAL_PHONEMES;
    switch (reaction) {
    case FIDGET_GOOD:
        anim = HEAD_ANIMATION_GOOD_PHONEMES;
        break;
    case FIDGET_BAD:
        anim = HEAD_ANIMATION_BAD_PHONEMES;
        break;
    }

    if (lipsFID != 0) {
        if (anim != phone_anim) {
            if (art_ptr_unlock(lipsKey) == -1) {
                debug_printf("failure unlocking lips frame!\n");
            }
            lipsKey = NULL;
            lipsFp = NULL;
            lipsFID = 0;
        }
    }

    if (lipsFID == 0) {
        phone_anim = anim;
        lipsFID = art_id(OBJ_TYPE_HEAD, headFrmId, anim, 0, 0);
        lipsFp = art_ptr_lock(lipsFID, &lipsKey);
        if (lipsFp == NULL) {
            debug_printf("failure!\n");

            char stats[200];
            cache_stats(&art_cache, stats, sizeof(stats));
            debug_printf("%s", stats);
        }
    }

    int fid = art_id(OBJ_TYPE_HEAD, headFrmId, reaction, 0, 0);
    int fidgetCount = art_head_fidgets(fid);
    if (fidgetCount == -1) {
        debug_printf("\tError - No available fidgets for given frame id\n");
        return;
    }

    int chance = roll_random(1, 100) + dialogue_seconds_since_last_input / 2;

    int fidget = fidgetCount;
    switch (fidgetCount) {
    case 1:
        fidget = 1;
        break;
    case 2:
        if (chance < 68) {
            fidget = 1;
        } else {
            fidget = 2;
        }
        break;
    case 3:
        dialogue_seconds_since_last_input = 0;
        if (chance < 52) {
            fidget = 1;
        } else if (chance < 77) {
            fidget = 2;
        } else {
            fidget = 3;
        }
        break;
    }

    debug_printf("Choosing fidget %d out of %d\n", fidget, fidgetCount);

    if (fidgetFp != NULL) {
        if (art_ptr_unlock(fidgetKey) == -1) {
            debug_printf("failure!\n");
        }
    }

    fidgetFID = art_id(OBJ_TYPE_HEAD, headFrmId, reaction, fidget, 0);
    fidgetFrameCounter = 0;
    fidgetFp = art_ptr_lock(fidgetFID, &fidgetKey);
    if (fidgetFp == NULL) {
        debug_printf("failure!\n");

        char stats[200];
        cache_stats(&art_cache, stats, sizeof(stats));
        debug_printf("%s", stats);
    }

    fidgetLastTime = 0;
    fidgetAnim = reaction;
    fidgetTocksPerFrame = 1000 / art_frame_fps(fidgetFp);
}

// 0x441A30
static void talk_to_wait_for_fidget()
{
    if (fidgetFp == NULL) {
        return;
    }

    if (dialogueWindow == -1) {
        return;
    }

    debug_printf("Waiting for fidget to complete...\n");

    while (art_frame_max_frame(fidgetFp) > fidgetFrameCounter) {
        sharedFpsLimiter.mark();

        if (elapsed_time(fidgetLastTime) >= fidgetTocksPerFrame) {
            talk_to_display_frame(fidgetFp, fidgetFrameCounter);
            fidgetLastTime = get_time();
            fidgetFrameCounter++;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    fidgetFrameCounter = 0;
}

// 0x441AAC
static void talk_to_play_transition(int anim)
{
    if (fidgetFp == NULL) {
        return;
    }

    if (dialogueWindow == -1) {
        return;
    }

    mouse_hide();

    debug_printf("Starting transition...\n");

    talk_to_wait_for_fidget();

    if (fidgetFp != NULL) {
        if (art_ptr_unlock(fidgetKey) == -1) {
            debug_printf("\tError unlocking fidget in transition func...");
        }
        fidgetFp = NULL;
    }

    CacheEntry* headFrmHandle;
    int headFid = art_id(OBJ_TYPE_HEAD, dialogue_head, anim, 0, 0);
    Art* headFrm = art_ptr_lock(headFid, &headFrmHandle);
    if (headFrm == NULL) {
        debug_printf("\tError locking transition...\n");
    }

    unsigned int delay = 1000 / art_frame_fps(headFrm);

    int frame = 0;
    unsigned int time = 0;
    while (frame < art_frame_max_frame(headFrm)) {
        sharedFpsLimiter.mark();

        if (elapsed_time(time) >= delay) {
            talk_to_display_frame(headFrm, frame);
            time = get_time();
            frame++;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (art_ptr_unlock(headFrmHandle) == -1) {
        debug_printf("\tError unlocking transition...\n");
    }

    debug_printf("Finished transition...\n");
    mouse_show();
}

// 0x441BBC
static void talk_to_translucent_trans_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, unsigned char* a9, unsigned char* a10)
{
    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;

    dest += destPitch * destY + destX;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            unsigned char v1 = *src++;
            if (v1 != 0) {
                v1 = (256 - v1) >> 4;
            }

            unsigned char v15 = *dest;
            *dest++ = a9[256 * v1 + v15];
        }
        src += srcStep;
        dest += destStep;
    }
}

// 0x441C50
static void talk_to_display_frame(Art* headFrm, int frame)
{
    // 0x50522C
    static int totalHotx = 0;

    if (dialogueWindow == -1) {
        return;
    }

    if (headFrm != NULL) {
        if (frame == 0) {
            totalHotx = 0;
        }

        int backgroundFid = art_id(OBJ_TYPE_BACKGROUND, backgroundIndex, 0, 0, 0);

        CacheEntry* backgroundHandle;
        Art* backgroundFrm = art_ptr_lock(backgroundFid, &backgroundHandle);
        if (backgroundFrm == NULL) {
            debug_printf("\tError locking background in display...\n");
        }

        unsigned char* backgroundFrmData = art_frame_data(backgroundFrm, 0, 0);
        if (backgroundFrmData != NULL) {
            buf_to_buf(backgroundFrmData, 388, 200, 388, headWindowBuffer, GAME_DIALOG_WINDOW_WIDTH);
        } else {
            debug_printf("\tError getting background data in display...\n");
        }

        art_ptr_unlock(backgroundHandle);

        int width = art_frame_width(headFrm, frame, 0);
        int height = art_frame_length(headFrm, frame, 0);
        unsigned char* data = art_frame_data(headFrm, frame, 0);

        int a3;
        int v8;
        art_frame_offset(headFrm, 0, &a3, &v8);

        int a4;
        int a5;
        art_frame_hot(headFrm, frame, 0, &a4, &a5);

        totalHotx += a4;
        a3 += totalHotx;

        if (data != NULL) {
            int destWidth = GAME_DIALOG_WINDOW_WIDTH;
            int destOffset = destWidth * (200 - height) + a3 + (388 - width) / 2;
            if (destOffset + width * v8 > 0) {
                destOffset += width * v8;
            }

            trans_buf_to_buf(
                data,
                width,
                height,
                width,
                headWindowBuffer + destOffset,
                destWidth);
        } else {
            debug_printf("\tError getting head data in display...\n");
        }
    } else {
        if (talk_need_to_center == 1) {
            talk_need_to_center = 0;
            tile_refresh_display();
        }

        unsigned char* src = win_get_buf(display_win);
        buf_to_buf(
            src + ((win_height(display_win) - 232) / 2) * win_width(display_win) + (win_width(display_win) - 388) / 2,
            388,
            200,
            win_width(display_win),
            headWindowBuffer,
            GAME_DIALOG_WINDOW_WIDTH);
    }

    Rect v27;
    v27.ulx = 126;
    v27.uly = 14;
    v27.lrx = 514;
    v27.lry = 214;

    unsigned char* dest = win_get_buf(dialogueBackWindow);

    unsigned char* data1 = art_frame_data(upper_hi_fp, 0, 0);
    talk_to_translucent_trans_buf_to_buf(data1, upper_hi_wid, upper_hi_len, upper_hi_wid, dest, 426, 15, GAME_DIALOG_WINDOW_WIDTH, light_BlendTable, light_GrayTable);

    unsigned char* data2 = art_frame_data(lower_hi_fp, 0, 0);
    talk_to_translucent_trans_buf_to_buf(data2, lower_hi_wid, lower_hi_len, lower_hi_wid, dest, 129, 214 - lower_hi_len - 2, GAME_DIALOG_WINDOW_WIDTH, dark_BlendTable, dark_GrayTable);

    for (int index = 0; index < 8; ++index) {
        Rect* rect = &(backgrndRects[index]);
        int width = rect->lrx - rect->ulx;

        trans_buf_to_buf(backgrndBufs[index],
            width,
            rect->lry - rect->uly,
            width,
            dest + GAME_DIALOG_WINDOW_WIDTH * rect->uly + rect->ulx,
            GAME_DIALOG_WINDOW_WIDTH);
    }

    win_draw_rect(dialogueBackWindow, &v27);
}

// 0x441FD4
static void talk_to_blend_table_init()
{
    for (int color = 0; color < 256; color++) {
        int r = (Color2RGB(color) & 0x7C00) >> 10;
        int g = (Color2RGB(color) & 0x3E0) >> 5;
        int b = Color2RGB(color) & 0x1F;
        light_GrayTable[color] = ((r + 2 * g + 2 * b) / 10) >> 2;
        dark_GrayTable[color] = ((r + g + b) / 10) >> 2;
    }

    light_GrayTable[0] = 0;
    dark_GrayTable[0] = 0;

    light_BlendTable = getColorBlendTable(colorTable[17969]);
    dark_BlendTable = getColorBlendTable(colorTable[22187]);

    // hilight1.frm - dialogue upper hilight
    int upperHighlightFid = art_id(OBJ_TYPE_INTERFACE, 115, 0, 0, 0);
    upper_hi_fp = art_ptr_lock(upperHighlightFid, &upper_hi_key);
    upper_hi_wid = art_frame_width(upper_hi_fp, 0, 0);
    upper_hi_len = art_frame_length(upper_hi_fp, 0, 0);

    // hilight2.frm - dialogue lower hilight
    int lowerHighlightFid = art_id(OBJ_TYPE_INTERFACE, 116, 0, 0, 0);
    lower_hi_fp = art_ptr_lock(lowerHighlightFid, &lower_hi_key);
    lower_hi_wid = art_frame_width(lower_hi_fp, 0, 0);
    lower_hi_len = art_frame_length(lower_hi_fp, 0, 0);
}

// 0x442128
static void talk_to_blend_table_exit()
{
    freeColorBlendTable(colorTable[17969]);
    freeColorBlendTable(colorTable[22187]);

    art_ptr_unlock(upper_hi_key);
    art_ptr_unlock(lower_hi_key);
}

// 0x442154
static int about_init()
{
    int fid;
    CacheEntry* background_key;
    Art* background_frm;
    unsigned char* background_data;
    int background_width;
    int background_height;
    MessageList msg_file;
    MessageListItem mesg;
    int width;
    Art* button_up_frm;
    unsigned char* button_up_data;
    Art* button_down_frm;
    unsigned char* button_down_data;
    int button_width;
    int button_height;
    int btn;

    if (about_win == -1) {
        about_old_font = text_curr();

        fid = art_id(OBJ_TYPE_INTERFACE, 238, 0, 0, 0);
        background_frm = art_ptr_lock(fid, &background_key);
        if (background_frm != NULL) {
            background_data = art_frame_data(background_frm, 0, 0);
            if (background_data != NULL) {
                background_width = art_frame_width(background_frm, 0, 0);
                background_height = art_frame_length(background_frm, 0, 0);
                about_win_width = background_width;
                about_win = win_add((screenGetWidth() - background_width) / 2,
                    (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + 356,
                    background_width,
                    background_height,
                    colorTable[0],
                    WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
                if (about_win != -1) {
                    about_win_buf = win_get_buf(about_win);
                    if (about_win_buf != NULL) {
                        buf_to_buf(background_data,
                            background_width,
                            background_height,
                            background_width,
                            about_win_buf,
                            background_width);

                        text_font(103);

                        if (message_init(&msg_file) == 1 && message_load(&msg_file, "game\\misc.msg") == 1) {
                            mesg.num = 6000;
                            if (message_search(&msg_file, &mesg) == 1) {
                                width = text_width(mesg.text);
                                text_to_buf(about_win_buf + background_width * 7 + (background_width - width) / 2,
                                    mesg.text,
                                    background_width - (background_width - width) / 2,
                                    background_width,
                                    colorTable[18979]);
                                message_exit(&msg_file);

                                text_font(103);

                                if (message_init(&msg_file) == 1 && message_load(&msg_file, "game\\dbox.msg") == 1) {
                                    mesg.num = 100;
                                    if (message_search(&msg_file, &mesg) == 1) {
                                        text_to_buf(about_win_buf + background_width * 57 + 56,
                                            mesg.text,
                                            background_width - 56,
                                            background_width,
                                            colorTable[18979]);

                                        mesg.num = 103;
                                        if (message_search(&msg_file, &mesg) == 1) {
                                            text_to_buf(about_win_buf + background_width * 57 + 181,
                                                mesg.text,
                                                background_width - 181,
                                                background_width,
                                                colorTable[18979]);
                                            message_exit(&msg_file);

                                            fid = art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
                                            button_up_frm = art_ptr_lock(fid, &about_button_up_key);
                                            if (button_up_frm != NULL) {
                                                button_up_data = art_frame_data(button_up_frm, 0, 0);
                                                if (button_up_data != NULL) {
                                                    fid = art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
                                                    button_down_frm = art_ptr_lock(fid, &about_button_down_key);
                                                    if (button_down_frm != NULL) {
                                                        button_down_data = art_frame_data(button_down_frm, 0, 0);
                                                        if (button_down_data != NULL) {
                                                            button_width = art_frame_width(button_down_frm, 0, 0);
                                                            button_height = art_frame_length(button_down_frm, 0, 0);

                                                            btn = win_register_button(about_win,
                                                                34,
                                                                58,
                                                                button_width,
                                                                button_height,
                                                                -1,
                                                                -1,
                                                                -1,
                                                                KEY_RETURN,
                                                                button_up_data,
                                                                button_down_data,
                                                                NULL,
                                                                BUTTON_FLAG_TRANSPARENT);
                                                            if (btn != -1) {
                                                                win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);

                                                                btn = win_register_button(about_win,
                                                                    160,
                                                                    58,
                                                                    button_width,
                                                                    button_height,
                                                                    -1,
                                                                    -1,
                                                                    -1,
                                                                    KEY_ESCAPE,
                                                                    button_up_data,
                                                                    button_down_data,
                                                                    NULL,
                                                                    BUTTON_FLAG_TRANSPARENT);
                                                                if (btn != -1) {
                                                                    win_register_button_sound_func(btn, gsound_red_butt_press, gsound_red_butt_release);

                                                                    about_input_string = (char*)mem_malloc(128);
                                                                    if (about_input_string != NULL) {
                                                                        strcpy(about_restore_string, dialogBlock.replyText);
                                                                        about_reset_string();
                                                                        about_last_time = get_time();
                                                                        about_update_display(0);

                                                                        art_ptr_unlock(background_key);

                                                                        win_draw(about_win);
                                                                        return 0;
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        art_ptr_unlock(about_button_down_key);
                                                    }
                                                }
                                                art_ptr_unlock(about_button_up_key);
                                            }
                                        }
                                    }
                                }
                            }
                            message_exit(&msg_file);
                        }
                    }
                    win_delete(about_win);
                    about_win = -1;
                }
            }

            art_ptr_unlock(background_key);
        }
    }

    text_font(about_old_font);
    GNWSystemError("Unable to create dialog box.");
    return -1;
}

// 0x442578
static void about_exit()
{
    if (about_win != -1) {
        if (about_input_string != NULL) {
            mem_free(about_input_string);
            about_input_string = NULL;
        }

        if (about_button_up_key != NULL) {
            art_ptr_unlock(about_button_up_key);
            about_button_up_key = NULL;
        }

        if (about_button_down_key != NULL) {
            art_ptr_unlock(about_button_down_key);
            about_button_down_key = NULL;
        }

        win_delete(about_win);
        about_win = -1;

        text_font(about_old_font);
    }
}

// 0x4425F8
static void about_loop()
{
    if (about_init() != 0) {
        return;
    }

    beginTextInput();

    while (1) {
        sharedFpsLimiter.mark();

        // CE: B / BACK cancels the ask-about window (it's otherwise keyboard text entry).
        int input = get_input();
        if (input == -1) {
            input = gdialog_gamepad_cancel_key();
        }

        if (about_process_input(input) == -1) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    endTextInput();

    about_exit();
    strcpy(dialogBlock.replyText, about_restore_string);
    dialogue_switch_mode = 0;
    talk_to_create_dialogue_win();
    gdialog_unhide();
    gDialogProcessReply();
}

// 0x44267C
static int about_process_input(int input)
{
    if (about_win == -1) {
        return -1;
    }

    switch (input) {
    case KEY_BACKSPACE:
        if (about_input_index > 0) {
            about_input_index--;
            about_input_string[about_input_index] = about_input_cursor;
            about_input_string[about_input_index + 1] = '\0';
            about_update_display(1);
        }
        break;
    case KEY_RETURN:
        about_process_string();
        break;
    case KEY_ESCAPE:
        if (gdialog_speech_playing == 1) {
            if (soundPlaying(lip_info.sound)) {
                gdialog_free_speech();
            }
        }
        return -1;
    default:
        if (input >= 0 && about_input_index < 126) {
            text_font(101);
            about_input_string[about_input_index] = '_';

            if (text_width(about_input_string) + text_char_width(input) < 244) {
                about_input_string[about_input_index] = input;
                about_input_string[about_input_index + 1] = about_input_cursor;
                about_input_string[about_input_index + 2] = '\0';
                about_input_index++;
                about_update_display(1);
            }

            about_input_string[about_input_index] = about_input_cursor;
        }
        break;
    }

    if (elapsed_time(about_last_time) > 333) {
        if (about_input_cursor == '_') {
            about_input_cursor = ' ';
        } else {
            about_input_cursor = '_';
        }
        about_input_string[about_input_index] = about_input_cursor;
        about_update_display(1);
        about_last_time = get_time();
    }

    return 0;
}

// 0x442800
static void about_update_display(unsigned char should_redraw)
{
    int old_font;
    int width;
    int skip = 0;

    old_font = text_curr();
    about_clear_display(0);
    text_font(101);

    width = text_width(about_input_string) - 244;
    while (width > 0) {
        width -= text_char_width(about_input_string[skip++]);
    }

    text_to_buf(about_win_buf + about_win_width * 32 + 22,
        about_input_string + skip,
        244,
        about_win_width,
        colorTable[992]);

    if (should_redraw) {
        win_draw_rect(about_win, &about_input_rect);
    }

    text_font(old_font);
}

// 0x4428C8
static void about_clear_display(unsigned char should_redraw)
{
    win_fill(about_win, 22, 32, 244, 14, colorTable[0]);

    if (should_redraw) {
        win_draw_rect(about_win, &about_input_rect);
    }
}

// 0x442910
static void about_reset_string()
{
    about_input_index = 0;
    about_input_string[0] = about_input_cursor;
    about_input_string[1] = '\0';
}

// 0x44292C
static void about_process_string()
{
    static const char* delimeters = " \t.,";
    int found = 0;
    char* tok;
    Script* scr;
    int count;
    int message_id;
    char* str;
    int random_msg_num;

    about_input_string[about_input_index] = '\0';

    if (about_input_string[0] != '\0') {
        tok = strtok(about_input_string, delimeters);
        while (tok != NULL) {
            if (about_lookup_word(tok) || about_lookup_name(tok)) {
                found = 1;
                break;
            }
            tok = strtok(NULL, delimeters);
        }

        if (!found) {
            if (scr_ptr(dialog_target->sid, &scr) != -1) {
                count = 0;
                for (message_id = 980; message_id < 1000; message_id++) {
                    str = scr_get_msg_str(scr->scr_script_idx + 1, message_id);
                    if (str != NULL && compat_stricmp(str, "error") != 0) {
                        count++;
                    }
                }

                if (count != 0) {
                    random_msg_num = roll_random(1, count);
                    for (message_id = 980; message_id < 1000; message_id++) {
                        str = scr_get_msg_str(scr->scr_script_idx + 1, message_id);
                        if (str != NULL && compat_stricmp(str, "error") != 0) {
                            random_msg_num--;
                            if (random_msg_num == 0) {
                                strncpy(dialogBlock.replyText, scr_get_msg_str_speech(scr->scr_script_idx + 1, message_id, 1), sizeof(dialogBlock.replyText) - 1);
                                *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
                                gdialog_unhide_reply();
                                gDialogProcessReply();
                            }
                        }
                    }
                } else {
                    for (message_id = 980; message_id < 1000; message_id++) {
                        str = scr_get_msg_str(1, message_id);
                        if (str != NULL && compat_stricmp(str, "error") != 0) {
                            count++;
                        }
                    }

                    if (count != 0) {
                        random_msg_num = roll_random(1, count);
                        for (message_id = 980; message_id < 1000; message_id++) {
                            str = scr_get_msg_str(1, message_id);
                            if (str != NULL && compat_stricmp(str, "error") != 0) {
                                random_msg_num--;
                                if (random_msg_num == 0) {
                                    strncpy(dialogBlock.replyText, scr_get_msg_str_speech(1, message_id, 1), sizeof(dialogBlock.replyText) - 1);
                                    *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
                                    gdialog_unhide_reply();
                                    gDialogProcessReply();
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    about_reset_string();
    about_update_display(1);
}

// 0x442B54
static int about_lookup_word(const char* search)
{
    Script* scr;
    int found = -1;
    int message_list_id;
    int message_id;
    char* str;

    if (scr_ptr(dialog_target->sid, &scr) != -1) {
        message_list_id = scr->scr_script_idx + 1;
        for (message_id = 1000; message_id < 1100; message_id++) {
            str = scr_get_msg_str(message_list_id, message_id);
            if (str != NULL && compat_stricmp(str, search) == 0) {
                found = message_id + 100;
                break;
            }
        }
    }

    if (found == -1) {
        message_list_id = 1;
        for (message_id = 600 * map_data.field_34 + 1000; message_id < 600 * map_data.field_34 + 1100; message_id++) {
            str = scr_get_msg_str(message_list_id, message_id);
            if (str != NULL && compat_stricmp(str, search) == 0) {
                found = message_id + 100;
                break;
            }
        }
    }

    if (found == -1) {
        return 0;
    }

    strncpy(dialogBlock.replyText, scr_get_msg_str_speech(message_list_id, found, 1), sizeof(dialogBlock.replyText) - 1);
    *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
    gdialog_unhide_reply();
    gDialogProcessReply();
    return 1;
}

// 0x442C58
static int about_lookup_name(const char* search)
{
    const char* name;
    Script* scr;
    int message_id;
    char* str;
    int count;
    int random_msg_num;

    if (PID_TYPE(dialog_target->pid) != OBJ_TYPE_CRITTER) {
        return 0;
    }

    name = critter_name(dialog_target);
    if (name == NULL) {
        return 0;
    }

    // NOTE: Implementation looks broken - why it checks against it's own name?
    // This defeats the purpose of looking up known names.
    if (compat_stricmp(search, name) != 0) {
        return 0;
    }

    if (scr_ptr(dialog_target->sid, &scr) == -1) {
        return 0;
    }

    count = 0;
    for (message_id = 970; message_id < 980; message_id++) {
        str = scr_get_msg_str(scr->scr_script_idx + 1, message_id);
        if (str != NULL && compat_stricmp(str, "error") != 0) {
            count++;
        }
    }

    if (count != 0) {
        random_msg_num = roll_random(1, count);
        for (message_id = 970; message_id < 980; message_id++) {
            str = scr_get_msg_str(scr->scr_script_idx + 1, message_id);
            if (str != NULL && compat_stricmp(str, "error") != 0) {
                random_msg_num--;
                if (random_msg_num == 0) {
                    strncpy(dialogBlock.replyText, scr_get_msg_str_speech(scr->scr_script_idx + 1, message_id, 1), sizeof(dialogBlock.replyText) - 1);
                    *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
                    gdialog_unhide_reply();
                    gDialogProcessReply();
                    return 1;
                }
            }
        }
    } else {
        for (message_id = 970; message_id < 980; message_id++) {
            str = scr_get_msg_str(1, message_id);
            if (str != NULL && compat_stricmp(str, "error") != 0) {
                count++;
            }
        }

        if (count != 0) {
            random_msg_num = roll_random(1, count);
            for (message_id = 970; message_id < 980; message_id++) {
                str = scr_get_msg_str(1, message_id);
                if (str != NULL && compat_stricmp(str, "error") != 0) {
                    random_msg_num--;
                    if (random_msg_num == 0) {
                        strncpy(dialogBlock.replyText, scr_get_msg_str_speech(1, message_id, 1), sizeof(dialogBlock.replyText) - 1);
                        *(dialogBlock.replyText + sizeof(dialogBlock.replyText) - 1) = '\0';
                        gdialog_unhide_reply();
                        gDialogProcessReply();
                        return 1;
                    }
                }
            }
        }
    }

    return 0;
}

} // namespace fallout
