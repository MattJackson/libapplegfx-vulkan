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
    /* M2c READARB (LAGFX_M2_READARB): the placement "PFN" is a VIRTUAL page
     * number for some allocations and physical for others (identity-mapped).
     * Live proof: refs 0x15/0x16/0x18 read boot-log text via raw GPA but real
     * float geometry via the VA-translated radix (VTXSRC mode-1 scores 59-100
     * vs 0), while 0xe/0x14/0x781 read fine raw. The old "raw unless all-zero"
     * arbitration keeps nonzero garbage. Read BOTH and keep the buffer with the
     * higher plausibility-as-float score (ties → raw, preserving behavior). */
    if (LAGFX_POLICY("M2_READARB") && getenv("LAGFX_M2_RAWGPA") && task) {
        lagfx_device_t *adev = p ? (lagfx_device_t *)p->dev : NULL;
        bool raw_ok = adev && adev->desc.shell.read_memory
                      && adev->desc.shell.read_memory(adev->desc.shell.opaque,
                                                      gpa, len, buf);
        uint32_t raw_score = raw_ok ? lagfx_vtx_float_plausibility(buf, len) : 0u;
        if (raw_score < 90u) {
            uint32_t probe_len = len < 4096u ? len : 4096u;
            uint8_t vprobe[4096];
            if (lagfx_task_read_virtual(p, (lagfx_task_entry_t *)task, gpa,
                                        probe_len, vprobe)) {
                uint32_t va_score = lagfx_vtx_float_plausibility(vprobe, probe_len);
                if (va_score > raw_score) {
                    return lagfx_read_virtual_besteffort(p, task, gpa, len, buf);
                }
            }
        }
        if (raw_ok) return true;
        return lagfx_read_virtual_besteffort(p, task, gpa, len, buf);
    }
    /* GATED (LAGFX_M2_RAWGPA): the raw-GPA read is strictly more correct (the
     * descriptor PFN is a guest-physical frame), but it surfaces REAL texture
     * data that the downstream M2 render path can't yet sample correctly — the
     * full-screen wallpaper draw mis-renders to white + a scrambled band (the
     * vertexCount=393216 geometry misparse). With the old VA read the same
     * textures read black, so M1's "recognizable" dark frame stays intact. Keep
     * the fix OPT-IN until the M2 rendering layer (texture dims + full-screen
     * sample geometry) is ready, so M1 production does NOT regress. Default:
     * old virtual best-effort read (M1 frame unchanged). */
    if (!getenv("LAGFX_M2_RAWGPA")) {
        return lagfx_read_virtual_besteffort(p, task, gpa, len, buf);
    }
    lagfx_device_t *dev = p ? (lagfx_device_t *)p->dev : NULL;
    if (dev && dev->desc.shell.read_memory
        && dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, len, buf)) {
        uint32_t nb = 0;
        for (uint32_t q = 0; q + 4u <= len; q += 4u)
            if (buf[q] | buf[q + 1] | buf[q + 2]) { nb = 1; break; }
        if (nb) return true;   /* raw GPA had real content — the correct path */
    }
    /* Raw read empty/unavailable — fall back to VA-translated best-effort. */
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
/* Photo-vs-noise discriminator: 0..1000, higher = more photo-like. A photo has
 * strong neighbour correlation (small adjacent-pixel deltas); random heap read
 * as pixels has ~uniform deltas (mean ~85/channel). Samples the buffer as BGRA
 * at a plausible row width and averages |px[i]-px[i+1]| over each channel; the
 * score is 1000*(1 - meandelta/128) clamped. Rejects the noise regions that a
 * pure nonblack/color-count metric cannot distinguish from a real image. */
static uint32_t photo_correlation_score(const uint8_t *buf, uint32_t bytes) {
    if (!buf || bytes < 4096u) return 0u;
    uint64_t sum = 0; uint32_t n = 0;
    uint32_t px = bytes / 4u;
    uint32_t step = (px / 4096u) + 1u;   /* sample ~4k adjacent pairs */
    for (uint32_t i = 0; i + 1u < px && n < 4096u; i += step) {
        const uint8_t *a = buf + (size_t)i * 4u;
        const uint8_t *b = a + 4u;
        sum += (uint32_t)abs((int)a[0]-(int)b[0])
             + (uint32_t)abs((int)a[1]-(int)b[1])
             + (uint32_t)abs((int)a[2]-(int)b[2]);
        n += 3u;
    }
    if (!n) return 0u;
    uint32_t mean = (uint32_t)(sum / n);         /* 0 (flat) .. ~85 (noise) */
    if (mean >= 128u) return 0u;
    return 1000u - (mean * 1000u) / 128u;
}

lagfx_vk_iosurface_t *lagfx_texture_realize(lagfx_protocol_t *p,
                                            lagfx_task_entry_t *task,
                                            struct lagfx_vk_state *vk,
                                            uint32_t tref) {
    if (!p || !task || !vk || tref == 0u) return NULL;
    uint8_t ttype = 0; uint64_t tva = 0, tgpa = 0;
    if (!lagfx_resolve_object_data(p, task, tref, &ttype, &tva, &tgpa)
        || tva == 0u || ttype != 0x03u)
        return NULL;
    uint8_t desc[64];
    if (!lagfx_task_read_virtual(p, task, tva, sizeof(desc), desc))
        return NULL;
    uint64_t total = 0;
    for (int e = 0; e < 4; e++) {
        uint64_t rsize = lagfx_le64(desc + (size_t)e * 16u);
        uint64_t pfn = lagfx_le64(desc + (size_t)e * 16u + 8u) & 0xffffffffull;
        if (pfn < 0x10u || pfn > 0xfffffu || rsize == 0u) continue;
        total += rsize;
    }
    if (total < 4096u || total > 8u * 1024u * 1024u) return NULL;
    uint32_t want = (uint32_t)total;
    uint8_t *raw = calloc(1u, want), *via = calloc(1u, want);
    if (!raw || !via) { free(raw); free(via); return NULL; }
    const char *how0 = "none", *how1 = "none";
    uint32_t got0 = lagfx_read_vtx_source(p, task, tref, 0u, want, raw, &how0, 0);
    uint32_t got1 = lagfx_read_vtx_source(p, task, tref, 0u, want, via, &how1, 1);
    /* Score by WRITTEN pixels: nonzero alpha counts (opaque black IS real
     * content — the login background buffer is 00 00 00 ff); a fully-zero
     * buffer is unwritten and rejected. */
    uint32_t nb0 = 0, nb1 = 0, na0 = 0, na1 = 0;
    for (uint32_t o = 0; o + 4u <= want; o += 4u) {
        if (raw[o] | raw[o+1] | raw[o+2]) nb0++;
        if (via[o] | via[o+1] | via[o+2]) nb1++;
        if (raw[o+3]) na0++;
        if (via[o+3]) na1++;
    }
    uint32_t sc0 = na0 > nb0 ? na0 : nb0, sc1 = na1 > nb1 ? na1 : nb1;
    uint8_t *pick = NULL; const char *how = "none"; uint32_t nb = 0;
    if (got1 && sc1 >= sc0) { pick = via; how = "va"; nb = nb1; }
    else if (got0)          { pick = raw; how = "raw"; nb = nb0; }
    if (!pick || (pick == via ? sc1 : sc0) == 0u) { free(raw); free(via); return NULL; }
    /* Wallpaper recovery: a large surface (wallpaper-sized) whose placement
     * read is fully black lives elsewhere — the guest CPU-decodes it into one
     * of the 0x39-mapped guest-physical regions. Scan those for the richest
     * (photo-content) region and bind THAT instead. */
    if (nb == 0u && want >= 2u * 1024u * 1024u && p->big_maps_n > 0u) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev && dev->desc.shell.read_memory) {
            /* Pick the 0x39 region that looks most like a PHOTO (high spatial
             * correlation), not merely the most non-black — a nonblack/colour
             * count cannot tell a photo from random heap noise (noise maxes it),
             * which bound noise bands. Require a real photo score AND coverage. */
            uint8_t *best = NULL; uint32_t best_score = 0, best_nb = 0;
            uint8_t *scan = calloc(1u, want);
            for (uint32_t bm = 0; scan && bm < p->big_maps_n && bm < 16u; bm++) {
                if (p->big_maps[bm].size < want) continue;
                if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                                 p->big_maps[bm].gpa, want, scan))
                    continue;
                uint32_t score = photo_correlation_score(scan, want);
                uint32_t cnt = 0;
                for (uint32_t o = 0; o + 4u <= want; o += 256u)
                    if (scan[o] | scan[o+1] | scan[o+2]) cnt++;
                LAGFX_LOG("TEXREAL: 0x39 region[%u] gpa=0x%llx photo=%u nonblack=%u",
                          bm, (unsigned long long)p->big_maps[bm].gpa, score, cnt);
                if (score > best_score && cnt > 0u) {
                    best_score = score; best_nb = cnt;
                    if (!best) best = calloc(1u, want);
                    if (best) memcpy(best, scan, want);
                }
            }
            free(scan);
            /* Bind only a genuinely photo-like region; a noise band is worse
             * than the black placement read. */
            if (best && best_score >= 500u) {
                free(raw); free(via);
                raw = best; via = NULL; pick = best; how = "mapmem"; nb = best_nb;
                LAGFX_LOG("TEXREAL: ref=0x%x wallpaper recovered from 0x39 map (photo=%u nonblack~%u)",
                          tref, best_score, best_nb);
            } else {
                free(best);
            }
        }
    }
    /* dims: first preferred width dividing the pixel count with sane aspect */
    uint32_t npx = want / 4u, W = 0, H = 0;
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
    if (!W) { W = 64u; H = npx / 64u ? npx / 64u : 1u; }
    lagfx_vk_iosurface_t *ios = NULL;
    if (lagfx_vk_iosurface_create(vk, W, H, 80u, &ios) != LAGFX_OK || !ios) {
        free(raw); free(via); return NULL;
    }
    if (lagfx_vk_iosurface_upload_pixels(vk, ios, pick, want) != LAGFX_OK) {
        lagfx_vk_iosurface_destroy(vk, ios);
        free(raw); free(via); return NULL;
    }
    /* Content-richness score (distinct color buckets, stride-sampled) BEFORE the
     * free — pick aliases raw/via. Photo scores high; a clear/fill scores 1-2. */
    uint32_t nbk = 0; {
        uint32_t buckets[64];
        for (uint32_t px = 0; px < npx && nbk < 64u; px += (npx / 512u) + 1u) {
            uint32_t c = lagfx_le32(pick + (size_t)px * 4u) & 0xf8f8f8u;
            bool seen = false;
            for (uint32_t b = 0; b < nbk; b++)
                if (buckets[b] == c) { seen = true; break; }
            if (!seen) buckets[nbk++] = c;
        }
    }
    free(raw); free(via);
    lagfx_resource_entry_t *te = lagfx_resource_lookup_texture(&p->resources, tref);
    if (!te) {
        lagfx_resource_register(&p->resources, tref, LAGFX_RESOURCE_TYPE_TEXTURE,
                                task->id, tgpa, total);
        te = lagfx_resource_lookup_texture(&p->resources, tref);
    }
    if (te) {
        te->host_handle = ios; te->image = ios->image; te->view = ios->view;
        te->realize_rich = (uint16_t)(nbk > 0xffffu ? 0xffffu : nbk);
        te->realize_seen = 0u;
    } else {
        lagfx_vk_iosurface_destroy(vk, ios);
        return NULL;
    }
    LAGFX_LOG("TEXREAL: ref=0x%x %ux%u from %uB backing (read=%s nonblack=%u/%u rich=%u%s)",
              tref, W, H, want, how, nb, npx, nbk, nbk >= 64u ? "+" : "");
    /* A recovered full-screen background (the wallpaper: mapmem-sourced, photo-
     * rich) is loaded AFTER the guest's one-shot composite already sampled the
     * black version, and the guest then idles — so it never re-composites.
     * Enqueue it at the FRONT of the frame-blit queue (background layer) so the
     * next display present carries it to the scanout under the UI layers. */
    if (how && how[0] == 'm' && nbk >= 32u && p->frame_blit_n < 16u) {
        for (uint32_t q = p->frame_blit_n; q > 0u; q--)
            p->frame_blit_queue[q] = p->frame_blit_queue[q - 1u];
        p->frame_blit_queue[0] = (uintptr_t)ios;
        p->frame_blit_n++;
        LAGFX_LOG("TEXREAL: wallpaper 0x%x enqueued as background layer", tref);
    }
    return ios;
}

/* Re-read a realized texture's guest backing and re-upload if the content got
 * RICHER since the cached read. The wallpaper is CPU-decoded by the guest
 * seconds into boot — a realize during the early draw burst caches the still-
 * black buffer forever. Called from the sample-bind path while the cached
 * score is below photo grade; rate-limited; uploads into the SAME VkImage so
 * every later composite picks the new pixels up automatically. */
void lagfx_texture_refresh(lagfx_protocol_t *p, lagfx_task_entry_t *task,
                           struct lagfx_vk_state *vk, uint32_t tref) {
    if (!p || !task || !vk || tref == 0u) return;
    lagfx_resource_entry_t *te = lagfx_resource_lookup_texture(&p->resources, tref);
    if (!te || !te->host_handle) return;
    if (te->realize_rich >= 64u) return;              /* already photo-grade */
    if ((te->realize_seen++ & 7u) != 0u) return;      /* rate limit: 1 in 8 */
    lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)te->host_handle;
    if (ios->image == VK_NULL_HANDLE) return;
    uint64_t total = te->size;
    if (total < 4096u || total > 8u * 1024u * 1024u) {
        total = (uint64_t)ios->width * ios->height * 4u;
        if (total < 4096u || total > 8u * 1024u * 1024u) return;
    }
    uint32_t want = (uint32_t)total;
    uint8_t *raw = calloc(1u, want), *via = calloc(1u, want);
    if (!raw || !via) { free(raw); free(via); return; }
    const char *how0 = "none", *how1 = "none";
    uint32_t got0 = lagfx_read_vtx_source(p, task, tref, 0u, want, raw, &how0, 0);
    uint32_t got1 = lagfx_read_vtx_source(p, task, tref, 0u, want, via, &how1, 1);
    uint32_t nb0 = 0, nb1 = 0;
    for (uint32_t o = 0; o + 4u <= want; o += 4u) {
        if (raw[o] | raw[o+1] | raw[o+2]) nb0++;
        if (via[o] | via[o+1] | via[o+2]) nb1++;
    }
    uint8_t *pick = NULL; const char *how = "none";
    if (got1 && nb1 >= nb0)      { pick = via; how = "va"; }
    else if (got0 && nb0 > 0u)   { pick = raw; how = "raw"; }
    if (!pick) { free(raw); free(via); return; }
    uint32_t npx = want / 4u, nbk = 0;
    {
        uint32_t buckets[64];
        for (uint32_t px = 0; px < npx && nbk < 64u; px += (npx / 512u) + 1u) {
            uint32_t c = lagfx_le32(pick + (size_t)px * 4u) & 0xf8f8f8u;
            bool seen = false;
            for (uint32_t b = 0; b < nbk; b++)
                if (buckets[b] == c) { seen = true; break; }
            if (!seen) buckets[nbk++] = c;
        }
    }
    /* Only accept a richer re-read that is also photo-like (spatial correlation)
     * — a colour-count alone promotes noise. */
    uint32_t pscore = photo_correlation_score(pick, want);
    if (nbk > te->realize_rich && (want < 2u*1024u*1024u || pscore >= 500u)
        && lagfx_vk_iosurface_upload_pixels(vk, ios, pick, want) == LAGFX_OK) {
        LAGFX_LOG("TEXFRESH: ref=0x%x re-read %uB (read=%s) rich %u -> %u photo=%u%s",
                  tref, want, how, te->realize_rich, nbk, pscore,
                  nbk >= 64u ? "+ PHOTO" : "");
        te->realize_rich = (uint16_t)nbk;
    }
    free(raw); free(via);
}

#endif /* LAGFX_HAVE_VULKAN */
