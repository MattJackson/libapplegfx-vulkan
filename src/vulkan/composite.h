/*
 * libapplegfx-vulkan — layer compositor
 * src/vulkan/composite.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Composites a sampled surface onto display->rt with a MAX blend
 * (out = max(src, dst)) via a fullscreen textured triangle. Used by
 * the frame-blit queue so per-pass UI layers overlay the wallpaper
 * background instead of replacing it: bright UI/clock texels win,
 * near-black layer backgrounds leave the wallpaper intact. Blend is
 * alpha-independent (the translated per-pass surfaces do not carry a
 * reliable alpha channel). loadOp=LOAD preserves the current rt.
 */

#ifndef LAGFX_VULKAN_COMPOSITE_H
#define LAGFX_VULKAN_COMPOSITE_H

#include "instance.h"
#include "render_target.h"
#include "libapplegfx-vulkan.h"

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>

struct lagfx_vk_state;

/* Composite src_view onto display_rt (MAX blend, loadOp=LOAD). Builds the
 * pipeline lazily on first use. Synchronous (own submit + fence wait), so a
 * single descriptor set is safely reused across calls. src must be in
 * SHADER_READ_ONLY_OPTIMAL. No-op-safe on any failure (rt left as-is). */
lagfx_status_t lagfx_vk_composite_over(struct lagfx_vk_state *vk,
                                        lagfx_vk_render_target_t *display_rt,
                                        VkImageView src_view);

void lagfx_vk_composite_shutdown(struct lagfx_vk_state *vk);
#endif

#endif /* LAGFX_VULKAN_COMPOSITE_H */
