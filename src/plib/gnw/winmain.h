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

// A self-played test (--autotest) runs beside a game that is already open: it takes neither
// single-instance mutex (gnw.cc names its window mutex apart for it).
extern bool GNW95_beside;

// The 3D editor program (game/fp_view.cc): this same game started as Fallout1-3D-Editor.exe -- any exe
// whose name holds "3d-editor" -- or with --3d-editor. It takes the single-instance mutex like the
// game: a start clears the temporary map states (MAPS\*.SAV) a running game still needs.
extern bool GNW95_editor_app;

extern char GNW95_title[256];

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_WINMAIN_H_ */
