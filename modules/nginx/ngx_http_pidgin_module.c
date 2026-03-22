/*
 * ngx_http_pidgin_module — nginx filter for Pidgin JSON compression.
 *
 * Directives:
 *   pidgin              on|off          (default: off)
 *   pidgin_learn_samples  N             (default: 30)
 *   pidgin_min_size       N             (default: 256)
 *
 * Behaviour:
 *   - Observes every JSON response (feeds pidgin_engine_observe).
 *   - When the engine profile is ready AND client sends
 *     Accept-Encoding: pidgin, compresses the response body.
 *   - Serves profile JSON at /.well-known/pidgin/<endpoint>.
 *   - Sets Content-Encoding: pidgin on compressed responses.
 *
 * How the body buffering works (mirrors the Kong handler logic):
 *   1. header_filter: if JSON + engine ready + client accepts pidgin,
 *      set Content-Encoding: pidgin, clear Content-Length.  If JSON
 *      but not compressing, still set up ctx for observe-only mode
 *      (clear Content-Length because we buffer).
 *   2. body_filter: accumulate ALL chunks by copying data and consuming
 *      originals (pos = last).  On non-final chunks, pass the consumed
 *      chain through.  On last_buf: concatenate, observe(), optionally
 *      compress(), emit a single new buffer.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "pidgin.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    ngx_flag_t  enable;
    ngx_int_t   learn_samples;
    ngx_int_t   min_size;
} ngx_http_pidgin_loc_conf_t;

/* Per-request context */
typedef struct {
    ngx_chain_t  *chunks;           /* linked list of buffered data    */
    ngx_chain_t  *last_chain;       /* tail for O(1) append            */
    size_t        body_len;         /* total accumulated bytes         */
    unsigned      done:1;           /* complete body received + emitted*/
    unsigned      compress:1;       /* will compress (CE header sent)  */
    unsigned      observe_only:1;   /* buffering for observe, no CE   */
} ngx_http_pidgin_ctx_t;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static ngx_int_t ngx_http_pidgin_postconfiguration(ngx_conf_t *cf);
static void *ngx_http_pidgin_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_pidgin_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

static ngx_int_t ngx_http_pidgin_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_pidgin_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_pidgin_wellknown_handler(ngx_http_request_t *r);

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt   ngx_http_next_body_filter;

/* Single global engine per worker (nginx workers are single-threaded). */
static pidgin_engine_t *pidgin_global_engine;
static int              pidgin_global_learn_samples = 30;

/* ------------------------------------------------------------------ */
/* Directives                                                          */
/* ------------------------------------------------------------------ */

static ngx_command_t ngx_http_pidgin_commands[] = {

    { ngx_string("pidgin"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF
          | NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_pidgin_loc_conf_t, enable),
      NULL },

    { ngx_string("pidgin_learn_samples"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF
          | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_pidgin_loc_conf_t, learn_samples),
      NULL },

    { ngx_string("pidgin_min_size"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF
          | NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_pidgin_loc_conf_t, min_size),
      NULL },

    ngx_null_command
};

/* ------------------------------------------------------------------ */
/* Module context + definition                                         */
/* ------------------------------------------------------------------ */

static ngx_http_module_t ngx_http_pidgin_module_ctx = {
    NULL,                                   /* preconfiguration      */
    ngx_http_pidgin_postconfiguration,      /* postconfiguration     */
    NULL, NULL,                             /* main conf             */
    NULL, NULL,                             /* server conf           */
    ngx_http_pidgin_create_loc_conf,        /* create location conf  */
    ngx_http_pidgin_merge_loc_conf          /* merge location conf   */
};

ngx_module_t ngx_http_pidgin_module = {
    NGX_MODULE_V1,
    &ngx_http_pidgin_module_ctx,
    ngx_http_pidgin_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static ngx_str_t pidgin_enc_value     = ngx_string("pidgin");
static ngx_str_t pidgin_wellknown_pfx = ngx_string("/.well-known/pidgin/");

static pidgin_engine_t *
ngx_http_pidgin_engine(ngx_log_t *log)
{
    if (pidgin_global_engine) {
        return pidgin_global_engine;
    }

    pidgin_global_engine = pidgin_engine_create(
        pidgin_global_learn_samples, 500);

    if (pidgin_global_engine == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "pidgin: failed to create engine");
    } else {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "pidgin: engine v%s ready (learn=%d)",
                      pidgin_version(), pidgin_global_learn_samples);
    }
    return pidgin_global_engine;
}

static ngx_int_t
ngx_http_pidgin_client_accepts(ngx_http_request_t *r)
{
    ngx_table_elt_t *h = r->headers_in.accept_encoding;
    if (h == NULL || h->value.len == 0) {
        return 0;
    }
    return ngx_strnstr(h->value.data, "pidgin", h->value.len) != NULL;
}

static ngx_int_t
ngx_http_pidgin_is_json(ngx_http_request_t *r)
{
    if (r->headers_out.content_type.len == 0) {
        return 0;
    }
    return ngx_strnstr(r->headers_out.content_type.data, "json",
                       r->headers_out.content_type.len) != NULL;
}

static void
ngx_http_pidgin_endpoint(ngx_http_request_t *r, char *buf, size_t cap)
{
    size_t len = r->uri.len < (cap - 1) ? r->uri.len : (cap - 1);
    ngx_memcpy(buf, r->uri.data, len);
    buf[len] = '\0';
}

/* Concatenate all accumulated chunks into one NUL-terminated buffer. */
static u_char *
ngx_http_pidgin_concat(ngx_http_request_t *r, ngx_http_pidgin_ctx_t *ctx)
{
    u_char      *out, *p;
    ngx_chain_t *cl;
    size_t       sz;

    out = ngx_pnalloc(r->pool, ctx->body_len + 1);
    if (out == NULL) {
        return NULL;
    }
    p = out;
    for (cl = ctx->chunks; cl; cl = cl->next) {
        sz = ngx_buf_size(cl->buf);
        if (sz) {
            ngx_memcpy(p, cl->buf->pos, sz);
            p += sz;
        }
    }
    *p = '\0';
    return out;
}

/* Create single-buffer output chain. */
static ngx_chain_t *
ngx_http_pidgin_output(ngx_http_request_t *r, u_char *data, size_t len)
{
    ngx_buf_t   *b;
    ngx_chain_t *cl;

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NULL;
    }
    b->pos          = data;
    b->last         = data + len;
    b->memory       = 1;
    b->last_buf     = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NULL;
    }
    cl->buf  = b;
    cl->next = NULL;
    return cl;
}

/* ------------------------------------------------------------------ */
/* /.well-known/pidgin/<endpoint>  — precontent-phase handler          */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_pidgin_wellknown_handler(ngx_http_request_t *r)
{
    pidgin_engine_t *engine;
    char            *json;
    char             ep[512];
    ngx_buf_t       *b;
    ngx_chain_t      out;
    ngx_int_t        rc;
    size_t           pfx, eplen, jlen;

    /* Only match /.well-known/pidgin/ prefix */
    pfx = pidgin_wellknown_pfx.len;
    if (r->uri.len < pfx
        || ngx_strncmp(r->uri.data, pidgin_wellknown_pfx.data, pfx) != 0)
    {
        return NGX_DECLINED;     /* not our request */
    }

    if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (r->uri.len <= pfx) {
        return NGX_HTTP_NOT_FOUND;
    }

    eplen = r->uri.len - pfx;
    if (eplen + 2 > sizeof(ep)) {
        return NGX_HTTP_REQUEST_URI_TOO_LARGE;
    }
    ep[0] = '/';
    ngx_memcpy(ep + 1, r->uri.data + pfx, eplen);
    ep[eplen + 1] = '\0';

    engine = ngx_http_pidgin_engine(r->connection->log);
    if (engine == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    json = pidgin_engine_profile(engine, ep);
    if (json == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }
    jlen = ngx_strlen(json);

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        pidgin_free(json);
        return rc;
    }

    r->headers_out.status            = NGX_HTTP_OK;
    r->headers_out.content_type_len  = sizeof("application/json") - 1;
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_length_n  = (off_t)jlen;

    /* ETag */
    {
        const char *etag = pidgin_engine_etag(engine, ep);
        if (etag) {
            ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
            if (h) {
                h->hash = 1;
                ngx_str_set(&h->key, "ETag");
                h->value.len  = ngx_strlen(etag);
                h->value.data = ngx_pnalloc(r->pool, h->value.len);
                if (h->value.data) {
                    ngx_memcpy(h->value.data, etag, h->value.len);
                }
            }
        }
    }

    if (r->method == NGX_HTTP_HEAD) {
        rc = ngx_http_send_header(r);
        pidgin_free(json);
        return rc;
    }

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        pidgin_free(json);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    b->pos = ngx_pnalloc(r->pool, jlen);
    if (b->pos == NULL) {
        pidgin_free(json);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(b->pos, json, jlen);
    pidgin_free(json);

    b->last     = b->pos + jlen;
    b->memory   = 1;
    b->last_buf = 1;

    out.buf  = b;
    out.next = NULL;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    return ngx_http_output_filter(r, &out);
}

/* ------------------------------------------------------------------ */
/* Header filter                                                       */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_pidgin_header_filter(ngx_http_request_t *r)
{
    ngx_http_pidgin_loc_conf_t *plcf;
    ngx_http_pidgin_ctx_t      *ctx;
    pidgin_engine_t            *engine;
    char                        endpoint[512];
    int                         will_compress;

    /* Skip subrequests */
    if (r != r->main) {
        return ngx_http_next_header_filter(r);
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_pidgin_module);
    if (!plcf->enable
        || r->headers_out.status != NGX_HTTP_OK
        || r->header_only
        || !ngx_http_pidgin_is_json(r))
    {
        return ngx_http_next_header_filter(r);
    }

    /* Skip tiny responses whose size is known */
    if (r->headers_out.content_length_n != -1
        && r->headers_out.content_length_n < plcf->min_size)
    {
        return ngx_http_next_header_filter(r);
    }

    /* Latch learn_samples for lazy engine init */
    pidgin_global_learn_samples = (int)plcf->learn_samples;

    /*
     * Decide NOW whether we will attempt compression.
     * We need this decision in the header filter because Content-Encoding
     * must be set before the header chain finishes (the core header filter
     * serializes headers into a buffer at the end of the chain).
     */
    will_compress = 0;
    if (ngx_http_pidgin_client_accepts(r)) {
        engine = ngx_http_pidgin_engine(r->connection->log);
        if (engine) {
            ngx_http_pidgin_endpoint(r, endpoint, sizeof(endpoint));
            if (pidgin_engine_ready(engine, endpoint)) {
                will_compress = 1;
            }
        }
    }

    /* Set up per-request context */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_pidgin_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    ngx_http_set_ctx(r, ctx, ngx_http_pidgin_module);

    if (will_compress) {
        ctx->compress = 1;

        /* Set Content-Encoding: pidgin NOW, before headers are serialized */
        r->headers_out.content_encoding =
            ngx_list_push(&r->headers_out.headers);
        if (r->headers_out.content_encoding) {
            r->headers_out.content_encoding->hash = 1;
            ngx_str_set(&r->headers_out.content_encoding->key,
                        "Content-Encoding");
            r->headers_out.content_encoding->value = pidgin_enc_value;
        }
    } else {
        ctx->observe_only = 1;
    }

    /*
     * Clear Content-Length: we are buffering the body and may change it.
     * This forces chunked transfer encoding in HTTP/1.1.
     */
    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_clear_etag(r);

    /* Ensure upstream data is provided in memory (not file-backed) */
    r->main_filter_need_in_memory = 1;

    return ngx_http_next_header_filter(r);
}

/* ------------------------------------------------------------------ */
/* Body filter                                                         */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_pidgin_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_pidgin_ctx_t      *ctx;
    ngx_http_pidgin_loc_conf_t *plcf;
    pidgin_engine_t            *engine;
    ngx_chain_t                *cl, *ln;
    ngx_buf_t                  *b;
    char                        endpoint[512];
    u_char                     *body;
    int                         saw_last;
    size_t                      sz;

    ctx = ngx_http_get_module_ctx(r, ngx_http_pidgin_module);
    if (ctx == NULL || ctx->done || r != r->main) {
        return ngx_http_next_body_filter(r, in);
    }

    if (in == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_pidgin_module);

    /*
     * Phase 1: Accumulate incoming data.
     *
     * For each buffer in the chain:
     *   - Copy its contents into pool-allocated memory
     *   - Mark the original as consumed (pos = last)
     *
     * We pass the consumed chain through to the next filter so nginx's
     * write filter sees the chain (with zero-length buffers) and the
     * upstream recycling works correctly.
     */
    saw_last = 0;
    for (cl = in; cl; cl = cl->next) {
        b = cl->buf;

        if (b->last_buf || b->last_in_chain) {
            saw_last = 1;
        }

        /* File-backed buffers shouldn't appear with main_filter_need_in_memory
         * but handle gracefully */
        if (b->in_file) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "pidgin: file-backed buffer, passing through");
            ctx->done = 1;
            return ngx_http_next_body_filter(r, in);
        }

        sz = ngx_buf_size(b);
        if (sz > 0) {
            ngx_buf_t *cb;
            u_char    *cd;

            cd = ngx_pnalloc(r->pool, sz);
            if (cd == NULL) {
                goto bail;
            }
            ngx_memcpy(cd, b->pos, sz);

            cb = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
            if (cb == NULL) {
                goto bail;
            }
            cb->pos    = cd;
            cb->last   = cd + sz;
            cb->memory = 1;

            ln = ngx_alloc_chain_link(r->pool);
            if (ln == NULL) {
                goto bail;
            }
            ln->buf  = cb;
            ln->next = NULL;

            if (ctx->last_chain) {
                ctx->last_chain->next = ln;
            } else {
                ctx->chunks = ln;
            }
            ctx->last_chain = ln;
            ctx->body_len  += sz;
        }

        /* Consume original so downstream sees empty buffers */
        b->pos = b->last;
    }

    /*
     * Phase 2: If not the final chunk, suppress output entirely.
     *
     * We do NOT call ngx_http_next_body_filter here.  This keeps the
     * serialized HTTP header bytes queued in r->out (unsent) so that
     * we can still modify headers_out before they hit the wire.
     * The original buffers are consumed (pos == last) so upstream
     * recycling works correctly.
     *
     * This is the nginx equivalent of the Kong pattern:
     *   ngx.arg[1] = ""   -- suppress chunk
     *   return            -- wait for EOF
     */
    if (!saw_last) {
        return NGX_OK;
    }

    /*
     * Phase 3: Complete body received.  Concatenate, observe, optionally
     * compress, and emit.
     */
    ctx->done = 1;

    body = ngx_http_pidgin_concat(r, ctx);
    if (body == NULL) {
        goto bail;
    }

    ngx_http_pidgin_endpoint(r, endpoint, sizeof(endpoint));

    engine = ngx_http_pidgin_engine(r->connection->log);
    if (engine == NULL) {
        goto emit_original;
    }

    /* Always observe — this is how the engine learns schema profiles */
    if ((off_t)ctx->body_len >= plcf->min_size) {
        pidgin_engine_observe(engine, endpoint, (const char *)body);
    }

    /* Compress if header filter decided we should */
    if (ctx->compress) {
        pidgin_buf_t compressed;
        size_t       clen;

        compressed = pidgin_engine_compress(engine, endpoint,
                                            (const char *)body);
        clen = compressed.len;

        if (compressed.data != NULL && clen > 0 && clen < ctx->body_len) {
            u_char      *cdata;
            ngx_chain_t *out;

            cdata = ngx_pnalloc(r->pool, clen);
            if (cdata == NULL) {
                pidgin_buf_free(&compressed);
                goto emit_original;
            }
            ngx_memcpy(cdata, compressed.data, clen);
            pidgin_buf_free(&compressed);

            out = ngx_http_pidgin_output(r, cdata, clen);
            if (out == NULL) {
                goto emit_original;
            }

            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                          "pidgin: %s %uz -> %uz (%ui%%)",
                          endpoint, ctx->body_len, clen,
                          (ngx_uint_t)(clen * 100 / ctx->body_len));

            return ngx_http_next_body_filter(r, out);
        }

        /* Compression failed or didn't save space */
        if (compressed.data != NULL) {
            pidgin_buf_free(&compressed);
        }

        /*
         * We already sent Content-Encoding: pidgin in the header filter,
         * but compression failed.  Since the header is already serialized
         * and sent, we cannot remove it.  The safest thing is to still
         * emit the original JSON — clients that don't find valid pidgin
         * binary will get a parse error, but this edge case only occurs
         * if the engine was ready at header time but compress() failed,
         * which should be extremely rare.
         *
         * A future improvement could use r->header_sent to check whether
         * we can restart or use a 2xx-to-5xx internal redirect.  For now,
         * log a warning.
         */
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "pidgin: %s compress failed after CE header sent, "
                      "passing original %uz bytes", endpoint, ctx->body_len);
    }

emit_original:
    {
        ngx_chain_t *out;

        out = ngx_http_pidgin_output(r, body, ctx->body_len);
        if (out == NULL) {
            return NGX_ERROR;
        }
        return ngx_http_next_body_filter(r, out);
    }

bail:
    /* OOM or unexpected state — let the original buffers through */
    ctx->done = 1;
    return ngx_http_next_body_filter(r, in);
}

/* ------------------------------------------------------------------ */
/* Configuration callbacks                                             */
/* ------------------------------------------------------------------ */

static void *
ngx_http_pidgin_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_pidgin_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_pidgin_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->enable        = NGX_CONF_UNSET;
    conf->learn_samples = NGX_CONF_UNSET;
    conf->min_size      = NGX_CONF_UNSET;
    return conf;
}

static char *
ngx_http_pidgin_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_pidgin_loc_conf_t *prev = parent;
    ngx_http_pidgin_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable,        prev->enable,        0);
    ngx_conf_merge_value(conf->learn_samples, prev->learn_samples, 30);
    ngx_conf_merge_value(conf->min_size,      prev->min_size,      256);

    return NGX_CONF_OK;
}

/* ------------------------------------------------------------------ */
/* Post-configuration: install filters + well-known handler            */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_pidgin_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    /* Install header filter (runs in reverse registration order) */
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter  = ngx_http_pidgin_header_filter;

    /* Install body filter */
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter  = ngx_http_pidgin_body_filter;

    /* Register /.well-known/pidgin/ handler in the precontent phase.
     * The handler itself checks the URI prefix and returns NGX_DECLINED
     * for unrelated requests. */
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_pidgin_wellknown_handler;

    return NGX_OK;
}
