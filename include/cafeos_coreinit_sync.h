#ifndef ARKCHEMY_CAFEOS_COREINIT_SYNC_H
#define ARKCHEMY_CAFEOS_COREINIT_SYNC_H

/* ppc_runtime.h must come first, before <pthread.h>/<time.h> below --
 * it's what defines _POSIX_C_SOURCE (needed for clock_gettime/nanosleep/
 * gmtime_r/pthread_mutexattr_settype+PTHREAD_MUTEX_RECURSIVE), and
 * glibc's feature-test macros only take effect if defined before the
 * *first* system header of the translation unit -- see ppc_runtime.h's
 * own comment on this. */
#include "ppc_runtime.h"

#include <pthread.h>
#include <time.h>

/* Real Wii U hardware clock constants -- sourced from Cemu's real
 * emulation core (src/Cafe/HW/Espresso/PPCState.h), not guessed. Used
 * by OSWaitEventWithTimeout below (real tick->wall-clock conversion for
 * its timeout) and by OSGetSystemInfo/OSSleepTicks/OSTicksToCalendarTime
 * further down this file -- moved up here since it's now needed before
 * those. See those functions' own comments for the full story. */
enum {
    ARKCHEMY_ESPRESSO_CORE_CLOCK = 1243125000,
    ARKCHEMY_ESPRESSO_BUS_CLOCK = 248625000,
    ARKCHEMY_ESPRESSO_TIMER_CLOCK = ARKCHEMY_ESPRESSO_BUS_CLOCK / 4,
};

/*
 * Phase 1d CafeOS runtime shim -- coreinit synchronization primitives
 * (OSMutex/OSEvent) and timing.
 *
 * Previously every one of these was a genuine no-op, correct only
 * because this runtime had exactly one PpcContext executing
 * sequentially with no real concurrent execution at all. Real threading
 * (OSCreateThread, cafeos_coreinit_thread.h) now exists, which makes
 * that reasoning actively *wrong*: two real host threads really can
 * contend for the same OSMutex or wait on the same OSEvent now, so a
 * no-op here would be a real, silent data race -- exactly the failure
 * mode this project's whole verification discipline exists to prevent.
 * Rewritten to be genuinely real, backed by real pthread primitives.
 *
 * OSMutex/OSEvent/OSSemaphore are opaque, caller-allocated structs in
 * the real API (real game code never reads/writes their fields itself,
 * only passes the pointer to these functions) -- this shim still never
 * writes through them, but now needs *some* real, persistent state to
 * back a real lock/wait, so it keeps a small host-side (not
 * guest-memory) table per primitive type, keyed by the real, stable
 * guest address the game already allocated -- the same "host-side state
 * keyed by a real guest address" pattern already used throughout this
 * project (the FS handle table, the MEM* heap table, the AXVoice pool).
 *
 * Real signatures/semantics confirmed against devkitPro/wut's
 * coreinit/mutex.h, event.h, semaphore.h, cross-checked against Cemu's
 * real HLE implementation (src/Cafe/OS/libs/coreinit/
 * coreinit_Synchronization.cpp) for exact return-value conventions wut's
 * headers don't spell out (e.g. OSSignalSemaphore/OSWaitSemaphore both
 * return the *previous* count, confirmed directly from Cemu's source,
 * not guessed).
 *
 * OSMutex is real, confirmed *recursive* (wut: "supports recursive
 * locking", same as std::recursive_mutex) -- backed by a real
 * `pthread_mutex_t` created with `PTHREAD_MUTEX_RECURSIVE`, which
 * matches this exactly with no extra bookkeeping needed.
 *
 * OSEvent's manual/auto-reset semantics (confirmed against wut's docs
 * and Cemu's real branching logic) are implemented with the standard,
 * race-free mutex+condvar+flag+epoch pattern: `signaled` is a sticky
 * flag for "signaled with nobody currently waiting" (persists for the
 * next waiter), `epoch` is bumped on every broadcast-style wake so
 * every *currently* blocked waiter reliably wakes exactly once without
 * a shared single-use flag racing between them (needed because real
 * hardware's explicit per-thread wake queue doesn't have a direct
 * analog in POSIX condvars, which always require a recheck loop).
 * `waiting_count` tracks whether anyone is currently blocked, to
 * replicate the real "empty queue -> stays signaled" vs. "non-empty
 * queue -> wakes waiters, doesn't persist" branch Cemu's source shows
 * for both OSSignalEvent and OSSignalEventAll in AUTO mode.
 */

#define ARKCHEMY_SYNC_TABLE_SIZE 64

/* --- OSMutex (real, recursive) --- */
typedef struct {
    uint32_t guest_addr;
    int active;
    pthread_mutex_t mutex;
} ArkchemyMutexEntry;
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyMutexEntry g_arkchemy_mutexes[ARKCHEMY_SYNC_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_mutex_table_lock;

/* Finds (or, if requested, lazily creates) the real pthread_mutex_t
 * backing a given guest OSMutex address. Lazy creation on first use
 * (not just on OSInitMutex) is a defensive safety net -- real code
 * always calls OSInitMutex first, but this avoids a NULL-mutex crash
 * if that assumption is ever violated, at the cost of never resetting
 * an already-initialized entry's state (fine: real OSInitMutex on an
 * address already in use would be a real game bug regardless). */
static inline pthread_mutex_t *arkchemy_mutex_get(uint32_t addr) {
    int i, free_slot = -1;
    pthread_mutex_t *result = NULL;
    pthread_mutex_lock(&g_arkchemy_mutex_table_lock);
    for (i = 0; i < ARKCHEMY_SYNC_TABLE_SIZE; i++) {
        if (g_arkchemy_mutexes[i].active && g_arkchemy_mutexes[i].guest_addr == addr) {
            result = &g_arkchemy_mutexes[i].mutex;
            break;
        }
        if (free_slot < 0 && !g_arkchemy_mutexes[i].active) free_slot = i;
    }
    if (result == NULL && free_slot >= 0) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_arkchemy_mutexes[free_slot].mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        g_arkchemy_mutexes[free_slot].guest_addr = addr;
        g_arkchemy_mutexes[free_slot].active = 1;
        result = &g_arkchemy_mutexes[free_slot].mutex;
    }
    pthread_mutex_unlock(&g_arkchemy_mutex_table_lock);
    return result;
}

static inline void ppc_import_coreinit_OSInitMutex(PpcContext *ctx) { (void)arkchemy_mutex_get(ctx->r[3]); }
static inline void ppc_import_coreinit_OSLockMutex(PpcContext *ctx) { pthread_mutex_lock(arkchemy_mutex_get(ctx->r[3])); }
static inline void ppc_import_coreinit_OSUnlockMutex(PpcContext *ctx) { pthread_mutex_unlock(arkchemy_mutex_get(ctx->r[3])); }
static inline void ppc_import_coreinit_OSTryLockMutex(PpcContext *ctx) {
    ctx->r[3] = (pthread_mutex_trylock(arkchemy_mutex_get(ctx->r[3])) == 0) ? 1u : 0u;
}

/* --- OSEvent (real, manual/auto-reset) --- */
typedef struct {
    uint32_t guest_addr;
    int active;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int signaled;
    int mode; /* 0 = OS_EVENT_MODE_MANUAL, 1 = OS_EVENT_MODE_AUTO */
    uint64_t epoch;
    int waiting_count;
} ArkchemyEventEntry;
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyEventEntry g_arkchemy_events[ARKCHEMY_SYNC_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_event_table_lock;

static inline ArkchemyEventEntry *arkchemy_event_get(uint32_t addr, int create_with_value, int create_with_mode) {
    int i, free_slot = -1;
    ArkchemyEventEntry *result = NULL;
    pthread_mutex_lock(&g_arkchemy_event_table_lock);
    for (i = 0; i < ARKCHEMY_SYNC_TABLE_SIZE; i++) {
        if (g_arkchemy_events[i].active && g_arkchemy_events[i].guest_addr == addr) {
            result = &g_arkchemy_events[i];
            break;
        }
        if (free_slot < 0 && !g_arkchemy_events[i].active) free_slot = i;
    }
    if (result == NULL && free_slot >= 0) {
        ArkchemyEventEntry *e = &g_arkchemy_events[free_slot];
        pthread_mutex_init(&e->lock, NULL);
        pthread_cond_init(&e->cond, NULL);
        e->guest_addr = addr;
        e->active = 1;
        e->signaled = create_with_value;
        e->mode = create_with_mode;
        e->epoch = 0;
        e->waiting_count = 0;
        result = e;
    }
    pthread_mutex_unlock(&g_arkchemy_event_table_lock);
    return result;
}

static inline void ppc_import_coreinit_OSInitEvent(PpcContext *ctx) {
    /* void OSInitEvent(OSEvent *event, BOOL value, OSEventMode mode) */
    uint32_t addr = ctx->r[3];
    int value = (int)(ctx->r[4] != 0);
    int mode = (int)ctx->r[5];
    ArkchemyEventEntry *e = arkchemy_event_get(addr, value, mode);
    /* Re-initializing an address already in this table (a real Init
     * called twice) resets its state -- matches real hardware, which
     * always fully re-initializes the struct. */
    pthread_mutex_lock(&e->lock);
    e->signaled = value;
    e->mode = mode;
    e->epoch++;
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSSignalEvent(PpcContext *ctx) {
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1 /* AUTO default if never Init'd */);
    pthread_mutex_lock(&e->lock);
    if (!e->signaled) {
        if (e->mode == 1 /* AUTO */) {
            if (e->waiting_count == 0) {
                e->signaled = 1;
            } else {
                e->epoch++;
                pthread_cond_signal(&e->cond); /* wake exactly one */
            }
        } else { /* MANUAL */
            e->signaled = 1;
            e->epoch++;
            pthread_cond_broadcast(&e->cond);
        }
    }
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSSignalEventAll(PpcContext *ctx) {
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    pthread_mutex_lock(&e->lock);
    if (!e->signaled) {
        if (e->mode == 1 /* AUTO */) {
            if (e->waiting_count == 0) {
                e->signaled = 1;
            } else {
                e->epoch++;
                pthread_cond_broadcast(&e->cond); /* wake everyone currently waiting; doesn't persist */
            }
        } else { /* MANUAL */
            e->signaled = 1;
            e->epoch++;
            pthread_cond_broadcast(&e->cond);
        }
    }
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSResetEvent(PpcContext *ctx) {
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    pthread_mutex_lock(&e->lock);
    e->signaled = 0;
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSWaitEvent(PpcContext *ctx) {
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    pthread_mutex_lock(&e->lock);
    if (e->signaled) {
        if (e->mode == 1 /* AUTO */) e->signaled = 0;
    } else {
        uint64_t my_epoch = e->epoch;
        e->waiting_count++;
        while (!e->signaled && e->epoch == my_epoch) {
            pthread_cond_wait(&e->cond, &e->lock);
        }
        e->waiting_count--;
        if (e->signaled && e->mode == 1 /* AUTO */) e->signaled = 0;
    }
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSWaitEventWithTimeout(PpcContext *ctx) {
    /* BOOL OSWaitEventWithTimeout(OSEvent *event, OSTime timeout) --
     * timeout is a real relative duration in timer ticks (confirmed via
     * Cemu's real HLE: ConvertNsToTimerTicks), not an absolute deadline.
     * Converted to real wall-clock time via the same confirmed
     * ARKCHEMY_ESPRESSO_TIMER_CLOCK this file's OSSleepTicks already
     * uses. Returns real TRUE if actually signaled, FALSE on a real
     * timeout -- not the old "always true, never times out" stand-in. */
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    /* PPC EABI: a 64-bit argument starts on an ODD register, so with the
     * OSEvent* in r3 the OSTime is in r5:r6 -- NOT r4:r5. This read r4:r5
     * until 2026-09-01. r4 is never written by the caller, so the high word
     * was whatever the previous code left behind; the observed spin had
     * r4=0x0452cfac against a real timeout of 0x1f:0x0010a444. Same defect
     * as the Atomic64 shims fixed on 2026-08-30, in the file that fix's
     * sweep did not look at. */
    int64_t timeout_ticks = (int64_t)((((uint64_t)ctx->r[5]) << 32) | (uint64_t)ctx->r[6]);
    int woke_signaled = 1;
    pthread_mutex_lock(&e->lock);
    if (e->signaled) {
        if (e->mode == 1) e->signaled = 0;
    } else if (timeout_ticks <= 0) {
        woke_signaled = 0; /* zero/negative timeout, not yet signaled -- immediate timeout */
    } else {
        struct timespec deadline;
        /* Split seconds from the remainder before scaling. ticks * 1000000000
         * overflows int64 above ~9.2e9 ticks, which is only ~148 seconds at
         * this clock -- well inside the range real timeouts use, and the wrap
         * lands negative as often as not, producing an already-expired
         * deadline and a busy spin. OSTicksToCalendarTime below already does
         * it this way. The remainder is < TIMER_CLOCK, so the multiply is
         * bounded by ~6.2e16 and cannot overflow. */
        int64_t secs = timeout_ticks / ARKCHEMY_ESPRESSO_TIMER_CLOCK;
        int64_t rem  = timeout_ticks % ARKCHEMY_ESPRESSO_TIMER_CLOCK;
        long    ns   = (long)((rem * 1000000000LL) / ARKCHEMY_ESPRESSO_TIMER_CLOCK);
        /* A deadline further out than this is indistinguishable from waiting
         * forever, and keeps time_t away from its own overflow. */
        if (secs > 31536000LL) secs = 31536000LL;   /* one year */
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)secs;
        deadline.tv_nsec += ns;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec += 1; }
        uint64_t my_epoch = e->epoch;
        e->waiting_count++;
        while (!e->signaled && e->epoch == my_epoch) {
            if (pthread_cond_timedwait(&e->cond, &e->lock, &deadline) != 0) break; /* real timeout */
        }
        e->waiting_count--;
        if (e->signaled) {
            if (e->mode == 1) e->signaled = 0;
        } else if (e->epoch == my_epoch) {
            woke_signaled = 0; /* timed out, never woken */
        }
    }
    pthread_mutex_unlock(&e->lock);
    ctx->r[3] = (uint32_t)woke_signaled;
}

/* OSTime OSGetTime(void) / OSGetTick(void): both real, well-documented
 * CafeOS calls -- OSGetTime returns a 64-bit tick count since console
 * boot (split across r3(high)/r4(low), real PPC 32-bit ABI's 64-bit
 * return convention), OSGetTick a 32-bit one. No real console-boot
 * epoch exists here, so this uses the host's own monotonic clock instead
 * -- real magnitude doesn't match a real Wii U's, but it's monotonic and
 * always advancing, which is what real code measuring *elapsed* time
 * (the overwhelmingly common use) actually depends on. Documented
 * approximation, not a guess at real timing behavior. */
static inline uint64_t ppc_coreinit_host_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void ppc_import_coreinit_OSGetTime(PpcContext *ctx) {
    uint64_t t = ppc_coreinit_host_ticks();
    ctx->r[3] = (uint32_t)(t >> 32);
    ctx->r[4] = (uint32_t)t;
}

static inline void ppc_import_coreinit_OSGetTick(PpcContext *ctx) {
    ctx->r[3] = (uint32_t)ppc_coreinit_host_ticks();
}

/*
 * OSSemaphore -- real, genuine counting semaphore now, backed by the
 * same host-side-table-keyed-by-guest-address pattern as OSMutex/OSEvent
 * above (same reason: no guest-memory writes needed, but real
 * persistent state is). Real signatures confirmed against
 * coreinit/semaphore.h. Real *return-value* semantics for
 * OSSignalSemaphore/OSWaitSemaphore -- not documented in wut's header
 * comments -- confirmed directly against Cemu's actual HLE source
 * (src/Cafe/OS/libs/coreinit/coreinit_Synchronization.cpp): both return
 * the count *before* their respective increment/decrement, exactly
 * matching OSTryWaitSemaphore's own documented "returns previous count"
 * convention, so all three are now consistent (previously guessed at
 * with placeholder 0/1 values that didn't match this real convention).
 */
typedef struct {
    uint32_t guest_addr;
    int active;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int32_t count;
} ArkchemySemEntry;
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemySemEntry g_arkchemy_sems[ARKCHEMY_SYNC_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_sem_table_lock;

static inline ArkchemySemEntry *arkchemy_sem_get(uint32_t addr, int create_with_count) {
    int i, free_slot = -1;
    ArkchemySemEntry *result = NULL;
    pthread_mutex_lock(&g_arkchemy_sem_table_lock);
    for (i = 0; i < ARKCHEMY_SYNC_TABLE_SIZE; i++) {
        if (g_arkchemy_sems[i].active && g_arkchemy_sems[i].guest_addr == addr) {
            result = &g_arkchemy_sems[i];
            break;
        }
        if (free_slot < 0 && !g_arkchemy_sems[i].active) free_slot = i;
    }
    if (result == NULL && free_slot >= 0) {
        ArkchemySemEntry *s = &g_arkchemy_sems[free_slot];
        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->cond, NULL);
        s->guest_addr = addr;
        s->active = 1;
        s->count = create_with_count;
        result = s;
    }
    pthread_mutex_unlock(&g_arkchemy_sem_table_lock);
    return result;
}

static inline void ppc_import_coreinit_OSInitSemaphore(PpcContext *ctx) {
    /* void OSInitSemaphore(OSSemaphore*, int32_t count) */
    ArkchemySemEntry *s = arkchemy_sem_get(ctx->r[3], (int32_t)ctx->r[4]);
    pthread_mutex_lock(&s->lock);
    s->count = (int32_t)ctx->r[4]; /* re-Init on an already-used address resets it, matching real hardware */
    pthread_mutex_unlock(&s->lock);
}
static inline void ppc_import_coreinit_OSInitSemaphoreEx(PpcContext *ctx) { ppc_import_coreinit_OSInitSemaphore(ctx); }

static inline void ppc_import_coreinit_OSSignalSemaphore(PpcContext *ctx) {
    ArkchemySemEntry *s = arkchemy_sem_get(ctx->r[3], 0);
    int32_t prev;
    pthread_mutex_lock(&s->lock);
    prev = s->count;
    s->count = prev + 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    ctx->r[3] = (uint32_t)prev;
}

static inline void ppc_import_coreinit_OSWaitSemaphore(PpcContext *ctx) {
    ArkchemySemEntry *s = arkchemy_sem_get(ctx->r[3], 0);
    int32_t prev;
    pthread_mutex_lock(&s->lock);
    while (s->count <= 0) {
        pthread_cond_wait(&s->cond, &s->lock);
    }
    prev = s->count;
    s->count = prev - 1;
    pthread_mutex_unlock(&s->lock);
    ctx->r[3] = (uint32_t)prev;
}

static inline void ppc_import_coreinit_OSTryWaitSemaphore(PpcContext *ctx) {
    ArkchemySemEntry *s = arkchemy_sem_get(ctx->r[3], 0);
    int32_t prev;
    pthread_mutex_lock(&s->lock);
    prev = s->count;
    if (prev > 0) s->count = prev - 1;
    pthread_mutex_unlock(&s->lock);
    ctx->r[3] = (uint32_t)prev;
}

static inline void ppc_import_coreinit_OSGetSemaphoreCount(PpcContext *ctx) {
    ArkchemySemEntry *s = arkchemy_sem_get(ctx->r[3], 0);
    int32_t count;
    pthread_mutex_lock(&s->lock);
    count = s->count;
    pthread_mutex_unlock(&s->lock);
    ctx->r[3] = (uint32_t)count;
}

/*
 * OSGetSystemInfo/OSSleepTicks/OSTicksToCalendarTime: previously deferred
 * -- correctly converting OSTime ticks to wall-clock time needs the real
 * Wii U bus clock speed, and no reliably-documented value turned up when
 * first checked. Found since, sourced directly from Cemu's real
 * emulation core (src/Cafe/HW/Espresso/PPCState.h, not guessed):
 *   ESPRESSO_CORE_CLOCK  = 1243125000 Hz
 *   ESPRESSO_BUS_CLOCK   =  248625000 Hz
 *   ESPRESSO_TIMER_CLOCK = ESPRESSO_BUS_CLOCK / 4 = 62156250 Hz
 * -- the same busClockSpeed/4 formula this project's own docs already
 * had right, just missing the real busClockSpeed value until now. This
 * also matches wut's own OSTimerClockSpeed macro definition
 * (`(OSGetSystemInfo()->busClockSpeed) / 4`), confirming both sources
 * agree.
 *
 * OSGetSystemInfo(void) -> OSSystemInfo*: real struct (wut's
 * coreinit/systeminfo.h, WUT_CHECK_SIZE 0x20) is
 * {busClockSpeed, coreClockSpeed, baseTime, 0x10 unknown bytes}. Real
 * Cafe OS keeps one OS-owned static instance; this shim does the same,
 * writing it into a small fixed reserved slot (same documented
 * fixed-address-placeholder trade-off as cafeos_coreinit_mem.h's
 * MEM1/MEM2/errno reservations, placed right after errno's 4 bytes,
 * well clear of both). `baseTime` -- an internal real-time reference
 * point wut's own header doesn't further specify -- is honestly 0
 * (no real persisted wall-clock epoch exists in this runtime).
 *
 * OSSleepTicks(OSTime ticks): real behavior pauses the calling thread
 * for `ticks` timer-clock ticks. With exactly one PpcContext executing
 * sequentially and no other thread to run instead (same reasoning as
 * every other sync primitive in this file), a real host `nanosleep` for
 * the equivalent wall-clock duration -- computed from the now-real
 * ESPRESSO_TIMER_CLOCK, not guessed -- is genuinely correct behavior,
 * not a placeholder. `ticks` is a 64-bit `OSTime`, passed as a register
 * pair per PPC32 ABI (r3=high, r4=low).
 *
 * OSTicksToCalendarTime(OSTime time, OSCalendarTime *calendarTime): the
 * real Wii U `OSTime` epoch (2000-01-01T00:00:00 UTC) is a long-
 * established, independently-verifiable Wii/Wii U homebrew-scene fact
 * (WiiUBrew), not this session's guess. Converts to seconds via the
 * real timer clock, offsets to the host's Unix epoch (946684800s --
 * the fixed, independently-computable/verifiable difference between
 * 1970-01-01 and 2000-01-01, the same constant systems like NTP/GPS
 * epoch math use), then reuses the host's own real `gmtime_r` for the
 * actual calendar-field breakdown rather than hand-rolling leap-year
 * math -- `OSCalendarTime`'s fields (real offsets confirmed against
 * `coreinit/time.h`) line up directly with `struct tm`'s own fields
 * (`tm_mon` 0-11, `tm_wday`/`tm_yday` both 0-based, both already
 * matching), except `tm_year`, which `struct tm` gives as
 * years-since-1900 vs. `OSCalendarTime`'s real full-AD-year convention
 * (+1900 to convert).
 */
#define ARKCHEMY_OSTIME_EPOCH_OFFSET_SECONDS 946684800LL /* 2000-01-01 minus 1970-01-01 */
#define ARKCHEMY_OSSYSTEMINFO_ADDR 0xE008u /* right after cafeos_coreinit_mem.h's 4-byte errno slot at 0xE000 */

static inline void ppc_import_coreinit_OSGetSystemInfo(PpcContext *ctx) {
    ppc_store_u32(ctx, ARKCHEMY_OSSYSTEMINFO_ADDR + 0x00, (uint32_t)ARKCHEMY_ESPRESSO_BUS_CLOCK);
    ppc_store_u32(ctx, ARKCHEMY_OSSYSTEMINFO_ADDR + 0x04, (uint32_t)ARKCHEMY_ESPRESSO_CORE_CLOCK);
    ppc_store_u32(ctx, ARKCHEMY_OSSYSTEMINFO_ADDR + 0x08, 0); /* baseTime hi */
    ppc_store_u32(ctx, ARKCHEMY_OSSYSTEMINFO_ADDR + 0x0c, 0); /* baseTime lo */
    ctx->r[3] = ARKCHEMY_OSSYSTEMINFO_ADDR;
}

static inline void ppc_import_coreinit_OSSleepTicks(PpcContext *ctx) {
    /* OSSleepTicks(OSTime ticks) -- the 64-bit value is the FIRST argument,
     * so r3:r4 is correct here; EABI's odd-register rule is already
     * satisfied by r3. Only the scaling needed fixing. */
    int64_t ticks = (int64_t)((((uint64_t)ctx->r[3]) << 32) | (uint64_t)ctx->r[4]);
    if (ticks > 0) {
        /* Same overflow as OSWaitEventWithTimeout had: divide first. */
        int64_t secs = ticks / ARKCHEMY_ESPRESSO_TIMER_CLOCK;
        int64_t rem  = ticks % ARKCHEMY_ESPRESSO_TIMER_CLOCK;
        struct timespec ts;
        ts.tv_sec  = (time_t)secs;
        ts.tv_nsec = (long)((rem * 1000000000LL) / ARKCHEMY_ESPRESSO_TIMER_CLOCK);
        nanosleep(&ts, NULL);
    }
}

static inline void ppc_import_coreinit_OSTicksToCalendarTime(PpcContext *ctx) {
    int64_t ticks = ((int64_t)ctx->r[3] << 32) | (int64_t)ctx->r[4];
    uint32_t out_addr = ctx->r[5];
    int64_t total_seconds = ticks / ARKCHEMY_ESPRESSO_TIMER_CLOCK;
    int64_t remainder_ticks = ticks % ARKCHEMY_ESPRESSO_TIMER_CLOCK;
    if (remainder_ticks < 0) { remainder_ticks += ARKCHEMY_ESPRESSO_TIMER_CLOCK; total_seconds -= 1; }
    int64_t microseconds = (remainder_ticks * 1000000LL) / ARKCHEMY_ESPRESSO_TIMER_CLOCK;

    time_t unix_time = (time_t)(total_seconds + ARKCHEMY_OSTIME_EPOCH_OFFSET_SECONDS);
    struct tm tm_result;
    gmtime_r(&unix_time, &tm_result);

    ppc_store_u32(ctx, out_addr + 0x00, (uint32_t)tm_result.tm_sec);
    ppc_store_u32(ctx, out_addr + 0x04, (uint32_t)tm_result.tm_min);
    ppc_store_u32(ctx, out_addr + 0x08, (uint32_t)tm_result.tm_hour);
    ppc_store_u32(ctx, out_addr + 0x0c, (uint32_t)tm_result.tm_mday);
    ppc_store_u32(ctx, out_addr + 0x10, (uint32_t)tm_result.tm_mon);
    ppc_store_u32(ctx, out_addr + 0x14, (uint32_t)(tm_result.tm_year + 1900));
    ppc_store_u32(ctx, out_addr + 0x18, (uint32_t)tm_result.tm_wday);
    ppc_store_u32(ctx, out_addr + 0x1c, (uint32_t)tm_result.tm_yday);
    ppc_store_u32(ctx, out_addr + 0x20, (uint32_t)(microseconds / 1000));
    ppc_store_u32(ctx, out_addr + 0x24, (uint32_t)(microseconds % 1000));
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_SYNC_H */
