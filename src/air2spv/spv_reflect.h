/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * libapplegfx-vulkan — SPIR-V descriptor-binding reflection
 * src/air2spv/spv_reflect.h
 *
 * Copyright © 2026 Matthew Jackson
 *
 * Scans a translated SPIR-V module for the descriptor bindings it declares
 * (set + binding + kind) so the host can build a matching
 * VkDescriptorSetLayout for the pipeline. The air2spv translator emits
 * texture/sampler resources as UniformConstant OpTypeImage / OpTypeSampler
 * and [[buffer(n)]] args as StorageBuffer Block structs, all in
 * DescriptorSet 0 with sequential bindings; this recovers that layout from
 * the finished blob without re-running translation. Pure (no Vulkan
 * dependency) so it unit-tests on any host.
 */
#ifndef LAGFX_AIR2SPV_SPV_REFLECT_H
#define LAGFX_AIR2SPV_SPV_REFLECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAGFX_SPV_BINDING_UNKNOWN        = 0,
    LAGFX_SPV_BINDING_SAMPLED_IMAGE  = 1, /* OpTypeImage, Sampled=1  -> VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE */
    LAGFX_SPV_BINDING_SAMPLER        = 2, /* OpTypeSampler           -> VK_DESCRIPTOR_TYPE_SAMPLER */
    LAGFX_SPV_BINDING_STORAGE_BUFFER = 3, /* StorageBuffer Block     -> VK_DESCRIPTOR_TYPE_STORAGE_BUFFER */
} lagfx_spv_binding_kind_t;

typedef struct {
    uint32_t                 set;
    uint32_t                 binding;
    lagfx_spv_binding_kind_t kind;
} lagfx_spv_binding_t;

/* Reflect descriptor bindings from a SPIR-V blob. Writes up to `cap`
 * entries to `out` (sorted by set then binding) and returns the TOTAL
 * number found (which may exceed `cap`; callers should size `cap` to the
 * device's max or check the return). 0 = no descriptor resources (e.g. a
 * resource-free colour shader -> an empty pipeline layout is correct). */
size_t lagfx_spv_reflect_bindings(const uint8_t *spv, size_t spv_len,
                                  lagfx_spv_binding_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_AIR2SPV_SPV_REFLECT_H */
