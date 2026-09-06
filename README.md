# libspill

Scratch and out-of-core I/O for electronic-structure codes. C99 core, with C++,
Fortran and Python bindings and no required dependencies beyond pthreads.

**Status: working, unproven.** The library and its bindings are complete and
tested; what has *not* happened is the thing the project exists for — see
[Status](#status) before adopting. [`DESIGN.md`](DESIGN.md) is the argument for
why this component and not another, and records what was tried and rejected.

## What it is

A keyed byte-range store for data with a lifetime shorter than the job:
integrals, amplitudes, per-k-point quantities, intermediate tensors. Records are
named, addressed by byte offset, and read back, overwritten and re-read during
the run.

```c
#include <libspill.h>

ls_opts o;
ls_opts_default(&o);
o.memory_budget = 8ull << 30;          /* stay in RAM below this */

int err;
ls_store *s = ls_open("scratch", &o, &err);

ls_write(s, "t2", 0, nbytes, amps);
ls_read (s, "t2", 0, nbytes, amps);

ls_req *r;                              /* the reason the library exists */
ls_aread(s, "t2", off, nbytes, next, &r);
/* ... compute on the current block while that one loads ... */
ls_wait(r);

ls_close(s, 0);                         /* 0 unlinks */
```

## What it is not

Durable, portable, self-describing output — including checkpoint and restart.
That is HDF5's and TREXIO's job. A scratch layer that also becomes an archival
format acquires the weight that kept every existing one unshared; the thinness
is the whole design. It also never learns what a tensor is: the store moves
opaque bytes.

## Building

```sh
cmake -S . -B build -DLIBSPILL_BUILD_FORTRAN=ON -DLIBSPILL_BUILD_TESTS=ON
cmake --build build -j
cd build && ctest
cmake --install build --prefix /where/you/want
```

Then, from a consuming project:

```cmake
find_package(libspill 0.1 REQUIRED)
target_link_libraries(mycode PRIVATE libspill::spill)     # C / C++
target_link_libraries(mycode PRIVATE libspill::spill_f)   # Fortran module
```

A `libspill.pc` is installed for pkg-config consumers. `tests/consumer/` is a
project that does exactly the above and is exercised by `make check-install`.

The `Makefile` is the development driver: `make check` runs everything —
the C suite, the C++ layer, the Fortran binding, the Python binding, the four
port shims, the packaging check, and the Psi4 header-conformance build. It
skips what is not available (numpy, a Psi4 tree, cmake) rather than failing.

## The four language layers

The C header is the ABI and the stable surface. Nothing else is.

| | header / module | notes |
|---|---|---|
| C | `libspill.h` | the ABI; see §4b of DESIGN.md for the contract |
| C++ | `libspill.hpp` | header-only, C++20. RAII, spans, exceptions, typed `accumulate` |
| Fortran | `use libspill` | `ISO_C_BINDING`; every dummy carries an explicit C-matching kind |
| Python | `python/libspill.py` | ctypes over the ABI, NumPy throughout; needs no compiler |

```cpp
ls::store s{"scratch", {.memory_budget = 8ull << 30}};
s.write<double>("t2", off, amps);              // size deduced from the span
s.accumulate<double>("t2", off, partial, 0.5); // reduction chosen by T
auto req = s.awrite<double>("t2", off, next);  // move-only; waits in its dtor
```

```python
with libspill.open("scratch", memory_budget=8 << 30) as s:
    s["t2"] = amps
    blk = s.read("t2", offset=o, shape=(n, m))
    s.read("t2", offset=o, out=buf)            # no allocation
    s.accumulate("t2", o, partial, alpha=0.5)
    arr = s.map("eri")                         # NumPy over the mapping, no copy
```

## Where the performance comes from

Four things, in expected order of magnitude — and only the second is measured
end to end so far (§7f of DESIGN.md):

1. **Asynchronous overlap.** Most legacy layers are synchronous; callers that
   want double-buffering hand-roll it, and most do not.
2. **The in-memory tier.** `memory_budget` makes the same call path zero-copy
   when the data fits, which on modern nodes it often does.
3. **Node-local staging.** `TMPDIR` on a cluster is usually node-local; the
   default scratch path is often a shared filesystem.
4. **Alignment and preallocation.** `pread`/`pwrite` against preallocated
   extents, optionally `O_DIRECT`.

## Ports

Four shims, each keeping its target's own signatures so that consumer call sites
are untouched. All four are tested; none has been built inside its own code.

| target | what it replaces | call sites changed |
|---|---|---|
| `port/psi4/` | `libpsio`, 23 files / 2036 lines | 0 of ~1080 |
| `port/openmolcas/` (DaFile) | `io_util`, 5942 lines | 0 of 2225 |
| `port/openmolcas/` (RunFile) | `runfile_util` generic core | 0 |
| `tests/crayio_shim.c` | `WOPEN`/`GETWA`/`PUTWA`, 4 codes | conformance only |

## Status

Two success criteria, from §1 of DESIGN.md, and neither is met yet:

1. **Deprecate the legacy layer in 3–5 codes** — not "be adoptable", but
   actually delete the in-house implementation with its maintainers' agreement
   and have their test suites pass. The four shims show the API fits; **none has
   been built inside its own code**, which is the step that would make this
   falsifiable.
2. **A measurable performance gain on a real out-of-core workload.** First
   measurements are encouraging — 11–14% over a hand-rolled prefetch at high
   compute intensity, 29% from the memory tier — but they come from one loaded
   workstation, and at lower intensity the machine noise exceeds the effect.
   This needs a quiet node, on both a shared filesystem and node-local NVMe.

The honest summary is that the hard parts are built and the evidence is not in.

## Licence

See the repository. The intent is a permissive licence, so that the codes this
targets can vendor it without friction.
