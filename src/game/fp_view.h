#ifndef FALLOUT_GAME_FP_VIEW_H_
#define FALLOUT_GAME_FP_VIEW_H_

namespace fallout {

// THE MAP IN 3D (2026-10-29, his "Ano, začni F1" and "dělej na 1 a 2"): the map the dude walks, with
// floors, roofs and walls built from the live map data and covered with the game's own pixels,
// critters standing as their own sprites (the paper3d prototype, ported). VIEW ONLY: the controls
// are the game's as ever. Two views:
//   - FP_VIEW_EYES: first person, through his eyes (F9 in a development build);
//   - FP_VIEW_DIABLO: the game's own camera through a perspective lens, as Diablo II's
//     "Perspective" option drew its 2D world (F8).
// FALLOUT_FP_VIEW=1 / =2 in the environment starts in one of them. See fp_view.cc.
enum {
    FP_VIEW_OFF,
    FP_VIEW_EYES,
    FP_VIEW_DIABLO,
};

void fp_view_init();
// The view's key: switches to `mode`, or back to the plain game when it is already on.
void fp_view_toggle(int mode);
int fp_view_mode();
// Through his eyes the stick steers the view as in Doom: x turns it, y walks along it. Rewrites x / y
// into the walk to make (screen terms, x right, y down; zero when it only turns) and returns true;
// false when the view is not his to steer, and x / y are untouched.
bool fp_view_steer(float* x, float* y, float dt);

// The 3D editor (Shift+F8, from Diablo's camera): while it is on the game stands still and every key
// and the mouse are the editor's (fp_view_edit_input, from the main loop).
bool fp_view_editing();
void fp_view_edit_toggle();
void fp_view_edit_input(int keyCode);

} // namespace fallout

#endif /* FALLOUT_GAME_FP_VIEW_H_ */
