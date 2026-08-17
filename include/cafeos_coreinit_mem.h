#ifndef BRAMBLE_CAFEOS_COREINIT_MEM_H
#define BRAMBLE_CAFEOS_COREINIT_MEM_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit's MEM* heap allocator family
 * (ExpHeap/FrmHeap) plus __gh_errno_ptr/__gh_set_errno, which need the
 * exact same thing: a real, stable, dereferenceable guest address.
 *
 * This was previously a documented, deliberately-unattempted gap (see
 * docs/phase1d_import_surface.md's "Deliberately not implemented"
 * section) because naively reserving a fixed slot in `PpcContext::mem`
 * risks silently colliding with the compile-time-assigned global/rodata
 * addresses `assign_global_addrs` hands out starting at 0x2000, or with
 * the stack (which starts at `sizeof(mem) - 256` and grows down).
 *
 * Real signatures confirmed against devkitPro/wut's
 * coreinit/memexpheap.h, coreinit/memfrmheap.h, coreinit/memheap.h --
 * important real-world detail those headers make clear: `MEMHeapHandle`
 * *is* the caller-supplied `heap` pointer itself (the real MEMExpHeap/
 * MEMFrmHeap struct lives at that address on real hardware), and real
 * game code always supplies its own `heap`/`size` -- either a static
 * buffer it owns, or memory carved from a heap it already got from
 * `MEMGetBaseHeapHandle`. So the *game* is the one handing us real
 * addresses to sub-allocate within; the only address range this shim
 * itself needs to reserve is for the two base heaps (MEM1/MEM2)
 * `MEMGetBaseHeapHandle` lazily creates on first call, standing in for
 * what real Cafe OS pre-creates at boot.
 *
 * That reservation is a fixed, generous placeholder (12KB each for
 * MEM1/MEM2, deliberately well clear of both the 0x2000+ global range
 * and the stack-top region) -- the same explicitly-accepted trade-off
 * `assign_global_addrs` already documents for itself ("plenty for this
 * milestone's scope... a real, documented limitation"). It will need
 * real re-architecture once this runtime's memory model grows to
 * actual Wii U game scale (this project's whole `PpcContext::mem` is
 * still a small fixed 64KB scratch region sized for tiny test
 * programs, not a real console-scale address space) -- tracked here,
 * not silently assumed to already be solved.
 *
 * Allocator model: a simple host-side (not guest-memory) table of
 * {base, size, bump} per live heap handle, bump-allocating forward
 * within [base, base+size) and never reclaiming individual frees
 * (`MEMFreeToExpHeap` is a documented no-op/leak -- real code that
 * allocates and frees in a long-running loop expecting space to be
 * reclaimed will exhaust the heap sooner than on real hardware, but
 * nothing corrupts or crashes; this matches the honesty standard
 * already used for e.g. `OSTryWaitSemaphore`'s count-tracking gap).
 * `MEMFreeToFrmHeap`, matching real Frame Heap semantics, *does* do a
 * real bulk reset (`FREE_HEAD`/`FREE_ALL` resets the whole heap back
 * to empty) since that's how real code actually uses a frame heap
 * (allocate a batch, free the whole batch at once) -- `FREE_TAIL`
 * alone is a no-op since this allocator only ever grows in one
 * direction, a known simplification.
 *
 * `MEMAllocFromExpHeapEx` writes a private 4-byte size header
 * immediately before each returned block so `MEMGetSizeForMBlockExpHeap`
 * (which per its real signature takes *only* the block pointer, no
 * heap handle) can recover it -- this is an internal convention for
 * this shim only, not real `MEMExpHeapBlock`'s actual byte layout,
 * which is fine since nothing outside this shim ever produces or reads
 * these block pointers.
 */

typedef struct BrambleMemHeap {
    uint32_t base;
    uint32_t size;
    uint32_t bump;
    int active;
} BrambleMemHeap;

#define BRAMBLE_MEM_MAX_HEAPS 8
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern BrambleMemHeap g_bramble_mem_heaps[BRAMBLE_MEM_MAX_HEAPS];

/* MEM_BASE_HEAP_MEM1=0, MEM_BASE_HEAP_MEM2=1, MEM_BASE_HEAP_FG=8 -- see
 * coreinit/memheap.h's MEMBaseHeapType. Indexed [0]=MEM1, [1]=MEM2,
 * [2]=FG; 0 means "not yet lazily created". */
extern uint32_t g_bramble_base_heap_handle[3];

/* Real layout, found necessary the hard way: these addresses used to sit
 * at 0x8000/0xB000/0x2000 -- fine as "well clear of both the 0x2000+
 * global range and the stack-top region" *only* for the tiny test
 * programs this was originally sized for. Against the real, complete
 * Skylanders: Spyro's Adventure binary, `assign_global_addrs`
 * (elf_loader.cpp) actually lays real game globals out from 0x2000 all
 * the way to 0x670b00 (~6.75MB, confirmed by direct instrumentation) --
 * so those old fixed addresses sat *inside* the real game's own global
 * variables, and every real MEM1/MEM2 allocation was silently
 * corrupting them (and vice versa). See ppc_runtime.h's own
 * PPC_MEM_SIZE comment for the matching guest-address-space size
 * increase this real bug also required. New layout, with real headroom
 * past that measured 0x670b00 global extent:
 *   0x000000 - 0x700000  (paired 1:1 with the real 0x2000-0x670b00
 *                          global range, plus safety margin) -- globals
 *   0x800000 - 0x802000  FG (foreground/overlay) arena -- tiny, rarely used
 *   0x802000             __gh_errno_ptr's real backing address
 *   0x1000000 - 0x2000000  MEM1 (16MB)
 *   0x2000000 - 0x6000000  MEM2 (64MB) -- real code's main/largest heap
 *   0x6000000 - 0x8000000  unreserved -- real guest stack space (see
 *                          switch/game/source/main.c's own r[1] init)
 * Still real, chosen-for-this-specific-game placeholders, not a claim
 * this matches real Wii U MEM1/MEM2 scale (which is far larger) --
 * grounded in this real binary's own measured global-region size,
 * unlike the old values which were never checked against it. */
#define BRAMBLE_FG_BASE    0x800000u
#define BRAMBLE_FG_SIZE    0x2000u
#define BRAMBLE_ERRNO_ADDR 0x802000u
#define BRAMBLE_MEM1_BASE 0x1000000u
#define BRAMBLE_MEM1_SIZE 0x1000000u
#define BRAMBLE_MEM2_BASE 0x2000000u
#define BRAMBLE_MEM2_SIZE 0x4000000u

static inline BrambleMemHeap *bramble_mem_heap_find(uint32_t handle) {
    int i;
    for (i = 0; i < BRAMBLE_MEM_MAX_HEAPS; i++) {
        if (g_bramble_mem_heaps[i].active && g_bramble_mem_heaps[i].base == handle) {
            return &g_bramble_mem_heaps[i];
        }
    }
    return NULL;
}

static inline uint32_t bramble_mem_heap_create(uint32_t base, uint32_t size) {
    int i;
    for (i = 0; i < BRAMBLE_MEM_MAX_HEAPS; i++) {
        if (!g_bramble_mem_heaps[i].active) {
            g_bramble_mem_heaps[i].base = base;
            g_bramble_mem_heaps[i].size = size;
            g_bramble_mem_heaps[i].bump = base;
            g_bramble_mem_heaps[i].active = 1;
            return base;
        }
    }
    return 0; /* out of heap table slots */
}

static inline void bramble_mem_heap_destroy(uint32_t handle) {
    BrambleMemHeap *h = bramble_mem_heap_find(handle);
    if (h != NULL) h->active = 0;
}

static inline uint32_t bramble_align_up(uint32_t v, uint32_t align) {
    if (align < 4) align = 4;
    return (v + (align - 1)) & ~(align - 1);
}

/* Real, optional logging hook -- fires on a real MEMCreateExpHeapEx or
 * MEMAllocFromExpHeapEx failure only (not every call, which would be far
 * too frequent to be useful -- see cafeos_coreinit_fs.h's own FSOpenFile
 * log for the same reasoning, applied here to answer a different real
 * question: is the real game's own memory-pool code spinning because of
 * genuine, real out-of-memory against this shim's fixed-size heaps, or
 * something else). `extern`, not `static`, for the same real multi-
 * translation-unit reason as everything else in this project -- shared,
 * single definition lives in cafeos_state.c. */
typedef void (*ppc_mem_alloc_fail_log_fn)(const char *what, uint32_t requested, uint32_t heap_base, uint32_t heap_size, uint32_t heap_used);
extern ppc_mem_alloc_fail_log_fn g_ppc_mem_alloc_fail_log; /* real definition in cafeos_state.c -- see its own file comment */
static inline void ppc_mem_set_alloc_fail_log(ppc_mem_alloc_fail_log_fn fn) { g_ppc_mem_alloc_fail_log = fn; }

static inline void ppc_import_coreinit_MEMCreateExpHeapEx(PpcContext *ctx) {
    /* MEMHeapHandle MEMCreateExpHeapEx(void *heap, uint32_t size, uint16_t flags) */
    uint32_t result = bramble_mem_heap_create(ctx->r[3], ctx->r[4]);
    /* Always logged (success or fail), not throttled the way real-alloc
     * failures are -- real heap creation is naturally rare (at most a
     * handful of real calls for the whole game), unlike real individual
     * allocations, so there's no real flood risk. Answers a real,
     * specific question: does the real game ever actually create the
     * heap whose handle a later real allocation call fails against, and
     * if so, in what order relative to that failure. */
    if (g_ppc_mem_alloc_fail_log) {
        g_ppc_mem_alloc_fail_log(result == 0 ? "MEMCreateExpHeapEx FAILED (out of heap table slots)" : "MEMCreateExpHeapEx ok",
                                  ctx->r[4], ctx->r[3], result, 0);
    }
    ctx->r[3] = result;
}

static inline void ppc_import_coreinit_MEMDestroyExpHeap(PpcContext *ctx) {
    /* void *MEMDestroyExpHeap(MEMHeapHandle heap) -- returns the same pointer */
    uint32_t handle = ctx->r[3];
    bramble_mem_heap_destroy(handle);
    ctx->r[3] = handle;
}

static inline void ppc_import_coreinit_MEMAllocFromExpHeapEx(PpcContext *ctx) {
    /* void *MEMAllocFromExpHeapEx(MEMHeapHandle heap, uint32_t size, int alignment) */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t size = ctx->r[4];
    int32_t alignment = (int32_t)ctx->r[5];
    uint32_t align = alignment < 0 ? (uint32_t)(-alignment) : (uint32_t)alignment;
    uint32_t addr, end;
    if (h == NULL) {
        if (g_ppc_mem_alloc_fail_log) g_ppc_mem_alloc_fail_log("MEMAllocFromExpHeapEx (unknown heap handle)", size, ctx->r[3], 0, 0);
        ctx->r[3] = 0;
        return;
    }
    addr = bramble_align_up(h->bump + 4, align); /* +4: room for the private size header */
    end = addr + size;
    if (end > h->base + h->size) { /* real OOM behavior: NULL */
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMAllocFromExpHeapEx (out of space)", size, h->base, h->size, h->bump - h->base);
        }
        ctx->r[3] = 0;
        return;
    }
    ppc_store_u32(ctx, addr - 4, size);
    h->bump = end;
    ctx->r[3] = addr;
}

static inline void ppc_import_coreinit_MEMFreeToExpHeap(PpcContext *ctx) { (void)ctx; /* documented leak -- see file comment */ }

static inline void ppc_import_coreinit_MEMGetAllocatableSizeForExpHeapEx(PpcContext *ctx) {
    /* uint32_t MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle heap, int alignment) */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t end;
    if (h == NULL) { ctx->r[3] = 0; return; }
    end = h->base + h->size;
    ctx->r[3] = (end > h->bump + 4) ? (end - h->bump - 4) : 0;
}

static inline void ppc_import_coreinit_MEMGetSizeForMBlockExpHeap(PpcContext *ctx) {
    /* uint32_t MEMGetSizeForMBlockExpHeap(void *block) -- no heap handle, size
     * lives in the private header this shim writes at block-4. */
    ctx->r[3] = ppc_load_u32(ctx, ctx->r[3] - 4);
}

static inline void ppc_import_coreinit_MEMAllocFromFrmHeapEx(PpcContext *ctx) {
    /* void *MEMAllocFromFrmHeapEx(MEMHeapHandle heap, uint32_t size, int alignment) */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t size = ctx->r[4];
    int32_t alignment = (int32_t)ctx->r[5];
    uint32_t align = alignment < 0 ? (uint32_t)(-alignment) : (uint32_t)alignment;
    uint32_t addr, end;
    if (h == NULL) { ctx->r[3] = 0; return; }
    addr = bramble_align_up(h->bump, align); /* no private header needed -- Frm blocks are never size-queried */
    end = addr + size;
    if (end > h->base + h->size) { ctx->r[3] = 0; return; }
    h->bump = end;
    ctx->r[3] = addr;
}

static inline void ppc_import_coreinit_MEMFreeToFrmHeap(PpcContext *ctx) {
    /* void MEMFreeToFrmHeap(MEMHeapHandle heap, MEMFrmHeapFreeMode mode) --
     * MEM_FRM_HEAP_FREE_HEAD=1, _TAIL=2, _ALL=3. This allocator only ever
     * grows from the head, so HEAD/ALL both mean "reset the whole heap"; a
     * TAIL-only free (2) is a real no-op here (nothing was ever allocated
     * from the tail direction), a known simplification. */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t mode = ctx->r[4];
    if (h != NULL && (mode & 1u) != 0) {
        h->bump = h->base;
    }
}

static inline void ppc_import_coreinit_MEMGetAllocatableSizeForFrmHeapEx(PpcContext *ctx) {
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t end;
    if (h == NULL) { ctx->r[3] = 0; return; }
    end = h->base + h->size;
    ctx->r[3] = (end > h->bump) ? (end - h->bump) : 0;
}

static inline void ppc_import_coreinit_MEMGetBaseHeapHandle(PpcContext *ctx) {
    /* MEMHeapHandle MEMGetBaseHeapHandle(MEMBaseHeapType type) -- real Cafe
     * OS pre-creates these at boot (MEM1 as a real frame heap, MEM2 as a
     * real expanded heap); this shim lazily creates the equivalent stand-in
     * on first call instead, since there's no real boot sequence here. */
    uint32_t type = ctx->r[3];
    int idx = (type == 0) ? 0 : (type == 1) ? 1 : 2;
    if (g_bramble_base_heap_handle[idx] == 0) {
        uint32_t base = (idx == 0) ? BRAMBLE_MEM1_BASE : (idx == 1) ? BRAMBLE_MEM2_BASE : BRAMBLE_FG_BASE;
        uint32_t size = (idx == 0) ? BRAMBLE_MEM1_SIZE : (idx == 1) ? BRAMBLE_MEM2_SIZE : BRAMBLE_FG_SIZE;
        g_bramble_base_heap_handle[idx] = bramble_mem_heap_create(base, size);
        /* Rare (at most 3 real calls total, one per base heap type) --
         * see MEMCreateExpHeapEx's own comment on why this is safe to
         * always log. */
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMGetBaseHeapHandle lazy-create", type, base, size, g_bramble_base_heap_handle[idx]);
        }
    }
    ctx->r[3] = g_bramble_base_heap_handle[idx];
}

/*
 * __gh_errno_ptr/__gh_set_errno: Green Hills libc's errno accessor pair
 * (real Wii U retail code is GHS-compiled -- see cafeos_ghs_runtime.h).
 * Needs one real, stable, persistent guest address -- a fixed reserved
 * slot, same documented-placeholder trade-off as the base heaps above.
 */
static inline void ppc_import_coreinit___gh_errno_ptr(PpcContext *ctx) { ctx->r[3] = BRAMBLE_ERRNO_ADDR; }
static inline void ppc_import_coreinit___gh_set_errno(PpcContext *ctx) {
    ppc_store_u32(ctx, BRAMBLE_ERRNO_ADDR, ctx->r[3]);
}

#endif /* BRAMBLE_CAFEOS_COREINIT_MEM_H */
