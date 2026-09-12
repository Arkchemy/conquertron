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

    gcc -O1 -g -w -I include -I ../jouster/game/include \
        hosttest/sync_harness.c include/cafeos_state.c \
        -o /tmp/sync_harness -lpthread -lm && /tmp/sync_harness

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

## Result so far

`tlsf_create` builds the full 5 MB arena and `tlsf_memalign` is correct in
isolation at every alignment from 4 to 4096: the returned pointer is properly
aligned, the block size is right, and the chain still reaches the sentinel.
The hardware fault therefore needs prior heap state, not a bad instruction in
`tlsf_memalign`.
