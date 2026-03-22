#!/usr/bin/env python3
"""Pidgin usage examples — SchemaCodec + RatchetCipher."""

from pidgin import SchemaCodec, RatchetCipher

# === 1. SchemaCodec — learn, compress, decompress ===

samples = [
    {"id": 1, "name": "Alice", "email": "alice@example.com", "role": "admin", "active": True},
    {"id": 2, "name": "Bob", "email": "bob@example.com", "role": "editor", "active": False},
    {"id": 3, "name": "Charlie", "email": "charlie@example.com", "role": "viewer", "active": True},
    {"id": 4, "name": "Diana", "email": "diana@example.com", "role": "admin", "active": True},
    {"id": 5, "name": "Eve", "email": "eve@example.com", "role": "editor", "active": False},
]

codec = SchemaCodec.learn(samples)

# Single object
data = {"id": 99, "name": "Zara", "email": "zara@test.com", "role": "admin", "active": True}
compressed = codec.compress(data)
restored = codec.decompress(compressed)
print(f"Single: {len(str(data))} chars → {len(compressed)} bytes ({len(compressed)/len(str(data)):.0%})")
assert restored == data

# Array
compressed = codec.compress(samples)
restored = codec.decompress(compressed)
print(f"Array:  {len(str(samples))} chars → {len(compressed)} bytes")
assert restored == samples

# Profile sharing
profile_json = codec.profile.to_json()
print(f"Profile: {len(profile_json)} bytes (share with client once)")

# === 2. Schema Evolution ===

new_samples = [
    {"id": 10, "name": "Frank", "email": "f@test.com", "role": "admin", "department": "Eng"},
    {"id": 11, "name": "Grace", "email": "g@test.com", "role": "editor", "department": "Sales"},
]

v2_profile = codec.profile.evolve(new_samples)
v2_codec = SchemaCodec(v2_profile)
print(f"\nEvolution v{codec.profile.version} → v{v2_profile.version}:")
for line in codec.profile.diff(v2_profile):
    print(f"  {line}")

# === 3. RatchetCipher — optional encryption ===

alice = RatchetCipher(b"shared-secret")
bob = RatchetCipher(b"shared-secret")

compressed = codec.compress(samples)
encrypted = alice.encrypt(compressed)
decrypted = bob.decrypt(encrypted)
restored = codec.decompress(decrypted)

print(f"\nEncrypted: {len(compressed)} → {len(encrypted)} bytes (+4 counter)")
print(f"Roundtrip: {'OK' if restored == samples else 'FAIL'}")

# Forward secrecy — same data, different ciphertext
encrypted2 = alice.encrypt(compressed)
print(f"Forward secrecy: {'OK' if encrypted[4:] != encrypted2[4:] else 'FAIL'}")
