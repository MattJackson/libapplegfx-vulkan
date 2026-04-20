/*
 * libapplegfx-vulkan — device lifecycle (Phase 1.A.1 no-op object)
 * src/device.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements lagfx_device_new / _free / _reset as heap-allocated
 * no-op state. No Vulkan, no protocol decoding; those are wired in
 * Phase 1.A.2 and Phase 1.B. This is the minimum needed for the
 * mos-qemu apple-gfx-pci-linux device to link and call into
 * libapplegfx without crashing.
 */

#include "device.h"
#include "display.h"
#include "protocol/protocol.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default MMIO region size matches Apple's layout:
 *   0x0000..0x0FFF   MSI-X vector table (4 KB)
 *   0x1000..0x3FFF   registers / doorbells (12 KB)
 * Total 16 KB. Shell may override via descriptor.mmio_region_size. */
#define LAGFX_MMIO_DEFAULT_SIZE 0x4000u

/* Helper: allocate and return a formatted error string into *errp_out
 * (if non-NULL). Uses malloc so callers free(). Keeps the lib free of
 * glib so we don't force that dep on consumers. */
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

/* Apply thread_count via LP_NUM_THREADS env var for Mesa lavapipe.
 *
 * Mesa reads LP_NUM_THREADS exactly once, at ICD-init time (triggered by
 * the first Vulkan call in the process — typically vkCreateInstance).
 * After that point, changes are invisible to the already-initialized
 * worker pool. Therefore this MUST be called before the first Vulkan
 * call in the process.
 *
 * In Phase 1.A.1 lagfx_device_new does not yet touch Vulkan, so the
 * ordering constraint is trivially satisfied; it remains correct when
 * Phase 1.B wires vkCreateInstance here.
 *
 * n == 0 is the "unset" sentinel — we leave LP_NUM_THREADS untouched so
 * any user/system-provided value (or lavapipe's default of host-core-
 * count) wins. */
static void lagfx_apply_thread_count_env(uint32_t n) {
    if (n == 0) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", n);
    /* overwrite=1: descriptor request takes priority over environment. */
    if (setenv("LP_NUM_THREADS", buf, 1) != 0) {
        LAGFX_WARN("thread_count: setenv(LP_NUM_THREADS=%s) failed", buf);
        return;
    }
    LAGFX_LOG("thread_count: LP_NUM_THREADS=%s", buf);
}

lagfx_device_t *lagfx_device_new(const lagfx_device_descriptor_t *desc,
                                  char **errp_out) {
    if (!desc) {
        set_err(errp_out, "lagfx_device_new: desc is NULL");
        return NULL;
    }

    /* Apply LP_NUM_THREADS BEFORE any Vulkan call. Must run on every
     * lagfx_device_new path, including error returns below, so that the
     * env var is consistent even if the allocation later fails. */
    lagfx_apply_thread_count_env(desc->thread_count);

    lagfx_device_t *dev = (lagfx_device_t *)calloc(1, sizeof(*dev));
    if (!dev) {
        set_err(errp_out, "lagfx_device_new: out of memory");
        return NULL;
    }

    dev->magic = LAGFX_DEVICE_MAGIC;
    dev->desc  = *desc;  /* shallow copy; callbacks are function pointers */

    dev->mmio_region_size = desc->mmio_region_size
                              ? desc->mmio_region_size
                              : LAGFX_MMIO_DEFAULT_SIZE;

    /* Phase 1.A.2: attach the protocol decoder. It owns the MMIO
     * register shadow (0x1000..0x1028), doorbell dispatch, and the
     * opcode jump table. If it fails to allocate we unwind. */
    dev->protocol_state = lagfx_protocol_new(dev);
    if (!dev->protocol_state) {
        set_err(errp_out, "lagfx_device_new: protocol decoder alloc failed");
        memset(dev, 0, sizeof(*dev));
        free(dev);
        return NULL;
    }

    /* Phase 1.B+: init Vulkan instance/device here. */
    dev->vulkan_state = NULL;

    LAGFX_LOG("device_new: dev=%p mmio_size=%zu shell_opaque=%p",
              (void *)dev, dev->mmio_region_size, dev->desc.shell.opaque);

    return dev;
}

void lagfx_device_free(lagfx_device_t *device) {
    if (!device) {
        return;
    }
    if (device->magic != LAGFX_DEVICE_MAGIC) {
        LAGFX_ERR("device_free: bad magic on %p (got 0x%08x)",
                  (void *)device, device->magic);
        return;
    }

    /* Detach any displays still attached. They remain owned by the
     * caller; we just drop our pointer. display_free handles the
     * reverse direction via lagfx_device_detach_display. */
    for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
        if (device->displays[i]) {
            LAGFX_WARN("device_free: display %p still attached at slot %zu",
                       (void *)device->displays[i], i);
            device->displays[i]->device = NULL;
            device->displays[i] = NULL;
        }
    }

    /* Tear down the protocol decoder before wiping state. */
    if (device->protocol_state) {
        lagfx_protocol_free((lagfx_protocol_t *)device->protocol_state);
        device->protocol_state = NULL;
    }

    LAGFX_LOG("device_free: dev=%p", (void *)device);

    /* Zero before free so use-after-free is noisy. */
    memset(device, 0, sizeof(*device));
    free(device);
}

void lagfx_device_reset(lagfx_device_t *device) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("device_reset: invalid device %p", (void *)device);
        return;
    }

    LAGFX_LOG("device_reset: dev=%p", (void *)device);

    /* Drain decoder tables + inflight state; registers stay. */
    if (device->protocol_state) {
        lagfx_protocol_reset((lagfx_protocol_t *)device->protocol_state);
    }
    /* Phase 1.B+: re-init GPU state. */
}

/* === Internal display attach/detach ============================ */

int lagfx_device_attach_display(lagfx_device_t *device,
                                lagfx_display_t *display) {
    if (!lagfx_device_is_valid(device) || !display) {
        return LAGFX_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
        if (!device->displays[i]) {
            device->displays[i] = display;
            device->display_count++;
            return LAGFX_OK;
        }
    }
    return LAGFX_ERR_INTERNAL;  /* no free slot */
}

void lagfx_device_detach_display(lagfx_device_t *device,
                                 lagfx_display_t *display) {
    if (!lagfx_device_is_valid(device) || !display) {
        return;
    }
    for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
        if (device->displays[i] == display) {
            device->displays[i] = NULL;
            if (device->display_count > 0) {
                device->display_count--;
            }
            return;
        }
    }
}
