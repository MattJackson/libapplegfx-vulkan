/*
 * libapplegfx-vulkan — internal draw-path interface
 * src/handlers/compute/compute_draw_internal.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Shared between the draw-path translation units (draw_resources.c,
 * draw_descriptors.c, draw_emit.c) and the inner-opcode dispatch
 * (compute_inner_ops.c). Not a public API.
 */

#ifndef LAGFX_COMPUTE_DRAW_INTERNAL_H
#define LAGFX_COMPUTE_DRAW_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol/state.h"

struct lagfx_vk_state;

#ifdef LAGFX_HAVE_VULKAN
#include <vulkan/vulkan.h>
#include "vulkan/pipeline_build.h"
#include "vulkan/iosurface.h"

/* 64 KiB: bounds a shader's dynamic storage-buffer indexing so an over-read
 * returns zero-padded data instead of faulting lavapipe; descriptor / MVP
 * reads use only the first bytes. */
#define LAGFX_DRAW_DS_BUF_SZ 65536u

/* Fragment-stage descriptor bindings are offset by this base so they never
 * collide with vertex-stage set-0 bindings in the merged pipeline layout. */
#define LAGFX_FRAG_BINDING_BASE 16u

/* Full-tile-stream vertex upload ceiling (heap-allocated, gated). */
#define LAGFX_BIGVERTS_BUF_SZ (16u * 1024u * 1024u)

/* draw_resources.c — guest-memory reads + content scoring */
bool lagfx_read_virtual_besteffort(lagfx_protocol_t *p,
                                   const lagfx_task_entry_t *task,
                                   uint64_t va, uint32_t len, uint8_t *buf);
bool lagfx_read_resource_backing(lagfx_protocol_t *p,
                                 const lagfx_task_entry_t *task,
                                 uint64_t gpa, uint32_t len, uint8_t *buf);
bool lagfx_read_binding_slot(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                             const lagfx_binding_slot_t *bs,
                             uint8_t *out, size_t len);
bool lagfx_looks_like_mvp_matrix(const uint8_t *data);
uint32_t lagfx_vtx_float_plausibility(const uint8_t *b, uint32_t len);
uint32_t lagfx_vtx_looks_like_positions(const uint8_t *b, uint32_t len, uint32_t stride);
uint32_t lagfx_read_vtx_source(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                               uint32_t ref, uint64_t offset,
                               uint32_t want, uint8_t *out, const char **how,
                               int mode);
void lagfx_texture_refresh(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                           struct lagfx_vk_state *vk, uint32_t tref);
lagfx_vk_iosurface_t *lagfx_texture_realize(lagfx_protocol_t *p,
                                            lagfx_task_entry_t *task,
                                            struct lagfx_vk_state *vk,
                                            uint32_t tref);

/* draw_descriptors.c — descriptor-set construction for translated draws */
VkDescriptorSet lagfx_build_draw_descriptor_set(
        lagfx_protocol_t *p, lagfx_task_entry_t *task,
        struct lagfx_vk_state *vk, VkDescriptorSetLayout dsl,
        const uint8_t *binding_no, const uint8_t *binding_kind, uint32_t n,
        VkBuffer *out_bufs, VkDeviceMemory *out_mems, uint32_t *out_n);

/* draw_emit.c — pipeline cache, vertex upload, draw emission */
void lagfx_pending_pipeline_drop_cache(lagfx_task_entry_t *task, VkDevice device);
VkPipeline lagfx_get_cached_pipeline(lagfx_task_entry_t *task, VkDevice device,
                                     const lagfx_pipeline_desc_t *pdesc);
/* GOAL-M2x: true when the pipeline's stage-in comes from separate per-attr
 * guest streams (>=2 non-AIR-claimed vertex slots) — the upload interleaves
 * them and the pipeline must use the tight reflected layout (vtx_stride=0). */
bool lagfx_vtx_multi_stream(lagfx_task_entry_t *task);

VkBuffer lagfx_upload_guest_vertex_buffer(lagfx_protocol_t *p,
                                          lagfx_task_entry_t *task,
                                          struct lagfx_vk_state *vk,
                                          VkDeviceMemory *out_mem,
                                          uint32_t *out_size);
void lagfx_emit_pending_draw(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                             const char *op, uint32_t count);

#endif /* LAGFX_HAVE_VULKAN */
#endif /* LAGFX_COMPUTE_DRAW_INTERNAL_H */
