/*
 * libapplegfx-vulkan — IOSurface-family opcode handlers (M6 log-only stubs)
 * src/protocol/ops_iosurface.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Log+ack handlers for the IOSurface cross-process back-buffer opcodes
 * that WindowServer emits during M6 startup. See re-followup-spec-
 * gaps.md §14.5 and phase-4-iosurface-videotoolbox-plan.md §3.3/§3.4
 * for the conjectured payload layouts.
 *
 * CONFIDENCE: LOW. The opcode numbers (0x27/0x28/0x29) and payload
 * shapes are conjectured by symmetry with the observed 0x26
 * `CmdDeleteIOSurfaceBacking2` (§14.5 ¶1). Phase 4 promotes these to
 * real handlers against a VkImage-backed IOSurface handle table; for
 * M6 the job is to (a) avoid the "unknown opcode" default-handler log
 * spam that hides real regressions and (b) retain an opcode-local
 * capture of whatever payload bytes arrive so the §14.8 instrumentation
 * pass can confirm/refute the conjectured shape.
 *
 * Storage: a small file-scope ring of the last N decoded commands
 * (per-opcode) so tests can assert dispatch fired + stamp propagated,
 * and so the §14.8 capture pass can dump real payloads.
 *
 * NOT installed; private to src/protocol/ + tests/.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_OPS_IOSURFACE_H
#define LIBAPPLEGFX_PROTOCOL_OPS_IOSURFACE_H

#include "opcodes.h"

#include <stdbool.h>
#include <stdint.h>

/* Maximum raw payload bytes we retain per opcode for introspection.
 * The real IOSurface command payloads are small (< 64 B per §14.5
 * conjecture); 256 B is comfortable slack. */
#define LAGFX_IOSURFACE_CAPTURE_MAX_BYTES 256u

/* Per-opcode capture record. One slot per opcode — newest overrides
 * previous. Tests read through the accessors below. */
typedef struct {
    bool     valid;                 /* true once at least one command landed */
    uint32_t dispatch_count;        /* cumulative dispatches of this opcode  */

    /* Last observed payload bytes (up to captured_len). Raw wire bytes
     * so the §14.8 instrumentation spike can hex-dump them. */
    uint32_t payload_size;
    uint32_t captured_len;
    uint8_t  bytes[LAGFX_IOSURFACE_CAPTURE_MAX_BYTES];

    /* Conjectured-layout decode of the last command (best-effort;
     * only fields whose offsets fit in payload_size are populated).
     *
     * For 0x27 `CmdDeleteIOSurface`:
     *   surface_id   @ +0x00 (u32)
     *
     * For 0x28 `CmdIOSurfaceCreate` (§14.5 + Phase 4 §3.3):
     *   surface_id   @ +0x00 (u32)
     *   width        @ +0x04 (u32)
     *   height       @ +0x08 (u32)
     *   pixel_format @ +0x0c (u32)
     *   bytes_per_row@ +0x10 (u32)
     *   size         @ +0x14 (u64)
     *
     * For 0x29 `CmdIOSurfaceUpdate` (§14.5 conjecture, symmetric with
     * swap-mapping):
     *   surface_id   @ +0x00 (u32)
     *   flags        @ +0x04 (u32)
     *   size         @ +0x08 (u64)
     */
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t bytes_per_row;
    uint32_t flags;
    uint64_t size;

    uint32_t last_stamp;
} lagfx_iosurface_capture_t;

/* Accessors — valid for the life of the process; do NOT free. */
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_delete(void);
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_create(void);
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_update(void);

/* Reset all captures (tests call between cases). Idempotent. */
void lagfx_ops_iosurface_reset(void);

/* Handler forward declarations — registered in the opcode table. */
lagfx_handler_status_t lagfx_op_iosurface_delete_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_iosurface_create_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_iosurface_lookup(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LIBAPPLEGFX_PROTOCOL_OPS_IOSURFACE_H */
