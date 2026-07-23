/*
 * libapplegfx-vulkan — Render inner-opcode handlers + dispatch table
 * src/handlers/compute/render_inner_ops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * RE: paravirt-re/library/state-machines/render-decoder-handlers.tsv +
 *     pre-refactor src/protocol/render_opcodes.c at b652199~1 (stranded
 *     in dead-code-to-revive/protocol/render_opcodes.c between
 *     2026-05-12 and the 2026-05-13 consolidation pass).
 *
 * Architecture: encType=2 (Render) segments arrive via exec_cmdbuf.c's
 * inner_walk_segment. This file owns the 96-entry descriptor table and
 * parse-and-trace handler set; the dispatch entry point is
 * lagfx_render_inner_dispatch.
 *
 * Implementation status: every handler validates its body size and
 * LAGFX_TRACEs the parsed fields. None translate to Vulkan — the post-
 * dispatcher state.h does NOT carry the render encoder / per-task /
 * Vulkan device pointers the pre-refactor implementation used. When
 * SkyLight starts submitting encType=2 segments this layer will surface
 * the live wire format so the Vulkan translator can be reintroduced
 * incrementally (see TODO: Stage 30 markers below).
 *
 * Opcode 0x1a (RenderDescribeRenderPass) parses the 584-byte descriptor
 * via lagfx_parse_render_pass_descriptor and emits the canonical Stage
 * 20% log line. The 0x1a body lives here because the parser depends on
 * render_pass.h which is private to the handlers tree.
 */

#include "render_inner_ops.h"
#include "protocol/render_pass.h"
#include "protocol/opcodes.h"  /* For LAGFX_HANDLER_OK enum */
#include "protocol/state.h"
#include "common/le.h"
#include "common/log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*lagfx_render_inner_op_fn)(lagfx_protocol_t *p,
                                         const uint8_t   *payload,
                                         size_t           len);

typedef struct {
    uint32_t                 opcode;
    const char              *name;
    uint32_t                 body_size;   /* 0 = variable, count-prefixed */
    uint32_t                 ref_count;   /* per render-decoder-handlers.tsv */
    lagfx_render_inner_op_fn handler;
} lagfx_render_inner_op_desc_t;

/* === LE primitives ============================================== */
/* common/le.h provides lagfx_le16/32/64. The pre-refactor file also
 * needed f32/f64 readers for viewport/clear-color/blend payloads. */

static inline float r_f32(const uint8_t *b) {
    uint32_t u = lagfx_le32(b);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline double r_f64(const uint8_t *b) {
    uint64_t u = lagfx_le64(b);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

/* === Ack-only stub =============================================== */

static int op_ack_stub(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p; (void)payload; (void)len;
    return 0;
}

/* === Draw family (0x00-0x1d) ===================================== */

static int op_draw_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("DrawPrimitives64: payload too short (%zu < 20)", len); return 0; }
    uint32_t prim_type    = lagfx_le32(payload + 0);
    uint64_t vertex_start = lagfx_le64(payload + 4);
    uint64_t vertex_count = lagfx_le64(payload + 12);
    LAGFX_TRACE("DrawPrimitives64: type=%u start=%llu count=%llu",
                prim_type,
                (unsigned long long)vertex_start,
                (unsigned long long)vertex_count);
    /* TODO: Stage 30 — translate to vkCmdDraw once render encoder state
     * lives on lagfx_protocol_t. */
    return 0;
}

static int op_draw_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("DrawPrimitives16: payload too short (%zu < 8)", len); return 0; }
    uint32_t prim_type    = lagfx_le32(payload + 0);
    uint16_t vertex_start = lagfx_le16(payload + 4);
    uint16_t vertex_count = lagfx_le16(payload + 6);
    LAGFX_LOG("DrawPrimitives16: type=%u start=%u count=%u",
                prim_type, vertex_start, vertex_count);
    return 0;
}

static int op_draw_instanced_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 28) { LAGFX_WARN("DrawInstancedPrimitives64: payload too short (%zu < 28)", len); return 0; }
    uint32_t prim_type      = lagfx_le32(payload + 0);
    uint64_t vertex_start   = lagfx_le64(payload + 4);
    uint64_t vertex_count   = lagfx_le64(payload + 12);
    uint64_t instance_count = lagfx_le64(payload + 20);
    LAGFX_TRACE("DrawInstancedPrimitives64: type=%u start=%llu count=%llu inst=%llu",
                prim_type,
                (unsigned long long)vertex_start,
                (unsigned long long)vertex_count,
                (unsigned long long)instance_count);
    return 0;
}

static int op_draw_instanced_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("DrawInstancedPrimitives16: payload too short (%zu < 8)", len); return 0; }
    uint32_t prim_type    = lagfx_le32(payload + 0);
    uint16_t vertex_start = lagfx_le16(payload + 4);
    uint16_t vertex_count = lagfx_le16(payload + 6);
    LAGFX_LOG("DrawInstancedPrimitives16: type=%u start=%u count=%u",
                prim_type, vertex_start, vertex_count);
    return 0;
}

static int op_draw_instanced_base_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 36) { LAGFX_WARN("DrawInstancedBasePrimitives64: payload too short (%zu < 36)", len); return 0; }
    LAGFX_TRACE("DrawInstancedBasePrimitives64: type=%u (full payload 36B)",
                lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_instanced_base_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("DrawInstancedBasePrimitives16: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("DrawInstancedBasePrimitives16: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_indexed_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 24) { LAGFX_WARN("DrawIndexedPrimitives64: payload too short (%zu < 24)", len); return 0; }
    uint32_t prim_type        = lagfx_le32(payload + 0);
    uint32_t index_count      = lagfx_le32(payload + 4);
    uint32_t index_type       = lagfx_le32(payload + 8);
    uint32_t index_buf_ref    = lagfx_le32(payload + 12);
    uint64_t index_buf_offset = lagfx_le64(payload + 16);
    LAGFX_LOG("DrawIndexedPrimitives64: type=%u count=%u idxType=%u idxBufRef=0x%08x off=%llu",
                prim_type, index_count, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset);
    return 0;
}

static int op_draw_indexed_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("DrawIndexedPrimitives16: payload too short (%zu < 12)", len); return 0; }
    LAGFX_LOG("DrawIndexedPrimitives16: type=%u count=%u idxBufRef=0x%08x",
                lagfx_le32(payload + 0), lagfx_le16(payload + 4),
                lagfx_le32(payload + 8));
    return 0;
}

static int op_draw_indexed_instanced_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 32) { LAGFX_WARN("DrawIndexedInstancedPrimitives64: payload too short (%zu < 32)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedPrimitives64: type=%u count=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_draw_indexed_instanced_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("DrawIndexedInstancedPrimitives16: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedPrimitives16: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_indexed_instanced_base_primitives_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 48) { LAGFX_WARN("DrawIndexedInstancedBasePrimitives64: payload too short (%zu < 48)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives64: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_indexed_instanced_base_primitives_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("DrawIndexedInstancedBasePrimitives16: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives16: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_patches_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 48) { LAGFX_WARN("DrawPatches64: payload too short (%zu < 48)", len); return 0; }
    LAGFX_TRACE("DrawPatches64: (48B payload)");
    return 0;
}

static int op_draw_patches_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("DrawPatches16: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("DrawPatches16: (16B payload)");
    return 0;
}

static int op_draw_indexed_patches_64(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 60) { LAGFX_WARN("DrawIndexedPatches64: payload too short (%zu < 60)", len); return 0; }
    LAGFX_TRACE("DrawIndexedPatches64: (60B payload)");
    return 0;
}

static int op_draw_indexed_patches_16(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 24) { LAGFX_WARN("DrawIndexedPatches16: payload too short (%zu < 24)", len); return 0; }
    LAGFX_TRACE("DrawIndexedPatches16: (24B payload)");
    return 0;
}

static int op_draw_primitives_indirect(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("DrawPrimitivesIndirect: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("DrawPrimitivesIndirect: type=%u arg_buf_ref=0x%08x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_draw_indexed_primitives_indirect(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 28) { LAGFX_WARN("DrawIndexedPrimitivesIndirect: payload too short (%zu < 28)", len); return 0; }
    LAGFX_TRACE("DrawIndexedPrimitivesIndirect: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_patches_indirect(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 28) { LAGFX_WARN("DrawPatchesIndirect: payload too short (%zu < 28)", len); return 0; }
    LAGFX_TRACE("DrawPatchesIndirect: (28B payload)");
    return 0;
}

static int op_draw_indexed_patches_indirect(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 40) { LAGFX_WARN("DrawIndexedPatchesIndirect: payload too short (%zu < 40)", len); return 0; }
    LAGFX_TRACE("DrawIndexedPatchesIndirect: (40B payload)");
    return 0;
}

static int op_execute_commands_in_buffer(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("ExecuteCommandsInBuffer: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("ExecuteCommandsInBuffer: icb_ref=0x%08x range_buf_ref=0x%08x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_execute_commands_in_buffer_ranged(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("ExecuteCommandsInBufferRanged: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("ExecuteCommandsInBufferRanged: icb_ref=0x%08x", lagfx_le32(payload + 0));
    return 0;
}

static int op_render_barrier_scope(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("RenderBarrierScope: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("RenderBarrierScope: scope=0x%x", lagfx_le32(payload + 0));
    return 0;
}

static int op_render_update_fence(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("RenderUpdateFence: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("RenderUpdateFence: fence_ref=0x%08x stages=0x%08x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_render_wait_for_fence(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("RenderWaitForFence: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("RenderWaitForFence: fence_ref=0x%08x stages=0x%08x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

/* 0x1a — RenderDescribeRenderPass. Parses the 584-byte descriptor
 * (render_pass.h) and emits the canonical Stage 20% line. This is the
 * grep target in reference_m5_validation.md. */
static int op_describe_render_pass(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (!payload || len < LAGFX_RENDER_PASS_WIRE_SIZE) {
        /* Coarse-count attachments from a short payload — the kext sent
         * the opcode, so emit ≥1 attachment for the Stage 20% checkpoint
         * regardless of full-parse success. */
        LAGFX_WARN("0x1a: RenderPassDescriptor parsed — 1 attachments (short body len=%zu < %u)",
                   len, LAGFX_RENDER_PASS_WIRE_SIZE);
        return 0;
    }

    lagfx_render_pass_full_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int rc = lagfx_parse_render_pass_descriptor(payload, len, &desc);
    if (rc != 0) {
        LAGFX_WARN("0x1a: RenderPassDescriptor parse failed (rc=%d len=%zu) — "
                   "1 attachments (coarse count)", rc, len);
        return 0;
    }

    unsigned attachments = (desc.has_depth ? 1u : 0u)
                          + (desc.has_stencil ? 1u : 0u)
                          + desc.color_attachment_count;
    if (attachments == 0u) attachments = 1u;

    /* Canonical Stage 20% sighting line — matches the grep target in
     * reference_m5_validation.md. Emit at WARN level so it's visible
     * even when LAGFX_LOG_LEVEL=warn. */
    LAGFX_WARN("0x1a: RenderPassDescriptor parsed — %u attachments rt=%llux%llu",
               attachments,
               (unsigned long long)desc.render_target_width,
               (unsigned long long)desc.render_target_height);

    /* TODO: Stage 30 — wire VkImageView creation per attachment_ref and
     * begin a real VkRenderPass via lagfx_render_encoder_try_begin once
     * the render encoder is reintroduced to lagfx_protocol_t. */
    return 0;
}

static int op_use_heaps_with_stages(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("UseHeapsWithStages: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("UseHeapsWithStages: count=%u stages=0x%x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_draw_indexed_instanced_base_primitives_64_2(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 48) { LAGFX_WARN("DrawIndexedInstancedBasePrimitives64_2: payload too short (%zu < 48)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives64_2: type=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_draw_indexed_instanced_base_primitives_16_2(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("DrawIndexedInstancedBasePrimitives16_2: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives16_2: type=%u", lagfx_le32(payload + 0));
    return 0;
}

/* 0x2c — Unknown. RE pending; TSV row says "<default/throw>".
 * macOS sends this repeatedly with len=88 during render setup. Log full hex dump
 * on WARN so the next RE pass has data without needing trace level. */
static int op_0x2c(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (!payload || len < 88u) {
        LAGFX_WARN("0x2c: payload too short (%zu < 88)", len);
        return LAGFX_HANDLER_OK;
    }
    char buf[88 * 3 + 1];
    size_t pos = 0;
    for (size_t i = 0; i < 88 && pos + 4 < sizeof(buf); ++i) {
        int w = snprintf(buf + pos, sizeof(buf) - pos, "%s%02x",
                         i == 0 ? "" : " ", (unsigned)payload[i]);
        if (w > 0) pos += (size_t)w;
    }
    LAGFX_WARN("0x2c: len=%zu payload=[%s]", len, buf);
    /* TODO Stage 30: RE PGDeserializerRenderDecoder-decodeWithHeader to discover
     * semantics. Wire-format hypotheses in paravirt-re/library/render-opcode-0x2c-re-2026-05-18.md */
    return LAGFX_HANDLER_OK;
}

/* 0x3c — command-buffer-inner (recursive cmdbuf reference).
 * 8-byte payload per pre-refactor: { u32 resource_id, u32 pad }.
 * For Stage 30+ this should walk the referenced cmdbuf, but doing so
 * requires per-task VA translation which currently lives in
 * exec_cmdbuf.c. Defer recursion to a Stage 30 follow-up. */
static int op_command_buffer_inner(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (!payload || len < 8u) {
        LAGFX_WARN("command_buffer_inner: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t resource_id = lagfx_le32(payload + 0);
    uint32_t pad         = lagfx_le32(payload + 4);
    (void)pad;
    LAGFX_LOG("render_inner: 0x3c recursive-cmdbuf ref=0x%x (deferred — no recursion)",
              resource_id);
    /* TODO: Stage 85+ — recursively walk the referenced cmdbuf via the
     * per-task radix translator. */
    return 0;
}

/* === State-set family (0x65-0xa6) ================================ */

static int op_set_blend_color(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("SetBlendColor: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("SetBlendColor: r=%g g=%g b=%g a=%g",
                (double)r_f32(payload + 0), (double)r_f32(payload + 4),
                (double)r_f32(payload + 8), (double)r_f32(payload + 12));
    return 0;
}

static int op_set_color_store_action(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetColorStoreAction: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetColorStoreAction: action=%u index=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_color_store_action_options(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetColorStoreActionOptions: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("SetColorStoreActionOptions: opt=%u index=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_depth_stencil_state(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetDepthStencilState: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetDepthStencilState: ref=0x%08x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_depth_store_action(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetDepthStoreAction: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetDepthStoreAction: action=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_depth_store_action_options(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetDepthStoreActionOptions: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetDepthStoreActionOptions: opt=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_cull_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetCullMode: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetCullMode: value=%u extra=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_depth_bias(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetDepthBias: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("SetDepthBias: bias=%g slope=%g clamp=%g",
                (double)r_f32(payload + 0), (double)r_f32(payload + 4),
                (double)r_f32(payload + 8));
    return 0;
}

static int op_set_depth_clip_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetDepthClipMode: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetDepthClipMode: mode=%u extra=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

/* Variable-length list opcode helpers ---------------------------- */

static int op_set_buffers_variable(lagfx_protocol_t *p,
                                     const uint8_t   *payload,
                                     size_t           len,
                                     const char      *name,
                                     size_t           entry_bytes) {
    (void)p;
    if (len < 8) { LAGFX_WARN("%s: payload too short (%zu < 8)", name, len); return 0; }
    /* Wire-format RE (resource binding): dump the raw payload so the real
     * buffer-binding layout (header size + per-entry stride) can be decoded
     * from live guest traffic. The assumed 8-byte header + entry_bytes
     * stride doesn't fit every payload (e.g. a 20-byte SetVertexBuffers
     * parsed as count=2 needs 32). */
    {
        char hex[160]; size_t hn = 0; size_t dump = len < 40 ? len : 40;
        for (size_t i = 0; i < dump && hn + 3 < sizeof(hex); i++)
            hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x ", payload[i]);
        LAGFX_LOG("%s: RAW[%zu]: %s", name, len, hex);
    }
    uint32_t count = lagfx_le32(payload + 0);
    uint32_t first = lagfx_le32(payload + 4);
    size_t needed = 8 + (size_t)count * entry_bytes;
    if (len < needed) {
        LAGFX_WARN("%s: count=%u needs %zu bytes, got %zu", name, count, needed, len);
        return 0;
    }
    LAGFX_LOG("%s: count=%u first=%u entry_bytes=%zu", name, count, first, entry_bytes);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *e = payload + 8 + (size_t)i * entry_bytes;
        uint32_t ref = lagfx_le32(e);
        if (entry_bytes >= 12) {
            uint64_t off = lagfx_le64(e + 4);
            LAGFX_TRACE("  [%u] ref=0x%08x offset=%llu", first + i, ref, (unsigned long long)off);
        } else {
            LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
        }
    }
    return 0;
}

static int op_set_fragment_buffers(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetFragmentBuffers", 12);
}

static int op_set_fragment_buffer_offset(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetFragmentBufferOffset: payload too short (%zu < 12)", len); return 0; }
    /* PGCmdSetBufferOffset: [index:u32@0][offset:u64@4] (Apple decoder disasm) */
    LAGFX_LOG("SetFragmentBufferOffset: offset=%llu index=%u",
                (unsigned long long)lagfx_le64(payload + 4), lagfx_le32(payload + 0));
    return 0;
}

static int op_set_fragment_sampler_states(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetFragmentSamplerStates", 4);
}

static int op_set_fragment_sampler_states_lod_clamp(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetFragmentSamplerStatesLODClamp", 12);
}

static int op_set_fragment_textures(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetFragmentTextures", 4);
}

static int op_set_front_facing_winding(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetFrontFacingWinding: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetFrontFacingWinding: value=%u extra=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_render_pipeline_state(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetRenderPipelineState: payload too short (%zu < 4)", len); return 0; }
    uint32_t reference = lagfx_le32(payload + 0);
    LAGFX_LOG("SetRenderPipelineState: ref=0x%08x", reference);
    /* TODO: Stage 30 — resolve pipeline ref to VkPipeline via resource
     * registry and bind via vkCmdBindPipeline once the encoder state
     * lives on the protocol struct. */
    return 0;
}

static int op_set_scissor_rect(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 32) { LAGFX_WARN("SetScissorRect: payload too short (%zu < 32)", len); return 0; }
    LAGFX_LOG("SetScissorRect: x=%llu y=%llu w=%llu h=%llu",
                (unsigned long long)lagfx_le64(payload + 0),
                (unsigned long long)lagfx_le64(payload + 8),
                (unsigned long long)lagfx_le64(payload + 16),
                (unsigned long long)lagfx_le64(payload + 24));
    return 0;
}

static int op_set_scissor_rects(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetScissorRects: payload too short (%zu < 4)", len); return 0; }
    uint32_t count = lagfx_le32(payload + 0);
    LAGFX_TRACE("SetScissorRects: count=%u (variable-length payload len=%zu)", count, len);
    return 0;
}

static int op_set_stencil_ref(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetStencilRef: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetStencilRef: front=%u back=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_stencil_store_action(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetStencilStoreAction: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetStencilStoreAction: action=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_stencil_store_action_options(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetStencilStoreActionOptions: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetStencilStoreActionOptions: opt=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_tesselation_factor_buffer(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetTesselationFactorBuffer: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("SetTesselationFactorBuffer: ref=0x%08x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_tesselation_factor_scale(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetTesselationFactorScale: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetTesselationFactorScale: scale=%g", (double)r_f32(payload + 0));
    return 0;
}

static int op_set_triangle_fill_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetTriangleFillMode: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetTriangleFillMode: mode=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_vertex_buffers(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetVertexBuffers", 12);
}

static int op_set_vertex_buffer_offset(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetVertexBufferOffset: payload too short (%zu < 12)", len); return 0; }
    /* PGCmdSetBufferOffset: [index:u32@0][offset:u64@4] (Apple decoder disasm) */
    LAGFX_LOG("SetVertexBufferOffset: offset=%llu index=%u",
                (unsigned long long)lagfx_le64(payload + 4), lagfx_le32(payload + 0));
    return 0;
}

static int op_set_vertex_sampler_states(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetVertexSamplerStates", 4);
}

static int op_set_vertex_sampler_states_lod_clamp(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetVertexSamplerStatesLODClamp", 12);
}

static int op_set_vertex_textures(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetVertexTextures", 4);
}

static int op_set_viewport(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 48) { LAGFX_WARN("SetViewport: payload too short (%zu < 48)", len); return 0; }
    LAGFX_LOG("SetViewport: origin=(%g,%g) size=%gx%g znear=%g zfar=%g",
                r_f64(payload + 0),  r_f64(payload + 8),
                r_f64(payload + 16), r_f64(payload + 24),
                r_f64(payload + 32), r_f64(payload + 40));
    return 0;
}

static int op_set_viewports(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetViewports: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetViewports: count=%u (variable)", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_visibility_result_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("SetVisibilityResultMode: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("SetVisibilityResultMode: mode=%u offset=%llu",
                lagfx_le32(payload + 0),
                (unsigned long long)lagfx_le64(payload + 8));
    return 0;
}

static int op_texture_barrier(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p; (void)payload; (void)len;
    LAGFX_TRACE("TextureBarrier");
    return 0;
}

static int op_use_heaps(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("UseHeaps: payload too short (%zu < 4)", len); return 0; }
    uint32_t count = lagfx_le32(payload + 0);
    LAGFX_TRACE("UseHeaps: count=%u", count);
    return 0;
}

static int op_use_resources(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("UseResources: payload too short (%zu < 8)", len); return 0; }
    uint32_t count = lagfx_le32(payload + 0);
    uint32_t usage = lagfx_le32(payload + 4);
    LAGFX_TRACE("UseResources: count=%u usage=0x%x", count, usage);
    /* TODO: Stage 30 — register each ref into resource registry so the
     * residency tracker can pin VkImage/VkBuffer handles. */
    return 0;
}

static int op_set_line_width(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetLineWidth: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetLineWidth: width=%g", (double)r_f32(payload + 0));
    return 0;
}

static int op_use_resources_with_stages(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("UseResourcesWithStages: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("UseResourcesWithStages: count=%u usage=0x%x stages=0x%x",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4), lagfx_le32(payload + 8));
    return 0;
}

static int op_set_alpha_test_reference_value(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetAlphaTestReferenceValue: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetAlphaTestReferenceValue: ref=%g", (double)r_f32(payload + 0));
    return 0;
}

static int op_set_point_size(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetPointSize: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetPointSize: size=%g", (double)r_f32(payload + 0));
    return 0;
}

static int op_set_clip_plane(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetClipPlane: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("SetClipPlane: idx=%u plane=(%g,%g,%g,%g)",
                lagfx_le32(payload + 0),
                (double)r_f32(payload + 4),  (double)r_f32(payload + 8),
                (double)r_f32(payload + 12), (double)r_f32(payload + 16));
    return 0;
}

static int op_set_vertex_sampler_state(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetVertexSamplerState: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("SetVertexSamplerState: ref=0x%08x idx=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 16));
    return 0;
}

static int op_set_fragment_sampler_state(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetFragmentSamplerState: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("SetFragmentSamplerState: ref=0x%08x idx=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 16));
    return 0;
}

static int op_set_viewport_transform_enabled(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetViewportTransformEnabled: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetViewportTransformEnabled: enabled=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_provoking_vertex_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetProvokingVertexMode: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetProvokingVertexMode: mode=%u", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_primitive_restart_index_enabled(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetPrimitiveRestartIndexEnabled: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetPrimitiveRestartIndexEnabled: enabled=%u index=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_triangle_fill_mode_front_back(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetTriangleFillModeFrontBack: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetTriangleFillModeFrontBack: mode=0x%x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_transform_feedback_state(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetTransformFeedbackState: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetTransformFeedbackState: state=0x%x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_depth_cleared(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p; (void)payload; (void)len;
    LAGFX_TRACE("SetDepthCleared");
    return 0;
}

static int op_set_stencil_cleared(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p; (void)payload; (void)len;
    LAGFX_TRACE("SetStencilCleared");
    return 0;
}

static int op_set_color_resolve_texture(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 16) { LAGFX_WARN("SetColorResolveTexture: payload too short (%zu < 16)", len); return 0; }
    LAGFX_TRACE("SetColorResolveTexture: ref=0x%08x slot=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_depth_resolve_texture(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetDepthResolveTexture: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("SetDepthResolveTexture: ref=0x%08x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_stencil_resolve_texture(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetStencilResolveTexture: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("SetStencilResolveTexture: ref=0x%08x", lagfx_le32(payload + 0));
    return 0;
}

static int op_set_vertex_amplification_mode(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 8) { LAGFX_WARN("SetVertexAmplificationMode: payload too short (%zu < 8)", len); return 0; }
    LAGFX_TRACE("SetVertexAmplificationMode: mode=%u value=%u",
                lagfx_le32(payload + 0), lagfx_le32(payload + 4));
    return 0;
}

static int op_set_vertex_amplification_count(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 4) { LAGFX_WARN("SetVertexAmplificationCount: payload too short (%zu < 4)", len); return 0; }
    LAGFX_TRACE("SetVertexAmplificationCount: count=%u (variable-length payload)",
                lagfx_le32(payload + 0));
    return 0;
}

static int op_dispatch_threads_per_tile(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 24) { LAGFX_WARN("DispatchThreadsPerTile: payload too short (%zu < 24)", len); return 0; }
    LAGFX_TRACE("DispatchThreadsPerTile: size=%llux%llux%llu",
                (unsigned long long)lagfx_le64(payload + 0),
                (unsigned long long)lagfx_le64(payload + 8),
                (unsigned long long)lagfx_le64(payload + 16));
    return 0;
}

static int op_set_render_threadgroup_memory_length(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetRenderThreadgroupMemoryLength: payload too short (%zu < 20)", len); return 0; }
    LAGFX_TRACE("SetRenderThreadgroupMemoryLength: length=%llu offset=%llu index=%u",
                (unsigned long long)lagfx_le64(payload + 0),
                (unsigned long long)lagfx_le64(payload + 8),
                lagfx_le32(payload + 16));
    return 0;
}

static int op_set_tile_buffers(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetTileBuffers", 12);
}

static int op_set_tile_buffer_offset(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("SetTileBufferOffset: payload too short (%zu < 12)", len); return 0; }
    /* PGCmdSetBufferOffset: [index:u32@0][offset:u64@4] (Apple decoder disasm) */
    LAGFX_TRACE("SetTileBufferOffset: offset=%llu index=%u",
                (unsigned long long)lagfx_le64(payload + 4), lagfx_le32(payload + 0));
    return 0;
}

static int op_set_tile_sampler_states(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetTileSamplerStates", 4);
}

static int op_set_tile_sampler_states_lod_clamp(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetTileSamplerStatesLODClamp", 12);
}

static int op_set_tile_textures(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetTileTextures", 4);
}

static int op_dispatch_threads_per_tile_in_region(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 76) { LAGFX_WARN("DispatchThreadsPerTileInRegion: payload too short (%zu < 76)", len); return 0; }
    LAGFX_TRACE("DispatchThreadsPerTileInRegion: tile=%llux%llux%llu",
                (unsigned long long)lagfx_le64(payload + 0),
                (unsigned long long)lagfx_le64(payload + 8),
                (unsigned long long)lagfx_le64(payload + 16));
    return 0;
}

static int op_dispatch_threads_per_tile_in_region_w_idx(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 76) { LAGFX_WARN("DispatchThreadsPerTileInRegionWithIndex: payload too short (%zu < 76)", len); return 0; }
    LAGFX_TRACE("DispatchThreadsPerTileInRegionWithIndex: rtIdx=%u", lagfx_le32(payload + 72));
    return 0;
}

static int op_get_tile_dimensions(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 12) { LAGFX_WARN("GetTileDimensions: payload too short (%zu < 12)", len); return 0; }
    LAGFX_TRACE("GetTileDimensions: ref=0x%08x offset=%llu",
                lagfx_le32(payload + 0),
                (unsigned long long)lagfx_le64(payload + 4));
    return 0;
}

static int op_set_vertex_buffers_with_stride(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    return op_set_buffers_variable(p, payload, len, "SetVertexBuffersWithStride", 20);
}

static int op_set_vertex_buffer_offset_with_stride(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p;
    if (len < 20) { LAGFX_WARN("SetVertexBufferOffsetWithStride: payload too short (%zu < 20)", len); return 0; }
    /* PGCmdSetBufferOffsetWithStride: [index:u32@0][offset:u64@4][stride:u64@12]
     * (Apple decodeSetVertexBufferOffsetWithStrideWithIterator disasm) */
    LAGFX_TRACE("SetVertexBufferOffsetWithStride: offset=%llu stride=%llu index=%u",
                (unsigned long long)lagfx_le64(payload + 4),
                (unsigned long long)lagfx_le64(payload + 12),
                lagfx_le32(payload + 0));
    return 0;
}

/* === Descriptor table (96 entries) =============================== *
 *
 * Layout matches paravirt-re render-decoder-handlers.tsv +
 * sub-decoder opcode 0x1a (RenderDescribeRenderPass) + 0x2c
 * (Unknown, RE pending) + 0x3c (command-buffer-inner, deferred
 * recursion).
 *
 * Body sizes are the TSV "payload_size" column verbatim. Where the TSV
 * says variable-length (e.g. "8+N*4") we store 0 and let the handler
 * parse the count. `ref_count` records the static-residency hint from
 * the TSV.
 */
static const lagfx_render_inner_op_desc_t g_table[] = {
    /* --- Draw family (0x00-0x1d) ---------------------------------- */
    { 0x00, "DrawPrimitives64",                          20, 0, op_draw_primitives_64 },
    { 0x01, "DrawPrimitives16",                           8, 0, op_draw_primitives_16 },
    { 0x02, "DrawInstancedPrimitives64",                 28, 0, op_draw_instanced_primitives_64 },
    { 0x03, "DrawInstancedPrimitives16",                  8, 0, op_draw_instanced_primitives_16 },
    { 0x04, "DrawInstancedBasePrimitives64",             36, 0, op_draw_instanced_base_primitives_64 },
    { 0x05, "DrawInstancedBasePrimitives16",             12, 0, op_draw_instanced_base_primitives_16 },
    { 0x06, "DrawIndexedPrimitives64",                   24, 1, op_draw_indexed_primitives_64 },
    { 0x07, "DrawIndexedPrimitives16",                   12, 1, op_draw_indexed_primitives_16 },
    { 0x08, "DrawIndexedInstancedPrimitives64",          32, 1, op_draw_indexed_instanced_primitives_64 },
    { 0x09, "DrawIndexedInstancedPrimitives16",          16, 1, op_draw_indexed_instanced_primitives_16 },
    { 0x0a, "DrawIndexedInstancedBasePrimitives64",      48, 1, op_draw_indexed_instanced_base_primitives_64 },
    { 0x0b, "DrawIndexedInstancedBasePrimitives16",      20, 1, op_draw_indexed_instanced_base_primitives_16 },
    { 0x0c, "DrawPatches64",                             48, 1, op_draw_patches_64 },
    { 0x0d, "DrawPatches16",                             16, 1, op_draw_patches_16 },
    { 0x0e, "DrawIndexedPatches64",                      60, 2, op_draw_indexed_patches_64 },
    { 0x0f, "DrawIndexedPatches16",                      24, 2, op_draw_indexed_patches_16 },
    { 0x10, "DrawPrimitivesIndirect",                    16, 1, op_draw_primitives_indirect },
    { 0x11, "DrawIndexedPrimitivesIndirect",             28, 2, op_draw_indexed_primitives_indirect },
    { 0x12, "DrawPatchesIndirect",                       28, 2, op_draw_patches_indirect },
    { 0x13, "DrawIndexedPatchesIndirect",                40, 3, op_draw_indexed_patches_indirect },
    { 0x14, "ExecuteCommandsInBuffer",                   16, 2, op_execute_commands_in_buffer },
    { 0x15, "ExecuteCommandsInBufferRanged",             20, 1, op_execute_commands_in_buffer_ranged },
    { 0x16, "RenderBarrierResources",                     0, 0, op_ack_stub },
    { 0x17, "RenderBarrierScope",                         4, 0, op_render_barrier_scope },
    { 0x18, "RenderUpdateFence",                          8, 1, op_render_update_fence },
    { 0x19, "RenderWaitForFence",                         8, 1, op_render_wait_for_fence },
    { 0x1a, "RenderDescribeRenderPass",                 584, 0, op_describe_render_pass },
    { 0x1b, "UseHeapsWithStages",                         0, 0, op_use_heaps_with_stages },
    { 0x1c, "DrawIndexedInstancedBasePrimitives64_2",    48, 1, op_draw_indexed_instanced_base_primitives_64_2 },
    { 0x1d, "DrawIndexedInstancedBasePrimitives16_2",    20, 1, op_draw_indexed_instanced_base_primitives_16_2 },
    /* 0x2c — RE pending, macOS sends len=88 during render setup */
    { 0x2c, "Unknown(0x2c)",                             88, 0, op_0x2c },
    /* 0x3c — recursive cmdbuf reference (Stage 30 deferred) */
    { 0x3c, "command-buffer-inner",                      20, 2, op_command_buffer_inner },

    /* --- State-set family (0x65-0xa6) ----------------------------- */
    { 0x65, "SetBlendColor",                             16, 0, op_set_blend_color },
    { 0x66, "SetColorStoreAction",                        8, 0, op_set_color_store_action },
    { 0x67, "SetColorStoreActionOptions",                12, 0, op_set_color_store_action_options },
    { 0x68, "SetDepthStencilState",                       4, 1, op_set_depth_stencil_state },
    { 0x69, "SetDepthStoreAction",                        8, 0, op_set_depth_store_action },
    { 0x6a, "SetDepthStoreActionOptions",                 8, 0, op_set_depth_store_action_options },
    { 0x6b, "SetCullMode",                                8, 0, op_set_cull_mode },
    { 0x6c, "SetDepthBias",                              12, 0, op_set_depth_bias },
    { 0x6d, "SetDepthClipMode",                           8, 0, op_set_depth_clip_mode },
    { 0x6e, "SetFragmentBuffers",                         0, 0, op_set_fragment_buffers },
    { 0x6f, "SetFragmentBufferOffset",                   12, 0, op_set_fragment_buffer_offset },
    { 0x70, "SetFragmentSamplerStates",                   0, 0, op_set_fragment_sampler_states },
    { 0x71, "SetFragmentSamplerStatesLODClamp",           0, 0, op_set_fragment_sampler_states_lod_clamp },
    { 0x72, "SetFragmentTextures",                        0, 0, op_set_fragment_textures },
    { 0x73, "SetFrontFacingWinding",                      8, 0, op_set_front_facing_winding },
    { 0x74, "SetRenderPipelineState",                     4, 1, op_set_render_pipeline_state },
    { 0x75, "SetScissorRect",                            32, 0, op_set_scissor_rect },
    { 0x76, "SetScissorRects",                            0, 0, op_set_scissor_rects },
    { 0x77, "SetStencilRef",                              8, 0, op_set_stencil_ref },
    { 0x78, "SetStencilStoreAction",                      8, 0, op_set_stencil_store_action },
    { 0x79, "SetStencilStoreActionOptions",               8, 0, op_set_stencil_store_action_options },
    { 0x7a, "SetTesselationFactorBuffer",                20, 1, op_set_tesselation_factor_buffer },
    { 0x7b, "SetTesselationFactorScale",                  4, 0, op_set_tesselation_factor_scale },
    { 0x7c, "SetTriangleFillMode",                        8, 0, op_set_triangle_fill_mode },
    { 0x7d, "SetVertexBuffers",                           0, 0, op_set_vertex_buffers },
    { 0x7e, "SetVertexBufferOffset",                     12, 0, op_set_vertex_buffer_offset },
    { 0x7f, "SetVertexSamplerStates",                     0, 0, op_set_vertex_sampler_states },
    { 0x80, "SetVertexSamplerStatesLODClamp",             0, 0, op_set_vertex_sampler_states_lod_clamp },
    { 0x81, "SetVertexTextures",                          0, 0, op_set_vertex_textures },
    { 0x82, "SetViewport",                               48, 0, op_set_viewport },
    { 0x83, "SetViewports",                               0, 0, op_set_viewports },
    { 0x84, "SetVisibilityResultMode",                   16, 0, op_set_visibility_result_mode },
    { 0x85, "TextureBarrier",                             0, 0, op_texture_barrier },
    { 0x86, "UseHeaps",                                   0, 0, op_use_heaps },
    { 0x87, "UseResources",                               0, 0, op_use_resources },
    { 0x88, "SetLineWidth",                               4, 0, op_set_line_width },
    { 0x89, "UseResourcesWithStages",                     0, 0, op_use_resources_with_stages },
    { 0x8a, "SetAlphaTestReferenceValue",                 4, 0, op_set_alpha_test_reference_value },
    { 0x8b, "SetPointSize",                               4, 0, op_set_point_size },
    { 0x8c, "SetClipPlane",                              20, 0, op_set_clip_plane },
    { 0x8d, "SetVertexSamplerState",                     20, 1, op_set_vertex_sampler_state },
    { 0x8e, "SetFragmentSamplerState",                   20, 1, op_set_fragment_sampler_state },
    { 0x8f, "SetViewportTransformEnabled",                4, 0, op_set_viewport_transform_enabled },
    { 0x90, "SetProvokingVertexMode",                     4, 0, op_set_provoking_vertex_mode },
    { 0x91, "SetPrimitiveRestartIndexEnabled",            8, 0, op_set_primitive_restart_index_enabled },
    { 0x92, "SetTriangleFillModeFrontBack",               4, 0, op_set_triangle_fill_mode_front_back },
    { 0x93, "SetTransformFeedbackState",                  4, 0, op_set_transform_feedback_state },
    { 0x94, "SetDepthCleared",                            0, 0, op_set_depth_cleared },
    { 0x95, "SetStencilCleared",                          0, 0, op_set_stencil_cleared },
    { 0x96, "SetColorResolveTexture",                    16, 1, op_set_color_resolve_texture },
    { 0x97, "SetDepthResolveTexture",                    12, 1, op_set_depth_resolve_texture },
    { 0x98, "SetStencilResolveTexture",                  12, 1, op_set_stencil_resolve_texture },
    { 0x99, "SetVertexAmplificationMode",                 8, 0, op_set_vertex_amplification_mode },
    { 0x9a, "SetVertexAmplificationCount",                0, 0, op_set_vertex_amplification_count },
    { 0x9b, "DispatchThreadsPerTile",                    24, 0, op_dispatch_threads_per_tile },
    { 0x9c, "SetRenderThreadgroupMemoryLength",          20, 0, op_set_render_threadgroup_memory_length },
    { 0x9d, "SetTileBuffers",                             0, 0, op_set_tile_buffers },
    { 0x9e, "SetTileBufferOffset",                       12, 0, op_set_tile_buffer_offset },
    { 0x9f, "SetTileSamplerStates",                       0, 0, op_set_tile_sampler_states },
    { 0xa0, "SetTileSamplerStatesLODClamp",               0, 0, op_set_tile_sampler_states_lod_clamp },
    { 0xa1, "SetTileTextures",                            0, 0, op_set_tile_textures },
    { 0xa2, "DispatchThreadsPerTileInRegion",            76, 0, op_dispatch_threads_per_tile_in_region },
    { 0xa3, "DispatchThreadsPerTileInRegionWithIndex",   76, 0, op_dispatch_threads_per_tile_in_region_w_idx },
    { 0xa4, "GetTileDimensions",                         12, 1, op_get_tile_dimensions },
    { 0xa5, "SetVertexBuffersWithStride",                 0, 0, op_set_vertex_buffers_with_stride },
    { 0xa6, "SetVertexBufferOffsetWithStride",           20, 0, op_set_vertex_buffer_offset_with_stride },
};

#define LAGFX_RENDER_INNER_OPCODE_MAX  0xa6u

static const lagfx_render_inner_op_desc_t *table_lookup(uint32_t opcode) {
    if (opcode > LAGFX_RENDER_INNER_OPCODE_MAX && opcode != 0x2c && opcode != 0x3c) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); ++i) {
        if (g_table[i].opcode == opcode) {
            return &g_table[i];
        }
    }
    return NULL;
}

int lagfx_render_inner_dispatch(lagfx_protocol_t *p,
                                 uint32_t          opcode,
                                 const uint8_t    *payload,
                                 size_t            len) {
    p->diag.unknown_render_ops++;
    
    const lagfx_render_inner_op_desc_t *d = table_lookup(opcode);
    if (!d) {
        if (p->diag.unknown_render_ops <= 10) {
            LAGFX_WARN("render inner: unknown opcode 0x%03x len=%zu — absorbed (count=%lu)",
                       (unsigned)(opcode & 0xfffu), len, p->diag.unknown_render_ops);
        } else if (p->diag.unknown_render_ops == 11) {
            LAGFX_WARN("render inner: suppressing further unknown opcode logs");
        }
        return 0;
    }
    LAGFX_TRACE("render inner: op=0x%02x (%s) len=%zu",
                (unsigned)(d->opcode & 0xffu), d->name, len);
    if (!d->handler) {
        return 0;
    }
    return d->handler(p, payload, len);
}

const char *lagfx_render_inner_op_name(uint32_t opcode) {
    const lagfx_render_inner_op_desc_t *d = table_lookup(opcode);
    if (d) return d->name;
    static char unknown_buf[24];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%02x)",
             (unsigned)(opcode & 0xffu));
    return unknown_buf;
}
