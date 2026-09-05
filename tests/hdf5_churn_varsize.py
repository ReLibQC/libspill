import os, tempfile, shutil, numpy as np, h5py
tmp=tempfile.mkdtemp(prefix="h5c2_"); rng=np.random.default_rng(1)
def churn(path, cycles=60, **kw):
    live=0; sizes={}
    with h5py.File(path,"w",**kw) as f:
        for k in range(8):
            n=int(rng.integers(1<<14,1<<18)); sizes[k]=n
            f.create_dataset(f"k{k}",data=rng.random(n))
        for c in range(cycles):
            k=c%8
            del f[f"k{k}"]
            n=int(rng.integers(1<<14,1<<18)); sizes[k]=n     # DIFFERENT size
            f.create_dataset(f"k{k}",data=rng.random(n))
        live=sum(sizes.values())*8
    return os.path.getsize(path), live
for label,kw in (("default strategy",{}),
                 ("fs_strategy='fsm'",dict(fs_strategy="fsm",fs_persist=True,fs_threshold=1)),
                 ("fs_strategy='page'",dict(fs_strategy="page",fs_persist=True))):
    try:
        sz,live=churn(os.path.join(tmp,label.split()[0]+".h5"),**kw)
        print(f"  {label:<22} file {sz/2**20:7.1f} MiB   live {live/2**20:6.1f} MiB   x{sz/live:5.2f}")
    except Exception as e:
        print(f"  {label:<22} FAILED {type(e).__name__}: {str(e)[:70]}")
shutil.rmtree(tmp,ignore_errors=True)
