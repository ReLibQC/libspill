/* SPDX-License-Identifier: BSD-3-Clause */
/* exact_name, ls_store_exists, and the ls_opts versioning that adding a field
 * finally exercised.
 *
 * The option exists because some callers inspect the filesystem libspill writes
 * to: OpenMolcas tests for its RunFile with f_inquire on a bare 8-character
 * name (gxwrrun.F90:58 and four others), and Psi4's PSIOManager builds and
 * unlinks psi.<pid>.<namespace>.<filenum> paths in psiclean(). With the
 * .libspill suffix appended those checks answer "no" forever.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
        printf("  [FAIL] %-46s got %s, wanted %s\n", what,
               ls_strerror(rc, a, sizeof a), ls_strerror(want, b, sizeof b));
    } else {
        printf("  [PASS] %s\n", what);
    }
}

static const char *dir(void)
{
    const char *d = getenv("TMPDIR");
    return (d && *d) ? d : "/tmp";
}

static int file_there(const char *rel)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir(), rel);
    return stat(p, &st) == 0;
}

int main(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0, found = 0;
    double v = 2.5, back = 0;

    puts("libspill exact_name");

    /* Default: the suffix is applied, as it always was. */
    ls_opts_default(&o);
    s = ls_open("plain", &o, &err);
    ok(s != NULL, "a default store opens");
    ls_close(s, 1);
    ok(file_there("plain.libspill"), "  ... and lands at <name>.libspill");
    ok(!file_there("plain"), "  ... not at the bare name");

    /* exact_name: the file carries the name the caller asked for. */
    ls_opts_default(&o);
    o.exact_name = 1;
    s = ls_open("RUNFILE", &o, &err);
    ok(s != NULL && err == LS_OK, "an exact-named store opens");
    ok_rc(ls_write(s, "k", 0, sizeof v, &v), LS_OK, "  ... and is writable");
    ok(file_there("RUNFILE"), "  ... at exactly <dir>/RUNFILE");
    ok(!file_there("RUNFILE.libspill"), "  ... with no suffix anywhere");
    ok_rc(ls_close(s, 1), LS_OK, "  ... closes keeping the file");

    /* It is a real store, not just a file with the right name. */
    ls_opts_default(&o);
    o.exact_name = 1;
    s = ls_open("RUNFILE", &o, &err);
    ok_rc(ls_read(s, "k", 0, sizeof back, &back), LS_OK, "reopens and reads back");
    ok(back == v, "  ... byte-exact");
    ok_rc(ls_close(s, 0), LS_OK, "closes with keep=0");
    ok(!file_there("RUNFILE"), "  ... and the file is gone");

    /* This is what OpenMolcas's f_inquire sites actually need. */
    ls_opts_default(&o);
    o.exact_name = 1;
    ok_rc(ls_store_exists("RUNFILE", &o, &found), LS_OK, "ls_store_exists on an absent store");
    ok(!found, "  ... reports absent");
    s = ls_open("RUNFILE", &o, &err);
    ls_write(s, "k", 0, sizeof v, &v);
    ls_close(s, 1);
    ok_rc(ls_store_exists("RUNFILE", &o, &found), LS_OK, "ls_store_exists after creation");
    ok(found, "  ... reports present");
    /* and it answers for suffixed stores too, without the caller knowing the rule */
    ls_opts_default(&o);
    ok_rc(ls_store_exists("plain", &o, &found), LS_OK, "ls_store_exists on a suffixed store");
    ok(found, "  ... also reports present");

    /* Refused combination: LS_PER_RANK folds a rank in, exact_name takes the
     * name verbatim; both at once is two different names. */
    ls_opts_default(&o);
    o.exact_name = 1;
    o.parallel = LS_PER_RANK;
    ok(ls_open("both", &o, &err) == NULL && err == LS_ERR_INVAL,
       "exact_name with LS_PER_RANK is refused");

    /* -------- the versioning §4b promised and never exercised --------
     * Simulate a caller compiled against the version-1 header: a shorter
     * struct, with no exact_name field at all. The library must accept it and
     * must not read past its end. */
    {
        struct opts_v1 {                   /* ls_opts exactly as version 1 was */
            uint32_t    version;
            ls_backend  backend;
            ls_mode     mode;
            ls_parallel parallel;
            int         rank;
            size_t      memory_budget;
            const char *dir;
            int         direct_io;
            ls_log      log;
            void       *log_ctx;
        } old;
        memset(&old, 0, sizeof old);
        old.version = 1u;
        old.backend = LS_POSIX;
        old.mode = LS_EXPLICIT;
        old.parallel = LS_LOCAL;
        old.rank = -1;

        s = ls_open("oldcaller", (const ls_opts *)&old, &err);
        ok(s != NULL && err == LS_OK, "a version-1 opts struct is still accepted");
        if (s) {
            ok_rc(ls_write(s, "k", 0, sizeof v, &v), LS_OK, "  ... and the store works");
            ls_close(s, 1);
            ok(file_there("oldcaller.libspill"),
               "  ... defaulting exact_name to 0, as version 1 had no such field");
            ls_opts_default(&o);
            s = ls_open("oldcaller", &o, &err);
            if (s) ls_close(s, 0);
        }

        old.version = 99u;
        ok(ls_open("bad", (const ls_opts *)&old, &err) == NULL && err == LS_ERR_INVAL,
           "an unknown opts version is refused");
        old.version = 0u;
        ok(ls_open("bad", (const ls_opts *)&old, &err) == NULL && err == LS_ERR_INVAL,
           "a zero opts version is refused");
    }

    /* ls_opts_init writes only what the caller's version declares. */
    {
        ls_opts probe;
        memset(&probe, 0xAB, sizeof probe);
        ls_opts_init(&probe, 1u);
        ok(probe.version == 1u, "ls_opts_init(o, 1) records version 1");
        ok(*((unsigned char *)&probe + offsetof(ls_opts, exact_name)) == 0xAB,
           "  ... and leaves bytes past version 1 untouched");
    }

    { ls_opts_default(&o); s = ls_open("plain", &o, &err); if (s) ls_close(s, 0); }

    printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
