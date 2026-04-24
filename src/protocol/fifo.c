/*
 * libapplegfx-vulkan — FIFO ring dequeue (Phase 1.A.2 skeleton)
 * src/protocol/fifo.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The command header is 12 bytes (see opcodes.h and
 * re-followup-spec-gaps.md §5.1). The doorbell is a write-pointer
 * update, not a stamp, and lives at an MMIO offset in the
 * 0x1004..0x1034 range that we haven't identified yet.
 *
 * This TU owns two real pieces of code:
 *   - lagfx_fifo_parse_header: decodes the 12-byte on-wire header
 *     into lagfx_cmd_header_t. Unit-tested.
 *   - lagfx_fifo_on_mmio_setter: probe handler for any MMIO write
 *     in the setter-candidate range. Logs (offset, value) and records
 *     the most recent pair for test inspection / runtime capture.
 *
 * The ring walk (lagfx_fifo_drain) remains stubbed pending R1.
 */

#include "fifo.h"
#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "../common/log.h"
#include "../device.h"

#include <string.h>

/* Sanity cap: one ring has hundreds of bytes of commands at most during
 * any single drain; 128 commands × 4 KiB is an absurd ceiling that
 * still protects against a runaway guest writing garbage. Raise later
 * if real workloads need more. */
#define LAGFX_FIFO_DRAIN_MAX_CMDS 128u
#define LAGFX_FIFO_MAX_CMD_BYTES  4096u

/* Little-endian u16 / u32 readers. Guest protocol is x86-64 LE per
 * command-buffer-format.md §2. We do a per-byte read so this compiles
 * to something sensible regardless of host alignment rules. */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

bool lagfx_fifo_parse_header(const uint8_t *bytes, size_t len,
                             lagfx_cmd_header_t *hdr_out) {
    if (!bytes || !hdr_out || len < LAGFX_CMD_HEADER_BYTES) {
        return false;
    }

    uint16_t opcode       = read_le16(bytes + 0);
    uint16_t arg_count_8b = read_le16(bytes + 2);
    uint32_t total_length = read_le32(bytes + 4);
    uint32_t stamp        = read_le32(bytes + 8);

    /* Sanity: length must be at least the header size. */
    if (total_length < LAGFX_CMD_HEADER_BYTES) {
        return false;
    }

    uint32_t payload_bytes = total_length - LAGFX_CMD_HEADER_BYTES;

    /* The caller may have only the 12 header bytes; flag that via
     * payload==NULL but still report payload_size so handlers can
     * distinguish "no payload" from "payload was elided". */
    const uint8_t *payload = NULL;
    if (payload_bytes > 0 && len >= (size_t)total_length) {
        payload = bytes + LAGFX_CMD_HEADER_BYTES;
    }

    hdr_out->opcode       = opcode;
    hdr_out->arg_count_8b = arg_count_8b;
    hdr_out->length       = total_length;
    hdr_out->stamp        = stamp;
    /* payload_size is declared u16; clamp the derived value. In
     * practice `length` is bounded by the ring size (128 KB), so this
     * clamp only triggers on malformed input. */
    hdr_out->payload_size =
        (payload_bytes > 0xffffu) ? 0xffffu : (uint16_t)payload_bytes;
    hdr_out->payload      = payload;

    return true;
}

void lagfx_fifo_on_mmio_setter(lagfx_protocol_t *p,
                               uint64_t offset, uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_setter_offset = (uint32_t)offset;
    p->last_setter_value  = value;
    p->setter_write_count += 1;

    /* Log at the candidate level until the runtime capture nails down
     * which offset carries which setter. The value is logged as both
     * hex (page number / length-in-bytes shape) and dec (byte offset
     * / write pointer shape) to aid disambiguation. */
    LAGFX_LOG("fifo: setter-candidate write off=0x%llx val=0x%08x (%u) #%llu",
              (unsigned long long)offset,
              value, value,
              (unsigned long long)p->setter_write_count);

    /* Attempt a drain regardless — if the write happened to be the
     * real doorbell, drain will work once ring geometry is known.
     * For now it's a no-op either way. */
    (void)lagfx_fifo_drain(p);
}

size_t lagfx_fifo_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_LOG("fifo_drain: ring not armed (armed=%d size=0x%x gpa=0x%llx)",
                  (int)p->ring_armed, p->ring_size,
                  (unsigned long long)p->ring_base_gpa);
        return 0;
    }

    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_WARN("fifo_drain: no shell.read_memory callback");
        return 0;
    }

    /* Standard ring-buffer drain: advance read_ptr toward write_ptr,
     * parsing each 12-byte header + payload in place, dispatching via
     * the opcode handler table. Stop when read_ptr catches write_ptr
     * or on malformed data (defensive). Handles ring wrap naturally
     * via modulo arithmetic on read_ptr.
     */
    size_t drained = 0;
    for (unsigned i = 0; i < LAGFX_FIFO_DRAIN_MAX_CMDS; ++i) {
        if (p->read_ptr == p->write_ptr) {
            break;  /* caught up */
        }

        /* Wrap at the ring boundary. */
        uint32_t rp = p->read_ptr % p->ring_size;
        uint64_t hdr_gpa = p->ring_base_gpa + (uint64_t)rp;

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            hdr_gpa,
                                            LAGFX_CMD_HEADER_BYTES,
                                            hdr_buf)) {
            LAGFX_WARN("fifo_drain: DMA read of header at gpa=0x%llx failed",
                       (unsigned long long)hdr_gpa);
            break;
        }

        lagfx_cmd_header_t hdr;
        if (!lagfx_fifo_parse_header(hdr_buf, LAGFX_CMD_HEADER_BYTES,
                                     &hdr)) {
            LAGFX_LOG("fifo_drain: malformed header at rp=0x%x — stop", rp);
            break;
        }

        if (hdr.length < LAGFX_CMD_HEADER_BYTES ||
            hdr.length > LAGFX_FIFO_MAX_CMD_BYTES ||
            hdr.length > p->ring_size) {
            LAGFX_WARN("fifo_drain: bad length at rp=0x%x (len=0x%x) — stop",
                       rp, hdr.length);
            break;
        }

        /* Read the whole command (header + payload) into a local buf.
         * Handles ring wrap by a second DMA for the tail when needed. */
        uint8_t cmd_buf[LAGFX_FIFO_MAX_CMD_BYTES];
        uint32_t head = p->ring_size - rp;
        if (head >= hdr.length) {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                hdr_gpa, hdr.length,
                                                cmd_buf)) {
                LAGFX_WARN("fifo_drain: DMA read of cmd body failed");
                break;
            }
        } else {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                hdr_gpa, head, cmd_buf)) {
                LAGFX_WARN("fifo_drain: DMA read of wrapped head failed");
                break;
            }
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                p->ring_base_gpa,
                                                hdr.length - head,
                                                cmd_buf + head)) {
                LAGFX_WARN("fifo_drain: DMA read of wrapped tail failed");
                break;
            }
        }

        LAGFX_LOG("fifo_drain: dispatch rp=0x%x opcode=0x%x len=0x%x stamp=0x%x",
                  rp, hdr.opcode, hdr.length, hdr.stamp);

        /* Hex dump of the full command (up to 32 bytes) for stamp-field
         * and payload cross-referencing during M3 bring-up. Resolves the
         * contradiction between spec-gaps §13 (stamps in trace 1..7) and
         * the static RE claim that `[stream+8..11]` is always zero. */
        if (lagfx_log_enabled()) {
            uint32_t dump_len = hdr.length < 32u ? hdr.length : 32u;
            char hexline[128];
            size_t pos = 0;
            for (uint32_t i = 0; i < dump_len && pos + 4 < sizeof(hexline); ++i) {
                int n = snprintf(hexline + pos, sizeof(hexline) - pos,
                                 "%02x ", cmd_buf[i]);
                if (n < 0 || (size_t)n >= sizeof(hexline) - pos) break;
                pos += (size_t)n;
            }
            hexline[pos] = '\0';
            LAGFX_LOG("fifo_drain: bytes[%u] %s", dump_len, hexline);
        }

        /* Expose ring-header GPA to handlers so they can DMA-write
         * header slots (e.g. 0x3a's actual_count) BEFORE dispatch_one
         * fires the stamp + IRQ — otherwise the guest services the
         * IRQ and reads stale bytes. */
        p->current_cmd_header_gpa = hdr_gpa;
        p->current_stamp_id = 0u;  /* RootChannel always uses slot 0 */

        (void)lagfx_protocol_dispatch_one(p, cmd_buf, hdr.length);

        p->current_cmd_header_gpa = 0;

        p->read_ptr = (rp + hdr.length) % p->ring_size;
        drained += 1;
    }

    LAGFX_LOG("fifo_drain: drained=%zu, new read_ptr=0x%x",
              drained, p->read_ptr);
    return drained;
}

/* === Child-channel ring drain (RE_SESSION_2026_04_24.md §9) ======= */

#define LAGFX_CHDESC_OFFSET_BASE 0x400u
#define LAGFX_CHDESC_STRIDE      20u
#define LAGFX_CHDESC_WRITE_HEAD  0u
#define LAGFX_CHDESC_READ_HEAD   4u
#define LAGFX_CHDESC_CHAN_ID     0x0cu
#define LAGFX_CHDESC_RING_PFN    0x10u
#define LAGFX_CHILD_RING_SIZE    0x10000u  /* 64 KiB per A7a RE */

size_t lagfx_fifo_drain_child_channel(lagfx_protocol_t *p,
                                      uint32_t channel_id) {
    if (!lagfx_protocol_is_valid(p)) return 0;

    if (p->ring_shared_page_pfn == 0u) {
        LAGFX_WARN("child_drain: shared page PFN not set");
        return 0;
    }
    if (!p->dev || !p->dev->desc.shell.read_memory ||
        !p->dev->desc.shell.write_memory) {
        LAGFX_WARN("child_drain: no shell read/write callbacks");
        return 0;
    }
    if (channel_id == 0u) {
        LAGFX_WARN("child_drain: channel_id=0 is RootChannel; use lagfx_fifo_drain");
        return 0;
    }

    uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
    uint64_t desc_gpa = shared_gpa + LAGFX_CHDESC_OFFSET_BASE
                        + (uint64_t)(channel_id - 1u) * LAGFX_CHDESC_STRIDE;

    uint8_t desc_buf[LAGFX_CHDESC_STRIDE];
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        desc_gpa, sizeof(desc_buf),
                                        desc_buf)) {
        LAGFX_WARN("child_drain: DMA read of descriptor at gpa=0x%llx failed",
                   (unsigned long long)desc_gpa);
        return 0;
    }

    uint32_t write_head   = read_le32(desc_buf + LAGFX_CHDESC_WRITE_HEAD);
    uint32_t read_head    = read_le32(desc_buf + LAGFX_CHDESC_READ_HEAD);
    uint32_t desc_chan_id = read_le32(desc_buf + LAGFX_CHDESC_CHAN_ID);
    uint32_t ring_tbl_pfn = read_le32(desc_buf + LAGFX_CHDESC_RING_PFN);

    LAGFX_LOG("child_drain[ch=%u]: write_head=0x%x read_head=0x%x "
              "desc_chan_id=%u ring_tbl_pfn=0x%x",
              channel_id, write_head, read_head, desc_chan_id, ring_tbl_pfn);

    if (desc_chan_id != channel_id) {
        /* The descriptor slot doesn't match — likely the descriptor
         * table isn't populated at the expected offset. Log and bail. */
        LAGFX_WARN("child_drain[ch=%u]: descriptor chan_id=%u mismatch",
                   channel_id, desc_chan_id);
        return 0;
    }

    if (write_head == read_head) {
        LAGFX_LOG("child_drain[ch=%u]: nothing to do (w==r)", channel_id);
        return 0;
    }

    if (ring_tbl_pfn == 0u) {
        LAGFX_WARN("child_drain[ch=%u]: ring_tbl_pfn=0", channel_id);
        return 0;
    }

    /* The ring_pfn_table page holds u32[] of ring page PFNs. Read the
     * first entry — that's the ring's first data page. For M3 we assume
     * the ring fits in a single page initially; span multi-page rings
     * later via iteration over the PFN table. */
    uint64_t ring_tbl_gpa = (uint64_t)ring_tbl_pfn << 12;
    uint8_t tbl_buf[4];
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        ring_tbl_gpa, sizeof(tbl_buf),
                                        tbl_buf)) {
        LAGFX_WARN("child_drain[ch=%u]: read of ring_tbl failed", channel_id);
        return 0;
    }
    uint32_t ring_page_pfn = read_le32(tbl_buf);
    uint64_t ring_gpa = (uint64_t)ring_page_pfn << 12;
    LAGFX_LOG("child_drain[ch=%u]: ring_gpa=0x%llx (pfn=0x%x)",
              channel_id, (unsigned long long)ring_gpa, ring_page_pfn);

    /* Drain commands from [read_head..write_head) in the ring page. */
    size_t drained = 0;
    uint32_t rp = read_head;
    for (unsigned i = 0; i < LAGFX_FIFO_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_head) break;

        uint32_t cursor = rp % LAGFX_CHILD_RING_SIZE;
        uint64_t hdr_gpa = ring_gpa + (uint64_t)cursor;
        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            hdr_gpa,
                                            LAGFX_CMD_HEADER_BYTES,
                                            hdr_buf)) {
            LAGFX_WARN("child_drain[ch=%u]: DMA read of hdr failed", channel_id);
            break;
        }

        lagfx_cmd_header_t hdr;
        if (!lagfx_fifo_parse_header(hdr_buf, LAGFX_CMD_HEADER_BYTES, &hdr)) {
            LAGFX_WARN("child_drain[ch=%u]: header parse failed at rp=0x%x",
                       channel_id, cursor);
            break;
        }

        if (hdr.length < LAGFX_CMD_HEADER_BYTES ||
            hdr.length > LAGFX_FIFO_MAX_CMD_BYTES) {
            LAGFX_WARN("child_drain[ch=%u]: bad length 0x%x at rp=0x%x",
                       channel_id, hdr.length, cursor);
            break;
        }

        uint8_t cmd_buf[LAGFX_FIFO_MAX_CMD_BYTES];
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            hdr_gpa, hdr.length, cmd_buf)) {
            LAGFX_WARN("child_drain[ch=%u]: DMA read of body failed",
                       channel_id);
            break;
        }

        LAGFX_LOG("child_drain[ch=%u]: dispatch opcode=0x%x len=0x%x stamp=0x%x",
                  channel_id, hdr.opcode, hdr.length, hdr.stamp);

        p->current_cmd_header_gpa = hdr_gpa;
        /* Per-channel: stamp_idx maps 1:1 to channel_id per dmesg
         * evidence (Display0 at channel 5 waits on stamp_idx=5). */
        p->current_stamp_id = channel_id;

        (void)lagfx_protocol_dispatch_one(p, cmd_buf, hdr.length);
        p->current_cmd_header_gpa = 0;

        rp += hdr.length;
        drained += 1;
    }

    /* Update read_head = write_head in the descriptor so the kext sees
     * the ring as drained and can submit more work. */
    uint32_t new_read_head = write_head;
    if (!p->dev->desc.shell.write_memory(
            p->dev->desc.shell.opaque,
            desc_gpa + LAGFX_CHDESC_READ_HEAD,
            sizeof(new_read_head),
            &new_read_head)) {
        LAGFX_WARN("child_drain[ch=%u]: failed to write read_head back",
                   channel_id);
    } else {
        LAGFX_LOG("child_drain[ch=%u]: read_head := 0x%x (drained=%zu)",
                  channel_id, new_read_head, drained);
    }

    return drained;
}
