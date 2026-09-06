/* Store lifecycle, and the on-disk superblock that makes keep=1 mean something.
 * Restart is out of scope (DESIGN.md §3): the persisted table of contents
 * exists so a scratch file can be inspected and reopened while debugging a
 * port, not so a job can resume from one. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "internal.h"

static void put32(unsigned char *p, uint32_t v)
{ p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static void put64(unsigned char *p, uint64_t v)
{ int i; for (i=0;i<8;i++) p[i]=(unsigned char)(v >> (8*i)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t get64(const unsigned char *p)
{ uint64_t v=0; int i; for (i=7;i>=0;i--) v = (v<<8) | p[i]; return v; }

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

/* ------------------------------------------------------------------ options */

void ls_opts_default(ls_opts *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->version  = LS_OPTS_VERSION;
    o->backend  = LS_POSIX;
    o->mode     = LS_EXPLICIT;
    o->parallel = LS_LOCAL;
    o->rank     = -1;
}

static int resolve_rank(int rank)
{
    static const char *vars[] = { "OMPI_COMM_WORLD_RANK", "PMI_RANK",
                                  "PMIX_RANK", "SLURM_PROCID", NULL };
    int i;
    if (rank >= 0) return rank;
    for (i = 0; vars[i]; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) {
            long r = strtol(v, NULL, 10);
            if (r >= 0) return (int)r;
        }
    }
    return (int)getpid();          /* never collides on a node; §4b */
}

static char *build_path(const char *name, const ls_opts *o)
{
    const char *dir = o->dir;
    char *p;
    size_t n;

    if (!dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";

    n = strlen(dir) + strlen(name) + 64;
    p = malloc(n);
    if (!p) return NULL;

    /* LS_SHARED deliberately does NOT fold in the rank: every process must
     * name the same file. */
    if (o->parallel == LS_PER_RANK)
        snprintf(p, n, "%s/%s.r%d.libspill", dir, name, resolve_rank(o->rank));
    else
        snprintf(p, n, "%s/%s.libspill", dir, name);
    return p;
}

/* ------------------------------------------------- table of contents on disk */

static int toc_serialise(ls_store *s, unsigned char **out, size_t *outn)
{
    size_t need = 0, i;
    unsigned char *b, *w;

    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r;
        for (r = s->tab[i]; r; r = r->hnext)
            need += 4 + strlen(r->key) + 8 + 4 + r->attrlen + 4 + 16 * r->next;
    }
    b = malloc(need ? need : 1);
    if (!b) return -ENOMEM;

    w = b;
    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r;
        for (r = s->tab[i]; r; r = r->hnext) {
            size_t kl = strlen(r->key), e;
            put32(w, (uint32_t)kl);            w += 4;
            memcpy(w, r->key, kl);             w += kl;
            put64(w, r->size);                 w += 8;
            put32(w, r->attrlen);              w += 4;
            if (r->attrlen) { memcpy(w, r->attr, r->attrlen); w += r->attrlen; }
            put32(w, (uint32_t)r->next);       w += 4;
            for (e = 0; e < r->next; e++) {
                put64(w, r->ext[e].foff);      w += 8;
                put64(w, r->ext[e].len);       w += 8;
            }
        }
    }
    *out = b;
    *outn = need;
    return LS_OK;
}

static int toc_load(ls_store *s, const unsigned char *b, size_t n, uint64_t count)
{
    const unsigned char *w = b, *end = b + n;
    uint64_t c;

    for (c = 0; c < count; c++) {
        char key[LS_KEY_MAX + 1];
        uint32_t kl, ne, e;
        ls_rec *r;

        if (end - w < 4) return LS_ERR_CORRUPT;
        kl = get32(w); w += 4;
        if (kl == 0 || kl > LS_KEY_MAX || (size_t)(end - w) < kl + 8u)
            return LS_ERR_CORRUPT;
        memcpy(key, w, kl); key[kl] = '\0'; w += kl;

        r = ls_toc_insert(s, key);
        if (!r) return -ENOMEM;
        r->size = get64(w); w += 8;
        if (end - w < 4) return LS_ERR_CORRUPT;
        r->attrlen = get32(w); w += 4;
        if (r->attrlen > LS_ATTR_MAX || (size_t)(end - w) < r->attrlen + 4u)
            return LS_ERR_CORRUPT;
        if (r->attrlen) { memcpy(r->attr, w, r->attrlen); w += r->attrlen; }
        ne = get32(w); w += 4;
        if ((size_t)(end - w) < (size_t)ne * 16u) return LS_ERR_CORRUPT;
        for (e = 0; e < ne; e++) {
            ls_extent x;
            x.foff = get64(w); w += 8;
            x.len  = get64(w); w += 8;
            if (x.foff < LS_SUPER_SIZE || x.foff + x.len > s->file_end)
                return LS_ERR_CORRUPT;
            if (r->next == r->ncap) {
                size_t cap = r->ncap ? r->ncap * 2 : 4;
                ls_extent *p = realloc(r->ext, cap * sizeof *p);
                if (!p) return -ENOMEM;
                r->ext = p; r->ncap = cap;
            }
            r->ext[r->next++] = x;
            r->ext_total += x.len;
        }
    }
    return LS_OK;
}

static int cmp_ext(const void *a, const void *b)
{
    uint64_t x = ((const ls_extent *)a)->foff, y = ((const ls_extent *)b)->foff;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Reconstructs the free list from the gaps between live extents. */
static int rebuild_free_list(ls_store *s)
{
    ls_extent *all;
    size_t n = 0, i, k = 0;
    uint64_t cur = LS_SUPER_SIZE;

    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r;
        for (r = s->tab[i]; r; r = r->hnext) n += r->next;
    }
    if (n == 0) return LS_OK;

    all = malloc(n * sizeof *all);
    if (!all) return -ENOMEM;
    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r;
        for (r = s->tab[i]; r; r = r->hnext) {
            size_t e;
            for (e = 0; e < r->next; e++) all[k++] = r->ext[e];
        }
    }
    qsort(all, n, sizeof *all, cmp_ext);

    for (i = 0; i < n; i++) {
        if (all[i].foff < cur) { free(all); return LS_ERR_CORRUPT; }  /* overlap */
        if (all[i].foff > cur) {
            ls_extent hole;
            hole.foff = cur;
            hole.len  = all[i].foff - cur;
            ls_free_extents(s, &hole, 1);
        }
        cur = all[i].foff + all[i].len;
    }
    free(all);
    return LS_OK;
}

/* ------------------------------------------------------------ open / close */

ls_store *ls_open(const char *name, const ls_opts *opts, int *err)
{
    ls_opts o;
    ls_store *s;
    struct stat st;
    int rc = LS_OK;

    if (opts) o = *opts; else ls_opts_default(&o);
    if (err) *err = LS_OK;

    if (!name || !*name) { if (err) *err = LS_ERR_INVAL; return NULL; }
    if (o.version != LS_OPTS_VERSION) { if (err) *err = LS_ERR_INVAL; return NULL; }

    /* §4b: the combinations that are wrong in principle are refused before the
     * ones that are merely not built yet, so that behaviour does not change
     * when LS_MAPPED and LS_HDF5 land. */
    /* §3: LS_MAPPED implies the POSIX backend, since mapping an HDF5 dataset is
     * not meaningful; §4b: a memory budget on a mapped store means nothing,
     * because its cache is the kernel's page cache. O_DIRECT and mmap are
     * likewise mutually exclusive -- the point of one is to bypass what the
     * other maps. */
    if (o.mode == LS_MAPPED &&
        (o.backend != LS_POSIX || o.memory_budget != 0 || o.direct_io)) {
        if (err) *err = LS_ERR_INVAL;
        return NULL;
    }
    /* A per-process memory tier would keep writes where the other processes
     * cannot see them. In a shared store that is a correctness failure, not a
     * tuning choice, so it is refused rather than ignored. */
    if (o.parallel == LS_SHARED && o.memory_budget != 0) {
        if (err) *err = LS_ERR_INVAL;
        return NULL;
    }
    if (o.backend == LS_HDF5) { if (err) *err = LS_ERR_BACKEND; return NULL; }

    s = calloc(1, sizeof *s);
    if (!s) { if (err) *err = -ENOMEM; return NULL; }
    s->o = o;
    s->fd = -1;
    s->nbuckets = 64;
    s->tab = calloc(s->nbuckets, sizeof *s->tab);
    s->path = build_path(name, &o);
    if (!s->tab || !s->path) { rc = -ENOMEM; goto fail; }

    pthread_mutex_init(&s->toc_lk, NULL);
    pthread_mutex_init(&s->alloc_lk, NULL);
    pthread_mutex_init(&s->q_lk, NULL);
    pthread_cond_init(&s->q_cv, NULL);

    {
        int flags = O_RDWR | O_CREAT;
#ifdef O_DIRECT
        if (o.direct_io) flags |= O_DIRECT;
#endif
        s->fd = open(s->path, flags, 0600);
#ifdef O_DIRECT
        if (s->fd < 0 && o.direct_io) {         /* filesystem may refuse it */
            s->fd = open(s->path, O_RDWR | O_CREAT, 0600);
            if (s->fd >= 0)
                ls_report(s, LS_OK, NULL, 0, 0, "O_DIRECT unavailable; using buffered I/O");
        } else if (s->fd >= 0 && o.direct_io) {
            s->direct = 1;
        }
#endif
        if (s->fd < 0) { rc = -errno; goto fail; }
    }

    if (fstat(s->fd, &st) != 0) { rc = -errno; goto fail; }
    s->file_end = LS_SUPER_SIZE;

    if (st.st_size >= (off_t)LS_SUPER_SIZE) {
        unsigned char sb[LS_SUPER_SIZE];
        rc = ls_pread_all(s, sb, LS_SUPER_SIZE, 0);
        if (rc != LS_OK) goto fail;
        if (memcmp(sb, LS_MAGIC, 8) != 0 || get32(sb + 8) != LS_FMT_VERSION) {
            rc = LS_ERR_CORRUPT;
            goto fail;
        }
        {
            uint64_t toc_off = get64(sb + 16), toc_n = get64(sb + 24);
            uint64_t fend = get64(sb + 32);
            uint32_t crc = get32(sb + 40);
            uint64_t tlen = (uint64_t)st.st_size - toc_off;
            unsigned char *tb;

            if (toc_off < LS_SUPER_SIZE || toc_off > (uint64_t)st.st_size) {
                rc = LS_ERR_CORRUPT; goto fail;
            }
            s->file_end = fend;
            tb = malloc(tlen ? (size_t)tlen : 1);
            if (!tb) { rc = -ENOMEM; goto fail; }
            rc = ls_pread_all(s, tb, (size_t)tlen, toc_off);
            if (rc == LS_OK && crc32_of(tb, (size_t)tlen) != crc) rc = LS_ERR_CORRUPT;
            if (rc == LS_OK) rc = toc_load(s, tb, (size_t)tlen, toc_n);
            free(tb);
            if (rc != LS_OK) goto fail;
            rc = rebuild_free_list(s);
            if (rc != LS_OK) goto fail;
        }
    } else if (st.st_size > 0) {
        rc = LS_ERR_CORRUPT;
        goto fail;
    } else if (o.parallel == LS_SHARED) {
        /* Nothing to share. The layout is frozen in this mode, so there is no
         * way for this process to create records the others would agree on;
         * saying so is better than opening an empty store that fails later. */
        rc = -ENOENT;
        goto fail;
    } else if (ftruncate(s->fd, (off_t)LS_SUPER_SIZE) != 0) {
        rc = -errno;
        goto fail;
    }

    rc = ls_pool_start(s);
    if (rc != LS_OK) goto fail;

    return s;

fail:
    if (err) *err = rc;
    if (s) {
        if (s->fd >= 0) close(s->fd);
        free(s->tab);
        free(s->path);
        free(s);
    }
    return NULL;
}

int ls_close(ls_store *s, int keep)
{
    int rc = LS_OK, first = LS_OK;
    size_t i;

    if (!s) return LS_ERR_INVAL;

    ls_pool_stop(s);                    /* drains in flight, as fclose flushes */

    /* A shared store belongs to whoever created it. Every process closing one
     * would race to rewrite the same table of contents, and with keep=0 each
     * would try to unlink a file the others are still reading. */
    if (s->o.parallel == LS_SHARED) keep = 1;

    if (keep && s->o.parallel != LS_SHARED) {
        unsigned char *tb = NULL, sb[LS_SUPER_SIZE];
        size_t tn = 0;

        rc = ls_spill_all(s);                   /* resident records must land */
        if (first == LS_OK && rc != LS_OK) first = rc;

        rc = toc_serialise(s, &tb, &tn);
        if (rc == LS_OK) {
            memset(sb, 0, sizeof sb);
            memcpy(sb, LS_MAGIC, 8);
            put32(sb + 8,  LS_FMT_VERSION);
            put32(sb + 12, 0);
            put64(sb + 16, s->file_end);
            put64(sb + 24, (uint64_t)s->nrec);
            put64(sb + 32, s->file_end);
            put32(sb + 40, crc32_of(tb, tn));
            rc = ls_pwrite_all(s, tb, tn, s->file_end);
            if (rc == LS_OK) rc = ls_pwrite_all(s, sb, LS_SUPER_SIZE, 0);
            if (rc == LS_OK && fsync(s->fd) != 0) rc = -errno;
        }
        free(tb);
        if (first == LS_OK) first = rc;
    }

    s->teardown = 1;
    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r = s->tab[i];
        while (r) { ls_rec *nx = r->hnext; ls_rec_free(s, r); r = nx; }
    }
    if (s->fd >= 0) close(s->fd);
    if (!keep && s->path) unlink(s->path);

    pthread_mutex_destroy(&s->toc_lk);
    pthread_mutex_destroy(&s->alloc_lk);
    pthread_mutex_destroy(&s->q_lk);
    pthread_cond_destroy(&s->q_cv);
    for (i = 0; i < s->nretired; i++) free(s->retired[i]);
    free(s->retired);
    free(s->fl);
    free(s->tab);
    free(s->path);
    free(s);
    return first;
}
