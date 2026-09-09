# Narrow loads and stores double-applied their relocation displacement

**Status:** fixed 2026-09-09 in `src/codegen.cpp`.
**Scope:** 423 instructions in Skylanders: Spyro's Adventure; any recompiled
binary that touches a relocated global with anything other than a word.
**Severity:** silent. No crash, no null pointer, no log line. The access
simply lands somewhere else and reads whatever happens to live there.

## The rule

When a `lis` carries `R_PPC_ADDR16_HA`, `assign_global_addrs` writes the
**complete** relocated address into the base register. The consuming
instruction's own 16-bit displacement field is then a relocation
placeholder — the linker's low half of an address that no longer exists —
and must be discarded, not added on top. That is what
`is_synthetic_addr_lo_reloc` is for, and `PPC_INS_LWZ`/`STW` and friends
have called it for a long time.

Eleven D-form handlers never did:

| handler | sites in this binary |
| --- | --- |
| `lbz` | 208 |
| `stb` | 103 |
| `lfsu` | 50 |
| `stfsu` | 33 |
| `lhz` | 18 |
| `stfdu` | 9 |
| `sth` | 2 |
| `ori` | 2 |
| `lha`, `lfdu`, `lmw`, `stmw` | 0 (fixed anyway) |

`lwz`/`stw` were always correct, and words are the overwhelming majority
of relocated-global traffic, which is why this survived every previous
bisection. The bug only shows up when the engine keeps a **flag** — a
`bool`, an enum byte, a `float` tuning constant — in a global.

## What it actually broke

`igArkCore::init` gates the entire configuration load on one byte:

```
2147ed4: lis  r5, 0x100d          ; r5 = &igRegistry::_autoLoad
2147edc: lbz  r0, -0x2bb0(r5)     ; R_PPC_ADDR16_LO
2147ee8: cmpwi r0, 0
2147ef0: beq  0x2148194           ; skip the whole registry load
```

`_autoLoad` lives at `0x100cd450` and is statically initialised to 1.
The recompiled `lbz` read `.data+2656` instead of `.data+5648` — 8,528
bytes short of the flag, in unrelated data that happened to be zero. So:

- `igRegistry::read` was never called (`XMLWHO calls=0`, measured);
- `alchemy.xml` — `_registryPath`, set correctly by
  `__sti___22_tfbCafeApplication_cpp` — was never opened by the registry;
- `igXmlNode::merge` never ran, so every registry value kept its default;
- `startLevel` stayed `"test"`, so `tfbGame::streamContext::load` closed
  `bootstrap.bld` to open `level/test.bld`, which does not exist;
- with no registered archive device the context pumped 8 times against
  1,341 presents, decompressed 1 block of 35, and drew an empty scene.

A one-byte read at the wrong address, and the game renders a full frame
of nothing. Every downstream symptom was real and correctly measured;
none of them was the bug.

## How it was found

Statically, not on hardware. `XMLWHO` came back with
`igRegistry::read calls=0` and exactly one caller of
`igXmlDocument::read` — at `0x21e4704`, which turned out to be the
sibling `read(const char*)` overload rather than a real client. Walking
the call graph in the retail RPX (`igArkCore::init` is the only caller of
`igRegistry::read`, and `__sti___22_tfbCafeApplication_cpp` is the only
writer of `_registryPath`) narrowed it to the guard byte, and comparing
the generated C against the raw instruction showed the displacement
applied twice.

The 423-site count came from cross-checking every folded `lis` in the
generated output against `.rela.text` — no guessing about how widespread
it was.

## Note on the relocation offset

`R_PPC_ADDR16_HA/HI/LO` point at the instruction's **low halfword**, i.e.
`insn_addr + 2`. `elf_loader.cpp` already aligns down (`r_offset & ~3u`);
anything checking the relocation table by hand has to do the same, or it
will conclude — as this investigation briefly did — that the sites carry
no relocation at all.
