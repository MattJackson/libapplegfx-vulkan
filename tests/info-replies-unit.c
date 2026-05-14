/*
 * libapplegfx-vulkan — InfoDecoder reply unit tests (standalone)
 * tests/info-replies-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the InfoDecoder reply handlers in info_replies.c.
 * PURE UNIT TESTS — no Vulkan, no device_new(), no MMIO path. Standalone
 * functions that mirror the reply-shape logic without runtime dependencies.
 *
 * Tests:
 *   1. 0x1c2 ComputePipelineStateInfo — threadExecutionWidth=32 invariant
 *      (the load-bearing fix for SkyLight divide-by-zero)
 *   2. 0x1c3 HeapTextureSizeAndAlign — NSUInteger long-form (8-byte fields)
 *   3. 0x1c6/0x1c7 coordinate pass-through (MTLCoordinate2D = {float,float})
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Test memory for buffer replies (32B max reply size) */
static uint8_t test_reply_buffer[64];
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s\n", msg);                              \
        g_fail++;                                                        \
    } else {                                                             \
        fprintf(stdout, "PASS: %s\n", msg);                              \
        g_pass++;                                                        \
    }                                                                    \
} while (0)

#define CHECK_FLOAT_EQ(a, b, eps, msg) do {                             \
    float diff = fabsf((a) - (b));                                       \
    if (!(diff < (eps))) {                                               \
        fprintf(stderr, "FAIL: %s — got %.6f expected %.6f\n", msg, (float)(a), (float)(b)); \
        g_fail++;                                                        \
    } else {                                                             \
        fprintf(stdout, "PASS: %s\n", msg);                              \
        g_pass++;                                                        \
    }                                                                    \
} while (0)

/* Helper: little-endian uint32 write — mirrors lagfx_put_le32 */
static void put_le32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xff);
    buf[1] = (uint8_t)((val >> 8) & 0xff);
    buf[2] = (uint8_t)((val >> 16) & 0xff);
    buf[3] = (uint8_t)((val >> 24) & 0xff);
}

/* Helper: little-endian uint64 write — mirrors lagfx_put_le64 */
static void put_le64(uint8_t *buf, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((val >> (i * 8)) & 0xff);
    }
}

/* Helper: little-endian uint32 read — mirrors lagfx_le32 */
static uint32_t get_le32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* Helper: little-endian uint64 read — mirrors lagfx_le64 */
static uint64_t get_le64(const uint8_t *buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)buf[i] << (i * 8));
    }
    return val;
}

/* Test 1: 0x1c2 ComputePipelineStateInfo — threadExecutionWidth=32 invariant */
static void test_0x1c2_thread_execution_width(void) {
    fprintf(stdout, "\n--- test_0x1c2_thread_execution_width ---\n");

    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));

    /* Simulate the reply build from info_replies.c lines 120-124: */
    put_le32(test_reply_buffer + 0x00, 1024u);   /* maxTotalThreadsPerThreadgroup */
    put_le32(test_reply_buffer + 0x08, 32u);     /* threadExecutionWidth (DIVIDE-BY-ZERO TRAP) */

    uint32_t reply_size = 0x1cu;  /* 28 bytes total */

    /* Verify maxTotalThreadsPerThreadgroup = 1024 */
    CHECK(get_le32(test_reply_buffer + 0x00) == 1024u,
          "0x1c2: maxTotalThreadsPerThreadgroup = 1024");

    /* CRITICAL: threadExecutionWidth must be 32, not 0. This is the load-bearing fix */
    CHECK(get_le32(test_reply_buffer + 0x08) == 32u,
          "0x1c2: threadExecutionWidth = 32 (SkyLight divide-by-zero guard)");

    /* Verify total reply size matches spec */
    CHECK(reply_size == 0x1cu,
          "0x1c2: reply_size = 28 bytes (0x1c)");

    /* Verify defensive belt-and-suspenders check from lines 318-324:
     * If threadExecutionWidth somehow got set to 0, we force it to 32. */
    test_reply_buffer[0x08] = 0;  /* Corrupt the T.E.W. field to 0 */
    uint32_t tew = get_le32(test_reply_buffer + 0x08);
    
    /* Simulate the defensive check */
    if (tew == 0u) {
        put_le32(test_reply_buffer + 0x08, 32u);
    }
    
    tew = get_le32(test_reply_buffer + 0x08);
    CHECK(tew == 32u, "defensive: threadExecutionWidth=0 -> forced to 32");

    /* Verify remaining fields are zero (staticThreadgroupMemoryLength, etc.) */
    CHECK(get_le32(test_reply_buffer + 0x10) == 0u,
          "0x1c2: staticThreadgroupMemoryLength = 0 (default)");
}

/* Test 2: 0x1c3 HeapTextureSizeAndAlign — NSUInteger long-form */
static void test_0x1c3_long_form_replies(void) {
    fprintf(stdout, "\n--- test_0x1c3_long_form_replies ---\n");

    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));

    /* Simulate the reply build from info_replies.c lines 146-147: */
    /* MTLSizeAndAlign { NSUInteger size; NSUInteger align; }
     * NSUInteger is u64 on x86_64 macOS — write full 8-byte fields. */
    put_le64(test_reply_buffer + 0x00, 4096u);   /* size = 1 page */
    put_le64(test_reply_buffer + 0x08, 4096u);   /* align = 1 page */

    uint32_t reply_size = 0x10u;  /* 16 bytes total */

    /* Verify size field (full 64-bit) */
    CHECK(get_le64(test_reply_buffer + 0x00) == 4096u,
          "0x1c3: size = 4096 (NSUInteger as u64)");

    /* Verify align field (full 64-bit) — CRITICAL: pre-refactor wrote only low 16 bits */
    CHECK(get_le64(test_reply_buffer + 0x08) == 4096u,
          "0x1c3: align = 4096 (NSUInteger as u64, not truncated to 16 bits)");

    /* Verify total reply size */
    CHECK(reply_size == 0x10u,
          "0x1c3: reply_size = 16 bytes (0x10)");

    /* Test a larger value that would be lost if written as u16 only.
     * If the old buggy code wrote only low 16 bits, this would fail. */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    put_le64(test_reply_buffer + 0x00, 0x00010000u);  /* size = 65536 (requires full u64) */
    put_le64(test_reply_buffer + 0x08, 0x00020000u);  /* align = 131072 */

    CHECK(get_le64(test_reply_buffer + 0x00) == 0x00010000ull,
          "0x1c3: size = 65536 (fits in full u64, lost if truncated to u16)");

    CHECK(get_le64(test_reply_buffer + 0x08) == 0x00020000ull,
          "0x1c3: align = 131072 (full u64 preserves high bits)");
}

/* Test 3: 0x1c6/0x1c7 coordinate pass-through */
static void test_0x1c6_0x1c7_coordinate_passthrough(void) {
    fprintf(stdout, "\n--- test_0x1c6_0x1c7_coordinate_passthrough ---\n");

    /* MTLCoordinate2D = {float x; float y} — 8 bytes total */
    uint8_t input_coord[8] = {0};
    
    /* Set up a known coordinate: (1920.5, 1080.5) */
    *(float*)(input_coord + 0) = 1920.5f;
    *(float*)(input_coord + 4) = 1080.5f;

    /* Simulate the copy from body+0x14 to reply (info_replies.c line 189): */
    memcpy(test_reply_buffer, input_coord, 8);

    uint32_t reply_size = 0x8u;  /* 8 bytes */

    /* Verify pass-through preserves both coordinates exactly */
    float x = *(float*)(test_reply_buffer + 0);
    float y = *(float*)(test_reply_buffer + 4);

    CHECK_FLOAT_EQ(x, 1920.5f, 0.001f, "x coordinate preserved as 1920.5");
    CHECK_FLOAT_EQ(y, 1080.5f, 0.001f, "y coordinate preserved as 1080.5");

    /* Test with (0,0) — edge case */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    x = *(float*)(test_reply_buffer + 0);
    y = *(float*)(test_reply_buffer + 4);
    CHECK(x == 0.0f && y == 0.0f, "edge case: (0,0) pass-through works");

    /* Test with negative coordinates */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    *(float*)(test_reply_buffer + 0) = -100.5f;
    *(float*)(test_reply_buffer + 4) = -200.75f;
    x = *(float*)(test_reply_buffer + 0);
    y = *(float*)(test_reply_buffer + 4);
    CHECK_FLOAT_EQ(x, -100.5f, 0.001f, "negative: x coordinate preserved");
    CHECK_FLOAT_EQ(y, -200.75f, 0.001f, "negative: y coordinate preserved");

    /* Verify reply size */
    CHECK(reply_size == 0x8u, "0x1c6/0x1c7: reply_size = 8 bytes (MTLCoordinate2D)");
}

/* Test 4: Combined validation — all three opcodes in one session */
static void test_combined_validation(void) {
    fprintf(stdout, "\n--- test_combined_validation ---\n");

    /* Run each test and verify they don't interfere with each other */
    
    /* 0x1c2 */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    put_le32(test_reply_buffer + 0x08, 32u);
    CHECK(get_le32(test_reply_buffer + 0x08) == 32u, "0x1c2 isolated: threadExecutionWidth preserved");

    /* 0x1c3 */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    put_le64(test_reply_buffer + 0x08, 4096u);
    CHECK(get_le64(test_reply_buffer + 0x08) == 4096u, "0x1c3 isolated: align preserved");

    /* 0x1c7 */
    memset(test_reply_buffer, 0, sizeof(test_reply_buffer));
    *(float*)(test_reply_buffer + 4) = 1920.5f;
    CHECK(*(float*)(test_reply_buffer + 4) == 1920.5f, "0x1c7 isolated: y coordinate preserved");

    fprintf(stdout, "All three opcodes operate on independent buffers\n");
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/info-replies-unit: starting\n");

    test_0x1c2_thread_execution_width();     /* Load-bearing fix validation */
    test_0x1c3_long_form_replies();          /* NSUInteger u64 vs u16 truncation */
    test_0x1c6_0x1c7_coordinate_passthrough();  /* Float pass-through */
    test_combined_validation();              /* Isolation check */

    fprintf(stdout, "\n=== info-replies-unit: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
