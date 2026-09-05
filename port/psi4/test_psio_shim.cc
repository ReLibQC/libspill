/* Exercises the shim the way Psi4 exercises libpsio. The patterns here were
 * taken from the call sites, not invented: the streaming write/read loop that
 * threads psio_address through a sequence of blocks is what libdpd, libtrans and
 * the cc modules do everywhere, and it is the one that would break if the
 * (page, offset) coordinate were not linear. */
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "psio_libspill.h"

using namespace psi;

static int fails = 0, ntest = 0;

static void ok(bool cond, const char *what) {
    ntest++;
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    psio_set_scratch_dir(dir);
    psio_init();

    const size_t UNIT = 35;                       /* PSIF_CC_TAMPS, in Psi4 */
    std::printf("Psi4 libpsio shim on libspill (scratch: %s)\n", dir);

    /* ---- whole-entry round trip, the read_entry/write_entry pattern ---- */
    psio_open(UNIT, PSIO_OPEN_NEW);
    ok(psio_open_check(UNIT) == 1, "open_check reports the unit open");

    std::vector<double> t2(4096), back(4096);
    for (size_t i = 0; i < t2.size(); i++) t2[i] = 1.0 + 0.5 * i;

    psio_write_entry(UNIT, "New tIJAB Amplitudes", (char *)t2.data(), t2.size() * sizeof(double));
    psio_read_entry(UNIT, "New tIJAB Amplitudes", (char *)back.data(), back.size() * sizeof(double));
    ok(std::memcmp(t2.data(), back.data(), t2.size() * sizeof(double)) == 0,
       "write_entry / read_entry round-trips byte-exactly");

    /* ---- the streaming pattern, and the page boundary it crosses ----
     * PSIO_PAGELEN is 65536 bytes = 8192 doubles. Writing 40 blocks of 4096
     * doubles crosses the boundary twenty times, so a wrong linearisation of
     * (page, offset) shows up here and nowhere else. */
    {
        const int NBLK = 40;
        psio_address next = PSIO_ZERO;
        std::vector<double> blk(4096);
        for (int b = 0; b < NBLK; b++) {
            for (size_t i = 0; i < blk.size(); i++) blk[i] = 1000.0 * b + i;
            psio_write(UNIT, "Streamed", (char *)blk.data(), blk.size() * sizeof(double), next, &next);
        }
        ok(next.page == (NBLK * 4096 * sizeof(double)) / PSIO_PAGELEN,
           "the threaded address lands on the right page");

        bool good = true;
        next = PSIO_ZERO;
        for (int b = 0; b < NBLK; b++) {
            psio_read(UNIT, "Streamed", (char *)blk.data(), blk.size() * sizeof(double), next, &next);
            for (size_t i = 0; i < blk.size(); i++)
                if (blk[i] != 1000.0 * b + (double)i) { good = false; break; }
        }
        ok(good, "40 blocks stream back exactly across 20 page boundaries");
    }

    /* ---- a read starting mid-page, which is where offset arithmetic bites --- */
    {
        std::vector<double> one(1);
        psio_address a = psio_get_address(PSIO_ZERO, 5000 * sizeof(double));
        psio_read(UNIT, "Streamed", (char *)one.data(), sizeof(double), a, nullptr);
        ok(one[0] == 1904.0,   /* index 5000 = block 1, element 904 */
           "an unaligned mid-page read returns the right element");
    }

    /* ---- tocscan is an existence test, which is all Psi4 asks of it ---- */
    ok(psio_tocscan(UNIT, "New tIJAB Amplitudes") != nullptr, "tocscan finds a written entry");
    ok(psio_tocscan(UNIT, "Never Written") == nullptr, "tocscan returns null for an absent entry");
    ok(psio_tocentry_exists(UNIT, "Streamed"), "tocentry_exists agrees");
    {
        psio_tocentry *e = psio_tocscan(UNIT, "New tIJAB Amplitudes");
        ok(e && e->eadd.page * PSIO_PAGELEN + e->eadd.offset == 4096 * sizeof(double),
           "the entry it returns carries the right end address");
    }

    /* ---- zero_disk: rows*cols writes in Psi4, one reserve here ---- */
    psio_zero_disk(UNIT, "Zeroed", 64, 512);
    {
        std::vector<double> z(512, 7.0);
        psio_read(UNIT, "Zeroed", (char *)z.data(), z.size() * sizeof(double),
                  psio_get_address(PSIO_ZERO, 63 * 512 * sizeof(double)), nullptr);
        bool allzero = true;
        for (double v : z) if (v != 0.0) allzero = false;
        ok(allzero, "zero_disk zeroes the whole extent, last row included");
    }

    /* ---- errors reach the caller as Psi4 errors ---- */
    {
        bool threw = false;
        std::vector<double> tmp(8);
        try {
            psio_read(UNIT, "Never Written", (char *)tmp.data(), sizeof(double), PSIO_ZERO, nullptr);
        } catch (const std::runtime_error &) { threw = true; }
        ok(threw, "reading a missing entry raises a PSIO error");

        threw = false;
        try {
            psio_read(UNIT, "New tIJAB Amplitudes", (char *)tmp.data(), sizeof(double),
                      psio_get_address(PSIO_ZERO, 4096 * sizeof(double)), nullptr);
        } catch (const std::runtime_error &) { threw = true; }
        ok(threw, "reading past the end of an entry raises a PSIO error");
    }

    /* ---- PSIO_OPEN_OLD keeps, PSIO_OPEN_NEW discards ---- */
    psio_close(UNIT, 1);
    psio_open(UNIT, PSIO_OPEN_OLD);
    std::memset(back.data(), 0, back.size() * sizeof(double));
    psio_read_entry(UNIT, "New tIJAB Amplitudes", (char *)back.data(), back.size() * sizeof(double));
    ok(std::memcmp(t2.data(), back.data(), t2.size() * sizeof(double)) == 0,
       "close(keep=1) then OPEN_OLD preserves the entry");

    psio_close(UNIT, 1);
    psio_open(UNIT, PSIO_OPEN_NEW);
    ok(psio_tocscan(UNIT, "New tIJAB Amplitudes") == nullptr,
       "OPEN_NEW discards what a previous run left");
    psio_close(UNIT, 0);

    psio_done();
    std::printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
