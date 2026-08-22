#ifndef ARKCHEMY_CAFEOS_NSYSNET_H
#define ARKCHEMY_CAFEOS_NSYSNET_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- nsysnet (BSD-style socket library
 * lifecycle). Confirmed (see the "Future titles & Pretendo" project
 * note) that these are the *only* three nsysnet functions this game's
 * real binary ever calls -- no `connect`/`send`/`recv`/etc. anywhere in
 * reachable code, meaning no real socket ever actually gets used even
 * though the library gets initialized/torn down around that dead code
 * path. `socketclose` is real (confirmed against `nsysnet/socket.h`,
 * `int socketclose(int sockfd)`); `socket_lib_init`/`socket_lib_finish`
 * aren't in that (deprecated) header, but are real, well-known
 * lifecycle functions with no meaningful parameters -- both implemented
 * as honest no-op successes, matching this shim never creating a real
 * socket for `socketclose` to ever legitimately need to act on.
 */
static inline void ppc_import_nsysnet_socket_lib_init(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_nsysnet_socket_lib_finish(PpcContext *ctx) { ctx->r[3] = 0; }
static inline void ppc_import_nsysnet_socketclose(PpcContext *ctx) { ctx->r[3] = 0; }

#endif /* ARKCHEMY_CAFEOS_NSYSNET_H */
