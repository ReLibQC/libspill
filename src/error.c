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
        /* The negated-errno range. strerror_r has two incompatible flavours --
         * GNU returns char* and may not touch the buffer, XSI returns int --
         * and getting this wrong prints uninitialised stack. */
        char sys[96];
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        const char *p = strerror_r(-err, sys, sizeof sys);
        snprintf(buf, buflen, "%s", p ? p : "unknown error");
#else
        if (strerror_r(-err, sys, sizeof sys) != 0)
            snprintf(sys, sizeof sys, "errno %d", -err);
        snprintf(buf, buflen, "%s", sys);
#endif
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
