#ifndef ARKCHEMY_CAFEOS_SND_USER_H
#define ARKCHEMY_CAFEOS_SND_USER_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- snd_user (AXFX audio-effects layer,
 * layered on top of snd_core). Real signatures confirmed against Cemu's
 * real HLE source (src/Cafe/OS/libs/snd_user/snd_user.cpp) -- not in
 * devkitPro/wut, which doesn't cover this library at all.
 *
 * void AXFXSetHooks(void *allocFunc, void *freeFunc): real behavior
 * stores two guest function pointers AXFX uses for its own internal
 * heap allocation. Genuine no-op here -- nothing in this shim ever
 * performs the dynamic FX allocation that would need to call them, the
 * same reasoning already used for proc_ui's callback-registration
 * functions.
 *
 * void AXFXMultiChReverbShutdown(AXFXMultiChReverbData *param): notably,
 * Cemu's own real HLE implementation of this is itself an empty
 * "// todo" stub with no actual teardown logic -- so a genuine no-op
 * here isn't a shortcut relative to real prior art, it's matching the
 * best known real-world reference implementation.
 */
static inline void ppc_import_snd_user_AXFXSetHooks(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_snd_user_AXFXMultiChReverbShutdown(PpcContext *ctx) { (void)ctx; }

#endif /* ARKCHEMY_CAFEOS_SND_USER_H */
