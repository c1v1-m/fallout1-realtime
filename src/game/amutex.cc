#include "game/amutex.h"

#include "plib/gnw/winmain.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fallout {

#ifdef _WIN32
// 0x540010
static HANDLE autorun_mutex;
#endif

// 0x413450
bool autorun_mutex_create()
{
#ifdef _WIN32
    autorun_mutex = CreateMutexA(NULL, FALSE, "InterplayGenericAutorunMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(autorun_mutex);
        return false;
    }
    // the 3D editor program must never start beside the game (its start clears the temporary map
    // states a running game needs): a mutex it may not open counts as a game running
    if (autorun_mutex == NULL && GNW95_editor_app) {
        return false;
    }
#endif

    return true;
}

// 0x413490
void autorun_mutex_destroy()
{
#ifdef _WIN32
    if (autorun_mutex != NULL) {
        CloseHandle(autorun_mutex);
    }
#endif
}

} // namespace fallout
