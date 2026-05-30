/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter
 * (Pattern E: Uniform-storage-class buffer load).
 * src/air2spv/emit_buffer_load.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces:
 *
 *   OpCapability Shader
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint Vertex %main "main" %pos
 *
 *   OpDecorate %ubo_type Block
 *   OpMemberDecorate %ubo_type 0 Offset 0
 *   OpDecorate %ubo_var DescriptorSet 0
 *   OpDecorate %ubo_var Binding 0
 *   OpDecorate %pos BuiltIn Position
 *
 *   %void           = OpTypeVoid
 *   %float          = OpTypeFloat 32
 *   %v4float        = OpTypeVector %float 4
 *   %ubo_type       = OpTypeStruct %v4float            ; the UBO block
 *   %_ptr_Uni_ubo   = OpTypePointer Uniform %ubo_type
 *   %_ptr_Uni_v4f   = OpTypePointer Uniform %v4float   ; for OpAccessChain
 *   %_ptr_Out_v4f   = OpTypePointer Output  %v4float
 *   %int            = OpTypeInt 32 1
 *   %fn_void        = OpTypeFunction %void
 *
 *   %c_int_0        = OpConstant %int 0
 *   %c_f_0          = OpConstant %float 0.0
 *   %c_f_1          = OpConstant %float 1.0
 *   %const_pos      = OpConstantComposite %v4float %c_f_0 %c_f_0 %c_f_0 %c_f_1
 *
 *   %ubo_var        = OpVariable %_ptr_Uni_ubo Uniform
 *   %pos            = OpVariable %_ptr_Out_v4f Output
 *
 *   %main = OpFunction %void None %fn_void
 *   %entry = OpLabel
 *           %ptr_factor = OpAccessChain %_ptr_Uni_v4f %ubo_var %c_int_0
 *           %factor     = OpLoad %v4float %ptr_factor
 *           %result     = OpFMul %v4float %const_pos %factor
 *           OpStore %pos %result
 *           OpReturn
 *           OpFunctionEnd
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 */

#include "emit_buffer_load.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_buffer_load_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(96u);
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

    /* Pre-allocate ids for forward references. */
    uint32_t id_void       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_int        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ubo_struct = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_uni_ubo = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_uni_v4 = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out_v4 = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_int_0    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_f_0      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_f_1      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_const_pos  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ubo_var    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_pos        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_factor = lagfx_spv_builder_alloc_id(b);
    uint32_t id_factor     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_result     = lagfx_spv_builder_alloc_id(b);

    /* 3. OpEntryPoint Vertex %main "main" %ubo_var %pos
     * Vulkan 1.4 requires every Input / Output / Uniform variable the
     * entry point uses to appear in the interface list. Older SPIR-V
     * (pre-1.4) only required Input + Output, but listing Uniform too
     * is forward-compatible and spirv-val accepts it under
     * vulkan1.0. */
    {
        uint32_t prefix[]  = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, id_main };
        uint32_t suffix[]  = { id_ubo_var, id_pos };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 2)) goto oom;
    }

    /* 4. Decorations.
     *
     *    OpDecorate %ubo_struct Block            ; uniform-buffer block
     *    OpMemberDecorate %ubo_struct 0 Offset 0 ; vec4 lives at byte 0
     *    OpDecorate %ubo_var DescriptorSet 0
     *    OpDecorate %ubo_var Binding 0
     *    OpDecorate %pos BuiltIn Position
     */
    {
        uint32_t ops[] = { id_ubo_struct, LAGFX_SPV_DECORATION_BLOCK };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 2)) goto oom;
    }
    {
        /* OpMemberDecorate %struct member_idx Offset offset_value */
        uint32_t ops[] = { id_ubo_struct, 0u, LAGFX_SPV_DECORATION_OFFSET, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMBER_DECORATE, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_ubo_var, LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_ubo_var, LAGFX_SPV_DECORATION_BINDING, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_pos, LAGFX_SPV_DECORATION_BUILTIN, LAGFX_SPV_BUILTIN_POSITION };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 5. Types */
    {
        uint32_t ops[] = { id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, 32 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4float, id_float, 4 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom;
    }
    {
        /* OpTypeInt %id 32 1   (signed 32-bit; needed for OpAccessChain index) */
        uint32_t ops[] = { id_int, 32, 1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_INT, ops, 3)) goto oom;
    }
    {
        /* OpTypeStruct %id %v4float (single member) */
        uint32_t ops[] = { id_ubo_struct, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_STRUCT, ops, 2)) goto oom;
    }
    {
        /* OpTypePointer %id Uniform %ubo_struct */
        uint32_t ops[] = { id_ptr_uni_ubo, LAGFX_SPV_STORAGE_UNIFORM, id_ubo_struct };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypePointer %id Uniform %v4float (for OpAccessChain result) */
        uint32_t ops[] = { id_ptr_uni_v4, LAGFX_SPV_STORAGE_UNIFORM, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypePointer %id Output %v4float */
        uint32_t ops[] = { id_ptr_out_v4, LAGFX_SPV_STORAGE_OUTPUT, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypeFunction %id %void */
        uint32_t ops[] = { id_fn_void, id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom;
    }

    /* 6. Constants */
    {
        /* OpConstant %int %c_int_0 0 */
        uint32_t ops[] = { id_int, id_c_int_0, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, id_c_f_0, f32_bits(0.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, id_c_f_1, f32_bits(1.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4float, id_const_pos, id_c_f_0, id_c_f_0, id_c_f_0, id_c_f_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 7. Variables.
     *
     *    %ubo_var = OpVariable %_ptr_Uni_ubo Uniform
     *    %pos     = OpVariable %_ptr_Out_v4f Output
     */
    {
        uint32_t ops[] = { id_ptr_uni_ubo, id_ubo_var, LAGFX_SPV_STORAGE_UNIFORM };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_ptr_out_v4, id_pos, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* 8. Function body. */
    {
        uint32_t ops[] = {
            id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %ptr_factor = OpAccessChain %_ptr_Uni_v4f %ubo_var %c_int_0 */
    {
        uint32_t ops[] = { id_ptr_uni_v4, id_ptr_factor, id_ubo_var, id_c_int_0 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_ACCESS_CHAIN, ops, 4)) goto oom;
    }
    /* %factor = OpLoad %v4float %ptr_factor */
    {
        uint32_t ops[] = { id_v4float, id_factor, id_ptr_factor };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom;
    }
    /* %result = OpFMul %v4float %const_pos %factor */
    {
        uint32_t ops[] = { id_v4float, id_result, id_const_pos, id_factor };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FMUL, ops, 4)) goto oom;
    }
    /* OpStore %pos %result */
    {
        uint32_t ops[] = { id_pos, id_result };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom;
    }
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
