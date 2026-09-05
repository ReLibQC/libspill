/* Out-of-core benchmark: does asynchronous overlap actually win?
 *
 * This is success criterion 2 (DESIGN.md §1), and the only question the design
 * says cannot be settled on paper. The kernel is the shape of an out-of-core
 * amplitude update: for each block, read it, do arithmetic on it, write it
 * back, repeated over a working set too large to hold.
 *
 * Four variants, because two separate things need measuring:
 *
 *   A  raw POSIX, synchronous       what libpsio does today -- the thing to beat
 *   D  libscratch, synchronous      A plus our overhead; the wrapper's cost
 *   B  raw POSIX, hand-rolled       a careful caller's own double buffering:
 *      prefetch                     one helper thread reads block b+1 while the
 *                                   main thread computes on block b
 *   C  libscratch, asynchronous     ls_aread/ls_awrite, depth 2
 *
 * A vs D is the honest cost of the abstraction. B vs C is the honest gain,
 * because measuring against A alone would credit the library with an overlap
 * any competent caller could have written themselves. §5.1 claims most callers
 * do not write it; that is an argument for the library, not a licence to
 * benchmark against the weaker baseline.
 *
 * Measurement discipline (§7e's caveat: a 1200-2000 MB/s "baseline" is the page
 * cache, not a device). Pass --direct to open everything O_DIRECT so the page
 * cache is out of the measurement. Without it the numbers describe memory.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libscratch.h"

static size_t NBLK   = 64;
static size_t BLKMIB = 8;
static int    FLOPS  = 24;          /* inner passes per element */
static int    DIRECT = 0;
static size_t BUDGET = 0;
static const char *DIR = NULL;
static int    REPS   = 5;

static size_t blkbytes(void) { return BLKMIB << 20; }
static size_t blkelem(void)  { return blkbytes() / sizeof(double); }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static void *amalloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 4096, n) != 0) { perror("posix_memalign"); exit(1); }
    return p;
}

/* The compute kernel. Deliberately memory-resident and unvectorisable-away:
 * the point is to occupy the CPU for a controllable time, so that the ratio of
 * compute to transfer can be swept. */
static double kernel(double *x, size_t n, int passes)
{
    double acc = 0.0;
    int p;
    size_t i;
    for (p = 0; p < passes; p++)
        for (i = 0; i < n; i++)
            x[i] = x[i] * 0.9999999 + 1e-9;   /* decays; must not overflow
                                                 across repetitions */
    for (i = 0; i < n; i++) acc += x[i];
    return acc;
}

/* ------------------------------------------------------------ raw POSIX file */

static int raw_open(const char *path)
{
    int flags = O_RDWR | O_CREAT | O_TRUNC;
#ifdef O_DIRECT
    if (DIRECT) flags |= O_DIRECT;
#endif
    {
        int fd = open(path, flags, 0600);
        if (fd < 0) { perror("open"); exit(1); }
        return fd;
    }
}

static void raw_rw(int fd, void *buf, size_t n, off_t off, int wr)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = wr ? pwrite(fd, (char *)buf + done, n - done, off + (off_t)done)
                       : pread (fd, (char *)buf + done, n - done, off + (off_t)done);
        if (r < 0) { if (errno == EINTR) continue; perror(wr ? "pwrite" : "pread"); exit(1); }
        if (r == 0) { fprintf(stderr, "short transfer\n"); exit(1); }
        done += (size_t)r;
    }
}

/* ---------------------------------------------------------------- variant A */

static double run_raw_sync(int fd, double *buf, double *chk)
{
    double t0 = now();
    size_t b;
    for (b = 0; b < NBLK; b++) {
        raw_rw(fd, buf, blkbytes(), (off_t)(b * blkbytes()), 0);
        *chk += kernel(buf, blkelem(), FLOPS);
        raw_rw(fd, buf, blkbytes(), (off_t)(b * blkbytes()), 1);
    }
    return now() - t0;
}

/* ---------------------------------------------------------------- variant B */

struct pf {
    int fd;
    double *dst;
    size_t block;
    int want, quit;
    pthread_mutex_t lk;
    pthread_cond_t  cv_req, cv_done;
    int ready;
};

static void *pf_thread(void *arg)
{
    struct pf *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lk);
        while (!p->want && !p->quit) pthread_cond_wait(&p->cv_req, &p->lk);
        if (p->quit) { pthread_mutex_unlock(&p->lk); break; }
        p->want = 0;
        pthread_mutex_unlock(&p->lk);

        raw_rw(p->fd, p->dst, blkbytes(), (off_t)(p->block * blkbytes()), 0);

        pthread_mutex_lock(&p->lk);
        p->ready = 1;
        pthread_cond_signal(&p->cv_done);
        pthread_mutex_unlock(&p->lk);
    }
    return NULL;
}

static double run_raw_prefetch(int fd, double *b0, double *b1, double *chk)
{
    struct pf p;
    pthread_t th;
    double *cur = b0, *nxt = b1, t0;
    size_t b;

    memset(&p, 0, sizeof p);
    p.fd = fd;
    pthread_mutex_init(&p.lk, NULL);
    pthread_cond_init(&p.cv_req, NULL);
    pthread_cond_init(&p.cv_done, NULL);
    pthread_create(&th, NULL, pf_thread, &p);

    t0 = now();
    raw_rw(fd, cur, blkbytes(), 0, 0);                  /* first block by hand */
    for (b = 0; b < NBLK; b++) {
        if (b + 1 < NBLK) {                             /* ask for the next    */
            pthread_mutex_lock(&p.lk);
            p.dst = nxt; p.block = b + 1; p.ready = 0; p.want = 1;
            pthread_cond_signal(&p.cv_req);
            pthread_mutex_unlock(&p.lk);
        }
        *chk += kernel(cur, blkelem(), FLOPS);
        raw_rw(fd, cur, blkbytes(), (off_t)(b * blkbytes()), 1);
        if (b + 1 < NBLK) {
            pthread_mutex_lock(&p.lk);
            while (!p.ready) pthread_cond_wait(&p.cv_done, &p.lk);
            pthread_mutex_unlock(&p.lk);
            { double *t = cur; cur = nxt; nxt = t; }
        }
    }
    {
        double el = now() - t0;
        pthread_mutex_lock(&p.lk);
        p.quit = 1;
        pthread_cond_signal(&p.cv_req);
        pthread_mutex_unlock(&p.lk);
        pthread_join(th, NULL);
        return el;
    }
}

/* ------------------------------------------------------------ variants D, C */

static void key_of(char *k, size_t b) { sprintf(k, "blk%zu", b); }

static double run_ls_sync(ls_store *s, double *buf, double *chk)
{
    double t0 = now();
    size_t b;
    char k[32];
    for (b = 0; b < NBLK; b++) {
        key_of(k, b);
        if (ls_read(s, k, 0, blkbytes(), buf) != LS_OK) { fprintf(stderr, "read\n"); exit(1); }
        *chk += kernel(buf, blkelem(), FLOPS);
        if (ls_write(s, k, 0, blkbytes(), buf) != LS_OK) { fprintf(stderr, "write\n"); exit(1); }
    }
    return now() - t0;
}

static double run_ls_async(ls_store *s, double *b0, double *b1, double *chk)
{
    double *cur = b0, *nxt = b1, t0;
    ls_req *rd = NULL, *wr = NULL;
    char k[32];
    size_t b;

    t0 = now();
    key_of(k, 0);
    if (ls_aread(s, k, 0, blkbytes(), cur, &rd) != LS_OK) { fprintf(stderr, "aread\n"); exit(1); }
    if (ls_wait(rd) != LS_OK) { fprintf(stderr, "wait\n"); exit(1); }

    for (b = 0; b < NBLK; b++) {
        rd = NULL;
        if (b + 1 < NBLK) {
            key_of(k, b + 1);
            ls_aread(s, k, 0, blkbytes(), nxt, &rd);
        }
        *chk += kernel(cur, blkelem(), FLOPS);

        if (wr) { ls_wait(wr); wr = NULL; }     /* previous write-back lands */
        key_of(k, b);
        ls_awrite(s, k, 0, blkbytes(), cur, &wr);

        if (rd) {
            ls_wait(rd);
            { double *t = cur; cur = nxt; nxt = t; }
        }
    }
    if (wr) ls_wait(wr);
    return now() - t0;
}

/* ------------------------------------------------------------------- driver */

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Median, not mean and not best-of-N. A shared machine produces a long right
 * tail that the mean chases and the minimum ignores; the median is the number
 * that survives someone else's compile job running on the other cores. */
static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static void report(const char *name, double *v, int n, double bytes)
{
    double med = median(v, n);
    printf("  %-34s %8.3f s   %8.1f MB/s   [%.3f-%.3f]\n", name, med,
           bytes / med / 1e6, v[0], v[n - 1]);
}

int main(int argc, char **argv)
{
    double *buf, *buf2, chk = 0.0;
    double bytes, tA, tB, tC, tD;
    char path[512];
    const char *dir;
    int fd, i;
    ls_opts o;
    ls_store *s;
    int err = 0;
    size_t b;

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--blocks")    && i + 1 < argc) NBLK   = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--block-mib") && i + 1 < argc) BLKMIB = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--flops")     && i + 1 < argc) FLOPS  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--budget")    && i + 1 < argc) BUDGET = (size_t)atol(argv[++i]) << 20;
        else if (!strcmp(argv[i], "--dir")       && i + 1 < argc) DIR    = argv[++i];
        else if (!strcmp(argv[i], "--reps")      && i + 1 < argc) REPS   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--direct"))                    DIRECT = 1;
        else { fprintf(stderr,
                 "usage: %s [--blocks N] [--block-mib M] [--flops F]\n"
                 "          [--budget MIB] [--dir PATH] [--direct] [--reps R]\n", argv[0]);
               return 2; }
    }

    dir = DIR ? DIR : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    bytes = (double)NBLK * blkbytes() * 2.0;         /* read + write per block */

    printf("out-of-core benchmark: %zu blocks x %zu MiB = %.1f GiB working set\n",
           NBLK, BLKMIB, NBLK * (double)BLKMIB / 1024.0);
    printf("  dir=%s  flops=%d  direct=%s  budget=%zu MiB\n", dir, FLOPS,
           DIRECT ? "yes" : "NO (page cache is in the measurement)", BUDGET >> 20);
    printf("  moving %.2f GiB per pass, %d repetitions\n\n", bytes / (1024.0 * 1024.0 * 1024.0), REPS);

    buf  = amalloc(blkbytes());
    buf2 = amalloc(blkbytes());
    for (b = 0; b < blkelem(); b++) buf[b] = 1.0 + (double)b;

    {
        double *vA = amalloc((size_t)REPS * sizeof(double));
        double *vB = amalloc((size_t)REPS * sizeof(double));
        double *vC = amalloc((size_t)REPS * sizeof(double));
        double *vD = amalloc((size_t)REPS * sizeof(double));
        int rep;

        snprintf(path, sizeof path, "%s/oocbench.raw", dir);
        fd = raw_open(path);
        for (b = 0; b < NBLK; b++) raw_rw(fd, buf, blkbytes(), (off_t)(b * blkbytes()), 1);

        ls_opts_default(&o);
        o.dir = dir;
        o.direct_io = DIRECT;
        o.memory_budget = BUDGET;
        s = ls_open("oocbench", &o, &err);
        if (!s) { fprintf(stderr, "ls_open: %d\n", err); return 1; }
        for (b = 0; b < NBLK; b++) {
            char k[32];
            key_of(k, b);
            if (ls_write(s, k, 0, blkbytes(), buf) != LS_OK) { fprintf(stderr, "fill\n"); return 1; }
        }

        /* Interleaved rather than variant-at-a-time: on a machine with other
         * work on it, running all of A then all of B measures the machine's
         * mood as much as the code. */
        for (rep = 0; rep < REPS; rep++) {
            if (!DIRECT) { fsync(fd); posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); }
            vA[rep] = run_raw_sync(fd, buf, &chk);
            vD[rep] = run_ls_sync(s, buf, &chk);
            if (!DIRECT) { fsync(fd); posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); }
            vB[rep] = run_raw_prefetch(fd, buf, buf2, &chk);
            vC[rep] = run_ls_async(s, buf, buf2, &chk);
        }
        close(fd);
        unlink(path);
        ls_close(s, 0);

        printf("  %-34s %10s   %12s   %s\n", "variant", "median", "effective", "[min-max]");
        report("A raw POSIX, synchronous",   vA, REPS, bytes);
        report("D libscratch, synchronous",  vD, REPS, bytes);
        report("B raw POSIX, hand prefetch", vB, REPS, bytes);
        report("C libscratch, async",        vC, REPS, bytes);

        tA = median(vA, REPS); tB = median(vB, REPS);
        tC = median(vC, REPS); tD = median(vD, REPS);
        free(vA); free(vB); free(vC); free(vD);
    }

    printf("\n  abstraction cost   A -> D  %+6.1f%%\n", 100.0 * (tD - tA) / tA);
    printf("  overlap, by hand   A -> B  %+6.1f%%\n", 100.0 * (tB - tA) / tA);
    printf("  overlap, library   D -> C  %+6.1f%%\n", 100.0 * (tC - tD) / tD);
    printf("  the honest number  B -> C  %+6.1f%%   (negative = libscratch wins)\n",
           100.0 * (tC - tB) / tB);
    printf("\n  checksum %.6e\n", chk);
    free(buf);
    free(buf2);
    return 0;
}
