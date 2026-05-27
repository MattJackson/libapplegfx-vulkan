/*
 * libapplegfx-vulkan — Compute inner-opcode handlers + dispatch
 * src/handlers/compute/compute_inner_ops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Architecture: encType=0 / encType=1 segments arrive via
 * exec_cmdbuf.c::inner_walk_segment. This file is the dispatch
 * table for individual inner opcodes within those segments.
 *
 * Implementation status (2026-05-16 Task 6): all 15 observed
 * encType=0 opcodes have real parse-and-trace handlers. Each handler:
 *   - Validates wire payload size against render-decoder-handlers.md spec
 *   - Parses fields using lagfx_le16/32/64 for alignment-safe reads
 *   - Emits LAGFX_LOG line with parsed fields
 *   - Marks TODO: Stage 70 for Vulkan translation hooks
 *
 * Observed encType=0 opcode frequency (current container, ~22 min):
 *   0x007e  27475   SetVertexBufferOffset — Task 6 implemented
 *   0x0074  26664   SetRenderPipelineState — Task 6 implemented
 *   0x007d  23347   SetVertexBuffers — Task 6 implemented
 *   0x0007  22439   DrawIndexedPrimitives16 — Task 6 implemented
 *   0x006e  15744   SetFragmentBuffers — Task 6 implemented
 *   0x0072  14890   SetFragmentTextures — Task 6 implemented
 *   0x0075  12853   SetScissorRect — Task 6 implemented
 *   0x0082   9577   SetViewport — Task 6 implemented
 *   0x0070   8317   SetFragmentSamplerStates — Task 6 implemented
 *   0x001a   8168   RenderDescribeRenderPass (encType=0 namespace) — Task 6 implemented
 *   0x0017   7416   RenderBarrierScope — Task 6 implemented
 *   0x0003   4256   DrawInstancedPrimitives16 — Task 6 implemented
 *   0x0006    310   DrawIndexedPrimitives64 — Task 6 implemented
 *   0x006f    305   SetFragmentBufferOffset — Task 6 implemented
 *   0x0001     86   DrawPrimitives16 — Task 6 implemented
 *
 * RE citations: all wire-format specs from
 * paravirt-re/library/state-machines/render-decoder-handlers.md.
 */

#include "compute_inner_ops.h"
#include "display.h"  /* Full lagfx_display_t definition with rt field */
#include "task_translate.h"
#include "protocol/object_resolver.h"

#include "common/le.h"
#include "common/log.h"
#include "device.h"
#include "protocol/state.h"
#include "vulkan/iosurface.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/draw_record.h"
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "air2spirv/metallib_extract.h"

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

typedef int (*lagfx_compute_inner_op_fn)(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            uint32_t          task_id,
                                            const uint8_t    *body,
                                            size_t            body_len);

typedef struct {
    uint32_t                    opcode;
    const char                 *name;
    lagfx_compute_inner_op_fn   handler;
} lagfx_compute_inner_op_desc_t;

/* === Group A — Draw opcodes (0x01, 0x03, 0x06, 0x07) ========== */

static int op_draw_primitives_16(lagfx_protocol_t *p,
                                    uint32_t          encoder_type,
                                    uint32_t          task_id,
                                    const uint8_t    *body,
                                    size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 55 — PGCmdDrawPrimitives16 (8 B), scalar family */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x01 DrawPrimitives16 payload too small (%zu < 8)",
                   body_len);
        return 1;
    }
    uint32_t vertex_start = lagfx_le32(body + 0);
    uint32_t vertex_count = lagfx_le32(body + 4);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x01 DrawPrimitives16 task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x01 DrawPrimitives16 task_id=%u not live", task_id);
        return 1;
    }

    /* Populate per-task pending draw state. */
    task->pending_draw.valid = true;
    task->pending_draw.indexed = false;           /* Unindexed draw */
    task->pending_draw.index_count = vertex_count;  /* index_count field used for both indexed/unindexed */
    task->pending_draw.base_vertex = (int32_t)vertex_start;
    task->pending_draw.instance_count = 1u;       /* Default for non-instanced variant */
    task->pending_draw.first_instance = 0u;

    LAGFX_LOG("compute_inner: 0x01 DrawPrimitives16 vertexStart=%u vertexCount=%u -> pending_draw.valid=true indexed=false",
              vertex_start, vertex_count);
    
    /* Stage 65d Option 3 Step 3: build VkPipeline when draw fires. */
#ifdef LAGFX_HAVE_VULKAN
    if (task->pending_pipeline.valid && task->render_pass_desc.valid) {
        lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
        if (dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized) {
            VkDevice device = dev_with_vk->vk->device;
            lagfx_pipeline_desc_t pdesc = {
                .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
                .fragment_shader      = (VkShaderModule)task->pending_pipeline.fragment_shader,
                .layout               = dev_with_vk->vk->empty_layout,
                /* Stage 65d Option 3: the substitute triangle SPVs were
                 * compiled by the AIR-to-SPIRV translator and use the
                 * function names from triangle.metal as entry points,
                 * not glslang's default "main". */
                .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
                .fragment_entry_point = task->pending_pipeline.translated ? "main" : "triangle_fragment",
                .color_format         = (VkFormat)task->render_pass_desc.color_format,
                .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            lagfx_status_t st = lagfx_pipeline_build(device, &pdesc, &pipeline);
            if (st == LAGFX_OK) {
                LAGFX_LOG("op_0x01 Option 3 Step 3: built VkPipeline=%p for draw count=%u",
                          (void *)pipeline, vertex_count);
                /* Step 4: record and submit the draw command */
                lagfx_display_t *display = dev_with_vk->displays[0];
                if (display && display->rt_ready && display->rt.image != VK_NULL_HANDLE) {
                    /* Stage 65d Option 3: always draw the bundled triangle
                     * (3 vertices, 1 instance, unindexed). The guest's
                     * vertex_count / instance_count / index buffer are
                     * meaningless for the substitute path — triangle.vert
                     * hardcodes vec2 verts[3] and reads gl_VertexIndex. */
                    st = lagfx_vk_draw_record_and_submit(
                        dev_with_vk->vk, pipeline, &display->rt,
                        false,  /* indexed — always unindexed for substitute */
                        3,      /* vertex_count — the triangle has 3 vertices */
                        1,      /* instance_count — one triangle is enough */
                        0, 0,   /* base_vertex, first_instance */
                        0);     /* index_buffer_ref unused */
                    if (st == LAGFX_OK) {
                        LAGFX_LOG("op_0x01 Option 3 Step 4: drew substitute triangle (guest req vertexCount=%u)", vertex_count);
                        /* Signal the display tick: post-draw, the render
                         * target VkImage has fresh pixels. QEMU's frame_ready_bh
                         * will pull via lagfx_display_read_frame. Without
                         * this the steady-state path is silent because the
                         * kext's vchan_display_submit only fires during boot. */
                        lagfx_display_signal_frame_ready(display);
                    } else {
                        LAGFX_WARN("op_0x01 Option 3 Step 4: lagfx_vk_draw_record_and_submit failed (%d)", (int)st);
                    }
                } else {
                    LAGFX_WARN("op_0x01 Option 3 Step 4: no render target available");
                }
            } else {
                LAGFX_WARN("op_0x01 Option 3 Step 3: lagfx_pipeline_build failed (%d)", (int)st);
            }
        }
    }
#endif
    
    /* TODO: Stage 70 — translate to vkCmdDraw once AIR translation is in place. */
    return 0;
}

static int op_draw_instanced_primitives_16(lagfx_protocol_t *p,
                                              uint32_t          encoder_type,
                                              uint32_t          task_id,
                                              const uint8_t    *body,
                                              size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 57 — PGCmdDrawInstancedPrimitives16 (8 B), scalar family */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x03 DrawInstancedPrimitives16 payload too small (%zu < 8)",
                   body_len);
        return 1;
    }
    /* Wire format: field0 = vertex_start + instance_count (packed), field1 = vertex_count.
     * Metal selector: drawPrimitives:vertexStart:vertexCount:instanceCount: */
    uint32_t vertex_start = lagfx_le32(body + 0);
    uint32_t vertex_count = lagfx_le32(body + 4);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x03 DrawInstancedPrimitives16 task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x03 DrawInstancedPrimitives16 task_id=%u not live", task_id);
        return 1;
    }

    /* Populate per-task pending draw state. */
    task->pending_draw.valid = true;
    task->pending_draw.indexed = false;           /* Unindexed draw */
    task->pending_draw.index_count = vertex_count;  /* index_count field used for both indexed/unindexed */
    task->pending_draw.base_vertex = (int32_t)vertex_start;
    task->pending_draw.instance_count = 1u;       /* Instance count from wire layout */
    task->pending_draw.first_instance = 0u;

    LAGFX_LOG("compute_inner: 0x03 DrawInstancedPrimitives16 vertexStart=%u vertexCount=%u instanceCount=1 -> pending_draw.valid=true indexed=false",
              vertex_start, vertex_count);
    
    /* Stage 65d Option 3 Step 3: build VkPipeline when draw fires. */
#ifdef LAGFX_HAVE_VULKAN
    if (task->pending_pipeline.valid && task->render_pass_desc.valid) {
        lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
        if (dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized) {
            VkDevice device = dev_with_vk->vk->device;
            lagfx_pipeline_desc_t pdesc = {
                .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
                .fragment_shader      = (VkShaderModule)task->pending_pipeline.fragment_shader,
                .layout               = dev_with_vk->vk->empty_layout,
                /* Stage 65d Option 3: the substitute triangle SPVs were
                 * compiled by the AIR-to-SPIRV translator and use the
                 * function names from triangle.metal as entry points,
                 * not glslang's default "main". */
                .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
                .fragment_entry_point = task->pending_pipeline.translated ? "main" : "triangle_fragment",
                .color_format         = (VkFormat)task->render_pass_desc.color_format,
                .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            lagfx_status_t st = lagfx_pipeline_build(device, &pdesc, &pipeline);
            if (st == LAGFX_OK) {
                LAGFX_LOG("op_0x03 Option 3 Step 3: built VkPipeline=%p for draw count=%u",
                          (void *)pipeline, vertex_count);
                
                /* Step 4: record and submit the draw command */
                lagfx_display_t *display = dev_with_vk->displays[0];
                if (display && display->rt_ready && display->rt.image != VK_NULL_HANDLE) {
                    /* Stage 65d Option 3 substitute — see op_0x01 site for rationale. */
                    st = lagfx_vk_draw_record_and_submit(
                        dev_with_vk->vk, pipeline, &display->rt,
                        false, 3, 1, 0, 0, 0);
                    if (st == LAGFX_OK) {
                        LAGFX_LOG("op_0x03 Option 3 Step 4: drew substitute triangle (guest req vertexCount=%u)", vertex_count);
                        lagfx_display_signal_frame_ready(display);
                    } else {
                        LAGFX_WARN("op_0x03 Option 3 Step 4: lagfx_vk_draw_record_and_submit failed (%d)", (int)st);
                    }
                } else {
                    LAGFX_WARN("op_0x03 Option 3 Step 4: no render target available");
                }
            } else {
                LAGFX_WARN("op_0x03 Option 3 Step 3: lagfx_pipeline_build failed (%d)", (int)st);
            }
        }
    }
#endif
    
    /* TODO: Stage 70 — translate to vkCmdDraw with instanceCount once wire format ambiguity resolved. */
    return 0;
}

static int op_draw_indexed_primitives_64(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            uint32_t          task_id,
                                            const uint8_t    *body,
                                            size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 60 — PGCmdDrawIndexedPrimitives64 (24 B), ref=1 */
    if (body_len < 24u) {
        LAGFX_WARN("compute_inner: 0x06 DrawIndexedPrimitives64 payload too small (%zu < 24)",
                   body_len);
        return 1;
    }
    uint32_t index_count      = lagfx_le32(body + 0);
    uint32_t index_type       = lagfx_le32(body + 4);  /* MTLIndexType UInt16/UInt32 */
    uint32_t index_buffer_ref = lagfx_le32(body + 8);
    uint64_t index_buffer_offset = lagfx_le64(body + 12);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x06 DrawIndexedPrimitives64 task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x06 DrawIndexedPrimitives64 task_id=%u not live", task_id);
        return 1;
    }

    /* Populate per-task pending draw state. */
    task->pending_draw.valid = true;
    task->pending_draw.indexed = true;              /* Indexed draw */
    task->pending_draw.index_count = index_count;
    task->pending_draw.index_buffer_ref = index_buffer_ref;
    task->pending_draw.base_vertex = 0;             /* Not specified in 0x06 variant */
    task->pending_draw.instance_count = 1u;         /* Default for non-instanced variant */
    task->pending_draw.first_instance = 0u;

    LAGFX_LOG("compute_inner: 0x06 DrawIndexedPrimitives64 count=%u type=%u bufRef=0x%x offset=0x%llx -> pending_draw.valid=true indexed=true",
              index_count, index_type, index_buffer_ref, (unsigned long long)index_buffer_offset);
    
    /* Stage 65d Option 3 Step 3: build VkPipeline when draw fires. */
#ifdef LAGFX_HAVE_VULKAN
    if (task->pending_pipeline.valid && task->render_pass_desc.valid) {
        lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
        if (dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized) {
            VkDevice device = dev_with_vk->vk->device;
            lagfx_pipeline_desc_t pdesc = {
                .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
                .fragment_shader      = (VkShaderModule)task->pending_pipeline.fragment_shader,
                .layout               = dev_with_vk->vk->empty_layout,
                /* Stage 65d Option 3: the substitute triangle SPVs were
                 * compiled by the AIR-to-SPIRV translator and use the
                 * function names from triangle.metal as entry points,
                 * not glslang's default "main". */
                .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
                .fragment_entry_point = task->pending_pipeline.translated ? "main" : "triangle_fragment",
                .color_format         = (VkFormat)task->render_pass_desc.color_format,
                .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            lagfx_status_t st = lagfx_pipeline_build(device, &pdesc, &pipeline);
            if (st == LAGFX_OK) {
                LAGFX_LOG("op_0x06 Option 3 Step 3: built VkPipeline=%p for draw count=%u",
                          (void *)pipeline, index_count);
                
                /* Step 4: record and submit the indexed draw command */
                lagfx_display_t *display = dev_with_vk->displays[0];
                if (display && display->rt_ready && display->rt.image != VK_NULL_HANDLE) {
                    /* Stage 65d Option 3 substitute — see op_0x01 site for rationale.
                     * Override indexed=true → false; the substitute triangle ignores
                     * the index buffer and draws 3 vertices unconditionally. */
                    st = lagfx_vk_draw_record_and_submit(
                        dev_with_vk->vk, pipeline, &display->rt,
                        false, 3, 1, 0, 0, 0);
                    if (st == LAGFX_OK) {
                        LAGFX_LOG("op_0x06 Option 3 Step 4: drew substitute triangle (guest req indexCount=%u)", index_count);
                        lagfx_display_signal_frame_ready(display);
                    } else {
                        LAGFX_WARN("op_0x06 Option 3 Step 4: lagfx_vk_draw_record_and_submit failed (%d)", (int)st);
                    }
                } else {
                    LAGFX_WARN("op_0x06 Option 3 Step 4: no render target available");
                }
            } else {
                LAGFX_WARN("op_0x06 Option 3 Step 3: lagfx_pipeline_build failed (%d)", (int)st);
            }
        }
    }
#endif
    
    /* TODO: Stage 70 — translate to vkCmdDrawIndexed after binding index buffer. */
    return 0;
}

static int op_draw_indexed_primitives_16(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            uint32_t          task_id,
                                            const uint8_t    *body,
                                            size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 61 — PGCmdDrawIndexedPrimitives16 (12 B), ref=1 */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x07 DrawIndexedPrimitives16 payload too small (%zu < 12)",
                   body_len);
        return 1;
    }
    uint32_t index_count = lagfx_le32(body + 0);
    uint32_t index_buffer_ref = lagfx_le32(body + 4);
    uint32_t index_buffer_offset = lagfx_le32(body + 8);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x07 DrawIndexedPrimitives16 task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x07 DrawIndexedPrimitives16 task_id=%u not live", task_id);
        return 1;
    }

    /* Populate per-task pending draw state. */
    task->pending_draw.valid = true;
    task->pending_draw.indexed = true;              /* Indexed draw */
    task->pending_draw.index_count = index_count;
    task->pending_draw.index_buffer_ref = index_buffer_ref;
    task->pending_draw.base_vertex = 0;             /* Not specified in 0x07 variant */
    task->pending_draw.instance_count = 1u;         /* Default for non-instanced variant */
    task->pending_draw.first_instance = 0u;

    LAGFX_LOG("compute_inner: 0x07 DrawIndexedPrimitives16 count=%u bufRef=0x%x offset=0x%x -> pending_draw.valid=true indexed=true",
              index_count, index_buffer_ref, index_buffer_offset);

    /* Stage 65d Option 3 Step 3: build VkPipeline when draw fires. */
#ifdef LAGFX_HAVE_VULKAN
    if (task->pending_pipeline.valid && task->render_pass_desc.valid) {
        lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
        if (dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized) {
            VkDevice device = dev_with_vk->vk->device;
            lagfx_pipeline_desc_t pdesc = {
                .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
                .fragment_shader      = (VkShaderModule)task->pending_pipeline.fragment_shader,
                .layout               = dev_with_vk->vk->empty_layout,
                /* Stage 65d Option 3: the substitute triangle SPVs were
                 * compiled by the AIR-to-SPIRV translator and use the
                 * function names from triangle.metal as entry points,
                 * not glslang's default "main". */
                .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
                .fragment_entry_point = task->pending_pipeline.translated ? "main" : "triangle_fragment",
                .color_format         = (VkFormat)task->render_pass_desc.color_format,
                .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
            };
            VkPipeline pipeline = VK_NULL_HANDLE;
            lagfx_status_t st = lagfx_pipeline_build(device, &pdesc, &pipeline);
            if (st == LAGFX_OK) {
                LAGFX_LOG("op_0x82 Option 3 Step 3: built VkPipeline=%p for draw count=%u",
                          (void *)pipeline, index_count);
                
                /* Step 4: Stage 65d Option 3 substitute — see op_0x01 for rationale. */
                lagfx_display_t *display = dev_with_vk->displays[0];
                if (display && display->rt_ready && display->rt.image != VK_NULL_HANDLE) {
                    st = lagfx_vk_draw_record_and_submit(
                        dev_with_vk->vk, pipeline, &display->rt,
                        false, 3, 1, 0, 0, 0);
                    if (st == LAGFX_OK) {
                        LAGFX_LOG("op_0x82 Option 3 Step 4: drew substitute triangle (guest req indexCount=%u)", index_count);
                        lagfx_display_signal_frame_ready(display);
                    } else {
                        LAGFX_WARN("op_0x82 Option 3 Step 4: lagfx_vk_draw_record_and_submit failed (%d)", (int)st);
                    }
                } else {
                    LAGFX_WARN("op_0x82 Option 3 Step 4: no render target available");
                }
            } else {
                LAGFX_WARN("op_0x82 Option 3 Step 3: lagfx_pipeline_build failed (%d)", (int)st);
            }
        }
    }
#endif

    /* Stage 70b/c/d observability: one-time full per-task state dump on first draw.
     * Cites: src/protocol/state.h line 96-107 (lagfx_render_pass_desc_t),
     *        line 131-140 (lagfx_pending_draw_t),
     *        line 159-172 (lagfx_bindings_t). */
    static int s_first_draw_dumped = 0;
    if (!s_first_draw_dumped) {
        s_first_draw_dumped = 1;
        LAGFX_LOG("=== first draw observed: full per-task state dump ===");
        LAGFX_LOG("  render_pass: valid=%d color_fmt=%u depth_fmt=%u clear=[%g,%g,%g,%g]",
                  (int)task->render_pass_desc.valid,
                  (unsigned)task->render_pass_desc.color_format,
                  (unsigned)task->render_pass_desc.depth_format,
                  task->render_pass_desc.clear_color[0],
                  task->render_pass_desc.clear_color[1],
                  task->render_pass_desc.clear_color[2],
                  task->render_pass_desc.clear_color[3]);
        LAGFX_LOG("  draw: prim_type=%u count=%u inst=%u base_vtx=%d first_inst=%u idx_ref=0x%x indexed=%d",
                  task->pending_draw.primitive_type,
                  task->pending_draw.index_count,
                  task->pending_draw.instance_count,
                  task->pending_draw.base_vertex,
                  task->pending_draw.first_instance,
                  task->pending_draw.index_buffer_ref,
                  (int)task->pending_draw.indexed);
        for (int i = 0; i < 8; i++) {
            if (task->bindings.vertex_buffers[i].valid) {
                LAGFX_LOG("  bindings: vbuf[%d] ref=0x%x offset=0x%llx",
                          i, task->bindings.vertex_buffers[i].ref,
                          (unsigned long long)task->bindings.vertex_buffers[i].offset);
            }
        }
        for (int i = 0; i < 8; i++) {
            if (task->bindings.fragment_buffers[i].valid) {
                LAGFX_LOG("  bindings: fbuf[%d] ref=0x%x offset=0x%llx",
                          i, task->bindings.fragment_buffers[i].ref,
                          (unsigned long long)task->bindings.fragment_buffers[i].offset);
            }
        }
        for (int i = 0; i < 8; i++) {
            if (task->bindings.vertex_textures[i].valid) {
                LAGFX_LOG("  bindings: vtex[%d] ref=0x%x",
                          i, task->bindings.vertex_textures[i].ref);
            }
        }
        for (int i = 0; i < 8; i++) {
            if (task->bindings.fragment_textures[i].valid) {
                LAGFX_LOG("  bindings: ftex[%d] ref=0x%x",
                          i, task->bindings.fragment_textures[i].ref);
            }
        }
    }

    /* TODO: Stage 70 — translate to vkCmdDrawIndexed after binding index buffer. */
    return 0;
}

/* === Group B — Render-pass + barrier (0x17, 0x1a) ============== */

static int op_render_barrier_scope(lagfx_protocol_t *p,
                                     uint32_t          encoder_type,
                                     uint32_t          task_id,
                                     const uint8_t    *body,
                                     size_t            body_len) {
    (void)p; (void)encoder_type; (void)task_id;
    /* RE: render-decoder-handlers.md line 82 — PGCmdRenderMemoryBarrierScope (4 B), scalar family */
    if (body_len < 4u) {
        LAGFX_WARN("compute_inner: 0x17 RenderBarrierScope payload too small (%zu < 4)",
                   body_len);
        return 1;
    }
    uint32_t packed = lagfx_le32(body + 0);
    LAGFX_LOG("compute_inner: 0x17 RenderBarrierScope packed=0x%x", packed);
    /* TODO: Stage 70 — translate to vkCmdPipelineBarrier2 once render encoder state lives on protocol. */
    return 0;
}

/* Apple Metal pixel format → VkFormat mapping helper.
 * Cites: iosurface.c line 30-41 (implementation).
 * Supported mappings per stage70a-vk-pipeline-build-scoping-2026-05-17.md:
 *   80 -> VK_FORMAT_B8G8R8A8_UNORM (MTLPixelFormatBGRA8Unorm) — high confidence
 *   70 -> VK_FORMAT_R8G8B8A8_UNORM (MTLPixelFormatRGBA8Unorm) — high confidence  
 *   252 -> VK_FORMAT_D32_SFLOAT (MTLPixelFormatDepth32Float) — high confidence
 *   25 -> VK_FORMAT_D16_UNORM (MTLPixelFormatDepth16Unorm) — medium confidence */
static uint32_t apple_format_to_vk(uint32_t fmt) {
#ifdef LAGFX_HAVE_VULKAN
    return (uint32_t)lagfx_metal_pixel_format_to_vk(fmt);
#else
    return 0u;  /* VK_FORMAT_UNDEFINED equivalent; vulkan-disabled stub build */
#endif
}

static int op_render_describe_render_pass(lagfx_protocol_t *p,
                                             uint32_t          encoder_type,
                                             uint32_t          task_id,
                                             const uint8_t    *body,
                                             size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 210 — PGCmdDescribeRenderPass (584 B), POD large.
     *
     * Payload structure inferred from stage70a-vk-pipeline-build-scoping-2026-05-17.md and
     * the PARTIAL confidence rating in render-decoder-handlers.md line 210:
     *
     * Offset  Size  Field                              Notes
     * -----   ----  -----                              -----
     * 0       4     view_count (u32)                   Number of color attachments
     * 4       4     color_format[0] (u32)              Apple pixel format code for first attachment
     * 8       4     depth_format (u32)                 Apple pixel format or 0 if none
     * 12      16    clear_color[4] (f32 x 4)           RGBA clear values
     * 28      4     clear_depth (f32)                  Depth clear value
     * 32      4     render_area_x (u32)                Origin X
     * 36      4     render_area_y (u32)                Origin Y  
     * 40      4     render_area_w (u32)                Extent width
     * 44      4     render_area_h (u32)                Extent height
     * 48+     ...   attachment descriptors (open)      Exact ordering RE'd later — MARKED OPEN
     *
     * The full struct is 584 B = 0x248, but the core fields above are what Stage 70c needs.
     */
    if (!body || body_len < 48u) {
        LAGFX_WARN("compute_inner: 0x1a RenderDescribeRenderPass payload too small (%zu < 48)",
                   body_len);
        return 1;
    }

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x1a RenderDescribeRenderPass task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x1a RenderDescribeRenderPass task_id=%u not live", task_id);
        return 1;
    }

    /* Parse payload fields using lagfx_le32 for alignment-safe reads. */
    uint32_t view_count = lagfx_le32(body + 0);           /* offset 0 — u32 */
    uint32_t color_fmt_raw = lagfx_le32(body + 4);        /* offset 4 — u32 Apple format code */
    uint32_t depth_fmt_raw = lagfx_le32(body + 8);        /* offset 8 — u32 Apple format code or 0 */
    
    float clear_color[4];
    for (int i = 0; i < 4; ++i) {
        uint32_t bits = lagfx_le32(body + 12 + ((size_t)i * 4));  /* offset 12-27 — f32 as u32 bits */
        memcpy(&clear_color[i], &bits, sizeof(float));
    }
    
    float clear_depth;
    {
        uint32_t bits = lagfx_le32(body + 28);                  /* offset 28-31 — f32 as u32 bits */
        memcpy(&clear_depth, &bits, sizeof(float));
    }
    
    uint32_t render_area_x = lagfx_le32(body + 32);           /* offset 32-35 — u32 */
    uint32_t render_area_y = lagfx_le32(body + 36);           /* offset 36-39 — u32 */
    uint32_t render_area_w = lagfx_le32(body + 40);           /* offset 40-43 — u32 */
    uint32_t render_area_h = lagfx_le32(body + 44);           /* offset 44-47 — u32 */

    /* Map Apple format codes to VkFormat (stored as u32 — cast to VkFormat
     * at use site under LAGFX_HAVE_VULKAN). Cites: iosurface.c line 30-41. */
    uint32_t color_format = apple_format_to_vk(color_fmt_raw);
    uint32_t depth_format = (depth_fmt_raw != 0u) ? apple_format_to_vk(depth_fmt_raw) : 0u /* VK_FORMAT_UNDEFINED */;

    /* Log parsed fields for observability. */
    LAGFX_LOG("compute_inner: 0x1a RenderDescribeRenderPass view_count=%u color_fmt=%u(%s) depth_fmt=%u(%s) "
              "clear_color=[%g,%g,%g,%g] clear_depth=%g render_area=%ux%u@(%u,%u)",
              (unsigned)view_count,
              (unsigned)color_fmt_raw,
              (color_fmt_raw == 80u) ? "BGRA8" :
              (color_fmt_raw == 70u) ? "RGBA8" :
              (color_fmt_raw == 252u) ? "D32Float" :
              (color_fmt_raw == 25u) ? "D16Unorm" : "UNKNOWN",
              (unsigned)depth_fmt_raw,
              (depth_fmt_raw == 252u) ? "D32Float" :
              (depth_fmt_raw == 25u) ? "D16Unorm" :
              (depth_fmt_raw == 0u) ? "none" : "UNKNOWN",
              clear_color[0], clear_color[1], clear_color[2], clear_color[3],
              clear_depth,
              (unsigned)render_area_w, (unsigned)render_area_h,
              (unsigned)render_area_x, (unsigned)render_area_y);

    /* Store in per-task render pass descriptor. */
    task->render_pass_desc.valid = true;
    task->render_pass_desc.view_count = view_count;
    task->render_pass_desc.color_format = color_format;
    task->render_pass_desc.depth_format = depth_format;
    memcpy(task->render_pass_desc.clear_color, clear_color, sizeof(clear_color));
    task->render_pass_desc.clear_depth = clear_depth;
    task->render_pass_desc.render_area_x = render_area_x;
    task->render_pass_desc.render_area_y = render_area_y;
    task->render_pass_desc.render_area_w = render_area_w;
    task->render_pass_desc.render_area_h = render_area_h;

    /* OPEN: The remaining 536 B (584 - 48) contain attachment descriptor arrays.
     * Exact field ordering not yet RE'd from guest trace. Log byte at offset 48 for later analysis. */
    if (body_len >= 48u + 4u) {
        uint32_t offset_48 = lagfx_le32(body + 48);
        LAGFX_LOG("compute_inner: 0x1a RenderDescribeRenderPass offset+48=0x%x (OPEN: attachment descriptor layout)",
                  (unsigned)offset_48);
    }

    /* TODO: Stage 70c — consume task->render_pass_desc to construct VkRenderingInfo at vkCmdBeginRendering. */
    return 0;
}

/* === Group C — Buffer/sampler/texture binding (0x6e, 0x6f, 0x70, 0x72, 0x7d, 0x7e) */

static int op_set_fragment_buffers(lagfx_protocol_t *p,
                                      uint32_t          encoder_type,
                                      uint32_t          task_id,
                                      const uint8_t    *body,
                                      size_t            body_len) {
    (void)encoder_type;

    /* RE: render-decoder-handlers.md line 107 — PGCmdSetBuffers (8 B head) + N×PGCmdSetBufferEntry (12 B), array.
     * Wire layout per spec: [count:u32@0-3][firstIndex:u32@4-7]; Entry: [ref:u32@0-3][offset:u64@4-11] = 12 B */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers payload too small (%zu < 8)", body_len);
        return 1;
    }
    uint32_t count = lagfx_le32(body + 0);
    uint32_t first_index = lagfx_le32(body + 4);
    size_t entry_bytes = 12u;
    size_t needed = 8u + (size_t)count * entry_bytes;
    if (body_len < needed) {
        LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers count=%u needs %zu bytes, got %zu",
                   count, needed, body_len);
        return 1;
    }

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers task_id=%u not live", task_id);
        return 1;
    }

    /* Parse and update binding slots. */
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        uint64_t offset = lagfx_le64(e + 4);
        uint32_t slot_index = first_index + i;

        if (slot_index >= LAGFX_MAX_BINDING_SLOTS) {
            LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers slot_index=%u exceeds max %d",
                       slot_index, LAGFX_MAX_BINDING_SLOTS);
            continue;
        }

        task->bindings.fragment_buffers[slot_index].ref = ref;
        task->bindings.fragment_buffers[slot_index].offset = offset;
        task->bindings.fragment_buffers[slot_index].valid = (ref != 0u);

        if (i < 4u) { /* Log first 4 entries */
            LAGFX_LOG("compute_inner: 0x6e SetFragmentBuffers [%u] ref=0x%x offset=%llu valid=%s",
                      slot_index, ref, (unsigned long long)offset, ref != 0u ? "true" : "false");
        }
    }

    /* TODO: Stage 70 — translate to vkCmdBindDescriptorBuffersEXT once descriptor buffer support added. */
    return 0;
}

static int op_set_fragment_buffer_offset(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            uint32_t          task_id,
                                            const uint8_t    *body,
                                            size_t            body_len) {

    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 108 — PGCmdSetBufferOffset (12 B), scalar family.
     * Wire layout per spec: [offset:u64@0-7][padding:u32@8-11][index:u32] */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x6f SetFragmentBufferOffset payload too small (%zu < 12)", body_len);
        return 1;
    }
    uint64_t offset = lagfx_le64(body + 0);
    uint32_t index = lagfx_le32(body + 8);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x6f SetFragmentBufferOffset task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x6f SetFragmentBufferOffset task_id=%u not live", task_id);
        return 1;
    }

    /* Bounds-check index and update offset only. */
    if (index >= LAGFX_MAX_BINDING_SLOTS) {
        LAGFX_WARN("compute_inner: 0x6f SetFragmentBufferOffset index=%u exceeds max %d",
                   index, LAGFX_MAX_BINDING_SLOTS);
        return 1;
    }

    task->bindings.fragment_buffers[index].offset = offset;
    /* Leave ref and valid alone — this opcode only updates offset on a previously-bound slot */

    LAGFX_LOG("compute_inner: 0x6f SetFragmentBufferOffset index=%u offset=0x%llx -> bindings.fragment_buffers[%u].offset updated",
              index, (unsigned long long)offset, index);
    /* TODO: Stage 70 — (offset rebind — no direct Vulkan equiv; re-bind descriptor). */
    return 0;
}

static int op_set_fragment_sampler_states(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            uint32_t          task_id,
                                            const uint8_t    *body,
                                            size_t            body_len) {
    (void)p; (void)encoder_type; (void)task_id;
    /* RE: render-decoder-handlers.md line 109 — PGCmdSetSamplerStates (8 B head) + N×u32 ref, array */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x70 SetFragmentSamplerStates payload too small (%zu < 8)", body_len);
        return 1;
    }
    uint32_t count = lagfx_le32(body + 0);
    uint32_t first_index = lagfx_le32(body + 4);
    size_t entry_bytes = 4u;
    size_t needed = 8u + (size_t)count * entry_bytes;
    if (body_len < needed) {
        LAGFX_WARN("compute_inner: 0x70 SetFragmentSamplerStates count=%u needs %zu bytes, got %zu",
                   count, needed, body_len);
        return 1;
    }
    /* Log head + first ref */
    for (uint32_t i = 0; i < count && i < 4u; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        LAGFX_LOG("compute_inner: 0x70 SetFragmentSamplerStates [%u] ref=0x%x",
                  first_index + i, ref);
    }
    /* TODO: Stage 70 — translate to vkCmdBindDescriptorSets (sampler descriptors). */
    return 0;
}

static int op_set_fragment_textures(lagfx_protocol_t *p,
                                       uint32_t          encoder_type,
                                       uint32_t          task_id,
                                       const uint8_t    *body,
                                       size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 111 — PGCmdSetTextures (8 B head) + N×u32 ref, array.
     * Wire layout per spec: [count:u32@0-3][firstIndex:u32@4-7]; Entry: [ref:u32] = 4 B each */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures payload too small (%zu < 8)", body_len);
        return 1;
    }
    uint32_t count = lagfx_le32(body + 0);
    uint32_t first_index = lagfx_le32(body + 4);
    size_t entry_bytes = 4u;
    size_t needed = 8u + (size_t)count * entry_bytes;
    if (body_len < needed) {
        LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures count=%u needs %zu bytes, got %zu",
                   count, needed, body_len);
        return 1;
    }

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures task_id=%u not live", task_id);
        return 1;
    }

    /* Parse and update binding slots. */
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        uint32_t slot_index = first_index + i;

        if (slot_index >= LAGFX_MAX_BINDING_SLOTS) {
            LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures slot_index=%u exceeds max %d",
                       slot_index, LAGFX_MAX_BINDING_SLOTS);
            continue;
        }

        /* Textures don't have offsets in Apple's binding model */
        task->bindings.fragment_textures[slot_index].ref = ref;
        task->bindings.fragment_textures[slot_index].offset = 0u;
        task->bindings.fragment_textures[slot_index].valid = (ref != 0u);

        if (i < 4u) { /* Log first 4 entries */
            LAGFX_LOG("compute_inner: 0x72 SetFragmentTextures [%u] ref=0x%x valid=%s",
                      slot_index, ref, ref != 0u ? "true" : "false");
        }
    }

    /* TODO: Stage 70 — translate to vkCmdBindDescriptorSets (sampled image descriptors). */
    return 0;
}

static int op_set_vertex_buffers(lagfx_protocol_t *p,
                                    uint32_t          encoder_type,
                                    uint32_t          task_id,
                                    const uint8_t    *body,
                                    size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 132 — PGCmdSetBuffers (8 B head) + N×PGCmdSetBufferEntry (12 B), array.
     * Wire layout per spec: [count:u32@0-3][firstIndex:u32@4-7]; Entry: [ref:u32@0-3][offset:u64@4-11] = 12 B */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers payload too small (%zu < 8)", body_len);
        return 1;
    }
    uint32_t count = lagfx_le32(body + 0);
    uint32_t first_index = lagfx_le32(body + 4);
    size_t entry_bytes = 12u;
    size_t needed = 8u + (size_t)count * entry_bytes;
    if (body_len < needed) {
        LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers count=%u needs %zu bytes, got %zu",
                   count, needed, body_len);
        return 1;
    }

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers task_id=%u not live", task_id);
        return 1;
    }

    /* Parse and update binding slots. */
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        uint64_t offset = lagfx_le64(e + 4);
        uint32_t slot_index = first_index + i;

        if (slot_index >= LAGFX_MAX_BINDING_SLOTS) {
            LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers slot_index=%u exceeds max %d",
                       slot_index, LAGFX_MAX_BINDING_SLOTS);
            continue;
        }

        task->bindings.vertex_buffers[slot_index].ref = ref;
        task->bindings.vertex_buffers[slot_index].offset = offset;
        task->bindings.vertex_buffers[slot_index].valid = (ref != 0u);

        if (i < 4u) { /* Log first 4 entries */
            LAGFX_LOG("compute_inner: 0x7d SetVertexBuffers [%u] ref=0x%x offset=%llu valid=%s",
                      slot_index, ref, (unsigned long long)offset, ref != 0u ? "true" : "false");
        }
    }

    /* TODO: Stage 70 — translate to vkCmdBindVertexBuffers2. */
    return 0;
}

static int op_set_vertex_buffer_offset(lagfx_protocol_t *p,
                                          uint32_t          encoder_type,
                                          uint32_t          task_id,
                                          const uint8_t    *body,
                                          size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 133 — PGCmdSetBufferOffset (12 B), scalar family.
     * Wire layout per spec: [offset:u64@0-7][padding:u32@8-11][index:u32] but index at +8 in practice */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x7e SetVertexBufferOffset payload too small (%zu < 12)", body_len);
        return 1;
    }
    uint64_t offset = lagfx_le64(body + 0);
    uint32_t index = lagfx_le32(body + 8);

    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x7e SetVertexBufferOffset task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x7e SetVertexBufferOffset task_id=%u not live", task_id);
        return 1;
    }

    /* Bounds-check index and update offset only. */
    if (index >= LAGFX_MAX_BINDING_SLOTS) {
        LAGFX_WARN("compute_inner: 0x7e SetVertexBufferOffset index=%u exceeds max %d",
                   index, LAGFX_MAX_BINDING_SLOTS);
        return 1;
    }

    task->bindings.vertex_buffers[index].offset = offset;
    /* Leave ref and valid alone — this opcode only updates offset on a previously-bound slot */

    LAGFX_LOG("compute_inner: 0x7e SetVertexBufferOffset index=%u offset=0x%llx -> bindings.vertex_buffers[%u].offset updated",
              index, (unsigned long long)offset, index);
    /* TODO: Stage 70 — vkCmdBindVertexBuffers2 rebind with new offset. Requires bound pipeline from 0x74 first. */
    return 0;
}

/* === Group D — Pipeline + scissor/viewport (0x74, 0x75, 0x82) == */

static int op_set_render_pipeline_state(lagfx_protocol_t *p,
                                           uint32_t          encoder_type,
                                           uint32_t          task_id,
                                           const uint8_t    *body,
                                           size_t            body_len) {
    (void)encoder_type;
    /* RE: render-decoder-handlers.md line 118 — PGCmdSetRenderPipelineState (4 B), ref=1 */
    if (body_len < 4u) {
        LAGFX_WARN("compute_inner: 0x74 SetRenderPipelineState payload too small (%zu < 4)", body_len);
        return 1;
    }
    uint32_t reference = lagfx_le32(body + 0);

    /* Lookup current task_id by scanning p->tasks table. */
    if (task_id >= LAGFX_MAX_TASKS) {
        LAGFX_WARN("compute_inner: 0x74 SetRenderPipelineState task_id=%u out of range", task_id);
        return 1;
    }
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) {
        LAGFX_WARN("compute_inner: 0x74 SetRenderPipelineState task_id=%u not live", task_id);
        return 1;
    }

    /* Look up resource registry entry for this reference. */
    lagfx_resource_entry_t *entry = NULL;
    const char *registry_status = "MISS";
    const char *type_str = "N/A";
    if (p != NULL && task_id != 0xffffffffu) {
        entry = lagfx_resource_lookup(&p->resources, reference, task_id);
        if (entry != NULL) {
            registry_status = "hit";
            switch (entry->type) {
                case LAGFX_RESOURCE_TYPE_BUFFER:     type_str = "BUFFER"; break;
                case LAGFX_RESOURCE_TYPE_TEXTURE:    type_str = "TEXTURE"; break;
                case LAGFX_RESOURCE_TYPE_PIPELINE:   type_str = "PIPELINE"; break;
                case LAGFX_RESOURCE_TYPE_SAMPLER:    type_str = "SAMPLER"; break;
                case LAGFX_RESOURCE_TYPE_HEAP:       type_str = "HEAP"; break;
                case LAGFX_RESOURCE_TYPE_DEPTH_STENCIL_STATE: type_str = "DEPTH_STENCIL"; break;
                default:                             type_str = "UNKNOWN"; break;
            }
        }
    }

    LAGFX_LOG("compute_inner: 0x74 SetRenderPipelineState ref=0x%x registry=%s type=%s",
              reference, registry_status, type_str);

    /* Phase B step 6/7: env-gated diagnostic using new object resolver helpers. */
    if (getenv("LAGFX_PHASE_B_LOOKUP") != NULL && task->heap_pfn != 0u) {
        static uint32_t lookup_count = 0u;
        if (lookup_count < 20u) {
            uint8_t vert_ref = 0, frag_ref = 0;
            if (lagfx_lookup_pipeline_function_refs(p, task, reference, &vert_ref, &frag_ref)) {
                uint64_t v_gpa = 0; uint32_t v_len = 0;
                uint64_t f_gpa = 0; uint32_t f_len = 0;
                bool got_v = lagfx_lookup_function_bytes(p, task, vert_ref, &v_gpa, &v_len);
                bool got_f = (frag_ref != 0) ? lagfx_lookup_function_bytes(p, task, frag_ref, &f_gpa, &f_len) : false;
                LAGFX_LOG("Phase B lookup pipeline_ref=0x%x vert=0x%x %s(gpa=0x%llx len=%u) frag=0x%x %s(gpa=0x%llx len=%u)",
                          reference, vert_ref, got_v ? "OK" : "FAIL", (unsigned long long)v_gpa, v_len,
                          frag_ref, got_f ? "OK" : (frag_ref == 0 ? "N/A" : "FAIL"), (unsigned long long)f_gpa, f_len);
                lookup_count++;
            }
        }
    }

    /* Phase C step 1: env-gated metallib bytes capture to disk. */
    if (getenv("LAGFX_PHASE_C_CAPTURE") != NULL && task->heap_pfn != 0u) {
        static uint32_t capture_count = 0u;
        static bool dir_created = false;
        if (capture_count < 20u) {
            uint8_t vert_ref = 0, frag_ref = 0;
            if (lagfx_lookup_pipeline_function_refs(p, task, reference, &vert_ref, &frag_ref)) {
                /* Create output directory on first capture. */
                if (!dir_created) {
                    if (mkdir("/tmp/lagfx-metallibs", 0755) != 0 && errno != EEXIST) {
                        LAGFX_WARN("Phase C: mkdir /tmp/lagfx-metallibs failed: %s", strerror(errno));
                    } else {
                        dir_created = true;
                    }
                }

                if (dir_created) {
                    /* Capture vertex metallib. */
                    uint64_t vert_gpa = 0, frag_gpa = 0;
                    uint32_t vert_len = 0, frag_len = 0;
                    
                    bool got_vert = lagfx_lookup_function_bytes(p, task, vert_ref, &vert_gpa, &vert_len);
                    if (got_vert && vert_len > 0) {
                        /* Allocate buffer on heap — metallibs are ~4-6 KB. */
                        uint8_t *buf = (uint8_t *)malloc(vert_len);
                        if (buf != NULL) {
                            lagfx_device_t *dev_for_dma = (lagfx_device_t *)p->dev;
                            bool ok_read = dev_for_dma->desc.shell.read_memory(
                                dev_for_dma->desc.shell.opaque, vert_gpa, vert_len, buf);
                            
                            if (ok_read) {
                                /* Build filename: task<TASK_ID>_pipeline<PIPELINE_REF>_vert_func<VERT_REF>_size<LEN>.metallib */
                                char filename[256];
                                int ret = snprintf(filename, sizeof(filename),
                                    "/tmp/lagfx-metallibs/task%d_pipeline0x%x_vert_func0x%x_size%u.metallib",
                                    (int)task->id, (int)reference, (int)vert_ref, (unsigned)vert_len);
                                
                                if (ret > 0 && ret < (int)sizeof(filename)) {
                                    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                    if (fd >= 0) {
                                        ssize_t written = write(fd, buf, vert_len);
                                        close(fd);
                                        if ((size_t)written == vert_len) {
                                            LAGFX_LOG("Phase C: captured vertex metallib %s (%u bytes)", filename, (unsigned)vert_len);
                                        } else {
                                            LAGFX_WARN("Phase C: write failed for %s (wrote %zd/%u)", filename, written, (unsigned)vert_len);
                                        }
                                    } else {
                                        LAGFX_WARN("Phase C: open failed for %s", filename);
                                    }
                                } else {
                                    LAGFX_WARN("Phase C: filename truncation or overflow");
                                }
                            } else {
                                LAGFX_WARN("Phase C: read_memory failed for vertex metallib gpa=0x%llx len=%u",
                                           (unsigned long long)vert_gpa, vert_len);
                            }
                            free(buf);
                        } else {
                            LAGFX_WARN("Phase C: malloc(%u) failed for vertex metallib", vert_len);
                        }
                    }

                    /* Capture fragment metallif if present. */
                    bool got_frag = false;
                    if (frag_ref != 0 && !got_vert) {
                        got_frag = lagfx_lookup_function_bytes(p, task, frag_ref, &frag_gpa, &frag_len);
                    } else if (frag_ref != 0) {
                        got_frag = lagfx_lookup_function_bytes(p, task, frag_ref, &frag_gpa, &frag_len);
                    }

                    if (got_frag && frag_len > 0 && frag_ref != 0) {
                        uint8_t *buf = (uint8_t *)malloc(frag_len);
                        if (buf != NULL) {
                            lagfx_device_t *dev_for_dma = (lagfx_device_t *)p->dev;
                            bool ok_read = dev_for_dma->desc.shell.read_memory(
                                dev_for_dma->desc.shell.opaque, frag_gpa, frag_len, buf);

                            if (ok_read) {
                                char filename[256];
                                int ret = snprintf(filename, sizeof(filename),
                                    "/tmp/lagfx-metallibs/task%d_pipeline0x%x_frag_func0x%x_size%u.metallib",
                                    (int)task->id, (int)reference, (int)frag_ref, (unsigned)frag_len);

                                if (ret > 0 && ret < (int)sizeof(filename)) {
                                    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                    if (fd >= 0) {
                                        ssize_t written = write(fd, buf, frag_len);
                                        close(fd);
                                        if ((size_t)written == frag_len) {
                                            LAGFX_LOG("Phase C: captured fragment metallib %s (%u bytes)", filename, (unsigned)frag_len);
                                        } else {
                                            LAGFX_WARN("Phase C: write failed for %s (wrote %zd/%u)", filename, written, (unsigned)frag_len);
                                        }
                                    } else {
                                        LAGFX_WARN("Phase C: open failed for %s", filename);
                                    }
                                } else {
                                    LAGFX_WARN("Phase C: filename truncation or overflow");
                                }
                            } else {
                                LAGFX_WARN("Phase C: read_memory failed for fragment metallib gpa=0x%llx len=%u",
                                           (unsigned long long)frag_gpa, frag_len);
                            }
                            free(buf);
                        } else {
                            LAGFX_WARN("Phase C: malloc(%u) failed for fragment metallib", frag_len);
                        }
                    }

                    capture_count++;
                }
            }
        }
    }

#ifdef LAGFX_HAVE_VULKAN
    lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;

    /* Phase 6a: translate AIR → SPIR-V → VkShaderModule for real
     * shaders. Env-gated via LAGFX_PHASE6_TRANSLATE=1 until proven on
     * the boot path. On any failure we fall through to the Option 3
     * substitute below so Stage 75 doesn't regress.
     *
     * Heap-VA-keyed pipeline-ref → (vert_func_ref, frag_func_ref) →
     * metallib bytes per task already works (Phase B/C); we just hand
     * the metallib bytes to lagfx_metallib_extract_functions +
     * lagfx_air_module_open + lagfx_air2spv_translate_module.
     *
     * The full MTLRenderPipelineDescriptor TLV (vertex input layout,
     * blend, depth) is NOT decoded here — see ENTRY-007. Phase 6a
     * accepts that mismatch and reuses lagfx_pipeline_build's
     * hardcoded defaults (matching the substitute path). Phase 6b
     * will land the descriptor decoder. */
    bool phase6_translated = false;
    const char *p6_env = getenv("LAGFX_PHASE6_TRANSLATE");
    /* Treat only "1" as enabled. Bare set-but-empty / "0" / anything
     * else stays on the substitute path. compose.test.yml has
     * `LAGFX_PHASE6_TRANSLATE=${LAGFX_PHASE6_TRANSLATE:-""}` which
     * delivers an empty string when the host shell var is unset —
     * `getenv != NULL` was true even in that case, making P6a
     * silently always-on. */
    bool p6_enabled = (p6_env && p6_env[0] == '1');
    if (p6_enabled &&
        dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized &&
        task->heap_pfn != 0u) {

        uint8_t vert_ref = 0, frag_ref = 0;
        bool lookup_ok = lagfx_lookup_pipeline_function_refs(p, task, reference,
                                                              &vert_ref, &frag_ref);
        if (!lookup_ok) {
            LAGFX_LOG("op_0x74 P6a: lookup_pipeline_function_refs failed for ref=0x%x (heap_pfn=0x%llx) — falling back",
                      reference, (unsigned long long)task->heap_pfn);
        }
        if (lookup_ok) {
            VkDevice vk_device = dev_with_vk->vk->device;
            VkShaderModule v_mod = VK_NULL_HANDLE, f_mod = VK_NULL_HANDLE;

            /* Inline helper: read metallib at vert/frag ref → extract
             * AIR for that stage → translate → vkCreateShaderModule.
             * On failure returns VK_NULL_HANDLE. */
            for (int stage = 0; stage < 2; stage++) {
                uint8_t fn_ref = (stage == 0) ? vert_ref : frag_ref;
                if (fn_ref == 0u) continue;

                uint64_t mlib_gpa = 0; uint32_t mlib_len = 0;
                if (!lagfx_lookup_function_bytes(p, task, fn_ref, &mlib_gpa, &mlib_len)) {
                    LAGFX_WARN("op_0x74 P6a: lookup_function_bytes failed for %s ref=0x%x",
                               stage == 0 ? "vert" : "frag", fn_ref);
                    break;
                }
                if (mlib_len == 0u || mlib_len > (1u << 20)) {
                    LAGFX_WARN("op_0x74 P6a: metallib len %u out of range", mlib_len);
                    break;
                }

                uint8_t *mlib_buf = (uint8_t *)malloc(mlib_len);
                if (!mlib_buf) break;
                if (!dev_with_vk->desc.shell.read_memory(dev_with_vk->desc.shell.opaque,
                                                          mlib_gpa, mlib_len, mlib_buf)) {
                    LAGFX_WARN("op_0x74 P6a: read_memory failed gpa=0x%llx len=%u",
                               (unsigned long long)mlib_gpa, mlib_len);
                    free(mlib_buf);
                    break;
                }

                /* Extract AIR bitcode for this stage from the MTLB
                 * container. We probe with capacity=8 — Apple
                 * metallibs we've seen carry ≤4 functions. */
                lagfx_metallib_function_t fns[8] = {0};
                size_t fn_count = 0;
                lagfx_status_t ext_st = lagfx_metallib_extract_functions(
                    mlib_buf, mlib_len, fns, 8, &fn_count);
                if (ext_st != LAGFX_OK || fn_count == 0u) {
                    LAGFX_WARN("op_0x74 P6a: metallib_extract failed (st=%d count=%zu)",
                               (int)ext_st, fn_count);
                    free(mlib_buf);
                    break;
                }

                /* Pick the matching stage. Apple stores both vertex
                 * and fragment in the SAME metallib for many
                 * pipelines; per-fn_ref lookup may return the whole
                 * blob and we filter here by stage_raw. */
                lagfx_metallib_stage_t want =
                    (stage == 0) ? LAGFX_METALLIB_STAGE_VERTEX
                                 : LAGFX_METALLIB_STAGE_FRAGMENT;
                const lagfx_metallib_function_t *fn = NULL;
                for (size_t i = 0; i < fn_count && i < 8; i++) {
                    if (fns[i].stage == want) { fn = &fns[i]; break; }
                }
                if (!fn || !fn->bitcode || fn->bitcode_len == 0u) {
                    LAGFX_LOG("op_0x74 P6a: no %s function in metallib (count=%zu)",
                              stage == 0 ? "vertex" : "fragment", fn_count);
                    free(mlib_buf);
                    break;
                }

                /* Parse AIR + translate to SPIR-V. */
                lagfx_air_module_t *m = NULL;
                lagfx_status_t open_st = lagfx_air_module_open(
                    fn->bitcode, fn->bitcode_len, &m);
                if (open_st != LAGFX_OK || !m) {
                    LAGFX_WARN("op_0x74 P6a: air_module_open failed (st=%d)", (int)open_st);
                    free(mlib_buf);
                    break;
                }

                uint8_t *spv = NULL;
                size_t   spv_sz = 0;
                lagfx_status_t xl_st = lagfx_air2spv_translate_module(m, &spv, &spv_sz);
                lagfx_air_module_free(m);
                if (xl_st != LAGFX_OK || !spv || spv_sz == 0u) {
                    LAGFX_WARN("op_0x74 P6a: translate_module failed (st=%d)", (int)xl_st);
                    if (spv) free(spv);
                    free(mlib_buf);
                    break;
                }

                /* Hand SPIR-V to lavapipe. */
                VkShaderModuleCreateInfo smci = {
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .codeSize = spv_sz,
                    .pCode = (const uint32_t *)spv,
                };
                VkShaderModule mod = VK_NULL_HANDLE;
                VkResult vr = vkCreateShaderModule(vk_device, &smci, NULL, &mod);
                free(spv);
                free(mlib_buf);
                if (vr != VK_SUCCESS) {
                    LAGFX_WARN("op_0x74 P6a: vkCreateShaderModule failed vr=%d", (int)vr);
                    break;
                }

                if (stage == 0) v_mod = mod;
                else            f_mod = mod;
                LAGFX_LOG("op_0x74 P6a: translated %s shader → VkShaderModule=%p (spv=%zu B)",
                          stage == 0 ? "vertex" : "fragment", (void *)mod, spv_sz);
            }

            /* Both stages successful → commit to pending_pipeline.
             * If only one succeeded, destroy it and fall back. */
            if (v_mod != VK_NULL_HANDLE && f_mod != VK_NULL_HANDLE) {
                /* If we previously installed translated modules for
                 * this task, free them. Substitute modules live on
                 * the device and must not be freed. */
                if (task->pending_pipeline.translated) {
                    if (task->pending_pipeline.vertex_shader)
                        vkDestroyShaderModule(vk_device,
                                              (VkShaderModule)task->pending_pipeline.vertex_shader,
                                              NULL);
                    if (task->pending_pipeline.fragment_shader)
                        vkDestroyShaderModule(vk_device,
                                              (VkShaderModule)task->pending_pipeline.fragment_shader,
                                              NULL);
                }
                task->pending_pipeline.valid           = true;
                task->pending_pipeline.translated      = true;
                task->pending_pipeline.vertex_shader   = (uintptr_t)v_mod;
                task->pending_pipeline.fragment_shader = (uintptr_t)f_mod;
                task->pending_pipeline.reference       = reference;
                phase6_translated = true;
                LAGFX_LOG("op_0x74 P6a: pipeline ref=0x%x using TRANSLATED shaders", reference);
            } else {
                if (v_mod != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, v_mod, NULL);
                if (f_mod != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, f_mod, NULL);
            }
        }
    }

    /* Stage 65d Option 3: substitute path — fallback when Phase 6a is
     * off or any translation step failed. Bundled triangle shaders. */
    if (!phase6_translated &&
        dev_with_vk &&
        dev_with_vk->triangle_vertex_module != VK_NULL_HANDLE &&
        dev_with_vk->triangle_fragment_module != VK_NULL_HANDLE) {
        /* Free previous translated modules if we're switching back. */
        if (task->pending_pipeline.translated) {
            VkDevice vk_device = dev_with_vk->vk->device;
            if (task->pending_pipeline.vertex_shader)
                vkDestroyShaderModule(vk_device,
                                      (VkShaderModule)task->pending_pipeline.vertex_shader,
                                      NULL);
            if (task->pending_pipeline.fragment_shader)
                vkDestroyShaderModule(vk_device,
                                      (VkShaderModule)task->pending_pipeline.fragment_shader,
                                      NULL);
        }
        task->pending_pipeline.valid           = true;
        task->pending_pipeline.translated      = false;
        task->pending_pipeline.vertex_shader   = (uintptr_t)dev_with_vk->triangle_vertex_module;
        task->pending_pipeline.fragment_shader = (uintptr_t)dev_with_vk->triangle_fragment_module;
        task->pending_pipeline.reference       = reference;
        LAGFX_LOG("op_0x74 Option 3: substituted triangle shaders for ref=0x%x", reference);
    } else if (!phase6_translated) {
        LAGFX_LOG("op_0x74 Option 3: triangle modules not loaded (set LAGFX_TRIANGLE_*_SPV); ref=0x%x", reference);
    }
#endif

    /* TODO: Stage 70 — resolve pipeline ref to VkPipeline via resource registry and bind via vkCmdBindShadersEXT once shader objects are in place. */
    return 0;
}

static int op_set_scissor_rect(lagfx_protocol_t *p,
                                 uint32_t          encoder_type,
                                 uint32_t          task_id,
                                 const uint8_t    *body,
                                 size_t            body_len) {
    (void)p; (void)encoder_type; (void)task_id;
    /* RE: render-decoder-handlers.md line 119 — PGCmdSetScissorRect (32 B == MTLScissorRect), POD family */
    if (body_len < 32u) {
        LAGFX_WARN("compute_inner: 0x75 SetScissorRect payload too small (%zu < 32)", body_len);
        return 1;
    }
    uint64_t x = lagfx_le64(body + 0);
    uint64_t y = lagfx_le64(body + 8);
    uint64_t w = lagfx_le64(body + 16);
    uint64_t h = lagfx_le64(body + 24);
    LAGFX_LOG("compute_inner: 0x75 SetScissorRect origin=(%llu,%llu) size=%llux%llu",
              x, y, w, h);
    /* TODO: Stage 70 — translate to vkCmdSetScissor. */
    return 0;
}

static int op_set_viewport(lagfx_protocol_t *p,
                             uint32_t          encoder_type,
                             uint32_t          task_id,
                             const uint8_t    *body,
                             size_t            body_len) {
    (void)p; (void)encoder_type; (void)task_id;
    /* RE: render-decoder-handlers.md line 142 — PGCmdSetViewport (48 B == MTLViewport), POD family */
    if (body_len < 48u) {
        LAGFX_WARN("compute_inner: 0x82 SetViewport payload too small (%zu < 48)", body_len);
        return 1;
    }
    /* Wire format: 6× f64 (originX, originY, width, height, znear, zfar) */
    double origin_x = (double)lagfx_le64(body + 0);
    double origin_y = (double)lagfx_le64(body + 8);
    double width = (double)lagfx_le64(body + 16);
    double height = (double)lagfx_le64(body + 24);
    double znear = (double)lagfx_le64(body + 32);
    double zfar = (double)lagfx_le64(body + 40);
    LAGFX_LOG("compute_inner: 0x82 SetViewport origin=(%g,%g) size=%gx%g z=[%g..%g]",
              origin_x, origin_y, width, height, znear, zfar);
    /* TODO: Stage 70 — translate to vkCmdSetViewport. */
    return 0;
}

/* === Opcode descriptor table ===================================== *
 *
 * Populated from the 2026-05-14 empirical sweep + Task 6 implementation.
 * Names and handlers now have real bodies per render-decoder-handlers.md.
 * All entries validated against observed payload sizes in /tmp/lagfx.log.
 */
static const lagfx_compute_inner_op_desc_t compute_inner_op_table[] = {
    { 0x007e, "SetVertexBufferOffset",           op_set_vertex_buffer_offset },
    { 0x0074, "SetRenderPipelineState",          op_set_render_pipeline_state },
    { 0x007d, "SetVertexBuffers",                op_set_vertex_buffers },
    { 0x0007, "DrawIndexedPrimitives16",         op_draw_indexed_primitives_16 },
    { 0x006e, "SetFragmentBuffers",              op_set_fragment_buffers },
    { 0x0072, "SetFragmentTextures",             op_set_fragment_textures },
    { 0x0075, "SetScissorRect",                  op_set_scissor_rect },
    { 0x0082, "SetViewport",                     op_set_viewport },
    { 0x0070, "SetFragmentSamplerStates",        op_set_fragment_sampler_states },
    { 0x001a, "RenderDescribeRenderPass",        op_render_describe_render_pass },
    { 0x0017, "RenderBarrierScope",              op_render_barrier_scope },
    { 0x0003, "DrawInstancedPrimitives16",       op_draw_instanced_primitives_16 },
    { 0x0006, "DrawIndexedPrimitives64",         op_draw_indexed_primitives_64 },
    { 0x006f, "SetFragmentBufferOffset",         op_set_fragment_buffer_offset },
    { 0x0001, "DrawPrimitives16",                op_draw_primitives_16 },
};

#define LAGFX_COMPUTE_INNER_OP_COUNT \
    (sizeof(compute_inner_op_table) / sizeof(compute_inner_op_table[0]))

static const lagfx_compute_inner_op_desc_t *
find_compute_inner_op_desc(uint32_t opcode) {
    for (size_t i = 0; i < LAGFX_COMPUTE_INNER_OP_COUNT; ++i) {
        if (compute_inner_op_table[i].opcode == opcode) {
            return &compute_inner_op_table[i];
        }
    }
    return NULL;
}

int lagfx_compute_inner_dispatch(lagfx_protocol_t *p,
                                  uint32_t          encoder_type,
                                  uint32_t          task_id,
                                  uint32_t          opcode,
                                  const uint8_t    *body,
                                  size_t            body_len) {
    const lagfx_compute_inner_op_desc_t *desc =
        find_compute_inner_op_desc(opcode);
    if (!desc) {
        LAGFX_TRACE("compute_inner: encType=%u op=0x%04x body_len=%zu "
                    "(UNKNOWN — not in encType=0 table)",
                    (unsigned)encoder_type, (unsigned)opcode, body_len);
        return 1;
    }
    return desc->handler(p, encoder_type, task_id, body, body_len);
}

const char *lagfx_compute_inner_op_name(uint32_t opcode) {
    const lagfx_compute_inner_op_desc_t *desc =
        find_compute_inner_op_desc(opcode);
    return desc ? desc->name : "unknown";
}
