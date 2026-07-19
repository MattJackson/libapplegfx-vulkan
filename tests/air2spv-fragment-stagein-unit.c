/*
 * libapplegfx-vulkan — fragment stage_in input + extractelement regression
 * tests/air2spv-fragment-stagein-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Guards two coupled fixes (coverage audit 2026-05-30) for fragment shaders
 * that read interpolated [[stage_in]] inputs:
 *
 *   - Fragment stage_in inputs: each float/vector fragment arg now gets a
 *     Location-decorated Input OpVariable + an OpLoad binding (mirrors the
 *     vertex ATTR path). Without it, `in.uv` resolved to a mistyped OpUndef.
 *   - EXTRACTELT element type: the handler treated element type INDEX 0 as
 *     "missing" (a truthiness bug — float is commonly type 0), dropping the
 *     `in.uv.x` extractelement so the comparison read an undef-typed bool
 *     (spirv-val: "FOrdGreaterThan operands must be float").
 *
 * Fixture: branch_fragment (`in.uv.x > 0.5 ? ... : ...`, with `in.uv.y`
 * ternary) compiled with xcrun metal. Asserts the translated module
 * contains >=1 Input OpVariable (a stage_in input) and >=1
 * OpVectorExtractDynamic (the extractelement that previously dropped).
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC               0x07230203u
#define OP_VARIABLE             59u
#define OP_VECTOR_EXTRACT_DYN   77u
#define STORAGE_CLASS_INPUT     1u

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

static void count(const uint8_t *blob, size_t sz, int *n_input_var, int *n_extract) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_input_var = 0; *n_extract = 0;
    if (nwords < 5u) return;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        /* OpVariable: [hdr][result-type][result-id][storage-class] */
        if (op == OP_VARIABLE && wc >= 4u && w[i + 3u] == STORAGE_CLASS_INPUT)
            (*n_input_var)++;
        else if (op == OP_VECTOR_EXTRACT_DYN)
            (*n_extract)++;
        i += wc;
    }
}

static int test_fragment_stagein(void) {
    const char *cands[] = {
        "tests/fixtures/branch_fragment.air.bc",
        "../tests/fixtures/branch_fragment.air.bc",
        SRCDIR "/fixtures/branch_fragment.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: branch_fragment.air.bc fixture not found\n"); return 1; }

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

    int rc = 0, n_in = 0, n_ex = 0;
    count(spv, spv_sz, &n_in, &n_ex);
    printf("Input OpVariable count=%d, OpVectorExtractDynamic count=%d (%zu spv bytes)\n",
           n_in, n_ex, spv_sz);

    if (n_in < 1) {
        printf("FAIL: no Input OpVariable — fragment stage_in inputs not "
               "declared (in.uv would resolve to OpUndef).\n");
        rc = 1;
    }
    if (n_ex < 1) {
        printf("FAIL: no OpVectorExtractDynamic — the in.uv.x extractelement "
               "was dropped (element-type index-0 truthiness bug).\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: fragment stage_in inputs declared + extractelement "
               "lowered\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_fragment_stagein();
}
