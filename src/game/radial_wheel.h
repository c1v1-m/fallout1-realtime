#ifndef FALLOUT_GAME_RADIAL_WHEEL_H_
#define FALLOUT_GAME_RADIAL_WHEEL_H_

namespace fallout {

// A generic gamepad radial (pie) menu overlay. Deliberately knows NOTHING about
// skills/items: the caller supplies labelled segments and reads back which one was
// aimed at. Reusable for skills now, and later for quick-use items, aimed-shot body
// parts, etc. -- only the segment list and the selection handling change.
//
// Driving model (no nested input loop, so no re-entrancy): the owner opens the wheel
// on a button press, feeds it the aim stick each frame, reads the highlight, and on
// release executes the highlighted entry then closes. ALL of these touch a GNW window,
// so they MUST be called from a safe frame-tick point (e.g. the main loop after
// get_input), NEVER from inside the mouse/input hook.

// One ring. A single ring cannot hold much more than eight item-sized cells before they collide, so
// callers with more entries than that PAGE through them (see the weapon wheel's LB/RB) rather than
// cramming the ring -- eight fixed positions stay learnable, a ring of twenty does not.
#define RADIAL_WHEEL_MAX 12

// ZERO-INITIALISE these before filling them in (`RadialSegment segs[8]; memset(...)` or
// `= {}`): the optional fields below are read on every draw, and a stack array left
// uninitialised feeds the wheel a garbage icon pointer.
struct RadialSegment {
    const char* label; // shown on the segment; not owned, must outlive the wheel
    int id;            // caller-defined, returned via radial_wheel_highlight_id()
    bool enabled;      // drawn greyed and never selectable when false
    int value;         // optional numeric badge (e.g. skill %); < 0 = none

    // Optional per-segment sprite -- a transparent FRM, e.g. an item's own inventory art.
    // NULL = none. When ANY segment carries one the wheel switches to the ICON look: the
    // icon (not the button frame) sizes the segment cell, each cell gets a box drawn in
    // its state colour, and the labels move to a single caption in the hub, because eight
    // item names spread around a ring overlap into mush. The wheel BLITS these -- the
    // caller owns them and must keep them alive for the wheel's lifetime.
    const unsigned char* icon;
    int iconWidth;
    int iconHeight;

    // Optional second line under the hub caption, shown only while THIS segment is aimed at:
    // what a drug does, a weapon's damage and ammo, and so on. NULL = none. Not owned, must
    // outlive the wheel. Only the icon look has a hub to draw it in.
    const char* detail;

    // Optional PROSE description (Fallout's own item text), shown word-wrapped in a band under the
    // ring while this segment is aimed at. NULL = none; if no segment has one the band is not
    // reserved at all and the panel keeps its square shape. Not owned, must outlive the wheel.
    const char* description;
};

// Optional art the caller supplies for a richer look (e.g. the skilldex button frame
// behind each segment, and a digit strip to render `value`). The wheel BLITS these but
// does NOT own them -- the caller keeps them alive for the wheel's lifetime. Any pointer
// may be NULL to fall back to the plain text look. Pass the whole struct as NULL to
// radial_wheel_open for no art at all.
struct RadialWheelArt {
    const unsigned char* buttonUp;   // per-segment backdrop, unpressed (transparent FRM)
    const unsigned char* buttonDown; // per-segment backdrop for the highlighted segment
    int buttonWidth;
    int buttonHeight;
    const unsigned char* digits;     // horizontal strip of glyphs '0'..'9' (digit d at x=d*digitWidth)
    int digitWidth;                  // one glyph cell width in the strip
    int digitHeight;
    int digitsPitch;                 // full pitch (row stride) of the digit strip
    const unsigned char* center;     // optional hub image (transparent FRM), e.g. the target's
                                     // body for an aimed-shot wheel; NULL = empty hub
    int centerWidth;
    int centerHeight;                // the ring is sized to clear this so it doesn't overlap labels
};

// Open a wheel of `count` segments (clamped to RADIAL_WHEEL_MAX). `title` is shown at the
// top (NULL = none). `art` may be NULL for the plain text look. The panel is centered on
// screen point (atX,atY); pass atX<0 || atY<0 to center it on the whole screen. It is
// always clamped fully on-screen. When `transparentBg` is true the panel background and
// border are left transparent (index 0) so the world shows through the hub -- e.g. an
// aimed-shot wheel placed over the highlighted enemy. No-op if already open. Safe-tick only.
void radial_wheel_open(const RadialSegment* segments, int count, const char* title,
    const RadialWheelArt* art, int atX, int atY, bool transparentBg);

// Feed the aim vector (e.g. right stick; x right, y DOWN, components ~ -1..1). Past the
// deadzone the segment nearest the aim angle is highlighted; below it nothing is (so a
// release with the stick centered cancels). Redraws only when the highlight changes.
void radial_wheel_update(float aimX, float aimY);

// Index of the highlighted segment, or -1 if none.
int radial_wheel_highlight();

// id of the highlighted segment if one is highlighted AND enabled, else -1. This is the
// value to act on at release time.
int radial_wheel_highlight_id();

// Destroy the overlay window. Safe-tick only. No-op if not open.
void radial_wheel_close();

bool radial_wheel_is_open();

} // namespace fallout

#endif /* FALLOUT_GAME_RADIAL_WHEEL_H_ */
