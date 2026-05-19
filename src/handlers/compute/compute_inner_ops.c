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

#include "common/le.h"
#include "common/log.h"
#include "device.h"
#include "protocol/state.h"
#include "vulkan/iosurface.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/draw_record.h"

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
                .vertex_entry_point   = "triangle_vertex",
                .fragment_entry_point = "triangle_fragment",
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
                .vertex_entry_point   = "triangle_vertex",
                .fragment_entry_point = "triangle_fragment",
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
                .vertex_entry_point   = "triangle_vertex",
                .fragment_entry_point = "triangle_fragment",
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
                .vertex_entry_point   = "triangle_vertex",
                .fragment_entry_point = "triangle_fragment",
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

    /* Phase B step 5 V2 — bytes-fetch diagnostic via radix walk.
     * V1 falsified 2026-05-18: heap_pfn=0x1 is a task-VA (page index 1),
     * not a real physical PFN. To read the slot table we must walk the
     * per-task radix tree: slot_va = (task->heap_pfn << 12) + reference*12.
     *
     * V2 algorithm:
     *   1. Translate task-VA slot_va → GPA via lagfx_task_translate
     *   2. Read 12 bytes at slot_gpa → parse (type, flags, bytes_va)
     *   3. Translate task-VA bytes_va → GPA via same radix walker
     *   4. Read first 16 bytes at bytes_gpa → check for "MTLB" magic
     *
     * Env-gated with LAGFX_PHASE_B_DUMP; hard cap of 20 dumps total. */
    /* V2.3 — one-time sweep at first op_0x74 invocation: walk slot[0..127]
     * and dump (slot_addr, type, bytes_va) for every live slot. Goal: find
     * the slot type that holds metallib bytes (MTLB magic at bytes_va).
     * Env-gated via LAGFX_PHASE_B_SWEEP. */
    if (getenv("LAGFX_PHASE_B_SWEEP") != NULL && task->heap_pfn != 0u) {
        static bool swept = false;
        if (!swept) {
            swept = true;
            lagfx_device_t *sdev = (lagfx_device_t *)p->dev;
            uint64_t heap_va = (uint64_t)task->heap_pfn << 12;
            for (uint32_t i = 0; i < 128u; i++) {
                uint64_t sv = heap_va + (uint64_t)i * 12ull;
                uint64_t sg = 0;
                if (!lagfx_task_translate(p, task, sv, &sg)) continue;
                uint8_t s[12] = {0};
                if (!sdev->desc.shell.read_memory(sdev->desc.shell.opaque, sg, 12, s)) continue;
                if (s[0] == 0u) continue;  /* slot not live */
                uint64_t bv = lagfx_le64(s + 4);
                LAGFX_LOG("Phase B step5 V2.3 sweep[ref=0x%x] type=0x%02x flags=%02x%02x%02x bytes_va=0x%llx",
                          i, s[0], s[1], s[2], s[3], (unsigned long long)bv);
                if (bv != 0u) {
                    uint64_t bg = 0;
                    if (lagfx_task_translate(p, task, bv, &bg)) {
                        uint8_t h[16] = {0};
                        if (sdev->desc.shell.read_memory(sdev->desc.shell.opaque, bg, 16, h)) {
                            LAGFX_LOG("Phase B step5 V2.3 sweep[ref=0x%x] bytes_gpa=0x%llx hdr=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x%s",
                                      i, (unsigned long long)bg,
                                      h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
                                      h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15],
                                      (h[0]=='M' && h[1]=='T' && h[2]=='L' && h[3]=='B') ? " *** MTLB ***" : "");
                            /* V2.4 — for type=0x06 (suspected function-bytes/library), the
                             * first u64 at bytes_va looks like a task-VA pointer to the actual
                             * shader bytes. Walk one more level. */
                            if (s[0] == 0x06u) {
                                uint64_t next_va = lagfx_le64(h + 0);
                                if (next_va != 0u) {
                                    uint64_t next_gpa = 0;
                                    if (lagfx_task_translate(p, task, next_va, &next_gpa)) {
                                        uint8_t nh[32] = {0};
                                        if (sdev->desc.shell.read_memory(sdev->desc.shell.opaque, next_gpa, 32, nh)) {
                                            LAGFX_LOG("Phase B step5 V2.4 child[ref=0x%x type=0x06] next_va=0x%llx next_gpa=0x%llx first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x%s",
                                                      i, (unsigned long long)next_va,
                                                      (unsigned long long)next_gpa,
                                                      nh[0], nh[1], nh[2], nh[3], nh[4], nh[5], nh[6], nh[7],
                                                      nh[8], nh[9], nh[10], nh[11], nh[12], nh[13], nh[14], nh[15],
                                                      nh[16], nh[17], nh[18], nh[19], nh[20], nh[21], nh[22], nh[23],
                                                      nh[24], nh[25], nh[26], nh[27], nh[28], nh[29], nh[30], nh[31],
                                                      (nh[0]=='M' && nh[1]=='T' && nh[2]=='L' && nh[3]=='B') ? " *** MTLB AT TYPE-6 ***" : "");
                                        } else {
                                            LAGFX_LOG("Phase B step5 V2.4 child[ref=0x%x] next_gpa=0x%llx read failed",
                                                      i, (unsigned long long)next_gpa);
                                        }
                                    } else {
                                        LAGFX_LOG("Phase B step5 V2.4 child[ref=0x%x] next_va=0x%llx translate failed",
                                                  i, (unsigned long long)next_va);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (getenv("LAGFX_PHASE_B_DUMP") != NULL && task->heap_pfn != 0u) {
        static uint32_t dump_count = 0u;
        if (dump_count < 20u) {
            lagfx_device_t *dev = (lagfx_device_t *)p->dev;
            uint64_t heap_va = (uint64_t)task->heap_pfn << 12;
            uint64_t slot_va = heap_va + (uint64_t)reference * 12ull;
            uint64_t slot_gpa = 0;
            if (lagfx_task_translate(p, task, slot_va, &slot_gpa)) {
                uint8_t slot[12] = {0};
                if (dev->desc.shell.read_memory(dev->desc.shell.opaque, slot_gpa, 12, slot)) {
                    uint8_t apv_type = slot[0];
                    uint64_t bytes_va = lagfx_le64(slot + 4);
                    LAGFX_LOG("Phase B step5 V2 slot[ref=0x%x] slot_va=0x%llx slot_gpa=0x%llx "
                              "type=0x%02x flags=%02x%02x%02x bytes_va=0x%llx",
                              reference, (unsigned long long)slot_va,
                              (unsigned long long)slot_gpa, apv_type,
                              slot[1], slot[2], slot[3],
                              (unsigned long long)bytes_va);
                    if (apv_type != 0u && bytes_va != 0u) {
                        uint64_t bytes_gpa = 0;
                        if (lagfx_task_translate(p, task, bytes_va, &bytes_gpa)) {
                            /* V2.1 — read 128 bytes (was 16); pipeline-state
                             * descriptors are larger than MTLB headers. The
                             * 2026-05-18 V2 deploy showed bytes[ref=0xd] starts
                             * with `0e 00 00 00 3c 00 00 00 0d 00 00 00 29 00 00 00`
                             * — a 16-byte TLV header with inner_type=0x0e,
                             * inner_size=0x3c, ref=reference, payload_len=0x29.
                             * Sub-objectIds for vertex/fragment funcs are
                             * likely in the payload beyond +16. */
                            uint8_t hdr[128] = {0};
                            if (dev->desc.shell.read_memory(dev->desc.shell.opaque, bytes_gpa, 128, hdr)) {
                                LAGFX_LOG("Phase B step5 V2.1 bytes[ref=0x%x] bytes_va=0x%llx "
                                          "bytes_gpa=0x%llx first16=%02x%02x%02x%02x "
                                          "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                                          reference, (unsigned long long)bytes_va,
                                          (unsigned long long)bytes_gpa,
                                          hdr[0], hdr[1], hdr[2], hdr[3],
                                          hdr[4], hdr[5], hdr[6], hdr[7],
                                          hdr[8], hdr[9], hdr[10], hdr[11],
                                          hdr[12], hdr[13], hdr[14], hdr[15]);
                                /* Parse TLV header per V2 observation:
                                 *   u32 inner_type @ +0, u32 inner_size @ +4,
                                 *   u32 ref @ +8, u32 payload_len @ +12 */
                                uint32_t tlv_type = lagfx_le32(hdr + 0);
                                uint32_t tlv_size = lagfx_le32(hdr + 4);
                                uint32_t tlv_ref  = lagfx_le32(hdr + 8);
                                uint32_t tlv_len  = lagfx_le32(hdr + 12);
                                LAGFX_LOG("Phase B step5 V2.1 TLV: type=0x%x size=0x%x ref=0x%x len=0x%x",
                                          tlv_type, tlv_size, tlv_ref, tlv_len);
                                LAGFX_LOG("Phase B step5 V2.1 desc[ref=0x%x] +16..+63: "
                                          "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                                          "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                                          "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                                          reference,
                                          hdr[16], hdr[17], hdr[18], hdr[19],
                                          hdr[20], hdr[21], hdr[22], hdr[23],
                                          hdr[24], hdr[25], hdr[26], hdr[27],
                                          hdr[28], hdr[29], hdr[30], hdr[31],
                                          hdr[32], hdr[33], hdr[34], hdr[35],
                                          hdr[36], hdr[37], hdr[38], hdr[39],
                                          hdr[40], hdr[41], hdr[42], hdr[43],
                                          hdr[44], hdr[45], hdr[46], hdr[47],
                                          hdr[48], hdr[49], hdr[50], hdr[51],
                                          hdr[52], hdr[53], hdr[54], hdr[55],
                                          hdr[56], hdr[57], hdr[58], hdr[59],
                                          hdr[60], hdr[61], hdr[62], hdr[63]);
                                if (hdr[0] == 'M' && hdr[1] == 'T' &&
                                    hdr[2] == 'L' && hdr[3] == 'B') {
                                    LAGFX_LOG("Phase B step5 V2.1 *** MTLB HIT *** ref=0x%x", reference);
                                }
                                /* V2.2 — parse child function objectIds from
                                 * the descriptor and walk slot[func_ref] to
                                 * read THEIR bytes. Per V2.1 observation:
                                 *   byte +30 = 0x04 (tag), byte +31 = vertex_func_ref
                                 *   byte +36 = 0x04 (tag), byte +37 = fragment_func_ref
                                 *   (for render pipelines whose desc starts +16:04 03)
                                 *   Compute pipelines (desc starts +16:03 08) have
                                 *   only the vertex slot used. */
                                if (hdr[14] == 0x04u && hdr[15] != 0u) {
                                    uint8_t func_ref = hdr[15];
                                    uint64_t fslot_va = heap_va + (uint64_t)func_ref * 12ull;
                                    uint64_t fslot_gpa = 0;
                                    if (lagfx_task_translate(p, task, fslot_va, &fslot_gpa)) {
                                        uint8_t fslot[12] = {0};
                                        if (dev->desc.shell.read_memory(dev->desc.shell.opaque, fslot_gpa, 12, fslot)) {
                                            uint8_t ftype = fslot[0];
                                            uint64_t fbytes_va = lagfx_le64(fslot + 4);
                                            LAGFX_LOG("Phase B step5 V2.2 child_slot[func_ref=0x%x] gpa=0x%llx type=0x%02x bytes_va=0x%llx",
                                                      func_ref, (unsigned long long)fslot_gpa, ftype,
                                                      (unsigned long long)fbytes_va);
                                            if (ftype != 0u && fbytes_va != 0u) {
                                                uint64_t fbytes_gpa = 0;
                                                if (lagfx_task_translate(p, task, fbytes_va, &fbytes_gpa)) {
                                                    uint8_t fhdr[32] = {0};
                                                    if (dev->desc.shell.read_memory(dev->desc.shell.opaque, fbytes_gpa, 32, fhdr)) {
                                                        LAGFX_LOG("Phase B step5 V2.2 child_bytes[func_ref=0x%x] gpa=0x%llx first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                                                                  func_ref, (unsigned long long)fbytes_gpa,
                                                                  fhdr[0], fhdr[1], fhdr[2], fhdr[3],
                                                                  fhdr[4], fhdr[5], fhdr[6], fhdr[7],
                                                                  fhdr[8], fhdr[9], fhdr[10], fhdr[11],
                                                                  fhdr[12], fhdr[13], fhdr[14], fhdr[15],
                                                                  fhdr[16], fhdr[17], fhdr[18], fhdr[19],
                                                                  fhdr[20], fhdr[21], fhdr[22], fhdr[23],
                                                                  fhdr[24], fhdr[25], fhdr[26], fhdr[27],
                                                                  fhdr[28], fhdr[29], fhdr[30], fhdr[31]);
                                                        if (fhdr[0] == 'M' && fhdr[1] == 'T' &&
                                                            fhdr[2] == 'L' && fhdr[3] == 'B') {
                                                            LAGFX_LOG("Phase B step5 V2.2 *** MTLB HIT *** func_ref=0x%x pipeline_ref=0x%x", func_ref, reference);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                /* Also try byte +36 for fragment function (render pipelines). */
                                if (hdr[20] == 0x04u && hdr[21] != 0u && hdr[21] != hdr[15]) {
                                    uint8_t func_ref = hdr[21];
                                    uint64_t fslot_va = heap_va + (uint64_t)func_ref * 12ull;
                                    uint64_t fslot_gpa = 0;
                                    if (lagfx_task_translate(p, task, fslot_va, &fslot_gpa)) {
                                        uint8_t fslot[12] = {0};
                                        if (dev->desc.shell.read_memory(dev->desc.shell.opaque, fslot_gpa, 12, fslot)) {
                                            uint8_t ftype = fslot[0];
                                            uint64_t fbytes_va = lagfx_le64(fslot + 4);
                                            LAGFX_LOG("Phase B step5 V2.2 frag_slot[func_ref=0x%x] gpa=0x%llx type=0x%02x bytes_va=0x%llx",
                                                      func_ref, (unsigned long long)fslot_gpa, ftype,
                                                      (unsigned long long)fbytes_va);
                                            if (ftype != 0u && fbytes_va != 0u) {
                                                uint64_t fbytes_gpa = 0;
                                                if (lagfx_task_translate(p, task, fbytes_va, &fbytes_gpa)) {
                                                    uint8_t fhdr[32] = {0};
                                                    if (dev->desc.shell.read_memory(dev->desc.shell.opaque, fbytes_gpa, 32, fhdr)) {
                                                        LAGFX_LOG("Phase B step5 V2.2 frag_bytes[func_ref=0x%x] gpa=0x%llx first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                                                                  func_ref, (unsigned long long)fbytes_gpa,
                                                                  fhdr[0], fhdr[1], fhdr[2], fhdr[3],
                                                                  fhdr[4], fhdr[5], fhdr[6], fhdr[7],
                                                                  fhdr[8], fhdr[9], fhdr[10], fhdr[11],
                                                                  fhdr[12], fhdr[13], fhdr[14], fhdr[15],
                                                                  fhdr[16], fhdr[17], fhdr[18], fhdr[19],
                                                                  fhdr[20], fhdr[21], fhdr[22], fhdr[23],
                                                                  fhdr[24], fhdr[25], fhdr[26], fhdr[27],
                                                                  fhdr[28], fhdr[29], fhdr[30], fhdr[31]);
                                                        if (fhdr[0] == 'M' && fhdr[1] == 'T' &&
                                                            fhdr[2] == 'L' && fhdr[3] == 'B') {
                                                            LAGFX_LOG("Phase B step5 V2.2 *** MTLB HIT (frag) *** func_ref=0x%x pipeline_ref=0x%x", func_ref, reference);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            LAGFX_LOG("Phase B step5 V2.1 bytes_va=0x%llx translate failed", (unsigned long long)bytes_va);
                        }
                    }
                    dump_count++;
                } else {
                    LAGFX_LOG("Phase B step5 V2 slot_gpa=0x%llx read failed", (unsigned long long)slot_gpa);
                    dump_count++;
                }
            } else {
                LAGFX_LOG("Phase B step5 V2 slot_va=0x%llx translate failed (heap_pfn=0x%x root_pfn=0x%llx)",
                          (unsigned long long)slot_va, task->heap_pfn,
                          (unsigned long long)task->root_page_pfn);
                dump_count++;
            }
        }
    }

#ifdef LAGFX_HAVE_VULKAN
    /* Stage 65d Option 3: substitute the device's bundled triangle
     * shaders for every render-pipeline reference. Real metallib
     * capture rides Mach IPC (Session 63 finding); first-pixel work
     * doesn't wait on that. */
    lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
    if (dev_with_vk &&
        dev_with_vk->triangle_vertex_module != VK_NULL_HANDLE &&
        dev_with_vk->triangle_fragment_module != VK_NULL_HANDLE) {
        task->pending_pipeline.valid = true;
        task->pending_pipeline.vertex_shader   = (uintptr_t)dev_with_vk->triangle_vertex_module;
        task->pending_pipeline.fragment_shader = (uintptr_t)dev_with_vk->triangle_fragment_module;
        task->pending_pipeline.reference       = reference;
        LAGFX_LOG("op_0x74 Option 3: substituted triangle shaders for ref=0x%x", reference);
    } else {
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
