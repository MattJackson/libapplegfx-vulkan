/*
 * libapplegfx-vulkan — channel dispatcher classes (OOP-style in C)
 * src/protocol/dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Per-channel dispatchers encapsulate their own opcode namespace and handler
 * logic. Each channel (or group of channels) gets its own dispatcher subclass:
 *   - Channel 1-4: Compute/Render dispatcher (CmdExecIndirect2, CmdMapMemory, etc.)
 *   - Channel 5+: Display vchan dispatcher (CmdDisplayTransaction3, etc.)
 *
 * OOP pattern in C:
 *   1. Base class struct with function pointers (vtable)
 *   2. Subclass embeds base as first member for polymorphism
 *   3. Constructor returns pointer to initialized subclass
 *   4. Polymorphic dispatch via vtable: dispatcher->dispatch(dispatcher, data)
 */

#ifndef LAGFX_DISPATCHER_H
#define LAGFX_DISPATCHER_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct lagfx_protocol lagfx_protocol_t;

/* Base class — defines the interface all dispatchers must implement */
typedef struct lagfx_dispatcher {
    const char *name;           /* Human-readable name: "Channel1", "DisplayVchan" */
    uint8_t channel_id_min;     /* Inclusive min channel ID this handles */
    uint8_t channel_id_max;     /* Inclusive max channel ID this handles */
    
    /* Virtual methods (vtable) */
    int (*dispatch)(struct lagfx_dispatcher *self, 
                   const uint8_t *payload, size_t len);
    void (*reset)(struct lagfx_dispatcher *self);
} lagfx_dispatcher_t;

/* === Subclass: Channel 1-4 Compute/Render Dispatcher ================== */

typedef struct {
    lagfx_dispatcher_t base;    /* Embed base class first for polymorphism */
    
    /* State unique to compute/render channels (ch 1-4) */
    uint32_t current_task_id;   /* Current CmdExecIndirect2 task context */
} lagfx_compute_dispatcher_t;

/* Constructor pattern */
lagfx_compute_dispatcher_t *compute_dispatcher_new(void);

/* === Subclass: Display Vchan Dispatcher (Channel 5+) ================== */

typedef struct {
    lagfx_dispatcher_t base;    /* Embed base class first for polymorphism */
    
    /* State unique to display channels (ch 5+) */
    uint32_t shared_state_pfn;
    uint32_t child_fifo_count;
} lagfx_display_vchan_dispatcher_t;

lagfx_display_vchan_dispatcher_t *display_vchan_dispatcher_new(void);

/* === Subclass: Immediate Channel Dispatcher (Channel 2) =============== */

typedef struct {
    lagfx_dispatcher_t base;    /* Embed base class first for polymorphism */
    
    /* State unique to immediate channel (ch 2) */
    uint32_t va_mapping_count;
} lagfx_immediate_dispatcher_t;

lagfx_immediate_dispatcher_t *immediate_dispatcher_new(void);

/* === Dispatcher Registry =================================================
 * Global registry maps channel IDs to dispatcher instances. Doorbell handler
 * looks up by channel ID and calls polymorphic dispatch.
 */

typedef struct {
    uint8_t ch_min, ch_max;
    lagfx_dispatcher_t *dispatcher;
} lagfx_channel_registry_entry_t;

/* Initialize all dispatcher subclasses and populate registry */
void lagfx_dispatcher_init(void);

/* Look up dispatcher for a given channel ID */
lagfx_dispatcher_t *lagfx_dispatcher_lookup(uint8_t chan_id);

/* Polymorphic dispatch wrapper — caller doesn't know which subclass */
static inline int lagfx_dispatcher_dispatch(lagfx_dispatcher_t *d,
                                           const uint8_t *payload, 
                                           size_t len) {
    if (!d || !d->dispatch) {
        return -1;
    }
    return d->dispatch(d, payload, len);
}

/* Polymorphic reset wrapper */
static inline void lagfx_dispatcher_reset(lagfx_dispatcher_t *d) {
    if (d && d->reset) {
        d->reset(d);
    }
}

#endif /* LAGFX_DISPATCHER_H */
