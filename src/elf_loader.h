#ifndef PORTALRECOMP_ELF_LOADER_H
#define PORTALRECOMP_ELF_LOADER_H

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

// A `lis`/load-or-store instruction pair addressing a read-only data
// constant (float/double literal pools, mainly) hasn't been linked yet, so
// the address it computes doesn't exist as a real, dereferenceable address
// in our recompiled world. Since the referenced data is always immutable
// (it's in a .rodata* section), codegen resolves these to a literal value
// at compile time instead of simulating address arithmetic -- see
// codegen.cpp's handling of PPC_INS_LIS/consuming-instruction pairs.
struct DataReloc {
    enum Type { HA, LO } type;
    std::string section;  // e.g. ".rodata.cst4"
    int32_t addend = 0;   // byte offset within that section
};

struct ElfImage {
    std::vector<uint8_t> text;
    uint32_t text_addr = 0;  // sh_addr of .text (usually 0 for a relocatable .o)
    uint32_t entry = 0;      // ELF header e_entry (meaningful for linked executables, not relocatable .o)
    std::vector<ElfFunction> functions;

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
};

// Loads a big-endian, 32-bit ELF relocatable object (as produced by
// `zig cc -target powerpc-freestanding-eabi -c`) and extracts its .text
// section, FUNC symbols defined within it, R_PPC_REL24 call relocations,
// and R_PPC_ADDR16_HA/LO data relocations (see ElfImage::call_relocs and
// ::data_relocs). Other relocation types (absolute calls, non-.text data
// writes, etc.) are not handled yet -- a real Wii U .rpx will need broader
// relocation support, tracked as follow-up work.
bool load_elf(const std::string &path, ElfImage &out, std::string &error);

}  // namespace recomp

#endif  // PORTALRECOMP_ELF_LOADER_H
