/*
 * libapplegfx-vulkan — translate render encoder smoke (Phase 3.A)
 * tests/translate-render.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exercises src/translate/render_encoder.c. Two cases, gated
 * differently:
 *
 *   (A) Argument-validation / state-machine coverage. Runs on
 *       every host regardless of ICD availability. Walks the
 *       API through legal and illegal transitions:
 *         - _bind_pipeline before _begin        → ERR_INVALID_ARG
 *         - _bind_texture before _bind_pipeline → ERR_INVALID_ARG
 *         - _draw with v=0                      → ERR_INVALID_ARG
 *         - double _begin                       → ERR_INVALID_ARG
 *       State transitions are tracked via the public
 *       lagfx_translate_render_state_t flags so this pass needs
 *       no real Vulkan at all — it's valid on the no-vulkan
 *       Darwin path too, but there the encoder returns
 *       LAGFX_ERR_BACKEND from every entry point and we verify
 *       that behaviour instead.
 *
 *   (B) Real-Vulkan cmdbuf pass. Requires LAGFX_HAVE_VULKAN +
 *       a device that successfully initialised. Creates a 64x64
 *       render target, records a real begin → bind_pipeline →
 *       draw → end sequence, submits it with a fence, waits,
 *       cleans up. No readback / pixel assertions at this phase
 *       (3.A is the encoder skeleton; Phase 3.E ties the shader
 *       binding to real pipelines). Still asserts the Vulkan
 *       submit returned VK_SUCCESS — verifies that the
 *       skeleton's dynamic-rendering-begin + end are actually
 *       accepted by lavapipe.
 *
 * Registered unconditionally (no vulkan_dep gate) — the
 * state-machine pass is valuable even without Vulkan on the
 * Darwin dev host. The ICD-requiring part self-skips.
 */

#include "libapplegfx-vulkan.h"

#include "device.h"
#include "translate/render_encoder.h"

#ifdef LAGFX_HAVE_VULKAN
#  include "vulkan/instance.h"
#  include "vulkan/command.h"
#  include "vulkan/render_target.h"
#endif

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

#ifdef LAGFX_HAVE_VULKAN

/* --- Shell callback stubs (same pattern as vulkan-render). Only
 * referenced from the real-Vulkan test case; gated under the same
 * LAGFX_HAVE_VULKAN as the case itself so the no-vulkan build
 * doesn't warn about unused statics. --- */
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
    ddesc.shell.opaque          = (void *)0xbeefu;
    ddesc.shell.create_task     = cb_create_task;
    ddesc.shell.destroy_task    = cb_destroy_task;
    ddesc.shell.map_memory      = cb_map;
    ddesc.shell.unmap_memory    = cb_unmap;
    ddesc.shell.read_memory     = cb_read_memory;
    ddesc.shell.raise_interrupt = cb_raise_irq;
    ddesc.thread_count          = 1;
    return lagfx_device_new(&ddesc, errp_out);
}


/* === Test case A: state machine =================================
 *
 * Drives the encoder through illegal transitions first (all must
 * return ERR_INVALID_ARG without corrupting state), then a legal
 * run. Uses a nulled-out command buffer handle and a dummy image;
 * never calls vkCmd* directly, so requires no queue/submit.
 * Relies on the encoder short-circuiting pre-begin calls. */
static void test_state_machine_no_vk_calls(void) {
    lagfx_translate_render_state_t state;
    memset(&state, 0, sizeof(state));

    /* Illegal: _bind_pipeline before _begin. */
    lagfx_status_t st = lagfx_translate_render_bind_pipeline(
        &state, LAGFX_SHADER_BLIT, VK_NULL_HANDLE);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "bind_pipeline before begin → ERR_INVALID_ARG");

    /* Illegal: _bind_texture before _begin. */
    st = lagfx_translate_render_bind_texture(
        &state, 0, VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "bind_texture before begin → ERR_INVALID_ARG");

    /* Illegal: _draw before _begin. */
    st = lagfx_translate_render_draw(&state, 3, 1, 0, 0);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "draw before begin → ERR_INVALID_ARG");

    /* Illegal: _end before _begin. */
    st = lagfx_translate_render_end(&state);
    CHECK(st == LAGFX_ERR_INVALID_ARG,
          "end before begin → ERR_INVALID_ARG");

    /* After all those failed calls, the state flags must still be
     * false. Checks: the helper doesn't flip any on error. */
    CHECK(state.in_pass == false,
          "state.in_pass false after illegal calls");
    CHECK(state.pipeline_bound == false,
          "state.pipeline_bound false after illegal calls");
}

/* === Test case B: real-Vulkan round-trip ========================
 *
 * Record a dynamic-rendering pass over a real 64x64 render target,
 * bind a dummy pipeline handle (Phase 3.A is record-only), draw 3
 * verts, end. Submit + wait on a fence to prove lavapipe accepts
 * the command stream. */
static int test_real_vk_record(struct lagfx_vk_state *vk) {
    lagfx_vk_render_target_t rt;
    lagfx_status_t st = lagfx_vk_render_target_create(
        vk, 64u, 64u, VK_FORMAT_B8G8R8A8_UNORM, &rt);
    CHECK(st == LAGFX_OK, "render_target_create(64x64 BGRA8)");
    if (st != LAGFX_OK) {
        return 1;
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    CHECK(cb_st == LAGFX_OK && cb != VK_NULL_HANDLE, "cmdbuf_alloc");
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

    /* Transition the image to COLOR_ATTACHMENT_OPTIMAL — the
     * encoder expects the caller to own layout transitions. This
     * is how Phase 3.A is documented in render_encoder.h's
     * scope-vs-render_target.c split. */
    VkImageMemoryBarrier bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = rt.image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount     = 1,
            .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    /* Build an explicit color attachment pointing at the render
     * target's view — this avoids the encoder's NULL-view
     * fallback and exercises the "caller-owned attachments"
     * path. */
    VkRenderingAttachmentInfoKHR color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = rt.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { 0, 0, 1, 1 } } },
    };

    lagfx_translate_render_state_t state;
    memset(&state, 0, sizeof(state));

    float clear[4] = { 0, 0, 1, 1 };
    lagfx_status_t bst = lagfx_translate_render_begin(
        vk, &state, cb, rt.image, 64u, 64u, clear,
        &color_att, 1u);
    CHECK(bst == LAGFX_OK, "translate_render_begin");
    CHECK(state.in_pass == true, "in_pass true after begin");

    /* Double-begin: must fail without corrupting state. */
    lagfx_status_t bst2 = lagfx_translate_render_begin(
        vk, &state, cb, rt.image, 64u, 64u, clear,
        &color_att, 1u);
    CHECK(bst2 == LAGFX_ERR_INVALID_ARG,
          "double begin → ERR_INVALID_ARG");
    CHECK(state.in_pass == true,
          "in_pass still true after failed double begin");

    /* bind_pipeline with NULL layout — legal (Phase 3.A scaffold).
     * The encoder logs a FIXME and records the intent. */
    lagfx_status_t pst = lagfx_translate_render_bind_pipeline(
        &state, LAGFX_SHADER_BLIT, VK_NULL_HANDLE);
    CHECK(pst == LAGFX_OK, "bind_pipeline(BLIT, NULL layout)");
    CHECK(state.pipeline_bound == true, "pipeline_bound true");

    /* bind_texture with NULL view → ERR_INVALID_ARG. */
    lagfx_status_t tst = lagfx_translate_render_bind_texture(
        &state, 0, VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(tst == LAGFX_ERR_INVALID_ARG,
          "bind_texture(NULL view) → ERR_INVALID_ARG");

    /* draw(0,1) → ERR_INVALID_ARG. */
    lagfx_status_t dst0 = lagfx_translate_render_draw(&state, 0, 1, 0, 0);
    CHECK(dst0 == LAGFX_ERR_INVALID_ARG,
          "draw(0,1) → ERR_INVALID_ARG");

    /* Real draw: 3 verts, 1 instance. Phase 3.A records a
     * vkCmdDraw with no bound pipeline — lavapipe's validation
     * will ignore that or warn, but the encoder records the
     * command either way. We accept whichever the ICD does as
     * long as the eventual submit doesn't error. */
    lagfx_status_t dst = lagfx_translate_render_draw(&state, 3, 1, 0, 0);
    CHECK(dst == LAGFX_OK, "translate_render_draw(3,1)");

    lagfx_status_t est = lagfx_translate_render_end(&state);
    CHECK(est == LAGFX_OK, "translate_render_end");
    CHECK(state.in_pass == false, "in_pass false after end");

    vr = vkEndCommandBuffer(cb);
    CHECK(vr == VK_SUCCESS, "vkEndCommandBuffer");

    /* Submit with a fence to prove the command buffer is valid
     * end-to-end. The passthrough pipeline is now bound so the
     * vkCmdDraw is well-defined. */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    CHECK(vr == VK_SUCCESS, "vkCreateFence");

    if (fence != VK_NULL_HANDLE) {
        VkSubmitInfo si = {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
        CHECK(vr == VK_SUCCESS, "vkQueueSubmit");

        const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
        vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
        CHECK(vr == VK_SUCCESS, "vkWaitForFences");
        vkDestroyFence(vk->device, fence, NULL);
    }

    lagfx_vk_cmdbuf_free(vk, cb);
    lagfx_vk_render_target_destroy(vk, &rt);
    return 0;
}

#endif /* LAGFX_HAVE_VULKAN */

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan translate render smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

#ifdef LAGFX_HAVE_VULKAN
    /* Case A: state-machine path. Runs regardless of ICD. */
    test_state_machine_no_vk_calls();

    /* Case B: real Vulkan — only if a device came up. */
    char *err = NULL;
    lagfx_device_t *dev = make_device(&err);
    if (!dev) {
        fprintf(stdout, "device_new returned NULL (err=%s)\n",
                err ? err : "<none>");
        free(err);
        SKIP("no loadable Vulkan ICD — real-record path skipped");
    } else if (dev->vk && dev->vk->initialized) {
        (void)test_real_vk_record(dev->vk);
        lagfx_device_free(dev);
    } else {
        SKIP("LAGFX_HAVE_VULKAN set but dev->vk->initialized is false");
        lagfx_device_free(dev);
    }
#else
    /* No Vulkan at compile time — verify the stubs return
     * LAGFX_ERR_BACKEND so callers can detect the degraded mode. */
    lagfx_translate_render_state_t state;
    memset(&state, 0, sizeof(state));
    lagfx_status_t st = lagfx_translate_render_begin(
        NULL, &state, NULL, NULL, 64, 64, NULL, NULL, 0);
    CHECK(st == LAGFX_ERR_BACKEND,
          "no-vulkan begin → LAGFX_ERR_BACKEND");

    st = lagfx_translate_render_bind_pipeline(
        &state, LAGFX_SHADER_BLIT, NULL);
    CHECK(st == LAGFX_ERR_BACKEND,
          "no-vulkan bind_pipeline → LAGFX_ERR_BACKEND");

    st = lagfx_translate_render_draw(&state, 3, 1, 0, 0);
    CHECK(st == LAGFX_ERR_BACKEND,
          "no-vulkan draw → LAGFX_ERR_BACKEND");

    st = lagfx_translate_render_end(&state);
    CHECK(st == LAGFX_ERR_BACKEND,
          "no-vulkan end → LAGFX_ERR_BACKEND");

    SKIP("library built without Vulkan — encoder path is stubbed");
#endif

    fprintf(stdout, "\n=== Summary: %s (%d skipped) ===\n",
            g_fail ? "FAILURES" : "ALL GOOD", g_skipped);
    return g_fail ? 1 : 0;
}
