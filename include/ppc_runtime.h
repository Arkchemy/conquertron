#ifndef BRAMBLE_PPC_RUNTIME_H
#define BRAMBLE_PPC_RUNTIME_H

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
 *
 * `f` holds the 32 FPRs as `double`, matching real PowerPC hardware (FPRs
 * are always 64-bit; single-precision ops compute a double result then
 * round it to float precision before it's "stored" in the register --
 * see ppc_frsp). This is not full IEEE-754 fidelity (no exception flags,
 * no explicit rounding-mode control), which is a known gap before this
 * generalizes to real Wii U floating-point code.
 */
typedef struct PpcContext {
    uint32_t r[32];
    double f[32];
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

static inline uint8_t ppc_load_u8(const PpcContext *ctx, uint32_t addr) {
    return ctx->mem[addr & (sizeof(ctx->mem) - 1)];
}

static inline void ppc_store_u8(PpcContext *ctx, uint32_t addr, uint8_t val) {
    ctx->mem[addr & (sizeof(ctx->mem) - 1)] = val;
}

static inline uint16_t ppc_load_u16(const PpcContext *ctx, uint32_t addr) {
    uint16_t v;
    memcpy(&v, &ctx->mem[addr & (sizeof(ctx->mem) - 1)], sizeof(v));
    return v;
}

static inline void ppc_store_u16(PpcContext *ctx, uint32_t addr, uint16_t val) {
    memcpy(&ctx->mem[addr & (sizeof(ctx->mem) - 1)], &val, sizeof(val));
}

static inline uint32_t ppc_rotl32(uint32_t v, unsigned int sh) {
    sh &= 31;
    return sh == 0 ? v : (v << sh) | (v >> (32 - sh));
}

static inline void ppc_cmpw(PpcContext *ctx, int32_t a, int32_t b) {
    ctx->cr0_lt = a < b;
    ctx->cr0_gt = a > b;
    ctx->cr0_eq = a == b;
}

static inline float ppc_load_f32(const PpcContext *ctx, uint32_t addr) {
    float v;
    memcpy(&v, &ctx->mem[addr & (sizeof(ctx->mem) - 1)], sizeof(v));
    return v;
}

static inline void ppc_store_f32(PpcContext *ctx, uint32_t addr, double val) {
    float v = (float)val;  /* narrow: stfs always stores the single-precision rounding */
    memcpy(&ctx->mem[addr & (sizeof(ctx->mem) - 1)], &v, sizeof(v));
}

/* Round-to-single-precision, matching PPC's single-precision FP ops
 * (fadds/fsubs/fmuls/fdivs/fmadds/...), which compute as double but store
 * a single-rounded result back into the (still 64-bit) FPR. */
static inline double ppc_frsp(double val) { return (double)(float)val; }

#endif /* BRAMBLE_PPC_RUNTIME_H */
