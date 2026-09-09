# Designing probes for recompiled code

Every finding in this project comes from instrumenting generated C and reading
a log off a Switch. That loop is slow — build, copy 176 MB over MTP, launch,
pull the log — so a probe that answers the wrong question costs a real day.

This is the list of ways they have actually gone wrong here, each one written
down after it cost a run. It is not hypothetical advice.

## Record on a predicate, never on arrival order

A fixed-size ring that fills with the first N events it sees will fill with the
boring ones. Boot makes millions of calls; the interesting ones are rarely
first.

This has cost a run **five separate times**:

- `OWNER` filled with neighbouring frees instead of the allocation under test
- `SIZECALC` filled with the first ten calls at ~19,000 and never reached the
  ones at ~159,000
- `GX2CENSUS` keyed on call-site `lr` with 24 slots, and attributed about 110
  calls out of 133,069 — the total was right, the breakdown worthless
- `XMLCFG` held ten attribute names, recorded exactly ten of 308 lookups, and
  dropped the one key the probe existed to check
- the `SPLITVERIFY` guard below

Prefer, in order:

1. **A key that is naturally bounded.** Per pool, per device, per imported
   function. `GX2CENSUS` keyed on the GX2 function instead of the call site
   went from 110 attributed calls to all 134,606, because there are only 140
   functions.
2. **A predicate that selects the event you care about.** "Record only when the
   returned block has no successor" beats "record the first six returns".
3. **A high-water or extremum**, which needs no storage at all.

If a ring is unavoidable, count what it dropped and report that count. A ring
that silently truncates looks identical to one that saw nothing.

## Beware the guard that hides the case you are hunting

```c
if (__bsz && ppc_load_u32(ctx, __succ + 4u) == 0u)   /* wrong */
```

This ran for a full cycle reporting `n=0`, which read as "the allocator is
clean". The bug being hunted was a returned block whose own size was **zero** —
and `__bsz &&` short-circuits past exactly that. The fix was one character of
logic:

```c
if (__bsz == 0u || ppc_load_u32(ctx, __succ + 4u) == 0u)
```

Before writing a guard, ask what value the bug would put in it.

## Hook every exit, not the first one you find

`igArchiveBlockManager::allocate` returns from two places: a conditional
`beqlr` the moment it finds a free block, and a final `blr`. A hook that
matched `blr` and not `beqlr` reported `gotBlock=0`, which looked like
allocation failing twice. It had actually succeeded twice, through the exit
that was not being watched.

PowerPC returns include `blr`, `beqlr`, `bnelr`, `bltlr`, `bgelr`, `bgtlr`,
`blelr`. Match all of them, and print how many you hooked so a count of 1 on a
600-byte function is visible as suspicious.

## "Did something come back" is not "what came back"

`getAttribute("startLevel")` was recorded as returning non-null, which was read
as the config being correct. The value it returned was `"test"` — the built-in
default. A whole cycle was spent on the wrong hypothesis because the probe
recorded a boolean where the question was a string.

If the question is *which*, record the value. Reading a guest C string costs a
few lines:

```c
char buf[24]; unsigned k;
for (k = 0; k < 23u && ptr; k++) {
    char c = (char)ppc_load_u8(ctx, ptr + k);
    buf[k] = c; if (!c) break;
}
buf[k < 23u ? k : 23u] = 0;
```

## Know which implementation of a virtual you are watching

`igVirtualStorageDevice::getIsActive` reads a byte at `+0x20`.
`igPhysicalStorageDevice::getIsActive` reads a word at `+0x2c`. A probe that
recorded `+0x20` for every device produced three entries with `flag=0x4` that
passed the gate and three with `flag=0x4` that failed — which looked like a
contradiction in the game and was a probe reading a field half its subjects
never consult.

**Record the vtable pointer.** It names the class, and therefore which
implementation runs. Vtable *contents* survive as real `.text` addresses even
though the vtable's own address is relocated, so compare the slot, never the
pointer to it.

## Absence of evidence needs its own evidence

Three separate "nothing happened" results here were artefacts:

- `hits=0` from the store watch meant *no translated store instruction* touched
  the address. `memset` and `memcpy` shims write `ctx->shared->mem` directly and
  never pass through `ppc_store_u32`, so they are invisible to it. A bulk clear
  produces exactly the same evidence as memory that was never written.
- A probe block gated on `g_arkchemy_cfg_replay` produced no output at all,
  because `watch.cfg` is parsed on the game thread and the block ran in
  `main()` first. Silence looked identical to the probe failing.
- `XMLMERGE n=0` was real, but only because the hook was checked in the source
  *and* the format string checked in the built `.nro` before it was believed.

Before reporting that something never happened, prove the probe could have seen
it. `strings Jouster.nro | grep -c "^YOURTAG"` costs nothing.

## `g_ppc_current_pc` cannot identify a writer

It is a single global, not thread-local, set at function entry and never
restored on return. With more than one guest thread it names whatever function
*any* thread last entered. It once reported `sputc_limit` and `tlsf_memalign`
as the same store's context.

- **`ctx->lr`** survives into the callee and names the code responsible.
- **The `PpcContext` pointer** is genuinely per-thread and is the only reliable
  thread identity.

The corrupting write that stalled boot for four sessions was found only when
the context pointer was added and showed the write coming from a thread that
had never entered the allocator.

## Bound every walk, and count what escapes

A chain walk bounded only by "stay within 1 GB of the base" left its arena and
reported a high-water mark 73 MB past the end of a 5 MB pool, which then made
every healthy walk afterwards look like a regression.

Bound by the real extent, and count the walks that tripped the bound as
**runaways** rather than recording them. A corrupt structure must not be able to
manufacture a result.

## Run it on the host if you can

`conquertron/hosttest` compiles a recompiled translation unit natively.
`ppc_runtime.h` needs only `math`/`stdint`/`stdlib`/`string`, and recompiled
code reaches guest memory solely through `ppc_load`/`ppc_store`, so anything
self-contained runs on a desktop in milliseconds — steppable in a debugger,
bisectable, with no Switch involved.

A week went into ten-minute hardware cycles chasing an allocator bug that turned
out to be reproducible from a captured 804-call trace in under a second. If the
subsystem does not need the OS, do not put it on the console to ask it a
question.

## Resolve a captured `lr` to a symbol before believing it

`XMLWHO` recorded the `lr` of every caller of `igXmlDocument::read` and found
exactly one: `0x21e4704`. That reads like a definitive answer — one client,
here it is. It was the sibling `read(const char*)` overload, eight
instructions into a wrapper that forwards to the `igFile*` version. The
probe had hooked the inner overload and captured the outer one.

Look the address up in the symbol table (the retail `.rpx` is unstripped,
so this is a lookup, not a guess) *before* drawing a conclusion from it. An
`lr` inside the same class, or the same function, means the probe is
watching the wrong level.

## Walk up the call graph before instrumenting further down

The boot stall of 2026-09-08 had a nine-link measured chain, every link
confirmed on hardware, and none of them was the bug. The cause was two
levels above the top of the chain, in a byte the recompiler loaded from the
wrong address.

The static tools answer "who *can* call this" in seconds and need no
hardware: scan `.text` for `bl` at a target, scan for `lis`+displacement
pairs that form a global's address. `igArkCore::init` is the only caller of
`igRegistry::read` in the whole binary; `__sti___22_tfbCafeApplication_cpp`
is the only writer of `_registryPath`. Both facts are one script each, and
between them they bounded the problem to a single guard byte.

When a measured chain is entirely consistent and still explains nothing,
stop extending it downward. The next probe belongs above the first link,
and it may not need to be a probe at all.

**Relocation offsets are not instruction addresses.** `R_PPC_ADDR16_HA/HI/LO`
point at the low halfword, `insn + 2`. A hand-written check that looks up
instruction addresses in `.rela.text` finds nothing and concludes the sites
are unrelocated — which inverts the answer.

## Two general rules that would have caught most of the above

**Before building:** write down what each possible outcome would mean. If two
different outcomes lead to the same next step, the probe is not worth building.
If one outcome is "nothing", say in advance how you will tell "it did not
happen" from "I could not see it".

**Before believing:** ask what else would produce this exact output. `hits=0`,
`n=0`, `flag=0x4` and `got=1` each had an innocent explanation that was more
likely than the interesting one.
