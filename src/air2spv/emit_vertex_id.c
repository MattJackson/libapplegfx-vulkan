/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.vertex_id input
 * src/air2spv/emit_vertex_id.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "emit_vertex_id.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_vertex_id_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(96u);
    if (!b) return -1;

    /* OpCapability Shader */
    uint32_t cap_op[] = { LAGFX_SPV_CAPABILITY_SHADER };
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, cap_op, 1)) goto oom;

    /* OpMemoryModel Logical GLSL450 */
    uint32_t mm_ops[] = {
        LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
        LAGFX_SPV_MEMORY_MODEL_GLSL450,
    };
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, mm_ops, 2)) goto oom;

    /* Pre-allocate ids. */
    uint32_t id_void      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_uint      = lagfx_spv_builder_alloc_id(b);   /* OpTypeInt 32 0 */
    uint32_t id_float     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_in_u  = lagfx_spv_builder_alloc_id(b);   /* OpTypePointer Input %uint */
    uint32_t id_ptr_out_v = lagfx_spv_builder_alloc_id(b);   /* OpTypePointer Output %v4float */
    uint32_t id_fn_void   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c0        = lagfx_spv_builder_alloc_id(b);   /* float 0 */
    uint32_t id_c1        = lagfx_spv_builder_alloc_id(b);   /* float 1 */
    uint32_t id_vid_var   = lagfx_spv_builder_alloc_id(b);   /* Input uint */
    uint32_t id_pos_var   = lagfx_spv_builder_alloc_id(b);   /* Output vec4 */
    uint32_t id_main      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_loaded    = lagfx_spv_builder_alloc_id(b);   /* %uint loaded from vid_var */
    uint32_t id_vid_f     = lagfx_spv_builder_alloc_id(b);   /* float converted from %uint */
    uint32_t id_out_vec   = lagfx_spv_builder_alloc_id(b);   /* vec4 constructed */

    /* OpEntryPoint Vertex %main "main" %vid_var %pos_var */
    uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, id_main };
    uint32_t ep_suffix[] = { id_vid_var, id_pos_var };
    if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                            ep_prefix, 2,
                                            "main",
                                            ep_suffix, 2)) goto oom;

    /* OpDecorate %vid_var BuiltIn VertexIndex */
    {
        uint32_t ops[] = { id_vid_var,
                           LAGFX_SPV_DECORATION_BUILTIN,
                           LAGFX_SPV_BUILTIN_VERTEX_INDEX };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }
    /* OpDecorate %pos_var BuiltIn Position */
    {
        uint32_t ops[] = { id_pos_var,
                           LAGFX_SPV_DECORATION_BUILTIN,
                           LAGFX_SPV_BUILTIN_POSITION };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* Types */
    {
        uint32_t ops[] = { id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom;
    }
    {
        /* OpTypeInt %id 32 0 (32-bit unsigned) */
        uint32_t ops[] = { id_uint, 32, 0 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_INT, ops, 3)) goto oom;
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
        uint32_t ops[] = { id_ptr_in_u, LAGFX_SPV_STORAGE_INPUT, id_uint };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_ptr_out_v, LAGFX_SPV_STORAGE_OUTPUT, id_v4float };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_fn_void, id_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom;
    }

    /* Constants */
    {
        uint32_t ops[] = { id_float, id_c0, f32_bits(0.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_float, id_c1, f32_bits(1.0f) };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom;
    }

    /* Variables */
    {
        uint32_t ops[] = { id_ptr_in_u, id_vid_var, LAGFX_SPV_STORAGE_INPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_ptr_out_v, id_pos_var, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* Function */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %loaded = OpLoad %uint %vid_var */
    {
        uint32_t ops[] = { id_uint, id_loaded, id_vid_var };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom;
    }
    /* %vid_f = OpConvertUToF %float %loaded */
    {
        uint32_t ops[] = { id_float, id_vid_f, id_loaded };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONVERT_U_TO_F, ops, 3)) goto oom;
    }
    /* %out_vec = OpCompositeConstruct %v4float %vid_f %c0 %c0 %c1 */
    {
        uint32_t ops[] = { id_v4float, id_out_vec, id_vid_f, id_c0, id_c0, id_c1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, ops, 6)) goto oom;
    }
    /* OpStore %pos_var %out_vec */
    {
        uint32_t ops[] = { id_pos_var, id_out_vec };
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
