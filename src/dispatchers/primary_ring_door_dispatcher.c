/*
 * libapplegfx-vulkan — Primary ring dispatcher (BAR0+0x1008)
 * src/dispatchers/primary_ring_door_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "primary_ring_door_dispatcher.h"
#include "channel_0_dispatcher.h"
#include "../common/log.h"
#include "protocol/state.h"

void primary_ring_door_dispatcher_dispatch(void *protocol_state, uint32_t data) {
    lagfx_protocol_t *p = (lagfx_protocol_t *)protocol_state;
    
    if (!lagfx_protocol_is_valid(p)) {
        LAGFX_WARN("primary_ring: invalid protocol state");
        return;
    }

    /* Data is the write pointer value from BAR0+0x1008.
     * This triggers a drain of the root channel ring buffer. */
    
    LAGFX_LOG("primary_ring_door: write_ptr=0x%08x", data);

    /* Update protocol state with new write pointer */
    p->write_ptr = data;

    /* Set current channel for context */
    p->current_chan_id = SLOT_ROOT_CHANNEL;

    /* Drain the ring buffer - parse commands and dispatch to handlers */
    size_t cmds_processed = channel_0_dispatcher_drain(p);

    LAGFX_LOG("primary_ring_door: processed %zu commands", cmds_processed);
}
