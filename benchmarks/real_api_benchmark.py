#!/usr/bin/env python3
"""
Benchmark Pidgin against real public API responses.

Tests against actual JSON from:
  1. GitHub API (repos, users, events)
  2. JSONPlaceholder (users, posts, comments, todos)
  3. Open-Meteo (weather forecast)

No API keys needed — all public endpoints.
"""

import gzip
import json
import time
import urllib.request

import brotli
import zstandard

from pidgin import SchemaCodec


def fetch_json(url: str) -> list[dict] | dict:
    """Fetch JSON from a public API."""
    req = urllib.request.Request(url, headers={"User-Agent": "Pidgin-Benchmark/1.0"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


def ensure_list(data) -> list[dict]:
    """Normalize response to list of dicts."""
    if isinstance(data, list):
        return [r for r in data if isinstance(r, dict)]
    if isinstance(data, dict):
        for key in ("data", "results", "items", "features", "hourly"):
            if key in data and isinstance(data[key], list):
                return [r for r in data[key] if isinstance(r, dict)]
        return [data]
    return []


def benchmark(name: str, url: str):
    """Fetch real API data and benchmark compression."""
    print(f"\n{'─' * 70}")
    print(f"  {name}")
    print(f"  {url}")
    print(f"{'─' * 70}")

    try:
        raw = fetch_json(url)
    except Exception as e:
        print(f"  SKIP: {e}")
        return None

    records = ensure_list(raw)
    if len(records) < 3:
        print(f"  SKIP: only {len(records)} records")
        return None

    json_str = json.dumps(records, separators=(",", ":"))
    json_bytes = json_str.encode()
    json_size = len(json_bytes)

    print(f"  Records: {len(records)}")
    print(f"  JSON: {json_size:,} bytes")
    print(f"  Fields: {sorted(records[0].keys()) if records else '?'}")
    print()

    # Learn schema (first 20 records)
    sample_size = min(20, len(records))
    codec = SchemaCodec.learn(records[:sample_size])

    methods = {}

    # gzip
    t0 = time.perf_counter()
    gz = gzip.compress(json_bytes, compresslevel=6)
    methods["gzip"] = (len(gz), time.perf_counter() - t0)

    # brotli
    t0 = time.perf_counter()
    br = brotli.compress(json_bytes, quality=6)
    methods["brotli"] = (len(br), time.perf_counter() - t0)

    # zstd
    cctx = zstandard.ZstdCompressor(level=6)
    t0 = time.perf_counter()
    zs = cctx.compress(json_bytes)
    methods["zstd"] = (len(zs), time.perf_counter() - t0)

    # Pidgin
    t0 = time.perf_counter()
    pidgin_compressed = codec.compress(records)
    methods["Pidgin"] = (len(pidgin_compressed), time.perf_counter() - t0)

    # Verify roundtrip
    restored = codec.decompress(pidgin_compressed)
    lossless = restored == records

    # Print results
    print(f"  {'Method':<12} {'Size':>10} {'Ratio':>8} {'Saved':>8} {'Time':>10}")
    print(f"  {'─'*12} {'─'*10} {'─'*8} {'─'*8} {'─'*10}")
    print(f"  {'JSON':<12} {json_size:>9,}B {'100.0%':>8} {'0.0%':>8} {'—':>10}")

    for method, (size, elapsed) in methods.items():
        ratio = size / json_size * 100
        saved = (1 - size / json_size) * 100
        marker = " <-- best" if method == "Pidgin" else ""
        print(f"  {method:<12} {size:>9,}B {ratio:>7.1f}% {saved:>7.1f}% {elapsed*1000:>8.2f}ms{marker}")

    print(f"\n  Pidgin roundtrip: {'PASS' if lossless else 'FAIL'}")

    return {
        "name": name,
        "records": len(records),
        "json_size": json_size,
        "fields": len(records[0]) if records else 0,
        "results": {k: v[0] for k, v in methods.items()},
        "lossless": lossless,
    }


def main():
    print("=" * 70)
    print("  PIDGIN — Real Public API Benchmark")
    print("=" * 70)

    apis = [
        # GitHub
        ("GitHub: Public Repos", "https://api.github.com/repositories?since=1000"),
        ("GitHub: Public Events", "https://api.github.com/events"),
        ("GitHub: Repo Search (python)", "https://api.github.com/search/repositories?q=python&per_page=30"),

        # JSONPlaceholder (fake but realistic structure)
        ("JSONPlaceholder: Users", "https://jsonplaceholder.typicode.com/users"),
        ("JSONPlaceholder: Posts", "https://jsonplaceholder.typicode.com/posts"),
        ("JSONPlaceholder: Comments", "https://jsonplaceholder.typicode.com/comments"),
        ("JSONPlaceholder: Todos (200)", "https://jsonplaceholder.typicode.com/todos"),
        ("JSONPlaceholder: Photos (5000)", "https://jsonplaceholder.typicode.com/photos"),

        # Open-Meteo (weather)
        ("Open-Meteo: Hourly Forecast", "https://api.open-meteo.com/v1/forecast?latitude=42.7&longitude=23.3&hourly=temperature_2m,relative_humidity_2m,wind_speed_10m&forecast_days=7"),
    ]

    results = []
    for name, url in apis:
        r = benchmark(name, url)
        if r:
            results.append(r)

    # Summary
    print(f"\n{'=' * 70}")
    print(f"  Summary — Pidgin vs brotli on REAL APIs")
    print(f"{'=' * 70}")
    print(f"\n  {'API':<35} {'Records':>7} {'brotli':>8} {'Pidgin':>8} {'Winner':>8}")
    print(f"  {'─'*35} {'─'*7} {'─'*8} {'─'*8} {'─'*8}")

    pidgin_wins = 0
    total = 0
    for r in results:
        br_ratio = r["results"]["brotli"] / r["json_size"] * 100
        pg_ratio = r["results"]["Pidgin"] / r["json_size"] * 100
        winner = "Pidgin" if pg_ratio < br_ratio else "brotli"
        if pg_ratio < br_ratio:
            pidgin_wins += 1
        total += 1
        print(f"  {r['name']:<35} {r['records']:>7} {br_ratio:>7.1f}% {pg_ratio:>7.1f}% {winner:>8}")

    print(f"\n  Pidgin wins: {pidgin_wins}/{total}")
    all_lossless = all(r["lossless"] for r in results)
    print(f"  All roundtrips lossless: {'YES' if all_lossless else 'NO'}")
    print(f"{'=' * 70}")


if __name__ == "__main__":
    main()
