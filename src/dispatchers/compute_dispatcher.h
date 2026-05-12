/*
 * libapplegfx-vulkan — Compute/Render channel dispatcher (Channels 1-4)
 * src/dispatchers/compute_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dumb routing layer for compute/render channels (1-4). Just calls existing
 * ops_*.c functions — no duplicate logic here. Opcodes are channel-specific,
 * so we log which channel each opcode came from for debugging.
 */

#ifndef LAGFX_COMPUTE_DISPATCHER_H
#define LAGFX_COMPUTE_DISPATCHER_H

#include "base.h"

/* Forward declarations */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/**
 * Compute/Render dispatcher subclass for channels 1-4.
 * Embeds base class as first member for polymorphism.
 * Dumb routing layer — no state, just calls existing ops_*.c logic.
 */
typedef struct {
    lagfx_dispatcher_base_t base;   /* Base class (first!) */
} lagfx_compute_dispatcher_t;

/** Constructor pattern */
lagfx_compute_dispatcher_t *compute_dispatcher_new(void);

/** Ring dispatch — delegates to existing protocol.c ring drain logic */
void compute_dispatcher_ring_dispatch(lagfx_compute_dispatcher_t *d,
                                     struct lagfx_protocol *p,
                                     uint64_t descr_gpa,
                                     uint8_t ch_id);

#endif /* LAGFX_COMPUTE_DISPATCHER_H */
