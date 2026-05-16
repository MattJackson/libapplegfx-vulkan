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
    /* Refcount. Initial value 1 in _create. _retain increments;
     * _release decrements and destroys when count hits 0.
     * Needed because CmdImportIOSurfaceMachPort (0x29) aliases the
     * same backing VkImage from a second resource-registry entry,
     * and Delete/Unmap on either entry must not free a backing the
     * other entry is still using. BQL-serialised in steady-state so
     * non-atomic. */
    uint32_t refcount;
} lagfx_vk_iosurface_t;

lagfx_status_t lagfx_vk_iosurface_create(struct lagfx_vk_state *vk,
                                          uint32_t width, uint32_t height,
                                          uint32_t pixel_format,
                                          lagfx_vk_iosurface_t **out);

/* Increment refcount on an existing iosurface backing. Use when
 * aliasing a backing across resource-registry entries (Import). */
void lagfx_vk_iosurface_retain(lagfx_vk_iosurface_t *ios);

/* Decrement refcount; destroy + free when refcount hits 0.
 * Replaces direct _destroy in the handler hot path so aliased
 * backings don't double-free. _destroy is kept for cases where the
 * caller knows there are no other references (test cleanup, etc.). */
void lagfx_vk_iosurface_release(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios);

void lagfx_vk_iosurface_destroy(struct lagfx_vk_state *vk,
                                 lagfx_vk_iosurface_t *ios);

#else

typedef struct lagfx_vk_iosurface_t {
    int _placeholder;
} lagfx_vk_iosurface_t;

#endif

#endif
