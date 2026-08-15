#ifndef BRAMBLE_CAFEOS_COREINIT_SYNC_H
#define BRAMBLE_CAFEOS_COREINIT_SYNC_H

#include <time.h>

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit synchronization primitives
 * (OSMutex/OSEvent) and timing.
 *
 * OSMutex/OSEvent are opaque structs the *game* allocates (in its own
 * .bss/.data or on its own stack), the same way FSClient/FSCmdBlock work
 * in cafeos_coreinit_fs.h -- this shim never needs to allocate or write
 * through them, so there's no guest-memory-address problem here the way
 * there is for __gh_errno_ptr.
 *
 * Every OSInitMutex/OSLockMutex/OSUnlockMutex/OSInitEvent/OSSignalEvent/
 * OSResetEvent/OSWaitEvent below is a genuine no-op, not a shortcut: this
 * runtime has exactly one PpcContext executing sequentially with no real
 * concurrent execution at all (OSCreateThread isn't implemented -- there
 * is no second thread that could ever contend for a lock or need to be
 * woken by an event). A no-op mutex is trivially correct with nothing to
 * exclude; a no-op "wait" that returns immediately is *more* correct
 * than actually blocking would be, since actually blocking would
 * deadlock forever waiting for a signal from a thread that will never
 * run. OSTryLockMutex returns true (lock always "acquired") for the same
 * reason.
 *
 * Real thread creation (OSCreateThread and friends) is a separate, much
 * larger problem -- genuine concurrent (or at least interleaved)
 * execution of more than one recompiled function -- not attempted here.
 */
static inline void ppc_import_coreinit_OSInitMutex(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSLockMutex(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSUnlockMutex(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSTryLockMutex(PpcContext *ctx) { ctx->r[3] = 1; /* BOOL true: always "acquired" */ }

static inline void ppc_import_coreinit_OSInitEvent(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSSignalEvent(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSSignalEventAll(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSResetEvent(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSWaitEvent(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSWaitEventWithTimeout(PpcContext *ctx) { ctx->r[3] = 1; /* BOOL true: "signaled" (never actually times out) */ }

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
 * OSSemaphore -- real signature confirmed against devkitPro/wut's
 * coreinit/semaphore.h: `void OSInitSemaphore(OSSemaphore*, int32_t
 * count)`, `int32_t OSSignalSemaphore(OSSemaphore*)`, `int32_t
 * OSWaitSemaphore(OSSemaphore*)`, `int32_t OSTryWaitSemaphore(OSSemaphore*)`
 * -- same "caller-allocated opaque struct" shape as OSMutex/OSEvent above,
 * and the same single-PpcContext reasoning applies: with no second thread
 * ever able to contend for it, a semaphore can't meaningfully block or
 * need signaling. OSWaitSemaphore/OSTryWaitSemaphore both report success
 * (the wait is immediately satisfied) rather than tracking a real count
 * in guest memory -- consistent with OSTryLockMutex above, and for the
 * same reason: this shim never allocates or writes through game-owned
 * structs (see the __gh_errno_ptr/MEM* note in cafeos_coreinit.h for why
 * that's a real, deliberate boundary, not an oversight). Real code that
 * depends on an exact post-signal/wait *count* (rather than just
 * treating the semaphore as a binary gate) isn't modeled correctly here
 * -- a known limitation of this whole no-op-synchronization approach, not
 * specific to semaphores.
 */
static inline void ppc_import_coreinit_OSInitSemaphore(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSInitSemaphoreEx(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSSignalSemaphore(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_coreinit_OSWaitSemaphore(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_coreinit_OSTryWaitSemaphore(PpcContext *ctx) { ctx->r[3] = 1; /* BOOL/int32_t true: always immediately available */ }
static inline void ppc_import_coreinit_OSGetSemaphoreCount(PpcContext *ctx) { ctx->r[3] = 0; }

#endif /* BRAMBLE_CAFEOS_COREINIT_SYNC_H */
