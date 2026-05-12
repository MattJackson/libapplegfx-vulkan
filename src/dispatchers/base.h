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

#endif /* LAGFX_DISPATCHER_BASE_H */
