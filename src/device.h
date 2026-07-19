/*
 * libapplegfx-vulkan — internal device state
 * src/device.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Not installed. Only other translation units inside the library
 * should include this (display.c reaches into struct lagfx_device
 * to register itself against the owning device).
 */

#ifndef LIBAPPLEGFX_DEVICE_INTERNAL_H
#define LIBAPPLEGFX_DEVICE_INTERNAL_H

#include "libapplegfx-vulkan.h"
#include "protocol/ops_display.h"  /* lagfx_cursor_show_state_t / _glyph_state_t */

#include <stddef.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

/* Forward decl; defined in src/vulkan/instance.h. Kept opaque here so
 * non-vulkan TUs don't need to pull <vulkan/vulkan.h>. */
struct lagfx_vk_state;

/* Magic cookie for liveness / sanity checks; ASCII "LAGX". */
#define LAGFX_DEVICE_MAGIC   0x4C414758u
#define LAGFX_DISPLAY_MAGIC  0x4C414744u  /* "LAGD" */

/* Minimal logging stubs - defined in device.c to avoid circular includes */
extern void lagfx_log_impl(const char *fmt, ...);
extern void lagfx_warn_impl(const char *fmt, ...);

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

    /* Doorbell callback — QEMU calls this when BAR0+0x1020 is written */
    void (*doorbell_callback)(void *opaque, uint8_t chan_id);
    void *doorbell_opaque;

    /* === Cursor state ==============================================
     * macOS publishes ONE cursor across all attached displays — the
     * user drags it between monitors, the cursor object is shared.
     * Reflect that on the device, not on each display: the cursor
     * handlers (0x13 CmdDisplayCursorShow, 0x14 CmdDisplayCursorGlyph)
     * stamp the device's `cursor_show` / `cursor_glyph` and every
     * display's clear-color submit reads from there.
     *
     * Pre-fix these lived as file-statics in display.c — refactor
     * scar from when ops_display.c was the only writer. Promoting
     * onto lagfx_device_t makes Stage 25 cursor work (real glyph
     * upload + dpy_mouse_set) tractable without re-introducing
     * cross-translation-unit globals. */
    lagfx_cursor_show_state_t  cursor_show;
    lagfx_cursor_glyph_state_t cursor_glyph;

    /* Stage 65d Option 3: bundled triangle shaders, used as
     * substitute for every runtime SetRenderPipelineState until
     * proper metallib capture lands. */
#ifdef LAGFX_HAVE_VULKAN
    VkShaderModule triangle_vertex_module;     /* VK_NULL_HANDLE if not loaded */
    VkShaderModule triangle_fragment_module;   /* VK_NULL_HANDLE if not loaded */
#endif
};

/* Validate a device handle — returns true if plausibly live. */
static inline int lagfx_device_is_valid(const lagfx_device_t *d) {
    return d != NULL && d->magic == LAGFX_DEVICE_MAGIC;
}

/* Doorbell write handler — called by QEMU when BAR0+0x1020 is written */
static inline void lagfx_device_doorbell_write(lagfx_device_t *dev, uint8_t chan_id) {
    if (!lagfx_device_is_valid(dev)) return;

    lagfx_log_impl("doorbell_write: device=%p channel=%u", (void *)dev, chan_id);

    /* Call back to registered doorbell handler */
    if (dev->doorbell_callback && dev->doorbell_opaque) {
        dev->doorbell_callback(dev->doorbell_opaque, chan_id);
    } else {
        lagfx_warn_impl("doorbell_write: no callback registered for device %p", (void *)dev);
    }
}

/* Called by display.c when a display attaches/detaches. Returns 0
 * on success, negative lagfx_status_t on failure. */
int lagfx_device_attach_display(lagfx_device_t *device,
                                lagfx_display_t *display);
void lagfx_device_detach_display(lagfx_device_t *device,
                                 lagfx_display_t *display);

/* Cursor state accessors — return a pointer into the device's
 * cursor_show / cursor_glyph fields. Safe to call with dev==NULL
 * (returns a zero-initialised static). Pointers are valid for the
 * life of the device. */
const lagfx_cursor_show_state_t  *lagfx_device_last_cursor_show(const lagfx_device_t *dev);
const lagfx_cursor_glyph_state_t *lagfx_device_last_cursor_glyph(const lagfx_device_t *dev);

#endif /* LIBAPPLEGFX_DEVICE_INTERNAL_H */
