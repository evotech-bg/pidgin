/*
 * cjson.h — Minimal embedded JSON parser for libpidgin.
 *
 * Inspired by cJSON (MIT license, Dave Gamble).
 * Stripped to essentials: parse, create, print. No external deps.
 *
 * Usage:
 *   #define CJSON_IMPL   (in exactly one .c file before including)
 *   #include "cjson.h"
 */

#ifndef CJSON_H
#define CJSON_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>

/* ================================================================
 * Types
 * ================================================================ */

#define CJSON_FALSE   0
#define CJSON_TRUE    1
#define CJSON_NULL    2
#define CJSON_NUMBER  3
#define CJSON_STRING  4
#define CJSON_ARRAY   5
#define CJSON_OBJECT  6

typedef struct cJSON {
    struct cJSON *next, *prev;
    struct cJSON *child;        /* for arrays/objects */
    int    type;
    char  *valuestring;         /* for strings */
    double valuedouble;         /* for numbers */
    long long valueint;         /* integer shortcut */
    char  *string;              /* key name (in objects) */
} cJSON;

/* Parse / Print */
cJSON *cJSON_Parse(const char *value);
char  *cJSON_PrintUnformatted(const cJSON *item);
void   cJSON_Delete(cJSON *c);

/* Creation */
cJSON *cJSON_CreateNull(void);
cJSON *cJSON_CreateTrue(void);
cJSON *cJSON_CreateFalse(void);
cJSON *cJSON_CreateBool(int boolean);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateObject(void);

/* Array/Object manipulation */
void   cJSON_AddItemToArray(cJSON *array, cJSON *item);
void   cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);

/* Access helpers */
int    cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
int    cJSON_IsNull(const cJSON *item);
int    cJSON_IsBool(const cJSON *item);
int    cJSON_IsTrue(const cJSON *item);
int    cJSON_IsNumber(const cJSON *item);
int    cJSON_IsString(const cJSON *item);
int    cJSON_IsArray(const cJSON *item);
int    cJSON_IsObject(const cJSON *item);

/* ================================================================
 * Implementation
 * ================================================================ */

#ifdef CJSON_IMPL

static char *cjson_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static cJSON *cjson_new(void) {
    cJSON *node = (cJSON *)calloc(1, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *c) {
    cJSON *next;
    while (c) {
        next = c->next;
        if (c->child) cJSON_Delete(c->child);
        if (c->valuestring) free(c->valuestring);
        if (c->string) free(c->string);
        free(c);
        c = next;
    }
}

/* ---- Creation ---- */

cJSON *cJSON_CreateNull(void) {
    cJSON *item = cjson_new(); if (item) item->type = CJSON_NULL; return item;
}
cJSON *cJSON_CreateTrue(void) {
    cJSON *item = cjson_new(); if (item) item->type = CJSON_TRUE; return item;
}
cJSON *cJSON_CreateFalse(void) {
    cJSON *item = cjson_new(); if (item) item->type = CJSON_FALSE; return item;
}
cJSON *cJSON_CreateBool(int b) {
    return b ? cJSON_CreateTrue() : cJSON_CreateFalse();
}
cJSON *cJSON_CreateNumber(double num) {
    cJSON *item = cjson_new();
    if (item) {
        item->type = CJSON_NUMBER;
        item->valuedouble = num;
        item->valueint = (long long)num;
    }
    return item;
}
cJSON *cJSON_CreateString(const char *string) {
    cJSON *item = cjson_new();
    if (item) {
        item->type = CJSON_STRING;
        item->valuestring = cjson_strdup(string ? string : "");
    }
    return item;
}
cJSON *cJSON_CreateArray(void) {
    cJSON *item = cjson_new(); if (item) item->type = CJSON_ARRAY; return item;
}
cJSON *cJSON_CreateObject(void) {
    cJSON *item = cjson_new(); if (item) item->type = CJSON_OBJECT; return item;
}

/* ---- Manipulation ---- */

static void cjson_suffix_object(cJSON *prev, cJSON *item) {
    prev->next = item;
    item->prev = prev;
}

void cJSON_AddItemToArray(cJSON *array, cJSON *item) {
    if (!item || !array) return;
    cJSON *child = array->child;
    if (!child) {
        array->child = item;
        item->prev = item;  /* circular prev for last element */
        item->next = NULL;
    } else {
        /* Find last */
        cJSON *last = child->prev;  /* O(1) via circular prev */
        cjson_suffix_object(last, item);
        child->prev = item;         /* update circular prev */
    }
}

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item) {
    if (!item || !object) return;
    if (item->string) free(item->string);
    item->string = cjson_strdup(string);
    cJSON_AddItemToArray(object, item);
}

/* ---- Access ---- */

int cJSON_GetArraySize(const cJSON *array) {
    if (!array) return 0;
    cJSON *child = array->child;
    int size = 0;
    while (child) { size++; child = child->next; }
    return size;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    if (!array) return NULL;
    cJSON *child = array->child;
    while (child && index > 0) { child = child->next; index--; }
    return child;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string) {
    if (!object || !string) return NULL;
    cJSON *child = object->child;
    while (child) {
        if (child->string && strcmp(child->string, string) == 0)
            return child;
        child = child->next;
    }
    return NULL;
}

int cJSON_IsNull(const cJSON *item)   { return item && item->type == CJSON_NULL; }
int cJSON_IsBool(const cJSON *item)   { return item && (item->type == CJSON_TRUE || item->type == CJSON_FALSE); }
int cJSON_IsTrue(const cJSON *item)   { return item && item->type == CJSON_TRUE; }
int cJSON_IsNumber(const cJSON *item) { return item && item->type == CJSON_NUMBER; }
int cJSON_IsString(const cJSON *item) { return item && item->type == CJSON_STRING; }
int cJSON_IsArray(const cJSON *item)  { return item && item->type == CJSON_ARRAY; }
int cJSON_IsObject(const cJSON *item) { return item && item->type == CJSON_OBJECT; }

/* ================================================================
 * Parser — recursive descent
 * ================================================================ */

typedef struct { const char *content; size_t offset; size_t length; } parse_buffer;

static int can_read(const parse_buffer *b, size_t n) {
    return b && (b->offset + n <= b->length);
}

static const char *skip_ws(parse_buffer *b) {
    if (!b || !b->content) return NULL;
    while (b->offset < b->length) {
        char c = b->content[b->offset];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            b->offset++;
        else
            break;
    }
    return b->content + b->offset;
}

static cJSON *parse_value(parse_buffer *b);

/* Parse a 4-hex-digit Unicode escape and write UTF-8 */
static unsigned parse_hex4(const char *str) {
    unsigned h = 0;
    for (int i = 0; i < 4; i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') h = (h << 4) | (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') h = (h << 4) | (unsigned)(10 + c - 'a');
        else if (c >= 'A' && c <= 'F') h = (h << 4) | (unsigned)(10 + c - 'A');
        else return 0;
    }
    return h;
}

static cJSON *parse_string(parse_buffer *b) {
    if (!can_read(b, 1) || b->content[b->offset] != '"') return NULL;
    b->offset++;  /* skip opening quote */

    /* First pass: find length */
    size_t start = b->offset;
    size_t out_len = 0;
    while (b->offset < b->length && b->content[b->offset] != '"') {
        if (b->content[b->offset] == '\\') {
            b->offset++;
            if (b->offset >= b->length) return NULL;
            if (b->content[b->offset] == 'u') {
                /* Unicode escape: worst case 4 UTF-8 bytes */
                out_len += 4;
                b->offset += 4;
            } else {
                out_len++;
                b->offset++;
            }
        } else {
            out_len++;
            b->offset++;
        }
    }
    if (b->offset >= b->length) return NULL;  /* no closing quote */
    b->offset++;  /* skip closing quote */

    /* Allocate and second pass: decode */
    char *output = (char *)malloc(out_len + 1);
    if (!output) return NULL;

    const char *src = b->content + start;
    const char *end = b->content + b->offset - 1;  /* before closing quote */
    size_t oi = 0;

    while (src < end) {
        if (*src != '\\') {
            output[oi++] = *src++;
        } else {
            src++;  /* skip backslash */
            switch (*src) {
                case '"':  output[oi++] = '"';  src++; break;
                case '\\': output[oi++] = '\\'; src++; break;
                case '/':  output[oi++] = '/';  src++; break;
                case 'b':  output[oi++] = '\b'; src++; break;
                case 'f':  output[oi++] = '\f'; src++; break;
                case 'n':  output[oi++] = '\n'; src++; break;
                case 'r':  output[oi++] = '\r'; src++; break;
                case 't':  output[oi++] = '\t'; src++; break;
                case 'u': {
                    src++;
                    unsigned cp = parse_hex4(src); src += 4;
                    /* Handle surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF && src + 5 < end && src[0] == '\\' && src[1] == 'u') {
                        unsigned cp2 = parse_hex4(src + 2);
                        if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
                            src += 6;
                        }
                    }
                    /* Encode UTF-8 */
                    if (cp < 0x80) {
                        output[oi++] = (char)cp;
                    } else if (cp < 0x800) {
                        output[oi++] = (char)(0xC0 | (cp >> 6));
                        output[oi++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        output[oi++] = (char)(0xE0 | (cp >> 12));
                        output[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        output[oi++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        output[oi++] = (char)(0xF0 | (cp >> 18));
                        output[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        output[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        output[oi++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: output[oi++] = *src++; break;
            }
        }
    }
    output[oi] = '\0';

    cJSON *item = cjson_new();
    if (!item) { free(output); return NULL; }
    item->type = CJSON_STRING;
    item->valuestring = output;
    return item;
}

static cJSON *parse_number(parse_buffer *b) {
    const char *start = b->content + b->offset;
    const char *p = start;

    /* Allow leading minus */
    if (*p == '-') p++;

    /* Integer part */
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
    } else {
        return NULL;
    }

    int is_float = 0;

    /* Fractional part */
    if (*p == '.') {
        is_float = 1;
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }

    /* Exponent */
    if (*p == 'e' || *p == 'E') {
        is_float = 1;
        p++;
        if (*p == '+' || *p == '-') p++;
        while (*p >= '0' && *p <= '9') p++;
    }

    double d = strtod(start, NULL);

    cJSON *item = cjson_new();
    if (!item) return NULL;
    item->type = CJSON_NUMBER;
    item->valuedouble = d;

    /* Store integer value if it fits */
    if (!is_float && d >= (double)LLONG_MIN && d <= (double)LLONG_MAX) {
        item->valueint = (long long)d;
    } else {
        item->valueint = (long long)d;
    }

    b->offset += (size_t)(p - start);
    return item;
}

static cJSON *parse_array(parse_buffer *b) {
    if (!can_read(b, 1) || b->content[b->offset] != '[') return NULL;
    b->offset++;  /* skip [ */

    cJSON *array = cjson_new();
    if (!array) return NULL;
    array->type = CJSON_ARRAY;

    skip_ws(b);
    if (can_read(b, 1) && b->content[b->offset] == ']') {
        b->offset++;
        return array;
    }

    for (;;) {
        skip_ws(b);
        cJSON *item = parse_value(b);
        if (!item) { cJSON_Delete(array); return NULL; }
        cJSON_AddItemToArray(array, item);

        skip_ws(b);
        if (!can_read(b, 1)) { cJSON_Delete(array); return NULL; }
        if (b->content[b->offset] == ',') {
            b->offset++;
        } else if (b->content[b->offset] == ']') {
            b->offset++;
            return array;
        } else {
            cJSON_Delete(array); return NULL;
        }
    }
}

static cJSON *parse_object(parse_buffer *b) {
    if (!can_read(b, 1) || b->content[b->offset] != '{') return NULL;
    b->offset++;  /* skip { */

    cJSON *object = cjson_new();
    if (!object) return NULL;
    object->type = CJSON_OBJECT;

    skip_ws(b);
    if (can_read(b, 1) && b->content[b->offset] == '}') {
        b->offset++;
        return object;
    }

    for (;;) {
        skip_ws(b);
        cJSON *key = parse_string(b);
        if (!key) { cJSON_Delete(object); return NULL; }

        skip_ws(b);
        if (!can_read(b, 1) || b->content[b->offset] != ':') {
            cJSON_Delete(key); cJSON_Delete(object); return NULL;
        }
        b->offset++;  /* skip : */

        skip_ws(b);
        cJSON *item = parse_value(b);
        if (!item) { cJSON_Delete(key); cJSON_Delete(object); return NULL; }

        item->string = key->valuestring;
        key->valuestring = NULL;
        cJSON_Delete(key);

        cJSON_AddItemToArray(object, item);  /* object uses same linked list */

        skip_ws(b);
        if (!can_read(b, 1)) { cJSON_Delete(object); return NULL; }
        if (b->content[b->offset] == ',') {
            b->offset++;
        } else if (b->content[b->offset] == '}') {
            b->offset++;
            return object;
        } else {
            cJSON_Delete(object); return NULL;
        }
    }
}

static cJSON *parse_value(parse_buffer *b) {
    skip_ws(b);
    if (!can_read(b, 1)) return NULL;

    char c = b->content[b->offset];

    if (c == 'n' && can_read(b, 4) && strncmp(b->content + b->offset, "null", 4) == 0) {
        b->offset += 4;
        cJSON *item = cjson_new(); if (item) item->type = CJSON_NULL; return item;
    }
    if (c == 't' && can_read(b, 4) && strncmp(b->content + b->offset, "true", 4) == 0) {
        b->offset += 4;
        cJSON *item = cjson_new(); if (item) item->type = CJSON_TRUE; return item;
    }
    if (c == 'f' && can_read(b, 5) && strncmp(b->content + b->offset, "false", 5) == 0) {
        b->offset += 5;
        cJSON *item = cjson_new(); if (item) item->type = CJSON_FALSE; return item;
    }
    if (c == '"') return parse_string(b);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(b);
    if (c == '[') return parse_array(b);
    if (c == '{') return parse_object(b);

    return NULL;
}

cJSON *cJSON_Parse(const char *value) {
    if (!value || !*value) return NULL;
    parse_buffer b;
    b.content = value;
    b.offset = 0;
    b.length = strlen(value);
    return parse_value(&b);
}

/* ================================================================
 * Printer — compact (unformatted)
 * ================================================================ */

typedef struct { char *buf; size_t len, cap; } print_buffer;

static int pb_init(print_buffer *p, size_t cap) {
    p->buf = (char *)malloc(cap);
    if (!p->buf) return -1;
    p->len = 0; p->cap = cap;
    return 0;
}

static int pb_grow(print_buffer *p, size_t need) {
    if (p->len + need <= p->cap) return 0;
    size_t nc = p->cap;
    while (nc < p->len + need) nc *= 2;
    char *nb = (char *)realloc(p->buf, nc);
    if (!nb) return -1;
    p->buf = nb; p->cap = nc;
    return 0;
}

static int pb_append(print_buffer *p, const char *s, size_t n) {
    if (pb_grow(p, n)) return -1;
    memcpy(p->buf + p->len, s, n);
    p->len += n;
    return 0;
}

static int pb_char(print_buffer *p, char c) {
    if (pb_grow(p, 1)) return -1;
    p->buf[p->len++] = c;
    return 0;
}

static int print_value(const cJSON *item, print_buffer *p);

static int print_string_ptr(const char *str, print_buffer *p) {
    if (!str) return pb_append(p, "\"\"", 2);

    pb_char(p, '"');
    const char *s = str;
    while (*s) {
        char c = *s;
        if ((unsigned char)c >= 0x20 && c != '"' && c != '\\') {
            pb_char(p, c);
        } else {
            pb_char(p, '\\');
            switch (c) {
                case '"':  pb_char(p, '"');  break;
                case '\\': pb_char(p, '\\'); break;
                case '\b': pb_char(p, 'b');  break;
                case '\f': pb_char(p, 'f');  break;
                case '\n': pb_char(p, 'n');  break;
                case '\r': pb_char(p, 'r');  break;
                case '\t': pb_char(p, 't');  break;
                default: {
                    char esc[6];
                    snprintf(esc, sizeof(esc), "u%04x", (unsigned char)c);
                    pb_append(p, esc, 5);
                    break;
                }
            }
        }
        s++;
    }
    pb_char(p, '"');
    return 0;
}

static int print_number(const cJSON *item, print_buffer *p) {
    char numbuf[64];
    double d = item->valuedouble;

    if (d == 0.0) {
        /* Check for integer zero vs float zero */
        if (item->valueint == 0 && d == (double)item->valueint) {
            return pb_char(p, '0');
        }
    }

    /* If integer value matches double, print as integer */
    if (d == (double)item->valueint && d >= -1e15 && d <= 1e15) {
        snprintf(numbuf, sizeof(numbuf), "%lld", item->valueint);
    } else {
        /* Print as double — use enough precision */
        if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9) {
            snprintf(numbuf, sizeof(numbuf), "%e", d);
        } else {
            snprintf(numbuf, sizeof(numbuf), "%.17g", d);
        }
    }
    return pb_append(p, numbuf, strlen(numbuf));
}

static int print_value(const cJSON *item, print_buffer *p) {
    if (!item) return pb_append(p, "null", 4);

    switch (item->type) {
    case CJSON_NULL:   return pb_append(p, "null", 4);
    case CJSON_FALSE:  return pb_append(p, "false", 5);
    case CJSON_TRUE:   return pb_append(p, "true", 4);
    case CJSON_NUMBER: return print_number(item, p);
    case CJSON_STRING: return print_string_ptr(item->valuestring, p);
    case CJSON_ARRAY: {
        pb_char(p, '[');
        cJSON *child = item->child;
        int first = 1;
        while (child) {
            if (!first) pb_char(p, ',');
            print_value(child, p);
            child = child->next;
            first = 0;
        }
        pb_char(p, ']');
        return 0;
    }
    case CJSON_OBJECT: {
        pb_char(p, '{');
        cJSON *child = item->child;
        int first = 1;
        while (child) {
            if (!first) pb_char(p, ',');
            print_string_ptr(child->string, p);
            pb_char(p, ':');
            print_value(child, p);
            child = child->next;
            first = 0;
        }
        pb_char(p, '}');
        return 0;
    }
    default: return pb_append(p, "null", 4);
    }
}

char *cJSON_PrintUnformatted(const cJSON *item) {
    print_buffer p;
    if (pb_init(&p, 256)) return NULL;
    if (print_value(item, &p)) { free(p.buf); return NULL; }
    pb_char(&p, '\0');
    return p.buf;
}

#endif /* CJSON_IMPL */
#endif /* CJSON_H */
