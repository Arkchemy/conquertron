#ifndef PORTALRECOMP_FUNC_RECOVERY_H
#define PORTALRECOMP_FUNC_RECOVERY_H

#include <cstdint>
#include <string>
#include <vector>

#include "elf_loader.h"

namespace recomp {

// Recovers function boundaries from a stripped .text blob via control-flow
// analysis, for when there's no symbol table to read them from directly
// (ElfFunction's usual source -- see elf_loader.h).
//
// Every `bl` target found by a single linear disassembly pass over the
// whole section becomes a candidate function start. This works because PPC
// instructions are a fixed 4 bytes wide, so decoding linearly from a
// section's start stays correctly aligned throughout, independent of where
// individual functions begin -- no chicken-and-egg problem with needing
// boundaries before you can disassemble.
//
// `entry_addr` seeds the search with the one function guaranteed to exist
// even if nothing inside .text ever calls it via a direct `bl` (the ELF
// entry point).
//
// This deliberately only finds functions reachable via a direct, resolved
// `bl` -- indirect calls (function pointers/vtables) are exactly the case
// the project plan already flags as needing manual review, not automated
// recovery.
std::vector<ElfFunction> recover_functions_heuristic(const std::vector<uint8_t> &text, uint32_t text_addr,
                                                      uint32_t entry_addr, std::string &error);

}  // namespace recomp

#endif  // PORTALRECOMP_FUNC_RECOVERY_H
