# conquertron roadmap

The recompiler translates 100% of the target's `.text` — 1,450,489
instructions, with 28 bytes unrecovered as 7 isolated unhandled opcodes. That
is a coverage statement, not a correctness one, and the work below reflects
that distinction.

## Correctness before coverage

Every bug found so far has been a **silent** one: wrong behaviour, no crash, no
warning, no unhandled instruction. Coverage cannot find these; only differential
testing and careful reading can.

- [ ] **Systematic instruction audit.** Each unusual opcode checked against real
      PowerPC semantics rather than assumed. Already caught and fixed:
      `srawi`'s carry (which `addze` consumes), `slw`/`srw`'s 6-bit shift rule
      where the result is 0 for 32–63, wrapping `rlwinm` masks where MB > ME,
      `lwzu`'s base-register update.
- [ ] **Differential execution.** Run a recompiled unit on the host and compare
      against the same code under an emulator, instruction for instruction, on
      the same inputs.
- [ ] **A regression corpus.** Captured input traces replayed on every change.
      `hosttest/tlsf_replay.c` is the model: 804 real allocator calls, replayed
      in under a second, catching any divergence.

## The Cafe OS shims

The shims are where the last two root causes actually lived, and they get less
scrutiny than the translator because they are hand-written and look simple.

- [x] guest thread stacks reserve an EABI linkage area — a missing 16 bytes
      corrupted a heap and stalled boot for four sessions
- [ ] **audit every shim against real Cafe OS semantics**, especially anything
      that sets up guest register state: thread creation, TLS, callbacks,
      anything that fabricates a stack frame
- [ ] `memset`/`memcpy` bypass `ppc_store_u32`, so they are invisible to the
      store watch. Either route them through it under a debug flag, or make
      that limitation impossible to forget.
- [ ] `setjmp`/`longjmp` are recompiled PowerPC. A `longjmp` restoring guest
      registers cannot unwind the **host** call stack the recompiled code runs
      on. Not yet implicated in a real bug, but it cannot work as written and
      will matter to any code using it for error handling.
- [ ] GX2 is 140 shims that mostly record and discard. That is fine until
      rendering is real; see jouster's roadmap.

## The memory model

- [ ] `PPC_MEM_SIZE` is a flat power-of-two array with every access masked. It
      works, but it cannot detect an out-of-bounds guest access — the very class
      of bug that stalled boot. A guard-page or range-check mode, even a slow
      one, would have caught that in minutes.
- [ ] the address-space split is documented in `docs/address-space-split.md`;
      globals are relocated to synthetic addresses while `.text` addresses stay
      real. That asymmetry has caused repeated confusion and deserves a helper
      rather than a convention.

## Tooling

- [x] `hosttest/` — run recompiled units natively, in milliseconds
- [x] `find_synth_addr` — map a synthetic address back to the real binary
- [ ] extend `hosttest` beyond the allocator: any self-contained unit should be
      runnable there
- [ ] a symbol-aware log decoder, so `lr=0x217cc58` resolves without a manual
      lookup every time

## Documentation

- [x] `docs/probe-design.md` — how to instrument this codebase without wasting
      a hardware cycle
- [ ] a codegen reference: which PowerPC constructs translate to what, and
      which are known-hard
