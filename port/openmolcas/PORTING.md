# Porting OpenMolcas's io_util and runfile_util onto libspill

Brief for a dedicated session working in an OpenMolcas checkout. Self-contained.

## Goal

Two independent ports, in this order:

1. **DaFile** (`src/io_util`) — replace the direct-access layer. −3019 / +295
   lines, **0 of 2225 call sites in 508 files.**
2. **RunFile generic core** (`src/runfile_util`) — replace `gxWrRun`, `gxRdRun`,
   `ffxRun`. −991 / +307 lines, **0 of 3755 call sites in 623 files.**

Signatures are OpenMolcas's throughout, so the typed wrappers above them
(`dDaFile`, `Put_dArray`, `Get_iArray`, `qpg_*`) are untouched.

## READ THIS FIRST: the integer kind will silently corrupt if you get it wrong

`iwp` is `int64` in OpenMolcas's default build (`src/system_util/definitions.F90`
selects `int64` or `int32`), and **every** interface-visible argument of
`DaFile`, `gxWrRun` and the rest is declared `integer(kind=iwp)`.

`DaFile` and its family are **external subroutines with no explicit interface
anywhere**. So a shim whose dummies are plain `integer` compiles cleanly under
every compiler, and is wrong from the first call. This already happened once
during development: the test still passed literal `1` and `4096` as default
integers after the dummies were widened, gfortran said nothing, and `iOpt`
arrived as noise.

In OpenMolcas, delete `port/openmolcas/molcas_kinds.F90` and replace its use
with `use Definitions, only: iwp, wp`. Likewise delete `molcas_stubs.F90` —
`SysAbendMsg` already exists in the tree.

**Regression test to keep:** a disk address of 3×10⁹ (above 2³¹) must round-trip
through `dDaFile`. It only does if the whole interface is 64-bit.

## Get and build libspill

```sh
git clone https://github.com/ReLibQC/libspill
cmake -S libspill -B libspill/build -DLIBSPILL_BUILD_FORTRAN=ON \
      -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build libspill/build -j && cmake --install libspill/build
```

```cmake
find_package(libspill 0.1 REQUIRED)
target_link_libraries(molcas PRIVATE libspill::spill_f)   # Fortran module
```

The `.mod` installs under `include/libspill/<compiler>-<version>/` because
Fortran modules are compiler- and version-specific.

## Port 1: DaFile

Copy `port/openmolcas/dafile_libspill.F90` into `src/io_util/`. Remove these 31
from that directory's `CMakeLists.txt` `set(sources ...)`:

```
dafile.F90  bdafile.F90  cdafile.F90  chdafile.F90  ddafile.F90
idafile.F90  i1dafile.F90  mpdafile.F90
daclos.F90  daeras.F90  dafile_checkarg.F90
daname.F90  daname_main.F90  daname_mf.F90  daname_mf_wa.F90  daname_wa.F90
fast_io.F90  fastio.F90
aixrd.F90  aixwr.F90  aixcls.F90  aixfsz.F90  aixprd.F90  aixpwr.F90
cio.c  cio.h
fscb2unit.F90  lu2desc.F90  lu2handle.F90  handle2name.F90  get_mbl_wa.F90
```

Safe to remove as a unit: `AixRd`, `AixWr`, `AixCls`, `AixFsz` have **zero**
users outside `io_util`, and `Fast_IO`'s shared state has zero. Verify before
deleting:

```sh
grep -rIl --include=*.F90 "use Fast_IO" src | grep -v '^src/io_util/'   # expect empty
```

**Stays:** `isfreeunit` (264 external users), `molcas_open*` (176),
`f_inquire` (128), `AixRm` (14), `fcopy`, `append_file*`, `prgm*`,
`text_file.F90`, `zip.c`, `filesystem_wrapper.c`. General utilities, not DaFile.

### The media block length is observable behaviour

Unlike Psi4, **OpenMolcas callers do arithmetic on `iDisk`** — hundreds of
places add byte and word counts to it. So this is part of the contract, not an
implementation detail:

- `MBl_wa = 8` (word-addressable units, `DaName_wa`), `MBl_nwa = 512`;
- a transfer that does not fill a block rounds the returned cursor **up**, so
  100 doubles written at cursor 0 of a 512-byte unit leaves it at **2**, not 1.

The shim reproduces this exactly. If you change it, you break callers silently.

### Two things vanish by construction

- `Multi_File`/`MaxFileSize` striping (328 of the 3019 lines, 23 references)
  exists because a unit could outgrow a file. A libspill store has no such
  limit.
- The shared position array `Addr()`, which is unguarded across ~672 call sites,
  has no counterpart: libspill uses `pread`/`pwrite` and carries no file
  position. The thread-safety defect becomes unrepresentable rather than fixed.

### And one thing becomes real

`iOpt = 6` and `7` are documented in `dafile.F90` as asynchronous write and
read. They are not: both fall through to the identical `AixWr`/`AixRd` calls as
options 1 and 2 (`if ((iOpt == 1) .or. (iOpt == 6))`), and **no call site in the
tree uses them** — 0 occurrences, against 717 and 912 for their synchronous
twins.

The shim implements them for real, with the contract that the caller's buffer
must stay valid until the next DaFile operation on that unit or `DaClos`.
Adopting overlap then becomes a one-character edit at whichever call sites their
authors judge safe. **Do this after the port lands, not as part of it.**

## Port 2: RunFile

Copy `port/openmolcas/runfile_libspill.F90` into `src/runfile_util/`. It
replaces the generic core: `gxwrrun.F90`, `gxrdrun.F90`, `gzrwrun.F90`,
`ffxrun.F90`, `mkrun.F90`, and the ToC machinery in `runfile_data.F90`.

The 53 typed `Get_`/`Put_`/`qpg_` wrappers are untouched.

This port *deletes* a mechanism rather than reproducing one. RunFile keeps a
fixed table of contents in the file — 1024 entries of
`{Lab, Ptr, Len, MaxLen, Typ}` — and libspill's table of contents already is
that. `Ptr` disappears; `Typ` and `Len` become an 8-byte attribute blob
(`ls_set_attr`); the linear scan becomes a hash lookup.

### Four defects it removes — worth telling the maintainers about

1. **The file never reuses space.** A record outgrowing its `MaxLen` abandons
   its slot and takes fresh space at `RunHdr%Next`, which only ever advances.
   There is no free list anywhere in `runfile_util`. `gxwrrun.F90` still carries
   a commented-out warning about it.
2. **`MaxLen` decays**, making (1) worse. It is recomputed as
   `max(NewLen, nData)` where `NewLen` is `Toc(item)%Len`, the previous
   *length*, not the previous `MaxLen` (`gxwrrun.F90:100`, `:125`). Two
   consecutive smaller writes make a slot forget capacity it still holds.
3. **`nToc = 1024`, fixed**, overflowing into `"Ran out of ToC record in
   RunFile"` and `Abend()`.
4. **Both scans are linear with no early exit** — the label lookup keeps
   assigning after it matches, and the free-slot search counts down from 1024
   without breaking. Every read and write pays 1024 iterations.

Measured on 8 records rewritten at varying sizes over 24 cycles: RunFile's own
algorithm gives **×17.6** of live data (a lower bound — it models `MaxLen` as
never decreasing), against libspill's measured **×1.37**. For scale, libspill
rejected HDF5 as a sole backend at ×1.4–1.8.

## Verification, in order

1. Build. Then run the 3×10⁹ address test — if the integer kind is wrong,
   everything else is meaningless.
2. OpenMolcas's own verification suite. This is the step that matters.
3. A job that exercises both layers: any CASSCF/CASPT2 run uses DaFile heavily
   and RunFile constantly.
4. `git diff --stat` outside `io_util` and `runfile_util` should be empty.

## Things that will need a decision

- **`memory_budget`.** Not set by the shims. OpenMolcas knows its memory limit;
  passing a fraction gives the in-memory tier — the largest measured win.
- **When to close the store.** OpenMolcas opens and closes the RunFile around
  *every single call*; the shim holds one store open and releases it in
  `Fin_Run_Use`. Confirm that is the right lifetime for their process model.
- **`LS_SHARED`.** If any RunFile access is genuinely cross-process, it needs
  `LS_SHARED` and a frozen layout, not the default.

## Honest status

Neither shim has been built inside OpenMolcas and its test suite has not been
run against them. Signatures, semantics, block-length arithmetic and the space
behaviour are verified standalone (14 + 15 checks).
