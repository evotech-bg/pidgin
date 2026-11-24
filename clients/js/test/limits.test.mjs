// Decoding untrusted payloads: output size and item count are capped, reads
// past the end and over-long varints are errors. Data: test/limit_data.json
// (made with the Python encoder). Run: npm test.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { brotliDecompressSync } from "node:zlib";
import * as codec from "../dist/esm/index.js";

const { PidginDecoder } = codec;
const here = dirname(fileURLToPath(import.meta.url));
const d = JSON.parse(readFileSync(join(here, "limit_data.json"), "utf8"));
const bin = (k) => new Uint8Array(Buffer.from(d[k], "base64"));
// the way the README tells Node users to pass brotli: with the limit
const boundedBrotli = (b, max) =>
  new Uint8Array(brotliDecompressSync(b, max === undefined ? {} : { maxOutputLength: max }));

function grewMiB(fn) {
  if (global.gc) global.gc();
  const before = process.memoryUsage().arrayBuffers + process.memoryUsage().heapUsed;
  return fn().then(
    (v) => ({ v, grew: (process.memoryUsage().arrayBuffers + process.memoryUsage().heapUsed - before) / 2 ** 20 }),
    (e) => ({ e, grew: (process.memoryUsage().arrayBuffers + process.memoryUsage().heapUsed - before) / 2 ** 20 }),
  );
}

test("brotli bomb is refused and memory stays small", async () => {
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  const { e, grew } = await grewMiB(() => dec.decode(bin("brotli_bomb")));
  assert.ok(e, "decode must fail");
  assert.ok(grew < 48, `grew ${grew.toFixed(0)} MiB`);
});

test("too many items is refused", async () => {
  const dec = PidginDecoder.fromJSON(d.nested_profile, boundedBrotli);
  const { e, grew } = await grewMiB(() => dec.decode(bin("records_bomb")));
  assert.ok(e, "decode must fail");
  assert.ok(grew < 300, `grew ${grew.toFixed(0)} MiB`);
});

test("limits can be set per decoder", async () => {
  const dec = new PidginDecoder(JSON.parse(d.profile), boundedBrotli, { maxItems: 1000 });
  await assert.rejects(() => dec.decode(bin("json_items")));
  const roomy = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  assert.equal((await roomy.decode(bin("json_items"))).x.length, 20000);
  const tiny = new PidginDecoder(JSON.parse(d.profile), boundedBrotli, { maxOutputBytes: 64 });
  await assert.rejects(() => tiny.decode(bin("ok_payload")));
});

test("truncated payload is an error, not partial data", () => {
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  assert.throws(() => dec.decodeSync(bin("truncated")));
});

test("varint longer than 10 bytes is an error", () => {
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  const p = new Uint8Array(3 + 100000);
  p[0] = 0x00; p[1] = 0x01; p.fill(0x80, 2, 2 + 100000); p[2 + 100000] = 0x00;
  assert.throws(() => dec.decodeSync(p));
});

test("unknown-fields section must be a JSON object", () => {
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  assert.throws(() => dec.decodeSync(bin("fallback_array")));
});

test("ordinary payload decodes as before", async () => {
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  assert.deepEqual(await dec.decode(bin("ok_payload")), d.ok);
  const plain = PidginDecoder.fromJSON(d.profile, (b) => new Uint8Array(brotliDecompressSync(b)));
  assert.deepEqual(await plain.decode(bin("ok_payload")), d.ok);
});

test("decode errors are PidginDecodeError", async () => {
  assert.equal(typeof codec.PidginDecodeError, "function");
  const dec = PidginDecoder.fromJSON(d.profile, boundedBrotli);
  await assert.rejects(() => dec.decode(bin("brotli_bomb")), codec.PidginDecodeError);
});

// Browsers decode brotli through DecompressionStream("br"). Node 22 has no
// "br" there, so the test stands one in, built on zlib's streaming brotli.
test("DecompressionStream path stops at maxOutputBytes", async () => {
  const { createBrotliDecompress } = await import("node:zlib");
  const { Duplex } = await import("node:stream");
  const Native = globalThis.DecompressionStream;
  let native = true;
  try { new Native("br"); } catch { native = false; }
  if (!native) {
    globalThis.DecompressionStream = class {
      constructor(fmt) {
        if (fmt !== "br") return new Native(fmt);
        const { readable, writable } = Duplex.toWeb(createBrotliDecompress());
        this.readable = readable;
        this.writable = writable;
      }
    };
  }
  try {
    const dec = PidginDecoder.fromJSON(d.profile);   // no brotli function: the stream path
    const { e, grew } = await grewMiB(() => dec.decode(bin("brotli_bomb")));
    assert.ok(e instanceof codec.PidginDecodeError, String(e));
    assert.ok(grew < 64, `grew ${grew.toFixed(0)} MiB`);
    assert.deepEqual(await dec.decode(bin("ok_payload")), d.ok);
  } finally {
    globalThis.DecompressionStream = Native;
  }
});
