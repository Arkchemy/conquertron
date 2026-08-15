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
static BrambleMemHeap g_bramble_mem_heaps[BRAMBLE_MEM_MAX_HEAPS];

/* MEM_BASE_HEAP_MEM1=0, MEM_BASE_HEAP_MEM2=1, MEM_BASE_HEAP_FG=8 -- see
 * coreinit/memheap.h's MEMBaseHeapType. Indexed [0]=MEM1, [1]=MEM2,
 * [2]=FG; 0 means "not yet lazily created". */
static uint32_t g_bramble_base_heap_handle[3];

#define BRAMBLE_MEM1_BASE 0x8000u
#define BRAMBLE_MEM1_SIZE 0x3000u
#define BRAMBLE_MEM2_BASE 0xB000u
#define BRAMBLE_MEM2_SIZE 0x3000u
#define BRAMBLE_FG_BASE   0x2000u /* FG (foreground/overlay) arena: tiny, rarely used real region */
#define BRAMBLE_FG_SIZE   0x0800u
#define BRAMBLE_ERRNO_ADDR 0xE000u

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

static inline void ppc_import_coreinit_MEMCreateExpHeapEx(PpcContext *ctx) {
    /* MEMHeapHandle MEMCreateExpHeapEx(void *heap, uint32_t size, uint16_t flags) */
    ctx->r[3] = bramble_mem_heap_create(ctx->r[3], ctx->r[4]);
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
    if (h == NULL) { ctx->r[3] = 0; return; }
    addr = bramble_align_up(h->bump + 4, align); /* +4: room for the private size header */
    end = addr + size;
    if (end > h->base + h->size) { ctx->r[3] = 0; return; } /* real OOM behavior: NULL */
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
