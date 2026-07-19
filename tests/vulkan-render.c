/*
 * libapplegfx-vulkan — render target clear + readback smoke (Phase 2.B)
 * tests/vulkan-render.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exercises src/vulkan/render_target.c end-to-end:
 *   1. Allocate a 64x64 BGRA8 render target.
 *   2. Begin a one-shot cmdbuf; record vkCmdBeginRendering with a
 *      clear-load of (1.0, 0.0, 0.0, 1.0) ("red"); end rendering;
 *      end + submit + fence-wait.
 *   3. Read back pixels via lagfx_vk_render_target_readback into a
 *      host buffer.
 *   4. Assert the pixel at (32,32) is (0,0,255,255) in BGRA byte
 *      order — B=0, G=0, R=255, A=255. That byte order is what
 *      VK_FORMAT_B8G8R8A8_UNORM produces on a little-endian x86
 *      or aarch64 host.
 *
 * Registered by tests/meson.build only when vulkan_dep was present at
 * configure time. At runtime, on hosts without a loadable ICD (typical
 * Darwin dev machine) we emit SKIP rather than failing — matches the
 * behaviour of tests/vulkan-init.c and tests/vulkan-command.c.
 */

#include "libapplegfx-vulkan.h"

#include "device.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/render_target.h"

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

/* Stubs — none should fire in a render-target unit test. */
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
    ddesc.shell.opaque          = (void *)0xfeedu;
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
/* Render a 64x64 red-clear into the target and read it back. Returns 0
 * on success; any CHECK failures go through the global counter. */
static int render_and_readback(struct lagfx_vk_state *vk) {
    lagfx_vk_render_target_t rt;
    lagfx_status_t st = lagfx_vk_render_target_create(
        vk, 64u, 64u, VK_FORMAT_B8G8R8A8_UNORM, &rt);
    CHECK(st == LAGFX_OK, "render_target_create(64x64 BGRA8)");
    if (st != LAGFX_OK) {
        return 1;
    }
    CHECK(rt.image != VK_NULL_HANDLE, "render target has non-null image");
    CHECK(rt.view  != VK_NULL_HANDLE, "render target has non-null view");
    CHECK(rt.memory != VK_NULL_HANDLE, "render target has non-null memory");
    CHECK(rt.width  == 64u && rt.height == 64u,
          "render target has requested dimensions");

    /* Record the clear into a one-shot primary cmdbuf. */
    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    CHECK(cb_st == LAGFX_OK && cb != VK_NULL_HANDLE,
          "cmdbuf_alloc for clear");
    if (cb == VK_NULL_HANDLE) {
        lagfx_vk_render_target_destroy(vk, &rt);
        return 1;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    CHECK(vr == VK_SUCCESS, "vkBeginCommandBuffer");

    float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    lagfx_status_t clear_st = lagfx_vk_render_clear_color(vk, cb, &rt, red);
    CHECK(clear_st == LAGFX_OK, "render_clear_color records OK");

    vr = vkEndCommandBuffer(cb);
    CHECK(vr == VK_SUCCESS, "vkEndCommandBuffer");

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    CHECK(vr == VK_SUCCESS, "vkCreateFence");

    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cb,
    };
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    CHECK(vr == VK_SUCCESS, "vkQueueSubmit(clear)");
    const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    CHECK(vr == VK_SUCCESS, "vkWaitForFences(clear)");
    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    /* Read back + verify. */
    size_t buf_bytes = 64u * 64u * 4u;
    uint8_t *pixels = (uint8_t *)malloc(buf_bytes);
    CHECK(pixels != NULL, "readback buffer malloc");
    if (!pixels) {
        lagfx_vk_render_target_destroy(vk, &rt);
        return 1;
    }

    size_t stride = 0;
    lagfx_status_t rb_st = lagfx_vk_render_target_readback(
        vk, &rt, pixels, buf_bytes, &stride);
    CHECK(rb_st == LAGFX_OK, "render_target_readback returns LAGFX_OK");
    CHECK(stride == 64u * 4u, "readback stride == width*4");

    /* Pixel at (32, 32): offset = y*stride + x*4. In BGRA8Unorm on a
     * little-endian host the byte order at that offset is B,G,R,A. */
    size_t off = (size_t)32u * stride + (size_t)32u * 4u;
    CHECK(pixels[off + 0] == 0u,   "pixel (32,32) B == 0");
    CHECK(pixels[off + 1] == 0u,   "pixel (32,32) G == 0");
    CHECK(pixels[off + 2] == 255u, "pixel (32,32) R == 255");
    CHECK(pixels[off + 3] == 255u, "pixel (32,32) A == 255");

    free(pixels);
    lagfx_vk_render_target_destroy(vk, &rt);
    CHECK(rt.image == VK_NULL_HANDLE,
          "render_target_destroy clears image handle");
    return 0;
}
#endif

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan render target smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    char *err = NULL;
    lagfx_device_t *dev = make_device(&err);

    if (!dev) {
        fprintf(stdout, "device_new returned NULL (err=%s)\n",
                err ? err : "<none>");
        free(err);
        SKIP("no loadable Vulkan ICD on this host — "
             "vulkan-render runtime path unavailable");
        fprintf(stdout, "\n=== Summary: %d skipped, 0 failed ===\n",
                g_skipped);
        return 0;
    }

#ifdef LAGFX_HAVE_VULKAN
    if (dev->vk && dev->vk->initialized) {
        (void)render_and_readback(dev->vk);
    } else {
        SKIP("LAGFX_HAVE_VULKAN set but dev->vk->initialized is false");
    }
#else
    SKIP("library built without Vulkan — render target path is stubbed");
#endif

    lagfx_device_free(dev);
    CHECK(1, "teardown completes");

    fprintf(stdout, "\n=== Summary: %s (%d skipped) ===\n",
            g_fail ? "FAILURES" : "ALL GOOD", g_skipped);
    return g_fail ? 1 : 0;
}
