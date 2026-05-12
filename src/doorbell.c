/*
 * libapplegfx-vulkan — Doorbell routing (BAR0 write listener)
 * src/doorbell.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "doorbell.h"
#include "dispatchers/channel_door_dispatcher.h"
#include "dispatchers/primary_ring_door_dispatcher.h"
#include "../common/log.h"

/* === Door Registry ================================================
 * Registry of all doors we listen to. Each entry maps a BAR offset
 * to the dispatcher that handles it.
 */
const doorbell_door_descriptor_t g_doorbell_doors[] = {
    /* Root channel write pointer (BAR0+0x1008) - primary ring FIFO doorbell */
    { .bar_offset = 0x1008u, .id = DOOR_PRIMARY_RING, .dispatch_fn = (void(*)(void*, uint32_t))primary_ring_door_dispatcher_dispatch },
    
    /* Channel ID selector (BAR0+0x1020) - routes to ch 0, compute (1-4), display (5+) */
    { .bar_offset = 0x1020u, .id = DOOR_CHANNEL, .dispatch_fn = (void(*)(void*, uint32_t))channel_door_dispatcher_dispatch },
};

const size_t g_doorbell_door_count = sizeof(g_doorbell_doors) / sizeof(doorbell_door_descriptor_t);

/* === Registry Lookup ============================================== */

const doorbell_door_descriptor_t* doorbell_lookup_by_offset(uint64_t offset) {
    for (size_t i = 0; i < g_doorbell_door_count; i++) {
        if (g_doorbell_doors[i].bar_offset == offset) {
            return &g_doorbell_doors[i];
        }
    }
    return NULL;
}

/* === Dispatch Entry Point ========================================= */

void doorbell_dispatch(void *protocol_state, doorbell_door_id_t id, uint32_t data) {
    if (id >= DOOR_CHANNEL + 1) {
        LAGFX_WARN("doorbell_dispatch: unknown door type %u", id);
        return;
    }

    const doorbell_door_descriptor_t* door = &g_doorbell_doors[id];
    if (!door || !door->dispatch_fn) {
        LAGFX_WARN("doorbell_dispatch: no handler for door type %u", id);
        return;
    }

    /* Call the dispatcher's routing function with protocol state */
    door->dispatch_fn(protocol_state, data);
}
