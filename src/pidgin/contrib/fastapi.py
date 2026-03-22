"""
Pidgin middleware for FastAPI/Starlette.

Usage:
    from fastapi import FastAPI
    from pidgin.contrib.fastapi import PidginMiddleware

    app = FastAPI()
    app.add_middleware(PidginMiddleware)

    # That's it. Pidgin auto-learns schemas and compresses JSON responses.
    # Profiles served at /.well-known/pidgin/{endpoint}
"""

from __future__ import annotations

import json
from typing import Callable

from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import Response

from ._engine import PidginEngine


class PidginMiddleware(BaseHTTPMiddleware):
    """Drop-in compression middleware for FastAPI.

    Auto-learns response schemas, negotiates encoding via Accept-Encoding header,
    and transparently compresses JSON responses.

    Args:
        app: ASGI app
        learn_samples: Number of responses to observe before compressing (default 30)
        evolve_interval: Check for schema drift every N requests (default 500)
        profile_prefix: URL prefix for profile endpoints (default /.well-known/pidgin)
        exclude_paths: Path prefixes to skip (default: /docs, /openapi.json, /redoc)
        min_size: Minimum response size in bytes to compress (default 256)
    """

    def __init__(
        self,
        app,
        learn_samples: int = 30,
        evolve_interval: int = 500,
        profile_prefix: str = "/.well-known/pidgin",
        exclude_paths: list[str] | None = None,
        min_size: int = 256,
    ):
        super().__init__(app)
        self.engine = PidginEngine(
            learn_samples=learn_samples,
            evolve_interval=evolve_interval,
            profile_prefix=profile_prefix,
        )
        self.exclude_paths = exclude_paths or ["/docs", "/openapi.json", "/redoc"]
        self.min_size = min_size

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        path = request.url.path

        # Serve profile requests
        if path.startswith(self.engine.profile_prefix):
            return self._serve_profile(request, path)

        # Status endpoint
        if path == f"{self.engine.profile_prefix}/_status":
            return Response(
                content=json.dumps(self.engine.list_endpoints(), indent=2),
                media_type="application/json",
            )

        # Skip excluded paths
        if any(path.startswith(p) for p in self.exclude_paths):
            return await call_next(request)

        # Process response
        response = await call_next(request)

        # Only compress JSON responses
        content_type = response.headers.get("content-type", "")
        if "application/json" not in content_type:
            return response

        # Read response body
        body_bytes = b""
        async for chunk in response.body_iterator:
            if isinstance(chunk, str):
                body_bytes += chunk.encode()
            else:
                body_bytes += chunk

        # Too small to bother
        if len(body_bytes) < self.min_size:
            return Response(
                content=body_bytes,
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

        # Parse JSON
        try:
            data = json.loads(body_bytes)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return Response(
                content=body_bytes,
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

        # Normalize endpoint (strip query params, collapse IDs)
        endpoint = self._normalize_endpoint(path)

        # Feed to learning engine
        self.engine.observe(endpoint, data)

        # Check if client accepts pidgin
        accept = request.headers.get("accept-encoding", "")
        if "pidgin" not in accept or not self.engine.is_ready(endpoint):
            return Response(
                content=body_bytes,
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

        # Compress
        compressed = self.engine.compress(endpoint, data)
        if compressed is None or len(compressed) >= len(body_bytes):
            return Response(
                content=body_bytes,
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

        # Return compressed response
        headers = dict(response.headers)
        headers["content-encoding"] = "pidgin"
        headers["content-type"] = "application/octet-stream"
        headers["x-pidgin-profile"] = self.engine.get_profile_endpoint(endpoint)
        headers["x-pidgin-original-size"] = str(len(body_bytes))
        etag = self.engine.get_profile_etag(endpoint)
        if etag:
            headers["x-pidgin-etag"] = etag

        return Response(
            content=compressed,
            status_code=response.status_code,
            headers=headers,
        )

    def _serve_profile(self, request: Request, path: str) -> Response:
        """Serve a profile JSON for the given endpoint."""
        # Extract endpoint name from path
        suffix = path[len(self.engine.profile_prefix):].strip("/")
        if not suffix:
            # List all profiles
            return Response(
                content=json.dumps(self.engine.list_endpoints(), indent=2),
                media_type="application/json",
            )

        # Find matching endpoint
        endpoint = "/" + suffix.replace("-", "/")
        profile_json = self.engine.get_profile(endpoint)
        if profile_json is None:
            return Response(
                content=json.dumps({"error": "profile not ready", "status": "learning"}),
                status_code=404,
                media_type="application/json",
            )

        etag = self.engine.get_profile_etag(endpoint)
        client_etag = request.headers.get("if-none-match")
        if client_etag and client_etag == etag:
            return Response(status_code=304)

        return Response(
            content=profile_json,
            media_type="application/json",
            headers={
                "etag": etag or "",
                "cache-control": "public, max-age=86400",
            },
        )

    @staticmethod
    def _normalize_endpoint(path: str) -> str:
        """Normalize path: /api/users/123/posts → /api/users/:id/posts"""
        parts = path.strip("/").split("/")
        normalized = []
        for part in parts:
            if part.isdigit() or (len(part) > 8 and not part.isalpha()):
                normalized.append(":id")
            else:
                normalized.append(part)
        return "/" + "/".join(normalized)
