/*
 * libapplegfx-vulkan — protocol decoder lifecycle + dispatcher
 * src/protocol/protocol.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 scaffold. Provides:
 *
 *   - lagfx_protocol_{new,free,reset} lifecycle.
 *   - lagfx_protocol_mmio_{read,write} register shadow + setter
 *     candidate probe (doorbell offset is not yet identified).
 *   - lagfx_protocol_dispatch_one — parse 12-byte header, look up
 *     opcode, invoke handler, run completion path.
 *
 * The FIFO ring dequeue itself is stubbed in fifo.c (R1 gap).
 * Tests drive dispatch via lagfx_protocol_dispatch_one directly.
 */

#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "fifo.h"
#include "../device.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Lifecycle ============================================== */

lagfx_protocol_t *lagfx_protocol_new(struct lagfx_device *dev) {
    if (!lagfx_device_is_valid(dev)) {
        LAGFX_ERR("protocol_new: invalid device %p", (void *)dev);
        return NULL;
    }

    lagfx_protocol_t *p =
        (lagfx_protocol_t *)calloc(1, sizeof(*p));
    if (!p) {
        LAGFX_ERR("protocol_new: out of memory");
        return NULL;
    }

    p->magic = LAGFX_PROTOCOL_MAGIC;
    p->dev   = dev;

    /* Register defaults. Bit 0 of STATUS_CONTROL = "present", bit 1
     * = "ready" per §3.1 of the brief (inferred). */
    p->reg[0] = 0x3u;  /* STATUS_CONTROL */

    LAGFX_LOG("protocol_new: p=%p dev=%p", (void *)p, (void *)dev);
    return p;
}

void lagfx_protocol_free(lagfx_protocol_t *p) {
    if (!p) {
        return;
    }
    if (p->magic != LAGFX_PROTOCOL_MAGIC) {
        LAGFX_ERR("protocol_free: bad magic on %p (got 0x%08x)",
                  (void *)p, p->magic);
        return;
    }
    LAGFX_LOG("protocol_free: p=%p (seen=%llu, completed=%llu, unknown=%llu)",
              (void *)p,
              (unsigned long long)p->total_cmds_seen,
              (unsigned long long)p->total_cmds_completed,
              (unsigned long long)p->unknown_opcode_count);
    memset(p, 0, sizeof(*p));
    free(p);
}

void lagfx_protocol_reset(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    LAGFX_LOG("protocol_reset: p=%p", (void *)p);

    /* Clear tables and inflight; preserve ring geometry and registers. */
    memset(p->tasks,    0, sizeof(p->tasks));
    memset(p->fifos,    0, sizeof(p->fifos));
    memset(p->inflight, 0, sizeof(p->inflight));
    memset(p->displays, 0, sizeof(p->displays));

    p->total_cmds_seen      = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->interrupts_raised    = 0;
    p->last_completed_stamp = 0;
    p->last_setter_offset   = 0;
    p->last_setter_value    = 0;
    p->setter_write_count   = 0;
    p->read_ptr             = 0;
    p->write_ptr            = 0;

    p->display_swaps_applied          = 0;
    p->display_transactions_submitted = 0;
    p->display_acks_received          = 0;

    /* Phase 3.A inner-opcode counters. */
    p->inner_opcodes_processed            = 0;
    p->inner_opcodes_bind_pipeline        = 0;
    p->inner_opcodes_bind_vertex_buffer   = 0;
    p->inner_opcodes_bind_fragment_resource = 0;
    p->inner_opcodes_set_render_target    = 0;
    p->inner_opcodes_draw                 = 0;
    p->inner_opcodes_set_viewport         = 0;
    p->inner_opcodes_unknown              = 0;
}

/* === Completion path ========================================
 *
 * Every command unconditionally signals its stamp when done. Per
 * re-followup-spec-gaps.md §5.1, the 12-byte header has no flags
 * field; the dylib's Cmd* handler tails always invoke the "signal
 * stamp" selector, which pushes the stamp into PGPendingStampQueue,
 * and the dequeue thread later writes the stamp into the host-to-
 * guest stamp cell (readable at MMIO 0x1014) and raises the IRQ.
 * We model that here by writing the stamp to the STAMP_CELL_1 shadow
 * and calling shell.raise_interrupt. */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    int cell_idx = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_1);
    if (cell_idx >= 0) {
        p->reg[cell_idx] = stamp;
    }
    /* Also mirror into STAMP_CELL_2 (interrupt-cause cell) — the real
     * device uses two distinct u32 slots, read-side is atomic-xchg. */
    int irq_cell_idx = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_2);
    if (irq_cell_idx >= 0) {
        p->reg[irq_cell_idx] = stamp;
    }

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque,
                                           /*vector=*/0);
        p->interrupts_raised += 1;
        LAGFX_LOG("complete_stamp: stamp=0x%08x "
                  "(stamp cell written + IRQ raised)",
                  stamp);
    } else {
        LAGFX_LOG("complete_stamp: stamp=0x%08x "
                  "(stamp cell written, no shell IRQ cb)",
                  stamp);
    }
}

/* === Dispatcher ============================================ */

int lagfx_protocol_dispatch_one(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes,
                                size_t cmd_len) {
    if (!lagfx_protocol_is_valid(p) || !cmd_bytes) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    lagfx_cmd_header_t hdr;
    if (!lagfx_fifo_parse_header(cmd_bytes, cmd_len, &hdr)) {
        LAGFX_ERR("dispatch: header parse failed (len=%zu)", cmd_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    p->total_cmds_seen += 1;

    const lagfx_op_descriptor_t *desc = lagfx_opcode_lookup(hdr.opcode);
    lagfx_op_handler_fn fn = (desc && desc->handler) ? desc->handler
                                                     : lagfx_op_default_handler;

    if (!desc) {
        p->unknown_opcode_count += 1;
    }

    LAGFX_LOG("dispatch: op=0x%04x (%s) stamp=0x%08x arg_count_8b=%u "
              "length=%u payload=%u",
              hdr.opcode, lagfx_opcode_name(hdr.opcode),
              hdr.stamp, (unsigned)hdr.arg_count_8b,
              (unsigned)hdr.length, (unsigned)hdr.payload_size);

    /* Payload-size guard. Handlers that want tighter validation do
     * their own extra checks. */
    if (desc) {
        if (hdr.payload_size < desc->min_payload) {
            LAGFX_WARN("dispatch: %s payload too small (%u < %u min)",
                       desc->name, (unsigned)hdr.payload_size,
                       (unsigned)desc->min_payload);
            /* Still run completion so guest doesn't hang. */
            lagfx_protocol_complete_stamp(p, hdr.stamp);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        if (desc->max_payload != 0 && hdr.payload_size > desc->max_payload) {
            LAGFX_WARN("dispatch: %s payload too large (%u > %u max)",
                       desc->name, (unsigned)hdr.payload_size,
                       (unsigned)desc->max_payload);
            /* Fall through anyway — fail-open. */
        }
    }

    lagfx_handler_status_t rc = fn(p, &hdr);

    /* Completion path runs unconditionally — see note on
     * lagfx_protocol_complete_stamp. Even if the handler returned
     * an error we write the stamp per the fail-open note in §6.3. */
    lagfx_protocol_complete_stamp(p, hdr.stamp);

    return (int)rc;
}

/* === MMIO register shadow =================================== */

uint32_t lagfx_protocol_mmio_read(lagfx_protocol_t *p, uint64_t offset) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* MSI-X table range — shell owns it. Return 0, no log spam. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return 0;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_LOG("mmio_read: unmapped offset 0x%llx -> 0",
                  (unsigned long long)offset);
        return 0;
    }

    uint32_t value = p->reg[idx];

    LAGFX_LOG("mmio_read: off=0x%llx -> 0x%08x",
              (unsigned long long)offset, value);
    return value;
}

void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset,
                               uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    /* MSI-X range — shell's problem. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_LOG("mmio_write: unmapped offset 0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    /* Shadow first so reads reflect the write even if we short-circuit
     * into a setter probe. Note that 0x101c is documented read-only in
     * the dylib (returns _rootPageNumber); we still shadow the written
     * value so a subsequent read in the test rig can confirm the path
     * was taken. The real hardware behavior on write to 0x101c is also
     * "store to setter-backed ivar" (_PGDevice calls its 1-arg
     * selector), so shadowing is consistent. */
    p->reg[idx] = value;

    LAGFX_LOG("mmio_write: off=0x%llx val=0x%08x",
              (unsigned long long)offset, value);

    /* STATUS_CONTROL — master FIFO enable/disable bit. */
    if (offset == LAGFX_REG_STATUS_CONTROL) {
        p->ring_armed = (value != 0u);
        LAGFX_LOG("mmio_write: STATUS_CONTROL -> ring_armed=%d",
                  (int)p->ring_armed);
        return;
    }

    /* Any write in the setter-candidate range: log, record for probe,
     * attempt drain. One of {0x1004, 0x1008, 0x1010, 0x101c, 0x1020,
     * 0x1024, 0x1028, 0x1030, 0x1034} is the real doorbell
     * (setFifoWritten:) and three others carry ring-geometry setters;
     * runtime capture disambiguates. */
    if (offset >= LAGFX_REG_SETTER_CAND_FIRST &&
        offset <= LAGFX_REG_SETTER_CAND_LAST) {
        lagfx_fifo_on_mmio_setter(p, offset, value);
        return;
    }
}

/* === Stats accessors ======================================== */

void lagfx_protocol_stats(const lagfx_protocol_t *p,
                          uint64_t *total_cmds_seen_out,
                          uint64_t *total_cmds_completed_out,
                          uint64_t *unknown_opcode_count_out) {
    if (!lagfx_protocol_is_valid(p)) {
        if (total_cmds_seen_out)      *total_cmds_seen_out = 0;
        if (total_cmds_completed_out) *total_cmds_completed_out = 0;
        if (unknown_opcode_count_out) *unknown_opcode_count_out = 0;
        return;
    }
    if (total_cmds_seen_out)      *total_cmds_seen_out = p->total_cmds_seen;
    if (total_cmds_completed_out) *total_cmds_completed_out = p->total_cmds_completed;
    if (unknown_opcode_count_out) *unknown_opcode_count_out = p->unknown_opcode_count;
}

uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}

uint32_t lagfx_protocol_last_setter_offset(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_setter_offset : 0u;
}

uint32_t lagfx_protocol_last_setter_value(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_setter_value : 0u;
}

uint64_t lagfx_protocol_setter_write_count(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->setter_write_count : 0u;
}
