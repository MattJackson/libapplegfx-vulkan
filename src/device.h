/*
 * libapplegfx-vulkan — internal device state
 * src/device.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not installed. Only other translation units inside the library
 * should include this (display.c reaches into struct lagfx_device
 * to register itself against the owning device).
 */

#ifndef LIBAPPLEGFX_DEVICE_INTERNAL_H
#define LIBAPPLEGFX_DEVICE_INTERNAL_H

#include "libapplegfx-vulkan.h"

#include <stddef.h>
#include <stdint.h>

/* Forward decl; defined in src/vulkan/instance.h. Kept opaque here so
 * non-vulkan TUs don't need to pull <vulkan/vulkan.h>. */
struct lagfx_vk_state;

/* Magic cookie for liveness / sanity checks; ASCII "LAGX". */
#define LAGFX_DEVICE_MAGIC   0x4C414758u
#define LAGFX_DISPLAY_MAGIC  0x4C414744u  /* "LAGD" */

/* Hard cap on simultaneously attached displays per device. Apple's
 * PGDevice permits multiple (Pro Display XDR mirroring etc.) but in
 * practice Phase 1 only ever wires one. Grow as needed. */
#define LAGFX_MAX_DISPLAYS 4

struct lagfx_device {
    uint32_t magic;                       /* LAGFX_DEVICE_MAGIC */
    lagfx_device_descriptor_t desc;       /* copied from caller */

    /* Effective MMIO region size (desc.mmio_region_size or default). */
    size_t mmio_region_size;

    /* Registered displays. NULL slots indicate free. */
    lagfx_display_t *displays[LAGFX_MAX_DISPLAYS];
    size_t display_count;

    /* Per-display shared-state page GPAs, set by vchan_setup_shared_state.
     * Used by the vblank timer to write pending_mask bits. */
    uint64_t display_ss_gpa[16];
    uint32_t display_ss_installed;        /* bitmask of installed displays */
    uint32_t display_ss_enabled;          /* bitmask of displays where ss[+0x104]==0xC (enable() called) */

    /* Protocol state (Phase 1.A.2) and Vulkan state (Phase 1.B). */
    void *protocol_state;
    struct lagfx_vk_state *vk;
};

/* Called by display.c when a display attaches/detaches. Returns 0
 * on success, negative lagfx_status_t on failure. */
int lagfx_device_attach_display(lagfx_device_t *device,
                                lagfx_display_t *display);
void lagfx_device_detach_display(lagfx_device_t *device,
                                 lagfx_display_t *display);

/* Validate a device handle — returns true if plausibly live. */
static inline int lagfx_device_is_valid(const lagfx_device_t *d) {
    return d != NULL && d->magic == LAGFX_DEVICE_MAGIC;
}

#endif /* LIBAPPLEGFX_DEVICE_INTERNAL_H */
