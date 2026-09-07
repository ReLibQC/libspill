/* SPDX-License-Identifier: BSD-3-Clause */
/* Threads for the test programs.
 *
 * src/os.h does this for the library, but it is internal and its
 * implementation is not exported, so a test linking only the public library
 * cannot reach it. The tests want exactly three things -- start a thread, join
 * it, and nothing else -- so here they are, with the pthread signature on both
 * sides to keep the thread bodies unchanged.
 */
#ifndef LS_TEST_THREAD_COMPAT_H
#define LS_TEST_THREAD_COMPAT_H

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef HANDLE ls_test_thread;

typedef struct { void *(*fn)(void *); void *arg; } ls_test_tramp;

static unsigned __stdcall ls_test_thread_main(void *p)
{
    ls_test_tramp t = *(ls_test_tramp *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

static int ls_test_thread_create(ls_test_thread *t, void *(*fn)(void *), void *arg)
{
    ls_test_tramp *tr = (ls_test_tramp *)malloc(sizeof *tr);
    uintptr_t h;
    if (!tr) return -1;
    tr->fn = fn;
    tr->arg = arg;
    h = _beginthreadex(NULL, 0, ls_test_thread_main, tr, 0, NULL);
    if (h == 0) { free(tr); return -1; }
    *t = (HANDLE)h;
    return 0;
}

static void ls_test_thread_join(ls_test_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else

#include <pthread.h>

typedef pthread_t ls_test_thread;

static int ls_test_thread_create(ls_test_thread *t, void *(*fn)(void *), void *arg)
{ return pthread_create(t, NULL, fn, arg); }

static void ls_test_thread_join(ls_test_thread t) { pthread_join(t, NULL); }

#endif
#endif /* LS_TEST_THREAD_COMPAT_H */
