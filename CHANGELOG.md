# Changelog

## pidgin-codec 0.2.0, @pidgin/codec 1.0.3, libpidgin 0.2.0 (not released yet)

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
