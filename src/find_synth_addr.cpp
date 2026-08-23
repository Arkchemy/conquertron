// Standalone ground-truth lookup, 2026-08-24 (cont.): reverse a synthetic
// PpcContext::mem[] address (as observed live on hardware, e.g. a pool's
// own vtable pointer word) back to its real Wii U vaddr, by scanning every
// real data_reloc for one whose (section_base + addend) == the synthetic
// address we're after. Works because any address a real pool object holds
// at runtime (like a vtable pointer) had to have been *computed* somewhere
// in real code via a real lis+addi/addic pair, which is exactly what
// data_relocs records -- same underlying mechanism already proven correct
// by verify_vtable.cpp, just searching instead of a single hardcoded query.
// Once found, dumps real .rodata bytes at a given byte offset from that
// real vaddr (e.g. a specific vtable slot), cross-referenced against
// img.functions -- lets us read real vtable slot contents directly from
// the real, legally-dumped RPX without needing another hardware round.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "elf_loader.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <path-to-real-rpx> <synthetic-addr-hex> [byte-offset-hex] [dump-words]\n", argv[0]);
        return 1;
    }
    recomp::ElfImage img;
    std::string err;
    if (!recomp::load_elf(argv[1], img, err)) {
        fprintf(stderr, "load_elf failed: %s\n", err.c_str());
        return 1;
    }
    printf("Loaded OK. text_addr=0x%x functions=%zu data_relocs=%zu sections=%zu\n",
           img.text_addr, img.functions.size(), img.data_relocs.size(), img.section_bytes.size());

    recomp::assign_global_addrs(img);

    uint32_t target_synth = (uint32_t)strtoul(argv[2], nullptr, 16);
    uint32_t byte_offset = argc >= 4 ? (uint32_t)strtoul(argv[3], nullptr, 16) : 0;
    int dump_words = argc >= 5 ? atoi(argv[4]) : 8;

    bool found = false;
    for (const auto &entry : img.data_relocs) {
        const recomp::DataReloc &dr = entry.second;
        if (dr.is_function || dr.is_import) continue;
        auto base_it = img.global_section_base.find(dr.section);
        if (base_it == img.global_section_base.end()) continue;
        uint32_t computed_synth = base_it->second + (uint32_t)dr.addend;
        if (computed_synth != target_synth) continue;

        found = true;
        printf("MATCH: real PPC instruction at 0x%x references synthetic 0x%x -- section='%s' addend=%d (section synth base=0x%x)\n",
               entry.first, target_synth, dr.section.c_str(), dr.addend, base_it->second);

        auto sec_it = img.section_bytes.find(dr.section);
        if (sec_it == img.section_bytes.end()) {
            printf("  Section '%s' has no bytes recorded (bss or unrecognized).\n", dr.section.c_str());
            continue;
        }
        const std::vector<uint8_t> &bytes = sec_it->second;
        uint32_t real_section_addr = 0;
        auto ra_it = img.section_real_addr.find(dr.section);
        if (ra_it != img.section_real_addr.end()) real_section_addr = ra_it->second;
        uint32_t real_base_addr = real_section_addr + (uint32_t)dr.addend;
        printf("  real base vaddr = 0x%x. Dumping %d words starting at +0x%x:\n", real_base_addr, dump_words, byte_offset);
        for (int i = 0; i < dump_words; i++) {
            size_t off = (size_t)dr.addend + (size_t)byte_offset + (size_t)i * 4;
            if (off + 4 > bytes.size()) { printf("    [%d] out of bounds\n", i); continue; }
            uint32_t val = ((uint32_t)bytes[off] << 24) | ((uint32_t)bytes[off+1] << 16) |
                           ((uint32_t)bytes[off+2] << 8) | (uint32_t)bytes[off+3];
            printf("    [%d] real vaddr 0x%x = 0x%08x", i, real_base_addr + (uint32_t)(byte_offset + i * 4), val);
            for (const auto &fn : img.functions) {
                if (fn.addr == val) { printf("  -> %s", fn.name.c_str()); break; }
            }
            printf("\n");
        }
    }

    if (!found) {
        printf("No data_reloc found whose computed synthetic address equals 0x%x.\n", target_synth);
        printf("Nearby synthetic addresses that DO exist (for context):\n");
        int shown = 0;
        for (const auto &entry : img.data_relocs) {
            const recomp::DataReloc &dr = entry.second;
            if (dr.is_function || dr.is_import) continue;
            auto base_it = img.global_section_base.find(dr.section);
            if (base_it == img.global_section_base.end()) continue;
            uint32_t computed_synth = base_it->second + (uint32_t)dr.addend;
            if (computed_synth > target_synth - 0x100 && computed_synth < target_synth + 0x100) {
                printf("  0x%x (section=%s addend=%d)\n", computed_synth, dr.section.c_str(), dr.addend);
                if (++shown > 20) break;
            }
        }
        return 1;
    }
    return 0;
}
