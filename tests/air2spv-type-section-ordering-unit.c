/*
 * libapplegfx-vulkan — SPIR-V type/constant section-ordering invariant
 * tests/air2spv-type-section-ordering-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * SPIR-V §2.4 requires every OpType and OpConstant declaration to appear
 * BEFORE the first OpFunction (in the "types, variables and constants"
 * section). Emitting one mid-body — when an emit_inst_* handler lazily
 * creates a new type/constant during body emission — makes spirv-val
 * reject the module ("OpType… cannot appear in the graph definitions
 * section"). This has bitten the translator repeatedly:
 *   - a non-void CALL's result struct type (blocker A follow-on),
 *   - OpTypeBool created by the CMP/SELECT handlers,
 * each fixed by pre-emitting the type in the before-OpFunction pass.
 *
 * This test enforces the invariant directly on the word stream, so ANY
 * future handler that emits a stray type/constant mid-body fails the
 * build's test run regardless of whether a fixture happens to exercise
 * it under spirv-val. It runs the CMP/SELECT fixture (which forced an
 * OpTypeBool) through the translator and asserts no type/constant opcode
 * follows the first OpFunction. Toolchain-independent (no spirv-dis).
 */

#include "air2spv/translate.h"
#include "air2spv/spv_builder.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC 0x07230203u

/* Canonical SPIR-V core opcodes used below (spirv/unified1/spirv.h). */
#define OP_FUNCTION 54u
_Static_assert(LAGFX_SPV_OP_TYPE_BOOL == 20, "OpTypeBool must be 20");

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

/* Is this a type/constant-declaration opcode that MUST precede the first
 * OpFunction? OpType* span 19..39 (Void..ForwardPointer); OpConstant
 * family: ConstantTrue=41, ConstantFalse=42, Constant=43,
 * ConstantComposite=44, ConstantSampler=45, ConstantNull=46,
 * SpecConstant* 48..52. (OpVariable/OpUndef are allowed in a function
 * body, so they are intentionally excluded.) */
static int is_type_or_const_opcode(uint16_t op) {
    if (op >= 19u && op <= 39u) return 1;          /* OpType* */
    if (op >= 41u && op <= 46u) return 1;          /* OpConstant* */
    if (op >= 48u && op <= 52u) return 1;          /* OpSpecConstant* */
    return 0;
}

static int test_section_ordering(void) {
    const char *candidates[] = {
        "tests/fixtures/cmp_select_fragment.air.bc",
        "../tests/fixtures/cmp_select_fragment.air.bc",
        SRCDIR "/fixtures/cmp_select_fragment.air.bc",
        NULL,
    };
    uint8_t *air = NULL;
    size_t   air_len = 0;
    for (int i = 0; candidates[i]; i++) {
        air = slurp(candidates[i], &air_len);
        if (air) break;
    }
    if (!air) {
        printf("FAIL: cmp_select_fragment.air.bc fixture not found\n");
        return 1;
    }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open\n");
        free(air);
        return 1;
    }

    uint8_t *spv = NULL;
    size_t   spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL: translate\n");
        lagfx_air_module_free(m);
        free(air);
        return 1;
    }

    uint32_t magic;
    memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad SPIR-V magic 0x%08x\n", magic);
        free(spv); lagfx_air_module_free(m); free(air);
        return 1;
    }

    /* Walk the word stream. Once the first OpFunction is seen, no
     * type/constant declaration may follow. Also confirm OpTypeBool
     * (the opcode that motivated this guard) was emitted at all and
     * before OpFunction. */
    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nwords = spv_sz / 4u;
    size_t i = 5u;  /* skip 5-word module header */
    int seen_function = 0;
    int bad_decl_after_fn = 0;
    int seen_bool = 0;
    int bool_after_fn = 0;
    uint16_t offending_op = 0;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_FUNCTION) seen_function = 1;
        if (op == LAGFX_SPV_OP_TYPE_BOOL) {
            seen_bool = 1;
            if (seen_function) bool_after_fn = 1;
        }
        if (seen_function && is_type_or_const_opcode(op)) {
            if (!bad_decl_after_fn) offending_op = op;
            bad_decl_after_fn = 1;
        }
        i += wc;
    }

    int rc = 0;
    if (!seen_function) {
        printf("FAIL: no OpFunction in translated module\n");
        rc = 1;
    }
    if (bad_decl_after_fn) {
        printf("FAIL: a type/constant opcode (Op#%u) appears AFTER OpFunction "
               "— SPIR-V §2.4 violation (a handler emitted a type mid-body)\n",
               offending_op);
        rc = 1;
    }
    if (!seen_bool) {
        printf("FAIL: expected an OpTypeBool (fixture has a CMP/SELECT)\n");
        rc = 1;
    }
    if (bool_after_fn) {
        printf("FAIL: OpTypeBool emitted after OpFunction (the bug this "
               "guards against)\n");
        rc = 1;
    }
    if (rc == 0) {
        printf("PASS: all type/constant decls precede OpFunction; "
               "OpTypeBool correctly pre-emitted (%zu spv bytes)\n", spv_sz);
    }

    free(spv);
    lagfx_air_module_free(m);
    free(air);
    return rc;
}

int main(void) {
    return test_section_ordering();
}
