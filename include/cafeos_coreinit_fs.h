#ifndef ARKCHEMY_CAFEOS_COREINIT_FS_H
#define ARKCHEMY_CAFEOS_COREINIT_FS_H

#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
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
 *    ppc_fs_translate_path() (below) now handles this: it recognizes the
 *    real Wii U mount prefixes (/vol/content/, content:/, /vol/save/,
 *    save:/) and any bare relative path (real game code commonly assumes
 *    its current directory is already the content mount) and rewrites
 *    them onto ARKCHEMY_FS_CONTENT_ROOT / ARKCHEMY_FS_SAVE_ROOT on the SD
 *    card -- see that function's own comment for the exact rules and the
 *    real reasoning behind picking these two roots specifically.
 *  - Only ever reports the most recent error (a single global, not
 *    tracked per-FSClient the way real hardware does).
 */

enum {
    ARKCHEMY_FS_STATUS_OK = 0,
    ARKCHEMY_FS_STATUS_CANCELLED = -1,
    ARKCHEMY_FS_STATUS_END = -2,
    ARKCHEMY_FS_STATUS_ALREADY_OPEN = -4,
    ARKCHEMY_FS_STATUS_EXISTS = -5,
    ARKCHEMY_FS_STATUS_NOT_FOUND = -6,
    ARKCHEMY_FS_STATUS_NOT_FILE = -7,
    ARKCHEMY_FS_STATUS_NOT_DIR = -8,
    ARKCHEMY_FS_STATUS_ACCESS_ERROR = -9,
};

#define ARKCHEMY_FS_MAX_HANDLES 64

extern FILE *g_ppc_fs_files[ARKCHEMY_FS_MAX_HANDLES]; /* real definition in cafeos_state.c -- see its own file comment */
extern int32_t g_ppc_fs_last_error; /* real definition in cafeos_state.c -- see its own file comment */

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

/* Where a real, user-extracted game dump's content and save-data
 * directories live on the SD card -- deliberately not under this .nro's
 * own switch/Jouster/ log directory, since this is the user's own
 * legally-dumped game data, not something this project generates or
 * distributes (see LICENSE section 6). Not `const` so a future real
 * settings/setup screen can point this at wherever the user actually put
 * their dump instead. */
extern char g_arkchemy_fs_content_root[256]; /* real definition in cafeos_state.c -- see its own file comment */
extern char g_arkchemy_fs_save_root[256]; /* real definition in cafeos_state.c -- see its own file comment */

/* Rewrites a real Wii U guest FS path onto a real host path under the
 * roots above. Real Wii U game code addresses files through one of a
 * small number of mounted volumes, most commonly the read-only content
 * mount (paths seen in the wild as either "/vol/content/..." or
 * "content:/...") and the per-title save mount ("/vol/save/..." or
 * "save:/..."); this shim doesn't emulate FSMount/FSBindMount's own real
 * volume-table bookkeeping, just recognizes these fixed, well-known
 * prefixes directly. A path with neither prefix is treated as already
 * relative to the content mount -- real game code frequently opens files
 * with bare relative paths (e.g. "data/foo.szs"), assuming its current
 * directory already is the content volume, which real hardware sets up
 * before the game's own code ever runs. `out` must be at least
 * `out_size` bytes; truncates rather than overflowing if a real path
 * turns out to be pathological. */
/* Case-insensitive component lookup, used only when an exact path misses.
 *
 * The Wii U tolerated case differences between what the game asks for and
 * what is on the disc; the SD card the Switch build reads is not guaranteed
 * to. This is not hypothetical for this title -- comparing the boot order
 * Cemu logged against the files actually present shows two mismatches in the
 * first handful of loads:
 *
 *     game asks for                on the card
 *     character/init_setup.bld     character/Init_Setup.bld
 *     level/title.arc              level/Title.arc
 *
 * Both are reached before anything renders. A miss here would surface as an
 * ordinary "file not found" and read exactly like a bug in the loader, which
 * is a bad way to lose a day.
 *
 * Returns 1 and writes the real spelling into `out` if a match exists. */
static inline int ppc_fs_lookup_ci(const char *dir, const char *name,
                                   char *out, size_t out_size) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, out_size, "%s", e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* Rebuilds `root`/`rest` one component at a time, correcting the case of any
 * component that does not exist as spelled. Only called after the exact path
 * has already missed, so the common case costs nothing. A component that has
 * no case-insensitive match either is left as-is, so a genuinely absent file
 * still cleanly reports NOT_FOUND (-6) -- which the engine relies on: it
 * probes for a .arc, expects -6, and falls back to the .bld. */
static inline void ppc_fs_resolve_case(const char *root, const char *rest,
                                       char *out, size_t out_size) {
    char cur[512];
    char comp[256];
    char joined[768];
    char fixed[256];
    const char *p = rest;

    snprintf(cur, sizeof(cur), "%s", root);
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || len >= sizeof(comp)) {
            snprintf(out, out_size, "%s/%s", root, rest);
            return;
        }
        memcpy(comp, p, len);
        comp[len] = '\0';

        snprintf(joined, sizeof(joined), "%s/%s", cur, comp);
        if (access(joined, F_OK) != 0 &&
            ppc_fs_lookup_ci(cur, comp, fixed, sizeof(fixed))) {
            snprintf(joined, sizeof(joined), "%s/%s", cur, fixed);
        }
        snprintf(cur, sizeof(cur), "%s", joined);

        p = slash ? slash + 1 : p + len;
        while (*p == '/') p++;
    }
    snprintf(out, out_size, "%s", cur);
}

static inline void ppc_fs_translate_path(const char *guest_path, char *out, size_t out_size) {
    const char *root = g_arkchemy_fs_content_root;
    const char *rest = guest_path;
    if (strncmp(guest_path, "/vol/content/", 13) == 0) {
        rest = guest_path + 13;
    } else if (strncmp(guest_path, "content:/", 9) == 0) {
        rest = guest_path + 9;
    } else if (strncmp(guest_path, "/vol/save/", 10) == 0) {
        root = g_arkchemy_fs_save_root;
        rest = guest_path + 10;
    } else if (strncmp(guest_path, "save:/", 6) == 0) {
        root = g_arkchemy_fs_save_root;
        rest = guest_path + 6;
    } else if (guest_path[0] == '/') {
        /* An absolute path under some other/unrecognized real mount --
         * pass it through unchanged rather than guessing; a real fopen()
         * against it will just cleanly fail with "not found", same as
         * this shim's prior always-passthrough behavior did for every
         * path. */
        snprintf(out, out_size, "%s", guest_path);
        return;
    }
    while (rest[0] == '/') rest++;
    if (rest[0] == '\0') {
        snprintf(out, out_size, "%s", root);
    } else {
        snprintf(out, out_size, "%s/%s", root, rest);
        /* Exact spelling first -- it is what the disc used and what almost
         * every path will be. Only pay for a directory scan on a miss. */
        if (access(out, F_OK) != 0) {
            ppc_fs_resolve_case(root, rest, out, out_size);
        }
    }
}

/* Real, optional logging hook for FSOpenFile -- same real reasoning and
 * pattern as ppc_runtime.h's own ppc_set_unhandled_log: a real,
 * first-ever run of the actual game against a real player-supplied dump
 * is exactly when knowing *what the game actually tried to open, and
 * whether it found it* matters most, and there's no other way to see
 * that short of this. `extern`, not `static`, for the same real
 * multi-translation-unit reason as everything else in this header --
 * shared, single definition lives in cafeos_state.c. */
typedef void (*ppc_fs_open_log_fn)(const char *guest_path, const char *real_path, const char *mode, int found, uint32_t handle, uint32_t handles_in_use);
extern ppc_fs_open_log_fn g_ppc_fs_open_log; /* real definition in cafeos_state.c -- see its own file comment */
static inline void ppc_fs_set_open_log(ppc_fs_open_log_fn fn) { g_ppc_fs_open_log = fn; }

/* 1-based handles (0 reserved so a zeroed-out FSFileHandle reads as
 * invalid, matching the convention real code's error paths rely on). */
static inline uint32_t ppc_fs_alloc_handle(FILE *f) {
    for (uint32_t i = 0; i < ARKCHEMY_FS_MAX_HANDLES; i++) {
        if (!g_ppc_fs_files[i]) {
            g_ppc_fs_files[i] = f;
            return i + 1;
        }
    }
    return 0;
}

/* Diagnostic only -- lets a log line show whether a failed alloc was
 * really pool exhaustion (ARKCHEMY_FS_MAX_HANDLES all in use, e.g. from
 * real game code that never calls FSCloseFile on some path) versus
 * something else. */
static inline uint32_t ppc_fs_handles_in_use(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ARKCHEMY_FS_MAX_HANDLES; i++) {
        if (g_ppc_fs_files[i]) n++;
    }
    return n;
}

static inline FILE *ppc_fs_get_handle(uint32_t handle) {
    if (handle == 0 || handle > ARKCHEMY_FS_MAX_HANDLES) return NULL;
    return g_ppc_fs_files[handle - 1];
}

static inline void ppc_fs_free_handle(uint32_t handle) {
    if (handle == 0 || handle > ARKCHEMY_FS_MAX_HANDLES) return;
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
    g_ppc_fs_last_error = ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_coreinit_FSShutdown(PpcContext *ctx) {
    (void)ctx;
    for (uint32_t i = 0; i < ARKCHEMY_FS_MAX_HANDLES; i++) {
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
static inline void ppc_import_coreinit_FSAddClient(PpcContext *ctx) { ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK; }
static inline void ppc_import_coreinit_FSDelClient(PpcContext *ctx) { ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK; }

/* void FSInitCmdBlock(FSCmdBlock *block); -- FSCmdBlock is opaque too, and
 * this shim runs every FS call synchronously (no real async command-queue
 * model), so there's no per-block state to initialize. */
static inline void ppc_import_coreinit_FSInitCmdBlock(PpcContext *ctx) { (void)ctx; }

/* FSStatus FSGetLastError(FSClient *client); */
static inline void ppc_import_coreinit_FSGetLastError(PpcContext *ctx) { ctx->r[3] = (uint32_t)g_ppc_fs_last_error; }

/* FSError FSGetLastErrorCodeForViewer(FSClient *client); -- real signature
 * confirmed against wut's coreinit/filesystem.h. A debug/viewer-facing
 * variant of the same last-error state FSGetLastError already reports;
 * this shim doesn't distinguish the two the real API might (e.g. a more
 * detailed underlying code vs. a coarser FSStatus), so both read the
 * same g_ppc_fs_last_error. */
static inline void ppc_import_coreinit_FSGetLastErrorCodeForViewer(PpcContext *ctx) {
    ctx->r[3] = (uint32_t)g_ppc_fs_last_error;
}

/* void FSSetStateChangeNotification(FSClient *client, FSStateChangeParams *info);
 * real signature confirmed against wut's coreinit/filesystem.h -- void
 * return, registers a callback for FS state changes (e.g. SD card
 * removal). Safe no-op: nothing in this shim ever triggers such an
 * event to invoke the callback from. */
static inline void ppc_import_coreinit_FSSetStateChangeNotification(PpcContext *ctx) { (void)ctx; }

/* FSStatus FSIsEof(FSClient*, FSCmdBlock*, FSFileHandle, FSErrorFlag);
 * r3=client r4=block r5=handle r6=errorMask -- lower confidence than the
 * rest of this file: not found in wut's public filesystem.h (same
 * situation as cafeos_coreinit_im.h's IM* functions), so the parameter
 * shape is inferred from every other FS* function's consistent
 * (client, block, handle, errorMask) pattern rather than confirmed
 * directly. Returns FS_STATUS_END when at end-of-file, matching how
 * FSReadDir already signals "no more" with the same code, else
 * FS_STATUS_OK. */
static inline void ppc_import_coreinit_FSIsEof(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    int c = fgetc(f);
    if (c == EOF) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_END;
        return;
    }
    ungetc(c, f);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSOpenFile(FSClient *client, FSCmdBlock *block, const char *path,
 *                      const char *mode, FSFileHandle *handle, FSErrorFlag errorMask);
 * r3=client r4=block r5=path r6=mode r7=out_handle r8=errorMask */
/* The counters above cover the SYNCHRONOUS read only. An engine with a file
 * scheduler and worker threads reads asynchronously, so those two said
 * nothing about the path that matters -- an instrument that reports nothing
 * proves nothing until it is shown capable of reporting something. Counted
 * separately rather than folded in, because "reads are happening, just not
 * synchronously" and "no reads at all" are different answers. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_async_read_calls = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_async_read_bytes = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_open_calls = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_last_read_handle = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_last_read_pos = 0;

static inline void ppc_import_coreinit_FSOpenFile(PpcContext *ctx) {
    char guest_path[512], real_path[512], mode[8];
    g_arkchemy_fs_open_calls++;
    ppc_fs_read_cstr(ctx, ctx->r[5], guest_path, sizeof(guest_path));
    ppc_fs_read_cstr(ctx, ctx->r[6], mode, sizeof(mode));
    ppc_fs_translate_path(guest_path, real_path, sizeof(real_path));

    FILE *f = fopen(real_path, mode);
    if (!f) {
        if (g_ppc_fs_open_log) g_ppc_fs_open_log(guest_path, real_path, mode, 0, 0, ppc_fs_handles_in_use());
        g_ppc_fs_last_error = ARKCHEMY_FS_STATUS_NOT_FOUND;
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t handle = ppc_fs_alloc_handle(f);
    if (handle == 0) {
        if (g_ppc_fs_open_log) g_ppc_fs_open_log(guest_path, real_path, mode, 1, 0, ppc_fs_handles_in_use());
        fclose(f);
        g_ppc_fs_last_error = ARKCHEMY_FS_STATUS_ACCESS_ERROR;
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_ACCESS_ERROR;
        return;
    }
    ppc_store_u32(ctx, ctx->r[7], handle);
    if (g_ppc_fs_open_log) g_ppc_fs_open_log(guest_path, real_path, mode, 1, handle, ppc_fs_handles_in_use());
    g_ppc_fs_last_error = ARKCHEMY_FS_STATUS_OK;
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSCloseFile(FSClient*, FSCmdBlock*, FSFileHandle handle, FSErrorFlag);
 * r3=client r4=block r5=handle r6=errorMask */
static inline void ppc_import_coreinit_FSCloseFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    fclose(f);
    ppc_fs_free_handle(ctx->r[5]);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSReadFile(FSClient*, FSCmdBlock*, uint8_t *buffer, uint32_t size,
 *                      uint32_t count, FSFileHandle handle, uint32_t unk1, FSErrorFlag);
 * r3=client r4=block r5=buffer r6=size r7=count r8=handle r9=unk1 r10=errorMask
 * Real return value is the number of *elements* (not bytes) read on
 * success, matching fread()'s own return convention -- confirmed against
 * real FS API documentation describing FSReadFile as fread()-shaped. */
/* Read accounting. FSOpenFile has always logged; reads never did, so a run
 * could show a file being opened and nothing at all about whether data ever
 * came out of it. That gap mattered on 2026-08-29: BinkDoFrame returned
 * success with completely untouched output planes, and "did the decoder get
 * any compressed data" was unanswerable from the log. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_read_calls = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_arkchemy_fs_read_bytes = 0;


static inline void ppc_import_coreinit_FSReadFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[8]);
    /* size is r6 and count is r7; the Wii U idiom is size=1/count=N, so
     * summing r6 alone counted calls, not bytes. Bink's decode truncates
     * partway down each frame and the read pattern is the thing that would
     * explain it, so log the first few reads outright rather than inferring
     * from a total. */
    g_arkchemy_fs_read_calls++;
    g_arkchemy_fs_read_bytes += ctx->r[6] * ctx->r[7];
    if (g_arkchemy_fs_read_calls <= 24u && g_ppc_fs_open_log) {
        char note[64];
        snprintf(note, sizeof(note), "read#%u size=%u", (unsigned)g_arkchemy_fs_read_calls,
                 (unsigned)(ctx->r[6] * ctx->r[7]));
        g_ppc_fs_open_log(note, "(FSReadFile)", "r", 1, ctx->r[8], ppc_fs_handles_in_use());
    }
    if (!f) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
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
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    long pos = ftell(f);
    ppc_store_u32(ctx, ctx->r[6], (uint32_t)pos);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSSetPosFile(FSClient*, FSCmdBlock*, FSFileHandle, uint32_t pos, FSErrorFlag);
 * r3=client r4=block r5=handle r6=pos r7=errorMask */
static inline void ppc_import_coreinit_FSSetPosFile(PpcContext *ctx) {
    FILE *f = ppc_fs_get_handle(ctx->r[5]);
    if (!f) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    fseek(f, (long)ctx->r[6], SEEK_SET);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
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
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, cur, SEEK_SET);

    uint32_t stat_addr = ctx->r[6];
    for (uint32_t i = 0; i < 0x64; i += 4) ppc_store_u32(ctx, stat_addr + i, 0);
    ppc_store_u32(ctx, stat_addr + 0x10, (uint32_t)size);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* Directory operations -- same handle-table pattern as the file ones
 * above, just a second table since FSFileHandle and FSDirectoryHandle
 * are numbered independently on real hardware. */
#define ARKCHEMY_FS_MAX_DIR_HANDLES 32
extern DIR *g_ppc_fs_dirs[ARKCHEMY_FS_MAX_DIR_HANDLES]; /* real definition in cafeos_state.c -- see its own file comment */

static inline uint32_t ppc_fs_alloc_dir_handle(DIR *d) {
    for (uint32_t i = 0; i < ARKCHEMY_FS_MAX_DIR_HANDLES; i++) {
        if (!g_ppc_fs_dirs[i]) {
            g_ppc_fs_dirs[i] = d;
            return i + 1;
        }
    }
    return 0;
}

static inline DIR *ppc_fs_get_dir_handle(uint32_t handle) {
    if (handle == 0 || handle > ARKCHEMY_FS_MAX_DIR_HANDLES) return NULL;
    return g_ppc_fs_dirs[handle - 1];
}

static inline void ppc_fs_free_dir_handle(uint32_t handle) {
    if (handle == 0 || handle > ARKCHEMY_FS_MAX_DIR_HANDLES) return;
    g_ppc_fs_dirs[handle - 1] = NULL;
}

/* FSStatus FSOpenDir(FSClient*, FSCmdBlock*, const char *path, FSDirectoryHandle*, FSErrorFlag);
 * r3=client r4=block r5=path r6=out_handle r7=errorMask */
static inline void ppc_import_coreinit_FSOpenDir(PpcContext *ctx) {
    char guest_path[512], real_path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], guest_path, sizeof(guest_path));
    ppc_fs_translate_path(guest_path, real_path, sizeof(real_path));
    DIR *d = opendir(real_path);
    if (!d) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t handle = ppc_fs_alloc_dir_handle(d);
    if (handle == 0) {
        closedir(d);
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_ACCESS_ERROR;
        return;
    }
    ppc_store_u32(ctx, ctx->r[6], handle);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSCloseDir(FSClient*, FSCmdBlock*, FSDirectoryHandle, FSErrorFlag);
 * r3=client r4=block r5=handle r6=errorMask */
static inline void ppc_import_coreinit_FSCloseDir(PpcContext *ctx) {
    DIR *d = ppc_fs_get_dir_handle(ctx->r[5]);
    if (!d) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    closedir(d);
    ppc_fs_free_dir_handle(ctx->r[5]);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
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
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
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
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_END;
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
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSMakeDir(FSClient*, FSCmdBlock*, const char *path, FSErrorFlag);
 * r3=client r4=block r5=path r6=errorMask */
static inline void ppc_import_coreinit_FSMakeDir(PpcContext *ctx) {
    char guest_path[512], path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], guest_path, sizeof(guest_path));
    ppc_fs_translate_path(guest_path, path, sizeof(path));
#ifdef _WIN32
    int rc = mkdir(path);
#else
    int rc = mkdir(path, 0777);
#endif
    if (rc != 0) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_EXISTS;
        return;
    }
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/* FSStatus FSRemove(FSClient*, FSCmdBlock*, const char *path, FSErrorFlag);
 * r3=client r4=block r5=path r6=errorMask */
static inline void ppc_import_coreinit_FSRemove(PpcContext *ctx) {
    char guest_path[512], path[512];
    ppc_fs_read_cstr(ctx, ctx->r[5], guest_path, sizeof(guest_path));
    ppc_fs_translate_path(guest_path, path, sizeof(path));
    if (remove(path) != 0) {
        ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_NOT_FOUND;
        return;
    }
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

/*
 * FSReadFileWithPosAsync/FSWriteFileWithPosAsync: previously deferred --
 * both take an `FSAsyncData *` whose real behavior is to invoke a guest
 * callback function pointer on completion, which this shim couldn't do
 * without a way to call *into* recompiled code. `ppc_runtime.h` now
 * declares `ppc_dispatch` for exactly this (see its own comment) -- real
 * generated programs already define it (built for mtctr/bctrl indirect
 * calls), and a guest function pointer is a guest function pointer
 * either way, so reusing it here isn't a new mechanism.
 *
 * FSStatus FSReadFileWithPosAsync(FSClient*, FSCmdBlock*, uint8_t *buffer,
 *   uint32_t size, uint32_t count, uint32_t pos, FSFileHandle handle,
 *   uint32_t unk1, FSErrorFlag errorMask, FSAsyncData *asyncData);
 * 10 real parameters -- only 8 fit in r3-r10 (PPC32 SVR4 ABI), so
 * errorMask/asyncData are the caller's 9th/10th args, stack-passed at
 * r1+8/r1+12 -- this project's own manyargs.c test already confirmed
 * this "just works" with the existing memory-access codegen, so reading
 * them the same way here is consistent, not a new assumption. Only
 * asyncData (r1+12) is actually read; errorMask (r1+8) is ignored, same
 * as every other FS* function in this file (see the file's own "Known,
 * deliberate gaps" note).
 *
 * real FSAsyncData layout (coreinit/filesystem.h, WUT_CHECK_OFFSET-
 * confirmed): callback@0x0, param@0x4, ioMsgQueue@0x8 (size 0xC).
 * FSAsyncCallback is `void (*)(FSClient*, FSCmdBlock*, FSStatus, uint32_t)`.
 *
 * This shim completes the read/write synchronously (no real async I/O
 * queue exists), then invokes the guest callback immediately with the
 * real result -- correct from real calling code's perspective as long as
 * it only observes completion through the callback, which is the normal
 * usage pattern. If `asyncData->ioMsgQueue` is used instead of `callback`
 * (an alternative real completion style, message-queue-based rather than
 * callback-based), this is a known, honestly-documented gap: no
 * OSMessageQueue send machinery exists here, so that path silently never
 * completes. The Async call's own synchronous return value is real
 * `FS_STATUS_OK` ("successfully queued"), matching how real code
 * typically ignores it and waits for the callback's own status instead.
 */
static inline void ppc_fs_invoke_async_callback(PpcContext *ctx, uint32_t async_data_addr, uint32_t client, uint32_t block, int32_t status) {
    uint32_t callback_addr = ppc_load_u32(ctx, async_data_addr + 0x0);
    uint32_t param = ppc_load_u32(ctx, async_data_addr + 0x4);
    if (callback_addr == 0) return; /* ioMsgQueue-style completion -- known gap, see file comment */
    ctx->r[3] = client;
    ctx->r[4] = block;
    ctx->r[5] = (uint32_t)status;
    ctx->r[6] = param;
    ppc_dispatch(ctx, callback_addr);
}

static inline void ppc_import_coreinit_FSReadFileWithPosAsync(PpcContext *ctx) {
    uint32_t client = ctx->r[3], block = ctx->r[4];
    uint32_t buffer_addr = ctx->r[5], size = ctx->r[6], count = ctx->r[7], pos = ctx->r[8], handle = ctx->r[9];
    uint32_t async_data_addr = ppc_load_u32(ctx, ctx->r[1] + 12);
    FILE *f = ppc_fs_get_handle(handle);
    int32_t result;
    g_arkchemy_fs_async_read_calls++;
    g_arkchemy_fs_async_read_bytes += size * count;
    g_arkchemy_fs_last_read_handle = handle;
    g_arkchemy_fs_last_read_pos = pos;
    if (!f) {
        result = ARKCHEMY_FS_STATUS_NOT_FOUND;
    } else if (size == 0 || count == 0) {
        result = 0;
    } else {
        fseek(f, (long)pos, SEEK_SET);
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
        result = (int32_t)elements_read;
    }
    ppc_fs_invoke_async_callback(ctx, async_data_addr, client, block, result);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

static inline void ppc_import_coreinit_FSWriteFileWithPosAsync(PpcContext *ctx) {
    uint32_t client = ctx->r[3], block = ctx->r[4];
    uint32_t buffer_addr = ctx->r[5], size = ctx->r[6], count = ctx->r[7], pos = ctx->r[8], handle = ctx->r[9];
    uint32_t async_data_addr = ppc_load_u32(ctx, ctx->r[1] + 12);
    FILE *f = ppc_fs_get_handle(handle);
    int32_t result;
    if (!f) {
        result = ARKCHEMY_FS_STATUS_NOT_FOUND;
    } else if (size == 0 || count == 0) {
        result = 0;
    } else {
        fseek(f, (long)pos, SEEK_SET);
        uint32_t elements_written = 0;
        for (uint32_t e = 0; e < count; e++) {
            uint32_t wrote_this_element = 0;
            for (uint32_t b = 0; b < size; b++) {
                uint8_t byte = ppc_load_u8(ctx, buffer_addr + e * size + b);
                if (fputc(byte, f) == EOF) break;
                wrote_this_element++;
            }
            if (wrote_this_element < size) break;
            elements_written++;
        }
        fflush(f);
        result = (int32_t)elements_written;
    }
    ppc_fs_invoke_async_callback(ctx, async_data_addr, client, block, result);
    ctx->r[3] = (uint32_t)ARKCHEMY_FS_STATUS_OK;
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_FS_H */
