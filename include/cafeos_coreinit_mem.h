#ifndef ARKCHEMY_CAFEOS_COREINIT_MEM_H
#define ARKCHEMY_CAFEOS_COREINIT_MEM_H

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
typedef struct ArkchemyFreeBlock {
    uint32_t addr;
    uint32_t size;
    struct ArkchemyFreeBlock *next;
} ArkchemyFreeBlock;

typedef struct ArkchemyMemHeap {
    uint32_t base;
    uint32_t size;
    uint32_t bump;
    int active;
    ArkchemyFreeBlock *free_list;
} ArkchemyMemHeap;

#define ARKCHEMY_MEM_MAX_HEAPS 8
/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyMemHeap g_arkchemy_mem_heaps[ARKCHEMY_MEM_MAX_HEAPS];

/* MEM_BASE_HEAP_MEM1=0, MEM_BASE_HEAP_MEM2=1, MEM_BASE_HEAP_FG=8 -- see
 * coreinit/memheap.h's MEMBaseHeapType. Indexed [0]=MEM1, [1]=MEM2,
 * [2]=FG; 0 means "not yet lazily created". */
extern uint32_t g_arkchemy_base_heap_handle[3];

/* Bootstrap heap handle, kept host-side so it survives the game clearing
 * its own .bss (see MEMAllocFromExpHeapEx's null-handle path). */
#ifdef __GNUC__
__attribute__((weak))
#endif
uint32_t g_arkchemy_bootstrap_heap_handle = 0;

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
 *   0x1000000 - 0x3000000  MEM1 (32MB) -- matches real Wii U MEM1; no
 *                          longer backs the default heap, see
 *                          MEMAllocFromDefaultHeapEx below
 *   0x3000000 - 0x4000000  bootstrap heap (16MB)
 *   0x4000000 - 0x3E000000 MEM2 (928MB) -- the app default heap, and so
 *                          the backing for the game's whole C heap via
 *                          Green Hills libc sbrk(). Sized after 96MB was
 *                          measured to run out during static init alone,
 *                          then 320MB filled to 99.97%. Grown here by
 *                          reclaiming the ~96MB of address space that
 *                          sat unused between MEM1 and the old MEM2
 *                          base, rather than by enlarging BSS again --
 *                          failures were measured to plateau (3,099,
 *                          flat for the last 116 checkpoints of a run),
 *                          so this is a bounded shortfall, not a leak.
 *   0x3E000000 - 0x40000000 guest stack (32MB), growing DOWN from
 *                          PPC_MEM_SIZE-256. MEM2 deliberately stops
 *                          short of this: sizing MEM2 to the top of the
 *                          address space would put the stack inside the
 *                          heap and corrupt it silently.
 * Layout updated 2026-08-24; the three regions above that moved are the
 * bootstrap heap and MEM2, both for reasons recorded at their own
 * defines. Keep this map in step with those defines -- a stale copy of
 * it is worse than none, since it is the first thing anyone reads.
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
#define ARKCHEMY_FG_BASE    0x800000u
#define ARKCHEMY_FG_SIZE    0x2000u
#define ARKCHEMY_ERRNO_ADDR 0x802000u
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
 * shim (see arkchemy_mem_bootstrap_heap_init below, called once from
 * main.c before the real game entry point runs) and writes its handle
 * into that exact real global -- functionally identical to what the
 * real missing function would have done. Small (64KB), carved out of
 * the real unused gap between ARKCHEMY_ERRNO_ADDR and ARKCHEMY_MEM1_BASE
 * above -- real observed allocations through this heap so far are tiny
 * (52 and 100 bytes), matching a genuine bootstrap-only heap, not a
 * general-purpose one.
 *
 * Real correction, 2026-08-24: "bootstrap-only" turned out to be wrong.
 * Traced a real, confirmed hang (Core::igMemoryPool::allocatePoolMemory
 * on the real pool at 0x810184 never getting its buffer backed, real
 * args this=0x810184 sourcePool=0x810128 size=0x100000) all the way
 * down through Core::igMemoryPool::reallocCommon -> a real virtual
 * dispatch into Core::igCafeSystemMemoryPool::reallocInternal ->
 * Core::igCafeSystemMemoryPool::mallocInternal -> a real
 * MEMAllocFromExpHeapEx call requesting ~1MB against whatever heap
 * handle lives at *(sourcePool+0x10). Real hardware's own
 * MEMGetAllocatableSizeForExpHeapEx query log (called from
 * igCafeSystemMemoryPool::activate itself, real addr 0x2156ccc)
 * confirmed that handle really is heap_base=0x810000 -- this exact
 * bootstrap heap, not MEM1/MEM2. So this comment's own earlier
 * "confirmed... genuine bootstrap-only heap" claim was based on the
 * *sizes* seen so far (52/100 bytes), not on tracing who else reads
 * this same handle -- real engine code (confirmed: this same header's
 * own note above already says this global is "read from many real
 * places across early engine code") also uses it as the backing heap
 * for at least one real igCafeSystemMemoryPool, which real Wii U
 * hardware would never have sized this small. 64KB can never satisfy a
 * real 1MB request no matter how little else uses this heap -- same
 * category of fix as MEM1's own two earlier real widenings above
 * (real logged shortfall, not speculative), just discovered on this
 * heap instead. Widened with real headroom above the one confirmed
 * 1MB need, same margin-over-minimum spirit as those MEM1 fixes;
 * still well clear of ARKCHEMY_MEM1_BASE below.
 *
 * Second real widening, 2026-08-24 (same day, later): 3MB was still not
 * enough. With the first fix in place, real hardware got far enough to
 * show `bootstrapInitialize` initialising *two* real pools out of this
 * same heap, not one -- pool id=0 (`0x810184`) really does succeed now
 * (real 1048584-byte carve-out, `tlsf_create` returned a real nonzero
 * handle, confirmed on hardware), but pool id=2 (`0x9101f4`) then asks
 * for 5242888 more and fails "out of space" with only ~2MB left. Both
 * numbers are real, logged shortfalls, not estimates: 1048584 + 5242888
 * = 6291472 bytes needed, against 3145728 provided. `igStringPool`'s own
 * activation then mallocs repeatedly against whichever pool it was
 * assigned, so a permanently-dead pool id=2 is a strong candidate for
 * the real stall observed right after that point. Sized to 7.5MB, which
 * covers both real requests with genuine headroom and still fits the
 * real address-space gap between this heap's base and ARKCHEMY_MEM1_BASE
 * below (0x1000000 - 0x810000 = 8323072 bytes available; 7864320 used,
 * leaving ~448KB clear).
 *
 * Third real widening AND a relocation, 2026-08-24: 7.5MB got pools
 * id=0 and id=2 both fully succeeding (real 1048584 and 5242888
 * carve-outs, both `tlsf_create` calls returning real nonzero handles,
 * confirmed on hardware) -- but then exposed a *third* real consumer
 * this heap has to serve: `initializeStringPool` (id=3) asks for
 * 2097160 more and failed "out of space" with ~1.5MB left. Real total
 * across all three: 1048584 + 5242888 + 2097160 = 8388632 bytes. That
 * no longer fits where this heap used to live -- the real gap between
 * the old base and ARKCHEMY_MEM1_BASE is only 8323072, genuinely 65560
 * bytes short, so this could not be fixed by another size bump alone.
 *
 * Confirmed real consequence of that shortfall, and why this matters:
 * with the string pool dead, execution reached
 * Core::igStringPoolContainer::reserveMemory (real addr 0x21a4e4c) and
 * spun there permanently -- the exact same real spin this project
 * chased back on 2026-08-21 (see ARKCHEMY_MEM1_SIZE's own comment
 * below, which correctly concluded the cause was upstream of MEM1;
 * this is that upstream cause, finally reached).
 *
 * Relocated into the real unreserved headroom this layout already
 * documents above (0x9000000 - 0x10000000, 112MB genuinely spare, well
 * clear of the guest stack region that ends at 0x9000000) rather than
 * shuffling globals/FG/errno/MEM1/MEM2 around each other. Sized 16MB:
 * comfortably past the 8388632 now actually measured, with real room
 * for the further consumers this heap has revealed three separate
 * times tonight, and still leaving 96MB of that headroom untouched. */
#define ARKCHEMY_BOOTSTRAP_HEAP_HANDLE_ADDR 421248u
#define ARKCHEMY_BOOTSTRAP_HEAP_BASE 0x3000000u
#define ARKCHEMY_BOOTSTRAP_HEAP_SIZE 0x1000000u
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
#define ARKCHEMY_MEM1_BASE 0x1000000u
#define ARKCHEMY_MEM1_SIZE 0x2000000u
/* MEM2 relocated and enlarged 2026-08-24. It used to be 64MB wedged
 * between MEM1 and the guest stack, which left no room to grow. It is
 * now the app's real default heap (see MEMAllocFromDefaultHeapEx below
 * for why that changed), so it has to absorb the game's entire C heap,
 * and the old 0x3000000-0x7000000 window could not be widened without
 * moving the stack. Moved into the 96MB of headroom this file's own
 * layout comment already documents as spare, immediately above the
 * bootstrap heap (0xA000000-0x10000000). The old window is simply left
 * unused. This is still far short of real Wii U MEM2 (2GB), but it is
 * three times what the game just exhausted, and the whole guest address
 * space is only 256MB. */
/* Moved from 0x4000000 on 2026-08-29. Data sections now live at their REAL
 * addresses rather than in a synthetic space, and the loaded image occupies
 * 0x02000020..0x10181290 -- .rodata at 0x10000000, .data at 0x100CBE40, .bss
 * at 0x100E6000. The old MEM2 window started at 0x4000000 and ran to
 * 0x3E000000, so it covered all of that: the heap would have handed out the
 * game's own globals as free memory. Starting above the image keeps 720MB,
 * still far more than anything observed. */
#define ARKCHEMY_MEM2_BASE 0x11000000u
#define ARKCHEMY_MEM2_SIZE 0x2D000000u
/* MEM2 is split in two, and both halves must agree on where the line is
 * or they will hand out the same memory twice. The low eighth is an
 * ordinary ExpHeap (headers, free list, reusable); the rest is the
 * strictly contiguous arena backing Green Hills libc's sbrk. Defined
 * once here so the ExpHeap bound and the arena start cannot drift
 * apart -- they were briefly inconsistent when the arena was added,
 * with the ExpHeap created over the FULL pool and therefore able to
 * bump straight into arena memory. */
/* Was MEM2_SIZE / 8, which is 116MB, and on 2026-08-29 the game asked this
 * heap for a single 180,355,080-byte block and got the project's first real
 * allocation failure. That request is not absurd -- real Wii U MEM2 is 2GB and
 * a 172MB engine pool is ordinary there -- it simply never used to be reached,
 * because before data relocations were applied the boot died long before any
 * allocation of that size. Half the pool leaves 464MB for the ExpHeap and
 * 464MB for the sbrk arena, both far beyond anything observed. */
#define ARKCHEMY_MEM2_EXPHEAP_SIZE (ARKCHEMY_MEM2_SIZE / 2)
#define ARKCHEMY_MEM2_ARENA_BASE   (ARKCHEMY_MEM2_BASE + ARKCHEMY_MEM2_EXPHEAP_SIZE)
#define ARKCHEMY_MEM2_ARENA_END    (ARKCHEMY_MEM2_BASE + ARKCHEMY_MEM2_SIZE)

/* Real PPC entry address of Green Hills libc's sbrk() in this title.
 * Used to route only genuine heap-growth calls to the contiguous arena;
 * see MEMAllocFromDefaultHeapEx. Title-specific by nature -- if this
 * ever targets a different binary it must be re-derived, and a wrong
 * value degrades safely (everything simply takes the ordinary
 * header-bearing ExpHeap path). */
#define ARKCHEMY_PPC_SBRK_ADDR 0x2583420u

static inline ArkchemyMemHeap *arkchemy_mem_heap_find(uint32_t handle) {
    int i;
    for (i = 0; i < ARKCHEMY_MEM_MAX_HEAPS; i++) {
        if (g_arkchemy_mem_heaps[i].active && g_arkchemy_mem_heaps[i].base == handle) {
            return &g_arkchemy_mem_heaps[i];
        }
    }
    return NULL;
}

/* Frees every host-side free-list node for a heap slot (not the guest
 * memory it describes, which belongs to the heap itself) -- needed
 * both on real destroy and before reusing a table slot for a new heap,
 * so a stale list from a previous heap that lived in this same slot
 * never leaks host memory or gets misread as the new heap's own list. */
static inline void arkchemy_mem_heap_clear_free_list(ArkchemyMemHeap *h) {
    ArkchemyFreeBlock *n = h->free_list;
    while (n != NULL) {
        ArkchemyFreeBlock *next = n->next;
        free(n);
        n = next;
    }
    h->free_list = NULL;
}

/* Real Cafe OS MEMHeapHeader is 0x40 bytes and sits at the start of every
 * heap block; allocations begin after it. Mirrored here so a child heap
 * carved out of a parent can never share the parent's base address. */
#define ARKCHEMY_MEM_HEAP_HEADER_SIZE 0x40u

static inline uint32_t arkchemy_mem_heap_create(uint32_t base, uint32_t size) {
    int i;
    for (i = 0; i < ARKCHEMY_MEM_MAX_HEAPS; i++) {
        if (!g_arkchemy_mem_heaps[i].active) {
            arkchemy_mem_heap_clear_free_list(&g_arkchemy_mem_heaps[i]);
            g_arkchemy_mem_heaps[i].base = base;
            g_arkchemy_mem_heaps[i].size = size;
            /* Start allocating AFTER a reserved header area, exactly as real
             * Cafe OS does -- MEMCreateExpHeapEx/MEMCreateFrmHeapEx write a
             * heap header at the start of the block and hand back that block
             * as the handle.
             *
             * Reserving nothing here caused a handle collision that cost
             * several hardware rounds to find. The game uses the standard
             * Cafe OS idiom:
             *
             *     h     = MEMGetBaseHeapHandle(MEM1);
             *     block = MEMAllocFromFrmHeapEx(h, <all of MEM1>);
             *     mine  = MEMCreateExpHeapEx(block, size);
             *
             * With bump == base, that first allocation returned MEM1's own
             * base address, so the game's new heap was created at 0x1000000
             * -- the same address as MEM1 itself. Handles here ARE base
             * addresses, so both heaps had handle 0x1000000, and
             * arkchemy_mem_heap_find returns the first match: the parent,
             * which the game had just emptied by taking all 32MB of it.
             *
             * Every allocation the game made against its own fresh 32MB heap
             * was therefore served from the exhausted parent and returned
             * NULL, while the real heap sat untouched and unreachable. That
             * is what stalled the boot in igStringPool and looked, from the
             * logs, exactly like a memory leak. */
            g_arkchemy_mem_heaps[i].bump = base + ARKCHEMY_MEM_HEAP_HEADER_SIZE;
            g_arkchemy_mem_heaps[i].active = 1;
            return base;
        }
    }
    return 0; /* out of heap table slots */
}

/* Real, targeted fix -- see ARKCHEMY_BOOTSTRAP_HEAP_HANDLE_ADDR's own
 * comment above for the full real hardware-traced explanation. Call
 * this once, from the boot shim, before the real game entry point runs
 * -- doing exactly what the real (currently unrecovered/unreached)
 * game function would have done: create the heap, store its handle
 * into the real global every early allocator reads it from. */
static inline void arkchemy_mem_bootstrap_heap_init(PpcContext *ctx) {
    uint32_t handle = arkchemy_mem_heap_create(ARKCHEMY_BOOTSTRAP_HEAP_BASE, ARKCHEMY_BOOTSTRAP_HEAP_SIZE);
    g_arkchemy_bootstrap_heap_handle = handle;
    ppc_store_u32(ctx, ARKCHEMY_BOOTSTRAP_HEAP_HANDLE_ADDR, handle);
}

static inline void arkchemy_mem_heap_destroy(uint32_t handle) {
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(handle);
    if (h != NULL) {
        arkchemy_mem_heap_clear_free_list(h);
        h->active = 0;
    }
}

static inline uint32_t arkchemy_align_up(uint32_t v, uint32_t align) {
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
extern uint64_t g_arkchemy_mem_alloc_fail_total;
extern uint64_t g_arkchemy_mem_free_total;
extern uint64_t g_arkchemy_mem_reuse_total;

static inline void ppc_import_coreinit_MEMCreateExpHeapEx(PpcContext *ctx) {
    /* MEMHeapHandle MEMCreateExpHeapEx(void *heap, uint32_t size, uint16_t flags) */
    uint32_t result = arkchemy_mem_heap_create(ctx->r[3], ctx->r[4]);
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
    arkchemy_mem_heap_destroy(handle);
    ctx->r[3] = handle;
}

/* Per-call-site allocation accounting, added 2026-08-29.
 *
 * Run 6 exhausted MEM1 (32MB, base 0x1000000) and hung: the engine asked
 * getAvailableHeapSize, got 0, and igStringPool::remove then spun forever
 * walking a bucket chain for an item that was no longer in it.
 *
 * Nothing in the logs said what filled 32MB, because the existing
 * instrumentation only reports single allocations of 64KB or more and the
 * run recorded just 78 frees. So it was filled by thousands of small,
 * unlogged, never-freed allocations -- exactly the case the 64KB threshold
 * cannot see.
 *
 * Rather than guess (the string pool is the obvious suspect, which is
 * precisely why it should be measured), accumulate bytes per guest call
 * site and name the top consumers at the moment the heap runs dry. */
typedef struct ArkchemyAllocSite {
    uint32_t pc;
    uint32_t heap_base;
    uint32_t count;
    uint32_t bytes;
} ArkchemyAllocSite;

#define ARKCHEMY_ALLOC_SITE_SLOTS 96
/* extern, defined once in cafeos_state.c -- NOT static. This header is
 * included by every generated_*.c, so a `static` table here gives each of
 * the hundreds of translation units its own private copy: allocations get
 * recorded into one TU's table and the exhaustion dump reads a different
 * one, which is exactly why runs 7 and 8 reported an empty table for a heap
 * that was demonstrably full. g_arkchemy_mem_heaps next door was already
 * extern for the same reason. */
extern ArkchemyAllocSite g_arkchemy_alloc_sites[ARKCHEMY_ALLOC_SITE_SLOTS];

static inline void arkchemy_mem_record_site(uint32_t heap_base, uint32_t size) {
    int i;
    for (i = 0; i < ARKCHEMY_ALLOC_SITE_SLOTS; i++) {
        ArkchemyAllocSite *e = &g_arkchemy_alloc_sites[i];
        if (e->count != 0 && (e->pc != g_ppc_current_pc || e->heap_base != heap_base)) continue;
        if (e->count == 0) { e->pc = g_ppc_current_pc; e->heap_base = heap_base; }
        e->count++;
        e->bytes += size;
        return;
    }
    /* Table full: fold the remainder into slot 0 so the total still adds up
     * rather than silently under-reporting. */
    g_arkchemy_alloc_sites[0].count++;
    g_arkchemy_alloc_sites[0].bytes += size;
}

/* Names the biggest consumers of one heap, largest first. Called only on
 * exhaustion, so its cost never matters. */
static inline void arkchemy_mem_dump_sites(uint32_t heap_base) {
    int rank, i, best;
    uint32_t seen = 0;
    if (!g_ppc_mem_alloc_fail_log) return;
    for (rank = 0; rank < 8; rank++) {
        uint32_t best_bytes = 0;
        best = -1;
        for (i = 0; i < ARKCHEMY_ALLOC_SITE_SLOTS; i++) {
            ArkchemyAllocSite *e = &g_arkchemy_alloc_sites[i];
            if (e->count == 0 || e->heap_base != heap_base) continue;
            if (e->bytes > best_bytes && e->bytes < (seen ? seen : 0xFFFFFFFFu)) {
                best_bytes = e->bytes;
                best = i;
            }
        }
        if (best < 0) return;
        seen = best_bytes;
        g_ppc_mem_alloc_fail_log("MEM top consumer",
                                 g_arkchemy_alloc_sites[best].pc,
                                 g_arkchemy_alloc_sites[best].count,
                                 g_arkchemy_alloc_sites[best].bytes,
                                 (uint32_t)rank);
    }
}

static inline void ppc_import_coreinit_MEMAllocFromExpHeapEx(PpcContext *ctx) {
    /* void *MEMAllocFromExpHeapEx(MEMHeapHandle heap, uint32_t size, int alignment) */
    /* Self-healing bootstrap heap, 2026-08-24. Real hardware traced a
     * silent, total boot failure to this exact case: a heap handle of 0.
     *
     * The engine reads its bootstrap heap handle from a real global
     * (.bss+306320). No recompiled function writes that global -- a
     * static scan of all 217 generated files finds zero stores to it --
     * because whatever real function creates that heap was never
     * recovered, which is why the boot shim pre-writes it. But by the
     * time igMemoryContext's constructor reads it, it is 0 again: the
     * game clears its own .bss during entry, after our write and before
     * the read, so the pre-write cannot survive on its own.
     *
     * The consequence was invisible and fatal. The constructor allocates
     * 52 bytes from that null handle, gets NULL, and returns NULL.
     * igArkCore::initBootstrap reaches userInstantiate only through a
     * vtable dispatch built from that return value (r3 -> vtable ->
     * vtable+0x34 -> bctrl), so a NULL reads a vtable from address 0 and
     * jumps nowhere. userInstantiate never runs, the current memory
     * context global stays 0, and every pool lookup afterwards
     * degenerates -- 71 million calls with a null pool, no crash, no
     * failed-allocation counter, nothing obviously wrong in a log.
     *
     * Rather than depend on write ordering that the game is free to
     * undo, a zero handle is treated as "the bootstrap heap", created on
     * demand. That is strictly better than the alternative: today a zero
     * handle means guaranteed silent boot failure, so there is no
     * legitimate behaviour being masked. It is logged so it stays
     * visible, and it survives a regen, unlike a hand-patch. */
    if (ctx->r[3] == 0) {
        if (g_arkchemy_bootstrap_heap_handle == 0) {
            g_arkchemy_bootstrap_heap_handle =
                arkchemy_mem_heap_create(ARKCHEMY_BOOTSTRAP_HEAP_BASE, ARKCHEMY_BOOTSTRAP_HEAP_SIZE);
        }
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("null heap handle -> bootstrap heap (self-heal)",
                                     ctx->r[4], g_arkchemy_bootstrap_heap_handle, 0, 0);
        }
        ctx->r[3] = g_arkchemy_bootstrap_heap_handle;
        /* Re-assert the global too, so the engine's own later reads see
         * a live handle instead of repeating this every allocation. */
        ppc_store_u32(ctx, ARKCHEMY_BOOTSTRAP_HEAP_HANDLE_ADDR, g_arkchemy_bootstrap_heap_handle);
    }
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
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
        ArkchemyFreeBlock **pp = &h->free_list;
        while (*pp != NULL) {
            ArkchemyFreeBlock *n = *pp;
            uint32_t candidate = arkchemy_align_up(n->addr + 4, align);
            if (candidate + size <= n->addr + n->size) {
                *pp = n->next;
                free(n);
                ppc_store_u32(ctx, candidate - 4, size);
                ctx->r[3] = candidate;
                g_arkchemy_mem_reuse_total++;
                arkchemy_mem_record_site(h->base, size);
                return;
            }
            pp = &n->next;
        }
    }

    addr = arkchemy_align_up(h->bump + 4, align); /* +4: room for the private size header */
    end = addr + size;
    if (end > h->base + h->size) { /* real OOM behavior: NULL */
        g_arkchemy_mem_alloc_fail_total++;
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMAllocFromExpHeapEx (out of space)", size, h->base, h->size, h->bump - h->base);
            arkchemy_mem_dump_sites(h->base);
        }
        ctx->r[3] = 0;
        return;
    }
    ppc_store_u32(ctx, addr - 4, size);
    h->bump = end;
    ctx->r[3] = addr;
    arkchemy_mem_record_site(h->base, size);
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
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
    uint32_t block = ctx->r[4];
    ArkchemyFreeBlock *node;
    uint32_t size;
    if (h == NULL || block == 0) return;
    size = ppc_load_u32(ctx, block - 4);
    node = (ArkchemyFreeBlock *)malloc(sizeof(ArkchemyFreeBlock));
    if (node == NULL) return; /* host allocation failure -- block just stays leaked, no crash */
    node->addr = block - 4;
    node->size = size + 4;
    node->next = h->free_list;
    h->free_list = node;
    g_arkchemy_mem_free_total++;
}

static inline void ppc_import_coreinit_MEMGetAllocatableSizeForExpHeapEx(PpcContext *ctx) {
    /* uint32_t MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle heap, int alignment) */
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
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
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
    uint32_t size = ctx->r[4];
    int32_t alignment = (int32_t)ctx->r[5];
    uint32_t align = alignment < 0 ? (uint32_t)(-alignment) : (uint32_t)alignment;
    uint32_t addr, end;
    if (h == NULL) { ctx->r[3] = 0; return; }
    addr = arkchemy_align_up(h->bump, align); /* no private header needed -- Frm blocks are never size-queried */
    end = addr + size;
    if (end > h->base + h->size) {
        /* Was a silent `return 0`. MEM1 is the frame heap, and run 6/7 filled
         * all 32MB of it and hung -- with no out-of-space line anywhere in
         * the log, because this path never logged one. A shim that fails
         * silently is indistinguishable from one that was never called, and
         * that cost a whole hardware round to notice. */
        g_arkchemy_mem_alloc_fail_total++;
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMAllocFromFrmHeapEx (out of space)", size, h->base, h->size, h->bump - h->base);
            arkchemy_mem_dump_sites(h->base);
        }
        ctx->r[3] = 0;
        return;
    }
    h->bump = end;
    ctx->r[3] = addr;
    /* Frame-heap allocations were the one bump path with no accounting, so
     * run 7's per-site table came back empty for MEM1 even though MEM1 was
     * full -- the allocations were real, just invisible here. */
    arkchemy_mem_record_site(h->base, size);
}

static inline void ppc_import_coreinit_MEMFreeToFrmHeap(PpcContext *ctx) {
    /* void MEMFreeToFrmHeap(MEMHeapHandle heap, MEMFrmHeapFreeMode mode) --
     * MEM_FRM_HEAP_FREE_HEAD=1, _TAIL=2, _ALL=3. This allocator only ever
     * grows from the head, so HEAD/ALL both mean "reset the whole heap"; a
     * TAIL-only free (2) is a real no-op here (nothing was ever allocated
     * from the tail direction), a known simplification. */
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
    uint32_t mode = ctx->r[4];
    if (h != NULL && (mode & 1u) != 0) {
        h->bump = h->base;
    }
}

static inline void ppc_import_coreinit_MEMGetAllocatableSizeForFrmHeapEx(PpcContext *ctx) {
    ArkchemyMemHeap *h = arkchemy_mem_heap_find(ctx->r[3]);
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
    if (g_arkchemy_base_heap_handle[idx] == 0) {
        uint32_t base = (idx == 0) ? ARKCHEMY_MEM1_BASE : (idx == 1) ? ARKCHEMY_MEM2_BASE : ARKCHEMY_FG_BASE;
        uint32_t size = (idx == 0) ? ARKCHEMY_MEM1_SIZE : (idx == 1) ? ARKCHEMY_MEM2_EXPHEAP_SIZE : ARKCHEMY_FG_SIZE;
        g_arkchemy_base_heap_handle[idx] = arkchemy_mem_heap_create(base, size);
        /* Rare (at most 3 real calls total, one per base heap type) --
         * see MEMCreateExpHeapEx's own comment on why this is safe to
         * always log. */
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("MEMGetBaseHeapHandle lazy-create", type, base, size, g_arkchemy_base_heap_handle[idx]);
        }
    }
    ctx->r[3] = g_arkchemy_base_heap_handle[idx];
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
    /* Real bug found on hardware 2026-08-24: this routed to MEM1, and
     * Green Hills libc's sbrk() grows the game's entire C heap through
     * here. sbrk never gives memory back, so MEM1's 32MB filled
     * monotonically (measured: 33,440,920 of 33,554,432 bytes used,
     * 3,609 failed allocations, free=0 reuse=0) and every later
     * allocation failed forever, leaving the engine spinning in
     * reallocCommon/getMemoryPoolByIndex retries.
     *
     * MEM1 was the wrong pool. On real Wii U hardware MEM1 is the small
     * 32MB fast pool and an application's default heap comes from MEM2,
     * the large main-memory pool -- so a general-purpose C heap growing
     * out of MEM1 does not match the real machine either. Routed to
     * MEM2 (index 1), which is also now considerably larger. */
    uint32_t size = ctx->r[3];
    int32_t alignment = (int32_t)ctx->r[4];
    if (g_arkchemy_base_heap_handle[1] == 0) {
        g_arkchemy_base_heap_handle[1] = arkchemy_mem_heap_create(ARKCHEMY_MEM2_BASE, ARKCHEMY_MEM2_EXPHEAP_SIZE);
    }
    /* Dedicated, strictly contiguous arena for this path, 2026-08-24.
     * Measured: every successive allocation here landed exactly 64 bytes
     * past the previous block's end (addresses 0x4000040, 0x4020080,
     * 0x40400c0, ... for 131072-byte requests), because the general
     * ExpHeap path puts a private 4-byte size header before each block
     * and then re-aligns. sbrk's contract is that successive calls
     * return ADJACENT memory, and this is the allocator behind Green
     * Hills libc's sbrk, so those gaps break the one assumption malloc
     * makes about its heap.
     *
     * Serving this path from its own bump arena with no headers and no
     * inter-block padding restores that contract exactly. Carved from
     * the top of MEM2 so it cannot collide with ordinary ExpHeap use of
     * the same pool.
     *
     * Honest caveat: real Cafe OS MEMAllocFromDefaultHeapEx also has
     * block headers, so real hardware sees gaps here too and this may
     * not be the whole story -- which is precisely why it is worth
     * testing directly rather than reasoning about. Blocks from this
     * arena carry no size header, so MEMGetSizeForMBlockExpHeap cannot
     * describe them; that is safe only while nothing frees or queries
     * them, which matches the observed behaviour (free=0, reuse=0 across
     * every run). If that ever stops being true this needs revisiting. */
    /* Serve the contiguous arena ONLY to real sbrk() calls.
     *
     * Real bug this fixes, reported by the engine itself on 2026-08-24:
     *   "Memory pool (index %d) failed integrity check."
     * Arena blocks deliberately carry no size header, which is what
     * makes successive sbrk returns adjacent -- but it also means
     * MEMGetSizeForMBlockExpHeap cannot describe them. The risk was
     * written down when the arena was added ("safe only while nothing
     * frees or queries them"), and a pool integrity check queries block
     * sizes, so that condition broke exactly as anticipated.
     *
     * MEMAllocFromDefaultHeapEx is not sbrk-only: hardware shows real
     * callers besides ppc_sbrk reaching it (0x217bc3c, 0x216c434,
     * 0x215c4cc, 0x214a1d8). Those want ordinary, describable blocks;
     * only sbrk needs adjacency, and malloc manages its own headers
     * inside the region it gets. Gating on the caller gives each what it
     * needs instead of trading one bug for the other -- reverting the
     * arena is not an option, since it is what let malloc reuse its heap
     * at all (allocation failures 1,563 -> 0, static init completing).
     *
     * g_ppc_current_pc is the last recompiled function entered, and this
     * shim is not itself recompiled, so inside this call it still names
     * the caller. */
    if (g_ppc_current_pc == ARKCHEMY_PPC_SBRK_ADDR) {
        static uint32_t s_sbrk_next = 0;
        static uint32_t s_sbrk_end  = 0;
        if (s_sbrk_next == 0) {
            /* Arena share raised from 1/2 to 7/8 of MEM2 on evidence,
             * 2026-08-24: with contiguity restored, malloc started
             * working properly -- ExpHeap failures went 1,563 -> 0 and
             * static init advanced from being stuck in initializer #87
             * all the way to #113 -- but this arena itself ran dry near
             * the end of init (25 exhaustions from a 208MB half-share).
             * The ordinary ExpHeap side of MEM2 is barely used (the
             * engine's own pools live in the bootstrap heap), so the
             * split was the wrong way round. */
            s_sbrk_next = ARKCHEMY_MEM2_ARENA_BASE;
            s_sbrk_end  = ARKCHEMY_MEM2_ARENA_END;
        }
        if (s_sbrk_next + size <= s_sbrk_end) {
            uint32_t got = s_sbrk_next;
            s_sbrk_next += size;              /* exactly contiguous: no header, no padding */
            ctx->r[3] = got;
            return;
        }
        /* Arena exhausted: FALL THROUGH to the general ExpHeap path
         * rather than failing. Returning NULL here was a real regression
         * introduced with this arena on 2026-08-24 and caught the same
         * day: sbrk demand is unbounded, so the arena always fills
         * eventually, and with no free list and no fallback every later
         * allocation failed forever -- including a 24-byte request, while
         * 52MB of ordinary ExpHeap in this same pool sat untouched.
         *
         * The concrete damage: igArkCore::initBootstrap runs at call
         * ~18,340, just after the arena ran dry at ~18,337. Its
         * igMemoryContext constructor could not allocate, returned NULL,
         * and initBootstrap reaches userInstantiate only through a vtable
         * dispatch built from that return value (r3 -> vtable ->
         * vtable+0x34 -> bctrl). A NULL there reads a vtable from address
         * 0 and jumps nowhere, so userInstantiate never ran, the current
         * memory context global stayed 0, and every later pool lookup
         * degenerated -- 71,343,306 getMemoryPoolByIndex calls with
         * context=0x0. No crash, no allocation-failure counter, nothing
         * obviously wrong in the log.
         *
         * Contiguity only matters while malloc is growing its main heap,
         * which is exactly the window the arena covers. Once it is spent,
         * ordinary header-bearing blocks are strictly better than
         * failure. */
        if (g_ppc_mem_alloc_fail_log) {
            g_ppc_mem_alloc_fail_log("DEFHEAP arena spent, falling back to ExpHeap", size, s_sbrk_next, s_sbrk_end, 0);
        }
    }
    ctx->r[3] = g_arkchemy_base_heap_handle[1];
    ctx->r[4] = size;
    ctx->r[5] = (uint32_t)alignment;
    {
        /* Contiguity check, 2026-08-24. sbrk()'s contract is that
         * successive calls return ADJACENT memory -- malloc relies on
         * that to treat the heap as one growing segment. This shim
         * returns ordinary ExpHeap blocks, each preceded by a private
         * 4-byte size header and then aligned, so successive returns are
         * NOT adjacent (observed: heap_used advancing 131,136 per
         * 131,072-byte request, a 64-byte gap). If malloc cannot see its
         * heap as contiguous it may keep growing it forever, which is
         * exactly the unbounded sbrk growth being chased. Logging the
         * gap makes that provable rather than suspected. */
        /* The per-allocation contiguity log that lived here is removed
         * (2026-08-24). It did its job -- it proved successive sbrk-path
         * allocations were separated by a 64-byte header+alignment gap,
         * which is what led to the arena above -- but it keyed the log
         * throttle on the returned address, so every call created a new
         * budget entry. With only 12 budget slots, later allocations all
         * collapsed onto one and burned its 40-event allowance, which
         * silently suppressed unrelated diagnostics fired later in boot
         * (the igMemoryContext constructor trace at call ~36,700 never
         * appeared because of this). A diagnostic that hides other
         * diagnostics is worse than none. */
        ppc_import_coreinit_MEMAllocFromExpHeapEx(ctx);
        return;
    }
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
    if (g_arkchemy_base_heap_handle[0] == 0) {
        g_arkchemy_base_heap_handle[0] = arkchemy_mem_heap_create(ARKCHEMY_MEM1_BASE, ARKCHEMY_MEM1_SIZE);
    }
    ctx->r[3] = g_arkchemy_base_heap_handle[0];
    ctx->r[4] = ptr;
    ppc_import_coreinit_MEMFreeToExpHeap(ctx);
}

/*
 * __gh_errno_ptr/__gh_set_errno: Green Hills libc's errno accessor pair
 * (real Wii U retail code is GHS-compiled -- see cafeos_ghs_runtime.h).
 * Needs one real, stable, persistent guest address -- a fixed reserved
 * slot, same documented-placeholder trade-off as the base heaps above.
 */
static inline void ppc_import_coreinit___gh_errno_ptr(PpcContext *ctx) { ctx->r[3] = ARKCHEMY_ERRNO_ADDR; }
static inline void ppc_import_coreinit___gh_set_errno(PpcContext *ctx) {
    ppc_store_u32(ctx, ARKCHEMY_ERRNO_ADDR, ctx->r[3]);
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_MEM_H */
