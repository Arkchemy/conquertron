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

// The paired-single second lane -- see PpcContext::ps1's comment.
std::string ps1(int i) {
    return "ctx->ps1[" + std::to_string(i) + "]";
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

// True if `addr` carries an R_PPC_ADDR16_LO relocation -- i.e. the
// consuming instruction's base register already holds the complete
// synthetic (or, for functions, real) address, written there by the
// paired `lis`'s R_PPC_ADDR16_HA handling (see PPC_INS_LIS below), so the
// instruction's own displacement/immediate field is a relocation
// placeholder to be ignored rather than added on top of it.
//
// This applies uniformly to read-only (.rodata*) and mutable (.data/.bss)
// sections alike: an earlier version of this code constant-folded
// .rodata* accesses to a compile-time literal instead, which is only
// correct for the single narrow case of a `lis`+single-scalar-load pair
// (e.g. a float/double literal pool entry) -- it silently miscompiled
// anything that takes a rodata array's *address* for later runtime-
// indexed access (a compiler-generated switch-statement jump/lookup
// table, notably, but the same shape applies to any `static const`
// array indexed by a variable). Treating rodata addresses as real,
// though technically writable, entries in PpcContext::mem -- exactly
// like mutable globals, just never actually written to by generated
// code -- handles both cases uniformly and correctly.
bool is_synthetic_addr_lo_reloc(const ElfImage &img, uint32_t addr) {
    auto it = img.data_relocs.find(addr);
    return it != img.data_relocs.end() && it->second.type == DataReloc::LO;
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
            case PPC_INS_BDNZ:
            case PPC_INS_BDZ:
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
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_u32(ctx, " << addr_expr << ", " << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_LWZ: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
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
            case PPC_INS_LWZU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LBZU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_STBU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_u8(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", (uint8_t)"
                    << reg(rD) << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LBZUX: {
                // indexed update form: rA(base) += rB, then load from the
                // new address
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_STWUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  ppc_store_u32(ctx, " << reg(rA) << ", " << reg(rD) << ");\n";
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
                    if (it->second.is_function) {
                        // &function -- a real address in the same address
                        // space as ElfFunction::addr, dispatched later via
                        // ppc_dispatch (see main.cpp) when it reaches a
                        // bctrl.
                        out << "  " << reg(rD) << " = " << it->second.func_addr << "u; /* &" << it->second.func_name
                            << " */\n";
                    } else {
                        // Real synthetic address (assign_global_addrs) --
                        // applies uniformly to mutable globals and
                        // read-only data alike, see
                        // is_synthetic_addr_lo_reloc above.
                        uint32_t addr = img.global_section_base.at(it->second.section) + (uint32_t)it->second.addend;
                        out << "  " << reg(rD) << " = " << addr << "u; /* &" << it->second.section << "+"
                            << it->second.addend << " */\n";
                    }
                } else {
                    out << "  " << reg(rD) << " = " << uimm(ppc.operands[1]) << "u << 16;\n";
                }
                break;
            }
            case PPC_INS_ADDIS: {
                // Same as PPC_INS_LIS's relocation handling, except rA is a
                // real input to add to (lis is this instruction with an
                // implied rA=0, which capstone already reports as the
                // separate 2-operand PPC_INS_LIS case above).
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                auto it = img.data_relocs.find(insn.address);
                if (it != img.data_relocs.end() && it->second.type == DataReloc::HA) {
                    if (it->second.is_function) {
                        out << "  " << reg(rD) << " = " << base_expr(rA) << " + " << it->second.func_addr
                            << "u; /* &" << it->second.func_name << " */\n";
                    } else {
                        uint32_t addr = img.global_section_base.at(it->second.section) + (uint32_t)it->second.addend;
                        out << "  " << reg(rD) << " = " << base_expr(rA) << " + " << addr << "u; /* &"
                            << it->second.section << "+" << it->second.addend << " */\n";
                    }
                } else {
                    out << "  " << reg(rD) << " = " << base_expr(rA) << " + (" << uimm(ppc.operands[2])
                        << "u << 16);\n";
                }
                break;
            }
            case PPC_INS_ANDI: {
                // andi. (unlike `and`, there's no non-recording immediate
                // AND in the ISA) always sets CR0 from the result.
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " & " << uimm(ppc.operands[2]) << "u;\n";
                out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
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
                if (is_synthetic_addr_lo_reloc(img, insn.address)) {
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
            case PPC_INS_ADDIC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_addic(ctx, " << reg(rA) << ", " << simm(ppc.operands[2]) << ");\n";
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_ADDZE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_addze(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_SUBFZE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_subfze(ctx, " << reg(rA) << ");\n";
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
            case PPC_INS_BDNZ: {
                // Decrements CTR (set up by an earlier mtctr, the loop trip
                // count) and branches while it's still nonzero -- what a
                // compiler emits for a `for`/`while` loop with a
                // runtime-but-not-compile-time-known iteration count instead
                // of a cmp+conditional-branch pair.
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                out << "  if (--ctx->ctr != 0) goto L_" << std::hex << target << std::dec << ";\n";
                break;
            }
            case PPC_INS_BDZ: {
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                out << "  if (--ctx->ctr == 0) goto L_" << std::hex << target << std::dec << ";\n";
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
            case PPC_INS_BEQLR:
            case PPC_INS_BNELR:
            case PPC_INS_BLTLR:
            case PPC_INS_BLELR:
            case PPC_INS_BGTLR:
            case PPC_INS_BGELR: {
                // Conditional early return -- the blr-flavored counterpart
                // to the beq/bne/... conditional branches above, common
                // for "if (some guard fails) return;" prologues.
                std::string cond;
                switch (insn.id) {
                    case PPC_INS_BEQLR: cond = "ctx->cr0_eq"; break;
                    case PPC_INS_BNELR: cond = "!ctx->cr0_eq"; break;
                    case PPC_INS_BLTLR: cond = "ctx->cr0_lt"; break;
                    case PPC_INS_BLELR: cond = "(ctx->cr0_lt || ctx->cr0_eq)"; break;
                    case PPC_INS_BGTLR: cond = "ctx->cr0_gt"; break;
                    case PPC_INS_BGELR: cond = "(ctx->cr0_gt || ctx->cr0_eq)"; break;
                    default: break;
                }
                out << "  if (" << cond << ") return;\n";
                break;
            }
            case PPC_INS_MFCR: {
                int rD = reg_idx(ppc.operands[0].reg);
                out << "  " << reg(rD) << " = ppc_mfcr(ctx);\n";
                break;
            }
            case PPC_INS_MTCRF: {
                // Only field 0 (CR0) is tracked by this runtime -- see
                // ppc_mtcrf_cr0's comment. The field mask (crm) operand is
                // an immediate; only bother emitting anything when it
                // actually selects field 0 (mask bit 0x80).
                if ((uimm(ppc.operands[0]) & 0x80u) != 0) {
                    int rS = reg_idx(ppc.operands[1].reg);
                    out << "  ppc_mtcrf_cr0(ctx, " << reg(rS) << ");\n";
                } else {
                    out << "  /* mtcrf targeting a CR field other than CR0 -- not tracked, no-op */\n";
                }
                break;
            }
            case PPC_INS_CRCLR: {
                // Clears a single CR bit. Only CR0's LT/GT/EQ are tracked;
                // any other field (including CR0's SO, which this runtime
                // never sets anyway) is silently a no-op.
                //
                // Real bug fixed here: this originally read
                // operands[0].crx.reg, assuming capstone reports crclr's
                // bit operand as a PPC_OP_CRX operand (crx is a struct
                // with a leading `scale` field before `reg`). Real capstone
                // output for both crclr and cror is a plain PPC_OP_REG
                // operand instead -- .crx.reg was reading past the union's
                // actual populated bytes, which happened to silently
                // no-op every time rather than crash (the garbage never
                // matched a tracked enum value), masking that CR0-bit
                // cases were never actually being applied.
                unsigned int r = ppc.operands[0].reg;
                if (r == PPC_REG_CR0LT) out << "  ctx->cr0_lt = 0;\n";
                else if (r == PPC_REG_CR0GT) out << "  ctx->cr0_gt = 0;\n";
                else if (r == PPC_REG_CR0EQ) out << "  ctx->cr0_eq = 0;\n";
                else out << "  /* crclr on an untracked CR bit -- no-op */\n";
                break;
            }
            case PPC_INS_CROR:
            case PPC_INS_CRMOVE: {
                // ORs two CR bits into a third (crmove crD,crA is the
                // `cror crD,crA,crA` alias -- capstone reports some
                // 3-distinct-operand encodings under PPC_INS_CRMOVE too
                // rather than PPC_INS_CROR, confirmed against real
                // devkitPPC-compiled code, so both IDs are handled the
                // same way here). Only representable when all three
                // operands are tracked CR0 bits -- see PPC_INS_CRCLR
                // above for why this reads plain .reg, not .crx.reg.
                auto trackedFlag = [](unsigned int r) -> const char * {
                    if (r == PPC_REG_CR0LT) return "ctx->cr0_lt";
                    if (r == PPC_REG_CR0GT) return "ctx->cr0_gt";
                    if (r == PPC_REG_CR0EQ) return "ctx->cr0_eq";
                    return nullptr;
                };
                const char *bt = trackedFlag(ppc.operands[0].reg);
                const char *ba = trackedFlag(ppc.operands[1].reg);
                const char *bb = insn.id == PPC_INS_CRMOVE && ppc.op_count == 2 ? ba : trackedFlag(ppc.operands[2].reg);
                if (bt && ba && bb) {
                    out << "  " << bt << " = (" << ba << " || " << bb << ");\n";
                } else {
                    out << "  /* cror/crmove involving an untracked CR bit -- no-op */\n";
                }
                break;
            }
            case PPC_INS_MCRF: {
                // Copies one whole CR field to another. Always a no-op
                // here: copying *out of* CR0 into some other (untracked)
                // field can't do anything useful since nothing reads that
                // field back, and copying *into* CR0 from some other
                // (untracked, always-zero-in-this-model) field would
                // incorrectly clobber real CR0 state with nothing.
                out << "  /* mcrf -- CR fields other than CR0 aren't tracked, no-op */\n";
                break;
            }
            case PPC_INS_NOP: {
                break;
            }
            case PPC_INS_MTCTR: {
                int rS = reg_idx(ppc.operands[0].reg);
                out << "  ctx->ctr = " << reg(rS) << ";\n";
                break;
            }
            case PPC_INS_BCTRL: {
                // Indirect call through a function pointer (vtables,
                // callback tables, etc.) -- unlike `bl`, the target isn't
                // known until runtime, so it can't be resolved to a direct
                // C function call at codegen time. ppc_dispatch (emitted in
                // main.cpp, which has the full function address table)
                // looks the address up and calls the matching ppc_<name>.
                out << "  ppc_dispatch(ctx, ctx->ctr);\n";
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
                // A real linked .rpx/.rpl calls into other system
                // libraries through a small linker-synthesized trampoline
                // (see ImportTrampoline) rather than a real local function
                // -- resolve straight through it to a named external call
                // instead of treating the trampoline's own four
                // instructions as something to recompile.
                auto it3 = img.import_trampolines.find(target);
                if (it3 != img.import_trampolines.end()) {
                    out << "  ppc_import_" << it3->second.library << "_" << it3->second.function << "(ctx);\n";
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
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << addr_expr << ");\n";
                break;
            }
            case PPC_INS_LFD: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << freg(fD) << " = ppc_load_f64(ctx, " << addr_expr << ");\n";
                break;
            }
            case PPC_INS_STFD: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_f64(ctx, " << addr_expr << ", " << freg(fD) << ");\n";
                break;
            }
            case PPC_INS_FCTIWZ: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = ppc_fctiwz(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_FRSP: {
                // Explicit round-to-single, as its own instruction --
                // distinct from the implicit rounding fadds/fsubs/etc.
                // already apply via ppc_frsp internally.
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = ppc_frsp(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_STFIWX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_f64_low32(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << freg(fD)
                    << ");\n";
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
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_f32(ctx, " << addr_expr << ", " << freg(fD) << ");\n";
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
            case PPC_INS_FMSUBS: {
                // fD = (fA * fC) - fB, single-precision rounded.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  " << freg(fD) << " = ppc_frsp(" << freg(fA) << " * " << freg(fC) << " - " << freg(fB)
                    << ");\n";
                break;
            }
            case PPC_INS_LMW: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                for (int i = rD; i <= 31; i++) {
                    out << "  " << reg(i) << " = ppc_load_u32(ctx, " << base_expr(m.base) << " + (int32_t)"
                        << (m.disp + 4 * (i - rD)) << ");\n";
                }
                break;
            }
            case PPC_INS_STMW: {
                int rS = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                for (int i = rS; i <= 31; i++) {
                    out << "  ppc_store_u32(ctx, " << base_expr(m.base) << " + (int32_t)" << (m.disp + 4 * (i - rS))
                        << ", " << reg(i) << ");\n";
                }
                break;
            }
            case PPC_INS_DCBST:
            case PPC_INS_ISYNC: {
                // Cache-management/memory-ordering barriers -- meaningless
                // in this purely sequential single-threaded interpreter
                // model (no cache, no reordering to synchronize against),
                // so both are genuine no-ops here.
                break;
            }
            case PPC_INS_LWARX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STWCX: {
                // stwcx. (always dotted -- Rc=1 is baked into the real
                // encoding). Reservation-based atomicity isn't modeled at
                // all: no multi-core/multi-threaded execution exists in
                // this runtime, so the store always "succeeds" (CR0 EQ=1).
                // Correct for the single-threaded case this runtime
                // actually executes; not real multi-core contention.
                int rS = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << reg(rS) << ");\n";
                out << "  ctx->cr0_lt = 0; ctx->cr0_gt = 0; ctx->cr0_eq = 1;\n";
                break;
            }
            case PPC_INS_SLW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = (" << reg(rB) << " & 0x20u) ? 0u : (" << reg(rA) << " << (" << reg(rB)
                    << " & 0x1Fu));\n";
                break;
            }
            case PPC_INS_SRW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = (" << reg(rB) << " & 0x20u) ? 0u : (" << reg(rA) << " >> (" << reg(rB)
                    << " & 0x1Fu));\n";
                break;
            }
            case PPC_INS_SUBFC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_subfc(ctx, " << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_SUBFE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_subfe(ctx, " << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_XORI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " ^ " << uimm(ppc.operands[2]) << "u;\n";
                break;
            }
            case PPC_INS_ORIS: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " | (" << uimm(ppc.operands[2]) << "u << 16);\n";
                break;
            }
            case PPC_INS_FABS: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = ppc_fabs(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_LFSX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_STFSX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_f32(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << freg(fD) << ");\n";
                break;
            }
            case PPC_INS_LFSU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << base_expr(m.base) << " + (int32_t)"
                    << m.disp << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_STFSU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_f32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << freg(fD)
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LFSUX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_LHZU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LHAX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)ppc_load_u16(ctx, " << base_expr(rA)
                    << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_BCTR: {
                // Unconditional jump through CTR. Compilers use this for
                // both tail calls (jump-with-no-return) and computed
                // intra-function jump tables (dense switch statements).
                // Only the tail-call case is modeled: ppc_dispatch
                // resolves the target as if it were a whole other
                // recompiled function, then this function returns. An
                // intra-function jump-table use (branching to a label
                // within *this* function rather than another function's
                // entry point) isn't representable this way -- a known
                // gap, not yet seen needing it.
                out << "  ppc_dispatch(ctx, ctx->ctr);\n";
                out << "  return;\n";
                break;
            }
            case PPC_INS_MFTB: {
                int rD = reg_idx(ppc.operands[0].reg);
                out << "  " << reg(rD) << " = ppc_mftb(ctx);\n";
                break;
            }
            case PPC_INS_FSEL: {
                // fsel fD, fA, fC, fB: fD = (fA >= 0.0) ? fC : fB. Operand
                // order matches the mnemonic's own textual order (D, A, C,
                // B), same convention as fmadd/fmsub above.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  " << freg(fD) << " = (" << freg(fA) << " >= 0.0) ? " << freg(fC) << " : " << freg(fB)
                    << ";\n";
                break;
            }
            case PPC_INS_FNMSUBS: {
                // fnmsubs fD, fA, fC, fB: fD = -(fA*fC - fB) = fB - fA*fC,
                // single-precision rounded.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  " << freg(fD) << " = ppc_frsp(" << freg(fB) << " - " << freg(fA) << " * " << freg(fC)
                    << ");\n";
                break;
            }
            case PPC_INS_FRSQRTE: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = ppc_frsqrte(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_SRAW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_sraw(ctx, (int32_t)" << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_LWZUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_STFSUX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  ppc_store_f32(ctx, " << reg(rA) << ", " << freg(fD) << ");\n";
                break;
            }
            case PPC_INS_LHAUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)ppc_load_u16(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_CRSET: {
                // Sets a single CR bit to 1 -- the set counterpart to
                // crclr. Only CR0's LT/GT/EQ are tracked (see crclr).
                unsigned int r = ppc.operands[0].reg;
                if (r == PPC_REG_CR0LT) out << "  ctx->cr0_lt = 1;\n";
                else if (r == PPC_REG_CR0GT) out << "  ctx->cr0_gt = 1;\n";
                else if (r == PPC_REG_CR0EQ) out << "  ctx->cr0_eq = 1;\n";
                else out << "  /* crset on an untracked CR bit -- no-op */\n";
                break;
            }
            case PPC_INS_STHU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_u16(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", (uint16_t)"
                    << reg(rD) << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LWBRX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u32_brx(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_LHBRX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u16_brx(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_LHAU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)ppc_load_u16(ctx, " << base_expr(m.base)
                    << " + (int32_t)" << m.disp << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_STHUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  ppc_store_u16(ctx, " << reg(rA) << ", (uint16_t)" << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_STBUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  ppc_store_u8(ctx, " << reg(rA) << ", (uint8_t)" << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_ADDME: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_addme(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_ROTLW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_rotl32(" << reg(rA) << ", " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_RLWNM: {
                // rlwnm rA, rS, rB, MB, ME -- like rlwinm, but the rotate
                // amount comes from a register (masked to 5 bits by
                // ppc_rotl32 itself) instead of a compile-time immediate.
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                int32_t mb = simm(ppc.operands[3]);
                int32_t me = simm(ppc.operands[4]);
                uint32_t mask = ppc_mask(mb, me);
                out << "  " << reg(rD) << " = ppc_rotl32(" << reg(rA) << ", " << reg(rB) << ") & " << mask
                    << "u;\n";
                break;
            }
            case PPC_INS_STFDX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_f64(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << freg(fD) << ");\n";
                break;
            }
            case PPC_INS_STFDU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  ppc_store_f64(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << freg(fD)
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LWSYNC: {
                // Memory-ordering barrier -- see the DCBST/ISYNC no-op
                // rationale above; no reordering model exists to
                // synchronize against here.
                break;
            }
            case PPC_INS_LFDX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << freg(fD) << " = ppc_load_f64(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_BEQL:
            case PPC_INS_BNEL: {
                // Conditional call (branch-and-link) -- the bl-flavored
                // counterpart to beq/bne, resolved through the same
                // call_relocs/addr_to_name/import_trampolines paths bl
                // itself uses below, just wrapped in the condition.
                std::string cond = insn.id == PPC_INS_BEQL ? "ctx->cr0_eq" : "!ctx->cr0_eq";
                std::string call_stmt;
                auto it = img.call_relocs.find(insn.address);
                if (it != img.call_relocs.end()) {
                    call_stmt = "ppc_" + it->second + "(ctx);";
                } else {
                    uint32_t target = (uint32_t)ppc.operands[0].imm;
                    auto it2 = addr_to_name.find(target);
                    if (it2 != addr_to_name.end()) {
                        call_stmt = "ppc_" + it2->second + "(ctx);";
                    } else {
                        auto it3 = img.import_trampolines.find(target);
                        if (it3 != img.import_trampolines.end()) {
                            call_stmt =
                                "ppc_import_" + it3->second.library + "_" + it3->second.function + "(ctx);";
                        }
                    }
                }
                if (call_stmt.empty()) {
                    out << "#error \"unresolved call at 0x" << std::hex << insn.address << std::dec
                        << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                out << "  if (" << cond << ") { " << call_stmt << " }\n";
                break;
            }
            case PPC_INS_TWU:
            case PPC_INS_TRAP: {
                out << "  ppc_trap();\n";
                break;
            }
            case PPC_INS_LSWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                uint32_t nb = (uint32_t)ppc.operands[2].imm;
                out << "  ppc_lswi(ctx, " << rD << "u, " << base_expr(rA) << ", " << nb << "u);\n";
                break;
            }
            case PPC_INS_STSWI: {
                int rS = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                uint32_t nb = (uint32_t)ppc.operands[2].imm;
                out << "  ppc_stswi(ctx, " << rS << "u, " << base_expr(rA) << ", " << nb << "u);\n";
                break;
            }
            case PPC_INS_FNMSUB: {
                // fnmsub fD,fA,fC,fB: fD = -(fA*fC - fB) = fB - fA*fC,
                // double precision (no ppc_frsp) -- the non-single-
                // rounded sibling of fnmsubs above.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  " << freg(fD) << " = " << freg(fB) << " - " << freg(fA) << " * " << freg(fC) << ";\n";
                break;
            }
            case PPC_INS_FNABS: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  " << freg(fD) << " = -ppc_fabs(" << freg(fB) << ");\n";
                break;
            }
            case PPC_INS_ORC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " | ~" << reg(rB) << ";\n";
                break;
            }
            case PPC_INS_ANDIS: {
                // andis. (like andi., always dotted -- there's no
                // non-recording form) always sets CR0 from the result.
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " & (" << uimm(ppc.operands[2]) << "u << 16);\n";
                out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                break;
            }
            case PPC_INS_SYNC: {
                // Full memory barrier -- see the DCBST/ISYNC/LWSYNC no-op
                // rationale above.
                break;
            }
            case PPC_INS_LHZUX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rA) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_LFDU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << freg(fD) << " = ppc_load_f64(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
                out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_ADD:
            case PPC_INS_BRAMBLE_PS_SUB:
            case PPC_INS_BRAMBLE_PS_DIV: {
                // Per-lane binary op: fD.ps0 = fA.ps0 op fB.ps0, fD.ps1 =
                // fA.ps1 op fB.ps1. Reads both lanes of both sources into
                // temporaries first since fD may alias fA/fB.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                char op = insn.id == PPC_INS_BRAMBLE_PS_ADD ? '+' : insn.id == PPC_INS_BRAMBLE_PS_SUB ? '-' : '/';
                out << "  { double _p0 = " << freg(fA) << " " << op << " " << freg(fB) << "; float _p1 = "
                    << ps1(fA) << " " << op << " " << ps1(fB) << "; " << freg(fD) << " = _p0; " << ps1(fD)
                    << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_MUL: {
                // ps_mul fD, fA, fC (no frB operand -- see emit_dac).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                out << "  { double _p0 = " << freg(fA) << " * " << freg(fC) << "; float _p1 = " << ps1(fA) << " * "
                    << ps1(fC) << "; " << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_MULS0:
            case PPC_INS_BRAMBLE_PS_MULS1: {
                // ps_muls0/1: both lanes of fA multiplied by a single
                // broadcast lane of fC (ps0 for muls0, ps1 for muls1).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                std::string c_bcast = insn.id == PPC_INS_BRAMBLE_PS_MULS0 ? freg(fC) : ("(double)" + ps1(fC));
                out << "  { double _cb = " << c_bcast << "; double _p0 = " << freg(fA) << " * _cb; float _p1 = "
                    << ps1(fA) << " * (float)_cb; " << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_RES:
            case PPC_INS_BRAMBLE_PS_RSQRTE: {
                // Per-lane reciprocal / reciprocal-sqrt estimate (see
                // ppc_frsqrte's comment: computed exactly rather than as a
                // low-precision hardware estimate).
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                bool rsqrt = insn.id == PPC_INS_BRAMBLE_PS_RSQRTE;
                std::string p0 = rsqrt ? ("ppc_frsqrte(" + freg(fB) + ")") : ("1.0 / " + freg(fB));
                std::string p1 = rsqrt ? ("1.0f / sqrtf(" + ps1(fB) + ")") : ("1.0f / " + ps1(fB));
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_NEG:
            case PPC_INS_BRAMBLE_PS_ABS:
            case PPC_INS_BRAMBLE_PS_NABS: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                std::string p0, p1;
                if (insn.id == PPC_INS_BRAMBLE_PS_NEG) {
                    p0 = "-" + freg(fB);
                    p1 = "-" + ps1(fB);
                } else if (insn.id == PPC_INS_BRAMBLE_PS_ABS) {
                    p0 = "ppc_fabs(" + freg(fB) + ")";
                    p1 = "fabsf(" + ps1(fB) + ")";
                } else {
                    p0 = "-ppc_fabs(" + freg(fB) + ")";
                    p1 = "-fabsf(" + ps1(fB) + ")";
                }
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_MR: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  { double _p0 = " << freg(fB) << "; float _p1 = " << ps1(fB) << "; " << freg(fD)
                    << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_SUM0:
            case PPC_INS_BRAMBLE_PS_SUM1: {
                // ps_sum0 fD,fA,fC,fB: fD.ps0 = fA.ps0+fB.ps1; fD.ps1 = fC.ps1
                // ps_sum1 fD,fA,fC,fB: fD.ps0 = fC.ps0;        fD.ps1 = fA.ps0+fB.ps1
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                std::string sum = "(" + freg(fA) + " + (double)" + ps1(fB) + ")";
                std::string p0 = insn.id == PPC_INS_BRAMBLE_PS_SUM0 ? sum : freg(fC);
                std::string p1 = insn.id == PPC_INS_BRAMBLE_PS_SUM0 ? ps1(fC) : ("(float)" + sum);
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_MADDS0:
            case PPC_INS_BRAMBLE_PS_MADDS1: {
                // Both lanes of fA multiplied by a broadcast lane of fC
                // (ps0 for madds0, ps1 for madds1), then fB added per-lane.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                std::string c_bcast = insn.id == PPC_INS_BRAMBLE_PS_MADDS0 ? freg(fC) : ("(double)" + ps1(fC));
                out << "  { double _cb = " << c_bcast << "; double _p0 = " << freg(fA) << " * _cb + " << freg(fB)
                    << "; float _p1 = " << ps1(fA) << " * (float)_cb + " << ps1(fB) << "; " << freg(fD)
                    << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_SEL: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  { double _p0 = (" << freg(fA) << " >= 0.0) ? " << freg(fC) << " : " << freg(fB)
                    << "; float _p1 = (" << ps1(fA) << " >= 0.0f) ? " << ps1(fC) << " : " << ps1(fB) << "; "
                    << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_MSUB:
            case PPC_INS_BRAMBLE_PS_MADD:
            case PPC_INS_BRAMBLE_PS_NMSUB:
            case PPC_INS_BRAMBLE_PS_NMADD: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                bool is_add = insn.id == PPC_INS_BRAMBLE_PS_MADD || insn.id == PPC_INS_BRAMBLE_PS_NMADD;
                bool negate = insn.id == PPC_INS_BRAMBLE_PS_NMSUB || insn.id == PPC_INS_BRAMBLE_PS_NMADD;
                char op = is_add ? '+' : '-';
                std::string p0 = freg(fA) + " * " + freg(fC) + " " + op + " " + freg(fB);
                std::string p1 = ps1(fA) + " * " + ps1(fC) + " " + op + " " + ps1(fB);
                if (negate) {
                    p0 = "-(" + p0 + ")";
                    p1 = "-(" + p1 + ")";
                }
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_BRAMBLE_PS_CMPU0:
            case PPC_INS_BRAMBLE_PS_CMPO0:
            case PPC_INS_BRAMBLE_PS_CMPU1:
            case PPC_INS_BRAMBLE_PS_CMPO1: {
                // Only crfD==0 (CR0) is tracked -- matching fcmpu/cmpw's
                // existing CR0-only convention (see ppc_runtime.h). ps0 is
                // compared for cmp*0, ps1 for cmp*1; "ordered" (NaN-aware)
                // vs "unordered" variants aren't distinguished, same known
                // gap ppc_fcmpu's comment already documents.
                uint32_t crfD = (uint32_t)ppc.operands[0].imm;
                if (crfD != 0) {
                    out << "  /* ps_cmp targeting a CR field other than CR0 -- not tracked, no-op */\n";
                    break;
                }
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                bool lane1 = insn.id == PPC_INS_BRAMBLE_PS_CMPU1 || insn.id == PPC_INS_BRAMBLE_PS_CMPO1;
                if (lane1) {
                    out << "  ctx->cr0_lt = " << ps1(fA) << " < " << ps1(fB) << "; ctx->cr0_gt = " << ps1(fA)
                        << " > " << ps1(fB) << "; ctx->cr0_eq = " << ps1(fA) << " == " << ps1(fB) << ";\n";
                } else {
                    out << "  ppc_fcmpu(ctx, " << freg(fA) << ", " << freg(fB) << ");\n";
                }
                break;
            }
            case PPC_INS_BRAMBLE_PSQ_L:
            case PPC_INS_BRAMBLE_PSQ_LU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                uint32_t w = (uint32_t)ppc.operands[2].imm;
                uint32_t gqr_i = (uint32_t)ppc.operands[3].imm;
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                if (gqr_i != 0) {
                    // GQR-selected quantized (non-float) formats aren't
                    // modeled -- no GQR register state tracked by this
                    // runtime. Left as an honest gap rather than a silent
                    // wrong decode; not seen in practice yet.
                    out << "#error \"psq_l/psq_lu with non-zero quantization type (I=" << gqr_i << ") at 0x"
                        << std::hex << insn.address << std::dec << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << addr_expr << ");\n";
                if (w == 0) {
                    out << "  ctx->ps1[" << fD << "] = ppc_load_f32(ctx, " << addr_expr << " + 4);\n";
                } else {
                    // W=1 (single-element mode): ps1 is defined to be set
                    // to 1.0, not left stale -- see the ISA note.
                    out << "  ctx->ps1[" << fD << "] = 1.0f;\n";
                }
                if (insn.id == PPC_INS_BRAMBLE_PSQ_LU) {
                    out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                }
                break;
            }
            case PPC_INS_BRAMBLE_PSQ_ST:
            case PPC_INS_BRAMBLE_PSQ_STU: {
                int fS = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                uint32_t w = (uint32_t)ppc.operands[2].imm;
                uint32_t gqr_i = (uint32_t)ppc.operands[3].imm;
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                if (gqr_i != 0) {
                    out << "#error \"psq_st/psq_stu with non-zero quantization type (I=" << gqr_i << ") at 0x"
                        << std::hex << insn.address << std::dec << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                out << "  ppc_store_f32(ctx, " << addr_expr << ", " << freg(fS) << ");\n";
                if (w == 0) {
                    out << "  ppc_store_f32(ctx, " << addr_expr << " + 4, (double)ctx->ps1[" << fS << "]);\n";
                }
                if (insn.id == PPC_INS_BRAMBLE_PSQ_STU) {
                    out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                }
                break;
            }
            case PPC_INS_BRAMBLE_PS_MERGE00:
            case PPC_INS_BRAMBLE_PS_MERGE01:
            case PPC_INS_BRAMBLE_PS_MERGE10:
            case PPC_INS_BRAMBLE_PS_MERGE11: {
                // ps_mergeXY: frD.ps0 = frA's laneX, frD.ps1 = frB's laneY
                // (X/Y in {0,1} per the mnemonic's trailing digits). Reads
                // both source lanes into temporaries before writing frD's
                // two slots, since frD may alias frA and/or frB.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                bool a_lane1 = insn.id == PPC_INS_BRAMBLE_PS_MERGE10 || insn.id == PPC_INS_BRAMBLE_PS_MERGE11;
                bool b_lane1 = insn.id == PPC_INS_BRAMBLE_PS_MERGE01 || insn.id == PPC_INS_BRAMBLE_PS_MERGE11;
                std::string ps0_src = a_lane1 ? ("(double)ctx->ps1[" + std::to_string(fA) + "]") : freg(fA);
                std::string ps1_src = b_lane1 ? ("ctx->ps1[" + std::to_string(fB) + "]") : ("(float)" + freg(fB));
                out << "  { double _ps0 = " << ps0_src << "; float _ps1 = " << ps1_src << "; " << freg(fD)
                    << " = _ps0; ctx->ps1[" << fD << "] = _ps1; }\n";
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
