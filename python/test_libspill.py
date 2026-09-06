# SPDX-License-Identifier: BSD-3-Clause
"""Checks §4a's Python sketch works as written, and that mapping gives NumPy
semantics with no copy."""
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libspill  # noqa: E402

nfail = ntest = 0


def ok(cond, what):
    global nfail, ntest
    ntest += 1
    print(f"  [{'PASS' if cond else 'FAIL'}] {what}")
    if not cond:
        nfail += 1


def main():
    d = tempfile.mkdtemp(prefix="lsp_")
    print("libspill Python binding")

    amps = np.arange(4096, dtype=np.float64) * 0.5 + 1.0

    # §4a, verbatim in shape
    with libspill.open("scratch", memory_budget=8 << 20, dir=d) as s:
        s["t2"] = amps                                  # whole record
        ok(np.array_equal(s["t2"], amps), "s[key] = arr then s[key] round-trips")

        blk = s.read("t2", offset=64 * 8, shape=(8, 8))  # -> ndarray
        ok(blk.shape == (8, 8) and blk[0, 0] == amps[64],
           "read(offset=..., shape=...) returns a shaped ndarray")

        buf = np.empty(4096, dtype=np.float64)
        s.read("t2", out=buf)                            # no allocation
        ok(np.array_equal(buf, amps), "read(out=...) fills a caller-supplied array")

        partial = np.full(4096, 2.0)
        s.accumulate("t2", 0, partial, alpha=0.5)
        ok(np.allclose(s["t2"], amps + 1.0), "accumulate(..., alpha=0.5)")

        nxt = amps * 2.0
        fut = s.awrite("t2", nxt)
        fut.wait()
        ok(np.array_equal(s["t2"], nxt), "awrite(...); fut.wait()")

        got = np.empty(4096, dtype=np.float64)
        fut = s.aread("t2", got)
        fut.wait()
        ok(np.array_equal(got, nxt), "aread into a caller array")

        s["other"] = np.arange(10, dtype=np.int64)
        ok(sorted(list(s)) == ["other", "t2"], "list(s) gives the table of contents")
        ok("t2" in s and len(s) == 2, "membership and len")

        off = s.append("stream", amps)
        ok(off == 0, "append reports its offset")
        off = s.append("stream", amps)
        ok(off == amps.nbytes, "  ... and the next one follows it")

        s.set_attr("t2", b"f8/(64,64)")
        ok(s.get_attr("t2") == b"f8/(64,64)", "attributes round-trip as bytes")

        # dtypes other than the two supplied reductions are refused, not guessed
        try:
            s.accumulate("other", 0, np.arange(10, dtype=np.int64))
            refused = False
        except libspill.Error:
            refused = True
        ok(refused, "accumulate on int64 is refused rather than guessed")

        # failures carry the C code
        try:
            s.read("absent")
            code = None
        except libspill.Error as e:
            code = e.code
        ok(code == libspill.LS_ERR_NOKEY, "a missing key raises Error with LS_ERR_NOKEY")
        ok(issubclass(libspill.Error, OSError), "Error is an OSError")

    ok(not os.path.exists(os.path.join(d, "scratch.libspill")),
       "the context manager unlinked the store on exit")

    # ---- §4a: "LS_MAPPED is where the Python binding earns the most" ----
    with libspill.open("cache", dir=d, keep=True) as s:
        eri = np.arange(8192, dtype=np.float64)
        s["eri"] = eri

    with libspill.open("cache", dir=d, mode=libspill.MAPPED, keep=True) as s:
        arr = s.map("eri")
        ok(isinstance(arr, np.ndarray) and arr.shape == (8192,),
           "map() returns an ndarray")
        ok(np.array_equal(arr, eri), "  ... with the stored values")
        # no copy: the array must share memory with the mapping, so writing
        # through it reaches the store
        arr[0] = -1.0
        arr[8191] = -2.0
        s.unmap("eri")

    with libspill.open("cache", dir=d, keep=True) as s:
        back = s["eri"]
        ok(back[0] == -1.0 and back[8191] == -2.0,
           "writes through the mapped array reached the store -- no copy")

    with libspill.open("cache", dir=d, mode=libspill.MAPPED, keep=True) as s:
        arr = s.map("eri", shape=(64, 128))
        ok(arr.shape == (64, 128), "map(shape=...) reshapes without copying")
        ok(arr.base is not None, "  ... and it is a view, not a copy")

    with libspill.open("cache", dir=d) as s:
        pass  # unlinked here

    print(f"{ntest} checks, {nfail} failed")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
