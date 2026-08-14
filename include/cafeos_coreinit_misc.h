#ifndef BRAMBLE_CAFEOS_COREINIT_MISC_H
#define BRAMBLE_CAFEOS_COREINIT_MISC_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- small coreinit system-state queries and
 * flags that don't fit any of the other cafeos_*.h files, each simple
 * enough to be high-confidence on its own:
 *
 * - OSEnableForegroundExit(void) / OSEnableHomeButtonMenu(BOOL): real
 *   hardware features (hold-HOME-to-exit, HOME menu availability) that
 *   don't exist in this runtime at all yet (no HOME button, no display) --
 *   genuine no-ops, not shortcuts.
 * - OSIsDebuggerInitialized(void) / OSIsDebuggerPresent(void): both
 *   real BOOL-returning queries; always FALSE is correct here (there is
 *   no real Cafe OS debugger attached to a recompiled process).
 * - OSSavesDone_ReadyToRelease(void): signals the save-data subsystem
 *   it's safe to release exclusive access -- no-op (no real save-data
 *   locking exists here to release).
 * - OSYieldThread(void): yields the CPU to another ready thread -- a
 *   genuine no-op in this runtime for the same reason the OSMutex/
 *   OSEvent primitives are (see cafeos_coreinit_sync.h): exactly one
 *   PpcContext executes sequentially, there is no other thread to yield
 *   to.
 * - OSGetCoreId(void): returns which of the real Wii U's 3 PPC cores is
 *   running (0, 1, or 2). Always reporting core 0 is a reasonable,
 *   consistent choice -- this runtime never models more than one
 *   execution context, so "which core" has no real meaning here beyond
 *   being a stable value real code can compare against.
 */
static inline void ppc_import_coreinit_OSEnableForegroundExit(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSEnableHomeButtonMenu(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_coreinit_OSIsDebuggerInitialized(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_coreinit_OSIsDebuggerPresent(PpcContext *ctx) { ctx->r[3] = 0; }

static inline void ppc_import_coreinit_OSSavesDone_ReadyToRelease(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSYieldThread(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_coreinit_OSGetCoreId(PpcContext *ctx) { ctx->r[3] = 0; }

#endif /* BRAMBLE_CAFEOS_COREINIT_MISC_H */
