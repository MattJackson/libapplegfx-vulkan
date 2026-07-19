/*
 * libapplegfx-vulkan — layer compositor
 * src/vulkan/composite.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "composite.h"
#include "command.h"
#include "../shaders/catalog.h"
#include "common/log.h"

#ifdef LAGFX_HAVE_VULKAN

static VkShaderModule make_module(VkDevice dev, lagfx_shader_kind_t kind,
                                   lagfx_shader_stage_t stage) {
    const lagfx_shader_blob_t *b = lagfx_shader_catalog_lookup_stage(kind, stage);
    if (!b) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = b->spirv_len,
        .pCode    = (const uint32_t *)b->spirv_bytes,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

/* Lazily build the MAX-blend passthrough pipeline (blit.vert + blit.frag),
 * a repeat sampler, and a single reusable set-0/binding-0 combined-image
 * sampler descriptor. */
static bool composite_ensure(struct lagfx_vk_state *vk) {
    if (vk->composite_ready) return true;
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) return false;
    VkDevice dev = vk->device;

    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(dev, &sci, NULL, &vk->composite_sampler) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding dslb = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &dslb,
    };
    if (vkCreateDescriptorSetLayout(dev, &dslci, NULL, &vk->composite_dsl) != VK_SUCCESS)
        goto fail;

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &vk->composite_dsl,
    };
    if (vkCreatePipelineLayout(dev, &plci, NULL, &vk->composite_layout) != VK_SUCCESS)
        goto fail;

    VkDescriptorPoolSize ps = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
    };
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &vk->composite_pool) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vk->composite_pool,
        .descriptorSetCount = 1, .pSetLayouts = &vk->composite_dsl,
    };
    if (vkAllocateDescriptorSets(dev, &dsai, &vk->composite_set) != VK_SUCCESS)
        goto fail;

    VkShaderModule vmod = make_module(dev, LAGFX_SHADER_BLIT, LAGFX_SHADER_STAGE_VERTEX);
    VkShaderModule fmod = make_module(dev, LAGFX_SHADER_BLIT, LAGFX_SHADER_STAGE_FRAGMENT);
    if (vmod == VK_NULL_HANDLE || fmod == VK_NULL_HANDLE) {
        if (vmod) vkDestroyShaderModule(dev, vmod, NULL);
        if (fmod) vkDestroyShaderModule(dev, fmod, NULL);
        goto fail;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vmod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* MAX blend: out = max(src, dst) — bright layer texels win, dark ones keep
     * the background. Alpha-independent (per-pass surfaces lack reliable alpha). */
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_MAX,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_MAX,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo prc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_fmt };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prc, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = vk->composite_layout,
    };
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL,
                                            &vk->composite_pipeline);
    vkDestroyShaderModule(dev, vmod, NULL);
    vkDestroyShaderModule(dev, fmod, NULL);
    if (pr != VK_SUCCESS) goto fail;

    vk->composite_ready = true;
    LAGFX_LOG("composite: MAX-blend layer pipeline built");
    return true;
fail:
    lagfx_vk_composite_shutdown(vk);
    return false;
}

lagfx_status_t lagfx_vk_composite_over(struct lagfx_vk_state *vk,
                                        lagfx_vk_render_target_t *display_rt,
                                        VkImageView src_view) {
    if (!vk || !display_rt || src_view == VK_NULL_HANDLE) return LAGFX_ERR_INVALID_ARG;
    if (!composite_ensure(vk)) return LAGFX_ERR_BACKEND;
    VkDevice dev = vk->device;

    VkDescriptorImageInfo dii = {
        .sampler = vk->composite_sampler, .imageView = src_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = vk->composite_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (lagfx_vk_cmdbuf_alloc(vk, &cb) != LAGFX_OK) return LAGFX_ERR_BACKEND;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        lagfx_vk_cmdbuf_free(vk, cb); return LAGFX_ERR_BACKEND;
    }

    VkImageMemoryBarrier bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = (display_rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                         ? VK_ACCESS_TRANSFER_READ_BIT : 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = display_rt->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = display_rt->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &bar);
    display_rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = display_rt->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { display_rt->width, display_rt->height } },
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att };
    vkCmdBeginRendering(cb, &ri);
    VkViewport vpp = { 0.0f, 0.0f, (float)display_rt->width, (float)display_rt->height,
                       0.0f, 1.0f };
    vkCmdSetViewport(cb, 0, 1, &vpp);
    VkRect2D sc = { { 0, 0 }, { display_rt->width, display_rt->height } };
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->composite_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk->composite_layout, 0, 1, &vk->composite_set, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { lagfx_vk_cmdbuf_free(vk, cb); return LAGFX_ERR_BACKEND; }
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS) { lagfx_vk_cmdbuf_free(vk, cb); return LAGFX_ERR_BACKEND; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    VkResult sr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (sr == VK_SUCCESS)
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
    vkDestroyFence(dev, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);
    return (sr == VK_SUCCESS) ? LAGFX_OK : LAGFX_ERR_BACKEND;
}

void lagfx_vk_composite_shutdown(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) return;
    if (vk->composite_pipeline) { vkDestroyPipeline(vk->device, vk->composite_pipeline, NULL); vk->composite_pipeline = VK_NULL_HANDLE; }
    if (vk->composite_layout) { vkDestroyPipelineLayout(vk->device, vk->composite_layout, NULL); vk->composite_layout = VK_NULL_HANDLE; }
    if (vk->composite_pool) { vkDestroyDescriptorPool(vk->device, vk->composite_pool, NULL); vk->composite_pool = VK_NULL_HANDLE; vk->composite_set = VK_NULL_HANDLE; }
    if (vk->composite_dsl) { vkDestroyDescriptorSetLayout(vk->device, vk->composite_dsl, NULL); vk->composite_dsl = VK_NULL_HANDLE; }
    if (vk->composite_sampler) { vkDestroySampler(vk->device, vk->composite_sampler, NULL); vk->composite_sampler = VK_NULL_HANDLE; }
    vk->composite_ready = false;
}

#endif /* LAGFX_HAVE_VULKAN */
