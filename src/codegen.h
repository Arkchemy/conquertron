#ifndef PORTALRECOMP_CODEGEN_H
#define PORTALRECOMP_CODEGEN_H

#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "disassembler.h"
#include "elf_loader.h"

namespace recomp {

// Emits one C function (named "ppc_<symbol>") that reproduces `func`'s
// behaviour against a PpcContext*. Any instruction outside the milestone's
// supported subset is emitted as a `#error` line (naming the unhandled
// mnemonic and address) rather than silently mistranslated -- gaps must be
// visible, not guessed at.
//
// `call_relocs` resolves `bl` targets in a relocatable object (see
// ElfImage::call_relocs). `addr_to_name` is a fallback for already-linked
// binaries, where a `bl`'s raw immediate operand is a real, directly usable
// target address (no relocation left to resolve) -- it maps a function's
// start address to its name, from the same function list codegen is being
// run over. A `bl` unresolved by either is treated as unhandled, same as
// any other unsupported instruction.
//
// Returns the mnemonics of any unhandled instructions encountered (empty on
// full success).
std::vector<std::string> generate_function_c(const ElfFunction &func, const DisasmResult &insns,
                                              const std::map<uint32_t, std::string> &call_relocs,
                                              const std::map<uint32_t, std::string> &addr_to_name, std::ostream &out);

}  // namespace recomp

#endif  // PORTALRECOMP_CODEGEN_H
