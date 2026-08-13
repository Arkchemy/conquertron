#include "func_recovery.h"

#include <algorithm>
#include <set>
#include <sstream>

#include "disassembler.h"

namespace recomp {

std::vector<ElfFunction> recover_functions_heuristic(const std::vector<uint8_t> &text, uint32_t text_addr,
                                                      uint32_t entry_addr, std::string &error) {
    std::vector<ElfFunction> out;

    DisasmResult all;
    if (!disassemble_range(text.data(), text.size(), text_addr, all, error)) {
        return out;
    }

    std::set<uint32_t> starts;
    starts.insert(entry_addr);
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i].id == PPC_INS_BL && all[i].detail->ppc.op_count >= 1) {
            starts.insert((uint32_t)all[i].detail->ppc.operands[0].imm);
        }
    }

    std::vector<uint32_t> sorted_starts(starts.begin(), starts.end());
    std::sort(sorted_starts.begin(), sorted_starts.end());

    uint32_t text_end = text_addr + (uint32_t)text.size();
    for (size_t i = 0; i < sorted_starts.size(); i++) {
        uint32_t start = sorted_starts[i];
        uint32_t end = (i + 1 < sorted_starts.size()) ? sorted_starts[i + 1] : text_end;
        if (start < text_addr || end > text_end || end <= start) continue;

        std::ostringstream name;
        name << "func_" << std::hex << start;

        ElfFunction fn;
        fn.name = name.str();
        fn.addr = start;
        fn.size = end - start;
        out.push_back(fn);
    }

    return out;
}

}  // namespace recomp
