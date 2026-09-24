# Prior art, September 2026 update

A follow-up to the "External prior art — useful repos" note (last edited
2026-09-12), which already covers igRewrite8, re_nsyshid, Texthead1,
spyrosadventure, hYdos, NefariousTechSupport, LG-RZ, decaf-emu,
kinnay/Nintendo-File-Formats, CafeGLSL and the GX2 shader samples. Nothing
from that page is repeated here.

Same rule as that page: **the licence decides whether a project can be built
on or only read.** Arkchemy's own licence is permission-required, so anything
GPL can be studied but not copied in.

Researched 2026-09-24. Star counts and commit counts are as of that day.

## The headline: other people are statically recompiling Wii U code now

When the prior-art page was written, the only other Skylanders
recompilation effort was an empty placeholder (`sky2015-recomp`). There are
now at least three Wii U static recompilers in public, one of them
running a real game.

| Project | Licence | State | Worth |
| --- | --- | --- | --- |
| [BlackLineInteractive/nWiiURecomp](https://github.com/BlackLineInteractive/nWiiURecomp) | **GPL-3.0** | *Wind Waker HD* EU v0 "authenticates, maps its sections and relocations, initializes Cafe ABI state, and runs deterministically through cooperative startup". One validated title. 6★, 11 commits. | **Read, compare notes.** The closest project to conquertron that exists: RPX in, C++ out, an HLE Cafe OS runtime, and an Espresso interpreter as a fallback for code it did not compile. It says it runs "deterministically through cooperative startup" -- the same cooperative-scheduling territory the loading stall lives in. Spun out of [NWiiRecomp](https://github.com/BlackLineInteractive/NWiiRecomp) (GameCube/Wii, 25★, 226 commits). |
| [ApfelTeeSaft/RebrewU](https://github.com/ApfelTeeSaft/RebrewU) | none stated | RPX/RPL → C++. Function discovery, CFGs, jump-table detection; tools to inspect sections, relocations, imports, exports. 5★, 5 commits. | **Read only** (no licence = all rights reserved). Its jump-table detection is the one piece worth comparing against. |
| [chrissotraidis/DolRecomp](https://github.com/chrissotraidis/DolRecomp) | **GPL-3.0** | GameCube/Wii recompiler with an "experimental" RPX frontend its authors say is not maintained. 236 opcodes; *Luigi's Mansion* reaches its title screen. | **Read.** Its decoder, CPU-behaviour and RPX tests are a second opinion on instruction semantics -- useful for the instruction audit in ROADMAP.md, where every bug so far has been a silent one. |
| [hardkiller2565123123/Wii-U-Recomp](https://github.com/hardkiller2565123123/Wii-U-Recomp) | not yet | Announced, no source. | Watch. |

None of these ports a game to the Switch, and none targets Skylanders or
Alchemy. They are peers, not overlap -- but the people behind nWiiURecomp in
particular are the ones most likely to have already met whatever
conquertron meets next.

## The best permissively-licensed reference for conquertron itself

### [hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — MIT

The Xbox 360 recompiler behind the *Unleashed Recompiled* port. The Xbox
360's CPU is PowerPC, so most of the hard problems are the same ones, and
**it is MIT: it can be built on, with attribution.** Three things in it map
directly onto open items:

- **`setjmp`/`longjmp`.** ROADMAP.md: "a `longjmp` restoring guest registers
  cannot unwind the host call stack the recompiled code runs on." XenonRecomp's
  answer is to not recompile them at all: calls to the guest's `setjmp`/
  `longjmp` are redirected to the host's own, with guest CPU state saved
  alongside. That is the known-good shape for the fix here.
- **Jump tables.** It detects `mtctr` … `bctr` patterns and turns them into C
  `switch` statements, with per-game TOML for the cases the pattern misses.
  Its README is candid that there is "no fully generic solution".
- **Mid-assembly hooks.** Calls to host functions injected at chosen guest
  addresses, with register arguments, and options to return or branch
  afterwards. conquertron's probes (ark_blockprobe.h) are hand-rolled
  versions of the same idea; a general mechanism would stop each probe
  needing its own codegen touch.

It also documents a subtlety conquertron should check: the FPU leaves
denormals alone while the vector unit flushes them. The Espresso has no VMX,
but it does have paired singles, which have their own rounding behaviour.

## Cafe OS semantics: where to check a shim against

### [cemu-project/Cemu](https://github.com/cemu-project/Cemu) — MPL-2.0

Already cited in the shim comments. Two findings from reading it on
2026-09-24:

1. **The event fix is right.** `coreinit_Synchronization.cpp`: in AUTO mode,
   `OSSignalEvent` latches if the wait queue is empty and otherwise calls
   `wakeupSingleThreadWaitQueue` -- one thread. `OSSignalEventAll` latches if
   empty and otherwise wakes the whole queue. `OSWaitEventWithTimeout`
   returns false on timeout and true on a signal. That is exactly what the
   EVCREDIT rewrite implements and what sync_harness now pins.
2. **Real FS callbacks run on dedicated threads.** `coreinit_FS.cpp`
   comments that on hardware async FS completion processing "delegates the
   processing to the AppIO threads" (Cemu itself runs it on its IPC thread).
   conquertron runs callbacks cooperatively, on whichever thread next calls
   an import that pumps -- which is why OSWaitEvent, OSWaitEventWithTimeout,
   OSLockMutex and now OSWaitSemaphore all have to pump, and why the archive
   pump exists at all. **A dedicated host thread that delivers completions,
   the way AppIO does, is the architecture that would retire the pumps.**
   Not attempted here: it changes which thread runs guest callbacks, which
   is a hardware question.

   Cemu also queues FS commands by priority (`__FSQueueCmdByPriority`), so
   completion order on hardware is not strictly submission order.
   conquertron's FIFO queue is stricter than hardware, which is the safe
   direction.

MPL-2.0 is file-level copyleft: a file adapted from Cemu must stay MPL-2.0,
but it can sit in a project under another licence. Reading it to confirm
semantics, as the shims do, carries no obligation at all.

### [devkitPro/wut](https://github.com/devkitPro/wut) — zlib

The homebrew Wii U SDK. Its `include/coreinit/*.h` headers are the
signatures and structure layouts the shims already cite, and its
[generated docs](https://wut.devkitpro.org/group__coreinit__event.html) are
the quickest place to check a function's documented contract. zlib licence:
usable freely.

### [WiiUBrew: Coreinit.rpl](https://wiiubrew.org/wiki/Coreinit.rpl) and [/dev/fsa](https://wiiubrew.org/wiki//dev/fsa)

Community documentation of coreinit exports and the filesystem IPC device.

## Tools for reading the binary

| Project | Licence | Why |
| --- | --- | --- |
| [Maschell/GhidraRPXLoader](https://github.com/Maschell/GhidraRPXLoader) | GPL-3.0 (tool) | Opens `.rpx`/`.rpl` directly in Ghidra, with Espresso (paired-singles) processor definitions and a script that names imports. Using a GPL tool puts no obligation on what it is used to read. Every "read the instruction, not the name" lesson in the Loading Deadlock notes gets cheaper with a decompiler view next to the probes. |
| [wiiu-env/RPXParserLib](https://github.com/wiiu-env/RPXParserLib) | Apache-2.0 | A Java library that parses RPX/RPL: symbols, imports, exports. Usable, and a cross-check for `elf_loader.cpp`, particularly compressed sections and import resolution. |
| [BullyWiiPlaza/RPL-Studio](https://github.com/BullyWiiPlaza/RPL-Studio) | see repo | GUI pack/unpack for RPX/RPL. |

## Paired singles: toolchain support is arriving

- [llvm/llvm-project#211463](https://github.com/llvm/llvm-project/pull/211463)
  adds a `ppc750cl` CPU and about 40 paired-single instructions to LLVM's
  assembler and disassembler (not codegen yet). Open, approved in review,
  last active 2026-09-24. **Once merged, clang -- and so the zig that
  verify.sh uses -- can assemble paired-single test programs**, which would
  let verify.sh check conquertron's `PPC_INS_ARKCHEMY_PS_*` handling the way
  it already checks integer and float code. Today nothing does.
- [capstone#476](https://github.com/capstone-engine/capstone/issues/476):
  Capstone still lacks paired singles, which is why conquertron decodes them
  itself.

## The Switch side

| Project | Licence | Why |
| --- | --- | --- |
| [devkitPro/uam](https://github.com/devkitPro/uam) | see repo (mesa/nouveau-derived) | The offline GLSL → DKSH compiler blaster's shader path targets. |
| [averne/libuam](https://github.com/averne/libuam) | see repo | A library form of uam. If shaders ever have to be compiled when the game first binds them, rather than ahead of time by `build-shaders.sh`, this is the route. 1★. |

## Suggested next steps, in order of payoff

1. **Build and run the conquertron branch on hardware.** Judge it on
   `served/back`, per the Loading Deadlock notes -- the FS queue race is the
   first candidate cause for the stall that is a certain bug rather than a
   theory.
2. **Port XenonRecomp's `setjmp`/`longjmp` approach** (MIT, with
   attribution). It is a ROADMAP item with a known-good answer.
3. **Prototype an AppIO-style completion thread** behind a flag, and compare
   it with the pumps on hardware.
4. **Contact the nWiiURecomp author.** Two projects doing the same thing to
   the same OS will keep finding the same bugs.
5. **Watch LLVM #211463.** When it lands, add paired-single programs to
   verify.sh.
