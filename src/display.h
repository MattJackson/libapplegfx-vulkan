/*
 * libapplegfx-vulkan — internal display state
 * src/display.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LIBAPPLEGFX_DISPLAY_INTERNAL_H
#define LIBAPPLEGFX_DISPLAY_INTERNAL_H

#include "libapplegfx-vulkan.h"
#include "device.h"

struct lagfx_display {
    uint32_t magic;                     /* LAGFX_DISPLAY_MAGIC */
    lagfx_device_t *device;             /* owning device (not owned) */
    lagfx_display_descriptor_t desc;    /* copied */
    uint32_t port;
    uint32_t serial_num;

    /* Current cursor position. Updated by Phase 1.A.2 MMIO handlers;
     * for now stays at (0,0). */
    lagfx_coord_t cursor_pos;

    /* Phase 1.A.1 has no renderer. Future: VkImage handles, frame
     * counter, dirty flag. */
    int has_frame;
};

#endif /* LIBAPPLEGFX_DISPLAY_INTERNAL_H */
