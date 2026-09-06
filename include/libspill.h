/* SPDX-License-Identifier: BSD-3-Clause */
/* libspill -- scratch and out-of-core I/O for electronic-structure codes.
 *
 * This header is the ABI. The C layer exists for reach -- Fortran, Python, and
 * a stable ABI -- not because anyone should enjoy writing against it; see
 * DESIGN.md section 4a for the C++ and Python layers where the ergonomics live.
 *
 * C99. No global state, no initialisation call, no thread-local storage.
 */
#ifndef LIBSPILL_H
#define LIBSPILL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ version
 * LS_VERSION_NUM is the header's version; ls_version() is the linked
 * library's. A caller that cares about the difference should compare them. */
#define LS_VERSION_MAJOR 0
#define LS_VERSION_MINOR 1
#define LS_VERSION_PATCH 0
#define LS_VERSION_NUM   (LS_VERSION_MAJOR * 10000 + LS_VERSION_MINOR * 100 \
                          + LS_VERSION_PATCH)

int         ls_version(void);
const char *ls_version_string(void);

/* ------------------------------------------------------------------- errors
 * Every entry point returns 0 on success and a negative code on failure. The
 * codes occupy two disjoint ranges:
 *
 *   -1 .. -999    an OS errno, negated: -ENOSPC, -EIO, -EACCES, -EDQUOT.
 *   -1000 ..      a libspill condition, enumerated below.
 *
 * There is no errno-like global and no per-store last-error slot. The return
 * value is the entire report, which is what lets every entry point be called
 * concurrently without the caller reasoning about error state. Passing the OS
 * code through unchanged matters most for -ENOSPC, which on a scratch
 * filesystem is a routine event rather than a bug, and which a caller may want
 * to handle by shrinking its block size rather than aborting. */
enum {
    LS_OK          =     0,
    LS_ERR_NOKEY   = -1000,  /* no record under that key                    */
    LS_ERR_RANGE   = -1001,  /* read of off..off+n outside the record       */
    LS_ERR_INVAL   = -1002,  /* NULL, empty or over-long key; bad options   */
    LS_ERR_MODE    = -1003,  /* entry point not valid for this store's mode */
    LS_ERR_BACKEND = -1004,  /* backend not compiled in, or it failed       */
    LS_ERR_BUSY    = -1005,  /* request still in flight                     */
    LS_ERR_CORRUPT = -1006   /* the store's table of contents did not check */
};

/* Describes `err` into buf, returns buf, always NUL-terminates. Takes a caller
 * buffer rather than returning a static string so that it is thread-safe for
 * the negated-errno range too, where the text comes from the C library. */
#define LS_ERRBUF_MIN 128
const char *ls_strerror(int err, char *buf, size_t buflen);

/* ------------------------------------------------------------------ options
 * A store is opened once and keeps its backend, mode and parallel policy for
 * its lifetime. Unimplemented combinations fail at ls_open with a clear code
 * rather than being absent from this header, so that a caller can compile
 * against the full option space and discover at run time what this build
 * supports. */

typedef enum {
    LS_POSIX = 0,   /* default; no dependencies; the only async backend      */
    LS_HDF5  = 1    /* optional at build time; LS_ERR_BACKEND if absent      */
} ls_backend;

typedef enum {
    LS_EXPLICIT = 0,  /* read/write calls                                    */
    LS_MAPPED   = 1   /* ls_map/ls_unmap; implies LS_POSIX; no async         */
} ls_mode;

typedef enum {
    LS_LOCAL    = 0,  /* one store, one process                              */
    LS_PER_RANK = 1,  /* rank folded into the path, following CP2K's HFX     */
    LS_SHARED   = 2   /* one file, many processes, disjoint ranges           */
} ls_parallel;
/* LS_SHARED answers §3a(1), which seven codes asked for -- BigDFT, DFT-FE,
 * MOLGW, Octopus, QE/EPW, yambo and SIESTA all want ranks to write disjoint
 * ranges of one key. It carries no MPI dependency: pwrite already provides
 * everything the mode promises, and the barrier stays in the caller, where
 * MADNESS, SIESTA and Conquest each independently said their fan-out logic has
 * to live anyway.
 *
 * The contract, stated negatively as §3a asks:
 *
 *   - ranges written by different processes must be DISJOINT. Overlap is not
 *     detected and not defined.
 *   - there is no implied ordering between processes.
 *   - a write is visible to another process only after the caller's own
 *     synchronisation. The library performs none.
 *
 * And one requirement §3a does not state, which falls out of the
 * implementation: **the layout is frozen**. Every process must agree on where
 * each record lives, and nothing in this mode negotiates that, so the store
 * must already contain the records. The protocol is: one process creates the
 * store and ls_reserves the keys, closes it with keep=1, the caller barriers,
 * and then every process opens LS_SHARED. In that mode ls_reserve, ls_erase,
 * ls_append and ls_set_attr return LS_ERR_MODE, and a write past a record's
 * existing size does too -- because any of them would allocate space the other
 * processes cannot see.
 *
 * memory_budget must be 0 for the same reason: a per-process memory tier would
 * hold writes where no other process could read them, which is not a
 * performance question but a correctness one.
 *
 * This does not contradict §5a. That settles intra-process concurrency;
 * positional I/O is the primitive here too. */

#define LS_KEY_MAX    255u   /* bytes in a key, excluding the NUL           */
#define LS_ATTR_MAX   256u   /* bytes in a key's attribute blob             */
#define LS_OPTS_VERSION 1u

/* Called on every failure, before the code is returned, with whatever context
 * the failing operation had. This is how a caller gets "-ENOSPC while writing
 * 8 MiB at offset 3.2 GiB of key t2_ampl" without a stateful error API.
 * Optional; may be called from an I/O thread, so the callback must be
 * thread-safe if the store is used from more than one thread. */
typedef void (*ls_log)(int err, const char *key, uint64_t off, size_t nbytes,
                       const char *msg, void *ctx);

typedef struct {
    uint32_t    version;        /* = LS_OPTS_VERSION; ls_opts_default sets it */
    ls_backend  backend;
    ls_mode     mode;
    ls_parallel parallel;
    int         rank;           /* LS_PER_RANK; see note below                */
    size_t      memory_budget;  /* keep up to this many bytes in RAM;
                                   0 = always spill. Must be 0 if LS_MAPPED   */
    const char *dir;            /* NULL => TMPDIR, node-local if available    */
    int         direct_io;      /* bypass the page cache for aligned bulk I/O */
    ls_log      log;
    void       *log_ctx;
} ls_opts;
/* rank: if >= 0 it is used as given. If < 0 the library takes the first of
 * OMPI_COMM_WORLD_RANK, PMI_RANK, PMIX_RANK, SLURM_PROCID present in the
 * environment, and failing all of those uses getpid(). It never links or calls
 * MPI to find out. The getpid() fallback is what keeps two ranks on one node
 * from colliding when the launcher sets nothing.
 *
 * The version field is what makes this struct extensible: new members are added
 * at the end and the version is bumped, so a caller built against an older
 * header keeps working against a newer library. Always obtain an ls_opts from
 * ls_opts_default rather than declaring one and filling it in. */

void ls_opts_default(ls_opts *o);

/* --------------------------------------------------------------- lifecycle */
typedef struct ls_store ls_store;
typedef struct ls_req   ls_req;

/* opts may be NULL, meaning ls_opts_default. err may be NULL. Returns NULL on
 * failure with *err set. `name` identifies the store within `dir`; it is not a
 * path, and the library owns the file layout underneath it. */
ls_store *ls_open(const char *name, const ls_opts *opts, int *err);

/* Drains any request still in flight -- as fclose flushes -- then closes. Frees
 * the store even when it returns an error, so the caller must not close twice.
 * keep = 0 unlinks the backing file, which is the usual case for scratch.
 * Returns the first error met while draining, else the error from closing. */
int ls_close(ls_store *s, int keep);

/* -------------------------------------------------------------------- data
 * off is a byte offset within the named record; a write past the current end
 * extends it, and a write beyond it leaves the gap zero-filled. A read outside
 * the record is LS_ERR_RANGE, never a short read.
 *
 * Short transfers from the OS are retried internally, so these are all-or-
 * nothing to the caller. On failure the affected range holds undefined bytes:
 * nothing is rolled back, and a caller that must distinguish a failed write
 * from a stale one should reserve and rewrite rather than trusting the range.
 *
 * Concurrency, from DESIGN.md 5a: distinct keys are safe from any number of
 * threads, and the POSIX backend uses pread/pwrite so that no file position is
 * shared. Overlapping ranges of one key from two threads are the caller's
 * problem -- the library takes no data lock, because one would make it slower
 * than the layers it replaces while claiming to be faster. */
int ls_write(ls_store *s, const char *key, uint64_t off, size_t n,
             const void *buf);
int ls_read (ls_store *s, const char *key, uint64_t off, size_t n,
             void *buf);

/* --------------------------------------------------------- table of contents
 * Unlike the data path, the table of contents IS internally serialised: these
 * calls are safe against each other and against concurrent I/O on other keys.
 * The lock is never held across an I/O operation, so it costs nothing that
 * matters, and without it the distinct-keys guarantee above could not hold --
 * a first write creates a key, which mutates the table. */
int ls_exists(ls_store *s, const char *key, int *found);
int ls_size  (ls_store *s, const char *key, uint64_t *nbytes);
int ls_erase (ls_store *s, const char *key);

/* Preallocates nbytes for `key`, extending or creating it. Two purposes: it
 * moves -ENOSPC to a point where the caller can still do something about it,
 * instead of the middle of a contraction an hour in, and it gives the backend
 * one extent to write into rather than growing the file per record (DESIGN.md
 * 5.4). The contents of newly reserved space are zero. Never shrinks. */
int ls_reserve(ls_store *s, const char *key, uint64_t nbytes);

/* Snapshot of the keys present at the moment of the call, owned by the caller
 * and released with ls_keys_free. It is a snapshot rather than a view into the
 * store precisely because another thread may create or erase a key; there is
 * no window in which the returned pointers can go stale. */
int  ls_keys(ls_store *s, char ***keys, size_t *n);
void ls_keys_free(char **keys, size_t n);

/* ------------------------------------------------------------------- async
 * The reason the library exists. Available on LS_POSIX only: DESIGN.md 7b
 * measured HDF5's global lock and the stock build's heap corruption, so the
 * HDF5 backend returns LS_ERR_MODE here rather than pretending.
 *
 * buf must stay valid and, for aread, untouched until ls_wait returns. The
 * request owns nothing else.
 *
 * ls_wait completes the request, releases it, and returns the operation's
 * status; the handle is dead afterwards. ls_test only reports completion --
 * a request that tests done must still be waited on to be reaped. */
int ls_awrite(ls_store *s, const char *key, uint64_t off, size_t n,
              const void *buf, ls_req **req);
int ls_aread (ls_store *s, const char *key, uint64_t off, size_t n,
              void *buf, ls_req **req);
int ls_wait  (ls_req *req);
int ls_test  (ls_req *req, int *done);

/* ------------------------------------------------------------------ append
 * Writes at the key's current end and reports where that was. The offset is
 * taken and the record extended under one lock, so two threads appending to one
 * key get two disjoint ranges rather than a race -- which a caller cannot build
 * out of ls_size plus ls_write.
 *
 * §3a(2): OpenMolcas's iDisk is "an opaque running cursor: the caller does not
 * supply an independently-computed offset -- it obtains one from a prior call's
 * output and the callee auto-advances", under all 2200 of its call sites. Also
 * APE's record framing and ABINIT's row-by-row growth. */
int ls_append(ls_store *s, const char *key, size_t n, const void *buf,
              uint64_t *off_out);

/* ------------------------------------------------------------ vectored I/O
 * One call, one table-of-contents lookup, many ranges of one record. Segments
 * whose file offsets turn out to be adjacent are issued as a single preadv or
 * pwritev; the rest go one at a time.
 *
 * §3a(4): Serenity needs "in-place strided partial update without either
 * read-modify-write (defeats the purpose -- these are large vectors) or the
 * caller buffering the whole vector", and Conquest performs "thousands of
 * individual scalar operations per file instead of one bulk record". A segment
 * list expresses both, and a regular stride is just a segment list with a
 * regular offset -- which is why this supersedes the ls_write_strided sketched
 * in §4 rather than joining it. */
typedef struct {
    uint64_t off;        /* offset within the record */
    size_t   len;
    void    *buf;        /* read: destination. write: source, not modified */
} ls_seg;

int ls_readv (ls_store *s, const char *key, const ls_seg *segs, size_t nseg);
int ls_writev(ls_store *s, const char *key, const ls_seg *segs, size_t nseg);

/* ---------------------------------------------------------------- mapping
 * Valid only on a store opened LS_MAPPED (§3). On such a store the
 * asynchronous entry points and ls_accumulate return LS_ERR_MODE rather than
 * pretending: you cannot prefetch a page fault, and an interface that silently
 * degraded ls_aread to a synchronous touch would be lying about the one
 * property it exists to provide. ls_read and ls_write do remain available, as a
 * copy through the mapping, so that a port can move call sites over gradually.
 *
 * `len` receives the record's logical size. The mapping itself is rounded up to
 * whole pages, so writing past `len` may touch bytes the store does not
 * consider part of the record.
 *
 * Errors under a mapping arrive as SIGBUS, in the caller's instruction stream
 * rather than as a return code. The library documents this and does not attempt
 * to hide it. A store closes any mapping it still holds.
 */
int ls_map  (ls_store *s, const char *key, void **addr, size_t *len);
int ls_unmap(ls_store *s, const char *key);

/* ------------------------------------------------------------- accumulate
 * Read-modify-write: buf is combined into what is stored, by the CALLER'S
 * arithmetic. Taken from NWChem's TCE, whose add_block sits alongside
 * get_block/put_block; contraction kernels accumulate partial results into a
 * target, and without this the caller must read into a temporary, add, and
 * write back -- three operations, an extra buffer, and no chance for the store
 * to do it in the memory tier without touching disk at all.
 *
 * The reduction is supplied rather than selected by an enum, because knowing
 * the element type is only necessary if the store does the addition, and having
 * it do so would break the one invariant everything else here preserves: that
 * the store moves opaque bytes and never learns what a block is. Size alone
 * genuinely is not enough -- eight bytes may be a double, an int64 or two
 * floats -- but the caller already knows, so the caller says.
 *
 * Cost is one indirect call per BLOCK, not per element.
 *
 * Concurrency is §5a's, unchanged: this is a read-modify-write, and two
 * concurrent accumulations into the same range would lose one update. That is
 * the caller's to prevent. The surveyed codes settle it by isolation rather
 * than locking, and a library that serialised here would be slower than the
 * layers it replaces while claiming to be faster. */
typedef void (*ls_reduce)(void *dst, const void *src, size_t nbytes, void *ctx);

int ls_accumulate(ls_store *s, const char *key, uint64_t off, size_t nbytes,
                  const void *buf, ls_reduce op, void *ctx);

/* Supplied reductions for the overwhelmingly common cases, so that callers --
 * especially Fortran ones -- rarely write their own. ctx is NULL, or a pointer
 * to a scale factor of the matching type: dst += *alpha * src. */
void ls_add_f64(void *dst, const void *src, size_t nbytes, void *ctx);
void ls_add_f32(void *dst, const void *src, size_t nbytes, void *ctx);

/* -------------------------------------------------------------- attributes
 * A bounded opaque blob beside a key, capped at LS_ATTR_MAX and never
 * interpreted. §3a(3): ERKALE, VeloxChem, ChronusQ, yambo and MRChem all want
 * to keep shape and dtype next to the bytes.
 *
 * This is not self-description and must not become it. The store defines no
 * schema, does not parse the blob, and will not grow one that outlives the
 * cap -- §3's refusal to know what a tensor is survives exactly because the
 * concession is bounded and opaque.
 *
 * ls_get_attr takes *n as the caller's capacity and sets it to the length
 * stored. A short buffer gets LS_ERR_RANGE with *n set to what was needed; a
 * NULL buffer just reports the length. */
int ls_set_attr(ls_store *s, const char *key, const void *blob, size_t n);
int ls_get_attr(ls_store *s, const char *key,       void *blob, size_t *n);

/* Not in this version, and shaped here so that adding them stays ABI-compatible
 * (new functions, new enum values, new trailing ls_opts members):
 *
   ls_aaccumulate          the asynchronous form; no consumer has asked, and
 *                           the reduction would run on an I/O thread, which is
 *                           a contract worth stating before offering.
 *   ls_read_strided,         superseded by ls_readv/ls_writev above: a regular
 *   ls_write_strided         stride is a segment list with a regular offset,
 *                            and the segment list also serves the scatter cases
 *                            the strided pair could not.
 *   ls_prefetch              a hint, meaningless until there is a cache to
 *                            prefetch into.
 */

#ifdef __cplusplus
}   /* extern "C" */
#endif
#endif /* LIBSPILL_H */
