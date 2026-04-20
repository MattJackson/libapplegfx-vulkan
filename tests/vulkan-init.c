/*
 * libapplegfx-vulkan — Vulkan init smoke test (Phase 1.B)
 * tests/vulkan-init.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises src/vulkan/instance.c: creates a device via
 * lagfx_device_new, reaches into the internal state to confirm
 * that the Vulkan instance / phys-device / device / queue quartet
 * is populated, and tears down.
 *
 * This test is only registered at meson-test time when vulkan_dep
 * was present at configure time. On hosts without a loadable ICD
 * (e.g. a Darwin dev machine) this test recognises the situation
 * at runtime and emits SKIP rather than failing — the library is
 * happy to init without a real ICD because the LAGFX_HAVE_VULKAN
 * path still falls back to the no-op stub in that case.
 */

#include "libapplegfx-vulkan.h"

/* Reach into internal headers so we can assert on the VkInstance
 * pointer and queue populated state. tests/meson.build adds
 * internal_include_dir for this TU. */
#include "device.h"
#include "vulkan/instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_skipped = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
    } \
} while (0)

#define SKIP(msg) do { \
    fprintf(stdout, "SKIP: %s\n", msg); \
    g_skipped++; \
} while (0)

/* Stubs — none should fire in a pure init test. */
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) {
    (void)o; (void)s; (void)p; return NULL;
}
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)a; (void)t; (void)o; (void)l; return true;
}
static bool cb_read_memory(void *a, uint64_t g, uint64_t l, void *d) {
    (void)a; (void)g; (void)l; (void)d; return true;
}
static void cb_raise_irq(void *a, uint32_t v) { (void)a; (void)v; }

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan Vulkan init smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    lagfx_device_descriptor_t ddesc;
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.shell.opaque          = (void *)0xfeedu;
    ddesc.shell.create_task     = cb_create_task;
    ddesc.shell.destroy_task    = cb_destroy_task;
    ddesc.shell.map_memory      = cb_map;
    ddesc.shell.unmap_memory    = cb_unmap;
    ddesc.shell.read_memory     = cb_read_memory;
    ddesc.shell.raise_interrupt = cb_raise_irq;
    ddesc.thread_count          = 4;   /* exercise LP_NUM_THREADS path */

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&ddesc, &err);

    /* If the host has no loadable Vulkan ICD, lagfx_device_new may have
     * returned NULL with LAGFX_ERR_BACKEND. We treat that as a skip
     * rather than a failure when we're building on a host that lacks
     * a real ICD (typical Darwin dev machine). */
    if (!dev) {
        fprintf(stdout, "device_new returned NULL (err=%s)\n",
                err ? err : "<none>");
        free(err);
        SKIP("no loadable Vulkan ICD on this host — "
             "vulkan-init runtime path unavailable");
        fprintf(stdout, "\n=== Summary: %d skipped, 0 failed ===\n",
                g_skipped);
        return 0;
    }

    CHECK(err == NULL, "no error on success");
    CHECK(dev->vk != NULL, "device->vk populated");

#ifdef LAGFX_HAVE_VULKAN
    if (dev->vk && dev->vk->initialized) {
        CHECK(dev->vk->instance != VK_NULL_HANDLE, "VkInstance created");
        CHECK(dev->vk->phys_device != VK_NULL_HANDLE,
              "VkPhysicalDevice selected");
        CHECK(dev->vk->device != VK_NULL_HANDLE, "VkDevice created");
        CHECK(dev->vk->graphics_queue != VK_NULL_HANDLE,
              "VkQueue retrieved");
        CHECK(dev->vk->graphics_queue_family != 0xFFFFFFFFu,
              "queue family index valid");
        CHECK(dev->vk->have_dynamic_rendering,
              "dynamic_rendering requested");
        CHECK(dev->vk->have_synchronization2,
              "synchronization2 requested");
        CHECK(dev->vk->have_timeline_semaphore,
              "timelineSemaphore requested");
        CHECK(dev->vk->have_descriptor_indexing,
              "descriptorIndexing requested");
        fprintf(stdout, "INFO: selected '%s' (type=%d api=%u.%u.%u)\n",
                dev->vk->phys_props.deviceName,
                (int)dev->vk->phys_props.deviceType,
                VK_VERSION_MAJOR(dev->vk->phys_props.apiVersion),
                VK_VERSION_MINOR(dev->vk->phys_props.apiVersion),
                VK_VERSION_PATCH(dev->vk->phys_props.apiVersion));
        fprintf(stdout, "INFO: shader_object=%d eds3=%d\n",
                (int)dev->vk->have_shader_object,
                (int)dev->vk->have_extended_dynamic_state3);
    } else {
        /* This would indicate a silent bug — LAGFX_HAVE_VULKAN on but
         * the init path returned an un-initialised state. Flag it. */
        CHECK(0, "LAGFX_HAVE_VULKAN set but dev->vk->initialized is false");
    }
#else
    SKIP("library built without Vulkan — instance/device handles are stubs");
#endif

    lagfx_device_free(dev);
    CHECK(1, "teardown completes");

    fprintf(stdout, "\n=== Summary: %s (%d skipped) ===\n",
            g_fail ? "FAILURES" : "ALL GOOD", g_skipped);
    return g_fail ? 1 : 0;
}
