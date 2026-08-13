#ifndef PORTALRECOMP_PPC_RUNTIME_H
#define PORTALRECOMP_PPC_RUNTIME_H

#include <stdint.h>
#include <string.h>

/*
 * Minimal PowerPC execution context used by recompiler-generated C code.
 *
 * `mem` stands in for addressable memory (stack, in this milestone). Register
 * r1 (the stack pointer) is treated as a plain offset into `mem`, not a real
 * pointer, since this PoC harness has no other memory regions to distinguish.
 *
 * CR0 is tracked as three flag bits (lt/gt/eq). Other CR fields are not
 * modeled -- fine for this milestone's instruction subset, but any
 * instruction that targets a non-zero crf would silently be treated as cr0.
 *
 * `lr` exists only so mflr/mtlr save/restore sequences around nested calls
 * compile; `bl` is translated as a direct C call (see codegen.cpp) rather
 * than true branch-and-link, so lr's value is never actually read to decide
 * where control returns.
 */
typedef struct PpcContext {
    uint32_t r[32];
    uint32_t lr;
    uint8_t cr0_lt;
    uint8_t cr0_gt;
    uint8_t cr0_eq;
    uint8_t mem[65536];
} PpcContext;

static inline uint32_t ppc_load_u32(const PpcContext *ctx, uint32_t addr) {
    uint32_t v;
    memcpy(&v, &ctx->mem[addr & (sizeof(ctx->mem) - 1)], sizeof(v));
    return v;
}

static inline void ppc_store_u32(PpcContext *ctx, uint32_t addr, uint32_t val) {
    memcpy(&ctx->mem[addr & (sizeof(ctx->mem) - 1)], &val, sizeof(val));
}

static inline void ppc_cmpw(PpcContext *ctx, int32_t a, int32_t b) {
    ctx->cr0_lt = a < b;
    ctx->cr0_gt = a > b;
    ctx->cr0_eq = a == b;
}

#endif /* PORTALRECOMP_PPC_RUNTIME_H */
