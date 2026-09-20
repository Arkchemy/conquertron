#ifndef ARK_BLOCKPROBE_H
#define ARK_BLOCKPROBE_H

/* Probe state for igArchiveBlockManager, kept out of ppc_runtime.h on purpose.
 *
 * ppc_runtime.h and cafeos_gx2.h are included by all 224 translation units, so
 * a counter added to either costs a full ~40 minute rebuild. This probe is
 * needed by exactly two files -- generated_0158.c, which holds
 * getNumAvailableBlocks, and main.c, which prints it -- so it lives here and
 * costs two objects and a link.
 *
 * What this measures, and why these fields:
 *
 * getNumAvailableBlocks at 0x216e230 walks the block array live and counts
 * entries whose _state (block+0x14) is 0 or 2. Measured 2026-09-19: the walk
 * ran 4549 times and saw 27,294 blocks, so the pool holds exactly 6. Of those
 * sightings 5,823 were state 0 and 92 were state 2 -- 21.7% available -- while
 * the last call returned 0. So the pool was working for much of the run and
 * was empty by the end: a leak that accumulates, not a pool that was never
 * filled.
 *
 * Two things that reading is missing, both of which this fixes:
 *
 * `avail last=0` came from g_ark_ap[8], assigned at the end of the function,
 * so it described the final call and nothing else. A histogram of the return
 * value says how the pool actually behaved over time, and `lastnz` -- the call
 * index of the last non-zero return, against 4549 total -- dates the death.
 * A cliff and a slow decay are different bugs and the old field could not tell
 * them apart.
 *
 * `other=21379` bucketed every non-available state into one label. That is the
 * same shape of mistake as reading allocEarly as a failure path: three or more
 * possibilities wearing one name. blkstate keeps the raw value per slot, so
 * "all six stuck in state 1" and "six blocks spread across states 1, 3 and 4"
 * are distinguishable without another run. */

#include <stdint.h>
#include <time.h>

/* The probes' own clock. arkchemy_gx2_host_ticks lives in cafeos_gx2.h, which
 * the FS shim does not include and should not have to; this is the same two
 * lines and keeps the probe header free-standing. */
static inline unsigned long long ark_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

#ifdef __GNUC__
__attribute__((weak))
#endif
/* Return-value histogram. Index is the count returned; 8 collects anything
 * larger, which would mean the pool is bigger than the 6 measured. */
volatile uint32_t g_ark_avh[9];

#ifdef __GNUC__
__attribute__((weak))
#endif
/* Call index (g_ark_ap[7]) of the last call that returned non-zero, the
 * largest count ever returned, and the total number of calls that returned
 * zero. lastnz against the call total is the age of death. */
volatile uint32_t g_ark_avh_lastnz = 0, g_ark_avh_maxret = 0, g_ark_avh_zero = 0;

#ifdef __GNUC__
__attribute__((weak))
#endif
/* Per-slot raw _state and block pointer, last call wins -- so this is the
 * state of the pool at exit, which is the moment worth knowing. */
volatile uint32_t g_ark_blkstate[8], g_ark_blkaddr[8];

#ifdef __GNUC__
__attribute__((weak))
#endif
/* list->_count as the walk itself read it, so a pool that is not 6 is visible
 * rather than assumed from arithmetic on two other counters. */
volatile uint32_t g_ark_blkn = 0, g_ark_blklist = 0;

/* Called once per block per walk, from inside the loop at 0x216e250. */
/* HOTPROBE: the hooks that run often enough to change what they measure.
 *
 * The byte count is deterministic per build and differs between builds --
 * 684,652, then 1,077,868 with one probe set, then 815,724 with another.
 * That is not run-to-run variance, it is the instrumentation perturbing a
 * timing-sensitive race for a six-block pool.
 *
 * ark_avh_block is the worst offender by a wide margin: it runs once per
 * block per availability walk, and the archive pump fix took that walk from
 * 4,647 calls a run to 80,651 -- about 484,000 hook calls, where before the
 * fix there were 28,000. The overhead scales up exactly when the fix starts
 * working, which is the worst possible shape for measuring whether the fix
 * helps.
 *
 * Set to 0 to run the fixes with the hot hooks quiet. The cold probes stay
 * on; they fire tens of times a run, not hundreds of thousands. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_hotprobe = 0u;   /* OFF: measuring the fixes, not the probes */

static inline void ark_avh_block(uint32_t idx, uint32_t addr, uint32_t state) {
    if (!g_ark_hotprobe) return;
    if (idx < 8u) { g_ark_blkstate[idx] = state; g_ark_blkaddr[idx] = addr; }
}

/* Called on every exit from the walk, including the early `bgelr` that
 * returns 0 without looping. `calls` is the walk's own call counter, passed in
 * rather than read here so this header does not depend on ppc_runtime.h. */
/* Runs once per availability walk, not once per block -- about 80,000 times a
 * run against ark_avh_block's 484,000. Cheap enough to leave on: a histogram
 * index, two compares and a couple of stores. It is deliberately NOT behind
 * g_ark_hotprobe, because knowing whether the pool is empty at the stall is
 * the whole question and this is the cheapest way to ask it.
 *
 * `list` is captured so the block array can be walked once at exit from
 * main.c, instead of hooking the walk and paying per block. One store here
 * buys the per-block detail BLKSTATE used to cost 484,000 calls for. */
static inline void ark_avh_ret(uint32_t ret, uint32_t n, uint32_t calls, uint32_t list) {
    /* Nothing runs here unless the histogram is wanted. The count POOLEXIT
     * needs is read from the list at exit instead, so this path is a single
     * compare when gated -- restoring the exact runtime shape of the build
     * that reached 1,208,940, to find out whether that figure reproduces. */
    if (!g_ark_hotprobe) return;
    g_ark_blkn = n;
    g_ark_avh[ret < 8u ? ret : 8u]++;
    if (ret) { g_ark_avh_lastnz = calls; if (ret > g_ark_avh_maxret) g_ark_avh_maxret = ret; }
    else g_ark_avh_zero++;
    (void)list;
}

/* RELGATE: the one branch that decides whether a block goes back to the pool.
 *
 * igArchive::updateTasks at 0x21682a0 reads task->_block (+0x1c) and, only if
 * it is non-null, copies the block's data out, calls ageBlocks and writes
 * state 2 at 0x21682d0. That store is the only per-block release in the
 * streaming path -- the block manager's own deallocate() is per-archive and
 * fires on archive close, which cannot help while global.arc is still open.
 *
 * So a null _block there means the block is never handed back while still
 * being held in use, which is the leak shape the state histogram shows: 6
 * blocks, 21.7% of sightings available, 0 available by the end.
 *
 * reached counts arrivals at the guard, skipped counts the null case, and
 * released counts the store actually executing. skipped tracking the frame
 * count while released stays near the 92 state-2 sightings names this
 * instruction as the leak. All three near zero means updateTasks reaches this
 * point rarely and the gate is further up, which is a different bug and worth
 * knowing in the same run. lasttask keeps the task pointer from the most
 * recent skip so it can be matched against ARCHQ's object. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_rel_reached = 0, g_ark_rel_skipped = 0,
                  g_ark_rel_released = 0, g_ark_rel_lasttask = 0,
                  g_ark_rel_skip_witha = 0, g_ark_rel_lasta = 0,
                  g_ark_rel_a_st1 = 0, g_ark_rel_a_st2 = 0, g_ark_rel_a_other = 0;

/* blockA (+0x18) alongside blockB (+0x1c) at the release gate.
 *
 * startNewTasks gives a task two block slots: +0x18 always, +0x1c only when
 * a second allocate ran (0x2168e60 branches past it). The gate at 0x21682a0
 * reads +0x1c alone and skips everything when it is NULL, so a single-block
 * task releases nothing. RELGATE reached=60 skipped=55 released=5 fits that
 * exactly, and TASKLINK shows 10 of 12 pairs with no blockB.
 *
 * withA counts skips where +0x18 still holds a block -- those are the leaks.
 * If skipped and withA diverge, single-block tasks are being released some
 * other way and this reading is wrong. */
/* blockA's STATE, not just its pointer.
 *
 * skip_withA came back 55 of 55, but a non-null pointer does not mean the
 * block is still held: the blockB path memcpys out of its buffer before
 * marking it cached, which reads like a spanning read, and a symmetric
 * blockA copy earlier in updateTasks would leave the pointer set on a block
 * already returned. Counting blockA's state at the gate separates those.
 *
 * a_st1 near 55 means blockA really is still in use and abandoned here, and
 * the leak is this instruction. a_st2 near 55 means blockA was already
 * released upstream, the pointer is just residue, and the leak is somewhere
 * else entirely -- which would make a naive "release +0x18 too" fix a double
 * free. */
static inline void ark_rel_gate2(uint32_t task, uint32_t blockB, uint32_t blockA,
                                 uint32_t blockA_state) {
    g_ark_rel_reached++;
    if (blockB) g_ark_rel_released++;
    else {
        g_ark_rel_skipped++; g_ark_rel_lasttask = task;
        if (blockA) {
            g_ark_rel_skip_witha++; g_ark_rel_lasta = blockA;
            if (blockA_state == 1u) g_ark_rel_a_st1++;
            else if (blockA_state == 2u) g_ark_rel_a_st2++;
            else g_ark_rel_a_other++;
        }
    }
}

static inline void ark_rel_gate(uint32_t task, uint32_t block) {
    g_ark_rel_reached++;
    if (block) g_ark_rel_released++;
    else { g_ark_rel_skipped++; g_ark_rel_lasttask = task; }
}

/* LOADCURVE: archive progress sampled against the wall clock.
 *
 * An end-of-run total says where the clock cut the run, not whether anything
 * was still moving. Cycles 65 and 66 both ended at exactly 684,652 bytes with
 * different pump counts, which looks like a hard stop; cycle 67 ended at
 * 1,077,868, which looks like a rate. One total per run cannot tell those
 * apart no matter how long the run is.
 *
 * Sampling the same counters on a fixed period turns a single run into a
 * curve. A straight line means the load works and is merely slow, and the
 * slope gives the time a full 16.9MB archive would need. A flat tail means a
 * real stall, and the value it flattens at, together with the sample index
 * where it flattened, dates it against everything else in the log. A staircase
 * means it moves in bursts and the gaps are the thing to explain.
 *
 * 64 slots at 5 s covers 320 s; the sampler stops recording when full rather
 * than wrapping, so a longer run keeps the beginning, which is where the
 * interesting shape is. */
#define ARK_LOADCURVE_SLOTS     64u
#define ARK_LOADCURVE_PERIOD_NS 5000000000ull

#ifdef __GNUC__
__attribute__((weak))
#endif
/* per sample: 0 elapsed seconds, 1 async bytes, 2 async reads,
 * 3 startBlockRead, 4 decompressBatch, 5 updateArchiveSystem */
volatile uint32_t g_lc[ARK_LOADCURVE_SLOTS][6];

#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_lc_n = 0;

#ifdef __GNUC__
__attribute__((weak))
#endif
volatile unsigned long long g_lc_next_ns = 0;

/* FCTXGATE: the branch that decides whether any storage device gets updated.
 *
 * igFileContext::update at 0x216e638 loads this->+0x10, calls that object's
 * vtable slot 0xcc (index 51) with 0, and at 0x216e674 returns early on a
 * non-zero result -- skipping the whole device loop at 0x216e678 that would
 * otherwise reach igArchive::update through vtable slot 0x184.
 *
 * Measured 2026-09-19 over a 900 s run: igFileContext::update reached 17,259
 * while igArchive::update froze at 4,647, and LOADCURVE showed every byte of
 * archive traffic arriving in one burst at t=40-45 s and nothing for the
 * remaining 855 s. So the context keeps ticking and the walk stops. This says
 * whether that gate is the reason.
 *
 * gated rising with calls while ran stays frozen names this branch. Both
 * rising together means the gate is fine and the loop is running but finding
 * no devices, which `devs` distinguishes -- it records the device count the
 * walk read, so an empty list and a skipped walk are different answers rather
 * than one silence. obj/vt/ret identify what is being asked and what it said,
 * so the object behind slot 0xcc can be named without another run. */
/* The resolved target of the slot-0xcc call, captured at the dispatch. The
 * retail .rpx is the user's own dump and is not in the tree, so the vtable
 * cannot be read offline; taking ctr at the indirect call gives the same
 * answer and jouster/tools/name-addr.py turns it into a symbol. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fx_target = 0, g_ark_fx_slotword = 0;

#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fx_calls = 0, g_ark_fx_gated = 0, g_ark_fx_ran = 0,
                  g_ark_fx_obj = 0, g_ark_fx_vt = 0, g_ark_fx_ret = 0,
                  g_ark_fx_devs = 0, g_ark_fx_devmin = 0xffffffffu,
                  g_ark_fx_lastgate_call = 0, g_ark_fx_lastran_call = 0;

/* Called with the object and vtable before the slot-0xcc call. */
static inline void ark_fx_enter(uint32_t obj, uint32_t vt) {
    g_ark_fx_calls++; g_ark_fx_obj = obj; g_ark_fx_vt = vt;
}

/* Called with the slot-0xcc return value, at the branch. */
static inline void ark_fx_gate(uint32_t ret) {
    g_ark_fx_ret = ret;
    if (ret) { g_ark_fx_gated++; g_ark_fx_lastgate_call = g_ark_fx_calls; }
    else { g_ark_fx_ran++; g_ark_fx_lastran_call = g_ark_fx_calls; }
}

/* Called with the device count the walk actually read. */
static inline void ark_fx_devs(uint32_t n) {
    g_ark_fx_devs = n;
    if (n < g_ark_fx_devmin) g_ark_fx_devmin = n;
}

/* SEMBAL: obtain/release balance per igCafeSemaphore object.
 *
 * igFileContext::update gates its whole device walk on
 * igCafeSemaphore::obtainResource(false) at 0x215672c, which returns 0 when
 * OSTryWaitSemaphore found a positive count and 1 otherwise. Measured
 * 2026-09-19: it succeeded for the first 2,042 calls and then failed on every
 * one of the next 15,000-odd, so the count reaches zero and stays there.
 *
 * The shim is not the suspect -- it is a plain counting semaphore with the
 * documented prev-count return, checked against Cemu's HLE. And a simple
 * missing release would fail on the second tick, not the 2,043rd. So
 * something takes this semaphore and does not give it back.
 *
 * Counting obtains against releases per object says which. Releases tracking
 * obtains right up to the end means the balance is fine and the count was
 * drained by a third party; releases stopping while obtains continue names
 * the caller that stopped as the bug. Keyed by `this` because the engine has
 * several of these and only the one at igFileContext+0x10 matters. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sem[6][3]; /* this, obtains, releases */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sem_n = 0, g_ark_sem_rel_lr = 0, g_ark_sem_obt_lr = 0;

/* SEMSITE: every (object, call site, kind) triple, counted.
 *
 * The first version kept one global "last release lr", which named
 * igPlatformVisualContext::submitEndDraw -- a graphics function releasing a
 * graphics semaphore, and nothing at all about the file context's. A single
 * global lr across six objects cannot attribute anything; the same shape of
 * error as reading avail(last=0) as a steady state.
 *
 * Measured 2026-09-19 on object 0x4503598: obt=6022 with 1382 gate failures,
 * so 4640 successful acquires against 4639 releases -- a deficit of exactly
 * one. Not an accumulating leak. One caller took it and never gave it back,
 * and this table names every site that touches it so that caller is visible
 * by subtraction: the site whose obtains exceed its releases by one. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_semsite[16][4]; /* self, lr, kind(0=obtain 1=release), count */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_semsite_n = 0, g_ark_semsite_over = 0;

static inline void ark_sem_note(uint32_t self, uint32_t is_release, uint32_t lr) {
    if (!g_ark_hotprobe) return;
    uint32_t i;
    for (i = 0; i < g_ark_sem_n && i < 6u; i++)
        if (g_ark_sem[i][0] == self) break;
    if (i < 6u) {
        if (i == g_ark_sem_n) {
            g_ark_sem[i][0] = self; g_ark_sem[i][1] = 0; g_ark_sem[i][2] = 0;
            g_ark_sem_n++;
        }
        if (is_release) { g_ark_sem[i][2]++; g_ark_sem_rel_lr = lr; }
        else { g_ark_sem[i][1]++; g_ark_sem_obt_lr = lr; }
    }
    for (i = 0; i < g_ark_semsite_n && i < 16u; i++)
        if (g_ark_semsite[i][0] == self && g_ark_semsite[i][1] == lr &&
            g_ark_semsite[i][2] == is_release) break;
    if (i >= 16u) { g_ark_semsite_over++; return; }
    if (i == g_ark_semsite_n) {
        g_ark_semsite[i][0] = self; g_ark_semsite[i][1] = lr;
        g_ark_semsite[i][2] = is_release; g_ark_semsite[i][3] = 0;
        g_ark_semsite_n++;
    }
    g_ark_semsite[i][3]++;
}

/* SCOPELOCK: locks currently held, tracked by construction and destruction.
 *
 * On 0x4503598 the igScopeLock constructor ran 2212 times and the destructor
 * 2211, matching a global deficit of exactly one successful acquire. So this
 * is not a forgotten release -- it is a thread still inside a critical
 * section, which explains the deadlock shape: the holder waits for data that
 * only igFileContext::update can deliver, and update cannot run because the
 * holder has the lock.
 *
 * Counting alone cannot finish the job, though. SEMSITE overflowed its 16
 * slots and dropped ~14k events, so the 2212/2211 pair matching the global
 * deficit is suggestive rather than proof -- other sites on the same object
 * went unrecorded. This tracks the locks themselves: the constructor adds an
 * entry keyed by the igScopeLock object, the destructor removes it, and
 * whatever is still present at exit is the outstanding lock, with the
 * semaphore it holds and the return address of whoever built it.
 *
 * live>0 at exit with a matching semaphore names the holder outright. live==0
 * means the lock was released after all and the deficit is somewhere else,
 * which is a different bug and worth knowing rather than assuming. `over`
 * counts constructions past the table, so a full table is visible instead of
 * silently truncating the answer. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sl[16][4]; /* this, semaphore, creator lr, call seq */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sl_live = 0, g_ark_sl_ctor = 0, g_ark_sl_dtor = 0,
                  g_ark_sl_over = 0, g_ark_sl_orphan = 0, g_ark_sl_peak = 0;

static inline void ark_sl_ctor(uint32_t self, uint32_t sem, uint32_t lr, uint32_t seq) {
    uint32_t i;
    g_ark_sl_ctor++;
    for (i = 0; i < 16u; i++) if (g_ark_sl[i][0] == 0u) break;
    if (i >= 16u) { g_ark_sl_over++; return; }
    g_ark_sl[i][0] = self; g_ark_sl[i][1] = sem;
    g_ark_sl[i][2] = lr;   g_ark_sl[i][3] = seq;
    g_ark_sl_live++;
    if (g_ark_sl_live > g_ark_sl_peak) g_ark_sl_peak = g_ark_sl_live;
}

static inline void ark_sl_dtor(uint32_t self) {
    uint32_t i;
    g_ark_sl_dtor++;
    for (i = 0; i < 16u; i++) {
        if (g_ark_sl[i][0] == self) {
            g_ark_sl[i][0] = 0u; g_ark_sl[i][1] = 0u;
            g_ark_sl[i][2] = 0u; g_ark_sl[i][3] = 0u;
            if (g_ark_sl_live) g_ark_sl_live--;
            return;
        }
    }
    g_ark_sl_orphan++;   /* destroyed something never recorded -- table was full */
}

/* SEMINIT: the resource count each igCafeSemaphore is activated with.
 *
 * activate() at 0x215665c reads this->+0x10 -- whatever setInitialResourceCount
 * stored -- and passes it to OSInitSemaphore for the OSSemaphore at this+0x18.
 *
 * This decides whether the deadlock is real or ours. igFileContext::update
 * gates on a non-blocking obtain of this semaphore, and blockUntilComplete
 * takes it blocking and then waits inside the scope. With a count of 1 those
 * two contend by construction, and on real hardware the boot would hang the
 * same way -- which it does not. A count above 1 lets update keep running
 * while blockUntilComplete waits, and the whole symptom disappears.
 *
 * So: count>1 here means the engine asked for a permissive semaphore and the
 * contention is genuine engine behaviour we are reproducing correctly, and
 * the fault is elsewhere. count==1 with everything else faithful means the
 * engine really does serialise these, and the difference must be that on
 * hardware the waiter's inner wait drives the device itself rather than
 * relying on igFileContext::update -- a different fix entirely. Either way it
 * is one number and it picks the branch. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_si[8][2]; /* this, count */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_si_n = 0;

static inline void ark_si_note(uint32_t self, uint32_t count) {
    uint32_t i;
    for (i = 0; i < g_ark_si_n && i < 8u; i++) if (g_ark_si[i][0] == self) return;
    if (i >= 8u) return;
    g_ark_si[i][0] = self; g_ark_si[i][1] = count; g_ark_si_n++;
}

/* BLOCKWAIT2: the device call that igFileContext::blockUntilComplete waits in.
 *
 * SEMINIT settled that the file context's semaphore is count=1, so the engine
 * genuinely serialises update against blockUntilComplete and hardware would
 * contend the same way. The lock is therefore not the bug.
 *
 * And the release is not missing either. Two instructions after the waiting
 * call, blockUntilComplete runs the igScopeLock destructor unconditionally:
 *
 *   r12 = device->vtable[0x13c]; r3 = device; r4 = 1; bctrl
 *   r31 = r3; r3 = &lock; r4 = 2; bl __dt__igScopeLock
 *
 * So the lock is held open only because that call never returns. enter and
 * exit differing by one confirms a thread is parked inside it, and target
 * names the function -- which is the thing to fix, since on hardware it must
 * drive the device to completion itself rather than waiting for a pump that
 * its own caller has locked out. enter==exit would mean the stall is not here
 * at all and the held lock came from somewhere else. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_bw_enter = 0, g_ark_bw_exit = 0, g_ark_bw_target = 0,
                  g_ark_bw_dev = 0, g_ark_bw_vt = 0, g_ark_bw_ret = 0;

/* PDEV: enter/exit per call site inside igPhysicalStorageDevice::update.
 *
 * The chain ends here. blockUntilComplete holds the file-context semaphore
 * (count=1, so hardware serialises it too) and calls igArchive::update
 * blocking, which forwards through igVirtualStorageDevice::update to this
 * function, which never returns -- so the igScopeLock destructor two
 * instructions later never runs and the lock is held forever.
 *
 * The item loop here is bounded, not a spin, so the park is inside one of the
 * calls it makes. Wrapping each with an enter/exit pair names it: the site
 * whose enter exceeds its exit by one is where the thread is sitting.
 *
 *   0 vtable call before the loop (0x2176fd8)
 *   1 start(igFileWorkItem*)      2 readFromBuffer(int)
 *   3 readCoalesced(int)          4 writeWithBuffer(int)
 *   5 writeWithBuffer/coalesced   6 vtable call in loop (0x21770b4)
 *   7 complete(int)               8 igObjectList::remove(int)
 *
 * All sites balanced would mean the park is in the loop's own control flow
 * rather than a callee, which is a different answer and worth having rather
 * than assuming. tgt0/tgt6 capture the two indirect targets so a vtable call
 * can be named without another run. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_pdev[10][2]; /* enter, exit per site */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_pd_tgt0 = 0, g_ark_pd_tgt6 = 0;

static inline void ark_pd_in(uint32_t i)  { if (g_ark_hotprobe && i < 10u) g_ark_pdev[i][0]++; }
static inline void ark_pd_out(uint32_t i) { if (g_ark_hotprobe && i < 10u) g_ark_pdev[i][1]++; }

/* SPINWAIT2: the innermost loop, and what it polls.
 *
 * igPhysicalStorageDevice::update(workItem, kBlocking) at 0x21755a4:
 *
 *   if (blocking != 1) return; if (item->[0x23] != 1) return;
 *   do { inner = this->+0x50; inner->vtable[0xb4](); }
 *   while (item->[0x23] == 1);
 *
 * An active spin, not a block, waiting on a status byte that never changes.
 * Everything else in the chain follows from this one loop not exiting: the
 * caller never returns, the igScopeLock destructor never runs, the file
 * context semaphore is never released, igFileContext::update is gated
 * forever, igArchive::update stops, and the archive stalls at 684,652 bytes.
 *
 * iters counts turns of the loop and target names the poll. A huge iters with
 * status stuck at 1 means the poll runs and does not advance the item, so the
 * fault is inside whatever slot 0xb4 reaches -- most likely our FS shim
 * failing to mark the item done, given asyncq reports 7 reads issued, 7
 * completed and 0 pending. A small iters would mean the loop is not actually
 * spinning and the thread is parked inside the poll instead, which is a
 * different fault. item and status record which work item and what it holds,
 * so a status other than 1 at exit would say the loop did leave and this
 * reading is wrong. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile unsigned long long g_ark_sw_iters = 0;
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sw_target = 0, g_ark_sw_item = 0, g_ark_sw_status = 0,
                  g_ark_sw_inner = 0, g_ark_sw_enter = 0;

/* SIGBAL: raise against wait, per igCafeSignal object.
 *
 * The guest thread is asleep in igCafeSignal::wait(), which is two
 * instructions -- `addi r3, r3, 0x10; b OSWaitEvent` -- on the signal at
 * 0x452d044. Its counterpart is raise() -> OSSignalEvent.
 *
 * The shim is not the suspect: OSSignalEvent latches, setting signaled=1 when
 * no thread is waiting, so a raise that lands before the wait is remembered
 * rather than lost. That rules out the obvious missed-wakeup race.
 *
 * What is left is whether raise() is ever called on this object. waits>0 with
 * raises==0 means the completion path never reaches it and the fix is on the
 * path that should signal -- most likely our FS completion, since asyncq
 * reports 7 issued, 7 done, 0 pending while the item stays at status 1.
 * raises>0 means the signal does fire and the waiter is missing it for some
 * other reason, which would send the search back to the event shim after all. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sig[8][4]; /* this, raises, waits, lowers */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_sig_n = 0;

static inline void ark_sig_note(uint32_t self, uint32_t kind) {
    uint32_t i;
    for (i = 0; i < g_ark_sig_n && i < 8u; i++) if (g_ark_sig[i][0] == self) break;
    if (i >= 8u) return;
    if (i == g_ark_sig_n) {
        g_ark_sig[i][0] = self; g_ark_sig[i][1] = 0;
        g_ark_sig[i][2] = 0;    g_ark_sig[i][3] = 0;
        g_ark_sig_n++;
    }
    g_ark_sig[i][1u + kind]++;
}

/* TASKLINK: task and block recorded where the code pairs them, not inferred.
 *
 * Three attempts to locate this relationship by address arithmetic have now
 * failed: block+0x0c read as an owner turned out to be a shared index table,
 * 0x08298fbc read as an object had vt=0x3000 and was plain data, and the
 * 0xD8 gap between a task and its block buffer held for one block out of six
 * and was a coincidence. Each looked convincing and each cost a cycle.
 *
 * So this records the pair at the moment startNewTasks creates it:
 * igArchiveBlockTask::instantiateFromPool returns the task in r3 at
 * 0x21690f4, with the blocks allocated moments earlier still live in r19 and
 * r21. No offsets are guessed and nothing is derived from a delta.
 *
 * With the pairing known, the six held blocks can be attributed to real tasks
 * and those tasks read with ARCHQ's own verified field offsets rather than
 * invented ones. A task of 0 means instantiateFromPool failed, which would be
 * its own answer -- a block allocated for a task that was never created can
 * never be released by anything. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_tl[12][3]; /* task, blockA, blockB */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_tl_n = 0, g_ark_tl_calls = 0, g_ark_tl_nulltask = 0;

static inline void ark_tl_note(uint32_t task, uint32_t ba, uint32_t bb) {
    uint32_t i;
    g_ark_tl_calls++;
    if (!task) g_ark_tl_nulltask++;
    for (i = 0; i < g_ark_tl_n && i < 12u; i++)
        if (g_ark_tl[i][0] == task && g_ark_tl[i][1] == ba) return;
    if (i >= 12u) return;
    g_ark_tl[i][0] = task; g_ark_tl[i][1] = ba; g_ark_tl[i][2] = bb;
    g_ark_tl_n++;
}

/* RELFIX: return blockA to the cache when its task completes.
 *
 * Measured 2026-09-20, three counters agreeing: at the release gate
 * 0x21682a8, reached=60 skipped=55 released=5, every one of the 55 skips had
 * a non-null blockA at task+0x18, and all 55 of those blocks read state 1 --
 * aSt1=55, aSt2=0, aOther=0. So blockA is still in use and is walked past,
 * not released earlier and left as a stale pointer. Six blocks leak one at a
 * time until the pool is dry and no read can be issued again.
 *
 * The write is state 2, not 0. State 2 is the cached-and-evictable state that
 * ageBlocks ages and allocate falls back to when nothing is free, so the
 * block's data survives and the block merely becomes reusable under pressure.
 * State 0 would mark it empty and invite a reader to be handed a block whose
 * contents are still wanted. This is the conservative of the two.
 *
 * Honest about what this is: a shim-side workaround, not a port fix. Retail
 * either releases blockA on a path we never reach or never leaves it held,
 * and which of those is true is still unknown -- so this makes the symptom go
 * away without explaining the divergence. Behind a flag for that reason. */
#ifdef __GNUC__
__attribute__((weak))
#endif
/* OFF for one run: the 60-second baseline is deterministic (684,652 bytes,
 * 65 reads, 60 tasks -- six runs, identical), so it can finally be used to
 * A/B this fix instead of guessing from single 120-second runs, which are
 * not reproducible and never were. */
volatile uint32_t g_ark_relfix_enabled = 0, g_ark_relfix_applied = 0;

/* ITEMDONE: which work items reach the completion write, and which one waits.
 *
 * igPhysicalStorageDevice::complete writes the status byte at item+0x23 from
 * 0x2176b34, and PDEV shows it running 108-141 times per run and returning
 * every time -- so completions do happen and the write is not missing. Yet
 * the parked thread sits in igCafeSignal::wait for item 0x452cfc8, whose
 * status never changes, and exactly six tasks never reach the release gate.
 *
 * So the question is not "does anything write the status" but "does anything
 * write THIS item's status". This records every item complete() finishes,
 * with the value written, and the item the spin loop is waiting on. If the
 * waited item never appears, it is being skipped by the device loop and the
 * fix belongs wherever that list is walked. If it does appear, the write
 * lands and the waiter is missing it, which is a signal problem instead.
 *
 * Deliberately not guessing a fix here. Writing a status to +0x23 blind would
 * mark an item done whose buffer may not hold its data, and the decompressor
 * downstream would consume whatever is there -- a far worse failure than the
 * stall, and one that would look like corruption rather than a hang. Every
 * structural guess made tonight was wrong and every measurement was right. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_id[16][2]; /* item, last value written */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_id_n = 0, g_ark_id_calls = 0, g_ark_id_over = 0;

static inline void ark_id_note(uint32_t item, uint32_t val);

/* Per-site tally: which of the 29 status writers in the physical device TU
 * ever fire. complete()'s write at 0x2176b34 came back n=0 calls=0 -- the
 * function runs 108 times a run and never reaches that instruction -- so
 * "complete() marks the item" was wrong and the real writer is elsewhere. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_idsite[32][3]; /* site addr, calls, last value */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_idsite_n = 0;

static inline void ark_id_site(uint32_t site, uint32_t item, uint32_t val) {
    uint32_t i;
    for (i = 0; i < g_ark_idsite_n && i < 32u; i++)
        if (g_ark_idsite[i][0] == site) { g_ark_idsite[i][1]++; g_ark_idsite[i][2] = val; break; }
    if (i == g_ark_idsite_n && i < 32u) {
        g_ark_idsite[i][0] = site; g_ark_idsite[i][1] = 1u; g_ark_idsite[i][2] = val;
        g_ark_idsite_n++;
    }
    ark_id_note(item, val);
}

static inline void ark_id_note(uint32_t item, uint32_t val) {
    uint32_t i;
    g_ark_id_calls++;
    for (i = 0; i < g_ark_id_n && i < 16u; i++)
        if (g_ark_id[i][0] == item) { g_ark_id[i][1] = val; return; }
    if (i >= 16u) { g_ark_id_over++; return; }
    g_ark_id[i][0] = item; g_ark_id[i][1] = val; g_ark_id_n++;
}

/* BURST: microsecond timing of every block read.
 *
 * LOADCURVE showed all archive traffic landing inside one five-second window
 * at t=40-45s, and the run-to-run variance is the size of that single burst,
 * not a scatter of late events. The totals observed sit on a lattice -- 65
 * reads always, then 81, 113 or 177, i.e. extra batches of 16, 32 and 64 --
 * so something delivers work in doubling groups and stops after a variable
 * number of them.
 *
 * Five second sampling cannot see inside a four second burst. This records
 * the elapsed microseconds of each startBlockRead, which is ~177 stores a
 * run at most: cheap enough not to be the thing it measures, unlike the
 * per-block hook that cost 484,000 calls.
 *
 * What the shape will say. Evenly spaced reads that simply stop means the
 * burst is cut off by something external and the cutoff time is the thing to
 * explain. Reads in visible clusters with gaps between means the doubling
 * batches are real events, and the gaps are where to look. A long tail of
 * slowing reads means it is grinding to a halt rather than being stopped,
 * which would point back at the block pool draining. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_burst[256], g_ark_burst_n = 0;

static inline void ark_burst_note(unsigned long long ticks_ns) {
    if (g_ark_burst_n >= 256u) return;
    g_ark_burst[g_ark_burst_n++] = (uint32_t)(ticks_ns / 1000ull);  /* us */
}

/* RELTIME: when each block release happens, against when the reads stall.
 *
 * BURST showed the load is not one burst but clusters of reads separated by
 * long stalls -- 24, 333, 196, 85, 25 and 78ms in the run measured. Six gaps
 * over 20ms, and RELGATE reported released=6 in that same run. Across three
 * runs, released=5 gave 684,652 bytes and released=6 gave 815,724 and
 * 946,796.
 *
 * The reading that suggests: each stall ends when a block is handed back,
 * and since the only working release is the blockB path -- which exists only
 * on spanning reads, which are rare -- progress is quantised by how many of
 * those happen to occur. When they run out, the load stops for good.
 *
 * That is a correlation on three points and one burst trace, so it gets
 * tested rather than believed. Timestamping the releases puts them on the
 * same clock as the reads: if each one lands at the end of a stall, the
 * mechanism is confirmed and the fix is to make single-block tasks release
 * too. If they land anywhere else, the gaps are caused by something else and
 * this whole reading is wrong.
 *
 * Six entries a run. Free. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_reltime[16], g_ark_reltime_n = 0;

static inline void ark_reltime_note(unsigned long long ticks_ns) {
    if (g_ark_reltime_n >= 16u) return;
    g_ark_reltime[g_ark_reltime_n++] = (uint32_t)(ticks_ns / 1000ull);  /* us */
}

/* FSTIME: when each SD read starts and finishes, on the reads' own clock.
 *
 * BURST showed the load running in fast clusters of block reads separated by
 * stalls of 25, 86, 271 and 274ms. The first guess was that each stall ended
 * when a block was handed back, but RELTIME killed it: only two of four gaps
 * had a release near them and three releases landed mid-cluster.
 *
 * The durations themselves point somewhere far more ordinary. There are 7-8
 * FS async reads a run and 4-6 big gaps, and a quarter of a second is an
 * unremarkable time to pull 128KB off an SD card. If the gaps are simply the
 * card being read, then the clusters are "consume the buffer, wait for the
 * next one" -- normal behaviour, not a defect -- and the run-to-run variance
 * is just card latency deciding how many 128KB reads land before the pool
 * locks up.
 *
 * start and end are recorded separately so the read's duration is visible
 * rather than inferred. A gap that brackets a read exactly is the mundane
 * explanation confirmed; a gap with no read inside it is a real stall and
 * stays interesting. Sixteen entries a run. */
#ifdef __GNUC__
__attribute__((weak))
#endif
volatile uint32_t g_ark_fst[16][2], g_ark_fst_n = 0;   /* start us, end us */

static inline void ark_fst_begin(unsigned long long ns) {
    if (g_ark_fst_n < 16u) g_ark_fst[g_ark_fst_n][0] = (uint32_t)(ns / 1000ull);
}
static inline void ark_fst_end(unsigned long long ns) {
    if (g_ark_fst_n < 16u) { g_ark_fst[g_ark_fst_n][1] = (uint32_t)(ns / 1000ull); g_ark_fst_n++; }
}

#endif /* ARK_BLOCKPROBE_H */
