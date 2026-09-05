/* Does HDF5 concurrency work well enough to build an async layer on?
 *
 * Two regimes, both bad for different reasons:
 *   - built WITHOUT --enable-threadsafe (the common distro default, and this
 *     system): concurrent API calls are simply unsafe.
 *   - built WITH it: safe, but a global lock serialises every API call.
 * Either way an async layer gets no in-process concurrency from HDF5 itself.
 * This measures the scaling and compares against plain POSIX pread.
 */
#include <hdf5.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define NELEM (1<<20)      /* 8 MiB dataset */
#define SLICE (1<<14)
#define ITERS 200

static const char *H5PATH="/tmp/h5thr.h5", *RAWPATH="/tmp/h5thr.raw";
static hid_t g_file=-1, g_dset=-1;
static int   g_rawfd=-1;

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+1e-9*t.tv_nsec;}

static void *h5_worker(void *arg){
  long id=(long)arg; double *buf=malloc(SLICE*sizeof(double));
  hsize_t start,count=SLICE;
  hid_t mem=H5Screate_simple(1,&count,NULL);
  for(int i=0;i<ITERS;i++){
    start=((id*97+i*31)%(NELEM/SLICE))*SLICE;
    hid_t sp=H5Dget_space(g_dset);
    H5Sselect_hyperslab(sp,H5S_SELECT_SET,&start,NULL,&count,NULL);
    H5Dread(g_dset,H5T_NATIVE_DOUBLE,mem,sp,H5P_DEFAULT,buf);
    H5Sclose(sp);
  }
  H5Sclose(mem); free(buf); return NULL;
}
static void *raw_worker(void *arg){
  long id=(long)arg; double *buf=malloc(SLICE*sizeof(double));
  for(int i=0;i<ITERS;i++){
    off_t off=(((id*97+i*31)%(NELEM/SLICE))*SLICE)*(off_t)sizeof(double);
    if(pread(g_rawfd,buf,SLICE*sizeof(double),off)<0){perror("pread");break;}
  }
  free(buf); return NULL;
}
static double run(void *(*fn)(void*),int nthr){
  pthread_t t[16]; double t0=now();
  for(long i=0;i<nthr;i++) pthread_create(&t[i],NULL,fn,(void*)i);
  for(int i=0;i<nthr;i++) pthread_join(t[i],NULL);
  return now()-t0;
}
int main(int argc,char**argv){
  int NT = argc>1?atoi(argv[1]):4;
  setvbuf(stdout,NULL,_IONBF,0);
  printf("HDF5 %d.%d.%d, threadsafe build: %s\n\n",
         H5_VERS_MAJOR,H5_VERS_MINOR,H5_VERS_RELEASE,
#ifdef H5_HAVE_THREADSAFE
         "YES");
#else
         "NO  <-- concurrent API calls are undefined behaviour");
#endif
  double *d=malloc(NELEM*sizeof(double));
  for(int i=0;i<NELEM;i++) d[i]=i;
  hid_t f=H5Fcreate(H5PATH,H5F_ACC_TRUNC,H5P_DEFAULT,H5P_DEFAULT);
  hsize_t dims=NELEM; hid_t sp=H5Screate_simple(1,&dims,NULL);
  hid_t ds=H5Dcreate2(f,"rec",H5T_NATIVE_DOUBLE,sp,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);
  H5Dwrite(ds,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,d);
  H5Sclose(sp); g_file=f; g_dset=ds;
  int fd=open(RAWPATH,O_CREAT|O_TRUNC|O_RDWR,0600);
  if(write(fd,d,NELEM*sizeof(double))<0){perror("write");}
  g_rawfd=fd; free(d);

  printf("%-26s%10s%10s%10s\n","backend","1 thread","4 threads","speedup");
  double a1=run(h5_worker,NT); fprintf(stderr,"  HDF5  %d thread(s): %.3f s -- completed\n",NT,a1);
  
  double b1=run(raw_worker,NT);
  fprintf(stderr,"  POSIX %d thread(s): %.3f s -- completed\n",NT,b1);
  
  H5Dclose(ds); H5Fclose(f); close(fd);
  unlink(H5PATH); unlink(RAWPATH);
  return 0;
}
