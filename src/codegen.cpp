#include "codegen.h"

#include <cstdint>
#include <set>
#include <sstream>

namespace recomp {

namespace {

int reg_idx(unsigned int capstone_reg) {
    return (int)capstone_reg - PPC_REG_R0;
}

std::string reg(int i) {
    return "ctx->r[" + std::to_string(i) + "]";
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

}  // namespace

std::vector<std::string> generate_function_c(const ElfFunction &func, const DisasmResult &insns,
                                              const std::map<uint32_t, std::string> &call_relocs,
                                              const std::map<uint32_t, std::string> &addr_to_name, std::ostream &out) {
    std::vector<std::string> unhandled;

    // Pass 1: collect branch targets within this function so we know where
    // to emit goto labels. (`bl` is excluded -- it's a call, not an
    // intra-function jump, and is handled via call_relocs instead.)
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
                out << "  ppc_store_u32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp << ", " << reg(rD)
                    << ");\n";
                break;
            }
            case PPC_INS_LWZ: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << base_expr(m.base) << " + (int32_t)" << m.disp
                    << ");\n";
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
                out << "  " << reg(rD) << " = " << uimm(ppc.operands[1]) << "u << 16;\n";
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
                out << "  " << reg(rD) << " = " << base_expr(rA) << " + (uint32_t)(int32_t)" << simm(ppc.operands[2])
                    << ";\n";
                break;
            }
            case PPC_INS_ADD: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " + " << reg(rB) << ";\n";
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
                auto it = call_relocs.find(insn.address);
                if (it != call_relocs.end()) {
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
