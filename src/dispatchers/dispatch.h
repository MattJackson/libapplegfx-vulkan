/*
 * libapplegfx-vulkan — protocol dispatch interface (tests)
 * src/dispatchers/dispatch.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal dispatch interface for tests that need to directly invoke
 * opcode handlers without going through the full doorbell path.
 */

#ifndef LAGFX_DISPATCH_H
#define LAGFX_DISPATCH_H

#include "base.h"

/* Forward decl */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/**
 * Dispatch a single command on protocol's current ring dispatcher.
 * This is the test-side replacement for removed protocol_dispatch_one().
 * Uses p->current_dispatcher_func which should be set by doorbell handler.
 */
static inline void
lagfx_protocol_dispatch_one(lagfx_protocol_t *p, const uint8_t *cmd, size_t len) {
    if (!lagfx_protocol_is_valid(p) || !cmd || len < 12) {
        return;
    }
    
    /* Get dispatcher for current channel */
    lagfx_dispatcher_base_t *disp = p->current_dispatcher;
    if (!disp || !p->ring_dispatch_fn) {
        LAGFX_WARN("dispatch_one: no dispatcher or dispatch fn");
        return;
    }
    
    /* Call through polymorphic dispatch function pointer */
    p->ring_dispatch_fn(disp, p, 0, p->current_chan_id);
}

#endif /* LAGFX_DISPATCH_H */
