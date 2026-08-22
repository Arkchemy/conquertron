#ifndef ARKCHEMY_CAFEOS_NN_SAVE_H
#define ARKCHEMY_CAFEOS_NN_SAVE_H

#include <sys/statvfs.h>

#include "cafeos_coreinit_fs.h"
#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- nn::save (Wii U save data API). Real
 * signatures quoted verbatim from devkitPro/wut's nn/save/save.h, one
 * call at a time to stay within fetch-tool quoting limits, then
 * cross-checked for consistency:
 *
 *   SAVEStatus SAVEInit();
 *   void SAVEShutdown();
 *   SAVEStatus SAVEInitSaveDir(uint8_t slotNo);
 *   FSStatus SAVEOpenFile(FSClient*, FSCmdBlock*, uint8_t slotNo,
 *                          const char *path, const char *mode,
 *                          FSFileHandle*, FSErrorFlag);
 *   FSStatus SAVERemove(FSClient*, FSCmdBlock*, uint8_t slotNo,
 *                        const char *path, FSErrorFlag);
 *   FSStatus SAVEGetStat(FSClient*, FSCmdBlock*, uint8_t slotNo,
 *                         FSStat*, FSErrorFlag);
 *   FSStatus SAVEGetFreeSpaceSize(FSClient*, FSCmdBlock*, uint8_t slotNo,
 *                                  uint64_t *outSize, FSErrorFlag);
 *   FSStatus SAVEFlushQuota(FSClient*, FSCmdBlock*, uint8_t slotNo,
 *                            FSErrorFlag);
 *
 * The SAVE API is a thin wrapper over the same FS* layer
 * cafeos_coreinit_fs.h already implements (same FSClient/FSCmdBlock/
 * FSStatus/FSFileHandle types, just with an extra leading `slotNo`
 * argument shifting every later parameter one register right) -- so
 * SAVEOpenFile/SAVERemove reuse that file's real fopen()/remove()-backed
 * logic and its shared file-handle table directly, rather than
 * duplicating it. `slotNo` itself is read but not acted on: this shim
 * doesn't do the "content:/"/"save:/" volume path translation FS* also
 * hasn't done yet (see cafeos_coreinit_fs.h's own documented gap), so
 * there's no real per-slot directory to route into regardless.
 *
 * SAVEGetStat takes no path -- it reports on the save directory as a
 * whole, not a specific file. Matching FSGetStatFile's own "only fields
 * real code overwhelmingly actually reads are filled in, the rest
 * zeroed rather than guessed" approach, every field here is zeroed
 * (there's no real per-slot directory to report a genuine size for).
 *
 * SAVEGetFreeSpaceSize reports the *real* free space of the host
 * filesystem the current working directory lives on (via `statvfs`) --
 * a real, verifiable number, not a fabricated Wii U-scale constant. It
 * won't match what a real console would report, but "is there enough
 * space" checks against a real (if differently-scaled) number are more
 * honest than either a guessed constant or a hardcoded "always plenty."
 */
static inline void ppc_import_nn_save_SAVEInit(PpcContext *ctx) { ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK; }
static inline void ppc_import_nn_save_SAVEShutdown(PpcContext *ctx) { (void)ctx; }
static inline void ppc_import_nn_save_SAVEInitSaveDir(PpcContext *ctx) { ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK; }

static inline void ppc_import_nn_save_SAVEOpenFile(PpcContext *ctx) {
    char path[512], mode[8];
    ppc_fs_read_cstr(ctx, ctx->r[6], path, sizeof(path));
    ppc_fs_read_cstr(ctx, ctx->r[7], mode, sizeof(mode));

    FILE *f = fopen(path, mode);
    if (!f) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t handle = ppc_fs_alloc_handle(f);
    if (handle == 0) {
        fclose(f);
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_ACCESS_ERROR;
        return;
    }
    ppc_store_u32(ctx, ctx->r[8], handle);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_nn_save_SAVERemove(PpcContext *ctx) {
    char path[512];
    ppc_fs_read_cstr(ctx, ctx->r[6], path, sizeof(path));
    if (remove(path) != 0) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_nn_save_SAVEGetStat(PpcContext *ctx) {
    uint32_t stat_addr = ctx->r[6];
    for (uint32_t i = 0; i < 0x64; i += 4) ppc_store_u32(ctx, stat_addr + i, 0);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_nn_save_SAVEGetFreeSpaceSize(PpcContext *ctx) {
    struct statvfs vfs;
    uint64_t free_bytes = 0;
    if (statvfs(".", &vfs) == 0) {
        free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    }
    ppc_store_u64(ctx, ctx->r[6], free_bytes);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_nn_save_SAVEFlushQuota(PpcContext *ctx) { ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK; }

#endif /* ARKCHEMY_CAFEOS_NN_SAVE_H */
