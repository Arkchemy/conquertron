#ifndef ARKCHEMY_CAFEOS_COREINIT_THREAD_H
#define ARKCHEMY_CAFEOS_COREINIT_THREAD_H

#include "ppc_runtime.h"

#include <pthread.h>
#include <string.h>

/*
 * Phase 1d CafeOS runtime shim -- coreinit real threading
 * (OSCreateThread and the OSThread-identity family). The last of the
 * four original architectural blockers this project's docs called
 * "a fundamentally different, much larger problem than anything in this
 * shim so far" -- genuine concurrent execution of more than one
 * recompiled function at a time. Only attempted once both real
 * prerequisites existed and were independently verified: shared memory
 * across PpcContexts (ppc_runtime.h's `PpcSharedMemory`) and real,
 * pthread-backed synchronization primitives (cafeos_coreinit_sync.h) --
 * without the second one, real threads here would just be silent data
 * races waiting to happen the moment a game used OSLockMutex to protect
 * shared state between them.
 *
 * Real signatures confirmed against devkitPro/wut's coreinit/thread.h.
 * `OSThread` stays an opaque, caller-allocated struct (same as OSMutex/
 * OSEvent) -- never written through -- backed by real host-side state
 * in a table keyed by the real guest `OSThread*` address, same pattern
 * as every other real-state-needed shim in this project.
 *
 * A real OSCreateThread spawns a genuine host pthread running the real
 * entry function (via `ppc_dispatch`, same reverse-call mechanism
 * cafeos_coreinit_fs.h's FS*Async and cafeos_nsyshid.h already
 * established) against a *fresh* PpcContext that shares the *same*
 * `PpcSharedMemory` as its creator -- real Wii U threads all share one
 * address space, and this now genuinely does too.
 *
 * Real, confirmed-via-Cemu-source detail that matters: a newly created
 * thread does **not** start running immediately -- Cemu's real HLE
 * (src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp,
 * `__OSCreateThreadInternal2`) sets `suspendCounter = 1` at creation, so
 * the thread stays suspended until exactly one real `OSResumeThread`
 * call decrements it to 0. This shim defers the actual `pthread_create`
 * call until that happens, for the same reason real hardware requires
 * it: some real games set up more thread state between Create and
 * Resume before letting it run.
 *
 * `OSGetThreadSpecific`/`OSSetThreadSpecific` operate on *the calling
 * thread*, not an explicit `OSThread*` argument (matching real
 * hardware's signature exactly -- there's no thread parameter) -- uses
 * a `pthread_key_t` to identify which real host thread is asking, then
 * looks up (or lazily creates, for the original/main thread that was
 * never itself created via this shim's OSCreateThread) that thread's
 * entry in the same table. A lazily-created entry's "address" is a
 * synthetic per-thread sentinel (a thread-local variable's own host
 * address), not a real guest address -- safe because, like every other
 * opaque `OSThread*` in this shim, nothing ever dereferences it as
 * guest memory, only passes it back into these same functions.
 */

#define ARKCHEMY_THREAD_TABLE_SIZE 32
#define ARKCHEMY_THREAD_SPECIFIC_SLOTS 14 /* OS_THREAD_SPECIFIC_0..13; 14/15 are wut-reserved */

typedef struct {
    uint32_t guest_addr;
    int active;
    pthread_t pthread;
    int started;   /* pthread_create has actually been called */
    int joined;
    int detached;
    int32_t suspend_count;
    int32_t priority;
    int32_t exit_value;
    uint32_t specific[ARKCHEMY_THREAD_SPECIFIC_SLOTS];
    /* Stashed at OSCreateThread time, consumed when suspend_count first reaches 0. */
    uint32_t entry_addr;
    int32_t argc;
    uint32_t argv;
    uint32_t stack_top;
    PpcSharedMemory *shared;
} ArkchemyThreadEntry;

/* Real definitions in cafeos_state.c -- see its own file comment. */
extern ArkchemyThreadEntry g_arkchemy_threads[ARKCHEMY_THREAD_TABLE_SIZE];
extern pthread_mutex_t g_arkchemy_thread_table_lock;
extern pthread_key_t g_arkchemy_current_thread_key;
extern pthread_once_t g_arkchemy_thread_tls_once;

static inline void arkchemy_thread_tls_init(void) { pthread_key_create(&g_arkchemy_current_thread_key, NULL); }

/* Finds an existing entry, or (if create_if_missing) lazily allocates a
 * fresh, real-but-never-Created one -- the same defensive pattern used
 * throughout this project's other host-side tables. A lazily-created
 * entry starts already "resumed" (suspend_count=0, started=1 but with
 * no real pthread of its own -- it represents a real host thread that
 * already exists by other means, e.g. the original entry thread). */
static inline ArkchemyThreadEntry *arkchemy_thread_get(uint32_t addr, int create_if_missing) {
    int i, free_slot = -1;
    ArkchemyThreadEntry *result = NULL;
    pthread_mutex_lock(&g_arkchemy_thread_table_lock);
    for (i = 0; i < ARKCHEMY_THREAD_TABLE_SIZE; i++) {
        if (g_arkchemy_threads[i].active && g_arkchemy_threads[i].guest_addr == addr) {
            result = &g_arkchemy_threads[i];
            break;
        }
        if (free_slot < 0 && !g_arkchemy_threads[i].active) free_slot = i;
    }
    if (result == NULL && create_if_missing && free_slot >= 0) {
        ArkchemyThreadEntry *e = &g_arkchemy_threads[free_slot];
        memset(e, 0, sizeof(*e));
        e->guest_addr = addr;
        e->active = 1;
        e->started = 1;
        e->suspend_count = 0;
        e->priority = 16; /* real Cafe OS default/mid-range priority (0=highest, 31=lowest) */
        result = e;
    }
    pthread_mutex_unlock(&g_arkchemy_thread_table_lock);
    return result;
}

static inline uint32_t arkchemy_current_thread_addr(void) {
    pthread_once(&g_arkchemy_thread_tls_once, arkchemy_thread_tls_init);
    void *v = pthread_getspecific(g_arkchemy_current_thread_key);
    if (v == NULL) {
        static __thread uint32_t sentinel_storage;
        uint32_t sentinel = (uint32_t)(uintptr_t)&sentinel_storage;
        pthread_setspecific(g_arkchemy_current_thread_key, (void *)(uintptr_t)sentinel);
        (void)arkchemy_thread_get(sentinel, 1); /* register so lookups by this "address" succeed */
        v = (void *)(uintptr_t)sentinel;
    }
    return (uint32_t)(uintptr_t)v;
}

static inline void *arkchemy_thread_trampoline(void *arg) {
    ArkchemyThreadEntry *te = (ArkchemyThreadEntry *)arg;
    pthread_once(&g_arkchemy_thread_tls_once, arkchemy_thread_tls_init);
    pthread_setspecific(g_arkchemy_current_thread_key, (void *)(uintptr_t)te->guest_addr);

    PpcContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shared = te->shared;
    ctx.r[1] = te->stack_top;
    ctx.r[3] = (uint32_t)te->argc;
    ctx.r[4] = te->argv;
    ppc_dispatch(&ctx, te->entry_addr);
    te->exit_value = (int32_t)ctx.r[3]; /* real entry point's return value is the thread's exit value */
    return NULL;
}

static inline void ppc_import_coreinit_OSCreateThread(PpcContext *ctx) {
    /* BOOL OSCreateThread(OSThread *thread, OSThreadEntryPointFn entry,
     *   int32_t argc, char *argv, void *stack, uint32_t stackSize,
     *   int32_t priority, OSThreadAttributes attributes) */
    uint32_t thread_addr = ctx->r[3];
    ArkchemyThreadEntry *te = arkchemy_thread_get(thread_addr, 1);
    if (te == NULL) { ctx->r[3] = 0; return; } /* table exhausted */
    te->entry_addr = ctx->r[4];
    te->argc = (int32_t)ctx->r[5];
    te->argv = ctx->r[6];
    te->stack_top = ctx->r[7];
    /* r8 (stackSize) not separately tracked -- the caller's own stack_top
     * already lives in the shared address space it owns; nothing here
     * needs to carve out or bounds-check that region itself. */
    te->priority = (int32_t)ctx->r[9];
    te->started = 0;
    te->joined = 0;
    te->detached = 0;
    te->suspend_count = 1; /* real: created suspended, confirmed via Cemu's __OSCreateThreadInternal2 */
    te->shared = ctx->shared; /* genuine shared memory with the creating thread */
    ctx->r[3] = 1; /* BOOL TRUE */
}

static inline void ppc_import_coreinit_OSResumeThread(PpcContext *ctx) {
    /* int32_t OSResumeThread(OSThread *thread) -- returns the PREVIOUS suspend counter */
    ArkchemyThreadEntry *te = arkchemy_thread_get(ctx->r[3], 1);
    int32_t prev = te->suspend_count;
    if (te->suspend_count > 0) te->suspend_count--;
    if (te->suspend_count == 0 && !te->started) {
        te->started = 1;
        pthread_create(&te->pthread, NULL, arkchemy_thread_trampoline, te);
    }
    ctx->r[3] = (uint32_t)prev;
}

static inline void ppc_import_coreinit_OSJoinThread(PpcContext *ctx) {
    /* BOOL OSJoinThread(OSThread *thread, int *threadResult) */
    ArkchemyThreadEntry *te = arkchemy_thread_get(ctx->r[3], 0);
    if (te == NULL || !te->started || te->detached) { ctx->r[3] = 0; return; }
    if (!te->joined) {
        pthread_join(te->pthread, NULL);
        te->joined = 1;
    }
    if (ctx->r[4] != 0) ppc_store_u32(ctx, ctx->r[4], (uint32_t)te->exit_value);
    ctx->r[3] = 1;
}

static inline void ppc_import_coreinit_OSDetachThread(PpcContext *ctx) {
    ArkchemyThreadEntry *te = arkchemy_thread_get(ctx->r[3], 0);
    if (te == NULL || te->detached) return;
    te->detached = 1;
    if (te->started) pthread_detach(te->pthread);
}

static inline void ppc_import_coreinit_OSGetCurrentThread(PpcContext *ctx) {
    ctx->r[3] = arkchemy_current_thread_addr();
}

static inline void ppc_import_coreinit_OSGetThreadPriority(PpcContext *ctx) {
    ArkchemyThreadEntry *te = arkchemy_thread_get(ctx->r[3], 1);
    ctx->r[3] = (uint32_t)te->priority;
}

static inline void ppc_import_coreinit_OSSetThreadPriority(PpcContext *ctx) {
    /* BOOL OSSetThreadPriority(OSThread *thread, int32_t priority) */
    ArkchemyThreadEntry *te = arkchemy_thread_get(ctx->r[3], 1);
    te->priority = (int32_t)ctx->r[4];
    ctx->r[3] = 1; /* BOOL TRUE -- real host OS scheduler handles real priority, this is bookkeeping only */
}

static inline void ppc_import_coreinit_OSSetThreadName(PpcContext *ctx) {
    /* void OSSetThreadName(OSThread *thread, const char *name) -- accepted,
     * not stored: nothing in this shim's real import list (OSGetThreadName
     * isn't called by the real binary) ever reads it back. */
    (void)ctx;
}

static inline void ppc_import_coreinit_OSSetThreadAffinity(PpcContext *ctx) {
    /* BOOL OSSetThreadAffinity(OSThread *thread, uint32_t affinity) -- real
     * hardware pins a thread to specific cores; this runtime has no
     * per-core scheduling model (the real host OS scheduler places real
     * threads), so accepted and reported successful without a real
     * effect, same "config nothing reads back" reasoning already used
     * for e.g. cafeos_snd_core.h's AX device functions. */
    (void)ctx;
    ctx->r[3] = 1;
}

static inline void ppc_import_coreinit_OSGetThreadSpecific(PpcContext *ctx) {
    /* void *OSGetThreadSpecific(OSThreadSpecificID id) -- operates on the
     * *current* thread; no explicit OSThread* argument in the real
     * signature. */
    uint32_t id = ctx->r[3];
    ArkchemyThreadEntry *te = arkchemy_thread_get(arkchemy_current_thread_addr(), 1);
    ctx->r[3] = (id < ARKCHEMY_THREAD_SPECIFIC_SLOTS) ? te->specific[id] : 0;
}

static inline void ppc_import_coreinit_OSSetThreadSpecific(PpcContext *ctx) {
    /* void OSSetThreadSpecific(OSThreadSpecificID id, void *value) */
    uint32_t id = ctx->r[3];
    uint32_t value = ctx->r[4];
    ArkchemyThreadEntry *te = arkchemy_thread_get(arkchemy_current_thread_addr(), 1);
    if (id < ARKCHEMY_THREAD_SPECIFIC_SLOTS) te->specific[id] = value;
}

static inline void ppc_import_coreinit_OSBlockThreadsOnExit(PpcContext *ctx) {
    /* Lower confidence: a real confirmed export (present in the actual
     * binary's import list), but no confirmed prototype found in either
     * wut's public headers or Cemu's HLE (unimplemented there too).
     * Real intent, per its name, is almost certainly a process-shutdown
     * safety mechanism (prevent threads from exiting mid-teardown) --
     * this shim never runs a real teardown sequence with other threads
     * racing it, so a safe no-op is defensible regardless of the exact
     * unconfirmed argument list, same caveat already used for
     * cafeos_snd_core.h's AXSetMaxVoices/AXRegisterExceedCallback. */
    (void)ctx;
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_THREAD_H */
