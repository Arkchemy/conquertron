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
 * Only the client registration functions are implemented. HIDRead/
 * HIDSetIdle/HIDSetProtocol/HIDSetReport are real, confirmed-signature
 * functions (int32_t HIDRead(uint32_t handle, uint8_t *buffer, uint32_t
 * bufferLength, HIDCallback callback, void *userContext); -- verified
 * against devkitPro/wut's nsyshid/hid.h) but deliberately NOT attempted
 * yet, for a reason distinct from every other deferred function in this
 * shim so far: they're *asynchronous*, completing via a callback the
 * real HID stack invokes later. Correctly honoring that means the shim
 * itself calling back *into* recompiled PPC code (via the same
 * ppc_dispatch mechanism bctrl/indirect calls already use) -- every
 * other shim function so far only goes one direction (recompiled code
 * calls the shim). That's a new, higher-risk integration pattern this
 * session doesn't have enough confidence in HIDCallback's exact
 * signature to attempt safely, on top of there being no real Portal of
 * Power hardware to ever actually attach in the first place. A real
 * fix needs both a real USB/HID backend and that callback-invocation
 * mechanism -- tracked here as its own problem, not folded into "just
 * another shim function."
 *
 * HIDAddClient/HIDDelClient themselves are safe: they only register/
 * unregister an attach-notification callback that (honestly) never
 * fires, since no device ever attaches -- no guest-callback-invocation
 * needed for these two specifically.
 */
static inline void ppc_import_nsyshid_HIDAddClient(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_nsyshid_HIDDelClient(PpcContext *ctx) { ctx->r[3] = 0; }

#endif /* BRAMBLE_CAFEOS_NSYSHID_H */
