"""Tests for SchemaCodec — binary lossless compression."""

import pytest
from pidgin import SchemaCodec


SAMPLES = [
    {"id": 1, "name": "Alice", "email": "alice@example.com", "role": "admin", "active": True},
    {"id": 2, "name": "Bob", "email": "bob@example.com", "role": "editor", "active": False},
    {"id": 3, "name": "Charlie", "email": "charlie@example.com", "role": "viewer", "active": True},
    {"id": 4, "name": "Diana", "email": "diana@example.com", "role": "admin", "active": True},
    {"id": 5, "name": "Eve", "email": "eve@example.com", "role": "editor", "active": False},
]


@pytest.fixture
def codec():
    return SchemaCodec.learn(SAMPLES)


def test_roundtrip_single(codec):
    original = {"id": 99, "name": "Zara", "email": "zara@test.com", "role": "admin", "active": True}
    compressed = codec.compress(original)
    restored = codec.decompress(compressed)
    assert restored == original


def test_roundtrip_array(codec):
    compressed = codec.compress(SAMPLES)
    restored = codec.decompress(compressed)
    assert restored == SAMPLES


def test_compression_ratio(codec):
    import json
    original_bytes = len(json.dumps(SAMPLES).encode())
    compressed_bytes = len(codec.compress(SAMPLES))
    ratio = compressed_bytes / original_bytes
    assert ratio < 0.7, f"Expected <70% ratio, got {ratio:.1%}"


def test_nullable_fields():
    """Fields with None values in samples are learned as nullable."""
    samples_with_nulls = [
        {"id": 1, "name": "Alice", "bio": "hello"},
        {"id": 2, "name": "Bob", "bio": None},
        {"id": 3, "name": "Charlie", "bio": "world"},
    ]
    c = SchemaCodec.learn(samples_with_nulls)
    original = {"id": 4, "name": "Test", "bio": None}
    compressed = c.compress(original)
    restored = c.decompress(compressed)
    assert restored["bio"] is None


def test_unknown_enum_value(codec):
    original = {"id": 11, "name": "X", "email": "x@x.com", "role": "superadmin", "active": True}
    compressed = codec.compress(original)
    restored = codec.decompress(compressed)
    assert restored["role"] == "superadmin"


def test_unicode(codec):
    original = {"id": 12, "name": "Кирилица", "email": "кир@тест.бг", "role": "admin", "active": True}
    compressed = codec.compress(original)
    restored = codec.decompress(compressed)
    assert restored["name"] == "Кирилица"
    assert restored["email"] == "кир@тест.бг"


def test_empty_string(codec):
    original = {"id": 13, "name": "", "email": "", "role": "admin", "active": True}
    compressed = codec.compress(original)
    restored = codec.decompress(compressed)
    assert restored["name"] == ""


def test_profile_serialization(codec):
    profile_json = codec.profile.to_json()
    from pidgin.schema_codec import SchemaProfile
    restored_profile = SchemaProfile.from_json(profile_json)
    codec2 = SchemaCodec(restored_profile)

    original = {"id": 1, "name": "Test", "email": "t@t.com", "role": "admin", "active": True}
    compressed = codec2.compress(original)
    assert codec2.decompress(compressed) == original
