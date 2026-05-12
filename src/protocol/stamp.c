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
    
    /* TODO: read current value from guest memory at ring_base_gpa + slot*4
     *       write max(target, cur+1) back (floor of 1) */
    
    LAGFX_LOG("stamp_cell[%u] := %u", slot, target);
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

    LAGFX_LOG("complete_stamp[slot=%u]: stamp=0x%08x mask=0x%x",
              slot, stamp, p->pending_stamps_bitmask);
}

/* Complete stamp for root channel (slot 0) */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    lagfx_protocol_complete_stamp_slot(p, SLOT_ROOT_CHANNEL, stamp);
}
