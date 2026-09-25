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
- **Floating point:** double and single arithmetic, fused multiply-add, `fsel`, `fabs`/`fnabs`/`fneg`/`fmr`, `frsp`, `fctiwz` with `stfiwx`, and `lfs`/`lfd`/`stfs`/`stfd`.

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

## Deliberately not compared

Each of these is either undefined in the architecture or outside what the
recompiler models, and each is written down where it is excluded:

- **XER[SO], and so the overflow (`o`) instruction forms.** Nothing the recompiler supports sets XER[SO], so integer compares copy a 0 into CR bit 3. The bit itself is modelled and compared; see the second round below.
- **The upper word of an FPR after `fctiwz`.** It is undefined; qemu sign-extends and the recompiler leaves 0. The stored integer is compared.
- **NaN sign and payload.** An invalid operation's default NaN is positive on PowerPC and ARM64 (the Switch) and negative on x86, where this runs.
- **Single-precision add, subtract, multiply and divide with double-precision operands.** They round twice. Compiled code feeds these instructions single-precision values, and for those the double intermediate is provably enough, so the fuzzer does the same.
- **Division by zero and `INT_MIN / -1`.** Their results are undefined; divisors are forced odd, and positive for `divw`.

## Not covered yet

- **Paired singles.** qemu does not implement them. LLVM #211463 will let them be assembled; a reference would still be needed.
- **Anything reading or writing r3**, which holds the state pointer.
- **Indirect branches.**
