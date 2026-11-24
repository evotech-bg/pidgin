/*
 * pidgin.c — Runtime-learned JSON compression library.
 *
 * Wire format (byte-compatible with Python pidgin.schema_codec):
 *
 *   Outer: [brotli_flag:1][payload]
 *     brotli_flag: 0x00 = raw, 0x01 = brotli-compressed payload
 *
 *   Payload: [type:1][data...]
 *     type 0x01 = single object
 *     type 0x02 = array, followed by [count:u16be]
 *
 *   Per record per field (in field_order):
 *     If nullable: [marker:1]
 *       0xFF = null, 0x00 = absent, 0x01 = present (then value follows)
 *     Value encoding by dtype:
 *       bool:      1 byte (0 or 1)
 *       int:       ZigZag varint
 *       float:     8 bytes big-endian IEEE 754 double
 *       str:       [len:u16be][utf8_bytes]
 *       enum:      [idx:1] (0xFE = unknown, then [len:u16be][utf8_bytes])
 *       timestamp: [len:1][utf8_bytes]
 *       object:    recursive record encoding
 *       array:     [count:u16be][records...]
 *     After all known fields:
 *       [unknown_len:u16be][json_fallback_bytes]  (0 if none)
 */

#define CJSON_IMPL
#include "cjson.h"

#include "../include/pidgin.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

/* Optional brotli support */
#ifdef PIDGIN_NO_BROTLI
#define HAS_BROTLI 0
#else
#include <brotli/encode.h>
#include <brotli/decode.h>
#define HAS_BROTLI 1
#endif

/* ================================================================
 * Version
 * ================================================================ */

static const char *VERSION_STRING = "0.1.0";

const char *pidgin_version(void) { return VERSION_STRING; }

/* ================================================================
 * Dynamic buffer
 * ================================================================ */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} pbuf_t;

static int pbuf_init(pbuf_t *b, size_t cap) {
    b->data = (uint8_t *)malloc(cap);
    if (!b->data) return -1;
    b->len = 0;
    b->cap = cap;
    return 0;
}

static void pbuf_free(pbuf_t *b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int pbuf_grow(pbuf_t *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t nc = b->cap;
    while (nc < b->len + need) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, nc);
    if (!p) return -1;
    b->data = p;
    b->cap = nc;
    return 0;
}

static int pbuf_u8(pbuf_t *b, uint8_t v) {
    if (pbuf_grow(b, 1)) return -1;
    b->data[b->len++] = v;
    return 0;
}

static int pbuf_write(pbuf_t *b, const void *src, size_t n) {
    if (pbuf_grow(b, n)) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int pbuf_u16be(pbuf_t *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    return pbuf_write(b, t, 2);
}

/* ZigZag varint encode */
static int pbuf_varint(pbuf_t *b, int64_t val) {
    uint64_t v = (uint64_t)((val << 1) ^ (val >> 63));
    do {
        uint8_t c = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) c |= 0x80;
        if (pbuf_u8(b, c)) return -1;
    } while (v);
    return 0;
}

/* Big-endian double */
static int pbuf_double(pbuf_t *b, double val) {
    uint8_t t[8];
    uint8_t *p = (uint8_t *)&val;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    memcpy(t, p, 8);
#else
    for (int i = 0; i < 8; i++) t[i] = p[7 - i];
#endif
    return pbuf_write(b, t, 8);
}

/* ================================================================
 * Read helpers (for decode)
 * ================================================================ */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} rdbuf_t;

static int rd_u8(rdbuf_t *r, uint8_t *out) {
    if (r->pos >= r->len) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static int rd_u16be(rdbuf_t *r, uint16_t *out) {
    if (r->pos + 2 > r->len) return -1;
    *out = ((uint16_t)r->data[r->pos] << 8) | r->data[r->pos + 1];
    r->pos += 2;
    return 0;
}

static int rd_bytes(rdbuf_t *r, const uint8_t **out, size_t n) {
    if (r->pos + n > r->len) return -1;
    *out = r->data + r->pos;
    r->pos += n;
    return 0;
}

static int rd_varint(rdbuf_t *r, int64_t *out) {
    uint64_t result = 0;
    int shift = 0;
    while (r->pos < r->len) {
        uint8_t c = r->data[r->pos++];
        result |= (uint64_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) break;
        shift += 7;
    }
    *out = (int64_t)((result >> 1) ^ -(int64_t)(result & 1));
    return 0;
}

static int rd_double(rdbuf_t *r, double *out) {
    if (r->pos + 8 > r->len) return -1;
    uint8_t t[8];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    memcpy(t, r->data + r->pos, 8);
#else
    for (int i = 0; i < 8; i++) t[i] = r->data[r->pos + 7 - i];
#endif
    r->pos += 8;
    memcpy(out, t, 8);
    return 0;
}

/* ================================================================
 * Internal field schema types
 * ================================================================ */

#define DTYPE_BOOL      0
#define DTYPE_INT       1
#define DTYPE_FLOAT     2
#define DTYPE_STR       3
#define DTYPE_ENUM      4
#define DTYPE_TIMESTAMP 5
#define DTYPE_OBJECT    6
#define DTYPE_ARRAY     7
#define DTYPE_NULL      8

#define MARKER_NULL    0xFF
#define MARKER_PRESENT 0x01
#define MARKER_ABSENT  0x00

typedef struct pfield {
    char   *name;
    int     dtype;          /* DTYPE_* */
    int     nullable;
    double  frequency;      /* 0.0 - 1.0 */

    /* Enum values */
    char  **enum_values;
    int     enum_count;

    /* Nested object schema (dtype == DTYPE_OBJECT) */
    struct pfield *nested;
    char  **nested_order;
    int     nested_count;

    /* Array items schema (dtype == DTYPE_ARRAY of objects) */
    struct pfield *items;
    char  **items_order;
    int     items_count;
} pfield_t;

struct pidgin_profile {
    pfield_t *fields;       /* array of field schemas */
    char    **field_order;   /* ordered field names */
    int       field_count;
    int       version;
};

/* ================================================================
 * Engine internal types
 * ================================================================ */

typedef struct endpoint_state {
    char              *path;
    pidgin_profile_t  *profile;
    int                ready;
    int                requests;
    int                version;
    char              *etag;

    /* Learning buffer */
    char             **samples;     /* stored JSON strings */
    int                sample_count;
    int                sample_cap;

    struct endpoint_state *next;
} endpoint_state_t;

struct pidgin_engine {
    endpoint_state_t *endpoints;   /* linked list */
    int               learn_samples;
    int               evolve_interval;
    pthread_mutex_t   lock;
};

/* ================================================================
 * Forward declarations
 * ================================================================ */

static void free_fields(pfield_t *fields, int count);
static pfield_t *learn_fields(cJSON **samples, int n_samples,
                              char ***out_order, int *out_count);
static int encode_object(pbuf_t *b, cJSON *obj, pfield_t *fields,
                         char **order, int field_count);
static cJSON *decode_object(rdbuf_t *r, pfield_t *fields,
                            char **order, int field_count);
static pfield_t *clone_fields(const pfield_t *src, int count);
static char **clone_order(char **src, int count);
static pfield_t *find_field(pfield_t *fields, int count, const char *name);
static pfield_t merge_field(const pfield_t *old, const pfield_t *new_f);


/* ================================================================
 * Memory management
 * ================================================================ */

void pidgin_free(void *ptr) {
    free(ptr);
}

void pidgin_buf_free(pidgin_buf_t *buf) {
    if (buf && buf->data) {
        free(buf->data);
        buf->data = NULL;
        buf->len = 0;
    }
}

static void free_field(pfield_t *f) {
    if (!f) return;
    if (f->name) free(f->name);
    if (f->enum_values) {
        for (int i = 0; i < f->enum_count; i++)
            if (f->enum_values[i]) free(f->enum_values[i]);
        free(f->enum_values);
    }
    if (f->nested) free_fields(f->nested, f->nested_count);
    if (f->nested_order) {
        for (int i = 0; i < f->nested_count; i++)
            if (f->nested_order[i]) free(f->nested_order[i]);
        free(f->nested_order);
    }
    if (f->items) free_fields(f->items, f->items_count);
    if (f->items_order) {
        for (int i = 0; i < f->items_count; i++)
            if (f->items_order[i]) free(f->items_order[i]);
        free(f->items_order);
    }
}

static void free_fields(pfield_t *fields, int count) {
    if (!fields) return;
    for (int i = 0; i < count; i++)
        free_field(&fields[i]);
    free(fields);
}

void pidgin_profile_free(pidgin_profile_t *profile) {
    if (!profile) return;
    free_fields(profile->fields, profile->field_count);
    if (profile->field_order) {
        for (int i = 0; i < profile->field_count; i++)
            if (profile->field_order[i]) free(profile->field_order[i]);
        free(profile->field_order);
    }
    free(profile);
}

/* ================================================================
 * Utility: string duplication, sorting comparators
 * ================================================================ */

static char *pstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *c = (char *)malloc(len);
    if (c) memcpy(c, s, len);
    return c;
}

/* Sort context for qsort_r (frequency desc, then alphabetical) */
typedef struct {
    pfield_t *fields;
    int       count;
} sort_ctx_t;

/* Portable comparator using global — we only sort during learn, single-threaded */
static sort_ctx_t *g_sort_ctx = NULL;

static int field_order_cmp(const void *a, const void *b) {
    const char *ka = *(const char **)a;
    const char *kb = *(const char **)b;
    sort_ctx_t *ctx = g_sort_ctx;

    double fa = 0, fb = 0;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->fields[i].name, ka) == 0) fa = ctx->fields[i].frequency;
        if (strcmp(ctx->fields[i].name, kb) == 0) fb = ctx->fields[i].frequency;
    }

    /* Descending by frequency */
    if (fa > fb) return -1;
    if (fa < fb) return 1;
    /* Then alphabetical */
    return strcmp(ka, kb);
}

/* ================================================================
 * Schema Learner
 * ================================================================ */

/* Infer dtype from an array of values for one field */
static int infer_dtype(cJSON **values, int count, char ***enum_values_out, int *enum_count_out) {
    *enum_values_out = NULL;
    *enum_count_out = 0;

    if (count == 0) return DTYPE_NULL;

    /* Count non-null values and collect type info */
    int n_bool = 0, n_int = 0, n_float = 0, n_str = 0;
    int n_obj = 0, n_arr = 0;
    int non_null = 0;

    for (int i = 0; i < count; i++) {
        cJSON *v = values[i];
        if (!v || cJSON_IsNull(v)) continue;
        non_null++;
        if (cJSON_IsBool(v)) n_bool++;
        else if (cJSON_IsNumber(v)) {
            double d = v->valuedouble;
            if (d == (double)v->valueint && d == floor(d)) n_int++;
            else n_float++;
        }
        else if (cJSON_IsString(v)) n_str++;
        else if (cJSON_IsObject(v)) n_obj++;
        else if (cJSON_IsArray(v)) n_arr++;
    }

    if (non_null == 0) return DTYPE_NULL;
    if (n_bool == non_null) return DTYPE_BOOL;

    if (n_int + n_float == non_null) {
        if (n_float > 0) return DTYPE_FLOAT;
        return DTYPE_INT;
    }

    if (n_str == non_null) {
        /* Check for timestamp: len >= 19, contains 'T', ends with Z or has + after pos 10 */
        cJSON *sample = NULL;
        for (int i = 0; i < count; i++) {
            if (values[i] && cJSON_IsString(values[i])) {
                sample = values[i];
                break;
            }
        }
        if (sample && sample->valuestring) {
            size_t slen = strlen(sample->valuestring);
            const char *s = sample->valuestring;
            if (slen >= 19 && strchr(s, 'T') != NULL) {
                int ends_z = (s[slen - 1] == 'Z');
                int has_plus = (slen > 10 && strchr(s + 10, '+') != NULL);
                if (ends_z || has_plus) return DTYPE_TIMESTAMP;
            }
        }

        /* Check for enum: low cardinality */
        /* Collect unique values */
        char **unique = (char **)malloc(sizeof(char *) * (size_t)non_null);
        int n_unique = 0;
        for (int i = 0; i < count; i++) {
            if (!values[i] || !cJSON_IsString(values[i])) continue;
            const char *s = values[i]->valuestring;
            int found = 0;
            for (int j = 0; j < n_unique; j++) {
                if (strcmp(unique[j], s) == 0) { found = 1; break; }
            }
            if (!found) {
                unique[n_unique++] = (char *)s;  /* borrow pointer */
            }
        }

        if (n_unique <= 20 && n_unique < non_null / 2) {
            /* Sort unique values for deterministic order */
            for (int i = 0; i < n_unique - 1; i++)
                for (int j = i + 1; j < n_unique; j++)
                    if (strcmp(unique[i], unique[j]) > 0) {
                        char *tmp = unique[i]; unique[i] = unique[j]; unique[j] = tmp;
                    }

            /* Copy enum values */
            char **enums = (char **)malloc(sizeof(char *) * (size_t)n_unique);
            for (int i = 0; i < n_unique; i++)
                enums[i] = pstrdup(unique[i]);

            *enum_values_out = enums;
            *enum_count_out = n_unique;
            free(unique);
            return DTYPE_ENUM;
        }
        free(unique);
        return DTYPE_STR;
    }

    if (n_obj == non_null) return DTYPE_OBJECT;
    if (n_arr == non_null) return DTYPE_ARRAY;

    /* Mixed types — fallback to string */
    return DTYPE_STR;
}

/*
 * Learn schema from an array of JSON object samples.
 * Returns array of pfield_t and sets *out_order, *out_count.
 */
static pfield_t *learn_fields(cJSON **samples, int n_samples,
                              char ***out_order, int *out_count) {
    if (n_samples <= 0) {
        *out_order = NULL;
        *out_count = 0;
        return NULL;
    }

    /* Pass 1: collect all field names and their values */
    typedef struct {
        char   *name;
        cJSON **values;
        int     count;
        int     cap;
        int     appearances;  /* how many samples contain this field */
    } finfo_t;

    finfo_t *finfos = NULL;
    int n_fields = 0;
    int finfos_cap = 0;

    for (int si = 0; si < n_samples; si++) {
        cJSON *obj = samples[si];
        if (!obj || !cJSON_IsObject(obj)) continue;

        cJSON *child = obj->child;
        while (child) {
            /* Find or create field info */
            int fi = -1;
            for (int i = 0; i < n_fields; i++) {
                if (strcmp(finfos[i].name, child->string) == 0) {
                    fi = i;
                    break;
                }
            }
            if (fi < 0) {
                if (n_fields >= finfos_cap) {
                    finfos_cap = finfos_cap ? finfos_cap * 2 : 16;
                    finfos = (finfo_t *)realloc(finfos, sizeof(finfo_t) * (size_t)finfos_cap);
                }
                fi = n_fields++;
                finfos[fi].name = pstrdup(child->string);
                finfos[fi].values = NULL;
                finfos[fi].count = 0;
                finfos[fi].cap = 0;
                finfos[fi].appearances = 0;
            }

            /* Add value */
            if (finfos[fi].count >= finfos[fi].cap) {
                finfos[fi].cap = finfos[fi].cap ? finfos[fi].cap * 2 : 8;
                finfos[fi].values = (cJSON **)realloc(finfos[fi].values,
                    sizeof(cJSON *) * (size_t)finfos[fi].cap);
            }
            finfos[fi].values[finfos[fi].count++] = child;
            finfos[fi].appearances++;

            child = child->next;
        }
    }

    /* Pass 2: build pfield_t array */
    int valid_count = 0;
    pfield_t *fields = (pfield_t *)calloc((size_t)(n_fields > 0 ? n_fields : 1), sizeof(pfield_t));

    for (int i = 0; i < n_fields; i++) {
        double freq = (double)finfos[i].appearances / (double)n_samples;
        if (freq < 0.1) {
            /* Skip rare fields */
            free(finfos[i].name);
            free(finfos[i].values);
            continue;
        }

        char **enum_vals = NULL;
        int enum_count = 0;
        int dtype = infer_dtype(finfos[i].values, finfos[i].count, &enum_vals, &enum_count);

        /* Check nullability */
        int has_null = 0;
        for (int j = 0; j < finfos[i].count; j++) {
            if (cJSON_IsNull(finfos[i].values[j])) { has_null = 1; break; }
        }
        int nullable = has_null || (freq < 1.0);

        pfield_t *f = &fields[valid_count];
        f->name = finfos[i].name;
        f->dtype = dtype;
        f->nullable = nullable;
        f->frequency = freq;
        f->enum_values = enum_vals;
        f->enum_count = enum_count;
        f->nested = NULL;
        f->nested_order = NULL;
        f->nested_count = 0;
        f->items = NULL;
        f->items_order = NULL;
        f->items_count = 0;

        /* Handle nested objects */
        if (dtype == DTYPE_OBJECT) {
            cJSON **nested_samples = (cJSON **)malloc(sizeof(cJSON *) * (size_t)finfos[i].count);
            int ns_count = 0;
            for (int j = 0; j < finfos[i].count; j++) {
                if (cJSON_IsObject(finfos[i].values[j]))
                    nested_samples[ns_count++] = finfos[i].values[j];
            }
            if (ns_count > 0) {
                f->nested = learn_fields(nested_samples, ns_count,
                                         &f->nested_order, &f->nested_count);
            }
            free(nested_samples);
        }

        /* Handle array of objects */
        if (dtype == DTYPE_ARRAY) {
            cJSON **all_items = NULL;
            int items_count = 0, items_cap = 0;
            int n_obj_items = 0, n_total_items = 0;

            for (int j = 0; j < finfos[i].count; j++) {
                if (!cJSON_IsArray(finfos[i].values[j])) continue;
                cJSON *elem = finfos[i].values[j]->child;
                while (elem) {
                    n_total_items++;
                    if (cJSON_IsObject(elem)) n_obj_items++;
                    if (items_count >= items_cap) {
                        items_cap = items_cap ? items_cap * 2 : 16;
                        all_items = (cJSON **)realloc(all_items, sizeof(cJSON *) * (size_t)items_cap);
                    }
                    all_items[items_count++] = elem;
                    elem = elem->next;
                }
            }

            if (n_obj_items > 0 && n_obj_items > n_total_items / 2) {
                /* Filter to object items only */
                cJSON **obj_items = (cJSON **)malloc(sizeof(cJSON *) * (size_t)n_obj_items);
                int oi = 0;
                for (int j = 0; j < items_count; j++) {
                    if (cJSON_IsObject(all_items[j]))
                        obj_items[oi++] = all_items[j];
                }
                f->items = learn_fields(obj_items, oi,
                                        &f->items_order, &f->items_count);
                free(obj_items);
            }
            free(all_items);
        }

        free(finfos[i].values);
        valid_count++;
    }

    /* Free skipped field infos */
    /* (already freed in the loop above for kept fields) */

    free(finfos);

    /* Build field_order: sorted by frequency desc, then alphabetical */
    char **order = (char **)malloc(sizeof(char *) * (size_t)(valid_count > 0 ? valid_count : 1));
    for (int i = 0; i < valid_count; i++)
        order[i] = fields[i].name;  /* borrow pointer for sorting */

    sort_ctx_t ctx = { fields, valid_count };
    g_sort_ctx = &ctx;
    qsort(order, (size_t)valid_count, sizeof(char *), field_order_cmp);
    g_sort_ctx = NULL;

    /* Now duplicate the names for the order array (they're owned by fields) */
    char **final_order = (char **)malloc(sizeof(char *) * (size_t)(valid_count > 0 ? valid_count : 1));
    for (int i = 0; i < valid_count; i++)
        final_order[i] = pstrdup(order[i]);
    free(order);

    *out_order = final_order;
    *out_count = valid_count;
    return fields;
}

/* ================================================================
 * Encoding
 * ================================================================ */

static pfield_t *find_field(pfield_t *fields, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(fields[i].name, name) == 0)
            return &fields[i];
    }
    return NULL;
}

static int encode_value(pbuf_t *b, cJSON *val, pfield_t *schema);

static int encode_object(pbuf_t *b, cJSON *obj, pfield_t *fields,
                         char **order, int field_count) {
    if (!obj || !cJSON_IsObject(obj)) {
        /* Encode as empty: all fields absent + zero unknown */
        for (int i = 0; i < field_count; i++) {
            pfield_t *f = find_field(fields, field_count, order[i]);
            if (f && f->nullable) {
                if (pbuf_u8(b, MARKER_ABSENT)) return -1;
            }
        }
        return pbuf_u16be(b, 0);
    }

    /* Encode known fields in order */
    for (int i = 0; i < field_count; i++) {
        pfield_t *schema = find_field(fields, field_count, order[i]);
        if (!schema) continue;

        cJSON *val = cJSON_GetObjectItem(obj, order[i]);

        if (schema->nullable) {
            if (!val) {
                /* Field absent from object */
                if (pbuf_u8(b, MARKER_ABSENT)) return -1;
                continue;
            }
            if (cJSON_IsNull(val)) {
                if (pbuf_u8(b, MARKER_NULL)) return -1;
                continue;
            }
            if (pbuf_u8(b, MARKER_PRESENT)) return -1;
        }

        if (encode_value(b, val, schema)) return -1;
    }

    /* Unknown fields: collect fields not in schema */
    cJSON *unknown = cJSON_CreateObject();
    cJSON *child = obj->child;
    while (child) {
        if (!find_field(fields, field_count, child->string)) {
            /* Clone the value into unknown object */
            char *printed = cJSON_PrintUnformatted(child);
            if (printed) {
                cJSON *val_copy = cJSON_Parse(printed);
                free(printed);
                if (val_copy) {
                    cJSON_AddItemToObject(unknown, child->string, val_copy);
                }
            }
        }
        child = child->next;
    }

    int unk_size = cJSON_GetArraySize(unknown);
    if (unk_size > 0) {
        char *fallback = cJSON_PrintUnformatted(unknown);
        cJSON_Delete(unknown);
        if (!fallback) return -1;
        size_t fb_len = strlen(fallback);
        if (pbuf_u16be(b, (uint16_t)fb_len)) { free(fallback); return -1; }
        if (pbuf_write(b, fallback, fb_len)) { free(fallback); return -1; }
        free(fallback);
    } else {
        cJSON_Delete(unknown);
        if (pbuf_u16be(b, 0)) return -1;
    }

    return 0;
}

/*
 * Does this value fit the field schema so that it decodes back unchanged?
 * A missing required field, a wrong type, an integer outside int range or
 * a length the wire format cannot hold would otherwise crash the encoder
 * (NULL value) or decode to different data. The caller then fails the
 * whole compress call and the server sends plain JSON.
 */
static int value_fits(const cJSON *val, const pfield_t *schema) {
    if (!val) return 0;
    switch (schema->dtype) {
    case DTYPE_BOOL:
        return cJSON_IsBool(val);
    case DTYPE_INT:
        return cJSON_IsNumber(val) && val->valuedouble == (double)val->valueint;
    case DTYPE_FLOAT:
        return cJSON_IsNumber(val);
    case DTYPE_STR:
    case DTYPE_ENUM:
        return cJSON_IsString(val) && val->valuestring
            && strlen(val->valuestring) <= 0xFFFF;
    case DTYPE_TIMESTAMP:
        return cJSON_IsString(val) && val->valuestring
            && strlen(val->valuestring) <= 0xFF;
    case DTYPE_OBJECT:
        if (!schema->nested) return 1;  /* JSON fallback */
        return cJSON_IsObject(val);
    case DTYPE_ARRAY: {
        if (!schema->items) return 1;  /* JSON fallback */
        if (!cJSON_IsArray(val) || cJSON_GetArraySize(val) > 0xFFFF) return 0;
        for (const cJSON *it = val->child; it; it = it->next) {
            if (!cJSON_IsObject(it)) return 0;
        }
        return 1;
    }
    default:
        return 1;  /* written as JSON fallback */
    }
}

static int encode_value(pbuf_t *b, cJSON *val, pfield_t *schema) {
    if (!value_fits(val, schema)) return -1;
    switch (schema->dtype) {
    case DTYPE_BOOL:
        return pbuf_u8(b, cJSON_IsTrue(val) ? 1 : 0);

    case DTYPE_INT:
        return pbuf_varint(b, val->valueint);

    case DTYPE_FLOAT:
        return pbuf_double(b, val->valuedouble);

    case DTYPE_STR: {
        const char *s = val->valuestring ? val->valuestring : "";
        size_t slen = strlen(s);
        if (pbuf_u16be(b, (uint16_t)slen)) return -1;
        return pbuf_write(b, s, slen);
    }

    case DTYPE_ENUM: {
        const char *s = val->valuestring ? val->valuestring : "";
        /* Find index */
        for (int i = 0; i < schema->enum_count; i++) {
            if (strcmp(schema->enum_values[i], s) == 0) {
                return pbuf_u8(b, (uint8_t)i);
            }
        }
        /* Unknown enum — 0xFE marker + string */
        if (pbuf_u8(b, 0xFE)) return -1;
        size_t slen = strlen(s);
        if (pbuf_u16be(b, (uint16_t)slen)) return -1;
        return pbuf_write(b, s, slen);
    }

    case DTYPE_TIMESTAMP: {
        const char *s = val->valuestring ? val->valuestring : "";
        size_t slen = strlen(s);
        if (pbuf_u8(b, (uint8_t)slen)) return -1;
        return pbuf_write(b, s, slen);
    }

    case DTYPE_OBJECT:
        /* Without a nested schema the value goes as JSON, as in Python */
        if (!schema->nested) goto json_fallback;
        return encode_object(b, val, schema->nested,
                            schema->nested_order, schema->nested_count);

    case DTYPE_ARRAY:
        /* Arrays of plain values have no items schema: JSON, as in Python */
        if (!schema->items) goto json_fallback;
        if (cJSON_IsArray(val)) {
            int count = cJSON_GetArraySize(val);
            if (pbuf_u16be(b, (uint16_t)count)) return -1;
            for (int i = 0; i < count; i++) {
                cJSON *item = cJSON_GetArrayItem(val, i);
                if (cJSON_IsObject(item)) {
                    if (encode_object(b, item, schema->items,
                                      schema->items_order, schema->items_count))
                        return -1;
                } else {
                    /* Primitive array item — JSON fallback */
                    char *s = cJSON_PrintUnformatted(item);
                    if (!s) return -1;
                    size_t slen = strlen(s);
                    if (pbuf_u16be(b, (uint16_t)slen)) { free(s); return -1; }
                    if (pbuf_write(b, s, slen)) { free(s); return -1; }
                    free(s);
                }
            }
            return 0;
        } else {
            return pbuf_u16be(b, 0);
        }

    default:
    json_fallback: {
        /* JSON fallback */
        char *s = cJSON_PrintUnformatted(val);
        if (!s) return -1;
        size_t slen = strlen(s);
        if (pbuf_u16be(b, (uint16_t)slen)) { free(s); return -1; }
        if (pbuf_write(b, s, slen)) { free(s); return -1; }
        free(s);
        return 0;
    }
    }
}

/* ================================================================
 * Decoding
 * ================================================================ */

static cJSON *decode_value(rdbuf_t *r, pfield_t *schema);

static cJSON *decode_object(rdbuf_t *r, pfield_t *fields,
                            char **order, int field_count) {
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    for (int i = 0; i < field_count; i++) {
        pfield_t *schema = find_field(fields, field_count, order[i]);
        if (!schema) continue;

        if (schema->nullable) {
            uint8_t marker;
            if (rd_u8(r, &marker)) { cJSON_Delete(result); return NULL; }
            if (marker == MARKER_NULL) {
                cJSON_AddItemToObject(result, order[i], cJSON_CreateNull());
                continue;
            } else if (marker == MARKER_ABSENT) {
                continue;
            }
            /* MARKER_PRESENT — fall through */
        }

        cJSON *val = decode_value(r, schema);
        if (!val) { cJSON_Delete(result); return NULL; }
        cJSON_AddItemToObject(result, order[i], val);
    }

    /* Unknown fields fallback */
    uint16_t fb_len;
    if (rd_u16be(r, &fb_len)) { cJSON_Delete(result); return NULL; }
    if (fb_len > 0) {
        const uint8_t *fb_data;
        if (rd_bytes(r, &fb_data, fb_len)) { cJSON_Delete(result); return NULL; }
        /* Parse the JSON fallback and merge */
        char *fb_str = (char *)malloc(fb_len + 1);
        if (fb_str) {
            memcpy(fb_str, fb_data, fb_len);
            fb_str[fb_len] = '\0';
            cJSON *fb = cJSON_Parse(fb_str);
            free(fb_str);
            if (fb && cJSON_IsObject(fb)) {
                cJSON *child = fb->child;
                while (child) {
                    cJSON *next = child->next;
                    /* Detach and move to result */
                    char *printed = cJSON_PrintUnformatted(child);
                    if (printed) {
                        cJSON *val_copy = cJSON_Parse(printed);
                        free(printed);
                        if (val_copy)
                            cJSON_AddItemToObject(result, child->string, val_copy);
                    }
                    child = next;
                }
                cJSON_Delete(fb);
            } else if (fb) {
                cJSON_Delete(fb);
            }
        }
    }

    return result;
}

static cJSON *decode_value(rdbuf_t *r, pfield_t *schema) {
    switch (schema->dtype) {
    case DTYPE_BOOL: {
        uint8_t v;
        if (rd_u8(r, &v)) return NULL;
        return v ? cJSON_CreateTrue() : cJSON_CreateFalse();
    }

    case DTYPE_INT: {
        int64_t v;
        if (rd_varint(r, &v)) return NULL;
        cJSON *item = cJSON_CreateNumber((double)v);
        if (item) item->valueint = v;
        return item;
    }

    case DTYPE_FLOAT: {
        double v;
        if (rd_double(r, &v)) return NULL;
        return cJSON_CreateNumber(v);
    }

    case DTYPE_STR: {
        uint16_t slen;
        if (rd_u16be(r, &slen)) return NULL;
        const uint8_t *data;
        if (rd_bytes(r, &data, slen)) return NULL;
        char *s = (char *)malloc(slen + 1);
        if (!s) return NULL;
        memcpy(s, data, slen);
        s[slen] = '\0';
        cJSON *item = cJSON_CreateString(s);
        free(s);
        return item;
    }

    case DTYPE_ENUM: {
        uint8_t idx;
        if (rd_u8(r, &idx)) return NULL;
        if (idx == 0xFE) {
            /* Unknown enum — read string */
            uint16_t slen;
            if (rd_u16be(r, &slen)) return NULL;
            const uint8_t *data;
            if (rd_bytes(r, &data, slen)) return NULL;
            char *s = (char *)malloc(slen + 1);
            if (!s) return NULL;
            memcpy(s, data, slen);
            s[slen] = '\0';
            cJSON *item = cJSON_CreateString(s);
            free(s);
            return item;
        }
        if (idx < schema->enum_count) {
            return cJSON_CreateString(schema->enum_values[idx]);
        }
        return cJSON_CreateNull();
    }

    case DTYPE_TIMESTAMP: {
        uint8_t slen;
        if (rd_u8(r, &slen)) return NULL;
        const uint8_t *data;
        if (rd_bytes(r, &data, slen)) return NULL;
        char *s = (char *)malloc(slen + 1);
        if (!s) return NULL;
        memcpy(s, data, slen);
        s[slen] = '\0';
        cJSON *item = cJSON_CreateString(s);
        free(s);
        return item;
    }

    case DTYPE_OBJECT:
        if (!schema->nested) goto json_fallback;
        return decode_object(r, schema->nested,
                            schema->nested_order, schema->nested_count);

    case DTYPE_ARRAY:
        if (!schema->items) goto json_fallback;
        {
            uint16_t count;
            if (rd_u16be(r, &count)) return NULL;
            cJSON *arr = cJSON_CreateArray();
            if (!arr) return NULL;
            for (int i = 0; i < count; i++) {
                cJSON *item = decode_object(r, schema->items,
                                            schema->items_order, schema->items_count);
                if (!item) { cJSON_Delete(arr); return NULL; }
                cJSON_AddItemToArray(arr, item);
            }
            return arr;
        }

    default:
    json_fallback: {
        /* JSON fallback */
        uint16_t slen;
        if (rd_u16be(r, &slen)) return NULL;
        const uint8_t *data;
        if (rd_bytes(r, &data, slen)) return NULL;
        char *s = (char *)malloc(slen + 1);
        if (!s) return NULL;
        memcpy(s, data, slen);
        s[slen] = '\0';
        cJSON *item = cJSON_Parse(s);
        free(s);
        return item ? item : cJSON_CreateNull();
    }
    }
}

/* ================================================================
 * Brotli compression wrapper
 * ================================================================ */

#if HAS_BROTLI
static int brotli_compress_buf(const uint8_t *input, size_t input_len,
                               uint8_t **output, size_t *output_len) {
    size_t max_out = BrotliEncoderMaxCompressedSize(input_len);
    if (max_out == 0) max_out = input_len + 16;
    *output = (uint8_t *)malloc(max_out);
    if (!*output) return -1;
    *output_len = max_out;
    if (BrotliEncoderCompress(6, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
                              input_len, input, output_len, *output)
        != BROTLI_TRUE) {
        free(*output);
        *output = NULL;
        return -1;
    }
    return 0;
}

static int brotli_decompress_buf(const uint8_t *input, size_t input_len,
                                 uint8_t **output, size_t *output_len) {
    /* Start with 4x input size, grow if needed */
    size_t out_cap = input_len * 4;
    if (out_cap < 256) out_cap = 256;
    *output = (uint8_t *)malloc(out_cap);
    if (!*output) return -1;

    BrotliDecoderState *state = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!state) { free(*output); return -1; }

    size_t available_in = input_len;
    const uint8_t *next_in = input;
    size_t available_out = out_cap;
    uint8_t *next_out = *output;
    size_t total_out = 0;

    BrotliDecoderResult result;
    for (;;) {
        result = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                               &available_out, &next_out, &total_out);
        if (result == BROTLI_DECODER_RESULT_SUCCESS) break;
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            size_t written = next_out - *output;
            out_cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(*output, out_cap);
            if (!nb) {
                BrotliDecoderDestroyInstance(state);
                free(*output);
                return -1;
            }
            *output = nb;
            next_out = nb + written;
            available_out = out_cap - written;
            continue;
        }
        /* Error */
        BrotliDecoderDestroyInstance(state);
        free(*output);
        *output = NULL;
        return -1;
    }

    BrotliDecoderDestroyInstance(state);
    *output_len = total_out;
    return 0;
}
#endif /* HAS_BROTLI */

/* ================================================================
 * Profile API
 * ================================================================ */

pidgin_profile_t *pidgin_learn(const char *json_samples) {
    if (!json_samples) return NULL;

    cJSON *root = cJSON_Parse(json_samples);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return NULL;
    }

    int n = cJSON_GetArraySize(root);
    if (n == 0) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON **samples = (cJSON **)malloc(sizeof(cJSON *) * (size_t)n);
    for (int i = 0; i < n; i++)
        samples[i] = cJSON_GetArrayItem(root, i);

    pidgin_profile_t *profile = (pidgin_profile_t *)calloc(1, sizeof(pidgin_profile_t));
    profile->version = 1;
    profile->fields = learn_fields(samples, n, &profile->field_order, &profile->field_count);

    free(samples);
    cJSON_Delete(root);
    return profile;
}

pidgin_buf_t pidgin_compress(pidgin_profile_t *profile, const char *json) {
    pidgin_buf_t result = { NULL, 0 };
    if (!profile || !json) return result;

    cJSON *root = cJSON_Parse(json);
    if (!root) return result;

    pbuf_t buf;
    if (pbuf_init(&buf, 256)) {
        cJSON_Delete(root);
        return result;
    }

    if (cJSON_IsArray(root)) {
        pbuf_u8(&buf, 0x02);  /* array marker */
        int count = cJSON_GetArraySize(root);
        if (count > 0xFFFF) {  /* count is written as 16 bits */
            pbuf_free(&buf);
            cJSON_Delete(root);
            return result;
        }
        pbuf_u16be(&buf, (uint16_t)count);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(root, i);
            if (encode_object(&buf, item, profile->fields,
                              profile->field_order, profile->field_count)) {
                pbuf_free(&buf);
                cJSON_Delete(root);
                return result;
            }
        }
    } else {
        pbuf_u8(&buf, 0x01);  /* single object marker */
        if (encode_object(&buf, root, profile->fields,
                          profile->field_order, profile->field_count)) {
            pbuf_free(&buf);
            cJSON_Delete(root);
            return result;
        }
    }

    cJSON_Delete(root);

    /* Try brotli compression */
#if HAS_BROTLI
    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    if (brotli_compress_buf(buf.data, buf.len, &compressed, &compressed_len) == 0
        && compressed_len < buf.len) {
        /* Brotli is smaller — use it */
        result.data = (uint8_t *)malloc(1 + compressed_len);
        if (result.data) {
            result.data[0] = 0x01;  /* brotli flag */
            memcpy(result.data + 1, compressed, compressed_len);
            result.len = 1 + compressed_len;
        }
        free(compressed);
        pbuf_free(&buf);
        return result;
    }
    if (compressed) free(compressed);
#endif

    /* Raw output (no brotli or brotli didn't help) */
    result.data = (uint8_t *)malloc(1 + buf.len);
    if (result.data) {
        result.data[0] = 0x00;  /* no brotli */
        memcpy(result.data + 1, buf.data, buf.len);
        result.len = 1 + buf.len;
    }
    pbuf_free(&buf);
    return result;
}

char *pidgin_decompress(pidgin_profile_t *profile, const uint8_t *data, size_t len) {
    if (!profile || !data || len < 2) return NULL;

    uint8_t brotli_flag = data[0];
    const uint8_t *payload = data + 1;
    size_t payload_len = len - 1;

    uint8_t *decompressed = NULL;

    if (brotli_flag == 0x01) {
#if HAS_BROTLI
        size_t dec_len = 0;
        if (brotli_decompress_buf(payload, payload_len, &decompressed, &dec_len) != 0)
            return NULL;
        payload = decompressed;
        payload_len = dec_len;
#else
        return NULL;  /* brotli required but not available */
#endif
    }

    rdbuf_t r = { payload, payload_len, 0 };

    uint8_t type_marker;
    if (rd_u8(&r, &type_marker)) {
        if (decompressed) free(decompressed);
        return NULL;
    }

    cJSON *result = NULL;

    if (type_marker == 0x02) {
        /* Array of objects */
        uint16_t count;
        if (rd_u16be(&r, &count)) {
            if (decompressed) free(decompressed);
            return NULL;
        }
        result = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON *obj = decode_object(&r, profile->fields,
                                       profile->field_order, profile->field_count);
            if (!obj) {
                cJSON_Delete(result);
                if (decompressed) free(decompressed);
                return NULL;
            }
            cJSON_AddItemToArray(result, obj);
        }
    } else {
        /* Single object */
        result = decode_object(&r, profile->fields,
                               profile->field_order, profile->field_count);
    }

    if (decompressed) free(decompressed);

    if (!result) return NULL;

    char *json_out = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json_out;
}

/* ================================================================
 * Schema Evolution
 * ================================================================ */

static pfield_t *clone_fields(const pfield_t *src, int count) {
    if (!src || count <= 0) return NULL;
    pfield_t *dst = (pfield_t *)calloc((size_t)count, sizeof(pfield_t));
    for (int i = 0; i < count; i++) {
        dst[i].name = pstrdup(src[i].name);
        dst[i].dtype = src[i].dtype;
        dst[i].nullable = src[i].nullable;
        dst[i].frequency = src[i].frequency;

        if (src[i].enum_values) {
            dst[i].enum_values = (char **)malloc(sizeof(char *) * (size_t)src[i].enum_count);
            dst[i].enum_count = src[i].enum_count;
            for (int j = 0; j < src[i].enum_count; j++)
                dst[i].enum_values[j] = pstrdup(src[i].enum_values[j]);
        }

        if (src[i].nested) {
            dst[i].nested = clone_fields(src[i].nested, src[i].nested_count);
            dst[i].nested_count = src[i].nested_count;
            dst[i].nested_order = clone_order(src[i].nested_order, src[i].nested_count);
        }
        if (src[i].items) {
            dst[i].items = clone_fields(src[i].items, src[i].items_count);
            dst[i].items_count = src[i].items_count;
            dst[i].items_order = clone_order(src[i].items_order, src[i].items_count);
        }
    }
    return dst;
}

static char **clone_order(char **src, int count) {
    if (!src || count <= 0) return NULL;
    char **dst = (char **)malloc(sizeof(char *) * (size_t)count);
    for (int i = 0; i < count; i++)
        dst[i] = pstrdup(src[i]);
    return dst;
}

static pfield_t merge_field(const pfield_t *old, const pfield_t *new_f) {
    pfield_t merged;
    memset(&merged, 0, sizeof(merged));

    merged.name = pstrdup(old->name);

    /* Type widening: int -> float is safe */
    if (old->dtype == DTYPE_INT && new_f->dtype == DTYPE_FLOAT)
        merged.dtype = DTYPE_FLOAT;
    else
        merged.dtype = old->dtype;

    /* Once nullable, stays nullable */
    merged.nullable = old->nullable || new_f->nullable;

    /* Frequency: use new observation */
    merged.frequency = new_f->frequency;

    /* Enum values: preserve old indices, append new */
    if (old->enum_values || new_f->enum_values) {
        if (old->enum_values && new_f->enum_values) {
            /* Start with old, append new */
            int total_cap = old->enum_count + new_f->enum_count;
            merged.enum_values = (char **)malloc(sizeof(char *) * (size_t)total_cap);
            merged.enum_count = old->enum_count;
            for (int i = 0; i < old->enum_count; i++)
                merged.enum_values[i] = pstrdup(old->enum_values[i]);
            for (int i = 0; i < new_f->enum_count; i++) {
                int found = 0;
                for (int j = 0; j < merged.enum_count; j++) {
                    if (strcmp(merged.enum_values[j], new_f->enum_values[i]) == 0) {
                        found = 1; break;
                    }
                }
                if (!found) {
                    merged.enum_values[merged.enum_count++] = pstrdup(new_f->enum_values[i]);
                }
            }
        } else if (old->enum_values) {
            merged.enum_values = (char **)malloc(sizeof(char *) * (size_t)old->enum_count);
            merged.enum_count = old->enum_count;
            for (int i = 0; i < old->enum_count; i++)
                merged.enum_values[i] = pstrdup(old->enum_values[i]);
        } else {
            merged.enum_values = (char **)malloc(sizeof(char *) * (size_t)new_f->enum_count);
            merged.enum_count = new_f->enum_count;
            for (int i = 0; i < new_f->enum_count; i++)
                merged.enum_values[i] = pstrdup(new_f->enum_values[i]);
        }
    }

    /* Nested: TODO recursive merge (simplified: take newer if both exist) */
    if (new_f->nested) {
        merged.nested = clone_fields(new_f->nested, new_f->nested_count);
        merged.nested_count = new_f->nested_count;
        merged.nested_order = clone_order(new_f->nested_order, new_f->nested_count);
    } else if (old->nested) {
        merged.nested = clone_fields(old->nested, old->nested_count);
        merged.nested_count = old->nested_count;
        merged.nested_order = clone_order(old->nested_order, old->nested_count);
    }

    if (new_f->items) {
        merged.items = clone_fields(new_f->items, new_f->items_count);
        merged.items_count = new_f->items_count;
        merged.items_order = clone_order(new_f->items_order, new_f->items_count);
    } else if (old->items) {
        merged.items = clone_fields(old->items, old->items_count);
        merged.items_count = old->items_count;
        merged.items_order = clone_order(old->items_order, old->items_count);
    }

    return merged;
}

pidgin_profile_t *pidgin_evolve(pidgin_profile_t *old, const char *new_samples_json) {
    if (!old || !new_samples_json) return NULL;

    /* Learn new schema */
    pidgin_profile_t *new_prof = pidgin_learn(new_samples_json);
    if (!new_prof) return NULL;

    int total_cap = old->field_count + new_prof->field_count;
    pfield_t *evolved_fields = (pfield_t *)calloc((size_t)total_cap, sizeof(pfield_t));
    char **evolved_order = (char **)malloc(sizeof(char *) * (size_t)total_cap);
    int evolved_count = 0;

    /* Copy existing field_order first */
    int order_count = 0;
    for (int i = 0; i < old->field_count; i++) {
        evolved_order[order_count++] = pstrdup(old->field_order[i]);
    }

    /* Process existing fields */
    for (int i = 0; i < old->field_count; i++) {
        const char *key = old->field_order[i];
        pfield_t *old_f = find_field(old->fields, old->field_count, key);
        pfield_t *new_f = find_field(new_prof->fields, new_prof->field_count, key);

        if (old_f && new_f) {
            evolved_fields[evolved_count++] = merge_field(old_f, new_f);
        } else if (old_f) {
            /* Field removed in new data — keep but nullable with freq=0 */
            pfield_t *cloned = clone_fields(old_f, 1);
            evolved_fields[evolved_count] = *cloned;
            evolved_fields[evolved_count].nullable = 1;
            evolved_fields[evolved_count].frequency = 0.0;
            free(cloned);  /* just the array, contents moved */
            evolved_count++;
        }
    }

    /* Append new fields not in old schema */
    for (int i = 0; i < new_prof->field_count; i++) {
        const char *key = new_prof->field_order[i];
        if (!find_field(old->fields, old->field_count, key)) {
            pfield_t *new_f = find_field(new_prof->fields, new_prof->field_count, key);
            if (new_f) {
                pfield_t *cloned = clone_fields(new_f, 1);
                evolved_fields[evolved_count] = *cloned;
                evolved_fields[evolved_count].nullable = 1;  /* new fields always nullable */
                free(cloned);
                evolved_count++;
                evolved_order[order_count++] = pstrdup(key);
            }
        }
    }

    pidgin_profile_t *result = (pidgin_profile_t *)calloc(1, sizeof(pidgin_profile_t));
    result->fields = evolved_fields;
    result->field_count = evolved_count;
    result->field_order = evolved_order;
    result->version = old->version + 1;

    /* Trim excess capacity */
    /* (field_order may have unused slots but that's OK — field_count is correct) */

    pidgin_profile_free(new_prof);
    return result;
}

/* ================================================================
 * Profile Serialization (to/from JSON)
 * ================================================================ */

static cJSON *field_to_json(const pfield_t *f) {
    cJSON *obj = cJSON_CreateObject();

    const char *dtype_str = "str";
    switch (f->dtype) {
        case DTYPE_BOOL:      dtype_str = "bool"; break;
        case DTYPE_INT:       dtype_str = "int"; break;
        case DTYPE_FLOAT:     dtype_str = "float"; break;
        case DTYPE_STR:       dtype_str = "str"; break;
        case DTYPE_ENUM:      dtype_str = "enum"; break;
        case DTYPE_TIMESTAMP: dtype_str = "timestamp"; break;
        case DTYPE_OBJECT:    dtype_str = "object"; break;
        case DTYPE_ARRAY:     dtype_str = "array"; break;
        case DTYPE_NULL:      dtype_str = "null"; break;
    }
    cJSON_AddItemToObject(obj, "dtype", cJSON_CreateString(dtype_str));
    cJSON_AddItemToObject(obj, "nullable", cJSON_CreateBool(f->nullable));
    cJSON_AddItemToObject(obj, "freq", cJSON_CreateNumber(f->frequency));

    if (f->enum_values && f->enum_count > 0) {
        cJSON *earr = cJSON_CreateArray();
        for (int i = 0; i < f->enum_count; i++)
            cJSON_AddItemToArray(earr, cJSON_CreateString(f->enum_values[i]));
        cJSON_AddItemToObject(obj, "enum", earr);
    }

    if (f->nested && f->nested_count > 0) {
        cJSON *nobj = cJSON_CreateObject();
        for (int i = 0; i < f->nested_count; i++)
            cJSON_AddItemToObject(nobj, f->nested[i].name, field_to_json(&f->nested[i]));
        cJSON_AddItemToObject(obj, "nested", nobj);
    }

    if (f->items && f->items_count > 0) {
        cJSON *iobj = cJSON_CreateObject();
        for (int i = 0; i < f->items_count; i++)
            cJSON_AddItemToObject(iobj, f->items[i].name, field_to_json(&f->items[i]));
        cJSON_AddItemToObject(obj, "items", iobj);
    }

    return obj;
}

char *pidgin_profile_to_json(pidgin_profile_t *profile) {
    if (!profile) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "version", cJSON_CreateNumber(profile->version));

    cJSON *order_arr = cJSON_CreateArray();
    for (int i = 0; i < profile->field_count; i++)
        cJSON_AddItemToArray(order_arr, cJSON_CreateString(profile->field_order[i]));
    cJSON_AddItemToObject(root, "field_order", order_arr);

    cJSON *fields_obj = cJSON_CreateObject();
    for (int i = 0; i < profile->field_count; i++) {
        cJSON_AddItemToObject(fields_obj, profile->fields[i].name,
                              field_to_json(&profile->fields[i]));
    }
    cJSON_AddItemToObject(root, "fields", fields_obj);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static int dtype_from_string(const char *s) {
    if (!s) return DTYPE_STR;
    if (strcmp(s, "bool") == 0) return DTYPE_BOOL;
    if (strcmp(s, "int") == 0) return DTYPE_INT;
    if (strcmp(s, "float") == 0) return DTYPE_FLOAT;
    if (strcmp(s, "str") == 0) return DTYPE_STR;
    if (strcmp(s, "enum") == 0) return DTYPE_ENUM;
    if (strcmp(s, "timestamp") == 0) return DTYPE_TIMESTAMP;
    if (strcmp(s, "object") == 0) return DTYPE_OBJECT;
    if (strcmp(s, "array") == 0) return DTYPE_ARRAY;
    if (strcmp(s, "null") == 0) return DTYPE_NULL;
    return DTYPE_STR;
}

static pfield_t field_from_json(const char *name, cJSON *jf) {
    pfield_t f;
    memset(&f, 0, sizeof(f));

    f.name = pstrdup(name);

    cJSON *jdtype = cJSON_GetObjectItem(jf, "dtype");
    f.dtype = dtype_from_string(jdtype && cJSON_IsString(jdtype) ? jdtype->valuestring : "str");

    cJSON *jnull = cJSON_GetObjectItem(jf, "nullable");
    f.nullable = (jnull && cJSON_IsTrue(jnull)) ? 1 : 0;

    cJSON *jfreq = cJSON_GetObjectItem(jf, "freq");
    f.frequency = (jfreq && cJSON_IsNumber(jfreq)) ? jfreq->valuedouble : 1.0;

    /* Enum values */
    cJSON *jenum = cJSON_GetObjectItem(jf, "enum");
    if (jenum && cJSON_IsArray(jenum)) {
        f.enum_count = cJSON_GetArraySize(jenum);
        f.enum_values = (char **)malloc(sizeof(char *) * (size_t)f.enum_count);
        for (int i = 0; i < f.enum_count; i++) {
            cJSON *ev = cJSON_GetArrayItem(jenum, i);
            f.enum_values[i] = pstrdup(ev && cJSON_IsString(ev) ? ev->valuestring : "");
        }
    }

    /* Nested */
    cJSON *jnested = cJSON_GetObjectItem(jf, "nested");
    if (jnested && cJSON_IsObject(jnested)) {
        int nc = cJSON_GetArraySize(jnested);
        f.nested = (pfield_t *)calloc((size_t)(nc > 0 ? nc : 1), sizeof(pfield_t));
        f.nested_order = (char **)malloc(sizeof(char *) * (size_t)(nc > 0 ? nc : 1));
        f.nested_count = 0;

        /* Build nested fields and order (sorted by freq desc, then alpha) */
        cJSON *nchild = jnested->child;
        while (nchild) {
            f.nested[f.nested_count] = field_from_json(nchild->string, nchild);
            f.nested_count++;
            nchild = nchild->next;
        }

        /* Sort order */
        for (int i = 0; i < f.nested_count; i++)
            f.nested_order[i] = f.nested[i].name;

        sort_ctx_t ctx = { f.nested, f.nested_count };
        g_sort_ctx = &ctx;
        qsort(f.nested_order, (size_t)f.nested_count, sizeof(char *), field_order_cmp);
        g_sort_ctx = NULL;

        /* Duplicate order strings */
        for (int i = 0; i < f.nested_count; i++)
            f.nested_order[i] = pstrdup(f.nested_order[i]);
    }

    /* Items */
    cJSON *jitems = cJSON_GetObjectItem(jf, "items");
    if (jitems && cJSON_IsObject(jitems)) {
        int ic = cJSON_GetArraySize(jitems);
        f.items = (pfield_t *)calloc((size_t)(ic > 0 ? ic : 1), sizeof(pfield_t));
        f.items_order = (char **)malloc(sizeof(char *) * (size_t)(ic > 0 ? ic : 1));
        f.items_count = 0;

        cJSON *ichild = jitems->child;
        while (ichild) {
            f.items[f.items_count] = field_from_json(ichild->string, ichild);
            f.items_count++;
            ichild = ichild->next;
        }

        for (int i = 0; i < f.items_count; i++)
            f.items_order[i] = f.items[i].name;

        sort_ctx_t ctx = { f.items, f.items_count };
        g_sort_ctx = &ctx;
        qsort(f.items_order, (size_t)f.items_count, sizeof(char *), field_order_cmp);
        g_sort_ctx = NULL;

        for (int i = 0; i < f.items_count; i++)
            f.items_order[i] = pstrdup(f.items_order[i]);
    }

    return f;
}

pidgin_profile_t *pidgin_profile_from_json(const char *json) {
    if (!json) return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return NULL;
    }

    cJSON *jversion = cJSON_GetObjectItem(root, "version");
    cJSON *jorder = cJSON_GetObjectItem(root, "field_order");
    cJSON *jfields = cJSON_GetObjectItem(root, "fields");

    if (!jorder || !cJSON_IsArray(jorder) || !jfields || !cJSON_IsObject(jfields)) {
        cJSON_Delete(root);
        return NULL;
    }

    pidgin_profile_t *profile = (pidgin_profile_t *)calloc(1, sizeof(pidgin_profile_t));
    profile->version = (jversion && cJSON_IsNumber(jversion)) ? (int)jversion->valueint : 1;

    /* field_order from JSON array */
    int order_count = cJSON_GetArraySize(jorder);
    profile->field_order = (char **)malloc(sizeof(char *) * (size_t)(order_count > 0 ? order_count : 1));
    for (int i = 0; i < order_count; i++) {
        cJSON *item = cJSON_GetArrayItem(jorder, i);
        profile->field_order[i] = pstrdup(item && cJSON_IsString(item) ? item->valuestring : "");
    }

    /* fields from JSON object */
    int fc = cJSON_GetArraySize(jfields);
    profile->fields = (pfield_t *)calloc((size_t)(fc > 0 ? fc : 1), sizeof(pfield_t));
    profile->field_count = 0;

    cJSON *child = jfields->child;
    while (child) {
        profile->fields[profile->field_count] = field_from_json(child->string, child);
        profile->field_count++;
        child = child->next;
    }

    cJSON_Delete(root);
    return profile;
}

/* ================================================================
 * Engine API
 * ================================================================ */

static endpoint_state_t *engine_find(pidgin_engine_t *engine, const char *endpoint) {
    endpoint_state_t *ep = engine->endpoints;
    while (ep) {
        if (strcmp(ep->path, endpoint) == 0) return ep;
        ep = ep->next;
    }
    return NULL;
}

static endpoint_state_t *engine_get_or_create(pidgin_engine_t *engine, const char *endpoint) {
    endpoint_state_t *ep = engine_find(engine, endpoint);
    if (ep) return ep;

    ep = (endpoint_state_t *)calloc(1, sizeof(endpoint_state_t));
    ep->path = pstrdup(endpoint);
    ep->sample_cap = engine->learn_samples;
    ep->samples = (char **)malloc(sizeof(char *) * (size_t)ep->sample_cap);

    ep->next = engine->endpoints;
    engine->endpoints = ep;
    return ep;
}

/* === Canonical Bytes Hash (Pidgin Fingerprint Format v1) ===
 *
 * Both libpidgin (C) and the Python codec must produce identical fingerprints
 * for the same logical schema profile, so they must hash the same byte
 * sequence. We define a deterministic binary serialization that does not
 * depend on JSON-library-specific key ordering choices.
 *
 * Format (all integers little-endian, all strings length-prefixed UTF-8):
 *   profile := [version:u32][field_count:u32][field*]
 *   field   := [name_len:u16][name][dtype:u8][nullable:u8]
 *              [enum_count:u16][enum*]
 *              [has_nested:u8] [nested_count:u32 if has_nested][field* if has_nested]
 *              [has_items:u8]  [items_count:u32  if has_items] [field* if has_items]
 *   enum    := [val_len:u16][val]
 *
 * dtype encoding: 0=bool 1=int 2=float 3=str 4=enum 5=timestamp 6=object 7=array 8=null
 *
 * The hash function is FNV-1a 64-bit (non-cryptographic, deterministic, sufficient
 * for cache validation). Output is rendered as a 16-character lowercase hex string,
 * matching the SchemaCodec.fingerprint format on the Python side.
 */

typedef struct { uint8_t *data; size_t len; size_t cap; } cbuf_t;

static int cbuf_grow(cbuf_t *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + need) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p; b->cap = cap;
    return 0;
}
static int cbuf_put(cbuf_t *b, const void *src, size_t n) {
    if (cbuf_grow(b, n)) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}
static int cbuf_u8(cbuf_t *b, uint8_t v)   { return cbuf_put(b, &v, 1); }
static int cbuf_u16(cbuf_t *b, uint16_t v) { uint8_t le[2]={(uint8_t)v,(uint8_t)(v>>8)}; return cbuf_put(b,le,2); }
static int cbuf_u32(cbuf_t *b, uint32_t v) {
    uint8_t le[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    return cbuf_put(b, le, 4);
}
static int cbuf_str(cbuf_t *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) return -1;
    if (cbuf_u16(b, (uint16_t)n)) return -1;
    if (n > 0 && cbuf_put(b, s, n)) return -1;
    return 0;
}

static uint8_t dtype_to_byte(int d) {
    /* keep in sync with header DTYPE_* values */
    switch (d) {
        case DTYPE_BOOL: return 0;
        case DTYPE_INT:  return 1;
        case DTYPE_FLOAT:return 2;
        case DTYPE_STR:  return 3;
        case DTYPE_ENUM: return 4;
        case DTYPE_TIMESTAMP: return 5;
        case DTYPE_OBJECT:return 6;
        case DTYPE_ARRAY: return 7;
        case DTYPE_NULL:  return 8;
        default: return 3; /* str */
    }
}

static int encode_canonical_field(cbuf_t *b, const pfield_t *f);

static int encode_canonical_fields(cbuf_t *b, const pfield_t *fields,
                                    char **order, int count) {
    if (cbuf_u32(b, (uint32_t)count)) return -1;
    for (int i = 0; i < count; i++) {
        const char *name = order ? order[i] : fields[i].name;
        /* Find the field by name (in case order != natural index) */
        const pfield_t *f = NULL;
        for (int j = 0; j < count; j++) {
            if (strcmp(fields[j].name, name) == 0) { f = &fields[j]; break; }
        }
        if (!f) f = &fields[i];
        if (encode_canonical_field(b, f)) return -1;
    }
    return 0;
}

static int encode_canonical_field(cbuf_t *b, const pfield_t *f) {
    if (cbuf_str(b, f->name)) return -1;
    if (cbuf_u8(b, dtype_to_byte(f->dtype))) return -1;
    if (cbuf_u8(b, f->nullable ? 1 : 0)) return -1;

    int ec = (f->enum_values && f->enum_count > 0) ? f->enum_count : 0;
    if (ec > 0xFFFF) return -1;
    if (cbuf_u16(b, (uint16_t)ec)) return -1;
    for (int i = 0; i < ec; i++)
        if (cbuf_str(b, f->enum_values[i])) return -1;

    int has_nested = (f->nested && f->nested_count > 0) ? 1 : 0;
    if (cbuf_u8(b, (uint8_t)has_nested)) return -1;
    if (has_nested) {
        if (encode_canonical_fields(b, f->nested, f->nested_order, f->nested_count)) return -1;
    }

    int has_items = (f->items && f->items_count > 0) ? 1 : 0;
    if (cbuf_u8(b, (uint8_t)has_items)) return -1;
    if (has_items) {
        if (encode_canonical_fields(b, f->items, f->items_order, f->items_count)) return -1;
    }
    return 0;
}

static uint64_t fnv1a_64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ETag: FNV-1a 64-bit hash of the canonical bytes. Format: "<16-hex>" */
static char *compute_etag(pidgin_profile_t *profile) {
    if (!profile) return NULL;

    cbuf_t b = {0};
    if (cbuf_u32(&b, (uint32_t)profile->version)) { free(b.data); return NULL; }
    if (encode_canonical_fields(&b, profile->fields, profile->field_order,
                                 profile->field_count)) {
        free(b.data); return NULL;
    }

    uint64_t h = fnv1a_64(b.data, b.len);
    free(b.data);

    char *etag = (char *)malloc(32);
    if (etag) snprintf(etag, 32, "\"%016llx\"", (unsigned long long)h);
    return etag;
}

pidgin_engine_t *pidgin_engine_create(int learn_samples, int evolve_interval) {
    pidgin_engine_t *engine = (pidgin_engine_t *)calloc(1, sizeof(pidgin_engine_t));
    if (!engine) return NULL;

    engine->learn_samples = learn_samples > 0 ? learn_samples : 30;
    engine->evolve_interval = evolve_interval > 0 ? evolve_interval : 500;
    pthread_mutex_init(&engine->lock, NULL);
    return engine;
}

void pidgin_engine_observe(pidgin_engine_t *engine, const char *endpoint, const char *json) {
    if (!engine || !endpoint || !json) return;

    pthread_mutex_lock(&engine->lock);

    endpoint_state_t *ep = engine_get_or_create(engine, endpoint);
    ep->requests++;

    if (!ep->ready && ep->sample_count < ep->sample_cap) {
        /* Still learning — store sample */
        ep->samples[ep->sample_count++] = pstrdup(json);

        if (ep->sample_count >= ep->sample_cap) {
            /* Ready to build profile */
            /* Build a JSON array from stored samples */
            /* First, parse them all */
            cJSON **parsed = (cJSON **)malloc(sizeof(cJSON *) * (size_t)ep->sample_count);
            int valid = 0;
            for (int i = 0; i < ep->sample_count; i++) {
                cJSON *p = cJSON_Parse(ep->samples[i]);
                if (p) {
                    /* If it's an array, flatten it */
                    if (cJSON_IsArray(p)) {
                        int n = cJSON_GetArraySize(p);
                        parsed = (cJSON **)realloc(parsed, sizeof(cJSON *) * (size_t)(valid + n + ep->sample_count));
                        for (int j = 0; j < n; j++) {
                            parsed[valid++] = cJSON_GetArrayItem(p, j);
                        }
                        /* Don't delete p yet — items are borrowed */
                    } else if (cJSON_IsObject(p)) {
                        parsed[valid++] = p;
                    } else {
                        cJSON_Delete(p);
                    }
                }
            }

            if (valid > 0) {
                ep->profile = (pidgin_profile_t *)calloc(1, sizeof(pidgin_profile_t));
                ep->profile->version = 1;
                ep->profile->fields = learn_fields(parsed, valid,
                                                   &ep->profile->field_order,
                                                   &ep->profile->field_count);
                ep->ready = 1;
                ep->version = 1;
                if (ep->etag) free(ep->etag);
                ep->etag = compute_etag(ep->profile);
            }

            /* Free parsed JSON trees */
            /* Note: we need to be careful because array items were borrowed.
               For simplicity, re-parse and delete all originals */
            for (int i = 0; i < ep->sample_count; i++) {
                cJSON *p = cJSON_Parse(ep->samples[i]);
                if (p) cJSON_Delete(p);
            }

            free(parsed);

            /* Free stored samples */
            for (int i = 0; i < ep->sample_count; i++)
                free(ep->samples[i]);
            ep->sample_count = 0;
        }
    } else if (ep->ready && engine->evolve_interval > 0 &&
               (ep->requests % engine->evolve_interval) == 0) {
        /* Schema evolution check — re-learn from recent and evolve */
        /* For simplicity, we store the current JSON as a single-sample evolution */
        char buf[65536];
        int n = snprintf(buf, sizeof(buf), "[%s]", json);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            pidgin_profile_t *evolved = pidgin_evolve(ep->profile, buf);
            if (evolved) {
                pidgin_profile_free(ep->profile);
                ep->profile = evolved;
                ep->version = evolved->version;
                if (ep->etag) free(ep->etag);
                ep->etag = compute_etag(ep->profile);
            }
        }
    }

    pthread_mutex_unlock(&engine->lock);
}

int pidgin_engine_ready(pidgin_engine_t *engine, const char *endpoint) {
    if (!engine || !endpoint) return 0;
    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    int ready = ep ? ep->ready : 0;
    pthread_mutex_unlock(&engine->lock);
    return ready;
}

pidgin_buf_t pidgin_engine_compress(pidgin_engine_t *engine, const char *endpoint, const char *json) {
    pidgin_buf_t result = { NULL, 0 };
    if (!engine || !endpoint || !json) return result;

    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    if (ep && ep->ready && ep->profile) {
        result = pidgin_compress(ep->profile, json);
    }
    pthread_mutex_unlock(&engine->lock);
    return result;
}

char *pidgin_engine_decompress(pidgin_engine_t *engine, const char *endpoint,
                                const uint8_t *data, size_t len) {
    if (!engine || !endpoint || !data) return NULL;

    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    char *result = NULL;
    if (ep && ep->ready && ep->profile) {
        result = pidgin_decompress(ep->profile, data, len);
    }
    pthread_mutex_unlock(&engine->lock);
    return result;
}

char *pidgin_engine_profile(pidgin_engine_t *engine, const char *endpoint) {
    if (!engine || !endpoint) return NULL;

    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    char *result = NULL;
    if (ep && ep->ready && ep->profile) {
        result = pidgin_profile_to_json(ep->profile);
    }
    pthread_mutex_unlock(&engine->lock);
    return result;
}

const char *pidgin_engine_etag(pidgin_engine_t *engine, const char *endpoint) {
    if (!engine || !endpoint) return NULL;

    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    const char *result = ep ? ep->etag : NULL;
    pthread_mutex_unlock(&engine->lock);
    return result;
}

pidgin_status_t pidgin_engine_status(pidgin_engine_t *engine, const char *endpoint) {
    pidgin_status_t status = { 0, 0, 0, NULL };
    if (!engine || !endpoint) return status;

    pthread_mutex_lock(&engine->lock);
    endpoint_state_t *ep = engine_find(engine, endpoint);
    if (ep) {
        status.ready = ep->ready;
        status.version = ep->version;
        status.requests = ep->requests;
        status.etag = ep->etag;
    }
    pthread_mutex_unlock(&engine->lock);
    return status;
}

void pidgin_engine_destroy(pidgin_engine_t *engine) {
    if (!engine) return;

    endpoint_state_t *ep = engine->endpoints;
    while (ep) {
        endpoint_state_t *next = ep->next;
        free(ep->path);
        if (ep->profile) pidgin_profile_free(ep->profile);
        if (ep->etag) free(ep->etag);
        for (int i = 0; i < ep->sample_count; i++)
            free(ep->samples[i]);
        free(ep->samples);
        free(ep);
        ep = next;
    }

    pthread_mutex_destroy(&engine->lock);
    free(engine);
}
