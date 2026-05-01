/*
 * libapplegfx-vulkan — protocol decoder public (internal) API
 * src/protocol/protocol.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The protocol decoder owns command-buffer interpretation:
 *
 *   - MMIO register shadow (0x1000..0x1038; 15 registers).
 *   - FIFO ring dequeue driven by the guest "write pointer" doorbell.
 *   - Dispatch to opcode handlers (opcodes.h / ops_*.c).
 *   - Completion path: write stamp to the host-to-guest stamp cell
 *     (readable at MMIO 0x1014) and call shell.raise_interrupt.
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
 *
 * These live in the BAR at 0x1000+. Phase 1.A.2 shadows them in a
 * 15-entry table (offsets 0x1000..0x1038, stride 4). The layout below
 * supersedes the prior scaffold's map, which incorrectly named 0x101c
 * a doorbell. Evidence: re-followup-spec-gaps.md §5.2 (mmioReadAtOffset
 * dispatch lines 40785+, mmioWriteAtOffset dispatch lines 42340+).
 *
 * KNOWN (corrected by A4d, 2026-04-24 — prior scaffold called these
 * "stamp cells" based on a misread of the ISR; they are actually
 * interrupt-pending bitmasks):
 *   0x1000  STATUS_CONTROL (master FIFO enable; R: _fifo state u32)
 *   0x1014  DISPLAY_IRQ_MASK — xchg-and-clear bitmask fed to
 *           AppleParavirtDisplayMachine::signalDisplays. Bit N = display N
 *           has a completed transaction. (Older name: STAMP_CELL_1.)
 *   0x1018  STAMP_IRQ_MASK — xchg-and-clear bitmask fed to
 *           AppleParavirtEventMachine::signalStamps. Bit N = stamp_id N
 *           completed since the last ISR read. (Older name: STAMP_CELL_2.)
 *   0x101c  ROOT_PAGE_NUMBER — read-only getter for _rootPageNumber
 *           (u32 at _PGDevice+0x210). This is NOT a doorbell; the
 *           prior scaffold was wrong.
 *   0x102c  FAULT_STATUS — non-zero signals pending faults and causes
 *           the kext ISR to walk the fault queue via handleFaultInterrupt.
 *           Return 0 unless a fault is actually queued.
 *   0x1034  BINARY_VERSION — read-only _binaryVersion u32.
 *
 * OPEN (runtime capture needed):
 *   One of {0x1004, 0x1008, 0x1010, 0x101c, 0x1020, 0x1024, 0x1028,
 *           0x1030, 0x1034} is the write-pointer doorbell
 *   (setFifoWritten:) — the write-side setter that forwards to
 *   _rootFIFO and drains the ring up to the provided byte offset.
 *   Three others (out of the same set) are setFifoBasePage:,
 *   setFifoLength:, setFifoStart: (ring geometry). The remaining
 *   five are other per-device setters.
 *
 *   Until the offset↔setter mapping is resolved by a runtime trace
 *   (see re-followup-spec-gaps.md §1.5), any write landing in this
 *   range is routed to a common "doorbell candidate" handler that
 *   logs (offset, value) so the capture immediately reveals which
 *   offset carries the write pointer.
 * ------------------------------------------------------------- */

#define LAGFX_REG_STATUS_CONTROL      0x1000u  /* RW — FIFO enable / state       */
#define LAGFX_REG_FIFO_FAULT_OFFSET   0x100cu  /* R  — _rootFIFO.fifoFaultOffset */
#define LAGFX_REG_STAMP_CELL_1        0x1014u  /* R  — display IRQ bitmask (was "stamp latch") */
#define LAGFX_REG_STAMP_CELL_2        0x1018u  /* R  — stamp IRQ bitmask (was "_interruptStamp") */
#define LAGFX_REG_ROOT_PAGE_NUMBER    0x101cu  /* R  — _rootPageNumber (NOT doorbell) */
#define LAGFX_REG_BINARY_VERSION      0x1034u  /* R  — _binaryVersion            */

/* Bank covers 0x1000..0x1038 inclusive (15 regs), stride 4. */
#define LAGFX_REG_BASE                0x1000u
#define LAGFX_REG_COUNT               15u
#define LAGFX_REG_LAST                0x1038u

/* Candidate range for the setFifoWritten doorbell + the three ring
 * geometry setters + five other per-device setters. Until runtime
 * capture disambiguates, writes to any offset here are funneled
 * through lagfx_fifo_on_mmio_setter() in fifo.c. Reads from this range
 * are served by the plain register shadow (most return the last
 * value written; 0x101c returns _rootPageNumber). */
#define LAGFX_REG_SETTER_CAND_FIRST   0x1004u
#define LAGFX_REG_SETTER_CAND_LAST    0x1034u

/* MSI-X table lives below the register bank; decoder must not claim. */
#define LAGFX_MSIX_RANGE_END          0x1000u

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
 * Writes in the setter-candidate range (0x1004..0x1034) log the
 * (offset, value) pair and trigger the FIFO drain probe — the real
 * doorbell offset is not yet known, so every candidate is observed. */
void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset,
                               uint32_t value);

/* === Test / introspection hooks ===============================
 *
 * These exist so unit tests can synthesize command bytes and drive
 * the dispatcher without going through the (stubbed) ring-buffer
 * read path. Not called from production code.
 * ------------------------------------------------------------- */

/* Dispatch a single pre-parsed command. The handler's return code is
 * passed back verbatim. Also runs the completion path (stamp writeback
 * + IRQ) unconditionally — the 12-byte header has no flags field and
 * every command signals its stamp on completion (see
 * re-followup-spec-gaps.md §5.1).
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

/* Query the last completed stamp (tests + observability). */
uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p);

/* Process the delayed stamp ACK tick (called from display_tick_vblank).
 * If a delayed ACK is pending and the threshold is reached, advances
 * the stamp cell and raises IRQ to unblock process_online. */
void lagfx_protocol_process_delayed_ack(lagfx_protocol_t *p);

#endif /* LIBAPPLEGFX_PROTOCOL_H */
