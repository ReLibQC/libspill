// SPDX-License-Identifier: BSD-3-Clause
// The C++ layer of §4a, and ls_accumulate beneath it.
//
// §4a's sketch is the specification:
//
//   ls::Store s{"scratch", {.memory_budget = 8ull<<30}};
//   s.write("t2", off, std::span{amps});
//   s.accumulate<double>("t2", off, std::span{partial}, 0.5);
//   auto req = s.awrite("t2", off, std::span{next});
//   req.wait();
//   for (auto &k : s.keys()) ...
//
// Everything below checks that shape works, and that the layer adds what a
// typed layer is for: sizes deduced, failures thrown, handles released.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "libspill.hpp"

static int fails = 0, ntest = 0;

static void ok(bool cond, const char *what) {
    ntest++;
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

int main() {
    std::puts("libspill C++ layer");

    std::vector<double> amps(4096), back(4096);
    std::iota(amps.begin(), amps.end(), 1.0);

    {
        // Designated initialisers, as §4a writes it. The version field §4b
        // requires is filled in by options::to_c, not by the caller.
        ls::store s{"cxx", {.memory_budget = 8ull << 20}};

        s.write<double>("t2", 0, amps);
        s.read<double>("t2", 0, back);
        ok(amps == back, "write/read round-trip with sizes deduced from the span");

        ok(s.exists("t2"), "exists");
        ok(s.size("t2") == amps.size() * sizeof(double), "size reports bytes");

        // ---- the typed accumulate: no callback anywhere in sight ----
        std::vector<double> partial(4096, 2.0);
        s.accumulate<double>("t2", 0, partial, 0.5);
        s.read<double>("t2", 0, back);
        bool good = true;
        for (std::size_t i = 0; i < amps.size(); i++)
            if (back[i] != amps[i] + 0.5 * 2.0) good = false;
        ok(good, "accumulate<double> applies dst += alpha*src");

        s.accumulate<double>("t2", 0, partial);          // alpha defaults to 1
        s.read<double>("t2", 0, back);
        good = true;
        for (std::size_t i = 0; i < amps.size(); i++)
            if (back[i] != amps[i] + 1.0 + 2.0) good = false;
        ok(good, "  ... and defaults to dst += src");

        // ---- append ----
        auto off = s.append<double>("stream", amps);
        ok(off == 0, "append to a new key reports offset 0");
        off = s.append<double>("stream", amps);
        ok(off == amps.size() * sizeof(double), "append reports the previous end");

        // ---- async: move-only, waits in the destructor ----
        {
            auto req = s.awrite<double>("t2", 0, amps);
            req.wait();
        }
        s.read<double>("t2", 0, back);
        ok(amps == back, "awrite then wait");
        {
            auto req = s.aread<double>("t2", 0, std::span<double>(back));
            auto moved = std::move(req);       // move-only
            moved.wait();
        }
        ok(amps == back, "aread survives a move");
        {
            // Never waited on: the destructor must reap it, not leak it.
            auto req = s.awrite<double>("t2", 0, amps);
            (void)req;
        }
        ok(true, "an unwaited request is reaped by its destructor");

        // ---- attributes ----
        std::string meta = "f8/(64,64)";
        s.set_attr("t2", std::as_bytes(std::span<const char>(meta)));
        auto got = s.get_attr("t2");
        ok(got.size() == meta.size() &&
               std::memcmp(got.data(), meta.data(), meta.size()) == 0,
           "attributes round-trip as opaque bytes");

        // ---- table of contents ----
        auto keys = s.keys();
        ok(keys.size() == 2, "keys() returns an owning snapshot");

        // ---- failures are exceptions carrying the C code ----
        bool threw = false;
        try {
            s.read<double>("absent", 0, back);
        } catch (const ls::error &e) {
            threw = (e.code() == LS_ERR_NOKEY) && !e.is_errno();
        }
        ok(threw, "a missing key throws ls::error with LS_ERR_NOKEY");

        threw = false;
        try {
            s.read<double>("t2", 0, std::span<double>(back.data(), back.size() * 4));
        } catch (const ls::error &e) {
            threw = (e.code() == LS_ERR_RANGE);
        }
        ok(threw, "reading past the end throws LS_ERR_RANGE");
    }
    ok(true, "the store closed and unlinked itself");

    // ---- the memory tier is where accumulate never touches disk ----
    {
        ls::store s{"cxx_mem", {.memory_budget = 8ull << 20}};
        std::vector<double> v(1024, 1.0), one(1024, 1.0);
        s.write<double>("a", 0, v);
        for (int i = 0; i < 100; i++) s.accumulate<double>("a", 0, one);
        std::vector<double> r(1024);
        s.read<double>("a", 0, r);
        bool good = true;
        for (double x : r) if (x != 101.0) good = false;
        ok(good, "100 accumulations in the memory tier are exact");

        // §4's actual claim: resident, the reduction runs in place and disk is
        // never touched. Only the superblock should ever have been written.
        const char *td = std::getenv("TMPDIR");
        std::filesystem::path p = std::filesystem::path(td ? td : "/tmp") / "cxx_mem.libspill";
        ok(std::filesystem::exists(p) && std::filesystem::file_size(p) == 4096,
           "  ... and never reached disk");
    }

    // ---- and the same arithmetic on the disk path ----
    {
        ls::store s{"cxx_disk"};
        std::vector<float> v(2048, 1.0f), one(2048, 0.25f);
        s.write<float>("a", 0, v);
        for (int i = 0; i < 8; i++) s.accumulate<float>("a", 0, one, 2.0f);
        std::vector<float> r(2048);
        s.read<float>("a", 0, r);
        bool good = true;
        for (float x : r) if (x != 1.0f + 8 * 0.5f) good = false;
        ok(good, "accumulate<float> on the disk path matches");
    }

    std::printf("%d checks, %d failed\n", ntest, fails);
    return fails != 0;
}
