#ifndef ARKCHEMY_CAFEOS_GX2_SHADERS_H
#define ARKCHEMY_CAFEOS_GX2_SHADERS_H

/* Matching the shaders the game binds to the deko3d modules built from them.
 *
 * The game never names a shader. It hands GX2SetVertexShader a pointer to a
 * GX2VertexShader, whose program is somewhere in guest memory, and expects the
 * driver to work out the rest. So this side has to recognise a program by its
 * contents: hash the bytes, and open the module named after the hash.
 *
 * Hashing the bytes rather than trusting the pointer matters twice over. The
 * same shader is re-bound 57,438 times a run from addresses the allocator
 * chooses, so a pointer is not an identity; and the sizes collide -- four of
 * the seven programs this game binds are within 40 bytes of each other, and
 * two are exactly 400 bytes and different shaders.
 *
 * FNV-1a 64, because blaster/build-shaders.sh has to compute the identical
 * value over the identical bytes when it names the file. That is the whole
 * reason it is not SHA-256: this end of the agreement is nine lines, and the
 * two ends have to be readable side by side to stay in agreement.
 *
 * What this does NOT do yet is draw with them. Loading is a separate question
 * from binding, and worth answering on its own: it says whether uam's output
 * is accepted by deko3d on the real device, which is the last thing between a
 * translated shader and a drawn one. A module that loads and is never used
 * costs a few KB; a draw path built on modules that turn out not to load
 * costs a day.
 *
 * The modules live in switch/Jouster/modules, deliberately not in
 * switch/Jouster/shaders beside the dumps they were built from. The courier
 * watches the dump directory for new filenames and pulls them; putting built
 * modules there means it pulls them straight back and files them as if the
 * console had produced them. It did that once, on 2026-09-12, within a minute
 * of the first push.
 */

#include "ppc_runtime.h"

#ifdef ARKCHEMY_HOSTTEST
static inline void ark_shd_module_bind(PpcContext *ctx, int kind, uint32_t obj)
{ (void)ctx; (void)kind; (void)obj; }
#else

#include <deko3d.h>
#include <stdio.h>
#include <string.h>

#define ARKCHEMY_SHD_MODULES 16u

typedef struct {
    uint64_t key;
    DkMemBlock code;
    DkShader shader;
    uint32_t size;      /* bytes of the guest program, for the log */
    int stage;          /* 0 vertex, 1 pixel, as the bind that found it */
    bool valid;
} ArkchemyShaderModule;

/* Weak, not static. This header is included by main.c and by the translation
 * units carrying the recompiled game, and `static` in a header gives each of
 * them a private copy: the shims run in one, main.c prints another's, and the
 * log reads distinct=0 while the loader is working perfectly. That is exactly
 * what the first version of this did -- 1,293 shader binds, nothing recorded.
 * Weak definitions collapse to one symbol at link, which is the idiom the
 * rest of the probes in ppc_runtime.h already use. */
#ifdef __GNUC__
__attribute__((weak))
#endif
ArkchemyShaderModule g_ark_shdmod[ARKCHEMY_SHD_MODULES];
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_shdmod_n = 0,
                  g_ark_shdmod_hits = 0,     /* binds served from the cache */
                  g_ark_shdmod_missing = 0,  /* no module file for that hash */
                  g_ark_shdmod_failed = 0,   /* file there, deko3d refused it */
                  g_ark_shdmod_early = 0;    /* bound before GX2Init ran */
/* The pair currently bound, as module index + 1 so that zero means "none".
 * A draw needs both, and needs them to be the modules for the programs the
 * game bound most recently -- not merely for some module to be loaded. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_shdmod_cur[2] = {0, 0};

/* FNV-1a 64 over the program bytes exactly as they sit in guest memory --
 * the same byte sequence ark_shd_sink writes out, so the dumps the modules
 * were built from and the hash computed here cannot disagree about what the
 * program is. */
static inline uint64_t ark_shd_hash(PpcContext *ctx, uint32_t prog, uint32_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < size; i++) {
        h ^= (uint64_t)ppc_load_u8(ctx, prog + i);
        h *= 0x100000001b3ull;
    }
    return h;
}

static inline bool ark_shd_module_load(ArkchemyShaderModule *m, uint64_t key)
{
    char path[96];
    snprintf(path, sizeof(path),
             "sdmc:/switch/Jouster/modules/%016llx.dksh",
             (unsigned long long)key);
    FILE *fh = fopen(path, "rb");
    if (!fh) return false;

    fseek(fh, 0, SEEK_END);
    long len = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (len <= 0 || len > 0x100000) { fclose(fh); return false; }

    /* Code memory has to be a whole number of deko3d blocks. */
    uint32_t rounded = ((uint32_t)len + DK_MEMBLOCK_ALIGNMENT - 1u)
                       & ~(DK_MEMBLOCK_ALIGNMENT - 1u);
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, g_arkchemy_gx2.device, rounded);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached
                | DkMemBlockFlags_Code;
    m->code = dkMemBlockCreate(&maker);
    if (!m->code) { fclose(fh); return false; }

    void *dst = dkMemBlockGetCpuAddr(m->code);
    size_t got = fread(dst, 1, (size_t)len, fh);
    fclose(fh);
    if (got != (size_t)len) { dkMemBlockDestroy(m->code); m->code = NULL; return false; }

    DkShaderMaker smaker;
    dkShaderMakerDefaults(&smaker, m->code, 0);
    dkShaderInitialize(&m->shader, &smaker);
    if (!dkShaderIsValid(&m->shader)) {
        dkMemBlockDestroy(m->code);
        m->code = NULL;
        return false;
    }
    return true;
}

/* Shader objects are re-bound 57,438 times a run from a handful of distinct
 * addresses, and hashing a 1,296-byte program on every one of those would be
 * a megabyte of hashing per second to answer a question already answered.
 * Memoised on the object pointer, which is what actually repeats. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_shdmod_seen[ARKCHEMY_SHD_MODULES];
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_shdmod_seen_mod[ARKCHEMY_SHD_MODULES];  /* index + 1 */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_shdmod_seen_n = 0;

static inline void ark_shd_module_bind(PpcContext *ctx, int kind, uint32_t obj)
{
    /* Before GX2Init there is no device to put code in, and the engine does
     * bind shaders early. Not an error -- just nothing to do yet. */
    if (!g_arkchemy_gx2.initialized || !g_arkchemy_gx2.device) {
        /* Counted rather than ignored: "the loader saw nothing" and "the
         * loader ran before there was a device to load into" look identical
         * in a log that only reports what it managed to load. */
        g_ark_shdmod_early++;
        return;
    }
    if (!obj) return;

    for (uint32_t i = 0; i < g_ark_shdmod_seen_n; i++)
        if (g_ark_shdmod_seen[i] == obj) {
            g_ark_shdmod_hits++;
            g_ark_shdmod_cur[kind ? 1 : 0] = g_ark_shdmod_seen_mod[i];
            return;
        }

    /* Confirmed against hardware: GX2VertexShader carries size at 0xD0 and
     * program at 0xD4, GX2PixelShader at 0xA4 and 0xA8. */
    uint32_t off = kind ? 0xA4u : 0xD0u;
    uint32_t size = ppc_load_u32(ctx, obj + off);
    uint32_t prog = ppc_load_u32(ctx, obj + off + 4u);
    if (!prog || !size || size > 65536u) return;

    uint32_t memo = g_ark_shdmod_seen_n < ARKCHEMY_SHD_MODULES
                    ? g_ark_shdmod_seen_n++ : ARKCHEMY_SHD_MODULES;
    if (memo < ARKCHEMY_SHD_MODULES) {
        g_ark_shdmod_seen[memo] = obj;
        g_ark_shdmod_seen_mod[memo] = 0;
    }

    uint64_t key = ark_shd_hash(ctx, prog, size);
    for (uint32_t i = 0; i < g_ark_shdmod_n; i++) {
        if (g_ark_shdmod[i].key == key) {
            if (g_ark_shdmod[i].valid) {
                g_ark_shdmod_hits++;
                g_ark_shdmod_cur[kind ? 1 : 0] = i + 1u;
                if (memo < ARKCHEMY_SHD_MODULES) g_ark_shdmod_seen_mod[memo] = i + 1u;
            }
            return;                      /* already resolved, either way */
        }
    }
    if (g_ark_shdmod_n >= ARKCHEMY_SHD_MODULES) return;

    ArkchemyShaderModule *m = &g_ark_shdmod[g_ark_shdmod_n++];
    m->key = key;
    m->size = size;
    m->stage = kind;
    m->valid = ark_shd_module_load(m, key);
    if (!m->valid) {
        /* Distinguish "no module built for this program" from "the module is
         * there and deko3d would not take it". The first is a translator
         * coverage gap; the second means uam's output is wrong or this loader
         * is. They have nothing to do with each other. */
        char path[96];
        snprintf(path, sizeof(path),
                 "sdmc:/switch/Jouster/modules/%016llx.dksh",
                 (unsigned long long)key);
        FILE *probe = fopen(path, "rb");
        if (probe) { fclose(probe); g_ark_shdmod_failed++; }
        else g_ark_shdmod_missing++;
    } else {
        g_ark_shdmod_hits++;
        g_ark_shdmod_cur[kind ? 1 : 0] = g_ark_shdmod_n;   /* index + 1 */
        if (memo < ARKCHEMY_SHD_MODULES)
            g_ark_shdmod_seen_mod[memo] = g_ark_shdmod_n;
    }
}

#endif /* ARKCHEMY_HOSTTEST */
#endif /* ARKCHEMY_CAFEOS_GX2_SHADERS_H */
