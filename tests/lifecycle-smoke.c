/*
 * libapplegfx-vulkan — lifecycle smoke test
 * tests/lifecycle-smoke.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises Phase 1.A.1 device/display/mmio no-op objects. No
 * Vulkan, no protocol — just: can we new/free/reset a device, attach
 * a display, hit MMIO, read the version string, without crashing?
 */

#include "libapplegfx-vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
    } \
} while (0)

/* Dummy shell callbacks — none should actually be invoked in 1.A.1. */
static lagfx_task_t *cb_create_task(void *opaque, uint64_t sz, void **out) {
    (void)opaque; (void)sz; (void)out; return NULL;
}
static void cb_destroy_task(void *opaque, lagfx_task_t *t) {
    (void)opaque; (void)t;
}
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)a; (void)t; (void)o; (void)l; return true;
}
static bool cb_read_memory(void *a, uint64_t gpa, uint64_t l, void *d) {
    (void)a; (void)gpa; (void)l; (void)d; return true;
}
static void cb_raise_irq(void *a, uint32_t v) { (void)a; (void)v; }

static void cb_frame_ready(void *opaque) { (void)opaque; }
static void cb_mode_changed(void *o, uint32_t w, uint32_t h) {
    (void)o; (void)w; (void)h;
}
static void cb_cursor_glyph(void *o, const uint8_t *px,
                            uint32_t w, uint32_t h, lagfx_coord_t hs) {
    (void)o; (void)px; (void)w; (void)h; (void)hs;
}
static void cb_cursor_moved(void *o) { (void)o; }
static void cb_cursor_show(void *o, bool s) { (void)o; (void)s; }

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan lifecycle smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());
    fprintf(stdout, "version: %d.%d.%d\n",
            lagfx_version_major(), lagfx_version_minor(),
            lagfx_version_patch());

    /* --- Version checks --- */
    CHECK(lagfx_version_major() >= 0, "version_major non-negative");
    CHECK(lagfx_version_minor() >= 0, "version_minor non-negative");
    CHECK(lagfx_version_patch() >= 0, "version_patch non-negative");
    CHECK(lagfx_build_info() != NULL && lagfx_build_info()[0] != '\0',
          "build_info non-empty");

    /* --- Null-safe free --- */
    lagfx_device_free(NULL);
    lagfx_display_free(NULL);
    CHECK(1, "free(NULL) is safe");

    /* --- Device create/destroy --- */
    lagfx_device_descriptor_t ddesc;
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.shell.opaque          = (void *)0xdeadbeefu;
    ddesc.shell.create_task     = cb_create_task;
    ddesc.shell.destroy_task    = cb_destroy_task;
    ddesc.shell.map_memory      = cb_map;
    ddesc.shell.unmap_memory    = cb_unmap;
    ddesc.shell.read_memory     = cb_read_memory;
    ddesc.shell.raise_interrupt = cb_raise_irq;
    ddesc.mmio_region_size      = 0;  /* use default */

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&ddesc, &err);
    CHECK(dev != NULL, "device_new returns non-NULL");
    CHECK(err == NULL, "no error on success");

    /* device_new(NULL) should fail gracefully. */
    char *err2 = NULL;
    lagfx_device_t *bad = lagfx_device_new(NULL, &err2);
    CHECK(bad == NULL, "device_new(NULL) returns NULL");
    CHECK(err2 != NULL, "error message set on failure");
    free(err2);

    /* --- Reset --- */
    lagfx_device_reset(dev);
    CHECK(1, "device_reset does not crash");

    /* --- Display --- */
    lagfx_display_mode_t modes[] = {
        { 1920, 1080, 60 },
        { 1280, 720, 60 },
    };
    lagfx_display_descriptor_t disp_desc;
    memset(&disp_desc, 0, sizeof(disp_desc));
    disp_desc.name                     = "smoke-display";
    disp_desc.size_mm_width            = 400;
    disp_desc.size_mm_height           = 300;
    disp_desc.modes                    = modes;
    disp_desc.mode_count               = 2;
    disp_desc.callbacks.opaque         = (void *)0xfeedfaceu;
    disp_desc.callbacks.frame_ready    = cb_frame_ready;
    disp_desc.callbacks.mode_changed   = cb_mode_changed;
    disp_desc.callbacks.cursor_glyph   = cb_cursor_glyph;
    disp_desc.callbacks.cursor_moved   = cb_cursor_moved;
    disp_desc.callbacks.cursor_show    = cb_cursor_show;

    char *derr = NULL;
    lagfx_display_t *disp = lagfx_display_new(dev, &disp_desc, 0, 1, &derr);
    CHECK(disp != NULL, "display_new returns non-NULL");
    CHECK(derr == NULL, "no error on display_new success");

    /* Cursor position is 0,0 by default. */
    lagfx_coord_t p = lagfx_display_cursor_position(disp);
    CHECK(p.x == 0 && p.y == 0, "cursor position defaults to (0,0)");

    /* read_frame must NOT crash and must report no-frame. */
    uint8_t buf[64];
    size_t stride = 42;
    bool newf = true;
    lagfx_status_t st = lagfx_display_read_frame(disp, buf, sizeof(buf),
                                                 &stride, &newf);
    CHECK(st == LAGFX_ERR_NO_FRAME, "read_frame returns NO_FRAME in 1.A.1");
    CHECK(newf == false, "new_frame_out cleared");

    /* --- MMIO stubs ---
     * Phase 1.A.2: STATUS_CONTROL (0x1000) returns a non-zero
     * "present+ready" bitmap from the decoder. An unmapped offset
     * still reads back as 0. */
    uint32_t r_status = lagfx_mmio_read(dev, 0x1000);
    CHECK(r_status != 0, "mmio_read STATUS_CONTROL is non-zero (decoder live)");
    uint32_t r_unmapped = lagfx_mmio_read(dev, 0x1030);
    CHECK(r_unmapped == 0, "mmio_read unmapped offset returns 0");
    lagfx_mmio_write(dev, 0x1004, 0xcafebabeu);
    CHECK(1, "mmio_write does not crash");

    /* Invalid device — should not crash. */
    lagfx_mmio_read(NULL, 0x1000);
    lagfx_mmio_write(NULL, 0x1000, 0);
    CHECK(1, "mmio on NULL device is safe");

    /* --- Teardown --- */
    lagfx_display_free(disp);
    lagfx_device_free(dev);
    CHECK(1, "teardown completes");

    fprintf(stdout, "\n=== Summary: %s ===\n",
            g_fail ? "FAILURES" : "ALL GOOD");
    return g_fail ? 1 : 0;
}
