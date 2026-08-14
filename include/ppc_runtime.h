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
 *
 * `mem` is genuinely big-endian, matching real PPC memory layout byte for
 * byte (not just "self-consistent under our own load/store pair," which a
 * host-native memcpy would have given us for free but doesn't hold up once
 * compiled code depends on real byte layout directly -- e.g. fctiwz stores
 * its 32-bit result via stfd as if it were a double, then a real compiler
 * reads it back with a plain 32-bit load at the low word's *fixed* byte
 * offset (+4 into the 8-byte value, because PPC is big-endian). Getting
 * that right requires our memory to actually match PPC's layout, not just
 * be internally consistent.
 */
typedef struct PpcContext {
    uint32_t r[32];
    double f[32];
    uint32_t lr;
    uint32_t ctr; /* count register -- used here for mtctr/bctrl indirect calls, not bdnz/bdz loop counting yet */
    uint8_t cr0_lt;
    uint8_t cr0_gt;
    uint8_t cr0_eq;
    uint8_t xer_ca; /* XER carry bit, set by addc/adde (used for multi-word/64-bit arithmetic) */
    uint8_t mem[65536];
} PpcContext;

static inline uint32_t ppc_load_u32(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void ppc_store_u32(PpcContext *ctx, uint32_t addr, uint32_t val) {
    uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

static inline uint8_t ppc_load_u8(const PpcContext *ctx, uint32_t addr) {
    return ctx->mem[addr & (sizeof(ctx->mem) - 1)];
}

static inline void ppc_store_u8(PpcContext *ctx, uint32_t addr, uint8_t val) {
    ctx->mem[addr & (sizeof(ctx->mem) - 1)] = val;
}

static inline uint16_t ppc_load_u16(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

static inline void ppc_store_u16(PpcContext *ctx, uint32_t addr, uint16_t val) {
    uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)val;
}

static inline uint64_t ppc_load_u64(const PpcContext *ctx, uint32_t addr) {
    return ((uint64_t)ppc_load_u32(ctx, addr) << 32) | (uint64_t)ppc_load_u32(ctx, addr + 4);
}

static inline void ppc_store_u64(PpcContext *ctx, uint32_t addr, uint64_t val) {
    ppc_store_u32(ctx, addr, (uint32_t)(val >> 32));
    ppc_store_u32(ctx, addr + 4, (uint32_t)val);
}

/* High 32 bits of a 64-bit product -- what a compiler emits for
 * division-by-constant (the well-known multiply-by-reciprocal trick), so
 * these show up constantly in real optimized code despite looking obscure. */
static inline uint32_t ppc_mulhw(int32_t a, int32_t b) {
    return (uint32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static inline uint32_t ppc_mulhwu(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
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

static inline void ppc_cmplw(PpcContext *ctx, uint32_t a, uint32_t b) {
    ctx->cr0_lt = a < b;
    ctx->cr0_gt = a > b;
    ctx->cr0_eq = a == b;
}

/* addc/adde: used together to add 64-bit (or wider) values held across
 * pairs of 32-bit registers -- addc computes the low word and captures the
 * carry-out in XER[CA], adde consumes that carry into the high word. */
static inline uint32_t ppc_addc(PpcContext *ctx, uint32_t a, uint32_t b) {
    uint64_t full = (uint64_t)a + (uint64_t)b;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

static inline uint32_t ppc_adde(PpcContext *ctx, uint32_t a, uint32_t b) {
    uint64_t full = (uint64_t)a + (uint64_t)b + (uint64_t)ctx->xer_ca;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* subfic rD, rA, SIMM: rD = SIMM - rA, computed (and XER[CA] set) the same
 * two's-complement way real hardware does it: ~rA + SIMM + 1. */
static inline uint32_t ppc_subfic(PpcContext *ctx, uint32_t a, int32_t simm) {
    uint64_t full = (uint64_t)(~a) + (uint64_t)(uint32_t)simm + 1u;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* Count of leading zero bits (0-32). Common for float/fixed-point
 * normalization and bit-scanning idioms. */
static inline uint32_t ppc_cntlzw(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    while ((v & 0x80000000u) == 0) {
        v <<= 1;
        n++;
    }
    return n;
}

static inline float ppc_load_f32(const PpcContext *ctx, uint32_t addr) {
    uint32_t bits = ppc_load_u32(ctx, addr);
    float v;
    memcpy(&v, &bits, sizeof(v)); /* host-native reinterpret, not a memory access -- fine either way */
    return v;
}

static inline void ppc_store_f32(PpcContext *ctx, uint32_t addr, double val) {
    float v = (float)val; /* narrow: stfs always stores the single-precision rounding */
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    ppc_store_u32(ctx, addr, bits);
}

static inline double ppc_load_f64(const PpcContext *ctx, uint32_t addr) {
    uint64_t bits = ppc_load_u64(ctx, addr);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static inline void ppc_store_f64(PpcContext *ctx, uint32_t addr, double val) {
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    ppc_store_u64(ctx, addr, bits);
}

/* fctiwz: convert a double to a 32-bit integer (round toward zero), placed
 * in the *low* 32 bits of the destination FPR per the real ISA -- the high
 * 32 bits are implementation-defined and never relied on by real compiled
 * code (it always reads the low word back out via a fixed-offset integer
 * load after storing the FPR with stfd). We don't have a distinct "FPR
 * holding a non-double bit pattern" representation, so this reuses the
 * f64 slot by round-tripping through the same 64-bit-bits path stfd/lfd
 * already use -- the high word is set to 0, which is never the part real
 * code reads.
 */
static inline double ppc_fctiwz(double val) {
    int32_t truncated = (int32_t)val;
    uint64_t bits = (uint64_t)(uint32_t)truncated;
    double result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* fcmpu: like ppc_cmpw but for floats. Real PPC also has an "unordered"
 * (NaN) case reported via a 4th CR bit this model doesn't track (see the
 * struct-level fidelity note above) -- comparisons involving NaN will
 * silently fall through as if not-less/not-greater/not-equal here rather
 * than setting an unordered flag. */
static inline void ppc_fcmpu(PpcContext *ctx, double a, double b) {
    ctx->cr0_lt = a < b;
    ctx->cr0_gt = a > b;
    ctx->cr0_eq = a == b;
}

/* Round-to-single-precision, matching PPC's single-precision FP ops
 * (fadds/fsubs/fmuls/fdivs/fmadds/...), which compute as double but store
 * a single-rounded result back into the (still 64-bit) FPR. */
static inline double ppc_frsp(double val) { return (double)(float)val; }

#endif /* BRAMBLE_PPC_RUNTIME_H */
