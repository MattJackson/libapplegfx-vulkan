/*
 * libapplegfx-vulkan — Vulkan init (Phase 1.B)
 * src/vulkan/instance.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Defines struct lagfx_vk_state and the init/shutdown entry points
 * consumed by src/device.c. See src/vulkan/instance.c for the
 * rationale around optional compilation when vulkan_dep is absent.
 */

#ifndef LIBAPPLEGFX_VULKAN_INSTANCE_H
#define LIBAPPLEGFX_VULKAN_INSTANCE_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* Internal Vulkan state. Opaque to callers outside src/vulkan/.
 *
 * When the library is built WITHOUT vulkan (LAGFX_HAVE_VULKAN unset),
 * the struct is still defined but fields are minimal — it exists only
 * so the device.c lifecycle can hold the pointer uniformly. */
struct lagfx_vk_state {
#ifdef LAGFX_HAVE_VULKAN
    VkInstance       instance;
    VkPhysicalDevice phys_device;
    VkDevice         device;
    VkQueue          graphics_queue;
    uint32_t         graphics_queue_family;

    /* Selected physical-device properties, cached at init for logging
     * + later extension decisions. */
    VkPhysicalDeviceProperties phys_props;

    /* Feature-enable flags — reflect what was actually requested +
     * accepted at vkCreateDevice time. Used by later phases to decide
     * whether to take a fast path (shader_object, dynamic_rendering)
     * or fall back. */
    bool have_dynamic_rendering;
    bool have_synchronization2;
    bool have_timeline_semaphore;
    bool have_descriptor_indexing;
    bool have_shader_object;
    bool have_extended_dynamic_state3;
    bool have_buffer_device_address;  /* M2 host-flattening (PhysicalStorageBuffer) */

    /* Phase 1.B.2: command pool bound to the graphics queue family.
     * Created at lagfx_vk_init completion, destroyed in
     * lagfx_vk_shutdown BEFORE the VkDevice. Owned by src/vulkan/command.c;
     * this struct just carries the handle so the init/shutdown plumbing
     * in instance.c can reach it. */
    VkCommandPool    cmd_pool;

    VkCommandBuffer  frame_cmdbuf;
    VkFence          frame_fence;
    bool             frame_in_progress;

    VkFence          pending_fence;
    bool             pending_fence_valid;
    VkCommandBuffer  pending_cmdbuf;

    VkPipeline       passthrough_pipeline;
    VkPipelineLayout passthrough_layout;
    VkDescriptorSetLayout passthrough_dsl;

    /* Stage 65d Option 3 — empty pipeline layout (no sets, no push
     * constants) for the substitute triangle pipeline whose shaders
     * declare zero descriptor bindings. vkCreateGraphicsPipelines
     * rejects layout=VK_NULL_HANDLE per the spec; this is the minimal
     * valid layout shared by every triangle-substitute pipeline build. */
    VkPipelineLayout empty_layout;

    VkImage          frame_image;
    VkImageView      frame_image_view;
    VkDeviceMemory   frame_image_mem;
    uint32_t         frame_image_w;
    uint32_t         frame_image_h;
    VkFormat         frame_image_fmt;
    VkImageLayout    frame_image_layout;

    VkBuffer         dummy_vb;
    VkDeviceMemory   dummy_vb_mem;

    VkDescriptorPool      fallback_desc_pool;
    VkDescriptorSet       fallback_desc_set;
    VkBuffer              fallback_ubo;
    VkDeviceMemory        fallback_ubo_mem;

    /* Stage 85b — general per-draw descriptor pool for translated
     * resource-using pipelines. Sized for many sets/bindings across a frame;
     * FREE_DESCRIPTOR_SET so each draw frees its set after submit. Holds
     * uniform + storage buffers + combined image samplers + (M1 c) separate
     * sampled images + samplers. */
    VkDescriptorPool      draw_desc_pool;

    /* M1 (c): shared default sampler (linear/clamp) for texture-sampling
     * translated compositor pipelines. */
    VkSampler             default_sampler;

    /* M3/perf (env LAGFX_PERF): frame-time instrumentation accumulators,
     * reset per frame at readback. Feeds the lavapipe-vs-GPU decision. */
    uint64_t              perf_last_frame_ns;
    uint64_t              perf_frame_draws;
    uint64_t              perf_frame_draw_ns;

    VkPipeline       cursor_pipeline;
    VkPipelineLayout cursor_layout;
    VkDescriptorSetLayout cursor_dsl;
    VkDescriptorPool      cursor_desc_pool;
    VkDescriptorSet       cursor_desc_set;

    /* Layer compositor (MAX-blend overlay of per-pass surfaces onto rt). */
    VkPipeline            composite_pipeline;
    VkPipelineLayout      composite_layout;
    VkDescriptorSetLayout composite_dsl;
    VkDescriptorPool      composite_pool;
    VkDescriptorSet       composite_set;
    VkSampler             composite_sampler;
    bool                  composite_ready;

    VkImage          cursor_glyph_image;
    VkImageView      cursor_glyph_view;
    VkDeviceMemory   cursor_glyph_mem;
    VkSampler        cursor_sampler;
    VkBuffer         cursor_ubo;
    VkDeviceMemory   cursor_ubo_mem;
    bool             cursor_glyph_valid;
    uint32_t         cursor_glyph_w;
    uint32_t         cursor_glyph_h;

    /* Per-device push-descriptor dispatch cache —
     * vkCmdPushDescriptorSetKHR resolved via vkGetDeviceProcAddr.
     * Lives on the vk state (not in a file-scope static) so multiple
     * VkDevices can each carry their own pfn. Populated lazily by
     * resolve_push_desc() in src/translate/render_encoder.c. */
    PFN_vkCmdPushDescriptorSetKHR push_desc_pfn;

    /* Vulkan validation layer debug messenger (optional, gated by
     * LAGFX_VK_VALIDATION=1 at runtime). Destroyed before vkDestroyInstance. */
    VkDebugUtilsMessengerEXT debug_messenger;
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger_fn;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger_fn;
#else
    /* Pad so sizeof(struct) > 0 on no-vulkan builds. */
    int _placeholder;
#endif
    /* Keep this flag in both paths so callers can cheaply ask
     * "did we actually init Vulkan?" without #ifdefs. */
    bool initialized;
};

/* Create VkInstance + select VkPhysicalDevice + create VkDevice +
 * retrieve VkQueue. Stores the whole lot in *out.
 *
 * On success returns LAGFX_OK. On failure returns LAGFX_ERR_VULKAN_INIT
 * (or LAGFX_ERR_OUT_OF_MEMORY) and *out is left NULL. The desc is used
 * for:
 *   - desc->shell_vulkan_instance: if non-NULL, reuse the caller's
 *     VkInstance rather than creating our own (future work — currently
 *     logged and ignored in Phase 1.B).
 *
 * The caller is responsible for having already applied LP_NUM_THREADS
 * via setenv before calling this function — Mesa reads that var once
 * at ICD init triggered by vkCreateInstance. */
lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                             const lagfx_device_descriptor_t *desc);

/* Tear down: pipeline resources + command pool + device + instance.
 * Safe on NULL. */
void lagfx_vk_shutdown(struct lagfx_vk_state *state);

#ifdef LAGFX_HAVE_VULKAN
/* Stage 85b — create a host-visible STORAGE buffer and upload `data` (NULL ⇒
 * zero-init). Caller destroys *out_buf + frees *out_mem after the draw submits. */
lagfx_status_t lagfx_vk_make_host_storage_buffer(struct lagfx_vk_state *vk,
                                                 const void *data,
                                                 VkDeviceSize size,
                                                 VkBuffer *out_buf,
                                                 VkDeviceMemory *out_mem);

/* M2: storage buffer of alloc_size bytes, first data_len from `data`, rest
 * zero-padded — so a buffer can be sized to the guest's DECLARED size (dynamic
 * index stays in bounds) with only data_len bytes of real content. */
lagfx_status_t lagfx_vk_make_host_storage_buffer_padded(struct lagfx_vk_state *vk,
                                                        const void *data,
                                                        VkDeviceSize data_len,
                                                        VkDeviceSize alloc_size,
                                                        VkBuffer *out_buf,
                                                        VkDeviceMemory *out_mem);

/* M2 host-flatten step 2: host-visible storage buffer that also exposes a
 * VkDeviceAddress (for PhysicalStorageBuffer arg-buffer flattening). Requires
 * vk->have_buffer_device_address. */
lagfx_status_t lagfx_vk_make_device_address_buffer(struct lagfx_vk_state *vk,
                                                   const void *data,
                                                   VkDeviceSize size,
                                                   VkBuffer *out_buf,
                                                   VkDeviceMemory *out_mem,
                                                   VkDeviceAddress *out_addr);
#endif

#endif /* LIBAPPLEGFX_VULKAN_INSTANCE_H */
