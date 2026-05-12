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
#include "doorbell.h"
#include "common/log.h"

#include <stdint.h>

uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_read: invalid device %p", (void *)device);
        return 0;
    }

    /* Reads are handled by QEMU's MMIO layer — we only do writes */
    (void)offset;
    return 0;
}

void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset,
                      uint32_t value) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_write: invalid device %p", (void *)device);
        return;
    }

    /* Route doorbell writes through unified dispatcher */
    lagfx_protocol_t *p = (lagfx_protocol_t *)device->protocol_state;
    if (!p) {
        LAGFX_LOG("mmio_write: no decoder attached, off=0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    /* Try to find a door that handles this offset */
    const doorbell_door_descriptor_t *door = doorbell_lookup_by_offset(offset);
    if (door && door->dispatch_fn) {
        LAGFX_LOG("mmio_write: BAR0+0x%llx → door 0x%x, dispatch", 
                  offset & 0xFFFFULL, door->id);
        door->dispatch_fn(p, value);
        return;
    }

    /* Not a doorbell — shadow config register and return */
    int idx = lagfx_reg_index(offset);
    if (idx >= 0 && idx < 16) {
        p->reg[idx] = value;
        LAGFX_TRACE("mmio_write: config reg[%d]=0x%x", idx, value);
    } else {
        LAGFX_WARN("mmio_write: unknown offset 0x%llx", (unsigned long long)offset);
    }
}
