/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter: OpVectorShuffle.
 * src/air2spv/emit_vector_shuffle.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces a fragment shader that reverses vec4(0.2, 0.5, 0.8, 1.0)
 * via OpVectorShuffle .wzyx, writing the result vec4(1.0, 0.8, 0.5, 0.2)
 * to Location 0.
 *
 * OpVectorShuffle operand layout (SPIR-V §3.32):
 *   Result Type <id>
 *   Result <id>
 *   Vector 1 <id>
 *   Vector 2 <id>            (may equal Vector 1 for unary swizzles)
 *   Components 0..N-1        (literal indices into the concatenation
 *                              Vector 1 followed by Vector 2)
 */

#include "emit_vector_shuffle.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_vector_shuffle_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(80u);
    if (!b) return -1;

    { uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom; }
    { uint32_t ops[] = {
          LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
          LAGFX_SPV_MEMORY_MODEL_GLSL450,
      };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom; }

    uint32_t id_void      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4float   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_r       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_g       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_b       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_a       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_input_vec = lagfx_spv_builder_alloc_id(b);
    uint32_t id_color_out = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_shuffled  = lagfx_spv_builder_alloc_id(b);

    {
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t suffix[] = { id_color_out };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 1)) goto oom;
    }
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }
    {
        uint32_t ops[] = { id_color_out, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    { uint32_t ops[] = { id_float, id_c_r, f32_bits(0.2f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_g, f32_bits(0.5f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_b, f32_bits(0.8f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_a, f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    {
        uint32_t ops[] = { id_v4float, id_input_vec, id_c_r, id_c_g, id_c_b, id_c_a };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }
    {
        uint32_t ops[] = { id_ptr_out, id_color_out, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %shuffled = OpVectorShuffle %v4float %input_vec %input_vec 3 2 1 0
     *
     * Both source vectors are the same (idiomatic for unary swizzle).
     * Component indices 0..3 select from Vector 1; 4..7 from Vector 2.
     * Here we pick 3,2,1,0 → all from Vector 1 in reverse. */
    {
        uint32_t ops[] = {
            id_v4float,
            id_shuffled,
            id_input_vec, id_input_vec,
            3u, 2u, 1u, 0u,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VECTOR_SHUFFLE, ops, 8)) goto oom;
    }
    {
        uint32_t ops[] = { id_color_out, id_shuffled };
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
