/*
 * libapplegfx-vulkan — MMIO dispatch stub (Phase 1.A.1)
 * src/mmio.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Entry points for guest MMIO hitting the paravirt GPU BAR. Phase
 * 1.A.1 stubs: read returns 0 for every offset, write is ack'd;
 * both log via LAGFX_LOG so envvar-enabled traces show the guest's
 * register-access pattern. The real dispatch table lives in
 * src/protocol/ (Phase 1.A.2). See
 * mos/paravirt-re/command-buffer-format.md for the shape.
 */

#include "device.h"
#include "common/log.h"

#include <stdint.h>

uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_read: invalid device %p", (void *)device);
        return 0;
    }

    /* TODO(Phase-1.A.2): dispatch through src/protocol/. For now every
     * offset returns 0 which matches "register not yet initialized"
     * — Apple's kext drivers are generally tolerant of zero reads
     * before firmware hands off. */
    LAGFX_LOG("mmio_read  dev=%p off=0x%llx -> 0 (stub)",
              (void *)device, (unsigned long long)offset);
    return 0;
}

void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset,
                      uint32_t value) {
    if (!lagfx_device_is_valid(device)) {
        LAGFX_ERR("mmio_write: invalid device %p", (void *)device);
        return;
    }

    /* TODO(Phase-1.A.2): dispatch through src/protocol/. Writes are
     * swallowed for now; the guest kext will see reads returning 0
     * and retry/time out, which is fine for bring-up.
     *
     * Future: offset < 0x1000 is MSI-X table (shell handles),
     *         offset >= 0x1000 hits register bank per
     *         paravirt-re/command-buffer-format.md. */
    LAGFX_LOG("mmio_write dev=%p off=0x%llx val=0x%08x (ack, stub)",
              (void *)device, (unsigned long long)offset, value);
}
