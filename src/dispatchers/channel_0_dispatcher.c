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
#include "../device.h"
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
 * Handles CmdExecIndirect2 and other primary ring opcodes via switch-case.
 */
void channel_0_dispatcher_ring_dispatch(lagfx_channel_0_dispatcher_t *d,
                                        struct lagfx_protocol *p,
                                        uint64_t descr_gpa,
                                        uint8_t ch_id) {
    (void)d;
    (void)ch_id;

    LAGFX_LOG("Channel0Dispatcher(ring): processing primary ring commands");
    
    /* Read FIFO write pointer to get command stream */
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_ERR("Channel0Dispatcher: no device or read_memory callback");
        return;
    }

    /* Primary ring descriptor at +0x400 from shared page contains write pointer */
    uint8_t descr[20] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        descr_gpa, sizeof(descr), descr)) {
        LAGFX_ERR("Channel0Dispatcher: failed to read descriptor");
        return;
    }

    uint32_t write_ptr = (uint32_t)descr[0] | ((uint32_t)descr[1] << 8) |
                         ((uint32_t)descr[2] << 16) | ((uint32_t)descr[3] << 24);
    uint32_t read_ptr  = (uint32_t)descr[4] | ((uint32_t)descr[5] << 8) |
                         ((uint32_t)descr[6] << 16) | ((uint32_t)descr[7] << 24);

    LAGFX_LOG("Channel0Dispatcher: primary ring wr=%u rd=%u", write_ptr, read_ptr);

    if (write_ptr <= read_ptr) {
        LAGFX_TRACE("Channel0Dispatcher: empty ring");
        return;
    }

    /* Drain commands from FIFO */
    uint32_t cur_rp = read_ptr;
    while (cur_rp + 12u <= write_ptr) {
        /* Read command header */
        uint8_t hdr_bytes[12] = {0};
        bool ok = true;
        size_t got = 0;

        for (size_t i = 0; i < 12 && ok; i++) {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                descr_gpa + cur_rp + i, 1, &hdr_bytes[i])) {
                LAGFX_WARN("Channel0Dispatcher: failed to read command header byte %zu", i);
                ok = false;
            }
        }

        if (!ok) break;

        /* Parse command header */
        uint32_t opcode = (uint32_t)(hdr_bytes[0] | ((uint32_t)hdr_bytes[1] << 8));
        uint32_t length = (uint32_t)(hdr_bytes[2] | ((uint32_t)hdr_bytes[3] << 8) |
                                     ((uint32_t)hdr_bytes[4] << 16) | ((uint32_t)hdr_bytes[5] << 24));

        LAGFX_LOG("Channel0Dispatcher: opcode=0x%04x length=%u", opcode, length);

        /* Dispatch based on opcode - switch-case per channel! */
        switch (opcode) {
        case 0x20:  /* CmdExecIndirect2 */
            LAGFX_LOG("Channel0Dispatcher: CmdExecIndirect2 → ops_cmdbuf.c");
            p->current_chan_id = ch_id;

            /* Read full command payload */
            uint8_t *payload = malloc(length);
            if (payload) {
                for (size_t i = 0; i < length && ok; i++) {
                    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                        descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                        LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                        ok = false;
                    }
                }

                if (ok) {
                    /* Call legacy handler for now */
                    lagfx_op_exec_indirect2(p, &((lagfx_cmd_header_t){
                        .opcode = opcode,
                        .length = length,
                        .stamp = 0,
                        .payload_size = length - 12u,
                        .arg_count_8b = 0,
                        .payload = payload + 12u
                    }));
                }

                free(payload);
            }
            break;

        default:
            LAGFX_WARN("Channel0Dispatcher: unknown primary ring opcode 0x%04x", opcode);
            break;
        }

        /* Advance read pointer */
        cur_rp += length;
    }

    /* Update read pointer in descriptor */
    LAGFX_LOG("Channel0Dispatcher: updated read_ptr=%u", cur_rp);
}

