#ifndef BRAMBLE_CAFEOS_COREINIT_FS_H
#define BRAMBLE_CAFEOS_COREINIT_FS_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit's high-level filesystem API
 * (FS*, not the lower-level FSA* one). Real signatures and FSStatus/FSMode/
 * FSDirectoryEntry values verified against the actual devkitPro/wut
 * headers (include/coreinit/filesystem.h), not guessed:
 *
 *   FSStatus FSAddClient(FSClient*, FSErrorFlag);
 *   FSStatus FSOpenFile(FSClient*, FSCmdBlock*, const char *path,
 *                        const char *mode, FSFileHandle*, FSErrorFlag);
 *   FSStatus FSReadFile(FSClient*, FSCmdBlock*, uint8_t *buffer,
 *                        uint32_t size, uint32_t count, FSFileHandle,
 *                        uint32_t unk1, FSErrorFlag);
 *   ... etc -- see individual functions below for the rest.
 *
 * FSClient/FSCmdBlock are opaque structs in the real API (real game code
 * never inspects their contents, only passes the pointer back into other
 * FS* calls) -- so this shim never reads/writes through them at all. All
 * real state (open file handles) lives in host-side globals here, keyed
 * by a small integer handle this shim hands back, exactly like the real
 * FSFileHandle/FSDirectoryHandle types (both plain uint32_t already).
 *
 * Known, deliberate gaps, not yet resolved:
 *  - FSErrorFlag (the "errorMask" argument on every call) controls
 *    whether the real FS layer can silently return a status code for a
 *    given error class or must abort/report it some other way. Ignored
 *    here -- every call always just returns its FSStatus in r3.
 *  - Path translation: real Wii U FS paths are relative to a mounted
 *    volume (game content, save data, ...), not a host filesystem path.
 *    This shim passes the path straight through to fopen()/etc as-is, no
 *    "content:/" or similar prefix handling -- correct behavior once
 *    there's a real decision about where extracted game content lives on
 *    a Switch build, not before.
 *  - Not yet safe to include from more than one compiled .c file in the
 *    same program: the handle table and last-error state below are
 *    file-scope `static`, so each translation unit that includes this
 *    header gets its own disconnected copy rather than sharing real
 *    state -- fine for a single-file build/test (today's usage), wrong
 *    the moment a multi-object build (see recomp's --extern-globals)
 *    includes this header more than once. Fixing that means splitting
 *    this into an `extern`-declaring header plus one real .c definition
 *    file compiled exactly once, matching how a normal C library would
 *    do it -- not done yet since nothing exercises multi-file FS calls
 *    to catch it being wrong.
 *  - Only ever reports the most recent error (a single global, not
 *    tracked per-FSClient the way real hardware does).
 */

enum {
    BRAMBLE_FS_STATUS_OK = 0,
    BRAMBLE_FS_STATUS_CANCELLED = -1,
    BRAMBLE_FS_STATUS_END = -2,
    BRAMBLE_FS_STATUS_ALREADY_OPEN = -4,
    BRAMBLE_FS_STATUS_EXISTS = -5,
    BRAMBLE_FS_STATUS_NOT_FOUND = -6,
    BRAMBLE_FS_STATUS_NOT_FILE = -7,
    BRAMBLE_FS_STATUS_NOT_DIR = -8,
    BRAMBLE_FS_STATUS_ACCESS_ERROR = -9,
};

#define BRAMBLE_FS_MAX_HANDLES 64

static FILE *g_ppc_fs_files[BRAMBLE_FS_MAX_HANDLES];
static int32_t g_ppc_fs_last_error = BRAMBLE_FS_STATUS_OK;

/* Reads a NUL-terminated string out of the guest's synthetic memory --
 * every FS* path/mode argument is a pointer into it, never a host
 * pointer. */
static inline void ppc_fs_read_cstr(const PpcContext *ctx, uint32_t addr, char *out, size_t max) {
    size_t i = 0;
    for (; i + 1 < max; i++) {
        uint8_t b = ppc_load_u8(ctx, addr + (uint32_t)i);
        if (b == 0) break;
        out[i] = (char)b;
    }
    out[i] = '\0';
}

/* 1-based handles (0 reserved so a zeroed-out FSFileHandle reads as
 * invalid, matching the convention real code's error paths rely on). */
static inline uint32_t ppc_fs_alloc_handle(FILE *f) {
    for (uint32_t i = 0; i < BRAMBLE_FS_MAX_HANDLES; i++) {
        if (!g_ppc_fs_files[i]) {
            g_ppc_fs_files[i] = f;
            return i + 1;
        }
    }
    return 0;
}

static inline FILE *ppc_fs_get_handle(uint32_t handle) {
    if (handle == 0 || handle > BRAMBLE_FS_MAX_HANDLES) return NULL;
    return g_ppc_fs_files[handle - 1];
}

static inline void ppc_fs_free_handle(uint32_t handle) {
    if (handle == 0 || handle > BRAMBLE_FS_MAX_HANDLES) return;
    g_ppc_fs_files[handle - 1] = NULL;
}

/* void FSInit(void); / void FSShutdown(void); -- real signatures take no
 * arguments and return nothing. Resetting the handle table on init is a
 * deliberate safety net (real code is expected to close everything
 * before FSShutdown, but a stale handle surviving a re-init would
 * otherwise silently alias a totally different later open()). */
static inline void ppc_import_coreinit_FSInit(PpcContext *ctx) {
    (void)ctx;
    memset(g_ppc_fs_files, 0, sizeof(g_ppc_fs_files));
    g_ppc_fs_last_error = BRAMBLE_FS_STATUS_OK;
}

static inline void ppc_import_coreinit_FSShutdown(PpcContext *ctx) {
    (void)ctx;
    for (uint32_t i = 0; i < BRAMBLE_FS_MAX_HANDLES; i++) {
        if (g_ppc_fs_files[i]) {
            fclose(g_ppc_fs_files[i]);
            g_ppc_fs_files[i] = NULL;
        }
    }
}

/* FSStatus FSAddClient(FSClient *client, FSErrorFlag errorMask);
 * FSStatus FSDelClient(FSClient *client, FSErrorFlag errorMask);
 * FSClient is opaque -- this shim never touches its contents, so both are
 * unconditional successes (real hardware can fail these on resource
 * exhaustion; not modeled, no known real code path here that hits it). */
static inline void ppc_import_coreinit_FSAddClient(PpcContext *ctx) { ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK; }
static inline void ppc_import_coreinit_FSDelClient(PpcContext *ctx) { ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK; }

/* void FSInitCmdBlock(FSCmdBlock *block); -- FSCmdBlock is opaque too, and
 * this shim runs every FS call synchronously (no real async command-queue
 * model), so there's no per-block state to initialize. */
static inline void ppc_import_coreinit_FSInitCmdBlock(PpcContext *ctx) { (void)ctx; }

/* FSStatus FSGetLastError(FSClient *client); */
static inline void ppc_import_coreinit_FSGetLastError(PpcContext *ctx) { ctx->r[3] = (uint32_t)g_ppc_fs_last_error; }

/* FSStatus FSOpenFile(FSClient *client, FSCmdBlock *block, const char *path,
 *                      const char *mode, FSFileHandle *handle, FSErrorFlag errorMask);
 * r3=client r4=block r5=path r6=mode r7=out_handle r8=errorMask */
static inline void ppc_import_coreinit_FSOpenFile(PpcContext *ctx) {
    char path[512], mode[8];
    ppc_fs_read_cstr(ctx, ctx->r[5], path, sizeof(path));
    ppc_fs_read_cstr(ctx, ctx->r[6], mode, sizeof(mode));

    FILE *f = fopen(path, mode);
    if (!f) {
        g_ppc_fs_last_error = BRAMBLE_FS_STATUS_NOT_FOUND;
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t handle = ppc_fs_alloc_handle(f);
    if (handle == 0) {
        fclose(f);
        g_ppc_fs_last_error = BRAMBLE_FS_STATUS_ACCESS_ERROR;
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_ACCESS_ERROR;
        return;
    }
    ppc_store_u32(ctx, ctx->r[7], handle);
    g_ppc_fs_last_error = BRAMBLE_FS_STATUS_OK;
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSCloseFile(FSClient*, FSCmdBlock*, FSFileHandle handle, FSErrorFlag);
 * r3=client r4=block r5=handle r6=errorMask */
static inline void ppc_import_coreinit_FSCloseFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    fclose(f);
    ppc_fs_free_handle(ctx->r[5]);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSReadFile(FSClient*, FSCmdBlock*, uint8_t *buffer, uint32_t size,
 *                      uint32_t count, FSFileHandle handle, uint32_t unk1, FSErrorFlag);
 * r3=client r4=block r5=buffer r6=size r7=count r8=handle r9=unk1 r10=errorMask
 * Real return value is the number of *elements* (not bytes) read on
 * success, matching fread()'s own return convention -- confirmed against
 * real FS API documentation describing FSReadFile as fread()-shaped. */
static inline void ppc_import_coreinit_FSReadFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[8]);
    if (!f) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t buffer_addr = ctx->r[5];
    uint32_t size = ctx->r[6];
    uint32_t count = ctx->r[7];
    if (size == 0 || count == 0) {
        ctx->r[3] = 0;
        return;
    }
    /* Read into a host-side staging buffer, then copy into the guest's
     * synthetic memory one byte at a time via ppc_store_u8 -- there's no
     * way to get a real host pointer directly into ctx->mem's synthetic
     * address space from outside ppc_runtime.h's own helpers, and this
     * keeps big reads from overrunning ctx->mem's fixed (and currently
     * quite small -- see ppc_runtime.h's own PpcContext::mem comment)
     * size silently. */
    uint64_t total = (uint64_t)size * (uint64_t)count;
    uint32_t elements_read = 0;
    for (uint32_t e = 0; e < count; e++) {
        uint32_t got_this_element = 0;
        for (uint32_t b = 0; b < size; b++) {
            int c = fgetc(f);
            if (c == EOF) break;
            ppc_store_u8(ctx, buffer_addr + e * size + b, (uint8_t)c);
            got_this_element++;
        }
        if (got_this_element < size) break;
        elements_read++;
    }
    (void)total;
    ctx->r[3] = elements_read;
}

/* FSStatus FSGetPosFile(FSClient*, FSCmdBlock*, FSFileHandle, uint32_t *pos, FSErrorFlag);
 * r3=client r4=block r5=handle r6=out_pos r7=errorMask */
static inline void ppc_import_coreinit_FSGetPosFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    long pos = ftell(f);
    ppc_store_u32(ctx, ctx->r[6], (uint32_t)pos);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSSetPosFile(FSClient*, FSCmdBlock*, FSFileHandle, uint32_t pos, FSErrorFlag);
 * r3=client r4=block r5=handle r6=pos r7=errorMask */
static inline void ppc_import_coreinit_FSSetPosFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    fseek(f, (long)ctx->r[6], SEEK_SET);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSGetStatFile(FSClient*, FSCmdBlock*, FSFileHandle, FSStat*, FSErrorFlag);
 * r3=client r4=block r5=handle r6=out_stat r7=errorMask
 *
 * FSStat's real byte layout (from wut's coreinit/filesystem.h, packed):
 *   0x00 flags, 0x04 mode, 0x08 owner, 0x0C group, 0x10 size,
 *   0x14 allocSize, 0x18 quotaSize (8 bytes), 0x20 entryId,
 *   0x24 created (FSTime, 8 bytes), 0x2C modified (FSTime, 8 bytes),
 *   padded to 0x64 bytes total.
 * Only `size` (the one field real code overwhelmingly actually reads) is
 * filled in with a real value; everything else is zeroed rather than
 * guessed. */
static inline void ppc_import_coreinit_FSGetStatFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, cur, SEEK_SET);

    uint32_t stat_addr = ctx->r[6];
    for (uint32_t i = 0; i < 0x64; i += 4) ppc_store_u32(ctx, stat_addr + i, 0);
    ppc_store_u32(ctx, stat_addr + 0x10, (uint32_t)size);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* Directory operations -- same handle-table pattern as the file ones
 * above, just a second table since FSFileHandle and FSDirectoryHandle
 * are numbered independently on real hardware. */
#define BRAMBLE_FS_MAX_DIR_HANDLES 32
static DIR *g_ppc_fs_dirs[BRAMBLE_FS_MAX_DIR_HANDLES];

static inline uint32_t ppc_fs_alloc_dir_handle(DIR *d) {
    for (uint32_t i = 0; i < BRAMBLE_FS_MAX_DIR_HANDLES; i++) {
        if (!g_ppc_fs_dirs[i]) {
            g_ppc_fs_dirs[i] = d;
            return i + 1;
        }
    }
    return 0;
}

static inline DIR *ppc_fs_get_dir_handle(uint32_t handle) {
    if (handle == 0 || handle > BRAMBLE_FS_MAX_DIR_HANDLES) return NULL;
    return g_ppc_fs_dirs[handle - 1];
}

static inline void ppc_fs_free_dir_handle(uint32_t handle) {
    if (handle == 0 || handle > BRAMBLE_FS_MAX_DIR_HANDLES) return;
    g_ppc_fs_dirs[handle - 1] = NULL;
}

/* FSStatus FSOpenDir(FSClient*, FSCmdBlock*, const char *path, FSDirectoryHandle*, FSErrorFlag);
 * r3=client r4=block r5=path r6=out_handle r7=errorMask */
static inline void ppc_import_coreinit_FSOpenDir(PpcContext *ctx) {
    char path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t handle = ppc_fs_alloc_dir_handle(d);
    if (handle == 0) {
        closedir(d);
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_ACCESS_ERROR;
        return;
    }
    ppc_store_u32(ctx, ctx->r[6], handle);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSCloseDir(FSClient*, FSCmdBlock*, FSDirectoryHandle, FSErrorFlag);
 * r3=client r4=block r5=handle r6=errorMask */
static inline void ppc_import_coreinit_FSCloseDir(PpcContext *ctx) {
    DIR *d = ppc_fs_get_dir_handle(ctx->r[5]);
    if (!d) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    closedir(d);
    ppc_fs_free_dir_handle(ctx->r[5]);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSReadDir(FSClient*, FSCmdBlock*, FSDirectoryHandle, FSDirectoryEntry*, FSErrorFlag);
 * r3=client r4=block r5=handle r6=out_entry r7=errorMask
 *
 * FSDirectoryEntry (from wut's coreinit/filesystem.h): `struct
 * FSDirectoryEntry { FSStat info; char name[256]; };` -- info is the same
 * 0x64-byte FSStat FSGetStatFile fills in (same "only `size`/`flags` are
 * real, rest zeroed" simplification), name follows immediately at +0x64,
 * NUL-terminated, matching a plain C string. FS_STATUS_END (-2) is the
 * real, documented "no more entries" signal real code branches on to end
 * its listing loop -- not just a generic error. */
static inline void ppc_import_coreinit_FSReadDir(PpcContext *ctx) {
    DIR *d = ppc_fs_get_dir_handle(ctx->r[5]);
    if (!d) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    /* Real Cafe OS directory listings don't include self/parent entries
     * (that's POSIX opendir()'s own convention, not a Wii U one) -- skip
     * them so real listing code iterating this doesn't see two entries
     * it wouldn't on real hardware. */
    struct dirent *de;
    do {
        de = readdir(d);
    } while (de && (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0));
    if (!de) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_END;
        return;
    }
    uint32_t entry_addr = ctx->r[6];
    for (uint32_t i = 0; i < 0x64; i += 4) ppc_store_u32(ctx, entry_addr + i, 0);
    /* FSStat::flags (offset 0x00) real hardware sets bit 0x80000000 for
     * directories -- the one flag bit real listing code actually checks
     * to tell files and subdirectories apart. */
#ifdef DT_DIR
    if (de->d_type == DT_DIR) ppc_store_u32(ctx, entry_addr + 0x00, 0x80000000u);
#endif
    uint32_t name_addr = entry_addr + 0x64;
    size_t i = 0;
    for (; de->d_name[i] && i + 1 < 256; i++) ppc_store_u8(ctx, name_addr + (uint32_t)i, (uint8_t)de->d_name[i]);
    ppc_store_u8(ctx, name_addr + (uint32_t)i, 0);
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSMakeDir(FSClient*, FSCmdBlock*, const char *path, FSErrorFlag);
 * r3=client r4=block r5=path r6=errorMask */
static inline void ppc_import_coreinit_FSMakeDir(PpcContext *ctx) {
    char path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], path, sizeof(path));
#ifdef _WIN32
    int rc = mkdir(path);
#else
    int rc = mkdir(path, 0777);
#endif
    if (rc != 0) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_EXISTS;
        return;
    }
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

/* FSStatus FSRemove(FSClient*, FSCmdBlock*, const char *path, FSErrorFlag);
 * r3=client r4=block r5=path r6=errorMask */
static inline void ppc_import_coreinit_FSRemove(PpcContext *ctx) {
    char path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], path, sizeof(path));
    if (remove(path) != 0) {
        ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_NOT_FOUND;
        return;
    }
    ctx->r[3] = (uint32_t)BRAMBLE_FS_STATUS_OK;
}

#endif /* BRAMBLE_CAFEOS_COREINIT_FS_H */
