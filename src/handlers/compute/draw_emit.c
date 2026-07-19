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
        for (uint32_t sm = 0; sm < 6u; sm++) {
            /* slot-major, then mode: s0/m0, s0/m1, s1/m0, s1/m1, s2/m0, s2/m1 —
             * mode 1 = VA-translated placement read (lead 3). */
            uint32_t s = sm / 2u;
            int mode = (int)(sm % 2u);
            lagfx_binding_slot_t *cs = &task->bindings.vertex_buffers[s];
            if (!cs->valid || cs->ref == 0u) continue;
            uint8_t sample[256];
            const char *how = "none";
            uint32_t got = lagfx_read_vtx_source(p, task, cs->ref, cs->offset,
                                                 sizeof(sample), sample, &how, mode);
            uint32_t score = got ? lagfx_vtx_float_plausibility(sample, got) : 0u;
            if (getenv("LAGFX_DUMP_SPV"))
                LAGFX_LOG("VTXSRC slot=%u mode=%d ref=0x%x off=%llu how=%s score=%u",
                          s, mode, cs->ref, (unsigned long long)cs->offset, how, score);
            if (score < 50u) continue;
            uint8_t *full = calloc(1u, want);
            if (!full) break;
            uint32_t ffill = lagfx_read_vtx_source(p, task, cs->ref, cs->offset,
                                                   want, full, &how, mode);
            if (ffill) {
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
            /* SAME root cause as the texture backing: the placement descriptor's
             * PFN is a guest-PHYSICAL frame, so dva=(pfn<<12)+off is a GPA — read
             * it raw (gated LAGFX_M2_RAWGPA), NOT VA-translated, else the vertex
             * positions come back garbage/zero → degenerate geometry. */
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
        for (uint32_t a = 0; a < pdesc.n_vtx_inputs && a < 8u; a++) {
            pdesc.vtx_in_loc[a]  = task->pending_pipeline.vtx_in_loc[a];
            pdesc.vtx_in_comp[a] = task->pending_pipeline.vtx_in_comp[a];
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
        if (!ios) {
            uint32_t W = task->render_pass_desc.render_area_w ? task->render_pass_desc.render_area_w : 1920u;
            uint32_t H = task->render_pass_desc.render_area_h ? task->render_pass_desc.render_area_h : 1080u;
            if (W < 16u) W = 1920u; if (H < 16u) H = 1080u;
            if (lagfx_vk_iosurface_create(dev_with_vk->vk, W, H, 80u, &ios) == LAGFX_OK && ios) {
                lagfx_resource_register(&p->resources, tref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                        task->id, 0u, 0u);
                lagfx_resource_entry_t *ne = lagfx_resource_lookup_texture(&p->resources, tref);
                if (ne) {
                    ne->host_handle = ios; ne->image = ios->image; ne->view = ios->view;
                    LAGFX_LOG("%s PERPASS: created RT IOSurface for target ref=0x%x %ux%u", op, tref, W, H);
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
                    active_rt, false, vc, 1, 0, 0, 0, vbuf);
            }
            if (st == LAGFX_OK) {
                LAGFX_LOG("%s P6b: drew TRANSLATED resource pipeline ref=0x%x verts=%u bindings=%u",
                          op, task->pending_pipeline.reference, vc,
                          task->pending_pipeline.n_spv_bindings);
                if (active_rt == &perpass_rt) perpass_real_draw = true;
                lagfx_display_signal_frame_ready(display);
            } else {
                LAGFX_WARN("%s P6b: bound draw failed (%d)", op, (int)st);
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
            active_rt, false, vc, 1, 0, 0, 0, vbuf);
        if (st == LAGFX_OK) {
            LAGFX_LOG("%s VTX: drew TRANSLATED vertex-input pipeline ref=0x%x verts=%u stride=%u",
                      op, task->pending_pipeline.reference, vc, vstride);
            if (active_rt == &perpass_rt) perpass_real_draw = true;
            lagfx_display_signal_frame_ready(display);
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
            lagfx_display_signal_frame_ready(display);
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

        for (uint32_t qi = 0; qi < p->frame_blit_n && qi < 16u; qi++) {
            lagfx_vk_iosurface_t *qios =
                (lagfx_vk_iosurface_t *)p->frame_blit_queue[qi];
            if (!qios || qios->image == VK_NULL_HANDLE) continue;
            /* Background (qi 0) replace-blits into the rt (no scanout yet);
             * upper layers MAX-blend composite so bright UI/clock overlays the
             * wallpaper and near-black layer regions keep it. */
            if (qi == 0u) {
                lagfx_vk_display_present_surface(
                    dev_with_vk->vk, &display->rt, qios->image, &qios->layout,
                    qios->width, qios->height, display->rt.width, display->rt.height,
                    0u, 0u,
                    dev_with_vk->desc.shell.opaque, dev_with_vk->desc.shell.write_memory);
            } else if (qios->view != VK_NULL_HANDLE
                       && !getenv("LAGFX_BG_ONLY")) {
                lagfx_vk_composite_over(dev_with_vk->vk, &display->rt, qios->view);
            }
        }
        /* Carry the fully composited rt to the scanout/fallback once. */
        lagfx_display_submit_rendered_frame(display, display->scanout_gpa,
                                            display->scanout_length);
        LAGFX_LOG("%s ASMBLIT: composited %u layers (bg + MAX-blend) -> display->rt",
                  op, p->frame_blit_n);
        /* Cursor on top of the composited set — read_frame serves display->rt
         * directly on the sparse-present guest, so overlay here too. */
        (void)lagfx_display_overlay_cursor(display);
        lagfx_display_signal_frame_ready(display);
    }

    if (vbuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vbuf, NULL);
        vkFreeMemory(device, vbmem, NULL);
    }
}

#endif /* LAGFX_HAVE_VULKAN */
