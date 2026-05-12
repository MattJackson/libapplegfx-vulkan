/*
 * libapplegfx-vulkan — channel dispatcher base class (OOP-style in C)
 * src/dispatchers/base.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Base class for all channel dispatchers. Each dispatcher subclass
 * embeds this as its first member, enabling polymorphic dispatch.
 */

#ifndef LAGFX_DISPATCHER_BASE_H
#define LAGFX_DISPATCHER_BASE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations - avoid circular includes */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/* === Base Class (vtable interface) ===================================== */

/**
 * Base dispatcher class — dumb routing layer.
 * Subclasses embed this as their first member for polymorphism.
 * Dispatchers just call existing ops_* logic, no duplicate code here.
 */
typedef struct {
    const char *name;           /* Human-readable: "ComputeDispatcher", etc. */
    uint8_t channel_id_min;     /* Inclusive min channel ID handled */
    uint8_t channel_id_max;     /* Inclusive max channel ID handled */
} lagfx_dispatcher_base_t;

/* === Inline helpers ===================================================== */

/** Polymorphic reset wrapper (no-op for now) */
static inline void lagfx_dispatcher_reset(lagfx_dispatcher_base_t *d) {
    (void)d;
}

/**
 * Polymorphic ring dispatch — call through function pointer stored in protocol.
 * The doorbell handler sets p->current_dispatcher_func before calling.
 */
typedef void (*lagfx_ring_dispatch_fn)(void *disp, struct lagfx_protocol *p,
                                       uint64_t descr_gpa, uint8_t ch_id);

static inline void lagfx_dispatcher_ring_dispatch(lagfx_dispatcher_base_t *d,
                                                  struct lagfx_protocol *p,
                                                  uint64_t descr_gpa,
                                                  uint8_t ch_id) {
    (void)d; (void)p; (void)descr_gpa; (void)ch_id;
}

#endif /* LAGFX_DISPATCHER_BASE_H */
