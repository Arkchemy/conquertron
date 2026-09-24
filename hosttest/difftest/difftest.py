#!/usr/bin/env python3
"""Differential fuzzing of the recompiler against a real PowerPC emulator.

Every recompiler bug found so far has been a silent one: wrong behaviour, no
crash, no warning, no unhandled instruction. Coverage cannot find those.
This does what ROADMAP.md calls "differential execution": random sequences of
PowerPC instructions are run twice,

  ground truth  assembled into a PowerPC Linux executable and run under
                qemu-ppc, which executes real PowerPC semantics
  recompiled    assembled into an object, put through recomp, compiled for
                the host and run natively

and every piece of architectural state the sequence can touch is compared:
r4..r11, the whole CR, XER[CA], f1..f6 and a 64-byte memory scratch area.

Each test function has the same frame. r3 points at a state block; the
function loads registers from it, sets CR and CA from it, runs the random
body, and stores everything back. r3 itself is never a destination, so the
body cannot lose the state block.

  state block (big-endian)
    0   r4..r11 in           32   CR in       36   CA in (0/1)
    40  f1..f6 in (doubles)
    88  r4..r11 out          120  CR out      124  CA out
    128 f1..f6 out           176  scratch memory, 64 bytes

A mismatch is shrunk -- instructions are dropped one at a time while the
mismatch persists -- and reported with the minimal sequence, the inputs and
the differing fields.

Deliberately NOT generated, because the architecture leaves the result
undefined and the two sides may legitimately differ: division by zero and
INT_MIN / -1 (divisors are forced odd and, for divw, positive), and
out-of-range shift amounts are generated on purpose (slw/srw/sraw define
them). NaN payloads are not compared, only NaN-ness: the host's default NaN
differs from PowerPC's and nothing in the game inspects the payload.

Usage: difftest.py [--seed N] [--programs N] [--length N] [--inputs N]
Needs: ZIG, QEMU_PPC (qemu-ppc-static), RECOMP (a built recomp).
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INCLUDE = os.path.normpath(os.path.join(HERE, "..", "..", "include"))

GPR = [4, 5, 6, 7, 8, 9, 10, 11]
FPR = [1, 2, 3, 4, 5, 6]
SCRATCH = 176
STATE_SIZE = SCRATCH + 64


def r(rng): return rng.choice(GPR)
def f(rng): return rng.choice(FPR)
def crf(rng): return rng.randrange(8)
def simm(rng): return rng.choice([rng.randrange(-32768, 32768), rng.randrange(-16, 16), 0, -1, 1])
def uimm(rng): return rng.choice([rng.randrange(0, 65536), rng.randrange(0, 16), 0xffff, 0x8000])
def sh(rng): return rng.randrange(32)


def dot(rng):
    return "." if rng.random() < 0.3 else ""


# Each generator returns a list of instruction lines. Some emit a guard
# before the instruction (forcing a divisor odd, or an index in range).
def g_arith3(rng):
    op = rng.choice(["add", "addc", "adde", "subf", "subfc", "subfe", "mullw",
                     "mulhw", "mulhwu", "and", "andc", "or", "orc", "xor",
                     "nor", "eqv", "slw", "srw", "sraw", "rlwnm_placeholder"])
    if op == "rlwnm_placeholder":
        return ["rlwnm%s %d, %d, %d, %d, %d" % (dot(rng), r(rng), r(rng), r(rng), sh(rng), sh(rng))]
    return ["%s%s %d, %d, %d" % (op, dot(rng), r(rng), r(rng), r(rng))]


def g_arith2(rng):
    op = rng.choice(["neg", "addze", "addme", "subfze", "cntlzw", "extsb", "extsh"])
    return ["%s%s %d, %d" % (op, dot(rng), r(rng), r(rng))]


def g_imm(rng):
    op = rng.choice(["addi", "addis", "addic", "addic.", "mulli", "subfic"])
    ra = r(rng)
    return ["%s %d, %d, %d" % (op, r(rng), ra, simm(rng))]


def g_logimm(rng):
    op = rng.choice(["ori", "oris", "xori", "xoris", "andi.", "andis."])
    return ["%s %d, %d, %d" % (op, r(rng), r(rng), uimm(rng))]


def g_rot(rng):
    op = rng.choice(["rlwinm", "rlwimi"])
    return ["%s%s %d, %d, %d, %d, %d" % (op, dot(rng), r(rng), r(rng), sh(rng), sh(rng), sh(rng))]


def g_srawi(rng):
    return ["srawi%s %d, %d, %d" % (dot(rng), r(rng), r(rng), sh(rng))]


def g_div(rng):
    op = rng.choice(["divw", "divwu"])
    rb = r(rng)
    guard = []
    if op == "divw":
        guard.append("rlwinm %d, %d, 0, 1, 31" % (rb, rb))   # positive
    guard.append("ori %d, %d, 1" % (rb, rb))                  # non-zero
    return guard + ["%s%s %d, %d, %d" % (op, dot(rng), r(rng), r(rng), rb)]


def g_cmp(rng):
    op = rng.choice(["cmpw", "cmplw", "cmpwi", "cmplwi"])
    if op == "cmpw" or op == "cmplw":
        return ["%s %d, %d, %d" % (op, crf(rng), r(rng), r(rng))]
    imm = simm(rng) if op == "cmpwi" else uimm(rng)
    return ["%s %d, %d, %d" % (op, crf(rng), r(rng), imm)]


# The recompiler models LT, GT and EQ in each CR field but not SO (summary
# overflow): nothing it supports can set XER[SO], so in real code SO is
# always 0 (see CRCLR's comment in codegen.cpp). The fuzzer tests that model,
# so it never puts a 1 into an SO bit: CR inputs are masked, mtcrf sources
# are masked, and CR-logical ops never target bit 3 of a field.
SO_MASK = 0xEEEEEEEE


def g_cr(rng):
    k = rng.random()
    if k < 0.3:
        return ["mcrf %d, %d" % (crf(rng), crf(rng))]
    if k < 0.6:
        rs, rt = r(rng), r(rng)
        return ["lis %d, 0xeeee" % rt, "ori %d, %d, 0xeeee" % (rt, rt),
                "and %d, %d, %d" % (rt, rs, rt), "mtcrf %d, %d" % (rng.randrange(256), rt)]
    if k < 0.8:
        rt, rm = r(rng), r(rng)
        while rm == rt:
            rm = r(rng)
        return ["mfcr %d" % rt, "lis %d, 0xeeee" % rm, "ori %d, %d, 0xeeee" % (rm, rm),
                "and %d, %d, %d" % (rt, rt, rm)]
    op = rng.choice(["cror", "crset", "crclr", "crmove"])
    b = lambda: rng.choice([x for x in range(32) if x % 4 != 3])
    if op == "cror":
        return ["cror %d, %d, %d" % (b(), b(), b())]
    if op == "crmove":
        return ["crmove %d, %d" % (b(), b())]
    return ["%s %d" % (op, b())]


def g_mem(rng):
    width = rng.choice([1, 2, 4])
    off = SCRATCH + rng.randrange(0, 64 // width) * width
    if rng.random() < 0.5:
        op = {1: "lbz", 2: rng.choice(["lhz", "lha"]), 4: "lwz"}[width]
        return ["%s %d, %d(3)" % (op, r(rng), off)]
    op = {1: "stb", 2: "sth", 4: "stw"}[width]
    return ["%s %d, %d(3)" % (op, r(rng), off)]


def g_memx(rng):
    # Indexed and byte-reversed forms. The index register is loaded with an
    # in-range offset first so the access stays inside the scratch area.
    ri = r(rng)
    width = rng.choice([2, 4])
    off = SCRATCH + rng.randrange(0, 64 // width) * width
    op = rng.choice({2: ["lhzx", "lhax", "sthx", "lhbrx"],
                     4: ["lwzx", "stwx", "lwbrx"]}[width])
    rt = r(rng)
    while rt == ri and op.startswith("l"):
        rt = r(rng)
    return ["li %d, %d" % (ri, off), "%s %d, 3, %d" % (op, rt, ri)]


# Single-precision arithmetic is given single-precision operands, as compiled
# code does (they come from lfs, frsp and other single ops). With a double
# operand the recompiler's add/sub/mul/div round twice -- to double, then to
# single -- and can break a tie the wrong way; see ppc_fmadds in
# ppc_runtime.h for why that is accepted, and for the fused ops, which are
# made exact for single operands.
def single_sources(regs):
    return ["frsp %d, %d" % (x, x) for x in sorted(set(regs))]


def g_fp3(rng):
    op = rng.choice(["fadd", "fsub", "fmul", "fdiv", "fadds", "fsubs", "fmuls", "fdivs"])
    fd, fa, fb = f(rng), f(rng), f(rng)
    pre = single_sources([fa, fb]) if op.endswith("s") else []
    return pre + ["%s %d, %d, %d" % (op, fd, fa, fb)]


def g_fp4(rng):
    op = rng.choice(["fmadd", "fmsub", "fnmsub", "fmadds", "fmsubs", "fnmsubs", "fsel"])
    fd, fa, fc, fb = f(rng), f(rng), f(rng), f(rng)
    pre = single_sources([fa, fc, fb]) if op.endswith("s") and op != "fsel" else []
    return pre + ["%s %d, %d, %d, %d" % (op, fd, fa, fc, fb)]


def g_fp2(rng):
    op = rng.choice(["fabs", "fnabs", "fneg", "fmr", "frsp"])
    return ["%s %d, %d" % (op, f(rng), f(rng))]


def g_fcmp(rng):
    return ["fcmpu %d, %d, %d" % (crf(rng), f(rng), f(rng))]


def g_fctiwz(rng):
    # The integer lands in the low word of an FPR; stfiwx is how game code
    # gets it out, so that is how it is observed here too.
    ri = r(rng)
    fd = f(rng)
    off = SCRATCH + rng.randrange(0, 16) * 4
    # The upper word fctiwz leaves in the FPR is architecturally undefined
    # (qemu sign-extends; the recompiler leaves 0), so the register is
    # overwritten afterwards and only the stored integer is compared.
    other = rng.choice([x for x in FPR if x != fd])
    return ["fctiwz %d, %d" % (fd, f(rng)), "li %d, %d" % (ri, off), "stfiwx %d, 3, %d" % (fd, ri),
            "fmr %d, %d" % (fd, other)]


def g_fmem(rng):
    k = rng.random()
    if k < 0.5:
        off = SCRATCH + rng.randrange(0, 8) * 8
        return [("lfd %d, %d(3)" if rng.random() < 0.5 else "stfd %d, %d(3)") % (f(rng), off)]
    off = SCRATCH + rng.randrange(0, 16) * 4
    return [("lfs %d, %d(3)" if rng.random() < 0.5 else "stfs %d, %d(3)") % (f(rng), off)]


_label = [0]


def label():
    _label[0] += 1
    return ".Ld%d" % _label[0]


def g_branch(rng):
    # A forward conditional branch over one or two instructions, on any CR
    # field. The branch and its label stay in one group, so shrinking keeps
    # or drops them together.
    cond = rng.choice(["blt", "bgt", "beq", "bne", "bge", "ble"])
    lab = label()
    inner = []
    for _ in range(rng.randrange(1, 3)):
        inner += rng.choice([g_arith3, g_imm, g_logimm, g_rot])(rng)
    return ["%s %d, %s" % (cond, crf(rng), lab)] + inner + [lab + ":"]


def g_loop(rng):
    # A counted loop: mtctr, a short body, bdnz. The counter register is
    # set from a small constant so the loop always ends.
    lab = label()
    rc = r(rng)
    body = []
    for _ in range(rng.randrange(1, 3)):
        body += rng.choice([g_arith3, g_imm, g_rot])(rng)
    return ["li %d, %d" % (rc, rng.randrange(1, 6)), "mtctr %d" % rc, lab + ":"] + body + ["bdnz " + lab]


def g_memu(rng):
    # Update forms (lwzu, stbu, ...) through r12, a copy of the state
    # pointer, so r3 survives. How far r12 moved is copied into a compared
    # register, so the update itself is checked as well as the access.
    width = rng.choice([1, 2, 4])
    op = rng.choice({1: ["lbzu", "stbu"], 2: ["lhzu", "lhau", "sthu"], 4: ["lwzu", "stwu"]}[width])
    start = SCRATCH + rng.randrange(0, 32 // width) * width
    disp = rng.randrange(0, 32 // width) * width
    rt, rd = r(rng), r(rng)
    return ["addi 12, 3, %d" % start, "%s %d, %d(12)" % (op, rt, disp), "subf %d, 3, 12" % rd]


GENERATORS = [
    (g_arith3, 12), (g_arith2, 5), (g_imm, 6), (g_logimm, 4), (g_rot, 6),
    (g_srawi, 2), (g_div, 2), (g_cmp, 4), (g_cr, 3), (g_mem, 4), (g_memx, 2),
    (g_fp3, 4), (g_fp4, 3), (g_fp2, 2), (g_fcmp, 2), (g_fctiwz, 1), (g_fmem, 2),
    (g_branch, 4), (g_loop, 2), (g_memu, 2),
]
_WEIGHTED = [g for g, w in GENERATORS for _ in range(w)]


def gen_body(rng, length):
    body = []
    while len(body) < length:
        body.append(rng.choice(_WEIGHTED)(rng))
    return body   # list of groups; a group is kept or dropped as a unit


PROLOGUE = (["lwz %d, %d(3)" % (g, 4 * i) for i, g in enumerate(GPR)] +
            ["lwz 0, 32(3)", "mtcrf 255, 0", "lwz 0, 36(3)", "addic 0, 0, -1"] +
            ["lfd %d, %d(3)" % (fr, 40 + 8 * i) for i, fr in enumerate(FPR)])
EPILOGUE = (["stw %d, %d(3)" % (g, 88 + 4 * i) for i, g in enumerate(GPR)] +
            ["mfcr 0", "stw 0, 120(3)", "li 0, 0", "addze 0, 0", "stw 0, 124(3)"] +
            ["stfd %d, %d(3)" % (fr, 128 + 8 * i) for i, fr in enumerate(FPR)] +
            ["blr"])


def asm_for(programs):
    out = ["\t.text"]
    for i, body in enumerate(programs):
        name = "t%d" % i
        out += ["\t.globl %s" % name, "\t.type %s,@function" % name, "\t.p2align 2", "%s:" % name]
        for line in PROLOGUE:
            out.append("\t" + line)
        for group in body:
            for line in group:
                out.append("\t" + line)
        for line in EPILOGUE:
            out.append("\t" + line)
        out.append("\t.size %s, .-%s" % (name, name))
    return "\n".join(out) + "\n"


# --- inputs ----------------------------------------------------------------

INTERESTING_W = [0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x8000, 0xffff, 0x10000, 31, 32, 33, 63]
INTERESTING_D = [0.0, -0.0, 1.0, -1.0, 0.5, 3.0, 1e300, -1e300, 1e-310, 2147483647.5, -2147483648.0,
                 2147483648.0, 4294967296.0, float("inf"), float("-inf"), float("nan"), 1.0 / 3.0,
                 16777217.0, 0.1, 123456789.123]


def gen_input(rng):
    words = [rng.choice(INTERESTING_W) if rng.random() < 0.35 else rng.getrandbits(32) for _ in GPR]
    cr = rng.getrandbits(32) & SO_MASK
    ca = rng.randrange(2)
    dbl = [rng.choice(INTERESTING_D) if rng.random() < 0.5 else rng.uniform(-1e6, 1e6) for _ in FPR]
    scratch = bytes(rng.getrandbits(8) for _ in range(64))
    blob = bytearray(STATE_SIZE)
    struct.pack_into(">8I", blob, 0, *words)
    struct.pack_into(">II", blob, 32, cr, ca)
    struct.pack_into(">6d", blob, 40, *dbl)
    blob[SCRATCH:SCRATCH + 64] = scratch
    return bytes(blob)


# --- the two runners ---------------------------------------------------------

def c_bytes(b):
    return ",".join("0x%02x" % x for x in b)


def build_and_run(programs, inputs, work, tools):
    asm = os.path.join(work, "t.s")
    with open(asm, "w") as fh:
        fh.write(asm_for(programs))
    n = len(programs)
    inputs_c = "static const unsigned char IN[%d][%d] = {%s};\n" % (
        len(inputs), STATE_SIZE, ",".join("{%s}" % c_bytes(b) for b in inputs))

    # ground truth: the same assembly, linked into a PowerPC Linux program
    decls = "".join("void t%d(unsigned char *);\n" % i for i in range(n))
    table = "static void (*T[])(unsigned char *) = {%s};\n" % ",".join("t%d" % i for i in range(n))
    gt_c = os.path.join(work, "gt.c")
    with open(gt_c, "w") as fh:
        fh.write("#include <stdio.h>\n#include <string.h>\n" + decls + table + inputs_c + r"""
int main(void) {
    static unsigned char s[%d] __attribute__((aligned(8)));
    for (int t = 0; t < %d; t++) for (int i = 0; i < %d; i++) {
        memcpy(s, IN[i], sizeof s);
        T[t](s);
        printf("%%d %%d ", t, i);
        for (int k = 88; k < %d; k++) printf("%%02x", s[k]);
        printf("\n");
    }
    return 0;
}
""" % (STATE_SIZE, n, len(inputs), STATE_SIZE))
    gt_bin = os.path.join(work, "gt")
    subprocess.run([tools["zig"], "cc", "-target", "powerpc-linux-musl", "-static", "-O1",
                    gt_c, asm, "-o", gt_bin], check=True, capture_output=True)
    gt_out = subprocess.run([tools["qemu"], "-cpu", "750", gt_bin], check=True,
                            capture_output=True, text=True, timeout=300).stdout

    # recompiled: the same assembly as an object, through recomp
    obj = os.path.join(work, "t.o")
    subprocess.run([tools["zig"], "cc", "-target", "powerpc-freestanding-eabihf", "-c", asm, "-o", obj],
                   check=True, capture_output=True)
    gen = os.path.join(work, "gen.c")
    rc = subprocess.run([tools["recomp"], obj, "-o", gen], capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError("recomp failed:\n" + rc.stderr[-2000:])
    unhandled = [l for l in rc.stderr.splitlines() if "unhandled" in l and ", 0 unhandled" not in l]
    hdecls = "".join("void ppc_t%d(PpcContext *);\n" % i for i in range(n))
    htable = "static void (*T[])(PpcContext *) = {%s};\n" % ",".join("ppc_t%d" % i for i in range(n))
    h_c = os.path.join(work, "h.c")
    with open(h_c, "w") as fh:
        fh.write('#include <stdio.h>\n#include "ppc_runtime.h"\nvoid ppc_init_globals(PpcContext *);\n'
                 + hdecls + htable + inputs_c + r"""
int main(void) {
    static PpcContext ctx;
    static PpcSharedMemory sh;
    const uint32_t base = 0x10000u;
    ctx.shared = &sh;
    ppc_init_globals(&ctx);
    for (int t = 0; t < %d; t++) for (int i = 0; i < %d; i++) {
        memset(ctx.r, 0, sizeof ctx.r);
        ctx.r[1] = PPC_MEM_SIZE - 256;
        ctx.r[3] = base;
        for (int k = 0; k < %d; k++) ppc_store_u8(&ctx, base + k, IN[i][k]);
        T[t](&ctx);
        printf("%%d %%d ", t, i);
        for (int k = 88; k < %d; k++) printf("%%02x", ppc_load_u8(&ctx, base + k));
        printf("\n");
    }
    return 0;
}
""" % (n, len(inputs), STATE_SIZE, STATE_SIZE))
    h_bin = os.path.join(work, "h")
    # DIFFTEST_UBSAN=1: build the recompiled side with UndefinedBehaviorSanitizer
    # and make any report fatal. Undefined behaviour in generated code can
    # produce the right value by luck and the wrong one after the next
    # compiler upgrade; `neg.` of INT_MIN was exactly that.
    ubsan = ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"] if os.environ.get("DIFFTEST_UBSAN") else []
    cc = subprocess.run(["gcc", "-O1", "-w"] + ubsan + ["-I", INCLUDE, gen, h_c, "-o", h_bin, "-lm"],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        raise RuntimeError("host build failed:\n" + cc.stderr[-3000:])
    hr = subprocess.run([h_bin], capture_output=True, text=True, timeout=300)
    if hr.returncode != 0:
        raise RuntimeError("recompiled program failed (exit %d):\n%s" % (hr.returncode, hr.stderr[-3000:]))
    h_out = hr.stdout
    return gt_out.splitlines(), h_out.splitlines(), unhandled


# --- comparing ---------------------------------------------------------------

def fields(hexstate):
    b = bytes.fromhex(hexstate)
    out = {}
    for i, g in enumerate(GPR):
        out["r%d" % g] = struct.unpack_from(">I", b, 4 * i)[0]
    # Bit 3 of each CR field is SO for integer compares and FU (unordered)
    # for fcmpu. The recompiler models neither: SO can never be set by
    # anything it supports, and bun/bnu -- the only branches that read FU --
    # are not in its instruction set, so no game code it can run observes
    # it. LT, GT and EQ are compared exactly.
    out["cr"] = struct.unpack_from(">I", b, 32)[0] & SO_MASK
    out["ca"] = struct.unpack_from(">I", b, 36)[0]
    for i, fr in enumerate(FPR):
        bits = struct.unpack_from(">Q", b, 40 + 8 * i)[0]
        v = struct.unpack_from(">d", b, 40 + 8 * i)[0]
        out["f%d" % fr] = "nan" if math.isnan(v) else "%016x" % bits
    for k in range(0, 64, 4):
        w = struct.unpack_from(">I", b, 88 + k)[0]
        # A word with a NaN's bit pattern (a stored single, or the high word
        # of a stored double) is compared without its sign. The default NaN
        # an invalid operation produces is positive on PowerPC and ARM64 --
        # the Switch -- but negative on x86, where this runs.
        if (w & 0x7f800000) == 0x7f800000 and (w & 0x007fffff):
            w &= 0x7fffffff
        out["mem+%d" % k] = w
    return out


def diff(gt_line, h_line):
    a, b = fields(gt_line.split()[2]), fields(h_line.split()[2])
    return {k: (a[k], b[k]) for k in a if a[k] != b[k]}


def fmt(v):
    return ("0x%08x" % v) if isinstance(v, int) else v


def run_batch(programs, inputs, tools):
    with tempfile.TemporaryDirectory() as work:
        gt, h, unhandled = build_and_run(programs, inputs, work, tools)
    fails = {}
    for gl, hl in zip(gt, h):
        if gl != hl:
            t, i = int(gl.split()[0]), int(gl.split()[1])
            d = diff(gl, hl)
            if d:
                fails.setdefault(t, (i, d))
    if len(gt) != len(h):
        raise RuntimeError("output length differs: qemu %d lines, host %d" % (len(gt), len(h)))
    return fails, unhandled


def shrink(body, inp, tools):
    """Drop instruction groups one at a time while the mismatch survives."""
    cur = list(body)
    changed = True
    while changed:
        changed = False
        for k in range(len(cur)):
            trial = cur[:k] + cur[k + 1:]
            fails, _ = run_batch([trial], [inp], tools)
            if fails:
                cur = trial
                changed = True
                break
    return cur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=200)
    ap.add_argument("--length", type=int, default=24)
    ap.add_argument("--inputs", type=int, default=8)
    ap.add_argument("--no-shrink", action="store_true")
    args = ap.parse_args()
    tools = {"zig": os.environ.get("ZIG", os.path.expanduser("~/devtools/zig/zig")),
             "qemu": os.environ.get("QEMU_PPC", "qemu-ppc-static"),
             "recomp": os.environ.get("RECOMP", "build/recomp")}
    rng = random.Random(args.seed)
    programs = [gen_body(rng, args.length) for _ in range(args.programs)]
    inputs = [gen_input(rng) for _ in range(args.inputs)]
    fails, unhandled = run_batch(programs, inputs, tools)
    if unhandled:
        print("recomp reported unhandled instructions:")
        for l in unhandled[:20]:
            print("  " + l)
    total = args.programs * args.inputs
    print("seed %d: %d programs x %d inputs = %d runs, %d program(s) differ"
          % (args.seed, args.programs, args.inputs, total, len(fails)))
    for t, (i, d) in sorted(fails.items())[:10]:
        body = programs[t] if args.no_shrink else shrink(programs[t], inputs[i], tools)
        print("\n--- program %d, input %d%s" % (t, i, "" if args.no_shrink else " (shrunk)"))
        for group in body:
            for line in group:
                print("    " + line)
        _, d2 = next(iter(run_batch([body], [inputs[i]], tools)[0].values()), (None, d))
        blob = inputs[i]
        print("  inputs: " + " ".join("r%d=0x%08x" % (g, struct.unpack_from(">I", blob, 4 * k)[0])
                                      for k, g in enumerate(GPR)))
        print("          cr=0x%08x ca=%d" % struct.unpack_from(">II", blob, 32) +
              " " + " ".join("f%d=%r" % (fr, struct.unpack_from(">d", blob, 40 + 8 * k)[0])
                             for k, fr in enumerate(FPR)))
        for k, (want, got) in sorted(d2.items()):
            print("  %-7s qemu %s   recompiled %s" % (k, fmt(want), fmt(got)))
    return 1 if (fails or unhandled) else 0


if __name__ == "__main__":
    sys.exit(main())
