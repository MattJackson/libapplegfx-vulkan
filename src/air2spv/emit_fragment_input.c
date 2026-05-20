/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.fragment_input (fragment)
 * src/air2spv/emit_fragment_input.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "emit_fragment_input.h"
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
lagfx_air2spv_emit_fragment_input_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(96u);
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
    uint32_t id_void       = lagfx_spv_builder_alloc_id(b);  /* %void */
    uint32_t id_float      = lagfx_spv_builder_alloc_id(b);  /* %float */
    uint32_t id_v2float    = lagfx_spv_builder_alloc_id(b);  /* %v2float (input type) */
    uint32_t id_v4float    = lagfx_spv_builder_alloc_id(b);  /* %v4float (output type) */
    uint32_t id_ptr_in     = lagfx_spv_builder_alloc_id(b);  /* %_ptr_Input_v2float */
    uint32_t id_ptr_out    = lagfx_spv_builder_alloc_id(b);  /* %_ptr_Output_v4float */
    uint32_t id_fn_void    = lagfx_spv_builder_alloc_id(b);  /* %fn_void */
    uint32_t id_c0         = lagfx_spv_builder_alloc_id(b);  /* %c_float_0 */
    uint32_t id_c1         = lagfx_spv_builder_alloc_id(b);  /* %c_float_1 */
    uint32_t id_uv_var     = lagfx_spv_builder_alloc_id(b);  /* %uv (input var) */
    uint32_t id_color_var  = lagfx_spv_builder_alloc_id(b);  /* %color (output var) */
    uint32_t id_main       = lagfx_spv_builder_alloc_id(b);  /* %main (function) */
    uint32_t id_entry      = lagfx_spv_builder_alloc_id(b);  /* %entry (label) */
    uint32_t id_loaded     = lagfx_spv_builder_alloc_id(b);  /* %loaded (vec2 from uv) */
    uint32_t id_uv_x       = lagfx_spv_builder_alloc_id(b);  /* %uv_x (float x component) */
    uint32_t id_uv_y       = lagfx_spv_builder_alloc_id(b);  /* %uv_y (float y component) */
    uint32_t id_out_vec    = lagfx_spv_builder_alloc_id(b);  /* %out_vec (constructed vec4) */

    /* 3. OpEntryPoint Fragment %main "main" %uv %color
     * Interface list MUST include both Input and Output variables. */
    uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
    uint32_t ep_suffix[] = { id_uv_var, id_color_var };
    if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                            ep_prefix, 2,
                                            "main",
                                            ep_suffix, 2)) goto oom;

    /* 4. OpExecutionMode %main OriginUpperLeft — REQUIRED for fragment
     * stage per Vulkan (defines pixel origin convention). */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* 5. OpDecorate %uv Location 0 — fragment INPUTs use Location too. */
    {
        uint32_t ops[] = { id_uv_var, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 6. OpDecorate %color Location 0 — fragment OUTPUTs use Location. */
    {
        uint32_t ops[] = { id_color_var, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 7. Types */
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
        /* OpTypeVector %id %float 2 (for input) */
        uint32_t ops[] = { id_v2float, id_float, 2 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom;
    }
    {
        /* OpTypeVector %id %float 4 (for output) */
        uint32_t ops[] = { id_v4float, id_float, 4 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom;
    }
    {
        /* OpTypePointer %id Input %v2float (input variable pointer) */
        uint32_t ops[] = { id_ptr_in, LAGFX_SPV_STORAGE_INPUT, id_v2float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypePointer %id Output %v4float (output variable pointer) */
        uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        /* OpTypeFunction %id %void (no params -> only return type) */
        uint32_t ops[] = { id_fn_void, id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom;
    }

    /* 8. Constants: float 0.0 and 1.0 */
    {
        uint32_t ops[] = { id_float, id_c0, f32_bits(0.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, id_c1, f32_bits(1.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }

    /* 9. Variables */
    {
        /* OpVariable %_ptr_Input_v2float %uv Input */
        uint32_t ops[] = { id_ptr_in, id_uv_var, LAGFX_SPV_STORAGE_INPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }
    {
        /* OpVariable %_ptr_Output_v4float %color Output */
        uint32_t ops[] = { id_ptr_out, id_color_var, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* 10. Function body.
     *   %main = OpFunction %void None %fn_void
     *   %entry = OpLabel
     *            %loaded = OpLoad %v2float %uv
     *            %uv_x   = OpCompositeExtract %float %loaded 0
     *            %uv_y   = OpCompositeExtract %float %loaded 1
     *            %out    = OpCompositeConstruct %v4float %uv_x %uv_y %c0 %c1
     *            OpStore %color %out
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

    /* %loaded = OpLoad %v2float %uv_var */
    {
        uint32_t ops[] = { id_v2float, id_loaded, id_uv_var };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom;
    }

    /* %uv_x = OpCompositeExtract %float %loaded 0 */
    {
        uint32_t ops[] = { id_float, id_uv_x, id_loaded, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, ops, 4)) goto oom;
    }

    /* %uv_y = OpCompositeExtract %float %loaded 1 */
    {
        uint32_t ops[] = { id_float, id_uv_y, id_loaded, 1u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, ops, 4)) goto oom;
    }

    /* %out_vec = OpCompositeConstruct %v4float %uv_x %uv_y %c0 %c1 */
    {
        uint32_t ops[] = { id_v4float, id_out_vec, id_uv_x, id_uv_y, id_c0, id_c1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, ops, 6)) goto oom;
    }

    /* OpStore %color_var %out_vec */
    {
        uint32_t ops[] = { id_color_var, id_out_vec };
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
