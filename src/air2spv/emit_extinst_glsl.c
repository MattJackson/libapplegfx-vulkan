/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * OpExtInst GLSL.std.450 demo (Pattern G).
 * src/air2spv/emit_extinst_glsl.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces:
 *
 *   OpCapability Shader
 *   %glsl = OpExtInstImport "GLSL.std.450"
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint Vertex %main "main" %pos
 *
 *   OpDecorate %pos BuiltIn Position
 *
 *   %void / %float / %v4float / %_ptr_out / %fn_void
 *   %c_4   = OpConstant %float 4.0
 *   %c_9   = OpConstant %float 9.0
 *   %c_16  = OpConstant %float 16.0
 *   %c_1   = OpConstant %float 1.0
 *   %input = OpConstantComposite %v4float %c_4 %c_9 %c_16 %c_1
 *   %pos   = OpVariable %_ptr_out_v4f Output
 *
 *   %main = OpFunction %void None %fn_void
 *   %entry = OpLabel
 *           %sqrt_v = OpExtInst %v4float %glsl Sqrt %input
 *                                                  ; sqrt(4,9,16,1) = (2,3,4,1)
 *           OpStore %pos %sqrt_v
 *           OpReturn
 *           OpFunctionEnd
 */

#include "emit_extinst_glsl.h"
#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t f32_bits(float f) {
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return out;
}

int
lagfx_air2spv_emit_extinst_glsl_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(96u);
    if (!b) return -1;

    /* Pre-allocate the GLSL import id BEFORE OpExtInstImport — that
     * op consumes the id as its Result <id>. */
    uint32_t id_glsl = lagfx_spv_builder_alloc_id(b);

    /* 1. OpCapability Shader */
    {
        uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom;
    }

    /* 2. OpExtInstImport %glsl "GLSL.std.450"  — must precede
     *    OpMemoryModel per SPIR-V §2.4. */
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
    uint32_t id_c_4       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_9       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_16      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_c_1       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_input_vec = lagfx_spv_builder_alloc_id(b);
    uint32_t id_pos       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_main      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_sqrt_res  = lagfx_spv_builder_alloc_id(b);

    /* 4. OpEntryPoint Vertex %main "main" %pos */
    {
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, id_main };
        uint32_t suffix[] = { id_pos };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 1)) goto oom;
    }

    /* 5. OpDecorate %pos BuiltIn Position */
    {
        uint32_t ops[] = { id_pos, LAGFX_SPV_DECORATION_BUILTIN, LAGFX_SPV_BUILTIN_POSITION };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom;
    }

    /* 6. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v4float, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4float }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 7. Constants: vec4(4.0, 9.0, 16.0, 1.0). */
    { uint32_t ops[] = { id_float, id_c_4,  f32_bits(4.0f)  }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_9,  f32_bits(9.0f)  }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_16, f32_bits(16.0f) }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_float, id_c_1,  f32_bits(1.0f)  }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    {
        uint32_t ops[] = { id_v4float, id_input_vec, id_c_4, id_c_9, id_c_16, id_c_1 };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 6)) goto oom;
    }

    /* 8. Variable */
    {
        uint32_t ops[] = { id_ptr_out, id_pos, LAGFX_SPV_STORAGE_OUTPUT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom;
    }

    /* 9. Function body */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %sqrt_res = OpExtInst %v4float %glsl Sqrt %input_vec
     *
     * Operand layout for OpExtInst:
     *   [Result Type, Result <id>, Set <id>, Literal Number: Instruction,
     *    Operand <id>, Operand <id>, ...] */
    {
        uint32_t ops[] = {
            id_v4float,
            id_sqrt_res,
            id_glsl,
            LAGFX_SPV_GLSL_SQRT,
            id_input_vec,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXT_INST, ops, 5)) goto oom;
    }
    {
        uint32_t ops[] = { id_pos, id_sqrt_res };
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
