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
#include <errno.h>   /* ETIMEDOUT, for OSWaitEvent's pump slices */

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
 * and Cemu's real branching logic) are implemented with a mutex, a
 * condvar, a sticky `signaled` latch and a count of wake credits granted
 * to parked waiters -- see "EVCREDIT" above ppc_import_coreinit_OSInitEvent
 * for why a credit count and not an epoch, which is what this used until
 * 2026-09-24 and which released every AUTO waiter on a single signal.
 */

/* Was 64. An engine of this size uses far more than 64 OSMutexes --
 * igCafeMutex alone is constructed freely -- and the old value was only
 * ever "enough" because the boot had not got far enough to need more.
 * Raising it postpones exhaustion; the handling below is what stops
 * exhaustion being a null dereference. */
#define ARKCHEMY_SYNC_TABLE_SIZE 256

/* Sync-table diagnostics. Indices: 0 = mutex, 1 = event, 2 = semaphore.
 *
 * These tables never free an entry, so the claim count IS the high-water
 * mark. Exposed so a run can report how close it came to the limit rather
 * than discovering the limit by crashing. */
extern unsigned g_arkchemy_sync_used[3];
extern unsigned g_arkchemy_sync_exhausted[3];

/* Event signalling, for the open question of whether the job queue's
 * sleeping workers are ever woken: signals counts OSSignalEvent and
 * OSSignalEventAll, wakes counts waits that returned because the event was
 * signalled, and timeouts counts waits that gave up. A signal count that
 * climbs beside a flat wake count means the wrong event is being signalled;
 * both flat means nothing is producing work at all. */
extern unsigned g_arkchemy_event_signals;
extern unsigned g_arkchemy_event_wakes;
extern unsigned g_arkchemy_event_timeouts;
extern uint32_t g_arkchemy_event_last_signal;
extern uint32_t g_arkchemy_event_last_wait;

/* --- OSMutex (real, recursive) --- */
typedef struct {
    uint32_t guest_addr;
    int active;
    pthread_mutex_t mutex;
} ArkchemyMutexEntry;
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyMutexEntry g_arkchemy_mutexes[ARKCHEMY_SYNC_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_mutex_table_lock;
/* Aliasing fallback used only once the table is full -- see arkchemy_mutex_get. */
extern pthread_mutex_t g_arkchemy_mutex_fallback;

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
        g_arkchemy_sync_used[0]++;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_arkchemy_mutexes[free_slot].mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        g_arkchemy_mutexes[free_slot].guest_addr = addr;
        g_arkchemy_mutexes[free_slot].active = 1;
        result = &g_arkchemy_mutexes[free_slot].mutex;
    }
    if (result == NULL) {
        /* Table full. Every caller here dereferences the result immediately,
         * so returning NULL is a null dereference with nothing to point at
         * the cause. A shared recursive fallback is wrong -- unrelated
         * OSMutexes alias onto one lock -- but it is wrong *visibly*: the
         * exhausted counter is reported every frame. Prefer a diagnosable
         * wrong answer to an undiagnosable crash. */
        g_arkchemy_sync_exhausted[0]++;
        result = &g_arkchemy_mutex_fallback;
    }
    pthread_mutex_unlock(&g_arkchemy_mutex_table_lock);
    return result;
}

static inline void ppc_import_coreinit_OSInitMutex(PpcContext *ctx) { (void)arkchemy_mutex_get(ctx->r[3]); }
/* Pump deferred FS completions here. While the file scheduler is idle these
 * mutex calls are the hot path -- roughly 31 per frame -- so a completion
 * queued by a read is delivered within a frame, and always after the function
 * that issued the read has returned. cafeos_coreinit_fs.h is included before
 * this header, which is what makes the call legal. */
static inline void ppc_import_coreinit_OSLockMutex(PpcContext *ctx) {
    arkchemy_fs_pump_completions(ctx);
    pthread_mutex_lock(arkchemy_mutex_get(ctx->r[3]));
}
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
    uint64_t epoch;       /* diagnostic only: signals that reached a parked waiter */
    int waiting_count;
    /* Added at the end so the positional initializer of the fallback entry
     * in cafeos_state.c stays valid. See "EVCREDIT" below. */
    int wake_credits;     /* wakes granted to parked waiters, not yet taken */
    uint64_t init_epoch;  /* moves only in OSInitEvent */
} ArkchemyEventEntry;
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyEventEntry g_arkchemy_events[ARKCHEMY_SYNC_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_event_table_lock;
extern ArkchemyEventEntry g_arkchemy_event_fallback;

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
        g_arkchemy_sync_used[1]++;
        pthread_mutex_init(&e->lock, NULL);
        pthread_cond_init(&e->cond, NULL);
        e->guest_addr = addr;
        e->active = 1;
        e->signaled = create_with_value;
        e->mode = create_with_mode;
        e->epoch = 0;
        e->waiting_count = 0;
        e->wake_credits = 0;
        e->init_epoch = 0;
        result = e;
    }
    if (result == NULL) {                    /* see arkchemy_mutex_get */
        g_arkchemy_sync_exhausted[1]++;
        result = &g_arkchemy_event_fallback;
    }
    pthread_mutex_unlock(&g_arkchemy_event_table_lock);
    return result;
}

/* EVCREDIT -- how a signal reaches a waiter. Rewritten 2026-09-24.
 *
 * What Cafe OS does, from wut's event.h and Cemu's coreinit_Synchronization:
 *
 *   AUTO    OSSignalEvent wakes exactly one queued waiter. With nobody
 *           queued the event latches, and the next waiter consumes the latch.
 *           OSSignalEventAll wakes every queued waiter and latches nothing.
 *   MANUAL  a signal releases every waiter and stays set until OSResetEvent.
 *
 * What this shim did before: every parked waiter remembered one per-event
 * epoch, every signal bumped it, and the wait loop ran while
 * `epoch == my_epoch`. So on an AUTO event
 *
 *   - one signal released EVERY parked waiter, not one of them. EVSTATE
 *     measured two waiters parked on the loading event, so this was live;
 *   - a second signal arriving before the first woken waiter had run still
 *     saw waiting_count > 0, bumped the epoch again and latched nothing, so
 *     it was lost. The window is wide here, because a waiter drops the lock
 *     to run the pumps while staying counted -- deliberately, since the pump
 *     runs guest code that signals this very event.
 *
 * Both are ours, not the engine's, which is why this is a fix rather than a
 * workaround behind a flag. sync_harness.c pins all of it: three of its
 * AUTO cases fail on the old code.
 *
 * The scheme. `wake_credits` counts wakes granted to parked waiters and not
 * yet taken, and never exceeds `waiting_count`. An AUTO signal grants one
 * more credit if some waiter has not been granted one, and otherwise latches
 * `signaled` -- which is exactly "wake one queued waiter, or latch if the
 * queue is empty", with "queue" meaning waiters not already on their way
 * out. The credit lives in the struct, so a signal that lands while its
 * waiter is off running the pump is still there when it gets back. A waiter
 * leaves by taking a credit first, then the latch. Broadcast, not
 * pthread_cond_signal: the waiter a condvar signal would pick may be the one
 * outside the wait, running the pump, and the signal would be spent on
 * nobody. The others re-check, find no credit, and sleep again.
 *
 * One divergence, stated: real Cafe OS wakes the highest-priority queued
 * thread, and a thread that starts waiting after a signal can never take
 * that signal. Here any parked waiter can take any credit, including one
 * that arrived after the grant. The count is always right -- one signal, one
 * wake -- but not which thread gets it.
 *
 * `epoch` used to be both the release mechanism and what EVSTATE printed. It
 * is now only the second: signals that reached a parked waiter. OSInitEvent
 * has its own `init_epoch`, the one thing the wait predicate still compares,
 * so re-initialising a live event releases its waiters without any signal
 * doing so. */

/* An AUTO signal: grant a wake if a parked waiter has none, else latch.
 * Caller holds e->lock. */
static inline void arkchemy_event_signal_auto_locked(ArkchemyEventEntry *e) {
    if (e->wake_credits < e->waiting_count) {
        e->wake_credits++;
        e->epoch++;
        pthread_cond_broadcast(&e->cond);
    } else {
        e->signaled = 1;
    }
}

/* Is there anything for a waiter that started under `my_init` to leave on? */
static inline int arkchemy_event_ready_locked(const ArkchemyEventEntry *e, uint64_t my_init) {
    return e->wake_credits > 0 || e->signaled || e->init_epoch != my_init;
}

/* A waiter leaving. Takes what it is leaving on, in order: a re-init takes
 * nothing, then a credit, then the latch (which AUTO consumes and MANUAL
 * keeps). Returns 1 if it was signalled, 0 if it left for any other reason
 * -- a timeout, EVWAKE, or a re-init. Caller holds e->lock.
 *
 * The clamp at the end keeps wake_credits <= waiting_count. It can only
 * trip when a waiter leaves without a credit while credits are outstanding,
 * which is the re-init path; the excess are signals meant for a queue that
 * no longer has anyone in it, and on AUTO that is a latch. */
static inline int arkchemy_event_leave_locked(ArkchemyEventEntry *e, uint64_t my_init) {
    int signalled = 0;
    if (e->init_epoch == my_init) {
        if (e->wake_credits > 0) {
            e->wake_credits--;
            signalled = 1;
        } else if (e->signaled) {
            if (e->mode == 1 /* AUTO */) e->signaled = 0;
            signalled = 1;
        }
    }
    e->waiting_count--;
    if (e->wake_credits > e->waiting_count) {
        e->wake_credits = e->waiting_count;
        if (e->mode == 1) e->signaled = 1;
    }
    return signalled;
}

static inline void ppc_import_coreinit_OSInitEvent(PpcContext *ctx) {
    /* void OSInitEvent(OSEvent *event, BOOL value, OSEventMode mode) */
    uint32_t addr = ctx->r[3];
    int value = (int)(ctx->r[4] != 0);
    int mode = (int)ctx->r[5];
    ArkchemyEventEntry *e = arkchemy_event_get(addr, value, mode);
    /* Re-initializing an address already in this table (a real Init
     * called twice) resets its state -- matches real hardware, which
     * always fully re-initializes the struct. Anyone parked on the old
     * incarnation is released: init_epoch moving is what they wait for. */
    pthread_mutex_lock(&e->lock);
    e->signaled = value;
    e->mode = mode;
    e->wake_credits = 0;
    e->init_epoch++;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSSignalEvent(PpcContext *ctx) {
    g_ark_sev_enter++;
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1 /* AUTO default if never Init'd */);
    g_ark_sev_got_entry++;
    g_arkchemy_event_signals++; g_arkchemy_event_last_signal = ctx->r[3];
    pthread_mutex_lock(&e->lock);
    g_ark_sev_got_lock++;
    if (e->mode == 1 /* AUTO */) {
        arkchemy_event_signal_auto_locked(e);
    } else if (!e->signaled) { /* MANUAL */
        e->signaled = 1;
        if (e->waiting_count) e->epoch++;
        pthread_cond_broadcast(&e->cond);
    }
    pthread_mutex_unlock(&e->lock);
    g_ark_sev_exit++;
}

static inline void ppc_import_coreinit_OSSignalEventAll(PpcContext *ctx) {
    g_arkchemy_event_signals++; g_arkchemy_event_last_signal = ctx->r[3];
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    pthread_mutex_lock(&e->lock);
    if (e->mode == 1 /* AUTO */) {
        if (e->waiting_count == 0) {
            e->signaled = 1;
        } else {
            /* everyone currently parked gets a wake; nothing persists */
            e->wake_credits = e->waiting_count;
            e->epoch++;
            pthread_cond_broadcast(&e->cond);
        }
    } else if (!e->signaled) { /* MANUAL */
        e->signaled = 1;
        if (e->waiting_count) e->epoch++;
        pthread_cond_broadcast(&e->cond);
    }
    pthread_mutex_unlock(&e->lock);
}

static inline void ppc_import_coreinit_OSResetEvent(PpcContext *ctx) {
    /* Clears the latch only. Credits already granted are wakes that have
     * happened -- on Cafe OS those threads are off the queue and running --
     * so a reset does not take them back. */
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    pthread_mutex_lock(&e->lock);
    e->signaled = 0;
    pthread_mutex_unlock(&e->lock);
}

/* How long OSWaitEvent sleeps between pumps. Short enough that a cooperative
 * completion is not the bottleneck, long enough not to spin: at 1ms a load
 * that needs a few hundred block reads costs well under a second of slicing,
 * against the 39 seconds per pump measured before this existed. */
#define ARKCHEMY_EVENT_PUMP_SLICE_NS 1000000L

/* EVWAKE: bounded spurious wakeup out of OSWaitEvent.
 *
 * Measured 2026-09-20 on the event at 0x452d054, which the game thread is
 * parked on: signaled=0, waiting=1, epoch=1 after 9 OSSignalEvent calls, and
 * 76,144 wait slices burned. epoch only advances when a signal lands while a
 * waiter is counted, so epoch=1 says exactly one signal ever did -- and none
 * at all since this thread parked. Meanwhile IDSITE shows the work item's
 * status byte was set to 2 by readFromBuffer at 0x2175818, so the condition
 * the thread wants has been true the whole time. Nothing raises the signal
 * for that transition and the sleeper never learns.
 *
 * Returning spuriously is safe for this caller and that is why it is done
 * here rather than by inventing a raise. igPhysicalStorageDevice::update
 * re-reads the status immediately after the wait returns
 * (0x21755e8: lbz r0, 0x23(r31); cmpwi r0, 1; beq back to the wait), so a
 * wake with nothing to show for it costs one re-check and nothing else. That
 * is the ordinary condition-variable contract and this guest code honours it.
 *
 * Bounded, not free-running: only after this many 1ms slices, so a waiter
 * that genuinely has nothing to do still parks instead of spinning.
 *
 * The risk, stated plainly: real OSWaitEvent does not return spuriously, so a
 * caller that does not re-check its predicate would see this as a wait that
 * ended early. Every caller reached so far does re-check, but that is an
 * observation about the paths this boot takes, not a guarantee about the
 * engine. Hence the flag and the counter -- if something downstream starts
 * behaving oddly, set the count to 0 and it reverts to the old behaviour.
 *
 * DEFAULT 0 SINCE 2026-09-20. It rested on a misreading. ITEMDONE reported
 * seen=1 for the waited item and that was taken as meaning its status was
 * already satisfied; seen only ever meant the item had been written at
 * some point. The item had completed once and been reused for a fresh
 * read, and SPINWAIT2 read status=1 throughout -- so the waiter was
 * waiting correctly and there was no missed wakeup to rescue.
 *
 * Measured with it on: evwake fired 1189 times, parked went 2 -> 1191 and
 * spin iterations 8 -> 1197, while bytes read, startBlockRead and the six
 * held blocks did not move at all. Mechanically sound, no effect. Kept in
 * the tree with its reasoning rather than deleted, because the argument
 * would hold if a real missed wakeup ever turns up -- but off, because
 * diverging from real OS semantics is not worth carrying without a
 * benefit to point at. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_evwake_slices = 0u, g_ark_evwake_fired = 0;

static inline void ppc_import_coreinit_OSWaitEvent(PpcContext *ctx) {
    /* Deliver queued FS completions before parking, for the same liveness
     * reason as OSWaitEventWithTimeout below -- and here it is not a
     * fallback, it is the only path. Found 2026-09-09, the first run in
     * which igArkCore::init actually loaded the registry:
     *
     *   igPhysicalStorageDevice::update issues FSReadFileWithPosAsync
     *     -> the shim reads the file and QUEUES the completion
     *   igCafeSignal::wait  ->  b OSWaitEvent
     *
     * At that point no guest thread has been created yet (threads:
     * created=0), so there is no worker parked in the timeout variant to
     * run the pump, and no other import is ever reached. The completion sat
     * in the queue (q=1 done=0 pend=1) with the file's 542 bytes already in
     * the guest buffer, and the one thread in existence blocked forever
     * waiting for a signal only that completion could raise.
     *
     * Pumping BEFORE arkchemy_event_get is deliberate: the callback runs
     * guest code that signals this very event, and e->lock is not
     * recursive. */
    g_ark_wev_enter++;
    arkchemy_fs_pump_completions(ctx);
    ArkchemyEventEntry *e = arkchemy_event_get(ctx->r[3], 0, 1);
    g_ark_wev_got_entry++;
    pthread_mutex_lock(&e->lock);
    g_ark_wev_got_lock++;
    if (e->signaled) {
        if (e->mode == 1 /* AUTO */) e->signaled = 0;
    } else {
        /* Wait in short slices and pump deferred work between them, instead
         * of parking on the condvar forever.
         *
         * The contract is unchanged -- this still does not return until the
         * event is signalled. What changes is that a thread waiting here can
         * still run the work that will cause the signal.
         *
         * 2026-09-12, measured. The game thread reaches
         * igIGZLoader::update -> igFileContext -> igPhysicalStorageDevice,
         * issues a block read, delivers its completion from the pump, raises
         * the signal (igCafeSignal::raise -> OSSignalEvent, the last
         * recompiled function the game thread ever enters -- identical
         * GAMEPC count of 2,877,916 across two different builds, so a
         * deterministic stop), and then waits here for the NEXT completion.
         *
         * But the next block read is only issued by
         * igArchive::updateArchiveSystem, and igArchive::update is its only
         * caller, and that is reached only by this same thread continuing
         * round its loop. So it waits for work that only it can create.
         *
         * It was not a hard deadlock, which is what made it confusing:
         * FMOD's threads call imports constantly and some of those pump,
         * so the archive did creep forward -- 23 updateArchiveSystem calls
         * and 19 decompressed blocks in 900 seconds, and the same 19 in the
         * 420-second run before it. Progress at roughly one pump every 39
         * seconds reads exactly like a hang.
         *
         * Real Cafe OS has no such problem: FS completions arrive on the
         * OS's own I/O thread, so a parked game thread is woken from
         * outside. Here the pump is cooperative, so the wait has to be too.
         *
         * The slice is deliberately short and the loop re-checks the same
         * predicate, so a spurious wake or a missed signal cannot make this
         * return early -- it only costs a wakeup. */
        uint64_t my_init = e->init_epoch;
        uint32_t my_slices = 0;
        e->waiting_count++;
        g_ark_wev_parked++;
        while (!arkchemy_event_ready_locked(e, my_init)) {
            struct timespec deadline;
            g_ark_wev_slices++;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += ARKCHEMY_EVENT_PUMP_SLICE_NS;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&e->cond, &e->lock, &deadline) == ETIMEDOUT) {
                /* Drop the lock before pumping: a completion callback runs
                 * guest code that can signal this very event, and e->lock is
                 * not recursive. Leave the waiter counted across the gap so
                 * an AUTO signal arriving meanwhile is granted to it as a
                 * credit, which waits in the struct until it is back. */
                pthread_mutex_unlock(&e->lock);
                arkchemy_fs_pump_completions(ctx);
                /* Nothing queued means nothing will arrive on its own: the
                 * read this thread is waiting for has not been issued, and
                 * only the archive pump issues it. See arkchemy_archive_pump
                 * for why driving it from here is safe. */
                arkchemy_archive_pump(ctx);
                pthread_mutex_lock(&e->lock);
                /* See EVWAKE: the predicate can become true without anyone
                 * signalling, so give the caller a chance to re-check. */
                if (g_ark_evwake_slices
                    && ++my_slices >= g_ark_evwake_slices) {
                    g_ark_evwake_fired++;
                    break;
                }
            }
        }
        arkchemy_event_leave_locked(e, my_init);
    }
    pthread_mutex_unlock(&e->lock);
    g_ark_wev_exit++;
}

static inline void ppc_import_coreinit_OSWaitEventWithTimeout(PpcContext *ctx) {
    /* BOOL OSWaitEventWithTimeout(OSEvent *event, OSTime timeout) --
     * timeout is a real relative duration in timer ticks (confirmed via
     * Cemu's real HLE: ConvertNsToTimerTicks), not an absolute deadline.
     * Converted to real wall-clock time via the same confirmed
     * ARKCHEMY_ESPRESSO_TIMER_CLOCK this file's OSSleepTicks already
     * uses. Returns real TRUE if actually signaled, FALSE on a real
     * timeout -- not the old "always true, never times out" stand-in. */
    /* Deliver any queued FS completion BEFORE parking. This is the path that
     * still runs when every other thread is blocked -- jqWorkerSleep's two
     * workers keep calling this even with the game thread stopped dead. The
     *2026-09-01 mutex-only pump deadlocked exactly here: the completion was
     * queued, the main thread blocked waiting for it, the workers parked, and
     * nothing ever took a mutex again to run the pump. Delivering from a
     * parked worker is also nearer what Cafe OS does than delivering from
     * whoever next happens to lock something. */
    arkchemy_fs_pump_completions(ctx);
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
    g_arkchemy_event_last_wait = ctx->r[3];
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
        uint64_t my_init = e->init_epoch;
        e->waiting_count++;
        while (!arkchemy_event_ready_locked(e, my_init)) {
            if (pthread_cond_timedwait(&e->cond, &e->lock, &deadline) != 0) break; /* real timeout */
        }
        /* TRUE only for a credit or the latch. The old test here was
         * `else if (epoch == my_epoch) FALSE`, which reported TRUE to a
         * waiter whose epoch had moved because some OTHER waiter was
         * signalled -- a timed-out worker told it had work. A credit is
         * checked before the timeout is believed, so a grant that lands at
         * the same instant as the deadline is still delivered. */
        woke_signaled = arkchemy_event_leave_locked(e, my_init);
    }
    pthread_mutex_unlock(&e->lock);
    if (woke_signaled) g_arkchemy_event_wakes++; else g_arkchemy_event_timeouts++;
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
extern ArkchemySemEntry g_arkchemy_sem_fallback;

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
        g_arkchemy_sync_used[2]++;
        result = s;
    }
    if (result == NULL) {                    /* see arkchemy_mutex_get */
        g_arkchemy_sync_exhausted[2]++;
        result = &g_arkchemy_sem_fallback;
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
    arkchemy_fs_pump_completions(ctx);   /* liveness: see OSWaitEventWithTimeout */
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
    arkchemy_fs_pump_completions(ctx);   /* liveness: see OSWaitEventWithTimeout */
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
