/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.render_target
 * src/air2spv/emit_render_target.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "emit_render_target.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_render_target_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(64u);
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
    uint32_t id_void     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c0       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c1       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_red_vec  = lagfx_spv_builder_alloc_id(b);  /* (1,0,0,1) */
    uint32_t id_color    = lagfx_spv_builder_alloc_id(b);  /* Output var */
    uint32_t id_main     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry    = lagfx_spv_builder_alloc_id(b);

    /* OpEntryPoint Fragment %main "main" %color */
    uint32_t ep_prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
    uint32_t ep_suffix[] = { id_color };
    if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                            ep_prefix, 2,
                                            "main",
                                            ep_suffix, 1)) goto oom;

    /* OpExecutionMode %main OriginUpperLeft — REQUIRED for fragment
     * stage per Vulkan. */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* OpDecorate %color Location 0 — fragment outputs use Location,
     * not BuiltIn (the latter is for things like FragCoord). */
    {
        uint32_t ops[] = { id_color, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* Constants: red (1, 0, 0, 1). */
    { uint32_t ops[] = { id_float, id_c0, f32_bits(0.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c1, f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_red_vec, id_c1, id_c0, id_c0, id_c1 };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom; }

    /* Variable */
    { uint32_t ops[] = { id_ptr_out, id_color, LAGFX_SPV_STORAGE_OUTPUT };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }

    /* Function: store the constant red and return. */
    { uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom; }
    { uint32_t ops[] = { id_entry };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_color, id_red_vec };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom; }
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
