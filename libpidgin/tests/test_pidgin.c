/*
 * test_pidgin.c — Basic tests for libpidgin.
 *
 * Tests:
 *   1. Learn schema from JSON samples
 *   2. Compress + decompress roundtrip (single object)
 *   3. Compress + decompress roundtrip (array of objects)
 *   4. Engine auto-learn lifecycle
 *   5. Profile serialization roundtrip
 *   6. Schema evolution
 *   7. Nullable and absent field handling
 *   8. Enum encoding
 *   9. Timestamp encoding
 *  10. Unknown field fallback
 */

#include "../include/pidgin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple JSON parsing for verification (reuse cjson) */
#define CJSON_IMPL
#include "../src/cjson.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* ================================================================
 * Test helpers
 * ================================================================ */

/* Check if a JSON string contains a key=value pair */
static int json_has_str(const char *json, const char *key, const char *val) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    /* Handle arrays — check first element */
    cJSON *obj = root;
    if (cJSON_IsArray(root)) {
        obj = cJSON_GetArrayItem(root, 0);
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    int result = (item && cJSON_IsString(item) && strcmp(item->valuestring, val) == 0);
    cJSON_Delete(root);
    return result;
}

static int json_has_int(const char *json, const char *key, long long val) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    cJSON *obj = root;
    if (cJSON_IsArray(root)) obj = cJSON_GetArrayItem(root, 0);

    cJSON *item = cJSON_GetObjectItem(obj, key);
    int result = (item && cJSON_IsNumber(item) && item->valueint == val);
    cJSON_Delete(root);
    return result;
}

static int json_has_bool(const char *json, const char *key, int val) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    cJSON *obj = root;
    if (cJSON_IsArray(root)) obj = cJSON_GetArrayItem(root, 0);

    cJSON *item = cJSON_GetObjectItem(obj, key);
    int result = 0;
    if (item) {
        if (val) result = cJSON_IsTrue(item);
        else result = (item->type == CJSON_FALSE);
    }
    cJSON_Delete(root);
    return result;
}

static int json_has_null(const char *json, const char *key) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    cJSON *obj = root;
    if (cJSON_IsArray(root)) obj = cJSON_GetArrayItem(root, 0);

    cJSON *item = cJSON_GetObjectItem(obj, key);
    int result = (item && cJSON_IsNull(item));
    cJSON_Delete(root);
    return result;
}

static int json_array_size(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }
    int n = cJSON_GetArraySize(root);
    cJSON_Delete(root);
    return n;
}

/* ================================================================
 * Test 1: Learn schema from samples
 * ================================================================ */

static void test_learn(void) {
    printf("Test 1: Learn schema from JSON samples\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\", \"active\":true, \"score\":9.5},"
        "  {\"id\":2, \"name\":\"Bob\",   \"active\":false, \"score\":7.2},"
        "  {\"id\":3, \"name\":\"Carol\", \"active\":true, \"score\":8.8}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "pidgin_learn should return a profile");

    /* Verify profile can be serialized */
    char *json = pidgin_profile_to_json(profile);
    ASSERT(json != NULL, "pidgin_profile_to_json should return JSON");

    if (json) {
        /* Check it contains expected fields */
        cJSON *root = cJSON_Parse(json);
        ASSERT(root != NULL, "profile JSON should be parseable");
        if (root) {
            cJSON *fields = cJSON_GetObjectItem(root, "fields");
            ASSERT(fields != NULL, "profile should have 'fields'");
            if (fields) {
                ASSERT(cJSON_GetObjectItem(fields, "id") != NULL, "fields should contain 'id'");
                ASSERT(cJSON_GetObjectItem(fields, "name") != NULL, "fields should contain 'name'");
                ASSERT(cJSON_GetObjectItem(fields, "active") != NULL, "fields should contain 'active'");
                ASSERT(cJSON_GetObjectItem(fields, "score") != NULL, "fields should contain 'score'");
            }

            cJSON *order = cJSON_GetObjectItem(root, "field_order");
            ASSERT(order != NULL && cJSON_IsArray(order), "profile should have 'field_order' array");
            if (order) {
                ASSERT(cJSON_GetArraySize(order) == 4, "field_order should have 4 fields");
            }

            cJSON_Delete(root);
        }
        pidgin_free(json);
    }

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 2: Compress + decompress roundtrip (single object)
 * ================================================================ */

static void test_roundtrip_single(void) {
    printf("Test 2: Compress + decompress roundtrip (single object)\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\", \"active\":true},"
        "  {\"id\":2, \"name\":\"Bob\",   \"active\":false},"
        "  {\"id\":3, \"name\":\"Carol\", \"active\":true}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile learned");

    const char *input = "{\"id\":42, \"name\":\"Dave\", \"active\":true}";
    pidgin_buf_t compressed = pidgin_compress(profile, input);
    ASSERT(compressed.data != NULL, "compressed data should not be NULL");
    ASSERT(compressed.len > 0, "compressed data should have length > 0");
    ASSERT(compressed.len < strlen(input), "compressed should be smaller than JSON");

    char *decompressed = pidgin_decompress(profile, compressed.data, compressed.len);
    ASSERT(decompressed != NULL, "decompressed should not be NULL");

    if (decompressed) {
        ASSERT(json_has_int(decompressed, "id", 42), "id should be 42");
        ASSERT(json_has_str(decompressed, "name", "Dave"), "name should be 'Dave'");
        ASSERT(json_has_bool(decompressed, "active", 1), "active should be true");
        pidgin_free(decompressed);
    }

    pidgin_buf_free(&compressed);
    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 3: Compress + decompress roundtrip (array)
 * ================================================================ */

static void test_roundtrip_array(void) {
    printf("Test 3: Compress + decompress roundtrip (array)\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\"},"
        "  {\"id\":2, \"name\":\"Bob\"},"
        "  {\"id\":3, \"name\":\"Carol\"}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile learned");

    const char *input =
        "["
        "  {\"id\":10, \"name\":\"Xander\"},"
        "  {\"id\":20, \"name\":\"Yara\"},"
        "  {\"id\":30, \"name\":\"Zack\"}"
        "]";

    pidgin_buf_t compressed = pidgin_compress(profile, input);
    ASSERT(compressed.data != NULL, "compressed data");
    ASSERT(compressed.len > 0, "compressed len > 0");

    char *decompressed = pidgin_decompress(profile, compressed.data, compressed.len);
    ASSERT(decompressed != NULL, "decompressed should not be NULL");

    if (decompressed) {
        ASSERT(json_array_size(decompressed) == 3, "should have 3 items");

        /* Parse and check individual items */
        cJSON *arr = cJSON_Parse(decompressed);
        if (arr && cJSON_IsArray(arr)) {
            cJSON *item0 = cJSON_GetArrayItem(arr, 0);
            cJSON *item2 = cJSON_GetArrayItem(arr, 2);

            ASSERT(item0 && cJSON_GetObjectItem(item0, "id") &&
                   cJSON_GetObjectItem(item0, "id")->valueint == 10,
                   "first item id should be 10");
            ASSERT(item2 && cJSON_GetObjectItem(item2, "name") &&
                   strcmp(cJSON_GetObjectItem(item2, "name")->valuestring, "Zack") == 0,
                   "third item name should be 'Zack'");
        }
        if (arr) cJSON_Delete(arr);
        pidgin_free(decompressed);
    }

    pidgin_buf_free(&compressed);
    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 4: Engine auto-learn lifecycle
 * ================================================================ */

static void test_engine_lifecycle(void) {
    printf("Test 4: Engine auto-learn lifecycle\n");

    pidgin_engine_t *engine = pidgin_engine_create(3, 0);  /* learn after 3 samples */
    ASSERT(engine != NULL, "engine created");

    const char *endpoint = "/api/users";

    /* Not ready yet */
    ASSERT(pidgin_engine_ready(engine, endpoint) == 0, "not ready before learning");

    /* Feed 3 samples */
    pidgin_engine_observe(engine, endpoint, "{\"id\":1, \"name\":\"Alice\"}");
    ASSERT(pidgin_engine_ready(engine, endpoint) == 0, "not ready after 1 sample");

    pidgin_engine_observe(engine, endpoint, "{\"id\":2, \"name\":\"Bob\"}");
    ASSERT(pidgin_engine_ready(engine, endpoint) == 0, "not ready after 2 samples");

    pidgin_engine_observe(engine, endpoint, "{\"id\":3, \"name\":\"Carol\"}");
    ASSERT(pidgin_engine_ready(engine, endpoint) == 1, "ready after 3 samples");

    /* Compress */
    pidgin_buf_t compressed = pidgin_engine_compress(engine, endpoint,
        "{\"id\":99, \"name\":\"Test\"}");
    ASSERT(compressed.data != NULL, "engine compress works");

    /* Decompress */
    char *dec = pidgin_engine_decompress(engine, endpoint, compressed.data, compressed.len);
    ASSERT(dec != NULL, "engine decompress works");
    if (dec) {
        ASSERT(json_has_int(dec, "id", 99), "id should be 99");
        ASSERT(json_has_str(dec, "name", "Test"), "name should be 'Test'");
        pidgin_free(dec);
    }

    /* Status */
    pidgin_status_t status = pidgin_engine_status(engine, endpoint);
    ASSERT(status.ready == 1, "status.ready should be 1");
    ASSERT(status.version == 1, "status.version should be 1");
    ASSERT(status.requests == 3, "status.requests should be 3");
    ASSERT(status.etag != NULL, "status.etag should not be NULL");

    /* Profile retrieval */
    char *prof_json = pidgin_engine_profile(engine, endpoint);
    ASSERT(prof_json != NULL, "engine profile JSON");
    if (prof_json) pidgin_free(prof_json);

    /* ETag */
    const char *etag = pidgin_engine_etag(engine, endpoint);
    ASSERT(etag != NULL, "engine etag");

    pidgin_buf_free(&compressed);
    pidgin_engine_destroy(engine);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 5: Profile serialization roundtrip
 * ================================================================ */

static void test_profile_serialization(void) {
    printf("Test 5: Profile serialization roundtrip\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\", \"score\":9.5, \"active\":true},"
        "  {\"id\":2, \"name\":\"Bob\",   \"score\":7.2, \"active\":false},"
        "  {\"id\":3, \"name\":\"Carol\", \"score\":8.8, \"active\":true}"
        "]";

    pidgin_profile_t *original = pidgin_learn(samples);
    ASSERT(original != NULL, "original profile");

    /* Serialize */
    char *json = pidgin_profile_to_json(original);
    ASSERT(json != NULL, "serialized to JSON");

    /* Deserialize */
    pidgin_profile_t *loaded = pidgin_profile_from_json(json);
    ASSERT(loaded != NULL, "deserialized from JSON");

    /* Compress with original, decompress with loaded */
    const char *input = "{\"id\":99, \"name\":\"Eve\", \"score\":6.1, \"active\":false}";
    pidgin_buf_t compressed = pidgin_compress(original, input);
    ASSERT(compressed.data != NULL, "compressed with original");

    char *dec = pidgin_decompress(loaded, compressed.data, compressed.len);
    ASSERT(dec != NULL, "decompressed with loaded profile");
    if (dec) {
        ASSERT(json_has_int(dec, "id", 99), "id should be 99");
        ASSERT(json_has_str(dec, "name", "Eve"), "name should be 'Eve'");
        pidgin_free(dec);
    }

    /* Re-serialize loaded and compare */
    char *json2 = pidgin_profile_to_json(loaded);
    ASSERT(json2 != NULL, "re-serialized");
    if (json && json2) {
        ASSERT(strcmp(json, json2) == 0, "re-serialized should match original");
    }

    pidgin_buf_free(&compressed);
    pidgin_free(json);
    pidgin_free(json2);
    pidgin_profile_free(original);
    pidgin_profile_free(loaded);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 6: Schema evolution
 * ================================================================ */

static void test_evolution(void) {
    printf("Test 6: Schema evolution\n");

    const char *samples_v1 =
        "["
        "  {\"id\":1, \"name\":\"Alice\"},"
        "  {\"id\":2, \"name\":\"Bob\"},"
        "  {\"id\":3, \"name\":\"Carol\"}"
        "]";

    pidgin_profile_t *v1 = pidgin_learn(samples_v1);
    ASSERT(v1 != NULL, "v1 profile");

    /* Evolve with new data that has an extra field */
    const char *samples_v2 =
        "["
        "  {\"id\":10, \"name\":\"Dave\", \"email\":\"dave@test.com\"},"
        "  {\"id\":11, \"name\":\"Eve\",  \"email\":\"eve@test.com\"},"
        "  {\"id\":12, \"name\":\"Frank\", \"email\":\"frank@test.com\"}"
        "]";

    pidgin_profile_t *v2 = pidgin_evolve(v1, samples_v2);
    ASSERT(v2 != NULL, "v2 profile");

    if (v2) {
        /* v2 should have version 2 */
        char *json = pidgin_profile_to_json(v2);
        if (json) {
            cJSON *root = cJSON_Parse(json);
            if (root) {
                cJSON *ver = cJSON_GetObjectItem(root, "version");
                ASSERT(ver && ver->valueint == 2, "v2 should have version 2");

                cJSON *fields = cJSON_GetObjectItem(root, "fields");
                ASSERT(fields && cJSON_GetObjectItem(fields, "email") != NULL,
                       "v2 should have 'email' field");

                cJSON_Delete(root);
            }
            pidgin_free(json);
        }

        /* Data with v1 schema (no email) should still work with v2 */
        const char *input = "{\"id\":42, \"name\":\"Test\"}";
        pidgin_buf_t compressed = pidgin_compress(v2, input);
        ASSERT(compressed.data != NULL, "compress v1-style data with v2");

        char *dec = pidgin_decompress(v2, compressed.data, compressed.len);
        ASSERT(dec != NULL, "decompress v1-style data with v2");
        if (dec) {
            ASSERT(json_has_int(dec, "id", 42), "id should be 42");
            ASSERT(json_has_str(dec, "name", "Test"), "name should be 'Test'");
            pidgin_free(dec);
        }

        pidgin_buf_free(&compressed);
        pidgin_profile_free(v2);
    }

    pidgin_profile_free(v1);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 7: Nullable and absent fields
 * ================================================================ */

static void test_nullable_fields(void) {
    printf("Test 7: Nullable and absent fields\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\", \"bio\":\"hello\"},"
        "  {\"id\":2, \"name\":\"Bob\",   \"bio\":null},"
        "  {\"id\":3, \"name\":\"Carol\"}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile with nullable fields");

    /* Test null value */
    const char *input_null = "{\"id\":10, \"name\":\"Dave\", \"bio\":null}";
    pidgin_buf_t c1 = pidgin_compress(profile, input_null);
    ASSERT(c1.data != NULL, "compress null field");
    char *d1 = pidgin_decompress(profile, c1.data, c1.len);
    ASSERT(d1 != NULL, "decompress null field");
    if (d1) {
        ASSERT(json_has_null(d1, "bio"), "bio should be null");
        ASSERT(json_has_int(d1, "id", 10), "id should be 10");
        pidgin_free(d1);
    }
    pidgin_buf_free(&c1);

    /* Test absent field */
    const char *input_absent = "{\"id\":20, \"name\":\"Eve\"}";
    pidgin_buf_t c2 = pidgin_compress(profile, input_absent);
    ASSERT(c2.data != NULL, "compress absent field");
    char *d2 = pidgin_decompress(profile, c2.data, c2.len);
    ASSERT(d2 != NULL, "decompress absent field");
    if (d2) {
        /* bio should not be in the output */
        cJSON *root = cJSON_Parse(d2);
        if (root) {
            ASSERT(cJSON_GetObjectItem(root, "bio") == NULL, "bio should be absent");
            ASSERT(cJSON_GetObjectItem(root, "id")->valueint == 20, "id should be 20");
            cJSON_Delete(root);
        }
        pidgin_free(d2);
    }
    pidgin_buf_free(&c2);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 8: Enum encoding
 * ================================================================ */

static void test_enum_encoding(void) {
    printf("Test 8: Enum encoding\n");

    /* Need enough samples with repeated values for enum detection */
    const char *samples =
        "["
        "  {\"id\":1, \"status\":\"active\"},"
        "  {\"id\":2, \"status\":\"inactive\"},"
        "  {\"id\":3, \"status\":\"active\"},"
        "  {\"id\":4, \"status\":\"inactive\"},"
        "  {\"id\":5, \"status\":\"active\"},"
        "  {\"id\":6, \"status\":\"inactive\"}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile with enums");

    /* Known enum value */
    const char *input = "{\"id\":10, \"status\":\"active\"}";
    pidgin_buf_t c = pidgin_compress(profile, input);
    ASSERT(c.data != NULL, "compress enum");
    char *d = pidgin_decompress(profile, c.data, c.len);
    ASSERT(d != NULL, "decompress enum");
    if (d) {
        ASSERT(json_has_str(d, "status", "active"), "status should be 'active'");
        pidgin_free(d);
    }
    pidgin_buf_free(&c);

    /* Unknown enum value (0xFE fallback) */
    const char *input2 = "{\"id\":11, \"status\":\"pending\"}";
    pidgin_buf_t c2 = pidgin_compress(profile, input2);
    ASSERT(c2.data != NULL, "compress unknown enum");
    char *d2 = pidgin_decompress(profile, c2.data, c2.len);
    ASSERT(d2 != NULL, "decompress unknown enum");
    if (d2) {
        ASSERT(json_has_str(d2, "status", "pending"), "status should be 'pending'");
        pidgin_free(d2);
    }
    pidgin_buf_free(&c2);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 9: Timestamp encoding
 * ================================================================ */

static void test_timestamp_encoding(void) {
    printf("Test 9: Timestamp encoding\n");

    const char *samples =
        "["
        "  {\"id\":1, \"ts\":\"2024-01-15T10:30:00Z\"},"
        "  {\"id\":2, \"ts\":\"2024-01-16T11:00:00Z\"},"
        "  {\"id\":3, \"ts\":\"2024-01-17T09:15:00Z\"}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile with timestamps");

    const char *input = "{\"id\":10, \"ts\":\"2024-06-01T14:00:00Z\"}";
    pidgin_buf_t c = pidgin_compress(profile, input);
    ASSERT(c.data != NULL, "compress timestamp");
    char *d = pidgin_decompress(profile, c.data, c.len);
    ASSERT(d != NULL, "decompress timestamp");
    if (d) {
        ASSERT(json_has_str(d, "ts", "2024-06-01T14:00:00Z"), "ts should roundtrip");
        pidgin_free(d);
    }
    pidgin_buf_free(&c);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 10: Unknown field fallback
 * ================================================================ */

static void test_unknown_fields(void) {
    printf("Test 10: Unknown field fallback\n");

    const char *samples =
        "["
        "  {\"id\":1, \"name\":\"Alice\"},"
        "  {\"id\":2, \"name\":\"Bob\"},"
        "  {\"id\":3, \"name\":\"Carol\"}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile for unknown fields test");

    /* Compress data that has an extra field not in schema */
    const char *input = "{\"id\":42, \"name\":\"Dave\", \"extra\":\"bonus\"}";
    pidgin_buf_t c = pidgin_compress(profile, input);
    ASSERT(c.data != NULL, "compress with unknown field");

    char *d = pidgin_decompress(profile, c.data, c.len);
    ASSERT(d != NULL, "decompress with unknown field");
    if (d) {
        ASSERT(json_has_int(d, "id", 42), "id should be 42");
        ASSERT(json_has_str(d, "name", "Dave"), "name should be 'Dave'");
        ASSERT(json_has_str(d, "extra", "bonus"), "extra field should be preserved");
        pidgin_free(d);
    }
    pidgin_buf_free(&c);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 11: Version string
 * ================================================================ */

static void test_version(void) {
    printf("Test 11: Version string\n");
    const char *v = pidgin_version();
    ASSERT(v != NULL, "version should not be NULL");
    ASSERT(strlen(v) > 0, "version should not be empty");
    printf("  version: %s\n", v);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 12: Varint edge cases
 * ================================================================ */

static void test_varint_edgecases(void) {
    printf("Test 12: Varint encoding edge cases\n");

    const char *samples =
        "["
        "  {\"val\":0},"
        "  {\"val\":1},"
        "  {\"val\":2}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile for varint test");

    /* Test zero */
    pidgin_buf_t c0 = pidgin_compress(profile, "{\"val\":0}");
    char *d0 = pidgin_decompress(profile, c0.data, c0.len);
    ASSERT(d0 && json_has_int(d0, "val", 0), "varint 0 roundtrips");
    pidgin_free(d0); pidgin_buf_free(&c0);

    /* Test negative */
    pidgin_buf_t c1 = pidgin_compress(profile, "{\"val\":-1}");
    char *d1 = pidgin_decompress(profile, c1.data, c1.len);
    ASSERT(d1 && json_has_int(d1, "val", -1), "varint -1 roundtrips");
    pidgin_free(d1); pidgin_buf_free(&c1);

    /* Test large positive */
    pidgin_buf_t c2 = pidgin_compress(profile, "{\"val\":100000}");
    char *d2 = pidgin_decompress(profile, c2.data, c2.len);
    ASSERT(d2 && json_has_int(d2, "val", 100000), "varint 100000 roundtrips");
    pidgin_free(d2); pidgin_buf_free(&c2);

    /* Test large negative */
    pidgin_buf_t c3 = pidgin_compress(profile, "{\"val\":-999999}");
    char *d3 = pidgin_decompress(profile, c3.data, c3.len);
    ASSERT(d3 && json_has_int(d3, "val", -999999), "varint -999999 roundtrips");
    pidgin_free(d3); pidgin_buf_free(&c3);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Test 13: Float precision
 * ================================================================ */

static void test_float_precision(void) {
    printf("Test 13: Float precision\n");

    const char *samples =
        "["
        "  {\"x\":1.1},"
        "  {\"x\":2.2},"
        "  {\"x\":3.3}"
        "]";

    pidgin_profile_t *profile = pidgin_learn(samples);
    ASSERT(profile != NULL, "profile for float test");

    pidgin_buf_t c = pidgin_compress(profile, "{\"x\":3.141592653589793}");
    char *d = pidgin_decompress(profile, c.data, c.len);
    ASSERT(d != NULL, "decompress float");
    if (d) {
        cJSON *root = cJSON_Parse(d);
        if (root) {
            cJSON *x = cJSON_GetObjectItem(root, "x");
            ASSERT(x && cJSON_IsNumber(x), "x should be a number");
            if (x) {
                double diff = x->valuedouble - 3.141592653589793;
                if (diff < 0) diff = -diff;
                ASSERT(diff < 1e-10, "float precision should be preserved");
            }
            cJSON_Delete(root);
        }
        pidgin_free(d);
    }
    pidgin_buf_free(&c);

    pidgin_profile_free(profile);
    printf("  -> done\n\n");
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    printf("=== libpidgin tests ===\n\n");

    test_learn();
    test_roundtrip_single();
    test_roundtrip_array();
    test_engine_lifecycle();
    test_profile_serialization();
    test_evolution();
    test_nullable_fields();
    test_enum_encoding();
    test_timestamp_encoding();
    test_unknown_fields();
    test_version();
    test_varint_edgecases();
    test_float_precision();

    printf("=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
