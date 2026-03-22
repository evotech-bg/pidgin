"""Tests for RatchetCipher — position-based instruction cipher."""

import os
import pytest
from pidgin import RatchetCipher


def test_roundtrip():
    alice = RatchetCipher(b"test-key")
    bob = RatchetCipher(b"test-key")
    data = b"Hello, World!"
    assert bob.decrypt(alice.encrypt(data)) == data


def test_forward_secrecy():
    """Same plaintext → different ciphertext each time."""
    alice = RatchetCipher(b"test-key")
    data = b"same data"
    enc1 = alice.encrypt(data)
    enc2 = alice.encrypt(data)
    assert enc1[4:] != enc2[4:]  # skip counter header


def test_multi_message_sync():
    alice = RatchetCipher(b"conv-key")
    bob = RatchetCipher(b"conv-key")
    for i in range(20):
        msg = f"Message {i}".encode()
        assert bob.decrypt(alice.encrypt(msg)) == msg


def test_desync_detection():
    """After one ratchet step, counters diverge → ValueError."""
    alice = RatchetCipher(b"key")
    bob = RatchetCipher(b"key")
    # Advance alice by one message without bob
    alice.encrypt(b"msg1")
    encrypted = alice.encrypt(b"msg2")
    # Bob is still at counter 0, alice sent counter 1
    with pytest.raises(ValueError, match="desync"):
        bob.decrypt(encrypted)


def test_empty_data():
    alice = RatchetCipher(b"key")
    bob = RatchetCipher(b"key")
    assert bob.decrypt(alice.encrypt(b"")) == b""


def test_edge_cases():
    alice = RatchetCipher(b"edge")
    bob = RatchetCipher(b"edge")
    cases = [
        b"\x00",
        b"\xff",
        b"\x00" * 100,
        b"\xff" * 100,
        bytes(range(256)),
        os.urandom(1000),
        "Кирилица 🎉".encode(),
    ]
    for data in cases:
        assert bob.decrypt(alice.encrypt(data)) == data
