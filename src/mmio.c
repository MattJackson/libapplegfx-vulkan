/*
 * libapplegfx-vulkan — MMIO dispatch (Phase 1.A.2)
 * src/mmio.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Entry points for guest MMIO hitting the paravirt GPU BAR. Phase
 * 1.A.2 forwards every access into the protocol decoder attached to
 * the device (dev->protocol_state). The decoder owns the register
 * shadow (0x1000..0x1028) and the doorbell / fence semantics per
 * mos/paravirt-re/phase-1a2-decoder-plan.md §3 and §7.2.
 *
 * Anything below LAGFX_MSIX_RANGE_END (0x1000) is MSI-X table /
 * PBA — the shell owns that; decoder ignores and we return 0 for
 * reads / drop for writes. That matches §3.1.
 */

#include "device.h"
#include "protocol/protocol.h"
#include "common/log.h"

#include <stdint.h>

uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_read: invalid device %p", (void *)device);
        return 0;
    }

    lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
    if (!p) {
        /* Decoder never attached (pre-1.A.2 shells or teardown race).
         * Log and return 0 — guest sees "not yet ready". */
        LAGFX_LOG("mmio_read: no decoder attached, off=0x%llx -> 0",
                  (unsigned long long)offset);
        return 0;
    }

    return lagfx_protocol_mmio_read(p, offset);
}

void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset,
                      uint32_t value) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_write: invalid device %p", (void *)device);
        return;
    }

    lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
    if (!p) {
        LAGFX_LOG("mmio_write: no decoder attached, off=0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    lagfx_protocol_mmio_write(p, offset, value);
}
