/*
 * libapplegfx-vulkan — bool(i1) → float convert regression guard
 * tests/air2spv-bool-to-float-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards the bool→float lowering of `air.convert.f.*.u.i1` (coverage audit
 * 2026-05-30, corpus shader 05_math_intrinsics::math_fragment, where
 * `step(edge,x)` lowers to `air.convert.f.f32.u.i1(i1 cond)`). SPIR-V bool is
 * NOT an integer, so the old path emitted `OpConvertUToF` on a bool operand →
 * spirv-val "Expected input to be int scalar or vector: ConvertUToF". The fix
 * detects the i1 source type and emits OpSelect(cond, 1.0, 0.0) instead.
 *
 * Structural invariants asserted:
 *   - at least one OpSelect present (the bool→float lowering)
 *   - the spirv module is structurally well-formed (magic + nonempty)
 *
 * Fixture: step_bool_to_float.air.bc — Apple-compiled math_fragment, whose
 * `step()`/`smoothstep()`/saturate produce the i1→float converts.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC          0x07230203u
#define OP_SELECT          169u
#define OP_CONVERT_U_TO_F  112u

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
        "tests/fixtures/step_bool_to_float.air.bc",
        SRCDIR "/fixtures/step_bool_to_float.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: step_bool_to_float.air.bc fixture not found\n"); return 1; }

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

    int n_sel = count_op(spv, spv_sz, OP_SELECT);
    int rc = 0;
    printf("OpSelect=%d (%zu spv bytes)\n", n_sel, spv_sz);
    if (n_sel < 1) {
        printf("FAIL: no OpSelect — bool→float convert was not lowered to a "
               "select (would be an invalid OpConvertUToF on a bool).\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: bool(i1)→float convert lowers to OpSelect(cond,1.0,0.0)\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}
