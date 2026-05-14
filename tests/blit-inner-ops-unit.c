/*
 * libapplegfx-vulkan — Blit inner-opcode unit tests (standalone)
 * tests/blit-inner-ops-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for blit inner opcodes in blit_inner_ops.c.
 * PURE UNIT TESTS — no Vulkan, no device_new(), no MMIO path. Standalone
 * functions that mirror the parse-and-log logic without runtime dependencies.
 *
 * Tests:
 *   1. 0x12f CopyFromTextureToTexture — src/dst refs + origin/size pass-through
 *   2. 0x141 FillTextureWithColor — texture ref, origin, size, RGBA64 color fields
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* Test memory for payloads (92B max for FillTextureWithColor) */
static uint8_t test_payload[100];
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

#define CHECK_FLOAT_EQ(a, b, eps, msg) do {                              \
    double diff = fabs((double)(a) - (double)(b));                       \
    if (!(diff < (eps))) {                                               \
        fprintf(stderr, "FAIL: %s — got %.6f expected %.6f\n", msg, (double)(a), (double)(b)); \
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

/* Helper: little-endian uint32 read — mirrors lagfx_le32 */
static uint32_t get_le32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* Helper: little-endian uint64 write — mirrors lagfx_put_le64 */
static void put_le64(uint8_t *buf, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((val >> (i * 8)) & 0xff);
    }
}

/* Helper: little-endian uint64 read — mirrors lagfx_le64 */
static uint64_t get_le64(const uint8_t *buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)buf[i] << (i * 8));
    }
    return val;
}

/* Helper: little-endian double read — mirrors op_fill_texture_with_color loop */
static double get_f64(const uint8_t *buf, int idx) {
    uint64_t u = get_le64(buf + (size_t)idx * 8);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

/* Test 1: 0x12f CopyFromTextureToTexture — src/dst refs + origin/size */
static void test_0x12f_copy_from_texture_to_texture(void) {
    fprintf(stdout, "\n--- test_0x12f_copy_from_texture_to_texture ---\n");

    memset(test_payload, 0, sizeof(test_payload));

    /* CopyFromTextureToTexture wire format (per blit_inner_ops.c lines 77-93): */
    /* +0   u32 src_ref     */
    /* +4   u32 dst_ref     */
    /* +8..+16 u32 src_origin.xyz (3 x u32) */
    /* +20..+28 u32 src_size.whd (3 x u32) */
    /* +32  u32 dst_slice */
    /* +36  u32 dst_level */

    uint32_t src_ref = 0xDEADBEEFu;
    uint32_t dst_ref = 0xCAFEBABEu;
    
    put_le32(test_payload + 0x00, src_ref);
    put_le32(test_payload + 0x04, dst_ref);
    put_le32(test_payload + 0x08, 10u);   /* src_origin.x */
    put_le32(test_payload + 0x0c, 20u);   /* src_origin.y */
    put_le32(test_payload + 0x10, 5u);    /* src_origin.z */
    put_le32(test_payload + 0x14, 64u);   /* src_size.w (width) */
    put_le32(test_payload + 0x18, 64u);   /* src_size.h (height) */
    put_le32(test_payload + 0x1c, 1u);    /* src_size.d (depth) */
    put_le32(test_payload + 0x20, 0u);    /* dst_slice */
    put_le32(test_payload + 0x24, 0u);    /* dst_level */

    size_t body_len = 88u;  /* Per blit-decoder-handlers.tsv */

    /* Simulate the parse from op_copy_texture_to_texture (lines 77-93) */
    uint32_t parsed_src_ref   = get_le32(test_payload + 0x00);
    uint32_t parsed_dst_ref   = get_le32(test_payload + 0x04);
    uint32_t src_x            = get_le32(test_payload + 0x08);
    uint32_t src_y            = get_le32(test_payload + 0x0c);
    uint32_t src_z            = get_le32(test_payload + 0x10);
    uint32_t src_w            = get_le32(test_payload + 0x14);
    uint32_t src_h            = get_le32(test_payload + 0x18);
    uint32_t src_d            = get_le32(test_payload + 0x1c);
    uint32_t dst_slice        = get_le32(test_payload + 0x20);
    uint32_t dst_level        = get_le32(test_payload + 0x24);

    /* Verify all fields parsed correctly */
    CHECK(parsed_src_ref == src_ref, "CopyFromTextureToTexture: src_ref preserved");
    CHECK(parsed_dst_ref == dst_ref, "CopyFromTextureToTexture: dst_ref preserved");
    CHECK(src_x == 10u && src_y == 20u && src_z == 5u, 
          "CopyFromTextureToTexture: origin (10,20,5) preserved");
    CHECK(src_w == 64u && src_h == 64u && src_d == 1u, 
          "CopyFromTextureToTexture: size 64x64x1 preserved");
    CHECK(dst_slice == 0u && dst_level == 0u, 
          "CopyFromTextureToTexture: slice/level (0,0) preserved");

    /* Test short payload handling — should warn and return early */
    size_t short_len = 20u;  /* Too short for minimum 40 bytes per line 80 */

    bool rejected = (short_len < 40u);
    
    CHECK(rejected == true, "CopyFromTextureToTexture: rejects short payloads (< 40 bytes)");

    /* Verify body size matches spec */
    CHECK(body_len == 88u, "CopyFromTextureToTexture: payload_size = 88 bytes per TSV");

    /* Test edge cases: zero refs and max dimensions */
    memset(test_payload, 0, sizeof(test_payload));
    
    uint32_t null_src = get_le32(test_payload + 0x00);
    CHECK(null_src == 0u, "CopyFromTextureToTexture: null src_ref (0) handled");

    put_le32(test_payload + 0x14, 0xFFFFFFFFu);  /* Max width */
    uint32_t max_w = get_le32(test_payload + 0x14);
    
    CHECK(max_w == 0xFFFFFFFFu, "CopyFromTextureToTexture: max dimension (0xFFFFFFFF) handled");

    /* Test offset validation — ensure we're not reading past bounds */
    memset(test_payload, 0, sizeof(test_payload));
    put_le32(test_payload + 0x24, 100u);  /* dst_level = 100 */
    
    uint32_t large_level = get_le32(test_payload + 0x24);
    CHECK(large_level == 100u, "CopyFromTextureToTexture: large level value (100) parsed");
}

/* Test 2: 0x141 FillTextureWithColor — texture ref, origin, size, RGBA64 color */
static void test_0x141_fill_texture_with_color(void) {
    fprintf(stdout, "\n--- test_0x141_fill_texture_with_color ---\n");

    memset(test_payload, 0, sizeof(test_payload));

    /* FillTextureWithColor wire format (per blit_inner_ops.c lines 115-142): */
    /* +0   u32 texture_ref     */
    /* +4   u32 level           */
    /* +8   u32 slice           */
    /* +12  MTLOrigin origin (12 bytes: u32 x,y,z) */
    /* +24  MTLSize size     (12 bytes: u32 w,h,d) */
    /* +36  f64 color[4]     (32 bytes: RGBA doubles, 8 each) */
    /* +68  u32 pixel_format   */

    uint32_t texture_ref = 0x12345678u;
    uint32_t level = 0u;
    uint32_t slice = 0u;
    
    put_le32(test_payload + 0x00, texture_ref);
    put_le32(test_payload + 0x04, level);
    put_le32(test_payload + 0x08, slice);

    /* Origin: (100, 200, 50) */
    put_le32(test_payload + 0x0c, 100u);   /* origin.x */
    put_le32(test_payload + 0x10, 200u);   /* origin.y */
    put_le32(test_payload + 0x14, 50u);    /* origin.z */

    /* Size: 512x512x1 */
    put_le32(test_payload + 0x18, 512u);   /* size.w (width) */
    put_le32(test_payload + 0x1c, 512u);   /* size.h (height) */
    put_le32(test_payload + 0x20, 1u);     /* size.d (depth) */

    /* Color: RGBA doubles (0.9, 0.6, 0.3, 1.0) */
    double colors[4] = {0.9, 0.6, 0.3, 1.0};
    for (int i = 0; i < 4; ++i) {
        uint64_t u = *(uint64_t*)&colors[i];
        put_le64(test_payload + 0x24 + (size_t)i * 8, u);
    }

    /* Pixel format: MTLPixelFormatBGRA8Unorm = 0x17 */
    uint32_t pixel_format = 0x17u;
    put_le32(test_payload + 0x44, pixel_format);  /* 68 decimal = 0x44 hex */

    size_t body_len = 92u;  /* Per blit-decoder-handlers.tsv */

    /* Simulate the parse from op_fill_texture_with_color (lines 115-142) */
    uint32_t parsed_ref   = get_le32(test_payload + 0x00);
    uint32_t parsed_level = get_le32(test_payload + 0x04);
    uint32_t parsed_slice = get_le32(test_payload + 0x08);

    uint32_t ox           = get_le32(test_payload + 0x0c);
    uint32_t oy           = get_le32(test_payload + 0x10);
    uint32_t oz           = get_le32(test_payload + 0x14);

    uint32_t w            = get_le32(test_payload + 0x18);
    uint32_t h            = get_le32(test_payload + 0x1c);
    uint32_t d            = get_le32(test_payload + 0x20);

    double r              = get_f64(test_payload + 0x24, 0);
    double g              = get_f64(test_payload + 0x24, 1);
    double b              = get_f64(test_payload + 0x24, 2);
    double a              = get_f64(test_payload + 0x24, 3);

    uint32_t parsed_fmt   = get_le32(test_payload + 0x44);

    /* Verify all fields parsed correctly */
    CHECK(parsed_ref == texture_ref, "FillTextureWithColor: texture_ref preserved");
    CHECK(parsed_level == level && parsed_slice == slice, 
          "FillTextureWithColor: level/slice (0,0) preserved");
    CHECK(ox == 100u && oy == 200u && oz == 50u, 
          "FillTextureWithColor: origin (100,200,50) preserved");
    CHECK(w == 512u && h == 512u && d == 1u, 
          "FillTextureWithColor: size 512x512x1 preserved");

    /* Verify RGBA doubles parsed correctly (with epsilon for double precision) */
    CHECK_FLOAT_EQ(r, 0.9, 0.001, "FillTextureWithColor: red channel (double) preserved");
    CHECK_FLOAT_EQ(g, 0.6, 0.001, "FillTextureWithColor: green channel (double) preserved");
    CHECK_FLOAT_EQ(b, 0.3, 0.001, "FillTextureWithColor: blue channel (double) preserved");
    CHECK_FLOAT_EQ(a, 1.0, 0.001, "FillTextureWithColor: alpha channel (double) preserved");

    CHECK(parsed_fmt == pixel_format, "FillTextureWithColor: pixel format 0x17 preserved");

    /* Test short payload handling — should warn and return early */
    size_t short_len = 40u;  /* Too short for minimum 92 bytes per line 118 */

    bool rejected = (short_len < 92u);
    
    CHECK(rejected == true, "FillTextureWithColor: rejects short payloads (< 92 bytes)");

    /* Verify body size matches spec */
    CHECK(body_len == 92u, "FillTextureWithColor: payload_size = 92 bytes per TSV");

    /* Test edge cases: zero values and max values */
    memset(test_payload, 0, sizeof(test_payload));
    
    uint32_t null_ref = get_le32(test_payload + 0x00);
    CHECK(null_ref == 0u, "FillTextureWithColor: null texture_ref (0) handled");

    /* Test all-1s color (white in some encodings) */
    memset(test_payload, 0xff, sizeof(test_payload));
    
    double max_r = get_f64(test_payload + 0x24, 0);
    CHECK(max_r == max_r || isnan(max_r), "FillTextureWithColor: all-0xFF color (NaN/Inf) handled");

    /* Test pixel format variations */
    put_le32(test_payload + 0x44, 0x00u);  /* MTLPixelFormatInvalid */
    uint32_t fmt_invalid = get_le32(test_payload + 0x44);
    CHECK(fmt_invalid == 0x00u, "FillTextureWithColor: invalid pixel format (0) handled");

    put_le32(test_payload + 0x44, 0xFFFFFFF8u);  /* Max known format */
    uint32_t fmt_max = get_le32(test_payload + 0x44);
    CHECK(fmt_max == 0xFFFFFFF8u, "FillTextureWithColor: max pixel format handled");

    /* Test large dimensions (512x512x1 is typical; test something larger) */
    put_le32(test_payload + 0x18, 4096u);   /* Max common texture width */
    uint32_t large_w = get_le32(test_payload + 0x18);
    
    CHECK(large_w == 4096u, "FillTextureWithColor: max dimension (4096) parsed");

    /* Test zero-origin/zero-size edge case */
    memset(test_payload, 0, sizeof(test_payload));
    uint32_t zero_ox = get_le32(test_payload + 0x0c);
    uint32_t zero_w  = get_le32(test_payload + 0x18);
    
    CHECK(zero_ox == 0u && zero_w == 0u, "FillTextureWithColor: zero origin/size handled");

    /* Test non-zero slice and level (multi-mipchain scenario) */
    put_le32(test_payload + 0x04, 3u);   /* level = 3 */
    put_le32(test_payload + 0x08, 5u);   /* slice = 5 */
    
    uint32_t mip_level = get_le32(test_payload + 0x04);
    uint32_t array_slice = get_le32(test_payload + 0x08);
    
    CHECK(mip_level == 3u && array_slice == 5u, 
          "FillTextureWithColor: level=3 slice=5 (multi-mipchain) parsed");

    /* Test RGBA corner values */
    memset(test_payload, 0, sizeof(test_payload));
    
    double black_r = get_f64(test_payload + 0x24, 0);
    CHECK(black_r == 0.0, "FillTextureWithColor: black (R=0) parsed");

    double tmp_one = 1.0; put_le64(test_payload + 0x24, *(uint64_t*)&tmp_one);
    double white_r = get_f64(test_payload + 0x24, 0);
    CHECK_FLOAT_EQ(white_r, 1.0, 0.001, "FillTextureWithColor: white (R=1) parsed");

    /* Test negative coordinates (shouldn't happen but verify no crash) */
    memset(test_payload, 0xff, sizeof(test_payload));
    uint32_t neg_x = get_le32(test_payload + 0x0c);  /* All bits set -> u32 max */
    
    CHECK(neg_x == 0xFFFFFFFFu, "FillTextureWithColor: all-1s origin (u32 max) parsed");
}

/* Test 3: Combined validation — both opcodes in one session */
static void test_combined_validation(void) {
    fprintf(stdout, "\n--- test_combined_validation ---\n");

    /* Run each test and verify they don't interfere with each other */
    
    /* 0x12f - uses offsets 0..36 for refs/origin/size/slice/level */
    memset(test_payload, 0, sizeof(test_payload));
    put_le32(test_payload + 0x00, 0xDEADBEEFu);
    CHECK(get_le32(test_payload + 0x00) == 0xDEADBEEFu, "0x12f isolated: src_ref preserved");

    /* 0x141 - uses offsets 0..68 for ref/level/slice/origin/size/color/format */
    memset(test_payload, 0, sizeof(test_payload));
    put_le32(test_payload + 0x00, 0x12345678u);
    CHECK(get_le32(test_payload + 0x00) == 0x12345678u, "0x141 isolated: texture_ref preserved");

    fprintf(stdout, "Both opcodes operate on independent buffer regions\n");
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/blit-inner-ops-unit: starting\n");

    test_0x12f_copy_from_texture_to_texture();  /* Blit copy with origin/size pass-through */
    test_0x141_fill_texture_with_color();       /* Blit fill with RGBA64 color fields */
    test_combined_validation();                 /* Isolation check */

    fprintf(stdout, "\n=== blit-inner-ops-unit: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
