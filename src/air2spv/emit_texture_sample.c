/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * separate image + sampler texture-sample (Pattern F).
 * src/air2spv/emit_texture_sample.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Produces:
 *
 *   OpCapability Shader
 *   OpMemoryModel Logical GLSL450
 *   OpEntryPoint Fragment %main "main" %uv_in %color_out
 *   OpExecutionMode %main OriginUpperLeft
 *
 *   OpDecorate %tex      DescriptorSet 0
 *   OpDecorate %tex      Binding 0
 *   OpDecorate %samp     DescriptorSet 0
 *   OpDecorate %samp     Binding 1
 *   OpDecorate %uv_in    Location 0
 *   OpDecorate %color_out Location 0
 *
 *   %void  = OpTypeVoid
 *   %float = OpTypeFloat 32
 *   %v2f   = OpTypeVector %float 2
 *   %v4f   = OpTypeVector %float 4
 *   %image = OpTypeImage %float 2D 0 0 0 1 Unknown
 *   %sampler = OpTypeSampler
 *   %sampled_image = OpTypeSampledImage %image
 *   %_ptr_uc_image   = OpTypePointer UniformConstant %image
 *   %_ptr_uc_sampler = OpTypePointer UniformConstant %sampler
 *   %_ptr_in_v2f     = OpTypePointer Input  %v2f
 *   %_ptr_out_v4f    = OpTypePointer Output %v4f
 *   %fn_void = OpTypeFunction %void
 *
 *   %tex     = OpVariable %_ptr_uc_image   UniformConstant
 *   %samp    = OpVariable %_ptr_uc_sampler UniformConstant
 *   %uv_in   = OpVariable %_ptr_in_v2f    Input
 *   %color_out = OpVariable %_ptr_out_v4f Output
 *
 *   %main = OpFunction %void None %fn_void
 *   %entry = OpLabel
 *           %uv         = OpLoad %v2f %uv_in
 *           %img_val    = OpLoad %image %tex
 *           %samp_val   = OpLoad %sampler %samp
 *           %sampled    = OpSampledImage %sampled_image %img_val %samp_val
 *           %rgba       = OpImageSampleImplicitLod %v4f %sampled %uv
 *           OpStore %color_out %rgba
 *           OpReturn
 *           OpFunctionEnd
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 */

#include "emit_texture_sample.h"
#include "spv_builder.h"

#include <stdlib.h>

int
lagfx_air2spv_emit_texture_sample_stub(uint8_t **out_blob, size_t *out_size) {
    if (!out_blob || !out_size) return -1;
    *out_blob = NULL;
    *out_size = 0u;

    lagfx_spv_builder_t *b = lagfx_spv_builder_create(128u);
    if (!b) return -1;

    /* 1. OpCapability Shader */
    { uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_CAPABILITY, ops, 1)) goto oom; }

    /* 2. OpMemoryModel Logical GLSL450 */
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
    uint32_t id_v2f         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_v4f         = lagfx_spv_builder_alloc_id(b);
    uint32_t id_image_t     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_sampler_t   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_sampimg_t   = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_uc_img  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_uc_samp = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_in_v2f  = lagfx_spv_builder_alloc_id(b);
    uint32_t id_ptr_out_v4f = lagfx_spv_builder_alloc_id(b);
    uint32_t id_fn_void     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_tex         = lagfx_spv_builder_alloc_id(b);  /* image var */
    uint32_t id_samp        = lagfx_spv_builder_alloc_id(b);  /* sampler var */
    uint32_t id_uv_in       = lagfx_spv_builder_alloc_id(b);  /* input var */
    uint32_t id_color_out   = lagfx_spv_builder_alloc_id(b);  /* output var */
    uint32_t id_main        = lagfx_spv_builder_alloc_id(b);
    uint32_t id_entry       = lagfx_spv_builder_alloc_id(b);
    uint32_t id_uv_val      = lagfx_spv_builder_alloc_id(b);
    uint32_t id_img_val     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_samp_val    = lagfx_spv_builder_alloc_id(b);
    uint32_t id_sampled     = lagfx_spv_builder_alloc_id(b);
    uint32_t id_rgba        = lagfx_spv_builder_alloc_id(b);

    /* 3. OpEntryPoint Fragment %main "main" %uv_in %color_out */
    {
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, id_main };
        uint32_t suffix[] = { id_uv_in, id_color_out };
        if (!lagfx_spv_builder_emit_op_string(b, LAGFX_SPV_OP_ENTRY_POINT,
                                              prefix, 2,
                                              "main",
                                              suffix, 2)) goto oom;
    }

    /* 4. OpExecutionMode OriginUpperLeft (fragment, Vulkan convention). */
    {
        uint32_t ops[] = { id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2)) goto oom;
    }

    /* 5. Decorations */
    { uint32_t ops[] = { id_tex,       LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_tex,       LAGFX_SPV_DECORATION_BINDING,        0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_samp,      LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_samp,      LAGFX_SPV_DECORATION_BINDING,        1u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_uv_in,     LAGFX_SPV_DECORATION_LOCATION,       0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_color_out, LAGFX_SPV_DECORATION_LOCATION,       0u }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_DECORATE, ops, 3)) goto oom; }

    /* 6. Types */
    { uint32_t ops[] = { id_void };       if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VOID, ops, 1)) goto oom; }
    { uint32_t ops[] = { id_float, 32 };  if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2)) goto oom; }
    { uint32_t ops[] = { id_v2f, id_float, 2 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_v4f, id_float, 4 }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3)) goto oom; }
    {
        /* OpTypeImage %id sampled_type Dim Depth Arrayed MS Sampled ImageFormat
         *
         * Sampled=1 means "used WITH a sampler" (separate sampler+image
         * binding model). Sampled=2 would be storage image. */
        uint32_t ops[] = {
            id_image_t,
            id_float,                          /* sampled_type */
            LAGFX_SPV_DIM_2D,                  /* Dim */
            0u,                                /* Depth = 0 (not a depth texture) */
            0u,                                /* Arrayed */
            0u,                                /* MS */
            1u,                                /* Sampled = 1 (with sampler) */
            LAGFX_SPV_IMAGE_FORMAT_UNKNOWN,    /* ImageFormat */
        };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_IMAGE, ops, 8)) goto oom;
    }
    {
        uint32_t ops[] = { id_sampler_t };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_SAMPLER, ops, 1)) goto oom;
    }
    {
        uint32_t ops[] = { id_sampimg_t, id_image_t };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_SAMPLED_IMAGE, ops, 2)) goto oom;
    }
    { uint32_t ops[] = { id_ptr_uc_img,  LAGFX_SPV_STORAGE_UNIFORM_CONSTANT, id_image_t   }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_uc_samp, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT, id_sampler_t }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_in_v2f,  LAGFX_SPV_STORAGE_INPUT,            id_v2f       }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out_v4f, LAGFX_SPV_STORAGE_OUTPUT,           id_v4f       }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_fn_void, id_void }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2)) goto oom; }

    /* 7. Variables */
    { uint32_t ops[] = { id_ptr_uc_img,  id_tex,       LAGFX_SPV_STORAGE_UNIFORM_CONSTANT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_uc_samp, id_samp,      LAGFX_SPV_STORAGE_UNIFORM_CONSTANT }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_in_v2f,  id_uv_in,     LAGFX_SPV_STORAGE_INPUT            }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }
    { uint32_t ops[] = { id_ptr_out_v4f, id_color_out, LAGFX_SPV_STORAGE_OUTPUT           }; if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_VARIABLE, ops, 3)) goto oom; }

    /* 8. Function body */
    {
        uint32_t ops[] = { id_void, id_main, LAGFX_SPV_FUNCTION_CONTROL_NONE, id_fn_void };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_FUNCTION, ops, 4)) goto oom;
    }
    {
        uint32_t ops[] = { id_entry };
        if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LABEL, ops, 1)) goto oom;
    }
    /* %uv = OpLoad %v2f %uv_in */
    { uint32_t ops[] = { id_v2f, id_uv_val, id_uv_in };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }
    /* %img_val = OpLoad %image %tex */
    { uint32_t ops[] = { id_image_t, id_img_val, id_tex };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }
    /* %samp_val = OpLoad %sampler %samp */
    { uint32_t ops[] = { id_sampler_t, id_samp_val, id_samp };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_LOAD, ops, 3)) goto oom; }
    /* %sampled = OpSampledImage %sampled_image %img_val %samp_val */
    { uint32_t ops[] = { id_sampimg_t, id_sampled, id_img_val, id_samp_val };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_SAMPLED_IMAGE, ops, 4)) goto oom; }
    /* %rgba = OpImageSampleImplicitLod %v4f %sampled %uv */
    { uint32_t ops[] = { id_v4f, id_rgba, id_sampled, id_uv_val };
      if (!lagfx_spv_builder_emit_op(b, LAGFX_SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, ops, 4)) goto oom; }
    /* OpStore %color_out %rgba */
    { uint32_t ops[] = { id_color_out, id_rgba };
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
