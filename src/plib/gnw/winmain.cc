#include "plib/gnw/winmain.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "game/main.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/svga.h"

#if __APPLE__ && TARGET_OS_IOS
#include "platform/ios/paths.h"
#endif

namespace fallout {

// 0x53A290
bool GNW95_isActive = false;

bool gnw_headless = false;
bool gnw_visible_override = false;

#if _WIN32
// 0x53A294
HANDLE GNW95_mutex = NULL;
#endif

bool GNW95_beside = false;
bool GNW95_editor_app = false;

// 0x6B0760
char GNW95_title[256];

int main(int argc, char* argv[])
{
    int rc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--autotest") == 0) {
            GNW95_beside = true;
        }
#ifndef FALLOUT_PUBLIC // the 3D editor is not in the GitHub zip (CMakeLists.txt FALLOUT_PUBLIC)
        if (strcmp(argv[i], "--3d-editor") == 0) {
            GNW95_editor_app = true;
        }
#endif
    }
#ifndef FALLOUT_PUBLIC
    if (argc > 0 && argv[0] != NULL) {
        char name[512];
        snprintf(name, sizeof(name), "%s", argv[0]);
        for (char* c = name; *c != '\0'; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        const char* slash = strrchr(name, '\\');
        const char* file = slash != NULL ? slash + 1 : name;
        slash = strrchr(file, '/');
        file = slash != NULL ? slash + 1 : file;
        if (strstr(file, "3d-editor") != NULL) {
            GNW95_editor_app = true;
        }
    }
#endif
#ifndef FALLOUT_RELEASE
    // the 3D editor program played by itself (FALLOUT_FP_APP_TEST, development): no window on his screen
    if (GNW95_editor_app && getenv("FALLOUT_FP_APP_TEST") != NULL) {
        gnw_headless = true;
    }
#endif

#if _WIN32
    if (!GNW95_beside) {
        GNW95_mutex = CreateMutexA(0, TRUE, "GNW95MUTEX");
        if (GetLastError() != ERROR_SUCCESS) {
            return 0;
        }
    }
#endif

#if __APPLE__ && TARGET_OS_IOS
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(iOSGetDocumentsPath());
#endif

#if __APPLE__ && TARGET_OS_OSX
    char* basePath = SDL_GetBasePath();
    chdir(basePath);
    SDL_free(basePath);
#endif

#if __ANDROID__
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    chdir(SDL_AndroidGetExternalStoragePath());
#endif

    SDL_ShowCursor(SDL_DISABLE);

    GNW95_isActive = true;
    rc = gnw_main(argc, argv);

#if _WIN32
    CloseHandle(GNW95_mutex);
#endif

    return rc;
}

} // namespace fallout

int main(int argc, char* argv[])
{
    return fallout::main(argc, argv);
}
