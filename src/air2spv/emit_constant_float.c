/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * multi-distinct-float OpConstantComposite (clear-color shape).
 * src/air2spv/emit_constant_float.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces a fragment shader writing vec4(0.2, 0.5, 0.8, 1.0) to
 * Location 0 via OpConstantComposite of four distinct float
 * constants. Differs from emit_render_target (which uses only 0.0
 * and 1.0): exercises the bitcode-level CST_CODE_FLOAT path with
 * non-trivial bit patterns.
 */

#include "emit_constant_float.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_constant_float_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(64u);
    if (!b) return -1;

    /* 1. Capability + memory model */
    {
        uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom;
    }
    {
        uint32_t ops[] = {
            LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
            LAGFX_SPV_MEMORY_MODEL_GLSL450,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom;
    }

    /* Ids */
    uint32_t id_void    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_r     = lagfx_spv_builder_alloc_id(b);  /* 0.2 */
    uint32_t id_c_g     = lagfx_spv_builder_alloc_id(b);  /* 0.5 */
    uint32_t id_c_b     = lagfx_spv_builder_alloc_id(b);  /* 0.8 */
    uint32_t id_c_a     = lagfx_spv_builder_alloc_id(b);  /* 1.0 */
    uint32_t id_color_v = lagfx_spv_builder_alloc_id(b);  /* composite */
    uint32_t id_color   = lagfx_spv_builder_alloc_id(b);  /* output var */
    uint32_t id_main    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry   = lagfx_spv_builder_alloc_id(b);

    /* 2. OpEntryPoint Fragment %main "main" %color */
    {
        uint32_t prefix[]  = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t suffix[]  = { id_color };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 1)) goto oom;
    }

    /* 3. OpExecutionMode OriginUpperLeft — required for fragment. */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* 4. OpDecorate %color Location 0 */
    {
        uint32_t ops[] = { id_color, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 5. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 6. Four distinct float constants → composite. */
    { uint32_t ops[] = { id_float, id_c_r, f32_bits(0.2f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_g, f32_bits(0.5f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_b, f32_bits(0.8f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_a, f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    /* %color_const = OpConstantComposite %v4float %c_r %c_g %c_b %c_a */
    {
        uint32_t ops[] = { id_v4float, id_color_v, id_c_r, id_c_g, id_c_b, id_c_a };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 7. Output variable */
    {
        uint32_t ops[] = { id_ptr_out, id_color, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* 8. Function body — store the composite and return. */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    {
        uint32_t ops[] = { id_color, id_color_v };
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
