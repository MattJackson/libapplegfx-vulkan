/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter
 * (Binary ALU ops: OpFAdd, OpFSub, OpFDiv, OpDot).
 * src/air2spv/emit_alu_binops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces a fragment shader that exercises four binary ALU opcodes:
 *
 *   OpFAdd %v4f %sum %a %b          ; (0.8, 0.8, 0.8, 1.0)
 *   OpFSub %v4f %diff %sum %c       ; (0.7, 0.7, 0.7, 1.0)
 *   OpFDiv %v4f %quot %diff %d      ; (0.35, 0.35, 0.35, 1.0)
 *   OpDot  %float %dotv %a %a       ; ~1.36 (scalar result!)
 *
 * The dot product is computed but not used in the output — it's exercised
 * purely to validate the opcode path. Final color output is `quot`.
 */

#include "emit_alu_binops.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_alu_binops_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(128u);
    if (!b) return -1;

    /* === SPIR-V §2.4 logical layout =============================== */

    /* 1. OpCapability Shader */
    {
        uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom;
    }

    /* 2. OpMemoryModel Logical GLSL450 */
    {
        uint32_t ops[] = {
            LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
            LAGFX_SPV_MEMORY_MODEL_GLSL450,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom;
    }

    /* Pre-allocate ids. */
    uint32_t id_void       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void    = lagfx_spv_builder_alloc_id(b);

    /* Constants: 0.6, 0.2, 0.1, 2.0, and re-use for composites */
    uint32_t id_c_0        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_06       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_02       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_01       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_20       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_1        = lagfx_spv_builder_alloc_id(b);

    /* Composite constants: a, b, c, d (vec4s) */
    uint32_t id_a          = lagfx_spv_builder_alloc_id(b);
    uint32_t id_b          = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c          = lagfx_spv_builder_alloc_id(b);
    uint32_t id_d          = lagfx_spv_builder_alloc_id(b);

    /* Intermediates: sum, diff, quot (vec4s), dotv (float) */
    uint32_t id_sum        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_diff       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_quot       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_dotv       = lagfx_spv_builder_alloc_id(b);

    /* Variables: color_out */
    uint32_t id_color      = lagfx_spv_builder_alloc_id(b);

    /* Entry point / function */
    uint32_t id_main       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry      = lagfx_spv_builder_alloc_id(b);

    /* 3. OpEntryPoint Fragment %main "main" %color */
    {
        uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t ep_suffix[] = { id_color };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              ep_prefix, 2,
                                              "main",
                                              ep_suffix, 1)) goto oom;
    }

    /* 4. OpExecutionMode %main OriginUpperLeft — REQUIRED for fragment stage */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* 5. Decorations: OpDecorate %color Location 0 */
    {
        uint32_t ops[] = { id_color, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 6. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 7. Constants: scalar floats */
    { uint32_t ops[] = { id_float, id_c_0, f32_bits(0.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_06, f32_bits(0.6f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_02, f32_bits(0.2f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_01, f32_bits(0.1f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_20, f32_bits(2.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_1, f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }

    /* 8. Constants: OpConstantComposite for vec4s a, b, c, d */
    {
        uint32_t ops[] = { id_v4float, id_a, id_c_06, id_c_06, id_c_06, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4float, id_b, id_c_02, id_c_02, id_c_02, id_c_0 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4float, id_c, id_c_01, id_c_01, id_c_01, id_c_0 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4float, id_d, id_c_20, id_c_20, id_c_20, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 9. Variables: OpVariable %color Output */
    { uint32_t ops[] = { id_ptr_out, id_color, LAGFX_SPV_STORAGE_OUTPUT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }

    /* 10. Function body */
    { uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom; }
    { uint32_t ops[] = { id_entry }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }

    /* %sum = OpFAdd %v4float %a %b */
    { uint32_t ops[] = { id_v4float, id_sum, id_a, id_b }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FADD, ops, 4)) goto oom; }

    /* %diff = OpFSub %v4float %sum %c */
    { uint32_t ops[] = { id_v4float, id_diff, id_sum, id_c }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FSUB, ops, 4)) goto oom; }

    /* %quot = OpFDiv %v4float %diff %d */
    { uint32_t ops[] = { id_v4float, id_quot, id_diff, id_d }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FDIV, ops, 4)) goto oom; }

    /* %dotv = OpDot %float %a %a (scalar result!) */
    { uint32_t ops[] = { id_float, id_dotv, id_a, id_a }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DOT, ops, 4)) goto oom; }

    /* OpStore %color %quot */
    { uint32_t ops[] = { id_color, id_quot }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom; }

    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
