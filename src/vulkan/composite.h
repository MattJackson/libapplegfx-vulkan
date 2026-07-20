/*
 * libapplegfx-vulkan — layer compositor
 * src/vulkan/composite.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Composites a sampled surface onto display->rt with src-over alpha
 * blending (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) via a textured triangle,
 * optionally into a destination sub-rect. Used by the frame-blit queue
 * so per-pass UI layers composite ON TOP of the wallpaper background in
 * guest submit order instead of replacing it: transparent/alpha-0 layer
 * texels leave the background intact, opaque texels win — the guest's
 * declared per-pixel alpha decides. loadOp=LOAD preserves the current rt.
 */

#ifndef LAGFX_VULKAN_COMPOSITE_H
#define LAGFX_VULKAN_COMPOSITE_H

#include "instance.h"
#include "render_target.h"
#include "libapplegfx-vulkan.h"

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

struct lagfx_vk_state;

/* Composite src_view onto display_rt (src-over blend, loadOp=LOAD) into the
 * dst rect (dst_w/dst_h == 0 → full-screen). Builds the pipeline lazily on
 * first use. Synchronous (own submit + fence wait), so a single descriptor
 * set is safely reused across calls. src must be in SHADER_READ_ONLY_OPTIMAL.
 * No-op-safe on any failure (rt left as-is). */
lagfx_status_t lagfx_vk_composite_over(struct lagfx_vk_state *vk,
                                        lagfx_vk_render_target_t *display_rt,
                                        VkImageView src_view,
                                        uint32_t dst_x, uint32_t dst_y,
                                        uint32_t dst_w, uint32_t dst_h);

void lagfx_vk_composite_shutdown(struct lagfx_vk_state *vk);
#endif

#endif /* LAGFX_VULKAN_COMPOSITE_H */
