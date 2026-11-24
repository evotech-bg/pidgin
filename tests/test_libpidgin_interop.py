"""libpidgin (the C engine inside the server modules) against Python.

C encodes, Python decodes to the same data; and data that does not fit the
profile makes the C encoder return an empty buffer (the module then sends
JSON) instead of crashing or encoding wrong values.

Skipped unless libpidgin is built: make -C libpidgin
"""

import ctypes
import json
from pathlib import Path

import pytest

from pidgin.schema_codec import SchemaCodec, SchemaProfile

LIBDIR = Path(__file__).resolve().parent.parent / "libpidgin"


class _Buf(ctypes.Structure):
    _fields_ = [("data", ctypes.POINTER(ctypes.c_uint8)), ("len", ctypes.c_size_t)]


@pytest.fixture(scope="module")
def lib():
    for name in ("libpidgin.dylib", "libpidgin.so"):
        if (LIBDIR / name).exists():
            lib = ctypes.CDLL(str(LIBDIR / name))
            break
    else:
        pytest.skip("libpidgin not built")
    lib.pidgin_engine_create.restype = ctypes.c_void_p
    lib.pidgin_engine_create.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pidgin_engine_observe.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.pidgin_engine_compress.restype = _Buf
    lib.pidgin_engine_compress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.pidgin_engine_profile.restype = ctypes.c_void_p
    lib.pidgin_engine_profile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.pidgin_engine_destroy.argtypes = [ctypes.c_void_p]
    lib.pidgin_buf_free.argtypes = [ctypes.POINTER(_Buf)]
    lib.pidgin_free.argtypes = [ctypes.c_void_p]
    return lib


RECORDS = [
    {"id": i, "name": f"U{i}", "tags": ["a", "b"][: i % 3], "score": i * 0.5,
     "ok": i % 2 == 0, "ts": "2026-01-01T00:00:00Z",
     "addr": {"city": "S", "zip": 1000 + i},
     "lines": [{"sku": "x", "q": j} for j in range(i % 3)],
     "note": None if i % 3 else "n"}
    for i in range(40)
]


def _engine(lib, records):
    eng = lib.pidgin_engine_create(30, 500)
    for r in records[:35]:
        lib.pidgin_engine_observe(eng, b"/api/x", json.dumps(r).encode())
    return eng


def _compress(lib, eng, data):
    buf = lib.pidgin_engine_compress(eng, b"/api/x", json.dumps(data).encode())
    out = ctypes.string_at(buf.data, buf.len) if buf.data else b""
    if buf.data:
        lib.pidgin_buf_free(ctypes.byref(buf))
    return out


def test_c_encodes_python_decodes(lib):
    eng = _engine(lib, RECORDS)
    try:
        payload = _compress(lib, eng, RECORDS)
        assert payload, "C engine refused data that fits its profile"
        ptr = lib.pidgin_engine_profile(eng, b"/api/x")
        profile = ctypes.string_at(ptr).decode()
        lib.pidgin_free(ptr)
        assert SchemaCodec(SchemaProfile.from_json(profile)).decompress(payload) == RECORDS
    finally:
        lib.pidgin_engine_destroy(eng)


@pytest.mark.parametrize("mutate", [
    lambda r: {k: v for k, v in r.items() if k != "name"},   # used to crash
    lambda r: dict(r, id=2**70),
    lambda r: dict(r, name=[1, 2]),
    lambda r: dict(r, addr=None),
    lambda r: dict(r, lines=[1, 2]),
    lambda r: dict(r, tags=["x" * 70000]),          # JSON fallback longer than 16 bits
    lambda r: dict(r, extra="y" * 70000),           # unknown field longer than 16 bits
], ids=["missing-field", "huge-int", "list-in-string", "object-none", "plain-values-in-object-array",
        "long-fallback", "long-unknown-field"])
def test_c_refuses_data_that_does_not_fit(lib, mutate):
    eng = _engine(lib, RECORDS)
    try:
        assert _compress(lib, eng, [mutate(r) for r in RECORDS[:5]]) == b""
    finally:
        lib.pidgin_engine_destroy(eng)
