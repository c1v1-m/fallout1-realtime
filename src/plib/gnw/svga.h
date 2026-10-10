#ifndef FALLOUT_PLIB_GNW_SVGA_H_
#define FALLOUT_PLIB_GNW_SVGA_H_

#include <SDL.h>

#include "fps_limiter.h"
#include "plib/gnw/rect.h"
#include "plib/gnw/svga_types.h"

namespace fallout {

extern Rect scr_size;
extern ScreenBlitFunc* scr_blit;

extern SDL_Window* gSdlWindow;
extern SDL_Surface* gSdlSurface;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Texture* gSdlTexture;
extern SDL_Surface* gSdlTextureSurface;
extern FpsLimiter sharedFpsLimiter;

void GNW95_SetPaletteEntries(unsigned char* a1, int a2, int a3);
void GNW95_SetPalette(unsigned char* palette);
void GNW95_ShowRect(unsigned char* src, unsigned int src_pitch, unsigned int a3, unsigned int src_x, unsigned int src_y, unsigned int src_width, unsigned int src_height, unsigned int dest_x, unsigned int dest_y);

bool svga_init(VideoOptions* video_options);
void svga_exit();
int screenGetWidth();
int screenGetHeight();
void handleWindowSizeChanged();
void svga_toggle_fullscreen();
void renderPresent();

// MODS "VSync" (cfg vsync): hand each frame to the display on its refresh, at ~60 fps whatever the
// panel's rate. Takes effect at the next present. svga_vsync_state: the swap interval in force
// (0 = none), the refresh it was derived from, and whether the display is really pacing the game.
void svga_set_vsync(bool on);
bool svga_get_vsync();
void svga_vsync_state(int* interval, int* refresh, bool* effective);
// While a map loads: present without waiting for the display (map.cc brackets map_load_file).
void svga_vsync_suspend(bool on);

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_SVGA_H_ */
