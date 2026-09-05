"""Same four questions, other candidate stores. sqlite3 is stdlib; blosc2 is C/BSD-3."""
import os, tempfile, shutil, sqlite3, numpy as np
tmp=tempfile.mkdtemp(prefix="alt_"); N=1<<17; live=N*8
def rep(name,ok,info): print(f"  [{'PASS' if ok else 'FAIL':<4}] {name:<46}{info}")

# --- SQLite incremental BLOB I/O -------------------------------------------
try:
    p=os.path.join(tmp,"s.db"); c=sqlite3.connect(p)
    c.execute("CREATE TABLE rec(k TEXT PRIMARY KEY, v BLOB)")
    c.execute("INSERT INTO rec VALUES(?,?)",("amp",np.zeros(N,dtype='f8').tobytes()))
    c.commit()
    ok_partial=False
    try:                                   # in-place partial update
        b=c.blobopen("rec","v","amp",readonly=False)
        b.seek(4096); b.write(np.ones(512,dtype='f8').tobytes()); b.close()
        b=c.blobopen("rec","v","amp"); b.seek(4096); back=np.frombuffer(b.read(4096),dtype='f8'); b.close()
        ok_partial=bool((back==1.0).all())
    except Exception as e:
        ok_partial=False
    for i in range(30):
        b=c.blobopen("rec","v","amp",readonly=False); b.write(np.full(N,i,dtype='f8').tobytes()); b.close()
    c.commit(); c.close()
    sz=os.path.getsize(p)
    rep("sqlite3 incremental BLOB: partial update",ok_partial,"")
    rep("sqlite3: storage after 30 overwrites",sz<2*live,f"{sz/2**20:.1f} MiB (x{sz/live:.2f})")
except Exception as e:
    rep("sqlite3",False,f"{type(e).__name__}: {e}")

# --- Blosc2 super-chunk ------------------------------------------------------
try:
    import blosc2
    p=os.path.join(tmp,"b.b2frame")
    a=blosc2.zeros(N,dtype='f8',urlpath=p,mode='w',chunks=(1<<13,))
    a[4096:8192]=1.0
    ok_partial=bool((a[4096:8192]==1.0).all() and (a[:4096]==0.0).all())
    for i in range(30): a[:]=float(i)
    sz=sum(os.path.getsize(os.path.join(r,f)) for r,_,fs in os.walk(p) for f in fs) if os.path.isdir(p) else os.path.getsize(p)
    rep("blosc2 ndarray: partial update in place",ok_partial,"")
    rep("blosc2: storage after 30 overwrites",sz<2*live,f"{sz/2**20:.2f} MiB (x{sz/live:.2f}, compressed)")
except Exception as e:
    rep("blosc2",False,f"{type(e).__name__}: {str(e)[:90]}")
shutil.rmtree(tmp,ignore_errors=True)
