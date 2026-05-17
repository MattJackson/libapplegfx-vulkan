/*
 * libapplegfx-vulkan — Vulkan-backed IOSurface create/destroy (Phase 4)
 * src/vulkan/iosurface.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "iosurface.h"
#include "instance.h"
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

VkFormat lagfx_metal_pixel_format_to_vk(uint32_t pixel_format) {
    switch (pixel_format) {
    case 80: return VK_FORMAT_B8G8R8A8_UNORM;      /* MTLPixelFormatBGRA8Unorm */
    case 70: return VK_FORMAT_R8G8B8A8_UNORM;      /* MTLPixelFormatRGBA8Unorm */
    case 252: return VK_FORMAT_D32_SFLOAT;         /* MTLPixelFormatDepth32Float */
    case 25: return VK_FORMAT_D16_UNORM;           /* MTLPixelFormatDepth16Unorm */
    default:
        LAGFX_WARN("iosurface: unknown Metal pixel format %u, "
                    "falling back to BGRA8", (unsigned)pixel_format);
        return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

static VkFormat metal_pixel_format_to_vk(uint32_t pixel_format) {
    return lagfx_metal_pixel_format_to_vk(pixel_format);
}

lagfx_status_t lagfx_vk_iosurface_create(struct lagfx_vk_state *vk,
                                          uint32_t width, uint32_t height,
                                          uint32_t pixel_format,
                                          lagfx_vk_iosurface_t **out) {
    if (!vk || !out || width == 0u || height == 0u) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        LAGFX_ERR("iosurface_create: vk state not initialized");
        return LAGFX_ERR_BACKEND;
    }

    lagfx_vk_iosurface_t *ios = calloc(1, sizeof(*ios));
    if (!ios) return LAGFX_ERR_OUT_OF_MEMORY;

    VkFormat fmt = metal_pixel_format_to_vk(pixel_format);

    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { width, height, 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult vr = vkCreateImage(vk->device, &ici, NULL, &ios->image);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkCreateImage failed (VkResult=%d)", (int)vr);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, ios->image, &req);

    uint32_t mtype = find_memory_type(vk->phys_device, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mtype == UINT32_MAX) {
        mtype = find_memory_type(vk->phys_device, req.memoryTypeBits, 0u);
    }
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("iosurface_create: no matching memory type (typeBits=0x%x)",
                   req.memoryTypeBits);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &ios->memory);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkAllocateMemory failed (VkResult=%d)", (int)vr);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindImageMemory(vk->device, ios->image, ios->memory, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkBindImageMemory failed (VkResult=%d)", (int)vr);
        vkFreeMemory(vk->device, ios->memory, NULL);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = ios->image,
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
    vr = vkCreateImageView(vk->device, &vci, NULL, &ios->view);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("iosurface_create: vkCreateImageView failed (VkResult=%d)", (int)vr);
        vkFreeMemory(vk->device, ios->memory, NULL);
        vkDestroyImage(vk->device, ios->image, NULL);
        free(ios);
        return LAGFX_ERR_BACKEND;
    }

    ios->width  = width;
    ios->height = height;
    ios->format = fmt;
    ios->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ios->refcount = 1u;

    LAGFX_LOG("iosurface_create: %ux%u fmt=%d image=%p view=%p mem=%p refcount=1",
              width, height, (int)fmt,
              (void *)ios->image, (void *)ios->view, (void *)ios->memory);

    *out = ios;
    return LAGFX_OK;
}

void lagfx_vk_iosurface_retain(lagfx_vk_iosurface_t *ios) {
    if (!ios) return;
    /* BQL-serialised in steady-state; non-atomic is correct here.
     * If we ever drain on a thread outside BQL, lift to _Atomic. */
    ios->refcount += 1u;
    LAGFX_TRACE("iosurface_retain: %ux%u → refcount=%u",
                ios->width, ios->height, ios->refcount);
}

void lagfx_vk_iosurface_release(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios) {
    if (!ios) return;
    if (ios->refcount == 0u) {
        LAGFX_WARN("iosurface_release: refcount already 0 — double release? "
                   "%ux%u fmt=%d", ios->width, ios->height, (int)ios->format);
        return;
    }
    ios->refcount -= 1u;
    LAGFX_TRACE("iosurface_release: %ux%u → refcount=%u",
                ios->width, ios->height, ios->refcount);
    if (ios->refcount == 0u) {
        lagfx_vk_iosurface_destroy(vk, ios);
    }
}

void lagfx_vk_iosurface_destroy(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios) {
    if (!vk || !ios) return;
    if (vk->device == VK_NULL_HANDLE) {
        free(ios);
        return;
    }
    if (ios->view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, ios->view, NULL);
    }
    if (ios->image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, ios->image, NULL);
    }
    if (ios->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, ios->memory, NULL);
    }
    LAGFX_LOG("iosurface_destroy: %ux%u fmt=%d (was refcount=%u)",
              ios->width, ios->height, (int)ios->format, ios->refcount);
    free(ios);
}

#endif
