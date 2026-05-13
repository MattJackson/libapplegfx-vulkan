/*
 * libapplegfx-vulkan — Display channel dispatcher (ch 5+)
 * src/dispatchers/channel_display_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Display vchan drain. Each display channel (chan_id 5+) has its own
 * command ring registered via FIFORingDescriptor at:
 *
 *   shared_control + 0x400 + 20 * (chan_id - 1)
 *
 * where shared_control is the page registered at BAR0+0x101c
 * (ring_shared_page_pfn). See PROTOCOL.md §3, §5 and
 * paravirt-re/library/state-machines/FIFORingDescriptor.md (the
 * authoritative reference verified 2026-04-25 against a live xp dump).
 *
 * Wire format and read semantics on each ring mirror the root-channel
 * drainer (channel_0_dispatcher.c); the only differences are (a) the
 * ring geometry comes from the per-channel descriptor not from
 * primary-ring MMIO setters, and (b) the descriptor's read_ptr is
 * authoritative (host advances it after draining). Opcodes are the
 * compact vchan namespace (0x01-0x07) plus the §3 display opcodes
 * (0x10-0x14) that fire on display channels.
 */

#include "channel_display_dispatcher.h"
#include "../doorbell.h"
#include "../device.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"

#include <stddef.h>

/* Little-endian readers (guest protocol is LE). */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

#define LAGFX_DISP_DRAIN_MAX_CMDS 128u
#define LAGFX_DISP_MAX_CMD_BYTES  4096u

/* FIFORingDescriptor wire layout — 20 bytes (RE:
 * paravirt-re/library/state-machines/FIFORingDescriptor.md):
 *
 *   +0x00 u32 write_ptr   (guest-incremented)
 *   +0x04 u32 read_ptr    (host-incremented)
 *   +0x08 u32 reserved
 *   +0x0c u32 chan_id     (1-based; matches BAR0+0x1020 doorbell)
 *   +0x10 u32 ring_pfn    (guest-physical PFN of the ring)
 */
#define LAGFX_FIFO_DESC_BYTES        20u
#define LAGFX_FIFO_DESC_BASE_OFFSET  0x400u

/* DisplayPipe ring size convention per FIFORingDescriptor.md §"ring size":
 * not stored in the descriptor; known by context. DisplayPipes use
 * 0x1000 (4 KiB); VirtualChannels use 0x10000 (64 KiB). */
#define LAGFX_DISPLAY_RING_SIZE  0x1000u

/* Dispatch a single command to appropriate handler. */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("display dispatch: opcode=0x%04x stamp=0x%08x", hdr->opcode, hdr->stamp);

    switch (hdr->opcode) {
        /* VChan display opcodes (compact namespace for ch 5+) */
        case 0x01u:  /* RE: PROTOCOL.md §3 — setupSharedState (vchan compact) */
            lagfx_display_vchan_setup_shared_state(p, hdr);
            break;
        case 0x02u:  /* RE: PROTOCOL.md §3 — displaySubmit (vchan compact) */
            lagfx_display_vchan_display_submit(p, hdr);
            break;
        case 0x04u:  /* RE: PROTOCOL.md §3 — CmdDefineChildFIFO (vchan compact) */
            lagfx_display_define_child_fifo(p, hdr);
            break;
        case 0x06u:  /* RE: PROTOCOL.md §3 — present (vchan compact) */
            lagfx_display_vchan_present(p, hdr);
            break;
        case 0x07u:  /* RE: PROTOCOL.md §3 — present+gamma (vchan compact) */
            lagfx_display_vchan_present_gamma(p, hdr);
            break;

        /* §3 display opcodes (also fire on display channels) */
        case LAGFX_OP_DISPLAY_ACK:           /* 0x10 */
            lagfx_display_ack(p, hdr);
            break;
        case LAGFX_OP_DISPLAY_SWAP_MAPPING:  /* 0x12 */
            lagfx_display_swap_mapping(p, hdr);
            break;
        case LAGFX_OP_DISPLAY_CURSOR_SHOW:   /* 0x13 */
            lagfx_display_cursor_show(p, hdr);
            break;
        case LAGFX_OP_DISPLAY_CURSOR_GLYPH:  /* 0x14 */
            lagfx_display_cursor_glyph(p, hdr);
            break;

        default:
            LAGFX_WARN("display dispatch: ch=%u unknown opcode 0x%04x stamp=0x%08x",
                       (unsigned)p->current_chan_id, hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
    }
}

/* Read the 20-byte FIFORingDescriptor for chan_id from the shared
 * control page. Returns true on success. On true, *out_write_ptr,
 * *out_read_ptr, *out_ring_gpa are populated and *out_desc_gpa is the
 * descriptor's GPA (so the caller can publish the bumped read_ptr).
 */
static bool fifo_descriptor_read(lagfx_protocol_t *p,
                                  uint32_t chan_id,
                                  uint64_t *out_desc_gpa,
                                  uint32_t *out_write_ptr,
                                  uint32_t *out_read_ptr,
                                  uint64_t *out_ring_gpa) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("fifo_descriptor_read: no shell.read_memory callback");
        return false;
    }
    if (p->ring_shared_page_pfn == 0u) {
        LAGFX_TRACE("fifo_descriptor_read: ring_shared_page_pfn=0 — kext "
                    "hasn't published shared page yet");
        return false;
    }
    if (chan_id == 0u) {
        return false;  /* root channel uses a different path */
    }

    uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
    uint64_t desc_gpa   = shared_gpa
                        + LAGFX_FIFO_DESC_BASE_OFFSET
                        + (uint64_t)(chan_id - 1u) * LAGFX_FIFO_DESC_BYTES;

    uint8_t desc[LAGFX_FIFO_DESC_BYTES];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      desc_gpa,
                                      LAGFX_FIFO_DESC_BYTES,
                                      desc)) {
        LAGFX_WARN("fifo_descriptor_read: read failed at 0x%llx (chan=%u)",
                   (unsigned long long)desc_gpa, chan_id);
        return false;
    }

    uint32_t write_ptr = read_le32(desc + 0x00);
    uint32_t read_ptr  = read_le32(desc + 0x04);
    uint32_t desc_chan = read_le32(desc + 0x0c);
    uint32_t ring_pfn  = read_le32(desc + 0x10);

    if (ring_pfn == 0u) {
        LAGFX_TRACE("fifo_descriptor_read: ch=%u ring_pfn=0 — descriptor "
                    "not yet initialised", chan_id);
        return false;
    }
    if (desc_chan != chan_id) {
        LAGFX_WARN("fifo_descriptor_read: ch=%u descriptor chan_id field "
                   "mismatch (got %u) — proceeding with caller's id",
                   chan_id, desc_chan);
    }

    *out_desc_gpa   = desc_gpa;
    *out_write_ptr  = write_ptr;
    *out_read_ptr   = read_ptr;
    *out_ring_gpa   = (uint64_t)ring_pfn << 12;
    return true;
}

/* Publish the host's drained position back to the descriptor's read_ptr
 * field (+0x04) so the kext's watchdog can observe progress. */
static void fifo_descriptor_publish_rp(lagfx_protocol_t *p,
                                        uint64_t desc_gpa,
                                        uint32_t read_ptr) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.write_memory) {
        return;
    }
    /* Write to +0x04. NOT +0x00 — the bug pattern called out in
     * FIFORingDescriptor.md §"Bug pattern: clobbering write_ptr". */
    if (!dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                       desc_gpa + 0x04u,
                                       sizeof(read_ptr),
                                       &read_ptr)) {
        LAGFX_WARN("fifo_descriptor_publish_rp: write failed at 0x%llx",
                   (unsigned long long)(desc_gpa + 0x04u));
    }
}

/* Drain ring buffer for display channels. Returns number of commands processed. */
size_t channel_display_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* Look up the per-channel ring geometry from the FIFORingDescriptor.
     * If absent / not yet initialised, drop the doorbell on the floor
     * (kext will retry; we don't have geometry to act on yet). */
    uint64_t desc_gpa = 0;
    uint32_t write_ptr = 0;
    uint32_t read_ptr = 0;
    uint64_t ring_gpa = 0;
    if (!fifo_descriptor_read(p, chan_id, &desc_gpa, &write_ptr, &read_ptr, &ring_gpa)) {
        return 0;
    }

    uint32_t ring_size = LAGFX_DISPLAY_RING_SIZE;

    if (write_ptr == read_ptr) {
        LAGFX_TRACE("display drain: ch=%u caught up rp=0x%x wp=0x%x",
                    chan_id, read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("display drain: ch=%u desc_gpa=0x%llx ring_gpa=0x%llx "
              "ring_size=0x%x rp=0x%x wp=0x%x",
              chan_id,
              (unsigned long long)desc_gpa,
              (unsigned long long)ring_gpa,
              ring_size, read_ptr, write_ptr);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("display drain: no shell.read_memory callback");
        return 0;
    }

    uint32_t rp = read_ptr;
    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_DISP_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_ptr) break;

        uint32_t rp_mod = rp % ring_size;
        uint64_t hdr_gpa = ring_gpa + (uint64_t)rp_mod;

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("display drain: ch=%u header DMA failed at gpa=0x%llx",
                       chan_id, (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = read_le16(hdr_buf + 0);
        uint16_t arg_count_8b = read_le16(hdr_buf + 2);
        uint32_t length       = read_le32(hdr_buf + 4);
        uint32_t stamp        = read_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_DISP_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("display drain: ch=%u bad length 0x%x at rp=0x%x "
                       "opcode=0x%04x — stop",
                       chan_id, length, rp_mod, opcode);
            break;
        }

        uint8_t cmd_buf[LAGFX_DISP_MAX_CMD_BYTES];
        uint32_t head_len = ring_size - rp_mod;
        if (head_len >= length) {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, length, cmd_buf)) {
                LAGFX_WARN("display drain: ch=%u body DMA failed", chan_id);
                break;
            }
        } else {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, head_len, cmd_buf)) break;
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              ring_gpa,
                                              length - head_len,
                                              cmd_buf + head_len)) break;
        }

        lagfx_cmd_header_t hdr;
        hdr.opcode       = opcode;
        hdr.arg_count_8b = arg_count_8b;
        hdr.length       = length;
        hdr.stamp        = stamp;
        hdr.payload_size = (uint16_t)((length > LAGFX_CMD_HEADER_BYTES)
                                       ? (length - LAGFX_CMD_HEADER_BYTES)
                                       : 0u);
        hdr.payload      = (hdr.payload_size > 0)
                              ? (cmd_buf + LAGFX_CMD_HEADER_BYTES)
                              : NULL;

        dispatch_command(p, &hdr);

        /* Display slots are chan_id (5+); see SLOT_DISPLAY_PIPE_0. */
        lagfx_protocol_complete_stamp_slot(p, chan_id, stamp);

        p->total_cmds_seen++;
        cmds++;
        rp = (rp_mod + length) % ring_size;
    }

    /* Publish the drained position back to the descriptor. The kext's
     * watchdog (per channel-progress-flag-RE.md) reads this to confirm
     * the host is making forward progress on the ring. */
    fifo_descriptor_publish_rp(p, desc_gpa, rp);

    LAGFX_LOG("display drain: ch=%u drained=%zu new rp=0x%x",
              chan_id, cmds, rp);
    return cmds;
}
