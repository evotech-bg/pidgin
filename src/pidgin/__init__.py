"""Pidgin: runtime-learned compression for JSON APIs.

Schema-aware API compression:
    from pidgin import SchemaCodec
    codec = SchemaCodec.learn(sample_responses)
    compressed = codec.compress(response)
    original = codec.decompress(compressed)

Ratchet cipher (needs the crypto extra):
    from pidgin import RatchetCipher
    cipher = RatchetCipher(shared_secret=b"key")
    encrypted = cipher.encrypt(data)

Secure channel (needs the crypto extra):
    from pidgin import SecureChannel
    channel = SecureChannel.create("alice")
"""

from pidgin.ratchet import RatchetCipher
from pidgin.schema_codec import SchemaCodec

__version__ = "0.2.0"
__all__ = ["SchemaCodec", "RatchetCipher", "SecureChannel"]


def __getattr__(name: str):
    if name == "SecureChannel":
        from pidgin.channel import SecureChannel
        return SecureChannel
    raise AttributeError(f"module 'pidgin' has no attribute {name!r}")
