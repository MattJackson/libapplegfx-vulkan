/*
 * libapplegfx-vulkan — Channel 1 dispatcher (Compute/Render)
 * src/dispatchers/channel_1_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LAGFX_CHANNEL_1_DISPATCHER_H
#define LAGFX_CHANNEL_1_DISPATCHER_H

#include "base.h"

struct lagfx_protocol;
typedef struct lagfx_protocol lagfx_protocol_t;

typedef struct {
    lagfx_dispatcher_base_t base;
} lagfx_channel_1_dispatcher_t;

lagfx_channel_1_dispatcher_t *channel_1_dispatcher_new(void);
void channel_1_dispatcher_ring_dispatch(lagfx_channel_1_dispatcher_t *d,
                                        struct lagfx_protocol *p,
                                        uint64_t descr_gpa,
                                        uint8_t ch_id);

#endif /* LAGFX_CHANNEL_1_DISPATCHER_H */
