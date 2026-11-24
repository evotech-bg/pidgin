"""
Pidgin compression engine — shared core for all middleware integrations.

Handles auto-learning, profile management, content negotiation,
schema evolution, and transparent compression.

Usage from middleware:
    engine = PidginEngine()
    engine.observe(endpoint, response_data)   # auto-learn phase
    compressed = engine.compress(endpoint, response_data)  # compress phase
    profile_json = engine.get_profile(endpoint)  # serve to client
"""

from __future__ import annotations

import hashlib
import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any

from pidgin.schema_codec import SchemaCodec, SchemaProfile


@dataclass
class EndpointState:
    """Per-endpoint compression state."""
    samples: list[dict] = field(default_factory=list)
    codec: SchemaCodec | None = None
    profile_etag: str | None = None
    created_at: float = 0.0
    request_count: int = 0
    last_schema_hash: str | None = None


class PidginEngine:
    """Auto-learning compression engine.

    Lifecycle per endpoint:
      1. LEARNING: Collects first `learn_samples` responses
      2. READY: Profile built, compressing responses
      3. EVOLVING: Detects schema drift, auto-evolves profile

    Thread-safe via per-endpoint locks.
    """

    def __init__(
        self,
        learn_samples: int = 30,
        evolve_interval: int = 500,
        profile_prefix: str = "/.well-known/pidgin",
    ):
        self.learn_samples = learn_samples
        self.evolve_interval = evolve_interval
        self.profile_prefix = profile_prefix

        self._endpoints: dict[str, EndpointState] = {}
        self._locks: dict[str, threading.Lock] = {}
        self._global_lock = threading.Lock()

    def _get_state(self, endpoint: str) -> tuple[EndpointState, threading.Lock]:
        if endpoint not in self._endpoints:
            with self._global_lock:
                if endpoint not in self._endpoints:
                    self._endpoints[endpoint] = EndpointState(created_at=time.time())
                    self._locks[endpoint] = threading.Lock()
        return self._endpoints[endpoint], self._locks[endpoint]

    def is_ready(self, endpoint: str) -> bool:
        """Check if endpoint has a compiled profile ready for compression."""
        state, _ = self._get_state(endpoint)
        return state.codec is not None

    def observe(self, endpoint: str, data: Any) -> None:
        """Feed a response to the learning engine.

        During LEARNING phase: collects samples.
        During READY phase: periodically checks for schema drift.
        """
        records = self._extract_records(data)
        if not records:
            return

        state, lock = self._get_state(endpoint)

        with lock:
            state.request_count += 1

            if state.codec is None:
                # LEARNING phase
                state.samples.extend(records[:10])  # cap per-request
                if len(state.samples) >= self.learn_samples:
                    self._build_profile(state)
            elif state.request_count % self.evolve_interval == 0:
                # EVOLVING check — sample new data, compare schema
                schema_hash = self._hash_keys(records[:5])
                if schema_hash != state.last_schema_hash:
                    self._evolve_profile(state, records[:20])

    def compress(self, endpoint: str, data: Any) -> bytes | None:
        """Compress response data. Returns None if not ready."""
        state, _ = self._get_state(endpoint)
        if state.codec is None:
            return None

        # The profile is learned from the records inside the response. A
        # pagination wrapper such as {"data": [...], "total": 10} has a
        # different shape, so encoding it with that profile would decode to
        # the wrong object. Send those responses as plain JSON instead.
        if not self._is_bare_records(data):
            return None

        try:
            return state.codec.compress(data)
        except Exception:
            return None

    def get_profile(self, endpoint: str) -> str | None:
        """Get profile JSON for client. Returns None if not ready."""
        state, _ = self._get_state(endpoint)
        if state.codec is None:
            return None
        return state.codec.profile.to_json()

    def get_profile_etag(self, endpoint: str) -> str | None:
        """Get ETag for profile caching."""
        state, _ = self._get_state(endpoint)
        return state.profile_etag

    def get_profile_endpoint(self, endpoint: str) -> str:
        """Get the well-known URL for this endpoint's profile."""
        safe = endpoint.strip("/").replace("/", "-") or "root"
        return f"{self.profile_prefix}/{safe}"

    def list_endpoints(self) -> dict[str, dict]:
        """List all known endpoints and their status."""
        result = {}
        for ep, state in self._endpoints.items():
            result[ep] = {
                "status": "ready" if state.codec else "learning",
                "samples": len(state.samples),
                "requests": state.request_count,
                "version": state.codec.profile.version if state.codec else 0,
                "etag": state.profile_etag,
            }
        return result

    # --- Internal ---

    def _build_profile(self, state: EndpointState) -> None:
        state.codec = SchemaCodec.learn(state.samples)
        state.profile_etag = self._make_etag(state.codec.profile)
        state.last_schema_hash = self._hash_keys(state.samples[:5])
        state.samples.clear()  # free memory

    def _evolve_profile(self, state: EndpointState, new_samples: list[dict]) -> None:
        if state.codec is None:
            return
        try:
            new_profile = state.codec.profile.evolve(new_samples)
            state.codec = SchemaCodec(new_profile)
            state.profile_etag = self._make_etag(new_profile)
            state.last_schema_hash = self._hash_keys(new_samples)
        except Exception:
            pass  # evolution failed, keep current profile

    @staticmethod
    def _extract_records(data: Any) -> list[dict]:
        """Extract list of dicts from various response shapes."""
        if isinstance(data, list):
            return [r for r in data if isinstance(r, dict)]
        if isinstance(data, dict):
            # Common pagination wrappers
            for key in ("data", "results", "items", "records"):
                if key in data and isinstance(data[key], list):
                    return [r for r in data[key] if isinstance(r, dict)]
            return [data]
        return []

    @classmethod
    def _is_bare_records(cls, data: Any) -> bool:
        """True when data is a record or a list of records, not a wrapper."""
        if isinstance(data, list):
            return bool(data) and all(isinstance(r, dict) for r in data)
        if isinstance(data, dict):
            return cls._extract_records(data) == [data]
        return False

    @staticmethod
    def _hash_keys(records: list[dict]) -> str:
        """Hash the key structure of records for drift detection."""
        keys = set()
        for r in records:
            keys.update(r.keys())
        return hashlib.md5(json.dumps(sorted(keys)).encode()).hexdigest()[:12]

    @staticmethod
    def _make_etag(profile: SchemaProfile) -> str:
        h = hashlib.md5(profile.to_json().encode()).hexdigest()[:12]
        return f'"pidgin-v{profile.version}-{h}"'
