# The synthetic/real address split, and why it should go

## What conquertron does today

`assign_global_addrs` (src/elf_loader.cpp) hands every **data** section a
*synthetic* base starting at `0x2000`, packed consecutively:

```
.rodata, .data, .bss, ...   ->  synthetic 0x2000 upward
functions / .text           ->  REAL addresses, used as-is
```

So a recompiled binary lives in two address spaces at once. Text keeps the
addresses it had in the RPX; data is relocated into a small synthetic region
near the bottom of guest memory.

The reason is recorded in the function's own comment: `PpcContext::mem` was
4 MB, and real Wii U data addresses (`0x10000000`+) obviously would not fit.

## Why that reason has expired

`PPC_MEM_SIZE` is now **1 GB** (`0x40000000`, ppc_runtime.h). Every real
address in the Skylanders module fits with room to spare:

| region      | real address              | fits in 1 GB |
|-------------|---------------------------|--------------|
| text start  | `0x02000020`              | yes |
| text end    | `0x0258881C`              | yes |
| .bss region | `0x10130000`              | yes |
| image end   | `0x10181290`              | yes |

The guest heap in jouster sits at `0x04000000` and runs to roughly
`0x0B400000`, so it collides with neither.

## What the split actually costs

It is not merely untidy. Any guest code that **compares** two pointers, or
tests whether a pointer falls in a range, gets a meaningless answer whenever
the two sides came from different spaces.

The concrete case, found 2026-08-29 and confirmed against Cemu:

`Core::igMemoryPoolFrameManager::setMemoryPool` asks whether a string lies
inside the loaded module image — a literal to keep, or pool memory to release:

```
lwz  r9, 0xc98(r30)    ; A = module start
blt  release           ; below the image
lwz  r0, 0xc9c(r29)    ; B = module end
blt  keep              ; inside the image -> static literal
release                ; above the image
```

Real values, read off the running game in Cemu:

```
A = 0x02000020   start of .text
B = 0x10181290   end of the image
```

Recompiled values, measured on hardware:

```
A = 0x00183190   the TOP of synthetic static data
B = 0x0070B990   the synthetic image end
```

`A > B`. The window is inverted, contains no static data at all, and every
string literal compares below `A` and is released. A released literal is then
read back as an `igStringPoolItem` through a header at `-0xc` that does not
exist, which is what produced `pool == item + 0xc` and hung the boot in
`igStringPool::remove` walking a bucket chain with `bucketCount` 0.

Note also the annotation conquertron emitted for `B`: `/* &+0 */`, with **no
section name**. `0x10181290` is one past the end of the image, so it belongs
to no section and the fold degenerated silently.

The deeper point is that no single `[A, B)` *can* describe the module when
text is real and data is synthetic. This is not a bug in the fold of either
bound; it is the split itself surfacing.

## The fix

Place data sections at their real addresses too, and delete the synthetic
space entirely. Then pointer comparisons, range tests, and any guest code that
does arithmetic across text and data all behave as they do on hardware, and
the `&+0` class of degenerate fold disappears with them.

## Why it is not done yet

It relocates every global in the generated output, so it needs a full
regeneration and a re-verification pass across everything currently working
(video playback, the boot splash, the Bink shim). That is a deliberate change
to make with hardware runs available to check it, not a drive-by.

Suggested shape: an opt-in flag (`--real-data-addrs`) so the two layouts can
be compared on the same binary before the default moves.
