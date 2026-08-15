#ifndef BRAMBLE_CAFEOS_COREINIT_DYNLOAD_H
#define BRAMBLE_CAFEOS_COREINIT_DYNLOAD_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit's OSDynLoad_* (dynamic RPL
 * module loading). Previously deliberately out of scope on the
 * assumption real code would only ever use this to load *other
 * recompiled modules*, which this project's whole approach doesn't
 * support (everything statically known ahead of time).
 *
 * Investigating the real, actual `tfbGame_cafe.rpx` (not guessed --
 * recompiled it and read the generated call sites directly) found a
 * different, real, narrower usage pattern: the binary calls
 * `OSDynLoad_Acquire("nn_erreula", &handle)` once, then
 * `OSDynLoad_FindExport` for a handful of named `nn::erreula::*`
 * exports (the "error viewer applet" library -- system dialog boxes).
 * This is a well-known, real Cafe OS pattern distinct from loading
 * another game module: optional/late-bound *system* libraries that
 * real games are already expected to handle failing gracefully.
 *
 * Confirmed directly from the real binary's own control flow, not
 * assumed: right after the `OSDynLoad_Acquire` call, real code does
 * `or. r4, r3, r3` / `beq` on the return value -- if nonzero (failure),
 * it falls through to a real `OSReport` error-log call and continues,
 * rather than crashing or looping. Since this runtime never has *any*
 * real dynamically-loadable module (matches the project's existing
 * static-linking design), honestly always failing every
 * `OSDynLoad_Acquire` is exactly the path this real code was already
 * built to handle cleanly -- the same "honest negative answer, real
 * code already has a fallback" reasoning already used throughout this
 * shim (e.g. `UCReadSysConfig` reporting every key not-found).
 *
 * Real signatures and error codes confirmed against
 * `coreinit/dynload.h` -- `OSDynLoad_Error` is a real enum,
 * `OS_DYNLOAD_MODULE_NOT_FOUND = 0xFFFFFFFA` reused here for both a
 * failed Acquire and any FindExport against the (always-invalid)
 * resulting handle, since no more specific "invalid handle" code is
 * documented and this is the closest real, confirmed value.
 */

enum {
    BRAMBLE_OS_DYNLOAD_OK = 0,
    BRAMBLE_OS_DYNLOAD_MODULE_NOT_FOUND = (int32_t)0xFFFFFFFA,
};

static inline void ppc_import_coreinit_OSDynLoad_Acquire(PpcContext *ctx) {
    /* OSDynLoad_Error OSDynLoad_Acquire(char const *name, OSDynLoad_Module *outModule) */
    if (ctx->r[4] != 0) ppc_store_u32(ctx, ctx->r[4], 0); /* *outModule = NULL */
    ctx->r[3] = (uint32_t)BRAMBLE_OS_DYNLOAD_MODULE_NOT_FOUND;
}

static inline void ppc_import_coreinit_OSDynLoad_FindExport(PpcContext *ctx) {
    /* OSDynLoad_Error OSDynLoad_FindExport(OSDynLoad_Module module,
     *   OSDynLoad_ExportType exportType, char const *name, void **outAddr) */
    if (ctx->r[6] != 0) ppc_store_u32(ctx, ctx->r[6], 0); /* *outAddr = NULL */
    ctx->r[3] = (uint32_t)BRAMBLE_OS_DYNLOAD_MODULE_NOT_FOUND;
}

static inline void ppc_import_coreinit_OSDynLoad_Release(PpcContext *ctx) {
    /* OSDynLoad_Error OSDynLoad_Release(OSDynLoad_Module module) -- real
     * hardware releasing an already-invalid/never-acquired handle isn't a
     * game-visible error condition, so this always succeeds. */
    (void)ctx;
    ctx->r[3] = (uint32_t)BRAMBLE_OS_DYNLOAD_OK;
}

static inline void ppc_import_coreinit_OSDynLoad_SetAllocator(PpcContext *ctx) {
    /* OSDynLoad_Error OSDynLoad_SetAllocator(OSDynLoadAllocFn, OSDynLoadFreeFn) --
     * stores allocator callbacks for internal loader-heap use; safe
     * accept-and-discard no-op since nothing here ever dynamically loads
     * anything to need them for. */
    (void)ctx;
    ctx->r[3] = (uint32_t)BRAMBLE_OS_DYNLOAD_OK;
}

#endif /* BRAMBLE_CAFEOS_COREINIT_DYNLOAD_H */
