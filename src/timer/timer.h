/*
 * libapplegfx-vulkan — host timer callbacks (Phase 1.A online event)
 * src/timer/timer.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBAPPLEGFX_TIMER_H
#define LIBAPPLEGFX_TIMER_H

#include "device.h"

/* Called by QEMU at ~60Hz from a host timer.
 *
 * Parameters:
 *   - dev: the libapplegfx-vulkan device state (contains display_ss_gpa[])
 *   - shell_opaque: AppleGFXLinuxState* — passed back to shell callbacks
 *   - write_memory: callback to write to guest MMIO space
 *   - read_memory:  callback to read from guest MMIO space
 *
 * Returns: true if online IRQ was raised, false otherwise.
 */
bool lagfx_timer_tick_vblank(
    lagfx_device_t *dev,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
    bool (*read_memory)(void *, uint64_t, uint64_t, void *));

#endif // LIBAPPLEGFX_TIMER_H
