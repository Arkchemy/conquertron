/* Run the recompiled TLSF allocator on the host.
 *
 * The whole igTlsfWrapper translation unit compiles natively -- ppc_runtime.h
 * needs only math/stdint/stdlib/string, and the allocator touches guest memory
 * exclusively through ppc_load/ppc_store, so it has no idea it is not on a
 * Wii U. Nothing about it requires a Switch, an NRO, or the game.
 *
 * That matters because the allocator bug has been chased through a
 * build -> copy 176 MB over MTP -> launch -> pull log cycle, about ten
 * minutes a question. Here the same question costs milliseconds and the
 * answer is deterministic and reproducible.
 *
 * Build:  see hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>
#include "ppc_runtime.h"

/* Real addresses from the failing run, so the harness reproduces the shape of
 * the actual arena rather than a tidy synthetic one. */
#define ARENA      0x045002e0u
#define ARENA_SIZE 0x00500000u
#define CTRL_SIZE  0xc70u          /* from tlsf_walk_heap: first block = ctrl + 0xc70 */

void ppc_tlsf_create(PpcContext *);
void ppc_tlsf_memalign(PpcContext *);
void ppc_tlsf_free(PpcContext *);
void ppc_tlsf_realloc(PpcContext *);
void ppc_tlsf_block_size(PpcContext *);

static PpcSharedMemory g_shared;
static PpcContext      g_ctx;

static uint32_t call(void (*fn)(PpcContext *), uint32_t a, uint32_t b, uint32_t c)
{
    g_ctx.r[3] = a; g_ctx.r[4] = b; g_ctx.r[5] = c;
    fn(&g_ctx);
    return g_ctx.r[3];
}

/* Walk the physical chain exactly as tlsf_walk_heap does: header at ctrl+0xc70,
 * size at +4 masked to ~3, next at block + 4 + size, zero size terminates. */
typedef struct { uint32_t blocks, used, freeb, span, zero_size_block; } Chain;

static Chain walk(void)
{
    Chain c = {0,0,0,0,0};
    uint32_t b = ARENA + CTRL_SIZE, end = ARENA + ARENA_SIZE, n = 0;
    for (;;) {
        uint32_t w, sz;
        if (n >= 100000u || b < ARENA || b + 8u > end) break;
        w  = ppc_load_u32(&g_ctx, b + 4u);
        sz = w & ~3u;
        if (sz == 0u) break;
        if (w & 1u) c.freeb += sz; else c.used += sz;
        n++; b += 4u + sz;
    }
    c.blocks = n; c.span = b;
    return c;
}

static int fail;
static void check(const char *what, uint32_t ptr)
{
    Chain c = walk();
    uint32_t bsz = ptr ? (ppc_load_u32(&g_ctx, ptr - 4u) & ~3u) : 1u;
    uint32_t sentinel = ARENA + ARENA_SIZE - 8u;
    if (ptr && bsz == 0u) {
        printf("  !! %-34s returned ptr=0x%08x whose OWN size word is 0\n", what, ptr);
        fail = 1;
    }
    if (c.span != sentinel) {
        printf("  !! %-34s chain ends 0x%08x, sentinel is 0x%08x (%u bytes orphaned)\n",
               what, c.span, sentinel, sentinel - c.span);
        fail = 1;
    }
}

int main(void)
{
    uint32_t heap, p, i;
    g_ctx.shared = &g_shared;
    memset(g_shared.mem, 0, PPC_MEM_SIZE);

    heap = call(ppc_tlsf_create, ARENA, ARENA_SIZE, 0);
    printf("tlsf_create(0x%08x, 0x%x) -> 0x%08x\n", ARENA, ARENA_SIZE, heap);
    if (!heap) { printf("  create failed\n"); return 1; }
    { Chain c = walk();
      printf("  fresh heap: %u block(s), span 0x%08x, free %u bytes\n\n",
             c.blocks, c.span, c.freeb); }

    /* 1. the exact call SPLITVERIFY caught: align 64, size 65540 -- the pool
       adds 4 to the caller's 65536 for its trailing size marker. Testing 65536
       instead was why the first version of this harness found nothing. */
    printf("case 1: the failing call in isolation -- memalign(align=64, size=65540)\n");
    p = call(ppc_tlsf_memalign, heap, 64, 65540);
    printf("  -> ptr=0x%08x  block size word=0x%x\n", p,
           p ? ppc_load_u32(&g_ctx, p - 4u) : 0);
    check("memalign(64, 65540)", p);
    printf("  %s\n\n", fail ? "REPRODUCED" : "clean -- needs prior state");

    /* 2. a spread of alignments, each on a fresh heap */
    printf("case 2: alignment sweep, fresh heap each time, size 65540\n");
    for (i = 4; i <= 4096; i <<= 1) {
        memset(g_shared.mem, 0, PPC_MEM_SIZE);
        heap = call(ppc_tlsf_create, ARENA, ARENA_SIZE, 0);
        fail = 0;
        p = call(ppc_tlsf_memalign, heap, i, 65540);
        {
            uint32_t bsz = p ? (ppc_load_u32(&g_ctx, p - 4u) & ~3u) : 0;
            Chain c = walk();
            printf("  align %5u -> ptr=0x%08x aligned=%s bsz=0x%-6x blocks=%u span=0x%08x%s\n",
                   i, p, (p && (p % i) == 0) ? "yes" : "NO ", bsz, c.blocks, c.span,
                   (c.span != ARENA + ARENA_SIZE - 8u) ? "  <-- ORPHANED" : "");
        }
    }
    return 0;
}
