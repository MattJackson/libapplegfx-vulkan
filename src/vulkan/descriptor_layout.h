/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — descriptor-set + pipeline layout from SPIR-V reflection
 * src/vulkan/descriptor_layout.h
 *
 * Copyright © 2026 Matthew Jackson
 *
 * Builds a single-set (set 0) VkDescriptorSetLayout + VkPipelineLayout from
 * the descriptor bindings a translated SPIR-V shader stage declares (via
 * lagfx_spv_reflect_bindings). The production substitute path uses the
 * device's empty layout because the triangle SPVs declare zero descriptors;
 * a real translated SkyLight pipeline with [[buffer]]/texture args declares
 * DescriptorSet-0 bindings and needs a matching layout or
 * vkCreateGraphicsPipelines fails. This is the host side of that match.
 */
#ifndef LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H
#define LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H

#ifdef LAGFX_HAVE_VULKAN

#include "libapplegfx-vulkan.h"
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a set-0 descriptor-set layout + pipeline layout covering the union
 * of descriptor bindings declared across `n_stages` translated SPIR-V blobs
 * (typically the vertex + fragment of one pipeline). On success returns
 * LAGFX_OK and writes the handles to *out_dsl / *out_pl. If the shaders
 * declare ZERO descriptors, both outputs are VK_NULL_HANDLE and the return
 * is LAGFX_OK — the caller should fall back to the device's empty layout.
 *
 * Binding type mapping: SAMPLED_IMAGE→VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
 * SAMPLER→VK_DESCRIPTOR_TYPE_SAMPLER, STORAGE_BUFFER→
 * VK_DESCRIPTOR_TYPE_STORAGE_BUFFER. Stage flags are VERTEX|FRAGMENT (a
 * valid over-approximation — a binding unused by a stage is harmless).
 *
 * Caller owns destruction: vkDestroyPipelineLayout(*out_pl) then
 * vkDestroyDescriptorSetLayout(*out_dsl) when non-NULL. */
lagfx_status_t lagfx_build_pipeline_layout_from_spv(
    VkDevice device,
    const uint8_t *const *spv_blobs,
    const size_t *spv_lens,
    uint32_t n_stages,
    VkDescriptorSetLayout *out_dsl,
    VkPipelineLayout *out_pl);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_HAVE_VULKAN */
#endif /* LAGFX_VULKAN_DESCRIPTOR_LAYOUT_H */
