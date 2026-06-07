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

/* A vertex stage-in attribute: a Location-decorated Input variable in the
 * VERTEX shader (the translator emits one per float/vector arg that is not a
 * resource or a builtin). `components` is 1..4 (scalar/vec2/vec3/vec4 float)
 * → maps to VK_FORMAT_R32[G32[B32[A32]]]_SFLOAT. The host needs these to build
 * a non-empty VkPipelineVertexInputState + bind the guest's vertex buffer;
 * without them the vertex shader reads unbound attributes → 0 positions →
 * degenerate draws → black. */
typedef struct {
    uint32_t location;
    uint32_t components;  /* 1..4 */
} lagfx_spv_vertex_input_t;

/* Reflect vertex stage-in attributes (Location-decorated Input vars) from a
 * translated VERTEX SPIR-V blob. Writes up to `cap` entries (sorted by
 * location) and returns the total found. 0 = the vertex shader uses no
 * stage-in attributes (purely vertex_id / builtin-driven). */
size_t lagfx_spv_reflect_vertex_inputs(const uint8_t *spv, size_t spv_len,
                                       lagfx_spv_vertex_input_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_AIR2SPV_SPV_REFLECT_H */
