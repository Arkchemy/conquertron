/* Replay a captured TLSF call sequence against the recompiled allocator.
 *
 * tlsf_memalign is correct on a fresh heap at every alignment (tlsf_harness),
 * so the hardware fault needs the specific history before it. This takes that
 * history -- recorded by TLSFTRACE on device -- and runs it here, checking the
 * physical chain after every single call.
 *
 * The first call whose result differs from the recorded one, or after which the
 * chain stops reaching the sentinel, is the bug, with the full prior state
 * available in a debugger.
 *
 * Usage: tlsf_replay <tlsf-trace.bin>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ppc_runtime.h"

#define CTRL_SIZE 0xc70u

void ppc_tlsf_create(PpcContext *);
void ppc_tlsf_memalign(PpcContext *);
void ppc_tlsf_free(PpcContext *);
void ppc_tlsf_realloc(PpcContext *);

static PpcSharedMemory g_shared;
static PpcContext      g_ctx;
static uint32_t        g_arena, g_size;

static uint32_t call3(void (*fn)(PpcContext *), uint32_t a, uint32_t b, uint32_t c)
{ g_ctx.r[3] = a; g_ctx.r[4] = b; g_ctx.r[5] = c; fn(&g_ctx); return g_ctx.r[3]; }

/* Physical walk, identical to tlsf_walk_heap: header at ctrl+0xc70, size at +4
   masked to ~3, next at block + 4 + size, zero terminates. */
static uint32_t chain_span(uint32_t *blocks)
{
    uint32_t b = g_arena + CTRL_SIZE, end = g_arena + g_size, n = 0;
    for (;;) {
        uint32_t w, sz;
        if (n >= 200000u || b < g_arena || b + 8u > end) break;
        w = ppc_load_u32(&g_ctx, b + 4u);
        sz = w & ~3u;
        if (sz == 0u) break;
        n++; b += 4u + sz;
    }
    if (blocks) *blocks = n;
    return b;
}

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t hdr[4], e[4], i = 0, heap, sentinel, span, blocks, prev_span;

    if (argc < 2) { fprintf(stderr, "usage: %s <tlsf-trace.bin>\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0x54534C46u) {
        fprintf(stderr, "not a TLSF trace (bad magic)\n"); return 2;
    }
    g_arena = hdr[1];
    printf("trace: ctrl=0x%08x entries=%u dropped=%u\n", hdr[1], hdr[2], hdr[3]);
    if (hdr[3]) printf("  NOTE: %u entries were dropped; the replay is incomplete\n", hdr[3]);

    /* The arena size is not in the trace -- it is the pool's _size, 5 MB for
       the pool under investigation. Overridable so other pools can be replayed. */
    g_size = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x00500000u;
    sentinel = g_arena + g_size - 8u;

    g_ctx.shared = &g_shared;
    memset(g_shared.mem, 0, PPC_MEM_SIZE);
    heap = call3(ppc_tlsf_create, g_arena, g_size, 0);
    if (!heap) { fprintf(stderr, "tlsf_create failed\n"); return 1; }
    prev_span = chain_span(&blocks);
    printf("fresh heap: %u block(s), span 0x%08x, sentinel 0x%08x\n\n", blocks, prev_span, sentinel);

    while (fread(e, sizeof e, 1, f) == 1) {
        uint32_t op = e[0], a = e[1], b = e[2], want = e[3], got = 0;
        i++;
        switch (op) {
            case 1: got = call3(ppc_tlsf_memalign, heap, a, b); break;
            case 2: call3(ppc_tlsf_free, heap, a, 0);           break;
            case 3: got = call3(ppc_tlsf_realloc, heap, a, b);  break;
            default: printf("#%u unknown op %u\n", i, op); continue;
        }
        span = chain_span(&blocks);

        if (op != 2 && got != want) {
            printf("#%u DIVERGED  op=%u a=0x%x b=%u  host->0x%08x  device->0x%08x\n",
                   i, op, a, b, got, want);
            printf("   chain: %u blocks, span 0x%08x\n", blocks, span);
            return 1;
        }
        if (span != sentinel && prev_span == sentinel) {
            printf("#%u ORPHANED  op=%u a=0x%x b=%u -> 0x%08x\n", i, op, a, b, got);
            printf("   chain now ends 0x%08x, sentinel 0x%08x, %u bytes lost, %u blocks\n",
                   span, sentinel, sentinel - span, blocks);
            if (got) printf("   returned block's own size word = 0x%x\n",
                            ppc_load_u32(&g_ctx, got - 4u));
            return 1;
        }
        prev_span = span;
    }
    printf("replayed %u calls, chain still reaches the sentinel\n", i);
    printf("  -> the recorded sequence alone does not reproduce it\n");
    return 0;
}
