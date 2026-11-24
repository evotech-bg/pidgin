/*
 * libpidgin — Runtime-learned JSON compression.
 *
 * Public C API. Link with -lpidgin.
 *
 * Lifecycle:
 *   1. pidgin_engine_create()          — create auto-learning engine
 *   2. pidgin_engine_observe(json)     — feed responses (auto-learns)
 *   3. pidgin_engine_compress(json)    — compress when ready
 *   4. pidgin_engine_decompress(data)  — decompress
 *   5. pidgin_engine_destroy()         — cleanup
 *
 * Or manual profile management:
 *   1. pidgin_learn(samples_json)      — learn profile from JSON array
 *   2. pidgin_compress(profile, json)  — compress
 *   3. pidgin_decompress(profile, data)— decompress
 */

#ifndef PIDGIN_H
#define PIDGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Types
 * ================================================================ */

typedef struct pidgin_profile pidgin_profile_t;
typedef struct pidgin_engine  pidgin_engine_t;

typedef struct {
    uint8_t *data;
    size_t   len;
} pidgin_buf_t;

typedef struct {
    int         ready;       /* 1 if profile is built */
    int         version;     /* profile version */
    int         requests;    /* total requests observed */
    const char *etag;        /* profile ETag (NULL if not ready) */
} pidgin_status_t;

/* ================================================================
 * Profile API — manual control
 * ================================================================ */

/*
 * Learn a profile from a JSON array of sample objects.
 * json_samples: UTF-8 JSON string, e.g. '[{"id":1,"name":"Alice"}, ...]'
 * Returns: new profile, or NULL on error. Caller must pidgin_profile_free().
 */
pidgin_profile_t *pidgin_learn(const char *json_samples);

/*
 * Compress a JSON value using a learned profile.
 * json: UTF-8 JSON string (object or array of objects).
 * Returns: compressed binary in buf. Caller must pidgin_buf_free().
 */
pidgin_buf_t pidgin_compress(pidgin_profile_t *profile, const char *json);

/*
 * Decompress binary back to JSON.
 * Returns: UTF-8 JSON string. Caller must pidgin_free().
 */
char *pidgin_decompress(pidgin_profile_t *profile, const uint8_t *data, size_t len);

/*
 * Limits for decoding payloads from an untrusted source.
 * max_output: largest payload after brotli, in bytes.
 * max_items:  largest number of decoded items: every object counts one and
 *             so does each field value in it, values inside the JSON
 *             section for unknown fields included.
 * max_json:   largest JSON text pidgin_decompress returns, in bytes (one
 *             byte on the wire can stand for a long string from the
 *             profile, so the text is counted separately).
 * A payload above any limit is refused (NULL), never truncated.
 * pidgin_decompress() and pidgin_engine_decompress() use the defaults,
 * which can be changed at build time with -DPIDGIN_MAX_OUTPUT=... and
 * -DPIDGIN_MAX_ITEMS=... and -DPIDGIN_MAX_JSON=...
 */
#ifndef PIDGIN_MAX_OUTPUT
#define PIDGIN_MAX_OUTPUT (32u * 1024u * 1024u)
#endif
#ifndef PIDGIN_MAX_ITEMS
#define PIDGIN_MAX_ITEMS 1000000u
#endif
#ifndef PIDGIN_MAX_JSON
#define PIDGIN_MAX_JSON (64u * 1024u * 1024u)
#endif
#define PIDGIN_DEFAULT_MAX_OUTPUT PIDGIN_MAX_OUTPUT
#define PIDGIN_DEFAULT_MAX_ITEMS  PIDGIN_MAX_ITEMS

typedef struct {
    size_t max_output;
    size_t max_items;
    size_t max_json;
} pidgin_limits_t;

/* As pidgin_decompress(), with explicit limits (NULL means the defaults). */
char *pidgin_decompress_limits(pidgin_profile_t *profile, const uint8_t *data, size_t len,
                               const pidgin_limits_t *limits);

/*
 * Evolve a profile with new samples (backward + forward compatible).
 * Returns: new profile with bumped version. Caller must pidgin_profile_free().
 */
pidgin_profile_t *pidgin_evolve(pidgin_profile_t *old, const char *new_samples_json);

/*
 * Serialize profile to JSON (for sharing with clients).
 * Returns: UTF-8 JSON string. Caller must pidgin_free().
 */
char *pidgin_profile_to_json(pidgin_profile_t *profile);

/*
 * Load profile from JSON.
 * Returns: profile, or NULL on error. Caller must pidgin_profile_free().
 */
pidgin_profile_t *pidgin_profile_from_json(const char *json);

void pidgin_profile_free(pidgin_profile_t *profile);

/* ================================================================
 * Engine API — auto-learning (for server modules)
 * ================================================================ */

/*
 * Create an engine instance.
 * learn_samples: how many responses to observe before building profile (default 30).
 * evolve_interval: check for schema drift every N requests (default 500).
 */
pidgin_engine_t *pidgin_engine_create(int learn_samples, int evolve_interval);

/*
 * Observe a JSON response for an endpoint. Feeds the learning engine.
 * endpoint: normalized path, e.g. "/api/users"
 * json: UTF-8 JSON response body.
 */
void pidgin_engine_observe(pidgin_engine_t *engine, const char *endpoint, const char *json);

/*
 * Check if an endpoint is ready for compression.
 */
int pidgin_engine_ready(pidgin_engine_t *engine, const char *endpoint);

/*
 * Compress a JSON response for an endpoint.
 * Returns compressed data in buf. buf.data is NULL if not ready.
 * Caller must pidgin_buf_free().
 */
pidgin_buf_t pidgin_engine_compress(pidgin_engine_t *engine, const char *endpoint, const char *json);

/*
 * Decompress binary data for an endpoint.
 * Returns: UTF-8 JSON string, or NULL if not ready. Caller must pidgin_free().
 */
char *pidgin_engine_decompress(pidgin_engine_t *engine, const char *endpoint,
                                const uint8_t *data, size_t len);

/*
 * Get profile JSON for an endpoint (for client download).
 * Returns: UTF-8 JSON string, or NULL if not ready. Caller must pidgin_free().
 */
char *pidgin_engine_profile(pidgin_engine_t *engine, const char *endpoint);

/*
 * Get profile ETag for caching.
 * Returns: ETag string, or NULL. Do NOT free (internal pointer).
 */
const char *pidgin_engine_etag(pidgin_engine_t *engine, const char *endpoint);

/*
 * Get engine status for an endpoint.
 */
pidgin_status_t pidgin_engine_status(pidgin_engine_t *engine, const char *endpoint);

void pidgin_engine_destroy(pidgin_engine_t *engine);

/* ================================================================
 * Memory management
 * ================================================================ */

void pidgin_free(void *ptr);
void pidgin_buf_free(pidgin_buf_t *buf);

/* ================================================================
 * Version
 * ================================================================ */

const char *pidgin_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PIDGIN_H */
