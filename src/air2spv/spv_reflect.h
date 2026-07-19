/* SPDX-License-Identifier: MIT */
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
#include <stdbool.h>

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

/* True if the SPIR-V contains any image sample/fetch/read instruction (the
 * shader USES a texture). If true but reflection found no SAMPLED_IMAGE
 * binding, the module is INCONSISTENT (translator dropped the texture
 * binding) → must not be drawn or it samples an unbound descriptor → crash. */
bool lagfx_spv_has_image_sample(const uint8_t *spv, size_t spv_len);

/* In-place ADD `base` to every `OpDecorate <id> Binding <n>` literal in the
 * SPIR-V module (DescriptorSet decorations are left untouched). Used to give
 * the FRAGMENT stage a disjoint binding range from the VERTEX stage: the
 * translator emits both stages' resources in descriptor set 0 starting at
 * binding 0, so vertex `[[buffer(0)]]` and fragment `[[texture(0)]]` collide
 * at set0/binding0 — and Vulkan cannot hold two descriptorTypes at one
 * (set,binding), so the merged pipeline layout drops one and the shader
 * samples/loads an unbound descriptor. Offsetting the fragment stage by a
 * fixed base (e.g. 16) makes the merged set-0 layout unambiguous: bindings
 * < base are vertex resources, >= base are fragment resources. The draw site
 * demuxes on the same base. Mutates `spv` in place; safe to call once per
 * fragment module before vkCreateShaderModule + reflection. */
void lagfx_spv_offset_bindings(uint8_t *spv, size_t spv_len, uint32_t base);

#ifdef __cplusplus
}
#endif

#endif /* LAGFX_AIR2SPV_SPV_REFLECT_H */
