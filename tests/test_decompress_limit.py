"""Decoding untrusted payloads: output size and record count are capped.

A small payload must not be able to make the decoder allocate without
bound. With the caps, oversized payloads fail with PidginDecodeError and
the process does not grow; ordinary payloads decode as before.
"""

import json
import struct
import tracemalloc

import pytest

brotli = pytest.importorskip("brotli")

from pidgin import PidginDecodeError, SchemaCodec  # noqa: E402

BOMB_OUT = 200 << 20   # 200 MiB once decompressed


@pytest.fixture(scope="module")
def bomb():
    # a few hundred bytes on the wire
    return b"\x01" + brotli.compress(b"\x01" + b"\x00" * BOMB_OUT, quality=5)


def _codec():
    return SchemaCodec.learn([{"id": i, "name": f"n{i}", "tags": [{"k": "a"}]} for i in range(40)])


def _peak(fn):
    tracemalloc.start()
    try:
        try:
            fn()
            raised = None
        except Exception as e:  # noqa: BLE001
            raised = e
        return tracemalloc.get_traced_memory()[1], raised
    finally:
        tracemalloc.stop()


def test_brotli_bomb_refused_without_growing(bomb):
    c = _codec()
    assert len(bomb) < 64 * 1024
    peak, err = _peak(lambda: c.decompress(bomb))
    assert isinstance(err, PidginDecodeError)
    assert peak < 48 << 20, f"decoder grew to {peak >> 20} MiB"


def test_cap_can_be_raised_and_lowered():
    c = _codec()
    rec = [{"id": i, "name": "x" * 200, "tags": []} for i in range(200)]
    p = c.compress(rec)
    assert c.decompress(p) == rec
    small = SchemaCodec(c.profile, max_output=1024)
    with pytest.raises(PidginDecodeError):
        small.decompress(p)
    with pytest.raises(PidginDecodeError):
        c.decompress(p, max_output=1024)
    assert c.decompress(p, max_output=1 << 20) == rec


def _nested_bomb(top, inner):
    # records {"a": [{"k": "x"}, ...]}: k is a one-value enum, each inner object is 3 bytes
    c = SchemaCodec.learn([{"a": [{"k": "x"}]} for _ in range(40)])
    assert c.profile.field_order == ["a"] and not c.profile.fields["a"].nullable
    rec = struct.pack(">H", inner) + b"\x00\x00\x00" * inner + b"\x00\x00"
    raw = b"\x02" + struct.pack(">H", top) + rec * top
    return c.profile, b"\x01" + brotli.compress(raw, quality=5)


def test_record_count_capped():
    prof, payload = _nested_bomb(2000, 2000)   # 4 million records from a small payload
    assert len(payload) < 64 * 1024
    c = SchemaCodec(prof)
    peak, err = _peak(lambda: c.decompress(payload))
    assert isinstance(err, PidginDecodeError), err
    assert peak < 160 << 20, f"decoder grew to {peak >> 20} MiB"


def test_deep_json_fallback_is_a_decode_error():
    c = _codec()
    deep = b"[" * 60000
    # single object: id (int varint), name (str), tags (array count 0), then fallback JSON
    body = bytearray(b"\x01")
    for key in c.profile.field_order:
        f = c.profile.fields[key]
        if f.nullable:
            body.append(0x00)
            continue
        body += {"int": b"\x00", "str": b"\x00\x00", "array": b"\x00\x00"}.get(f.dtype, b"\x00\x00")
    body += struct.pack(">H", len(deep)) + deep
    with pytest.raises(PidginDecodeError):
        c.decompress(b"\x00" + bytes(body))


def test_ordinary_payloads_unchanged():
    c = _codec()
    for data in ([{"id": 1, "name": "a", "tags": [{"k": "a"}]}],
                 {"id": 2, "name": "b" * 5000, "tags": []},
                 [{"id": i, "name": str(i), "tags": [{"k": "a"}] * 3} for i in range(1000)]):
        assert c.decompress(c.compress(data)) == data


def _single(c, tail):
    """Single record with every known field empty, then ``tail`` as the fallback section."""
    body = bytearray(b"\x01")
    for key in c.profile.field_order:
        f = c.profile.fields[key]
        if f.nullable:
            body.append(0x00)
            continue
        body += {"int": b"\x00", "str": b"\x00\x00", "array": b"\x00\x00"}.get(f.dtype, b"\x00\x00")
    return bytes(body) + tail


def test_json_fallback_counts_against_items():
    c = _codec()
    blob = b'{"x":[' + b",".join([b"[]"] * 20000) + b"]}"
    p = b"\x00" + _single(c, struct.pack(">H", len(blob)) + blob)
    with pytest.raises(PidginDecodeError):
        c.decompress(p, max_items=1000)
    assert len(c.decompress(p)["x"]) == 20000   # within the default limit it decodes
    bad = b"[1,2]"   # the fallback section must be an object
    with pytest.raises(PidginDecodeError):
        c.decompress(b"\x00" + _single(c, struct.pack(">H", len(bad)) + bad))


def test_long_varint_refused():
    c = _codec()
    key = c.profile.field_order[0]
    assert c.profile.fields[key].dtype == "int" and not c.profile.fields[key].nullable
    payload = b"\x00\x01" + b"\x80" * 100000 + b"\x00"
    with pytest.raises(PidginDecodeError):
        c.decompress(payload)
