"""
Pidgin middleware for Django.

Usage:
    # settings.py
    MIDDLEWARE = [
        "pidgin.contrib.django.PidginMiddleware",
        # ... other middleware
    ]

    # Optional config in settings.py:
    PIDGIN_LEARN_SAMPLES = 30
    PIDGIN_EVOLVE_INTERVAL = 500
    PIDGIN_EXCLUDE_PATHS = ["/admin", "/static"]
    PIDGIN_MIN_SIZE = 256
"""

from __future__ import annotations

import json

from ._engine import PidginEngine

# Singleton engine — shared across all requests
_engine: PidginEngine | None = None


def _get_engine() -> PidginEngine:
    global _engine
    if _engine is None:
        try:
            from django.conf import settings
            _engine = PidginEngine(
                learn_samples=getattr(settings, "PIDGIN_LEARN_SAMPLES", 30),
                evolve_interval=getattr(settings, "PIDGIN_EVOLVE_INTERVAL", 500),
            )
        except ImportError:
            _engine = PidginEngine()
    return _engine


class PidginMiddleware:
    """Drop-in compression middleware for Django.

    Auto-learns response schemas, negotiates encoding,
    and transparently compresses JSON responses.
    """

    def __init__(self, get_response):
        self.get_response = get_response
        self.engine = _get_engine()

        try:
            from django.conf import settings
            self.exclude_paths = getattr(settings, "PIDGIN_EXCLUDE_PATHS", ["/admin", "/static"])
            self.min_size = getattr(settings, "PIDGIN_MIN_SIZE", 256)
        except ImportError:
            self.exclude_paths = ["/admin", "/static"]
            self.min_size = 256

    def __call__(self, request):
        path = request.path

        # Serve profile requests
        if path.startswith(self.engine.profile_prefix):
            return self._serve_profile(request, path)

        # Skip excluded paths
        if any(path.startswith(p) for p in self.exclude_paths):
            return self.get_response(request)

        response = self.get_response(request)

        # Only compress JSON responses
        content_type = response.get("Content-Type", "")
        if "application/json" not in content_type:
            return response

        body = response.content
        if len(body) < self.min_size:
            return response

        try:
            data = json.loads(body)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return response

        endpoint = self._normalize_endpoint(path)
        self.engine.observe(endpoint, data)

        accept = request.META.get("HTTP_ACCEPT_ENCODING", "")
        if "pidgin" not in accept or not self.engine.is_ready(endpoint):
            return response

        compressed = self.engine.compress(endpoint, data)
        if compressed is None or len(compressed) >= len(body):
            return response

        response.content = compressed
        response["Content-Encoding"] = "pidgin"
        response["Content-Type"] = "application/octet-stream"
        response["X-Pidgin-Profile"] = self.engine.get_profile_endpoint(endpoint)
        response["X-Pidgin-Original-Size"] = str(len(body))
        etag = self.engine.get_profile_etag(endpoint)
        if etag:
            response["X-Pidgin-ETag"] = etag

        return response

    def _serve_profile(self, request, path):
        from django.http import JsonResponse, HttpResponse

        suffix = path[len(self.engine.profile_prefix):].strip("/")
        if not suffix:
            return JsonResponse(self.engine.list_endpoints())

        endpoint = "/" + suffix.replace("-", "/")
        profile_json = self.engine.get_profile(endpoint)
        if profile_json is None:
            return JsonResponse({"error": "profile not ready", "status": "learning"}, status=404)

        etag = self.engine.get_profile_etag(endpoint)
        client_etag = request.META.get("HTTP_IF_NONE_MATCH")
        if client_etag and client_etag == etag:
            return HttpResponse(status=304)

        resp = HttpResponse(profile_json, content_type="application/json")
        if etag:
            resp["ETag"] = etag
        resp["Cache-Control"] = "public, max-age=86400"
        return resp

    @staticmethod
    def _normalize_endpoint(path: str) -> str:
        parts = path.strip("/").split("/")
        normalized = []
        for part in parts:
            if part.isdigit() or (len(part) > 8 and not part.isalpha()):
                normalized.append(":id")
            else:
                normalized.append(part)
        return "/" + "/".join(normalized)
