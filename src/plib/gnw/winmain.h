#ifndef FALLOUT_PLIB_GNW_WINMAIN_H_
#define FALLOUT_PLIB_GNW_WINMAIN_H_

#ifdef _WIN32
#include <windows.h>
#endif

namespace fallout {

extern bool GNW95_isActive;

// The game plays by itself (game/autotest.cc): a hidden window that never loses "focus", so it never
// sleeps in GNW95_lost_focus. gnw_visible_override shows the window anyway (--autotest-visible).
extern bool gnw_headless;
extern bool gnw_visible_override;

#ifdef _WIN32
extern HANDLE GNW95_mutex;
#endif

extern char GNW95_title[256];

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_WINMAIN_H_ */
