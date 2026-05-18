/* SPDX-License-Identifier: AGPL-3.0-or-later */
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
    VkShaderModule   vertex_shader;     /* required */
    VkShaderModule   fragment_shader;   /* required */
    VkPipelineLayout layout;            /* required — Vulkan spec forbids VK_NULL_HANDLE */
    VkFormat         color_format;      /* default: VK_FORMAT_B8G8R8A8_UNORM */
    VkFormat         depth_format;      /* default: VK_FORMAT_UNDEFINED (no depth) */
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
