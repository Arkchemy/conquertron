# conquertron

The PowerPC32 → C static recompiler at the centre of [Arkchemy](https://github.com/Arkchemy).

It takes a legally-dumped Wii U copy of *Skylanders: Spyro's Adventure*, decodes
every real function out of the retail `.rpx`, and emits portable C. That C is
then compiled for ARM64 and packaged as Switch homebrew by
[jouster](https://github.com/Arkchemy/jouster) — there is no PowerPC emulation at
runtime.

This repository contains no game code or game assets, and none are required to
read it. You supply your own dump.

## What's in here

| Path | What it is |
| --- | --- |
| `src/main.cpp` | Driver: loads an `.rpx`, recovers functions, writes C |
| `src/elf_loader.*` | RPX/RPL parsing, relocations, zlib-compressed sections, synthetic address assignment |
| `src/disassembler.*` | Capstone-backed PowerPC decode |
| `src/func_recovery.*` | Function-boundary recovery |
| `src/codegen.*` | PowerPC → C translation |
| `include/ppc_runtime.h` | Guest CPU/memory model the generated C runs against |
| `include/cafeos_*.h` | Cafe OS (Wii U) runtime shims: coreinit, GX2, VPAD, sound, filesystem, … |

## Building

Requires CMake ≥ 3.16, a C++17 compiler, zlib, and [Capstone](https://www.capstone-engine.org/).

Capstone has no system package in this project's usual dev environment, so
point `CAPSTONE_PREFIX` at a local install if the default
(`~/devtools/capstone-install`) doesn't match your machine:

```bash
cmake -S . -B build -DCAPSTONE_PREFIX=/path/to/capstone-install
cmake --build build -j
```

That produces three binaries in `build/`:

- **`recomp`** — the recompiler itself.
- **`verify_vtable`** — ground-truth check: dumps what the real retail binary
  actually contains at a given relocation, cross-referenced against real
  function names.
- **`find_synth_addr`** — reverses a synthetic `PpcContext::mem` address (the
  kind you observe live on hardware) back to its real Wii U virtual address.

The two verification tools exist because guessing is expensive. Both were
written while root-causing real recompiler bugs — `verify_vtable` found a
relocation-folding bug in `ADDIC` by comparing a computed vtable address
against the real `.rodata`, and `find_synth_addr` identified a pool's real
vtable methods from a live address without needing another hardware test run.
They read a real dumped `.rpx` directly, with no emulator in the loop.

```bash
./build/verify_vtable    /path/to/tfbGame_cafe.rpx [dump_words]
./build/find_synth_addr  /path/to/tfbGame_cafe.rpx <synthetic-addr-hex> [byte-offset-hex] [dump-words]
```

## Status

Actively developed and **not** finished. Every function in the target binary
translates and links — 1,450,489 instructions, 100.00% of `.text`, with 28
bytes unrecovered as 7 isolated unhandled opcodes — but that is a statement
about coverage, not correctness. Silent codegen bugs are still being found and
fixed. Treat output as under active repair rather than trustworthy.

Bugs found and fixed so far, all of which produced wrong behaviour with no
crash, no warning and no unhandled instruction:

- record forms (`Rc` bit) not updating `CR0` — 6,745 sites
- `srawi` not setting the carry, breaking the standard signed-divide idiom —
  2,890 sites
- `divw`/`divwu` hitting C undefined behaviour on overflow and division by zero
- computed jump tables emitted as indirect *calls*, so every jump-table `switch`
  in the binary silently did nothing — 61 sites
- **only `.rela.text` was ever read.** Relocations targeting `.rodata` and
  `.data` were discarded outright — 48,625 of them, 33,260 pointing at
  functions. Those are every pointer *stored in* data: vtable slots, function
  tables, string pointers, pointers between globals. In a relocatable RPL the
  file holds zero in the slot and the relocation supplies the address, so
  dropping them left every such pointer null. This one was behind a long run of
  symptoms that each looked like a separate bug — null virtual calls, a path
  formatted as `"(null)/"`, 1,335 globals reading as never-written, five
  filesystem classes that never registered, and a poisoned allocator free list.

### Known design defect: guest-memory masking hides the bug

Every memory accessor masks its address with `(PPC_MEM_SIZE - 1)`, which is
what lets recompiled code use guest addresses directly. The cost is that a null
or wild pointer becomes a **legal** access instead of a fault, so the program
never stops where the bug is.

Traced in full on 2026-08-30: a null memory pool produced a null allocation,
element addresses computed from that null base came out as small integers —
`0xc`, `0x34D4`, `0x47430` — used as pointers, and an ordinary flag write four
bytes into one of them landed on the engine's memory-context global. Every
later read then treated that value as an object and called through it, giving
24,152 silent indirect calls. On real hardware the first bad write would have
crashed. Here it produced days of symptoms in unrelated subsystems.

Two mitigations exist: stores into the first 16 bytes are dropped rather than
performed (`ARKCHEMY_DROP_NULL_WRITES`), and `ppc_dispatch` counts the calls it
cannot resolve instead of returning silently. Neither is a fix for the design.

### Instrumentation: record `lr`, not the entry PC

`g_ppc_current_pc` is set when a recompiled function is entered and never
restored when it returns, so any probe reporting it names the innermost
function **entered** rather than the code responsible.

Over one investigation it named `_main` for a low-memory write,
`_savegpr_14_l` for 277,834 dispatch misses, `igMetaField::construct` — whose
entire body is a single `blr` — for null virtual calls, and a nine-instruction
getter for a store that corrupted a global. Four wrong answers from one field.

Every emitted call writes its own return address into `ctx->lr` on the line
above, so `lr` is exact. The dispatch-miss counter and the store watch both
record `lr` and the relevant registers now; inference had been choosing the
wrong function and, twice, the wrong argument.

### Instrumentation: a store watch cannot answer ordering questions

The store watch lives inside `ppc_store_u32`, so it sees only writes routed
through that helper. A memset shim, a memcpy, anything writing
`ctx->shared->mem` directly, is invisible to it. It also reports the most
recent write, which is right for catching a corrupting store and useless for
asking *when* a value changed.

Both limits bit at once. A global was written correctly at call 3,625 and read
back as zero at call 414,746, while the watch reported four writes whose latest
arrived at 428,352 — after the null had already been read, so a consequence
rather than the cause. Which write zeroed it, or whether any of them did, was
not answerable from what the watch kept.

`ppc_poll_watch_mem` reads the watched address at every function entry and
records each *transition* — call count, new value, and the `pc`/`lr` running at
the time — keeping the first several rather than only the latest. It sees
changes whoever made them. `ppc_sample_pc` shares that call site and records
every 4096th entry into a small ring, which is what names a loop: the
per-frame `last_pc` flickers between half a dozen addresses without saying
which loop they belong to.

### Known design defect: the synthetic/real address split

**Partly addressed.** `.text` now keeps its real address, which is measured and
good: the module-image range check went from a synthetic `0x183190` to
`0x02000020`, matching what Cemu reads from the retail game, and unresolved
indirect calls fell by 60% with the remainder landing inside real `.text`.

Extending the same treatment to `.rodata`, `.data` and `.bss` **broke the boot**
— class registration went from 99 to zero — and was reverted. The reasoning
still looks right, so what it collides with is not yet understood.

Data sections are packed into a synthetic address space from `0x2000` while
functions keep their real addresses, so a recompiled binary lives in two
address spaces at once. Any guest code that compares two pointers, or
range-tests one, gets a meaningless answer when the sides come from different
spaces — confirmed in the field against Cemu, where a module-image range check
came out inverted and caused every string literal in the game to be freed.

The justification recorded in the code is that guest memory was 4MB and real
Wii U addresses would not fit. It is 1GB now and they fit comfortably, so the
reason has expired. Written up with measurements in
[`docs/address-space-split.md`](docs/address-space-split.md).

### Validating against a reference

Cemu can be driven as an oracle for questions this recompiler's output cannot
answer on its own — what a global is *supposed* to contain, what a structure
really looks like at runtime:

```sh
flatpak run info.cemu.Cemu -g <game>/code/<title>.rpx --enable-gdbstub
```

```
set architecture powerpc:750
set endian big                    # required: without it gdb decodes backwards
target remote localhost:1337      # not gdb's default 1234
```

`set endian big` is not optional. Without it gdb both disassembles garbage and
writes byte-swapped breakpoint traps, so breakpoints silently never fire. The
stub has no detach, so restart the emulator between sessions.

## Licence

See [`LICENSE`](LICENSE) — Arkchemy Free & Source-Available License v2.0. It is
**not** an OSI-approved open source licence and some uses require permission,
so please read it before reusing anything here. Contact details and the
project Discord are in [`llms.txt`](llms.txt).

Contributors are listed in [`CONTRIBUTORS.csv`](https://github.com/Arkchemy/woodburrow/blob/main/CONTRIBUTORS.csv); the codename
scheme is explained in [`CODENAMES.md`](CODENAMES.md).
