/*
 * pidgin_wasm.c — WASM build of libpidgin.
 *
 * No brotli (handled in JS). No real pthreads (Workers are single-threaded).
 * Adds WASM-friendly wrappers for struct-returning functions.
 */

#define PIDGIN_NO_BROTLI 1
#define CJSON_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

/* No-op mutex wrappers — Workers are single-threaded */
int pidgin_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)m; (void)a; return 0; }
int pidgin_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
int pidgin_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
int pidgin_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

#define pthread_mutex_init pidgin_mutex_init
#define pthread_mutex_lock pidgin_mutex_lock
#define pthread_mutex_unlock pidgin_mutex_unlock
#define pthread_mutex_destroy pidgin_mutex_destroy

#include "../libpidgin/src/cjson.h"
#include "../libpidgin/src/pidgin.c"

/*
 * WASM-friendly wrappers.
 *
 * emscripten ccall can't handle struct returns (pidgin_buf_t).
 * These wrappers use output pointers instead.
 */

/* Compress: writes data pointer and length to output params */
void pidgin_engine_compress_wasm(
    pidgin_engine_t *engine,
    const char *endpoint,
    const char *json,
    uint8_t **out_data,
    size_t *out_len)
{
    pidgin_buf_t buf = pidgin_engine_compress(engine, endpoint, json);
    *out_data = buf.data;
    *out_len = buf.len;
}

/* Profile compress (manual API) */
void pidgin_compress_wasm(
    pidgin_profile_t *profile,
    const char *json,
    uint8_t **out_data,
    size_t *out_len)
{
    pidgin_buf_t buf = pidgin_compress(profile, json);
    *out_data = buf.data;
    *out_len = buf.len;
}
