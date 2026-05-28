/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * Unary float ops (OpFNegate + OpExtInst FAbs).
 * src/air2spv/emit_unop.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces a fragment shader that exercises two unary float patterns:
 *
 *   in    = vec4(-0.4, 0.3, -0.2, 1.0)
 *   neg   = OpFNegate %v4f in   -> (0.4, -0.3, 0.2, -1.0)
 *   abs   = OpExtInst FAbs in   -> (0.4, 0.3, 0.2, 1.0)
 *   sum   = OpFAdd neg abs      -> (0.8, 0.0, 0.4, 0.0)
 *   OpStore %color_out sum
 *
 * The dual-source distinction matters — when integrating into
 * translate_function.c later, the per-AIR-opcode dispatcher will route
 * to whichever path fits, so the catalog shows both shapes.
 */

#include "emit_unop.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_unop_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(96u);
    if (!b) return -1;

    /* Pre-allocate the GLSL import id BEFORE OpExtInstImport. */
    uint32_t id_glsl = lagfx_spv_builder_alloc_id(b);

    /* 1. OpCapability Shader */
    {
        uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom;
    }

    /* 2. OpExtInstImport %glsl "GLSL.std.450" */
    {
        uint32_t prefix[] = { id_glsl };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_EXT_INST_IMPORT,
                                              prefix, 1,
                                              "GLSL.std.450",
                                              NULL, 0)) goto oom;
    }

    /* 3. OpMemoryModel Logical GLSL450 */
    {
        uint32_t ops[] = {
            LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
            LAGFX_SPV_MEMORY_MODEL_GLSL450,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom;
    }

    /* Ids for the rest of the module. */
    uint32_t id_void      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_n4      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_3       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_n2      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_1       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_input     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_color_out = lagfx_spv_builder_alloc_id(b);
    uint32_t id_neg       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_abs       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_sum       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry     = lagfx_spv_builder_alloc_id(b);

    /* 4. OpEntryPoint Fragment %main "main" %color_out */
    {
        uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t ep_suffix[] = { id_color_out };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              ep_prefix, 2,
                                              "main",
                                              ep_suffix, 1)) goto oom;
    }

    /* 5. OpExecutionMode %main OriginUpperLeft — REQUIRED for fragment stage */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* 6. Decorations: OpDecorate %color_out Location 0 */
    {
        uint32_t ops[] = { id_color_out, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 7. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 8. Constants: vec4(-0.4, 0.3, -0.2, 1.0). */
    { uint32_t ops[] = { id_float, id_c_n4, f32_bits(-0.4f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_3,  f32_bits(0.3f)  }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_n2, f32_bits(-0.2f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_1,  f32_bits(1.0f)  }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    {
        uint32_t ops[] = { id_v4float, id_input, id_c_n4, id_c_3, id_c_n2, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 9. Variables: OpVariable %color_out Output */
    { uint32_t ops[] = { id_ptr_out, id_color_out, LAGFX_SPV_STORAGE_OUTPUT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }

    /* 10. Function body */
    { uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom; }
    { uint32_t ops[] = { id_entry }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }

    /* %neg = OpFNegate %v4float %input
     *
     * Operand layout for OpFNegate (core SPIR-V unary op):
     *   [Result Type, Result <id>, Operand <id>] — exactly 3 operands. */
    { uint32_t ops[] = { id_v4float, id_neg, id_input }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FNEGATE, ops, 3)) goto oom; }

    /* %abs = OpExtInst %v4float %glsl FAbs %input
     *
     * Operand layout for OpExtInst:
     *   [Result Type, Result <id>, Set <id>, Literal Number: Instruction,
     *    Operand <id>] */
    {
        uint32_t ops[] = {
            id_v4float,
            id_abs,
            id_glsl,
            LAGFX_SPV_GLSL_FABS,
            id_input,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXT_INST, ops, 5)) goto oom;
    }

    /* %sum = OpFAdd %v4float %neg %abs */
    { uint32_t ops[] = { id_v4float, id_sum, id_neg, id_abs }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FADD, ops, 4)) goto oom; }

    /* OpStore %color_out %sum */
    { uint32_t ops[] = { id_color_out, id_sum }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom; }

    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
