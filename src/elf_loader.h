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
};

// Loads a big-endian, 32-bit ELF relocatable object (as produced by
// `zig cc -target powerpc-freestanding-eabi -c`) and extracts its .text
// section, FUNC symbols defined within it, and R_PPC_REL24 call relocations
// (see ElfImage::call_relocs). Other relocation types (data references,
// absolute calls, etc.) are not handled yet -- a real Wii U .rpx will need
// broader relocation support, tracked as follow-up work.
bool load_elf(const std::string &path, ElfImage &out, std::string &error);

}  // namespace recomp

#endif  // PORTALRECOMP_ELF_LOADER_H
