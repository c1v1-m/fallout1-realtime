#ifndef FALLOUT_PLIB_GNW_CRT_SHADER_H_
#define FALLOUT_PLIB_GNW_CRT_SHADER_H_

#include <SDL.h>

namespace fallout {

// Optional CRT post-process for the final present. See crt_shader.cc for the model.
//
// The whole feature is a no-op unless SDL ended up on the OpenGL renderer and the
// program compiles; every entry point below is safe to call regardless.

// Reads the [preferences] crt* keys out of game_config. Call once, after gconfig_init
// and after the SDL renderer exists (end of svga_init).
void crt_init();

// Drops the GL objects. `contextGone` = the GL context died with the renderer, so the
// handles must be forgotten WITHOUT calling into GL (SDL_DestroyRenderer already took
// them); false = a normal teardown where deleting is still legal.
void crt_invalidate(bool contextGone);

// The user-facing toggle (Preferences screen / `crt` cfg key). Enabling on a renderer
// that cannot run the shader silently leaves the plain blit in place.
bool crt_is_enabled();
void crt_set_enabled(bool enabled);

// True only once a program has actually been built, i.e. after the first successful
// frame. Meaningless before that -- the shader is compiled lazily.
bool crt_is_active();

// Presents `texture` (the game's full-frame texture) through the CRT pass. Returns
// false if it could not -- the caller must then do its own plain SDL_RenderCopy.
// Does NOT swap; the caller still calls SDL_RenderPresent.
bool crt_present(SDL_Window* window, SDL_Renderer* renderer, SDL_Texture* texture, int texWidth, int texHeight);

// Ask for the NEXT filtered frame to be written to `path` as a 24-bit BMP.
//
// The engine's own screenshot (dump_screen) re-runs the window manager's blit into a
// private 8-bit buffer and never touches the GPU, so it captures the game BEFORE this
// shader -- which is correct for its purpose and useless for judging the filter. This
// reads the actual framebuffer after the pass instead.
//
// Deferred rather than immediate because the shader only runs inside crt_present, and
// F12 is handled from the input pump. Silently does nothing if the filter is not active.
void crt_request_capture(const char* path);

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_CRT_SHADER_H_ */
