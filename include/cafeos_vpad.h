#ifndef ARKCHEMY_CAFEOS_VPAD_H
#define ARKCHEMY_CAFEOS_VPAD_H

#include "ppc_runtime.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

/*
 * Phase 1d CafeOS runtime shim -- vpad (Wii U GamePad input). Real
 * signatures confirmed against devkitPro/wut's vpad/input.h.
 *
 * Real Switch controller input wired into VPADRead as of 2026-08-20,
 * per direct owner request (button presses previously reached nothing
 * -- both this and padscore's KPADRead honestly reported "no samples"
 * always, since no real input source existed yet). Real `VPADStatus`
 * struct layout (offsets, 0xAC total size) and `VPADButtons` bit
 * values below are taken directly from devkitPro/wut's own public
 * vpad/input.h (an open-source Wii U SDK reimplementation this project
 * already treats as an authoritative real-signature reference
 * elsewhere), not guessed from this binary's own disassembly -- its
 * own `mulli r12, r0, 0xac` sample-array stride (found searching for
 * this binary's real VPADRead call sites) independently confirms the
 * 0xAC size matches. Switch face buttons are mapped to Wii U ones by
 * *name*, not screen position (A->A, B->B, X->X, Y->Y) -- both
 * controllers happen to arrange them identically (A right, B bottom, X
 * top, Y left) anyway, so the two mappings agree either way. Every
 * other real VPADStatus field this runtime has no real Switch-side
 * equivalent for (DRC accelerometer/gyro/angle/touchscreen/magnetometer,
 * headphone jack, battery, mic) is honestly left zeroed, matching real
 * hardware's own idle/disconnected-sensor default rather than
 * fabricating plausible-looking values.
 *
 * Same reasoning as cafeos_padscore.h for KPADRead: no analogous real
 * Wii Remote input source exists on Switch, so that one still honestly
 * reports "no samples" -- only VPAD (Wii U GamePad) has a real Switch
 * hardware equivalent (this project's own controller) to wire up.
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
 * `x`/`y`/`touched`/`validity` straight through -- still never
 * exercised against real touch input either way, since this runtime
 * has no real DRC touchscreen to read from.
 */
#ifdef __SWITCH__
typedef struct {
    uint32_t held;
    float stick_lx, stick_ly, stick_rx, stick_ry;
} ArkchemyVpadState;
extern ArkchemyVpadState g_arkchemy_vpad;

/* Called once per frame by main.c's own main loop, right after its
 * existing padUpdate(&pad) call -- keeps this shim's real button/stick
 * state current without needing its own separate PadState (main.c
 * already owns and updates one). Deliberately not mutex-protected
 * against ppc_import_vpad_VPADRead's own read of g_arkchemy_vpad below
 * (called from the separate game thread) -- a torn or one-frame-stale
 * read of a handful of plain integers/floats is the same accepted,
 * low-severity raciness already used elsewhere in this runtime (e.g.
 * main.c's own "racy" g_ctx.r[] register snapshot), not a real
 * correctness risk like the console's own shared *rendering* state was
 * (see main.c's g_console_mutex fix for why that one genuinely needed
 * one). */
static inline void arkchemy_vpad_update(const PadState *pad) {
    u64 k = padGetButtons(pad);
    uint32_t held = 0;
    if (k & HidNpadButton_A) held |= 0x8000u;          /* VPAD_BUTTON_A */
    if (k & HidNpadButton_B) held |= 0x4000u;          /* VPAD_BUTTON_B */
    if (k & HidNpadButton_X) held |= 0x2000u;          /* VPAD_BUTTON_X */
    if (k & HidNpadButton_Y) held |= 0x1000u;          /* VPAD_BUTTON_Y */
    if (k & HidNpadButton_Left) held |= 0x0800u;       /* VPAD_BUTTON_LEFT */
    if (k & HidNpadButton_Right) held |= 0x0400u;      /* VPAD_BUTTON_RIGHT */
    if (k & HidNpadButton_Up) held |= 0x0200u;         /* VPAD_BUTTON_UP */
    if (k & HidNpadButton_Down) held |= 0x0100u;       /* VPAD_BUTTON_DOWN */
    if (k & HidNpadButton_ZL) held |= 0x0080u;         /* VPAD_BUTTON_ZL */
    if (k & HidNpadButton_ZR) held |= 0x0040u;         /* VPAD_BUTTON_ZR */
    if (k & HidNpadButton_L) held |= 0x0020u;          /* VPAD_BUTTON_L */
    if (k & HidNpadButton_R) held |= 0x0010u;          /* VPAD_BUTTON_R */
    if (k & HidNpadButton_Plus) held |= 0x0008u;       /* VPAD_BUTTON_PLUS */
    if (k & HidNpadButton_Minus) held |= 0x0004u;      /* VPAD_BUTTON_MINUS */
    if (k & HidNpadButton_StickR) held |= 0x00020000u; /* VPAD_BUTTON_STICK_R */
    if (k & HidNpadButton_StickL) held |= 0x00040000u; /* VPAD_BUTTON_STICK_L */
    g_arkchemy_vpad.held = held;

    HidAnalogStickState ls = padGetStickPos(pad, 0);
    HidAnalogStickState rs = padGetStickPos(pad, 1);
    g_arkchemy_vpad.stick_lx = (float)ls.x / 32767.0f;
    g_arkchemy_vpad.stick_ly = (float)ls.y / 32767.0f;
    g_arkchemy_vpad.stick_rx = (float)rs.x / 32767.0f;
    g_arkchemy_vpad.stick_ry = (float)rs.y / 32767.0f;
}
#endif /* __SWITCH__ */

static inline void ppc_import_vpad_VPADRead(PpcContext *ctx) {
    /* int32_t VPADRead(chan, buffers, count, outError) -- r6 is
     * outError (VPADReadError*); VPAD_READ_NO_SAMPLES = -1, confirmed
     * against the real vpad/input.h enum. */
#ifdef __SWITCH__
    uint32_t buffers_addr = ctx->r[4];
    uint32_t count = ctx->r[5];
    if (count == 0) {
        if (ctx->r[6] != 0) ppc_store_u32(ctx, ctx->r[6], (uint32_t)-1); /* VPAD_READ_NO_SAMPLES */
        ctx->r[3] = 0;
        return;
    }

    /* One real sample per call, matching this runtime's real 60fps
     * VPADRead call rate against a real 60fps input source -- no real
     * backlog of multiple distinct samples to report, same reasoning
     * KPADRead's own comment already gives for "no samples" elsewhere.
     * `trigger`/`release` need the *previous* call's held state, kept
     * here (not in g_arkchemy_vpad, which is real "current" state, not
     * "last read") since only VPADRead's own real semantics need it. */
    static uint32_t s_prev_held = 0;
    uint32_t held = g_arkchemy_vpad.held;
    uint32_t trigger = held & ~s_prev_held;
    uint32_t release = s_prev_held & ~held;
    s_prev_held = held;

    /* Zero the entire real 0xAC-byte VPADStatus struct first (see this
     * file's own top comment for the real, wut-confirmed layout), then
     * fill in only the specific fields this runtime has a real Switch-
     * side value for. */
    for (uint32_t off = 0; off < 0xAC; off += 4) ppc_store_u32(ctx, buffers_addr + off, 0);
    ppc_store_u32(ctx, buffers_addr + 0x00, held);    /* hold */
    ppc_store_u32(ctx, buffers_addr + 0x04, trigger);
    ppc_store_u32(ctx, buffers_addr + 0x08, release);
    ppc_store_f32(ctx, buffers_addr + 0x0C, g_arkchemy_vpad.stick_lx);
    ppc_store_f32(ctx, buffers_addr + 0x10, g_arkchemy_vpad.stick_ly);
    ppc_store_f32(ctx, buffers_addr + 0x14, g_arkchemy_vpad.stick_rx);
    ppc_store_f32(ctx, buffers_addr + 0x18, g_arkchemy_vpad.stick_ry);

    if (ctx->r[6] != 0) ppc_store_u32(ctx, ctx->r[6], 0); /* VPAD_READ_SUCCESS */
    ctx->r[3] = 1;
#else
    if (ctx->r[6] != 0) {
        ppc_store_u32(ctx, ctx->r[6], (uint32_t)-1);
    }
    ctx->r[3] = 0; /* 0 samples read */
#endif
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

#endif /* ARKCHEMY_CAFEOS_VPAD_H */
