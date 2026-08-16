#ifndef BRAMBLE_CAFEOS_GX2_H
#define BRAMBLE_CAFEOS_GX2_H

#include "ppc_runtime.h"
#include <math.h>
#include <string.h>
#include <time.h>

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
#include <switch.h>

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
#define BRAMBLE_GX2_NUM_FRAMEBUFFERS 2u
#define BRAMBLE_GX2_FB_WIDTH 1280u
#define BRAMBLE_GX2_FB_HEIGHT 720u

typedef struct {
    bool initialized;
    DkDevice device;
    DkMemBlock cmd_mem_block;
    DkCmdBuf cmdbuf;
    DkQueue queue;

    /* Real swapchain/framebuffer state -- following devkitPro's own
     * official deko3d Example01 (Simple Setup) structure directly, not
     * improvised. Fixed 1280x720/2-framebuffer sizing is a real,
     * documented placeholder (same "generous starting point, not a
     * final answer" trade-off already used throughout this project),
     * not read from any real GX2 surface description yet -- see
     * GX2SetColorBuffer's own deferred status below. */
    DkMemBlock fb_mem_block;
    DkImage framebuffers[BRAMBLE_GX2_NUM_FRAMEBUFFERS];
    DkSwapchain swapchain;
    int acquired_slot; /* -1 if no framebuffer image is currently acquired this frame */

    /* Real GX2 timestamp/swap-status tracking (GX2GetLastSubmittedTimeStamp/
     * GX2GetRetiredTimeStamp/GX2WaitTimeStamp/GX2GetSwapStatus below).
     * Real hardware's OSTime is an actual wall-clock-derived tick count
     * a real GPU command's completion is stamped with; this runtime has
     * no asynchronous GPU pipeline to stamp against yet (every submit
     * this shim makes is followed by a real, synchronous
     * dkQueueWaitIdle -- see GX2Flush/GX2DrawDone/GX2SwapScanBuffers
     * below), so `submitted_timestamp`/`retired_timestamp` are a
     * monotonically increasing host tick count (same
     * `clock_gettime(CLOCK_MONOTONIC)` source already used for
     * `OSGetTime` in cafeos_coreinit_sync.h) taken at each real submit
     * point -- not a claim of matching real Wii U tick magnitude/epoch,
     * same documented approximation already used there. */
    uint64_t submitted_timestamp;
    uint64_t retired_timestamp;
    uint32_t swap_count;
    uint32_t flip_count;

    /* Real, persistent host-side shadow copies of deko3d's own combined
     * state objects. Real GX2, like real deko3d, groups several
     * logically-separate settings into one hardware register/one bind
     * call each (e.g. deko3d's single DkColorState carries blend-enable
     * mask, logic-op, *and* alpha-test compare op together) -- multiple
     * *different* real GX2 calls (GX2SetColorControl, GX2SetAlphaTest)
     * can each touch only part of the *same* underlying deko3d state
     * object. Binding a freshly-defaulted local struct per call (this
     * file's original approach, before GX2SetAlphaTest below needed to
     * touch DkColorState too) would silently reset whichever fields the
     * *other* real GX2 call had set -- these shadow copies exist so
     * each GX2Set* function can read-modify-write just its own real
     * field(s) and rebind the whole combined object, matching real
     * hardware's actual "these are independent settings that happen to
     * share one register" semantics. */
    DkRasterizerState rasterizer_state;
    DkDepthStencilState depth_stencil_state;
    DkColorState color_state;
    DkMultisampleState multisample_state;

    /* GX2SetColorControl's `colorWriteEnable` and GX2SetTargetChannelMasks'
     * per-target `GX2ChannelMask`s are two real, independent GX2 calls
     * that both ultimately determine the same deko3d DkColorWriteState
     * -- but unlike the shadow structs above, they don't each own a
     * disjoint subset of *fields* within one struct; they both want to
     * control the *same* per-target/per-channel mask bits, just from
     * two different real angles (a master on/off switch vs. explicit
     * per-channel selection). Kept as two separate source-of-truth
     * values instead of a shared `DkColorWriteState`, combined via
     * `bramble_gx2_rebind_color_write_state` below (colorWriteEnable
     * ANDed against channel_masks) whenever either changes -- a real,
     * documented interpretation of GX2SetColorControl's own "master
     * on/off switch" description, not the "whichever call happened
     * last wins outright" behavior this file used to have (which
     * would silently discard GX2SetTargetChannelMasks' finer-grained
     * per-channel masking the next time GX2SetColorControl ran for an
     * unrelated reason, e.g. just toggling blend). */
    uint32_t color_write_enable;
    uint32_t channel_masks;

    /* GX2SetEventCallback registration (see BRAMBLE_GX2_NUM_EVENT_TYPES
     * below) -- real guest addresses (function pointer + userData),
     * not host pointers, same real addressing model as every other
     * guest-memory reference in this project (see ppc_load_u32/
     * ppc_store_u32 in ppc_runtime.h). Genuinely stored, not yet
     * genuinely invoked -- see GX2SetEventCallback's own comment for
     * why that's a real, honest, separate gap. */
    uint32_t event_callback_func[5];
    uint32_t event_callback_userdata[5];
} BrambleGx2State;

/* GX2EventType's real enumerator count (confirmed against wut's
 * gx2/enum.h: START_OF_PIPE_INTERRUPT=0, END_OF_PIPE_INTERRUPT=1,
 * VSYNC=2, FLIP=3, DISPLAY_LIST_OVERRUN=4) -- sizes the
 * event_callback_func/userdata arrays above. */
#define BRAMBLE_GX2_NUM_EVENT_TYPES 5u

static BrambleGx2State g_bramble_gx2;

#define BRAMBLE_GX2_CMD_MEM_SIZE 0x10000u

/* Same real host monotonic clock source/reasoning as
 * cafeos_coreinit_sync.h's ppc_coreinit_host_ticks -- a distinct,
 * gx2-scoped name since both headers are `static inline` and can be
 * included in the same translation unit (a real generated program
 * pulls in every cafeos_*.h it needs), which a shared name would
 * redefine. */
static inline uint64_t bramble_gx2_host_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void bramble_gx2_create_framebuffers(void) {
    DkImageLayout layout;
    DkImageLayoutMaker layout_maker;
    DkImage const *fb_array[BRAMBLE_GX2_NUM_FRAMEBUFFERS];
    uint64_t fb_size;
    uint32_t fb_align, i;
    DkMemBlockMaker fb_mem_maker;
    DkSwapchainMaker swapchain_maker;

    dkImageLayoutMakerDefaults(&layout_maker, g_bramble_gx2.device);
    layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = BRAMBLE_GX2_FB_WIDTH;
    layout_maker.dimensions[1] = BRAMBLE_GX2_FB_HEIGHT;
    dkImageLayoutInitialize(&layout, &layout_maker);

    fb_size = dkImageLayoutGetSize(&layout);
    fb_align = dkImageLayoutGetAlignment(&layout);

    dkMemBlockMakerDefaults(&fb_mem_maker, g_bramble_gx2.device,
                             (uint32_t)((fb_size * BRAMBLE_GX2_NUM_FRAMEBUFFERS + fb_align - 1) & ~(uint64_t)(fb_align - 1)));
    fb_mem_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_bramble_gx2.fb_mem_block = dkMemBlockCreate(&fb_mem_maker);

    for (i = 0; i < BRAMBLE_GX2_NUM_FRAMEBUFFERS; i++) {
        dkImageInitialize(&g_bramble_gx2.framebuffers[i], &layout, g_bramble_gx2.fb_mem_block, (uint32_t)(i * fb_size));
        fb_array[i] = &g_bramble_gx2.framebuffers[i];
    }

    dkSwapchainMakerDefaults(&swapchain_maker, g_bramble_gx2.device, nwindowGetDefault(), fb_array, BRAMBLE_GX2_NUM_FRAMEBUFFERS);
    g_bramble_gx2.swapchain = dkSwapchainCreate(&swapchain_maker);

    g_bramble_gx2.acquired_slot = -1;
}

/* Ensures a real swapchain framebuffer image is acquired and bound as
 * the current render target before any clear/draw command that needs
 * one -- lazy, so a frame with no clear/draw calls at all doesn't
 * needlessly acquire+present an unused image. */
static inline void bramble_gx2_ensure_frame_acquired(void) {
    DkImageView color_target;
    DkImageView const *targets[1];
    if (g_bramble_gx2.acquired_slot >= 0) return;
    g_bramble_gx2.acquired_slot = dkQueueAcquireImage(g_bramble_gx2.queue, g_bramble_gx2.swapchain);
    dkImageViewDefaults(&color_target, &g_bramble_gx2.framebuffers[g_bramble_gx2.acquired_slot]);
    targets[0] = &color_target;
    dkCmdBufBindRenderTargets(g_bramble_gx2.cmdbuf, targets, 1, NULL);
}

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

    bramble_gx2_create_framebuffers();

    dkRasterizerStateDefaults(&g_bramble_gx2.rasterizer_state);
    dkDepthStencilStateDefaults(&g_bramble_gx2.depth_stencil_state);
    dkColorStateDefaults(&g_bramble_gx2.color_state);
    dkMultisampleStateDefaults(&g_bramble_gx2.multisample_state);
    /* deko3d's own default (alphaToCoverageDither=true, deko3d.h) does
     * NOT match real GX2's own real default (GX2_ALPHA_TO_MASK_MODE_
     * NON_DITHERED=0, i.e. dither off) -- corrected here so a game that
     * never calls GX2SetAlphaToMask gets real GX2's actual power-on
     * behavior, not deko3d's differing one. alphaToCoverageEnable's
     * deko3d default (false) already matches GX2's real default
     * (alpha-to-coverage off), so that one's untouched. */
    g_bramble_gx2.multisample_state.alphaToCoverageDither = 0;

    /* Real GX2 default: color writes enabled, all channels, all
     * targets (matching deko3d's own DkColorWriteState default of
     * masks=0xFFFFFFFF) -- see the color_write_enable/channel_masks
     * field comment on BrambleGx2State above for why these are two
     * separate values instead of one shared DkColorWriteState. */
    g_bramble_gx2.color_write_enable = 1;
    g_bramble_gx2.channel_masks = 0xFFFFFFFFu;

    g_bramble_gx2.initialized = true;
}

static inline void ppc_import_gx2_GX2Shutdown(PpcContext *ctx) {
    (void)ctx;
    if (!g_bramble_gx2.initialized) return;
    dkQueueWaitIdle(g_bramble_gx2.queue);
    dkCmdBufClear(g_bramble_gx2.cmdbuf); /* destroys any recorded cmdlists still referencing the framebuffers below */
    dkSwapchainDestroy(g_bramble_gx2.swapchain);
    /* DkImage itself needs no explicit per-image destroy call -- only its backing DkMemBlock does */
    dkMemBlockDestroy(g_bramble_gx2.fb_mem_block);
    dkQueueDestroy(g_bramble_gx2.queue);
    dkCmdBufDestroy(g_bramble_gx2.cmdbuf);
    dkMemBlockDestroy(g_bramble_gx2.cmd_mem_block);
    dkDeviceDestroy(g_bramble_gx2.device);
    /* Real correctness fix: event_callback_func/userdata (see
     * GX2SetEventCallback) hold real *guest* addresses that only stay
     * meaningful for the lifetime of the game session that registered
     * them -- a real GX2Shutdown followed by a fresh GX2Init (e.g. a
     * resolution/mode change) without every event type being
     * re-registered would otherwise leave stale callback addresses
     * from the prior session around. Harmless today since invocation
     * isn't implemented yet (see that function's own comment), but a
     * real landmine for whenever dispatch is added -- cleared now so
     * it never becomes one. */
    memset(g_bramble_gx2.event_callback_func, 0, sizeof(g_bramble_gx2.event_callback_func));
    memset(g_bramble_gx2.event_callback_userdata, 0, sizeof(g_bramble_gx2.event_callback_userdata));
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

static inline DkBlendFactor bramble_gx2_blend_mode_to_dk(uint32_t gx2_mode) {
    /* GX2BlendMode -> DkBlendFactor. Real GX2BlendMode has 21 values
     * (0-20, confirmed against wut's gx2/enum.h), real DkBlendFactor
     * values confirmed directly from deko3d.h. For indices 0-10 and
     * 15-18 both enums enumerate the exact same real blend factors in
     * the exact same real order, just offset by 1 (GX2 is 0-based,
     * deko3d reserves 0). GX2_BLEND_MODE_BLEND_FACTOR/INV_BLEND_FACTOR
     * (13/14) and CONSTANT_ALPHA/INV_CONSTANT_ALPHA (19/20) map to
     * deko3d's real Const/InvConst factors (same blend-constant-color
     * concept both APIs expose, via GX2SetBlendConstantColor /
     * dkCmdBufSetBlendConst above). GX2_BLEND_MODE_BOTH_SRC_ALPHA/
     * BOTH_INV_SRC_ALPHA (11/12) are real dual-source-blend modes with
     * no deko3d equivalent found (deko3d's Src1Color/Src1Alpha factors
     * are a different real feature, a second bound color, not both
     * src and dst using src alpha); approximated here as plain
     * SrcAlpha/InvSrcAlpha, a documented, unconfirmed simplification
     * for a pair of blend modes not expected to be hit in practice. */
    static const uint8_t table[21] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, /* 0-10: direct +1 offset */
        5, 6,                               /* 11-12: BOTH_SRC_ALPHA / BOTH_INV_SRC_ALPHA -- approximated, see above */
        0x21, 0x22,                         /* 13-14: BLEND_FACTOR / INV_BLEND_FACTOR -> ConstColor / InvConstColor */
        16, 17, 18, 19,                     /* 15-18: SRC1_* -- direct +1 offset resumes */
        0x23, 0x24,                         /* 19-20: CONSTANT_ALPHA / INV_CONSTANT_ALPHA -> ConstAlpha / InvConstAlpha */
    };
    if (gx2_mode >= 21) return DkBlendFactor_One; /* defensive fallback for an out-of-range/unrecognized value */
    return (DkBlendFactor)table[gx2_mode];
}

static inline DkBlendOp bramble_gx2_blend_combine_to_dk(uint32_t gx2_combine) {
    /* GX2BlendCombineMode -> DkBlendOp. Real GX2 order (confirmed
     * against wut's gx2/enum.h) is ADD=0, SUB=1, MIN=2, MAX=3,
     * REV_SUB=4; real deko3d order (confirmed against deko3d.h) is
     * Add=1, Sub=2, RevSub=3, Min=4, Max=5 -- same five real blend
     * operations, different real ordering, so this needs an actual
     * lookup table rather than an offset. */
    static const uint8_t table[5] = {1, 2, 4, 5, 3};
    if (gx2_combine >= 5) return DkBlendOp_Add; /* defensive fallback for an out-of-range/unrecognized value */
    return (DkBlendOp)table[gx2_combine];
}

static inline void ppc_import_gx2_GX2SetBlendControl(PpcContext *ctx) {
    /* void GX2SetBlendControl(GX2RenderTarget target,
     * GX2BlendMode colorSrcBlend, GX2BlendMode colorDstBlend,
     * GX2BlendCombineMode colorCombine, BOOL useAlphaBlend,
     * GX2BlendMode alphaSrcBlend, GX2BlendMode alphaDstBlend,
     * GX2BlendCombineMode alphaCombine) -- real signature confirmed
     * against wut's gx2/registers.h. All 8 params are integers/enums/
     * BOOL, so per the real PPC32 SVR4 ABI (integer args in r3-r10,
     * independent of any float sequence) they land in r3-r10 directly,
     * no stack-passed args.
     *
     * Real GX2 lets color and alpha blending use fully independent
     * factors/combine ops, gated by useAlphaBlend; deko3d's DkBlendState
     * always has separate color/alpha slots (dkBlendStateSetFactors/
     * SetOps take both), so useAlphaBlend==false is handled here by
     * mirroring the color blend settings into the alpha slots -- the
     * real, documented interpretation of "don't use separate alpha
     * blending" (not confirmed against real hardware/Cemu behavior for
     * this exact corner case, but consistent with how every other GX2
     * "combined color+alpha state" function in this file has been
     * handled so far). */
    uint32_t target = ctx->r[3];
    uint32_t color_src = ctx->r[4], color_dst = ctx->r[5], color_combine = ctx->r[6];
    uint32_t use_alpha_blend = ctx->r[7];
    uint32_t alpha_src = ctx->r[8], alpha_dst = ctx->r[9], alpha_combine = ctx->r[10];
    DkBlendState state;

    dkBlendStateDefaults(&state);
    dkBlendStateSetOps(&state,
        bramble_gx2_blend_combine_to_dk(color_combine),
        use_alpha_blend ? bramble_gx2_blend_combine_to_dk(alpha_combine) : bramble_gx2_blend_combine_to_dk(color_combine));
    dkBlendStateSetFactors(&state,
        bramble_gx2_blend_mode_to_dk(color_src), bramble_gx2_blend_mode_to_dk(color_dst),
        use_alpha_blend ? bramble_gx2_blend_mode_to_dk(alpha_src) : bramble_gx2_blend_mode_to_dk(color_src),
        use_alpha_blend ? bramble_gx2_blend_mode_to_dk(alpha_dst) : bramble_gx2_blend_mode_to_dk(color_dst));

    if (target < 8) dkCmdBufBindBlendState(g_bramble_gx2.cmdbuf, target, &state);
}

static inline DkLogicOp bramble_gx2_logic_op_to_dk(uint32_t gx2_rop3) {
    /* GX2LogicOp -> DkLogicOp. GX2's real values (confirmed against
     * wut's gx2/enum.h) are a byte with the same nibble duplicated
     * twice (0x00, 0x11, 0x22, ... 0xFF) -- i.e. value == index * 0x11,
     * so `value >> 4` recovers a real 0-15 index in GX2's own
     * declaration order. GX2's raw byte values are NOT the same
     * encoding deko3d/OpenGL use (their `GX2_LOGIC_OP_NOR == 0x11`
     * does not correspond to `DkLogicOp_Nor == 8` numerically), so this
     * maps by real operation *name* -- CLEAR, NOR, AND, XOR, etc. are
     * the same well-known, standard two-operand logic operations in
     * both APIs, just assigned different raw enum encodings by each
     * vendor. */
    static const uint8_t table[16] = {
        0,  /* CLEAR    -> Clear */
        8,  /* NOR      -> Nor */
        4,  /* INV_AND  -> AndInverted */
        12, /* INV_COPY -> CopyInverted */
        2,  /* REV_AND  -> AndReverse */
        10, /* INV      -> Invert */
        6,  /* XOR      -> Xor */
        14, /* NOT_AND  -> Nand */
        1,  /* AND      -> And */
        9,  /* EQUIV    -> Equivalent */
        5,  /* NOP      -> NoOp */
        13, /* INV_OR   -> OrInverted */
        3,  /* COPY     -> Copy */
        11, /* REV_OR   -> OrReverse */
        7,  /* OR       -> Or */
        15, /* SET      -> Set */
    };
    uint32_t index = (gx2_rop3 >> 4) & 0xF;
    return (DkLogicOp)table[index];
}

static inline void bramble_gx2_rebind_color_write_state(void) {
    /* Combines GX2SetColorControl's `color_write_enable` (a real
     * master on/off switch) with GX2SetTargetChannelMasks'
     * `channel_masks` (real per-target/per-channel selection) via a
     * real bitwise AND -- when disabled, every channel/target is
     * masked off regardless of what channel_masks says; when enabled,
     * channel_masks' actual per-channel granularity is preserved. Both
     * GX2SetColorControl and GX2SetTargetChannelMasks call this after
     * updating their own field, so either one's change is always
     * reflected without either silently discarding the other's. */
    DkColorWriteState state;
    dkColorWriteStateDefaults(&state);
    state.masks = g_bramble_gx2.color_write_enable ? g_bramble_gx2.channel_masks : 0u;
    dkCmdBufBindColorWriteState(g_bramble_gx2.cmdbuf, &state);
}

static inline void ppc_import_gx2_GX2SetColorControl(PpcContext *ctx) {
    /* void GX2SetColorControl(GX2LogicOp rop3, uint8_t targetBlendEnable,
     * BOOL multiWriteEnable, BOOL colorWriteEnable) -- real signature
     * confirmed against wut's gx2/registers.h. 4 integer/enum/BOOL
     * params, all in r3-r6.
     *
     * rop3 -> DkColorState.logicOp via bramble_gx2_logic_op_to_dk.
     * targetBlendEnable is a real per-render-target bitmask (bit i =
     * blending enabled for target i) -- applied via
     * dkColorStateSetBlendEnable per target, matching deko3d's own
     * per-target model exactly. colorWriteEnable is a real master
     * on/off switch, combined with GX2SetTargetChannelMasks' real
     * per-channel masks via `bramble_gx2_rebind_color_write_state`
     * (see its own comment) rather than unilaterally overwriting every
     * target to all-or-nothing -- a real correctness fix over this
     * file's earlier "whichever of these two calls happened most
     * recently wins outright" behavior, which would have silently
     * discarded GX2SetTargetChannelMasks' finer-grained masking the
     * next time GX2SetColorControl ran for an unrelated reason (e.g.
     * just toggling blend). multiWriteEnable (a real AMD-specific
     * feature broadcasting render target 0's color to every bound
     * target) has no deko3d equivalent found -- a real, honest,
     * unimplemented gap, not silently dropped: the argument is read
     * but intentionally unused.
     *
     * Reads/writes the persistent `g_bramble_gx2.color_state` shadow
     * copy (see its field comment on BrambleGx2State) rather than
     * binding a fresh, freshly-defaulted local struct -- GX2SetAlphaTest
     * below also touches `color_state.alphaCompareOp`, a real,
     * independent GX2 call that shares deko3d's same combined state
     * object; rebinding a local default here would silently erase
     * whatever it already set. */
    uint32_t rop3 = ctx->r[3];
    uint32_t target_blend_enable = ctx->r[4];
    uint32_t multi_write_enable = ctx->r[5];
    uint32_t color_write_enable = ctx->r[6];
    uint32_t i;

    (void)multi_write_enable; /* no deko3d equivalent -- see comment above */

    g_bramble_gx2.color_state.logicOp = bramble_gx2_logic_op_to_dk(rop3);
    for (i = 0; i < 8; i++) {
        dkColorStateSetBlendEnable(&g_bramble_gx2.color_state, i, (target_blend_enable >> i) & 1u);
    }
    dkCmdBufBindColorState(g_bramble_gx2.cmdbuf, &g_bramble_gx2.color_state);

    g_bramble_gx2.color_write_enable = color_write_enable ? 1u : 0u;
    bramble_gx2_rebind_color_write_state();
}

static inline DkCompareOp bramble_gx2_compare_func_to_dk(uint32_t gx2_func) {
    /* GX2CompareFunction -> DkCompareOp. Real GX2 order (confirmed
     * against wut's gx2/enum.h: NEVER=0, LESS=1, EQUAL=2, LEQUAL=3,
     * GREATER=4, NOT_EQUAL=5, GEQUAL=6, ALWAYS=7) is a uniform +1
     * offset from deko3d's real order (confirmed against deko3d.h:
     * Never=1, Less=2, Equal=3, Lequal=4, Greater=5, NotEqual=6,
     * Gequal=7, Always=8) -- both APIs enumerate the same 8 real
     * comparison functions in the exact same order. */
    if (gx2_func >= 8) return DkCompareOp_Always; /* defensive fallback for an out-of-range/unrecognized value */
    return (DkCompareOp)(gx2_func + 1);
}

static inline DkStencilOp bramble_gx2_stencil_func_to_dk(uint32_t gx2_func) {
    /* GX2StencilFunction -> DkStencilOp. Real GX2 order (confirmed
     * against wut's gx2/enum.h: KEEP=0, ZERO=1, REPLACE=2,
     * INCR_CLAMP=3, DECR_CLAMP=4, INV=5, INCR_WRAP=6, DECR_WRAP=7) is a
     * uniform +1 offset from deko3d's real order (confirmed against
     * deko3d.h: Keep=1, Zero=2, Replace=3, Incr=4, Decr=5, Invert=6,
     * IncrWrap=7, DecrWrap=8) -- both APIs enumerate the same 8 real
     * stencil operations in the exact same order. */
    if (gx2_func >= 8) return DkStencilOp_Keep; /* defensive fallback for an out-of-range/unrecognized value */
    return (DkStencilOp)(gx2_func + 1);
}

static inline void ppc_import_gx2_GX2SetAlphaTest(PpcContext *ctx) {
    /* void GX2SetAlphaTest(BOOL alphaTest, GX2CompareFunction func,
     * float ref) -- real signature confirmed against wut's
     * gx2/registers.h, 2 integer/BOOL params (r3-r4) + 1 real float
     * param (f1, per this project's established convention of floats
     * arriving in the FPR file, not the GPR file, regardless of GPR
     * argument position -- see e.g. GX2ClearColor above).
     *
     * Real deko3d equivalent lives inside the *same* combined
     * DkColorState object GX2SetColorControl above already binds
     * (`alphaCompareOp`, confirmed against deko3d.h sitting alongside
     * `logicOp`/`blendEnableMask` in that one struct) plus a separate
     * real `dkCmdBufSetAlphaRef(cmdbuf, ref)` call for the reference
     * value -- real hardware's alpha test has no separate on/off bit in
     * either API, "disabled" is expressed as "always passes"
     * (DkCompareOp_Always), so alphaTest==false maps to that rather
     * than leaving the previous compare op in place. Reads/writes the
     * persistent shadow copy, same reasoning as GX2SetColorControl
     * above -- rebinding a fresh local DkColorState here would silently
     * erase whatever logicOp/blendEnableMask GX2SetColorControl already
     * set. */
    uint32_t alpha_test = ctx->r[3];
    uint32_t func = ctx->r[4];
    float ref = (float)ctx->f[1];

    g_bramble_gx2.color_state.alphaCompareOp = alpha_test ? bramble_gx2_compare_func_to_dk(func) : DkCompareOp_Always;
    dkCmdBufBindColorState(g_bramble_gx2.cmdbuf, &g_bramble_gx2.color_state);
    dkCmdBufSetAlphaRef(g_bramble_gx2.cmdbuf, ref);
}

static inline void ppc_import_gx2_GX2SetAlphaToMask(PpcContext *ctx) {
    /* void GX2SetAlphaToMask(BOOL alphaToMask, GX2AlphaToMaskMode mode)
     * -- real signature confirmed against wut's gx2/registers.h.
     * GX2AlphaToMaskMode (confirmed against wut's gx2/enum.h:
     * NON_DITHERED=0, DITHER_0/90/180/270=1-4) real values encode both
     * whether dithering is used *and* one of four real dither pattern
     * phase offsets; deko3d's DkMultisampleState only has a plain
     * on/off `alphaToCoverageDither` bit (no phase-offset control) --
     * mapped here as mode==0 (non-dithered) -> dither off, any of the
     * four real DITHER_* variants -> dither on, with the specific real
     * phase offset a documented, honest gap (no deko3d field to put it
     * in). Reads/writes the persistent shadow copy, same
     * read-modify-write reasoning as the other combined-state GX2Set*
     * functions in this file -- DkMultisampleState also carries real
     * MSAA mode fields nothing here sets yet, which a fresh local
     * default would silently reset once those exist. */
    uint32_t alpha_to_mask = ctx->r[3];
    uint32_t mode = ctx->r[4];

    g_bramble_gx2.multisample_state.alphaToCoverageEnable = alpha_to_mask ? 1 : 0;
    g_bramble_gx2.multisample_state.alphaToCoverageDither = (mode != 0) ? 1 : 0;
    dkCmdBufBindMultisampleState(g_bramble_gx2.cmdbuf, &g_bramble_gx2.multisample_state);
}

static inline void ppc_import_gx2_GX2SetDepthStencilControl(PpcContext *ctx) {
    /* void GX2SetDepthStencilControl(BOOL depthTest, BOOL depthWrite,
     * GX2CompareFunction depthCompare, BOOL stencilTest,
     * BOOL backfaceStencil, GX2CompareFunction frontStencilFunc,
     * GX2StencilFunction frontStencilZPass,
     * GX2StencilFunction frontStencilZFail,
     * GX2StencilFunction frontStencilFail,
     * GX2CompareFunction backStencilFunc,
     * GX2StencilFunction backStencilZPass,
     * GX2StencilFunction backStencilZFail,
     * GX2StencilFunction backStencilFail) -- real signature confirmed
     * against wut's gx2/registers.h, 13 real params. Only the first 8
     * fit in r3-r10 (real PPC32 SVR4 ABI); the remaining 5
     * (frontStencilFail, backStencilFunc, backStencilZPass,
     * backStencilZFail, backStencilFail) are real stack-passed args at
     * r1+8/r1+12/r1+16/r1+20/r1+24 -- the exact same real convention
     * already confirmed and used by
     * `ppc_import_coreinit_FSReadFileWithPosAsync`
     * (cafeos_coreinit_fs.h) for its own 9th/10th stack args, verified
     * against this project's own manyargs.c test, not a new
     * assumption.
     *
     * backfaceStencil (real GX2 "does back-facing geometry use its own
     * separate stencil state") has no matching single on/off field in
     * deko3d's DkDepthStencilState (which always carries independent
     * front/back fields) -- when false, this mirrors the front stencil
     * settings into the back fields, a documented, unconfirmed
     * simplification (consistent with how this file already handles
     * GX2SetBlendControl's useAlphaBlend). */
    uint32_t depth_test = ctx->r[3];
    uint32_t depth_write = ctx->r[4];
    uint32_t depth_compare = ctx->r[5];
    uint32_t stencil_test = ctx->r[6];
    uint32_t backface_stencil = ctx->r[7];
    uint32_t front_stencil_func = ctx->r[8];
    uint32_t front_stencil_zpass = ctx->r[9];
    uint32_t front_stencil_zfail = ctx->r[10];
    uint32_t front_stencil_fail = ppc_load_u32(ctx, ctx->r[1] + 8);
    uint32_t back_stencil_func = ppc_load_u32(ctx, ctx->r[1] + 12);
    uint32_t back_stencil_zpass = ppc_load_u32(ctx, ctx->r[1] + 16);
    uint32_t back_stencil_zfail = ppc_load_u32(ctx, ctx->r[1] + 20);
    uint32_t back_stencil_fail = ppc_load_u32(ctx, ctx->r[1] + 24);
    DkDepthStencilState *state = &g_bramble_gx2.depth_stencil_state;

    /* Reads/writes the persistent shadow copy (see BrambleGx2State's
     * field comment) -- GX2SetDepthOnlyControl below is a real subset
     * of this same hardware register and shares this same object. */
    state->depthTestEnable = depth_test ? 1 : 0;
    state->depthWriteEnable = depth_write ? 1 : 0;
    state->depthCompareOp = bramble_gx2_compare_func_to_dk(depth_compare);
    state->stencilTestEnable = stencil_test ? 1 : 0;

    state->stencilFrontCompareOp = bramble_gx2_compare_func_to_dk(front_stencil_func);
    state->stencilFrontPassOp = bramble_gx2_stencil_func_to_dk(front_stencil_zpass);
    state->stencilFrontDepthFailOp = bramble_gx2_stencil_func_to_dk(front_stencil_zfail);
    state->stencilFrontFailOp = bramble_gx2_stencil_func_to_dk(front_stencil_fail);

    if (backface_stencil) {
        state->stencilBackCompareOp = bramble_gx2_compare_func_to_dk(back_stencil_func);
        state->stencilBackPassOp = bramble_gx2_stencil_func_to_dk(back_stencil_zpass);
        state->stencilBackDepthFailOp = bramble_gx2_stencil_func_to_dk(back_stencil_zfail);
        state->stencilBackFailOp = bramble_gx2_stencil_func_to_dk(back_stencil_fail);
    } else {
        state->stencilBackCompareOp = state->stencilFrontCompareOp;
        state->stencilBackPassOp = state->stencilFrontPassOp;
        state->stencilBackDepthFailOp = state->stencilFrontDepthFailOp;
        state->stencilBackFailOp = state->stencilFrontFailOp;
    }

    dkCmdBufBindDepthStencilState(g_bramble_gx2.cmdbuf, state);
}

static inline DkFrontFace bramble_gx2_front_face_to_dk(uint32_t gx2_front_face) {
    /* GX2FrontFace -> DkFrontFace. Real GX2 order (CCW=0, CW=1,
     * confirmed against wut's gx2/enum.h) is *inverted* relative to
     * deko3d's real order (CW=0, CCW=1, confirmed against deko3d.h) --
     * both APIs support the same two real winding conventions, just
     * assign the opposite raw values, so this needs a real 2-entry
     * lookup table, not an offset. */
    static const uint8_t table[2] = {1, 0}; /* GX2 CCW(0)->Dk CCW(1), GX2 CW(1)->Dk CW(0) */
    return (DkFrontFace)table[gx2_front_face & 1u];
}

static inline DkPolygonMode bramble_gx2_polygon_mode_to_dk(uint32_t gx2_mode) {
    /* GX2PolygonMode -> DkPolygonMode. Real GX2 order (POINT=0, LINE=1,
     * TRIANGLE=2, confirmed against wut's gx2/enum.h) is a direct match
     * with deko3d's real order (Point=0, Line=1, Fill=2, confirmed
     * against deko3d.h) -- GX2's "draw as filled triangles" and
     * deko3d's "Fill" are the same real rasterization mode under
     * different names, no offset or reordering needed. */
    if (gx2_mode > 2) return DkPolygonMode_Fill; /* defensive fallback for an out-of-range/unrecognized value */
    return (DkPolygonMode)gx2_mode;
}

static inline void bramble_gx2_apply_cull_state(DkRasterizerState *state, uint32_t front_face, uint32_t cull_front, uint32_t cull_back) {
    /* Shared frontFace/cullMode translation -- GX2SetPolygonControl
     * and GX2SetCullOnlyControl below both real, independent GX2 calls
     * writing this exact same subset of the same hardware register;
     * extracted so a future correction only needs to change one place
     * instead of two byte-identical copies staying in sync by hand. */
    state->frontFace = bramble_gx2_front_face_to_dk(front_face);
    if (cull_front && cull_back) state->cullMode = DkFace_FrontAndBack;
    else if (cull_front) state->cullMode = DkFace_Front;
    else if (cull_back) state->cullMode = DkFace_Back;
    else state->cullMode = DkFace_None;
}

static inline void ppc_import_gx2_GX2SetPolygonControl(PpcContext *ctx) {
    /* void GX2SetPolygonControl(GX2FrontFace frontFace, BOOL cullFront,
     * BOOL cullBack, BOOL polyMode, GX2PolygonMode polyModeFront,
     * GX2PolygonMode polyModeBack, BOOL polyOffsetFrontEnable,
     * BOOL polyOffsetBackEnable, BOOL polyOffsetParaEnable) -- real
     * signature confirmed against wut's gx2/registers.h, 9 real
     * params. Only the first 8 fit in r3-r10; polyOffsetParaEnable
     * (the 9th) is a real stack-passed arg at r1+8, same real
     * convention already established by GX2SetDepthStencilControl
     * above and FSReadFileWithPosAsync (cafeos_coreinit_fs.h).
     *
     * cullFront/cullBack (independent BOOLs) combine into deko3d's
     * single DkFace cullMode (None/Front/Back/FrontAndBack) -- a
     * direct, lossless real translation of the same two real culling
     * switches into one field.
     *
     * polyMode (real AMD hardware "use polyModeFront/Back at all"
     * master switch -- when false, real hardware always rasterizes as
     * filled triangles regardless of polyModeFront/Back) has no
     * separate on/off field in deko3d (DkRasterizerState's
     * polygonModeFront/Back are always active); handled here by
     * substituting Fill for both when polyMode is false, matching real
     * hardware's actual behavior in that case rather than just
     * ignoring the flag.
     *
     * polyOffsetFrontEnable/BackEnable/ParaEnable (real per-context
     * depth-bias enable flags) have no deko3d equivalent -- deko3d's
     * dkCmdBufSetDepthBias (used by GX2SetPolygonOffset above) has no
     * separate enable/disable state of its own, consistent with how
     * GX2SetPolygonOffset already documents always applying its bias
     * unconditionally; these three flags are read but intentionally
     * unused here, a known, honest gap rather than a silent drop. */
    uint32_t front_face = ctx->r[3];
    uint32_t cull_front = ctx->r[4];
    uint32_t cull_back = ctx->r[5];
    uint32_t poly_mode = ctx->r[6];
    uint32_t poly_mode_front = ctx->r[7];
    uint32_t poly_mode_back = ctx->r[8];
    uint32_t poly_offset_front_enable = ctx->r[9];
    uint32_t poly_offset_back_enable = ctx->r[10];
    uint32_t poly_offset_para_enable = ppc_load_u32(ctx, ctx->r[1] + 8);
    DkRasterizerState *state = &g_bramble_gx2.rasterizer_state;

    (void)poly_offset_front_enable; /* no deko3d equivalent -- see comment above */
    (void)poly_offset_back_enable;  /* no deko3d equivalent -- see comment above */
    (void)poly_offset_para_enable;  /* no deko3d equivalent -- see comment above */

    /* Reads/writes the persistent shadow copy (see BrambleGx2State's
     * field comment) -- GX2SetCullOnlyControl below and
     * GX2SetRasterizerClipControl further down are real subsets/
     * neighbors of this same hardware register and share this same
     * object. */
    bramble_gx2_apply_cull_state(state, front_face, cull_front, cull_back);

    if (poly_mode) {
        state->polygonModeFront = bramble_gx2_polygon_mode_to_dk(poly_mode_front);
        state->polygonModeBack = bramble_gx2_polygon_mode_to_dk(poly_mode_back);
    } else {
        state->polygonModeFront = DkPolygonMode_Fill;
        state->polygonModeBack = DkPolygonMode_Fill;
    }

    dkCmdBufBindRasterizerState(g_bramble_gx2.cmdbuf, state);
}

static inline void ppc_import_gx2_GX2SetCullOnlyControl(PpcContext *ctx) {
    /* void GX2SetCullOnlyControl(GX2FrontFace frontFace, BOOL cullFront,
     * BOOL cullBack) -- real, confirmed signature (wut's
     * gx2/registers.h), a real 3-parameter subset of
     * GX2SetPolygonControl above (same real hardware register, culling
     * fields only) -- reuses the exact same frontFace/cullMode
     * translation, leaving the shadow rasterizer state's polygon fill
     * mode/depth-clip fields untouched (unlike a local default, which
     * would silently reset whatever GX2SetPolygonControl/
     * GX2SetRasterizerClipControl already set there). */
    uint32_t front_face = ctx->r[3];
    uint32_t cull_front = ctx->r[4];
    uint32_t cull_back = ctx->r[5];
    DkRasterizerState *state = &g_bramble_gx2.rasterizer_state;

    bramble_gx2_apply_cull_state(state, front_face, cull_front, cull_back);

    dkCmdBufBindRasterizerState(g_bramble_gx2.cmdbuf, state);
}

static inline void ppc_import_gx2_GX2SetRasterizerClipControl(PpcContext *ctx) {
    /* void GX2SetRasterizerClipControl(BOOL rasterizer, BOOL
     * zclipEnable) -- real signature confirmed against wut's
     * gx2/registers.h. `rasterizer` is real hardware's rasterizer-stage
     * on/off switch (disabling it real-mode-skips rasterization
     * entirely, used for e.g. transform-feedback-only passes) -- maps
     * directly onto deko3d's `DkRasterizerState.rasterizerEnable`,
     * confirmed as the same real concept by name and by deko3d.h's own
     * default (enabled). `zclipEnable` (real hardware "clip primitives
     * against the near/far depth planes") is the real *inverse* of
     * deko3d's `depthClampEnable` (depth-clamp *disables* depth
     * clipping in favor of clamping depth values to [0,1] instead) --
     * confirmed by deko3d.h's own default (depthClampEnable=0, i.e.
     * clipping-on, matching GX2's own real default of zclipEnable=TRUE)
     * -- so zclipEnable maps to `!depthClampEnable`, not a direct
     * passthrough. Reads/writes the persistent shadow copy, same
     * reasoning as GX2SetPolygonControl/GX2SetCullOnlyControl above,
     * which share this same combined object. */
    uint32_t rasterizer = ctx->r[3];
    uint32_t zclip_enable = ctx->r[4];
    DkRasterizerState *state = &g_bramble_gx2.rasterizer_state;

    state->rasterizerEnable = rasterizer ? 1 : 0;
    state->depthClampEnable = zclip_enable ? 0 : 1;

    dkCmdBufBindRasterizerState(g_bramble_gx2.cmdbuf, state);
}

static inline void ppc_import_gx2_GX2SetDepthOnlyControl(PpcContext *ctx) {
    /* void GX2SetDepthOnlyControl(BOOL depthTest, BOOL depthWrite,
     * GX2CompareFunction depthCompare) -- real, confirmed signature
     * (wut's gx2/registers.h), a real 3-parameter subset of
     * GX2SetDepthStencilControl above (same real hardware register,
     * depth fields only) -- reuses the exact same depth-field
     * translation, leaving the shadow depth-stencil state's stencil
     * fields untouched (unlike a local default, which would silently
     * reset whatever GX2SetDepthStencilControl already set there),
     * matching the real semantics of "depth only". */
    uint32_t depth_test = ctx->r[3];
    uint32_t depth_write = ctx->r[4];
    uint32_t depth_compare = ctx->r[5];
    DkDepthStencilState *state = &g_bramble_gx2.depth_stencil_state;

    state->depthTestEnable = depth_test ? 1 : 0;
    state->depthWriteEnable = depth_write ? 1 : 0;
    state->depthCompareOp = bramble_gx2_compare_func_to_dk(depth_compare);

    dkCmdBufBindDepthStencilState(g_bramble_gx2.cmdbuf, state);
}

static inline void ppc_import_gx2_GX2SetStencilMask(PpcContext *ctx) {
    /* void GX2SetStencilMask(uint8_t frontMask, uint8_t frontWriteMask,
     * uint8_t frontRef, uint8_t backMask, uint8_t backWriteMask,
     * uint8_t backRef) -- real signature confirmed against wut's
     * gx2/registers.h, 6 real params, all fit in r3-r8 (each promoted
     * to a full register per the real PPC32 ABI, standard for
     * sub-word integer args). Real deko3d equivalent is
     * dkCmdBufSetStencil(cmdbuf, face, mask, funcRef, funcMask), one
     * real call per face -- its `mask` parameter is the real stencil
     * *write* mask and `funcMask` is the real stencil *compare* mask
     * (confirmed by the field naming/order matching deko3d's own
     * DkDepthStencilState set-up convention, where write and compare
     * are always kept as two distinct real GPU state values), so
     * GX2's frontWriteMask/backWriteMask map to deko3d's `mask` and
     * GX2's frontMask/backMask (the real compare masks) map to
     * deko3d's `funcMask`. */
    uint32_t front_mask = ctx->r[3];
    uint32_t front_write_mask = ctx->r[4];
    uint32_t front_ref = ctx->r[5];
    uint32_t back_mask = ctx->r[6];
    uint32_t back_write_mask = ctx->r[7];
    uint32_t back_ref = ctx->r[8];

    dkCmdBufSetStencil(g_bramble_gx2.cmdbuf, DkFace_Front, (uint8_t)front_write_mask, (uint8_t)front_ref, (uint8_t)front_mask);
    dkCmdBufSetStencil(g_bramble_gx2.cmdbuf, DkFace_Back, (uint8_t)back_write_mask, (uint8_t)back_ref, (uint8_t)back_mask);
}

static inline void ppc_import_gx2_GX2SetTargetChannelMasks(PpcContext *ctx) {
    /* void GX2SetTargetChannelMasks(GX2ChannelMask mask0, mask1, ...,
     * mask7) -- real signature confirmed against wut's
     * gx2/registers.h: 8 real per-render-target params, one
     * GX2ChannelMask each, all fitting exactly in r3-r10 (PPC32's
     * entire integer argument register file -- a real "every register
     * used" case, not the usual <8-arg function). GX2ChannelMask
     * (confirmed against wut's gx2/enum.h: R=1, G=2, B=4, A=8, ORed
     * together) uses the identical bit layout to deko3d's own
     * DkColorMask (deko3d.h: R=1<<0, G=1<<1, B=1<<2, A=1<<3) -- a
     * direct 1:1 passthrough into DkColorWriteState's per-target mask,
     * no translation table needed, unlike most other GX2<->deko3d enum
     * mappings in this file.
     *
     * Reads/writes the persistent `g_bramble_gx2.channel_masks` value,
     * combined with GX2SetColorControl's `color_write_enable` via
     * `bramble_gx2_rebind_color_write_state` (see its own comment) --
     * both real, independent GX2 calls that affect the same real
     * per-target mask bits, now genuinely independent instead of
     * whichever one happened to run last silently overwriting the
     * other's setting. */
    DkColorWriteState packed;
    uint32_t i;

    dkColorWriteStateDefaults(&packed);
    for (i = 0; i < 8; i++) {
        dkColorWriteStateSetMask(&packed, i, ctx->r[3 + i]);
    }
    g_bramble_gx2.channel_masks = packed.masks;
    bramble_gx2_rebind_color_write_state();
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

static inline void ppc_import_gx2_GX2ClearColor(PpcContext *ctx) {
    /* void GX2ClearColor(GX2ColorBuffer *colorBuffer, float red, float
     * green, float blue, float alpha) -- real args: r3=colorBuffer
     * (ignored, see below), f1-f4=r,g,b,a.
     *
     * Real, honestly-documented simplification: the real `colorBuffer`
     * argument (a full real `GX2ColorBuffer`/`GX2Surface` describing an
     * arbitrary render target, possibly an off-screen texture) is not
     * yet parsed -- there's no real surface/texture-to-deko3d-image
     * binding path yet (that needs the still-unattempted AMD tiling
     * math, or a documented simplification of its own, for real pixel
     * data interpretation). This always clears whichever real Switch
     * swapchain framebuffer is currently the active render target
     * instead -- correct only for the common "clear the actual screen
     * you're about to draw the frame to" case, not for clearing an
     * arbitrary off-screen surface. Real, visible, testable progress
     * for that common case; a known, documented gap for the other. */
    (void)ctx;
    bramble_gx2_ensure_frame_acquired();
    dkCmdBufClearColorFloat(g_bramble_gx2.cmdbuf, 0, DkColorMask_RGBA,
                            (float)ctx->f[1], (float)ctx->f[2], (float)ctx->f[3], (float)ctx->f[4]);
}

static inline void ppc_import_gx2_GX2SwapScanBuffers(PpcContext *ctx) {
    /* void GX2SwapScanBuffers(void) -- real behavior presents the TV
     * scan buffer (and, on real hardware, the separate GamePad/DRC scan
     * buffer -- this runtime has only one real display target, the
     * Switch's own screen, so there's no second buffer to swap here).
     * Submits whatever was recorded into the persistent command buffer
     * since the last swap (state changes, GX2ClearColor, and -- once
     * implemented -- real draw calls), presents the acquired
     * framebuffer, and resets for the next frame. If nothing this frame
     * ever called something that acquires a framebuffer (e.g. a frame
     * with no GX2ClearColor/draw calls at all), this is a real, safe
     * no-op -- there's nothing to present.
     *
     * Real bug fixed here, found via a real on-hardware test (not
     * caught by compiling/linking, which is why this needed real
     * hardware to find): the command buffer was never being reset
     * between frames, so every frame's recorded commands kept
     * accumulating in the same fixed BRAMBLE_GX2_CMD_MEM_SIZE (64KB)
     * pool forever -- devkitPro's own official deko3d Example01 this
     * project otherwise follows closely avoids this entirely by
     * recording its rendering commands exactly *once* into static
     * command lists at startup and replaying the same lists every
     * frame, never re-recording; this shim's design instead re-records
     * fresh commands every single frame (necessary here, since a real
     * recompiled game's actual GX2 calls per frame aren't known ahead
     * of time the way a fixed demo scene's are), which means it -- not
     * the reference example -- is the one that actually needs to
     * reclaim that memory each frame. `dkQueueWaitIdle` first is a
     * real, deliberate simplification: waits for the GPU to fully
     * finish the frame just presented before reusing its command
     * memory, trading real pipelining/performance for straightforward
     * correctness at this "does it work at all yet" stage -- a real,
     * known place to come back to once double-buffered command memory
     * (a separate real memory block per frame-in-flight) is worth the
     * added complexity. */
    (void)ctx;
    if (g_bramble_gx2.acquired_slot < 0) return;
    dkQueueSubmitCommands(g_bramble_gx2.queue, dkCmdBufFinishList(g_bramble_gx2.cmdbuf));
    g_bramble_gx2.submitted_timestamp = bramble_gx2_host_ticks();
    dkQueuePresentImage(g_bramble_gx2.queue, g_bramble_gx2.swapchain, g_bramble_gx2.acquired_slot);
    dkQueueWaitIdle(g_bramble_gx2.queue);
    g_bramble_gx2.retired_timestamp = g_bramble_gx2.submitted_timestamp;
    g_bramble_gx2.swap_count++;
    g_bramble_gx2.flip_count++;
    dkCmdBufClear(g_bramble_gx2.cmdbuf);
    g_bramble_gx2.acquired_slot = -1;
}

static inline void ppc_import_gx2_GX2Flush(PpcContext *ctx) {
    /* void GX2Flush(void) -- real behavior submits whatever's been
     * recorded so far to the GPU without waiting for it to finish and
     * without presenting (that's GX2SwapScanBuffers' job), so the CPU
     * can keep recording the next batch of GX2 calls while the GPU
     * works through this one. Maps directly onto
     * dkCmdBufFinishList+dkQueueSubmitCommands -- the same real submit
     * step GX2SwapScanBuffers already performs, just without the
     * present/waitIdle/clear that make that one a full frame boundary.
     * Real, deliberate simplification carried over from
     * GX2SwapScanBuffers' own documented one: this project's command
     * memory isn't double-buffered yet, so a flush here still shares
     * the same persistent `cmdbuf`/`BRAMBLE_GX2_CMD_MEM_SIZE` pool --
     * safe because deko3d's command allocator is append-only until an
     * explicit `dkCmdBufClear` (only called on the real frame boundary
     * in GX2SwapScanBuffers), not a ring buffer that could be
     * overwritten mid-frame. Safe (and correct per deko3d's own
     * multi-list-per-cmdbuf usage pattern) to call with nothing
     * recorded yet -- submits an empty list. Also stamps
     * `submitted_timestamp` (see GX2GetLastSubmittedTimeStamp/
     * GX2WaitTimeStamp below), since this is a real submit point same
     * as GX2SwapScanBuffers'. */
    (void)ctx;
    dkQueueSubmitCommands(g_bramble_gx2.queue, dkCmdBufFinishList(g_bramble_gx2.cmdbuf));
    g_bramble_gx2.submitted_timestamp = bramble_gx2_host_ticks();
}

static inline void ppc_import_gx2_GX2DrawDone(PpcContext *ctx) {
    /* BOOL GX2DrawDone(void) -- real signature confirmed against wut's
     * gx2/event.h (returns BOOL, not void). Real Cemu HLE
     * (GX2_Event.cpp) implements it as "flush the pipeline, then wait
     * on the just-submitted timestamp, return that wait's result" --
     * i.e. a real GX2Flush followed by a real
     * GX2WaitTimeStamp(GX2GetLastSubmittedTimeStamp()), not a bare
     * wait. Mirrored here: submits whatever's pending (same as
     * GX2Flush above), blocks until the GPU is fully idle
     * (dkQueueWaitIdle), stamps `retired_timestamp` caught up to
     * `submitted_timestamp`, and returns TRUE -- correct per real
     * semantics since an unconditional dkQueueWaitIdle always
     * completes (this runtime has no timeout/cancellation path for
     * GX2WaitTimeStamp to have failed on). */
    (void)ctx;
    dkQueueSubmitCommands(g_bramble_gx2.queue, dkCmdBufFinishList(g_bramble_gx2.cmdbuf));
    g_bramble_gx2.submitted_timestamp = bramble_gx2_host_ticks();
    dkQueueWaitIdle(g_bramble_gx2.queue);
    g_bramble_gx2.retired_timestamp = g_bramble_gx2.submitted_timestamp;
    ctx->r[3] = 1; /* TRUE */
}

static inline void ppc_import_gx2_GX2WaitForVsync(PpcContext *ctx) {
    /* void GX2WaitForVsync(void) -- real signature confirmed against
     * wut's gx2/event.h. Real hardware blocks the calling thread until
     * the next real vertical blank. This runtime has no real vsync
     * signal distinct from "the GPU has fully retired the last
     * submitted work" (same reasoning already used for
     * GX2GetSwapStatus's `lastVsync` above -- dkQueuePresentImage +
     * the unconditional dkQueueWaitIdle already used throughout this
     * file effectively synchronize to the display's actual present
     * cadence), so this maps onto the same real dkQueueWaitIdle used
     * by GX2DrawDone/GX2SwapScanBuffers, stamping `retired_timestamp`
     * caught up to `submitted_timestamp` the same way. Real, honest
     * simplification: a dedicated hardware vblank interrupt is not
     * modeled separately from GPU-idle, consistent with this whole
     * file's synchronous-for-now design (see GX2SwapScanBuffers'
     * own `dkQueueWaitIdle` comment).
     *
     * Real correctness fix: like GX2DrawDone, this must submit
     * whatever's currently only *recorded* in `cmdbuf` before waiting
     * -- `dkQueueWaitIdle` alone only waits on work already submitted
     * to the GPU, so a game that records state/draw calls and then
     * calls GX2WaitForVsync without an intervening GX2Flush/
     * GX2SwapScanBuffers would otherwise get back a "synced" result
     * while that work was never actually sent to the GPU. */
    (void)ctx;
    dkQueueSubmitCommands(g_bramble_gx2.queue, dkCmdBufFinishList(g_bramble_gx2.cmdbuf));
    g_bramble_gx2.submitted_timestamp = bramble_gx2_host_ticks();
    dkQueueWaitIdle(g_bramble_gx2.queue);
    g_bramble_gx2.retired_timestamp = g_bramble_gx2.submitted_timestamp;
}

static inline void ppc_import_gx2_GX2GetLastSubmittedTimeStamp(PpcContext *ctx) {
    /* OSTime GX2GetLastSubmittedTimeStamp(void) -- real 64-bit OSTime
     * return, split across r3(high)/r4(low) per the real PPC32 ABI's
     * 64-bit return convention (same convention already used by
     * cafeos_coreinit_sync.h's OSGetTime). See the
     * `submitted_timestamp` field comment above for what this actually
     * measures here (a host monotonic tick at each real submit point,
     * not a real Wii U-magnitude tick count). */
    uint64_t t = g_bramble_gx2.submitted_timestamp;
    ctx->r[3] = (uint32_t)(t >> 32);
    ctx->r[4] = (uint32_t)t;
}

static inline void ppc_import_gx2_GX2GetRetiredTimeStamp(PpcContext *ctx) {
    /* OSTime GX2GetRetiredTimeStamp(void) -- same real 64-bit OSTime
     * return convention as GX2GetLastSubmittedTimeStamp above. Since
     * every real submit point in this runtime (GX2Flush/GX2DrawDone/
     * GX2SwapScanBuffers) is immediately followed by a real, blocking
     * dkQueueWaitIdle except GX2Flush itself, `retired_timestamp` only
     * catches up to `submitted_timestamp` on an actual wait -- a real,
     * honest reflection of "GX2Flush submits without waiting" even
     * though this runtime has no real async completion tracking of its
     * own. */
    uint64_t t = g_bramble_gx2.retired_timestamp;
    ctx->r[3] = (uint32_t)(t >> 32);
    ctx->r[4] = (uint32_t)t;
}

static inline void ppc_import_gx2_GX2WaitTimeStamp(PpcContext *ctx) {
    /* BOOL GX2WaitTimeStamp(OSTime time) -- real 64-bit OSTime arg,
     * split across r3(high)/r4(low) per the real PPC32 ABI (same
     * convention already used by cafeos_coreinit_sync.h's
     * OSSleepTicks). Real behavior blocks until `time` has retired.
     * Since this runtime always fully idles the GPU on any real wait
     * (see GX2DrawDone/GX2SwapScanBuffers above), if `time` is already
     * <=`retired_timestamp` there's nothing to wait for; otherwise a
     * real dkQueueWaitIdle catches `retired_timestamp` all the way up
     * to `submitted_timestamp` (the furthest this runtime can ever
     * retire to, since nothing is submitted beyond that point yet).
     * Always returns TRUE -- same "no timeout/cancellation path to
     * have failed on" reasoning as GX2DrawDone above. */
    uint64_t time = ((uint64_t)ctx->r[3] << 32) | (uint64_t)ctx->r[4];
    if (time > g_bramble_gx2.retired_timestamp) {
        dkQueueWaitIdle(g_bramble_gx2.queue);
        g_bramble_gx2.retired_timestamp = g_bramble_gx2.submitted_timestamp;
    }
    ctx->r[3] = 1; /* TRUE */
}

static inline void ppc_import_gx2_GX2GetSwapStatus(PpcContext *ctx) {
    /* void GX2GetSwapStatus(uint32_t *swapCount, uint32_t *flipCount,
     * OSTime *lastFlip, OSTime *lastVsync) -- real signature confirmed
     * against wut's gx2/event.h, 4 real guest-memory out-pointers in
     * r3-r6. Real GX2 lets any of these be NULL when the caller only
     * wants a subset -- guarded here the same way, not a guess: every
     * other guest-out-pointer shim in this project (e.g.
     * cafeos_coreinit_fs.h's FS* calls) already treats a NULL/0 guest
     * address as "caller doesn't want this one", consistent with how
     * real Cafe OS pointer args are conventionally optional unless
     * documented otherwise. `swapCount`/`flipCount` are real per-swap
     * counters (see GX2SwapScanBuffers above, which increments both --
     * this runtime never distinguishes a "swap" from a "flip" the way
     * real hardware's separate scan-buffer-flip vs. GX2SwapScanBuffers-call
     * concepts might, so both counters move together here, a
     * documented simplification). `lastFlip`/`lastVsync` both report
     * `retired_timestamp` -- this runtime has no separate real vsync
     * signal distinct from "the last frame's GPU work fully retired"
     * (dkQueueWaitIdle already blocks until then in
     * GX2SwapScanBuffers), so the two real hardware concepts collapse
     * to the same value here. */
    uint32_t swap_count_ptr = ctx->r[3];
    uint32_t flip_count_ptr = ctx->r[4];
    uint32_t last_flip_ptr = ctx->r[5];
    uint32_t last_vsync_ptr = ctx->r[6];

    if (swap_count_ptr) ppc_store_u32(ctx, swap_count_ptr, g_bramble_gx2.swap_count);
    if (flip_count_ptr) ppc_store_u32(ctx, flip_count_ptr, g_bramble_gx2.flip_count);
    if (last_flip_ptr) ppc_store_u64(ctx, last_flip_ptr, g_bramble_gx2.retired_timestamp);
    if (last_vsync_ptr) ppc_store_u64(ctx, last_vsync_ptr, g_bramble_gx2.retired_timestamp);
}

static inline void ppc_import_gx2_GX2SetEventCallback(PpcContext *ctx) {
    /* GX2DRCConnectCallback GX2SetEventCallback(GX2EventType type,
     * GX2EventCallbackFunction func, void *userData) -- real signature
     * confirmed against wut's gx2/event.h. `type` selects one of the 5
     * real GX2EventType slots (see BRAMBLE_GX2_NUM_EVENT_TYPES above);
     * `func`/`userData` are real *guest* addresses (a PPC function
     * pointer and its opaque argument), stored verbatim, not
     * dereferenced or called here.
     *
     * Real, honest, deliberately separate gap: this only implements
     * real *registration* -- storing what the game asked to be called
     * back for a real GX2EventType -- not real *invocation*.
     * Invocation would mean this runtime actually detecting each real
     * event (an end-of-pipe interrupt, a real vsync, a real flip, a
     * display-list overrun) and dispatching through `ppc_dispatch` to
     * the stored guest function address (this project's established
     * callback-invocation mechanism, already used for real by
     * cafeos_coreinit_fs.h's FS*WithPosAsync and cafeos_nsyshid.h) --
     * that needs deciding *when*, precisely, each of these 5 real
     * events should be considered to have fired in this runtime's
     * still-synchronous-for-now GX2 pipeline, which isn't confirmed
     * against real hardware/Cemu behavior yet, so it's not guessed at
     * here. A real, functioning improvement over doing nothing at all
     * (the callback is at least genuinely remembered, not silently
     * dropped), with the actual dispatch left as a clearly-flagged
     * follow-up rather than invented timing. Return value: wut's own
     * header names it `GX2DRCConnectCallback` (a different, unrelated
     * real typedef from this function's own `GX2EventCallbackFunction`
     * parameter type -- plausibly a wut documentation copy/paste quirk,
     * not confirmed either way), but real GX2's documented behavior for
     * this class of Set*Callback function is "returns the
     * previously-registered callback" -- returned here as the
     * best-effort real interpretation (this event type's previous
     * `func`, or 0 if none was registered yet) rather than leaving
     * `r[3]` holding stale garbage from whatever the caller's register
     * last held, which a real caller checking the return value (e.g.
     * "was one already registered?") could otherwise misread as a
     * valid address. */
    uint32_t type = ctx->r[3];
    uint32_t func = ctx->r[4];
    uint32_t user_data = ctx->r[5];
    uint32_t previous_func = 0;

    if (type < BRAMBLE_GX2_NUM_EVENT_TYPES) {
        previous_func = g_bramble_gx2.event_callback_func[type];
        g_bramble_gx2.event_callback_func[type] = func;
        g_bramble_gx2.event_callback_userdata[type] = user_data;
    }
    ctx->r[3] = previous_func;
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
static inline void ppc_import_gx2_GX2SetBlendControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetColorControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetAlphaTest(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetAlphaToMask(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetDepthStencilControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetPolygonControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetCullOnlyControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetRasterizerClipControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetDepthOnlyControl(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetStencilMask(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetTargetChannelMasks(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetPrimitiveRestartIndex(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2ClearColor(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SwapScanBuffers(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2Flush(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2DrawDone(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2WaitForVsync(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2GetLastSubmittedTimeStamp(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2GetRetiredTimeStamp(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2WaitTimeStamp(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2GetSwapStatus(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_gx2_GX2SetEventCallback(PpcContext *ctx) { (void)ctx; }

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

static inline void ppc_import_gx2_GX2SetTVEnable(PpcContext *ctx) {
    /* void GX2SetTVEnable(BOOL enable) -- real Wii U hardware has two
     * independent real scan-out targets (TV + GamePad/DRC); enabling/
     * disabling either changes what actually gets sent to that
     * display. This runtime has exactly one real display target (the
     * Switch's own screen, driven by GX2SwapScanBuffers' swapchain --
     * see that function's own "only one real display target" note),
     * so there's no second real output to gate. Accepted, not stored:
     * confirmed by recompiling the actual tfbGame_cafe.rpx that no
     * `GX2Get*TVEnable`-shaped getter is in this game's real import
     * list, so nothing here ever reads it back. Real, documented gap:
     * if the game ever disables the TV and expects nothing to appear
     * on-screen, this shim still presents every frame -- untested
     * against real hardware behavior since there's no signal in this
     * game's own calls that it relies on that. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetDRCEnable(PpcContext *ctx) {
    /* void GX2SetDRCEnable(BOOL enable) -- same real GamePad/DRC scan
     * target GX2SetTVEnable above gates, just the other one. Same
     * reasoning applies: no second real display target exists on this
     * runtime to enable/disable, and no real getter for it exists in
     * this game's actual import list, so accepted and not stored. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetTVScale(PpcContext *ctx) {
    /* void GX2SetTVScale(uint32_t x, uint32_t y) -- real hardware
     * scales the TV scan buffer's real output resolution independently
     * of its render resolution. This runtime's swapchain is a fixed
     * `BRAMBLE_GX2_FB_WIDTH`x`BRAMBLE_GX2_FB_HEIGHT` (see the framebuffer
     * setup above) with no real output-scaling stage of its own yet --
     * accepted, not stored, same "no real getter in this game's actual
     * import list to contradict it" reasoning as GX2SetSwapInterval
     * above. Real, documented gap, not silently assumed harmless: a
     * game relying on this to letterbox/scale non-native content would
     * render at the wrong apparent size until this is wired up for
     * real. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetDRCScale(PpcContext *ctx) {
    /* void GX2SetDRCScale(uint32_t x, uint32_t y) -- same real
     * per-scan-target output scaling GX2SetTVScale above is, just for
     * the GamePad/DRC target this runtime also has no second real
     * display for (see GX2SetDRCEnable above). Same reasoning: accepted,
     * not stored, no real getter in this game's actual import list. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetTVBuffer(PpcContext *ctx) {
    /* void GX2SetTVBuffer(void *buffer, uint32_t size, GX2TVRenderMode
     * tvRenderMode, GX2SurfaceFormat surfaceFormat, GX2BufferingMode
     * bufferingMode) -- real signature confirmed against wut's
     * gx2/display.h, 5 real params, all in r3-r7. Same real "no second
     * display target to attach a scan buffer to" reasoning as
     * GX2SetTVEnable/GX2SetTVScale above -- this runtime's one real
     * display target is entirely owned by GX2Init's own swapchain/
     * framebuffer setup, not a game-supplied buffer. Accepted, not
     * stored: no real getter for this in the actual tfbGame_cafe.rpx
     * import list. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetDRCBuffer(PpcContext *ctx) {
    /* void GX2SetDRCBuffer(void *buffer, uint32_t size, GX2DrcRenderMode
     * drcRenderMode, GX2SurfaceFormat surfaceFormat, GX2BufferingMode
     * bufferingMode) -- same real per-scan-target buffer registration
     * GX2SetTVBuffer above is, just for the GamePad/DRC target this
     * runtime also has no second real display for (see
     * GX2SetDRCEnable above). Same reasoning: accepted, not stored. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2Invalidate(PpcContext *ctx) {
    /* void GX2Invalidate(GX2InvalidateMode mode, void *buffer,
     * uint32_t size) -- real signature confirmed against wut's
     * gx2/mem.h. Real hardware invalidates GPU-side caches for a CPU-
     * written buffer region before the GPU reads it (or the reverse).
     * A genuine no-op here, same reasoning already established for
     * DCFlushRange in cafeos_coreinit.h: this runtime has no modeled
     * GPU cache of its own to invalidate (deko3d/the real Switch
     * hardware handle their own real memory coherency internally), and
     * there's no real GX2 surface/texture upload path yet for `buffer`/
     * `size` to even correspond to actual GPU-visible memory (see
     * GX2CalcSurfaceSizeAndAlignment's own still-unattempted status in
     * docs/phase1d_import_surface.md) -- so there's genuinely nothing
     * real for this to do yet, not a shortcut around something that
     * matters. Backend-independent (works identically whether or not
     * deko3d is available), unlike most of this file's GX2* functions. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetPointLimits(PpcContext *ctx) {
    /* void GX2SetPointLimits(float min, float max) -- real signature
     * confirmed against wut's gx2/registers.h, 2 real float args
     * (f1-f2, GX2SetPointSize's own already-established float-arg
     * convention above). Real hardware clamps the effective point size
     * (set via GX2SetPointSize) to this [min, max] range; deko3d's own
     * `dkCmdBufSetPointSize` (already used by GX2SetPointSize above)
     * has no matching min/max clamp parameter or separate real API
     * found for one -- a genuine, documented gap, not a guess, since
     * there's no plausible deko3d call to guess at rather than simply
     * having none. Accepted, not stored. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetStreamOutEnable(PpcContext *ctx) {
    /* void GX2SetStreamOutEnable(BOOL enable) -- real signature
     * confirmed against wut's gx2/shaders.h. Real hardware transform
     * feedback (writing vertex-shader output to a real memory buffer
     * instead of/in addition to rasterizing) has no real deko3d bind/
     * enable call found in this project's actual deko3d.h (only a
     * `DkCounter_TransformFeedbackPrimitivesWritten` query counter
     * exists, no setup API) -- consistent with there being no real
     * vertex/geometry shader translation pipeline in this project yet
     * either (a separate, much larger unattempted problem, see
     * docs/phase1d_import_surface.md), so there's genuinely nothing for
     * a real transform-feedback buffer to attach to regardless.
     * Accepted, not stored -- a real, honest gap. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetTessellation(PpcContext *ctx) {
    /* void GX2SetTessellation(GX2TessellationMode tessellationMode,
     * GX2PrimitiveMode primitiveMode, GX2IndexType indexType) -- real
     * signature confirmed against wut's gx2/tessellation.h. Real AMD
     * hardware tessellation (a fixed-function tessellator stage between
     * hull and domain shaders) has no matching concept anywhere in
     * deko3d.h -- confirmed by its complete absence, not assumed; this
     * project also has no real hull/domain shader translation to feed
     * it regardless (shader translation itself remains unattempted, see
     * docs/phase1d_import_surface.md). Accepted, not stored -- a real,
     * honest gap, not a guess at unavailable state. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetMinTessellationLevel(PpcContext *ctx) {
    /* void GX2SetMinTessellationLevel(float min) -- same real
     * unimplemented-hardware-feature reasoning as GX2SetTessellation
     * above; this real per-edge tessellation-factor floor has nothing
     * in deko3d to configure. Accepted, not stored. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2SetMaxTessellationLevel(PpcContext *ctx) {
    /* void GX2SetMaxTessellationLevel(float max) -- same real
     * unimplemented-hardware-feature reasoning as GX2SetTessellation
     * above. Accepted, not stored. */
    (void)ctx;
}

static inline void ppc_import_gx2_GX2GetDisplayListWriteStatus(PpcContext *ctx) {
    /* BOOL GX2GetDisplayListWriteStatus(void) -- real signature
     * confirmed against wut's gx2/displaylist.h: reports whether the
     * GPU is currently recording into a display list opened by
     * GX2BeginDisplayListEx (real hardware toggles an internal flag
     * across that Begin/EndDisplayList pair). This shim has no real
     * display-list recording of its own yet (GX2BeginDisplayListEx/
     * GX2EndDisplayList/GX2CallDisplayList/GX2CopyDisplayList are all
     * still on docs/phase1d_import_surface.md's remaining list) -- so
     * a display list can genuinely never be open here, making FALSE
     * the real, honest current answer rather than a guess: there's no
     * invented "recording" state being reported on, just the true
     * absence of the feature. Documented as a real gap to revisit once
     * display-list recording exists, same as every other
     * not-yet-attempted GX2 feature in this file. */
    (void)ctx;
    ctx->r[3] = 0; /* FALSE */
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

/* ---- GX2DepthBuffer clear-value setters --------------------------------
 *
 * GX2SetClearDepth/GX2SetClearStencil/GX2SetClearDepthStencil write the
 * real depthClear/stencilClear fields directly into a guest
 * `GX2DepthBuffer` struct (confirmed real offsets: depthClear at 0x88,
 * a float; stencilClear at 0x8C, a uint32_t despite the real API taking
 * a uint8_t -- both WUT_CHECK_OFFSET-confirmed against wut's
 * gx2/surface.h). Confirmed against Cemu's real HLE
 * (src/Cafe/OS/libs/gx2/GX2_Blit.cpp): real hardware also submits a
 * DB_DEPTH_CLEAR/DB_STENCIL_CLEAR PM4 register write here, but this
 * runtime has no real PM4 command-stream model to submit into (see
 * `ppc_dispatch`/command-buffer notes elsewhere in this file) -- the
 * real, guest-visible effect these functions have on `GX2DepthBuffer`
 * itself is what's implemented; the immediate register write is a real
 * gap, consistent with how this shim already only tracks guest-visible
 * state rather than modeling the underlying real GPU command stream
 * byte-for-byte. Backend-independent (pure guest-memory writes, no
 * deko3d call), works identically on host and Switch. */

#define BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_DEPTH_OFFSET 0x88u
#define BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_STENCIL_OFFSET 0x8Cu

static inline void ppc_import_gx2_GX2SetClearDepth(PpcContext *ctx) {
    /* void GX2SetClearDepth(GX2DepthBuffer *depthBuffer, float depth) --
     * real args: r3=depthBuffer (pointer, integer sequence), f1=depth
     * (independent float sequence, real PPC32 SVR4 ABI). */
    uint32_t depth_buffer_addr = ctx->r[3];
    ppc_store_f32(ctx, depth_buffer_addr + BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_DEPTH_OFFSET, ctx->f[1]);
}

static inline void ppc_import_gx2_GX2SetClearStencil(PpcContext *ctx) {
    /* void GX2SetClearStencil(GX2DepthBuffer *depthBuffer,
     * uint8_t stencil) -- real args: both integers, r3=depthBuffer,
     * r4=stencil (widened to the real uint32_t field width, matching
     * the real GX2DepthBuffer struct's own `stencilClear` field type). */
    uint32_t depth_buffer_addr = ctx->r[3];
    uint32_t stencil = ctx->r[4] & 0xFFu;
    ppc_store_u32(ctx, depth_buffer_addr + BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_STENCIL_OFFSET, stencil);
}

static inline void ppc_import_gx2_GX2SetClearDepthStencil(PpcContext *ctx) {
    /* void GX2SetClearDepthStencil(GX2DepthBuffer *depthBuffer,
     * float depth, uint8_t stencil) -- real args: r3=depthBuffer
     * (integer sequence #1), f1=depth (independent float sequence),
     * r4=stencil (integer sequence #2) -- real PPC32 SVR4 ABI keeps the
     * integer and float argument sequences independent regardless of
     * their position in the real source-level parameter list. */
    uint32_t depth_buffer_addr = ctx->r[3];
    uint32_t stencil = ctx->r[4] & 0xFFu;
    ppc_store_f32(ctx, depth_buffer_addr + BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_DEPTH_OFFSET, ctx->f[1]);
    ppc_store_u32(ctx, depth_buffer_addr + BRAMBLE_GX2_DEPTH_BUFFER_CLEAR_STENCIL_OFFSET, stencil);
}

/* ---- GX2Sampler init family ------------------------------------------
 *
 * GX2Sampler is a real, tiny opaque struct: `uint32_t regs[3]` (12 bytes,
 * WUT_CHECK_SIZE-confirmed) holding three raw AMD "Latte" GPU hardware
 * register values (SQ_TEX_SAMPLER_WORD0/1/2) in the real, exact bit
 * layout real Wii U hardware uses -- these Init* functions just pack
 * guest-supplied enum/float arguments into those bits, matching what the
 * real GX2 library itself does (and what real recompiled code that later
 * peeks at a GX2Sampler's raw bytes, however unlikely, would still see
 * correctly). This shim doesn't guess the bit layout: every field
 * offset/width and every fixed-point scale factor below is taken
 * directly from Cemu's real HLE reimplementation
 * (src/Cafe/HW/Latte/ISA/LatteReg.h's LATTE_SQ_TEX_SAMPLER_WORD0/1/2_0
 * structs, and src/Cafe/OS/libs/gx2/GX2_Texture.cpp's actual
 * GX2InitSampler / GX2InitSamplerXYFilter / etc. bodies), not derived
 * or assumed. GX2's own enum values (GX2TexClampMode, GX2TexXYFilterMode,
 * GX2TexZFilterMode, GX2TexMipFilterMode, GX2TexBorderType,
 * GX2CompareFunction) are real, direct 1:1 hardware register encodings
 * with no translation needed -- confirmed by Cemu's own real
 * implementation passing them straight through with no lookup table of
 * its own either.
 *
 * No deko3d call happens here: unlike every other real GX2 function in
 * this file, GX2Sampler is pure guest-memory state -- the real, actual
 * binding to a deko3d DkSampler only happens once GX2SetPixelSampler/
 * GX2SetVertexSampler (still unimplemented) decode these same bits back
 * out, the same real two-step "build state struct, then bind it" shape
 * real hardware itself uses. This works identically on host and Switch
 * (no __SWITCH__ guard needed), same as every other pure-guest-memory-
 * struct shim in this project.
 */

#define BRAMBLE_GX2_SAMPLER_WORD0_OFFSET 0u
#define BRAMBLE_GX2_SAMPLER_WORD1_OFFSET 4u
#define BRAMBLE_GX2_SAMPLER_WORD2_OFFSET 8u

static inline uint32_t bramble_gx2_bitfield_set(uint32_t word, uint32_t value, uint32_t shift, uint32_t width) {
    uint32_t mask = ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << shift;
    return (word & ~mask) | ((value << shift) & mask);
}

static inline void ppc_import_gx2_GX2InitSampler(PpcContext *ctx) {
    /* void GX2InitSampler(GX2Sampler *sampler, GX2TexClampMode clampMode,
     * GX2TexXYFilterMode minMagFilterMode) -- real body (GX2_Texture.cpp):
     * sets CLAMP_X/Y/Z all to clampMode, XY_MAG/MIN_FILTER both to
     * minMagFilterMode, Z_FILTER/MIP_FILTER to POINT(1), TEX_ARRAY_OVERRIDE
     * to true, WORD1's MAX_LOD to 0x3FF (the real, full, "no limit" 10-bit
     * max), and WORD2's TYPE field to E_SAMPLER_TYPE::UKN1(1) -- an
     * unconfirmed-meaning hardware field this project preserves exactly
     * as real GX2 sets it, not guessed independently. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t clamp_mode = ctx->r[4];
    uint32_t filter_mode = ctx->r[5];
    uint32_t word0 = 0, word1 = 0, word2 = 0;

    word0 = bramble_gx2_bitfield_set(word0, clamp_mode, 0, 3);  /* CLAMP_X */
    word0 = bramble_gx2_bitfield_set(word0, clamp_mode, 3, 3);  /* CLAMP_Y */
    word0 = bramble_gx2_bitfield_set(word0, clamp_mode, 6, 3);  /* CLAMP_Z */
    word0 = bramble_gx2_bitfield_set(word0, filter_mode, 9, 3);  /* XY_MAG_FILTER */
    word0 = bramble_gx2_bitfield_set(word0, filter_mode, 12, 3); /* XY_MIN_FILTER */
    word0 = bramble_gx2_bitfield_set(word0, 1u, 15, 2); /* Z_FILTER = POINT */
    word0 = bramble_gx2_bitfield_set(word0, 1u, 17, 2); /* MIP_FILTER = POINT */
    word0 = bramble_gx2_bitfield_set(word0, 1u, 25, 1); /* TEX_ARRAY_OVERRIDE = true */

    word1 = bramble_gx2_bitfield_set(word1, 0x3FFu, 10, 10); /* MAX_LOD = 0x3FF */

    word2 = bramble_gx2_bitfield_set(word2, 1u, 31, 1); /* TYPE = UKN1 */

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD1_OFFSET, word1);
    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD2_OFFSET, word2);
}

static inline void ppc_import_gx2_GX2InitSamplerXYFilter(PpcContext *ctx) {
    /* void GX2InitSamplerXYFilter(GX2Sampler *sampler,
     * GX2TexXYFilterMode filterMag, GX2TexXYFilterMode filterMin,
     * GX2TexAnisoRatio maxAniso) -- real body: if maxAniso==0, sets
     * XY_MAG/MIN_FILTER directly and MAX_ANISO_RATIO to 0; otherwise
     * remaps POINT(0)->ANISO_POINT(4) and LINEAR/BILINEAR(1)->
     * ANISO_BILINEAR(5) (real hardware's separate anisotropic filter
     * mode values -- BICUBIC(2) has no anisotropic counterpart in real
     * hardware, not exercised here since this shim only translates
     * inputs it's given, matching Cemu's own `cemu_assert_debug`-guarded
     * real behavior of not handling that case either) before storing,
     * and sets MAX_ANISO_RATIO to the real requested ratio. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t filter_mag = ctx->r[4];
    uint32_t filter_min = ctx->r[5];
    uint32_t max_aniso = ctx->r[6];
    uint32_t word0 = ppc_load_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET);

    if (max_aniso != 0u) {
        if (filter_mag == 0u) filter_mag = 4u; /* POINT -> ANISO_POINT */
        else if (filter_mag == 1u) filter_mag = 5u; /* LINEAR -> ANISO_BILINEAR */
        if (filter_min == 0u) filter_min = 4u;
        else if (filter_min == 1u) filter_min = 5u;
    }

    word0 = bramble_gx2_bitfield_set(word0, filter_mag, 9, 3);
    word0 = bramble_gx2_bitfield_set(word0, filter_min, 12, 3);
    word0 = bramble_gx2_bitfield_set(word0, max_aniso, 19, 3);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
}

static inline void ppc_import_gx2_GX2InitSamplerZMFilter(PpcContext *ctx) {
    /* void GX2InitSamplerZMFilter(GX2Sampler *sampler,
     * GX2TexZFilterMode zFilter, GX2TexMipFilterMode mipFilter) --
     * real, direct field writes, no remapping. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t z_filter = ctx->r[4];
    uint32_t mip_filter = ctx->r[5];
    uint32_t word0 = ppc_load_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET);

    word0 = bramble_gx2_bitfield_set(word0, z_filter, 15, 2);
    word0 = bramble_gx2_bitfield_set(word0, mip_filter, 17, 2);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
}

static inline void ppc_import_gx2_GX2InitSamplerLOD(PpcContext *ctx) {
    /* void GX2InitSamplerLOD(GX2Sampler *sampler, float minLod,
     * float maxLod, float lodBias) -- real args: r3=sampler (pointer,
     * integer sequence), f1/f2/f3=minLod/maxLod/lodBias (independent
     * float sequence, real PPC32 SVR4 ABI). Real fixed-point encoding
     * (Cemu's actual GX2InitSamplerLOD body, not derived): minLod
     * clamped to >=0, maxLod clamped to <=16.0 first (a real, documented
     * special-case clamp Cemu itself applies for known real game
     * compatibility), then each float is scaled by 64.0 and floored;
     * minLod/maxLod (10-bit unsigned fields) clamp to [0, 1023], lodBias
     * (12-bit signed field) clamps to [-2048, 2047]. */
    uint32_t sampler_addr = ctx->r[3];
    float min_lod = (float)ctx->f[1];
    float max_lod = (float)ctx->f[2];
    float lod_bias = (float)ctx->f[3];
    int32_t i_min_lod, i_max_lod, i_lod_bias;
    uint32_t word1;

    if (min_lod < 0.0f) min_lod = 0.0f;
    if (max_lod > 16.0f) max_lod = 16.0f;

    i_min_lod = (int32_t)floorf(min_lod * 64.0f);
    i_max_lod = (int32_t)floorf(max_lod * 64.0f);
    i_lod_bias = (int32_t)floorf(lod_bias * 64.0f);

    if (i_min_lod < 0) i_min_lod = 0;
    if (i_min_lod > 1023) i_min_lod = 1023;
    if (i_max_lod < 0) i_max_lod = 0;
    if (i_max_lod > 1023) i_max_lod = 1023;
    if (i_lod_bias < -2048) i_lod_bias = -2048;
    if (i_lod_bias > 2047) i_lod_bias = 2047;

    word1 = 0;
    word1 = bramble_gx2_bitfield_set(word1, (uint32_t)i_min_lod, 0, 10);
    word1 = bramble_gx2_bitfield_set(word1, (uint32_t)i_max_lod, 10, 10);
    word1 = bramble_gx2_bitfield_set(word1, (uint32_t)(i_lod_bias & 0xFFF), 20, 12);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD1_OFFSET, word1);
}

static inline void ppc_import_gx2_GX2InitSamplerClamping(PpcContext *ctx) {
    /* void GX2InitSamplerClamping(GX2Sampler *sampler,
     * GX2TexClampMode clampX, GX2TexClampMode clampY,
     * GX2TexClampMode clampZ) -- real, direct field writes. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t clamp_x = ctx->r[4];
    uint32_t clamp_y = ctx->r[5];
    uint32_t clamp_z = ctx->r[6];
    uint32_t word0 = ppc_load_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET);

    word0 = bramble_gx2_bitfield_set(word0, clamp_x, 0, 3);
    word0 = bramble_gx2_bitfield_set(word0, clamp_y, 3, 3);
    word0 = bramble_gx2_bitfield_set(word0, clamp_z, 6, 3);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
}

static inline void ppc_import_gx2_GX2InitSamplerBorderType(PpcContext *ctx) {
    /* void GX2InitSamplerBorderType(GX2Sampler *sampler,
     * GX2TexBorderType borderColorType) -- real, direct field write. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t border_type = ctx->r[4];
    uint32_t word0 = ppc_load_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET);

    word0 = bramble_gx2_bitfield_set(word0, border_type, 22, 2);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
}

static inline void ppc_import_gx2_GX2InitSamplerDepthCompare(PpcContext *ctx) {
    /* void GX2InitSamplerDepthCompare(GX2Sampler *sampler,
     * GX2CompareFunction depthCompareFunction) -- real, direct field
     * write; unlike every deko3d-facing GX2CompareFunction usage
     * elsewhere in this file, no DkCompareOp translation happens here
     * because this raw value is going straight into a real hardware
     * register bit layout that already matches GX2's own encoding, not
     * into a deko3d struct. */
    uint32_t sampler_addr = ctx->r[3];
    uint32_t depth_compare = ctx->r[4];
    uint32_t word0 = ppc_load_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET);

    word0 = bramble_gx2_bitfield_set(word0, depth_compare, 26, 3);

    ppc_store_u32(ctx, sampler_addr + BRAMBLE_GX2_SAMPLER_WORD0_OFFSET, word0);
}

#endif /* BRAMBLE_CAFEOS_GX2_H */
