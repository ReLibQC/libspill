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

/* Fortran symbol mangling.
 *
 * NOT called FSYM. Dalton's DALTON/include/FSYMdef.h and LSDalton's
 * src/dft/lsdalton_general.h both define a macro of that name, so an installed
 * header claiming it would collide with the very codes this layer is for.
 *
 * And the underscore is not a constant. FSYMdef.h defines `FSYM(a) a` on some
 * platforms and `FSYM(a) a ## _` on others -- abstracting the mangling is the
 * macro's whole purpose, so hardcoding one form would emit the wrong symbol
 * names wherever the other is right. The default below matches gfortran and
 * Intel on Linux; define LS_CRAY_FSYM yourself, or LIBSPILL_CRAY_NO_UNDERSCORE,
 * to match a compiler that differs. */
#ifndef LS_CRAY_FSYM
#  if defined(LIBSPILL_CRAY_NO_UNDERSCORE)
#    define LS_CRAY_FSYM(a) a
#  else
#    define LS_CRAY_FSYM(a) a##_
#  endif
#endif

void LS_CRAY_FSYM(wopen)(const CRAY_INT *unit, const char *name, const CRAY_INT *lennam,
                 const CRAY_INT *blocks, const CRAY_INT *stats, CRAY_INT *ierr);
void LS_CRAY_FSYM(wclose)(const CRAY_INT *unit, CRAY_INT *ierr);
void LS_CRAY_FSYM(getwa)(const CRAY_INT *unit, double *result, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr);
void LS_CRAY_FSYM(putwa)(const CRAY_INT *unit, const double *source, const CRAY_INT *addr,
                 const CRAY_INT *count, CRAY_INT *ierr);

void ls_crayio_reset(void);   /* test-only teardown; not part of crayio */
#endif
