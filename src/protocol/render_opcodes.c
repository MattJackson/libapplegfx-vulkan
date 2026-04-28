/*
 * libapplegfx-vulkan — Render-decoder dispatch table (M5 scaffold)
 * src/protocol/render_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Populates the 96-entry descriptor table for the Render inner-opcode
 * decoder. Most entries point at `render_op_ack_stub`; real handlers
 * land one-by-one as M5 progresses. Entries are listed in numerical
 * opcode order (matches the row order of
 * paravirt-re/library/state-machines/render-decoder-handlers.tsv
 * plus the sub-decoder opcode 0x1a).
 *
 * Source-of-truth row count: 96 (95 main TSV rows + sub-decoder
 * opcode 0x1a RenderDescribeRenderPass).
 *
 * Body sizes ("payload_size" in the TSV) are stored verbatim. Variable-
 * length entries (TSV value of the form "8+N*4") store 0 in `body_size`
 * — handlers that consume them parse the count themselves; the real
 * length arrives via the wire-level totalLengthBytes field, not the
 * descriptor.
 */

#include "render_opcodes.h"
#include "render_pass.h"
#include "state.h"

#include "../common/log.h"
#include "../translate/render_encoder.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p)
         | ((uint64_t)read_le32(p + 4) << 32);
}

static float read_f32(const uint8_t *p) {
    uint32_t u = read_le32(p);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static double read_f64(const uint8_t *p) {
    uint64_t u = read_le64(p);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static int render_op_ack_stub(lagfx_protocol_t *p,
                              const uint8_t    *payload,
                              size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    return 0;
}

bool lagfx_render_op_is_stub(uint32_t opcode) {
    const lagfx_render_op_descriptor_t *d = lagfx_render_op_lookup(opcode);
    return d != NULL && d->default_handler == render_op_ack_stub;
}

static int render_op_set_viewport(lagfx_protocol_t *p,
                                   const uint8_t    *payload,
                                   size_t            len) {
    if (len < 48) {
        LAGFX_WARN("SetViewport: payload too short (%zu < 48)", len);
        return 0;
    }
    double ox = read_f64(payload);
    double oy = read_f64(payload + 8);
    double w  = read_f64(payload + 16);
    double h  = read_f64(payload + 24);
    double zn = read_f64(payload + 32);
    double zf = read_f64(payload + 40);
    LAGFX_TRACE("SetViewport: origin=(%g,%g) size=%gx%g znear=%g zfar=%g",
                ox, oy, w, h, zn, zf);

    if (p && p->render_enc.in_pass) {
#ifdef LAGFX_HAVE_VULKAN
        VkViewport vp = {
            .x        = (float)ox,
            .y        = (float)oy,
            .width    = (float)w,
            .height   = (float)h,
            .minDepth = (float)zn,
            .maxDepth = (float)zf,
        };
        lagfx_translate_render_set_viewport(&p->render_enc, &vp);
#else
        (void)ox; (void)oy; (void)w; (void)h; (void)zn; (void)zf;
#endif
    }
    return 0;
}

static int render_op_set_scissor_rect(lagfx_protocol_t *p,
                                       const uint8_t    *payload,
                                       size_t            len) {
    if (len < 32) {
        LAGFX_WARN("SetScissorRect: payload too short (%zu < 32)", len);
        return 0;
    }
    uint64_t x = read_le64(payload);
    uint64_t y = read_le64(payload + 8);
    uint64_t w = read_le64(payload + 16);
    uint64_t h = read_le64(payload + 24);
    LAGFX_TRACE("SetScissorRect: x=%llu y=%llu w=%llu h=%llu",
                (unsigned long long)x, (unsigned long long)y,
                (unsigned long long)w, (unsigned long long)h);

    if (p && p->render_enc.in_pass) {
#ifdef LAGFX_HAVE_VULKAN
        VkRect2D scissor = {
            .offset = { (int32_t)x, (int32_t)y },
            .extent = { (uint32_t)w, (uint32_t)h },
        };
        lagfx_translate_render_set_scissor(&p->render_enc, &scissor);
#endif
    }
    return 0;
}

static int render_op_set_blend_color(lagfx_protocol_t *p,
                                      const uint8_t    *payload,
                                      size_t            len) {
    if (len < 16) {
        LAGFX_WARN("SetBlendColor: payload too short (%zu < 16)", len);
        return 0;
    }
    float r = read_f32(payload);
    float g = read_f32(payload + 4);
    float b = read_f32(payload + 8);
    float a = read_f32(payload + 12);
    LAGFX_TRACE("SetBlendColor: r=%g g=%g b=%g a=%g",
                (double)r, (double)g, (double)b, (double)a);

    if (p && p->render_enc.in_pass) {
        float rgba[4] = { r, g, b, a };
        lagfx_translate_render_set_blend_color(&p->render_enc, rgba);
    }
    return 0;
}

static int render_op_set_front_facing_winding(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetFrontFacingWinding: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t value = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetFrontFacingWinding: value=%u extra=%u", value, extra);
    return 0;
}

static int render_op_set_cull_mode(lagfx_protocol_t *p,
                                   const uint8_t    *payload,
                                   size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetCullMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t value = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetCullMode: value=%u extra=%u", value, extra);
    return 0;
}

static int render_op_texture_barrier(lagfx_protocol_t *p,
                                     const uint8_t    *payload,
                                     size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    LAGFX_TRACE("TextureBarrier");
    return 0;
}

static int render_op_set_render_pipeline_state(lagfx_protocol_t *p,
                                                const uint8_t    *payload,
                                                size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetRenderPipelineState: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t reference = read_le32(payload);
    LAGFX_TRACE("SetRenderPipelineState: reference=0x%08x", reference);
    return 0;
}

static int render_op_draw_primitives_64(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("DrawPrimitives64: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint64_t vertex_start = read_le64(payload + 4);
    uint64_t vertex_count = read_le64(payload + 12);
    LAGFX_TRACE("DrawPrimitives64: type=%u start=%llu count=%llu",
                prim_type,
                (unsigned long long)vertex_start,
                (unsigned long long)vertex_count);
    return 0;
}

static int render_op_set_vertex_buffers(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexBuffers: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetVertexBuffers: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexBuffers: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        uint64_t off = read_le64(entry + 4);
        LAGFX_TRACE("  [%u] ref=0x%08x offset=%llu",
                    first + i, ref, (unsigned long long)off);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_fragment_textures(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetFragmentTextures: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetFragmentTextures: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetFragmentTextures: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_use_resources(lagfx_protocol_t *p,
                                   const uint8_t    *payload,
                                   size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("UseResources: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t usage = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("UseResources: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("UseResources: count=%u usage=0x%x", count, usage);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_fragment_buffers(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetFragmentBuffers: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetFragmentBuffers: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetFragmentBuffers: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        uint64_t off = read_le64(entry + 4);
        LAGFX_TRACE("  [%u] ref=0x%08x offset=%llu",
                    first + i, ref, (unsigned long long)off);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_draw_primitives_16(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("DrawPrimitives16: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t vertex_start = (uint16_t)read_le32(payload + 4);
    uint16_t vertex_count = (uint16_t)read_le32(payload + 6);
    LAGFX_TRACE("DrawPrimitives16: type=%u start=%u count=%u",
                prim_type, vertex_start, vertex_count);
    return 0;
}

static int render_op_draw_indexed_primitives_64(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 24) {
        LAGFX_WARN("DrawIndexedPrimitives64: payload too short (%zu < 24)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t index_count = read_le32(payload + 4);
    uint32_t index_type = read_le32(payload + 8);
    uint32_t index_buf_ref = read_le32(payload + 12);
    uint64_t index_buf_offset = read_le64(payload + 16);
    LAGFX_TRACE("DrawIndexedPrimitives64: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%llu",
                prim_type, index_count, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset);
    return 0;
}

static int render_op_render_barrier_scope(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("RenderBarrierScope: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t scope = read_le32(payload);
    LAGFX_TRACE("RenderBarrierScope: scope=0x%x", scope);
    return 0;
}

static int render_op_set_color_store_action(lagfx_protocol_t *p,
                                             const uint8_t    *payload,
                                             size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetColorStoreAction: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t action = read_le32(payload);
    uint32_t index = read_le32(payload + 4);
    LAGFX_TRACE("SetColorStoreAction: action=%u index=%u", action, index);
    return 0;
}

static int render_op_set_depth_stencil_state(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetDepthStencilState: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t reference = read_le32(payload);
    LAGFX_TRACE("SetDepthStencilState: reference=0x%08x", reference);
    return 0;
}

static int render_op_set_depth_bias(lagfx_protocol_t *p,
                                     const uint8_t    *payload,
                                     size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetDepthBias: payload too short (%zu < 12)", len);
        return 0;
    }
    float bias = read_f32(payload);
    float slope = read_f32(payload + 4);
    float clamp = read_f32(payload + 8);
    LAGFX_TRACE("SetDepthBias: bias=%g slope=%g clamp=%g",
                (double)bias, (double)slope, (double)clamp);
    return 0;
}

static int render_op_set_depth_clip_mode(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetDepthClipMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetDepthClipMode: mode=%u extra=%u", mode, extra);
    return 0;
}

static int render_op_set_fragment_sampler_states(lagfx_protocol_t *p,
                                                  const uint8_t    *payload,
                                                  size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetFragmentSamplerStates: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetFragmentSamplerStates: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetFragmentSamplerStates: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_stencil_ref(lagfx_protocol_t *p,
                                      const uint8_t    *payload,
                                      size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetStencilRef: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t front_ref = read_le32(payload);
    uint32_t back_ref = read_le32(payload + 4);
    LAGFX_TRACE("SetStencilRef: front=%u back=%u", front_ref, back_ref);
    return 0;
}

static int render_op_set_vertex_textures(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexTextures: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetVertexTextures: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexTextures: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_vertex_buffers_with_stride(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexBuffersWithStride: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 20;
    if (len < needed) {
        LAGFX_WARN("SetVertexBuffersWithStride: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexBuffersWithStride: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 20;
        uint32_t ref = read_le32(entry);
        uint64_t off = read_le64(entry + 4);
        uint64_t stride = read_le64(entry + 12);
        LAGFX_TRACE("  [%u] ref=0x%08x offset=%llu stride=%llu",
                    first + i, ref,
                    (unsigned long long)off, (unsigned long long)stride);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static lagfx_render_pass_desc_t g_last_render_pass_desc;

static int render_op_describe_render_pass(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    lagfx_render_pass_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int rc = lagfx_parse_render_pass_descriptor(payload, len, &desc);
    if (rc != 0) {
        LAGFX_ERR("render_op_describe_render_pass: parse failed (rc=%d len=%zu)",
                  rc, len);
        return rc;
    }
    g_last_render_pass_desc = desc;
    LAGFX_LOG("render_op_describe_render_pass: depth=%u stencil=%u "
              "colors=%u rt=%llux%llu",
              desc.has_depth, desc.has_stencil,
              desc.color_attachment_count,
              (unsigned long long)desc.render_target_width,
              (unsigned long long)desc.render_target_height);
    return 0;
}

const lagfx_render_pass_desc_t *
lagfx_render_pass_desc_get(void) {
    return &g_last_render_pass_desc;
}

/* === Descriptor table — 96 entries ============================
 *
 * Layout: { opcode, name, body_size, ref_count, default_handler }.
 *
 * `name` strings match the `name` column of the TSV exactly. */
static const lagfx_render_op_descriptor_t g_render_op_table[] = {
    /* --- Draw family (0x00-0x1d) -------------------------------- */
    { 0x00, "DrawPrimitives64",                          20, 0, render_op_draw_primitives_64 },
    { 0x01, "DrawPrimitives16",                           8, 0, render_op_draw_primitives_16 },
    { 0x02, "DrawInstancedPrimitives64",                 28, 0, render_op_ack_stub },
    { 0x03, "DrawInstancedPrimitives16",                  8, 0, render_op_ack_stub },
    { 0x04, "DrawInstancedBasePrimitives64",             36, 0, render_op_ack_stub },
    { 0x05, "DrawInstancedBasePrimitives16",             12, 0, render_op_ack_stub },
    { 0x06, "DrawIndexedPrimitives64",                   24, 1, render_op_draw_indexed_primitives_64 },
    { 0x07, "DrawIndexedPrimitives16",                   12, 1, render_op_ack_stub },
    { 0x08, "DrawIndexedInstancedPrimitives64",          32, 1, render_op_ack_stub },
    { 0x09, "DrawIndexedInstancedPrimitives16",          16, 1, render_op_ack_stub },
    { 0x0a, "DrawIndexedInstancedBasePrimitives64",      48, 1, render_op_ack_stub },
    { 0x0b, "DrawIndexedInstancedBasePrimitives16",      20, 1, render_op_ack_stub },
    { 0x0c, "DrawPatches64",                             48, 1, render_op_ack_stub },
    { 0x0d, "DrawPatches16",                             16, 1, render_op_ack_stub },
    { 0x0e, "DrawIndexedPatches64",                      60, 2, render_op_ack_stub },
    { 0x0f, "DrawIndexedPatches16",                      24, 2, render_op_ack_stub },
    { 0x10, "DrawPrimitivesIndirect",                    16, 1, render_op_ack_stub },
    { 0x11, "DrawIndexedPrimitivesIndirect",             28, 2, render_op_ack_stub },
    { 0x12, "DrawPatchesIndirect",                       28, 2, render_op_ack_stub },
    { 0x13, "DrawIndexedPatchesIndirect",                40, 3, render_op_ack_stub },
    { 0x14, "ExecuteCommandsInBuffer",                   16, 2, render_op_ack_stub },
    { 0x15, "ExecuteCommandsInBufferRanged",             20, 1, render_op_ack_stub },
    { 0x16, "RenderBarrierResources",                     0, 0, render_op_ack_stub },
    { 0x17, "RenderBarrierScope",                         4, 0, render_op_render_barrier_scope },
    { 0x18, "RenderUpdateFence",                          8, 1, render_op_ack_stub },
    { 0x19, "RenderWaitForFence",                         8, 1, render_op_ack_stub },
    { 0x1a, "RenderDescribeRenderPass",                   584, 0, render_op_describe_render_pass },
    /* TSV body_size for 0x1b is "8+N*4" (variable) — store 0. */
    { 0x1b, "UseHeapsWithStages",                         0, 0, render_op_ack_stub },
    { 0x1c, "DrawIndexedInstancedBasePrimitives64_2",    48, 1, render_op_ack_stub },
    { 0x1d, "DrawIndexedInstancedBasePrimitives16_2",    20, 1, render_op_ack_stub },

    /* --- State-set family (0x65-0xa6) --------------------------- */
    { 0x65, "SetBlendColor",                             16, 0, render_op_set_blend_color },
    { 0x66, "SetColorStoreAction",                        8, 0, render_op_set_color_store_action },
    { 0x67, "SetColorStoreActionOptions",                12, 0, render_op_ack_stub },
    { 0x68, "SetDepthStencilState",                       4, 1, render_op_set_depth_stencil_state },
    { 0x69, "SetDepthStoreAction",                        8, 0, render_op_ack_stub },
    { 0x6a, "SetDepthStoreActionOptions",                 8, 0, render_op_ack_stub },
    { 0x6b, "SetCullMode",                                8, 0, render_op_set_cull_mode },
    { 0x6c, "SetDepthBias",                              12, 0, render_op_set_depth_bias },
    { 0x6d, "SetDepthClipMode",                           8, 0, render_op_set_depth_clip_mode },
    /* 0x6e SetFragmentBuffers — variable "8+N*12". */
    { 0x6e, "SetFragmentBuffers",                         0, 0, render_op_set_fragment_buffers },
    { 0x6f, "SetFragmentBufferOffset",                   12, 0, render_op_ack_stub },
    /* 0x70 SetFragmentSamplerStates — variable "8+N*4". */
    { 0x70, "SetFragmentSamplerStates",                   0, 0, render_op_set_fragment_sampler_states },
    /* 0x71 SetFragmentSamplerStatesLODClamp — variable "8+N*12". */
    { 0x71, "SetFragmentSamplerStatesLODClamp",           0, 0, render_op_ack_stub },
    /* 0x72 SetFragmentTextures — variable "8+N*4". */
    { 0x72, "SetFragmentTextures",                        0, 0, render_op_set_fragment_textures },
    { 0x73, "SetFrontFacingWinding",                      8, 0, render_op_set_front_facing_winding },
    { 0x74, "SetRenderPipelineState",                     4, 1, render_op_set_render_pipeline_state },
    { 0x75, "SetScissorRect",                            32, 0, render_op_set_scissor_rect },
    /* 0x76 SetScissorRects — variable "8+N*32". */
    { 0x76, "SetScissorRects",                            0, 0, render_op_ack_stub },
    { 0x77, "SetStencilRef",                              8, 0, render_op_set_stencil_ref },
    { 0x78, "SetStencilStoreAction",                      8, 0, render_op_ack_stub },
    { 0x79, "SetStencilStoreActionOptions",               8, 0, render_op_ack_stub },
    { 0x7a, "SetTesselationFactorBuffer",                20, 1, render_op_ack_stub },
    { 0x7b, "SetTesselationFactorScale",                  4, 0, render_op_ack_stub },
    { 0x7c, "SetTriangleFillMode",                        8, 0, render_op_ack_stub },
    /* 0x7d SetVertexBuffers — variable "8+N*12". */
    { 0x7d, "SetVertexBuffers",                           0, 0, render_op_set_vertex_buffers },
    { 0x7e, "SetVertexBufferOffset",                     12, 0, render_op_ack_stub },
    /* 0x7f SetVertexSamplerStates — variable "8+N*4". */
    { 0x7f, "SetVertexSamplerStates",                     0, 0, render_op_ack_stub },
    /* 0x80 SetVertexSamplerStatesLODClamp — variable "8+N*12". */
    { 0x80, "SetVertexSamplerStatesLODClamp",             0, 0, render_op_ack_stub },
    /* 0x81 SetVertexTextures — variable "8+N*4". */
    { 0x81, "SetVertexTextures",                          0, 0, render_op_set_vertex_textures },
    { 0x82, "SetViewport",                               48, 0, render_op_set_viewport },
    /* 0x83 SetViewports — variable "4+N*48". */
    { 0x83, "SetViewports",                               0, 0, render_op_ack_stub },
    { 0x84, "SetVisibilityResultMode",                   16, 0, render_op_ack_stub },
    { 0x85, "TextureBarrier",                             0, 0, render_op_texture_barrier },
    /* 0x86 UseHeaps — variable "4+N*4". */
    { 0x86, "UseHeaps",                                   0, 0, render_op_ack_stub },
    /* 0x87 UseResources — variable "8+N*4". */
    { 0x87, "UseResources",                               0, 0, render_op_use_resources },
    { 0x88, "SetLineWidth",                               4, 0, render_op_ack_stub },
    /* 0x89 UseResourcesWithStages — variable "8+N*4". */
    { 0x89, "UseResourcesWithStages",                     0, 0, render_op_ack_stub },
    { 0x8a, "SetAlphaTestReferenceValue",                 4, 0, render_op_ack_stub },
    { 0x8b, "SetPointSize",                               4, 0, render_op_ack_stub },
    { 0x8c, "SetClipPlane",                              20, 0, render_op_ack_stub },
    { 0x8d, "SetVertexSamplerState",                     20, 1, render_op_ack_stub },
    { 0x8e, "SetFragmentSamplerState",                   20, 1, render_op_ack_stub },
    { 0x8f, "SetViewportTransformEnabled",                4, 0, render_op_ack_stub },
    { 0x90, "SetProvokingVertexMode",                     4, 0, render_op_ack_stub },
    { 0x91, "SetPrimitiveRestartIndexEnabled",            8, 0, render_op_ack_stub },
    { 0x92, "SetTriangleFillModeFrontBack",               4, 0, render_op_ack_stub },
    { 0x93, "SetTransformFeedbackState",                  4, 0, render_op_ack_stub },
    { 0x94, "SetDepthCleared",                            0, 0, render_op_ack_stub },
    { 0x95, "SetStencilCleared",                          0, 0, render_op_ack_stub },
    { 0x96, "SetColorResolveTexture",                    16, 1, render_op_ack_stub },
    { 0x97, "SetDepthResolveTexture",                    12, 1, render_op_ack_stub },
    { 0x98, "SetStencilResolveTexture",                  12, 1, render_op_ack_stub },
    { 0x99, "SetVertexAmplificationMode",                 8, 0, render_op_ack_stub },
    /* 0x9a SetVertexAmplificationCount — variable "4+N*8". */
    { 0x9a, "SetVertexAmplificationCount",                0, 0, render_op_ack_stub },
    { 0x9b, "DispatchThreadsPerTile",                    24, 0, render_op_ack_stub },
    { 0x9c, "SetRenderThreadgroupMemoryLength",          20, 0, render_op_ack_stub },
    /* 0x9d SetTileBuffers — variable "8+N*12". */
    { 0x9d, "SetTileBuffers",                             0, 0, render_op_ack_stub },
    { 0x9e, "SetTileBufferOffset",                       12, 0, render_op_ack_stub },
    /* 0x9f SetTileSamplerStates — variable "8+N*4". */
    { 0x9f, "SetTileSamplerStates",                       0, 0, render_op_ack_stub },
    /* 0xa0 SetTileSamplerStatesLODClamp — variable "8+N*12". */
    { 0xa0, "SetTileSamplerStatesLODClamp",               0, 0, render_op_ack_stub },
    /* 0xa1 SetTileTextures — variable "8+N*4". */
    { 0xa1, "SetTileTextures",                            0, 0, render_op_ack_stub },
    { 0xa2, "DispatchThreadsPerTileInRegion",            76, 0, render_op_ack_stub },
    { 0xa3, "DispatchThreadsPerTileInRegionWithIndex",   76, 0, render_op_ack_stub },
    { 0xa4, "GetTileDimensions",                         12, 1, render_op_ack_stub },
    /* 0xa5 SetVertexBuffersWithStride — variable "8+N*20". */
    { 0xa5, "SetVertexBuffersWithStride",                 0, 0, render_op_set_vertex_buffers_with_stride },
    { 0xa6, "SetVertexBufferOffsetWithStride",           20, 0, render_op_ack_stub },
};

static const size_t g_render_op_table_count =
    sizeof(g_render_op_table) / sizeof(g_render_op_table[0]);

/* Compile-time guarantee that the table size matches the public count
 * declared in render_opcodes.h. If the TSV ever grows/shrinks, the
 * header constant must be bumped in lockstep. */
_Static_assert(sizeof(g_render_op_table) / sizeof(g_render_op_table[0]) ==
               LAGFX_RENDER_OPCODE_COUNT,
               "render_opcodes.c table size must match "
               "LAGFX_RENDER_OPCODE_COUNT (96)");

const lagfx_render_op_descriptor_t *
lagfx_render_op_lookup(uint32_t opcode) {
    if (opcode > LAGFX_RENDER_OPCODE_MAX) {
        return NULL;
    }
    for (size_t i = 0; i < g_render_op_table_count; ++i) {
        if (g_render_op_table[i].opcode == opcode) {
            return &g_render_op_table[i];
        }
    }
    return NULL;
}

size_t lagfx_render_op_table_size(void) {
    return g_render_op_table_count;
}

const lagfx_render_op_descriptor_t *
lagfx_render_op_table_entry(size_t index) {
    if (index >= g_render_op_table_count) {
        return NULL;
    }
    return &g_render_op_table[index];
}

const char *lagfx_render_op_name(uint32_t opcode) {
    const lagfx_render_op_descriptor_t *d = lagfx_render_op_lookup(opcode);
    if (d) {
        return d->name;
    }
    /* Per src/protocol/protocol.h header comment, the decoder runs
     * single-threaded, so a file-static buffer is safe. */
    static char unknown_buf[24];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%02x)",
             (unsigned)(opcode & 0xffu));
    return unknown_buf;
}
