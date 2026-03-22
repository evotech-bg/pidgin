/*
 * mod_pidgin — Apache output filter for Pidgin JSON compression.
 *
 * Directives:
 *   PidginEnable         On|Off          (default: Off)
 *   PidginLearnSamples   N               (default: 30)
 *   PidginMinSize        N               (default: 256)
 *   PidginEvolveInterval N               (default: 500)
 *
 * The module registers as an AP_FTYPE_CONTENT_SET output filter.
 * It observes JSON responses, auto-learns per-endpoint profiles,
 * and compresses when a client sends Accept-Encoding: pidgin.
 * Profile JSON is served at /.well-known/pidgin/<endpoint>.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "ap_config.h"
#include "apr_strings.h"
#include "apr_buckets.h"
#include "util_filter.h"

#include "pidgin.h"

/* ------------------------------------------------------------------ */
/* Module declaration                                                  */
/* ------------------------------------------------------------------ */

module AP_MODULE_DECLARE_DATA pidgin_module;

#define PIDGIN_FILTER_NAME "PIDGIN_COMPRESS"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int         enable;
    int         learn_samples;
    int         min_size;
    int         evolve_interval;
} pidgin_dir_conf_t;

/* Server-wide engine (shared across requests in a process) */
static pidgin_engine_t *pidgin_global_engine = NULL;
static apr_thread_mutex_t *pidgin_engine_mutex = NULL;
static apr_pool_t *pidgin_engine_pool = NULL;

/* ------------------------------------------------------------------ */
/* Engine lifecycle                                                    */
/* ------------------------------------------------------------------ */

static pidgin_engine_t *
get_engine(pidgin_dir_conf_t *conf, server_rec *s)
{
    if (pidgin_global_engine != NULL) {
        return pidgin_global_engine;
    }

    pidgin_global_engine = pidgin_engine_create(
        conf->learn_samples,
        conf->evolve_interval
    );

    if (pidgin_global_engine == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "mod_pidgin: failed to create pidgin engine");
    }

    return pidgin_global_engine;
}

static apr_status_t
destroy_engine(void *data)
{
    (void)data;
    if (pidgin_global_engine) {
        pidgin_engine_destroy(pidgin_global_engine);
        pidgin_global_engine = NULL;
    }
    return APR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Configuration handlers                                              */
/* ------------------------------------------------------------------ */

static void *
create_dir_conf(apr_pool_t *pool, char *dir)
{
    pidgin_dir_conf_t *conf = apr_pcalloc(pool, sizeof(pidgin_dir_conf_t));
    conf->enable          = -1;  /* unset */
    conf->learn_samples   = -1;
    conf->min_size        = -1;
    conf->evolve_interval = -1;
    return conf;
}

static void *
merge_dir_conf(apr_pool_t *pool, void *base_conf, void *new_conf)
{
    pidgin_dir_conf_t *base = base_conf;
    pidgin_dir_conf_t *add  = new_conf;
    pidgin_dir_conf_t *conf = apr_pcalloc(pool, sizeof(pidgin_dir_conf_t));

    conf->enable          = (add->enable >= 0)          ? add->enable          : base->enable;
    conf->learn_samples   = (add->learn_samples >= 0)   ? add->learn_samples   : base->learn_samples;
    conf->min_size        = (add->min_size >= 0)        ? add->min_size        : base->min_size;
    conf->evolve_interval = (add->evolve_interval >= 0) ? add->evolve_interval : base->evolve_interval;

    /* Defaults */
    if (conf->enable < 0)          conf->enable = 0;
    if (conf->learn_samples < 0)   conf->learn_samples = 30;
    if (conf->min_size < 0)        conf->min_size = 256;
    if (conf->evolve_interval < 0) conf->evolve_interval = 500;

    return conf;
}

/* ------------------------------------------------------------------ */
/* Directive handlers                                                  */
/* ------------------------------------------------------------------ */

static const char *
set_pidgin_enable(cmd_parms *cmd, void *cfg, const char *arg)
{
    pidgin_dir_conf_t *conf = cfg;
    if (strcasecmp(arg, "on") == 0) {
        conf->enable = 1;
    } else if (strcasecmp(arg, "off") == 0) {
        conf->enable = 0;
    } else {
        return "PidginEnable must be On or Off";
    }
    return NULL;
}

static const char *
set_pidgin_learn_samples(cmd_parms *cmd, void *cfg, const char *arg)
{
    pidgin_dir_conf_t *conf = cfg;
    conf->learn_samples = atoi(arg);
    if (conf->learn_samples < 1) {
        return "PidginLearnSamples must be a positive integer";
    }
    return NULL;
}

static const char *
set_pidgin_min_size(cmd_parms *cmd, void *cfg, const char *arg)
{
    pidgin_dir_conf_t *conf = cfg;
    conf->min_size = atoi(arg);
    if (conf->min_size < 0) {
        return "PidginMinSize must be a non-negative integer";
    }
    return NULL;
}

static const char *
set_pidgin_evolve_interval(cmd_parms *cmd, void *cfg, const char *arg)
{
    pidgin_dir_conf_t *conf = cfg;
    conf->evolve_interval = atoi(arg);
    if (conf->evolve_interval < 1) {
        return "PidginEvolveInterval must be a positive integer";
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int
client_accepts_pidgin(request_rec *r)
{
    const char *ae = apr_table_get(r->headers_in, "Accept-Encoding");
    if (ae == NULL) {
        return 0;
    }
    return (ap_strstr_c(ae, "pidgin") != NULL) ? 1 : 0;
}

static int
is_json_response(request_rec *r)
{
    const char *ct = r->content_type;
    if (ct == NULL) {
        return 0;
    }
    return (ap_strstr_c(ct, "application/json") != NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* /.well-known/pidgin/ handler                                       */
/* ------------------------------------------------------------------ */

static int
pidgin_wellknown_handler(request_rec *r)
{
    pidgin_dir_conf_t *conf;
    pidgin_engine_t   *engine;
    const char        *uri;
    char              *endpoint;
    char              *profile_json;
    const char        *etag;
    size_t             prefix_len;

    if (!r->handler || strcmp(r->handler, "pidgin-profile") != 0) {
        return DECLINED;
    }

    if (r->method_number != M_GET) {
        return HTTP_METHOD_NOT_ALLOWED;
    }

    conf = ap_get_module_config(r->per_dir_config, &pidgin_module);
    engine = get_engine(conf, r->server);
    if (engine == NULL) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Extract endpoint from URI: /.well-known/pidgin/api/users -> /api/users */
    uri = r->uri;
    prefix_len = strlen("/.well-known/pidgin");
    if (strlen(uri) <= prefix_len) {
        return HTTP_NOT_FOUND;
    }

    endpoint = apr_pstrdup(r->pool, uri + prefix_len);
    if (endpoint[0] != '/') {
        endpoint = apr_pstrcat(r->pool, "/", endpoint, NULL);
    }

    profile_json = pidgin_engine_profile(engine, endpoint);
    if (profile_json == NULL) {
        return HTTP_NOT_FOUND;
    }

    /* Set Content-Type */
    ap_set_content_type(r, "application/json");

    /* Set ETag */
    etag = pidgin_engine_etag(engine, endpoint);
    if (etag != NULL) {
        apr_table_set(r->headers_out, "ETag", etag);
    }

    /* Set Cache-Control */
    apr_table_set(r->headers_out, "Cache-Control", "public, max-age=60");

    ap_rputs(profile_json, r);

    pidgin_free(profile_json);

    return OK;
}

/* ------------------------------------------------------------------ */
/* Output filter                                                       */
/* ------------------------------------------------------------------ */

static apr_status_t
pidgin_output_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    request_rec       *r = f->r;
    pidgin_dir_conf_t *conf;
    pidgin_engine_t   *engine;
    apr_bucket        *b;
    char              *body = NULL;
    apr_size_t         body_len = 0;
    char               endpoint[512];
    int                accepts;
    apr_status_t       rv;

    conf = ap_get_module_config(r->per_dir_config, &pidgin_module);

    /* Skip if disabled or not JSON */
    if (!conf->enable || !is_json_response(r)) {
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    engine = get_engine(conf, r->server);
    if (engine == NULL) {
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    accepts = client_accepts_pidgin(r);

    /* Flatten the brigade to get the entire body */
    {
        apr_bucket_brigade *tmp;
        const char *data;
        apr_size_t  len;

        tmp = apr_brigade_create(r->pool, f->c->bucket_alloc);

        for (b = APR_BRIGADE_FIRST(bb);
             b != APR_BRIGADE_SENTINEL(bb);
             b = APR_BUCKET_NEXT(b))
        {
            if (APR_BUCKET_IS_EOS(b)) {
                break;
            }
            if (APR_BUCKET_IS_METADATA(b)) {
                continue;
            }

            rv = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
            if (rv != APR_SUCCESS) {
                ap_remove_output_filter(f);
                return ap_pass_brigade(f->next, bb);
            }

            if (body == NULL) {
                body = apr_palloc(r->pool, len + 1);
                memcpy(body, data, len);
                body_len = len;
            } else {
                char *new_body = apr_palloc(r->pool, body_len + len + 1);
                memcpy(new_body, body, body_len);
                memcpy(new_body + body_len, data, len);
                body = new_body;
                body_len += len;
            }
        }

        apr_brigade_destroy(tmp);
    }

    if (body == NULL || body_len == 0) {
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    body[body_len] = '\0';

    /* Check minimum size */
    if ((int)body_len < conf->min_size) {
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    /* Build endpoint string */
    {
        size_t uri_len = strlen(r->uri);
        size_t copy_len = (uri_len < sizeof(endpoint) - 1)
                        ? uri_len : sizeof(endpoint) - 1;
        memcpy(endpoint, r->uri, copy_len);
        endpoint[copy_len] = '\0';
    }

    /* Always observe for learning */
    pidgin_engine_observe(engine, endpoint, body);

    /* Try to compress if client accepts pidgin */
    if (accepts && pidgin_engine_ready(engine, endpoint)) {
        pidgin_buf_t compressed;

        compressed = pidgin_engine_compress(engine, endpoint, body);
        if (compressed.data != NULL && compressed.len > 0) {
            apr_bucket_brigade *out;
            apr_bucket         *bkt;

            /* Set headers */
            apr_table_set(r->headers_out, "Content-Encoding", "pidgin");
            apr_table_unset(r->headers_out, "Content-Length");

            {
                char ratio_buf[32];
                snprintf(ratio_buf, sizeof(ratio_buf), "%.1f%%",
                         (double)compressed.len / (double)body_len * 100.0);
                apr_table_set(r->headers_out, "X-Pidgin-Ratio", ratio_buf);
            }

            /* Build new brigade with compressed data */
            out = apr_brigade_create(r->pool, f->c->bucket_alloc);

            {
                char *comp_copy = apr_pmemdup(r->pool, compressed.data, compressed.len);
                bkt = apr_bucket_pool_create(comp_copy, compressed.len,
                                             r->pool, f->c->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(out, bkt);
            }

            bkt = apr_bucket_eos_create(f->c->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(out, bkt);

            pidgin_buf_free(&compressed);

            ap_remove_output_filter(f);

            /* Set correct content length */
            ap_set_content_length(r, (apr_off_t)compressed.len);

            return ap_pass_brigade(f->next, out);
        }

        if (compressed.data != NULL) {
            pidgin_buf_free(&compressed);
        }
    }

    /* Pass through original body */
    {
        apr_bucket_brigade *out;
        apr_bucket         *bkt;

        out = apr_brigade_create(r->pool, f->c->bucket_alloc);

        bkt = apr_bucket_pool_create(body, body_len, r->pool, f->c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(out, bkt);

        bkt = apr_bucket_eos_create(f->c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(out, bkt);

        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, out);
    }
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

static void
pidgin_insert_filter(request_rec *r)
{
    pidgin_dir_conf_t *conf;

    conf = ap_get_module_config(r->per_dir_config, &pidgin_module);
    if (conf->enable > 0) {
        ap_add_output_filter(PIDGIN_FILTER_NAME, NULL, r, r->connection);
    }
}

static int
pidgin_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                   apr_pool_t *ptemp, server_rec *s)
{
    pidgin_engine_pool = pconf;
    apr_pool_cleanup_register(pconf, NULL, destroy_engine, apr_pool_cleanup_null);

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s,
                 "mod_pidgin/%s initialized", pidgin_version());

    return OK;
}

static void
pidgin_register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(pidgin_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(pidgin_wellknown_handler, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_insert_filter(pidgin_insert_filter, NULL, NULL, APR_HOOK_MIDDLE);

    ap_register_output_filter(PIDGIN_FILTER_NAME, pidgin_output_filter,
                              NULL, AP_FTYPE_CONTENT_SET);
}

/* ------------------------------------------------------------------ */
/* Directives table                                                    */
/* ------------------------------------------------------------------ */

static const command_rec pidgin_cmds[] = {
    AP_INIT_TAKE1("PidginEnable", set_pidgin_enable, NULL,
                  OR_ALL, "Enable Pidgin compression (On|Off)"),

    AP_INIT_TAKE1("PidginLearnSamples", set_pidgin_learn_samples, NULL,
                  OR_ALL, "Number of responses to observe before learning (default 30)"),

    AP_INIT_TAKE1("PidginMinSize", set_pidgin_min_size, NULL,
                  OR_ALL, "Minimum response size for compression in bytes (default 256)"),

    AP_INIT_TAKE1("PidginEvolveInterval", set_pidgin_evolve_interval, NULL,
                  OR_ALL, "Re-learn interval in requests (default 500)"),

    { NULL }
};

/* ------------------------------------------------------------------ */
/* Module declaration                                                  */
/* ------------------------------------------------------------------ */

AP_DECLARE_MODULE(pidgin) = {
    STANDARD20_MODULE_STUFF,
    create_dir_conf,       /* create per-directory config */
    merge_dir_conf,        /* merge per-directory config */
    NULL,                  /* create per-server config */
    NULL,                  /* merge per-server config */
    pidgin_cmds,           /* command table */
    pidgin_register_hooks  /* register hooks */
};
