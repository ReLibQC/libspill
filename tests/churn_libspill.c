/* SPDX-License-Identifier: BSD-3-Clause */
/* The other half of tests/hdf5_churn_varsize.py, run against this backend.
 *
 * Same protocol: 8 records, 60 cycles, each cycle erasing one record and
 * recreating it at a DIFFERENT random size drawn from the same range, then
 * comparing file size against live bytes. HDF5 measured x1.61 (default), x1.75
 * (fsm) and x1.37 (page) on this protocol, which is what §7b calls
 * disqualifying for a scratch heap. Best-fit with coalescing is the answer;
 * this is the number that says whether it worked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libspill.h"

#define NREC   8
#define CYCLES 60
#define LO     (1 << 14)
#define HI     (1 << 18)

static unsigned long rs = 1;
static unsigned long nextr(void) { rs = rs * 6364136223846793005UL + 1442695040888963407UL;
                                   return (rs >> 33); }
static size_t pick(void) { return (size_t)(LO + nextr() % (HI - LO)); }

int main(void)
{
    ls_opts o;
    ls_store *s;
    int err = 0, i, c;
    size_t sizes[NREC];
    double *buf = malloc((size_t)HI * sizeof *buf);
    char key[32];
    unsigned long long live = 0, fsz = 0;
    struct stat st;
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char path[512];

    if (!buf) return 77;
    for (i = 0; i < HI; i++) buf[i] = (double)i;

    ls_opts_default(&o);
    o.memory_budget = 0;                    /* disk path: this is a space test */
    s = ls_open("churn", &o, &err);
    if (!s) { fprintf(stderr, "open failed: %d\n", err); return 1; }

    for (i = 0; i < NREC; i++) {
        sizes[i] = pick();
        sprintf(key, "k%d", i);
        if (ls_write(s, key, 0, sizes[i] * sizeof *buf, buf) != LS_OK) return 1;
    }
    for (c = 0; c < CYCLES; c++) {
        int k = c % NREC;
        sprintf(key, "k%d", k);
        if (ls_erase(s, key) != LS_OK) return 1;
        sizes[k] = pick();
        if (ls_write(s, key, 0, sizes[k] * sizeof *buf, buf) != LS_OK) return 1;
    }
    for (i = 0; i < NREC; i++) live += (unsigned long long)sizes[i] * sizeof *buf;

    snprintf(path, sizeof path, "%s/churn.libspill", dir);
    if (stat(path, &st) == 0) fsz = (unsigned long long)st.st_size;
    ls_close(s, 0);
    free(buf);

    printf("  %-22s file %7.1f MiB   live %6.1f MiB   x%5.2f\n",
           "libspill POSIX", fsz / 1048576.0, live / 1048576.0,
           live ? (double)fsz / (double)live : 0.0);

    /* HDF5's best on this protocol was x1.37. Anything at or above that would
     * mean the allocator is not earning its place. */
    if (!live || (double)fsz / (double)live > 1.20) {
        printf("  FAIL: growth ratio is not better than HDF5's\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}
