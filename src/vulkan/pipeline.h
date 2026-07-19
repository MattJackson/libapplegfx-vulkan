/*
 * libapplegfx-vulkan — passthrough pipeline + device-level Vulkan resources
 * src/vulkan/pipeline.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Owns the creation of a built-in "passthrough" graphics pipeline
 * (VkPipeline + VkPipelineLayout) at device init time, plus a
 * default frame image and dummy vertex buffer. These resources allow
 * the render encoder to issue valid Vulkan draw calls even when no
 * guest pipeline / vertex buffer / attachment is explicitly bound.
 *
 * The passthrough pipeline uses the clear.vert / clear.frag SPIR-V
 * pair from the shader catalog: vertex emits a fullscreen triangle,
 * fragment outputs a solid color from push-descriptor uniform.
 */

#ifndef LIBAPPLEGFX_VULKAN_PIPELINE_H
#define LIBAPPLEGFX_VULKAN_PIPELINE_H

#include "instance.h"

#include "libapplegfx-vulkan.h"

#ifdef LAGFX_HAVE_VULKAN

/* Create the passthrough pipeline + pipeline layout + default frame
 * image + dummy vertex buffer. Called from lagfx_vk_init after the
 * command pool is live. On failure returns non-OK; caller unwinds. */
lagfx_status_t lagfx_vk_pipeline_init(struct lagfx_vk_state *vk);

/* Destroy pipeline, layout, frame image, dummy VB. Called from
 * lagfx_vk_shutdown before command pool destroy. */
void lagfx_vk_pipeline_shutdown(struct lagfx_vk_state *vk);

#endif /* LAGFX_HAVE_VULKAN */

#endif /* LIBAPPLEGFX_VULKAN_PIPELINE_H */
