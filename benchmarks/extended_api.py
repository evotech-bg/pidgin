#!/usr/bin/env python3
"""
Extended benchmark — add modern binary JSON formats to the comparison.

Adds: CBOR (RFC 8949), Avro (schemaless), BSON (MongoDB wire), Ion (binary).
Reuses data generators from realistic_api.py.

Smile (Jackson binary JSON) is omitted — no maintained Python implementation.
"""

import io
import sys
import time
from pathlib import Path

import bson
import brotli
import cbor2
import fastavro
import gzip
import msgpack
import orjson
import zstandard
from amazon.ion import simpleion as ion

sys.path.insert(0, str(Path(__file__).parent))
import benchmark_pb2 as pb
from realistic_api import (
    gen_users, gen_orders, gen_events,
    _dict_to_user, _dict_to_order, _dict_to_event,
    compress_protobuf, compress_protobuf_zstd,
)

from pidgin import SchemaCodec


# ============================================================
# Avro schemas (hand-written for fairness — gives Avro the best chance)
# ============================================================

USER_SCHEMA = {
    "type": "array",
    "items": {
        "type": "record", "name": "User",
        "fields": [
            {"name": "id", "type": "int"},
            {"name": "uuid", "type": "string"},
            {"name": "name", "type": "string"},
            {"name": "email", "type": "string"},
            {"name": "role", "type": {"type": "enum", "name": "Role",
                "symbols": ["admin", "editor", "viewer", "billing", "support"]}},
            {"name": "department", "type": ["null", "string"], "default": None},
            {"name": "is_active", "type": "boolean"},
            {"name": "is_verified", "type": "boolean"},
            {"name": "login_count", "type": "int"},
            {"name": "storage_used_mb", "type": "double"},
            {"name": "created_at", "type": "string"},
            {"name": "last_login_at", "type": "string"},
            {"name": "avatar_url", "type": ["null", "string"], "default": None},
            {"name": "locale", "type": {"type": "enum", "name": "Locale",
                "symbols": ["en", "bg", "de", "fr", "es", "ja", "zh"]}},
        ],
    },
}

ORDER_SCHEMA = {
    "type": "array",
    "items": {
        "type": "record", "name": "Order",
        "fields": [
            {"name": "order_id", "type": "string"},
            {"name": "customer_id", "type": "int"},
            {"name": "status", "type": {"type": "enum", "name": "Status",
                "symbols": ["pending", "processing", "shipped", "delivered", "cancelled", "refunded"]}},
            {"name": "currency", "type": {"type": "enum", "name": "Currency",
                "symbols": ["EUR", "USD", "GBP"]}},
            {"name": "subtotal", "type": "double"},
            {"name": "tax", "type": "double"},
            {"name": "shipping_cost", "type": "double"},
            {"name": "total", "type": "double"},
            {"name": "items", "type": {
                "type": "array",
                "items": {
                    "type": "record", "name": "Item",
                    "fields": [
                        {"name": "sku", "type": "string"},
                        {"name": "name", "type": "string"},
                        {"name": "quantity", "type": "int"},
                        {"name": "unit_price", "type": "double"},
                        {"name": "total", "type": "double"},
                        {"name": "category", "type": "string"},
                    ],
                },
            }},
            {"name": "shipping_address", "type": {
                "type": "record", "name": "Address",
                "fields": [
                    {"name": "line1", "type": "string"},
                    {"name": "city", "type": "string"},
                    {"name": "postal_code", "type": "string"},
                    {"name": "country", "type": "string"},
                ],
            }},
            {"name": "payment_method", "type": "string"},
            {"name": "carrier", "type": ["null", "string"], "default": None},
            {"name": "tracking_number", "type": ["null", "string"], "default": None},
            {"name": "created_at", "type": "string"},
            {"name": "updated_at", "type": "string"},
            {"name": "notes", "type": ["null", "string"], "default": None},
        ],
    },
}

EVENT_SCHEMA = {
    "type": "array",
    "items": {
        "type": "record", "name": "Event",
        "fields": [
            {"name": "event_id", "type": "string"},
            {"name": "session_id", "type": "string"},
            {"name": "user_id", "type": ["null", "int"], "default": None},
            {"name": "event_type", "type": "string"},
            {"name": "page", "type": "string"},
            {"name": "referrer", "type": ["null", "string"], "default": None},
            {"name": "browser", "type": "string"},
            {"name": "os", "type": "string"},
            {"name": "country", "type": "string"},
            {"name": "viewport_width", "type": "int"},
            {"name": "viewport_height", "type": "int"},
            {"name": "load_time_ms", "type": "int"},
            {"name": "is_bot", "type": "boolean"},
            {"name": "timestamp", "type": "string"},
        ],
    },
}


# ============================================================
# Encoders
# ============================================================

def encode_cbor(data):
    return cbor2.dumps(data)

def encode_cbor_zstd(data):
    return zstandard.ZstdCompressor(level=6).compress(cbor2.dumps(data))

def encode_avro(data, schema):
    buf = io.BytesIO()
    fastavro.schemaless_writer(buf, schema, data)
    return buf.getvalue()

def encode_avro_zstd(data, schema):
    return zstandard.ZstdCompressor(level=6).compress(encode_avro(data, schema))

def encode_bson(data):
    # BSON encodes single document; wrap list under a key for array support
    return bson.encode({"items": data})

def encode_bson_zstd(data):
    return zstandard.ZstdCompressor(level=6).compress(encode_bson(data))

def encode_ion(data):
    return ion.dumps(data, binary=True, omit_version_marker=False)

def encode_ion_zstd(data):
    return zstandard.ZstdCompressor(level=6).compress(encode_ion(data))


# ============================================================
# Benchmark runner
# ============================================================

def benchmark_dataset(name: str, data: list, *, avro_schema=None,
                     proto_converter=None, proto_list_cls=None, proto_list_field=None):
    json_raw = orjson.dumps(data)
    json_size = len(json_raw)

    sample_size = min(20, len(data))
    codec = SchemaCodec.learn(data[:sample_size])

    methods = [
        ("JSON (orjson)",         lambda: orjson.dumps(data)),
        ("gzip -6",               lambda: gzip.compress(orjson.dumps(data), 6)),
        ("brotli -6",             lambda: brotli.compress(orjson.dumps(data), quality=6)),
        ("zstd -6",               lambda: zstandard.ZstdCompressor(6).compress(orjson.dumps(data))),
        ("msgpack",               lambda: msgpack.packb(data, use_bin_type=True)),
        ("msgpack + zstd",        lambda: zstandard.ZstdCompressor(6).compress(msgpack.packb(data, use_bin_type=True))),
        ("CBOR",                  lambda: encode_cbor(data)),
        ("CBOR + zstd",           lambda: encode_cbor_zstd(data)),
        ("BSON",                  lambda: encode_bson(data)),
        ("BSON + zstd",           lambda: encode_bson_zstd(data)),
        ("Ion (binary)",          lambda: encode_ion(data)),
        ("Ion + zstd",            lambda: encode_ion_zstd(data)),
    ]

    if avro_schema is not None:
        methods.append(("Avro (schemaless)",  lambda: encode_avro(data, avro_schema)))
        methods.append(("Avro + zstd",        lambda: encode_avro_zstd(data, avro_schema)))

    if proto_converter is not None:
        methods.append(("Protobuf",        lambda: compress_protobuf(data, proto_converter, proto_list_cls, proto_list_field)))
        methods.append(("Protobuf + zstd", lambda: compress_protobuf_zstd(data, proto_converter, proto_list_cls, proto_list_field)))

    methods.append(("Pidgin",         lambda: codec.compress(data)))

    print(f"\n{'─' * 80}")
    print(f"  {name} — {len(data)} records, JSON baseline: {json_size:,} bytes")
    print(f"{'─' * 80}")
    print(f"  {'Method':<24} {'Size':>10} {'Ratio':>8} {'Saved':>8} {'Time':>10}")
    print(f"  {'─'*24} {'─'*10} {'─'*8} {'─'*8} {'─'*10}")

    rows = []
    for method_name, fn in methods:
        try:
            fn()  # warmup
            times = []
            result = None
            for _ in range(3):
                t0 = time.perf_counter()
                result = fn()
                times.append(time.perf_counter() - t0)
            size = len(result)
            ratio = size / json_size
            saved = 1 - ratio
            best_time = min(times)
            rows.append((method_name, size, ratio, saved, best_time))
            print(f"  {method_name:<24} {size:>9,}B {ratio:>7.1%} {saved:>7.1%} {best_time*1000:>8.2f}ms")
        except Exception as e:
            print(f"  {method_name:<24} ERROR: {type(e).__name__}: {str(e)[:50]}")
            rows.append((method_name, None, None, None, None))

    # Find winner (smallest size)
    valid = [r for r in rows if r[1] is not None and r[0] != "JSON (orjson)"]
    if valid:
        winner = min(valid, key=lambda r: r[1])
        print(f"\n  → Winner: {winner[0]} ({winner[2]:.1%} of JSON)")
    return rows


def main():
    print("=" * 80)
    print("  PIDGIN EXTENDED BENCHMARK — vs CBOR / Avro / BSON / Ion")
    print("=" * 80)

    benchmark_dataset("/api/v2/users (1000)", gen_users(1000),
                      avro_schema=USER_SCHEMA,
                      proto_converter=_dict_to_user,
                      proto_list_cls=pb.UserList, proto_list_field="users")

    benchmark_dataset("/api/v2/orders (500)", gen_orders(500),
                      avro_schema=ORDER_SCHEMA,
                      proto_converter=_dict_to_order,
                      proto_list_cls=pb.OrderList, proto_list_field="orders")

    benchmark_dataset("/api/analytics/events (5000)", gen_events(5000),
                      avro_schema=EVENT_SCHEMA,
                      proto_converter=_dict_to_event,
                      proto_list_cls=pb.EventList, proto_list_field="events")

    print("\n" + "=" * 80)
    print("  Note: Smile (Jackson binary JSON) excluded — no maintained Python lib.")
    print("=" * 80)


if __name__ == "__main__":
    main()
