/*
 * libapplegfx-vulkan — multi-render-target (struct-return) fragment output
 * tests/air2spv-multi-render-target-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Guards struct-returning fragments that write multiple render targets
 * (real SkyLight corpus, 2026-05-30). A `fragment struct { float, float2 } f()`
 * (ColorFillYCbCr: plane_y + plane_uv) must emit ONE Location-N Output
 * variable per struct member, each typed to that member, and the RET must
 * OpCompositeExtract member k and OpStore it to output k. Previously the
 * translator emitted a single hardcoded v4float Location-0 output and tried
 * to store the whole struct -> spirv-val "OpStore type mismatch".
 *
 * Asserts: >=2 Output OpVariables, one a scalar float and one a 2-vector,
 * decorated Location 0 and Location 1, and >=2 OpCompositeExtract feeding
 * OpStore.
 *
 * Fixture: ColorFillYCbCr (returns <{ float, <2 x float> }>).
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC              0x07230203u
#define OP_DECORATE            71u
#define OP_TYPE_FLOAT          22u
#define OP_TYPE_VECTOR         23u
#define OP_TYPE_POINTER        32u
#define OP_VARIABLE            59u
#define OP_COMPOSITE_EXTRACT   81u
#define OP_STORE               62u
#define STORAGE_CLASS_OUTPUT   3u
#define DECOR_LOCATION         30u

#define MAXID 4096u

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

static uint8_t  is_float[MAXID];
static uint32_t vec_lanes[MAXID];
static uint32_t ptr_pointee[MAXID];
static uint8_t  ptr_is_output[MAXID];
static int32_t  var_location[MAXID];   /* Output var id -> Location, -1 if none */

static int test_mrt(void) {
    const char *cands[] = {
        "tests/fixtures/colorfillycbcr.air.bc",
        "../tests/fixtures/colorfillycbcr.air.bc",
        SRCDIR "/fixtures/colorfillycbcr.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: colorfillycbcr.air.bc fixture not found\n"); return 1; }

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

    for (uint32_t i = 0; i < MAXID; i++) var_location[i] = -1;

    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nwords = spv_sz / 4u;
    int rc = 0, n_extract = 0, n_store = 0;

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        switch (op) {
            case OP_TYPE_FLOAT:
                if (wc >= 2u && w[i + 1u] < MAXID) is_float[w[i + 1u]] = 1;
                break;
            case OP_TYPE_VECTOR:
                if (wc >= 4u && w[i + 1u] < MAXID) vec_lanes[w[i + 1u]] = w[i + 3u];
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && w[i + 1u] < MAXID) {
                    ptr_pointee[w[i + 1u]] = w[i + 3u];
                    ptr_is_output[w[i + 1u]] = (w[i + 2u] == STORAGE_CLASS_OUTPUT);
                }
                break;
            case OP_DECORATE:
                if (wc >= 4u && w[i + 2u] == DECOR_LOCATION && w[i + 1u] < MAXID)
                    var_location[w[i + 1u]] = (int32_t)w[i + 3u];
                break;
            case OP_COMPOSITE_EXTRACT: n_extract++; break;
            case OP_STORE:             n_store++;   break;
            default: break;
        }
        i += wc;
    }

    /* Walk OpVariables again for the Output ones (need full type table first). */
    int n_out_scalar_float = 0, n_out_vec2 = 0, n_out = 0;
    int saw_loc0 = 0, saw_loc1 = 0;
    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_VARIABLE && wc >= 4u && w[i + 3u] == STORAGE_CLASS_OUTPUT) {
            uint32_t var_id = w[i + 2u];
            uint32_t pt = w[i + 1u];
            uint32_t pointee = (pt < MAXID) ? ptr_pointee[pt] : 0;
            n_out++;
            if (pointee < MAXID && is_float[pointee]) n_out_scalar_float++;
            if (pointee < MAXID && vec_lanes[pointee] == 2u) n_out_vec2++;
            if (var_id < MAXID && var_location[var_id] == 0) saw_loc0 = 1;
            if (var_id < MAXID && var_location[var_id] == 1) saw_loc1 = 1;
        }
        i += wc;
    }

    if (n_out < 2) {
        printf("FAIL: %d Output variable(s); expected 2 (multi-target struct "
               "return not split into per-member outputs)\n", n_out);
        rc = 1;
    }
    if (n_out_scalar_float < 1) {
        printf("FAIL: no scalar-float Output (plane_y member 0 mistyped)\n");
        rc = 1;
    }
    if (n_out_vec2 < 1) {
        printf("FAIL: no float2 Output (plane_uv member 1 mistyped)\n");
        rc = 1;
    }
    if (!saw_loc0 || !saw_loc1) {
        printf("FAIL: outputs not decorated Location 0 AND 1 (loc0=%d loc1=%d)\n",
               saw_loc0, saw_loc1);
        rc = 1;
    }
    if (n_extract < 2 || n_store < 2) {
        printf("FAIL: expected >=2 CompositeExtract (%d) and >=2 Store (%d) "
               "for per-member writes\n", n_extract, n_store);
        rc = 1;
    }

    if (rc == 0)
        printf("PASS: %d Output targets (1 float @loc0, 1 float2 @loc1), "
               "%d extract / %d store\n", n_out, n_extract, n_store);

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_mrt();
}
