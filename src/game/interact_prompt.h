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
bool interact_prompt_request_object(Object* obj);

// The object currently prompted, or NULL. Lets the A arbitration know a world interaction is offered.
Object* interact_prompt_target();

// Step the action A will perform on the current target (+1 next, -1 previous) and repaint the
// floating icon to match. Returns false when there is no target or it has only one action, so the
// caller can refuse audibly instead of eating the press. Safe from the input hook: it only moves an
// index and redraws the overlay window.
bool interact_prompt_cycle(int delta);
// The right stick is pointing (every frame it is out): the prompt's target wears the yellow
// outline until a moment after it comes home. See interact_prompt.cc.
void interact_prompt_note_stick();

// How many interactables are in reach. The right-stick picker only takes the stick when this is 2 or
// more -- with a single candidate there is nothing to choose and the stick must stay a cursor.
int interact_prompt_candidate_count();
void interact_prompt_trace_candidates();

// The nearest on-screen object the given SKILL would sensibly be aimed at, or NULL when nothing
// nearby fits. Lets the pad park an armed skill cursor on the obvious target (first aid on the
// dude, the lockpick on a locked chest) instead of leaving it wherever the pointer was abandoned.
Object* interact_prompt_skill_target(int skill);

// What an armed use-on / skill cursor may be stepped onto: the dude, any critter, and any item,
// scenery or wall that is interactable or scripted (never HIDDEN staged content). The list form
// returns the ones on screen within the picker's reach.
bool interact_prompt_is_use_target(Object* obj);
bool interact_prompt_object_interactable(Object* obj);
// The prompt's "nothing behind a wall" test: the straight line from the dude to `obj` crosses no
// wall or closed door. The loot window's nearby bodies use it too (inventry.cc).
bool interact_prompt_sees(Object* obj);
int interact_prompt_use_targets(Object** out, int max);

// Move the prompt to the interactable lying in screen direction (dirX, dirY) from the CURRENT one
// (a unit vector, y positive down). Returns false when nothing lies that way, so the caller can
// refuse audibly. The choice overrides the automatic nearest-target pick until it goes out of reach,
// stops being interactable, or times out.
// fresh = the first frame of a new flick (the stick was back under the threshold before): a flick
// the same way as the current selection then steps to the NEXT thing that way, so two objects in a
// line are both reachable.
bool interact_prompt_pick_direction(float dirX, float dirY, bool fresh);

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

// Drop any cached pointer to ONE object that is about to be freed (obj_erase_object).
// Cheaper and less destructive than interact_prompt_reset(), which is for whole-map teardown.
void interact_prompt_notify_object_freed(Object* obj);

} // namespace fallout

#endif /* FALLOUT_GAME_INTERACT_PROMPT_H_ */
