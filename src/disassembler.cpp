#include "disassembler.h"

#include <cstring>

namespace recomp {

namespace {

uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void set_reg_op(cs_ppc_op &op, unsigned int reg) {
    op.type = PPC_OP_REG;
    op.reg = (ppc_reg)reg;
}

void set_mem_op(cs_ppc_op &op, unsigned int base_reg, int32_t disp) {
    op.type = PPC_OP_MEM;
    op.mem.base = (ppc_reg)base_reg;
    op.mem.disp = disp;
}

void set_imm_op(cs_ppc_op &op, int64_t imm) {
    op.type = PPC_OP_IMM;
    op.imm = imm;
}

// Hand-decodes PowerPC 750CL ("Gekko"/Broadway/Espresso) "paired-single"
// instructions -- a real vendor SIMD extension used pervasively in actual
// Wii U game code (2x-float loads/stores/lane-shuffles) that Capstone's
// generic PPC decoder has zero support for. `insn` must already be a
// cs_malloc'd instruction (so detail is allocated); this fills it in by
// hand exactly as Capstone would for a recognized opcode, so the rest of
// the pipeline (codegen.cpp, branch-target scanning, etc.) can treat it
// like any other decoded instruction.
//
// Bit layout confirmed against the IBM Gekko RISC Microprocessor User's
// Manual (real hardware documentation, not guessed) and cross-checked
// against real instruction words extracted from Skylanders: Spyro's
// Adventure's igMatrix44f::transpose -- the decoded displacements came out
// as clean multiples of 8 with W=I=0 throughout, consistent with loading
// shuffled 8-byte row-pairs of a 4x4 matrix.
//
// Deliberately narrow: only the specific opcodes actually observed in real
// code are recognized here. Anything else falls through to the normal
// "unhandled" error path rather than being guessed at.
bool try_decode_paired_single(uint32_t word, uint64_t addr, cs_insn *insn) {
    uint32_t primary = word >> 26;
    uint32_t d_field = (word >> 21) & 0x1F;  // frD / frS
    uint32_t a_field = (word >> 16) & 0x1F;  // rA (psq_*) or frA (ps_merge*)
    uint32_t b_field = (word >> 11) & 0x1F;  // frB (ps_merge*) or rest of psq_* immediate
    uint32_t w_field = (word >> 15) & 0x1;
    uint32_t i_field = (word >> 12) & 0x7;
    int32_t disp = (int32_t)(word & 0xFFF);
    if (disp & 0x800) disp |= ~0xFFF;  // sign-extend the 12-bit field

    const char *mnemonic = nullptr;
    unsigned int id = 0;

    if (primary == 56 || primary == 57 || primary == 60 || primary == 61) {
        // psq_l / psq_lu / psq_st / psq_stu -- D-form (immediate
        // displacement), fields per the Gekko manual's PSQA/PSQS layout.
        switch (primary) {
            case 56: id = PPC_INS_BRAMBLE_PSQ_L; mnemonic = "psq_l"; break;
            case 57: id = PPC_INS_BRAMBLE_PSQ_LU; mnemonic = "psq_lu"; break;
            case 60: id = PPC_INS_BRAMBLE_PSQ_ST; mnemonic = "psq_st"; break;
            case 61: id = PPC_INS_BRAMBLE_PSQ_STU; mnemonic = "psq_stu"; break;
            default: return false;
        }
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "f%u, %d(r%u), %u, %u", d_field, disp, a_field, w_field,
                  i_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 4;
        set_reg_op(ppc.operands[0], PPC_REG_F0 + d_field);
        set_mem_op(ppc.operands[1], PPC_REG_R0 + a_field, disp);
        set_imm_op(ppc.operands[2], w_field);
        set_imm_op(ppc.operands[3], i_field);
        return true;
    }

    if (primary == 4) {
        uint32_t c_field = (word >> 6) & 0x1F;    // frC, for the 3-source (D,A,B,C) family
        uint32_t crfD = (word >> 23) & 0x7;       // for ps_cmp*
        uint32_t xo10 = (word >> 1) & 0x3FF;
        uint32_t xo5 = (word >> 1) & 0x1F;

        // 10-bit-XO families first: ps_merge* (D,A,B), ps_neg/mr/nabs/abs
        // (D,B; A architecturally 0), ps_cmp* (crfD,A,B). None of these
        // XO10 values can coincide with a real 5-bit-XO-family encoding
        // below (they all reduce to XO5 in {0, 8, 16}, none of which the
        // 5-bit family below uses), so checking these first is safe.
        switch (xo10) {
            case 528: mnemonic = "ps_merge00"; id = PPC_INS_BRAMBLE_PS_MERGE00; goto emit_dab;
            case 560: mnemonic = "ps_merge01"; id = PPC_INS_BRAMBLE_PS_MERGE01; goto emit_dab;
            case 592: mnemonic = "ps_merge10"; id = PPC_INS_BRAMBLE_PS_MERGE10; goto emit_dab;
            case 624: mnemonic = "ps_merge11"; id = PPC_INS_BRAMBLE_PS_MERGE11; goto emit_dab;
            case 40: mnemonic = "ps_neg"; id = PPC_INS_BRAMBLE_PS_NEG; goto emit_db;
            case 72: mnemonic = "ps_mr"; id = PPC_INS_BRAMBLE_PS_MR; goto emit_db;
            case 136: mnemonic = "ps_nabs"; id = PPC_INS_BRAMBLE_PS_NABS; goto emit_db;
            case 264: mnemonic = "ps_abs"; id = PPC_INS_BRAMBLE_PS_ABS; goto emit_db;
            case 0: mnemonic = "ps_cmpu0"; id = PPC_INS_BRAMBLE_PS_CMPU0; goto emit_cmp;
            case 32: mnemonic = "ps_cmpo0"; id = PPC_INS_BRAMBLE_PS_CMPO0; goto emit_cmp;
            case 64: mnemonic = "ps_cmpu1"; id = PPC_INS_BRAMBLE_PS_CMPU1; goto emit_cmp;
            case 96: mnemonic = "ps_cmpo1"; id = PPC_INS_BRAMBLE_PS_CMPO1; goto emit_cmp;
            default: break;
        }

        switch (xo5) {
            case 18: mnemonic = "ps_div"; id = PPC_INS_BRAMBLE_PS_DIV; goto emit_dab;
            case 20: mnemonic = "ps_sub"; id = PPC_INS_BRAMBLE_PS_SUB; goto emit_dab;
            case 21: mnemonic = "ps_add"; id = PPC_INS_BRAMBLE_PS_ADD; goto emit_dab;
            case 24: mnemonic = "ps_res"; id = PPC_INS_BRAMBLE_PS_RES; goto emit_db;
            case 26: mnemonic = "ps_rsqrte"; id = PPC_INS_BRAMBLE_PS_RSQRTE; goto emit_db;
            case 12: mnemonic = "ps_muls0"; id = PPC_INS_BRAMBLE_PS_MULS0; goto emit_dac;
            case 13: mnemonic = "ps_muls1"; id = PPC_INS_BRAMBLE_PS_MULS1; goto emit_dac;
            case 25: mnemonic = "ps_mul"; id = PPC_INS_BRAMBLE_PS_MUL; goto emit_dac;
            case 10: mnemonic = "ps_sum0"; id = PPC_INS_BRAMBLE_PS_SUM0; goto emit_dabc;
            case 11: mnemonic = "ps_sum1"; id = PPC_INS_BRAMBLE_PS_SUM1; goto emit_dabc;
            case 14: mnemonic = "ps_madds0"; id = PPC_INS_BRAMBLE_PS_MADDS0; goto emit_dabc;
            case 15: mnemonic = "ps_madds1"; id = PPC_INS_BRAMBLE_PS_MADDS1; goto emit_dabc;
            case 23: mnemonic = "ps_sel"; id = PPC_INS_BRAMBLE_PS_SEL; goto emit_dabc;
            case 28: mnemonic = "ps_msub"; id = PPC_INS_BRAMBLE_PS_MSUB; goto emit_dabc;
            case 29: mnemonic = "ps_madd"; id = PPC_INS_BRAMBLE_PS_MADD; goto emit_dabc;
            case 30: mnemonic = "ps_nmsub"; id = PPC_INS_BRAMBLE_PS_NMSUB; goto emit_dabc;
            case 31: mnemonic = "ps_nmadd"; id = PPC_INS_BRAMBLE_PS_NMADD; goto emit_dabc;
            default: return false;
        }

    emit_dab : {  // D, A, B (3-operand: frD, frA, frB)
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "f%u, f%u, f%u", d_field, a_field, b_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 3;
        set_reg_op(ppc.operands[0], PPC_REG_F0 + d_field);
        set_reg_op(ppc.operands[1], PPC_REG_F0 + a_field);
        set_reg_op(ppc.operands[2], PPC_REG_F0 + b_field);
        return true;
    }
    emit_dac : {  // D, A, C (3-operand: frD, frA, frC -- B field unused)
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "f%u, f%u, f%u", d_field, a_field, c_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 3;
        set_reg_op(ppc.operands[0], PPC_REG_F0 + d_field);
        set_reg_op(ppc.operands[1], PPC_REG_F0 + a_field);
        set_reg_op(ppc.operands[2], PPC_REG_F0 + c_field);
        return true;
    }
    emit_db : {  // D, B (2-operand: frD, frB -- A field unused)
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "f%u, f%u", d_field, b_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 2;
        set_reg_op(ppc.operands[0], PPC_REG_F0 + d_field);
        set_reg_op(ppc.operands[1], PPC_REG_F0 + b_field);
        return true;
    }
    emit_dabc : {  // D, A, B, C (4-operand fused multiply-add family)
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "f%u, f%u, f%u, f%u", d_field, a_field, c_field, b_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 4;
        set_reg_op(ppc.operands[0], PPC_REG_F0 + d_field);
        set_reg_op(ppc.operands[1], PPC_REG_F0 + a_field);
        set_reg_op(ppc.operands[2], PPC_REG_F0 + c_field);
        set_reg_op(ppc.operands[3], PPC_REG_F0 + b_field);
        return true;
    }
    emit_cmp : {  // ps_cmp* -- only crfD==0 (CR0) is tracked by this
        // runtime, matching fcmpu/cmpw's existing CR0-only convention.
        insn->id = id;
        strncpy(insn->mnemonic, mnemonic, sizeof(insn->mnemonic) - 1);
        insn->mnemonic[sizeof(insn->mnemonic) - 1] = '\0';
        snprintf(insn->op_str, sizeof(insn->op_str), "cr%u, f%u, f%u", crfD, a_field, b_field);
        cs_ppc &ppc = insn->detail->ppc;
        ppc.op_count = 3;
        set_imm_op(ppc.operands[0], crfD);
        set_reg_op(ppc.operands[1], PPC_REG_F0 + a_field);
        set_reg_op(ppc.operands[2], PPC_REG_F0 + b_field);
        return true;
    }
    }

    return false;
}

}  // namespace

bool disassemble_range(const uint8_t *code, size_t size, uint32_t addr, DisasmResult &out, std::string &error) {
    csh handle;
    cs_err err = cs_open(CS_ARCH_PPC, (cs_mode)(CS_MODE_32 + CS_MODE_BIG_ENDIAN), &handle);
    if (err != CS_ERR_OK) {
        error = std::string("cs_open failed: ") + cs_strerror(err);
        return false;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    const uint8_t *cursor = code;
    size_t remaining = size;
    uint64_t address = addr;

    cs_insn *insn = cs_malloc(handle);
    while (remaining > 0) {
        // Paired-single opcodes are checked *before* Capstone, not just as
        // a failure fallback: primary opcodes 4/60/61 collide with real
        // POWER8+ VMX/VSX encodings, which Capstone's generic PPC decoder
        // happily matches -- it doesn't fail on these words, it succeeds
        // with a confidently *wrong* decode (as "vmhaddshs", "xsaddsp",
        // "xxsel", etc.). The real Wii U CPU (Broadway/Espresso, a 750CL
        // derivative) has no VMX/VSX hardware at all, so on this target
        // every primary-4/60/61 word is unambiguously paired-single, never
        // genuinely VMX/VSX -- confirmed by hand-decoding real instruction
        // words from Skylanders: Spyro's Adventure that Capstone had
        // mislabeled this way. Only opcode 56/57 (psq_l/psq_lu) don't
        // collide with anything Capstone recognizes, which is why the
        // collision wasn't visible until testing against a function that
        // also used psq_st/ps_merge.
        if (remaining >= 4) {
            uint32_t word = rd_be32(cursor);
            if (try_decode_paired_single(word, address, insn)) {
                insn->address = address;
                insn->size = 4;
                memcpy(insn->bytes, cursor, 4);
                out.push_back(insn);
                insn = cs_malloc(handle);
                cursor += 4;
                remaining -= 4;
                address += 4;
                continue;
            }
        }

        if (cs_disasm_iter(handle, &cursor, &remaining, &address, insn)) {
            out.push_back(insn);
            insn = cs_malloc(handle);
            continue;
        }

        // Not paired-single and Capstone couldn't decode it either -- a
        // genuine unknown opcode, stop here.
        break;
    }
    cs_free(insn, 1);
    cs_close(&handle);

    if (out.size() == 0) {
        error = "failed to disassemble any instructions (capstone, and this isn't a recognized paired-single opcode)";
        return false;
    }
    return true;
}

}  // namespace recomp
