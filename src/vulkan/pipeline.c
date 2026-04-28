/*
 * libapplegfx-vulkan — passthrough pipeline + device-level Vulkan resources
 * src/vulkan/pipeline.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Creates a built-in "passthrough" graphics pipeline from the clear
 * shader pair's SPIR-V (fullscreen triangle vertex + solid-color
 * fragment). Also creates:
 *
 *   - VkPipelineLayout with one push-descriptor set for a UBO at
 *     binding 0 (matches clear.frag's layout).
 *   - Default 1920x1080 BGRA8 VkImage + VkImageView for use as a
 *     color attachment when no target image is provided.
 *   - A small (256-byte) dummy VkBuffer bound as vertex buffer when
 *     no VB is set by the guest.
 *
 * The pipeline is created with all dynamic state the encoder already
 * records (viewport, scissor, blend constants, stencil ref, depth
 * bias) so it doesn't need per-draw VkPipeline variants.
 */

#include "pipeline.h"
#include "command.h"
#include "common/log.h"
#include "shaders/catalog.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Memory type helper (duplicated from render_target.c to keep
 * this TU self-contained) ----------------------------------------- */

static uint32_t find_memory_type(VkPhysicalDevice phys,
                                 uint32_t typeBits,
                                 VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) {
            continue;
        }
        if ((mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* --- Pipeline layout creation ------------------------------------ */

static lagfx_status_t create_pipeline_layout(struct lagfx_vk_state *vk) {
    VkDescriptorSetLayoutBinding ubo_binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &ubo_binding,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkResult vr = vkCreateDescriptorSetLayout(vk->device, &dslci, NULL, &dsl);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateDescriptorSetLayout failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = 16,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pcr,
    };
    vr = vkCreatePipelineLayout(vk->device, &plci, NULL,
                                &vk->passthrough_layout);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreatePipelineLayout failed (%d)",
                  (int)vr);
        vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
        return LAGFX_ERR_BACKEND;
    }

    vk->passthrough_dsl = dsl;
    return LAGFX_OK;
}

/* --- Pipeline creation from clear shader pair -------------------- */

static lagfx_status_t create_passthrough_pipeline(struct lagfx_vk_state *vk) {
    const lagfx_shader_blob_t *vert_blob =
        lagfx_shader_catalog_lookup_stage(LAGFX_SHADER_CLEAR,
                                          LAGFX_SHADER_STAGE_VERTEX);
    const lagfx_shader_blob_t *frag_blob =
        lagfx_shader_catalog_lookup_stage(LAGFX_SHADER_CLEAR,
                                          LAGFX_SHADER_STAGE_FRAGMENT);
    if (!vert_blob || !frag_blob) {
        LAGFX_ERR("pipeline_init: clear shader blobs not found in catalog");
        return LAGFX_ERR_BACKEND;
    }

    VkShaderModule vert_mod = VK_NULL_HANDLE;
    VkShaderModule frag_mod = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo vmci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_blob->spirv_len,
        .pCode    = (const uint32_t *)vert_blob->spirv_bytes,
    };
    VkResult vr = vkCreateShaderModule(vk->device, &vmci, NULL, &vert_mod);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateShaderModule(vert) failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkShaderModuleCreateInfo fmci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_blob->spirv_len,
        .pCode    = (const uint32_t *)frag_blob->spirv_bytes,
    };
    vr = vkCreateShaderModule(vk->device, &fmci, NULL, &frag_mod);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateShaderModule(frag) failed (%d)",
                  (int)vr);
        vkDestroyShaderModule(vk->device, vert_mod, NULL);
        return LAGFX_ERR_BACKEND;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_mod,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_mod,
            .pName  = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .cullMode                = VK_CULL_MODE_NONE,
        .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable         = VK_TRUE,
        .lineWidth               = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable   = VK_FALSE,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    VkPipelineColorBlendAttachmentState color_blend_att = {
        .blendEnable         = VK_FALSE,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT
                             | VK_COLOR_COMPONENT_G_BIT
                             | VK_COLOR_COMPONENT_B_BIT
                             | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments    = &color_blend_att,
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
        .pDynamicStates    = dynamic_states,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState   = &multisample,
        .pDepthStencilState  = &depth_stencil,
        .pColorBlendState    = &color_blend,
        .pDynamicState       = &dynamic_state,
        .layout              = vk->passthrough_layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    vr = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &gpci,
                                   NULL, &vk->passthrough_pipeline);
    vkDestroyShaderModule(vk->device, vert_mod, NULL);
    vkDestroyShaderModule(vk->device, frag_mod, NULL);

    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateGraphicsPipelines failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    LAGFX_LOG("pipeline_init: passthrough pipeline created (%p) layout=%p",
              (void *)vk->passthrough_pipeline,
              (void *)vk->passthrough_layout);
    return LAGFX_OK;
}

/* --- Default frame image ----------------------------------------- */

static lagfx_status_t create_frame_image(struct lagfx_vk_state *vk) {
    const uint32_t w = 1920u;
    const uint32_t h = 1080u;
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { w, h, 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = vkCreateImage(vk->device, &ici, NULL, &vk->frame_image);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateImage(frame) failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, vk->frame_image, &req);
    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("pipeline_init: no memory type for frame image");
        vkDestroyImage(vk->device, vk->frame_image, NULL);
        vk->frame_image = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &vk->frame_image_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkAllocateMemory(frame) failed (%d)",
                  (int)vr);
        vkDestroyImage(vk->device, vk->frame_image, NULL);
        vk->frame_image = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindImageMemory(vk->device, vk->frame_image, vk->frame_image_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkBindImageMemory(frame) failed (%d)",
                  (int)vr);
        vkFreeMemory(vk->device, vk->frame_image_mem, NULL);
        vkDestroyImage(vk->device, vk->frame_image, NULL);
        vk->frame_image     = VK_NULL_HANDLE;
        vk->frame_image_mem = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = vk->frame_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vr = vkCreateImageView(vk->device, &vci, NULL, &vk->frame_image_view);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateImageView(frame) failed (%d)",
                  (int)vr);
        vkFreeMemory(vk->device, vk->frame_image_mem, NULL);
        vkDestroyImage(vk->device, vk->frame_image, NULL);
        vk->frame_image      = VK_NULL_HANDLE;
        vk->frame_image_mem  = VK_NULL_HANDLE;
        vk->frame_image_view = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    vk->frame_image_w     = w;
    vk->frame_image_h     = h;
    vk->frame_image_fmt   = fmt;
    vk->frame_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    LAGFX_LOG("pipeline_init: default frame image %ux%u image=%p view=%p",
              w, h,
              (void *)vk->frame_image, (void *)vk->frame_image_view);
    return LAGFX_OK;
}

static void destroy_frame_image(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->frame_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, vk->frame_image_view, NULL);
        vk->frame_image_view = VK_NULL_HANDLE;
    }
    if (vk->frame_image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, vk->frame_image, NULL);
        vk->frame_image = VK_NULL_HANDLE;
    }
    if (vk->frame_image_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->frame_image_mem, NULL);
        vk->frame_image_mem = VK_NULL_HANDLE;
    }
}

/* --- Dummy vertex buffer ----------------------------------------- */

static lagfx_status_t create_dummy_vb(struct lagfx_vk_state *vk) {
    const VkDeviceSize size = 256u;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &vk->dummy_vb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateBuffer(dummy_vb) failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, vk->dummy_vb, &req);
    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("pipeline_init: no memory type for dummy VB");
        vkDestroyBuffer(vk->device, vk->dummy_vb, NULL);
        vk->dummy_vb = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &vk->dummy_vb_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkAllocateMemory(dummy_vb) failed (%d)",
                  (int)vr);
        vkDestroyBuffer(vk->device, vk->dummy_vb, NULL);
        vk->dummy_vb = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindBufferMemory(vk->device, vk->dummy_vb, vk->dummy_vb_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkBindBufferMemory(dummy_vb) failed (%d)",
                  (int)vr);
        vkFreeMemory(vk->device, vk->dummy_vb_mem, NULL);
        vkDestroyBuffer(vk->device, vk->dummy_vb, NULL);
        vk->dummy_vb     = VK_NULL_HANDLE;
        vk->dummy_vb_mem = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    LAGFX_LOG("pipeline_init: dummy VB created (%p, 256 bytes)",
              (void *)vk->dummy_vb);
    return LAGFX_OK;
}

static void destroy_dummy_vb(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->dummy_vb != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, vk->dummy_vb, NULL);
        vk->dummy_vb = VK_NULL_HANDLE;
    }
    if (vk->dummy_vb_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->dummy_vb_mem, NULL);
        vk->dummy_vb_mem = VK_NULL_HANDLE;
    }
}

/* --- Fallback descriptor set (UBO with white vec4) -------------- */

static lagfx_status_t create_fallback_ds(struct lagfx_vk_state *vk) {
    const VkDeviceSize ubo_size = 64u;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = ubo_size,
        .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &vk->fallback_ubo);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateBuffer(fallback_ubo) failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, vk->fallback_ubo, &req);
    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("pipeline_init: no memory type for fallback UBO");
        vkDestroyBuffer(vk->device, vk->fallback_ubo, NULL);
        vk->fallback_ubo = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &vk->fallback_ubo_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkAllocateMemory(fallback_ubo) failed (%d)",
                  (int)vr);
        vkDestroyBuffer(vk->device, vk->fallback_ubo, NULL);
        vk->fallback_ubo = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindBufferMemory(vk->device, vk->fallback_ubo,
                            vk->fallback_ubo_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkBindBufferMemory(fallback_ubo) failed (%d)",
                  (int)vr);
        vkFreeMemory(vk->device, vk->fallback_ubo_mem, NULL);
        vkDestroyBuffer(vk->device, vk->fallback_ubo, NULL);
        vk->fallback_ubo     = VK_NULL_HANDLE;
        vk->fallback_ubo_mem = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    {
        void *mapped = NULL;
        vr = vkMapMemory(vk->device, vk->fallback_ubo_mem, 0, ubo_size, 0,
                         &mapped);
        if (vr == VK_SUCCESS && mapped) {
            float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            memcpy(mapped, white, sizeof(white));
            vkUnmapMemory(vk->device, vk->fallback_ubo_mem);
        }
    }

    VkDescriptorPoolSize pool_size = {
        .type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_size,
    };
    vr = vkCreateDescriptorPool(vk->device, &dpci, NULL,
                                &vk->fallback_desc_pool);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkCreateDescriptorPool failed (%d)",
                  (int)vr);
        vkFreeMemory(vk->device, vk->fallback_ubo_mem, NULL);
        vkDestroyBuffer(vk->device, vk->fallback_ubo, NULL);
        vk->fallback_ubo     = VK_NULL_HANDLE;
        vk->fallback_ubo_mem = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorSetAllocateInfo dsai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = vk->fallback_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &vk->passthrough_dsl,
    };
    vr = vkAllocateDescriptorSets(vk->device, &dsai, &vk->fallback_desc_set);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("pipeline_init: vkAllocateDescriptorSets failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorBufferInfo dbi = {
        .buffer = vk->fallback_ubo,
        .offset = 0,
        .range  = 16,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = vk->fallback_desc_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo     = &dbi,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

    LAGFX_LOG("pipeline_init: fallback descriptor set created "
              "(set=%p ubo=%p, white fill)", (void *)vk->fallback_desc_set,
              (void *)vk->fallback_ubo);
    return LAGFX_OK;
}

static void destroy_fallback_ds(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->fallback_desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk->device, vk->fallback_desc_pool, NULL);
        vk->fallback_desc_pool = VK_NULL_HANDLE;
        vk->fallback_desc_set  = VK_NULL_HANDLE;
    }
    if (vk->fallback_ubo != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, vk->fallback_ubo, NULL);
        vk->fallback_ubo = VK_NULL_HANDLE;
    }
    if (vk->fallback_ubo_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->fallback_ubo_mem, NULL);
        vk->fallback_ubo_mem = VK_NULL_HANDLE;
    }
}

/* --- Cursor pipeline layout ------------------------------------- */

static lagfx_status_t create_cursor_pipeline_layout(struct lagfx_vk_state *vk) {
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT
                             | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings    = bindings,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkResult vr = vkCreateDescriptorSetLayout(vk->device, &dslci, NULL, &dsl);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_layout: vkCreateDescriptorSetLayout failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkPipelineLayoutCreateInfo plci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &dsl,
    };
    vr = vkCreatePipelineLayout(vk->device, &plci, NULL, &vk->cursor_layout);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_layout: vkCreatePipelineLayout failed (%d)",
                  (int)vr);
        vkDestroyDescriptorSetLayout(vk->device, dsl, NULL);
        return LAGFX_ERR_BACKEND;
    }

    vk->cursor_dsl = dsl;
    return LAGFX_OK;
}

/* --- Cursor pipeline from cursor.vert + cursor.frag -------------- */

static lagfx_status_t create_cursor_pipeline(struct lagfx_vk_state *vk) {
    const lagfx_shader_blob_t *vert_blob =
        lagfx_shader_catalog_lookup_stage(LAGFX_SHADER_CURSOR,
                                          LAGFX_SHADER_STAGE_VERTEX);
    const lagfx_shader_blob_t *frag_blob =
        lagfx_shader_catalog_lookup_stage(LAGFX_SHADER_CURSOR,
                                          LAGFX_SHADER_STAGE_FRAGMENT);
    if (!vert_blob || !frag_blob) {
        LAGFX_ERR("cursor_pipeline: shader blobs not found in catalog");
        return LAGFX_ERR_BACKEND;
    }

    VkShaderModule vert_mod = VK_NULL_HANDLE;
    VkShaderModule frag_mod = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo vmci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_blob->spirv_len,
        .pCode    = (const uint32_t *)vert_blob->spirv_bytes,
    };
    VkResult vr = vkCreateShaderModule(vk->device, &vmci, NULL, &vert_mod);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_pipeline: vkCreateShaderModule(vert) failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkShaderModuleCreateInfo fmci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_blob->spirv_len,
        .pCode    = (const uint32_t *)frag_blob->spirv_bytes,
    };
    vr = vkCreateShaderModule(vk->device, &fmci, NULL, &frag_mod);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_pipeline: vkCreateShaderModule(frag) failed (%d)",
                  (int)vr);
        vkDestroyShaderModule(vk->device, vert_mod, NULL);
        return LAGFX_ERR_BACKEND;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_mod,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_mod,
            .pName  = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode             = VK_POLYGON_MODE_FILL,
        .cullMode                = VK_CULL_MODE_NONE,
        .frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable         = VK_FALSE,
        .lineWidth               = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable   = VK_FALSE,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    VkPipelineColorBlendAttachmentState color_blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT
                             | VK_COLOR_COMPONENT_G_BIT
                             | VK_COLOR_COMPONENT_B_BIT
                             | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable   = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments    = &color_blend_att,
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
        .pDynamicStates    = dynamic_states,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState   = &multisample,
        .pDepthStencilState  = &depth_stencil,
        .pColorBlendState    = &color_blend,
        .pDynamicState       = &dynamic_state,
        .layout              = vk->cursor_layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    vr = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &gpci,
                                    NULL, &vk->cursor_pipeline);
    vkDestroyShaderModule(vk->device, vert_mod, NULL);
    vkDestroyShaderModule(vk->device, frag_mod, NULL);

    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_pipeline: vkCreateGraphicsPipelines failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    LAGFX_LOG("cursor_pipeline: created (%p) layout=%p",
              (void *)vk->cursor_pipeline, (void *)vk->cursor_layout);
    return LAGFX_OK;
}

/* --- Cursor resources: UBO, descriptor set, glyph texture, sampler */

static lagfx_status_t create_cursor_resources(struct lagfx_vk_state *vk) {
    const VkDeviceSize ubo_size = 16u;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = ubo_size,
        .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &vk->cursor_ubo);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkCreateBuffer(ubo) failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, vk->cursor_ubo, &req);
    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("cursor_res: no memory type for cursor UBO");
        vkDestroyBuffer(vk->device, vk->cursor_ubo, NULL);
        vk->cursor_ubo = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &vk->cursor_ubo_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkAllocateMemory(ubo) failed (%d)", (int)vr);
        vkDestroyBuffer(vk->device, vk->cursor_ubo, NULL);
        vk->cursor_ubo = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindBufferMemory(vk->device, vk->cursor_ubo, vk->cursor_ubo_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkBindBufferMemory(ubo) failed (%d)", (int)vr);
        vkFreeMemory(vk->device, vk->cursor_ubo_mem, NULL);
        vkDestroyBuffer(vk->device, vk->cursor_ubo, NULL);
        vk->cursor_ubo     = VK_NULL_HANDLE;
        vk->cursor_ubo_mem = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 2,
        .pPoolSizes    = pool_sizes,
    };
    vr = vkCreateDescriptorPool(vk->device, &dpci, NULL,
                                &vk->cursor_desc_pool);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkCreateDescriptorPool failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorSetAllocateInfo dsai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = vk->cursor_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &vk->cursor_dsl,
    };
    vr = vkAllocateDescriptorSets(vk->device, &dsai, &vk->cursor_desc_set);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkAllocateDescriptorSets failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorBufferInfo dbi = {
        .buffer = vk->cursor_ubo,
        .offset = 0,
        .range  = 16,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = vk->cursor_desc_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo     = &dbi,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

    VkSamplerCreateInfo sci = {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter               = VK_FILTER_LINEAR,
        .minFilter               = VK_FILTER_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable        = VK_FALSE,
        .compareEnable           = VK_FALSE,
        .borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    vr = vkCreateSampler(vk->device, &sci, NULL, &vk->cursor_sampler);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkCreateSampler failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    const uint32_t glyph_w = 64u;
    const uint32_t glyph_h = 64u;
    const VkFormat glyph_fmt = VK_FORMAT_B8G8R8A8_UNORM;

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = glyph_fmt,
        .extent        = { glyph_w, glyph_h, 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_LINEAR,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    vr = vkCreateImage(vk->device, &ici, NULL, &vk->cursor_glyph_image);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkCreateImage(glyph) failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements greq;
    vkGetImageMemoryRequirements(vk->device, vk->cursor_glyph_image, &greq);
    uint32_t gmtype = find_memory_type(vk->phys_device, greq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (gmtype == UINT32_MAX) {
        gmtype = find_memory_type(vk->phys_device, greq.memoryTypeBits, 0u);
    }
    if (gmtype == UINT32_MAX) {
        LAGFX_ERR("cursor_res: no memory type for glyph image");
        vkDestroyImage(vk->device, vk->cursor_glyph_image, NULL);
        vk->cursor_glyph_image = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo gmai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = greq.size,
        .memoryTypeIndex = gmtype,
    };
    vr = vkAllocateMemory(vk->device, &gmai, NULL, &vk->cursor_glyph_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkAllocateMemory(glyph) failed (%d)", (int)vr);
        vkDestroyImage(vk->device, vk->cursor_glyph_image, NULL);
        vk->cursor_glyph_image = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindImageMemory(vk->device, vk->cursor_glyph_image,
                           vk->cursor_glyph_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkBindImageMemory(glyph) failed (%d)",
                   (int)vr);
        vkFreeMemory(vk->device, vk->cursor_glyph_mem, NULL);
        vkDestroyImage(vk->device, vk->cursor_glyph_image, NULL);
        vk->cursor_glyph_image = VK_NULL_HANDLE;
        vk->cursor_glyph_mem   = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = vk->cursor_glyph_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = glyph_fmt,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vr = vkCreateImageView(vk->device, &vci, NULL, &vk->cursor_glyph_view);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_res: vkCreateImageView(glyph) failed (%d)", (int)vr);
        vkFreeMemory(vk->device, vk->cursor_glyph_mem, NULL);
        vkDestroyImage(vk->device, vk->cursor_glyph_image, NULL);
        vk->cursor_glyph_image = VK_NULL_HANDLE;
        vk->cursor_glyph_mem   = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    vk->cursor_glyph_w     = glyph_w;
    vk->cursor_glyph_h     = glyph_h;
    vk->cursor_glyph_valid = false;

    LAGFX_LOG("cursor_res: UBO=%p desc_set=%p sampler=%p glyph=%ux%u image=%p",
              (void *)vk->cursor_ubo, (void *)vk->cursor_desc_set,
              (void *)vk->cursor_sampler, glyph_w, glyph_h,
              (void *)vk->cursor_glyph_image);
    return LAGFX_OK;
}

static void destroy_cursor_resources(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (vk->cursor_glyph_view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, vk->cursor_glyph_view, NULL);
        vk->cursor_glyph_view = VK_NULL_HANDLE;
    }
    if (vk->cursor_glyph_image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, vk->cursor_glyph_image, NULL);
        vk->cursor_glyph_image = VK_NULL_HANDLE;
    }
    if (vk->cursor_glyph_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->cursor_glyph_mem, NULL);
        vk->cursor_glyph_mem = VK_NULL_HANDLE;
    }
    if (vk->cursor_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk->device, vk->cursor_sampler, NULL);
        vk->cursor_sampler = VK_NULL_HANDLE;
    }
    if (vk->cursor_desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk->device, vk->cursor_desc_pool, NULL);
        vk->cursor_desc_pool = VK_NULL_HANDLE;
        vk->cursor_desc_set  = VK_NULL_HANDLE;
    }
    if (vk->cursor_ubo != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, vk->cursor_ubo, NULL);
        vk->cursor_ubo = VK_NULL_HANDLE;
    }
    if (vk->cursor_ubo_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, vk->cursor_ubo_mem, NULL);
        vk->cursor_ubo_mem = VK_NULL_HANDLE;
    }
    if (vk->cursor_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->cursor_pipeline, NULL);
        vk->cursor_pipeline = VK_NULL_HANDLE;
    }
    if (vk->cursor_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->cursor_layout, NULL);
        vk->cursor_layout = VK_NULL_HANDLE;
    }
    if (vk->cursor_dsl != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk->device, vk->cursor_dsl, NULL);
        vk->cursor_dsl = VK_NULL_HANDLE;
    }
}

/* --- Public entry points ---------------------------------------- */

lagfx_status_t lagfx_vk_pipeline_init(struct lagfx_vk_state *vk) {
    if (!vk || !vk->initialized) {
        return LAGFX_ERR_INVALID_ARG;
    }

    lagfx_status_t st = create_pipeline_layout(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_passthrough_pipeline(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_frame_image(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_dummy_vb(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_fallback_ds(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_cursor_pipeline_layout(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_cursor_pipeline(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    st = create_cursor_resources(vk);
    if (st != LAGFX_OK) {
        return st;
    }

    LAGFX_LOG("pipeline_init: all resources created");
    return LAGFX_OK;
}

void lagfx_vk_pipeline_shutdown(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return;
    }
    destroy_cursor_resources(vk);
    destroy_fallback_ds(vk);
    destroy_dummy_vb(vk);
    destroy_frame_image(vk);
    if (vk->passthrough_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, vk->passthrough_pipeline, NULL);
        vk->passthrough_pipeline = VK_NULL_HANDLE;
    }
    if (vk->passthrough_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, vk->passthrough_layout, NULL);
        vk->passthrough_layout = VK_NULL_HANDLE;
    }
    if (vk->passthrough_dsl != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk->device, vk->passthrough_dsl, NULL);
        vk->passthrough_dsl = VK_NULL_HANDLE;
    }
}

#endif /* LAGFX_HAVE_VULKAN */
