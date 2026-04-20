/*
 * libapplegfx-vulkan — display-domain opcode handlers (Phase 2.A)
 * src/protocol/ops_display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 2.A real handlers for the display-pipe opcodes needed to drive
 * the first-pixel clear-colour path per
 * mos/paravirt-re/phase-2-first-pixel-plan.md §4:
 *
 *   CmdDisplayAck          (0x10) — guest acknowledges a prior
 *                                    display transaction completed.
 *   CmdDisplaySwapMapping  (0x12) — swap the mapping of guest-visible
 *                                    display ID → host-side scanout
 *                                    (buffer VA, stride, geometry).
 *   CmdDisplayTransaction3 (0x16) — submit a display transaction
 *                                    (for Phase 2: the clear-colour
 *                                    attachment carrier).
 *
 * Payload layouts are PARTIAL confidence per the Phase 2 brief — the
 * spec names the three fields that each opcode carries (see
 * command-buffer-format.md §3 "Display Commands") but the exact order
 * and byte layout is inferred. Handlers read the most-likely encoding
 * and error with LAGFX_HANDLER_ERR_STATE or LAGFX_HANDLER_ERR_SIZE on
 * mismatch; the dispatcher still signals the stamp (fail-open, §6).
 *
 * Runtime capture on a booted VM (per plan §8 item 4) would
 * definitively confirm the ordering; until then the commit message for
 * this handler set records the assumption.
 *
 * Layout assumptions (LOW-to-MEDIUM confidence):
 *
 *   CmdDisplayAck (0x10)
 *     payload[0..3]   u32 displayID
 *     payload[4..7]   u32 frameID         (aka transactionID being acked)
 *     Total: 8 bytes.
 *
 *   CmdDisplaySwapMapping (0x12)
 *     payload[0..3]    u32 displayID
 *     payload[4..7]    u32 mappingID       (guest-chosen swap cookie)
 *     payload[8..15]   u64 bufferVA        (guest VA into task-mapped RAM)
 *     payload[16..23]  u64 length          (scanout byte count; 0 = derive)
 *     payload[24..27]  u32 width_px
 *     payload[28..31]  u32 height_px
 *     payload[32..35]  u32 stride_bytes
 *     payload[36..39]  u32 format          (0 = BGRA8_UNORM)
 *     Total: 40 bytes.
 *
 *   CmdDisplayTransaction3 (0x16)
 *     payload[0..3]   u32 displayID
 *     payload[4..7]   u32 transactionID
 *     payload[8..11]  u32 attachmentCount   (Phase 2: 1 for clear-colour)
 *     payload[12..]   attachmentDescriptor [attachmentCount]
 *
 *   attachmentDescriptor (32 bytes per attachment — PARTIAL):
 *     [0..3]   u32 attachmentIndex
 *     [4..7]   u32 loadAction    (0=dontcare, 1=load, 2=clear)
 *     [8..11]  u32 storeAction   (0=dontcare, 1=store, 2=multisampleResolve)
 *     [12..15] u32 flags
 *     [16..19] f32 clearR
 *     [20..23] f32 clearG
 *     [24..27] f32 clearB
 *     [28..31] f32 clearA
 *
 *     Total per attachment: 32 bytes.
 *
 * Side effects:
 *   - DisplayAck marks the pending transaction on the named display as
 *     acknowledged and bumps display_acks_received.
 *   - DisplaySwapMapping finds-or-allocates the display entry and
 *     updates its (bufferVA, length, geometry, stride, format, mappingID)
 *     fields; bumps display_swaps_applied. If the display table is full
 *     returns LAGFX_HANDLER_ERR_STATE (the dispatcher still stamps).
 *   - DisplayTransaction3 parses up to N attachments (N capped for
 *     stack safety), extracts the clear-colour from the first attachment
 *     when loadAction==2 (clear), records it on the display entry, and
 *     marks transaction_pending=true. Bumps display_transactions_submitted.
 *
 * All three handlers fail-open for unknown displayIDs: DisplayAck logs
 * and returns OK with a state-miss warning; the two emitters that take
 * a mapping/transaction auto-allocate a slot on first sight so the
 * guest doesn't need to pre-register displays. Apple's dylib handles
 * the implicit-register case identically (per dylib-inventory.md).
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

#include <stdint.h>
#include <string.h>

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

/* IEEE 754 little-endian f32 decode. x86-64 host is LE so a direct
 * memcpy is equivalent; we read via u32 first to keep the code
 * portable against a big-endian test host. */
static inline float lagfx_lef32(const uint8_t *b) {
    uint32_t u = lagfx_le32(b);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* === CmdDisplayAck (0x10) — Phase 2.A =================================
 *
 * Guest tells the host "the transaction that touched displayID=X,
 * frameID=Y is done on my side — you can reclaim any resources pinned
 * to it." In the Phase 2 first-pixel path this fires once per clear
 * round-trip, after the guest has consumed the present-notification.
 *
 * For Phase 2 the only observable side effect is:
 *   - Unknown displayID: log + return OK (fail-open).
 *   - Known displayID whose pending transaction's ID matches frameID:
 *     mark transaction_acked=true, transaction_pending=false, bump
 *     display_acks_received.
 *   - Known displayID where the pending txn ID does NOT match: log a
 *     mismatch warning (possible reordering), still bump the counter
 *     — Apple's dylib does not error on out-of-order acks, it just
 *     counts them.
 * ===================================================================== */

lagfx_handler_status_t lagfx_op_display_ack(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdDisplayAck: payload missing or too small "
                   "(size=%u, need 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    uint32_t frame_id   = lagfx_le32(hdr->payload + 4);

    p->display_acks_received += 1;

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplayAck: displayID=%u frameID=%u — display not "
                   "registered (fail-open, no-op)",
                   display_id, frame_id);
        return LAGFX_HANDLER_OK;
    }

    if (d->transaction_pending && d->pending_transaction_id == frame_id) {
        d->transaction_pending = false;
        d->transaction_acked   = true;
        LAGFX_LOG("CmdDisplayAck: displayID=%u frameID=%u stamp=0x%08x "
                  "(matched pending txn)",
                  display_id, frame_id, hdr->stamp);
    } else if (d->transaction_pending) {
        LAGFX_WARN("CmdDisplayAck: displayID=%u frameID=%u "
                   "!= pending txn=%u (out-of-order ack — tolerating)",
                   display_id, frame_id, d->pending_transaction_id);
        /* Still record the ack so a later-arriving match can resolve
         * against the pending field; we don't clear pending here. */
    } else {
        LAGFX_LOG("CmdDisplayAck: displayID=%u frameID=%u stamp=0x%08x "
                  "(no pending txn to match — idempotent)",
                  display_id, frame_id, hdr->stamp);
    }

    return LAGFX_HANDLER_OK;
}

/* === CmdDisplaySwapMapping (0x12) — Phase 2.A =========================
 *
 * Establishes the mapping of a guest-visible display to a host-side
 * scanout buffer (guest VA into task-mapped RAM). In Phase 2 the shell
 * will later vkCmdCopyImageToBuffer the rendered clear into a
 * HOST_VISIBLE staging buffer and memcpy into this region for noVNC to
 * pick up — see phase-2-first-pixel-plan.md §2.B.4.
 *
 * The handler records {bufferVA, length, width, height, stride,
 * format} on the display entry. If the display isn't known yet it is
 * auto-allocated; if the table is full, returns ERR_STATE (stamp still
 * signals — the guest will time out and retry).
 * ===================================================================== */

lagfx_handler_status_t lagfx_op_display_swap_mapping(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 40) {
        LAGFX_WARN("CmdDisplaySwapMapping: payload missing or too small "
                   "(size=%u, need 40)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    uint32_t mapping_id = lagfx_le32(hdr->payload + 4);
    uint64_t buffer_va  = lagfx_le64(hdr->payload + 8);
    uint64_t length     = lagfx_le64(hdr->payload + 16);
    uint32_t width      = lagfx_le32(hdr->payload + 24);
    uint32_t height     = lagfx_le32(hdr->payload + 28);
    uint32_t stride     = lagfx_le32(hdr->payload + 32);
    uint32_t format     = lagfx_le32(hdr->payload + 36);

    lagfx_display_entry_t *d = lagfx_protocol_get_or_alloc_display(p,
                                                                   display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplaySwapMapping: display table full (max=%u), "
                   "displayID=%u",
                   LAGFX_PROTO_MAX_DISPLAYS, display_id);
        return LAGFX_HANDLER_ERR_STATE;
    }

    d->live       = true;
    d->mapping_id = mapping_id;
    d->buffer_va  = buffer_va;
    d->length     = length;
    d->width      = width;
    d->height     = height;
    d->stride     = stride;
    d->format     = format;
    d->mapped     = true;

    p->display_swaps_applied += 1;

    LAGFX_LOG("CmdDisplaySwapMapping: displayID=%u mappingID=%u "
              "bufferVA=0x%llx length=%llu %ux%u stride=%u fmt=%u "
              "stamp=0x%08x",
              display_id, mapping_id,
              (unsigned long long)buffer_va,
              (unsigned long long)length,
              width, height, stride, format, hdr->stamp);

    return LAGFX_HANDLER_OK;
}

/* === CmdDisplayTransaction3 (0x16) — Phase 2.A ========================
 *
 * Submit a rendering transaction against a display. This is the
 * guest-side "present this frame" signal; for Phase 2 (first-pixel)
 * the only transaction shape we recognise is a single clear-colour
 * attachment (loadAction = clear, RGBA floats). Draw-call paths are
 * Phase 3 territory and remain scaffolded.
 *
 * Parse strategy:
 *   1. Read {displayID, transactionID, attachmentCount}.
 *   2. Overflow-safe size check: 12 + attachmentCount * 32 fits.
 *   3. For each attachment (capped at 8 for stack safety): read
 *      attachmentIndex, loadAction, storeAction, flags, RGBA.
 *   4. Record the first attachment's clear-colour on the display entry
 *      if loadAction==2 (clear). This is the value Phase 2.B.2 will
 *      pull into lagfx_translate_clear_color → vkCmdBeginRendering.
 *   5. Mark transaction_pending=true; guest will ack via 0x10.
 *
 * Phase 2.A is parse-and-log: we do NOT yet submit a VkCommandBuffer
 * with vkCmdClearColorImage — that's Phase 2.B. This handler closes
 * the wire-loop so the guest's present call completes.
 * ===================================================================== */

#define LAGFX_DISPLAY_TX_MAX_ATTACHMENTS 8u
#define LAGFX_DISPLAY_TX_ATTACHMENT_BYTES 32u

lagfx_handler_status_t lagfx_op_display_transaction3(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12) {
        LAGFX_WARN("CmdDisplayTransaction3: payload missing or too small "
                   "(size=%u, need >= 12)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id     = lagfx_le32(hdr->payload + 0);
    uint32_t transaction_id = lagfx_le32(hdr->payload + 4);
    uint32_t attach_count   = lagfx_le32(hdr->payload + 8);

    /* Overflow-safe size check: 12 + 32*count must fit. */
    if (attach_count > ((uint32_t)hdr->payload_size - 12u) /
                       LAGFX_DISPLAY_TX_ATTACHMENT_BYTES) {
        LAGFX_WARN("CmdDisplayTransaction3: attachmentCount=%u exceeds "
                   "payload (size=%u)",
                   attach_count, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Cap the attachment count we actually parse — 8 is far more than
     * Metal renders ever carry (MTLRenderPassDescriptor.colorAttachments
     * is size 8 per Apple spec) so this is not a practical limit, it
     * just bounds our stack use. */
    uint32_t parse_count = attach_count;
    if (parse_count > LAGFX_DISPLAY_TX_MAX_ATTACHMENTS) {
        LAGFX_WARN("CmdDisplayTransaction3: attachmentCount=%u capped at %u "
                   "for parse",
                   attach_count, LAGFX_DISPLAY_TX_MAX_ATTACHMENTS);
        parse_count = LAGFX_DISPLAY_TX_MAX_ATTACHMENTS;
    }

    lagfx_display_entry_t *d = lagfx_protocol_get_or_alloc_display(p,
                                                                   display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplayTransaction3: display table full (max=%u), "
                   "displayID=%u",
                   LAGFX_PROTO_MAX_DISPLAYS, display_id);
        return LAGFX_HANDLER_ERR_STATE;
    }
    d->live = true;

    /* Clear out last-attachment summary before repopulating. */
    d->last_attachment_count = attach_count;
    d->last_load_action      = 0;
    d->last_clear_rgba[0]    = 0.f;
    d->last_clear_rgba[1]    = 0.f;
    d->last_clear_rgba[2]    = 0.f;
    d->last_clear_rgba[3]    = 0.f;

    /* Parse attachments and capture the first clear-colour. */
    for (uint32_t i = 0; i < parse_count; ++i) {
        const uint8_t *a =
            hdr->payload + 12u + i * LAGFX_DISPLAY_TX_ATTACHMENT_BYTES;
        uint32_t idx          = lagfx_le32(a + 0);
        uint32_t load_action  = lagfx_le32(a + 4);
        uint32_t store_action = lagfx_le32(a + 8);
        uint32_t flags        = lagfx_le32(a + 12);
        float r = lagfx_lef32(a + 16);
        float g = lagfx_lef32(a + 20);
        float b = lagfx_lef32(a + 24);
        float al = lagfx_lef32(a + 28);

        LAGFX_LOG("CmdDisplayTransaction3: attach[%u] idx=%u load=%u "
                  "store=%u flags=0x%x clear=(%f,%f,%f,%f)",
                  i, idx, load_action, store_action, flags,
                  (double)r, (double)g, (double)b, (double)al);

        /* Record first attachment's state as the display's
         * last-transaction summary. Phase 2.B pulls out the clear
         * colour to drive vkCmdClearColorImage / render-pass clear. */
        if (i == 0) {
            d->last_load_action   = (uint8_t)(load_action & 0xff);
            d->last_clear_rgba[0] = r;
            d->last_clear_rgba[1] = g;
            d->last_clear_rgba[2] = b;
            d->last_clear_rgba[3] = al;
        }
    }

    d->pending_transaction_id = transaction_id;
    d->transaction_pending    = true;
    d->transaction_acked      = false;

    p->display_transactions_submitted += 1;

    LAGFX_LOG("CmdDisplayTransaction3: displayID=%u transactionID=%u "
              "attachments=%u parsed=%u stamp=0x%08x "
              "(Phase 2.A parse-and-log; Phase 2.B will submit VkClear)",
              display_id, transaction_id, attach_count, parse_count,
              hdr->stamp);

    return LAGFX_HANDLER_OK;
}
