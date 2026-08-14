#ifndef BRAMBLE_DISASSEMBLER_H
#define BRAMBLE_DISASSEMBLER_H

#include <cstdint>
#include <string>
#include <vector>

#include <capstone/capstone.h>

namespace recomp {

// Synthetic instruction IDs for PowerPC 750CL ("Gekko"/Broadway/Espresso)
// "paired-single" SIMD opcodes -- a real vendor extension used pervasively
// in actual Wii U game code that Capstone's generic PPC decoder cannot
// decode at all (confirmed against real Skylanders: Spyro's Adventure
// binary code, not hypothetical). See try_decode_paired_single in
// disassembler.cpp for the hand-rolled decoder and its source. Values are
// chosen well above Capstone's real ppc_insn enum range to avoid collision.
enum {
    PPC_INS_BRAMBLE_PSQ_L = 100000,
    PPC_INS_BRAMBLE_PSQ_LU,
    PPC_INS_BRAMBLE_PSQ_ST,
    PPC_INS_BRAMBLE_PSQ_STU,
    PPC_INS_BRAMBLE_PS_MERGE00,
    PPC_INS_BRAMBLE_PS_MERGE01,
    PPC_INS_BRAMBLE_PS_MERGE10,
    PPC_INS_BRAMBLE_PS_MERGE11,
    // Rest of the paired-single arithmetic family -- added after
    // confirming (against real Skylanders: Spyro's Adventure code) that
    // Capstone doesn't just fail to decode these, it confidently mislabels
    // them as unrelated POWER8+ VMX/VSX instructions (ps_sub as
    // "vmsumshm", ps_add as "vsel", ps_sel as "vmaddfp", ps_div as
    // "vmsumubm" -- all four verified by hand-decoding real instruction
    // words). Encodings from PPCInstrPaired.td (Tilka/llvm-ppc750cl).
    PPC_INS_BRAMBLE_PS_ADD,
    PPC_INS_BRAMBLE_PS_SUB,
    PPC_INS_BRAMBLE_PS_MUL,
    PPC_INS_BRAMBLE_PS_DIV,
    PPC_INS_BRAMBLE_PS_MULS0,
    PPC_INS_BRAMBLE_PS_MULS1,
    PPC_INS_BRAMBLE_PS_RES,
    PPC_INS_BRAMBLE_PS_RSQRTE,
    PPC_INS_BRAMBLE_PS_NEG,
    PPC_INS_BRAMBLE_PS_MR,
    PPC_INS_BRAMBLE_PS_NABS,
    PPC_INS_BRAMBLE_PS_ABS,
    PPC_INS_BRAMBLE_PS_SUM0,
    PPC_INS_BRAMBLE_PS_SUM1,
    PPC_INS_BRAMBLE_PS_MADDS0,
    PPC_INS_BRAMBLE_PS_MADDS1,
    PPC_INS_BRAMBLE_PS_SEL,
    PPC_INS_BRAMBLE_PS_MSUB,
    PPC_INS_BRAMBLE_PS_MADD,
    PPC_INS_BRAMBLE_PS_NMSUB,
    PPC_INS_BRAMBLE_PS_NMADD,
    PPC_INS_BRAMBLE_PS_CMPU0,
    PPC_INS_BRAMBLE_PS_CMPO0,
    PPC_INS_BRAMBLE_PS_CMPU1,
    PPC_INS_BRAMBLE_PS_CMPO1,
};

// Owns a set of individually Capstone-allocated instructions (each a
// separate cs_malloc'd cs_insn, freed one at a time via cs_free(ptr, 1) on
// destruction) and frees them on destruction. Move-only.
//
// Individually-owned (rather than one contiguous cs_disasm block) because
// instructions are decoded one at a time via cs_disasm_iter, with a
// hand-rolled fallback decoder splicing in synthetic entries for
// paired-single opcodes Capstone can't decode -- see disassemble_range in
// disassembler.cpp.
class DisasmResult {
public:
    DisasmResult() = default;
    DisasmResult(const DisasmResult &) = delete;
    DisasmResult &operator=(const DisasmResult &) = delete;
    DisasmResult(DisasmResult &&other) noexcept : insns_(std::move(other.insns_)) { other.insns_.clear(); }
    DisasmResult &operator=(DisasmResult &&other) noexcept {
        if (this != &other) {
            free();
            insns_ = std::move(other.insns_);
            other.insns_.clear();
        }
        return *this;
    }
    ~DisasmResult() { free(); }

    void push_back(cs_insn *insn) { insns_.push_back(insn); }

    size_t size() const { return insns_.size(); }
    const cs_insn &operator[](size_t i) const { return *insns_[i]; }

private:
    void free() {
        for (cs_insn *insn : insns_) cs_free(insn, 1);
        insns_.clear();
    }
    std::vector<cs_insn *> insns_;
};

// Disassembles `size` bytes of big-endian PPC32 code starting at `addr`
// (addr is used only to compute each instruction's address field, matching
// how ElfFunction::addr is a .text-relative offset for this milestone).
// Returns false and sets `error` if Capstone itself fails to initialize or
// open a handle.
bool disassemble_range(const uint8_t *code, size_t size, uint32_t addr, DisasmResult &out, std::string &error);

}  // namespace recomp

#endif  // BRAMBLE_DISASSEMBLER_H
