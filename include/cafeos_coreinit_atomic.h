#ifndef ARKCHEMY_CAFEOS_COREINIT_ATOMIC_H
#define ARKCHEMY_CAFEOS_COREINIT_ATOMIC_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit 64-bit atomics.
 *
 * Real signatures verified against devkitPro/wut's coreinit/atomic64.h:
 *   int64_t  OSAddAtomic64(volatile int64_t *ptr, int64_t value);
 *   uint64_t OSAndAtomic64(volatile uint64_t *ptr, uint64_t value);
 *   uint64_t OSOrAtomic64(volatile uint64_t *ptr, uint64_t value);
 *   BOOL     OSCompareAndSwapAtomic64(volatile uint64_t *ptr, uint64_t compare, uint64_t value);
 *
 * "Atomic" only matters when something else could run between the read
 * and the write -- this runtime has exactly one PpcContext executing
 * sequentially (no threads), so a plain read-modify-write is genuinely
 * atomic here, not just a shortcut standing in for one.
 *
 * OSCompareAndSwapAtomic64's semantics are unambiguous (the same CAS
 * definition every architecture/OS uses: swap and return true only if
 * *ptr == compare, otherwise leave it alone and return false) -- high
 * confidence. Whether OSAddAtomic64/OSAndAtomic64/OSOrAtomic64 return the
 * *old* or *new* value isn't independently confirmed here (real APIs
 * disagree with each other on this by name alone); implemented returning
 * the *new* value, matching the more common convention across similar
 * real-world atomic APIs, but flagged as the lower-confidence half of
 * this file.
 *
 * PPC EABI note, corrected 2026-08-30 after a real hardware hang. A 64-bit
 * argument occupies a register PAIR whose first register is ODD-numbered:
 * (r3,r4), (r5,r6), (r7,r8), (r9,r10). With a pointer in r3, the first
 * 64-bit argument therefore SKIPS r4 and lands in r5:r6, and the second in
 * r7:r8. This comment previously asserted r4:r5, and every function below
 * was written to match it, so each one read one register short.
 *
 * The retail code says so plainly at the call site (igJobQueue::init,
 * 0x21d9e44):
 *
 *     21d9e5c: lwz  r6, 4(r10)     low word of the current value
 *     21d9e60: lwz  r8, 0(r10)     high word
 *     21d9e68: mr   r5, r8         so r5:r6 is `compare`
 *     21d9e3c: andc r8, r6, r10    and r7:r8 is `value`
 *     21d9e44: bl   OSCompareAndSwapAtomic64
 *
 * Reading r4:r5 made the comparand garbage, so the CAS could never succeed
 * and the lock-free retry loop around it spun forever. On hardware that was
 * a hard hang with the recompiled-call counter frozen -- invisible, because
 * a shim increments no counter -- at 1.39 million import calls per second.
 *
 * A 64-bit RETURN value is split across r3(high):r4(low), which was correct
 * and is unchanged.
 */
static inline void ppc_import_coreinit_OSAddAtomic64(PpcContext *ctx) {
    uint32_t addr = ctx->r[3];
    int64_t value = (int64_t)(((uint64_t)ctx->r[5] << 32) | ctx->r[6]);
    int64_t cur = (int64_t)ppc_load_u64(ctx, addr);
    int64_t result = cur + value;
    ppc_store_u64(ctx, addr, (uint64_t)result);
    ctx->r[3] = (uint32_t)((uint64_t)result >> 32);
    ctx->r[4] = (uint32_t)(uint64_t)result;
}

static inline void ppc_import_coreinit_OSAndAtomic64(PpcContext *ctx) {
    uint32_t addr = ctx->r[3];
    uint64_t value = ((uint64_t)ctx->r[5] << 32) | ctx->r[6];
    uint64_t result = ppc_load_u64(ctx, addr) & value;
    ppc_store_u64(ctx, addr, result);
    ctx->r[3] = (uint32_t)(result >> 32);
    ctx->r[4] = (uint32_t)result;
}

static inline void ppc_import_coreinit_OSOrAtomic64(PpcContext *ctx) {
    uint32_t addr = ctx->r[3];
    uint64_t value = ((uint64_t)ctx->r[5] << 32) | ctx->r[6];
    uint64_t result = ppc_load_u64(ctx, addr) | value;
    ppc_store_u64(ctx, addr, result);
    ctx->r[3] = (uint32_t)(result >> 32);
    ctx->r[4] = (uint32_t)result;
}

/* BOOL OSCompareAndSwapAtomic64(volatile uint64_t *ptr, uint64_t compare, uint64_t value);
 * r3=ptr r4:r5=compare(hi:lo) r6:r7=value(hi:lo) -- 3 64-bit-or-pointer
 * args after the first spill across register pairs per the real PPC
 * ABI: ptr(r3), compare(r4:r5), value(r6:r7). */
static inline void ppc_import_coreinit_OSCompareAndSwapAtomic64(PpcContext *ctx) {
    uint32_t addr = ctx->r[3];
    uint64_t compare = ((uint64_t)ctx->r[5] << 32) | ctx->r[6];
    uint64_t value = ((uint64_t)ctx->r[7] << 32) | ctx->r[8];
    uint64_t cur = ppc_load_u64(ctx, addr);
    if (cur == compare) {
        ppc_store_u64(ctx, addr, value);
        ctx->r[3] = 1;
    } else {
        ctx->r[3] = 0;
    }
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_ATOMIC_H */
