/* The five requests §3a extracted from the surveys, as tests.
 *
 * §3a(1) LS_SHARED       7 codes   -- exercised across real processes, below
 * §3a(2) ls_append       3 codes   -- OpenMolcas's 2200 cursor-threaded sites
 * §3a(3) attributes      5 codes
 * §3a(4) ls_readv/writev 3 codes
 * §3a(5) tuple keys      3 codes   -- already answered by string keys; no code
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
        printf("  [FAIL] %-50s got %s, wanted %s\n", what,
               ls_strerror(rc, a, sizeof a), ls_strerror(want, b, sizeof b));
    } else {
        printf("  [PASS] %s\n", what);
    }
}

static ls_store *fresh(const char *name)
{
    ls_opts o;
    int err = 0;
    ls_store *s;
    ls_opts_default(&o);
    s = ls_open(name, &o, &err);
    if (!s) { printf("  [FAIL] open %s: %d\n", name, err); exit(1); }
    return s;
}

/* ------------------------------------------------------------- §3a(2) append */

#define NAPP_THREAD 6
#define NAPP_EACH   200

struct app { ls_store *s; int id; uint64_t off[NAPP_EACH]; int bad; };

static void *appender(void *p)
{
    struct app *a = p;
    int i;
    for (i = 0; i < NAPP_EACH; i++) {
        double v = a->id * 1000.0 + i;
        if (ls_append(a->s, "amps", sizeof v, &v, &a->off[i]) != LS_OK) { a->bad = 1; return NULL; }
    }
    return NULL;
}

static int cmp_u64(const void *x, const void *y)
{
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void t_append(void)
{
    ls_store *s = fresh("t_app");
    uint64_t off = 99, sz = 0;
    double v = 1.5, back = 0;

    ok_rc(ls_append(s, "k", sizeof v, &v, &off), LS_OK, "append to a new key");
    ok(off == 0, "  ... reports offset 0");
    v = 2.5;
    ok_rc(ls_append(s, "k", sizeof v, &v, &off), LS_OK, "append again");
    ok(off == sizeof v, "  ... reports the previous end");
    ls_size(s, "k", &sz);
    ok(sz == 2 * sizeof v, "the record grew by exactly what was appended");
    ls_read(s, "k", sizeof v, sizeof back, &back);
    ok(back == 2.5, "the second append landed where it said it did");

    /* The reason this is a primitive rather than ls_size plus ls_write: with
     * six threads appending, every offset must still be distinct. */
    {
        pthread_t th[NAPP_THREAD];
        struct app ar[NAPP_THREAD];
        uint64_t all[NAPP_THREAD * NAPP_EACH];
        int i, j, k = 0, bad = 0, dup = 0;

        for (i = 0; i < NAPP_THREAD; i++) {
            ar[i].s = s; ar[i].id = i; ar[i].bad = 0;
            pthread_create(&th[i], NULL, appender, &ar[i]);
        }
        for (i = 0; i < NAPP_THREAD; i++) {
            pthread_join(th[i], NULL);
            if (ar[i].bad) bad = 1;
            for (j = 0; j < NAPP_EACH; j++) all[k++] = ar[i].off[j];
        }
        ok(!bad, "6 threads x 200 concurrent appends all succeed");
        qsort(all, (size_t)k, sizeof all[0], cmp_u64);
        for (i = 1; i < k; i++) if (all[i] == all[i-1]) dup = 1;
        ok(!dup, "  ... and no two of the 1200 offsets collide");
        for (i = 1; i < k; i++) if (all[i] != all[i-1] + sizeof(double)) dup = 1;
        ok(!dup, "  ... and they tile the record with no gaps");
    }
    ls_close(s, 0);
}

/* ------------------------------------------------------- §3a(4) vectored I/O */

static void t_vectored(void)
{
    ls_store *s = fresh("t_vec");
    double base[64], back[64], patch[8];
    ls_seg seg[8];
    int i, good = 1;

    for (i = 0; i < 64; i++) base[i] = i;
    ls_write(s, "vec", 0, sizeof base, base);

    /* Serenity's case: update every eighth element in place, without reading
     * the vector back and without buffering the whole of it. */
    for (i = 0; i < 8; i++) {
        patch[i] = -1.0 - i;
        seg[i].off = (uint64_t)(i * 8) * sizeof(double);
        seg[i].len = sizeof(double);
        seg[i].buf = &patch[i];
    }
    ok_rc(ls_writev(s, "vec", seg, 8), LS_OK, "strided in-place update via writev");

    ls_read(s, "vec", 0, sizeof back, back);
    for (i = 0; i < 64; i++) {
        double want = (i % 8 == 0) ? -1.0 - (i / 8) : (double)i;
        if (back[i] != want) good = 0;
    }
    ok(good, "  ... touches exactly the strided elements and nothing else");

    memset(back, 0, sizeof back);
    for (i = 0; i < 8; i++) seg[i].buf = &back[i];
    ok_rc(ls_readv(s, "vec", seg, 8), LS_OK, "gather the same elements back");
    good = 1;
    for (i = 0; i < 8; i++) if (back[i] != -1.0 - i) good = 0;
    ok(good, "  ... and they are what was written");

    seg[0].off = 0; seg[0].len = sizeof base * 2; seg[0].buf = back;
    ok_rc(ls_readv(s, "vec", seg, 1), LS_ERR_RANGE, "readv past the end is RANGE");
    ok_rc(ls_readv(s, "absent", seg, 1), LS_ERR_NOKEY, "readv of a missing key is NOKEY");
    ok_rc(ls_readv(s, "vec", NULL, 0), LS_OK, "an empty segment list is a no-op");

    ls_close(s, 0);
}

/* --------------------------------------------------------- §3a(3) attributes */

static void t_attrs(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0;
    char blob[LS_ATTR_MAX + 8], out[LS_ATTR_MAX + 8];
    size_t n;
    double v = 1.0;

    s = fresh("t_attr");
    ls_write(s, "eri", 0, sizeof v, &v);

    ok_rc(ls_set_attr(s, "eri", "shape=(8,8,8,8) f8", 18), LS_OK, "set an attribute");
    n = sizeof out;
    ok_rc(ls_get_attr(s, "eri", out, &n), LS_OK, "get it back");
    ok(n == 18 && memcmp(out, "shape=(8,8,8,8) f8", 18) == 0, "  ... byte-exact");

    n = 0;
    ok_rc(ls_get_attr(s, "eri", NULL, &n), LS_OK, "a NULL buffer is an enquiry");
    ok(n == 18, "  ... and reports the length");

    n = 4;
    ok_rc(ls_get_attr(s, "eri", out, &n), LS_ERR_RANGE, "a short buffer is RANGE");
    ok(n == 18, "  ... and says how much was needed");

    memset(blob, 'x', sizeof blob);
    ok_rc(ls_set_attr(s, "eri", blob, LS_ATTR_MAX), LS_OK, "an attribute at the cap fits");
    ok_rc(ls_set_attr(s, "eri", blob, LS_ATTR_MAX + 1), LS_ERR_INVAL,
          "one byte over the cap is refused");
    ok_rc(ls_get_attr(s, "nope", out, &n), LS_ERR_NOKEY, "attributes of a missing key");

    /* An attribute that did not survive a reopen would be useless to the codes
     * that asked for it, all of which want it for restart-validity checks. */
    ls_set_attr(s, "eri", "f8/(4,4)", 8);
    ls_close(s, 1);
    ls_opts_default(&o);
    s = ls_open("t_attr", &o, &err);
    ok(s != NULL, "reopen a store with attributes");
    if (s) {
        n = sizeof out;
        ok_rc(ls_get_attr(s, "eri", out, &n), LS_OK, "the attribute survived the round trip");
        ok(n == 8 && memcmp(out, "f8/(4,4)", 8) == 0, "  ... byte-exact");
        ls_close(s, 0);
    }
}

/* ------------------------------------------------------------ §3a(1) LS_SHARED
 * Across real processes. A single-process test of a cross-process mode would
 * demonstrate nothing. */

#define NPROC 4
#define NPER  4096

static void t_shared(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0, i, bad = 0;
    pid_t kids[NPROC];
    double *chk;

    /* Step 1: one process lays the store out and closes it, keeping the file. */
    s = fresh("t_shared");
    ok_rc(ls_reserve(s, "psi", (uint64_t)NPROC * NPER * sizeof(double)), LS_OK,
          "the creating process reserves the shared record");
    ok_rc(ls_close(s, 1), LS_OK, "and closes it, keeping the layout");

    /* Step 2: the caller's barrier -- here, fork. Each child writes only its
     * own disjoint range. */
    for (i = 0; i < NPROC; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            ls_opts co;
            ls_store *cs;
            int cerr = 0, j, rc;
            double *buf = malloc(NPER * sizeof *buf);
            ls_opts_default(&co);
            co.parallel = LS_SHARED;
            cs = ls_open("t_shared", &co, &cerr);
            if (!cs) _exit(10);
            for (j = 0; j < NPER; j++) buf[j] = i * 100000.0 + j;
            rc = ls_write(cs, "psi", (uint64_t)i * NPER * sizeof(double),
                          NPER * sizeof(double), buf);
            if (rc != LS_OK) _exit(11);
            if (ls_close(cs, 1) != LS_OK) _exit(12);
            free(buf);
            _exit(0);
        }
    }
    for (i = 0; i < NPROC; i++) {
        int st = 0;
        waitpid(kids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    ok(!bad, "4 processes each write a disjoint range of one shared key");

    /* Step 3: after the caller's synchronisation, read it all back. */
    ls_opts_default(&o);
    s = ls_open("t_shared", &o, &err);
    ok(s != NULL, "the store reopens after the shared writes");
    if (s) {
        int good = 1;
        chk = malloc((size_t)NPROC * NPER * sizeof *chk);
        if (ls_read(s, "psi", 0, (size_t)NPROC * NPER * sizeof(double), chk) != LS_OK) good = 0;
        for (i = 0; good && i < NPROC * NPER; i++)
            if (chk[i] != (i / NPER) * 100000.0 + (i % NPER)) good = 0;
        ok(good, "  ... and every range holds what its process wrote");
        free(chk);
        ls_close(s, 0);
    }

    /* The refusals that keep the layout frozen. */
    ls_opts_default(&o);
    o.parallel = LS_SHARED;
    o.memory_budget = 1 << 20;
    s = ls_open("t_shared_x", &o, &err);
    ok(s == NULL && err == LS_ERR_INVAL,
       "LS_SHARED with a memory budget is refused: other processes could not see it");

    ls_opts_default(&o);
    o.parallel = LS_SHARED;
    s = ls_open("t_never_made", &o, &err);
    ok(s == NULL && err == -ENOENT, "LS_SHARED on a store nobody laid out is ENOENT");

    /* rebuild one to test the frozen-layout refusals against */
    s = fresh("t_shared2");
    ls_reserve(s, "psi", 4096);
    ls_close(s, 1);
    ls_opts_default(&o);
    o.parallel = LS_SHARED;
    s = ls_open("t_shared2", &o, &err);
    ok(s != NULL, "reopen a laid-out store as LS_SHARED");
    if (s) {
        double v = 1.0;
        ok_rc(ls_write(s, "psi", 0, sizeof v, &v), LS_OK, "a write inside the layout is fine");
        ok_rc(ls_write(s, "psi", 4096, sizeof v, &v), LS_ERR_MODE, "a write past the end is MODE");
        ok_rc(ls_write(s, "new", 0, sizeof v, &v), LS_ERR_MODE, "creating a key is MODE");
        ok_rc(ls_reserve(s, "psi", 8192), LS_ERR_MODE, "reserve is MODE");
        ok_rc(ls_erase(s, "psi"), LS_ERR_MODE, "erase is MODE");
        ok_rc(ls_append(s, "psi", sizeof v, &v, NULL), LS_ERR_MODE, "append is MODE");
        ok_rc(ls_set_attr(s, "psi", "x", 1), LS_ERR_MODE, "set_attr is MODE");
        ls_close(s, 0);
    }
    /* LS_SHARED never unlinks, so clean up through an ordinary store */
    ls_opts_default(&o);
    s = ls_open("t_shared2", &o, &err);
    if (s) ls_close(s, 0);
}

int main(void)
{
    puts("libspill: the requests §3a extracted from the surveys");
    t_append();
    t_vectored();
    t_attrs();
    t_shared();
    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
