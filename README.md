# libspill

[![CI](https://github.com/ReLibQC/libspill/actions/workflows/ci.yml/badge.svg)](https://github.com/ReLibQC/libspill/actions/workflows/ci.yml)

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
cmake -S . -B build -DLIBSPILL_BUILD_FORTRAN=ON -DLIBSPILL_BUILD_TESTS=ON \
      -DLIBSPILL_WITH_HDF5=ON        # optional; see below
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

## Vendoring

libspill is meant to be embedded, so the namespace is kept narrow deliberately:

- **C.** Every public entry point is `ls_`-prefixed and marked `LS_API`; the
  library is compiled `-fvisibility=hidden`, so the ~34 internal helpers
  (`ls_rw`, `ls_toc_find`, `ls_pool_start`, …) are not linkable and are not part
  of the ABI. The static library is built position-independent, so it can go
  inside your shared object.
- **Macros.** Public macros are `LS_*` or `LIBSPILL_*`. No installed header
  defines a generic name.
- **C++.** The namespace is `libspill`; `ls` is a convenience alias you can turn
  off with `-DLIBSPILL_NO_SHORT_NAMESPACE`.
- **Fortran.** Module `libspill`, so symbols mangle to `__libspill_MOD_*`.
- **crayio.** Deliberately *not* namespaced — `wopen_`, `getwa_` and the rest
  are the names it exists to provide — which is why it is a separate library you
  link only in place of your own `crayio.o`. `libspill` itself exports none of
  them.

To keep even the public symbols internal to your library, link with
`-Wl,--exclude-libs,libspill.a`; the 28 exported `ls_*` names then do not appear
in your shared object at all.

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

## Backends

POSIX is the default and carries no dependencies. It is also the only backend
that can deliver asynchrony: HDF5's stock build is not thread-safe and its
thread-safe build serialises every call behind one lock, so `ls_aread` and
`ls_awrite` return `LS_ERR_MODE` there.

HDF5 is optional, for codes that already link it and want scratch files
`h5ls` and `h5dump` can read — the per-key attribute blob becomes a native HDF5
attribute, so shape and dtype metadata is visible to standard tooling. The cost
is space: on the same churn protocol, through the same libspill calls, the POSIX
backend grows to x1.13 of live data and HDF5 to x1.70. Pick it for
inspectability, not for performance.

A library built without HDF5 still accepts `LS_HDF5` at compile time and refuses
it at `ls_open` with `LS_ERR_BACKEND`, so no consumer needs conditional
compilation.

## The crayio compatibility layer

`-DLIBSPILL_WITH_CRAYIO=ON` builds `libspill_crayio`, a drop-in for the 1980s
Cray word-addressable I/O emulation that four codes still carry a private copy
of (Dalton, LSDalton, MADNESS, NWChem). It is a **separate** library because
`wopen_`, `getwa_` and `putwa_` are global Fortran symbols; linking it is how a
code opts in, in place of its own `crayio.o`.

`-DLIBSPILL_CRAYIO_I8=ON` selects a 64-bit Fortran `INTEGER` and **must match
the code it joins** — every argument arrives by pointer, so a mismatch reads
bytes the caller never wrote, and nothing in the toolchain will object. The test
suite builds it at both widths for exactly that reason.

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
| `port/crayio/` | `WOPEN`/`GETWA`/`PUTWA`, 4 codes | shipped compatibility layer |

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

BSD-3-Clause; see [`LICENSE`](LICENSE). Every source file carries an
`SPDX-License-Identifier`, so a code that vendors libspill can label it without
reading the tree.

One exception, marked in the file itself: `port/psi4/psio_types.h` transcribes
Psi4's own public declarations so the shim can be tested outside the Psi4 tree.
Those are interoperability facts about an LGPL-3 interface rather than
libspill's own work, and the file is not needed when building inside Psi4 —
define `PSIO_USE_PSI4_HEADERS` and Psi4's `psio.h` supplies them.
