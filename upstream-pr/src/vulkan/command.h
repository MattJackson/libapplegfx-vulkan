/*
 * libapplegfx-vulkan — Vulkan command pool / buffer helpers (Phase 1.B.2)
 * src/vulkan/command.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal header. Not installed.
 *
 * Phase 1.B.2 layers on top of the VkInstance/VkDevice/VkQueue quartet
 * created by src/vulkan/instance.c. This module owns:
 *
 *   - One VkCommandPool bound to the graphics queue family, created
 *     with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT so primary
 *     buffers can be rerecorded in place (matches the per-doorbell
 *     recording pattern we expect in Phase 2).
 *   - Primary VkCommandBuffer allocation/free against that pool.
 *   - An "empty submit" smoke-test path that proves we can drive the
 *     queue end-to-end (alloc, begin, end, submit w/ fence, wait,
 *     free) without any rendering state.
 *
 * When built without Vulkan (LAGFX_HAVE_VULKAN unset) the functions
 * remain defined but become no-ops that return LAGFX_OK. This mirrors
 * the graceful-degradation policy used by instance.c so consumers on
 * a Darwin dev host (no loadable ICD) keep compiling and testing
 * the non-rendering paths.
 */

#ifndef LIBAPPLEGFX_VULKAN_COMMAND_H
#define LIBAPPLEGFX_VULKAN_COMMAND_H

#include "instance.h"

#include "libapplegfx-vulkan.h"

/* Create a VkCommandPool on the graphics queue family. Idempotent:
 * if the pool is already created returns LAGFX_OK. On no-vulkan
 * builds this is a no-op that returns LAGFX_OK. */
lagfx_status_t lagfx_vk_command_pool_create(struct lagfx_vk_state *vk);

/* Destroy the VkCommandPool (if any). Safe on NULL / on a state that
 * never had a pool created. */
void lagfx_vk_command_pool_destroy(struct lagfx_vk_state *vk);

#ifdef LAGFX_HAVE_VULKAN

/* Allocate one primary command buffer from the pool. On no-vulkan
 * builds these are gated out — callers must also be gated. */
lagfx_status_t lagfx_vk_cmdbuf_alloc(struct lagfx_vk_state *vk,
                                     VkCommandBuffer *out);

/* Free a previously-allocated primary command buffer. */
void lagfx_vk_cmdbuf_free(struct lagfx_vk_state *vk, VkCommandBuffer cb);

#endif /* LAGFX_HAVE_VULKAN */

/* Smoke-test the whole queue path: allocate a primary command buffer,
 * begin (one-time-submit), end, submit with a fresh VkFence, wait on
 * the fence with a 1-second timeout, free. Returns LAGFX_OK if the
 * fence signalled within the timeout, LAGFX_ERR_VULKAN_INIT otherwise.
 *
 * On no-vulkan builds this is a no-op that returns LAGFX_OK (so tests
 * can assert "the API exists and doesn't crash" in a uniform way). */
lagfx_status_t lagfx_vk_submit_empty(struct lagfx_vk_state *vk);

#endif /* LIBAPPLEGFX_VULKAN_COMMAND_H */
