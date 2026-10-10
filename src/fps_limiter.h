#ifndef FALLOUT_FPS_LIMITER_H_
#define FALLOUT_FPS_LIMITER_H_

namespace fallout {

class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);
    void mark();
    void throttle() const;

    // VSYNC PACING (svga.cc). With a swap interval in force the present itself waits for the
    // display, and the sleep must never be what sets the pace: sleeping to 16 ms and THEN waiting
    // for the next refresh alternates long and short frames. With a period set, throttle() only
    // keeps a floor a few ms under it -- so a frame the present already paced is never delayed,
    // and a loop that does not present still cannot spin. 0 = the original 60 fps sleep.
    void setVsyncPeriod(double periodMs);

private:
    const unsigned int _fps;
    unsigned int _ticks;
    double _vsyncPeriodMs;
};

} // namespace fallout

#endif /* FALLOUT_FPS_LIMITER_H_ */
