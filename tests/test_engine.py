"""Tests for PidginEngine — auto-learning compression."""

import json
from pidgin.contrib._engine import PidginEngine


def _make_users(n: int) -> list[dict]:
    return [
        {"id": i, "name": f"User {i}", "email": f"u{i}@test.com",
         "role": "admin" if i % 3 == 0 else "editor", "active": i % 2 == 0}
        for i in range(n)
    ]


def test_learning_phase():
    engine = PidginEngine(learn_samples=10)
    assert not engine.is_ready("/api/users")

    for i in range(5):
        engine.observe("/api/users", _make_users(3))

    # 15 samples observed (5 * 3), above threshold of 10
    assert engine.is_ready("/api/users")


def test_compress_after_learning():
    engine = PidginEngine(learn_samples=5)
    engine.observe("/api/users", _make_users(10))
    assert engine.is_ready("/api/users")

    data = _make_users(100)
    compressed = engine.compress("/api/users", data)
    assert compressed is not None
    assert len(compressed) < len(json.dumps(data).encode())


def test_profile_serving():
    engine = PidginEngine(learn_samples=5)
    assert engine.get_profile("/api/users") is None

    engine.observe("/api/users", _make_users(10))
    profile = engine.get_profile("/api/users")
    assert profile is not None

    parsed = json.loads(profile)
    assert "fields" in parsed
    assert "field_order" in parsed


def test_etag():
    engine = PidginEngine(learn_samples=5)
    engine.observe("/api/users", _make_users(10))
    etag = engine.get_profile_etag("/api/users")
    assert etag is not None
    assert etag.startswith('"pidgin-v')


def test_independent_endpoints():
    engine = PidginEngine(learn_samples=5)
    engine.observe("/api/users", _make_users(10))
    engine.observe("/api/orders", [{"order_id": f"ORD-{i}", "total": i * 10.5} for i in range(10)])

    assert engine.is_ready("/api/users")
    assert engine.is_ready("/api/orders")
    assert not engine.is_ready("/api/products")


def test_list_endpoints():
    engine = PidginEngine(learn_samples=5)
    engine.observe("/api/users", _make_users(10))
    status = engine.list_endpoints()
    assert "/api/users" in status
    assert status["/api/users"]["status"] == "ready"


def test_pagination_wrapper():
    """Engine extracts records from common pagination wrappers."""
    engine = PidginEngine(learn_samples=5)
    wrapped = {"data": _make_users(10), "total": 100, "page": 1}
    engine.observe("/api/users", wrapped)
    assert engine.is_ready("/api/users")


def test_not_ready_returns_none():
    engine = PidginEngine(learn_samples=100)
    engine.observe("/api/users", _make_users(3))
    assert engine.compress("/api/users", _make_users(10)) is None


def test_profile_endpoint_url():
    engine = PidginEngine()
    assert engine.get_profile_endpoint("/api/v2/users") == "/.well-known/pidgin/api-v2-users"
