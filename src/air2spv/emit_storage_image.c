/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * storage-image read/write (Pattern K). src/air2spv/emit_storage_image.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces:
 *
 *   OpCapability Shader
 *   OpCapability StorageImageExtendedFormats (for Rgba8 format)
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint GLCompute %main "main" %in_img %out_img
 *   OpExecutionMode %main LocalSize 1 1 1
 *
 *   OpDecorate %in_img  DescriptorSet 0
 *   OpDecorate %in_img  Binding 0
 *   OpDecorate %out_img DescriptorSet 0
 *   OpDecorate %out_img Binding 1
 *
 *   %void  = OpTypeVoid
 *   %float = OpTypeFloat 32
 *   %int   = OpTypeInt 32 1
 *   %v4f   = OpTypeVector %float 4
 *   %v2i   = OpTypeVector %int 2
 *   %image = OpTypeImage %float 2D 0 0 0 2 Rgba8
 *   %ptr_uc_in_img   = OpTypePointer UniformConstant %image
 *   %ptr_uc_out_img  = OpTypePointer UniformConstant %image
 *   %fn_void         = OpTypeFunction %void
 *
 *   %in_img  = OpVariable %ptr_uc_in_img  UniformConstant
 *   %out_img = OpVariable %ptr_uc_out_img UniformConstant
 *
 *   %main = OpFunction %void None %fn_void
 *   %entry = OpLabel
 *           %img_val     = OpLoad %image %in_img
 *           %coord_const = OpConstantComposite %v2i %int_0 %int_0
 *           %texel       = OpImageRead %v4f %img_val %coord_const
 *           %out_img_val = OpLoad %image %out_img
 *   OpImageWrite %out_img_val %coord_const %texel
 *           OpReturn
 *           OpFunctionEnd
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 */

#include "emit_storage_image.h"
#include "spv_builder.h"

#include <stdlib.h>

int
lagfx_air2spv_emit_storage_image_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(256u);
    if (!b) return -1;

    /* 1. OpCapability Shader — covers basic image operations.
     *    (An earlier draft also declared `Capability::Image`, but
     *    SPIR-V capability 3 is actually `Tessellation`; image ops
     *    don't need a separate capability declaration.) */
    { uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom; }

    /* 2. OpCapability StorageImageExtendedFormats — required for
     *    OpTypeImage with a concrete format like Rgba8. */
    { uint32_t ops[] = { LAGFX_SPV_CAPABILITY_STORAGE_IMAGE_EXTENDED_FORMATS };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom; }

    /* 3. OpMemoryModel Logical GLSL450 */
    {
        uint32_t ops[] = {
            LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
            LAGFX_SPV_MEMORY_MODEL_GLSL450,
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2)) goto oom;
    }

    /* Ids */
    uint32_t id_void        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_float       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_int         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4f         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v2i         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_image_t     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_uc_img  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_in_img      = lagfx_spv_builder_alloc_id(b);   /* input storage image */
    uint32_t id_out_img     = lagfx_spv_builder_alloc_id(b);   /* output storage image */
    uint32_t id_main        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_int_0       = lagfx_spv_builder_alloc_id(b);   /* int constant 0 */
    uint32_t id_coord_const = lagfx_spv_builder_alloc_id(b);   /* int2 composite (0,0) */
    uint32_t id_img_val     = lagfx_spv_builder_alloc_id(b);   /* loaded input image */
    uint32_t id_texel       = lagfx_spv_builder_alloc_id(b);   /* OpImageRead result */
    uint32_t id_out_img_val = lagfx_spv_builder_alloc_id(b);   /* loaded output image */

    /* 6. OpEntryPoint GLCompute %main "main" %in_img %out_img
     *    SPIR-V 1.4+ (we now emit 1.4) requires the OpEntryPoint interface
     *    list to contain EVERY global statically used by the entry point,
     *    including UniformConstant storage images (1.0-1.3 listed only
     *    Input/Output). Descriptor-set + binding decorations still locate
     *    them in the pipeline. */
    {
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_GLCOMPUTE, id_main };
        uint32_t suffix[] = { id_in_img, id_out_img };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 2)) goto oom;
    }

    /* 7. OpExecutionMode LocalSize 1 1 1 */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_LOCAL_SIZE, 1u, 1u, 1u };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 5)) goto oom;
    }

    /* 8. Decorations */
    { uint32_t ops[] = { id_in_img,  LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_in_img,  LAGFX_SPV_DECORATION_BINDING,        0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_out_img, LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_out_img, LAGFX_SPV_DECORATION_BINDING,        1u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }

    /* 9. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_int, 32, 1u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_INT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v4f, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v2i, id_int, 2 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }

    /* OpTypeImage %id sampled_type Dim Depth Arrayed MS Sampled ImageFormat
     * Sampled=2 means storage image (no sampler companion). */
    {
        uint32_t ops[] = {
            id_image_t,
            id_float,                          /* sampled_type */
            LAGFX_SPV_DIM_2D,                  /* Dim */
            0u,                                /* Depth */
            0u,                                /* Arrayed */
            0u,                                /* MS */
            2u,                                /* Sampled = 2 (storage image) */
            LAGFX_SPV_IMAGE_FORMAT_RGBA8,      /* ImageFormat - concrete format required */
        };
        /* 8 operands: result, sampled_type, Dim, Depth, Arrayed, MS,
         * Sampled, ImageFormat. (Earlier draft passed `9` here while
         * the array had 8 entries — the 9th word was uninitialized
         * stack which spirv-val reported as "Invalid access qualifier
         * operand: 5". AccessQualifier is OPTIONAL and we don't emit
         * it; Vulkan's image-format inference covers our usage.) */
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_IMAGE, ops, 8)) goto oom;
    }

    /* Storage class for storage images = UniformConstant (0).
     * SPIR-V storage class 4 is `Workgroup`, NOT "StorageBuffer". */
    { uint32_t ops[] = { id_ptr_uc_img,  LAGFX_SPV_STORAGE_UNIFORM_CONSTANT, id_image_t   }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 8. Constants */
    { uint32_t ops[] = { id_int, id_int_0, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v2i, id_coord_const, id_int_0, id_int_0 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CONSTANT_COMPOSITE, ops, 4)) goto oom; }

    /* 11. Variables */
    { uint32_t ops[] = { id_ptr_uc_img,  id_in_img,  LAGFX_SPV_STORAGE_UNIFORM_CONSTANT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_uc_img,  id_out_img, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }

    /* 12. Function body */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }

    /* %img_val = OpLoad %image %in_img */
    { uint32_t ops[] = { id_image_t, id_img_val, id_in_img };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }

    /* %coord_const already defined above as OpConstantComposite */

    /* %texel = OpImageRead %v4f %img_val %coord_const */
    { uint32_t ops[] = { id_v4f, id_texel, id_img_val, id_coord_const };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_IMAGE_READ, ops, 4)) goto oom; }

    /* %out_img_val = OpLoad %image %out_img */
    { uint32_t ops[] = { id_image_t, id_out_img_val, id_out_img };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }

    /* OpImageWrite %out_img_val %coord_const %texel */
    { uint32_t ops[] = { id_out_img_val, id_coord_const, id_texel };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_IMAGE_WRITE, ops, 3)) goto oom; }

    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_RETURN, NULL, 0)) goto oom;
    if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0)) goto oom;

    *out_blob = lagfx_spv_builder_finish(b, out_size);
    lagfx_spv_builder_free(b);
    return (*out_blob == NULL) ? -1 : 0;

oom:
    lagfx_spv_builder_free(b);
    return -1;
}
