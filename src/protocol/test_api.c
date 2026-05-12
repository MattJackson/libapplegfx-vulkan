/*
 * libapplegfx-vulkan — Test API (Phase 1.A.2)
 * src/protocol/test_api.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal test API exports for lagfx_protocol_dispatch_one and mmio helpers.
 * These functions are called by tests (resource-registry.c, m4-doorbell-drain.c, etc.)
 * to exercise the protocol decoder directly without going through QEMU MMIO path.
 */

#include "protocol.h"
#include "common/log.h"

int lagfx_protocol_dispatch_one(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes,
                                size_t cmd_len) {
    lagfx_cmd_header_t hdr;
    int did_run = 0;
    int rc = lagfx_dispatch_inner(p, cmd_bytes, cmd_len, &hdr, &did_run);
    /* RootChannel completions go to slot 0; whether the handler ran
     * or we hit a parse/size error, we ack the stamp so the guest
     * doesn't park. */
    if (did_run || rc == LAGFX_HANDLER_ERR_SIZE) {
        lagfx_protocol_complete_stamp(p, hdr.stamp);
    }
    return rc;
}

/* Per-channel variant — runs the handler but does NOT auto-complete
 * the stamp. The caller is responsible for advancing stamp_cell[ch] +
 * setting the pending_stamps_bitmask bit + raising the IRQ once after
 * draining all cmds in the ring. */
int lagfx_protocol_dispatch_one_no_stamp(lagfx_protocol_t *p,
                                         const uint8_t *cmd_bytes,
                                         size_t cmd_len,
                                         lagfx_cmd_header_t *out_hdr) {
    lagfx_cmd_header_t local_hdr;
    int did_run = 0;
    int rc = lagfx_dispatch_inner(p, cmd_bytes, cmd_len, &local_hdr, &did_run);
    (void)did_run;
    if (out_hdr) {
        *out_hdr = local_hdr;
    }
    return rc;
}

uint32_t lagfx_protocol_mmio_read(lagfx_protocol_t *p, uint64_t offset) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }
    (void)offset;
    /* Legacy MMIO read path - returns 0 for all accesses.
     * Tests that need register shadow should access p->ring_size directly. */
    LAGFX_TRACE("mmio_read: legacy stub off=0x%llx", (unsigned long long)offset);
    return 0;
}

void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset, uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }
    (void)offset;
    (void)value;
    /* Legacy MMIO write path - all writes ignored.
     * Tests should set protocol state directly instead of going through MMIO. */
    LAGFX_TRACE("mmio_write: legacy stub off=0x%llx val=0x%08x",
                (unsigned long long)offset, value);
}

uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}
