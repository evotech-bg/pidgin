#!/bin/bash
# Build libpidgin as WASM for Cloudflare Workers.
#
# Requires: emscripten (brew install emscripten)
# Output: pidgin.wasm + pidgin.js (glue code)

set -euo pipefail
cd "$(dirname "$0")"

echo "Building pidgin.wasm..."

emcc -O2 \
    -s WASM=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME="PidginModule" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=4194304 \
    -s MAXIMUM_MEMORY=67108864 \
    -s EXPORTED_FUNCTIONS='[
        "_pidgin_engine_create",
        "_pidgin_engine_observe",
        "_pidgin_engine_ready",
        "_pidgin_engine_compress_wasm",
        "_pidgin_engine_decompress",
        "_pidgin_engine_profile",
        "_pidgin_engine_etag",
        "_pidgin_engine_destroy",
        "_pidgin_free",
        "_pidgin_version",
        "_pidgin_learn",
        "_pidgin_compress_wasm",
        "_pidgin_decompress",
        "_pidgin_profile_free",
        "_malloc",
        "_free"
    ]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","getValue","setValue"]' \
    -s ENVIRONMENT=web,node \
    -s FILESYSTEM=0 \
    -s ASSERTIONS=0 \
    -s STACK_SIZE=262144 \
    -s SINGLE_FILE=0 \
    -I ../libpidgin/include \
    pidgin_wasm.c \
    -o pidgin.js

echo "Output:"
ls -la pidgin.js pidgin.wasm
echo ""
echo "pidgin.wasm: $(wc -c < pidgin.wasm | tr -d ' ') bytes"
echo "pidgin.js:   $(wc -c < pidgin.js | tr -d ' ') bytes"
echo ""
echo "Done. Use in Worker: import PidginModule from './pidgin.js'"
