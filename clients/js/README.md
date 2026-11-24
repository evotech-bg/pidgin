# @pidgin/codec

JavaScript and TypeScript decoder for the [Pidgin](https://github.com/evotech-bg/pidgin) binary wire format. It turns a Pidgin payload back into the JSON a server encoded, using the schema profile the server publishes.

This package decodes only. Encoding happens on the server (the Python package `pidgin-codec` or the C library `libpidgin`).

## Install

```bash
npm install @pidgin/codec
```

Node 18 or newer, or a modern browser.

## Decode a response

```typescript
import { PidginDecoder } from "@pidgin/codec";
import { brotliDecompressSync } from "node:zlib";

// profile: JSON text from /.well-known/pidgin/<endpoint> on the server
// body: response bytes sent with Content-Encoding: pidgin
const decoder = PidginDecoder.fromJSON(profile, (b) => new Uint8Array(brotliDecompressSync(b)));
const data = await decoder.decode(body);
```

Pass a brotli function. Without one the decoder uses `DecompressionStream("br")` when the runtime has it. Node 22 does not, so brotli payloads then fail with a clear error. `decodeSync()` works for payloads that are not brotli compressed.

A payload decodes only with the profile it was encoded with. The server sends the current profile ETag in `X-Pidgin-ETag`; fetch the profile again when it changes. Decoded objects have their keys in schema order; the values are the same.

## `PidginClient`: not working yet

`PidginClient` asks for Pidgin with `Accept: application/x-pidgin`, but Pidgin servers look at `Accept-Encoding: pidgin` and answer with `Content-Encoding: pidgin`. So the client always gets, and returns, plain JSON. It does not break anything, it just never receives Pidgin. Browsers do not allow scripts to set `Accept-Encoding`, so a browser client needs a different way to ask; this is open.

## Tests

```bash
npm ci
npm test    # builds, then decodes the Python-encoded fixtures in ../../benchmarks/js_fixtures
```

## License

MIT
