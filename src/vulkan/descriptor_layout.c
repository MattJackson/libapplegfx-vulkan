/* SPDX-License-Identifier: MIT */
/*
 * libapplegfx-vulkan — descriptor-set + pipeline layout from SPIR-V reflection
 * src/vulkan/descriptor_layout.c
 *
 * Copyright © 2026 Matthew Jackson
 */
#include "descriptor_layout.h"

#ifdef LAGFX_HAVE_VULKAN

#include "air2spv/spv_reflect.h"
#include "common/log.h"

#define LAGFX_MAX_DESC_BINDINGS 32u

static VkDescriptorType kind_to_vk(lagfx_spv_binding_kind_t k) {
    switch (k) {
        case LAGFX_SPV_BINDING_SAMPLED_IMAGE:  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case LAGFX_SPV_BINDING_SAMPLER:        return VK_DESCRIPTOR_TYPE_SAMPLER;
        case LAGFX_SPV_BINDING_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        default:                               return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

lagfx_status_t
lagfx_build_pipeline_layout_from_spv(VkDevice device,
                                     const uint8_t *const *spv_blobs,
                                     const size_t *spv_lens,
                                     uint32_t n_stages,
                                     VkDescriptorSetLayout *out_dsl,
                                     VkPipelineLayout *out_pl) {
    if (!device || !spv_blobs || !spv_lens || !out_dsl || !out_pl)
        return LAGFX_ERR_INVALID_ARG;
    *out_dsl = VK_NULL_HANDLE;
    *out_pl = VK_NULL_HANDLE;

    /* Accumulate the union of bindings across stages, deduped by binding
     * number (our translator only uses descriptor set 0). */
    VkDescriptorSetLayoutBinding binds[LAGFX_MAX_DESC_BINDINGS];
    uint32_t n_binds = 0;

    for (uint32_t s = 0; s < n_stages; s++) {
        if (!spv_blobs[s] || spv_lens[s] == 0) continue;
        lagfx_spv_binding_t refl[LAGFX_MAX_DESC_BINDINGS];
        size_t n = lagfx_spv_reflect_bindings(spv_blobs[s], spv_lens[s],
                                              refl, LAGFX_MAX_DESC_BINDINGS);
        if (n > LAGFX_MAX_DESC_BINDINGS) n = LAGFX_MAX_DESC_BINDINGS;
        for (size_t i = 0; i < n; i++) {
            if (refl[i].set != 0u) {
                /* Multi-set shaders aren't emitted by our translator yet;
                 * skip so we don't silently build a wrong layout. */
                LAGFX_WARN("descriptor_layout: skipping binding in set %u "
                           "(only set 0 supported)", refl[i].set);
                continue;
            }
            VkDescriptorType vt = kind_to_vk(refl[i].kind);
            if (vt == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
            /* Dedup: if this binding number already present, keep the first
             * (a binding has one type; vertex+fragment sharing it agree). */
            bool dup = false;
            for (uint32_t b = 0; b < n_binds; b++) {
                if (binds[b].binding == refl[i].binding) { dup = true; break; }
            }
            if (dup) continue;
            if (n_binds >= LAGFX_MAX_DESC_BINDINGS) break;
            binds[n_binds++] = (VkDescriptorSetLayoutBinding){
                .binding = refl[i].binding,
                .descriptorType = vt,
                .descriptorCount = 1u,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = NULL,
            };
        }
    }

    if (n_binds == 0u) {
        /* No descriptors — caller uses the device empty layout. */
        return LAGFX_OK;
    }

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = n_binds,
        .pBindings = binds,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl) != VK_SUCCESS) {
        LAGFX_ERR("descriptor_layout: vkCreateDescriptorSetLayout failed (%u bindings)",
                  n_binds);
        return LAGFX_ERR_PROTOCOL;
    }

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1u,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &plci, NULL, &pl) != VK_SUCCESS) {
        LAGFX_ERR("descriptor_layout: vkCreatePipelineLayout failed");
        vkDestroyDescriptorSetLayout(device, dsl, NULL);
        return LAGFX_ERR_PROTOCOL;
    }

    *out_dsl = dsl;
    *out_pl = pl;
    LAGFX_LOG("descriptor_layout: built set-0 layout with %u binding(s)", n_binds);
    return LAGFX_OK;
}

#endif /* LAGFX_HAVE_VULKAN */
