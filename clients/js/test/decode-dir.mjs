// Decode every <name>.bin in a directory with <name>.profile.json and write
// <name>.decoded.json next to it. Used by tests/test_js_interop.py so that
// Python checks what the JS decoder produced.
//
//   node clients/js/test/decode-dir.mjs <dir> [--no-brotli-fn]
import { readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { brotliDecompressSync } from "node:zlib";
import { PidginDecoder } from "../dist/esm/index.js";

const dir = process.argv[2];
const useFn = !process.argv.includes("--no-brotli-fn");
const brotli = useFn ? (b) => new Uint8Array(brotliDecompressSync(b)) : undefined;

for (const f of readdirSync(dir).filter((n) => n.endsWith(".bin"))) {
  const name = f.slice(0, -4);
  const profile = readFileSync(join(dir, `${name}.profile.json`), "utf8");
  const dec = PidginDecoder.fromJSON(profile, brotli);
  let out;
  try {
    out = { ok: true, value: await dec.decode(readFileSync(join(dir, f))) };
  } catch (e) {
    out = { ok: false, error: String(e && e.message ? e.message : e) };
  }
  writeFileSync(join(dir, `${name}.decoded.json`), JSON.stringify(out));
}
