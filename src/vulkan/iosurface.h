/*
 * libapplegfx-vulkan — Vulkan-backed IOSurface (Phase 4)
 * src/vulkan/iosurface.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal header. Not installed.
 */

#ifndef LIBAPPLEGFX_VULKAN_IOSURFACE_H
#define LIBAPPLEGFX_VULKAN_IOSURFACE_H

#include "instance.h"

#include "libapplegfx-vulkan.h"

#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN

typedef struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    VkImageLayout layout;
} lagfx_vk_iosurface_t;

lagfx_status_t lagfx_vk_iosurface_create(struct lagfx_vk_state *vk,
                                          uint32_t width, uint32_t height,
                                          uint32_t pixel_format,
                                          lagfx_vk_iosurface_t **out);

void lagfx_vk_iosurface_destroy(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios);

#else

typedef struct lagfx_vk_iosurface_t {
    int _placeholder;
} lagfx_vk_iosurface_t;

#endif

#endif
