/*
 * libapplegfx-vulkan — display surface blit (present path)
 * src/vulkan/display_blit.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 */

#ifndef LIBAPPLEGFX_VULKAN_DISPLAY_BLIT_H
#define LIBAPPLEGFX_VULKAN_DISPLAY_BLIT_H

#include "instance.h"
#include "render_target.h"

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN

lagfx_status_t lagfx_vk_display_present_surface(
    struct lagfx_vk_state *vk,
    lagfx_vk_render_target_t *display_rt,
    VkImage surface_image,
    VkImageLayout *surface_layout,
    uint32_t surface_width, uint32_t surface_height,
    uint32_t display_width, uint32_t display_height,
    uint64_t scanout_gpa, uint64_t scanout_length,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *));

#endif

#endif
