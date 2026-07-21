/* SPDX-License-Identifier: MIT */
#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H

#include "libapplegfx-vulkan.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#else
/* Vulkan-disabled stub build: declare opaque handles so this header
 * still parses. Callers under LAGFX_HAVE_VULKAN see real Vulkan types. */
typedef void *VkDevice;
typedef void *VkShaderModule;
typedef void *VkPipeline;
typedef void *VkPipelineLayout;
typedef int   VkFormat;
#endif

/**
 * MVP pipeline descriptor — fields hardcoded to match triangle.metallib
 * for first-pixel work. Will be extended in Stage 70b+ as wire-opcode
 * handlers populate more fields.
 */
typedef struct {
    VkShaderModule   vertex_shader;            /* required */
    VkShaderModule   fragment_shader;          /* required */
    VkPipelineLayout layout;                   /* required — Vulkan spec forbids VK_NULL_HANDLE */
    const char      *vertex_entry_point;       /* default: "main" if NULL */
    const char      *fragment_entry_point;     /* default: "main" if NULL */
    VkFormat         color_format;             /* default: VK_FORMAT_B8G8R8A8_UNORM */
    VkFormat         depth_format;             /* default: VK_FORMAT_UNDEFINED (no depth) */
    /* Vertex stage-in attributes (from lagfx_spv_reflect_vertex_inputs). When
     * n_vtx_inputs > 0, the pipeline gets a non-empty vertex-input state
     * (binding 0, tightly-packed R32..A32_SFLOAT) so the vertex shader's
     * stage-in reads come from the bound guest vertex buffer instead of an
     * unbound attribute (= 0 = degenerate positions = black). */
    uint8_t          vtx_in_loc[8];
    uint8_t          vtx_in_comp[8];            /* 1..4 components per attribute */
    uint8_t          n_vtx_inputs;
    /* Real per-vertex stride decoded from the PSO's MTLVertexDescriptor
     * (lagfx_parse_pso_vertex_stride). 0 = unknown → fall back to round8(sum).
     * The round8 heuristic is wrong for the CoreAnimation composites (real 48,
     * heuristic 24) and smears the geometry into horizontal bands. */
    uint32_t         vtx_stride;
    /* GOAL-M2z: real per-attribute {MTLVertexFormat, offset} from the PSO
     * descriptor (sorted by offset; n_pso_attrs 0 = not decoded -> tight-pack
     * SFLOAT fallback). Fixes the rgba8 color attr read as SFLOAT NaN. */
    uint32_t         pso_attr_fmt[8];
    uint32_t         pso_attr_off[8];
    uint8_t          n_pso_attrs;
    /* Future fields (Stage 70b+):
     *   cull_mode, front_face, blend factor, depth-stencil state... */
} lagfx_pipeline_desc_t;

/**
 * Build a VkGraphicsPipeline from the MVP descriptor. Mirrors the
 * exact pipeline-build sequence used by tests/triangle-lavapipe-e2e.c
 * but parameterised on the two shader modules and color/depth formats.
 *
 * Uses VK_KHR_dynamic_rendering when available (the e2e test does).
 *
 * @param device  VkDevice handle (must outlive the returned VkPipeline)
 * @param desc    populated descriptor; both shader modules required
 * @param out_pipeline  receives the new VkPipeline on success
 * @return LAGFX_OK on success; LAGFX_ERR_INVALID_ARG on NULL inputs;
 *         LAGFX_ERR_BACKEND on Vulkan failures.
 */
lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline);

#ifdef __cplusplus
}
#endif

#endif /* LIBAPPLEGFX_VULKAN_PIPELINE_BUILD_H */
