#ifndef BRAMBLE_CAFEOS_COREINIT_LOG_H
#define BRAMBLE_CAFEOS_COREINIT_LOG_H

#include <stdio.h>
#include <stdlib.h>

#include "ppc_runtime.h"
#include "cafeos_coreinit_fs.h" /* ppc_fs_read_cstr */

/*
 * Phase 1d CafeOS runtime shim -- coreinit logging (OSReport, OSPanic,
 * OSConsoleWrite).
 *
 * OSReport(const char *fmt, ...) and OSPanic(const char *file, int32_t
 * line, const char *fmt, ...) are real, variadic printf-style CafeOS
 * functions. Deliberately NOT attempting real varargs substitution here:
 * correctly reading an arbitrary mix of int/pointer args (r4..r10) and
 * float/double args (f1..f8) in the right order for arbitrary format
 * strings depends on GHS's exact variadic-argument ABI, which isn't
 * independently confirmed here the way the FS API's signatures are --
 * getting it wrong would silently print plausible-looking *garbage*
 * (misread registers formatted as if they were the right ones), which is
 * a worse outcome than the honest gap of just printing the format string
 * un-substituted. Every %-format real code embeds is still visible in
 * the output, just not filled in.
 *
 * OSConsoleWrite(const char *str, uint32_t len) is NOT variadic --
 * printed in full, correctly, no gap.
 */
static inline void ppc_import_coreinit_OSReport(PpcContext *ctx) {
    char fmt[1024];
    ppc_fs_read_cstr(ctx, ctx->r[3], fmt, sizeof(fmt));
    fprintf(stderr, "[OSReport] %s", fmt);
}

/* OSPanic(const char *file, int32_t line, const char *fmt, ...); r3=file
 * r4=line r5=fmt -- argument order (file, line, message) matches the
 * common assert()-macro convention but isn't independently confirmed
 * against a real header the way FSOpenFile's signature is. Low stakes
 * either way: getting this order wrong would only mislabel the
 * diagnostic printed just before aborting, not change program
 * behavior (it aborts regardless). */
static inline void ppc_import_coreinit_OSPanic(PpcContext *ctx) {
    char file[256], fmt[1024];
    ppc_fs_read_cstr(ctx, ctx->r[3], file, sizeof(file));
    ppc_fs_read_cstr(ctx, ctx->r[5], fmt, sizeof(fmt));
    fprintf(stderr, "[OSPanic] %s:%d: %s\n", file, (int32_t)ctx->r[4], fmt);
    /* Real hardware halts the whole system on a panic -- an
     * unrecoverable OS-level error, not something real code expects to
     * return from. abort() matches this runtime's existing ppc_trap()
     * precedent (see ppc_runtime.h): fail loud immediately rather than
     * silently continue in whatever corrupt state triggered the panic. */
    abort();
}

/* OSConsoleWrite(const char *str, uint32_t len); r3=str r4=len -- not
 * NUL-terminated in general, so this can't reuse ppc_fs_read_cstr. */
static inline void ppc_import_coreinit_OSConsoleWrite(PpcContext *ctx) {
    uint32_t addr = ctx->r[3];
    uint32_t len = ctx->r[4];
    for (uint32_t i = 0; i < len; i++) fputc((int)ppc_load_u8(ctx, addr + i), stderr);
}

#endif /* BRAMBLE_CAFEOS_COREINIT_LOG_H */
