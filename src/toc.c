/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#ifdef LS_HAVE_MMAP
#include <sys/mman.h>
#endif

int ls_key_ok(const char *key)
{
    size_t n;
    if (!key) return 0;
    n = strlen(key);
    return n > 0 && n <= LS_KEY_MAX;
}

static size_t hash_of(const char *k)
{
    /* FNV-1a; keys are short and the table is small, so this is ample */
    size_t h = (size_t)1469598103934665603ULL;
    while (*k) {
        h ^= (unsigned char)*k++;
        h *= (size_t)1099511628211ULL;
    }
    return h;
}

ls_rec *ls_toc_find(ls_store *s, const char *key)
{
    ls_rec *r = s->tab[hash_of(key) & (s->nbuckets - 1)];
    for (; r; r = r->hnext)
        if (strcmp(r->key, key) == 0) return r;
    return NULL;
}

static int toc_grow(ls_store *s)
{
    size_t nb = s->nbuckets * 2, i;
    ls_rec **t = calloc(nb, sizeof *t);
    if (!t) return -ENOMEM;

    for (i = 0; i < s->nbuckets; i++) {
        ls_rec *r = s->tab[i];
        while (r) {
            ls_rec *nx = r->hnext;
            size_t b = hash_of(r->key) & (nb - 1);
            r->hnext = t[b];
            t[b] = r;
            r = nx;
        }
    }
    free(s->tab);
    s->tab = t;
    s->nbuckets = nb;
    return LS_OK;
}

ls_rec *ls_toc_insert(ls_store *s, const char *key)
{
    ls_rec *r;
    size_t b;

    if (s->nrec + 1 > s->nbuckets - s->nbuckets / 4 && toc_grow(s) != LS_OK)
        return NULL;

    r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->key = strdup(key);
    if (!r->key) { free(r); return NULL; }

    b = hash_of(key) & (s->nbuckets - 1);
    r->hnext = s->tab[b];
    s->tab[b] = r;
    s->nrec++;
    return r;
}

void ls_toc_unlink(ls_store *s, ls_rec *r)
{
    size_t b = hash_of(r->key) & (s->nbuckets - 1);
    ls_rec **pp = &s->tab[b];
    while (*pp && *pp != r) pp = &(*pp)->hnext;
    if (*pp) { *pp = r->hnext; s->nrec--; }
}

void ls_rec_free(ls_store *s, ls_rec *r)
{
    if (!r) return;
#ifdef LS_HAVE_MMAP
    if (r->map_addr) munmap(r->map_addr, r->map_len);   /* close unmaps */
#endif
    if (r->mem) {
        if (s->resident >= r->mem_cap) s->resident -= (size_t)r->mem_cap;
        free(r->mem);
    }
    if (r->ext) ls_free_extents(s, r->ext, r->next);
    free(r->ext);
    free(r->key);
    free(r);
}
