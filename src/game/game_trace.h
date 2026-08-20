#ifndef FALLOUT_GAME_GAME_TRACE_H_
#define FALLOUT_GAME_GAME_TRACE_H_

// Flight-recorder / health-monitor for the realtime build. ONE structured log
// (trace_<date>_<time>.log next to the exe) that records the player's input, the
// engine's actions, animation-table state and a periodic state heartbeat, plus a
// crash handler that, on a hard crash, writes a compact DIGEST: the faulting
// address + symbolic call stack + the last heartbeat + event counts + the tail of
// the in-memory ring buffer. Designed to be read by layers: read the digest first
// (~50 lines, usually enough), grep the body by category/time only if needed.
//
// Coalescing collapses a rapid run of the same (category, sub) event into a single
// "xN over Ds" line, so storms (e.g. a 60Hz attack-retry burst) cost one line, not
// hundreds. Quality-over-performance: every line is flushed to disk immediately so a
// crash that bypasses the handler still leaves the trace on disk.

namespace fallout {

// Event categories. The short tag written to the log is defined by trace_cat_tag()
// in the .cc -- keep them in sync.
enum TraceCat {
    TRACE_INPUT = 0, // player input: gamepad button edges, keys, stick gestures
    TRACE_ACTION,    // high-level player intent: move, attack, inventory, save, swap
    TRACE_COMBAT,    // attack issued / resolve / commit / hit / miss / death / AP / watchdog
    TRACE_ANIM,      // animation register failures, table saturation
    TRACE_STATE,     // periodic heartbeat snapshot
    TRACE_FLOW,      // map load, combat enter/exit, save/load
    TRACE_WARN,      // an invariant looked wrong but the game continued
    TRACE_ERR,       // a hard error
    TRACE_CAT_COUNT,
};

// A snapshot callback the recorder invokes ~once per second (driven by trace_tick).
// It should emit one TRACE_STATE heartbeat line and any health-check TRACE_WARNs.
// Registered by the owner of the game state (combat_rt) so this module stays
// dependency-free.
typedef void (*TraceSnapshotFn)();

// Open the log, write the header, install the crash handlers. Idempotent. Reads the
// "trace_log" preference (default on); if disabled, every other call is a cheap no-op.
void trace_init();

// Write the footer (session summary) and close the log. Safe to call without init.
void trace_shutdown();

// True when tracing is active (config-enabled and the file opened).
bool trace_enabled();

// Register the heartbeat snapshot callback (see TraceSnapshotFn). Pass nullptr to clear.
void trace_set_snapshot_fn(TraceSnapshotFn fn);

// Set the realtime combat clock shown by combat heartbeats / digest (0 out of combat).
void trace_set_clock(float seconds);

// Record one structured event. `sub` is a short coalescing key ("A.press", "rc-1",
// "attack", ...); a rapid run of the same (cat, sub) collapses into one xN line.
void trace_event(TraceCat cat, const char* sub, const char* fmt, ...);

// Per-frame pump: advances the frame counter, ages out a stale coalesced run, and
// fires the heartbeat snapshot when a second has elapsed. dt is wall-clock seconds.
void trace_tick(float dt);

// Write a symbolic backtrace of the current call stack to the log (header "bt[label]:").
// For pinning down WHO triggered a rare event (an object erased/hidden, a sprite vanishing).
// No-op when tracing is disabled. Cheap enough for rare events; do NOT call every frame.
void trace_backtrace(const char* label);

} // namespace fallout

#endif /* FALLOUT_GAME_GAME_TRACE_H_ */
