/*
 * libapplegfx-vulkan — numeric bitcast (`as_type`) regression guard
 * tests/air2spv-bitcast-as-type-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards numeric bitcasts (coverage audit 2026-05-30, corpus shader
 * 04_int_bitwise::bitcast_fragment, `as_type<uint>(float)` /
 * `bitcast <2 x float> to <2 x i32>`). The CAST_BITCAST handler previously
 * ALWAYS aliased the result to the source SPIR-V id without retyping, so a
 * float-vector flowed unchanged into an int consumer
 * (`extractelement <2 x i32>`) → spirv-val "Expected Vector component type to
 * be equal to Result Type" on the resulting OpVectorExtractDynamic. The fix
 * emits a real OpBitcast when the destination is a numeric scalar/vector
 * (int/float/half); pointer bitcasts still alias.
 *
 * Structural invariant asserted: at least one OpBitcast present (a genuine
 * numeric reinterpret was emitted, not silently aliased).
 *
 * Fixture: bitcast_as_type.air.bc — Apple-compiled bitcast_fragment.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC     0x07230203u
#define OP_BITCAST    124u

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

static int count_op(const uint8_t *blob, size_t sz, uint16_t want) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    int n = 0;
    if (nwords < 5u) return 0;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == want) n++;
        i += wc;
    }
    return n;
}

int main(void) {
    const char *cands[] = {
        "tests/fixtures/bitcast_as_type.air.bc",
        SRCDIR "/fixtures/bitcast_as_type.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: bitcast_as_type.air.bc fixture not found\n"); return 1; }

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

    int n_bc = count_op(spv, spv_sz, OP_BITCAST);
    int rc = 0;
    printf("OpBitcast=%d (%zu spv bytes)\n", n_bc, spv_sz);
    if (n_bc < 1) {
        printf("FAIL: no OpBitcast — a numeric bitcast was silently aliased "
               "(float vector would flow into an int consumer untyped).\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: numeric bitcast lowers to a real OpBitcast\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}
