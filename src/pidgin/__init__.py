"""Pidgin — Dual-layer compression for AI communication.

Semantic compression (AI-to-AI):
    from pidgin import Protocol
    prompt = Protocol().system_prompt("bidirectional")

Schema-aware API compression:
    from pidgin import SchemaCodec
    codec = SchemaCodec.learn(sample_responses)
    compressed = codec.compress(response)
    original = codec.decompress(compressed)

Ratchet cipher (forward secrecy):
    from pidgin import RatchetCipher
    cipher = RatchetCipher(shared_secret=b"key")
    encrypted = cipher.encrypt(data)

Secure channel (compress + encrypt + authenticate):
    from pidgin import SecureChannel
    channel = SecureChannel.create("alice")

LLM-powered encode/decode:
    from pidgin import Codec
    codec = Codec()
    result = await codec.encode("message")
"""

from pidgin._protocol import Protocol
from pidgin._types import CompressedMessage, DecodedMessage
from pidgin.ratchet import RatchetCipher
from pidgin.schema_codec import SchemaCodec

__version__ = "0.1.0"
__all__ = [
    "Protocol", "Codec", "SchemaCodec", "RatchetCipher",
    "SecureChannel", "CompressedMessage", "DecodedMessage",
]


def __getattr__(name: str):
    if name == "Codec":
        from pidgin._codec import Codec
        return Codec
    if name == "SecureChannel":
        from pidgin.channel import SecureChannel
        return SecureChannel
    raise AttributeError(f"module 'pidgin' has no attribute {name!r}")
