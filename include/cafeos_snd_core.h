#ifndef ARKCHEMY_CAFEOS_SND_CORE_H
#define ARKCHEMY_CAFEOS_SND_CORE_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- snd_core (the AX audio mixer library).
 * Real signatures confirmed against devkitPro/wut's sndcore2/core.h,
 * sndcore2/device.h, and sndcore2/drcvs.h, cross-checked against Cemu's
 * real HLE implementation (src/Cafe/OS/libs/snd_core/) where wut didn't
 * cover something.
 *
 * This runtime has no real audio mixer/output path at all yet (matches
 * this project's own priority notes: gx2 graphics is the real critical
 * path, audio is a later "fastest win" once something exists worth
 * hearing) -- every function here is a genuine no-op / always-success
 * stub for the *global* AX subsystem config surface (init/quit, device
 * mix routing, DRC virtual-surround mode, aux/final-mix callback
 * registration). None of these have anywhere in this shim that would
 * ever actually read the state they set, so "accept the call, report
 * success, do nothing" is honestly correct here, not a placeholder --
 * the same reasoning already used for proc_ui's callback registration
 * functions.
 *
 * AXIsInit's state (unlike proc_ui's callbacks) genuinely needs to
 * persist across calls to be correct -- tracked as a file-scope static,
 * same known limitation already documented for coreinit_fs's handle
 * table: won't share correctly across more than one compiled `.c` file
 * in the same program.
 *
 * The AXVoice-pointer family (AXAcquireVoice/AXFreeVoice/AXVoiceBegin/
 * AXVoiceEnd/AXIsVoiceRunning/AXGetVoiceOffsets/AXSetVoice*, 19
 * functions) is now implemented too, using the same fixed-region
 * allocator approach `cafeos_coreinit_mem.h` introduced for MEM*: a
 * dedicated fixed pool of real, dereferenceable 0x58-byte slots
 * (AXVoice's real confirmed size per wut's WUT_CHECK_SIZE) reserved at
 * 0xE100-0xEC00 in `PpcContext::mem` -- clear of both `cafeos_coreinit_
 * mem.h`'s MEM1/MEM2/errno reservations and the stack-top region, same
 * documented-placeholder trade-off as those. `AXAcquireVoice` hands
 * back a real address into this pool; every setter that has a real,
 * WUT-confirmed offset in the public `AXVoice` struct (state, the
 * `AXVoiceOffsets` sub-struct, priority/callback/userContext) writes
 * through to the real address for genuine round-trip correctness
 * (`AXGetVoiceOffsets` reads back exactly what `AXSetVoiceOffsets`
 * wrote). Setters whose real target data lives in Nintendo's *internal*
 * mixer-only structures with no corresponding public `AXVoice` field at
 * all (confirmed by reading Cemu's real HLE source, not guessed) --
 * ADPCM data, source type/ratio storage, device mix, voice-effects
 * volume -- are genuine accept-and-discard no-ops, since nothing in
 * this shim (or real hardware, for that matter) ever reads them back
 * through the public struct either. `AXSetVoiceSrcRatio` still does a
 * real, meaningful check: real hardware's `AX_VOICE_RATIO_RESULT`
 * enum rejects a non-positive ratio, so this validates and reports
 * that correctly without needing to store anything.
 *
 * `AXIsVoiceRunning` always reports FALSE, which is honestly correct
 * here rather than a shortcut: on real hardware this reflects whether
 * the DSP mixer's *next processed frame* actually started the voice,
 * confirmed via Cemu's HLE source to be tracked in a separate internal
 * struct a real audio-frame callback updates -- since this runtime has
 * no mixer and never processes an audio frame, a voice genuinely never
 * "runs" here, the same honesty standard as vpad/padscore reporting
 * "no real input source" rather than fabricating samples.
 *
 * `AXVoiceBegin`/`AXVoiceEnd` real behavior is a per-thread reentrancy
 * *count* (Cemu's `__AXVoiceProtection`, keyed by which real OS thread
 * currently holds the voice) -- simplified here to a flat
 * acquire-returns-1/release-returns-0, since this runtime has no
 * concept of "current thread" to key that count by in the first place
 * (matches the no-real-concurrency reasoning used throughout this
 * shim). A known, documented simplification of the real reentrancy
 * semantics, not a guess at unconfirmed behavior.
 *
 * AXSetMaxVoices and AXRegisterExceedCallback: lower confidence than
 * everything else in this file. Both are real, confirmed exports (both
 * appear in wut's own cafe/sndcore2.def alongside AXGetMaxVoices), but
 * neither wut nor Cemu's HLE (which doesn't implement or even stub
 * them) documents a prototype for either -- implemented on the simplest
 * plausible shape by symmetry with their confirmed siblings
 * (AXGetMaxVoices takes no args/returns sint32; AXRegisterFrameCallback
 * takes/returns one AXFrameCallback), same "no confirmed primary
 * source" caveat already used for cafeos_ghs_runtime.h's
 * __ghs_flock_* functions.
 */
extern int g_ax_initialized; /* real definition in cafeos_state.c -- see its own file comment */

static inline void ppc_import_snd_core_AXInit(PpcContext *ctx) { (void)ctx; g_ax_initialized = 1; }
static inline void ppc_import_snd_core_AXInitWithParams(PpcContext *ctx) { (void)ctx; g_ax_initialized = 1; }
static inline void ppc_import_snd_core_AXQuit(PpcContext *ctx) { (void)ctx; g_ax_initialized = 0; }
static inline void ppc_import_snd_core_AXIsInit(PpcContext *ctx) { ctx->r[3] = (uint32_t)g_ax_initialized; }

static inline void ppc_import_snd_core_AXSetDefaultMixerSelect(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; /* AX_RESULT_SUCCESS */ }

static inline void ppc_import_snd_core_AXGetDeviceMode(PpcContext *ctx) {
    /* AXResult AXGetDeviceMode(AXDeviceType type, AXDeviceMode *mode) --
     * r3=type, r4=mode out-pointer. AX_DEVICE_MODE_UNKNOWN=0 is the only
     * value wut's own enum defines (Nintendo never documented the real
     * set), so 0 is the honest "best known value", not a placeholder. */
    if (ctx->r[4] != 0) {
        ppc_store_u32(ctx, ctx->r[4], 0);
    }
    ctx->r[3] = 0; /* AX_RESULT_SUCCESS */
}
static inline void ppc_import_snd_core_AXRegisterDeviceFinalMixCallback(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }
static inline void ppc_import_snd_core_AXRegisterAuxCallback(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }
static inline void ppc_import_snd_core_AXSetDeviceUpsampleStage(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }
static inline void ppc_import_snd_core_AXSetDRCVSMode(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }

/* Lower confidence -- see file comment. */
static inline void ppc_import_snd_core_AXSetMaxVoices(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }
static inline void ppc_import_snd_core_AXRegisterExceedCallback(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; /* no previous callback was ever set */ }

/* --- AXVoice pool -- see file comment for the real-offset/no-op split. --- */

#define ARKCHEMY_AXVOICE_POOL_BASE 0xE100u
#define ARKCHEMY_AXVOICE_SLOT_SIZE 0x58u
#define ARKCHEMY_AXVOICE_MAX 32

/* Real, WUT_CHECK_OFFSET-confirmed offsets within AXVoice. */
#define ARKCHEMY_AXVOICE_OFF_INDEX 0x00u
#define ARKCHEMY_AXVOICE_OFF_STATE 0x04u
#define ARKCHEMY_AXVOICE_OFF_PRIORITY 0x1Cu
#define ARKCHEMY_AXVOICE_OFF_CALLBACK 0x20u
#define ARKCHEMY_AXVOICE_OFF_USERCONTEXT 0x24u
#define ARKCHEMY_AXVOICE_OFF_OFFSETS 0x34u /* AXVoiceOffsets: dataType u16@+0, loopingEnabled u16@+2, loopOffset u32@+4, endOffset u32@+8, currentOffset u32@+c, data ptr@+0x10 */

extern int g_arkchemy_ax_voice_used[ARKCHEMY_AXVOICE_MAX]; /* real definition in cafeos_state.c -- see its own file comment */

static inline void ppc_import_snd_core_AXAcquireVoice(PpcContext *ctx) {
    /* AXVoice *AXAcquireVoice(uint32_t priority, AXVoiceCallbackFn callback, void *userContext) */
    uint32_t priority = ctx->r[3];
    uint32_t callback = ctx->r[4];
    uint32_t user_context = ctx->r[5];
    int i;
    for (i = 0; i < ARKCHEMY_AXVOICE_MAX; i++) {
        uint32_t addr, b;
        if (g_arkchemy_ax_voice_used[i]) continue;
        g_arkchemy_ax_voice_used[i] = 1;
        addr = ARKCHEMY_AXVOICE_POOL_BASE + (uint32_t)i * ARKCHEMY_AXVOICE_SLOT_SIZE;
        for (b = 0; b < ARKCHEMY_AXVOICE_SLOT_SIZE; b++) ppc_store_u8(ctx, addr + b, 0);
        ppc_store_u32(ctx, addr + ARKCHEMY_AXVOICE_OFF_INDEX, (uint32_t)i);
        ppc_store_u32(ctx, addr + ARKCHEMY_AXVOICE_OFF_PRIORITY, priority);
        ppc_store_u32(ctx, addr + ARKCHEMY_AXVOICE_OFF_CALLBACK, callback);
        ppc_store_u32(ctx, addr + ARKCHEMY_AXVOICE_OFF_USERCONTEXT, user_context);
        ctx->r[3] = addr;
        return;
    }
    ctx->r[3] = 0; /* pool exhausted */
}

static inline void ppc_import_snd_core_AXFreeVoice(PpcContext *ctx) {
    /* void AXFreeVoice(AXVoice *voice) */
    uint32_t addr = ctx->r[3];
    if (addr >= ARKCHEMY_AXVOICE_POOL_BASE) {
        uint32_t idx = (addr - ARKCHEMY_AXVOICE_POOL_BASE) / ARKCHEMY_AXVOICE_SLOT_SIZE;
        if (idx < ARKCHEMY_AXVOICE_MAX) g_arkchemy_ax_voice_used[idx] = 0;
    }
}

static inline void ppc_import_snd_core_AXVoiceBegin(PpcContext *ctx) { (void)ctx; ctx->r[3] = 1; /* simplified reentrancy count -- see file comment */ }
static inline void ppc_import_snd_core_AXVoiceEnd(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; }
static inline void ppc_import_snd_core_AXIsVoiceRunning(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; /* no mixer ever processes a frame -- see file comment */ }

static inline void ppc_import_snd_core_AXSetVoiceState(PpcContext *ctx) {
    /* void AXSetVoiceState(AXVoice *voice, AXVoiceState state) */
    ppc_store_u32(ctx, ctx->r[3] + ARKCHEMY_AXVOICE_OFF_STATE, ctx->r[4]);
}

static inline void ppc_import_snd_core_AXSetVoiceOffsets(PpcContext *ctx) {
    /* void AXSetVoiceOffsets(AXVoice *voice, AXVoiceOffsets *offsets) --
     * copies the 5 scalar fields; `data` (the pointer at +0x10) is
     * copied too for round-trip completeness even though nothing here
     * dereferences it. */
    uint32_t voice = ctx->r[3], src = ctx->r[4], dst = ctx->r[3] + ARKCHEMY_AXVOICE_OFF_OFFSETS;
    (void)voice;
    ppc_store_u16(ctx, dst + 0x0, ppc_load_u16(ctx, src + 0x0));
    ppc_store_u16(ctx, dst + 0x2, ppc_load_u16(ctx, src + 0x2));
    ppc_store_u32(ctx, dst + 0x4, ppc_load_u32(ctx, src + 0x4));
    ppc_store_u32(ctx, dst + 0x8, ppc_load_u32(ctx, src + 0x8));
    ppc_store_u32(ctx, dst + 0xc, ppc_load_u32(ctx, src + 0xc));
    ppc_store_u32(ctx, dst + 0x10, ppc_load_u32(ctx, src + 0x10));
}

static inline void ppc_import_snd_core_AXGetVoiceOffsets(PpcContext *ctx) {
    /* void AXGetVoiceOffsets(AXVoice *voice, AXVoiceOffsets *offsets) -- inverse of Set above */
    uint32_t src = ctx->r[3] + ARKCHEMY_AXVOICE_OFF_OFFSETS, dst = ctx->r[4];
    ppc_store_u16(ctx, dst + 0x0, ppc_load_u16(ctx, src + 0x0));
    ppc_store_u16(ctx, dst + 0x2, ppc_load_u16(ctx, src + 0x2));
    ppc_store_u32(ctx, dst + 0x4, ppc_load_u32(ctx, src + 0x4));
    ppc_store_u32(ctx, dst + 0x8, ppc_load_u32(ctx, src + 0x8));
    ppc_store_u32(ctx, dst + 0xc, ppc_load_u32(ctx, src + 0xc));
    ppc_store_u32(ctx, dst + 0x10, ppc_load_u32(ctx, src + 0x10));
}

static inline void ppc_import_snd_core_AXSetVoiceCurrentOffset(PpcContext *ctx) {
    /* void AXSetVoiceCurrentOffset(AXVoice *voice, uint32_t offset) */
    ppc_store_u32(ctx, ctx->r[3] + ARKCHEMY_AXVOICE_OFF_OFFSETS + 0xc, ctx->r[4]);
}

static inline void ppc_import_snd_core_AXSetVoiceEndOffset(PpcContext *ctx) {
    /* void AXSetVoiceEndOffset(AXVoice *voice, uint32_t offset) */
    ppc_store_u32(ctx, ctx->r[3] + ARKCHEMY_AXVOICE_OFF_OFFSETS + 0x8, ctx->r[4]);
}

static inline void ppc_import_snd_core_AXSetVoiceLoop(PpcContext *ctx) {
    /* void AXSetVoiceLoop(AXVoice *voice, AXVoiceLoop loop) */
    ppc_store_u16(ctx, ctx->r[3] + ARKCHEMY_AXVOICE_OFF_OFFSETS + 0x2, (uint16_t)ctx->r[4]);
}

static inline void ppc_import_snd_core_AXSetVoiceSrcRatio(PpcContext *ctx) {
    /* AXVoiceSrcRatioResult AXSetVoiceSrcRatio(AXVoice *voice, float ratio) --
     * real hardware validates the ratio and rejects <= 0; nothing to
     * store since the real ratio storage is internal-only (see file
     * comment).
     *
     * Real bug fix: this previously read `ratio` from ctx->r[4], as if
     * float args shared the integer GPR sequence with the preceding
     * pointer arg. Real PPC32 SVR4 ABI keeps GPRs and FPRs as
     * completely independent argument-register sequences -- a
     * (pointer, float) parameter list places the pointer in r3 and the
     * float in f1, *not* r3/r4 -- confirmed empirically by compiling a
     * real (void*, float) test function with this project's own
     * zig-cc-based toolchain and reading recomp's real generated
     * output (`stw r3` for the pointer, `stfs f1` for the float),
     * not just asserted from a general ABI rule. r[4] held whatever
     * the caller happened to leave there, not the real ratio. */
    float ratio = (float)ctx->f[1];
    ctx->r[3] = (uint32_t)((ratio > 0.0f) ? 0 /* AX_VOICE_RATIO_RESULT_SUCCESS */ : (uint32_t)-1 /* _LESS_THAN_ZERO */);
}

/* Internal-only on real hardware (confirmed via Cemu's HLE source) --
 * no corresponding public AXVoice field exists to round-trip through,
 * so these genuinely are accept-and-discard no-ops, not shortcuts. */
static inline void ppc_import_snd_core_AXSetVoiceType(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceSrcType(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceSrc(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceAdpcm(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceAdpcmLoop(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceVe(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_core_AXSetVoiceDeviceMix(PpcContext *ctx) { (void)ctx; ctx->r[3] = 0; /* AX_RESULT_SUCCESS */ }

#endif /* ARKCHEMY_CAFEOS_SND_CORE_H */
