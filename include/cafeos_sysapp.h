#ifndef ARKCHEMY_CAFEOS_SYSAPP_H
#define ARKCHEMY_CAFEOS_SYSAPP_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- sysapp (system application launching).
 *
 * SYSLaunchSettings(SysAppSettingsArgs *args): real signature confirmed
 * against devkitPro/wut's sysapp/launch.h (there named `_SYSLaunchSettings`
 * -- the leading-underscore/no-underscore naming split between a wrapped
 * public API and its real RPL export is common across CafeOS libraries;
 * the real binary's own import table names this one without the
 * underscore). Real behavior: launches the Wii U system Settings app once
 * the current application exits. Genuine no-op here -- there is no HOME
 * menu or system Settings app in this runtime to launch into, the same
 * reasoning as OSEnableHomeButtonMenu in cafeos_coreinit_misc.h. `args`
 * is caller-allocated and never read here, so there's no guest-memory
 * concern the way there is for __gh_errno_ptr.
 */
static inline void ppc_import_sysapp_SYSLaunchSettings(PpcContext *ctx) { (void)ctx; }

#endif /* ARKCHEMY_CAFEOS_SYSAPP_H */
