// Decodes the Python-encoded fixtures in benchmarks/js_fixtures and checks
// that the result equals the original JSON. Run: npm test (after npm run build).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { brotliDecompressSync } from "node:zlib";
import { PidginDecoder } from "../dist/esm/index.js";

const here = dirname(fileURLToPath(import.meta.url));
const fx = join(here, "..", "..", "..", "benchmarks", "js_fixtures");
const brotli = (b) => new Uint8Array(brotliDecompressSync(b));

for (const name of ["users_1k", "orders_500", "events_5k"]) {
  const original = JSON.parse(readFileSync(join(fx, `${name}.json`), "utf8"));
  const profile = readFileSync(join(fx, `${name}.profile.json`), "utf8");
  const dec = PidginDecoder.fromJSON(profile, brotli);

  test(`${name}: raw payload decodes to the original JSON`, () => {
    const out = dec.decodeSync(readFileSync(join(fx, `${name}.pidgin.raw.bin`)));
    assert.deepEqual(out, original);
  });

  test(`${name}: brotli payload decodes to the original JSON`, async () => {
    const out = await dec.decode(readFileSync(join(fx, `${name}.pidgin.br.bin`)));
    assert.deepEqual(out, original);
  });
}
