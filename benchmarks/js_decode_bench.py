#!/usr/bin/env python3
"""Generate test fixtures for the JavaScript decoder benchmark.

Produces three pairs of (profile.json, payload.bin) — one per dataset
(Users 1k, Orders 500, Events 5k) — alongside the equivalent JSON and
brotli-compressed JSON for comparison. The Node.js benchmark in
`js_decode_bench.mjs` reads these files and times client-side decoding.
"""
import json
import sys
from pathlib import Path

import brotli

sys.path.insert(0, str(Path(__file__).parent))

from realistic_api import gen_users, gen_orders, gen_events
from pidgin import SchemaCodec


def main():
    out_dir = Path(__file__).parent / "js_fixtures"
    out_dir.mkdir(exist_ok=True)

    datasets = [
        ("users_1k", gen_users(1000)),
        ("orders_500", gen_orders(500)),
        ("events_5k", gen_events(5000)),
    ]

    for name, data in datasets:
        codec_brotli = SchemaCodec.learn(data[:30], use_brotli=True)
        codec_raw = SchemaCodec.learn(data[:30], use_brotli=False)
        profile_json = codec_brotli.profile.to_json()
        pidgin_brotli = codec_brotli.compress(data)
        pidgin_raw = codec_raw.compress(data)
        json_bytes = json.dumps(data, separators=(",", ":")).encode()
        json_brotli = brotli.compress(json_bytes, quality=6)

        (out_dir / f"{name}.profile.json").write_text(profile_json)
        (out_dir / f"{name}.pidgin.br.bin").write_bytes(pidgin_brotli)
        (out_dir / f"{name}.pidgin.raw.bin").write_bytes(pidgin_raw)
        (out_dir / f"{name}.json").write_bytes(json_bytes)
        (out_dir / f"{name}.json.br").write_bytes(json_brotli)

        print(f"{name}: records={len(data)}  json={len(json_bytes):,}B  "
              f"json.br={len(json_brotli):,}B  pidgin.br={len(pidgin_brotli):,}B  "
              f"pidgin.raw={len(pidgin_raw):,}B  profile={len(profile_json):,}B")

    print(f"\nFixtures written to {out_dir}/")
    print("Run Node benchmark:  node benchmarks/js_decode_bench.mjs")


if __name__ == "__main__":
    main()
