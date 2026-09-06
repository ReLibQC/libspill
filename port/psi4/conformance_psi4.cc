/* Conformance check: does the shim implement Psi4's ACTUAL declarations?
 *
 * §6b measured the port against psio_types.h, our copy of Psi4's public types.
 * A copy can drift. This translation unit includes Psi4's own psio.h and takes
 * the address of every entry point the shim is meant to provide, through a
 * pointer whose type comes from that header. Linking it against the shim
 * therefore fails -- with an undefined reference -- if any definition's
 * signature does not match Psi4's declaration exactly.
 *
 *   g++ -DPSIO_USE_PSI4_HEADERS -I<psi4>/psi4/src -I<psi4>/psi4/include ...
 *
 * Not checked here, because the shim does not replace them: decode_errno,
 * psio_compose_err_msg, psio_getpid and psio_volseek stay Psi4's.
 */
#include <cstdio>

#include "psi4/libpsio/psio.h"

namespace {

/* Each initialiser both names the psi4-declared type and demands our symbol. */
int         (*p_init)()                                              = &psi::psio_init;
int         (*p_open)(size_t, int)                                   = &psi::psio_open;
int         (*p_close)(size_t, int)                                  = &psi::psio_close;
int         (*p_open_check)(size_t)                                  = &psi::psio_open_check;

psi::psio_address (*p_get_address)(psi::psio_address, size_t)        = &psi::psio_get_address;
psi::psio_address (*p_get_global)(psi::psio_address, psi::psio_address)
                                                                     = &psi::psio_get_global_address;

int (*p_write)(size_t, const char *, char *, size_t, psi::psio_address, psi::psio_address *)
                                                                     = &psi::psio_write;
int (*p_read)(size_t, const char *, char *, size_t, psi::psio_address, psi::psio_address *)
                                                                     = &psi::psio_read;
int (*p_write_entry)(size_t, const char *, char *, size_t)           = &psi::psio_write_entry;
int (*p_read_entry)(size_t, const char *, char *, size_t)            = &psi::psio_read_entry;

psi::psio_tocentry *(*p_tocscan)(size_t, const char *)               = &psi::psio_tocscan;
bool   (*p_tocexists)(size_t, const char *)                          = &psi::psio_tocentry_exists;
void   (*p_tocprint)(size_t)                                         = &psi::psio_tocprint;
int    (*p_tocwrite)(size_t)                                         = &psi::psio_tocwrite;
size_t (*p_rd_toclen)(size_t)                                        = &psi::psio_rd_toclen;

void (*p_error)(size_t, size_t, std::string)                         = &psi::psio_error;

/* The constants the shim depends on must also be Psi4's, not our copy's. A
 * wrong PSIO_PAGELEN would put every streamed address on the wrong page. */
static_assert(psi::PSIO_KEYLEN == 80,     "PSIO_KEYLEN changed in Psi4");
static_assert(psi::PSIO_PAGELEN == 65536, "PSIO_PAGELEN changed in Psi4");
static_assert(psi::PSIO_OPEN_NEW == 0,    "PSIO_OPEN_NEW changed in Psi4");
static_assert(psi::PSIO_OPEN_OLD == 1,    "PSIO_OPEN_OLD changed in Psi4");
static_assert(sizeof(psi::psio_address) == 2 * sizeof(size_t),
              "psio_address is no longer two size_t words");

}  // namespace

int main()
{
    const void *all[] = {(const void *)p_init, (const void *)p_open, (const void *)p_close,
                         (const void *)p_open_check, (const void *)p_get_address,
                         (const void *)p_get_global, (const void *)p_write, (const void *)p_read,
                         (const void *)p_write_entry, (const void *)p_read_entry,
                         (const void *)p_tocscan, (const void *)p_tocexists,
                         (const void *)p_tocprint, (const void *)p_tocwrite,
                         (const void *)p_rd_toclen, (const void *)p_error};
    size_t i, n = sizeof all / sizeof all[0];
    for (i = 0; i < n; i++)
        if (!all[i]) { std::printf("  [FAIL] null entry point %zu\n", i); return 1; }
    std::printf("  [PASS] %zu entry points match Psi4's own declarations\n", n);
    std::printf("  [PASS] PSIO_KEYLEN, PSIO_PAGELEN and the open modes are unchanged\n");
    return 0;
}
