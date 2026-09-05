# libscratch — design sketch

**Start here.** Every design question is settled; what remains is implementation
and one measurement. The ABI-level consequences of those decisions — error
model, failure semantics, ownership, locking — are settled separately in §4b,
which also corrects two defects in the §4 sketch. Read §1 (criteria), §3 (scope), §4 + §4a (API and bindings),
§5a (threading). §§7a–7e record *why* each alternative was rejected — read them
before reopening a settled question, because each was closed by a test in
`tests/`, not by opinion, and the tests are runnable.

The one thing not settled, and not settleable on paper: **does asynchronous
overlap actually win on a real out-of-core workload?** That is success criterion
2 (§1) and it decides whether the project is worth finishing. Cheapest route to
an answer is the abacus port (~6 call sites, §6), which also tests whether the
API survives contact with a caller before either lead adopter spends anything.

Evidence base: the 53-code survey in `/home/work/libxcsurvey` — `scan_io.py`
(dispersion), the scratch-I/O addendum in each `SUBLIBRARY_MANIFEST.md`, and the
dedicated `SCRATCH_IO_SURVEY.md` per code. Quotations in this document cite those.

A reusable scratch and out-of-core I/O library for electronic-structure codes.
C99 core, C/C++/Fortran/Python bindings, permissive licence.

## 1. Success criteria (these define the project)

1. **Deprecate the legacy layer in 3–5 codes.** Not "be adoptable" — actually
   delete the in-house implementation in at least three codes, with their
   maintainers' agreement, and have their test suites pass.
2. **Measurable performance gain** on a real out-of-core workload, not a
   microbenchmark. If it is only as fast, it is not worth anyone's port.

Both are falsifiable, and the second is the harder one. A thin wrapper over the
same syscalls will be *slower*. The gain has to come from doing something the
legacy layers do not (§5).

## 2. Why this component and not another

The survey found the boundary already drawn, independently, in the same place:
~12 codes have a wholly generic keyed store with all layout knowledge in the
callers; ~25 more draw the split inside the layer; only ~6 are layout-specific
throughout. Nobody has to be persuaded where the interface goes.

It was never extracted because the generic half is small enough that no single
project found it worth factoring out, and common enough that all of them wrote
it. That is an argument for a shared component, not against one.

## 3. Scope, and what it refuses

IN: temporary, high-volume, performance-critical data with a lifetime shorter
than the job, accessed repeatedly during the run — integrals, amplitudes,
per-k-point quantities, intermediate tensors.

OUT: durable, portable, self-describing output, including checkpoint and restart.
That is HDF5's and TREXIO's job. A scratch layer that also becomes an archival
format acquires the weight that kept every existing one unshared. **The thinness
is the whole design.**

**The dedicated scratch-I/O survey confirms this boundary is real, and shrinks
the audience.** Of 34 codes surveyed so far:

| what the code actually does with disk | codes |
|---|---|
| **out-of-core paging** — random or repeated access during the run | **10** |
| checkpoint / restart — write-once whole-object, read only on restart | 11 |
| nothing — no spilling at all | 13 |

The checkpoint group is not a missed audience. It is durable wavefunction
storage, which TREXIO already addresses, and admitting it would turn this into a
different library competing with HDF5 on its own ground. The surveys draw the
line in the design's own words: one code is "write-once-at-exit /
read-once-at-entry, entirely absent during the iterative solve"; another is
"checkpoint-and-restart, not scratch-and-page".

The ten that do page fall into two coherent workloads, which is what the API must
serve:

1. **Per-k-point records in periodic codes** — Elk ("write-once per k-point,
   read-back-many-times-in-place via direct access"), exciting
   ("random-access-by-key" keyed by k-point, q-point, (q,k) pair or R-vector),
   FLEUR ("read-back-many-times-within-the-iteration, whole-k-point-and-spin
   granularity"), CP2K's HFX, CPMD, DFT-FE.
2. **Word-addressed vector and symmetry-block access in the Gaussian correlated
   lineage** — Dalton ("random-access, word-addressed get/put of whole vectors or
   symmetry-blocks, re-fetched"), LSDalton, and DIRAC's "federation of at least
   five independently-invented" layers.

**Audience risk, stated plainly.** Ten of thirty-four, and the trend may be
against us: gpu4pyscf spills nothing to disk (GPU overflow goes to pinned host
RAM), and ChronusQ keeps integrals, amplitudes and Davidson vectors distributed
in-core through TiledArray. If more memory and better distribution are the
field's answer to out-of-core work, this library targets a shrinking problem.
That is a reason to keep it small and to insist on the deprecation criterion, not
necessarily a reason not to build it.

Also out: knowing what a tensor is. The store moves opaque bytes.

**Memory-mapped access: supported, as an explicit second mode.** An earlier
draft excluded mapping outright. That was wrong, and the survey is why: two codes
use it for real work — RMG holds scratch mapped for the life of a geometry step,
and qp2 maps its AO/MO two-electron integral cache through its own generic
wrapper (`src/utils/mmap.f90` over `map_module.f90`) — and for their access
patterns it is the right mechanism, not a shortcut.

The two mechanisms suit different patterns, and both patterns are present:

| | mapping wins | read/write wins |
|---|---|---|
| access | sparse touches of a large record; hot subset re-read | sequential streaming of large blocks |
| caching | kernel page cache, global eviction, free | explicit buffers, our policy |
| accumulate | `p[i] += x`, no round trip | read-modify-write |
| overlap | impossible — a fault cannot be issued ahead | the entire point of `ls_aread` |
| errors | `SIGBUS` | `errno` |
| filesystem | poor on Lustre/GPFS | fine everywhere |
| in corpus | qp2 integral cache, RMG | Psi4 `libdpd` blocks, PySCF block streams, NWChem tiles |

**Backend and mode are different kinds of choice, and the distinction matters to
callers.** A *backend* (POSIX or HDF5) is interchangeable: the caller writes the
same calls either way and switches by editing one enum. A *mode* is not. Choosing
`LS_MAPPED` changes which functions exist — the caller indexes a pointer instead
of calling reads, and no flag converts one style into the other. qp2 indexes;
PySCF calls reads; neither becomes the other without rewriting its access sites.
A store is one mode for its lifetime, though a program may open several stores
and use both.

The two axes are also not independent: **`LS_MAPPED` implies the POSIX
backend**, since mapping an HDF5 dataset is not meaningful. There is no 2x2
matrix here, and an implementer should not build one.

Mapping is therefore an opt-in mode chosen at `ls_open`, exposing one extra pair
of calls:

```c
int ls_map  (ls_store *s, const char *key, void **addr, size_t *len);
int ls_unmap(ls_store *s, const char *key);
```

valid only on a store opened `LS_MAPPED`. On such a store the asynchronous
entry points are **not available**, and the library says so rather than
pretending otherwise: you cannot prefetch a page fault, and an interface that
silently degrades `ls_aread` to a synchronous touch would be lying about the one
property it exists to provide. Errors under a mapping arrive as signals, which
the library documents and does not attempt to hide.

The cost of carrying both is smaller than it first appears: mapping is a modest
addition over the POSIX backend, where the hard, valuable work — asynchrony,
the memory tier, space management under churn — lives entirely on the explicit
path. What must be resisted is letting the two modes acquire separate feature
sets beyond this one difference.

## 4. API sketch (C core)

Opaque handles; no global unit registry; thread-safe; no init call.

```c
typedef struct ls_store ls_store;
typedef struct ls_req   ls_req;      /* async request handle */

/* policy chosen at open, not baked into the API. Backend and mode are part of
   it: an earlier draft discussed both in prose (§3, §7b) but left neither in the
   struct, so there was no way to ask for either. §4b settles the rest. */
typedef enum { LS_POSIX, LS_HDF5 }      ls_backend;
typedef enum { LS_EXPLICIT, LS_MAPPED } ls_mode;
typedef enum { LS_LOCAL, LS_PER_RANK }  ls_parallel;
typedef struct {
  uint32_t    version;         /* = LS_OPTS_VERSION; from ls_opts_default    */
  ls_backend  backend;
  ls_mode     mode;
  ls_parallel parallel;
  int         rank;            /* LS_PER_RANK; <0 => environment, else pid   */
  size_t      memory_budget;   /* stay in RAM below this; 0 = always spill   */
  const char *dir;             /* NULL => TMPDIR, node-local if available    */
  int         direct_io;       /* bypass page cache for large aligned writes */
  ls_log      log;             /* optional; called on every failure          */
  void       *log_ctx;
} ls_opts;
void ls_opts_default(ls_opts *o);

ls_store *ls_open (const char *name, const ls_opts *opts, int *err);
int       ls_close(ls_store *s, int keep);          /* keep=0 => unlink */

/* NAMED byte ranges + offset within the named record. Key space settled by the
   two lead adopters, which independently chose the same shape: Psi4's libpsio
   takes `const char *key` with a {page, offset} address and an 80-char table of
   contents; OpenMolcas's RunFile uses a 16-char Label in an in-memory ToC
   dereferenced to a word offset. Integer keys would have made both ports
   non-mechanical. The store never learns what a block is. */
int ls_write(ls_store *s, const char *key, uint64_t off, size_t n, const void *buf);
int ls_read (ls_store *s, const char *key, uint64_t off, size_t n,       void *buf);

/* table of contents — both adopters maintain one; it belongs in the library */
int ls_exists(ls_store *s, const char *key, int *found);
int ls_size  (ls_store *s, const char *key, uint64_t *nbytes);
int ls_erase (ls_store *s, const char *key);
int ls_reserve(ls_store *s, const char *key, uint64_t nbytes);  /* preallocate */
int  ls_keys(ls_store *s, char ***keys, size_t *n);      /* snapshot, caller-owned */
void ls_keys_free(char **keys, size_t n);

/* ACCUMULATE: read-modify-write, buf combined into what is stored.
   Taken from NWChem's TCE, whose `add_block` sits alongside get_block/put_block
   in the seam its own survey identifies as "already implement[ing] the seam a
   shared library would replace". Contraction kernels accumulate partial results
   into a target; without this the caller must read into a temporary, add, and
   write back — three operations, an extra buffer, and no chance for the library
   to do it in the memory tier without touching disk at all.

   The arithmetic is the CALLER'S, supplied as a reduction over whole buffers.
   A typed enum was drafted first and rejected: knowing the element type is only
   necessary if the store does the addition, and having it do so would break the
   one invariant everything else here preserves — that the store moves opaque
   bytes and never learns what a block is. Size alone genuinely is not enough
   (eight bytes may be a double, an int64, or two floats, and each adds
   differently), but the caller already knows, so let the caller say.

   Cost: one indirect call per BLOCK, not per element. Fortran callers need a
   bind(c) procedure; ls_add_f64 below covers the common case so that most never
   write one. */
typedef void (*ls_reduce)(void *dst, const void *src, size_t nbytes, void *ctx);

int ls_accumulate (ls_store *s, const char *key, uint64_t off, size_t nbytes,
                   const void *buf, ls_reduce op, void *ctx);
int ls_aaccumulate(ls_store *s, const char *key, uint64_t off, size_t nbytes,
                   const void *buf, ls_reduce op, void *ctx, ls_req **req);

/* supplied reductions for the overwhelmingly common cases, so that callers --
   especially Fortran ones -- rarely write their own */
void ls_add_f64 (void *dst, const void *src, size_t nbytes, void *ctx); /* ctx: NULL or *alpha */
void ls_add_f32 (void *dst, const void *src, size_t nbytes, void *ctx);

/* strided: express "block (i,j)" without the store modelling tensors */
int ls_write_strided(ls_store *s, const char *key, uint64_t off,
                     size_t elem, size_t count, ptrdiff_t stride, const void *buf);

/* asynchronous — the reason the library exists */
int ls_awrite(ls_store *s, const char *key, uint64_t off, size_t n,
              const void *buf, ls_req **req);
int ls_aread (ls_store *s, const char *key, uint64_t off, size_t n,
                    void *buf, ls_req **req);
int ls_wait  (ls_req *req);
int ls_test  (ls_req *req, int *done);

/* hint the access pattern; the store may prefetch */
int ls_prefetch(ls_store *s, const char *key, uint64_t off, size_t n);
```

Fortran binding via `ISO_C_BINDING`, Python via the same C ABI.
Layout-aware typed views belong in an **optional header-only C++ layer above**
this, never inside it — templating the core would forfeit the Fortran and Python
reach that makes the component worth building.

## 4a. The C core is the ABI, not the API

The C layer exists for reach — Fortran, Python, and a stable ABI — not because
anyone should enjoy writing against it. Two thin bindings sit above it, and they
are where the ergonomics live.

**C++ (header-only).** RAII, spans, exceptions, and — the point of the exercise —
a typed `accumulate` that supplies the reduction from `T`, so a C++ caller never
writes a callback:

```cpp
ls::Store s{"scratch", {.memory_budget = 8ull<<30}};      // throws on failure
s.write("t2", off, std::span{amps});                       // typed, size deduced
s.accumulate<double>("t2", off, std::span{partial}, 0.5);  // reduction chosen by T
auto req = s.awrite("t2", off, std::span{next});           // move-only; waits in dtor
req.wait();
for (auto &k : s.keys()) ...
```

**Python.** Context manager, NumPy throughout, and reads into a caller-supplied
array when the caller wants to control allocation:

```python
with libscratch.open("scratch", memory_budget=8<<30) as s:
    s["t2"] = amps                                  # whole record
    blk = s.read("t2", offset=o, shape=(n, m))      # -> ndarray
    s.read("t2", offset=o, out=buf)                 # no allocation
    s.accumulate("t2", o, partial, alpha=0.5)
    fut = s.awrite("t2", o, nxt); fut.wait()
    list(s)                                         # table of contents
```

`LS_MAPPED` is where the Python binding earns the most: a mapped record is
naturally a NumPy array backed by the mapping, so `arr = s.map("eri")` gives
array semantics with no copy at all — which is precisely qp2's access pattern,
expressed in one line.

**Fortran** gets `ISO_C_BINDING` wrappers plus the supplied reductions
(`ls_add_f64`), so that Fortran callers also never write a callback in practice.

The C entry points remain public and supported — Fortran needs them and ABI
stability is the point — but no C++ or Python consumer should have to touch
them.

## 4b. The ABI contract

§4 sketches the calls; this settles the parts a second implementer would
otherwise have to guess, and which cannot be changed later without breaking
every consumer. `include/libscratch.h` is the normative form. Six questions were
open, and two of them were defects rather than omissions.

**Errors are negated errno, with our own codes below -1000.** No global error
variable and no per-store last-error slot: the return value is the whole report,
which is what lets every entry point be called concurrently without the caller
reasoning about shared error state. Passing the OS code through unchanged
matters most for `-ENOSPC`, which on a scratch filesystem is a routine event and
not a bug — a caller may reasonably respond by shrinking its block size rather
than aborting the run. `ls_strerror` takes a caller buffer instead of returning
a static string, so that it stays thread-safe across the errno range too.

**`ENOSPC` and partial transfers.** Short transfers from the OS are retried
internally, so the data calls are all-or-nothing to the caller. On failure the
affected range holds undefined bytes; nothing is rolled back. `ls_reserve`
exists so that a full filesystem is discovered where the caller can still act on
it rather than an hour into a contraction, and it doubles as the preallocation
that §5.4 counts on. The context a diagnostic needs — key, offset, length —
reaches the caller through the optional `ls_log` callback rather than a stateful
error API.

**`LS_SHARED_MPIIO` is removed.** It had no communicator anywhere in `ls_opts`,
and adding one would put MPI in the dependency set of a library whose default
backend has none. §5a already settles cross-process sharing as out of scope.
`LS_PER_RANK` takes an explicit rank; if the caller passes a negative one the
library reads `OMPI_COMM_WORLD_RANK`, `PMI_RANK`, `PMIX_RANK` or `SLURM_PROCID`
and otherwise falls back to the pid, which is what keeps two ranks on a node
from colliding when the launcher sets nothing. It never links MPI to find out.

**`ls_keys` returns a caller-owned snapshot**, released with `ls_keys_free`,
rather than a library-owned view. A view would have had to specify how long it
stays valid, and under the concurrency §5a promises the honest answer is "until
any other thread writes a new key" — which is not a usable contract. Copying a
few hundred short strings costs nothing against the I/O this library exists for.

**The table of contents is locked, even though the data path is not.** This
corrects §5a rather than extending it: "concurrent operations on distinct keys
are safe" cannot hold without it, because a first write to a key creates that
key, and creation mutates a structure every other thread reads. The lock is
never held across an I/O operation, so it does not serialise the async layer —
the objection §5a raises against NWChem's global lock does not apply to a lock
that is only ever held for a table update. No data-range lock is added.

**Undefined mode combinations now fail rather than being undefined.**
`LS_MAPPED` requires `LS_POSIX` and `memory_budget == 0` — a mapped store's
cache is the kernel's page cache, and a second budget on top of it means
nothing — with `LS_ERR_INVAL` at `ls_open` for either violation. On a mapped
store `ls_accumulate` and the asynchronous calls return `LS_ERR_MODE`, for the
reason §3 already gives: you cannot prefetch a page fault, and `p[i] += x` is
the caller's own accumulate. `ls_read` and `ls_write` do remain available on a
mapped store, defined as a copy through the mapping; that is not a mode
conversion but a porting aid, and it costs one `memcpy` to provide.

**Two conventions that are ABI whether or not anyone writes them down.**
`ls_opts` carries a `version` as its first member so that later options can be
appended without breaking a caller compiled against an older header; obtain one
from `ls_opts_default`, never by declaring and filling. Keys are opaque byte
strings up to `LS_KEY_MAX` = 255, with no character restrictions, which is a
consequence of the POSIX backend being **one file per store** — a heap with its
own table of contents and extent free list, not a directory of files per key.
That is the same choice that makes the free-list reuse of §7b ours to control,
and it sidesteps HDF5's many-small-datasets risk rather than reproducing it.

**Deferred, with their shapes fixed so that adding them stays compatible:**
`ls_map`/`ls_unmap`, `ls_accumulate`, `ls_prefetch`, and a *symmetric*
`ls_read_strided`/`ls_write_strided` pair — the sketch had only the write half.
There is no asynchronous strided form; nothing in the corpus asks for both at
once. `LS_HDF5` and `LS_MAPPED` are declared in the enums but rejected at
`ls_open` with `LS_ERR_BACKEND` and `LS_ERR_MODE` until they exist, so a caller
can compile against the whole option space and discover at run time what a given
build actually supports.

## 5a. Threading and concurrency: isolation, not locking

The one operation that could require atomicity is `ls_accumulate`, since it is a
read-modify-write and two concurrent accumulations into the same range would lose
one update. The surveyed codes settle how to handle this, and they settle it
against locking.

**CP2K's HFX gives every (MPI rank x OpenMP thread x repetition) its own private
file**, with all three indices baked into the filename; there is no shared file
and no collective I/O in that path at all. Concurrent accumulation into one range
cannot arise. **NWChem takes the opposite route**, wrapping `get_block`,
`put_block` and `add_block` in a single hard-coded global lock — which is
atomicity, and which serialises every block operation in the program. That is the
same defect as HDF5's global lock (§7b) and would defeat the asynchronous layer
this library exists to provide. **Psi4** is single-process and
single-writer-per-unit, with no locking in `libpsio` whatever. **OpenMolcas's**
core direct-access primitives are not thread-safe — a shared module-level
position array, unguarded across some 672 call sites — but it already ships a
thread-safe alternative pair built on `pread`/`pwrite`.

That last point is the design, handed over: **positional I/O is the thread-safe
primitive**, because `pread` and `pwrite` carry their own offsets and share no
file position. The POSIX backend uses them regardless.

The contract is therefore:

1. **Concurrent operations on distinct keys are safe**, including from multiple
   threads. This is required by the asynchronous layer and costs nothing beyond
   positional I/O.
2. **Concurrent operations on the same key and overlapping range are the
   caller's responsibility**, and the library says so. No per-key locks, no
   global lock. A library that serialised here would be slower than the layers
   it replaces while claiming to be faster.
3. **Across processes, isolation rather than sharing**: `LS_PER_RANK` follows
   CP2K, and cross-rank accumulation into a shared range is out of scope. If a
   consumer ever needs it, it belongs in that consumer's parallel runtime — which
   already has the collective machinery — not in a scratch layer.

No atomicity is implemented, because no surveyed code needs it: the ones that do
concurrent work isolate, and the one that locks pays for it globally.

## 5. Where the performance gain comes from

Honestly, and in expected order of magnitude:

1. **Asynchronous overlap.** Most legacy layers are synchronous; callers that
   want double-buffering hand-roll it, and most do not. Out-of-core correlated
   methods are the canonical case where transfer can hide behind compute.
2. **The in-memory tier.** Many jobs spill to disk when the data would have fit
   in RAM, because the layer has no concept of a budget. `memory_budget` makes
   the same call path zero-copy when it fits — likely the largest practical win
   on modern nodes.
3. **Node-local staging.** On clusters the default scratch is often a shared
   parallel filesystem; local NVMe is far faster for per-rank temporaries.
4. **Alignment and preallocation.** `pread`/`pwrite` against preallocated
   extents, optionally `O_DIRECT`, avoids per-record overhead and double
   buffering that Fortran direct-access records incur.

## 6. Target adopters, ranked by porting cost

From the survey (module LOC; binding-site counts are code-wide):

**Primary deprecation targets — Psi4 and OpenMolcas.** Both are already keyed
named-record stores, so the API matches without contortion, and both projects
have an established willingness to outsource work to reusable components, which
matters more than any technical consideration: the criterion is *deprecation*,
and that needs a maintainer who wants it.

| code       | module                     | LOC   | role |
|------------|----------------------------|-------|------|
| psi4       | `libpsio`                  | 4.3k  | primary. Named keys + `{page, offset}`, 64 KiB pages, linked-list ToC — the canonical match |
| openmolcas | `io_util` + `runfile_util` | 12k   | primary. 16-char Label → in-memory ToC → word offset; independent convergence on the same shape |
| abacus     | `Binstream`                | 4.3k  | API shakedown *if* they are willing. Schema-agnostic, layout in ~6 call sites — the cheapest port available, but receptivity unknown |
| yambo      | `Yio` (IO_m/IO_int)        | 2.8k  | technical fit only; receptivity unknown, netCDF-backed |
| eT         | *(surveys pending)*        | ?     | likely candidate — a modern Fortran coupled-cluster code is exactly the out-of-core workload this targets |

abacus and yambo were selected on technical fit alone. Neither has been sounded
out, and for a project whose success criterion is *deprecation* rather than
adoption, willingness outranks fit: a cheap port nobody wants is worth less than
an expensive one somebody has asked for.

**Not a target: ERKALE.** Its checkpoints are portable HDF5 for long-term
wavefunction storage — durable output, which §3 explicitly excludes. The
enumeration tagged it `scratch_io`; that tag is wrong, and the distinction is
exactly the one this library must not blur.

Avoid as first targets: conquest (`io_module`, 128k LOC — the whole I/O hub),
fleur (641 binding sites), QE/EPW (1553), nwchem (vendored 4.4BSD hash db).

## 7. Validation plan

- Correctness: byte-exact round-trip against each adopter's existing layer,
  then their own test suites.
- Performance: one I/O-bound workload per adopter — out-of-core DF-CCSD(T) or
  disk-based MP2 — wall-clock before and after, on both a shared filesystem and
  node-local NVMe. Publish the negative results too.
- The API is wrong if a port needs more than a mechanical translation of call
  sites. Measure that: count the lines changed per adopter.

## 7a. Why not just use ADIOS2?

The right question, and the project's own policy is to adopt before inventing.
ADIOS2 is Apache-2.0, buildable with minimal dependencies, and ships C, C++,
Fortran and Python bindings — on paper an ideal candidate.

It does not fit, for a structural reason rather than a quality one. **ADIOS2's
data model is step-based and append-only**: variables are `Put` within a
`BeginStep`/`EndStep` pair, an engine is opened for writing *or* reading rather
than both, and there is no read-modify-write within an open write session.

Scratch I/O in these codes is the opposite pattern — a heap living inside a
single job. A record is written, read back later in the same run, overwritten,
and read again at a different offset: DIIS error vectors, integral batches,
coupled-cluster amplitudes updated every iteration. Psi4's `libpsio` and
OpenMolcas's RunFile are both used this way. ADIOS2 cannot express it.

This was verified experimentally, not taken from the manual
(`tests/adios_rmw_test.py`, `tests/adios_growth.py`, ADIOS2 2.12.1):

| test | result |
|---|---|
| read a variable back inside one open write session | **fails** — `CheckOpenModes: Engine open mode not valid` |
| overwrite a record in place | **fails** — produces a second *step*, not an update |
| partial sub-range update of an existing record | **fails** — does not merge into the record |
| close-and-reopen workaround | works; 20 cycles on an 8 KiB record in 0.022 s |

The workaround functions, so the fatal objection is not latency but storage.
Because writes append, **a scratch file grows linearly with the number of
updates**: 31 overwrites of a 1 MiB record leave a 31 MiB file, exactly 31x the
live data, with no compaction. A coupled-cluster run holding 10 GB of amplitudes
across 50 iterations would write 500 GB of scratch for 10 GB of live data. That
disqualifies it for this role however fast it is.

None of which is a criticism. ADIOS2 is built for output, checkpointing and
in-situ or streaming coupling — exactly the *durable* half that §3 excludes, and
append-only is the right model for that job. The two are complementary: a code
could reasonably use libscratch for within-run temporaries and ADIOS2 or HDF5
for everything it keeps.

## 7b. HDF5: right semantics, wrong space behaviour — an optional backend

The same experiment was put to HDF5 (`tests/hdf5_scratch_test.py`, HDF5 2.0.0
via h5py 3.16):

| test | ADIOS2 | HDF5 |
|---|---|---|
| read back inside one open write session | fail | **pass** |
| in-place overwrite of a record | fail | **pass** |
| partial sub-range update | fail | **pass** |
| storage after 30 overwrites of a 1 MiB record | 31 MiB (x31) | **1.0 MiB (x1.00)** |
| 1000 read-modify-write cycles over 50 records | — | **0.31 s, file x1.02 live** |
| extendable record, size unknown up front | — | **pass** |

**But HDF5 balloons under churn.** The tests above only overwrote the *same*
dataset. A scratch heap also creates and destroys records — integral batches,
amplitude blocks, DIIS history rolling off. Retested with delete-and-recreate
(`tests/hdf5_churn_varsize.py`), 60 cycles over 8 records:

| record sizes | default | `fs_strategy='fsm'` | `fs_strategy='page'` |
|---|---|---|---|
| all equal | x1.00 | x1.00 | x1.00 |
| **varying** | **x1.61** | **x1.75** | **x1.37** |

Equal-size churn reuses the hole exactly; varying sizes fragment and the file
grows, and free-space management does not fix it. This is the long-standing
HDF5 behaviour and it disqualifies HDF5 as the *only* backend, though not as an
optional one.

**Hence two backends.** A POSIX backend is the default and carries no
dependencies at all — extent allocation and free-list reuse are ours to control,
which is exactly the part HDF5 gets wrong for this workload. HDF5 is compiled in
optionally, for codes that already link it and want their scratch files
inspectable with standard tooling. The API in §4 is the same either way; the
backend is chosen at `ls_open`.

**What HDF5 does give**, and why it is worth supporting: named datasets give the table of contents, hyperslab
selections give strided sub-block access, `H5F_ACC_RDWR` gives read-modify-write,
and space is reused rather than appended. It also already ships C, Fortran and
Python bindings, and most of the target codes already link it.

So libscratch should not be a storage implementation. What remains to build is
comparatively small:

1. **A narrow API.** HDF5's surface is large and its defaults are wrong for this
   use; the libpsio-shaped set in §4 is about eight functions. Most of the value
   is in what the facade *refuses* to expose.
2. **Asynchronous overlap.** HDF5's own API is synchronous; the async VOL
   connector is an add-on requiring Argobots. This is where the performance
   claim has to be earned, and it is now the main reason the library exists.
3. **The memory tier.** `H5FD_CORE` gives an in-memory file behind the same API,
   so a `memory_budget` policy may be nearly free — verify.
4. **Chunking defaults** tuned to the access patterns these codes actually have,
   and node-local staging as a path policy.
5. **Migration shims** so the Psi4 and OpenMolcas ports stay mechanical.

**Thread safety: measured, and worse than expected** (`tests/hdf5_thread_test.c`,
HDF5 1.14.6 as shipped by Fedora). `H5_HAVE_THREADSAFE` is **not** defined in the
stock distribution build, which is what most users will have. Concurrent
hyperslab reads from a shared dataset:

| threads | result |
|---|---|
| 1 | completes cleanly (HDF5 0.005 s, POSIX pread 0.003 s) |
| 2 | `free(): double free detected in tcache 2` |
| 4 | crash before any output |

Single-threaded completion confirms the test itself is correct, so this is
concurrency, not a test bug. The two regimes are therefore:

- **stock build (no `--enable-threadsafe`)**: concurrent calls corrupt the heap;
- **thread-safe build**: safe, but a global lock serialises every API call.

**Neither offers in-process I/O concurrency.** This changes a design conclusion
rather than adding a caveat: the asynchronous overlap that is this library's
main performance justification can only be delivered by the POSIX backend. With
the HDF5 backend every call must funnel through one dedicated I/O thread, which
buys overlap with *computation* but no parallel I/O.

POSIX is therefore not merely the no-dependency default — it is the only backend
that can deliver the headline feature, and the choice between backends is a
performance decision rather than a packaging one.

A secondary risk: HDF5 is known to handle very large numbers of small datasets
poorly. The 1000-cycle test above used 50 records; the real pattern may involve
thousands, and should be measured at that scale.

## 7c. Other candidates considered

| library | licence | verdict |
|---|---|---|
| **HDF5** | BSD-like | optional backend. Right semantics, wrong space behaviour under varying-size churn (x1.4–1.8) |
| **ADIOS2** | Apache-2.0 | rejected — append-only, no read-modify-write, x31 growth (§7a) |
| **NetCDF-4** | MIT-like | built on HDF5; inherits its behaviour with a more restrictive model. No advantage |
| **Blosc2** | BSD-3 | C, permissive, light deps, chunked, in-place partial update — all verified. But compression does **not** pay on the target payload (§7e). Situational third backend at best |
| **SQLite** | public domain | incremental BLOB I/O allows in-place partial update in principle; test not completed. Unsuited to GB-scale arrays regardless |
| **LMDB / RocksDB** | permissive | key-value stores tuned for many small records with transactional guarantees nobody here needs; write amplification is wrong for GB arrays |
| **TensorStore** | Apache-2.0 | chunked array I/O with the right access model, but a heavy C++ dependency for a library selling thinness |

## 7d. The interface is achievable — eT proves it

Worth recording, because the rest of the survey suggests otherwise. eT routes
**every** file operation through one interface: a single `open(` call in roughly
325k lines of source (`src/io/abstract_file_class.F90`), with no scratch markers
anywhere in the tree. Against NWChem binding files in 673 places, abacus in 588
and exciting in 428, this is not a difference of degree. It shows the
concentrated layer is reachable at production scale, and it is the closest thing
in the corpus to what this library's callers would look like after adoption.

eT correspondingly does not *need* libscratch. It is the model, not a target.

## 7e. Blosc2: compression does not pay on this data

Measured on 32 MiB float64 payloads (`tests/blosc2_eval.py`):

| payload | scheme | ratio | compress MB/s |
|---|---|---|---|
| uniform random (control) | lz4+bitshuffle | 1.14 | 1297 |
| **amplitude-like** (decaying magnitude) | lz4+bitshuffle | **1.05** | 1462 |
| amplitude-like | zstd+bitshuffle | 1.14 | 329 |
| screened (68% exact zeros) | lz4+shuffle | 1.66 | 624 |
| screened | zstd+bitshuffle | 2.73 | 51 |

Dense float64 amplitudes — the payload this library exists for — compress by
**five per cent**. That does not justify a codec in the write path. Only screened
or sparse data pays, and there zstd's ratio comes at a throughput no storage
device needs help beating.

Two caveats, both of which could flip this:

1. The raw-write baseline in that test reached 1200–2000 MB/s, which is the page
   cache, not a device. Against a shared parallel filesystem at a few hundred
   MB/s, lz4+shuffle at 624 MB/s with a 1.66x ratio on screened data is a clear
   win. **The decision is the ratio of compression throughput to *device*
   throughput**, and should be re-measured on the target machine, not this one.
2. The amplitude-like payload is synthetic — a decaying-magnitude model, not real
   amplitudes. Real ones are blocked by orbital indices and may compress better
   under chunking aligned to that structure. Worth repeating with a dump from an
   actual CCSD run before the option is discarded.

Conclusion: not a primary backend, and compression is not the library's selling
point. Keep it as an optional codec for sparse payloads on slow filesystems,
decided per store at open time rather than designed in.

## 8. Open questions

- ~~Is HDF5 an adequate backend, making this a façade?~~ **settled, partly**
  (§7b): correct semantics, but x1.4–1.8 growth under varying-size churn and no
  usable threading, so an optional backend rather than the implementation.
- ~~Would ADIOS2 serve?~~ **settled, no** (§7a): append-only, x31 growth.
- ~~Does HDF5's global lock defeat the async layer?~~ **settled, yes** — and the
  stock non-threadsafe build is worse: it corrupts memory at two threads (§7b).
- ~~Key space~~ **settled**: string keys with a table of contents, on the
  evidence of both lead adopters.
- ~~Does the page abstraction survive?~~ **settled: no, and it need not.**
  Checked in Psi4's source: zero sites outside `libpsio` do arithmetic on
  `.page`, and `PSIO_PAGELEN` appears nowhere outside it. All 37 consuming files
  construct addresses solely through `psio_get_address(start, shift)`, which is
  `start + shift` in (page, offset) coordinates. The shim is a typedef and a
  one-line inline function; consumers are untouched.
- ~~Does concurrent accumulation need atomicity?~~ **settled: no — isolation
  instead** (§5a).
- ~~`mmap`~~ **settled: supported as an opt-in `LS_MAPPED` mode** (§3), after the
  qp2 survey showed two codes using it for real work. Async is unavailable on
  mapped stores, by nature rather than by omission.
  Cursor-threaded addressing (OpenMolcas's in/out `iDisk`) was never a problem:
  the cursor is an offset the caller already holds.
- ~~Error model, `ENOSPC` and partial transfers, `ls_keys` ownership, table-of-
  contents locking, the missing communicator on `LS_SHARED_MPIIO`, and the
  undefined `LS_MAPPED` combinations~~ **settled in §4b**, and expressed in
  `include/libscratch.h`. Two of the six were defects in §4, not omissions: the
  struct had no backend or mode member at all, and §5a's distinct-key guarantee
  was unimplementable without a lock on the table of contents.

Still open, and the only one that matters:

- **Does asynchronous overlap win on a real out-of-core workload?** Criterion 2
  (§1), answerable only by measurement, and cheapest via the abacus port (§6).
