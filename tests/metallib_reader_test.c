/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "metallib/metallib_reader.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test-local log stubs. Production impls live in src/device.c which pulls
 * in Vulkan headers; defining them here keeps the test binary standalone.
 * Logs are silenced — pass/fail is reported via ASSERT to stderr. */
void lagfx_log_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void lagfx_warn_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void lagfx_err_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void lagfx_trace_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

#define ASSERT(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "read_file: cannot open '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static int test_smoke_open(void) {
    /* Test step 1: open succeeds, close doesn't crash */
    size_t len = 0;
    uint8_t *data = read_file("tests/fixtures/triangle.metallib", &len);
    ASSERT(data != NULL, "read_file triangle.metallib succeeded");

    lagfx_metallib_t *ml = lagfx_metallib_open(data, len);
    ASSERT(ml != NULL, "lagfx_metallib_open returned non-NULL");

    lagfx_metallib_close(ml);

    free(data);
    return 0;
}

static int test_list_functions(void) {
    /* Test step 2: list functions returns >= 1 */
    size_t len = 0;
    uint8_t *data = read_file("tests/fixtures/triangle.metallib", &len);
    ASSERT(data != NULL, "read_file triangle.metallib succeeded");

    lagfx_metallib_t *ml = lagfx_metallib_open(data, len);
    ASSERT(ml != NULL, "lagfx_metallib_open returned non-NULL");

    lagfx_metallib_function_t funcs[16];
    size_t count = lagfx_metallib_list_functions(ml, funcs, 16);
    ASSERT(count >= 1, "list_functions returned >= 1");

    for (size_t i = 0; i < count; ++i) {
        ASSERT(funcs[i].name != NULL, "function name is non-NULL");
        int first_char_ok = funcs[i].name[0] >= 32 && funcs[i].name[0] <= 126;
        ASSERT(first_char_ok, "function name starts with printable ASCII");
    }

    lagfx_metallib_close(ml);
    free(data);
    return 0;
}

static int test_null_data(void) {
    /* Test step 3: bad input - NULL data */
    lagfx_metallib_t *ml1 = lagfx_metallib_open(NULL, 0);
    ASSERT(ml1 == NULL, "lagfx_metallib_open(NULL, 0) returned NULL");

    lagfx_metallib_t *ml2 = lagfx_metallib_open(NULL, 1000);
    ASSERT(ml2 == NULL, "lagfx_metallib_open(NULL, 1000) returned NULL");

    return 0;
}

static int test_truncated(void) {
    /* Test step 4: bad input - truncated data */
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(i * 17 + 42);
    }

    lagfx_metallib_t *ml = lagfx_metallib_open(buf, 32);
    ASSERT(ml == NULL, "lagfx_metallib_open(truncated) returned NULL");

    return 0;
}

static int test_wrong_magic(void) {
    /* Test step 5: bad input - wrong magic */
    uint8_t buf[200];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = (uint8_t)(i * 43 + 13);
    }

    lagfx_metallib_t *ml = lagfx_metallib_open(buf, 200);
    ASSERT(ml == NULL, "lagfx_metallib_open(wrong magic) returned NULL");

    return 0;
}

static int test_close_null(void) {
    /* Test step 6: close is NULL-safe */
    lagfx_metallib_close(NULL);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (test_smoke_open())     return 1;
    if (test_list_functions()) return 1;
    if (test_null_data())      return 1;
    if (test_truncated())      return 1;
    if (test_wrong_magic())    return 1;
    if (test_close_null())     return 1;
    printf("metallib_reader: all tests passed\n");
    return 0;
}
