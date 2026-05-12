/*
 * libapplegfx-vulkan — Catch-all/Unknown channel dispatcher implementation
 * src/dispatchers/unknown_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Always registered for any channel ID not explicitly handled elsewhere.
 * Logs all commands for debugging — "I don't know this channel, but I'll watch it."
 */

#ifndef LAGFX_UNKNOWN_DISPATCHER_H
#define LAGFX_UNKNOWN_DISPATCHER_H

#include "base.h"

/* Forward declarations */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/**
 * Unknown/Catch-all dispatcher subclass.
 * Embeds base class as first member for polymorphism.
 * Logs all commands from unregistered channels for debugging.
 */
typedef struct {
    lagfx_dispatcher_base_t base;   /* Base class (first!) */
} lagfx_unknown_dispatcher_t;

/** Constructor pattern */
lagfx_unknown_dispatcher_t *unknown_dispatcher_new(void);

/** Ring dispatch — logs everything from unknown channels */
void unknown_dispatcher_ring_dispatch(lagfx_unknown_dispatcher_t *d,
                                     struct lagfx_protocol *p,
                                     uint64_t descr_gpa,
                                     uint8_t ch_id);

#endif /* LAGFX_UNKNOWN_DISPATCHER_H */
