/* SPDX-License-Identifier: BSD-3-Clause */
/* The platform floor on POSIX: every one of these is the system call it is
 * named after. The file exists so that os_win32.c has something to be the
 * counterpart of; see os.h for what the two must agree on. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                 /* O_DIRECT and fallocate; see CMakeLists */
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "os.h"

int ls_os_open_rw(const char *path, int want_direct, int *got_direct)
{
    int flags = O_RDWR | O_CREAT;
    int fd;

    if (got_direct) *got_direct = 0;
#ifdef O_DIRECT
    if (want_direct) flags |= O_DIRECT;
#else
    (void)want_direct;
#endif
    fd = open(path, flags, 0600);
#ifdef O_DIRECT
    if (fd < 0 && want_direct)                  /* the filesystem may refuse it */
        return open(path, O_RDWR | O_CREAT, 0600);
    if (fd >= 0 && want_direct && got_direct) *got_direct = 1;
#endif
    return fd;
}

int ls_os_close(int fd) { return close(fd); }

int64_t ls_os_pread(int fd, void *buf, size_t n, uint64_t off)
{ return (int64_t)pread(fd, buf, n, (off_t)off); }

int64_t ls_os_pwrite(int fd, const void *buf, size_t n, uint64_t off)
{ return (int64_t)pwrite(fd, buf, n, (off_t)off); }

int ls_os_ftruncate(int fd, uint64_t len) { return ftruncate(fd, (off_t)len); }

int ls_os_fsync(int fd) { return fsync(fd); }

int ls_os_file_size(int fd, uint64_t *size)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (size) *size = (uint64_t)st.st_size;
    return 0;
}

int ls_os_path_size(const char *path, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (size) *size = (uint64_t)st.st_size;
    return 0;
}

int ls_os_unlink(const char *path) { return unlink(path); }

int ls_os_getpid(void) { return (int)getpid(); }

int ls_os_zero_range(int fd, uint64_t off, uint64_t len)
{
#ifdef FALLOC_FL_ZERO_RANGE
    return fallocate(fd, FALLOC_FL_ZERO_RANGE, (off_t)off, (off_t)len);
#else
    (void)fd; (void)off; (void)len;
    errno = ENOSYS;
    return -1;
#endif
}

void ls_os_strerror(int errnum, char *buf, size_t buflen)
{
    if (buflen == 0) return;
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    {   /* GNU: returns the text, which may or may not be in buf */
        const char *p = strerror_r(errnum, buf, buflen);
        if (p != buf) snprintf(buf, buflen, "%s", p ? p : "unknown error");
    }
#else
    if (strerror_r(errnum, buf, buflen) != 0)
        snprintf(buf, buflen, "errno %d", errnum);
#endif
    buf[buflen - 1] = '\0';
}

void *ls_os_aligned_alloc(size_t align, size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, align, n) != 0) return NULL;
    return p;
}

void ls_os_aligned_free(void *p) { free(p); }

/* ----------------------------------------------------------------- threads */

void ls_mutex_init(ls_mutex *m)    { pthread_mutex_init(m, NULL); }
void ls_mutex_destroy(ls_mutex *m) { pthread_mutex_destroy(m); }
void ls_mutex_lock(ls_mutex *m)    { pthread_mutex_lock(m); }
void ls_mutex_unlock(ls_mutex *m)  { pthread_mutex_unlock(m); }

void ls_cond_init(ls_cond *c)                 { pthread_cond_init(c, NULL); }
void ls_cond_destroy(ls_cond *c)              { pthread_cond_destroy(c); }
void ls_cond_wait(ls_cond *c, ls_mutex *m)    { pthread_cond_wait(c, m); }
void ls_cond_signal(ls_cond *c)               { pthread_cond_signal(c); }
void ls_cond_broadcast(ls_cond *c)            { pthread_cond_broadcast(c); }

int  ls_thread_create(ls_thread *t, void *(*fn)(void *), void *arg)
{
    int rc = pthread_create(t, NULL, fn, arg);
    if (rc != 0) { errno = rc; return -1; }
    return 0;
}
void ls_thread_join(ls_thread t) { pthread_join(t, NULL); }
