# SPDX-License-Identifier: BSD-3-Clause
"""libspill -- Python binding.

DESIGN.md §4a: a context manager, NumPy throughout, and reads into a
caller-supplied array when the caller wants to control allocation. The C entry
points stay public and supported, but no Python consumer should have to touch
them.

This is ctypes over the C ABI rather than an extension module, on purpose: §4b
made that ABI the stable surface, so a binding that goes through it needs no
compiler at install time and no rebuild when libspill is upgraded in place.

§4a is also right that LS_MAPPED is where this layer earns the most. A mapped
record is a NumPy array backed by the mapping, so

    arr = s.map("eri")

gives array semantics with no copy at all, which is qp2's access pattern in one
line.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from contextlib import contextmanager

import numpy as np

__all__ = [
    "Store", "open", "Error", "LS_OK", "LS_ERR_NOKEY", "LS_ERR_RANGE",
    "LS_ERR_INVAL", "LS_ERR_MODE", "LS_ERR_BACKEND", "LS_ERR_BUSY",
    "LS_ERR_CORRUPT", "POSIX", "HDF5", "EXPLICIT", "MAPPED", "LOCAL",
    "PER_RANK", "SHARED", "ATTR_MAX", "KEY_MAX", "store_exists",
]

LS_OK, LS_ERR_NOKEY, LS_ERR_RANGE, LS_ERR_INVAL = 0, -1000, -1001, -1002
LS_ERR_MODE, LS_ERR_BACKEND, LS_ERR_BUSY, LS_ERR_CORRUPT = -1003, -1004, -1005, -1006

POSIX, HDF5 = 0, 1
EXPLICIT, MAPPED = 0, 1
LOCAL, PER_RANK, SHARED = 0, 1, 2

KEY_MAX, ATTR_MAX = 255, 256
_OPTS_VERSION = 2


def _load():
    env = os.environ.get("LIBSPILL_LIBRARY")
    if env:
        return ctypes.CDLL(env)
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, os.pardir, "libspill.so"),
                 os.path.join(here, "libspill.so"),
                 "libspill.so"):
        try:
            return ctypes.CDLL(cand)
        except OSError:
            continue
    found = ctypes.util.find_library("spill")
    if found:
        return ctypes.CDLL(found)
    raise ImportError(
        "cannot find libspill.so; build it with `make` or set LIBSPILL_LIBRARY")


_lib = _load()


class _Opts(ctypes.Structure):
    """Mirrors ls_opts. The version field §4b requires is set by _defaults."""
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("backend", ctypes.c_int),
        ("mode", ctypes.c_int),
        ("parallel", ctypes.c_int),
        ("rank", ctypes.c_int),
        ("memory_budget", ctypes.c_size_t),
        ("dir", ctypes.c_char_p),
        ("direct_io", ctypes.c_int),
        ("log", ctypes.c_void_p),
        ("log_ctx", ctypes.c_void_p),
        ("exact_name", ctypes.c_int),      # LS_OPTS_VERSION 2
    ]


_P = ctypes.POINTER
_lib.ls_opts_default.argtypes = [_P(_Opts)]
_lib.ls_open.restype = ctypes.c_void_p
_lib.ls_open.argtypes = [ctypes.c_char_p, _P(_Opts), _P(ctypes.c_int)]
_lib.ls_store_exists.argtypes = [ctypes.c_char_p, _P(_Opts), _P(ctypes.c_int)]
_lib.ls_opts_size.restype = ctypes.c_size_t
_lib.ls_opts_size.argtypes = [ctypes.c_uint32]
_lib.ls_opts_init.argtypes = [_P(_Opts), ctypes.c_uint32]

# _Opts mirrors a C struct, so a layout disagreement is silent memory
# corruption rather than an error. Make it loud, once, at import.
_want = _lib.ls_opts_size(_OPTS_VERSION)
if _want == 0:
    raise ImportError(
        f"libspill does not know ls_opts version {_OPTS_VERSION}; "
        "the library is older than this binding")
if ctypes.sizeof(_Opts) != _want:
    raise ImportError(
        f"ls_opts layout mismatch: this binding mirrors {ctypes.sizeof(_Opts)} "
        f"bytes for version {_OPTS_VERSION}, the library says {_want}. "
        "The binding and libspill.so are out of step.")
_lib.ls_close.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.ls_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64,
                          ctypes.c_size_t, ctypes.c_void_p]
_lib.ls_read.argtypes = _lib.ls_write.argtypes
_lib.ls_append.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                           ctypes.c_void_p, _P(ctypes.c_uint64)]
_lib.ls_accumulate.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64,
                               ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_void_p]
_lib.ls_add_f64.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                            ctypes.c_void_p]
_lib.ls_add_f32.argtypes = _lib.ls_add_f64.argtypes
_lib.ls_exists.argtypes = [ctypes.c_void_p, ctypes.c_char_p, _P(ctypes.c_int)]
_lib.ls_size.argtypes = [ctypes.c_void_p, ctypes.c_char_p, _P(ctypes.c_uint64)]
_lib.ls_erase.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.ls_reserve.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64]
_lib.ls_keys.argtypes = [ctypes.c_void_p, _P(_P(ctypes.c_char_p)), _P(ctypes.c_size_t)]
_lib.ls_keys_free.argtypes = [_P(ctypes.c_char_p), ctypes.c_size_t]
_lib.ls_set_attr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.c_size_t]
_lib.ls_get_attr.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
                             _P(ctypes.c_size_t)]
_lib.ls_awrite.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64,
                           ctypes.c_size_t, ctypes.c_void_p, _P(ctypes.c_void_p)]
_lib.ls_aread.argtypes = _lib.ls_awrite.argtypes
_lib.ls_wait.argtypes = [ctypes.c_void_p]
_lib.ls_test.argtypes = [ctypes.c_void_p, _P(ctypes.c_int)]
_lib.ls_map.argtypes = [ctypes.c_void_p, ctypes.c_char_p, _P(ctypes.c_void_p),
                        _P(ctypes.c_size_t)]
_lib.ls_unmap.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.ls_strerror.restype = ctypes.c_char_p
_lib.ls_strerror.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
_lib.ls_version.restype = ctypes.c_int
_lib.ls_version_string.restype = ctypes.c_char_p


def strerror(code: int) -> str:
    buf = ctypes.create_string_buffer(128)
    _lib.ls_strerror(code, buf, len(buf))
    return buf.value.decode()


class Error(OSError):
    """Raised by every failing operation.

    Subclasses OSError so that ``except OSError`` catches it, and keeps the C
    code in ``.code`` -- §4b makes -ENOSPC meaningful, and a caller that wants
    to shrink its block size rather than abort still can.
    """

    def __init__(self, code: int, what: str):
        self.code = code
        super().__init__(f"{what}: {strerror(code)}")

    @property
    def is_errno(self) -> bool:
        return -1000 < self.code < 0


def _check(rc: int, what: str) -> None:
    if rc != LS_OK:
        raise Error(rc, what)


def _contig(a):
    """Anything crossing the boundary must be C-contiguous and own real bytes."""
    arr = np.ascontiguousarray(a)
    return arr


class Request:
    """An in-flight operation. ``wait()`` both reports status and releases it,
    so a request that is dropped unwaited is waited for on collection."""

    __slots__ = ("_req", "_keepalive")

    def __init__(self, req, keepalive):
        self._req = req
        self._keepalive = keepalive     # the buffer must outlive the transfer

    def done(self) -> bool:
        if self._req is None:
            return True
        d = ctypes.c_int(0)
        _check(_lib.ls_test(self._req, ctypes.byref(d)), "ls_test")
        return bool(d.value)

    def wait(self) -> None:
        if self._req is None:
            return
        req, self._req = self._req, None
        rc = _lib.ls_wait(req)
        self._keepalive = None
        _check(rc, "ls_wait")

    def __del__(self):
        if getattr(self, "_req", None) is not None:
            _lib.ls_wait(self._req)
            self._req = None


class Store:
    """A scratch store. Use it as a context manager; it unlinks on exit unless
    ``keep=True``."""

    def __init__(self, name, *, memory_budget=0, dir=None, direct_io=False,
                 backend=POSIX, mode=EXPLICIT, parallel=LOCAL, rank=-1,
                 keep=False, exact_name=False):
        o = _Opts()
        # ls_opts_init with OUR version, never ls_opts_default: that symbol
        # fills the newest version and would write past a mirror of an older
        # layout.
        _lib.ls_opts_init(ctypes.byref(o), _OPTS_VERSION)
        o.backend = backend
        o.mode = mode
        o.parallel = parallel
        o.rank = rank
        o.memory_budget = memory_budget
        o.dir = dir.encode() if dir else None
        o.direct_io = 1 if direct_io else 0
        o.exact_name = 1 if exact_name else 0
        err = ctypes.c_int(0)
        self._s = _lib.ls_open(name.encode(), ctypes.byref(o), ctypes.byref(err))
        if not self._s:
            raise Error(err.value, f"ls_open({name!r})")
        self._keep = keep
        self._mode = mode
        self._maps = {}

    # ---- lifecycle -----------------------------------------------------

    def close(self):
        if getattr(self, "_s", None) is None:
            return
        self._maps.clear()
        s, self._s = self._s, None
        _check(_lib.ls_close(s, 1 if self._keep else 0), "ls_close")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        if getattr(self, "_s", None) is not None:
            _lib.ls_close(self._s, 1 if self._keep else 0)
            self._s = None

    def _k(self, key):
        b = key.encode() if isinstance(key, str) else key
        if not b or len(b) > KEY_MAX:
            raise Error(LS_ERR_INVAL, f"key {key!r}")
        return b

    # ---- data ----------------------------------------------------------

    def write(self, key, value, offset=0):
        a = _contig(value)
        _check(_lib.ls_write(self._s, self._k(key), offset, a.nbytes,
                             a.ctypes.data_as(ctypes.c_void_p)), "ls_write")

    def read(self, key, offset=0, *, shape=None, dtype=np.float64, out=None,
             count=None):
        """Read into ``out`` when given -- the caller controls allocation --
        otherwise allocate from ``shape``/``count`` and ``dtype``."""
        if out is not None:
            a = out
            if not a.flags["C_CONTIGUOUS"]:
                raise Error(LS_ERR_INVAL, "out array must be C-contiguous")
        else:
            if shape is None:
                if count is None:
                    remaining = self.size(key) - offset
                    count = remaining // np.dtype(dtype).itemsize
                shape = (int(count),)
            a = np.empty(shape, dtype=dtype)
        _check(_lib.ls_read(self._s, self._k(key), offset, a.nbytes,
                            a.ctypes.data_as(ctypes.c_void_p)), "ls_read")
        return a

    def append(self, key, value) -> int:
        a = _contig(value)
        off = ctypes.c_uint64(0)
        _check(_lib.ls_append(self._s, self._k(key), a.nbytes,
                              a.ctypes.data_as(ctypes.c_void_p), ctypes.byref(off)),
               "ls_append")
        return off.value

    def accumulate(self, key, offset, value, alpha=None):
        """dst += alpha*src, with the reduction chosen from the dtype. Only
        float64 and float32 are supplied; anything else is the caller's own
        ls_reduce through the C API, because the store never learns what a
        block is (§4)."""
        a = _contig(value)
        if a.dtype == np.float64:
            op, ctx = _lib.ls_add_f64, (ctypes.c_double(alpha) if alpha is not None else None)
        elif a.dtype == np.float32:
            op, ctx = _lib.ls_add_f32, (ctypes.c_float(alpha) if alpha is not None else None)
        else:
            raise Error(LS_ERR_INVAL,
                        f"accumulate supplies a reduction for float64/float32, not {a.dtype}")
        _check(_lib.ls_accumulate(self._s, self._k(key), offset, a.nbytes,
                                  a.ctypes.data_as(ctypes.c_void_p),
                                  ctypes.cast(op, ctypes.c_void_p),
                                  ctypes.byref(ctx) if ctx is not None else None),
               "ls_accumulate")

    # ---- asynchronous --------------------------------------------------

    def awrite(self, key, value, offset=0) -> Request:
        a = _contig(value)
        req = ctypes.c_void_p()
        _check(_lib.ls_awrite(self._s, self._k(key), offset, a.nbytes,
                              a.ctypes.data_as(ctypes.c_void_p), ctypes.byref(req)),
               "ls_awrite")
        return Request(req, a)

    def aread(self, key, out, offset=0) -> Request:
        if not out.flags["C_CONTIGUOUS"]:
            raise Error(LS_ERR_INVAL, "out array must be C-contiguous")
        req = ctypes.c_void_p()
        _check(_lib.ls_aread(self._s, self._k(key), offset, out.nbytes,
                             out.ctypes.data_as(ctypes.c_void_p), ctypes.byref(req)),
               "ls_aread")
        return Request(req, out)

    # ---- mapping: where this layer earns the most (§4a) -----------------

    def map(self, key, dtype=np.float64, shape=None):
        """Return a NumPy array backed by the mapping -- no copy at all.

        Writes through the array go to the store. Valid only on a store opened
        with mode=MAPPED; errors under a mapping arrive as SIGBUS rather than as
        an exception, which §3 documents rather than hides."""
        addr = ctypes.c_void_p()
        length = ctypes.c_size_t(0)
        _check(_lib.ls_map(self._s, self._k(key), ctypes.byref(addr),
                           ctypes.byref(length)), "ls_map")
        itemsize = np.dtype(dtype).itemsize
        n = length.value // itemsize
        buf = (ctypes.c_char * length.value).from_address(addr.value)
        arr = np.frombuffer(buf, dtype=dtype, count=n)
        if shape is not None:
            arr = arr.reshape(shape)
        self._maps[key] = arr          # keep it alive while the store is open
        return arr

    def unmap(self, key):
        self._maps.pop(key, None)
        _check(_lib.ls_unmap(self._s, self._k(key)), "ls_unmap")

    # ---- table of contents ---------------------------------------------

    def exists(self, key) -> bool:
        f = ctypes.c_int(0)
        _check(_lib.ls_exists(self._s, self._k(key), ctypes.byref(f)), "ls_exists")
        return bool(f.value)

    def size(self, key) -> int:
        n = ctypes.c_uint64(0)
        _check(_lib.ls_size(self._s, self._k(key), ctypes.byref(n)), "ls_size")
        return n.value

    def reserve(self, key, nbytes):
        _check(_lib.ls_reserve(self._s, self._k(key), nbytes), "ls_reserve")

    def erase(self, key):
        _check(_lib.ls_erase(self._s, self._k(key)), "ls_erase")

    def keys(self):
        kk = _P(ctypes.c_char_p)()
        n = ctypes.c_size_t(0)
        _check(_lib.ls_keys(self._s, ctypes.byref(kk), ctypes.byref(n)), "ls_keys")
        try:
            return [kk[i].decode() for i in range(n.value)]
        finally:
            _lib.ls_keys_free(kk, n.value)

    def set_attr(self, key, blob: bytes):
        _check(_lib.ls_set_attr(self._s, self._k(key), blob, len(blob)), "ls_set_attr")

    def get_attr(self, key) -> bytes:
        n = ctypes.c_size_t(0)
        _check(_lib.ls_get_attr(self._s, self._k(key), None, ctypes.byref(n)),
               "ls_get_attr")
        if n.value == 0:
            return b""
        buf = ctypes.create_string_buffer(n.value)
        _check(_lib.ls_get_attr(self._s, self._k(key), buf, ctypes.byref(n)),
               "ls_get_attr")
        return buf.raw[:n.value]

    # ---- mapping protocol, as §4a writes it -----------------------------

    def __setitem__(self, key, value):
        self.write(key, value)

    def __getitem__(self, key):
        return self.read(key)

    def __delitem__(self, key):
        self.erase(key)

    def __contains__(self, key):
        return self.exists(key)

    def __iter__(self):
        return iter(self.keys())

    def __len__(self):
        return len(self.keys())


def open(name, **kw) -> Store:        # noqa: A001  -- §4a spells it libspill.open
    """libspill.open("scratch", memory_budget=8<<30) -> Store"""
    return Store(name, **kw)


def store_exists(name, *, dir=None, exact_name=False, parallel=LOCAL, rank=-1) -> bool:
    """Is there already a store of this name? Does not create one."""
    o = _Opts()
    _lib.ls_opts_init(ctypes.byref(o), _OPTS_VERSION)
    o.dir = dir.encode() if dir else None
    o.exact_name = 1 if exact_name else 0
    o.parallel = parallel
    o.rank = rank
    f = ctypes.c_int(0)
    _check(_lib.ls_store_exists(name.encode(), ctypes.byref(o), ctypes.byref(f)),
           "ls_store_exists")
    return bool(f.value)


def version() -> str:
    return _lib.ls_version_string().decode()
