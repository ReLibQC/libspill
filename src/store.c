/* POSIX backend: one file per store, a heap with its own table of contents and
 * extent free list (DESIGN.md §4b). Not a directory of files per key -- that is
 * what makes the free-list reuse ours to control, and it sidesteps the
 * many-small-datasets problem §7b flags for HDF5. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "internal.h"

/* ------------------------------------------------------------- raw file I/O */

static void *aligned_alloc_or_null(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, LS_ALIGN, n) != 0) return NULL;
    return p;
}

/* Retries short transfers, so the data calls are all-or-nothing to the caller
 * as §4b promises. Under O_DIRECT everything is staged through an aligned
 * bounce buffer: correctness first, and the benchmark's reason for wanting
 * O_DIRECT is to leave the page cache out of the measurement, not to save a
 * copy. */
static int rw_all(ls_store *s, void *buf, const void *cbuf, size_t n,
                  uint64_t off, int is_write)
{
    unsigned char *p = buf;
    const unsigned char *cp = cbuf;
    size_t done = 0;

    if (n == 0) return LS_OK;

    /* The bounce buffer below exists for unaligned callers. When offset, length
     * and buffer are all aligned -- which is the case for whole-block traffic,
     * the workload this library is for -- O_DIRECT can take the caller's buffer
     * directly, and must, since copying every byte costs more than the page
     * cache O_DIRECT was meant to avoid. */
    if (!s->direct ||
        (((uintptr_t)(is_write ? cbuf : buf) & (LS_ALIGN - 1)) == 0 &&
         (off & (uint64_t)(LS_ALIGN - 1)) == 0 &&
         (n   & (size_t)(LS_ALIGN - 1)) == 0)) {
        while (done < n) {
            ssize_t r = is_write
                ? pwrite(s->fd, cp + done, n - done, (off_t)(off + done))
                : pread (s->fd, p  + done, n - done, (off_t)(off + done));
            if (r < 0) { if (errno == EINTR) continue; return -errno; }
            if (r == 0) return is_write ? -ENOSPC : -EIO;
            done += (size_t)r;
        }
        return LS_OK;
    }

    {   /* O_DIRECT: offset, length and buffer must all be LS_ALIGN-aligned */
        unsigned char *bb = aligned_alloc_or_null(LS_BOUNCE);
        int rc = LS_OK;
        if (!bb) return -ENOMEM;

        while (done < n && rc == LS_OK) {
            uint64_t abs   = off + done;
            uint64_t base  = abs & ~(uint64_t)(LS_ALIGN - 1);
            size_t   skew  = (size_t)(abs - base);
            size_t   chunk = n - done;
            size_t   span;
            ssize_t  r;

            if (chunk > LS_BOUNCE - skew) chunk = LS_BOUNCE - skew;
            span = (skew + chunk + LS_ALIGN - 1) & ~(size_t)(LS_ALIGN - 1);

            if (is_write) {
                if (skew || span != skew + chunk) {     /* read-modify-write */
                    r = pread(s->fd, bb, span, (off_t)base);
                    if (r < 0) { rc = -errno; break; }
                    if ((size_t)r < span) memset(bb + r, 0, span - (size_t)r);
                }
                memcpy(bb + skew, cp + done, chunk);
                r = pwrite(s->fd, bb, span, (off_t)base);
            } else {
                r = pread(s->fd, bb, span, (off_t)base);
                if (r >= 0 && (size_t)r < span) memset(bb + r, 0, span - (size_t)r);
                if (r >= 0) memcpy(p + done, bb + skew, chunk);
            }
            if (r < 0) { rc = (errno == EINTR) ? LS_OK : -errno; if (rc) break; continue; }
            done += chunk;
        }
        free(bb);
        return rc;
    }
}

int ls_pread_all (ls_store *s, void *buf, size_t n, uint64_t off)
{ return rw_all(s, buf, NULL, n, off, 0); }

int ls_pwrite_all(ls_store *s, const void *buf, size_t n, uint64_t off)
{ return rw_all(s, NULL, buf, n, off, 1); }

/* Zeroes a file range. A freshly extended file is sparse and already reads as
 * zero, but a reused hole holds whatever the previous record left there, and
 * §4b promises reserved space and write gaps read as zero. */
static int zero_range(ls_store *s, uint64_t off, uint64_t len)
{
    static const size_t CH = 1u << 20;
    unsigned char *z;
    int rc = LS_OK;

#ifdef FALLOC_FL_ZERO_RANGE
    if (!s->direct &&
        fallocate(s->fd, FALLOC_FL_ZERO_RANGE, (off_t)off, (off_t)len) == 0)
        return LS_OK;
#endif
    z = aligned_alloc_or_null(CH);
    if (!z) return -ENOMEM;
    memset(z, 0, CH);
    while (len && rc == LS_OK) {
        size_t k = len < CH ? (size_t)len : CH;
        rc = ls_pwrite_all(s, z, k, off);
        off += k;
        len -= k;
    }
    free(z);
    return rc;
}

/* ----------------------------------------------------------- memory tier */

void ls_lru_drop(ls_store *s, ls_rec *r)
{
    if (r->lru_prev) r->lru_prev->lru_next = r->lru_next;
    else if (s->lru_head == r) s->lru_head = r->lru_next;
    if (r->lru_next) r->lru_next->lru_prev = r->lru_prev;
    else if (s->lru_tail == r) s->lru_tail = r->lru_prev;
    r->lru_prev = r->lru_next = NULL;
}

void ls_lru_touch(ls_store *s, ls_rec *r)
{
    if (s->lru_head == r) return;
    ls_lru_drop(s, r);
    r->lru_next = s->lru_head;
    if (s->lru_head) s->lru_head->lru_prev = r;
    s->lru_head = r;
    if (!s->lru_tail) s->lru_tail = r;
}

static int ensure_extents(ls_store *s, ls_rec *r, uint64_t need);

/* Writes a resident record out to extents and releases its buffer. Called with
 * toc_lk held, which does mean eviction serialises the table while it writes.
 * Accepted for now: the two configurations that matter to §5.2 -- everything
 * fits, or memory_budget is zero -- never reach this path. */
static int spill(ls_store *s, ls_rec *r)
{
    int rc;
    if (!r->mem) return LS_OK;

    rc = ensure_extents(s, r, r->size);
    if (rc != LS_OK) return rc;

    {   /* scatter the buffer across the record's extents */
        uint64_t left = r->size, pos = 0;
        size_t i;
        for (i = 0; i < r->next && left; i++) {
            uint64_t k = r->ext[i].len < left ? r->ext[i].len : left;
            rc = ls_pwrite_all(s, r->mem + pos, (size_t)k, r->ext[i].foff);
            if (rc != LS_OK) return rc;
            pos += k;
            left -= k;
        }
    }
    if (s->resident >= r->mem_cap) s->resident -= (size_t)r->mem_cap;
    free(r->mem);
    r->mem = NULL;
    r->mem_cap = 0;
    ls_lru_drop(s, r);
    return LS_OK;
}

int ls_evict_to(ls_store *s, size_t budget, ls_rec *keep)
{
    while (s->resident > budget) {
        ls_rec *v = s->lru_tail;
        while (v && (v == keep || v->busy)) v = v->lru_prev;
        if (!v) return LS_OK;                 /* nothing evictable; caller spills */
        {
            int rc = spill(s, v);
            if (rc != LS_OK) return rc;
        }
    }
    return LS_OK;
}

/* Forces every resident record out to extents; used by ls_close(keep=1) so
 * that the persisted table of contents describes bytes that are actually
 * on disk. */
int ls_spill_all(ls_store *s)
{
    int rc;
    pthread_mutex_lock(&s->toc_lk);
    rc = ls_evict_to(s, 0, NULL);
    pthread_mutex_unlock(&s->toc_lk);
    return rc;
}

/* ---------------------------------------------------------------- capacity */

/* Growing a record's extent list must not move it: another thread may be part
 * way through an I/O using a snapshot of the old array, taken under toc_lk and
 * held across the transfer. So the array is replaced rather than realloc'd, and
 * the old one is retired to the store until close. Writing at index r->next is
 * safe unlocked -- a snapshot's count never includes it. */
static int ext_retire(ls_store *s, ls_extent *old)
{
    if (!old) return LS_OK;
    if (s->nretired == s->retcap) {
        size_t cap = s->retcap ? s->retcap * 2 : 8;
        ls_extent **p = realloc(s->retired, cap * sizeof *p);
        if (!p) return -ENOMEM;
        s->retired = p;
        s->retcap = cap;
    }
    s->retired[s->nretired++] = old;
    return LS_OK;
}

static int ext_push(ls_store *s, ls_rec *r, ls_extent e)
{
    if (r->next == r->ncap) {
        size_t cap = r->ncap ? r->ncap * 2 : 4;
        ls_extent *p = malloc(cap * sizeof *p);
        if (!p) return -ENOMEM;
        if (r->next) memcpy(p, r->ext, r->next * sizeof *p);
        if (ext_retire(s, r->ext) != LS_OK) { free(p); return -ENOMEM; }
        r->ext = p;
        r->ncap = cap;
    }
    r->ext[r->next] = e;
    r->next++;
    return LS_OK;
}

void ls_place_of(const ls_rec *r, ls_place *p)
{
    p->mem  = r->mem;
    p->ext  = r->ext;
    p->next = r->next;
}

static int ensure_extents(ls_store *s, ls_rec *r, uint64_t need)
{
    while (r->ext_total < need) {
        uint64_t deficit = need - r->ext_total;
        ls_extent e;
        int rc;

        /* Prefer a hole that finishes the job; failing that take the largest
         * hole worth having and come round again; failing that extend the
         * file. Reusing several holes rather than insisting on one contiguous
         * run is what keeps churn from growing the file. */
        if (!ls_alloc_best(s, deficit, &e) &&
            (r->next + 1 >= LS_MAX_EXTENTS ||
             !ls_alloc_largest(s, LS_MIN_EXTENT < deficit ? LS_MIN_EXTENT : deficit, &e)))
            ls_alloc_tail(s, deficit, &e);

        rc = zero_range(s, e.foff, e.len);
        if (rc != LS_OK) { ls_free_extents(s, &e, 1); return rc; }
        rc = ext_push(s, r, e);
        if (rc != LS_OK) { ls_free_extents(s, &e, 1); return rc; }
        r->ext_total += e.len;
    }
    return LS_OK;
}

/* Makes `need` bytes of capacity available, choosing the tier. Returns with the
 * record either resident with mem_cap >= need, or backed by extents. */
static int ensure_cap(ls_store *s, ls_rec *r, uint64_t need)
{
    size_t budget = s->o.memory_budget;

    if (need <= r->mem_cap && r->mem) return LS_OK;
    if (r->ext_total >= need && !r->mem) return LS_OK;

    if (budget && !r->ext) {
        size_t have = r->mem ? (size_t)r->mem_cap : 0;
        if (need <= budget) {
            if (s->resident - have + need > budget)
                (void)ls_evict_to(s, budget - (size_t)need + have, r);
            if (s->resident - have + need <= budget) {
                unsigned char *p = realloc(r->mem, (size_t)need);
                if (p) {
                    if (need > r->mem_cap)
                        memset(p + r->mem_cap, 0, (size_t)(need - r->mem_cap));
                    s->resident = s->resident - have + (size_t)need;
                    r->mem = p;
                    r->mem_cap = need;
                    ls_lru_touch(s, r);
                    return LS_OK;
                }
            }
        }
    }

    if (r->mem) {                            /* does not fit any more: spill */
        int rc = spill(s, r);
        if (rc != LS_OK) return rc;
    }
    return ensure_extents(s, r, need);
}

/* -------------------------------------------------------------- data path */

int ls_scatter(ls_store *s, const ls_place *p, uint64_t off, size_t n,
               void *rbuf, const void *wbuf, int op)
{
    unsigned char *out = rbuf;
    const unsigned char *in = wbuf;
    uint64_t base = 0;
    size_t i, done = 0;

    if (n == 0) return LS_OK;

    if (p->mem) {
        if (op == LS_OP_WRITE) memcpy(p->mem + off, in, n);
        else                   memcpy(out, p->mem + off, n);
        return LS_OK;
    }

    for (i = 0; i < p->next && done < n; i++) {
        uint64_t elo = base, ehi = base + p->ext[i].len;
        base = ehi;
        if (off + done >= ehi) continue;
        {
            uint64_t within = (off + done) - elo;
            size_t   k = (size_t)(ehi - (off + done));
            int rc;
            if (k > n - done) k = n - done;
            rc = (op == LS_OP_WRITE)
                ? ls_pwrite_all(s, in + done, k, p->ext[i].foff + within)
                : ls_pread_all (s, out + done, k, p->ext[i].foff + within);
            if (rc != LS_OK) return rc;
            done += k;
        }
    }
    return done == n ? LS_OK : -EIO;
}

int ls_rw(ls_store *s, const char *key, uint64_t off, size_t n,
          void *rbuf, const void *wbuf, int op)
{
    ls_rec  *r;
    ls_place pl;
    int rc = LS_OK;

    if (!s) return LS_ERR_INVAL;
    if (!ls_key_ok(key) || (n && !(op == LS_OP_WRITE ? (const void *)wbuf : rbuf)))
        return LS_ERR_INVAL;
    if (off + n < off) return LS_ERR_RANGE;

    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);

    if (op == LS_OP_READ) {
        if (!r)                { pthread_mutex_unlock(&s->toc_lk);
                                 ls_report(s, LS_ERR_NOKEY, key, off, n, "read");
                                 return LS_ERR_NOKEY; }
        if (off + n > r->size) { pthread_mutex_unlock(&s->toc_lk);
                                 ls_report(s, LS_ERR_RANGE, key, off, n, "read");
                                 return LS_ERR_RANGE; }
    } else {
        if (s->o.parallel == LS_SHARED &&
            (!r || off + n > r->size)) {     /* would change the layout */
            pthread_mutex_unlock(&s->toc_lk);
            ls_report(s, LS_ERR_MODE, key, off, n,
                      "a shared store's layout is frozen; reserve it before sharing");
            return LS_ERR_MODE;
        }
        if (!r) r = ls_toc_insert(s, key);
        if (!r) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }
        /* A write past the end leaves the gap zero-filled (§4b); fresh extents
         * arrive zeroed from ensure_extents and the resident path memsets. */
        rc = ensure_cap(s, r, off + n);
        if (rc != LS_OK) {
            pthread_mutex_unlock(&s->toc_lk);
            ls_report(s, rc, key, off, n, "reserving space for write");
            return rc;
        }
        if (off + n > r->size) r->size = off + n;
    }
    if (r->mem) ls_lru_touch(s, r);
    r->busy++;
    ls_place_of(r, &pl);
    pthread_mutex_unlock(&s->toc_lk);

    rc = ls_scatter(s, &pl, off, n, rbuf, wbuf, op);

    pthread_mutex_lock(&s->toc_lk);
    r->busy--;
    pthread_mutex_unlock(&s->toc_lk);

    if (rc != LS_OK)
        ls_report(s, rc, key, off, n, op == LS_OP_WRITE ? "write" : "read");
    return rc;
}

int ls_write(ls_store *s, const char *key, uint64_t off, size_t n, const void *buf)
{ return ls_rw(s, key, off, n, NULL, buf, LS_OP_WRITE); }

int ls_read(ls_store *s, const char *key, uint64_t off, size_t n, void *buf)
{ return ls_rw(s, key, off, n, buf, NULL, LS_OP_READ); }

/* ------------------------------------------------------- table of contents */

int ls_exists(ls_store *s, const char *key, int *found)
{
    if (!s || !found || !ls_key_ok(key)) return LS_ERR_INVAL;
    pthread_mutex_lock(&s->toc_lk);
    *found = ls_toc_find(s, key) != NULL;
    pthread_mutex_unlock(&s->toc_lk);
    return LS_OK;
}

int ls_size(ls_store *s, const char *key, uint64_t *nbytes)
{
    ls_rec *r;
    if (!s || !nbytes || !ls_key_ok(key)) return LS_ERR_INVAL;
    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (r) *nbytes = r->size;
    pthread_mutex_unlock(&s->toc_lk);
    return r ? LS_OK : LS_ERR_NOKEY;
}

int ls_erase(ls_store *s, const char *key)
{
    ls_rec *r;
    if (!s || !ls_key_ok(key)) return LS_ERR_INVAL;
    if (s->o.parallel == LS_SHARED) return LS_ERR_MODE;
    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (r) { ls_toc_unlink(s, r); ls_lru_drop(s, r); ls_rec_free(s, r); }
    pthread_mutex_unlock(&s->toc_lk);
    return r ? LS_OK : LS_ERR_NOKEY;
}

int ls_reserve(ls_store *s, const char *key, uint64_t nbytes)
{
    ls_rec *r;
    int rc;
    if (!s || !ls_key_ok(key)) return LS_ERR_INVAL;
    if (s->o.parallel == LS_SHARED) return LS_ERR_MODE;

    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (!r) r = ls_toc_insert(s, key);
    if (!r) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }
    rc = ensure_cap(s, r, nbytes);
    if (rc == LS_OK && nbytes > r->size) r->size = nbytes;
    pthread_mutex_unlock(&s->toc_lk);

    if (rc != LS_OK)
        ls_report(s, rc, key, 0, 0, "reserve");
    return rc;
}

int ls_keys(ls_store *s, char ***keys, size_t *n)
{
    char **out;
    size_t i, k = 0;

    if (!s || !keys || !n) return LS_ERR_INVAL;

    pthread_mutex_lock(&s->toc_lk);
    out = s->nrec ? calloc(s->nrec, sizeof *out) : calloc(1, sizeof *out);
    if (!out) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }
    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r;
        for (r = s->tab[i]; r; r = r->hnext) {
            out[k] = strdup(r->key);
            if (!out[k]) {
                while (k) free(out[--k]);
                free(out);
                pthread_mutex_unlock(&s->toc_lk);
                return -ENOMEM;
            }
            k++;
        }
    }
    pthread_mutex_unlock(&s->toc_lk);

    *keys = out;
    *n = k;
    return LS_OK;
}

void ls_keys_free(char **keys, size_t n)
{
    size_t i;
    if (!keys) return;
    for (i = 0; i < n; i++) free(keys[i]);
    free(keys);
}

/* ------------------------------------------------------------------ append */

int ls_append(ls_store *s, const char *key, size_t n, const void *buf,
              uint64_t *off_out)
{
    ls_rec  *r;
    ls_place pl;
    uint64_t off;
    int      rc;

    if (!s || !ls_key_ok(key) || (n && !buf)) return LS_ERR_INVAL;
    if (s->o.parallel == LS_SHARED) return LS_ERR_MODE;

    /* The offset is taken and the record extended under one lock. That is the
     * whole point: ls_size followed by ls_write is not the same thing, because
     * another thread can append in between. */
    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (!r) r = ls_toc_insert(s, key);
    if (!r) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }

    off = r->size;
    if (off + n < off) { pthread_mutex_unlock(&s->toc_lk); return LS_ERR_RANGE; }

    rc = ensure_cap(s, r, off + n);
    if (rc != LS_OK) {
        pthread_mutex_unlock(&s->toc_lk);
        ls_report(s, rc, key, off, n, "reserving space for append");
        return rc;
    }
    r->size = off + n;
    if (r->mem) ls_lru_touch(s, r);
    r->busy++;
    ls_place_of(r, &pl);
    pthread_mutex_unlock(&s->toc_lk);

    rc = ls_scatter(s, &pl, off, n, NULL, buf, LS_OP_WRITE);

    pthread_mutex_lock(&s->toc_lk);
    r->busy--;
    pthread_mutex_unlock(&s->toc_lk);

    if (rc != LS_OK) ls_report(s, rc, key, off, n, "append");
    else if (off_out) *off_out = off;
    return rc;
}

/* ------------------------------------------------------------- vectored I/O */

static int segv(ls_store *s, const char *key, const ls_seg *segs, size_t nseg, int op)
{
    ls_rec  *r;
    ls_place pl;
    size_t   i;
    int      rc = LS_OK;
    uint64_t hi = 0;

    if (!s || !ls_key_ok(key) || (nseg && !segs)) return LS_ERR_INVAL;
    if (nseg == 0) return LS_OK;

    for (i = 0; i < nseg; i++) {
        if (segs[i].len && !segs[i].buf) return LS_ERR_INVAL;
        if (segs[i].off + segs[i].len < segs[i].off) return LS_ERR_RANGE;
        if (segs[i].off + segs[i].len > hi) hi = segs[i].off + segs[i].len;
    }

    /* One lookup for the whole list. Against Conquest's "thousands of individual
     * scalar operations per file", that alone is most of the gain. */
    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);

    if (op == LS_OP_READ) {
        if (!r) { pthread_mutex_unlock(&s->toc_lk);
                  ls_report(s, LS_ERR_NOKEY, key, 0, 0, "readv");
                  return LS_ERR_NOKEY; }
        if (hi > r->size) { pthread_mutex_unlock(&s->toc_lk);
                            ls_report(s, LS_ERR_RANGE, key, 0, 0, "readv");
                            return LS_ERR_RANGE; }
    } else {
        if (s->o.parallel == LS_SHARED && (!r || hi > r->size)) {
            pthread_mutex_unlock(&s->toc_lk);
            ls_report(s, LS_ERR_MODE, key, 0, hi,
                      "a shared store's layout is frozen; reserve it before sharing");
            return LS_ERR_MODE;
        }
        if (!r) r = ls_toc_insert(s, key);
        if (!r) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }
        rc = ensure_cap(s, r, hi);
        if (rc != LS_OK) { pthread_mutex_unlock(&s->toc_lk);
                           ls_report(s, rc, key, 0, 0, "reserving space for writev");
                           return rc; }
        if (hi > r->size) r->size = hi;
    }
    if (r->mem) ls_lru_touch(s, r);
    r->busy++;
    ls_place_of(r, &pl);
    pthread_mutex_unlock(&s->toc_lk);

    for (i = 0; i < nseg && rc == LS_OK; i++)
        rc = ls_scatter(s, &pl, segs[i].off, segs[i].len,
                        op == LS_OP_READ ? segs[i].buf : NULL,
                        op == LS_OP_READ ? NULL : segs[i].buf, op);

    pthread_mutex_lock(&s->toc_lk);
    r->busy--;
    pthread_mutex_unlock(&s->toc_lk);

    if (rc != LS_OK)
        ls_report(s, rc, key, 0, 0, op == LS_OP_READ ? "readv" : "writev");
    return rc;
}

int ls_readv(ls_store *s, const char *key, const ls_seg *segs, size_t nseg)
{ return segv(s, key, segs, nseg, LS_OP_READ); }

int ls_writev(ls_store *s, const char *key, const ls_seg *segs, size_t nseg)
{ return segv(s, key, segs, nseg, LS_OP_WRITE); }

/* -------------------------------------------------------------- attributes */

int ls_set_attr(ls_store *s, const char *key, const void *blob, size_t n)
{
    ls_rec *r;

    if (!s || !ls_key_ok(key)) return LS_ERR_INVAL;
    if (n > LS_ATTR_MAX || (n && !blob)) return LS_ERR_INVAL;
    if (s->o.parallel == LS_SHARED) return LS_ERR_MODE;

    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (!r) r = ls_toc_insert(s, key);
    if (!r) { pthread_mutex_unlock(&s->toc_lk); return -ENOMEM; }
    if (n) memcpy(r->attr, blob, n);
    r->attrlen = (uint32_t)n;
    pthread_mutex_unlock(&s->toc_lk);
    return LS_OK;
}

int ls_get_attr(ls_store *s, const char *key, void *blob, size_t *n)
{
    ls_rec *r;
    int rc = LS_OK;

    if (!s || !ls_key_ok(key) || !n) return LS_ERR_INVAL;

    pthread_mutex_lock(&s->toc_lk);
    r = ls_toc_find(s, key);
    if (!r) {
        rc = LS_ERR_NOKEY;
    } else if (!blob) {
        *n = r->attrlen;                 /* enquiry only */
    } else if (*n < r->attrlen) {
        *n = r->attrlen;                 /* tell the caller what it needs */
        rc = LS_ERR_RANGE;
    } else {
        if (r->attrlen) memcpy(blob, r->attr, r->attrlen);
        *n = r->attrlen;
    }
    pthread_mutex_unlock(&s->toc_lk);
    return rc;
}
