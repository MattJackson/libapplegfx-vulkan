/*
 * libapplegfx-vulkan — Channel 1 dispatcher (Compute/Render)
 * src/dispatchers/channel_1_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dispatcher for channel 1 (compute/render). Handles compute opcodes via switch-case.
 */

#include "channel_1_dispatcher.h"
#include "../device.h"
#include "../protocol/protocol.h"
#include "../protocol/state.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

lagfx_channel_1_dispatcher_t *channel_1_dispatcher_new(void) {
    lagfx_channel_1_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    
    d->base.name = "Channel1Dispatcher(ch1)";
    d->base.channel_id_min = 1;
    d->base.channel_id_max = 1;
    
    LAGFX_LOG("Channel1Dispatcher: created for channel 1");
    return d;
}

void channel_1_dispatcher_ring_dispatch(lagfx_channel_1_dispatcher_t *d,
                                        struct lagfx_protocol *p,
                                        uint64_t descr_gpa,
                                        uint8_t ch_id) {
    (void)d;
    
    LAGFX_LOG("Channel1Dispatcher(ch%u): processing compute commands", ch_id);
    
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_ERR("Channel1Dispatcher: no device or read_memory callback");
        return;
    }
    
    uint8_t descr[20] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        descr_gpa, sizeof(descr), descr)) {
        LAGFX_ERR("Channel1Dispatcher: failed to read descriptor");
        return;
    }
    
    uint32_t write_ptr = (uint32_t)descr[0] | ((uint32_t)descr[1] << 8) |
                         ((uint32_t)descr[2] << 16) | ((uint32_t)descr[3] << 24);
    uint32_t read_ptr  = (uint32_t)descr[4] | ((uint32_t)descr[5] << 8) |
                         ((uint32_t)descr[6] << 16) | ((uint32_t)descr[7] << 24);
    uint32_t ring_pfn  = (uint32_t)descr[16] | ((uint32_t)descr[17] << 8) |
                         ((uint32_t)descr[18] << 16) | ((uint32_t)descr[19] << 24);
    
    LAGFX_LOG("Channel1Dispatcher(ch%u): ring wr=%u rd=%u pfn=0x%x",
              ch_id, write_ptr, read_ptr, ring_pfn);
    
    if (ring_pfn == 0 || write_ptr <= read_ptr) {
        LAGFX_TRACE("Channel1Dispatcher: empty or invalid ring");
        return;
    }
    
    uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
    uint32_t cur_rp = read_ptr;
    unsigned cmd_idx = 0;
    uint32_t last_stamp = 0u;
    
    while (cur_rp + 12u <= write_ptr && cmd_idx < 256) {
        uint8_t hdr_bytes[12] = {0};
        bool ok = true;
        
        for (size_t i = 0; i < 12 && ok; i++) {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                ring_gpa_base + cur_rp + i, 1, &hdr_bytes[i])) {
                LAGFX_WARN("Channel1Dispatcher: failed to read header byte %zu", i);
                ok = false;
            }
        }

        if (!ok) break;

        uint32_t opcode = (uint32_t)(hdr_bytes[0] | ((uint32_t)hdr_bytes[1] << 8));
        uint32_t length = (uint32_t)(hdr_bytes[4] | ((uint32_t)hdr_bytes[5] << 8) |
                                     ((uint32_t)hdr_bytes[6] << 16) | ((uint32_t)hdr_bytes[7] << 24));
        uint32_t stamp = (uint32_t)(hdr_bytes[8] | ((uint32_t)hdr_bytes[9] << 8) |
                                   ((uint32_t)hdr_bytes[10] << 16) | ((uint32_t)hdr_bytes[11] << 24));

        LAGFX_LOG("Channel1Dispatcher(ch%u): opcode=0x%04x length=%u stamp=0x%08x",
                  ch_id, opcode, length, stamp);

        p->current_chan_id = ch_id;
        
        switch (opcode) {
        case 0x37:  /* CmdExecIndirect2/Kext */
            LAGFX_LOG("Channel1Dispatcher(ch%u): CmdExecIndirect2/Kext → ops_cmdbuf.c", ch_id);
            p->extra_stamp_advance = 0;
            
            uint32_t payload_len = length - 12u;
            if (length > sizeof(p->doorbell_bounce_buffer)) {
                LAGFX_ERR("Channel1Dispatcher: cmd_len %u exceeds bounce buffer", length);
                break;
            }

            memcpy(p->doorbell_bounce_buffer, hdr_bytes, 12u);
            for (uint32_t i = 0; i < payload_len && ok; i++) {
                if (!p->dev->desc.shell.read_memory(
                        p->dev->desc.shell.opaque,
                        ring_gpa_base + cur_rp + 12u + i, 1, &p->doorbell_bounce_buffer[12u + i])) {
                    LAGFX_WARN("Channel1Dispatcher: failed to read payload byte %u", i);
                    ok = false;
                }
            }

            if (ok) {
                lagfx_cmd_header_t parsed = {
                    .opcode = opcode,
                    .arg_count_8b = 0,
                    .length = length,
                    .stamp = stamp,
                    .payload_size = payload_len,
                    .payload = p->doorbell_bounce_buffer + 12u,
                };
                
                lagfx_protocol_dispatch_one_no_stamp(p, p->doorbell_bounce_buffer, length, &parsed);
                last_stamp = parsed.stamp + p->extra_stamp_advance;
            } else {
                last_stamp = stamp;
            }
            break;

        case 0x38:  /* CmdDefineHostTask */
            LAGFX_LOG("Channel1Dispatcher(ch%u): CmdDefineHostTask", ch_id);
            p->extra_stamp_advance = 0;
            
            payload_len = length - 12u;
            if (length > sizeof(p->doorbell_bounce_buffer)) {
                LAGFX_ERR("Channel1Dispatcher: cmd_len %u exceeds bounce buffer", length);
                break;
            }

            memcpy(p->doorbell_bounce_buffer, hdr_bytes, 12u);
            for (uint32_t i = 0; i < payload_len && ok; i++) {
                if (!p->dev->desc.shell.read_memory(
                        p->dev->desc.shell.opaque,
                        ring_gpa_base + cur_rp + 12u + i, 1, &p->doorbell_bounce_buffer[12u + i])) {
                    LAGFX_WARN("Channel1Dispatcher: failed to read payload byte %u", i);
                    ok = false;
                }
            }

            if (ok) {
                lagfx_cmd_header_t parsed = {
                    .opcode = opcode,
                    .arg_count_8b = 0,
                    .length = length,
                    .stamp = stamp,
                    .payload_size = payload_len,
                    .payload = p->doorbell_bounce_buffer + 12u,
                };

                lagfx_protocol_dispatch_one_no_stamp(p, p->doorbell_bounce_buffer, length, &parsed);
                last_stamp = parsed.stamp + p->extra_stamp_advance;
            } else {
                last_stamp = stamp;
            }
            break;

        case 0x39:  /* CmdMapMemoryImmediate */
            LAGFX_LOG("Channel1Dispatcher(ch%u): CmdMapMemoryImmediate", ch_id);
            p->extra_stamp_advance = 0;
            
            payload_len = length - 12u;
            if (length > sizeof(p->doorbell_bounce_buffer)) {
                LAGFX_ERR("Channel1Dispatcher: cmd_len %u exceeds bounce buffer", length);
                break;
            }

            memcpy(p->doorbell_bounce_buffer, hdr_bytes, 12u);
            for (uint32_t i = 0; i < payload_len && ok; i++) {
                if (!p->dev->desc.shell.read_memory(
                        p->dev->desc.shell.opaque,
                        ring_gpa_base + cur_rp + 12u + i, 1, &p->doorbell_bounce_buffer[12u + i])) {
                    LAGFX_WARN("Channel1Dispatcher: failed to read payload byte %u", i);
                    ok = false;
                }
            }

            if (ok) {
                lagfx_cmd_header_t parsed = {
                    .opcode = opcode,
                    .arg_count_8b = 0,
                    .length = length,
                    .stamp = stamp,
                    .payload_size = payload_len,
                    .payload = p->doorbell_bounce_buffer + 12u,
                };

                lagfx_protocol_dispatch_one_no_stamp(p, p->doorbell_bounce_buffer, length, &parsed);
                last_stamp = parsed.stamp + p->extra_stamp_advance;
            } else {
                last_stamp = stamp;
            }
            break;

        default:
            LAGFX_WARN("Channel1Dispatcher(ch%u): unknown opcode 0x%04x", ch_id, opcode);
            last_stamp = stamp;
            break;
        }

        cur_rp += length;
        cmd_idx++;
    }
    
    uint32_t new_rp = cur_rp;
    if (p->dev->desc.shell.write_memory) {
        p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque,
                                        descr_gpa + 4u, sizeof(new_rp), &new_rp);
    }
    
    LAGFX_LOG("Channel1Dispatcher(ch%u): drained %u cmd(s)", ch_id, cmd_idx);
    
    if (last_stamp > 0u) {
        lagfx_advance_stamp_cell(p, ch_id, last_stamp);
        p->pending_stamps_bitmask |= (1u << ch_id);
        
        if (p->dev && p->dev->desc.shell.raise_interrupt) {
            p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
            p->interrupts_raised += 1;
        }
    }
}
