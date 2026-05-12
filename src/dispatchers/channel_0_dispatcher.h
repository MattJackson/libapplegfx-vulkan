/*
 * libapplegfx-vulkan — Root channel dispatcher (ch 0)
 * src/dispatchers/channel_0_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LAGFX_CHANNEL_0_DISPATCHER_H
#define LAGFX_CHANNEL_0_DISPATCHER_H

#include <stddef.h>
#include "protocol/state.h"

/* Drain root channel ring buffer. Returns number of commands processed. */
size_t channel_0_dispatcher_drain(lagfx_protocol_t *p);

#endif /* LAGFX_CHANNEL_0_DISPATCHER_H */
