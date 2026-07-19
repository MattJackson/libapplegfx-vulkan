/*
 * libapplegfx-vulkan — Doorbell routing (BAR0 write/read entry point)
 * src/doorbell.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Listens to BAR offset writes and routes them to appropriate door
 * dispatchers. This module owns the SINGLE entry point for all MMIO
 * doorbell traffic — both the actual ring/channel doorbells AND the
 * surrounding state-register writes (STATUS_CONTROL, ring geometry,
 * shared-page PFN, etc.). Any handling outside doorbell_handle_write /
 * doorbell_handle_read is by definition wrong and should be moved in.
 *
 * The architecture is:
 *
 *   QEMU BAR0 write/read  →  lagfx_mmio_write/read (thin shim in device.c)
 *                         →  doorbell_handle_write / doorbell_handle_read
 *                            ├── state-only registers: update protocol state
 *                            │   + register shadow
 *                            └── actual doorbells (0x1008, 0x1020):
 *                                doorbell_dispatch → registry → dispatcher
 *
 * Anything unmapped is logged as an unhandled warning so the gap is
 * visible in /tmp/lagfx.log rather than silently ignored.
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

/* === Unified MMIO entry points ====================================
 *
 * The single inbound choke point for every BAR0 write/read above the
 * MSI-X region. lagfx_mmio_write/read in device.c are thin shims that
 * forward straight to these. Any new register or doorbell goes here,
 * not in device.c.
 *
 * `protocol_state` is the (void *) handle from device->protocol_state
 * (really a lagfx_protocol_t *); doorbell.c casts internally.
 */
void     doorbell_handle_write(void *protocol_state, uint64_t offset, uint32_t value);
uint32_t doorbell_handle_read (void *protocol_state, uint64_t offset);

/* One-shot init for the register shadow / decoder defaults. Called
 * from lagfx_device_new after lagfx_protocol_new. Owns any startup
 * register values (e.g. STATUS_CONTROL=1) by stamping into the
 * passed protocol_state's reg[] array; doorbell.c is the only writer
 * to that array, but state lives on the protocol struct so multiple
 * devices don't share one shadow. */
void doorbell_init(void *protocol_state);

/* Dispatch: given a BAR offset (must be a registered doorbell offset)
 * and value, dispatch to the matching dispatcher. Called internally
 * by doorbell_handle_write for 0x1008 and 0x1020; left exported for
 * test fixtures that want to drive the dispatcher directly without
 * going through MMIO. */
void doorbell_dispatch(void *protocol_state, uint64_t bar_offset, uint32_t data);

#endif /* LAGFX_DOORBELL_H */
