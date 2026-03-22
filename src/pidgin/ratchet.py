"""
Ratchet Cipher — position-based instruction cipher with forward secrecy.

Each message gets a unique "transform program" — a list of byte-level operations
derived from a shared secret. After each message, the secret ratchets forward,
so each message uses a different cipher.

NOT a replacement for TLS/AES. This is an obfuscation layer that makes
compressed binary data (from SchemaCodec) completely opaque without
both the schema profile AND the current ratchet state.

Architecture:
    SchemaCodec.compress() → RatchetCipher.encrypt() → wire
    wire → RatchetCipher.decrypt() → SchemaCodec.decompress()
"""

from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass
from enum import IntEnum

try:
    from pidgin._fast import ratchet_from_random as _c_ratchet
    from pidgin._fast import expand_key as _c_expand_key
except ImportError:
    _c_ratchet = None
    _c_expand_key = None


class Op(IntEnum):
    """Byte-level operations for the transform program."""
    XOR = 0    # XOR byte at pos with value
    SWAP = 1   # Swap bytes at pos and param
    ROT = 2    # Rotate bits left by param
    ADD = 3    # Modular add param to byte at pos
    INV = 4    # Bitwise invert byte at pos


@dataclass(frozen=True, slots=True)
class Instruction:
    """Single transform instruction: [position][op][param]."""
    pos: int
    op: Op
    param: int


class RatchetCipher:
    """Position-based instruction cipher with ratcheting key.

    Usage:
        # Both sides start with same shared secret
        alice = RatchetCipher(shared_secret=b"my-secret-key-256bit")
        bob = RatchetCipher(shared_secret=b"my-secret-key-256bit")

        # Alice encrypts
        ciphertext = alice.encrypt(plaintext)

        # Bob decrypts (ratchet state stays in sync)
        plaintext = bob.decrypt(ciphertext)

        # Next message uses a different cipher automatically
        ciphertext2 = alice.encrypt(plaintext2)
        plaintext2 = bob.decrypt(ciphertext2)
    """

    def __init__(self, shared_secret: bytes, instructions_per_block: int = 16):
        """Initialize cipher with shared secret.

        Args:
            shared_secret: Initial shared key (any length, hashed internally).
            instructions_per_block: Number of transform ops per 64-byte block.
                More = stronger obfuscation, slightly slower.
        """
        self._state = hashlib.sha256(shared_secret).digest()
        self._counter = 0
        self._ops_per_block = instructions_per_block

    def encrypt(self, data: bytes) -> bytes:
        """Encrypt data using current ratchet state.

        Returns: counter (4 bytes) + encrypted data
        """
        result = bytearray(data)

        if _c_ratchet is not None and len(data) > 0:
            random_bytes = self._get_random_bytes(len(data))
            _c_ratchet(result, random_bytes, self._ops_per_block, 1)
        else:
            program = self._derive_program(len(data))
            for instr in program:
                self._apply(result, instr)

        output = struct.pack(">I", self._counter) + bytes(result)
        self._ratchet(data)
        return output

    def decrypt(self, data: bytes) -> bytes:
        """Decrypt data using current ratchet state.

        Returns: original plaintext bytes
        """
        msg_counter = struct.unpack(">I", data[:4])[0]

        if msg_counter != self._counter:
            raise ValueError(
                f"Ratchet desync: expected counter {self._counter}, got {msg_counter}. "
                f"Keys are out of sync."
            )

        ciphertext = data[4:]
        result = bytearray(ciphertext)

        if _c_ratchet is not None and len(ciphertext) > 0:
            random_bytes = self._get_random_bytes(len(ciphertext))
            _c_ratchet(result, random_bytes, self._ops_per_block, 0)
        else:
            program = self._derive_program(len(ciphertext))
            for instr in reversed(program):
                self._apply_inverse(result, instr)

        plaintext = bytes(result)
        self._ratchet(plaintext)
        return plaintext

    def _get_random_bytes(self, data_len: int) -> bytes:
        """Generate random bytes for instruction derivation (shared with C path)."""
        n_blocks = max(1, (data_len + 63) // 64)
        n_instructions = n_blocks * self._ops_per_block
        prng_seed = hmac.new(self._state, struct.pack(">I", self._counter), hashlib.sha256).digest()
        length = n_instructions * 4
        if _c_expand_key is not None:
            return _c_expand_key(prng_seed, length)
        return self._expand_key(prng_seed, length)

    def _derive_program(self, data_len: int) -> list[Instruction]:
        """Derive transform program from current state."""
        if data_len == 0:
            return []

        n_blocks = max(1, (data_len + 63) // 64)
        n_instructions = n_blocks * self._ops_per_block

        prng_seed = hmac.new(self._state, struct.pack(">I", self._counter), hashlib.sha256).digest()
        random_bytes = self._expand_key(prng_seed, n_instructions * 4)

        instructions = []
        for i in range(n_instructions):
            offset = i * 4
            pos = random_bytes[offset] % data_len
            op = Op(random_bytes[offset + 1] % len(Op))
            param_byte = random_bytes[offset + 2]

            if op == Op.SWAP:
                param = random_bytes[offset + 3] % data_len
            elif op == Op.ROT:
                param = param_byte % 8
            elif op == Op.XOR:
                param = param_byte
            elif op == Op.ADD:
                param = param_byte
            elif op == Op.INV:
                param = 0
            else:
                param = param_byte

            instructions.append(Instruction(pos=pos, op=op, param=param))

        return instructions

    def _ratchet(self, message: bytes):
        """Advance state using HMAC of current state + message hash."""
        msg_hash = hashlib.sha256(message).digest()
        self._state = hmac.new(self._state, msg_hash, hashlib.sha256).digest()
        self._counter += 1

    @staticmethod
    def _expand_key(seed: bytes, length: int) -> bytes:
        """Expand seed into arbitrary-length pseudorandom bytes (HKDF-like)."""
        blocks = []
        block = b""
        i = 0
        while len(b"".join(blocks)) < length:
            block = hmac.new(seed, block + struct.pack(">I", i), hashlib.sha256).digest()
            blocks.append(block)
            i += 1
        return b"".join(blocks)[:length]

    @staticmethod
    def _apply(data: bytearray, instr: Instruction):
        """Apply a single instruction (forward direction)."""
        if instr.pos >= len(data):
            return

        if instr.op == Op.XOR:
            data[instr.pos] ^= instr.param
        elif instr.op == Op.SWAP:
            if instr.param < len(data):
                data[instr.pos], data[instr.param] = data[instr.param], data[instr.pos]
        elif instr.op == Op.ROT:
            byte = data[instr.pos]
            data[instr.pos] = ((byte << instr.param) | (byte >> (8 - instr.param))) & 0xFF
        elif instr.op == Op.ADD:
            data[instr.pos] = (data[instr.pos] + instr.param) & 0xFF
        elif instr.op == Op.INV:
            data[instr.pos] ^= 0xFF

    @staticmethod
    def _apply_inverse(data: bytearray, instr: Instruction):
        """Apply inverse of a single instruction (for decryption)."""
        if instr.pos >= len(data):
            return

        if instr.op == Op.XOR:
            data[instr.pos] ^= instr.param
        elif instr.op == Op.SWAP:
            if instr.param < len(data):
                data[instr.pos], data[instr.param] = data[instr.param], data[instr.pos]
        elif instr.op == Op.ROT:
            byte = data[instr.pos]
            data[instr.pos] = ((byte >> instr.param) | (byte << (8 - instr.param))) & 0xFF
        elif instr.op == Op.ADD:
            data[instr.pos] = (data[instr.pos] - instr.param) & 0xFF
        elif instr.op == Op.INV:
            data[instr.pos] ^= 0xFF
