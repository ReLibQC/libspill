/* SPDX-License-Identifier: BSD-3-Clause */
/* Reopening a kept store must not serve its own table of contents back as data.
 *
 * ls_close(s, 1) writes the ToC at file_end, so the bytes just past the last
 * data byte are ToC, not data. On the next open file_end is restored and a
 * later record growth hands out a tail extent starting exactly there -- on top
 * of those ToC bytes. ensure_extents zeroes every new extent for precisely this
 * reason, but the zeroing went through one FALLOC_FL_ZERO_RANGE call that was
 * asked to both zero and extend the file; on ext4 that call extended the file,
 * returned success, and left the bytes below the old end of file alone. The
 * caller then read ToC bytes out of a region it had never written, with
 * ls_write and ls_read both reporting LS_OK throughout (issue #7).
 *
 * The offsets below are the ones the original random reproducer failed on: the
 * third cycle's write is what first needs a tail extent over the old ToC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libspill.h"

static int fails = 0, ntest = 0;

static void ok(int cond, const char *what)
{
    ntest++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

static const char *dir(void)
{
    const char *d = getenv("TMPDIR");
    return (d && *d) ? d : "/tmp";
}

#define CAP 600000u

static unsigned char shadow[CAP];
static unsigned char back[CAP];

static unsigned char pat(unsigned long off, int gen)
{
    return (unsigned char)((off * 31u + (unsigned)gen * 7u) & 0xffu);
}

/* Every byte below the record's size must read back as written, and the bytes
 * never written must read as zero -- §4b makes no distinction. */
static int intact(ls_store *s, const char *when)
{
    uint64_t sz = 0;
    size_t i;
    int rc;

    rc = ls_size(s, "d", &sz);
    if (rc == LS_ERR_NOKEY) return 1;          /* nothing written yet */
    if (rc != LS_OK) { printf("  ls_size failed %s\n", when); return 0; }
    if (sz > CAP) { printf("  record grew past the harness buffer\n"); return 0; }
    if (ls_read(s, "d", 0, (size_t)sz, back) != LS_OK) {
        printf("  ls_read failed %s\n", when);
        return 0;
    }
    for (i = 0; i < (size_t)sz; i++) {
        if (back[i] != shadow[i]) {
            printf("  [FAIL] %s: byte %lu reads %u, wanted %u\n",
                   when, (unsigned long)i, back[i], shadow[i]);
            return 0;
        }
    }
    return 1;
}

static void put(ls_store *s, unsigned long off, size_t n, int gen)
{
    static unsigned char buf[32768];
    size_t i;

    for (i = 0; i < n; i++) buf[i] = pat(off + i, gen);
    if (ls_write(s, "d", off, n, buf) != LS_OK) { printf("  ls_write failed\n"); fails++; return; }
    memcpy(shadow + off, buf, n);
}

int main(void)
{
    /* Cycle 2's last write is the one that must reach past the old ToC. */
    static const struct { unsigned long off; size_t n; } w[3][3] = {
        { { 209721, 8061 }, { 420121, 1946 }, { 399881, 7612 } },
        { { 237806, 2781 }, { 391073, 16919 }, { 104440, 3153 } },
        { { 207143, 14743 }, { 65277, 12791 }, { 487137, 17910 } },
    };
    ls_opts o;
    ls_store *s;
    int err = 0, c, i;

    /* Worth saying out loud: on a filesystem without FALLOC_FL_ZERO_RANGE
     * (tmpfs, notably) the zeroing always went through the explicit write
     * fallback, which was never wrong, so a pass here proves nothing. */
    printf("libspill reopen (issue #7), stores in %s\n", dir());

    ls_opts_default(&o);
    o.dir = dir();

    for (c = 0; c < 3; c++) {
        char what[64];

        s = ls_open("reopen", &o, &err);
        if (!s) { printf("  open failed at cycle %d: %d\n", c, err); return 1; }

        snprintf(what, sizeof what, "cycle %d opens intact", c);
        ok(intact(s, what), what);

        for (i = 0; i < 3; i++) {
            put(s, w[c][i].off, w[c][i].n, c);
            snprintf(what, sizeof what, "  ... intact after cycle %d write %d", c, i);
            ok(intact(s, what), what);
        }

        /* keep = 1: this is what leaves a ToC in the path of the next extent. */
        if (ls_close(s, 1) != LS_OK) { printf("  close failed\n"); return 1; }
    }

    s = ls_open("reopen", &o, &err);
    ok(s != NULL && intact(s, "final reopen"), "the store survives three reopens");
    if (s) ls_close(s, 0);

    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
