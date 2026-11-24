# Changelog

## pidgin-codec 0.2.1, @pidgin/codec 1.1.0, libpidgin 0.2.1 (not released yet)

### Security

- **Decoders had no limit on how far a payload could expand.** A payload of a few hundred bytes could make the decoder allocate hundreds of megabytes: brotli output was not capped, and the record counts and nested arrays in a payload were trusted. In our tests a 323-byte payload decoded to 200 MiB, and a 39-byte payload decoded to 4 million records (744 MiB in Python, 787 MiB in libpidgin, 317 MiB in the JS decoder). Anyone who decodes Pidgin payloads from a source they do not control is affected: pidgin-codec 0.1.0 and 0.2.0, @pidgin/codec up to 1.0.2, libpidgin up to 0.2.0. Encoding is not affected.
  - All three decoders now refuse payloads larger than 32 MiB after brotli and payloads with more than 1,000,000 objects and values, with `PidginDecodeError` (Python, JS) or `NULL` (C). Brotli is decoded as a stream that stops at the limit.
  - Python: `SchemaCodec(profile, max_output=..., max_items=...)`, or per call `decompress(data, max_output=..., max_items=...)`. Needs brotli 1.2 or later; with an older brotli, brotli payloads are refused instead of expanded.
  - JS: `new PidginDecoder(profile, brotli, { maxOutputBytes, maxItems })`. A brotli function you pass gets the limit as its second argument; pass it on (in Node: `brotliDecompressSync(b, { maxOutputLength: max })`) so an oversized payload is never allocated. Errors are the new exported `PidginDecodeError`.
  - libpidgin: new `pidgin_decompress_limits()` with `max_output`, `max_items` and `max_json` (the JSON text returned, default 64 MiB). `pidgin_decompress()` and the engine use the defaults, which can be set at build time with `-DPIDGIN_MAX_OUTPUT`, `-DPIDGIN_MAX_ITEMS` and `-DPIDGIN_MAX_JSON`.
- **libpidgin overflowed the stack on deeply nested JSON** in the unknown-fields section (a crash in a worker thread with a small stack). JSON nesting deeper than 256 is now refused.
- **Malformed payloads were not always errors.** The JS decoder read past the end of a truncated payload as zeros and returned partial data; varints had no length limit in all three decoders; an unknown-fields section that was not a JSON object was dropped (libpidgin) or raised a bare `ValueError` (Python); deep JSON raised `RecursionError` (Python). All of these are now decode errors.

### Changed

- pidgin-codec requires `brotli>=1.2.0` (for bounded decoding).
- The Python decoder no longer calls the C fast path for decoding: the call had the wrong arguments and always fell back to Python, so nothing changes in behaviour or speed. The C extension's decode function itself has no bounds checks and is not reachable from the public API.

### Added

- `tests/test_decompress_limit.py`, `libpidgin/tests/test_pidgin.c` (limit tests), `clients/js/test/limits.test.mjs`: each oversized payload above fails, without the process growing past the limit; ordinary payloads decode as before.

## libpidgin 0.2.1, number printing (not released yet)

Only libpidgin changes: the C engine inside the Kong, nginx, Apache, Caddy, Traefik and HAProxy modules and the Cloudflare worker. The Python package (pidgin-codec 0.2.0 on PyPI) does not contain libpidgin and is not affected; neither is the npm decoder itself.

### Fixed

- **libpidgin changed large and small numbers without an error.** Every number above 1e9 or below 1e-6 in absolute value, and every whole number above 1e15, was written as text with `"%e"`, which keeps 7 significant digits: 78185373126.77031 became 78185370000. This text is part of the compressed payload wherever libpidgin writes JSON (fields the profile does not know, nested objects without a schema, arrays of plain values), so a Python or JS client decoded a different number, and `pidgin_decompress` returned it in every field. Present in every libpidgin version up to and including 0.2.0. Numbers are now written in the shortest form that reads back as the same double, whole numbers digit for digit.
- **Integers that no double holds were rounded.** libpidgin parses JSON numbers as doubles, so an integer above 2^53 (for example a 64-bit snowflake id) lost its last digits. Such a message is now refused and the server sends the JSON as it is.
- **The libpidgin safety check compared types, not values.** `pidgin_compress` now decodes its own output and compares it with the input by value (numbers bit for bit, -0.0 included); if they differ it returns an empty buffer and the server sends JSON. This costs about 15% more encoding time for single records and about 40% for arrays of 100 records (`benchmarks/wins/cbench.py`); the payload bytes do not change.

### Added

- `libpidgin/tests/test_pidgin.c`: numbers above 1e9, below 1e-6 and whole numbers above 1e15 come back exact; integers above 2^53 are refused.
- `tests/test_libpidgin_interop.py`: the same through the Python decoder and `pidgin_decompress`, the nine CoinGecko supply values from `benchmarks/real_api_fixtures` that came back wrong, and 3000 random doubles from the whole range, compared bit for bit.

## pidgin-codec 0.2.0 (PyPI, 24.11.2026); @pidgin/codec 1.0.3 and libpidgin 0.2.0 (not released)

This release makes the documentation match what runs, and fixes the ways Pidgin could send data that decodes to something else.

### Fixed

- **Encoding could return wrong data without an error.** A record with a required field missing came back with a made-up default, an integer wider than 64 bits was cut, and a list in a string field came back as text. `SchemaCodec.compress()` now decodes its own output and raises the new `PidginEncodeError` if it differs. Pass `verify=False` to skip the check when the data is known to fit.
- **libpidgin crashed on a record with a missing field** (NULL dereference in the encoder), which would take down a server worker. It now checks every value against its field and returns an empty buffer for data that does not fit, so the server sends the JSON as it is.
- **Long values could be cut on the wire.** Strings, JSON fallback values and unknown fields longer than 65535 bytes had their 16-bit length prefix truncated in libpidgin. The call now fails and the JSON is sent as it is.
- **libpidgin wrote arrays of plain values and objects without a nested schema in a format its own decoder could not read** (the decoder returned null and lost its place). They are now written as JSON, the same way the Python encoder writes them.
- **FastAPI middleware cut the connection on every compressed response.** It kept the `Content-Length` of the JSON body. It now sends the right length.
- **Pagination wrappers decoded to the wrong object.** The middleware learns from the records inside `{"data": [...], "total": ...}` but encoded the whole wrapper with that profile. On Django this returned a different object without an error. Wrappers are now sent as plain JSON.
- **A fresh clone of the repository could not be imported.** `pidgin/__init__.py` imported modules that are not in the repository.

### Changed

- `Protocol`, `Codec`, `CompressedMessage` and `DecodedMessage` are no longer exported. They belonged to an earlier AI messaging experiment, were shipped by mistake in the 0.1.0 source distribution and are not part of this project.
- `PidginEncodeError` and `PidginDecodeError` are exported from `pidgin`.
- Development status is Alpha instead of Beta.
- README, landing page and the npm README now state for each integration whether it works and is tested, is experimental, or does not work yet, and show benchmark numbers from this release with the commands that produce them.

### Added

- `tests/test_middleware.py`: FastAPI and Django end to end.
- `tests/test_encode_safety.py`: data that does not fit the profile is refused, never encoded wrong.
- `tests/test_libpidgin_interop.py`: libpidgin encodes, Python decodes; libpidgin refuses data that does not fit.
- `tests/test_js_interop.py` and `npm test` in `clients/js`: Python encodes, the JS decoder decodes.

### What was checked for this release (24 November 2026)

| Piece | Result |
|---|---|
| Python tests, with libpidgin, JS client and web frameworks installed | 86 passed |
| libpidgin C tests | 94 passed |
| FastAPI and Django, real servers | responses decode to the same JSON; wrappers go as JSON |
| JS decoder | 6 fixture tests pass; Python and libpidgin output decode to the same data |
| JS `PidginClient` against FastAPI | gets plain JSON only (does not ask for Pidgin in a way servers read) |
| Kong handler unit test | 15 passed; plugin not run inside Kong |
| Traefik plugin, HAProxy agent | Go code builds; not run in the servers |
| nginx, Apache, Caddy | not built in this check; known problems listed in the README |
| Benchmarks | `benchmarks/realistic_api.py`, `benchmarks/extended_real_api.py` |

### Not changed

- The nginx, Apache, Caddy, Traefik and HAProxy modules. They are marked as not working yet.
- `PidginClient` in the JS package. It is marked as not working yet.

## pidgin-codec 0.1.0, @pidgin/codec 1.0.2 (31 March 2026)

First release.
