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

function PidginHandler:access(config)
    local path = kong.request.get_path()
    if path:sub(1, 21) == "/.well-known/pidgin/" then
        if not init(config) then return kong.response.exit(500, {error="init failed"}) end
        local ep = "/" .. path:sub(22)
        local ptr = lib.pidgin_engine_profile(engine, ep)
        if ptr == nil then return kong.response.exit(404, {error="not ready"}) end
        local json = ffi.string(ptr)
        lib.pidgin_free(ptr)
        return kong.response.exit(200, json, {["Content-Type"]="application/json"})
    end
end

function PidginHandler:header_filter(config)
    -- Remove upstream Content-Length so we can modify body
    local ct = ngx.header["Content-Type"] or ""
    if ct:find("json", 1, true) then
        local ae = ngx.var.http_accept_encoding or ""
        if ae:find("pidgin", 1, true) then
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

    -- Compress if client wants pidgin AND ready
    local ae = ngx.var.http_accept_encoding or ""
    if ae:find("pidgin", 1, true) and lib.pidgin_engine_ready(engine, endpoint) == 1 then
        local buf = lib.pidgin_engine_compress(engine, endpoint, body)
        local clen = tonumber(buf.len) or 0
        if buf.data ~= nil and clen > 0 and clen < #body then
            ngx.arg[1] = ffi.string(buf.data, clen)
            lib.pidgin_buf_free(buf)
            -- Headers already sent, but we cleared Content-Length in header_filter
            kong.log.notice("pidgin: ", endpoint, " ", #body, " -> ", clen, " (", string.format("%.0f", clen/#body*100), "%)")
            return
        end
        if buf.data ~= nil then lib.pidgin_buf_free(buf) end
    end

    ngx.arg[1] = body
end

return PidginHandler
