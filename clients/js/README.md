# Pidgin

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Python 3.10+](https://img.shields.io/badge/python-3.10%2B-blue.svg)](https://python.org)

**Runtime-learned compression that beats Protobuf.** No `.proto` files, no codegen, zero config. One line to enable in your web server.

---

## How It Works

1. Pidgin observes your JSON API responses and learns the schema automatically
2. It strips all keys (both sides know the schema) and encodes values with type-optimal binary
3. Brotli compression on top for maximum density
4. Clients negotiate via `Accept-Encoding: pidgin`

**Result:** 10-19% of original JSON size. Smaller than gzip, brotli, and Protobuf+zstd.

## Quick Start

### Web Server (one line)

```nginx
# nginx
pidgin on;
```

```lua
-- Kong plugin
plugins = { { name = "pidgin" } }
```

```python
# FastAPI
from pidgin.contrib.fastapi import PidginMiddleware
app.add_middleware(PidginMiddleware)
```

```python
# Django
MIDDLEWARE = ["pidgin.contrib.django.PidginMiddleware"]
```

Zero changes to your backend code. Pidgin auto-learns, auto-compresses, auto-evolves.

### Python Library

```bash
pip install pidgin-codec
```

```python
from pidgin import SchemaCodec

codec = SchemaCodec.learn(sample_records)      # learn from 20-50 samples
compressed = codec.compress(records)            # 10-19% of JSON size
original = codec.decompress(compressed)         # lossless roundtrip
```

### JavaScript Client

```bash
npm install @pidgin/codec
```

```typescript
import { PidginClient } from '@pidgin/codec';

const client = new PidginClient({
  baseUrl: 'https://api.example.com',
  profiles: { users: profileJson },
});
const users = await client.get('/api/users', 'users');
```

## Benchmarks

All benchmarks: same machine, same data, single-threaded. Lower is better.

### Compression Ratio (% of original JSON)

| Dataset | gzip | brotli | Protobuf+zstd | **Pidgin** |
|---|---|---|---|---|
| Users x1000 | 21.4% | 19.5% | 20.3% | **18.9%** |
| Orders x500 (nested) | 19.1% | 17.5% | 17.4% | **16.1%** |
| Events x5000 | 13.6% | 12.3% | 12.3% | **10.8%** |

### Speed (ms, C extension)

| Dataset | brotli | zstd | Proto+zstd | **Pidgin** |
|---|---|---|---|---|
| Users x1000 | 7.78 | 3.67 | 5.76 | **6.41** |
| Orders x500 | 10.52 | 6.41 | 6.97 | **7.96** |
| Events x5000 | 30.49 | 14.85 | 24.92 | **25.62** |

Speed competitive with brotli, faster at scale.

### Real Public APIs (verified, all roundtrips lossless)

| API | Fields | brotli | Pidgin | Winner |
|---|---|---|---|---|
| GitHub Repos | 46 | 6.7% | **6.0%** | Pidgin |
| GitHub Search | 82 | 8.8% | **8.4%** | Pidgin |
| JSONPlaceholder Posts | 4 | 27.5% | 27.5% | Tie |
| JSONPlaceholder Comments | 5 | **27.5%** | 28.4% | brotli |
| JSONPlaceholder Todos | 4 | 20.5% | **20.2%** | Pidgin |
| JSONPlaceholder Photos | 5 | **13.5%** | 14.3% | brotli |

Pidgin wins on field-heavy APIs (many keys to eliminate). brotli wins on text/URL-heavy data with few fields. Both produce lossless roundtrips.

## Schema Evolution

API structure changes are handled automatically:

| Change | What happens |
|--------|-------------|
| **New field added** | Falls to JSON fallback, then auto-evolve incorporates it |
| **Field removed** | ABSENT marker (1 byte), old clients unaffected |
| **Field returns** | Already in schema as nullable, encodes typed immediately |
| **New enum value** | Appended to enum list (old indices preserved) |
| **Type widened** (int to float) | Auto-widened safely |
| **Schema drift detected** | Auto-evolve every 500 requests, profile version bumped |

```python
# Manual evolution
v2_profile = codec.profile.evolve(new_samples)
v2_codec = SchemaCodec(v2_profile)

# See what changed
for line in codec.profile.diff(v2_profile):
    print(line)
```

Old clients with v1 profiles can still decode v2 data (unknown fields in JSON fallback).
New clients with v2 profiles can decode v1 data (missing fields as absent).

## Why Not Just Use X?

| Approach | Limitation Pidgin solves |
|---|---|
| **gzip/brotli** | Treats data as opaque bytes. Cannot exploit key repetition or type knowledge. |
| **Protobuf** | Requires `.proto` files, codegen, schema management. Pidgin learns at runtime. |
| **Protobuf+zstd** | Closes compression gap but adds toolchain complexity. Pidgin beats it with zero setup. |
| **msgpack/CBOR** | Binary JSON that still encodes every key. Pidgin eliminates keys entirely. |
| **Avro** | Needs schema registry infrastructure. Pidgin schemas are self-contained. |

## Architecture

```
Client                     Web Server                    Backend
  |                            |                            |
  |  Accept-Encoding: pidgin   |                            |
  | -------------------------> |  proxy_pass                |
  |                            | -------------------------> |
  |                            | <-------- JSON response -- |
  |                            |                            |
  |                            |  Pidgin engine:            |
  |                            |    if learning: observe    |
  |                            |    if ready: compress      |
  |                            |    if drift: evolve        |
  |                            |                            |
  | <-- Content-Encoding: pidgin                            |
  |     (binary, 10-19% of JSON)                            |
```

Backend returns normal JSON. Pidgin compresses transparently at the server level.

## Server Modules

| Server | Integration | Config |
|--------|------------|--------|
| **Kong** | Lua FFI plugin | `pidgin = true` |
| **nginx** | Dynamic C module | `pidgin on;` |
| **Apache** | Output filter | `PidginEnable On` |
| **Caddy** | Go middleware (cgo) | `pidgin` |
| **Traefik** | Pure Go plugin | middleware config |
| **HAProxy** | SPOE agent | filter config |
| **FastAPI** | Python middleware | `add_middleware(PidginMiddleware)` |
| **Django** | Python middleware | `MIDDLEWARE = [...]` |

## API Reference

### SchemaCodec

```python
from pidgin import SchemaCodec

codec = SchemaCodec.learn(samples)          # learn schema
compressed = codec.compress(data)            # dict or list[dict] -> bytes
original = codec.decompress(compressed)      # bytes -> dict or list[dict]
profile_json = codec.profile.to_json()       # share with clients
```

### Schema Evolution

```python
v2 = codec.profile.evolve(new_samples)       # backward + forward compatible
diff = codec.profile.diff(v2)                 # human-readable changes
codec_v2 = SchemaCodec(v2)                    # use evolved profile
```

### RatchetCipher (optional)

```python
from pidgin import RatchetCipher              # pip install pidgin[crypto]

cipher = RatchetCipher(shared_secret=b"key")
encrypted = cipher.encrypt(compressed)        # forward secrecy
original = cipher.decrypt(encrypted)          # ratchet stays in sync
```

### SecureChannel (optional)

```python
from pidgin import SecureChannel              # pip install pidgin[crypto]

alice = SecureChannel.create("alice")
bob = SecureChannel.create("bob")
alice.handshake(bob.public_key)               # X25519 key exchange
bob.handshake(alice.public_key)
envelope = alice.send("Hello")                # compress + encrypt + HMAC
msg = bob.receive(envelope)                   # verify + decrypt + decompress
```

## C Library

For non-Python environments, `libpidgin` provides a standalone C API:

```c
#include <pidgin.h>

pidgin_engine_t *engine = pidgin_engine_create(30, 500);
pidgin_engine_observe(engine, "/api/users", json_str);

if (pidgin_engine_ready(engine, "/api/users")) {
    pidgin_buf_t compressed = pidgin_engine_compress(engine, "/api/users", json_str);
    // send compressed.data (compressed.len bytes) to client
    pidgin_buf_free(&compressed);
}
```

## Project Structure

```
pidgin/
  libpidgin/          C library (foundation for all server modules)
  modules/            nginx, Apache, Kong, Caddy, Traefik, HAProxy
  src/pidgin/         Python package with C extension
  clients/js/         TypeScript decoder (@pidgin/codec)
  benchmarks/         Protobuf comparison suite
  docker/             Docker Compose test stack
  docs/               Landing page
```

## License

MIT -- see [LICENSE](LICENSE).

---

Built by Evo Tech Labs.
