/* SPDX-License-Identifier: BSD-3-Clause */
/* Conformance test for the crayio shim (§6a).
 *
 * The question is narrow: can §4's API express the word-addressed workload that
 * four codes have been carrying private copies of since the 1980s? The cases
 * below are the ones those copies' own error paths care about, plus the two
 * things the shim does better than the original.
 */
#include <stdio.h>
#include <string.h>

#include "crayio_shim.h"

static int fails = 0, ntest = 0;

static void ok(int cond, const char *what)
{
    ntest++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

static void ok_err(CRAY_INT got, CRAY_INT want, const char *what)
{
    ntest++;
    if (got != want) {
        fails++;
        printf("  [FAIL] %-52s ierr %ld, wanted %ld\n", what, (long)got, (long)want);
    } else {
        printf("  [PASS] %s\n", what);
    }
}

int main(void)
{
    CRAY_INT unit = 7, lennam, blocks = 0, stats = 0, ierr = 99;
    CRAY_INT addr, count;
    double out[256], back[256];
    int i, good;

    puts("crayio (WOPEN/WCLOSE/GETWA/PUTWA) over libspill");

    for (i = 0; i < 256; i++) out[i] = 1.0 + 0.5 * i;

    lennam = 6;
    wopen_(&unit, "SIRIUS", &lennam, &blocks, &stats, &ierr);
    ok_err(ierr, 0, "wopen a named unit");

    /* The defining idiom: 1-based word addresses, counts in 64-bit words. */
    addr = 1; count = 256;
    putwa_(&unit, out, &addr, &count, &ierr);
    ok_err(ierr, 0, "putwa 256 words at word 1");

    memset(back, 0, sizeof back);
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, 0, "getwa them back");
    ok(memcmp(out, back, sizeof out) == 0, "  ... byte-exact");

    /* A read starting mid-record, which is where an off-by-one in the 1-based
     * translation would show and nowhere else. */
    addr = 129; count = 1;
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, 0, "getwa one word at word 129");
    ok(back[0] == out[128], "  ... 1-based addressing lands on element 129");

    /* Append past the end, the way these callers grow a file. */
    addr = 257; count = 256;
    putwa_(&unit, out, &addr, &count, &ierr);
    ok_err(ierr, 0, "putwa extends the file");
    memset(back, 0, sizeof back);
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, 0, "getwa reads the extension");
    ok(memcmp(out, back, sizeof out) == 0, "  ... byte-exact");

    /* The error paths every copy implements. */
    addr = 1000; count = 64;
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, -5, "getwa past the end is -5");

    addr = 0; count = 1;
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, -4, "a zero word address is -4");
    addr = -3;
    putwa_(&unit, out, &addr, &count, &ierr);
    ok_err(ierr, -4, "a negative word address is -4");
    addr = 1; count = -1;
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, -4, "a negative count is -4");

    {
        CRAY_INT bad = 4242;
        getwa_(&bad, back, &addr, &count, &ierr);
        ok_err(ierr, -1, "an unopened unit is -1");
    }

    /* wclose keeps the file, and a reopened unit still has its words. */
    wclose_(&unit, &ierr);
    ok_err(ierr, 0, "wclose");
    lennam = 6;
    wopen_(&unit, "SIRIUS", &lennam, &blocks, &stats, &ierr);
    ok_err(ierr, 0, "reopen the same unit");
    addr = 1; count = 256;
    memset(back, 0, sizeof back);
    getwa_(&unit, back, &addr, &count, &ierr);
    ok_err(ierr, 0, "getwa after reopen");
    ok(memcmp(out, back, sizeof out) == 0, "  ... the file survived wclose");
    wclose_(&unit, &ierr);

    /* §6a's "bug class we delete by construction": every copy has a fixed
     * max_file of 99 (LSDalton 250, after someone hit it). There is no table to
     * outgrow here. */
    {
        CRAY_INT big = 4096;
        lennam = 0;                       /* exercise the fort.NN default too */
        wopen_(&big, NULL, &lennam, &blocks, &stats, &ierr);
        ok_err(ierr, 0, "wopen unit 4096, far past every copy's max_file");
        addr = 1; count = 4;
        putwa_(&big, out, &addr, &count, &ierr);
        ok_err(ierr, 0, "  ... and it works");
        memset(back, 0, sizeof back);
        getwa_(&big, back, &addr, &count, &ierr);
        good = (ierr == 0) && back[0] == out[0] && back[3] == out[3];
        ok(good, "  ... round-trips");
        wclose_(&big, &ierr);
    }

    cray_shim_reset();
    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
