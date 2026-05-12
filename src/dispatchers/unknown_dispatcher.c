/*
 * libapplegfx-vulkan — Catch-all/Unknown channel dispatcher implementation
 * src/dispatchers/unknown_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Always registered for any channel ID not explicitly handled elsewhere.
 * Logs all commands for debugging — "I don't know this channel, but I'll watch it."
 */

#include "unknown_dispatcher.h"
#include "../device.h"
#include "../protocol/state.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Constructor ======================================================== */

lagfx_unknown_dispatcher_t *unknown_dispatcher_new(void) {
    lagfx_unknown_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    
    /* Name covers all channels as catch-all */
    d->base.name = "UnknownDispatcher(ch0-255)";
    d->base.channel_id_min = 0;
    d->base.channel_id_max = 255;
    
    return d;
}

/* === Ring Dispatch — log everything, then delegate ====================== */

void unknown_dispatcher_ring_dispatch(lagfx_unknown_dispatcher_t *d,
                                     struct lagfx_protocol *p,
                                     uint64_t descr_gpa,
                                     uint8_t ch_id) {
    (void)d;  /* No state needed for logging */
    
    LAGFX_WARN("UnknownDispatcher(ch%u): UNREGISTERED CHANNEL! Logging all commands", ch_id);
    
    /* Read descriptor to understand what we're dealing with */
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_ERR("UnknownDispatcher: cannot read shared page");
        return;
    }
    
    uint8_t descr[20] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        descr_gpa, sizeof(descr), descr)) {
        LAGFX_ERR("UnknownDispatcher: failed to read descriptor");
        return;
    }
    
    /* Descriptor layout: { write_ptr, read_ptr, mid, chan_id, ring_pfn } */
    uint32_t write_ptr = (uint32_t)descr[0] | ((uint32_t)descr[1] << 8) |
                         ((uint32_t)descr[2] << 16) | ((uint32_t)descr[3] << 24);
    uint32_t read_ptr  = (uint32_t)descr[4] | ((uint32_t)descr[5] << 8) |
                         ((uint32_t)descr[6] << 16) | ((uint32_t)descr[7] << 24);
    uint32_t mid       = (uint32_t)descr[8] | ((uint32_t)descr[9] << 8) |
                        ((uint32_t)descr[10] << 16) | ((uint32_t)descr[11] << 24);
    uint32_t chan_id   = (uint32_t)descr[12] | ((uint32_t)descr[13] << 8) |
                        ((uint32_t)descr[14] << 16) | ((uint32_t)descr[15] << 24);
    uint32_t ring_pfn  = (uint32_t)descr[16] | ((uint32_t)descr[17] << 8) |
                         ((uint32_t)descr[18] << 16) | ((uint32_t)descr[19] << 24);
    
    LAGFX_WARN("UnknownDispatcher(ch%u): descriptor wr=%u rd=%u mid=0x%x "
               "chan_id=%u ring_pfn=0x%x",
               ch_id, write_ptr, read_ptr, mid, chan_id, ring_pfn);
    
    if (ring_pfn == 0 || write_ptr <= read_ptr) {
        LAGFX_WARN("UnknownDispatcher: empty ring or invalid pointers");
        return;
    }
    
    /* Log first few commands from this unknown channel */
    uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
    uint32_t child_ring_size = (ch_id >= 5u) ? 0x1000u : 0x10000u;
    
    uint32_t cur_rp = read_ptr;
    int log_count = 0;
    
    while (cur_rp + 12u <= write_ptr && log_count < 5) {
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
            LAGFX_WARN("UnknownDispatcher: failed to read header at rp=%u", cur_rp);
            break;
        }
        
        uint16_t opcode = (uint16_t)(hdr_bytes[0] | (hdr_bytes[1] << 8));
        uint32_t cmd_len = (uint32_t)hdr_bytes[4] | ((uint32_t)hdr_bytes[5] << 8) |
                          ((uint32_t)hdr_bytes[6] << 16) | ((uint32_t)hdr_bytes[7] << 24);
        
        LAGFX_WARN("UnknownDispatcher(ch%u): cmd[%d] opcode=0x%04x len=%u at rp=%u",
                   ch_id, log_count, opcode, cmd_len, cur_rp);
        
        /* Track per-channel opcode namespace */
        if (ch_id >= 1 && ch_id <= 16) {
            int idx = ch_id - 1;
            p->ch_opcode_seen[idx][opcode] |= (1u << (opcode & 7));
            if (!p->ch_namespace_logged[idx]) {
                p->ch_namespace_logged[idx] = true;
                LAGFX_WARN("UnknownDispatcher(ch%u): namespace tracking enabled", ch_id);
            }
        }
        
        log_count++;
        cur_rp += cmd_len;
    }
    
    LAGFX_WARN("UnknownDispatcher(ch%u): logged first %d commands — check if this channel should have a dedicated dispatcher",
               ch_id, log_count);
}

/* === Reset ============================================================= */

void unknown_dispatcher_reset(lagfx_unknown_dispatcher_t *d) {
    (void)d;  /* No state to reset */
}
