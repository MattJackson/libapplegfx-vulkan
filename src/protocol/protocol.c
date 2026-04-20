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
 *   - lagfx_protocol_mmio_{read,write} register shadow + doorbell
 *     forwarding.
 *   - lagfx_protocol_dispatch_one — parse header, look up opcode,
 *     invoke handler, run completion path.
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

    p->total_cmds_seen      = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->doorbell_writes      = 0;
    p->interrupts_raised    = 0;
    p->last_completed_stamp = 0;
    p->last_doorbell_stamp  = 0;
    p->read_ptr             = 0;
    p->write_ptr            = 0;
}

/* === Completion path ======================================== */

void lagfx_protocol_complete_stamp(lagfx_protocol_t *p,
                                   uint32_t stamp, uint8_t flags) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    /* Fence register always gets the latest stamp. Guests that
     * poll 0x1020 see it immediately. */
    int fence_idx = lagfx_protocol_reg_index(LAGFX_REG_FENCE);
    if (fence_idx >= 0) {
        p->reg[fence_idx] = stamp;
    }
    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    /* Interrupt arm is conditional. R6: bit-0 of flags is inferred
     * as COMPLETION_EXPECTED; until confirmed we also fire on any
     * non-zero flags byte. */
    bool completion_requested =
        (flags & LAGFX_FLAG_COMPLETION_EXPECTED) != 0;

    if (completion_requested && p->dev &&
        p->dev->desc.shell.raise_interrupt) {
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque,
                                           /*vector=*/0);
        p->interrupts_raised += 1;
        LAGFX_LOG("complete_stamp: stamp=0x%08x flags=0x%02x "
                  "(fence written + IRQ raised)",
                  stamp, flags);
    } else {
        LAGFX_LOG("complete_stamp: stamp=0x%08x flags=0x%02x "
                  "(fence written, no IRQ)",
                  stamp, flags);
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

    LAGFX_LOG("dispatch: op=0x%02x (%s) stamp=0x%08x flags=0x%02x "
              "length=%u payload=%u",
              hdr.opcode, lagfx_opcode_name(hdr.opcode),
              hdr.stamp, hdr.flags,
              (unsigned)hdr.length, (unsigned)hdr.payload_size);

    /* Payload-size guard. Handlers that want tighter validation do
     * their own extra checks. */
    if (desc) {
        if (hdr.payload_size < desc->min_payload) {
            LAGFX_WARN("dispatch: %s payload too small (%u < %u min)",
                       desc->name, (unsigned)hdr.payload_size,
                       (unsigned)desc->min_payload);
            /* Still run completion so guest doesn't hang. */
            lagfx_protocol_complete_stamp(p, hdr.stamp, hdr.flags);
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

    /* Completion path. Even if the handler returned an error we
     * write the fence per the fail-open note in §6.3. */
    lagfx_protocol_complete_stamp(p, hdr.stamp, hdr.flags);

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

    /* FIFO_STATE is derived, not stored. */
    if (offset == LAGFX_REG_FIFO_STATE) {
        value = (p->read_ptr != p->write_ptr) ? 1u : 0u;
    }

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

    /* Shadow first so reads reflect the write even if we short-circuit. */
    p->reg[idx] = value;

    LAGFX_LOG("mmio_write: off=0x%llx val=0x%08x",
              (unsigned long long)offset, value);

    /* Doorbell — drive the FIFO drain. */
    if (offset == LAGFX_REG_DOORBELL) {
        lagfx_fifo_on_doorbell(p, value);
        return;
    }

    /* Queue-control arm/disarm. */
    if (offset == LAGFX_REG_QUEUE_CONTROL) {
        p->ring_armed = (value & 0x1u) != 0u;
        LAGFX_LOG("mmio_write: ring_armed=%d", (int)p->ring_armed);
        return;
    }

    /* INTR_CLEAR write side wipes pending bits (we don't track any
     * yet in 1.A.2, but preserve the read-side semantics: write_ptr). */
    if (offset == LAGFX_REG_INTR_CLEAR) {
        /* TODO(R4): resolve read-vs-write overloading of 0x1008. */
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

uint32_t lagfx_protocol_last_doorbell_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_doorbell_stamp : 0u;
}
