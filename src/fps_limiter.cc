#include "fps_limiter.h"

#include <SDL.h>

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _ticks(0)
    , _vsyncPeriodMs(0.0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
}

void FpsLimiter::throttle() const
{
    if (_vsyncPeriodMs > 0.0) {
        unsigned int floorMs = _vsyncPeriodMs > 4.0 ? (unsigned int)(_vsyncPeriodMs - 3.0) : 1;
        unsigned int elapsed = SDL_GetTicks() - _ticks;
        if (floorMs > elapsed) {
            SDL_Delay(floorMs - elapsed);
        }
        return;
    }

    if (1000 / _fps > SDL_GetTicks() - _ticks) {
        SDL_Delay(1000 / _fps - (SDL_GetTicks() - _ticks));
    }
}

void FpsLimiter::setVsyncPeriod(double periodMs)
{
    _vsyncPeriodMs = periodMs > 0.0 ? periodMs : 0.0;
}

} // namespace fallout
