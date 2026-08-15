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
 * - OSGetConsoleType(void): real signature and real return value both
 *   confirmed directly against Cemu's actual HLE implementation
 *   (src/Cafe/OS/libs/coreinit/coreinit.cpp) -- `return 0x03000050;`,
 *   a fixed hardware-identifying constant, not a guess. Since real
 *   retail Wii U hardware always returns this same fixed value too
 *   (it's not runtime-detected there either), reproducing it verbatim
 *   is a real, correct implementation, not a stand-in.
 * - OSSendAppSwitchRequest(...): requests switching to another
 *   application (e.g. force-opening the HOME menu) -- lower confidence
 *   than everything else in this file: it's a real confirmed export
 *   (in wut's own `cafe/coreinit.def`), but no confirmed prototype
 *   exists in either wut's public headers or Cemu's HLE (which doesn't
 *   implement it at all). Since no HOME menu or other application
 *   exists in this runtime to switch to, always reporting failure
 *   (`BOOL` 0) is the correct real-world behavior regardless of the
 *   exact unconfirmed argument list -- this shim never reads any
 *   register beyond touching `r3`, so it's safe independent of how
 *   many real arguments the function actually takes.
 */
static inline void ppc_import_coreinit_OSEnableForegroundExit(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSEnableHomeButtonMenu(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_coreinit_OSIsDebuggerInitialized(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_coreinit_OSIsDebuggerPresent(PpcContext *ctx) { ctx->r[3] = 0; }

static inline void ppc_import_coreinit_OSSavesDone_ReadyToRelease(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_coreinit_OSYieldThread(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_coreinit_OSGetCoreId(PpcContext *ctx) { ctx->r[3] = 0; }

static inline void ppc_import_coreinit_OSGetConsoleType(PpcContext *ctx) { ctx->r[3] = 0x03000050u; }
static inline void ppc_import_coreinit_OSSendAppSwitchRequest(PpcContext *ctx) { ctx->r[3] = 0; /* BOOL FALSE -- no app to switch to */ }

/*
 * UCOpen/UCClose/UCReadSysConfig (Wii U system config, e.g. region and
 * parental-control settings): real signatures confirmed against
 * `coreinit/userconfig.h` -- `UCHandle UCOpen()`, `UCError
 * UCClose(UCHandle)`, `UCError UCReadSysConfig(UCHandle, uint32_t count,
 * UCSysConfig *settings)`. `UC_ERROR_OK` (0) is real success,
 * `UC_ERROR_KEY_NOT_FOUND` (-0x200009) is real "no such config key."
 *
 * No real Wii U system config store exists here. `UCOpen` hands back a
 * fixed valid-looking handle (1) since there's nothing that needs real
 * per-open state; `UCClose` always succeeds. `UCReadSysConfig`
 * deliberately reports every requested key as not-found rather than
 * guessing at `UCSysConfig`'s real per-entry layout (name/dataType/data
 * pointer) to fabricate a plausible-looking value -- real code asking
 * about config it can't get is expected to have a sane default/fallback
 * for exactly this "not found" case already, so this is an honest
 * negative answer, not a guess.
 */
static inline void ppc_import_coreinit_UCOpen(PpcContext *ctx) { ctx->r[3] = 1; }
static inline void ppc_import_coreinit_UCClose(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_coreinit_UCReadSysConfig(PpcContext *ctx) { ctx->r[3] = (uint32_t)-0x200009; }

#endif /* BRAMBLE_CAFEOS_COREINIT_MISC_H */
