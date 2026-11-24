"""compress() must never produce a payload that decodes to different data.

Each case used to round-trip silently to wrong values (a missing required
field came back as a default, a 70-bit integer was truncated, a list in a
string field came back as its string form). Now compress raises
PidginEncodeError, or another error, and the middleware sends plain JSON.
"""

import pytest

from pidgin import SchemaCodec
from pidgin.contrib._engine import PidginEngine

BASE = [
    {"id": i, "name": f"U{i}", "role": "a" if i % 2 else "b", "score": i * 1.5,
     "ok": i % 2 == 0, "addr": {"city": "S", "zip": 1000 + i},
     "lines": [{"sku": "x", "q": 1}]}
    for i in range(40)
]

MUTATIONS = {
    "missing required field": lambda r: {k: v for k, v in r.items() if k != "role"},
    "missing nested field": lambda r: dict(r, addr={"zip": 1}),
    "missing field in array item": lambda r: dict(r, lines=[{"sku": "y"}]),
    "nested object is None": lambda r: dict(r, addr=None),
    "integer wider than 64 bits": lambda r: dict(r, id=2**70),
    "list in a string field": lambda r: dict(r, name=[1, 2]),
}


@pytest.fixture(scope="module")
def codec():
    return SchemaCodec.learn(BASE)


@pytest.mark.parametrize("name", list(MUTATIONS))
@pytest.mark.parametrize("shape", ["list", "single"])
def test_never_silently_wrong(codec, name, shape):
    f = MUTATIONS[name]
    data = [f(r) for r in BASE[:5]] if shape == "list" else f(BASE[0])
    try:
        payload = codec.compress(data)
    except Exception:
        return  # refused: the caller sends JSON
    assert codec.decompress(payload) == data


def test_engine_falls_back_to_json():
    engine = PidginEngine(learn_samples=30)
    for r in BASE:
        engine.observe("/api/x", r)
    assert engine.is_ready("/api/x")
    bad = [MUTATIONS["missing required field"](r) for r in BASE[:5]]
    assert engine.compress("/api/x", bad) is None
    assert engine.compress("/api/x", BASE[:5]) is not None


def test_engine_record_with_items_field_is_not_mangled():
    """A single object with a list field named "items" is taken for a
    pagination wrapper, so the engine learns the inner items. Encoding the
    outer records with that profile must fall back to JSON, not return
    mixed-up records."""
    records = [dict(r, items=[{"sku": "x", "q": 1}]) for r in BASE]
    engine = PidginEngine(learn_samples=30)
    for r in records:
        engine.observe("/api/y", r)
    payload = engine.compress("/api/y", records[:5])
    if payload is not None:
        assert engine._endpoints["/api/y"].codec.decompress(payload) == records[:5]
