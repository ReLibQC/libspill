#!/usr/bin/env python3
"""Can ADIOS2 serve as a scratch heap? -- the read-modify-write question.

Quantum-chemistry scratch I/O (Psi4 libpsio, OpenMolcas RunFile) is a HEAP
INSIDE ONE JOB: write a record, read it back later in the same run, overwrite
part of it, read it again. DIIS vectors, integral batches, CC amplitudes updated
every iteration.

ADIOS2's documented model is step-based and append-only, with an engine opened
for writing OR reading. If that is right, ADIOS2 cannot replace those layers --
however good it is at the durable-output job it was designed for.

This decides it by experiment rather than by reading the manual.
"""
import os, shutil, tempfile, traceback
import numpy as np
import adios2

tmp = tempfile.mkdtemp(prefix="adios_rmw_")
bp  = os.path.join(tmp, "scratch.bp")
res = {}

def check(name, fn):
    try:
        fn(); res[name] = ("PASS", "")
    except Exception as e:
        res[name] = ("FAIL", f"{type(e).__name__}: {str(e)[:150]}")

# 1. read back a variable inside the SAME open write session
def t1():
    with adios2.Stream(bp + ".t1", "w") as s:
        s.begin_step()
        s.write("rec", np.arange(16, dtype=np.float64), [16], [0], [16])
        s.end_step()
        # the scratch pattern: read it back without closing
        _ = s.read("rec")
check("1. read back within one open WRITE session", t1)

# 2. overwrite a record in place, same session, then read the new value
def t2():
    with adios2.Stream(bp + ".t2", "w") as s:
        s.begin_step()
        s.write("rec", np.zeros(16, dtype=np.float64), [16], [0], [16])
        s.end_step()
        s.begin_step()
        s.write("rec", np.ones(16, dtype=np.float64), [16], [0], [16])
        s.end_step()
    # reading back: does "rec" have one value, or two steps?
    with adios2.Stream(bp + ".t2", "r") as s:
        n = 0
        for _ in s.steps():
            n += 1
        if n != 1:
            raise RuntimeError(f"overwrite produced {n} steps, not an in-place update")
check("2. in-place overwrite (not a new step)", t2)

# 3. partial/sub-range update of an existing record
def t3():
    with adios2.Stream(bp + ".t3", "w") as s:
        s.begin_step()
        s.write("rec", np.zeros(64, dtype=np.float64), [64], [0], [64])
        s.end_step()
        s.begin_step()
        # update only elements [16,32) -- a strided sub-block write
        s.write("rec", np.ones(16, dtype=np.float64), [64], [16], [16])
        s.end_step()
    with adios2.Stream(bp + ".t3", "r") as s:
        s.begin_step()
        v = s.read("rec")
        s.end_step()
    if v is None or len(v) != 64:
        raise RuntimeError("could not read back full record after partial update")
    if not (v[16:32] == 1.0).all() or not (v[:16] == 0.0).all():
        raise RuntimeError("partial update did not merge into the existing record")
check("3. partial sub-range update merges in place", t3)

# 4. the workaround: close and reopen per update. does it at least work,
#    and what does it cost? (if this is the only route, it is fatal for a
#    per-iteration scratch heap)
def t4():
    import time
    p = bp + ".t4"
    with adios2.Stream(p, "w") as s:
        s.begin_step(); s.write("rec", np.zeros(1024, dtype=np.float64), [1024],[0],[1024]); s.end_step()
    t0 = time.perf_counter()
    for i in range(20):
        with adios2.Stream(p, "r") as s:
            s.begin_step(); _ = s.read("rec"); s.end_step()
        with adios2.Stream(p, "a") as s:
            s.begin_step(); s.write("rec", np.full(1024, i, dtype=np.float64), [1024],[0],[1024]); s.end_step()
    res["4. close/reopen workaround: 20 cycles"] = ("INFO", f"{time.perf_counter()-t0:.3f} s")
check("4. close/reopen workaround", t4)

print(f"ADIOS2 {adios2.__version__}\n")
for k in sorted(res):
    st, msg = res[k]
    print(f"  [{st:<4}] {k}" + (f"\n           {msg}" if msg else ""))
shutil.rmtree(tmp, ignore_errors=True)
