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

## Result so far

`tlsf_create` builds the full 5 MB arena and `tlsf_memalign` is correct in
isolation at every alignment from 4 to 4096: the returned pointer is properly
aligned, the block size is right, and the chain still reaches the sentinel.
The hardware fault therefore needs prior heap state, not a bad instruction in
`tlsf_memalign`.
