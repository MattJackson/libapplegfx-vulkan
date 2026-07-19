/*
 * libapplegfx-vulkan — Display channel dispatcher (ch 5+)
 * src/dispatchers/channel_display_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LAGFX_CHANNEL_DISPLAY_DISPATCHER_H
#define LAGFX_CHANNEL_DISPLAY_DISPATCHER_H

#include <stddef.h>
#include "protocol/state.h"

/* Drain display channel ring buffer. Returns number of commands processed. */
size_t channel_display_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id);

#endif /* LAGFX_CHANNEL_DISPLAY_DISPATCHER_H */
