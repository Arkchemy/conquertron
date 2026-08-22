#ifndef ARKCHEMY_ELF_LOADER_H
#define ARKCHEMY_ELF_LOADER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace recomp {

// One recovered function, boundaries taken directly from the ELF symbol
// table (the input is expected to be unstripped). This sidesteps
// control-flow-based function boundary recovery, which is only needed for
// stripped binaries later in the project.
struct ElfFunction {
    std::string name;
    uint32_t addr = 0;  // offset within .text
    uint32_t size = 0;  // bytes
};

// A `lis`/load-or-store instruction pair addressing a data symbol -- the
// object isn't linked yet, so there's no real address to resolve to;
// codegen instead gives every referenced section (mutable .data/.bss and
// read-only .rodata* alike) a synthetic address in PpcContext::mem, real
// enough for indexed/pointer-taking access to work correctly regardless of
// whether the eventual use is a single scalar load or something more
// general (e.g. a compiler-generated switch-statement lookup table's
// address, indexed at runtime) -- see is_synthetic_addr_lo_reloc in
// codegen.cpp.
struct DataReloc {
    // HI is R_PPC_ADDR16_HI's unrounded high-16-bits counterpart to HA
    // (used with a plain `ori` low half instead of a signed `addi`/load,
    // which doesn't need HA's +0x10000 rounding correction) -- real
    // linked .rpx output uses HI/LO for RPL import trampolines. Treated
    // identically to HA everywhere in this codebase: both just mark "the
    // high-half instruction of a lis+something pair", since codegen
    // resolves the whole pair to a literal computed address rather than
    // reconstructing it via real bitwise hi/lo assembly.
    enum Type { HA, LO, HI } type;
    std::string section;  // e.g. ".rodata.cst4"
    int32_t addend = 0;   // byte offset within that section

    // Set when the relocation's symbol is a function (STT_FUNC) rather than
    // a data section -- the `lis`+`addi` idiom for taking a function's
    // address (e.g. a function pointer later fed through `mtctr`+`bctrl`)
    // uses the exact same relocation pair as addressing mutable data, but
    // the value it needs is the function's real entry address, not a
    // synthetic PpcContext::mem address. func_addr is that address, in the
    // same address space as ElfFunction::addr (so it can be dispatched via
    // the same addr_to_name/ppc_dispatch mechanism used for already-linked
    // `bl` targets).
    bool is_function = false;
    std::string func_name;
    uint32_t func_addr = 0;

    // Set when the relocation's symbol lives in a `.fimport_<library>`
    // section -- an RPL cross-library import, not a real local address at
    // all. See ImportTrampoline/find_import_trampolines.
    bool is_import = false;
    std::string import_library;
    std::string import_function;
};

// A real Wii U .rpx/.rpl calls into other system libraries (coreinit,
// vpad, proc_ui, ...) through a small linker-synthesized stub in .text:
// `lis r0,HI(import_addr) / ori r0,r0,LO(import_addr) / mtctr r0 / bctr`.
// The stub itself has no symbol table entry (the linker invents it), but
// its `lis`/`ori` pair carries R_PPC_ADDR16_HI/LO relocations targeting a
// real, named FUNC symbol defined in a `.fimport_<library>` section (see
// WiiUBrew's RPL format docs) -- confirmed against a real devkitPPC-built
// .rpx, not guessed. find_import_trampolines below recognizes this exact
// instruction pattern and records what each stub's address really means,
// so a `bl` to one resolves to a named external call instead of an
// "unresolved call" error.
struct ImportTrampoline {
    std::string library;   // e.g. "coreinit", from the .fimport_<library> section name
    std::string function;  // e.g. "FSFlushFile"
};

// A real Cafe OS "data import" -- a memory slot (an ordinary global, not
// a real local function) that the real RPL loader fills in at load time
// with a real function's actual address, so a real `lwz` from that slot
// followed by `mtctr`+`bctrl` calls straight into it -- architecturally
// different from a `.fimport_*` trampoline (a real, local `bl` target
// this project already resolves directly). Confirmed real and not
// hypothetical: found 2026-08-20 tracing a real hang -- Green Hills
// libc's own real sbrk() calls through exactly one of these
// (coreinit's MEMAllocFromDefaultHeapEx) to grow the heap, and this
// project's runtime had no mechanism at all for populating such a slot
// with anything real, so the indirect call silently dispatched nowhere.
// Detected during load_elf against a small, explicit, curated allowlist
// of real Cafe OS function names this project's own shim can actually
// provide (see elf_loader.cpp) -- deliberately not "every symbol in a
// .dimport_* section", since the same real sections also carry genuine
// *data* symbols (errno, environ, _iob, __OSCurrentThread, ...) that
// must stay ordinary memory, never routed through ppc_dispatch.
struct DataImportSymbol {
    std::string library;
    std::string function;
    std::string section;  // e.g. ".dimport_coreinit"
    uint32_t st_value = 0; // symbol's own real st_value (absolute addr for an already-linked binary)
};

// A resolved data import: fake_addr is a real, reserved, synthetic
// "function address" (never colliding with any real .text address in
// this binary) that ppc_init_globals writes into the slot's own real
// synthetic address (this map's key) and that ppc_dispatch recognizes,
// routing a real bctrl through that slot to ppc_import_<library>_<function>.
struct DataImport {
    uint32_t fake_addr = 0;
    ImportTrampoline target;
};

struct ElfImage {
    std::vector<uint8_t> text;
    uint32_t text_addr = 0;  // sh_addr of .text (usually 0 for a relocatable .o)
    uint32_t entry = 0;      // ELF header e_entry (meaningful for linked executables, not relocatable .o)
    std::vector<ElfFunction> functions;

    // Import trampoline addresses (within .text) resolved by
    // find_import_trampolines, keyed by the trampoline's own start
    // address (the same address a `bl` targeting it would use).
    std::map<uint32_t, ImportTrampoline> import_trampolines;

    // R_PPC_REL24 relocations found in .rela.text, keyed by the address of
    // the `bl` instruction they apply to, valued by the target function's
    // symbol name. Populated so codegen can resolve inter-function calls
    // without the linker having run yet.
    std::map<uint32_t, std::string> call_relocs;

    // R_PPC_ADDR16_HA / R_PPC_ADDR16_LO relocations found in .rela.text,
    // keyed by the address of the instruction they apply to. See DataReloc.
    std::map<uint32_t, DataReloc> data_relocs;

    // Raw bytes of every PROGBITS section, keyed by name, so codegen can
    // read the actual constant a DataReloc points at.
    std::map<std::string, std::vector<uint8_t>> section_bytes;

    // Real, declared sh_size of every named section, including SHT_NOBITS
    // (.bss) -- section_bytes above is deliberately empty for .bss (it has
    // no file content to copy), so this is the only place its true real
    // size is ever recorded. assign_global_addrs needs this: relying on
    // section_bytes[section].size() alone silently gives every .bss-only
    // section just the 256-byte placeholder minimum regardless of its real
    // size, aliasing every real global inside a section bigger than that
    // on top of each other.
    std::map<std::string, uint32_t> section_sizes;

    // Real declared sh_addr of every named section -- 0 for a relocatable
    // .o (addresses aren't assigned until link time, so symbol st_value
    // there is already section-relative on its own), a real absolute
    // virtual address for an already-linked binary like the actual
    // Skylanders .rpx. Needed to correctly normalize a *named object*
    // symbol's own st_value (see the real bug this fixed, 2026-08-20,
    // in the DataReloc-construction code in load_elf): st_value - this
    // gives the real, correct section-relative offset in both cases.
    std::map<std::string, uint32_t> section_real_addr;

    // Synthetic base address (an offset into PpcContext::mem) assigned to
    // each *mutable* section referenced by a DataReloc (typically .data or
    // .bss) -- read-only sections (.rodata*) don't need one, since codegen
    // resolves those to compile-time literal constants instead (see
    // DataReloc). A real symbol's address is this base plus its DataReloc
    // addend, which correctly preserves relative offsets between symbols in
    // the same section (needed for indexed access into e.g. a global
    // array). Computed once, after load_elf finishes parsing relocations,
    // by assign_global_addrs below.
    std::map<std::string, uint32_t> global_section_base;

    // Real data-import symbols found during load_elf (see DataImportSymbol's
    // own comment), not yet resolved to real synthetic addresses -- that
    // needs global_section_base, which doesn't exist until after
    // assign_global_addrs runs. resolve_data_imports below does that,
    // populating data_imports.
    std::vector<DataImportSymbol> data_import_symbols;

    // Resolved data imports, keyed by each slot's own real synthetic
    // address -- see DataImport's own comment. Populated by
    // resolve_data_imports after assign_global_addrs.
    std::map<uint32_t, DataImport> data_imports;
};

// Assigns global_section_base entries for every mutable section referenced
// in img.data_relocs. Call once after load_elf succeeds and before
// generating any function bodies (codegen.cpp assumes it's already been
// done). Idempotent -- safe to call more than once, though there's no
// reason to.
void assign_global_addrs(ElfImage &img);

// Scans img.data_relocs for HI-type relocations targeting a `.fimport_*`
// section, confirms the classic `lis/ori/mtctr/bctr` trampoline shape
// immediately follows in img.text, and populates
// img.import_trampolines for each match. Call once after load_elf
// succeeds (order relative to assign_global_addrs doesn't matter).
// Harmless no-op on objects with no RPL imports (ordinary relocatable
// .o test objects, for instance).
void find_import_trampolines(ElfImage &img);

// Resolves img.data_import_symbols (found during load_elf, see
// DataImportSymbol's own comment) to real synthetic addresses and
// assigns each a real, reserved "fake function address", populating
// img.data_imports. Call once after assign_global_addrs (needs each
// symbol's own section to already have a synthetic base) -- order
// relative to find_import_trampolines doesn't matter. Harmless no-op
// on objects with no recognized data imports.
void resolve_data_imports(ElfImage &img);

// Loads a big-endian, 32-bit ELF relocatable object (as produced by
// `zig cc -target powerpc-freestanding-eabi -c`) and extracts its .text
// section, FUNC symbols defined within it, R_PPC_REL24 call relocations,
// and R_PPC_ADDR16_HA/LO data relocations (see ElfImage::call_relocs and
// ::data_relocs). Other relocation types (absolute calls, non-.text data
// writes, etc.) are not handled yet -- a real Wii U .rpx will need broader
// relocation support, tracked as follow-up work.
bool load_elf(const std::string &path, ElfImage &out, std::string &error);

}  // namespace recomp

#endif  // ARKCHEMY_ELF_LOADER_H
