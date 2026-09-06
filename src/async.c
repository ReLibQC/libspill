/* SPDX-License-Identifier: BSD-3-Clause */
/* The asynchronous layer -- "the reason the library exists" (DESIGN.md §4), and
 * the only place criterion 2 can be earned, since §7b's measurements confine it
 * to the POSIX backend.
 *
 * A small pool of worker threads runs ordinary ls_rw calls. That is sound only
 * because the data path uses pread/pwrite and shares no file position, which is
 * the design §5a takes from OpenMolcas's thread-safe pair; nothing here needs a
 * data lock, and the table-of-contents lock is held only for lookups. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define LS_THREADS_DEFAULT 4

static void req_release(ls_store *s, ls_req *r)   /* q_lk held */
{
    if (r->lprev) r->lprev->lnext = r->lnext; else s->live = r->lnext;
    if (r->lnext) r->lnext->lprev = r->lprev;
    ls_mutex_destroy(&r->lk);
    ls_cond_destroy(&r->cv);
    free(r);
}

static void *worker(void *arg)
{
    ls_store *s = arg;

    for (;;) {
        ls_req *r;

        ls_mutex_lock(&s->q_lk);
        while (!s->qh && !s->stop)
            ls_cond_wait(&s->q_cv, &s->q_lk);
        if (!s->qh && s->stop) { ls_mutex_unlock(&s->q_lk); break; }

        r = s->qh;
        s->qh = r->next;
        if (!s->qh) s->qt = NULL;
        ls_mutex_unlock(&s->q_lk);

        {
            int rc = ls_rw(s, r->key, r->off, r->n,
                           r->rbuf, r->wbuf, r->op);
            ls_mutex_lock(&r->lk);
            r->status = rc;
            r->done = 1;
            ls_cond_broadcast(&r->cv);
            ls_mutex_unlock(&r->lk);

            ls_mutex_lock(&s->q_lk);
            if (rc != LS_OK && s->drain_err == LS_OK) s->drain_err = rc;
            s->inflight--;
            ls_cond_broadcast(&s->q_cv);
            ls_mutex_unlock(&s->q_lk);
        }
    }
    return NULL;
}

int ls_pool_start(ls_store *s)
{
    const char *e = getenv("LIBSPILL_IO_THREADS");
    size_t i, n = LS_THREADS_DEFAULT;

    if (e && *e) {
        long v = strtol(e, NULL, 10);
        if (v > 0 && v < 256) n = (size_t)v;
    }
    s->thr = calloc(n, sizeof *s->thr);
    if (!s->thr) return -ENOMEM;

    for (i = 0; i < n; i++)
        if (ls_thread_create(&s->thr[i], worker, s) != 0) break;
    s->nthr = i;
    if (s->nthr == 0) { free(s->thr); s->thr = NULL; return -EAGAIN; }
    return LS_OK;
}

/* Drains rather than cancels: ls_close is documented to behave as fclose does.
 * Requests the caller never waited on are released here, so closing a store
 * cannot leak them; ls_wait on a request after its store is closed is
 * undefined, and always was. */
void ls_pool_stop(ls_store *s)
{
    size_t i;

    ls_mutex_lock(&s->q_lk);
    s->stop = 1;
    ls_cond_broadcast(&s->q_cv);
    ls_mutex_unlock(&s->q_lk);

    for (i = 0; i < s->nthr; i++)
        ls_thread_join(s->thr[i]);
    free(s->thr);
    s->thr = NULL;
    s->nthr = 0;

    ls_mutex_lock(&s->q_lk);
    while (s->live) req_release(s, s->live);
    ls_mutex_unlock(&s->q_lk);
}

static int submit(ls_store *s, const char *key, uint64_t off, size_t n,
                  void *rbuf, const void *wbuf, int op, ls_req **out)
{
    ls_req *r;

    if (!s || !out) return LS_ERR_INVAL;
    if (!ls_key_ok(key)) return LS_ERR_INVAL;
    if (n && !(op == LS_OP_WRITE ? (const void *)wbuf : rbuf)) return LS_ERR_INVAL;
    if (s->o.mode != LS_EXPLICIT) return LS_ERR_MODE;
    if (s->o.backend != LS_POSIX) return LS_ERR_MODE;
    if (s->nthr == 0) return LS_ERR_BACKEND;

    r = calloc(1, sizeof *r);
    if (!r) return -ENOMEM;
    r->s = s;
    r->op = op;
    memcpy(r->key, key, strlen(key) + 1);
    r->off = off;
    r->n = n;
    r->rbuf = rbuf;
    r->wbuf = wbuf;
    ls_mutex_init(&r->lk);
    ls_cond_init(&r->cv);

    ls_mutex_lock(&s->q_lk);
    if (s->stop) {
        ls_mutex_unlock(&s->q_lk);
        ls_mutex_destroy(&r->lk);
        ls_cond_destroy(&r->cv);
        free(r);
        return LS_ERR_BUSY;
    }
    r->lnext = s->live;
    if (s->live) s->live->lprev = r;
    s->live = r;

    if (s->qt) s->qt->next = r; else s->qh = r;
    s->qt = r;
    s->inflight++;
    ls_cond_signal(&s->q_cv);
    ls_mutex_unlock(&s->q_lk);

    *out = r;
    return LS_OK;
}

int ls_awrite(ls_store *s, const char *key, uint64_t off, size_t n,
              const void *buf, ls_req **req)
{ return submit(s, key, off, n, NULL, buf, LS_OP_WRITE, req); }

int ls_aread(ls_store *s, const char *key, uint64_t off, size_t n,
             void *buf, ls_req **req)
{ return submit(s, key, off, n, buf, NULL, LS_OP_READ, req); }

int ls_wait(ls_req *req)
{
    ls_store *s;
    int rc;

    if (!req) return LS_ERR_INVAL;
    s = req->s;

    ls_mutex_lock(&req->lk);
    while (!req->done)
        ls_cond_wait(&req->cv, &req->lk);
    rc = req->status;
    ls_mutex_unlock(&req->lk);

    ls_mutex_lock(&s->q_lk);
    req_release(s, req);
    ls_mutex_unlock(&s->q_lk);
    return rc;
}

int ls_test(ls_req *req, int *done)
{
    if (!req || !done) return LS_ERR_INVAL;
    ls_mutex_lock(&req->lk);
    *done = req->done;
    ls_mutex_unlock(&req->lk);
    return LS_OK;
}
