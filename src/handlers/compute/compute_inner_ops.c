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

#include "common/le.h"
#include "common/log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*lagfx_compute_inner_op_fn)(lagfx_protocol_t *p,
                                           uint32_t          encoder_type,
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
                                  const uint8_t    *body,
                                  size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 55 — PGCmdDrawPrimitives16 (8 B), scalar family */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x01 DrawPrimitives16 payload too small (%zu < 8)",
                   body_len);
        return 1;
    }
    uint32_t vertex_start = lagfx_le32(body + 0);
    uint32_t vertex_count = lagfx_le32(body + 4);
    LAGFX_LOG("compute_inner: 0x01 DrawPrimitives16 vertexStart=%u vertexCount=%u",
              vertex_start, vertex_count);
    /* TODO: Stage 70 — translate to vkCmdDraw once AIR translation is in place. */
    return 0;
}

static int op_draw_instanced_primitives_16(lagfx_protocol_t *p,
                                            uint32_t          encoder_type,
                                            const uint8_t    *body,
                                            size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 57 — PGCmdDrawInstancedPrimitives16 (8 B), scalar family */
    if (body_len < 8u) {
        LAGFX_WARN("compute_inner: 0x03 DrawInstancedPrimitives16 payload too small (%zu < 8)",
                   body_len);
        return 1;
    }
    /* Caveat: Metal selector takes 4 args but payload only fits 2 u32.
     * Wire format may pack differently than documented C++ struct field names. */
    uint32_t field0 = lagfx_le32(body + 0);
    uint32_t field1 = lagfx_le32(body + 4);
    LAGFX_LOG("compute_inner: 0x03 DrawInstancedPrimitives16 field0=%u field1=%u",
              field0, field1);
    /* TODO: Stage 70 — translate to vkCmdDraw with instanceCount once wire format ambiguity resolved. */
    return 0;
}

static int op_draw_indexed_primitives_64(lagfx_protocol_t *p,
                                          uint32_t          encoder_type,
                                          const uint8_t    *body,
                                          size_t            body_len) {
    (void)p; (void)encoder_type;
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
    LAGFX_LOG("compute_inner: 0x06 DrawIndexedPrimitives64 count=%u type=%u bufRef=0x%x offset=0x%llx",
              index_count, index_type, index_buffer_ref, (unsigned long long)index_buffer_offset);
    /* TODO: Stage 70 — translate to vkCmdDrawIndexed after binding index buffer. */
    return 0;
}

static int op_draw_indexed_primitives_16(lagfx_protocol_t *p,
                                          uint32_t          encoder_type,
                                          const uint8_t    *body,
                                          size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 61 — PGCmdDrawIndexedPrimitives16 (12 B), ref=1 */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x07 DrawIndexedPrimitives16 payload too small (%zu < 12)",
                   body_len);
        return 1;
    }
    uint32_t index_count = lagfx_le32(body + 0);
    uint32_t index_buffer_ref = lagfx_le32(body + 4);
    uint32_t index_buffer_offset = lagfx_le32(body + 8);
    LAGFX_LOG("compute_inner: 0x07 DrawIndexedPrimitives16 count=%u bufRef=0x%x offset=0x%x",
              index_count, index_buffer_ref, index_buffer_offset);
    /* TODO: Stage 70 — translate to vkCmdDrawIndexed after binding index buffer. */
    return 0;
}

/* === Group B — Render-pass + barrier (0x17, 0x1a) ============== */

static int op_render_barrier_scope(lagfx_protocol_t *p,
                                    uint32_t          encoder_type,
                                    const uint8_t    *body,
                                    size_t            body_len) {
    (void)p; (void)encoder_type;
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

static int op_render_describe_render_pass(lagfx_protocol_t *p,
                                           uint32_t          encoder_type,
                                           const uint8_t    *body,
                                           size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 210 — PGCmdDescribeRenderPass (584 B), POD large */
    if (!body || body_len < 16u) {
        LAGFX_WARN("compute_inner: 0x1a RenderDescribeRenderPass payload too small (%zu)",
                   body_len);
        return 1;
    }
    /* Log first 16 bytes as hex for debug — full struct ordering is PARTIAL in RE notes. */
    char hex[33];
    for (int i = 0; i < 16 && i < (int)body_len; ++i) {
        snprintf(hex + (i * 2), sizeof(hex) - (i * 2), "%02x", body[i]);
    }
    LAGFX_LOG("compute_inner: 0x1a RenderDescribeRenderPass size=%zu first_bytes=[%s]",
              body_len, hex);
    /* TODO: Stage 70 — wire VkImageView creation per attachment_ref and begin a real
     * VkRenderPass via lagfx_render_encoder_try_begin once the render encoder is reintroduced. */
    return 0;
}

/* === Group C — Buffer/sampler/texture binding (0x6e, 0x6f, 0x70, 0x72, 0x7d, 0x7e) */

static int op_set_fragment_buffers(lagfx_protocol_t *p,
                                    uint32_t          encoder_type,
                                    const uint8_t    *body,
                                    size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 107 — PGCmdSetBuffers (8 B head) + N×PGCmdSetBufferEntry (12 B), array */
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
    /* Log head + first entry summary */
    for (uint32_t i = 0; i < count && i < 4u; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        uint64_t offset = lagfx_le64(e + 4);
        LAGFX_LOG("compute_inner: 0x6e SetFragmentBuffers [%u] ref=0x%x offset=%llu",
                  first_index + i, ref, (unsigned long long)offset);
    }
    /* TODO: Stage 70 — translate to vkCmdBindDescriptorBuffersEXT once descriptor buffer support added. */
    return 0;
}

static int op_set_fragment_buffer_offset(lagfx_protocol_t *p,
                                          uint32_t          encoder_type,
                                          const uint8_t    *body,
                                          size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 108 — PGCmdSetBufferOffset (12 B), scalar family */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x6f SetFragmentBufferOffset payload too small (%zu < 12)", body_len);
        return 1;
    }
    uint32_t index = lagfx_le32(body + 8);
    uint64_t offset = lagfx_le64(body + 0);
    LAGFX_LOG("compute_inner: 0x6f SetFragmentBufferOffset index=%u offset=0x%llx",
              index, (unsigned long long)offset);
    /* TODO: Stage 70 — (offset rebind — no direct Vulkan equiv; re-bind descriptor). */
    return 0;
}

static int op_set_fragment_sampler_states(lagfx_protocol_t *p,
                                           uint32_t          encoder_type,
                                           const uint8_t    *body,
                                           size_t            body_len) {
    (void)p; (void)encoder_type;
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
                                     const uint8_t    *body,
                                     size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 111 — PGCmdSetTextures (8 B head) + N×u32 ref, array */
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
    /* Log head + first ref */
    for (uint32_t i = 0; i < count && i < 4u; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        LAGFX_LOG("compute_inner: 0x72 SetFragmentTextures [%u] ref=0x%x",
                  first_index + i, ref);
    }
    /* TODO: Stage 70 — translate to vkCmdBindDescriptorSets (sampled image descriptors). */
    return 0;
}

static int op_set_vertex_buffers(lagfx_protocol_t *p,
                                  uint32_t          encoder_type,
                                  const uint8_t    *body,
                                  size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 132 — PGCmdSetBuffers (8 B head) + N×PGCmdSetBufferEntry (12 B), array */
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
    /* Log head + first entry summary */
    for (uint32_t i = 0; i < count && i < 4u; ++i) {
        const uint8_t *e = body + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        uint64_t offset = lagfx_le64(e + 4);
        LAGFX_LOG("compute_inner: 0x7d SetVertexBuffers [%u] ref=0x%x offset=%llu",
                  first_index + i, ref, (unsigned long long)offset);
    }
    /* TODO: Stage 70 — translate to vkCmdBindVertexBuffers2. */
    return 0;
}

static int op_set_vertex_buffer_offset(lagfx_protocol_t *p,
                                        uint32_t          encoder_type,
                                        const uint8_t    *body,
                                        size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 133 — PGCmdSetBufferOffset (12 B), scalar family */
    if (body_len < 12u) {
        LAGFX_WARN("compute_inner: 0x7e SetVertexBufferOffset payload too small (%zu < 12)", body_len);
        return 1;
    }
    uint32_t index = lagfx_le32(body + 8);
    uint64_t offset = lagfx_le64(body + 0);
    LAGFX_LOG("compute_inner: 0x7e SetVertexBufferOffset index=%u offset=0x%llx",
              index, (unsigned long long)offset);
    /* TODO: Stage 70 — vkCmdBindVertexBuffers2 rebind with new offset. Requires bound pipeline from 0x74 first, which requires SPIR-V translation of the MTLRenderPipelineState (Stage 70 long pole). */
    return 0;
}

/* === Group D — Pipeline + scissor/viewport (0x74, 0x75, 0x82) == */

static int op_set_render_pipeline_state(lagfx_protocol_t *p,
                                         uint32_t          encoder_type,
                                         const uint8_t    *body,
                                         size_t            body_len) {
    (void)p; (void)encoder_type;
    /* RE: render-decoder-handlers.md line 118 — PGCmdSetRenderPipelineState (4 B), ref=1 */
    if (body_len < 4u) {
        LAGFX_WARN("compute_inner: 0x74 SetRenderPipelineState payload too small (%zu < 4)", body_len);
        return 1;
    }
    uint32_t reference = lagfx_le32(body + 0);
    LAGFX_LOG("compute_inner: 0x74 SetRenderPipelineState ref=0x%x", reference);
    /* TODO: Stage 70 — resolve pipeline ref to VkPipeline via resource registry and bind via vkCmdBindShadersEXT once shader objects are in place. */
    return 0;
}

static int op_set_scissor_rect(lagfx_protocol_t *p,
                                uint32_t          encoder_type,
                                const uint8_t    *body,
                                size_t            body_len) {
    (void)p; (void)encoder_type;
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
                            const uint8_t    *body,
                            size_t            body_len) {
    (void)p; (void)encoder_type;
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
    return desc->handler(p, encoder_type, body, body_len);
}

const char *lagfx_compute_inner_op_name(uint32_t opcode) {
    const lagfx_compute_inner_op_desc_t *desc =
        find_compute_inner_op_desc(opcode);
    return desc ? desc->name : "unknown";
}
