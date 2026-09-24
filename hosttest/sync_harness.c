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
#include <sched.h>
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
    printf("  %-60s", name);
    fflush(stdout);
}

static int g_case_failed;
static void pass(void) { g_case_done = 1; printf(g_case_failed ? "FAILED\n" : "ok\n"); g_case_failed = 0; }

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
    arkchemy_fs_enqueue(ad, 0, 0, 1);
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

/* The blast radius. Five of the six pump call sites read ctx->r[3] on the
 * very next line, so before the fix each of them acted on whatever the last
 * completion callback left behind:
 *
 *   OSLockMutex             locked the WRONG mutex
 *   OSWaitEvent             waited on the wrong event
 *   OSWaitEventWithTimeout  wrong event, and r5:r6 is its 64-bit timeout,
 *                           so the deadline was garbage too
 *   OSWaitSemaphore         waited on the wrong semaphore
 *   OSTryWaitSemaphore      likewise
 *
 * Locking the wrong mutex is the frightening one: it does not hang, it
 * quietly fails to exclude, and the damage surfaces somewhere else entirely.
 * Checked here rather than reasoned about. */
static void case_lock_mutex_locks_the_mutex_it_was_asked_for(void)
{
    PpcContext *ctx = make_ctx();
    begin("OSLockMutex locks the mutex it was given", 5.0);
    queue_one_completion(ctx, EVENT_B);
    uint32_t want = 0x7100u;
    ctx->r[3] = want;
    ppc_import_coreinit_OSLockMutex(ctx);
    if (ctx->r[3] != want) {
        fprintf(stderr, "\n*** locked mutex %08x, was asked for %08x\n", ctx->r[3], want);
        _exit(4);
    }
    ppc_import_coreinit_OSUnlockMutex(ctx);
    pass();
    free(ctx);
}

static void case_wait_with_timeout_keeps_its_timeout(void)
{
    PpcContext *ctx = make_ctx();
    begin("OSWaitEventWithTimeout keeps event and deadline", 5.0);
    queue_one_completion(ctx, EVENT_B);
    ctx->r[3] = EVENT_A;
    ctx->r[5] = 0; ctx->r[6] = 1000;      /* a tiny but positive timeout */
    ppc_import_coreinit_OSWaitEventWithTimeout(ctx);
    /* It should time out and return, not block on some other event. Reaching
     * here at all is the assertion; the watchdog is the failure path. */
    pass();
    free(ctx);
}


/* ---- AUTO / MANUAL semantics ---------------------------------------------
 *
 * What Cafe OS guarantees, and what the loading stall depends on:
 *
 *   AUTO    OSSignalEvent wakes exactly ONE queued waiter. With nobody
 *           queued it latches, and the next waiter consumes the latch.
 *           OSSignalEventAll wakes every queued waiter and latches nothing.
 *   MANUAL  a signal releases everyone and stays set until OSResetEvent.
 *
 * Before 2026-09-24 the shim broke the AUTO rule twice over. A signal bumped
 * one per-event epoch that every parked waiter was comparing against, so one
 * signal released ALL of them. And a second signal arriving before the
 * first woken waiter had run found waiting_count still non-zero, bumped the
 * epoch again and latched nothing -- so it was lost.
 *
 * These cases fail on that code and pass on the credit scheme. Each uses its
 * own event address so a failure cannot leak into the next case. */

static int g_semantic_failures;

static void expect(int ok, const char *what)
{
    if (!ok) { g_semantic_failures++; g_case_failed = 1; fprintf(stderr, "\n      * %s ", what); }
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static int waiting_on(uint32_t addr)
{
    ArkchemyEventEntry *e = arkchemy_event_get(addr, 0, 1);
    pthread_mutex_lock(&e->lock);
    int n = e->waiting_count;
    pthread_mutex_unlock(&e->lock);
    return n;
}

static void wait_until_waiting(uint32_t addr, int n)
{
    for (int i = 0; i < 400 && waiting_on(addr) < n; i++) sleep_ms(5);
}

static void init_event(uint32_t addr, int value, int mode)
{
    PpcContext *c = make_ctx();
    c->r[3] = addr; c->r[4] = (uint32_t)value; c->r[5] = (uint32_t)mode;
    ppc_import_coreinit_OSInitEvent(c);
    free(c);
}

static void signal_event(uint32_t addr)
{
    PpcContext *c = make_ctx();
    c->r[3] = addr;
    ppc_import_coreinit_OSSignalEvent(c);
    free(c);
}

static void signal_event_all(uint32_t addr)
{
    PpcContext *c = make_ctx();
    c->r[3] = addr;
    ppc_import_coreinit_OSSignalEventAll(c);
    free(c);
}

static void reset_event(uint32_t addr)
{
    PpcContext *c = make_ctx();
    c->r[3] = addr;
    ppc_import_coreinit_OSResetEvent(c);
    free(c);
}

/* A timed wait from the calling thread; returns the shim's BOOL. Timeout in
 * milliseconds, converted to the 62.15625 MHz timer the shim expects. */
static int timed_wait(uint32_t addr, long ms)
{
    PpcContext *c = make_ctx();
    int64_t ticks = (int64_t)ms * (ARKCHEMY_ESPRESSO_TIMER_CLOCK / 1000);
    c->r[3] = addr;
    c->r[5] = (uint32_t)((uint64_t)ticks >> 32);
    c->r[6] = (uint32_t)ticks;
    ppc_import_coreinit_OSWaitEventWithTimeout(c);
    int r = (int)c->r[3];
    free(c);
    return r;
}

typedef struct { uint32_t addr; long timeout_ms; volatile int done; volatile int result; } Waiter;

static void *untimed_waiter(void *arg)
{
    Waiter *w = arg;
    PpcContext *c = make_ctx();
    c->r[3] = w->addr;
    ppc_import_coreinit_OSWaitEvent(c);
    free(c);
    w->result = 1;
    __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void *timed_waiter(void *arg)
{
    Waiter *w = arg;
    w->result = timed_wait(w->addr, w->timeout_ms);
    __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static int count_done(Waiter *w, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++) k += __atomic_load_n(&w[i].done, __ATOMIC_SEQ_CST);
    return k;
}

#define EV_AUTO_ONE   0x3000u
#define EV_AUTO_TWICE 0x3100u
#define EV_AUTO_TIMED 0x3200u
#define EV_MANUAL     0x3300u
#define EV_AUTO_ALL   0x3400u
#define EV_RESET      0x3500u
#define EV_REINIT     0x3600u

/* The stall's own shape: two threads parked on one AUTO event, one signal. */
static void case_auto_signal_wakes_exactly_one(void)
{
    begin("AUTO: one signal wakes exactly one of two waiters", 10.0);
    init_event(EV_AUTO_ONE, 0, 1);
    Waiter w[2] = {{EV_AUTO_ONE}, {EV_AUTO_ONE}};
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, untimed_waiter, &w[i]);
    wait_until_waiting(EV_AUTO_ONE, 2);
    signal_event(EV_AUTO_ONE);
    sleep_ms(150);
    expect(count_done(w, 2) == 1, "one signal released both waiters (or neither)");
    signal_event(EV_AUTO_ONE);                /* releases the other */
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    pass();
}

/* Two signals before the woken waiter has run: the first wakes it, the
 * second must latch rather than vanish. */
static void case_auto_second_signal_latches(void)
{
    begin("AUTO: a second signal while the first is in flight latches", 10.0);
    init_event(EV_AUTO_TWICE, 0, 1);
    Waiter w = {EV_AUTO_TWICE};
    pthread_t th;
    pthread_create(&th, NULL, untimed_waiter, &w);
    wait_until_waiting(EV_AUTO_TWICE, 1);
    signal_event(EV_AUTO_TWICE);
    signal_event(EV_AUTO_TWICE);
    pthread_join(th, NULL);
    expect(timed_wait(EV_AUTO_TWICE, 50) == 1, "the second signal was lost");
    expect(timed_wait(EV_AUTO_TWICE, 50) == 0, "the latch was consumed twice");
    pass();
}

/* The timeout variant, which the job-queue workers park in: one signal must
 * return TRUE to exactly one of them, and the other must time out FALSE. */
static void case_auto_timed_waiters_one_true(void)
{
    begin("AUTO timed: one signal, one TRUE and one FALSE", 10.0);
    init_event(EV_AUTO_TIMED, 0, 1);
    Waiter w[2] = {{EV_AUTO_TIMED, 400}, {EV_AUTO_TIMED, 400}};
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, timed_waiter, &w[i]);
    wait_until_waiting(EV_AUTO_TIMED, 2);
    signal_event(EV_AUTO_TIMED);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    expect(w[0].result + w[1].result == 1, "both or neither reported being signalled");
    expect(waiting_on(EV_AUTO_TIMED) == 0, "a timed-out waiter was left counted");
    pass();
}

static void case_manual_releases_all_and_stays_set(void)
{
    begin("MANUAL: releases every waiter and stays set", 10.0);
    init_event(EV_MANUAL, 0, 0);
    Waiter w[3] = {{EV_MANUAL}, {EV_MANUAL}, {EV_MANUAL}};
    pthread_t th[3];
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, untimed_waiter, &w[i]);
    wait_until_waiting(EV_MANUAL, 3);
    signal_event(EV_MANUAL);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    expect(timed_wait(EV_MANUAL, 50) == 1, "MANUAL did not stay set");
    expect(timed_wait(EV_MANUAL, 50) == 1, "MANUAL was consumed like AUTO");
    reset_event(EV_MANUAL);
    expect(timed_wait(EV_MANUAL, 50) == 0, "OSResetEvent did not clear MANUAL");
    pass();
}

static void case_auto_signal_all_wakes_all_latches_nothing(void)
{
    begin("AUTO: SignalEventAll wakes all, latches nothing", 10.0);
    init_event(EV_AUTO_ALL, 0, 1);
    Waiter w[3] = {{EV_AUTO_ALL}, {EV_AUTO_ALL}, {EV_AUTO_ALL}};
    pthread_t th[3];
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, untimed_waiter, &w[i]);
    wait_until_waiting(EV_AUTO_ALL, 3);
    signal_event_all(EV_AUTO_ALL);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    expect(timed_wait(EV_AUTO_ALL, 50) == 0, "SignalEventAll with waiters left a latch");
    pass();
}

static void case_reset_forgets_a_latched_signal(void)
{
    begin("OSResetEvent forgets a latched AUTO signal", 10.0);
    init_event(EV_RESET, 0, 1);
    signal_event(EV_RESET);                   /* nobody waiting: latches */
    reset_event(EV_RESET);
    expect(timed_wait(EV_RESET, 50) == 0, "a reset latch still woke a waiter");
    pass();
}

/* Re-initialising an event with a waiter parked on it is undefined on real
 * hardware, but must not wedge the shim: the waiter is released, and the
 * event behaves as freshly initialised afterwards. */
static void case_reinit_releases_waiters(void)
{
    begin("OSInitEvent on a live event releases its waiters", 10.0);
    init_event(EV_REINIT, 0, 1);
    Waiter w = {EV_REINIT};
    pthread_t th;
    pthread_create(&th, NULL, untimed_waiter, &w);
    wait_until_waiting(EV_REINIT, 1);
    init_event(EV_REINIT, 0, 1);
    pthread_join(th, NULL);
    expect(waiting_on(EV_REINIT) == 0, "re-init left a waiter counted");
    expect(timed_wait(EV_REINIT, 50) == 0, "re-init left the event signalled");
    pass();
}

/* ---- the completion queue under contention ---------------------------------
 *
 * The FS completion queue is written by the thread that issues a read and
 * drained by whichever thread next pumps -- the game thread, a job-queue
 * worker in OSWaitEventWithTimeout, an FMOD thread in OSLockMutex. Until
 * 2026-09-24 it had no lock, and the pump's re-entrancy flag was a plain int
 * two threads could both see clear. A completion taken twice, or shifted out
 * from under a concurrent enqueue, is a read whose callback never runs: its
 * work item never reaches status 2 and its block is never released.
 *
 * This hammers the queue from several producers and several pumpers and
 * counts every delivery by id. Each completion must arrive exactly once. */

#define STRESS_PRODUCERS 4
#define STRESS_PER_PRODUCER 4000
#define STRESS_PUMPERS 4
#define STRESS_TOTAL (STRESS_PRODUCERS * STRESS_PER_PRODUCER)

static volatile uint32_t g_stress_seen[STRESS_TOTAL];
static volatile int g_stress_stop;

/* The id rides in `client`; the callback records it. */
static void count_delivery(PpcContext *ctx, uint32_t addr)
{
    (void)addr;
    uint32_t id = ctx->r[3];
    if (id < STRESS_TOTAL) __atomic_add_fetch(&g_stress_seen[id], 1, __ATOMIC_SEQ_CST);
}

static void *stress_producer(void *arg)
{
    uint32_t base = (uint32_t)(uintptr_t)arg * STRESS_PER_PRODUCER;
    for (uint32_t i = 0; i < STRESS_PER_PRODUCER; i++) {
        /* a full queue is back-pressure, not loss: retry until it takes */
        while (!arkchemy_fs_enqueue(0x8000u, base + i, 0, 1)) sched_yield();
    }
    return NULL;
}

static void *stress_pumper(void *arg)
{
    PpcContext *c = make_ctx();
    (void)arg;
    while (!__atomic_load_n(&g_stress_stop, __ATOMIC_SEQ_CST)) arkchemy_fs_pump_completions(c);
    arkchemy_fs_pump_completions(c);
    free(c);
    return NULL;
}

static void case_queue_delivers_each_completion_once(void)
{
    begin("FS queue: every completion delivered exactly once", 30.0);
    PpcContext *c = make_ctx();
    ppc_store_u32(c, 0x8000u + 0, 0xdead0000u);   /* non-zero callback */
    ppc_store_u32(c, 0x8000u + 4, 0u);
    free(c);
    g_on_dispatch = count_delivery;
    g_stress_stop = 0;
    pthread_t prod[STRESS_PRODUCERS], pump[STRESS_PUMPERS];
    for (int i = 0; i < STRESS_PUMPERS; i++) pthread_create(&pump[i], NULL, stress_pumper, NULL);
    for (int i = 0; i < STRESS_PRODUCERS; i++) pthread_create(&prod[i], NULL, stress_producer, (void *)(uintptr_t)i);
    for (int i = 0; i < STRESS_PRODUCERS; i++) pthread_join(prod[i], NULL);
    __atomic_store_n(&g_stress_stop, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < STRESS_PUMPERS; i++) pthread_join(pump[i], NULL);
    PpcContext *d = make_ctx();
    arkchemy_fs_pump_completions(d);             /* anything left behind */
    free(d);
    int lost = 0, doubled = 0;
    for (int i = 0; i < STRESS_TOTAL; i++) {
        if (g_stress_seen[i] == 0) lost++;
        if (g_stress_seen[i] > 1) doubled++;
    }
    if (lost || doubled) {
        char msg[128];
        snprintf(msg, sizeof msg, "%d of %d lost, %d delivered more than once", lost, STRESS_TOTAL, doubled);
        expect(0, msg);
    }
    g_on_dispatch = NULL;
    pass();
}

int main(void)
{
    printf("sync harness -- the real shims, no Switch\n\n");
    case_signal_with_no_waiter_latches();
    case_pump_does_not_eat_its_callers_argument();
    case_lock_mutex_locks_the_mutex_it_was_asked_for();
    case_wait_with_timeout_keeps_its_timeout();
    case_wait_for_already_queued_completion();
    case_signal_from_another_thread_wakes_waiter();
    case_waiter_pumps_its_own_rescue();
    printf("\n");
    case_auto_signal_wakes_exactly_one();
    case_auto_second_signal_latches();
    case_auto_timed_waiters_one_true();
    case_manual_releases_all_and_stays_set();
    case_auto_signal_all_wakes_all_latches_nothing();
    case_reset_forgets_a_latched_signal();
    case_reinit_releases_waiters();
    case_queue_delivers_each_completion_once();
    printf("\nOSSignalEvent enter=%u entry=%u lock=%u exit=%u\n",
           g_ark_sev_enter, g_ark_sev_got_entry, g_ark_sev_got_lock, g_ark_sev_exit);
    printf("OSWaitEvent   enter=%u entry=%u lock=%u parked=%u slices=%u exit=%u\n",
           g_ark_wev_enter, g_ark_wev_got_entry, g_ark_wev_got_lock,
           g_ark_wev_parked, g_ark_wev_slices, g_ark_wev_exit);
    printf("fs pump       queued=%u delivered=%u pending=%u\n",
           g_arkchemy_fs_queued, g_arkchemy_fs_delivered, g_arkchemy_fs_pending_n);
    if (g_semantic_failures) {
        printf("\n%d semantic check(s) FAILED\n", g_semantic_failures);
        return 1;
    }
    printf("\nall cases completed\n");
    return 0;
}
