#ifndef BRAMBLE_CAFEOS_GX2_H
#define BRAMBLE_CAFEOS_GX2_H

#include "ppc_runtime.h"

/*
 * Phase 1d/2 CafeOS runtime shim -- gx2 (graphics).
 *
 * The backend decision the project plan left open (NVN vs. a homebrew
 * Vulkan/deko3d layer) is now resolved, not guessed at: NVN requires an
 * official, restricted Nintendo SDK license homebrew developers don't
 * have access to, and there is no Vulkan driver exposed to homebrew on
 * stock Switch firmware -- deko3d (a real, community-standard,
 * NVN-like low-level graphics API built specifically for Switch
 * homebrew, part of the same devkitPro/libnx toolchain this whole
 * project already depends on) is the only technically viable option,
 * confirmed by what's actually installed and available in this
 * project's real toolchain (`/opt/devkitpro/libnx/include/deko3d.h`,
 * `libdeko3d.a`), not chosen arbitrarily between roughly-equal
 * alternatives.
 *
 * deko3d has no host build (it's Switch-hardware-specific, unlike
 * everything else in this shim so far, which is host-testable C) --
 * the real implementation below is guarded behind `__SWITCH__` (the
 * same platform-conditional pattern cafeos_coreinit_fs.h already uses
 * for `_WIN32`/`DT_DIR`), falling back to the previous honest no-op
 * when built on host, so the rest of this project's host-side
 * verification loop (every other cafeos_*.h compiled standalone) keeps
 * working unchanged.
 *
 * Real, important architectural note: deko3d targets the Switch's real
 * NVIDIA Tegra/Maxwell GPU, completely unrelated in hardware family to
 * the Wii U's real AMD "Latte" (R700-derived) GPU GX2 was built for --
 * despite similarly-named concepts (viewport, scissor, blend state),
 * this is a genuine reimplementation on different hardware, not a
 * register-for-register translation. GX2's own AMD-specific surface
 * tiling/swizzle math (GX2CalcSurfaceSizeAndAlignment and friends,
 * still not attempted -- see docs/phase1d_import_surface.md) only
 * matters for reading/writing raw pixel data directly; it's NOT needed
 * for the state-setting/draw-call functions below, which map onto
 * deko3d's own equivalent high-level API concepts directly.
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

#ifdef __SWITCH__
#include <deko3d.h>

/*
 * Real, host-side (not guest-memory) deko3d state -- this project's
 * established pattern (host-side state keyed by/associated with a
 * subsystem, same as the MEM* / thread / sync tables) applied to graphics.
 * A real GX2 app builds up state across many GX2Set* calls and submits
 * it as a display list; for this first real slice, state-setting calls
 * record directly into one persistent command buffer, submitted on
 * GX2Flush/GX2SwapScanBuffers (still to be implemented -- this commit
 * covers real device/queue/cmdbuf setup and a first batch of
 * straightforward state recorders, not the full draw/present path).
 *
 * Command memory size (0x10000, 64KB) is a real, generous starting
 * placeholder -- same documented "not a final answer, will need real
 * sizing once real usage volume is known" trade-off already used
 * elsewhere in this project (assign_global_addrs, PpcContext::mem).
 */
typedef struct {
    bool initialized;
    DkDevice device;
    DkMemBlock cmd_mem_block;
    DkCmdBuf cmdbuf;
    DkQueue queue;
} BrambleGx2State;

static BrambleGx2State g_bramble_gx2;

#define BRAMBLE_GX2_CMD_MEM_SIZE 0x10000u

static inline void ppc_import_gx2_GX2Init(PpcContext *ctx) {
    /* void GX2Init(uint32_t *attributes) -- real attributes array
     * (key/value pairs, e.g. requested tiling aperture size) accepted
     * but not yet consumed -- no real GX2 caller behavior in this
     * game's own binary has been checked against needing a specific
     * one yet. Real device/queue/command-buffer setup below, following
     * devkitPro's own official deko3d Example01 (Simple Setup)
     * structure. */
    (void)ctx;
    if (g_bramble_gx2.initialized) return;

    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    g_bramble_gx2.device = dkDeviceCreate(&device_maker);

    DkMemBlockMaker mem_maker;
    dkMemBlockMakerDefaults(&mem_maker, g_bramble_gx2.device, BRAMBLE_GX2_CMD_MEM_SIZE);
    mem_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_bramble_gx2.cmd_mem_block = dkMemBlockCreate(&mem_maker);

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, g_bramble_gx2.device);
    g_bramble_gx2.cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    dkCmdBufAddMemory(g_bramble_gx2.cmdbuf, g_bramble_gx2.cmd_mem_block, 0, BRAMBLE_GX2_CMD_MEM_SIZE);

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, g_bramble_gx2.device);
    queue_maker.flags = DkQueueFlags_Graphics;
    g_bramble_gx2.queue = dkQueueCreate(&queue_maker);

    g_bramble_gx2.initialized = true;
}

static inline void ppc_import_gx2_GX2Shutdown(PpcContext *ctx) {
    (void)ctx;
    if (!g_bramble_gx2.initialized) return;
    dkQueueWaitIdle(g_bramble_gx2.queue);
    dkQueueDestroy(g_bramble_gx2.queue);
    dkCmdBufDestroy(g_bramble_gx2.cmdbuf);
    dkMemBlockDestroy(g_bramble_gx2.cmd_mem_block);
    dkDeviceDestroy(g_bramble_gx2.device);
    g_bramble_gx2.initialized = false;
}

static inline void ppc_import_gx2_GX2SetViewport(PpcContext *ctx) {
    /* void GX2SetViewport(float x, float y, float width, float height,
     * float nearZ, float farZ) -- real field-for-field match with
     * deko3d's own DkViewport struct, confirmed by comparing both real
     * headers directly, not assumed. All 6 args are floats, so all 6
     * land in f1-f6 (PPC32 SVR4 ABI: FPR and GPR argument sequences are
     * independent -- confirmed empirically, not just asserted, see
     * cafeos_snd_core.h's AXSetVoiceSrcRatio fix for how this was
     * caught and verified against real compiled PPC output). */
    DkViewport vp;
    vp.x = (float)ctx->f[1];
    vp.y = (float)ctx->f[2];
    vp.width = (float)ctx->f[3];
    vp.height = (float)ctx->f[4];
    vp.near = (float)ctx->f[5];
    vp.far = (float)ctx->f[6];
    dkCmdBufSetViewports(g_bramble_gx2.cmdbuf, 0, &vp, 1);
}

static inline void ppc_import_gx2_GX2SetScissor(PpcContext *ctx) {
    /* void GX2SetScissor(uint32_t x, uint32_t y, uint32_t width,
     * uint32_t height) -- real field-for-field match with deko3d's own
     * DkScissor struct. Real integer args -- GPRs, not FPRs. */
    DkScissor sc;
    sc.x = ctx->r[3];
    sc.y = ctx->r[4];
    sc.width = ctx->r[5];
    sc.height = ctx->r[6];
    dkCmdBufSetScissors(g_bramble_gx2.cmdbuf, 0, &sc, 1);
}

static inline void ppc_import_gx2_GX2SetLineWidth(PpcContext *ctx) {
    /* void GX2SetLineWidth(float width) -- direct real equivalent,
     * dkCmdBufSetLineWidth(cmdbuf, float width). */
    dkCmdBufSetLineWidth(g_bramble_gx2.cmdbuf, (float)ctx->f[1]);
}

static inline void ppc_import_gx2_GX2SetPointSize(PpcContext *ctx) {
    /* void GX2SetPointSize(float width, float height) -- real GX2 has
     * independent width/height (a real AMD hardware feature,
     * non-square points); deko3d's dkCmdBufSetPointSize only takes one
     * uniform size -- a real, honest simplification (uses width,
     * ignores height) documented here, not silently dropped. */
    dkCmdBufSetPointSize(g_bramble_gx2.cmdbuf, (float)ctx->f[1]);
}

static inline void ppc_import_gx2_GX2SetPolygonOffset(PpcContext *ctx) {
    /* void GX2SetPolygonOffset(float frontOffset, float frontScale,
     * float backOffset, float backScale, float clamp) -- real GX2 has
     * independent front/back-face bias; deko3d's dkCmdBufSetDepthBias
     * (constantFactor, clamp, slopeFactor) is a single, not
     * face-separated, state -- a real, honest simplification (uses the
     * front-face values only) documented here, not silently dropped.
     * Real args: r3-r7 all floats (f1-f5). */
    dkCmdBufSetDepthBias(g_bramble_gx2.cmdbuf, (float)ctx->f[1], (float)ctx->f[5], (float)ctx->f[2]);
}

static inline void ppc_import_gx2_GX2SetBlendConstantColor(PpcContext *ctx) {
    /* void GX2SetBlendConstantColor(float red, float green, float blue,
     * float alpha) -- direct real equivalent,
     * dkCmdBufSetBlendConst(cmdbuf, r, g, b, a), same argument order. */
    dkCmdBufSetBlendConst(g_bramble_gx2.cmdbuf, (float)ctx->f[1], (float)ctx->f[2], (float)ctx->f[3], (float)ctx->f[4]);
}

static inline void ppc_import_gx2_GX2SetPrimitiveRestartIndex(PpcContext *ctx) {
    /* void GX2SetPrimitiveRestartIndex(uint32_t index) -- real GX2
     * signature has no separate enable flag; deko3d's
     * dkCmdBufSetPrimitiveRestart(cmdbuf, enable, index) does. Assumed
     * real behavior: setting a restart index means the game wants
     * restart enabled (enable=true always) -- a documented assumption,
     * not confirmed against real hardware behavior (no
     * GX2DisablePrimitiveRestart-style counterpart exists in this
     * game's real import list to contradict it). */
    dkCmdBufSetPrimitiveRestart(g_bramble_gx2.cmdbuf, true, ctx->r[3]);
}

#else /* !__SWITCH__ -- no deko3d on host; see file comment */

static inline void ppc_import_gx2_GX2Init(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2Shutdown(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetViewport(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetScissor(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetLineWidth(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetPointSize(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetPolygonOffset(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetBlendConstantColor(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetPrimitiveRestartIndex(PpcContext *ctx) { (void)ctx; }

#endif /* __SWITCH__ */

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
