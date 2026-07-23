/*
 * libapplegfx-vulkan — pipeline descriptor function-ref scan unit test
 * tests/object-resolver-scan-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Red-on-revert regression for the login-composite substitution bug: the old
 * two-ref lookup assumed "first function ref = vertex, second = fragment" and
 * only scanned the first 128 bytes of the pipeline descriptor. Real login
 * composites list the SHARED TextureCopy fragment ref (0x1f) BEFORE the vertex
 * ref, and the vertex ref often lives past byte 128 in the 0x3c..0x154-byte
 * descriptor. The old scan therefore returned {0x1f=vertex(WRONG), junk} and
 * logged "no vertex function" → every composite draw substituted (the login
 * scene never composited to the scanout).
 *
 * lagfx_scan_descriptor_function_refs() is the fix's core: collect ALL
 * function-typed `0x04 XX` refs over the full descriptor length, in order, so
 * the caller can bind each stage from whichever ref's metallib holds it. These
 * tests pin the exact properties that were broken.
 */
#include "protocol/object_resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); g_failures++; } \
    else         { fprintf(stdout, "ok: %s\n", (msg)); } \
} while (0)

/* Accept a fixed set of "function-typed" refs (mirrors slot type == 0x06). */
static uint8_t g_func_refs[8];
static size_t  g_n_func_refs;
static bool is_func(void *ctx, uint8_t ref) {
    (void)ctx;
    for (size_t i = 0; i < g_n_func_refs; i++)
        if (g_func_refs[i] == ref) return true;
    return false;
}
static void set_func_refs(const uint8_t *refs, size_t n) {
    memcpy(g_func_refs, refs, n);
    g_n_func_refs = n;
}

/* Put a `04 XX` token at byte offset off. */
static void put_tok(uint8_t *d, uint32_t off, uint8_t ref) {
    d[off] = 0x04u; d[off + 1] = ref;
}

/* The real failing shape: fragment ref 0x1f first, vertex ref 0x22 later and
 * PAST byte 128. Old scan (first-2, <128B) returns {0x1f, <none>} → no vertex.
 * The list must contain BOTH, with the fragment first. */
static void test_fragment_first_vertex_past_128(void) {
    fprintf(stdout, "\n--- test: fragment-first, vertex past 128B ---\n");
    uint8_t desc[384];
    memset(desc, 0, sizeof(desc));
    put_tok(desc, 4 + 0, 0);            /* length field region; harmless */
    put_tok(desc, 40, 0x1f);            /* fragment ref, early */
    put_tok(desc, 200, 0x22);           /* vertex ref, past old 128B cap */
    uint8_t only[] = { 0x1f, 0x22 };
    set_func_refs(only, 2);

    uint8_t out[16] = {0};
    size_t n = lagfx_scan_descriptor_function_refs(desc, sizeof(desc), is_func, NULL, out, 16);
    CHECK(n == 2, "collects both refs across full descriptor");
    CHECK(n == 2 && out[0] == 0x1f, "fragment ref (0x1f) seen first");
    CHECK(n == 2 && out[1] == 0x22, "vertex ref (0x22) collected despite being past byte 128");
}

/* Non-function 0x04 XX tokens (offsets, strides) must be filtered out. */
static void test_filters_non_function_tokens(void) {
    fprintf(stdout, "\n--- test: filter non-function tokens ---\n");
    uint8_t desc[128];
    memset(desc, 0, sizeof(desc));
    put_tok(desc, 16, 0x30);   /* stride/offset byte, NOT a function */
    put_tok(desc, 24, 0x22);   /* real vertex function */
    put_tok(desc, 32, 0x50);   /* NOT a function (extractor-fail decoy) */
    put_tok(desc, 40, 0x1f);   /* real fragment function */
    uint8_t only[] = { 0x22, 0x1f };
    set_func_refs(only, 2);

    uint8_t out[16] = {0};
    size_t n = lagfx_scan_descriptor_function_refs(desc, sizeof(desc), is_func, NULL, out, 16);
    CHECK(n == 2, "only the two function-typed refs are collected");
    CHECK(n == 2 && out[0] == 0x22 && out[1] == 0x1f, "collected in byte order, non-func skipped");
}

/* Repeated refs collapse; cap and limit are respected. */
static void test_dedup_cap_limit(void) {
    fprintf(stdout, "\n--- test: dedup / cap / limit ---\n");
    uint8_t desc[256];
    memset(desc, 0, sizeof(desc));
    put_tok(desc, 8,  0x1f);
    put_tok(desc, 20, 0x1f);   /* duplicate → collapses */
    put_tok(desc, 60, 0x22);
    put_tok(desc, 240, 0x99);  /* beyond a tighter limit */
    uint8_t only[] = { 0x1f, 0x22, 0x99 };
    set_func_refs(only, 3);

    uint8_t out[16] = {0};
    size_t n = lagfx_scan_descriptor_function_refs(desc, /*limit=*/200, is_func, NULL, out, 16);
    CHECK(n == 2, "duplicate 0x1f collapses; token past limit excluded");
    CHECK(n == 2 && out[0] == 0x1f && out[1] == 0x22, "dedup preserves first-seen order");

    /* cap respected */
    uint8_t small[1] = {0};
    size_t n2 = lagfx_scan_descriptor_function_refs(desc, 200, is_func, NULL, small, 1);
    CHECK(n2 == 1 && small[0] == 0x1f, "cap bounds the collected count");
}

/* NULL is_function accepts every nonzero XX (defensive default). */
static void test_null_filter_accepts_all(void) {
    fprintf(stdout, "\n--- test: NULL filter accepts all nonzero ---\n");
    uint8_t desc[64];
    memset(desc, 0, sizeof(desc));
    put_tok(desc, 10, 0x1f);
    put_tok(desc, 12, 0x00);   /* zero ref skipped */
    put_tok(desc, 14, 0x22);
    uint8_t out[16] = {0};
    size_t n = lagfx_scan_descriptor_function_refs(desc, sizeof(desc), NULL, NULL, out, 16);
    CHECK(n == 2 && out[0] == 0x1f && out[1] == 0x22, "nonzero refs collected, zero skipped");
}

int main(void) {
    test_fragment_first_vertex_past_128();
    test_filters_non_function_tokens();
    test_dedup_cap_limit();
    test_null_filter_accepts_all();
    if (g_failures) {
        fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    fprintf(stdout, "\nall object-resolver scan checks passed\n");
    return 0;
}
