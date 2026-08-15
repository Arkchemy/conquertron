#ifndef BRAMBLE_CAFEOS_VPAD_H
#define BRAMBLE_CAFEOS_VPAD_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- vpad (Wii U GamePad input). Real
 * signatures confirmed against devkitPro/wut's vpad/input.h.
 *
 * Same reasoning as cafeos_padscore.h: no real Wii U GamePad input
 * source exists in this runtime, so every function here honestly
 * reports "no samples" rather than fabricating input. VPADRead's
 * caller-provided `buffers` is deliberately left untouched, matching
 * KPADRead's reasoning -- a 0-count return means no caller should read
 * from it.
 *
 * VPADGetTPCalibratedPoint (touch-panel calibration) is deliberately
 * NOT implemented here: it writes a real calibrated VPADTouchData based
 * on a real calibration transform this project hasn't verified, and
 * since no real touch events are ever generated in this runtime to
 * begin with, there's nothing to correctly calibrate -- tracked as a
 * gap rather than guessed at.
 */
static inline void ppc_import_vpad_VPADRead(PpcContext *ctx) {
    /* int32_t VPADRead(chan, buffers, count, outError) -- r6 is
     * outError (VPADReadError*); VPAD_READ_NO_SAMPLES = -1, confirmed
     * against the real vpad/input.h enum. */
    if (ctx->r[6] != 0) {
        ppc_store_u32(ctx, ctx->r[6], (uint32_t)-1);
    }
    ctx->r[3] = 0; /* 0 samples read */
}
static inline void ppc_import_vpad_VPADControlMotor(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_vpad_VPADStopMotor(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_vpad_VPADSetAccParam(PpcContext *ctx) { (void)ctx; }

#endif /* BRAMBLE_CAFEOS_VPAD_H */
