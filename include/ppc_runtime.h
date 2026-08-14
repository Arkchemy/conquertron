#ifndef BRAMBLE_PPC_RUNTIME_H
#define BRAMBLE_PPC_RUNTIME_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
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
    /* ps1: the second lane of each FPR when used in PowerPC 750CL
     * ("Gekko"/Broadway/Espresso) paired-single mode -- a real vendor SIMD
     * extension, not modeled by generic PowerPC. Real hardware packs ps0
     * and ps1 as two 32-bit floats sharing one 64-bit FPR; ps0 reuses the
     * existing f[] slot (same convention as every other single-precision
     * value in this runtime -- see the f[] comment below), ps1 has no
     * scalar-FPR equivalent so it needs its own array. Only ever written
     * by paired-single loads/merges (see codegen.cpp's PSQ_L/PS_MERGE*
     * handling) -- plain scalar FP instructions never touch it, matching
     * real hardware where non-paired ops don't disturb ps1. */
    float ps1[32];
    uint32_t lr;
    uint32_t ctr; /* count register -- used here for mtctr/bctrl indirect calls, not bdnz/bdz loop counting yet */
    uint8_t cr0_lt;
    uint8_t cr0_gt;
    uint8_t cr0_eq;
    uint8_t xer_ca; /* XER carry bit, set by addc/adde (used for multi-word/64-bit arithmetic) */
    /* mftb (move-from-timebase): real code reads this as a free-running
     * hardware cycle counter (profiling, or occasionally a random seed).
     * No real timing model exists here, so this is just a counter that
     * advances by 1 on every read -- monotonic and always-changing (so
     * "poll until the timebase moves" loops still terminate) but not a
     * real elapsed-time value. Anything relying on actual wall-clock
     * timing from this would be a known, narrow gap. */
    uint32_t tb;
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

/* lwbrx/lhbrx: byte-reversed loads (real hardware reads the same
 * big-endian bytes as ppc_load_u32/u16, then swaps them) -- code that
 * needs little-endian data from a big-endian machine, or vice versa. */
static inline uint32_t ppc_load_u32_brx(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static inline uint16_t ppc_load_u16_brx(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->mem[addr & (sizeof(ctx->mem) - 1)];
    return (uint16_t)(((uint32_t)p[1] << 8) | (uint32_t)p[0]);
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

/* mfcr: packs CR0 (the only CR field this model tracks -- see the
 * struct-level fidelity note above) into bits 28-31 of a 32-bit value,
 * matching the real CR register layout (CR0 is the top 4 bits: LT,GT,EQ,
 * SO). SO is never tracked/set, so that bit is always 0. Other CR fields
 * are always 0 here too, which is only actually correct if nothing in
 * the recompiled code reads them -- a real but narrow gap shared with
 * every other cr0-only piece of this runtime. */
static inline uint32_t ppc_mfcr(const PpcContext *ctx) {
    uint32_t cr0 = (ctx->cr0_lt ? 8u : 0u) | (ctx->cr0_gt ? 4u : 0u) | (ctx->cr0_eq ? 2u : 0u);
    return cr0 << 28;
}

/* mtcrf targeting field 0 (CR0) specifically -- see codegen.cpp's
 * PPC_INS_MTCRF handling for why only field 0 is wired up. */
static inline void ppc_mtcrf_cr0(PpcContext *ctx, uint32_t val) {
    uint32_t cr0 = (val >> 28) & 0xFu;
    ctx->cr0_lt = (cr0 & 8u) != 0;
    ctx->cr0_gt = (cr0 & 4u) != 0;
    ctx->cr0_eq = (cr0 & 2u) != 0;
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

/* subfc rD, rA, rB: rD = rB - rA (register form of subfic), same
 * ~rA + rB + 1 formulation, capturing XER[CA]. */
static inline uint32_t ppc_subfc(PpcContext *ctx, uint32_t a, uint32_t b) {
    uint64_t full = (uint64_t)(~a) + (uint64_t)b + 1u;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* subfe rD, rA, rB: rD = ~rA + rB + XER[CA] -- the subtract-with-borrow
 * counterpart to adde, for wider subtraction chains built from subfc. */
static inline uint32_t ppc_subfe(PpcContext *ctx, uint32_t a, uint32_t b) {
    uint64_t full = (uint64_t)(~a) + (uint64_t)b + (uint64_t)ctx->xer_ca;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* addme rD, rA: rD = rA + XER[CA] - 1 ("add minus one extended") -- the
 * -1-biased sibling of addze, used the same way in wider add chains. */
static inline uint32_t ppc_addme(PpcContext *ctx, uint32_t a) {
    uint64_t full = (uint64_t)a + (uint64_t)ctx->xer_ca + 0xFFFFFFFFull;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* sraw rA, rS, rB: arithmetic shift right by a *register*-specified amount
 * (0-63, though only 0-31 shift distinctly -- 32+ saturates to all-sign-
 * bit). Unlike a plain `>>`, real hardware also sets XER[CA] here: 1 if
 * the source was negative and any 1-bits were shifted out (i.e. the
 * shifted-out bits would have needed a borrow to reconstruct via a
 * later shift-left -- the same "was information lost from a negative
 * value" signal subfe-style carry chains rely on elsewhere). */
static inline uint32_t ppc_sraw(PpcContext *ctx, int32_t a, uint32_t shift_reg) {
    uint32_t n = shift_reg & 0x3Fu;
    if (n >= 32) {
        ctx->xer_ca = (a < 0) ? 1u : 0u;
        return (a < 0) ? 0xFFFFFFFFu : 0u;
    }
    uint32_t mask = (n == 0) ? 0u : ((1u << n) - 1u);
    ctx->xer_ca = (a < 0 && ((uint32_t)a & mask) != 0) ? 1u : 0u;
    return (uint32_t)(a >> n);
}

/* addic rD, rA, SIMM: like addc, but the second operand is an immediate. */
static inline uint32_t ppc_addic(PpcContext *ctx, uint32_t a, int32_t simm) {
    uint64_t full = (uint64_t)a + (uint64_t)(uint32_t)simm;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* addze rD, rA: rD = rA + XER[CA] (propagating a carry into the next word
 * of a wider add, when there's nothing else to add at this word). */
static inline uint32_t ppc_addze(PpcContext *ctx, uint32_t a) {
    uint64_t full = (uint64_t)a + (uint64_t)ctx->xer_ca;
    ctx->xer_ca = (uint8_t)((full >> 32) & 1);
    return (uint32_t)full;
}

/* subfze rD, rA: rD = ~rA + XER[CA] -- the subtract-with-borrow counterpart
 * to addze, for wider subtraction chains. */
static inline uint32_t ppc_subfze(PpcContext *ctx, uint32_t a) {
    uint64_t full = (uint64_t)(uint32_t)(~a) + (uint64_t)ctx->xer_ca;
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

/* stfiwx: store the low 32 bits of an FPR's raw bit pattern to memory, as
 * a plain integer word -- the single-instruction shortcut for the
 * fctiwz-then-stfd-then-read-low-word idiom ppc_fctiwz's own comment
 * describes. (uint32_t)bits truncates by *value*, not byte layout, so
 * this is host-endianness-independent; ppc_store_u32 handles writing it
 * out in genuine PPC big-endian order. */
static inline void ppc_store_f64_low32(PpcContext *ctx, uint32_t addr, double val) {
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    ppc_store_u32(ctx, addr, (uint32_t)bits);
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

/* fabs fD, fB: absolute value, done branchlessly here to avoid pulling in
 * <math.h> for something this simple. */
static inline double ppc_fabs(double val) { return val < 0.0 ? -val : val; }

/* tw/twu (unconditional trap): real hardware raises a program exception,
 * which real compiled code relies on to actually stop execution (e.g.
 * compiler-inserted bounds/null-pointer/assert checks on a path that
 * should be unreachable in correct code). abort() is the closest
 * equivalent this runtime has -- deliberately not a no-op, since silently
 * continuing past a real trap would let known-corrupt state keep
 * executing instead of failing loudly the way the original binary
 * would. */
static inline void ppc_trap(void) { abort(); }

/* lswi rD, rA, NB: loads NB bytes (1-32; 0 means 32) from memory starting
 * at EA into consecutive GPRs starting at rD, wrapping r31 -> r0. Each
 * register is packed MSB-first (byte 0 of the copy goes in the top byte
 * of the first register), matching how a real struct/array byte-copy
 * naturally lays out; a register only partially filled by the final few
 * bytes has its remaining low-order bytes zeroed, matching real hardware
 * (this is a real ISA instruction, e.g. for structs whose size isn't a
 * multiple of 4, not a byte-order guess). */
static inline void ppc_lswi(PpcContext *ctx, uint32_t rD_start, uint32_t addr, uint32_t nb) {
    if (nb == 0) nb = 32;
    uint32_t idx = rD_start;
    uint32_t word = 0;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t byte_in_word = i & 3u;
        word |= (uint32_t)ppc_load_u8(ctx, addr + i) << (24 - 8 * byte_in_word);
        if (byte_in_word == 3 || i == nb - 1) {
            ctx->r[idx] = word;
            word = 0;
            idx = (idx + 1) & 31u;
        }
    }
}

/* stswi rS, rA, NB: the store counterpart to lswi -- same byte packing
 * and register wraparound, in reverse. */
static inline void ppc_stswi(PpcContext *ctx, uint32_t rS_start, uint32_t addr, uint32_t nb) {
    if (nb == 0) nb = 32;
    uint32_t idx = rS_start;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t byte_in_word = i & 3u;
        uint8_t b = (uint8_t)(ctx->r[idx] >> (24 - 8 * byte_in_word));
        ppc_store_u8(ctx, addr + i, b);
        if (byte_in_word == 3) idx = (idx + 1) & 31u;
    }
}

/* mftb rD: see the PpcContext::tb field comment for what this does and
 * doesn't model. */
static inline uint32_t ppc_mftb(PpcContext *ctx) { return ++ctx->tb; }

/* frsqrte: reciprocal square root *estimate* on real hardware (a fast,
 * low-precision lookup used as a Newton-Raphson starting point). Computed
 * exactly here instead -- a more-precise-than-real-hardware estimate is
 * still a valid starting point for any refinement steps the compiled
 * code performs afterward, so this doesn't change final results. */
static inline double ppc_frsqrte(double val) { return 1.0 / sqrt(val); }

/* Round-to-single-precision, matching PPC's single-precision FP ops
 * (fadds/fsubs/fmuls/fdivs/fmadds/...), which compute as double but store
 * a single-rounded result back into the (still 64-bit) FPR. */
static inline double ppc_frsp(double val) { return (double)(float)val; }

#endif /* BRAMBLE_PPC_RUNTIME_H */
