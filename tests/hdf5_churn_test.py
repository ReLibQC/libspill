#!/usr/bin/env python3
"""Does an HDF5 file balloon when records are deleted and recreated?

The scratch pattern is not just overwrite-in-place (which HDF5 handles at x1.00).
It is CHURN: records are created, used, deleted, and different ones created --
integral batches, amplitude blocks, DIIS history rolling off. HDF5 historically
does not reclaim freed space, so the file can grow without bound even though the
live data does not.

Tested with the default file-space strategy and with free-space management
(H5Pset_file_space_strategy / fs_strategy='fsm'), which exists since HDF5 1.10
but is NOT the default.
"""
import os, tempfile, shutil, numpy as np, h5py

tmp = tempfile.mkdtemp(prefix="h5churn_")
N   = 1 << 17                     # 1 MiB records
LIVE = 8 * N * 8                  # 8 live records = 8 MiB
rng = np.random.default_rng(0)
payload = rng.random(N)           # incompressible

def churn(path, cycles=40, **kw):
    with h5py.File(path, "w", **kw) as f:
        for k in range(8):
            f.create_dataset(f"k{k}", data=payload)
        for c in range(cycles):
            k = c % 8
            del f[f"k{k}"]                       # record retired
            f.create_dataset(f"k{k}", data=payload)   # new one takes its place
    return os.path.getsize(path)

for label, kw in (("default file-space strategy", {}),
                  ("fs_strategy='fsm', persist",
                   dict(fs_strategy="fsm", fs_persist=True, fs_threshold=1))):
    try:
        p = os.path.join(tmp, label.split()[0] + ".h5")
        sz = churn(p, **kw)
        print(f"  {label:<32} {sz/2**20:7.1f} MiB   live {LIVE/2**20:.1f} MiB "
              f"  x{sz/LIVE:.2f}")
    except Exception as e:
        print(f"  {label:<32} FAILED: {type(e).__name__}: {str(e)[:80]}")

# and the control: pure overwrite, no delete
p = os.path.join(tmp, "ovr.h5")
with h5py.File(p, "w") as f:
    for k in range(8):
        f.create_dataset(f"k{k}", data=payload)
    for c in range(40):
        f[f"k{c%8}"][...] = payload
print(f"  {'overwrite only (no delete)':<32} {os.path.getsize(p)/2**20:7.1f} MiB"
      f"   live {LIVE/2**20:.1f} MiB   x{os.path.getsize(p)/LIVE:.2f}")
shutil.rmtree(tmp, ignore_errors=True)
