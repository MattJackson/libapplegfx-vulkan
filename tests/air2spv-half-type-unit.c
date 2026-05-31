/*
 * libapplegfx-vulkan — `half` (IEEE-754 binary16) type regression guard
 * tests/air2spv-half-type-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards `half`/`half2`/`half4` support (coverage audit 2026-05-30, corpus
 * shader 05_math_intrinsics::half_fragment). Before the fix `emit_air_type`
 * had no LAGFX_AIR_TYPE_HALF case, so `half` fell through to `default` → uint32:
 *   - `<2 x half>` came out `v2uint` and the f→f convert that targets it
 *     (`air.convert.f.v2f16.f.v2f32` → OpFConvert) had a non-float result type;
 *   - a `<4 x half>` aggregate constant got float (not half) constituents;
 *   - `fmul half` was classified integer → OpIMul %half.
 * After the fix `half` lowers to OpTypeFloat 16 (with the Float16 capability),
 * half scalar constants emit as %half OpConstants, and half arithmetic uses
 * the float ops (OpFMul, not OpIMul).
 *
 * Structural invariants asserted (spirv-val isn't linkable here):
 *   - at least one OpTypeFloat with width 16  (the half type exists)
 *   - OpCapability Float16 present
 *   - ZERO OpIMul (a half fmul mis-lowered to integer multiply would appear)
 *
 * Fixture: half_mixed_math.air.bc — Apple-compiled half_fragment.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC            0x07230203u
#define OP_CAPABILITY        17u
#define OP_TYPE_FLOAT        22u
#define OP_IMUL              132u
#define CAP_FLOAT16          9u

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

static void scan(const uint8_t *blob, size_t sz,
                 int *n_f16, int *n_cap_f16, int *n_imul) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_f16 = 0; *n_cap_f16 = 0; *n_imul = 0;
    if (nwords < 5u) return;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_TYPE_FLOAT && wc >= 3u && w[i + 2u] == 16u) (*n_f16)++;
        else if (op == OP_CAPABILITY && wc >= 2u && w[i + 1u] == CAP_FLOAT16) (*n_cap_f16)++;
        else if (op == OP_IMUL) (*n_imul)++;
        i += wc;
    }
}

int main(void) {
    const char *cands[] = {
        "tests/fixtures/half_mixed_math.air.bc",
        SRCDIR "/fixtures/half_mixed_math.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: half_mixed_math.air.bc fixture not found\n"); return 1; }

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

    int n_f16 = 0, n_cap = 0, n_imul = 0, rc = 0;
    scan(spv, spv_sz, &n_f16, &n_cap, &n_imul);
    printf("OpTypeFloat-16=%d, OpCapability-Float16=%d, OpIMul=%d (%zu spv bytes)\n",
           n_f16, n_cap, n_imul, spv_sz);

    if (n_f16 < 1) {
        printf("FAIL: no OpTypeFloat 16 — `half` not modelled (fell back to int).\n");
        rc = 1;
    }
    if (n_cap < 1) {
        printf("FAIL: no OpCapability Float16 — half type emitted without its cap.\n");
        rc = 1;
    }
    if (n_imul != 0) {
        printf("FAIL: %d OpIMul present — a `fmul half` was mis-lowered to integer "
               "multiply (half mis-classified as int).\n", n_imul);
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: half lowers to OpTypeFloat 16 + Float16 cap, half arithmetic "
               "uses float ops.\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}
