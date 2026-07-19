/*
 * libapplegfx-vulkan — command pool + empty-submit smoke (Phase 1.B.2)
 * tests/vulkan-command.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exercises src/vulkan/command.c. Two cases:
 *
 *   test_vk_cmdbuf_alloc_free       — allocate a primary command
 *                                     buffer, free it, no crash.
 *   test_vk_submit_empty_roundtrip  — begin/end an empty primary
 *                                     buffer and submit with a fence;
 *                                     fence must signal within 1s.
 *
 * Registered by tests/meson.build only when vulkan_dep was present at
 * configure time. At runtime we tolerate the "no loadable ICD" case
 * (typical Darwin dev host) by emitting SKIP rather than failing —
 * matching the behaviour of tests/vulkan-init.c.
 *
 * For builds without LAGFX_HAVE_VULKAN the library exposes no-op stubs
 * for these entry points; we don't exercise the stubs here because the
 * test binary itself only links against a libapplegfx-vulkan that was
 * configured WITH vulkan_dep (the test is meson-gated on vulkan_dep).
 */

#include "libapplegfx-vulkan.h"

/* Reach into internal headers for the VkDevice handle + command
 * entry points. tests/meson.build adds internal_include_dir so these
 * resolve. */
#include "device.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"

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

/* Stubs — none should fire in a command-pool test. */
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

static lagfx_device_t *make_device(char **errp_out) {
    lagfx_device_descriptor_t ddesc;
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.shell.opaque          = (void *)0xcafeu;
    ddesc.shell.create_task     = cb_create_task;
    ddesc.shell.destroy_task    = cb_destroy_task;
    ddesc.shell.map_memory      = cb_map;
    ddesc.shell.unmap_memory    = cb_unmap;
    ddesc.shell.read_memory     = cb_read_memory;
    ddesc.shell.raise_interrupt = cb_raise_irq;
    ddesc.thread_count          = 2;
    return lagfx_device_new(&ddesc, errp_out);
}

#ifdef LAGFX_HAVE_VULKAN
static void test_vk_cmdbuf_alloc_free(struct lagfx_vk_state *vk) {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    CHECK(st == LAGFX_OK, "cmdbuf_alloc returns LAGFX_OK");
    CHECK(cb != VK_NULL_HANDLE, "cmdbuf_alloc returns a non-null handle");
    /* free must be safe; no return value to check. */
    lagfx_vk_cmdbuf_free(vk, cb);
    CHECK(1, "cmdbuf_free completes without crash");
}

static void test_vk_submit_empty_roundtrip(struct lagfx_vk_state *vk) {
    lagfx_status_t st = lagfx_vk_submit_empty(vk);
    CHECK(st == LAGFX_OK,
          "submit_empty round-trip (alloc/begin/end/submit/wait/free)");
}
#endif

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan command pool smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    char *err = NULL;
    lagfx_device_t *dev = make_device(&err);

    if (!dev) {
        fprintf(stdout, "device_new returned NULL (err=%s)\n",
                err ? err : "<none>");
        free(err);
        SKIP("no loadable Vulkan ICD on this host — "
             "vulkan-command runtime path unavailable");
        fprintf(stdout, "\n=== Summary: %d skipped, 0 failed ===\n",
                g_skipped);
        return 0;
    }

    CHECK(err == NULL, "no error on success");
    CHECK(dev->vk != NULL, "device->vk populated");

#ifdef LAGFX_HAVE_VULKAN
    if (dev->vk && dev->vk->initialized) {
        CHECK(dev->vk->cmd_pool != VK_NULL_HANDLE,
              "command pool created by lagfx_vk_init");

        test_vk_cmdbuf_alloc_free(dev->vk);
        test_vk_submit_empty_roundtrip(dev->vk);
    } else {
        /* LAGFX_HAVE_VULKAN is set but init fell back to un-initialized
         * state — mirror vulkan-init's handling of this case. */
        SKIP("LAGFX_HAVE_VULKAN set but dev->vk->initialized is false");
    }
#else
    SKIP("library built without Vulkan — command pool path is stubbed");
#endif

    lagfx_device_free(dev);
    CHECK(1, "teardown completes");

    fprintf(stdout, "\n=== Summary: %s (%d skipped) ===\n",
            g_fail ? "FAILURES" : "ALL GOOD", g_skipped);
    return g_fail ? 1 : 0;
}
