#include "codegen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace recomp {

namespace {

int reg_idx(unsigned int capstone_reg) {
    return (int)capstone_reg - PPC_REG_R0;
}

int freg_idx(unsigned int capstone_reg) {
    return (int)capstone_reg - PPC_REG_F0;
}

std::string reg(int i) {
    return "ctx->r[" + std::to_string(i) + "]";
}

std::string freg(int i) {
    return "ctx->f[" + std::to_string(i) + "]";
}

// Sign-extending cast: matches how addi/li/mulli/branch-displacement
// immediates are meant to be interpreted.
int32_t simm(const cs_ppc_op &op) { return (int32_t)op.imm; }

// Zero-extending cast: matches ori/lis, where the 16-bit field is placed
// verbatim rather than sign-extended.
uint32_t uimm(const cs_ppc_op &op) { return (uint32_t)(uint16_t)op.imm; }

struct MemOp {
    int base;
    int32_t disp;
};

MemOp mem_operand(const cs_ppc_op &op) { return {reg_idx(op.mem.base), op.mem.disp}; }

std::string base_expr(int base_reg) {
    // Per the PPC ISA, rA==0 in a d(rA) form means literal 0, not the value
    // of r0.
    if (base_reg == 0) return "0u";
    return reg(base_reg);
}

// Standard PPC ISA mask formula for rlwinm/rlwimi-family instructions: a
// contiguous (in PPC bit order, bit 0 = MSB) run of 1 bits from mb to me
// inclusive, wrapping around if mb > me. Computed at codegen time since
// mb/me are always immediates -- the result is baked into the generated C
// as a plain literal, not recomputed at runtime.
uint32_t ppc_mask(int mb, int me) {
    uint32_t mask_begin = 0xFFFFFFFFu >> mb;
    uint32_t mask_end = 0xFFFFFFFFu << (31 - me);
    return (mb <= me) ? (mask_begin & mask_end) : (mask_begin | mask_end);
}

bool is_rodata_section(const std::string &name) { return name.rfind(".rodata", 0) == 0; }

// True if `addr` carries an R_PPC_ADDR16_LO relocation targeting a mutable
// (.data/.bss) section -- i.e. the consuming instruction's base register
// already holds the complete synthetic address (assigned by
// assign_global_addrs, via the paired lis -- see its PPC_INS_LIS case
// below), so the instruction's own displacement/immediate field must be
// ignored rather than added on top of it.
bool is_mutable_lo_reloc(const ElfImage &img, uint32_t addr) {
    auto it = img.data_relocs.find(addr);
    return it != img.data_relocs.end() && it->second.type == DataReloc::LO && !is_rodata_section(it->second.section);
}

// Reads `width` bytes at `reloc.addend` out of the named section's raw
// (big-endian, as stored in the ELF file) bytes. Used to resolve a
// `lis`+load/store pair addressing a read-only data constant -- see
// DataReloc in elf_loader.h.
bool read_data_const(const ElfImage &img, const DataReloc &reloc, uint32_t width, std::vector<uint8_t> &out) {
    auto it = img.section_bytes.find(reloc.section);
    if (it == img.section_bytes.end()) return false;
    const std::vector<uint8_t> &bytes = it->second;
    if (reloc.addend < 0) return false;
    size_t off = (size_t)reloc.addend;
    if (off + width > bytes.size()) return false;
    out.assign(bytes.begin() + off, bytes.begin() + off + width);
    return true;
}

float be_bytes_to_float(const std::vector<uint8_t> &b) {
    uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

// %a (hex float) round-trips exactly and sidesteps any decimal-rounding
// concerns -- valid C99/C++ syntax, understood by both gcc and clang.
std::string hexfloat(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%a", v);
    return buf;
}

}  // namespace

std::vector<std::string> generate_function_c(const ElfImage &img, const ElfFunction &func, const DisasmResult &insns,
                                              const std::map<uint32_t, std::string> &addr_to_name, std::ostream &out) {
    std::vector<std::string> unhandled;

    // Pass 1: collect branch targets within this function so we know where
    // to emit goto labels. (`bl` is excluded -- it's a call, not an
    // intra-function jump, and is handled via img.call_relocs instead.)
    std::set<uint32_t> targets;
    for (size_t i = 0; i < insns.size(); i++) {
        const cs_insn &insn = insns[i];
        bool is_branch = false;
        switch (insn.id) {
            case PPC_INS_B:
            case PPC_INS_BEQ:
            case PPC_INS_BNE:
            case PPC_INS_BLT:
            case PPC_INS_BLE:
            case PPC_INS_BGT:
            case PPC_INS_BGE:
                is_branch = true;
                break;
            default:
                break;
        }
        if (is_branch && insn.detail->ppc.op_count >= 1) {
            targets.insert((uint32_t)insn.detail->ppc.operands[0].imm);
        }
    }

    out << "void ppc_" << func.name << "(PpcContext *ctx) {\n";

    // Tracks GPRs holding the "high half" of a not-yet-linked data address,
    // set by a `lis` with an R_PPC_ADDR16_HA relocation and consumed by the
    // paired load/store's R_PPC_ADDR16_LO relocation. See DataReloc.
    std::map<int, DataReloc> pending_hi;

    for (size_t i = 0; i < insns.size(); i++) {
        const cs_insn &insn = insns[i];
        const cs_ppc &ppc = insn.detail->ppc;

        if (targets.count(insn.address)) {
            // The trailing ";" is a null statement so the label is valid C
            // even if it happens to be the function's last instruction.
            out << "  L_" << std::hex << insn.address << std::dec << ": ;\n";
        }

        out << "  /* " << std::hex << insn.address << std::dec << ": " << insn.mnemonic << " " << insn.op_str
            << " */\n";

        switch (insn.id) {
            case PPC_INS_STWU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_u32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << reg(rD)
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_STW: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                std::string addr_expr = is_mutable_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_u32(ctx, " << addr_expr << ", " << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_LWZ: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                std::string addr_expr = is_mutable_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << addr_expr << ");\n";
                break;
            }
            case PPC_INS_LBZ: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                break;
            }
            case PPC_INS_LHZ: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                break;
            }
            case PPC_INS_LHA: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)ppc_load_u16(ctx, " << base_expr(m.base)
                    << " + (int32_t)" << m.disp << ");\n";
                break;
            }
            case PPC_INS_STB: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_u8(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", (uint8_t)"
                    << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_STH: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_u16(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", (uint16_t)"
                    << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_MR: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << ";\n";
                break;
            }
            case PPC_INS_LI: {
                int rD = reg_idx(ppc.operands[0].reg);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)" << simm(ppc.operands[1]) << ";\n";
                break;
            }
            case PPC_INS_LIS: {
                int rD = reg_idx(ppc.operands[0].reg);
                auto it = img.data_relocs.find(insn.address);
                if (it != img.data_relocs.end() && it->second.type == DataReloc::HA) {
                    if (is_rodata_section(it->second.section)) {
                        // Read-only, so codegen resolves the whole
                        // lis+consumer pair to a compile-time literal
                        // value instead (see e.g. PPC_INS_LFS below) --
                        // not linked yet, so this "address" isn't real,
                        // and there's nothing to write it to at runtime
                        // anyway. rD is deliberately left untouched.
                        pending_hi[rD] = it->second;
                        out << "  /* address-of " << it->second.section << "+" << it->second.addend
                            << " -- resolved at compile time by the paired load/store */\n";
                    } else {
                        // Mutable global -- give rD the real synthetic
                        // address (assign_global_addrs), since unlike the
                        // read-only case there's an actual runtime value
                        // that needs a stable, real address other
                        // functions' accesses to the same symbol will
                        // agree on.
                        uint32_t addr = img.global_section_base.at(it->second.section) + (uint32_t)it->second.addend;
                        out << "  " << reg(rD) << " = " << addr << "u; /* &" << it->second.section << "+"
                            << it->second.addend << " */\n";
                    }
                } else {
                    out << "  " << reg(rD) << " = " << uimm(ppc.operands[1]) << "u << 16;\n";
                }
                break;
            }
            case PPC_INS_ORI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " | " << uimm(ppc.operands[2]) << "u;\n";
                break;
            }
            case PPC_INS_ADDI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                if (is_mutable_lo_reloc(img, insn.address)) {
                    // lis+addi computing a global's address for later
                    // (usually indexed) use -- rA already holds the
                    // complete synthetic address, so the immediate here is
                    // a relocation placeholder, not real data.
                    out << "  " << reg(rD) << " = " << base_expr(rA) << ";\n";
                } else {
                    out << "  " << reg(rD) << " = " << base_expr(rA) << " + (uint32_t)(int32_t)"
                        << simm(ppc.operands[2]) << ";\n";
                }
                break;
            }
            case PPC_INS_ADD: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_AND:
            case PPC_INS_OR:
            case PPC_INS_XOR: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                char op = '&';
                switch (insn.id) {
                    case PPC_INS_AND: op = '&'; break;
                    case PPC_INS_OR: op = '|'; break;
                    case PPC_INS_XOR: op = '^'; break;
                    default: break;
                }
                out << "  " << reg(rD) << " = " << reg(rA) << " " << op << " " << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_ANDC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " & ~" << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_EQV: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ~(" << reg(rA) << " ^ " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_CNTLZW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_cntlzw(" << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_SUBFIC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_subfic(ctx, " << reg(rA) << ", " << simm(ppc.operands[2])
                    << ");\n";
                break;
            }
            case PPC_INS_NOR: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ~(" << reg(rA) << " | " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_NEG: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = (uint32_t)(-(int32_t)" << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_SLWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t sh = simm(ppc.operands[2]);
                out << "  " << reg(rD) << " = " << reg(rA) << " << " << sh << ";\n";
                break;
            }
            case PPC_INS_SRWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t sh = simm(ppc.operands[2]);
                out << "  " << reg(rD) << " = " << reg(rA) << " >> " << sh << ";\n";
                break;
            }
            case PPC_INS_SRAWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t sh = simm(ppc.operands[2]);
                out << "  " << reg(rD) << " = (uint32_t)((int32_t)" << reg(rA) << " >> " << sh << ");\n";
                break;
            }
            case PPC_INS_EXTSB: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int8_t)" << reg(rA) << ";\n";
                break;
            }
            case PPC_INS_RLWINM: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t sh = simm(ppc.operands[2]);
                int32_t mb = simm(ppc.operands[3]);
                int32_t me = simm(ppc.operands[4]);
                uint32_t mask = ppc_mask(mb, me);
                std::string rotated = sh == 0 ? reg(rA) : ("ppc_rotl32(" + reg(rA) + ", " + std::to_string(sh) + ")");
                out << "  " << reg(rD) << " = " << rotated << " & " << mask << "u;\n";
                break;
            }
            case PPC_INS_RLWIMI: {
                // Unlike rlwinm, rD is also a *source* here: bits inside the
                // mask come from the rotated rA, bits outside it are left
                // as whatever rD already held (an insert, not an
                // overwrite).
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t sh = simm(ppc.operands[2]);
                int32_t mb = simm(ppc.operands[3]);
                int32_t me = simm(ppc.operands[4]);
                uint32_t mask = ppc_mask(mb, me);
                std::string rotated = sh == 0 ? reg(rA) : ("ppc_rotl32(" + reg(rA) + ", " + std::to_string(sh) + ")");
                out << "  " << reg(rD) << " = (" << rotated << " & " << mask << "u) | (" << reg(rD) << " & "
                    << (~mask) << "u);\n";
                break;
            }
            case PPC_INS_CLRLWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t n = simm(ppc.operands[2]);
                uint32_t mask = ppc_mask(n, 31);
                out << "  " << reg(rD) << " = " << reg(rA) << " & " << mask << "u;\n";
                break;
            }
            case PPC_INS_ROTLWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t n = simm(ppc.operands[2]);
                out << "  " << reg(rD) << " = ppc_rotl32(" << reg(rA) << ", " << n << ");\n";
                break;
            }
            case PPC_INS_EXTSH: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)" << reg(rA) << ";\n";
                break;
            }
            case PPC_INS_SUBF: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rB) << " - " << reg(rA) << ";\n";
                break;
            }
            case PPC_INS_MULLI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " * (uint32_t)(int32_t)" << simm(ppc.operands[2])
                    << ";\n";
                break;
            }
            case PPC_INS_MULHW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_mulhw((int32_t)" << reg(rA) << ", (int32_t)" << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_MULHWU: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_mulhwu(" << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_DIVW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = (uint32_t)((int32_t)" << reg(rA) << " / (int32_t)" << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_DIVWU: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " / " << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_MULLW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " * " << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_CMPWI: {
                int rA = reg_idx(ppc.operands[0].reg);
                out << "  ppc_cmpw(ctx, (int32_t)" << reg(rA) << ", " << simm(ppc.operands[1]) << ");\n";
                break;
            }
            case PPC_INS_CMPW: {
                int rA = reg_idx(ppc.operands[0].reg);
                int rB = reg_idx(ppc.operands[1].reg);
                out << "  ppc_cmpw(ctx, (int32_t)" << reg(rA) << ", (int32_t)" << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_CMPLW: {
                int rA = reg_idx(ppc.operands[0].reg);
                int rB = reg_idx(ppc.operands[1].reg);
                out << "  ppc_cmplw(ctx, " << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_CMPLWI: {
                int rA = reg_idx(ppc.operands[0].reg);
                out << "  ppc_cmplw(ctx, " << reg(rA) << ", " << uimm(ppc.operands[1]) << "u);\n";
                break;
            }
            case PPC_INS_LWZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STWX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_LBZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STBX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u8(ctx, " << base_expr(rA) << " + " << reg(rB) << ", (uint8_t)" << reg(rD)
                    << ");\n";
                break;
            }
            case PPC_INS_LHZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STHX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u16(ctx, " << base_expr(rA) << " + " << reg(rB) << ", (uint16_t)" << reg(rD)
                    << ");\n";
                break;
            }
            case PPC_INS_ADDC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_addc(ctx, " << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_ADDE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_adde(ctx, " << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_B: {
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                out << "  goto L_" << std::hex << target << std::dec << ";\n";
                break;
            }
            case PPC_INS_BEQ:
            case PPC_INS_BNE:
            case PPC_INS_BLT:
            case PPC_INS_BLE:
            case PPC_INS_BGT:
            case PPC_INS_BGE: {
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                std::string cond;
                switch (insn.id) {
                    case PPC_INS_BEQ: cond = "ctx->cr0_eq"; break;
                    case PPC_INS_BNE: cond = "!ctx->cr0_eq"; break;
                    case PPC_INS_BLT: cond = "ctx->cr0_lt"; break;
                    case PPC_INS_BLE: cond = "(ctx->cr0_lt || ctx->cr0_eq)"; break;
                    case PPC_INS_BGT: cond = "ctx->cr0_gt"; break;
                    case PPC_INS_BGE: cond = "(ctx->cr0_gt || ctx->cr0_eq)"; break;
                    default: break;
                }
                out << "  if (" << cond << ") goto L_" << std::hex << target << std::dec << ";\n";
                break;
            }
            case PPC_INS_MFLR: {
                int rD = reg_idx(ppc.operands[0].reg);
                out << "  " << reg(rD) << " = ctx->lr;\n";
                break;
            }
            case PPC_INS_MTLR: {
                int rS = reg_idx(ppc.operands[0].reg);
                out << "  ctx->lr = " << reg(rS) << ";\n";
                break;
            }
            case PPC_INS_BLR: {
                out << "  return;\n";
                break;
            }
            case PPC_INS_BL: {
                auto it = img.call_relocs.find(insn.address);
                if (it != img.call_relocs.end()) {
                    out << "  ppc_" << it->second << "(ctx);\n";
                    break;
                }
                // No relocation (already-linked binary): the raw immediate
                // is a real target address.
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                auto it2 = addr_to_name.find(target);
                if (it2 != addr_to_name.end()) {
                    out << "  ppc_" << it2->second << "(ctx);\n";
                    break;
                }
                out << "#error \"unresolved call at 0x" << std::hex << insn.address << " to 0x" << target << std::dec
                    << " in function " << func.name << "\"\n";
                unhandled.push_back(insn.mnemonic);
                break;
            }
            case PPC_INS_LFS: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                auto rit = img.data_relocs.find(insn.address);
                auto pit = pending_hi.find(m.base);
                std::vector<uint8_t> bytes;
                if (rit != img.data_relocs.end() && rit->second.type == DataReloc::LO && pit != pending_hi.end() &&
                    read_data_const(img, pit->second, 4, bytes)) {
                    out << "  " << freg(fD) << " = " << hexfloat((double)be_bytes_to_float(bytes)) << ";\n";
                } else {
                    out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << base_expr(m.base) << " + (int32_t)"
                        << m.disp << ");\n";
                }
                break;
            }
            case PPC_INS_LFD: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << freg(fD) << " = ppc_load_f64(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                break;
            }
            case PPC_INS_STFD: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_f64(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << freg(fD)
                    << ");\n";
                break;
            }
            case PPC_INS_FCTIWZ: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = ppc_fctiwz(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_XORIS: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " ^ (" << uimm(ppc.operands[2]) << "u << 16);\n";
                break;
            }
            case PPC_INS_FADD:
            case PPC_INS_FSUB:
            case PPC_INS_FMUL:
            case PPC_INS_FDIV: {
                // Double-precision forms: no ppc_frsp rounding, unlike the
                // single-precision fadds/fsubs/fmuls/fdivs above.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                char op = '+';
                switch (insn.id) {
                    case PPC_INS_FADD: op = '+'; break;
                    case PPC_INS_FSUB: op = '-'; break;
                    case PPC_INS_FMUL: op = '*'; break;
                    case PPC_INS_FDIV: op = '/'; break;
                    default: break;
                }
                out << "  " << freg(fD) << " = " << freg(fA) << " " << op << " " << freg(fB) << ";\n";
                break;
            }
            case PPC_INS_FMADD:
            case PPC_INS_FMSUB: {
                // fD = (fA * fC) op fB, double precision (no ppc_frsp).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                char op = insn.id == PPC_INS_FMADD ? '+' : '-';
                out << "  " << freg(fD) << " = " << freg(fA) << " * " << freg(fC) << " " << op << " " << freg(fB)
                    << ";\n";
                break;
            }
            case PPC_INS_STFS: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_f32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << freg(fD)
                    << ");\n";
                break;
            }
            case PPC_INS_FCMPU: {
                // operands[0] is the crf field (always cr0 in this model --
                // see the struct-level fidelity note in ppc_runtime.h).
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                out << "  ppc_fcmpu(ctx, " << freg(fA) << ", " << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_FMR: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = " << freg(fB) << ";\n";
                break;
            }
            case PPC_INS_FNEG: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = -" << freg(fB) << ";\n";
                break;
            }
            case PPC_INS_FADDS:
            case PPC_INS_FSUBS:
            case PPC_INS_FMULS:
            case PPC_INS_FDIVS: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                char op = '+';
                switch (insn.id) {
                    case PPC_INS_FADDS: op = '+'; break;
                    case PPC_INS_FSUBS: op = '-'; break;
                    case PPC_INS_FMULS: op = '*'; break;
                    case PPC_INS_FDIVS: op = '/'; break;
                    default: break;
                }
                out << "  " << freg(fD) << " = ppc_frsp(" << freg(fA) << " " << op << " " << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_FMADDS: {
                // fD = (fA * fC) + fB. Capstone's operand order matches the
                // mnemonic's own textual operand order (D, A, C, B).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  " << freg(fD) << " = ppc_frsp(" << freg(fA) << " * " << freg(fC) << " + " << freg(fB)
                    << ");\n";
                break;
            }
            default: {
                out << "#error \"unhandled PPC instruction '" << insn.mnemonic << "' at 0x" << std::hex
                    << insn.address << std::dec << " in function " << func.name << "\"\n";
                unhandled.push_back(insn.mnemonic);
                break;
            }
        }
    }

    out << "}\n";
    return unhandled;
}

}  // namespace recomp
