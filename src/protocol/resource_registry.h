/*
 * libapplegfx-vulkan — resource reference registry
 * src/protocol/resource_registry.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Maps (PGCmdReference, task_id) → host-side resource metadata.
 * Inner render opcodes use u32 refs to refer to pipelines, buffers,
 * textures, samplers, etc. The registry is populated by outer opcodes
 * and IOUserClient selectors during resource creation, and consumed
 * by inner-opcode handlers that need to resolve refs to host objects.
 *
 * Phase 3 scaffold: host_handle is always NULL. Real Vulkan object
 * creation fills it in later.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_RESOURCE_REGISTRY_H
#define LIBAPPLEGFX_PROTOCOL_RESOURCE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#define LAGFX_MAX_RESOURCES 256u

typedef enum {
    LAGFX_RESOURCE_TYPE_UNKNOWN = 0,
    LAGFX_RESOURCE_TYPE_BUFFER,
    LAGFX_RESOURCE_TYPE_TEXTURE,
    LAGFX_RESOURCE_TYPE_PIPELINE,
    LAGFX_RESOURCE_TYPE_SAMPLER,
    LAGFX_RESOURCE_TYPE_HEAP,
    LAGFX_RESOURCE_TYPE_DEPTH_STENCIL_STATE,
} lagfx_resource_type_t;

typedef struct {
    uint32_t ref;
    lagfx_resource_type_t type;
    uint32_t task_id;
    void    *host_handle;        /* For textures: points to lagfx_vk_iosurface_t* */
    VkImage image;               /* Cached VkImage handle for quick access */
    VkImageView view;            /* Cached VkImageView handle for quick access */
    uint64_t gpu_addr;
    uint64_t size;
} lagfx_resource_entry_t;

typedef struct {
    lagfx_resource_entry_t entries[LAGFX_MAX_RESOURCES];
    uint32_t count;
} lagfx_resource_registry_t;

void lagfx_resource_register(lagfx_resource_registry_t *reg,
                             uint32_t ref,
                             lagfx_resource_type_t type,
                             uint32_t task_id,
                             uint64_t gpu_addr,
                             uint64_t size);

lagfx_resource_entry_t *lagfx_resource_lookup(lagfx_resource_registry_t *reg,
                                               uint32_t ref,
                                               uint32_t task_id);

void lagfx_resource_unregister(lagfx_resource_registry_t *reg,
                               uint32_t ref,
                               uint32_t task_id);

void lagfx_resource_clear_task(lagfx_resource_registry_t *reg,
                               uint32_t task_id);

#endif /* LIBAPPLEGFX_PROTOCOL_RESOURCE_REGISTRY_H */
