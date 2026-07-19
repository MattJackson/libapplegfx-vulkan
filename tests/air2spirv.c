/*
 * libapplegfx-vulkan — air2spirv extraction + retarget smoke (Phase 3.C.2)
 * tests/air2spirv.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exercises src/air2spirv/ with BOTH synthesised fixtures and a
 * real-bytes copy of Apple's `default.metallib` (shipped at
 * tests/fixtures/default.metallib, sourced from
 * paravirt-re/metallib/default.metallib). The real-bytes case is the
 * primary validation gate — a synthesised fixture can accidentally
 * agree with our parser even when the parser disagrees with Apple's
 * actual container layout (e.g. u16 vs u32 tag lengths, entry
 * size-prefix framing, stage enum values). The real-bytes test
 * catches those cases.
 *
 * Cases:
 *
 *   (A) Metallib extractor, synthesised:
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
 *   (C) Bitcode retarget, synthesised:
 *       - Synthesise an LLVM Bitcode wrapper header + embedded
 *         `air64-apple-macosx26.3.0` triple.
 *       - Call retarget_to_spirv; assert the output contains
 *         `spir64-unknown-vulkan1.3` and no longer contains
 *         the AIR triple.
 *       - NULL inputs → ERR_INVALID_ARG.
 *       - No AIR pattern in bitcode → ERR_PROTOCOL.
 *       - Unknown format (no bitcode magic) → ERR_INVALID_ARG.
 *
 *   (D) Real-bytes metallib extractor + retarget (only when
 *       LAGFX_HAVE_REAL_METALLIB is defined at compile time, i.e.
 *       tests/fixtures/default.metallib exists; see
 *       tests/meson.build):
 *       - Read tests/fixtures/default.metallib (24,500 bytes,
 *         MTLB v1.2.9, 5 shaders per paravirt-re/metallib-analysis.md).
 *       - Assert MTLB magic recognised, function count == 5.
 *       - Assert the 5 function names match the known set
 *         (BlitInRGB_2P_XR10_A8, BlitOutRGB_2P_XR10_A8,
 *         displayPresentVertex, displayPresentFragment,
 *         displayPresentFragmentWithGammaTableAndColorMatrix).
 *       - Assert every extracted bitcode payload begins with the
 *         LLVM Bitcode wrapper magic 0x0B17C0DE (little-endian
 *         DE C0 17 0B).
 *       - Assert every extracted function name is ASCII + non-empty.
 *       - Assert every extracted bitcode contains an `air64*-apple-macosx`
 *         triple (before retarget).
 *       - Retarget each bitcode to SPIR-V, assert the output contains
 *         `spir64-unknown-vulkan1.3`.
 *
 * Pure in-process test, no Vulkan. Reads one small fixture from
 * disk on the real-bytes path. Runs on every host.
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

/* --- Little-endian write helpers (u16) ------------------------- */

static void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

/* --- Fixture: synth MTLB buffer ------------------------------- */

/* Layout (offsets) — matches the Apple MTLB v1.2.9 format as
 * observed in tests/fixtures/default.metallib (see
 * paravirt-re/metallib-analysis.md §"Container format"):
 *
 *   0x00 'M' 'T' 'L' 'B'
 *   0x04 .. 0x17  padding / version (ignored at parse time)
 *   0x18 u64 FET_OFFSET
 *   0x20 .. 0x47  padding (other header u64s)
 *   0x48 u64 BC_BASE (absolute file offset of bitcode region)
 *   0x50 bitcode payload (BC_BASE = 0x50; 16 bytes: wrapper magic
 *        + 12 zeros)
 *   0x60 FET:
 *        u32 entry_count (=1)
 *        for each entry:
 *          u32 entry_size (includes the size field itself)
 *          NAME u16 len | bytes (NUL-terminated)
 *          TYPE u16 len=1 | u8 stage
 *          OFFT u16 len=24 | 3x u64 (last is bitcode offset rel. BC_BASE)
 *          MDSZ u16 len=8 | u64 bitcode_size
 *          ENDT (no length field)
 *
 * Stage enum values: 0=vertex, 1=fragment, 2=kernel (per the real
 * corpus). */
#define MTLB_BUF_LEN 256u

static size_t make_synth_mtlb(uint8_t *buf, size_t cap,
                              const char *name,
                              uint8_t stage,
                              size_t *out_bc_offset,
                              size_t *out_bc_len) {
    memset(buf, 0, cap);
    buf[0] = 'M'; buf[1] = 'T'; buf[2] = 'L'; buf[3] = 'B';

    const uint64_t fet_offset    = 0x60ull;
    const uint64_t bc_base       = 0x50ull;
    const uint64_t bc_offset_rel = 0x00ull;
    const uint64_t bc_len        = 0x10ull;

    wr_u64_le(buf + 0x18, fet_offset);
    wr_u64_le(buf + 0x48, bc_base);

    /* Bitcode payload: LLVM wrapper magic + pad. */
    static const uint8_t bc_magic[4] = { 0xDE, 0xC0, 0x17, 0x0B };
    memcpy(buf + (size_t)bc_base + (size_t)bc_offset_rel, bc_magic, 4u);
    /* Rest stays zero — we only test extraction, not bitcode
     * parsing. */

    /* FET: u32 entry_count = 1. */
    size_t p = (size_t)fet_offset;
    wr_u32_le(buf + p, 1u);
    p += 4u;

    /* Remember where the entry begins — we patch its size at the
     * end once we know how many bytes the body consumed. */
    size_t entry_start = p;
    wr_u32_le(buf + p, 0u);  /* placeholder, fixed up below */
    p += 4u;

    /* NAME tag. Apple includes a trailing NUL inside the length. */
    const size_t name_len = strlen(name) + 1u;
    memcpy(buf + p, "NAME", 4u);
    wr_u16_le(buf + p + 4u, (uint16_t)name_len);
    memcpy(buf + p + 6u, name, name_len);
    p += 6u + name_len;

    /* TYPE — 1-byte stage. */
    memcpy(buf + p, "TYPE", 4u);
    wr_u16_le(buf + p + 4u, 1u);
    buf[p + 6u] = stage;
    p += 7u;

    /* OFFT — 24 bytes (3 u64s). The third is the bitcode offset
     * relative to BC_BASE. The first two we leave zero. */
    memcpy(buf + p, "OFFT", 4u);
    wr_u16_le(buf + p + 4u, 24u);
    wr_u64_le(buf + p +  6u, 0ull);
    wr_u64_le(buf + p + 14u, 0ull);
    wr_u64_le(buf + p + 22u, bc_offset_rel);
    p += 6u + 24u;

    /* MDSZ — 8 bytes (u64 size). */
    memcpy(buf + p, "MDSZ", 4u);
    wr_u16_le(buf + p + 4u, 8u);
    wr_u64_le(buf + p + 6u, bc_len);
    p += 6u + 8u;

    /* ENDT — 4 bytes (no length field). */
    memcpy(buf + p, "ENDT", 4u);
    p += 4u;

    /* Fix up the entry_size field (includes the size field itself). */
    wr_u32_le(buf + entry_start, (uint32_t)(p - entry_start));

    if (out_bc_offset) *out_bc_offset = (size_t)(bc_base + bc_offset_rel);
    if (out_bc_len)    *out_bc_len    = (size_t)bc_len;
    return p;
}

/* --- Test (A): extraction happy path -------------------------- */

static void test_extract_happy(void) {
    uint8_t buf[MTLB_BUF_LEN];
    size_t bc_off = 0, bc_len_x = 0;
    /* Stage 0 = VERTEX in Apple's enum (see
     * src/air2spirv/metallib_extract.c). */
    size_t used = make_synth_mtlb(buf, sizeof(buf), "blit",
                                   0u /* VERTEX */,
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

/* --- Test (D): real-bytes metallib ---------------------------- */

#ifdef LAGFX_HAVE_REAL_METALLIB

/* Known-good function name set for paravirt-re/metallib/default.metallib
 * (MTLB v1.2.9, 24,500 bytes). See metallib-analysis.md §"Shader
 * inventory". */
static const char *kExpectedRealNames[] = {
    "BlitInRGB_2P_XR10_A8",
    "BlitOutRGB_2P_XR10_A8",
    "displayPresentVertex",
    "displayPresentFragmentWithGammaTableAndColorMatrix",
    "displayPresentFragment",
};
#define K_EXPECTED_REAL_COUNT \
    ((size_t)(sizeof(kExpectedRealNames) / sizeof(kExpectedRealNames[0])))

/* Return 1 iff `name` matches one of the expected real names. Order
 * of the metallib entries is stable (see metallib-analysis.md hex
 * dump) but we match via set-membership for resilience. */
static int is_expected_real_name(const char *name) {
    for (size_t i = 0; i < K_EXPECTED_REAL_COUNT; ++i) {
        if (strcmp(name, kExpectedRealNames[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Read the entire file at `path` into a malloc'd buffer. Returns
 * NULL on error; *out_len set to 0 in that case. Caller frees. */
static uint8_t *slurp_file(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "slurp_file: cannot open '%s'\n", path);
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

static void test_real_metallib(void) {
    const char *dir = LAGFX_TEST_FIXTURE_DIR;
    char path[1024];
    int r = snprintf(path, sizeof(path), "%s/default.metallib", dir);
    CHECK(r > 0 && (size_t)r < sizeof(path),
          "real-metallib fixture path fits");

    size_t buf_len = 0;
    uint8_t *buf = slurp_file(path, &buf_len);
    CHECK(buf != NULL, "real-metallib fixture loads from disk");
    if (!buf) {
        return;
    }
    fprintf(stdout, "real-metallib: loaded %zu bytes from %s\n",
            buf_len, path);

    /* The Phase 0 corpus is exactly 24,500 bytes per
     * metallib-analysis.md. Allow a little wiggle (±256) in case a
     * future corpus refresh nudges the size but the structural
     * assertions below still apply. */
    CHECK(buf_len > 24000u && buf_len < 25000u,
          "real-metallib size in expected range (~24,500 bytes)");

    CHECK(lagfx_metallib_has_magic(buf, buf_len),
          "real-metallib recognised via MTLB magic");

    /* Two-pass: first learn the count, then extract all entries. */
    size_t n = 0;
    lagfx_status_t st = lagfx_metallib_extract_functions(
        buf, buf_len, NULL, 0, &n);
    CHECK(st == LAGFX_OK,
          "real-metallib probe (capacity=0) returns LAGFX_OK");
    CHECK(n == K_EXPECTED_REAL_COUNT,
          "real-metallib function count == 5");

    /* Full extract with capacity matched to what we saw. */
    lagfx_metallib_function_t fns[16];
    memset(fns, 0, sizeof(fns));
    size_t n2 = 0;
    st = lagfx_metallib_extract_functions(
        buf, buf_len, fns,
        sizeof(fns) / sizeof(fns[0]),
        &n2);
    CHECK(st == LAGFX_OK,
          "real-metallib full extract returns LAGFX_OK");
    CHECK(n2 == K_EXPECTED_REAL_COUNT,
          "real-metallib full extract count == 5");

    /* Per-entry assertions. */
    for (size_t i = 0; i < n2 && i < K_EXPECTED_REAL_COUNT; ++i) {
        char label[192];

        /* Name is non-empty + ASCII. */
        int nlen = (int)strlen(fns[i].name);
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] name non-empty", i);
        CHECK(nlen > 0, label);

        int ascii_ok = 1;
        for (int k = 0; k < nlen; ++k) {
            uint8_t c = (uint8_t)fns[i].name[k];
            if (c < 0x20 || c > 0x7e) { ascii_ok = 0; break; }
        }
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] name ASCII (got '%s')",
                 i, fns[i].name);
        CHECK(ascii_ok, label);

        /* Name is in the expected set. */
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] name '%s' in expected set",
                 i, fns[i].name);
        CHECK(is_expected_real_name(fns[i].name), label);

        /* Stage is not UNKNOWN — the real corpus has vertex,
         * fragment, and kernel shaders. A parser that does not
         * recognise Apple's real stage enum would leave stage as
         * UNKNOWN, so this is a meaningful assertion. */
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] stage recognised (raw=%u)",
                 i, (unsigned)fns[i].stage_raw);
        CHECK(fns[i].stage != LAGFX_METALLIB_STAGE_UNKNOWN, label);

        /* Bitcode payload resolved. */
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] bitcode pointer resolved",
                 i);
        CHECK(fns[i].bitcode != NULL && fns[i].bitcode_len > 0, label);
        if (!fns[i].bitcode || fns[i].bitcode_len == 0) {
            continue;
        }

        /* Bitcode starts with LLVM Bitcode wrapper magic. */
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] bitcode has LLVM wrapper magic",
                 i);
        CHECK(fns[i].bitcode[0] == 0xDE
              && fns[i].bitcode[1] == 0xC0
              && fns[i].bitcode[2] == 0x17
              && fns[i].bitcode[3] == 0x0B,
              label);

        /* Bitcode contains an `air64*-apple-macosx` triple. */
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] bitcode contains AIR triple",
                 i);
        CHECK(buf_contains(fns[i].bitcode, fns[i].bitcode_len,
                           "air64")
              && buf_contains(fns[i].bitcode, fns[i].bitcode_len,
                              "apple-macosx"),
              label);

        /* Retarget to SPIR-V — exercises bitcode_retarget.c
         * against REAL AIR bitcode bytes. */
        uint8_t *rt_out = NULL;
        size_t   rt_len = 0;
        lagfx_status_t rs = lagfx_bitcode_retarget_to_spirv(
            fns[i].bitcode, fns[i].bitcode_len, &rt_out, &rt_len);
        snprintf(label, sizeof(label),
                 "real-metallib fn[%zu] retarget returns LAGFX_OK",
                 i);
        CHECK(rs == LAGFX_OK, label);

        if (rs == LAGFX_OK && rt_out != NULL) {
            snprintf(label, sizeof(label),
                     "real-metallib fn[%zu] retarget output "
                     "contains spir64 triple", i);
            CHECK(buf_contains(rt_out, rt_len,
                               "spir64-unknown-vulkan1.3"),
                  label);
        }
        free(rt_out);
    }

    free(buf);
}

#else  /* !LAGFX_HAVE_REAL_METALLIB */

static void test_real_metallib(void) {
    fprintf(stdout, "real-metallib: skipped (fixture not present at "
                    "configure time)\n");
}

#endif

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan air2spirv smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    test_extract_happy();
    test_extract_corners();
    test_retarget();
    test_real_metallib();

    fprintf(stdout, "\n=== Summary: %s ===\n",
            g_fail ? "FAILURES" : "ALL GOOD");
    return g_fail ? 1 : 0;
}
