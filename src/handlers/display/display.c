/*
 * libapplegfx-vulkan — Display handlers (stub implementations)
 * src/handlers/display/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "../handlers.h"
#include "../../display.h"  /* LAGFX_DISPLAY_DEFAULT_W / _H / _BPP */
#include "common/le.h"
#include "common/log.h"
#ifdef LAGFX_HAVE_VULKAN
#include "../../device.h"             /* dev->vk for the C1 frame-blit drain */
#include "../../vulkan/display_blit.h"
#include "../../vulkan/iosurface.h"
#endif

#include <string.h>

lagfx_handler_status_t lagfx_display_ack(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayAck payload (8 bytes):
     *   +0  u32 display_id
     *   +4  u32 frame_id (present sequence number) */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdDisplayAck: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    uint32_t frame_id   = lagfx_le32(hdr->payload + 4);

    LAGFX_LOG("CmdDisplayAck: display_id=%u frame_id=%u stamp=0x%08x",
              display_id, frame_id, hdr->stamp);

    /* TODO: Match frame_id to pending transactions; advance vblank counter;
     *       notify guest via stamp completion that present is complete. */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayCursorGlyph payload (32 bytes):
     *   +0  u32 display_id
     *   +4  u32 width
     *   +8  u32 height
     *   +12 u32 bytes_per_row
     *   +16 u32 hot_x
     *   +20 u32 hot_y
     *   +24 u64 glyph_va (guest VA of ARGB8888 pixel data) */
    if (!hdr->payload || hdr->payload_size < 32) {
        LAGFX_WARN("CmdDisplayCursorGlyph: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id   = lagfx_le32(hdr->payload + 0);
    uint32_t width        = lagfx_le32(hdr->payload + 4);
    uint32_t height       = lagfx_le32(hdr->payload + 8);
    uint32_t bytes_per_row = lagfx_le32(hdr->payload + 12);
    uint32_t hot_x        = lagfx_le32(hdr->payload + 16);
    uint32_t hot_y        = lagfx_le32(hdr->payload + 20);
    uint64_t glyph_va     = lagfx_le64(hdr->payload + 24);

    LAGFX_LOG("CmdDisplayCursorGlyph: display_id=%u %ux%d bpr=%u hot=(%u,%u) va=0x%llx",
              display_id, width, height, bytes_per_row, hot_x, hot_y, (unsigned long long)glyph_va);
    LAGFX_LOG("[cursor] glyph_upload: display_id=%u size=%ux%d hotspot=(%u,%u) bytes_per_row=%u VA=0x%llx",
              display_id, width, height, hot_x, hot_y, bytes_per_row, (unsigned long long)glyph_va);

    /* Stamp the device-shared cursor_glyph state so
     * lagfx_display_submit_clear_color picks up the new metadata.
     * Captured pixel payload is Stage 25 work — read via
     * shell.read_memory once we have the per-task radix tree for the
     * glyph_va. For now, dimensions + hot-spot only. */
    if (p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        dev->cursor_glyph.valid         = true;
        dev->cursor_glyph.display_id    = display_id;
        dev->cursor_glyph.glyph_va      = glyph_va;
        dev->cursor_glyph.width         = width;
        dev->cursor_glyph.height        = height;
        dev->cursor_glyph.bytes_per_row = bytes_per_row;
        dev->cursor_glyph.hot_x         = hot_x;
        dev->cursor_glyph.hot_y         = hot_y;
        dev->cursor_glyph.captured_len  = 0;  /* TODO Stage 25: shell.read_memory(glyph_va) */
    }

    if (p->dev && display_id < LAGFX_MAX_DISPLAYS) {
        lagfx_device_t *dev   = (lagfx_device_t *)p->dev;
        lagfx_display_t *display = dev->displays[display_id];
        if (display && display->desc.callbacks.cursor_glyph) {
            display->desc.callbacks.cursor_glyph(
                display->desc.callbacks.opaque,
                NULL,
                width, height,
                (lagfx_coord_t){ hot_x, hot_y });
        }
    }

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayCursorShow payload (16 bytes):
     *   +0  u32 display_id
     *   +4  int16_t x (signed screen coords)
     *   +6  int16_t y
     *   +8  u32 visible (nonzero = show cursor)
     *   +12 u16 hot_x
     *   +14 u16 hot_y */
    if (!hdr->payload || hdr->payload_size < 16) {
        LAGFX_WARN("CmdDisplayCursorShow: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    int16_t x           = (int16_t)lagfx_le16(hdr->payload + 4);
    int16_t y           = (int16_t)lagfx_le16(hdr->payload + 6);
    uint32_t visible    = lagfx_le32(hdr->payload + 8);
    uint16_t hot_x      = lagfx_le16(hdr->payload + 12);
    uint16_t hot_y      = lagfx_le16(hdr->payload + 14);

    LAGFX_LOG("CmdDisplayCursorShow: display_id=%u pos=(%d,%d) visible=%u hot=(%u,%u)",
              display_id, x, y, visible, hot_x, hot_y);
    LAGFX_LOG("[cursor] cursor_move: display_id=%u pos=(%d,%d) visible=%u hotspot=(%u,%u)",
              display_id, x, y, (int)visible, hot_x, hot_y);

    /* Stamp the device-shared cursor_show state. macOS publishes one
     * cursor across all attached displays — see device.h "Cursor
     * state" doc-block. The per-display clear-color submit reads
     * from here via lagfx_device_last_cursor_show(). */
    if (p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        dev->cursor_show.valid      = true;
        dev->cursor_show.display_id = display_id;
        dev->cursor_show.x          = x;
        dev->cursor_show.y          = y;
        dev->cursor_show.visible    = visible;
        dev->cursor_show.hot_x      = hot_x;
        dev->cursor_show.hot_y      = hot_y;
    }

    if (p->dev && display_id < LAGFX_MAX_DISPLAYS) {
        lagfx_device_t *dev   = (lagfx_device_t *)p->dev;
        lagfx_display_t *display = dev->displays[display_id];
        if (display) {
            if (display->desc.callbacks.cursor_moved) {
                display->desc.callbacks.cursor_moved(display->desc.callbacks.opaque);
            }
            if (display->desc.callbacks.cursor_show) {
                display->desc.callbacks.cursor_show(
                    display->desc.callbacks.opaque,
                    visible != 0);
            }
        }
    }

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * VChan Display Opcodes (0x01, 0x02, 0x04, 0x06, 0x07)
 * Sent on display channels ch 5+ using compact opcode namespace
 * ================================================================ */

lagfx_handler_status_t lagfx_display_vchan_setup_shared_state(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("vchan_setup_shared_state: payload too small (%u, need 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t ss_pfn        = lagfx_le32(hdr->payload + 4);

    LAGFX_LOG("vchan_setup_shared_state: display[%u] ss_pfn=0x%x stamp=0x%08x publishing %ux%u BGRA",
              display_index, ss_pfn, hdr->stamp, LAGFX_DISPLAY_DEFAULT_W, LAGFX_DISPLAY_DEFAULT_H);

    if (ss_pfn == 0u) {
        LAGFX_WARN("vchan_setup_shared_state: ss_pfn=0 for display[%u]", display_index);
        return LAGFX_HANDLER_ERR_STATE;
    }

    /* Store shared state GPA and mark as installed */
    uint64_t ss_gpa = ((uint64_t)ss_pfn << 12);
    if (display_index < 16 && p->dev != NULL) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        dev->display_ss_gpa[display_index] = ss_gpa;
        dev->display_ss_installed |= (1u << display_index);
        LAGFX_LOG("vchan_setup_shared_state: installed display[%u] at gpa=0x%llx",
                  display_index, (unsigned long long)ss_gpa);
    }

    /* Write shared state page fields */
    if (!p->dev || !((lagfx_device_t *)p->dev)->desc.shell.write_memory) {
        return LAGFX_HANDLER_OK;
    }
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;

    /* === Shared-state page byte map ================================
     *
     * Build the full prefix of the shared-state page in one local
     * buffer, then ship it via a single shell.write_memory call. The
     * pre-fix code issued 13 sequential write_memory calls — each is
     * a guest-RAM DMA through the QEMU MemoryRegion path, and on
     * Stage 25/30 this handler will fire on every display
     * mode-change / hot-plug. Single DMA cuts the per-event cost
     * to ~1 / 13th.
     *
     * Field offsets (RE: paravirt-re/annotated/AppleParavirtDisplayPipe-
     * process_online.annotated.asm + neighbour kext disasm). LE byte
     * order throughout — the guest is x86_64.
     *
     *   +0x000  u32 conn_id                = 1
     *   +0x004  char[10] mode_name        = "1920x1080\0"
     *   +0x012  u16 port                   = display_index
     *   +0x014  u16 width                  = 1920
     *   +0x016  u16 height                 = 1080
     *   +0x018  u16 cursor_max_w           = 64
     *   +0x01a  u16 cursor_max_h           = 64
     *   +0x01c  u32 resp_code              = 0
     *   +0x020  u32 fb_len                 = 1920 * 1080 * 4
     *   +0x100  u32 pending_online_bit     = 0x4
     *   +0x104  u32 online_event_enabled   = 0xC  (LOAD-BEARING — gate for
     *                                              process_online's online
     *                                              event; must land before
     *                                              the kext re-reads it)
     *   +0x208  u16 numPixelFormats        = 1
     *   +0x210  u8[16] pixel_format_rec    = {1920, 1080, 'BGRA', 0}
     *
     * Tail bytes between published fields stay zero (page was bzero'd
     * by the kext via PGFB_DispatcherInitialise). Total span: 0x220
     * bytes.
     */
    enum { SS_BLOB_BYTES = 0x220u };
    uint8_t blob[SS_BLOB_BYTES] = { 0 };
    static const char mode_name[10] = "1920x1080";  /* +NUL */

    const uint32_t w  = LAGFX_DISPLAY_DEFAULT_W;
    const uint32_t h  = LAGFX_DISPLAY_DEFAULT_H;
    const uint32_t bpp = LAGFX_DISPLAY_DEFAULT_BYTES_PER_PIXEL;

    lagfx_put_le32(blob + 0x000, 1u);                 /* conn_id */
    memcpy(blob + 0x004, mode_name, sizeof(mode_name));
    lagfx_put_le16(blob + 0x012, (uint16_t)display_index); /* port */
    lagfx_put_le16(blob + 0x014, (uint16_t)w);        /* width */
    lagfx_put_le16(blob + 0x016, (uint16_t)h);        /* height */
    lagfx_put_le16(blob + 0x018, 64u);                /* cursor_max_w */
    lagfx_put_le16(blob + 0x01a, 64u);                /* cursor_max_h */
    lagfx_put_le32(blob + 0x01c, 0u);                 /* resp_code */
    lagfx_put_le32(blob + 0x020, w * h * bpp);        /* fb_len */
    lagfx_put_le32(blob + 0x100, 0x4u);               /* pending_online_bit */
    lagfx_put_le32(blob + 0x104, 0xCu);               /* online_event_enabled */
    lagfx_put_le16(blob + 0x208, 1u);                 /* numPixelFormats */

    /* Pixel-format record at +0x210 — {u32 width, u32 height, fourcc
     * 'BGRA', u32 unverified=0}. PGDisplayNub-presentSurface.annotated.asm
     * line 213 confirms ARGB / BGRA are the only accepted formats. */
    lagfx_put_le32(blob + 0x210, w);
    lagfx_put_le32(blob + 0x214, h);
    blob[0x218] = 'B';
    blob[0x219] = 'G';
    blob[0x21a] = 'R';
    blob[0x21b] = 'A';

    if (!dev->desc.shell.write_memory(dev->desc.shell.opaque, ss_gpa,
                                       SS_BLOB_BYTES, blob)) {
        LAGFX_WARN("vchan_setup_shared_state: shared-state batch DMA failed "
                   "for display[%u] at gpa=0x%llx",
                   display_index, (unsigned long long)ss_gpa);
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    LAGFX_LOG("vchan_setup_shared_state: display[%u] wrote shared-state "
              "(w=%u h=%u fb_len=%u fmt='BGRA' num_formats=1 ss[+0x104]=0xC)",
              display_index, w, h, w * h * bpp);

    LAGFX_LOG("vchan_setup_shared_state: display[%u] ONLINE EVENT - "
              "ss[+0x104] set to 0xC (enable() completed) for multi-display routing",
              display_index);

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_display_submit(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    uint32_t display_index = 0u;
    uint32_t arg2          = 0u;
    if (hdr->payload && hdr->payload_size >= 8u) {
        display_index = lagfx_le32(hdr->payload + 0);
        arg2          = lagfx_le32(hdr->payload + 4);
    }

    LAGFX_LOG("vchan_display_submit: display[%u] arg2=0x%08x stamp=0x%08x",
              display_index, arg2, hdr->stamp);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;

    /* Probe framebuffer to verify guest rendered content. Kept as the
     * first-N + every-100th sample so the log stays readable while still
     * giving us a periodic sighting of what the kext is asking us to
     * scan out. */
    if (arg2 != 0u && dev && dev->desc.shell.read_memory && display_index < 8u) {
        static unsigned probe_count;
        probe_count++;
        if (probe_count <= 4 || probe_count % 100 == 0) {
            uint64_t fb_base = (uint64_t)arg2 << 12;
            uint8_t probe[16] = {0};
            bool ok = dev->desc.shell.read_memory(
                dev->desc.shell.opaque, fb_base + 0x400u, 16, probe);
            uint32_t sum = 0;
            for (int i = 0; i < 16; ++i) {
                sum += probe[i];
            }
            LAGFX_LOG("vchan_display_submit: fb probe @0x%llx+0x400 ok=%d sum=%u bytes=%02x%02x...",
                      (unsigned long long)fb_base, ok, sum, probe[0], probe[1]);
        }
    }

    /* Stage 25/30 scanout wiring. The kext just told us "frame ready at
     * fb_base" for display[display_index]; pass that through to the
     * Vulkan/scanout path so the host actually presents pixels.
     *
     * Pre-refactor parity note: src/protocol/ops_display.c (deleted in
     * b652199) only ever called lagfx_display_submit_clear_color from a
     * legacy fixture path (CmdDisplayTransaction3 gated on
     * last_load_action==2u). The real production scanout was never
     * wired — even pre-refactor. This is that wiring, written from
     * scratch around the actual production trigger (vchan_display_submit
     * = kext's per-frame "scan this out" signal).
     *
     * Two code paths inside lagfx_display_submit_rendered_frame:
     *   - scanout_gpa != 0: DMA writeback to the kext-supplied buffer
     *     (the steady-state path; what we now exercise)
     *   - scanout_gpa == 0: fallback readback into display->fallback_pixels
     *     so the shell can pull via read_frame (used when CmdDisplaySwapMapping
     *     hasn't fired yet)
  * Both end with set_frame_ready() so QEMU's display-tick callback
      * notices something to push to noVNC.
      */
    if (dev && display_index < LAGFX_MAX_DISPLAYS) {
        lagfx_display_t *disp = dev->displays[display_index];
        if (disp != NULL) {
            uint64_t fb_base, scanout_len;
            
            /* If kext sends arg2=0, use the last-seen CmdDisplaySwapMapping
              * values. This is the common pattern when scanout buffer is stable. */
            if (arg2 == 0 && disp->scanout_valid) {
                fb_base     = disp->scanout_gpa;
                scanout_len = disp->scanout_length;
                LAGFX_LOG("vchan_display_submit: using stored swap mapping gpa=0x%llx len=%llu for display[%u]",
                          (unsigned long long)fb_base, (unsigned long long)scanout_len,
                          display_index);
            } else if (arg2 == 0 && !disp->scanout_valid) {
                /* No stored swap mapping - kext expects us to use rt content.
                  * Pass 0 gpa to trigger fallback readback path from render target. */
                fb_base     = 0;
                scanout_len = disp->rt_ready ? ((uint64_t)disp->rt_width * (uint64_t)disp->rt_height * 4u) :
                              ((uint64_t)LAGFX_DISPLAY_DEFAULT_W *
                               (uint64_t)LAGFX_DISPLAY_DEFAULT_H *
                               (uint64_t)LAGFX_DISPLAY_DEFAULT_BYTES_PER_PIXEL);
                LAGFX_LOG("vchan_display_submit: arg2=0, no stored mapping - using rt=%ux%u fallback len=%llu",
                          disp->rt_width, disp->rt_height, (unsigned long long)scanout_len);
            } else {
                fb_base     = (uint64_t)arg2 << 12;
                scanout_len = disp->scanout_length > 0 ? disp->scanout_length :
                              ((uint64_t)LAGFX_DISPLAY_DEFAULT_W *
                               (uint64_t)LAGFX_DISPLAY_DEFAULT_H *
                               (uint64_t)LAGFX_DISPLAY_DEFAULT_BYTES_PER_PIXEL);
            }

#ifdef LAGFX_HAVE_VULKAN
            /* M2c C1 (audit B1): drain the frame-blit queue IN DRAW ORDER into
             * display->rt before scanning out — the per-pass surfaces drawn
             * since the last present compose here (earlier passes first, the
             * final composite last), replacing the old last-blit-wins per-draw
             * blit. Failures skip that surface; the present proceeds. */
            if (p->frame_blit_n > 0u && dev->vk && disp->rt_ready
                && disp->rt.image != VK_NULL_HANDLE) {
                for (uint32_t qi = 0; qi < p->frame_blit_n && qi < 16u; qi++) {
                    lagfx_vk_iosurface_t *qios =
                        (lagfx_vk_iosurface_t *)p->frame_blit_queue[qi];
                    if (!qios || qios->image == VK_NULL_HANDLE) continue;
                    lagfx_status_t qst = lagfx_vk_display_present_surface(
                        dev->vk, &disp->rt, qios->image, &qios->layout,
                        qios->width, qios->height,
                        disp->rt.width, disp->rt.height,
                        0u, 0u, /* no per-surface scanout write — the final
                                   submit below does the readback+scanout */
                        dev->desc.shell.opaque, dev->desc.shell.write_memory);
                    if (qst != LAGFX_OK)
                        LAGFX_WARN("vchan_display_submit: ASMBLIT queue[%u] blit failed (%d)",
                                   qi, (int)qst);
                }
                LAGFX_LOG("vchan_display_submit: ASMBLIT drained %u queued surfaces in draw order",
                          p->frame_blit_n);
                p->frame_blit_n = 0u;
            }
#endif
            lagfx_status_t st = lagfx_display_submit_rendered_frame(
                disp, fb_base, scanout_len);
            if (st != LAGFX_OK) {
                LAGFX_WARN("vchan_display_submit: scanout failed for "
                           "display[%u] fb=0x%llx (%d) — frame skipped",
                           display_index, (unsigned long long)fb_base,
                           (int)st);
            }
        } else {
            /* display_rt_create runs lazily after vchan_setup_shared_state;
             * if we get a submit before the display struct exists, just
             * stamp and skip — the next submit will land. */
            LAGFX_LOG("vchan_display_submit: display[%u] not yet "
                      "instantiated — submit skipped",
                      display_index);
        }
    }

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_define_child_fifo(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 44u) {
        LAGFX_WARN("vchan CmdDefineChildFIFO: payload too small (%u, need 44)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint64_t ring_base = lagfx_le64(hdr->payload + 8);
    uint64_t ring_size = lagfx_le64(hdr->payload + 16);
    uint32_t entry_count = lagfx_le32(hdr->payload + 24);

    /* display_index = current_chan_id - 5 (SLOT_DISPLAY_PIPE_0 starts
     * at chan 5). Guard against the underflow that would happen if
     * this handler is ever invoked on a non-display channel — the
     * vchan-compact 0x04 currently routes only from channel 5+, but
     * a future router change could regress that. Log a sentinel
     * instead of u32-wrapping to ~4 billion. */
    int display_index = (int)p->current_chan_id - 5;
    if (display_index < 0) {
        LAGFX_WARN("vchan CmdDefineChildFIFO: invoked on chan=%u (<5) — "
                   "logging as display_index=-1",
                   (unsigned)p->current_chan_id);
        display_index = -1;
    }

    LAGFX_LOG("vchan CmdDefineChildFIFO: display[%d] ring_base=0x%llx size=0x%x entries=%u stamp=0x%08x",
              display_index, (unsigned long long)ring_base,
              (uint32_t)ring_size, entry_count, hdr->stamp);

    /* TODO: Store child FIFO geometry for sub-channel command delivery */
    LAGFX_WARN("CmdDefineChildFIFO: FIFO geometry storage not implemented yet");

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_present(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("vchan_present: payload too small (%u, need 12)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t surface_id    = lagfx_le32(hdr->payload + 4);
    uint32_t plane_id      = lagfx_le32(hdr->payload + 8);

    LAGFX_LOG("vchan_present: display[%u] surface=0x%x plane=%u stamp=0x%08x",
              display_index, surface_id, plane_id, hdr->stamp);

    /* TODO: Call lagfx_vk_display_present_surface() for Vulkan scanout */
    LAGFX_WARN("vchan_present: Vulkan scanout not wired (Stage 80 blocker)");

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_present_gamma(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 36u) {
        LAGFX_WARN("vchan_present_gamma: payload too small (%u, need 36)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t plane_id      = lagfx_le32(hdr->payload + 4);
    uint32_t surface_id    = lagfx_le32(hdr->payload + 8);

    LAGFX_LOG("vchan_present_gamma: display[%u] surface=0x%x gamma_len=%u stamp=0x%08x",
              display_index, surface_id, lagfx_le32(hdr->payload + 20), hdr->stamp);

    /* TODO: Upload gamma table and call lagfx_vk_display_present_surface() */
    LAGFX_WARN("vchan_present: Vulkan scanout not wired (Stage 80 blocker)");

    return LAGFX_HANDLER_OK;
}
