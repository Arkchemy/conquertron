#ifndef ARKCHEMY_CAFEOS_COREINIT_H
#define ARKCHEMY_CAFEOS_COREINIT_H

#include <stdlib.h>

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit, first slice.
 *
 * recomp declares (but does not define) one `void ppc_import_<library>_
 * <function>(PpcContext *ctx)` per real CafeOS/RPL function the recompiled
 * game actually calls (see docs/phase1d_import_surface.md for the full,
 * measured list -- 284 functions across 13 libraries). Generated code
 * follows the real PowerPC ABI: integer/pointer arguments arrive in
 * ctx->r[3], ctx->r[4], ... in order; this file is what actually
 * implements a first few of them.
 *
 * Scope for this first slice is deliberately narrow: only functions whose
 * real-hardware behavior is simple enough to implement with full
 * confidence from documented semantics, no guessing. Picked from
 * docs/phase1d_import_surface.md's "good early/incremental targets"
 * note -- small, self-contained coreinit primitives that don't require a
 * graphics/audio/input subsystem decision first.
 */

/* DCFlushRange(void *addr, u32 length) / DCFlushRangeNoSync(...): flushes
 * the data cache for a byte range, real Cafe OS hardware operations with
 * no return value. A genuine no-op here, not a shortcut: this runtime
 * executes recompiled game code as plain host/ARM64 native code, not real
 * PowerPC instructions on real PPC hardware -- there is no PPC data cache
 * to flush in the first place, the same reasoning already applied to
 * dcbst/isync/lwsync in ppc_runtime.h's own instruction-level handling. */
static inline void ppc_import_coreinit_DCFlushRange(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_DCFlushRangeNoSync(PpcContext *ctx) { (void)ctx; }

/* exit(int code) / _Exit(int code): real Cafe OS process termination,
 * same signature and semantics as the standard C functions of the same
 * name -- terminate the whole process with the given exit code. This is
 * a real, observable difference from every other shim function: control
 * never returns to the caller (or to ppc_dispatch/whatever recompiled
 * function invoked it), exactly like the real thing. */
static inline void ppc_import_coreinit_exit(PpcContext *ctx) { exit((int)ctx->r[3]); }
static inline void ppc_import_coreinit__Exit(PpcContext *ctx) { _Exit((int)ctx->r[3]); }

#endif /* ARKCHEMY_CAFEOS_COREINIT_H */
