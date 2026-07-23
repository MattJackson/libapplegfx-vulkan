/*
 * libapplegfx-vulkan — pipeline cache, vertex upload, draw emission
 * src/handlers/compute/draw_emit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "compute_draw_internal.h"
#include "display.h"
#include "protocol/object_resolver.h"
#include "air2spv/spv_reflect.h"

#include "common/le.h"
#include "common/log.h"
#include "common/policy.h"
#include "device.h"
#include "protocol/state.h"
#include "vulkan/iosurface.h"
#include "vulkan/display_blit.h"
#include "vulkan/composite.h"
#include "vulkan/pipeline_build.h"
#include "vulkan/draw_record.h"
#include "vulkan/descriptor_layout.h"

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int lagfx_draw_emit_not_empty_t;

#ifdef LAGFX_HAVE_VULKAN

/* B1 (M0): destroy a task's cached draw pipeline (on shader change / teardown). */
void lagfx_pending_pipeline_drop_cache(lagfx_task_entry_t *task, VkDevice device) {
    if (task->pending_pipeline.cached_pipeline) {
        vkDestroyPipeline(device, (VkPipeline)task->pending_pipeline.cached_pipeline, NULL);
        task->pending_pipeline.cached_pipeline  = 0;
        task->pending_pipeline.cached_color_fmt = 0;
        task->pending_pipeline.cached_depth_fmt = 0;
    }
}

/* B1 (M0): return the cached VkPipeline for this pending_pipeline, building +
 * caching it on first use or when the RT formats change. Replaces the per-draw
 * lagfx_pipeline_build that leaked a VkPipeline every draw (→ OOM on long runs).
 * The cache is keyed on pending_pipeline identity (shaders/layout, invalidated by
 * op_0x74 via drop_cache) + the render-target formats. */
VkPipeline lagfx_get_cached_pipeline(lagfx_task_entry_t *task, VkDevice device,
                                            const lagfx_pipeline_desc_t *pdesc) {
    if (task->pending_pipeline.cached_pipeline
        && task->pending_pipeline.cached_color_fmt == (uint32_t)pdesc->color_format
        && task->pending_pipeline.cached_depth_fmt == (uint32_t)pdesc->depth_format) {
        return (VkPipeline)task->pending_pipeline.cached_pipeline;
    }
    lagfx_pending_pipeline_drop_cache(task, device);
    VkPipeline p = VK_NULL_HANDLE;
    if (lagfx_pipeline_build(device, pdesc, &p) != LAGFX_OK || p == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    task->pending_pipeline.cached_pipeline  = (uintptr_t)p;
    task->pending_pipeline.cached_color_fmt = (uint32_t)pdesc->color_format;
    task->pending_pipeline.cached_depth_fmt = (uint32_t)pdesc->depth_format;
    return p;
}


/* Metal buffer indices CLAIMED by the vertex shader's [[buffer(n)]] args
 * (AIR arg-metadata map) — those slots are uniforms (MVP etc.), never the
 * stage-in stream(s). */
static uint32_t lagfx_vtx_claimed_mask(const lagfx_task_entry_t *task) {
    uint32_t claimed = 0u;
    for (uint32_t bi = 0; bi < task->pending_pipeline.n_spv_bindings && bi < 16u; bi++) {
        if (task->pending_pipeline.spv_binding_kind[bi]
                != (uint8_t)LAGFX_SPV_BINDING_STORAGE_BUFFER)
            continue;
        if (task->pending_pipeline.spv_binding_no[bi] >= LAGFX_FRAG_BINDING_BASE)
            continue;   /* fragment-stage binding */
        int16_t mi = task->pending_pipeline.spv_binding_metal[bi];
        if (mi >= 0 && mi < 32) claimed |= (1u << (uint32_t)mi);
    }
    return claimed;
}

/* MULTI-STREAM stage-in detection (GOAL-M2x lldb ground truth, 2026-07-21):
 * SkyLight's own composite PSOs (unlabeled; host 0x01 DrawPrimitives16) bind
 * SEPARATE per-attribute streams via setVertexBytes — pos float2 stride 8 at
 * Metal index 0, texcoord float2 stride 8 at index 1, with the pixel->NDC MVP
 * at index 2 (AIR-claimed) — NOT one interleaved buffer. Discriminator: >= 2
 * valid non-claimed vertex slots. Returns the candidate slots in ascending
 * order (attr a <- a-th stream, the binding order lldb observed). */
static uint32_t lagfx_vtx_candidate_slots(lagfx_task_entry_t *task,
                                          uint32_t slots_out[8]) {
    uint32_t claimed = lagfx_vtx_claimed_mask(task);
    uint32_t n = 0;
    for (uint32_t s = 0; s < 8u && n < 8u; s++) {
        if (claimed & (1u << s)) continue;
        lagfx_binding_slot_t *cs = &task->bindings.vertex_buffers[s];
        if (!cs->valid || cs->ref == 0u) continue;
        slots_out[n++] = s;
    }
    return n;
}

bool lagfx_vtx_multi_stream(lagfx_task_entry_t *task) {
    /* Multi-stream shader shape: attr0/attr1 are float2 (the 8-byte pos/tex
     * streams — SkyLight UberComposite pos2/tex2/scale1). The interleaved CA
     * family leads with a float4 screen pos, so it can never match. */
    if (task->pending_pipeline.n_vtx_inputs < 2u
        || task->pending_pipeline.vtx_in_comp[0] != 2u
        || task->pending_pipeline.vtx_in_comp[1] != 2u)
        return false;
    /* lldb ground truth: this family ALWAYS binds pos stream at slot 0 and
     * tex stream at slot 1 (MVP at 2). Do NOT consult the AIR-claimed mask
     * here — the map's mvp index (fixture: [[buffer(1)]]) differs from the
     * live binding (MVP at 2), and masking slot 1 pushed the scan onto the
     * matrix slot (rendered 2/w-scale junk). Slots 0/1 verbatim. */
    return task->bindings.vertex_buffers[0].valid
           && task->bindings.vertex_buffers[0].ref != 0u
           && task->bindings.vertex_buffers[1].valid
           && task->bindings.vertex_buffers[1].ref != 0u;
}

/* GOAL-M2x: flag PIXEL-space texcoords (attr1 max |uv| > 2) so the
 * descriptor bind selects the unnormalized sampler (Metal coord::pixel —
 * CA glyph-atlas composites; normalized sampling clamp-smears the atlas
 * edge into full-width white bands). */
static void lagfx_flag_pixel_texcoords(lagfx_task_entry_t *task, const uint8_t *buf,
                                       uint32_t buf_sz, uint32_t stride, uint32_t tex_off) {
    task->pending_draw.pixel_texcoords = false;
    if (task->pending_pipeline.n_vtx_inputs < 2u || stride == 0u
        || tex_off + 8u > stride) return;
    float mx = 0.0f;
    for (uint32_t v = 0; v < 8u && (size_t)(v + 1u) * stride <= buf_sz; v++) {
        float tu, tv; uint32_t u;
        u = lagfx_le32(buf + (size_t)v * stride + tex_off);      memcpy(&tu, &u, 4);
        u = lagfx_le32(buf + (size_t)v * stride + tex_off + 4u); memcpy(&tv, &u, 4);
        if (tu == tu && tu > mx) mx = tu;
        if (tv == tv && tv > mx) mx = tv;
    }
    task->pending_draw.pixel_texcoords = (mx > 2.0f);
}

/* Upload the guest's vertex buffer (vertex_buffers[0], real data via the
 * placement descriptor PFN<<12 + page-table translate — the M2 path) into a
 * host VkBuffer for vertex-input binding. Returns VK_NULL_HANDLE if there is no
 * real vertex data; the caller frees the returned buffer + *out_mem. */
VkBuffer lagfx_upload_guest_vertex_buffer(lagfx_protocol_t *p,
                                                 lagfx_task_entry_t *task,
                                                 struct lagfx_vk_state *vk,
                                                 VkDeviceMemory *out_mem,
                                                 uint32_t *out_size) {
    *out_mem = VK_NULL_HANDLE;
    *out_size = LAGFX_DRAW_DS_BUF_SZ;
    if (!task->bindings.vertex_buffers[0].valid
        || task->bindings.vertex_buffers[0].ref == 0u)
        return VK_NULL_HANDLE;
    lagfx_binding_slot_t *vbs = &task->bindings.vertex_buffers[0];
    /* MULTI-STREAM interleave (GOAL-M2x): when the guest binds separate
     * per-attribute streams (SkyLight composites: pos f2/8B + tex f2/8B +
     * AIR-claimed MVP), interleave them CPU-side into ONE binding-0 buffer at
     * the reflected tight layout (attr a at off_a = sum(comp[<a])*4, stride =
     * round8(sum)) — the exact layout pipeline_build emits when vtx_stride==0.
     * Streams beyond those bound are filled with 1.0f (neutral scale/opacity).
     * Kill-switch: LAGFX_DISABLE_MULTISTREAM. */
    if (LAGFX_POLICY("M2_VTXSRC") && getenv("LAGFX_DISABLE_MULTISTREAM") == NULL
        && lagfx_vtx_multi_stream(task)) {
        /* Fixed stream slots per lldb ground truth: pos@0, tex@1 (MVP@2 is a
         * shader buffer arg, not a stream). */
        uint32_t slots[8] = {0u, 1u};
        uint32_t sstart = 0;
        uint32_t nstreams = 2u;
        uint32_t want = getenv("LAGFX_M2_BIGVERTS")
                            ? LAGFX_BIGVERTS_BUF_SZ : LAGFX_DRAW_DS_BUF_SZ;
        uint32_t nvi = task->pending_pipeline.n_vtx_inputs > 8u
                           ? 8u : task->pending_pipeline.n_vtx_inputs;
        uint32_t offs[8], tight = 0;
        for (uint32_t a = 0; a < nvi; a++) {
            offs[a] = tight;
            tight += (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
        }
        uint32_t stride = (tight + 7u) & ~7u;
        if (stride == 0u) stride = 8u;
        uint32_t maxv = want / stride;
        uint8_t *full = calloc(1u, want);
        if (full) {
            uint32_t filled_streams = 0;
            for (uint32_t a = 0; a < nvi; a++) {
                uint32_t sa = (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
                if (a < nstreams) {
                    lagfx_binding_slot_t *cs = &task->bindings.vertex_buffers[slots[sstart + a]];
                    uint32_t sneed = maxv * sa;
                    if (sneed > want) sneed = want;
                    uint8_t *sbuf = calloc(1u, sneed);
                    const char *how = "none";
                    uint32_t got = sbuf ? lagfx_read_vtx_source(p, task, cs->ref,
                                              cs->offset, sneed, sbuf, &how, 0) : 0;
                    /* SANITY (GOAL-M2y): the c2c2 attr shape ALSO matches
                     * interleaved 3-attr pipes whose real stream is at another
     * slot — their slot-0/1 leftovers read ALL-ZERO and the draw
                     * uploads dead geometry. A position stream with no nonzero
                     * byte in its first 96 B is never valid → treat the whole
                     * multi-stream match as wrong and fall back to VTXSRC. */
                    if (a == 0u && got) {
                        bool any = false;
                        for (uint32_t z = 0; z < got && z < 96u; z++)
                            if (sbuf[z]) { any = true; break; }
                        if (!any) {
                            free(sbuf);
                            free(full);
                            full = NULL;
                            break;
                        }
                    }
                    if (got) filled_streams++;
                    for (uint32_t i = 0; i < maxv && sbuf; i++)
                        memcpy(full + (size_t)i * stride + offs[a],
                               sbuf + (size_t)i * sa, sa);
                    free(sbuf);
                } else {
                    /* No stream bound for this attr — neutral 1.0f per comp. */
                    float one = 1.0f;
                    for (uint32_t i = 0; i < maxv; i++)
                        for (uint32_t cco = 0; cco < sa; cco += 4u)
                            memcpy(full + (size_t)i * stride + offs[a] + cco, &one, 4u);
                }
            }
            if (filled_streams) {
                /* Indexed multi-stream draws: expand indices over the
                 * interleaved buffer at the SAME tight stride. */
                if (task->pending_draw.indexed
                    && task->pending_draw.index_type == 0u
                    && task->pending_draw.index_count > 0u
                    && task->pending_draw.index_count <= 4096u
                    && task->pending_draw.index_buffer_ref != 0u) {
                    uint32_t icount = task->pending_draw.index_count;
                    uint8_t *ib = malloc((size_t)icount * 2u);
                    const char *ihow = "none";
                    if (ib && lagfx_read_vtx_source(p, task,
                            task->pending_draw.index_buffer_ref,
                            task->pending_draw.index_buffer_offset,
                            icount * 2u, ib, &ihow, 0) == icount * 2u) {
                        uint8_t *exp = calloc(1u, want);
                        if (exp) {
                            uint32_t emitted = 0;
                            for (uint32_t k = 0; k < icount
                                 && (size_t)(k + 1u) * stride <= want; k++) {
                                uint32_t idx = (uint32_t)ib[k*2]
                                               | ((uint32_t)ib[k*2+1] << 8);
                                if (idx >= maxv) continue;
                                memcpy(exp + (size_t)k * stride,
                                       full + (size_t)idx * stride, stride);
                                emitted++;
                            }
                            if (emitted == icount) memcpy(full, exp, want);
                            free(exp);
                        }
                    }
                    free(ib);
                }
                lagfx_flag_pixel_texcoords(task, full, want, stride, offs[0+1] - 0u);
                VkBuffer svb = VK_NULL_HANDLE;
                if (lagfx_vk_make_host_storage_buffer(vk, full, want,
                                                      &svb, out_mem) == LAGFX_OK) {
                    *out_size = want;
                    if (getenv("LAGFX_DUMP_SPV")) {
                        LAGFX_LOG("MSTREAM: pipe=0x%x interleaved %u streams -> %u attrs stride=%u (slots %u/%u/%u)",
                                  task->pending_pipeline.reference, nstreams, nvi,
                                  stride, slots[sstart],
                                  nstreams > 1 ? slots[sstart + 1] : 99u,
                                  nstreams > 2 ? slots[sstart + 2] : 99u);
                        for (uint32_t vt = 0; vt < 4 && (size_t)(vt+1)*stride <= want; vt++) {
                            float px, py; uint32_t u2;
                            u2 = lagfx_le32(full + (size_t)vt*stride);      memcpy(&px,&u2,4);
                            u2 = lagfx_le32(full + (size_t)vt*stride + 4u); memcpy(&py,&u2,4);
                            LAGFX_LOG("MSTREAM v%u pos[%.4g %.4g]", vt, px, py);
                        }
                    }
                    free(full);
                    return svb;
                }
            }
            free(full);
        }
        /* fall through to the single-stream scan on any failure */
    }
    /* M2c VTXSRC (LAGFX_M2_VTXSRC): multi-slot vertex-SOURCE search. The slot-0
     * refs for the composite draws (0x15/0x16/0x18) resolve to unwritten
     * text/poison slabs, while slot-1's ref 0x13 receives live 0x3b backing
     * updates — the real position stream may be in a different slot than 0.
     * Sample each bound slot (BACKUPD-VA preferred, else placement walk), score
     * plausibility-as-float32, upload the first slot scoring ≥50. Falls through
     * to the legacy slot-0 path when off or when nothing scores. */
    if (LAGFX_POLICY("M2_VTXSRC")) {
        uint32_t want = getenv("LAGFX_M2_BIGVERTS")
                            ? LAGFX_BIGVERTS_BUF_SZ : LAGFX_DRAW_DS_BUF_SZ;
        /* Score EVERY bound slot/mode and pick the HIGHEST — not the first ≥50.
         * The per-vertex geometry is at buffer index 1 (ground truth: ref 0x15
         * scores 100), while slot-0 uniform buffers score 45–80 (floats look
         * "plausible"). Taking the first ≥50 let slot-0 uniforms win and fed the
         * shader uniforms-as-positions → smear/zero-coverage. Highest-score wins. */
        /* Slots CLAIMED by the vertex shader's [[buffer(n)]] args (from the
         * AIR arg-metadata map) are NOT the stage-in stream — e.g. the 64 B
         * mvp_matrix at index 2: its matrix floats score 100 on plausibility,
         * so an unrestricted scan uploads the MATRIX (+ trailing pool garbage)
         * as vertex data → horizontal-band smear. Exclude claimed slots. */
        uint32_t claimed = 0u;
        for (uint32_t bi = 0; bi < task->pending_pipeline.n_spv_bindings && bi < 16u; bi++) {
            if (task->pending_pipeline.spv_binding_kind[bi]
                    != (uint8_t)LAGFX_SPV_BINDING_STORAGE_BUFFER)
                continue;
            if (task->pending_pipeline.spv_binding_no[bi] >= LAGFX_FRAG_BINDING_BASE)
                continue;   /* fragment-stage binding */
            int16_t mi = task->pending_pipeline.spv_binding_metal[bi];
            if (mi >= 0 && mi < 32) claimed |= (1u << mi);
        }
        /* Pipeline vertex stride (for the position-signature test). PSO stride
         * is authoritative when sane (lldb ground truth GOAL-M2x: buffer stride
         * 48 vs reflected extent 32 — see pipeline_build.c). */
        uint32_t pstride = 0;
        for (uint32_t a = 0; a < task->pending_pipeline.n_vtx_inputs && a < 8u; a++)
            pstride += (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
        pstride = (pstride + 7u) & ~7u;
        if (task->pending_pipeline.vtx_stride >= pstride
            && task->pending_pipeline.vtx_stride <= 256u)
            pstride = task->pending_pipeline.vtx_stride;
        if (pstride == 0u) pstride = 48u;

        uint32_t best_s = 0u; int best_mode = 0; uint32_t best_score = 0u;
        bool have_best = false;
        /* SLOT-1 AUTHORITATIVE (GOAL-M2y, lldb ground truth): every captured
         * CA composite draw (caret, textured, 9-patch frame) binds its vertex
         * stream at METAL INDEX 1 — uniforms at 2+. The score-scan is fooled
         * twice over: legit 9-patch verts have NEGATIVE coords (x=-271, z=-10)
         * that FAIL the positive-screen-range position signature, while the
         * "uniform" slab's read window covers ADJACENT pool content that
         * contains other draws' embedded verts (573,87..) and wins — feeding
         * stretched caret-junk to the frame draws (the band artifact). For the
         * c4-led interleaved family, use slot 1 verbatim. */
        if (task->pending_pipeline.n_vtx_inputs >= 2u
            && task->pending_pipeline.vtx_in_comp[0] == 4u
            && task->bindings.vertex_buffers[1].valid
            && task->bindings.vertex_buffers[1].ref != 0u
            && getenv("LAGFX_DISABLE_SLOT1AUTH") == NULL) {
            best_s = 1u; best_mode = 0; best_score = 2000u; have_best = true;
        } else {
        /* Scan slots 0..7 (was 0..2): the per-vertex stream is bound at a slot
         * that varies per draw — live SetVertexBuffers binds ref 0x24 at slots
         * 1/2/3, and the ~20% of composites that fell back to the matrix had
         * their positions in a slot > 2. */
        for (uint32_t sm = 0; sm < 16u; sm++) {
            /* slot-major, then mode: s/m0, s/m1 for slots 0..7 —
             * mode 1 = VA-translated placement read (lead 3). */
            uint32_t s = sm / 2u;
            int mode = (int)(sm % 2u);
            if (claimed & (1u << s)) continue;   /* shader buffer arg, not stage-in */
            lagfx_binding_slot_t *cs = &task->bindings.vertex_buffers[s];
            if (!cs->valid || cs->ref == 0u) continue;
            uint8_t sample[512];
            const char *how = "none";
            uint32_t got = lagfx_read_vtx_source(p, task, cs->ref, cs->offset,
                                                 sizeof(sample), sample, &how, mode);
            uint32_t plaus = got ? lagfx_vtx_float_plausibility(sample, got) : 0u;
            /* PRIMARY selector: does this slot hold a per-vertex POSITION stream
             * at the pipeline stride (screen-space, varying) vs a matrix/uniform
             * block (repeated ±1/tiny)? The plausibility score can't tell them
             * apart — a matrix scores 100 too — so an unrestricted highest-score
             * pick uploaded the MVP as the vertex stream → band smear. Weight the
             * position signature far above bare plausibility. */
            uint32_t posity = got ? lagfx_vtx_looks_like_positions(sample, got, pstride) : 0u;
            uint32_t score = posity ? (1000u + posity) : plaus;
            if (getenv("LAGFX_DUMP_SPV"))
                LAGFX_LOG("VTXSRC slot=%u mode=%d ref=0x%x off=%llu how=%s plaus=%u pos=%u score=%u",
                          s, mode, cs->ref, (unsigned long long)cs->offset, how, plaus, posity, score);
            if (score >= 50u && score > best_score) {
                best_score = score; best_s = s; best_mode = mode; have_best = true;
            }
        }
        /* GOAL-M2y: bare float-plausibility is NOT a vertex stream (the M2v
         * lesson, again): uploading a "plausible" uniform/junk slab draws
         * garbage slivers into per-pass surfaces, which the overlay then
         * re-stamps onto the scanout every frame — the persistent stripe/line
         * artifacts. Without a POSITION-signature match (score >= 1000), skip
         * the draw entirely: no stream identified -> nothing to render.
         * Revert switch: LAGFX_VTX_ALLOW_JUNK=1. */
        if (have_best && best_score < 1000u
            && getenv("LAGFX_VTX_ALLOW_JUNK") == NULL) {
            if (getenv("LAGFX_DUMP_SPV"))
                LAGFX_LOG("VTXSRC: no position-signature stream (best=%u) — draw skipped",
                          best_score);
            return VK_NULL_HANDLE;
        }
        }   /* end score-scan (non-slot1-authoritative pipes) */
        if (have_best) {
            uint32_t s = best_s; int mode = best_mode; uint32_t score = best_score;
            lagfx_binding_slot_t *cs = &task->bindings.vertex_buffers[s];
            const char *how = "none";
            uint8_t *full = calloc(1u, want);
            if (!full) return VK_NULL_HANDLE;
            uint32_t ffill = lagfx_read_vtx_source(p, task, cs->ref, cs->offset,
                                                   want, full, &how, mode);
            /* GROUND-TRUTH LOCATOR (LAGFX_DUMP_SPV): the guest DTrace proved the
             * real CA vertex is float4 (572,87,0,1) w=1.0 at 48B stride. Scan the
             * resolved backing for a vertex-like 16B: pos.x,pos.y in screen range
             * AND pos.w == 1.0 at +12. Log the FIRST such offset — if != 0 the
             * real vertices sit at a different offset than our slot read starts,
             * pinpointing the pool-buffer offset/resolve delta. */
            if (ffill && getenv("LAGFX_DUMP_SPV")) {
                int found = -1;
                for (uint32_t o = 0; o + 16u <= ffill && found < 0; o += 4u) {
                    float px, py, pw; uint32_t u;
                    u = lagfx_le32(full+o);    memcpy(&px,&u,4);
                    u = lagfx_le32(full+o+4);  memcpy(&py,&u,4);
                    u = lagfx_le32(full+o+12); memcpy(&pw,&u,4);
                    float ax = px<0?-px:px, ay = py<0?-py:py;
                    if (pw == 1.0f && ax >= 1.0f && ax <= 4096.0f
                        && ay >= 1.0f && ay <= 4096.0f) found = (int)o;
                }
                LAGFX_LOG("VTXLOCATE ref=0x%x slotoff=%llu: real-vertex(w=1) signature at backing offset %d (slot read starts at 0)",
                          cs->ref, (unsigned long long)cs->offset, found);
            }
            /* INDEXED draws (0x07): expand the index list CPU-side into an
             * unindexed vertex stream so the existing vkCmdDraw path renders
             * the REAL topology. Wire fields sourced from Apple's decoder
             * (prim/indexType/count/ref/offset — see op_draw_indexed_
             * primitives_16). Without this, indexed geometry was drawn
             * unindexed: vertices consumed 0..N-1 → wrong triangles. */
            if (ffill && task->pending_draw.indexed
                && task->pending_draw.index_type == 0u        /* UInt16 */
                && task->pending_draw.index_count > 0u
                && task->pending_draw.index_count <= 4096u
                && task->pending_draw.index_buffer_ref != 0u) {
                uint32_t icount = task->pending_draw.index_count;
                uint8_t *ib = malloc((size_t)icount * 2u);
                const char *ihow = "none";
                if (ib && lagfx_read_vtx_source(p, task,
                        task->pending_draw.index_buffer_ref,
                        task->pending_draw.index_buffer_offset,
                        icount * 2u, ib, &ihow, 0) == icount * 2u) {
                    /* Vertex stride: same rule as pipeline_build (decoded PSO
                     * stride authoritative when sane — lldb ground truth
                     * GOAL-M2x — else round8 of the attr sum). */
                    uint32_t vs = 0;
                    for (uint32_t a = 0; a < task->pending_pipeline.n_vtx_inputs && a < 8u; a++)
                        vs += (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
                    vs = (vs + 7u) & ~7u;
                    if (task->pending_pipeline.vtx_stride >= vs
                        && task->pending_pipeline.vtx_stride <= 256u)
                        vs = task->pending_pipeline.vtx_stride;
                    if (vs == 0u) vs = 48u;
                    uint8_t *exp = calloc(1u, want);
                    if (exp) {
                        uint32_t maxv = want / vs;
                        uint32_t emitted = 0;
                        for (uint32_t k = 0; k < icount && (size_t)(k + 1u) * vs <= want; k++) {
                            uint32_t idx = (uint32_t)ib[k*2] | ((uint32_t)ib[k*2+1] << 8);
                            if (idx >= maxv) continue;
                            memcpy(exp + (size_t)k * vs, full + (size_t)idx * vs, vs);
                            emitted++;
                        }
                        if (emitted == icount) {
                            memcpy(full, exp, want);
                            /* FORCE w=1 on the float4 position (offset +12). The
                             * texture-composite draws (0x23/0x28/0x2c) carry real
                             * X,Y screen coords but garbage Z,W (w=920/938) — the
                             * perspective divide by that w shrinks the quad to
                             * nothing. Every valid CA composite vertex has w=1, so
                             * clamping w=1 recovers the geometry when only Z/W are
                             * misaligned. Gated LAGFX_DISABLE_FORCE_W1. */
                            if (getenv("LAGFX_DISABLE_FORCE_W1") == NULL) {
                                float one = 1.0f;
                                for (uint32_t k = 0; (size_t)(k + 1u) * vs <= want; k++) {
                                    uint8_t *pw = full + (size_t)k * vs + 12u;
                                    uint32_t wu; memcpy(&wu, pw, 4);
                                    float wf; memcpy(&wf, &wu, 4);
                                    /* only override implausible w (keep genuine 1.0) */
                                    if (!(wf >= 0.99f && wf <= 1.01f))
                                        memcpy(pw, &one, 4);
                                }
                            }
                            if (getenv("LAGFX_DUMP_SPV")) {
                                /* Whether THIS pipeline samples a texture (has a
                                 * SAMPLED_IMAGE binding) — so we can tell a flat
                                 * ColorFill panel (texcoords irrelevant) from a
                                 * texture draw whose texcoords must span 0..1. */
                                bool samples_tex = false;
                                for (uint32_t bb = 0; bb < task->pending_pipeline.n_spv_bindings && bb < 16u; bb++)
                                    if (task->pending_pipeline.spv_binding_kind[bb]
                                        == (uint8_t)LAGFX_SPV_BINDING_SAMPLED_IMAGE) { samples_tex = true; break; }
                                LAGFX_LOG("IDXEXP: pipe=0x%x samples_tex=%d expanded %u u16 indices (ref=0x%x off=%u stride=%u how=%s)",
                                          task->pending_pipeline.reference, samples_tex ? 1 : 0,
                                          icount, task->pending_draw.index_buffer_ref,
                                          task->pending_draw.index_buffer_offset, vs, ihow);
                                /* Dump the first 3 expanded verts as float4@0 +
                                 * float2@16 so we can see if positions are real
                                 * screen-space (e.g. (572,87)) or garbage. */
                                for (uint32_t vt = 0; vt < 3 && (size_t)(vt+1)*vs <= want; vt++) {
                                    const uint8_t *vp = full + (size_t)vt * vs;
                                    float px,py,pz,pw,tu,tv; uint32_t u;
                                    u=lagfx_le32(vp+0);  memcpy(&px,&u,4);
                                    u=lagfx_le32(vp+4);  memcpy(&py,&u,4);
                                    u=lagfx_le32(vp+8);  memcpy(&pz,&u,4);
                                    u=lagfx_le32(vp+12); memcpy(&pw,&u,4);
                                    u=lagfx_le32(vp+16); memcpy(&tu,&u,4);
                                    u=lagfx_le32(vp+20); memcpy(&tv,&u,4);
                                    LAGFX_LOG("IDXEXP48 v%u idx=%u pos[%.4g %.4g %.4g %.4g] tex[%.4g %.4g]",
                                              vt, (uint32_t)ib[vt*2] | ((uint32_t)ib[vt*2+1]<<8),
                                              px,py,pz,pw,tu,tv);
                                }
                            }
                        }
                        free(exp);
                    }
                }
                free(ib);
            }
            if (ffill) {
                lagfx_flag_pixel_texcoords(task, full, want, pstride,
                                           (uint32_t)task->pending_pipeline.vtx_in_comp[0] * 4u);
                VkBuffer svb = VK_NULL_HANDLE;
                if (lagfx_vk_make_host_storage_buffer(vk, full, want,
                                                      &svb, out_mem) == LAGFX_OK) {
                    *out_size = want;
                    LAGFX_LOG("VTXSRC: uploading slot=%u ref=0x%x how=%s score=%u filled=%u",
                              s, cs->ref, how, score, ffill);
                    /* VTX24-style dump of the CHOSEN source (stride 24) so the
                     * actual uploaded positions are visible. */
                    if (getenv("LAGFX_DUMP_SPV")) {
                        for (int vt = 0; vt < 6; vt++) {
                            size_t bo = (size_t)vt * 24u;
                            if (bo + 24u > (size_t)want) break;
                            float px2, py2, tx2, ty2; uint32_t u2;
                            u2 = lagfx_le32(full+bo+0);  memcpy(&px2,&u2,4);
                            u2 = lagfx_le32(full+bo+4);  memcpy(&py2,&u2,4);
                            u2 = lagfx_le32(full+bo+8);  memcpy(&tx2,&u2,4);
                            u2 = lagfx_le32(full+bo+12); memcpy(&ty2,&u2,4);
                            LAGFX_LOG("VTXSRC24 ref=0x%x v%d[%.4g,%.4g|%.4g,%.4g]",
                                      cs->ref, vt, px2, py2, tx2, ty2);
                        }
                    }
                    free(full);
                    return svb;
                }
            }
            free(full);
        }
        /* nothing plausible → fall through to legacy slot-0 behavior */
    }
    /* M2c BACKUPD (LAGFX_M2_BACKUPD): if a 0x3b backing update recorded a data
     * address for this ref, read the vertex bytes from THAT (task VA, page-aware)
     * instead of the placement-table PFN walk — the placement is stale for some
     * refs (0x15/0x16/0x18 slabs read as boot-log text/poison). */
    if (getenv("LAGFX_M2_BACKUPD")) {
        for (uint32_t bi = 0; bi < p->backing_update_n && bi < 64u; bi++) {
            if (!p->backing_update[bi].valid
                || p->backing_update[bi].ref != vbs->ref) continue;
            uint32_t bwant = getenv("LAGFX_M2_BIGVERTS")
                                 ? LAGFX_BIGVERTS_BUF_SZ : LAGFX_DRAW_DS_BUF_SZ;
            uint8_t *bdata = calloc(1u, bwant);
            if (!bdata) return VK_NULL_HANDLE;
            uint64_t src = p->backing_update[bi].addr + vbs->offset;
            if (lagfx_task_read_virtual(p, task, src, bwant, bdata)) {
                VkBuffer bvb = VK_NULL_HANDLE;
                if (lagfx_vk_make_host_storage_buffer(vk, bdata, bwant,
                                                      &bvb, out_mem) == LAGFX_OK) {
                    *out_size = bwant;
                    if (getenv("LAGFX_DUMP_SPV")) {
                        float b0, b1; uint32_t u;
                        u = lagfx_le32(bdata+0); memcpy(&b0,&u,4);
                        u = lagfx_le32(bdata+4); memcpy(&b1,&u,4);
                        LAGFX_LOG("BACKUPD ref=0x%x addr=0x%llx off=%llu v0=[%.4g,%.4g]",
                                  vbs->ref, (unsigned long long)p->backing_update[bi].addr,
                                  (unsigned long long)vbs->offset, b0, b1);
                    }
                    free(bdata);
                    return bvb;
                }
            }
            free(bdata);
            break; /* update present but read failed → fall through to placement */
        }
    }
    uint8_t vtype = 0; uint64_t vva = 0, vgpa = 0;
    if (!lagfx_resolve_object_data(p, task, vbs->ref, &vtype, &vva, &vgpa) || vva == 0u)
        return VK_NULL_HANDLE;
    uint8_t vdesc[64];
    if (!lagfx_task_read_virtual(p, task, vva, sizeof(vdesc), vdesc))
        return VK_NULL_HANDLE;
    /* M2c sub-problem 1 (LAGFX_DUMP_SPV): dump the placement descriptor + resolve
     * so garbage/NaN vertex reads (ref=0x16/0x18) can be told apart from clean ones
     * (ref=0x14). Shows vva/vgpa + the 4 {size,pfn} entries that scatter-gather. */
    if (getenv("LAGFX_DUMP_SPV")) {
        LAGFX_LOG("VBDBG ref=0x%x type=%u off=%llu vva=0x%llx vgpa=0x%llx ents[%llu:0x%llx %llu:0x%llx %llu:0x%llx %llu:0x%llx]",
                  vbs->ref, vtype, (unsigned long long)vbs->offset,
                  (unsigned long long)vva, (unsigned long long)vgpa,
                  (unsigned long long)lagfx_le64(vdesc+0),  (unsigned long long)(lagfx_le64(vdesc+8)  & 0xffffffffull),
                  (unsigned long long)lagfx_le64(vdesc+16), (unsigned long long)(lagfx_le64(vdesc+24) & 0xffffffffull),
                  (unsigned long long)lagfx_le64(vdesc+32), (unsigned long long)(lagfx_le64(vdesc+40) & 0xffffffffull),
                  (unsigned long long)lagfx_le64(vdesc+48), (unsigned long long)(lagfx_le64(vdesc+56) & 0xffffffffull));
        /* M2c: RAW descriptor bytes — the working refs' (0x14) and broken refs'
         * (0x16/0x18) vva rows are only 12-72 B apart, so our 64 B read overlaps
         * multiple table rows; dump raw hex to decode the TRUE row framing. */
        char rh[200]; size_t rn = 0;
        for (size_t k = 0; k < 64u && rn + 3 < sizeof(rh); k++)
            rn += (size_t)snprintf(rh + rn, sizeof(rh) - rn, "%02x ", vdesc[k]);
        LAGFX_LOG("VBRAW ref=0x%x vva=0x%llx: %s", vbs->ref, (unsigned long long)vva, rh);
    }
    /* B8: LOGICAL scatter-gather walk (same as the descriptor-binding path) — the
     * placement descriptor's {size,PFN} entries tile the buffer's LOGICAL address
     * space; read vbs->offset by accumulating sizes and reading PFN<<12 + (offset -
     * range_start). The old first-non-zero-probe mis-bound to the wrong physical
     * range when an entry's page started with zero bytes (a leading-zero vertex
     * attribute) → garbage vertex data → degenerate geometry. Mask the PFN's high
     * 32 bits (flags live there for some object types).
     *
     * M2c: the gather now (a) spans MULTIPLE entries (LAGFX_M2_BIGVERTS needs the
     * full ~9.4 MB tile stream, which no single entry covers), and (b) caps each
     * chunk at the entry's remaining size — the old code read a flat 64 KiB from
     * the chosen entry regardless of its size, over-reading past small entries
     * into unrelated physical memory (a garbage-vertex source). Unread tail stays
     * zero-filled. Heap buffer (16 MiB is too big for the stack). */
    bool bigverts = getenv("LAGFX_M2_BIGVERTS") != NULL;
    uint32_t want = bigverts ? LAGFX_BIGVERTS_BUF_SZ : LAGFX_DRAW_DS_BUF_SZ;
    uint8_t *vdata = calloc(1u, want);
    if (!vdata)
        return VK_NULL_HANDLE;
    uint64_t vacc = 0, logical = vbs->offset;
    uint32_t filled = 0;
    for (int e = 0; e < 4 && filled < want; e++) {
        uint64_t rsize = lagfx_le64(vdesc + (size_t)e * 16u);
        uint64_t pfn = lagfx_le64(vdesc + (size_t)e * 16u + 8u) & 0xffffffffull;
        if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
        if (logical < vacc + rsize) {
            uint64_t within = logical - vacc;
            uint64_t avail = rsize - within;
            uint32_t chunk = (avail < (uint64_t)(want - filled))
                                 ? (uint32_t)avail : (want - filled);
            /* M2q: contract read — VA-translated when the address maps in the
             * per-task radix, raw GPA otherwise (see read_resource_backing). */
            uint64_t dva = (pfn << 12) + within;
            if (!lagfx_read_resource_backing(p, task, dva, chunk, vdata + filled)) {
                if (filled == 0u) { free(vdata); return VK_NULL_HANDLE; }
                break; /* partial gather: keep what we have, tail stays zero */
            }
            filled += chunk;
            logical += chunk;
        }
        vacc += rsize;
    }
    if (filled == 0u) { free(vdata); return VK_NULL_HANDLE; }
    VkBuffer vb = VK_NULL_HANDLE;
    if (lagfx_vk_make_host_storage_buffer(vk, vdata, want, &vb, out_mem) != LAGFX_OK) {
        free(vdata);
        return VK_NULL_HANDLE;
    }
    *out_size = want;
    /* Dump 6 vertices at the REAL stride 24 ([pos.xy@0, tex.xy@8, float@16],
     * stride = (20+7)&~7 = 24 per pipeline_build). Reveals whether the quad's
     * positions + texcoords span a real range or are degenerate (all ~one value
     * → the composite samples one texel → uniform output). Gated dump. */
    if (getenv("LAGFX_DUMP_SPV")) {
        for (int vtx = 0; vtx < 6; vtx++) {
            size_t bo = (size_t)vtx * 24u;
            if (bo + 24u > (size_t)want) break;
            float px, py, tx, ty, fz;
            uint32_t u;
            u = lagfx_le32(vdata+bo+0);  memcpy(&px,&u,4);
            u = lagfx_le32(vdata+bo+4);  memcpy(&py,&u,4);
            u = lagfx_le32(vdata+bo+8);  memcpy(&tx,&u,4);
            u = lagfx_le32(vdata+bo+12); memcpy(&ty,&u,4);
            u = lagfx_le32(vdata+bo+16); memcpy(&fz,&u,4);
            LAGFX_LOG("VTX24 ref=0x%x off=%llu v%d[pos %.4g,%.4g tex %.4g,%.4g f %.4g]",
                      vbs->ref, (unsigned long long)vbs->offset, vtx,
                      px, py, tx, ty, fz);
        }
        LAGFX_LOG("VBGATHER ref=0x%x want=%u filled=%u bigverts=%d",
                  vbs->ref, want, filled, bigverts ? 1 : 0);
    }
    free(vdata);
    return vb;
}

/* M1 (a) — shared resource-aware draw emission for ALL draw opcodes
 * (0x01/0x03/0x06/0x07). Previously only op_0x01 had the resource/descriptor
 * path; 0x03/0x06/0x07 (incl. the dominant 0x07, ~22439 occurrences) hardcoded
 * empty_layout + substitute. Factoring op_0x01's path here gives draw-op parity
 * so translated resource-using pipelines render through every draw selector.
 *
 * Builds (or reuses, via the B1 cache) the VkPipeline for the pending
 * pipeline+render-pass, then:
 *   - resource-using translated pipeline → bind the guest's storage buffers in a
 *     descriptor set matching the reflected layout, draw `count` vertices;
 *   - otherwise → the substitute triangle (suppressed for non-translated
 *     pipelines when LAGFX_NO_SUBSTITUTE is set, so real output isn't clobbered).
 * Any resolve/alloc/translate failure degrades to skip/substitute — never crashes.
 * `count` is the guest vertex_count (0x01/0x03) or index_count (0x06/0x07).
 * Indexed geometry is currently drawn unindexed: SkyLight's procedural-quad
 * vertex shaders compute corners from vertex_id and render correctly without an
 * index buffer; true index-buffer binding is a later M1 step (G5). */
void lagfx_emit_pending_draw(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                                    const char *op, uint32_t count) {
    if (!(task->pending_pipeline.valid && task->render_pass_desc.valid)) {
        return;
    }
    /* DRAW BISECTOR (diagnostic): LAGFX_SKIP_PIPES="0x74,0x76" skips drawing
     * the listed pipeline refs — attribute visible artifacts to their draw
     * family over the proven display. Pipeline refs are PER-BOOT dynamic, so
     * an env list goes stale on reboot; /tmp/lagfx_skip.txt (same format,
     * re-read every draw) allows live in-boot bisection via docker exec. */
    {
        char skipbuf[256];
        const char *skip = getenv("LAGFX_SKIP_PIPES");
        FILE *sf = fopen("/tmp/lagfx_skip.txt", "r");
        if (sf) {
            size_t sgot = fread(skipbuf, 1, sizeof(skipbuf) - 1, sf);
            fclose(sf);
            skipbuf[sgot] = '\0';
            if (sgot) skip = skipbuf;
        }
        if (skip) {
            char pref[16];
            snprintf(pref, sizeof(pref), "0x%x", task->pending_pipeline.reference);
            const char *hit = strstr(skip, pref);
            size_t pl = strlen(pref);
            if (hit && (hit[pl] == '\0' || hit[pl] == ',' || hit[pl] == '\n'))
                return;
        }
    }
    lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
    if (!(dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized)) {
        return;
    }
    VkDevice device = dev_with_vk->vk->device;
    /* M2c SOLIDFRAG (LAGFX_M2_SOLIDFRAG): split-test — draw the REAL translated
     * vertex geometry with the SUBSTITUTE solid-colour fragment. If colour
     * appears where black/teal persisted, the geometry+transform chain is
     * proven and the blocker is fragment-stage (texture alpha/blend/uniform);
     * if still nothing, the kill is vertex/raster-side. Diagnostic, gated. */
    bool solidfrag = getenv("LAGFX_M2_SOLIDFRAG") != NULL
                     && task->pending_pipeline.translated
                     && dev_with_vk->triangle_fragment_module != VK_NULL_HANDLE;
    lagfx_pipeline_desc_t pdesc = {
        .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
        .fragment_shader      = solidfrag
                                  ? dev_with_vk->triangle_fragment_module
                                  : (VkShaderModule)task->pending_pipeline.fragment_shader,
        /* Resource-using translated pipelines use their reflected pipeline
         * layout; substitute/resource-free use the device empty layout. */
        .layout               = (task->pending_pipeline.translated
                                  && task->pending_pipeline.pipeline_layout)
                                  ? (VkPipelineLayout)task->pending_pipeline.pipeline_layout
                                  : dev_with_vk->vk->empty_layout,
        .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
        .fragment_entry_point = (task->pending_pipeline.translated && !solidfrag)
                                  ? "main" : "triangle_fragment",
        .color_format         = (VkFormat)task->render_pass_desc.color_format,
        .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
    };
    /* Vertex input (gated by LAGFX_VTX_INPUT): feed the vertex shader's
     * reflected stage-in attributes from a bound guest vertex buffer. Without
     * this the positions read unbound → 0 → degenerate → black. */
    bool vtx_input_on = task->pending_pipeline.translated
                        && task->pending_pipeline.n_vtx_inputs > 0u
                        && LAGFX_POLICY("VTX_INPUT");
    if (vtx_input_on) {
        pdesc.n_vtx_inputs = task->pending_pipeline.n_vtx_inputs;
        pdesc.vtx_stride   = task->pending_pipeline.vtx_stride;
        /* MULTI-STREAM (GOAL-M2x): the upload interleaves the separate guest
         * streams at the tight reflected layout — force the pipeline onto the
         * same layout (vtx_stride=0 -> round8(attr sum)), NOT the PSO stride
         * (which describes the guest's separate stream layouts, not our
         * interleave). */
        if (getenv("LAGFX_DISABLE_MULTISTREAM") == NULL
            && lagfx_vtx_multi_stream(task))
            pdesc.vtx_stride = 0u;
        for (uint32_t a = 0; a < pdesc.n_vtx_inputs && a < 8u; a++) {
            pdesc.vtx_in_loc[a]  = task->pending_pipeline.vtx_in_loc[a];
            pdesc.vtx_in_comp[a] = task->pending_pipeline.vtx_in_comp[a];
        }
        /* The PSO blob omits the attribute-0 entry (offset 0 is serialized
         * implicitly): when the parsed list starts past 0 and we're exactly
         * one attr short, prepend the ground-truth position attr (Float4 @0 —
         * every lldb capture). Without this the 4-attr pipes fell back to
         * SFLOAT (NaN colors again) and 3-attr pipes bound shifted offsets. */
        {
            uint8_t na = task->pending_pipeline.n_vtx_attrs;
            const uint32_t *pf = task->pending_pipeline.vtx_attr_fmt;
            const uint32_t *po = task->pending_pipeline.vtx_attr_off;
            if (na > 0u && na < 8u && po[0] != 0u
                && na + 1u == pdesc.n_vtx_inputs) {
                pdesc.pso_attr_fmt[0] = 31u;  /* MTLVertexFormatFloat4 */
                pdesc.pso_attr_off[0] = 0u;
                for (uint32_t a = 0; a < na; a++) {
                    pdesc.pso_attr_fmt[a + 1u] = pf[a];
                    pdesc.pso_attr_off[a + 1u] = po[a];
                }
                pdesc.n_pso_attrs = (uint8_t)(na + 1u);
            } else if (na > 0u && po[0] == 0u) {
                pdesc.n_pso_attrs = na;
                for (uint32_t a = 0; a < na && a < 8u; a++) {
                    pdesc.pso_attr_fmt[a] = pf[a];
                    pdesc.pso_attr_off[a] = po[a];
                }
            } else {
                pdesc.n_pso_attrs = 0u;  /* misaligned decode — fall back */
            }
        }
    }
    VkPipeline pipeline = lagfx_get_cached_pipeline(task, device, &pdesc);
    if (pipeline == VK_NULL_HANDLE) {
        LAGFX_WARN("%s: pipeline build failed", op);
        return;
    }
    lagfx_display_t *display = dev_with_vk->displays[0];
    if (!(display && display->rt_ready && display->rt.image != VK_NULL_HANDLE)) {
        LAGFX_WARN("%s: no render target available", op);
        return;
    }
    /* M2 PER-PASS RT (LAGFX_M2_PERPASS): if this render pass targets a non-scanout
     * surface (render_pass_desc.target_ref != 0), route its draws INTO that
     * surface's VkImage instead of the single display->rt. A later composite that
     * SAMPLES the same surface then reads real rendered content (the wallpaper /
     * intermediate layer) instead of black. The target IOSurface is created on
     * first use, sized to the render area, and registered in the resource registry
     * under target_ref so the texture-bind path finds it when sampled. The
     * scanout (target_ref==0) keeps using display->rt. Gated; on any failure,
     * fall back to display->rt (never crash). */
    lagfx_vk_render_target_t perpass_rt;
    lagfx_vk_render_target_t *active_rt = &display->rt;
    /* M2a SCANOUT-ASSEMBLY (LAGFX_M2_ASMBLIT): track the per-pass surface this
     * draw renders into, so that after a successful per-pass draw we can blit it
     * to display->rt (the scanned-out target). Per-pass routing lands every pass
     * in an intermediate → nothing reaches the scanout → black. This makes the
     * last-rendered per-pass surface's content visible. */
    lagfx_vk_iosurface_t *perpass_ios = NULL;
    uint32_t perpass_tref = 0u; (void)perpass_tref;
    bool perpass_real_draw = false;  /* set only when a TRANSLATED draw lands in it */
    if (LAGFX_POLICY("M2_PERPASS") && task->render_pass_desc.target_ref != 0u) {
        uint32_t tref = task->render_pass_desc.target_ref;
        lagfx_resource_entry_t *te = lagfx_resource_lookup_texture(&p->resources, tref);
        lagfx_vk_iosurface_t *ios = te ? (lagfx_vk_iosurface_t *)te->host_handle : NULL;
        /* M2q GROUND-TRUTH PROBE (LAGFX_M2_PPPROBE, log-only): does the pass
         * target ref's guest BACKING hold real content (the forest), or is it
         * a pure GPU render target we must draw into? Resolve the ref, read its
         * type-0x03-style placement backing BOTH ways (raw GPA + VA-translated),
         * report type/size/nonblack/photo for each — the single discriminator
         * between "sample its backing" and "our draws must produce it". */
        if (getenv("LAGFX_M2_PPPROBE")) {
            uint8_t pt = 0; uint64_t pva = 0, pgpa = 0;
            if (lagfx_resolve_object_data(p, task, tref, &pt, &pva, &pgpa) && pva) {
                uint8_t pd[64] = {0};
                if (lagfx_task_read_virtual(p, task, pva, sizeof(pd), pd)) {
                    uint64_t tot = 0;
                    for (int e = 0; e < 4; e++) {
                        uint64_t rs = lagfx_le64(pd + (size_t)e*16u);
                        uint64_t pf = lagfx_le64(pd + (size_t)e*16u+8u) & 0xffffffffull;
                        if (pf >= 0x10u && pf <= 0xfffffu && rs) tot += rs;
                    }
                    uint32_t cap = tot > 5u*1024u*1024u ? 5u*1024u*1024u : (uint32_t)tot;
                    if (cap >= 4096u) {
                        uint8_t *rb = calloc(1u, cap), *vb = calloc(1u, cap);
                        const char *h0="none", *h1="none";
                        uint32_t g0 = rb ? lagfx_read_vtx_source(p, task, tref, 0u, cap, rb, &h0, 0) : 0;
                        uint32_t g1 = vb ? lagfx_read_vtx_source(p, task, tref, 0u, cap, vb, &h1, 1) : 0;
                        uint32_t nb0=0, nb1=0;
                        for (uint32_t o=0;o+4u<=cap;o+=4u){ if(rb&&(rb[o]|rb[o+1]|rb[o+2]))nb0++; if(vb&&(vb[o]|vb[o+1]|vb[o+2]))nb1++; }
                        LAGFX_LOG("PPPROBE tref=0x%x type=0x%02x tot=%llu cap=%u raw(%s g=%u nb=%u) va(%s g=%u nb=%u)",
                                  tref, pt, (unsigned long long)tot, cap,
                                  h0, g0, nb0, h1, g1, nb1);
                        /* LAGFX_M2_PPDUMP: persist both reads so the bytes can be
                         * rendered host-side — counts can't distinguish forest
                         * from boot-log garbage; only an image can. */
                        if (getenv("LAGFX_M2_PPDUMP")) {
                            char fn[64];
                            snprintf(fn, sizeof(fn), "/tmp/pp-%x-raw.bin", tref);
                            FILE *f = rb ? fopen(fn, "wb") : NULL;
                            if (f) { fwrite(rb, 1, cap, f); fclose(f); }
                            snprintf(fn, sizeof(fn), "/tmp/pp-%x-va.bin", tref);
                            f = vb ? fopen(fn, "wb") : NULL;
                            if (f) { fwrite(vb, 1, cap, f); fclose(f); }
                        }
                        free(rb); free(vb);
                    } else {
                        LAGFX_LOG("PPPROBE tref=0x%x type=0x%02x tot=%llu (too small)",
                                  tref, pt, (unsigned long long)tot);
                    }
                } else {
                    LAGFX_LOG("PPPROBE tref=0x%x type=0x%02x va=0x%llx desc-read-fail",
                              tref, pt, (unsigned long long)pva);
                }
            } else {
                LAGFX_LOG("PPPROBE tref=0x%x unresolved", tref);
            }
        }
        if (!ios) {
            /* DUMB-FAITHFUL (M2q): the pass target IS the guest texture — its
             * memory may already hold real content (ref 0x17's backing holds
             * the decoded wallpaper). SEED the render target from the declared
             * backing via texture_realize, so a later pass that SAMPLES the
             * same ref reads the guest's real bytes plus whatever we rendered
             * (loadOp=LOAD preserves the seed). The old empty invented RT
             * registered under the ref SHADOWED the real backing → black. */
            ios = lagfx_texture_realize(p, task, dev_with_vk->vk, tref);
            if (ios) {
                if (task->vp_valid && !ios->dst_valid) {
                    ios->dst_x = task->vp_x; ios->dst_y = task->vp_y;
                    ios->dst_w = task->vp_w; ios->dst_h = task->vp_h;
                    ios->dst_valid = 1u;
                }
                LAGFX_LOG("%s PERPASS: seeded RT from backing ref=0x%x %ux%u",
                          op, tref, ios->width, ios->height);
            }
        }
        if (!ios) {
            /* No realizable backing — size the per-pass surface from the guest's
             * authoritative source: render_area (usually 0), else the scissor
             * (0x75, real draw region), else the scanout default. */
            uint32_t W = task->render_pass_desc.render_area_w ? task->render_pass_desc.render_area_w
                       : (task->scissor_w ? task->scissor_w : 1920u);
            uint32_t H = task->render_pass_desc.render_area_h ? task->render_pass_desc.render_area_h
                       : (task->scissor_h ? task->scissor_h : 1080u);
            if (W < 16u) W = 1920u; if (H < 16u) H = 1080u;
            if (lagfx_vk_iosurface_create(dev_with_vk->vk, W, H, 80u, &ios) == LAGFX_OK && ios) {
                lagfx_resource_register(&p->resources, tref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                        task->id, 0u, 0u);
                lagfx_resource_entry_t *ne = lagfx_resource_lookup_texture(&p->resources, tref);
                if (ne) {
                    ne->host_handle = ios; ne->image = ios->image; ne->view = ios->view;
                    /* Stamp the layer's declared destination rect (0x82
                     * viewport) so the composite blit places it at the guest's
                     * real offset (clock/UI land correctly) instead of a
                     * full-screen stretch. */
                    if (task->vp_valid) {
                        ios->dst_x = task->vp_x; ios->dst_y = task->vp_y;
                        ios->dst_w = task->vp_w; ios->dst_h = task->vp_h;
                        ios->dst_valid = 1u;
                    }
                    LAGFX_LOG("%s PERPASS: created RT IOSurface for target ref=0x%x %ux%u vp=%ux%u@(%u,%u) valid=%u",
                              op, tref, W, H, task->vp_w, task->vp_h,
                              task->vp_x, task->vp_y, task->vp_valid);
                } else {
                    /* Registry register failed — without an owning entry the
                     * image would leak and be recreated per draw. Destroy and
                     * fall through to display->rt. */
                    LAGFX_WARN("%s PERPASS: registry register failed for ref=0x%x — surface destroyed",
                               op, tref);
                    lagfx_vk_iosurface_destroy(dev_with_vk->vk, ios);
                    ios = NULL;
                }
            }
        }
        if (ios && ios->image != VK_NULL_HANDLE
            && lagfx_vk_render_target_wrap(ios->image, ios->view, ios->memory,
                                           ios->width, ios->height, ios->format,
                                           &perpass_rt) == LAGFX_OK) {
            active_rt = &perpass_rt;
            ios->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; /* sampleable after render */
            perpass_ios = ios;      /* M2a: candidate for the scanout-assembly blit */
            perpass_tref = tref;
        }
    }
    bool resource_using = task->pending_pipeline.translated
                          && task->pending_pipeline.descriptor_set_layout != 0
                          && task->pending_pipeline.pipeline_layout != 0;
    lagfx_status_t st = LAGFX_OK;

    /* NOTE: the earlier "guest bound a texture but reflection has no
     * SAMPLED_IMAGE → skip" gate was REMOVED — it was backwards. A shader whose
     * reflected SPIR-V has NO SAMPLED_IMAGE does NOT sample a texture (e.g.
     * ColorFill, which just fills a colour from a buffer), so a stale guest
     * texture binding is irrelevant and the draw is safe — skipping it wrongly
     * dropped renderable colour-fill pipelines. The real texture-sample crash
     * (a REFLECTED texture whose IOSurface isn't sampleable) is handled in
     * lagfx_build_draw_descriptor_set, which skips only when a texture the
     * shader actually samples can't be bound. With the translator static-init
     * fix, composite shaders now reflect their textures and go through that
     * path. */

    /* Conservative crash guard (LAGFX_M2_SIMPLE_DRAW): the multi-buffer
     * COMPOSITE pipelines still translate incompletely (live shaders drop some
     * vertex-input/texture handling beyond the static-init fix) and SIGSEGV
     * lavapipe with real data. The SIMPLE colour-fill pipelines (ColorFill =
     * pipeline 0xd: exactly 1 storage buffer, no stage-in complications) render
     * correctly. Until the composite translation is complete, draw only the
     * simple (≤1 storage binding) pipelines so the system is stable AND
     * ColorFill's real colour output is visible. Off by default. */
    /* (LAGFX_M2_SIMPLE_DRAW was a broken heuristic — the pipeline reflection
     * MERGES vertex + fragment bindings, so even ColorFill reflects 3 buffers
     * and it skipped everything. Removed; the storage-buffer SIZING fix below
     * — buffers allocated to their declared size — addresses the real crash
     * (a shader's dynamic index reading past a fixed 64 KiB buffer).) */
    (void)resource_using;

    /* Vertex-input upload — SHARED by the resource-using and resource-free
     * translated paths. A vertex shader with stage-in attributes may be
     * resource-free (no [[buffer]] descriptors) — e.g. SkyLight ref=0x14/0x1d
     * have 3 stage-in attrs but no storage buffers — so the upload must NOT
     * live only inside the descriptor-set branch (that was the vtx_bound=0 bug:
     * resource-free vtx pipelines fell through to the substitute triangle).
     * stride = sum(comp*4) (tightly-packed, matching pipeline_build). */
    uint32_t vstride = 16u;
    uint32_t vbuf_size = LAGFX_DRAW_DS_BUF_SZ;
    VkBuffer vbuf = VK_NULL_HANDLE; VkDeviceMemory vbmem = VK_NULL_HANDLE;
    if (vtx_input_on) {
        vstride = 0u;
        for (uint32_t a = 0; a < task->pending_pipeline.n_vtx_inputs && a < 8u; a++)
            vstride += (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
        /* Use the REAL per-pipeline stride (decoded from the PSO descriptor) when
         * known, so the vertex-count cap below (vc = BUF_SZ/stride) matches how
         * pipeline_build reads the buffer. The tight-packed sum under-counts the
         * stride (48→24) → an over-large vc that reads past valid vertices. */
        if (task->pending_pipeline.vtx_stride >= vstride
            && task->pending_pipeline.vtx_stride <= 256u)
            vstride = task->pending_pipeline.vtx_stride;
        if (vstride == 0u) vstride = 16u;
        vbuf = lagfx_upload_guest_vertex_buffer(p, task, dev_with_vk->vk, &vbmem,
                                                &vbuf_size);
    }
    /* M2: real data makes the shader actually fetch; cap the vertex count to
     * what the 4096B upload holds (BUF_SZ/stride) so a vertex_id index stays in
     * bounds — else a hard SIGSEGV inside lavapipe (mid-draw in Mesa, uncatchable
     * by our fallback). Env-tunable. The guest's huge counts (0x60000) are also
     * likely a wire misparse to fix. */
    uint32_t vc = count ? count : 3u;
    /* Hard buffer-safe cap: a vertex fetch reads vc*vstride bytes from the
     * 64 KiB vertex buffer; vc > BUF_SZ/vstride reads OOB → lavapipe SIGSEGV.
     * LAGFX_MAX_DRAW_VERTS may only LOWER this, never raise it past safe. */
    /* M2c BIGVERTS: the cap scales with the ACTUAL allocated vertex buffer
     * (vbuf_size = 16 MiB under LAGFX_M2_BIGVERTS, 64 KiB default) — buffer grown
     * BEFORE the cap is raised, so a vertex fetch can never read OOB. */
    uint32_t vmax = vbuf_size / vstride;
    const char *vcap = getenv("LAGFX_MAX_DRAW_VERTS");
    if (vcap) { unsigned long v = strtoul(vcap, NULL, 0); if (v && (uint32_t)v < vmax) vmax = (uint32_t)v; }
    if (vc > vmax) vc = vmax;

    /* Per-draw correlation: task, target view, route (perpass vs scanout),
     * pipeline ref, translate + resource status. Maps the wallpaper pass's
     * draw (target view 0x17/0x32) to its execution outcome. Gated. */
    if (getenv("LAGFX_DRAWMAP")) {
        LAGFX_LOG("DRAWMAP t%u tgt=0x%x route=%s pipe=0x%x xlated=%d res_using=%d "
                  "nbind=%u nvtx=%u verts=%u scissor=%ux%u",
                  task->id, task->render_pass_desc.target_ref,
                  (active_rt == &perpass_rt) ? "perpass" : "scanout",
                  task->pending_pipeline.reference,
                  task->pending_pipeline.translated ? 1 : 0,
                  resource_using ? 1 : 0,
                  task->pending_pipeline.n_spv_bindings,
                  task->pending_pipeline.n_vtx_inputs, vc,
                  task->scissor_w, task->scissor_h);
    }

    /* PASSGRAPH: the dependency DAG. Each draw WRITES target_ref and READS the
     * bound fragment_textures — dump both edges so the forest-source chain and
     * the terminal (target=scanout) pass are visible in one boot. Gated. */
    if (getenv("LAGFX_PASSGRAPH")) {
        char rd[128]; size_t rn = 0; uint32_t nread = 0;
        for (uint32_t s = 0; s < LAGFX_MAX_BINDING_SLOTS && rn + 8 < sizeof(rd); s++) {
            if (!task->bindings.fragment_textures[s].valid) continue;
            rn += (size_t)snprintf(rd + rn, sizeof(rd) - rn, "0x%x,",
                                   task->bindings.fragment_textures[s].ref);
            nread++;
        }
        if (rn == 0) { rd[0] = '-'; rd[1] = 0; }
        LAGFX_LOG("PASSGRAPH t%u WRITES tgt=0x%x pipe=0x%x xlated=%d READS[%u]=%s scissor=%ux%u",
                  task->id, task->render_pass_desc.target_ref,
                  task->pending_pipeline.reference,
                  task->pending_pipeline.translated ? 1 : 0,
                  nread, rd, task->scissor_w, task->scissor_h);
    }

    /* Guest per-draw viewport + scissor (KICKOFF-shading-throughput): the
     * rects the guest declared for THIS pass (0x82/0x75, reset at 0x1a).
     * Without them every draw mapped NDC onto the full RT and shaded every
     * RT pixel — small scissored washes cost 450x their real pixel count
     * (the 20-30s draws). Kill-switch LAGFX_DISABLE_GUEST_VPSC. */
    lagfx_draw_region_t region = {0};
    bool have_region = false;
    if (getenv("LAGFX_DISABLE_GUEST_VPSC") == NULL) {
        if (task->vp_current) {
            region.vp_x = task->vp_x; region.vp_y = task->vp_y;
            region.vp_w = task->vp_w; region.vp_h = task->vp_h;
            have_region = true;
        }
        if (task->sc_have) {
            region.sc_x = task->sc_x; region.sc_y = task->sc_y;
            region.sc_w = task->sc_w; region.sc_h = task->sc_h;
            have_region = true;
        }
    }
    const lagfx_draw_region_t *regionp = have_region ? &region : NULL;

    if (resource_using) {
        /* Bind the guest's storage buffers into a descriptor set matching the
         * reflected layout, then draw the guest geometry. Failure → skip (no crash). */
        VkBuffer tbuf[16]; VkDeviceMemory tmem[16]; uint32_t ntr = 0;
        VkDescriptorSet ds = lagfx_build_draw_descriptor_set(
            p, task, dev_with_vk->vk,
            (VkDescriptorSetLayout)task->pending_pipeline.descriptor_set_layout,
            task->pending_pipeline.spv_binding_no,
            task->pending_pipeline.spv_binding_kind,
            task->pending_pipeline.n_spv_bindings,
            tbuf, tmem, &ntr);
        if (ds != VK_NULL_HANDLE) {
            /* With vertex input on but no real vertex data, SKIP the draw
             * (st != OK) to avoid the unbound-attribute lavapipe fault — don't
             * signal a frame for a draw that didn't happen. */
            if (vtx_input_on && vbuf == VK_NULL_HANDLE) {
                st = LAGFX_ERR_INVALID_ARG;
            } else {
                st = lagfx_vk_draw_record_and_submit_bound(
                    dev_with_vk->vk, pipeline,
                    (VkPipelineLayout)task->pending_pipeline.pipeline_layout, ds,
                    active_rt, false, vc, 1, 0, 0, 0, vbuf, regionp);
            }
            if (st == LAGFX_OK) {
                LAGFX_LOG("%s P6b: drew TRANSLATED resource pipeline ref=0x%x verts=%u bindings=%u",
                          op, task->pending_pipeline.reference, vc,
                          task->pending_pipeline.n_spv_bindings);
                if (active_rt == &perpass_rt) {
                    perpass_real_draw = true;
                    if (perpass_ios) perpass_ios->gpu_drawn = 1u;
                }
                if (getenv("LAGFX_FRAME_ON_SWAP") == NULL) lagfx_display_signal_frame_ready(display);
            } else {
                LAGFX_WARN("%s P6b: bound draw failed (%d)", op, (int)st);
            }
            /* LAGFX_DUMP_SLOW_BINDS=<ms>: the descriptor build snapshotted
             * this draw's storage windows into task->slowsnap; if the fence
             * wait (set by draw_record even on timeout) exceeded the
             * threshold, flush them — the runaway draws' exact inputs for
             * the offline xgc-replay harness. Cap 16 dump sets per boot. */
            {
                const char *sb = getenv("LAGFX_DUMP_SLOW_BINDS");
                if (sb && task->slowsnap && task->slowsnap_n) {
                    uint32_t thresh = (uint32_t)strtoul(sb, NULL, 10);
                    if (thresh < 100u) thresh = 1000u;
                    struct lagfx_vk_state *vks = dev_with_vk->vk;
                    if (vks->last_draw_wait_ms >= thresh
                        && vks->slow_bind_dumps < 16u) {
                        uint32_t seq = vks->slow_bind_dumps++;
                        (void)system("mkdir -p /tmp/lagfx-binds-slow");
                        for (uint32_t w = 0; w < task->slowsnap_n; w++) {
                            char bp[128];
                            snprintf(bp, sizeof(bp),
                                     "/tmp/lagfx-binds-slow/d%02u-%ums-pipe0x%x-bind%u.bin",
                                     seq, vks->last_draw_wait_ms,
                                     task->pending_pipeline.reference,
                                     task->slowsnap_bindno[w]);
                            FILE *bf = fopen(bp, "wb");
                            if (bf) {
                                fwrite(task->slowsnap + (size_t)w * LAGFX_DRAW_DS_BUF_SZ,
                                       1, LAGFX_DRAW_DS_BUF_SZ, bf);
                                fclose(bf);
                            }
                        }
                        LAGFX_LOG("SLOWBINDS: dumped set d%02u (%u windows, %ums, "
                                  "pipe=0x%x tgt=0x%x verts=%u)",
                                  seq, task->slowsnap_n, vks->last_draw_wait_ms,
                                  task->pending_pipeline.reference,
                                  task->render_pass_desc.target_ref, vc);
                    }
                }
            }
            for (uint32_t k = 0; k < ntr; k++) {
                vkDestroyBuffer(device, tbuf[k], NULL);
                vkFreeMemory(device, tmem[k], NULL);
            }
            vkFreeDescriptorSets(device, dev_with_vk->vk->draw_desc_pool, 1, &ds);
        } else {
            LAGFX_WARN("%s P6b: descriptor-set build failed for ref=0x%x — skipping draw (no crash)",
                       op, task->pending_pipeline.reference);
        }
    } else if (vtx_input_on && vbuf != VK_NULL_HANDLE) {
        /* Resource-free TRANSLATED pipeline WITH vertex input: real geometry
         * from the bound vertex buffer, no descriptor set (empty layout). This
         * is the SkyLight ref=0x14/0x1d case — stage-in attrs, no [[buffer]]. */
        st = lagfx_vk_draw_record_and_submit_bound(
            dev_with_vk->vk, pipeline, VK_NULL_HANDLE, VK_NULL_HANDLE,
            active_rt, false, vc, 1, 0, 0, 0, vbuf, regionp);
        if (st == LAGFX_OK) {
            LAGFX_LOG("%s VTX: drew TRANSLATED vertex-input pipeline ref=0x%x verts=%u stride=%u",
                      op, task->pending_pipeline.reference, vc, vstride);
            if (active_rt == &perpass_rt) {
                perpass_real_draw = true;
                if (perpass_ios) perpass_ios->gpu_drawn = 1u;
            }
            if (getenv("LAGFX_FRAME_ON_SWAP") == NULL) lagfx_display_signal_frame_ready(display);
        } else {
            LAGFX_WARN("%s VTX: vertex-input draw failed (%d)", op, (int)st);
        }
    } else if (vtx_input_on) {
        /* vtx-input pipeline but no real vertex data → skip (no unbound fault). */
        LAGFX_LOG("%s VTX: ref=0x%x no real vertex data — skip (no crash)",
                  op, task->pending_pipeline.reference);
    } else if ((getenv("LAGFX_SUBSTITUTE") != NULL)
               && !(LAGFX_POLICY("M2_ASMBLIT") && active_rt == &perpass_rt)) {
        /* Substitute / resource-free path: bundled 3-vertex triangle.
         * NO_SUBSTITUTE skips this ENTIRELY (translated or not): a TRANSLATED
         * pipeline that fell through here (no descriptor layout, no vtx input)
         * still drew a full-screen WHITE substitute triangle via the white-fill
         * fallback, clobbering the real translated composite draws (e.g. the
         * wallpaper ref=0x19) underneath. Suppressing it lets the real content
         * show. (The old guard only skipped substitutes for UNtranslated
         * pipelines — that was the white-screen-over-wallpaper bug.)
         * M2a: additionally, under ASMBLIT, DON'T draw the substitute into a
         * per-pass surface — it would pollute the surface with a fake triangle
         * and (being the last draw) win the scanout blit over the real composite
         * layers. The substitute still renders to display->rt directly. */
        st = lagfx_vk_draw_record_and_submit(
            dev_with_vk->vk, pipeline, active_rt, false, 3, 1, 0, 0, 0);
        if (st == LAGFX_OK) {
            LAGFX_LOG("%s: drew substitute triangle (guest req count=%u)", op, count);
            if (getenv("LAGFX_FRAME_ON_SWAP") == NULL) lagfx_display_signal_frame_ready(display);
        } else {
            LAGFX_WARN("%s: substitute draw failed (%d)", op, (int)st);
        }
    }

    /* Scanout assembly: a per-pass draw records its surface in draw order
     * (dedup, first-draw position) and re-blits the WHOLE ordered set into
     * display->rt — earlier passes first, this pass last. Re-blitting all of
     * them each draw keeps a later near-empty surface from wiping an earlier
     * content-bearing one, and does not depend on a trailing display present
     * (the guest may issue none after the compositing burst). The queue is
     * reset at the next present (soft frame boundary). */
    if (LAGFX_POLICY("M2_ASMBLIT") && active_rt == &perpass_rt
        && perpass_ios && perpass_ios->image != VK_NULL_HANDLE
        && perpass_real_draw && st == LAGFX_OK) {
        bool queued = false;
        for (uint32_t qi = 0; qi < p->frame_blit_n; qi++)
            if (p->frame_blit_queue[qi] == (uintptr_t)perpass_ios) { queued = true; break; }
        if (!queued && p->frame_blit_n < 16u)
            p->frame_blit_queue[p->frame_blit_n++] = (uintptr_t)perpass_ios;

        /* DUMB-FAITHFUL (M2q): re-composite the queue in the guest's SUBMIT
         * ORDER. Layer 0 (the earliest pass, the background) replace-blits;
         * every later layer composites src-over ON TOP at its declared dst
         * rect — a later opaque-black or empty layer must never WIPE an
         * earlier content-bearing one (the old default replace-blitted every
         * layer full-screen, so the last pass always won the whole frame).
         * Kill-switch: LAGFX_DISABLE_M2_OVERLAY reverts upper layers to
         * replace-blit. */
        bool overlay = LAGFX_POLICY("M2_OVERLAY");
        /* LIVE CLEAR-ALL (GOAL-M2z): /tmp/lagfx_clearall.txt wipes the
         * queued PER-PASS surfaces (stale burst content the overlay re-stamps
         * forever — RT clears can't reach it). Consumed once. */
        {
            FILE *caf = fopen("/tmp/lagfx_clearall.txt", "r");
            if (caf) {
                fclose(caf);
                remove("/tmp/lagfx_clearall.txt");
                for (uint32_t qi = 0; qi < p->frame_blit_n && qi < 16u; qi++) {
                    lagfx_vk_iosurface_t *cios =
                        (lagfx_vk_iosurface_t *)p->frame_blit_queue[qi];
                    if (!cios || cios->image == VK_NULL_HANDLE) continue;
                    lagfx_vk_clear_image(dev_with_vk->vk, cios->image, &cios->layout);
                }
                LAGFX_LOG("ASMBLIT: CLEARALL consumed — %u per-pass surfaces wiped",
                          p->frame_blit_n);
            }
        }
        for (uint32_t qi = 0; qi < p->frame_blit_n && qi < 16u; qi++) {
            lagfx_vk_iosurface_t *qios =
                (lagfx_vk_iosurface_t *)p->frame_blit_queue[qi];
            if (!qios || qios->image == VK_NULL_HANDLE) continue;
            uint32_t px = 0, py = 0, pw = 0, ph = 0;
            if (qios->dst_valid) {
                px = qios->dst_x; py = qios->dst_y;
                pw = qios->dst_w; ph = qios->dst_h;
            }
            /* OFFSCREEN-PARKED WINDOWS (GOAL-M2z): the guest hides windows by
             * placing them BEYOND its scanout (dst_x >= scanout_width, e.g.
             * x=1280 on a 1280-wide mode). Our display->rt is larger (1920),
             * so those layers rendered visibly in the right/bottom gutter —
             * the persistent stripe block at x>=1280. Skip layers parked
             * fully outside the guest scanout. */
            if (qios->dst_valid && display->scanout_width
                && (px >= display->scanout_width
                    || py >= display->scanout_height)) {
                if (getenv("LAGFX_DUMP_SPV"))
                    LAGFX_LOG("ASMBLIT layer %u PARKED offscreen dst=(%u,%u %ux%u) — skipped",
                              qi, px, py, pw, ph);
                continue;
            }
            if (getenv("LAGFX_DUMP_SPV"))
                LAGFX_LOG("ASMBLIT layer %u dst=(%u,%u %ux%u) valid=%d",
                          qi, px, py, pw, ph, qios->dst_valid ? 1 : 0);
            /* Layer 0 ALSO composites src-over (was: replace-blit). DRAWMAP proved
             * content draws split between per-pass targets (0x17) AND direct-to-
             * scanout (display->rt: pipes 0x23/0x4e/0x5b). The old layer-0 replace-
             * blit of the per-pass 0x17 surface WIPED the direct-scanout content →
             * black frame. composite_over uses LOAD_OP_LOAD + src-over, so it
             * preserves whatever is already in display->rt (the direct draws) and
             * layers the per-pass content on top. Kill-switch DISABLE_M2_OVERLAY
             * still reverts to the old full replace-blit. */
            if (overlay && qios->view != VK_NULL_HANDLE) {
                lagfx_vk_composite_over(dev_with_vk->vk, &display->rt, qios->view,
                                        px, py, pw, ph);
            } else {
                lagfx_vk_display_present_surface(
                    dev_with_vk->vk, &display->rt, qios->image, &qios->layout,
                    qios->width, qios->height, display->rt.width, display->rt.height,
                    display->scanout_gpa, display->scanout_length,
                    dev_with_vk->desc.shell.opaque, dev_with_vk->desc.shell.write_memory,
                    px, py, pw, ph);
            }
        }
        lagfx_display_submit_rendered_frame(display, display->scanout_gpa,
                                            display->scanout_length);
        LAGFX_LOG("%s ASMBLIT: %u layers -> display->rt (overlay=%d)",
                  op, p->frame_blit_n, overlay ? 1 : 0);
        /* Cursor on top of the composited set — read_frame serves display->rt
         * directly on the sparse-present guest, so overlay here too. */
        (void)lagfx_display_overlay_cursor(display);
        if (getenv("LAGFX_FRAME_ON_SWAP") == NULL) lagfx_display_signal_frame_ready(display);
    }

    if (vbuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vbuf, NULL);
        vkFreeMemory(device, vbmem, NULL);
    }
}

#endif /* LAGFX_HAVE_VULKAN */
