/* SPDX-License-Identifier: BSD-3-Clause */
/* The optional HDF5 backend (DESIGN.md §7b).
 *
 * §7b's verdict is precise and this file is built to it: HDF5 has the right
 * SEMANTICS for a scratch heap -- read-back inside one open write session,
 * in-place overwrite, partial sub-range update, space reused rather than
 * appended -- and the wrong SPACE BEHAVIOUR, growing x1.4-1.8 under
 * varying-size churn where the POSIX backend's free list gives x1.13. So it is
 * an option, not the implementation.
 *
 * It exists for one reason, stated in §7b: codes that already link HDF5 and
 * want their scratch files inspectable with standard tooling. h5ls and h5dump
 * work on these files; that is the whole benefit, and it is a real one when
 * debugging a port.
 *
 * TWO THINGS THIS BACKEND CANNOT DO, both measured rather than assumed.
 *
 * 1. Asynchrony. §7b put the stock Fedora build under concurrent hyperslab
 *    reads and got a double free at two threads and a crash at four, because
 *    H5_HAVE_THREADSAFE is not defined there -- still true of the 1.14.6 this
 *    was built against. A thread-safe build instead serialises every API call
 *    behind one global lock. Neither offers in-process I/O concurrency, so
 *    ls_aread and ls_awrite return LS_ERR_MODE here rather than pretending.
 *    That is not a limitation we could engineer away: it is why §7b concludes
 *    POSIX is "the only backend that can deliver the headline feature".
 *
 * 2. Concurrency of any kind. Since the library cannot rely on the underlying
 *    HDF5 being thread-safe, every entry point below takes one lock for its
 *    whole duration -- exactly the global lock §5a faults NWChem for. On this
 *    backend it is unavoidable; on POSIX it is absent. Callers who want the
 *    concurrency §5a promises must use LS_POSIX.
 *
 * The memory tier is likewise not available here: it lives in the POSIX
 * backend's extent layer. A store asking for both is refused at ls_open, since
 * silently ignoring a budget would misreport where the data is.
 */
#include "internal.h"

#ifdef LIBSPILL_HAVE_HDF5

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#define H5_ATTR_NAME "libspill_attr"

/* HDF5 prints its own error stack to stderr on every failure. A library that
 * returns error codes should not also scribble on the caller's terminal. */
static void h5_quiet(void)
{
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
}

static int h5_dataset_exists(ls_store *s, const char *key)
{
    return H5Lexists(s->h5_file, key, H5P_DEFAULT) > 0;
}

/* One 1-D unsigned-char dataset per key, chunked so it can be extended.
 *
 * §7b asks for "chunking defaults tuned to the access patterns these codes
 * actually have", and a fixed chunk is the wrong answer in both directions. A
 * flat 1 MiB gave `h5ls -v` "32 logical bytes, 1048576 allocated bytes, 0.00%
 * utilization" for a four-element record -- HDF5 allocates whole chunks, so a
 * fixed size is a floor on every dataset in the file, and these stores hold
 * small records (a nuclear charge array, a title) beside large ones.
 *
 * So the chunk follows the record: the size it is created at, clamped to a page
 * below and 1 MiB above. Small records stop wasting a megabyte each; large ones
 * still get chunks worth reading. */
#define H5_CHUNK_MAX (1u << 20)
#define H5_CHUNK_MIN 4096u

static hsize_t h5_chunk_for(uint64_t need)
{
    hsize_t c = need ? (hsize_t)need : H5_CHUNK_MIN;
    if (c < H5_CHUNK_MIN) c = H5_CHUNK_MIN;
    if (c > H5_CHUNK_MAX) c = H5_CHUNK_MAX;
    return c;
}

static hid_t h5_open_or_create(ls_store *s, const char *key, uint64_t need, int create)
{
    hid_t ds = -1;

    if (h5_dataset_exists(s, key)) {
        ds = H5Dopen2(s->h5_file, key, H5P_DEFAULT);
        if (ds < 0) return -1;
        if (need) {                              /* extend if the write needs it */
            hid_t sp = H5Dget_space(ds);
            hsize_t cur = 0;
            H5Sget_simple_extent_dims(sp, &cur, NULL);
            H5Sclose(sp);
            if ((uint64_t)cur < need) {
                hsize_t want = (hsize_t)need;
                if (H5Dset_extent(ds, &want) < 0) { H5Dclose(ds); return -1; }
            }
        }
        return ds;
    }
    if (!create) return -1;

    {
        hsize_t dims = (hsize_t)need, maxd = H5S_UNLIMITED;
        hsize_t chunk = h5_chunk_for(need);
        hid_t sp = H5Screate_simple(1, &dims, &maxd);
        hid_t pl = H5Pcreate(H5P_DATASET_CREATE);
        unsigned char zero = 0;
        H5Pset_chunk(pl, 1, &chunk);
        /* §4b promises reserved space and write gaps read as zero. */
        H5Pset_fill_value(pl, H5T_NATIVE_UCHAR, &zero);
        H5Pset_fill_time(pl, H5D_FILL_TIME_ALLOC);
        ds = H5Dcreate2(s->h5_file, key, H5T_NATIVE_UCHAR, sp,
                        H5P_DEFAULT, pl, H5P_DEFAULT);
        H5Pclose(pl);
        H5Sclose(sp);
    }
    return ds;
}

static int h5_extent(ls_store *s, const char *key, uint64_t *n)
{
    hid_t ds, sp;
    hsize_t dims = 0;

    if (!h5_dataset_exists(s, key)) return LS_ERR_NOKEY;
    ds = H5Dopen2(s->h5_file, key, H5P_DEFAULT);
    if (ds < 0) return LS_ERR_BACKEND;
    sp = H5Dget_space(ds);
    H5Sget_simple_extent_dims(sp, &dims, NULL);
    H5Sclose(sp);
    H5Dclose(ds);
    *n = (uint64_t)dims;
    return LS_OK;
}

/* One hyperslab of one dataset: the byte range the caller asked for. */
static int h5_transfer(ls_store *s, const char *key, uint64_t off, size_t n,
                       void *rbuf, const void *wbuf, int op)
{
    hid_t ds, fsp, msp;
    hsize_t start = (hsize_t)off, count = (hsize_t)n;
    int rc = LS_OK;

    ds = h5_open_or_create(s, key, op == LS_OP_WRITE ? off + n : 0,
                           op == LS_OP_WRITE);
    if (ds < 0) return op == LS_OP_WRITE ? LS_ERR_BACKEND : LS_ERR_NOKEY;

    fsp = H5Dget_space(ds);
    if (op == LS_OP_READ) {
        hsize_t dims = 0;
        H5Sget_simple_extent_dims(fsp, &dims, NULL);
        if (off + n > (uint64_t)dims) {
            H5Sclose(fsp);
            H5Dclose(ds);
            return LS_ERR_RANGE;
        }
    }
    if (H5Sselect_hyperslab(fsp, H5S_SELECT_SET, &start, NULL, &count, NULL) < 0)
        rc = LS_ERR_BACKEND;

    msp = H5Screate_simple(1, &count, NULL);
    if (rc == LS_OK) {
        herr_t e = (op == LS_OP_WRITE)
            ? H5Dwrite(ds, H5T_NATIVE_UCHAR, msp, fsp, H5P_DEFAULT, wbuf)
            : H5Dread (ds, H5T_NATIVE_UCHAR, msp, fsp, H5P_DEFAULT, rbuf);
        if (e < 0) rc = op == LS_OP_WRITE ? LS_ERR_BACKEND : LS_ERR_BACKEND;
    }
    H5Sclose(msp);
    H5Sclose(fsp);
    H5Dclose(ds);
    return rc;
}

/* ------------------------------------------------------------- entry points
 * Each takes h5_lk for its whole duration; see the note at the top of the file
 * on why a global lock is unavoidable on this backend. */

int ls_h5_open(ls_store *s, const char *path, int existing)
{
    h5_quiet();
    pthread_mutex_init(&s->h5_lk, NULL);
    s->h5_file = existing
        ? H5Fopen(path, H5F_ACC_RDWR, H5P_DEFAULT)
        : H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (s->h5_file < 0) {
        /* An existing file that is not HDF5 is the same condition the POSIX
         * backend reports as a bad superblock. */
        return existing ? LS_ERR_CORRUPT : LS_ERR_BACKEND;
    }
    return LS_OK;
}

int ls_h5_close(ls_store *s)
{
    int rc = LS_OK;
    if (s->h5_file >= 0 && H5Fclose(s->h5_file) < 0) rc = LS_ERR_BACKEND;
    s->h5_file = -1;
    pthread_mutex_destroy(&s->h5_lk);
    return rc;
}

int ls_h5_rw(ls_store *s, const char *key, uint64_t off, size_t n,
             void *rbuf, const void *wbuf, int op)
{
    int rc;
    if (n == 0) return LS_OK;
    pthread_mutex_lock(&s->h5_lk);
    rc = h5_transfer(s, key, off, n, rbuf, wbuf, op);
    pthread_mutex_unlock(&s->h5_lk);
    if (rc != LS_OK)
        ls_report(s, rc, key, off, n, op == LS_OP_WRITE ? "write" : "read");
    return rc;
}

int ls_h5_size(ls_store *s, const char *key, uint64_t *n)
{
    int rc;
    pthread_mutex_lock(&s->h5_lk);
    rc = h5_extent(s, key, n);
    pthread_mutex_unlock(&s->h5_lk);
    return rc;
}

int ls_h5_exists(ls_store *s, const char *key, int *found)
{
    pthread_mutex_lock(&s->h5_lk);
    *found = h5_dataset_exists(s, key);
    pthread_mutex_unlock(&s->h5_lk);
    return LS_OK;
}

int ls_h5_erase(ls_store *s, const char *key)
{
    int rc = LS_OK;
    pthread_mutex_lock(&s->h5_lk);
    if (!h5_dataset_exists(s, key)) rc = LS_ERR_NOKEY;
    else if (H5Ldelete(s->h5_file, key, H5P_DEFAULT) < 0) rc = LS_ERR_BACKEND;
    pthread_mutex_unlock(&s->h5_lk);
    return rc;
}

int ls_h5_reserve(ls_store *s, const char *key, uint64_t nbytes)
{
    hid_t ds;
    int rc = LS_OK;
    pthread_mutex_lock(&s->h5_lk);
    ds = h5_open_or_create(s, key, nbytes, 1);
    if (ds < 0) rc = LS_ERR_BACKEND;
    else H5Dclose(ds);
    pthread_mutex_unlock(&s->h5_lk);
    if (rc != LS_OK) ls_report(s, rc, key, 0, 0, "reserve");
    return rc;
}

int ls_h5_append(ls_store *s, const char *key, size_t n, const void *buf,
                 uint64_t *off_out)
{
    uint64_t at = 0;
    int rc;

    pthread_mutex_lock(&s->h5_lk);
    if (h5_dataset_exists(s, key)) {
        rc = h5_extent(s, key, &at);
        if (rc != LS_OK) { pthread_mutex_unlock(&s->h5_lk); return rc; }
    }
    rc = h5_transfer(s, key, at, n, NULL, buf, LS_OP_WRITE);
    pthread_mutex_unlock(&s->h5_lk);

    if (rc == LS_OK && off_out) *off_out = at;
    return rc;
}

int ls_h5_accumulate(ls_store *s, const char *key, uint64_t off, size_t n,
                     const void *buf, ls_reduce op, void *ctx)
{
    unsigned char *tmp;
    int rc;

    tmp = malloc(n);
    if (!tmp) return -ENOMEM;

    pthread_mutex_lock(&s->h5_lk);
    rc = h5_transfer(s, key, off, n, tmp, NULL, LS_OP_READ);
    if (rc == LS_ERR_NOKEY || rc == LS_ERR_RANGE) {
        memset(tmp, 0, n);                       /* accumulate into fresh space */
        rc = LS_OK;
    }
    if (rc == LS_OK) {
        op(tmp, buf, n, ctx);
        rc = h5_transfer(s, key, off, n, NULL, tmp, LS_OP_WRITE);
    }
    pthread_mutex_unlock(&s->h5_lk);

    free(tmp);
    if (rc != LS_OK) ls_report(s, rc, key, off, n, "accumulate");
    return rc;
}

/* Attributes are the one place HDF5 is a better fit than our own format: a
 * dataset attribute is exactly what §3a(3) describes, and h5dump shows it. */
int ls_h5_set_attr(ls_store *s, const char *key, const void *blob, size_t n)
{
    hid_t ds, sp, at;
    hsize_t dim = (hsize_t)n;
    int rc = LS_OK;

    pthread_mutex_lock(&s->h5_lk);
    ds = h5_open_or_create(s, key, 0, 1);
    if (ds < 0) { pthread_mutex_unlock(&s->h5_lk); return LS_ERR_BACKEND; }
    if (H5Aexists(ds, H5_ATTR_NAME) > 0) H5Adelete(ds, H5_ATTR_NAME);
    sp = H5Screate_simple(1, &dim, NULL);
    at = H5Acreate2(ds, H5_ATTR_NAME, H5T_NATIVE_UCHAR, sp, H5P_DEFAULT, H5P_DEFAULT);
    if (at < 0 || (n && H5Awrite(at, H5T_NATIVE_UCHAR, blob) < 0)) rc = LS_ERR_BACKEND;
    if (at >= 0) H5Aclose(at);
    H5Sclose(sp);
    H5Dclose(ds);
    pthread_mutex_unlock(&s->h5_lk);
    return rc;
}

int ls_h5_get_attr(ls_store *s, const char *key, void *blob, size_t *n)
{
    hid_t ds, at, sp;
    hsize_t dim = 0;
    int rc = LS_OK;

    pthread_mutex_lock(&s->h5_lk);
    if (!h5_dataset_exists(s, key)) { pthread_mutex_unlock(&s->h5_lk); return LS_ERR_NOKEY; }
    ds = H5Dopen2(s->h5_file, key, H5P_DEFAULT);
    if (ds < 0) { pthread_mutex_unlock(&s->h5_lk); return LS_ERR_BACKEND; }
    if (H5Aexists(ds, H5_ATTR_NAME) <= 0) {
        *n = 0;
        H5Dclose(ds);
        pthread_mutex_unlock(&s->h5_lk);
        return LS_OK;
    }
    at = H5Aopen(ds, H5_ATTR_NAME, H5P_DEFAULT);
    sp = H5Aget_space(at);
    H5Sget_simple_extent_dims(sp, &dim, NULL);
    H5Sclose(sp);

    if (!blob) {
        *n = (size_t)dim;
    } else if (*n < (size_t)dim) {
        *n = (size_t)dim;
        rc = LS_ERR_RANGE;
    } else {
        if (dim && H5Aread(at, H5T_NATIVE_UCHAR, blob) < 0) rc = LS_ERR_BACKEND;
        *n = (size_t)dim;
    }
    H5Aclose(at);
    H5Dclose(ds);
    pthread_mutex_unlock(&s->h5_lk);
    return rc;
}

struct h5_keylist { char **v; size_t n, cap; int bad; };

static herr_t h5_collect(hid_t g, const char *name, const H5L_info2_t *info, void *op)
{
    struct h5_keylist *kl = op;
    (void)g; (void)info;
    if (kl->n == kl->cap) {
        size_t cap = kl->cap ? kl->cap * 2 : 16;
        char **p = realloc(kl->v, cap * sizeof *p);
        if (!p) { kl->bad = 1; return -1; }
        kl->v = p;
        kl->cap = cap;
    }
    kl->v[kl->n] = strdup(name);
    if (!kl->v[kl->n]) { kl->bad = 1; return -1; }
    kl->n++;
    return 0;
}

int ls_h5_keys(ls_store *s, char ***keys, size_t *n)
{
    struct h5_keylist kl;
    hsize_t idx = 0;

    memset(&kl, 0, sizeof kl);
    pthread_mutex_lock(&s->h5_lk);
    H5Literate2(s->h5_file, H5_INDEX_NAME, H5_ITER_NATIVE, &idx, h5_collect, &kl);
    pthread_mutex_unlock(&s->h5_lk);

    if (kl.bad) {
        while (kl.n) free(kl.v[--kl.n]);
        free(kl.v);
        return -ENOMEM;
    }
    if (!kl.v) {
        kl.v = calloc(1, sizeof *kl.v);
        if (!kl.v) return -ENOMEM;
    }
    *keys = kl.v;
    *n = kl.n;
    return LS_OK;
}

#endif /* LIBSPILL_HAVE_HDF5 */
