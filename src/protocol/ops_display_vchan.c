/*
 * libapplegfx-vulkan — display virtual-channel opcode handler implementations
 * src/protocol/ops_display_vchan.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Last updated: 2026-05-01
 *
 * Handlers for display vchan opcodes 0x01, 0x06, 0x07.  Dispatched by
 * the per-channel doorbell loop in protocol.c (ch >= 5).
 *
 * Online event fires immediately — no delay/delayed ACK.
 */

#include "ops_display_vchan.h"
#include "state.h"
#include "resource_registry.h"
#include "../device.h"
#include "../display.h"
#include "../common/log.h"

#ifdef LAGFX_HAVE_VULKAN
#include "../vulkan/iosurface.h"
#include "../vulkan/display_blit.h"
#endif

#include <stdint.h>
#include <string.h>

static inline uint32_t vchan_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t vchan_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

/* ================================================================
 * 0x02 display submit
 *
 * 8-byte payload: { u32 display_index, u32 arg2 }
 * Sent by the guest after setupSharedState to trigger a display
 * mode update or frame submission. For now we log and acknowledge.
 * ================================================================ */

lagfx_handler_status_t lagfx_op_vchan_display_submit(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    uint32_t display_index = 0u;
    uint32_t arg2 = 0u;
    if (hdr->payload && hdr->payload_size >= 8u) {
        display_index = vchan_le32(hdr->payload + 0);
        arg2 = vchan_le32(hdr->payload + 4);
    }
    LAGFX_LOG("vchan_display_submit: display_index=%u arg2=0x%08x "
               "stamp=0x%08x",
               display_index, arg2, hdr->stamp);

    /* Try to read a few bytes from the framebuffer to check if
     * the guest has rendered any content. The framebuffer base
     * is at (arg2 << 12) and the ss page itself is the first
     * 4 KiB. Actual pixel data starts at ss_gpa + 0x400 (after
     * the BMD header). For a quick probe, read 16 bytes from
     * offset 0x400 and log if non-zero. */
    if (arg2 != 0u && p->dev && p->dev->desc.shell.read_memory
        && display_index < 8u) {
        static unsigned probe_count;
        probe_count++;
        if (probe_count <= 4 || probe_count % 100 == 0) {
            uint64_t fb_base = (uint64_t)arg2 << 12;
            uint8_t probe[16] = {0};
            bool ok = p->dev->desc.shell.read_memory(
                p->dev->desc.shell.opaque,
                fb_base + 0x400u, 16, probe);
            uint32_t sum = 0;
            for (int i = 0; i < 16; ++i) sum += probe[i];
            LAGFX_LOG("vchan_display_submit: fb probe @0x%llx+0x400 "
                      "ok=%d sum=%u bytes=%02x%02x%02x%02x...",
                      (unsigned long long)fb_base, ok, sum,
                      probe[0], probe[1], probe[2], probe[3]);
        }
    }

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * 0x01 setupSharedState
 *
 * 8-byte payload: { u32 display_index, u32 ss_pfn }
 *
 * Writes the per-display BMD shared-state page to satisfy the
 * post-wait apvAssert and advertise a synthetic 1920x1080 mode.
 * Sets pending_displays_bitmask and raises the display-online IRQ.
 * ================================================================ */

lagfx_handler_status_t lagfx_op_vchan_setup_shared_state(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8u) {
        LAGFX_WARN("vchan_setup_shared_state: payload missing or too small "
                   "(size=%u, need 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = vchan_le32(hdr->payload + 0);
    uint32_t ss_pfn       = vchan_le32(hdr->payload + 4);

    LAGFX_LOG("vchan_setup_shared_state: display_index=%u ss_pfn=0x%x "
              "stamp=0x%08x",
              display_index, ss_pfn, hdr->stamp);

    if (ss_pfn == 0u) {
        LAGFX_WARN("vchan_setup_shared_state: ss_pfn=0, cannot address "
                   "BMD ss page (display_index=%u)", display_index);
        return LAGFX_HANDLER_ERR_STATE;
    }

    if (!p->dev || !p->dev->desc.shell.write_memory) {
        return LAGFX_HANDLER_OK;
    }

    uint64_t ss_gpa = ((uint64_t)ss_pfn << 12);

    uint16_t port = (uint16_t)display_index;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x12u,
        sizeof(port), &port);
    LAGFX_TRACE("vchan_setup_shared_state: ss[0x12] := %u "
                "(port, ss_gpa=0x%llx)",
                (unsigned)port,
                (unsigned long long)(ss_gpa + 0x12u));

    uint32_t resp_code = 0u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x1cu,
        sizeof(resp_code), &resp_code);
    LAGFX_TRACE("vchan_setup_shared_state: ss[0x1c] := %u "
                "(ss_gpa=0x%llx)",
                resp_code,
                (unsigned long long)(ss_gpa + 0x1cu));

    uint32_t conn_id = 1u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x00u,
        sizeof(conn_id), &conn_id);

    static const char mode_name[] = "1920x1080";
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x04u,
        sizeof(mode_name), mode_name);

    uint16_t width  = 1920u;
    uint16_t height = 1080u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x14u,
        sizeof(width), &width);
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x16u,
        sizeof(height), &height);

    uint16_t cursor_max = 64u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x18u,
        sizeof(cursor_max), &cursor_max);
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x1au,
        sizeof(cursor_max), &cursor_max);

    uint32_t fb_len = 1920u * 1080u * 4u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x20u,
        sizeof(fb_len), &fb_len);

    uint8_t orient = 0u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x50u,
        sizeof(orient), &orient);

    uint16_t hsync_total = 2200u;
    uint16_t vsync_total = 1125u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x4cu,
        sizeof(hsync_total), &hsync_total);
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x4eu,
        sizeof(vsync_total), &vsync_total);

    uint32_t fb_pfn = ss_pfn;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x200u,
        sizeof(fb_pfn), &fb_pfn);

    uint16_t num_formats = 1u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x208u,
        sizeof(num_formats), &num_formats);

    uint16_t fmt_w = 1920u;
    uint16_t fmt_h = 1080u;
    uint32_t fmt_bpp = 4u;
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x210u,
        sizeof(fmt_w), &fmt_w);
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x212u,
        sizeof(fmt_h), &fmt_h);
    p->dev->desc.shell.write_memory(
        p->dev->desc.shell.opaque, ss_gpa + 0x214u,
        sizeof(fmt_bpp), &fmt_bpp);

    LAGFX_LOG("vchan_setup_shared_state: populated ss for online "
              "(display_index=%u, 1920x1080, ss_gpa=0x%llx)",
              display_index, (unsigned long long)ss_gpa);

    /* Save ss_gpa for vblank timer access. */
    if (p->dev && display_index < 16u) {
        p->dev->display_ss_gpa[display_index] = ss_gpa;
        p->dev->display_ss_installed |= (1u << display_index);
    }

    /* Set bit 2 (online) in pending_mask (ss[+0x100]). The guest's
     * signalDisplay CAS loop will claim this once the guest enables
     * the pipe (ss[+0x104] = 0xC), dispatching the online IES
     * which tells WindowServer the display is ready. */
    if (p->dev && p->dev->desc.shell.write_memory) {
        uint32_t pending = 0x4u;
        p->dev->desc.shell.write_memory(
            p->dev->desc.shell.opaque, ss_gpa + 0x100u,
            sizeof(pending), &pending);
        LAGFX_LOG("vchan_setup_shared_state: wrote ss[+0x100]=0x4 "
                  "(pending online bit) display_index=%u",
                  display_index);
    }

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * 0x04 CmdDefineChildFIFO (display vchan variant)
 *
 * Display virtual channels (ch >= 5) send a 44-byte ring buffer
 * descriptor, unlike compute channels which send only a 4-byte
 * fifoID. Up to 5 sub-rings are registered per display vchan.
 *
 * Wire layout (44 bytes total):
 *   +0x00  u64 reserved       (zeros)
 *   +0x08  u64 ring_base_gpa  (PFN-array base for the sub-ring)
 *   +0x10  u64 ring_size      (e.g., 0x4000 = 16 KiB)
 *   +0x18  u32 entry_count    (e.g., 32, 128)
 *   +0x1c  u16 read_stride    (bytes between read-pointer slots)
 *   +0x1e  u16 write_stride   (bytes between write-pointer slots)
 *   +0x20  u16 field1         (e.g., 4)
 *   +0x22  u16 field2         (e.g., 4)
 *   +0x24  u8  remaining[8]
 * ================================================================ */

#define LAGFX_DISPLAY_CHILD_FIFO_PAYLOAD 44u

lagfx_handler_status_t lagfx_op_display_define_child_fifo(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < LAGFX_DISPLAY_CHILD_FIFO_PAYLOAD) {
        LAGFX_WARN("display CmdDefineChildFIFO: payload too small "
                   "(size=%u, need %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_DISPLAY_CHILD_FIFO_PAYLOAD);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint64_t reserved     = vchan_le64(hdr->payload + 0);
    uint64_t ring_base    = vchan_le64(hdr->payload + 8);
    uint64_t ring_size    = vchan_le64(hdr->payload + 16);
    uint32_t entry_count  = vchan_le32(hdr->payload + 24);
    uint16_t read_stride  = (uint16_t)(vchan_le32(hdr->payload + 28) & 0xffffu);
    uint16_t write_stride = (uint16_t)(vchan_le32(hdr->payload + 30) & 0xffffu);
    uint16_t field1       = (uint16_t)(vchan_le32(hdr->payload + 32) & 0xffffu);
    uint16_t field2       = (uint16_t)(vchan_le32(hdr->payload + 34) & 0xffffu);

    LAGFX_LOG("display CmdDefineChildFIFO: ring_base=0x%llx "
              "ring_size=0x%llx entry_count=%u "
              "strides=(%u,%u) fields=(%u,%u) reserved=0x%llx "
              "stamp=0x%08x",
              (unsigned long long)ring_base,
              (unsigned long long)ring_size,
              entry_count,
              (unsigned)read_stride, (unsigned)write_stride,
              (unsigned)field1, (unsigned)field2,
              (unsigned long long)reserved,
              hdr->stamp);

    {
        unsigned dump_n = hdr->payload_size > 64 ? 64 : hdr->payload_size;
        char hex[200];
        for (unsigned i = 0; i < dump_n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", hdr->payload[i]);
        }
        LAGFX_LOG("display CmdDefineChildFIFO: raw payload: [%s]", hex);
    }


    // When we find an empty slot, use it; otherwise deallocate oldest and reuse
    lagfx_display_child_ring_t *slot = NULL;
    
    // First try to find a free slot
    for (unsigned i = 0; i < LAGFX_MAX_DISPLAY_CHILD_RINGS; ++i) {
        if (!p->display_child_rings[i].live) {
            slot = &p->display_child_rings[i];
            break;
        }
    }
    
    // If no free slot, deallocate the oldest one (ring_index 0) and reuse it
    if (!slot) {
        LAGFX_WARN("display CmdDefineChildFIFO: child ring table full, "
                   "deallocating oldest ring to make room");
        
        // Find the first live entry and mark it dead
        for (unsigned i = 0; i < LAGFX_MAX_DISPLAY_CHILD_RINGS; ++i) {
            if (p->display_child_rings[i].live) {
                LAGFX_LOG("display CmdDefineChildFIFO: deallocating ring[%u] "
                          "(ring_base=0x%llx) to make room",
                          i, (unsigned long long)p->display_child_rings[i].ring_base_gpa);
                memset(&p->display_child_rings[i], 0, sizeof(p->display_child_rings[i]));
                slot = &p->display_child_rings[i];
                break;
            }
        }
    }
    
    if (!slot) {
        LAGFX_ERR("display CmdDefineChildFIFO: failed to allocate ring slot "
                  "(all slots busy and deallocation failed)");
        return LAGFX_HANDLER_ERR_STATE;
    }

    // Populate the slot with new ring geometry
    slot->ring_base_gpa = ring_base;
    slot->ring_size     = ring_size;
    slot->entry_count   = entry_count;
    slot->read_stride   = read_stride;
    slot->write_stride  = write_stride;
    slot->ring_index    = 0;
    slot->live          = true;
    return LAGFX_HANDLER_OK;
}
/* ================================================================
 * 0x06 present
 *
 * 12-byte payload: { u32 display_index, u32 surface_id, u32 plane_id }
 * ================================================================ */

#define LAGFX_VCHAN_PRESENT_PAYLOAD 12u

lagfx_handler_status_t lagfx_op_vchan_present(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < LAGFX_VCHAN_PRESENT_PAYLOAD) {
        LAGFX_WARN("vchan_present: payload missing or too small "
                   "(size=%u, need %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_VCHAN_PRESENT_PAYLOAD);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = vchan_le32(hdr->payload + 0);
    uint32_t surface_id    = vchan_le32(hdr->payload + 4);
    uint32_t plane_id      = vchan_le32(hdr->payload + 8);

    lagfx_resource_entry_t *surf =
        lagfx_resource_lookup(&p->resources, surface_id, 0u);

    LAGFX_LOG("vchan_present: display=%u surface=0x%x plane=%u "
              "resolved=%s host_handle=%p gpu_addr=0x%llx "
              "stamp=0x%08x",
              display_index, surface_id, plane_id,
              surf ? "yes" : "no",
              surf ? surf->host_handle : NULL,
              surf ? (unsigned long long)surf->gpu_addr : 0ull,
              hdr->stamp);

#ifdef LAGFX_HAVE_VULKAN
    if (surf && surf->host_handle && p->dev && p->dev->vk
        && p->dev->vk->initialized) {
        lagfx_vk_iosurface_t *ios =
            (lagfx_vk_iosurface_t *)surf->host_handle;
        lagfx_display_t *disp = NULL;
        for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
            if (p->dev->displays[i] != NULL
                && p->dev->displays[i]->port == display_index) {
                disp = p->dev->displays[i];
                break;
            }
        }
        if (!disp) {
            for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
                if (p->dev->displays[i] != NULL) {
                    disp = p->dev->displays[i];
                    break;
                }
            }
        }
        if (disp && disp->rt_ready) {
            uint64_t scanout_gpa = 0;
            uint64_t scanout_len = 0;
            lagfx_display_entry_t *pe =
                lagfx_protocol_find_display(p, display_index);
            if (pe && pe->mapped) {
                scanout_gpa = pe->buffer_va;
                scanout_len = pe->length;
            }
            lagfx_vk_display_present_surface(
                p->dev->vk, &disp->rt,
                ios->image, &ios->layout,
                ios->width, ios->height,
                disp->rt_width, disp->rt_height,
                scanout_gpa, scanout_len,
                p->dev->desc.shell.opaque,
                p->dev->desc.shell.write_memory);
        }
    }
#endif

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * 0x07 present+gamma
 *
 * 36-byte payload:
 *   +0x00 u32 display_index
 *   +0x04 u32 plane_id
 *   +0x08 u32 surface_id
 *   +0x0c u64 gamma_phys
 *   +0x14 u32 gamma_len
 *   +0x18 u32 gamma_param_a
 *   +0x1c u32 gamma_param_b
 *   +0x20 u32 gamma_param_c
 * ================================================================ */

#define LAGFX_VCHAN_PRESENT_GAMMA_PAYLOAD 36u

lagfx_handler_status_t lagfx_op_vchan_present_gamma(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload ||
        hdr->payload_size < LAGFX_VCHAN_PRESENT_GAMMA_PAYLOAD) {
        LAGFX_WARN("vchan_present_gamma: payload missing or too small "
                   "(size=%u, need %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_VCHAN_PRESENT_GAMMA_PAYLOAD);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index  = vchan_le32(hdr->payload + 0);
    uint32_t plane_id       = vchan_le32(hdr->payload + 4);
    uint32_t surface_id     = vchan_le32(hdr->payload + 8);
    uint64_t gamma_phys     = vchan_le64(hdr->payload + 12);
    uint32_t gamma_len      = vchan_le32(hdr->payload + 20);
    uint32_t gamma_param_a  = vchan_le32(hdr->payload + 24);
    uint32_t gamma_param_b  = vchan_le32(hdr->payload + 28);
    uint32_t gamma_param_c  = vchan_le32(hdr->payload + 32);

    lagfx_resource_entry_t *surf =
        lagfx_resource_lookup(&p->resources, surface_id, 0u);

    LAGFX_LOG("vchan_present_gamma: display=%u plane=%u surface=0x%x "
              "gamma_phys=0x%llx gamma_len=%u "
              "param_a=0x%x param_b=0x%x param_c=0x%x "
              "resolved=%s host_handle=%p gpu_addr=0x%llx "
              "stamp=0x%08x",
              display_index, plane_id, surface_id,
              (unsigned long long)gamma_phys, gamma_len,
              gamma_param_a, gamma_param_b, gamma_param_c,
              surf ? "yes" : "no",
              surf ? surf->host_handle : NULL,
              surf ? (unsigned long long)surf->gpu_addr : 0ull,
              hdr->stamp);

#ifdef LAGFX_HAVE_VULKAN
    if (surf && surf->host_handle && p->dev && p->dev->vk
        && p->dev->vk->initialized) {
        lagfx_vk_iosurface_t *ios =
            (lagfx_vk_iosurface_t *)surf->host_handle;
        lagfx_display_t *disp = NULL;
        for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
            if (p->dev->displays[i] != NULL
                && p->dev->displays[i]->port == display_index) {
                disp = p->dev->displays[i];
                break;
            }
        }
        if (!disp) {
            for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
                if (p->dev->displays[i] != NULL) {
                    disp = p->dev->displays[i];
                    break;
                }
            }
        }
        if (disp && disp->rt_ready) {
            uint64_t scanout_gpa = 0;
            uint64_t scanout_len = 0;
            lagfx_display_entry_t *pe =
                lagfx_protocol_find_display(p, display_index);
            if (pe && pe->mapped) {
                scanout_gpa = pe->buffer_va;
                scanout_len = pe->length;
            }
            lagfx_vk_display_present_surface(
                p->dev->vk, &disp->rt,
                ios->image, &ios->layout,
                ios->width, ios->height,
                disp->rt_width, disp->rt_height,
                scanout_gpa, scanout_len,
                p->dev->desc.shell.opaque,
                p->dev->desc.shell.write_memory);
        }
    }
#endif

    return LAGFX_HANDLER_OK;
}
