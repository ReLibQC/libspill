/* Extent allocator.
 *
 * This is the part §7b says must be ours rather than HDF5's: "extent allocation
 * and free-list reuse are ours to control, which is exactly the part HDF5 gets
 * wrong for this workload". HDF5 grows x1.4-1.8 under varying-size churn; the
 * policy here is best-fit with immediate coalescing, which is chosen against
 * exactly that measurement rather than for its own elegance. Best-fit rather
 * than first-fit because the failing case is a spread of record sizes, where
 * first-fit carves big holes into unusable fragments.
 */
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "internal.h"

static uint64_t round_up(uint64_t n)
{
    return (n + (LS_ALIGN - 1)) & ~(uint64_t)(LS_ALIGN - 1);
}

static int fl_reserve(ls_store *s, size_t want)
{
    ls_hole *p;
    size_t cap = s->flcap ? s->flcap : 16;
    if (want <= s->flcap) return LS_OK;
    while (cap < want) cap *= 2;
    p = realloc(s->fl, cap * sizeof *p);
    if (!p) return -ENOMEM;
    s->fl = p;
    s->flcap = cap;
    return LS_OK;
}

/* Insert [foff,len) into the free list, coalescing with either neighbour. */
static int fl_insert(ls_store *s, uint64_t foff, uint64_t len)
{
    size_t i, j;
    if (len == 0) return LS_OK;

    for (i = 0; i < s->nfl && s->fl[i].foff < foff; i++)
        ;

    /* merge left */
    if (i > 0 && s->fl[i - 1].foff + s->fl[i - 1].len == foff) {
        s->fl[i - 1].len += len;
        i--;
    } else {
        if (fl_reserve(s, s->nfl + 1) != LS_OK) return -ENOMEM;
        memmove(&s->fl[i + 1], &s->fl[i], (s->nfl - i) * sizeof s->fl[0]);
        s->fl[i].foff = foff;
        s->fl[i].len  = len;
        s->nfl++;
    }

    /* merge right */
    j = i + 1;
    if (j < s->nfl && s->fl[i].foff + s->fl[i].len == s->fl[j].foff) {
        s->fl[i].len += s->fl[j].len;
        memmove(&s->fl[j], &s->fl[j + 1], (s->nfl - j - 1) * sizeof s->fl[0]);
        s->nfl--;
    }

    /* A hole running to the end of the file is not a hole, it is the end. Give
     * the space back to the filesystem rather than keeping the high-water mark:
     * a scratch heap that never shrinks is the behaviour §7b faults HDF5 for,
     * and on a full scratch filesystem it is the difference between a job that
     * finishes and one that does not. */
    if (s->nfl && s->fl[s->nfl - 1].foff + s->fl[s->nfl - 1].len == s->file_end) {
        s->file_end = s->fl[s->nfl - 1].foff;
        s->nfl--;
        if (ftruncate(s->fd, (off_t)s->file_end) != 0)
            ls_report(s, -errno, NULL, s->file_end, 0, "truncating freed tail");
    }
    return LS_OK;
}

/* Three primitives; store.c composes them. Splitting a record across several
 * holes is what keeps the file from growing: a freed hole is otherwise dead
 * whenever the next record is larger than it, which is precisely the case that
 * gives HDF5 its x1.4-1.8. Extents are never smaller than LS_MIN_EXTENT unless
 * they complete a record, so the scatter list stays short. */

/* Best fit: the smallest hole that covers `need`, split to size. */
int ls_alloc_best(ls_store *s, uint64_t need, ls_extent *out)
{
    size_t i, best = (size_t)-1;
    need = round_up(need);
    if (need == 0) return 0;

    pthread_mutex_lock(&s->alloc_lk);
    for (i = 0; i < s->nfl; i++)
        if (s->fl[i].len >= need &&
            (best == (size_t)-1 || s->fl[i].len < s->fl[best].len))
            best = i;
    if (best != (size_t)-1) {
        out->foff = s->fl[best].foff;
        out->len  = need;
        if (s->fl[best].len == need) {
            memmove(&s->fl[best], &s->fl[best + 1],
                    (s->nfl - best - 1) * sizeof s->fl[0]);
            s->nfl--;
        } else {
            s->fl[best].foff += need;
            s->fl[best].len  -= need;
        }
    }
    pthread_mutex_unlock(&s->alloc_lk);
    return best != (size_t)-1;
}

/* The largest hole, taken whole, if it is worth having. */
int ls_alloc_largest(ls_store *s, uint64_t least, ls_extent *out)
{
    size_t i, big = (size_t)-1;

    pthread_mutex_lock(&s->alloc_lk);
    for (i = 0; i < s->nfl; i++)
        if (big == (size_t)-1 || s->fl[i].len > s->fl[big].len) big = i;
    if (big != (size_t)-1 && s->fl[big].len >= round_up(least)) {
        out->foff = s->fl[big].foff;
        out->len  = s->fl[big].len;
        memmove(&s->fl[big], &s->fl[big + 1],
                (s->nfl - big - 1) * sizeof s->fl[0]);
        s->nfl--;
    } else {
        big = (size_t)-1;
    }
    pthread_mutex_unlock(&s->alloc_lk);
    return big != (size_t)-1;
}

/* Fresh space past the end of the file. */
void ls_alloc_tail(ls_store *s, uint64_t need, ls_extent *out)
{
    need = round_up(need);
    pthread_mutex_lock(&s->alloc_lk);
    out->foff = s->file_end;
    out->len  = need;
    s->file_end += need;
    pthread_mutex_unlock(&s->alloc_lk);
}

void ls_free_extents(ls_store *s, ls_extent *e, size_t n)
{
    size_t i;
    if (!e || n == 0) return;
    /* During teardown the records are being freed wholesale and the file has
     * already been written; recycling their extents would shrink the free list
     * back over the table of contents we just persisted. */
    if (s->teardown) return;
    pthread_mutex_lock(&s->alloc_lk);
    for (i = 0; i < n; i++)
        (void)fl_insert(s, e[i].foff, e[i].len);
    pthread_mutex_unlock(&s->alloc_lk);
}

uint64_t ls_alloc_live(ls_store *s)
{
    uint64_t hole = 0, i;
    pthread_mutex_lock(&s->alloc_lk);
    for (i = 0; i < s->nfl; i++) hole += s->fl[i].len;
    i = s->file_end - LS_SUPER_SIZE - hole;
    pthread_mutex_unlock(&s->alloc_lk);
    return i;
}
