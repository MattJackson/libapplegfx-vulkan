/*
 * libapplegfx-vulkan — CALL callee value-id base + fragment-output typing
 * tests/air2spv-call-globalvar-base-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards two coupled fixes (real SkyLight corpus, 2026-05-30):
 *
 *  1. CALL callee resolution must map the absolute value-id to a function
 *     index via `callee_id - n_globalvars` (functions enumerate AFTER global
 *     variables). After GLOBALVAR records started being counted, every
 *     shader has >=1 globalvar (@llvm.global_ctors); a constexpr-sampler
 *     shader has 2. The old `fns[callee_id]` lookup ran past the table and
 *     DROPPED air.sample_texture_2d -> the result undef inherited the
 *     global_ctors `[0 x {i32,ptr,ptr}]` type -> an invalid zero-length
 *     OpTypeArray downstream. Symptom: NO OpImageSample in the output.
 *
 *  2. The fragment Location-0 colour output must be typed to the function's
 *     actual return type. SimpleTextureFragmentUV returns `float2`; the old
 *     code hardcoded a v4float output, so the `OpStore %out %v2` mismatched.
 *
 * Fixture: SimpleTextureFragmentUV (samples a texture via a constexpr
 * sampler, returns `.xy` -> float2). Asserts the sample call lowered (an
 * OpImageSample* exists), no zero-length OpTypeArray, and the colour output
 * variable is a 2-component vector.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC                  0x07230203u
#define OP_TYPE_VECTOR             23u
#define OP_TYPE_ARRAY              28u
#define OP_TYPE_POINTER            32u
#define OP_VARIABLE                59u
#define OP_CONSTANT                43u
#define OP_IMAGE_SAMPLE_IMPL_LOD   87u
#define OP_IMAGE_SAMPLE_EXPL_LOD   88u
#define STORAGE_CLASS_OUTPUT       3u

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

static uint32_t vec_lanes[MAXID];   /* OpTypeVector id -> lane count, else 0 */
static uint32_t ptr_pointee[MAXID]; /* OpTypePointer id -> pointee type id */
static uint8_t  ptr_is_output[MAXID];
static uint32_t const_val[MAXID];   /* OpConstant id -> literal (low word) */
static uint8_t  const_seen[MAXID];

static int test_call_base(void) {
    const char *cands[] = {
        "tests/fixtures/simpletexfraguv.air.bc",
        "../tests/fixtures/simpletexfraguv.air.bc",
        SRCDIR "/fixtures/simpletexfraguv.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: simpletexfraguv.air.bc fixture not found\n"); return 1; }

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

    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nwords = spv_sz / 4u;
    int rc = 0, n_sample = 0, n_zero_len_array = 0;
    uint32_t out_var_ptr_ty = 0;

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        switch (op) {
            case OP_TYPE_VECTOR:
                if (wc >= 4u && w[i + 1u] < MAXID) vec_lanes[w[i + 1u]] = w[i + 3u];
                break;
            case OP_CONSTANT:
                if (wc >= 4u && w[i + 2u] < MAXID) {
                    const_seen[w[i + 2u]] = 1; const_val[w[i + 2u]] = w[i + 3u];
                }
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && w[i + 1u] < MAXID) {
                    ptr_pointee[w[i + 1u]] = w[i + 3u];
                    ptr_is_output[w[i + 1u]] = (w[i + 2u] == STORAGE_CLASS_OUTPUT);
                }
                break;
            case OP_TYPE_ARRAY:
                /* length is an OpConstant id at w[i+3]; 0 length is illegal. */
                if (wc >= 4u && w[i + 3u] < MAXID && const_seen[w[i + 3u]] &&
                    const_val[w[i + 3u]] == 0u)
                    n_zero_len_array++;
                break;
            case OP_VARIABLE:
                if (wc >= 4u && w[i + 3u] == STORAGE_CLASS_OUTPUT &&
                    w[i + 1u] < MAXID)
                    out_var_ptr_ty = w[i + 1u];
                break;
            case OP_IMAGE_SAMPLE_IMPL_LOD:
            case OP_IMAGE_SAMPLE_EXPL_LOD:
                n_sample++;
                break;
            default: break;
        }
        i += wc;
    }

    if (n_sample < 1) {
        printf("FAIL: no OpImageSample — air.sample_texture_2d was DROPPED "
               "(callee value-id not offset by n_globalvars).\n");
        rc = 1;
    }
    if (n_zero_len_array > 0) {
        printf("FAIL: %d zero-length OpTypeArray(s) — the dropped-call undef "
               "took the global_ctors array type.\n", n_zero_len_array);
        rc = 1;
    }
    /* Output var should be a pointer to a 2-component vector (float2 return). */
    uint32_t out_pointee = (out_var_ptr_ty < MAXID) ? ptr_pointee[out_var_ptr_ty] : 0;
    uint32_t out_lanes = (out_pointee < MAXID) ? vec_lanes[out_pointee] : 0;
    if (out_lanes != 2u) {
        printf("FAIL: fragment colour output is %u-lane, expected 2 (float2 "
               "return type not honoured).\n", out_lanes);
        rc = 1;
    }

    if (rc == 0)
        printf("PASS: sample call lowered (%d OpImageSample), no zero-length "
               "array, float2 colour output\n", n_sample);

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_call_base();
}
