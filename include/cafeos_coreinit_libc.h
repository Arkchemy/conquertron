#ifndef BRAMBLE_CAFEOS_COREINIT_LIBC_H
#define BRAMBLE_CAFEOS_COREINIT_LIBC_H

#include <string.h>

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- plain C library functions Cafe OS
 * itself re-exports for game code to call (real, standard semantics, not
 * guessed -- these are exactly libc's memcpy/memset, PPC ABI aside).
 *
 * void *memcpy(void *dst, const void *src, size_t n);  r3=dst r4=src r5=n
 * void *memset(void *dst, int c, size_t n);             r3=dst r4=c   r5=n
 * Both return their first argument (`dst`) unchanged -- since this shim
 * never writes r3 before returning, it's already correct: r3 already
 * holds `dst` on entry (PPC ABI's first-arg register), so leaving it
 * alone *is* returning it.
 *
 * Operates directly on ctx->shared->mem (masked exactly like every
 * other ppc_load/store helper in ppc_runtime.h) rather than looping one
 * byte at a time through ppc_load_u8/ppc_store_u8 -- both addresses are
 * already real offsets into the same flat guest memory array, so a
 * direct host memmove/memset over that sub-range is both correct and
 * far faster for anything copying more than a few bytes (texture/audio-
 * sized copies are exactly the case this matters for). Doesn't attempt
 * to handle a copy/fill that would run past the end of PPC_MEM_SIZE by
 * wrapping -- that's the same known, separate memory-model-scaling gap
 * already noted in cafeos_coreinit_fs.h, not a new one.
 */
static inline void ppc_import_coreinit_memcpy(PpcContext *ctx) {
    uint32_t dst = ctx->r[3] & (uint32_t)(PPC_MEM_SIZE - 1);
    uint32_t src = ctx->r[4] & (uint32_t)(PPC_MEM_SIZE - 1);
    uint32_t n = ctx->r[5];
    if ((uint64_t)dst + n <= PPC_MEM_SIZE && (uint64_t)src + n <= PPC_MEM_SIZE) {
        memmove(&ctx->shared->mem[dst], &ctx->shared->mem[src], n);
    }
}

static inline void ppc_import_coreinit_memset(PpcContext *ctx) {
    uint32_t dst = ctx->r[3] & (uint32_t)(PPC_MEM_SIZE - 1);
    int c = (int)ctx->r[4];
    uint32_t n = ctx->r[5];
    if ((uint64_t)dst + n <= PPC_MEM_SIZE) {
        memset(&ctx->shared->mem[dst], c, n);
    }
}

#endif /* BRAMBLE_CAFEOS_COREINIT_LIBC_H */
