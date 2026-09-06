#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Does Blosc2 earn a place as a libspill backend?

The question is not whether it compresses -- it is whether compress+write beats
write for the payloads these codes actually spill: coupled-cluster amplitudes,
integral batches, DIIS history. Compression only helps if the CPU cost is less
than the I/O saved, which depends on both the data and the device.
"""
import os, time, tempfile, shutil
import numpy as np, blosc2

tmp = tempfile.mkdtemp(prefix="b2_")
N   = 1 << 22                     # 32 MiB of float64
rng = np.random.default_rng(0)

def payloads():
    yield "incompressible (uniform random)", rng.random(N)
    # amplitude-like: wide dynamic range, magnitudes decaying over the index
    x = rng.standard_normal(N) * np.exp(-np.linspace(0, 12, N))
    yield "amplitude-like (decaying magnitude)", x
    # integral-like: many negligible values below a screening threshold
    y = rng.standard_normal(N); y[np.abs(y) < 1.2] = 0.0
    yield "screened (68% exact zeros)", y

def raw_roundtrip(a, path):
    t0=time.perf_counter(); a.tofile(path); tw=time.perf_counter()-t0
    t0=time.perf_counter(); b=np.fromfile(path,dtype=a.dtype); tr=time.perf_counter()-t0
    assert b.shape==a.shape
    return os.path.getsize(path), tw, tr

def b2_roundtrip(a, codec, filt):
    t0=time.perf_counter()
    c=blosc2.compress2(a, codec=codec, filter=filt, clevel=5)
    tw=time.perf_counter()-t0
    t0=time.perf_counter()
    d=blosc2.decompress2(c); tr=time.perf_counter()-t0
    assert len(d)==a.nbytes
    return len(c), tw, tr

print(f"{N*8/2**20:.0f} MiB float64 payloads\n")
hdr=f'{"payload":<36}{"scheme":<22}{"ratio":>7}{"comp MB/s":>11}{"decomp MB/s":>13}'
print(hdr); print('-'*len(hdr))
for name,a in payloads():
    sz,tw,tr = raw_roundtrip(a, os.path.join(tmp,"raw.bin"))
    print(f'{name:<36}{"raw file write/read":<22}{1.0:>7.2f}{a.nbytes/tw/1e6:>11.0f}{a.nbytes/tr/1e6:>13.0f}')
    for codec,filt,label in ((blosc2.Codec.LZ4,blosc2.Filter.SHUFFLE,"lz4+shuffle"),
                             (blosc2.Codec.LZ4,blosc2.Filter.BITSHUFFLE,"lz4+bitshuffle"),
                             (blosc2.Codec.ZSTD,blosc2.Filter.BITSHUFFLE,"zstd+bitshuffle")):
        try:
            cs,cw,cr=b2_roundtrip(a,codec,filt)
            print(f'{"":<36}{label:<22}{a.nbytes/cs:>7.2f}{a.nbytes/cw/1e6:>11.0f}{a.nbytes/cr/1e6:>13.0f}')
        except Exception as e:
            print(f'{"":<36}{label:<22}  FAILED {type(e).__name__}')
    print()
shutil.rmtree(tmp,ignore_errors=True)
