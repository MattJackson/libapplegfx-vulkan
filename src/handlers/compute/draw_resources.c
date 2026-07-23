/*
 * libapplegfx-vulkan — draw-path guest-memory reads + content scoring
 * src/handlers/compute/draw_resources.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "compute_draw_internal.h"
#include "display.h"
#include "protocol/object_resolver.h"
#include "task_translate.h"

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

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int lagfx_draw_resources_not_empty_t;

#ifdef LAGFX_HAVE_VULKAN

/* Best-effort guest-VA read: zero-fill `buf`, then read page by page, stopping
 * at the first unmapped page (keeping what was read). Returns true if at least
 * the first page read. Unlike lagfx_task_read_virtual (all-or-nothing), this
 * lets a small real buffer (e.g. a 64 B MVP matrix) bind from a large request
 * by zero-padding the tail, and bounds a shader's dynamic over-read to zeros. */
bool lagfx_read_virtual_besteffort(lagfx_protocol_t *p,
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

/* Read a TEXTURE BACKING whose descriptor PFN is a guest-PHYSICAL frame number
 * (gpa = pfn<<12). The texture-object descriptor's PFN|flags word is a physical
 * frame, NOT a task-virtual address — so it must be read with a raw guest-
 * physical read (shell.read_memory), the same primitive the ring dispatchers
 * use, NOT lagfx_task_read_virtual (which page-table-translates and lands on the
 * wrong page → all-zeros for e.g. the 1280×1024 wallpaper texture 0x10, whose
 * pixels pmemsave proves live at the raw GPA). Reads raw GPA first; if that
 * yields all-zeros (or no read_memory callback), falls back to the virtual
 * best-effort read so any genuinely VA-backed resource still resolves. Returns
 * true if either path produced data. Proven 2026-06-11: raw GPA 0x741000 =
 * 73.5% nonblack wallpaper; virtual read of the same pfn<<12 = 0% (mistranslated). */
bool lagfx_read_resource_backing(lagfx_protocol_t *p,
                                       const lagfx_task_entry_t *task,
                                       uint64_t gpa, uint32_t len, uint8_t *buf) {
    /* DUMB-FAITHFUL (M2q): the placement descriptor's page_index is a per-task
     * VIRTUAL page — the decoded Apple host contract resolves backings via
     * -[PGLocalTask newBufferForVirtualPage:length:] (CONTRACTDIAG-proven:
     * ref 0x17 page_index → VA → GPA translate=1 → the real wallpaper bytes).
     * Read it the ONE contract way: VA-translated through the per-task radix.
     * DETERMINISTIC fallback only when the address does not translate at all
     * (identity-mapped allocation): raw guest-physical read. No content
     * scoring, no arbitration — the old READARB float-plausibility pick and
     * the raw-unless-zero heuristics chose reads by content and are removed. */
    if (task) {
        uint64_t probe_gpa = 0;
        if (lagfx_task_translate(p, task, gpa, &probe_gpa))
            return lagfx_read_virtual_besteffort(p, task, gpa, len, buf);
    }
    lagfx_device_t *dev = p ? (lagfx_device_t *)p->dev : NULL;
    if (dev && dev->desc.shell.read_memory
        && dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, len, buf))
        return true;
    return lagfx_read_virtual_besteffort(p, task, gpa, len, buf);
}

/* Read a binding slot's actual buffer content (resolve ref → placement descriptor
 * → walk ranges to the slot's offset → raw-GPA read). Returns false if the slot
 * is invalid/unresolvable. Used to SCAN candidate vertex-buffer slots for the one
 * carrying the viewport matrix (the vertex MVP), since the reflected-binding →
 * guest-slot map is ambiguous for multi-buffer composites. */
bool lagfx_read_binding_slot(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                                    const lagfx_binding_slot_t *bs,
                                    uint8_t *out, size_t len) {
    if (!bs || !bs->valid || bs->ref == 0u) return false;
    uint8_t type = 0; uint64_t va = 0, gpa = 0;
    if (!lagfx_resolve_object_data(p, task, bs->ref, &type, &va, &gpa) || va == 0u)
        return false;
    uint8_t desc[64];
    if (!lagfx_task_read_virtual(p, task, va, sizeof(desc), desc)) return false;
    uint64_t acc = 0;
    for (int e = 0; e < 4; e++) {
        uint64_t rsize = lagfx_le64(desc + (size_t)e * 16u);
        uint64_t pfn = lagfx_le64(desc + (size_t)e * 16u + 8u) & 0xffffffffull;
        if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
        if (bs->offset < acc + rsize) {
            uint64_t dva = (pfn << 12) + (bs->offset - acc);
            return lagfx_read_resource_backing(p, task, dva, (uint32_t)len, out);
        }
        acc += rsize;
    }
    return false;
}

/* True if `data`'s first 32 bytes look like a viewport/MVP transform matrix:
 * column 1's y-component (float[5]) is a small non-zero scale (~ -2/H). The
 * WRONG buffer (screen dims) has float[5]==0 → gl_Position.y constant → the
 * quad collapses to a band. This signature robustly distinguishes the matrix
 * from the screen-dims / colour buffers regardless of slot index. */
bool lagfx_looks_like_mvp_matrix(const uint8_t *data) {
    float c1y; uint32_t u = lagfx_le32(data + 20); memcpy(&c1y, &u, 4);
    float c0x; uint32_t u0 = lagfx_le32(data + 0); memcpy(&c0x, &u0, 4);
    float ac1 = c1y < 0 ? -c1y : c1y, ac0 = c0x < 0 ? -c0x : c0x;
    return (ac1 > 1e-6f && ac1 < 1.0f) && (ac0 > 1e-6f && ac0 < 1.0f);
}



/* M2c VTXSRC: score a byte buffer's plausibility as float32 vertex data —
 * fraction of nonzero dwords that read as sane finite floats (|x| in
 * [1e-6, 1e6]). Text/poison slabs (UTF-16 boot log, 0xFFFFFFFF fill) score ~0;
 * real position/texcoord streams score high. */
uint32_t lagfx_vtx_float_plausibility(const uint8_t *b, uint32_t len) {
    uint32_t sampled = 0, sane = 0;
    for (uint32_t o = 0; o + 4u <= len && sampled < 64u; o += 4u) {
        uint32_t d = lagfx_le32(b + o);
        if (d == 0u) continue;
        sampled++;
        float f; memcpy(&f, &d, 4);
        float m = f < 0 ? -f : f;
        if (f == f && m >= 1e-6f && m <= 1e6f) sane++;
    }
    return sampled ? (sane * 100u) / sampled : 0u;
}

/* Distinguish a per-vertex POSITION stream from a matrix/uniform block. The
 * float-plausibility scorer can't: a pixel→NDC matrix (values ~0.0015, ±1) and
 * a screen-space vertex (values ~572, 87) are both "plausible floats". But a
 * real vertex stream has, at the per-vertex STRIDE, position.x values that (a)
 * span screen-pixel range [~0.5, 8192], (b) VARY between consecutive vertices,
 * and (c) are not the tell-tale ±1/tiny-scale of a projection matrix. Returns a
 * 0..100 confidence that `b` is a position stream at `stride`. Grounded in the
 * DTrace ground truth (CA::OGL verts are float4 pixel positions at offset 0). */
uint32_t lagfx_vtx_looks_like_positions(const uint8_t *b, uint32_t len, uint32_t stride) {
    if (stride < 8u || stride > 256u) stride = 48u;
    float xs[16]; uint32_t nv = 0, w_ok = 0;
    for (uint32_t v = 0; (size_t)(v + 1u) * stride <= len && nv < 16u; v++) {
        uint32_t dx = lagfx_le32(b + (size_t)v * stride);
        uint32_t dy = lagfx_le32(b + (size_t)v * stride + 4u);
        uint32_t dw = lagfx_le32(b + (size_t)v * stride + 12u);
        float fx, fy, fw; memcpy(&fx, &dx, 4); memcpy(&fy, &dy, 4); memcpy(&fw, &dw, 4);
        if (fx != fx || fy != fy || fw != fw) continue;           /* NaN */
        /* The homogeneous w (float4 pos @ +12) MUST be ~1.0 for a valid CA
         * composite vertex. w=0 → degenerate (divide-by-zero garbage lines);
         * w=6/29 → shrinks the quad to nothing. Reject streams whose w isn't 1,
         * so VTXSRC never uploads a malformed/misaligned buffer as the geometry. */
        if (fw >= 0.99f && fw <= 1.01f) w_ok++;
        xs[nv++] = fx;
        (void)fy;
    }
    if (nv < 3u) return 0u;
    /* Require the MAJORITY of vertices to have w≈1 — a matrix or misaligned
     * read has w=0/scale/garbage. This is the strongest single discriminator. */
    if ((w_ok * 2u) < nv) return 0u;
    uint32_t in_screen = 0, distinct = 0;
    for (uint32_t i = 0; i < nv; i++) {
        float m = xs[i] < 0 ? -xs[i] : xs[i];
        if (m >= 0.5f && m <= 8192.0f) in_screen++;
        bool seen = false;
        for (uint32_t j = 0; j < i; j++) {
            float d = xs[i] - xs[j]; if (d < 0) d = -d;
            if (d < 0.01f) { seen = true; break; }
        }
        if (!seen) distinct++;
    }
    /* Need most positions in screen range AND genuine variation (a matrix
     * column repeats 0/±1 → few distinct, out of screen range). */
    uint32_t screen_frac = (in_screen * 100u) / nv;
    if (distinct < 2u) return 0u;
    return screen_frac;
}

/* M2c VTXSRC: read `want` bytes of a bound buffer (ref,offset) — prefer the
 * 0x3b BackingUpdate address (task VA, page-aware) when one exists for the
 * ref, else the placement-table walk (entry-size-capped multi-entry gather).
 * Returns bytes filled (0 = fail). */
uint32_t lagfx_read_vtx_source(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                                      uint32_t ref, uint64_t offset,
                                      uint32_t want, uint8_t *out, const char **how,
                                      int mode) {
    /* mode 0: 0x3b BACKUPD addr (VA) preferred, else placement walk with the
     *         standard (RAWGPA-gated) read — the legacy behavior.
     * mode 1: placement walk but read (pfn<<12)+within as a TASK VA through the
     *         per-task radix (lead 3: the slab address may be virtual for some
     *         refs — the raw-GPA read lands on boot-log text for 0x15/0x16/0x18). */
    *how = "none";
    if (mode == 0) {
        for (uint32_t bi = 0; bi < p->backing_update_n && bi < 64u; bi++) {
            if (!p->backing_update[bi].valid || p->backing_update[bi].ref != ref) continue;
            if (lagfx_task_read_virtual(p, task, p->backing_update[bi].addr + offset,
                                        want, out)) {
                *how = "backupd-va";
                return want;
            }
            break;
        }
    }
    uint8_t vtype = 0; uint64_t vva = 0, vgpa = 0;
    if (!lagfx_resolve_object_data(p, task, ref, &vtype, &vva, &vgpa) || vva == 0u)
        return 0u;
    uint8_t vdesc[64];
    if (!lagfx_task_read_virtual(p, task, vva, sizeof(vdesc), vdesc))
        return 0u;
    uint64_t vacc = 0, logical = offset;
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
            bool ok = (mode == 1)
                          ? lagfx_task_read_virtual(p, task, (pfn << 12) + within,
                                                    chunk, out + filled)
                          : lagfx_read_resource_backing(p, task, (pfn << 12) + within,
                                                        chunk, out + filled);
            if (!ok) break;
            filled += chunk;
            logical += chunk;
        }
        vacc += rsize;
    }
    if (filled) *how = (mode == 1) ? "placement-va" : "placement";
    return filled;
}


/* Realize a bound texture ref as a sampleable IOSurface filled with the
 * guest's REAL pixel bytes. The type-0x03 texture object's placement
 * descriptor is walked like the vertex path ({size,pfn} entries); the bytes
 * are read BOTH raw-GPA and VA-translated and the read with the higher
 * NONBLACK pixel fraction wins (pixel data scores meaninglessly as floats,
 * so the float arbitration cannot decide here). Dims are inferred from the
 * byte size at BGRA8: first 16-aligned width from a preference list whose
 * aspect lands in [0.4, 2.5] — the 20 KiB login-UI textures resolve to
 * 64x80, the 4 KiB cursor to 32x32. The surface is registered under the
 * ref so later draws bind it directly. Returns NULL on any miss. */
lagfx_vk_iosurface_t *lagfx_texture_realize(lagfx_protocol_t *p,
                                            lagfx_task_entry_t *task,
                                            struct lagfx_vk_state *vk,
                                            uint32_t tref) {
    if (!p || !task || !vk || tref == 0u) return NULL;
    uint8_t ttype = 0; uint64_t tva = 0, tgpa = 0;
    /* Kinds 0x02, 0x03 and 0x0c ALL dispatch to createNormalTexture in
     * Apple's createObject<PGResource*> jump table (object-delegate-path.md
     * 0x22db2f85c) — same MTLTexture constructor, same descriptor. The
     * 0x03-only check made every type-0x02 texture unrealizable: the Xgc
     * login-panel pipeline's blur/backdrop sources are 0x02, so ALL its
     * draws were skipped (te=0, GOAL-M2aa). The descriptor plausibility
     * checks below (placement sizes, stride/height words) still reject a
     * layout mismatch safely. */
    if (!lagfx_resolve_object_data(p, task, tref, &ttype, &tva, &tgpa)
        || tva == 0u
        || (ttype != 0x03u && ttype != 0x02u && ttype != 0x0cu)) {
        /* GOAL-M2aa: the Xgc login-panel pipeline skips its draws on
         * exactly these bails (te=0 texture refs) — log WHY so the panel's
         * blur/backdrop source-texture type is identifiable from one boot. */
        LAGFX_LOG("TEXREALIZE-BAIL ref=0x%x resolve=%d type=0x%02x va=0x%llx gpa=0x%llx",
                  tref,
                  lagfx_resolve_object_data(p, task, tref, &ttype, &tva, &tgpa) ? 1 : 0,
                  ttype, (unsigned long long)tva, (unsigned long long)tgpa);
        return NULL;
    }
    uint8_t desc[64];
    if (!lagfx_task_read_virtual(p, task, tva, sizeof(desc), desc))
        return NULL;
    /* Texture-object descriptor RE (TEXDESC): dump the raw 64 bytes as u32
     * words so the width/height/rowBytes/format fields can be calibrated from
     * the known-dim small textures (ref 0x16=32x32, 0x31=64x81) and applied to
     * the wallpaper (ref 0x10). The dims are currently INFERRED from byte size,
     * which loses the true rowBytes/stride — the banded-scramble root cause. */
    LAGFX_LOG("TEXDESC ref=0x%x tva=0x%llx w[%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u]",
              tref, (unsigned long long)tva,
              lagfx_le32(desc+0),  lagfx_le32(desc+4),  lagfx_le32(desc+8),  lagfx_le32(desc+12),
              lagfx_le32(desc+16), lagfx_le32(desc+20), lagfx_le32(desc+24), lagfx_le32(desc+28),
              lagfx_le32(desc+32), lagfx_le32(desc+36), lagfx_le32(desc+40), lagfx_le32(desc+44),
              lagfx_le32(desc+48), lagfx_le32(desc+52), lagfx_le32(desc+56), lagfx_le32(desc+60));
    /* CONTRACTDIAG (M2n, gated): the decoded Apple host contract resolves an
     * IOSurface backing via -[PGLocalTask newBufferForVirtualPage:length:] —
     * i.e. treat the descriptor's page_index (placement pfn, blob+0x08) as a
     * per-task VIRTUAL page and walk the per-task page table (the op-0x39
     * commits). This logs, for the requested ref, whether that VA→GPA
     * translation SUCCEEDS and to which GPA — the single discriminator between
     * (a) a missing 0x39 mapping [translate fails => real host bug to chase]
     * and (b) the guest never DMAing the pixels [translate ok but reads
     * black => guest-side gap, needs guest ground-truth]. Diagnostic only;
     * production resolution is unchanged. See iosurface-backing-contract-RE.md. */
    if (getenv("LAGFX_M2_CONTRACTDIAG")) {
        uint64_t page_index = lagfx_le64(desc + 8u) & 0xffffffffull; /* blob+0x08 */
        uint64_t va = page_index << 12, gpa = 0;
        bool xok = lagfx_task_translate(p, task, va, &gpa);
        LAGFX_LOG("CONTRACTDIAG ref=0x%x page_index=0x%llx newBufferForVirtualPage-VA=0x%llx "
                  "translate=%d gpa=0x%llx",
                  tref, (unsigned long long)page_index, (unsigned long long)va,
                  xok ? 1 : 0, (unsigned long long)gpa);
    }
    uint64_t total = 0;
    for (int e = 0; e < 4; e++) {
        uint64_t rsize = lagfx_le64(desc + (size_t)e * 16u);
        uint64_t pfn = lagfx_le64(desc + (size_t)e * 16u + 8u) & 0xffffffffull;
        if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
        total += rsize;
    }
    /* 128 MiB (was 8 MiB): the login panel samples a 7680x3840 Retina-scale
     * backdrop (ref TEXDESC: 66,355,200 B, stride 30720, height 3840) that
     * the old cap silently rejected — its pipeline's draws then skipped on
     * descriptor build (879/boot). Realized surfaces are registered under
     * their ref, so the big upload happens once, not per draw. */
    if (total < 4096u || total > 128u * 1024u * 1024u) return NULL;
    uint32_t want = (uint32_t)total;
    /* DUMB-FAITHFUL (M2q): ONE read, the contract way — 0x3b BackingUpdate
     * address when the guest announced one, else the placement walk whose
     * backing read VA-translates the descriptor page_index (CONTRACTDIAG).
     * No raw-vs-va scoring, no colour/photo picking: bind what the guest's
     * declared backing holds, even if that is currently black. */
    uint8_t *pick = calloc(1u, want);
    if (!pick) return NULL;
    const char *how = "none";
    uint32_t got = lagfx_read_vtx_source(p, task, tref, 0u, want, pick, &how, 0);
    if (!got) { free(pick); return NULL; }
    uint32_t nb = 0;
    for (uint32_t o = 0; o + 4u <= want; o += 4u)
        if (pick[o] | pick[o+1] | pick[o+2]) nb++;
    /* Dims from the DESCRIBED contract, not size inference: the type-0x03
     * descriptor carries rowBytes at word 13 and height at word 15 (RE'd from
     * ref 0x10=1280x1024 stride 5120, 0x16=32x17 stride 128, 0x2e=240x234
     * stride 960). width = rowBytes/4. Fall back to size inference only when
     * the descriptor fields are implausible. */
    uint32_t npx = want / 4u, W = 0, H = 0;
    uint32_t desc_stride = lagfx_le32(desc + 52);   /* word 13 */
    uint32_t desc_h      = lagfx_le32(desc + 60);   /* word 15 */
    /* The format word's TOP BYTE is bytes-per-pixel: 0x04010001 = BGRA8
     * (4 Bpp), 0x08010001 = RGBA16F (8 Bpp; the 3840x2160 login backdrop —
     * 66,355,200 B = stride 30720 x 2160 rows). The old 4-Bpp-only
     * plausibility check rejected 8-Bpp descriptors and the size-inference
     * fallback fabricated 64x259200 — beyond lavapipe's max dimension,
     * SIGSEGV on upload. 8-Bpp surfaces get a true RGBA16F VkImage
     * (W = stride/8) — modelling them as doubled-width BGRA8 fed the panel
     * half-float bytes as colour/alpha, i.e. near-black with garbage alpha.
     * 1-Bpp (A8 glyph atlases) get a true R8_UNORM VkImage (W = stride) with
     * an (r,r,r,r)-swizzled view — the 4-Bpp byte-model put the coverage
     * bytes in the WRONG CHANNELS (alpha mostly 0 → invisible text).
     * 2-Bpp rows still ride the BGRA8 view (W = stride/4; byte-exact).
     * Height = min(descriptor height, rows that fit the allocation) — small
     * textures pad the allocation (0x16: 23 padded rows vs 17 real), the
     * backdrop under-fills it. */
    uint32_t bpp = (lagfx_le32(desc + 32) >> 24) & 0xFFu;
    if (bpp != 4u && bpp != 8u && bpp != 2u && bpp != 1u) bpp = 4u;
    if (desc_stride >= 4u && ((desc_stride & 3u) == 0u || bpp == 1u) && desc_h > 0u) {
        uint32_t rows_fit = (uint32_t)(want / desc_stride);
        uint32_t Heff = desc_h < rows_fit ? desc_h : rows_fit;
        if (Heff > 0u) {
            W = desc_stride / (bpp == 8u ? 8u : bpp == 1u ? 1u : 4u);
            H = Heff;
        }
    }
    if (!W) {
        static const uint32_t widths[] = {64u, 128u, 32u, 256u, 16u, 512u,
                                          1280u, 1920u, 1024u, 2048u};
        for (size_t wi = 0; wi < sizeof(widths)/sizeof(widths[0]); wi++) {
            uint32_t w = widths[wi];
            if (npx % w) continue;
            uint32_t h = npx / w;
            if (h == 0u) continue;
            float aspect = (float)w / (float)h;
            if (aspect < 0.4f || aspect > 2.5f) continue;
            W = w; H = h; break;
        }
    }
    if (!W) { W = 64u; H = npx / 64u ? npx / 64u : 1u; }
    /* Hard sanity clamp: lavapipe's max image dimension is 16384; an
     * implausible fabricated dim must SKIP, not crash the device. */
    if (W == 0u || H == 0u || W > 16384u || H > 16384u) {
        LAGFX_WARN("TEXREAL: ref=0x%x implausible dims %ux%u — skip",
                   tref, W, H);
        free(pick);
        return NULL;
    }
    LAGFX_LOG("TEXREAL: ref=0x%x dims %ux%u (descriptor stride=%u h=%u bpp=%u)",
              tref, W, H, desc_stride, desc_h, bpp);
    lagfx_vk_iosurface_t *ios = NULL;
    uint32_t mtl_fmt = (bpp == 8u) ? 115u /* RGBA16Float */
                     : (bpp == 1u) ? 10u  /* R8Unorm (A8 coverage, rrrr view) */
                                   : 80u  /* BGRA8 */;
    if (lagfx_vk_iosurface_create(vk, W, H, mtl_fmt, &ios) != LAGFX_OK || !ios) {
        free(pick); return NULL;
    }
    if (lagfx_vk_iosurface_upload_pixels(vk, ios, pick, want) != LAGFX_OK) {
        lagfx_vk_iosurface_destroy(vk, ios);
        free(pick); return NULL;
    }
    free(pick);
    lagfx_resource_entry_t *te = lagfx_resource_lookup_texture(&p->resources, tref);
    if (!te) {
        lagfx_resource_register(&p->resources, tref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                task->id, tgpa, total);
        te = lagfx_resource_lookup_texture(&p->resources, tref);
    }
    if (te) {
        te->host_handle = ios; te->image = ios->image; te->view = ios->view;
        te->realize_seen = 0u;
        te->backing_dirty = 0u;
    } else {
        lagfx_vk_iosurface_destroy(vk, ios);
        return NULL;
    }
    LAGFX_LOG("TEXREAL: ref=0x%x %ux%u from %uB backing (read=%s nonblack=%u/%u)",
              tref, W, H, want, how, nb, npx);
    return ios;
}

/* Re-read a realized texture's guest backing and re-upload it. The wallpaper
 * is CPU-decoded by the guest seconds into boot — a realize during the early
 * draw burst caches the still-black buffer. Called from the sample-bind path;
 * rate-limited; uploads into the SAME VkImage so every later composite picks
 * the new pixels up automatically. DUMB-FAITHFUL: one contract read, no
 * scoring. A surface the GPU has rendered into (gpu_drawn) is authoritative
 * until the guest announces a new backing write (0x3b → backing_dirty). An
 * all-zero read (unwritten from our view) never overwrites existing content. */
void lagfx_texture_refresh(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                           struct lagfx_vk_state *vk, uint32_t tref) {
    if (!p || !task || !vk || tref == 0u) return;
    lagfx_resource_entry_t *te = lagfx_resource_lookup_texture(&p->resources, tref);
    if (!te || !te->host_handle) return;
    lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)te->host_handle;
    if (ios->image == VK_NULL_HANDLE) return;
    /* Live texdump trigger (imagery-vs-noise judging is only possible from an
     * actual image — nonzero-count heuristics can't tell a dim desktop from
     * noise). `echo 0x52 > /tmp/lagfx_texdump.txt` inside the container dumps
     * that realized ref's CURRENT backing bytes to
     * /tmp/lagfx_texdump_ref0x<ref>.bin on its next sample-bind; the trigger
     * file is consumed. Offline: reshape by the TEXREAL-logged stride/format. */
    {
        FILE *tf = fopen("/tmp/lagfx_texdump.txt", "r");
        if (tf) {
            unsigned want_ref = 0;
            int ok = fscanf(tf, "%i", &want_ref);
            fclose(tf);
            if (ok == 1 && (uint32_t)want_ref == tref) {
                uint64_t dsz = te->size;
                if (dsz >= 4096u && dsz <= 128u * 1024u * 1024u) {
                    uint8_t *db = calloc(1u, (size_t)dsz);
                    if (db) {
                        const char *dhow = "none";
                        uint32_t dgot = lagfx_read_vtx_source(p, task, tref, 0u,
                                                              (uint32_t)dsz, db,
                                                              &dhow, 0);
                        if (dgot) {
                            char path[96];
                            snprintf(path, sizeof(path),
                                     "/tmp/lagfx_texdump_ref0x%x.bin", tref);
                            FILE *df = fopen(path, "wb");
                            if (df) {
                                fwrite(db, 1u, dgot, df);
                                fclose(df);
                                remove("/tmp/lagfx_texdump.txt");
                                LAGFX_LOG("TEXDUMP: ref=0x%x wrote %u/%llu B → %s "
                                          "(read=%s img=%ux%u fmt=%d)",
                                          tref, dgot, (unsigned long long)dsz, path,
                                          dhow, ios->width, ios->height,
                                          (int)ios->format);
                            }
                        } else {
                            LAGFX_WARN("TEXDUMP: ref=0x%x read failed", tref);
                        }
                        free(db);
                    }
                }
            }
        }
    }
    if (ios->gpu_drawn && !te->backing_dirty) return;
    if ((te->realize_seen++ & 7u) != 0u) return;      /* rate limit: 1 in 8 */
    uint64_t total = te->size;
    if (total < 4096u || total > 8u * 1024u * 1024u) {
        total = (uint64_t)ios->width * ios->height * 4u;
        if (total < 4096u || total > 8u * 1024u * 1024u) return;
    }
    uint32_t want = (uint32_t)total;
    uint8_t *data = calloc(1u, want);
    if (!data) return;
    const char *how = "none";
    uint32_t got = lagfx_read_vtx_source(p, task, tref, 0u, want, data, &how, 0);
    if (!got) { free(data); return; }
    uint32_t nz = 0;
    for (uint32_t o = 0; o + 4u <= want; o += 4u)
        if (data[o] | data[o+1] | data[o+2] | data[o+3]) { nz = 1u; break; }
    if (nz && lagfx_vk_iosurface_upload_pixels(vk, ios, data, want) == LAGFX_OK) {
        LAGFX_LOG("TEXFRESH: ref=0x%x re-read %uB (read=%s)", tref, want, how);
        te->backing_dirty = 0u;
        ios->gpu_drawn = 0u;
    }
    free(data);
}

#endif /* LAGFX_HAVE_VULKAN */
