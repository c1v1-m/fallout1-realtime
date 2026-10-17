#ifndef FALLOUT_GAME_INTFACE_H_
#define FALLOUT_GAME_INTFACE_H_

#include "game/object_types.h"
#include "plib/db/db.h"

namespace fallout {

#define INTERFACE_BAR_WIDTH 640
#define INTERFACE_BAR_HEIGHT 100

typedef enum Hand {
    // Item1 (Punch)
    HAND_LEFT,
    // Item2 (Kick)
    HAND_RIGHT,
    HAND_COUNT,
} Hand;

typedef enum InterfaceItemAction {
    INTERFACE_ITEM_ACTION_DEFAULT = -1,
    INTERFACE_ITEM_ACTION_USE,
    INTERFACE_ITEM_ACTION_PRIMARY,
    INTERFACE_ITEM_ACTION_PRIMARY_AIMING,
    INTERFACE_ITEM_ACTION_SECONDARY,
    INTERFACE_ITEM_ACTION_SECONDARY_AIMING,
    INTERFACE_ITEM_ACTION_RELOAD,
    INTERFACE_ITEM_ACTION_COUNT,
} InterfaceItemAction;

extern int interfaceWindow;
extern int bar_window;

int intface_init();
void intface_reset();
void intface_exit();
int intface_load(DB_FILE* stream);
int intface_save(DB_FILE* stream);
void intface_hide();
void intface_show();
int intface_is_hidden();
void intface_enable();
void intface_disable();
bool intface_is_enabled();
void intface_redraw();
void intface_update_hit_points(bool animate);
// Frame-loop pump for the non-blocking hit-point counter that intface_update_hit_points(true)
// starts during realtime combat (the vanilla roll blocks the whole world for ~250ms). Called
// from combat_rt_update; a no-op when no roll is in flight.
void intface_hp_roll_tick(float dt);
// Abandons a roll in flight and snaps the counter to the dude's real hit points. Must be called
// whenever the value underneath it changes out from under the animation (load, map teardown).
void intface_hp_roll_cancel();
void intface_update_ac(bool animate);
void intface_update_move_points(int actionPoints, int bonusMove);

// Realtime heal slot (the RB stimpak indicator) -- see the block above intface_redraw.
void intface_update_heal_slot(Object* item, int count, bool armed = false);
void intface_invalidate_heal_slot();
// The LB quick slot, in the strip LEFT of the bar (gamepad_ui "QUICK SLOTS"); armed = the item is
// drawn and waiting for the release. No strip (a 640-wide screen) = nothing is drawn.
void intface_update_quick_slot_lb(Object* item, int count, bool armed);
// The part an aimed RT goes to, printed in the weapon slot beside its bullseye.
void intface_update_aim_part(int hitLocation);
// REALTIME: the end-turn box on the bar opens its combat curtains while a fight is live and closes
// them when it is over, frame-stepped so nothing waits for it. Only when the heal slot has moved out
// to the side panel (a screen wider than 640). Every frame from combat_rt_update.
void intface_rt_combat_box_tick(bool engaged, float dt);
void intface_rt_combat_box_reset();
void intface_rt_combat_box_invalidate();
// MODS "Combat box sound": the curtains open/close clunk and the lights chime. Default ON (vanilla).
bool intface_get_combat_box_sound();
void intface_set_combat_box_sound(bool on);
// Repairs the dude's weapon animation code when a swap animation failed to finish (realtime can
// cancel it). Cheap and idempotent; combat_rt calls it once a frame. See the note in intface.cc.
void intface_reconcile_weapon_fid();
int intface_get_attack(int* hitMode, bool* aiming);
int intface_update_items(bool animated);
int intface_toggle_items(bool animated);
// Reloads the active-hand weapon from inventory ammo (controller reload button).
int intface_reload_weapon();
// Dry run of the above: true when a reload would actually load something (see intface.cc).
bool intface_can_reload_weapon();
// The active hand's item, NULL for an empty hand -- what intface_reload_weapon would reload.
Object* intface_reload_target();
int intface_toggle_item_state();
// True when a toggle would reach a different firing mode. Realtime skips the reload entry (it has
// its own button), so a weapon with one mode and no called shot has nothing to cycle.
bool intface_item_can_toggle_state();
// Puts the active hand on its AIMED mode (the bullseye); false when this mode cannot be aimed.
bool intface_item_set_aiming();
void intface_use_item();
int intface_is_item_right_hand();
int intface_get_current_item(Object** itemPtr);
int intface_update_ammo_lights();
void intface_end_window_open(bool animated);
void intface_end_window_close(bool animated);
void intface_end_buttons_enable();
void intface_end_buttons_disable();
int refresh_box_bar_win();
bool enable_box_bar_win();
bool disable_box_bar_win();

} // namespace fallout

#endif /* FALLOUT_GAME_INTFACE_H_ */
