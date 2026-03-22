/**
 * PidginDecoder — decodes binary payloads produced by the Python SchemaCodec
 * encoder.
 *
 * Wire format (outermost layer):
 *   byte 0:  brotli flag  (0x00 = raw, 0x01 = brotli-compressed)
 *   rest:    payload bytes (decompress first if flag is 0x01)
 *
 * Payload:
 *   byte 0:  type marker  (0x01 = single object, 0x02 = array of objects)
 *   if array: bytes 1-2 = uint16 BE item count, then N encoded objects
 *   if single: one encoded object follows immediately
 *
 * Object encoding (per field in field_order):
 *   nullable preamble (if field.nullable):
 *     0xFF = null, 0x00 = absent (field omitted), 0x01 = value follows
 *   value encoding depends on dtype (see _decodeValue)
 *   after all known fields: 2-byte BE length of unknown-fields JSON fallback
 *     (0x00 0x00 when there are none)
 */

import type { SchemaProfile, FieldSchema, FieldDtype } from "./types.js";
import { decodeZigZagVarint, type Cursor } from "./varint.js";

// --- Marker constants (must match Python encoder) ---
const MARKER_NULL = 0xff;
const MARKER_ABSENT = 0x00;
// MARKER_PRESENT = 0x01  (any non-null, non-absent byte — we just skip it)

const BROTLI_FLAG = 0x01;
const TYPE_SINGLE = 0x01;
const TYPE_ARRAY = 0x02;

const ENUM_UNKNOWN = 0xfe;

// --- UTF-8 text decoder (shared singleton) ---
const textDecoder = new TextDecoder("utf-8");

// --- Brotli decompression helpers ---

/**
 * Attempt brotli decompression using the web-standard DecompressionStream API
 * (available in modern browsers and Node 18+).  Returns null if the API is
 * unavailable so the caller can try alternative strategies.
 */
async function decompressBrotliStream(
  compressed: Uint8Array,
): Promise<Uint8Array | null> {
  if (typeof DecompressionStream === "undefined") {
    return null;
  }

  try {
    const ds = new DecompressionStream("deflate");
    // DecompressionStream does not support "br" in all environments.
    // We try it — if the constructor throws, we fall back.
    void ds; // just to check it exists
  } catch {
    // "br" not supported in this runtime
  }

  try {
    const ds = new DecompressionStream("br" as CompressionFormat);
    const writer = ds.writable.getWriter();
    const reader = ds.readable.getReader();

    // Pump compressed data in, then close.
    void writer.write(new Uint8Array(compressed) as unknown as BufferSource);
    void writer.close();

    const chunks: Uint8Array[] = [];
    // eslint-disable-next-line no-constant-condition
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value as Uint8Array);
    }

    // Fast path: single chunk (common case)
    if (chunks.length === 1) {
      return chunks[0]!;
    }

    // Merge chunks
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const merged = new Uint8Array(total);
    let off = 0;
    for (const chunk of chunks) {
      merged.set(chunk, off);
      off += chunk.length;
    }
    return merged;
  } catch {
    return null;
  }
}

/** Optional user-provided brotli decompress function (sync or async). */
export type BrotliDecompressFn = (
  data: Uint8Array,
) => Uint8Array | Promise<Uint8Array>;

// ---- Helpers for reading big-endian values from Uint8Array ----

function readUint16BE(data: Uint8Array, offset: number): number {
  return ((data[offset]! << 8) | data[offset + 1]!) >>> 0;
}

function readFloat64BE(data: Uint8Array, offset: number): number {
  // Use a DataView for IEEE 754 double decoding.
  const dv = new DataView(data.buffer, data.byteOffset + offset, 8);
  return dv.getFloat64(0, false /* big-endian */);
}

// ---- Compute nested field_order from a fields map ----

function fieldOrderFor(fields: Record<string, FieldSchema>): string[] {
  return Object.keys(fields).sort((a, b) => {
    const fa = fields[a]!;
    const fb = fields[b]!;
    // Highest frequency first, then alphabetical
    if (fb.freq !== fa.freq) return fb.freq - fa.freq;
    return a < b ? -1 : a > b ? 1 : 0;
  });
}

// =====================================================================
// PidginDecoder
// =====================================================================

export class PidginDecoder {
  private readonly profile: SchemaProfile;
  private readonly brotli: BrotliDecompressFn | null;

  /**
   * @param profile  Parsed SchemaProfile JSON object.
   * @param brotli   Optional brotli decompress function.  If omitted the
   *                 decoder will try the web-standard DecompressionStream API
   *                 and throw if brotli-compressed data is encountered in an
   *                 environment that supports neither.
   */
  constructor(profile: SchemaProfile, brotli?: BrotliDecompressFn) {
    this.profile = profile;
    this.brotli = brotli ?? null;
  }

  /** Convenience: construct from a JSON string. */
  static fromJSON(json: string, brotli?: BrotliDecompressFn): PidginDecoder {
    const profile: SchemaProfile = JSON.parse(json) as SchemaProfile;
    return new PidginDecoder(profile, brotli);
  }

  /**
   * Decode a binary Pidgin payload into one or more plain JS objects.
   *
   * The method is `async` because brotli decompression may require the
   * streaming DecompressionStream API which is async.  If you know the
   * payload is not brotli-compressed you can safely `await` with negligible
   * overhead.
   */
  async decode(
    input: ArrayBuffer | Uint8Array,
  ): Promise<Record<string, unknown> | Record<string, unknown>[]> {
    const raw =
      input instanceof Uint8Array ? input : new Uint8Array(input);

    if (raw.length === 0) {
      throw new Error("PidginDecoder: empty payload");
    }

    const brotliFlag = raw[0]!;
    let payload: Uint8Array;

    if (brotliFlag === BROTLI_FLAG) {
      const compressed = raw.subarray(1);
      payload = await this._decompressBrotli(compressed);
    } else {
      payload = raw.subarray(1);
    }

    return this._decodePayload(payload);
  }

  /**
   * Synchronous decode — use ONLY when you are certain the payload is NOT
   * brotli-compressed (brotli flag byte === 0x00).  Throws if brotli data
   * is encountered.
   */
  decodeSync(
    input: ArrayBuffer | Uint8Array,
  ): Record<string, unknown> | Record<string, unknown>[] {
    const raw =
      input instanceof Uint8Array ? input : new Uint8Array(input);

    if (raw.length === 0) {
      throw new Error("PidginDecoder: empty payload");
    }

    if (raw[0] === BROTLI_FLAG) {
      throw new Error(
        "PidginDecoder.decodeSync: payload is brotli-compressed — use async decode() instead",
      );
    }

    return this._decodePayload(raw.subarray(1));
  }

  // ------------------------------------------------------------------
  // Internal helpers
  // ------------------------------------------------------------------

  private async _decompressBrotli(compressed: Uint8Array): Promise<Uint8Array> {
    // 1. User-provided decompressor
    if (this.brotli) {
      const result = this.brotli(compressed);
      return result instanceof Promise ? await result : result;
    }

    // 2. Web-standard DecompressionStream
    const streamed = await decompressBrotliStream(compressed);
    if (streamed) return streamed;

    throw new Error(
      "PidginDecoder: brotli-compressed payload but no decompressor available. " +
        "Pass a brotli decompressor to the constructor or use an environment " +
        "with DecompressionStream('br') support.",
    );
  }

  private _decodePayload(
    payload: Uint8Array,
  ): Record<string, unknown> | Record<string, unknown>[] {
    const cursor: Cursor = { offset: 0 };
    const marker = payload[cursor.offset]!;
    cursor.offset += 1;

    if (marker === TYPE_ARRAY) {
      const count = readUint16BE(payload, cursor.offset);
      cursor.offset += 2;
      const results: Record<string, unknown>[] = [];
      for (let i = 0; i < count; i++) {
        results.push(
          this._decodeObject(
            payload,
            cursor,
            this.profile.fields,
            this.profile.field_order,
          ),
        );
      }
      return results;
    }

    if (marker === TYPE_SINGLE) {
      return this._decodeObject(
        payload,
        cursor,
        this.profile.fields,
        this.profile.field_order,
      );
    }

    throw new Error(`PidginDecoder: unknown type marker 0x${marker.toString(16).padStart(2, "0")}`);
  }

  private _decodeObject(
    data: Uint8Array,
    cursor: Cursor,
    fields: Record<string, FieldSchema>,
    fieldOrder: string[],
  ): Record<string, unknown> {
    const result: Record<string, unknown> = {};

    for (const key of fieldOrder) {
      const schema = fields[key];
      if (!schema) continue;

      if (schema.nullable) {
        const flag = data[cursor.offset]!;
        cursor.offset += 1;
        if (flag === MARKER_NULL) {
          result[key] = null;
          continue;
        }
        if (flag === MARKER_ABSENT) {
          // Field was not present in the original object — omit from result.
          continue;
        }
        // flag === MARKER_PRESENT (0x01) — fall through to read value
      }

      result[key] = this._decodeValue(data, cursor, schema);
    }

    // Unknown fields JSON fallback
    const fbLen = readUint16BE(data, cursor.offset);
    cursor.offset += 2;
    if (fbLen > 0) {
      const jsonBytes = data.subarray(cursor.offset, cursor.offset + fbLen);
      cursor.offset += fbLen;
      const fallback = JSON.parse(textDecoder.decode(jsonBytes)) as Record<
        string,
        unknown
      >;
      Object.assign(result, fallback);
    }

    return result;
  }

  private _decodeValue(
    data: Uint8Array,
    cursor: Cursor,
    schema: FieldSchema,
  ): unknown {
    const dtype: FieldDtype = schema.dtype;

    switch (dtype) {
      case "bool": {
        const val = data[cursor.offset]!;
        cursor.offset += 1;
        return val !== 0;
      }

      case "int":
        return decodeZigZagVarint(data, cursor);

      case "float": {
        const val = readFloat64BE(data, cursor.offset);
        cursor.offset += 8;
        return val;
      }

      case "str": {
        const len = readUint16BE(data, cursor.offset);
        cursor.offset += 2;
        const val = textDecoder.decode(
          data.subarray(cursor.offset, cursor.offset + len),
        );
        cursor.offset += len;
        return val;
      }

      case "enum": {
        const idx = data[cursor.offset]!;
        cursor.offset += 1;
        if (idx === ENUM_UNKNOWN) {
          // Unknown enum — read string fallback
          const slen = readUint16BE(data, cursor.offset);
          cursor.offset += 2;
          const val = textDecoder.decode(
            data.subarray(cursor.offset, cursor.offset + slen),
          );
          cursor.offset += slen;
          return val;
        }
        if (schema.enum && idx < schema.enum.length) {
          return schema.enum[idx]!;
        }
        // Index out of range (schema mismatch) — return null rather than crash
        return null;
      }

      case "timestamp": {
        const len = data[cursor.offset]!;
        cursor.offset += 1;
        const val = textDecoder.decode(
          data.subarray(cursor.offset, cursor.offset + len),
        );
        cursor.offset += len;
        return val;
      }

      case "object": {
        if (!schema.nested) {
          // No nested schema — fall through to JSON fallback
          return this._decodeJsonFallback(data, cursor);
        }
        const nestedOrder = fieldOrderFor(schema.nested);
        return this._decodeObject(data, cursor, schema.nested, nestedOrder);
      }

      case "array": {
        if (!schema.items) {
          // No items schema — fall through to JSON fallback
          return this._decodeJsonFallback(data, cursor);
        }
        const count = readUint16BE(data, cursor.offset);
        cursor.offset += 2;
        const itemsOrder = fieldOrderFor(schema.items);
        const arr: Record<string, unknown>[] = [];
        for (let i = 0; i < count; i++) {
          arr.push(this._decodeObject(data, cursor, schema.items, itemsOrder));
        }
        return arr;
      }

      default: {
        // Unknown dtype — read as JSON fallback (uint16 BE length + UTF-8 JSON)
        return this._decodeJsonFallback(data, cursor);
      }
    }
  }

  /** Read a uint16-BE-prefixed JSON blob and parse it. */
  private _decodeJsonFallback(data: Uint8Array, cursor: Cursor): unknown {
    const len = readUint16BE(data, cursor.offset);
    cursor.offset += 2;
    const raw = textDecoder.decode(data.subarray(cursor.offset, cursor.offset + len));
    cursor.offset += len;
    return JSON.parse(raw) as unknown;
  }
}
