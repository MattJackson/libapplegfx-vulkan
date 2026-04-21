/*
 * libapplegfx-vulkan — Vulkan command pool + empty-submit (Phase 1.B.2)
 * src/vulkan/command.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * === Scope =====================================================
 *
 * Layers on the VkInstance/VkDevice/VkQueue quartet created by
 * src/vulkan/instance.c. Adds:
 *
 *   lagfx_vk_command_pool_create / _destroy
 *       Own a single VkCommandPool on the graphics queue family,
 *       created with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
 *       so primary buffers can be reset and rerecorded (the pattern
 *       Phase 2 will use once per doorbell ring).
 *
 *   lagfx_vk_cmdbuf_alloc / _free
 *       Primary command buffer alloc + free against the pool. Thin
 *       wrappers around vkAllocateCommandBuffers / vkFreeCommandBuffers
 *       but centralise error-logging and the "pool exists?" check so
 *       upper layers stay tidy.
 *
 *   lagfx_vk_submit_empty
 *       Smoke-test path: alloc a primary buffer, begin (one-time
 *       submit), end, submit with a fresh VkFence, wait on the fence
 *       with a 1 second timeout, free. This is the "metal-no-op
 *       round-trip proved at the Vulkan level" — mirrors what the
 *       guest does via Metal (empty command buffer end-to-end) but
 *       inside our library. It's the exit gate for Phase 1.B.2.
 *
 * === Graceful degradation ======================================
 *
 * When built without Vulkan (LAGFX_HAVE_VULKAN unset) every function
 * here is a no-op that returns LAGFX_OK. The Darwin dev-host workflow
 * (no loadable ICD) keeps working: lagfx_device_new still succeeds,
 * tests that exercise non-rendering paths still pass, and the
 * submit-empty test recognises the stub path and emits SKIP.
 *
 * === Concurrency ===============================================
 *
 * Not yet thread-safe. VkCommandPool is externally synchronised per
 * Vulkan spec; once we grow multi-threaded recording we'll need one
 * pool per recording thread. Phase 1.B.2 keeps a single pool because
 * the dispatch layer is still single-threaded.
 */

#include "command.h"
#include "instance.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* === Public entry points --------------------------------------- */

lagfx_status_t lagfx_vk_command_pool_create(struct lagfx_vk_state *vk) {
    if (!vk) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        LAGFX_ERR("cmd_pool_create: Vulkan state not initialized");
        return LAGFX_ERR_VULKAN_INIT;
    }
    if (vk->cmd_pool != VK_NULL_HANDLE) {
        /* Already created — idempotent. */
        return LAGFX_OK;
    }

    VkCommandPoolCreateInfo ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        /* RESET_COMMAND_BUFFER_BIT: primary buffers allocated from
         * this pool can be reset individually via vkResetCommandBuffer
         * (vs. pool-wide reset). Matches the per-doorbell recording
         * pattern expected in Phase 2.
         *
         * We intentionally do NOT set TRANSIENT_BIT. Transient is a
         * hint for short-lived buffers that the driver may use to
         * skip some bookkeeping; we want stable, reusable buffers.
         */
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk->graphics_queue_family,
    };

    VkResult vr = vkCreateCommandPool(vk->device, &ci, NULL, &vk->cmd_pool);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cmd_pool_create: vkCreateCommandPool failed "
                  "(VkResult=%d)", (int)vr);
        vk->cmd_pool = VK_NULL_HANDLE;
        return LAGFX_ERR_VULKAN_INIT;
    }

    LAGFX_LOG("cmd_pool_create: pool=%p qf=%u", (void *)vk->cmd_pool,
              vk->graphics_queue_family);
    return LAGFX_OK;
}

void lagfx_vk_command_pool_destroy(struct lagfx_vk_state *vk) {
    if (!vk) {
        return;
    }
    if (vk->cmd_pool == VK_NULL_HANDLE || vk->device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
    vk->cmd_pool = VK_NULL_HANDLE;
}

lagfx_status_t lagfx_vk_cmdbuf_alloc(struct lagfx_vk_state *vk,
                                     VkCommandBuffer *out) {
    if (!vk || !out) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out = VK_NULL_HANDLE;
    if (!vk->initialized || vk->cmd_pool == VK_NULL_HANDLE) {
        LAGFX_ERR("cmdbuf_alloc: command pool not available");
        return LAGFX_ERR_VULKAN_INIT;
    }

    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult vr = vkAllocateCommandBuffers(vk->device, &ai, out);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("cmdbuf_alloc: vkAllocateCommandBuffers failed "
                  "(VkResult=%d)", (int)vr);
        *out = VK_NULL_HANDLE;
        return LAGFX_ERR_VULKAN_INIT;
    }
    return LAGFX_OK;
}

void lagfx_vk_cmdbuf_free(struct lagfx_vk_state *vk, VkCommandBuffer cb) {
    if (!vk || cb == VK_NULL_HANDLE) {
        return;
    }
    if (vk->cmd_pool == VK_NULL_HANDLE || vk->device == VK_NULL_HANDLE) {
        return;
    }
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);
}

lagfx_status_t lagfx_vk_submit_empty(struct lagfx_vk_state *vk) {
    if (!vk) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE
        || vk->graphics_queue == VK_NULL_HANDLE
        || vk->cmd_pool == VK_NULL_HANDLE) {
        LAGFX_ERR("submit_empty: Vulkan state not fully initialized");
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* --- Allocate a primary command buffer -------------------- */
    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }

    /* --- Begin recording (one-time submit) -------------------- */
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("submit_empty: vkBeginCommandBuffer failed (VkResult=%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* No rendering commands — this is the "Metal no-op" analogue. */

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("submit_empty: vkEndCommandBuffer failed (VkResult=%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* --- Create a fence --------------------------------------- */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        /* Unsignaled by default — we want vkWaitForFences below to
         * actually block until submission completes. */
        .flags = 0,
    };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("submit_empty: vkCreateFence failed (VkResult=%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* --- Submit ----------------------------------------------- */
    VkSubmitInfo si = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cb,
    };
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("submit_empty: vkQueueSubmit failed (VkResult=%d)",
                  (int)vr);
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* --- Wait (1 second budget) ------------------------------- */
    /* 1 second in nanoseconds. Empty submit on lavapipe should be
     * sub-millisecond; the generous timeout is defensive against a
     * busy CI host. If it fires we want a clear failure, not a hang. */
    const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    lagfx_status_t result = LAGFX_OK;
    if (vr == VK_TIMEOUT) {
        LAGFX_ERR("submit_empty: vkWaitForFences timed out after 1s");
        result = LAGFX_ERR_VULKAN_INIT;
    } else if (vr != VK_SUCCESS) {
        LAGFX_ERR("submit_empty: vkWaitForFences failed (VkResult=%d)",
                  (int)vr);
        result = LAGFX_ERR_VULKAN_INIT;
    }

    /* --- Cleanup ---------------------------------------------- */
    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    if (result == LAGFX_OK) {
        LAGFX_LOG("submit_empty: empty submit round-trip OK");
    }
    return result;
}

#else  /* !LAGFX_HAVE_VULKAN -------------------------------------- */

/* No-vulkan stubs. Preserve the Darwin-no-ICD development path by
 * returning success; callers must treat the handles as opaque and
 * not dereference. */

lagfx_status_t lagfx_vk_command_pool_create(struct lagfx_vk_state *vk) {
    (void)vk;
    return LAGFX_OK;
}

void lagfx_vk_command_pool_destroy(struct lagfx_vk_state *vk) {
    (void)vk;
}

lagfx_status_t lagfx_vk_submit_empty(struct lagfx_vk_state *vk) {
    (void)vk;
    return LAGFX_OK;
}

#endif /* LAGFX_HAVE_VULKAN */
