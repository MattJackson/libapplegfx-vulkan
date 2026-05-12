/*
 * libapplegfx-vulkan — Doorbell routing (BAR0 write listener)
 * src/doorbell.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Listens to BAR offset writes and routes them to appropriate door dispatchers.
 * This is the SINGLE entry point for all MMIO doorbell traffic.
 */

#ifndef LAGFX_DOORBELL_H
#define LAGFX_DOORBELL_H

#include <stdint.h>
#include <stddef.h>

/* === Door Types ==================================================
 * Each door type handles a specific BAR offset pattern and routes
 * messages to the appropriate dispatcher based on message content.
 */
typedef enum {
    DOOR_PRIMARY_RING     = 0,   // BAR0+0x1008 - primary ring write pointer (root ch)
    DOOR_PER_CHANNEL_0    = 1,   // BAR0+0x1004 - per-channel doorbell for channel 0
    DOOR_PER_CHANNEL_1    = 2,   // BAR0+0x1008 - per-channel doorbell for channel 1 (compute)
    DOOR_PER_CHANNEL_2    = 3,   // BAR0+0x100c - per-channel doorbell for channel 2 (compute)
    DOOR_PER_CHANNEL_3    = 4,   // BAR0+0x1010 - per-channel doorbell for channel 3 (compute)
    DOOR_CHANNEL          = 5,   // BAR0+0x1020 - channel ID selector (routes to ch 0, 1-4, 5+)
} doorbell_door_id_t;

/* === Door Descriptor ==============================================
 * Registry entry for each door we listen to.
 */
typedef struct {
    uint64_t               bar_offset;      // BAR offset this door listens to
    doorbell_door_id_t     id;              // Door type enum value
    void (*dispatch_fn)(void *protocol_state, uint32_t data);  // Route to dispatcher with protocol state
} doorbell_door_descriptor_t;

/* === Registry =====================================================
 * Registry of all doors we listen to. Defined in doorbell.c.
 */
extern const doorbell_door_descriptor_t g_doorbell_doors[];
extern const size_t g_doorbell_door_count;

/* Lookup: given BAR offset, find which door handles it */
const doorbell_door_descriptor_t* doorbell_lookup_by_offset(uint64_t offset);

/* Dispatch: call the appropriate dispatcher for this BAR write (with protocol state) */
void doorbell_dispatch(void *protocol_state, doorbell_door_id_t id, uint32_t data);

#endif /* LAGFX_DOORBELL_H */
