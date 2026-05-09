/*
 * Display-domain opcode handlers
 * src/protocol/ops_display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Last updated: 2026-05-01
 *
 * Display-pipe opcodes for the Apple Paravirt GPU protocol.
 * These are sent on the per-display VirtualChannel rings (ch 5..12)
 * using the compact opcode namespace {0x01, 0x02, 0x04, 0x06, 0x07}.
 *
 * RE context (paravirt-re/classes/):
 *   - DisplayPipe-setupSharedState-summary.md: opcode 0x01 flow
 *   - display-pipe-enable-online-sequence.md: 0x02 + online state machine
 *   - CmdDefineChildFIFO-display-vchan-analysis.md: 0x04 44-byte descriptor
 *   - DisplayPipe-submitTransaction-opcodes.md: 0x06/0x07 present path
 *   - cursor-rendering-stage-20.md: cursor opcodes 0x13/0x14 are
 *     fully wired: handlers parse payloads AND invoke QEMU callbacks
 *     (cursor_glyph → dpy_cursor_define, cursor_show → dpy_mouse_set).
 *
 * Display init sequence (from RE):
 *   setupSharedState (0x01) → enable() → host online event →
 *   process_online → connectionChange → ACTIVE →
 *   frame delivery via 0x06/0x07 + sub-channel FIFOs via 0x04
 *   cursor glyph/position via 0x13/0x14 on sub-channels
 *
 * Dispatcher wiring note: 0x13/0x14 are registered in opcodes.c
 * with their respective handler functions (not NULL). The handlers
 * parse payloads and invoke display callbacks (cursor_glyph,
 * cursor_moved, cursor_show) which call into QEMU's dpy_ API.
 * Tests drive handlers directly — see tests/protocol-dispatch.c.
 *
 * Payload-layout confidence: HIGH for 0x13/0x14 (validated
 * against kext vtables at +0x14562094 and +0x145619c4).
 * Handlers fail-open on size mismatch; stamp still signaled so the
 * guest never hangs on a bad decode (re-followup-spec-gaps.md §6).
 */

#include "opcodes.h"
#include "ops_display.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"
#include "../device.h"
#include "../display.h"

#include <stdint.h>
#include <string.h>

/* ================================================================
 * Little-endian primitives
 * ================================================================ */

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

static inline uint16_t lagfx_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

/* Helper to read float from little-endian u32. */
static inline float lagfx_lef32(const uint8_t *b) {
    union { uint32_t u; float f; } conv;
    conv.u = lagfx_le32(b);
    return conv.f;
}

static inline void lagfx_put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8) & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* ================================================================
 * CmdDisplayAck (0x10) — present completion signal
 *
 * Guest sends this after a frame present (opcode 0x06/0x07) completes.
 * The kext's IOAccelEventFence waits for the stamp signal via
 * the waitForStamp mechanism (paravirt-re/classes/waitForStamp-mechanism-summary.md).
 * Without acking this stamp, the present loop deadlocks.
 *
 * display_acks_received is diagnostic only; real matching uses
 * the transaction_pending flag + pending_transaction_id.
 * ================================================================ */

lagfx_handler_status_t lagfx_op_display_ack(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdDisplayAck: payload missing or too small "
                   "(size=%u, need 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    uint32_t frame_id   = lagfx_le32(hdr->payload + 4);

    p->display_acks_received += 1;

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplayAck: displayID=%u frameID=%u — display not "
                   "registered (fail-open, no-op)",
                   display_id, frame_id);
        return LAGFX_HANDLER_OK;
    }

    if (d->transaction_pending && d->pending_transaction_id == frame_id) {
        d->transaction_pending = false;
        d->transaction_acked   = true;
        LAGFX_LOG("CmdDisplayAck: displayID=%u frameID=%u stamp=0x%08x "
                  "(matched pending txn)",
                  display_id, frame_id, hdr->stamp);
    } else if (d->transaction_pending) {
        LAGFX_WARN("CmdDisplayAck: displayID=%u frameID=%u "
                   "!= pending txn=%u (out-of-order ack — tolerating)",
                   display_id, frame_id, d->pending_transaction_id);
    } else {
        LAGFX_LOG("CmdDisplayAck: displayID=%u frameID=%u stamp=0x%08x "
                  "(no pending txn to match — idempotent)",
                  display_id, frame_id, hdr->stamp);
    }

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * CmdDisplaySwapMapping (0x12) — scanout buffer install
 *
 * Sent by WindowServer during IOFBSetDisplayModeAndDepth to bind
 * an IOSurface's GPU VA as the active scanout buffer.
 * DisplayPipe::submitTransaction() emits this on the display vchan.
 *
 * RE confirmed (DisplayPipe-submitTransaction-opcodes.md):
 *   Wire form (32 B): {displayID, bufferVA, stride, w, h, fmt, flags}
 *   Fixture form (40 B): adds mappingID + explicit length
 *
 * The host must capture bufferVA+length and use them for subsequent
 * present calls (opcode 0x06/0x07). Without this handler,
 * the framebuffer has no backing store → black screen.
 *
 * Shape discrimination by payload_size: ≥40 = fixture, else wire.
 * Both decode into lagfx_display_entry_t.
 * ================================================================ */

#define LAGFX_DISPLAY_SWAP_V2_BYTES 32u
#define LAGFX_DISPLAY_SWAP_V1_BYTES 40u

lagfx_handler_status_t lagfx_op_display_swap_mapping(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload ||
        hdr->payload_size < LAGFX_DISPLAY_SWAP_V2_BYTES) {
        LAGFX_WARN("CmdDisplaySwapMapping: payload missing or too small "
                   "(size=%u, need >= %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_DISPLAY_SWAP_V2_BYTES);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id;
    uint32_t mapping_id;
    uint64_t buffer_va;
    uint64_t length;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t flags = 0;

    if (hdr->payload_size >= LAGFX_DISPLAY_SWAP_V1_BYTES) {
        /* Phase 2.A fixture form — preserves mappingID + explicit
         * length that the existing tests drive. */
        display_id = lagfx_le32(hdr->payload + 0);
        mapping_id = lagfx_le32(hdr->payload + 4);
        buffer_va  = lagfx_le64(hdr->payload + 8);
        length     = lagfx_le64(hdr->payload + 16);
        width      = lagfx_le32(hdr->payload + 24);
        height     = lagfx_le32(hdr->payload + 28);
        stride     = lagfx_le32(hdr->payload + 32);
        format     = lagfx_le32(hdr->payload + 36);
    } else {
        /* §14.3.2 real wire form. Implicit mapping cookie: bump a
         * per-display counter so callers can still correlate swaps. */
        display_id = lagfx_le32(hdr->payload + 0);
        buffer_va  = lagfx_le64(hdr->payload + 4);
        stride     = lagfx_le32(hdr->payload + 12);
        width      = lagfx_le32(hdr->payload + 16);
        height     = lagfx_le32(hdr->payload + 20);
        format     = lagfx_le32(hdr->payload + 24);
        flags      = lagfx_le32(hdr->payload + 28);
        length     = (uint64_t)stride * (uint64_t)height;
        mapping_id = 0u; /* filled below */
    }

    lagfx_display_entry_t *d =
        lagfx_protocol_get_or_alloc_display(p, display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplaySwapMapping: display table full (max=%u), "
                   "displayID=%u",
                   LAGFX_PROTO_MAX_DISPLAYS, display_id);
        return LAGFX_HANDLER_ERR_STATE;
    }

    d->live      = true;
    if (hdr->payload_size >= LAGFX_DISPLAY_SWAP_V1_BYTES) {
        d->mapping_id = mapping_id;
    } else {
        d->mapping_id = d->mapping_id + 1u;
        mapping_id = (uint32_t)d->mapping_id;
    }
    d->buffer_va = buffer_va;
    d->length    = length;
    d->width     = width;
    d->height    = height;
    d->stride    = stride;
    d->format    = format;
    d->mapped    = true;

    p->display_swaps_applied += 1;

    LAGFX_LOG("CmdDisplaySwapMapping: displayID=%u mappingID=%u "
              "bufferVA=0x%llx length=%llu %ux%u stride=%u fmt=%u "
              "flags=0x%x wire=%u stamp=0x%08x",
              display_id, mapping_id,
              (unsigned long long)buffer_va,
              (unsigned long long)length,
              width, height, stride, format, flags,
              (unsigned)hdr->payload_size, hdr->stamp);

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * CmdDisplayTransaction3 (0x16) — present/commit trigger
 *
 * Sent by WindowServer to commit a frame's layer list or attachment
 * list to the display pipe. This is the primary frame-present
 * path: after IOSurface swaps are mapped (0x12), the transaction
 * lists which surfaces go to which screen planes.
 *
 * RE confirmed (DisplayPipe-submitTransaction-opcodes.md):
 *   Layer-list form (§14.3.3): 16 B header + 0x2c B/layer
 *     Used by modern WindowServer for multi-plane compositing.
 *   Legacy form (Phase 2.A): 12 B header + 0x20 B/attachment
 *     Clear-color attachment form used by older paths.
 *
 * Shape discrimination by payload_size: the two forms have
 * different header sizes (16 vs 12) so they never collide.
 * count=0 is valid for both (empty transaction / no-op present).
 *
 * The transaction_pending flag gates the subsequent CmdDisplayAck (0x10)
 * which fires when the guest-side framebuffer work completes.
 * ================================================================ */

#define LAGFX_DISPLAY_TX_HEADER_LEGACY   12u
#define LAGFX_DISPLAY_TX_HEADER_LAYER    16u
#define LAGFX_DISPLAY_TX_ATTACH_BYTES    32u
#define LAGFX_DISPLAY_TX_LAYER_BYTES     0x2cu
#define LAGFX_DISPLAY_TX_MAX_ENTRIES     8u

static bool lagfx_tx_shape_fits(uint16_t payload_size,
                                uint16_t header_bytes,
                                uint16_t entry_bytes,
                                uint16_t max_n,
                                uint16_t *out_n) {
    if (payload_size < header_bytes) return false;
    uint32_t rem = (uint32_t)payload_size - (uint32_t)header_bytes;
    if (entry_bytes == 0) {
        if (rem == 0) { if (out_n) *out_n = 0; return true; }
        return false;
    }
    if ((rem % (uint32_t)entry_bytes) != 0) return false;
    uint32_t n = rem / (uint32_t)entry_bytes;
    if (n > (uint32_t)max_n) return false;
    if (out_n) *out_n = (uint16_t)n;
    return true;
}

static void lagfx_tx_parse_layer(const uint8_t *layer, uint32_t i) {
    uint32_t surface   = lagfx_le32(layer + 0x00);
    uint32_t src_x     = lagfx_le32(layer + 0x04);
    uint32_t src_y     = lagfx_le32(layer + 0x08);
    uint32_t src_w     = lagfx_le32(layer + 0x0c);
    uint32_t src_h     = lagfx_le32(layer + 0x10);
    uint32_t dst_x     = lagfx_le32(layer + 0x14);
    uint32_t dst_y     = lagfx_le32(layer + 0x18);
    uint32_t dst_w     = lagfx_le32(layer + 0x1c);
    uint32_t dst_h     = lagfx_le32(layer + 0x20);
    uint32_t fmt_blend = lagfx_le32(layer + 0x24);
    uint32_t z_order   = lagfx_le32(layer + 0x28);
    LAGFX_LOG("CmdDisplayTransaction3: layer[%u] surface=0x%x "
              "src=(%u,%u,%u,%u) dst=(%u,%u,%u,%u) fmt/blend=0x%x z=%u",
              i, surface, src_x, src_y, src_w, src_h,
              dst_x, dst_y, dst_w, dst_h, fmt_blend, z_order);
}

static void lagfx_tx_parse_attachment(const uint8_t *a, uint32_t i,
                                      lagfx_display_entry_t *d) {
    uint32_t idx          = lagfx_le32(a + 0x00);
    uint32_t load_action  = lagfx_le32(a + 0x04);
    uint32_t store_action = lagfx_le32(a + 0x08);
    uint32_t flags        = lagfx_le32(a + 0x0c);
    float    r  = lagfx_lef32(a + 0x10);
    float    g  = lagfx_lef32(a + 0x14);
    float    b  = lagfx_lef32(a + 0x18);
    float    al = lagfx_lef32(a + 0x1c);
    LAGFX_LOG("CmdDisplayTransaction3: attach[%u] idx=%u load=%u "
              "store=%u flags=0x%x clear=(%f,%f,%f,%f)",
              i, idx, load_action, store_action, flags,
              (double)r, (double)g, (double)b, (double)al);
    if (i == 0 && d) {
        d->last_load_action   = (uint8_t)(load_action & 0xff);
        d->last_clear_rgba[0] = r;
        d->last_clear_rgba[1] = g;
        d->last_clear_rgba[2] = b;
        d->last_clear_rgba[3] = al;
    }
}

lagfx_handler_status_t lagfx_op_display_transaction3(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < LAGFX_DISPLAY_TX_HEADER_LEGACY) {
        LAGFX_WARN("CmdDisplayTransaction3: payload missing or too small "
                   "(size=%u, need >= %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_DISPLAY_TX_HEADER_LEGACY);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint16_t legacy_n = 0;
    uint16_t layer_n  = 0;
    bool fits_legacy = lagfx_tx_shape_fits(
        hdr->payload_size,
        LAGFX_DISPLAY_TX_HEADER_LEGACY,
        LAGFX_DISPLAY_TX_ATTACH_BYTES,
        LAGFX_DISPLAY_TX_MAX_ENTRIES,
        &legacy_n);
    bool fits_layer = lagfx_tx_shape_fits(
        hdr->payload_size,
        LAGFX_DISPLAY_TX_HEADER_LAYER,
        LAGFX_DISPLAY_TX_LAYER_BYTES,
        LAGFX_DISPLAY_TX_MAX_ENTRIES,
        &layer_n);

    if (!fits_legacy && !fits_layer) {
        LAGFX_WARN("CmdDisplayTransaction3: payload_size=%u matches neither "
                   "legacy (12 + 32*n) nor layer (16 + 44*n) shape",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Prefer layer form when it fits with >=1 layer (§14.3.3 real
     * emission). When both fit at n=0 we go legacy — that preserves
     * the (displayID, txID, count=0) header read the Phase 2.A
     * tests drive. */
    bool use_layer = fits_layer && (!fits_legacy || layer_n > 0);

    uint32_t display_id;
    uint32_t transaction_id;
    uint32_t entry_count;
    uint32_t flags = 0;
    if (use_layer) {
        transaction_id = lagfx_le32(hdr->payload + 0);
        display_id     = lagfx_le32(hdr->payload + 4);
        entry_count    = lagfx_le32(hdr->payload + 8);
        flags          = lagfx_le32(hdr->payload + 12);
        if (entry_count != layer_n) {
            LAGFX_WARN("CmdDisplayTransaction3: declared layerCount=%u "
                       "!= size-implied n=%u; trusting size-implied",
                       entry_count, layer_n);
            entry_count = layer_n;
        }
    } else {
        display_id     = lagfx_le32(hdr->payload + 0);
        transaction_id = lagfx_le32(hdr->payload + 4);
        entry_count    = lagfx_le32(hdr->payload + 8);
        if (entry_count != legacy_n) {
            LAGFX_WARN("CmdDisplayTransaction3: declared attachmentCount=%u "
                       "!= size-implied n=%u; trusting size-implied",
                       entry_count, legacy_n);
            entry_count = legacy_n;
        }
    }

    lagfx_display_entry_t *d =
        lagfx_protocol_get_or_alloc_display(p, display_id);
    if (!d) {
        LAGFX_WARN("CmdDisplayTransaction3: display table full (max=%u), "
                   "displayID=%u",
                   LAGFX_PROTO_MAX_DISPLAYS, display_id);
        return LAGFX_HANDLER_ERR_STATE;
    }
    d->live = true;

    /* last_attachment_count pulls double duty as "last entry count"
     * so state.h need not be extended to carry layer-vs-attachment
     * kind (keeping state.h outside the write-set). */
    d->last_attachment_count = entry_count;
    d->last_load_action      = 0;
    d->last_clear_rgba[0]    = 0.f;
    d->last_clear_rgba[1]    = 0.f;
    d->last_clear_rgba[2]    = 0.f;
    d->last_clear_rgba[3]    = 0.f;

    if (use_layer) {
        for (uint32_t i = 0; i < entry_count; ++i) {
            const uint8_t *layer = hdr->payload
                                 + LAGFX_DISPLAY_TX_HEADER_LAYER
                                 + (size_t)i * LAGFX_DISPLAY_TX_LAYER_BYTES;
            lagfx_tx_parse_layer(layer, i);
        }
    } else {
        for (uint32_t i = 0; i < entry_count; ++i) {
            const uint8_t *a = hdr->payload
                             + LAGFX_DISPLAY_TX_HEADER_LEGACY
                             + (size_t)i * LAGFX_DISPLAY_TX_ATTACH_BYTES;
            lagfx_tx_parse_attachment(a, i, d);
        }
    }

    d->pending_transaction_id = transaction_id;
    d->transaction_pending    = true;
    d->transaction_acked      = false;

    p->display_transactions_submitted += 1;

    /* Phase 2.B clear-path side-effect. Only the legacy fixture form
     * carries a clear RGBA; real §14.3.3 layer transactions composite
     * pre-rendered IOSurfaces so no clear value lands here. */
    if (d->last_load_action == 2u && p->dev != NULL) {
        lagfx_display_t *disp = NULL;
        for (size_t i = 0; i < LAGFX_MAX_DISPLAYS; ++i) {
            if (p->dev->displays[i] != NULL) {
                disp = p->dev->displays[i];
                break;
            }
        }
        if (disp != NULL) {
            lagfx_status_t st = lagfx_display_submit_clear_color(
                disp, d->last_clear_rgba, d->buffer_va, d->length);
            if (st != LAGFX_OK) {
                LAGFX_WARN("CmdDisplayTransaction3: submit_clear_color "
                           "failed (%d) — continuing without frame",
                           (int)st);
            }
        } else {
            LAGFX_LOG("CmdDisplayTransaction3: no display attached to "
                      "device — clear trigger skipped");
        }
    }

    LAGFX_LOG("CmdDisplayTransaction3: displayID=%u transactionID=%u "
              "%s=%u flags=0x%x stamp=0x%08x",
              display_id, transaction_id,
              use_layer ? "layers" : "attachments",
              entry_count, flags, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * Cursor + shared-state handlers (M6 — wired and active)
 *
 * RE note (cursor-rendering-stage-20.md):
 *   0x13 CmdDisplayCursorShow / 0x14 CmdDisplayCursorGlyph are
 *   fully wired: each handler parses its payload and invokes
 *   the corresponding display callback (cursor_show, cursor_moved,
 *   cursor_glyph). These callbacks land in QEMU at:
 *     - apple_gfx_cursor_glyph()   (apple-gfx-common-linux.c:544)
 *     - apple_gfx_cursor_moved()   (apple-gfx-common-linux.c:605)
 *     - apple_gfx_cursor_show()    (apple-gfx-common-linux.c:625)
 *
 * Without these callbacks, the guest's hardware-cursor path falls
 * back to software-cursor rendering into the main framebuffer
 * (lag, smear, or disappearance under Metal composition —
 * paravirt-re/re-followup-spec-gaps.md:2012-2017).
 *
 * 0x17 (CmdDisplaySetSharedStatePage) is wired — it installs
 * the vblank counter page that WindowServer polls for vsync.
 *
 * Storage: handler-captured state in file-scope static.
 * Phase 2 single-device assumption.
 * ================================================================ */

static lagfx_cursor_show_state_t       g_cursor_show        = {0};
static lagfx_cursor_glyph_state_t      g_cursor_glyph       = {0};
static lagfx_shared_state_t            g_shared_state       = {0};
static lagfx_compositor_params_state_t g_compositor_params  = {0};
static lagfx_icc_profile_state_t       g_icc_profile        = {0};

const lagfx_cursor_show_state_t *lagfx_ops_display_last_cursor_show(void) {
    return &g_cursor_show;
}

const lagfx_cursor_glyph_state_t *lagfx_ops_display_last_cursor_glyph(void) {
    return &g_cursor_glyph;
}

const lagfx_shared_state_t *lagfx_ops_display_shared_state(void) {
    return &g_shared_state;
}

const lagfx_compositor_params_state_t *
lagfx_ops_display_last_compositor_params(void) {
    return &g_compositor_params;
}

const lagfx_icc_profile_state_t *
lagfx_ops_display_last_icc_profile(void) {
    return &g_icc_profile;
}

void lagfx_ops_display_reset(void) {
    memset(&g_cursor_show,       0, sizeof(g_cursor_show));
    memset(&g_cursor_glyph,      0, sizeof(g_cursor_glyph));
    memset(&g_shared_state,      0, sizeof(g_shared_state));
    memset(&g_compositor_params, 0, sizeof(g_compositor_params));
    memset(&g_icc_profile,       0, sizeof(g_icc_profile));
}

bool lagfx_ops_display_tick_vblank(
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *)) {
    if (!g_shared_state.installed) {
        return false;
    }
    g_shared_state.vblank_counter += 1u;
    if (write_memory == NULL) {
        return false;
    }
    uint8_t buf[4];
    lagfx_put_le32(buf, g_shared_state.vblank_counter);
    return write_memory(shell_opaque, g_shared_state.page_va,
                        sizeof(buf), buf);
}

/* ONLINE EVENT 2026-05-03: fires when ss[0x104]==0xC after adaptive polling.
 *
 * ROOT CAUSE (confirmed via RE):
 *   IOServiceOpen(type=5) hangs when called from main thread after displays are online
 *   Early boot processes (WindowServer, SecurityAgent, BluetoothSetupAs) succeed
 *   Post-boot processes (metal-test, system_profiler) hang at mach_msg2_trap
 *   Hang occurs because lifecycle lock (accel[+0x88]) is held by connectionChange
 *     during display pipeline's process_online path when new device creation tries to acquire it
 *
 * FIX: Adaptive polling loop waiting for CmdDefineChildFIFO commands.
 *   Instead of fixed 5s sleep, wait until WindowServer starts sending CmdDefineChildFIFO.
 *   Poll every ~500ms (30 ticks at 60Hz) with 60s timeout to prevent boot hang.
 *   The first valid CmdDefineChildFIFO signals device creation complete.
 *
 * Implementation: Track CmdDefineChildFIFO arrivals via flag in ops_queue.c.
 *   Once we see >= 1 valid command, fire online event immediately (not delayed).
 *   This is more precise than arbitrary time-based delay. */
bool lagfx_display_tick_vblank(
    lagfx_device_t *dev,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
    bool (*read_memory)(void *, uint64_t, uint64_t, void *)) {
    
    if (!dev || !write_memory || !read_memory) {
        return false;
    }

    bool irq_raised = false;

    /* Check each display for enable() completion (ss[+0x104] == 0xC).
     * CRITICAL FIX: Fire online IRQ IMMEDIATELY when guest enables display.
     * 
     * DEADLOCK FIX (2026-05-03): Previous attempts deferred or threshold-counted
     * online events, all hitting chicken-and-egg problems:
     *   - 39a351c: defer 5s → WindowServer times out waiting for display
     *   - aa91185: wait 5 submits → same timeout issue  
     *   - f6e059b: threshold counting → still blocks on lock ordering
     * 
     * Correct solution: fire IRQ IMMEDIATELY when ss[+0x104]==0xC. This breaks
     * the ABBA cycle because WindowServer's doSetDisplayMode() can proceed
     * without waiting for DisplayPipe to complete its lock acquisition.
     *
     * See deadlock-fix-history.md (#12) and paravirt-re/library/online-event-simplification.md
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

                LAGFX_LOG("display_tick_vblank: display[%u] enabled (ss[+0x104]=0xC), "
                          "raised IRQ vec=0 IMMEDIATELY", i);
            } else {
                LAGFX_WARN("display_tick_vblank: display[%u] enable() but write_memory failed", i);
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

#define LAGFX_CURSOR_SHOW_PAYLOAD_BYTES 16u
lagfx_handler_status_t lagfx_op_display_cursor_show(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload ||
        hdr->payload_size < LAGFX_CURSOR_SHOW_PAYLOAD_BYTES) {
        LAGFX_WARN("CmdDisplayCursorShow: payload missing or too small "
                   "(size=%u, need %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_CURSOR_SHOW_PAYLOAD_BYTES);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    int16_t  x          = (int16_t)lagfx_le16(hdr->payload + 4);
    int16_t  y          = (int16_t)lagfx_le16(hdr->payload + 6);
    uint32_t visible    = lagfx_le32(hdr->payload + 8);
    uint32_t hotpack    = lagfx_le32(hdr->payload + 12);
    uint16_t hot_x      = (uint16_t)((hotpack >> 16) & 0xffffu);
    uint16_t hot_y      = (uint16_t)(hotpack & 0xffffu);

    g_cursor_show.valid      = true;
    g_cursor_show.display_id = display_id;
    g_cursor_show.x          = x;
    g_cursor_show.y          = y;
    g_cursor_show.visible    = visible;
    g_cursor_show.hot_x      = hot_x;
    g_cursor_show.hot_y      = hot_y;

    /* Invoke QEMU cursor callbacks so the cursor becomes visible via noVNC.
     * The callbacks were registered in apple_gfx_common_realize()
     * (apple-gfx-common-linux.c:697-702) and land at:
     *   cursor_moved → apple_gfx_cursor_moved()   (line 605)
     *   cursor_show  → apple_gfx_cursor_show()    (line 625)
     *
     * Find the first live display on the device and call its callbacks. */
    if (p->dev) {
        for (int i = 0; i < LAGFX_MAX_DISPLAYS; i++) {
            if (p->dev->displays[i]) {
                lagfx_display_t *disp = p->dev->displays[i];
                if (disp->desc.callbacks.cursor_moved) {
                    /* Update cursor position in display state */
                    disp->cursor_pos.x = (uint16_t)(int)x;
                    disp->cursor_pos.y = (uint16_t)(int)y;
                    disp->desc.callbacks.cursor_moved(
                        disp->desc.callbacks.opaque);
                }
                if (disp->desc.callbacks.cursor_show) {
                    disp->desc.callbacks.cursor_show(
                        disp->desc.callbacks.opaque,
                        visible ? true : false);
                }
                break;
            }
        }
    }

    LAGFX_LOG("CmdDisplayCursorShow: displayID=%u pos=(%d,%d) visible=%u "
               "hot=(%u,%u) stamp=0x%08x",
               display_id, (int)x, (int)y, visible,
               (unsigned)hot_x, (unsigned)hot_y, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ----------------------------------------------------------------
 * 0x14 CmdDisplayCursorGlyph — §14.4
 *
 *   +0x00  u32 displayID
 *   +0x04  u64 glyphVA          (task-mapped; ARGB8888 pixels)
 *   +0x0c  u32 width
 *   +0x10  u32 height
 *   +0x14  u32 bytes_per_row
 *   +0x18  u32 hot_x
 *   +0x1c  u32 hot_y
 *   = 32 B total
 *
 * Captures pixels via shell.read_memory into g_cursor_glyph.bytes,
 * capped at LAGFX_CURSOR_GLYPH_MAX_BYTES to defend against a
 * malformed / malicious guest.
 * ---------------------------------------------------------------- */

#define LAGFX_CURSOR_GLYPH_PAYLOAD_BYTES 32u

lagfx_handler_status_t lagfx_op_display_cursor_glyph(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload ||
        hdr->payload_size < LAGFX_CURSOR_GLYPH_PAYLOAD_BYTES) {
        LAGFX_WARN("CmdDisplayCursorGlyph: payload missing or too small "
                   "(size=%u, need %u)",
                   (unsigned)hdr->payload_size,
                   LAGFX_CURSOR_GLYPH_PAYLOAD_BYTES);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id    = lagfx_le32(hdr->payload + 0);
    uint64_t glyph_va      = lagfx_le64(hdr->payload + 4);
    uint32_t width         = lagfx_le32(hdr->payload + 12);
    uint32_t height        = lagfx_le32(hdr->payload + 16);
    uint32_t bytes_per_row = lagfx_le32(hdr->payload + 20);
    uint32_t hot_x         = lagfx_le32(hdr->payload + 24);
    uint32_t hot_y         = lagfx_le32(hdr->payload + 28);

    g_cursor_glyph.valid         = true;
    g_cursor_glyph.display_id    = display_id;
    g_cursor_glyph.glyph_va      = glyph_va;
    g_cursor_glyph.width         = width;
    g_cursor_glyph.height        = height;
    g_cursor_glyph.bytes_per_row = bytes_per_row;
    g_cursor_glyph.hot_x         = hot_x;
    g_cursor_glyph.hot_y         = hot_y;
    g_cursor_glyph.captured_len  = 0;

    uint64_t total_bytes = (uint64_t)bytes_per_row * (uint64_t)height;
    if (total_bytes == 0 || total_bytes > LAGFX_CURSOR_GLYPH_MAX_BYTES) {
        LAGFX_WARN("CmdDisplayCursorGlyph: skipping glyph DMA (size=%llu "
                   "out-of-range, cap=%u)",
                   (unsigned long long)total_bytes,
                   (unsigned)LAGFX_CURSOR_GLYPH_MAX_BYTES);
    } else if (p->dev != NULL && p->dev->desc.shell.read_memory != NULL) {
        if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                           glyph_va,
                                           total_bytes,
                                           g_cursor_glyph.bytes)) {
            g_cursor_glyph.captured_len = (size_t)total_bytes;
        } else {
            LAGFX_WARN("CmdDisplayCursorGlyph: read_memory failed at "
                       "glyphVA=0x%llx len=%llu — pixels unavailable",
                       (unsigned long long)glyph_va,
                       (unsigned long long)total_bytes);
        }
    }

    /* Invoke QEMU cursor_glyph callback so the cursor image updates.
     * The callback was registered in apple_gfx_common_realize()
     * (apple-gfx-common-linux.c:697-702) and lands at:
     *   cursor_glyph → apple_gfx_cursor_glyph()   (line 544)
     * which converts BGRA→RGBA and calls dpy_cursor_define().
     *
     * Find the first live display on the device and call its callback. */
    if (g_cursor_glyph.captured_len > 0 && p->dev) {
        for (int i = 0; i < LAGFX_MAX_DISPLAYS; i++) {
            if (p->dev->displays[i]) {
                lagfx_display_t *disp = p->dev->displays[i];
                if (disp->desc.callbacks.cursor_glyph) {
                    lagfx_coord_t hotspot = {
                        .x = (uint16_t)hot_x,
                        .y = (uint16_t)hot_y
                    };
                    disp->desc.callbacks.cursor_glyph(
                        disp->desc.callbacks.opaque,
                        g_cursor_glyph.bytes,
                        width, height, hotspot);
                }
                break;
            }
        }
    }

    LAGFX_LOG("CmdDisplayCursorGlyph: displayID=%u glyphVA=0x%llx %ux%u "
               "bpr=%u hot=(%u,%u) captured=%zu bytes stamp=0x%08x",
               display_id,
               (unsigned long long)glyph_va,
               width, height, bytes_per_row, hot_x, hot_y,
               g_cursor_glyph.captured_len, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ----------------------------------------------------------------
 * 0x17 CmdDisplaySetSharedStatePage — §14.6
 *
 *   +0x00  u64 pageVA    (guest GPA of a 4 KiB mailbox page)
 *   = 8 B total
 *
 * On registration:
 *   1. Record the mailbox GPA.
 *   2. Best-effort zero the first 64 bytes of the page.
 *   3. Kick one vblank tick so WindowServer never polls a zero
 *      counter after attach — prevents the spin-or-degrade failure
 *      mode called out in §14.6.
 *
 * Subsequent ticks are driven by tick_vblank_counter_only — the counter bump only. */

lagfx_handler_status_t lagfx_op_display_set_shared_page(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr || !hdr->payload || hdr->payload_size < 8u) {
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint64_t page_va = lagfx_le64(hdr->payload);

    g_shared_state.installed   = true;
    g_shared_state.page_va     = page_va;
    g_shared_state.vblank_counter = 0u;

    /* Best-effort zero the first 64 bytes of the mailbox page. */
    if (p->dev != NULL && p->dev->desc.shell.write_memory != NULL) {
        static const uint8_t zeros[64] = {0};
        (void)p->dev->desc.shell.write_memory(
            p->dev->desc.shell.opaque, page_va, sizeof(zeros), zeros);
        lagfx_ops_display_tick_vblank(p->dev->desc.shell.opaque, p->dev->desc.shell.write_memory, NULL);
    } else {
        /* Shadow-only kick so "counter > 0" checks succeed before
         * any DMA path lands. */
        g_shared_state.vblank_counter = 1u;
    }

    LAGFX_LOG("CmdDisplaySetSharedStatePage: pageVA=0x%llx installed; "
              "vblank_counter=%u stamp=0x%08x",
              (unsigned long long)page_va,
              g_shared_state.vblank_counter,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * 0x19 CmdDisplayCompositorParameters — §14 M6 readiness (log-only)
 *
 * WindowServer passes layer-blend + gamma-related settings once per
 * mode-set.  §14.10 classifies colour accuracy as cosmetic through
 * M6, so we only need to (a) avoid the "unknown opcode" default
 * handler log spam, (b) retain the raw payload for the §14.8
 * instrumentation pass so the conjectured layout can be validated
 * against a real capture, and (c) ack the stamp.
 *
 * Conjectured shape (best-effort; we don't gate on it):
 *   +0x00  u32 display_id
 *   +0x04  ... layer-blend + gamma-table descriptor, variable
 * ================================================================ */

lagfx_handler_status_t lagfx_op_display_compositor_params(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    g_compositor_params.valid          = true;
    g_compositor_params.dispatch_count += 1;
    g_compositor_params.last_stamp     = hdr->stamp;
    g_compositor_params.display_id     = 0;
    g_compositor_params.payload_size   = hdr->payload_size;

    uint32_t to_copy = hdr->payload_size;
    if (to_copy > LAGFX_COMPOSITOR_PARAMS_CAPTURE_MAX) {
        to_copy = LAGFX_COMPOSITOR_PARAMS_CAPTURE_MAX;
    }
    g_compositor_params.captured_len = to_copy;
    if (to_copy > 0 && hdr->payload != NULL) {
        memcpy(g_compositor_params.bytes, hdr->payload, to_copy);
    }
    if (hdr->payload && hdr->payload_size >= 4) {
        g_compositor_params.display_id = lagfx_le32(hdr->payload + 0);
    }

    LAGFX_LOG("CmdDisplayCompositorParameters: displayID=%u payload_size=%u "
              "stamp=0x%08x (M6 log-only; §14.10 cosmetic)",
              g_compositor_params.display_id,
              (unsigned)hdr->payload_size,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * 0x1a CmdDisplaySetGuestICCProfile — §14 M6 readiness (log-only)
 *
 * Guest uploads an ICC colour-management profile. §14.10 classifies
 * as cosmetic; log + ack. Conjectured shape:
 *   +0x00  u32 display_id
 *   +0x04  u32 profile_size   (bytes of the ICC blob, task-addressable)
 *   +0x08  u64 profile_va     (task-mapped GPA of the blob)
 *
 * We retain the first N bytes of the payload (not the profile blob
 * itself — that reaches guest RAM via a separate DMA path we're not
 * wiring at M6).
 * ================================================================ */

lagfx_handler_status_t lagfx_op_display_set_icc_profile(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    g_icc_profile.valid          = true;
    g_icc_profile.dispatch_count += 1;
    g_icc_profile.last_stamp     = hdr->stamp;
    g_icc_profile.display_id     = 0;
    g_icc_profile.profile_va     = 0;
    g_icc_profile.profile_size   = 0;
    g_icc_profile.payload_size   = hdr->payload_size;

    uint32_t to_copy = hdr->payload_size;
    if (to_copy > LAGFX_ICC_PROFILE_CAPTURE_MAX) {
        to_copy = LAGFX_ICC_PROFILE_CAPTURE_MAX;
    }
    g_icc_profile.captured_len = to_copy;
    if (to_copy > 0 && hdr->payload != NULL) {
        memcpy(g_icc_profile.bytes, hdr->payload, to_copy);
    }
    if (hdr->payload && hdr->payload_size >= 4) {
        g_icc_profile.display_id = lagfx_le32(hdr->payload + 0);
    }
    if (hdr->payload && hdr->payload_size >= 8) {
        g_icc_profile.profile_size = lagfx_le32(hdr->payload + 4);
    }
    if (hdr->payload && hdr->payload_size >= 16) {
        g_icc_profile.profile_va = lagfx_le64(hdr->payload + 8);
    }

    LAGFX_LOG("CmdDisplaySetGuestICCProfile: displayID=%u profile_size=%u "
              "profile_va=0x%llx payload_size=%u stamp=0x%08x "
              "(M6 log-only; §14.10 cosmetic)",
              g_icc_profile.display_id,
              g_icc_profile.profile_size,
              (unsigned long long)g_icc_profile.profile_va,
              (unsigned)hdr->payload_size,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}
