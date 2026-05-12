/*
 * libapplegfx-vulkan — Stamp advancement infrastructure
 * src/protocol/stamp.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "state.h"
#include "../common/log.h"

/* Monotonic stamp-cell advance — never regresses */
void lagfx_advance_stamp_cell(lagfx_protocol_t *p, uint32_t slot, uint32_t target) {
    if (!lagfx_protocol_is_valid(p)) return;
    
    /* Read current value from guest memory at ring_base_gpa + slot*4 */
    uint64_t cell_addr = p->ring_base_gpa + (slot * 4);
    uint32_t cur = 0u;
    
    if (p->dev && p->dev->desc.shell.read_memory) {
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque, cell_addr, 4, &cur)) {
            LAGFX_LOG("stamp_cell[%u]: read failed at 0x%llx, using cur=0", slot, (long long)cell_addr);
        }
    } else if (p->dev && !p->dev->desc.shell.read_memory) {
        /* Shell has no read_memory callback — tests can inspect cell value directly */
        LAGFX_LOG("stamp_cell[%u]: no read_memory callback, using cur=0", slot);
    }
    
    /* Write max(target, cur+1), floor of 1 */
    uint32_t next = target > cur ? target : cur + 1;
    if (next < 1) next = 1;
    
    if (p->dev && p->dev->desc.shell.write_memory) {
        if (!p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque, cell_addr, 4, &next)) {
            LAGFX_LOG("stamp_cell[%u]: write failed at 0x%llx", slot, (long long)cell_addr);
        } else {
            LAGFX_LOG("stamp_cell[%u]: %u -> %u", slot, cur, next);
        }
    } else if (!p->dev || !p->dev->desc.shell.write_memory) {
        /* Shell has no write_memory callback — tests can inspect cell value directly */
        LAGFX_LOG("stamp_cell[%u]: no write_memory callback, would write %u", slot, next);
    }
}

/* Complete stamp for a specific slot */
void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p, uint32_t slot, uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) return;

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    /* Advance the stamp cell monotonically */
    lagfx_advance_stamp_cell(p, slot, stamp);

    /* Set pending bitmask bit for this slot */
    p->pending_stamps_bitmask |= (1u << slot);

    /* Raise interrupt if shell callback is available */
    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
    }

    LAGFX_LOG("complete_stamp[slot=%u]: stamp=0x%08x mask=0x%x",
              slot, stamp, p->pending_stamps_bitmask);
}

/* Complete stamp for root channel (slot 0) */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    lagfx_protocol_complete_stamp_slot(p, SLOT_ROOT_CHANNEL, stamp);
}
