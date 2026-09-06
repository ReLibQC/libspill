/* SPDX-License-Identifier: BSD-3-Clause */
/* The platform floor on Windows (MSVC, clang-cl and MinGW alike).
 *
 * The two things worth reading os.h for before changing anything here: the
 * reads and writes must stay POSITIONAL, because §5a promises concurrent
 * ls_read/ls_write need no external locking, and offsets must stay 64-bit,
 * because the CRT's off_t is 32 bits.
 *
 * Descriptors: the library passes `int fd` around, so this keeps the CRT layer
 * (_open/_close) for the lifecycle and reaches the underlying HANDLE with
 * _get_osfhandle for the positional transfers. That is cheaper than threading a
 * HANDLE through every structure, and it keeps _unlink and the delete-on-close
 * semantics working the way the rest of the library expects.
 */
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <process.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#define WIN32_LEAN_AND_MEAN
#include <string.h>
#include <windows.h>

#include "os.h"

/* os.h mirrors these as one pointer each rather than including <windows.h>
 * everywhere; if that ever stops being true it must fail here, loudly. */
typedef char ls_srw_size_check[sizeof(SRWLOCK) <= sizeof(void *) ? 1 : -1];
typedef char ls_cv_size_check [sizeof(CONDITION_VARIABLE) <= sizeof(void *) ? 1 : -1];
typedef char ls_h_size_check  [sizeof(HANDLE) <= sizeof(void *) ? 1 : -1];

/* The rest of the library reports failures as -errno, so Win32 status has to
 * become errno here rather than at the call sites. Only the codes these
 * operations can actually produce are listed; anything else becomes EIO, which
 * is what the callers treat as "the I/O failed" anyway. */
static void set_errno_from_win32(DWORD e)
{
    switch (e) {
        case ERROR_SUCCESS:             errno = 0;       break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:      errno = ENOENT;  break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:   errno = EACCES;  break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:      errno = EEXIST;  break;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:         errno = ENOMEM;  break;
        case ERROR_DISK_FULL:           errno = ENOSPC;  break;
        case ERROR_INVALID_HANDLE:      errno = EBADF;   break;
        case ERROR_INVALID_PARAMETER:   errno = EINVAL;  break;
        case ERROR_HANDLE_EOF:          errno = 0;       break;
        default:                        errno = EIO;     break;
    }
}

static HANDLE handle_of(int fd)
{
    intptr_t h = _get_osfhandle(fd);
    return (h == -1) ? INVALID_HANDLE_VALUE : (HANDLE)h;
}

/* ------------------------------------------------------------------- files */

int ls_os_open_rw(const char *path, int want_direct, int *got_direct)
{
    int fd = -1;

    /* FILE_FLAG_NO_BUFFERING is the O_DIRECT equivalent, but it cannot be
     * requested through _open, and it constrains offset, length and buffer
     * alignment more tightly than O_DIRECT does. Buffered I/O is a correct
     * answer to direct_io -- the caller is told it did not get it -- so this
     * reports "not granted" rather than carrying an untested aligned path. */
    (void)want_direct;
    if (got_direct) *got_direct = 0;

    if (_sopen_s(&fd, path, _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYNO,
                 _S_IREAD | _S_IWRITE) != 0)
        return -1;                       /* _sopen_s has set errno */
    return fd;
}

int ls_os_close(int fd) { return _close(fd); }

/* Positional, and short transfers are the caller's to loop over, exactly as
 * pread/pwrite.
 *
 * The subtlety, since a reviewer will reach for the documentation here: the
 * handle is NOT opened FILE_FLAG_OVERLAPPED, and passing an OVERLAPPED to a
 * synchronous handle is explicitly allowed -- the transfer starts at the offset
 * in the OVERLAPPED and the call does not return until it completes. It also
 * leaves the shared file pointer wherever it finished, which is why this is
 * still safe under §5a: every transfer states its own offset and nothing in the
 * library ever reads that pointer. Two concurrent calls each get their own
 * bytes. What would NOT be safe is _lseeki64 + _read, where the seek and the
 * read are separate steps over that same shared pointer. */
int64_t ls_os_pread(int fd, void *buf, size_t n, uint64_t off)
{
    HANDLE h = handle_of(fd);
    OVERLAPPED ov;
    DWORD got = 0, want = (n > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (DWORD)n;

    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    memset(&ov, 0, sizeof ov);
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);

    if (!ReadFile(h, buf, want, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;    /* read past end: 0, like pread */
        set_errno_from_win32(e);
        return -1;
    }
    return (int64_t)got;
}

int64_t ls_os_pwrite(int fd, const void *buf, size_t n, uint64_t off)
{
    HANDLE h = handle_of(fd);
    OVERLAPPED ov;
    DWORD put = 0, want = (n > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (DWORD)n;

    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    memset(&ov, 0, sizeof ov);
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);

    if (!WriteFile(h, buf, want, &put, &ov)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    return (int64_t)put;
}

int ls_os_ftruncate(int fd, uint64_t len)
{
    /* _chsize_s takes a 64-bit length, which _chsize does not. Growing a file
     * this way zero-fills, as ftruncate does. */
    int e = _chsize_s(fd, (__int64)len);
    if (e != 0) { errno = e; return -1; }
    return 0;
}

int ls_os_fsync(int fd)
{
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!FlushFileBuffers(h)) { set_errno_from_win32(GetLastError()); return -1; }
    return 0;
}

int ls_os_file_size(int fd, uint64_t *size)
{
    HANDLE h = handle_of(fd);
    LARGE_INTEGER li;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!GetFileSizeEx(h, &li)) { set_errno_from_win32(GetLastError()); return -1; }
    if (size) *size = (uint64_t)li.QuadPart;
    return 0;
}

int ls_os_path_size(const char *path, uint64_t *size)
{
    struct __stat64 st;
    if (_stat64(path, &st) != 0) return -1;      /* _stat64 has set errno */
    if (size) *size = (uint64_t)st.st_size;
    return 0;
}

int ls_os_unlink(const char *path) { return _unlink(path); }

int ls_os_getpid(void) { return (int)_getpid(); }

int ls_os_zero_range(int fd, uint64_t off, uint64_t len)
{
    /* FSCTL_SET_ZERO_DATA is the equivalent, but only on a sparse file, and
     * making the store sparse is a policy decision this port is not entitled to
     * take. The caller writes zeros instead, which is always correct. */
    (void)fd; (void)off; (void)len;
    errno = ENOSYS;
    return -1;
}

void ls_os_tmpdir(char *buf, size_t buflen)
{
    /* GetTempPath is the documented resolution -- TMP, then TEMP, then the
     * user profile, then the Windows directory -- and it always ends with a
     * separator, which the caller does not want. */
    DWORD n;
    if (buflen == 0) return;
    n = GetTempPathA((DWORD)buflen, buf);
    if (n == 0 || n >= buflen) { snprintf(buf, buflen, "."); return; }
    while (n > 0 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) buf[--n] = '\0';
    if (n == 0) snprintf(buf, buflen, ".");
}

void ls_os_strerror(int errnum, char *buf, size_t buflen)
{
    if (buflen == 0) return;
    if (strerror_s(buf, buflen, errnum) != 0)
        snprintf(buf, buflen, "errno %d", errnum);
    buf[buflen - 1] = '\0';
}

/* _aligned_malloc, not malloc: the CRT tracks the adjustment it made, which is
 * why the matching _aligned_free is not optional. */
void *ls_os_aligned_alloc(size_t align, size_t n) { return _aligned_malloc(n, align); }
void  ls_os_aligned_free(void *p) { _aligned_free(p); }

/* ----------------------------------------------------------------- threads
 *
 * SRWLOCK and CONDITION_VARIABLE need no destruction and cannot fail to
 * initialise, which is why the destroy entry points below are empty rather than
 * missing: the POSIX side does need them. Neither is recursive -- nor is the
 * pthread mutex this replaces, and the library never relocks. */

void ls_mutex_init(ls_mutex *m)    { InitializeSRWLock((PSRWLOCK)&m->srw); }
void ls_mutex_destroy(ls_mutex *m) { (void)m; }
void ls_mutex_lock(ls_mutex *m)    { AcquireSRWLockExclusive((PSRWLOCK)&m->srw); }
void ls_mutex_unlock(ls_mutex *m)  { ReleaseSRWLockExclusive((PSRWLOCK)&m->srw); }

void ls_cond_init(ls_cond *c)      { InitializeConditionVariable((PCONDITION_VARIABLE)&c->cond); }
void ls_cond_destroy(ls_cond *c)   { (void)c; }

void ls_cond_wait(ls_cond *c, ls_mutex *m)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->cond,
                              (PSRWLOCK)&m->srw, INFINITE, 0);
}
void ls_cond_signal(ls_cond *c)    { WakeConditionVariable((PCONDITION_VARIABLE)&c->cond); }
void ls_cond_broadcast(ls_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->cond); }

/* The worker signature is pthread's, so Win32 needs a trampoline. _beginthreadex
 * rather than CreateThread because the workers call into the CRT. */
typedef struct { void *(*fn)(void *); void *arg; } ls_trampoline;

static unsigned __stdcall ls_thread_main(void *p)
{
    ls_trampoline t = *(ls_trampoline *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

int ls_thread_create(ls_thread *t, void *(*fn)(void *), void *arg)
{
    ls_trampoline *tr = (ls_trampoline *)malloc(sizeof *tr);
    uintptr_t h;

    if (!tr) { errno = ENOMEM; return -1; }
    tr->fn = fn;
    tr->arg = arg;

    h = _beginthreadex(NULL, 0, ls_thread_main, tr, 0, NULL);
    if (h == 0) { free(tr); return -1; }   /* _beginthreadex has set errno */
    t->h = (void *)h;
    return 0;
}

void ls_thread_join(ls_thread t)
{
    if (!t.h) return;
    WaitForSingleObject((HANDLE)t.h, INFINITE);
    CloseHandle((HANDLE)t.h);
}
