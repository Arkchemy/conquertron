#include "elf_loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>

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
constexpr uint32_t R_PPC_REL24 = 10;

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
        std::string name = secname(name_off);
        if (name == ".text") text_idx = i;
        if (sh_type == 2 /* SHT_SYMTAB */) symtab_idx = i;
        if (name == ".strtab") strtab_idx = i;
        if (name == ".rela.text" && sh_type == SHT_RELA) rela_text_idx = i;
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
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *sym = &data[sym_off + (size_t)i * sym_entsize];
        uint32_t st_name = rd_be32(sym + 0);
        uint32_t st_value = rd_be32(sym + 4);
        uint32_t st_size = rd_be32(sym + 8);
        uint8_t st_info = sym[12];
        uint16_t st_shndx = rd_be16(sym + 14);

        const char *name = reinterpret_cast<const char *>(&data[str_off + st_name]);
        all_sym_names[i] = name;

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
            uint32_t r_sym = r_info >> 8;
            uint32_t r_type = r_info & 0xff;

            if (r_type != R_PPC_REL24) continue;  // only `bl`-style call relocations
            if (r_sym >= all_sym_names.size()) continue;

            out.call_relocs[r_offset] = all_sym_names[r_sym];
        }
    }

    return true;
}

}  // namespace recomp
