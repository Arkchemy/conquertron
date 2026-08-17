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

/*
 * __dotsyscall: real GHS runtime primitive, called by __ghs_syscall (the
 * C library's own low-level raw-syscall trampoline, used by things like
 * sbrk/read/write when no higher-level real CafeOS call covers them
 * directly). Unlike every other function in this file, this one isn't
 * an RPL import at all -- it's a real, bare `bl` to a local/external
 * symbol (recomp's own generated output marks it "external -- defined
 * in another translation unit", the same category as `ppc_dispatch`),
 * and on real hardware it's genuinely a tiny assembly stub that issues
 * a real `sc` (supervisor call) instruction -- already a documented,
 * deliberately out-of-scope real instruction elsewhere in this project
 * (privileged, no real equivalent this runtime could issue). A real,
 * honest stub here, using the same `ppc_unhandled_stub` fallback this
 * project's first full-game build already established for the small
 * number of genuinely-unhandled real cases, rather than a silent,
 * fabricated return value for a real raw syscall this runtime has no
 * way to actually service.
 */
static inline void ppc___dotsyscall(PpcContext *ctx) {
    ppc_unhandled_stub(ctx, "ppc___dotsyscall (real GHS raw syscall trampoline -- no real equivalent this runtime can issue)");
}

/*
 * __cpp_exception_init: real GHS C++ runtime primitive, called once
 * during real program startup (confirmed: this project's own real
 * `ppc_bramble_game_entry` -- the actual game's real entry point --
 * calls it directly, early, during its own startup sequence) to set up
 * the compiler's internal C++ exception-handling tables. This
 * project's own recompiled code has no real C++ exception model at
 * all (matching devkitA64's own real `-fno-exceptions` this project
 * already builds with -- see e.g. switch/gx2_test/Makefile's own
 * CXXFLAGS), so there's nothing real for this to actually initialize
 * here. Real, honest no-op: unlike `ppc___dotsyscall` above (a real,
 * potentially-reachable-at-any-time raw syscall path), this is purely
 * one-time startup bookkeeping for a feature this runtime doesn't
 * model, so a silent no-op (not even logged) is the honest behavior --
 * there's no real "did this matter" question to flag for whoever reads
 * the log later, unlike a real, arbitrary unhandled instruction might
 * raise.
 */
static inline void ppc___cpp_exception_init(PpcContext *ctx) { (void)ctx; }

#endif /* BRAMBLE_CAFEOS_GHS_RUNTIME_H */
