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
#include "ring_common.h"
#include "../doorbell.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"

#include <stddef.h>

#define LAGFX_DISP_DRAIN_MAX_CMDS 128u
#define LAGFX_DISP_MAX_CMD_BYTES  4096u

/* DisplayPipe ring size convention per FIFORingDescriptor.md §"ring size":
 * not stored in the descriptor; known by context. DisplayPipes use
 * 0x1000 (4 KiB); VirtualChannels use 0x10000 (64 KiB). Display rings
 * don't wrap (small enough), so PFN-array idx is always 0 — but we
 * still go through ring_common.lagfx_ring_resolve_data_gpa for the
 * indirection.
 */
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
        case LAGFX_OP_DISPLAY_SET_PROPERTIES: /* 0x11 */
            /* RE: command-buffer-format.md §3.5 — display mode commit
             * (resolution / refresh / colour space). Pre-refactor
             * lagfx_op_display_set_properties at b652199~1:src/protocol/
             * ops_display.c (deleted in commit 4b46de9 because the
             * dispatcher refactor moved the responsibility, but the
             * post-refactor display handler doesn't implement it).
             * Log + ack until macOS observed to fire it. */
            LAGFX_LOG("display: 0x11 CmdDisplaySetProperties ch=%u stamp=0x%08x payload=%u "
                      "(M6 log-ack stub; TODO: Stage 30 mode commit)",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
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
        case LAGFX_OP_DISPLAY_TRANSACTION2_DEP: /* 0x15 */
            /* RE: pre-refactor lagfx_op_display_transaction2_dep (deprecated
             * variant of 0x16; same dispatch). Log + ack. */
            LAGFX_LOG("display: 0x15 CmdDisplayTransaction2 (dep) ch=%u stamp=0x%08x payload=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_TRANSACTION3:  /* 0x16 */
            /* RE: command-buffer-format.md §3.10 — surface attach for
             * present. Pre-refactor lagfx_op_display_transaction3 had
             * a 12 + 32*N (legacy) or 16 + 44*N (layered) shape parser
             * that fed compositor state; the post-refactor present
             * path lives in src/handlers/display/display.c (vchan
             * opcode 0x06). When SkyLight starts driving the §3
             * compositor path this needs reinstatement. */
            LAGFX_LOG("display: 0x16 CmdDisplayTransaction3 ch=%u stamp=0x%08x payload=%u "
                      "(log-ack; TODO: Stage 30 compositor reinstatement)",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_SET_SHARED_PAGE: /* 0x17 */
            /* RE: command-buffer-format.md §3.6 — install the vblank
             * mailbox page. Pre-refactor parsed `u64 page_va` at +0
             * and zeroed the first 64 bytes via shell.write_memory.
             * The vchan-compact opcode 0x01 (setupSharedState) covers
             * the same operation on the live display vchans, so this
             * §3 variant is currently unused. */
            LAGFX_LOG("display: 0x17 CmdDisplaySetSharedPage ch=%u stamp=0x%08x payload=%u "
                      "(log-ack; superseded by vchan 0x01)",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_SLEEP_STATE:   /* 0x18 */
            /* RE: command-buffer-format.md §3.11 — display sleep / wake.
             * Pre-refactor was log-only. */
            LAGFX_LOG("display: 0x18 CmdDisplaySleepState ch=%u stamp=0x%08x payload=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS: /* 0x19 */
            /* RE: §14.10 cosmetic — gamma / blend curve. Pre-refactor
             * captured payload + display_id into p->compositor_params
             * for diagnostics; post-refactor state.h doesn't carry
             * that struct. Log + ack. */
            LAGFX_LOG("display: 0x19 CmdDisplayCompositorParameters ch=%u stamp=0x%08x payload=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_SET_ICC_PROFILE: /* 0x1a */
            /* RE: §14.11 cosmetic — ICC profile install. Pre-refactor
             * captured display_id / profile_va / profile_size into
             * p->icc_profile; not reachable from post-refactor state.h.
             * Log + ack. NOTE: shares the value 0x1a with the Render
             * inner opcode RenderDescribeRenderPass, but the wire
             * paths are disjoint — this is the outer-FIFO §3 opcode. */
            LAGFX_LOG("display: 0x1a CmdDisplaySetICCProfile ch=%u stamp=0x%08x payload=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISPLAY_EXT_1E:        /* 0x1e */
            /* RE: §13.5 display-adjacent extended (kext-only). Pre-
             * refactor was log-only — semantics never confirmed. */
            LAGFX_LOG("display: 0x1e CmdDisplayExt1E ch=%u stamp=0x%08x payload=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;

        default:
            LAGFX_WARN("display dispatch: ch=%u unknown opcode 0x%04x stamp=0x%08x",
                       (unsigned)p->current_chan_id, hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
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
    uint64_t page0_gpa = 0;
    if (!lagfx_ring_fifo_descriptor_read(p, chan_id, &desc_gpa, &write_ptr, &read_ptr, &page0_gpa)) {
        return 0;
    }

    uint32_t ring_size = LAGFX_DISPLAY_RING_SIZE;

    if (write_ptr == read_ptr) {
        LAGFX_TRACE("display drain: ch=%u caught up rp=0x%x wp=0x%x",
                    chan_id, read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("display drain: ch=%u desc_gpa=0x%llx page0_gpa=0x%llx "
              "ring_size=0x%x rp=0x%x wp=0x%x",
              chan_id,
              (unsigned long long)desc_gpa,
              (unsigned long long)page0_gpa,
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

        /* Resolve the command-bytes GPA via page0's PFN-array (see
         * per-channel-ring-pfn-array.md). page0_gpa<<12 is NOT data —
         * it's a u32 PFN array of page pointers. */
        uint64_t hdr_gpa = 0;
        if (!lagfx_ring_resolve_data_gpa(p, page0_gpa, ring_size, rp, &hdr_gpa)) {
            break;
        }

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("display drain: ch=%u header DMA failed at gpa=0x%llx",
                       chan_id, (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = lagfx_le16(hdr_buf + 0);
        uint16_t arg_count_8b = lagfx_le16(hdr_buf + 2);
        uint32_t length       = lagfx_le32(hdr_buf + 4);
        uint32_t stamp        = lagfx_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_DISP_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("display drain: ch=%u bad length 0x%x at rp=0x%x "
                       "opcode=0x%04x first8=%02x%02x%02x%02x%02x%02x%02x%02x — stop",
                       chan_id, length, rp, opcode,
                       hdr_buf[0], hdr_buf[1], hdr_buf[2], hdr_buf[3],
                       hdr_buf[4], hdr_buf[5], hdr_buf[6], hdr_buf[7]);
            break;
        }

        /* Body lands in the protocol's display scratch (state.h's
         * "Per-dispatcher scratch buffers"). BQL-serialised — must
         * not be used from anywhere outside this drain loop. */
        uint8_t *cmd_buf = p->scratch_display;
        /* Display rings don't wrap (ring_size=0x1000 small enough),
         * but read the body in one DMA. */
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa, length, cmd_buf)) {
            LAGFX_WARN("display drain: ch=%u body DMA failed", chan_id);
            break;
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
        rp = rp + length;  /* absolute, monotonic — see per-channel-ring-pfn-array.md */
    }

    /* Publish the drained position back to the descriptor. The kext's
     * watchdog (per channel-progress-flag-RE.md) reads this to confirm
     * the host is making forward progress on the ring. */
    lagfx_ring_publish_read_ptr(p, desc_gpa, rp);

    LAGFX_LOG("display drain: ch=%u drained=%zu new rp=0x%x",
              chan_id, cmds, rp);
    return cmds;
}
