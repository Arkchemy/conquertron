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

// Real, confirmed bug fixed here (found running against the real game
// binary's own real lwarx/stwcx. atomic singleton-init sequences, not
// hypothetical): per the PPC ISA, RA==0 in a base-register position
// (either a d(RA) displacement form or an X-form rD,RA,RB indexed form)
// means literal 0, not "read GPR r0" -- base_expr() below already
// handles this, but only for a base register index of plain `0`.
// Capstone represents this "RA encoded as 0" case two different ways
// depending on the operand kind: a memory operand's own `.mem.base`
// field reports it as literal capstone value 0 directly (so
// reg_idx(0) = 0 - PPC_REG_R0's own value correctly happens to look
// like -PPC_REG_R0, NOT what base_expr checks for) -- while a plain
// register operand (`.reg`, used by X-form instructions like
// lwarx/stwcx.) reports the same real "RA=0" case with that *same* raw
// capstone value 0 too. Either way, naively feeding that raw 0 through
// reg_idx() (which subtracts PPC_REG_R0, i.e. 87) produces a wildly
// out-of-range negative index (`ctx->r[-87]`, confirmed exactly this
// value in real generated output) instead of base_expr()'s expected
// "0 means literal zero" sentinel. This wrapper normalizes both real
// capstone conventions (raw 0, and the normal PPC_REG_R0 enum value)
// to reg_idx's own "0" sentinel before reg_idx's blind subtraction
// ever runs, so base_expr()'s existing check works as intended for
// every base-register source, not just the ones that happened not to
// hit this.
int base_reg_idx(unsigned int capstone_reg) {
    if (capstone_reg == 0 || capstone_reg == PPC_REG_R0) return 0;
    return reg_idx(capstone_reg);
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

// Real C expression for a real, individual CR bit register (one of
// PPC_REG_CR0LT..CR7LT / CR0GT..CR7GT / CR0EQ..CR7EQ / CR0UN..CR7UN --
// confirmed contiguous per bit-type in real Capstone's own ppc.h). CR0
// keeps using PpcContext's original, dedicated `cr0_lt`/`cr0_gt`/
// `cr0_eq` fields (untouched, zero risk to every already-proven cr0
// codegen path); CR1-CR7 map directly into the newer `cr_lt`/`cr_gt`/
// `cr_eq` arrays (see PpcContext's own comment for why). Returns an
// empty string for an untracked bit (any real CR*UN/"summary overflow"
// bit -- never set by this runtime, same real, narrow, pre-existing gap
// CR0's own SO bit already has).
std::string cr_bit_expr(unsigned int r) {
    if (r == PPC_REG_CR0LT) return "ctx->cr0_lt";
    if (r == PPC_REG_CR0GT) return "ctx->cr0_gt";
    if (r == PPC_REG_CR0EQ) return "ctx->cr0_eq";
    if (r >= PPC_REG_CR1LT && r <= PPC_REG_CR7LT) return "ctx->cr_lt[" + std::to_string(r - PPC_REG_CR0LT) + "]";
    if (r >= PPC_REG_CR1GT && r <= PPC_REG_CR7GT) return "ctx->cr_gt[" + std::to_string(r - PPC_REG_CR0GT) + "]";
    if (r >= PPC_REG_CR1EQ && r <= PPC_REG_CR7EQ) return "ctx->cr_eq[" + std::to_string(r - PPC_REG_CR0EQ) + "]";
    return "";
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

MemOp mem_operand(const cs_ppc_op &op) { return {base_reg_idx(op.mem.base), op.mem.disp}; }

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

// Resolves a call target (the instruction's own address, for relocation
// lookup, plus the raw target address) to a C call statement -- shared by
// every branch-like instruction that can turn into a call: bl, the
// conditional beql/bnel, and a tail-call b (see PPC_INS_B below) whose
// target lands outside the current function. Returns "" if the target
// can't be resolved through any of call_relocs/addr_to_name/
// import_trampolines.
std::string resolve_call_stmt(const ElfImage &img, const std::map<uint32_t, std::string> &addr_to_name,
                               uint32_t insn_addr, uint32_t target) {
    // A real linked .rpx/.rpl calls into other system libraries either via
    // a small linker-synthesized trampoline stub (import_trampolines
    // keyed by the *trampoline's* address, checked below by `target`) or,
    // as GHS-linked retail code actually does, via a direct `bl` relocated
    // straight at the import symbol (import_trampolines keyed by the
    // *call site's* own address instead -- see the R_PPC_REL24 handling
    // in elf_loader.cpp). Check the call-site keying first since it's a
    // relocation-based lookup, same precedence as call_relocs below.
    auto it_call_import = img.import_trampolines.find(insn_addr);
    if (it_call_import != img.import_trampolines.end()) {
        return "ppc_import_" + it_call_import->second.library + "_" + it_call_import->second.function + "(ctx);";
    }
    auto it = img.call_relocs.find(insn_addr);
    if (it != img.call_relocs.end()) {
        return "ppc_" + it->second + "(ctx);";
    }
    // No relocation (already-linked binary): the raw immediate is a real
    // target address.
    auto it2 = addr_to_name.find(target);
    if (it2 != addr_to_name.end()) {
        return "ppc_" + it2->second + "(ctx);";
    }
    // Trampoline-stub case: target is the trampoline's own resolved
    // address (see ImportTrampoline).
    auto it3 = img.import_trampolines.find(target);
    if (it3 != img.import_trampolines.end()) {
        return "ppc_import_" + it3->second.library + "_" + it3->second.function + "(ctx);";
    }
    return "";
}

// Emits a conditional branch as a local goto, or -- if the target lands
// outside this function's own address range -- as a conditional tail
// call (see PPC_INS_B's comment for why real compiled code does this).
// Shared by beq/bne/blt/ble/bgt/bge and bdnz/bdz; `cond` may itself have a
// side effect (bdnz/bdz's `--ctx->ctr`), which is fine since it only ever
// appears once in the emitted C either way.
void emit_conditional_branch(std::ostream &out, const std::string &cond, uint32_t target, const ElfImage &img,
                              const ElfFunction &func, const cs_insn &insn,
                              const std::map<uint32_t, std::string> &addr_to_name,
                              std::vector<std::string> &unhandled) {
    if (target >= func.addr && target < func.addr + func.size) {
        out << "  if (" << cond << ") goto L_" << std::hex << target << std::dec << ";\n";
        return;
    }
    std::string call_stmt = resolve_call_stmt(img, addr_to_name, insn.address, target);
    if (call_stmt.empty()) {
        out << "#error \"unresolved conditional tail call at 0x" << std::hex << insn.address << " to 0x" << target
            << std::dec << " in function " << func.name << "\"\n";
        unhandled.push_back(insn.mnemonic);
        return;
    }
    out << "  if (" << cond << ") { " << call_stmt << " return; }\n";
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
            // BEQ/BNE/BLT/BLE/BGT/BGE can explicitly name a non-default CR
            // field (e.g. real code emits `bne cr1, target` -- confirmed
            // against the real game binary, see PPC_INS_BEQ's own comment
            // below for the real ABI reason). When present, Capstone gives
            // that CR register as operands[0] and the real branch target
            // as operands[1]; with the implicit-cr0 form (the overwhelming
            // majority of real branches), the target is operands[0] and
            // there's nothing else. Using the wrong index here previously
            // recorded the CR register's own encoded value as a bogus
            // "target" address instead of the real one.
            int target_op = (insn.detail->ppc.op_count >= 2) ? 1 : 0;
            targets.insert((uint32_t)insn.detail->ppc.operands[target_op].imm);
        }
    }

    out << "void ppc_" << func.name << "(PpcContext *ctx) {\n";
    // Real, cheap "current PC" tracker (see ppc_runtime.h's own comment on
    // g_ppc_current_pc) -- lets a real hardware run's diagnostic log show
    // which real function is actually executing, found necessary after a
    // real 10-minute smoke test run produced zero FSOpenFile/unhandled-stub
    // log lines, leaving no way to tell whether the game thread was making
    // real progress through unrelated code or genuinely stuck in one place.
    // Real, cheap "who called this" tracker added 2026-08-20 alongside
    // g_ppc_current_pc above -- found necessary hunting a real hang that
    // froze on a tiny, universally-shared linker helper (a real register-
    // spill routine called from hundreds of unrelated real sites): the PC
    // alone only names *that* helper, not which of its many real callers
    // is actually the one that hung. ctx->lr at function entry is the
    // real return address the calling `bl` set, i.e. the exact real
    // address to resume the *caller* at -- naming the caller (and where
    // within it) precisely, not just "some caller of this helper".
    out << "  g_ppc_last_caller_lr = ctx->lr;\n";
    out << "  g_ppc_current_pc = 0x" << std::hex << func.addr << std::dec << "u; g_ppc_fn_call_count++;\n";
    out << "  for (int __w = 0; __w < ARKCHEMY_WATCH_SLOTS; __w++) { if (g_ppc_current_pc == g_ppc_watch[__w].pc) { "
           "g_ppc_watch[__w].r3 = ctx->r[3]; g_ppc_watch[__w].r4 = ctx->r[4]; g_ppc_watch[__w].r5 = ctx->r[5]; g_ppc_watch[__w].r6 = ctx->r[6]; "
           "g_ppc_watch[__w].hit_count++; g_ppc_watch[__w].last_hit_call_count = g_ppc_fn_call_count; } }\n";

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
                // See the LWZU/LBZU/STBU comment above (this instruction
                // is almost always a stack-frame `stwu r1, -N(r1)`, which
                // never carries a data relocation, but is_synthetic_
                // addr_lo_reloc still needs checking for correctness/
                // consistency with every other *U form).
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_u32(ctx, " << addr_expr << ", " << reg(rD) << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
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
            // Update-form (rA = EA) loads/stores need the *same*
            // is_synthetic_addr_lo_reloc fold as their plain LWZ/STW
            // siblings above, applied to BOTH the memory address and the
            // base-register writeback -- found real, 2026-08-22, via a
            // hardware bisection that traced a NULL igArkCore singleton
            // pointer back to exactly this gap: getClassMetaSafeInternal
            // fetches it with `lwzu r3, -0x2da4(r30)` after a `lis r30,
            // ha16(sym)` that -- per real ELF relocation data -- already
            // gives r30 the complete resolved address (same as the STW
            // path a few instructions earlier in the *writer*, which
            // folds correctly because it's plain STW). Unconditionally
            // re-applying the LO16 displacement here, as if it were a
            // real numeric offset rather than a relocation placeholder,
            // computes a second, never-written address and silently
            // reads 0/NULL forever. This was LWZU-only in the one
            // occurrence that surfaced it, but LBZU/STBU/LHZU/STHU/LHAU/
            // STWU all had the identical unconditional-disp bug --
            // fixed uniformly rather than patching just the one hit.
            case PPC_INS_LWZU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << addr_expr << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LBZU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << addr_expr << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_STBU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_u8(ctx, " << addr_expr << ", (uint8_t)" << reg(rD) << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
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
                    } else if (it->second.is_import) {
                        // &imported-function (e.g. building a function-
                        // pointer table/comparing against a known pointer
                        // that happens to be a real CafeOS import) -- a
                        // real, distinct case from both is_function (a
                        // local function's real address) and the plain
                        // synthetic-global-data case below (whose
                        // global_section_base map is never populated for
                        // .fimport_* sections in the first place -- see
                        // its own construction site). Confirmed as a real,
                        // not hypothetical, gap: recompiling a real,
                        // different devkitPPC-built homebrew binary
                        // (Aroma's root.rpx) hit this and crashed the
                        // whole tool with an unhandled std::out_of_range
                        // from .at() before this check existed. No real
                        // dispatch mechanism exists yet for calling an
                        // import indirectly through a computed pointer
                        // (unlike a direct `bl`, resolved via
                        // import_trampolines/call_relocs) -- real, honest
                        // gap, not guessed at. */
                        out << "#error \"unresolved &import (" << it->second.import_library << "."
                            << it->second.import_function << ") at 0x" << std::hex << insn.address << std::dec
                            << " in function " << func.name << " -- taking an imported function's address is not "
                            << "supported yet, see PPC_INS_LIS's codegen.cpp comment\"\n";
                        unhandled.push_back(insn.mnemonic);
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
                int rA = base_reg_idx(ppc.operands[1].reg);
                auto it = img.data_relocs.find(insn.address);
                if (it != img.data_relocs.end() && it->second.type == DataReloc::HA) {
                    if (it->second.is_function) {
                        out << "  " << reg(rD) << " = " << base_expr(rA) << " + " << it->second.func_addr
                            << "u; /* &" << it->second.func_name << " */\n";
                    } else if (it->second.is_import) {
                        // &imported-function via addis -- same real gap as
                        // PPC_INS_LIS's own is_import case above (see its
                        // comment for the full real explanation); addis is
                        // the same relocation shape with an extra `+ rA`,
                        // real code can use either form for the same
                        // real idiom.
                        out << "#error \"unresolved &import (" << it->second.import_library << "."
                            << it->second.import_function << ") at 0x" << std::hex << insn.address << std::dec
                            << " in function " << func.name << " -- taking an imported function's address is not "
                            << "supported yet, see PPC_INS_LIS's codegen.cpp comment\"\n";
                        unhandled.push_back(insn.mnemonic);
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
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0 --
                // same real "add." record-form gap, same fix.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // Real, severe bug found and fixed here (confirmed live
                // against the real game binary -- a real "or. r31, r4,
                // r4" left CR0 completely unset, so a `beq` right after
                // it branched on stale, unrelated condition-register
                // state instead of the real "is r31 zero" check it's
                // supposed to gate, sending a real function into a
                // genuine infinite loop instead of taking its intended
                // fast-path branch). ppc.update_cr0 is Capstone's real
                // Rc-bit flag -- and./or./xor. are real, common GHS-
                // compiler idioms for fusing a compute with a "compare
                // against zero", not a hypothetical case.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_ANDC: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " & ~" << reg(rB) << ";\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_EQV: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ~(" << reg(rA) << " ^ " << reg(rB) << ");\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_CNTLZW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_cntlzw(" << reg(rA) << ");\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_NEG: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = (uint32_t)(-(int32_t)" << reg(rA) << ");\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                out << "  " << reg(rD) << " = ppc_srawi(ctx, " << reg(rA) << ", " << sh << ");\n";
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `srawi.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_EXTSB: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int8_t)" << reg(rA) << ";\n";
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `extsb.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0
                // -- rlwinm. is a real, common idiom too (e.g. extracting
                // and testing a single bit field in one instruction).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `rlwimi.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_CLRLWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int32_t n = simm(ppc.operands[2]);
                uint32_t mask = ppc_mask(n, 31);
                out << "  " << reg(rD) << " = " << reg(rA) << " & " << mask << "u;\n";
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `clrlwi.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `extsh.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_SUBF: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rB) << " - " << reg(rA) << ";\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                /* PowerPC leaves rD UNDEFINED on a zero divisor (and on the
                 * signed INT_MIN / -1 overflow) but does NOT trap. Emitting a
                 * bare C `/` for those makes them undefined behaviour in C,
                 * which is strictly worse than the hardware: the compiler may
                 * assume the divisor is non-zero and optimise on that basis.
                 *
                 * Not hypothetical. igStringPool::searchForString computes
                 * `hash %% bucketCount` as divwu/mullw/subf, and on 2026-08-29
                 * it ran with bucketCount == 0, leaving the raw FNV basis
                 * 0x811C9DC5 as a bucket index and hanging the boot. The zero
                 * divisor is a real value this game really produces. */
                out << "  " << reg(rD) << " = (uint32_t)(((int32_t)" << reg(rB) << " == 0 || ((int32_t)"
                    << reg(rA) << " == (-2147483647-1) && (int32_t)" << reg(rB) << " == -1)) ? 0 :\n"
                    << "      (int32_t)" << reg(rA) << " / (int32_t)" << reg(rB) << ");\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_DIVWU: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                /* PowerPC leaves rD UNDEFINED on a zero divisor (and on the
                 * signed INT_MIN / -1 overflow) but does NOT trap. Emitting a
                 * bare C `/` for those makes them undefined behaviour in C,
                 * which is strictly worse than the hardware: the compiler may
                 * assume the divisor is non-zero and optimise on that basis.
                 *
                 * Not hypothetical. igStringPool::searchForString computes
                 * `hash %% bucketCount` as divwu/mullw/subf, and on 2026-08-29
                 * it ran with bucketCount == 0, leaving the raw FNV basis
                 * 0x811C9DC5 as a bucket index and hanging the boot. The zero
                 * divisor is a real value this game really produces. */
                out << "  " << reg(rD) << " = " << reg(rB) << " ? (" << reg(rA) << " / " << reg(rB) << ") : 0u;\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_MULLW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = " << reg(rA) << " * " << reg(rB) << ";\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_CMPWI:
            case PPC_INS_CMPW:
            case PPC_INS_CMPLW:
            case PPC_INS_CMPLWI: {
                // Real cmp*-form instructions can explicitly name a
                // non-default CR field, e.g. `cmpw cr1, r3, r4` --
                // confirmed real and live in real-world code (21 real
                // instances found testing against two different real,
                // legally-obtained open-source Wii U homebrew binaries).
                // Capstone represents this as a 3-operand form (CR
                // register first, then the real operands), vs. the far
                // more common implicit-cr0 2-operand form -- the same
                // real ambiguity PPC_INS_BEQ/BNE/etc.'s branch handling
                // already has to account for (see its own comment). Using
                // a fixed operand index regardless previously misread the
                // CR register as if it were the first real source
                // register -- not even a compile error, a genuinely
                // *silent* miscompile. Real CR1-CR7 fields are now
                // tracked (see PpcContext::cr_lt's own comment and
                // ppc_cmpw_cr/ppc_cmplw_cr below) -- routes to the real
                // per-field variant for an explicit non-cr0 field,
                // instead of erroring. */
                bool has_cr_operand = ppc.op_count == 3;
                int cr_field = has_cr_operand ? (int)(ppc.operands[0].reg - PPC_REG_CR0) : 0;
                int op_base = has_cr_operand ? 1 : 0;
                int rA = reg_idx(ppc.operands[op_base].reg);
                std::string cmpw_call = cr_field == 0 ? "ppc_cmpw(ctx, " : ("ppc_cmpw_cr(ctx, " + std::to_string(cr_field) + ", ");
                std::string cmplw_call = cr_field == 0 ? "ppc_cmplw(ctx, " : ("ppc_cmplw_cr(ctx, " + std::to_string(cr_field) + ", ");
                switch (insn.id) {
                    case PPC_INS_CMPWI:
                        out << "  " << cmpw_call << "(int32_t)" << reg(rA) << ", " << simm(ppc.operands[op_base + 1])
                            << ");\n";
                        break;
                    case PPC_INS_CMPW: {
                        int rB = reg_idx(ppc.operands[op_base + 1].reg);
                        out << "  " << cmpw_call << "(int32_t)" << reg(rA) << ", (int32_t)" << reg(rB) << ");\n";
                        break;
                    }
                    case PPC_INS_CMPLW: {
                        int rB = reg_idx(ppc.operands[op_base + 1].reg);
                        out << "  " << cmplw_call << reg(rA) << ", " << reg(rB) << ");\n";
                        break;
                    }
                    case PPC_INS_CMPLWI:
                        out << "  " << cmplw_call << reg(rA) << ", " << uimm(ppc.operands[op_base + 1]) << "u);\n";
                        break;
                    default:
                        break;
                }
                break;
            }
            case PPC_INS_LWZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STWX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u32(ctx, " << base_expr(rA) << " + " << reg(rB) << ", " << reg(rD) << ");\n";
                break;
            }
            case PPC_INS_LBZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u8(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STBX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  ppc_store_u8(ctx, " << base_expr(rA) << " + " << reg(rB) << ", (uint8_t)" << reg(rD)
                    << ");\n";
                break;
            }
            case PPC_INS_LHZX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_STHX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // Record form (Rc bit) -- see PPC_INS_EXTSB.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_ADDIC: {
                // Real recompiler bug, found 2026-08-24 via a real
                // standalone tool cross-checking the actual, retail RPX's
                // own .rodata against what this project's own runtime
                // was reading for a real pool object's vtable pointer:
                // GHS sometimes emits `lis`+`addic` (not `addi`) for an
                // HA/LO address-computation pair -- same shape as the
                // already-fixed LWZU-family bug (#2), just a different
                // sibling instruction never given the same is_synthetic_
                // addr_lo_reloc check PPC_INS_ADDI already has above.
                // Unconditionally adding the immediate as literal data
                // (the old behavior) silently computed a real, garbage-
                // but-plausible-looking address for any lis+addic pair
                // addressing a relocated symbol -- confirmed on this
                // exact vtable reference: the correct address was the
                // `lis` value alone (a real, populated igMemoryPool-
                // family vtable sat right there in the real .rodata),
                // and adding the addic's "immediate" on top landed 16860
                // bytes into unrelated data (which happened to alias
                // with three real igMetaField methods in the real
                // binary, purely by coincidence of what data lives at
                // that wrong offset).
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                if (is_synthetic_addr_lo_reloc(img, insn.address)) {
                    out << "  " << reg(rD) << " = " << reg(rA) << ";\n";
                    if (ppc.update_cr0) {
                        out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                    }
                } else {
                    out << "  " << reg(rD) << " = ppc_addic(ctx, " << reg(rA) << ", " << simm(ppc.operands[2]) << ");\n";
                    if (ppc.update_cr0) {
                        out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                    }
                }
                break;
            }
            case PPC_INS_ADDZE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_addze(ctx, " << reg(rA) << ");\n";
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `addze.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_SUBFZE: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                out << "  " << reg(rD) << " = ppc_subfze(ctx, " << reg(rA) << ");\n";
                break;
            }
            case PPC_INS_B: {
                // A plain `b` can be a real intra-function jump (the
                // common case -- goto a local label) or a tail call: real
                // compiled code reuses the current stack frame and jumps
                // directly to another whole function when there's no work
                // left to do after the call, relying on the callee's own
                // blr to return using the *original* caller's still-intact
                // LR. Confirmed as a real, not hypothetical, gap running
                // against the actual Spyro's Adventure binary -- gcc
                // rejected "goto"s to labels that were never defined
                // because the real target was a different function
                // entirely (GHS adjustor thunks and Bink audio/IO thread
                // routines both do this). Targets inside this function's
                // own address range are still a local goto; anything
                // outside is resolved as a call, same as bl, followed by
                // an immediate return (nothing else in this function runs
                // after a tail call).
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                if (target >= func.addr && target < func.addr + func.size) {
                    out << "  goto L_" << std::hex << target << std::dec << ";\n";
                    break;
                }
                std::string call_stmt = resolve_call_stmt(img, addr_to_name, insn.address, target);
                if (call_stmt.empty()) {
                    out << "#error \"unresolved tail call at 0x" << std::hex << insn.address << " to 0x" << target
                        << std::dec << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                out << "  " << call_stmt << "\n";
                out << "  return;\n";
                break;
            }
            case PPC_INS_BEQ:
            case PPC_INS_BNE:
            case PPC_INS_BLT:
            case PPC_INS_BLE:
            case PPC_INS_BGT:
            case PPC_INS_BGE: {
                // Real code can explicitly name a non-default CR field,
                // e.g. `bne cr1, target` -- confirmed against the actual
                // game binary, inside real GHS varargs-handling prologues
                // (the real PowerPC SVR4 ABI convention where a caller
                // sets CR bit 6, i.e. CR1's EQ bit, to tell a vararg
                // callee whether any floating-point register arguments
                // were passed, via a real `crclr`/`cror`/`crmove` on that
                // bit -- see those cases' own comments, now real-tracked
                // for CR1-CR7 too, not just CR0). Capstone represents
                // this as a 2-operand form (CR register, then target);
                // the far more common implicit-cr0 form is a single
                // operand (just the target). Real CR1-CR7 fields are now
                // tracked (see PpcContext::cr_lt's own comment) -- routes
                // to the real per-field state for an explicit non-cr0
                // field, instead of erroring. */
                bool has_cr_operand = ppc.op_count >= 2;
                uint32_t target = (uint32_t)ppc.operands[has_cr_operand ? 1 : 0].imm;
                int cr_field = has_cr_operand ? (int)(ppc.operands[0].reg - PPC_REG_CR0) : 0;
                std::string lt = cr_field == 0 ? "ctx->cr0_lt" : ("ctx->cr_lt[" + std::to_string(cr_field) + "]");
                std::string gt = cr_field == 0 ? "ctx->cr0_gt" : ("ctx->cr_gt[" + std::to_string(cr_field) + "]");
                std::string eq = cr_field == 0 ? "ctx->cr0_eq" : ("ctx->cr_eq[" + std::to_string(cr_field) + "]");
                std::string cond;
                switch (insn.id) {
                    case PPC_INS_BEQ: cond = eq; break;
                    case PPC_INS_BNE: cond = "!" + eq; break;
                    case PPC_INS_BLT: cond = lt; break;
                    case PPC_INS_BLE: cond = "(" + lt + " || " + eq + ")"; break;
                    case PPC_INS_BGT: cond = gt; break;
                    case PPC_INS_BGE: cond = "(" + gt + " || " + eq + ")"; break;
                    default: break;
                }
                emit_conditional_branch(out, cond, target, img, func, insn, addr_to_name, unhandled);
                break;
            }
            case PPC_INS_BDNZ: {
                // Decrements CTR (set up by an earlier mtctr, the loop trip
                // count) and branches while it's still nonzero -- what a
                // compiler emits for a `for`/`while` loop with a
                // runtime-but-not-compile-time-known iteration count instead
                // of a cmp+conditional-branch pair.
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                emit_conditional_branch(out, "--ctx->ctr != 0", target, img, func, insn, addr_to_name, unhandled);
                break;
            }
            case PPC_INS_BDZ: {
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                emit_conditional_branch(out, "--ctx->ctr == 0", target, img, func, insn, addr_to_name, unhandled);
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
                // for "if (some guard fails) return;" prologues. Real,
                // same explicit-non-cr0-field support as the plain
                // conditional branches above (e.g. `beqlr cr1`) --
                // previously always assumed cr0 unconditionally here with
                // no operand check at all, a real latent gap (not caught
                // by any #error, since nothing flagged it) fixed now,
                // same real reasoning/pattern as the branch case above. */
                bool has_cr_operand = ppc.op_count >= 1;
                int cr_field = has_cr_operand ? (int)(ppc.operands[0].reg - PPC_REG_CR0) : 0;
                std::string lt = cr_field == 0 ? "ctx->cr0_lt" : ("ctx->cr_lt[" + std::to_string(cr_field) + "]");
                std::string gt = cr_field == 0 ? "ctx->cr0_gt" : ("ctx->cr_gt[" + std::to_string(cr_field) + "]");
                std::string eq = cr_field == 0 ? "ctx->cr0_eq" : ("ctx->cr_eq[" + std::to_string(cr_field) + "]");
                std::string cond;
                switch (insn.id) {
                    case PPC_INS_BEQLR: cond = eq; break;
                    case PPC_INS_BNELR: cond = "!" + eq; break;
                    case PPC_INS_BLTLR: cond = lt; break;
                    case PPC_INS_BLELR: cond = "(" + lt + " || " + eq + ")"; break;
                    case PPC_INS_BGTLR: cond = gt; break;
                    case PPC_INS_BGELR: cond = "(" + gt + " || " + eq + ")"; break;
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
                // Real, all 8 CR fields tracked now (see PpcContext's own
                // comment) -- the field mask (crm) operand is an 8-bit
                // immediate, bit 0x80 selecting field 0 (CR0) down to bit
                // 0x01 selecting field 7 (CR7), matching real hardware's
                // own real bit-to-field mapping (see ppc_mfcr's own
                // comment). Emits one real update per real set mask bit.
                uint32_t mask = uimm(ppc.operands[0]);
                int rS = reg_idx(ppc.operands[1].reg);
                int field;
                bool any = false;
                for (field = 0; field < 8; field++) {
                    if ((mask & (0x80u >> field)) == 0) continue;
                    any = true;
                    if (field == 0) out << "  ppc_mtcrf_cr0(ctx, " << reg(rS) << ");\n";
                    else out << "  ppc_mtcrf_field(ctx, " << field << ", " << reg(rS) << ");\n";
                }
                if (!any) out << "  /* mtcrf with an empty field mask -- no-op */\n";
                break;
            }
            case PPC_INS_CRCLR: {
                // Clears a single CR bit. Real LT/GT/EQ bits across all
                // real CR0-CR7 fields are tracked (see cr_bit_expr's own
                // comment); any other real bit (every real CR*UN/"summary
                // overflow" bit, never set by this runtime at all) is
                // silently a no-op.
                //
                // Real bug fixed here originally: this used to read
                // operands[0].crx.reg, assuming capstone reports crclr's
                // bit operand as a PPC_OP_CRX operand (crx is a struct
                // with a leading `scale` field before `reg`). Real capstone
                // output for both crclr and cror is a plain PPC_OP_REG
                // operand instead -- .crx.reg was reading past the union's
                // actual populated bytes, which happened to silently
                // no-op every time rather than crash (the garbage never
                // matched a tracked enum value), masking that CR0-bit
                // cases were never actually being applied.
                std::string bit = cr_bit_expr(ppc.operands[0].reg);
                if (!bit.empty()) out << "  " << bit << " = 0;\n";
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
                // same way here). Real, any tracked bit across all real
                // CR0-CR7 fields (see cr_bit_expr's own comment) -- see
                // PPC_INS_CRCLR above for why this reads plain .reg, not
                // .crx.reg.
                std::string bt = cr_bit_expr(ppc.operands[0].reg);
                std::string ba = cr_bit_expr(ppc.operands[1].reg);
                std::string bb = insn.id == PPC_INS_CRMOVE && ppc.op_count == 2 ? ba : cr_bit_expr(ppc.operands[2].reg);
                if (!bt.empty() && !ba.empty() && !bb.empty()) {
                    out << "  " << bt << " = (" << ba << " || " << bb << ");\n";
                } else {
                    out << "  /* cror/crmove involving an untracked CR bit -- no-op */\n";
                }
                break;
            }
            case PPC_INS_MCRF: {
                // Copies one whole real CR field to another. Real, all 8
                // CR fields tracked now (see PpcContext's own comment).
                int dst_field = (int)(ppc.operands[0].reg - PPC_REG_CR0);
                int src_field = (int)(ppc.operands[1].reg - PPC_REG_CR0);
                std::string dst_lt = dst_field == 0 ? "ctx->cr0_lt" : ("ctx->cr_lt[" + std::to_string(dst_field) + "]");
                std::string dst_gt = dst_field == 0 ? "ctx->cr0_gt" : ("ctx->cr_gt[" + std::to_string(dst_field) + "]");
                std::string dst_eq = dst_field == 0 ? "ctx->cr0_eq" : ("ctx->cr_eq[" + std::to_string(dst_field) + "]");
                std::string src_lt = src_field == 0 ? "ctx->cr0_lt" : ("ctx->cr_lt[" + std::to_string(src_field) + "]");
                std::string src_gt = src_field == 0 ? "ctx->cr0_gt" : ("ctx->cr_gt[" + std::to_string(src_field) + "]");
                std::string src_eq = src_field == 0 ? "ctx->cr0_eq" : ("ctx->cr_eq[" + std::to_string(src_field) + "]");
                out << "  " << dst_lt << " = " << src_lt << "; " << dst_gt << " = " << src_gt << "; "
                    << dst_eq << " = " << src_eq << ";\n";
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
            case PPC_INS_MFCTR: {
                // Real, not hypothetical -- found running recomp against
                // real, different, legally-obtained open-source Wii U
                // homebrew binaries (not this project's own Skylanders
                // target, which never needed this). ctx->ctr is already
                // real, tracked state (see PPC_INS_MTCTR/BDNZ/BDZ/BCTRL
                // above), so reading it back is direct.
                int rD = reg_idx(ppc.operands[0].reg);
                out << "  " << reg(rD) << " = ctx->ctr;\n";
                break;
            }
            case PPC_INS_MTSPR:
            case PPC_INS_MFSPR: {
                // Real, confirmed against a direct real-Capstone probe (not
                // guessed): the SPR field is a plain immediate operand
                // (`.imm`, the real SPR number, e.g. 912), not a named
                // register -- Capstone has no PPC_REG_GQR* enum at all.
                // Only the real GQR bank (SPR 912-919, GQR0-GQR7 -- see
                // PpcContext::gqr's own comment) is tracked; any other real
                // SPR real-fails loudly rather than silently no-op'ing
                // (matching this file's established "don't guess" pattern
                // -- an untracked SPR write real code actually depends on
                // reading back would otherwise be a silent miscompile).
                bool is_mtspr = insn.id == PPC_INS_MTSPR;
                uint32_t spr = is_mtspr ? (uint32_t)ppc.operands[0].imm : (uint32_t)ppc.operands[1].imm;
                if (spr < 912u || spr > 919u) {
                    out << "#error \"" << (is_mtspr ? "mtspr" : "mfspr") << " on untracked SPR " << spr << " at 0x"
                        << std::hex << insn.address << std::dec << " in function " << func.name
                        << " -- only the real GQR bank (912-919) is tracked, see PPC_INS_MTSPR's codegen.cpp comment\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                uint32_t gqr_idx = spr - 912u;
                if (is_mtspr) {
                    int rS = reg_idx(ppc.operands[1].reg);
                    out << "  ctx->gqr[" << gqr_idx << "] = " << reg(rS) << ";\n";
                } else {
                    int rD = reg_idx(ppc.operands[0].reg);
                    out << "  " << reg(rD) << " = ctx->gqr[" << gqr_idx << "];\n";
                }
                break;
            }
            case PPC_INS_BCTRL: {
                // Indirect call through a function pointer (vtables,
                // callback tables, etc.) -- unlike `bl`, the target isn't
                // known until runtime, so it can't be resolved to a direct
                // C function call at codegen time. ppc_dispatch (emitted in
                // main.cpp, which has the full function address table)
                // looks the address up and calls the matching ppc_<name>.
                //
                // Real bug found and fixed 2026-08-20, chasing a real,
                // non-deterministic hang (a different static initializer
                // hung on two separate identical hardware runs -- the
                // classic signature of code branching on uninitialized/
                // stale data): real PPC bctrl always sets LR to the real
                // return address (the next instruction) as part of the
                // same instruction that branches -- this recompiler's own
                // `bl` handling never needed to model that explicitly
                // (the call becomes a real C function call, so the actual
                // return is handled by the host C call stack, not by
                // jumping to ctx->lr), but any real code that legitimately
                // *reads* LR after an indirect call for something other
                // than simple save-before/restore-after a nested call
                // (e.g. a real GHS/PPC EABI position-independent-code
                // idiom computing a base address from the current PC, or
                // a real C++ adjustor-thunk pattern -- this project's own
                // README already documents real adjustor thunks existing
                // in this exact binary) would silently read whatever
                // stale value an unrelated, earlier real `mtlr` happened
                // to leave there instead. ctx->lr was never being set at
                // all for this call form, so it was pure leftover garbage
                // from wherever execution had been before -- explaining
                // real, non-deterministic behavior that depends on
                // whatever happened to run earlier.
                out << "  ctx->lr = 0x" << std::hex << (insn.address + 4) << std::dec << "u;\n";
                out << "  ppc_dispatch(ctx, ctx->ctr);\n";
                break;
            }
            case PPC_INS_BL: {
                uint32_t target = (uint32_t)ppc.operands[0].imm;
                std::string call_stmt = resolve_call_stmt(img, addr_to_name, insn.address, target);
                if (call_stmt.empty()) {
                    out << "#error \"unresolved call at 0x" << std::hex << insn.address << " to 0x" << target
                        << std::dec << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                // Same real gap as PPC_INS_BCTRL's own comment -- real
                // `bl` always sets LR to the real return address too.
                // Not needed for the call itself (a real C function
                // call/return handles that correctly on its own), only
                // for real guest code that legitimately reads LR after a
                // call for something other than simple save/restore.
                out << "  ctx->lr = 0x" << std::hex << (insn.address + 4) << std::dec << "u;\n";
                out << "  " << call_stmt << "\n";
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
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // operands[0] is the real crfD field -- fcmpu's encoding
                // always includes it explicitly (unlike cmpw/branches,
                // which have a genuinely optional implicit-cr0 form), but
                // real compilers apparently only ever emit cr0 here in
                // practice (confirmed zero non-cr0 instances found
                // disassembling this project's own Skylanders target and
                // two other real, different, legally-obtained open-source
                // Wii U binaries). Real CR1-CR7 fields are now tracked
                // (see PpcContext::cr_lt's own comment and ppc_fcmpu_cr
                // above) -- routes to the real per-field variant for an
                // explicit non-cr0 field, instead of erroring, same
                // reasoning as cmpw/branches. */
                int cr_field = (int)(ppc.operands[0].reg - PPC_REG_CR0);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                if (cr_field == 0) {
                    out << "  ppc_fcmpu(ctx, " << freg(fA) << ", " << freg(fB) << ");\n";
                } else {
                    out << "  ppc_fcmpu_cr(ctx, " << cr_field << ", " << freg(fA) << ", " << freg(fB) << ");\n";
                }
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
            case PPC_INS_ISYNC:
            case PPC_INS_EIEIO: {
                // Cache-management/memory-ordering barriers -- meaningless
                // in this purely sequential single-threaded interpreter
                // model (no cache, no reordering to synchronize against),
                // so all three are genuine no-ops here. eieio specifically
                // (real, not hypothetical -- found running recomp against
                // real, different, legally-obtained open-source Wii U
                // homebrew binaries, not this project's own Skylanders
                // target) enforces real I/O ordering on actual hardware
                // between memory-mapped device accesses, a real hardware
                // concern this shim-based runtime doesn't model at all.
                break;
            }
            case PPC_INS_LWARX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_SRW: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = (" << reg(rB) << " & 0x20u) ? 0u : (" << reg(rA) << " >> (" << reg(rB)
                    << " & 0x1Fu));\n";
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // Record form (Rc bit): the `.` suffix means this also
                // compares the result against zero into CR0. Omitting it
                // left every `subfe.` in the game testing a *stale* flag from
                // some earlier instruction -- which is exactly how
                // igStringBuf's `extsb.` terminator check became an
                // infinite loop during boot (2026-08-28).
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << freg(fD) << " = (double)ppc_load_f32(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_STFSX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << reg(rD) << " = ppc_load_u16(ctx, " << addr_expr << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LHAX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  ppc_store_u16(ctx, " << addr_expr << ", (uint16_t)" << reg(rD) << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                break;
            }
            case PPC_INS_LWBRX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u32_brx(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_LHBRX: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << reg(rD) << " = ppc_load_u16_brx(ctx, " << base_expr(rA) << " + " << reg(rB)
                    << ");\n";
                break;
            }
            case PPC_INS_LHAU: {
                int rD = reg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                bool folded = is_synthetic_addr_lo_reloc(img, insn.address);
                std::string addr_expr = folded ? base_expr(m.base) : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                out << "  " << reg(rD) << " = (uint32_t)(int32_t)(int16_t)ppc_load_u16(ctx, " << addr_expr << ");\n";
                if (!folded) out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
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
                // Record form (Rc bit) -- see PPC_INS_EXTSB.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
                // Record form (Rc bit) -- see PPC_INS_EXTSB.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
                break;
            }
            case PPC_INS_STFDX: {
                int fD = freg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                int rA = base_reg_idx(ppc.operands[1].reg);
                int rB = reg_idx(ppc.operands[2].reg);
                out << "  " << freg(fD) << " = ppc_load_f64(ctx, " << base_expr(rA) << " + " << reg(rB) << ");\n";
                break;
            }
            case PPC_INS_BEQL:
            case PPC_INS_BNEL: {
                // Conditional call (branch-and-link) -- the bl-flavored
                // counterpart to beq/bne, resolved through the same
                // call_relocs/addr_to_name/import_trampolines paths bl
                // itself uses below, just wrapped in the condition. Real,
                // same explicit-non-cr0-field support as the plain
                // conditional branches above -- previously always read
                // operands[0] as the target unconditionally, a real
                // latent misread risk if an explicit CR operand were
                // ever present (same class of bug the plain-branch case
                // above already had and fixed), addressed here too for
                // consistency even though no real instance of this exact
                // combination (conditional *call*) has been found yet. */
                bool has_cr_operand = ppc.op_count >= 2;
                int cr_field = has_cr_operand ? (int)(ppc.operands[0].reg - PPC_REG_CR0) : 0;
                std::string eq = cr_field == 0 ? "ctx->cr0_eq" : ("ctx->cr_eq[" + std::to_string(cr_field) + "]");
                std::string cond = insn.id == PPC_INS_BEQL ? eq : ("!" + eq);
                uint32_t target = (uint32_t)ppc.operands[has_cr_operand ? 1 : 0].imm;
                std::string call_stmt = resolve_call_stmt(img, addr_to_name, insn.address, target);
                if (call_stmt.empty()) {
                    out << "#error \"unresolved call at 0x" << std::hex << insn.address << std::dec
                        << " in function " << func.name << "\"\n";
                    unhandled.push_back(insn.mnemonic);
                    break;
                }
                // Same real gap as PPC_INS_BL/PPC_INS_BCTRL's own
                // comments -- the "L" in beql/bnel means real hardware
                // sets LR here too, conditionally on the call actually
                // happening.
                out << "  if (" << cond << ") { ctx->lr = 0x" << std::hex << (insn.address + 4) << std::dec
                    << "u; " << call_stmt << " }\n";
                break;
            }
            case PPC_INS_TWU:
            case PPC_INS_TRAP: {
                out << "  ppc_trap();\n";
                break;
            }
            case PPC_INS_LSWI: {
                int rD = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
                uint32_t nb = (uint32_t)ppc.operands[2].imm;
                out << "  ppc_lswi(ctx, " << rD << "u, " << base_expr(rA) << ", " << nb << "u);\n";
                break;
            }
            case PPC_INS_STSWI: {
                int rS = reg_idx(ppc.operands[0].reg);
                int rA = base_reg_idx(ppc.operands[1].reg);
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
                // See PPC_INS_AND/OR/XOR's own comment on ppc.update_cr0.
                if (ppc.update_cr0) {
                    out << "  ppc_cmpw(ctx, (int32_t)" << reg(rD) << ", 0);\n";
                }
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
            case PPC_INS_ARKCHEMY_PS_ADD:
            case PPC_INS_ARKCHEMY_PS_SUB:
            case PPC_INS_ARKCHEMY_PS_DIV: {
                // Per-lane binary op: fD.ps0 = fA.ps0 op fB.ps0, fD.ps1 =
                // fA.ps1 op fB.ps1. Reads both lanes of both sources into
                // temporaries first since fD may alias fA/fB.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                char op = insn.id == PPC_INS_ARKCHEMY_PS_ADD ? '+' : insn.id == PPC_INS_ARKCHEMY_PS_SUB ? '-' : '/';
                out << "  { double _p0 = " << freg(fA) << " " << op << " " << freg(fB) << "; float _p1 = "
                    << ps1(fA) << " " << op << " " << ps1(fB) << "; " << freg(fD) << " = _p0; " << ps1(fD)
                    << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_MUL: {
                // ps_mul fD, fA, fC (no frB operand -- see emit_dac).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                out << "  { double _p0 = " << freg(fA) << " * " << freg(fC) << "; float _p1 = " << ps1(fA) << " * "
                    << ps1(fC) << "; " << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_MULS0:
            case PPC_INS_ARKCHEMY_PS_MULS1: {
                // ps_muls0/1: both lanes of fA multiplied by a single
                // broadcast lane of fC (ps0 for muls0, ps1 for muls1).
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                std::string c_bcast = insn.id == PPC_INS_ARKCHEMY_PS_MULS0 ? freg(fC) : ("(double)" + ps1(fC));
                out << "  { double _cb = " << c_bcast << "; double _p0 = " << freg(fA) << " * _cb; float _p1 = "
                    << ps1(fA) << " * (float)_cb; " << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_RES:
            case PPC_INS_ARKCHEMY_PS_RSQRTE: {
                // Per-lane reciprocal / reciprocal-sqrt estimate (see
                // ppc_frsqrte's comment: computed exactly rather than as a
                // low-precision hardware estimate).
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                bool rsqrt = insn.id == PPC_INS_ARKCHEMY_PS_RSQRTE;
                std::string p0 = rsqrt ? ("ppc_frsqrte(" + freg(fB) + ")") : ("1.0 / " + freg(fB));
                std::string p1 = rsqrt ? ("1.0f / sqrtf(" + ps1(fB) + ")") : ("1.0f / " + ps1(fB));
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_NEG:
            case PPC_INS_ARKCHEMY_PS_ABS:
            case PPC_INS_ARKCHEMY_PS_NABS: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                std::string p0, p1;
                if (insn.id == PPC_INS_ARKCHEMY_PS_NEG) {
                    p0 = "-" + freg(fB);
                    p1 = "-" + ps1(fB);
                } else if (insn.id == PPC_INS_ARKCHEMY_PS_ABS) {
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
            case PPC_INS_ARKCHEMY_PS_MR: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fB = freg_idx(ppc.operands[1].reg);
                out << "  { double _p0 = " << freg(fB) << "; float _p1 = " << ps1(fB) << "; " << freg(fD)
                    << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_SUM0:
            case PPC_INS_ARKCHEMY_PS_SUM1: {
                // ps_sum0 fD,fA,fC,fB: fD.ps0 = fA.ps0+fB.ps1; fD.ps1 = fC.ps1
                // ps_sum1 fD,fA,fC,fB: fD.ps0 = fC.ps0;        fD.ps1 = fA.ps0+fB.ps1
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                std::string sum = "(" + freg(fA) + " + (double)" + ps1(fB) + ")";
                std::string p0 = insn.id == PPC_INS_ARKCHEMY_PS_SUM0 ? sum : freg(fC);
                std::string p1 = insn.id == PPC_INS_ARKCHEMY_PS_SUM0 ? ps1(fC) : ("(float)" + sum);
                out << "  { double _p0 = " << p0 << "; float _p1 = " << p1 << "; " << freg(fD) << " = _p0; "
                    << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_MADDS0:
            case PPC_INS_ARKCHEMY_PS_MADDS1: {
                // Both lanes of fA multiplied by a broadcast lane of fC
                // (ps0 for madds0, ps1 for madds1), then fB added per-lane.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                std::string c_bcast = insn.id == PPC_INS_ARKCHEMY_PS_MADDS0 ? freg(fC) : ("(double)" + ps1(fC));
                out << "  { double _cb = " << c_bcast << "; double _p0 = " << freg(fA) << " * _cb + " << freg(fB)
                    << "; float _p1 = " << ps1(fA) << " * (float)_cb + " << ps1(fB) << "; " << freg(fD)
                    << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_SEL: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                out << "  { double _p0 = (" << freg(fA) << " >= 0.0) ? " << freg(fC) << " : " << freg(fB)
                    << "; float _p1 = (" << ps1(fA) << " >= 0.0f) ? " << ps1(fC) << " : " << ps1(fB) << "; "
                    << freg(fD) << " = _p0; " << ps1(fD) << " = _p1; }\n";
                break;
            }
            case PPC_INS_ARKCHEMY_PS_MSUB:
            case PPC_INS_ARKCHEMY_PS_MADD:
            case PPC_INS_ARKCHEMY_PS_NMSUB:
            case PPC_INS_ARKCHEMY_PS_NMADD: {
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fC = freg_idx(ppc.operands[2].reg);
                int fB = freg_idx(ppc.operands[3].reg);
                bool is_add = insn.id == PPC_INS_ARKCHEMY_PS_MADD || insn.id == PPC_INS_ARKCHEMY_PS_NMADD;
                bool negate = insn.id == PPC_INS_ARKCHEMY_PS_NMSUB || insn.id == PPC_INS_ARKCHEMY_PS_NMADD;
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
            case PPC_INS_ARKCHEMY_PS_CMPU0:
            case PPC_INS_ARKCHEMY_PS_CMPO0:
            case PPC_INS_ARKCHEMY_PS_CMPU1:
            case PPC_INS_ARKCHEMY_PS_CMPO1: {
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
                bool lane1 = insn.id == PPC_INS_ARKCHEMY_PS_CMPU1 || insn.id == PPC_INS_ARKCHEMY_PS_CMPO1;
                if (lane1) {
                    out << "  ctx->cr0_lt = " << ps1(fA) << " < " << ps1(fB) << "; ctx->cr0_gt = " << ps1(fA)
                        << " > " << ps1(fB) << "; ctx->cr0_eq = " << ps1(fA) << " == " << ps1(fB) << ";\n";
                } else {
                    out << "  ppc_fcmpu(ctx, " << freg(fA) << ", " << freg(fB) << ");\n";
                }
                break;
            }
            case PPC_INS_ARKCHEMY_PSQ_L:
            case PPC_INS_ARKCHEMY_PSQ_LU: {
                int fD = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                uint32_t w = (uint32_t)ppc.operands[2].imm;
                uint32_t gqr_i = (uint32_t)ppc.operands[3].imm;
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                if (gqr_i != 0) {
                    // Real GQR-selected quantized (non-float) load -- see
                    // ppc_psq_load_quantized's own comment for the real
                    // bit layout/dispatch. `ctx->gqr[gqr_i]` real-tracked
                    // via mtspr now (see PPC_INS_MTSPR above); real
                    // hardware's own real zero-reset default (FLOAT, no
                    // quantization) applies whenever nothing has written
                    // it, an honest real default, not a guess.
                    out << "  { double _ps0, _ps1 = 1.0; ppc_psq_load_quantized(ctx, " << addr_expr << ", &_ps0, &_ps1, ctx->gqr["
                        << gqr_i << "], " << (w != 0 ? 1 : 0) << "); " << freg(fD) << " = _ps0; ctx->ps1[" << fD
                        << "] = (float)_ps1; }\n";
                    if (insn.id == PPC_INS_ARKCHEMY_PSQ_LU) {
                        out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                    }
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
                if (insn.id == PPC_INS_ARKCHEMY_PSQ_LU) {
                    out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                }
                break;
            }
            case PPC_INS_ARKCHEMY_PSQ_ST:
            case PPC_INS_ARKCHEMY_PSQ_STU: {
                int fS = freg_idx(ppc.operands[0].reg);
                MemOp m = mem_operand(ppc.operands[1]);
                uint32_t w = (uint32_t)ppc.operands[2].imm;
                uint32_t gqr_i = (uint32_t)ppc.operands[3].imm;
                std::string addr_expr = is_synthetic_addr_lo_reloc(img, insn.address)
                                             ? base_expr(m.base)
                                             : (base_expr(m.base) + " + (int32_t)" + std::to_string(m.disp));
                if (gqr_i != 0) {
                    // Real GQR-selected quantized (non-float) store -- see
                    // ppc_psq_store_quantized's own comment for the real
                    // bit layout/dispatch, same real reasoning as the
                    // psq_l/psq_lu case above.
                    out << "  ppc_psq_store_quantized(ctx, " << addr_expr << ", " << freg(fS) << ", (double)ctx->ps1["
                        << fS << "], ctx->gqr[" << gqr_i << "], " << (w != 0 ? 1 : 0) << ");\n";
                    if (insn.id == PPC_INS_ARKCHEMY_PSQ_STU) {
                        out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                    }
                    break;
                }
                out << "  ppc_store_f32(ctx, " << addr_expr << ", " << freg(fS) << ");\n";
                if (w == 0) {
                    out << "  ppc_store_f32(ctx, " << addr_expr << " + 4, (double)ctx->ps1[" << fS << "]);\n";
                }
                if (insn.id == PPC_INS_ARKCHEMY_PSQ_STU) {
                    out << "  " << reg(m.base) << " = " << reg(m.base) << " + (int32_t)" << m.disp << ";\n";
                }
                break;
            }
            case PPC_INS_ARKCHEMY_PS_MERGE00:
            case PPC_INS_ARKCHEMY_PS_MERGE01:
            case PPC_INS_ARKCHEMY_PS_MERGE10:
            case PPC_INS_ARKCHEMY_PS_MERGE11: {
                // ps_mergeXY: frD.ps0 = frA's laneX, frD.ps1 = frB's laneY
                // (X/Y in {0,1} per the mnemonic's trailing digits). Reads
                // both source lanes into temporaries before writing frD's
                // two slots, since frD may alias frA and/or frB.
                int fD = freg_idx(ppc.operands[0].reg);
                int fA = freg_idx(ppc.operands[1].reg);
                int fB = freg_idx(ppc.operands[2].reg);
                bool a_lane1 = insn.id == PPC_INS_ARKCHEMY_PS_MERGE10 || insn.id == PPC_INS_ARKCHEMY_PS_MERGE11;
                bool b_lane1 = insn.id == PPC_INS_ARKCHEMY_PS_MERGE01 || insn.id == PPC_INS_ARKCHEMY_PS_MERGE11;
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
