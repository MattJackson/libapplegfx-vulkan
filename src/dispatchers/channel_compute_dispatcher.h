/*
 * libapplegfx-vulkan — Compute channel dispatcher (ch 1-4)
 * src/dispatchers/channel_compute_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LAGFX_CHANNEL_COMPUTE_DISPATCHER_H
#define LAGFX_CHANNEL_COMPUTE_DISPATCHER_H

#include <stddef.h>
#include "protocol/state.h"

/* Drain compute channel ring buffer. Returns number of commands processed. */
size_t channel_compute_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id);

#endif /* LAGFX_CHANNEL_COMPUTE_DISPATCHER_H */
