#ifndef BRAMBLE_CAFEOS_NN_AC_H
#define BRAMBLE_CAFEOS_NN_AC_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- nn_ac (network auto-connect).
 *
 * Real nn_ac functions return nn::Result, a Nintendo-wide bitfield-encoded
 * status type used across the whole nn::* namespace (not the simple
 * int32_t-with-0-meaning-OK convention FSStatus/MCPError use) -- its
 * exact bit layout isn't independently confirmed here. Rather than
 * fabricate a specific encoding and risk it silently mismatching real
 * ACIsFailure's real bit-level check, this shim is instead internally
 * self-consistent: ACConnect/ACGetStatus/ACGetAssignedAddress all report
 * failure using the *same* sentinel value ACIsFailure (also implemented
 * here) treats as failure. Real code that asks "did this succeed?" via
 * ACIsFailure(result) -- the documented, intended way to check an
 * nn::Result, not direct bit inspection -- gets the right answer
 * end-to-end within this shim even without matching real hardware's
 * exact bit pattern.
 *
 * All of this deliberately, honestly reports "no network" rather than
 * faking a working connection: there is no real network stack behind
 * this runtime at all. This isn't even a shortcut relative to real
 * hardware -- a real Wii U with no wifi configured, or in airplane mode,
 * hits this exact same "ACConnect fails, game proceeds offline" path,
 * so any real game already has to handle it gracefully.
 */
#define BRAMBLE_AC_RESULT_SUCCESS 0u
#define BRAMBLE_AC_RESULT_FAILURE 1u

static inline void ppc_import_nn_ac_ACInitialize(PpcContext *ctx) { ctx->r[3] = BRAMBLE_AC_RESULT_SUCCESS; }
static inline void ppc_import_nn_ac_ACFinalize(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_nn_ac_ACConnect(PpcContext *ctx) { ctx->r[3] = BRAMBLE_AC_RESULT_FAILURE; }
static inline void ppc_import_nn_ac_ACGetStatus(PpcContext *ctx) { ctx->r[3] = BRAMBLE_AC_RESULT_FAILURE; }

/* ACGetAssignedAddress(ACIpAddress *addr) -- r3=out_addr. Writes 0.0.0.0
 * (no address, matching the "no connection" state) and reports failure,
 * consistent with never having connected. */
static inline void ppc_import_nn_ac_ACGetAssignedAddress(PpcContext *ctx) {
    ppc_store_u32(ctx, ctx->r[3], 0);
    ctx->r[3] = BRAMBLE_AC_RESULT_FAILURE;
}

/* ACIsFailure(Result result) -- BOOL. The self-consistency anchor: as
 * long as this agrees with the sentinel values above, real code's own
 * "did it work?" checks come out correct regardless of whether
 * BRAMBLE_AC_RESULT_FAILURE happens to match real hardware's actual
 * nn::Result failure encoding. */
static inline void ppc_import_nn_ac_ACIsFailure(PpcContext *ctx) {
    ctx->r[3] = (ctx->r[3] != BRAMBLE_AC_RESULT_SUCCESS) ? 1u : 0u;
}

#endif /* BRAMBLE_CAFEOS_NN_AC_H */
