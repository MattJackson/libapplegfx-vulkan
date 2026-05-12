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
#include "doorbell.h"
#include "display.h"
#include "vulkan/instance.h"
#include "shaders/catalog.h"
#include "common/log.h"
#include "protocol/state.h"
#include "doorbell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* MSIX range (4 KB) */
#define LAGFX_MSIX_RANGE_END 0x1000u

/* Helper: map BAR0 offset to register array index (0..15 for 0x1000..0x1FFF) */
static inline int lagfx_reg_index(uint64_t offset) {
    if (offset < LAGFX_MSIX_RANGE_END || offset >= 0x2000) return -1;
    int idx = ((int)(offset & 0xFFF)) / 4;
    return (idx >= 0 && idx < 16) ? idx : -1;
}

/* Register shadow for MMIO reads/writes */
static uint32_t g_reg_shadow[16];

uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset) {
    if (!lagfx_device_is_valid(device)) {
        return 0;
    }

    /* MSI-X table — shell owns it */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return 0;
    }

    int idx = lagfx_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_read: unmapped offset 0x%llx", (unsigned long long)offset);
        return 0;
    }

    uint32_t value = g_reg_shadow[idx];

    /* Special handling for status bits */
    if (offset == 0x1018u && device->protocol_state) {
        /* Stamp bitmask - xchg-and-clear semantics */
        lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
        uint32_t mask = p->pending_stamps_bitmask;
        p->pending_stamps_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1018 stamp_bitmask -> 0x%x", mask);
        return mask;
    }

    if (offset == 0x1014u && device->protocol_state) {
        /* Display bitmask - xchg-and-clear */
        lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
        uint32_t mask = p->pending_displays_bitmask;
        p->pending_displays_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1014 display_bitmask -> 0x%x", mask);
        return mask;
    }

    /* Capability gate */
    if (offset == 0x122cu) {
        return 9u;  /* Modern paravirt path */
    }

    LAGFX_TRACE("mmio_read: off=0x%llx idx=%d -> 0x%08x",
              (unsigned long long)offset, idx, value);
    return value;
}

void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset, uint32_t value) {
    if (!lagfx_device_is_valid(device)) {
        return;
    }

    /* MSI-X range — shell's problem */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return;
    }

    int idx = lagfx_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_write: unmapped offset 0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    /* Shadow the write */
    g_reg_shadow[idx] = value;

    LAGFX_TRACE("mmio_write: off=0x%llx idx=%d val=0x%08x",
              (unsigned long long)offset, idx, value);

    /* Primary-ring MMIO handlers */
    switch (idx) {
        case 0:  /* STATUS_CONTROL @ 0x1000 */
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->ring_armed = (value != 0u);
            }
            return;

        case 1:  /* ring_size @ 0x1004 */
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->ring_size = value ? value : 0x10000u;
            }
            return;

        case 2:  /* doorbell @ 0x1008 - route via doorbell.c */
            if (device->protocol_state) {
                LAGFX_LOG("doorbell: primary ring wp=0x%x → doorbell_dispatch", value);
            }
            /* Delegate to doorbell.c for dispatcher routing */
            doorbell_dispatch(device->protocol_state, DOOR_PRIMARY_RING, value);
            return;

        case 4:  /* ring_start_offset @ 0x1010 */
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->ring_start_offset = value;
                p->page_size = 0x1000u;
                p->ring_base_gpa = ((uint64_t)p->ring_base_pfn << 12) + p->ring_start_offset;
            }
            return;

        case 5:  /* ring_shared_page_pfn @ 0x101c */
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->ring_shared_page_pfn = value;
            }
            return;

        case 7:  /* ring_base_pfn @ 0x1030 */
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->ring_base_pfn = value;
                p->ring_base_gpa = ((uint64_t)value << 12) + p->ring_start_offset;
                if (p->ring_size == 0u) {
                    p->ring_size = 0x10000u;
                }
            }
            return;

        case 8:  /* doorbell @ 0x1020 - route via doorbell.c */
        {
            if (device->protocol_state) {
                lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
                p->current_chan_id = value;
                
                LAGFX_LOG("doorbell @ 0x1020: ch=%u → doorbell_dispatch", value);
            }
            
            /* Delegate to doorbell.c for dispatcher routing (with protocol state) */
            doorbell_dispatch(device->protocol_state, DOOR_CHANNEL, value);
            return;
        }

        default: break;
    }
}

/* Minimal logging stubs for device.h macros */
void lagfx_log_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "LAGFX [device] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void lagfx_warn_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "LAGFX [device WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

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
 * Phase 1.B hooks lagfx_vk_init() (which calls vkCreateInstance) into
 * lagfx_device_new. The ordering below is load-bearing: this function
 * MUST run before lagfx_vk_init, not after.
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
     * env var is consistent even if the allocation later fails.
     *
     * Ordering note (Phase 1.B): this setenv MUST precede the
     * lagfx_vk_init call below. Mesa lavapipe reads LP_NUM_THREADS at
     * ICD init triggered by the first Vulkan call in the process —
     * and vkCreateInstance inside lagfx_vk_init is exactly that first
     * call on a fresh lavapipe worker pool. Inverting the two lines
     * would silently lose the gpu_cores setting the first time through
     * and leave later devices stuck on whatever the first one picked. */
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
      * shadow and ring geometry. */
     /* Protocol lifecycle removed - legacy protocol.c deleted */
     /* dev->protocol_state = lagfx_protocol_new(dev); */
     dev->protocol_state = NULL;

    /* Initialize STATUS_CONTROL (0x1000) to non-zero "FIFO enabled" value
     * for Phase 1.A. tests that expect decoder to be live even without
     * protocol state attached yet. Value of 1 = FIFO armed/enabled. */
     g_reg_shadow[0] = 1u;

 /* Phase 1.B: Vulkan instance + device + queue. In no-vulkan builds
     * this is a no-op that still returns LAGFX_OK with a tiny placeholder
     * state (see src/vulkan/instance.c). We still surface real Vulkan
     * init failures as LAGFX_ERR_BACKEND so the shell can route them to
     * a clean error path rather than a cryptic later-stage crash. */
    lagfx_status_t vk_st = lagfx_vk_init(&dev->vk, desc);
    if (vk_st != LAGFX_OK) {
        set_err(errp_out, "lagfx_device_new: Vulkan backend init failed");
        /* Protocol lifecycle removed - legacy protocol.c deleted */
        /* Log the intended error class + underlying status so a
         * consumer without errp_out can still see why we failed. */
        LAGFX_ERR("device_new: lagfx_vk_init failed (status=%d) -> "
                  "LAGFX_ERR_BACKEND (%d)", (int)vk_st,
                  (int)LAGFX_ERR_BACKEND);
        memset(dev, 0, sizeof(*dev));
        free(dev);
        return NULL;
    }

    /* Phase 3.C scaffold: pre-register the stock shader catalog. This
     * currently only logs; Phase 3.E promotes it to a real
     * VkShaderEXT pre-creation pass per
     * paravirt-re/phase-3-metal-vulkan-plan.md §3.E. A failure here
     * does NOT unwind the device — the catalog is a scaffold and
     * downstream consumers fall back cleanly (shader-catalog-plan.md
     * §7). */
    lagfx_status_t cat_st = lagfx_device_register_shader_catalog(dev);
    if (cat_st != LAGFX_OK) {
        LAGFX_WARN("device_new: shader catalog registration returned %d "
                   "(non-fatal, continuing)",
                   (int)cat_st);
    }

    LAGFX_LOG("device_new: dev=%p mmio_size=%zu shell_opaque=%p vk=%p",
              (void *)dev, dev->mmio_region_size, dev->desc.shell.opaque,
              (void *)dev->vk);

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

    /* Protocol lifecycle removed - legacy protocol.c deleted */

    /* Tear down Vulkan state (safe on NULL; in no-vulkan builds this
     * just free()s the placeholder struct). */
    if (device->vk) {
        lagfx_vk_shutdown(device->vk);
        device->vk = NULL;
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
    /* Protocol reset moved to protocol layer - removed legacy call */
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
