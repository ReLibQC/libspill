/* libspill POSIX backend -- internal structures. Not installed. */
#ifndef LS_INTERNAL_H
#define LS_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#include "libspill.h"

#define LS_SUPER_SIZE  4096u          /* superblock, and the first data offset */
#define LS_ALIGN       4096u          /* extent alignment; O_DIRECT needs it   */
#define LS_MAGIC       "LIBSPILL"     /* 8 bytes, no NUL                       */
#define LS_FMT_VERSION 1u
#define LS_BOUNCE      (4u << 20)     /* O_DIRECT staging chunk                */
#define LS_MIN_EXTENT  (64u << 10)    /* below this a hole is not worth taking */
#define LS_MAX_EXTENTS 8u             /* scatter-list bound; past it, use the tail */

/* A record's bytes live either wholly in the memory tier or wholly in a list of
 * file extents. Splitting a record across both was considered and rejected: it
 * doubles the read path for a win the budget policy already gets by keeping
 * whole records. */
typedef struct {
    uint64_t foff;                    /* offset in the store file              */
    uint64_t len;                     /* allocated length, LS_ALIGN-rounded    */
} ls_extent;

typedef struct ls_rec {
    char          *key;
    uint64_t       size;              /* logical size the caller sees          */

    unsigned char *mem;               /* non-NULL => resident in memory tier   */
    uint64_t       mem_cap;

    size_t         busy;              /* in-flight ops; eviction skips these   */
    ls_extent     *ext;               /* in logical order; covers >= size      */
    size_t         next, ncap;
    uint64_t       ext_total;

    struct ls_rec *hnext;             /* hash chain                            */
    struct ls_rec *lru_prev, *lru_next;
} ls_rec;

typedef struct { uint64_t foff, len; } ls_hole;

struct ls_store {
    int              fd;
    char            *path;
    ls_opts          o;
    int              direct;          /* O_DIRECT actually in force            */

    /* table of contents. Locked -- DESIGN.md §4b. Never held across I/O. */
    pthread_mutex_t  toc_lk;
    ls_rec         **tab;
    size_t           nbuckets, nrec;

    /* extent allocator: free list sorted by offset, coalescing on release */
    pthread_mutex_t  alloc_lk;
    int              teardown;        /* in ls_close: stop recycling extents   */
    ls_hole         *fl;
    size_t           nfl, flcap;
    uint64_t         file_end;

    /* memory tier */
    size_t           resident;
    ls_rec          *lru_head, *lru_tail;   /* head = most recently used */

    /* async worker pool */
    pthread_t       *thr;
    size_t           nthr;
    pthread_mutex_t  q_lk;
    pthread_cond_t   q_cv;
    struct ls_req   *qh, *qt;         /* pending queue                         */
    struct ls_req   *live;            /* every request not yet reaped          */
    int              stop;
    int              drain_err;       /* first error seen while draining       */
    size_t           inflight;
};

struct ls_req {
    ls_store       *s;
    int             op;               /* LS_OP_READ / LS_OP_WRITE              */
    char            key[LS_KEY_MAX + 1];
    uint64_t        off;
    size_t          n;
    void           *rbuf;
    const void     *wbuf;
    int             status;
    int             done;
    pthread_mutex_t lk;
    pthread_cond_t  cv;
    struct ls_req  *next;             /* pending queue                         */
    struct ls_req  *lnext, *lprev;    /* live list, for release at ls_close    */
};

enum { LS_OP_READ = 0, LS_OP_WRITE = 1 };

/* error.c */
void ls_report(ls_store *s, int err, const char *key, uint64_t off, size_t n,
               const char *msg);

/* alloc.c */
int      ls_alloc_best   (ls_store *s, uint64_t need, ls_extent *out);
int      ls_alloc_largest(ls_store *s, uint64_t least, ls_extent *out);
void     ls_alloc_tail   (ls_store *s, uint64_t need, ls_extent *out);
void     ls_free_extents (ls_store *s, ls_extent *e, size_t n);
uint64_t ls_alloc_live   (ls_store *s);   /* bytes handed out, for the tests */

/* toc.c */
ls_rec *ls_toc_find   (ls_store *s, const char *key);          /* toc_lk held */
ls_rec *ls_toc_insert (ls_store *s, const char *key);          /* toc_lk held */
void    ls_toc_unlink (ls_store *s, ls_rec *r);                /* toc_lk held */
void    ls_rec_free   (ls_store *s, ls_rec *r);
int     ls_key_ok     (const char *key);

/* store.c -- shared by the sync and async paths */
int  ls_rw (ls_store *s, const char *key, uint64_t off, size_t n,
            void *rbuf, const void *wbuf, int op);
int  ls_pread_all  (ls_store *s, void *buf, size_t n, uint64_t off);
int  ls_pwrite_all (ls_store *s, const void *buf, size_t n, uint64_t off);

/* memory tier; both called with toc_lk held */
void ls_lru_touch (ls_store *s, ls_rec *r);
void ls_lru_drop  (ls_store *s, ls_rec *r);
int  ls_evict_to  (ls_store *s, size_t budget, ls_rec *keep);
int  ls_spill_all (ls_store *s);

/* async.c */
int  ls_pool_start(ls_store *s);
void ls_pool_stop (ls_store *s);

#endif /* LS_INTERNAL_H */
