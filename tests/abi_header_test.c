/* SPDX-License-Identifier: BSD-3-Clause */
/* Checks the assumptions DESIGN.md §4b makes about the ABI, so that they fail
 * loudly on a platform where they do not hold rather than silently aliasing an
 * OS error onto one of ours.
 *
 *   cc -std=c99 -Wall -Wextra -pedantic -Iinclude tests/abi_header_test.c -o abi
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "libspill.h"
#include "libspill.h"   /* include guard must make this a no-op */

static int fails = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  [%s] %-52s%s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) fails++;
}

int main(void)
{
    char detail[256];
    int  i, highest = 0;

    puts("libspill ABI assumptions");

    /* 1. The two error ranges must not overlap. Ours start at -1000; the errno
     *    range is everything above. Find the largest errno this platform
     *    actually defines by asking the C library which codes it can name. */
    for (i = 1; i < 4096; i++) {
        const char *m = strerror(i);       /* single-threaded here; and this
                                              avoids strerror_r's two flavours */
        if (m && strncmp(m, "Unknown error", 13) != 0) highest = i;
    }
    sprintf(detail, "highest named errno = %d", highest);
    check(highest > 0 && highest < 1000,
          "errno range stays clear of -1000", detail);

    /* 2. Every libspill code sits in its own range. */
    check(LS_OK == 0, "LS_OK is zero", "");
    check(LS_ERR_NOKEY   <= -1000 && LS_ERR_RANGE   <= -1000 &&
          LS_ERR_INVAL   <= -1000 && LS_ERR_MODE    <= -1000 &&
          LS_ERR_BACKEND <= -1000 && LS_ERR_BUSY    <= -1000 &&
          LS_ERR_CORRUPT <= -1000,
          "all LS_ERR_* are <= -1000", "");

    /* 3. The errnos §4b promises to pass through unchanged must be nameable and
     *    must not collide with anything of ours. */
#ifdef EDQUOT
    sprintf(detail, "ENOSPC=%d EIO=%d EDQUOT=%d", ENOSPC, EIO, EDQUOT);
    check(ENOSPC < 1000 && EIO < 1000 && EDQUOT < 1000,
          "passed-through errnos are in range", detail);
#else
    /* EDQUOT is POSIX, not C: the MSVC CRT has no such errno. The property
     * being checked is about the ones that exist. */
    sprintf(detail, "ENOSPC=%d EIO=%d (no EDQUOT on this platform)", ENOSPC, EIO);
    check(ENOSPC < 1000 && EIO < 1000, "passed-through errnos are in range", detail);
#endif

    /* 4. version is first in ls_opts, which is what lets the struct grow
     *    without breaking a caller built against an older header. */
    check(offsetof(ls_opts, version) == 0, "ls_opts.version is the first member", "");

    /* 5. A key plus its NUL must fit whatever the table of contents allots. */
    check(LS_KEY_MAX >= 80,
          "LS_KEY_MAX admits Psi4's 80-char keys", "");

    printf("%s\n", fails ? "FAILED" : "all assumptions hold");
    return fails != 0;
}
