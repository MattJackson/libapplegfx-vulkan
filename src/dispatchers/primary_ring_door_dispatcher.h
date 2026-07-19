/*
 * libapplegfx-vulkan — Primary ring door dispatcher (BAR0+0x1008)
 * src/dispatchers/primary_ring_door_dispatcher.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LAGFX_PRIMARY_RING_DOOR_DISPATCHER_H
#define LAGFX_PRIMARY_RING_DOOR_DISPATCHER_H

#include <stdint.h>

/* Dispatch primary ring doorbell write (BAR0+0x1008) */
void primary_ring_door_dispatcher_dispatch(void *protocol_state, uint32_t data);

#endif /* LAGFX_PRIMARY_RING_DOOR_DISPATCHER_H */
