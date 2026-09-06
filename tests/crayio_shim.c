/* The 1980s Cray word-addressable I/O emulation, over libspill.
 *
 * §6a calls this a conformance target rather than an adoption target, and that
 * is exactly what it is: five copies of this interface live in four codes
 * (Dalton, LSDalton, MADNESS, NWChem) and none of those four will deprecate
 * anything, so this is here to answer one question -- is the API in §4
 * sufficient for the word-addressed workload? -- without asking anyone to merge
 * a line.
 *
 * Semantics taken from Dalton's DALTON/cc/crayio.c, which the LSDalton header
 * names as the common ancestor:
 *
 *   wopen (unit, name, lennam, blocks, stats, ierr)
 *   wclose(unit, ierr)                       closes; does NOT delete the file
 *   getwa (unit, result, addr, count, ierr)  addr is a 1-BASED WORD address
 *   putwa (unit, source, addr, count, ierr)  count is in 64-bit words
 *
 *   ierr:  0  success
 *         -1  unit out of range, or not open
 *         -4  addr <= 0, or count < 0, or a seek failed
 *         -5  the read would run past the end of the file
 *         -6  the open or the write failed
 *
 * `blocks` was a Cray blocking hint and is ignored by every copy; `stats` asks
 * for timing output that only PrintFileStats consumed.
 *
 * One deliberate difference. Every copy carries `#define max_file 99` (LSDalton
 * raised it to 250 after someone hit it), a fixed static table with no growth
 * path -- §6a's "bug class we delete by construction". Here the unit table
 * grows, so there is no limit to hit and no reason for a fork to raise it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libspill.h"
#include "crayio_shim.h"

/* One store per unit; the whole word-addressed space is one record, because
 * crayio addresses a unit by offset alone and has no notion of a key. */
#define CRAY_KEY "w"

typedef struct {
    ls_store *store;
    int       open;
} cray_unit;

static cray_unit *units = NULL;
static size_t     nunits = 0;

static int ensure_unit(long unit)
{
    if (unit < 0) return 0;
    if ((size_t)unit >= nunits) {
        size_t want = (size_t)unit + 1, i, n = nunits ? nunits : 16;
        cray_unit *p;
        while (n < want) n *= 2;
        p = realloc(units, n * sizeof *p);
        if (!p) return 0;
        for (i = nunits; i < n; i++) { p[i].store = NULL; p[i].open = 0; }
        units = p;
        nunits = n;
    }
    return 1;
}

static cray_unit *unit_open(long unit)
{
    if (unit < 0 || (size_t)unit >= nunits || !units[unit].open) return NULL;
    return &units[unit];
}

void FSYM(wopen)(const CRAY_INT *unit, const char *name, const CRAY_INT *lennam,
                 const CRAY_INT *blocks, const CRAY_INT *stats, CRAY_INT *ierr)
{
    char nm[256];
    ls_opts o;
    int err = 0;
    long u = (long)*unit;

    (void)blocks;   /* a Cray blocking hint; every copy ignores it */
    (void)stats;

    *ierr = 0;
    if (u < 0 || !ensure_unit(u)) { *ierr = -1; return; }
    if (units[u].open) return;                       /* already open */

    if (*lennam > 0) {
        size_t k = (size_t)*lennam;
        if (k >= sizeof nm) k = sizeof nm - 1;
        memcpy(nm, name, k);
        nm[k] = '\0';
    } else {
        sprintf(nm, "fort.%.2ld", u);                /* crayio's own default */
    }

    ls_opts_default(&o);
    units[u].store = ls_open(nm, &o, &err);
    if (!units[u].store) { *ierr = -6; return; }
    units[u].open = 1;
}

void FSYM(wclose)(const CRAY_INT *unit, CRAY_INT *ierr)
{
    cray_unit *cu = unit_open((long)*unit);
    if (!cu) { *ierr = -1; return; }
    /* crayio closes the descriptor and leaves the file: keep=1. */
    *ierr = ls_close(cu->store, 1) == LS_OK ? 0 : -6;
    cu->store = NULL;
    cu->open = 0;
}

/* The whole of the word-versus-byte translation, and the reason §6a calls the
 * shim "a multiply by eight": a 1-based word address becomes a byte offset. */
static uint64_t byte_off(CRAY_INT addr) { return (uint64_t)(addr - 1) * 8u; }

void FSYM(getwa)(const CRAY_INT *unit, double *result, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr)
{
    cray_unit *cu = unit_open((long)*unit);
    int rc;

    if (!cu)                      { *ierr = -1; return; }
    if (*addr <= 0 || *count < 0) { *ierr = -4; return; }
    if (*count == 0)              { *ierr =  0; return; }

    rc = ls_read(cu->store, CRAY_KEY, byte_off(*addr),
                 (size_t)*count * 8u, result);

    /* LS_ERR_RANGE is exactly crayio's -5: the read ran past what the file
     * holds. LS_ERR_NOKEY means nothing was ever written to this unit, which
     * crayio reaches by the same route -- length 0, so the read is past it. */
    if (rc == LS_ERR_RANGE || rc == LS_ERR_NOKEY) *ierr = -5;
    else if (rc != LS_OK)                         *ierr = -4;
    else                                          *ierr = 0;
}

void FSYM(putwa)(const CRAY_INT *unit, const double *source, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr)
{
    cray_unit *cu = unit_open((long)*unit);
    int rc;

    if (!cu)                      { *ierr = -1; return; }
    if (*addr <= 0 || *count < 0) { *ierr = -4; return; }
    if (*count == 0)              { *ierr =  0; return; }

    rc = ls_write(cu->store, CRAY_KEY, byte_off(*addr),
                  (size_t)*count * 8u, source);
    *ierr = (rc == LS_OK) ? 0 : -6;
}

void cray_shim_reset(void)
{
    size_t i;
    for (i = 0; i < nunits; i++)
        if (units[i].open) ls_close(units[i].store, 0);
    free(units);
    units = NULL;
    nunits = 0;
}
