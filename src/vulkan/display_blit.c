/*
 * libapplegfx-vulkan — display surface blit (present path)
 * src/vulkan/display_blit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "display_blit.h"
#include "instance.h"
#include "command.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

static uint32_t find_memory_type(VkPhysicalDevice phys,
                                  uint32_t typeBits,
                                  VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

static void image_barrier(VkCommandBuffer cmd,
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

lagfx_status_t lagfx_vk_display_present_surface(
    struct lagfx_vk_state *vk,
    lagfx_vk_render_target_t *display_rt,
    VkImage surface_image,
    VkImageLayout *surface_layout,
    uint32_t surface_width, uint32_t surface_height,
    uint32_t display_width, uint32_t display_height,
    uint64_t scanout_gpa, uint64_t scanout_length,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
    uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h) {
    if (!vk || !display_rt || surface_image == VK_NULL_HANDLE) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        return LAGFX_ERR_BACKEND;
    }

    VkImageLayout surf_old = VK_IMAGE_LAYOUT_UNDEFINED;
    if (surface_layout) {
        surf_old = *surface_layout;
    }

    VkAccessFlags surf_src_access = 0;
    VkPipelineStageFlags surf_src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (surf_old == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        surf_src_access = VK_ACCESS_TRANSFER_READ_BIT;
        surf_src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (surf_old == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        surf_src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        surf_src_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (surf_old == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        surf_src_access = VK_ACCESS_SHADER_READ_BIT;
        surf_src_stage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    VkAccessFlags rt_src_access = 0;
    VkPipelineStageFlags rt_src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (display_rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        rt_src_access = VK_ACCESS_TRANSFER_READ_BIT;
        rt_src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (display_rt->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        rt_src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        rt_src_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }

    const size_t rt_stride = (size_t)display_width * 4u;
    const size_t rt_bytes  = rt_stride * (size_t)display_height;
    const bool want_readback = (scanout_gpa != 0 && scanout_length >= rt_bytes
                                && write_memory != NULL);

    VkBuffer        staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory  staging_mem = VK_NULL_HANDLE;
    VkCommandBuffer cb          = VK_NULL_HANDLE;
    VkFence         fence       = VK_NULL_HANDLE;
    lagfx_status_t  result      = LAGFX_OK;

    if (want_readback) {
        VkBufferCreateInfo bci = {
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = rt_bytes,
            .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &staging_buf);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkCreateBuffer failed (%d)", (int)vr);
            return LAGFX_ERR_BACKEND;
        }
        VkMemoryRequirements breq;
        vkGetBufferMemoryRequirements(vk->device, staging_buf, &breq);
        uint32_t mtype = find_memory_type(
            vk->phys_device, breq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mtype == UINT32_MAX) {
            LAGFX_ERR("display_blit: no HOST_VISIBLE+COHERENT memory type");
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
        VkMemoryAllocateInfo mai = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = breq.size,
            .memoryTypeIndex = mtype,
        };
        vr = vkAllocateMemory(vk->device, &mai, NULL, &staging_mem);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkAllocateMemory failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
        vr = vkBindBufferMemory(vk->device, staging_buf, staging_mem, 0);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkBindBufferMemory failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    {
        lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
        if (cb_st != LAGFX_OK) {
            result = cb_st;
            goto cleanup;
        }
    }

    {
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkResult vr = vkBeginCommandBuffer(cb, &bi);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkBeginCommandBuffer failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    image_barrier(cb, surface_image,
                  surf_old, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  surf_src_access, VK_ACCESS_TRANSFER_READ_BIT,
                  surf_src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);

    image_barrier(cb, display_rt->image,
                  display_rt->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  rt_src_access, VK_ACCESS_TRANSFER_WRITE_BIT,
                  rt_src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);

    {
        VkImageBlit blit_region = {
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .srcOffsets[0] = { 0, 0, 0 },
            .srcOffsets[1] = { (int32_t)surface_width,
                               (int32_t)surface_height, 1 },
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            /* Placement: blit into the layer's declared dst rect (offset+
             * extent) when given; else full-screen (legacy). Clamped to the
             * display bounds. display_rt persists across layers, so sub-rect
             * blits compose into the final scanout. */
            .dstOffsets[0] = { (int32_t)dst_x, (int32_t)dst_y, 0 },
            .dstOffsets[1] = {
                (int32_t)((dst_w && dst_x + dst_w <= display_width) ? dst_x + dst_w : display_width),
                (int32_t)((dst_h && dst_y + dst_h <= display_height) ? dst_y + dst_h : display_height),
                1 },
        };
        vkCmdBlitImage(cb,
                       surface_image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       display_rt->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit_region, VK_FILTER_LINEAR);
    }

    image_barrier(cb, surface_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    image_barrier(cb, display_rt->image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (want_readback) {
        VkBufferImageCopy region = {
            .bufferOffset      = 0,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { display_width, display_height, 1u },
        };
        vkCmdCopyImageToBuffer(cb, display_rt->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging_buf, 1, &region);
    }

    {
        VkResult vr = vkEndCommandBuffer(cb);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkEndCommandBuffer failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    {
        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        VkResult vr = vkCreateFence(vk->device, &fci, NULL, &fence);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkCreateFence failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    {
        VkSubmitInfo si = {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        VkResult vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkQueueSubmit failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    {
        const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
        VkResult vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE,
                                       timeout_ns);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkWaitForFences failed/timeout (%d)",
                       (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
    }

    if (surface_layout) {
        *surface_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    display_rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    if (want_readback) {
        void *mapped = NULL;
        VkResult vr = vkMapMemory(vk->device, staging_mem, 0, rt_bytes,
                                   0, &mapped);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_blit: vkMapMemory failed (%d)", (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup;
        }
        if (!write_memory(shell_opaque, scanout_gpa, (uint64_t)rt_bytes,
                          mapped)) {
            LAGFX_WARN("display_blit: DMA writeback to gpa=0x%llx "
                       "(%zu bytes) failed",
                       (unsigned long long)scanout_gpa, rt_bytes);
        } else {
            LAGFX_LOG("display_blit: DMA writeback gpa=0x%llx bytes=%zu OK",
                      (unsigned long long)scanout_gpa, rt_bytes);
        }
        vkUnmapMemory(vk->device, staging_mem);
    }

    LAGFX_LOG("display_blit: %ux%u -> %ux%u OK",
              surface_width, surface_height,
              display_width, display_height);

cleanup:
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk->device, fence, NULL);
    }
    if (cb != VK_NULL_HANDLE) {
        lagfx_vk_cmdbuf_free(vk, cb);
    }
    if (staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, staging_mem, NULL);
    }
    if (staging_buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, staging_buf, NULL);
    }
    return result;
}

#endif
