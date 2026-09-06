# Porting Quantum Package 2's scratch I/O onto libspill

Brief for a dedicated session working in a qp2 checkout. Self-contained.

## Goal

qp2 spills to disk through **four independent, hand-rolled mechanisms that do
not share code** — two of which solve the same problem twice. Replace them with
[libspill](https://github.com/ReLibQC/libspill), starting with the one that has
a working shim already.

## Read this first: qp2's own survey said mechanism 2 was impossible

A survey of qp2's scratch I/O named the `mmap`-backed Cholesky work matrix and
Davidson `W`/`S` matrices as *"the one clear negative result"* — they are not
records being read and written but *"dense linear-algebra operands (BLAS3
`dgemm` arguments, random-index element updates) that happen to be too large for
RAM"*, and a keyed store *"would force these algorithms to be rewritten as
explicit blocked out-of-core linear algebra — a real, nontrivial rewrite"*.

It then stated what would change that verdict:

> If a shared library wants to serve this use case, the primitive it needs to
> expose is closer to **"give me a large flat memory region backed by scratch
> storage" (i.e. its own `mmap`-like allocator with byte addressing)**, not a
> record/key API.

libspill's `LS_MAPPED` mode is that primitive. `ls_reserve` sizes the region,
`ls_map` returns one pointer, `c_f_pointer` makes it the array qp2 already
indexes — so `w => map_w%d2` and `L(Lset(p),k)` keep working unchanged.

**The negative result no longer holds.** Do not let it steer the port.

## Mechanisms, in the order worth doing them

### 1. `src/utils/mmap.f90` (343 lines) — shim exists, start here

`port/qp2/mmap_libspill.F90` in the libspill repo implements qp2's signatures
(`mmap_create_d/s/i/i8`, `mmap_destroy`, `mmap_sync`, `mmap_type`) over
`LS_MAPPED`. 7 checks, including the two patterns the survey said a record API
cannot express: random-index element updates, and **three mapped regions as the
operands of one `dgemm`**.

Call sites, all of which stay as they are:

| file | line | what |
|---|---|---|
| `src/ao_two_e_ints/cholesky.irp.f` | 185, 480 | pivoted-Cholesky work matrix |
| `src/davidson/diagonalization_hs2_dressed.irp.f` | 280–281, 733–734 | Davidson `W`/`S` |
| `src/dav_general_mat/dav_general.irp.f` | — | the same, mirrored |

**Two behavioural differences, stated rather than hidden:**

- qp2 creates an anonymous mapping and unlinks it *immediately* while still
  mapped, so a crash leaves nothing behind. The shim unlinks at
  `mmap_destroy`. The window is longer; the file is still gone at exit. If that
  matters, libspill needs an "unlink now, keep the mapping" call — worth raising
  upstream rather than working around.
- `single_node` is accepted and has no separate effect. libspill honours
  `TMPDIR`, which on a cluster is normally node-local.

### 2. Mechanism 3 — a dozen ad-hoc `open`+`write(N) array` pairs

No shared helper at all: one bespoke read/write pair per tensor, each ~10–20
lines, each reinventing "open, write whole array, close, flip an EZFIO flag".
In `tc_integ.irp.f`, `total_tc_int.irp.f`, `cholesky.irp.f` (AO and MO),
`io_two_rdm.irp.f`, `io_6_index_tensor.irp.f`, `guess_t.irp.f`.

The survey calls these *"mostly yes"* for a keyed store: one key per tensor,
write-once/read-once, no partial updates.

**Be selective.** Some are genuine *restart* files, which libspill deliberately
excludes — it is a within-run scratch layer, not durable output. Take the
within-run ones; leave restart to EZFIO. The CCSD T1/T2 amplitude files in
`guess_t.irp.f` are formatted, one scalar per `write`, and are restart: leave
them.

### 3. Mechanisms 1 and 4 — the same problem solved twice, in one codebase

- **Mechanism 1:** `src/utils/map_module.f90` (902 lines) +
  `src/utils/map_functions.irp.f` (133). `map_save_to_disk` /
  `map_load_from_disk` at `map_functions.irp.f:1,70`, called from
  `two_e_integrals.irp.f:426,475` and `mo_bi_integrals.irp.f:55,90`.
  Survey verdict: *"fits nearly as well"* — three related keys per map
  (`_idx`, `_key`, `_value`).
- **Mechanism 4:** `plugins/local/ao_tc_eff_map/map_integrals_eff_pot.irp.f`
  (313 lines), `dump_ao_tc_sym_two_e_pot` at :235 and
  `load_ao_tc_sym_two_e_pot` at :266. A **fourth independent serialisation of
  "a sharded integral map"** — structurally the same problem as mechanism 1,
  reimplemented with per-shard sequential records.

That duplication *inside a single codebase* is the strongest argument for the
port. If nothing else lands, unifying 1 and 4 is worth doing on its own.

## Get and build libspill

```sh
git clone https://github.com/ReLibQC/libspill
cmake -S libspill -B libspill/build -DLIBSPILL_BUILD_FORTRAN=ON \
      -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build libspill/build -j && cmake --install libspill/build
```

Link `libspill::spill_f` (the Fortran module). The `.mod` installs under
`include/libspill/<compiler>-<version>/`.

## qp2-specific build notes

- **qp2 uses IRPF90.** `.irp.f` files are inputs to a code generator, and the
  build regenerates. `src/utils/mmap.f90` is *plain* Fortran, not `.irp.f`, so
  mechanism 1 is the straightforward one; the `.irp.f` call sites are only
  touched if you go past it.
- The build is `./configure` + ninja. Adding an external dependency means
  touching the config; check how other externals (`qp2-dependencies`) are wired
  before inventing a mechanism.
- **EZFIO is not in scope.** It is the hierarchical parameter/array store qp2 is
  built around and is a different thing from scratch I/O. Do not try to replace
  it.

## Verification, in order

1. Build with the mmap shim substituted for `mmap.f90`.
2. A Davidson run and a Cholesky decomposition — those are the mechanism-2 call
   sites. Energies must match to machine precision.
3. qp2's own test suite.
4. `git diff --stat` should show changes only in `src/utils/mmap.f90` (deleted)
   and the new shim. Zero call-site changes for mechanism 2.

## Things that will need a decision

- **Is the shorter unlink window acceptable** for the anonymous mappings? If
  not, that is a libspill feature request, not a qp2 workaround.
- **`memory_budget`** is unset. qp2 knows its memory limit; the in-memory tier
  is the largest measured win elsewhere.
- **How far to go.** Mechanism 2 alone is a clean, self-contained change. 1, 3
  and 4 are larger and touch `.irp.f` generated code. Landing 2 first gives
  everyone something to judge.

## Honest status

The shim has **not** been built inside qp2 and qp2's test suite has not been run
against it. What is verified: qp2's signatures, the array-syntax access pattern,
zero-fill on creation, `dgemm` operands, and named/anonymous lifetimes — all
standalone, 7 checks.
