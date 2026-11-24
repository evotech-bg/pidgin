"""End-to-end checks for the FastAPI and Django middleware.

A client that sends Accept-Encoding: pidgin must get a response it can decode
back to the exact JSON the view returned, or plain JSON. Never a broken body.
"""

import json

import pytest

from pidgin.schema_codec import SchemaCodec, SchemaProfile

USERS = [
    {"id": i, "name": f"User {i}", "email": f"user{i}@example.com",
     "role": "admin" if i % 5 == 0 else "editor", "active": i % 3 != 0}
    for i in range(1, 201)
]


def page(n: int) -> list[dict]:
    n = int(n) % 10
    return USERS[n * 20:n * 20 + 20]


def wrapped(n: int) -> dict:
    return {"data": page(n), "total": len(USERS), "page": int(n)}


def _decode(get, path_and_query, expected):
    """Learn, then request with pidgin and check the decoded body."""
    for i in range(40):
        get(f"{path_and_query}?page={i}", {})
    status, headers, body = get(f"{path_and_query}?page=3", {"Accept-Encoding": "pidgin"})
    assert status == 200
    if "content-length" in headers:
        assert int(headers["content-length"]) == len(body)
    if headers.get("content-encoding") == "pidgin":
        _, _, profile = get(headers["x-pidgin-profile"], {})
        codec = SchemaCodec(SchemaProfile.from_json(profile.decode()))
        assert codec.decompress(body) == expected
        return True
    assert json.loads(body) == expected
    return False


def test_fastapi_roundtrip():
    pytest.importorskip("fastapi")
    pytest.importorskip("httpx")
    from fastapi import FastAPI
    from fastapi.testclient import TestClient
    from pidgin.contrib.fastapi import PidginMiddleware

    app = FastAPI()
    app.add_middleware(PidginMiddleware)

    @app.get("/api/users")
    async def users(page: int = 1):
        return globals()["page"](page)

    @app.get("/api/wrapped")
    async def wr(page: int = 1):
        return wrapped(page)

    client = TestClient(app)

    def get(url, headers):
        r = client.get(url, headers=headers)
        return r.status_code, {k.lower(): v for k, v in r.headers.items()}, r.content

    assert _decode(get, "/api/users", page(3)) is True
    # Paginated wrappers are not compressed (plain JSON), but never broken.
    _decode(get, "/api/wrapped", wrapped(3))


def test_django_roundtrip():
    pytest.importorskip("django")
    from django.conf import settings

    if not settings.configured:
        settings.configure(DEBUG=False, ALLOWED_HOSTS=["*"], SECRET_KEY="test-only",
                           ROOT_URLCONF=__name__, MIDDLEWARE=[])
    from django.http import JsonResponse
    from django.test import RequestFactory
    from pidgin.contrib import django as pdj

    pdj._engine = None  # fresh engine for this test
    rf = RequestFactory()

    def view(request):
        n = request.GET.get("page", 1)
        if request.path == "/api/wrapped":
            return JsonResponse(wrapped(n))
        return JsonResponse(page(n), safe=False)

    mw = pdj.PidginMiddleware(view)

    def get(url, headers):
        extra = {"HTTP_ACCEPT_ENCODING": headers["Accept-Encoding"]} if headers else {}
        r = mw(rf.get(url, **extra))
        return r.status_code, {k.lower(): v for k, v in r.items()}, r.content

    assert _decode(get, "/api/users", page(3)) is True
    _decode(get, "/api/wrapped", wrapped(3))
