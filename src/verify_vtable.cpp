// Standalone ground-truth check, 2026-08-24: does the real, retail RPX's
// own .rodata really only have 3 populated vtable slots (resetByReference/
// construct/destruct) for the pool-placeholder vtable this session traced
// to real address 0x10f738 (synthetic) / computed via `lis r0,0x1005;
// addic r0,r0,0x41dc` at real PPC address 0x217a21c -- or did this
// project's own recompiler mis-extract/truncate it from the real binary?
// Uses this project's own, already-tested elf_loader.cpp directly against
// the real dumped RPX -- no live emulator involved, so nothing to crash.
#include <cstdio>
#include <cstdint>
#include "elf_loader.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-real-rpx>\n", argv[0]);
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

    // The HA instruction (`lis r0, 0x1005`) at real PPC address 0x217a21c --
    // the pool-initialization function's vtable-stamp computation.
    uint32_t query_addr = 0x217a21cu;
    auto it = img.data_relocs.find(query_addr);
    if (it == img.data_relocs.end()) {
        printf("No data_reloc found at 0x%x -- trying the LO partner at 0x217a224 instead.\n", query_addr);
        query_addr = 0x217a224u;
        it = img.data_relocs.find(query_addr);
    }
    if (it == img.data_relocs.end()) {
        printf("No data_reloc found at either candidate address. Dumping nearby data_relocs for context:\n");
        for (const auto &entry : img.data_relocs) {
            if (entry.first >= 0x217a1f0u && entry.first <= 0x217a240u) {
                printf("  0x%x -> section=%s addend=%d is_function=%d func_name=%s\n",
                       entry.first, entry.second.section.c_str(), entry.second.addend,
                       entry.second.is_function, entry.second.func_name.c_str());
            }
        }
        return 1;
    }

    const recomp::DataReloc &dr = it->second;
    printf("Found data_reloc at 0x%x: section=%s addend=%d is_function=%d func_name=%s\n",
           query_addr, dr.section.c_str(), dr.addend, dr.is_function, dr.func_name.c_str());

    recomp::assign_global_addrs(img);
    auto base_it = img.global_section_base.find(dr.section);
    if (base_it == img.global_section_base.end()) {
        printf("!! Section '%s' has NO entry in global_section_base at all!\n", dr.section.c_str());
    } else {
        uint32_t computed_synth = base_it->second + (uint32_t)dr.addend;
        printf("assign_global_addrs: section '%s' base=%u (0x%x), this reloc's synthetic addr = %u (0x%x)\n",
               dr.section.c_str(), base_it->second, base_it->second, computed_synth, computed_synth);
        printf("Expected (from real hardware trace, recompiled build's own literal): 0x10f738 (1112888)\n");
        printf("Match? %s\n", computed_synth == 0x10f738u ? "YES" : "NO -- MISMATCH");
    }

    auto sec_it = img.section_bytes.find(dr.section);
    if (sec_it == img.section_bytes.end()) {
        printf("Section '%s' has no bytes recorded (bss or unrecognized).\n", dr.section.c_str());
        return 1;
    }
    const std::vector<uint8_t> &bytes = sec_it->second;
    uint32_t real_section_addr = 0;
    auto ra_it = img.section_real_addr.find(dr.section);
    if (ra_it != img.section_real_addr.end()) real_section_addr = ra_it->second;
    printf("Section '%s': %zu real bytes, real sh_addr=0x%x, real vtable addr=0x%x\n",
           dr.section.c_str(), bytes.size(), real_section_addr, real_section_addr + (uint32_t)dr.addend);

    int dump_words = argc >= 3 ? atoi(argv[2]) : 8;
    printf("Raw vtable dump (%d words) from real .rodata, starting at addend %d:\n", dump_words, dr.addend);
    for (int i = 0; i < dump_words; i++) {
        size_t off = (size_t)dr.addend + (size_t)i * 4;
        if (off + 4 > bytes.size()) { printf("  [%d] out of bounds\n", i); continue; }
        uint32_t val = ((uint32_t)bytes[off] << 24) | ((uint32_t)bytes[off+1] << 16) |
                       ((uint32_t)bytes[off+2] << 8) | (uint32_t)bytes[off+3];
        printf("  [%d] real vaddr 0x%x = 0x%08x", i, real_section_addr + (uint32_t)off, val);
        // cross-reference against the real function table
        for (const auto &fn : img.functions) {
            if (fn.addr == val) { printf("  -> %s", fn.name.c_str()); break; }
        }
        printf("\n");
    }
    return 0;
}
