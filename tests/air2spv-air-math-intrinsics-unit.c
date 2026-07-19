/*
 * libapplegfx-vulkan — real-Apple math-intrinsic naming regression
 * tests/air2spv-air-math-intrinsics-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Regression guard for the intrinsic-naming fix (coverage audit 2026-05-30).
 *
 * Apple's metal compiler emits math intrinsics as `air.<op>` / `air.fast_<op>`
 * (e.g. air.mix.v3f32, air.dot.v3f32, air.fast_clamp.v3f32, air.fast_pow.v3f32)
 * — NOT the `air.fast.<op>` (dot-separated) convention the original intrinsic
 * table assumed. Under the old table every one of these fell through to the
 * "unrecognised non-void call -> typed OpUndef placeholder" path, so a math
 * shader translated to all-undef garbage. This guard asserts the real names
 * are now lowered to real SPIR-V ops:
 *   - air.dot.*          -> OpDot           (core op 148)
 *   - air.mix.*          -> OpExtInst FMix
 *   - air.fast_clamp.*   -> OpExtInst FClamp
 *   - air.fast_pow.*     -> OpExtInst Pow
 *
 * NOTE: this checks the intrinsics are LOWERED (not OpUndef), not that the
 * whole module passes spirv-val — the fixture also exercises scalar->vector
 * broadcast (mix(v3,v3,scalar)) which is a SEPARATE, still-open translator
 * gap (see the coverage-audit backlog). The point of this test is purely the
 * naming/recognition fix.
 *
 * Fixture: mathmin_vertex (dot + mix + clamp + pow) compiled with xcrun metal.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC       0x07230203u
#define SPV_OP_EXT_INST 12u
#define SPV_OP_DOT      148u

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

/* Count OpDot and OpExtInst occurrences in the SPIR-V stream. */
static void count_ops(const uint8_t *blob, size_t sz,
                      int *n_dot, int *n_extinst) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_dot = 0; *n_extinst = 0;
    if (nwords < 5u) return;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;
        if (op == SPV_OP_DOT) (*n_dot)++;
        else if (op == SPV_OP_EXT_INST) (*n_extinst)++;
        i += wc;
    }
}

static int test_math_intrinsics(void) {
    const char *cands[] = {
        "tests/fixtures/mathmin_vertex.air.bc",
        "../tests/fixtures/mathmin_vertex.air.bc",
        SRCDIR "/fixtures/mathmin_vertex.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: mathmin_vertex.air.bc fixture not found\n"); return 1; }

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

    int rc = 0, n_dot = 0, n_ext = 0;
    count_ops(spv, spv_sz, &n_dot, &n_ext);
    printf("OpDot count=%d, OpExtInst count=%d (%zu spv bytes)\n",
           n_dot, n_ext, spv_sz);

    /* dot(a,b) -> >=1 OpDot. mix + clamp + pow -> >=3 OpExtInst.
     * Under the broken naming all of these were OpUndef -> both counts 0. */
    if (n_dot < 1) {
        printf("FAIL: zero OpDot — air.dot.* not lowered (the math intrinsic "
               "naming regressed to the dead air.fast.<op> table).\n");
        rc = 1;
    }
    if (n_ext < 3) {
        printf("FAIL: OpExtInst count %d < 3 — air.mix / air.fast_clamp / "
               "air.fast_pow not lowered to GLSL.std.450 ext-insts.\n", n_ext);
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: real-Apple math intrinsics (air.dot/air.mix/"
               "air.fast_clamp/air.fast_pow) lower to OpDot + OpExtInst\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_math_intrinsics();
}
