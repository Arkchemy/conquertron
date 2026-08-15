#ifndef BRAMBLE_CAFEOS_NSYSHID_H
#define BRAMBLE_CAFEOS_NSYSHID_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- nsyshid (USB HID device access). This
 * is how real hardware detects the Skylanders Portal of Power (a USB
 * peripheral), among other HID devices -- genuinely important to real
 * gameplay here, unlike the network functions this session found are
 * simply unused by this game.
 *
 * HIDAddClient/HIDDelClient: safe, simple -- they only register/
 * unregister an attach-notification callback that (honestly) never
 * fires, since no device ever attaches -- no guest-callback-invocation
 * needed for these two.
 *
 * HIDRead/HIDSetIdle/HIDSetProtocol/HIDSetReport were previously
 * deferred for a real reason: they're *asynchronous*, real hardware
 * completing them via a callback invoked later, and this shim had no
 * way to call back *into* recompiled PPC code yet. `ppc_runtime.h` now
 * declares `ppc_dispatch` for exactly that (see its own comment; also
 * used by `cafeos_coreinit_fs.h`'s FS*Async functions).
 *
 * But reading Cemu's actual real HLE source for these functions
 * (src/Cafe/OS/libs/nsyshid/nsyshid.cpp) resolved the callback question
 * a simpler way: every one of these functions looks up the target
 * device by handle *before* deciding sync-vs-async at all, and returns
 * -1 immediately with **no callback invocation whatsoever** if no such
 * device exists (`GetDeviceByHandle(...) == nullptr`). Since this
 * runtime never has any real HID device attached at all (no backend, no
 * Portal of Power hardware, `HIDAddClient`'s attach callback above never
 * fires), every one of these calls always hits exactly that
 * device-not-found path on real hardware's own logic -- so the
 * ppc_dispatch machinery, while now available, isn't even needed here:
 * an immediate, honest `-1`, matching Cemu's own verified behavior
 * exactly rather than a guess, is correct.
 *
 * HIDDecodeError's real signature and behavior are also pulled directly
 * from Cemu's HLE (not in wut's public headers at all): `void
 * HIDDecodeError(uint32_t errorCode, uint32_t *ukn0, uint32_t *ukn1)` --
 * Cemu's own implementation is itself an honest `// todo` writing two
 * fixed placeholder values (`0x3FF`, `-0x7FFF`) and returning 0
 * regardless of the input error code; reproducing that verbatim matches
 * the best known real-world reference rather than guessing independently.
 */
static inline void ppc_import_nsyshid_HIDAddClient(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_nsyshid_HIDDelClient(PpcContext *ctx) { ctx->r[3] = 0; }

static inline void ppc_import_nsyshid_HIDRead(PpcContext *ctx) { ctx->r[3] = (uint32_t)-1; }
static inline void ppc_import_nsyshid_HIDSetIdle(PpcContext *ctx) { ctx->r[3] = (uint32_t)-1; }
static inline void ppc_import_nsyshid_HIDSetProtocol(PpcContext *ctx) { ctx->r[3] = (uint32_t)-1; }
static inline void ppc_import_nsyshid_HIDSetReport(PpcContext *ctx) { ctx->r[3] = (uint32_t)-1; }

static inline void ppc_import_nsyshid_HIDDecodeError(PpcContext *ctx) {
    if (ctx->r[4] != 0) ppc_store_u32(ctx, ctx->r[4], 0x3FFu);
    if (ctx->r[5] != 0) ppc_store_u32(ctx, ctx->r[5], (uint32_t)-0x7FFF);
    ctx->r[3] = 0;
}

#endif /* BRAMBLE_CAFEOS_NSYSHID_H */
