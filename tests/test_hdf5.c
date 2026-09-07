/* SPDX-License-Identifier: BSD-3-Clause */
/* The optional HDF5 backend (§7b).
 *
 * Two things to establish. That the API behaves the same on both backends --
 * §7b says "the API in §4 is the same either way" and that is only true if
 * checked -- and that §7b's reason for keeping HDF5 optional still holds, by
 * running the same churn protocol through both and comparing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libspill.h"

static int fails = 0, ntest = 0;

static void ok(int cond, const char *what)
{
    ntest++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

static void ok_rc(int rc, int want, const char *what)
{
    char a[LS_ERRBUF_MIN], b[LS_ERRBUF_MIN];
    ntest++;
    if (rc != want) {
        fails++;
        printf("  [FAIL] %-46s got %s, wanted %s\n", what,
               ls_strerror(rc, a, sizeof a), ls_strerror(want, b, sizeof b));
    } else {
        printf("  [PASS] %s\n", what);
    }
}

/* A 64-bit LCG, so it must say so: unsigned long is 32 bits on Windows, where
 * the constants would truncate and rs >> 33 is undefined. */
static uint64_t rs = 1;
static uint64_t nextr(void) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
                              return (rs >> 33); }

/* The protocol of tests/churn_libspill.c and tests/hdf5_churn_varsize.py:
 * 8 records, 60 cycles, each recreated at a different random size. */
static double churn(ls_backend backend, const char *name)
{
    ls_opts o;
    ls_store *s;
    int err = 0, i, c;
    size_t sizes[8];
    double *buf = malloc((size_t)(1 << 18) * sizeof *buf);
    char key[32], path[512];
    unsigned long long live = 0, fsz = 0;
    struct stat st;
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";

    if (!buf) return -1.0;
    for (i = 0; i < (1 << 18); i++) buf[i] = (double)i;

    rs = 1;                                   /* same size sequence both times */
    ls_opts_default(&o);
    o.backend = backend;
    s = ls_open(name, &o, &err);
    if (!s) { free(buf); return -1.0; }

    for (i = 0; i < 8; i++) {
        sizes[i] = (size_t)((1 << 14) + nextr() % ((1 << 18) - (1 << 14)));
        sprintf(key, "k%d", i);
        if (ls_write(s, key, 0, sizes[i] * sizeof *buf, buf) != LS_OK) goto bad;
    }
    for (c = 0; c < 60; c++) {
        int k = c % 8;
        sprintf(key, "k%d", k);
        if (ls_erase(s, key) != LS_OK) goto bad;
        sizes[k] = (size_t)((1 << 14) + nextr() % ((1 << 18) - (1 << 14)));
        if (ls_write(s, key, 0, sizes[k] * sizeof *buf, buf) != LS_OK) goto bad;
    }
    for (i = 0; i < 8; i++) live += (unsigned long long)sizes[i] * sizeof *buf;

    snprintf(path, sizeof path, "%s/%s.libspill", dir, name);
    if (stat(path, &st) == 0) fsz = (unsigned long long)st.st_size;
    ls_close(s, 0);
    free(buf);
    return live ? (double)fsz / (double)live : -1.0;
bad:
    ls_close(s, 0);
    free(buf);
    return -1.0;
}

int main(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0, i, found = 0;
    double v[512], back[512];
    uint64_t n = 0, off = 0;

    puts("libspill HDF5 backend");

    ls_opts_default(&o);
    o.backend = LS_HDF5;
    s = ls_open("h5_probe", &o, &err);
    if (!s && err == LS_ERR_BACKEND) {
        puts("  skipped: built without HDF5");
        return 0;
    }
    ok(s != NULL, "open an HDF5-backed store");
    if (!s) return 1;

    for (i = 0; i < 512; i++) v[i] = 1.0 + 0.25 * i;

    /* §7b: "the API in §4 is the same either way". */
    ok_rc(ls_write(s, "t2", 0, sizeof v, v), LS_OK, "write");
    memset(back, 0, sizeof back);
    ok_rc(ls_read(s, "t2", 0, sizeof back, back), LS_OK, "read");
    ok(memcmp(v, back, sizeof v) == 0, "  ... byte-exact");

    ok_rc(ls_read(s, "t2", 256 * sizeof(double), 8, back), LS_OK, "read at an offset");
    ok(back[0] == v[256], "  ... returns the right element");
    ok_rc(ls_read(s, "t2", 0, 2 * sizeof v, back), LS_ERR_RANGE, "read past the end is RANGE");
    ok_rc(ls_read(s, "absent", 0, 8, back), LS_ERR_NOKEY, "read of a missing key is NOKEY");

    ok_rc(ls_size(s, "t2", &n), LS_OK, "size");
    ok(n == sizeof v, "  ... reports the byte length");
    ok_rc(ls_exists(s, "t2", &found), LS_OK, "exists");
    ok(found, "  ... finds it");

    /* Reserved space must read as zero here too (§4b). */
    ok_rc(ls_reserve(s, "z", 4096), LS_OK, "reserve");
    ok_rc(ls_read(s, "z", 0, sizeof back, back), LS_OK, "read reserved space");
    {
        int zeroed = 1;
        for (i = 0; i < 512; i++) if (back[i] != 0.0) zeroed = 0;
        ok(zeroed, "  ... and it is zero");
    }

    ok_rc(ls_append(s, "app", sizeof v, v, &off), LS_OK, "append");
    ok(off == 0, "  ... to a new key is offset 0");
    ok_rc(ls_append(s, "app", sizeof v, v, &off), LS_OK, "append again");
    ok(off == sizeof v, "  ... reports the previous end");

    {
        double one[512];
        for (i = 0; i < 512; i++) one[i] = 1.0;
        ok_rc(ls_accumulate(s, "t2", 0, sizeof one, one, ls_add_f64, NULL), LS_OK,
              "accumulate");
        ls_read(s, "t2", 0, sizeof back, back);
        ok(back[0] == v[0] + 1.0 && back[511] == v[511] + 1.0, "  ... dst += src");
    }

    /* Attributes are a native HDF5 concept, and h5dump shows them. */
    {
        char blob[64];
        size_t an = sizeof blob;
        ok_rc(ls_set_attr(s, "t2", "f8/(512,)", 9), LS_OK, "set_attr");
        ok_rc(ls_get_attr(s, "t2", blob, &an), LS_OK, "get_attr");
        ok(an == 9 && memcmp(blob, "f8/(512,)", 9) == 0, "  ... byte-exact");
    }

    {
        char **keys = NULL;
        size_t nk = 0;
        ok_rc(ls_keys(s, &keys, &nk), LS_OK, "keys");
        ok(nk == 3, "  ... lists every dataset");   /* t2, z, app */
        ls_keys_free(keys, nk);
    }
    ok_rc(ls_erase(s, "z"), LS_OK, "erase");
    ok_rc(ls_erase(s, "z"), LS_ERR_NOKEY, "  ... twice is NOKEY");

    /* What this backend refuses, and why (§7b). */
    {
        ls_req *rq = NULL;
        ok_rc(ls_aread(s, "t2", 0, 8, back, &rq), LS_ERR_MODE,
              "ls_aread is LS_ERR_MODE: HDF5 offers no I/O concurrency");
        ok_rc(ls_awrite(s, "t2", 0, 8, back, &rq), LS_ERR_MODE, "ls_awrite likewise");
    }
    ls_close(s, 0);

    ls_opts_default(&o);
    o.backend = LS_HDF5;
    o.memory_budget = 1 << 20;
    ok(ls_open("h5_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "a memory budget on an HDF5 store is refused, not ignored");
    ls_opts_default(&o);
    o.backend = LS_HDF5;
    o.direct_io = 1;
    ok(ls_open("h5_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "O_DIRECT on an HDF5 store is refused");
    ls_opts_default(&o);
    o.backend = LS_HDF5;
    o.mode = LS_MAPPED;
    ok(ls_open("h5_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "LS_MAPPED with HDF5 is refused");

    /* Persistence, and that the file really is HDF5. */
    ls_opts_default(&o);
    o.backend = LS_HDF5;
    s = ls_open("h5_keep", &o, &err);
    ls_write(s, "kept", 0, sizeof v, v);
    ok_rc(ls_close(s, 1), LS_OK, "close with keep=1");
    s = ls_open("h5_keep", &o, &err);
    ok(s != NULL, "reopen");
    memset(back, 0, sizeof back);
    ok_rc(ls_read(s, "kept", 0, sizeof back, back), LS_OK, "read after reopen");
    ok(memcmp(v, back, sizeof v) == 0, "  ... byte-exact");
    ls_close(s, 0);

    /* §7b's reason for keeping this optional, measured through our own code
     * rather than quoted: the same churn protocol on both backends. */
    {
        double posix_ratio = churn(LS_POSIX, "h5_churn_p");
        double hdf5_ratio  = churn(LS_HDF5,  "h5_churn_h");
        printf("  churn, 8 records x 60 cycles at varying sizes:\n");
        printf("    POSIX backend  x%.2f\n", posix_ratio);
        printf("    HDF5 backend   x%.2f\n", hdf5_ratio);
        ok(posix_ratio > 0 && hdf5_ratio > 0, "both backends completed the churn");
        ok(hdf5_ratio > posix_ratio,
           "  ... and HDF5 grows more, as §7b found");
    }

    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
