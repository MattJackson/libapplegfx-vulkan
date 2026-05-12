/*
 * libapplegfx-vulkan — Protocol lifecycle management
 * src/protocol/lifecycle.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "state.h"
#include "../common/log.h"

static int g_initialized = 0;

lagfx_protocol_t* lagfx_protocol_new(void) {
    lagfx_protocol_t *p = calloc(1, sizeof(*p));
    if (!p) {
        LAGFX_ERR("protocol_new: out of memory");
        return NULL;
    }

    p->magic = LAGFX_PROTOCOL_MAGIC;
    
    /* Initialize dispatcher registry once */
    if (!g_initialized) {
        // TODO: call lagfx_dispatcher_registry_init() here
        g_initialized = 1;
    }

    /* Set initial register shadow (STATUS_CONTROL: present + ready) */
    p->reg[REG_STATUS_CONTROL] = 0x3u;
    
    LAGFX_LOG("protocol_new: created");
    return p;
}

void lagfx_protocol_free(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) return;
    
    LAGFX_LOG("protocol_free: seen=%llu completed=%llu",
              (unsigned long long)p->total_cmds_seen,
              (unsigned long long)p->total_cmds_completed);
    
    memset(p, 0, sizeof(*p));
    free(p);
}

void lagfx_protocol_reset(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) return;
    
    /* Clear tables and inflight state; preserve ring geometry */
    memset(p->tasks, 0, sizeof(p->tasks));
    memset(p->fifos, 0, sizeof(p->fifos));
    memset(p->display_child_rings, 0, sizeof(p->display_child_rings));
    
    p->total_cmds_seen = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->write_ptr = 0;
    p->read_ptr = 0;
    p->pending_stamps_bitmask = 0;
    p->pending_displays_bitmask = 0;
}
