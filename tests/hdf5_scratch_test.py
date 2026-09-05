#!/usr/bin/env python3
"""Can HDF5 serve as the backend for a scratch heap?

Same questions put to ADIOS2 in adios_rmw_test.py, which failed all three:
read-back inside one open write session, in-place overwrite, and partial
sub-range update -- with storage growing linearly in the number of updates.

If HDF5 passes, libscratch becomes a thin facade over an existing dependency
that most of these codes already link, rather than a new implementation.
"""
import os, shutil, tempfile, time
import numpy as np, h5py

tmp = tempfile.mkdtemp(prefix="h5_scratch_"); p = os.path.join(tmp, "scratch.h5")
res = {}
def check(name, fn):
    try:
        info = fn(); res[name] = ("PASS", info or "")
    except Exception as e:
        res[name] = ("FAIL", f"{type(e).__name__}: {str(e)[:140]}")

N = 1 << 17          # 128k doubles = 1 MiB record

# 1. read a record back inside ONE open write session
def t1():
    with h5py.File(p, "w") as f:
        d = f.create_dataset("rec", data=np.arange(16, dtype="f8"))
        back = d[...]                       # read without closing
        assert (back == np.arange(16)).all()
check("1. read back within one open write session", t1)

# 2. in-place overwrite, same session, no growth
def t2():
    with h5py.File(p, "w") as f:
        d = f.create_dataset("rec", data=np.zeros(N, dtype="f8"))
        d[...] = 1.0
        assert (d[...] == 1.0).all()
check("2. in-place overwrite of a whole record", t2)

# 3. partial sub-range update merges into the record
def t3():
    with h5py.File(p, "w") as f:
        d = f.create_dataset("rec", data=np.zeros(64, dtype="f8"))
        d[16:32] = 1.0
        v = d[...]
        assert (v[16:32] == 1.0).all() and (v[:16] == 0.0).all() and (v[32:] == 0.0).all()
check("3. partial sub-range update merges in place", t3)

# 4. STORAGE GROWTH -- the question that disqualified ADIOS2
def t4():
    q = os.path.join(tmp, "grow.h5")
    with h5py.File(q, "w") as f:
        d = f.create_dataset("amp", data=np.zeros(N, dtype="f8"), chunks=(1 << 13,))
        for i in range(1, 31):
            d[...] = float(i)
    sz = os.path.getsize(q)
    return f"{sz/2**20:.1f} MiB after 30 overwrites of a 1.0 MiB record (x{sz/(N*8):.2f})"
check("4. storage after 30 overwrites", t4)

# 5. keyed random access across many records, one session (the scratch pattern)
def t5():
    q = os.path.join(tmp, "heap.h5")
    t0 = time.perf_counter()
    with h5py.File(q, "w") as f:
        for k in range(50):
            f.create_dataset(f"key{k:03d}", data=np.zeros(1 << 14, dtype="f8"), chunks=(1 << 12,))
        for it in range(20):                       # 20 "iterations"
            for k in range(50):
                d = f[f"key{k:03d}"]
                v = d[4096:8192]                   # read a sub-block
                d[4096:8192] = v + 1.0             # modify it in place
    dt = time.perf_counter() - t0
    sz = os.path.getsize(q)
    live = 50 * (1 << 14) * 8
    return f"{dt:.2f} s, file {sz/2**20:.1f} MiB vs {live/2**20:.1f} MiB live (x{sz/live:.2f})"
check("5. 1000 read-modify-write cycles across 50 records", t5)

# 6. can a dataset grow on demand? (scratch records rarely have known size)
def t6():
    q = os.path.join(tmp, "ext.h5")
    with h5py.File(q, "w") as f:
        d = f.create_dataset("rec", shape=(0,), maxshape=(None,), dtype="f8", chunks=(4096,))
        for i in range(10):
            d.resize((d.shape[0] + 4096,))
            d[-4096:] = float(i)
        assert d.shape[0] == 40960
check("6. extendable record (size not known up front)", t6)

print(f"h5py {h5py.__version__}, HDF5 {h5py.version.hdf5_version}\n")
for k in sorted(res):
    st, msg = res[k]
    print(f"  [{st:<4}] {k}" + (f"\n           {msg}" if msg else ""))
shutil.rmtree(tmp, ignore_errors=True)
