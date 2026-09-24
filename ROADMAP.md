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
- [x] **Differential execution, instruction level** (2026-09-24).
      `hosttest/difftest` runs random instruction sequences under qemu-ppc
      and through recomp and compares all state. On its first day it found
      and this change fixed: ble/bge evaluated wrongly after a NaN compare,
      ten record forms that never set CR0, crset on any field but CR0, the
      fused multiply-add family rounding twice, fabs/fnabs on -0.0 and NaN,
      fctiwz out of range, and stfs rounding where it should truncate. CI
      runs three fixed seeds. See hosttest/difftest/README.md for what is
      deliberately not compared and what is not covered yet.
- [ ] **Differential execution, whole functions.** Run a recompiled unit on
      the host and compare against the same code under an emulator, on the
      same inputs -- the instruction-level fuzzer does not reach calls,
      indirect branches or the runtime's memory model.
- [ ] **A regression corpus.** Captured input traces replayed on every change.
      `hosttest/tlsf_replay.c` is the model: 804 real allocator calls, replayed
      in under a second, catching any divergence.

## The Cafe OS shims

The shims are where the last two root causes actually lived, and they get less
scrutiny than the translator because they are hand-written and look simple.

- [x] guest thread stacks reserve an EABI linkage area — a missing 16 bytes
      corrupted a heap and stalled boot for four sessions
- [x] OSEvent AUTO mode wakes exactly one waiter per signal, and a signal
      landing mid-pump is kept rather than lost (2026-09-24, EVCREDIT;
      pinned by seven sync_harness cases)
- [x] the FS completion queue is locked and the pumps claim atomically; the
      unlocked queue lost and duplicated completions under contention
      (2026-09-24). Next hardware run should say whether this moves the
      loading stall -- judge it on `served/back`, not bytes
- [ ] **audit every shim against real Cafe OS semantics**, especially anything
      that sets up guest register state: thread creation, TLS, callbacks,
      anything that fabricates a stack frame
- [ ] `memset`/`memcpy` bypass `ppc_store_u32`, so they are invisible to the
      store watch. Either route them through it under a debug flag, or make
      that limitation impossible to forget.
- [x] `setjmp`/`longjmp` are done on the host (2026-09-24). A recompiled
      `longjmp` restored guest registers but could not unwind the host call
      stack. Calls to them are now recognised by name and replaced -- the
      host `setjmp` runs inline in the caller's own C function, and `longjmp`
      jumps back to it with the guest register file restored. XenonRecomp's
      approach, adapted to C. Pinned by blaster's verify.sh at guest -O0 and
      -O1. **Unconfirmed against the retail binary:** the default names are
      `setjmp`/`_setjmp`/`__setjmp` and their `longjmp` twins; if GHS's
      libc spells them differently, pass `--setjmp-name`/`--longjmp-name`
- [x] GX2 is no longer "shims that mostly record and discard". As of
      2026-09-16 the path is real end to end: surfaces are sized and
      allocated, each one keeps its own deko3d image across re-binds, draws
      are recorded with full state, the scan target is honoured, and a
      composited frame reaches the display. deko3d's own pipeline counters
      confirm the geometry rasterising and passing the depth test.

      Five separate faults were between the engine and the screen, each
      hiding the next, all in this repository:

      - `GX2CalcSurfaceSizeAndAlignment` left every guest field untouched for
        `TILE_MODE_DEFAULT` on a 2D surface, so colour buffers were never
        allocated at all. One condition, `dim == 0u` widened to `dim <= 1u`.
      - Memory blocks were destroyed while commands recorded against them were
        still unsubmitted, which killed the GPU queue on the first frame that
        ever reached the scan buffer. Fixed with a retirement list drained only
        after a `dkQueueWaitIdle` that follows a submit.
      - The colour-image cache was keyed by render-target index, so the four
        surfaces the game actually drives -- two scan buffers and two
        ping-ponged 1024x576 offscreen targets -- shared one slot and each
        wiped the others. Now keyed by the guest surface descriptor.
      - `GX2CopyColorBufferToScanBuffer` ignored its scan-target argument by an
        explicit documented simplification, so the GamePad copy blitted over
        the composited TV frame every time. Harmless while no copy ever ran.
      - A render target bound as a texture was overwritten with guest memory,
        which for a GPU-written surface is always zero. Same wrong assumption
        as the colour-buffer case, one step further along the frame.

      The common thread is worth stating: **four of the five came from
      treating the guest surface as the source of truth for memory only the
      GPU ever writes.** That is a shim-design principle, not five bugs.

- [ ] The per-pixel upload loops (`ppc_load_u8` a byte at a time, in
      `GX2SetColorBuffer`, `GX2CopyColorBufferToScanBuffer` and
      `arkchemy_gx2_set_texture`) are measured at 5% of a run. Ugly, and not
      the bottleneck -- they were nearly rewritten first on the assumption
      they were. Worth doing, worth doing after the things that are.
- [ ] Nothing here has been tested above `dim <= 1`. 3D and cube surfaces have
      layout rules the tiling fix deliberately did not touch.

## The thing nobody can explain yet

Buffering the diagnostic log's stdio stream stops the recompiled game booting.

Not a subtle degradation -- the boot takes a different, much shorter path, and
the difference is stark and perfectly reproducible on identical binaries with
one setting changed from the SD card:

    79d8ac6c  flush=256  frames=14400 draws=0   modules=2
    79d8ac6c  flush=1    frames=14400 draws=284 modules=7
    79d8ac6c  flush=1    frames=14400 draws=345 modules=7

Ruled out by measurements that addressed each directly: the flush cadence (the
cost was paid back and the boot stayed broken), the buffer living in BSS (moved
to the heap, still broken), run length, and main-thread scheduling (14,340
frames of 8ms sleep is 115 seconds against the 108 seconds of flushing it
replaced, and it changed the guest's work by 0.14%).

It costs 31% of every run, since the log cannot be buffered until it is
understood. But the interesting part is not the 31%. **A recompiled program
should not be able to tell whether the host's logging is buffered.** That it
can means something in this runtime depends on a timing or scheduling property
nobody chose, and that is a conquertron-level fact rather than a jouster one.

- [ ] Attribute `checkpoint()` calls by thread. The guest thread calls it too,
      so the per-line `fflush` is a blocking syscall on *that* thread as well
      as the harness's. If nearly all calls turn out to be main-thread, this
      dies immediately and filesystem contention becomes the leading
      explanation -- the game reads its archives off the same SD card.
- [ ] Then, if it is the guest thread: make it block periodically from a Cafe
      OS shim with no logging involved, and see whether that substitutes.

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
