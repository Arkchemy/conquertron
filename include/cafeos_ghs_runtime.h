#ifndef BRAMBLE_CAFEOS_GHS_RUNTIME_H
#define BRAMBLE_CAFEOS_GHS_RUNTIME_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- GHS (Green Hills Software) C runtime
 * internals. Unlike the FS functions, these aren't in the public Cafe OS
 * SDK; they're the compiler's own runtime support library, statically
 * linked into every GHS-built binary (confirmed real, not hypothetical:
 * this project's own README documents the real Spyro's Adventure binary
 * being GHS-compiled, and these exact names appear in its real import
 * list -- see docs/phase1d_import_surface.md).
 *
 * __ghsLock()/__ghsUnlock(): real GHS runtime source (sbrk()'s heap
 * allocator) calls these with no arguments to guard the C runtime's own
 * internal heap state during allocation -- confirmed via a real GHS
 * ind_heap.c excerpt, not guessed. This runtime has no concurrent
 * execution model at all (one PpcContext runs sequentially, nothing else
 * can interleave), so a real mutual-exclusion primitive would already be
 * a no-op here even if a second execution context existed -- correct
 * regardless of exactly how many arguments the real function takes,
 * since a no-op body doesn't need to inspect any of them.
 */
static inline void ppc_import_coreinit___ghsLock(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit___ghsUnlock(PpcContext *ctx) { (void)ctx; }

/*
 * __ghs_flock_file/__ghs_funlock_file/__ghs_flock_ptr/__ghs_flock_destroy:
 * the per-FILE-pointer/per-object lock family for thread-safe stdio, same
 * single-threaded reasoning as above applies to *locking*. Less certain
 * about the exact real signatures than __ghsLock/__ghsUnlock (no
 * confirmed primary source for these specific four, unlike the sbrk()
 * one above) -- so these are implemented defensively: pass r3 straight
 * through as the return value wherever one might be expected (safe
 * whether the real function returns a fresh handle or the same one it
 * was given, since nothing in this runtime ever dereferences it) rather
 * than fabricating a specific handle scheme that might not match.
 */
static inline void ppc_import_coreinit___ghs_flock_file(PpcContext *ctx) {
    /* takes a FILE*-like handle in r3; real semantics likely return a lock
     * handle -- leaving r3 unmodified passes the input straight through
     * as the return value, a safe default either way since nothing here
     * ever dereferences it. */
    (void)ctx;
}
static inline void ppc_import_coreinit___ghs_funlock_file(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit___ghs_flock_ptr(PpcContext *ctx) {
    /* same passthrough reasoning as __ghs_flock_file. */
    (void)ctx;
}
static inline void ppc_import_coreinit___ghs_flock_destroy(PpcContext *ctx) { (void)ctx; }

#endif /* BRAMBLE_CAFEOS_GHS_RUNTIME_H */
