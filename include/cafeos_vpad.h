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
 * VPADGetTPCalibratedPoint: real signature confirmed against
 * vpad/input.h -- `void VPADGetTPCalibratedPoint(VPADChan chan,
 * VPADTouchData *calibratedData, const VPADTouchData *uncalibratedData)`.
 * The real transform (confirmed via Cemu's actual HLE source,
 * src/Cafe/OS/libs/vpad/vpad.cpp) is
 * `calibrated = raw - (calibrationParam.{x,y} * calibrationParam.scale_{x,y})`
 * against a real, confirmed `VPADTouchCalibrationParam` this game never
 * sets (`VPADSetTPCalibrationParam` isn't in this binary's real import
 * list, confirmed by recompiling the actual `tfbGame_cafe.rpx`) -- so
 * the calibration param this transform would use is real hardware's own
 * zero-initialized default (matching Cemu's own default-constructed
 * `VPADTPCalibrationParam{}`), which makes the formula an honest
 * identity transform (`x - 0*0 == x`), not a guessed shortcut. Copies
 * `x`/`y`/`touched`/`validity` straight through. No real touch events
 * are ever generated in this runtime to begin with (matches `VPADRead`
 * above), so this transform is never exercised against real user input
 * either way, same as everywhere else in this file.
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

static inline void ppc_import_vpad_VPADGetTPCalibratedPoint(PpcContext *ctx) {
    uint32_t calibrated_addr = ctx->r[4], raw_addr = ctx->r[5];
    ppc_store_u16(ctx, calibrated_addr + 0x0, ppc_load_u16(ctx, raw_addr + 0x0)); /* x */
    ppc_store_u16(ctx, calibrated_addr + 0x2, ppc_load_u16(ctx, raw_addr + 0x2)); /* y */
    ppc_store_u16(ctx, calibrated_addr + 0x4, ppc_load_u16(ctx, raw_addr + 0x4)); /* touched */
    ppc_store_u16(ctx, calibrated_addr + 0x6, ppc_load_u16(ctx, raw_addr + 0x6)); /* validity */
}

#endif /* BRAMBLE_CAFEOS_VPAD_H */
