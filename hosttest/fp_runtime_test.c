/* The floating-point runtime helpers, against values the architecture fixes.
 *
 * Every case here is either a value hosttest/difftest found differing from
 * qemu-ppc on 2026-09-24, or an edge of the same rule it is unlikely to hit
 * by chance (denormals, saturation, NaN). Build and run:
 *
 *     gcc -O2 -I include hosttest/fp_runtime_test.c -o /tmp/fp_runtime_test -lm
 *     /tmp/fp_runtime_test
 *
 * Also run at -O2 on purpose: a helper that only works because the compiler
 * did not fold something at -O0 is not working. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ppc_runtime.h"

static int failures;

static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static double from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

static void check_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) { failures++; printf("FAIL %-44s got 0x%08x want 0x%08x\n", what, got, want); }
}

static void check_bits(const char *what, double got, uint64_t want)
{
    if (dbits(got) != want) {
        failures++;
        printf("FAIL %-44s got %016llx want %016llx\n", what,
               (unsigned long long)dbits(got), (unsigned long long)want);
    }
}

int main(void)
{
    /* stfs truncates rather than rounds */
    check_u32("stfs 2147483647.5 (truncates)", ppc_double_to_single_bits(2147483647.5), 0x4effffffu);
    check_u32("stfs 1.0", ppc_double_to_single_bits(1.0), 0x3f800000u);
    check_u32("stfs -0.0", ppc_double_to_single_bits(-0.0), 0x80000000u);
    check_u32("stfs +inf", ppc_double_to_single_bits(INFINITY), 0x7f800000u);
    check_u32("stfs 0.1 (truncates the 0.1 mantissa)", ppc_double_to_single_bits(0.1), 0x3dcccccc);
    /* single denormals: 2^-149 is mantissa 1, 2^-127 is 2^22 of those */
    check_u32("stfs 2^-149", ppc_double_to_single_bits(ldexp(1.0, -149)), 0x00000001u);
    check_u32("stfs 2^-127", ppc_double_to_single_bits(ldexp(1.0, -127)), 0x00400000u);
    check_u32("stfs -2^-126 (smallest normal)", ppc_double_to_single_bits(-ldexp(1.0, -126)), 0x80800000u);
    check_u32("stfs 1.5 * 2^-130", ppc_double_to_single_bits(1.5 * ldexp(1.0, -130)), 0x000c0000u);
    uint32_t nan_bits = ppc_double_to_single_bits(NAN);
    check_u32("stfs NaN stays NaN", (nan_bits & 0x7f800000u) == 0x7f800000u && (nan_bits & 0x7fffffu), 1);

    /* fctiwz saturates, and NaN is 0x80000000 */
    check_bits("fctiwz 2147483648.0 saturates", ppc_fctiwz(2147483648.0), 0x000000007fffffffull);
    check_bits("fctiwz 1e300 saturates", ppc_fctiwz(1e300), 0x000000007fffffffull);
    check_bits("fctiwz -2147483649.0 saturates", ppc_fctiwz(-2147483649.0), 0x0000000080000000ull);
    check_bits("fctiwz -2147483648.9 truncates", ppc_fctiwz(-2147483648.9), 0x0000000080000000ull);
    check_bits("fctiwz NaN", ppc_fctiwz(NAN), 0x0000000080000000ull);
    check_bits("fctiwz -inf", ppc_fctiwz(-INFINITY), 0x0000000080000000ull);
    check_bits("fctiwz -7842.7 truncates", ppc_fctiwz(-7842.7), 0x00000000ffffe15eull);
    check_bits("fctiwz 2147483647.5", ppc_fctiwz(2147483647.5), 0x000000007fffffffull);

    /* fabs/fnabs are sign-bit operations */
    check_bits("fabs -0.0", ppc_fabs(-0.0), 0x0000000000000000ull);
    check_bits("fnabs +0.0", ppc_fnabs(0.0), 0x8000000000000000ull);
    check_bits("fnabs -0.0", ppc_fnabs(-0.0), 0x8000000000000000ull);
    check_bits("fabs of a negative NaN", ppc_fabs(from_bits(0xfff8000000000001ull)), 0x7ff8000000000001ull);

    /* fmadds rounds once.
     *
     * 16777217 = 2^24 + 1 sits exactly halfway between the singles 2^24 and
     * 2^24 + 2, and is 24929 * 673, a product of two exact singles. */
    {
        /* an exact tie with nothing behind it goes to even: 2^24 */
        check_bits("fmadds exact tie -> even", ppc_fmadds(24929.0, 673.0, 0.0), dbits(16777216.0));
        /* the same tie plus 2^-40. A double cannot hold 2^24 + 1 + 2^-40
         * (its ulp there is 2^-28), so fma() returns 2^24 + 1 exactly and
         * frsp then breaks the false tie to even: 2^24. The exact value is
         * above the tie, so the hardware gives 2^24 + 2. */
        check_bits("fmadds tie + 2^-40 rounds up", ppc_fmadds(24929.0, 673.0, ldexp(1.0, -40)),
                   dbits(16777218.0));
        check_bits("fmadds tie - 2^-40 rounds down", ppc_fmadds(24929.0, 673.0, -ldexp(1.0, -40)),
                   dbits(16777216.0));
        /* negated: fnmsubs is -(a*c - b), rounding is symmetric */
        check_bits("fnmsubs form of the same", -ppc_fmadds(24929.0, 673.0, -(-ldexp(1.0, -40))),
                   dbits(-16777218.0));
        /* and the unfixed path, for contrast, would have given 2^24 */
        if (dbits((double)(float)fma(24929.0, 673.0, ldexp(1.0, -40))) != dbits(16777216.0))
            printf("note: this host's fma-then-frsp does not show the double rounding\n");
        /* ordinary values are untouched */
        check_bits("fmadds 1.5*2+0.25", ppc_fmadds(1.5, 2.0, 0.25), dbits(3.25));
    }

    /* ppc_single_to_double: lfs moves a NaN's bits, signaling bit included */
    {
        const uint32_t snan = 0x7fb40eb5u, qnan = 0xffc00001u;
        float fs, fq;
        memcpy(&fs, &snan, 4);
        memcpy(&fq, &qnan, 4);
        check_bits("lfs keeps a signaling NaN signaling", ppc_single_to_double(fs), 0x7ff681d6a0000000ull);
        check_u32("lfs;stfs round-trips a signaling NaN", ppc_double_to_single_bits(ppc_single_to_double(fs)), snan);
        check_u32("lfs;stfs round-trips a negative quiet NaN", ppc_double_to_single_bits(ppc_single_to_double(fq)), qnan);
        check_bits("lfs of 1.5 is exact", ppc_single_to_double(1.5f), dbits(1.5));
        check_bits("lfs of a single denormal is exact", ppc_single_to_double(ldexpf(1.0f, -140)), dbits(ldexp(1.0, -140)));
    }

    /* ppc_fneg_result: fnmadd/fnmsub negate everything but a NaN */
    {
        double pnan;
        uint64_t pn = 0x7ff8000000000001ull;
        memcpy(&pnan, &pn, 8);
        check_bits("fnm* leaves a NaN's sign", ppc_fneg_result(pnan), pn);
        check_bits("fnm* negates 2.0", ppc_fneg_result(2.0), dbits(-2.0));
        check_bits("fnm* negates +0.0", ppc_fneg_result(0.0), dbits(-0.0));
        /* -(-663088.4 * 0 + 0): the sum is +0, so the result is -0. An
         * ARM64 fnmadd, -(a*c) - b, would give +0. */
        volatile double m = -663088.4, z = 0.0;
        check_bits("fnmadd of an exact zero is -0", ppc_fneg_result(ppc_fmadd(m, z, z)), dbits(-0.0));
    }

    /* the fused family's NaN rule: first NaN of frA, frB, frC, quieted,
     * sign and payload kept -- checked against qemu-ppc */
    {
        double qa, sb;
        uint64_t qa_bits = 0x7ff8000000000123ull, sb_bits = 0xfff4000000000456ull;
        memcpy(&qa, &qa_bits, 8);
        memcpy(&sb, &sb_bits, 8);
        check_bits("fmsub keeps b's NaN sign", ppc_fmsub(1.0, 2.0, sb), 0xfffc000000000456ull);
        check_bits("fmadd prefers frA over frB", ppc_fmadd(sb, 2.0, qa), 0xfffc000000000456ull);
        check_bits("fmadd prefers frB over frC", ppc_fmadd(1.0, qa, sb), 0xfffc000000000456ull);
        check_bits("fmsubs: single payload, b's sign", ppc_fmsubs(2.0, 1.0, qa), 0x7ff8000000000000ull);
        check_bits("fnmsubs leaves the NaN alone", ppc_fneg_result(ppc_fmsubs(1.0, 2.0, sb)), 0xfffc000000000000ull);
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("fp runtime: all checks passed\n");
    return 0;
}
