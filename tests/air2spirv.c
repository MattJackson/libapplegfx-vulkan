/*
 * libapplegfx-vulkan — air2spirv extraction + retarget smoke (Phase 3.C.2)
 * tests/air2spirv.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises src/air2spirv/ with synthesised fixtures so we don't
 * depend on the Phase 0 metallib corpus (which lives in the mos
 * tree, not in the library repo). Three cases:
 *
 *   (A) Metallib extractor:
 *       - Synthesise a minimal MTLB header + one function entry
 *         (NAME + TYPE + OFFT + MDSZ + ENDT) + a 16-byte "LLVM
 *         Bitcode" payload (wrapper magic only — not a valid
 *         module, but we only care about extraction).
 *       - Call extract_functions; assert count=1, name matches,
 *         stage matches, bitcode pointer + length match.
 *
 *   (B) Metallib extractor corner cases:
 *       - NULL buffer → ERR_INVALID_ARG.
 *       - Truncated (too short for FET offset) → ERR_INVALID_ARG.
 *       - No MTLB magic → ERR_INVALID_ARG.
 *       - FET offset past EOF → ERR_INVALID_ARG.
 *
 *   (C) Bitcode retarget:
 *       - Synthesise an LLVM Bitcode wrapper header + embedded
 *         `air64-apple-macosx26.3.0` triple.
 *       - Call retarget_to_spirv; assert the output contains
 *         `spir64-unknown-vulkan1.3` and no longer contains
 *         the AIR triple.
 *       - NULL inputs → ERR_INVALID_ARG.
 *       - No AIR pattern in bitcode → ERR_PROTOCOL.
 *       - Unknown format (no bitcode magic) → ERR_INVALID_ARG.
 *
 * Pure in-process test, no Vulkan, no file IO. Runs on every
 * host.
 */

#include "libapplegfx-vulkan.h"
#include "air2spirv/metallib_extract.h"
#include "air2spirv/bitcode_retarget.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
    } \
} while (0)

/* --- Little-endian write helpers ------------------------------ */

static void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8)  & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
static void wr_u64_le(uint8_t *p, uint64_t v) {
    wr_u32_le(p,     (uint32_t)(v & 0xffffffffull));
    wr_u32_le(p + 4, (uint32_t)((v >> 32) & 0xffffffffull));
}

/* --- Fixture: synth MTLB buffer ------------------------------ */

/* Layout (offsets):
 *   0x00 'M' 'T' 'L' 'B'
 *   0x04 .. 0x17  padding / version (ignored at parse time)
 *   0x18 u64 FET_OFFSET
 *   0x20 bitcode payload (16 bytes: wrapper magic + 12 zeros)
 *   0x30 FET: u32 entry_count (=1)
 *   0x34 entry record:
 *         NAME len=5 "blit\0"
 *         TYPE len=1 1 (VERTEX)
 *         OFFT len=8 0x20
 *         MDSZ len=8 0x10
 *         ENDT len=0 -
 *   end.
 *
 * All tag lengths are VBR-style u32 length fields (as documented).
 */
#define MTLB_BUF_LEN 128u

static size_t make_synth_mtlb(uint8_t *buf, size_t cap,
                              const char *name,
                              uint8_t stage,
                              size_t *out_bc_offset,
                              size_t *out_bc_len) {
    memset(buf, 0, cap);
    buf[0] = 'M'; buf[1] = 'T'; buf[2] = 'L'; buf[3] = 'B';

    const uint64_t fet_offset = 0x30ull;
    const uint64_t bc_offset  = 0x20ull;
    const uint64_t bc_len     = 0x10ull;

    wr_u64_le(buf + 0x18, fet_offset);

    /* Bitcode payload: LLVM wrapper magic + pad. */
    static const uint8_t bc_magic[4] = { 0xDE, 0xC0, 0x17, 0x0B };
    memcpy(buf + bc_offset, bc_magic, 4u);
    /* Rest stays zero — we only test extraction, not bitcode
     * parsing. */

    /* FET: u32 entry_count = 1. */
    size_t p = (size_t)fet_offset;
    wr_u32_le(buf + p, 1u);
    p += 4u;

    /* Entry record: NAME tag. */
    const size_t name_len = strlen(name) + 1u;  /* include NUL */
    memcpy(buf + p, "NAME", 4u);
    wr_u32_le(buf + p + 4u, (uint32_t)name_len);
    memcpy(buf + p + 8u, name, name_len);
    p += 8u + name_len;

    /* TYPE. */
    memcpy(buf + p, "TYPE", 4u);
    wr_u32_le(buf + p + 4u, 1u);
    buf[p + 8u] = stage;
    p += 9u;

    /* OFFT. */
    memcpy(buf + p, "OFFT", 4u);
    wr_u32_le(buf + p + 4u, 8u);
    wr_u64_le(buf + p + 8u, bc_offset);
    p += 16u;

    /* MDSZ. */
    memcpy(buf + p, "MDSZ", 4u);
    wr_u32_le(buf + p + 4u, 8u);
    wr_u64_le(buf + p + 8u, bc_len);
    p += 16u;

    /* ENDT. */
    memcpy(buf + p, "ENDT", 4u);
    wr_u32_le(buf + p + 4u, 0u);
    p += 8u;

    if (out_bc_offset) *out_bc_offset = (size_t)bc_offset;
    if (out_bc_len)    *out_bc_len    = (size_t)bc_len;
    return p;
}

/* --- Test (A): extraction happy path -------------------------- */

static void test_extract_happy(void) {
    uint8_t buf[MTLB_BUF_LEN];
    size_t bc_off = 0, bc_len_x = 0;
    size_t used = make_synth_mtlb(buf, sizeof(buf), "blit",
                                   1u /* VERTEX */,
                                   &bc_off, &bc_len_x);
    CHECK(used > 0u && used <= sizeof(buf),
          "synth MTLB fits the buffer");

    lagfx_metallib_function_t fn;
    memset(&fn, 0, sizeof(fn));
    size_t n = 0;
    lagfx_status_t st = lagfx_metallib_extract_functions(
        buf, sizeof(buf), &fn, 1u, &n);
    CHECK(st == LAGFX_OK, "extract happy path returns LAGFX_OK");
    CHECK(n == 1u, "extract returns count=1");
    CHECK(strcmp(fn.name, "blit") == 0, "extracted name matches");
    CHECK(fn.stage == LAGFX_METALLIB_STAGE_VERTEX,
          "extracted stage is VERTEX");
    CHECK(fn.bitcode != NULL, "extracted bitcode pointer non-NULL");
    CHECK(fn.bitcode_len == bc_len_x, "extracted bitcode length matches");
    if (fn.bitcode) {
        CHECK(fn.bitcode[0] == 0xDE && fn.bitcode[1] == 0xC0
              && fn.bitcode[2] == 0x17 && fn.bitcode[3] == 0x0B,
              "extracted bitcode starts with LLVM wrapper magic");
    }
}

/* --- Test (B): extraction corner cases ------------------------ */

static void test_extract_corners(void) {
    size_t n = 0;
    lagfx_metallib_function_t fn;
    memset(&fn, 0, sizeof(fn));

    /* NULL buf. */
    lagfx_status_t st = lagfx_metallib_extract_functions(
        NULL, 0, &fn, 1, &n);
    CHECK(st == LAGFX_ERR_INVALID_ARG, "NULL buf → INVALID_ARG");

    /* Buffer too small for header. */
    uint8_t tiny[8] = { 'M', 'T', 'L', 'B', 0, 0, 0, 0 };
    st = lagfx_metallib_extract_functions(tiny, sizeof(tiny), &fn, 1, &n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "too-small buf → INVALID_ARG");

    /* No MTLB magic. */
    uint8_t nomagic[64];
    memset(nomagic, 0, sizeof(nomagic));
    st = lagfx_metallib_extract_functions(nomagic, sizeof(nomagic),
                                          &fn, 1, &n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "missing MTLB magic → INVALID_ARG");
    CHECK(!lagfx_metallib_has_magic(nomagic, sizeof(nomagic)),
          "has_magic returns false for non-MTLB buffer");
    CHECK(lagfx_metallib_has_magic((const uint8_t *)"MTLB" "\0\0\0\0", 8u),
          "has_magic returns true for MTLB-prefixed buffer");

    /* FET offset past end of buffer. */
    uint8_t badfet[64];
    memset(badfet, 0, sizeof(badfet));
    memcpy(badfet, "MTLB", 4u);
    wr_u64_le(badfet + 0x18, 0xfffffff0ull);  /* huge offset */
    st = lagfx_metallib_extract_functions(badfet, sizeof(badfet),
                                          &fn, 1, &n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "FET offset past EOF → INVALID_ARG");
}

/* --- Test (C): bitcode retarget ------------------------------- */

/* Synth bitcode buffer: wrapper magic + 12 bytes padding +
 * the embedded AIR triple string + a trailing block. Total
 * length picked to keep the test deterministic. */
static size_t make_synth_bitcode(uint8_t *buf, size_t cap,
                                 const char *triple) {
    memset(buf, 0, cap);
    buf[0] = 0xDE; buf[1] = 0xC0; buf[2] = 0x17; buf[3] = 0x0B;
    /* Pad with 12 zero bytes (stand-in for wrapper header). */
    size_t pos = 16u;
    size_t tl  = strlen(triple);
    if (pos + tl >= cap) {
        return 0;
    }
    memcpy(buf + pos, triple, tl);
    pos += tl;
    /* Trailing marker so we can detect the retargeter doesn't
     * clobber data outside the triple. */
    if (pos + 4u < cap) {
        memcpy(buf + pos, "END", 4u);
        pos += 4u;
    }
    return pos;
}

static bool buf_contains(const uint8_t *p, size_t n, const char *s) {
    size_t sl = strlen(s);
    if (n < sl) return false;
    for (size_t i = 0; i + sl <= n; ++i) {
        if (memcmp(p + i, s, sl) == 0) return true;
    }
    return false;
}

static void test_retarget(void) {
    /* Tier 1 path: AIR triple longer than replacement —
     * `air64_v28-apple-macosx26.3.0` is 30 chars,
     * `spir64-unknown-vulkan1.3` is 24 chars. */
    uint8_t in[128];
    size_t n = make_synth_bitcode(in, sizeof(in),
                                  "air64_v28-apple-macosx26.3.0");
    CHECK(n > 0u, "synth bitcode fits");
    CHECK(lagfx_bitcode_has_magic(in, n),
          "synth bitcode has LLVM wrapper magic");

    uint8_t *out = NULL;
    size_t   out_n = 0u;
    lagfx_status_t st = lagfx_bitcode_retarget_to_spirv(
        in, n, &out, &out_n);
    CHECK(st == LAGFX_OK, "retarget(AIR→SPIR-V) returns LAGFX_OK");
    CHECK(out != NULL, "retarget output buffer non-NULL");
    if (out) {
        CHECK(out_n == n,
              "Tier 1 preserves total length");
        CHECK(buf_contains(out, out_n, "spir64-unknown-vulkan1.3"),
              "output contains spir64-unknown-vulkan1.3");
        CHECK(!buf_contains(out, out_n, "air64_v28-apple-macosx"),
              "output no longer contains AIR triple prefix");
        CHECK(buf_contains(out, out_n, "END"),
              "output preserves trailing marker (no overwrite)");
        free(out);
    }

    /* Tier 2 path: AIR triple shorter than replacement — use
     * just `air64-apple-macosx1` (20 chars vs 24). */
    uint8_t in2[128];
    size_t n2 = make_synth_bitcode(in2, sizeof(in2),
                                   "air64-apple-macosx1");
    out = NULL; out_n = 0u;
    st = lagfx_bitcode_retarget_to_spirv(in2, n2, &out, &out_n);
    CHECK(st == LAGFX_OK, "Tier 2 retarget returns LAGFX_OK");
    if (out) {
        CHECK(out_n >= n2, "Tier 2 output grows or stays equal");
        CHECK(buf_contains(out, out_n, "spir64-unknown-vulkan1.3"),
              "Tier 2 output contains spir64 triple");
        CHECK(buf_contains(out, out_n, "END"),
              "Tier 2 preserves trailing marker");
        free(out);
    }

    /* Error: no AIR pattern. */
    uint8_t clean[32];
    memset(clean, 0, sizeof(clean));
    clean[0] = 0xDE; clean[1] = 0xC0; clean[2] = 0x17; clean[3] = 0x0B;
    memcpy(clean + 16, "hello world", 11);
    out = NULL; out_n = 0;
    st = lagfx_bitcode_retarget_to_spirv(clean, sizeof(clean),
                                          &out, &out_n);
    CHECK(st == LAGFX_ERR_PROTOCOL,
          "no AIR pattern → ERR_PROTOCOL");
    CHECK(out == NULL, "no output on PROTOCOL error");

    /* Error: no wrapper magic. */
    uint8_t junk[32];
    memset(junk, 0xaa, sizeof(junk));
    out = NULL; out_n = 0;
    st = lagfx_bitcode_retarget_to_spirv(junk, sizeof(junk),
                                          &out, &out_n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "no wrapper magic → INVALID_ARG");

    /* Error: NULL inputs. */
    st = lagfx_bitcode_retarget_to_spirv(NULL, 0, &out, &out_n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "NULL input → INVALID_ARG");
    st = lagfx_bitcode_retarget_to_spirv(in, n, NULL, &out_n);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "NULL out_buf → INVALID_ARG");
}

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan air2spirv smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    test_extract_happy();
    test_extract_corners();
    test_retarget();

    fprintf(stdout, "\n=== Summary: %s ===\n",
            g_fail ? "FAILURES" : "ALL GOOD");
    return g_fail ? 1 : 0;
}
