#ifndef BRAMBLE_CAFEOS_COREINIT_MEM_H
#define BRAMBLE_CAFEOS_COREINIT_MEM_H

#include "ppc_runtime.h"
#include <stdlib.h>

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
 * within [base, base+size) for anything the free list below can't
 * satisfy.
 *
 * `MEMFreeToExpHeap` used to be an unconditional no-op/leak. Real,
 * confirmed hardware consequence found the hard way in an actual ten-
 * minute run: `Core::igStringPoolContainer::mallocString` (real game
 * code, not this shim) allocates and frees from this exact heap in a
 * loop as it grows the string pool, and with frees doing nothing the
 * 16MB MEM1 heap was already sitting at ~16.65MB "used" purely from the
 * real static initializers before the game's own entry point had even
 * started (see the `[MEM EVENT] MEMAllocFromExpHeapEx (out of space)`
 * log lines this shim's own fail-log hook already produces) -- so every
 * later real allocation attempt failed forever, and mallocString spun
 * calling `Core::igMemoryPool::malloc`/`getMemoryPool` for over a
 * billion real calls without the game ever making forward progress.
 * Fixed with a real, if simple, first-fit free list per heap (host
 * `malloc`/`free` for the list nodes themselves, which live entirely on
 * this shim's own side, never in guest memory): `MEMFreeToExpHeap`
 * pushes the freed block onto its heap's list using the size already
 * recorded in the private header below; `MEMAllocFromExpHeapEx` checks
 * that list before falling back to the bump allocator. Freed blocks are
 * reused whole, not split, and adjacent free blocks are never coalesced
 * -- real fragmentation is possible this way, but that is a correctness
 * trade-off, not the total, unconditional leak this shim shipped with
 * before; matches the honesty standard already used for e.g.
 * `OSTryWaitSemaphore`'s count-tracking gap.
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

/* One freed block, host-side only -- addr/size describe the *header*
 * (block-4) through the end of the payload, i.e. exactly what
 * MEMFreeToExpHeap's block pointer minus 4, plus the size recovered
 * from that header, spans. Reused whole on the next matching alloc. */
typedef struct BrambleFreeBlock {
    uint32_t addr;
    uint32_t size;
    struct BrambleFreeBlock *next;
} BrambleFreeBlock;

typedef struct BrambleMemHeap {
    uint32_t base;
    uint32_t size;
    uint32_t bump;
    int active;
    BrambleFreeBlock *free_list;
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
 *   0x1000000 - 0x3000000  MEM1 (32MB)
 *   0x3000000 - 0x7000000  MEM2 (64MB) -- real code's main/largest heap
 *   0x7000000 - 0x9000000  unreserved -- real guest stack space (see
 *                          switch/game/source/main.c's own r[1] init)
 *   0x9000000 - 0x10000000 unreserved headroom (real, spare, from the
 *                          256MB bump -- see ppc_runtime.h's own
 *                          PPC_MEM_SIZE comment)
 * MEM1 was widened 16MB -> 32MB on 2026-08-21: real hardware logs
 * showed the game's own boot-time MEMAllocFromExpHeapEx call spinning
 * forever on a 128KB request against this exact heap, which was
 * already sitting at ~16.65MB used out of a 16MB total -- a genuine,
 * permanent out-of-space condition, not a pointer/context bug. 32MB
 * matches real Wii U MEM1's actual real size (the old 16MB value was
 * never meant to claim that, see below); MEM2/stack now otherwise
 * unchanged in size, just shifted up by MEM1's extra 16MB. Still real,
 * chosen-for-this-specific-game placeholders otherwise, not a claim
 * MEM2 matches real Wii U MEM2 scale (which is far larger) -- grounded
 * in this real binary's own measured global-region size and this one
 * confirmed real allocation failure, not picked blind. */
#define BRAMBLE_FG_BASE    0x800000u
#define BRAMBLE_FG_SIZE    0x2000u
#define BRAMBLE_ERRNO_ADDR 0x802000u
/* Real, targeted fix added 2026-08-21 after a full real hardware trace
 * (see main.c's own comment trail) found the true root cause of the
 * boot-time igStringPool spin: real engine code has a small, dedicated
 * "bootstrap heap" whose handle lives in a real global at synthetic
 * address 421248u (&.bss+306320) -- read from many real places across
 * early engine code (Core::igMemoryContext's own constructor among
 * them), but never once written anywhere in this project's currently
 * recompiled output. Whatever real function calls MEMCreateExpHeapEx
 * and stores its result there either isn't reached yet or wasn't
 * recovered by this project's own function-boundary-recovery pass --
 * either way, with the handle stuck at its zero-initialized default,
 * the very first real object allocation in the whole engine
 * (Core::igMemoryContext's own bootstrap instance) fails, and every
 * single downstream symptom traced this session (the null vtable read,
 * userInstantiate never running, the "current memory context" global
 * staying unset, the infinite igStringPoolContainer::reserveMemory
 * retry) is a direct, confirmed consequence of just this one missing
 * heap. Rather than try to recover the real missing function, this
 * creates the same real heap directly from this project's own boot
 * shim (see bramble_mem_bootstrap_heap_init below, called once from
 * main.c before the real game entry point runs) and writes its handle
 * into that exact real global -- functionally identical to what the
 * real missing function would have done. Small (64KB), carved out of
 * the real unused gap between BRAMBLE_ERRNO_ADDR and BRAMBLE_MEM1_BASE
 * above -- real observed allocations through this heap so far are tiny
 * (52 and 100 bytes), matching a genuine bootstrap-only heap, not a
 * general-purpose one. */
#define BRAMBLE_BOOTSTRAP_HEAP_HANDLE_ADDR 421248u
#define BRAMBLE_BOOTSTRAP_HEAP_BASE 0x810000u
#define BRAMBLE_BOOTSTRAP_HEAP_SIZE 0x10000u
/* Real, decisive diagnostic result as of 2026-08-21: MEM1 was doubled
 * twice (16MB -> 32MB -> 64MB) chasing the real boot-time sbrk/malloc
 * spin, and the real hardware log showed the fill ratio *climbing
 * toward 100%* each time (99.27% -> 99.66% -> 99.85%) instead of
 * settling -- real proof this is unbounded growth, not an undersized
 * fixed need. Confirmed why: the actual spin (Core::igStringPoolContainer
 * ::reserveMemory's own pool-list loop, real vaddr 0x21a4f9c-0x21a5024)
 * calls Core::igObject::getMemoryPool() -- the same function found at
 * the very start of this investigation that unconditionally reads the
 * real "current memory context" global (.data+5336, only ever written
 * by Core::igMemoryContext::userInstantiate, which real hardware logs
 * confirm never runs) -- so every pool it gets back is degenerate and
 * every malloc(28) against it fails, forever, regardless of how much
 * real heap this shim provides. The real fix is upstream of MEM1
 * entirely; see main.c's own comment trail for that investigation.
 * Restored to the real, Wii-U-accurate 32MB value (still a genuine
 * correctness improvement over the old undersized 16MB placeholder,
 * just not what was causing this specific hang). */
#define BRAMBLE_MEM1_BASE 0x1000000u
#define BRAMBLE_MEM1_SIZE 0x2000000u
#define BRAMBLE_MEM2_BASE 0x3000000u
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

/* Frees every host-side free-list node for a heap slot (not the guest
 * memory it describes, which belongs to the heap itself) -- needed
 * both on real destroy and before reusing a table slot for a new heap,
 * so a stale list from a previous heap that lived in this same slot
 * never leaks host memory or gets misread as the new heap's own list. */
static inline void bramble_mem_heap_clear_free_list(BrambleMemHeap *h) {
    BrambleFreeBlock *n = h->free_list;
    while (n != NULL) {
        BrambleFreeBlock *next = n->next;
        free(n);
        n = next;
    }
    h->free_list = NULL;
}

static inline uint32_t bramble_mem_heap_create(uint32_t base, uint32_t size) {
    int i;
    for (i = 0; i < BRAMBLE_MEM_MAX_HEAPS; i++) {
        if (!g_bramble_mem_heaps[i].active) {
            bramble_mem_heap_clear_free_list(&g_bramble_mem_heaps[i]);
            g_bramble_mem_heaps[i].base = base;
            g_bramble_mem_heaps[i].size = size;
            g_bramble_mem_heaps[i].bump = base;
            g_bramble_mem_heaps[i].active = 1;
            return base;
        }
    }
    return 0; /* out of heap table slots */
}

/* Real, targeted fix -- see BRAMBLE_BOOTSTRAP_HEAP_HANDLE_ADDR's own
 * comment above for the full real hardware-traced explanation. Call
 * this once, from the boot shim, before the real game entry point runs
 * -- doing exactly what the real (currently unrecovered/unreached)
 * game function would have done: create the heap, store its handle
 * into the real global every early allocator reads it from. */
static inline void bramble_mem_bootstrap_heap_init(PpcContext *ctx) {
    uint32_t handle = bramble_mem_heap_create(BRAMBLE_BOOTSTRAP_HEAP_BASE, BRAMBLE_BOOTSTRAP_HEAP_SIZE);
    ppc_store_u32(ctx, BRAMBLE_BOOTSTRAP_HEAP_HANDLE_ADDR, handle);
}

static inline void bramble_mem_heap_destroy(uint32_t handle) {
    BrambleMemHeap *h = bramble_mem_heap_find(handle);
    if (h != NULL) {
        bramble_mem_heap_clear_free_list(h);
        h->active = 0;
    }
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

/* Unthrottled running totals, real definitions in cafeos_state.c -- the
 * log hook above stops printing individual events after the first 40 to
 * avoid flooding the SD card on a genuine spin loop, which leaves no way
 * to tell from the log alone whether a long-running stall billions of
 * calls later is *still* hitting real ExpHeap exhaustion, or whether
 * MEMFreeToExpHeap is even being reached at all. These two counters
 * answer that cheaply -- main.c prints them in its own periodic status
 * line instead of a per-event log line. */
extern uint64_t g_bramble_mem_alloc_fail_total;
extern uint64_t g_bramble_mem_free_total;
extern uint64_t g_bramble_mem_reuse_total;

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

    /* First-fit against previously-freed blocks before falling back to
     * the bump allocator below -- see this file's own top comment for
     * why this matters (a real, confirmed hang without it). Not split
     * on partial use and not coalesced with neighbours; just whole-block
     * reuse, which is enough to stop the heap only ever growing. */
    {
        BrambleFreeBlock **pp = &h->free_list;
        while (*pp != NULL) {
            BrambleFreeBlock *n = *pp;
            uint32_t candidate = bramble_align_up(n->addr + 4, align);
            if (candidate + size <= n->addr + n->size) {
                *pp = n->next;
                free(n);
                ppc_store_u32(ctx, candidate - 4, size);
                ctx->r[3] = candidate;
                g_bramble_mem_reuse_total++;
                return;
            }
            pp = &n->next;
        }
    }

    addr = bramble_align_up(h->bump + 4, align); /* +4: room for the private size header */
    end = addr + size;
    if (end > h->base + h->size) { /* real OOM behavior: NULL */
        g_bramble_mem_alloc_fail_total++;
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMAllocFromExpHeapEx (out of space)", size, h->base, h->size, h->bump - h->base);
        }
        ctx->r[3] = 0;
        return;
    }
    ppc_store_u32(ctx, addr - 4, size);
    h->bump = end;
    ctx->r[3] = addr;
    /* Real, deliberate instrumentation added 2026-08-21 chasing why
     * doubling MEM1's real size (16MB -> 32MB, see this file's own
     * layout comment) didn't fix the real boot-time sbrk/malloc spin --
     * the heap filled to the same ~99.5% ratio either way, which only
     * happens if whatever's consuming it scales with how much room it's
     * given, not a fixed real need. Logging every real single allocation
     * of 64KB+ from this heap (success, not just failure) answers the
     * real question directly: which real call is responsible for that
     * scaling growth. */
    if (size >= 65536u && g_ppc_mem_alloc_fail_log) {
        g_ppc_mem_alloc_fail_log("MEMAllocFromExpHeapEx (large alloc)", size, h->base, h->size, h->bump - h->base);
    }
}

static inline void ppc_import_coreinit_MEMFreeToExpHeap(PpcContext *ctx) {
    /* void MEMFreeToExpHeap(MEMHeapHandle heap, void *block) -- real
     * reclamation now (see this file's own top comment for why this
     * used to be, and no longer is, an unconditional no-op). A NULL
     * block (real code's own "freeing NULL is a no-op" convention) and
     * an unknown heap handle are both silently ignored, same honesty
     * standard as the rest of this shim's real-hardware-matching
     * no-crash-on-bad-input behaviour. */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t block = ctx->r[4];
    BrambleFreeBlock *node;
    uint32_t size;
    if (h == NULL || block == 0) return;
    size = ppc_load_u32(ctx, block - 4);
    node = (BrambleFreeBlock *)malloc(sizeof(BrambleFreeBlock));
    if (node == NULL) return; /* host allocation failure -- block just stays leaked, no crash */
    node->addr = block - 4;
    node->size = size + 4;
    node->next = h->free_list;
    h->free_list = node;
    g_bramble_mem_free_total++;
}

static inline void ppc_import_coreinit_MEMGetAllocatableSizeForExpHeapEx(PpcContext *ctx) {
    /* uint32_t MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle heap, int alignment) */
    BrambleMemHeap *h = bramble_mem_heap_find(ctx->r[3]);
    uint32_t end;
    uint32_t result;
    if (h == NULL) { ctx->r[3] = 0; return; }
    end = h->base + h->size;
    result = (end > h->bump + 4) ? (end - h->bump - 4) : 0;
    ctx->r[3] = result;
    /* Real instrumentation added 2026-08-21 alongside the large-alloc
     * log above -- this is the real "how much room is left" query a
     * real percentage-of-remaining-heap sizing pattern would call right
     * before making one real big allocation for a pool/cache. Always
     * logged (real calls to this are naturally rare), `requested` field
     * repurposed to carry the real returned remaining-size value since
     * this isn't itself a real allocation request. */
    if (g_ppc_mem_alloc_fail_log) {
        g_ppc_mem_alloc_fail_log("MEMGetAllocatableSizeForExpHeapEx (query)", result, h->base, h->size, h->bump - h->base);
    }
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

/* Real Cafe OS convenience wrappers around the real default (MEM1) app
 * heap -- confirmed real and not hypothetical: Green Hills libc's own
 * real sbrk() calls through MEMAllocFromDefaultHeapEx specifically (via
 * a real Cafe OS "data import" -- see recomp's DataImport/
 * resolve_data_imports for the mechanism that makes an indirect call
 * through this real symbol route here at all) whenever malloc's own
 * free list can't satisfy a request and needs to grow the heap.
 * MEM1 is lazily created the same way MEMGetBaseHeapHandle already
 * does for it, since this can legitimately be the very first real call
 * that needs it -- confirmed real signatures against devkitPro/wut's
 * coreinit/memdefaultheap.h. */
static inline void ppc_import_coreinit_MEMAllocFromDefaultHeapEx(PpcContext *ctx) {
    /* void *MEMAllocFromDefaultHeapEx(uint32_t size, int alignment) */
    uint32_t size = ctx->r[3];
    int32_t alignment = (int32_t)ctx->r[4];
    if (g_bramble_base_heap_handle[0] == 0) {
        g_bramble_base_heap_handle[0] = bramble_mem_heap_create(BRAMBLE_MEM1_BASE, BRAMBLE_MEM1_SIZE);
    }
    ctx->r[3] = g_bramble_base_heap_handle[0];
    ctx->r[4] = size;
    ctx->r[5] = (uint32_t)alignment;
    ppc_import_coreinit_MEMAllocFromExpHeapEx(ctx);
}

static inline void ppc_import_coreinit_MEMAllocFromDefaultHeap(PpcContext *ctx) {
    /* void *MEMAllocFromDefaultHeap(uint32_t size) -- same as Ex with a
     * real, fixed default alignment (4 bytes). */
    uint32_t size = ctx->r[3];
    ctx->r[3] = size;
    ctx->r[4] = 4;
    ppc_import_coreinit_MEMAllocFromDefaultHeapEx(ctx);
}

static inline void ppc_import_coreinit_MEMFreeToDefaultHeap(PpcContext *ctx) {
    /* void MEMFreeToDefaultHeap(void *ptr) -- MEM1-backed default heap,
     * same real reclamation as MEMFreeToExpHeap now (see its own
     * comment); this is just that same call with the lazily-created
     * MEM1 handle filled in, matching MEMAllocFromDefaultHeapEx's own
     * pattern above. */
    uint32_t ptr = ctx->r[3];
    if (g_bramble_base_heap_handle[0] == 0) {
        g_bramble_base_heap_handle[0] = bramble_mem_heap_create(BRAMBLE_MEM1_BASE, BRAMBLE_MEM1_SIZE);
    }
    ctx->r[3] = g_bramble_base_heap_handle[0];
    ctx->r[4] = ptr;
    ppc_import_coreinit_MEMFreeToExpHeap(ctx);
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
