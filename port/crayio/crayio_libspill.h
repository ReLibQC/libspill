/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CRAYIO_LIBSPILL_H
#define CRAYIO_LIBSPILL_H

/* The Fortran integer width, mirroring what every crayio.c does verbatim:
 *
 *     #if defined (VAR_INT64)
 *     typedef int64_t INTEGER;
 *     #else
 *     typedef int INTEGER;
 *     #endif
 *
 * THIS MUST MATCH THE CODE THE SHIM IS LINKED INTO. Every argument arrives by
 * pointer, so a mismatch is not a widening -- reading a caller's int32 through
 * an int64 pointer reads four bytes it never wrote. An earlier version of this
 * file used `long` and claimed either width would work; a 32-bit-INTEGER caller
 * got ierr = -1 from every call, because the unit number came back as garbage.
 *
 * It is the same defect, and the same cause, as the one DESIGN.md §6e records
 * in the OpenMolcas shims: Fortran external subroutines carry no interface, so
 * nothing in the toolchain will object. Build this file with the same
 * integer-width macro as the code it joins. */
#if defined(VAR_INT64)
#include <stdint.h>
typedef int64_t CRAY_INT;
#else
typedef int CRAY_INT;
#endif

/* The Fortran symbol convention the crayio copies use: lower case, one trailing
 * underscore. */
#define FSYM(a) a##_

void FSYM(wopen)(const CRAY_INT *unit, const char *name, const CRAY_INT *lennam,
                 const CRAY_INT *blocks, const CRAY_INT *stats, CRAY_INT *ierr);
void FSYM(wclose)(const CRAY_INT *unit, CRAY_INT *ierr);
void FSYM(getwa)(const CRAY_INT *unit, double *result, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr);
void FSYM(putwa)(const CRAY_INT *unit, const double *source, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr);

void cray_shim_reset(void);   /* test-only teardown */
#endif
