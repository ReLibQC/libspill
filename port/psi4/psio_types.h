/* Deliberately NOT carrying libspill's SPDX tag.
 *
 * This file transcribes the public types and constants of Psi4's libpsio --
 * struct layouts, enumerator values, PSIO_PAGELEN -- so that the shim compiles
 * and is tested outside the Psi4 tree. They are interoperability facts about
 * someone else's interface rather than libspill's own work, and Psi4 is
 * LGPL-3. Nothing here is needed when building inside Psi4: define
 * PSIO_USE_PSI4_HEADERS and psio.h supplies all of it, which is also the
 * conformance build (§6e). Whoever ships libspill should decide how to label
 * this file; the rest of the repository is BSD-3-Clause.
 *
 * The public types and constants of Psi4's libpsio, reproduced so the
 * shim compiles and is tested outside the Psi4 tree. In Psi4 this file is not
 * used at all: psio.h and config.h already declare these, unchanged. Keeping a
 * copy here is what lets the port be exercised before Psi4 is rebuilt.
 *
 * Values checked against psi4/src/psi4/libpsio/config.h at a0e6ba5c4. */
#ifndef PSIO_TYPES_H
#define PSIO_TYPES_H

#include <cstddef>
#include <string>

namespace psi {

inline constexpr int PSIO_OPEN_NEW = 0;
inline constexpr int PSIO_OPEN_OLD = 1;

inline constexpr int PSIO_KEYLEN = 80;
inline constexpr int PSIO_MAXUNIT = 500;
inline constexpr int PSIO_PAGELEN = 65536;

inline constexpr int PSIO_ERROR_OPEN = 5;
inline constexpr int PSIO_ERROR_CLOSE = 7;
inline constexpr int PSIO_ERROR_READ = 11;
inline constexpr int PSIO_ERROR_WRITE = 12;
inline constexpr int PSIO_ERROR_NOTOCENT = 13;
inline constexpr int PSIO_ERROR_KEYLEN = 15;
inline constexpr int PSIO_ERROR_BLKSTART = 17;
inline constexpr int PSIO_ERROR_BLKEND = 18;
inline constexpr int PSIO_ERROR_MAXUNIT = 20;
inline constexpr int PSIO_ERROR_UNOPENED = 21;

struct psio_address {
    size_t page;
    size_t offset;
};

typedef struct psio_entry {
    char key[PSIO_KEYLEN];
    psio_address sadd;
    psio_address eadd;
    struct psio_entry *next;
    struct psio_entry *last;
} psio_tocentry;

extern psio_address PSIO_ZERO;

}  // namespace psi
#endif
