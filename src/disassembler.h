#ifndef BRAMBLE_DISASSEMBLER_H
#define BRAMBLE_DISASSEMBLER_H

#include <cstdint>
#include <string>

#include <capstone/capstone.h>

namespace recomp {

// Owns a Capstone-allocated instruction array (cs_insn* + count) and frees
// it via cs_free on destruction. Move-only.
class DisasmResult {
public:
    DisasmResult() = default;
    DisasmResult(cs_insn *insns, size_t count) : insns_(insns), count_(count) {}
    DisasmResult(const DisasmResult &) = delete;
    DisasmResult &operator=(const DisasmResult &) = delete;
    DisasmResult(DisasmResult &&other) noexcept : insns_(other.insns_), count_(other.count_) {
        other.insns_ = nullptr;
        other.count_ = 0;
    }
    DisasmResult &operator=(DisasmResult &&other) noexcept {
        if (this != &other) {
            free();
            insns_ = other.insns_;
            count_ = other.count_;
            other.insns_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }
    ~DisasmResult() { free(); }

    const cs_insn *data() const { return insns_; }
    size_t size() const { return count_; }
    const cs_insn &operator[](size_t i) const { return insns_[i]; }

private:
    void free() {
        if (insns_) cs_free(insns_, count_);
        insns_ = nullptr;
        count_ = 0;
    }
    cs_insn *insns_ = nullptr;
    size_t count_ = 0;
};

// Disassembles `size` bytes of big-endian PPC32 code starting at `addr`
// (addr is used only to compute each instruction's address field, matching
// how ElfFunction::addr is a .text-relative offset for this milestone).
// Returns false and sets `error` if Capstone itself fails to initialize or
// open a handle.
bool disassemble_range(const uint8_t *code, size_t size, uint32_t addr, DisasmResult &out, std::string &error);

}  // namespace recomp

#endif  // BRAMBLE_DISASSEMBLER_H
