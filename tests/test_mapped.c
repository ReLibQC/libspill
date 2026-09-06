/* SPDX-License-Identifier: BSD-3-Clause */
/* LS_MAPPED (§3): the opt-in second mode, added after the survey found qp2 and
 * RMG using mapped scratch for real work.
 *
 * The interesting case is a record made of several extents. A record is not
 * contiguous in the file, so it cannot be handed to one mmap call -- but it can
 * still be handed to the caller as one pointer, by reserving the span and
 * mapping each extent over its slice with MAP_FIXED. If that stitching is
 * wrong, a multi-extent record reads correctly through ls_read and incorrectly
 * through the mapping, which is exactly what the test below separates.
 */
#include <errno.h>
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
        printf("  [FAIL] %-48s got %s, wanted %s\n", what,
               ls_strerror(rc, a, sizeof a), ls_strerror(want, b, sizeof b));
    } else {
        printf("  [PASS] %s\n", what);
    }
}

static ls_store *open_mapped(const char *name, int *err)
{
    ls_opts o;
    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    return ls_open(name, &o, err);
}

int main(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0, i;
    void *addr = NULL;
    size_t len = 0;
    double *p;

    puts("libspill LS_MAPPED");

    /* The combinations §3 and §4b rule out. */
    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    o.memory_budget = 1 << 20;
    ok(ls_open("m_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "a memory budget on a mapped store is refused");
    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    o.backend = LS_HDF5;
    ok(ls_open("m_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "LS_MAPPED with the HDF5 backend is refused");
    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    o.direct_io = 1;
    ok(ls_open("m_bad", &o, &err) == NULL && err == LS_ERR_INVAL,
       "O_DIRECT and mmap together are refused");

    /* Everything above is option validation, which holds everywhere. The rest
     * needs mapping to exist. Where it does not -- Windows today -- ls_open
     * says so with LS_ERR_MODE, and there is nothing here left to test. */
    s = open_mapped("m_probe", &err);
    if (!s && err == LS_ERR_MODE) {
        puts("  [SKIP] mapping is not supported on this platform");
        return 77;
    }
    if (s) ls_close(s, 0);

    /* A single-extent record. */
    s = open_mapped("m_one", &err);
    ok(s != NULL, "open a mapped store");
    if (!s) return 1;

    {
        double v[512];
        for (i = 0; i < 512; i++) v[i] = 1.0 + i;
        ok_rc(ls_write(s, "eri", 0, sizeof v, v), LS_OK,
              "ls_write still works on a mapped store, as a porting aid");
        ok_rc(ls_map(s, "eri", &addr, &len), LS_OK, "ls_map");
        ok(len == sizeof v, "  ... reports the logical length");
        p = addr;
        ok(p[0] == 1.0 && p[511] == 512.0, "  ... and the bytes are there");

        /* The access pattern the mode exists for: index it, do not call. */
        for (i = 0; i < 512; i++) p[i] += 0.5;
        ok_rc(ls_unmap(s, "eri"), LS_OK, "ls_unmap");

        memset(v, 0, sizeof v);
        ok_rc(ls_read(s, "eri", 0, sizeof v, v), LS_OK, "read back through the API");
        ok(v[0] == 1.5 && v[511] == 512.5, "  ... writes through the mapping persisted");
    }

    /* What is not available under a mapping, and says so rather than degrading. */
    {
        ls_req *rq = NULL;
        double v = 1.0;
        ok_rc(ls_aread(s, "eri", 0, sizeof v, &v, &rq), LS_ERR_MODE,
              "ls_aread on a mapped store is LS_ERR_MODE");
        ok_rc(ls_awrite(s, "eri", 0, sizeof v, &v, &rq), LS_ERR_MODE,
              "ls_awrite likewise");
        ok_rc(ls_accumulate(s, "eri", 0, sizeof v, &v, ls_add_f64, NULL), LS_ERR_MODE,
              "ls_accumulate likewise: p[i] += x is the caller's");
        ok_rc(ls_map(s, "absent", &addr, &len), LS_ERR_NOKEY, "mapping a missing key");
    }
    ls_close(s, 0);

    /* A mapped store must refuse the calls only an explicit store has, and an
     * explicit store must refuse ls_map. */
    ls_opts_default(&o);
    s = ls_open("m_expl", &o, &err);
    ok_rc(ls_map(s, "x", &addr, &len), LS_ERR_MODE, "ls_map on an explicit store is refused");
    ls_close(s, 0);

    /* THE case: a record spanning several extents, so the mapping is stitched.
     * Grown a little at a time, with other records interleaved so the extents
     * cannot be adjacent in the file. */
    s = open_mapped("m_many", &err);
    ok(s != NULL, "reopen mapped for the multi-extent case");
    if (s) {
        const size_t CH = 64 * 1024;          /* well above LS_ALIGN */
        const int NCH = 12;
        double *chunk = malloc(CH);
        double *whole = malloc((size_t)NCH * CH);
        size_t j, nper = CH / sizeof(double);
        int good = 1;

        for (i = 0; i < NCH; i++) {
            for (j = 0; j < nper; j++) chunk[j] = 1000.0 * i + (double)j;
            ls_write(s, "big", (uint64_t)i * CH, CH, chunk);
            /* an interleaved record, so "big" cannot grow contiguously */
            ls_write(s, "filler", (uint64_t)i * CH, CH, chunk);
        }
        ok_rc(ls_map(s, "big", &addr, &len), LS_OK, "map a record built from many extents");
        ok(len == (size_t)NCH * CH, "  ... with the whole logical length");
        p = addr;
        for (i = 0; i < NCH && good; i++)
            for (j = 0; j < nper; j++)
                if (p[(size_t)i * nper + j] != 1000.0 * i + (double)j) { good = 0; break; }
        ok(good, "  ... and every extent is stitched into the right place");

        /* write through the stitched mapping, read back through the API */
        p[0] = -1.0;
        p[(size_t)NCH * nper - 1] = -2.0;
        ok_rc(ls_unmap(s, "big"), LS_OK, "unmap the stitched record");
        ls_read(s, "big", 0, (size_t)NCH * CH, whole);
        ok(whole[0] == -1.0 && whole[(size_t)NCH * nper - 1] == -2.0,
           "  ... writes at both ends of the stitching persisted");

        /* closing with a mapping still live must not leak it */
        ls_map(s, "big", &addr, &len);
        ok_rc(ls_close(s, 0), LS_OK, "close unmaps whatever is still mapped");
        free(chunk);
        free(whole);
    }

    /* ls_unlink_now: anonymous scratch that is gone from the filesystem the
     * instant it exists, but still usable through the mapping. qp2's Davidson
     * and Cholesky work matrices are exactly this, so that a crash leaves
     * nothing behind. */
    {
        const char *d = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        char path[512];
        struct stat st;
        double *q;

        s = open_mapped("m_anon", &err);
        ok(s != NULL, "open a store for the anonymous-scratch pattern");
        if (s) {
            double vals[256];
            for (i = 0; i < 256; i++) vals[i] = 3.0 + i;
            ls_write(s, "w", 0, sizeof vals, vals);
            ok_rc(ls_map(s, "w", &addr, &len), LS_OK, "  ... map it");

            snprintf(path, sizeof path, "%s/m_anon.libspill", d);
            ok(stat(path, &st) == 0, "  ... the backing file exists");
            ok_rc(ls_unlink_now(s), LS_OK, "ls_unlink_now");
            ok(stat(path, &st) != 0, "  ... and the file is gone from the filesystem");

            q = addr;
            ok(q[0] == 3.0 && q[255] == 258.0, "  ... but the mapping still reads");
            q[0] = -5.0;
            ok(q[0] == -5.0, "  ... and still writes");

            ok_rc(ls_unlink_now(s), LS_OK, "a second ls_unlink_now is harmless");
            ok_rc(ls_close(s, 1), LS_OK, "close with keep=1 on an unlinked store");
            ok(stat(path, &st) != 0, "  ... leaves nothing behind either way");
        }
    }

    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
