#ifndef ARKCHEMY_CAFEOS_GX2_DRAW_H
#define ARKCHEMY_CAFEOS_GX2_DRAW_H

/* GX2DrawEx as a deko3d draw.
 *
 * Everything this needs now exists: the shader modules load (SHADERMOD, seven
 * of seven), the attribute streams are recorded, the uniform-register writes
 * are recorded, and the GX2 bridge already owns a device, a command buffer, a
 * queue and bound render targets. This is the piece that joins them.
 *
 * A draw is only attempted when every part of the state it needs is present.
 * Half-configured draws are how a GPU hangs, and a hang costs a whole run
 * plus a console reboot, so each missing piece is counted and the draw is
 * skipped. The counters say which piece, which is the difference between
 * knowing what to fix and running it again to find out.
 *
 * Two things about guest data are not optional:
 *
 * **Vertex data is big-endian.** It was written by PowerPC code and the
 * Switch's GPU reads little-endian, so every 32-bit component is swapped on
 * upload. This is safe only because every attribute format this game uses is
 * 32-bit components (FLOAT_32 through FLOAT_32_32_32_32, confirmed by
 * FETCHATTR across both of its attribute sets); a 16-bit or packed format
 * would need a different swap width, so one appearing skips the draw rather
 * than quietly uploading garbage.
 *
 * **Uniforms are a register file, not a buffer.** GX2SetVertexUniformReg
 * writes u32 words at an offset, measured on hardware (UNIFREG: offset 0
 * count 16 is a 4x4 matrix). The shadow below is that register file, and it
 * is uploaded whole, so constant index n is at uf[n] in the generated GLSL.
 */

#include "ppc_runtime.h"

#ifdef ARKCHEMY_HOSTTEST
/* Link dkCmdBufDraw without ever calling it.
 *
 * The padding control settled that this is not about code size: 752 bytes of
 * dead code in the same function booted fine, where dkCmdBufDraw's 312 kills
 * it. So it is that symbol. Two ways that can be true, and this separates
 * them:
 *
 *   - the presence of deko3d's draw code in the binary is itself the problem,
 *     in which case taking its address is enough to reproduce;
 *   - or it is the call site -- what the compiler does to ark_draw_ex when it
 *     has to set up and make that call -- in which case this boots fine.
 *
 * volatile so the reference cannot be optimised away, and nothing ever reads
 * it back. */
static inline void ark_draw_set_attrib_buffer(uint32_t i, uint32_t size,
                                              uint32_t stride, uint32_t addr)
{ (void)i; (void)size; (void)stride; (void)addr; }
static inline void ark_draw_note_fetch(PpcContext *ctx, uint32_t fs,
                                       uint32_t attribs, uint32_t count)
{ (void)ctx; (void)fs; (void)attribs; (void)count; }
static inline void ark_draw_bind_fetch(uint32_t fs) { (void)fs; }
static inline void ark_draw_uniform(PpcContext *ctx, int kind, uint32_t off,
                                    uint32_t count, uint32_t data)
{ (void)ctx; (void)kind; (void)off; (void)count; (void)data; }
static inline void ark_draw_ex(PpcContext *ctx, uint32_t mode, uint32_t count,
                               uint32_t offset, uint32_t instances)
{ (void)ctx; (void)mode; (void)count; (void)offset; (void)instances; }
#else

#include <deko3d.h>
#include <string.h>

#define ARK_VB_SLOTS      16u
#define ARK_FS_OBJECTS     8u
#define ARK_UNIF_WORDS  1024u    /* 256 vec4, the whole GX2 constant file */
#define ARK_MAX_ATTRIBS    8u
#define ARK_DRAW_QUEUE    64u    /* draws queued between two submits */

/* -- recorded GX2 state --------------------------------------------------- */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_vb_addr[ARK_VB_SLOTS], g_ark_vb_size[ARK_VB_SLOTS],
                  g_ark_vb_stride[ARK_VB_SLOTS];
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fsobj[ARK_FS_OBJECTS], g_ark_fsobj_count[ARK_FS_OBJECTS],
                  g_ark_fsobj_n = 0;
/* The attribute streams themselves, copied rather than pointed at.
 *
 * GX2InitFetchShaderEx receives a GX2AttribStream array that callers build on
 * the stack -- FETCHATTR reported them at 0x3ffff518 and 0x3ffff548, right at
 * the top of a guest stack. Keeping the pointer and reading it at draw time
 * reads whatever occupies that frame by then: on 2026-09-13 that rejected 483
 * of 485 draws as unsupported attribute formats, while the two that survived
 * were the ones issued before the frame was reused.
 *
 * Six fields per attribute: location, buffer, offset, format, index type,
 * divisor. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fsobj_attr[ARK_FS_OBJECTS][ARK_MAX_ATTRIBS][6];
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fs_cur = 0xFFFFFFFFu, g_ark_fs_cur_count = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_unif_file[2][ARK_UNIF_WORDS];

/* -- why a draw did not happen -------------------------------------------- */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_draw_tried = 0, g_ark_draw_done = 0,
                  g_ark_draw_noshader = 0, g_ark_draw_nofetch = 0,
                  g_ark_draw_nobuf = 0, g_ark_draw_badfmt = 0,
                  g_ark_draw_badprim = 0, g_ark_draw_nomem = 0;

/* -- GPU-side staging ------------------------------------------------------ */
#ifdef __GNUC__
__attribute__((weak))
#endif
DkMemBlock g_ark_vb_block[ARK_VB_SLOTS];
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_vb_block_size[ARK_VB_SLOTS];
#ifdef __GNUC__
__attribute__((weak))
#endif
DkMemBlock g_ark_unif_block;

/* Draws waiting to be recorded: primitive, count, instances, first vertex. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_dq[ARK_DRAW_QUEUE][4], g_ark_dq_n = 0,
                  g_ark_dq_lost = 0, g_ark_dq_recorded = 0;

/* Drained where the command buffer is submitted. Declared here, defined in
 * the same single translation unit as the draw body. */

#if ARK_REF_DRAW_ONLY
#ifdef __GNUC__
__attribute__((weak))
#endif
void *volatile g_ark_draw_ref;
#endif


static inline void ark_draw_set_attrib_buffer(uint32_t i, uint32_t size,
                                              uint32_t stride, uint32_t addr)
{
    if (i >= ARK_VB_SLOTS) return;
    g_ark_vb_addr[i] = addr;
    g_ark_vb_size[i] = size;
    g_ark_vb_stride[i] = stride;
}

static inline void ark_draw_note_fetch(PpcContext *ctx, uint32_t fs,
                                      uint32_t attribs, uint32_t count)
{
    if (!fs || !attribs) return;
    uint32_t slot = ARK_FS_OBJECTS;
    for (uint32_t i = 0; i < g_ark_fsobj_n; i++)
        if (g_ark_fsobj[i] == fs) { slot = i; break; }
    if (slot == ARK_FS_OBJECTS) {
        if (g_ark_fsobj_n >= ARK_FS_OBJECTS) return;
        slot = g_ark_fsobj_n++;
    }
    if (count > ARK_MAX_ATTRIBS) count = ARK_MAX_ATTRIBS;
    g_ark_fsobj[slot] = fs;
    g_ark_fsobj_count[slot] = count;
    /* Read now, while the caller's array is still alive. */
    for (uint32_t a = 0; a < count; a++)
        for (uint32_t w = 0; w < 6u; w++)
            g_ark_fsobj_attr[slot][a][w] =
                ppc_load_u32(ctx, attribs + a * 32u + w * 4u);
}

static inline void ark_draw_bind_fetch(uint32_t fs)
{
    for (uint32_t i = 0; i < g_ark_fsobj_n; i++)
        if (g_ark_fsobj[i] == fs) {
            g_ark_fs_cur = i;
            g_ark_fs_cur_count = g_ark_fsobj_count[i];
            return;
        }
    g_ark_fs_cur = 0xFFFFFFFFu;
    g_ark_fs_cur_count = 0;
}

static inline void ark_draw_uniform(PpcContext *ctx, int kind, uint32_t off,
                                    uint32_t count, uint32_t data)
{
    if (!data || off >= ARK_UNIF_WORDS) return;
    if (count > ARK_UNIF_WORDS - off) count = ARK_UNIF_WORDS - off;
    /* ppc_load_u32 already returns the value, not the bytes, so the shadow
     * holds host-order words and needs no further swapping on upload. */
    for (uint32_t i = 0; i < count; i++)
        g_ark_unif_file[kind ? 1 : 0][off + i] = ppc_load_u32(ctx, data + i * 4u);
}

/* GX2AttribFormat -> deko3d. Only the 32-bit float formats, which is every
 * format FETCHATTR saw. Anything else returns false and the draw is skipped:
 * the byte-swap below assumes 32-bit components. */
static inline bool ark_draw_format(uint32_t fmt, DkVtxAttribSize *size,
                                   DkVtxAttribType *type)
{
    *type = DkVtxAttribType_Float;
    switch (fmt) {
        case 0x806: *size = DkVtxAttribSize_1x32; return true;
        case 0x80d: *size = DkVtxAttribSize_2x32; return true;
        case 0x811: *size = DkVtxAttribSize_3x32; return true;
        case 0x813: *size = DkVtxAttribSize_4x32; return true;
        default: return false;
    }
}

/* A control for the draw-call experiment.
 *
 * Linking dkCmdBufDraw adds exactly one symbol and exactly 312 bytes of text,
 * no data, no bss -- and the boot dies before the engine ever calls it. That
 * is not a symptom of pulling in a driver; it is a symptom of moving
 * everything after it by 312 bytes. Every failure has also resolved the wrong
 * memory manager (POOL28 mgr=0x45f3964, the SETPOOLS MISMATCH pointer), which
 * is a race this project has known about for days.
 *
 * So: the same growth, in the same function, without the draw. If the boot
 * still dies, the draw code is innocent and there is a latent bug that any
 * change of the right size can trip -- which matters far more than graphics.
 * If it survives, something about dkCmdBufDraw specifically is at fault.
 *
 * Referenced from a branch that cannot be taken (no GX2 primitive mode is
 * 0xDEADBEEF) but that the compiler cannot fold away, so the bytes are linked
 * and never executed -- exactly the position dkCmdBufDraw was in. */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static void ark_pad_fn(void)
{
    __asm__ volatile (".space 300");
}

static inline bool ark_draw_primitive(uint32_t mode, DkPrimitive *prim)
{
    switch (mode) {
        case 1:  *prim = DkPrimitive_Points;        return true;
        case 2:  *prim = DkPrimitive_Lines;         return true;
        case 3:  *prim = DkPrimitive_LineStrip;     return true;
        case 4:  *prim = DkPrimitive_Triangles;     return true;
        case 5:  *prim = DkPrimitive_TriangleFan;   return true;
        case 6:  *prim = DkPrimitive_TriangleStrip; return true;
        default: return false;      /* quads and the strip forms, unhandled */
    }
}

/* A GPU-visible block of at least `need` bytes for slot `i`, grown rather
 * than reallocated per draw -- the same buffers come back every frame. */
static inline void *ark_draw_vb_memory(uint32_t i, uint32_t need)
{
    uint32_t rounded = (need + DK_MEMBLOCK_ALIGNMENT - 1u)
                       & ~(DK_MEMBLOCK_ALIGNMENT - 1u);
    if (g_ark_vb_block[i] && g_ark_vb_block_size[i] >= rounded)
        return dkMemBlockGetCpuAddr(g_ark_vb_block[i]);
    if (g_ark_vb_block[i]) {
        dkMemBlockDestroy(g_ark_vb_block[i]);
        g_ark_vb_block[i] = NULL;
        g_ark_vb_block_size[i] = 0;
    }
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, g_arkchemy_gx2.device, rounded);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_ark_vb_block[i] = dkMemBlockCreate(&maker);
    if (!g_ark_vb_block[i]) return NULL;
    g_ark_vb_block_size[i] = rounded;
    return dkMemBlockGetCpuAddr(g_ark_vb_block[i]);
}

/* NOT inline, and that is the whole point.
 *
 * 2026-09-13. With this function inlined, three runs in a row bound two or
 * three shaders instead of seven, issued zero draws, and died before the
 * frame cap; four runs without it completed normally with ~500 draws. The
 * confusing part was that the engine never called GX2DrawEx in any of the
 * failures, so the code doing the damage never executed.
 *
 * Inlining is how code that never runs still costs something. The locals
 * below -- DkVtxAttribState[8] and DkVtxBufferState[16] -- become part of the
 * *caller's* stack frame the moment this is inlined into the GX2DrawEx shim,
 * and that frame is then paid on every call through it, drawing or not. The
 * recompiled game is built at -O0, where frames are fat already, and
 * THREADSTACK reports threads running on 64KB stacks. A few hundred extra
 * bytes per frame down a deep guest call chain is a smashed stack, and a
 * smashed stack is exactly what the failures looked like: the wrong memory
 * manager resolved (POOL28 mgr=0x45f3964, the SETPOOLS MISMATCH pointer),
 * the boot taking its bad branch, and the run ending early.
 *
 * Keeping it out of line gives it its own frame, paid only when a draw
 * actually happens. */
/* Defined in exactly one translation unit.
 *
 * This header is included by every one of the 200-odd files of recompiled
 * game code, so anything defined here is compiled into all of them. A call to
 * dkCmdBufDraw in this body stops the game booting -- without ever executing,
 * and identically whether the call is direct or through a volatile pointer,
 * while merely linking the symbol is harmless. What has not been tested is
 * that call existing in one file rather than in every file, which is also
 * simply the right way to arrange it.
 *
 * ARKCHEMY_GX2_DRAW_IMPL is defined by that one file; everywhere else this is
 * a declaration and nothing more. */
void ark_draw_ex(PpcContext *ctx, uint32_t mode, uint32_t count,
                 uint32_t offset, uint32_t instances);

#ifdef ARKCHEMY_GX2_DRAW_IMPL
#ifdef __GNUC__
__attribute__((noinline))
#endif
void ark_draw_ex(PpcContext *ctx, uint32_t mode, uint32_t count,
                 uint32_t offset, uint32_t instances)
{
#if !ARKCHEMY_GX2_DRAW_BODY
    /* Body switched off, 2026-09-13. The failures happen without this function
     * ever being called -- the engine issues zero GX2DrawEx in every failing
     * run -- so the body cannot be what breaks them, and this build proves it
     * one way or the other. Call site present, function present, does nothing.
     * Still failing means the damage is done by compiling the code, not by
     * running it, which is a layout or codegen effect and wants a different
     * kind of hunt entirely. */
    (void)ctx; (void)mode; (void)count; (void)offset; (void)instances;
    return;
#else
    if (!g_arkchemy_gx2.initialized || !g_arkchemy_gx2.device
        || !g_arkchemy_gx2.cmdbuf) return;
    g_ark_draw_tried++;

    if (!g_ark_shdmod_cur[0] || !g_ark_shdmod_cur[1]) { g_ark_draw_noshader++; return; }
    if (g_ark_fs_cur >= ARK_FS_OBJECTS || !g_ark_fs_cur_count) { g_ark_draw_nofetch++; return; }

    DkPrimitive prim;
    if (!ark_draw_primitive(mode, &prim)) { g_ark_draw_badprim++; return; }
#if ARK_PAD_TEST
    if (mode == 0xDEADBEEFu) ark_pad_fn();
#endif
#if ARK_CALL_UNUSED
    /* A deko3d function nothing else calls, in an unreachable branch.
     *
     * Calling dkCmdBufDraw breaks the boot from anywhere; calling
     * dkCmdBufBindVtxBuffer -- already called several times a draw -- does
     * not; taking dkCmdBufDraw's address without calling it does not. The
     * difference between those cases is not the function, it is whether the
     * binary already contains a call to it: at 176MB of text a branch beyond
     * AArch64's +/-128MB range needs a linker-inserted veneer, a new call
     * target needs a new veneer, and an address-of needs none at all.
     *
     * dkCmdBufDrawIndexed is a near neighbour of dkCmdBufDraw that nothing in
     * this project calls. If it breaks the boot too, the problem is adding a
     * new call target, not that particular function -- and the search moves
     * to the linker's layout rather than to deko3d. */
    if (mode == 0xDEADBEEFu)
        dkCmdBufDrawIndexed(g_arkchemy_gx2.cmdbuf, DkPrimitive_Triangles, 0, 0, 0, 0, 0);
#endif
#if ARK_CALL_OTHER
    /* One more call, to a function already linked and already called.
     *
     * dkCmdBufDraw breaks the boot; linking it without calling does not; dead
     * code of the same size does not; direct and indirect calls behave the
     * same; one translation unit or two hundred behaves the same. The thing
     * never tested is whether it has to be *that* function at all.
     *
     * dkCmdBufBindVtxBuffer is already linked and already called several
     * times a draw, so this adds no symbol and no new deko3d code -- only one
     * more call site, in the same unreachable branch. If the boot breaks, the
     * problem is adding a call here, and dkCmdBufDraw was never special. If
     * it survives, that function really is the culprit. */
    if (mode == 0xDEADBEEFu)
        dkCmdBufBindVtxBuffer(g_arkchemy_gx2.cmdbuf, 0, 0, 0);
#endif
#if ARK_REF_DRAW_ONLY
    /* Taken here rather than at file scope: an unreferenced global is exactly
     * what --gc-sections exists to remove, and removing it takes the
     * reference -- and therefore the symbol under test -- with it. This sits
     * in a function the shim calls, so it survives; the branch never runs. */
    if (mode == 0xDEADBEEFu) g_ark_draw_ref = (void *)&dkCmdBufDraw;
#endif

    uint32_t n = g_ark_fs_cur_count;
    if (n > ARK_MAX_ATTRIBS) n = ARK_MAX_ATTRIBS;
    uint32_t attrib_n = 0;      /* highest location + 1, not the stream count */

    DkVtxAttribState attribs[ARK_MAX_ATTRIBS];
    DkVtxBufferState buffers[ARK_VB_SLOTS];
    memset(attribs, 0, sizeof(attribs));
    memset(buffers, 0, sizeof(buffers));
    uint32_t used_mask = 0, highest = 0;

    for (uint32_t a = 0; a < n; a++) {
        volatile uint32_t *st = g_ark_fsobj_attr[g_ark_fs_cur][a];
        uint32_t loc = st[0], buf = st[1], off = st[2];
        uint32_t fmt = st[3], type = st[4], div = st[5];
        if (loc >= ARK_MAX_ATTRIBS || buf >= ARK_VB_SLOTS) { g_ark_draw_badfmt++; return; }

        DkVtxAttribSize asize; DkVtxAttribType atype;
        if (!ark_draw_format(fmt, &asize, &atype)) { g_ark_draw_badfmt++; return; }
        if (!g_ark_vb_addr[buf] || !g_ark_vb_size[buf]) { g_ark_draw_nobuf++; return; }

        attribs[loc].bufferId = buf;
        attribs[loc].isFixed  = 0;
        attribs[loc].offset   = off;
        attribs[loc].size     = asize;
        attribs[loc].type     = atype;
        attribs[loc].isBgra   = 0;

        buffers[buf].stride = g_ark_vb_stride[buf];
        /* GX2AttribIndexType 1 is per-instance; aluDivisor is the rate. */
        buffers[buf].divisor = type ? (div ? div : 1u) : 0u;
        used_mask |= 1u << buf;
        if (buf > highest) highest = buf;
        if (loc + 1u > attrib_n) attrib_n = loc + 1u;
    }

    /* Upload every buffer the attributes name, byte-swapping as it goes --
     * PowerPC wrote these and the GPU reads the other way round. */
    for (uint32_t b = 0; b <= highest; b++) {
        if (!(used_mask & (1u << b))) continue;
        uint32_t size = g_ark_vb_size[b];
        if (size > 0x400000u) { g_ark_draw_nomem++; return; }
        uint32_t *dst = (uint32_t *)ark_draw_vb_memory(b, size);
        if (!dst) { g_ark_draw_nomem++; return; }
        uint32_t words = size / 4u;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = ppc_load_u32(ctx, g_ark_vb_addr[b] + w * 4u);
        #if ARK_REC_VTXBUF
        dkCmdBufBindVtxBuffer(g_arkchemy_gx2.cmdbuf, b,
                              dkMemBlockGetGpuAddr(g_ark_vb_block[b]), size);
#endif
    }

    /* The constant file, both stages, bound at 0 -- which is where the
     * generated GLSL declares its Constants block. */
    if (!g_ark_unif_block) {
        DkMemBlockMaker maker;
        dkMemBlockMakerDefaults(&maker, g_arkchemy_gx2.device,
                                2u * ARK_UNIF_WORDS * 4u);
        maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        g_ark_unif_block = dkMemBlockCreate(&maker);
        if (!g_ark_unif_block) { g_ark_draw_nomem++; return; }
    }
    uint32_t *ubo = (uint32_t *)dkMemBlockGetCpuAddr(g_ark_unif_block);
    for (uint32_t i = 0; i < ARK_UNIF_WORDS; i++) {
        ubo[i] = g_ark_unif_file[0][i];
        ubo[ARK_UNIF_WORDS + i] = g_ark_unif_file[1][i];
    }
    DkGpuAddr ubo_addr = dkMemBlockGetGpuAddr(g_ark_unif_block);
    #if ARK_REC_UBO
    dkCmdBufBindUniformBuffer(g_arkchemy_gx2.cmdbuf, DkStage_Vertex, 0,
                              ubo_addr, ARK_UNIF_WORDS * 4u);
#endif
    #if ARK_REC_UBO
    dkCmdBufBindUniformBuffer(g_arkchemy_gx2.cmdbuf, DkStage_Fragment, 0,
                              ubo_addr + ARK_UNIF_WORDS * 4u,
                              ARK_UNIF_WORDS * 4u);
#endif

    /* Re-bind the descriptor sets, every draw.
     *
     * These are recorded COMMANDS, not persistent device state. GX2SetPixelTexture
     * and arkchemy_gx2_set_sampler record them, and GX2SwapScanBuffers'
     * dkCmdBufClear wipes every recorded command at the frame boundary. The game
     * sets textures four times in a whole run, so any draw in a later frame ran
     * with no descriptor set bound at all -- and a shader that samples a texture
     * with no descriptor set then reads its descriptor from address 0.
     *
     * That is the "GPU page fault / Address: 0x0000000000 / Access type: Read"
     * of 2026-09-14, which survived moving the draw back inline because the
     * deferral was never what removed these.
     *
     * The descriptor memory itself is fine: the addresses are taken once in
     * GX2Init and the contents are written by dkCmdBufPushData in earlier,
     * already-submitted frames. It is only the binding that has to be redone,
     * and it is two commands pointing at memory that never moves. */
    if (g_arkchemy_gx2.texture_descriptor_gpu_addr)
        dkCmdBufBindImageDescriptorSet(g_arkchemy_gx2.cmdbuf,
                                       g_arkchemy_gx2.texture_descriptor_gpu_addr,
                                       ARKCHEMY_GX2_NUM_SAMPLER_DESCRIPTORS);
    if (g_arkchemy_gx2.sampler_descriptor_gpu_addr)
        dkCmdBufBindSamplerDescriptorSet(g_arkchemy_gx2.cmdbuf,
                                         g_arkchemy_gx2.sampler_descriptor_gpu_addr,
                                         ARKCHEMY_GX2_NUM_SAMPLER_DESCRIPTORS);

    const DkShader *shaders[2] = {
        &g_ark_shdmod[g_ark_shdmod_cur[0] - 1u].shader,
        &g_ark_shdmod[g_ark_shdmod_cur[1] - 1u].shader,
    };
    #if ARK_REC_SHADERS
    dkCmdBufBindShaders(g_arkchemy_gx2.cmdbuf,
                        DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
#endif
    #if ARK_REC_ATTRIBSTATE
    dkCmdBufBindVtxAttribState(g_arkchemy_gx2.cmdbuf, attribs, attrib_n);
#endif
    #if ARK_REC_BUFSTATE
    dkCmdBufBindVtxBufferState(g_arkchemy_gx2.cmdbuf, buffers, highest + 1u);
#endif
    #if ARK_REC_DRAW_DEFERRED
    /* Queue the draw instead of recording it here.
     *
     * Calling dkCmdBufDraw from this function stops the game booting, and six
     * builds narrowed that as far as it goes: linking the symbol without
     * calling it is fine, dead code of the same size is fine, direct and
     * indirect calls behave alike, one translation unit behaves like two
     * hundred, and an extra call to a *different* already-linked deko3d
     * function is fine. It is that call, from here, and the mechanism is
     * still unknown.
     *
     * So it moves. The parameters go in a queue and the draws are recorded
     * where the command buffer is submitted -- which is where they belong in
     * any case: everything else this function binds is state, and state is
     * cheap to rebind, while a draw is the thing that has to sit in the right
     * place in the stream. */
    if (g_ark_dq_n < ARK_DRAW_QUEUE) {
        uint32_t q = g_ark_dq_n++;
        g_ark_dq[q][0] = (uint32_t)prim;
        g_ark_dq[q][1] = count;
        g_ark_dq[q][2] = instances ? instances : 1u;
        g_ark_dq[q][3] = offset;
    }
    /* No increment here. There is an unconditional one below that fires in
     * every configuration, and counting in both places double-counted every
     * draw -- which is why a run reported `tried=2 drawn=4`, a ratio that
     * cannot happen, drawn being a subset of tried by construction. Every
     * draw figure recorded before 2026-09-14 is therefore exactly twice the
     * truth: the runs reported as 489 and 513 draws really made 245 and 257. */
#endif
#if ARK_REC_DRAW
    /* Called through a volatile pointer, not directly.
     *
     * A direct call here stops the game booting -- and it does so without ever
     * running, since the engine never reaches its first draw in a build that
     * has one. Linking the symbol alone is harmless (a build that takes its
     * address and never calls it boots fine, 523 draws), and 752 bytes of dead
     * code in this same function is harmless too, so it is neither the symbol
     * nor the size. It is what the compiler does to this function when it has
     * to emit that particular call.
     *
     * An indirect call is emitted differently: no direct branch to that
     * address, no relocation against it, and the compiler cannot reason about
     * the callee at all. If the boot survives this, the draw path works and
     * the underlying cause can be found without blocking a picture on it. */
    static void (*volatile draw_fn)(DkCmdBuf, DkPrimitive, uint32_t, uint32_t,
                                    uint32_t, uint32_t) = dkCmdBufDraw;
    draw_fn(g_arkchemy_gx2.cmdbuf, prim, count,
            instances ? instances : 1u, offset, 0);
#endif
    g_ark_draw_done++;
#endif
}

#endif /* ARKCHEMY_GX2_DRAW_IMPL */

#endif /* ARKCHEMY_HOSTTEST */
#endif /* ARKCHEMY_CAFEOS_GX2_DRAW_H */
