/*
 * libapplegfx-vulkan — Display VChan channel dispatcher (Channels 5+)
 * src/dispatchers/display_vchan_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dumb routing layer for display virtual channels (5+). Just calls existing
 * ops_*.c functions — no duplicate logic here. Opcodes are channel-specific,
 * so we log which channel each opcode came from for debugging.
 */

#include "display_vchan_dispatcher.h"
#include "../device.h"
#include "../protocol/protocol.h"
#include "../protocol/state.h"
#include "../protocol/opcodes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* Forward declarations for display vchan opcode handlers */
lagfx_handler_status_t lagfx_op_vchan_setup_shared_state(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_vchan_display_submit(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_define_child_fifo(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_vchan_present(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_vchan_present_gamma(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* Stamp cell advancement — defined in protocol.c, exported via state.h. */
void lagfx_advance_stamp_cell(lagfx_protocol_t *p, uint32_t slot, uint32_t target);

/* === Constructor ======================================================== */

lagfx_display_vchan_dispatcher_t *display_vchan_dispatcher_new(void) {
    lagfx_display_vchan_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    
    d->base.name = "DisplayVchanDispatcher(ch5+)";
    d->base.channel_id_min = 5;
    d->base.channel_id_max = LAGFX_MAX_CHANNELS - 1;
    
    return d;
}

/* === Ring Dispatch — log per-channel, delegate ========================== */

void display_vchan_dispatcher_ring_dispatch(lagfx_display_vchan_dispatcher_t *d,
                                           struct lagfx_protocol *p,
                                           uint64_t descr_gpa,
                                           uint8_t ch_id) {
    (void)d;  /* No state needed for routing */
    
    LAGFX_LOG("DisplayVchanDispatcher(ch%u): processing display vchan commands", ch_id);
    
    /* Read descriptor to get ring geometry */
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_ERR("DisplayVchanDispatcher: no device or read_memory callback");
        return;
    }
    
    uint8_t descr[20] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        descr_gpa, sizeof(descr), descr)) {
        LAGFX_ERR("DisplayVchanDispatcher: failed to read descriptor");
        return;
    }
    
    /* Descriptor layout: { write_ptr, read_ptr, mid, chan_id, ring_pfn } */
    uint32_t write_ptr = (uint32_t)descr[0] | ((uint32_t)descr[1] << 8) |
                         ((uint32_t)descr[2] << 16) | ((uint32_t)descr[3] << 24);
    uint32_t read_ptr  = (uint32_t)descr[4] | ((uint32_t)descr[5] << 8) |
                         ((uint32_t)descr[6] << 16) | ((uint32_t)descr[7] << 24);
    uint32_t ring_pfn  = (uint32_t)descr[16] | ((uint32_t)descr[17] << 8) |
                         ((uint32_t)descr[18] << 16) | ((uint32_t)descr[19] << 24);
    
    LAGFX_LOG("DisplayVchanDispatcher(ch%u): ring wr=%u rd=%u pfn=0x%x",
              ch_id, write_ptr, read_ptr, ring_pfn);
    
    if (ring_pfn == 0 || write_ptr <= read_ptr) {
        LAGFX_TRACE("DisplayVchanDispatcher: empty or invalid ring");
        return;
    }
    
    /* Drain commands from the ring */
    uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
    uint32_t child_ring_size = 0x1000u;  /* Display channels have 4 KiB rings */
    
    uint32_t cur_rp = read_ptr;
    unsigned cmd_idx = 0;
    uint32_t last_stamp = 0u;
    bool saw_non_setup = false;
    
    while (cur_rp + 12u <= write_ptr && cmd_idx < 256) {
        /* Read command header */
        uint8_t hdr_bytes[12] = {0};
        bool ok = true;
        uint32_t off = cur_rp;
        size_t got = 0;
        
        while (got < 12u && ok) {
            uint32_t mod_off = off % child_ring_size;
            uint32_t page_idx = mod_off >> 12;
            uint32_t page_off = mod_off & 0xfffu;
            uint32_t can = 0x1000u - page_off;
            uint32_t want = (uint32_t)(12u - got);
            uint32_t take = (want < can) ? want : can;
            uint32_t pte_pfn = 0u;
            
            if (!p->dev->desc.shell.read_memory(
                    p->dev->desc.shell.opaque,
                    ring_gpa_base + (uint64_t)page_idx * 4u,
                    sizeof(pte_pfn), &pte_pfn) || pte_pfn == 0u) {
                ok = false; break;
            }
            
            if (!p->dev->desc.shell.read_memory(
                    p->dev->desc.shell.opaque,
                    ((uint64_t)pte_pfn << 12) + page_off,
                    take, hdr_bytes + got)) {
                ok = false; break;
            }
            
            off += take;
            got += take;
        }
        
        if (!ok) {
            LAGFX_TRACE("DisplayVchanDispatcher: failed to read header at rp=%u", cur_rp);
            break;
        }
        
        uint16_t opcode = (uint16_t)(hdr_bytes[0] | (hdr_bytes[1] << 8));
        uint32_t cmd_len = (uint32_t)hdr_bytes[4] | ((uint32_t)hdr_bytes[5] << 8) |
                          ((uint32_t)hdr_bytes[6] << 16) | ((uint32_t)hdr_bytes[7] << 24);
        uint32_t stamp = (uint32_t)hdr_bytes[8] | ((uint32_t)hdr_bytes[9] << 8) |
                        ((uint32_t)hdr_bytes[10] << 16) | ((uint32_t)hdr_bytes[11] << 24);
        
        LAGFX_TRACE("DisplayVchanDispatcher(ch%u) cmd[%u]: opcode=0x%04x len=%u at rp=%u",
                    ch_id, cmd_idx, opcode, cmd_len, cur_rp);
        
        /* Track per-channel opcode namespace */
        if (ch_id >= 1 && ch_id <= 16) {
            int idx = ch_id - 1;
            p->ch_opcode_seen[idx][opcode] |= (1u << (opcode & 7));
            if (!p->ch_namespace_logged[idx]) {
                p->ch_namespace_logged[idx] = true;
                LAGFX_LOG("DisplayVchanDispatcher(ch%u): namespace tracking enabled", ch_id);
            }
        }
        
        /* Read full command payload */
        uint32_t payload_len = cmd_len - 12u;
        if (cmd_len > sizeof(p->doorbell_bounce_buffer)) {
            LAGFX_ERR("DisplayVchanDispatcher: cmd_len %u exceeds bounce buffer", cmd_len);
            break;
        }
        
        memcpy(p->doorbell_bounce_buffer, hdr_bytes, 12u);
        if (payload_len > 0u) {
            uint8_t *payload_buf = p->doorbell_bounce_buffer + 12u;
            bool ok = true;
            uint32_t off = cur_rp + 12u;
            size_t got = 0;
            
            while (got < payload_len && ok) {
                uint32_t mod_off = off % child_ring_size;
                uint32_t page_idx = mod_off >> 12;
                uint32_t page_off = mod_off & 0xfffu;
                uint32_t can = 0x1000u - page_off;
                uint32_t want = (uint32_t)(payload_len - got);
                uint32_t take = (want < can) ? want : can;
                uint32_t pte_pfn = 0u;
                
                if (!p->dev->desc.shell.read_memory(
                        p->dev->desc.shell.opaque,
                        ring_gpa_base + (uint64_t)page_idx * 4u,
                        sizeof(pte_pfn), &pte_pfn) || pte_pfn == 0u) {
                    ok = false; break;
                }
                
                if (!p->dev->desc.shell.read_memory(
                        p->dev->desc.shell.opaque,
                        ((uint64_t)pte_pfn << 12) + page_off,
                        take, payload_buf + got)) {
                    ok = false; break;
                }
                
                off += take;
                got += take;
            }
            
            if (!ok) {
                LAGFX_TRACE("DisplayVchanDispatcher: failed to read payload at rp=%u", cur_rp);
                break;
            }
        }
        
        /* Dispatch display vchan compact opcode */
        lagfx_cmd_header_t parsed = (lagfx_cmd_header_t){
            .opcode       = opcode,
            .arg_count_8b = 0u,
            .length       = cmd_len,
            .stamp        = stamp,
            .payload_size = (uint16_t)payload_len,
            .payload      = (payload_len > 0u) ? (p->doorbell_bounce_buffer + 12u) : NULL,
        };
        
        p->current_chan_id = ch_id;
        
        switch (opcode) {
        case 0x01u:
            lagfx_op_vchan_setup_shared_state(p, &parsed);
            break;
        case 0x02u:
            saw_non_setup = true;
            lagfx_op_vchan_display_submit(p, &parsed);
            break;
        case 0x04u:
            lagfx_op_display_define_child_fifo(p, &parsed);
            break;
        case 0x06u:
            saw_non_setup = true;
            lagfx_op_vchan_present(p, &parsed);
            break;
        case 0x07u:
            saw_non_setup = true;
            lagfx_op_vchan_present_gamma(p, &parsed);
            break;
        case 0x13u:
            lagfx_op_display_cursor_show(p, &parsed);
            break;
        case 0x14u:
            lagfx_op_display_cursor_glyph(p, &parsed);
            break;
        default:
            LAGFX_TRACE("DisplayVchanDispatcher(ch%u): unknown opcode 0x%04x", ch_id, opcode);
            /* Log first occurrence */
            if (ch_id >= 1 && ch_id <= 16) {
                int idx = ch_id - 1;
                p->ch_opcode_seen[idx][opcode] |= (1u << (opcode & 7));
                if (!p->ch_namespace_logged[idx]) {
                    p->ch_namespace_logged[idx] = true;
                }
            }
            break;
        }
        
        if (stamp > last_stamp) {
            last_stamp = stamp;
        }
        
        cur_rp += cmd_len;
        cmd_idx++;
    }
    
    /* Update read pointer in descriptor */
    uint32_t new_rp = cur_rp;
    if (p->dev->desc.shell.write_memory) {
        p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque,
                                        descr_gpa + 4u, sizeof(new_rp), &new_rp);
    }
    
    LAGFX_LOG("DisplayVchanDispatcher(ch%u): drained %u cmd(s), rp=%u->%u",
              ch_id, cmd_idx, read_ptr, new_rp);
    
    /* Advance stamp cell and raise IRQ */
    if (last_stamp > 0u) {
        lagfx_advance_stamp_cell(p, ch_id, last_stamp);
        p->pending_stamps_bitmask |= (1u << ch_id);
        
        if (saw_non_setup) {
            p->pending_displays_bitmask |= (1u << (ch_id - 5u));
        }
        
        if (p->dev && p->dev->desc.shell.raise_interrupt) {
            p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
            p->interrupts_raised += 1;
        }
    }
}
