/*
 * libapplegfx-vulkan — Vulkan-backed IOSurface (Phase 4)
 * src/vulkan/iosurface.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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
    /* Composite placement (0x82 viewport): the layer's destination rect in
     * the scanout. dst_valid=0 → blit full-screen (legacy). Set at per-pass
     * creation from the owning task's viewport so the ASMBLIT/composite path
     * places each layer at its guest-declared offset instead of stretching. */
    uint32_t dst_x, dst_y, dst_w, dst_h;
    uint8_t  dst_valid;
} lagfx_vk_iosurface_t;

lagfx_status_t lagfx_vk_iosurface_create(struct lagfx_vk_state *vk,
                                          uint32_t width, uint32_t height,
                                          uint32_t pixel_format,
                                          lagfx_vk_iosurface_t **out);

/* Upload CPU pixel bytes (tightly-packed BGRA8/RGBA8 at ios->width×height) into
 * an existing IOSurface VkImage via a staging buffer, then transition it to
 * SHADER_READ_ONLY_OPTIMAL (sets ios->layout) so it is immediately sampleable.
 * `data_len` shorter than the image is zero-padded; longer is truncated. Used to
 * back a guest texture from its placement-descriptor memory so texture-sampling
 * composites can sample real guest content. */
lagfx_status_t lagfx_vk_iosurface_upload_pixels(struct lagfx_vk_state *vk,
                                                lagfx_vk_iosurface_t *ios,
                                                const uint8_t *data,
                                                size_t data_len);

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

/* Map Apple Metal pixel format codes to VkFormat.
 * Cites: iosurface.c line 30-41 (implementation).
 * Supported mappings:
 *   80 -> VK_FORMAT_B8G8R8A8_UNORM (MTLPixelFormatBGRA8Unorm)
 *   70 -> VK_FORMAT_R8G8B8A8_UNORM (MTLPixelFormatRGBA8Unorm)
 *   252 -> VK_FORMAT_D32_SFLOAT (MTLPixelFormatDepth32Float)
 *   25 -> VK_FORMAT_D16_UNORM (MTLPixelFormatDepth16Unorm)
 * Unknown formats fall back to BGRA8 with a WARN log. */
VkFormat lagfx_metal_pixel_format_to_vk(uint32_t pixel_format);

#else

typedef struct lagfx_vk_iosurface_t {
    int _placeholder;
} lagfx_vk_iosurface_t;

#endif

#endif
