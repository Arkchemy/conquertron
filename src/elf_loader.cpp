#include "elf_loader.h"

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

constexpr int STT_FUNC = 2;
constexpr uint32_t SHT_RELA = 4;
constexpr uint32_t SHT_NOBITS = 8;
constexpr uint32_t R_PPC_ADDR16_LO = 4;
constexpr uint32_t R_PPC_ADDR16_HI = 5;
constexpr uint32_t R_PPC_ADDR16_HA = 6;
constexpr uint32_t R_PPC_REL24 = 10;

const char kImportSectionPrefix[] = ".fimport_";

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

    uint32_t shstrtab_off = rd_be32(shdr(e_shstrndx) + 16);
    auto secname = [&](uint32_t name_off) -> std::string {
        const char *s = reinterpret_cast<const char *>(&data[shstrtab_off + name_off]);
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
        uint32_t sh_offset = rd_be32(sh + 16);
        uint32_t sh_size = rd_be32(sh + 20);
        std::string name = secname(name_off);
        if (name == ".text") text_idx = i;
        if (sh_type == 2 /* SHT_SYMTAB */) symtab_idx = i;
        if (name == ".strtab") strtab_idx = i;
        if (name == ".rela.text" && sh_type == SHT_RELA) rela_text_idx = i;

        // Keep raw bytes of every real (non-bss) section so DataReloc
        // lookups can read constants directly out of e.g. .rodata.cst4.
        if (sh_type != 0 && sh_type != SHT_NOBITS && !name.empty()) {
            out.section_bytes[name] = std::vector<uint8_t>(data.begin() + sh_offset, data.begin() + sh_offset + sh_size);
        }
    }

    if (text_idx < 0) {
        error = ".text section not found";
        return false;
    }

    const uint8_t *text_sh = shdr(text_idx);
    uint32_t text_addr = rd_be32(text_sh + 12);
    uint32_t text_off = rd_be32(text_sh + 16);
    uint32_t text_size = rd_be32(text_sh + 20);

    out.text.assign(data.begin() + text_off, data.begin() + text_off + text_size);
    out.text_addr = text_addr;
    out.entry = rd_be32(&data[24]);  // e_entry

    // No symbol table means a stripped binary -- not an error here. Caller
    // decides whether to fall back to heuristic function recovery
    // (func_recovery.h); ElfImage::functions is simply left empty.
    if (symtab_idx < 0 || strtab_idx < 0) {
        return true;
    }

    const uint8_t *sym_sh = shdr(symtab_idx);
    uint32_t sym_off = rd_be32(sym_sh + 16);
    uint32_t sym_size = rd_be32(sym_sh + 20);
    uint32_t sym_entsize = rd_be32(sym_sh + 36);
    if (sym_entsize == 0) sym_entsize = 16;

    const uint8_t *str_sh = shdr(strtab_idx);
    uint32_t str_off = rd_be32(str_sh + 16);

    uint32_t nsyms = sym_size / sym_entsize;
    std::vector<std::string> all_sym_names(nsyms);
    // For STT_SECTION symbols (what data relocations normally target), the
    // symbol's own name is empty -- the name that matters is the section
    // it points at, looked up via st_shndx.
    std::vector<std::string> sym_section_names(nsyms);
    std::vector<uint8_t> sym_types(nsyms);
    std::vector<uint32_t> sym_values(nsyms);
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *sym = &data[sym_off + (size_t)i * sym_entsize];
        uint32_t st_name = rd_be32(sym + 0);
        uint32_t st_value = rd_be32(sym + 4);
        uint32_t st_size = rd_be32(sym + 8);
        uint8_t st_info = sym[12];
        uint16_t st_shndx = rd_be16(sym + 14);

        const char *name = reinterpret_cast<const char *>(&data[str_off + st_name]);
        all_sym_names[i] = name;
        if (st_shndx < e_shnum) {
            sym_section_names[i] = secname(rd_be32(shdr(st_shndx) + 0));
        }
        sym_types[i] = st_info & 0xf;
        sym_values[i] = st_value;

        int type = st_info & 0xf;
        if (type != STT_FUNC) continue;
        if (st_shndx != (uint16_t)text_idx) continue;
        if (st_size == 0) continue;

        ElfFunction fn;
        fn.name = name;
        fn.addr = st_value;
        fn.size = st_size;
        out.functions.push_back(fn);
    }

    // A symtab with no FUNC entries (e.g. it only kept debug-info-related
    // symbols) is handled the same way as a missing symtab -- caller falls
    // back to heuristic recovery.

    if (rela_text_idx >= 0) {
        const uint8_t *rela_sh = shdr(rela_text_idx);
        uint32_t rela_off = rd_be32(rela_sh + 16);
        uint32_t rela_size = rd_be32(rela_sh + 20);
        uint32_t rela_entsize = rd_be32(rela_sh + 36);
        if (rela_entsize == 0) rela_entsize = 12;

        uint32_t nrelocs = rela_size / rela_entsize;
        for (uint32_t i = 0; i < nrelocs; i++) {
            const uint8_t *r = &data[rela_off + (size_t)i * rela_entsize];
            uint32_t r_offset = rd_be32(r + 0);
            uint32_t r_info = rd_be32(r + 4);
            int32_t r_addend = (int32_t)rd_be32(r + 8);
            uint32_t r_sym = r_info >> 8;
            uint32_t r_type = r_info & 0xff;

            if (r_sym >= all_sym_names.size()) continue;

            if (r_type == R_PPC_REL24) {
                out.call_relocs[r_offset] = all_sym_names[r_sym];
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
                dr.addend = r_addend;
                if (sym_types[r_sym] == STT_FUNC && dr.section.rfind(kImportSectionPrefix, 0) == 0) {
                    // RPL cross-library import (e.g. a coreinit function
                    // called via a linker-synthesized trampoline stub) --
                    // see ImportTrampoline. Checked before the plain
                    // is_function case below since import symbols are also
                    // STT_FUNC, just not local code.
                    dr.is_import = true;
                    dr.import_library = dr.section.substr(sizeof(kImportSectionPrefix) - 1);
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
    // where these small test programs' stacks operate (mem is 65536 bytes;
    // the stack starts near the top and grows down). Each section gets at
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
        size_t sz = 256;
        auto it = img.section_bytes.find(section);
        if (it != img.section_bytes.end() && it->second.size() > sz) sz = it->second.size();
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
