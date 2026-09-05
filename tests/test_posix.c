/* Correctness suite for the POSIX backend: the §4b contract, the memory tier,
 * the async layer, and the concurrency §5a promises. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libscratch.h"

static int fails = 0, ntest = 0;

static void ok(int cond, const char *what)
{
    ntest++;
    if (!cond) { fails++; printf("  [FAIL] %s\n", what); }
    else         printf("  [PASS] %s\n", what);
}

static void ok_rc(int rc, int want, const char *what)
{
    char a[LS_ERRBUF_MIN], b[LS_ERRBUF_MIN];
    ntest++;
    if (rc != want) {
        fails++;
        printf("  [FAIL] %-52s got %s, wanted %s\n", what,
               ls_strerror(rc, a, sizeof a), ls_strerror(want, b, sizeof b));
    } else {
        printf("  [PASS] %s\n", what);
    }
}

static ls_store *fresh(const char *name, size_t budget)
{
    ls_opts o;
    int err = 0;
    ls_store *s;
    ls_opts_default(&o);
    o.memory_budget = budget;
    s = ls_open(name, &o, &err);
    if (!s) { printf("  [FAIL] open %s: %d\n", name, err); exit(1); }
    return s;
}

/* ------------------------------------------------------------------------- */

static void t_roundtrip(void)
{
    ls_store *s = fresh("t_rt", 0);
    double out[512], in[512];
    uint64_t sz = 0;
    int i, found = 0;

    for (i = 0; i < 512; i++) out[i] = 1.0 + i;

    ok_rc(ls_write(s, "amp", 0, sizeof out, out), LS_OK, "write a record");
    memset(in, 0, sizeof in);
    ok_rc(ls_read(s, "amp", 0, sizeof in, in), LS_OK, "read it back");
    ok(memcmp(in, out, sizeof out) == 0, "bytes round-trip exactly");

    ok_rc(ls_size(s, "amp", &sz), LS_OK, "ls_size succeeds");
    ok(sz == sizeof out, "ls_size reports the logical size");
    ok_rc(ls_exists(s, "amp", &found), LS_OK, "ls_exists succeeds");
    ok(found == 1, "ls_exists finds the record");

    /* offset write, then a partial read from the middle */
    ok_rc(ls_write(s, "amp", 256 * sizeof(double), 8, out), LS_OK, "write at an offset");
    ok_rc(ls_read(s, "amp", 256 * sizeof(double), 8, in), LS_OK, "read at an offset");
    ok(in[0] == out[0], "offset read returns the offset write");

    ls_close(s, 0);
}

static void t_errors(void)
{
    ls_store *s = fresh("t_err", 0);
    char big[LS_KEY_MAX + 8];
    double v = 1.0;
    uint64_t sz;

    ok_rc(ls_read(s, "absent", 0, 8, &v), LS_ERR_NOKEY, "read of a missing key");
    ok_rc(ls_size(s, "absent", &sz), LS_ERR_NOKEY, "size of a missing key");
    ok_rc(ls_erase(s, "absent"), LS_ERR_NOKEY, "erase of a missing key");

    ok_rc(ls_write(s, "k", 0, 8, &v), LS_OK, "write one element");
    ok_rc(ls_read(s, "k", 0, 16, &v), LS_ERR_RANGE, "read past the end is RANGE");
    ok_rc(ls_read(s, "k", 8, 8, &v), LS_ERR_RANGE, "read wholly past the end is RANGE");

    ok_rc(ls_write(s, NULL, 0, 8, &v), LS_ERR_INVAL, "NULL key is INVAL");
    ok_rc(ls_write(s, "", 0, 8, &v), LS_ERR_INVAL, "empty key is INVAL");
    memset(big, 'x', sizeof big); big[sizeof big - 1] = '\0';
    ok_rc(ls_write(s, big, 0, 8, &v), LS_ERR_INVAL, "over-long key is INVAL");
    ok_rc(ls_write(s, "k", 0, 8, NULL), LS_ERR_INVAL, "NULL buffer is INVAL");

    {   /* §4b: the description must be usable for both error ranges */
        char b[LS_ERRBUF_MIN];
        ok(strstr(ls_strerror(LS_ERR_NOKEY, b, sizeof b), "key") != NULL,
           "ls_strerror describes a libscratch code");
        ok(strlen(ls_strerror(-ENOSPC, b, sizeof b)) > 0,
           "ls_strerror describes a negated errno");
    }
    ls_close(s, 0);
}

static void t_options(void)
{
    ls_opts o;
    int err = 0;
    ls_store *s;

    ls_opts_default(&o);
    o.backend = LS_HDF5;
    s = ls_open("t_opt", &o, &err);
    ok(s == NULL, "LS_HDF5 store is refused");
    ok_rc(err, LS_ERR_BACKEND, "  ... with LS_ERR_BACKEND");

    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    s = ls_open("t_opt", &o, &err);
    ok(s == NULL, "LS_MAPPED store is refused in this build");
    ok_rc(err, LS_ERR_MODE, "  ... with LS_ERR_MODE");

    /* the combination that is wrong in principle outranks the one that is
     * merely not built yet, so this stays stable when LS_MAPPED lands */
    ls_opts_default(&o);
    o.mode = LS_MAPPED;
    o.memory_budget = 1 << 20;
    s = ls_open("t_opt", &o, &err);
    ok(s == NULL, "LS_MAPPED with a memory budget is refused");
    ok_rc(err, LS_ERR_INVAL, "  ... with LS_ERR_INVAL, not LS_ERR_MODE");

    ls_opts_default(&o);
    o.version = 999;
    s = ls_open("t_opt", &o, &err);
    ok(s == NULL && err == LS_ERR_INVAL, "an unknown ls_opts version is refused");
}

static void t_toc(void)
{
    ls_store *s = fresh("t_toc", 0);
    double v = 2.5;
    char **k = NULL;
    size_t n = 0, i;
    int seen = 0;

    ls_write(s, "alpha", 0, 8, &v);
    ls_write(s, "beta",  0, 8, &v);
    ls_write(s, "gamma", 0, 8, &v);

    ok_rc(ls_keys(s, &k, &n), LS_OK, "ls_keys succeeds");
    ok(n == 3, "ls_keys returns every key");
    for (i = 0; i < n; i++) if (strcmp(k[i], "beta") == 0) seen = 1;
    ok(seen, "ls_keys contains a known key");

    /* the snapshot must survive mutation of the store -- that is why §4b makes
     * it a copy rather than a view */
    ls_erase(s, "beta");
    seen = 0;
    for (i = 0; i < n; i++) if (strcmp(k[i], "beta") == 0) seen = 1;
    ok(seen, "the snapshot still reads after the key is erased");
    ls_keys_free(k, n);

    ok_rc(ls_keys(s, &k, &n), LS_OK, "ls_keys after erase");
    ok(n == 2, "erase removed the key from the table");
    ls_keys_free(k, n);
    ls_close(s, 0);
}

static void t_gap_and_reserve(void)
{
    ls_store *s = fresh("t_gap", 0);
    double v = 7.0, back[4];
    uint64_t sz = 0;
    int i, zeroed = 1;

    /* §4b: a write past the end leaves the gap zero-filled */
    ok_rc(ls_write(s, "g", 24, 8, &v), LS_OK, "write past the end");
    ok_rc(ls_size(s, "g", &sz), LS_OK, "size after a gap write");
    ok(sz == 32, "the record extends to cover the write");
    ok_rc(ls_read(s, "g", 0, 32, back), LS_OK, "read across the gap");
    for (i = 0; i < 3; i++) if (back[i] != 0.0) zeroed = 0;
    ok(zeroed, "the gap reads as zero");
    ok(back[3] == 7.0, "the written element survives");

    ok_rc(ls_reserve(s, "r", 1 << 20), LS_OK, "reserve a megabyte");
    ok_rc(ls_size(s, "r", &sz), LS_OK, "size of a reserved record");
    ok(sz == (1 << 20), "reserve sets the logical size");
    ok_rc(ls_read(s, "r", 4096, 8, back), LS_OK, "read reserved space");
    ok(back[0] == 0.0, "reserved space reads as zero");
    ok_rc(ls_reserve(s, "r", 4096), LS_OK, "a smaller reserve is accepted");
    ls_size(s, "r", &sz);
    ok(sz == (1 << 20), "reserve never shrinks");

    ls_close(s, 0);
}

static void t_memory_tier(void)
{
    const size_t N = 1 << 16;                    /* 512 KiB of doubles */
    double *buf = malloc(N * sizeof *buf), *back = malloc(N * sizeof *back);
    ls_store *s;
    struct stat st;
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char path[512];
    size_t i;

    for (i = 0; i < N; i++) buf[i] = (double)i;

    /* fits: §5.2 says the same call path should not touch disk at all */
    s = fresh("t_mem", 8u << 20);
    ls_write(s, "a", 0, N * sizeof *buf, buf);
    memset(back, 0, N * sizeof *back);
    ok_rc(ls_read(s, "a", 0, N * sizeof *back, back), LS_OK, "read from the memory tier");
    ok(memcmp(buf, back, N * sizeof *buf) == 0, "memory tier round-trips exactly");
    snprintf(path, sizeof path, "%s/t_mem.libscratch", dir);
    ok(stat(path, &st) == 0 && st.st_size == 4096,
       "nothing was written to disk while under budget");
    ls_close(s, 0);

    /* does not fit: must spill and stay correct */
    s = fresh("t_mem2", 64 << 10);
    ls_write(s, "a", 0, N * sizeof *buf, buf);
    ls_write(s, "b", 0, N * sizeof *buf, buf);
    memset(back, 0, N * sizeof *back);
    ok_rc(ls_read(s, "a", 0, N * sizeof *back, back), LS_OK, "read a spilled record");
    ok(memcmp(buf, back, N * sizeof *buf) == 0, "spilled record round-trips exactly");
    snprintf(path, sizeof path, "%s/t_mem2.libscratch", dir);
    ok(stat(path, &st) == 0 && st.st_size > 4096, "the spill reached disk");
    ls_close(s, 0);

    free(buf);
    free(back);
}

static void t_async(void)
{
    const size_t N = 1 << 14;
    double *a = malloc(N * sizeof *a), *b = malloc(N * sizeof *b);
    ls_store *s = fresh("t_async", 0);
    ls_req *rq = NULL, *rq2 = NULL;
    size_t i;
    int done = 0;

    for (i = 0; i < N; i++) a[i] = (double)(i * 3u);

    ok_rc(ls_awrite(s, "x", 0, N * sizeof *a, a, &rq), LS_OK, "ls_awrite accepted");
    ok_rc(ls_wait(rq), LS_OK, "ls_wait reports success");

    memset(b, 0, N * sizeof *b);
    ok_rc(ls_aread(s, "x", 0, N * sizeof *b, b, &rq2), LS_OK, "ls_aread accepted");
    ok_rc(ls_test(rq2, &done), LS_OK, "ls_test succeeds");
    ok_rc(ls_wait(rq2), LS_OK, "ls_wait on the read");
    ok(memcmp(a, b, N * sizeof *a) == 0, "async round-trips exactly");

    /* a failing operation must report through ls_wait, not ls_aread */
    ok_rc(ls_aread(s, "nope", 0, 8, b, &rq), LS_OK, "async read of a missing key is queued");
    ok_rc(ls_wait(rq), LS_ERR_NOKEY, "  ... and fails at ls_wait");

    /* many in flight at once */
    {
        ls_req *rs[32];
        int bad = 0;
        for (i = 0; i < 32; i++) {
            char k[16];
            sprintf(k, "m%zu", i);
            if (ls_awrite(s, k, 0, N * sizeof *a, a, &rs[i]) != LS_OK) bad = 1;
        }
        for (i = 0; i < 32; i++) if (ls_wait(rs[i]) != LS_OK) bad = 1;
        ok(!bad, "32 requests in flight all complete");
    }

    /* closing with a request outstanding must drain, not leak or hang */
    ls_awrite(s, "drain", 0, N * sizeof *a, a, &rq);
    ok_rc(ls_close(s, 0), LS_OK, "ls_close drains an unwaited request");

    free(a);
    free(b);
}

/* §5a: concurrent operations on distinct keys are safe. This is the guarantee
 * that needed the table-of-contents lock §4b adds -- every thread here creates
 * a key, which mutates the table the others are reading. */
#define NTHREAD 8
#define NPERTHREAD 64

struct arg { ls_store *s; int id; int bad; };

static void *hammer(void *p)
{
    struct arg *a = p;
    double buf[128], back[128];
    int i, j;

    for (i = 0; i < NPERTHREAD; i++) {
        char k[32];
        sprintf(k, "t%d_%d", a->id, i);
        for (j = 0; j < 128; j++) buf[j] = a->id * 1000.0 + i;
        if (ls_write(a->s, k, 0, sizeof buf, buf) != LS_OK) { a->bad = 1; return NULL; }
        memset(back, 0, sizeof back);
        if (ls_read(a->s, k, 0, sizeof back, back) != LS_OK) { a->bad = 1; return NULL; }
        if (memcmp(buf, back, sizeof buf) != 0) { a->bad = 2; return NULL; }
        if (ls_size(a->s, k, &(uint64_t){0}) != LS_OK) { a->bad = 3; return NULL; }
    }
    return NULL;
}

static void t_concurrent(void)
{
    ls_store *s = fresh("t_conc", 0);
    pthread_t th[NTHREAD];
    struct arg ar[NTHREAD];
    int i, bad = 0;
    char **k = NULL;
    size_t n = 0;

    for (i = 0; i < NTHREAD; i++) {
        ar[i].s = s; ar[i].id = i; ar[i].bad = 0;
        pthread_create(&th[i], NULL, hammer, &ar[i]);
    }
    for (i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        if (ar[i].bad) bad = ar[i].bad;
    }
    ok(!bad, "8 threads x 64 distinct keys: no error, no corruption");
    ls_keys(s, &k, &n);
    ok(n == NTHREAD * NPERTHREAD, "every key reached the table of contents");
    ls_keys_free(k, n);
    ls_close(s, 0);
}

static void t_persist(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0;
    double out[256], in[256];
    uint64_t sz = 0;
    int i;

    for (i = 0; i < 256; i++) out[i] = 3.5 * i;

    ls_opts_default(&o);
    o.memory_budget = 8u << 20;              /* resident: close must spill it */
    s = ls_open("t_keep", &o, &err);
    ls_write(s, "kept", 0, sizeof out, out);
    ls_write(s, "also", 0, 64, out);
    ok_rc(ls_close(s, 1), LS_OK, "close with keep=1");

    ls_opts_default(&o);
    s = ls_open("t_keep", &o, &err);
    ok(s != NULL, "reopen a kept store");
    if (s) {
        ok_rc(ls_size(s, "kept", &sz), LS_OK, "the table of contents survived");
        ok(sz == sizeof out, "  ... with the right size");
        memset(in, 0, sizeof in);
        ok_rc(ls_read(s, "kept", 0, sizeof in, in), LS_OK, "read from the reopened store");
        ok(memcmp(in, out, sizeof out) == 0, "the bytes survived the round trip");
        ls_close(s, 0);
    }

    /* a file that is not one of ours must not be silently truncated */
    {
        const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        char path[512];
        FILE *f;
        snprintf(path, sizeof path, "%s/t_junk.libscratch", dir);
        f = fopen(path, "wb");
        if (f) { char junk[8192]; memset(junk, 'Z', sizeof junk);
                 fwrite(junk, 1, sizeof junk, f); fclose(f); }
        ls_opts_default(&o);
        s = ls_open("t_junk", &o, &err);
        ok(s == NULL && err == LS_ERR_CORRUPT, "a foreign file is refused, not clobbered");
        unlink(path);
    }
}

int main(void)
{
    puts("libscratch POSIX backend");
    t_roundtrip();
    t_errors();
    t_options();
    t_toc();
    t_gap_and_reserve();
    t_memory_tier();
    t_async();
    t_concurrent();
    t_persist();
    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
