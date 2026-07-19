/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * structured control flow (Pattern I).
 * src/air2spv/emit_control_flow.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Fragment shader: if (uv.x > 0.5) color = red; else color = blue;
 *
 *   ;; entry
 *   %uv     = OpLoad %v2f %uv_in
 *   %x      = OpCompositeExtract %float %uv 0
 *   %cond   = OpFOrdGreaterThan %bool %x %c_half
 *   OpSelectionMerge %merge None
 *   OpBranchConditional %cond %lbl_true %lbl_false
 *
 *   ;; lbl_true
 *   OpBranch %merge
 *
 *   ;; lbl_false
 *   OpBranch %merge
 *
 *   ;; merge
 *   %picked = OpPhi %v4f %red %lbl_true %blue %lbl_false
 *   OpStore %color_out %picked
 *   OpReturn
 *
 * Validates clean under `spirv-val`.
 */

#include "emit_control_flow.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_control_flow_stub(uint8_t **out_blob, size_t *out_size) {
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

    /* Ids — keep the SSA tightly grouped by phase. */
    uint32_t id_void      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_bool      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v2f       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4f       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_in_v2 = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out_v4 = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_0       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_1       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_half    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_red       = lagfx_spv_builder_alloc_id(b);  /* (1,0,0,1) */
    uint32_t id_blue      = lagfx_spv_builder_alloc_id(b);  /* (0,0,1,1) */
    uint32_t id_uv_in     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_color_out = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_lbl_true  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_lbl_false = lagfx_spv_builder_alloc_id(b);
    uint32_t id_lbl_merge = lagfx_spv_builder_alloc_id(b);
    uint32_t id_uv_val    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_x         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_cond      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_picked    = lagfx_spv_builder_alloc_id(b);

    {
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t suffix[] = { id_uv_in, id_color_out };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 2)) goto oom;
    }
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }
    {
        uint32_t ops[] = { id_uv_in, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_color_out, LAGFX_SPV_DECORATION_LOCATION, 0u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_bool };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_BOOL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v2f, id_float, 2 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v4f, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_in_v2,  LAGFX_SPV_STORAGE_INPUT,  id_v2f }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out_v4, LAGFX_SPV_STORAGE_OUTPUT, id_v4f }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* Constants */
    { uint32_t ops[] = { id_float, id_c_0,    f32_bits(0.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_1,    f32_bits(1.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_half, f32_bits(0.5f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    {
        uint32_t ops[] = { id_v4f, id_red,  id_c_1, id_c_0, id_c_0, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }
    {
        uint32_t ops[] = { id_v4f, id_blue, id_c_0, id_c_0, id_c_1, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* Variables */
    {
        uint32_t ops[] = { id_ptr_in_v2,  id_uv_in,     LAGFX_SPV_STORAGE_INPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }
    {
        uint32_t ops[] = { id_ptr_out_v4, id_color_out, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* Function body */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }

    /* entry block */
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %uv = OpLoad %v2f %uv_in */
    { uint32_t ops[] = { id_v2f, id_uv_val, id_uv_in };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }
    /* %x = OpCompositeExtract %float %uv 0 */
    { uint32_t ops[] = { id_float, id_x, id_uv_val, 0u };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, ops, 4)) goto oom; }
    /* %cond = OpFOrdGreaterThan %bool %x %c_half */
    { uint32_t ops[] = { id_bool, id_cond, id_x, id_c_half };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FORD_GREATER_THAN, ops, 4)) goto oom; }
    /* OpSelectionMerge %merge None */
    { uint32_t ops[] = { id_lbl_merge, LAGFX_SPV_SELECTION_CONTROL_NONE };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_SELECTION_MERGE, ops, 2)) goto oom; }
    /* OpBranchConditional %cond %lbl_true %lbl_false */
    { uint32_t ops[] = { id_cond, id_lbl_true, id_lbl_false };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, ops, 3)) goto oom; }

    /* true block */
    { uint32_t ops[] = { id_lbl_true };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_lbl_merge };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* false block */
    { uint32_t ops[] = { id_lbl_false };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_lbl_merge };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_BRANCH, ops, 1)) goto oom; }

    /* merge block — OpPhi picks based on incoming predecessor.
     *
     * OpPhi operand layout:
     *   Result Type <id>
     *   Result <id>
     *   (Variable <id>, Parent <id>) pair per incoming predecessor
     *
     * Here: red came from %lbl_true, blue from %lbl_false. */
    { uint32_t ops[] = { id_lbl_merge };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom; }
    {
        uint32_t ops[] = {
            id_v4f,
            id_picked,
            id_red,  id_lbl_true,
            id_blue, id_lbl_false,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_PHI, ops, 6)) goto oom;
    }
    { uint32_t ops[] = { id_color_out, id_picked };
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
