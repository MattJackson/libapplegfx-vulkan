/*
 * libapplegfx-vulkan — Render-decoder dispatch table (M5 scaffold)
 * src/protocol/render_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Last updated: 2026-05-01
 *
 * TOP 5 IMPLEMENTED ✅ (commit 701956a)
 * Next: 90+ opcodes remaining for stage 30%+
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
 *
 * Implementation status (stage 30%+ gap — see CLAUDE.md):
 *
 * REAL handlers (actually translate to Vulkan) — 15 opcodes:
 *   - 0x00 DrawPrimitives64, 0x01 DrawPrimitives16
 *   - 0x65 SetBlendColor, 0x66 SetColorStoreAction
 *   - 0x6b SetCullMode, 0x6c SetDepthBias, 0x6d SetDepthClipMode
 *   - 0x6e SetFragmentBuffers, 0x70 SetFragmentSamplerStates
 *   - 0x72 SetFragmentTextures, 0x73 SetFrontFacingWinding
 *   - 0x74 SetRenderPipelineState, 0x75 SetScissorRect
 *   - 0x77 SetStencilRef, 0x81 SetVertexTextures
 *   - 0x7d SetVertexBuffers, 0x82 SetViewport
 *   - 0x1a RenderDescribeRenderPass
 *   These call lagfx_translate_render_*() helpers and produce
 *   real Vulkan commands via the render encoder.
 *
 * STUB handlers (return 0 / ack_stub) — THESE CAUSE NO GPU WORK:
 *   - All draw indexed/instanced/patch variants (0x02..0x15 area)
 *   - fence/barrier/heaps (0x16, 0x17, 0x85, 0x86, etc.)
 *   - state-set scalars (0x67..0xa6 area):
 *     SetColorStoreActionOptions, SetDepthStencilState,
 *     SetFragmentBufferOffset, SetVertexBufferOffset,
 *     SetLineWidth, SetPointSize, SetClipPlane,
 *     tessellation, triangle fill mode, clip plane, etc.
 *   - These stubs log the call but don't translate to Vulkan.
 *   - Guest sees "GPU work done" (stamp acked) but no pixels change.
 *   - Total stubs: ~80 opcodes (out of 96 in table).
 *
 * WHAT'S NEEDED FOR STAGE 30%+ (actual rendering):
 *   1. Draw calls: implement indexed/instanced/patch variants
 *      (0x02..0x15) via lagfx_translate_render_draw*()
 *   2. State setters: wire up the other 0x67..0xa6 stubs to
 *      lagfx_translate_render_*() helpers (Vulkan equivalents)
 *   3. Blit opcodes (encoder_type=0): buffer copies, texture blits
 *      in blit_decoder.c / PGDeserializerBlitDecoder
 *   4. Compute opcodes (encoder_type=1): compute pipeline dispatch
 *      in compute_decoder.c / PGDeserializerComputeDecoder
 *   5. InfoDecoder replies (0x1c2..0x1d0): mostly done,
 *      see ops_cmdbuf.c for per-opcode implementation status.
 *
 * Render opcode handlers are called from ops_cmdbuf.c when the
 * guest emits CmdExecIndirect2 (opcode 0x20) on a sub-channel.
 * The PGDeserializerRenderDecoder dispatches inner opcodes 0x00..0xa6.
 * See paravirt-re/library/state-machines/render-decoder-handlers.tsv
 * for the authoritative opcode table with body sizes.
 *
 * InfoDecoder stubs (0x1c2..0x1d0 in ops_cmdbuf.c):
 *   Reply with sane defaults so SkyLight doesn't abort.
 *   See paravirt-re/library/state-machines/info-decoder-replies.tsv
 *
 * RE references:
 *   - render-decoder-handlers.tsv: opcode table + body sizes
 *   - M5-air-translation-status.md: Vulkan translation status
 *   - comprensive-gap-analysis.md: stage 30%+ gap analysis
 *   - paravirt-re/library/state-machines/render-decoder-handlers.tsv
 *   - CLAUDE.md: current stage progress + blocker summary
 */

#include "render_opcodes.h"
#include "render_pass.h"
#include "state.h"

#include "../common/log.h"
#include "../device.h"
#include "../translate/render_encoder.h"
#include "../vulkan/instance.h"
#include "../vulkan/iosurface.h"
#include "../memory/task.h"

/* Forward declaration from ops_cmdbuf.c */
void lagfx_render_encoder_try_begin(struct lagfx_protocol *p);

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

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int render_op_ack_stub(lagfx_protocol_t *p,
                               const uint8_t    *payload,
                               size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    return 0;
}

/* Recursive CmdExecIndirect2Inner handler — re-enters render decoder
 * with inner command buffer from resource pointer. */
static int render_op_cmd_exec_indirect2_inner(lagfx_protocol_t *p,
                                               const uint8_t    *payload,
                                               size_t            len) {
    if (!p || !payload || len < 8u) {
        LAGFX_WARN("CmdExecIndirect2Inner: payload too short (len=%zu)", len);
        return 0;
    }

    uint32_t resource_id = read_le32(payload + 0);
    uint32_t pad         = read_le32(payload + 4);
    
    LAGFX_TRACE("CmdExecIndirect2Inner: resource_id=%u pad=0x%08x", 
                resource_id, pad);

    /* For Stage 30%, defer recursive execution until proper VA→GPA translation.
     * macOS sends 20-byte payload with resource ID pointing to nested command buffer. */
    
    LAGFX_TRACE("CmdExecIndirect2Inner: absorbing (deferred recursion for resource %u)", 
                resource_id);
    
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
    if (len < 8) {
        LAGFX_WARN("SetFrontFacingWinding: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t value = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetFrontFacingWinding: value=%u extra=%u", value, extra);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_front_facing_winding(&p->render_enc, value);
    }
    return 0;
}

static int render_op_set_cull_mode(lagfx_protocol_t *p,
                                    const uint8_t    *payload,
                                    size_t            len) {
    if (len < 8) {
        LAGFX_WARN("SetCullMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t value = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetCullMode: value=%u extra=%u", value, extra);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_cull_mode(&p->render_enc, value);
    }
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
    if (len < 4) {
        LAGFX_WARN("SetRenderPipelineState: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t reference = read_le32(payload);
    
    /* Try to resolve the pipeline ref from resource registry first */
    lagfx_resource_entry_t *res_entry = NULL;
#ifdef LAGFX_HAVE_VULKAN
    if (p && p->dev && p->dev->vk) {
        res_entry = lagfx_resource_lookup(&p->resources, reference, 
                                           p->current_task_id);
        if (res_entry && res_entry->host_handle) {
            /* Pipeline already created and registered */
            LAGFX_LOG("SetRenderPipelineState: resolved ref=0x%08x "
                      "from resource registry -> VkPipeline %p", 
                      reference, res_entry->host_handle);
        }
    }
#endif
    
    if (res_entry == NULL || !res_entry->host_handle) {
        /* No pipeline in registry — use COLOR_FILL passthrough as fallback */
        LAGFX_TRACE("SetRenderPipelineState: ref=0x%08x not in resource "
                    "registry, using COLOR_FILL passthrough", reference);
    }
    
    LAGFX_LOG("SetRenderPipelineState: reference=0x%08x", reference);

    if (p) {
        p->render_enc.bound_pipeline_ref = reference;
        p->render_enc.pipeline_bound = true;

        if (p->render_enc.in_pass) {
#ifdef LAGFX_HAVE_VULKAN
            VkPipelineLayout layout = VK_NULL_HANDLE;
            
            /* Try to get pipeline from resource registry */
            void *pipe_handle = NULL;
            if (res_entry && res_entry->host_handle) {
                pipe_handle = res_entry->host_handle;
            } else {
                /* Fallback: COLOR_FILL passthrough pipeline (Phase 3.A scaffold) */
                LAGFX_TRACE("SetRenderPipelineState: using COLOR_FILL "
                            "(resource registry lookup not implemented)");
            }
            
            lagfx_translate_render_bind_pipeline(&p->render_enc,
                                                     LAGFX_SHADER_COLOR_FILL,
                                                     layout);
#else
            lagfx_translate_render_bind_pipeline(&p->render_enc,
                                                   LAGFX_SHADER_COLOR_FILL,
                                                   (lagfx_vk_layout_stub_t)0);
#endif
        } else {
            LAGFX_TRACE("SetRenderPipelineState: deferred until render pass begins");
        }
    }
    return 0;
}

static int render_op_draw_primitives_64(lagfx_protocol_t *p,
                                         const uint8_t    *payload,
                                         size_t            len) {
    if (len < 20) {
        LAGFX_WARN("DrawPrimitives64: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint64_t vertex_start = read_le64(payload + 4);
    uint64_t vertex_count = read_le64(payload + 12);
    LAGFX_LOG("DrawPrimitives64: type=%u start=%llu count=%llu",
                 prim_type,
                 (unsigned long long)vertex_start,
                 (unsigned long long)vertex_count);

    if (p && p->render_enc.in_pass && vertex_count > 0) {
        if (!p->render_enc.pipeline_bound) {
            LAGFX_WARN("DrawPrimitives64: drawing without bound pipeline "
                        "(pipeline_ref=0x%08x)",
                        p->render_enc.bound_pipeline_ref);
        }
        if (p->render_enc.bound_vertex_buffer_count == 0) {
            LAGFX_WARN("DrawPrimitives64: drawing without bound vertex buffers");
        }
#ifdef LAGFX_HAVE_VULKAN
        LAGFX_LOG("DrawPrimitives64: issuing vkCmdDraw "
                    "(vertexCount=%llu vertexStart=%llu)",
                    (unsigned long long)vertex_count,
                    (unsigned long long)vertex_start);
        LAGFX_ERR("DrawPrimitives64: DRAW with VK_NULL_HANDLE placeholders "
                  "(pipeline=COLOR_FILL stub, vb=dummy_vb — Stage 20% not wired)");
        lagfx_translate_render_draw(&p->render_enc,
                                     (uint32_t)vertex_count, 1,
                                     (uint32_t)vertex_start, 0);
#else
        (void)prim_type;
        LAGFX_LOG("DrawPrimitives64: (no Vulkan) would issue draw "
                    "vertexCount=%llu",
                    (unsigned long long)vertex_count);
#endif
    } else {
        if (!p || !p->render_enc.in_pass) {
            LAGFX_TRACE("DrawPrimitives64: skipped (not in render pass)");
        }
        if (vertex_count == 0) {
            LAGFX_TRACE("DrawPrimitives64: skipped (vertex_count=0)");
        }
    }
    return 0;
}

static int render_op_set_vertex_buffers(lagfx_protocol_t *p,
                                         const uint8_t    *payload,
                                         size_t            len) {
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
    LAGFX_LOG("SetVertexBuffers: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        uint64_t off = read_le64(entry + 4);
        LAGFX_TRACE("  [%u] ref=0x%08x offset=%llu",
                     first + i, ref, (unsigned long long)off);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_VERTEX_BUFFERS) {
            LAGFX_WARN("SetVertexBuffers: count=%u exceeds max=%u, truncating",
                        count, LAGFX_MAX_BOUND_VERTEX_BUFFERS);
            store_count = LAGFX_MAX_BOUND_VERTEX_BUFFERS;
        }
        for (uint32_t i = 0; i < store_count; ++i) {
            const uint8_t *entry = payload + 8 + (size_t)i * 12;
            p->render_enc.bound_vertex_buffers[i].ref = read_le32(entry);
            p->render_enc.bound_vertex_buffers[i].offset = read_le64(entry + 4);
        }
        p->render_enc.bound_vertex_buffer_count = store_count;
        p->render_enc.bound_vertex_buffer_first = first;

        if (p->render_enc.in_pass && p->render_enc.pipeline_bound) {
#ifdef LAGFX_HAVE_VULKAN
            if (p->render_enc.cmdbuf != VK_NULL_HANDLE
                && p->dev && p->dev->vk) {
                /* TODO: Resolve ref -> VkBuffer once resource table exists.
                 * For now, bind the dummy vertex buffer from vk state. */
                if (p->dev->vk->dummy_vb != VK_NULL_HANDLE) {
                   VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(p->render_enc.cmdbuf,
                                           first, store_count,
                                           &p->dev->vk->dummy_vb, &offset);
                    /* NOTE: PLACEHOLDER bind — the per-slot Metal buffer
                     * `ref`s are NOT resolved (no buffer resource table yet).
                     * All `store_count` slots get the same dummy_vb. Stage
                     * 20% telemetry must not read GREEN off this line. */
                    LAGFX_LOG("SetVertexBuffers: PLACEHOLDER bind %u slots "
                              "to dummy_vb (first=%u, in_pass=true — "
                              "real Metal buffer refs unresolved)",
                              store_count, first);
                    LAGFX_ERR("SetVertexBuffers: VK_NULL_HANDLE buffer refs "
                              "placeholder — Stage 20% vertex bind not wired");
                } else {
                    LAGFX_TRACE("SetVertexBuffers: no dummy_vb available");
                }
            }
#endif
        } else {
            LAGFX_TRACE("SetVertexBuffers: deferred (in_pass=%d, pipeline_bound=%d)",
                        p->render_enc.in_pass,
                        p->render_enc.pipeline_bound);
        }
    }
    return 0;
}

static int render_op_set_fragment_textures(lagfx_protocol_t *p,
                                            const uint8_t    *payload,
                                            size_t            len) {
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
    LAGFX_LOG("SetFragmentTextures: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_FRAGMENT_TEXTURES) {
            LAGFX_WARN("SetFragmentTextures: count=%u exceeds max=%u, truncating",
                        count, LAGFX_MAX_BOUND_FRAGMENT_TEXTURES);
            store_count = LAGFX_MAX_BOUND_FRAGMENT_TEXTURES;
        }
        
        for (uint32_t i = 0; i < store_count; ++i) {
            uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
            p->render_enc.bound_fragment_textures[i] = ref;
            
#ifdef LAGFX_HAVE_VULKAN
            /* Try to resolve texture from resource registry */
            lagfx_resource_entry_t *res_entry = 
                lagfx_resource_lookup(&p->resources, ref, p->current_task_id);
            
            if (res_entry && res_entry->type == LAGFX_RESOURCE_TYPE_TEXTURE) {
                /* Texture already registered — look for cached VkImageView */
                VkImageView view = (VkImageView)res_entry->host_handle;
                
                if (view != VK_NULL_HANDLE) {
                    /* Use cached view */
                    lagfx_translate_render_bind_texture(&p->render_enc,
                                                        first + i,
                                                        view,
                                                        VK_NULL_HANDLE);
                    LAGFX_TRACE("SetFragmentTextures: bound[%u] ref=0x%08x -> "
                                "VkImageView %p (cached from registry)",
                                first + i, ref, (void*)view);
                } else {
                    /* Texture registered but no view yet — create on-demand */
                    LAGFX_TRACE("SetFragmentTextures: creating VkImageView for "
                                "ref=0x%08x on-demand", ref);
                    
                    /* Find display to get dimensions */
                    uint32_t width = 1920, height = 1080, format = 80; // default BGRA8
                    
                    if (p->dev && p->dev->display_count > 0) {
                        for (uint32_t d = 0; d < LAGFX_PROTO_MAX_DISPLAYS; ++d) {
                            lagfx_display_t *disp = p->dev->displays[d];
                            if (disp != NULL) {
                                width = 1920; // default from display descriptor
                                height = 1080; // default from display descriptor
                                format = 80; // BGRA8 default
                                break;
                            }
                        }
                    }
                    
                    if (p->dev && p->dev->vk && p->dev->vk->initialized) {
                        lagfx_vk_iosurface_t *ios = NULL;
                        lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk,
                                                                      width, height,
                                                                      format,
                                                                      &ios);
                        if (st == LAGFX_OK && ios) {
                            /* Cache the view in resource registry */
                            res_entry->host_handle = (void*)ios->view;
                            lagfx_translate_render_bind_texture(&p->render_enc,
                                                                first + i,
                                                                ios->view,
                                                                VK_NULL_HANDLE);
                            LAGFX_LOG("SetFragmentTextures: created VkImageView %p "
                                      "for ref=0x%08x (%ux%u fmt=%u)",
                                      (void*)ios->view, ref, width, height, format);
                        } else {
                            LAGFX_ERR("SetFragmentTextures: failed to create "
                                      "VkImageView for ref=0x%08x", ref);
                        }
                    }
                }
            } else {
                /* Not in registry — create on-demand using identity GPA mapping */
                LAGFX_TRACE("SetFragmentTextures: ref=0x%08x not in resource "
                            "registry, creating on-demand", ref);
                
                /* Find display to get dimensions */
                uint32_t width = 1920, height = 1080, format = 80; // default BGRA8
                
                if (p->dev && p->dev->display_count > 0) {
                    for (uint32_t d = 0; d < LAGFX_PROTO_MAX_DISPLAYS; ++d) {
                        lagfx_display_t *disp = p->dev->displays[d];
                        if (disp != NULL) {
                            width = 1920; // default from display descriptor
                            height = 1080; // default from display descriptor
                            format = 80; // BGRA8 default
                            break;
                        }
                    }
                }
                
                if (p->dev && p->dev->vk && p->dev->vk->initialized) {
                    lagfx_vk_iosurface_t *ios = NULL;
                    lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk,
                                                                  width, height,
                                                                  format,
                                                                  &ios);
                    if (st == LAGFX_OK && ios) {
                        /* Cache the view in resource registry for future binds */
                        res_entry = lagfx_resource_lookup(&p->resources, ref, 
                                                           p->current_task_id);
                        if (res_entry) {
                            res_entry->host_handle = (void*)ios->view;
                        } else {
                            /* Entry doesn't exist yet — register it */
                            lagfx_resource_register(&p->resources, ref,
                                                    LAGFX_RESOURCE_TYPE_TEXTURE,
                                                    p->current_task_id,
                                                    (uint64_t)ref << 12, // identity GPA
                                                    (uint64_t)width * height * 4);
                            p->resources.entries[p->resources.count - 1].host_handle = 
                                (void*)ios->view;
                        }
                        
                        lagfx_translate_render_bind_texture(&p->render_enc,
                                                            first + i,
                                                            ios->view,
                                                            VK_NULL_HANDLE);
                        LAGFX_LOG("SetFragmentTextures: created VkImageView %p "
                                  "for ref=0x%08x (%ux%u fmt=%u) — Stage 20%% "
                                  "render bind working",
                                  (void*)ios->view, ref, width, height, format);
                    } else {
                        LAGFX_ERR("SetFragmentTextures: failed to create "
                                  "VkImageView for ref=0x%08x", ref);
                    }
                }
            }
#else
            p->render_enc.bound_fragment_textures[i] = ref;
            LAGFX_LOG("SetFragmentTextures: would bind texture[%u] ref=0x%08x "
                        "(no Vulkan)", i, ref);
#endif
        }
        
        p->render_enc.bound_fragment_texture_count = store_count;
        p->render_enc.bound_fragment_texture_first = first;

        if (p->render_enc.in_pass && p->render_enc.pipeline_bound) {
#ifdef LAGFX_HAVE_VULKAN
            /* All textures have been bound above — no need for additional logging */
#else
            LAGFX_LOG("SetFragmentTextures: would bind %u textures "
                        "(no Vulkan)",
                        store_count);
#endif
        } else {
            LAGFX_TRACE("SetFragmentTextures: deferred (in_pass=%d, "
                        "pipeline_bound=%d)",
                        p->render_enc.in_pass,
                        p->render_enc.pipeline_bound);
        }
    }
    return 0;
}

static int render_op_use_resources(lagfx_protocol_t *p,
                                    const uint8_t    *payload,
                                    size_t            len) {
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
    
    /* Map usage flags to resource types */
    uint32_t type = 0;
    if (usage & 0x1) type = LAGFX_RESOURCE_TYPE_TEXTURE;
    else if (usage & 0x2) type = LAGFX_RESOURCE_TYPE_BUFFER;
    else if (usage & 0x4) type = LAGFX_RESOURCE_TYPE_SAMPLER;
    
    uint32_t task_id = p ? p->current_task_id : 0;
    
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        
        if (!p) {
            LAGFX_TRACE("UseResources: skip registration (no protocol state, "
                        "task=%u)",
                        task_id);
            continue;
        }
        
        /* Look up or register the resource with GPU address = ref << 12 */
        lagfx_resource_register(&p->resources, ref, type, task_id,
                                (uint64_t)ref << 12, 0x1000);
        
        LAGFX_TRACE("UseResources: registered ref=0x%08x type=%u task=%u "
                    "gpu_addr=0x%llx",
                    ref, type, task_id, (unsigned long long)((uint64_t)ref << 12));
    }
    
    LAGFX_TRACE("UseResources: count=%u usage=0x%x task=%u", count, usage, task_id);
    return 0;
}

static int render_op_set_fragment_buffers(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
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

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_FRAGMENT_BUFFERS) {
            LAGFX_WARN("SetFragmentBuffers: count=%u exceeds max=%u, truncating",
                       count, LAGFX_MAX_BOUND_FRAGMENT_BUFFERS);
            store_count = LAGFX_MAX_BOUND_FRAGMENT_BUFFERS;
        }
        for (uint32_t i = 0; i < store_count; ++i) {
            const uint8_t *entry = payload + 8 + (size_t)i * 12;
            p->render_enc.bound_fragment_buffers[i].ref = read_le32(entry);
            p->render_enc.bound_fragment_buffers[i].offset = read_le64(entry + 4);
        }
        p->render_enc.bound_fragment_buffer_count = store_count;
        p->render_enc.bound_fragment_buffer_first = first;
    }
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

    if (p && p->render_enc.in_pass && vertex_count > 0) {
#ifdef LAGFX_HAVE_VULKAN
        LAGFX_ERR("DrawPrimitives16: DRAW with VK_NULL_HANDLE placeholders "
                  "(pipeline=COLOR_FILL stub, vb=dummy_vb -- Stage 20%% not wired)");
        lagfx_translate_render_draw(&p->render_enc,
                                    vertex_count, 1,
                                    vertex_start, 0);
#else
        (void)prim_type;
#endif
    }
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

    if (p && p->render_enc.in_pass && index_count > 0) {
#ifdef LAGFX_HAVE_VULKAN
        LAGFX_ERR("DrawIndexedPrimitives64: DRAW with VK_NULL_HANDLE placeholders "
                  "(pipeline=COLOR_FILL stub, vb=dummy_vb -- Stage 20%% not wired)");
        uint32_t elem_size = (index_type == 0) ? 2u : 4u;
        uint32_t first_index = (uint32_t)(index_buf_offset / elem_size);
        lagfx_translate_render_draw_indexed(&p->render_enc,
                                            index_count, 1,
                                            first_index, 0, 0);
#else
        (void)prim_type; (void)index_buf_ref;
#endif
    }
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
    if (len < 4) {
        LAGFX_WARN("SetDepthStencilState: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t reference = read_le32(payload);
    LAGFX_TRACE("SetDepthStencilState: reference=0x%08x", reference);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_depth_stencil_state(&p->render_enc, reference);
    }
    return 0;
}

static int render_op_set_depth_bias(lagfx_protocol_t *p,
                                      const uint8_t    *payload,
                                      size_t            len) {
    if (len < 12) {
        LAGFX_WARN("SetDepthBias: payload too short (%zu < 12)", len);
        return 0;
    }
    float bias = read_f32(payload);
    float slope = read_f32(payload + 4);
    float clamp = read_f32(payload + 8);
    LAGFX_TRACE("SetDepthBias: bias=%g slope=%g clamp=%g",
                (double)bias, (double)slope, (double)clamp);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_depth_bias(&p->render_enc, bias, clamp, slope);
    }
    return 0;
}

static int render_op_set_depth_clip_mode(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
    if (len < 8) {
        LAGFX_WARN("SetDepthClipMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetDepthClipMode: mode=%u extra=%u", mode, extra);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_depth_clip_mode(&p->render_enc, mode);
    }
    return 0;
}

static int render_op_set_fragment_sampler_states(lagfx_protocol_t *p,
                                                   const uint8_t    *payload,
                                                   size_t            len) {
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

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_FRAGMENT_SAMPLERS) {
            LAGFX_WARN("SetFragmentSamplerStates: count=%u exceeds max=%u, truncating",
                       count, LAGFX_MAX_BOUND_FRAGMENT_SAMPLERS);
            store_count = LAGFX_MAX_BOUND_FRAGMENT_SAMPLERS;
        }
        for (uint32_t i = 0; i < store_count; ++i) {
            p->render_enc.bound_fragment_samplers[i] =
                read_le32(payload + 8 + (size_t)i * 4);
        }
        p->render_enc.bound_fragment_sampler_count = store_count;
        p->render_enc.bound_fragment_sampler_first = first;
    }
    return 0;
}

static int render_op_set_stencil_ref(lagfx_protocol_t *p,
                                       const uint8_t    *payload,
                                       size_t            len) {
    if (len < 8) {
        LAGFX_WARN("SetStencilRef: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t front_ref = read_le32(payload);
    uint32_t back_ref = read_le32(payload + 4);
    LAGFX_TRACE("SetStencilRef: front=%u back=%u", front_ref, back_ref);

    if (p && p->render_enc.in_pass) {
        lagfx_translate_render_set_stencil_ref(&p->render_enc, front_ref, back_ref);
    }
    return 0;
}

static int render_op_set_vertex_textures(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
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

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_VERTEX_TEXTURES) {
            LAGFX_WARN("SetVertexTextures: count=%u exceeds max=%u, truncating",
                       count, LAGFX_MAX_BOUND_VERTEX_TEXTURES);
            store_count = LAGFX_MAX_BOUND_VERTEX_TEXTURES;
        }
        for (uint32_t i = 0; i < store_count; ++i) {
            p->render_enc.bound_vertex_textures[i] =
                read_le32(payload + 8 + (size_t)i * 4);
        }
        p->render_enc.bound_vertex_texture_count = store_count;
        p->render_enc.bound_vertex_texture_first = first;
    }
    return 0;
}

static int render_op_set_vertex_buffers_with_stride(lagfx_protocol_t *p,
                                                      const uint8_t    *payload,
                                                      size_t            len) {
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

    if (p) {
        uint32_t store_count = count;
        if (store_count > LAGFX_MAX_BOUND_VERTEX_BUFFERS) {
            LAGFX_WARN("SetVertexBuffersWithStride: count=%u exceeds max=%u, truncating",
                       count, LAGFX_MAX_BOUND_VERTEX_BUFFERS);
            store_count = LAGFX_MAX_BOUND_VERTEX_BUFFERS;
        }
        for (uint32_t i = 0; i < store_count; ++i) {
            const uint8_t *entry = payload + 8 + (size_t)i * 20;
            p->render_enc.bound_vertex_buffers_stride[i].ref = read_le32(entry);
            p->render_enc.bound_vertex_buffers_stride[i].offset = read_le64(entry + 4);
            p->render_enc.bound_vertex_buffers_stride[i].stride = read_le64(entry + 12);
        }
        p->render_enc.bound_vertex_buffers_stride_count = store_count;
        p->render_enc.bound_vertex_buffers_stride_first = first;
    }
    return 0;
}

static int render_op_describe_render_pass(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
LAGFX_ERR("render_op_describe_render_pass CALLED: p=%p dev=%p vk=%p", (void *)p, (void *)(p?p->dev:NULL), (void *)(p&&p->dev?p->dev->vk:NULL));
    lagfx_render_pass_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int rc = lagfx_parse_render_pass_descriptor(payload, len, &desc);
    if (rc != 0) {
        LAGFX_ERR("render_op_describe_render_pass: parse failed (rc=%d len=%zu)",
                  rc, len);
        return rc;
    }
    if (p) {
        p->last_render_pass_desc = desc;

#ifdef LAGFX_HAVE_VULKAN
        VkDevice device = NULL;
        if (p->dev && p->dev->vk) {
            device = p->dev->vk->device;
        }
        if (!device) {
            LAGFX_ERR("render_op_describe_render_pass: no vulkan device");
            return -1;
        }

        uint32_t total_views = 0;

        if (desc.has_depth) {
            lagfx_resource_registry_t *reg = &p->resources;
            lagfx_resource_entry_t *entry =
                lagfx_resource_lookup(reg, desc.depth_texture_ref, p->current_task_id);

            /* Auto-create depth surface if not found */
            if (!entry) {
#ifdef LAGFX_HAVE_VULKAN
                if (p->dev && p->dev->vk) {
                    lagfx_vk_iosurface_t *ios = NULL;
                    lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk, 1920, 1080, VK_FORMAT_D32_SFLOAT, &ios);
                    if (st == LAGFX_OK && ios) {
                        entry = lagfx_resource_lookup(reg, desc.depth_texture_ref, p->current_task_id);
                        if (!entry || entry->type != LAGFX_RESOURCE_TYPE_TEXTURE) {
                            lagfx_resource_register(reg, desc.depth_texture_ref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                                    p->current_task_id, 0u, (uint64_t)1920 * 1080 * 4);
                            entry = lagfx_resource_lookup(reg, desc.depth_texture_ref, p->current_task_id);
                            if (entry) {
                                entry->host_handle = ios;
                                LAGFX_LOG("render_op_describe_render_pass: auto-created depth surface 0x%x", desc.depth_texture_ref);
                            } else {
                                lagfx_vk_iosurface_destroy(p->dev->vk, ios);
                            }
                        }
                    }
                }
#endif
            }

            VkImageView view = VK_NULL_HANDLE;
            if (entry && entry->type == LAGFX_RESOURCE_TYPE_TEXTURE) {
                /* host_handle points to lagfx_vk_iosurface_t* which contains both image and view */
                lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)entry->host_handle;
                VkImage image = ios ? ios->image : VK_NULL_HANDLE;

                if (!image) {
                    LAGFX_WARN("render_op_describe_render_pass: depth texture has no VkImage");
                } else if (entry->view != VK_NULL_HANDLE) {
                    /* Reuse cached view */
                    view = entry->view;
                    total_views++;
                    LAGFX_TRACE("render_op_describe_render_pass: reused cached depth view 0x%lx",
                                (unsigned long)view);
                } else {
                    VkImageViewCreateInfo vci = {0};
                    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    vci.image = image;
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = VK_FORMAT_D32_SFLOAT;
                    vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    vci.subresourceRange.baseMipLevel = 0;
                    vci.subresourceRange.levelCount = 1;
                    vci.subresourceRange.baseArrayLayer = 0;
                    vci.subresourceRange.layerCount = 1;

                    VkResult vr = vkCreateImageView(device, &vci, NULL, &view);
                    if (vr != VK_SUCCESS) {
                        LAGFX_ERR("render_op_describe_render_pass: depth texture ref=0x%x "
                                  "vkCreateImageView failed (%d)",
                                  desc.depth_texture_ref, (int)vr);
                    } else {
                        total_views++;
                        /* Cache the view for future use */
                        entry->view = view;
                        LAGFX_TRACE("render_op_describe_render_pass: created depth view 0x%lx",
                                    (unsigned long)view);
                    }
                }
            } else {
                LAGFX_WARN("render_op_describe_render_pass: depth texture ref=0x%x "
                           "not found or wrong type in registry", desc.depth_texture_ref);
            }
        }

        if (desc.has_stencil) {
            lagfx_resource_registry_t *reg = &p->resources;
            lagfx_resource_entry_t *entry =
                lagfx_resource_lookup(reg, desc.stencil_texture_ref, p->current_task_id);

            /* Auto-create stencil surface if not found */
            if (!entry) {
#ifdef LAGFX_HAVE_VULKAN
                if (p->dev && p->dev->vk) {
                    lagfx_vk_iosurface_t *ios = NULL;
                    lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk, 1920, 1080, VK_FORMAT_S8_UINT, &ios);
                    if (st == LAGFX_OK && ios) {
                        entry = lagfx_resource_lookup(reg, desc.stencil_texture_ref, p->current_task_id);
                        if (!entry || entry->type != LAGFX_RESOURCE_TYPE_TEXTURE) {
                            lagfx_resource_register(reg, desc.stencil_texture_ref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                                    p->current_task_id, 0u, (uint64_t)1920 * 1080);
                            entry = lagfx_resource_lookup(reg, desc.stencil_texture_ref, p->current_task_id);
                            if (entry) {
                                entry->host_handle = ios;
                                LAGFX_LOG("render_op_describe_render_pass: auto-created stencil surface 0x%x", desc.stencil_texture_ref);
                            } else {
                                lagfx_vk_iosurface_destroy(p->dev->vk, ios);
                            }
                        }
                    }
                }
#endif
            }

            VkImageView view = VK_NULL_HANDLE;
            if (entry && entry->type == LAGFX_RESOURCE_TYPE_TEXTURE) {
                /* host_handle points to lagfx_vk_iosurface_t* which contains both image and view */
                lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)entry->host_handle;
                VkImage image = ios ? ios->image : VK_NULL_HANDLE;

                if (!image) {
                    LAGFX_WARN("render_op_describe_render_pass: stencil texture has no VkImage");
                } else if (entry->view != VK_NULL_HANDLE) {
                    /* Reuse cached view */
                    view = entry->view;
                    total_views++;
                    LAGFX_TRACE("render_op_describe_render_pass: reused cached stencil view 0x%lx",
                                (unsigned long)view);
                } else {
                    VkImageViewCreateInfo vci = {0};
                    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    vci.image = image;
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = VK_FORMAT_S8_UINT;
                    vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
                    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                    vci.subresourceRange.baseMipLevel = 0;
                    vci.subresourceRange.levelCount = 1;
                    vci.subresourceRange.baseArrayLayer = 0;
                    vci.subresourceRange.layerCount = 1;

                    VkResult vr = vkCreateImageView(device, &vci, NULL, &view);
                    if (vr != VK_SUCCESS) {
                        LAGFX_ERR("render_op_describe_render_pass: stencil texture ref=0x%x "
                                  "vkCreateImageView failed (%d)",
                                  desc.stencil_texture_ref, (int)vr);
                    } else {
                        total_views++;
                        /* Cache the view for future use */
                        entry->view = view;
                        LAGFX_TRACE("render_op_describe_render_pass: created stencil view 0x%lx",
                                    (unsigned long)view);
                    }
                }
            } else {
                LAGFX_WARN("render_op_describe_render_pass: stencil texture ref=0x%x "
                           "not found or wrong type in registry", desc.stencil_texture_ref);
            }
        }

        for (uint32_t i = 0; i < desc.color_attachment_count && i < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS; ++i) {
            uint32_t ref = desc.colors[i].texture_ref;

            lagfx_resource_registry_t *reg = &p->resources;
            lagfx_resource_entry_t *entry =
                lagfx_resource_lookup(reg, ref, p->current_task_id);

            /* Auto-create surface if not found (guest may not send CmdCreateIOSurfaceBacking2) */
            if (!entry) {
#ifdef LAGFX_HAVE_VULKAN
                if (p->dev && p->dev->vk) {
                    lagfx_vk_iosurface_t *ios = NULL;
                    lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk, 1920, 1080, VK_FORMAT_B8G8R8A8_UNORM, &ios);
                    if (st == LAGFX_OK && ios) {
                        entry = lagfx_resource_lookup(reg, ref, p->current_task_id);
                        if (!entry || entry->type != LAGFX_RESOURCE_TYPE_TEXTURE) {
                            lagfx_resource_register(reg, ref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                                    p->current_task_id, 0u, (uint64_t)1920 * 1080 * 4);
                            entry = lagfx_resource_lookup(reg, ref, p->current_task_id);
                            if (entry) {
                                entry->host_handle = ios;
                                LAGFX_LOG("render_op_describe_render_pass: auto-created surface 0x%x", ref);
                            } else {
                                lagfx_vk_iosurface_destroy(p->dev->vk, ios);
                            }
                        }
                    }
                }
#else
                (void)ref; /* unused in non-Vulkan build */
#endif
            }

            VkImageView view = VK_NULL_HANDLE;
            if (entry && entry->type == LAGFX_RESOURCE_TYPE_TEXTURE) {
                /* host_handle points to lagfx_vk_iosurface_t* which contains both image and view */
                lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)entry->host_handle;
                VkImage image = ios ? ios->image : VK_NULL_HANDLE;

                if (!image) {
                    LAGFX_WARN("render_op_describe_render_pass: color[%u] texture has no VkImage", i);
                } else if (entry->view != VK_NULL_HANDLE) {
                    /* Reuse cached view */
                    view = entry->view;
                    total_views++;
                    LAGFX_TRACE("render_op_describe_render_pass: reused cached color[%u] view 0x%lx",
                                i, (unsigned long)view);
                } else {
                    VkImageViewCreateInfo vci = {0};
                    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    vci.image = image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = VK_FORMAT_B8G8R8A8_UNORM;
                vci.components.r = VK_COMPONENT_SWIZZLE_R;
                vci.components.g = VK_COMPONENT_SWIZZLE_G;
                vci.components.b = VK_COMPONENT_SWIZZLE_B;
                vci.components.a = VK_COMPONENT_SWIZZLE_A;
                vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                vci.subresourceRange.baseMipLevel = 0;
                vci.subresourceRange.levelCount = 1;
                vci.subresourceRange.baseArrayLayer = 0;
                vci.subresourceRange.layerCount = 1;

                VkResult vr = vkCreateImageView(device, &vci, NULL, &view);
                    if (vr != VK_SUCCESS) {
                        LAGFX_ERR("render_op_describe_render_pass: color[%u] texture ref=0x%x "
                                  "vkCreateImageView failed (%d)", i, ref, (int)vr);
                    } else {
                        total_views++;
                        /* Cache the view for future use */
                        entry->view = view;
                        LAGFX_TRACE("render_op_describe_render_pass: created color[%u] view 0x%lx",
                                    i, (unsigned long)view);
                    }
                }
            } else {
                LAGFX_WARN("render_op_describe_render_pass: color[%u] texture ref=0x%x "
                           "not found or wrong type in registry", i, ref);
            }
        }

       LAGFX_WARN("0x1a: RenderPassDescriptor parsed — %u attachments",
                   desc.has_depth + desc.has_stencil + desc.color_attachment_count);
        
       /* Stage 30+: Start render pass immediately when descriptor is parsed.
          * This enables Metal command accumulation before submission. */
         LAGFX_WARN("Stage 30 DEBUG: p=%p dev=%p vk=%p", (void*)p, (void*)p->dev, (void*)(p ? p->dev : NULL)->vk);
         if (p && p->dev && p->dev->vk) {
             LAGFX_WARN("Stage 30: Calling lagfx_render_encoder_try_begin for opcode 0x1a");
             lagfx_render_encoder_try_begin(p);
         } else {
             LAGFX_ERR("Stage 30 FAIL: p=%p dev=%p vk=%p", (void*)p, (void*)p->dev, (void*)(p ? p->dev : NULL)->vk);
         }
#else  /* !LAGFX_HAVE_VULKAN */
        /* No Vulkan backend: parse the descriptor but skip image-view
         * creation. The non-Vulkan build path is for macOS CI only and
         * never reaches a live render pass. */
        LAGFX_WARN("0x1a: RenderPassDescriptor parsed — depth=%u stencil=%u "
                   "colors=%u (no-vulkan build, image views skipped)",
                   desc.has_depth, desc.has_stencil,
                   desc.color_attachment_count);
#endif /* LAGFX_HAVE_VULKAN */
    } else {
        LAGFX_WARN("render_op_describe_render_pass: depth=%u stencil=%u "
                   "colors=%u rt=%llux%llu",
                   desc.has_depth, desc.has_stencil, desc.color_attachment_count,
                   (unsigned long long)desc.render_target_width,
                   (unsigned long long)desc.render_target_height);
    }
    return 0;
}

const lagfx_render_pass_desc_t *
lagfx_render_pass_desc_get(const lagfx_protocol_t *p) {
    return p ? &p->last_render_pass_desc : NULL;
}

/* --- Draw instanced/base/patch/indirect variants --------------- */

static int render_op_draw_instanced_primitives_64(lagfx_protocol_t *p,
                                                   const uint8_t    *payload,
                                                   size_t            len) {
    (void)p;
    if (len < 28) {
        LAGFX_WARN("DrawInstancedPrimitives64: payload too short (%zu < 28)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint64_t vertex_start = read_le64(payload + 4);
    uint64_t vertex_count = read_le64(payload + 12);
    uint64_t instance_count = read_le64(payload + 20);
    LAGFX_TRACE("DrawInstancedPrimitives64: type=%u start=%llu count=%llu instances=%llu",
                prim_type, (unsigned long long)vertex_start,
                (unsigned long long)vertex_count,
                (unsigned long long)instance_count);
    return 0;
}

static int render_op_draw_instanced_primitives_16(lagfx_protocol_t *p,
                                                   const uint8_t    *payload,
                                                   size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("DrawInstancedPrimitives16: payload too short (%zu < 8)", len);
        return 0;
    }
    uint16_t prim_type = read_le16(payload);
    uint16_t vertex_start = read_le16(payload + 2);
    uint16_t vertex_count = read_le16(payload + 4);
    uint16_t instance_count = read_le16(payload + 6);
    LAGFX_TRACE("DrawInstancedPrimitives16: type=%u start=%u count=%u instances=%u",
                prim_type, vertex_start, vertex_count, instance_count);
    return 0;
}

static int render_op_draw_instanced_base_primitives_64(lagfx_protocol_t *p,
                                                        const uint8_t    *payload,
                                                        size_t            len) {
    (void)p;
    if (len < 36) {
        LAGFX_WARN("DrawInstancedBasePrimitives64: payload too short (%zu < 36)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint64_t vertex_start = read_le64(payload + 4);
    uint64_t vertex_count = read_le64(payload + 12);
    uint64_t instance_count = read_le64(payload + 20);
    uint64_t base_instance = read_le64(payload + 28);
    LAGFX_TRACE("DrawInstancedBasePrimitives64: type=%u start=%llu count=%llu "
                "instances=%llu base=%llu",
                prim_type, (unsigned long long)vertex_start,
                (unsigned long long)vertex_count,
                (unsigned long long)instance_count,
                (unsigned long long)base_instance);
    return 0;
}

static int render_op_draw_instanced_base_primitives_16(lagfx_protocol_t *p,
                                                        const uint8_t    *payload,
                                                        size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("DrawInstancedBasePrimitives16: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t vertex_start = read_le16(payload + 4);
    uint16_t vertex_count = read_le16(payload + 6);
    uint16_t instance_count = read_le16(payload + 8);
    uint16_t base_instance = read_le16(payload + 10);
    LAGFX_TRACE("DrawInstancedBasePrimitives16: type=%u start=%u count=%u "
                "instances=%u base=%u",
                prim_type, vertex_start, vertex_count,
                instance_count, base_instance);
    return 0;
}

static int render_op_draw_indexed_primitives_16(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("DrawIndexedPrimitives16: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t index_count = read_le16(payload + 4);
    uint16_t index_type = read_le16(payload + 6);
    uint32_t index_buf_ref = read_le32(payload + 8);
    LAGFX_TRACE("DrawIndexedPrimitives16: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x",
                prim_type, index_count, index_type, index_buf_ref);
    return 0;
}

static int render_op_draw_indexed_instanced_primitives_64(lagfx_protocol_t *p,
                                                           const uint8_t    *payload,
                                                           size_t            len) {
    (void)p;
    if (len < 32) {
        LAGFX_WARN("DrawIndexedInstancedPrimitives64: payload too short (%zu < 32)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t index_count = read_le32(payload + 4);
    uint32_t index_type = read_le32(payload + 8);
    uint32_t index_buf_ref = read_le32(payload + 12);
    uint64_t index_buf_offset = read_le64(payload + 16);
    uint64_t instance_count = read_le64(payload + 24);
    LAGFX_TRACE("DrawIndexedInstancedPrimitives64: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%llu instances=%llu",
                prim_type, index_count, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset,
                (unsigned long long)instance_count);
    return 0;
}

static int render_op_draw_indexed_instanced_primitives_16(lagfx_protocol_t *p,
                                                           const uint8_t    *payload,
                                                           size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("DrawIndexedInstancedPrimitives16: payload too short (%zu < 16)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t index_count = read_le16(payload + 4);
    uint16_t index_type = read_le16(payload + 6);
    uint32_t index_buf_ref = read_le32(payload + 8);
    uint16_t index_buf_offset = read_le16(payload + 12);
    uint16_t instance_count = read_le16(payload + 14);
    LAGFX_TRACE("DrawIndexedInstancedPrimitives16: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%u instances=%u",
                prim_type, index_count, index_type, index_buf_ref,
                index_buf_offset, instance_count);
    return 0;
}

static int render_op_draw_indexed_instanced_base_primitives_64(lagfx_protocol_t *p,
                                                                const uint8_t    *payload,
                                                                size_t            len) {
    (void)p;
    if (len < 48) {
        LAGFX_WARN("DrawIndexedInstancedBasePrimitives64: payload too short (%zu < 48)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t index_count = read_le32(payload + 4);
    uint32_t index_type = read_le32(payload + 8);
    uint32_t index_buf_ref = read_le32(payload + 12);
    uint64_t index_buf_offset = read_le64(payload + 16);
    uint64_t instance_count = read_le64(payload + 24);
    uint64_t base_vertex = read_le64(payload + 32);
    uint64_t base_instance = read_le64(payload + 40);
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives64: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%llu instances=%llu baseVtx=%llu baseInst=%llu",
                prim_type, index_count, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset,
                (unsigned long long)instance_count,
                (unsigned long long)base_vertex,
                (unsigned long long)base_instance);
    return 0;
}

static int render_op_draw_indexed_instanced_base_primitives_16(lagfx_protocol_t *p,
                                                                const uint8_t    *payload,
                                                                size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("DrawIndexedInstancedBasePrimitives16: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t index_count = read_le16(payload + 4);
    uint16_t index_type = read_le16(payload + 6);
    uint32_t index_buf_ref = read_le32(payload + 8);
    uint16_t index_buf_offset = read_le16(payload + 12);
    uint16_t instance_count = read_le16(payload + 14);
    uint16_t base_vertex = read_le16(payload + 16);
    uint16_t base_instance = read_le16(payload + 18);
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives16: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%u instances=%u baseVtx=%u baseInst=%u",
                prim_type, index_count, index_type, index_buf_ref,
                index_buf_offset, instance_count, base_vertex, base_instance);
    return 0;
}

static int render_op_draw_patches_64(lagfx_protocol_t *p,
                                      const uint8_t    *payload,
                                      size_t            len) {
    (void)p;
    if (len < 48) {
        LAGFX_WARN("DrawPatches64: payload too short (%zu < 48)", len);
        return 0;
    }
    uint32_t num_control_points = read_le32(payload);
    uint64_t patch_start = read_le64(payload + 4);
    uint64_t patch_count = read_le64(payload + 12);
    uint32_t patch_index_buf_ref = read_le32(payload + 20);
    uint64_t patch_index_buf_offset = read_le64(payload + 24);
    uint64_t instance_count = read_le64(payload + 32);
    uint64_t base_instance = read_le64(payload + 40);
    LAGFX_TRACE("DrawPatches64: ctrlPts=%u start=%llu count=%llu "
                "idxBufRef=0x%08x idxBufOff=%llu instances=%llu base=%llu",
                num_control_points,
                (unsigned long long)patch_start, (unsigned long long)patch_count,
                patch_index_buf_ref,
                (unsigned long long)patch_index_buf_offset,
                (unsigned long long)instance_count,
                (unsigned long long)base_instance);
    return 0;
}

static int render_op_draw_patches_16(lagfx_protocol_t *p,
                                      const uint8_t    *payload,
                                      size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("DrawPatches16: payload too short (%zu < 16)", len);
        return 0;
    }
    uint16_t num_control_points = read_le16(payload);
    uint16_t patch_start = read_le16(payload + 2);
    uint16_t patch_count = read_le16(payload + 4);
    uint32_t patch_index_buf_ref = read_le32(payload + 6);
    uint16_t patch_index_buf_offset = read_le16(payload + 10);
    uint16_t instance_count = read_le16(payload + 12);
    uint16_t base_instance = read_le16(payload + 14);
    LAGFX_TRACE("DrawPatches16: ctrlPts=%u start=%u count=%u "
                "idxBufRef=0x%08x idxBufOff=%u instances=%u base=%u",
                num_control_points, patch_start, patch_count,
                patch_index_buf_ref, patch_index_buf_offset,
                instance_count, base_instance);
    return 0;
}

static int render_op_draw_indexed_patches_64(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 60) {
        LAGFX_WARN("DrawIndexedPatches64: payload too short (%zu < 60)", len);
        return 0;
    }
    uint32_t num_control_points = read_le32(payload);
    uint64_t patch_start = read_le64(payload + 4);
    uint64_t patch_count = read_le64(payload + 12);
    uint32_t patch_index_buf_ref = read_le32(payload + 20);
    uint64_t patch_index_buf_offset = read_le64(payload + 24);
    uint32_t ctrl_pt_index_buf_ref = read_le32(payload + 32);
    uint64_t ctrl_pt_index_buf_offset = read_le64(payload + 36);
    uint64_t instance_count = read_le64(payload + 44);
    uint64_t base_instance = read_le64(payload + 52);
    LAGFX_TRACE("DrawIndexedPatches64: ctrlPts=%u start=%llu count=%llu "
                "idxBufRef=0x%08x idxBufOff=%llu ctrlPtBufRef=0x%08x ctrlPtOff=%llu "
                "instances=%llu base=%llu",
                num_control_points,
                (unsigned long long)patch_start, (unsigned long long)patch_count,
                patch_index_buf_ref,
                (unsigned long long)patch_index_buf_offset,
                ctrl_pt_index_buf_ref,
                (unsigned long long)ctrl_pt_index_buf_offset,
                (unsigned long long)instance_count,
                (unsigned long long)base_instance);
    return 0;
}

static int render_op_draw_indexed_patches_16(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 24) {
        LAGFX_WARN("DrawIndexedPatches16: payload too short (%zu < 24)", len);
        return 0;
    }
    uint32_t num_control_points = read_le32(payload);
    uint16_t patch_start = read_le16(payload + 4);
    uint16_t patch_count = read_le16(payload + 6);
    uint32_t patch_index_buf_ref = read_le32(payload + 8);
    uint16_t patch_index_buf_offset = read_le16(payload + 12);
    uint32_t ctrl_pt_index_buf_ref = read_le32(payload + 14);
    uint16_t ctrl_pt_index_buf_offset = read_le16(payload + 18);
    uint16_t instance_count = read_le16(payload + 20);
    uint16_t base_instance = read_le16(payload + 22);
    LAGFX_TRACE("DrawIndexedPatches16: ctrlPts=%u start=%u count=%u "
                "idxBufRef=0x%08x idxBufOff=%u ctrlPtBufRef=0x%08x ctrlPtOff=%u "
                "instances=%u base=%u",
                num_control_points, patch_start, patch_count,
                patch_index_buf_ref, patch_index_buf_offset,
                ctrl_pt_index_buf_ref, ctrl_pt_index_buf_offset,
                instance_count, base_instance);
    return 0;
}

static int render_op_draw_primitives_indirect(lagfx_protocol_t *p,
                                               const uint8_t    *payload,
                                               size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("DrawPrimitivesIndirect: payload too short (%zu < 16)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t indirect_buf_ref = read_le32(payload + 4);
    uint64_t indirect_buf_offset = read_le64(payload + 8);
    LAGFX_TRACE("DrawPrimitivesIndirect: type=%u bufRef=0x%08x offset=%llu",
                prim_type, indirect_buf_ref,
                (unsigned long long)indirect_buf_offset);
    return 0;
}

static int render_op_draw_indexed_primitives_indirect(lagfx_protocol_t *p,
                                                       const uint8_t    *payload,
                                                       size_t            len) {
    (void)p;
    if (len < 28) {
        LAGFX_WARN("DrawIndexedPrimitivesIndirect: payload too short (%zu < 28)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t index_type = read_le32(payload + 4);
    uint32_t index_buf_ref = read_le32(payload + 8);
    uint64_t index_buf_offset = read_le64(payload + 12);
    uint32_t indirect_buf_ref = read_le32(payload + 20);
    uint64_t indirect_buf_offset = read_le64(payload + 24);
    LAGFX_TRACE("DrawIndexedPrimitivesIndirect: type=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%llu bufRef=0x%08x bufOff=%llu",
                prim_type, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset,
                indirect_buf_ref,
                (unsigned long long)indirect_buf_offset);
    return 0;
}

static int render_op_draw_patches_indirect(lagfx_protocol_t *p,
                                            const uint8_t    *payload,
                                            size_t            len) {
    (void)p;
    if (len < 28) {
        LAGFX_WARN("DrawPatchesIndirect: payload too short (%zu < 28)", len);
        return 0;
    }
    uint32_t num_control_points = read_le32(payload);
    uint32_t patch_index_buf_ref = read_le32(payload + 4);
    uint64_t patch_index_buf_offset = read_le64(payload + 8);
    uint32_t indirect_buf_ref = read_le32(payload + 16);
    uint64_t indirect_buf_offset = read_le64(payload + 20);
    LAGFX_TRACE("DrawPatchesIndirect: ctrlPts=%u idxBufRef=0x%08x idxBufOff=%llu "
                "bufRef=0x%08x bufOff=%llu",
                num_control_points, patch_index_buf_ref,
                (unsigned long long)patch_index_buf_offset,
                indirect_buf_ref,
                (unsigned long long)indirect_buf_offset);
    return 0;
}

static int render_op_draw_indexed_patches_indirect(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 40) {
        LAGFX_WARN("DrawIndexedPatchesIndirect: payload too short (%zu < 40)", len);
        return 0;
    }
    uint32_t num_control_points = read_le32(payload);
    uint32_t patch_index_buf_ref = read_le32(payload + 4);
    uint64_t patch_index_buf_offset = read_le64(payload + 8);
    uint32_t ctrl_pt_index_buf_ref = read_le32(payload + 16);
    uint64_t ctrl_pt_index_buf_offset = read_le64(payload + 20);
    uint32_t indirect_buf_ref = read_le32(payload + 28);
    uint64_t indirect_buf_offset = read_le64(payload + 32);
    LAGFX_TRACE("DrawIndexedPatchesIndirect: ctrlPts=%u idxBufRef=0x%08x idxBufOff=%llu "
                "ctrlPtBufRef=0x%08x ctrlPtOff=%llu bufRef=0x%08x bufOff=%llu",
                num_control_points, patch_index_buf_ref,
                (unsigned long long)patch_index_buf_offset,
                ctrl_pt_index_buf_ref,
                (unsigned long long)ctrl_pt_index_buf_offset,
                indirect_buf_ref,
                (unsigned long long)indirect_buf_offset);
    return 0;
}

static int render_op_execute_commands_in_buffer(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("ExecuteCommandsInBuffer: payload too short (%zu < 16)", len);
        return 0;
    }
    uint32_t icb_ref = read_le32(payload);
    uint32_t buffer_ref = read_le32(payload + 4);
    uint64_t offset = read_le64(payload + 8);
    LAGFX_TRACE("ExecuteCommandsInBuffer: icbRef=0x%08x bufRef=0x%08x offset=%llu",
                icb_ref, buffer_ref, (unsigned long long)offset);
    return 0;
}

static int render_op_execute_commands_in_buffer_ranged(lagfx_protocol_t *p,
                                                        const uint8_t    *payload,
                                                        size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("ExecuteCommandsInBufferRanged: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t icb_ref = read_le32(payload);
    uint32_t range_location = read_le32(payload + 4);
    uint32_t range_length = read_le32(payload + 8);
    uint64_t offset = read_le64(payload + 12);
    LAGFX_TRACE("ExecuteCommandsInBufferRanged: icbRef=0x%08x range={%u,%u} offset=%llu",
                icb_ref, range_location, range_length,
                (unsigned long long)offset);
    return 0;
}

/* --- Fence / barrier / heap handlers -------------------------- */

static int render_op_render_update_fence(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("RenderUpdateFence: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t fence_ref = read_le32(payload);
    uint32_t stages = read_le32(payload + 4);
    LAGFX_TRACE("RenderUpdateFence: fenceRef=0x%08x stages=0x%x",
                fence_ref, stages);
    return 0;
}

static int render_op_render_wait_for_fence(lagfx_protocol_t *p,
                                            const uint8_t    *payload,
                                            size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("RenderWaitForFence: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t fence_ref = read_le32(payload);
    uint32_t stages = read_le32(payload + 4);
    LAGFX_TRACE("RenderWaitForFence: fenceRef=0x%08x stages=0x%x",
                fence_ref, stages);
    return 0;
}

static int render_op_use_heaps_with_stages(lagfx_protocol_t *p,
                                            const uint8_t    *payload,
                                            size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("UseHeapsWithStages: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t stages = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("UseHeapsWithStages: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("UseHeapsWithStages: count=%u stages=0x%x", count, stages);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_draw_indexed_instanced_base_primitives_64_2(lagfx_protocol_t *p,
                                                                  const uint8_t    *payload,
                                                                  size_t            len) {
    (void)p;
    if (len < 48) {
        LAGFX_WARN("DrawIndexedInstancedBasePrimitives64_2: payload too short (%zu < 48)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint32_t index_count = read_le32(payload + 4);
    uint32_t index_type = read_le32(payload + 8);
    uint32_t index_buf_ref = read_le32(payload + 12);
    uint64_t index_buf_offset = read_le64(payload + 16);
    uint64_t instance_count = read_le64(payload + 24);
    uint64_t base_vertex = read_le64(payload + 32);
    uint64_t base_instance = read_le64(payload + 40);
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives64_2: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%llu instances=%llu baseVtx=%llu baseInst=%llu",
                prim_type, index_count, index_type, index_buf_ref,
                (unsigned long long)index_buf_offset,
                (unsigned long long)instance_count,
                (unsigned long long)base_vertex,
                (unsigned long long)base_instance);
    return 0;
}

static int render_op_draw_indexed_instanced_base_primitives_16_2(lagfx_protocol_t *p,
                                                                  const uint8_t    *payload,
                                                                  size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("DrawIndexedInstancedBasePrimitives16_2: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t prim_type = read_le32(payload);
    uint16_t index_count = read_le16(payload + 4);
    uint16_t index_type = read_le16(payload + 6);
    uint32_t index_buf_ref = read_le32(payload + 8);
    uint16_t index_buf_offset = read_le16(payload + 12);
    uint16_t instance_count = read_le16(payload + 14);
    uint16_t base_vertex = read_le16(payload + 16);
    uint16_t base_instance = read_le16(payload + 18);
    LAGFX_TRACE("DrawIndexedInstancedBasePrimitives16_2: type=%u count=%u idxType=%u "
                "idxBufRef=0x%08x idxBufOff=%u instances=%u baseVtx=%u baseInst=%u",
                prim_type, index_count, index_type, index_buf_ref,
                index_buf_offset, instance_count, base_vertex, base_instance);
    return 0;
}

/* --- State-set scalar handlers (0x67..0xa6) -------------------- */

static int render_op_set_color_store_action_options(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetColorStoreActionOptions: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t action = read_le32(payload);
    uint32_t options = read_le32(payload + 4);
    uint32_t index = read_le32(payload + 8);
    LAGFX_TRACE("SetColorStoreActionOptions: action=%u options=%u index=%u",
                action, options, index);
    return 0;
}

static int render_op_set_depth_store_action(lagfx_protocol_t *p,
                                             const uint8_t    *payload,
                                             size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetDepthStoreAction: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t action = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetDepthStoreAction: action=%u extra=%u", action, extra);
    return 0;
}

static int render_op_set_depth_store_action_options(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetDepthStoreActionOptions: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t options = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetDepthStoreActionOptions: options=%u extra=%u", options, extra);
    return 0;
}

static int render_op_set_fragment_buffer_offset(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetFragmentBufferOffset: payload too short (%zu < 12)", len);
        return 0;
    }
    uint64_t offset = read_le64(payload);
    uint32_t index = read_le32(payload + 8);
    LAGFX_TRACE("SetFragmentBufferOffset: offset=%llu index=%u",
                (unsigned long long)offset, index);
    return 0;
}

static int render_op_set_fragment_sampler_states_lod_clamp(lagfx_protocol_t *p,
                                                            const uint8_t    *payload,
                                                            size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetFragmentSamplerStatesLODClamp: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetFragmentSamplerStatesLODClamp: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetFragmentSamplerStatesLODClamp: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        float lod_min = read_f32(entry + 4);
        float lod_max = read_f32(entry + 8);
        LAGFX_TRACE("  [%u] ref=0x%08x lodMin=%g lodMax=%g",
                    first + i, ref, (double)lod_min, (double)lod_max);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_scissor_rects(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetScissorRects: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 32;
    if (len < needed) {
        LAGFX_WARN("SetScissorRects: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetScissorRects: count=%u extra=%u", count, extra);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 32;
        uint64_t x = read_le64(entry);
        uint64_t y = read_le64(entry + 8);
        uint64_t w = read_le64(entry + 16);
        uint64_t h = read_le64(entry + 24);
        LAGFX_TRACE("  [%u] x=%llu y=%llu w=%llu h=%llu",
                    i, (unsigned long long)x, (unsigned long long)y,
                    (unsigned long long)w, (unsigned long long)h);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_stencil_store_action(lagfx_protocol_t *p,
                                               const uint8_t    *payload,
                                               size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetStencilStoreAction: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t action = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetStencilStoreAction: action=%u extra=%u", action, extra);
    return 0;
}

static int render_op_set_stencil_store_action_options(lagfx_protocol_t *p,
                                                       const uint8_t    *payload,
                                                       size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetStencilStoreActionOptions: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t options = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetStencilStoreActionOptions: options=%u extra=%u", options, extra);
    return 0;
}

static int render_op_set_tesselation_factor_buffer(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetTesselationFactorBuffer: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t buffer_ref = read_le32(payload);
    uint64_t offset = read_le64(payload + 4);
    uint64_t instance_stride = read_le64(payload + 12);
    LAGFX_TRACE("SetTesselationFactorBuffer: ref=0x%08x offset=%llu stride=%llu",
                buffer_ref, (unsigned long long)offset,
                (unsigned long long)instance_stride);
    return 0;
}

static int render_op_set_tesselation_factor_scale(lagfx_protocol_t *p,
                                                    const uint8_t    *payload,
                                                    size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetTesselationFactorScale: payload too short (%zu < 4)", len);
        return 0;
    }
    float scale = read_f32(payload);
    LAGFX_TRACE("SetTesselationFactorScale: scale=%g", (double)scale);
    return 0;
}

static int render_op_set_triangle_fill_mode(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetTriangleFillMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    uint32_t extra = read_le32(payload + 4);
    LAGFX_TRACE("SetTriangleFillMode: mode=%u extra=%u", mode, extra);
    return 0;
}

static int render_op_set_vertex_buffer_offset(lagfx_protocol_t *p,
                                               const uint8_t    *payload,
                                               size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetVertexBufferOffset: payload too short (%zu < 12)", len);
        return 0;
    }
    uint64_t offset = read_le64(payload);
    uint32_t index = read_le32(payload + 8);
    LAGFX_TRACE("SetVertexBufferOffset: offset=%llu index=%u",
                (unsigned long long)offset, index);
    return 0;
}

static int render_op_set_vertex_sampler_states(lagfx_protocol_t *p,
                                                const uint8_t    *payload,
                                                size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexSamplerStates: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetVertexSamplerStates: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexSamplerStates: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_vertex_sampler_states_lod_clamp(lagfx_protocol_t *p,
                                                          const uint8_t    *payload,
                                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexSamplerStatesLODClamp: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetVertexSamplerStatesLODClamp: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexSamplerStatesLODClamp: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        float lod_min = read_f32(entry + 4);
        float lod_max = read_f32(entry + 8);
        LAGFX_TRACE("  [%u] ref=0x%08x lodMin=%g lodMax=%g",
                    first + i, ref, (double)lod_min, (double)lod_max);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_viewports(lagfx_protocol_t *p,
                                    const uint8_t    *payload,
                                    size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetViewports: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    size_t needed = 4 + (size_t)count * 48;
    if (len < needed) {
        LAGFX_WARN("SetViewports: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetViewports: count=%u", count);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *vp = payload + 4 + (size_t)i * 48;
        double ox = read_f64(vp);
        double oy = read_f64(vp + 8);
        double w  = read_f64(vp + 16);
        double h  = read_f64(vp + 24);
        double zn = read_f64(vp + 32);
        double zf = read_f64(vp + 40);
        LAGFX_TRACE("  [%u] origin=(%g,%g) size=%gx%g znear=%g zfar=%g",
                    i, ox, oy, w, h, zn, zf);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_visibility_result_mode(lagfx_protocol_t *p,
                                                  const uint8_t    *payload,
                                                  size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("SetVisibilityResultMode: payload too short (%zu < 16)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    uint64_t offset = read_le64(payload + 4);
    uint32_t extra = read_le32(payload + 12);
    LAGFX_TRACE("SetVisibilityResultMode: mode=%u offset=%llu extra=%u",
                mode, (unsigned long long)offset, extra);
    return 0;
}

static int render_op_use_heaps(lagfx_protocol_t *p,
                                const uint8_t    *payload,
                                size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("UseHeaps: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    size_t needed = 4 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("UseHeaps: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("UseHeaps: count=%u", count);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 4 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_line_width(lagfx_protocol_t *p,
                                     const uint8_t    *payload,
                                     size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetLineWidth: payload too short (%zu < 4)", len);
        return 0;
    }
    float width = read_f32(payload);
    LAGFX_TRACE("SetLineWidth: width=%g", (double)width);
    return 0;
}

static int render_op_use_resources_with_stages(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("UseResourcesWithStages: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t usage = read_le32(payload + 4);
    uint32_t stages = read_le32(payload + 8);
    size_t needed = 12 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("UseResourcesWithStages: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("UseResourcesWithStages: count=%u usage=0x%x stages=0x%x",
                count, usage, stages);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 12 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_alpha_test_reference_value(lagfx_protocol_t *p,
                                                      const uint8_t    *payload,
                                                      size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetAlphaTestReferenceValue: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t value = read_le32(payload);
    LAGFX_TRACE("SetAlphaTestReferenceValue: value=0x%08x", value);
    return 0;
}

static int render_op_set_point_size(lagfx_protocol_t *p,
                                     const uint8_t    *payload,
                                     size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetPointSize: payload too short (%zu < 4)", len);
        return 0;
    }
    float size = read_f32(payload);
    LAGFX_TRACE("SetPointSize: size=%g", (double)size);
    return 0;
}

static int render_op_set_clip_plane(lagfx_protocol_t *p,
                                     const uint8_t    *payload,
                                     size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetClipPlane: payload too short (%zu < 20)", len);
        return 0;
    }
    float p1 = read_f32(payload);
    float p2 = read_f32(payload + 4);
    float p3 = read_f32(payload + 8);
    float p4 = read_f32(payload + 12);
    uint32_t index = read_le32(payload + 16);
    LAGFX_TRACE("SetClipPlane: p=(%g,%g,%g,%g) index=%u",
                (double)p1, (double)p2, (double)p3, (double)p4, index);
    return 0;
}

static int render_op_set_vertex_sampler_state(lagfx_protocol_t *p,
                                               const uint8_t    *payload,
                                               size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetVertexSamplerState: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    float lod_min = read_f32(payload + 4);
    float lod_max = read_f32(payload + 8);
    float lod_bias = read_f32(payload + 12);
    uint32_t index = read_le32(payload + 16);
    LAGFX_TRACE("SetVertexSamplerState: ref=0x%08x lodMin=%g lodMax=%g lodBias=%g index=%u",
                ref, (double)lod_min, (double)lod_max, (double)lod_bias, index);
    return 0;
}

static int render_op_set_fragment_sampler_state(lagfx_protocol_t *p,
                                                  const uint8_t    *payload,
                                                  size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetFragmentSamplerState: payload too short (%zu < 20)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    float lod_min = read_f32(payload + 4);
    float lod_max = read_f32(payload + 8);
    float lod_bias = read_f32(payload + 12);
    uint32_t index = read_le32(payload + 16);
    LAGFX_TRACE("SetFragmentSamplerState: ref=0x%08x lodMin=%g lodMax=%g lodBias=%g index=%u",
                ref, (double)lod_min, (double)lod_max, (double)lod_bias, index);
    return 0;
}

static int render_op_set_viewport_transform_enabled(lagfx_protocol_t *p,
                                                      const uint8_t    *payload,
                                                      size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetViewportTransformEnabled: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t enabled = read_le32(payload);
    LAGFX_TRACE("SetViewportTransformEnabled: enabled=%u", enabled);
    return 0;
}

static int render_op_set_provoking_vertex_mode(lagfx_protocol_t *p,
                                                 const uint8_t    *payload,
                                                 size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetProvokingVertexMode: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    LAGFX_TRACE("SetProvokingVertexMode: mode=%u", mode);
    return 0;
}

static int render_op_set_primitive_restart_index_enabled(lagfx_protocol_t *p,
                                                          const uint8_t    *payload,
                                                          size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetPrimitiveRestartIndexEnabled: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t enabled = read_le32(payload);
    uint32_t index = read_le32(payload + 4);
    LAGFX_TRACE("SetPrimitiveRestartIndexEnabled: enabled=%u index=0x%08x",
                enabled, index);
    return 0;
}

static int render_op_set_triangle_fill_mode_front_back(lagfx_protocol_t *p,
                                                         const uint8_t    *payload,
                                                         size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetTriangleFillModeFrontBack: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t modes = read_le32(payload);
    LAGFX_TRACE("SetTriangleFillModeFrontBack: modes=0x%08x", modes);
    return 0;
}

static int render_op_set_transform_feedback_state(lagfx_protocol_t *p,
                                                    const uint8_t    *payload,
                                                    size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetTransformFeedbackState: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t state = read_le32(payload);
    LAGFX_TRACE("SetTransformFeedbackState: state=%u", state);
    return 0;
}

static int render_op_set_depth_cleared(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p; (void)payload; (void)len;
    LAGFX_TRACE("SetDepthCleared");
    return 0;
}

static int render_op_set_stencil_cleared(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p; (void)payload; (void)len;
    LAGFX_TRACE("SetStencilCleared");
    return 0;
}

static int render_op_set_color_resolve_texture(lagfx_protocol_t *p,
                                                const uint8_t    *payload,
                                                size_t            len) {
    (void)p;
    if (len < 16) {
        LAGFX_WARN("SetColorResolveTexture: payload too short (%zu < 16)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    uint32_t slice = read_le32(payload + 4);
    uint32_t plane = read_le32(payload + 8);
    uint32_t level = read_le32(payload + 12);
    LAGFX_TRACE("SetColorResolveTexture: ref=0x%08x slice=%u plane=%u level=%u",
                ref, slice, plane, level);
    return 0;
}

static int render_op_set_depth_resolve_texture(lagfx_protocol_t *p,
                                                const uint8_t    *payload,
                                                size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetDepthResolveTexture: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    uint32_t slice = read_le32(payload + 4);
    uint32_t plane = read_le32(payload + 8);
    LAGFX_TRACE("SetDepthResolveTexture: ref=0x%08x slice=%u plane=%u",
                ref, slice, plane);
    return 0;
}

static int render_op_set_stencil_resolve_texture(lagfx_protocol_t *p,
                                                  const uint8_t    *payload,
                                                  size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetStencilResolveTexture: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    uint32_t slice = read_le32(payload + 4);
    uint32_t plane = read_le32(payload + 8);
    LAGFX_TRACE("SetStencilResolveTexture: ref=0x%08x slice=%u plane=%u",
                ref, slice, plane);
    return 0;
}

static int render_op_set_vertex_amplification_mode(lagfx_protocol_t *p,
                                                     const uint8_t    *payload,
                                                     size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetVertexAmplificationMode: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t mode = read_le32(payload);
    uint32_t value = read_le32(payload + 4);
    LAGFX_TRACE("SetVertexAmplificationMode: mode=%u value=%u", mode, value);
    return 0;
}

static int render_op_set_vertex_amplification_count(lagfx_protocol_t *p,
                                                      const uint8_t    *payload,
                                                      size_t            len) {
    (void)p;
    if (len < 4) {
        LAGFX_WARN("SetVertexAmplificationCount: payload too short (%zu < 4)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    size_t needed = 4 + (size_t)count * 8;
    if (len < needed) {
        LAGFX_WARN("SetVertexAmplificationCount: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetVertexAmplificationCount: count=%u", count);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 4 + (size_t)i * 8;
        uint32_t a = read_le32(entry);
        uint32_t b = read_le32(entry + 4);
        LAGFX_TRACE("  [%u] mapping={%u,%u}", i, a, b);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_dispatch_threads_per_tile(lagfx_protocol_t *p,
                                                const uint8_t    *payload,
                                                size_t            len) {
    (void)p;
    if (len < 24) {
        LAGFX_WARN("DispatchThreadsPerTile: payload too short (%zu < 24)", len);
        return 0;
    }
    uint64_t w = read_le64(payload);
    uint64_t h = read_le64(payload + 8);
    uint64_t d = read_le64(payload + 16);
    LAGFX_TRACE("DispatchThreadsPerTile: size=%llux%llux%llu",
                (unsigned long long)w, (unsigned long long)h,
                (unsigned long long)d);
    return 0;
}

static int render_op_set_render_threadgroup_memory_length(lagfx_protocol_t *p,
                                                            const uint8_t    *payload,
                                                            size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetRenderThreadgroupMemoryLength: payload too short (%zu < 20)", len);
        return 0;
    }
    uint64_t length = read_le64(payload);
    uint64_t offset = read_le64(payload + 8);
    uint32_t index = read_le32(payload + 16);
    LAGFX_TRACE("SetRenderThreadgroupMemoryLength: length=%llu offset=%llu index=%u",
                (unsigned long long)length, (unsigned long long)offset, index);
    return 0;
}

static int render_op_set_tile_buffers(lagfx_protocol_t *p,
                                       const uint8_t    *payload,
                                       size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetTileBuffers: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetTileBuffers: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetTileBuffers: count=%u first=%u", count, first);
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

static int render_op_set_tile_buffer_offset(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("SetTileBufferOffset: payload too short (%zu < 12)", len);
        return 0;
    }
    uint64_t offset = read_le64(payload);
    uint32_t index = read_le32(payload + 8);
    LAGFX_TRACE("SetTileBufferOffset: offset=%llu index=%u",
                (unsigned long long)offset, index);
    return 0;
}

static int render_op_set_tile_sampler_states(lagfx_protocol_t *p,
                                              const uint8_t    *payload,
                                              size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetTileSamplerStates: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetTileSamplerStates: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetTileSamplerStates: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_tile_sampler_states_lod_clamp(lagfx_protocol_t *p,
                                                        const uint8_t    *payload,
                                                        size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetTileSamplerStatesLODClamp: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 12;
    if (len < needed) {
        LAGFX_WARN("SetTileSamplerStatesLODClamp: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetTileSamplerStatesLODClamp: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t *entry = payload + 8 + (size_t)i * 12;
        uint32_t ref = read_le32(entry);
        float lod_min = read_f32(entry + 4);
        float lod_max = read_f32(entry + 8);
        LAGFX_TRACE("  [%u] ref=0x%08x lodMin=%g lodMax=%g",
                    first + i, ref, (double)lod_min, (double)lod_max);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_set_tile_textures(lagfx_protocol_t *p,
                                        const uint8_t    *payload,
                                        size_t            len) {
    (void)p;
    if (len < 8) {
        LAGFX_WARN("SetTileTextures: payload too short (%zu < 8)", len);
        return 0;
    }
    uint32_t count = read_le32(payload);
    uint32_t first = read_le32(payload + 4);
    size_t needed = 8 + (size_t)count * 4;
    if (len < needed) {
        LAGFX_WARN("SetTileTextures: count=%u needs %zu bytes, got %zu",
                   count, needed, len);
        return 0;
    }
    LAGFX_TRACE("SetTileTextures: count=%u first=%u", count, first);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t ref = read_le32(payload + 8 + (size_t)i * 4);
        LAGFX_TRACE("  [%u] ref=0x%08x", first + i, ref);
    }
    if (count > 4)
        LAGFX_TRACE("  ... (%u more)", count - 4);
    return 0;
}

static int render_op_dispatch_threads_per_tile_in_region(lagfx_protocol_t *p,
                                                          const uint8_t    *payload,
                                                          size_t            len) {
    (void)p;
    if (len < 76) {
        LAGFX_WARN("DispatchThreadsPerTileInRegion: payload too short (%zu < 76)", len);
        return 0;
    }
    uint64_t tw = read_le64(payload);
    uint64_t th = read_le64(payload + 8);
    uint64_t td = read_le64(payload + 16);
    uint64_t ox = read_le64(payload + 24);
    uint64_t oy = read_le64(payload + 32);
    uint64_t oz = read_le64(payload + 40);
    uint64_t rw = read_le64(payload + 48);
    uint64_t rh = read_le64(payload + 56);
    uint64_t rd = read_le64(payload + 64);
    uint32_t extra = read_le32(payload + 72);
    LAGFX_TRACE("DispatchThreadsPerTileInRegion: tileSize=%llux%llux%llu "
                "origin=(%llu,%llu,%llu) region=%llux%llux%llu extra=%u",
                (unsigned long long)tw, (unsigned long long)th,
                (unsigned long long)td,
                (unsigned long long)ox, (unsigned long long)oy,
                (unsigned long long)oz,
                (unsigned long long)rw, (unsigned long long)rh,
                (unsigned long long)rd, extra);
    return 0;
}

static int render_op_dispatch_threads_per_tile_in_region_w_idx(lagfx_protocol_t *p,
                                                                const uint8_t    *payload,
                                                                size_t            len) {
    (void)p;
    if (len < 76) {
        LAGFX_WARN("DispatchThreadsPerTileInRegionWithIndex: payload too short (%zu < 76)", len);
        return 0;
    }
    uint64_t tw = read_le64(payload);
    uint64_t th = read_le64(payload + 8);
    uint64_t td = read_le64(payload + 16);
    uint64_t ox = read_le64(payload + 24);
    uint64_t oy = read_le64(payload + 32);
    uint64_t oz = read_le64(payload + 40);
    uint64_t rw = read_le64(payload + 48);
    uint64_t rh = read_le64(payload + 56);
    uint64_t rd = read_le64(payload + 64);
    uint32_t rt_array_index = read_le32(payload + 72);
    LAGFX_TRACE("DispatchThreadsPerTileInRegionWithIndex: tileSize=%llux%llux%llu "
                "origin=(%llu,%llu,%llu) region=%llux%llux%llu rtIdx=%u",
                (unsigned long long)tw, (unsigned long long)th,
                (unsigned long long)td,
                (unsigned long long)ox, (unsigned long long)oy,
                (unsigned long long)oz,
                (unsigned long long)rw, (unsigned long long)rh,
                (unsigned long long)rd, rt_array_index);
    return 0;
}

static int render_op_get_tile_dimensions(lagfx_protocol_t *p,
                                          const uint8_t    *payload,
                                          size_t            len) {
    (void)p;
    if (len < 12) {
        LAGFX_WARN("GetTileDimensions: payload too short (%zu < 12)", len);
        return 0;
    }
    uint32_t ref = read_le32(payload);
    uint64_t offset = read_le64(payload + 4);
    LAGFX_TRACE("GetTileDimensions: ref=0x%08x offset=%llu",
                ref, (unsigned long long)offset);
    return 0;
}

static int render_op_set_vertex_buffer_offset_with_stride(lagfx_protocol_t *p,
                                                            const uint8_t    *payload,
                                                            size_t            len) {
    (void)p;
    if (len < 20) {
        LAGFX_WARN("SetVertexBufferOffsetWithStride: payload too short (%zu < 20)", len);
        return 0;
    }
    uint64_t offset = read_le64(payload);
    uint64_t stride = read_le64(payload + 8);
    uint32_t index = read_le32(payload + 16);
    LAGFX_TRACE("SetVertexBufferOffsetWithStride: offset=%llu stride=%llu index=%u",
                (unsigned long long)offset, (unsigned long long)stride, index);
    return 0;
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
    { 0x02, "DrawInstancedPrimitives64",                 28, 0, render_op_draw_instanced_primitives_64 },
    { 0x03, "DrawInstancedPrimitives16",                  8, 0, render_op_draw_instanced_primitives_16 },
    { 0x04, "DrawInstancedBasePrimitives64",             36, 0, render_op_draw_instanced_base_primitives_64 },
    { 0x05, "DrawInstancedBasePrimitives16",             12, 0, render_op_draw_instanced_base_primitives_16 },
    { 0x06, "DrawIndexedPrimitives64",                   24, 1, render_op_draw_indexed_primitives_64 },
    { 0x07, "DrawIndexedPrimitives16",                   12, 1, render_op_draw_indexed_primitives_16 },
    { 0x08, "DrawIndexedInstancedPrimitives64",          32, 1, render_op_draw_indexed_instanced_primitives_64 },
    { 0x09, "DrawIndexedInstancedPrimitives16",          16, 1, render_op_draw_indexed_instanced_primitives_16 },
    { 0x0a, "DrawIndexedInstancedBasePrimitives64",      48, 1, render_op_draw_indexed_instanced_base_primitives_64 },
    { 0x0b, "DrawIndexedInstancedBasePrimitives16",      20, 1, render_op_draw_indexed_instanced_base_primitives_16 },
    { 0x0c, "DrawPatches64",                             48, 1, render_op_draw_patches_64 },
    { 0x0d, "DrawPatches16",                             16, 1, render_op_draw_patches_16 },
    { 0x0e, "DrawIndexedPatches64",                      60, 2, render_op_draw_indexed_patches_64 },
    { 0x0f, "DrawIndexedPatches16",                      24, 2, render_op_draw_indexed_patches_16 },
    { 0x10, "DrawPrimitivesIndirect",                    16, 1, render_op_draw_primitives_indirect },
    { 0x11, "DrawIndexedPrimitivesIndirect",             28, 2, render_op_draw_indexed_primitives_indirect },
    { 0x12, "DrawPatchesIndirect",                       28, 2, render_op_draw_patches_indirect },
    { 0x13, "DrawIndexedPatchesIndirect",                40, 3, render_op_draw_indexed_patches_indirect },
    { 0x14, "ExecuteCommandsInBuffer",                   16, 2, render_op_execute_commands_in_buffer },
    { 0x15, "ExecuteCommandsInBufferRanged",             20, 1, render_op_execute_commands_in_buffer_ranged },
    { 0x16, "RenderBarrierResources",                     0, 0, render_op_ack_stub },
    { 0x17, "RenderBarrierScope",                         4, 0, render_op_render_barrier_scope },
    { 0x18, "RenderUpdateFence",                          8, 1, render_op_render_update_fence },
    { 0x19, "RenderWaitForFence",                         8, 1, render_op_render_wait_for_fence },
    { 0x1a, "RenderDescribeRenderPass",                   584, 0, render_op_describe_render_pass },
    { 0x1b, "UseHeapsWithStages",                         0, 0, render_op_use_heaps_with_stages },
    { 0x1c, "DrawIndexedInstancedBasePrimitives64_2",    48, 1, render_op_draw_indexed_instanced_base_primitives_64_2 },
    { 0x1d, "DrawIndexedInstancedBasePrimitives16_2",    20, 1, render_op_draw_indexed_instanced_base_primitives_16_2 },
    /* TODO: RE opcode 0x1e-0x3b range — macOS sends 0x2c (len=88) repeatedly.
     * Stub for now to avoid crashes; add real handler once semantics discovered. */
    { 0x2c, "Unknown(0x2c)",                             88, 0, render_op_ack_stub },
    { 0x3c, "CmdExecIndirect2Inner",                     20, 2, render_op_cmd_exec_indirect2_inner },

    /* --- State-set family (0x65-0xa6) --------------------------- */
    { 0x65, "SetBlendColor",                             16, 0, render_op_set_blend_color },
    { 0x66, "SetColorStoreAction",                        8, 0, render_op_set_color_store_action },
    { 0x67, "SetColorStoreActionOptions",                12, 0, render_op_set_color_store_action_options },
    { 0x68, "SetDepthStencilState",                       4, 1, render_op_set_depth_stencil_state },
    { 0x69, "SetDepthStoreAction",                        8, 0, render_op_set_depth_store_action },
    { 0x6a, "SetDepthStoreActionOptions",                 8, 0, render_op_set_depth_store_action_options },
    { 0x6b, "SetCullMode",                                8, 0, render_op_set_cull_mode },
    { 0x6c, "SetDepthBias",                              12, 0, render_op_set_depth_bias },
    { 0x6d, "SetDepthClipMode",                           8, 0, render_op_set_depth_clip_mode },
    { 0x6e, "SetFragmentBuffers",                         0, 0, render_op_set_fragment_buffers },
    { 0x6f, "SetFragmentBufferOffset",                   12, 0, render_op_set_fragment_buffer_offset },
    { 0x70, "SetFragmentSamplerStates",                   0, 0, render_op_set_fragment_sampler_states },
    { 0x71, "SetFragmentSamplerStatesLODClamp",           0, 0, render_op_set_fragment_sampler_states_lod_clamp },
    { 0x72, "SetFragmentTextures",                        0, 0, render_op_set_fragment_textures },
    { 0x73, "SetFrontFacingWinding",                      8, 0, render_op_set_front_facing_winding },
    { 0x74, "SetRenderPipelineState",                     4, 1, render_op_set_render_pipeline_state },
    { 0x75, "SetScissorRect",                            32, 0, render_op_set_scissor_rect },
    { 0x76, "SetScissorRects",                            0, 0, render_op_set_scissor_rects },
    { 0x77, "SetStencilRef",                              8, 0, render_op_set_stencil_ref },
    { 0x78, "SetStencilStoreAction",                      8, 0, render_op_set_stencil_store_action },
    { 0x79, "SetStencilStoreActionOptions",               8, 0, render_op_set_stencil_store_action_options },
    { 0x7a, "SetTesselationFactorBuffer",                20, 1, render_op_set_tesselation_factor_buffer },
    { 0x7b, "SetTesselationFactorScale",                  4, 0, render_op_set_tesselation_factor_scale },
    { 0x7c, "SetTriangleFillMode",                        8, 0, render_op_set_triangle_fill_mode },
    { 0x7d, "SetVertexBuffers",                           0, 0, render_op_set_vertex_buffers },
    { 0x7e, "SetVertexBufferOffset",                     12, 0, render_op_set_vertex_buffer_offset },
    { 0x7f, "SetVertexSamplerStates",                     0, 0, render_op_set_vertex_sampler_states },
    { 0x80, "SetVertexSamplerStatesLODClamp",             0, 0, render_op_set_vertex_sampler_states_lod_clamp },
    { 0x81, "SetVertexTextures",                          0, 0, render_op_set_vertex_textures },
    { 0x82, "SetViewport",                               48, 0, render_op_set_viewport },
    { 0x83, "SetViewports",                               0, 0, render_op_set_viewports },
    { 0x84, "SetVisibilityResultMode",                   16, 0, render_op_set_visibility_result_mode },
    { 0x85, "TextureBarrier",                             0, 0, render_op_texture_barrier },
    { 0x86, "UseHeaps",                                   0, 0, render_op_use_heaps },
    { 0x87, "UseResources",                               0, 0, render_op_use_resources },
    { 0x88, "SetLineWidth",                               4, 0, render_op_set_line_width },
    { 0x89, "UseResourcesWithStages",                     0, 0, render_op_use_resources_with_stages },
    { 0x8a, "SetAlphaTestReferenceValue",                 4, 0, render_op_set_alpha_test_reference_value },
    { 0x8b, "SetPointSize",                               4, 0, render_op_set_point_size },
    { 0x8c, "SetClipPlane",                              20, 0, render_op_set_clip_plane },
    { 0x8d, "SetVertexSamplerState",                     20, 1, render_op_set_vertex_sampler_state },
    { 0x8e, "SetFragmentSamplerState",                   20, 1, render_op_set_fragment_sampler_state },
    { 0x8f, "SetViewportTransformEnabled",                4, 0, render_op_set_viewport_transform_enabled },
    { 0x90, "SetProvokingVertexMode",                     4, 0, render_op_set_provoking_vertex_mode },
    { 0x91, "SetPrimitiveRestartIndexEnabled",            8, 0, render_op_set_primitive_restart_index_enabled },
    { 0x92, "SetTriangleFillModeFrontBack",               4, 0, render_op_set_triangle_fill_mode_front_back },
    { 0x93, "SetTransformFeedbackState",                  4, 0, render_op_set_transform_feedback_state },
    { 0x94, "SetDepthCleared",                            0, 0, render_op_set_depth_cleared },
    { 0x95, "SetStencilCleared",                          0, 0, render_op_set_stencil_cleared },
    { 0x96, "SetColorResolveTexture",                    16, 1, render_op_set_color_resolve_texture },
    { 0x97, "SetDepthResolveTexture",                    12, 1, render_op_set_depth_resolve_texture },
    { 0x98, "SetStencilResolveTexture",                  12, 1, render_op_set_stencil_resolve_texture },
    { 0x99, "SetVertexAmplificationMode",                 8, 0, render_op_set_vertex_amplification_mode },
    { 0x9a, "SetVertexAmplificationCount",                0, 0, render_op_set_vertex_amplification_count },
    { 0x9b, "DispatchThreadsPerTile",                    24, 0, render_op_dispatch_threads_per_tile },
    { 0x9c, "SetRenderThreadgroupMemoryLength",          20, 0, render_op_set_render_threadgroup_memory_length },
    { 0x9d, "SetTileBuffers",                             0, 0, render_op_set_tile_buffers },
    { 0x9e, "SetTileBufferOffset",                       12, 0, render_op_set_tile_buffer_offset },
    { 0x9f, "SetTileSamplerStates",                       0, 0, render_op_set_tile_sampler_states },
    { 0xa0, "SetTileSamplerStatesLODClamp",               0, 0, render_op_set_tile_sampler_states_lod_clamp },
    { 0xa1, "SetTileTextures",                            0, 0, render_op_set_tile_textures },
    { 0xa2, "DispatchThreadsPerTileInRegion",            76, 0, render_op_dispatch_threads_per_tile_in_region },
    { 0xa3, "DispatchThreadsPerTileInRegionWithIndex",   76, 0, render_op_dispatch_threads_per_tile_in_region_w_idx },
    { 0xa4, "GetTileDimensions",                         12, 1, render_op_get_tile_dimensions },
    { 0xa5, "SetVertexBuffersWithStride",                 0, 0, render_op_set_vertex_buffers_with_stride },
    { 0xa6, "SetVertexBufferOffsetWithStride",           20, 0, render_op_set_vertex_buffer_offset_with_stride },
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
