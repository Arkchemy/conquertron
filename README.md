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

Actively developed and **not** finished. Every real function in the target
binary translates and links, but that is a statement about coverage, not
correctness — real silent codegen bugs are still being found and fixed. Treat
output as under active repair rather than trustworthy.

## Licence

See [`LICENSE`](LICENSE) — Arkchemy Free & Source-Available License v2.0. It is
**not** an OSI-approved open source licence and some uses require permission,
so please read it before reusing anything here. Contact details and the
project Discord are in [`llms.txt`](llms.txt).

Contributors are listed in [`CONTRIBUTORS.csv`](CONTRIBUTORS.csv); the codename
scheme is explained in [`CODENAMES.md`](CODENAMES.md).
