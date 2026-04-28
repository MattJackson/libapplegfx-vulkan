/*
 * libapplegfx-vulkan — cursor rendering (M5-20%)
 * src/vulkan/cursor.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "cursor.h"
#include "command.h"
#include "common/log.h"

#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

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

static void cursor_pipeline_barrier(VkCommandBuffer cmd,
                                     VkImage image,
                                     VkImageLayout old_layout,
                                     VkImageLayout new_layout,
                                     VkAccessFlags src_access,
                                     VkAccessFlags dst_access,
                                     VkPipelineStageFlags src_stage,
                                     VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = src_access,
        .dstAccessMask       = dst_access,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                          0, NULL,
                          0, NULL,
                          1, &b);
}

lagfx_status_t lagfx_vk_cursor_upload_glyph(struct lagfx_vk_state *vk,
                                             const uint8_t *argb_pixels,
                                             uint32_t width, uint32_t height,
                                             uint32_t bytes_per_row) {
    if (!vk || !argb_pixels || width == 0 || height == 0) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        return LAGFX_ERR_BACKEND;
    }

    VkImageSubresource subres = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel   = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    memset(&layout, 0, sizeof(layout));
    vkGetImageSubresourceLayout(vk->device, vk->cursor_glyph_image,
                                &subres, &layout);

    void *mapped = NULL;
    VkResult vr = vkMapMemory(vk->device, vk->cursor_glyph_mem,
                              0, VK_WHOLE_SIZE, 0, &mapped);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_upload: vkMapMemory failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    uint8_t *dst_base = (uint8_t *)mapped + layout.offset;
    const uint32_t dst_row_len = vk->cursor_glyph_w * 4u;

    if (width > vk->cursor_glyph_w || height > vk->cursor_glyph_h) {
        LAGFX_WARN("cursor_upload: glyph %ux%u exceeds image %ux%u — clamping",
                   width, height, vk->cursor_glyph_w, vk->cursor_glyph_h);
        width  = width  > vk->cursor_glyph_w  ? vk->cursor_glyph_w  : width;
        height = height > vk->cursor_glyph_h ? vk->cursor_glyph_h : height;
    }

    uint8_t *zeroes = (uint8_t *)calloc(1, dst_row_len);
    if (!zeroes) {
        vkUnmapMemory(vk->device, vk->cursor_glyph_mem);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    for (uint32_t y = 0; y < vk->cursor_glyph_h; ++y) {
        uint8_t *dst_row = dst_base + y * layout.rowPitch;
        if (y < height) {
            memcpy(dst_row, argb_pixels + y * bytes_per_row,
                   width * 4u);
            if (width * 4u < dst_row_len) {
                memset(dst_row + width * 4u, 0,
                       dst_row_len - width * 4u);
            }
        } else {
            memcpy(dst_row, zeroes, dst_row_len);
        }
    }
    free(zeroes);
    vkUnmapMemory(vk->device, vk->cursor_glyph_mem);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (cb_st != LAGFX_OK) {
        return cb_st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    cursor_pipeline_barrier(cb, vk->cursor_glyph_image,
                            VK_IMAGE_LAYOUT_PREINITIALIZED,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_HOST_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cb,
    };
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr == VK_SUCCESS) {
        const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
        vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    }

    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_upload: submit/wait failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkDescriptorImageInfo dii = {
        .sampler     = vk->cursor_sampler,
        .imageView   = vk->cursor_glyph_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = vk->cursor_desc_set,
        .dstBinding      = 1,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &dii,
    };
    vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

    vk->cursor_glyph_valid = true;
    LAGFX_LOG("cursor_upload: %ux%u bpr=%u uploaded OK", width, height,
              bytes_per_row);
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_cursor_draw(struct lagfx_vk_state *vk,
                                     VkCommandBuffer cb,
                                     uint32_t target_w, uint32_t target_h,
                                     int16_t cursor_x, int16_t cursor_y,
                                     uint16_t hot_x, uint16_t hot_y) {
    if (!vk || cb == VK_NULL_HANDLE) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->cursor_glyph_valid) {
        return LAGFX_OK;
    }
    if (vk->cursor_pipeline == VK_NULL_HANDLE) {
        LAGFX_WARN("cursor_draw: pipeline is VK_NULL_HANDLE — skipping");
        return LAGFX_OK;
    }
    if (vk->cursor_glyph_view == VK_NULL_HANDLE) {
        LAGFX_WARN("cursor_draw: glyph view is VK_NULL_HANDLE — skipping");
        return LAGFX_OK;
    }

    float ndc_x = 2.0f * (float)(cursor_x - (int16_t)hot_x)
                / (float)target_w - 1.0f;
    float ndc_y = 2.0f * (float)(cursor_y - (int16_t)hot_y)
                / (float)target_h - 1.0f;
    float ndc_w = 2.0f * (float)vk->cursor_glyph_w / (float)target_w;
    float ndc_h = 2.0f * (float)vk->cursor_glyph_h / (float)target_h;

    float params[4] = { ndc_x, ndc_y, ndc_w, ndc_h };

    void *mapped = NULL;
    VkResult vr = vkMapMemory(vk->device, vk->cursor_ubo_mem,
                              0, 16, 0, &mapped);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cursor_draw: vkMapMemory failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }
    memcpy(mapped, params, sizeof(params));
    vkUnmapMemory(vk->device, vk->cursor_ubo_mem);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vk->cursor_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk->cursor_layout, 0, 1,
                            &vk->cursor_desc_set, 0, NULL);

    VkViewport vp = {
        .x      = 0.0f,
        .y      = 0.0f,
        .width  = (float)target_w,
        .height = (float)target_h,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = { target_w, target_h },
    };
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdDraw(cb, 4, 1, 0, 0);

    LAGFX_TRACE("cursor_draw: pos=(%d,%d) hot=(%u,%u) ndc=(%f,%f,%f,%f)",
                (int)cursor_x, (int)cursor_y,
                (unsigned)hot_x, (unsigned)hot_y,
                (double)ndc_x, (double)ndc_y,
                (double)ndc_w, (double)ndc_h);
    return LAGFX_OK;
}

#endif
