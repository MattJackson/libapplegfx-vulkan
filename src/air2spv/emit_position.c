/*
 * libapplegfx-vulkan — Phase 4 skeleton SPIR-V emitter for air.position
 * src/air2spv/emit_position.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Pattern reference for subsequent per-intrinsic emitters (air.read,
 * air.sample, etc.). Produces:
 *
 *   OpCapability Shader
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint Vertex %main "main" %pos
 *
 *   %void          = OpTypeVoid
 *   %float         = OpTypeFloat 32
 *   %v4float       = OpTypeVector %float 4
 *   %_ptr_out_v4f  = OpTypePointer Output %v4float
 *   %fn_void       = OpTypeFunction %void
 *   %c0            = OpConstant %float 0.0
 *   %c1            = OpConstant %float 1.0
 *   %const_pos     = OpConstantComposite %v4float %c0 %c0 %c0 %c1
 *
 *   %pos           = OpVariable %_ptr_out_v4f Output
 *                    OpDecorate %pos BuiltIn Position
 *
 *   %main = OpFunction %void None %fn_void
 *   %entry = OpLabel
 *           OpStore %pos %const_pos
 *           OpReturn
 *           OpFunctionEnd
 *
 * Validates clean under spirv-val and spirv-dis renders the expected
 * text per the test fixture.
 */

#include "emit_position.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    /* Bit-cast helper for OpConstant %float operand encoding. */
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_position_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(64u);
    if (!b) return -1;

    /* === Layout follows SPIR-V §2.4 Logical Layout ================= */

    /* 1. OpCapability Shader */
    uint32_t cap_op[] = { LAGFX_SPV_CAPABILITY_SHADER };
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, cap_op, 1)) goto oom;

    /* 2. OpMemoryModel Logical GLSL450 */
    uint32_t mm_ops[] = {
        LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
        LAGFX_SPV_MEMORY_MODEL_GLSL450,
    };
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, mm_ops, 2)) goto oom;

    /* Pre-allocate ids for forward references. */
    uint32_t id_void     = lagfx_spv_builder_alloc_id(b);  /* %void */
    uint32_t id_float    = lagfx_spv_builder_alloc_id(b);  /* %float */
    uint32_t id_v4float  = lagfx_spv_builder_alloc_id(b);  /* %v4float */
    uint32_t id_ptr_out  = lagfx_spv_builder_alloc_id(b);  /* %_ptr_Output_v4float */
    uint32_t id_fn_void  = lagfx_spv_builder_alloc_id(b);  /* %fn_void */
    uint32_t id_c0       = lagfx_spv_builder_alloc_id(b);  /* %c_zero */
    uint32_t id_c1       = lagfx_spv_builder_alloc_id(b);  /* %c_one */
    uint32_t id_const    = lagfx_spv_builder_alloc_id(b);  /* %const_pos */
    uint32_t id_pos      = lagfx_spv_builder_alloc_id(b);  /* %pos (output var) */
    uint32_t id_main     = lagfx_spv_builder_alloc_id(b);  /* %main (function) */
    uint32_t id_entry    = lagfx_spv_builder_alloc_id(b);  /* %entry (label) */

    /* 3. OpEntryPoint Vertex %main "main" %pos
     * Interface list at the end MUST include every Input / Output
     * variable the entry point uses (here, just %pos). */
    uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, id_main };
    uint32_t ep_suffix[] = { id_pos };
    if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                            ep_prefix, 2,
                                            "main",
                                            ep_suffix, 1)) goto oom;

    /* 4. OpDecorate %pos BuiltIn Position */
    uint32_t dec_ops[] = {
        id_pos,
        LAGFX_SPV_DECORATION_BUILTIN,
        LAGFX_SPV_BUILTIN_POSITION,
    };
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, dec_ops, 3)) goto oom;

    /* 5. Types */
    {
        uint32_t ops_void[] = { id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops_void, 1)) goto oom;
    }
    {
        /* OpTypeFloat %id 32 */
        uint32_t ops[] = { id_float, 32 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom;
    }
    {
        /* OpTypeVector %id %float 4 */
        uint32_t ops[] = { id_v4float, id_float, 4 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom;
    }
    {
        /* OpTypePointer %id Output %v4float */
        uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypeFunction %id %void  (no params -> only return type) */
        uint32_t ops[] = { id_fn_void, id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom;
    }

    /* 6. Constants */
    {
        /* OpConstant %float %id 0x0 */
        uint32_t ops[] = { id_float, id_c0, f32_bits(0.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, id_c1, f32_bits(1.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        /* OpConstantComposite %v4float %id %c0 %c0 %c0 %c1 */
        uint32_t ops[] = { id_v4float, id_const, id_c0, id_c0, id_c0, id_c1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 7. The output variable. OpVariable %_ptr_out_v4f %pos Output */
    {
        uint32_t ops[] = { id_ptr_out, id_pos, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* 8. Function body.
     *   %main  = OpFunction %void None %fn_void
     *   %entry = OpLabel
     *            OpStore %pos %const_pos
     *            OpReturn
     *            OpFunctionEnd                                      */
    {
        uint32_t ops[] = {
            id_void,
            id_main,
            LAGFX_SPV_FUNCTION_CONTROL_NONE,
            id_fn_void,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    {
        uint32_t ops[] = { id_pos, id_const };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom;
    }
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    /* Finalize. */
    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
