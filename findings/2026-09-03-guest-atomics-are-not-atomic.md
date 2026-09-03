# Guest atomics are not atomic, and there are no memory barriers

**Status:** structural defect in the runtime, found 2026-09-03.
**Scope:** every recompiled program that uses more than one guest thread.
**Severity:** silent. Nothing crashes; work is simply never observed.

## What the runtime does today

Guest memory is a plain array:

```c
typedef struct PpcSharedMemory {
    uint8_t mem[PPC_MEM_SIZE];
} PpcSharedMemory;
```

Not `volatile`, not `_Atomic`. Every access goes through plain byte loads:

```c
static inline uint32_t ppc_load_u32(const PpcContext *ctx, uint32_t addr) {
    const uint8_t *p = &ctx->shared->mem[addr & (PPC_MEM_SIZE - 1)];
    return ((uint32_t)p[0] << 24) | ... ;
}
```

The reservation instructions are translated as ordinary accesses:

```
/* 21690d8: lwarx r9, 0, r6 */
ctx->r[9] = ppc_load_u32(ctx, 0u + ctx->r[6]);
/* 21690e4: stwcx. r11, 0, r6 */
ppc_store_u32(ctx, 0u + ctx->r[6], ctx->r[11]);
ctx->cr0_lt = 0; ctx->cr0_gt = 0; ctx->cr0_eq = 1;
```

`stwcx.` sets `cr0_eq = 1` **unconditionally** -- "the store-conditional always
succeeded" -- so the `bne` retry loop always falls through. And `sync`,
`lwsync`, `isync` and `eieio` are not emitted at all.

Single-threaded, all of this is correct and cheap. The reservation loop
degenerates to a plain read-modify-write, which is what it means with one
thread.

## Why it stopped being correct

`cafeos_coreinit_thread.h`'s `OSCreateThread` spawns a **real host pthread**
running against a fresh `PpcContext` that shares the **same**
`PpcSharedMemory` as its creator -- deliberately, because real Wii U threads
share one address space.

So the moment a guest program creates a second thread, two real CPU cores are
performing unsynchronised read-modify-write on the same plain array, with:

- **no atomicity** -- concurrent RMW loses updates (refcounts, queue indices,
  free-list heads)
- **no failure path** -- `stwcx.` never reports contention, so the "someone
  else won the race, retry" branch is dead code
- **no ordering** -- and the host is **ARM64, which is weakly ordered**. A
  write by one thread carries no guarantee of becoming visible to another
  without a barrier. PowerPC is weakly ordered too, which is exactly why the
  original code contains the barriers we are dropping.

The last point is the dangerous one. On x86 this would mostly paper over
itself; on ARM64 it does not.

## The failure it appears to be causing

Skylanders' job queue (`Core::jqWorkerLoop`) is a producer/consumer queue
across threads. Measured on hardware:

- `igJobQueue::start` runs, `Core::_jqStart` runs, two worker threads are
  created and both enter `Core::jqWorkerThread`
- the game thread submits exactly one batch via `igJobQueue::addBatch`
- the workers wake ~286 times over a 420-second run (timed sleep expiring)
- the batch is **never executed**

Workers alive and polling, work present, work never seen. That is precisely
the shape of a missing release/acquire pair between producer and consumer.

## What a fix looks like

Roughly in order of cost and value:

1. **Make guest memory accesses ordered.** At minimum the store side needs a
   release and the load side an acquire where the guest had a barrier.
   Emitting `sync`/`lwsync`/`eieio` as `__atomic_thread_fence` is the direct
   translation and is nearly free on the fast path.
2. **Implement `lwarx`/`stwcx.` properly** with a per-context reservation
   address and a real compare-and-swap, and set `cr0_eq` from whether the CAS
   succeeded. This makes the retry loops mean what they say.
3. **Reconsider plain `uint8_t mem[]`.** Non-volatile access lets the compiler
   keep guest memory in registers across a guest loop body, which is fine
   single-threaded and wrong across threads.

Do not "fix" this by serialising guest threads. The threading shim spawning
real pthreads is the right design; the memory model underneath it is what is
incomplete.

## Honest caveat

This is a strong hypothesis with a concrete mechanism, not yet a proven
diagnosis of the job-queue stall. It predicts what has been measured, and the
next hardware probe (`jqPopNextBatch` / `jqWorkerSleep` hit counts) adds
evidence either way. It is worth fixing regardless of whether it turns out to
be *this* bug, because it is unsound for any multithreaded title.
