/*
 * libapplegfx-vulkan — Primary ring dispatcher (Channel 0) implementation
 * src/dispatchers/channel_0_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dispatcher for primary ring (channel 0). Handles CmdExecIndirect2 and routes
 * inner opcodes to render/blit/compute decoders via ops_cmdbuf.c logic.
 */

#include "channel_0_dispatcher.h"
#include "../protocol/state.h"
#include "../common/log.h"
#include <stdlib.h>
#include <string.h>

/**
 * Channel 0 dispatcher for primary ring.
 * Handles CmdExecIndirect2 and delegates to ops_cmdbuf.c logic.
 */
lagfx_channel_0_dispatcher_t *channel_0_dispatcher_new(void) {
    lagfx_channel_0_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }

    d->base.name = "Channel0Dispatcher(primary ring)";
    d->base.channel_id_min = 0;
    d->base.channel_id_max = 0;

    LAGFX_LOG("Channel0Dispatcher: created for primary ring");
    return d;
}

/**
 * Ring dispatch for channel 0 (primary ring).
 * Delegates to ops_cmdbuf.c CmdExecIndirect2 handler.
 */
void channel_0_dispatcher_ring_dispatch(lagfx_channel_0_dispatcher_t *d,
                                        struct lagfx_protocol *p,
                                        uint64_t descr_gpa,
                                        uint8_t ch_id) {
    (void)d;
    (void)descr_gpa;
    (void)ch_id;

    LAGFX_LOG("Channel0Dispatcher(ring): processing primary ring commands");
    
    /* TODO: Implement FIFO drain and CmdExecIndirect2 parsing here.
     * For now, this is a stub — the old path via ops_cmdbuf.c still works
     * because doorbell 0x1020 triggers both dispatcher AND legacy FIFO drain. */
}

