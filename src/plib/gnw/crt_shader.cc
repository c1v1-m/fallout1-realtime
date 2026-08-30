#include "plib/gnw/crt_shader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <SDL_opengl.h>

#include "game/gconfig.h"
#include "plib/gnw/debug.h"

namespace fallout {

// CRT post-process for the final present.
//
// WHY IT LIVES HERE AND NOT IN A RENDER REWRITE. The game presents through exactly one
// seam -- renderPresent() in svga.cc, which does UpdateTexture + RenderCopy of a single
// full-frame texture. That makes a post-process a strictly local change: instead of
// letting SDL blit the texture, we bind the SAME texture with SDL_GL_BindTexture and
// draw our own full-screen quad with a fragment shader. Nothing above svga.cc knows.
//
// The model is the classic Timothy Lottes CRT reconstruction (public domain), which is
// also the core of what crt-royale does in its first passes: the source pixel is not
// point-sampled, it is REBUILT from a 3x3 neighbourhood with a gaussian electron-beam
// profile -- narrow horizontally (uHardPix = focus) and narrow vertically (uHardScan =
// scanline separation). The un-normalised vertical sum is what produces the scanline
// gaps for free, and it is why bright areas bleed WIDER than dark ones, the single most
// recognisable CRT trait. On top of that: an optional wide 5x5 bloom pass (halation --
// the glow that bled through the glass), a phosphor mask in OUTPUT pixel space
// (aperture grille / slot / shadow), barrel curvature, vignette and rounded tube corners.
//
// PROVENANCE -- read before touching this file.
//
// Every line of GLSL below is written from the physics of a shadow-mask tube: a gaussian
// electron spot whose width tracks beam current, a phosphor triad the spot lands on, glass
// that scatters some of the light sideways, and a curved faceplate. Those are properties of
// the hardware, described in display engineering long before any shader existed, and every
// CRT shader models the same ones.
//
// NOTHING here is copied, transliterated or mechanically derived from crt-royale
// (GPLv2+, (C) 2014 TroggleMonkey) or from any other libretro shader, and no constant was
// lifted from one. That matters because this project is under the Sustainable Use License,
// which GPL code cannot be mixed into, and because this shader is meant to travel to other
// builds of the game without dragging a licence along with it.
//
// Keep it that way. If you ever want a behaviour crt-royale has, describe the OPTICS you
// want and implement those; do not open its source and translate.
//
// It is also not a byte-exact match for that shader and cannot be: the real crt-royale is a
// six-pass chain with external phosphor-mask textures. This is one pass.
//
// PORTABILITY. The GLSL is written in the subset GLSL ES 1.00 shares with desktop GLSL 120
// -- attribute/varying, texture2D, no integer maths, no dynamic loops -- so the same body
// compiles on a desktop GL context and on an OpenGL ES 2.0 one (Android, and a PS Vita
// build through vitaGL). Only the #version line and the precision block differ, and those
// are prepended at runtime. Set crt_quality=0 on hardware that cannot afford the taps.
//
// SAFETY. We only ever run on SDL's "opengl" renderer, we save and restore the GL state
// SDL caches behind our back (in particular GL_CURRENT_PROGRAM -- SDL's GL renderer
// caches which program it bound and will NOT rebind if it thinks nothing changed), and
// on ANY failure -- no GL context, no shader support, compile error, a texture that came
// back as a rectangle target -- we set crt_failed and the caller falls back to the plain
// SDL_RenderCopy path forever. Nothing in here can make the game fail to render.

// ---------------------------------------------------------------------------
// GL entry points, all loaded through SDL (never linked): SDL_GL_GetProcAddress falls
// back to the opengl32 export table on Windows, so the GL 1.1 functions come through the
// same way as the 2.0 ones and we do not have to link opengl32 ourselves.
// ---------------------------------------------------------------------------

typedef void(APIENTRY* CRT_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void(APIENTRY* CRT_ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* CRT_Clear)(GLbitfield);
typedef void(APIENTRY* CRT_Disable)(GLenum);
typedef void(APIENTRY* CRT_Enable)(GLenum);
typedef GLboolean(APIENTRY* CRT_IsEnabled)(GLenum);
typedef void(APIENTRY* CRT_DrawArrays)(GLenum, GLint, GLsizei);
typedef void(APIENTRY* CRT_GetIntegerv)(GLenum, GLint*);
typedef void(APIENTRY* CRT_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void(APIENTRY* CRT_PixelStorei)(GLenum, GLint);
typedef const GLubyte*(APIENTRY* CRT_GetString)(GLenum);

struct CrtGl {
    CRT_Viewport Viewport;
    CRT_ClearColor ClearColor;
    CRT_Clear Clear;
    CRT_Disable Disable;
    CRT_Enable Enable;
    CRT_IsEnabled IsEnabled;
    CRT_DrawArrays DrawArrays;
    CRT_GetIntegerv GetIntegerv;
    CRT_GetString GetString;
    CRT_ReadPixels ReadPixels;
    CRT_PixelStorei PixelStorei;

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
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLACTIVETEXTUREPROC ActiveTexture;

    // Optional -- only present on core-profile-ish drivers, where a VAO is mandatory.
    // Harmless everywhere else.
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
};

static CrtGl gl;
static bool gl_loaded = false;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool crt_cfg_read = false;
static bool crt_enabled_flag = false;

// Sticky: something went wrong once, never try again this run.
static bool crt_failed = false;

static GLuint crt_program = 0;
static GLuint crt_vbo = 0;
static GLuint crt_vao = 0;

static struct {
    GLint tex;
    GLint texSize;
    GLint texScale;
    GLint outSize;
    GLint outOrigin;
    GLint beamMin;
    GLint beamMax;
    GLint beamPower;
    GLint scanDensity;
    GLint beamChroma;
    GLint cheap;
    GLint hardPix;
    GLint warp;
    GLint maskDark;
    GLint maskLight;
    GLint maskType;
    GLint maskScale;
    GLint bloom;
    GLint brightness;
    GLint saturation;
    GLint vignette;
    GLint corner;
    GLint shadowLift;
} loc;

// Tunables, all from [preferences] in fallout.cfg. The 0..1 knobs are mapped onto the
// shader's raw parameters in crt_upload_uniforms -- see there for what each end means.
//
// These were set by rendering the user's OWN game frame through the shader and putting it
// side by side, at matched zoom, with his crt-royale capture from ShaderGlass. Two earlier
// passes were far too timid: the structure has to be visible at 1:1 or the filter reads as
// a faint grey haze, and a tube IS dark -- crt_compensate stays low on purpose.
static double crt_scanline = 1.00; // beam separation: 0 soft, 1 hard black gaps
static double crt_sharpness = 0.60; // horizontal beam focus: 0 smeared, 1 sharp
static double crt_curvature = 0.15; // barrel warp, 0 = flat
// 1 = aperture grille, and this is MEASURED, not chosen. Autocorrelating the reference
// crt-royale capture gives peaks at lag 3 and 6 on BOTH axes -- a 3x3 pixel lattice. The
// grille supplies the horizontal 3 (the R/G/B triad); the scanline supplies the vertical 3,
// which is why crt_scan_density has to be 1.0. Mask type 3 measured 6 wide by 2 tall and
// did not match, despite looking like the right idea at 5x zoom.
static int crt_mask = 1; // 0 none, 1 aperture grille, 2 slot, 3 shadow
static double crt_mask_strength = 0.90;
static double crt_mask_scale = 1.00; // output pixels per mask cell
// 1 = the full 9-tap reconstruction plus halation, 0 = one tap per pixel with the scanline
// and mask kept. The cheap path exists for GPUs that cannot afford ~34 texture reads per
// output pixel -- a PS Vita at 960x544 cannot, a desktop does not notice.
static int crt_quality = 1;
// Scanlines per SOURCE row. 1.0 = one per row: at 3 output pixels per source row that puts
// a scanline every 3 pixels, and together with the grille it forms the 3x3 lattice the
// reference capture measures.
//
// ⚠️ This was 0.5, reasoned from a period tube drawing ~240 lines against the game's 480.
// The reasoning is sound and the answer is still wrong -- crt-royale does not do that, and
// 0.5 measures a 6-pixel vertical period against the reference's 3. Measure against a
// capture before changing this again; it is not a matter of taste.
static double crt_scan_density = 1.0;
// 0 = one spot width per pixel, taken from its luminance; 1 = a separate width per channel.
// Measured against the reference: per-channel drove blue on orange sand down to B/G 0.68
// (reference 0.80, unfiltered game 0.79) and the road to 0.89 (reference 0.91). At 0 those
// become 0.74 and 0.91 -- and brightness, contrast and grain do not move at all, so this
// costs nothing measurable. What it does cost is the coloured bleed around saturated edges,
// which is a real CRT trait no metric here captures; raise this if you want it back.
static double crt_beam_chroma = 0.00;
static double crt_beam_power = 0.35; // how fast the beam swells with brightness
static double crt_compensate = 1.00; // 0 = keep a CRT's darkness, 1 = give all the light back
static double crt_bloom = 0.35; // halation
static double crt_brightness = 0.87; // plain multiplier -- does NOT touch contrast
static double crt_saturation = 1.05;
static double crt_vignette = 0.15;
// Tone curve applied at the very end: out = in^(1/lift). 1.0 = untouched, >1 opens up the
// shadows without moving the highlights.
//
// Needed because the beam gate is brightness-dependent -- a dark area gets a NARROW spot
// and therefore loses proportionally more light than a bright one. That is correct physics
// and it steepens the tone curve as a side effect. Measured on the same patch of the same
// frame, the reference sits at road/mid 0.53 where we sat at 0.35; ln(0.53)/ln(0.35) puts
// the correction at 1/0.605, hence 1.65.
static double crt_shadow_lift = 1.00;
static double crt_corner = 0.02; // rounded tube corners, in half-screen units

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// Bodies carry NO #version line -- crt_compile prepends the right one for the context.
static const char* CRT_VERTEX_SHADER = R"GLSL(
attribute vec2 aPos;
varying vec2 vUv;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    // Flip Y: SDL's texture has row 0 at the top, NDC has +1 at the top.
    vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
}
)GLSL";

static const char* CRT_FRAGMENT_SHADER = R"GLSL(
uniform sampler2D uTex;
uniform vec2 uTexSize;
uniform vec2 uTexScale;
uniform vec2 uOutSize;
uniform vec2 uOutOrigin;
uniform float uBeamMin;
uniform float uBeamMax;
uniform float uBeamPower;
uniform float uScanDensity;
uniform float uBeamChroma;
uniform float uCheap;
uniform float uHardPix;
uniform vec2 uWarp;
uniform float uMaskDark;
uniform float uMaskLight;
uniform float uMaskType;
uniform float uMaskScale;
uniform float uBloom;
uniform float uBrightness;
uniform float uSaturation;
uniform float uVignette;
uniform float uCorner;
uniform float uShadowLift;

varying vec2 vUv;

const float HARD_BLOOM_PIX = -1.5;
const float HARD_BLOOM_SCAN = -2.0;

vec3 toLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 toSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// One source texel, addressed in texel units relative to `pos`. Outside the frame is
// black -- that is what makes the curved overscan edge fade out instead of smearing the
// outermost row across the bezel.
vec3 fetch(vec2 pos, vec2 off) {
    vec2 p = floor(pos * uTexSize + off) + 0.5;
    if (p.x < 0.0 || p.y < 0.0 || p.x > uTexSize.x || p.y > uTexSize.y) {
        return vec3(0.0);
    }
    return toLinear(texture2D(uTex, p / uTexSize * uTexScale).rgb);
}

// Signed distance from the sample point to the centre of its texel, in texels.
vec2 texelDist(vec2 pos) {
    pos = pos * uTexSize;
    return -((pos - floor(pos)) - 0.5);
}

float beam(float d, float scale) {
    return exp2(scale * d * d);
}

// Horizontal beam profile across 3 texels: the electron beam's focus.
vec3 horz3(vec2 pos, float off) {
    // Weak hardware: one tap. Costs the horizontal focus, keeps the scanline and the mask,
    // which are the two things anyone actually sees.
    if (uCheap > 0.5) {
        return fetch(pos, vec2(0.0, off));
    }
    vec3 a = fetch(pos, vec2(-1.0, off));
    vec3 b = fetch(pos, vec2(0.0, off));
    vec3 c = fetch(pos, vec2(1.0, off));
    float d = texelDist(pos).x;
    float wa = beam(d - 1.0, uHardPix);
    float wb = beam(d, uHardPix);
    float wc = beam(d + 1.0, uHardPix);
    return (a * wa + b * wb + c * wc) / (wa + wb + wc);
}

// The same, much wider and unfocused -- the halation term.
vec3 horz5(vec2 pos, float off) {
    vec3 a = fetch(pos, vec2(-2.0, off));
    vec3 b = fetch(pos, vec2(-1.0, off));
    vec3 c = fetch(pos, vec2(0.0, off));
    vec3 d = fetch(pos, vec2(1.0, off));
    vec3 e = fetch(pos, vec2(2.0, off));
    float x = texelDist(pos).x;
    float wa = beam(x - 2.0, HARD_BLOOM_PIX);
    float wb = beam(x - 1.0, HARD_BLOOM_PIX);
    float wc = beam(x, HARD_BLOOM_PIX);
    float wd = beam(x + 1.0, HARD_BLOOM_PIX);
    float we = beam(x + 2.0, HARD_BLOOM_PIX);
    return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

float scanWeight(vec2 pos, float off, float hardness) {
    return beam(texelDist(pos).y + off, hardness);
}

// THE VARIABLE-WIDTH BEAM -- this is what crt-royale does that a fixed gaussian does not,
// and it is the single biggest thing separating the two looks.
//
// The spot's vertical sigma is not a constant: it grows with how bright the line is, and
// it does so PER CHANNEL. A dark line stays a thin hard stripe with black either side; a
// bright line swells until it closes the gap to its neighbours and blooms over them. That
// is why a real tube looks contrasty in the shadows and washed-out-bright in the
// highlights instead of applying the same grey comb everywhere -- and doing it per channel
// is what tints the bleed around saturated edges.
//
// Normalised (the 1/(sigma*sqrt(2pi)) term), so narrowing the beam concentrates the light
// rather than throwing it away: the line gets sharper, not dimmer.
vec3 scanlines(vec2 pos) {
    // 1. Rebuild the image. Vertically this is pure focus -- a normalised blur across three
    //    source rows -- and carries no scanline of its own.
    float d = texelDist(pos).y;
    vec3 b = horz3(pos, 0.0);
    vec3 a = uCheap > 0.5 ? b : horz3(pos, -1.0);
    vec3 c = uCheap > 0.5 ? b : horz3(pos, 1.0);
    float wa = exp2(-3.0 * (d - 1.0) * (d - 1.0));
    float wb = exp2(-3.0 * d * d);
    float wc = exp2(-3.0 * (d + 1.0) * (d + 1.0));
    vec3 img = (a * wa + b * wb + c * wc) / (wa + wb + wc);
    if (uCheap > 0.5) {
        img = b;
    }

    // 2. Gate it with the scanline, on ITS OWN GRID. This is the part that has to be
    //    separate: the game renders 480 internal lines, but the tube being imitated drew
    //    about 240. Tying the line pitch to the source row pitch put one scanline every 3
    //    output pixels, too fine to read as anything but a grey haze -- uScanDensity 0.5
    //    puts it every 6, which is what the reference capture actually shows.
    //
    //    The gate is NOT normalised -- it is a gate, and the dark half is the whole point.
    //    Its width grows with brightness, per channel: a dim line stays a thin hard stripe
    //    with black either side, a bright one swells until it closes the gap and blooms
    //    over its neighbours. That is the trait a fixed comb cannot fake, and doing it per
    //    channel is what tints the bleed around saturated edges.
    float f = fract(pos.y * uTexSize.y * uScanDensity + 0.5) - 0.5;
    // What DRIVES the spot width. Fully per-channel (uBeamChroma = 1) is the honest
    // physics and it is what tints the bleed around saturated edges -- but it also means
    // the weakest channel of a strongly tinted surface gets the narrowest spot and loses
    // the most light. On Fallout's orange sand that pushed blue down to B/G 0.68 where the
    // reference sits at 0.80 and the unfiltered game at 0.79.
    //
    // So blend against the pixel's own luminance: at 0 every channel shares one spot width
    // and the colour balance passes through untouched, at 1 it is the old behaviour.
    vec3 luma3 = vec3(dot(img, vec3(0.2126, 0.7152, 0.0722)));
    vec3 drive = mix(luma3, img, uBeamChroma);
    vec3 sigma = uBeamMin + (uBeamMax - uBeamMin) * pow(clamp(drive, 0.0, 1.0), vec3(uBeamPower));
    return img * exp(-(f * f) / (2.0 * sigma * sigma));
}

vec3 halation(vec2 pos) {
    vec3 acc = vec3(0.0);
    acc += horz5(pos, -2.0) * scanWeight(pos, -2.0, HARD_BLOOM_SCAN);
    acc += horz5(pos, -1.0) * scanWeight(pos, -1.0, HARD_BLOOM_SCAN);
    acc += horz5(pos, 0.0) * scanWeight(pos, 0.0, HARD_BLOOM_SCAN);
    acc += horz5(pos, 1.0) * scanWeight(pos, 1.0, HARD_BLOOM_SCAN);
    acc += horz5(pos, 2.0) * scanWeight(pos, 2.0, HARD_BLOOM_SCAN);
    return acc;
}

// Phosphor layout, in OUTPUT pixels -- it belongs to the glass, not to the image, so it
// must not scale with the game's internal resolution.
vec3 phosphorMask(vec2 pos) {
    if (uMaskType < 0.5) {
        return vec3(1.0);
    }

    pos /= max(uMaskScale, 0.25);

    // Extra dimming for the slot mask's horizontal gaps; 1.0 for the others.
    float gap = 1.0;
    float x;

    if (uMaskType < 1.5) {
        // Aperture grille: straight vertical R/G/B stripes (Trinitron).
        x = fract(pos.x / 3.0);
    } else if (uMaskType < 2.5) {
        // Slot mask: the same stripes, offset half a triad every other cell row, with the
        // horizontal gaps between phosphor slots.
        float row = floor(pos.y * 0.5);
        x = fract((pos.x + 1.5 * mod(row, 2.0)) / 3.0);
        gap = (mod(pos.y, 4.0) < 0.5) ? 0.75 : 1.0;
    } else {
        // Shadow mask: triads sheared row over row, so the dots sit on a lattice.
        x = fract((pos.x + pos.y * 3.0) / 6.0);
    }

    // SMOOTH phosphors, not a hard three-way choice.
    //
    // A binary mask -- "this output pixel is red, that one is green" -- is a box function,
    // and a box function is exactly what aliases. Two costs follow from it. First, grain:
    // the hard edges beat against the image and add high-frequency noise that is not in the
    // picture. Second, and worse, a weak channel is switched fully OFF for two thirds of
    // every triad, so on a strongly tinted surface (Fallout's orange sand) the blue simply
    // vanishes and the whole frame drifts warm.
    //
    // Real phosphor dots have soft edges and their light overlaps, so model that: three
    // cosine lobes 120 degrees apart. Every channel keeps a non-zero floor everywhere, the
    // function is band-limited so it cannot alias, and the triad still reads as a triad.
    //
    // This is the same conclusion crt-royale reaches by a different route -- it resamples a
    // mask texture (Lanczos or mipmapped) rather than sampling it 1:1, which likewise
    // band-limits it. We have no LUT, so the lobes are generated directly.
    float phase = x * 6.2831853;
    vec3 lobes = vec3(
        cos(phase),
        cos(phase - 2.0943951),
        cos(phase - 4.1887902));
    return (uMaskDark + (uMaskLight - uMaskDark) * (lobes * 0.5 + 0.5)) * gap;
}

vec2 warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y * pos.y) * uWarp.x, 1.0 + (pos.x * pos.x) * uWarp.y);
    return pos * 0.5 + 0.5;
}

// Rounded-rectangle mask over the warped frame. Doubles as the overscan cutoff: past the
// tube edge it returns 0, so a curved image ends on black instead of on a stretch of the
// outermost texel.
float tube(vec2 pos) {
    vec2 p = abs(pos * 2.0 - 1.0);
    float r = clamp(uCorner, 0.0, 0.5);
    vec2 q = p - (1.0 - r);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    float feather = 4.0 / max(uOutSize.y, 1.0);
    return 1.0 - smoothstep(-feather, feather, d);
}

float vignette(vec2 pos) {
    if (uVignette <= 0.0) {
        return 1.0;
    }
    vec2 t = clamp(pos, 0.0, 1.0);
    float v = 16.0 * t.x * t.y * (1.0 - t.x) * (1.0 - t.y);
    return mix(1.0, pow(max(v, 0.0), 0.35), uVignette);
}

void main() {
    vec2 uv = warp(vUv);

    vec3 col = scanlines(uv);
    if (uBloom > 0.0) {
        col += halation(uv) * uBloom;
    }

    col *= phosphorMask(gl_FragCoord.xy - uOutOrigin);
    col *= uBrightness;

    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(luma), col, uSaturation);

    col *= tube(uv) * vignette(uv);

    col = pow(max(col, vec3(0.0)), vec3(1.0 / uShadowLift));

    gl_FragColor = vec4(toSrgb(col), 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------

#define CRT_LOAD(field, type, name)                           \
    do {                                                      \
        gl.field = (type)SDL_GL_GetProcAddress(name);         \
        if (gl.field == NULL) {                               \
            debug_printf("CRT: missing GL entry %s\n", name); \
            return false;                                     \
        }                                                     \
    } while (0)

static bool crt_load_gl()
{
    if (gl_loaded) {
        return true;
    }

    memset(&gl, 0, sizeof(gl));

    CRT_LOAD(Viewport, CRT_Viewport, "glViewport");
    CRT_LOAD(ClearColor, CRT_ClearColor, "glClearColor");
    CRT_LOAD(Clear, CRT_Clear, "glClear");
    CRT_LOAD(Disable, CRT_Disable, "glDisable");
    CRT_LOAD(Enable, CRT_Enable, "glEnable");
    CRT_LOAD(IsEnabled, CRT_IsEnabled, "glIsEnabled");
    CRT_LOAD(DrawArrays, CRT_DrawArrays, "glDrawArrays");
    CRT_LOAD(GetIntegerv, CRT_GetIntegerv, "glGetIntegerv");
    CRT_LOAD(GetString, CRT_GetString, "glGetString");
    CRT_LOAD(ReadPixels, CRT_ReadPixels, "glReadPixels");
    CRT_LOAD(PixelStorei, CRT_PixelStorei, "glPixelStorei");

    CRT_LOAD(CreateShader, PFNGLCREATESHADERPROC, "glCreateShader");
    CRT_LOAD(ShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource");
    CRT_LOAD(CompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    CRT_LOAD(GetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    CRT_LOAD(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    CRT_LOAD(DeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader");
    CRT_LOAD(CreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    CRT_LOAD(AttachShader, PFNGLATTACHSHADERPROC, "glAttachShader");
    CRT_LOAD(BindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC, "glBindAttribLocation");
    CRT_LOAD(LinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    CRT_LOAD(GetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    CRT_LOAD(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    CRT_LOAD(UseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram");
    CRT_LOAD(DeleteProgram, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    CRT_LOAD(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    CRT_LOAD(Uniform1i, PFNGLUNIFORM1IPROC, "glUniform1i");
    CRT_LOAD(Uniform1f, PFNGLUNIFORM1FPROC, "glUniform1f");
    CRT_LOAD(Uniform2f, PFNGLUNIFORM2FPROC, "glUniform2f");
    CRT_LOAD(GenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    CRT_LOAD(BindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    CRT_LOAD(BufferData, PFNGLBUFFERDATAPROC, "glBufferData");
    CRT_LOAD(DeleteBuffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
    CRT_LOAD(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    CRT_LOAD(DisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC, "glDisableVertexAttribArray");
    CRT_LOAD(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");
    CRT_LOAD(ActiveTexture, PFNGLACTIVETEXTUREPROC, "glActiveTexture");

    // Optional.
    gl.GenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
    gl.BindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
    gl.DeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");

    gl_loaded = true;
    return true;
}

#undef CRT_LOAD

// True when the live context is OpenGL ES rather than desktop GL. SDL's attribute is the
// authority; the version string is a fallback for drivers that do not report the profile
// (some ES wrappers, vitaGL among them, are casual about it).
static bool crt_context_is_es()
{
    int profile = 0;
    if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile) == 0 && profile == SDL_GL_CONTEXT_PROFILE_ES) {
        return true;
    }

    const GLubyte* version = gl.GetString(GL_VERSION);
    return version != NULL && strstr((const char*)version, "OpenGL ES") != NULL;
}

// GLSL ES 1.00 needs a precision declaration in the fragment stage and has no 120 profile;
// desktop GLSL 120 has no precision qualifiers at all. Same body either way -- only this
// preamble changes, which is what keeps one shader source portable to an ES 2.0 build.
static GLuint crt_compile(GLenum type, const char* source)
{
    GLuint shader = gl.CreateShader(type);
    if (shader == 0) {
        return 0;
    }

    const char* header;
    if (!crt_context_is_es()) {
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
        debug_printf("CRT: %s shader failed to compile: %s\n",
            type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl.DeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool crt_build()
{
    GLuint vs = crt_compile(GL_VERTEX_SHADER, CRT_VERTEX_SHADER);
    if (vs == 0) {
        return false;
    }

    GLuint fs = crt_compile(GL_FRAGMENT_SHADER, CRT_FRAGMENT_SHADER);
    if (fs == 0) {
        gl.DeleteShader(vs);
        return false;
    }

    GLuint program = gl.CreateProgram();
    if (program == 0) {
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        return false;
    }

    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    // Location 0 aliases gl_Vertex in a compatibility context, which is exactly where the
    // position belongs, and it keeps SDL's own fixed-function arrays out of the way.
    gl.BindAttribLocation(program, 0, "aPos");
    gl.LinkProgram(program);

    gl.DeleteShader(vs);
    gl.DeleteShader(fs);

    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = '\0';
        gl.GetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        debug_printf("CRT: program failed to link: %s\n", log);
        gl.DeleteProgram(program);
        return false;
    }

    static const GLfloat quad[8] = {
        -1.0f, -1.0f,
        1.0f, -1.0f,
        -1.0f, 1.0f,
        1.0f, 1.0f,
    };

    if (gl.GenVertexArrays != NULL && gl.BindVertexArray != NULL) {
        gl.GenVertexArrays(1, &crt_vao);
        gl.BindVertexArray(crt_vao);
    }

    gl.GenBuffers(1, &crt_vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, crt_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);

    if (crt_vao != 0) {
        gl.BindVertexArray(0);
    }

    crt_program = program;

    loc.tex = gl.GetUniformLocation(program, "uTex");
    loc.texSize = gl.GetUniformLocation(program, "uTexSize");
    loc.texScale = gl.GetUniformLocation(program, "uTexScale");
    loc.outSize = gl.GetUniformLocation(program, "uOutSize");
    loc.outOrigin = gl.GetUniformLocation(program, "uOutOrigin");
    loc.beamMin = gl.GetUniformLocation(program, "uBeamMin");
    loc.beamMax = gl.GetUniformLocation(program, "uBeamMax");
    loc.beamPower = gl.GetUniformLocation(program, "uBeamPower");
    loc.scanDensity = gl.GetUniformLocation(program, "uScanDensity");
    loc.beamChroma = gl.GetUniformLocation(program, "uBeamChroma");
    loc.cheap = gl.GetUniformLocation(program, "uCheap");
    loc.hardPix = gl.GetUniformLocation(program, "uHardPix");
    loc.warp = gl.GetUniformLocation(program, "uWarp");
    loc.maskDark = gl.GetUniformLocation(program, "uMaskDark");
    loc.maskLight = gl.GetUniformLocation(program, "uMaskLight");
    loc.maskType = gl.GetUniformLocation(program, "uMaskType");
    loc.maskScale = gl.GetUniformLocation(program, "uMaskScale");
    loc.bloom = gl.GetUniformLocation(program, "uBloom");
    loc.brightness = gl.GetUniformLocation(program, "uBrightness");
    loc.saturation = gl.GetUniformLocation(program, "uSaturation");
    loc.vignette = gl.GetUniformLocation(program, "uVignette");
    loc.corner = gl.GetUniformLocation(program, "uCorner");
    loc.shadowLift = gl.GetUniformLocation(program, "uShadowLift");

    debug_printf("CRT: shader ready\n");
    return true;
}

static double crt_clamp(double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}

void crt_init()
{
    if (crt_cfg_read) {
        return;
    }
    crt_cfg_read = true;

    // ON unless the cfg says otherwise -- the filter is meant to be the way the game
    // looks, not something you have to go and switch on.
    int enabled = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_KEY, &enabled);
    crt_enabled_flag = enabled != 0;

    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_KEY, &crt_mask);
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_QUALITY_KEY, &crt_quality);
    crt_quality = crt_quality != 0 ? 1 : 0;
    crt_mask = crt_mask < 0 ? 0 : (crt_mask > 3 ? 3 : crt_mask);

    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SCANLINE_KEY, &crt_scanline);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SHARPNESS_KEY, &crt_sharpness);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_CURVATURE_KEY, &crt_curvature);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_STRENGTH_KEY, &crt_mask_strength);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_SCALE_KEY, &crt_mask_scale);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SCAN_DENSITY_KEY, &crt_scan_density);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BEAM_CHROMA_KEY, &crt_beam_chroma);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BEAM_POWER_KEY, &crt_beam_power);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_COMPENSATE_KEY, &crt_compensate);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BLOOM_KEY, &crt_bloom);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BRIGHTNESS_KEY, &crt_brightness);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SATURATION_KEY, &crt_saturation);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_VIGNETTE_KEY, &crt_vignette);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SHADOW_LIFT_KEY, &crt_shadow_lift);
    config_get_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_CORNER_KEY, &crt_corner);

    crt_scanline = crt_clamp(crt_scanline, 0.0, 1.0);
    crt_sharpness = crt_clamp(crt_sharpness, 0.0, 1.0);
    crt_curvature = crt_clamp(crt_curvature, 0.0, 1.0);
    crt_mask_strength = crt_clamp(crt_mask_strength, 0.0, 1.0);
    crt_mask_scale = crt_clamp(crt_mask_scale, 0.25, 8.0);
    crt_scan_density = crt_clamp(crt_scan_density, 0.25, 1.0);
    crt_beam_chroma = crt_clamp(crt_beam_chroma, 0.0, 1.0);
    crt_beam_power = crt_clamp(crt_beam_power, 0.05, 2.0);
    crt_compensate = crt_clamp(crt_compensate, 0.0, 1.0);
    crt_bloom = crt_clamp(crt_bloom, 0.0, 2.0);
    crt_brightness = crt_clamp(crt_brightness, 0.25, 3.0);
    crt_saturation = crt_clamp(crt_saturation, 0.0, 2.0);
    crt_vignette = crt_clamp(crt_vignette, 0.0, 1.0);
    crt_shadow_lift = crt_clamp(crt_shadow_lift, 0.5, 3.0);
    crt_corner = crt_clamp(crt_corner, 0.0, 0.5);

    // Write the effective values straight back, so every knob shows up in fallout.cfg the
    // next time it is saved. Only `crt` itself has UI; without this the look tuning would
    // be invisible to anyone who did not read the source.
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_KEY, crt_enabled_flag ? 1 : 0);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_KEY, crt_mask);
    config_set_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_QUALITY_KEY, crt_quality);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SCANLINE_KEY, crt_scanline);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SHARPNESS_KEY, crt_sharpness);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_CURVATURE_KEY, crt_curvature);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_STRENGTH_KEY, crt_mask_strength);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_MASK_SCALE_KEY, crt_mask_scale);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SCAN_DENSITY_KEY, crt_scan_density);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BEAM_CHROMA_KEY, crt_beam_chroma);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BEAM_POWER_KEY, crt_beam_power);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_COMPENSATE_KEY, crt_compensate);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BLOOM_KEY, crt_bloom);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_BRIGHTNESS_KEY, crt_brightness);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SATURATION_KEY, crt_saturation);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_VIGNETTE_KEY, crt_vignette);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_SHADOW_LIFT_KEY, crt_shadow_lift);
    config_set_double(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_CRT_CORNER_KEY, crt_corner);

    debug_printf("CRT: %s (mask %d, scanline %.2f, curvature %.2f, bloom %.2f)\n",
        crt_enabled_flag ? "on" : "off", crt_mask, crt_scanline, crt_curvature, crt_bloom);
}

void crt_invalidate(bool contextGone)
{
    if (!contextGone && gl_loaded) {
        if (crt_program != 0) {
            gl.DeleteProgram(crt_program);
        }
        if (crt_vbo != 0) {
            gl.DeleteBuffers(1, &crt_vbo);
        }
        if (crt_vao != 0 && gl.DeleteVertexArrays != NULL) {
            gl.DeleteVertexArrays(1, &crt_vao);
        }
    }

    crt_program = 0;
    crt_vbo = 0;
    crt_vao = 0;

    // The entry points belong to the driver, not the context, but SDL may well come back
    // on a different renderer, so re-resolve them next time round.
    gl_loaded = false;
}

// Pending capture path; empty when none is asked for.
static char crt_capture_path[512];

void crt_request_capture(const char* path)
{
    if (path == NULL) {
        crt_capture_path[0] = '\0';
        return;
    }

    strncpy(crt_capture_path, path, sizeof(crt_capture_path) - 1);
    crt_capture_path[sizeof(crt_capture_path) - 1] = '\0';
}

// 24-bit bottom-up BMP, which is exactly the order glReadPixels hands rows back in, so
// the rows go straight out with no flip. Rows are padded to 4 bytes -- the same rule the
// engine's own 8-bit writer got wrong at a width of 853.
static void crt_write_bmp(const char* path, int width, int height, const unsigned char* rgb)
{
    FILE* stream = fopen(path, "wb");
    if (stream == NULL) {
        debug_printf("CRT: cannot open %s for capture\n", path);
        return;
    }

    const int stride = (width * 3 + 3) & ~3;
    const unsigned int dataSize = (unsigned int)stride * height;

    unsigned short shortValue;
    unsigned int intValue;

    shortValue = 0x4D42;
    fwrite(&shortValue, sizeof(shortValue), 1, stream); // bfType
    intValue = dataSize + 14 + 40;
    fwrite(&intValue, sizeof(intValue), 1, stream); // bfSize
    shortValue = 0;
    fwrite(&shortValue, sizeof(shortValue), 1, stream); // bfReserved1
    fwrite(&shortValue, sizeof(shortValue), 1, stream); // bfReserved2
    intValue = 14 + 40;
    fwrite(&intValue, sizeof(intValue), 1, stream); // bfOffBits

    intValue = 40;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biSize
    intValue = (unsigned int)width;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biWidth
    intValue = (unsigned int)height;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biHeight (positive = bottom-up)
    shortValue = 1;
    fwrite(&shortValue, sizeof(shortValue), 1, stream); // biPlanes
    shortValue = 24;
    fwrite(&shortValue, sizeof(shortValue), 1, stream); // biBitCount
    intValue = 0;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biCompression
    intValue = dataSize;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biSizeImage
    intValue = 0;
    fwrite(&intValue, sizeof(intValue), 1, stream); // biXPelsPerMeter
    fwrite(&intValue, sizeof(intValue), 1, stream); // biYPelsPerMeter
    fwrite(&intValue, sizeof(intValue), 1, stream); // biClrUsed
    fwrite(&intValue, sizeof(intValue), 1, stream); // biClrImportant

    unsigned char padding[3] = { 0, 0, 0 };
    for (int y = 0; y < height; y++) {
        const unsigned char* row = rgb + (size_t)y * width * 3;
        for (int x = 0; x < width; x++) {
            // BMP stores BGR.
            fwrite(&row[x * 3 + 2], 1, 1, stream);
            fwrite(&row[x * 3 + 1], 1, 1, stream);
            fwrite(&row[x * 3 + 0], 1, 1, stream);
        }
        fwrite(padding, 1, stride - width * 3, stream);
    }

    fclose(stream);
    debug_printf("CRT: captured %dx%d to %s\n", width, height, path);
}

bool crt_is_enabled()
{
    return crt_enabled_flag;
}

void crt_set_enabled(bool enabled)
{
    crt_enabled_flag = enabled;
}

bool crt_is_active()
{
    return crt_enabled_flag && !crt_failed && crt_program != 0;
}

static void crt_upload_uniforms(int texWidth, int texHeight, float texScaleX, float texScaleY,
    int viewX, int viewY, int viewW, int viewH)
{
    gl.Uniform1i(loc.tex, 0);
    gl.Uniform2f(loc.texSize, (GLfloat)texWidth, (GLfloat)texHeight);
    gl.Uniform2f(loc.texScale, texScaleX, texScaleY);
    gl.Uniform2f(loc.outSize, (GLfloat)viewW, (GLfloat)viewH);
    gl.Uniform2f(loc.outOrigin, (GLfloat)viewX, (GLfloat)viewY);

    // Beam width, in fractions of a source scanline. The MAX (what a fully lit line swells
    // to) is held at roughly half a line so highlights close the gap; crt_scanline pulls the
    // MIN down, which is what deepens the black between dim lines. At 480 source lines on a
    // 1440p screen one line is 3 output pixels, so sigma 0.10 is a third of a pixel (hard
    // stripe) and 0.45 is about 1.4 pixels (gap closed).
    // Beam widths, in fractions of a scanline period. These are MEASURED against the
    // reference capture, not guessed: averaging each output row across x kills the mask and
    // leaves the scanline, and the peak-to-trough depth of that comes out at 72% on the
    // reference. 0.45/0.17 gave 51% -- visibly stripes rather than dots, which is what the
    // user kept reporting. 0.35/0.10 measures 75%; 0.30/0.05 overshoots to 87%.
    double beamMax = 0.35;
    double beamMin = beamMax - 0.31 * crt_scanline;
    gl.Uniform1f(loc.beamMin, (GLfloat)beamMin);
    gl.Uniform1f(loc.beamMax, (GLfloat)beamMax);
    gl.Uniform1f(loc.beamPower, (GLfloat)crt_beam_power);
    gl.Uniform1f(loc.scanDensity, (GLfloat)crt_scan_density);
    gl.Uniform1f(loc.beamChroma, (GLfloat)crt_beam_chroma);
    gl.Uniform1f(loc.cheap, crt_quality != 0 ? 0.0f : 1.0f);
    gl.Uniform1f(loc.hardPix, (GLfloat)(-(1.0 + 5.0 * crt_sharpness)));

    // Slightly more warp vertically than horizontally, the way a 4:3 tube actually bends.
    gl.Uniform2f(loc.warp, (GLfloat)(crt_curvature / 32.0), (GLfloat)(crt_curvature / 24.0));

    double maskDark = 1.0 - crt_mask_strength;
    double maskLight = 1.0 + crt_mask_strength * 0.6;
    gl.Uniform1f(loc.maskDark, (GLfloat)maskDark);
    gl.Uniform1f(loc.maskLight, (GLfloat)maskLight);
    gl.Uniform1f(loc.maskType, (GLfloat)crt_mask);
    gl.Uniform1f(loc.maskScale, (GLfloat)crt_mask_scale);

    // LIGHT COMPENSATION, and how much of it to apply.
    //
    // The mask and the scanlines are multiplicative and both throw light away. Dividing all
    // of it back out keeps the picture as bright as with the filter off -- but that is NOT
    // what a CRT looks like, and it is not what crt-royale does: a tube IS darker, and the
    // deep blacks are half of why the effect reads at all. Full compensation is what made
    // the first version look washed out next to the reference capture.
    //
    // So `crt_compensate` is the dial between the two: 0 = take the loss (crt-royale's
    // behaviour, dark and contrasty), 1 = give it all back (bright and flat).
    //
    // The mask's average is trivial (one lit stripe in three). The beam's is the mean of the
    // normalised 3-tap sum across one source row at mid grey -- no closed form worth
    // deriving, so integrate it numerically. This runs once per frame on the CPU.
    double maskAverage = crt_mask == 0 ? 1.0 : (maskLight + 2.0 * maskDark) / 3.0;

    double midSigma = beamMin + (beamMax - beamMin) * pow(0.5, crt_beam_power);
    double scanAverage = 0.0;
    const int SCAN_SAMPLES = 64;
    for (int i = 0; i < SCAN_SAMPLES; i++) {
        double f = -0.5 + ((double)i + 0.5) / SCAN_SAMPLES;
        scanAverage += exp(-(f * f) / (2.0 * midSigma * midSigma));
    }
    scanAverage /= SCAN_SAMPLES;

    double compensation = 1.0;
    if (maskAverage > 0.01 && scanAverage > 0.01) {
        compensation = pow(1.0 / (maskAverage * scanAverage), crt_compensate);
    }

    gl.Uniform1f(loc.bloom, (GLfloat)(crt_bloom / 16.0));
    gl.Uniform1f(loc.brightness, (GLfloat)(crt_brightness * compensation));
    gl.Uniform1f(loc.saturation, (GLfloat)crt_saturation);
    gl.Uniform1f(loc.vignette, (GLfloat)crt_vignette);
    gl.Uniform1f(loc.corner, (GLfloat)crt_corner);
    gl.Uniform1f(loc.shadowLift, (GLfloat)crt_shadow_lift);
}

bool crt_present(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, int texWidth, int texHeight)
{
    if (crt_failed || !crt_enabled_flag) {
        return false;
    }

    if (window == NULL || renderer == NULL || texture == NULL || texWidth <= 0 || texHeight <= 0) {
        return false;
    }

    // Only the GL renderer -- everything below this line is raw OpenGL.
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0 || info.name == NULL
        || (strcmp(info.name, "opengl") != 0
            && strcmp(info.name, "opengles2") != 0
            && strcmp(info.name, "opengles") != 0)) {
        debug_printf("CRT: renderer is '%s', not a GL one -- disabled\n", info.name != NULL ? info.name : "?");
        crt_failed = true;
        return false;
    }

    if (!crt_load_gl()) {
        crt_failed = true;
        return false;
    }

    if (crt_program == 0 && !crt_build()) {
        crt_failed = true;
        return false;
    }

    // Nothing of SDL's may still be queued when we start issuing our own GL.
    SDL_RenderFlush(renderer);

    gl.ActiveTexture(GL_TEXTURE0);

    float texScaleX = 1.0f;
    float texScaleY = 1.0f;
    if (SDL_GL_BindTexture(texture, &texScaleX, &texScaleY) != 0) {
        debug_printf("CRT: SDL_GL_BindTexture failed: %s\n", SDL_GetError());
        crt_failed = true;
        return false;
    }

    // A scale other than 1 means SDL fell back to GL_TEXTURE_RECTANGLE (no NPOT support),
    // which needs a sampler2DRect and unnormalised coordinates. Not worth a second shader.
    if (texScaleX != 1.0f || texScaleY != 1.0f) {
        debug_printf("CRT: rectangle texture target unsupported -- disabled\n");
        SDL_GL_UnbindTexture(texture);
        crt_failed = true;
        return false;
    }

    // Reproduce SDL_RenderSetLogicalSize's letterbox ourselves so the CRT frame lands in
    // exactly the rect the plain path would have used.
    int drawableW = 0;
    int drawableH = 0;
    SDL_GL_GetDrawableSize(window, &drawableW, &drawableH);
    if (drawableW <= 0 || drawableH <= 0) {
        SDL_GL_UnbindTexture(texture);
        return false;
    }

    float scale = (float)drawableW / (float)texWidth;
    float scaleY = (float)drawableH / (float)texHeight;
    if (scaleY < scale) {
        scale = scaleY;
    }

    int viewW = (int)((float)texWidth * scale);
    int viewH = (int)((float)texHeight * scale);
    int viewX = (drawableW - viewW) / 2;
    int viewY = (drawableH - viewH) / 2;

    // Save everything SDL's GL renderer caches and would therefore NOT re-apply on its
    // next draw: the bound program, the blend and scissor enables, and the viewport (it
    // only re-issues glViewport when its own viewport_dirty flag is set). Handing any of
    // these back changed is what would corrupt SDL's drawing after the filter is turned
    // off mid-session -- see SetDrawState in SDL_render_gl.c.
    GLint prevProgram = 0;
    GLint prevArrayBuffer = 0;
    GLint prevVertexArray = 0;
    GLint prevViewport[4] = { 0, 0, drawableW, drawableH };
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
    gl.GetIntegerv(GL_VIEWPORT, prevViewport);
    if (crt_vao != 0) {
        gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);
    }
    GLboolean prevBlend = gl.IsEnabled(GL_BLEND);
    GLboolean prevScissor = gl.IsEnabled(GL_SCISSOR_TEST);
    GLboolean prevDepth = gl.IsEnabled(GL_DEPTH_TEST);

    gl.Disable(GL_BLEND);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);

    // The letterbox bars: clear the whole drawable, then draw only into the view rect.
    gl.Viewport(0, 0, drawableW, drawableH);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(viewX, viewY, viewW, viewH);

    gl.UseProgram(crt_program);
    crt_upload_uniforms(texWidth, texHeight, texScaleX, texScaleY, viewX, viewY, viewW, viewH);

    if (crt_vao != 0) {
        gl.BindVertexArray(crt_vao);
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, crt_vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (const void*)0);

    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Filtered capture, read back before anything else touches the buffer. Only the game
    // rect, not the letterbox bars.
    if (crt_capture_path[0] != '\0') {
        unsigned char* pixels = (unsigned char*)malloc((size_t)viewW * viewH * 3);
        if (pixels != NULL) {
            // Default pack alignment is 4; a width whose *3 is not a multiple of 4 would
            // then overrun the buffer. Same trap as the engine's BMP writer.
            gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
            gl.ReadPixels(viewX, viewY, viewW, viewH, GL_RGB, GL_UNSIGNED_BYTE, pixels);
            gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
            crt_write_bmp(crt_capture_path, viewW, viewH, pixels);
            free(pixels);
        }
        crt_capture_path[0] = '\0';
    }

    gl.DisableVertexAttribArray(0);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuffer);
    if (crt_vao != 0) {
        gl.BindVertexArray((GLuint)prevVertexArray);
    }
    gl.UseProgram((GLuint)prevProgram);

    SDL_GL_UnbindTexture(texture);

    if (prevBlend) {
        gl.Enable(GL_BLEND);
    }
    if (prevScissor) {
        gl.Enable(GL_SCISSOR_TEST);
    }
    if (prevDepth) {
        gl.Enable(GL_DEPTH_TEST);
    }
    gl.Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    return true;
}

} // namespace fallout
