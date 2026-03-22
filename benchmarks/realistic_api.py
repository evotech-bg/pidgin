#!/usr/bin/env python3
"""
Benchmark: Pidgin vs industry-standard compression on realistic API data.

Compares: raw JSON, gzip, brotli, zstd, msgpack, Pidgin SchemaCodec, Pidgin + RatchetCipher
Datasets: Users API, E-commerce Orders, Analytics Events, Nested Catalog
"""

import gzip
import json
import random
import string
import struct
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

import brotli
import msgpack
import orjson
import zstandard

# Add benchmarks dir to path for proto imports
sys.path.insert(0, str(Path(__file__).parent))
import benchmark_pb2 as pb

from pidgin import SchemaCodec, RatchetCipher


# ============================================================
# Realistic data generators
# ============================================================

def gen_users(n: int) -> list[dict]:
    """Realistic /api/v2/users response — flat with enums, timestamps, nullable."""
    roles = ["admin", "editor", "viewer", "billing", "support"]
    departments = ["Engineering", "Marketing", "Sales", "HR", "Finance", "Legal", "Product", None]
    random.seed(42)
    users = []
    base = datetime(2024, 1, 1)
    for i in range(n):
        created = base + timedelta(days=random.randint(0, 800), seconds=random.randint(0, 86400))
        last_login = created + timedelta(days=random.randint(0, 200), seconds=random.randint(0, 86400))
        users.append({
            "id": 10000 + i,
            "uuid": f"usr_{random.randbytes(8).hex()}",
            "name": f"{''.join(random.choices(string.ascii_uppercase, k=1))}{''.join(random.choices(string.ascii_lowercase, k=random.randint(4,10)))} {''.join(random.choices(string.ascii_uppercase, k=1))}{''.join(random.choices(string.ascii_lowercase, k=random.randint(4,12)))}",
            "email": f"user{i}@{''.join(random.choices(string.ascii_lowercase, k=5))}.com",
            "role": random.choice(roles),
            "department": random.choice(departments),
            "is_active": random.random() > 0.15,
            "is_verified": random.random() > 0.3,
            "login_count": random.randint(0, 2000),
            "storage_used_mb": round(random.uniform(0, 5000), 2),
            "created_at": created.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "last_login_at": last_login.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "avatar_url": f"https://cdn.example.com/avatars/{10000+i}.jpg" if random.random() > 0.3 else None,
            "locale": random.choice(["en", "bg", "de", "fr", "es", "ja", "zh"]),
        })
    return users


def gen_orders(n: int) -> list[dict]:
    """Realistic /api/v2/orders response — nested line_items, address, payment."""
    statuses = ["pending", "processing", "shipped", "delivered", "cancelled", "refunded"]
    currencies = ["EUR", "USD", "GBP"]
    carriers = ["DHL", "FedEx", "UPS", "DPD", "GLS", None]
    countries = ["BG", "DE", "US", "FR", "GB", "JP", "NL", "ES", "IT"]
    random.seed(43)
    orders = []
    for i in range(n):
        n_items = random.randint(1, 6)
        items = []
        subtotal = 0
        for j in range(n_items):
            qty = random.randint(1, 5)
            price = round(random.uniform(5, 500), 2)
            items.append({
                "sku": f"SKU-{random.randint(10000,99999)}",
                "name": f"Product {''.join(random.choices(string.ascii_uppercase, k=2))}-{random.randint(100,999)}",
                "quantity": qty,
                "unit_price": price,
                "total": round(qty * price, 2),
                "category": random.choice(["Electronics", "Clothing", "Home", "Books", "Sports", "Food"]),
            })
            subtotal += qty * price
        tax = round(subtotal * 0.2, 2)
        shipping = round(random.uniform(0, 15), 2)
        total = round(subtotal + tax + shipping, 2)
        created = datetime(2025, 1, 1) + timedelta(days=random.randint(0, 400), seconds=random.randint(0, 86400))
        country = random.choice(countries)
        orders.append({
            "order_id": f"ORD-{2025}-{random.randint(10000,99999)}",
            "customer_id": random.randint(10000, 50000),
            "status": random.choice(statuses),
            "currency": random.choice(currencies),
            "subtotal": round(subtotal, 2),
            "tax": tax,
            "shipping_cost": shipping,
            "total": total,
            "items": items,
            "shipping_address": {
                "line1": f"{random.randint(1,200)} {''.join(random.choices(string.ascii_uppercase, k=1))}{''.join(random.choices(string.ascii_lowercase, k=6))} St",
                "city": f"City-{random.randint(1,50)}",
                "postal_code": f"{random.randint(10000,99999)}",
                "country": country,
            },
            "payment_method": random.choice(["credit_card", "paypal", "bank_transfer", "apple_pay"]),
            "carrier": random.choice(carriers),
            "tracking_number": f"TRK{random.randbytes(6).hex().upper()}" if random.random() > 0.4 else None,
            "created_at": created.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "updated_at": (created + timedelta(hours=random.randint(1, 72))).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "notes": f"Customer note #{i}" if random.random() > 0.7 else None,
        })
    return orders


def gen_events(n: int) -> list[dict]:
    """Realistic analytics events — high cardinality, lots of repetition in structure."""
    event_types = ["page_view", "click", "scroll", "form_submit", "purchase", "signup", "logout", "error"]
    browsers = ["Chrome/124", "Firefox/128", "Safari/17", "Edge/124"]
    oses = ["Windows 11", "macOS 15", "iOS 18", "Android 15", "Linux"]
    pages = ["/", "/products", "/cart", "/checkout", "/account", "/about", "/contact", "/blog", "/pricing"]
    random.seed(44)
    events = []
    base = datetime(2026, 3, 20)
    for i in range(n):
        ts = base + timedelta(seconds=i * random.uniform(0.1, 2))
        events.append({
            "event_id": f"evt_{random.randbytes(8).hex()}",
            "session_id": f"sess_{random.randbytes(4).hex()}",
            "user_id": random.randint(10000, 20000) if random.random() > 0.3 else None,
            "event_type": random.choice(event_types),
            "page": random.choice(pages),
            "referrer": random.choice(["google", "direct", "facebook", "twitter", "email", None]),
            "browser": random.choice(browsers),
            "os": random.choice(oses),
            "country": random.choice(["BG", "DE", "US", "FR", "GB"]),
            "viewport_width": random.choice([375, 768, 1024, 1280, 1440, 1920]),
            "viewport_height": random.choice([667, 1024, 768, 800, 900, 1080]),
            "load_time_ms": random.randint(200, 5000),
            "is_bot": random.random() < 0.05,
            "timestamp": ts.strftime("%Y-%m-%dT%H:%M:%S.") + f"{random.randint(0,999):03d}Z",
        })
    return events


# ============================================================
# Compression methods
# ============================================================

def compress_json(data: list[dict]) -> bytes:
    return orjson.dumps(data)

def compress_gzip(data: list[dict]) -> bytes:
    return gzip.compress(orjson.dumps(data), compresslevel=6)

def compress_brotli(data: list[dict]) -> bytes:
    return brotli.compress(orjson.dumps(data), quality=6)

def compress_zstd(data: list[dict]) -> bytes:
    cctx = zstandard.ZstdCompressor(level=6)
    return cctx.compress(orjson.dumps(data))

def compress_msgpack(data: list[dict]) -> bytes:
    return msgpack.packb(data, use_bin_type=True)

def compress_msgpack_zstd(data: list[dict]) -> bytes:
    cctx = zstandard.ZstdCompressor(level=6)
    return cctx.compress(msgpack.packb(data, use_bin_type=True))

def compress_pidgin(data: list[dict], codec: SchemaCodec) -> bytes:
    return codec.compress(data)

def compress_pidgin_encrypted(data: list[dict], codec: SchemaCodec, cipher: RatchetCipher) -> bytes:
    compressed = codec.compress(data)
    return cipher.encrypt(compressed)


# === Protobuf ===

def _dict_to_user(d: dict) -> pb.User:
    u = pb.User()
    u.id = d["id"]
    u.uuid = d["uuid"]
    u.name = d["name"]
    u.email = d["email"]
    u.role = d["role"]
    if d.get("department") is not None:
        u.department = d["department"]
    u.is_active = d["is_active"]
    u.is_verified = d["is_verified"]
    u.login_count = d["login_count"]
    u.storage_used_mb = d["storage_used_mb"]
    u.created_at = d["created_at"]
    u.last_login_at = d["last_login_at"]
    if d.get("avatar_url") is not None:
        u.avatar_url = d["avatar_url"]
    u.locale = d["locale"]
    return u

def _dict_to_order(d: dict) -> pb.Order:
    o = pb.Order()
    o.order_id = d["order_id"]
    o.customer_id = d["customer_id"]
    o.status = d["status"]
    o.currency = d["currency"]
    o.subtotal = d["subtotal"]
    o.tax = d["tax"]
    o.shipping_cost = d["shipping_cost"]
    o.total = d["total"]
    for item_d in d["items"]:
        item = o.items.add()
        item.sku = item_d["sku"]
        item.name = item_d["name"]
        item.quantity = item_d["quantity"]
        item.unit_price = item_d["unit_price"]
        item.total = item_d["total"]
        item.category = item_d["category"]
    addr = d["shipping_address"]
    o.shipping_address.line1 = addr["line1"]
    o.shipping_address.city = addr["city"]
    o.shipping_address.postal_code = addr["postal_code"]
    o.shipping_address.country = addr["country"]
    o.payment_method = d["payment_method"]
    if d.get("carrier") is not None:
        o.carrier = d["carrier"]
    if d.get("tracking_number") is not None:
        o.tracking_number = d["tracking_number"]
    o.created_at = d["created_at"]
    o.updated_at = d["updated_at"]
    if d.get("notes") is not None:
        o.notes = d["notes"]
    return o

def _dict_to_event(d: dict) -> pb.Event:
    e = pb.Event()
    e.event_id = d["event_id"]
    e.session_id = d["session_id"]
    if d.get("user_id") is not None:
        e.user_id = d["user_id"]
    e.event_type = d["event_type"]
    e.page = d["page"]
    if d.get("referrer") is not None:
        e.referrer = d["referrer"]
    e.browser = d["browser"]
    e.os = d["os"]
    e.country = d["country"]
    e.viewport_width = d["viewport_width"]
    e.viewport_height = d["viewport_height"]
    e.load_time_ms = d["load_time_ms"]
    e.is_bot = d["is_bot"]
    e.timestamp = d["timestamp"]
    return e

_PROTO_CONVERTERS = {}

def compress_protobuf(data: list[dict], converter, list_cls, list_field: str) -> bytes:
    msg = list_cls()
    lst = getattr(msg, list_field)
    for d in data:
        lst.append(converter(d))
    return msg.SerializeToString()

def compress_protobuf_zstd(data: list[dict], converter, list_cls, list_field: str) -> bytes:
    raw = compress_protobuf(data, converter, list_cls, list_field)
    cctx = zstandard.ZstdCompressor(level=6)
    return cctx.compress(raw)


# ============================================================
# Benchmark runner
# ============================================================

def benchmark_dataset(name: str, data: list[dict], n_display: int | None = None,
                       proto_converter=None, proto_list_cls=None, proto_list_field=None):
    """Run all compression methods on a dataset and print comparison."""
    display_n = n_display or len(data)
    json_raw = orjson.dumps(data)
    json_size = len(json_raw)

    # Learn schema for Pidgin
    sample_size = min(20, len(data))
    codec = SchemaCodec.learn(data[:sample_size])

    # Cipher pair
    alice = RatchetCipher(b"benchmark-key-2026")
    bob = RatchetCipher(b"benchmark-key-2026")

    methods = [
        ("JSON (orjson)",         lambda: compress_json(data)),
        ("gzip -6",               lambda: compress_gzip(data)),
        ("brotli -6",             lambda: compress_brotli(data)),
        ("zstd -6",               lambda: compress_zstd(data)),
        ("msgpack",               lambda: compress_msgpack(data)),
        ("msgpack + zstd",        lambda: compress_msgpack_zstd(data)),
    ]

    # Protobuf (with pre-compiled .proto)
    if proto_converter:
        methods.append(("Protobuf",           lambda: compress_protobuf(data, proto_converter, proto_list_cls, proto_list_field)))
        methods.append(("Protobuf + zstd",    lambda: compress_protobuf_zstd(data, proto_converter, proto_list_cls, proto_list_field)))

    methods.append(("Pidgin SchemaCodec",     lambda: compress_pidgin(data, codec)))
    methods.append(("Pidgin + RatchetCipher", lambda: compress_pidgin_encrypted(data, codec, alice)))

    print(f"\n{'─' * 78}")
    print(f"  {name} — {display_n} records, JSON baseline: {json_size:,} bytes")
    print(f"{'─' * 78}")
    print(f"  {'Method':<24} {'Size':>10} {'Ratio':>8} {'Saved':>8} {'Time':>10}")
    print(f"  {'─'*24} {'─'*10} {'─'*8} {'─'*8} {'─'*10}")

    for method_name, fn in methods:
        # Warmup
        fn()

        # Timed run (best of 3)
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

        bar = "█" * int(ratio * 30)
        print(f"  {method_name:<24} {size:>9,}B {ratio:>7.1%} {saved:>7.1%} {best_time*1000:>8.2f}ms  {bar}")

    # Verify Pidgin lossless roundtrip (fresh cipher pair)
    verify_a = RatchetCipher(b"verify-key")
    verify_b = RatchetCipher(b"verify-key")
    compressed = codec.compress(data)
    encrypted = verify_a.encrypt(compressed)
    decrypted = verify_b.decrypt(encrypted)
    restored = codec.decompress(decrypted)
    lossless = data == restored
    print(f"\n  Pidgin roundtrip (compress→encrypt→decrypt→decompress): {'PASS' if lossless else 'FAIL'}")


def main():
    print("=" * 78)
    print("  PIDGIN BENCHMARK — Realistic API Data vs Industry Protocols")
    print("=" * 78)

    # Dataset 1: Users (flat, enums, nullable, timestamps)
    for n in [10, 100, 1000]:
        benchmark_dataset(f"/api/v2/users", gen_users(n), n,
                          proto_converter=_dict_to_user,
                          proto_list_cls=pb.UserList, proto_list_field="users")

    # Dataset 2: Orders (deeply nested — items[], address{}, payment)
    for n in [10, 100, 500]:
        benchmark_dataset(f"/api/v2/orders", gen_orders(n), n,
                          proto_converter=_dict_to_order,
                          proto_list_cls=pb.OrderList, proto_list_field="orders")

    # Dataset 3: Analytics events (high volume, high cardinality)
    for n in [100, 1000, 5000]:
        benchmark_dataset(f"/api/analytics/events", gen_events(n), n,
                          proto_converter=_dict_to_event,
                          proto_list_cls=pb.EventList, proto_list_field="events")

    # C extension status
    try:
        from pidgin._fast import ratchet_from_random
        c_ext = "LOADED"
    except ImportError:
        c_ext = "not available (pure Python)"

    print(f"\n{'=' * 78}")
    print(f"  Notes:")
    print(f"  - C extension: {c_ext}")
    print(f"  - Pidgin learns schema from first 20 records, then compresses the full set")
    print(f"  - Protobuf uses pre-compiled .proto with typed messages (the gold standard)")
    print(f"  - RatchetCipher adds 4 bytes overhead (counter) + zero size increase")
    print(f"  - gzip/brotli/zstd compress the full JSON string (keys + values)")
    print(f"  - Pidgin strips keys entirely — only values + type markers on the wire")
    print(f"  - msgpack is binary JSON (still transmits keys)")
    print(f"{'=' * 78}")


if __name__ == "__main__":
    main()
