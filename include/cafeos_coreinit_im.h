#ifndef ARKCHEMY_CAFEOS_COREINIT_IM_H
#define ARKCHEMY_CAFEOS_COREINIT_IM_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit "IM" (idle/inactivity
 * management, screen auto-dim) functions.
 *
 * Lower confidence than the FS shim: IMEnableDim/IMDisableDim/
 * IMIsDimEnabled appear in wut's coreinit.def export list (confirming
 * they're real, exported functions) but have no documented prototype in
 * any public header -- unlike FSOpenFile etc, which came from a real,
 * fetched header with an exact signature. Implemented on the simplest
 * plausible shape for a real enable/disable/query trio (no arguments,
 * IMIsDimEnabled returns a 0/1 bool in r3) since nothing in the real
 * export list contradicts it, but flagged here rather than presented as
 * verified the way the FS functions are.
 *
 * Real hardware effect (dimming the actual screen after inactivity)
 * isn't modeled at all -- this only tracks the enabled/disabled flag
 * real code can toggle and query, since nothing about screen brightness
 * exists in this runtime yet (no display output at all until gx2 is
 * implemented).
 *
 * Same multi-translation-unit caveat as cafeos_coreinit_fs.h: the flag
 * below is file-scope `static`, so it won't share state correctly if
 * this header is included from more than one compiled .c file.
 */
extern int g_ppc_im_dim_enabled; /* real definition in cafeos_state.c -- see its own file comment */

static inline void ppc_import_coreinit_IMEnableDim(PpcContext *ctx) {
    (void)ctx;
    g_ppc_im_dim_enabled = 1;
}

static inline void ppc_import_coreinit_IMDisableDim(PpcContext *ctx) {
    (void)ctx;
    g_ppc_im_dim_enabled = 0;
}

static inline void ppc_import_coreinit_IMIsDimEnabled(PpcContext *ctx) {
    ctx->r[3] = (uint32_t)g_ppc_im_dim_enabled;
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_IM_H */
