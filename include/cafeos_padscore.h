#ifndef ARKCHEMY_CAFEOS_PADSCORE_H
#define ARKCHEMY_CAFEOS_PADSCORE_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- padscore (Wii Remote/Classic-style
 * input, the KPAD/WPAD APIs). Real signatures confirmed against
 * devkitPro/wut's padscore/kpad.h and padscore/wpad.h.
 *
 * This runtime has no real Wii Remote/Classic Controller input source at
 * all (this project targets Switch, not a Wii Remote) -- every function
 * here honestly reports "nothing connected, no samples" rather than
 * fabricating button presses or motion data, the same reasoning already
 * used for nn_ac's "honestly report no network" shim. This lets real
 * code that polls these APIs and gracefully handles "no controller"
 * (which every well-behaved Wii U title has to, since Wii Remotes are
 * physically optional) run without crashing, rather than pretending to
 * emulate real Wii Remote hardware.
 *
 * KPADRead returns a sample count (0 = none read) per real
 * padscore/kpad.h; the caller-provided `data` buffer is deliberately
 * left untouched rather than writing a guessed-at KPADStatus layout
 * into it -- safe precisely because a real 0-count return means no
 * caller should read from that buffer at all.
 */
static inline void ppc_import_padscore_KPADInit(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_padscore_KPADRead(PpcContext *ctx) { ctx->r[3] = 0; /* uint32_t: 0 samples read */ }
static inline void ppc_import_padscore_KPADSetConnectCallback(PpcContext *ctx) { ctx->r[3] = 0; /* returns the previous callback -- none was ever set */ }
static inline void ppc_import_padscore_KPADEnableDPD(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_padscore_KPADDisableDPD(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_padscore_KPADSetAccParam(PpcContext *ctx) { (void)ctx; }

/* WPAD_ERROR_NO_CONTROLLER = -1, WPAD_EXT_DEV_NOT_FOUND = 0xfd -- both
 * confirmed against the real padscore/wpad.h enum values. */
static inline void ppc_import_padscore_WPADProbe(PpcContext *ctx) {
    if (ctx->r[4] != 0) {
        ppc_store_u32(ctx, ctx->r[4], 0xfd); /* *outExtensionType = WPAD_EXT_DEV_NOT_FOUND */
    }
    ctx->r[3] = (uint32_t)-1; /* WPAD_ERROR_NO_CONTROLLER */
}
static inline void ppc_import_padscore_WPADControlMotor(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_padscore_WPADDisconnect(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_padscore_WPADEnableURCC(PpcContext *ctx) { (void)ctx; }

#endif /* ARKCHEMY_CAFEOS_PADSCORE_H */
