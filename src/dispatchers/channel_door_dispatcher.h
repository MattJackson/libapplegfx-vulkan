/*
 * libapplegfx-vulkan — Channel door dispatcher (BAR0+0x1020)
 * src/dispatchers/channel_door_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LAGFX_CHANNEL_DOOR_DISPATCHER_H
#define LAGFX_CHANNEL_DOOR_DISPATCHER_H

#include <stdint.h>

/* Dispatch channel doorbell write (BAR0+0x1020) - routes by channel ID to compute/display */
void channel_door_dispatcher_dispatch(void *protocol_state, uint32_t data);

#endif /* LAGFX_CHANNEL_DOOR_DISPATCHER_H */
