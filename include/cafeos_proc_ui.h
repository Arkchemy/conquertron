#ifndef BRAMBLE_CAFEOS_PROC_UI_H
#define BRAMBLE_CAFEOS_PROC_UI_H

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- proc_ui (process/foreground-state
 * management -- HOME menu overlay, background/foreground transitions).
 *
 * Real signatures verified against the actual devkitPro/wut
 * include/proc_ui/procui.h:
 *   void ProcUIInit(ProcUISaveCallback saveCallback);
 *   void ProcUIShutdown(void);
 *   ProcUIStatus ProcUIProcessMessages(BOOL block);
 *   void ProcUIRegisterCallback(ProcUICallbackType, ProcUICallback, void*, uint32_t);
 *   void ProcUIRegisterBackgroundCallback(ProcUICallback, void*, OSTime);
 *   void ProcUIDrawDoneRelease(void);
 *
 * ProcUIStatus's four values (PROCUI_STATUS_IN_FOREGROUND,
 * _IN_BACKGROUND, _RELEASE_FOREGROUND, _EXITING) are a plain sequential
 * enum starting at 0 -- IN_FOREGROUND = 0 is the safe, standard C
 * assumption for an unqualified enum's first member, not an independent
 * guess.
 *
 * Real code's main loop typically calls ProcUIProcessMessages every
 * frame and exits when it returns PROCUI_STATUS_EXITING (triggered by a
 * real HOME-menu "close app" request) -- since none of that exists here
 * (no HOME button, no OS-level backgrounding), always reporting
 * IN_FOREGROUND is the correct behavior for keeping the game's own main
 * loop running rather than spuriously telling it to quit. The
 * callback-registration functions accept and discard their callback
 * pointers -- nothing in this shim ever triggers a background/exit
 * event to invoke them from, so there's nothing to call them with yet.
 */
enum {
    BRAMBLE_PROCUI_STATUS_IN_FOREGROUND = 0,
};

static inline void ppc_import_proc_ui_ProcUIInit(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_proc_ui_ProcUIShutdown(PpcContext *ctx) { (void)ctx; }

static inline void ppc_import_proc_ui_ProcUIProcessMessages(PpcContext *ctx) {
    ctx->r[3] = (uint32_t)BRAMBLE_PROCUI_STATUS_IN_FOREGROUND;
}

static inline void ppc_import_proc_ui_ProcUIRegisterCallback(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_proc_ui_ProcUIRegisterBackgroundCallback(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_proc_ui_ProcUIDrawDoneRelease(PpcContext *ctx) { (void)ctx; }

#endif /* BRAMBLE_CAFEOS_PROC_UI_H */
