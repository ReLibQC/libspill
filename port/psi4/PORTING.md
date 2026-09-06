# Porting Psi4's libpsio onto libspill

Brief for a dedicated session working in a Psi4 checkout. Self-contained: you do
not need the conversation this came from.

## Goal

Replace the *implementation* of Psi4's `libpsio` with a shim over
[libspill](https://github.com/ReLibQC/libspill), **without changing a single
consumer call site**. `psio.h`, `psio.hpp` and `config.h` stay byte-identical;
what goes away is the ~2000 lines behind them.

Expected diff: **−2036 / +365 lines inside `libpsio`, 0 changes in the 452 files
that call it.**

## What already exists

`port/psi4/psio_libspill.cc` and `.h` in the libspill repo. Copy both into
`psi4/src/psi4/libpsio/`. They are tested (14 checks) and — importantly — a
conformance build already links them against **Psi4's own headers**, so the
signatures are known to match:

```sh
make check-psi4 PSI4_DIR=/path/to/psi4/psi4     # in the libspill tree
#   [PASS] 16 entry points match Psi4's own declarations
#   [PASS] PSIO_KEYLEN, PSIO_PAGELEN and the open modes are unchanged
```

**Build the shim with `-DPSIO_USE_PSI4_HEADERS`.** That makes it include Psi4's
real `psio.h` instead of the copy of Psi4's types it carries for standalone
testing. `port/psi4/psio_types.h` is **not** needed inside Psi4 — do not copy it
(it also has an unresolved licensing note, being a transcription of Psi4's
LGPL-3 declarations).

## Get and build libspill

```sh
git clone https://github.com/ReLibQC/libspill
cmake -S libspill -B libspill/build -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build libspill/build -j && cmake --install libspill/build
```

Then in Psi4's CMake:

```cmake
find_package(libspill 0.1 REQUIRED)
target_link_libraries(psi4-core PRIVATE libspill::spill)
```

## Files to delete from `psi4/src/psi4/libpsio/`

Remove these 23 from the `list(APPEND sources ...)` in that directory's
`CMakeLists.txt`, and add `psio_libspill.cc`:

```
read.cc  write.cc  read_entry.cc  write_entry.cc  rw.cc
open.cc  close.cc  open_check.cc
tocscan.cc  tocwrite.cc  tocread.cc  toclen.cc  toclast.cc
tocdel.cc  tocclean.cc  tocprint.cc
volseek.cc  zero_disk.cc
get_address.cc  get_global_address.cc  get_length.cc
init.cc  done.cc
```

## Files that stay, untouched

`error.cc`, `decode_errno.cc`, `compose_err_msg.cc`, `getpid.cc`,
`filemanager.cc` (PSIOManager), `filescfg.cc`, `get_filename.cc`,
`rename_file.cc`, `change_namespace.cc` — 850 lines. These are Psi4 *policy*
(scratch paths, namespaces, retention, error text), not I/O, and libspill has no
opinion about them.

`aio_handler.cc` also stays for now — see "Stage 2".

## Why this is mechanical — three facts, each checked against the tree

Re-verify if you like; they were true at `a0e6ba5c4`.

1. **`psio_address` is linear.** `psio_get_address(start, shift)` is exactly
   `start + shift` in (page, offset) coordinates, so `page * PSIO_PAGELEN +
   offset` is a faithful byte offset. `.page` appears **zero** times outside
   `libpsio`:
   `grep -rn "\.page\b" psi4/src --include=*.cc --include=*.h | grep -v libpsio`
2. **`psio_tocscan` is used only as an existence test.** `->sadd` and `->eadd`
   appear zero times outside `libpsio`; every consumer writes
   `if (!psio_tocscan(...))` or compares to `nullptr`. The one exception is
   `export_psio.cc`, which hands the pointer to Python.
3. **Nothing outside `libpsio` cares about the on-disk table of contents.**
   `rd_toclen` 0 consumers, `tocread` 0, `toclen` 0, `tocwrite` 1.

## The clearest win

`PSIO::zero_disk(unit, key, rows, cols)` currently loops `rows` times writing a
zeroed row through the whole stack. The shim is one `ls_reserve`, which on Linux
is one `fallocate`. **59 call sites.**

## Stage 2, deliberately not part of this port

`aio_handler.cc` (630 lines) is Psi4's own asynchronous layer, used at 27 call
sites in nine files (DiskDFJK, the PK Fock builders, SAPT). It could be replaced
by `ls_aread`/`ls_awrite`. **Do not do it in the same change** — a first port
should change nothing that could alter results or timings.

Worth knowing: `AIOHandler` covers those nine files, against **452** that touch
psio. The other 443 have no overlap available today, so there is headroom in
Psi4 — just not where you would look first.

## Verification, in order

1. Psi4 builds.
2. `ctest` — the full Psi4 test suite. This is the step that turns the port from
   "the API fits" into evidence.
3. Byte-exactness against the old layer on a real job: run a DF-CCSD(T) or
   disk-based MP2 before and after and compare energies to machine precision.
4. Count the lines changed outside `libpsio`. It should be zero. If it is not,
   the API is wrong and libspill should change, not Psi4.

## Things that will need a decision

- **Scratch path policy.** libspill's `ls_opts.dir` takes a directory;
  PSIOManager already computes one per unit. The shim currently uses a single
  directory set by `psio_set_scratch_dir`. Wiring it to PSIOManager's per-unit
  paths is the natural next step and needs a Psi4 maintainer's view.
- **`memory_budget`.** Not set by the shim. Psi4 knows its memory limit; passing
  a fraction of it would give the in-memory tier, which is the single largest
  measured win (29% over a hand-rolled prefetch, and read-modify-write that
  never touches disk).
- **Error mapping.** libspill's codes are richer than Psi4's `PSIO_ERROR_*`; the
  shim narrows them and reports the detail through a log callback instead.
  Check that suits Psi4's error conventions.

## Honest status

The shim has **not** been built inside Psi4 and Psi4's test suite has not been
run against it. Everything above is verified at the level of signatures,
semantics and standalone tests. That last mile is what this session is for.
