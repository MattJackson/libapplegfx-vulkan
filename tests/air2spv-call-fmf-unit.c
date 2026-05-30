/*
 * libapplegfx-vulkan — CALL_FMF (fast-math-flags) operand-skip regression
 * tests/air2spv-call-fmf-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression guard for the CALL_FMF decode bug. An AIR CALL record's
 * cc field (llvm::CallMarkersFlags): bit 15 = CALL_EXPLICIT_TYPE,
 * bit 17 = CALL_FMF (an OPTIONAL fast-math-flags operand inserted BEFORE
 * fnty/callee/args). The translator didn't skip the FMF operand, so for
 * any fast-math call (≈ every Metal op) the callee + fnty + arg slots
 * were off by one → the callee resolved to a bogus value-id and the call
 * was DROPPED → its result type never propagated → downstream ops
 * mis-dispatched (e.g. a `float(vid)` conversion fed into `vec * 0.5f`
 * lowered to `OpIMul %uint` instead of `OpFMul %v4float`).
 *
 * Also guards the type-index-0 sentinel in call_return_air_type: a return
 * type of index 0 (float) was conflated with "no type" and dropped.
 *
 * Fixture: a vid-vertex shader `float4(float(vid),…) * 0.5f` compiled with
 * `xcrun metal` — its multiply exercises a fast-math conversion CALL feeding
 * a float multiply. Assert the multiply is OpFMul, never OpIMul. (The
 * fixture isn't fully spirv-val-clean yet — a separate return-struct
 * handling gap — so this is a targeted opcode-signature assertion.)
 */

#include "air2spv/translate.h"
#include "air2spv/spv_builder.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC 0x07230203u
_Static_assert(LAGFX_SPV_OP_FMUL == 133, "OpFMul must be 133");
_Static_assert(LAGFX_SPV_OP_IMUL == 132, "OpIMul must be 132");

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

static int count_opcode(const uint8_t *blob, size_t sz, uint16_t opcode) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u) return -1;
    size_t i = 5u;
    int count = 0;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        if (wc == 0u) break;
        if ((uint16_t)(header & 0xFFFFu) == opcode) count++;
        i += wc;
    }
    return count;
}

static int test_call_fmf(void) {
    const char *cands[] = {
        "tests/fixtures/call_fmf_convert.air.bc",
        "../tests/fixtures/call_fmf_convert.air.bc",
        SRCDIR "/fixtures/call_fmf_convert.air.bc",
        NULL,
    };
    uint8_t *air = NULL;
    size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: call_fmf_convert.air.bc fixture not found\n"); return 1; }

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

    int rc = 0;
    int n_fmul = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_FMUL);
    int n_imul = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_IMUL);
    printf("OpFMul=%d OpIMul=%d (%zu spv bytes)\n", n_fmul, n_imul, spv_sz);
    if (n_fmul < 1) {
        printf("FAIL: expected >=1 OpFMul (float multiply). The fast-math "
               "conversion CALL did not propagate its float type — CALL_FMF "
               "operand-skip regressed.\n");
        rc = 1;
    }
    if (n_imul != 0) {
        printf("FAIL: expected 0 OpIMul; got %d. A float multiply was "
               "mis-dispatched to integer — the CALL's float result type was "
               "lost (CALL_FMF skip or type-index-0 sentinel regressed).\n", n_imul);
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: fast-math conversion CALL propagates float type; "
               "vec*scalar multiply dispatches as OpFMul\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_call_fmf();
}
