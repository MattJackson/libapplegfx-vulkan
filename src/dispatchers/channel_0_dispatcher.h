/*
 * libapplegfx-vulkan — Primary ring dispatcher (Channel 0)
 * src/dispatchers/channel_0_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dispatcher for primary ring (channel 0). Handles CmdExecIndirect2 and routes
 * inner opcodes to render/blit/compute decoders via ops_cmdbuf.c logic.
 */

#ifndef LAGFX_CHANNEL_0_DISPATCHER_H
#define LAGFX_CHANNEL_0_DISPATCHER_H

#include "base.h"

/* Forward declarations - avoid circular includes */
struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

/**
 * Primary ring dispatcher for channel 0.
 * Embeds base class as first member for polymorphism.
 * Handles CmdExecIndirect2 and delegates to ops_cmdbuf.c logic.
 */
typedef struct {
    lagfx_dispatcher_base_t base;   /* Base class (first!) */
} lagfx_channel_0_dispatcher_t;

/** Constructor pattern */
lagfx_channel_0_dispatcher_t *channel_0_dispatcher_new(void);

/** Ring dispatch — delegates to ops_cmdbuf.c CmdExecIndirect2 handler */
void channel_0_dispatcher_ring_dispatch(lagfx_channel_0_dispatcher_t *d,
                                        struct lagfx_protocol *p,
                                        uint64_t descr_gpa,
                                        uint8_t ch_id);

#endif /* LAGFX_CHANNEL_0_DISPATCHER_H */
