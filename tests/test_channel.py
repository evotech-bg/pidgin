"""Tests for SecureChannel — encrypted compressed chat."""

import pytest

pytest.importorskip("cryptography", reason="SecureChannel requires cryptography package")

from pidgin import SecureChannel
from pidgin.channel import Envelope


@pytest.fixture
def alice_bob():
    alice = SecureChannel.create("alice")
    bob = SecureChannel.create("bob")
    alice.handshake(bob.public_key)
    bob.handshake(alice.public_key)
    return alice, bob


def test_conversation(alice_bob):
    alice, bob = alice_bob
    envelope = alice.send("Hello Bob!")
    msg = bob.receive(envelope)
    assert msg["text"] == "Hello Bob!"
    assert msg["sender"] == "alice"


def test_bidirectional(alice_bob):
    alice, bob = alice_bob
    msg1 = bob.receive(alice.send("From Alice"))
    msg2 = alice.receive(bob.send("From Bob"))
    assert msg1["text"] == "From Alice"
    assert msg2["text"] == "From Bob"


def test_tamper_detection(alice_bob):
    alice, bob = alice_bob
    envelope = alice.send("Tamper me")
    tampered_raw = bytearray(envelope.raw)
    tampered_raw[-1] ^= 0xFF
    tampered = Envelope(raw=bytes(tampered_raw), size=len(tampered_raw), original_json_size=envelope.original_json_size)
    with pytest.raises(ValueError, match="tampered"):
        bob.receive(tampered)


def test_wrong_recipient(alice_bob):
    alice, _ = alice_bob
    eve = SecureChannel.create("eve")
    charlie = SecureChannel.create("charlie")
    eve.handshake(charlie.public_key)
    charlie.handshake(eve.public_key)

    envelope = alice.send("Secret message")
    with pytest.raises((ValueError, Exception)):
        eve.receive(envelope)


def test_unicode_messages(alice_bob):
    alice, bob = alice_bob
    messages = ["Кирилица", "田中太郎", "🎉🚀💻", "", "x" * 3000]
    for text in messages:
        msg = bob.receive(alice.send(text))
        assert msg["text"] == text


def test_100_messages():
    a = SecureChannel.create("alice")
    b = SecureChannel.create("bob")
    a.handshake(b.public_key)
    b.handshake(a.public_key)

    for i in range(100):
        sender, receiver = (a, b) if i % 2 == 0 else (b, a)
        text = f"Message #{i}"
        msg = receiver.receive(sender.send(text))
        assert msg["text"] == text
