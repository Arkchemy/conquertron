#ifndef BRAMBLE_CAFEOS_NN_ACT_H
#define BRAMBLE_CAFEOS_NN_ACT_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- nn::act (Wii U account management).
 * Real signatures confirmed against devkitPro/wut's nn/act/client_cpp.h:
 * `nn::Result Initialize(void)`, `SlotNo GetSlotNo(void)` (SlotNo is
 * uint8_t).
 *
 * No real Wii U account/profile system exists in this runtime.
 * `Initialize` reports success (`nn::Result`'s all-zero value is the
 * real, universal "no error" sentinel) so real code gets past its
 * startup account check rather than treating account access as a fatal
 * failure it likely has no real recovery path for. `GetSlotNo` reports
 * slot 0 -- a real, valid value on actual hardware (the default/primary
 * account slot), reasonable here since this runtime only ever models a
 * single implicit "current user."
 */
static inline void ppc_import_nn_act_Initialize__Q2_2nn3actFv(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_nn_act_GetSlotNo__Q2_2nn3actFv(PpcContext *ctx) { ctx->r[3] = 0; }

#endif /* BRAMBLE_CAFEOS_NN_ACT_H */
