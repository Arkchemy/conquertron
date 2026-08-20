#include "elf_loader.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "disassembler.h"

namespace recomp {

namespace {

uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Real compiler-generated register-spill helpers (GCC/GHS's well-known
// "_savegpr_N"/"_restgpr_N"/"_savefpr_N_l"/etc. convention -- confirmed
// present, by exact name, in real Skylanders: Spyro's Adventure code) are
// linker-synthesized: multiple STT_FUNC symbols at different addresses
// that all fall through into a *shared* tail ending in one real `blr`,
// with st_size left as 0 in the symbol table since no single symbol owns
// a disjoint range. Scanning forward for the first blr (0x4e800020) gives
// each entry point its own correct, self-contained instruction range --
// verified against the real binary: _savegpr_29's scanned range genuinely
// covers stw r29/r30/r31 + mflr r31 + blr, matching the real bytes at
// that address exactly (checked by hand before writing this).
//
// Capped at a small byte budget so a genuinely bare (mis-sized, no code)
// symbol fails safe back to size 0 (skipped, same as today) rather than
// scanning arbitrarily far into unrelated code.
uint32_t scan_forward_for_blr_size(const std::vector<uint8_t> &text, uint32_t text_addr, uint32_t addr) {
    constexpr uint32_t kMaxScanBytes = 256;
    uint32_t off = addr - text_addr;
    if (off >= text.size()) return 0;
    uint32_t limit = (uint32_t)std::min<size_t>(text.size(), (size_t)off + kMaxScanBytes);
    for (uint32_t p = off; p + 4 <= limit; p += 4) {
        if (rd_be32(&text[p]) == 0x4e800020u) return (p - off) + 4;
    }
    return 0;
}

// GHS emits some symbol names that are valid ELF strings but not valid C
// identifiers -- e.g. debug begin-of-file/end-of-file markers shaped like
// "..bof.C.3A.5CDev.5Cdepot...src.cpp..<hash>..0" (a percent-encoded-ish
// source path, literal dots and all). Every symbol name in this file
// eventually gets used as `ppc_<name>` in generated C, so any character
// outside [A-Za-z0-9_] has to be neutralized once, here, rather than at
// every call site that later stringifies a name into C source. Confirmed
// necessary against the real binary: gcc failed with "expected '=', ',',
// ';'... before '.' token" on exactly these symbols before this existed.
std::string sanitize_c_identifier(const std::string &name) {
    std::string out = name;
    for (char &c : out) {
        if (!std::isalnum((unsigned char)c) && c != '_') c = '_';
    }
    return out;
}

// Real, previously-undiscovered bug fixed here: a real RPL cross-library
// import's own `.fimport_<library>` section name preserves the real
// Wii U shared-library file's own real name verbatim, including a
// literal `.rpl` extension for the real libraries that have one (e.g.
// `.fimport_nsyshid.rpl`, `.fimport_vpad.rpl` -- confirmed directly
// against the real Skylanders binary's own real section names) --
// unlike e.g. `coreinit`/`gx2`, which have no such suffix at all. Every
// `ppc_import_<library>_<function>` identifier this project's own real,
// hand-written CafeOS shim headers use is named *without* that suffix
// (`cafeos_nsyshid.h`'s real functions are `ppc_import_nsyshid_...`,
// not `ppc_import_nsyshid_rpl_...` or the literally-invalid
// `ppc_import_nsyshid.rpl_...`) -- a real mismatch never caught before
// now because no earlier real test happened to recompile a call site
// or generate the full forward-declaration list for one of the two
// real libraries (`nsyshid.rpl`, `vpad.rpl`) that actually have this
// suffix in the same build. Stripping it here, once, at the real
// source of the library name, keeps every downstream use (forward
// declarations in main.cpp, real call-site codegen in codegen.cpp)
// consistent with the shim headers' own already-established real
// naming convention, instead of just sanitizing the literal `.`
// character into a `_` (which would still produce a real, genuine
// mismatch -- `nsyshid_rpl` vs. the real `nsyshid` the shim headers
// actually use).
std::string strip_rpl_suffix(const std::string &library) {
    constexpr const char *kRplSuffix = ".rpl";
    constexpr size_t kRplSuffixLen = 4;
    if (library.size() > kRplSuffixLen && library.compare(library.size() - kRplSuffixLen, kRplSuffixLen, kRplSuffix) == 0) {
        return library.substr(0, library.size() - kRplSuffixLen);
    }
    return library;
}

constexpr int STT_FUNC = 2;
constexpr uint32_t SHT_NOBITS = 8;
constexpr uint32_t SHT_RELA = 4;
// Real retail .rpx/.rpl files store most sections zlib-compressed to
// shrink file size -- confirmed against an actual Wii U game disc (not
// guessed at, and not needed by anything tested here before now, since
// the homebrew .rpx used for earlier real-world validation didn't happen
// to use it). In-file layout when set: a 4-byte big-endian "real
// (decompressed) size" prefix, followed by a plain zlib stream.
constexpr uint32_t SHF_RPL_ZLIB = 0x08000000;
constexpr uint32_t R_PPC_ADDR16_LO = 4;
constexpr uint32_t R_PPC_ADDR16_HI = 5;
constexpr uint32_t R_PPC_ADDR16_HA = 6;
constexpr uint32_t R_PPC_REL24 = 10;

const char kImportSectionPrefix[] = ".fimport_";

// Returns a section's real content bytes, transparently decompressing it
// first if SHF_RPL_ZLIB is set. SHT_NOBITS (.bss) sections have no real
// file content and are always empty. On a decompression failure, returns
// false and leaves `error` set -- callers propagate this as a load_elf
// failure rather than silently reading garbage.
bool read_section_bytes(const std::vector<uint8_t> &data, uint32_t sh_type, uint32_t sh_flags, uint32_t sh_offset,
                         uint32_t sh_size, std::vector<uint8_t> &out, std::string &error) {
    out.clear();
    if (sh_type == SHT_NOBITS || sh_size == 0) return true;
    if (sh_offset + sh_size > data.size()) {
        error = "section extends past end of file";
        return false;
    }
    if (!(sh_flags & SHF_RPL_ZLIB)) {
        out.assign(data.begin() + sh_offset, data.begin() + sh_offset + sh_size);
        return true;
    }
    if (sh_size < 4) {
        error = "RPL-compressed section too small to hold a size prefix";
        return false;
    }
    uint32_t decompressed_size = rd_be32(&data[sh_offset]);
    out.resize(decompressed_size);
    uLongf dest_len = decompressed_size;
    int rc = uncompress(out.data(), &dest_len, &data[sh_offset + 4], sh_size - 4);
    if (rc != Z_OK) {
        error = "zlib decompression failed for an RPL-compressed section (code " + std::to_string(rc) + ")";
        return false;
    }
    out.resize(dest_len);
    return true;
}

}  // namespace

bool load_elf(const std::string &path, ElfImage &out, std::string &error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 52) {
        error = "file too small to be an ELF32 object";
        return false;
    }
    if (!(data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')) {
        error = "not an ELF file (bad magic)";
        return false;
    }
    if (data[4] != 1) {  // EI_CLASS: ELFCLASS32
        error = "only ELF32 is supported";
        return false;
    }
    if (data[5] != 2) {  // EI_DATA: ELFDATA2MSB
        error = "only big-endian ELF is supported (expected a PowerPC object)";
        return false;
    }

    uint32_t e_shoff = rd_be32(&data[32]);
    uint16_t e_shentsize = rd_be16(&data[46]);
    uint16_t e_shnum = rd_be16(&data[48]);
    uint16_t e_shstrndx = rd_be16(&data[50]);

    if (e_shoff == 0 || e_shnum == 0) {
        error = "ELF has no section headers";
        return false;
    }

    auto shdr = [&](int i) -> const uint8_t * { return &data[e_shoff + (size_t)i * e_shentsize]; };

    // Decompress (or plain-copy) every section's real content up front --
    // section names, symbols, relocations, and .text are all potentially
    // RPL-compressed in a real .rpx, so nothing downstream can safely read
    // straight from `data` at a raw file offset anymore. Indexed the same
    // way as the section headers themselves.
    std::vector<std::vector<uint8_t>> section_data(e_shnum);
    for (int i = 0; i < e_shnum; i++) {
        const uint8_t *sh = shdr(i);
        uint32_t sh_type = rd_be32(sh + 4);
        uint32_t sh_flags = rd_be32(sh + 8);
        uint32_t sh_offset = rd_be32(sh + 16);
        uint32_t sh_size = rd_be32(sh + 20);
        if (!read_section_bytes(data, sh_type, sh_flags, sh_offset, sh_size, section_data[i], error)) {
            error = "section " + std::to_string(i) + ": " + error;
            return false;
        }
    }

    const std::vector<uint8_t> &shstrtab = section_data[e_shstrndx];
    auto secname = [&](uint32_t name_off) -> std::string {
        if (name_off >= shstrtab.size()) return {};
        const char *s = reinterpret_cast<const char *>(&shstrtab[name_off]);
        return std::string(s);
    };

    int text_idx = -1;
    int symtab_idx = -1;
    int strtab_idx = -1;
    int rela_text_idx = -1;

    for (int i = 0; i < e_shnum; i++) {
        const uint8_t *sh = shdr(i);
        uint32_t name_off = rd_be32(sh + 0);
        uint32_t sh_type = rd_be32(sh + 4);
        uint32_t sh_addr = rd_be32(sh + 12);
        uint32_t sh_size = rd_be32(sh + 20);
        std::string name = secname(name_off);
        if (name == ".text") text_idx = i;
        if (sh_type == 2 /* SHT_SYMTAB */) symtab_idx = i;
        if (name == ".strtab") strtab_idx = i;
        if (name == ".rela.text" && sh_type == SHT_RELA) rela_text_idx = i;

        // Keep the real bytes of every real (non-bss) section so DataReloc
        // lookups can read constants directly out of e.g. .rodata.cst4.
        if (sh_type != 0 && sh_type != SHT_NOBITS && !name.empty()) {
            out.section_bytes[name] = section_data[i];
        }
        // Real declared size of every named section, .bss included -- see
        // ElfImage::section_sizes' own comment on why this can't just be
        // derived from section_bytes.
        if (sh_type != 0 && !name.empty()) {
            out.section_sizes[name] = sh_size;
            out.section_real_addr[name] = sh_addr;
        }
    }

    if (text_idx < 0) {
        error = ".text section not found";
        return false;
    }

    const uint8_t *text_sh = shdr(text_idx);
    uint32_t text_addr = rd_be32(text_sh + 12);

    out.text = section_data[text_idx];
    out.text_addr = text_addr;
    out.entry = rd_be32(&data[24]);  // e_entry

    // No symbol table means a stripped binary -- not an error here. Caller
    // decides whether to fall back to heuristic function recovery
    // (func_recovery.h); ElfImage::functions is simply left empty.
    if (symtab_idx < 0 || strtab_idx < 0) {
        return true;
    }

    const uint8_t *sym_sh = shdr(symtab_idx);
    uint32_t sym_entsize = rd_be32(sym_sh + 36);
    if (sym_entsize == 0) sym_entsize = 16;

    const std::vector<uint8_t> &symtab_bytes = section_data[symtab_idx];
    const std::vector<uint8_t> &strtab_bytes = section_data[strtab_idx];

    uint32_t nsyms = (uint32_t)(symtab_bytes.size() / sym_entsize);
    std::vector<std::string> all_sym_names(nsyms);
    // For STT_SECTION symbols (what data relocations normally target), the
    // symbol's own name is empty -- the name that matters is the section
    // it points at, looked up via st_shndx.
    std::vector<std::string> sym_section_names(nsyms);
    std::vector<uint8_t> sym_types(nsyms);
    std::vector<uint32_t> sym_values(nsyms);
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *sym = &symtab_bytes[(size_t)i * sym_entsize];
        uint32_t st_name = rd_be32(sym + 0);
        uint32_t st_value = rd_be32(sym + 4);
        uint32_t st_size = rd_be32(sym + 8);
        uint8_t st_info = sym[12];
        uint16_t st_shndx = rd_be16(sym + 14);

        if (st_name < strtab_bytes.size()) {
            all_sym_names[i] =
                sanitize_c_identifier(reinterpret_cast<const char *>(&strtab_bytes[st_name]));
        }
        if (st_shndx < e_shnum) {
            sym_section_names[i] = secname(rd_be32(shdr(st_shndx) + 0));
        }
        sym_types[i] = st_info & 0xf;
        sym_values[i] = st_value;

        int type = st_info & 0xf;
        if (st_shndx != (uint16_t)text_idx) continue;
        bool is_savres_helper = type == 0 && (all_sym_names[i].rfind("_savegpr_", 0) == 0 ||
                                               all_sym_names[i].rfind("_restgpr_", 0) == 0 ||
                                               all_sym_names[i].rfind("_savefpr_", 0) == 0 ||
                                               all_sym_names[i].rfind("_restfpr_", 0) == 0);
        // Real compiler-generated register-spill helpers are individually
        // typed STT_NOTYPE (0), not STT_FUNC -- only the first entry point
        // in each chain (e.g. _savegpr_14) is STT_FUNC with a real size
        // covering the whole shared-tail chain; the others (_savegpr_29,
        // _savegpr_31, ...) are plain global labels into the *middle* of
        // that same code, confirmed by name and by hand-decoding the real
        // bytes at one of them (see scan_forward_for_blr_size's comment).
        // .text also holds ~44k *other* STT_NOTYPE symbols (compiler
        // branch-target labels like ".L47149") that must NOT be treated as
        // functions, hence the name-prefix allowlist rather than accepting
        // every STT_NOTYPE symbol in .text.
        if (type != STT_FUNC && !is_savres_helper) continue;

        // GHS emits real, genuine STT_FUNC symbols purely for source-level
        // debug info -- begin-of-file/end-of-file markers (raw names shaped
        // like "..bof.C.3A.5CDev..." / "..eof...", sanitized above to
        // "__bof_.../__eof_...", plus a "..bof.trg..."/"..eof.trg..."
        // variant and a shorter "..b../..e.." form, all confirmed present
        // in the real Skylanders: Spyro's Adventure binary) that always
        // share the exact same start address as the real function whose
        // source line they're marking -- confirmed by hand: e.g. a real
        // `__bof_..hkFixedSizeAllocator..` symbol and the real, correctly
        // STT_FUNC-sized `blockAlloc__20hkFixedSizeAllocatorFi` both start
        // at 0x254a57c, and a real `__b___..gfdInterface..` symbol and
        // `_GFDGetHeaderVersions` both start at 0x2579a0c. These debug
        // markers carry st_size=0 (they own no real code of their own),
        // which used to trigger scan_forward_for_blr_size's fallback --
        // designed for the different, real _savegpr_*-style shared-tail
        // case above, where the blr genuinely belongs to the symbol being
        // sized. Here it doesn't: the scan finds whatever blr comes first
        // in the *real* function's body (or a neighboring one) and gives
        // this debug marker a bogus, truncated size covering only part of
        // that real function -- producing a second, broken "function" that
        // duplicates the real one's early instructions and then hits a
        // conditional branch whose target lands outside its own
        // (wrongly-truncated) range, emitting an "unresolved conditional
        // tail call" #error. Confirmed as the real cause of the large
        // majority (167 of ~200) of such errors when recompiling the real
        // game binary; skipped here rather than sized, since they never
        // own real, independently-callable code to begin with.
        bool is_debug_marker = all_sym_names[i].rfind("__bof_", 0) == 0 ||
                                all_sym_names[i].rfind("__eof_", 0) == 0 ||
                                all_sym_names[i].rfind("__b___", 0) == 0 ||
                                all_sym_names[i].rfind("__e___", 0) == 0;
        if (is_debug_marker) continue;

        uint32_t size = st_size;
        if (size == 0) {
            // See scan_forward_for_blr_size's comment -- these are real,
            // deliberately zero-sized linker helper symbols, not garbage.
            size = scan_forward_for_blr_size(out.text, out.text_addr, st_value);
            if (size == 0) continue;  // genuinely couldn't recover a size; skip as before
        }

        ElfFunction fn;
        fn.name = all_sym_names[i];
        fn.addr = st_value;
        fn.size = size;
        out.functions.push_back(fn);
    }

    // A symtab with no FUNC entries (e.g. it only kept debug-info-related
    // symbols) is handled the same way as a missing symtab -- caller falls
    // back to heuristic recovery.

    if (rela_text_idx >= 0) {
        const uint8_t *rela_sh = shdr(rela_text_idx);
        uint32_t rela_entsize = rd_be32(rela_sh + 36);
        if (rela_entsize == 0) rela_entsize = 12;

        const std::vector<uint8_t> &rela_bytes = section_data[rela_text_idx];
        uint32_t nrelocs = (uint32_t)(rela_bytes.size() / rela_entsize);
        for (uint32_t i = 0; i < nrelocs; i++) {
            const uint8_t *r = &rela_bytes[(size_t)i * rela_entsize];
            uint32_t r_offset = rd_be32(r + 0);
            uint32_t r_info = rd_be32(r + 4);
            int32_t r_addend = (int32_t)rd_be32(r + 8);
            uint32_t r_sym = r_info >> 8;
            uint32_t r_type = r_info & 0xff;

            if (r_sym >= all_sym_names.size()) continue;

            if (r_type == R_PPC_REL24) {
                // Real GHS-linked retail code doesn't always route
                // cross-library calls through a separate lis/ori/mtctr/
                // bctr trampoline stub (see find_import_trampolines) --
                // confirmed against the real Skylanders: Spyro's
                // Adventure binary, where every import (memcpy,
                // OSGetTime, GX2SetTVGamma, ...) is instead called via a
                // plain `bl` straight at the .fimport_<library> symbol,
                // with the real Cafe OS RPL loader patching this exact
                // relocation at load time. Recognized here so codegen
                // resolves it as a named external system call
                // (ppc_import_<library>_<function>) instead of treating
                // "memcpy"/"OSGetTime"/etc. as if they were local
                // functions meant to be linked in from another
                // recompiled object -- a real, not hypothetical, name
                // collision risk (a game can easily define its own
                // "memcpy"-named helper too).
                if (sym_types[r_sym] == STT_FUNC && sym_section_names[r_sym].rfind(kImportSectionPrefix, 0) == 0) {
                    ImportTrampoline it;
                    it.library = strip_rpl_suffix(sym_section_names[r_sym].substr(sizeof(kImportSectionPrefix) - 1));
                    it.function = all_sym_names[r_sym];
                    out.import_trampolines[r_offset] = it;
                } else {
                    out.call_relocs[r_offset] = all_sym_names[r_sym];
                }
            } else if (r_type == R_PPC_ADDR16_HA || r_type == R_PPC_ADDR16_LO || r_type == R_PPC_ADDR16_HI) {
                // Unlike REL24 (which points at the instruction's own start
                // address), ADDR16_HA/HI/LO point at byte offset+2 within
                // the instruction word -- the low halfword, in PPC's
                // big-endian encoding. Align down to recover the
                // instruction's address so this can be looked up the same
                // way as everything else keyed by insn.address.
                DataReloc dr;
                dr.type = (r_type == R_PPC_ADDR16_LO)   ? DataReloc::LO
                          : (r_type == R_PPC_ADDR16_HI) ? DataReloc::HI
                                                         : DataReloc::HA;
                dr.section = sym_section_names[r_sym];
                // Real, severe bug found and fixed 2026-08-20, confirmed by
                // directly inspecting the real symbol table: r_addend alone
                // is only the correct section-relative offset when r_sym is
                // a real STT_SECTION symbol (whose own st_value is the
                // section's own base, contributing nothing extra). This
                // binary's real relocations instead target real, named
                // STT_OBJECT symbols directly (confirmed: e.g. a real
                // `__gGlobalMem` FMOD global at real address 0x10172524 and
                // a real, completely unrelated `__mallocInfo` heap-control
                // struct at real address 0x100ee258, both referenced with
                // r_addend=0) -- silently discarding each symbol's own real
                // st_value collapsed every such symbol referenced at its
                // own offset 0 onto the exact same synthetic address,
                // aliasing genuinely unrelated globals on top of each
                // other (confirmed as the real, direct cause of a real
                // heap-corruption hang: the FMOD global and malloc's own
                // heap struct landing at the same address, ~528KB apart on
                // real hardware). st_value - the section's own real sh_addr
                // (section_real_addr; 0 for a relocatable .o, where
                // st_value is already section-relative on its own, making
                // this a no-op) gives the real, correct section-relative
                // offset in both cases -- for a genuine section symbol,
                // st_value equals the section's own real base, so this
                // formula still correctly reduces to plain r_addend there
                // too, same real result as before for the (apparently
                // common) case that was already working.
                {
                    uint32_t sec_real_base = 0;
                    auto sec_addr_it = out.section_real_addr.find(dr.section);
                    if (sec_addr_it != out.section_real_addr.end()) sec_real_base = sec_addr_it->second;
                    dr.addend = (int32_t)(sym_values[r_sym] - sec_real_base) + r_addend;
                }
                if (sym_types[r_sym] == STT_FUNC && dr.section.rfind(kImportSectionPrefix, 0) == 0) {
                    // RPL cross-library import (e.g. a coreinit function
                    // called via a linker-synthesized trampoline stub) --
                    // see ImportTrampoline. Checked before the plain
                    // is_function case below since import symbols are also
                    // STT_FUNC, just not local code.
                    dr.is_import = true;
                    dr.import_library = strip_rpl_suffix(dr.section.substr(sizeof(kImportSectionPrefix) - 1));
                    dr.import_function = all_sym_names[r_sym];
                } else if (sym_types[r_sym] == STT_FUNC) {
                    // &function idiom (e.g. building a function-pointer
                    // table) -- same lis+addi relocation pair as addressing
                    // mutable data, but the value needed is the function's
                    // real entry address, not a synthetic mem[] address.
                    dr.is_function = true;
                    dr.func_name = all_sym_names[r_sym];
                    dr.func_addr = sym_values[r_sym] + (uint32_t)r_addend;
                }
                out.data_relocs[r_offset & ~3u] = dr;
            }
        }
    }

    return true;
}

void assign_global_addrs(ElfImage &img) {
    // Fixed, generous starting point in PpcContext::mem -- well clear of
    // where these small test programs' stacks operate (mem is 4MB as of
    // this writing, grown from an original 65536 bytes -- see
    // ppc_runtime.h's own PpcContext::mem comment for why; the stack
    // starts near the top and grows down). Each section gets at
    // least 256 bytes of room, rounded up, which is plenty for this
    // milestone's scope but is a real, documented limitation: a genuinely
    // large .data/.bss (as real Wii U game code will have) would need a
    // proper size-aware allocator, not this fixed-stride placeholder.
    //
    // Read-only (.rodata*) sections get a real synthetic address here too,
    // same as mutable .data/.bss -- see is_synthetic_addr_lo_reloc in
    // codegen.cpp for why an earlier compile-time-constant-fold approach
    // for rodata was wrong in general (it only handled a single scalar
    // load, not e.g. a switch-statement jump/lookup table's *address*
    // being taken for later runtime-indexed access).
    uint32_t next_addr = 0x2000;
    for (const auto &entry : img.data_relocs) {
        if (entry.second.is_function) continue;  // handled separately, see codegen.cpp
        if (entry.second.is_import) continue;    // not a real address at all, see find_import_trampolines
        const std::string &section = entry.second.section;
        if (img.global_section_base.count(section)) continue;

        img.global_section_base[section] = next_addr;
        // Real bug, found 2026-08-20 debugging a real hang: section_bytes
        // is deliberately empty for .bss (SHT_NOBITS has no file content
        // to copy -- see load_elf), so sizing purely off it silently gave
        // every .bss-only section just the 256-byte placeholder minimum
        // below, no matter how big it actually declared itself in the
        // section header. A real, complex GHS-compiled game's .bss is
        // nowhere near that small (confirmed: this project's own
        // Skylanders target has a real, ~622KB .bss) -- every global that
        // didn't fit was silently aliasing on top of another, corrupting
        // real cached state (e.g. a lazily-cached heap handle read back as
        // 0/garbage) without ever touching a single genuinely-unhandled
        // instruction or a bounds check, so it never surfaced as a
        // compile error or a crash, only as wrong runtime behavior. Now
        // uses the section's real, declared sh_size (section_sizes,
        // populated for every named section including .bss) as the real
        // size, falling back to section_bytes/256 only for a section this
        // real binary's own section headers never declared at all.
        size_t sz = 256;
        auto sz_it = img.section_sizes.find(section);
        if (sz_it != img.section_sizes.end() && sz_it->second > sz) {
            sz = sz_it->second;
        } else {
            auto it = img.section_bytes.find(section);
            if (it != img.section_bytes.end() && it->second.size() > sz) sz = it->second.size();
        }
        next_addr += (uint32_t)((sz + 15) & ~15u); // round up to 16
    }
}

void find_import_trampolines(ElfImage &img) {
    for (const auto &entry : img.data_relocs) {
        uint32_t addr = entry.first;
        const DataReloc &reloc = entry.second;
        if (!reloc.is_import || reloc.type != DataReloc::HI) continue;
        if (addr < img.text_addr) continue;
        uint32_t off = addr - img.text_addr;
        // lis/ori(or addi)/mtctr/bctr -- 16 bytes, 4 instructions.
        if (off + 16 > img.text.size()) continue;

        DisasmResult insns;
        std::string err;
        if (!disassemble_range(&img.text[off], 16, addr, insns, err) || insns.size() < 4) continue;

        bool is_lis = insns[0].id == PPC_INS_LIS;
        bool is_lo_half = insns[1].id == PPC_INS_ORI || insns[1].id == PPC_INS_ADDI || insns[1].id == PPC_INS_NOP;
        bool is_mtctr = insns[2].id == PPC_INS_MTCTR;
        bool is_bctr = insns[3].id == PPC_INS_BCTR;
        if (is_lis && is_lo_half && is_mtctr && is_bctr) {
            img.import_trampolines[addr] = {reloc.import_library, reloc.import_function};
        }
    }
}

}  // namespace recomp
