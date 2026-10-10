#ifndef FALLOUT_GAME_MODS_PAGE_H_
#define FALLOUT_GAME_MODS_PAGE_H_

namespace fallout {

// The MODS screen: every lever THIS build added, in one place the player can actually reach.
//
// Deliberately separate from vanilla's PREFERENCES. Those are the 1997 options and their screen is
// a fixed table painted into its background art -- there is no free slot in it, and the headings are
// part of the picture rather than text we draw. More to the point, these settings are a different
// KIND of thing: they turn our additions on and off, and the honest default for every one of them is
// the game as it shipped. Keeping them apart says which is which.
//
// ⚠️ THE WHOLE POINT IS DISCOVERABILITY. Travel speed, the doorway magnet and the crowd slide all
// existed as cfg keys nobody would ever find -- the same trap that hid barter's "even out" and the
// split-stack picker for weeks. A lever with no screen is a lever that does not exist.
//
// Modal: runs its own input loop and returns when the player closes it.
void mods_page();

} // namespace fallout

#endif /* FALLOUT_GAME_MODS_PAGE_H_ */
