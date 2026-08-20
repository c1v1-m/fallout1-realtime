#ifndef FALLOUT_GAME_SFXCACHE_H_
#define FALLOUT_GAME_SFXCACHE_H_

namespace fallout {

// The maximum number of sound effects that can be loaded and played
// simultaneously. CE: raised from the vanilla 4 -- realtime combat_rt has many
// critters attacking at once and a single ranged shot queues up to three effects
// (fire + ammo-flying + hit), so 4 overflowed and concurrent gunshots went silent.
// Must stay below AUDIO_ENGINE_SOUND_BUFFERS (which also serves music + speech).
#define SOUND_EFFECTS_MAX_COUNT 16

int sfxc_init(int cache_size, const char* effectsPath);
void sfxc_exit();
int sfxc_is_initialized();
void sfxc_flush();
int sfxc_cached_open(const char* fname, int mode);
int sfxc_cached_close(int handle);
int sfxc_cached_read(int handle, void* buf, unsigned int size);
int sfxc_cached_write(int handle, const void* buf, unsigned int size);
long sfxc_cached_seek(int handle, long offset, int origin);
long sfxc_cached_tell(int handle);
long sfxc_cached_file_size(int handle);

} // namespace fallout

#endif /* FALLOUT_GAME_SFXCACHE_H_ */
