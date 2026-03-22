"""Tests for schema evolution — backward + forward compatibility."""

import pytest
from pidgin import SchemaCodec
from pidgin.schema_codec import SchemaProfile


# === V1 schema: users with id, name, email, role ===
# Need enough samples for 'role' to be detected as enum (unique < 50% of total)
V1_SAMPLES = [
    {"id": 1, "name": "Alice", "email": "a@test.com", "role": "admin", "active": True},
    {"id": 2, "name": "Bob", "email": "b@test.com", "role": "editor", "active": False},
    {"id": 3, "name": "Charlie", "email": "c@test.com", "role": "viewer", "active": True},
    {"id": 4, "name": "Diana", "email": "d@test.com", "role": "admin", "active": True},
    {"id": 5, "name": "Eve", "email": "e@test.com", "role": "editor", "active": False},
    {"id": 6, "name": "Frank", "email": "f@test.com", "role": "admin", "active": True},
    {"id": 7, "name": "Grace", "email": "g@test.com", "role": "viewer", "active": False},
    {"id": 8, "name": "Hank", "email": "h@test.com", "role": "editor", "active": True},
]

# === V2 schema: added 'department' + 'locale', removed 'active', new enum value 'billing' ===
# Need 10+ samples so 'role' with 4 unique values is detected as enum (4/10 < 0.5)
V2_SAMPLES = [
    {"id": 10, "name": "Iris", "email": "i@test.com", "role": "admin", "department": "Engineering", "locale": "en"},
    {"id": 11, "name": "Jack", "email": "j@test.com", "role": "billing", "department": "Finance", "locale": "bg"},
    {"id": 12, "name": "Kate", "email": "k@test.com", "role": "viewer", "department": None, "locale": "de"},
    {"id": 13, "name": "Leo", "email": "l@test.com", "role": "editor", "department": "Marketing", "locale": "en"},
    {"id": 14, "name": "Mia", "email": "m@test.com", "role": "admin", "department": "Engineering", "locale": "fr"},
    {"id": 15, "name": "Nate", "email": "n@test.com", "role": "billing", "department": "Sales", "locale": "bg"},
    {"id": 16, "name": "Olga", "email": "o@test.com", "role": "viewer", "department": None, "locale": "en"},
    {"id": 17, "name": "Pete", "email": "p@test.com", "role": "editor", "department": "HR", "locale": "de"},
    {"id": 18, "name": "Quinn", "email": "q@test.com", "role": "admin", "department": "Engineering", "locale": "en"},
    {"id": 19, "name": "Rose", "email": "r@test.com", "role": "billing", "department": "Finance", "locale": "bg"},
]


@pytest.fixture
def v1_profile():
    return SchemaCodec.learn(V1_SAMPLES).profile


@pytest.fixture
def v2_profile(v1_profile):
    return v1_profile.evolve(V2_SAMPLES)


# === Profile evolution ===

def test_evolve_bumps_version(v1_profile, v2_profile):
    assert v1_profile.version == 1
    assert v2_profile.version == 2


def test_evolve_preserves_field_order(v1_profile, v2_profile):
    """Old fields stay in the same position — critical for binary compat."""
    for i, key in enumerate(v1_profile.field_order):
        assert v2_profile.field_order[i] == key


def test_evolve_adds_new_fields_at_end(v1_profile, v2_profile):
    old_set = set(v1_profile.field_order)
    new_fields = [k for k in v2_profile.field_order if k not in old_set]
    assert "department" in new_fields
    assert "locale" in new_fields


def test_evolve_new_fields_are_nullable(v2_profile):
    """New fields must be nullable — old encoders won't send them."""
    assert v2_profile.fields["department"].nullable is True
    assert v2_profile.fields["locale"].nullable is True


def test_evolve_removed_fields_become_nullable(v2_profile):
    """Removed fields stay in schema but become nullable with freq=0."""
    assert "active" in v2_profile.fields
    assert v2_profile.fields["active"].nullable is True
    assert v2_profile.fields["active"].frequency == 0.0


def test_evolve_extends_enum_values(v1_profile, v2_profile):
    """New enum values appended — old indices stay valid."""
    old_enums = v1_profile.fields["role"].enum_values
    new_enums = v2_profile.fields["role"].enum_values
    # Old values preserve their indices
    for i, v in enumerate(old_enums):
        assert new_enums[i] == v
    # New value 'billing' is appended
    assert "billing" in new_enums


def test_diff(v1_profile, v2_profile):
    diff = v1_profile.diff(v2_profile)
    diff_text = "\n".join(diff)
    assert "v1 → v2" in diff_text
    assert "department" in diff_text
    assert "locale" in diff_text


# === Backward compatibility: v2 profile handles old-shape data ===

def test_backward_compat():
    """V2 codec can encode/decode v1-shaped data (missing new fields)."""
    v1_codec = SchemaCodec.learn(V1_SAMPLES)
    v2_profile = v1_codec.profile.evolve(V2_SAMPLES)
    v2_codec = SchemaCodec(v2_profile)

    # Old-shape data (no department/locale, has active)
    old_data = {"id": 99, "name": "Test", "email": "t@t.com", "role": "admin", "active": True}
    compressed = v2_codec.compress(old_data)
    restored = v2_codec.decompress(compressed)

    assert restored["id"] == 99
    assert restored["name"] == "Test"
    assert restored["role"] == "admin"
    assert restored["active"] is True


# === Forward compatibility: v2 profile handles new-shape data ===

def test_forward_compat():
    """V2 codec handles new data shapes with new fields."""
    v1_codec = SchemaCodec.learn(V1_SAMPLES)
    v2_profile = v1_codec.profile.evolve(V2_SAMPLES)
    v2_codec = SchemaCodec(v2_profile)

    # New-shape data (has department/locale, no active)
    new_data = {"id": 50, "name": "New User", "email": "n@t.com", "role": "admin",
                "department": "Engineering", "locale": "bg"}
    compressed = v2_codec.compress(new_data)
    restored = v2_codec.decompress(compressed)

    assert restored["id"] == 50
    assert restored["name"] == "New User"
    assert restored["role"] == "admin"
    assert restored["department"] == "Engineering"
    assert restored["locale"] == "bg"


# === Roundtrip with evolved profile ===

def test_v2_roundtrip():
    """V2 data encoded/decoded with v2 profile is lossless."""
    v1_codec = SchemaCodec.learn(V1_SAMPLES)
    v2_profile = v1_codec.profile.evolve(V2_SAMPLES)
    v2_codec = SchemaCodec(v2_profile)

    for sample in V2_SAMPLES:
        compressed = v2_codec.compress(sample)
        restored = v2_codec.decompress(compressed)
        # Removed field 'active' won't appear since it's not in sample
        assert restored == sample


# === Multi-generation evolution ===

def test_triple_evolution():
    """v1 → v2 → v3 works correctly."""
    v1 = SchemaCodec.learn(V1_SAMPLES).profile

    v2 = v1.evolve(V2_SAMPLES)
    assert v2.version == 2

    v3_samples = [
        {"id": 20, "name": "Kate", "email": "k@t.com", "role": "billing",
         "department": "Legal", "locale": "es", "avatar_url": "https://cdn.test/20.jpg"},
    ]
    v3 = v2.evolve(v3_samples)
    assert v3.version == 3
    assert "avatar_url" in v3.fields
    assert v3.fields["avatar_url"].nullable is True

    # All old fields still at same positions
    for i, key in enumerate(v1.field_order):
        assert v3.field_order[i] == key


# === Profile serialization with evolution ===

def test_evolved_profile_serialization(v2_profile):
    """Evolved profile survives JSON roundtrip."""
    json_str = v2_profile.to_json()
    restored = SchemaProfile.from_json(json_str)

    assert restored.version == v2_profile.version
    assert restored.field_order == v2_profile.field_order
    assert set(restored.fields.keys()) == set(v2_profile.fields.keys())
    assert restored.fields["role"].enum_values == v2_profile.fields["role"].enum_values
