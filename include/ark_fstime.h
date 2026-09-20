#ifndef ARK_FSTIME_H
#define ARK_FSTIME_H

/* Split out of ark_blockprobe.h on 2026-09-20, and the reason is the build
 * loop rather than tidiness.
 *
 * Timing the SD reads meant the FS shim needed the probe helpers, so
 * cafeos_coreinit_fs.h included ark_blockprobe.h -- and that header is
 * included by every generated translation unit. In one edit the probe header
 * went from "two files rebuild, 25 seconds" to "all 224 rebuild, 22 minutes",
 * which is the single change that made a dozen probes in a night possible in
 * the first place.
 *
 * So the pieces the FS shim needs live here, in a header that is expected to
 * stay still: a clock, and the read timings. ark_blockprobe.h goes back to
 * being included by a handful of files and stays cheap to edit.
 *
 * ark_now_ns duplicates the two lines of arkchemy_gx2_host_ticks rather than
 * reaching into cafeos_gx2.h, because the filesystem has no business
 * including the graphics header to read a clock. */

#include <stdint.h>
#include <time.h>

static inline unsigned long long ark_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

/* FSTIME: when each SD read starts and finishes.
 *
 * Measured 2026-09-20 against BURST's block-read timeline: a 24ms gap between
 * block reads contains a 23ms card read, a 95ms gap contains a 95ms read, and
 * a 197ms gap contains two totalling 149ms. The unexplained remainder in the
 * larger gaps is LZMA decompression, which is CPU work and shows up as no
 * read at all.
 *
 * That makes the cluster-and-stall shape of the load ordinary buffered
 * streaming -- fetch 128KB, decompress, consume, repeat -- and not a defect.
 * What is a defect is that it stops afterwards with all six blocks held. The
 * run-to-run spread in how far it gets is card latency and scheduling
 * deciding how many tasks win a block before those six stick, which is why
 * the byte count cannot be used to judge a loading change. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fst[16][2], g_ark_fst_n = 0;   /* start us, end us */

static inline void ark_fst_begin(unsigned long long ns) {
    if (g_ark_fst_n < 16u) g_ark_fst[g_ark_fst_n][0] = (uint32_t)(ns / 1000ull);
}
static inline void ark_fst_end(unsigned long long ns) {
    if (g_ark_fst_n < 16u) { g_ark_fst[g_ark_fst_n][1] = (uint32_t)(ns / 1000ull); g_ark_fst_n++; }
}

#endif /* ARK_FSTIME_H */
