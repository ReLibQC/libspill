/* SPDX-License-Identifier: BSD-3-Clause */
/* libspill -- the platform floor. Internal; not installed.
 *
 * Everything the library asks of the operating system, in one place: the file
 * primitives, threading, and mapping. POSIX and Win32 implement it in
 * os_posix.c and os_win32.c respectively, and nothing else in the library names
 * a system call -- mapping included, since ls_map's reserve-then-MAP_FIXED
 * sequence is exactly the kind of thing that would otherwise sit in store.c
 * with a platform guard around it. The mapping entry points are declared in
 * internal.h rather than here, because they speak in ls_extent.
 *
 * Two rules the Win32 side has to keep, because breaking either would compile
 * silently and be wrong at run time:
 *
 *   1. The reads and writes are POSITIONAL. §5a promises that concurrent
 *      ls_read/ls_write on one store need no external locking, and the POSIX
 *      backend delivers that by using pread/pwrite, which carry the offset and
 *      touch no shared file position. _lseeki64 followed by _read would compile
 *      and would quietly break that promise. Win32 gets OVERLAPPED ReadFile /
 *      WriteFile, which is positional in the same sense.
 *
 *   2. Offsets are 64-bit. off_t is 32 bits in the MSVC CRT, so the casts this
 *      library used to do would have truncated silently somewhere past 2 GB --
 *      inside, not beyond, the range libspill exists for. Everything below
 *      takes uint64_t and no caller mentions off_t.
 *
 * Errors come back the way the rest of the library expects: -1 (or NULL) with
 * errno set, so the `rc = -errno` idiom at the call sites is unchanged. The
 * Win32 side translates GetLastError() into the nearest errno itself.
 */
#ifndef LS_OS_H
#define LS_OS_H

#include <stddef.h>
#include <stdint.h>

/* Mapping is the one facility with no cheap Win32 equivalent: ls_map reserves
 * the logical span PROT_NONE and maps each extent over its slice with
 * MAP_FIXED, which on Windows needs a placeholder reservation
 * (VirtualAlloc2 + MapViewOfFile3, Windows 10 1803 and later). Rather than
 * carry an untested version of that, LS_MAPPED is refused at ls_open where
 * this is not defined, with LS_ERR_MODE -- the same answer a caller already
 * gets for the other unsupported mode combinations. Every other mode works. */
/* LS_NO_MMAP forces the same path on a platform that does have mapping. It
 * exists so the no-mapping branches are compiled and run by the ordinary
 * Linux CI rather than only ever on Windows: everything below os.h behaves
 * identically either way. */
#if !defined(_WIN32) && !defined(LS_NO_MMAP)
#  define LS_HAVE_MMAP 1
#endif

/* The separator to build a path with. Windows accepts '/' in its file APIs, so
 * this is tidiness rather than a fix -- but a platform floor that abstracts the
 * temp directory and then hardcodes the separator is only half a floor. */
#ifdef _WIN32
#  define LS_PATH_SEP "\\"
#else
#  define LS_PATH_SEP "/"
#endif

/* ------------------------------------------------------------------- files */

/* Open (creating if needed) for read and write. want_direct asks for
 * uncached I/O; *got_direct says whether it was granted, since a filesystem
 * may refuse it and buffered I/O is the correct fallback. Returns a descriptor
 * or -1/errno. */
int      ls_os_open_rw(const char *path, int want_direct, int *got_direct);
int      ls_os_close(int fd);

/* Positional, and short transfers are the caller's to loop over -- exactly
 * pread/pwrite. Return bytes moved, or -1/errno. */
int64_t  ls_os_pread (int fd, void *buf, size_t n, uint64_t off);
int64_t  ls_os_pwrite(int fd, const void *buf, size_t n, uint64_t off);

int      ls_os_ftruncate(int fd, uint64_t len);
int      ls_os_fsync(int fd);
int      ls_os_file_size(int fd, uint64_t *size);   /* 0, or -1/errno */
int      ls_os_path_size(const char *path, uint64_t *size);  /* same, by name */
int      ls_os_unlink(const char *path);
int      ls_os_getpid(void);

/* Punch a zeroed range without writing it, where the platform can. Returns 0 on
 * success, -1 if unsupported or refused -- the caller then writes zeros, which
 * is always correct and merely slower. */
int      ls_os_zero_range(int fd, uint64_t off, uint64_t len);

/* The directory a store goes in when the caller names none. POSIX reads TMPDIR
 * and falls back to /tmp; Windows has neither, and its own answer comes from
 * GetTempPath (TMP, then TEMP, then the user profile). Never returns an empty
 * string, and never a trailing separator. */
void     ls_os_tmpdir(char *buf, size_t buflen);

/* Fill buf with the system's text for errnum, always NUL-terminated and always
 * something. This is in the floor because the three implementations disagree
 * about almost everything: glibc's strerror_r returns char* and may not touch
 * the buffer at all, XSI's returns int, and the MSVC CRT spells it
 * strerror_s with the arguments in a different order. Getting it wrong prints
 * uninitialised stack. */
void     ls_os_strerror(int errnum, char *buf, size_t buflen);

/* LS_ALIGN-aligned allocation, for the uncached-I/O bounce buffers. The free
 * has to match the alloc: memory from Windows' _aligned_malloc must never
 * reach free(), and it does not fault until much later if it does. */
void    *ls_os_aligned_alloc(size_t align, size_t n);
void     ls_os_aligned_free(void *p);

/* ----------------------------------------------------------------- threads
 *
 * A mutex, a condition variable and a joinable thread: the whole of what this
 * library uses. Named ls_* rather than shimmed under the pthread_ names on
 * purpose -- a header that defines pthread_mutex_lock is a trap for anyone who
 * later links a real pthreads on Windows. */

#ifdef _WIN32
/* <windows.h> is not included here: it would reach every translation unit and
 * bring min/max macros and a great deal else with it. These mirror SRWLOCK,
 * CONDITION_VARIABLE and HANDLE, which are a pointer each; os_win32.c asserts
 * the sizes agree. */
typedef struct { void *srw;  } ls_mutex;
typedef struct { void *cond; } ls_cond;
typedef struct { void *h;    } ls_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t ls_mutex;
typedef pthread_cond_t  ls_cond;
typedef pthread_t       ls_thread;
#endif

void ls_mutex_init(ls_mutex *m);
void ls_mutex_destroy(ls_mutex *m);
void ls_mutex_lock(ls_mutex *m);
void ls_mutex_unlock(ls_mutex *m);

void ls_cond_init(ls_cond *c);
void ls_cond_destroy(ls_cond *c);
void ls_cond_wait(ls_cond *c, ls_mutex *m);
void ls_cond_signal(ls_cond *c);
void ls_cond_broadcast(ls_cond *c);

/* Returns 0, or -1/errno. The worker signature is pthread's so that the POSIX
 * implementation is a direct call and the Win32 one trampolines. */
int  ls_thread_create(ls_thread *t, void *(*fn)(void *), void *arg);
void ls_thread_join(ls_thread t);

#endif /* LS_OS_H */
