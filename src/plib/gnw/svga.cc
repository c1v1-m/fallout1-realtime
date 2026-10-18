#include "plib/gnw/svga.h"

#include <math.h>

#include <SDL_opengl.h>

#include "game/gconfig.h"
#include "game/game_trace.h"
#include "plib/gnw/crt_shader.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/winmain.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect scr_size;

// 0x6ACA18
ScreenBlitFunc* scr_blit = GNW95_ShowRect;

SDL_Window* gSdlWindow = NULL;
SDL_Surface* gSdlSurface = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Texture* gSdlTexture = NULL;
SDL_Surface* gSdlTextureSurface = NULL;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// --- VSYNC (MODS "VSync", cfg vsync; 2026-10-08) ----------------------------------------------------
//
// ⚠️ WITHOUT IT NOTHING ON SCREEN CAN MOVE SMOOTHLY, however smooth the motion itself is. The renderer
// was created with no present-vsync and the loop slept to 1000/60 = 16 WHOLE ms -- 62.5 frames a
// second, handed to the display whenever they were ready. His panel runs 240 Hz: a frame stayed up
// for 4 refreshes, sometimes 3, in no fixed rhythm, so a camera gliding at a constant speed was shown
// moving in uneven jerks (and with no vsync at all, torn).
//
// With it: the swap interval is round(refresh / 60) -- 4 at 240 Hz, 2 at 120/144, 1 at 60 -- so the
// game still runs ~60 fps and everything tuned per frame stays tuned, but every frame is on screen for
// exactly the same number of refreshes. glFinish after the swap keeps the driver from queueing frames
// ahead (that would be input lag, and uneven again), and FpsLimiter steps back to a floor.
//
// ⚠️ A DRIVER CAN REFUSE (the panel's "wait for vertical refresh: off", an occluded window). Frames then
// arrive faster than the display shows them; the check below sees that and hands the pacing back to
// the sleep for good -- until the row is toggled or the renderer is rebuilt.
static bool vsync_wanted = false;
static bool vsync_dirty = true; // apply at the next present, with the renderer's GL context current
static int vsync_interval = 0; // swap interval in force; 0 = not waiting for the display
static int vsync_refresh = 0;
static double vsync_period_ms = 0.0;
static bool vsync_effective = false;
// A map is loading (svga_vsync_suspend). Its reads present a frame every 8 KB to keep the cursor
// alive, and with each present waiting for the display that turned into seconds of extra loading.
static bool vsync_suspended = false;
static bool vsync_suspend_applied = false;
static Uint64 vsync_last_present = 0;
static double vsync_acc_ms = 0.0;
static int vsync_acc_frames = 0;

typedef void(APIENTRY* SvgaGlFinishProc)(void);
static SvgaGlFinishProc vsync_gl_finish = NULL;

static void svga_vsync_apply()
{
    vsync_dirty = false;
    vsync_interval = 0;
    vsync_period_ms = 0.0;
    vsync_effective = false;
    vsync_last_present = 0;
    vsync_acc_ms = 0.0;
    vsync_acc_frames = 0;

    if (!vsync_wanted || gSdlWindow == NULL) {
        SDL_GL_SetSwapInterval(0);
        sharedFpsLimiter.setVsyncPeriod(0.0);
        return;
    }

    int refresh = 60;
    SDL_DisplayMode mode;
    int display = SDL_GetWindowDisplayIndex(gSdlWindow);
    if (SDL_GetCurrentDisplayMode(display >= 0 ? display : 0, &mode) == 0 && mode.refresh_rate > 0) {
        refresh = mode.refresh_rate;
    }

    int interval = (int)floor(refresh / 60.0 + 0.5);
    if (interval < 1) {
        interval = 1;
    }

    if (SDL_GL_SetSwapInterval(interval) != 0) {
        // Some drivers take only 0/1. One refresh per frame on a fast panel is still tear-free.
        if (interval == 1 || SDL_GL_SetSwapInterval(1) != 0) {
            sharedFpsLimiter.setVsyncPeriod(0.0);
            return;
        }
        interval = 1;
    }

    if (vsync_gl_finish == NULL) {
        vsync_gl_finish = (SvgaGlFinishProc)SDL_GL_GetProcAddress("glFinish");
    }

    vsync_refresh = refresh;
    vsync_interval = interval;
    vsync_period_ms = interval * 1000.0 / refresh;
    vsync_effective = true; // until the check says otherwise
    sharedFpsLimiter.setVsyncPeriod(vsync_period_ms);
}

// Is the display really pacing us? Averaged over two seconds of presents.
static void svga_vsync_check()
{
    Uint64 now = SDL_GetPerformanceCounter();
    if (vsync_last_present != 0) {
        double ms = (double)(now - vsync_last_present) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        if (ms < 250.0) {
            vsync_acc_ms += ms;
            vsync_acc_frames++;
        }
    }
    vsync_last_present = now;

    if (vsync_acc_frames >= 120) {
        double avg = vsync_acc_ms / vsync_acc_frames;
        if (vsync_effective && avg < vsync_period_ms - 1.5) {
            // Faster than the display can show them: the interval is not being honoured.
            vsync_effective = false;
            sharedFpsLimiter.setVsyncPeriod(0.0);
        }
        vsync_acc_ms = 0.0;
        vsync_acc_frames = 0;
    }
}

void svga_vsync_suspend(bool on)
{
    vsync_suspended = on;
}

void svga_set_vsync(bool on)
{
    vsync_wanted = on;
    vsync_dirty = true;
}

bool svga_get_vsync()
{
    return vsync_wanted;
}

void svga_vsync_state(int* interval, int* refresh, bool* effective)
{
    *interval = vsync_interval;
    *refresh = vsync_refresh;
    *effective = vsync_interval > 0 && vsync_effective;
}

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }
}

// 0x4CB850
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3, unsigned int srcX, unsigned int srcY, unsigned int srcWidth, unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
}

bool svga_init(VideoOptions* video_options)
{
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

    if (video_options->fullscreen) {
        // CE: _DESKTOP, not plain SDL_WINDOW_FULLSCREEN. A real fullscreen window makes SDL
        // switch the display mode, and SDL_video.c:ShouldMinimizeOnFocusLoss() then returns true
        // for it by default -- so ANY focus loss (alt-tab, a notification, Steam) calls
        // SDL_MinimizeWindow(). GNW95_isActive goes false with it, get_input() drops into
        // idleImpl()'s SDL_Delay(125), and the game sits at 0% CPU behind the taskbar looking
        // exactly like a crash -- it cost a play-test on 2026-08-28. _DESKTOP never minimizes,
        // and since the requested size already equals the desktop the picture is unchanged.
        // svga_toggle_fullscreen() has always used _DESKTOP; this just makes init agree with it.
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    // The game playing by itself (game/autotest.cc): a hidden window, never fullscreen.
    if (gnw_headless || gnw_visible_override) {
        windowFlags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (gnw_headless) {
            windowFlags |= SDL_WINDOW_HIDDEN;
        }
    }

    gSdlWindow = SDL_CreateWindow(GNW95_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        video_options->width * video_options->scale,
        video_options->height * video_options->scale,
        windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

    if (!createRenderer(video_options->width, video_options->height)) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;

        return false;
    }

    gSdlSurface = SDL_CreateRGBSurface(0,
        video_options->width,
        video_options->height,
        8,
        0,
        0,
        0,
        0);
    if (gSdlSurface == NULL) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = video_options->width - 1;
    scr_size.lry = video_options->height - 1;

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

    // gconfig_init() has already run by the time initWindow() reaches here, so the crt*
    // keys are readable. The GL objects themselves are built lazily on the first present.
    crt_init();

    // MODS "VSync" -- default ON since the release (his setting); 0 = the original sleep-paced
    // present. Applied at the first present, when the renderer's GL context is current.
    int vsync = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_VSYNC_KEY, &vsync);
    svga_set_vsync(vsync != 0);

    return true;
}

void svga_exit()
{
    destroyRenderer();

    if (gSdlWindow != NULL) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&scr_size);
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == NULL) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == NULL) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }

    // A new renderer is a new GL context, born with its own swap interval.
    vsync_dirty = true;

    return true;
}

static void destroyRenderer()
{
    // The GL context dies with the renderer, and every CRT object with it -- so the
    // handles must be dropped WITHOUT calling back into GL.
    crt_invalidate(true);

    if (gSdlTextureSurface != NULL) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = NULL;
    }

    if (gSdlTexture != NULL) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = NULL;
    }

    if (gSdlRenderer != NULL) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

// Alt+Enter runtime toggle between fullscreen and a window (bound in input.cc's SDL pump).
//
// The game's INTERNAL resolution never changes -- that stays whatever f1_res.ini set up
// (SCR_WIDTH/SCR_HEIGHT/SCALE_2X); only the presentation does. SDL posts a SIZE_CHANGED right
// after, which runs handleWindowSizeChanged() and rebuilds the renderer at the same logical size,
// so nothing else needs touching. Going back to fullscreen uses BORDERLESS desktop fullscreen
// rather than the exclusive mode-set the game boots with: no video mode change, so the flip is
// instant and alt-tab stays well behaved.
//
// Leaving fullscreen also RESIZES the window: it was created at width*scale x height*scale, which
// for a config matched to the monitor (e.g. 2560x1440) is a "window" exactly as big as the screen,
// with its title bar off the top. So size it to ~80% of the desktop, keeping the aspect ratio,
// and center it.
void svga_toggle_fullscreen()
{
    if (gSdlWindow == NULL) {
        return;
    }

    bool wasFullscreen = (SDL_GetWindowFlags(gSdlWindow) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(gSdlWindow, wasFullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);

    if (!wasFullscreen) {
        return; // now fullscreen -- nothing to size
    }

    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(gSdlWindow), &bounds) != 0) {
        return;
    }

    int w = screenGetWidth();
    int h = screenGetHeight();
    if (w <= 0 || h <= 0) {
        return;
    }

    int targetH = bounds.h * 4 / 5;
    int targetW = (int)((double)targetH * w / h);
    if (targetW > bounds.w * 4 / 5) {
        targetW = bounds.w * 4 / 5;
        targetH = (int)((double)targetW * h / w);
    }

    SDL_SetWindowSize(gSdlWindow, targetW, targetH);
    SDL_SetWindowPosition(gSdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void renderPresent()
{
    trace_alive(); // the hang watchdog's pulse (game_trace.cc)

    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);

    // With the CRT filter on the frame goes up through our own GL pass instead of SDL's
    // blit -- and then SDL must not draw anything else this frame, because its renderer
    // caches GL state we deliberately went around. crt_present() returns false whenever
    // it could not run (filter off, non-GL renderer, shader failed to build), and the
    // plain path below then runs exactly as it always did.
    if (!crt_present(gSdlWindow, gSdlRenderer, gSdlTexture, screenGetWidth(), screenGetHeight())) {
        SDL_RenderClear(gSdlRenderer);
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    }

    // The renderer's GL context is current here -- it has just drawn -- which makes this the one
    // safe place to change its swap interval.
    if (vsync_dirty) {
        svga_vsync_apply();
        vsync_suspend_applied = false;
    }

    // Loading: present at once, as without VSync. Back on with the first present after it.
    if (vsync_interval > 0 && vsync_suspend_applied != vsync_suspended) {
        SDL_GL_SetSwapInterval(vsync_suspended ? 0 : vsync_interval);
        vsync_suspend_applied = vsync_suspended;
        vsync_last_present = 0;
        vsync_acc_ms = 0.0;
        vsync_acc_frames = 0;
    }

    SDL_RenderPresent(gSdlRenderer);

    if (vsync_interval > 0 && !vsync_suspended) {
        // Wait for the swap itself, so the driver never queues frames ahead of the display.
        if (vsync_gl_finish != NULL) {
            vsync_gl_finish();
        }
        svga_vsync_check();
    }
}

} // namespace fallout
