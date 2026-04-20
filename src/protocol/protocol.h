/*
 * libapplegfx-vulkan — protocol decoder public (internal) API
 * src/protocol/protocol.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The protocol decoder owns command-buffer interpretation:
 *
 *   - MMIO register shadow (0x1000..0x1028).
 *   - FIFO ring dequeue driven by the 0x101c doorbell.
 *   - Dispatch to opcode handlers (opcodes.h / ops_*.c).
 *   - Completion path: write 0x1020 fence + call shell.raise_interrupt.
 *
 * Concurrency: Phase 1.A.2 is single-threaded. Every entry point is
 * expected to run under the QEMU BQL (or the device's AIO context —
 * QEMU's device model guarantees serialization per-device). No
 * internal locking. See phase-1a2-decoder-plan.md §5.1.
 *
 * This header is private to other TUs inside libapplegfx-vulkan
 * (device.c, mmio.c, tests). It is NOT installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_H
#define LIBAPPLEGFX_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration. src/device.h is the authoritative definition. */
struct lagfx_device;

/* Opaque handle to an attached decoder. Real layout is in state.h. */
typedef struct lagfx_protocol lagfx_protocol_t;

/* === MMIO register offsets ====================================
 * These live in the BAR at 0x1000+. Phase 1.A.2 shadows them in
 * an 11-entry table. See phase-1a2-decoder-plan.md §3.
 * ------------------------------------------------------------- */

#define LAGFX_REG_STATUS_CONTROL  0x1000u  /* RW — device status */
#define LAGFX_REG_INTR_CONTROL    0x1004u  /* RW — interrupt enable / raise */
#define LAGFX_REG_INTR_CLEAR      0x1008u  /* RW — clear pending / queue write ptr */
#define LAGFX_REG_CONFIG          0x100cu  /* RW — config flags / queue size */
#define LAGFX_REG_FEATURE_ENABLE  0x1010u  /* RW */
#define LAGFX_REG_QUEUE_CONTROL   0x1014u  /* RW — ring arm */
#define LAGFX_REG_FIFO_STATE      0x1018u  /* R  — drain state */
#define LAGFX_REG_DOORBELL        0x101cu  /* RW — ATOMIC_SWAP_1; hot path */
#define LAGFX_REG_FENCE           0x1020u  /* RW — ATOMIC_SWAP_2; host writes stamps */
#define LAGFX_REG_UNKNOWN_24      0x1024u
#define LAGFX_REG_UNKNOWN_28      0x1028u

#define LAGFX_REG_BASE            0x1000u
#define LAGFX_REG_COUNT           11u
#define LAGFX_REG_LAST            0x1028u

/* MSI-X table lives below the register bank; decoder must not claim. */
#define LAGFX_MSIX_RANGE_END      0x1000u

/* === Lifecycle ================================================ */

/* Attach a decoder to a device. Called from lagfx_device_new().
 * Returns NULL on allocation failure. The returned handle is stored
 * in dev->protocol_state. */
lagfx_protocol_t *lagfx_protocol_new(struct lagfx_device *dev);

/* Tear down. Safe on NULL. */
void lagfx_protocol_free(lagfx_protocol_t *p);

/* Reset in-flight state (tables + counters). Keeps ring geometry
 * and registers in their current state so the guest can re-arm the
 * ring without losing negotiated parameters. */
void lagfx_protocol_reset(lagfx_protocol_t *p);

/* === MMIO dispatch =========================================== */

/* Read a 4-byte register. Returns 0 for out-of-range offsets and
 * logs via LAGFX_LOG. Offsets below LAGFX_MSIX_RANGE_END return 0
 * silently (shell owns that range). */
uint32_t lagfx_protocol_mmio_read(lagfx_protocol_t *p, uint64_t offset);

/* Write a 4-byte register. Out-of-range writes are logged and dropped.
 * A write to LAGFX_REG_DOORBELL triggers the FIFO drain path. */
void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset,
                               uint32_t value);

/* === Test / introspection hooks ===============================
 *
 * These exist so unit tests can synthesize command bytes and drive
 * the dispatcher without going through the (stubbed) ring-buffer
 * read path. Not called from production code.
 * ------------------------------------------------------------- */

/* Dispatch a single pre-parsed command. The handler's return code is
 * passed back verbatim. Also runs the completion path (fence + IRQ)
 * if the command's flags include LAGFX_FLAG_COMPLETION_EXPECTED.
 *
 * Used by fifo.c once it has read a command from the ring and by
 * tests/protocol-dispatch.c to drive handlers directly. */
int lagfx_protocol_dispatch_one(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes,
                                size_t cmd_len);

/* Read protocol counters. Useful for tests. Any of the out
 * pointers may be NULL. */
void lagfx_protocol_stats(const lagfx_protocol_t *p,
                          uint64_t *total_cmds_seen_out,
                          uint64_t *total_cmds_completed_out,
                          uint64_t *unknown_opcode_count_out);

/* Query the last completed / doorbell stamps (tests + observability). */
uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p);
uint32_t lagfx_protocol_last_doorbell_stamp(const lagfx_protocol_t *p);

#endif /* LIBAPPLEGFX_PROTOCOL_H */
