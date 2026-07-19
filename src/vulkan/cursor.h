/*
 * libapplegfx-vulkan — cursor rendering (M5-20%)
 * src/vulkan/cursor.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 */

#ifndef LIBAPPLEGFX_VULKAN_CURSOR_H
#define LIBAPPLEGFX_VULKAN_CURSOR_H

#include "instance.h"
#include "libapplegfx-vulkan.h"

#ifdef LAGFX_HAVE_VULKAN

lagfx_status_t lagfx_vk_cursor_upload_glyph(struct lagfx_vk_state *vk,
                                             const uint8_t *argb_pixels,
                                             uint32_t width, uint32_t height,
                                             uint32_t bytes_per_row);

lagfx_status_t lagfx_vk_cursor_draw(struct lagfx_vk_state *vk,
                                     VkCommandBuffer cb,
                                     uint32_t target_w, uint32_t target_h,
                                     int16_t cursor_x, int16_t cursor_y,
                                     uint16_t hot_x, uint16_t hot_y);

#endif
#endif
