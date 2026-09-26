# difftest

Differential fuzzing of the recompiler against a real PowerPC.

Random sequences of PowerPC instructions are assembled twice: into a PowerPC
Linux program run under `qemu-ppc`, and into an object that goes through
`recomp`, is compiled for the host and run natively. After each sequence,
every piece of state it can touch is compared:

- r4 to r11
- all 32 bits of CR
- XER[CA]
- f1 to f6
- 64 bytes of scratch memory

A mismatch is shrunk to the fewest instructions that still reproduce it and
printed with its inputs.

This is ROADMAP.md's "differential execution", at the instruction level. It
exists because every recompiler bug found so far was silent, and coverage
cannot see silent bugs.

## Running

    cmake --build build
    RECOMP=build/recomp python3 hosttest/difftest/difftest.py --seed 7 --programs 100 --inputs 8

Needs zig (`ZIG=`, default `~/devtools/zig/zig`) and `qemu-ppc-static`
(`QEMU_PPC=`). Set `DIFFTEST_UBSAN=1` to build the recompiled side with
UndefinedBehaviorSanitizer, and `DIFFTEST_VALGRIND=1` to run recomp itself
under valgrind. CI runs with both.

A seed always generates the same programs and inputs, so a failure is
reproduced by its seed. CI runs seeds 1–3; other seeds are for exploring.

## What it generates

- **Integer:** arithmetic, logical, rotate and shift, with random record (`.`) forms.
- **Carry:** `addc`/`adde`/`addze`/`addme`/`subfc`/`subfe`/`subfze`.
- **Multiply and divide.**
- **Compares:** `cmpw`, `cmplw`, `fcmpu`, `fcmpo` into any CR field.
- **CR:** `mtcrf`, `mfcr`, `mcrf`, and `cror`/`crset`/`crclr`/`crmove` on any bit.
- **Loads and stores:** byte, half and word, including indexed, byte-reversed, update (`lwzu`) and indexed-update (`lwzux`) forms.
- **`lmw`/`stmw`.**
- **CTR and LR moves.**
- **Branches:** forward conditional branches on any condition and field, including `bso`/`bns`/`bun`/`bnu`, and counted `bdnz` loops.
- **Calls:** `bl`, calls through function pointers (`bctrl`), tail calls, non-leaf callees with frames of their own.
- **Switches:** jump tables of branches after the `bctr`, and of addresses in `.rodata`.
- **Floating point:** double and single arithmetic, the whole fused multiply-add family, `fsel`, `fabs`/`fnabs`/`fneg`/`fmr`, `frsp`, `fctiwz` with `stfiwx`, and `lfs`/`lfd`/`stfs`/`stfd`.

The test frame saves r14–r31 and zeroes them, so `lmw`/`stmw` see identical
registers on both sides, and it restores them on the way out.

## What it found on its first day (2026-09-24)

All fixed in the same change. Each one was silent: no crash, no unhandled
instruction, just a wrong value.

| Instruction | What was wrong |
| --- | --- |
| `ble`, `bge`, `blelr`, `bgelr` | Evaluated as LT‖EQ and GT‖EQ instead of "not GT" and "not LT". The two agree after an integer compare, but not after `fcmpu` with a NaN, where the hardware takes both branches. `!(a < b)` on a NaN went the other way. |
| `mulhw.`, `mulhwu.`, `addc.`, `subfc.`, `subfze.`, `mr.`, `slwi.`, `srwi.`, `rotlw.`, `rotlwi.` | Record forms that never updated CR0: ten instructions. `or.` missing it once hung a real game function (see PPC_INS_AND in codegen.cpp). There is now a backstop after the per-instruction switch, so a future case cannot forget. |
| `crset` | Only handled CR0. `crset 26` (CR6[EQ]) did nothing. |
| `fmadd`, `fmsub`, `fnmsub` | Rounded twice, `a*c+b` in C, instead of fused. Off by an ulp. |
| `fnmsub`, `fnmsubs` | `b - a*c` instead of `-(a*c - b)`, which gets the sign of an exact zero wrong. |
| `fmadds`, `fmsubs`, `fnmsubs` | fma-then-frsp rounds twice and can break a single-precision tie the wrong way. Now exact for single-precision operands (see `ppc_fmadds`). |
| `fabs`, `fnabs` | `x < 0 ? -x : x` leaves `-0.0` negative and never touches a NaN's sign. Now sign-bit operations. |
| `fctiwz` | A C `(int32_t)` cast, which is undefined out of range. PowerPC saturates, and NaN gives `0x80000000`; x86 and ARM64 each disagree with that differently. |
| `stfs` (and `psq_st` of floats) | Rounded like a C `(float)` cast. The architecture truncates the mantissa. |
| `neg.` | Emitted `-(int32_t)x`, which is signed overflow (undefined behaviour) for `INT_MIN`. GCC used that to fold the CR0 compare to GT where hardware sets LT. Found at seed 25; `DIFFTEST_UBSAN=1` now builds the recompiled side with UndefinedBehaviorSanitizer so undefined behaviour fails even when the value happens to match. CI runs with it on. |

## Second round (2026-09-25)

**CR bit 3 is now modelled.** It is SO after an integer compare and FU
(unordered) after `fcmpu`. The recompiler used to drop it everywhere. So:

- `fcmpu` against a NaN left no trace;
- `mfcr` lost the bit;
- `bun`/`bnu`/`bso`/`bns` could not be recompiled at all.

It is a `cr_so[8]` array now, carried by compares, `mfcr`, `mtcrf`, `mcrf`,
`stwcx.` and the CR logic ops, and the four branches (and their `lr` forms)
are supported. The fuzzer compares the whole CR.

Found along the way:

| Instruction | What was wrong |
| --- | --- |
| `fcmpo` | Capstone 5.0.3 has no id for it and fails to decode the word, which ends disassembly of the whole function at that point. It is now decoded by conquertron's own fallback decoder, as `fcmpu` with its own mnemonic: the CR result is the same, and the two differ only in FPSCR exception bits. |
| `ps_cmpu0/1`, `ps_cmpo0/1` | Any CR field other than CR0 was a silent no-op. qemu has no paired singles, so this was found by reading the code, not by the fuzzer. |

## Third round (2026-09-25): recomp was not deterministic

Seeds 43, 47 and 48 failed in a batch run of seeds 40–49 and passed when run
alone. The generated C had `mr r8, r30` followed by a CR0 compare, which is
an `mr.`.

The cause was in Capstone 5.0.3. `PPC_post_printer` sets `update_cr0` by
looking for a `.` in `insn->mnemonic`, but it runs before the mnemonic is
written. It was reading whatever the heap held, so any instruction could
become its own record form, depending on what an earlier allocation left
behind. Every CR0 update recomp emitted rested on that flag. The record-form
backstop from the first round only made it easier to see.

conquertron now:

- takes Rc from the instruction word (`is_record_form` in `disassembler.cpp`);
- clears the mnemonic before each Capstone call, which also makes Capstone's
  branch-hint field deterministic;
- zeroes the detail before its own paired-single decoder.

`DIFFTEST_VALGRIND=1` runs recomp under valgrind and fails on any error. It
fails on the old code and passes on the new, and CI runs with it on.

FP record forms (`fadd.` and friends) set CR1 from the FPSCR, which is not
modelled. recomp now prints a warning for each one instead of saying
nothing.

## Fourth round (2026-09-26): across function boundaries

The fuzzer used to stay inside one function. Now each program has two
helper functions of its own. The first is a leaf. The second is one of:

- another leaf;
- a non-leaf with its own stack frame that calls the leaf;
- a tail call: a plain `b` into the leaf.

Bodies call them with `bl`, and through function pointers with
`mtctr`/`bctrl`. Bodies also run four-way switches in both shapes compilers
emit: a table of branches straight after the `bctr`, and a table of case
addresses in `.rodata` loaded with `lwzx`. The frame saves LR so the body
may call.

| What | What was wrong |
| --- | --- |
| Tail calls (`b` to another function) | In an unlinked object, a `b`'s displacement is 0 until the relocation is applied, so its raw target is the `b` itself. That is inside the function, so recomp emitted `goto` itself: an infinite loop. The relocation now decides before the raw target does. |
| Switches through a table of addresses | recomp only understood a table of branches after the `bctr`. A jump through a table of addresses in `.rodata` went to `ppc_dispatch`, which knows only function entries. The lookup missed, and the function returned halfway through without running the case. Every address-taken label inside a function is now a `bctr` target. The loader's own notes had already recorded this shape in retail: an address next to `__gh_vsprintf` that `ppc_dispatch` was asked to call 278,093 times. |
| `fnmadd`, `fnmadds` | Not supported at all. |
| `fnmsub`, `fnmsubs` (and the new `fnmadd`s) | Negated a NaN result. PowerPC leaves a NaN's sign alone in these instructions. |
| `lfs` (and every single-precision load) | Widened with a C `(double)` cast. That is an arithmetic conversion, and it quiets a signaling NaN. A load is data movement, and PowerPC moves the bits as they are. Compilers copy float fields, structs and unions through FPRs, so any 32-bit value shaped like a signaling NaN came back changed: `lfs; stfs` turned `0x7fb40eb5` into `0x7ff40eb5`. |

Found while adding a compiled-C test of the jump table to blaster:

- **A guest function named like a runtime function did not compile.** A
  function called `dispatch` became a second `ppc_dispatch`.
- **The fix.** `tools/gen_reserved_names.py` collects every `ppc_*`
  identifier in `include/` at build time. recomp renames any colliding
  guest function to `<name>_guest` and says so.
- **Pinned in blaster.** `switch_table` and `name_collision` fail on the old
  code and pass now.

`DIFFTEST_TARGET=arm64` runs the recompiled side on ARM64 under
qemu-aarch64 instead of natively on x86. ARM64 is what the Switch runs, and
its default NaN is PowerPC's, so in that mode NaNs are compared exactly,
sign and payload included, and nothing is masked. CI runs one seed that way.

Its first run found 16 mismatches across three seeds, all in the fused
multiply-add family:

| What | What was wrong |
| --- | --- |
| Which NaN comes out | PowerPC returns the first NaN of frA, frB, frC, quieted, with its sign and payload kept; single forms keep only a single's payload. `fma()` makes its own choice, and `fmsub`'s `fma(a, c, -b)` had already flipped a NaN b's sign. Now `ppc_fmadd`/`ppc_fmsub`/`ppc_fmsubs` apply the rule first. All 40 cases of a probe covering the eight ops and every operand position match qemu bit for bit. |
| `fnmadd` of an exact zero | clang compiles `-fma(a, c, b)` to ARM64's `fnmadd`, which is `-(a*c) - b`. That agrees with `-(a*c + b)` except in the sign of an exact zero: `-(-663088.4 * 0 + 0)` is `-0` on PowerPC, and the fused form gave `+0`. The negation is now done on the sign bit behind an empty `asm`, so it cannot be folded in. x86 did not show this, because its compiler does not make the same fold. |

## Deliberately not compared

Each of these is either undefined in the architecture or outside what the
recompiler models, and each is written down where it is excluded:

- **XER[SO], and so the overflow (`o`) instruction forms.** Nothing the recompiler supports sets XER[SO], so integer compares copy a 0 into CR bit 3. The bit itself is modelled and compared; see the second round below.
- **The upper word of an FPR after `fctiwz`.** It is undefined; qemu sign-extends and the recompiler leaves 0. The stored integer is compared.
- **NaN sign and payload.** An invalid operation's default NaN is positive on PowerPC and ARM64 (the Switch) and negative on x86, where this runs. This applies to FPRs, and to NaN bit patterns in memory and in GPRs (a stored NaN can be loaded back into one), which are compared without their sign.
- **Single-precision add, subtract, multiply and divide with double-precision operands.** They round twice. Compiled code feeds these instructions single-precision values, and for those the double intermediate is provably enough, so the fuzzer does the same.
- **Division by zero and `INT_MIN / -1`.** Their results are undefined; divisors are forced odd, and positive for `divw`.

## Not covered yet

- **Paired singles.** qemu does not implement them. LLVM #211463 will let them be assembled; a reference would still be needed.
- **Anything reading or writing r3**, which holds the state pointer.
- **Calls into imported functions (the Cafe OS shims)**, and recursion.
