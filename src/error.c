/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

int ls_version(void) { return LS_VERSION_NUM; }

const char *ls_version_string(void)
{
    return "0.1.0";
}

const char *ls_strerror(int err, char *buf, size_t buflen)
{
    const char *m = NULL;

    if (!buf || buflen == 0) return buf;

    switch (err) {
    case LS_OK:          m = "success";                                    break;
    case LS_ERR_NOKEY:   m = "no record under that key";                   break;
    case LS_ERR_RANGE:   m = "range outside the record";                   break;
    case LS_ERR_INVAL:   m = "invalid argument or option combination";     break;
    case LS_ERR_MODE:    m = "not valid for this store's mode or backend";  break;
    case LS_ERR_BACKEND: m = "backend unavailable or failed";              break;
    case LS_ERR_BUSY:    m = "request still in flight";                    break;
    case LS_ERR_CORRUPT: m = "table of contents did not validate";         break;
    default:             break;
    }

    if (m) {
        snprintf(buf, buflen, "%s", m);
    } else if (err < 0 && err > -1000) {
        /* The negated-errno range. Which strerror this is, and how it reports,
         * is the platform floor's problem -- see ls_os_strerror. */
        ls_os_strerror(-err, buf, buflen);
    } else {
        snprintf(buf, buflen, "unknown error %d", err);
    }
    buf[buflen - 1] = '\0';
    return buf;
}

/* Hands the failing operation's context to the caller's log callback. This is
 * why there is no per-store last-error slot: the context that would justify one
 * is delivered here instead, at the point of failure, with no shared state. */
void ls_report(ls_store *s, int err, const char *key, uint64_t off, size_t n,
               const char *msg)
{
    if (s && s->o.log && err != LS_OK)
        s->o.log(err, key, off, n, msg, s->o.log_ctx);
}
