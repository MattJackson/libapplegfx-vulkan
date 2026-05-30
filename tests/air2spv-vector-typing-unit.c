/*
 * libapplegfx-vulkan — vector result-type consistency regression
 * tests/air2spv-vector-typing-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards three coupled translator fixes (coverage audit 2026-05-30) that
 * together let general math vertex shaders translate cleanly:
 *
 *   - Vertex RET: store a bare float4 return directly instead of
 *     OpCompositeExtract field 0 (extracting field 0 of a vec4 yields a
 *     SCALAR mistyped as v4 -> spirv-val reject).
 *   - SHUFFLEVEC: result width = mask lane count (was hardcoded vec4), so
 *     the scalar->vec3 splat Apple emits for normalize()/mix(v3,v3,scalar)
 *     yields a v3, not a v4.
 *   - BINOP result type: a shuffle now records its float-vecN AIR type so a
 *     downstream `v3 * splat` (normalize) infers a v3 result, not a v4.
 *
 * The mini-checker below verifies arithmetic type consistency without
 * pulling in spirv-tools: collect each id's result-type id, then assert
 * every OpFMul / OpFAdd / OpFSub has result-type == both operand types.
 * Under any of the old bugs an FMul came out as v4float over v3 operands.
 *
 * Fixtures: mathclean_vertex (normalize+mix+pow+clamp+float4) and
 * scalarmath_vertex (scalar math + bare-float4 return), both xcrun metal.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC      0x07230203u
#define OP_FMUL        133u
#define OP_FADD        129u
#define OP_FSUB        131u

/* id -> result-type id. SPIR-V "typed result" instructions put the result
 * type in word 1 and the result id in word 2. We only need a flat map; ids
 * are small and bounded by the module's id bound (word 3 of the header). */
static int test_one(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: fixture not found: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); printf("FAIL: empty %s\n", path); return 1; }
    uint8_t *air = (uint8_t *)malloc((size_t)sz);
    if (fread(air, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(air); return 1; }
    fclose(f);

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, (size_t)sz, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open %s\n", path); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL: translate %s\n", path); lagfx_air_module_free(m); free(air); return 1;
    }
    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nwords = spv_sz / 4u;
    if (nwords < 5u || w[0] != SPV_MAGIC) {
        printf("FAIL: bad spv %s\n", path);
        free(spv); lagfx_air_module_free(m); free(air); return 1;
    }
    uint32_t bound = w[3];
    if (bound == 0u || bound > 100000u) bound = 100000u;
    uint32_t *type_of = (uint32_t *)calloc(bound, sizeof(uint32_t));

    /* Opcodes whose word layout is [hdr][result-type][result-id]... — the
     * common typed-result form. Pass 1: record type_of[result] for these.
     * We deliberately list the arithmetic + a few producers we cross-check. */
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        /* Heuristic: arithmetic, ExtInst, shuffle, composite, load, convert,
         * dot, vector-insert — all are [type][result] form. Record them. */
        if (wc >= 3u && (op == OP_FMUL || op == OP_FADD || op == OP_FSUB ||
                         op == 12u /*ExtInst*/ || op == 79u /*Shuffle*/ ||
                         op == 80u /*CompositeConstruct*/ || op == 81u /*CompExtract*/ ||
                         op == 61u /*Load*/ || op == 112u /*UToF*/ || op == 148u /*Dot*/ ||
                         op == 78u /*VInsertDyn*/)) {
            uint32_t rtype = w[i + 1u];
            uint32_t rid   = w[i + 2u];
            if (rid < bound) type_of[rid] = rtype;
        }
        i += wc;
    }

    /* Pass 2: arithmetic result type must equal each operand's result type. */
    int rc = 0, checked = 0;
    i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if ((op == OP_FMUL || op == OP_FADD || op == OP_FSUB) && wc == 5u) {
            uint32_t rtype = w[i + 1u];
            uint32_t a = w[i + 3u], b = w[i + 4u];
            uint32_t ta = (a < bound) ? type_of[a] : 0u;
            uint32_t tb = (b < bound) ? type_of[b] : 0u;
            checked++;
            if (ta != 0u && ta != rtype) {
                printf("FAIL: %s: arith op result-type %u != operand-A type %u "
                       "(vector width mismatch)\n", path, rtype, ta);
                rc = 1;
            }
            if (tb != 0u && tb != rtype) {
                printf("FAIL: %s: arith op result-type %u != operand-B type %u "
                       "(vector width mismatch)\n", path, rtype, tb);
                rc = 1;
            }
        }
        i += wc;
    }
    if (rc == 0)
        printf("PASS: %s — %d arithmetic ops type-consistent (%zu spv bytes)\n",
               path, checked, spv_sz);

    free(type_of); free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    const char *fix[][3] = {
        {"tests/fixtures/mathclean_vertex.air.bc",
         "../tests/fixtures/mathclean_vertex.air.bc",
         SRCDIR "/fixtures/mathclean_vertex.air.bc"},
        {"tests/fixtures/scalarmath_vertex.air.bc",
         "../tests/fixtures/scalarmath_vertex.air.bc",
         SRCDIR "/fixtures/scalarmath_vertex.air.bc"},
    };
    int rc = 0;
    for (int k = 0; k < 2; k++) {
        const char *chosen = NULL;
        for (int j = 0; j < 3; j++) {
            FILE *t = fopen(fix[k][j], "rb");
            if (t) { fclose(t); chosen = fix[k][j]; break; }
        }
        if (!chosen) { printf("FAIL: fixture %d not found\n", k); rc = 1; continue; }
        rc |= test_one(chosen);
    }
    return rc;
}
