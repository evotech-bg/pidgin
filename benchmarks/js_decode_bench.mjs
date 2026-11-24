// Node.js client-side decode benchmark for Pidgin.
//
// Compares decode wall-clock times for:
//   - JSON.parse on raw JSON (lower bound)
//   - JSON.parse after Node's zlib.brotliDecompress on JSON (realistic deploy)
//   - Pidgin decodeSync on a raw (non-brotli) payload
//   - Pidgin async decode on a brotli-compressed payload
//
// Reports mean ± std over N=30 iterations after a warmup, matching the
// Python-side methodology in extended_api.py.

import { readFileSync } from "node:fs";
import { brotliDecompressSync } from "node:zlib";
import { performance } from "node:perf_hooks";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { PidginDecoder } from "../clients/js/dist/esm/decoder.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = join(__dirname, "js_fixtures");

const datasets = ["users_1k", "orders_500", "events_5k"];

function timeRuns(fn, n = 30, warmup = 3) {
  for (let i = 0; i < warmup; i++) fn();
  const samples = [];
  for (let i = 0; i < n; i++) {
    const t0 = performance.now();
    fn();
    samples.push(performance.now() - t0);
  }
  return summarize(samples);
}

async function timeRunsAsync(fn, n = 30, warmup = 3) {
  for (let i = 0; i < warmup; i++) await fn();
  const samples = [];
  for (let i = 0; i < n; i++) {
    const t0 = performance.now();
    await fn();
    samples.push(performance.now() - t0);
  }
  return summarize(samples);
}

function summarize(samples) {
  const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
  const variance = samples.reduce((a, b) => a + (b - mean) ** 2, 0) / samples.length;
  const std = Math.sqrt(variance);
  const sorted = [...samples].sort((a, b) => a - b);
  const median = sorted[Math.floor(samples.length / 2)];
  return { mean, std, median };
}

function fmt(t) {
  if (Number.isNaN(t.mean)) return "—";
  return `${t.mean.toFixed(2)}±${t.std.toFixed(2)}ms`;
}

function brotliDecompress(bytes) {
  return Promise.resolve(new Uint8Array(brotliDecompressSync(bytes)));
}

(async () => {
  console.log("=".repeat(96));
  console.log("  PIDGIN — JavaScript client-side decode benchmark (Node.js)");
  console.log("=".repeat(96));
  console.log(`  Node ${process.version}, platform ${process.platform}/${process.arch}\n`);

  console.log(
    `  ${"Dataset".padEnd(14)} ${"JSON.parse".padStart(18)} ${"brotli+JSON".padStart(18)} ${"Pidgin (raw)".padStart(18)} ${"Pidgin+brotli".padStart(18)}`,
  );
  console.log("  " + "-".repeat(96));

  for (const name of datasets) {
    const profileJson = readFileSync(join(fixturesDir, `${name}.profile.json`), "utf8");
    const pidginBrBytes = readFileSync(join(fixturesDir, `${name}.pidgin.br.bin`));
    const pidginRawBytes = readFileSync(join(fixturesDir, `${name}.pidgin.raw.bin`));
    const jsonBytes = readFileSync(join(fixturesDir, `${name}.json`));
    const jsonBrBytes = readFileSync(join(fixturesDir, `${name}.json.br`));

    const profile = JSON.parse(profileJson);
    const decoder = new PidginDecoder(profile, brotliDecompress);

    const tJson = timeRuns(() => JSON.parse(jsonBytes.toString("utf8")));
    const tBrJson = timeRuns(() =>
      JSON.parse(brotliDecompressSync(jsonBrBytes).toString("utf8")),
    );
    const tPidginRaw = timeRuns(() => decoder.decodeSync(pidginRawBytes));
    const tPidginBr = await timeRunsAsync(() => decoder.decode(pidginBrBytes));

    console.log(
      `  ${name.padEnd(14)} ${fmt(tJson).padStart(18)} ${fmt(tBrJson).padStart(18)} ${fmt(tPidginRaw).padStart(18)} ${fmt(tPidginBr).padStart(18)}`,
    );
  }

  console.log("\n  Notes:");
  console.log("  - JSON.parse: parses raw JSON bytes (no decompression). Lower bound.");
  console.log("  - brotli+JSON: Node's zlib.brotliDecompressSync on JSON, then JSON.parse.");
  console.log("    This is the realistic decode path for an API serving brotli-compressed JSON.");
  console.log("  - Pidgin (raw): decodeSync on a non-brotli Pidgin payload.");
  console.log("  - Pidgin+brotli: async decode on a brotli-flagged Pidgin payload.");
})();
