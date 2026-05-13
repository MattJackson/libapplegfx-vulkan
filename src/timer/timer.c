/*
 * libapplegfx-vulkan — host timer callbacks (Phase 1.A online event)
 * src/timer/timer.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "timer.h"
#include "../common/log.h"

#define LAGFX_MAX_DISPLAYS 8u

/* Called by QEMU at ~60Hz from a host timer.
 * 
 * Implements the critical deadlock fix (2026-05-03): fire online IRQ
 * IMMEDIATELY when guest enables display (ss[+0x104] == 0xC). Previous
 * attempts to defer or threshold-count led to WindowServer timeouts.
 */
bool lagfx_timer_tick_vblank(
    lagfx_device_t *dev,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
    bool (*read_memory)(void *, uint64_t, uint64_t, void *)) {
    
    if (!dev || !write_memory || !read_memory) {
        return false;
    }

    bool irq_raised = false;

    /* Check each display for enable() completion (ss[+0x104] == 0xC).
     * 
     * DEADLOCK FIX (2026-05-03): Previous attempts deferred or threshold-counted
     * online events, all hitting chicken-and-egg problems:
     *   - defer 5s → WindowServer times out waiting for display
     *   - wait 5 submits → same timeout issue  
     *   - threshold counting → still blocks on lock ordering
     * 
     * Correct solution: fire IRQ IMMEDIATELY when ss[+0x104]==0xC. This breaks
     * the ABBA cycle because WindowServer's doSetDisplayMode() can proceed
     * without waiting for DisplayPipe to complete its lock acquisition.
     */
    for (unsigned i = 0u; i < LAGFX_MAX_DISPLAYS; ++i) {
        uint64_t ss_gpa = dev->display_ss_gpa[i];
        if (ss_gpa == 0u || !(dev->display_ss_installed & (1u << i))) {
            continue;
        }

        /* Read enable flag at ss[+0x104]. Guest writes 0xC when enable() completes. */
        uint32_t enabled_mask = 0u;
        if (!read_memory(shell_opaque, ss_gpa + 0x104u, sizeof(enabled_mask), &enabled_mask)) {
            continue;
        }

        /* Check if display just enabled (ss[+0x104] == 0xC). */
        if (enabled_mask == 0xCu && !(dev->display_ss_enabled & (1u << i))) {
            /* CRITICAL: Raise online IRQ IMMEDIATELY. Do NOT defer or wait. */
            
            /* Set online pending bit (ss[+0x100]=0x4). */
            uint32_t pending = 0x4u;
            if (write_memory(shell_opaque, ss_gpa + 0x100u, sizeof(pending), &pending)) {
                irq_raised = true;

                LAGFX_LOG("timer_tick_vblank: display[%u] enabled (ss[+0x104]=0xC), "
                          "raised IRQ vec=0 IMMEDIATELY", i);
            } else {
                LAGFX_WARN("timer_tick_vblank: display[%u] enable() but write_memory failed", i);
            }

            /* Mark as enabled to avoid repeated IRQ raises. */
            dev->display_ss_enabled |= (1u << i);
        }
    }

    if (irq_raised && dev->desc.shell.raise_interrupt) {
        dev->desc.shell.raise_interrupt(dev->desc.shell.opaque, 0u);
    }

    return irq_raised;
}
