#ifndef BRAMBLE_CAFEOS_SND_CORE_H
#define BRAMBLE_CAFEOS_SND_CORE_H

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
 * Deliberately NOT implemented here: the entire AXVoice-pointer family
 * (AXAcquireVoice/AXFreeVoice/AXVoiceBegin/AXVoiceEnd/AXIsVoiceRunning/
 * AXGetVoiceOffsets/AXSetVoice*, ~19 functions) -- real behavior hands
 * back a pointer into an OS-owned voice pool (AXVoice is exactly 0x58
 * bytes per wut's WUT_CHECK_SIZE, confirmed real, not caller-allocated
 * the way OSMutex/OSEvent are), and this runtime's `PpcContext::mem` is
 * a small fixed 64KB scratch region with no general allocator exposed
 * to shim code for handing out real backing storage per voice. Faking a
 * handle without real backing storage would silently break the moment
 * any real game code reads an AXVoice field directly instead of going
 * through an accessor (the struct layout is real and documented, so
 * that's not a hypothetical). Needs a real voice-pool allocation model,
 * tracked as its own follow-up, not guessed at here -- same standard as
 * nsyshid's HIDRead callback-invocation gap and coreinit's
 * OSCreateThread/OSThread-struct gap.
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
static int g_ax_initialized = 0;

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

#endif /* BRAMBLE_CAFEOS_SND_CORE_H */
