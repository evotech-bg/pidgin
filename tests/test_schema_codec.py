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


# === Profile Fingerprinting (version negotiation) ===

def test_fingerprint_is_deterministic(codec):
    """Same profile → same fingerprint."""
    fp1 = codec.fingerprint
    fp2 = codec.fingerprint
    assert fp1 == fp2
    assert len(fp1) == 16
    assert all(c in "0123456789abcdef" for c in fp1)


def test_fingerprint_survives_json_roundtrip(codec):
    """A profile reconstructed from canonical JSON must yield the same fingerprint."""
    from pidgin.schema_codec import SchemaProfile
    json_str = codec.profile.to_json()
    restored = SchemaProfile.from_json(json_str)
    assert SchemaCodec(restored).fingerprint == codec.fingerprint


def test_fingerprint_changes_on_evolution(codec):
    """Adding fields must change the fingerprint."""
    new_samples = [{"id": 99, "name": "X", "email": "x@x.com", "role": "admin",
                    "active": True, "added_field": "new"}]
    evolved = codec.profile.evolve(new_samples)
    assert SchemaCodec(evolved).fingerprint != codec.fingerprint


def test_fingerprint_negotiation_match(codec):
    """matches_fingerprint accepts the codec's own fingerprint."""
    assert codec.matches_fingerprint(codec.fingerprint)
    assert codec.matches_fingerprint(codec.fingerprint.upper())  # case insensitive


def test_fingerprint_negotiation_mismatch(codec):
    """matches_fingerprint rejects a different fingerprint."""
    assert not codec.matches_fingerprint("0000000000000000")
    assert not codec.matches_fingerprint("")
    assert not codec.matches_fingerprint("not-a-hash")


def test_fingerprint_negotiation_truncated_prefix(codec):
    """Clients may advertise a shorter prefix; server still recognizes a match."""
    assert codec.matches_fingerprint(codec.fingerprint[:8])
    assert codec.matches_fingerprint(codec.fingerprint[:4])
    assert not codec.matches_fingerprint("ffffffff")


# === Decoder Hardening (adversarial input handling) ===

def test_decode_empty_bytes_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="too short"):
        codec.decompress(b"")


def test_decode_single_byte_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="too short"):
        codec.decompress(b"\x00")


def test_decode_non_bytes_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="must be bytes"):
        codec.decompress("not bytes")  # type: ignore[arg-type]


def test_decode_truncated_payload_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    good = codec.compress({"id": 1, "name": "X", "email": "x@x.com", "role": "admin", "active": True})
    with pytest.raises(PidginDecodeError):
        codec.decompress(good[:5])


def test_decode_invalid_brotli_flag_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="brotli flag"):
        codec.decompress(b"\x99\x01\x01")


def test_decode_invalid_record_marker_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="record marker"):
        codec.decompress(b"\x00\x99")


def test_decode_corrupt_brotli_raises(codec):
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError, match="brotli"):
        codec.decompress(b"\x01\xff\xff\xff\xff")


def test_decode_length_overflow_attack_raises(codec):
    """Length prefix claims a 64KB string in a 10-byte buffer — must not over-read."""
    from pidgin.schema_codec import PidginDecodeError
    with pytest.raises(PidginDecodeError):
        codec.decompress(b"\x00\x01\x01\xff\xff" + b"x" * 5)


# === Cross-Implementation Fingerprint Interop ===
# These tests verify that the Python codec and the libpidgin C library
# produce identical fingerprints for the same logical schema. Without this
# guarantee, a Python client cannot negotiate version skew with a Kong
# (libpidgin-backed) server, which is the production deployment path.

def _try_load_libpidgin():
    import ctypes, os
    candidates = [
        os.path.join(os.path.dirname(__file__), "..", "libpidgin", "libpidgin.dylib"),
        os.path.join(os.path.dirname(__file__), "..", "libpidgin", "libpidgin.so"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return ctypes.CDLL(path)
    return None


def test_fingerprint_matches_libpidgin():
    """Python SchemaCodec.fingerprint must equal libpidgin's pidgin_engine_etag."""
    import ctypes, json
    lib = _try_load_libpidgin()
    if lib is None:
        pytest.skip("libpidgin not built; skipping cross-implementation interop test")

    lib.pidgin_engine_create.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pidgin_engine_create.restype = ctypes.c_void_p
    lib.pidgin_engine_observe.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.pidgin_engine_observe.restype = None
    lib.pidgin_engine_etag.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.pidgin_engine_etag.restype = ctypes.c_char_p
    lib.pidgin_engine_destroy.argtypes = [ctypes.c_void_p]
    lib.pidgin_engine_destroy.restype = None

    samples = [
        {"id": i, "name": f"User{i}", "email": f"u{i}@test.com",
         "role": "admin" if i % 3 == 0 else "viewer", "active": i % 2 == 0}
        for i in range(35)
    ]

    endpoint = b"/api/users"
    engine = lib.pidgin_engine_create(30, 500)
    try:
        for record in samples:
            lib.pidgin_engine_observe(engine, endpoint, json.dumps(record).encode())

        c_etag = lib.pidgin_engine_etag(engine, endpoint)
        assert c_etag is not None, "C library failed to produce ETag"
        c_fingerprint = c_etag.decode().strip('"')
    finally:
        lib.pidgin_engine_destroy(engine)

    py_codec = SchemaCodec.learn(samples)
    assert py_codec.fingerprint == c_fingerprint, (
        f"Fingerprint mismatch:\n"
        f"  Python: {py_codec.fingerprint}\n"
        f"  C:      {c_fingerprint}\n"
        f"This breaks Python-client / Kong-server negotiation."
    )
