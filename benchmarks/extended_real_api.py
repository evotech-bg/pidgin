#!/usr/bin/env python3
"""
Extended real-world API benchmark for Pidgin.

Tests 20 public endpoints across diverse shape categories (field-heavy,
text-heavy, enum-heavy, nested, sparse, numeric-heavy). No API keys
required. Reports compression ratio for gzip, brotli, zstd, and Pidgin
against the raw JSON baseline. Roundtrip is verified for every endpoint.

By default the benchmark reads cached fixtures from real_api_fixtures/
to provide reproducibility — the SHA256 of every fixture is recorded in
manifest.json. Pass --live to refetch from the public APIs (the live
mode is non-deterministic because endpoints can return different content
over time).
"""

import argparse
import gzip
import hashlib
import json
import statistics
import sys
import urllib.request
from pathlib import Path

import brotli
import zstandard

from pidgin import SchemaCodec

FIXTURES_DIR = Path(__file__).parent / "real_api_fixtures"


def fetch_json(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "Pidgin-Benchmark/1.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def to_records(payload, key=None):
    if key:
        payload = payload.get(key, payload)
    if isinstance(payload, list):
        return [r for r in payload if isinstance(r, dict)]
    if isinstance(payload, dict):
        for k in ("results", "data", "items", "features", "hourly", "stories", "objects", "docs"):
            if k in payload and isinstance(payload[k], list):
                return [r for r in payload[k] if isinstance(r, dict)]
        return [payload]
    return []


# (name, category, url, optional extraction key)
ENDPOINTS = [
    # GitHub — field-heavy structured
    ("GitHub Repos",          "field-heavy",  "https://api.github.com/repositories?since=1000", None),
    ("GitHub Events",         "field-heavy",  "https://api.github.com/events", None),
    ("GitHub Repo Search",    "field-heavy",  "https://api.github.com/search/repositories?q=python&per_page=30", "items"),

    # JSONPlaceholder — shallow learning
    ("JSONPlaceholder Users",     "shallow",  "https://jsonplaceholder.typicode.com/users", None),
    ("JSONPlaceholder Posts",     "shallow",  "https://jsonplaceholder.typicode.com/posts", None),
    ("JSONPlaceholder Comments",  "shallow",  "https://jsonplaceholder.typicode.com/comments", None),
    ("JSONPlaceholder Todos",     "shallow",  "https://jsonplaceholder.typicode.com/todos", None),
    ("JSONPlaceholder Photos",    "shallow",  "https://jsonplaceholder.typicode.com/photos", None),

    # Open-Meteo — numeric-heavy
    ("Open-Meteo Hourly",     "numeric",      "https://api.open-meteo.com/v1/forecast?latitude=42.7&longitude=23.3&hourly=temperature_2m,relative_humidity_2m,wind_speed_10m&forecast_days=7", None),

    # SWAPI — field-heavy with enums
    ("SWAPI People",          "enum-heavy",   "https://swapi.dev/api/people/?page=1", "results"),
    ("SWAPI Planets",         "enum-heavy",   "https://swapi.dev/api/planets/?page=1", "results"),

    # Rick and Morty — nested
    ("RaM Characters",        "nested",       "https://rickandmortyapi.com/api/character", "results"),
    ("RaM Locations",         "nested",       "https://rickandmortyapi.com/api/location", "results"),
    ("RaM Episodes",          "nested",       "https://rickandmortyapi.com/api/episode", "results"),

    # PokeAPI — enum-heavy + nested
    ("PokeAPI Types",         "enum-heavy",   "https://pokeapi.co/api/v2/type?limit=20", "results"),
    ("PokeAPI Pokemon List",  "enum-heavy",   "https://pokeapi.co/api/v2/pokemon?limit=100", "results"),

    # REST Countries — field-heavy with categorical fields
    ("REST Countries (subset)","field-heavy", "https://restcountries.com/v3.1/region/europe?fields=name,cca2,region,subregion,population,area,currencies,languages", None),

    # Open Library — text-heavy
    ("OpenLibrary Search",    "text-heavy",   "https://openlibrary.org/search.json?q=python&limit=20", "docs"),

    # HN — sparse + numeric mixed
    ("HN Top Stories",        "sparse",       "https://hacker-news.firebaseio.com/v0/topstories.json", None),

    # CoinGecko — numeric-heavy
    ("CoinGecko Top Coins",   "numeric",      "https://api.coingecko.com/api/v3/coins/markets?vs_currency=usd&order=market_cap_desc&per_page=25&page=1", None),
]


def safe_name(name):
    return name.replace("/", "_").replace(" ", "_").replace(":", "")


def load_fixture(name):
    """Load cached records from fixture, verifying SHA256 from manifest."""
    manifest_path = FIXTURES_DIR / "manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(
            f"No fixture manifest at {manifest_path}. Run with --live --save-fixtures first."
        )
    manifest = json.loads(manifest_path.read_text())
    sn = safe_name(name)
    if sn not in manifest:
        return None
    fixture_path = FIXTURES_DIR / f"{sn}.json"
    if not fixture_path.exists():
        return None
    raw = fixture_path.read_bytes()
    expected_sha = manifest[sn]["sha256"]
    actual_sha = hashlib.sha256(raw).hexdigest()
    if actual_sha != expected_sha:
        raise ValueError(
            f"Fixture {sn}.json checksum mismatch: expected {expected_sha[:16]}..., got {actual_sha[:16]}..."
        )
    return json.loads(raw)


def run_benchmark(name, category, url, key, live=False):
    if not live:
        try:
            cached = load_fixture(name)
            if cached is None:
                return {"name": name, "skip": "no fixture (run with --live first)"}
            records = cached
        except Exception as e:
            return {"name": name, "skip": f"fixture: {str(e)[:80]}"}
    else:
        try:
            payload = fetch_json(url)
            records = to_records(payload, key)
        except Exception as e:
            return {"name": name, "skip": str(e)[:80]}

    # HN topstories returns list of ints, not dicts — skip
    if len(records) < 3:
        return {"name": name, "skip": f"only {len(records)} records"}

    json_bytes = json.dumps(records, separators=(",", ":")).encode()
    json_size = len(json_bytes)
    if json_size < 200:
        return {"name": name, "skip": f"too small: {json_size}B"}

    sample_size = min(20, len(records))
    try:
        codec = SchemaCodec.learn(records[:sample_size])
    except Exception as e:
        return {"name": name, "skip": f"learn failed: {str(e)[:60]}"}

    cctx = zstandard.ZstdCompressor(level=6)
    sizes = {}
    sizes["gzip"] = len(gzip.compress(json_bytes, compresslevel=6))
    sizes["brotli"] = len(brotli.compress(json_bytes, quality=6))
    sizes["zstd"] = len(cctx.compress(json_bytes))
    try:
        pid = codec.compress(records)
        sizes["pidgin"] = len(pid)
        try:
            restored = codec.decompress(pid)
            lossless = restored == records
        except Exception:
            lossless = False
    except Exception as e:
        return {"name": name, "skip": f"compress failed: {str(e)[:60]}"}

    field_count = len(records[0]) if records else 0
    ratios = {k: sizes[k] / json_size * 100 for k in sizes}

    # Determine winner among brotli vs pidgin (the contested case)
    diff_pp = ratios["brotli"] - ratios["pidgin"]
    if abs(diff_pp) < 0.1:
        winner = "tie"
    elif diff_pp > 0:
        winner = "pidgin"
    else:
        winner = "brotli"

    return {
        "name": name,
        "category": category,
        "records": len(records),
        "fields": field_count,
        "json_size": json_size,
        "ratios": ratios,
        "winner": winner,
        "diff_pp": diff_pp,
        "lossless": lossless,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true",
                        help="Fetch live from public APIs (default: use cached fixtures)")
    args = parser.parse_args()

    print("=" * 96)
    mode = "LIVE FETCH" if args.live else "CACHED FIXTURES"
    print(f"  PIDGIN — Extended Real Public API Benchmark (20 endpoints, mode: {mode})")
    if not args.live:
        print(f"  Loading fixtures from {FIXTURES_DIR.relative_to(FIXTURES_DIR.parent.parent)}/")
        print(f"  SHA256 checksums verified against manifest.json")
    print("=" * 96)
    print(f"\n  {'Endpoint':<28} {'Category':<13} {'Recs':>5} {'Fields':>7} {'JSON':>9} {'gzip':>6} {'brotli':>7} {'zstd':>6} {'Pidgin':>7} {'Δpp':>6} Winner")
    print("  " + "-" * 94)

    results = []
    for name, category, url, key in ENDPOINTS:
        r = run_benchmark(name, category, url, key, live=args.live)
        if "skip" in r:
            print(f"  {name:<28} SKIP: {r['skip']}")
            continue
        ratios = r["ratios"]
        print(f"  {r['name']:<28} {r['category']:<13} {r['records']:>5d} {r['fields']:>7d} "
              f"{r['json_size']:>8,}B {ratios['gzip']:>5.1f}% {ratios['brotli']:>6.1f}% "
              f"{ratios['zstd']:>5.1f}% {ratios['pidgin']:>6.1f}% {r['diff_pp']:>+5.1f} "
              f"{'✓' if r['lossless'] else '✗'} {r['winner']}")
        results.append(r)

    if not results:
        return

    print("\n" + "=" * 96)
    print(f"  SUMMARY ({len(results)} endpoints)")
    print("=" * 96)

    # Per-method aggregates
    for method in ("gzip", "brotli", "zstd", "pidgin"):
        ratios = [r["ratios"][method] for r in results]
        print(f"  {method:<8} median={statistics.median(ratios):5.1f}%  "
              f"mean={statistics.mean(ratios):5.1f}%  "
              f"best={min(ratios):5.1f}%  worst={max(ratios):5.1f}%")

    # Pidgin vs brotli win/tie/loss
    wins = sum(1 for r in results if r["winner"] == "pidgin")
    ties = sum(1 for r in results if r["winner"] == "tie")
    losses = sum(1 for r in results if r["winner"] == "brotli")
    print(f"\n  Pidgin vs brotli: {wins}W / {ties}T / {losses}L")

    # Roundtrip verification
    lossless_count = sum(1 for r in results if r["lossless"])
    print(f"  Lossless roundtrip: {lossless_count}/{len(results)} endpoints")

    # By category
    print("\n  By category (Pidgin median ratio):")
    cats = {}
    for r in results:
        cats.setdefault(r["category"], []).append(r["ratios"]["pidgin"])
    for cat, vals in sorted(cats.items()):
        print(f"    {cat:<13} n={len(vals):2d}  median={statistics.median(vals):5.1f}%  "
              f"range={min(vals):5.1f}% to {max(vals):5.1f}%")


if __name__ == "__main__":
    main()
