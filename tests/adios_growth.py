import os, shutil, tempfile, numpy as np, adios2
tmp=tempfile.mkdtemp(prefix="adios_grow_"); p=os.path.join(tmp,"s.bp")
N=1<<17                      # 128k doubles = 1 MiB record
rec=np.zeros(N,dtype=np.float64)
def du(path):
    t=0
    for r,d,f in os.walk(path):
        for x in f: t+=os.path.getsize(os.path.join(r,x))
    return t
with adios2.Stream(p,"w") as s:
    s.begin_step(); s.write("amp",rec,[N],[0],[N]); s.end_step()
print(f"record size            : {N*8/2**20:.1f} MiB")
print(f"after 1 write          : {du(p)/2**20:.1f} MiB")
for i in range(1,31):
    with adios2.Stream(p,"a") as s:
        s.begin_step(); s.write("amp",np.full(N,i,dtype=np.float64),[N],[0],[N]); s.end_step()
    if i in (5,10,20,30):
        print(f"after {i+1:2d} updates      : {du(p)/2**20:6.1f} MiB   "
              f"(x{du(p)/(N*8):.1f} the live data)")
shutil.rmtree(tmp,ignore_errors=True)
