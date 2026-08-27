#ifndef ARKCHEMY_PPC_RUNTIME_H
#define ARKCHEMY_PPC_RUNTIME_H

/* Some cafeos_*.h shims (e.g. cafeos_coreinit_sync.h's real pthread-
 * backed OSMutex/OSEvent/OSSemaphore) need POSIX APIs (clock_gettime,
 * nanosleep, gmtime_r, pthread_mutexattr_settype/PTHREAD_MUTEX_RECURSIVE,
 * ...) beyond ISO C. glibc feature-test macros only take effect if
 * defined before the *first* system header of the translation unit is
 * ever processed (glibc's <features.h> computes and locks its __USE_*
 * set once, then no-ops on repeat inclusion) -- since this header is
 * always the first thing every cafeos_*.h includes, and in turn is
 * always the first #include in any translation unit that pulls in more
 * than one cafeos_*.h file, defining it here (rather than redundantly,
 * and too late to matter, in cafeos_coreinit_sync.h alone) is what
 * actually makes it reliably apply regardless of header include order. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Real, always-on, cheap "where is execution right now" tracker: codegen.cpp
 * emits a one-line write to this at the start of every real recompiled
 * function (see its own comment there), so a diagnostic log (or a debugger)
 * can see which real PPC function address is currently executing without
 * any per-instruction tracing. Real problem this specific form solves: this
 * header is included by every one of a real many-file build's separately-
 * compiled translation units (switch/game/'s 213 generated_*.c files), each
 * of which needs to update and a single *shared* value that switch/game/'s
 * own main.c (yet another, different translation unit) can read -- the
 * usual fix for that elsewhere in this project (extern here + one real
 * definition in cafeos_state.c) would mean every one of this project's much
 * smaller single-translation-unit programs (switch/native/, switch/gx2_test/,
 * and every tools/verify.sh test harness) would also need to link
 * cafeos_state.c just to satisfy this one symbol, for no real benefit to
 * them. `__attribute__((weak))` sidesteps that: every translation unit gets
 * its own real, tentative definition, and the linker coalesces all of them
 * that end up in the same final binary into exactly one shared instance
 * automatically -- correct, single-instance behavior in a real multi-file
 * build, and just as correct (trivially, since there's only one TU to
 * coalesce) in a single-file one, with no separate definition file needed
 * either way. Supported by every real toolchain this project's own builds
 * already depend on (GNU ld via devkitA64, and clang/lld via zig cc). */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ppc_current_pc = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint64_t g_ppc_fn_call_count = 0;

/* Same real reasoning as g_ppc_current_pc above, added 2026-08-20 for a
 * real, specific need it doesn't cover on its own: ppc_run_static_
 * initializers (see recomp's own main.cpp) calls up to 114 real,
 * completely untested C++ static initializers in a flat sequence -- if
 * one of them genuinely hangs, g_ppc_current_pc alone only shows the
 * last *recompiled* function entered, which is often a tiny, widely-
 * shared linker helper (e.g. a real "_savegprN"/"_restgprN"-style
 * register-spill routine used by hundreds of unrelated call sites) that
 * gives no clue which of the 114 initializers is actually stuck. This
 * index is set right before each of the 114 calls, so combined with
 * g_ppc_current_pc it answers "stuck inside initializer #N specifically"
 * instead of just "stuck somewhere". */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ppc_static_init_index = 0xFFFFFFFFu;

/* Real "who called the currently-executing function" tracker -- see
 * codegen.cpp's own comment on why g_ppc_current_pc alone isn't enough
 * once a hang is inside a tiny, universally-shared helper (a real
 * register-spill routine, in the specific real case this was added
 * for) that hundreds of unrelated call sites all call identically. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ppc_last_caller_lr = 0;

/* Bounded log of real indirect/virtual dispatch targets seen within a
 * specific real call-count window, 2026-08-24 -- set at ppc_dispatch()'s
 * own entry (a hand-inserted one-line edit in its generated_*.c
 * definition, not a per-instruction watch, so no risk of the
 * buffer/regen reliability issues this session's other hand-inserted
 * watches ran into). Neither a plain overwriting "last dispatch" global
 * nor an unconditional "first N ever" log works here: a plain global
 * gets overwritten by millions of later unrelated steady-state
 * dispatches before any checkpoint print sees it, and logging
 * unconditionally from boot fills the whole cap with static-init-phase
 * noise (confirmed on real hardware, 2026-08-24: all 64 slots used up
 * long before reaching the real target calls around g_ppc_fn_call_count
 * ~21100-21150) -- gating recording to ARKCHEMY_DISPATCH_LOG_WINDOW_LO/HI
 * instead keeps only the window this session actually needs to inspect
 * (initializePool's own two vtable-dispatched capacity-reservation
 * calls, hit at call 21131 per w0) regardless of how long the run
 * continues before or after it. Answers a specific question: does a
 * particular vtable-dispatched call resolve to a real function (a value
 * matching a real recompiled address) or 0/garbage (the vtable slot
 * backing it was never populated). */
#define ARKCHEMY_DISPATCH_LOG_WINDOW_LO 21100u



/* Generic "dump r3-r6 the instant a specific real function is entered"
 * mechanism, added 2026-08-20 hunting a real hang inside
 * Core::igStringPool::remove: g_ppc_current_pc alone says *that* the
 * function was entered, not what real arguments it was called with, and
 * this one has no internal call to any other traced function (a pure
 * pointer-chase loop over its own hash-bucket linked list), so nothing
 * else in the existing diagnostic set can show the real 'this'/item/
 * bucket-index values it's looping on. codegen.cpp's function prologue
 * checks every one of ARKCHEMY_WATCH_SLOTS real addresses on every real
 * function call and snapshots r3-r6 (plus the real g_ppc_fn_call_count
 * at that moment, and a running hit count) into whichever slot's own
 * `pc` field matches -- cheap enough (a handful of comparisons per
 * real function call) to leave compiled in permanently.
 *
 * Widened from a single watch point to 4 slots the same day, once the
 * first hit (a NULL `this` reaching `igStringPool::remove`) raised a
 * new, more specific question needing several real call sites'
 * arguments correlated *together* in one real run: does
 * `Core::igStringPool::bootstrapInitialize` (the real singleton
 * constructor) actually run before `Core::igStringPool::getDefault`
 * (the real accessor) is ever called, and what does getDefault() end
 * up returning each time. */
#define ARKCHEMY_WATCH_SLOTS 4
typedef struct {
    volatile uint32_t pc;         /* 0xFFFFFFFF = unused/never matches */
    volatile uint32_t r3, r4, r5, r6;
    volatile uint32_t hit_count;
    volatile uint64_t last_hit_call_count; /* g_ppc_fn_call_count at last hit, for ordering slots against each other */
} ArkchemyWatchSlot;
#ifdef __GNUC__
__attribute__((weak))
#endif
ArkchemyWatchSlot g_ppc_watch[ARKCHEMY_WATCH_SLOTS] = {
    {0xFFFFFFFFu, 0, 0, 0, 0, 0, 0},
    {0xFFFFFFFFu, 0, 0, 0, 0, 0, 0},
    {0xFFFFFFFFu, 0, 0, 0, 0, 0, 0},
    {0xFFFFFFFFu, 0, 0, 0, 0, 0, 0},
};

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
    /* Real CR1-CR7 field bits (LT/GT/EQ; SO is never tracked, same real
     * gap CR0's own SO already has). Added additively, alongside the
     * existing `cr0_lt`/`cr0_gt`/`cr0_eq` fields above (left completely
     * untouched -- every already-proven cr0-only codegen path keeps
     * using them exactly as before), rather than folding cr0 into this
     * array too, to avoid touching heavily-tested existing code for no
     * real benefit. Index 0 of each array is real but unused (cr0 has
     * its own dedicated fields above instead) -- kept anyway so `crN`
     * maps directly to array index N with no off-by-one, matching
     * codegen.cpp's own real crN address-to-index arithmetic
     * (`reg - PPC_REG_CR0LT` etc., confirmed contiguous in real
     * Capstone's own `ppc.h` register enum). Real motivation: the real
     * PowerPC SVR4 ABI's own varargs convention has a real caller set
     * CR1's EQ bit via `crclr`/`cror`/`crmove` to tell a vararg callee
     * whether floating-point register arguments were passed -- real,
     * confirmed-live code in the actual Skylanders binary this project
     * targets (GHS-compiled varargs prologues), not a hypothetical. */
    uint8_t cr_lt[8];
    uint8_t cr_gt[8];
    uint8_t cr_eq[8];
    /* Real GQR0-GQR7 (Graphics Quantization Registers), SPRs 912-919 --
     * real hardware state controlling psq_l/psq_lu/psq_st/psq_stu's
     * real quantized (non-float) paired-single formats (see
     * ppc_psq_store_quantized/ppc_psq_load_quantized below for the real
     * bit layout/semantics, confirmed against Dolphin's real, open-
     * source Gekko/Broadway CPU emulation -- same real CPU family as
     * the Wii U's Espresso). Zero-initialized, matching real hardware's
     * own real post-reset GQR state (type=FLOAT, scale=0, i.e. no
     * quantization) -- an honest, documented real default, not a
     * guess, for the common case where nothing in a given recompiled
     * program ever writes a GQR via mtspr before using a quantized
     * paired-single load/store. */
    uint32_t gqr[8];
    uint8_t xer_ca; /* XER carry bit, set by addc/adde (used for multi-word/64-bit arithmetic) */
    /* mftb (move-from-timebase): real code reads this as a free-running
     * hardware cycle counter (profiling, or occasionally a random seed).
     * No real timing model exists here, so this is just a counter that
     * advances by 1 on every read -- monotonic and always-changing (so
     * "poll until the timebase moves" loops still terminate) but not a
     * real elapsed-time value. Anything relying on actual wall-clock
     * timing from this would be a known, narrow gap. */
    uint32_t tb;
    /* `shared`, not an inline array: was a 65536-byte (64KB) inline
     * `mem[]` sized only for tiny test programs, then grown 64x to 4MB
     * inline -- still fine for one `PpcContext`, but real threading
     * (OSCreateThread, still unimplemented at the time of this change)
     * needs multiple *concurrent* PpcContexts (one per real host thread,
     * each with its own registers) that all see the *same* underlying
     * guest memory, the same way real Wii U threads share one address
     * space. An inline array can't be shared between separate struct
     * instances; a pointer to a separately-allocated `PpcSharedMemory`
     * can -- every thread's `PpcContext` gets its own fresh registers
     * but points `shared` at the same block. A single-threaded program
     * (everything so far) just points its one `PpcContext` at its own
     * privately-owned `PpcSharedMemory` -- behaviorally identical to the
     * old inline array, see `PPC_MEM_SIZE` below for the size.
     *
     * Every existing `PpcContext` consumer (tools/gen_harness*.c,
     * switch/native/source/main.c) already used `static` storage
     * instead of a stack-local -- updated to also allocate and bind a
     * `static PpcSharedMemory` alongside, the same mechanical pattern
     * every one of those files now follows. New standalone shim tests
     * should do the same (`ctx.shared = &some_static_PpcSharedMemory;`
     * before first use) -- `shared` is NULL by default (zeroed BSS/
     * stack), so forgetting this is a fast, loud NULL-deref crash, not
     * a silent wrong-answer bug.
     */
    struct PpcSharedMemory *shared;
} PpcContext;

/* Real, found-the-hard-way sizing: the previous 4MB was "an arbitrary,
 * generous, documented placeholder, not a claim this matches real Wii U
 * game scale" -- turned out to be a real, severe bug once actually
 * exercised against the real, complete Skylanders: Spyro's Adventure
 * binary. `assign_global_addrs` (elf_loader.cpp) lays out that real
 * game's own .data/.bss/.rodata globals starting at 0x2000, and for
 * this specific real binary that region runs all the way out to
 * 0x670b00 (~6.75MB, confirmed by direct instrumentation, not
 * estimated) -- already bigger than the entire old 4MB guest address
 * space by itself, before any heap or stack. Every guest memory access
 * masks its address with `& (PPC_MEM_SIZE - 1)` (see ppc_load_u32 and
 * friends below), so with the old 4MB size, large stretches of the
 * real game's own global variables were silently wrapping around and
 * aliasing on top of *each other*, and on top of the small fixed-
 * address heap regions cafeos_coreinit_mem.h reserves -- real,
 * ongoing memory corruption from the moment the game's own static
 * initializers ran, long before any of its own code had a chance to
 * misbehave on its own. 128MB gave real breathing room for this
 * specific game's ~6.75MB of globals, cafeos_coreinit_mem.h's own
 * MEM1/MEM2 heap regions (see that header's own layout comment), and a
 * real stack -- still an arbitrary, chosen-for-this-game placeholder,
 * not a claim this matches real Wii U MEM1/MEM2 scale (which is far
 * larger), but grounded in this real binary's own measured needs
 * rather than picked blind.
 *
 * Bumped 128MB -> 256MB on 2026-08-21 after real hardware logs caught
 * the actual root cause of the boot-time igStringPool spin: a
 * MEMAllocFromExpHeapEx call for 128KB failing over and over against
 * cafeos_coreinit_mem.h's MEM1 heap, which was sitting at ~16.65MB used
 * out of only 16MB total. That 16MB MEM1 size was always documented as
 * an undersized placeholder, not a real Wii U value -- real Wii U MEM1
 * is 32MB. Doubling MEM1 to the real size needed 16MB more guest
 * address space than the old 128MB total had room for, and this mask
 * requires a power of two, so the whole space steps up to 256MB rather
 * than some tighter number -- see cafeos_coreinit_mem.h's own layout
 * comment for where that extra room actually goes. */
/* Raised 256MB -> 512MB, 2026-08-24. Must stay a power of two: every
 * accessor masks with (PPC_MEM_SIZE - 1).
 *
 * Measured reason, not a guess. With the default heap correctly routed
 * to MEM2, Green Hills libc's sbrk() consumed the entire 96MB MEM2 pool
 * during static initialisation alone (heap_used 100,582,552 of
 * 100,663,296) and allocations began failing again. sbrk only ever
 * grows, so the game's C heap needs real room. Real Wii U MEM2 is 2GB;
 * the binding constraint here is this address space, not the pool
 * layout inside it, so the space itself had to grow.
 *
 * 1GB again as of 2026-08-24 (late). This was tried and reverted once
 * before, and the reasons it failed then no longer hold: malloc was
 * broken at the time (successive sbrk allocations were not adjacent, so
 * it never reused its heap and consumed whatever it was given), which
 * is why extra memory bought nothing. With that contract fixed, malloc
 * genuinely reuses memory, and the run that exposed this now reaches
 * real engine work instead of a null-pool spin -- so headroom should
 * now convert into progress rather than vanish. Being explicit that
 * this is a deliberate retry of a previously-failed change under
 * materially different conditions, not a forgotten lesson.
 *
 * Historical note on that first attempt: MEM2 at
 * 928MB filled to 99.995% and produced 1,567 failures against 1,563 at
 * 416MB. Doubling the pool moved only when the wall was hit (call
 * 18,257 -> 38,727), not whether. sbrk consumes whatever it is given at
 * every size from 32MB to 928MB, so this is unbounded growth and no
 * amount of BSS fixes it -- see MEM2's note for where the real suspicion
 * now sits.
 *
 * Cost: mem[] is a static array, so this is 512MB of BSS on top of
 * ~158MB of .text. That is comfortable in application mode (the owner
 * launches via a Sphaira forwarder from the HOME menu, so gigabytes are
 * available) but would NOT fit applet mode's ~448MB budget -- a build
 * launched from the Album applet will fail to allocate. Worth knowing
 * before anyone tries that. */
/* Overridable at compile time (-DPPC_MEM_SIZE=...) because not every
 * consumer of this header wants the full-game arena. jouster's native/
 * on-hardware test suite links nine small recompiled test programs into
 * one .nro, each with its own PpcSharedMemory for isolation -- at 1GB
 * apiece that is 9GB of BSS in a homebrew app, which simply will not
 * load. It builds with 4MB instead. Must stay a power of two: every
 * accessor below masks with (PPC_MEM_SIZE - 1). */
#ifndef PPC_MEM_SIZE
#define PPC_MEM_SIZE (1024u * 1024u * 1024u)
#endif

typedef struct PpcSharedMemory {
    uint8_t mem[PPC_MEM_SIZE];
} PpcSharedMemory;

static inline uint32_t ppc_load_u32(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Real, general-purpose "watch every store to one specific real address"
 * mechanism, added 2026-08-20 alongside ppc_debug_watch below (same real
 * extern/shared-definition pattern, see cafeos_state.c) -- found
 * necessary chasing a real, confirmed heap-corruption bug: a malloc
 * free-list head field read back 0xFFFFFFFF instead of a real address,
 * and grepping generated source by hand for "whoever writes this
 * specific computed address" isn't reliable (the same real address can
 * be split into a `lis`+offset pair in many different, equally valid
 * ways across different real call sites -- there's no single literal
 * string to grep for). Every 32-bit store in the entire recompiled
 * program already goes through this one function, making it the exact
 * right choke point: set g_ppc_watch_store_addr to the real address of
 * interest and every write to it fires ppc_debug_watch with the value
 * being written, tagged so it's distinguishable from other watch call
 * sites -- reveals *who* writes a bad value somewhere in ~19,000
 * functions without needing to guess which one to hand-instrument. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ppc_watch_store_addr = 0xFFFFFFFFu;
static inline void ppc_debug_watch(uint32_t pc, uint32_t value); /* real definition below */

static inline void ppc_store_u32(PpcContext *ctx, uint32_t addr, uint32_t val) {
    if (addr == g_ppc_watch_store_addr) {
        ppc_debug_watch(0xf0000001u, val);              /* the value being written */
        ppc_debug_watch(0xf0000002u, g_ppc_current_pc);  /* which real function is doing it */
    }
    uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

static inline uint8_t ppc_load_u8(const PpcContext *ctx, uint32_t addr) {
    return ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
}

static inline void ppc_store_u8(PpcContext *ctx, uint32_t addr, uint8_t val) {
    // Real gap found and fixed 2026-08-20: g_ppc_watch_store_addr's check
    // only lived in ppc_store_u32, so it silently missed real writes made
    // one byte at a time -- confirmed real, not hypothetical:
    // ppc_init_globals (see main.cpp) copies every section's real initial
    // byte content via exactly this function, one ppc_store_u8 call per
    // real non-zero byte, and a watched address that only ever gets
    // written this way would show zero hits despite genuinely holding
    // real, non-zero initial content. Same real address, any of its 4
    // real bytes.
    if (addr >= g_ppc_watch_store_addr && addr < g_ppc_watch_store_addr + 4) {
        ppc_debug_watch(0xf0000003u, ((addr - g_ppc_watch_store_addr) << 8) | val);
        ppc_debug_watch(0xf0000004u, g_ppc_current_pc);
    }
    ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)] = val;
}

static inline uint16_t ppc_load_u16(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}

static inline void ppc_store_u16(PpcContext *ctx, uint32_t addr, uint16_t val) {
    uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)val;
}

/* lwbrx/lhbrx: byte-reversed loads (real hardware reads the same
 * big-endian bytes as ppc_load_u32/u16, then swaps them) -- code that
 * needs little-endian data from a big-endian machine, or vice versa. */
static inline uint32_t ppc_load_u32_brx(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static inline uint16_t ppc_load_u16_brx(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
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

/* Real cmpw/cmplw variants targeting an explicit non-cr0 field
 * (`cr` is 1-7 -- see `PpcContext::cr_lt`/`cr_gt`/`cr_eq`'s own
 * comment for why cr0 keeps using the separate, original functions
 * above instead of index 0 here). */
static inline void ppc_cmpw_cr(PpcContext *ctx, int cr, int32_t a, int32_t b) {
    ctx->cr_lt[cr] = a < b;
    ctx->cr_gt[cr] = a > b;
    ctx->cr_eq[cr] = a == b;
}

static inline void ppc_cmplw_cr(PpcContext *ctx, int cr, uint32_t a, uint32_t b) {
    ctx->cr_lt[cr] = a < b;
    ctx->cr_gt[cr] = a > b;
    ctx->cr_eq[cr] = a == b;
}

/* mfcr: packs all 8 real CR fields into a real 32-bit CR value, matching
 * real hardware's layout (CR0 is the top 4 bits: LT,GT,EQ,SO; CR1 the
 * next 4; ...; CR7 the bottom 4). SO is never tracked/set for any real
 * field (a real, narrow, pre-existing gap), so those bits are always 0.
 * CR1-CR7 now come from the real, tracked `cr_lt`/`cr_gt`/`cr_eq` arrays
 * (see PpcContext's own comment) -- previously always 0 here
 * regardless, correct only when nothing recompiled read them. */
static inline uint32_t ppc_mfcr(const PpcContext *ctx) {
    uint32_t cr = ((ctx->cr0_lt ? 8u : 0u) | (ctx->cr0_gt ? 4u : 0u) | (ctx->cr0_eq ? 2u : 0u)) << 28;
    int i;
    for (i = 1; i < 8; i++) {
        uint32_t field = (ctx->cr_lt[i] ? 8u : 0u) | (ctx->cr_gt[i] ? 4u : 0u) | (ctx->cr_eq[i] ? 2u : 0u);
        cr |= field << (28 - i * 4);
    }
    return cr;
}

/* mtcrf targeting field 0 (CR0) specifically -- real hardware bit
 * layout, same reasoning as ppc_mfcr above. */
static inline void ppc_mtcrf_cr0(PpcContext *ctx, uint32_t val) {
    uint32_t cr0 = (val >> 28) & 0xFu;
    ctx->cr0_lt = (cr0 & 8u) != 0;
    ctx->cr0_gt = (cr0 & 4u) != 0;
    ctx->cr0_eq = (cr0 & 2u) != 0;
}

/* mtcrf targeting an explicit non-cr0 field (1-7) -- extracts that
 * field's real 4-bit slice (LT,GT,EQ,SO) from the same real bit
 * position it occupies in a full 32-bit CR value, matching
 * ppc_mtcrf_cr0's own real bit-layout reasoning. */
static inline void ppc_mtcrf_field(PpcContext *ctx, int field, uint32_t val) {
    uint32_t bits = (val >> (28 - field * 4)) & 0xFu;
    ctx->cr_lt[field] = (bits & 8u) != 0;
    ctx->cr_gt[field] = (bits & 4u) != 0;
    ctx->cr_eq[field] = (bits & 2u) != 0;
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

/* Real fcmpu variant targeting an explicit non-cr0 field, same real
 * reasoning as ppc_cmpw_cr/ppc_cmplw_cr above. */
static inline void ppc_fcmpu_cr(PpcContext *ctx, int cr, double a, double b) {
    ctx->cr_lt[cr] = a < b;
    ctx->cr_gt[cr] = a > b;
    ctx->cr_eq[cr] = a == b;
}

/* fabs fD, fB: absolute value, done branchlessly here to avoid pulling in
 * <math.h> for something this simple. */
static inline double ppc_fabs(double val) { return val < 0.0 ? -val : val; }

/* ---- Real GQR-quantized paired-single load/store -----------------------
 *
 * Real GQR register bit layout and real quantize-type encodings,
 * confirmed against Dolphin's real, open-source Gekko/Broadway CPU
 * emulation (`Source/Core/Core/PowerPC/Gekko.h`'s real `UGQR` union and
 * `EQuantizeType` enum) -- same real CPU family as the Wii U's Espresso,
 * not guessed/derived: `st_type` bits [0:2], `st_scale` bits [8:13],
 * `ld_type` bits [16:18], `ld_scale` bits [24:29]. Real types: 0=FLOAT
 * (no quantization), 1-3=reserved/invalid (never expected in practice,
 * treated the same as FLOAT here -- an honest "no quantization applied"
 * fallback, not a guess at some other real behavior), 4=U8, 5=U16,
 * 6=S8, 7=S16. */
#define PPC_GQR_TYPE_U8 4u
#define PPC_GQR_TYPE_U16 5u
#define PPC_GQR_TYPE_S8 6u
#define PPC_GQR_TYPE_S16 7u

/* Real quantize/dequantize scale factor: a real GQR scale field is a
 * real, signed 6-bit value (raw 32-63 means -32..-1 in two's
 * complement) -- confirmed against Dolphin's real dequantize/quantize
 * lookup tables, which are mathematically just a real `2^scale` for a
 * signed scale in that same range; `ldexpf` gives the identical real
 * result without needing a 64-entry table. */
static inline float ppc_gqr_scale_factor(uint32_t scale6, int negate) {
    int signed_scale = (scale6 >= 32) ? (int)scale6 - 64 : (int)scale6;
    return ldexpf(1.0f, negate ? -signed_scale : signed_scale);
}

/* Real quantized paired-single store: writes 1 or 2 real elements (per
 * `single`, matching the real instruction's W bit) of a real
 * quantized-type-and-width-appropriate size, each real element =
 * `clamp(ps * 2^st_scale, real type range)`, matching Dolphin's own
 * real `ScaleAndClamp`/`QuantizeAndStore` logic exactly. `gqr` is the
 * real, raw 32-bit GQR register value (only its real ST_TYPE/ST_SCALE
 * bits are read here). */
static inline void ppc_psq_store_quantized(PpcContext *ctx, uint32_t addr, double ps0, double ps1, uint32_t gqr, int single) {
    uint32_t type = gqr & 0x7u;
    uint32_t scale = (gqr >> 8) & 0x3Fu;
    float factor = ppc_gqr_scale_factor(scale, 0);
    float v0 = (float)ps0 * factor;
    float v1 = (float)ps1 * factor;
    switch (type) {
        case PPC_GQR_TYPE_U8: {
            float c0f = v0 < 0.0f ? 0.0f : (v0 > 255.0f ? 255.0f : v0);
            ppc_store_u8(ctx, addr, (uint8_t)c0f);
            if (!single) {
                float c1f = v1 < 0.0f ? 0.0f : (v1 > 255.0f ? 255.0f : v1);
                ppc_store_u8(ctx, addr + 1, (uint8_t)c1f);
            }
            break;
        }
        case PPC_GQR_TYPE_S8: {
            float c0f = v0 < -128.0f ? -128.0f : (v0 > 127.0f ? 127.0f : v0);
            ppc_store_u8(ctx, addr, (uint8_t)(int8_t)c0f);
            if (!single) {
                float c1f = v1 < -128.0f ? -128.0f : (v1 > 127.0f ? 127.0f : v1);
                ppc_store_u8(ctx, addr + 1, (uint8_t)(int8_t)c1f);
            }
            break;
        }
        case PPC_GQR_TYPE_U16: {
            float c0f = v0 < 0.0f ? 0.0f : (v0 > 65535.0f ? 65535.0f : v0);
            ppc_store_u16(ctx, addr, (uint16_t)c0f);
            if (!single) {
                float c1f = v1 < 0.0f ? 0.0f : (v1 > 65535.0f ? 65535.0f : v1);
                ppc_store_u16(ctx, addr + 2, (uint16_t)c1f);
            }
            break;
        }
        case PPC_GQR_TYPE_S16: {
            float c0f = v0 < -32768.0f ? -32768.0f : (v0 > 32767.0f ? 32767.0f : v0);
            ppc_store_u16(ctx, addr, (uint16_t)(int16_t)c0f);
            if (!single) {
                float c1f = v1 < -32768.0f ? -32768.0f : (v1 > 32767.0f ? 32767.0f : v1);
                ppc_store_u16(ctx, addr + 2, (uint16_t)(int16_t)c1f);
            }
            break;
        }
        default: /* FLOAT (0) or a real reserved/invalid type (1-3) -- honest fallback, no quantization */
            ppc_store_f32(ctx, addr, ps0);
            if (!single) ppc_store_f32(ctx, addr + 4, ps1);
            break;
    }
}

/* Real quantized paired-single load, the dequantize counterpart to
 * ppc_psq_store_quantized above -- `gqr`'s real LD_TYPE/LD_SCALE bits
 * (a different real bit range from ST_TYPE/ST_SCALE) determine the
 * real element width/dequantize factor. `*out_ps1` is left untouched
 * when `single` (matching real psq_l/psq_lu's own W=1 behavior of
 * always setting ps1 to 1.0f, handled by the caller, not here, same
 * as the existing float-only psq_l/psq_lu codegen already does). */
static inline void ppc_psq_load_quantized(const PpcContext *ctx, uint32_t addr, double *out_ps0, double *out_ps1, uint32_t gqr, int single) {
    uint32_t type = (gqr >> 16) & 0x7u;
    uint32_t scale = (gqr >> 24) & 0x3Fu;
    float factor = ppc_gqr_scale_factor(scale, 1);
    switch (type) {
        case PPC_GQR_TYPE_U8:
            *out_ps0 = (double)((float)ppc_load_u8(ctx, addr) * factor);
            if (!single) *out_ps1 = (double)((float)ppc_load_u8(ctx, addr + 1) * factor);
            break;
        case PPC_GQR_TYPE_S8:
            *out_ps0 = (double)((float)(int8_t)ppc_load_u8(ctx, addr) * factor);
            if (!single) *out_ps1 = (double)((float)(int8_t)ppc_load_u8(ctx, addr + 1) * factor);
            break;
        case PPC_GQR_TYPE_U16:
            *out_ps0 = (double)((float)ppc_load_u16(ctx, addr) * factor);
            if (!single) *out_ps1 = (double)((float)ppc_load_u16(ctx, addr + 2) * factor);
            break;
        case PPC_GQR_TYPE_S16:
            *out_ps0 = (double)((float)(int16_t)ppc_load_u16(ctx, addr) * factor);
            if (!single) *out_ps1 = (double)((float)(int16_t)ppc_load_u16(ctx, addr + 2) * factor);
            break;
        default: /* FLOAT (0) or a real reserved/invalid type (1-3) -- honest fallback, no quantization */
            *out_ps0 = (double)ppc_load_f32(ctx, addr);
            if (!single) *out_ps1 = (double)ppc_load_f32(ctx, addr + 4);
            break;
    }
}

/* tw/twu (unconditional trap): real hardware raises a program exception,
 * which real compiled code relies on to actually stop execution (e.g.
 * compiler-inserted bounds/null-pointer/assert checks on a path that
 * should be unreachable in correct code). abort() is the closest
 * equivalent this runtime has -- deliberately not a no-op, since silently
 * continuing past a real trap would let known-corrupt state keep
 * executing instead of failing loudly the way the original binary
 * would. */
static inline void ppc_trap(void) { abort(); }

/* Real, honest, deliberately temporary fallback for the small number of
 * real instructions this runtime doesn't yet model correctly (every
 * real call site names itself via recomp's own generated `what`
 * string, taken directly from the exact real `#error` diagnostic that
 * would otherwise have blocked the whole program from compiling --
 * this exists specifically to unblock a full real-game build/run while
 * those few genuinely-unhandled real cases get fixed properly, not to
 * hide them). Logs (if a sink is registered via
 * ppc_set_unhandled_log -- real, optional, e.g. a real SD-card
 * checkpoint file) and returns, letting the rest of a real recompiled
 * program keep running instead of hard-aborting the whole process over
 * one, possibly-never-exercised code path -- a real, deliberate choice
 * favoring "see the real game run" over "perfect from instruction
 * one," consistent with this project's own established shader/draw-
 * call no-op precedent (see cafeos_gx2.h's own comment on that). */
typedef void (*ppc_unhandled_log_fn)(const char *);
/* extern, not `static` -- this same header is included by every one of
 * a real many-file build's separately-compiled translation units (see
 * switch/game/'s own 213 generated_*.c files), and the actual
 * ppc_unhandled_stub() calls this sink almost always happen inside one
 * of *those* files, not the one file (main.c) that calls
 * ppc_set_unhandled_log() to register it. A `static` copy here would
 * mean every one of those 213 files gets its own, separate, never-set
 * NULL copy -- real bug found and fixed this way (same root cause as
 * cafeos_state.c's own g_arkchemy_gx2 and friends): logging would
 * silently never fire for any real unhandled-instruction hit inside
 * the actual recompiled game code, only from a hit in main.c itself
 * (which has none). Real, single, shared definition lives in
 * cafeos_state.c alongside everything else that needed this fix. */
extern ppc_unhandled_log_fn g_ppc_unhandled_log;
static inline void ppc_set_unhandled_log(ppc_unhandled_log_fn fn) { g_ppc_unhandled_log = fn; }
static inline void ppc_unhandled_stub(PpcContext *ctx, const char *what) {
    (void)ctx;
    if (g_ppc_unhandled_log) g_ppc_unhandled_log(what);
}

/* Real, general-purpose, ad hoc debug watchpoint -- not called anywhere
 * by default (no codegen support needed): a one-off call to
 * ppc_debug_watch() can be hand-inserted directly into a specific
 * generated_*.c file (machine-generated, gitignored, safe to hand-edit
 * for one debugging session -- regenerate.sh overwrites it back to
 * clean output next real run) at whatever exact real PPC instruction
 * address needs a live register/memory value confirmed on real
 * hardware, without needing new codegen support or a full regenerate +
 * rebuild cycle for something this targeted.
 *
 * Weak, not plain extern (real regression found and fixed 2026-08-20):
 * this used to only ever get referenced by a translation unit that had
 * an explicit, hand-inserted ppc_debug_watch() call, so small single-
 * file test programs (tools/gen_harness*.c) never needed a real shared
 * definition from cafeos_state.c. That stopped being true once
 * ppc_store_u32 itself started calling this unconditionally (see
 * g_ppc_watch_store_addr above) -- now every single program using
 * ppc_store_u32 at all references it, including every one of those
 * small test harnesses, which broke tools/verify.sh with a real
 * "undefined reference to g_ppc_debug_watch" link error. Same weak-
 * symbol pattern as g_ppc_current_pc above fixes it the same way. */
typedef void (*ppc_debug_watch_fn)(uint32_t pc, uint32_t value);
#ifdef __GNUC__
__attribute__((weak))
#endif
ppc_debug_watch_fn g_ppc_debug_watch = NULL;
static inline void ppc_set_debug_watch(ppc_debug_watch_fn fn) { g_ppc_debug_watch = fn; }
static inline void ppc_debug_watch(uint32_t pc, uint32_t value) {
    if (g_ppc_debug_watch) g_ppc_debug_watch(pc, value);
}

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

/*
 * ppc_dispatch: recomp emits a real definition of this once per compiled
 * program (see main.cpp) -- a switch over every recovered function's
 * address, used to resolve mtctr/bctrl indirect calls at runtime since
 * their target isn't known until then. Declared here (not defined --
 * every real generated program provides the real definition) so CafeOS
 * shim headers can reuse the exact same mechanism to call *into*
 * recompiled code for real guest callback invocation (e.g. an FSAsyncData
 * completion callback, or nsyshid's HIDCallback) -- setting up the
 * callback's arguments in ctx->r[3..], calling this, then restoring
 * ctx->r[3] to the shim's own actual return value afterward, exactly the
 * same calling convention a real indirect call already uses. This is the
 * one piece of shim-authored code that depends on something the
 * recompiler itself emits rather than being fully self-contained --
 * shims that use it are not yet exercised through the real recompile
 * pipeline (see docs/phase1d_import_surface.md), only via a standalone
 * test that supplies its own fake ppc_dispatch.
 */
void ppc_dispatch(PpcContext *ctx, uint32_t addr);

#endif /* ARKCHEMY_PPC_RUNTIME_H */
