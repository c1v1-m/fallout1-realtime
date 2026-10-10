#ifndef FALLOUT_GAME_VERSION_H_
#define FALLOUT_GAME_VERSION_H_

#include <stddef.h>

namespace fallout {

// The size of buffer for version string.
#define VERSION_MAX 32

#define VERSION_MAJOR 1
#define VERSION_MINOR 1
#define VERSION_RELEASE 'R'

#define VERSION_BUILD_TIME "Nov 11 1997 14:59:39"

// Fallout 1 Realtime's own version, shown beside the game's in the main menu corner (and on
// Ctrl+V). NOT VERSION_MAJOR/MINOR above: those are written into every save file.
#define REALTIME_VERSION "0.1.0"

// The window title. The development build says so, so the two can sit side by side.
#ifdef FALLOUT_RELEASE
#define REALTIME_WINDOW_TITLE "Fallout 1 Realtime"
#else
#define REALTIME_WINDOW_TITLE "Fallout 1 Realtime (dev)"
#endif

char* getverstr(char* dest, size_t size);

} // namespace fallout

#endif /* FALLOUT_GAME_VERSION_H_ */
