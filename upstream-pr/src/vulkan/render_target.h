/*
 * libapplegfx-vulkan — Vulkan render target + clear + readback (Phase 2.B)
 * src/vulkan/render_target.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Phase 2.B layers a minimum-viable colour-attachment render context on
 * the instance+device+queue+command_pool quartet from Phase 1.B/1.B.2.
 * It owns three operations:
 *
 *   lagfx_vk_render_target_create  — allocate a VkImage + view + backing
 *                                    DeviceMemory suitable as a colour
 *                                    attachment + transfer source.
 *   lagfx_vk_render_target_destroy — reciprocal teardown.
 *   lagfx_vk_render_clear_color    — record clear-load dynamic-rendering
 *                                    into a caller-owned VkCommandBuffer
 *                                    (caller begins/ends/submits).
 *   lagfx_vk_render_target_readback — submit a one-shot vkCmdCopyImageToBuffer
 *                                     (with the right layout barriers) into
 *                                     a host-visible staging buffer, wait on
 *                                     a fence, memcpy into caller-provided
 *                                     destination. Returns the linear BGRA8
 *                                     stride via *out_stride.
 *
 * === Graceful degradation ======================================
 *
 * When built without Vulkan (LAGFX_HAVE_VULKAN unset) every entry point
 * here is a no-op that returns LAGFX_ERR_BACKEND (create/readback) or
 * does nothing (destroy/clear). Callers must be prepared for that on
 * the Darwin dev path.
 */

#ifndef LIBAPPLEGFX_VULKAN_RENDER_TARGET_H
#define LIBAPPLEGFX_VULKAN_RENDER_TARGET_H

#include "instance.h"

#include "libapplegfx-vulkan.h"

#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN

/* A render target owns:
 *   - VkImage   (colour attachment + transfer source)
 *   - VkImageView (whole-image 2D view over the image)
 *   - VkDeviceMemory (device-local backing)
 * Created in COLOR_ATTACHMENT_OPTIMAL and tracked through the layout
 * transitions driven by the clear + readback helpers below.
 */
typedef struct lagfx_vk_render_target {
    VkImage         image;
    VkImageView     view;
    VkDeviceMemory  memory;

    uint32_t        width;
    uint32_t        height;
    VkFormat        format;

    /* Current layout — mutated by the helpers. Starts at UNDEFINED and
     * is transitioned to COLOR_ATTACHMENT_OPTIMAL on first clear. */
    VkImageLayout   layout;
} lagfx_vk_render_target_t;

/* Allocate a render target at (width,height,format). Memory backing is
 * device-local; on lavapipe that is ordinary host RAM. Layout starts at
 * VK_IMAGE_LAYOUT_UNDEFINED — the clear helper transitions it to
 * COLOR_ATTACHMENT_OPTIMAL. Returns LAGFX_OK on success and writes the
 * populated struct into *out. On failure *out is left with zeroed
 * handles. */
lagfx_status_t lagfx_vk_render_target_create(struct lagfx_vk_state *vk,
                                             uint32_t width,
                                             uint32_t height,
                                             VkFormat format,
                                             lagfx_vk_render_target_t *out);

/* Tear down: destroys view, image, memory (in that order). Safe on a
 * zeroed struct. */
void lagfx_vk_render_target_destroy(struct lagfx_vk_state *vk,
                                    lagfx_vk_render_target_t *rt);

/* Record into an already-begun VkCommandBuffer:
 *   1. pipeline barrier transitioning rt->layout to
 *      COLOR_ATTACHMENT_OPTIMAL (from either UNDEFINED on first use or
 *      TRANSFER_SRC_OPTIMAL on a subsequent frame post-readback);
 *   2. vkCmdBeginRendering with one color attachment using LOAD_OP_CLEAR
 *      + STORE_OP_STORE and the clear value from rgba[4];
 *   3. vkCmdEndRendering (no draws — the clear is executed by the
 *      load-op).
 *
 * Updates rt->layout to COLOR_ATTACHMENT_OPTIMAL on return. Requires
 * vk->have_dynamic_rendering to be true. */
lagfx_status_t lagfx_vk_render_clear_color(struct lagfx_vk_state *vk,
                                           VkCommandBuffer cmd,
                                           lagfx_vk_render_target_t *rt,
                                           const float rgba[4]);

/* Read back the rendered pixels into a caller-provided host buffer.
 * Allocates a transient staging buffer, records a one-shot command
 * buffer with the image→transfer-src layout transition +
 * vkCmdCopyImageToBuffer, submits with a fence, waits up to 1s,
 * memcpys into dst, frees the staging buffer.
 *
 * dst_size must be >= height * stride (where stride = width*4 for
 * BGRA8 / RGBA8). *out_stride receives the linear stride (bytes per
 * row, == width*4).
 *
 * After a successful readback rt->layout is TRANSFER_SRC_OPTIMAL. The
 * next clear will transition back to COLOR_ATTACHMENT_OPTIMAL. */
lagfx_status_t lagfx_vk_render_target_readback(struct lagfx_vk_state *vk,
                                               lagfx_vk_render_target_t *rt,
                                               void *dst,
                                               size_t dst_size,
                                               size_t *out_stride);

#else  /* !LAGFX_HAVE_VULKAN ------------------------------------- */

/* No-vulkan stubs: keep the type visible (as opaque) so display.c can
 * carry a pointer without branching on LAGFX_HAVE_VULKAN at every
 * struct member access. */
typedef struct lagfx_vk_render_target {
    int _placeholder;
} lagfx_vk_render_target_t;

#endif /* LAGFX_HAVE_VULKAN */

#endif /* LIBAPPLEGFX_VULKAN_RENDER_TARGET_H */
