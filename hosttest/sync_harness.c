/* Does the event/pump machinery deadlock, and if so where?
 *
 * The game thread's last recompiled function has been igCafeSignal::raise for
 * three runs running, at an identical GAMEPC count across three different
 * builds. raise tail-calls OSSignalEvent. Everything that could go wrong from
 * there is OUR code -- an event table, two pthread mutexes, a condvar and a
 * cooperative completion pump -- and none of it needs a PowerPC, a Switch, or
 * fifteen minutes.
 *
 * So this reproduces the shape on the host, where a question costs
 * milliseconds instead of a hardware cycle. It drives the real shims, not a
 * model of them: cafeos_coreinit_sync.h is included directly.
 *
 * The shape being reproduced, from the logs:
 *
 *   game thread : issues a read, pumps a completion, the callback signals E,
 *                 then waits on E for the next one
 *   worker      : parks in OSWaitEvent on a different event, wakes on timeout
 *   pump        : delivers completions, and a completion signals E
 *
 * Every case runs under a watchdog. A deadlocked case is REPORTED and the
 * harness carries on, rather than hanging the run -- a test that hangs tells
 * you less than one that says which case hung.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ppc_runtime.h"

/* The shims call into recompiled guest code to deliver a completion. Here
 * there is no guest code, so the dispatch is replaced by a hook the test
 * controls -- which is the point: it lets a completion do the one thing
 * suspected of deadlocking, signal the event its own waiter is on. */
static void (*g_on_dispatch)(PpcContext *ctx, uint32_t addr);
#define ppc_dispatch arkchemy_test_dispatch
static void arkchemy_test_dispatch(PpcContext *ctx, uint32_t addr)
{
    if (g_on_dispatch) g_on_dispatch(ctx, addr);
}

/* The FS shim owns the completion queue and the pump; the sync shim calls
 * into it. Both are needed, and in this order, because the sync shim only
 * forward-declares nothing -- it expects the pump to already exist. */
#include "cafeos_coreinit_fs.h"
#include "cafeos_coreinit_sync.h"

static PpcSharedMemory g_shared;

static PpcContext *make_ctx(void)
{
    PpcContext *c = calloc(1, sizeof(PpcContext));
    c->shared = &g_shared;
    return c;
}

/* ---- watchdog ---------------------------------------------------------- */

static volatile int g_case_done;
static const char *g_case_name;

static void *watchdog(void *arg)
{
    double limit = *(double *)arg;
    struct timespec t = {0, 20 * 1000 * 1000};
    double waited = 0;
    while (waited < limit) {
        if (g_case_done) return NULL;
        nanosleep(&t, NULL);
        waited += 0.02;
    }
    fprintf(stderr,
            "\n*** DEADLOCK: %s did not finish in %.1fs\n"
            "    OSSignalEvent enter=%u entry=%u lock=%u exit=%u\n"
            "    OSWaitEvent   enter=%u entry=%u lock=%u parked=%u slices=%u exit=%u\n"
            "    fs pump       queued=%u delivered=%u dropped=%u pending=%u cbwork=%u\n",
            g_case_name, limit,
            g_ark_sev_enter, g_ark_sev_got_entry, g_ark_sev_got_lock, g_ark_sev_exit,
            g_ark_wev_enter, g_ark_wev_got_entry, g_ark_wev_got_lock,
            g_ark_wev_parked, g_ark_wev_slices, g_ark_wev_exit,
            g_arkchemy_fs_queued, g_arkchemy_fs_delivered, g_arkchemy_fs_dropped,
            g_arkchemy_fs_pending_n, g_arkchemy_fs_cb_work);
    _exit(3);            /* the whole point is not to hang the run */
    return NULL;
}

static void begin(const char *name, double limit_s)
{
    static double limit;
    g_case_name = name;
    g_case_done = 0;
    limit = limit_s;
    pthread_t th;
    pthread_create(&th, NULL, watchdog, &limit);
    pthread_detach(th);
    printf("  %-52s", name);
    fflush(stdout);
}

static void pass(void) { g_case_done = 1; printf("ok\n"); }

/* ---- the event under test --------------------------------------------- */

#define EVENT_A 0x1000u
#define EVENT_B 0x2000u

static uint32_t g_signal_this;      /* what a delivered completion signals */

static void completion_signals_event(PpcContext *ctx, uint32_t addr)
{
    (void)addr;
    uint32_t saved = ctx->r[3];
    ctx->r[3] = g_signal_this;
    ppc_import_coreinit_OSSignalEvent(ctx);
    ctx->r[3] = saved;
}

static void queue_one_completion(PpcContext *ctx, uint32_t event_addr)
{
    g_signal_this = event_addr;
    g_on_dispatch = completion_signals_event;
    /* async_data lives in guest memory; the shim reads a callback pointer at
     * +0 and a param at +4. Any non-zero callback reaches the dispatch hook. */
    uint32_t ad = 0x8000u;
    ppc_store_u32(ctx, ad + 0, 0xdead0000u);
    ppc_store_u32(ctx, ad + 4, 0u);
    ppc_store_u32(ctx, ad + 8, 0u);
    if (g_arkchemy_fs_pending_n < ARKCHEMY_FS_ASYNC_QUEUE) {
        ArkchemyFsPending *q = &g_arkchemy_fs_pending[g_arkchemy_fs_pending_n++];
        q->async_data = ad; q->client = 0; q->block = 0; q->status = 1;
        g_arkchemy_fs_queued++;
    }
}

/* ---- cases ------------------------------------------------------------- */

/* The one the hardware looks like: a thread waits for a completion that is
 * already queued, and the completion's callback signals the very event it is
 * waiting on. If the pump runs while holding the event lock, this never
 * returns. */
static void case_wait_for_already_queued_completion(void)
{
    PpcContext *ctx = make_ctx();
    begin("wait for a completion already in the queue", 5.0);
    queue_one_completion(ctx, EVENT_A);
    ctx->r[3] = EVENT_A;
    ppc_import_coreinit_OSWaitEvent(ctx);
    pass();
    free(ctx);
}

/* Signalling from inside a delivered completion, with no waiter present. An
 * AUTO event must latch so the next waiter consumes it rather than blocking
 * forever on a signal that already happened. */
static void case_signal_with_no_waiter_latches(void)
{
    PpcContext *ctx = make_ctx();
    begin("a signal with nobody waiting is not lost", 5.0);
    ctx->r[3] = EVENT_B;
    ppc_import_coreinit_OSSignalEvent(ctx);
    ctx->r[3] = EVENT_B;
    ppc_import_coreinit_OSWaitEvent(ctx);     /* must return immediately */
    pass();
    free(ctx);
}

static void *waiter_thread(void *arg)
{
    PpcContext *ctx = (PpcContext *)arg;
    ctx->r[3] = EVENT_A;
    ppc_import_coreinit_OSWaitEvent(ctx);
    return NULL;
}

/* Two threads, the real arrangement: one parked in OSWaitEvent while another
 * signals from inside a pump. This is where a lock-order inversion between
 * the event table lock and the per-event lock would show. */
static void case_signal_from_another_thread_wakes_waiter(void)
{
    PpcContext *waiter = make_ctx();
    PpcContext *signaller = make_ctx();
    begin("a parked waiter is woken by another thread", 5.0);
    pthread_t th;
    pthread_create(&th, NULL, waiter_thread, waiter);
    struct timespec t = {0, 50 * 1000 * 1000};
    nanosleep(&t, NULL);                      /* let it park */
    signaller->r[3] = EVENT_A;
    ppc_import_coreinit_OSSignalEvent(signaller);
    pthread_join(th, NULL);
    pass();
    free(waiter); free(signaller);
}

/* A waiter that must be rescued by work only the pump can do -- the exact
 * starvation the 1ms slices were added for. The completion is queued AFTER
 * the waiter has parked, by another thread, and only the waiter's own pump
 * can deliver it. */
static void *late_queuer(void *arg)
{
    PpcContext *ctx = (PpcContext *)arg;
    struct timespec t = {0, 100 * 1000 * 1000};
    nanosleep(&t, NULL);
    queue_one_completion(ctx, EVENT_A);
    return NULL;
}

static void case_waiter_pumps_its_own_rescue(void)
{
    PpcContext *waiter = make_ctx();
    PpcContext *queuer = make_ctx();
    begin("a parked waiter pumps the work that frees it", 5.0);
    pthread_t th;
    pthread_create(&th, NULL, late_queuer, queuer);
    waiter->r[3] = EVENT_A;
    ppc_import_coreinit_OSWaitEvent(waiter);
    pthread_join(th, NULL);
    pass();
    free(waiter); free(queuer);
}

/* The bug this harness found, pinned so it cannot come back: the pump runs a
 * guest callback using the caller's context, and must leave the argument
 * registers as it found them. A caller that reads r3 after pumping -- which
 * both OSWaitEvent variants do -- otherwise waits on the wrong event. */
static void case_pump_does_not_eat_its_callers_argument(void)
{
    PpcContext *ctx = make_ctx();
    begin("the pump leaves its caller's registers alone", 5.0);
    queue_one_completion(ctx, EVENT_B);
    for (int i = 3; i <= 12; i++) ctx->r[i] = 0xA0000000u + (uint32_t)i;
    uint32_t lr = 0x1234u;
    ctx->lr = lr;
    arkchemy_fs_pump_completions(ctx);
    for (int i = 3; i <= 12; i++) {
        if (ctx->r[i] != 0xA0000000u + (uint32_t)i) {
            fprintf(stderr, "\n*** r%d was eaten: %08x\n", i, ctx->r[i]);
            _exit(4);
        }
    }
    if (ctx->lr != lr) { fprintf(stderr, "\n*** lr was eaten\n"); _exit(4); }
    pass();
    free(ctx);
}

int main(void)
{
    printf("sync harness -- the real shims, no Switch\n\n");
    case_signal_with_no_waiter_latches();
    case_pump_does_not_eat_its_callers_argument();
    case_wait_for_already_queued_completion();
    case_signal_from_another_thread_wakes_waiter();
    case_waiter_pumps_its_own_rescue();
    printf("\nOSSignalEvent enter=%u entry=%u lock=%u exit=%u\n",
           g_ark_sev_enter, g_ark_sev_got_entry, g_ark_sev_got_lock, g_ark_sev_exit);
    printf("OSWaitEvent   enter=%u entry=%u lock=%u parked=%u slices=%u exit=%u\n",
           g_ark_wev_enter, g_ark_wev_got_entry, g_ark_wev_got_lock,
           g_ark_wev_parked, g_ark_wev_slices, g_ark_wev_exit);
    printf("fs pump       queued=%u delivered=%u pending=%u\n",
           g_arkchemy_fs_queued, g_arkchemy_fs_delivered, g_arkchemy_fs_pending_n);
    printf("\nall cases completed\n");
    return 0;
}
