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


#endif /* LAGFX_HAVE_VULKAN */
