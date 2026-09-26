/* PPC_MEM_CHECK and the arena's end, tested on the host.
 *
 *   gcc -O1 -w -fsanitize=address -I include hosttest/mem_check_test.c -o /tmp/mct -lm && /tmp/mct
 *
 * Two things are proved here:
 *
 * 1. An access in the last bytes of guest memory stays inside the host
 *    array. Accessors mask only the start address and then touch up to four
 *    bytes, so before 2026-09-26 a store at PPC_MEM_SIZE - 2 wrote past the
 *    end of mem[] into the next host global. Built with AddressSanitizer
 *    (as CI does), that was a global-buffer-overflow report; now the
 *    PPC_MEM_PAD bytes absorb it.
 * 2. PPC_MEM_CHECK records exactly the bad accesses: past the end, and in
 *    the NULL page, loads and stores of every width -- and nothing for an
 *    ordinary access. Sites are counted by (function, lr).
 *
 * A small arena keeps the test quick; the mask arithmetic is the same at
 * any power of two. */
#define PPC_MEM_SIZE (1024u * 1024u)
#define PPC_MEM_CHECK 1
#include "ppc_runtime.h"
#include "cafeos_coreinit_libc.h"
#include <stdio.h>

void ppc_dispatch(PpcContext *c, uint32_t a) { (void)c; (void)a; }

static PpcSharedMemory sh;
static int failures;
static void expect(const char *what, uint32_t got, uint32_t want) {
    if (got != want) { printf("FAIL %s: got %u, want %u\n", what, got, want); failures++; }
}

int main(void) {
    static PpcContext ctx;
    ctx.shared = &sh;

    /* ordinary traffic: nothing recorded */
    g_ppc_current_pc = 0x02000100u; ctx.lr = 0x02000200u;
    ppc_store_u32(&ctx, 0x8000u, 0xdeadbeefu);
    expect("ordinary word round-trips", ppc_load_u32(&ctx, 0x8000u), 0xdeadbeefu);
    ppc_store_u16(&ctx, 0x8010u, 0x1234u);
    (void)ppc_load_u8(&ctx, PPC_MEM_SIZE - 1u);       /* the last byte is fine */
    (void)ppc_load_u32(&ctx, PPC_MEM_SIZE - 4u);      /* so is the last word */
    expect("no reports for ordinary accesses", g_ppc_memchk_past_end + g_ppc_memchk_null, 0u);

    /* straddling the end: reported, and no host overflow (ASan would abort) */
    g_ppc_current_pc = 0x02000300u; ctx.lr = 0x02000400u;
    ppc_store_u32(&ctx, PPC_MEM_SIZE - 2u, 0x11223344u);
    (void)ppc_load_u32(&ctx, PPC_MEM_SIZE - 2u);
    (void)ppc_load_u16(&ctx, PPC_MEM_SIZE - 1u);
    expect("three straddling accesses", g_ppc_memchk_past_end, 3u);

    /* wholly past the end: would alias low memory through the mask */
    ppc_store_u8(&ctx, PPC_MEM_SIZE + 0x10u, 7u);
    expect("an address beyond the arena", g_ppc_memchk_past_end, 4u);
    expect("one site so far (same function, same lr)", g_ppc_memchk_site_n, 1u);
    expect("that site counted every hit", g_ppc_memchk_site[0][4], 4u);

    /* the NULL page, from a different site: loads, stores, bulk copies */
    g_ppc_current_pc = 0x02000500u; ctx.lr = 0x02000600u;
    (void)ppc_load_u32(&ctx, 0x0cu);                  /* a field read through NULL */
    ppc_store_u16(&ctx, 0x20u, 1u);                   /* stores of every width */
    ctx.r[3] = 0x9000u; ctx.r[4] = 0x0u; ctx.r[5] = 16u;
    ppc_import_coreinit_memcpy(&ctx);                 /* copying from NULL */
    ctx.r[3] = 0u; ctx.r[4] = 0u; ctx.r[5] = 0u;
    ppc_import_coreinit_memset(&ctx);                 /* zero bytes: legal, not reported */
    expect("three NULL-page accesses", g_ppc_memchk_null, 3u);
    expect("a second site", g_ppc_memchk_site_n, 2u);
    expect("its pc", g_ppc_memchk_site[1][0], 0x02000500u);
    expect("its lr", g_ppc_memchk_site[1][1], 0x02000600u);
    expect("its first address", g_ppc_memchk_site[1][2], 0x0cu);
    expect("its kind: NULL, load, 4 bytes", g_ppc_memchk_site[1][3], (PPC_MEMCHK_NULL << 8) | 4u);

    /* a bulk copy past the end is still dropped, but no longer silently */
    ctx.r[3] = PPC_MEM_SIZE - 8u; ctx.r[4] = 0x8000u; ctx.r[5] = 64u;
    ppc_import_coreinit_memcpy(&ctx);
    expect("a memcpy running off the end", g_ppc_memchk_past_end, 5u);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("mem check: all checks passed\n");
    return 0;
}
