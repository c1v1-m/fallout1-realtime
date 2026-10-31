#include "game/fp_view.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>

#include "game/anim.h"
#include "game/art.h"
#include "game/combat_rt.h"
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/display.h"
#include "game/gmouse.h"
#include "game/gdialog.h"
#include "game/intface.h"
#include "game/light.h"
#include "game/map.h"
#include "game/object.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/radial_wheel.h"
#include "game/tile.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "platform_compat.h"
#include "plib/gnw/svga.h"

namespace fallout {

// FIRST-PERSON VIEW, step 1 (2026-10-29): the browser prototype's "3D jako Doom"
// (.claude/tools/paper3d, build.js --doom + geometry.js + viewer.html) ported into the engine.
//
// HOW IT DRAWS. renderPresent (svga.cc) uploads the 2D frame to one texture; at the MAIN LOOP's
// present (svga_fp_frame) this module renders the 3D view INTO that texture's map rows through an
// FBO, so the interface bar below stays the game's own, the picture keeps the internal resolution
// (blown up NEAREST like the 2D world) and the CRT filter, if on, filters it too. Any failure sets
// fp_failed and the 2D frame shows as always.
//
// WHAT IT DRAWS. The engine's projection is an orthographic view of a regular hex grid from
// sin(theta) = sqrt(3)/4: a point h units up that the game draws at screen (sx, sy) lives at
// W(sx, sy, h) = (sx / 32, h, (sy + KY h) / KZ). Every surface samples its sprite where the ORIGINAL
// camera sees it (projective texturing), so from the game's own angle it is the game's picture, and
// from the eye it is the same pixels on 3D shapes:
//   - floors (whole 80x36 tiles, their transparency interlocks) and roofs, 96 px = 3.33 units up;
//   - walls and other standing scenery on GROUND LINES fitted under their sprites (fitGround: a wall
//     axis or a corner of both, the proto's light-type bits naming a straight wall's axis), the
//     pieces of one wall snapped onto one line (snapWallRuns), cut at 96 px with the rest of the
//     sprite -- the top seen from above -- laid flat as a lid on a box as deep as it shows;
//   - multi-sprite scenery grouped into objects (groupScenery); a lump that fits as a card becomes
//     ONE board turned to the eye, as Doom stood its decorations;
//   - critters as billboards of their own six directions, the side picked by the bearing from the
//     eye, their painted ground shadow stripped; bodies and ground items as decals.
// Static geometry is rebuilt whenever the walls/scenery/items of the elevation change (a door
// swings, an item is picked up); its light is refreshed whenever a light source moves (the dude
// carries one); critters are rebuilt every frame from the live objects.
//
// THE EYE (F9). The dude's feet (his hex + his sub-hex offset), 1.72 units up, looking where he
// faces (his rotation, eased); 90 degrees across. Step 1 changes no control: the stick walks him as
// ever.
//
// DIABLO II'S PERSPECTIVE (F8, his "dělej na 1 a 2" after the map-viewer demo, paper3d --diablo).
// The game's own camera -- its heading, 25.7 degrees down -- but through a perspective lens riding
// above the dude: near things grow, far ones shrink towards the top, walls lean as you pass. At the
// ground under the dude the scale is the original's. Everyone stays the game's sprite as drawn
// (shadow and all), walls are sheets (a box's top and ends smeared into stripes on the near walls),
// a wall is cut at 96 px only under a roof, and the roof the dude walks under is lifted with
// everything flooded to it (tile_fill_roof), as in the game.

// ---------------------------------------------------------------------------
// GL entry points, loaded through SDL like crt_shader.cc's (never linked).
// ---------------------------------------------------------------------------

typedef void(APIENTRY* FP_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY* FP_Scissor)(GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY* FP_ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* FP_Clear)(GLbitfield);
typedef void(APIENTRY* FP_Disable)(GLenum);
typedef void(APIENTRY* FP_Enable)(GLenum);
typedef GLboolean(APIENTRY* FP_IsEnabled)(GLenum);
typedef void(APIENTRY* FP_DrawArrays)(GLenum, GLint, GLsizei);
typedef void(APIENTRY* FP_GetIntegerv)(GLenum, GLint*);
typedef void(APIENTRY* FP_GetFloatv)(GLenum, GLfloat*);
typedef void(APIENTRY* FP_GetBooleanv)(GLenum, GLboolean*);
typedef const GLubyte*(APIENTRY* FP_GetString)(GLenum);
typedef void(APIENTRY* FP_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void(APIENTRY* FP_PixelStorei)(GLenum, GLint);
typedef void(APIENTRY* FP_DepthMask)(GLboolean);
typedef void(APIENTRY* FP_DepthFunc)(GLenum);
typedef void(APIENTRY* FP_BlendFunc)(GLenum, GLenum);
typedef void(APIENTRY* FP_PolygonOffset)(GLfloat, GLfloat);
typedef void(APIENTRY* FP_BindTexture)(GLenum, GLuint);
typedef void(APIENTRY* FP_GenTextures)(GLsizei, GLuint*);
typedef void(APIENTRY* FP_DeleteTextures)(GLsizei, const GLuint*);
typedef void(APIENTRY* FP_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void(APIENTRY* FP_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
typedef void(APIENTRY* FP_TexParameteri)(GLenum, GLenum, GLint);

struct FpGl {
    FP_Viewport Viewport;
    FP_Scissor Scissor;
    FP_ClearColor ClearColor;
    FP_Clear Clear;
    FP_Disable Disable;
    FP_Enable Enable;
    FP_IsEnabled IsEnabled;
    FP_DrawArrays DrawArrays;
    FP_GetIntegerv GetIntegerv;
    FP_GetFloatv GetFloatv;
    FP_GetBooleanv GetBooleanv;
    FP_GetString GetString;
    FP_ReadPixels ReadPixels;
    FP_PixelStorei PixelStorei;
    FP_DepthMask DepthMask;
    FP_DepthFunc DepthFunc;
    FP_BlendFunc BlendFunc;
    FP_PolygonOffset PolygonOffset;
    FP_BindTexture BindTexture;
    FP_GenTextures GenTextures;
    FP_DeleteTextures DeleteTextures;
    FP_TexImage2D TexImage2D;
    FP_TexSubImage2D TexSubImage2D;
    FP_TexParameteri TexParameteri;

    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM1FPROC Uniform1f;
    PFNGLUNIFORM2FPROC Uniform2f;
    PFNGLUNIFORM3FPROC Uniform3f;
    PFNGLUNIFORM4FPROC Uniform4f;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLGETVERTEXATTRIBIVPROC GetVertexAttribiv;
    PFNGLACTIVETEXTUREPROC ActiveTexture;

    // Framebuffer objects: core names (GL 3.0 / ARB) or the EXT ones a GL 2.1 driver may only have.
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;

    // Optional: a VAO only where the driver insists.
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
};

static FpGl gl;
static bool fp_gl_loaded = false;
static int fp_mode = FP_VIEW_OFF;
static bool fp_failed = false;
static char fp_capture_path[1024];

// ---------------------------------------------------------------------------
// The engine's projection read as a camera (paper3d geometry.js).
// ---------------------------------------------------------------------------

static const double FP_PI = 3.14159265358979323846;
static const double FP_S = 32.0;
static const double FP_SIN = 0.4330127018922193; // sqrt(3) / 4
static const double FP_COS = 0.9013878188659973; // sqrt(13) / 4
static const double FP_KZ = FP_S * FP_SIN; // screen y per unit of ground depth
static const double FP_KY = FP_S * FP_COS; // screen y per unit of height
static const double FP_H_ROOF = 96.0 / FP_KY; // square_coord_roof lifts the roof 96 px
static const double FP_TO_CAMERA[3] = { 0.0, FP_SIN, FP_COS };
// Card, wall axis a (48,-12), wall axis b (32,24): the screen slope of a ground line.
static const double FP_SLOPES[3] = { 0.0, -0.25, 0.75 };
// The unit ground normal of a panel on each axis, towards the side the original camera saw.
static const double FP_WALL_NORMAL[3][3] = { { 0.0, 0.0, 1.0 }, { 0.5, 0.0, 0.8660254037844386 }, { -0.8660254037844386, 0.0, 0.5 } };
#define FP_AXIS_A 1
#define FP_AXIS_B 2
#define FP_V_PENALTY 1.0 // px of average gap a corner split must win by
#define FP_GROUP_SPAN 40 // px: how deep one object of several sprites may reach (the gate car)
#define FP_SNAP_TOL 12.5 // px: one wall strays up to ~11 px; parallel walls a hex apart are 16 / 24
#define FP_FOOT_TOL 12.0 // px a wall's feet may stand below its hex line (the Junktown gate's: 1.4 and 3.1)
#define FP_BOX_MIN 0.15 // units: the thinnest box behind a wall (POV)
#define FP_BOX_MAX 0.9 // units: the deepest (the prototype's cap; the port had lost it)
#define FP_BOX_THIN 0.25 // units: the box behind a wall with no top to measure
#define FP_BOX_CLEAR 0.3 // units a box keeps from the centre of a walkable hex behind it
#define FP_CUBOID_MAX 3.0 // units: the deepest box under a one-faced box-shaped object's top
#define FP_DOOR_MAX_W 120 // px: wider than a doorway (Vault 13's gear doors are 151 and 189)
#define FP_EGG_RX 1.2 // units: the egg's half-width round the dude (Diablo's view)
#define FP_EGG_RY 1.6 // units: its half-height round his middle
#define FP_LIFT_MIN 24.0 // px a decoration's lowest pixel stands above its hex when it rests ON something

// The eye (viewer.html first person: critter tops are ~2.0, so 1.9 looked over everyone's head).
static const double FP_EYE = 1.72;
static const double FP_NEAR = 0.05;
static const double FP_FAR = 120.0;
static const double FP_FOG_START = 18.0;
static const double FP_FOG_END = 75.0;
// Underground through his eyes: no sky, the dark comes on with distance, as Doom's light faded.
static const double FP_CAVE_FOG_START = 4.0;
static const double FP_CAVE_FOG_END = 40.0;
static const float FP_SKY[3] = { 0x8a / 255.0f, 0x7f / 255.0f, 0x6c / 255.0f };

struct FpV3 {
    double x;
    double y;
    double z;
};

static FpV3 fp_w(double sx, double sy, double h)
{
    return { sx / FP_S, h, (sy + FP_KY * h) / FP_KZ };
}

// tile_coord / square_coord with the view parked on tile 0 (camera-invariant world pixels).
static void fp_hex_top_left(int tile, int* x, int* y)
{
    int col = 199 - (tile % 200);
    int row = tile / 200;
    int half = col >> 1;
    *x = 48 * half + ((col & 1) != 0 ? 32 : 0) + 16 * row;
    *y = -12 * half + 12 * row;
}

// The hex whose centre is nearest the world pixel (sx, sy) (fp_hex_top_left's inverse), or -1.
static int fp_hex_at(double sx, double sy)
{
    // centres lie at x = 24 col + 16 row + 16, y = -6 col + 12 row + 8 (+8 / +6 on odd columns)
    double x = sx - 16.0;
    double y = sy - 8.0;
    double c = (x - (4.0 / 3.0) * y) / 32.0;
    double r = (y + 6.0 * c) / 12.0;
    int best = -1;
    double bestD = 1e18;
    for (int col = (int)floor(c) - 1; col <= (int)floor(c) + 2; col++) {
        for (int row = (int)floor(r) - 1; row <= (int)floor(r) + 2; row++) {
            if (col < 0 || col >= 200 || row < 0 || row >= 200) {
                continue;
            }
            int tile = row * 200 + (199 - col);
            int hx;
            int hy;
            fp_hex_top_left(tile, &hx, &hy);
            double dx = hx + 16 - sx;
            double dy = (hy + 8 - sy) * 2.0; // a hex is half as tall as wide on screen
            double d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                best = tile;
            }
        }
    }
    return best;
}

static void fp_square_top_left(int sq, int* x, int* y)
{
    int col = 99 - (sq % 100);
    int row = sq / 100;
    *x = -16 + 48 * col + 32 * row;
    *y = -2 - 12 * col + 24 * row;
}

// ---------------------------------------------------------------------------
// Sprites: the game's frames copied out of the art cache (palette indices), packed into atlas pages.
// ---------------------------------------------------------------------------

struct FpSprite {
    int w;
    int h;
    int xOff; // the art's per-direction shift (art->xOffsets[dir])
    int yOff;
    std::vector<unsigned char> px;
    int page; // -1 = not placed
    int ax;
    int ay;
};

#define FP_PAGE_SIZE 2048
#define FP_MAX_PAGES 8

struct FpPage {
    std::vector<unsigned char> px;
    int x;
    int y;
    int shelf;
    bool full;
    GLuint tex;
    int dirtyY0; // rows [dirtyY0, dirtyY1) still to upload
    int dirtyY1;
};

static std::vector<FpSprite> fp_sprites;
static std::unordered_map<uint64_t, int> fp_sprite_ids;
static std::vector<FpPage> fp_pages;
static bool fp_atlas_overflow = false;

// The painted ground SHADOW under a critter is the palette's darkest entries (hull.js silhouette):
// it would stand up with the billboard. Judged on the BASE palette (cmap, 6-bit), in the lowest
// 35% of the frame, and a dark pixel touching the body stays (boots, trouser edges).
static int fp_shadow_class(unsigned char c)
{
    // COLOR.PAL's out-of-range entries -- the cycling and glowing colours 229..255 -- load as black
    // into cmap, but their raw values are bright: body, as the prototype judged them
    if (!mappedColor[c]) {
        return 0;
    }
    int r = cmap[c * 3];
    int g = cmap[c * 3 + 1];
    int b = cmap[c * 3 + 2];
    if (std::max(r, std::max(g, b)) > 8) {
        return 0;
    }
    if (r == g && g == b && r >= 7) {
        return 0; // the boots' greys
    }
    if (b >= r + 3 && b >= g) {
        return 2; // a dark blue that is also the trousers' folds: shadow only among shadow
    }
    return 1;
}

static void fp_strip_shadow(FpSprite* s)
{
    int w = s->w;
    int h = s->h;
    int low = (int)floor(h * 0.65);
    std::vector<unsigned char> cls((size_t)w * h, 0);
    for (int p = low * w; p < w * h; p++) {
        if (s->px[p] != 0) {
            cls[p] = (unsigned char)fp_shadow_class(s->px[p]);
        }
    }
    std::vector<unsigned char> out = s->px;
    for (int y = low; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int p = y * w + x;
            if (cls[p] == 0) {
                continue;
            }
            int shadowNear = 0;
            bool bodyNear = false;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx;
                    int yy = y + dy;
                    if ((dx == 0 && dy == 0) || xx < 0 || yy < 0 || xx >= w || yy >= h) {
                        continue;
                    }
                    int q = yy * w + xx;
                    if (cls[q] == 1) {
                        shadowNear++;
                    } else if (s->px[q] != 0 && cls[q] == 0) {
                        bodyNear = true;
                    }
                }
            }
            if (bodyNear) {
                continue;
            }
            if (cls[p] == 1 || shadowNear >= 2) {
                out[p] = 0;
            }
        }
    }
    s->px.swap(out);
}

// A frame of `fid` as a sprite id, or -1. The art is locked only for the copy.
static int fp_sprite(int fid, int frame, int dir, bool noShadow)
{
    uint64_t key = ((uint64_t)(uint32_t)fid << 32) | ((uint64_t)(frame & 0xFFFF) << 16) | ((uint64_t)(dir & 0xFF) << 8) | (noShadow ? 1u : 0u);
    auto it = fp_sprite_ids.find(key);
    if (it != fp_sprite_ids.end()) {
        return it->second;
    }

    int id = -1;
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art != NULL) {
        int frames = art_frame_max_frame(art);
        int f = std::min(std::max(frame, 0), std::max(frames - 1, 0));
        int w = art_frame_width(art, f, dir);
        int h = art_frame_length(art, f, dir);
        unsigned char* data = art_frame_data(art, f, dir);
        if (w > 0 && h > 0 && data != NULL && w + 2 <= FP_PAGE_SIZE && h + 2 <= FP_PAGE_SIZE) {
            FpSprite s;
            s.w = w;
            s.h = h;
            s.xOff = art->xOffsets[dir];
            s.yOff = art->yOffsets[dir];
            s.px.assign(data, data + (size_t)w * h);
            s.page = -1;
            s.ax = 0;
            s.ay = 0;
            if (noShadow) {
                fp_strip_shadow(&s);
            }
            id = (int)fp_sprites.size();
            fp_sprites.push_back(std::move(s));
        }
        art_ptr_unlock(handle);
    }

    fp_sprite_ids[key] = id;
    return id;
}

// A frame whose pixels the game redraws in place (the to-hit reticle, gmouse's frame 284): its copy is
// refreshed whenever they differ -- written over its spot in the atlas when it keeps its size.
static int fp_sprite_live(int fid, int frame, int dir)
{
    int id = fp_sprite(fid, frame, dir, false);
    if (id < 0) {
        return -1;
    }
    CacheEntry* handle;
    Art* art = art_ptr_lock(fid, &handle);
    if (art == NULL) {
        return id;
    }
    int frames = art_frame_max_frame(art);
    int fr = std::min(std::max(frame, 0), std::max(frames - 1, 0));
    int w = art_frame_width(art, fr, dir);
    int h = art_frame_length(art, fr, dir);
    unsigned char* data = art_frame_data(art, fr, dir);
    FpSprite& s = fp_sprites[id];
    if (data != NULL && w == s.w && h == s.h && memcmp(data, s.px.data(), (size_t)w * h) != 0) {
        s.px.assign(data, data + (size_t)w * h);
        if (s.page >= 0 && s.page < (int)fp_pages.size()) {
            FpPage& page = fp_pages[s.page];
            for (int r = 0; r < h; r++) {
                memcpy(&(page.px[(size_t)(s.ay + r) * FP_PAGE_SIZE + s.ax]), &(s.px[(size_t)r * w]), w);
            }
            page.dirtyY0 = page.dirtyY1 > page.dirtyY0 ? std::min(page.dirtyY0, s.ay) : s.ay;
            page.dirtyY1 = std::max(page.dirtyY1, s.ay + h);
        }
    }
    art_ptr_unlock(handle);
    return id;
}

static void fp_atlas_reset()
{
    for (FpPage& page : fp_pages) {
        if (page.tex != 0 && fp_gl_loaded) {
            gl.DeleteTextures(1, &(page.tex));
        }
    }
    fp_pages.clear();
    for (FpSprite& s : fp_sprites) {
        s.page = -1;
    }
}

// Shelf-pack the sprite into the atlas (CPU copy; uploaded in the GL pass). False when the atlas is
// full -- the caller then resets it and rebuilds.
static bool fp_place(int id)
{
    FpSprite& s = fp_sprites[id];
    if (s.page >= 0) {
        return true;
    }

    for (;;) {
        if (fp_pages.empty() || fp_pages.back().full) {
            if ((int)fp_pages.size() >= FP_MAX_PAGES) {
                fp_atlas_overflow = true;
                return false;
            }
            FpPage page;
            page.px.assign((size_t)FP_PAGE_SIZE * FP_PAGE_SIZE, 0);
            page.x = 1;
            page.y = 1;
            page.shelf = 0;
            page.full = false;
            page.tex = 0;
            page.dirtyY0 = 0;
            page.dirtyY1 = 0;
            fp_pages.push_back(std::move(page));
        }

        FpPage& page = fp_pages.back();
        if (page.x + s.w + 1 > FP_PAGE_SIZE) {
            page.x = 1;
            page.y += page.shelf + 1;
            page.shelf = 0;
        }
        if (page.y + s.h + 1 > FP_PAGE_SIZE) {
            page.full = true;
            continue;
        }

        for (int r = 0; r < s.h; r++) {
            memcpy(&(page.px[(size_t)(page.y + r) * FP_PAGE_SIZE + page.x]), &(s.px[(size_t)r * s.w]), s.w);
        }
        s.page = (int)fp_pages.size() - 1;
        s.ax = page.x;
        s.ay = page.y;
        if (page.dirtyY1 <= page.dirtyY0) {
            page.dirtyY0 = page.y;
            page.dirtyY1 = page.y + s.h;
        } else {
            page.dirtyY0 = std::min(page.dirtyY0, page.y);
            page.dirtyY1 = std::max(page.dirtyY1, page.y + s.h);
        }
        page.x += s.w + 1;
        page.shelf = std::max(page.shelf, s.h);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Quads. A vertex: position, the sprite pixel it shows, the sprite's atlas rect, its light, and how
// dark its unpainted back is. Kinds: opaque, decal (pulled in front of the floor), translucent.
// ---------------------------------------------------------------------------

enum {
    FP_KIND_OPAQUE,
    FP_KIND_DECAL,
    FP_KIND_TRANS,
    FP_KIND_ROCK, // a cave's rock slopes: opaque, and alone what hides the holes the 2D frame fills
    FP_KIND_OVERLAY, // the game's cursor on the map (the red reticle on a target): over everything
    FP_KIND_SELECT, // the 3D editor's highlight over the piece it has picked
    FP_KIND_COUNT,
};

#define FP_VERTEX_FLOATS 11

struct FpQuad {
    int kind;
    int page;
    int lightTiles[6]; // per vertex: the hex its light is read from, -1 = a fixed light (roofs, see-through walls)
    float v[6 * FP_VERTEX_FLOATS]; // two triangles
};

// The hex the quads being emitted take their light from (fp_quad_fill), set by whoever emits them --
// or, for a floor, a hex and its light per corner (fp_emit_corner_*), so the light runs smoothly
// across it as the 2D floor's does instead of one value a tile.
static int fp_emit_light_tile = -1;
static const int* fp_emit_corner_tiles = NULL;
static const float* fp_emit_corner_lights = NULL;
// Whether the quads being emitted stand up (walls, scenery, decorations): only those are cut by the
// egg round the dude (FP_FRAGMENT_SHADER), never the floor. Carried as a negative "back" value.
static bool fp_emit_egg = false;

struct FpRange {
    int kind;
    int page;
    int first;
    int count;
};

// The triangle order that makes a quad face the side the original camera saw (geometry.js frontOrder).
static bool fp_front_is_ccw(const FpV3 c[4])
{
    double ax = c[1].x - c[0].x, ay = c[1].y - c[0].y, az = c[1].z - c[0].z;
    double bx = c[2].x - c[0].x, by = c[2].y - c[0].y, bz = c[2].z - c[0].z;
    double nx = ay * bz - az * by;
    double ny = az * bx - ax * bz;
    double nz = ax * by - ay * bx;
    return nx * FP_TO_CAMERA[0] + ny * FP_TO_CAMERA[1] + nz * FP_TO_CAMERA[2] >= 0.0;
}

static void fp_quad_fill(FpQuad* quad, int kind, const FpSprite& s, const FpV3 c[4], const double pix[4][2], float light, float back, bool ccw)
{
    static const int ORDER_CCW[6] = { 0, 1, 2, 0, 2, 3 };
    static const int ORDER_CW[6] = { 0, 2, 1, 0, 3, 2 };
    const int* order = ccw ? ORDER_CCW : ORDER_CW;
    quad->kind = kind;
    quad->page = s.page;
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        quad->lightTiles[i] = fp_emit_corner_tiles != NULL ? fp_emit_corner_tiles[k] : fp_emit_light_tile;
        float* v = &(quad->v[i * FP_VERTEX_FLOATS]);
        v[0] = (float)c[k].x;
        v[1] = (float)c[k].y;
        v[2] = (float)c[k].z;
        v[3] = (float)pix[k][0];
        v[4] = (float)pix[k][1];
        v[5] = (float)s.ax;
        v[6] = (float)s.ay;
        v[7] = (float)s.w;
        v[8] = (float)s.h;
        v[9] = fp_emit_corner_lights != NULL ? fp_emit_corner_lights[k] : light;
        v[10] = fp_emit_egg ? -back : back;
    }
}

// A quad textured PROJECTIVELY: each corner shows the sprite pixel the original camera sees there
// (with `shift`, a face behind the front samples the front -- the back of a box is its mirror). It
// faces the original camera (its other side is the "unpainted back", dimmed) -- or, for a face of a
// box, `outward`, so whichever side of the box the eye is on, the face it sees is lit.
static bool fp_emit_proj(std::vector<FpQuad>& out, int kind, int sprite, double sx, double sy, const FpV3 c[4], const FpV3* shift, float light, float back, const FpV3* outward = NULL)
{
    if (!fp_place(sprite)) {
        return false;
    }
    const FpSprite& s = fp_sprites[sprite];
    double pix[4][2];
    for (int i = 0; i < 4; i++) {
        FpV3 p = c[i];
        if (shift != NULL) {
            p.x += shift[i].x;
            p.y += shift[i].y;
            p.z += shift[i].z;
        }
        pix[i][0] = FP_S * p.x - sx;
        pix[i][1] = FP_KZ * p.z - FP_KY * p.y - sy;
    }
    bool ccw = fp_front_is_ccw(c);
    if (outward != NULL) {
        double ax = c[1].x - c[0].x, ay = c[1].y - c[0].y, az = c[1].z - c[0].z;
        double bx = c[2].x - c[0].x, by = c[2].y - c[0].y, bz = c[2].z - c[0].z;
        double nx = ay * bz - az * by;
        double ny = az * bx - ax * bz;
        double nz = ax * by - ay * bx;
        ccw = nx * outward->x + ny * outward->y + nz * outward->z >= 0.0;
    }
    FpQuad quad;
    fp_quad_fill(&quad, kind, s, c, pix, light, back, ccw);
    out.push_back(quad);
    return true;
}

// A billboard quad: the sprite standing upright at `anchor`, turned to `psi` (the bearing it is seen
// from), dx0/dy0 the sprite's top-left relative to the anchor in screen px; `lift` pulls it towards
// the eye (a board's pieces, back to front).
static bool fp_emit_billboard(std::vector<FpQuad>& out, int sprite, FpV3 anchor, double psi, double dx0, double dy0, double lift, float light)
{
    if (!fp_place(sprite)) {
        return false;
    }
    const FpSprite& s = fp_sprites[sprite];
    double rx = cos(psi);
    double rz = sin(psi);
    double tx = -sin(psi) * lift; // towards the eye
    double tz = cos(psi) * lift;
    double x0 = dx0 / FP_S;
    double x1 = (dx0 + s.w) / FP_S;
    double top = -dy0 / FP_KY;
    double bot = -(dy0 + s.h) / FP_KY;
    FpV3 c[4] = {
        { anchor.x + rx * x0 + tx, bot, anchor.z + rz * x0 + tz },
        { anchor.x + rx * x1 + tx, bot, anchor.z + rz * x1 + tz },
        { anchor.x + rx * x1 + tx, top, anchor.z + rz * x1 + tz },
        { anchor.x + rx * x0 + tx, top, anchor.z + rz * x0 + tz },
    };
    double pix[4][2] = { { 0.0, (double)s.h }, { (double)s.w, (double)s.h }, { (double)s.w, 0.0 }, { 0.0, 0.0 } };
    FpQuad quad;
    fp_quad_fill(&quad, FP_KIND_OPAQUE, s, c, pix, light, 1.0f, true);
    out.push_back(quad);
    return true;
}

// ---------------------------------------------------------------------------
// Ground lines (paper3d build.js bottomEdge / slopeTable / fitGround / groupScenery / snapWallRuns).
// A standing sprite rests on a line y = k + m x in screen px, drawn UNDER every opaque pixel (a pixel
// below the ground would vanish into the floor and the original view would lose it).
// ---------------------------------------------------------------------------

struct FpTable {
    std::vector<double> pmax;
    std::vector<double> smax;
    std::vector<double> psum;
    std::vector<int> pn;
    int w;
};

struct FpRun {
    double k;
    int n;
    double sum;
};

static void fp_table_build(FpTable* t, const std::vector<double>& bottom, double ulx, double m)
{
    int w = (int)bottom.size();
    double neg = -std::numeric_limits<double>::infinity();
    t->w = w;
    t->pmax.assign(w + 1, neg);
    t->smax.assign(w + 1, neg);
    t->psum.assign(w + 1, 0.0);
    t->pn.assign(w + 1, 0);
    for (int c = 0; c < w; c++) {
        bool ok = !std::isnan(bottom[c]);
        double v = ok ? bottom[c] - m * (ulx + c + 0.5) : 0.0;
        t->pmax[c + 1] = ok ? std::max(t->pmax[c], v) : t->pmax[c];
        t->psum[c + 1] = t->psum[c] + v;
        t->pn[c + 1] = t->pn[c] + (ok ? 1 : 0);
    }
    for (int c = w - 1; c >= 0; c--) {
        bool ok = !std::isnan(bottom[c]);
        double v = ok ? bottom[c] - m * (ulx + c + 0.5) : 0.0;
        t->smax[c] = ok ? std::max(t->smax[c + 1], v) : t->smax[c + 1];
    }
}

static FpRun fp_left(const FpTable& t, int s)
{
    return { t.pmax[s], t.pn[s], t.psum[s] };
}

static FpRun fp_right(const FpTable& t, int s)
{
    return { t.smax[s], t.pn[t.w] - t.pn[s], t.psum[t.w] - t.psum[s] };
}

static double fp_gap(const FpRun& r, double k)
{
    return r.n * k - r.sum;
}

struct FpSeg {
    int axis;
    double k;
    double x0;
    double x1;
    int line; // snapWallRuns: the hex line it stands on
    // Through his eyes (fp_box_runs): the wall run it belongs to (0 = none), its box's depth in units
    // (0 = measured by fp_emit_panel), the height of its box top in px when it is not cut at 96 px,
    // and from what depth each end face shows (-1 = none: inside a run, or another wall's front
    // covers it all).
    int run;
    double depth;
    double topPx;
    double from0;
    double from1;
    double lo0; // px: an end face starts this high (where a lower neighbour's top meets it)
    double lo1;
    // A piece standing off its chain's line (fp_snap_wall_runs): `fore` is its part in front, cut
    // `cut` px high (no lid) -- the junk painted before the wall; a twin seg on the chain's line
    // (chainRun) draws the rest of the sprite where the wall is.
    bool fore;
    double cut;
    bool sloped; // turned in the 3D editor: its line's screen slope is `slope`, not its axis'
    double slope;
    double chainK;
    int chainRun;
};

struct FpFit {
    double cost;
    std::vector<FpSeg> segs;
};

static void fp_consider(FpFit* best, bool* have, double cost, const std::vector<FpSeg>& segs)
{
    if (!*have || cost < best->cost) {
        best->cost = cost;
        best->segs = segs;
        *have = true;
    }
}

static FpFit fp_fit_ground(const std::vector<double>& bottom, double ulx, const int* axes, int axisCount, bool allowV, int prefer)
{
    int w = (int)bottom.size();
    FpTable tables[3];
    for (int a = 0; a < 3; a++) {
        fp_table_build(&tables[a], bottom, ulx, FP_SLOPES[a]);
    }
    int cols = fp_left(tables[0], w).n;
    if (cols == 0) {
        cols = 1;
    }

    FpFit best;
    best.cost = 0.0;
    bool have = false;
    for (int i = 0; i < axisCount; i++) {
        int axis = axes[i];
        FpRun r = fp_left(tables[axis], w);
        std::vector<FpSeg> segs = { { axis, r.k, ulx, ulx + w, 0 } };
        fp_consider(&best, &have, fp_gap(r, r.k) / cols - (axis == prefer ? 0.75 : 0.0), segs);
    }
    if (allowV) {
        static const int PAIRS[2][2] = { { FP_AXIS_A, FP_AXIS_B }, { FP_AXIS_B, FP_AXIS_A } };
        for (int p = 0; p < 2; p++) {
            int a1 = PAIRS[p][0];
            int a2 = PAIRS[p][1];
            double m1 = FP_SLOPES[a1];
            double m2 = FP_SLOPES[a2];
            for (int s = 6; s <= w - 6; s++) {
                FpRun l = fp_left(tables[a1], s);
                FpRun r = fp_right(tables[a2], s);
                if (l.n == 0 || r.n == 0) {
                    continue;
                }
                double xs = ulx + s;
                double yv = std::max(l.k + m1 * xs, r.k + m2 * xs); // meet at the lower of the two
                double k1 = yv - m1 * xs;
                double k2 = yv - m2 * xs;
                double cost = (fp_gap(l, k1) + fp_gap(r, k2)) / cols + FP_V_PENALTY;
                if (!have || cost < best.cost) {
                    std::vector<FpSeg> segs = { { a1, k1, ulx, xs, 0 }, { a2, k2, xs, ulx + w, 0 } };
                    fp_consider(&best, &have, cost, segs);
                }
            }
        }
    }
    return best;
}

static double fp_median(std::vector<double>& v)
{
    if (v.empty()) {
        return 0.0;
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// A standing sprite: a wall, a door, a piece of scenery, a container.
struct FpPiece {
    int sprite;
    int ulx;
    int uly;
    int w;
    int h;
    int tile;
    int anchorY; // its hex foot, for drawing a board's pieces back to front
    double footY; // the same with the object's own shift (obj->y): where it stands
    bool trans;
    bool door;
    float light;
    int lightTile; // fp_light_tile_of
    int flags; // the object's, for its light in a board
    std::vector<double> bottom;
    std::vector<double> top; // its topmost opaque row per column, screen y (NaN = none)
    double lowest;
    FpTable tables[3];
    std::vector<FpSeg> segs;
    int group; // -1 = none (walls, doors, translucent pieces)
    bool boarded;
    // through his eyes: a box-shaped object (fp_box_fit) cut at its own height, the rest of it --
    // its top, seen from above -- laid flat there; `closed` = two faces meeting at a front corner,
    // whose ends would only repeat the other face
    bool cuboid;
    bool closed;
    double cutPx;
    bool hung; // a decoration painted onto the wall it hangs on (its one seg is that wall's line)
    int fid; // the object's art, with its tile what the 3D editor knows it by
    int copy; // -1 = the object itself, else a copy the 3D editor made of it
    bool edited; // the 3D editor touched it (picked out while editing)
    bool hiddenEdit; // hidden by the 3D editor (shown, marked, while editing)
    bool rock; // a cave's rock slope (fp_is_rock_wall)
};

// Through his eyes only the strips of ONE picture join: the game cuts big scenery into columns that
// abut (gap -2..2 px) and continue each other across the seam. Anything merely overlapping -- an
// easel in front of a bookshelf, a sign on the far side of a wall behind a desk, a row of cabbages --
// stays apart.
static bool fp_seam(const FpPiece& a, const FpPiece& b)
{
    const FpPiece& l = a.ulx <= b.ulx ? a : b;
    const FpPiece& r = a.ulx <= b.ulx ? b : a;
    int gap = r.ulx - (l.ulx + l.w);
    if (gap < -2 || gap > 2) {
        return false;
    }
    const FpSprite& ls = fp_sprites[l.sprite];
    const FpSprite& rs = fp_sprites[r.sprite];
    int y0 = std::max(l.uly, r.uly);
    int y1 = std::min(l.uly + l.h, r.uly + r.h);
    int count = 0;
    for (int yy = y0; yy < y1; yy++) {
        bool left = false;
        bool right = false;
        for (int c = std::max(0, l.w - 3); c < l.w && !left; c++) {
            left = ls.px[(size_t)(yy - l.uly) * l.w + c] != 0;
        }
        for (int c = 0; c < std::min(3, r.w) && !right; c++) {
            right = rs.px[(size_t)(yy - r.uly) * r.w + c] != 0;
        }
        if (left && right) {
            count++;
        }
    }
    return count >= std::max(6, std::min(l.h, r.h) / 4);
}

// The ground line under screen x on a fit (the last seg starting at or before x).
static double fp_line_at(const std::vector<FpSeg>& segs, double x)
{
    const FpSeg* s = &segs[0];
    for (const FpSeg& seg : segs) {
        if (x >= seg.x0) {
            s = &seg;
        }
    }
    return s->k + FP_SLOPES[s->axis] * x;
}

struct FpBoxFit {
    double hpx; // its height in screen px, 0 = not a box
    bool v; // two faces (a front corner)
    double depthB; // the boxes behind its faces, units (a corner's: the other face's length)
    double depthA;
};

// Is the object drawn as a box standing on the hex grid? The game's camera draws such a box as a
// hexagon: its feet on one ground line or on a corner pointing at the camera, its outermost columns
// its vertical edges -- exactly KY px per unit of height -- and its top edge(s) parallel to the
// feet, that height above them. Shelves, desks, crates, cars pass; round and ragged things fail.
static FpBoxFit fp_box_fit(const std::vector<double>& bottom, const std::vector<double>& top, double gx0, const std::vector<FpSeg>& segs)
{
    FpBoxFit none = { 0.0, false, 0.0, 0.0 };
    if (segs.empty() || segs[0].axis == 0) {
        return none;
    }
    bool v = segs.size() == 2;
    if (v && (segs[0].axis != FP_AXIS_B || segs[1].axis != FP_AXIS_A)) {
        return none; // only a corner towards the camera
    }
    int w = (int)top.size();
    int cL = -1;
    int cR = -1;
    for (int c = 0; c < w; c++) {
        if (!std::isnan(top[c]) && !std::isnan(bottom[c])) {
            cR = c;
            if (cL < 0) {
                cL = c;
            }
        }
    }
    if (cL < 0 || cR - cL < 12) {
        return none;
    }
    auto rise = [&](int c) {
        return fp_line_at(segs, gx0 + c + 0.5) - top[c];
    };
    auto med3 = [&](int c) {
        std::vector<double> r;
        for (int i = c; i < c + 3; i++) {
            if (!std::isnan(top[i])) {
                r.push_back(rise(i));
            }
        }
        return fp_median(r);
    };
    double hl = med3(cL);
    double hr = med3(cR - 2);
    double hpx = v ? (hl + hr) / 2.0 : (segs[0].axis == FP_AXIS_A ? hr : hl);
    if (hpx < 12.0 || fabs(hl - hr) > std::max(v ? 3.0 : 8.0, 0.15 * hpx)) {
        return none;
    }

    // its feet stand on the line at both ends of every face
    for (const FpSeg& s : segs) {
        int a = std::max(cL, (int)ceil(s.x0 - gx0));
        int b = std::min(cR, (int)floor(s.x1 - gx0) - 1);
        if (b - a < 4) {
            continue;
        }
        int q = std::max(2, (b - a + 1) / 4);
        for (int end = 0; end < 2; end++) {
            double best = 1e9;
            for (int c = end == 0 ? a : b - q + 1; c <= (end == 0 ? a + q - 1 : b); c++) {
                if (!std::isnan(bottom[c])) {
                    best = std::min(best, fp_line_at(segs, gx0 + c + 0.5) - bottom[c]);
                }
            }
            if (best > 2.5) {
                return none;
            }
        }
    }

    // its top edge(s): the far outline of a box that high
    std::vector<double> miss;
    int above = 0;
    if (v) {
        double xL = gx0 + cL + 0.5;
        double xR = gx0 + cR + 0.5;
        double xv = segs[0].x1;
        double yL = fp_line_at(segs, xL);
        double yR = fp_line_at(segs, xR);
        double xB = xL + xR - xv;
        for (int c = cL; c <= cR; c++) {
            if (std::isnan(top[c])) {
                continue;
            }
            double xc = gx0 + c + 0.5;
            double backLine = xc <= xB ? yL + FP_SLOPES[FP_AXIS_A] * (xc - xL) : yR - FP_SLOPES[FP_AXIS_B] * (xR - xc);
            double d = top[c] - (backLine - hpx);
            miss.push_back(fabs(d));
            if (d > 3.0) {
                above++; // the outline dips under the box's top: not a box (pixels above it are things on top)
            }
        }
        if (fp_median(miss) > 2.0 || above > (int)miss.size() / 10) {
            return none;
        }
        FpBoxFit fit = { hpx, true, (xR - xv) / 27.71, (xv - xL) / 16.0 };
        return fit;
    }
    int m0 = cL + (cR - cL) / 5;
    int m1 = cR - (cR - cL) / 5;
    std::vector<double> rises;
    for (int c = m0; c <= m1; c++) {
        if (!std::isnan(top[c])) {
            rises.push_back(rise(c));
        }
    }
    double mid = fp_median(rises);
    for (double& r : rises) {
        r = fabs(r - mid);
    }
    if (rises.empty() || fp_median(rises) > 2.0) {
        return none;
    }
    // one face: its top's far edge rises above the front edge by 16 px per unit of depth on axis a,
    // 27.7 on axis b (the rise over its middle minus its height)
    double depth = std::min(FP_CUBOID_MAX, std::max(FP_BOX_MIN, (mid - hpx) * FP_WALL_NORMAL[segs[0].axis][2] / FP_KZ));
    FpBoxFit fit = { hpx, false, depth, depth };
    return fit;
}

// The share of an object's columns whose feet stand on its fitted line (a fence does; a lump of junk
// touches it here and there).
static double fp_standing_share(const std::vector<double>& bottom, double gx0, const std::vector<FpSeg>& segs)
{
    int cols = 0;
    int standing = 0;
    for (int c = 0; c < (int)bottom.size(); c++) {
        if (std::isnan(bottom[c])) {
            continue;
        }
        cols++;
        if (fp_line_at(segs, gx0 + c + 0.5) - bottom[c] <= 3.0) {
            standing++;
        }
    }
    return cols > 0 ? (double)standing / cols : 0.0;
}

// Big scenery is often several sprites on neighbouring hexes; fitted one by one it came apart in
// strips. Pieces whose rectangles overlap or touch and stand on nearly the same line are ONE object
// with one ground under their combined bottom edge (through his eyes: only strips of one picture).
// From the game's camera a lump stands on a card unless an axis or a corner clearly fits better.
// Through his eyes the object's shape decides what it becomes (`boardable`, per group): a box
// becomes a cuboid; a long run standing on its line (a fence of scenery) an uncut panel; anything
// else a Doom decoration -- one picture turned to the eye.
static void fp_group_scenery(std::vector<FpPiece>& pieces, const std::vector<int>& ids, std::vector<std::vector<int>>& groups, std::vector<char>& boardable, bool eyes)
{
    int n = (int)ids.size();
    std::vector<int> parent(n);
    std::vector<double> lo(n);
    std::vector<double> hi(n);
    for (int i = 0; i < n; i++) {
        parent[i] = i;
        lo[i] = pieces[ids[i]].lowest;
        hi[i] = pieces[ids[i]].lowest;
    }
    auto find = [&](int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            const FpPiece& a = pieces[ids[i]];
            const FpPiece& b = pieces[ids[j]];
            if (a.ulx > b.ulx + b.w + 2 || b.ulx > a.ulx + a.w + 2 || a.uly > b.uly + b.h + 2 || b.uly > a.uly + a.h + 2) {
                continue;
            }
            if (eyes && !fp_seam(a, b)) {
                continue;
            }
            int ri = find(i);
            int rj = find(j);
            if (ri == rj) {
                continue;
            }
            double l = std::min(lo[ri], lo[rj]);
            double h = std::max(hi[ri], hi[rj]);
            if (h - l > FP_GROUP_SPAN) {
                continue;
            }
            parent[ri] = rj;
            lo[rj] = l;
            hi[rj] = h;
        }
    }

    std::unordered_map<int, int> groupOfRoot;
    for (int i = 0; i < n; i++) {
        int r = find(i);
        auto it = groupOfRoot.find(r);
        if (it == groupOfRoot.end()) {
            groupOfRoot[r] = (int)groups.size();
            groups.emplace_back();
            groups.back().push_back(ids[i]);
        } else {
            groups[it->second].push_back(ids[i]);
        }
    }
    boardable.resize(groups.size(), 0);

    for (int g = 0; g < (int)groups.size(); g++) {
        std::vector<int>& members = groups[g];
        if (members.empty() || pieces[members[0]].group != -1) {
            continue; // a group from an earlier call
        }
        int gx0 = std::numeric_limits<int>::max();
        int gx1 = std::numeric_limits<int>::min();
        for (int id : members) {
            gx0 = std::min(gx0, pieces[id].ulx);
            gx1 = std::max(gx1, pieces[id].ulx + pieces[id].w);
        }
        std::vector<double> comb(gx1 - gx0, std::numeric_limits<double>::quiet_NaN());
        std::vector<double> combTop(gx1 - gx0, std::numeric_limits<double>::quiet_NaN());
        for (int id : members) {
            const FpPiece& p = pieces[id];
            for (int c = 0; c < p.w; c++) {
                double b = p.bottom[c];
                if (std::isnan(b)) {
                    continue;
                }
                int x = p.ulx + c - gx0;
                comb[x] = std::isnan(comb[x]) ? b : std::max(comb[x], b);
                combTop[x] = std::isnan(combTop[x]) ? p.top[c] : std::min(combTop[x], p.top[c]);
            }
        }
        // a card is the safe default for a lump; an axis (or a corner) must beat it clearly
        static const int CARD[1] = { 0 };
        static const int AXES[2] = { FP_AXIS_A, FP_AXIS_B };
        FpFit fit = fp_fit_ground(comb, gx0, CARD, 1, false, 0);
        bool board = eyes;
        bool cuboid = false;
        FpBoxFit box = { 0.0, false, 0.0, 0.0 };
        if (eyes) {
            if (gx1 - gx0 >= 16) {
                FpFit shaped = fp_fit_ground(comb, gx0, AXES, 2, true, 0);
                box = fp_box_fit(comb, combTop, gx0, shaped.segs);
                if (box.hpx > 0.0) {
                    fit = shaped;
                    board = false;
                    cuboid = true;
                } else if (gx1 - gx0 >= 40 && shaped.cost <= fit.cost * 0.5) {
                    fit = shaped;
                    board = !(gx1 - gx0 >= 64 && fp_standing_share(comb, gx0, shaped.segs) >= 0.6);
                }
            }
        } else if (gx1 - gx0 >= 40) {
            FpFit shaped = fp_fit_ground(comb, gx0, AXES, 2, true, 0);
            if (shaped.cost <= fit.cost * 0.5) {
                fit = shaped;
            }
        }
        boardable[g] = board ? 1 : 0;
        for (int id : members) {
            FpPiece& p = pieces[id];
            p.group = g;
            p.cuboid = cuboid;
            p.closed = cuboid && box.v;
            p.cutPx = cuboid ? box.hpx : 0.0;
            p.segs.clear();
            for (const FpSeg& s : fit.segs) {
                double x0 = std::max(s.x0, (double)p.ulx);
                double x1 = std::min(s.x1, (double)(p.ulx + p.w));
                if (x1 - x0 > 0.01) {
                    p.segs.push_back({ s.axis, s.k, x0, x1, 0 });
                }
            }
            if (p.segs.empty() && !fit.segs.empty()) {
                p.segs.push_back({ fit.segs[0].axis, fit.segs[0].k, (double)p.ulx, (double)(p.ulx + p.w), 0 });
            }
            if (cuboid) {
                for (FpSeg& s : p.segs) {
                    s.depth = s.axis == FP_AXIS_B ? box.depthB : box.depthA;
                    if (box.v) {
                        s.from0 = -1.0; // a closed corner's ends are the other face
                        s.from1 = -1.0;
                    }
                }
            }
        }
    }
}

// The pieces of one wall each fit their own line and opened gaps between them. Pieces with touching
// stretches on the same axis are one wall when they stand on the same hex line, or their lines
// nearly agree; the whole chain takes the front-most line of the members whose feet stand on their
// own hex line -- a piece reaching further forward (junk painted in front of the Junktown gate's
// container, 47 px) keeps its own line instead of dragging the whole wall out of place. A corner's
// two lines are then made to meet again. Each seg learns its chain (seg.run); returns how many
// chains were mostly off their hex lines and kept the old front-most rule.
static int fp_snap_wall_runs(std::vector<FpPiece>& pieces, const std::vector<int>& ids)
{
    std::vector<FpSeg*> segs;
    std::vector<double> hexK; // the line through the seg's hex centre, on its axis
    for (int id : ids) {
        FpPiece& p = pieces[id];
        int hx;
        int hy;
        fp_hex_top_left(p.tile, &hx, &hy);
        for (FpSeg& s : p.segs) {
            if (s.axis == 0) {
                continue;
            }
            s.line = s.axis == FP_AXIS_A ? 1000 + p.tile / 200 : 2000 + p.tile % 200;
            segs.push_back(&s);
            hexK.push_back((hy + 8) - FP_SLOPES[s.axis] * (hx + 16));
        }
    }
    int n = (int)segs.size();
    std::vector<int> parent(n);
    for (int i = 0; i < n; i++) {
        parent[i] = i;
    }
    auto find = [&](int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    auto touch = [](const FpSeg* a, const FpSeg* b) {
        return a->x0 <= b->x1 + 2 && b->x0 <= a->x1 + 2;
    };

    std::unordered_map<int, std::vector<int>> byLine;
    for (int i = 0; i < n; i++) {
        byLine[segs[i]->line].push_back(i);
    }
    for (auto& entry : byLine) {
        std::vector<int>& line = entry.second;
        std::stable_sort(line.begin(), line.end(), [&](int a, int b) { return segs[a]->x0 < segs[b]->x0; });
        for (int i = 1; i < (int)line.size(); i++) {
            for (int j = i - 1; j >= 0 && segs[line[j]]->x1 + 2 >= segs[line[i]]->x0 - 64; j--) {
                if (touch(segs[line[j]], segs[line[i]])) {
                    parent[find(line[i])] = find(line[j]);
                    break;
                }
            }
        }
    }

    std::vector<int> order(n);
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (segs[a]->axis != segs[b]->axis) {
            return segs[a]->axis < segs[b]->axis;
        }
        return segs[a]->k < segs[b]->k;
    });
    for (int oi = 0; oi < n; oi++) {
        const FpSeg* a = segs[order[oi]];
        for (int oj = oi + 1; oj < n; oj++) {
            const FpSeg* b = segs[order[oj]];
            if (b->axis != a->axis || b->k - a->k > FP_SNAP_TOL) {
                break;
            }
            if (touch(a, b)) {
                parent[find(order[oi])] = find(order[oj]);
            }
        }
    }

    std::unordered_map<int, double> front; // over the members on their hex line
    std::unordered_map<int, double> anyFront; // over all members
    std::unordered_map<int, int> members;
    std::unordered_map<int, int> offLine;
    for (int i = 0; i < n; i++) {
        int r = find(i);
        double k = segs[i]->k;
        auto any = anyFront.find(r);
        anyFront[r] = any == anyFront.end() ? k : std::max(any->second, k);
        members[r]++;
        if (k <= hexK[i] + FP_FOOT_TOL) {
            auto it = front.find(r);
            front[r] = it == front.end() ? k : std::max(it->second, k);
        } else {
            offLine[r]++;
        }
    }
    int oldRule = 0;
    for (auto& entry : members) {
        if (front.find(entry.first) == front.end() || offLine[entry.first] * 3 > entry.second) {
            front[entry.first] = anyFront[entry.first]; // a chain mostly off its hex lines: as before
            oldRule++;
        }
    }
    for (int i = 0; i < n; i++) {
        int r = find(i);
        double chain = front[r];
        // an off-line piece in front of its chain stands alone: its own box, its own ends
        if (segs[i]->k > chain + 0.5) {
            segs[i]->run = n + 1 + i;
            segs[i]->fore = true;
            segs[i]->chainK = chain;
            segs[i]->chainRun = r + 1;
        } else {
            segs[i]->run = r + 1;
            segs[i]->k = std::max(segs[i]->k, chain);
        }
    }

    for (int id : ids) {
        FpPiece& p = pieces[id];
        if (p.segs.size() != 2) {
            continue;
        }
        FpSeg& s1 = p.segs[0];
        FpSeg& s2 = p.segs[1];
        double m1 = FP_SLOPES[s1.axis];
        double m2 = FP_SLOPES[s2.axis];
        if (m1 == m2) {
            continue;
        }
        for (int iter = 0; iter < 3; iter++) {
            double xv = (s2.k - s1.k) / (m1 - m2);
            xv = std::min(std::max(xv, s1.x0 + 1), s2.x1 - 1);
            s1.x1 = xv;
            s2.x0 = xv;
            int split = (int)lround(xv - p.ulx);
            split = std::max(0, std::min(p.w, split));
            double need1 = fp_left(p.tables[s1.axis], split).k;
            double need2 = fp_right(p.tables[s2.axis], split).k;
            if (need1 <= s1.k + 1e-6 && need2 <= s2.k + 1e-6) {
                break;
            }
            s1.k = std::max(s1.k, need1);
            s2.k = std::max(s2.k, need2);
        }
    }
    return oldRule;
}

// The unit vector along a ground line of `axis` (screen x growing), in world x/z.
static void fp_axis_dir(int axis, double* ux, double* uz, double* unitsPerPx)
{
    double m = FP_SLOPES[axis];
    double lpx = sqrt(1.0 / (FP_S * FP_S) + (m * m) / (FP_KZ * FP_KZ));
    *ux = (1.0 / FP_S) / lpx;
    *uz = (m / FP_KZ) / lpx;
    *unitsPerPx = lpx;
}

// How deep the end face at screen x `xe` of `s` lies on another wall's front: an end's ground trace
// runs from the wall's foot straight back (-normal), which is along the OTHER axis, so at a corner it
// lies in the other wall's front plane, and drawn there it would paint its smear over that front.
static double fp_end_cover(const std::vector<const FpSeg*>& others, const FpSeg& s, double xe, double depth)
{
    double ye = s.k + FP_SLOPES[s.axis] * xe;
    double step = -FP_S * FP_WALL_NORMAL[s.axis][0]; // screen px per unit back: -16 (axis a), +27.7 (b)
    double best = 0.0;
    for (const FpSeg* o : others) {
        if (o->axis == s.axis || o->axis == 0 || o->fore || fabs(o->k + FP_SLOPES[o->axis] * xe - ye) > 1.5) {
            continue;
        }
        double c = 0.0;
        if (step < 0.0 && o->x0 < xe - 0.5 && o->x1 >= xe - 1.5) {
            c = xe - std::max(o->x0, xe + step * depth);
        } else if (step > 0.0 && o->x1 > xe + 0.5 && o->x0 <= xe + 1.5) {
            c = std::min(o->x1, xe + step * depth) - xe;
        }
        best = std::max(best, c);
    }
    return best / fabs(step);
}

// Through his eyes a wall is a box. Its depth is what its own top shows (the opaque run above the
// 96 px cut: its back edge lies 16 px per unit above the cut on axis a, 27.7 on axis b), kept out of
// any walkable hex behind it, one depth for a whole run so the backs line up; a wall with no top to
// measure (and a run of scenery, never cut) gets a thin box and a top at its median height. A run is
// cut into stretches at gaps and at doors (a doorway's jambs are ends); only a stretch's two outer
// ends get end faces, and only the part no other wall's front covers -- plus, inside it, the side a
// taller piece shows above a lower neighbour.
// The hexes a wall or a piece of scenery blocks (a door is a passage, open or not).
static void fp_blocked_hexes(const std::vector<Object*>& objects, std::vector<unsigned char>& blocked)
{
    blocked.assign(HEX_GRID_SIZE, 0);
    for (Object* obj : objects) {
        int type = FID_TYPE(obj->fid);
        if ((type != OBJ_TYPE_WALL && type != OBJ_TYPE_SCENERY) || (obj->flags & OBJECT_NO_BLOCK) != 0) {
            continue;
        }
        Proto* proto;
        if (type == OBJ_TYPE_SCENERY && proto_ptr(obj->pid, &proto) == 0 && proto->scenery.type == SCENERY_TYPE_DOOR) {
            continue;
        }
        if (obj->tile >= 0 && obj->tile < HEX_GRID_SIZE) {
            blocked[obj->tile] = 1;
            if ((obj->flags & OBJECT_MULTIHEX) != 0) {
                for (int dir = 0; dir < 6; dir++) {
                    int t = tile_num_in_direction(obj->tile, dir, 1);
                    if (t >= 0 && t < HEX_GRID_SIZE) {
                        blocked[t] = 1;
                    }
                }
            }
        }
    }
}

static void fp_box_runs(std::vector<FpPiece>& pieces, const std::vector<int>& ids, const std::vector<Object*>& objects)
{
    // hexes a box must keep out of: whatever is not blocked by a wall or scenery (doors are passages)
    std::vector<unsigned char> blocked;
    fp_blocked_hexes(objects, blocked);

    struct Member {
        FpPiece* piece;
        FpSeg* seg;
        double depth;
        double guard;
        bool capped;
    };
    std::unordered_map<int, std::vector<Member>> runs;
    std::vector<const FpSeg*> all;
    for (int id : ids) {
        FpPiece& p = pieces[id];
        if (p.trans) {
            continue;
        }
        const FpSprite& s = fp_sprites[p.sprite];
        for (FpSeg& seg : p.segs) {
            if (seg.axis == 0) {
                continue;
            }
            all.push_back(&seg);
            double m = FP_SLOPES[seg.axis];
            const double* nrm = FP_WALL_NORMAL[seg.axis];
            double b0 = seg.k + m * seg.x0;
            double b1 = seg.k + m * seg.x1;
            int c0 = std::max(0, (int)floor(seg.x0 - p.ulx));
            int c1 = std::min(p.w, (int)ceil(seg.x1 - p.ulx));

            Member mb;
            mb.piece = &p;
            mb.seg = &seg;
            mb.capped = p.group < 0 && seg.cut <= 0.0 && (b0 - p.uly > 96.0 || b1 - p.uly > 96.0);
            std::vector<double> samples;
            if (mb.capped) {
                for (int c = c0; c < c1; c++) {
                    double bc = seg.k + m * (p.ulx + c + 0.5);
                    int r = (int)floor(bc - 96.0) - p.uly - 1;
                    int run = 0;
                    while (r - run >= 0 && r - run < p.h && s.px[(size_t)(r - run) * p.w + c] != 0) {
                        run++;
                    }
                    if (run > 0) {
                        samples.push_back(run);
                    }
                }
                mb.depth = samples.empty() ? FP_BOX_MIN : std::min(FP_BOX_MAX, std::max(FP_BOX_MIN, fp_median(samples) / FP_KZ * nrm[2]));
            } else {
                for (int c = c0; c < c1; c++) {
                    if (!std::isnan(p.top[c])) {
                        samples.push_back(seg.k + m * (p.ulx + c + 0.5) - p.top[c]);
                    }
                }
                mb.depth = FP_BOX_THIN;
                seg.topPx = std::min(samples.empty() ? 1e9 : fp_median(samples), std::min(b0, b1) - p.uly);
                if (seg.cut > 0.0) {
                    seg.topPx = std::min(seg.topPx, seg.cut);
                }
            }

            // the centres of walkable hexes behind it, within its stretch
            double ux;
            double uz;
            double lpx;
            fp_axis_dir(seg.axis, &ux, &uz, &lpx);
            FpV3 g0 = fp_w(seg.x0, b0, 0.0);
            double len = (seg.x1 - seg.x0) * lpx;
            mb.guard = 1e9;
            int row0 = p.tile / 200;
            int col0 = p.tile % 200;
            for (int row = row0 - 4; row <= row0 + 4; row++) {
                for (int col = col0 - 4; col <= col0 + 4; col++) {
                    if (row < 0 || row >= 200 || col < 0 || col >= 200 || blocked[row * 200 + col]) {
                        continue;
                    }
                    int hx;
                    int hy;
                    fp_hex_top_left(row * 200 + col, &hx, &hy);
                    FpV3 c = fp_w(hx + 16, hy + 8, 0.0);
                    double dx = c.x - g0.x;
                    double dz = c.z - g0.z;
                    double along = dx * ux + dz * uz;
                    double behind = -(dx * nrm[0] + dz * nrm[2]);
                    if (along >= 0.0 && along <= len && behind >= 0.5) {
                        mb.guard = std::min(mb.guard, behind - FP_BOX_CLEAR);
                    }
                }
            }
            runs[seg.run].push_back(mb);
        }
    }

    for (auto& entry : runs) {
        std::vector<Member>& run = entry.second;
        std::vector<double> depths;
        std::vector<double> tops;
        double guard = 1e9;
        for (const Member& mb : run) {
            depths.push_back(mb.depth);
            guard = std::min(guard, mb.guard);
            if (!mb.capped && mb.seg->topPx < 1e8) {
                tops.push_back(mb.seg->topPx);
            }
        }
        double depth = std::min(fp_median(depths), guard); // a guard is never under 0.2
        double top = tops.empty() ? 0.0 : fp_median(tops);
        std::vector<Member*> order;
        for (Member& mb : run) {
            FpSeg& seg = *mb.seg;
            seg.depth = depth;
            if (!mb.capped) {
                seg.topPx = top > 0.0 ? std::min(top, seg.topPx) : seg.topPx;
            }
            seg.from0 = -1.0;
            seg.from1 = -1.0;
            seg.lo0 = 0.0;
            seg.lo1 = 0.0;
            order.push_back(&mb);
        }
        std::stable_sort(order.begin(), order.end(), [](const Member* a, const Member* b) { return a->seg->x0 < b->seg->x0; });

        std::vector<std::vector<Member*>> stretches;
        double reach = 0.0;
        for (Member* mb : order) {
            if (stretches.empty() || mb->seg->x0 > reach + 2.0 || mb->piece->door || stretches.back().back()->piece->door) {
                stretches.emplace_back();
                reach = mb->seg->x1;
            }
            stretches.back().push_back(mb);
            reach = std::max(reach, mb->seg->x1);
        }
        auto height = [](const Member* mb) {
            return mb->capped ? 96.0 : mb->seg->topPx;
        };
        for (std::vector<Member*>& st : stretches) {
            for (size_t i = 1; i < st.size(); i++) {
                Member* a = st[i - 1];
                Member* b = st[i];
                if (height(a) > height(b) + 2.0) {
                    a->seg->from1 = 0.0;
                    a->seg->lo1 = height(b);
                } else if (height(b) > height(a) + 2.0) {
                    b->seg->from0 = 0.0;
                    b->seg->lo0 = height(a);
                }
            }
            Member* first = st[0];
            Member* last = st[0];
            for (Member* mb : st) {
                if (mb->seg->x0 < first->seg->x0) {
                    first = mb;
                }
                if (mb->seg->x1 > last->seg->x1) {
                    last = mb;
                }
            }
            for (int e = 0; e < 2; e++) {
                FpSeg& seg = *(e == 0 ? first : last)->seg;
                double from = fp_end_cover(all, seg, e == 0 ? seg.x0 : seg.x1, depth);
                if (from >= depth - 0.02) {
                    from = -1.0;
                }
                (e == 0 ? seg.from0 : seg.from1) = from;
                (e == 0 ? seg.lo0 : seg.lo1) = 0.0;
            }
        }
    }
}

// A decoration whose lowest pixel is well above its hex hangs on something. Is it a wall? A wall line
// (a run's pieces together: a sign may straddle a seam) at or just behind its hex -- within a hex of
// it, the frontmost such line -- must stand under 90% of its columns: above the line's foot, no higher
// than the wall's 96 px. Returns that line, or false.
static bool fp_hang_wall(const std::vector<FpPiece>& pieces, const std::vector<int>& group, const std::vector<const FpPiece*>& walls, FpSeg* on)
{
    int minX = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    for (int id : group) {
        minX = std::min(minX, pieces[id].ulx);
        maxX = std::max(maxX, pieces[id].ulx + pieces[id].w);
    }
    std::vector<double> bottom(maxX - minX, std::numeric_limits<double>::quiet_NaN());
    std::vector<double> top(maxX - minX, std::numeric_limits<double>::quiet_NaN());
    for (int id : group) {
        const FpPiece& p = pieces[id];
        for (int c = 0; c < p.w; c++) {
            if (std::isnan(p.bottom[c])) {
                continue;
            }
            int x = p.ulx + c - minX;
            bottom[x] = std::isnan(bottom[x]) ? p.bottom[c] : std::max(bottom[x], p.bottom[c]);
            top[x] = std::isnan(top[x]) ? p.top[c] : std::min(top[x], p.top[c]);
        }
    }
    int cols = 0;
    for (double b : bottom) {
        cols += std::isnan(b) ? 0 : 1;
    }
    if (cols == 0) {
        return false;
    }
    int row0 = pieces[group[0]].tile / 200;
    int col0 = pieces[group[0]].tile % 200;
    double foot = -std::numeric_limits<double>::infinity();
    for (int id : group) {
        foot = std::max(foot, pieces[id].footY);
    }
    double xc = (minX + maxX) / 2.0;
    std::unordered_map<int, std::vector<const FpSeg*>> lines; // the wall lines near it, by run
    for (const FpPiece* w : walls) {
        if (abs(w->tile / 200 - row0) > 4 || abs(w->tile % 200 - col0) > 4) {
            continue;
        }
        for (const FpSeg& s : w->segs) {
            if (s.axis != 0 && !s.fore) {
                lines[s.run].push_back(&s);
            }
        }
    }
    bool found = false;
    double best = 0.0;
    for (auto& entry : lines) {
        const FpSeg* s0 = entry.second[0];
        double m = FP_SLOPES[s0->axis];
        double behind = foot - (s0->k + m * xc); // how far behind its hex the line runs, screen px
        if (behind < -4.0 || behind > 30.0) {
            continue;
        }
        int ok = 0;
        for (int c = 0; c < (int)bottom.size(); c++) {
            double x = minX + c + 0.5;
            if (std::isnan(bottom[c])) {
                continue;
            }
            for (const FpSeg* s : entry.second) {
                double g = s->k + FP_SLOPES[s->axis] * x;
                if (x >= s->x0 && x < s->x1 && g - bottom[c] >= 2.0 && g - top[c] <= 96.0) {
                    ok++;
                    break;
                }
            }
        }
        if (ok * 10 < cols * 9) {
            continue;
        }
        if (!found || behind < best) {
            found = true;
            best = behind;
            *on = *s0;
        }
    }
    return found;
}

// A cave's rock walls (art\walls\ca*.frm: Vault 13's cave, the rock round Mariposa and the Master's
// halls) -- and not the built walls of a vault or a base.
static std::unordered_map<int, bool> fp_rock_walls;

static bool fp_is_rock_wall(int fid)
{
    auto it = fp_rock_walls.find(fid);
    if (it != fp_rock_walls.end()) {
        return it->second;
    }
    bool rock = false;
    char* name = art_get_name(fid);
    if (name != NULL) {
        char* file = strrchr(name, '\\');
        file = file != NULL ? file + 1 : name;
        rock = (file[0] == 'c' || file[0] == 'C') && (file[1] == 'a' || file[1] == 'A');
    }
    fp_rock_walls[fid] = rock;
    return rock;
}

// A quad with the sprite laid on it as given (pix per corner), facing `outward`.
static bool fp_emit_affine(std::vector<FpQuad>& out, int sprite, const FpV3 c[4], const double pix[4][2], float light, const FpV3& outward)
{
    if (!fp_place(sprite)) {
        return false;
    }
    double ax = c[1].x - c[0].x, ay = c[1].y - c[0].y, az = c[1].z - c[0].z;
    double bx = c[2].x - c[0].x, by = c[2].y - c[0].y, bz = c[2].z - c[0].z;
    double nx = ay * bz - az * by;
    double ny = az * bx - ax * bz;
    double nz = ax * by - ay * bx;
    FpQuad quad;
    fp_quad_fill(&quad, FP_KIND_OPAQUE, fp_sprites[sprite], c, pix, light, 1.0f, nx * outward.x + ny * outward.y + nz * outward.z >= 0.0);
    out.push_back(quad);
    return true;
}

// THE CAVE'S ROCK AS SOLID COLUMNS (his pick, 2026-10-30, "kamenné sloupy samy"): standing up as flat
// cards, a cave's rock walls left holes between them -- the 2D art's slopes cover what a card does not
// -- and through the holes the eye saw far into the dark. Through his eyes each hex a rock wall
// stands on is a hexagonal column from the floor to the ceiling: outside, the wall's own picture where
// the original camera sees it (projective, as everywhere); inside, a hair smaller, plain rock (the
// cave's rock floor tile, tiled) wherever that picture is see-through. A side against another rock
// column is not drawn.
#define FP_HEX_R 0.5773502691896258 // a hex's corner from its centre, units (1 / sqrt(3))
#define FP_ROCK_ROWS 12 // the plain rock repeats this many times up a column (as fine as across it)

static bool fp_emit_rock_column(std::vector<FpQuad>& out, int tile, const std::vector<unsigned char>& rockHex, int wallSprite, int ulx, int uly, int rockSprite, float light)
{
    int hx;
    int hy;
    fp_hex_top_left(tile, &hx, &hy);
    FpV3 centre = fp_w(hx + 16, hy + 8, 0.0);
    const FpSprite* rs = rockSprite >= 0 ? &fp_sprites[rockSprite] : NULL;
    for (int k = 0; k < 6; k++) {
        int next = tile_num_in_direction(tile, k, 1);
        if (next >= 0 && next < HEX_GRID_SIZE && rockHex[next]) {
            continue;
        }
        double a = (k - 1) * FP_PI / 3.0; // rotation k looks (k - 1) * 60 degrees
        FpV3 outward = { cos(a), 0.0, sin(a) };
        for (int layer = 0; layer < 2; layer++) {
            double r = layer == 0 ? FP_HEX_R : FP_HEX_R * 0.96;
            FpV3 v0 = { centre.x + r * cos(a - FP_PI / 6.0), 0.0, centre.z + r * sin(a - FP_PI / 6.0) };
            FpV3 v1 = { centre.x + r * cos(a + FP_PI / 6.0), 0.0, centre.z + r * sin(a + FP_PI / 6.0) };
            if (layer == 0) {
                // a side facing east or west lies along the original camera's rays: its picture would be
                // one column of pixels smeared across it, so only the plain rock shows there
                if (fabs(sin(a)) < 0.5) {
                    continue;
                }
                FpV3 c[4] = { v0, v1, { v1.x, FP_H_ROOF, v1.z }, { v0.x, FP_H_ROOF, v0.z } };
                if (wallSprite >= 0 && !fp_emit_proj(out, FP_KIND_OPAQUE, wallSprite, ulx, uly, c, NULL, light, 1.0f, &outward)) {
                    return false;
                }
                continue;
            }
            if (rs == NULL) {
                continue;
            }
            // the tile's middle (its corners are see-through, interlocking with the next)
            double px0 = rs->w * 0.3;
            double px1 = rs->w * 0.7;
            double py0 = rs->h * 0.3;
            double py1 = rs->h * 0.7;
            for (int row = 0; row < FP_ROCK_ROWS; row++) {
                double y0 = FP_H_ROOF * row / FP_ROCK_ROWS;
                double y1 = FP_H_ROOF * (row + 1) / FP_ROCK_ROWS;
                FpV3 c[4] = { { v0.x, y0, v0.z }, { v1.x, y0, v1.z }, { v1.x, y1, v1.z }, { v0.x, y1, v0.z } };
                double pix[4][2] = { { px0, py1 }, { px1, py1 }, { px1, py0 }, { px0, py0 } };
                if (!fp_emit_affine(out, rockSprite, c, pix, light, outward)) {
                    return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The static scene of one elevation.
// ---------------------------------------------------------------------------

struct FpBoardPiece {
    int sprite;
    double dx0; // its top-left relative to the board's anchor, screen px
    double dy0;
    int tile; // its hex and flags, for its light
    int flags;
};

struct FpBoard {
    FpV3 anchor;
    std::vector<FpBoardPiece> pieces; // back to front
};

static std::vector<float> fp_static_verts;
static std::vector<int> fp_static_light_tiles; // per static vertex: the hex its light is read from, or -1
static std::vector<FpRange> fp_static_ranges;
static std::vector<FpBoard> fp_boards;
static uint64_t fp_static_sig = 0;
static uint64_t fp_light_sig = 0;
static int fp_static_elev = -1;
static bool fp_static_valid = false;
static bool fp_static_uploaded = false;
static bool fp_static_under = false; // the static scene is an underground elevation through his eyes
static std::vector<unsigned char> fp_static_kind; // underground: per square 0 = nothing, 1 = walkable, 2 = rock

// Through his eyes what hangs over him changes as he walks (fp_build_overhead): kept apart from the
// static scene, so walking in or out of a building rebuilds only this.
static std::vector<float> fp_overhead_verts;
// Diablo's roofs as the editor's mouse meets them (they are painted over everything, so a click on
// one picks nothing under it): each one's corners and the picture that is opaque where it is.
struct FpRoofPick {
    FpV3 c[4];
    int sprite;
    int ulx;
    int uly;
};
static std::vector<FpRoofPick> fp_roof_picks;
static std::vector<int> fp_overhead_light_tiles;
static std::vector<FpRange> fp_overhead_ranges;
static uint64_t fp_overhead_sig = 0;
static bool fp_overhead_valid = false;
static bool fp_overhead_uploaded = false;

static float fp_light_of(int elevation, int tile, int flags)
{
    if ((flags & OBJECT_FLAG_0xFC000) == OBJECT_TRANS_WALL) {
        return 1.0f;
    }
    if (tile < 0 || tile >= HEX_GRID_SIZE) {
        return 0.0f;
    }
    return (float)light_get_tile(elevation, tile) / 65536.0f;
}

// The hex an object's light follows, or -1 when it does not (a see-through wall is always lit).
static int fp_light_tile_of(int tile, int flags)
{
    if ((flags & OBJECT_FLAG_0xFC000) == OBJECT_TRANS_WALL || tile < 0 || tile >= HEX_GRID_SIZE) {
        return -1;
    }
    return tile;
}

// The objects of one type standing on `elevation` (obj_create_list: not hidden).
static void fp_collect(int elevation, int type, std::vector<Object*>& out)
{
    out.clear();
    if (art_get_disable(type)) {
        return;
    }
    Object** list = NULL;
    int count = obj_create_list(-1, elevation, type, &list);
    for (int i = 0; i < count; i++) {
        Object* obj = list[i];
        if (obj == NULL || obj == obj_dude || obj->tile < 0) {
            continue;
        }
        out.push_back(obj);
    }
    if (list != NULL) {
        obj_delete_list(list);
    }
}

static void fp_hash(uint64_t* h, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        *h ^= (value >> (i * 8)) & 0xFF;
        *h *= 1099511628211ull;
    }
}

// What the static scene was built from: the view, the elevation, every wall, scenery and item there
// (the roofs, which the game lifts and drops as the dude walks, are fp_build_overhead's). An object that is ANIMATING counts
// without its frame (a door swinging is one rebuild when it stops, not one per frame; something that
// animates forever stays on the frame it had).
// The 3D editor's revision (bumped on every edit) and whether he is editing (see THE 3D EDITOR).
static int fp_edit_rev = 0;
static bool fp_editing = false;

static uint64_t fp_signature(int mode, int elevation, const std::vector<Object*>& objects)
{
    uint64_t h = 1469598103934665603ull;
    fp_hash(&h, (uint32_t)mode);
    if (mode == FP_VIEW_DIABLO) {
        fp_hash(&h, (uint32_t)fp_edit_rev);
        fp_hash(&h, fp_editing ? 1u : 0u);
    }
    fp_hash(&h, (uint32_t)elevation);
    for (Object* obj : objects) {
        fp_hash(&h, (uint32_t)obj->pid);
        fp_hash(&h, (uint32_t)obj->tile);
        fp_hash(&h, (uint32_t)obj->fid);
        fp_hash(&h, (uint32_t)(anim_busy(obj) != 0 ? -1 : obj->frame));
        fp_hash(&h, (uint32_t)obj->rotation);
        fp_hash(&h, (uint32_t)obj->flags);
        fp_hash(&h, (uint32_t)obj->x);
        fp_hash(&h, (uint32_t)obj->y);
    }
    return h;
}

// Where the lights are: every object carrying one (the dude does, 4 hexes round him), and the
// ambient. When this changes, the static scene's light is read again (fp_relight_static).
static uint64_t fp_light_signature(const std::vector<Object*>& statics, const std::vector<Object*>& critters)
{
    uint64_t h = 1469598103934665603ull;
    fp_hash(&h, (uint32_t)light_get_ambient());
    auto add = [&](Object* obj) {
        if (obj != NULL && obj->lightDistance > 0 && obj->lightIntensity > 0) {
            fp_hash(&h, (uint32_t)obj->tile);
            fp_hash(&h, (uint32_t)obj->lightDistance);
            fp_hash(&h, (uint32_t)obj->lightIntensity);
        }
    };
    add(obj_dude);
    for (Object* obj : statics) {
        add(obj);
    }
    for (Object* obj : critters) {
        add(obj);
    }
    return h;
}

static void fp_relight(std::vector<float>& verts, const std::vector<int>& lightTiles, int elevation)
{
    size_t vertices = lightTiles.size();
    for (size_t i = 0; i < vertices; i++) {
        int tile = lightTiles[i];
        if (tile >= 0) {
            verts[i * FP_VERTEX_FLOATS + 9] = (float)light_get_tile(elevation, tile) / 65536.0f;
        }
    }
}

static void fp_relight_static(int elevation)
{
    fp_relight(fp_static_verts, fp_static_light_tiles, elevation);
    fp_static_uploaded = false;
    fp_relight(fp_overhead_verts, fp_overhead_light_tiles, elevation);
    fp_overhead_uploaded = false;
}

// One object's sprite on the original screen: its id and top-left (obj_render_object's placement).
// obj->x / obj->y is where its walk (and its animations' drift) has carried it from its hex: a
// displacement on the ground, whichever of its six sides is shown.
static bool fp_object_sprite(Object* obj, int dir, bool noShadow, int* sprite, int* ulx, int* uly, bool withOffset)
{
    int id = fp_sprite(obj->fid, obj->frame, dir, noShadow);
    if (id < 0) {
        return false;
    }
    const FpSprite& s = fp_sprites[id];
    int hx;
    int hy;
    fp_hex_top_left(obj->tile, &hx, &hy);
    int ax = hx + 16 + s.xOff + (withOffset ? obj->x : 0);
    int ay = hy + 8 + s.yOff + (withOffset ? obj->y : 0);
    *sprite = id;
    *ulx = ax - (s.w >> 1);
    *uly = ay - (s.h - 1);
    return true;
}

// The panel, its lid and its box (geometry.js levelQuads). `cut` (px, 0 = none): the panel ends that
// high, and with `lid` the rest of the sprite -- its top, seen from above -- lies flat there. The
// game's own angle cuts only the walls under a roof, at 96 px. Through his eyes (`boxes`) every
// wall is cut at 96 px with no lid -- that is above the eye, so all a lid could ever show is its
// underside, overhanging the box -- and a box-shaped object is cut at its own height, its top lying
// flat there; a box stands behind the panel, as deep as fp_box_runs (or the box fit) measured.
static bool fp_emit_panel(std::vector<FpQuad>& out, const FpPiece& p, const FpSeg& seg, double cut, bool lid, bool boxes)
{
    int axis = seg.axis;
    if (seg.cut > 0.0) {
        cut = seg.cut; // the part of an off-line piece in front of its chain (fp_snap_wall_runs)
        lid = false;
    }
    if (axis == 0) {
        cut = 0.0;
    }
    double m = seg.sloped ? seg.slope : FP_SLOPES[axis];
    double x = p.ulx;
    double y = p.uly;
    double b0 = seg.k + m * seg.x0;
    double b1 = seg.k + m * seg.x1;
    int kind = p.trans ? FP_KIND_TRANS : (p.rock ? FP_KIND_ROCK : FP_KIND_OPAQUE);
    float back = p.trans ? 0.55f : (axis == 0 ? 0.8f : 0.55f);
    double t0 = cut > 0.0 ? std::min(cut, b0 - y) : b0 - y;
    double t1 = cut > 0.0 ? std::min(cut, b1 - y) : b1 - y;
    FpV3 f[4] = { fp_w(seg.x0, b0, 0.0), fp_w(seg.x1, b1, 0.0), fp_w(seg.x1, b1 - t1, t1 / FP_KY), fp_w(seg.x0, b0 - t0, t0 / FP_KY) };
    if (!fp_emit_proj(out, kind, p.sprite, x, y, f, NULL, p.light, back)) {
        return false;
    }

    double capDepth = 0.0;
    bool capped = cut > 0.0 && (b0 - y > cut || b1 - y > cut);
    if (capped) {
        double back0 = std::min(y, b0 - cut);
        double back1 = std::min(y, b1 - cut);
        capDepth = ((b0 - cut - back0) + (b1 - cut - back1)) / 2.0 / FP_KZ;
        if (lid) {
            double h = cut / FP_KY;
            FpV3 c[4] = { fp_w(seg.x0, b0 - cut, h), fp_w(seg.x1, b1 - cut, h), fp_w(seg.x1, back1, h), fp_w(seg.x0, back0, h) };
            if (!fp_emit_proj(out, kind, p.sprite, x, y, c, NULL, p.light, back)) {
                return false;
            }
        }
    }

    if (p.trans || !boxes) {
        return true;
    }

    // A wall is a box, not a sheet; a scenery card gets depth too (half its width).
    const double* nrm = FP_WALL_NORMAL[axis];
    double t;
    if (seg.depth > 0.0) {
        t = seg.depth;
    } else if (axis != 0) {
        t = capDepth > 0.0 ? std::min(FP_CUBOID_MAX, std::max(FP_BOX_MIN, capDepth * nrm[2])) : FP_BOX_THIN;
    } else {
        t = std::min(1.0, std::max(0.2, ((seg.x1 - seg.x0) / FP_S) * 0.5));
    }
    FpV3 bk[4];
    for (int i = 0; i < 4; i++) {
        bk[i] = { f[i].x - nrm[0] * t, f[i].y, f[i].z - nrm[2] * t };
    }
    FpV3 s = { nrm[0] * t, 0.0, nrm[2] * t };
    FpV3 shiftAll[4] = { s, s, s, s };
    // the back is the front's mirror
    FpV3 away = { -nrm[0], 0.0, -nrm[2] };
    if (!fp_emit_proj(out, kind, p.sprite, x, y, bk, shiftAll, p.light, back, &away)) {
        return false;
    }

    // The top and the ends WRAP the front round the box's edges, as a Doom wall shows its material
    // round a corner: an end samples the front's columns going on in from that end, the deeper the
    // further in (one way: a tent that came back out to the edge mirrored the rock walls into a spine
    // down the middle; the seam it saved at the back corner is the lesser evil), and the top samples
    // the front's rows just under its top edge, there and back. (They used to sample one column, the
    // edge, smeared across the whole depth.)
    double ux;
    double uz;
    double lpx;
    fp_axis_dir(axis, &ux, &uz, &lpx);
    double eps = 0.5 * lpx; // half a sprite column: sample column centres, never the boundary
    double sig = std::min(1.0, std::max(0.0, ((seg.x1 - seg.x0) * lpx - 2.0 * eps) / t)); // never past the far end
    auto fold = [&](double d) {
        return std::min(d, t - d);
    };
    auto at = [&](const FpV3& v, double d) {
        return FpV3 { v.x - nrm[0] * d, v.y, v.z - nrm[2] * d };
    };
    // the band between depths d1 and d2 behind the front edge lo..hi, its corners sampling with s1 / s2
    auto band = [&](const FpV3& lo, const FpV3& hi, double d1, double d2, const FpV3& s1, const FpV3& s2, const FpV3& outward) {
        if (d2 - d1 < 1e-4) {
            return true;
        }
        FpV3 c[4] = { at(lo, d1), at(hi, d1), at(hi, d2), at(lo, d2) };
        FpV3 sh[4] = { s1, s1, s2, s2 };
        return fp_emit_proj(out, kind, p.sprite, x, y, c, sh, p.light, back, &outward);
    };

    // a cut panel's top is its lid (a wall's: above the eye); an uncut one has its box top at its
    // median height
    double hb = capped ? cut : (seg.topPx > 0.0 ? seg.topPx : std::min(t0, t1));
    if ((!capped || seg.cut > 0.0) && hb > 1.0) { // an off-line piece's front part is low: closed on top
        FpV3 lo = fp_w(seg.x0, b0 - hb, hb / FP_KY);
        FpV3 hi = fp_w(seg.x1, b1 - hb, hb / FP_KY);
        double ep = 0.5 / FP_KY;
        double kap = std::min(1.0, std::max(0.0, (hb / FP_KY - 2.0 * ep) / (t / 2.0)));
        auto shift = [&](double d) {
            return FpV3 { nrm[0] * d, -(ep + kap * fold(d)), nrm[2] * d };
        };
        FpV3 up = { 0.0, 1.0, 0.0 };
        if (!band(lo, hi, 0.0, t / 2.0, shift(0.0), shift(t / 2.0), up) || !band(lo, hi, t / 2.0, t, shift(t / 2.0), shift(t), up)) {
            return false;
        }
    }

    for (int e = 0; e < 2; e++) {
        double from = e == 0 ? seg.from0 : seg.from1;
        double xe = e == 0 ? seg.x0 : seg.x1;
        double be = e == 0 ? b0 : b1;
        double he = std::min(hb, e == 0 ? t0 : t1);
        double hl = std::max(0.0, e == 0 ? seg.lo0 : seg.lo1); // a step's side starts at the lower neighbour's top
        if (from < 0.0 || he <= hl + 0.5) {
            continue;
        }
        double dir = e == 0 ? 1.0 : -1.0;
        FpV3 lo = fp_w(xe, be - hl, hl / FP_KY);
        FpV3 hi = fp_w(xe, be - he, he / FP_KY);
        auto shift = [&](double d) {
            double a = dir * (eps + sig * d);
            return FpV3 { nrm[0] * d + a * ux, 0.0, nrm[2] * d + a * uz };
        };
        double mid = std::max(from, t / 2.0);
        FpV3 side = { -dir * ux, 0.0, -dir * uz }; // end0 faces back along the wall, end1 on along it
        if (!band(lo, hi, from, mid, shift(from), shift(mid), side) || !band(lo, hi, mid, t, shift(mid), shift(t), side)) {
            return false;
        }
    }
    return true;
}

static void fp_flush_quads(std::vector<FpQuad>& quads, std::vector<float>& verts, std::vector<FpRange>& ranges, std::vector<int>* lightTiles)
{
    std::stable_sort(quads.begin(), quads.end(), [](const FpQuad& a, const FpQuad& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.page < b.page;
    });
    verts.clear();
    ranges.clear();
    verts.reserve(quads.size() * 6 * FP_VERTEX_FLOATS);
    if (lightTiles != NULL) {
        lightTiles->clear();
        lightTiles->reserve(quads.size() * 6);
    }
    for (const FpQuad& q : quads) {
        int first = (int)(verts.size() / FP_VERTEX_FLOATS);
        if (ranges.empty() || ranges.back().kind != q.kind || ranges.back().page != q.page) {
            ranges.push_back({ q.kind, q.page, first, 0 });
        }
        verts.insert(verts.end(), q.v, q.v + 6 * FP_VERTEX_FLOATS);
        ranges.back().count += 6;
        if (lightTiles != NULL) {
            lightTiles->insert(lightTiles->end(), q.lightTiles, q.lightTiles + 6);
        }
    }
}

// Is there a roof over the ground point (px, py) (build.js roofAt: the square under it and its
// neighbours)? Diablo's view cuts a wall at 96 px only where it stands under one.
// Fallout paints the rock round underground rooms as near-black speckle (cav4008..4010): in 2D a dark
// sea round the rooms, through his eyes an endless noisy plain -- and as roof tiles the Master's
// "starry" ceiling. Told by the art itself: dark (mean r+g+b under 24 of 189, 6-bit) and grainy
// (neighbours differ by more than 11 on average); the nearest ordinary floors are 27 / 12.0 and
// 17 / 3.5.
static std::unordered_map<int, bool> fp_rock_tiles;

static bool fp_tile_is_rock(int id)
{
    auto it = fp_rock_tiles.find(id);
    if (it != fp_rock_tiles.end()) {
        return it->second;
    }
    bool rock = false;
    CacheEntry* handle;
    Art* art = art_ptr_lock(art_id(OBJ_TYPE_TILE, id, 0, 0, 0), &handle);
    if (art != NULL) {
        int w = art_frame_width(art, 0, 0);
        int h = art_frame_length(art, 0, 0);
        unsigned char* data = art_frame_data(art, 0, 0);
        auto lum = [](unsigned char c) {
            return cmap[c * 3] + cmap[c * 3 + 1] + cmap[c * 3 + 2];
        };
        long sum = 0;
        long n = 0;
        long step = 0;
        long steps = 0;
        for (int y = 0; data != NULL && y < h; y++) {
            for (int x = 0; x < w; x++) {
                unsigned char c = data[y * w + x];
                if (c == 0 || !mappedColor[c]) {
                    continue;
                }
                int l = lum(c);
                sum += l;
                n++;
                unsigned char d = x + 1 < w ? data[y * w + x + 1] : 0;
                if (d != 0 && mappedColor[d]) {
                    step += abs(l - lum(d));
                    steps++;
                }
            }
        }
        rock = n > 0 && steps > 0 && sum < 24 * n && step > 11 * steps;
        art_ptr_unlock(handle);
    }
    fp_rock_tiles[id] = rock;
    return rock;
}

// Underground, what is floor is what he can walk to: the hexes reachable from his own through
// everything that does not block (doors count as passages), and the squares they lie in -- plus every
// square touching one, which reaches under the walls' feet. kind[sq] = 1 for those (floor and
// ceiling), 0 for the rest. (Telling floor from fill by the tile -- rock or not -- hid the floor of
// every cave, which IS that rock: black holes at Vault 13's door.) False when he is not on it.
static bool fp_reach_squares(int elevation, const std::vector<Object*>& objects, std::vector<unsigned char>& kind)
{
    if (obj_dude == NULL || obj_dude->elevation != elevation || obj_dude->tile < 0 || obj_dude->tile >= HEX_GRID_SIZE) {
        return false;
    }
    std::vector<unsigned char> blocked;
    fp_blocked_hexes(objects, blocked);
    std::vector<unsigned char> seen(HEX_GRID_SIZE, 0);
    std::vector<int> queue;
    queue.reserve(HEX_GRID_SIZE / 4);
    queue.push_back(obj_dude->tile);
    seen[obj_dude->tile] = 1;
    std::vector<unsigned char> walk(SQUARE_GRID_SIZE, 0);
    for (size_t i = 0; i < queue.size(); i++) {
        int tile = queue[i];
        walk[(tile / 200 / 2) * 100 + (tile % 200) / 2] = 1;
        for (int dir = 0; dir < 6; dir++) {
            int next = tile_num_in_direction(tile, dir, 1);
            if (next >= 0 && next < HEX_GRID_SIZE && !seen[next] && !blocked[next]) {
                seen[next] = 1;
                queue.push_back(next);
            }
        }
    }
    kind.assign(SQUARE_GRID_SIZE, 0);
    for (int sq = 0; sq < SQUARE_GRID_SIZE; sq++) {
        int row = sq / 100;
        int col = sq % 100;
        for (int dr = -1; dr <= 1 && kind[sq] == 0; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                int r = row + dr;
                int c = col + dc;
                if (r >= 0 && r < 100 && c >= 0 && c < 100 && walk[r * 100 + c]) {
                    kind[sq] = 1;
                    break;
                }
            }
        }
    }
#ifndef FALLOUT_RELEASE
    // FALLOUT_FP_REACH_DUMP=<file.pgm> (development): the hexes, 255 reached, 96 blocked, 0 the rest
    const char* dump = getenv("FALLOUT_FP_REACH_DUMP");
    if (dump != NULL && dump[0] != '\0') {
        FILE* stream = fopen(dump, "wb");
        if (stream != NULL) {
            fprintf(stream, "P5\n200 200\n255\n");
            for (int tile = 0; tile < HEX_GRID_SIZE; tile++) {
                fputc(seen[tile] ? 255 : (blocked[tile] ? 96 : 0), stream);
            }
            fclose(stream);
        }
    }
    trace_event(TRACE_FLOW, "fp-reach", "hexes=%d from=%d", (int)queue.size(), obj_dude->tile);
#endif
    return true;
}

// An elevation is underground when rock is most of the floor it draws (vaults, bases, caves:
// 0.61-0.95; outdoors at most 0.33, Mariposa's cliffs, and mostly none). F1's maps carry no flag.
static bool fp_underground(int elevation)
{
    TileData* squares = square[elevation];
    if (squares == NULL) {
        return false;
    }
    int drawn = 0;
    int rock = 0;
    for (int sq = 0; sq < SQUARE_GRID_SIZE; sq++) {
        int floor = squares->field_0[sq] & 0xFFFF;
        if (((floor >> 12) & 1) != 0 || (floor & 0xFFF) == 1) {
            continue;
        }
        drawn++;
        rock += fp_tile_is_rock(floor & 0xFFF) ? 1 : 0;
    }
    return drawn > 0 && rock * 2 > drawn;
}

static bool fp_roof_at(int elevation, double px, double py)
{
    if (square[elevation] == NULL) {
        return false;
    }
    // continuous square coordinates of a ground point (inverse of fp_square_top_left)
    double dx = px + 16.0;
    double dy = py - 10.0;
    double u = (24.0 * dx - 32.0 * dy) / 1536.0;
    double v = (12.0 * dx + 48.0 * dy) / 1536.0;
    for (int du = -1; du <= 1; du++) {
        for (int dv = -1; dv <= 1; dv++) {
            int col = (int)floor(u) + du;
            int row = (int)floor(v) + dv;
            if (col < 0 || col > 99 || row < 0 || row > 99) {
                continue;
            }
            int roof = (square[elevation]->field_0[row * 100 + (99 - col)] >> 16) & 0xFFF;
            if (roof != 0 && roof != 1) {
                return true;
            }
        }
    }
    return false;
}

// The roofs, 96 px up. From Diablo's camera they go where the game lifts them. Through his eyes it is
// the other way round: only the roof the game has lifted over him is his ceiling (any other roof is
// above the eye, and all he could see of it is its underside where it overhangs its walls, a band of
// grazing pixels over every building); underground, every walkable square without such a roof gets
// its floor's own picture 96 px up, as Doom hung a flat overhead, dimmed as an underside -- the rock
// lids over rooms (the Master's hall) are left out. Apart from the static scene, so walking under a
// roof or out from one rebuilds only this. False when the atlas ran out.
static bool fp_build_overhead(int mode, int elevation)
{
    bool eyes = mode == FP_VIEW_EYES;
    std::vector<FpQuad> quads;
    TileData* squares = square[elevation];
    bool under = fp_static_under && (int)fp_static_kind.size() == SQUARE_GRID_SIZE;
    int roofs = 0;
    int ceilings = 0;
    fp_roof_picks.clear();
    for (int sq = 0; squares != NULL && sq < SQUARE_GRID_SIZE; sq++) {
        int v = squares->field_0[sq];
        int floor = v & 0xFFFF;
        int roof = (v >> 16) & 0xFFFF;
        int tx;
        int ty;
        fp_square_top_left(sq, &tx, &ty);
        int ry = ty - 96;
        // From Diablo's camera a roof covers the tops of the walls under it, as the game paints it over
        // them; their lids lie at the same 96 px, so the roof sits a hair higher, slid along the
        // original camera's ray (the same pixels from there) -- a tie in depth would be a coin toss.
        double h = eyes ? FP_H_ROOF : FP_H_ROOF + 0.01;
        FpV3 c[4] = { fp_w(tx, ry, h), fp_w(tx + 80, ry, h), fp_w(tx + 80, ry + 36, h), fp_w(tx, ry + 36, h) };
        bool lifted = ((roof >> 12) & 1) != 0;
        bool roofArt = (roof & 0xFFF) != 0 && (roof & 0xFFF) != 1 && !(under && fp_tile_is_rock(roof & 0xFFF));
        if (roofArt && lifted == eyes) {
            int id = fp_sprite(art_id(OBJ_TYPE_TILE, roof & 0xFFF, 0, 0, 0), 0, 0, false);
            // roofs are lit by the ambient light alone
            fp_emit_light_tile = -1;
            if (id >= 0 && !fp_emit_proj(quads, FP_KIND_OPAQUE, id, tx, ry, c, NULL, 0.0f, 1.0f)) {
                return false;
            }
            if (id >= 0 && !eyes) {
                FpRoofPick pick = { { c[0], c[1], c[2], c[3] }, id, tx, ry };
                fp_roof_picks.push_back(pick);
            }
            roofs += id >= 0 ? 1 : 0;
        } else if (eyes && under && fp_static_kind[sq] == 1) {
            // the floor's own picture straight above it (the mapping a roof uses)
            int id = fp_sprite(art_id(OBJ_TYPE_TILE, floor & 0xFFF, 0, 0, 0), 0, 0, false);
            int lightTile = 400 * (sq / 100) + 2 * (sq % 100) + 1;
            float light = lightTile < HEX_GRID_SIZE ? (float)light_get_tile(elevation, lightTile) / 65536.0f : 0.0f;
            fp_emit_light_tile = lightTile < HEX_GRID_SIZE ? lightTile : -1;
            if (id >= 0 && !fp_emit_proj(quads, FP_KIND_OPAQUE, id, tx, ry, c, NULL, light, 0.55f)) {
                return false;
            }
            ceilings += id >= 0 ? 1 : 0;
        }
    }
    fp_emit_light_tile = -1;
    fp_flush_quads(quads, fp_overhead_verts, fp_overhead_ranges, &fp_overhead_light_tiles);
    fp_overhead_uploaded = false;
    trace_event(TRACE_FLOW, "fp-overhead", "mode=%d elev=%d roofs=%d ceilings=%d", mode, elevation, roofs, ceilings);
    return true;
}

// The overhead depends on the static scene (its elevation, its underground squares) and on which roofs
// the game has lifted.
static uint64_t fp_overhead_signature(int elevation)
{
    uint64_t h = fp_static_sig;
    fp_hash(&h, 0x0f0f0f0fu);
    if (square[elevation] != NULL) {
        for (int sq = 0; sq < SQUARE_GRID_SIZE; sq++) {
            if ((((square[elevation]->field_0[sq] >> 16) >> 12) & 1) != 0) {
                fp_hash(&h, (uint32_t)sq);
            }
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// THE 3D EDITOR (his "zkusíme udělat editor a já to budu moci upravit", 2026-10-31; his four tools).
// From Diablo's camera (F8) Shift+F8 pauses the game and lets him pick a wall, a rock or a piece of
// scenery with the mouse and: push it nearer or farther (the wheel) -- along the game camera's rays,
// so from the game's own angle it stays exactly where it was drawn; turn it (Q / E); hide it from the
// 3D (H); copy it (C) and move the copy (the arrows) to cover a gap. Kept per map in
// fp3d\<MAP>.txt next to the game, and read back whenever the map is shown.
// ---------------------------------------------------------------------------

struct FpEdit {
    int elevation;
    int tile;
    int fid;
    int copy; // -1 = the object itself
    int dx; // a copy's shift on the original screen, px
    int dy;
    double push; // px of its ground line nearer the camera (negative: farther)
    double rot; // degrees it is turned about its corner (else its middle)
    bool hide;
};

struct FpPick {
    int elevation;
    int tile;
    int fid;
    int copy;
    FpV3 c[4];
    int sprite;
    int ulx;
    int uly;
    FpQuad quad;
    bool hidden;
};

static std::vector<FpEdit> fp_edits;
static std::vector<std::vector<FpEdit>> fp_edit_undo;
static std::vector<FpPick> fp_picks;
static char fp_edits_map[32] = "";
static bool fp_edit_selected = false;
static FpEdit fp_edit_sel; // the key of the picked piece (elevation, tile, fid, copy)
static int fp_edit_view_w = 0;
static int fp_edit_view_h = 0;

static void fp_edits_path(const char* map, char* path, size_t size)
{
    char name[32];
    snprintf(name, sizeof(name), "%s", map);
    char* dot = strrchr(name, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    snprintf(path, size, "fp3d\\%s.txt", name);
}

static void fp_edits_save()
{
    if (fp_edits_map[0] == '\0') {
        return;
    }
    compat_mkdir("fp3d");
    char path[64];
    fp_edits_path(fp_edits_map, path, sizeof(path));
    FILE* stream = compat_fopen(path, "wt");
    if (stream == NULL) {
        trace_event(TRACE_WARN, "fp-edit", "cannot write %s", path);
        return;
    }
    fprintf(stream, "# Fallout 1 Realtime, the 3D view's edits (F8, Shift+F8) for %s\n", fp_edits_map);
    fprintf(stream, "# elevation tile fid copy dx dy push rot hide\n");
    for (const FpEdit& e : fp_edits) {
        fprintf(stream, "%d %d %x %d %d %d %.1f %.1f %d\n", e.elevation, e.tile, e.fid, e.copy, e.dx, e.dy, e.push, e.rot, e.hide ? 1 : 0);
    }
    fclose(stream);
}

// The map shown now: its edits, read when it changes.
static void fp_edits_follow_map()
{
    if (strcmp(map_data.name, fp_edits_map) == 0) {
        return;
    }
    snprintf(fp_edits_map, sizeof(fp_edits_map), "%s", map_data.name);
    fp_edits.clear();
    fp_edit_undo.clear();
    fp_edit_selected = false;
    fp_edit_rev++;
    char path[64];
    fp_edits_path(fp_edits_map, path, sizeof(path));
    FILE* stream = compat_fopen(path, "rt");
    if (stream == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), stream) != NULL) {
        if (line[0] == '#') {
            continue;
        }
        FpEdit e = {};
        int hide = 0;
        if (sscanf(line, "%d %d %x %d %d %d %lf %lf %d", &e.elevation, &e.tile, &e.fid, &e.copy, &e.dx, &e.dy, &e.push, &e.rot, &hide) == 9) {
            e.hide = hide != 0;
            fp_edits.push_back(e);
        }
    }
    fclose(stream);
    trace_event(TRACE_FLOW, "fp-edit", "map=%s edits=%d", fp_edits_map, (int)fp_edits.size());
}

static FpEdit* fp_edit_find(int elevation, int tile, int fid, int copy)
{
    for (FpEdit& e : fp_edits) {
        if (e.elevation == elevation && e.tile == tile && e.fid == fid && e.copy == copy) {
            return &e;
        }
    }
    return NULL;
}

// Turn a piece on the ground about one pivot -- the point where its faces meet, else the middle of its
// first face, so a corner stays a corner -- lift it as one under all of its pixels, then push it along
// the game camera's rays.
static void fp_edit_shape(FpPiece& p, const FpEdit& e)
{
    if (e.rot != 0.0 && !p.segs.empty()) {
        const FpSeg& first = p.segs[0];
        double m0 = first.sloped ? first.slope : FP_SLOPES[first.axis];
        // a corner: the next line starts where the first ends (a front part's twin lies over it instead)
        bool corner = p.segs.size() >= 2 && fabs(p.segs[1].x0 - first.x1) < 1.0;
        double px = corner ? first.x1 : (first.x0 + first.x1) / 2.0;
        double py = first.k + m0 * px;
        double a = e.rot * FP_PI / 180.0;
        // a screen point on the ground, turned about the pivot on the ground (world x = sx / 32, z = sy / KZ)
        auto turn = [&](double sx, double sy, double* ox, double* oy) {
            double wx = (sx - px) / FP_S;
            double wz = (sy - py) / FP_KZ;
            *ox = px + FP_S * (wx * cos(a) - wz * sin(a));
            *oy = py + FP_KZ * (wx * sin(a) + wz * cos(a));
        };
        for (FpSeg& s : p.segs) {
            double m = s.sloped ? s.slope : FP_SLOPES[s.axis];
            double ax;
            double ay;
            double bx;
            double by;
            turn(s.x0, s.k + m * s.x0, &ax, &ay);
            turn(s.x1, s.k + m * s.x1, &bx, &by);
            double run = bx - ax;
            if (fabs(run) < 0.5) {
                run = run < 0.0 ? -0.5 : 0.5; // never along the camera's rays: one column smeared
            }
            double m2 = std::max(-6.0, std::min(6.0, (by - ay) / run));
            s.sloped = true;
            s.slope = m2;
            s.k = ay - m2 * ax;
        }
        // under every pixel, as a fitted line is: the whole piece lifted by what its worst column needs
        double lift = 0.0;
        for (const FpSeg& s : p.segs) {
            for (int c = std::max(0, (int)floor(s.x0 - p.ulx)); c < std::min(p.w, (int)ceil(s.x1 - p.ulx)); c++) {
                if (!std::isnan(p.bottom[c])) {
                    lift = std::max(lift, p.bottom[c] - (s.k + s.slope * (p.ulx + c + 0.5)));
                }
            }
        }
        for (FpSeg& s : p.segs) {
            s.k += lift;
        }
    }
    for (FpSeg& s : p.segs) {
        s.k += e.push;
    }
}

// From Diablo's camera, the edits on this elevation's pieces: shaped, hidden, and the copies added.
static int fp_apply_edits(std::vector<FpPiece>& pieces, int elevation)
{
    int applied = 0;
    std::vector<FpPiece> copies;
    for (FpPiece& p : pieces) {
        if (p.trans && p.segs.empty()) {
            continue;
        }
        const FpEdit* own = NULL;
        bool any = false;
        for (const FpEdit& e : fp_edits) {
            if (e.elevation == elevation && e.tile == p.tile && e.fid == p.fid) {
                any = true;
                if (e.copy < 0) {
                    own = &e;
                }
            }
        }
        if (!any) {
            continue;
        }
        const FpPiece base = p; // the copies start from the piece as the game has it
        if (own != NULL) {
            fp_edit_shape(p, *own);
            p.edited = true;
            p.hiddenEdit = own->hide;
            applied++;
        }
        for (const FpEdit& e : fp_edits) {
            if (e.elevation != elevation || e.tile != base.tile || e.fid != base.fid || e.copy < 0) {
                continue;
            }
            // a copy: the same picture shifted on the original screen, then shaped
            FpPiece q = base;
            q.copy = e.copy;
            q.ulx += e.dx;
            q.uly += e.dy;
            q.lowest += e.dy;
            for (double& b : q.bottom) {
                b += e.dy;
            }
            for (double& b : q.top) {
                b += e.dy;
            }
            for (FpSeg& s : q.segs) {
                // every line moves alike, so a front part's cut height still holds
                double m = s.sloped ? s.slope : FP_SLOPES[s.axis];
                s.x0 += e.dx;
                s.x1 += e.dx;
                s.k += e.dy - m * e.dx;
                s.chainK += e.dy - m * e.dx;
            }
            fp_edit_shape(q, e);
            q.edited = true;
            q.hiddenEdit = e.hide;
            copies.push_back(std::move(q));
            applied++;
        }
    }
    for (FpPiece& q : copies) {
        pieces.push_back(std::move(q));
    }
    return applied;
}

// Build the floors, roofs, decals, walls, scenery and boards of `elevation` for view `mode`. False when
// the atlas ran out (the caller resets it and tries again).
static bool fp_build_static(int mode, int elevation, const std::vector<Object*>& objects)
{
    bool eyes = mode == FP_VIEW_EYES;
    Uint64 started = SDL_GetPerformanceCounter();
    std::vector<FpQuad> quads;
    quads.reserve(20000);
    fp_boards.clear();
    int floors = 0;

    // Squares: the whole 80x36 floor rect (its transparency interlocks the tiles; clipping left
    // seams); the roofs are fp_build_overhead's.
    // Underground through his eyes (fp_underground) the rock round the rooms is no floor to look
    // across: only the squares he can walk to, and those touching them, are drawn (fp_reach_squares).
    TileData* squares = square[elevation];
    bool under = eyes && fp_underground(elevation);
    std::vector<unsigned char>& kind = fp_static_kind; // underground: 1 = his floor (and ceiling), 0 = not
    kind.clear();
    if (under && !fp_reach_squares(elevation, objects, kind)) {
        under = false; // he is not on it: draw it as any other
    }
    fp_static_under = under;
    for (int sq = 0; squares != NULL && sq < SQUARE_GRID_SIZE; sq++) {
        int floor = squares->field_0[sq] & 0xFFFF;
        int tx;
        int ty;
        fp_square_top_left(sq, &tx, &ty);
        // the hex under the square, for its light (2D interpolates ten; one is enough for a first look)
        int lightTile = 400 * (sq / 100) + 2 * (sq % 100) + 1;
        float light = lightTile < HEX_GRID_SIZE ? (float)light_get_tile(elevation, lightTile) / 65536.0f : 0.0f;
        bool drawn = ((floor >> 12) & 1) == 0 && (floor & 0xFFF) != 1 && (!under || kind[sq] == 1);
        int floorId = -1;
        if (drawn) {
            floorId = fp_sprite(art_id(OBJ_TYPE_TILE, floor & 0xFFF, 0, 0, 0), 0, 0, false);
            if (floorId >= 0) {
                FpV3 c[4] = { fp_w(tx, ty, 0.0), fp_w(tx + 80, ty, 0.0), fp_w(tx + 80, ty + 36, 0.0), fp_w(tx, ty + 36, 0.0) };
                int cornerTiles[4];
                float cornerLights[4];
                static const int CX[4] = { 0, 80, 80, 0 };
                static const int CY[4] = { 0, 0, 36, 36 };
                for (int k = 0; k < 4; k++) {
                    cornerTiles[k] = fp_hex_at(tx + CX[k], ty + CY[k]);
                    cornerLights[k] = cornerTiles[k] >= 0 ? (float)light_get_tile(elevation, cornerTiles[k]) / 65536.0f : light;
                }
                fp_emit_corner_tiles = cornerTiles;
                fp_emit_corner_lights = cornerLights;
                bool ok = fp_emit_proj(quads, FP_KIND_OPAQUE, floorId, tx, ty, c, NULL, light, 1.0f);
                fp_emit_corner_tiles = NULL;
                fp_emit_corner_lights = NULL;
                if (!ok) {
                    return false;
                }
                floors++;
            }
        }
    }

    // Objects: flat ones become decals, the standing ones pieces on fitted ground lines -- through his
    // eyes a cave's rock walls columns (fp_emit_rock_column), gathered first: a column's side against
    // another is not drawn.
    std::vector<FpPiece> pieces;
    pieces.reserve(objects.size());
    int decalOrder = 0;
    struct FpRockWall {
        Object* obj;
        int sprite;
        int ulx;
        int uly;
    };
    std::vector<FpRockWall> rockWalls;
    std::vector<unsigned char> rockHex;
    if (eyes) {
        rockHex.assign(HEX_GRID_SIZE, 0);
        for (Object* obj : objects) {
            if (FID_TYPE(obj->fid) == OBJ_TYPE_WALL && obj->tile >= 0 && obj->tile < HEX_GRID_SIZE && fp_is_rock_wall(obj->fid)) {
                rockHex[obj->tile] = 1;
            }
        }
    }
    for (Object* obj : objects) {
        int type = FID_TYPE(obj->fid);
        int subtype = -1;
        int ext = 0;
        Proto* proto;
        if (proto_ptr(obj->pid, &proto) == 0) {
            ext = proto->extendedFlags;
            if (type == OBJ_TYPE_SCENERY) {
                subtype = proto->scenery.type;
            } else if (type == OBJ_TYPE_ITEM) {
                subtype = proto->item.type;
            }
        }

        int rot = std::min(std::max(obj->rotation, 0), 5);
        int sprite;
        int ulx;
        int uly;
        if (!fp_object_sprite(obj, rot, false, &sprite, &ulx, &uly, true)) {
            continue;
        }
        const FpSprite& s = fp_sprites[sprite];
        float light = fp_light_of(elevation, obj->tile, obj->flags);
        bool trans = (obj->flags & OBJECT_FLAG_0xFC000) != 0 && (obj->flags & OBJECT_TRANS_NONE) == 0;
        bool flat = (obj->flags & OBJECT_FLAT) != 0 || (type == OBJ_TYPE_ITEM && subtype != ITEM_TYPE_CONTAINER);
        if (eyes && type == OBJ_TYPE_WALL && obj->tile >= 0 && obj->tile < HEX_GRID_SIZE && rockHex[obj->tile]) {
            rockWalls.push_back({ obj, sprite, ulx, uly });
            continue;
        }

        if (flat) {
            double lift = 0.01 + decalOrder++ * 0.0002;
            FpV3 c[4] = { fp_w(ulx, uly, lift), fp_w(ulx + s.w, uly, lift), fp_w(ulx + s.w, uly + s.h, lift), fp_w(ulx, uly + s.h, lift) };
            fp_emit_light_tile = fp_light_tile_of(obj->tile, obj->flags);
            if (!fp_emit_proj(quads, FP_KIND_DECAL, sprite, ulx, uly, c, NULL, light, 1.0f)) {
                return false;
            }
            continue;
        }

        FpPiece p;
        p.sprite = sprite;
        p.ulx = ulx;
        p.uly = uly;
        p.w = s.w;
        p.h = s.h;
        p.tile = obj->tile;
        int hx;
        int hy;
        fp_hex_top_left(obj->tile, &hx, &hy);
        p.anchorY = hy + 8;
        p.footY = hy + 8 + obj->y;
        p.trans = trans;
        p.door = type == OBJ_TYPE_SCENERY && subtype == SCENERY_TYPE_DOOR;
        p.light = light;
        p.lightTile = fp_light_tile_of(obj->tile, obj->flags);
        p.flags = obj->flags;
        p.group = -1;
        p.boarded = false;
        p.cuboid = false;
        p.closed = false;
        p.cutPx = 0.0;
        p.hung = false;
        p.fid = obj->fid;
        p.copy = -1;
        p.edited = false;
        p.hiddenEdit = false;
        p.rock = type == OBJ_TYPE_WALL && fp_is_rock_wall(obj->fid);
        p.bottom.assign(s.w, std::numeric_limits<double>::quiet_NaN());
        p.top.assign(s.w, std::numeric_limits<double>::quiet_NaN());
        p.lowest = -std::numeric_limits<double>::infinity();
        for (int c = 0; c < s.w; c++) {
            for (int r = s.h - 1; r >= 0; r--) {
                if (s.px[(size_t)r * s.w + c] != 0) {
                    p.bottom[c] = uly + r + 1;
                    p.lowest = std::max(p.lowest, p.bottom[c]);
                    break;
                }
            }
            for (int r = 0; r < s.h; r++) {
                if (s.px[(size_t)r * s.w + c] != 0) {
                    p.top[c] = uly + r;
                    break;
                }
            }
        }
        if (!std::isfinite(p.lowest)) {
            continue; // a frame with no opaque pixel
        }
        for (int a = 0; a < 3; a++) {
            fp_table_build(&p.tables[a], p.bottom, ulx, FP_SLOPES[a]);
        }

        // A straight wall's proto names its axis (light-type bits: none = NORTH_SOUTH = axis b,
        // 0x08000000 = EAST_WEST = axis a), and that is final -- only a clearly better corner fit
        // overrides it. Corners and doors: the shape decides.
        bool wallLike = type == OBJ_TYPE_WALL || p.door;
        if (wallLike) {
            static const int AXES[2] = { FP_AXIS_A, FP_AXIS_B };
            int light5 = (int)(((unsigned int)ext >> 27) & 0x1F);
            int flagged = type == OBJ_TYPE_WALL && (light5 == 0 || light5 == 1) ? (light5 == 0 ? FP_AXIS_B : FP_AXIS_A) : 0;
            if (flagged != 0) {
                FpFit straight = fp_fit_ground(p.bottom, ulx, &flagged, 1, false, 0);
                p.segs = straight.segs;
                if (s.w >= 24) {
                    FpFit corner = fp_fit_ground(p.bottom, ulx, AXES, 2, true, 0);
                    if (corner.segs.size() == 2 && corner.cost < straight.cost * 0.3) {
                        p.segs = corner.segs;
                    }
                }
            } else {
                // A door far wider than any doorway -- Vault 13's gear doors with their frame and steps
                // (151, 189 px) -- is one face along one axis: the steps painted before it made a
                // corner fit stand it as a slab leaning across the cave.
                bool wide = p.door && s.w > FP_DOOR_MAX_W;
                p.segs = fp_fit_ground(p.bottom, ulx, AXES, 2, !wide, 0).segs;
            }
        }
        pieces.push_back(std::move(p));
    }

    // The rock columns, in the cave's own rock: the first rock floor tile of the elevation, else the
    // floor under each column.
    int rockColumns = 0;
    if (!rockWalls.empty()) {
        int rockTile = -1;
        for (int sq = 0; squares != NULL && sq < SQUARE_GRID_SIZE && rockTile < 0; sq++) {
            int floor = squares->field_0[sq] & 0xFFFF;
            if (((floor >> 12) & 1) == 0 && (floor & 0xFFF) != 1 && fp_tile_is_rock(floor & 0xFFF)) {
                rockTile = floor & 0xFFF;
            }
        }
        std::vector<unsigned char> columnDone(HEX_GRID_SIZE, 0);
        for (const FpRockWall& rw : rockWalls) {
            if (columnDone[rw.obj->tile]) {
                continue; // one column a hex, whatever stands on it
            }
            columnDone[rw.obj->tile] = 1;
            int tileId = rockTile;
            if (tileId < 0 && squares != NULL) {
                int sq = (rw.obj->tile / 200 / 2) * 100 + (rw.obj->tile % 200) / 2;
                int floor = squares->field_0[sq] & 0xFFFF;
                tileId = (floor & 0xFFF) != 1 ? floor & 0xFFF : -1;
            }
            int rockSprite = tileId >= 0 ? fp_sprite(art_id(OBJ_TYPE_TILE, tileId, 0, 0, 0), 0, 0, false) : -1;
            fp_emit_light_tile = fp_light_tile_of(rw.obj->tile, rw.obj->flags);
            fp_emit_egg = true;
            bool columnOk = fp_emit_rock_column(quads, rw.obj->tile, rockHex, rw.sprite, rw.ulx, rw.uly, rockSprite, fp_light_of(elevation, rw.obj->tile, rw.obj->flags));
            fp_emit_egg = false;
            if (!columnOk) {
                return false;
            }
            rockColumns++;
        }
        fp_emit_light_tile = -1;
    }

    // Scenery into objects, translucent pieces as plain cards.
    std::vector<int> loose;
    for (int i = 0; i < (int)pieces.size(); i++) {
        if (pieces[i].segs.empty() && !pieces[i].trans) {
            loose.push_back(i);
        }
    }
    std::vector<std::vector<int>> groups;
    std::vector<char> boardable;
    fp_group_scenery(pieces, loose, groups, boardable, eyes);
    static const int CARD[1] = { 0 };
    for (FpPiece& p : pieces) {
        if (p.segs.empty()) {
            p.segs = fp_fit_ground(p.bottom, p.ulx, CARD, 1, false, 0).segs;
        }
    }

    // Decorations (through his eyes: neither a box nor a long run) keep out of the walls' snap.
    for (int gi = 0; gi < (int)groups.size(); gi++) {
        for (int id : groups[gi]) {
            pieces[id].boarded = boardable[gi] != 0;
        }
    }

    std::vector<int> standing;
    for (int i = 0; i < (int)pieces.size(); i++) {
        if (!pieces[i].boarded && !pieces[i].trans && !pieces[i].cuboid) {
            standing.push_back(i);
        }
    }
    int offLineChains = fp_snap_wall_runs(pieces, standing);
    // A piece standing off its chain's line keeps in front only what stands before the chain (the junk
    // painted before the Junktown gate's container); a twin on the chain's line carries the rest of
    // its sprite, the wall it is part of -- no hole in the wall, no slit between its pieces.
    for (int id : standing) {
        FpPiece& p = pieces[id];
        size_t count = p.segs.size();
        for (size_t i = 0; i < count; i++) {
            if (!p.segs[i].fore) {
                continue;
            }
            FpSeg twin = p.segs[i];
            twin.fore = false;
            twin.k = twin.chainK;
            twin.run = twin.chainRun;
            p.segs[i].cut = std::max(1.0, p.segs[i].k - p.segs[i].chainK);
            p.segs.push_back(twin);
        }
    }
    if (eyes) {
        fp_box_runs(pieces, standing, objects);
    }

    // Through his eyes a decoration stands up as ONE board turned to the eye, as Doom stood its
    // decorations, its pieces where the game drew them, back to front, its lowest pixel on the ground.
    // When that pixel is well above its hex it hangs on something: on a wall (a sign, the growths on
    // the Master's walls) it is painted onto the wall's face, as the game drew it; on anything else (a
    // car on a container) its board stands over its hex at that height, never above the 96 px where
    // walls end and ceilings hang. (From the game's own camera the sprite already is the picture.)
    std::vector<const FpPiece*> walls;
    for (int id : standing) {
        if (pieces[id].group < 0 && !pieces[id].door && !pieces[id].trans) {
            walls.push_back(&pieces[id]);
        }
    }
    int lifted = 0;
    int hung = 0;
    for (int gi = 0; gi < (int)groups.size(); gi++) {
        const std::vector<int>& g = groups[gi];
        if (g.empty() || !boardable[gi]) {
            continue;
        }
        int minX = std::numeric_limits<int>::max();
        int maxX = std::numeric_limits<int>::min();
        double lowest = -std::numeric_limits<double>::infinity();
        double foot = -std::numeric_limits<double>::infinity();
        double highest = std::numeric_limits<double>::infinity();
        for (int id : g) {
            minX = std::min(minX, pieces[id].ulx);
            maxX = std::max(maxX, pieces[id].ulx + pieces[id].w);
            lowest = std::max(lowest, pieces[id].lowest);
            foot = std::max(foot, pieces[id].footY);
            for (double top : pieces[id].top) {
                if (!std::isnan(top)) {
                    highest = std::min(highest, top);
                }
            }
        }
        double ax = (minX + maxX) / 2.0;
        double ay = lowest + 1.0; // the height reference: this row stands on the ground
        double groundY = ay; // where it stands
        if (foot - lowest >= FP_LIFT_MIN) {
            FpSeg on;
            if (fp_hang_wall(pieces, g, walls, &on)) {
                for (int id : g) {
                    FpPiece& q = pieces[id];
                    q.boarded = false;
                    q.hung = true;
                    q.segs.clear();
                    q.segs.push_back({ on.axis, on.k + 0.75, (double)q.ulx, (double)(q.ulx + q.w), 0 });
                }
                hung++;
                continue;
            }
            ay = std::max(lowest + 1.0, std::min(foot, highest + 95.0));
            groundY = foot;
            lifted++;
            trace_event(TRACE_FLOW, "fp-lift", "tile=%d pieces=%d lift=%.0f", pieces[g[0]].tile, (int)g.size(), ay - lowest);
        }
        std::vector<int> back = g;
        std::stable_sort(back.begin(), back.end(), [&](int a, int b) { return pieces[a].anchorY < pieces[b].anchorY; });
        FpBoard board;
        board.anchor = fp_w(ax, groundY, 0.0);
        for (int id : back) {
            const FpPiece& q = pieces[id];
            board.pieces.push_back({ q.sprite, q.ulx - ax, q.uly - ay, q.tile, q.flags });
        }
        fp_boards.push_back(std::move(board));
    }

    int edited = !eyes ? fp_apply_edits(pieces, elevation) : 0;
    if (!eyes) {
        fp_picks.clear();
    }

    int panels = 0;
    int cuboids = 0;
    fp_emit_egg = true;
    for (const FpPiece& p : pieces) {
        if (p.boarded) {
            continue;
        }
        if (p.hiddenEdit && !fp_editing) {
            continue; // hidden from the 3D (shown, marked, while he edits)
        }
        cuboids += p.cuboid ? 1 : 0;
        fp_emit_light_tile = p.lightTile;
        for (const FpSeg& seg : p.segs) {
            double cut;
            bool lid;
            if (!eyes) {
                // From the game's camera a wall stands whole, as the game paints it, and none is boxed
                // (the sides smeared). A wall under a roof used to be cut at 96 px with the rest laid flat
                // on top -- every cave rock and the Vault's door frame came out halved -- to keep its top
                // from poking through the roof; the roofs are now painted over everything instead, as
                // the 2D game paints them (see drawScene).
                cut = 0.0;
                lid = false;
            } else if (p.hung) {
                cut = 96.0; // painted on its wall, no box: nothing above the wall's top
                lid = false;
            } else if (p.cuboid) {
                cut = p.cutPx; // a box: its top lies flat at its own height
                lid = true;
            } else if (p.group >= 0) {
                cut = 0.0; // a long run of scenery stands on its line, uncut
                lid = false;
            } else {
                cut = 96.0; // a wall: at roof height, above the eye
                lid = false;
            }
            size_t before = quads.size();
            if (!fp_emit_panel(quads, p, seg, cut, lid, eyes && !p.hung)) {
                return false;
            }
            if (!eyes && quads.size() > before) {
                // what the editor's mouse can pick: the panel's front
                FpPick pick;
                pick.elevation = elevation;
                pick.tile = p.tile;
                pick.fid = p.fid;
                pick.copy = p.copy;
                double m = seg.sloped ? seg.slope : FP_SLOPES[seg.axis];
                double b0 = seg.k + m * seg.x0;
                double b1 = seg.k + m * seg.x1;
                double t0 = b0 - p.uly;
                double t1 = b1 - p.uly;
                pick.c[0] = fp_w(seg.x0, b0, 0.0);
                pick.c[1] = fp_w(seg.x1, b1, 0.0);
                pick.c[2] = fp_w(seg.x1, b1 - t1, t1 / FP_KY);
                pick.c[3] = fp_w(seg.x0, b0 - t0, t0 / FP_KY);
                pick.sprite = p.sprite;
                pick.ulx = p.ulx;
                pick.uly = p.uly;
                pick.quad = quads[before];
                pick.hidden = p.hiddenEdit;
                fp_picks.push_back(pick);
            }
            panels++;
        }
    }

    fp_emit_egg = false;
    fp_emit_light_tile = -1;
    fp_flush_quads(quads, fp_static_verts, fp_static_ranges, &fp_static_light_tiles);
    fp_static_uploaded = false;

    double ms = (double)(SDL_GetPerformanceCounter() - started) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    trace_event(TRACE_FLOW, "fp-scene", "mode=%d elev=%d under=%d floors=%d objects=%d rock=%d panels=%d cuboids=%d boards=%d lifted=%d hung=%d edited=%d quads=%d offline=%d sprites=%d pages=%d ms=%.1f",
        mode, elevation, under ? 1 : 0, floors, (int)objects.size(), rockColumns, panels, cuboids, (int)fp_boards.size(), lifted, hung, edited, (int)quads.size(), offLineChains,
        (int)fp_sprites.size(), (int)fp_pages.size(), ms);
    return true;
}

// ---------------------------------------------------------------------------
// The eye.
// ---------------------------------------------------------------------------

static double fp_eye_x = 0.0;
static double fp_eye_z = 0.0;
static double fp_yaw = 0.0; // the look direction on the ground: 0 = +x (screen right), towards +z
static double fp_pitch = 0.0; // up and down: 0 = level, negative = looking down
static bool fp_yaw_set = false;
static Uint32 fp_last_ticks = 0;
static int fp_eye_map = -1; // where the eye last was: a new map or elevation, or a jump, starts it afresh
static int fp_eye_elevation = -1;
// THE STICK STEERS THE VIEW, as in Doom (his pick, 2026-10-29): left / right turn it, up / down walk
// along it (fp_view_steer, called from combat_rt's stick reading); turning in place turns the dude
// with it, a hex direction at a time, so whatever reads his facing (LT's first lock) agrees with
// the view. While the player steers, the view is his; when the dude turns by himself (a click walk,
// a script, an attack) it follows him again.
static Uint32 fp_steered = 0; // when the player last turned or walked with the stick
static Uint32 fp_steer_ticks = 0;
static int fp_seen_rotation = -1;
static bool fp_follow = true; // easing towards the dude's own facing
#define FP_TURN_RATE (160.0 * 3.14159265358979323846 / 180.0) // per second at full deflection
#define FP_STEER_HOLD 400 // ms the view stays the player's after he last steered
#define FP_STICK_DEAD 0.3f // the walk code's own deadzone (RT_GAMEPAD_DEADZONE), on each axis
// In a fight the eye turns and tilts onto the enemy (his pick, "sklopit na nepřítele"): the locked
// one, else the nearest hostile within FP_AIM_HEXES (combat_rt_eye_target: never a bystander) --
// turned to when locked, or within FP_AIM_TURN_HEXES (a rat at his feet is otherwise under the bottom
// of a 48-degree-high view); tilted to only as far as it is ahead of the view.
#define FP_AIM_TURN_HEXES 2
#define FP_AIM_HEXES 8

static int fp_rotation_of_yaw(double yaw)
{
    // rotation r faces (r - 1) * 60 degrees
    int r = (int)lround(yaw / (FP_PI / 3.0)) + 1;
    return ((r % 6) + 6) % 6;
}

static void fp_update_eye()
{
    int hx;
    int hy;
    fp_hex_top_left(obj_dude->tile, &hx, &hy);
    int gx = 0;
    int gy = 0;
    anim_smooth_offset(obj_dude, &gx, &gy);
    double wx = hx + 16 + obj_dude->x + gx;
    double wy = hy + 8 + obj_dude->y + gy;
    double ex = wx / FP_S;
    double ez = wy / FP_KZ;
    // a new map, another elevation or a jump (a teleport, a load) is a new start; a gap in time (a
    // menu was up) is not -- the view he steered stays his
    bool jumped = fp_yaw_set && ((ex - fp_eye_x) * (ex - fp_eye_x) + (ez - fp_eye_z) * (ez - fp_eye_z) > 9.0);
    if (map_get_index_number() != fp_eye_map || obj_dude->elevation != fp_eye_elevation || jumped) {
        fp_yaw_set = false;
        fp_eye_map = map_get_index_number();
        fp_eye_elevation = obj_dude->elevation;
    }
    fp_eye_x = ex;
    fp_eye_z = ez;

    // Rotation r faces (r - 1) * 60 degrees (ROTATION_E = 1 along +x); eased, so a turn of the
    // six-way facing is a turn of the head, not a cut.
    int rotation = std::min(std::max(obj_dude->rotation, 0), 5);
    double facing = (rotation - 1) * FP_PI / 3.0;
    Uint32 now = SDL_GetTicks();
    double dt = fp_last_ticks != 0 ? std::min(0.1, (now - fp_last_ticks) / 1000.0) : 0.0;
    fp_last_ticks = now;
    if (!fp_yaw_set) {
        fp_yaw = facing;
        fp_pitch = 0.0;
        fp_yaw_set = true;
        fp_follow = true;
        fp_seen_rotation = rotation;
        return;
    }
    bool steering = now - fp_steered < FP_STEER_HOLD;
    if (steering) {
        fp_follow = false;
    } else if (rotation != fp_seen_rotation) {
        fp_follow = true; // he turned by himself
    }
    fp_seen_rotation = rotation;

    // the enemy to look at (through his eyes only: Diablo's camera never turns)
    double wantYaw = facing;
    bool turnTo = fp_follow;
    double wantPitch = 0.0;
    Object* target = fp_mode == FP_VIEW_EYES ? combat_rt_eye_target(FP_AIM_HEXES) : NULL;
    if (target != NULL && target->elevation == obj_dude->elevation) {
        int thx;
        int thy;
        fp_hex_top_left(target->tile, &thx, &thy);
        int tgx = 0;
        int tgy = 0;
        anim_smooth_offset(target, &tgx, &tgy);
        double tx = (thx + 16 + target->x + tgx) / FP_S - fp_eye_x;
        double tz = (thy + 8 + target->y + tgy) / FP_KZ - fp_eye_z;
        double ground = sqrt(tx * tx + tz * tz);
        if (ground > 0.05) {
            double bearing = atan2(tz, tx);
            if (!steering && (combat_rt_has_lock() || tile_dist(obj_dude->tile, target->tile) <= FP_AIM_TURN_HEXES)) {
                wantYaw = bearing;
                turnTo = true;
            }
            // its middle: half the height of the frame it shows now (read off the art, not copied)
            double height = 1.0;
            CacheEntry* handle;
            Art* art = art_ptr_lock(target->fid, &handle);
            if (art != NULL) {
                int frames = art_frame_max_frame(art);
                int frame = std::min(std::max(target->frame, 0), std::max(frames - 1, 0));
                height = art_frame_length(art, frame, std::min(std::max(target->rotation, 0), 5)) / FP_KY;
                art_ptr_unlock(handle);
            }
            // tilted to as far as it is ahead: none for an enemy at his side or behind him
            double ahead = std::max(0.0, cos(bearing - (turnTo ? wantYaw : fp_yaw)));
            double tilt = std::min(15.0 * FP_PI / 180.0, std::max(-55.0 * FP_PI / 180.0, atan2(height * 0.5 - FP_EYE, ground)));
            wantPitch = tilt * ahead * ahead;
        }
    }
    if (turnTo) {
        double diff = atan2(sin(wantYaw - fp_yaw), cos(wantYaw - fp_yaw));
        fp_yaw += diff * std::min(1.0, dt * 8.0);
    }
    fp_pitch += (wantPitch - fp_pitch) * std::min(1.0, dt * 6.0);
}

static bool fp_world_visible();

// The stick in his eyes (see FP_TURN_RATE): turns the view by its left / right, and hands back as
// the walk the stick's up / down along the view, in the screen terms the walk code reads (x right,
// y down) -- or nothing to walk, when it only turns. Its own clock, not the fight's scaled one: the
// view turns as fast whatever the combat speed. False when the view is not his to steer (another
// view, a menu over it): the stick walks as ever.
bool fp_view_steer(float* x, float* y, float dt)
{
    (void)dt;
    Uint32 now = SDL_GetTicks();
    double step = fp_steer_ticks != 0 ? std::min(0.1, (now - fp_steer_ticks) / 1000.0) : 0.0;
    fp_steer_ticks = now;
    if (fp_mode != FP_VIEW_EYES || fp_failed || !fp_yaw_set || !fp_world_visible()) {
        return false;
    }
    // past the deadzone, the turn grows from nothing to full speed
    float turn = 0.0f;
    if (fabsf(*x) > FP_STICK_DEAD) {
        turn = (fabsf(*x) - FP_STICK_DEAD) / (1.0f - FP_STICK_DEAD) * (*x > 0.0f ? 1.0f : -1.0f);
    }
    float ahead = fabsf(*y) > FP_STICK_DEAD ? -*y : 0.0f; // SDL's y is +down: up walks forward
    if (turn != 0.0f || ahead != 0.0f) {
        fp_steered = now;
    }
    fp_yaw += turn * FP_TURN_RATE * step;
    *x = 0.0f;
    *y = 0.0f;
    if (ahead != 0.0f) {
        // the world direction of the view on the original screen: 32 px per unit across, KZ down;
        // a full-length stick (the walk code reads only the direction)
        double sx = FP_S * cos(fp_yaw);
        double sy = FP_KZ * sin(fp_yaw);
        double len = sqrt(sx * sx + sy * sy);
        float sign = ahead > 0.0f ? 1.0f : -1.0f;
        *x = (float)(sign * sx / len);
        *y = (float)(sign * sy / len);
    } else if (turn != 0.0f && anim_busy(obj_dude) == 0) {
        // turning in place turns him too, once the view is nearer another of his six directions
        int rotation = fp_rotation_of_yaw(fp_yaw);
        if (rotation != obj_dude->rotation) {
            Rect dirty;
            obj_set_rotation(obj_dude, rotation, &dirty);
            tile_refresh_rect(&dirty, obj_dude->elevation);
            fp_seen_rotation = rotation;
        }
    }
    return true;
}

// Column-major 4x4 maths for the one matrix the shader needs.
static void fp_mat_mul(const float* a, const float* b, float* out)
{
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
}

// The view matrix of a camera at `eye` looking along the unit vector `fwd`, world up (0, 1, 0).
static void fp_look_along(const double eye[3], const double fwd[3], float* view)
{
    // right = fwd x up, normalised; up = right x fwd
    double rx = -fwd[2];
    double ry = 0.0;
    double rz = fwd[0];
    double rl = sqrt(rx * rx + rz * rz);
    if (rl < 1e-9) {
        rx = 1.0;
        rz = 0.0;
        rl = 1.0;
    }
    rx /= rl;
    rz /= rl;
    double ux = ry * fwd[2] - rz * fwd[1];
    double uy = rz * fwd[0] - rx * fwd[2];
    double uz = rx * fwd[1] - ry * fwd[0];
    memset(view, 0, sizeof(float) * 16);
    view[0] = (float)rx;
    view[4] = (float)ry;
    view[8] = (float)rz;
    view[12] = (float)-(rx * eye[0] + ry * eye[1] + rz * eye[2]);
    view[1] = (float)ux;
    view[5] = (float)uy;
    view[9] = (float)uz;
    view[13] = (float)-(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    view[2] = (float)-fwd[0];
    view[6] = (float)-fwd[1];
    view[10] = (float)-fwd[2];
    view[14] = (float)(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    view[15] = 1.0f;
}

// Perspective, FLIPPED vertically: GL's row 0 is the bottom of a viewport, the frame texture's row 0
// its top (the shader turns gl_FrontFacing round to match, uFlip).
static void fp_perspective(double fovy, double aspect, double nearZ, double farZ, float* proj)
{
    double f = 1.0 / tan(fovy / 2.0);
    memset(proj, 0, sizeof(float) * 16);
    proj[0] = (float)(f / aspect);
    proj[5] = (float)-f;
    proj[10] = (float)((farZ + nearZ) / (nearZ - farZ));
    proj[11] = -1.0f;
    proj[14] = (float)((2.0 * farZ * nearZ) / (nearZ - farZ));
}

// Diablo's lens: 30 degrees from top to bottom (the map viewer's default; 45 and up blew the
// foreground into big pixels). Its distance puts the original's scale at the dude's feet.
static const double FP_DIABLO_FOVY = 30.0 * 3.14159265358979323846 / 180.0;

// Diablo's camera without its lens: the same view, orthographic, at the scale the lens has where it
// meets the ground under the dude (viewH game pixels tall) -- the game's own picture, slid.
static void fp_diablo_flat_matrix(int viewW, int viewH, float* mvp)
{
    double halfH = viewH / (2.0 * FP_S);
    double halfW = halfH * viewW / (double)viewH;
    double dist = 100.0;
    double eye[3] = { fp_eye_x + FP_TO_CAMERA[0] * dist, FP_TO_CAMERA[1] * dist, fp_eye_z + FP_TO_CAMERA[2] * dist };
    double fwd[3] = { -FP_TO_CAMERA[0], -FP_TO_CAMERA[1], -FP_TO_CAMERA[2] };
    float view[16];
    fp_look_along(eye, fwd, view);
    float proj[16];
    memset(proj, 0, sizeof(proj));
    proj[0] = (float)(1.0 / halfW);
    proj[5] = (float)(-1.0 / halfH); // flipped like fp_perspective
    proj[10] = -0.001f;
    proj[15] = 1.0f;
    fp_mat_mul(proj, view, mvp);
}

// Diablo's lens, top to bottom (FALLOUT_FP_FOVY=<degrees> tries another in a development build).
static double fp_diablo_fovy()
{
    double fovy = FP_DIABLO_FOVY;
#ifndef FALLOUT_RELEASE
    const char* lens = getenv("FALLOUT_FP_FOVY");
    if (lens != NULL && atof(lens) > 1.0 && atof(lens) < 90.0) {
        fovy = atof(lens) * FP_PI / 180.0;
    }
#endif
    return fovy;
}

static void fp_camera_matrix(int mode, int viewW, int viewH, float* mvp)
{
    double aspect = (double)viewW / (double)viewH;
    float proj[16];
    float view[16];
    if (mode == FP_VIEW_DIABLO) {
        // the view is viewH game pixels tall where it meets the ground under the dude, S to a unit
        double fovy = fp_diablo_fovy();
        double dist = viewH / (2.0 * FP_S * tan(fovy / 2.0));
        double eye[3] = { fp_eye_x + FP_TO_CAMERA[0] * dist, FP_TO_CAMERA[1] * dist, fp_eye_z + FP_TO_CAMERA[2] * dist };
        double fwd[3] = { -FP_TO_CAMERA[0], -FP_TO_CAMERA[1], -FP_TO_CAMERA[2] };
        fp_perspective(fovy, aspect, 0.5, 2000.0, proj);
        fp_look_along(eye, fwd, view);
    } else {
        // 90 degrees ACROSS whatever the shape of the view (a fixed vertical angle shrank everyone)
        fp_perspective(2.0 * atan(1.0 / aspect), aspect, FP_NEAR, FP_FAR, proj);
        double eye[3] = { fp_eye_x, FP_EYE, fp_eye_z };
        double fwd[3] = { cos(fp_yaw) * cos(fp_pitch), sin(fp_pitch), sin(fp_yaw) * cos(fp_pitch) };
        fp_look_along(eye, fwd, view);
    }
    fp_mat_mul(proj, view, mvp);
}

// The per-frame part: critters as billboards or, lying, as decals; boards turned to the eye. Through
// his eyes each critter shows the side the eye sees it from (the bearing), its painted ground shadow
// stripped; from Diablo's camera it is the game's sprite as drawn, the dude among them. False when
// the atlas ran out.
static bool fp_build_dynamic(int mode, int elevation, const std::vector<Object*>& critters, std::vector<float>& verts, std::vector<FpRange>& ranges)
{
    bool eyes = mode == FP_VIEW_EYES;
    std::vector<FpQuad> quads;
    quads.reserve(critters.size() + fp_boards.size() * 2 + 8);

    for (Object* obj : critters) {
        int anim = FID_ANIM_TYPE(obj->fid);
        bool lying = (anim >= 20 && anim <= 35) || (anim >= 48 && anim <= 63);
        int rot = std::min(std::max(obj->rotation, 0), 5);
        int gx = 0;
        int gy = 0;
        anim_smooth_offset(obj, &gx, &gy);
        float light = fp_light_of(elevation, obj->tile, obj->flags);

        if (lying || (obj->flags & OBJECT_FLAT) != 0) {
            int sprite;
            int ulx;
            int uly;
            if (!fp_object_sprite(obj, rot, false, &sprite, &ulx, &uly, true)) {
                continue;
            }
            ulx += gx;
            uly += gy;
            const FpSprite& s = fp_sprites[sprite];
            double lift = 0.012;
            FpV3 c[4] = { fp_w(ulx, uly, lift), fp_w(ulx + s.w, uly, lift), fp_w(ulx + s.w, uly + s.h, lift), fp_w(ulx, uly + s.h, lift) };
            if (!fp_emit_proj(quads, FP_KIND_DECAL, sprite, ulx, uly, c, NULL, light, 1.0f)) {
                return false;
            }
            continue;
        }

        // obj->x/y and the glide carry the body over the ground from its hex: they move the anchor,
        // whichever side is shown (in the sprite's plane they made walkers bob and hop)
        int hx;
        int hy;
        fp_hex_top_left(obj->tile, &hx, &hy);
        FpV3 anchor = fp_w(hx + 16 + obj->x + gx, hy + 8 + obj->y + gy, 0.0);
        double psi = eyes ? atan2(anchor.x - fp_eye_x, -(anchor.z - fp_eye_z)) : 0.0;
        double facing = (rot - 1) * FP_PI / 3.0;
        int dir = (((int)lround((facing - psi) / (FP_PI / 3.0)) + 1) % 6 + 6) % 6;
        // per-direction art files (knock-downs, deaths): only the current direction is at hand
        if ((obj->fid & 0x70000000) != 0) {
            dir = rot;
        }
        int sprite = fp_sprite(obj->fid, obj->frame, dir, eyes);
        if (sprite < 0) {
            continue;
        }
        const FpSprite& s = fp_sprites[sprite];
        double dx0 = s.xOff - (s.w >> 1);
        double dy0 = s.yOff - (s.h - 1);
        if (!fp_emit_billboard(quads, sprite, anchor, psi, dx0, dy0, 0.0, light)) {
            return false;
        }
    }

    fp_emit_egg = true;
    for (const FpBoard& board : fp_boards) {
        double psi = atan2(board.anchor.x - fp_eye_x, -(board.anchor.z - fp_eye_z));
        for (int i = 0; i < (int)board.pieces.size(); i++) {
            const FpBoardPiece& piece = board.pieces[i];
            if (!fp_emit_billboard(quads, piece.sprite, board.anchor, psi, piece.dx0, piece.dy0, i * 0.004, fp_light_of(elevation, piece.tile, piece.flags))) {
                fp_emit_egg = false;
                return false;
            }
        }
    }
    fp_emit_egg = false;

    // The game's cursor on the map: the aim's red reticle and to-hit % glued on a target, the hex
    // under the pointer. In 2D they are objects drawn with the scene; here they stand where they stand
    // and are drawn over everything, as a sight must be.
    // the 3D editor: the picked piece lit, the hidden ones marked
    if (!eyes && fp_editing) {
        for (const FpPick& pick : fp_picks) {
            bool chosen = fp_edit_selected && pick.elevation == fp_edit_sel.elevation && pick.tile == fp_edit_sel.tile
                && pick.fid == fp_edit_sel.fid && pick.copy == fp_edit_sel.copy;
            if (chosen || pick.hidden) {
                FpQuad q = pick.quad;
                q.kind = FP_KIND_SELECT;
                // a hidden piece is marked red, the picked one yellow (in the light slot: > 1 = red)
                for (int i = 0; i < 6; i++) {
                    q.v[i * FP_VERTEX_FLOATS + 9] = chosen ? 1.0f : 2.0f;
                }
                quads.push_back(q);
            }
        }
    }

    Object* cursors[2] = { obj_mouse_flat, obj_mouse };
    // the 2D view's offset (world px = screen px + origin), for a cursor placed on the screen
    int originX = 0;
    int originY = 0;
    {
        int sx;
        int sy;
        int wx;
        int wy;
        tile_coord(obj_dude->tile, &sx, &sy, obj_dude->elevation);
        fp_hex_top_left(obj_dude->tile, &wx, &wy);
        originX = wx - sx;
        originY = wy - sy;
    }
    // the critter a screen-placed cursor sits on in 2D (the reticle on the lock, on a quick throw's
    // target, on whatever the pointer is over): the one whose picture holds the cursor's middle,
    // the lock first
    auto under = [&](Object* cursor, const FpSprite& s) -> Object* {
        int cx = cursor->sx + (s.w >> 1);
        int cy = cursor->sy + (s.h >> 1);
        auto holds = [&](Object* critter) {
            if (critter == NULL || critter->elevation != elevation || critter->tile < 0) {
                return false;
            }
            Rect r;
            obj_bound(critter, &r);
            return cx >= r.ulx && cx <= r.lrx && cy >= r.uly && cy <= r.lry;
        };
        Object* lock = combat_rt_lock_target();
        if (holds(lock)) {
            return lock;
        }
        for (Object* critter : critters) {
            if (holds(critter)) {
                return critter;
            }
        }
        return NULL;
    };
    for (Object* cursor : cursors) {
        // a cursor placed on the screen (tile -1) shows whatever elevation is shown, as the 2D draws it
        if (cursor == NULL || (cursor->flags & OBJECT_HIDDEN) != 0 || (cursor->tile >= 0 && cursor->elevation != elevation)) {
            continue;
        }
        int sprite = fp_sprite_live(cursor->fid, cursor->frame, 0);
        if (sprite < 0) {
            continue;
        }
        const FpSprite& s = fp_sprites[sprite];
        FpV3 anchor;
        double dx0;
        double dy0;
        if (cursor->tile >= 0 && cursor->tile < HEX_GRID_SIZE) {
            int hx;
            int hy;
            fp_hex_top_left(cursor->tile, &hx, &hy);
            double footX = hx + 16 + cursor->x;
            double footY = hy + 8 + cursor->y;
            dx0 = s.xOff - (s.w >> 1);
            dy0 = s.yOff - (s.h - 1);
            if ((cursor->flags & OBJECT_FLAT) != 0) {
                // lying on the ground as the game draws it (the hex under the pointer)
                double ulx = footX + dx0;
                double uly = footY + dy0;
                FpV3 c[4] = { fp_w(ulx, uly, 0.02), fp_w(ulx + s.w, uly, 0.02), fp_w(ulx + s.w, uly + s.h, 0.02), fp_w(ulx, uly + s.h, 0.02) };
                if (!fp_emit_proj(quads, FP_KIND_OVERLAY, sprite, ulx, uly, c, NULL, 1.0f, 1.0f)) {
                    return false;
                }
                continue;
            }
            anchor = fp_w(footX, footY, 0.0);
        } else if (Object* aimed = under(cursor, s)) {
            // placed on the screen over a critter (the red reticle and its to-hit %): it rides on the
            // critter where the 3D shows it, as far from its feet as the 2D draws it
            int tx;
            int ty;
            tile_coord(aimed->tile, &tx, &ty, aimed->elevation);
            int hx;
            int hy;
            fp_hex_top_left(aimed->tile, &hx, &hy);
            int gx = 0;
            int gy = 0;
            anim_smooth_offset(aimed, &gx, &gy);
            dx0 = cursor->sx - (tx + 16 + aimed->x + gx);
            dy0 = cursor->sy - (ty + 8 + aimed->y + gy);
            anchor = fp_w(hx + 16 + aimed->x + gx, hy + 8 + aimed->y + gy, 0.0);
        } else {
            // placed on the screen with nothing aimed: standing on the ground where it points
            double footX = cursor->sx + originX + (s.w >> 1);
            double footY = cursor->sy + originY + (s.h - 1);
            anchor = fp_w(footX, footY, 0.0);
            dx0 = -(s.w >> 1);
            dy0 = -(s.h - 1);
        }
        double psi = eyes ? atan2(anchor.x - fp_eye_x, -(anchor.z - fp_eye_z)) : 0.0;
        if (!fp_emit_billboard(quads, sprite, anchor, psi, dx0, dy0, 0.0, 1.0f)) {
            return false;
        }
        quads.back().kind = FP_KIND_OVERLAY;
    }

    fp_flush_quads(quads, verts, ranges, NULL);
    return true;
}

// ---------------------------------------------------------------------------
// GL objects.
// ---------------------------------------------------------------------------

static GLuint fp_program = 0;
static GLuint fp_static_vbo = 0;
static GLuint fp_overhead_vbo = 0;
static GLuint fp_dynamic_vbo = 0;
static GLuint fp_vao = 0;
static GLuint fp_fbo = 0;
static GLuint fp_depth_rb = 0;
static GLuint fp_palette_tex = 0;
// THE HOLES, FILLED WITH THE GAME'S OWN PICTURE (his "fill the gaps with the same texture",
// 2026-10-30). From Diablo's camera the lens shows a little of what the game's camera never sees --
// the floor behind a rock slope between it and the next -- and the original never painted it. Each
// frame the cave's rock slopes are drawn once more from the game's own camera (orthographic, the 2D
// projection): their depth, and their own lit picture; a fragment the game's camera cannot see
// behind rock takes that picture's colour at its spot -- the very rock that hides it in 2D, so the
// slot is filled with the slope's own picture and runs on from it (never with a critter, the egg or
// a cursor the 2D frame has over it).
static GLuint fp_frame_copy = 0; // the rock's picture from the game's camera
static bool fp_fill_failed = false; // the fill's framebuffer would not complete: no fill
static GLuint fp_odepth_tex = 0; // the scene's depth from the game's camera, packed in RGBA
static GLuint fp_odepth_fbo = 0;
static GLuint fp_odepth_rb = 0;
static int fp_fill_w = 0;
static int fp_fill_h = 0;
#define FP_FILL_RANGE 600.0 // units of depth either side of the dude the depth map holds
#define FP_FILL_EPS 0.2 // units nearer the game's camera something must stand to hide a point
static GLuint fp_fbo_target = 0; // the frame texture the FBO is attached to
static int fp_fbo_w = 0;
static int fp_fbo_h = 0;

static struct {
    GLint mvp;
    GLint atlas;
    GLint palette;
    GLint atlasSize;
    GLint ambient;
    GLint fog;
    GLint fogRange;
    GLint flip;
    GLint opacity;
    GLint egg;
    GLint eggDepth;
    GLint pass;
    GLint fill;
    GLint frame;
    GLint odepth;
    GLint viewSize;
    GLint origin;
    GLint toCam;
    GLint depthCR;
    GLint k;
    GLint eps;
    GLint flatMVP;
    GLint edgeFlat;
    GLint select;
} fp_loc;

static const char* FP_VERTEX_SHADER = R"GLSL(
uniform mat4 uMVP;
uniform mat4 uFlatMVP;
uniform vec2 uEdgeFlat;
attribute vec3 aPos;
attribute vec2 aPix;
attribute vec4 aRect;
attribute float aLight;
attribute float aBack;
varying vec2 vPix;
varying vec4 vRect;
varying float vLight;
varying float vBack;
varying float vDepth;
varying vec3 vWorld;
void main() {
    vPix = aPix;
    vRect = aRect;
    vLight = aLight;
    vBack = aBack;
    vWorld = aPos;
    vec4 p = uMVP * vec4(aPos, 1.0);
    if (uEdgeFlat.y > uEdgeFlat.x) {
        // toward the view's edges the lens gives way to the flat picture: where it would pull two
        // overlapping pieces of the 2D art apart the most, it lets them lie as the game drew them
        vec4 q = uFlatMVP * vec4(aPos, 1.0);
        // judged where it stands on the ground, so a standing thing gives way as one piece, top and foot
        vec4 g = uFlatMVP * vec4(aPos.x, 0.0, aPos.z, 1.0);
        float t = smoothstep(uEdgeFlat.x, uEdgeFlat.y, length(g.xy));
        p.xy = mix(p.xy / p.w, q.xy, t) * p.w;
    }
    vDepth = p.w;
    gl_Position = p;
}
)GLSL";

// The sprite pixel under the fragment (outside the sprite or palette index 0: nothing), through the
// live palette (colour cycling, gamma and fades come with it), darkened by its light as the 2D
// renderer does (never the glowing indices 229..255), its unpainted back darker, then the haze.
// THE EGG (obj_render_object's EGG.FRM, which the 2D game cuts into walls before the dude): a
// standing thing (vBack < 0) nearer the camera than the dude is cut out in an ellipse round him
// (uEgg: its centre and radii in the view's pixels, radii 0 = none), dithered away over its rim.
static const char* FP_FRAGMENT_SHADER = R"GLSL(
uniform sampler2D uAtlas;
uniform sampler2D uPal;
uniform vec2 uAtlasSize;
uniform float uAmbient;
uniform vec3 uFog;
uniform vec2 uFogRange;
uniform float uFlip;
uniform float uOpacity;
uniform vec4 uEgg;
uniform float uEggDepth;
uniform float uPass;
uniform float uFill;
uniform sampler2D uFrame;
uniform sampler2D uODepth;
uniform vec2 uViewSize;
uniform vec2 uOrigin;
uniform vec3 uToCam;
uniform vec2 uDepthCR;
uniform vec2 uK;
uniform float uEps;
uniform float uSelect;
varying vec2 vPix;
varying vec4 vRect;
varying float vLight;
varying float vBack;
varying float vDepth;
varying vec3 vWorld;
void main() {
    if (vBack < 0.0 && uEgg.z > 0.0 && vDepth < uEggDepth) {
        vec2 e = (gl_FragCoord.xy - uEgg.xy) / uEgg.zw;
        float r = dot(e, e);
        if (r < 1.0) {
            vec2 cell = mod(floor(gl_FragCoord.xy), 2.0);
            float rank = (cell.x * 2.0 + cell.y + 0.5) / 4.0;
            if (r < 0.64 || rank > (r - 0.64) / 0.36) discard;
        }
    }
    vec2 l = floor(vPix);
    if (l.x < 0.0 || l.y < 0.0 || l.x >= vRect.z || l.y >= vRect.w) discard;
    float idx = texture2D(uAtlas, (vRect.xy + l + 0.5) / uAtlasSize).r * 255.0;
    if (idx < 0.5) discard;
    if (uPass > 0.5 && uPass < 1.5) {
        // the depth map from the game's camera: depth packed into four bytes (from the world point,
        // as the fill reads it back, not from the window's depth)
        float d = clamp((uDepthCR.x - dot(vWorld, uToCam)) / uDepthCR.y * 0.5 + 0.5, 0.0, 0.999999);
        vec4 e = fract(d * vec4(1.0, 255.0, 65025.0, 16581375.0));
        e -= e.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);
        gl_FragColor = e;
        return;
    }
    if (uFill > 0.5) {
        // where the game's camera sees this point, and whether something nearer hides it there
        vec2 o = floor(vec2(32.0 * vWorld.x, uK.x * vWorld.z - uK.y * vWorld.y) - uOrigin);
        if (o.x >= 0.0 && o.y >= 0.0 && o.x < uViewSize.x && o.y < uViewSize.y) {
            vec2 uv = (o + 0.5) / uViewSize;
            float hidden = dot(texture2D(uODepth, uv), vec4(1.0, 1.0 / 255.0, 1.0 / 65025.0, 1.0 / 16581375.0));
            float mine = (uDepthCR.x - dot(vWorld, uToCam)) / uDepthCR.y * 0.5 + 0.5;
            if (mine > hidden + uEps) {
                gl_FragColor = vec4(texture2D(uFrame, uv).rgb, 1.0);
                return;
            }
        }
    }
    vec3 c = texture2D(uPal, vec2((floor(idx + 0.5) + 0.5) / 256.0, 0.5)).rgb;
    float lit = idx > 228.5 ? 1.0 : clamp(max(uAmbient, vLight), 0.0, 1.0);
    bool front = gl_FrontFacing;
    if (uFlip > 0.5) front = !front;
    c *= lit * (front ? 1.0 : abs(vBack));
    c = mix(c, uFog, smoothstep(uFogRange.x, uFogRange.y, vDepth));
    if (uSelect > 0.5) {
        // the 3D editor's mark: yellow on the picked piece, red on a hidden one
        c = mix(c, vLight > 1.5 ? vec3(1.0, 0.15, 0.1) : vec3(1.0, 0.85, 0.2), 0.45);
    }
    gl_FragColor = vec4(c, uOpacity);
}
)GLSL";

#define FP_LOAD(field, type, name)                                \
    do {                                                          \
        gl.field = (type)SDL_GL_GetProcAddress(name);             \
        if (gl.field == NULL) {                                   \
            debug_printf("FP view: missing GL entry %s\n", name); \
            return false;                                         \
        }                                                         \
    } while (0)

#define FP_LOAD_EITHER(field, type, name, alt)                        \
    do {                                                              \
        gl.field = (type)SDL_GL_GetProcAddress(name);                 \
        if (gl.field == NULL) {                                       \
            gl.field = (type)SDL_GL_GetProcAddress(alt);              \
        }                                                             \
        if (gl.field == NULL) {                                       \
            debug_printf("FP view: missing GL entry %s\n", name);     \
            return false;                                             \
        }                                                             \
    } while (0)

static bool fp_load_gl()
{
    if (fp_gl_loaded) {
        return true;
    }
    memset(&gl, 0, sizeof(gl));

    FP_LOAD(Viewport, FP_Viewport, "glViewport");
    FP_LOAD(Scissor, FP_Scissor, "glScissor");
    FP_LOAD(ClearColor, FP_ClearColor, "glClearColor");
    FP_LOAD(Clear, FP_Clear, "glClear");
    FP_LOAD(Disable, FP_Disable, "glDisable");
    FP_LOAD(Enable, FP_Enable, "glEnable");
    FP_LOAD(IsEnabled, FP_IsEnabled, "glIsEnabled");
    FP_LOAD(DrawArrays, FP_DrawArrays, "glDrawArrays");
    FP_LOAD(GetIntegerv, FP_GetIntegerv, "glGetIntegerv");
    FP_LOAD(GetFloatv, FP_GetFloatv, "glGetFloatv");
    FP_LOAD(GetBooleanv, FP_GetBooleanv, "glGetBooleanv");
    FP_LOAD(GetString, FP_GetString, "glGetString");
    FP_LOAD(ReadPixels, FP_ReadPixels, "glReadPixels");
    FP_LOAD(PixelStorei, FP_PixelStorei, "glPixelStorei");
    FP_LOAD(DepthMask, FP_DepthMask, "glDepthMask");
    FP_LOAD(DepthFunc, FP_DepthFunc, "glDepthFunc");
    FP_LOAD(BlendFunc, FP_BlendFunc, "glBlendFunc");
    FP_LOAD(PolygonOffset, FP_PolygonOffset, "glPolygonOffset");
    FP_LOAD(BindTexture, FP_BindTexture, "glBindTexture");
    FP_LOAD(GenTextures, FP_GenTextures, "glGenTextures");
    FP_LOAD(DeleteTextures, FP_DeleteTextures, "glDeleteTextures");
    FP_LOAD(TexImage2D, FP_TexImage2D, "glTexImage2D");
    FP_LOAD(TexSubImage2D, FP_TexSubImage2D, "glTexSubImage2D");
    FP_LOAD(TexParameteri, FP_TexParameteri, "glTexParameteri");

    FP_LOAD(CreateShader, PFNGLCREATESHADERPROC, "glCreateShader");
    FP_LOAD(ShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource");
    FP_LOAD(CompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    FP_LOAD(GetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    FP_LOAD(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    FP_LOAD(DeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader");
    FP_LOAD(CreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    FP_LOAD(AttachShader, PFNGLATTACHSHADERPROC, "glAttachShader");
    FP_LOAD(BindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC, "glBindAttribLocation");
    FP_LOAD(LinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    FP_LOAD(GetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    FP_LOAD(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    FP_LOAD(UseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram");
    FP_LOAD(DeleteProgram, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    FP_LOAD(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    FP_LOAD(Uniform1i, PFNGLUNIFORM1IPROC, "glUniform1i");
    FP_LOAD(Uniform1f, PFNGLUNIFORM1FPROC, "glUniform1f");
    FP_LOAD(Uniform2f, PFNGLUNIFORM2FPROC, "glUniform2f");
    FP_LOAD(Uniform3f, PFNGLUNIFORM3FPROC, "glUniform3f");
    FP_LOAD(Uniform4f, PFNGLUNIFORM4FPROC, "glUniform4f");
    FP_LOAD(UniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC, "glUniformMatrix4fv");
    FP_LOAD(GenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    FP_LOAD(BindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    FP_LOAD(BufferData, PFNGLBUFFERDATAPROC, "glBufferData");
    FP_LOAD(DeleteBuffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
    FP_LOAD(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    FP_LOAD(DisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC, "glDisableVertexAttribArray");
    FP_LOAD(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");
    FP_LOAD(GetVertexAttribiv, PFNGLGETVERTEXATTRIBIVPROC, "glGetVertexAttribiv");
    FP_LOAD(ActiveTexture, PFNGLACTIVETEXTUREPROC, "glActiveTexture");

    FP_LOAD_EITHER(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC, "glGenFramebuffers", "glGenFramebuffersEXT");
    FP_LOAD_EITHER(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC, "glBindFramebuffer", "glBindFramebufferEXT");
    FP_LOAD_EITHER(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC, "glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    FP_LOAD_EITHER(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    FP_LOAD_EITHER(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC, "glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    FP_LOAD_EITHER(GenRenderbuffers, PFNGLGENRENDERBUFFERSPROC, "glGenRenderbuffers", "glGenRenderbuffersEXT");
    FP_LOAD_EITHER(BindRenderbuffer, PFNGLBINDRENDERBUFFERPROC, "glBindRenderbuffer", "glBindRenderbufferEXT");
    FP_LOAD_EITHER(RenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC, "glRenderbufferStorage", "glRenderbufferStorageEXT");
    FP_LOAD_EITHER(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC, "glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    FP_LOAD_EITHER(DeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC, "glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");

    gl.GenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
    gl.BindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
    gl.DeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
    if (gl.GenVertexArrays == NULL || gl.BindVertexArray == NULL) {
        // ES 2.0 has them only as an extension
        gl.GenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArraysOES");
        gl.BindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArrayOES");
        gl.DeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArraysOES");
    }

    fp_gl_loaded = true;
    return true;
}

#undef FP_LOAD
#undef FP_LOAD_EITHER

static bool fp_context_is_es()
{
    int profile = 0;
    if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile) == 0 && profile == SDL_GL_CONTEXT_PROFILE_ES) {
        return true;
    }
    const GLubyte* version = gl.GetString(GL_VERSION);
    return version != NULL && strstr((const char*)version, "OpenGL ES") != NULL;
}

// Same preamble rule as crt_shader.cc: GLSL 1.20 on desktop, ES 1.00 with a precision block.
static GLuint fp_compile(GLenum type, const char* source)
{
    GLuint shader = gl.CreateShader(type);
    if (shader == 0) {
        return 0;
    }
    const char* header;
    if (!fp_context_is_es()) {
        header = "#version 120\n";
    } else if (type == GL_FRAGMENT_SHADER) {
        header = "#version 100\n"
                 "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                 "precision highp float;\n"
                 "#else\n"
                 "precision mediump float;\n"
                 "#endif\n";
    } else {
        header = "#version 100\n";
    }
    const char* parts[2] = { header, source };
    gl.ShaderSource(shader, 2, parts, NULL);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        debug_printf("FP view: %s shader failed: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        trace_event(TRACE_WARN, "fp-view", "shader failed: %s", log);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool fp_build_program()
{
    GLuint vs = fp_compile(GL_VERTEX_SHADER, FP_VERTEX_SHADER);
    if (vs == 0) {
        return false;
    }
    GLuint fs = fp_compile(GL_FRAGMENT_SHADER, FP_FRAGMENT_SHADER);
    if (fs == 0) {
        gl.DeleteShader(vs);
        return false;
    }
    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.BindAttribLocation(program, 0, "aPos");
    gl.BindAttribLocation(program, 1, "aPix");
    gl.BindAttribLocation(program, 2, "aRect");
    gl.BindAttribLocation(program, 3, "aLight");
    gl.BindAttribLocation(program, 4, "aBack");
    gl.LinkProgram(program);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        debug_printf("FP view: program failed to link: %s\n", log);
        trace_event(TRACE_WARN, "fp-view", "link failed: %s", log);
        gl.DeleteProgram(program);
        return false;
    }
    fp_program = program;
    fp_loc.mvp = gl.GetUniformLocation(program, "uMVP");
    fp_loc.atlas = gl.GetUniformLocation(program, "uAtlas");
    fp_loc.palette = gl.GetUniformLocation(program, "uPal");
    fp_loc.atlasSize = gl.GetUniformLocation(program, "uAtlasSize");
    fp_loc.ambient = gl.GetUniformLocation(program, "uAmbient");
    fp_loc.fog = gl.GetUniformLocation(program, "uFog");
    fp_loc.fogRange = gl.GetUniformLocation(program, "uFogRange");
    fp_loc.flip = gl.GetUniformLocation(program, "uFlip");
    fp_loc.opacity = gl.GetUniformLocation(program, "uOpacity");
    fp_loc.egg = gl.GetUniformLocation(program, "uEgg");
    fp_loc.eggDepth = gl.GetUniformLocation(program, "uEggDepth");
    fp_loc.pass = gl.GetUniformLocation(program, "uPass");
    fp_loc.fill = gl.GetUniformLocation(program, "uFill");
    fp_loc.frame = gl.GetUniformLocation(program, "uFrame");
    fp_loc.odepth = gl.GetUniformLocation(program, "uODepth");
    fp_loc.viewSize = gl.GetUniformLocation(program, "uViewSize");
    fp_loc.origin = gl.GetUniformLocation(program, "uOrigin");
    fp_loc.toCam = gl.GetUniformLocation(program, "uToCam");
    fp_loc.depthCR = gl.GetUniformLocation(program, "uDepthCR");
    fp_loc.k = gl.GetUniformLocation(program, "uK");
    fp_loc.eps = gl.GetUniformLocation(program, "uEps");
    fp_loc.flatMVP = gl.GetUniformLocation(program, "uFlatMVP");
    fp_loc.edgeFlat = gl.GetUniformLocation(program, "uEdgeFlat");
    fp_loc.select = gl.GetUniformLocation(program, "uSelect");

    if (gl.GenVertexArrays != NULL && gl.BindVertexArray != NULL) {
        gl.GenVertexArrays(1, &fp_vao);
    }
    gl.GenBuffers(1, &fp_static_vbo);
    gl.GenBuffers(1, &fp_overhead_vbo);
    gl.GenBuffers(1, &fp_dynamic_vbo);
    gl.GenTextures(1, &fp_palette_tex);
    return true;
}

// The FBO over the frame texture, with its own depth buffer (the window's is 16 bit at best).
static bool fp_ensure_fbo(GLuint target, int width, int height)
{
    if (fp_fbo != 0 && fp_fbo_target == target && fp_fbo_w == width && fp_fbo_h == height) {
        return true;
    }
    if (fp_fbo != 0) {
        gl.DeleteFramebuffers(1, &fp_fbo);
        fp_fbo = 0;
    }
    if (fp_depth_rb != 0) {
        gl.DeleteRenderbuffers(1, &fp_depth_rb);
        fp_depth_rb = 0;
    }

    gl.GenRenderbuffers(1, &fp_depth_rb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, fp_depth_rb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, fp_context_is_es() ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24, width, height);
    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);

    gl.GenFramebuffers(1, &fp_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fp_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fp_depth_rb);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        debug_printf("FP view: framebuffer incomplete (0x%x)\n", status);
        trace_event(TRACE_WARN, "fp-view", "framebuffer incomplete 0x%x", status);
        return false;
    }
    fp_fbo_target = target;
    fp_fbo_w = width;
    fp_fbo_h = height;
    return true;
}

// The hole fill's frame copy and depth map, the view's size.
static bool fp_ensure_fill(int width, int height)
{
    if (fp_odepth_fbo != 0 && fp_fill_w == width && fp_fill_h == height) {
        return true;
    }
    if (fp_fill_failed) {
        return false;
    }
    if (fp_frame_copy == 0) {
        gl.GenTextures(1, &fp_frame_copy);
    }
    if (fp_odepth_tex == 0) {
        gl.GenTextures(1, &fp_odepth_tex);
    }
    GLuint texs[2] = { fp_frame_copy, fp_odepth_tex };
    for (int i = 0; i < 2; i++) {
        gl.BindTexture(GL_TEXTURE_2D, texs[i]);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    if (fp_odepth_rb != 0) {
        gl.DeleteRenderbuffers(1, &fp_odepth_rb);
    }
    gl.GenRenderbuffers(1, &fp_odepth_rb);
    gl.BindRenderbuffer(GL_RENDERBUFFER, fp_odepth_rb);
    gl.RenderbufferStorage(GL_RENDERBUFFER, fp_context_is_es() ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24, width, height);
    gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
    if (fp_odepth_fbo == 0) {
        gl.GenFramebuffers(1, &fp_odepth_fbo);
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, fp_odepth_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fp_odepth_tex, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fp_odepth_rb);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        trace_event(TRACE_WARN, "fp-view", "fill framebuffer incomplete 0x%x -- no fill", status);
        gl.DeleteFramebuffers(1, &fp_odepth_fbo);
        fp_odepth_fbo = 0;
        fp_fill_failed = true;
        return false;
    }
    fp_fill_w = width;
    fp_fill_h = height;
    return true;
}

// 24-bit bottom-up BMP of the view (the FBO's rows run top-down: row 0 is the frame's top).
static void fp_write_capture(const char* path, int width, int height, const unsigned char* rgb)
{
    FILE* stream = fopen(path, "wb");
    if (stream == NULL) {
        return;
    }
    const int stride = (width * 3 + 3) & ~3;
    const unsigned int dataSize = (unsigned int)stride * height;
    unsigned char header[54] = { 0 };
    header[0] = 'B';
    header[1] = 'M';
    unsigned int fileSize = dataSize + 54;
    memcpy(header + 2, &fileSize, 4);
    unsigned int offset = 54;
    memcpy(header + 10, &offset, 4);
    unsigned int infoSize = 40;
    memcpy(header + 14, &infoSize, 4);
    memcpy(header + 18, &width, 4);
    memcpy(header + 22, &height, 4);
    unsigned short planes = 1;
    unsigned short bits = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bits, 2);
    memcpy(header + 34, &dataSize, 4);
    fwrite(header, 1, sizeof(header), stream);
    std::vector<unsigned char> row(stride, 0);
    for (int y = height - 1; y >= 0; y--) {
        const unsigned char* src = rgb + (size_t)y * width * 3;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 3 + 2];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 0];
        }
        fwrite(row.data(), 1, stride, stream);
    }
    fclose(stream);
}

static bool fp_world_visible();

static void fp_request_capture(const char* path)
{
    // only when the next main-loop present is going to draw the view: a request it cannot serve
    // is dropped there, never written later under this screenshot's name
    if (fp_mode == FP_VIEW_OFF || fp_failed || path == NULL || !fp_world_visible()) {
        return;
    }
    snprintf(fp_capture_path, sizeof(fp_capture_path), "%s", path);
}

static void fp_invalidate(bool contextGone)
{
    if (!contextGone && fp_gl_loaded) {
        if (fp_program != 0) {
            gl.DeleteProgram(fp_program);
        }
        if (fp_static_vbo != 0) {
            gl.DeleteBuffers(1, &fp_static_vbo);
        }
        if (fp_overhead_vbo != 0) {
            gl.DeleteBuffers(1, &fp_overhead_vbo);
        }
        if (fp_dynamic_vbo != 0) {
            gl.DeleteBuffers(1, &fp_dynamic_vbo);
        }
        if (fp_vao != 0 && gl.DeleteVertexArrays != NULL) {
            gl.DeleteVertexArrays(1, &fp_vao);
        }
        if (fp_fbo != 0) {
            gl.DeleteFramebuffers(1, &fp_fbo);
        }
        if (fp_depth_rb != 0) {
            gl.DeleteRenderbuffers(1, &fp_depth_rb);
        }
        if (fp_palette_tex != 0) {
            gl.DeleteTextures(1, &fp_palette_tex);
        }
        if (fp_frame_copy != 0) {
            gl.DeleteTextures(1, &fp_frame_copy);
        }
        if (fp_odepth_tex != 0) {
            gl.DeleteTextures(1, &fp_odepth_tex);
        }
        if (fp_odepth_fbo != 0) {
            gl.DeleteFramebuffers(1, &fp_odepth_fbo);
        }
        if (fp_odepth_rb != 0) {
            gl.DeleteRenderbuffers(1, &fp_odepth_rb);
        }
        for (FpPage& page : fp_pages) {
            if (page.tex != 0) {
                gl.DeleteTextures(1, &(page.tex));
            }
        }
    }
    fp_program = 0;
    fp_static_vbo = 0;
    fp_overhead_vbo = 0;
    fp_dynamic_vbo = 0;
    fp_vao = 0;
    fp_fbo = 0;
    fp_depth_rb = 0;
    fp_palette_tex = 0;
    fp_frame_copy = 0;
    fp_odepth_tex = 0;
    fp_odepth_fbo = 0;
    fp_odepth_rb = 0;
    fp_fill_w = 0;
    fp_fill_h = 0;
    fp_fill_failed = false;
    fp_fbo_target = 0;
    fp_fbo_w = 0;
    fp_fbo_h = 0;
    fp_static_uploaded = false;
    fp_overhead_uploaded = false;
    // the pages keep their pixels; each uploads whole again into a new texture
    for (FpPage& page : fp_pages) {
        page.tex = 0;
        page.dirtyY0 = 0;
        page.dirtyY1 = FP_PAGE_SIZE;
    }
    fp_gl_loaded = false;
}

// Is the world on screen -- not a menu, a conversation, the world map, a load?
static bool fp_world_visible()
{
    if (obj_dude == NULL || map_is_loading()) {
        return false;
    }
    if (win_get_top_modal() != -1 || gamepad_ui_menu_active() || dialog_active() || world_win != -1) {
        return false;
    }
    // a radial wheel is no modal window, but it takes the input and lives in the map area the 3D
    // view would paint over: the plain game shows while one is up
    if (radial_wheel_is_open()) {
        return false;
    }
    if (intface_is_hidden()) {
        return false;
    }
    return map_elevation >= 0 && map_elevation < ELEVATION_COUNT;
}

static void fp_bind_attributes()
{
    const GLsizei stride = FP_VERTEX_FLOATS * sizeof(float);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
    gl.EnableVertexAttribArray(3);
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(5 * sizeof(float)));
    gl.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(9 * sizeof(float)));
    gl.VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(10 * sizeof(float)));
}

static void fp_draw_ranges(const std::vector<FpRange>& ranges, int kind)
{
    for (const FpRange& range : ranges) {
        if (range.kind != kind || range.page < 0 || range.page >= (int)fp_pages.size()) {
            continue;
        }
        gl.BindTexture(GL_TEXTURE_2D, fp_pages[range.page].tex);
        gl.DrawArrays(GL_TRIANGLES, range.first, range.count);
    }
}

// The 3D view into the frame texture's map rows. True when it drew.
static bool fp_present(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, int texWidth, int texHeight)
{
    (void)window;
    if (fp_mode == FP_VIEW_OFF || fp_failed || renderer == NULL || texture == NULL) {
        fp_capture_path[0] = '\0';
        return false;
    }
    // whatever happens below, a capture asked for is this present's or nobody's
    struct CaptureDrop {
        ~CaptureDrop() { fp_capture_path[0] = '\0'; }
    } captureDrop;
    int mode = fp_mode;
    if (!fp_world_visible()) {
        return false;
    }
    int viewW = texWidth;
    int viewH = texHeight - INTERFACE_BAR_HEIGHT;
    if (viewW <= 0 || viewH <= 0) {
        return false;
    }

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0 || info.name == NULL
        || (strcmp(info.name, "opengl") != 0 && strcmp(info.name, "opengles2") != 0)) {
        trace_event(TRACE_WARN, "fp-view", "renderer '%s' is not a GL one -- off", info.name != NULL ? info.name : "?");
        fp_failed = true;
        return false;
    }
    if (!fp_load_gl() || (fp_program == 0 && !fp_build_program())) {
        fp_failed = true;
        return false;
    }

    SDL_RenderFlush(renderer);

    // The frame texture's GL name: SDL binds it for us, and says.
    gl.ActiveTexture(GL_TEXTURE0);
    float texScaleX = 1.0f;
    float texScaleY = 1.0f;
    if (SDL_GL_BindTexture(texture, &texScaleX, &texScaleY) != 0) {
        fp_failed = true;
        return false;
    }
    GLint target = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &target);
    SDL_GL_UnbindTexture(texture);
    if (target == 0 || texScaleX != 1.0f || texScaleY != 1.0f) {
        trace_event(TRACE_WARN, "fp-view", "frame texture not usable (name %d, scale %.2f x %.2f) -- off", target, texScaleX, texScaleY);
        fp_failed = true;
        return false;
    }

    // ---- the CPU side: the eye, the scene, this frame's critters
    int elevation = map_elevation;
    fp_update_eye();

    std::vector<Object*> statics;
    std::vector<Object*> part;
    static const int STATIC_TYPES[3] = { OBJ_TYPE_ITEM, OBJ_TYPE_SCENERY, OBJ_TYPE_WALL };
    for (int t = 0; t < 3; t++) {
        fp_collect(elevation, STATIC_TYPES[t], part);
        statics.insert(statics.end(), part.begin(), part.end());
    }
    std::vector<Object*> critters;
    fp_collect(elevation, OBJ_TYPE_CRITTER, critters);

    if (mode == FP_VIEW_DIABLO && obj_dude->elevation == elevation && (obj_dude->flags & OBJECT_HIDDEN) == 0) {
        critters.push_back(obj_dude); // seen from above, he is in the picture
    }

    fp_edits_follow_map();
    uint64_t sig = fp_signature(mode, elevation, statics);
    uint64_t lightSig = fp_light_signature(statics, critters);
    std::vector<float> dynVerts;
    std::vector<FpRange> dynRanges;
    for (int attempt = 0; attempt < 2; attempt++) {
        fp_atlas_overflow = false;
        bool ok = true;
        if (!fp_static_valid || sig != fp_static_sig || elevation != fp_static_elev) {
            ok = fp_build_static(mode, elevation, statics);
            fp_static_valid = ok;
            fp_static_sig = sig;
            fp_static_elev = elevation;
            fp_light_sig = lightSig; // built with the light as it is now
            fp_overhead_valid = false; // and what hangs over it is built again with that light
        } else if (lightSig != fp_light_sig) {
            fp_relight_static(elevation);
            fp_light_sig = lightSig;
        }
        if (ok) {
            uint64_t over = fp_overhead_signature(elevation);
            if (!fp_overhead_valid || over != fp_overhead_sig) {
                ok = fp_build_overhead(mode, elevation);
                fp_overhead_valid = ok;
                fp_overhead_sig = over;
            }
        }
        if (ok) {
            ok = fp_build_dynamic(mode, elevation, critters, dynVerts, dynRanges);
        }
        if (ok) {
            break;
        }
        // the atlas filled up: start it again and rebuild everything once
        trace_event(TRACE_FLOW, "fp-view", "atlas full (%d pages, %d sprites) -- reset", (int)fp_pages.size(), (int)fp_sprites.size());
        fp_atlas_reset();
        fp_static_valid = false;
        fp_overhead_valid = false;
        if (attempt == 1) {
            return false;
        }
    }

    // ---- the GL side. Everything SDL's renderer caches, or never re-applies, goes back as it was.
    GLint prevProgram = 0;
    GLint prevArrayBuffer = 0;
    GLint prevVertexArray = 0;
    GLint prevFramebuffer = 0;
    GLint prevRenderbuffer = 0;
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    GLint prevScissorBox[4] = { 0, 0, 0, 0 };
    GLint prevActiveTexture = GL_TEXTURE0;
    GLint prevTex0 = 0;
    GLint prevTex1 = 0;
    GLint prevTex2 = 0;
    GLint prevTex3 = 0;
    GLint prevDepthFunc = GL_LESS;
    GLint prevBlendSrc = GL_ONE;
    GLint prevBlendDst = GL_ZERO;
    GLint prevUnpackAlign = 4;
    GLint prevUnpackRow = 0;
    GLint prevPackAlign = 4;
    GLfloat prevClear[4] = { 0, 0, 0, 0 };
    GLboolean prevDepthMask = GL_TRUE;
    bool es = fp_context_is_es();
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
    if (fp_vao != 0) {
        gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);
    }
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
    gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &prevRenderbuffer);
    gl.GetIntegerv(GL_VIEWPORT, prevViewport);
    gl.GetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    gl.ActiveTexture(GL_TEXTURE3);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex3);
    gl.ActiveTexture(GL_TEXTURE2);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
    gl.GetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    gl.GetIntegerv(GL_BLEND_SRC, &prevBlendSrc);
    gl.GetIntegerv(GL_BLEND_DST, &prevBlendDst);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpackAlign);
    if (!es) {
        gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &prevUnpackRow);
    }
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlign);
    gl.GetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
    gl.GetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevBlend = gl.IsEnabled(GL_BLEND);
    GLboolean prevScissor = gl.IsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepth = gl.IsEnabled(GL_DEPTH_TEST);
    GLboolean prevCull = gl.IsEnabled(GL_CULL_FACE);
    GLboolean prevOffset = gl.IsEnabled(GL_POLYGON_OFFSET_FILL);
    // Without a VAO of our own the attribute arrays are the default state SDL's ES renderer set up
    // once and never again: they go back exactly as they were.
    GLint prevAttrib[5] = { 0, 0, 0, 0, 0 };
    if (fp_vao == 0) {
        for (int a = 0; a < 5; a++) {
            gl.GetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prevAttrib[a]);
        }
    }
    // SDL's ES renderer keeps an RGB888 frame byte-swapped and swaps red and blue back as it draws:
    // what we write into it must come swapped too
    bool swapRB = strcmp(info.name, "opengles2") == 0;

    bool drew = false;
    if (fp_ensure_fbo((GLuint)target, texWidth, texHeight)) {
        // uploads: pixel rows tightly packed, whatever SDL's last upload left behind
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!es) {
            gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        for (FpPage& page : fp_pages) {
            if (page.tex == 0) {
                gl.GenTextures(1, &(page.tex));
                gl.BindTexture(GL_TEXTURE_2D, page.tex);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, FP_PAGE_SIZE, FP_PAGE_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, page.px.data());
                page.dirtyY0 = page.dirtyY1 = 0;
            } else if (page.dirtyY1 > page.dirtyY0) {
                gl.BindTexture(GL_TEXTURE_2D, page.tex);
                gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, page.dirtyY0, FP_PAGE_SIZE, page.dirtyY1 - page.dirtyY0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                    page.px.data() + (size_t)page.dirtyY0 * FP_PAGE_SIZE);
                page.dirtyY0 = page.dirtyY1 = 0;
            }
        }

        // the palette as the 2D frame has it this very moment
        unsigned char pal[256 * 4];
        memset(pal, 0, sizeof(pal));
        if (gSdlSurface != NULL && gSdlSurface->format != NULL && gSdlSurface->format->palette != NULL) {
            SDL_Palette* palette = gSdlSurface->format->palette;
            for (int i = 0; i < 256 && i < palette->ncolors; i++) {
                pal[i * 4 + 0] = swapRB ? palette->colors[i].b : palette->colors[i].r;
                pal[i * 4 + 1] = palette->colors[i].g;
                pal[i * 4 + 2] = swapRB ? palette->colors[i].r : palette->colors[i].b;
                pal[i * 4 + 3] = 255;
            }
        }
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, fp_palette_tex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pal);
        gl.ActiveTexture(GL_TEXTURE0);

        if (fp_vao != 0) {
            gl.BindVertexArray(fp_vao);
        }
        if (!fp_static_uploaded) {
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
            gl.BufferData(GL_ARRAY_BUFFER, fp_static_verts.size() * sizeof(float), fp_static_verts.empty() ? NULL : fp_static_verts.data(), GL_STATIC_DRAW);
            fp_static_uploaded = true;
        }
        if (!fp_overhead_uploaded) {
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_overhead_vbo);
            gl.BufferData(GL_ARRAY_BUFFER, fp_overhead_verts.size() * sizeof(float), fp_overhead_verts.empty() ? NULL : fp_overhead_verts.data(), GL_STATIC_DRAW);
            fp_overhead_uploaded = true;
        }

        // ---- draw
        float ambient = (float)light_get_ambient() / 65536.0f;
        // Through his eyes a hazy wasteland sky; from Diablo's camera the game's black past the map.
        float fog[3] = { 0.0f, 0.0f, 0.0f };
        float fogStart = 1e9f;
        float fogEnd = 2e9f;
        if (mode == FP_VIEW_EYES && fp_static_under) {
            fogStart = (float)FP_CAVE_FOG_START; // underground: black, and the dark comes on with distance
            fogEnd = (float)FP_CAVE_FOG_END;
        } else if (mode == FP_VIEW_EYES) {
            fog[0] = FP_SKY[swapRB ? 2 : 0] * ambient;
            fog[1] = FP_SKY[1] * ambient;
            fog[2] = FP_SKY[swapRB ? 0 : 2] * ambient;
            fogStart = (float)FP_FOG_START;
            fogEnd = (float)FP_FOG_END;
        }
        // From Diablo's camera: the frame copy and the depth map from the game's camera (THE HOLES).
        bool fill = false;
        float origin[2] = { 0.0f, 0.0f };
        double depthC = FP_TO_CAMERA[1] * 0.0 + FP_TO_CAMERA[2] * fp_eye_z + FP_TO_CAMERA[0] * fp_eye_x;
        bool hasRock = false;
        for (const FpRange& range : fp_static_ranges) {
            hasRock = hasRock || range.kind == FP_KIND_ROCK;
        }
        if (mode == FP_VIEW_DIABLO && hasRock && fp_ensure_fill(viewW, viewH)) {
            // where the game's view stands: the dude's hex on screen against the same hex in the world
            int sx;
            int sy;
            int wx;
            int wy;
            tile_coord(obj_dude->tile, &sx, &sy, obj_dude->elevation);
            fp_hex_top_left(obj_dude->tile, &wx, &wy);
            origin[0] = (float)(wx - sx);
            origin[1] = (float)(wy - sy);

            // the game's own projection: x = 32 wx - ox, y = KZ wz - KY wy - oy (rows from the top), depth
            // along its rays round the dude
            double r = FP_FILL_RANGE;
            float ortho[16];
            memset(ortho, 0, sizeof(ortho));
            ortho[0] = (float)(2.0 * FP_S / viewW);
            ortho[5] = (float)(-2.0 * FP_KY / viewH);
            ortho[9] = (float)(2.0 * FP_KZ / viewH);
            ortho[2] = (float)(-FP_TO_CAMERA[0] / r);
            ortho[6] = (float)(-FP_TO_CAMERA[1] / r);
            ortho[10] = (float)(-FP_TO_CAMERA[2] / r);
            ortho[12] = (float)(-2.0 * origin[0] / viewW - 1.0);
            ortho[13] = (float)(-2.0 * origin[1] / viewH - 1.0);
            ortho[14] = (float)(depthC / r);
            ortho[15] = 1.0f;

            gl.BindFramebuffer(GL_FRAMEBUFFER, fp_odepth_fbo);
            gl.Viewport(0, 0, viewW, viewH);
            gl.Scissor(0, 0, viewW, viewH);
            gl.Enable(GL_SCISSOR_TEST);
            gl.ClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            gl.DepthMask(GL_TRUE);
            gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            gl.Disable(GL_BLEND);
            gl.Disable(GL_CULL_FACE);
            gl.Enable(GL_DEPTH_TEST);
            gl.DepthFunc(GL_LEQUAL);
            gl.UseProgram(fp_program);
            gl.UniformMatrix4fv(fp_loc.mvp, 1, GL_FALSE, ortho);
            gl.Uniform1i(fp_loc.atlas, 0);
            gl.Uniform1i(fp_loc.palette, 1);
            gl.Uniform2f(fp_loc.atlasSize, (float)FP_PAGE_SIZE, (float)FP_PAGE_SIZE);
            gl.Uniform4f(fp_loc.egg, 0.0f, 0.0f, 0.0f, 0.0f);
            gl.Uniform1f(fp_loc.pass, 1.0f);
            gl.Uniform1f(fp_loc.fill, 0.0f);
            gl.Uniform2f(fp_loc.edgeFlat, 0.0f, 0.0f);
            gl.Uniform3f(fp_loc.toCam, (float)FP_TO_CAMERA[0], (float)FP_TO_CAMERA[1], (float)FP_TO_CAMERA[2]);
            gl.Uniform2f(fp_loc.depthCR, (float)depthC, (float)FP_FILL_RANGE);
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
            fp_bind_attributes();
            fp_draw_ranges(fp_static_ranges, FP_KIND_ROCK); // only the rock hides what the fill fills

            // ... and the rock's own picture, lit as the 3D lights it, nearest rock first
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fp_frame_copy, 0);
            gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            gl.Uniform1f(fp_loc.pass, 0.0f);
            gl.Uniform1f(fp_loc.ambient, ambient);
            gl.Uniform3f(fp_loc.fog, 0.0f, 0.0f, 0.0f);
            gl.Uniform2f(fp_loc.fogRange, 1e9f, 2e9f);
            gl.Uniform1f(fp_loc.flip, 1.0f);
            gl.Uniform1f(fp_loc.opacity, 1.0f);
            fp_draw_ranges(fp_static_ranges, FP_KIND_ROCK);
            gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fp_odepth_tex, 0);
            fill = true;
        }

        auto drawScene = [&](const float* mvp) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, fp_fbo);
            gl.Viewport(0, 0, viewW, viewH);
            gl.Scissor(0, 0, viewW, viewH);
            gl.Enable(GL_SCISSOR_TEST);
            gl.ClearColor(fog[0], fog[1], fog[2], 1.0f);
            gl.DepthMask(GL_TRUE);
            // From Diablo's camera the 2D frame just uploaded stays under the 3D (his "30 degrees, and no
            // holes"): where the perspective opens a slot between two pieces of the picture -- the
            // cave's overlapping rock slopes -- the game's own picture at that spot shows through
            // instead of the black past the map. Through his eyes the sky or the dark is the background.
            gl.Clear(mode == FP_VIEW_DIABLO ? GL_DEPTH_BUFFER_BIT : (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
            gl.Disable(GL_BLEND);
            gl.Disable(GL_CULL_FACE);
            gl.Enable(GL_DEPTH_TEST);
            gl.DepthFunc(GL_LEQUAL);

            gl.UseProgram(fp_program);
            gl.UniformMatrix4fv(fp_loc.mvp, 1, GL_FALSE, mvp);
            {
                float flat[16];
                fp_diablo_flat_matrix(viewW, viewH, flat);
                gl.UniformMatrix4fv(fp_loc.flatMVP, 1, GL_FALSE, flat);
                // Flattening the lens toward the view's edges (it closed the seams between rock slopes
                // there) is OFF: walking, the picture swam -- "jako kdybych byl na LSD" (2026-10-31).
                // Kept only as a development trial.
                float e0 = 0.0f;
                float e1 = 0.0f;
#ifndef FALLOUT_RELEASE
                const char* edge = getenv("FALLOUT_FP_EDGEFLAT"); // development: "from,to", "0,0" = off
                if (mode == FP_VIEW_DIABLO && edge != NULL) {
                    sscanf(edge, "%f,%f", &e0, &e1);
                }
#endif
                gl.Uniform2f(fp_loc.edgeFlat, e0, e1);
            }
            gl.Uniform1i(fp_loc.atlas, 0);
            gl.Uniform1i(fp_loc.palette, 1);
            gl.Uniform2f(fp_loc.atlasSize, (float)FP_PAGE_SIZE, (float)FP_PAGE_SIZE);
            gl.Uniform1f(fp_loc.ambient, ambient);
            gl.Uniform3f(fp_loc.fog, fog[0], fog[1], fog[2]);
            gl.Uniform2f(fp_loc.fogRange, fogStart, fogEnd);
            gl.Uniform1f(fp_loc.flip, 1.0f);
            gl.Uniform1f(fp_loc.opacity, 1.0f);
            // the egg: from Diablo's camera only (through his eyes nothing stands between)
            float egg[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float eggDepth = 0.0f;
            if (mode == FP_VIEW_DIABLO && (obj_dude->flags & OBJECT_HIDDEN) == 0) {
                auto project = [&](double x, double y, double z, double* px, double* py, double* w) {
                    double cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
                    double cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
                    double cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
                    *w = cw;
                    *px = (cx / cw + 1.0) * 0.5 * viewW;
                    *py = (cy / cw + 1.0) * 0.5 * viewH;
                };
                double ex;
                double ey;
                double ew;
                project(fp_eye_x, 1.0, fp_eye_z, &ex, &ey, &ew);
                if (ew > 0.1) {
                    double rx;
                    double ry;
                    double rw;
                    project(fp_eye_x + FP_EGG_RX, 1.0, fp_eye_z, &rx, &ry, &rw);
                    double ux;
                    double uy;
                    double uw;
                    project(fp_eye_x, 1.0 + FP_EGG_RY, fp_eye_z, &ux, &uy, &uw);
                    egg[0] = (float)ex;
                    egg[1] = (float)ey;
                    egg[2] = (float)fabs(rx - ex);
                    egg[3] = (float)fabs(uy - ey);
                    eggDepth = (float)(ew - 0.2);
                }
            }
            gl.Uniform4f(fp_loc.egg, egg[0], egg[1], egg[2], egg[3]);
            gl.Uniform1f(fp_loc.eggDepth, eggDepth);
            gl.Uniform1f(fp_loc.pass, 0.0f);
            gl.Uniform1f(fp_loc.fill, fill ? 1.0f : 0.0f);
            gl.Uniform1i(fp_loc.frame, 2);
            gl.Uniform1i(fp_loc.odepth, 3);
            gl.Uniform2f(fp_loc.viewSize, (float)viewW, (float)viewH);
            gl.Uniform2f(fp_loc.origin, origin[0], origin[1]);
            gl.Uniform3f(fp_loc.toCam, (float)FP_TO_CAMERA[0], (float)FP_TO_CAMERA[1], (float)FP_TO_CAMERA[2]);
            gl.Uniform2f(fp_loc.depthCR, (float)depthC, (float)FP_FILL_RANGE);
            gl.Uniform2f(fp_loc.k, (float)FP_KZ, (float)FP_KY);
            gl.Uniform1f(fp_loc.eps, (float)(FP_FILL_EPS / (2.0 * FP_FILL_RANGE)));
            if (fill) {
                gl.ActiveTexture(GL_TEXTURE3);
                gl.BindTexture(GL_TEXTURE_2D, fp_odepth_tex);
                gl.ActiveTexture(GL_TEXTURE2);
                gl.BindTexture(GL_TEXTURE_2D, fp_frame_copy);
                gl.ActiveTexture(GL_TEXTURE0);
            }

            // what hangs over him (his ceiling, through his eyes), then the static scene
            if (mode == FP_VIEW_EYES && !fp_overhead_verts.empty()) {
                gl.BindBuffer(GL_ARRAY_BUFFER, fp_overhead_vbo);
                fp_bind_attributes();
                fp_draw_ranges(fp_overhead_ranges, FP_KIND_OPAQUE);
            }
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
            fp_bind_attributes();
            fp_draw_ranges(fp_static_ranges, FP_KIND_OPAQUE);
            fp_draw_ranges(fp_static_ranges, FP_KIND_ROCK);
            gl.Enable(GL_POLYGON_OFFSET_FILL);
            gl.PolygonOffset(-1.0f, -4.0f);
            fp_draw_ranges(fp_static_ranges, FP_KIND_DECAL);

            // this frame's critters and boards (themselves, never the 2D frame)
            gl.Uniform1f(fp_loc.fill, 0.0f);
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_dynamic_vbo);
            gl.BufferData(GL_ARRAY_BUFFER, dynVerts.size() * sizeof(float), dynVerts.empty() ? NULL : dynVerts.data(), GL_STREAM_DRAW);
            fp_bind_attributes();
            fp_draw_ranges(dynRanges, FP_KIND_DECAL);
            gl.Disable(GL_POLYGON_OFFSET_FILL);
            fp_draw_ranges(dynRanges, FP_KIND_OPAQUE);
            gl.Uniform1f(fp_loc.select, 0.0f);
            if (fp_editing) {
                gl.Enable(GL_POLYGON_OFFSET_FILL);
                gl.PolygonOffset(-1.0f, -8.0f);
                gl.Uniform1f(fp_loc.select, 1.0f);
                fp_draw_ranges(dynRanges, FP_KIND_SELECT);
                gl.Uniform1f(fp_loc.select, 0.0f);
                gl.Disable(GL_POLYGON_OFFSET_FILL);
            }

            // From Diablo's camera the roofs go over everything, depth or not, as the 2D game paints
            // them after every object: the walls under them stand whole and their tops stay hidden.
            if (mode == FP_VIEW_DIABLO && !fp_overhead_verts.empty()) {
                gl.Disable(GL_DEPTH_TEST);
                gl.BindBuffer(GL_ARRAY_BUFFER, fp_overhead_vbo);
                fp_bind_attributes();
                fp_draw_ranges(fp_overhead_ranges, FP_KIND_OPAQUE);
                gl.Enable(GL_DEPTH_TEST);
            }

            // translucent last, over everything, writing no depth
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
            fp_bind_attributes();
            gl.Enable(GL_BLEND);
            gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gl.DepthMask(GL_FALSE);
            gl.Uniform1f(fp_loc.opacity, 0.5f);
            fp_draw_ranges(fp_static_ranges, FP_KIND_TRANS);

            // the game's cursor on the map, over everything and never in the haze
            gl.Uniform2f(fp_loc.fogRange, 1e9f, 2e9f);
            gl.Disable(GL_BLEND);
            gl.DepthMask(GL_TRUE);
            gl.Uniform1f(fp_loc.opacity, 1.0f);
            gl.Uniform1f(fp_loc.fill, 0.0f);
            gl.Uniform4f(fp_loc.egg, 0.0f, 0.0f, 0.0f, 0.0f);
            gl.Disable(GL_DEPTH_TEST);
            gl.BindBuffer(GL_ARRAY_BUFFER, fp_dynamic_vbo);
            fp_bind_attributes();
            fp_draw_ranges(dynRanges, FP_KIND_OVERLAY);
            gl.Enable(GL_DEPTH_TEST);
        };

        auto capture = [&](const char* path) {
            // RGBA is the one read-back ES 2.0 guarantees; desktop takes it as well
            std::vector<unsigned char> rgba((size_t)viewW * viewH * 4);
            std::vector<unsigned char> rgb((size_t)viewW * viewH * 3);
            gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
            gl.ReadPixels(0, 0, viewW, viewH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            for (size_t p = 0; p < (size_t)viewW * viewH; p++) {
                rgb[p * 3 + 0] = rgba[p * 4 + (swapRB ? 2 : 0)];
                rgb[p * 3 + 1] = rgba[p * 4 + 1];
                rgb[p * 3 + 2] = rgba[p * 4 + (swapRB ? 0 : 2)];
            }
            fp_write_capture(path, viewW, viewH, rgb.data());
            trace_event(TRACE_FLOW, "fp-view", "captured %dx%d to %s", viewW, viewH, path);
        };

#ifndef FALLOUT_RELEASE
        // FALLOUT_FP_PANO=1 (development): a capture through his eyes also turns the head all the
        // way round first, six shots 60 degrees apart (scrNNNNN_fp_0..5.bmp, yaw 0 = screen right),
        // to judge a place without walking it
        const char* pano = getenv("FALLOUT_FP_PANO");
        if (fp_capture_path[0] != '\0' && mode == FP_VIEW_EYES && pano != NULL && pano[0] == '1') {
            double yaw = fp_yaw;
            double pitch = fp_pitch;
            fp_pitch = 0.0;
            for (int i = 0; i < 6; i++) {
                fp_yaw = i * FP_PI / 3.0;
                float turned[16];
                fp_camera_matrix(mode, viewW, viewH, turned);
                drawScene(turned);
                char path[sizeof(fp_capture_path) + 8];
                snprintf(path, sizeof(path), "%s", fp_capture_path);
                char* dot = strrchr(path, '.');
                snprintf(dot != NULL ? dot : path + strlen(path), 8, "_%d.bmp", i);
                capture(path);
            }
            fp_yaw = yaw;
            fp_pitch = pitch;
        }
#endif

        float mvp[16];
        fp_camera_matrix(mode, viewW, viewH, mvp);
        drawScene(mvp);
        if (fp_editing && mode == FP_VIEW_DIABLO) {
            // the editor's pointer: the game's own is drawn into the 2D frame, under the 3D
            int mx;
            int my;
            mouse_get_position(&mx, &my);
            auto box = [&](int x, int y, int w, int h, float r, float g, float b) {
                if (x < 0 || y < 0 || x + w > viewW || y + h > viewH) {
                    return;
                }
                gl.Scissor(x, y, w, h);
                gl.ClearColor(swapRB ? b : r, g, swapRB ? r : b, 1.0f);
                gl.Clear(GL_COLOR_BUFFER_BIT);
            };
            gl.BindFramebuffer(GL_FRAMEBUFFER, fp_fbo);
            gl.Enable(GL_SCISSOR_TEST);
            box(mx - 6, my - 1, 13, 3, 0.0f, 0.0f, 0.0f);
            box(mx - 1, my - 6, 3, 13, 0.0f, 0.0f, 0.0f);
            box(mx - 5, my, 11, 1, 1.0f, 0.9f, 0.2f);
            box(mx, my - 5, 1, 11, 1.0f, 0.9f, 0.2f);
            gl.Scissor(0, 0, viewW, viewH);
        }
        fp_edit_view_w = viewW;
        fp_edit_view_h = viewH;
        if (fp_capture_path[0] != '\0') {
            capture(fp_capture_path);
            fp_capture_path[0] = '\0';
        }

        for (int a = 0; a < 5; a++) {
            if (fp_vao == 0 && prevAttrib[a] != 0) {
                gl.EnableVertexAttribArray(a);
            } else {
                gl.DisableVertexAttribArray(a);
            }
        }
        drew = true;
    } else {
        fp_failed = true;
    }

    // ---- put it all back
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFramebuffer);
    gl.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)prevRenderbuffer);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuffer);
    if (fp_vao != 0) {
        gl.BindVertexArray((GLuint)prevVertexArray);
    }
    gl.UseProgram((GLuint)prevProgram);
    gl.ActiveTexture(GL_TEXTURE3);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex3);
    gl.ActiveTexture(GL_TEXTURE2);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex2);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex1);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex0);
    gl.ActiveTexture((GLenum)prevActiveTexture);
    gl.DepthMask(prevDepthMask);
    gl.DepthFunc((GLenum)prevDepthFunc);
    gl.BlendFunc((GLenum)prevBlendSrc, (GLenum)prevBlendDst);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, prevUnpackAlign);
    if (!es) {
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, prevUnpackRow);
    }
    gl.PixelStorei(GL_PACK_ALIGNMENT, prevPackAlign);
    gl.ClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    gl.Scissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    gl.Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) {
        gl.Enable(GL_BLEND);
    } else {
        gl.Disable(GL_BLEND);
    }
    if (prevScissor) {
        gl.Enable(GL_SCISSOR_TEST);
    } else {
        gl.Disable(GL_SCISSOR_TEST);
    }
    if (prevDepth) {
        gl.Enable(GL_DEPTH_TEST);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }
    if (prevCull) {
        gl.Enable(GL_CULL_FACE);
    } else {
        gl.Disable(GL_CULL_FACE);
    }
    if (prevOffset) {
        gl.Enable(GL_POLYGON_OFFSET_FILL);
    } else {
        gl.Disable(GL_POLYGON_OFFSET_FILL);
    }
    // SDL re-binds its own texture on its next draw only if it knows ours took the unit
    gl.ActiveTexture(GL_TEXTURE0);
    SDL_GL_UnbindTexture(texture);
    return drew;
}

// ---------------------------------------------------------------------------

// Whether the ray from eye along dir meets the quad c (corners in order round it) nearer than
// `before`, where its picture -- the sprite the game camera sees there, placed at ulx, uly on the
// original screen -- is opaque; *t is how far.
static bool fp_ray_hits_picture(const double eye[3], const double dir[3], const FpV3 c4[4], int sprite, int ulx, int uly, double before, double* t)
{
    static const int TRI[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    bool hit = false;
    for (int tri = 0; tri < 2; tri++) {
        const FpV3& a = c4[TRI[tri][0]];
        const FpV3& b = c4[TRI[tri][1]];
        const FpV3& c = c4[TRI[tri][2]];
        double e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
        double e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
        double p[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
        double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (fabs(det) < 1e-12) {
            continue;
        }
        double inv = 1.0 / det;
        double s[3] = { eye[0] - a.x, eye[1] - a.y, eye[2] - a.z };
        double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0.0 || u > 1.0) {
            continue;
        }
        double q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        double v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
        if (v < 0.0 || u + v > 1.0) {
            continue;
        }
        double d = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (d <= 0.0 || d >= before) {
            continue;
        }
        // only where the picture is: the sprite pixel the game camera sees at the hit
        double hx = eye[0] + dir[0] * d;
        double hy = eye[1] + dir[1] * d;
        double hz = eye[2] + dir[2] * d;
        const FpSprite& sp = fp_sprites[sprite];
        int ix = (int)floor(FP_S * hx - ulx);
        int iy = (int)floor(FP_KZ * hz - FP_KY * hy - uly);
        if (ix < 0 || iy < 0 || ix >= sp.w || iy >= sp.h || sp.px[(size_t)iy * sp.w + ix] == 0) {
            continue;
        }
        before = d;
        *t = d;
        hit = true;
    }
    return hit;
}

// The piece under the screen pixel (px, py) from Diablo's camera, or false.
static bool fp_edit_pick_at(int px, int py, FpEdit* key)
{
    int viewW = fp_edit_view_w;
    int viewH = fp_edit_view_h;
    if (viewW <= 0 || viewH <= 0 || px < 0 || py < 0 || px >= viewW || py >= viewH) {
        return false;
    }
    double fovy = fp_diablo_fovy();
    double tanHalf = tan(fovy / 2.0);
    double dist = viewH / (2.0 * FP_S * tanHalf);
    double eye[3] = { fp_eye_x + FP_TO_CAMERA[0] * dist, FP_TO_CAMERA[1] * dist, fp_eye_z + FP_TO_CAMERA[2] * dist };
    double fwd[3] = { -FP_TO_CAMERA[0], -FP_TO_CAMERA[1], -FP_TO_CAMERA[2] };
    double rx = -fwd[2];
    double rz = fwd[0];
    double rl = sqrt(rx * rx + rz * rz);
    rx /= rl;
    rz /= rl;
    double up[3] = { -rz * fwd[1], rz * fwd[0] - rx * fwd[2], rx * fwd[1] };
    double nx = 2.0 * (px + 0.5) / viewW - 1.0;
    double ny = 2.0 * (py + 0.5) / viewH - 1.0;
    double vx = nx * tanHalf * viewW / (double)viewH;
    double vy = -ny * tanHalf;
    double dir[3] = { rx * vx + up[0] * vy + fwd[0], up[1] * vy + fwd[1], rz * vx + up[2] * vy + fwd[2] };

    // a roof is painted over everything from Diablo's camera: where one is, nothing under it is seen
    double t;
    for (const FpRoofPick& roof : fp_roof_picks) {
        if (fp_ray_hits_picture(eye, dir, roof.c, roof.sprite, roof.ulx, roof.uly, 1e18, &t)) {
            return false;
        }
    }

    double best = 1e18;
    bool found = false;
    for (const FpPick& pick : fp_picks) {
        if (fp_ray_hits_picture(eye, dir, pick.c, pick.sprite, pick.ulx, pick.uly, best, &t)) {
            best = t;
            found = true;
            key->elevation = pick.elevation;
            key->tile = pick.tile;
            key->fid = pick.fid;
            key->copy = pick.copy;
        }
    }
    return found;
}

// The picked piece's edit, made when it has none.
static FpEdit* fp_edit_of_selection()
{
    if (!fp_edit_selected) {
        return NULL;
    }
    FpEdit* e = fp_edit_find(fp_edit_sel.elevation, fp_edit_sel.tile, fp_edit_sel.fid, fp_edit_sel.copy);
    if (e == NULL && fp_edit_sel.copy >= 0) {
        fp_edit_selected = false; // a copy that is gone
        return NULL;
    }
    if (e == NULL) {
        FpEdit fresh = fp_edit_sel;
        fresh.dx = 0;
        fresh.dy = 0;
        fresh.push = 0.0;
        fresh.rot = 0.0;
        fresh.hide = false;
        fp_edits.push_back(fresh);
        e = &fp_edits.back();
    }
    return e;
}

// Editing on or off. On, the world stands still as it does behind a menu: the map's background
// work -- animations, the critters' scripts, the clock and the timed events -- is stopped, and goes
// on again when editing ends, whatever ends it.
static bool fp_edit_bk_off = false;

static void fp_edit_set(bool on)
{
    if (on == fp_editing) {
        return;
    }
    fp_editing = on;
    fp_edit_selected = false;
    fp_edit_rev++;
    if (on) {
        fp_edit_bk_off = map_disable_bk_processes();
    } else if (fp_edit_bk_off) {
        map_enable_bk_processes();
        fp_edit_bk_off = false;
    }
    trace_event(TRACE_FLOW, "fp-edit", "editing=%d", on ? 1 : 0);
    char line[] = "3D EDIT: click pick, wheel push, Q/E turn, H hide, C copy, arrows move copy, R reset, Ctrl+Z undo, Esc done";
    char off[] = "3D EDIT: off, saved";
    display_print(on ? line : off);
}

static void fp_edit_changed()
{
    fp_edit_rev++;
    fp_edits_save();
}

static void fp_edit_remember()
{
    fp_edit_undo.push_back(fp_edits);
    if (fp_edit_undo.size() > 200) {
        fp_edit_undo.erase(fp_edit_undo.begin());
    }
}

bool fp_view_editing()
{
    if (fp_editing && (fp_mode != FP_VIEW_DIABLO || fp_failed)) {
        fp_edit_set(false); // the 3D gave out under the editor: the world goes on again
    }
    return fp_editing;
}

void fp_view_edit_toggle()
{
    if (fp_mode != FP_VIEW_DIABLO) {
        fp_mode = FP_VIEW_DIABLO; // Shift+F8 from elsewhere: into Diablo's view, editing
        fp_yaw_set = false;
        fp_edit_set(true);
        return;
    }
    fp_edit_set(!fp_editing);
}

void fp_view_edit_input(int keyCode)
{
    if (!fp_view_editing()) {
        return;
    }
    if (keyCode == KEY_ESCAPE) {
        fp_view_edit_toggle();
        return;
    }
    int mx;
    int my;
    mouse_get_position(&mx, &my);
    bool click = (mouse_get_buttons() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0;
#ifndef FALLOUT_RELEASE
    // FALLOUT_FP_EDITPICK=x,y (development, the self-played tests): one click there, once
    static bool testPicked = false;
    const char* testPick = getenv("FALLOUT_FP_EDITPICK");
    if (!testPicked && testPick != NULL && sscanf(testPick, "%d,%d", &mx, &my) == 2) {
        testPicked = true;
        click = true;
    }
#endif
    if (click) {
        FpEdit key = {};
        fp_edit_selected = fp_edit_pick_at(mx, my, &key);
        trace_event(TRACE_FLOW, "fp-edit", "click x=%d y=%d picks=%d hit=%d", mx, my, (int)fp_picks.size(), fp_edit_selected ? 1 : 0);
        if (fp_edit_selected) {
            fp_edit_sel = key;
            trace_event(TRACE_FLOW, "fp-edit", "pick tile=%d fid=%x copy=%d", key.tile, key.fid, key.copy);
        }
        fp_edit_rev++; // the mark moves
    }
    if (keyCode == KEY_CTRL_Z) {
        if (!fp_edit_undo.empty()) {
            fp_edits = fp_edit_undo.back();
            fp_edit_undo.pop_back();
            if (fp_edit_selected && fp_edit_sel.copy >= 0
                && fp_edit_find(fp_edit_sel.elevation, fp_edit_sel.tile, fp_edit_sel.fid, fp_edit_sel.copy) == NULL) {
                fp_edit_selected = false;
            }
            fp_edit_changed();
        }
        return;
    }
    if (!fp_edit_selected) {
        return;
    }
    int wheelX;
    int wheelY;
    mouseGetWheel(&wheelX, &wheelY);
    double push = wheelY * 4.0;
    double rot = 0.0;
    int dx = 0;
    int dy = 0;
    switch (keyCode) {
    case KEY_PAGE_UP:
        push += 4.0;
        break;
    case KEY_PAGE_DOWN:
        push -= 4.0;
        break;
    case 'q':
        rot -= 5.0;
        break;
    case 'Q':
        rot -= 15.0;
        break;
    case 'e':
        rot += 5.0;
        break;
    case 'E':
        rot += 15.0;
        break;
    case KEY_ARROW_LEFT:
        dx = -4;
        break;
    case KEY_ARROW_RIGHT:
        dx = 4;
        break;
    case KEY_ARROW_UP:
        dy = -4;
        break;
    case KEY_ARROW_DOWN:
        dy = 4;
        break;
    case 'h':
    case 'H': {
        fp_edit_remember();
        FpEdit* e = fp_edit_of_selection();
        if (e == NULL) {
            return;
        }
        e->hide = !e->hide;
        fp_edit_changed();
        return;
    }
    case 'c':
    case 'C': {
        fp_edit_remember();
        FpEdit* src = fp_edit_of_selection();
        if (src == NULL) {
            return;
        }
        FpEdit copy = *src; // the copy push_back below may move fp_edits: src is not used after it
        int next = 0;
        for (const FpEdit& e : fp_edits) {
            if (e.elevation == copy.elevation && e.tile == copy.tile && e.fid == copy.fid) {
                next = std::max(next, e.copy + 1);
            }
        }
        copy.copy = next;
        copy.dx = src->copy >= 0 ? src->dx + 16 : 16;
        copy.dy = src->copy >= 0 ? src->dy : 0;
        copy.hide = false;
        fp_edits.push_back(copy);
        fp_edit_sel.copy = next;
        fp_edit_changed();
        return;
    }
    case 'r':
    case 'R':
    case KEY_DELETE: {
        fp_edit_remember();
        for (size_t i = 0; i < fp_edits.size(); i++) {
            const FpEdit& e = fp_edits[i];
            if (e.elevation == fp_edit_sel.elevation && e.tile == fp_edit_sel.tile && e.fid == fp_edit_sel.fid && e.copy == fp_edit_sel.copy) {
                fp_edits.erase(fp_edits.begin() + i);
                break;
            }
        }
        if (fp_edit_sel.copy >= 0) {
            fp_edit_selected = false; // a copy reset is a copy gone
        }
        fp_edit_changed();
        return;
    }
    default:
        break;
    }
    if (push == 0.0 && rot == 0.0 && dx == 0 && dy == 0) {
        return;
    }
    if (fp_edit_sel.copy < 0 && (dx != 0 || dy != 0)) {
        return; // only a copy moves on the screen; the object itself stays where the game draws it
    }
    fp_edit_remember();
    FpEdit* e = fp_edit_of_selection();
    if (e == NULL) {
        return;
    }
    e->push += push;
    e->rot += rot;
    e->dx += dx;
    e->dy += dy;
    fp_edit_changed();
}

void fp_view_init()
{
    svga_fp_present = fp_present;
    svga_fp_invalidate = fp_invalidate;
    svga_fp_request_capture = fp_request_capture;
    const char* env = getenv("FALLOUT_FP_VIEW");
    if (env != NULL && (env[0] == '1' || env[0] == '2')) {
        fp_mode = env[0] == '1' ? FP_VIEW_EYES : FP_VIEW_DIABLO;
        trace_event(TRACE_FLOW, "fp-view", "mode=%d (FALLOUT_FP_VIEW)", fp_mode);
    }
}

void fp_view_toggle(int mode)
{
    fp_edit_set(false); // switching view ends any editing
    fp_mode = fp_mode == mode ? FP_VIEW_OFF : mode;
    fp_yaw_set = false;
    fp_pitch = 0.0;
    fp_capture_path[0] = '\0';
    trace_event(TRACE_FLOW, "fp-view", "mode=%d%s", fp_mode, fp_failed ? " (failed earlier: stays 2D)" : "");
}

int fp_view_mode()
{
    return fp_failed ? FP_VIEW_OFF : fp_mode;
}

} // namespace fallout
