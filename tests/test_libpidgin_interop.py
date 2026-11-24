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


# --- numbers written as text by libpidgin (fixed 24.11.2026) -------------
#
# libpidgin writes some values as JSON text inside the payload (fields it
# does not know, nested objects without a schema, arrays of plain values)
# and pidgin_decompress returns JSON text. Up to 0.2.0 every number above
# 1e9 or below 1e-6 was printed with "%e", 7 significant digits, so the
# client silently got another number.

import random
import struct

FIXTURES = Path(__file__).resolve().parent.parent / "benchmarks" / "real_api_fixtures"


def _learned(lib, records, n=None):
    eng = lib.pidgin_engine_create(n or len(records), 500)
    for r in records:
        lib.pidgin_engine_observe(eng, b"/api/x", json.dumps(r).encode())
    ptr = lib.pidgin_engine_profile(eng, b"/api/x")
    assert ptr, "engine did not build a profile"
    codec = SchemaCodec(SchemaProfile.from_json(ctypes.string_at(ptr).decode()))
    lib.pidgin_free(ptr)
    return eng, codec


def _c_decompress(lib, eng, payload):
    lib.pidgin_engine_decompress.restype = ctypes.c_void_p
    lib.pidgin_engine_decompress.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                             ctypes.c_char_p, ctypes.c_size_t]
    ptr = lib.pidgin_engine_decompress(eng, b"/api/x", payload, len(payload))
    assert ptr, "C decompress failed"
    out = json.loads(ctypes.string_at(ptr))
    lib.pidgin_free(ptr)
    return out


def _bits(x):
    return struct.pack("<d", float(x))


def _assert_same_numbers(a, b, path=""):
    if isinstance(a, dict):
        assert isinstance(b, dict) and a.keys() == b.keys(), path
        for k in a:
            _assert_same_numbers(a[k], b[k], f"{path}/{k}")
    elif isinstance(a, list):
        assert isinstance(b, list) and len(a) == len(b), path
        for i, (x, y) in enumerate(zip(a, b)):
            _assert_same_numbers(x, y, f"{path}[{i}]")
    elif isinstance(a, (int, float)) and not isinstance(a, bool):
        # value, not text: 20026453.0 may come back as 20026453
        assert isinstance(b, (int, float)) and not isinstance(b, bool), path
        assert _bits(a) == _bits(b) and a == b, f"{path}: sent {a!r}, got {b!r}"
    else:
        assert a == b, path


SIMPLE = [{"id": i, "v": 1.5 + i} for i in range(30)]


@pytest.mark.parametrize("msg", [
    {"id": 1, "v": 78185373126.77031},
    {"id": 2, "v": 2.5, "meta": {"big": 12345678901.123, "tiny": 1.23456789e-07}},
    {"id": 3, "v": 3.5, "meta": {"snowflake_like": 1234567890123456}},   # above 1e15, below 2^53
    {"id": 4, "v": 4.9e-324, "meta": {"tiny": 5e-300, "neg": -98765432109.5}},
    {"id": 5, "v": 1.7976931348623157e308, "meta": {"list": [1e20, 3.0e-9, -0.0]}},
])
def test_c_writes_numbers_exactly(lib, msg):
    eng, codec = _learned(lib, SIMPLE)
    try:
        payload = _compress(lib, eng, msg)
        assert payload, "C refused a message that fits"
        _assert_same_numbers(msg, codec.decompress(payload))   # Python / JS client path
        _assert_same_numbers(msg, _c_decompress(lib, eng, payload))
    finally:
        lib.pidgin_engine_destroy(eng)


@pytest.mark.parametrize("msg", [
    {"id": 9007199254740993, "v": 1.5},                   # 2^53 + 1: no double holds it
    {"id": 1, "v": 1.5, "meta": {"snowflake": 1234567890123456789}},
    {"id": 1, "v": 1.5, "meta": {"huge": 123456789012345678901234567890}},
])
def test_c_refuses_numbers_no_double_holds(lib, msg):
    eng, _ = _learned(lib, SIMPLE)
    try:
        assert _compress(lib, eng, msg) == b"", "must send JSON instead of a rounded number"
    finally:
        lib.pidgin_engine_destroy(eng)


def test_c_coingecko_supply_values_exact(lib):
    """The nine values the 24.11 benchmark caught (circulating/total supply)."""
    coins = json.loads((FIXTURES / "CoinGecko_Top_Coins.json").read_text())
    eng, codec = _learned(lib, coins)
    try:
        caught = {("usd-coin", "circulating_supply"), ("usd-coin", "total_supply"),
                  ("dogecoin", "circulating_supply"), ("dogecoin", "total_supply"),
                  ("cardano", "circulating_supply"), ("the-open-network", "circulating_supply"),
                  ("the-open-network", "total_supply"), ("dai", "circulating_supply"),
                  ("dai", "total_supply")}
        seen = set()
        for coin in coins:
            payload = _compress(lib, eng, coin)
            if not payload:
                continue  # refused: the server sends this one as JSON, which is exact
            _assert_same_numbers(coin, codec.decompress(payload))
            _assert_same_numbers(coin, _c_decompress(lib, eng, payload))
            seen |= {(coin["id"], k) for k in ("circulating_supply", "total_supply")}
        assert caught <= seen, f"not exercised: {caught - seen}"
        whole = _compress(lib, eng, coins)
        assert whole, "C refused the whole response"
        _assert_same_numbers(coins, codec.decompress(whole))
    finally:
        lib.pidgin_engine_destroy(eng)


def test_c_random_doubles_bit_exact(lib):
    """Random doubles from the whole range, in a schema field and in JSON text."""
    rng = random.Random(20261124)
    vals = []
    while len(vals) < 3000:
        x = struct.unpack("<d", rng.getrandbits(64).to_bytes(8, "little"))[0]
        if x == x and abs(x) != float("inf"):
            vals.append(x)
    eng, codec = _learned(lib, SIMPLE)
    try:
        for i in range(0, len(vals), 3):
            msg = {"id": i, "v": vals[i], "meta": {"a": vals[i + 1], "b": [vals[i + 2]]}}
            payload = _compress(lib, eng, msg)
            assert payload, f"refused {msg}"
            _assert_same_numbers(msg, codec.decompress(payload))
            _assert_same_numbers(msg, _c_decompress(lib, eng, payload))
    finally:
        lib.pidgin_engine_destroy(eng)
