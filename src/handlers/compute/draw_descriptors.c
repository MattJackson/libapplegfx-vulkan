/*
 * libapplegfx-vulkan — descriptor-set construction for translated draws
 * src/handlers/compute/draw_descriptors.c
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
#include "vulkan/pipeline_build.h"
#include "vulkan/draw_record.h"
#include "vulkan/descriptor_layout.h"
#include "air2spv/spv_reflect.h"

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int lagfx_draw_descriptors_not_empty_t;

#ifdef LAGFX_HAVE_VULKAN

/* Stage 85b — build a descriptor set for a translated resource-using pipeline,
 * populated from the guest's bound storage buffers. Returns VK_NULL_HANDLE on
 * any failure (caller falls back to substitute). out_bufs/out_mems[0..*out_n)
 * are transient and must be destroyed after the draw submits. The reflected
 * binding number doubles as the Metal resource index (the translator assigns
 * bindings in resource-arg/index order); we bind the guest buffer at that slot,
 * checking the fragment then vertex buffer table. */
VkDescriptorSet lagfx_build_draw_descriptor_set(
        lagfx_protocol_t *p, lagfx_task_entry_t *task,
        struct lagfx_vk_state *vk, VkDescriptorSetLayout dsl,
        const uint8_t *binding_no, const uint8_t *binding_kind, uint32_t n,
        VkBuffer *out_bufs, VkDeviceMemory *out_mems, uint32_t *out_n) {
    *out_n = 0;
    if (!vk || vk->draw_desc_pool == VK_NULL_HANDLE
        || dsl == VK_NULL_HANDLE || n == 0u) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = vk->draw_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &dsl,
    };
    if (vkAllocateDescriptorSets(vk->device, &ai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkWriteDescriptorSet    writes[16];
    VkDescriptorBufferInfo  binfos[16];
    VkDescriptorImageInfo   iinfos[16];
    uint32_t nw = 0;
    /* M1 texture-composite: when fragment bindings are offset (LAGFX_M1_TEXCOMP),
     * the reflected binding number no longer equals the Metal resource index.
     * Demux per stage/kind: bindings < FRAG_BASE are vertex; >= FRAG_BASE are
     * fragment. The guest binds textures/buffers by Metal index per namespace,
     * so resolve by per-kind ORDINAL among the (sorted) fragment bindings —
     * the N-th fragment SAMPLED_IMAGE → fragment_textures[N], N-th fragment
     * STORAGE → fragment_buffers[N]. */
    const bool m1_texcomp = LAGFX_POLICY("M1_TEXCOMP");
    uint32_t tex_ord = 0, fbuf_ord = 0;
    /* M2 multi-texture: SAMPLED_IMAGE bindings are linear (0,1,2,…) but the guest
     * binds textures at SPARSE Metal slots (e.g. 0 and 3 for login composites).
     * Map the N-th SAMPLED_IMAGE to the N-th VALID fragment_texture slot so
     * multi-texture composites resolve — the old `fragment_textures[tex_ord]`
     * mis-hit the empty slots 1/2 → "texture unresolved" → skipped draw (the login
     * UI elements). Gated with texcomp. */
    uint8_t valid_tex_slots[LAGFX_MAX_BINDING_SLOTS]; uint32_t n_valid_tex = 0;
    if (m1_texcomp) {
        /* M2 UITEX: build the valid-texture-slot list PREFERENCE-ORDERED so a
         * SAMPLED_IMAGE binds the real login-UI texture, not the cursor/black
         * framebuffer. The translator loses the Metal [[texture(n)]] index
         * (emits sequential bindings), so the N-th-valid-slot map picks the
         * wrong texture for multi-texture login composites. TEXSCAN proved the
         * login UI lives in SMALL type-0x03 textures (ref=0x25/0x2a/0x2e/0x32,
         * 20480 B, non-black) bound at slot 3, while slot 0 holds the 5 MiB BLACK
         * framebuffer (ref=0x10) or the cursor. Prefer slots resolving to a
         * small (≤1 MiB) type-0x03 texture; append the rest. The cursor composite
         * (only ref=0x16, a small type-0x03) is unchanged. Gated; falls back to
         * plain valid order when off. */
        bool uitex = LAGFX_POLICY("M2_UITEX");
        if (uitex) {
            for (int pass = 0; pass < 2; pass++) {
                for (uint32_t s = 0; s < LAGFX_MAX_BINDING_SLOTS; s++) {
                    if (!task->bindings.fragment_textures[s].valid) continue;
                    uint32_t r = task->bindings.fragment_textures[s].ref;
                    uint8_t rt = 0; uint64_t rva = 0, rgpa = 0;
                    uint8_t rd[16] = {0}; uint64_t rsz = 0;
                    bool small_tex = false;
                    if (lagfx_resolve_object_data(p, task, r, &rt, &rva, &rgpa)
                        && rva != 0u && rt == 0x03u
                        && lagfx_task_read_virtual(p, task, rva, sizeof(rd), rd)) {
                        rsz = lagfx_le64(rd);
                        small_tex = (rsz >= 4096u && rsz <= 1024u * 1024u);
                    }
                    bool want = (pass == 0) ? small_tex : !small_tex;
                    if (want && n_valid_tex < LAGFX_MAX_BINDING_SLOTS) {
                        /* dedup */
                        bool seen = false;
                        for (uint32_t k = 0; k < n_valid_tex; k++)
                            if (valid_tex_slots[k] == (uint8_t)s) { seen = true; break; }
                        if (!seen) valid_tex_slots[n_valid_tex++] = (uint8_t)s;
                    }
                }
            }
        } else {
            for (uint32_t s = 0; s < LAGFX_MAX_BINDING_SLOTS; s++)
                if (task->bindings.fragment_textures[s].valid)
                    valid_tex_slots[n_valid_tex++] = (uint8_t)s;
        }
    }
    /* M2q DUMB-FAITHFUL: SKIPBLACKFILL removed. A black fill the guest submits
     * is drawn; with src-over blending and preserved submit order, an early
     * opaque background does not destroy later layers and a transparent black
     * layer does not wipe what is under it. Black is never special-cased. */
    for (uint32_t i = 0; i < n && i < 16u; i++) {
        uint32_t slot = binding_no[i];
        /* M1 (c): texture (SAMPLED_IMAGE) binding — resolve the guest's bound
         * fragment texture ref → IOSurface VkImageView via the task-agnostic
         * registry lookup (B9). If the texture isn't guest-visible yet, fail
         * the whole set (skip this draw) rather than form a partial descriptor
         * set — no crash, no garbage sample. */
        if (binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLED_IMAGE) {
            VkImageView view = VK_NULL_HANDLE;
            VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
            const char *tex_src = "none";
            /* Textures are fragment-stage; map the N-th SAMPLED_IMAGE to the N-th
             * VALID guest texture slot (sparse), not linearly to slot N. */
            uint32_t tex_idx = m1_texcomp
                ? (tex_ord < n_valid_tex ? valid_tex_slots[tex_ord] : LAGFX_MAX_BINDING_SLOTS)
                : slot;
            tex_ord++;
            uint32_t tref = 0;
            if (tex_idx < LAGFX_MAX_BINDING_SLOTS
                && task->bindings.fragment_textures[tex_idx].valid) {
                tref = task->bindings.fragment_textures[tex_idx].ref;
                lagfx_resource_entry_t *te =
                    lagfx_resource_lookup_texture(&p->resources, tref);
                if (te && te->view != VK_NULL_HANDLE) {
                    lagfx_vk_iosurface_t *ios =
                        (lagfx_vk_iosurface_t *)te->host_handle;
                    /* CRASH FIX: only bind the texture if its image is genuinely
                     * in a SAMPLEABLE layout. Binding a view whose image is still
                     * UNDEFINED/uninitialized (declared here as GENERAL) makes
                     * lavapipe sample a layout-mismatched image → SIGSEGV. An
                     * unbacked/not-yet-rendered IOSurface → leave view NULL →
                     * skip the draw (no crash). This is the texture-sample crash
                     * that killed the M2 runs once real data drove the shader. */
                    if (ios && (ios->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                || ios->layout == VK_IMAGE_LAYOUT_GENERAL)) {
                        /* Cached content may predate the guest's write of the
                         * backing (wallpaper decodes seconds into boot) —
                         * re-read (rate-limited; refresh itself skips
                         * GPU-drawn surfaces) so later draws sample the
                         * guest's current bytes. */
                        lagfx_texture_refresh(p, task, vk, tref);
                        view = te->view;
                        lay  = ios->layout;
                        tex_src = "direct";
                    }
                }
            }
            if (view == VK_NULL_HANDLE && tref != 0u) {
                /* Realize the bound ref's REAL guest texture content before any
                 * fallback: create + upload a surface from its type-0x03
                 * backing so composites sample actual login pixels. */
                lagfx_vk_iosurface_t *rt_ios =
                    (lagfx_vk_iosurface_t *)lagfx_texture_realize(p, task, vk, tref);
                if (rt_ios && rt_ios->view != VK_NULL_HANDLE
                    && (rt_ios->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        || rt_ios->layout == VK_IMAGE_LAYOUT_GENERAL)) {
                    view = rt_ios->view; lay = rt_ios->layout;
                    tex_src = "realized";
                }
            }
            if (view == VK_NULL_HANDLE) {
                /* M2 UIRENDER: rather than SKIP a loginwindow UI draw whose sampled
                 * texture is unresolved (→ black screen, the whole UI vanishes),
                 * bind any already-backed sampleable texture as a fallback so the
                 * draw RENDERS (geometry + a real, if wrong, texture). The login UI
                 * is many small textured quads; skipping any one drops the whole
                 * draw. A fallback lets the UI shapes appear (toward a recognizable
                 * loginwindow) instead of nothing. Gated. */
                /* M2c WHITEFB (LAGFX_M2_WHITEFB): bind a lazily-created opaque
                 * WHITE texture for unresolved samples. SOLIDFRAG proved the
                 * whole geometry chain; the translated composite fragments then
                 * emitted NOTHING (blend disabled ⇒ they DISCARD, SkyLight's
                 * alpha-threshold discard, on alpha-0 fallback textures).
                 * Opaque white guarantees alpha=1 → no discard → the real
                 * fragment's computed output becomes visible. */
                /* M2q: WHITEFB / UIRENDER are OPT-IN diagnostics now — binding
                 * a texture the guest did not name is fabrication. The dumb-
                 * faithful default binds exactly the named texture or skips. */
                if (view == VK_NULL_HANDLE && getenv("LAGFX_M2_WHITEFB")) {
                    lagfx_vk_iosurface_t *wfb =
                        (lagfx_vk_iosurface_t *)p->white_fb_ios;
                    if (!wfb) {
                        if (lagfx_vk_iosurface_create(vk, 4u, 4u, 80u, &wfb) == LAGFX_OK
                            && wfb) {
                            uint8_t wpx[4u * 4u * 4u];
                            memset(wpx, 0xFF, sizeof(wpx));
                            if (lagfx_vk_iosurface_upload_pixels(vk, wfb, wpx,
                                                                sizeof(wpx)) == LAGFX_OK) {
                                p->white_fb_ios = (uintptr_t)wfb;
                                LAGFX_LOG("P6b WHITEFB: created 4x4 opaque-white fallback");
                            } else {
                                lagfx_vk_iosurface_destroy(vk, wfb);
                                wfb = NULL;
                            }
                        } else {
                            wfb = NULL;
                        }
                    }
                    if (wfb && wfb->view != VK_NULL_HANDLE) {
                        view = wfb->view; lay = wfb->layout;
                        tex_src = "whitefb";
                    }
                }
                if (view == VK_NULL_HANDLE && getenv("LAGFX_M2_UIRENDER")) {
                    /* M2c: PREFER textures with a REAL guest backing (gpu_addr
                     * != 0) — the registry also holds PERPASS-created empty
                     * surfaces (alpha 0); sampling one makes the composite's
                     * output invisible (the post-SOLIDFRAG fragment-stage kill).
                     * Pass 0: real-backed only; pass 1: any sampleable. */
                    for (int fbpass = 0; fbpass < 2 && view == VK_NULL_HANDLE; fbpass++) {
                        for (uint32_t ri = 0; ri < p->resources.count; ri++) {
                            lagfx_resource_entry_t *fre = &p->resources.entries[ri];
                            lagfx_vk_iosurface_t *fb =
                                (lagfx_vk_iosurface_t *)fre->host_handle;
                            if (fbpass == 0 && fre->gpu_addr == 0u) continue;
                            if (fb && fb->view != VK_NULL_HANDLE
                                && (fb->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    || fb->layout == VK_IMAGE_LAYOUT_GENERAL)) {
                                view = fb->view; lay = fb->layout;
                                tex_src = "uirender";
                                break;
                            }
                        }
                    }
                }
                if (view != VK_NULL_HANDLE) goto uirender_bound;
                lagfx_resource_entry_t *dbg_te =
                    tref ? lagfx_resource_lookup_texture(&p->resources, tref) : NULL;
                LAGFX_LOG("P6b bind#%u slot=%u texture unresolved — skip draw (no crash) "
                          "[ref=0x%x pipe=0x%x tex_idx=%u tex_ord=%u n_valid=%u "
                          "te=%d te_view=%d]",
                          binding_no[i], slot, tref, task->pending_pipeline.reference,
                          tex_idx, tex_ord, n_valid_tex, dbg_te ? 1 : 0,
                          (dbg_te && dbg_te->view != VK_NULL_HANDLE) ? 1 : 0);
                for (uint32_t k = 0; k < *out_n; k++) {
                    vkDestroyBuffer(vk->device, out_bufs[k], NULL);
                    vkFreeMemory(vk->device, out_mems[k], NULL);
                }
                vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
                *out_n = 0;
                return VK_NULL_HANDLE;
            }
          uirender_bound:
            if (getenv("LAGFX_DUMP_SPV"))
                LAGFX_LOG("TEXBIND: pipe=0x%x bind=%u tref=0x%x src=%s",
                          task->pending_pipeline.reference, binding_no[i], tref, tex_src);
            iinfos[nw] = (VkDescriptorImageInfo){ .imageView = view, .imageLayout = lay };
            writes[nw] = (VkWriteDescriptorSet){
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = set,
                .dstBinding      = binding_no[i],
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo      = &iinfos[nw],
            };
            /* Diagnostic: a draw SUCCESSFULLY bound a texture. Reveals whether the
             * wallpaper (ref=0x10) is ever sampled (→ overpaint problem) or never
             * (→ translator drops the sample). */
            LAGFX_LOG("P6b TEXBIND ok pipe=0x%x bind#%u tref=0x%x layout=%d",
                      task->pending_pipeline.reference, binding_no[i], tref, (int)lay);
            nw++;
            continue;
        }
        /* M1 (c): sampler binding — the shared default linear/clamp sampler. */
        if (binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLER) {
            if (vk->default_sampler == VK_NULL_HANDLE) {
                for (uint32_t k = 0; k < *out_n; k++) {
                    vkDestroyBuffer(vk->device, out_bufs[k], NULL);
                    vkFreeMemory(vk->device, out_mems[k], NULL);
                }
                vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
                *out_n = 0;
                return VK_NULL_HANDLE;
            }
            iinfos[nw] = (VkDescriptorImageInfo){ .sampler = vk->default_sampler };
            writes[nw] = (VkWriteDescriptorSet){
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = set,
                .dstBinding      = binding_no[i],
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo      = &iinfos[nw],
            };
            nw++;
            continue;
        }
        if (binding_kind[i] != (uint8_t)LAGFX_SPV_BINDING_STORAGE_BUFFER) continue;
        uint8_t data[LAGFX_DRAW_DS_BUF_SZ];
        bool have = false;
        /* M2: the bound buffer's DECLARED size (from the placement descriptor's
         * matched range). The VkBuffer is sized to this so a shader's dynamic
         * index stays in bounds (no lavapipe OOB). Default to the read size. */
        uint64_t bind_alloc_sz = LAGFX_DRAW_DS_BUF_SZ;
        if (slot < LAGFX_MAX_BINDING_SLOTS) {
            lagfx_binding_slot_t *bs = NULL;
            if (m1_texcomp) {
                /* TEXCOMP demux: fragment buffers are at binding >= FRAG_BASE
                 * (resolve by ordinal among fragment STORAGE bindings →
                 * fragment_buffers[ordinal]); lower bindings are vertex buffers
                 * at their binding index. Unambiguous — no cross-stage collision. */
                if (slot >= LAGFX_FRAG_BINDING_BASE) {
                    if (fbuf_ord < LAGFX_MAX_BINDING_SLOTS
                        && task->bindings.fragment_buffers[fbuf_ord].valid)
                        bs = &task->bindings.fragment_buffers[fbuf_ord];
                    fbuf_ord++;
                } else {
                    /* Vertex storage buffer. When the pipeline has STAGE-IN vertex
                     * attributes (UberCompositeVertex et al.), the guest binds the
                     * per-vertex stage-in data at SetVertexBuffers[0] and the
                     * shader's [[buffer(n)]] (e.g. the MVP at [[buffer(1)]]) at the
                     * NEXT vertex-buffer slot(s). But our translator numbers the
                     * vertex storage buffer linearly from binding 0, so binding 0
                     * would mis-resolve to vertex_buffers[0] = the stage-in data
                     * (garbage as a matrix) → degenerate geometry → zero coverage.
                     * Skip the stage-in vertex buffer slot(s): the storage buffer's
                     * guest slot = binding + n_vtx_inputs's buffer count (1 for a
                     * single interleaved stage-in buffer). Proven: composites cover
                     * nothing while ColorFill (no stage-in) covers fullscreen. */
                    uint32_t vslot = slot;
                    /* Skip the stage-in vertex buffer slot(s) so the MVP storage
                     * buffer resolves to the right guest slot. PROVEN (STORDUMP):
                     * the UberComposite viewport matrix is at SetVertexBuffers[2]
                     * (ref 0xe off 128) — there are TWO stage-in buffers (ref 0x14
                     * + ref 0x13) to skip, so binding 0 → vertex_buffers[2], not [1].
                     * The +1 heuristic landed on the screen-dims buffer (off 0/64) →
                     * gl_Position.y constant → quad collapses to a band (the black
                     * bar). MTXSCAN below overrides binding 0 by content signature. */
                    if (task->pending_pipeline.n_vtx_inputs > 0u)
                        vslot = slot + 1u;
                    if (vslot < LAGFX_MAX_BINDING_SLOTS
                        && task->bindings.vertex_buffers[vslot].valid)
                        bs = &task->bindings.vertex_buffers[vslot];
                    else if (task->bindings.vertex_buffers[slot].valid)
                        bs = &task->bindings.vertex_buffers[slot];
                    /* M2 MTXSCAN: the fixed skip is fragile (per-draw binding varies).
                     * For the vertex MVP binding (binding 0), scan ALL vertex-buffer
                     * slots for the one whose content has the viewport-matrix signature
                     * (col1.y != 0) and bind THAT — robust against slot reordering.
                     * Only overrides binding 0 (the MVP); other bindings keep the skip
                     * heuristic. Gated. */
                    if (LAGFX_POLICY("M2_MTXSCAN") && binding_no[i] == 0u
                        && task->pending_pipeline.n_vtx_inputs > 0u) {
                        uint8_t probe[64];
                        for (uint32_t cs = 0; cs < LAGFX_MAX_BINDING_SLOTS && cs < 8u; cs++) {
                            if (!task->bindings.vertex_buffers[cs].valid) continue;
                            if (lagfx_read_binding_slot(p, task,
                                    &task->bindings.vertex_buffers[cs], probe, sizeof(probe))
                                && lagfx_looks_like_mvp_matrix(probe)) {
                                bs = &task->bindings.vertex_buffers[cs];
                                LAGFX_LOG("M2 MTXSCAN: vertex MVP binding0 -> vbuf slot %u "
                                          "(ref=0x%x off=%llu, viewport-matrix signature)",
                                          cs, bs->ref, (unsigned long long)bs->offset);
                                break;
                            }
                        }
                    }
                }
            } else {
                /* B8: VERTEX-FIRST (no binding offset). The pipeline reflection
                 * MERGES the vertex and fragment [[buffer(n)]] namespaces into one
                 * set-0 binding array, so binding N collides when BOTH stages bind
                 * buffer N (e.g. ColorFill: vertex ViewportToNDC binds positions at
                 * [[buffer(0)]] offset 0, fragment ColorFill binds its colour at
                 * [[buffer(0)]] offset 192 of the SAME guest buffer ref=0xe).
                 * Checking fragment first gave the VERTEX shader's binding-0 the
                 * fragment colour bytes → garbage positions → degenerate geometry →
                 * black frame (confirmed on ref=0xd: bind#0 read off0xc0=192 not
                 * off0=positions). Vertex MUST win: a wrong fragment colour only
                 * mis-tints; wrong vertex positions collapse the whole draw. So
                 * resolve vertex_buffers[slot] first and only fall back to
                 * fragment_buffers[slot] for fragment-only bindings. (Superseded by
                 * the TEXCOMP demux above when fragment bindings are offset.) */
                if (task->bindings.vertex_buffers[slot].valid)
                    bs = &task->bindings.vertex_buffers[slot];
                else if (task->bindings.fragment_buffers[slot].valid)
                    bs = &task->bindings.fragment_buffers[slot];
            }
            if (bs && bs->ref != 0u) {
                uint8_t type = 0; uint64_t va = 0, gpa = 0;
                if (lagfx_resolve_object_data(p, task, bs->ref, &type, &va, &gpa)
                    && va != 0u
                    && lagfx_read_virtual_besteffort(p, task, va + bs->offset,
                                                     LAGFX_DRAW_DS_BUF_SZ, data)) {
                    have = true;
                    /* M2: `va` points to the placement DESCRIPTOR ((size,PFN)
                     * range pairs), not the buffer content — binding it gives
                     * the shader garbage. The real data lives at PFN<<12 read
                     * as a guest VA (translate-then-read; confirmed: descriptor
                     * PFN 0x741 → VA 0x741000 → real non-zero data, where direct
                     * read_memory saw zeros). Scan the descriptor for the data
                     * range (first PFN whose translated page is non-zero) and
                     * rebind the real bytes. Crash-proof: on any miss, keep the
                     * descriptor bytes already in `data`. */
                    uint8_t desc[64];
                    /* Bind the buffer's REAL data (the MVP matrix, vertex data,
                     * etc.) instead of the placement descriptor — the vertex
                     * shaders compute position = MVP * vertex_input, so the MVP
                     * [[buffer]] must be real or every position collapses to 0.
                     * The descriptor is (size@e*16, PFN@e*16+8) 16-byte entries;
                     * the data is at PFN<<12 read as a guest VA (translate). The
                     * 64 KiB best-effort read zero-pads the tail so a fixed 64 B
                     * MVP binds AND a dynamic over-read returns zeros (no crash). */
                    if (LAGFX_POLICY("VTX_INPUT")
                        && lagfx_task_read_virtual(p, task, va, sizeof(desc), desc)) {
                        /* GROUND-TRUTH dump (LAGFX_M2_DUMP): the full placement
                         * descriptor (4× {size,PFN}) plus the leaf-page bytes at
                         * each entry's PFN<<12 read at offsets 0/64/128/192 — so we
                         * can SEE where the real float position/uniform data lives
                         * instead of guessing the buffer layout. */
                        if (getenv("LAGFX_M2_DUMP")) {
                            for (int e = 0; e < 4; e++) {
                                uint64_t es = lagfx_le64(desc + (size_t)e * 16u);
                                uint64_t ep = lagfx_le64(desc + (size_t)e * 16u + 8u);
                                LAGFX_LOG("P6b DUMP ref=0x%x bind%u desc[%d]={size=%llu PFN=0x%llx}",
                                          bs->ref, binding_no[i], e,
                                          (unsigned long long)es, (unsigned long long)ep);
                                if (ep < 0x10u || ep > 0xfffffu) continue;
                                for (int o = 0; o < 4; o++) {
                                    uint64_t off = (uint64_t)o * 64u;
                                    uint8_t lb[16] = {0};
                                    if (lagfx_task_read_virtual(p, task, (ep << 12) + off, 16, lb))
                                        LAGFX_LOG("P6b DUMP   PFN0x%llx+%llu: %02x %02x %02x %02x "
                                                  "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                                  (unsigned long long)ep, (unsigned long long)off,
                                                  lb[0],lb[1],lb[2],lb[3],lb[4],lb[5],lb[6],lb[7],
                                                  lb[8],lb[9],lb[10],lb[11],lb[12],lb[13],lb[14],lb[15]);
                                }
                            }
                        }
                        /* B8 LOGICAL SCATTER-GATHER WALK. The placement descriptor
                         * is an ORDERED list of {size,PFN} ranges that tile the
                         * buffer's LOGICAL address space: desc[0] covers logical
                         * [0, size0), desc[1] covers [size0, size0+size1), etc. To
                         * read the binding at logical offset `bs->offset`, find the
                         * entry whose range CONTAINS it and read PFN<<12 + (offset -
                         * range_start). The previous "first PFN whose page is
                         * non-zero at +offset" heuristic was WRONG: ground-truth
                         * dump of ref=0xe showed desc[0]={131072, PFN0x721} (real
                         * float uniforms: 1024.0@+12, 0.0015@+128, 1.0@+192) and
                         * desc[1]={5242880, PFN0x741} (5 MiB of 00 00 00 ff black
                         * pixels). All bindings (offsets 0/64/128/192 < 131072)
                         * belong to PFN0x721, but because PFN0x721+0 STARTS with 8
                         * zero bytes (the 1024.0 float sits at +12), the zero-probe
                         * skipped it and mis-bound bindings 0/1 to the black-pixel
                         * buffer PFN0x741. The logical walk binds the correct page
                         * regardless of leading zero bytes. */
                        uint64_t acc = 0;
                        for (int e = 0; e < 4; e++) {
                            uint64_t rsize = lagfx_le64(desc + (size_t)e * 16u);
                            uint64_t pfn = lagfx_le64(desc + (size_t)e * 16u + 8u);
                            if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
                            if (bs->offset < acc + rsize) {
                                uint64_t local = bs->offset - acc;
                                uint64_t dva = (pfn << 12) + local;
                                /* SAME root cause: pfn is guest-PHYSICAL, dva is a
                                 * GPA — read raw (gated LAGFX_M2_RAWGPA), not
                                 * VA-translated, so the composite's storage buffers
                                 * (UVs, MVP transforms, uniforms) are correct, not
                                 * garbage. Falls back to VA read when gate off (M1). */
                                lagfx_read_resource_backing(p, task, dva,
                                                            LAGFX_DRAW_DS_BUF_SZ, data);
                                /* DRAW-TIME storage-buffer dump (gated DUMP_SPV):
                                 * the matrix/uniforms that transform the vertex
                                 * positions. If this is zero at draw time, gl_Position
                                 * collapses → degenerate quad → uniform gray. */
                                if (getenv("LAGFX_DUMP_SPV")) {
                                    float mf[16]; uint32_t mu;
                                    for (int q = 0; q < 16; q++) {
                                        mu = lagfx_le32(data + q*4); memcpy(&mf[q], &mu, 4);
                                    }
                                    /* 4 vec4 columns: col0[0..3] col1[4..7] col2[8..11]
                                     * col3[12..15]. The shader uses col0,col1,col3; col3
                                     * is the translation/w — if (… ,1) then w=1 (ok); if
                                     * all-zero then gl_Position.w=0 → degenerate. */
                                    LAGFX_LOG("STORDUMP ref=0x%x bind%u off=%llu col0[%.4g %.4g %.4g %.4g] "
                                              "col1[%.4g %.4g %.4g %.4g] col3[%.4g %.4g %.4g %.4g]",
                                              bs->ref, binding_no[i],
                                              (unsigned long long)bs->offset,
                                              mf[0],mf[1],mf[2],mf[3], mf[4],mf[5],mf[6],mf[7],
                                              mf[12],mf[13],mf[14],mf[15]);
                                }
                                /* Size the VkBuffer to the matched range (cap 16 MiB,
                                 * floor 8 MiB) so any dynamic shader index stays in
                                 * bounds (OOB reads zero-pad, no lavapipe crash). */
                                bind_alloc_sz = 8u * 1024u * 1024u;
                                if (rsize > bind_alloc_sz)
                                    bind_alloc_sz = rsize > (16u*1024u*1024u)
                                                        ? (16u*1024u*1024u) : rsize;
                                LAGFX_LOG("P6b M2: ref=0x%x binding%u logical off=%llu -> desc[%d] "
                                          "PFN0x%llx+%llu (range size=%llu)", bs->ref, binding_no[i],
                                          (unsigned long long)bs->offset, e,
                                          (unsigned long long)pfn, (unsigned long long)local,
                                          (unsigned long long)rsize);
                                break;
                            }
                            acc += rsize;
                        }
                    }
                }
            }
        }
        if (have) {
            LAGFX_LOG("P6b bind#%u slot=%u DATA first32: %02x %02x %02x %02x %02x %02x %02x %02x "
                      "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                      "%02x %02x %02x %02x %02x %02x %02x %02x", binding_no[i], slot,
                      data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7],
                      data[8],data[9],data[10],data[11],data[12],data[13],data[14],data[15],
                      data[16],data[17],data[18],data[19],data[20],data[21],data[22],data[23],
                      data[24],data[25],data[26],data[27],data[28],data[29],data[30],data[31]);
            /* Geometry RE: dump bytes 32..63 (matrix cols 2,3 for a 64-B float4x4)
             * as float words — col3 carries the translation + w; if w (the last
             * float) != 1.0 the perspective divide collapses every vertex. */
            {
                float f[16];
                for (int q = 0; q < 16; q++) { uint32_t u = lagfx_le32(data + q*4); memcpy(&f[q], &u, 4); }
                LAGFX_LOG("P6b bind#%u FLOATS: c0[%.4g %.4g %.4g %.4g] c1[%.4g %.4g %.4g %.4g] "
                          "c2[%.4g %.4g %.4g %.4g] c3[%.4g %.4g %.4g %.4g]", binding_no[i],
                          f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7],
                          f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15]);
            }
            /* Arg-buffer RE: the bound buffer may be an ARGUMENT buffer whose
             * u32 words are object IDs referencing the real resources. Probe
             * the first 8 words: any that resolve as a live object are refs. */
            for (int w = 0; w < 8; w++) {
                uint32_t cand = lagfx_le32(data + w * 4);
                if (cand == 0u || cand > 0xffffu) continue;
                uint8_t t = 0; uint64_t cva = 0, cgpa = 0;
                if (lagfx_resolve_object_data(p, task, cand, &t, &cva, &cgpa) && cva != 0u) {
                    LAGFX_LOG("P6b   arg[%d]=0x%x RESOLVES type=0x%02x va=0x%llx",
                              w, cand, t, (unsigned long long)cva);
                    /* If it's a buffer, read its real data — is THIS the float
                     * struct the shader wants (host-flatten target)? */
                    if (t == 0x01u) {
                        uint8_t rd[64] = {0};
                        if (lagfx_task_read_virtual(p, task, cva, 64, rd)) {
                            LAGFX_LOG("P6b     ref0x%x desc u64: %016llx %016llx %016llx %016llx "
                                      "%016llx %016llx %016llx %016llx", cand,
                                      (unsigned long long)lagfx_le64(rd+0),  (unsigned long long)lagfx_le64(rd+8),
                                      (unsigned long long)lagfx_le64(rd+16), (unsigned long long)lagfx_le64(rd+24),
                                      (unsigned long long)lagfx_le64(rd+32), (unsigned long long)lagfx_le64(rd+40),
                                      (unsigned long long)lagfx_le64(rd+48), (unsigned long long)lagfx_le64(rd+56));
                            /* Each non-tiny u64 might be a guest VA to the actual data. Probe word1
                             * and word2 as VAs + read 16 bytes (is it the shader's float data?). */
                            for (int q = 1; q <= 3; q++) {
                                uint64_t cand_va = lagfx_le64(rd + q*8);
                                if (cand_va < 0x1000u || cand_va > 0xffffffffull) continue;
                                uint8_t dd[16] = {0};
                                if (lagfx_task_read_virtual(p, task, cand_va, 16, dd))
                                    LAGFX_LOG("P6b       ref0x%x w%d=0x%llx -> %02x %02x %02x %02x %02x %02x %02x %02x "
                                              "%02x %02x %02x %02x %02x %02x %02x %02x", cand, q,
                                              (unsigned long long)cand_va, dd[0],dd[1],dd[2],dd[3],dd[4],dd[5],dd[6],dd[7],
                                              dd[8],dd[9],dd[10],dd[11],dd[12],dd[13],dd[14],dd[15]);
                            }
                            /* M2 BREAKTHROUGH probe: the descriptor holds (size, PFN)
                             * range pairs — the real data is at PFN<<12 (a GPA read via
                             * the shell, NOT a VA). 0x39 CmdMapMemoryImmediate maps
                             * exactly these GPAs (descriptor PFN 0x721 → GPA 0x721000,
                             * size 0x20000 = the 0x39 map). Probe each descriptor word
                             * as a PFN and read PFN<<12 directly. */
                            {
                                lagfx_device_t *pdev = (lagfx_device_t *)p->dev;
                                for (int q = 0; q < 8; q++) {
                                    uint64_t pfn = lagfx_le64(rd + q*8);
                                    if (pfn < 0x10u || pfn > 0xfffffu) continue;
                                    uint64_t gpa = pfn << 12;
                                    uint8_t dd[16] = {0};
                                    if (pdev && pdev->desc.shell.read_memory
                                        && pdev->desc.shell.read_memory(pdev->desc.shell.opaque, gpa, 16, dd))
                                        LAGFX_LOG("P6b     PFNDEREF ref0x%x d[%d]=PFN0x%llx -> GPA0x%llx: "
                                                  "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                                  cand, q, (unsigned long long)pfn, (unsigned long long)gpa,
                                                  dd[0],dd[1],dd[2],dd[3],dd[4],dd[5],dd[6],dd[7],
                                                  dd[8],dd[9],dd[10],dd[11],dd[12],dd[13],dd[14],dd[15]);
                                    /* Hypothesis 1: the 0x39 'GPA' may be a guest VA — translate it
                                     * through the per-task page table before reading. */
                                    {
                                        uint8_t dv[16] = {0};
                                        if (lagfx_task_read_virtual(p, task, gpa, 16, dv)
                                            && (dv[0]|dv[1]|dv[2]|dv[3]|dv[4]|dv[5]|dv[6]|dv[7]))
                                            LAGFX_LOG("P6b     PFNVIRT  ref0x%x d[%d] VA0x%llx -> "
                                                      "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                                                      cand, q, (unsigned long long)gpa,
                                                      dv[0],dv[1],dv[2],dv[3],dv[4],dv[5],dv[6],dv[7],
                                                      dv[8],dv[9],dv[10],dv[11],dv[12],dv[13],dv[14],dv[15]);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            LAGFX_LOG("P6b bind#%u slot=%u NO guest data (zero buffer)", binding_no[i], slot);
        }
        VkBuffer vb = VK_NULL_HANDLE; VkDeviceMemory vm = VK_NULL_HANDLE;
        /* Buffer sized to the declared range (bind_alloc_sz, ≥ BUF_SZ): real
         * content in the first LAGFX_DRAW_DS_BUF_SZ bytes, rest zero-padded, so
         * a shader's dynamic index into the real buffer stays in bounds. */
        if (lagfx_vk_make_host_storage_buffer_padded(
                vk, have ? data : NULL,
                have ? (VkDeviceSize)LAGFX_DRAW_DS_BUF_SZ : 0u,
                (VkDeviceSize)bind_alloc_sz, &vb, &vm) != LAGFX_OK
            || vb == VK_NULL_HANDLE) {
            for (uint32_t k = 0; k < *out_n; k++) {
                vkDestroyBuffer(vk->device, out_bufs[k], NULL);
                vkFreeMemory(vk->device, out_mems[k], NULL);
            }
            vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
            *out_n = 0;
            return VK_NULL_HANDLE;
        }
        out_bufs[*out_n] = vb; out_mems[*out_n] = vm; (*out_n)++;
        binfos[nw] = (VkDescriptorBufferInfo){
            .buffer = vb, .offset = 0, .range = (VkDeviceSize)bind_alloc_sz };
        writes[nw] = (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = set,
            .dstBinding      = binding_no[i],
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &binfos[nw],
        };
        nw++;
    }
    if (nw == 0u) {
        vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
        *out_n = 0;
        return VK_NULL_HANDLE;
    }
    vkUpdateDescriptorSets(vk->device, nw, writes, 0, NULL);
    return set;
}


#endif /* LAGFX_HAVE_VULKAN */
