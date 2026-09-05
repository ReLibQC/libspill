#include "psio_libscratch.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "libscratch.h"
}

namespace psi {

psio_address PSIO_ZERO = {0, 0};

namespace {

std::string g_dir;                       /* PSIOManager's job in Psi4 */
std::mutex  g_lk;

struct Unit {
    ls_store *store = nullptr;
    /* Entries handed back by tocscan. Psi4 never dereferences these, but a
     * function that returns a pointer should return a valid one; keeping them
     * per unit means the pointer stays good until the unit is closed, which is
     * the lifetime the old linked-list TOC gave. */
    std::map<std::string, psio_tocentry> toc;
};

std::map<size_t, Unit> g_units;

Unit *find_unit(size_t unit) {
    auto it = g_units.find(unit);
    return it == g_units.end() ? nullptr : &it->second;
}

/* (page, offset) -> byte offset. Exact, because psio_get_address is just
 * addition in these coordinates; see the header. */
inline uint64_t to_bytes(psio_address a) {
    return static_cast<uint64_t>(a.page) * static_cast<uint64_t>(PSIO_PAGELEN) + a.offset;
}

inline psio_address to_address(uint64_t b) {
    psio_address a;
    a.page = static_cast<size_t>(b / PSIO_PAGELEN);
    a.offset = static_cast<size_t>(b % PSIO_PAGELEN);
    return a;
}

/* libscratch's codes carry more detail than Psi4's, so the mapping loses
 * information on purpose: Psi4's call sites only ever branch on which of its
 * own PSIO_ERROR_* values came back. The detail is not thrown away -- the log
 * callback below reports it with the key and offset attached, which is more
 * than the old layer ever managed. */
int psio_err_of(int rc, int on_read) {
    switch (rc) {
        case LS_ERR_NOKEY:   return PSIO_ERROR_NOTOCENT;
        case LS_ERR_RANGE:   return PSIO_ERROR_BLKEND;
        case LS_ERR_INVAL:   return PSIO_ERROR_KEYLEN;
        default:             return on_read ? PSIO_ERROR_READ : PSIO_ERROR_WRITE;
    }
}

void log_cb(int err, const char *key, uint64_t off, size_t nbytes, const char *msg, void *) {
    char buf[LS_ERRBUF_MIN];
    std::fprintf(stderr, "PSIO/libscratch: %s (key %s, offset %llu, %zu bytes): %s\n",
                 msg ? msg : "operation", key ? key : "-",
                 static_cast<unsigned long long>(off), nbytes,
                 ls_strerror(err, buf, sizeof buf));
}

std::string name_of(size_t unit) { return "psi." + std::to_string(unit); }

}  // namespace

void psio_set_scratch_dir(const char *dir) { g_dir = dir ? dir : ""; }

/* ------------------------------------------------------------------ addresses
 * Psi4's own implementation, kept because it is the definition of the
 * coordinate system rather than an implementation detail. */
psio_address psio_get_address(psio_address start, size_t shift) {
    return to_address(to_bytes(start) + shift);
}

psio_address psio_get_global_address(psio_address entry_start, psio_address rel_address) {
    return to_address(to_bytes(entry_start) + to_bytes(rel_address));
}

void psio_error(size_t unit, size_t errval, std::string prev_msg) {
    std::fprintf(stderr, "PSIO_ERROR: unit %zu, errval %zu %s\n", unit, errval,
                 prev_msg.c_str());
    throw std::runtime_error("PSIO error " + std::to_string(errval) + " on unit " +
                             std::to_string(unit) + " " + prev_msg);
}

int psio_init() { return 1; }

int psio_done() {
    std::lock_guard<std::mutex> g(g_lk);
    for (auto &kv : g_units)
        if (kv.second.store) ls_close(kv.second.store, 0);
    g_units.clear();
    return 1;
}

/* ----------------------------------------------------------------- lifecycle */

int psio_open(size_t unit, int status) {
    std::lock_guard<std::mutex> g(g_lk);

    if (unit >= static_cast<size_t>(PSIO_MAXUNIT)) psio_error(unit, PSIO_ERROR_MAXUNIT);
    if (find_unit(unit)) return 1;                    /* already open */

    ls_opts o;
    ls_opts_default(&o);
    if (!g_dir.empty()) o.dir = g_dir.c_str();
    o.log = log_cb;

    /* PSIO_OPEN_NEW must not inherit a previous run's contents. libscratch
     * reopens a kept store when one is there, so a NEW unit removes it first. */
    if (status == PSIO_OPEN_NEW) {
        int err = 0;
        ls_store *old = ls_open(name_of(unit).c_str(), &o, &err);
        if (old) ls_close(old, 0);                    /* keep=0 unlinks */
    }

    int err = 0;
    ls_store *s = ls_open(name_of(unit).c_str(), &o, &err);
    if (!s) psio_error(unit, PSIO_ERROR_OPEN);

    Unit u;
    u.store = s;
    g_units[unit] = u;
    return 1;
}

int psio_close(size_t unit, int keep) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u) psio_error(unit, PSIO_ERROR_UNOPENED);

    int rc = ls_close(u->store, keep);
    g_units.erase(unit);
    if (rc != LS_OK) psio_error(unit, PSIO_ERROR_CLOSE);
    return 1;
}

int psio_open_check(size_t unit) {
    std::lock_guard<std::mutex> g(g_lk);
    return find_unit(unit) != nullptr ? 1 : 0;
}

/* ---------------------------------------------------------------- data path */

namespace {

int rw(size_t unit, const char *key, char *buffer, size_t size, psio_address sadd,
       psio_address *eadd, int is_write) {
    std::lock_guard<std::mutex> g(g_lk);

    Unit *u = find_unit(unit);
    if (!u) psio_error(unit, PSIO_ERROR_UNOPENED);
    if (std::strlen(key) >= static_cast<size_t>(PSIO_KEYLEN)) psio_error(unit, PSIO_ERROR_KEYLEN);

    const uint64_t off = to_bytes(sadd);
    const int rc = is_write ? ls_write(u->store, key, off, size, buffer)
                            : ls_read (u->store, key, off, size, buffer);
    if (rc != LS_OK) psio_error(unit, psio_err_of(rc, !is_write));

    if (eadd) *eadd = to_address(off + size);
    return 1;
}

}  // namespace

int psio_write(size_t unit, const char *key, char *buffer, size_t size, psio_address sadd,
               psio_address *eadd) {
    return rw(unit, key, buffer, size, sadd, eadd, 1);
}

int psio_read(size_t unit, const char *key, char *buffer, size_t size, psio_address sadd,
              psio_address *eadd) {
    return rw(unit, key, buffer, size, sadd, eadd, 0);
}

int psio_write_entry(size_t unit, const char *key, char *buffer, size_t size) {
    psio_address end;
    return psio_write(unit, key, buffer, size, PSIO_ZERO, &end);
}

int psio_read_entry(size_t unit, const char *key, char *buffer, size_t size) {
    psio_address end;
    return psio_read(unit, key, buffer, size, PSIO_ZERO, &end);
}

/* ------------------------------------------------------- table of contents */

psio_tocentry *psio_tocscan(size_t unit, const char *key) {
    std::lock_guard<std::mutex> g(g_lk);

    Unit *u = find_unit(unit);
    if (!u || !key) return nullptr;

    uint64_t nbytes = 0;
    if (ls_size(u->store, key, &nbytes) != LS_OK) return nullptr;

    psio_tocentry &e = u->toc[key];
    std::snprintf(e.key, PSIO_KEYLEN, "%s", key);
    e.sadd = PSIO_ZERO;
    e.eadd = to_address(nbytes);
    e.next = nullptr;
    e.last = nullptr;
    return &e;
}

bool psio_tocentry_exists(size_t unit, const char *key) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u || !key) return false;
    int found = 0;
    return ls_exists(u->store, key, &found) == LS_OK && found;
}

bool psio_tocdel(size_t unit, const char *key) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u || !key) return false;
    u->toc.erase(key);
    return ls_erase(u->store, key) == LS_OK;
}

void psio_tocprint(size_t unit) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u) return;

    char **keys = nullptr;
    size_t n = 0;
    if (ls_keys(u->store, &keys, &n) != LS_OK) return;
    for (size_t i = 0; i < n; i++) {
        uint64_t sz = 0;
        ls_size(u->store, keys[i], &sz);
        std::printf("%-80s %llu\n", keys[i], static_cast<unsigned long long>(sz));
    }
    ls_keys_free(keys, n);
}

/* The on-disk table of contents is libscratch's now, so these have nothing left
 * to do. tocwrite has exactly one consumer outside libpsio and rd_toclen has
 * none, which is why that ownership transfer is free. */
int psio_tocwrite(size_t) { return 1; }

size_t psio_rd_toclen(size_t unit) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u) return 0;
    char **keys = nullptr;
    size_t n = 0;
    if (ls_keys(u->store, &keys, &n) != LS_OK) return 0;
    ls_keys_free(keys, n);
    return n;
}

/* Psi4 writes rows*cols zeroed doubles one row at a time -- `rows` separate
 * write calls through the whole stack. ls_reserve is one call, and on Linux one
 * fallocate. This is the single clearest example of what the port buys. */
void psio_zero_disk(size_t unit, const char *key, size_t rows, size_t cols) {
    std::lock_guard<std::mutex> g(g_lk);
    Unit *u = find_unit(unit);
    if (!u) psio_error(unit, PSIO_ERROR_UNOPENED);

    const int rc = ls_reserve(u->store, key,
                              static_cast<uint64_t>(rows) * cols * sizeof(double));
    if (rc != LS_OK) psio_error(unit, psio_err_of(rc, 0));
}

}  // namespace psi
