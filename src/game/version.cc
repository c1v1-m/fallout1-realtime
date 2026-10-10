#include "game/version.h"

#include <stdio.h>

namespace fallout {

// 0x4A10C0
char* getverstr(char* dest, size_t size)
{
#ifdef FALLOUT_RELEASE
    snprintf(dest, size, "FALLOUT %d.%d  RT %s", VERSION_MAJOR, VERSION_MINOR, REALTIME_VERSION);
#else
    snprintf(dest, size, "FALLOUT %d.%d  RT %s DEV", VERSION_MAJOR, VERSION_MINOR, REALTIME_VERSION);
#endif
    return dest;
}

} // namespace fallout
