#ifndef FALLOUT_GAME_INTERACT_PROMPT_H_
#define FALLOUT_GAME_INTERACT_PROMPT_H_

#include "game/object_types.h"

namespace fallout {

// Contextual "use" prompt for the gamepad. Out of combat, the nearest interactable object near the
// dude (a usable door/scenery, an item on the ground, a container, a corpse, or a talkable NPC) is
// highlighted and a floating "hand" icon pops up over it; pressing A performs the interaction (the
// engine walks the dude there and uses/gets/talks/loots). Mirrors the lock-on model but for the
// world instead of enemies.

// Per-frame driver -- call from a SAFE main-loop context (gamepad_ui_tick). Scans, updates the
// highlight + floating hand overlay, and performs any interaction queued by interact_prompt_request().
void interact_prompt_tick();

// From the gamepad A handler: if a prompt target is active, queue the interaction on it (performed
// from the tick, never from the input hook) and return true so the caller suppresses the walk-click.
// No target -> false (A falls through to its normal behaviour).
bool interact_prompt_request();

// The object currently prompted, or NULL. Lets the A arbitration know a world interaction is offered.
Object* interact_prompt_target();

// Step the action A will perform on the current target (+1 next, -1 previous) and repaint the
// floating icon to match. Returns false when there is no target or it has only one action, so the
// caller can refuse audibly instead of eating the press. Safe from the input hook: it only moves an
// index and redraws the overlay window.
bool interact_prompt_cycle(int delta);

// How many interactables are in reach. The right-stick picker only takes the stick when this is 2 or
// more -- with a single candidate there is nothing to choose and the stick must stay a cursor.
int interact_prompt_candidate_count();

// Move the prompt to the interactable lying in screen direction (dirX, dirY) from the CURRENT one
// (a unit vector, y positive down). Returns false when nothing lies that way, so the caller can
// refuse audibly. The choice overrides the automatic nearest-target pick until it goes out of reach,
// stops being interactable, or times out.
bool interact_prompt_pick_direction(float dirX, float dirY);

// True only when the current prompt target is a CORPSE (the one interaction the post-kill loot
// cooldown gates). A door/scenery/item/talk target returns false so A performs it immediately even
// while a distant enemy keeps combat "warm".
bool interact_prompt_offering_corpse();

// True when the dude is standing RIGHT at the prompt target (adjacent). The A router uses this to
// let a deliberate interaction (a door/corpse you walked up to) override the attack mid-combat,
// while a merely-nearby interactable does NOT steal A from firing.
bool interact_prompt_target_adjacent();

// Drop the overlay + all cached pointers (map teardown, entering combat, shutdown). Idempotent.
void interact_prompt_reset();

} // namespace fallout

#endif /* FALLOUT_GAME_INTERACT_PROMPT_H_ */
