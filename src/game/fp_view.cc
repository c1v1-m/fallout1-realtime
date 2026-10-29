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
#include "game/game_trace.h"
#include "game/gamepad_ui.h"
#include "game/gdialog.h"
#include "game/intface.h"
#include "game/light.h"
#include "game/map.h"
#include "game/object.h"
#include "game/proto.h"
#include "game/proto_types.h"
#include "game/radial_wheel.h"
#include "game/worldmap.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/gnw.h"
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

// The eye (viewer.html first person: critter tops are ~2.0, so 1.9 looked over everyone's head).
static const double FP_EYE = 1.72;
static const double FP_NEAR = 0.05;
static const double FP_FAR = 120.0;
static const double FP_FOG_START = 18.0;
static const double FP_FOG_END = 75.0;
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
    FP_KIND_COUNT,
};

#define FP_VERTEX_FLOATS 11

struct FpQuad {
    int kind;
    int page;
    int lightTile; // the hex its light is read from, -1 = a fixed light (roofs, see-through walls)
    float v[6 * FP_VERTEX_FLOATS]; // two triangles
};

// The hex the quads being emitted take their light from (fp_quad_fill), set by whoever emits them.
static int fp_emit_light_tile = -1;

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
    quad->lightTile = fp_emit_light_tile;
    for (int i = 0; i < 6; i++) {
        int k = order[i];
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
        v[9] = light;
        v[10] = back;
    }
}

// A quad textured PROJECTIVELY: each corner shows the sprite pixel the original camera sees there
// (with `shift`, a face behind the front samples the front -- the back of a box is its mirror).
static bool fp_emit_proj(std::vector<FpQuad>& out, int kind, int sprite, double sx, double sy, const FpV3 c[4], const FpV3* shift, float light, float back)
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
    FpQuad quad;
    fp_quad_fill(&quad, kind, s, c, pix, light, back, fp_front_is_ccw(c));
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

// A standing sprite: a wall, a door, a piece of scenery, a container.
struct FpPiece {
    int sprite;
    int ulx;
    int uly;
    int w;
    int h;
    int tile;
    int anchorY; // its hex foot, for drawing a board's pieces back to front
    bool trans;
    bool door;
    float light;
    int lightTile; // fp_light_tile_of
    int flags; // the object's, for its light in a board
    std::vector<double> bottom;
    double lowest;
    FpTable tables[3];
    std::vector<FpSeg> segs;
    int group; // -1 = none (walls, doors, translucent pieces)
    bool boarded;
};

// Big scenery is often several sprites on neighbouring hexes; fitted one by one it came apart in
// strips. Pieces whose rectangles overlap or touch and stand on nearly the same line are ONE object
// with one ground under their combined bottom edge.
static void fp_group_scenery(std::vector<FpPiece>& pieces, const std::vector<int>& ids, std::vector<std::vector<int>>& groups)
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
        for (int id : members) {
            const FpPiece& p = pieces[id];
            for (int c = 0; c < p.w; c++) {
                double b = p.bottom[c];
                if (std::isnan(b)) {
                    continue;
                }
                int x = p.ulx + c - gx0;
                comb[x] = std::isnan(comb[x]) ? b : std::max(comb[x], b);
            }
        }
        // a card is the safe default for a lump; an axis (or a corner) must beat it clearly
        static const int CARD[1] = { 0 };
        static const int AXES[2] = { FP_AXIS_A, FP_AXIS_B };
        FpFit fit = fp_fit_ground(comb, gx0, CARD, 1, false, 0);
        if (gx1 - gx0 >= 40) {
            FpFit shaped = fp_fit_ground(comb, gx0, AXES, 2, true, 0);
            if (shaped.cost <= fit.cost * 0.5) {
                fit = shaped;
            }
        }
        for (int id : members) {
            FpPiece& p = pieces[id];
            p.group = g;
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
        }
    }
}

// The pieces of one wall each fit their own line and opened gaps between them. Pieces with touching
// stretches on the same axis are one wall when they stand on the same hex line, or their lines
// nearly agree; the whole chain takes its front-most line. A corner's two lines are then made to
// meet again.
static void fp_snap_wall_runs(std::vector<FpPiece>& pieces, const std::vector<int>& ids)
{
    std::vector<FpSeg*> segs;
    for (int id : ids) {
        FpPiece& p = pieces[id];
        for (FpSeg& s : p.segs) {
            if (s.axis == 0) {
                continue;
            }
            s.line = s.axis == FP_AXIS_A ? 1000 + p.tile / 200 : 2000 + p.tile % 200;
            segs.push_back(&s);
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

    std::unordered_map<int, double> front;
    for (int i = 0; i < n; i++) {
        int r = find(i);
        auto it = front.find(r);
        front[r] = it == front.end() ? segs[i]->k : std::max(it->second, segs[i]->k);
    }
    for (int i = 0; i < n; i++) {
        segs[i]->k = front[find(i)];
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
// -- and in Diablo's view which roofs the game has lifted. An object that is ANIMATING counts
// without its frame (a door swinging is one rebuild when it stops, not one per frame; something that
// animates forever stays on the frame it had).
static uint64_t fp_signature(int mode, int elevation, const std::vector<Object*>& objects)
{
    uint64_t h = 1469598103934665603ull;
    fp_hash(&h, (uint32_t)mode);
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
    if (mode == FP_VIEW_DIABLO && square[elevation] != NULL) {
        for (int sq = 0; sq < SQUARE_GRID_SIZE; sq++) {
            int roof = (square[elevation]->field_0[sq] >> 16) & 0xFFFF;
            if (((roof >> 12) & 1) != 0) {
                fp_hash(&h, (uint32_t)sq);
            }
        }
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

static void fp_relight_static(int elevation)
{
    size_t vertices = fp_static_light_tiles.size();
    for (size_t i = 0; i < vertices; i++) {
        int tile = fp_static_light_tiles[i];
        if (tile >= 0) {
            fp_static_verts[i * FP_VERTEX_FLOATS + 9] = (float)light_get_tile(elevation, tile) / 65536.0f;
        }
    }
    fp_static_uploaded = false;
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

// The panel, its lid and its box (geometry.js levelQuads). `cap`: cut at 96 px with the rest of the
// sprite -- its top, seen from above -- laid flat as a lid (the --doom build cuts every wall; the
// game's own angle only those under a roof). `boxes`: a box as deep as that top shows behind it.
static bool fp_emit_panel(std::vector<FpQuad>& out, const FpPiece& p, const FpSeg& seg, int cap, bool boxes)
{
    int axis = seg.axis;
    if (axis == 0) {
        cap = 0;
    }
    double m = FP_SLOPES[axis];
    double x = p.ulx;
    double y = p.uly;
    double b0 = seg.k + m * seg.x0;
    double b1 = seg.k + m * seg.x1;
    int kind = p.trans ? FP_KIND_TRANS : FP_KIND_OPAQUE;
    float back = p.trans ? 0.55f : (axis == 0 ? 0.8f : 0.55f);
    double t0 = cap != 0 ? std::min(96.0, b0 - y) : b0 - y;
    double t1 = cap != 0 ? std::min(96.0, b1 - y) : b1 - y;
    FpV3 f[4] = { fp_w(seg.x0, b0, 0.0), fp_w(seg.x1, b1, 0.0), fp_w(seg.x1, b1 - t1, t1 / FP_KY), fp_w(seg.x0, b0 - t0, t0 / FP_KY) };
    if (!fp_emit_proj(out, kind, p.sprite, x, y, f, NULL, p.light, back)) {
        return false;
    }

    double capDepth = 0.0;
    bool lid = false;
    if (cap == 1 && (b0 - y > 96.0 || b1 - y > 96.0)) {
        double back0 = std::min(y, b0 - 96.0);
        double back1 = std::min(y, b1 - 96.0);
        FpV3 c[4] = { fp_w(seg.x0, b0 - 96.0, FP_H_ROOF), fp_w(seg.x1, b1 - 96.0, FP_H_ROOF), fp_w(seg.x1, back1, FP_H_ROOF), fp_w(seg.x0, back0, FP_H_ROOF) };
        if (!fp_emit_proj(out, kind, p.sprite, x, y, c, NULL, p.light, back)) {
            return false;
        }
        capDepth = ((b0 - 96.0 - back0) + (b1 - 96.0 - back1)) / 2.0 / FP_KZ;
        lid = true;
    }

    if (p.trans || !boxes) {
        return true;
    }

    // A wall is a box, not a sheet; a scenery card gets depth too (half its width).
    const double* nrm = FP_WALL_NORMAL[axis];
    double t;
    if (axis != 0) {
        t = capDepth > 0.0 ? std::max(0.15, capDepth * nrm[2]) : 0.25;
    } else {
        t = std::min(1.0, std::max(0.2, ((seg.x1 - seg.x0) / FP_S) * 0.5));
    }
    FpV3 bk[4];
    for (int i = 0; i < 4; i++) {
        bk[i] = { f[i].x - nrm[0] * t, f[i].y, f[i].z - nrm[2] * t };
    }
    FpV3 s = { nrm[0] * t, 0.0, nrm[2] * t };
    FpV3 o = { 0.0, 0.0, 0.0 };
    FpV3 shiftAll[4] = { s, s, s, s };
    FpV3 shiftHalf[4] = { o, o, s, s };
    if (!fp_emit_proj(out, kind, p.sprite, x, y, bk, shiftAll, p.light, back)) {
        return false;
    }
    // The box's top would lie on the lid (both at the 96 px line) and fight it for every pixel;
    // the lid is the top, carrying the sprite's real top pixels.
    if (!lid) {
        FpV3 top[4] = { f[3], f[2], bk[2], bk[3] };
        if (!fp_emit_proj(out, kind, p.sprite, x, y, top, shiftHalf, p.light, back)) {
            return false;
        }
    }
    FpV3 end0[4] = { f[0], f[3], bk[3], bk[0] };
    FpV3 end1[4] = { f[1], f[2], bk[2], bk[1] };
    if (!fp_emit_proj(out, kind, p.sprite, x, y, end0, shiftHalf, p.light, back)) {
        return false;
    }
    return fp_emit_proj(out, kind, p.sprite, x, y, end1, shiftHalf, p.light, back);
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
            lightTiles->insert(lightTiles->end(), 6, q.lightTile);
        }
    }
}

// Is there a roof over the ground point (px, py) (build.js roofAt: the square under it and its
// neighbours)? Diablo's view cuts a wall at 96 px only where it stands under one.
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
    int roofs = 0;

    // Squares: the whole 80x36 floor rect (its transparency interlocks the tiles; clipping left
    // seams), and the roof 96 px up. Through his eyes a roof stays even where the game has lifted it
    // over the dude, so an interior has its ceiling; from Diablo's camera it goes as in the game.
    TileData* squares = square[elevation];
    for (int sq = 0; squares != NULL && sq < SQUARE_GRID_SIZE; sq++) {
        int v = squares->field_0[sq];
        int floor = v & 0xFFFF;
        int roof = (v >> 16) & 0xFFFF;
        int tx;
        int ty;
        fp_square_top_left(sq, &tx, &ty);
        // the hex under the square, for its light (2D interpolates ten; one is enough for a first look)
        int lightTile = 400 * (sq / 100) + 2 * (sq % 100) + 1;
        if (((floor >> 12) & 1) == 0 && (floor & 0xFFF) != 1) {
            int id = fp_sprite(art_id(OBJ_TYPE_TILE, floor & 0xFFF, 0, 0, 0), 0, 0, false);
            if (id >= 0) {
                FpV3 c[4] = { fp_w(tx, ty, 0.0), fp_w(tx + 80, ty, 0.0), fp_w(tx + 80, ty + 36, 0.0), fp_w(tx, ty + 36, 0.0) };
                float light = lightTile < HEX_GRID_SIZE ? (float)light_get_tile(elevation, lightTile) / 65536.0f : 0.0f;
                fp_emit_light_tile = lightTile < HEX_GRID_SIZE ? lightTile : -1;
                if (!fp_emit_proj(quads, FP_KIND_OPAQUE, id, tx, ty, c, NULL, light, 1.0f)) {
                    return false;
                }
                floors++;
            }
        }
        bool lifted = ((roof >> 12) & 1) != 0;
        if ((roof & 0xFFF) != 0 && (roof & 0xFFF) != 1 && (eyes || !lifted)) {
            int id = fp_sprite(art_id(OBJ_TYPE_TILE, roof & 0xFFF, 0, 0, 0), 0, 0, false);
            if (id >= 0) {
                int ry = ty - 96;
                FpV3 c[4] = { fp_w(tx, ry, FP_H_ROOF), fp_w(tx + 80, ry, FP_H_ROOF), fp_w(tx + 80, ry + 36, FP_H_ROOF), fp_w(tx, ry + 36, FP_H_ROOF) };
                // roofs are lit by the ambient light alone
                fp_emit_light_tile = -1;
                if (!fp_emit_proj(quads, FP_KIND_OPAQUE, id, tx, ry, c, NULL, 0.0f, 1.0f)) {
                    return false;
                }
                roofs++;
            }
        }
    }

    // Objects: flat ones become decals, the standing ones pieces on fitted ground lines.
    std::vector<FpPiece> pieces;
    pieces.reserve(objects.size());
    int decalOrder = 0;
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
        p.trans = trans;
        p.door = type == OBJ_TYPE_SCENERY && subtype == SCENERY_TYPE_DOOR;
        p.light = light;
        p.lightTile = fp_light_tile_of(obj->tile, obj->flags);
        p.flags = obj->flags;
        p.group = -1;
        p.boarded = false;
        p.bottom.assign(s.w, std::numeric_limits<double>::quiet_NaN());
        p.lowest = -std::numeric_limits<double>::infinity();
        for (int c = 0; c < s.w; c++) {
            for (int r = s.h - 1; r >= 0; r--) {
                if (s.px[(size_t)r * s.w + c] != 0) {
                    p.bottom[c] = uly + r + 1;
                    p.lowest = std::max(p.lowest, p.bottom[c]);
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
                p.segs = fp_fit_ground(p.bottom, ulx, AXES, 2, true, 0).segs;
            }
        }
        pieces.push_back(std::move(p));
    }

    // Scenery into objects, translucent pieces as plain cards.
    std::vector<int> loose;
    for (int i = 0; i < (int)pieces.size(); i++) {
        if (pieces[i].segs.empty() && !pieces[i].trans) {
            loose.push_back(i);
        }
    }
    std::vector<std::vector<int>> groups;
    fp_group_scenery(pieces, loose, groups);
    static const int CARD[1] = { 0 };
    for (FpPiece& p : pieces) {
        if (p.segs.empty()) {
            p.segs = fp_fit_ground(p.bottom, p.ulx, CARD, 1, false, 0).segs;
        }
    }

    // A lump that fits as a card stands up as ONE board turned to the eye, its pieces where the game
    // drew them, back to front, its lowest pixel on the ground. Anything along a wall axis stays.
    // (From the game's own camera a card already is the picture: no boards there.)
    for (const std::vector<int>& g : groups) {
        bool allCards = eyes && !g.empty();
        for (int id : g) {
            const FpPiece& q = pieces[id];
            if (q.door || q.trans || q.segs.size() != 1 || q.segs[0].axis != 0) {
                allCards = false;
                break;
            }
        }
        if (!allCards) {
            continue;
        }
        int minX = std::numeric_limits<int>::max();
        int maxX = std::numeric_limits<int>::min();
        double lowest = -std::numeric_limits<double>::infinity();
        for (int id : g) {
            minX = std::min(minX, pieces[id].ulx);
            maxX = std::max(maxX, pieces[id].ulx + pieces[id].w);
            lowest = std::max(lowest, pieces[id].lowest);
        }
        double ax = (minX + maxX) / 2.0;
        double ay = lowest + 1.0;
        std::vector<int> back = g;
        std::stable_sort(back.begin(), back.end(), [&](int a, int b) { return pieces[a].anchorY < pieces[b].anchorY; });
        FpBoard board;
        board.anchor = fp_w(ax, ay, 0.0);
        for (int id : back) {
            const FpPiece& q = pieces[id];
            board.pieces.push_back({ q.sprite, q.ulx - ax, q.uly - ay, q.tile, q.flags });
            pieces[id].boarded = true;
        }
        fp_boards.push_back(std::move(board));
    }

    std::vector<int> standing;
    for (int i = 0; i < (int)pieces.size(); i++) {
        if (!pieces[i].boarded && !pieces[i].trans) {
            standing.push_back(i);
        }
    }
    fp_snap_wall_runs(pieces, standing);

    int panels = 0;
    for (const FpPiece& p : pieces) {
        if (p.boarded) {
            continue;
        }
        fp_emit_light_tile = p.lightTile;
        for (const FpSeg& seg : p.segs) {
            // through his eyes every wall is cut and boxed; from the game's camera only a wall under a
            // roof is cut (its top would poke through), and none is boxed (the sides smeared)
            double xm = (seg.x0 + seg.x1) / 2.0;
            int cap = eyes || fp_roof_at(elevation, xm, seg.k + FP_SLOPES[seg.axis] * xm) ? 1 : 0;
            if (!fp_emit_panel(quads, p, seg, cap, eyes)) {
                return false;
            }
            panels++;
        }
    }

    fp_emit_light_tile = -1;
    fp_flush_quads(quads, fp_static_verts, fp_static_ranges, &fp_static_light_tiles);
    fp_static_uploaded = false;

    double ms = (double)(SDL_GetPerformanceCounter() - started) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    trace_event(TRACE_FLOW, "fp-scene", "mode=%d elev=%d floors=%d roofs=%d objects=%d panels=%d boards=%d quads=%d sprites=%d pages=%d ms=%.1f",
        mode, elevation, floors, roofs, (int)objects.size(), panels, (int)fp_boards.size(), (int)quads.size(),
        (int)fp_sprites.size(), (int)fp_pages.size(), ms);
    return true;
}

// ---------------------------------------------------------------------------
// The eye.
// ---------------------------------------------------------------------------

static double fp_eye_x = 0.0;
static double fp_eye_z = 0.0;
static double fp_yaw = 0.0; // the look direction on the ground: 0 = +x (screen right), towards +z
static bool fp_yaw_set = false;
static Uint32 fp_last_ticks = 0;

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
    fp_eye_x = wx / FP_S;
    fp_eye_z = wy / FP_KZ;

    // Rotation r faces (r - 1) * 60 degrees (ROTATION_E = 1 along +x); eased, so a turn of the
    // six-way facing is a turn of the head, not a cut.
    double target = (std::min(std::max(obj_dude->rotation, 0), 5) - 1) * FP_PI / 3.0;
    Uint32 now = SDL_GetTicks();
    double dt = fp_last_ticks != 0 ? (now - fp_last_ticks) / 1000.0 : 0.0;
    fp_last_ticks = now;
    if (!fp_yaw_set || dt > 0.5) {
        fp_yaw = target;
        fp_yaw_set = true;
    } else {
        double diff = atan2(sin(target - fp_yaw), cos(target - fp_yaw));
        fp_yaw += diff * std::min(1.0, dt * 8.0);
    }
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

static void fp_camera_matrix(int mode, int viewW, int viewH, float* mvp)
{
    double aspect = (double)viewW / (double)viewH;
    float proj[16];
    float view[16];
    if (mode == FP_VIEW_DIABLO) {
        // the view is viewH game pixels tall where it meets the ground under the dude, S to a unit
        double dist = viewH / (2.0 * FP_S * tan(FP_DIABLO_FOVY / 2.0));
        double eye[3] = { fp_eye_x + FP_TO_CAMERA[0] * dist, FP_TO_CAMERA[1] * dist, fp_eye_z + FP_TO_CAMERA[2] * dist };
        double fwd[3] = { -FP_TO_CAMERA[0], -FP_TO_CAMERA[1], -FP_TO_CAMERA[2] };
        fp_perspective(FP_DIABLO_FOVY, aspect, 0.5, 400.0, proj);
        fp_look_along(eye, fwd, view);
    } else {
        // 90 degrees ACROSS whatever the shape of the view (a fixed vertical angle shrank everyone)
        fp_perspective(2.0 * atan(1.0 / aspect), aspect, FP_NEAR, FP_FAR, proj);
        double eye[3] = { fp_eye_x, FP_EYE, fp_eye_z };
        double fwd[3] = { cos(fp_yaw), 0.0, sin(fp_yaw) };
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

    for (const FpBoard& board : fp_boards) {
        double psi = atan2(board.anchor.x - fp_eye_x, -(board.anchor.z - fp_eye_z));
        for (int i = 0; i < (int)board.pieces.size(); i++) {
            const FpBoardPiece& piece = board.pieces[i];
            if (!fp_emit_billboard(quads, piece.sprite, board.anchor, psi, piece.dx0, piece.dy0, i * 0.004, fp_light_of(elevation, piece.tile, piece.flags))) {
                return false;
            }
        }
    }

    fp_flush_quads(quads, verts, ranges, NULL);
    return true;
}

// ---------------------------------------------------------------------------
// GL objects.
// ---------------------------------------------------------------------------

static GLuint fp_program = 0;
static GLuint fp_static_vbo = 0;
static GLuint fp_dynamic_vbo = 0;
static GLuint fp_vao = 0;
static GLuint fp_fbo = 0;
static GLuint fp_depth_rb = 0;
static GLuint fp_palette_tex = 0;
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
} fp_loc;

static const char* FP_VERTEX_SHADER = R"GLSL(
uniform mat4 uMVP;
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
void main() {
    vPix = aPix;
    vRect = aRect;
    vLight = aLight;
    vBack = aBack;
    vec4 p = uMVP * vec4(aPos, 1.0);
    vDepth = p.w;
    gl_Position = p;
}
)GLSL";

// The sprite pixel under the fragment (outside the sprite or palette index 0: nothing), through the
// live palette (colour cycling, gamma and fades come with it), darkened by its light as the 2D
// renderer does (never the glowing indices 229..255), its unpainted back darker, then the haze.
static const char* FP_FRAGMENT_SHADER = R"GLSL(
uniform sampler2D uAtlas;
uniform sampler2D uPal;
uniform vec2 uAtlasSize;
uniform float uAmbient;
uniform vec3 uFog;
uniform vec2 uFogRange;
uniform float uFlip;
uniform float uOpacity;
varying vec2 vPix;
varying vec4 vRect;
varying float vLight;
varying float vBack;
varying float vDepth;
void main() {
    vec2 l = floor(vPix);
    if (l.x < 0.0 || l.y < 0.0 || l.x >= vRect.z || l.y >= vRect.w) discard;
    float idx = texture2D(uAtlas, (vRect.xy + l + 0.5) / uAtlasSize).r * 255.0;
    if (idx < 0.5) discard;
    vec3 c = texture2D(uPal, vec2((floor(idx + 0.5) + 0.5) / 256.0, 0.5)).rgb;
    float lit = idx > 228.5 ? 1.0 : clamp(max(uAmbient, vLight), 0.0, 1.0);
    bool front = gl_FrontFacing;
    if (uFlip > 0.5) front = !front;
    c *= lit * (front ? 1.0 : vBack);
    c = mix(c, uFog, smoothstep(uFogRange.x, uFogRange.y, vDepth));
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

    if (gl.GenVertexArrays != NULL && gl.BindVertexArray != NULL) {
        gl.GenVertexArrays(1, &fp_vao);
    }
    gl.GenBuffers(1, &fp_static_vbo);
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
        for (FpPage& page : fp_pages) {
            if (page.tex != 0) {
                gl.DeleteTextures(1, &(page.tex));
            }
        }
    }
    fp_program = 0;
    fp_static_vbo = 0;
    fp_dynamic_vbo = 0;
    fp_vao = 0;
    fp_fbo = 0;
    fp_depth_rb = 0;
    fp_palette_tex = 0;
    fp_fbo_target = 0;
    fp_fbo_w = 0;
    fp_fbo_h = 0;
    fp_static_uploaded = false;
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
        } else if (lightSig != fp_light_sig) {
            fp_relight_static(elevation);
            fp_light_sig = lightSig;
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

        // ---- draw
        float ambient = (float)light_get_ambient() / 65536.0f;
        // Through his eyes a hazy wasteland sky; from Diablo's camera the game's black past the map.
        float fog[3] = { 0.0f, 0.0f, 0.0f };
        float fogStart = 1e9f;
        float fogEnd = 2e9f;
        if (mode == FP_VIEW_EYES) {
            fog[0] = FP_SKY[swapRB ? 2 : 0] * ambient;
            fog[1] = FP_SKY[1] * ambient;
            fog[2] = FP_SKY[swapRB ? 0 : 2] * ambient;
            fogStart = (float)FP_FOG_START;
            fogEnd = (float)FP_FOG_END;
        }
        float mvp[16];
        fp_camera_matrix(mode, viewW, viewH, mvp);

        gl.BindFramebuffer(GL_FRAMEBUFFER, fp_fbo);
        gl.Viewport(0, 0, viewW, viewH);
        gl.Scissor(0, 0, viewW, viewH);
        gl.Enable(GL_SCISSOR_TEST);
        gl.ClearColor(fog[0], fog[1], fog[2], 1.0f);
        gl.DepthMask(GL_TRUE);
        gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl.Disable(GL_BLEND);
        gl.Disable(GL_CULL_FACE);
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_LEQUAL);

        gl.UseProgram(fp_program);
        gl.UniformMatrix4fv(fp_loc.mvp, 1, GL_FALSE, mvp);
        gl.Uniform1i(fp_loc.atlas, 0);
        gl.Uniform1i(fp_loc.palette, 1);
        gl.Uniform2f(fp_loc.atlasSize, (float)FP_PAGE_SIZE, (float)FP_PAGE_SIZE);
        gl.Uniform1f(fp_loc.ambient, ambient);
        gl.Uniform3f(fp_loc.fog, fog[0], fog[1], fog[2]);
        gl.Uniform2f(fp_loc.fogRange, fogStart, fogEnd);
        gl.Uniform1f(fp_loc.flip, 1.0f);
        gl.Uniform1f(fp_loc.opacity, 1.0f);

        // the static scene
        gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
        fp_bind_attributes();
        fp_draw_ranges(fp_static_ranges, FP_KIND_OPAQUE);
        gl.Enable(GL_POLYGON_OFFSET_FILL);
        gl.PolygonOffset(-1.0f, -4.0f);
        fp_draw_ranges(fp_static_ranges, FP_KIND_DECAL);

        // this frame's critters and boards
        gl.BindBuffer(GL_ARRAY_BUFFER, fp_dynamic_vbo);
        gl.BufferData(GL_ARRAY_BUFFER, dynVerts.size() * sizeof(float), dynVerts.empty() ? NULL : dynVerts.data(), GL_STREAM_DRAW);
        fp_bind_attributes();
        fp_draw_ranges(dynRanges, FP_KIND_DECAL);
        gl.Disable(GL_POLYGON_OFFSET_FILL);
        fp_draw_ranges(dynRanges, FP_KIND_OPAQUE);

        // translucent last, over everything, writing no depth
        gl.BindBuffer(GL_ARRAY_BUFFER, fp_static_vbo);
        fp_bind_attributes();
        gl.Enable(GL_BLEND);
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.DepthMask(GL_FALSE);
        gl.Uniform1f(fp_loc.opacity, 0.5f);
        fp_draw_ranges(fp_static_ranges, FP_KIND_TRANS);

        if (fp_capture_path[0] != '\0') {
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
            fp_write_capture(fp_capture_path, viewW, viewH, rgb.data());
            trace_event(TRACE_FLOW, "fp-view", "captured %dx%d to %s", viewW, viewH, fp_capture_path);
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
    fp_mode = fp_mode == mode ? FP_VIEW_OFF : mode;
    fp_yaw_set = false;
    fp_capture_path[0] = '\0';
    trace_event(TRACE_FLOW, "fp-view", "mode=%d%s", fp_mode, fp_failed ? " (failed earlier: stays 2D)" : "");
}

int fp_view_mode()
{
    return fp_failed ? FP_VIEW_OFF : fp_mode;
}

} // namespace fallout
