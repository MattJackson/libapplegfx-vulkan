/*
 * libapplegfx-vulkan — air.sample_texture_2d translation regression
 * tests/air2spv-texture-sample-translate-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards the texture-sampling lowering (coverage audit 2026-05-30). Apple
 * lowers `tex.sample(s, uv)` to a CALL of
 *   air.sample_texture_2d.v4f32(tex_ptr, samp_ptr, uv, <opts...>)
 * returning { vec4 colour, i8 status }. The texture (ptr addrspace 1) and
 * sampler (ptr addrspace 2) args become UniformConstant OpVariables; the
 * call lowers to OpLoad image + OpLoad sampler + OpSampledImage +
 * OpImageSampleImplicitLod + OpCompositeConstruct of the result struct.
 * Previously the whole call fell through to a typed OpUndef placeholder
 * (the texture read returned undefined -> black/garbage).
 *
 * Asserts the translated tex_fragment contains an OpImageSampleImplicitLod
 * (op 87) and an OpTypeImage (op 25) — neither present under the old undef
 * path. Fixture: tex_fragment (texture2d + sampler) from xcrun metal.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC                       0x07230203u
#define OP_TYPE_IMAGE                   25u
#define OP_IMAGE_SAMPLE_IMPLICIT_LOD    87u

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

static void count(const uint8_t *blob, size_t sz, int *n_image_t, int *n_sample) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_image_t = 0; *n_sample = 0;
    if (nwords < 5u) return;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_TYPE_IMAGE) (*n_image_t)++;
        else if (op == OP_IMAGE_SAMPLE_IMPLICIT_LOD) (*n_sample)++;
        i += wc;
    }
}

static int test_texture_sample(void) {
    const char *cands[] = {
        "tests/fixtures/tex_fragment.air.bc",
        "../tests/fixtures/tex_fragment.air.bc",
        SRCDIR "/fixtures/tex_fragment.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: tex_fragment.air.bc fixture not found\n"); return 1; }

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

    int rc = 0, n_img = 0, n_smp = 0;
    count(spv, spv_sz, &n_img, &n_smp);
    printf("OpTypeImage count=%d, OpImageSampleImplicitLod count=%d (%zu spv bytes)\n",
           n_img, n_smp, spv_sz);

    if (n_img < 1) {
        printf("FAIL: no OpTypeImage — the texture arg was not declared as an "
               "image global.\n");
        rc = 1;
    }
    if (n_smp < 1) {
        printf("FAIL: no OpImageSampleImplicitLod — air.sample_texture_2d fell "
               "through to the OpUndef placeholder (texture read returns "
               "undefined).\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: air.sample_texture_2d lowers to a real image sample\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_texture_sample();
}
