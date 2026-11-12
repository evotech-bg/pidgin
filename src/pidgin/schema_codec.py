"""
Schema-Aware API Compression.

Learn the schema from sample API responses, then compress future responses
by stripping keys, encoding values optimally, and applying brotli on top.

Two-phase workflow:
  Phase 1 (once): profile = SchemaCodec.learn(sample_responses)
  Phase 2 (runtime): compressed = codec.compress(response)
                     original = codec.decompress(compressed)

The profile is shared between encoder (server) and decoder (client).
Both sides know the schema, so keys are never transmitted.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import struct
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

try:
    import brotli
    HAS_BROTLI = True
except ImportError:
    HAS_BROTLI = False

try:
    from pidgin._fast import encode_records as _c_encode_records
    from pidgin._fast import decode_records as _c_decode_records
except ImportError:
    _c_encode_records = None
    _c_decode_records = None


# === Exceptions ===

class PidginDecodeError(ValueError):
    """Raised when a Pidgin-encoded payload cannot be decoded.

    Wraps the underlying cause (truncation, malformed length prefix, brotli
    error, JSON parse error in fallback section, etc.) so callers can handle
    all decode failures with a single except clause without leaking
    implementation-specific exception types.
    """


# === Schema Types ===

@dataclass
class FieldSchema:
    """Learned schema for a single JSON field."""
    name: str
    dtype: str  # "int", "float", "str", "bool", "enum", "timestamp", "null", "object", "array"
    nullable: bool = False
    enum_values: list[str] | None = None  # for dtype="enum"
    nested: dict[str, "FieldSchema"] | None = None  # for dtype="object"
    items_schema: dict[str, "FieldSchema"] | None = None  # for dtype="array" of objects
    frequency: float = 1.0  # how often this field appears (0.0-1.0)


@dataclass
class SchemaProfile:
    """Learned API response profile — shared between encoder and decoder."""
    fields: dict[str, FieldSchema]
    field_order: list[str]  # deterministic field ordering
    version: int = 1

    def to_dict(self) -> dict:
        """Serialize for sharing with client."""
        def field_to_dict(f: FieldSchema) -> dict:
            d = {"dtype": f.dtype, "nullable": f.nullable, "freq": f.frequency}
            if f.enum_values:
                d["enum"] = f.enum_values
            if f.nested:
                d["nested"] = {k: field_to_dict(v) for k, v in f.nested.items()}
            if f.items_schema:
                d["items"] = {k: field_to_dict(v) for k, v in f.items_schema.items()}
            return d

        return {
            "version": self.version,
            "field_order": self.field_order,
            "fields": {k: field_to_dict(v) for k, v in self.fields.items()},
        }

    @classmethod
    def from_dict(cls, d: dict) -> "SchemaProfile":
        """Deserialize from shared profile."""
        def dict_to_field(name: str, fd: dict) -> FieldSchema:
            nested = None
            items = None
            if "nested" in fd:
                nested = {k: dict_to_field(k, v) for k, v in fd["nested"].items()}
            if "items" in fd:
                items = {k: dict_to_field(k, v) for k, v in fd["items"].items()}
            return FieldSchema(
                name=name,
                dtype=fd["dtype"],
                nullable=fd.get("nullable", False),
                enum_values=fd.get("enum"),
                nested=nested,
                items_schema=items,
                frequency=fd.get("freq", 1.0),
            )

        fields = {k: dict_to_field(k, v) for k, v in d["fields"].items()}
        return cls(fields=fields, field_order=d["field_order"], version=d.get("version", 1))

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"))

    def to_canonical_json(self) -> bytes:
        """Sorted-keys JSON serialization (legacy, retained for compatibility)."""
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True).encode("utf-8")

    def to_canonical_bytes(self) -> bytes:
        """Pidgin Fingerprint Format v1 — must match libpidgin's encoding.

        Format (all integers little-endian, all strings length-prefixed UTF-8):
          profile := [version:u32][field_count:u32][field*]
          field   := [name_len:u16][name][dtype:u8][nullable:u8]
                     [enum_count:u16][enum*]
                     [has_nested:u8] [field_count:u32 if has_nested][field* if has_nested]
                     [has_items:u8]  [field_count:u32 if has_items] [field* if has_items]
          enum    := [val_len:u16][val]

        dtype encoding: 0=bool 1=int 2=float 3=str 4=enum 5=timestamp 6=object 7=array 8=null
        """
        DTYPE = {"bool": 0, "int": 1, "float": 2, "str": 3, "enum": 4,
                 "timestamp": 5, "object": 6, "array": 7, "null": 8}

        def emit_str(out: bytearray, s: str) -> None:
            b = s.encode("utf-8")
            if len(b) > 0xFFFF:
                raise ValueError("string too long for canonical encoding")
            out += struct.pack("<H", len(b))
            out += b

        def emit_field(out: bytearray, f: "FieldSchema") -> None:
            emit_str(out, f.name)
            out += bytes([DTYPE.get(f.dtype, 3)])
            out += bytes([1 if f.nullable else 0])

            evals = f.enum_values or []
            if len(evals) > 0xFFFF:
                raise ValueError("too many enum values")
            out += struct.pack("<H", len(evals))
            for v in evals:
                emit_str(out, v)

            has_nested = bool(f.nested)
            out += bytes([1 if has_nested else 0])
            if has_nested:
                emit_fields(out, f.nested)

            has_items = bool(f.items_schema)
            out += bytes([1 if has_items else 0])
            if has_items:
                emit_fields(out, f.items_schema)

        def emit_fields(out: bytearray, fields: dict) -> None:
            order = list(fields.keys())  # nested fields preserve insertion order
            out += struct.pack("<I", len(order))
            for name in order:
                emit_field(out, fields[name])

        out = bytearray()
        out += struct.pack("<I", self.version)
        out += struct.pack("<I", len(self.field_order))
        for name in self.field_order:
            emit_field(out, self.fields[name])
        return bytes(out)

    @property
    def fingerprint(self) -> str:
        """16-hex-digit (64-bit FNV-1a) deterministic fingerprint of this profile.

        Used for client-server profile-version negotiation. The hash function
        and canonical-bytes encoding match libpidgin's compute_etag, so a
        Python codec and a libpidgin-backed server (e.g., Kong via FFI)
        produce identical fingerprints for the same logical profile.

        FNV-1a is non-cryptographic; the fingerprint is for cache validation,
        not a security boundary (see Security Considerations).
        """
        h = 0xcbf29ce484222325
        mask = (1 << 64) - 1
        for byte in self.to_canonical_bytes():
            h ^= byte
            h = (h * 0x100000001b3) & mask
        return f"{h:016x}"

    @classmethod
    def from_json(cls, s: str) -> "SchemaProfile":
        return cls.from_dict(json.loads(s))

    def evolve(self, new_samples: list[dict]) -> "SchemaProfile":
        """Create a backward+forward compatible new schema version.

        - Preserves existing field_order (critical for binary compat)
        - New fields appended at end (nullable)
        - Removed fields become nullable with freq=0
        - Enum values extended (old indices preserved)
        - Type widening: int->float safe
        - Version bumped
        """
        new_profile = learn_schema(new_samples)

        evolved_fields: dict[str, FieldSchema] = {}
        evolved_order: list[str] = list(self.field_order)

        # Process existing fields — preserve order, merge changes
        for key in self.field_order:
            old_field = self.fields[key]
            if key in new_profile.fields:
                evolved_fields[key] = _merge_field(old_field, new_profile.fields[key])
            else:
                # Field removed in new data — keep but mark as nullable with freq=0
                evolved_fields[key] = FieldSchema(
                    name=old_field.name,
                    dtype=old_field.dtype,
                    nullable=True,
                    enum_values=old_field.enum_values,
                    nested=old_field.nested,
                    items_schema=old_field.items_schema,
                    frequency=0.0,
                )

        # Append new fields at end (not in old schema)
        for key in new_profile.field_order:
            if key not in evolved_fields:
                new_field = new_profile.fields[key]
                evolved_fields[key] = FieldSchema(
                    name=new_field.name,
                    dtype=new_field.dtype,
                    nullable=True,  # new fields are always nullable for backward compat
                    enum_values=new_field.enum_values,
                    nested=new_field.nested,
                    items_schema=new_field.items_schema,
                    frequency=new_field.frequency,
                )
                evolved_order.append(key)

        return SchemaProfile(
            fields=evolved_fields,
            field_order=evolved_order,
            version=self.version + 1,
        )

    def diff(self, other: "SchemaProfile") -> list[str]:
        """Human-readable change list between this and another schema."""
        changes: list[str] = [f"v{self.version} \u2192 v{other.version}"]

        all_keys = set(self.fields.keys()) | set(other.fields.keys())
        for key in sorted(all_keys):
            old = self.fields.get(key)
            new = other.fields.get(key)

            if old is None and new is not None:
                changes.append(f"+ added field '{key}' ({new.dtype}, nullable={new.nullable})")
            elif old is not None and new is None:
                changes.append(f"- removed field '{key}' ({old.dtype})")
            elif old is not None and new is not None:
                if old.dtype != new.dtype:
                    changes.append(f"~ field '{key}' type changed: {old.dtype} -> {new.dtype}")
                if old.nullable != new.nullable:
                    changes.append(f"~ field '{key}' nullable changed: {old.nullable} -> {new.nullable}")
                if old.frequency != new.frequency:
                    changes.append(f"~ field '{key}' frequency changed: {old.frequency:.2f} -> {new.frequency:.2f}")
                if old.enum_values != new.enum_values:
                    if old.enum_values and new.enum_values:
                        added = set(new.enum_values) - set(old.enum_values)
                        removed = set(old.enum_values) - set(new.enum_values)
                        if added:
                            changes.append(f"~ field '{key}' enum values added: {sorted(added)}")
                        if removed:
                            changes.append(f"~ field '{key}' enum values removed: {sorted(removed)}")
                    else:
                        changes.append(f"~ field '{key}' enum_values changed: {old.enum_values} -> {new.enum_values}")

        if self.field_order != other.field_order:
            new_in_order = [k for k in other.field_order if k not in self.field_order]
            if new_in_order:
                changes.append(f"~ field_order extended with: {new_in_order}")

        if self.version != other.version:
            changes.append(f"~ version: {self.version} -> {other.version}")

        return changes


# === Schema Learner ===

def _infer_dtype(values: list[Any]) -> tuple[str, list[str] | None]:
    """Infer the best dtype for a list of values."""
    non_null = [v for v in values if v is not None]
    if not non_null:
        return "null", None

    types = set(type(v).__name__ for v in non_null)

    if types == {"bool"}:
        return "bool", None

    if types <= {"int", "float"}:
        if types == {"int"}:
            return "int", None
        return "float", None

    if types == {"str"}:
        # Check if it's a timestamp
        sample = non_null[0]
        if len(sample) >= 19 and "T" in sample and (sample.endswith("Z") or "+" in sample[10:]):
            return "timestamp", None

        # Check if it's an enum (low cardinality)
        unique = set(non_null)
        if len(unique) <= 20 and len(unique) < len(non_null) * 0.5:
            return "enum", sorted(unique)

        return "str", None

    if types == {"dict"}:
        return "object", None

    if types == {"list"}:
        return "array", None

    return "str", None


def learn_schema(samples: list[dict], min_frequency: float = 0.1) -> SchemaProfile:
    """Learn schema from sample API responses.

    Args:
        samples: List of JSON response dicts (or dicts from response["data"], etc.)
        min_frequency: Minimum field occurrence ratio to include in schema.

    Returns:
        SchemaProfile ready for compression.
    """
    n = len(samples)
    if n == 0:
        return SchemaProfile(fields={}, field_order=[])

    # Collect all field values
    field_values: dict[str, list[Any]] = {}
    field_counts: Counter = Counter()

    for sample in samples:
        if not isinstance(sample, dict):
            continue
        for key, value in sample.items():
            field_counts[key] += 1
            field_values.setdefault(key, []).append(value)

    # Build schema
    fields = {}
    for key, values in field_values.items():
        freq = field_counts[key] / n
        if freq < min_frequency:
            continue

        dtype, enum_vals = _infer_dtype(values)
        nullable = any(v is None for v in values) or freq < 1.0

        nested = None
        items_schema = None

        if dtype == "object":
            # Recurse into nested objects
            nested_samples = [v for v in values if isinstance(v, dict)]
            if nested_samples:
                nested_profile = learn_schema(nested_samples)
                nested = nested_profile.fields

        elif dtype == "array":
            # Check if array of objects
            all_items = [item for v in values if isinstance(v, list) for item in v]
            obj_items = [item for item in all_items if isinstance(item, dict)]
            if obj_items and len(obj_items) > len(all_items) * 0.5:
                items_profile = learn_schema(obj_items)
                items_schema = items_profile.fields

        fields[key] = FieldSchema(
            name=key,
            dtype=dtype,
            nullable=nullable,
            enum_values=enum_vals,
            nested=nested,
            items_schema=items_schema,
            frequency=freq,
        )

    # Order by frequency (most common first) then alphabetically
    field_order = sorted(fields.keys(), key=lambda k: (-fields[k].frequency, k))

    return SchemaProfile(fields=fields, field_order=field_order)


def _merge_field(old: FieldSchema, new: FieldSchema) -> FieldSchema:
    """Merge an old field with a new observation, preserving backward compat.

    - Type widening: int->float is safe
    - Enum values: old indices preserved, new values appended
    - Nullable: once nullable, stays nullable
    - Nested/items: recursively merged
    """
    # Type widening
    dtype = old.dtype
    if old.dtype == "int" and new.dtype == "float":
        dtype = "float"
    elif old.dtype != new.dtype:
        # Incompatible type change — keep old type, mark nullable
        dtype = old.dtype

    # Nullable: once nullable, stays nullable
    nullable = old.nullable or new.nullable

    # Enum values: preserve old indices, append new
    enum_values = None
    if old.enum_values is not None or new.enum_values is not None:
        if old.enum_values and new.enum_values:
            enum_values = list(old.enum_values)
            for val in new.enum_values:
                if val not in enum_values:
                    enum_values.append(val)
        elif old.enum_values:
            enum_values = list(old.enum_values)
        else:
            enum_values = list(new.enum_values) if new.enum_values else None

    # Nested: recursive merge
    nested = None
    if old.nested or new.nested:
        nested = {}
        all_nested_keys = set()
        if old.nested:
            all_nested_keys |= set(old.nested.keys())
        if new.nested:
            all_nested_keys |= set(new.nested.keys())
        for k in all_nested_keys:
            old_sub = old.nested.get(k) if old.nested else None
            new_sub = new.nested.get(k) if new.nested else None
            if old_sub and new_sub:
                nested[k] = _merge_field(old_sub, new_sub)
            elif old_sub:
                nested[k] = FieldSchema(
                    name=old_sub.name, dtype=old_sub.dtype, nullable=True,
                    enum_values=old_sub.enum_values, nested=old_sub.nested,
                    items_schema=old_sub.items_schema, frequency=0.0,
                )
            elif new_sub:
                nested[k] = FieldSchema(
                    name=new_sub.name, dtype=new_sub.dtype, nullable=True,
                    enum_values=new_sub.enum_values, nested=new_sub.nested,
                    items_schema=new_sub.items_schema, frequency=new_sub.frequency,
                )

    # Items schema: recursive merge
    items_schema = None
    if old.items_schema or new.items_schema:
        items_schema = {}
        all_item_keys = set()
        if old.items_schema:
            all_item_keys |= set(old.items_schema.keys())
        if new.items_schema:
            all_item_keys |= set(new.items_schema.keys())
        for k in all_item_keys:
            old_sub = old.items_schema.get(k) if old.items_schema else None
            new_sub = new.items_schema.get(k) if new.items_schema else None
            if old_sub and new_sub:
                items_schema[k] = _merge_field(old_sub, new_sub)
            elif old_sub:
                items_schema[k] = FieldSchema(
                    name=old_sub.name, dtype=old_sub.dtype, nullable=True,
                    enum_values=old_sub.enum_values, nested=old_sub.nested,
                    items_schema=old_sub.items_schema, frequency=0.0,
                )
            elif new_sub:
                items_schema[k] = FieldSchema(
                    name=new_sub.name, dtype=new_sub.dtype, nullable=True,
                    enum_values=new_sub.enum_values, nested=new_sub.nested,
                    items_schema=new_sub.items_schema, frequency=new_sub.frequency,
                )

    # Frequency: use new observation
    frequency = new.frequency

    return FieldSchema(
        name=old.name,
        dtype=dtype,
        nullable=nullable,
        enum_values=enum_values,
        nested=nested,
        items_schema=items_schema,
        frequency=frequency,
    )


# === Encoder / Decoder ===

# Wire format markers
_MARKER_NULL = 0xFF
_MARKER_PRESENT = 0x01
_MARKER_ABSENT = 0x00


class SchemaCodec:
    """Schema-aware JSON compressor.

    Usage:
        # Learn (once)
        codec = SchemaCodec.learn(sample_responses)
        profile_json = codec.profile.to_json()  # share with client

        # Compress (per response)
        compressed = codec.compress(response_dict)
        original = codec.decompress(compressed)
    """

    _DTYPE_MAP = {
        "bool": 0,
        "int": 1,
        "float": 2,
        "str": 3,
        "enum": 4,
        "timestamp": 5,
        "object": 6,
        "array": 7,
    }

    def __init__(self, profile: SchemaProfile, use_brotli: bool = True):
        self.profile = profile
        self.use_brotli = use_brotli and HAS_BROTLI
        self._c_schema = self._compile_c_schema() if _c_encode_records else None

    @classmethod
    def learn(cls, samples: list[dict], use_brotli: bool = True) -> "SchemaCodec":
        """Learn schema from sample responses and create a codec."""
        profile = learn_schema(samples)
        return cls(profile, use_brotli)

    @property
    def fingerprint(self) -> str:
        """Profile fingerprint — see SchemaProfile.fingerprint."""
        return self.profile.fingerprint

    def matches_fingerprint(self, client_fingerprint: str) -> bool:
        """Check if a client's advertised profile fingerprint matches the active profile.

        Used for HTTP content negotiation. The standard flow is:
          1. Client requests with `Accept-Encoding: pidgin; profile=<hex>`
          2. Server calls codec.matches_fingerprint(<hex>)
          3. If True: encode with Pidgin
             If False: fall back to brotli/gzip and let the client re-fetch the profile

        The comparison is case-insensitive and tolerates a trailing length mismatch
        (clients may truncate further than the server's 16 hex chars).
        """
        if not client_fingerprint:
            return False
        active = self.fingerprint.lower()
        candidate = client_fingerprint.strip().lower()
        # Allow client to send a prefix (we use 16; client may send 8 for shorter cache keys)
        return active.startswith(candidate) or candidate.startswith(active)

    def _compile_c_schema(self) -> tuple | None:
        """Compile schema into a flat tuple structure for the C extension.

        Returns recursive tuple: (names, types, nullable, emaps, sub_schemas)
        where sub_schemas is a list of (sub_tuple | None) per field.
        """
        try:
            return self._compile_fields(self.profile.fields, self.profile.field_order)
        except Exception:
            return None

    @classmethod
    def _compile_fields(
        cls,
        fields: dict[str, FieldSchema],
        order: list[str],
    ) -> tuple:
        """Recursively compile fields into C-friendly tuple structure.

        Returns:
            (names, types, nullable, emaps, sub_schemas)
            - names: tuple of field name strings
            - types: tuple of int type codes (see _DTYPE_MAP)
            - nullable: tuple of bools
            - emaps: tuple of (tuple_of_enum_strings | None) per field
            - sub_schemas: tuple of (compiled_sub | None) per field
        """
        names: list[str] = []
        types: list[int] = []
        nullable: list[bool] = []
        emaps: list[tuple[str, ...] | None] = []
        sub_schemas: list[tuple | None] = []

        for key in order:
            schema = fields[key]
            names.append(key)
            nullable.append(schema.nullable)

            # Enum map
            if schema.enum_values:
                emaps.append(tuple(schema.enum_values))
            else:
                emaps.append(None)

            # Type code and sub-schema
            if schema.dtype == "object" and schema.nested:
                types.append(cls._DTYPE_MAP["object"])
                nested_order = sorted(
                    schema.nested.keys(),
                    key=lambda k, n=schema.nested: (-n[k].frequency, k),
                )
                sub_schemas.append(cls._compile_fields(schema.nested, nested_order))
            elif schema.dtype == "array" and schema.items_schema:
                types.append(cls._DTYPE_MAP["array"])
                items_order = sorted(
                    schema.items_schema.keys(),
                    key=lambda k, it=schema.items_schema: (-it[k].frequency, k),
                )
                sub_schemas.append(cls._compile_fields(schema.items_schema, items_order))
            else:
                types.append(cls._DTYPE_MAP.get(schema.dtype, cls._DTYPE_MAP["str"]))
                sub_schemas.append(None)

        return (
            tuple(names),
            tuple(types),
            tuple(nullable),
            tuple(emaps),
            tuple(sub_schemas),
        )

    def compress(self, data: dict | list[dict]) -> bytes:
        """Compress a JSON response using learned schema.

        Supports single object or array of objects.
        """
        # C fast path for list of records
        if isinstance(data, list) and self._c_schema is not None:
            try:
                raw = _c_encode_records(data, self._c_schema)
                if self.use_brotli:
                    compressed = brotli.compress(raw, quality=6)
                    if len(compressed) < len(raw):
                        return b"\x01" + compressed
                    return b"\x00" + raw
                return b"\x00" + raw
            except Exception:
                pass  # Fall through to Python path

        buf = bytearray()

        if isinstance(data, list):
            buf.append(0x02)  # array marker
            buf.extend(struct.pack(">H", len(data)))
            for item in data:
                self._encode_object(item, self.profile.fields, self.profile.field_order, buf)
        else:
            buf.append(0x01)  # single object marker
            self._encode_object(data, self.profile.fields, self.profile.field_order, buf)

        raw = bytes(buf)

        if self.use_brotli:
            compressed = brotli.compress(raw, quality=6)  # quality 6 = fast + good ratio
            # Only use brotli if it actually helps
            if len(compressed) < len(raw):
                return b"\x01" + compressed  # 0x01 = brotli flag
            return b"\x00" + raw
        return b"\x00" + raw

    def decompress(self, data: bytes) -> dict | list[dict]:
        """Decompress back to original JSON structure.

        Raises:
            PidginDecodeError: if the payload is truncated, has an invalid
                header, fails brotli decompression, contains a malformed
                length prefix that would read past the buffer end, or has a
                corrupt JSON fallback section. The exception preserves the
                original cause via __cause__.
        """
        if not isinstance(data, (bytes, bytearray)):
            raise PidginDecodeError(f"input must be bytes, got {type(data).__name__}")
        if len(data) < 2:
            raise PidginDecodeError(f"payload too short: {len(data)} bytes (minimum 2)")

        try:
            brotli_flag = data[0]
            payload = data[1:]

            if brotli_flag == 0x01:
                try:
                    payload = brotli.decompress(payload)
                except Exception as e:
                    raise PidginDecodeError("brotli decompression failed") from e
            elif brotli_flag != 0x00:
                raise PidginDecodeError(f"invalid brotli flag byte: 0x{brotli_flag:02x}")

            if len(payload) == 0:
                raise PidginDecodeError("payload is empty after header")

            # C fast path for array records
            if self._c_schema is not None and payload[0] == 0x02:
                try:
                    return _c_decode_records(payload, self._c_schema)
                except Exception:
                    pass  # Fall through to Python path

            pos = [0]
            marker = payload[pos[0]]
            pos[0] += 1

            if marker == 0x02:
                if pos[0] + 2 > len(payload):
                    raise PidginDecodeError("array count truncated")
                count = struct.unpack(">H", payload[pos[0]:pos[0]+2])[0]
                pos[0] += 2
                return [self._decode_object(payload, pos, self.profile.fields, self.profile.field_order) for _ in range(count)]
            elif marker == 0x01:
                return self._decode_object(payload, pos, self.profile.fields, self.profile.field_order)
            else:
                raise PidginDecodeError(f"invalid record marker byte: 0x{marker:02x}")
        except PidginDecodeError:
            raise
        except (IndexError, struct.error, json.JSONDecodeError, UnicodeDecodeError) as e:
            raise PidginDecodeError("payload truncated or malformed") from e

    def _encode_object(self, obj: dict, fields: dict[str, FieldSchema], order: list[str], buf: bytearray):
        """Encode a single object according to schema — NO keys transmitted."""
        # Collect fields with None values that aren't nullable — they go to fallback
        none_non_nullable = set()

        for key in order:
            schema = fields[key]
            value = obj.get(key)

            if schema.nullable:
                if key not in obj:
                    buf.append(_MARKER_ABSENT)
                    continue
                if value is None:
                    buf.append(_MARKER_NULL)
                    continue
                buf.append(_MARKER_PRESENT)
            elif value is None or key not in obj:
                # Non-nullable field but value is None — encode type-safe default,
                # store real value in fallback to preserve lossless roundtrip
                none_non_nullable.add(key)
                self._encode_default(schema, buf)
                continue

            self._encode_value(value, schema, buf)

        # Handle unknown fields + non-nullable None fields — store as JSON fallback
        unknown = {k: v for k, v in obj.items() if k not in fields or k in none_non_nullable}
        if unknown:
            fallback = json.dumps(unknown, separators=(",", ":")).encode("utf-8")
            buf.extend(struct.pack(">H", len(fallback)))
            buf.extend(fallback)
        else:
            buf.extend(struct.pack(">H", 0))

    def _decode_object(self, data: bytes, pos: list[int], fields: dict[str, FieldSchema], order: list[str]) -> dict:
        """Decode a single object."""
        result = {}

        for key in order:
            schema = fields[key]

            if schema.nullable:
                marker = data[pos[0]]
                pos[0] += 1
                if marker == _MARKER_NULL:
                    result[key] = None
                    continue
                elif marker == _MARKER_ABSENT:
                    continue

            result[key] = self._decode_value(data, pos, schema)

        # Unknown fields fallback
        fb_len = struct.unpack(">H", data[pos[0]:pos[0]+2])[0]
        pos[0] += 2
        if fb_len > 0:
            fallback = json.loads(data[pos[0]:pos[0]+fb_len])
            pos[0] += fb_len
            result.update(fallback)

        return result

    @staticmethod
    def _encode_default(schema: FieldSchema, buf: bytearray):
        """Encode a type-safe default placeholder for non-nullable field with None value."""
        if schema.dtype == "bool":
            buf.append(0)
        elif schema.dtype == "int":
            buf.append(0)  # varint 0
        elif schema.dtype == "float":
            buf.extend(struct.pack(">d", 0.0))
        elif schema.dtype == "enum":
            buf.append(0)  # first enum value
        elif schema.dtype == "timestamp":
            buf.append(0)  # empty timestamp
        elif schema.dtype == "str":
            buf.extend(struct.pack(">H", 0))  # empty string
        else:
            buf.extend(struct.pack(">H", 0))  # empty fallback

    def _encode_value(self, value: Any, schema: FieldSchema, buf: bytearray):
        """Encode a single value based on its schema type."""
        if schema.dtype == "bool":
            buf.append(1 if value else 0)

        elif schema.dtype == "int":
            # Variable-length integer encoding
            buf.extend(self._encode_varint(value))

        elif schema.dtype == "float":
            buf.extend(struct.pack(">d", value))  # double for full precision

        elif schema.dtype == "enum":
            if value in schema.enum_values:
                buf.append(schema.enum_values.index(value))
            else:
                # Unknown enum value — encode as string with 0xFE marker
                buf.append(0xFE)
                encoded = str(value).encode("utf-8")
                buf.extend(struct.pack(">H", len(encoded)))
                buf.extend(encoded)

        elif schema.dtype == "timestamp":
            # Store as compact string — preserving original format for lossless
            encoded = str(value).encode("utf-8")
            buf.append(len(encoded))  # timestamps are < 256 chars
            buf.extend(encoded)

        elif schema.dtype == "str":
            encoded = value.encode("utf-8") if isinstance(value, str) else str(value).encode("utf-8")
            buf.extend(struct.pack(">H", len(encoded)))
            buf.extend(encoded)

        elif schema.dtype == "object" and schema.nested:
            if isinstance(value, dict):
                nested_order = sorted(schema.nested.keys(), key=lambda k: (-schema.nested[k].frequency, k))
                self._encode_object(value, schema.nested, nested_order, buf)
            else:
                buf.extend(struct.pack(">H", 0))

        elif schema.dtype == "array" and schema.items_schema:
            if isinstance(value, list):
                buf.extend(struct.pack(">H", len(value)))
                items_order = sorted(schema.items_schema.keys(), key=lambda k: (-schema.items_schema[k].frequency, k))
                for item in value:
                    if isinstance(item, dict):
                        self._encode_object(item, schema.items_schema, items_order, buf)
                    else:
                        # Primitive array item — store as JSON
                        s = json.dumps(item, separators=(",", ":")).encode("utf-8")
                        buf.extend(struct.pack(">H", len(s)))
                        buf.extend(s)
            else:
                buf.extend(struct.pack(">H", 0))

        else:
            # Fallback: JSON encode
            s = json.dumps(value, separators=(",", ":")).encode("utf-8")
            buf.extend(struct.pack(">H", len(s)))
            buf.extend(s)

    def _decode_value(self, data: bytes, pos: list[int], schema: FieldSchema) -> Any:
        """Decode a single value based on its schema type."""
        if schema.dtype == "bool":
            val = bool(data[pos[0]])
            pos[0] += 1
            return val

        elif schema.dtype == "int":
            return self._decode_varint(data, pos)

        elif schema.dtype == "float":
            val = struct.unpack(">d", data[pos[0]:pos[0]+8])[0]
            pos[0] += 8
            return val

        elif schema.dtype == "enum":
            idx = data[pos[0]]
            pos[0] += 1
            if idx == 0xFE:
                # Unknown enum — read string fallback
                slen = struct.unpack(">H", data[pos[0]:pos[0]+2])[0]
                pos[0] += 2
                val = data[pos[0]:pos[0]+slen].decode("utf-8")
                pos[0] += slen
                return val
            if idx < len(schema.enum_values):
                return schema.enum_values[idx]
            return None

        elif schema.dtype == "timestamp":
            slen = data[pos[0]]
            pos[0] += 1
            val = data[pos[0]:pos[0]+slen].decode("utf-8")
            pos[0] += slen
            return val

        elif schema.dtype == "str":
            slen = struct.unpack(">H", data[pos[0]:pos[0]+2])[0]
            pos[0] += 2
            val = data[pos[0]:pos[0]+slen].decode("utf-8")
            pos[0] += slen
            return val

        elif schema.dtype == "object" and schema.nested:
            nested_order = sorted(schema.nested.keys(), key=lambda k: (-schema.nested[k].frequency, k))
            return self._decode_object(data, pos, schema.nested, nested_order)

        elif schema.dtype == "array" and schema.items_schema:
            count = struct.unpack(">H", data[pos[0]:pos[0]+2])[0]
            pos[0] += 2
            items_order = sorted(schema.items_schema.keys(), key=lambda k: (-schema.items_schema[k].frequency, k))
            return [self._decode_object(data, pos, schema.items_schema, items_order) for _ in range(count)]

        else:
            slen = struct.unpack(">H", data[pos[0]:pos[0]+2])[0]
            pos[0] += 2
            val = json.loads(data[pos[0]:pos[0]+slen])
            pos[0] += slen
            return val

    @staticmethod
    def _encode_varint(value: int) -> bytes:
        """Encode signed integer as variable-length bytes using ZigZag encoding."""
        # ZigZag: map signed to unsigned (0→0, -1→1, 1→2, -2→3, 2→4, ...)
        value = (value << 1) ^ (value >> 63)
        buf = bytearray()
        while value > 0x7F:
            buf.append((value & 0x7F) | 0x80)
            value >>= 7
        buf.append(value & 0x7F)
        return bytes(buf)

    @staticmethod
    def _decode_varint(data: bytes, pos: list[int]) -> int:
        """Decode ZigZag variable-length integer."""
        result = 0
        shift = 0
        while True:
            byte = data[pos[0]]
            pos[0] += 1
            result |= (byte & 0x7F) << shift
            if not (byte & 0x80):
                break
            shift += 7
        # Undo ZigZag: unsigned back to signed
        return (result >> 1) ^ -(result & 1)
