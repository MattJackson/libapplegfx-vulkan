/*
 * libapplegfx-vulkan — Blit-encoder Vulkan translation layer (M5 phase 1)
 * src/translate/blit_encoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implements real Vulkan translation for critical blit opcodes that are
 * blocking M5 stage 20%. This is the minimum viable set to produce visible
 * pixels: FillTextureWithColor (clear) and CopyFromTextureToTexture (surface copies).
 *
 * Implementation notes:
 *   - Uses staging buffers for host-visible access (matches display.c patterns)
 *   - Fails gracefully on missing resources (fail-open like render encoder)
 *   - Minimal validation; production will need tighter error handling
 *
 * Phase 1 target (2026-05-07): Get something visible on screen.
 * Future phases: Complete remaining blit ops, optimize paths.
 */

#include "blit_encoder.h"

#include "../common/log.h"
#include "../device.h"
#include "../display.h"
#include "../vulkan/command.h"
#include "../vulkan/iosurface.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* ================================================================
 * Helper lookup — find memory type index (inlined from display.c pattern)
 * ================================================================ */
#ifdef LAGFX_HAVE_VULKAN

static uint32_t blit_find_memory_type(VkPhysicalDevice phys,
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

/* ================================================================
 * Internal helpers — staging buffer management
 * ================================================================ */

static VkBuffer create_staging_buffer(struct lagfx_vk_state *vk_impl,
                                      size_t size) {
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult vr = vkCreateBuffer(vk_impl->device, &bci, NULL, &buf);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: create staging buffer failed (%zu bytes, %d)",
                   size, (int)vr);
        return VK_NULL_HANDLE;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(vk_impl->device, buf, &mem_reqs);

    VkMemoryAllocateInfo mai = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = blit_find_memory_type(vk_impl->phys_device,
                                                    mem_reqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };

    VkDeviceMemory mem = VK_NULL_HANDLE;
    vr = vkAllocateMemory(vk_impl->device, &mai, NULL, &mem);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: allocate staging memory failed (%d)", (int)vr);
        vkDestroyBuffer(vk_impl->device, buf, NULL);
        return VK_NULL_HANDLE;
    }

    vr = vkBindBufferMemory(vk_impl->device, buf, mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_WARN("blit_encoder: bind staging memory failed (%d)", (int)vr);
        vkFreeMemory(vk_impl->device, mem, NULL);
        vkDestroyBuffer(vk_impl->device, buf, NULL);
        return VK_NULL_HANDLE;
    }

    return buf;
}

static void destroy_staging_buffer(struct lagfx_vk_state *vk_impl,
                                   VkBuffer buf,
                                   VkDeviceMemory mem) {
    if (buf != VK_NULL_HANDLE) vkDestroyBuffer(vk_impl->device, buf, NULL);
    if (mem != VK_NULL_HANDLE) vkFreeMemory(vk_impl->device, mem, NULL);
}

#endif /* LAGFX_HAVE_VULKAN */

/* ================================================================
 * 0x141 FillTextureWithColor — vkCmdClearColorImage
 *
 *   +0x00  u32 texture_ref        (resource registry index)
 *   +0x04  u32 level              (mip level)
 *   +0x08  u32 slice              (array slice / depth plane)
 *   +0x0c  MTLOrigin origin       (8 bytes: x, y, z)
 *   +0x18  MTLSize size           (12 bytes: width, height, depth)
 *   +0x24  u64 color[4]           (32 bytes: RGBA doubles -> float conversion)
 *   +0x44  u32 pixel_format       (4 bytes: MTLPixelFormat enum)
 *   = 92 B total
 * ================================================================ */

#ifdef LAGFX_HAVE_VULKAN

lagfx_status_t lagfx_translate_blit_fill_texture_color(void *vk,
                                                       void *texture_ref,
                                                       uint32_t level,
                                                       uint32_t slice,
                                                       const MTLOrigin *origin,
                                                       const MTLSize *size,
                                                       const double color[4],
                                                       uint32_t pixel_format) {
    struct lagfx_vk_state *vk_impl = (struct lagfx_vk_state *)vk;

    (void)pixel_format;  // TODO: validate format compatibility

    if (!vk || !texture_ref || !origin || !size || !color) {
        return LAGFX_ERR_INVALID_ARG;
    }

    if (size->width == 0 || size->height == 0) {
        LAGFX_WARN("blit_fill_texture_color: empty region (%ux%ux%u)",
                   size->width, size->height, size->depth);
        return LAGFX_OK;  // No-op for empty region
    }

    lagfx_vk_iosurface_t *tex = (lagfx_vk_iosurface_t *)texture_ref;
    if (!tex || tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_fill_texture_color: invalid texture handle");
        return LAGFX_ERR_INVALID_ARG;
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_fill_texture_color: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Transition image to transfer destination layout */
    VkImageMemoryBarrier barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = tex->image,
        .subresourceRange = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = level,
            .levelCount   = 1,
            .baseArrayLayer = slice,
            .layerCount   = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &barrier);

    /* Convert double color to float */
    VkClearColorValue clear_color = {0};
    for (int i = 0; i < 4; ++i) {
        clear_color.float32[i] = (float)color[i];
    }

    /* Issue clear command */
    VkImageSubresourceRange subres = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = level,
        .levelCount     = 1,
        .baseArrayLayer = slice,
        .layerCount     = 1,
    };

    vkCmdClearColorImage(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &clear_color, 1, &subres);

    /* Transition back to general layout for subsequent operations */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, NULL, 0, NULL, 1, &barrier);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_fill_texture_color: vkEndCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Submit and wait */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    vr = vkCreateFence(vk_impl->device, &fci, NULL, &fence);
    if (vr == VK_SUCCESS) {
        VkSubmitInfo si = {
            .sType  = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        vkQueueSubmit(vk_impl->graphics_queue, 1, &si, fence);
        vkWaitForFences(vk_impl->device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk_impl->device, fence, NULL);
    } else {
        LAGFX_WARN("blit_fill_texture_color: create fence failed (%d)", (int)vr);
    }

    lagfx_vk_cmdbuf_free(vk, cb);
    return LAGFX_OK;
}

/* ================================================================
 * 0x12f CopyFromTextureToTexture — vkCmdCopyImage
 *
 *   +0x00  u32 src_texture_ref      (resource registry index)
 *   +0x08  u32 dst_texture_ref      (resource registry index)
 *   +0x10  MTLOrigin src_origin     (8 bytes: x, y, z)
 *   +0x1c  MTLSize src_size         (12 bytes: width, height, depth)
 *   +0x28  u32 dst_slice            (4 bytes)
 *   +0x2c  u32 dst_level            (4 bytes)
*   = 88 B total
 * ================================================================ */

lagfx_status_t lagfx_translate_blit_copy_texture_to_texture(void *vk,
                                                            void *src_texture_ref,
                                                            void *dst_texture_ref,
                                                            const MTLOrigin *src_origin,
                                                            const MTLSize *src_size,
                                                            uint32_t dst_slice,
                                                            uint32_t dst_level) {
#endif /* LAGFX_HAVE_VULKAN */

#ifdef LAGFX_HAVE_VULKAN
    struct lagfx_vk_state *vk_impl = (struct lagfx_vk_state *)vk;

    if (!vk || !src_texture_ref || !dst_texture_ref
        || !src_origin || !src_size) {
        return LAGFX_ERR_INVALID_ARG;
    }

    if (src_size->width == 0 || src_size->height == 0) {
        LAGFX_WARN("blit_copy_texture_to_texture: empty region");
        return LAGFX_OK;
    }

    lagfx_vk_iosurface_t *src_tex = (lagfx_vk_iosurface_t *)src_texture_ref;
    lagfx_vk_iosurface_t *dst_tex = (lagfx_vk_iosurface_t *)dst_texture_ref;

    if (!src_tex || src_tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_copy_texture_to_texture: invalid source texture");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!dst_tex || dst_tex->image == VK_NULL_HANDLE) {
        LAGFX_WARN("blit_copy_texture_to_texture: invalid dest texture");
        return LAGFX_ERR_INVALID_ARG;
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_copy_texture_to_texture: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Transition src to transfer source layout */
    VkImageMemoryBarrier src_barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = src_tex->image,
        .subresourceRange = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount   = 1,
            .baseArrayLayer = 0,
            .layerCount   = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &src_barrier);

    /* Transition dst to transfer dest layout */
    VkImageMemoryBarrier dst_barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image         = dst_tex->image,
        .subresourceRange = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = dst_level,
            .levelCount   = 1,
            .baseArrayLayer = dst_slice,
            .layerCount   = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &dst_barrier);

    /* Issue copy command */
    VkImageCopy region = {
        .srcSubresource = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel    = 0,
            .baseArrayLayer = 0,
            .layerCount   = 1,
        },
        .srcOffset = {
            .x = (int32_t)src_origin->x,
            .y = (int32_t)src_origin->y,
            .z = (int32_t)src_origin->z,
        },
        .dstSubresource = {
            .aspectMask  = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel    = dst_level,
            .baseArrayLayer = dst_slice,
            .layerCount   = 1,
        },
        .dstOffset = {0, 0, 0},
        .extent = {
            .width  = src_size->width,
            .height = src_size->height,
            .depth  = src_size->depth,
        },
    };

    vkCmdCopyImage(cb, src_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dst_tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region);

    /* Restore layouts */
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, NULL, 0, NULL, 1, &src_barrier);

    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, NULL, 0, NULL, 1, &dst_barrier);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("blit_copy_texture_to_texture: vkEndCommandBuffer failed (%d)",
                   (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    /* Submit and wait */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    vr = vkCreateFence(vk_impl->device, &fci, NULL, &fence);
    if (vr == VK_SUCCESS) {
        VkSubmitInfo si = {
            .sType  = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        vkQueueSubmit(vk_impl->graphics_queue, 1, &si, fence);
        vkWaitForFences(vk_impl->device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(vk_impl->device, fence, NULL);
    } else {
        LAGFX_WARN("blit_copy_texture_to_texture: create fence failed (%d)", (int)vr);
    }

    lagfx_vk_cmdbuf_free(vk, cb);
    return LAGFX_OK;
}


#endif /* LAGFX_HAVE_VULKAN */

#ifndef LAGFX_HAVE_VULKAN
/* Non-Vulkan host stubs. blit_opcodes.c calls these unconditionally;
 * the real implementations live above inside LAGFX_HAVE_VULKAN. On
 * non-Vulkan hosts (macOS CI runner, etc.) we satisfy the linker with
 * stubs that return NOT_SUPPORTED so the library can build + run the
 * non-Vulkan stub init path. */

lagfx_status_t lagfx_translate_blit_fill_texture_color(void *vk,
                                                       void *texture_ref,
                                                       uint32_t level,
                                                       uint32_t slice,
                                                       const MTLOrigin *origin,
                                                       const MTLSize *size,
                                                       const double color[4],
                                                       uint32_t pixel_format) {
    (void)vk; (void)texture_ref; (void)level; (void)slice;
    (void)origin; (void)size; (void)color; (void)pixel_format;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_blit_copy_texture_to_texture(void *vk,
                                                            void *src_texture_ref,
                                                            void *dst_texture_ref,
                                                            const MTLOrigin *src_origin,
                                                            const MTLSize *src_size,
                                                            uint32_t dst_slice,
                                                            uint32_t dst_level) {
    (void)vk; (void)src_texture_ref; (void)dst_texture_ref;
    (void)src_origin; (void)src_size; (void)dst_slice; (void)dst_level;
    return LAGFX_ERR_BACKEND;
}
#endif /* !LAGFX_HAVE_VULKAN */
