#include "game/game_trace.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "game/config.h"
#include "game/gconfig.h"
#include "game/version.h"

#ifdef _WIN32
// dbghelp pulls in windows.h; keep the lean defines the project already sets.
#include <windows.h>
#include <dbghelp.h>
#else
#include <signal.h>
#endif

namespace fallout {

// --- Tunables -------------------------------------------------------------

#define TRACE_LINE_MAX 256        // max chars in one formatted log line
#define TRACE_RING_CAP 256        // events kept in memory for the crash tail
#define TRACE_HEARTBEAT_MS 1000.0 // state-snapshot cadence
#define TRACE_COALESCE_MAX_MS 2000.0 // force-flush a coalesced run after this idle
#define TRACE_COALESCE_MAX_COUNT 2000 // ...or this many repeats

// Only these categories coalesce (the storm-prone ones). Everything else is written
// the instant it happens so the file mirrors the action in near-real-time.
static bool trace_cat_coalesces(TraceCat c)
{
    return c == TRACE_INPUT || c == TRACE_WARN || c == TRACE_ANIM;
}

static const char* trace_cat_tag(TraceCat c)
{
    switch (c) {
    case TRACE_INPUT: return "INPUT";
    case TRACE_ACTION: return "ACTION";
    case TRACE_COMBAT: return "COMBAT";
    case TRACE_ANIM: return "ANIM";
    case TRACE_STATE: return "STATE";
    case TRACE_FLOW: return "FLOW";
    case TRACE_WARN: return "WARN";
    case TRACE_ERR: return "ERR";
    default: return "?";
    }
}

// --- State ----------------------------------------------------------------

static bool g_enabled = false;
static FILE* g_file = NULL;
static unsigned long long g_seq = 0;
static unsigned long long g_frame = 0;
static double g_start_ms = 0.0;
static double g_heartbeat_ms = 0.0; // session-ms of the last heartbeat
static float g_clock = 0.0f;        // realtime combat clock (for digest)
static TraceSnapshotFn g_snapshot = NULL;

static unsigned long long g_counts[TRACE_CAT_COUNT] = { 0 };

// In-memory ring of the last TRACE_RING_CAP emitted lines (for the crash tail).
static char g_ring[TRACE_RING_CAP][TRACE_LINE_MAX];
static int g_ring_head = 0; // next write slot
static int g_ring_count = 0;

// Latest STATE heartbeat line, surfaced verbatim in the crash digest.
static char g_last_state[TRACE_LINE_MAX] = { 0 };

// Pending coalesced run (at most one event is ever buffered).
static bool g_pend_active = false;
static TraceCat g_pend_cat = TRACE_INPUT;
static char g_pend_sub[48] = { 0 };
static char g_pend_msg[TRACE_LINE_MAX] = { 0 };
static unsigned int g_pend_count = 0;
static double g_pend_first_ms = 0.0;
static double g_pend_last_ms = 0.0;

static double trace_session_ms()
{
    return (double)SDL_GetTicks() - g_start_ms;
}

static void trace_ring_push(const char* line)
{
    snprintf(g_ring[g_ring_head], TRACE_LINE_MAX, "%s", line);
    g_ring_head = (g_ring_head + 1) % TRACE_RING_CAP;
    if (g_ring_count < TRACE_RING_CAP) {
        g_ring_count++;
    }
}

// Compose + write a single final line. count>1 appends the coalesced summary.
static void trace_write_line(TraceCat cat, const char* sub, const char* msg,
    unsigned int count, double firstMs, double lastMs)
{
    if (g_file == NULL) {
        return;
    }

    char line[TRACE_LINE_MAX];
    int n = snprintf(line, sizeof(line), "%llu s=%.2f [%s/%s] %s",
        ++g_seq, firstMs / 1000.0, trace_cat_tag(cat), sub, msg);

    if (count > 1 && n > 0 && n < (int)sizeof(line)) {
        snprintf(line + n, sizeof(line) - n, "  x%u/%.2fs", count, (lastMs - firstMs) / 1000.0);
    }

    fputs(line, g_file);
    fputc('\n', g_file);
    fflush(g_file);

    trace_ring_push(line);
    g_counts[cat] += count;

    if (cat == TRACE_STATE) {
        snprintf(g_last_state, sizeof(g_last_state), "%s", msg);
    }
}

static void trace_flush_pending()
{
    if (!g_pend_active) {
        return;
    }
    g_pend_active = false;
    trace_write_line(g_pend_cat, g_pend_sub, g_pend_msg, g_pend_count, g_pend_first_ms, g_pend_last_ms);
}

// --- AddressSanitizer defaults --------------------------------------------

// Baked-in ASAN_OPTIONS for the sanitizer build. The runtime calls this at startup, so the
// options hold NO MATTER HOW THE EXE IS LAUNCHED -- double-clicked, from a shortcut, from a
// debugger -- instead of only through the launcher script that sets the env var.
//
// This is not a nicety, it is the difference between a report and silence: the game is a WIN32
// GUI app, so ASan's stderr goes nowhere. Without log_path a detected error halts the process
// with NO window, NO event-log entry and NO file -- the trace just stops mid-line and looks
// exactly like a hard crash that ate its own black box. (Cost one play-test on 2026-08-27.)
//
// log_path stays RELATIVE on purpose: ASAN_OPTIONS is colon separated, so a "C:\..." value makes
// the flag parser bail out ("expected '=' in ASAN_OPTIONS") and the process exits before main.
// Relative means "next to the working directory", which for a double-clicked exe is its folder.
#if defined(__SANITIZE_ADDRESS__)
extern "C" __declspec(dllexport) const char* __asan_default_options()
{
    return "log_path=asan.log:halt_on_error=1:print_stacktrace=1:alloc_dealloc_mismatch=0:detect_stack_use_after_return=0";
}
#endif

// --- Crash digest ---------------------------------------------------------

// How many crash digests this session has written. Both the first-chance vectored handler and
// the unhandled filter can fire for the SAME fault, and a first-chance fault that something else
// legitimately handles must not be able to spam the log.
static int g_crash_digests = 0;

// Thread that called trace_init (i.e. the game loop). A crash on any OTHER thread -- the audio
// mixer, an SDL worker -- means a completely different suspect list, so the digest says which.
static unsigned long g_main_tid = 0;

static void trace_write_digest_body()
{
    if (g_file == NULL) {
        return;
    }

    double sec = trace_session_ms() / 1000.0;
    fprintf(g_file, "when: s=%.2f frame=%llu combat_t=%.2f seq=%llu\n", sec, g_frame, g_clock, g_seq);
    fprintf(g_file, "counts: INPUT=%llu ACTION=%llu COMBAT=%llu ANIM=%llu STATE=%llu FLOW=%llu WARN=%llu ERR=%llu\n",
        g_counts[TRACE_INPUT], g_counts[TRACE_ACTION], g_counts[TRACE_COMBAT], g_counts[TRACE_ANIM],
        g_counts[TRACE_STATE], g_counts[TRACE_FLOW], g_counts[TRACE_WARN], g_counts[TRACE_ERR]);
    fprintf(g_file, "last STATE: %s\n", g_last_state[0] != '\0' ? g_last_state : "(none)");

    fprintf(g_file, "ring tail (last %d events):\n", g_ring_count);
    int start = (g_ring_head - g_ring_count + TRACE_RING_CAP) % TRACE_RING_CAP;
    for (int i = 0; i < g_ring_count; i++) {
        int idx = (start + i) % TRACE_RING_CAP;
        fprintf(g_file, "  %s\n", g_ring[idx]);
    }
    fputs("=== end CRASH ===\n", g_file);
    fflush(g_file);
}

#ifdef _WIN32
static const char* trace_seh_describe(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    default: return "EXCEPTION";
    }
}

// Walk the faulting stack from the exception CONTEXT and write symbolic frames
// (name+offset, file:line) using DbgHelp. Requires a Release PDB next to the exe.
static void trace_seh_stack(CONTEXT* ctx)
{
    if (g_file == NULL || ctx == NULL) {
        return;
    }

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);

    // StackWalk64 mutates the context, so work on a copy.
    CONTEXT walk = *ctx;
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));

    DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = walk.Rip;
    frame.AddrFrame.Offset = walk.Rbp;
    frame.AddrStack.Offset = walk.Rsp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = walk.Eip;
    frame.AddrFrame.Offset = walk.Ebp;
    frame.AddrStack.Offset = walk.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    char symbolBuf[sizeof(SYMBOL_INFO) + 256] = { 0 };
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuf;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    fputs("stack:\n", g_file);
    for (int depth = 0; depth < 32; depth++) {
        if (!StackWalk64(machine, process, thread, &frame, &walk, NULL,
                SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        DWORD64 addr = frame.AddrPC.Offset;
        DWORD64 disp = 0;
        const char* name = "???";
        if (SymFromAddr(process, addr, &disp, symbol)) {
            name = symbol->Name;
        }

        IMAGEHLP_LINE64 lineInfo;
        memset(&lineInfo, 0, sizeof(lineInfo));
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &lineInfo)) {
            fprintf(g_file, "  %s+0x%llx (%s:%lu)\n", name, (unsigned long long)disp,
                lineInfo.FileName, lineInfo.LineNumber);
        } else {
            fprintf(g_file, "  %s+0x%llx (0x%llx)\n", name, (unsigned long long)disp,
                (unsigned long long)addr);
        }
    }
    fflush(g_file);
}

// On-demand symbolic backtrace of the CURRENT call stack, written under a "bt[label]:" header.
// Records WHO triggered a rare, suspicious event (e.g. an object erased/hidden mid-combat) so a
// "the sprite just vanished" report can be pinned to its exact call path without needing a crash.
// Symbols are initialized once and reused. No-op when tracing is off.
void trace_backtrace(const char* label)
{
    if (g_file == NULL) {
        return;
    }

    static bool symReady = false;
    HANDLE process = GetCurrentProcess();
    if (!symReady) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(process, NULL, TRUE);
        symReady = true;
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    RtlCaptureContext(&ctx);

    CONTEXT walk = ctx;
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = walk.Rip;
    frame.AddrFrame.Offset = walk.Rbp;
    frame.AddrStack.Offset = walk.Rsp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = walk.Eip;
    frame.AddrFrame.Offset = walk.Ebp;
    frame.AddrStack.Offset = walk.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    char symbolBuf[sizeof(SYMBOL_INFO) + 256] = { 0 };
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuf;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    HANDLE thread = GetCurrentThread();
    fprintf(g_file, "bt[%s]:\n", label != NULL ? label : "");
    for (int depth = 0; depth < 14; depth++) {
        if (!StackWalk64(machine, process, thread, &frame, &walk, NULL,
                SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        DWORD64 addr = frame.AddrPC.Offset;
        DWORD64 disp = 0;
        const char* name = "???";
        if (SymFromAddr(process, addr, &disp, symbol)) {
            name = symbol->Name;
        }
        IMAGEHLP_LINE64 lineInfo;
        memset(&lineInfo, 0, sizeof(lineInfo));
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &lineInfo)) {
            fprintf(g_file, "  %s (%s:%lu)\n", name, lineInfo.FileName, lineInfo.LineNumber);
        } else {
            fprintf(g_file, "  %s+0x%llx\n", name, (unsigned long long)disp);
        }
    }
    fflush(g_file);
}

// Faults worth a digest. Everything else a first-chance handler sees (C++ throws, debugger
// breakpoints, OutputDebugString, driver-internal probes) is normal traffic and stays out.
static bool trace_seh_is_fatal(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

// One fault -> one digest. `kind` says which handler caught it.
//
// ORDER MATTERS: the ring tail is written BEFORE the symbolic stack, because DbgHelp allocates
// and can die itself when the fault IS memory corruption -- and the last 256 events are worth
// more than the backtrace. Learned 2026-08-27 23:34: an ACCESS_VIOLATION inside ntdll's own
// exception dispatch left the log with NO digest at all, because the unhandled filter never got
// to run. Hence the vectored handler below, which runs first-chance, before that dispatch.
static void trace_write_crash(const char* kind, EXCEPTION_POINTERS* ep)
{
    if (g_file == NULL || ep == NULL || ep->ExceptionRecord == NULL) {
        return;
    }

    if (g_crash_digests >= 3) {
        return;
    }
    g_crash_digests++;

    trace_flush_pending();

    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(g_file, "=== CRASH (%s) ===\n", kind);
    fprintf(g_file, "exception: %s @ 0x%llx\n", trace_seh_describe(er->ExceptionCode),
        (unsigned long long)(uintptr_t)er->ExceptionAddress);
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
        && er->NumberParameters >= 2) {
        fprintf(g_file, "access: %s 0x%llx\n",
            er->ExceptionInformation[0] == 0 ? "read" : (er->ExceptionInformation[0] == 1 ? "write" : "exec"),
            (unsigned long long)er->ExceptionInformation[1]);
    }
    fprintf(g_file, "thread: %lu (game loop %lu)\n", GetCurrentThreadId(), g_main_tid);
    fflush(g_file);

    trace_write_digest_body();
    trace_seh_stack(ep->ContextRecord);
}

// First-chance handler: runs at the moment of the fault, before Windows unwinds anything, so it
// still reports when the unwinder itself walks into a smashed stack. It never handles anything --
// the normal crash path (and any real handler) still runs.
static LONG WINAPI trace_veh(EXCEPTION_POINTERS* ep)
{
    if (ep != NULL && ep->ExceptionRecord != NULL && trace_seh_is_fatal(ep->ExceptionRecord->ExceptionCode)) {
        trace_write_crash("first-chance", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI trace_seh_filter(EXCEPTION_POINTERS* ep)
{
    trace_write_crash("unhandled", ep);
    // Let the normal crash path proceed (so any debugger / Windows error UI still runs).
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void trace_posix_handler(int sig)
{
    if (g_file != NULL) {
        g_crash_digests++;
        trace_flush_pending();
        fputs("=== CRASH ===\n", g_file);
        fprintf(g_file, "signal: %d\n", sig);
        trace_write_digest_body();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void trace_install_crash_handlers()
{
#ifdef _WIN32
    // First (the 1 = call before any previously registered vectored handler), so a fault is
    // recorded even when exception dispatch dies before reaching the unhandled filter.
    AddVectoredExceptionHandler(1, trace_veh);
    SetUnhandledExceptionFilter(trace_seh_filter);
#else
    signal(SIGSEGV, trace_posix_handler);
    signal(SIGABRT, trace_posix_handler);
    signal(SIGFPE, trace_posix_handler);
    signal(SIGILL, trace_posix_handler);
#endif
}

// --- Public API -----------------------------------------------------------

void trace_init()
{
    if (g_enabled || g_file != NULL) {
        return; // idempotent
    }

    int on = 1;
    config_get_value(&game_config, GAME_CONFIG_PREFERENCES_KEY, GAME_CONFIG_TRACE_LOG_KEY, &on);
    if (!on) {
        return;
    }

    time_t now = time(NULL);
    struct tm* lt = localtime(&now);
    char stamp[32];
    if (lt != NULL) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", lt);
    } else {
        snprintf(stamp, sizeof(stamp), "session");
    }

    char* base = SDL_GetBasePath();
    char path[1280];
    snprintf(path, sizeof(path), "%strace_%s.log", base != NULL ? base : "", stamp);
    if (base != NULL) {
        SDL_free(base);
    }

    g_file = fopen(path, "w");
    if (g_file == NULL) {
        return;
    }

    g_enabled = true;
#ifdef _WIN32
    g_main_tid = GetCurrentThreadId();

    // Keep stack in RESERVE for the crash handler. A STACK_OVERFLOW otherwise leaves so little
    // room that the digest's DbgHelp walk cannot run -- and that is precisely the crash where the
    // frames matter most, because an overflow IS a runaway recursion and the repeating cycle in
    // the backtrace names the culprit outright. Seen 2026-08-28: the digest came out, the stack
    // did not. 256 KB is ample for StackWalk64 + SymFromAddr.
    {
        ULONG guarantee = 256 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }
#endif
    g_start_ms = (double)SDL_GetTicks();
    g_heartbeat_ms = 0.0;

    fprintf(g_file, "=== fallout-ce trace v%d.%d%c  %s ===\n", VERSION_MAJOR, VERSION_MINOR, VERSION_RELEASE, stamp);
    fputs("legend: <seq> s=<sessionSec> [CAT/sub] msg [ xN/span if coalesced]\n", g_file);
    fputs("cats: INPUT ACTION COMBAT ANIM STATE FLOW WARN ERR\n", g_file);
    fflush(g_file);

    trace_install_crash_handlers();
}

void trace_shutdown()
{
    if (g_file == NULL) {
        return;
    }

    static bool wroteEnd = false;
    if (!wroteEnd) {
        wroteEnd = true;
        trace_flush_pending();
        fputs("=== session end ===\n", g_file);
        fprintf(g_file, "counts: INPUT=%llu ACTION=%llu COMBAT=%llu ANIM=%llu STATE=%llu FLOW=%llu WARN=%llu ERR=%llu  frames=%llu\n",
            g_counts[TRACE_INPUT], g_counts[TRACE_ACTION], g_counts[TRACE_COMBAT], g_counts[TRACE_ANIM],
            g_counts[TRACE_STATE], g_counts[TRACE_FLOW], g_counts[TRACE_WARN], g_counts[TRACE_ERR], g_frame);
        fflush(g_file);
    }

    // Deliberately DO NOT fclose / null g_file here. trace_shutdown runs inside
    // main_exit_system BEFORE game_exit() + SDL_Quit(), and a crash in THOSE (the
    // realtime "crashed on the way out after leaving a map" case) happens after this
    // returns. Keeping the handle open + the SEH filter armed means such a post-shutdown
    // crash still appends a full "=== CRASH ===" digest after the session-end line. Every
    // line is already fflush'd, so leaving the FILE* for the OS to reclaim at process exit
    // loses nothing. g_enabled stays true only so trace_enabled()-gated callers still work
    // during the remaining teardown; no more heartbeats fire (trace_tick stops with the loop).
}

bool trace_enabled()
{
    return g_enabled;
}

void trace_set_snapshot_fn(TraceSnapshotFn fn)
{
    g_snapshot = fn;
}

void trace_set_clock(float seconds)
{
    g_clock = seconds;
}

void trace_event(TraceCat cat, const char* sub, const char* fmt, ...)
{
    if (!g_enabled || g_file == NULL) {
        return;
    }
    if (sub == NULL) {
        sub = "";
    }

    char msg[TRACE_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    double now = trace_session_ms();

    if (trace_cat_coalesces(cat)) {
        if (g_pend_active && g_pend_cat == cat && strcmp(g_pend_sub, sub) == 0) {
            g_pend_count++;
            g_pend_last_ms = now;
            snprintf(g_pend_msg, sizeof(g_pend_msg), "%s", msg);
            if (g_pend_count >= TRACE_COALESCE_MAX_COUNT) {
                trace_flush_pending();
            }
            return;
        }
        // Different (cat,sub): close out any run, then start a new one.
        trace_flush_pending();
        g_pend_active = true;
        g_pend_cat = cat;
        snprintf(g_pend_sub, sizeof(g_pend_sub), "%s", sub);
        snprintf(g_pend_msg, sizeof(g_pend_msg), "%s", msg);
        g_pend_count = 1;
        g_pend_first_ms = now;
        g_pend_last_ms = now;
        return;
    }

    // Non-coalescing category: flush any open run and write immediately.
    trace_flush_pending();
    trace_write_line(cat, sub, msg, 1, now, now);
}

void trace_tick(float dt)
{
    if (!g_enabled) {
        return;
    }
    (void)dt;
    g_frame++;

    double now = trace_session_ms();

    // Age out a coalesced run that has gone quiet so it can't linger unbounded.
    if (g_pend_active && (now - g_pend_last_ms) >= TRACE_COALESCE_MAX_MS) {
        trace_flush_pending();
    }

    // Periodic heartbeat snapshot.
    if (g_snapshot != NULL && (now - g_heartbeat_ms) >= TRACE_HEARTBEAT_MS) {
        g_heartbeat_ms = now;
        g_snapshot();
    }
}

} // namespace fallout
