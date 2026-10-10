#include "game/radial_wheel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define RADIAL_WHEEL_PI 3.14159265358979323846f

// A firm push is required to select; a centered / barely-pushed stick highlights
// nothing, so releasing there cancels.
#define RADIAL_WHEEL_DEADZONE 0.4f

// Description band: how many lines of wrapped prose it holds, and the left/right margin. Four lines
// covers most Fallout item descriptions; anything longer is cut at a word boundary.
#define RADIAL_WHEEL_DESC_LINES 4
#define RADIAL_WHEEL_DESC_PAD 8

// How much wider than tall the ring panel is allowed to get. The screen has far more width than
// height to spare, and spreading the segments sideways is the only way to keep eight item icons
// from touching once the description band has taken its share of the height.
#define RADIAL_WHEEL_ASPECT 1.5f

// Square overlay panel size for the plain text look (no art). The art look sizes
// itself from the supplied button frame instead. Kept compact so an opaque panel
// anchored at the screen edge leaves most of the battlefield visible.
#define RADIAL_WHEEL_SIZE 190

static int wheel_win = -1;
static RadialSegment wheel_segs[RADIAL_WHEEL_MAX];
static int wheel_count = 0;
static int wheel_highlight = -1;

// ⚠️ THE AIM STICK MUST PASS THROUGH CENTRE BEFORE IT CAN SELECT ANYTHING.
//
// Three of the wheels (quick-use on B, weapon on Y, ammo on X) are aimed with the LEFT stick,
// which is also the WALK stick -- and a player who presses a face button mid-fight is, almost by
// definition, already holding a direction. The wheel then opened with the stick ALREADY deflected,
// the very first radial_wheel_update highlighted whatever segment the running direction pointed at,
// and the release picked it. Nobody ever aimed at it.
//
// That is the "reloading was weird, it loaded the wrong ammo" report, and the trace has it twice in
// two seconds (trace_2026-09-07_233432, s=138.2 and s=139.3): X down while walking -> wheel up
// 220 ms later -> release -> UNLOAD fired, dumping a full 30-round magazine back into the pack
// (gun 30/30 -> 0/30, pack 126 -> 156 rounds). Both times the [INPUT/stick] event reappears ~20 ms
// after the wheel closes, because combat_rt stops reading the stick while a wheel is up -- so the
// deflection was held right through the wheel and never showed in the log.
//
// So a wheel opens DISARMED: deflection is ignored until the stick has come back inside the
// deadzone once. Deliberately aiming costs nothing (the thumb returns to centre on the way to the
// segment you want anyway); carrying a walk direction into the wheel now selects nothing.
static bool wheel_aim_armed = false;
static int wheel_w = 0;
static int wheel_h = 0;
// The ring is an ELLIPSE, not a circle. The panel's height is the scarce budget -- an internal
// resolution of 853x480 leaves barely 420 px of height once the description band is reserved, and a
// circle inside that puts eight item icons almost shoulder to shoulder. Width is plentiful, so the
// ring is stretched sideways into it: same angles, same hit test, just more room between segments.
static int wheel_radius_x = 0;
static int wheel_radius_y = 0;
// Height reserved at the BOTTOM of the panel for the aimed segment's description, or 0 when no
// segment carries one. Fallout's item descriptions are proper prose -- two or three sentences --
// which is exactly why they are worth showing and exactly why they cannot go in the hub: the middle
// of the ring is maybe 25 characters wide before the text runs under the icons.
static int wheel_desc_h = 0;

// One wrapped line of the description, CENTRED. Centring each line rather than left-aligning the
// block keeps the text visually attached to the wheel it belongs to -- a left-aligned paragraph
// under a circle reads as a separate panel that happens to be nearby.
static void wheel_draw_desc_line(unsigned char* buf, const char* line, int y)
{
    int pitch = wheel_w;
    int x = (wheel_w - text_width(line)) / 2;
    if (x < RADIAL_WHEEL_DESC_PAD) {
        x = RADIAL_WHEEL_DESC_PAD;
    }
    text_to_buf(buf + y * pitch + x, line, pitch, pitch, colorTable[992]);
}

// Word-wrap `text` into the description band. Greedy wrap on spaces, and it stops rather than
// overflowing -- a description longer than the band is cut at a word boundary, which reads as an
// abbreviation instead of as corruption.
static void wheel_draw_description(unsigned char* buf, const char* text, int top)
{
    if (text == NULL || text[0] == '\0' || wheel_desc_h <= 0) {
        return;
    }

    int pitch = wheel_w;
    int lineH = text_height();
    int maxWidth = wheel_w - 2 * RADIAL_WHEEL_DESC_PAD;
    int y = top;

    char line[128];  // the line being filled
    char probe[128]; // that line plus the next word, to measure before committing
    int lineLen = 0;
    const char* p = text;

    while (*p != '\0') {
        if (y + lineH > top + wheel_desc_h) {
            return; // band full
        }

        // Next word.
        const char* word = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        int wordLen = (int)(p - word);
        while (*p == ' ') {
            p++;
        }
        if (wordLen <= 0) {
            continue;
        }

        // Every write below is bounded by these two guards, not by trusting the source string:
        // descriptions come from the game's message files and a long one must clip, never scribble.
        if (wordLen > (int)sizeof(line) - 1) {
            wordLen = (int)sizeof(line) - 1;
        }
        if (lineLen + 1 + wordLen > (int)sizeof(probe) - 1) {
            break;
        }

        int probeLen = 0;
        if (lineLen > 0) {
            memcpy(probe, line, lineLen);
            probeLen = lineLen;
            probe[probeLen++] = ' ';
        }
        memcpy(probe + probeLen, word, wordLen);
        probeLen += wordLen;
        probe[probeLen] = '\0';

        if (lineLen == 0 || text_width(probe) <= maxWidth) {
            memcpy(line, probe, probeLen + 1);
            lineLen = probeLen;
            continue;
        }

        // Doesn't fit: flush the line and start the next one with this word.
        line[lineLen] = '\0';
        wheel_draw_desc_line(buf, line, y);
        y += lineH;
        memcpy(line, word, wordLen);
        lineLen = wordLen;
    }

    if (lineLen > 0 && y + lineH <= top + wheel_desc_h) {
        line[lineLen] = '\0';
        wheel_draw_desc_line(buf, line, y);
    }
}
static int wheel_seg_h = 0;    // vertical footprint of one segment (cell + digits)
static int wheel_cell_w = 0;   // one segment cell: the button frame, or the icon when bigger
static int wheel_cell_h = 0;
static bool wheel_has_icons = false; // any segment carries per-segment art -> icon look
static RadialWheelArt wheel_art;
static bool wheel_has_art = false;
static char wheel_title[32];

// Control hints pinned to the panel's TOP CORNERS -- as far left and as far right as the panel
// goes. NOT the middle of those edges: with eight segments the ring puts a cell at 3 and 9
// o'clock whose icon runs to within ~10 px of the border, so text there would land on a weapon.
// And not in the hub either: stacking a third line under the caption and the detail line was too
// much text in one column ("hodně textu pod sebou").
static char wheel_hint_left[16];
static char wheel_hint_right[16];

static bool wheel_transparent = false;
static bool wheel_buffering_on = false; // we toggled GNW buffering on for a transparent panel

// Transparent, fully-CLIPPED blit of a source sprite into the wheel buffer at (dx,dy).
// Index 0 is transparent. Clipping to the buffer bounds is deliberate: the layout math
// should keep everything inside, but clipping guarantees we can never write out of the
// allocation (the original buf_outline border bug taught us that lesson the hard way).
static void wheel_blit_trans(unsigned char* buf, const unsigned char* src, int sw, int sh, int spitch, int dx, int dy)
{
    for (int y = 0; y < sh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= wheel_h) {
            continue;
        }
        const unsigned char* srow = src + y * spitch;
        unsigned char* drow = buf + ty * wheel_w;
        for (int x = 0; x < sw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= wheel_w) {
                continue;
            }
            unsigned char p = srow[x];
            if (p != 0) {
                drow[tx] = p;
            }
        }
    }
}

// Render `value` (e.g. a skill %) as engine big-number glyphs, centered horizontally on
// centerX with its top at topY. No-op without a digit strip or for a negative value.
static void wheel_draw_value(unsigned char* buf, int value, int centerX, int topY)
{
    if (!wheel_has_art || wheel_art.digits == NULL || value < 0) {
        return;
    }

    if (value > 999) {
        value = 999;
    }

    int digits[3];
    int n = 0;
    if (value >= 100) {
        digits[n++] = value / 100;
    }
    if (value >= 10) {
        digits[n++] = (value / 10) % 10;
    }
    digits[n++] = value % 10;

    // ⚠️ KEEP THE WHOLE NUMBER ON THE PANEL. The segments sit on an ellipse, so the left and right
    // ones are centred close to the panel edge -- and wheel_blit_trans clips per pixel against the
    // panel, silently. A two-digit badge fitted; a three-digit one centred on the same point hung
    // over the edge and lost its outer digit, so "123 rounds" was drawn and read as two digits.
    // Slide it inward instead: an off-centre number is still the right number.
    int span = n * wheel_art.digitWidth;
    int x = centerX - span / 2;
    if (x < 0) {
        x = 0;
    }
    if (x + span > wheel_w) {
        x = wheel_w - span;
    }

    for (int i = 0; i < n; i++) {
        wheel_blit_trans(buf,
            wheel_art.digits + digits[i] * wheel_art.digitWidth,
            wheel_art.digitWidth,
            wheel_art.digitHeight,
            wheel_art.digitsPitch,
            x,
            topY);
        x += wheel_art.digitWidth;
    }
}

// Draw one label string centered at (centerX, centerY), clamped inside the panel.
static void wheel_draw_label(unsigned char* buf, const char* label, int centerX, int centerY, int color)
{
    if (label == NULL) {
        return;
    }

    int pitch = wheel_w;
    int lineHeight = text_height();
    int labelWidth = text_width(label);

    int tx = centerX - labelWidth / 2;
    int ty = centerY - lineHeight / 2;
    if (tx < 2) {
        tx = 2;
    } else if (tx + labelWidth > wheel_w - 2) {
        tx = wheel_w - 2 - labelWidth;
    }
    if (ty < 2) {
        ty = 2;
    } else if (ty + lineHeight > wheel_h - 2) {
        ty = wheel_h - 2 - lineHeight;
    }
    text_to_buf(buf + ty * pitch + tx, label, pitch, pitch, color);
}

// Repaint the whole panel: black backdrop, green border, centered title, and the
// segments arranged clockwise from the top. Each segment is either the plain text look
// or (with art) a skilldex button frame behind the label plus the skill level in big
// numbers. Cheap; called only on highlight change.
static void wheel_draw()
{
    if (wheel_win == -1) {
        return;
    }

    unsigned char* buf = win_get_buf(wheel_win);
    if (buf == NULL) {
        return;
    }

    int pitch = wheel_w;
    int cx = wheel_w / 2;
    // The RING is centred in the square part of the panel, NOT in the window: when a description
    // band is reserved at the bottom the window is taller than it is wide, and centring on the
    // window would push the ring down over the text.
    int cy = (wheel_h - wheel_desc_h) / 2;

    // Transparent mode fills with index 0 (the window blits transparently) so the world --
    // e.g. the green-highlighted enemy under an aimed-shot wheel -- shows through; opaque
    // mode fills black for a readable panel.
    buf_fill(buf, wheel_w, wheel_h, pitch, wheel_transparent ? 0 : colorTable[0]);
    if (!wheel_transparent) {
        // A plain rectangle border. NOT buf_outline -- that traces the edge of an opaque
        // shape on a 0 background and writes out of bounds on a solid fill (heap corruption).
        draw_box(buf, pitch, 0, 0, wheel_w - 1, wheel_h - 1, colorTable[992]);
    }

    // The icon look has no room for a title strip: its cells are much taller than a button
    // frame (an 88x59 item FRM against 86x33), which pushes the top segment up to y=12 --
    // straight under the title. That layout puts the title in the empty hub instead (see
    // the end of this function), where the highlighted segment's name replaces it.
    if (wheel_title[0] != '\0' && !wheel_has_icons) {
        int titleWidth = text_width(wheel_title);
        text_to_buf(buf + 6 * pitch + (cx - titleWidth / 2), wheel_title, pitch, pitch, colorTable[992]);
    }

    // Hub image (e.g. the target's body for an aimed-shot wheel), centered. Drawn before
    // the segments so the needle/labels sit on top.
    if (wheel_has_art && wheel_art.center != NULL) {
        wheel_blit_trans(buf, wheel_art.center, wheel_art.centerWidth, wheel_art.centerHeight,
            wheel_art.centerWidth, cx - wheel_art.centerWidth / 2, cy - wheel_art.centerHeight / 2);
    }

    for (int i = 0; i < wheel_count; i++) {
        float angle = -RADIAL_WHEEL_PI / 2.0f + i * (2.0f * RADIAL_WHEEL_PI / wheel_count);
        int px = cx + (int)(wheel_radius_x * cosf(angle));
        int py = cy + (int)(wheel_radius_y * sinf(angle));

        int color;
        if (!wheel_segs[i].enabled) {
            color = colorTable[31744]; // red: blocked (e.g. skill unusable in combat)
        } else if (i == wheel_highlight) {
            color = colorTable[32767]; // white: aimed
        } else {
            color = colorTable[992]; // green: available
        }

        // Needle from the hub to the aimed segment, in both looks.
        if (i == wheel_highlight && wheel_segs[i].enabled) {
            draw_line(buf, pitch, cx, cy, px, py, colorTable[32767]);
        }

        if (wheel_has_art && (wheel_art.buttonUp != NULL || wheel_segs[i].icon != NULL)) {
            int cw = wheel_cell_w;
            int ch = wheel_cell_h;
            int top = py - wheel_seg_h / 2;
            int left = px - cw / 2;

            if (wheel_art.buttonUp != NULL) {
                // Pressed frame on the highlighted segment, unpressed otherwise. Centered in
                // the cell, which an icon may have made larger than the frame itself.
                int bw = wheel_art.buttonWidth;
                int bh = wheel_art.buttonHeight;
                const unsigned char* btn = (i == wheel_highlight && wheel_segs[i].enabled && wheel_art.buttonDown != NULL)
                    ? wheel_art.buttonDown
                    : wheel_art.buttonUp;
                wheel_blit_trans(buf, btn, bw, bh, bw, px - bw / 2, top + (ch - bh) / 2);
            }

            if (wheel_segs[i].icon != NULL) {
                // The item's own inventory art IS the segment. A box in the segment colour
                // carries the available / aimed / out-of-stock state that the pressed and
                // unpressed button frames carry in the text look.
                int iw = wheel_segs[i].iconWidth;
                int ih = wheel_segs[i].iconHeight;
                wheel_blit_trans(buf, wheel_segs[i].icon, iw, ih, iw, px - iw / 2, top + (ch - ih) / 2);

                // Box ONLY when it says something: WHITE on the segment you are aiming at, RED on
                // one you cannot pick. The plain green "this is available" box used to be drawn on
                // every cell at once, which is a ring of boxes stating what the ring already
                // states -- so it read as clutter around the art instead of as information. The
                // icons are the segments; the box is now purely the state marker.
                if (!wheel_segs[i].enabled || i == wheel_highlight) {
                    draw_box(buf, pitch, left - 2, top - 2, left + cw + 1, top + ch + 1, color);
                }
            } else {
                // Label centered on the button.
                wheel_draw_label(buf, wheel_segs[i].label, px, top + ch / 2, color);
            }

            // Level / count digits just below the cell.
            wheel_draw_value(buf, wheel_segs[i].value, px, top + ch + 1);
        } else {
            // Plain text look: label plus the to-hit / level value as "Name NN%".
            if (wheel_segs[i].value >= 0) {
                char line[40];
                snprintf(line, sizeof(line), "%s %d%%", wheel_segs[i].label, wheel_segs[i].value);
                wheel_draw_label(buf, line, px, py, color);
            } else {
                wheel_draw_label(buf, wheel_segs[i].label, px, py, color);
            }
        }
    }

    // Icon look only: the per-segment labels were dropped, so the hub carries the text -- the
    // middle of the ring is empty and it is where the eye already is. Aiming at a segment
    // names it; aiming at nothing falls back to the wheel's title, which the icon look has no
    // room for up top. Drawn last so it sits over the needle that runs out of the hub.
    if (wheel_has_icons) {
        if (wheel_highlight >= 0 && wheel_highlight < wheel_count) {
            const RadialSegment* seg = &wheel_segs[wheel_highlight];
            int nameColor = seg->enabled ? colorTable[32767] : colorTable[31744];

            // With a detail line the pair is centred as a block: the name lifts by half a line so
            // the two together still sit on the hub's middle instead of drifting downward.
            int lineH = text_height();
            int nameY = seg->detail != NULL ? cy - lineH / 2 : cy;
            wheel_draw_label(buf, seg->label, cx, nameY, nameColor);

            if (seg->detail != NULL) {
                // Green, not white: it is supporting information, and the name has to stay the
                // thing the eye lands on when you are picking under fire.
                wheel_draw_label(buf, seg->detail, cx, nameY + lineH, colorTable[992]);
            }
        } else if (wheel_title[0] != '\0') {
            wheel_draw_label(buf, wheel_title, cx, cy, colorTable[992]);
        }
    }

    // Corner hints last, so no segment can overdraw them.
    if (wheel_hint_left[0] != '\0') {
        text_to_buf(buf + 3 * pitch + 4, wheel_hint_left, pitch, pitch, colorTable[992]);
    }
    if (wheel_hint_right[0] != '\0') {
        int hintWidth = text_width(wheel_hint_right);
        int hx = wheel_w - 4 - hintWidth;
        if (hx < 4) {
            hx = 4;
        }
        text_to_buf(buf + 3 * pitch + hx, wheel_hint_right, pitch, pitch, colorTable[992]);
    }

    // Fallout's own item description for whatever is aimed at, in its own band under the ring.
    if (wheel_desc_h > 0 && wheel_highlight >= 0 && wheel_highlight < wheel_count) {
        wheel_draw_description(buf, wheel_segs[wheel_highlight].description,
            wheel_h - wheel_desc_h + 3);
    }

    win_draw(wheel_win);
}

void radial_wheel_set_side_hints(const char* left, const char* right)
{
    if (left != NULL) {
        strncpy(wheel_hint_left, left, sizeof(wheel_hint_left) - 1);
        wheel_hint_left[sizeof(wheel_hint_left) - 1] = '\0';
    } else {
        wheel_hint_left[0] = '\0';
    }

    if (right != NULL) {
        strncpy(wheel_hint_right, right, sizeof(wheel_hint_right) - 1);
        wheel_hint_right[sizeof(wheel_hint_right) - 1] = '\0';
    } else {
        wheel_hint_right[0] = '\0';
    }

    // The wheel repaints only on a highlight change, so hints set right after opening would
    // otherwise stay invisible until the first stick sweep.
    if (wheel_win != -1) {
        wheel_draw();
    }
}

void radial_wheel_open(const RadialSegment* segments, int count, const char* title,
    const RadialWheelArt* art, int atX, int atY, bool transparentBg)
{
    if (wheel_win != -1) {
        return;
    }
    if (segments == NULL || count <= 0) {
        return;
    }
    if (count > RADIAL_WHEEL_MAX) {
        count = RADIAL_WHEEL_MAX;
    }

    wheel_transparent = transparentBg;

    // True see-through needs GNW double-buffering: WINDOW_TRANSPARENT is ignored when the
    // engine runs un-buffered (gnw.cc:1009), which it does the whole game (win_buffering is
    // never called otherwise). screen_buffer is allocated at init, so enable buffering just
    // for the lifetime of a transparent wheel and restore on close.
    if (wheel_transparent) {
        win_buffering(true);
        wheel_buffering_on = true;
    }

    memcpy(wheel_segs, segments, count * sizeof(RadialSegment));
    wheel_count = count;
    wheel_highlight = -1;
    wheel_aim_armed = false; // a walk direction carried in from gameplay must not select

    if (title != NULL) {
        strncpy(wheel_title, title, sizeof(wheel_title) - 1);
        wheel_title[sizeof(wheel_title) - 1] = '\0';
    } else {
        wheel_title[0] = '\0';
    }

    // Never inherited from the previous wheel -- the caller sets them after opening, if at all.
    wheel_hint_left[0] = '\0';
    wheel_hint_right[0] = '\0';

    // Per-segment art switches the wheel to the icon look and, being typically larger than
    // a button frame (an FO1 inventory FRM is 88x59 against the skilldex button's 86x33),
    // takes over sizing the cell.
    int iconMaxW = 0;
    int iconMaxH = 0;
    for (int i = 0; i < wheel_count; i++) {
        if (wheel_segs[i].icon != NULL) {
            if (wheel_segs[i].iconWidth > iconMaxW) {
                iconMaxW = wheel_segs[i].iconWidth;
            }
            if (wheel_segs[i].iconHeight > iconMaxH) {
                iconMaxH = wheel_segs[i].iconHeight;
            }
        }
    }
    wheel_has_icons = (iconMaxW > 0 && iconMaxH > 0);

    wheel_has_art = (art != NULL && (art->buttonUp != NULL || wheel_has_icons));
    if (wheel_has_art) {
        wheel_art = *art;

        int bw = (art->buttonUp != NULL) ? art->buttonWidth : 0;
        int bh = (art->buttonUp != NULL) ? art->buttonHeight : 0;
        if (iconMaxW > bw) {
            bw = iconMaxW;
        }
        if (iconMaxH > bh) {
            bh = iconMaxH;
        }
        wheel_cell_w = bw;
        wheel_cell_h = bh;
        // Each segment's vertical footprint is the cell plus (if present) the level
        // digits drawn underneath it.
        int digitsH = (art->digits != NULL) ? art->digitHeight + 2 : 0;
        wheel_seg_h = bh + digitsH;
        int segMax = (bw > wheel_seg_h) ? bw : wheel_seg_h;

        // Spread the ring so adjacent wide buttons don't collide, then size the panel so
        // a full segment footprint fits at every ring position.
        int ring = (int)(bw * 1.4f);
        if (ring < 110) {
            ring = 110;
        }

        // If a hub image is present (e.g. the target's body), push the ring out so the
        // segments clear the image's bounding circle.
        if (art->center != NULL && art->centerWidth > 0 && art->centerHeight > 0) {
            int halfDiag = (int)(0.5f * sqrtf((float)(art->centerWidth * art->centerWidth
                + art->centerHeight * art->centerHeight)));
            int needRing = halfDiag + segMax / 2 + 6;
            if (ring < needRing) {
                ring = needRing;
            }
        }

        int half = ring + segMax / 2 + 12;
        int size = 2 * half;

        int screenW = screenGetWidth();
        int screenH = screenGetHeight();
        // ⚠️ THE DESCRIPTION BAND IS MEASURED BEFORE THE CLAMP, NOT ADDED AFTER IT. Adding it
        // afterwards pushed the panel past the screen height (464 square + a 46 px band against a
        // 480 px internal resolution), win_add refused the oversized window, and the wheel simply
        // never appeared -- "nejde mi quick wheel pro zbraně". The band is part of the panel, so it
        // has to be inside the budget the clamp is enforcing.
        bool anyDesc = false;
        for (int i = 0; i < wheel_count; i++) {
            if (wheel_segs[i].description != NULL && wheel_segs[i].description[0] != '\0') {
                anyDesc = true;
                break;
            }
        }
        wheel_desc_h = anyDesc ? RADIAL_WHEEL_DESC_LINES * text_height() + 6 : 0;

        int maxSize = (screenH < screenW ? screenH : screenW) - 16 - wheel_desc_h;
        if (maxSize < 80) {
            maxSize = 80; // pathological resolution: keep a ring rather than none
        }
        if (size > maxSize) {
            size = maxSize;
        }
        // Stretch the panel sideways into the spare width. The height stays the binding constraint;
        // the extra width is what actually separates the icons.
        int wide = (int)(size * RADIAL_WHEEL_ASPECT);
        int maxWide = screenW - 16;
        if (wide > maxWide) {
            wide = maxWide;
        }
        if (wide < size) {
            wide = size; // never narrower than tall
        }

        wheel_w = wide;
        wheel_h = size + wheel_desc_h;

        wheel_radius_x = wheel_w / 2 - segMax / 2 - 10;
        wheel_radius_y = size / 2 - segMax / 2 - 10;
        if (wheel_radius_x < 40) {
            wheel_radius_x = 40;
        }
        if (wheel_radius_y < 40) {
            wheel_radius_y = 40;
        }
    } else {
        wheel_seg_h = 0;
        wheel_cell_w = 0;
        wheel_cell_h = 0;
        wheel_has_icons = false;
        wheel_w = wheel_h = RADIAL_WHEEL_SIZE;
        wheel_radius_x = wheel_radius_y = wheel_w / 2 - 36;
    }

    // Center on the requested screen point, or on the whole screen when none is given.
    // win_move (inside win_add) clamps the panel fully on-screen, so an edge target just
    // nudges the panel inward.
    int x;
    int y;
    if (atX >= 0 && atY >= 0) {
        x = atX - wheel_w / 2;
        y = atY - wheel_h / 2;
    } else {
        x = (screenGetWidth() - wheel_w) / 2;
        y = (screenGetHeight() - wheel_h) / 2;
    }

    // Draw on top of the world without registering as a modal (we drive it ourselves
    // each frame; a modal would also flip gamepad_ui_menu_active and change A/stick
    // handling under us).
    // WINDOW_TRANSPARENT makes index-0 pixels see-through (GNW trans_buf_to_buf blit), so a
    // transparent wheel floats its labels/figure over the live world instead of a black block.
    wheel_win = win_add(x, y, wheel_w, wheel_h, colorTable[0],
        WINDOW_MOVE_ON_TOP | (wheel_transparent ? WINDOW_TRANSPARENT : 0));
    if (wheel_win == -1) {
        wheel_count = 0;
        wheel_desc_h = 0;
        wheel_has_art = false;
        return;
    }

    wheel_draw();
}

void radial_wheel_update(float aimX, float aimY)
{
    if (wheel_win == -1) {
        return;
    }

    int newHighlight = -1;
    float mag = sqrtf(aimX * aimX + aimY * aimY);

    // Not armed yet: the stick was still deflected from before the wheel opened (see
    // wheel_aim_armed). Nothing highlights until it passes through the centre once.
    if (!wheel_aim_armed) {
        if (mag >= RADIAL_WHEEL_DEADZONE) {
            return;
        }
        wheel_aim_armed = true;
    }

    if (mag >= RADIAL_WHEEL_DEADZONE) {
        // Angle measured from the top (12 o'clock), increasing clockwise. The stick's
        // y is down-positive, so negate it; atan2(x, -y) gives 0 at top, +pi/2 at right.
        float angle = atan2f(aimX, -aimY);
        if (angle < 0.0f) {
            angle += 2.0f * RADIAL_WHEEL_PI;
        }

        float segArc = 2.0f * RADIAL_WHEEL_PI / wheel_count;
        newHighlight = ((int)floorf(angle / segArc + 0.5f)) % wheel_count;
    }

    if (newHighlight != wheel_highlight) {
        wheel_highlight = newHighlight;
        wheel_draw();
    }
}

int radial_wheel_highlight()
{
    return wheel_win != -1 ? wheel_highlight : -1;
}

int radial_wheel_highlight_id()
{
    if (wheel_win == -1 || wheel_highlight < 0 || wheel_highlight >= wheel_count) {
        return -1;
    }
    if (!wheel_segs[wheel_highlight].enabled) {
        return -1;
    }
    return wheel_segs[wheel_highlight].id;
}

void radial_wheel_close()
{
    if (wheel_win == -1) {
        return;
    }
    win_delete(wheel_win);
    wheel_win = -1;
    wheel_count = 0;
    wheel_highlight = -1;
    wheel_aim_armed = false;
    wheel_has_art = false;

    // Restore the engine's normal un-buffered mode if we turned buffering on for see-through.
    if (wheel_buffering_on) {
        win_buffering(false);
        wheel_buffering_on = false;
    }
}

bool radial_wheel_is_open()
{
    return wheel_win != -1;
}

} // namespace fallout
