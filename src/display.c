/*
 * libapplegfx-vulkan — display lifecycle (Phase 1.A.1 no-op object)
 * src/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Stub display: stores descriptor + callbacks, serves cursor
 * position (always 0,0 for now), and returns LAGFX_ERR_NO_FRAME
 * from read_frame. Real rendering lives in Phase 1.B.
 */

#include "device.h"
#include "display.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

static void set_err(char **errp_out, const char *msg) {
    if (!errp_out) {
        return;
    }
    size_t len = strlen(msg) + 1;
    char *buf = (char *)malloc(len);
    if (buf) {
        memcpy(buf, msg, len);
    }
    *errp_out = buf;
}

lagfx_display_t *lagfx_display_new(lagfx_device_t *device,
                                    const lagfx_display_descriptor_t *desc,
                                    uint32_t port, uint32_t serial_num,
                                    char **errp_out) {
    if (!lagfx_device_is_valid(device)) {
        set_err(errp_out, "lagfx_display_new: invalid device");
        return NULL;
    }
    if (!desc) {
        set_err(errp_out, "lagfx_display_new: desc is NULL");
        return NULL;
    }

    lagfx_display_t *disp = (lagfx_display_t *)calloc(1, sizeof(*disp));
    if (!disp) {
        set_err(errp_out, "lagfx_display_new: out of memory");
        return NULL;
    }

    disp->magic      = LAGFX_DISPLAY_MAGIC;
    disp->device     = device;
    disp->desc       = *desc;  /* shallow; modes ptr not deep-copied */
    disp->port       = port;
    disp->serial_num = serial_num;
    disp->cursor_pos = (lagfx_coord_t){ 0, 0 };
    disp->has_frame  = 0;

    int rc = lagfx_device_attach_display(device, disp);
    if (rc != LAGFX_OK) {
        set_err(errp_out,
                "lagfx_display_new: device at max display count");
        free(disp);
        return NULL;
    }

    LAGFX_LOG("display_new: disp=%p dev=%p port=%u serial=%u name=%s",
              (void *)disp, (void *)device, port, serial_num,
              desc->name ? desc->name : "(null)");

    return disp;
}

void lagfx_display_free(lagfx_display_t *display) {
    if (!display) {
        return;
    }
    if (display->magic != LAGFX_DISPLAY_MAGIC) {
        LAGFX_ERR("display_free: bad magic on %p (got 0x%08x)",
                  (void *)display, display->magic);
        return;
    }

    if (display->device) {
        lagfx_device_detach_display(display->device, display);
    }

    LAGFX_LOG("display_free: disp=%p", (void *)display);

    memset(display, 0, sizeof(*display));
    free(display);
}

lagfx_coord_t lagfx_display_cursor_position(lagfx_display_t *display) {
    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return (lagfx_coord_t){ 0, 0 };
    }
    return display->cursor_pos;
}

lagfx_status_t lagfx_display_read_frame(lagfx_display_t *display,
                                         void *dst,
                                         size_t dst_size_bytes,
                                         size_t *stride_out,
                                         bool *new_frame_out) {
    (void)dst;
    (void)dst_size_bytes;

    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Phase 1.A.1 never produces frames. Report "no frame yet"
     * so callers can keep their existing DisplaySurface. */
    if (new_frame_out) {
        *new_frame_out = false;
    }
    if (stride_out) {
        *stride_out = 0;
    }
    return LAGFX_ERR_NO_FRAME;
}
