/*
 * libapplegfx-vulkan — Display VChan channel dispatcher (Channels 5+)
 * src/dispatchers/display_vchan_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dumb routing layer for display virtual channels (5+). Just calls existing
 * ops_*.c functions — no duplicate logic here. Opcodes are channel-specific,
 * so we log which channel each opcode came from for debugging.
 */

#ifndef LAGFX_DISPLAY_VCHAN_DISPATCHER_H
#define LAGFX_DISPLAY_VCHAN_DISPATCHER_H

#include "base.h"

/* Forward declarations */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/**
 * Display VChan dispatcher subclass for channels 5+.
 * Embeds base class as first member for polymorphism.
 * Dumb routing layer — no state, just calls existing ops_*.c logic.
 */
typedef struct {
    lagfx_dispatcher_base_t base;   /* Base class (first!) */
} lagfx_display_vchan_dispatcher_t;

/** Constructor pattern */
lagfx_display_vchan_dispatcher_t *display_vchan_dispatcher_new(void);

/** Ring dispatch — delegates to existing protocol.c ring drain logic */
void display_vchan_dispatcher_ring_dispatch(lagfx_display_vchan_dispatcher_t *d,
                                           struct lagfx_protocol *p,
                                           uint64_t descr_gpa,
                                           uint8_t ch_id);

#endif /* LAGFX_DISPLAY_VCHAN_DISPATCHER_H */
