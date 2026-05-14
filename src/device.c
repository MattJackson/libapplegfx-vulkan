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
#include "protocol/state.h"  /* lagfx_protocol_t typedef */
#include "vulkan/instance.h"
#include "shaders/catalog.h"
#include "common/log.h"

/* Forward decl for protocol lifecycle functions (defined in lifecycle.c) */
lagfx_protocol_t *lagfx_protocol_new(struct lagfx_device *dev);
void lagfx_protocol_free(lagfx_protocol_t *p);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/* MSIX range (4 KB) — shell owns it; we never see writes below this. */
#define LAGFX_MSIX_RANGE_END 0x1000u

/* === MMIO shims =================================================
 *
 * Thin pass-throughs to the unified doorbell entry point. ALL inbound
 * register/doorbell traffic flows through doorbell_handle_write /
 * doorbell_handle_read so we have one place to dispatch, log, and
 * surface unhandled offsets. If you need to add a new register, do it
 * in doorbell.c — do NOT special-case it here.
 */

uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset) {
    if (!lagfx_device_is_valid(device)) return 0;
    if (offset < LAGFX_MSIX_RANGE_END) return 0;  /* MSI-X — shell owns */
    return doorbell_handle_read(device->protocol_state, offset);
}

void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset, uint32_t value) {
    if (!lagfx_device_is_valid(device)) return;
    if (offset < LAGFX_MSIX_RANGE_END) return;  /* MSI-X — shell owns */
    doorbell_handle_write(device->protocol_state, offset, value);
}

/* Global log file handle — opened lazily on first write */
static FILE *lagfx_log_file = NULL;

/* Default log-file path. Override by setting LAGFX_LOG_FILE in the
 * environment; mirrors the LAGFX_LOG_LEVEL pattern from common/log.h.
 * Used by the test harness to redirect lagfx output to a per-test
 * file, and by ops who want to capture into a long-lived /var/log
 * location instead of /tmp. */
#define LAGFX_LOG_FILE_DEFAULT "/tmp/lagfx.log"

/* Internal logging function — single source of truth for all lagfx logging.
 * Opens the configured log file once, flushes after every write to ensure
 * real-time visibility in Docker logs. Falls back to stderr if file open
 * fails. */
static void lagfx_log_internal(const char *prefix, const char *fmt, va_list args) {
    /* Lazy-open log file on first write */
    if (!lagfx_log_file) {
        const char *path = getenv("LAGFX_LOG_FILE");
        if (!path || path[0] == '\0') {
            path = LAGFX_LOG_FILE_DEFAULT;
        }
        lagfx_log_file = fopen(path, "a");
        if (!lagfx_log_file) {
            /* Fallback to stderr if we can't open file */
            lagfx_log_file = stderr;
        }
    }

    /* Self-test startup banner — emitted exactly once, before the
     * triggering log line. Bypasses level gates by design: this is a
     * sanity check that every channel is wired, not gated content. If
     * a level goes missing after a config change, the banner at the
     * top of the file makes the regression obvious. */
    static int banner_emitted = 0;
    if (!banner_emitted) {
        const char *path = getenv("LAGFX_LOG_FILE");
        if (!path || path[0] == '\0') {
            path = LAGFX_LOG_FILE_DEFAULT;
        }
        banner_emitted = 1;
        fprintf(lagfx_log_file,
                "LAGFX [INIT ] pid=%d level=%s build=%s — %s opened\n"
                "LAGFX [ERROR] startup-banner channel test\n"
                "LAGFX [WARN ] startup-banner channel test\n"
                "LAGFX [INFO ] startup-banner channel test\n"
                "LAGFX [trace] startup-banner channel test (silent unless LAGFX_LOG_LEVEL=trace)\n",
                (int)getpid(), lagfx_level_name(),
                __DATE__ " " __TIME__, path);
        fflush(lagfx_log_file);
    }

    va_list args_copy;
    va_copy(args_copy, args);
    fprintf(lagfx_log_file, "LAGFX [%s] ", prefix);
    vfprintf(lagfx_log_file, fmt, args_copy);
    fprintf(lagfx_log_file, "\n");
    fflush(lagfx_log_file);  /* Ensure immediate flush for real-time visibility */
    va_end(args_copy);
}

void lagfx_log_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    lagfx_log_internal("INFO", fmt, args);
    va_end(args);
}

void lagfx_warn_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    lagfx_log_internal("WARN", fmt, args);
    va_end(args);
}

void lagfx_err_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    lagfx_log_internal("ERROR", fmt, args);
    va_end(args);
}

void lagfx_trace_impl(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    lagfx_log_internal("trace", fmt, args);
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
    lagfx_protocol_t *proto = lagfx_protocol_new(dev);
    if (!proto) {
        set_err(errp_out, "lagfx_device_new: protocol allocation failed");
        memset(dev, 0, sizeof(*dev));
        free(dev);
        return NULL;
    }
    dev->protocol_state = proto;

    /* Hand off register-shadow ownership to doorbell.c. This sets
     * STATUS_CONTROL (0x1000) to 1 so the decoder reads back as
     * "FIFO armed/enabled" from the very first lagfx_mmio_read,
     * matching Phase 1.A lifecycle-smoke expectations. The shadow
     * lives entirely inside doorbell.c — device.c does not touch it. */
    doorbell_init(proto);

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

    /* Release protocol state. Allocated in lagfx_device_new via
     * lagfx_protocol_new; mirror the lifetime here so ASAN doesn't
     * report a leak on device teardown. */
    if (device->protocol_state) {
        lagfx_protocol_free((lagfx_protocol_t *)device->protocol_state);
        device->protocol_state = NULL;
    }

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
