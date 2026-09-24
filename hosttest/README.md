# hosttest

Runs recompiled translation units natively, without a Switch.

`ppc_runtime.h` needs only math/stdint/stdlib/string, and recompiled code
touches guest memory exclusively through `ppc_load`/`ppc_store`, so a
self-contained unit like the TLSF allocator has no idea it is not on a Wii U.

## Why

The allocator bug was being chased through build -> copy 176 MB over MTP ->
launch -> pull log, roughly ten minutes per question. The same question here
costs milliseconds, is deterministic, and can be stepped in a debugger.

## Running

    sh conquertron/hosttest/run.sh

`stubs.c` aborts loudly on any symbol the unit references but the code under
test should never reach, so an unexpected call is reported rather than
silently returning a plausible value.

## sync_harness.c

    gcc -O1 -g -w -I include \
        hosttest/sync_harness.c include/cafeos_state.c \
        -o /tmp/sync_harness -lpthread -lm && /tmp/sync_harness

Exits non-zero if any semantic check fails. Worth also running once under
ThreadSanitizer (`-fsanitize=thread`) after touching the shims; the only races
it should report are the unlocked diagnostic counters (`g_ark_sev_*`,
`g_ark_wev_*`, `g_arkchemy_event_*`), which are deliberately unsynchronised.

Drives the real event, mutex, semaphore and FS-completion shims -- not a
model of them -- with a watchdog per case, so a deadlocked case is reported
with the phase counters rather than hanging the run.

It found a bug three hardware cycles had missed. `ppc_fs_invoke_async_callback`
sets r3-r6 to the guest callback's arguments and never restores them, so
`arkchemy_fs_pump_completions` destroyed the argument of whichever import
called it. Five of the six pump call sites read `ctx->r[3]` on the very next
line:

| import | what it did instead |
| --- | --- |
| `OSLockMutex` | locked the **wrong mutex** |
| `OSWaitEvent` | waited on the wrong event |
| `OSWaitEventWithTimeout` | wrong event, and r5:r6 is its 64-bit timeout, so the deadline was garbage too |
| `OSWaitSemaphore` | waited on the wrong semaphore |
| `OSTryWaitSemaphore` | likewise |

Locking the wrong mutex is the frightening one: it does not hang, it quietly
fails to exclude, and the damage lands somewhere else entirely.

On hardware this showed as the game thread stopping at an identical
`GAMEPC` count of 2,877,916 across three different builds. Three probes and
three console cycles narrowed it to "somewhere in OSSignalEvent"; the harness
answered it in minutes and gave the blast radius for free.

## 2026-09-24: two more shim bugs, both on the loading path

**AUTO events released every waiter.** Each parked waiter compared one
per-event epoch that every signal bumped, so a single `OSSignalEvent` on an
AUTO event released all of them, and a second signal landing before the first
woken waiter ran was lost outright. EVSTATE had measured two waiters parked on
the loading event, so this was live. Replaced by a count of wake credits;
see "EVCREDIT" in `cafeos_coreinit_sync.h`. Three new cases fail on the old
code: one signal releasing both of two waiters, a second signal vanishing,
and two timed waiters both being told TRUE.

**The FS completion queue had no lock.** It is filled by whichever thread
issues a read and drained by whichever thread next pumps, and the pump's
re-entrancy flag was a plain int two threads could both see clear. Four
producers and four pumpers, 16,000 completions: the unlocked queue delivered
one completion twice, then wrapped its count to 4,294,967,295 -- after which
it reads as permanently full and every later completion is dropped. A
completion that never runs is a work item that never reaches status 2 and a
block that is never released, which is the shape of the loading stall.
Whether it is *the* cause is for a hardware run to say; the race itself is
not in doubt. The queue now has a mutex, and both pump guards are atomic
claims.

The Sep 16 hardware log reports `skip=0` and `dropped=0` on every line, so
neither the message-queue completion gap nor a full queue was losing reads in
that run.

## Result so far

`tlsf_create` builds the full 5 MB arena and `tlsf_memalign` is correct in
isolation at every alignment from 4 to 4096: the returned pointer is properly
aligned, the block size is right, and the chain still reaches the sentinel.
The hardware fault therefore needs prior heap state, not a bad instruction in
`tlsf_memalign`.
