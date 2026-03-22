"""
SecureChannel — encrypted compressed chat with forward secrecy.

Full stack:
    1. SchemaCodec — strip keys, compact values (schema learned from chat message format)
    2. RatchetCipher — position-based instruction cipher, ratcheting key
    3. X25519 — Diffie-Hellman key exchange (no pre-shared secret needed)
    4. HMAC-SHA256 — authenticate ciphertext (tamper detection)

Usage:
    alice = SecureChannel.create("alice")
    bob = SecureChannel.create("bob")

    # Key exchange (once)
    alice.handshake(bob.public_key)
    bob.handshake(alice.public_key)

    # Send messages
    envelope = alice.send("Hello Bob!")
    message = bob.receive(envelope)
"""

from __future__ import annotations

import hashlib
import hmac
import json
import struct
import time
from dataclasses import dataclass

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from .ratchet import RatchetCipher
from .schema_codec import SchemaCodec


# === Chat message schema (pre-defined, both sides know it) ===

_CHAT_SAMPLES = [
    {"id": "msg_001", "sender": "alice", "text": "Hello!", "ts": "2026-03-14T10:00:00Z", "type": "text"},
    {"id": "msg_002", "sender": "bob", "text": "Hi there", "ts": "2026-03-14T10:00:01Z", "type": "text"},
    {"id": "msg_003", "sender": "alice", "text": "How are you?", "ts": "2026-03-14T10:00:02Z", "type": "text"},
    {"id": "msg_004", "sender": "bob", "text": "", "ts": "2026-03-14T10:00:03Z", "type": "typing"},
    {"id": "msg_005", "sender": "alice", "text": "Sent a file", "ts": "2026-03-14T10:00:04Z", "type": "file"},
    {"id": "msg_006", "sender": "bob", "text": "OK", "ts": "2026-03-14T10:00:05Z", "type": "text"},
    {"id": "msg_007", "sender": "alice", "text": "See you!", "ts": "2026-03-14T10:00:06Z", "type": "text"},
    {"id": "msg_008", "sender": "bob", "text": "Bye", "ts": "2026-03-14T10:00:07Z", "type": "text"},
]

_CHAT_CODEC = SchemaCodec.learn(_CHAT_SAMPLES)


# === Envelope format ===
# [version:1][hmac:32][counter:4][ciphertext:N]

_VERSION = 0x01
_HMAC_LEN = 32


@dataclass
class Envelope:
    """Wire format for a secure message."""
    raw: bytes
    size: int
    original_json_size: int

    @property
    def ratio(self) -> float:
        return self.size / self.original_json_size if self.original_json_size else 0


class SecureChannel:
    """Encrypted compressed chat channel with forward secrecy.

    Properties:
        - Compressed: SchemaCodec strips keys, compacts values
        - Encrypted: RatchetCipher with per-message ratcheting
        - Authenticated: HMAC-SHA256 on ciphertext
        - Key exchange: X25519 Diffie-Hellman (no pre-shared secret)
        - Forward secrecy: compromising current key doesn't reveal past messages
    """

    def __init__(self, name: str = "anonymous"):
        self.name = name
        self._private_key = X25519PrivateKey.generate()
        self._codec = _CHAT_CODEC
        self._cipher: RatchetCipher | None = None
        self._hmac_key: bytes | None = None
        self._established = False

    @classmethod
    def create(cls, name: str = "anonymous") -> "SecureChannel":
        """Create a new secure channel."""
        return cls(name)

    @property
    def public_key(self) -> bytes:
        """Get public key bytes for key exchange."""
        return self._private_key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)

    def handshake(self, peer_public_key: bytes):
        """Complete key exchange with peer's public key."""
        peer_key = X25519PublicKey.from_public_bytes(peer_public_key)
        shared_secret = self._private_key.exchange(peer_key)

        cipher_key = hashlib.sha256(shared_secret + b"cipher").digest()
        self._hmac_key = hashlib.sha256(shared_secret + b"hmac").digest()

        self._cipher = RatchetCipher(cipher_key)
        self._established = True

    def send(self, text: str, msg_type: str = "text") -> Envelope:
        """Encrypt and compress a chat message."""
        if not self._established:
            raise RuntimeError("Handshake not completed. Call handshake(peer_public_key) first.")

        msg = {
            "id": f"msg_{int(time.time()*1000)}",
            "sender": self.name,
            "text": text,
            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "type": msg_type,
        }
        json_bytes = json.dumps(msg, separators=(",", ":")).encode()

        compressed = self._codec.compress(msg)
        encrypted = self._cipher.encrypt(compressed)
        mac = hmac.new(self._hmac_key, encrypted, hashlib.sha256).digest()
        envelope_bytes = struct.pack("B", _VERSION) + mac + encrypted

        return Envelope(raw=envelope_bytes, size=len(envelope_bytes), original_json_size=len(json_bytes))

    def receive(self, envelope: Envelope) -> dict:
        """Decrypt and decompress a received message."""
        if not self._established:
            raise RuntimeError("Handshake not completed.")

        data = envelope.raw
        pos = 0

        version = data[pos]; pos += 1
        if version != _VERSION:
            raise ValueError(f"Unknown envelope version: {version}")

        received_mac = data[pos:pos + _HMAC_LEN]; pos += _HMAC_LEN
        encrypted = data[pos:]

        expected_mac = hmac.new(self._hmac_key, encrypted, hashlib.sha256).digest()
        if not hmac.compare_digest(received_mac, expected_mac):
            raise ValueError("HMAC verification failed — message was tampered with!")

        compressed = self._cipher.decrypt(encrypted)
        return self._codec.decompress(compressed)

    def send_raw(self, msg: dict) -> Envelope:
        """Send an arbitrary dict (must match chat schema)."""
        if not self._established:
            raise RuntimeError("Handshake not completed.")

        json_bytes = json.dumps(msg, separators=(",", ":")).encode()
        compressed = self._codec.compress(msg)
        encrypted = self._cipher.encrypt(compressed)
        mac = hmac.new(self._hmac_key, encrypted, hashlib.sha256).digest()
        envelope_bytes = struct.pack("B", _VERSION) + mac + encrypted

        return Envelope(raw=envelope_bytes, size=len(envelope_bytes), original_json_size=len(json_bytes))
