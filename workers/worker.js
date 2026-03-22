/**
 * Cloudflare Worker: Pidgin JSON compression.
 *
 * Deploy in front of any JSON API. Zero backend changes.
 * Auto-learns schemas, compresses when client sends Accept-Encoding: pidgin.
 *
 * wrangler deploy
 */

import PidginModule from './pidgin.js';

let wasm = null;
let engine = null;

async function init() {
  if (engine) return;
  wasm = await PidginModule();
  engine = wasm.ccall('pidgin_engine_create', 'number', ['number', 'number'], [30, 500]);
}

function normalizeEndpoint(path) {
  return path.replace(/\/\d+/g, '/:id').replace(/\/[a-f0-9-]{20,}/g, '/:id');
}

function compressWithEngine(endpoint, jsonBody) {
  const outDataPtr = wasm._malloc(4);
  const outLenPtr = wasm._malloc(4);

  wasm.ccall('pidgin_engine_compress_wasm', null,
    ['number', 'string', 'string', 'number', 'number'],
    [engine, endpoint, jsonBody, outDataPtr, outLenPtr]);

  const dataPtr = wasm.getValue(outDataPtr, 'i32');
  const dataLen = wasm.getValue(outLenPtr, 'i32');
  wasm._free(outDataPtr);
  wasm._free(outLenPtr);

  if (dataPtr <= 0 || dataLen <= 0) return null;

  const bytes = new Uint8Array(dataLen);
  for (let i = 0; i < dataLen; i++) {
    bytes[i] = wasm.getValue(dataPtr + i, 'i8') & 0xFF;
  }
  wasm._free(dataPtr);
  return bytes;
}

export default {
  async fetch(request, env, ctx) {
    await init();

    const url = new URL(request.url);
    const path = url.pathname;

    // Serve profiles
    if (path.startsWith('/.well-known/pidgin/')) {
      const ep = '/' + path.slice('/.well-known/pidgin/'.length);
      const ptr = wasm.ccall('pidgin_engine_profile', 'number', ['number', 'string'], [engine, ep]);
      if (!ptr) {
        return new Response(JSON.stringify({ error: 'profile not ready' }), {
          status: 404, headers: { 'Content-Type': 'application/json' },
        });
      }
      const json = wasm.UTF8ToString(ptr);
      wasm.ccall('pidgin_free', null, ['number'], [ptr]);
      return new Response(json, {
        headers: { 'Content-Type': 'application/json', 'Cache-Control': 'public, max-age=86400' },
      });
    }

    // Proxy to origin
    const response = await fetch(request);
    const ct = response.headers.get('Content-Type') || '';
    if (!ct.includes('json')) return response;

    const body = await response.text();
    if (body.length < 256) return new Response(body, response);

    const endpoint = normalizeEndpoint(path);

    // Always observe
    wasm.ccall('pidgin_engine_observe', null,
      ['number', 'string', 'string'], [engine, endpoint, body]);

    // Compress if client accepts pidgin AND ready
    const ae = request.headers.get('Accept-Encoding') || '';
    const ready = wasm.ccall('pidgin_engine_ready', 'number',
      ['number', 'string'], [engine, endpoint]);

    if (!ae.includes('pidgin') || !ready) {
      return new Response(body, {
        status: response.status, headers: response.headers,
      });
    }

    const compressed = compressWithEngine(endpoint, body);
    if (!compressed || compressed.length >= body.length) {
      return new Response(body, {
        status: response.status, headers: response.headers,
      });
    }

    const headers = new Headers(response.headers);
    headers.set('Content-Encoding', 'pidgin');
    headers.set('Content-Type', 'application/octet-stream');
    headers.delete('Content-Length');
    headers.set('X-Pidgin-Original-Size', String(body.length));
    headers.set('X-Pidgin-Ratio', (compressed.length / body.length * 100).toFixed(1) + '%');

    return new Response(compressed, { status: response.status, headers });
  },
};
