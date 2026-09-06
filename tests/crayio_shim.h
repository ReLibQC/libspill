/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CRAYIO_SHIM_H
#define CRAYIO_SHIM_H

/* Dalton's INTEGER is 32- or 64-bit depending on the build; the shim only ever
 * widens it, so either works. */
typedef long CRAY_INT;

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
