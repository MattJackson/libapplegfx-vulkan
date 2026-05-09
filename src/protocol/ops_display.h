/*
 * libapplegfx-vulkan — display-domain opcode handler state (Phase 2.C)
 * src/protocol/ops_display.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Private exposure surface for tests to observe state captured by the
 * display-opcode handlers that does NOT fit cleanly on
 * lagfx_display_entry_t (per-protocol cursor glyph + shared-state page +
 * vblank counter). All storage lives in ops_display.c file-scope
 * static — Phase 2 assumes one lagfx_protocol_t per process (matches
 * how tests build devices). See re-followup-spec-gaps.md §14.4 / §14.6
 * for the underlying wire-format assumptions.
 *
 * NOT installed; private to src/protocol/ + tests/.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_OPS_DISPLAY_H
#define LIBAPPLEGFX_PROTOCOL_OPS_DISPLAY_H

#include "opcodes.h"

#include <stdbool.h>
#include <stdint.h>

/* Maximum glyph bytes we retain for introspection. Real cursor
 * glyphs on macOS are 32x32 ARGB8888 = 4 KiB; we cap at 16 KiB to
 * tolerate up to 64x64 without a heap allocation. */
#define LAGFX_CURSOR_GLYPH_MAX_BYTES  (16u * 1024u)

/* Opaque decode record for the last cursor-show command (0x13).
 * Populated by lagfx_op_display_cursor_show. */
typedef struct {
    bool     valid;
    uint32_t display_id;
    int16_t  x;              /* screen-space, signed 16-bit on the wire */
    int16_t  y;
    uint32_t visible;        /* nonzero = cursor should draw */
    uint16_t hot_x;          /* upper half of the packed hot u32      */
    uint16_t hot_y;          /* lower half                             */
} lagfx_cursor_show_state_t;

/* Opaque decode record for the last cursor-glyph command (0x14).
 * Populated by lagfx_op_display_cursor_glyph. */
typedef struct {
    bool     valid;
    uint32_t display_id;
    uint64_t glyph_va;       /* task-mapped guest VA carrying ARGB8888 pixels */
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t hot_x;
    uint32_t hot_y;

    /* Captured pixel payload (up to LAGFX_CURSOR_GLYPH_MAX_BYTES
     * bytes read via shell.read_memory); `captured_len` reflects the
     * actual number of bytes read. Zero on read_memory failure. */
    size_t   captured_len;
    uint8_t  bytes[LAGFX_CURSOR_GLYPH_MAX_BYTES];
} lagfx_cursor_glyph_state_t;

/* Shared-state page (0x17) tracking. See re-followup-spec-gaps.md
 * §14.6. WindowServer polls this page for vblank counter + frame
 * sequence numbers at compositor rate. We treat it as a mailbox: on
 * registration the handler zeroes the page via shell.write_memory and
 * each subsequent lagfx_ops_display_tick_vblank() bumps u32 at
 * offset +0 and DMAs it back to the page.
 *
 * The counter is also kept in host-side shadow so tests can read
 * it without driving a synthetic mailbox page. */
typedef struct {
    bool     installed;      /* guest wrote a non-zero pageVA via 0x17 */
    uint64_t page_va;        /* guest GPA of the mailbox page         */
    uint32_t vblank_counter; /* monotonic; mirrors what we wrote      */
} lagfx_shared_state_t;

/* ----------------------------------------------------------------
 * 0x19 `CmdDisplayCompositorParameters` (§14 M6 readiness):
 * WindowServer passes layer-blend + gamma-related settings. Log-only
 * stub — §14.10 classifies colour-correctness as "cosmetic only"
 * through M6, so we capture the raw payload for later analysis and
 * ack the stamp. */
#define LAGFX_COMPOSITOR_PARAMS_CAPTURE_MAX 256u
typedef struct {
    bool     valid;
    uint32_t dispatch_count;
    uint32_t last_stamp;
    uint32_t display_id;
    uint32_t payload_size;
    uint32_t captured_len;
    uint8_t  bytes[LAGFX_COMPOSITOR_PARAMS_CAPTURE_MAX];
} lagfx_compositor_params_state_t;

/* ----------------------------------------------------------------
 * 0x1a `CmdDisplaySetGuestICCProfile` (§14 M6 readiness):
 * color-management ICC profile upload. Log-only stub for M6 —
 * §14.10 classifies as "cosmetic only". We capture a prefix of the
 * raw profile bytes for the §14.8 instrumentation pass. */
#define LAGFX_ICC_PROFILE_CAPTURE_MAX 512u
typedef struct {
    bool     valid;
    uint32_t dispatch_count;
    uint32_t last_stamp;
    uint32_t display_id;
    uint64_t profile_va;     /* task-mapped GPA of the ICC profile blob */
    uint32_t profile_size;   /* bytes, as declared by the guest         */
    uint32_t payload_size;
    uint32_t captured_len;
    uint8_t  bytes[LAGFX_ICC_PROFILE_CAPTURE_MAX];
} lagfx_icc_profile_state_t;

/* Accessors — declared here so tests can introspect. Pointers are
 * valid for the life of the process; do NOT free. */
const lagfx_cursor_show_state_t       *lagfx_ops_display_last_cursor_show(void);
const lagfx_cursor_glyph_state_t      *lagfx_ops_display_last_cursor_glyph(void);
const lagfx_shared_state_t            *lagfx_ops_display_shared_state(void);
const lagfx_compositor_params_state_t *lagfx_ops_display_last_compositor_params(void);
const lagfx_icc_profile_state_t       *lagfx_ops_display_last_icc_profile(void);

/* Reset all display-handler static state (tests call between cases).
 * Idempotent; safe to call before any command arrives. */
void lagfx_ops_display_reset(void);

/* Advance the vblank counter and DMA it back to the guest's mailbox
 * page at u32@+0. Intended to be called from the QEMU display timer
 * at 60 Hz once the shell integration lands; tests can invoke it
 * directly to assert mailbox behaviour.
 *
* `shell_opaque` + `write_memory` mirror the shell-callback surface
     * so the implementation stays self-contained and doesn't reach into
     * lagfx_device_t internals. Returns true if the DMA write was
     * attempted (page was installed and write_memory is non-NULL). */
 bool lagfx_ops_display_tick_vblank(
     void *shell_opaque,
     bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
     bool (*read_memory)(void *, uint64_t, uint64_t, void *));

/* Handler forward declarations — registered in the opcode table. */
lagfx_handler_status_t lagfx_op_display_cursor_show(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_cursor_glyph(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_set_shared_page(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_compositor_params(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_set_icc_profile(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LIBAPPLEGFX_PROTOCOL_OPS_DISPLAY_H */
