/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter: structured loop.
 * src/air2spv/emit_loop.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Fragment shader implementing:
 *   int i = 0;
 *   while (i < 4) { i++; }
 *   color = red;
 *
 * Demonstrates the canonical SPIR-V structured-loop layout:
 *
 *   entry:    OpStore %i_var %c_int_0
 *             OpBranch %header
 *
 *   header:   OpLoopMerge %merge %continue None
 *             OpBranch %body
 *
 *   body:     %cur  = OpLoad %int %i_var
 *             %cond = OpSLessThan %bool %cur %c_int_4
 *             OpBranchConditional %cond %inner %merge
 *
 *   inner:    %next = OpIAdd %int %cur %c_int_1
 *             OpStore %i_var %next
 *             OpBranch %continue
 *
 *   continue: OpBranch %header
 *
 *   merge:    OpStore %color_out %red
 *             OpReturn
 *
 * Function-storage OpVariable lives at the entry-block start (SPIR-V
 * §2.4 requires every Function-storage variable to be declared at
 * the function's entry).
 */

#include "emit_loop.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_loop_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(128u);
    if (!b) return -1;

    { uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom; }
    { uint32_t ops[] = {
          LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
          LAGFX_SPV_MEMORY_MODEL_GLSL450,
      };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom; }

    /* Ids */
    uint32_t id_void       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_bool       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_int        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4f        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_fn_int = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out_v4 = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_int_0    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_int_1    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_int_4    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_f_0      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_f_1      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_red        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_color_out  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_header     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_body       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_inner      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_cont       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_merge      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_i_var      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_cur        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_next       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_cond       = lagfx_spv_builder_alloc_id(b);

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

    /* Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_bool };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_BOOL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_int, 32, 1 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_INT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4f, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_fn_int, LAGFX_SPV_STORAGE_FUNCTION, id_int }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out_v4, LAGFX_SPV_STORAGE_OUTPUT,   id_v4f }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* Constants */
    { uint32_t ops[] = { id_int, id_c_int_0, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_int, id_c_int_1, 1u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_int, id_c_int_4, 4u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_f_0, f32_bits(0.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_f_1, f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    {
        uint32_t ops[] = { id_v4f, id_red, id_c_f_1, id_c_f_0, id_c_f_0, id_c_f_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* Output variable */
    {
        uint32_t ops[] = { id_ptr_out_v4, id_color_out, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* Function */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }

    /* entry block — declare Function-storage variable at top, then
     * initialize + branch to header. */
    { uint32_t ops[] = { id_entry };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    /* OpVariable %_ptr_fn_int %i_var Function — must be first in entry block. */
    { uint32_t ops[] = { id_ptr_fn_int, id_i_var, LAGFX_SPV_STORAGE_FUNCTION };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_i_var, id_c_int_0 };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_header };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* header — declare the loop. */
    { uint32_t ops[] = { id_header };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_merge, id_cont, LAGFX_SPV_LOOP_CONTROL_NONE };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOOP_MERGE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_body };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* body — check condition. */
    { uint32_t ops[] = { id_body };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_int, id_cur, id_i_var };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_bool, id_cond, id_cur, id_c_int_4 };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_SLESS_THAN, ops, 4)) goto oom; }
    { uint32_t ops[] = { id_cond, id_inner, id_merge };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, ops, 3)) goto oom; }

    /* inner — i++ */
    { uint32_t ops[] = { id_inner };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_int, id_next, id_cur, id_c_int_1 };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_IADD, ops, 4)) goto oom; }
    { uint32_t ops[] = { id_i_var, id_next };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_STORE, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_cont };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* continue — back to header. */
    { uint32_t ops[] = { id_cont };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_header };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* merge — post-loop. */
    { uint32_t ops[] = { id_merge };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_color_out, id_red };
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
