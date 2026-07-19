/*
 * libapplegfx-vulkan — Compute inner-opcode handlers + dispatch
 * src/handlers/compute/compute_inner_ops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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
#include "compute_draw_internal.h"
#include "display.h"  /* Full lagfx_display_t definition with rt field */
#include "task_translate.h"
#include "protocol/object_resolver.h"

#include "common/le.h"
#include "common/log.h"
#include "common/policy.h"
#include "device.h"
#include "protocol/state.h"
#include "vulkan/iosurface.h"
#include "vulkan/display_blit.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/draw_record.h"
#include "vulkan/descriptor_layout.h"
#include "air2spv/spv_reflect.h"
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
    /* WIRE FIX (LAGFX_M2_DRAWCOUNT16): PGCmdDrawPrimitives16's vertexCount is a
     * 16-bit field at byte offset 6, NOT a u32 at offset 4. Proven by raw dump:
     * `03 00 00 00 | 00 00 | 06 00` = vertexStart=3, vertexCount=le16@6=6 (one
     * quad); `…18 00` = 24 (4 quads). The old le32@4 read 0x00060000=393216 →
     * 1024 clamped garbage verts → the on-screen band. Gated so M1 is preserved
     * until verified; enable for M2 so draws rasterize only the real geometry. */
    if (LAGFX_POLICY("M2_DRAWCOUNT16"))
        vertex_count = lagfx_le16(body + 6);

    /* DUMP_SPV: hex-dump the raw payload to decode the real field layout — the
     * "vertexCount=0x60000" reading is a misparse (the band = garbage triangles).
     * Show all words so swap / packed / wrong-offset can be identified. */
    if (getenv("LAGFX_DUMP_SPV")) {
        char hx[160]; size_t hn = 0;
        for (size_t q = 0; q < body_len && q < 32u && hn + 3 < sizeof(hx); q++)
            hn += (size_t)snprintf(hx + hn, sizeof(hx) - hn, "%02x ", body[q]);
        LAGFX_LOG("DRAWRAW 0x01 len=%zu enc=%u words[0..3]=%u %u %u %u | bytes: %s",
                  body_len, encoder_type, lagfx_le32(body+0),
                  body_len>=8?lagfx_le32(body+4):0,
                  body_len>=12?lagfx_le32(body+8):0,
                  body_len>=16?lagfx_le32(body+12):0, hx);
    }

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

    /* M1 (a): resource-aware draw via the shared helper (was op_0x01-only). */
#ifdef LAGFX_HAVE_VULKAN
    lagfx_emit_pending_draw(p, task, "op_0x01", vertex_count);
#endif
    return 0;
}

/* 0x00 DrawPrimitives64 — the suspected FULL-SCREEN draw, previously UNHANDLED
 * (silently dropped at TRACE). If the wallpaper/background is drawn via 0x00,
 * not handling it = black background. Raw-dump the payload to decode the count
 * field (RE: 20 B), then emit the draw via the shared path. Always logs when it
 * fires so we learn whether 0x00 is even used. */
static int op_draw_primitives_64(lagfx_protocol_t *p,
                                   uint32_t          encoder_type,
                                   uint32_t          task_id,
                                   const uint8_t    *body,
                                   size_t            body_len) {
    (void)encoder_type;
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x00 DrawPrimitives64 payload too small (%zu)", body_len);
        return 1;
    }
    if (task_id >= LAGFX_MAX_TASKS) return 1;
    lagfx_task_entry_t *task = &p->tasks[task_id];
    if (!task->live) return 1;

    char hx[160]; size_t hn = 0;
    for (size_t q = 0; q < body_len && q < 32u && hn + 3 < sizeof(hx); q++)
        hn += (size_t)snprintf(hx + hn, sizeof(hx) - hn, "%02x ", body[q]);
    /* Candidate count fields (decode from the dump): u32@0, u32@4, u32@8,
     * u16@6 (matching the 0x01 layout). Default to u32@4 (vertexCount slot). */
    uint32_t f0 = lagfx_le32(body + 0);
    uint32_t f4 = body_len >= 8 ? lagfx_le32(body + 4) : 0;
    uint32_t f8 = body_len >= 12 ? lagfx_le32(body + 8) : 0;
    uint32_t c16 = body_len >= 8 ? lagfx_le16(body + 6) : 0;
    LAGFX_LOG("compute_inner: 0x00 DrawPrimitives64 FIRES len=%zu words=%u %u %u u16@6=%u bytes: %s",
              body_len, f0, f4, f8, c16, hx);

    /* Heuristic count: prefer the u16@6 (proven for 0x01) if it's a sane small
     * count, else f4. Env override to probe (LAGFX_M2_DC64_FIELD = 0/4/6). */
    uint32_t vcount = (c16 > 0u && c16 < 0x10000u) ? c16 : f4;
    const char *fld = getenv("LAGFX_M2_DC64_FIELD");
    if (fld) { uint32_t which = (uint32_t)strtoul(fld, NULL, 0);
        vcount = which == 0u ? f0 : which == 6u ? c16 : f4; }

    task->pending_draw.valid = true;
    task->pending_draw.indexed = false;
    task->pending_draw.index_count = vcount;
    task->pending_draw.base_vertex = 0;
    task->pending_draw.instance_count = 1u;
    task->pending_draw.first_instance = 0u;
#ifdef LAGFX_HAVE_VULKAN
    if (getenv("LAGFX_M2_DRAW64"))
        lagfx_emit_pending_draw(p, task, "op_0x00", vcount);
#endif
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

    /* M1 (a): resource-aware draw via the shared helper (was substitute-only). */
#ifdef LAGFX_HAVE_VULKAN
    lagfx_emit_pending_draw(p, task, "op_0x03", vertex_count);
#endif
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

    /* M1 (a): resource-aware draw via the shared helper (was substitute-only).
     * Indexed geometry drawn unindexed for now — see lagfx_emit_pending_draw. */
#ifdef LAGFX_HAVE_VULKAN
    lagfx_emit_pending_draw(p, task, "op_0x06", index_count);
#endif
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

    /* M1 (a): resource-aware draw via the shared helper. 0x07 is the DOMINANT
     * draw (~22439) and was substitute-only (with mislabeled op_0x82 logs) —
     * now it renders translated resource pipelines like 0x01. */
#ifdef LAGFX_HAVE_VULKAN
    lagfx_emit_pending_draw(p, task, "op_0x07", index_count);
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

    /* M2 per-pass RT: parse the color-attachment TARGET ref from the attachment
     * region (offset 48+). RP_RAW decode showed target refs at fixed offsets
     * (the color attachment surface, type 0x05 view / 0x03 texture). Take the
     * first ref in the region that resolves to a renderable surface object
     * (type 0x03/0x05); 0 = scanout. This selects where the pass's draws go. */
    /* NOTE: view_count/render_area are MIS-PARSED (0x1a wire format PARTIAL — Apple
     * uses doubles; observed view_count=0 + render_area=0x0 even for real draw
     * passes). So do NOT gate target parsing on view_count. Scan the attachment
     * region (offset 48..128; RP_RAW showed target refs @60/@76/@80) for the first
     * ref resolving to a renderable surface (type 0x03/0x05). */
    task->render_pass_desc.target_ref = 0u;
    if (body_len >= 52u) {
        for (size_t off = 48u; off + 4u <= body_len && off < 128u; off += 4u) {
            uint32_t v = lagfx_le32(body + off);
            if (v == 0u || v > 0xffffu) continue;
            uint8_t rt = 0; uint64_t rva = 0, rgpa = 0;
            if (lagfx_resolve_object_data(p, task, v, &rt, &rva, &rgpa) && rva != 0u
                && (rt == 0x03u || rt == 0x05u)) {
                task->render_pass_desc.target_ref = v;
                if (LAGFX_POLICY("M2_PERPASS"))
                    LAGFX_LOG("0x1a target_ref=0x%x (t%02x) @off=%zu", v, rt, off);
                break;
            }
        }
    }

    /* OPEN: The remaining 536 B (584 - 48) contain attachment descriptor arrays.
     * Exact field ordering not yet RE'd from guest trace. Log byte at offset 48 for later analysis. */
    if (body_len >= 48u + 4u) {
        uint32_t offset_48 = lagfx_le32(body + 48);
        LAGFX_LOG("compute_inner: 0x1a RenderDescribeRenderPass offset+48=0x%x (OPEN: attachment descriptor layout)",
                  (unsigned)offset_48);
    }

    /* M1 wire-format RE: the current offsets are GUESSED (RE doc marks 0x1a
     * PARTIAL). The parsed values are garbage (view_count=0, color_fmt=0,
     * render_area_x=0x3FF00000 = high word of double 1.0), strongly implying
     * Apple's MTLClearColor/MTLViewport DOUBLE fields read as u32/f32. Dump
     * the head of the real payload as u32 words + f64 doubles to recover the
     * true layout. Env-gated to avoid spamming the 8168×/run hot path. */
    if (getenv("LAGFX_DUMP_RP") != NULL) {
        size_t n = body_len < 96u ? body_len : 96u;
        for (size_t off = 0; off + 8u <= n; off += 8u) {
            uint32_t w0 = lagfx_le32(body + off);
            uint32_t w1 = lagfx_le32(body + off + 4u);
            uint64_t q  = lagfx_le64(body + off);
            double d; memcpy(&d, &q, sizeof(d));
            float f0; memcpy(&f0, &w0, sizeof(f0));
            LAGFX_LOG("0x1a RAW @%02zu: u32=[0x%08x 0x%08x] f64=%g f32@%02zu=%g",
                      off, (unsigned)w0, (unsigned)w1, d, off, (double)f0);
        }
    }
    /* M2 RP-TARGET (LAGFX_RP_TARGET): the attachment descriptors (offset 48+)
     * carry the TARGET IOSurface ref(s) for this render pass — needed for
     * per-pass render targets (route the wallpaper draw into its IOSurface so a
     * later composite samples real content, not black). The layout is un-RE'd;
     * scan the full payload for small u32 words that match a registered texture
     * resource (a plausible target ref) and log them with their byte offset, so
     * we can pin the attachment-ref field. */
    if (getenv("LAGFX_RP_TARGET") != NULL && view_count > 0u) {
        char hits[384]; size_t hl = 0;
        for (size_t off = 48u; off + 4u <= body_len && hl < sizeof(hits) - 48u; off += 4u) {
            uint32_t v = lagfx_le32(body + off);
            if (v == 0u || v > 0xffffu) continue;
            uint8_t tt = 0; uint64_t tva = 0, tgpa = 0;
            if (!(lagfx_resolve_object_data(p, task, v, &tt, &tva, &tgpa) && tva != 0u)) continue;
            /* Resolve the target ref's BACKING PFN (scan its descriptor like
             * BACKREF) — the decisive test for keying per-pass render targets by
             * backing-GPA: if a render TARGET (view 0x7/0x9) shares a backing PFN
             * with a SAMPLED texture (ref=0x10, PFN0x741), then rendering into that
             * backing fills what the composite samples → wallpaper renders. */
            uint8_t td2[64] = {0}; uint64_t bpfn = 0;
            if (lagfx_task_read_virtual(p, task, tva, sizeof(td2), td2)) {
                for (int e = 0; e < 4; e++) {
                    uint64_t es = lagfx_le64(td2 + (size_t)e*16u);
                    uint64_t ep = lagfx_le64(td2 + (size_t)e*16u + 8u) & 0xffffffffull;
                    if (ep < 0x10u || ep > 0xfffffu) continue;
                    /* type-0x05 view: follow the {size,PFN} entry whose first words
                     * point to a sub-object; else take the direct PFN. */
                    if (es >= 4u) { bpfn = ep; break; }
                }
            }
            hl += (size_t)snprintf(hits + hl, sizeof(hits) - hl,
                                   "@%zu=0x%x(t%02x,bPFN0x%llx) ", off, v, tt,
                                   (unsigned long long)bpfn);
        }
        LAGFX_LOG("0x1a RP_TARGET vc=%u target-refs+backingPFN: %s", view_count, hl ? hits : "(none)");
    }
    /* M2 VIEW_DESC: for each render-target ref in the attachment region, dump its
     * FULL heap descriptor (u32 words) + FOLLOW the handle one level — read the
     * content at its first PFN and resolve any object refs there — to find the
     * REAL backing object/GPA. Decisive: if a render-target view's real backing
     * GPA == a sampled texture's backing GPA (ref=0x10 → 0x741000), per-pass RTs
     * are implementable keyed by backing-GPA (render the wallpaper pass into that
     * VkImage; the composite samples the same GPA). */
    if (getenv("LAGFX_VIEW_DESC") != NULL && view_count > 0u) {
        for (size_t off = 48u; off + 4u <= body_len && off < 96u; off += 4u) {
            uint32_t v = lagfx_le32(body + off);
            if (v == 0u || v > 0xffffu) continue;
            uint8_t tt = 0; uint64_t tva = 0, tgpa = 0;
            if (!(lagfx_resolve_object_data(p, task, v, &tt, &tva, &tgpa) && tva != 0u)) continue;
            if (tt != 0x05u && tt != 0x03u) continue;  /* views + textures only */
            uint8_t d[64] = {0};
            if (!lagfx_task_read_virtual(p, task, tva, sizeof(d), d)) continue;
            LAGFX_LOG("VIEW_DESC ref=0x%x t%02x desc u32: %x %x %x %x | %x %x %x %x | %x %x %x %x | %x %x %x %x",
                      v, tt, lagfx_le32(d+0),lagfx_le32(d+4),lagfx_le32(d+8),lagfx_le32(d+12),
                      lagfx_le32(d+16),lagfx_le32(d+20),lagfx_le32(d+24),lagfx_le32(d+28),
                      lagfx_le32(d+32),lagfx_le32(d+36),lagfx_le32(d+40),lagfx_le32(d+44),
                      lagfx_le32(d+48),lagfx_le32(d+52),lagfx_le32(d+56),lagfx_le32(d+60));
            /* follow handle: read the first PFN's page, resolve any object refs */
            uint64_t hpfn = lagfx_le64(d + 8) & 0xffffffffull;
            if (hpfn >= 0x10u && hpfn <= 0xfffffu) {
                uint8_t h[32] = {0};
                if (lagfx_task_read_virtual(p, task, hpfn << 12, sizeof(h), h)) {
                    char fb[160]; size_t fl = 0;
                    for (int w = 0; w < 8; w++) {
                        uint32_t cv = lagfx_le32(h + w*4);
                        if (cv == 0u || cv > 0xffffu) continue;
                        uint8_t ct = 0; uint64_t cva = 0, cgpa = 0;
                        if (lagfx_resolve_object_data(p, task, cv, &ct, &cva, &cgpa) && cva != 0u)
                            fl += (size_t)snprintf(fb+fl, sizeof(fb)-fl, "0x%x(t%02x) ", cv, ct);
                    }
                    LAGFX_LOG("VIEW_DESC ref=0x%x handle@PFN0x%llx resolves: %s", v,
                              (unsigned long long)hpfn, fl ? fb : "(none)");
                }
            }
        }
    }
    /* M2 RP-RAW (LAGFX_RP_RAW): raw u32 dump of the attachment region (48..176) so
     * the 6-attachment layout can be decoded by hand — each color attachment slot
     * (view_count=6, ~89 B span) should carry its target IOSurface ref + load/store
     * op + clear. The resolve-filtered scan misses targets not yet in the registry. */
    if (getenv("LAGFX_RP_RAW") != NULL && view_count >= 1u) {
        for (size_t base = 48u; base + 32u <= body_len && base < 176u; base += 32u) {
            LAGFX_LOG("0x1a RP_RAW @%03zu: %08x %08x %08x %08x %08x %08x %08x %08x", base,
                      lagfx_le32(body+base+0),  lagfx_le32(body+base+4),
                      lagfx_le32(body+base+8),  lagfx_le32(body+base+12),
                      lagfx_le32(body+base+16), lagfx_le32(body+base+20),
                      lagfx_le32(body+base+24), lagfx_le32(body+base+28));
        }
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
     * Wire layout per spec: [firstIndex:u32@0-3][count:u32@4-7] (live 15.7.5 — rule 21; was mis-documented as count-first); Entry: [ref:u32@0-3][offset:u64@4-11] = 12 B */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x6e SetFragmentBuffers payload too small (%zu < 8)", body_len);
        return 1;
    }
    if (LAGFX_POLICY("PHASE6_TRANSLATE")) {
        char _hx[160]; size_t _hn = 0;
        for (size_t _k = 0; _k < body_len && _hn + 3 < sizeof(_hx); _k++)
            _hn += (size_t)snprintf(_hx + _hn, sizeof(_hx) - _hn, "%02x ", body[_k]);
        LAGFX_LOG("WIRE_RAW 0x6e body_len=%zu: %s", body_len, _hx);
    }
    uint32_t first_index = lagfx_le32(body + 0);
    uint32_t count = lagfx_le32(body + 4);
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
    uint32_t first_index = lagfx_le32(body + 0);
    uint32_t count = lagfx_le32(body + 4);
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
     * Wire layout per spec: [firstIndex:u32@0-3][count:u32@4-7] (live 15.7.5 — rule 21; was mis-documented as count-first); Entry: [ref:u32] = 4 B each */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x72 SetFragmentTextures payload too small (%zu < 8)", body_len);
        return 1;
    }
    if (LAGFX_POLICY("PHASE6_TRANSLATE")) {
        char _hx[160]; size_t _hn = 0;
        for (size_t _k = 0; _k < body_len && _hn + 3 < sizeof(_hx); _k++)
            _hn += (size_t)snprintf(_hx + _hn, sizeof(_hx) - _hn, "%02x ", body[_k]);
        LAGFX_LOG("WIRE_RAW 0x72 body_len=%zu: %s", body_len, _hx);
    }
    uint32_t first_index = lagfx_le32(body + 0);
    uint32_t count = lagfx_le32(body + 4);
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

        /* M2 TEXSCAN (LAGFX_M2_TEXSCAN): probe EVERY bound fragment texture's
         * type + pixel content at bind time — incl. the slot-3 login textures
         * (0x25/0x2a/0x2e/0x32) the composites sample but we never reached via
         * the SAMPLED_IMAGE path. Tells us which login-UI textures are real
         * (type 0x03/0x04) with non-black pixels = directly backable for the
         * avatar/field/wallpaper. Read-only; gated. */
        if (ref != 0u && getenv("LAGFX_M2_TEXSCAN")) {
            uint8_t xt = 0; uint64_t xva = 0, xgpa = 0;
            uint8_t xd[64] = {0};
            uint64_t xpfn = 0, xsz = 0;
            if (lagfx_resolve_object_data(p, task, ref, &xt, &xva, &xgpa)
                && xva != 0u && lagfx_task_read_virtual(p, task, xva, sizeof(xd), xd)) {
                for (int e = 0; e < 4; e++) {
                    uint64_t es = lagfx_le64(xd + (size_t)e * 16u);
                    uint64_t ep = lagfx_le64(xd + (size_t)e * 16u + 8u) & 0xffffffffull;
                    if (ep < 0x10u || ep > 0xfffffu || es < 4u) continue;
                    xpfn = ep; xsz = es; break;
                }
            }
            uint32_t xnb = 0; uint8_t xpix[4096] = {0};
            if (xpfn != 0u
                && lagfx_task_read_virtual(p, task, xpfn << 12, sizeof(xpix), xpix)) {
                for (size_t q = 0; q + 4 <= sizeof(xpix); q += 4)
                    if (xpix[q] | xpix[q+1] | xpix[q+2]) xnb++;
            }
            LAGFX_LOG("M2 TEXSCAN tex[%u] ref=0x%x type=0x%02x PFN0x%llx sz=%llu "
                      "nonblack=%u/%zu", slot_index, ref, xt,
                      (unsigned long long)xpfn, (unsigned long long)xsz, xnb, sizeof(xpix)/4);
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
     * Wire layout per spec: [firstIndex:u32@0-3][count:u32@4-7] (live 15.7.5 — rule 21; was mis-documented as count-first); Entry: [ref:u32@0-3][offset:u64@4-11] = 12 B */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x7d SetVertexBuffers payload too small (%zu < 8)", body_len);
        return 1;
    }
    if (LAGFX_POLICY("PHASE6_TRANSLATE")) {
        char _hx[160]; size_t _hn = 0;
        for (size_t _k = 0; _k < body_len && _hn + 3 < sizeof(_hx); _k++)
            _hn += (size_t)snprintf(_hx + _hn, sizeof(_hx) - _hn, "%02x ", body[_k]);
        LAGFX_LOG("WIRE_RAW 0x7d body_len=%zu: %s", body_len, _hx);
    }
    uint32_t first_index = lagfx_le32(body + 0);
    uint32_t count = lagfx_le32(body + 4);
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

        /* Resource-binding RE (env-gated): resolve the bound buffer ref to
         * its guest data and dump the first bytes (read page-aware), to
         * confirm the resolver works and reveal the buffer-object layout —
         * the input to draw-time descriptor binding. */
        if (ref != 0u && i < 4u &&
            (getenv("LAGFX_RE_BUFFERS") != NULL ||
             LAGFX_POLICY("PHASE6_TRANSLATE"))) {
            uint8_t btype = 0; uint64_t bva = 0, bgpa = 0;
            if (lagfx_resolve_object_data(p, task, ref, &btype, &bva, &bgpa)) {
                uint8_t data[64] = {0};
                bool ok = (bva != 0u) &&
                          lagfx_task_read_virtual(p, task, bva + offset, sizeof(data), data);
                char hex[210]; size_t hn = 0;
                for (size_t k = 0; ok && k < sizeof(data) && hn + 3 < sizeof(hex); k++)
                    hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ", data[k]);
                LAGFX_LOG("RE_BUF: ref=0x%x type=0x%02x data_va=0x%llx gpa=0x%llx "
                          "read=%s bytes[%llu..]: %s",
                          ref, btype, (unsigned long long)bva, (unsigned long long)bgpa,
                          ok ? "ok" : "FAIL", (unsigned long long)offset, ok ? hex : "");
            } else {
                LAGFX_LOG("RE_BUF: ref=0x%x resolve FAILED", ref);
            }
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

    /* Phase 6b — consult the per-task active-objects set populated
     * by 0x25 CmdSetObjectAndPlacementList. The pipeline `reference`
     * here is the same value space as the objectId the kext publishes
     * via 0x25 — both ultimately map to the heap-VA resolver slots.
     * If the reference appears in the active set, that's positive
     * evidence the kext has finished registering the object and the
     * heap-VA lookup should succeed. */
    bool obj_active = false;
    for (uint32_t i = 0; i < task->active_objects.count; i++) {
        if (task->active_objects.object_ids[i] == reference) {
            obj_active = true;
            break;
        }
    }

    LAGFX_LOG("compute_inner: 0x74 SetRenderPipelineState ref=0x%x registry=%s type=%s active=%s",
              reference, registry_status, type_str, obj_active ? "yes" : "no");

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
    /* Real-shader translation is default-on; LAGFX_DISABLE_PHASE6_TRANSLATE
     * reverts to the substitute path. */
    bool p6_enabled = LAGFX_POLICY("PHASE6_TRANSLATE");
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
            /* Keep each stage's SPIR-V alive past module creation so we can
             * reflect descriptor bindings and build a matching pipeline
             * layout once both stages are translated. Freed after the loop. */
            uint8_t *spv_keep[2] = { NULL, NULL };
            size_t   spv_keep_sz[2] = { 0, 0 };

            /* Inline helper: read metallib at vert/frag ref → extract
             * AIR for that stage → translate → vkCreateShaderModule.
             * On failure returns VK_NULL_HANDLE. */
            for (int stage = 0; stage < 2; stage++) {
                uint8_t fn_ref = (stage == 0) ? vert_ref : frag_ref;
                if (fn_ref == 0u) continue;

                uint64_t mlib_gpa = 0; uint32_t mlib_len = 0; uint64_t mlib_va = 0;
                if (!lagfx_lookup_function_bytes(p, task, fn_ref, &mlib_gpa, &mlib_len, &mlib_va)) {
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
                /* Page-aware read: the metallib is contiguous in the task's
                 * VIRTUAL address space but its GPA pages are not, so a flat
                 * read_memory(gpa, len) corrupts the bitcode past the first
                 * page boundary -> the reader/translate then chokes (and can
                 * crash) on a real multi-page guest shader. */
                if (!lagfx_task_read_virtual(p, task, mlib_va, mlib_len, mlib_buf)) {
                    LAGFX_WARN("op_0x74 P6a: read_virtual failed va=0x%llx len=%u",
                               (unsigned long long)mlib_va, mlib_len);
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

                /* M1 TEXTURE-COMPOSITE (LAGFX_M1_TEXCOMP): give the FRAGMENT
                 * stage a disjoint binding range so the merged set-0 layout
                 * doesn't collide vertex `[[buffer(n)]]` with fragment
                 * `[[texture(n)]]`/`[[buffer(n)]]` (both emitted at set0 from
                 * binding 0). Without this the merge drops the fragment's
                 * texture/colour binding → composites flagged inconsistent and
                 * substituted (no real content ever drawn). The draw site
                 * demuxes on the same LAGFX_FRAG_BINDING_BASE. */
                if (stage == 1 && LAGFX_POLICY("M1_TEXCOMP")) {
                    lagfx_spv_offset_bindings(spv, spv_sz, LAGFX_FRAG_BINDING_BASE);
                }

                /* Hand SPIR-V to lavapipe. */
                VkShaderModuleCreateInfo smci = {
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .codeSize = spv_sz,
                    .pCode = (const uint32_t *)spv,
                };
                VkShaderModule mod = VK_NULL_HANDLE;
                VkResult vr = vkCreateShaderModule(vk_device, &smci, NULL, &mod);
                free(mlib_buf);
                if (vr != VK_SUCCESS) {
                    LAGFX_WARN("op_0x74 P6a: vkCreateShaderModule failed vr=%d", (int)vr);
                    free(spv);
                    break;
                }
                /* Retain the SPIR-V for descriptor reflection (freed after
                 * the stage loop). */
                spv_keep[stage] = spv;
                spv_keep_sz[stage] = spv_sz;

                if (stage == 0) v_mod = mod;
                else            f_mod = mod;
                LAGFX_LOG("op_0x74 P6a: translated %s shader → VkShaderModule=%p (spv=%zu B)",
                          stage == 0 ? "vertex" : "fragment", (void *)mod, spv_sz);
                /* M2 DUMP_SPV: write the translated SPIR-V to /tmp so it can be
                 * spirv-dis'd offline — to verify whether the UberComposite vertex
                 * shader actually WRITES the texcoord Output varying (else the
                 * fragment's UV is constant → uniform sample). Gated, write-once
                 * per ref+stage. */
                if (getenv("LAGFX_DUMP_SPV")) {
                    char path[64];
                    snprintf(path, sizeof(path), "/tmp/spv-0x%x-%s.spv",
                             reference, stage == 0 ? "vtx" : "frag");
                    FILE *df = fopen(path, "wb");
                    if (df) { fwrite(spv, 1, spv_sz, df); fclose(df);
                        LAGFX_LOG("op_0x74 DUMP_SPV wrote %s (%zu B)", path, spv_sz); }
                }
            }

            /* Both stages successful → commit to pending_pipeline.
             * If only one succeeded, destroy it and fall back. */
            if (v_mod != VK_NULL_HANDLE && f_mod != VK_NULL_HANDLE) {
                /* Reflect both stages' SPIR-V to classify the pipeline BEFORE
                 * committing. A non-NULL pipeline layout means the translated
                 * shaders read descriptor set 0 (textures/buffers). Draw-time
                 * descriptor binding is not implemented yet: drawing such a
                 * pipeline with the device empty_layout and NO bound descriptor
                 * sets segfaults lavapipe (confirmed — the op_0x01 draw site
                 * hardcodes empty_layout). Until binding lands, resource-using
                 * translated pipelines fall back to the substitute triangle so
                 * the guest stays stable; resource-free translated shaders still
                 * render for real. */
                const uint8_t *blobs[2] = { spv_keep[0], spv_keep[1] };
                const size_t   lens[2]  = { spv_keep_sz[0], spv_keep_sz[1] };
                VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
                VkPipelineLayout pl = VK_NULL_HANDLE;
                bool reflect_ok = (lagfx_build_pipeline_layout_from_spv(
                                       vk_device, blobs, lens, 2, &dsl, &pl) == LAGFX_OK);
                bool has_resources = reflect_ok && (pl != VK_NULL_HANDLE);

                /* Recover the merged set-0 binding list (binding + kind) so the
                 * draw site can populate a descriptor set from the guest's
                 * bound resources. */
                lagfx_spv_binding_t rb[16]; size_t nrb = 0;
                if (has_resources) {
                    for (int s = 0; s < 2; s++) {
                        if (!spv_keep[s]) continue;
                        lagfx_spv_binding_t tmp[16];
                        size_t nt = lagfx_spv_reflect_bindings(spv_keep[s], spv_keep_sz[s], tmp, 16);
                        if (nt > 16) nt = 16;
                        for (size_t t = 0; t < nt && nrb < 16; t++) {
                            if (tmp[t].set != 0u) continue;
                            bool seen = false;
                            for (size_t u = 0; u < nrb; u++)
                                if (rb[u].binding == tmp[t].binding) { seen = true; break; }
                            if (!seen) rb[nrb++] = tmp[t];
                        }
                    }
                }
                /* M1 (c): resource-using pipelines are drawable when every
                 * reflected binding is one the draw site can satisfy — storage
                 * buffers (guest data), sampled images (IOSurface views), or
                 * samplers (shared default). Any other kind → substitute. No
                 * pool → substitute. (Was buffer-only through Stage 85b.) */
                bool drawable = has_resources && nrb > 0 &&
                                dev_with_vk->vk->draw_desc_pool != VK_NULL_HANDLE;
                for (size_t u = 0; drawable && u < nrb; u++)
                    if (rb[u].kind != LAGFX_SPV_BINDING_STORAGE_BUFFER
                        && rb[u].kind != LAGFX_SPV_BINDING_SAMPLED_IMAGE
                        && rb[u].kind != LAGFX_SPV_BINDING_SAMPLER)
                        drawable = false;

                /* INCONSISTENT-SHADER GUARD: if the fragment SPIR-V SAMPLES a
                 * texture but reflection surfaced NO SAMPLED_IMAGE binding, the
                 * translator dropped the texture binding (live composite shaders
                 * still do this for some variants). The descriptor layout then
                 * lacks the binding the shader samples → lavapipe NULL-deref at
                 * draw. Such a pipeline is NOT drawable → falls back to the
                 * substitute (no crash). ColorFill (no sample) is unaffected. */
                if (drawable && spv_keep[1]) {
                    bool frag_samples = lagfx_spv_has_image_sample(spv_keep[1], spv_keep_sz[1]);
                    bool have_image = false;
                    for (size_t u = 0; u < nrb; u++)
                        if (rb[u].kind == LAGFX_SPV_BINDING_SAMPLED_IMAGE) { have_image = true; break; }
                    if (frag_samples && !have_image) {
                        LAGFX_LOG("op_0x74 P6a: ref=0x%x frag SAMPLES a texture but reflection "
                                  "has no SAMPLED_IMAGE — incomplete translation, SUBSTITUTE (no crash)",
                                  reference);
                        drawable = false;
                    }
                }

                if (has_resources && !drawable) {
                    /* Resource-using but not yet drawable (textures/samplers, or
                     * no pool) → leave phase6_translated false so the substitute
                     * block installs the triangle (no crash). */
                    LAGFX_LOG("op_0x74 P6a: pipeline ref=0x%x resource-using but not "
                              "buffer-only / no pool — SUBSTITUTE (Stage 90)", reference);
                    vkDestroyShaderModule(vk_device, v_mod, NULL);
                    vkDestroyShaderModule(vk_device, f_mod, NULL);
                    vkDestroyDescriptorSetLayout(vk_device, dsl, NULL);
                    vkDestroyPipelineLayout(vk_device, pl, NULL);
                } else {
                    /* Commit translated. Free any previously-installed translated
                     * modules/layouts first (substitute modules live on the
                     * device — never freed). */
                    if (task->pending_pipeline.translated) {
                        if (task->pending_pipeline.vertex_shader)
                            vkDestroyShaderModule(vk_device,
                                                  (VkShaderModule)task->pending_pipeline.vertex_shader, NULL);
                        if (task->pending_pipeline.fragment_shader)
                            vkDestroyShaderModule(vk_device,
                                                  (VkShaderModule)task->pending_pipeline.fragment_shader, NULL);
                        if (task->pending_pipeline.pipeline_layout)
                            vkDestroyPipelineLayout(vk_device,
                                                    (VkPipelineLayout)task->pending_pipeline.pipeline_layout, NULL);
                        if (task->pending_pipeline.descriptor_set_layout)
                            vkDestroyDescriptorSetLayout(vk_device,
                                                         (VkDescriptorSetLayout)task->pending_pipeline.descriptor_set_layout, NULL);
                    }
                    /* B1: shaders changing → the cached VkPipeline is stale. */
                    lagfx_pending_pipeline_drop_cache(task, vk_device);
                    task->pending_pipeline.valid           = true;
                    task->pending_pipeline.translated      = true;
                    task->pending_pipeline.vertex_shader   = (uintptr_t)v_mod;
                    task->pending_pipeline.fragment_shader = (uintptr_t)f_mod;
                    task->pending_pipeline.reference       = reference;
                    /* Vertex-input reflection: the translated vertex shader's
                     * Location-decorated stage-in attributes. The host builds a
                     * non-empty vertex-input state + binds the guest vertex
                     * buffer from these; without them positions read unbound → 0
                     * → degenerate draws → black. Independent of descriptors, so
                     * reflect for both drawable and resource-free pipelines. */
                    task->pending_pipeline.n_vtx_inputs = 0;
                    if (spv_keep[0]) {
                        lagfx_spv_vertex_input_t vin[8];
                        size_t nvi = lagfx_spv_reflect_vertex_inputs(
                            spv_keep[0], spv_keep_sz[0], vin, 8);
                        if (nvi > 8) nvi = 8;
                        for (size_t v = 0; v < nvi; v++) {
                            task->pending_pipeline.vtx_in_loc[v]  = (uint8_t)vin[v].location;
                            task->pending_pipeline.vtx_in_comp[v] = (uint8_t)vin[v].components;
                        }
                        task->pending_pipeline.n_vtx_inputs = (uint8_t)nvi;
                        if (nvi)
                            LAGFX_LOG("op_0x74 P6a: ref=0x%x vertex shader has %zu stage-in attribute(s)",
                                      reference, nvi);
                    }
                    if (drawable) {
                        task->pending_pipeline.descriptor_set_layout = (uintptr_t)dsl;
                        task->pending_pipeline.pipeline_layout       = (uintptr_t)pl;
                        task->pending_pipeline.n_spv_bindings        = (uint8_t)nrb;
                        for (size_t u = 0; u < nrb; u++) {
                            task->pending_pipeline.spv_binding_no[u]   = (uint8_t)rb[u].binding;
                            task->pending_pipeline.spv_binding_kind[u] = (uint8_t)rb[u].kind;
                        }
                        phase6_translated = true;
                        LAGFX_LOG("op_0x74 P6a: ref=0x%x using TRANSLATED shaders "
                                  "(resource-using, %zu buffer binding(s))", reference, nrb);
                    } else {
                        /* Resource-free → draws with the device empty_layout. */
                        if (dsl != VK_NULL_HANDLE)
                            vkDestroyDescriptorSetLayout(vk_device, dsl, NULL);
                        if (pl != VK_NULL_HANDLE)
                            vkDestroyPipelineLayout(vk_device, pl, NULL);
                        task->pending_pipeline.descriptor_set_layout = 0;
                        task->pending_pipeline.pipeline_layout       = 0;
                        task->pending_pipeline.n_spv_bindings        = 0;
                        phase6_translated = true;
                        LAGFX_LOG("op_0x74 P6a: ref=0x%x using TRANSLATED shaders "
                                  "(resource-free)", reference);
                    }
                }
            } else {
                if (v_mod != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, v_mod, NULL);
                if (f_mod != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, f_mod, NULL);
            }
            /* Reflection buffers no longer needed (modules + layout built). */
            free(spv_keep[0]);
            free(spv_keep[1]);
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
        /* B1: switching to substitute shaders → cached VkPipeline is stale. */
        lagfx_pending_pipeline_drop_cache(task, dev_with_vk->vk->device);
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
    { 0x0000, "DrawPrimitives64",                op_draw_primitives_64 },
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
