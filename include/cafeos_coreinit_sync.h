#ifndef BRAMBLE_CAFEOS_COREINIT_SYNC_H
#define BRAMBLE_CAFEOS_COREINIT_SYNC_H

/* clock_gettime/nanosleep/gmtime_r are POSIX, not ISO C -- needed under
 * strict -std=c11 (glibc exposes them by default under looser standards
 * modes, which is why this went unnoticed until compiled strictly). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

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
enum {
    BRAMBLE_ESPRESSO_CORE_CLOCK = 1243125000,
    BRAMBLE_ESPRESSO_BUS_CLOCK = 248625000,
    BRAMBLE_ESPRESSO_TIMER_CLOCK = BRAMBLE_ESPRESSO_BUS_CLOCK / 4,
};
#define BRAMBLE_OSTIME_EPOCH_OFFSET_SECONDS 946684800LL /* 2000-01-01 minus 1970-01-01 */
#define BRAMBLE_OSSYSTEMINFO_ADDR 0xE008u /* right after cafeos_coreinit_mem.h's 4-byte errno slot at 0xE000 */

static inline void ppc_import_coreinit_OSGetSystemInfo(PpcContext *ctx) {
    ppc_store_u32(ctx, BRAMBLE_OSSYSTEMINFO_ADDR + 0x00, (uint32_t)BRAMBLE_ESPRESSO_BUS_CLOCK);
    ppc_store_u32(ctx, BRAMBLE_OSSYSTEMINFO_ADDR + 0x04, (uint32_t)BRAMBLE_ESPRESSO_CORE_CLOCK);
    ppc_store_u32(ctx, BRAMBLE_OSSYSTEMINFO_ADDR + 0x08, 0); /* baseTime hi */
    ppc_store_u32(ctx, BRAMBLE_OSSYSTEMINFO_ADDR + 0x0c, 0); /* baseTime lo */
    ctx->r[3] = BRAMBLE_OSSYSTEMINFO_ADDR;
}

static inline void ppc_import_coreinit_OSSleepTicks(PpcContext *ctx) {
    int64_t ticks = ((int64_t)ctx->r[3] << 32) | (int64_t)ctx->r[4];
    if (ticks > 0) {
        int64_t ns = (ticks * 1000000000LL) / BRAMBLE_ESPRESSO_TIMER_CLOCK;
        struct timespec ts;
        ts.tv_sec = (time_t)(ns / 1000000000LL);
        ts.tv_nsec = (long)(ns % 1000000000LL);
        nanosleep(&ts, NULL);
    }
}

static inline void ppc_import_coreinit_OSTicksToCalendarTime(PpcContext *ctx) {
    int64_t ticks = ((int64_t)ctx->r[3] << 32) | (int64_t)ctx->r[4];
    uint32_t out_addr = ctx->r[5];
    int64_t total_seconds = ticks / BRAMBLE_ESPRESSO_TIMER_CLOCK;
    int64_t remainder_ticks = ticks % BRAMBLE_ESPRESSO_TIMER_CLOCK;
    if (remainder_ticks < 0) { remainder_ticks += BRAMBLE_ESPRESSO_TIMER_CLOCK; total_seconds -= 1; }
    int64_t microseconds = (remainder_ticks * 1000000LL) / BRAMBLE_ESPRESSO_TIMER_CLOCK;

    time_t unix_time = (time_t)(total_seconds + BRAMBLE_OSTIME_EPOCH_OFFSET_SECONDS);
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

#endif /* BRAMBLE_CAFEOS_COREINIT_SYNC_H */
