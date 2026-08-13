#include "disassembler.h"

namespace recomp {

bool disassemble_range(const uint8_t *code, size_t size, uint32_t addr, DisasmResult &out, std::string &error) {
    csh handle;
    cs_err err = cs_open(CS_ARCH_PPC, (cs_mode)(CS_MODE_32 + CS_MODE_BIG_ENDIAN), &handle);
    if (err != CS_ERR_OK) {
        error = std::string("cs_open failed: ") + cs_strerror(err);
        return false;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    cs_insn *insns = nullptr;
    size_t count = cs_disasm(handle, code, size, addr, 0, &insns);
    cs_close(&handle);

    if (count == 0) {
        error = "capstone failed to disassemble any instructions";
        return false;
    }

    out = DisasmResult(insns, count);
    return true;
}

}  // namespace recomp
