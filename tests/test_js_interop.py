"""Python encodes, the JS decoder (clients/js) decodes, Python compares.

Needs node and a built JS client (cd clients/js && npm ci && npm run build).
Skipped otherwise. The JS package only decodes, so there is no JS -> Python
direction to test.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from pidgin import SchemaCodec

ROOT = Path(__file__).resolve().parent.parent
JS = ROOT / "clients" / "js"
SCRIPT = JS / "test" / "decode-dir.mjs"


def _cases():
    users = [
        {"id": i, "name": f"Ünïcødé {i} ✓", "email": f"u{i}@example.com",
         "role": ["admin", "editor", "viewer"][i % 3], "active": i % 2 == 0,
         "score": round(i * 1.37 - 40, 3), "big": 2**40 + i, "neg": -i,
         "nickname": None if i % 4 == 0 else f"n{i}",
         "created": f"2026-0{1 + i % 9}-1{i % 9}T10:00:00Z"}
        for i in range(60)
    ]
    for i in range(0, 60, 7):
        del users[i]["nickname"]  # absent field
    orders = [
        {"id": i, "customer": {"id": i * 10, "name": f"C{i}", "vip": i % 5 == 0},
         "items": [{"sku": f"S{j}", "qty": j, "price": j * 2.5} for j in range(i % 4)],
         "tags": ["x", "y"][: i % 3], "status": ["new", "paid", "sent"][i % 3]}
        for i in range(40)
    ]
    return {"users": users, "orders": orders}


@pytest.fixture(scope="module")
def node_ready():
    if shutil.which("node") is None:
        pytest.skip("node not installed")
    if not (JS / "dist" / "esm" / "index.js").exists():
        pytest.skip("JS client not built (npm ci && npm run build in clients/js)")


@pytest.mark.parametrize("brotli_fn", [True, False], ids=["zlib-brotli", "no-brotli-fn"])
def test_python_encode_js_decode(tmp_path, node_ready, brotli_fn):
    expected = {}
    for name, records in _cases().items():
        for variant in ("c-brotli", "c-raw", "py-brotli", "py-raw", "single"):
            codec = SchemaCodec.learn(records[:30], use_brotli="brotli" in variant or variant == "single")
            if variant.startswith("py"):
                codec._c_schema = None  # force the pure Python encoder
            data = records[0] if variant == "single" else records
            key = f"{name}-{variant}"
            (tmp_path / f"{key}.bin").write_bytes(codec.compress(data))
            (tmp_path / f"{key}.profile.json").write_text(codec.profile.to_json())
            assert codec.decompress(codec.compress(data)) == data
            expected[key] = data

    args = ["node", str(SCRIPT), str(tmp_path)] + ([] if brotli_fn else ["--no-brotli-fn"])
    subprocess.run(args, check=True, timeout=60)

    failures = []
    for key, data in expected.items():
        out = json.loads((tmp_path / f"{key}.decoded.json").read_text())
        needs_fn = (not brotli_fn) and (key.endswith("brotli") or key.endswith("single"))
        if needs_fn and data is not None and (tmp_path / f"{key}.bin").read_bytes()[:1] == b"\x01":
            # Without a brotli function the decoder relies on
            # DecompressionStream("br"), which Node 22 does not have. It must
            # fail with a clear error, never return wrong data.
            if out["ok"] and out["value"] != data:
                failures.append(f"{key}: wrong data instead of an error")
            elif not out["ok"] and "no decompressor available" not in out["error"]:
                failures.append(f"{key}: unexpected error {out['error']}")
            continue
        if not out["ok"]:
            failures.append(f"{key}: {out['error']}")
        elif out["value"] != data:
            failures.append(f"{key}: decoded value differs")
    assert not failures, "\n".join(failures)
