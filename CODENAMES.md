# Codenames

Every function this project has traced deeply enough to matter gets an individual
codename, drawn from Spyro's Adventure's own roster of Skylanders and enemies, colored
by that character's real element (or, for enemies without an official element, the
color most associated with them). This file is the index — the actual technical
identity of each function lives in code comments next to the codename, since the
codename alone doesn't carry enough information to debug with.

Heroes (Skylanders) are used for the big, top-level boot phases. Enemies are used for
the individual functions found and fixed along the way — a small nod to "hunting down
the bug" as the thing actually being defeated.

## Boot phases (`switch/game/source/main.c`)

| Codename | Element | Color | Real meaning |
|---|---|---|---|
| Trigger Happy | Tech | Amber/Yellow | Globals not yet initialized (earliest phase) |
| Spyro | Magic | Purple | Globals initialized, running the 114 real static initializers |
| Gill Grunt | Water | Blue | Static initializers done, running the real recompiled game entry point |
| Stealth Elf | Life | Green | Game entry point returned |

## Key traced functions (the 2026-08-21 boot-hang investigation)

| Codename | Type | Color | Real function | Real vaddr |
|---|---|---|---|---|
| Kaos | Enemy (final boss) | Purple | `Core::igArkCore::initBootstrap` — the real function whose incomplete real bootstrap sequence turned out to be the root of everything traced this session | `0x214726c` |
| Glumshanks | Enemy (henchman) | Grey | `Core::igMemoryContext::__ct__` — the constructor whose own allocation failure (a missing "bootstrap heap") left the whole bootstrap object unconstructed | `0x2178438` |
| Arkeyan | Enemy faction | Gold | `Core::igMemoryContext::getMemoryPoolByIndex` — read a real "current memory context" global that was never set, degenerate-pooled every caller until the bootstrap heap fix. Named for the real Arkeyan enemy faction, since the real function's own class name already carries the same "Ark" prefix as the engine namespace it belongs to | `0x217b058` |
| Chompy | Enemy (common) | Green | `Core::igObject::getMemoryPool` — the small, ubiquitous utility every pool lookup in the engine routes through | `0x215c280` |
| Drobot | Skylander (Tech) | Cyan | `Core::igMemoryContext::userInstantiate` — the real function that, once it finally ran (after the bootstrap-heap fix), correctly set the "current memory context" global for the first time all session | `0x217b820` |
| Greeble | Enemy (common) | Amber | `Core::igStringPoolContainer::reserveMemory` — the real pool-list loop that spun forever retrying a `malloc(28)` against a degenerate pool | `0x21a4f68` area |
| Troll | Enemy (big) | Brown | `Core::igObjectList::setCount` — the real function whose "old count" read back as an implausible `0x100000` (1,048,576), driving two ~131,056-iteration loops | `0x2164250` |
| Drow | Enemy faction | Dark grey | `Core::igObject::decrementRefCount` — the real atomic (`lwarx`/`stwcx.`) refcount decrement, confirmed correct; not itself a bug, just heavily traced while chasing the above | `0x215c464` |

## Real fix, for context

The actual root cause: `Core::igMemoryContext`'s own constructor needs a small,
dedicated "bootstrap heap" (its handle lives at a real global, `.bss+306320`) to
allocate itself — solving the real chicken-and-egg problem of needing a working memory
pool to construct the very object that sets up memory pools. That heap was never
created anywhere in this project's recompiled output, so the constructor silently
failed and returned unconstructed, cascading into every symptom above. Fixed by
creating the heap directly from the boot shim (`arkchemy_mem_bootstrap_heap_init` in
`recomp/include/cafeos_coreinit_mem.h`), called once before the real game entry point
runs.
