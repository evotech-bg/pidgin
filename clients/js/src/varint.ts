/**
 * ZigZag variable-length integer decoding.
 *
 * Wire format (identical to Protocol Buffers):
 *   - Signed integers are mapped to unsigned via ZigZag:
 *       0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, ...
 *   - The unsigned value is then encoded as a base-128 varint:
 *       each byte stores 7 payload bits (LSB first) with the MSB set to 1
 *       if more bytes follow.
 *
 * This decoder reads from a Uint8Array and advances a mutable offset.
 */

/**
 * Mutable position wrapper so callees can advance the read cursor.
 * Using an object avoids allocating a single-element array on every call.
 */
export interface Cursor {
  offset: number;
}

/**
 * Decode a ZigZag-encoded varint from `data` starting at `cursor.offset`.
 *
 * The cursor is advanced past the consumed bytes.
 *
 * Returns a standard JS `number`.  This is safe for integers up to 2^53 - 1
 * which covers all practical API payloads.  (The Python encoder uses 64-bit
 * signed integers with `>> 63`, but JS bitwise ops are 32-bit; we use
 * arithmetic shifts to handle the full safe-integer range.)
 */
export function decodeZigZagVarint(data: Uint8Array, cursor: Cursor): number {
  let result = 0;
  let shift = 0;

  // eslint-disable-next-line no-constant-condition
  while (true) {
    if (cursor.offset >= data.length) {
      throw new RangeError("varint truncated");
    }
    if (shift > 63) {
      throw new RangeError("varint longer than 10 bytes");
    }
    const byte = data[cursor.offset]!;
    cursor.offset += 1;
    // Accumulate 7 payload bits.  Use multiplication instead of `<<` for
    // shifts >= 32 so we stay in double-precision territory.
    result += (byte & 0x7f) * (shift < 32 ? (1 << shift) : 2 ** shift);
    if ((byte & 0x80) === 0) {
      break;
    }
    shift += 7;
  }

  // Undo ZigZag: (result >>> 1) ^ -(result & 1)
  // For values that fit in 32 bits we use the fast bit path, otherwise
  // we fall back to arithmetic.
  if (result <= 0xffffffff) {
    // Unsigned right-shift works within u32 range.
    return (result >>> 1) ^ -(result & 1);
  }
  const sign = result & 1;
  const magnitude = Math.floor(result / 2);
  return sign ? -magnitude - 1 : magnitude;
}
