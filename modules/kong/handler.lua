-- Kong plugin: Pidgin JSON compression via libpidgin FFI.

local ffi = require "ffi"

ffi.cdef[[
    typedef struct pidgin_engine  pidgin_engine_t;
    typedef struct { uint8_t *data; size_t len; } pidgin_buf_t;

    pidgin_engine_t *pidgin_engine_create(int learn_samples, int evolve_interval);
    void pidgin_engine_observe(pidgin_engine_t *engine, const char *endpoint, const char *json);
    int  pidgin_engine_ready(pidgin_engine_t *engine, const char *endpoint);
    pidgin_buf_t pidgin_engine_compress(pidgin_engine_t *engine, const char *endpoint, const char *json);
    char *pidgin_engine_profile(pidgin_engine_t *engine, const char *endpoint);
    const char *pidgin_engine_etag(pidgin_engine_t *engine, const char *endpoint);
    void pidgin_engine_destroy(pidgin_engine_t *engine);
    void pidgin_free(void *ptr);
    void pidgin_buf_free(pidgin_buf_t *buf);
    const char *pidgin_version(void);
]]

local PidginHandler = { PRIORITY = 800, VERSION = "1.0.0" }

-- Shared across workers via module-level (each worker gets its own copy)
local lib = nil
local engine = nil

local function init(config)
    if engine then return true end
    local ok, l = pcall(ffi.load, config.library_path or "pidgin")
    if not ok then
        kong.log.err("pidgin: ffi.load failed: ", l)
        return false
    end
    lib = l
    engine = lib.pidgin_engine_create(config.learn_samples or 30, config.evolve_interval or 500)
    kong.log.notice("pidgin: engine v", ffi.string(lib.pidgin_version()), " ready")
    return engine ~= nil
end

-- Strip surrounding quotes from C ETag ("hex" → hex) for use as profile parameter.
local function unquote_etag(s)
    if not s then return nil end
    if s:sub(1,1) == '"' and s:sub(-1) == '"' then return s:sub(2, -2) end
    return s
end

-- Parse Accept-Encoding for: pidgin or pidgin; profile=<fingerprint>
-- Returns: wants_pidgin (bool), advertised_fingerprint (string|nil)
local function parse_accept_encoding(ae)
    if not ae or not ae:find("pidgin", 1, true) then return false, nil end
    -- Match a pidgin segment optionally with profile param
    -- Examples:
    --   "gzip, pidgin"                            → true, nil
    --   "pidgin; profile=abc123"                  → true, "abc123"
    --   "pidgin;profile=abc123, gzip"             → true, "abc123"
    local fp = ae:match("pidgin%s*;%s*profile%s*=%s*([%w]+)")
    return true, fp
end

function PidginHandler:access(config)
    local path = kong.request.get_path()
    if path:sub(1, 21) == "/.well-known/pidgin/" then
        if not init(config) then return kong.response.exit(500, {error="init failed"}) end
        local ep = "/" .. path:sub(22)
        local ptr = lib.pidgin_engine_profile(engine, ep)
        if ptr == nil then return kong.response.exit(404, {error="not ready"}) end
        local json = ffi.string(ptr)
        lib.pidgin_free(ptr)
        local etag_ptr = lib.pidgin_engine_etag(engine, ep)
        local headers = {["Content-Type"]="application/json"}
        if etag_ptr ~= nil then
            local etag = ffi.string(etag_ptr)
            headers["ETag"] = etag
            headers["Cache-Control"] = "max-age=86400, must-revalidate"
        end
        return kong.response.exit(200, json, headers)
    end
end

function PidginHandler:header_filter(config)
    -- Remove upstream Content-Length so we can modify body
    local ct = ngx.header["Content-Type"] or ""
    if ct:find("json", 1, true) then
        local ae = ngx.var.http_accept_encoding or ""
        local wants, _ = parse_accept_encoding(ae)
        if wants then
            ngx.header["Content-Length"] = nil
        end
    end
end

function PidginHandler:body_filter(config)
    if not init(config) then return end

    local ctx = ngx.ctx._pidgin
    if not ctx then
        ctx = { chunks = {} }
        ngx.ctx._pidgin = ctx
    end
    if ctx.done then return end

    local chunk = ngx.arg[1]
    local eof = ngx.arg[2]

    if type(chunk) == "string" and #chunk > 0 then
        ctx.chunks[#ctx.chunks + 1] = chunk
    end

    if not eof then
        ngx.arg[1] = ""
        return
    end

    ctx.done = true
    local body = table.concat(ctx.chunks)
    ctx.chunks = nil

    -- Only JSON
    local ct = ngx.header["Content-Type"] or ""
    if not ct:find("json", 1, true) then
        ngx.arg[1] = body
        return
    end

    if #body < (config.min_size or 256) then
        ngx.arg[1] = body
        return
    end

    local endpoint = (ngx.var.uri or "/"):match("^([^?]+)") or "/"

    -- Always observe
    lib.pidgin_engine_observe(engine, endpoint, body)

    -- Negotiation: client must (1) accept pidgin and (2) either omit profile fingerprint
    -- or advertise a fingerprint matching the active profile. A mismatch means the
    -- client has a stale cached profile; we serve uncompressed and let the client
    -- re-fetch the profile via /.well-known/pidgin/<endpoint>.
    local ae = ngx.var.http_accept_encoding or ""
    local wants, advertised_fp = parse_accept_encoding(ae)
    local ready = wants and (lib.pidgin_engine_ready(engine, endpoint) == 1)

    if ready and advertised_fp then
        local etag_ptr = lib.pidgin_engine_etag(engine, endpoint)
        if etag_ptr == nil then
            ready = false
        else
            local active_fp = unquote_etag(ffi.string(etag_ptr))
            -- Allow truncated client fingerprints (prefix match in either direction)
            if active_fp ~= advertised_fp
               and not active_fp:find(advertised_fp, 1, true) == 1
               and not advertised_fp:find(active_fp, 1, true) == 1 then
                kong.log.info("pidgin: fingerprint mismatch on ", endpoint,
                              " (client=", advertised_fp, " active=", active_fp, ") — falling back")
                ready = false
            end
        end
    end

    if ready then
        local buf = lib.pidgin_engine_compress(engine, endpoint, body)
        local clen = tonumber(buf.len) or 0
        if buf.data ~= nil and clen > 0 and clen < #body then
            ngx.arg[1] = ffi.string(buf.data, clen)
            lib.pidgin_buf_free(buf)

            -- Set response Content-Encoding with the active profile fingerprint.
            -- Headers haven't been written yet (we buffered all chunks until eof),
            -- so ngx.header[] modifications still take effect.
            local etag_ptr = lib.pidgin_engine_etag(engine, endpoint)
            if etag_ptr ~= nil then
                local fp = unquote_etag(ffi.string(etag_ptr))
                ngx.header["Content-Encoding"] = "pidgin; profile=" .. fp
            else
                ngx.header["Content-Encoding"] = "pidgin"
            end
            -- Vary tells caches that the response depends on the encoding header.
            local existing_vary = ngx.header["Vary"]
            if existing_vary then
                if not tostring(existing_vary):find("Accept%-Encoding") then
                    ngx.header["Vary"] = tostring(existing_vary) .. ", Accept-Encoding"
                end
            else
                ngx.header["Vary"] = "Accept-Encoding"
            end

            kong.log.notice("pidgin: ", endpoint, " ", #body, " -> ", clen, " (", string.format("%.0f", clen/#body*100), "%)")
            return
        end
        if buf.data ~= nil then lib.pidgin_buf_free(buf) end
    end

    ngx.arg[1] = body
end

return PidginHandler
