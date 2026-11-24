# Pidgin

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Python 3.10+](https://img.shields.io/badge/python-3.10%2B-blue.svg)](https://python.org)

**Runtime-learned binary encoding for JSON APIs.** Pidgin watches your JSON responses, learns their schema, drops the keys and writes the values in a compact binary form. No `.proto` files and no code generation.

Status: early. The Python library, the FastAPI and Django middleware and the JavaScript decoder work and are tested. Most web server modules do not work yet. See [What works today](#what-works-today).

---

## How it works

1. Pidgin observes JSON responses and learns the schema (field names, types, enum values, nesting).
2. Keys are dropped (both sides hold the schema) and values are written with type-specific binary encoders.
3. Brotli runs on top when it makes the payload smaller.
4. A client asks for it with `Accept-Encoding: pidgin` and decodes with the schema profile the server publishes at `/.well-known/pidgin/<endpoint>`.

On generated API data the result is 10.8% to 18.9% of the original JSON, a little smaller than brotli and Protobuf+zstd. On real public API responses it is about the same as brotli. Pidgin is slower to compress than brotli. Numbers and commands are in [Benchmarks](#benchmarks).

## What works today

Checked for release 0.2.0 by running each piece, not by reading the code. The logs of that check are summarised in [CHANGELOG.md](CHANGELOG.md).

| Integration | State | What was checked |
|---|---|---|
| Python library (`SchemaCodec`) | Works, tested | unit tests, C extension and pure Python encoders |
| `RatchetCipher`, `SecureChannel` | Works, tested | unit tests (needs the `crypto` extra) |
| FastAPI middleware | Works, tested | real uvicorn server, `Accept-Encoding: pidgin`, body decodes to the same JSON |
| Django middleware | Works, tested | real Django server, same check |
| JavaScript decoder (`PidginDecoder`) | Works, tested | decodes Python and libpidgin output to the same data |
| C library (`libpidgin`) | Works, tested | C tests, output decoded by Python and JavaScript |
| Kong plugin | Experimental | header parsing unit test passes and the engine it wraps is tested; the plugin was not run inside Kong for this release |
| JavaScript `PidginClient` | Not working yet | sends `Accept: application/x-pidgin`, which no Pidgin server reads, so it always gets plain JSON. Browsers do not let scripts set `Accept-Encoding` |
| nginx module | Not working yet | loads and learns, but never compresses a response |
| Apache module | Not working yet | builds, never run against Apache |
| Caddy module | Not working yet | the Caddy build cannot find the module |
| Traefik plugin | Not working yet | Go code builds, Traefik cannot load the plugin |
| HAProxy agent | Not working yet | agent builds, the HAProxy config uses an action HAProxy does not have |

Known limits of the working parts:

- `compress()` checks its own output. Data that does not fit the learned profile (a required field missing, an integer wider than 64 bits, a list where a string was learned) raises `PidginEncodeError` instead of producing a payload that decodes to something else. The middleware then sends plain JSON. `compress(data, verify=False)` skips the check.
- Responses wrapped for pagination, such as `{"data": [...], "total": 10}`, are sent as plain JSON. Only a record or a list of records is encoded. A record that has a list field named `data`, `results`, `items` or `records` is taken for such a wrapper and is also sent as JSON.
- A payload decodes only with the profile it was encoded with. When the server's profile changes (it evolves after schema drift), clients must fetch the new one. The middleware sends the current profile ETag in `X-Pidgin-ETag`.
- Decoded objects come back with keys in schema order, not in the order the server wrote them. Values are identical.

## Quick start

### Python library

```bash
pip install pidgin-codec
```

The PyPI package is a source distribution, so installing it builds a small C extension (a C compiler is needed).

```python
from pidgin import SchemaCodec

codec = SchemaCodec.learn(sample_records)      # learn from 20-50 records
compressed = codec.compress(records)            # dict or list[dict] -> bytes
original = codec.decompress(compressed)         # same data back
profile_json = codec.profile.to_json()          # give this to clients
```

### FastAPI

```python
from fastapi import FastAPI
from pidgin.contrib.fastapi import PidginMiddleware

app = FastAPI()
app.add_middleware(PidginMiddleware)
```

### Django

```python
# settings.py
MIDDLEWARE = [
    "pidgin.contrib.django.PidginMiddleware",
    # ...
]
```

The middleware learns from the first 30 records it sees per endpoint, then encodes responses for clients that send `Accept-Encoding: pidgin`. Other clients keep getting JSON. Profiles are served at `/.well-known/pidgin/<endpoint>`, for example `/.well-known/pidgin/api-users` for `/api/users`.

### JavaScript decoder

```bash
npm install @pidgin/codec
```

```typescript
import { PidginDecoder } from "@pidgin/codec";
import { brotliDecompressSync } from "node:zlib";

// profile: JSON text from /.well-known/pidgin/<endpoint>
// body: the response bytes (Content-Encoding: pidgin)
const decoder = PidginDecoder.fromJSON(profile, (b, max) => new Uint8Array(brotliDecompressSync(b, { maxOutputLength: max })));
const data = await decoder.decode(body);
```

Pass a brotli function. Without one the decoder falls back to `DecompressionStream("br")`, which Node 22 does not have; brotli payloads then fail with a clear error.

## Benchmarks

Measured on 24 November 2026: Apple M5 Max laptop, Python 3.15, C extension on, single thread. Lower is better.

### Generated API data

`python benchmarks/realistic_api.py`

| Dataset | gzip -6 | brotli -6 | Protobuf+zstd | Pidgin |
|---|---|---|---|---|
| Users x1000 | 21.4% | 19.5% | 20.3% | 18.9% |
| Orders x500 (nested) | 19.1% | 17.5% | 17.4% | 16.1% |
| Events x5000 | 13.6% | 12.3% | 12.3% | 10.8% |

Time to compress, in ms, with the default output check:

| Dataset | brotli -6 | zstd -6 | Protobuf+zstd | Pidgin |
|---|---|---|---|---|
| Users x1000 | 11.1 | 7.0 | 8.2 | 30.7 |
| Orders x500 | 10.8 | 5.8 | 8.0 | 34.3 |
| Events x5000 | 35.8 | 19.9 | 21.1 | 86.8 |

Pidgin is smaller on this data and 2.5 to 3 times slower to compress than brotli. Without the output check (`verify=False`) the same run measured 16.7, 20.7 and 63.3 ms. Times vary between runs on a laptop; sizes do not.

### Real public API responses

`python benchmarks/extended_real_api.py` (16 cached responses in `benchmarks/real_api_fixtures`, checksums verified)

| API | Fields | brotli | Pidgin |
|---|---|---|---|
| GitHub Repos | 46 | 6.7% | 6.0% |
| GitHub Repo Search | 82 | 8.8% | 8.4% |
| Rick and Morty Characters | 12 | 6.0% | 4.6% |
| JSONPlaceholder Posts | 4 | 27.5% | 27.5% |
| JSONPlaceholder Comments | 5 | 27.5% | 28.4% |
| JSONPlaceholder Photos | 5 | 13.5% | 14.3% |
| CoinGecko Top Coins | 26 | 22.9% | 23.9% |

Across all 16: Pidgin is smaller on 5, equal on 3 and larger on 8. Median 18.3% for Pidgin, 18.0% for brotli. Pidgin helps most on responses with many fields and nested objects, and does not help on text-heavy or few-field data. Every response decoded back to the same data.

## Schema evolution

```python
v2_profile = codec.profile.evolve(new_samples)
v2_codec = SchemaCodec(v2_profile)

for line in codec.profile.diff(v2_profile):
    print(line)
```

| Change | What happens |
|--------|-------------|
| New field added | Written as JSON inside the payload until the profile evolves, then typed |
| Field becomes optional | Evolved profile marks it nullable; absent costs one byte |
| New enum value | Appended, old indices keep their meaning |
| Int becomes float | Widened |
| Schema drift in the middleware | Every 500 requests the keys are compared; if they changed the profile evolves and its version and ETag change |

An evolved codec reads both old and new shapes of the data. A payload encoded with one profile version cannot be decoded with another version, so clients must refresh the profile when the ETag changes.

## Optional: encryption

```bash
pip install "pidgin-codec[crypto]"
```

```python
from pidgin import RatchetCipher

cipher = RatchetCipher(shared_secret=b"key")
encrypted = cipher.encrypt(compressed)        # key moves forward after each message
original = peer_cipher.decrypt(encrypted)     # peer with the same secret stays in step
```

```python
from pidgin import SecureChannel

alice = SecureChannel.create("alice")
bob = SecureChannel.create("bob")
alice.handshake(bob.public_key)               # X25519 key exchange
bob.handshake(alice.public_key)
envelope = alice.send("Hello")                # compress, encrypt, sign
message = bob.receive(envelope)               # dict with text, sender, id, ts
```

## C library

`libpidgin` is the engine the server modules are built on.

```c
#include <pidgin.h>

pidgin_engine_t *engine = pidgin_engine_create(30, 500);
pidgin_engine_observe(engine, "/api/users", json_str);

if (pidgin_engine_ready(engine, "/api/users")) {
    pidgin_buf_t compressed = pidgin_engine_compress(engine, "/api/users", json_str);
    if (compressed.len > 0) {
        /* send compressed.data (compressed.len bytes) */
    } else {
        /* data does not fit the profile: send json_str as it is */
    }
    pidgin_buf_free(&compressed);
}
```

Build and test: `make -C libpidgin && make -C libpidgin test` (needs brotli).

## Running the checks

```bash
make setup                                        # venv with the C extension
.venv/bin/pip install fastapi httpx django        # for the middleware tests
make -C libpidgin                                 # enables the C and Python tests against libpidgin
cd clients/js && npm ci && npm test && cd ../..   # JS decoder; also enables the Python to JS test
make test
```

Tests whose tools are missing are skipped, not failed.

## Project structure

```
pidgin/
  src/pidgin/         Python package with C extension and FastAPI/Django middleware
  libpidgin/          C library
  clients/js/         TypeScript decoder (@pidgin/codec)
  modules/            Kong (experimental); nginx, Apache, Caddy, Traefik, HAProxy (not working yet)
  benchmarks/         comparison scripts and fixtures
  docker/             test stack for the server modules
  docs/               landing page
```

## License

MIT, see [LICENSE](LICENSE).

---

Built by Evo Tech Labs.
