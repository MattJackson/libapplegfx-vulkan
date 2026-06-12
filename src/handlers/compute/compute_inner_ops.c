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

#ifdef LAGFX_HAVE_VULKAN
/* B1 (M0): destroy a task's cached draw pipeline (on shader change / teardown). */
static void lagfx_pending_pipeline_drop_cache(lagfx_task_entry_t *task, VkDevice device) {
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
static VkPipeline lagfx_get_cached_pipeline(lagfx_task_entry_t *task, VkDevice device,
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

/* 64 KiB: large enough to bound a shader's dynamic storage-buffer indexing so
 * an over-read returns zero-padded data instead of SIGSEGV'ing lavapipe, while
 * the descriptor / MVP reads use only the first bytes. */
#define LAGFX_DRAW_DS_BUF_SZ 65536u

/* M1 texture-composite: fragment-stage descriptor bindings are offset by this
 * base so they never collide with vertex-stage set-0 bindings in the merged
 * pipeline layout (SkyLight vertex shaders use ≤4 buffers, well under 16). The
 * draw site treats binding < base as vertex, >= base as fragment. */
#define LAGFX_FRAG_BINDING_BASE 16u

/* Best-effort guest-VA read: zero-fill `buf`, then read page by page, stopping
 * at the first unmapped page (keeping what was read). Returns true if at least
 * the first page read. Unlike lagfx_task_read_virtual (all-or-nothing), this
 * lets a small real buffer (e.g. a 64 B MVP matrix) bind from a large request
 * by zero-padding the tail, and bounds a shader's dynamic over-read to zeros. */
static bool lagfx_read_virtual_besteffort(lagfx_protocol_t *p,
                                          const lagfx_task_entry_t *task,
                                          uint64_t va, uint32_t len, uint8_t *buf) {
    memset(buf, 0, len);
    const uint32_t PAGE = 4096u;
    uint32_t done = 0u;
    bool any = false;
    while (done < len) {
        uint64_t cur = va + done;
        uint32_t page_off = (uint32_t)(cur & (PAGE - 1u));
        uint32_t chunk = PAGE - page_off;
        if (chunk > len - done) chunk = len - done;
        if (!lagfx_task_read_virtual(p, task, cur, chunk, buf + done)) break;
        any = true;
        done += chunk;
    }
    return any;
}

/* Stage 85b — build a descriptor set for a translated resource-using pipeline,
 * populated from the guest's bound storage buffers. Returns VK_NULL_HANDLE on
 * any failure (caller falls back to substitute). out_bufs/out_mems[0..*out_n)
 * are transient and must be destroyed after the draw submits. The reflected
 * binding number doubles as the Metal resource index (the translator assigns
 * bindings in resource-arg/index order); we bind the guest buffer at that slot,
 * checking the fragment then vertex buffer table. */
static VkDescriptorSet lagfx_build_draw_descriptor_set(
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
    /* Diagnostic (LAGFX_SKIP_TEX): skip any pipeline that samples a fragment
     * texture, so the pure-geometry (buffer + vertex) pipelines render without
     * the lavapipe NULL-deref that an unbacked IOSurface sample causes once
     * real storage data drives the shader down its texturing branch. Isolates
     * whether the MVP+vertex geometry path itself produces visible pixels. */
    if (getenv("LAGFX_SKIP_TEX")) {
        for (uint32_t i = 0; i < n && i < 16u; i++) {
            if (binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLED_IMAGE
                || binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLER) {
                vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
                return VK_NULL_HANDLE;
            }
        }
    }
    /* Diagnostic (LAGFX_TEXCOMP_ONLY): draw ONLY texture-sampling composites
     * (pipelines with a SAMPLED_IMAGE binding), skipping the buffer-only fills
     * (ColorFill draws fullscreen black 18× and dominates the shared render
     * target, burying the 1× composite). Isolates whether the translated
     * composite + backed guest texture produces VISIBLE non-black content. */
    if (getenv("LAGFX_TEXCOMP_ONLY")) {
        bool has_img = false;
        for (uint32_t i = 0; i < n && i < 16u; i++)
            if (binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLED_IMAGE) { has_img = true; break; }
        if (!has_img) {
            vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
            return VK_NULL_HANDLE;
        }
    }
    /* M1 texture-composite: when fragment bindings are offset (LAGFX_M1_TEXCOMP),
     * the reflected binding number no longer equals the Metal resource index.
     * Demux per stage/kind: bindings < FRAG_BASE are vertex; >= FRAG_BASE are
     * fragment. The guest binds textures/buffers by Metal index per namespace,
     * so resolve by per-kind ORDINAL among the (sorted) fragment bindings —
     * the N-th fragment SAMPLED_IMAGE → fragment_textures[N], N-th fragment
     * STORAGE → fragment_buffers[N]. */
    const bool m1_texcomp = getenv("LAGFX_M1_TEXCOMP") != NULL;
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
        bool uitex = getenv("LAGFX_M2_UITEX") != NULL;
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
    /* M2 SKIPBLACKFILL: a no-texture composite whose fragment colour is BLACK is
     * a fullscreen black FILL (ColorFill ref=0xd). In the shared-RT compositing
     * it OVERWRITES the texture-composite UI layer drawn before it → the login UI
     * vanishes. The real dark login background (0,0,2) comes from the texture
     * composites anyway, so a (0,0,0) fill is purely destructive. Track whether
     * the pipeline binds a texture and whether any fragment colour is non-black;
     * skip the draw if it's an all-black no-texture fill. Gated. */
    bool sbf_had_tex = false, sbf_nonblack = false;
    const bool m2_skipblackfill = getenv("LAGFX_M2_SKIPBLACKFILL") != NULL;
    for (uint32_t i = 0; i < n && i < 16u; i++) {
        uint32_t slot = binding_no[i];
        /* M1 (c): texture (SAMPLED_IMAGE) binding — resolve the guest's bound
         * fragment texture ref → IOSurface VkImageView via the task-agnostic
         * registry lookup (B9). If the texture isn't guest-visible yet, fail
         * the whole set (skip this draw) rather than form a partial descriptor
         * set — no crash, no garbage sample. */
        if (binding_kind[i] == (uint8_t)LAGFX_SPV_BINDING_SAMPLED_IMAGE) {
            sbf_had_tex = true;
            VkImageView view = VK_NULL_HANDLE;
            VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
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
                        /* REFRESH: the guest texture content changes over time, but
                         * the backing is created ONCE and cached — a stale first
                         * backing may be black while the guest has since drawn real
                         * content. Re-upload the CURRENT guest bytes each draw so we
                         * sample live content. (Gated with TEXBACK.) */
                        if (getenv("LAGFX_M1_TEXBACK") && te->gpu_addr != 0u
                            && ios->width && ios->height) {
                            size_t rl = (size_t)ios->width * ios->height * 4u;
                            if (rl > 8u * 1024u * 1024u) rl = 8u * 1024u * 1024u;
                            uint8_t *rp = malloc(rl);
                            if (rp) {
                                if (lagfx_read_virtual_besteffort(p, task, te->gpu_addr, rl, rp)) {
                                    uint32_t nb = 0;
                                    for (size_t q = 0; q + 4 <= rl; q += 4)
                                        if (rp[q] | rp[q+1] | rp[q+2]) nb++;
                                    lagfx_vk_iosurface_upload_pixels(vk, ios, rp, rl);
                                    LAGFX_LOG("P6b TEXREFRESH ref=0x%x %ux%u nonblack_px=%u/%zu",
                                              tref, ios->width, ios->height, nb, rl/4);
                                }
                                free(rp);
                            }
                        }
                        view = te->view;
                        lay  = ios->layout;
                    }
                }
            }
            /* M1 TEXBACK: back the sampled guest texture from its memory. The
             * texture object descriptor (type 0x03) is {size@0, PFN|flags@8} 16-B
             * entries (flags in the PFN word's high 32 bits). Read the content at
             * PFN<<12, create a sampleable VkImage of derived dimensions (BGRA8),
             * upload, and register it so this + future draws sample real guest
             * pixels instead of skipping → non-black content from the real
             * translated composite shaders. Gated; created once per ref. */
            if (view == VK_NULL_HANDLE && getenv("LAGFX_M1_TEXBACK") && tref != 0u) {
                uint8_t tt = 0; uint64_t tva = 0, tgpa = 0;
                uint8_t td[64] = {0};
                uint64_t pfn = 0, sz = 0;
                /* The task whose page table the backing pixels live in — defaults
                 * to the current task, set to task-0 when a BACKREF follow resolves
                 * the backing in the global task. */
                const lagfx_task_entry_t *pix_task = task;
                if (lagfx_resolve_object_data(p, task, tref, &tt, &tva, &tgpa)
                    && tva != 0u
                    && lagfx_task_read_virtual(p, task, tva, sizeof(td), td)) {
                    /* M2 TEXDIM: dump the type-0x03 texture descriptor as u32 words
                     * to find the real width/height (the byte-count guess is wrong
                     * — 20480 B = 5120 px guessed 256×20, real ~64×80). The
                     * MTLTextureDescriptor encodes W/H somewhere in these bytes. */
                    if (getenv("LAGFX_M2_TEXDIM") && tt == 0x03u) {
                        LAGFX_LOG("M2 TEXDIM ref=0x%x type=0x03 desc u32: "
                                  "%u %u %u %u | %u %u %u %u | %u %u %u %u | %u %u %u %u", tref,
                                  lagfx_le32(td+0),lagfx_le32(td+4),lagfx_le32(td+8),lagfx_le32(td+12),
                                  lagfx_le32(td+16),lagfx_le32(td+20),lagfx_le32(td+24),lagfx_le32(td+28),
                                  lagfx_le32(td+32),lagfx_le32(td+36),lagfx_le32(td+40),lagfx_le32(td+44),
                                  lagfx_le32(td+48),lagfx_le32(td+52),lagfx_le32(td+56),lagfx_le32(td+60));
                    }
                    for (int e = 0; e < 4; e++) {
                        uint64_t es = lagfx_le64(td + (size_t)e * 16u);
                        uint64_t ep = lagfx_le64(td + (size_t)e * 16u + 8u) & 0xffffffffull;
                        if (ep < 0x10u || ep > 0xfffffu || es == 0u) continue;
                        pfn = ep; sz = es; break;
                    }
                    /* M2 BACKREF (LAGFX_M2_BACKREF): a type-0x05 texture is a
                     * createBackingRefTexture — a VIEW whose APVObjectRefTexture
                     * descriptor references a BACKING texture (the real pixels),
                     * not its own (sz=2 = a ref, not data). To resolve it, find the
                     * backing object ref in the descriptor + follow it. The struct
                     * layout isn't RE'd, so SCAN the descriptor's u32 words for any
                     * that resolve to a DIFFERENT texture object → that's the
                     * backing; recurse one level to its pixels. Read-only probe
                     * first (logs candidates); the follow uses the first backing
                     * whose pixels are non-black. */
                    if (getenv("LAGFX_M2_BACKREF") && tt == 0x05u && sz < 4u) {
                        pfn = 0u; sz = 0u;  /* the entry was a ref, not pixels — re-find via backing */
                        /* The disasm of createBackingRefTexture says "Cannot get
                         * backing from a non-zero task ID" → the backing object is
                         * TASK-0 GLOBAL, not in the current task. Resolve candidates
                         * against the current task first, then fall back to task 0 —
                         * this is why backing 0x30 resolved intermittently (only when
                         * the current task happened to alias it). */
                        lagfx_task_entry_t *task0 = lagfx_protocol_find_task(p, 0u);
                        for (int w = 0; w < 16; w++) {
                            uint32_t cand = lagfx_le32(td + (size_t)w * 4u);
                            if (cand == 0u || cand == tref || cand > 0xffffu) continue;
                            uint8_t ct = 0; uint64_t cva = 0, cgpa = 0;
                            const lagfx_task_entry_t *rtask = task;
                            bool live = lagfx_resolve_object_data(p, task, cand, &ct, &cva, &cgpa)
                                        && cva != 0u;
                            if (!live && task0 && task0 != task
                                && lagfx_resolve_object_data(p, task0, cand, &ct, &cva, &cgpa)
                                && cva != 0u) { live = true; rtask = task0; }
                            uint8_t cd[64] = {0};
                            uint64_t cpfn = 0, csz = 0;
                            if (live && lagfx_task_read_virtual(p, rtask, cva, sizeof(cd), cd)) {
                                for (int e = 0; e < 4; e++) {
                                    uint64_t es = lagfx_le64(cd + (size_t)e * 16u);
                                    uint64_t ep = lagfx_le64(cd + (size_t)e * 16u + 8u) & 0xffffffffull;
                                    if (ep < 0x10u || ep > 0xfffffu || es < 4u) continue;
                                    cpfn = ep; csz = es; break;
                                }
                            }
                            bool is_tex_backing = (cpfn != 0u && csz >= 256u
                                && csz <= 16u * 1024u * 1024u && (csz % 4u) == 0u
                                && (ct == 0x01u || ct == 0x03u || ct == 0x04u));
                            /* M2 LIFECYCLE CACHE: when a candidate resolves LIVE to a
                             * real texture backing, persist {obj→pfn,sz,task} — the
                             * guest evicts these (e.g. 0x30) before the view's draw,
                             * so a draw-time resolve later misses. On a MISS, replay
                             * the cached backing. This is the cross-draw resource
                             * tracking the createBackingRefTexture path needs. */
                            if (live && is_tex_backing) {
                                bool found = false;
                                for (uint32_t k = 0; k < p->m2_backing_cache_n; k++)
                                    if (p->m2_backing_cache[k].obj_id == (uint16_t)cand) {
                                        p->m2_backing_cache[k].pfn = (uint32_t)cpfn;
                                        p->m2_backing_cache[k].sz = csz;
                                        p->m2_backing_cache[k].task_id = (uint16_t)rtask->id;
                                        p->m2_backing_cache[k].valid = 1u; found = true; break;
                                    }
                                if (!found && p->m2_backing_cache_n < 64u) {
                                    uint32_t k = p->m2_backing_cache_n++;
                                    p->m2_backing_cache[k].obj_id = (uint16_t)cand;
                                    p->m2_backing_cache[k].pfn = (uint32_t)cpfn;
                                    p->m2_backing_cache[k].sz = csz;
                                    p->m2_backing_cache[k].task_id = (uint16_t)rtask->id;
                                    p->m2_backing_cache[k].valid = 1u;
                                }
                            } else if (!is_tex_backing) {
                                /* MISS (evicted / not a backing live) — replay cache. */
                                for (uint32_t k = 0; k < p->m2_backing_cache_n; k++)
                                    if (p->m2_backing_cache[k].valid
                                        && p->m2_backing_cache[k].obj_id == (uint16_t)cand) {
                                        cpfn = p->m2_backing_cache[k].pfn;
                                        csz  = p->m2_backing_cache[k].sz;
                                        lagfx_task_entry_t *ct0 =
                                            lagfx_protocol_find_task(p, p->m2_backing_cache[k].task_id);
                                        if (ct0) { rtask = ct0; is_tex_backing = true;
                                            LAGFX_LOG("P6b BACKREF ref=0x%x CACHE-HIT obj 0x%x "
                                                      "(PFN0x%llx %llu B)", tref, cand,
                                                      (unsigned long long)cpfn, (unsigned long long)csz); }
                                        break;
                                    }
                            }
                            LAGFX_LOG("P6b BACKREF ref=0x%x word[%d]=0x%x -> type=0x%02x "
                                      "backing PFN0x%llx sz=%llu live=%d", tref, w, cand, ct,
                                      (unsigned long long)cpfn, (unsigned long long)csz, (int)live);
                            if (pfn == 0u && is_tex_backing) {
                                pfn = cpfn; sz = csz;
                                pix_task = rtask;
                                LAGFX_LOG("P6b BACKREF ref=0x%x FOLLOWING backing 0x%x "
                                          "(type 0x%02x, %llu B, task=%u)", tref, cand, ct,
                                          (unsigned long long)csz, rtask->id);
                            }
                        }
                    }
                }
                if (pfn != 0u && sz >= 4u) {
                    uint32_t total_px = (uint32_t)(sz / 4u);
                    uint32_t W = 1u, H = 1u;
                    /* REAL dimensions from the texture descriptor: word[4] (offset
                     * 16) = bytesPerRow → width = bytesPerRow/4, height =
                     * sz/bytesPerRow. Validated: cursor 4096 B bpr128 → 32×32; UI
                     * 20480 B bpr256 → 64×80; framebuffer bpr5120 → 1280×1024. This
                     * replaces the byte-count guess (which mis-shaped UI textures →
                     * scrambled UVs → no visible content). Only for a real texture
                     * descriptor (type 0x03); the backing-follow path keeps the
                     * heuristic since it has no descriptor of its own. */
                    uint32_t bpr = (tt == 0x03u) ? lagfx_le32(td + 16) : 0u;
                    if (bpr >= 4u && (bpr % 4u) == 0u && (sz % bpr) == 0u
                        && (sz / bpr) <= 8192u && (bpr / 4u) <= 8192u) {
                        W = bpr / 4u; H = (uint32_t)(sz / bpr);
                    } else if (total_px >= 1310720u) { W = 1280u; H = 1024u; }
                    else {
                        /* Fallback (no usable bytesPerRow): prefer SQUARE, else the
                         * largest dividing power-of-2 width. */
                        uint32_t s = 1u; while (s * s < total_px) s++;
                        if (s * s == total_px) { W = s; H = s; }
                        else {
                            for (uint32_t cand = 256u; cand >= 1u; cand >>= 1) {
                                if (total_px % cand == 0u) { W = cand; H = total_px / cand; break; }
                            }
                        }
                    }
                    size_t rd_len = (size_t)W * H * 4u;
                    if (rd_len > 8u * 1024u * 1024u) rd_len = 8u * 1024u * 1024u;
                    uint8_t *pix = malloc(rd_len);
                    lagfx_vk_iosurface_t *ios = NULL;
                    if (pix
                        && lagfx_read_virtual_besteffort(p, pix_task, pfn << 12, rd_len, pix)
                        && lagfx_vk_iosurface_create(vk, W, H, 80u, &ios) == LAGFX_OK
                        && lagfx_vk_iosurface_upload_pixels(vk, ios, pix, rd_len) == LAGFX_OK) {
                        lagfx_resource_register(&p->resources, tref,
                                                LAGFX_RESOURCE_TYPE_TEXTURE, task->id,
                                                pfn << 12, sz);
                        lagfx_resource_entry_t *ne =
                            lagfx_resource_lookup_texture(&p->resources, tref);
                        if (ne) { ne->host_handle = ios; ne->image = ios->image; ne->view = ios->view; }
                        view = ios->view;
                        lay  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        uint32_t nb = 0;
                        for (size_t q = 0; q + 4 <= rd_len; q += 4)
                            if (pix[q] | pix[q+1] | pix[q+2]) nb++;
                        LAGFX_LOG("P6b TEXBACK ref=0x%x backed %ux%u from PFN0x%llx (%llu B) "
                                  "nonblack_px=%u/%zu", tref, W, H,
                                  (unsigned long long)pfn, (unsigned long long)sz, nb, rd_len/4);
                    } else if (ios) {
                        lagfx_vk_iosurface_destroy(vk, ios);
                    }
                    free(pix);
                }
            }
            if (view == VK_NULL_HANDLE) {
                /* M1 TEXPROBE: the texture has no host VkImage backing. Resolve
                 * its guest memory (placement descriptor, same as buffers) and
                 * histogram the first leaf page as BGRA8 — does the guest texture
                 * hold real (non-black) content (CPU-uploaded image/atlas) that a
                 * future backing would surface, or is it an empty GPU render
                 * target? Decides whether texture-backing is viable at this guest
                 * state. Read-only; gated. */
                if (getenv("LAGFX_M1_TEXPROBE") && tref != 0u) {
                    uint8_t ttype = 0; uint64_t tva = 0, tgpa = 0;
                    bool resolved = lagfx_resolve_object_data(p, task, tref, &ttype, &tva, &tgpa);
                    uint8_t tdesc[64] = {0};
                    uint64_t leaf_pfn = 0, leaf_sz = 0;
                    if (resolved && tva != 0u
                        && lagfx_task_read_virtual(p, task, tva, sizeof(tdesc), tdesc)) {
                        for (int e = 0; e < 4; e++) {
                            uint64_t es = lagfx_le64(tdesc + (size_t)e * 16u);
                            uint64_t raw = lagfx_le64(tdesc + (size_t)e * 16u + 8u);
                            /* Texture objects carry FLAGS in the PFN word's high 32
                             * bits (e.g. 0x0001000100000741 → PFN 0x741); mask low. */
                            uint64_t ep = raw & 0xffffffffull;
                            if (ep < 0x10u || ep > 0xfffffu || es == 0u) continue;
                            leaf_pfn = ep; leaf_sz = es; break;
                        }
                    }
                    uint32_t nonblack = 0, nonzero = 0; uint8_t px[4096] = {0};
                    if (leaf_pfn
                        && lagfx_task_read_virtual(p, task, leaf_pfn << 12, sizeof(px), px)) {
                        for (size_t q = 0; q + 4 <= sizeof(px); q += 4) {
                            if (px[q] | px[q+1] | px[q+2]) nonblack++;
                            if (px[q] | px[q+1] | px[q+2] | px[q+3]) nonzero++;
                        }
                    }
                    LAGFX_LOG("P6b TEXPROBE ref=0x%x type=0x%02x resolved=%d va=0x%llx gpa=0x%llx "
                              "leafPFN=0x%llx sz=%llu | of %zu px: nonblack=%u nonzero=%u",
                              tref, ttype, (int)resolved, (unsigned long long)tva,
                              (unsigned long long)tgpa, (unsigned long long)leaf_pfn,
                              (unsigned long long)leaf_sz, sizeof(px)/4, nonblack, nonzero);
                    /* Raw texture-object descriptor (first 64 B at tva) — the
                     * format for type 0x03/0x05 texture objects is not yet RE'd;
                     * dump as u64 words to decode width/height/format/backing. */
                    LAGFX_LOG("P6b TEXPROBE ref=0x%x desc u64: %016llx %016llx %016llx %016llx "
                              "%016llx %016llx %016llx %016llx", tref,
                              (unsigned long long)lagfx_le64(tdesc+0),  (unsigned long long)lagfx_le64(tdesc+8),
                              (unsigned long long)lagfx_le64(tdesc+16), (unsigned long long)lagfx_le64(tdesc+24),
                              (unsigned long long)lagfx_le64(tdesc+32), (unsigned long long)lagfx_le64(tdesc+40),
                              (unsigned long long)lagfx_le64(tdesc+48), (unsigned long long)lagfx_le64(tdesc+56));
                }
                LAGFX_LOG("P6b bind#%u slot=%u texture unresolved — skip draw (no crash)",
                          binding_no[i], slot);
                for (uint32_t k = 0; k < *out_n; k++) {
                    vkDestroyBuffer(vk->device, out_bufs[k], NULL);
                    vkFreeMemory(vk->device, out_mems[k], NULL);
                }
                vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
                *out_n = 0;
                return VK_NULL_HANDLE;
            }
            iinfos[nw] = (VkDescriptorImageInfo){ .imageView = view, .imageLayout = lay };
            writes[nw] = (VkWriteDescriptorSet){
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = set,
                .dstBinding      = binding_no[i],
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo      = &iinfos[nw],
            };
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
                    if (task->pending_pipeline.n_vtx_inputs > 0u)
                        vslot = slot + 1u;
                    if (vslot < LAGFX_MAX_BINDING_SLOTS
                        && task->bindings.vertex_buffers[vslot].valid)
                        bs = &task->bindings.vertex_buffers[vslot];
                    else if (task->bindings.vertex_buffers[slot].valid)
                        bs = &task->bindings.vertex_buffers[slot];
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
                    /* GATED (LAGFX_M2_REBIND): binding the descriptor's first
                     * non-zero PFN range is a heuristic — it often picks the
                     * WRONG indirection level (the arg-buffer needs following to
                     * the actual resource), and wrong-but-real data drives the
                     * shader to crash lavapipe (hard SIGSEGV in Mesa, mid-draw,
                     * uncatchable). Off by default keeps production stable
                     * (degenerate-but-no-crash); the M2 work iterates with it on. */
                    /* Bind the buffer's REAL data (the MVP matrix, vertex data,
                     * etc.) instead of the placement descriptor. Active when
                     * LAGFX_M2_REBIND or LAGFX_VTX_INPUT is set — the vertex
                     * shaders compute position = MVP * vertex_input, so the MVP
                     * [[buffer]] MUST be real or every position collapses to 0.
                     * The descriptor is (size@e*16, PFN@e*16+8) 16-byte entries;
                     * the data is at PFN<<12 read as a guest VA (translate). The
                     * 64 KiB best-effort read zero-pads the tail so a fixed 64 B
                     * MVP binds AND a dynamic over-read returns zeros (no crash). */
                    if ((getenv("LAGFX_M2_REBIND") || getenv("LAGFX_VTX_INPUT"))
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
                                lagfx_read_virtual_besteffort(p, task, dva,
                                                              LAGFX_DRAW_DS_BUF_SZ, data);
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
                                /* LAGFX_SIMPLE_ONLY: skip large (arg-buffer/heap)
                                 * ranges — only plain small uniform buffers bind raw. */
                                if (getenv("LAGFX_SIMPLE_ONLY") && rsize > 512u) {
                                    vkFreeDescriptorSets(vk->device,
                                                         vk->draw_desc_pool, 1, &set);
                                    for (uint32_t k = 0; k < *out_n; k++) {
                                        vkDestroyBuffer(vk->device, out_bufs[k], NULL);
                                        vkFreeMemory(vk->device, out_mems[k], NULL);
                                    }
                                    *out_n = 0;
                                    return VK_NULL_HANDLE;
                                }
                                break;
                            }
                            acc += rsize;
                        }
                    }
                }
            }
        }
        if (have) {
            /* SKIPBLACKFILL: track whether THIS fragment buffer carries a non-black
             * colour. ColorFill's fragment colour is a vec4 at the buffer start
             * (RGB at bytes 0..11; e.g. blue = non-zero, black = all-zero). A
             * fragment storage buffer (binding >= FRAG_BASE under texcomp) with any
             * non-zero RGB means this is a COLOURED fill (keep it, e.g. the blue
             * strip); all-zero across all fragment buffers = a black fill (skip). */
            if (binding_no[i] >= LAGFX_FRAG_BINDING_BASE
                && (data[0] | data[1] | data[2] | data[4] | data[5] | data[6]
                    | data[8] | data[9] | data[10]))
                sbf_nonblack = true;
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
    /* SKIPBLACKFILL: a no-texture composite with no non-black fragment colour is
     * a fullscreen BLACK fill (ColorFill) → skip so it doesn't clobber the
     * texture-composite UI layer in the shared RT. The dark login bg comes from
     * the texture composites; coloured fills (blue strip) keep sbf_nonblack=true. */
    if (m2_skipblackfill && !sbf_had_tex && !sbf_nonblack) {
        for (uint32_t k = 0; k < *out_n; k++) {
            vkDestroyBuffer(vk->device, out_bufs[k], NULL);
            vkFreeMemory(vk->device, out_mems[k], NULL);
        }
        vkFreeDescriptorSets(vk->device, vk->draw_desc_pool, 1, &set);
        *out_n = 0;
        return VK_NULL_HANDLE;
    }
    vkUpdateDescriptorSets(vk->device, nw, writes, 0, NULL);
    return set;
}

/* Upload the guest's vertex buffer (vertex_buffers[0], real data via the
 * placement descriptor PFN<<12 + page-table translate — the M2 path) into a
 * host VkBuffer for vertex-input binding. Returns VK_NULL_HANDLE if there is no
 * real vertex data; the caller frees the returned buffer + *out_mem. */
static VkBuffer lagfx_upload_guest_vertex_buffer(lagfx_protocol_t *p,
                                                 lagfx_task_entry_t *task,
                                                 struct lagfx_vk_state *vk,
                                                 VkDeviceMemory *out_mem) {
    *out_mem = VK_NULL_HANDLE;
    if (!task->bindings.vertex_buffers[0].valid
        || task->bindings.vertex_buffers[0].ref == 0u)
        return VK_NULL_HANDLE;
    lagfx_binding_slot_t *vbs = &task->bindings.vertex_buffers[0];
    uint8_t vtype = 0; uint64_t vva = 0, vgpa = 0;
    if (!lagfx_resolve_object_data(p, task, vbs->ref, &vtype, &vva, &vgpa) || vva == 0u)
        return VK_NULL_HANDLE;
    uint8_t vdesc[64];
    if (!lagfx_task_read_virtual(p, task, vva, sizeof(vdesc), vdesc))
        return VK_NULL_HANDLE;
    uint8_t vdata[LAGFX_DRAW_DS_BUF_SZ];
    /* B8: LOGICAL scatter-gather walk (same as the descriptor-binding path) — the
     * placement descriptor's {size,PFN} entries tile the buffer's LOGICAL address
     * space; read vbs->offset by accumulating sizes and reading PFN<<12 + (offset -
     * range_start). The old first-non-zero-probe mis-bound to the wrong physical
     * range when an entry's page started with zero bytes (a leading-zero vertex
     * attribute) → garbage vertex data → degenerate geometry. Mask the PFN's high
     * 32 bits (flags live there for some object types). */
    uint64_t vacc = 0;
    for (int e = 0; e < 4; e++) {
        uint64_t rsize = lagfx_le64(vdesc + (size_t)e * 16u);
        uint64_t pfn = lagfx_le64(vdesc + (size_t)e * 16u + 8u) & 0xffffffffull;
        if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
        if (vbs->offset < vacc + rsize) {
            uint64_t dva = (pfn << 12) + (vbs->offset - vacc);
            if (lagfx_read_virtual_besteffort(p, task, dva, LAGFX_DRAW_DS_BUF_SZ, vdata)) {
                VkBuffer vb = VK_NULL_HANDLE;
                if (lagfx_vk_make_host_storage_buffer(vk, vdata, LAGFX_DRAW_DS_BUF_SZ,
                                                      &vb, out_mem) == LAGFX_OK) {
                    /* Dump first 3 vertices as floats assuming tight pack
                     * [pos.xy(8), tex.xy(8), perspective(4)] stride 20. Reveals
                     * whether the _tex UVs are valid [0,1] or degenerate (→ the
                     * composite samples black). */
                    float vf[15];
                    for (int q = 0; q < 15; q++) {
                        uint32_t u = lagfx_le32(vdata + q*4); memcpy(&vf[q], &u, 4);
                    }
                    LAGFX_LOG("VTX: ref=0x%x off=%llu PFN0x%llx v0[pos %.3g,%.3g tex %.3g,%.3g] "
                              "v1@20[%.3g,%.3g %.3g,%.3g] (stride20 assumed)", vbs->ref,
                              (unsigned long long)vbs->offset, (unsigned long long)pfn,
                              vf[0],vf[1],vf[2],vf[3], vf[5],vf[6],vf[7],vf[8]);
                    return vb;
                }
            }
            return VK_NULL_HANDLE;
        }
        vacc += rsize;
    }
    return VK_NULL_HANDLE;
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
static void lagfx_emit_pending_draw(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                                    const char *op, uint32_t count) {
    if (!(task->pending_pipeline.valid && task->render_pass_desc.valid)) {
        return;
    }
    lagfx_device_t *dev_with_vk = (lagfx_device_t *)p->dev;
    if (!(dev_with_vk && dev_with_vk->vk && dev_with_vk->vk->initialized)) {
        return;
    }
    VkDevice device = dev_with_vk->vk->device;
    lagfx_pipeline_desc_t pdesc = {
        .vertex_shader        = (VkShaderModule)task->pending_pipeline.vertex_shader,
        .fragment_shader      = (VkShaderModule)task->pending_pipeline.fragment_shader,
        /* Resource-using translated pipelines use their reflected pipeline
         * layout; substitute/resource-free use the device empty layout. */
        .layout               = (task->pending_pipeline.translated
                                  && task->pending_pipeline.pipeline_layout)
                                  ? (VkPipelineLayout)task->pending_pipeline.pipeline_layout
                                  : dev_with_vk->vk->empty_layout,
        .vertex_entry_point   = task->pending_pipeline.translated ? "main" : "triangle_vertex",
        .fragment_entry_point = task->pending_pipeline.translated ? "main" : "triangle_fragment",
        .color_format         = (VkFormat)task->render_pass_desc.color_format,
        .depth_format         = (VkFormat)task->render_pass_desc.depth_format,
    };
    /* Vertex input (gated by LAGFX_VTX_INPUT): feed the vertex shader's
     * reflected stage-in attributes from a bound guest vertex buffer. Without
     * this the positions read unbound → 0 → degenerate → black. */
    bool vtx_input_on = task->pending_pipeline.translated
                        && task->pending_pipeline.n_vtx_inputs > 0u
                        && getenv("LAGFX_VTX_INPUT") != NULL;
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
    if (getenv("LAGFX_M2_PERPASS") && task->render_pass_desc.target_ref != 0u) {
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
                if (ne) { ne->host_handle = ios; ne->image = ios->image; ne->view = ios->view; }
                LAGFX_LOG("%s PERPASS: created RT IOSurface for target ref=0x%x %ux%u", op, tref, W, H);
            }
        }
        if (ios && ios->image != VK_NULL_HANDLE
            && lagfx_vk_render_target_wrap(ios->image, ios->view, ios->memory,
                                           ios->width, ios->height, ios->format,
                                           &perpass_rt) == LAGFX_OK) {
            active_rt = &perpass_rt;
            ios->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; /* sampleable after render */
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
    VkBuffer vbuf = VK_NULL_HANDLE; VkDeviceMemory vbmem = VK_NULL_HANDLE;
    if (vtx_input_on) {
        vstride = 0u;
        for (uint32_t a = 0; a < task->pending_pipeline.n_vtx_inputs && a < 8u; a++)
            vstride += (uint32_t)task->pending_pipeline.vtx_in_comp[a] * 4u;
        if (vstride == 0u) vstride = 16u;
        vbuf = lagfx_upload_guest_vertex_buffer(p, task, dev_with_vk->vk, &vbmem);
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
    uint32_t vmax = LAGFX_DRAW_DS_BUF_SZ / vstride;
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
            lagfx_display_signal_frame_ready(display);
        } else {
            LAGFX_WARN("%s VTX: vertex-input draw failed (%d)", op, (int)st);
        }
    } else if (vtx_input_on) {
        /* vtx-input pipeline but no real vertex data → skip (no unbound fault). */
        LAGFX_LOG("%s VTX: ref=0x%x no real vertex data — skip (no crash)",
                  op, task->pending_pipeline.reference);
    } else if (!(!task->pending_pipeline.translated && getenv("LAGFX_NO_SUBSTITUTE"))) {
        /* Substitute / resource-free path: bundled 3-vertex triangle. */
        st = lagfx_vk_draw_record_and_submit(
            dev_with_vk->vk, pipeline, active_rt, false, 3, 1, 0, 0, 0);
        if (st == LAGFX_OK) {
            LAGFX_LOG("%s: drew substitute triangle (guest req count=%u)", op, count);
            lagfx_display_signal_frame_ready(display);
        } else {
            LAGFX_WARN("%s: substitute draw failed (%d)", op, (int)st);
        }
    }

    if (vbuf != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vbuf, NULL);
        vkFreeMemory(device, vbmem, NULL);
    }
}
#endif /* LAGFX_HAVE_VULKAN */

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

    /* M1 (a): resource-aware draw via the shared helper (was op_0x01-only). */
#ifdef LAGFX_HAVE_VULKAN
    lagfx_emit_pending_draw(p, task, "op_0x01", vertex_count);
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
                if (getenv("LAGFX_M2_PERPASS"))
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
    if (getenv("LAGFX_PHASE6_TRANSLATE")) {
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

        /* M2 ARGBUF (LAGFX_M2_ARGBUF): test the argument-buffer/bindless hypothesis —
         * the wallpaper texture handle is likely embedded INSIDE a fragment argument
         * buffer (a guest buffer of resource handles the shader indexes), never bound
         * as a 0x72 texture ref. Read the bound buffer's contents and scan its u32
         * words for any that resolve to a real texture object; log the resolved
         * type+size. A wallpaper-sized hit (MBs) here CONFIRMS bindless and yields the
         * handle→texture resolution path. Slots >=1 only (slot 0 = small uniforms).
         * Gated, read-only probe — no binding/render change. */
        if (getenv("LAGFX_M2_ARGBUF") && ref != 0u && slot_index >= 1u) {
            uint8_t bt = 0; uint64_t bva = 0, bgpa = 0;
            if (lagfx_resolve_object_data(p, task, ref, &bt, &bva, &bgpa) && bva != 0u) {
                uint8_t abuf[256] = {0};
                if (lagfx_task_read_virtual(p, task, bva + offset, sizeof(abuf), abuf)) {
                    for (int w = 0; w < 64; w++) {
                        uint32_t h = lagfx_le32(abuf + (size_t)w * 4u);
                        if (h == 0u || h == ref || h > 0xffffu) continue;
                        uint8_t ht = 0; uint64_t hva = 0, hgpa = 0;
                        if (lagfx_resolve_object_data(p, task, h, &ht, &hva, &hgpa) && hva != 0u
                            && (ht == 0x03u || ht == 0x04u || ht == 0x05u)) {
                            uint8_t hd[64] = {0}; uint64_t hsz = 0, hpf = 0;
                            if (lagfx_task_read_virtual(p, task, hva, sizeof(hd), hd)) {
                                hsz = lagfx_le64(hd + 0);
                                hpf = lagfx_le64(hd + 8) & 0xffffffffull;
                            }
                            LAGFX_LOG("M2 ARGBUF buf=0x%x(t%02x)+%llu word[%d]=0x%x -> tex "
                                      "type=0x%02x sz=%llu pfn=0x%llx", ref, bt,
                                      (unsigned long long)offset, w, h, ht,
                                      (unsigned long long)hsz, (unsigned long long)hpf);

                            /* M2 ARGUPLOAD: the embedded handle resolves to a REAL
                             * type-0x03 texture with live pixels at THIS (0x6e bind)
                             * time — but draw-time TEXBACK fails to resolve it (the
                             * eviction/timing gap). Upload the pixels NOW, while live,
                             * and register under the handle ref so the later sampling
                             * draw binds real content instead of black. Reuses the
                             * TEXBACK derivation (bytesPerRow @ desc word[4]) + upload.
                             * Once per handle (skip if already host-backed). Gated. */
                            if (getenv("LAGFX_M2_ARGUPLOAD") && ht == 0x03u
                                && hpf >= 0x10u && hpf <= 0xfffffu
                                && hsz >= 256u && hsz <= 16u * 1024u * 1024u
                                && (hsz % 4u) == 0u) {
                                lagfx_resource_entry_t *ex =
                                    lagfx_resource_lookup_texture(&p->resources, h);
                                if (!(ex && ex->view != VK_NULL_HANDLE)) {
                                    lagfx_device_t *dwv = (lagfx_device_t *)p->dev;
                                    if (dwv && dwv->vk && dwv->vk->initialized) {
                                        uint32_t bpr = lagfx_le32(hd + 16);
                                        uint32_t total_px = (uint32_t)(hsz / 4u);
                                        uint32_t W = 1u, H = 1u;
                                        if (bpr >= 4u && (bpr % 4u) == 0u && (hsz % bpr) == 0u
                                            && (hsz / bpr) <= 8192u && (bpr / 4u) <= 8192u) {
                                            W = bpr / 4u; H = (uint32_t)(hsz / bpr);
                                        } else {
                                            uint32_t s = 1u; while (s * s < total_px) s++;
                                            if (s * s == total_px) { W = s; H = s; }
                                            else for (uint32_t c = 256u; c >= 1u; c >>= 1)
                                                if (total_px % c == 0u) { W = c; H = total_px / c; break; }
                                        }
                                        size_t rl = (size_t)W * H * 4u;
                                        if (rl > 8u * 1024u * 1024u) rl = 8u * 1024u * 1024u;
                                        uint8_t *pix = malloc(rl);
                                        lagfx_vk_iosurface_t *ios = NULL;
                                        if (pix
                                            && lagfx_read_virtual_besteffort(p, task, hpf << 12, rl, pix)
                                            && lagfx_vk_iosurface_create(dwv->vk, W, H, 80u, &ios) == LAGFX_OK
                                            && lagfx_vk_iosurface_upload_pixels(dwv->vk, ios, pix, rl) == LAGFX_OK) {
                                            lagfx_resource_register(&p->resources, h,
                                                                    LAGFX_RESOURCE_TYPE_TEXTURE,
                                                                    task->id, hpf << 12, hsz);
                                            lagfx_resource_entry_t *ne =
                                                lagfx_resource_lookup_texture(&p->resources, h);
                                            if (ne) { ne->host_handle = ios;
                                                      ne->image = ios->image; ne->view = ios->view; }
                                            uint32_t nb = 0;
                                            for (size_t q = 0; q + 4 <= rl; q += 4)
                                                if (pix[q] | pix[q+1] | pix[q+2]) nb++;
                                            LAGFX_LOG("M2 ARGUPLOAD handle=0x%x backed %ux%u from "
                                                      "PFN0x%llx (%llu B) nonblack_px=%u/%zu", h, W, H,
                                                      (unsigned long long)hpf,
                                                      (unsigned long long)hsz, nb, rl / 4);
                                        } else if (ios) {
                                            lagfx_vk_iosurface_destroy(dwv->vk, ios);
                                        }
                                        free(pix);
                                    }
                                }
                            }
                        }
                    }
                }
            }
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
    if (getenv("LAGFX_PHASE6_TRANSLATE")) {
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
    if (getenv("LAGFX_PHASE6_TRANSLATE")) {
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
             getenv("LAGFX_PHASE6_TRANSLATE") != NULL)) {
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

    /* Phase B step 6/7: env-gated diagnostic using new object resolver helpers. */
    if (getenv("LAGFX_PHASE_B_LOOKUP") != NULL && task->heap_pfn != 0u) {
        static uint32_t lookup_count = 0u;
        if (lookup_count < 20u) {
            uint8_t vert_ref = 0, frag_ref = 0;
            if (lagfx_lookup_pipeline_function_refs(p, task, reference, &vert_ref, &frag_ref)) {
                uint64_t v_gpa = 0; uint32_t v_len = 0;
                uint64_t f_gpa = 0; uint32_t f_len = 0;
                bool got_v = lagfx_lookup_function_bytes(p, task, vert_ref, &v_gpa, &v_len, NULL);
                bool got_f = (frag_ref != 0) ? lagfx_lookup_function_bytes(p, task, frag_ref, &f_gpa, &f_len, NULL) : false;
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
                    uint64_t vert_va = 0, frag_va = 0;

                    bool got_vert = lagfx_lookup_function_bytes(p, task, vert_ref, &vert_gpa, &vert_len, &vert_va);
                    if (got_vert && vert_len > 0) {
                        /* Allocate buffer on heap — metallibs are ~4-6 KB. */
                        uint8_t *buf = (uint8_t *)malloc(vert_len);
                        if (buf != NULL) {
                            /* Page-aware read: the metallib is virtually
                             * contiguous but its GPA pages are not, so a flat
                             * read past the first page boundary is corrupt. */
                            bool ok_read = lagfx_task_read_virtual(
                                p, task, vert_va, vert_len, buf);

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
                    if (frag_ref != 0) {
                        got_frag = lagfx_lookup_function_bytes(p, task, frag_ref, &frag_gpa, &frag_len, &frag_va);
                    }

                    if (got_frag && frag_len > 0 && frag_ref != 0) {
                        uint8_t *buf = (uint8_t *)malloc(frag_len);
                        if (buf != NULL) {
                            /* Page-aware read (see vertex capture above). */
                            bool ok_read = lagfx_task_read_virtual(
                                p, task, frag_va, frag_len, buf);

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
                if (stage == 1 && getenv("LAGFX_M1_TEXCOMP")) {
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
