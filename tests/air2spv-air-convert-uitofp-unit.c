/*
 * libapplegfx-vulkan — air.convert.* numeric-conversion regression
 * tests/air2spv-air-convert-uitofp-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression guard for the Stage-80 black-screen bug.
 *
 * Apple's metal compiler lowers a Metal numeric cast such as
 * `float2(uint x, uint y)` NOT to an LLVM `uitofp`/CAST instruction but
 * to a CALL of an `air.convert.f.f32.u.i32(i32) -> float` intrinsic.
 * The CALL dispatch only recognised air.fast and air.precise GLSL.std.450
 * intrinsics, so air.convert fell through to the "unrecognised non-void
 * call -> typed OpUndef placeholder" path. The dependent insertelement then
 * inserted OpUndef into the position vector, producing
 * gl_Position = (undef, undef, 0, 1). Lenient lavapipe tolerated the undef
 * and rendered; the strict container llvmpipe 19.1.4 turned the undefined
 * positions into unrasterisable garbage -> BLACK screen (the baked triangle
 * rendered red on the same driver, isolating the fault to the translator).
 *
 * Fixture: demo_vertex (`float2 p = float2((vid<<1)&2, vid&2);
 * o.position = float4(p*2-1, 0, 1);`) compiled with `xcrun metal`. The
 * translated vertex MUST emit at least one OpConvertUToF (op 112) and that
 * convert's result MUST feed a downstream OpVectorInsertDynamic value
 * operand (NOT OpUndef).
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC            0x07230203u
#define SPV_OP_UNDEF         1u
#define SPV_OP_CONVERT_U_TO_F 112u
#define SPV_OP_VINSERT_DYN   78u   /* OpVectorInsertDynamic (§3.32.12) */

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* Walk the SPIR-V stream. Collect OpUndef result ids and OpConvertUToF
 * result ids; then check whether any OpVectorInsertDynamic inserts a
 * convert result (good) and whether any inserts an undef (bad).
 *
 * OpVectorInsertDynamic layout: [hdr][result_type][result][vector][component][index]
 * — the inserted scalar is the 'component' operand (word i+4). */
static int analyse(const uint8_t *blob, size_t sz,
                   int *n_convert, int *insert_uses_convert,
                   int *insert_uses_undef) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_convert = 0; *insert_uses_convert = 0; *insert_uses_undef = 0;
    if (nwords < 5u) return 0;

    uint32_t undefs[64]; size_t n_undef = 0;
    uint32_t convs[64];  size_t n_conv = 0;

    size_t i = 5u;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;
        if (op == SPV_OP_UNDEF && wc >= 3u) {
            if (n_undef < 64) undefs[n_undef++] = w[i + 2u];
        } else if (op == SPV_OP_CONVERT_U_TO_F && wc >= 3u) {
            if (n_conv < 64) convs[n_conv++] = w[i + 2u];
            (*n_convert)++;
        } else if (op == SPV_OP_VINSERT_DYN && wc >= 6u) {
            uint32_t component = w[i + 4u];   /* the inserted scalar */
            for (size_t k = 0; k < n_conv; k++)
                if (component == convs[k]) { *insert_uses_convert = 1; break; }
            for (size_t k = 0; k < n_undef; k++)
                if (component == undefs[k]) { *insert_uses_undef = 1; break; }
        }
        i += wc;
    }
    return 1;
}

static int test_air_convert_uitofp(void) {
    const char *cands[] = {
        "tests/fixtures/demo_vertex_uitofp.air.bc",
        "../tests/fixtures/demo_vertex_uitofp.air.bc",
        SRCDIR "/fixtures/demo_vertex_uitofp.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: demo_vertex_uitofp.air.bc fixture not found\n"); return 1; }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open\n"); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL: translate\n"); lagfx_air_module_free(m); free(air); return 1;
    }
    uint32_t magic; memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad magic 0x%08x\n", magic);
        free(spv); lagfx_air_module_free(m); free(air); return 1;
    }

    int rc = 0, n_conv = 0, uses_conv = 0, uses_undef = 0;
    analyse(spv, spv_sz, &n_conv, &uses_conv, &uses_undef);
    printf("OpConvertUToF count=%d, insert-uses-convert=%d, insert-uses-undef=%d "
           "(%zu spv bytes)\n", n_conv, uses_conv, uses_undef, spv_sz);

    if (n_conv < 1) {
        printf("FAIL: zero OpConvertUToF — air.convert.f.f32.u.i32 was NOT "
               "lowered (the uint->float conversion was dropped). gl_Position "
               "would be built from undefined scalars → black screen.\n");
        rc = 1;
    }
    if (!uses_conv) {
        printf("FAIL: no OpVectorInsertDynamic consumes an OpConvertUToF result "
               "— the converted coordinate never reaches the position vector.\n");
        rc = 1;
    }
    if (uses_undef) {
        printf("FAIL: an OpVectorInsertDynamic still inserts OpUndef into the "
               "position vector — the Stage-80 black-screen bug regressed.\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: air.convert.f.f32.u.i32 lowers to OpConvertUToF and feeds "
               "the position vector insertelement\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_air_convert_uitofp();
}
