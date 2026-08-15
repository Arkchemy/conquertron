#ifndef BRAMBLE_CAFEOS_GX2_H
#define BRAMBLE_CAFEOS_GX2_H

#include "ppc_runtime.h"

/*
 * Phase 1d/2 CafeOS runtime shim -- gx2 (graphics). gx2 as a whole is
 * NOT a "more shims" problem: 98 of this game's 98 real gx2 imports
 * need either real GPU command-buffer/state work or a real surface-
 * tiling algorithm, and the project plan explicitly leaves the actual
 * Switch graphics backend (NVN vs. a homebrew Vulkan/deko3d layer) as
 * an open decision -- see docs/phase1d_import_surface.md's Phase 2
 * section. This file is *not* an attempt at that: it's the small,
 * genuinely backend-independent slice of gx2's real import surface --
 * fixed hardware queries and pure calculations -- that doesn't need
 * that decision made first, the same "quick win investigation" standard
 * applied to every other library in this project.
 *
 * Real values/formulas for the functions with no wut documentation
 * (GX2TempGetGPUVersion, GX2GetSystemTVScanMode/AspectRatio,
 * GX2GetSurfaceFormatBits) are sourced directly from Cemu's real HLE
 * implementation (src/Cafe/OS/libs/gx2/GX2.cpp,
 * src/Cafe/HW/Latte/ISA/LatteReg.h), not guessed -- these are fixed
 * hardware facts (the real "Latte" GPU's version number, this specific
 * console's real TV output mode) or a real, table-driven bit-depth
 * formula, independent of any backend choice.
 */

static inline void ppc_import_gx2_GX2Init(PpcContext *ctx) {
    /* void GX2Init(uint32_t *attributes) -- real hardware sets up ring
     * buffers/GPU command-processor state here; genuine no-op until a
     * real graphics backend exists to initialize. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2Shutdown(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_gx2_GX2TempGetGPUVersion(PpcContext *ctx) {
    /* uint32_t GX2TempGetGPUVersion(void) -- real fixed hardware
     * constant confirmed directly against Cemu's HLE (`return 2;`),
     * not a guess: this is the real "Latte" GPU's version number, the
     * same on every retail Wii U. */
    (void)ctx;
    ctx->r[3] = 2;
}

static inline void ppc_import_gx2_GX2GetSystemTVScanMode(PpcContext *ctx) {
    /* GX2TVScanMode GX2GetSystemTVScanMode(void) -- real value (7 =
     * 1080p) confirmed against Cemu's HLE comment ("1080p = 7"). Not a
     * simulated display setting -- this shim reports a fixed, plausible
     * real TV mode the same way every other "no real console-specific
     * hardware state" query in this project does. */
    (void)ctx;
    ctx->r[3] = 7;
}

static inline void ppc_import_gx2_GX2GetSystemTVAspectRatio(PpcContext *ctx) {
    /* GX2AspectRatio GX2GetSystemTVAspectRatio(void) -- 1 = 16:9,
     * confirmed against Cemu's HLE (`return 1; // 16:9`) and wut's own
     * GX2_ASPECT_RATIO_16_9 = 1. */
    (void)ctx;
    ctx->r[3] = 1;
}

static inline void ppc_import_gx2_GX2SetSwapInterval(PpcContext *ctx) {
    /* void GX2SetSwapInterval(uint32_t interval) -- accepted, not
     * stored: GX2GetSwapInterval isn't in this game's real import list
     * (confirmed by recompiling the actual tfbGame_cafe.rpx), so
     * nothing here ever reads it back. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2GetSurfaceFormatBits(PpcContext *ctx) {
    /* uint32_t GX2GetSurfaceFormatBits(GX2SurfaceFormat format) -- real
     * formula and table confirmed directly against Cemu's HLE
     * (Latte::GetFormatBits + Latte::IsCompressedFormat in
     * src/Cafe/HW/Latte/ISA/LatteReg.h): mask the format to its low 6
     * bits (the real hardware format index, GX2's own surface-format
     * encoding already reserves the upper bits for sign/int/float/sRGB
     * modifiers that don't change bit width), look up a fixed 64-entry
     * bits-per-pixel table, then for the real hardware's BC1-BC5
     * compressed-format range (0x31-0x35) divide by 16 (a compressed
     * "pixel" entry in this table is really a 4x4 block). Cross-checked
     * by hand against wut's own confirmed GX2SurfaceFormat values: e.g.
     * UNORM_R8_G8_B8_A8 (0x1a) -> 32 bits (4x8bpp, correct), UNORM_R8
     * (0x01) -> 8 bits (correct), UNORM_BC1 (0x31) -> 64/16 = 4 bits
     * (BC1's real, well-known 4-bits-per-pixel compression ratio,
     * correct) -- not just copied blind. */
    static const uint8_t bits_table[64] = {
        0x00, 0x08, 0x08, 0x00, 0x00, 0x10, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00,
        0x00, 0x20, 0x20, 0x20, 0x40, 0x40, 0x40, 0x40,
        0x40, 0x00, 0x80, 0x80, 0x00, 0x00, 0x00, 0x10,
        0x10, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x60,
        0x60, 0x40, 0x80, 0x80, 0x40, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint32_t format = ctx->r[3];
    uint32_t hw_format = format & 0x3Fu;
    uint32_t bpp = bits_table[hw_format];
    if (hw_format >= 0x31u && hw_format <= 0x35u) { /* real BC1-BC5 compressed range */
        bpp /= 16u;
    }
    ctx->r[3] = bpp;
}

#endif /* BRAMBLE_CAFEOS_GX2_H */
