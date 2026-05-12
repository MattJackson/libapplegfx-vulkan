/*
 * libapplegfx-vulkan — protocol decoder lifecycle + dispatcher
 * src/protocol/protocol.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Decoder lifecycle, MMIO register shadow + per-channel doorbell
 * (BAR0+0x1020), and the per-cmd dispatch + stamp-completion path.
 * Tests drive dispatch via lagfx_protocol_dispatch_one directly.
 *
 * RE file references (paravirt-re/):
 *   - PROTOCOL.md: MMIO map, stamp-cell mechanics, doorbell protocol
 *   - PGFIFO-sub-channel-opcode-table.md: display vchan namespace
 *   - waitForStamp-mechanism-summary.md: stamp-cell + signalStamps
 *   - re-followup-spec-gaps.md: ss[0x104] polling, per-channel
 *     doorbell descriptor layout at shared_pfn<<12 + 0x400
 *   - cursor-rendering-stage-20.md: 0x13/0x14 cursor opcodes
 *   - display-pipe-enable-online-sequence.md: cursor wiring gap (§Q6)
 *   - CmdDefineChildFIFO-display-vchan-analysis.md: 44-byte descriptor
 *   - flows/display-init-flow.md, flows/display-swap-flow.md
 *
 * Channel allocation note: IOAccelerator creates channels 1-2
 * (compute) and 5+ (display); channel 3 is never opened by the
 * guest because macOS only uses 2 compute channels for this GPU
 * configuration.
 */

#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "fifo.h"
#include "ops_display.h"
#include "../device.h"
#include "../common/log.h"
#include "../dispatchers/base.h"
#include "../dispatchers/registry.h"
#include "../dispatchers/channel_0_dispatcher.h"
#include "../dispatchers/channel_1_dispatcher.h"
#include "../dispatchers/channel_2_dispatcher.h"
#include "../dispatchers/channel_3_dispatcher.h"
#include "../dispatchers/channel_4_dispatcher.h"
#include "../dispatchers/channel_5_plus_dispatcher.h"
#include "../dispatchers/unknown_dispatcher.h"

#include <stdlib.h>
#include <string.h>

/* Bounce buffer for doorbell payload reads — prevents malloc in hot path.
 * Max command size from guest is bounded by ring_size (64 KiB). Use 32 KiB
 * as a practical cap that covers all legitimate macOS commands while
 * preventing DoS via repeated large allocations. */
#define LAGFX_DOORBELL_BOUNCE_BUFFER_SIZE 32768u
/* Per-instance bounce buffer moved to lagfx_protocol_t in state.h:65536u
 * to avoid concurrent MMIO corruption across devices/threads. */

/* Little-endian u32 read with no alignment / strict-aliasing assumptions.
 * Use for any decode of a 4-byte field out of an arbitrary byte buffer. */
static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* === Lifecycle ============================================== */

lagfx_protocol_t *lagfx_protocol_new(struct lagfx_device *dev) {
    if (!lagfx_device_is_valid(dev)) {
        LAGFX_ERR("protocol_new: invalid device %p", (void *)dev);
        return NULL;
    }

    lagfx_protocol_t *p =
        (lagfx_protocol_t *)calloc(1, sizeof(*p));
    if (!p) {
        LAGFX_ERR("protocol_new: out of memory");
        return NULL;
    }

    p->magic = LAGFX_PROTOCOL_MAGIC;
    p->dev   = dev;

    /* Initialize dispatcher registry on first protocol creation */
    {
        static int initialized = 0;
        if (!initialized) {
            lagfx_dispatcher_registry_init();
            initialized = 1;
        }
    }

    /* Register defaults. Bit 0 of STATUS_CONTROL = "present", bit 1
     * = "ready" per §3.1 of the brief (inferred). */
    p->reg[0] = 0x3u;  /* STATUS_CONTROL */

    LAGFX_LOG("protocol_new: p=%p dev=%p", (void *)p, (void *)dev);
    return p;
}

void lagfx_protocol_free(lagfx_protocol_t *p) {
    if (!p) {
        return;
    }
    if (p->magic != LAGFX_PROTOCOL_MAGIC) {
        LAGFX_ERR("protocol_free: bad magic on %p (got 0x%08x)",
                  (void *)p, p->magic);
        return;
    }
    LAGFX_LOG("protocol_free: p=%p (seen=%llu, completed=%llu, unknown=%llu)",
              (void *)p,
              (unsigned long long)p->total_cmds_seen,
              (unsigned long long)p->total_cmds_completed,
              (unsigned long long)p->unknown_opcode_count);
    memset(p, 0, sizeof(*p));
    free(p);
}

void lagfx_protocol_reset(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    LAGFX_LOG("protocol_reset: p=%p", (void *)p);

    /* Clear tables and inflight; preserve ring geometry and registers. */
    memset(p->tasks,    0, sizeof(p->tasks));
    memset(p->fifos,    0, sizeof(p->fifos));
    memset(p->inflight, 0, sizeof(p->inflight));
    memset(p->displays, 0, sizeof(p->displays));
    memset(p->display_child_rings, 0, sizeof(p->display_child_rings));
    memset(&p->resources, 0, sizeof(p->resources));

    p->total_cmds_seen      = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->interrupts_raised    = 0;
    p->last_completed_stamp = 0;
    p->current_task_id      = 1; /* Default to root channel */
    p->read_ptr             = 0;
    p->write_ptr            = 0;
    p->pending_stamps_bitmask = 0;

    p->display_swaps_applied          = 0;
    p->display_transactions_submitted = 0;
    p->display_acks_received          = 0;
    p->display_submit_count           = 0;
    p->display_1e_logged              = false;

    /* Per-protocol diagnostic counters previously held in function-local
     * statics — now reset properly across reconnects. */
    memset(p->log_suppress, 0, sizeof(p->log_suppress));
    memset(p->ch_drained,   0, sizeof(p->ch_drained));

    /* Display-handler captures (canonical per-protocol copies). The
     * legacy file-scope static mirror in ops_display.c is cleared via
     * lagfx_ops_display_reset() when tests need a fresh slate. */
    memset(&p->cursor_show,       0, sizeof(p->cursor_show));
    memset(&p->cursor_glyph,      0, sizeof(p->cursor_glyph));
    memset(&p->shared_state,      0, sizeof(p->shared_state));
    memset(&p->compositor_params, 0, sizeof(p->compositor_params));
    memset(&p->icc_profile,       0, sizeof(p->icc_profile));

    /* IOSurface capture counters. */
    memset(&p->cap_counters, 0, sizeof(p->cap_counters));

    /* Queue / render-pass scratch state. */
    p->cmd_define_fifo_called = false;
    memset(&p->last_render_pass_desc, 0, sizeof(p->last_render_pass_desc));
}

/* === Completion path ========================================
 *
 * Every command unconditionally signals its stamp when done.
 *
 * RE (waitForStamp-mechanism-summary.md):
 *   - waitForStamp(slot, target) parks thread until
 *     *stampBases[slot] >= target (bounded ~5 s total).
 *   - stampBases[slot] lives in DMA-visible memory at
 *     (ring_base_pfn << 12) + slot*4.
 *   - Kext ISR reads BAR0+0x1018 (stamp bitmask), feeds to
 *     signalStamps() which calls commandWakeup(slot) per bit.
 *
 * Per-completion work:
 *   1. Monotonically advance *stampBases[slot] via
 *      lagfx_advance_stamp_cell() — never regresses (floor=1).
 *   2. OR bit `slot` into pending_stamps_bitmask.
 *   3. Raise MSI vec 0 (unified ISR on vec 0).
 *
 * Slot mapping:
 *     0 = RootChannel, 1..4 = compute vchans,
 *     5..12 = display pipes (display_index + 5).
 *     See waitForStamp-mechanism-summary.md for full table.
 */

/* Monotonic stamp-cell advance — never regresses. Reads current cell,
 * writes max(target, cur+1), with a 1 floor (never 0). All callers
 * that update a stamp cell MUST go through this helper, otherwise a
 * stale write < current value will park the kext until the bounded
 * 1-second deadline kicks in (waitForStamp-deadline-semantics.md). */
void lagfx_advance_stamp_cell(lagfx_protocol_t *p,
                               uint32_t slot,
                               uint32_t target_stamp) {
    if (!p || !p->dev || !p->dev->desc.shell.write_memory
        || p->ring_base_pfn == 0u) {
        return;
    }
    uint64_t cell_gpa = ((uint64_t)p->ring_base_pfn << 12)
                        + (uint64_t)slot * 4u;
    uint32_t cur = 0u;
    if (p->dev->desc.shell.read_memory) {
        p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                       cell_gpa, sizeof(cur), &cur);
    }
    /* max(target, cur+1) with a non-zero floor. */
    uint32_t want = (target_stamp > cur + 1u) ? target_stamp : (cur + 1u);
    if (want == 0u) {
        want = 1u;
    }
    if (p->dev->desc.shell.write_memory(
            p->dev->desc.shell.opaque,
            cell_gpa, sizeof(want), &want)) {
                LAGFX_LOG("stamp_cell[%u] := %u (was %u, target=%u, gpa=0x%llx)",
                  slot, want, cur, target_stamp,
                  (unsigned long long)cell_gpa);
    }
}

void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p,
                                        uint32_t slot,
                                        uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    lagfx_advance_stamp_cell(p, slot, stamp);

    p->pending_stamps_bitmask |= (1u << slot);

    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        /* The accelerator kext registers exactly one interrupt source
         * at MSI-X vector 0. A single unified ISR demuxes stamps /
         * displays / faults off BAR0 status regs. No separate
         * "stamp-interrupt" vector exists; vec 1 hits another kext's
         * ISR and breaks guest networking. Pin to vec 0. */
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
        p->interrupts_raised += 1;
        LAGFX_LOG("complete_stamp[slot=%u]: cmd_stamp=0x%08x + IRQ vec=0 "
                  "(pending_mask=0x%08x)",
                  slot, stamp, p->pending_stamps_bitmask);
    } else {
        LAGFX_LOG("complete_stamp[slot=%u]: cmd_stamp=0x%08x (no IRQ cb)",
                  slot, stamp);
    }
}

void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    /* RootChannel completions land on slot 0. Every init-phase
     * dispatcher-driven command emits `xor esi, esi` before writeStamp.
     * Doorbell-driven completions for per-channel rings (ch >= 1) call
     * lagfx_protocol_complete_stamp_slot directly with the per-channel
     * slot. */
    lagfx_protocol_complete_stamp_slot(p, 0u, stamp);
}

/* === Display sub-channel ring drain =========================
 *
 * Display vchan (ch 5..12) acts as a parent ring. After draining
 * it, we poll any registered child FIFO rings for pending work.
 *
 * RE (paravirt-re/PGFIFO-sub-channel-opcode-table.md,
 *     CmdDefineChildFIFO-*.md, display-pipe-enable-online-sequence.md):
 *   - Child rings use FULL PGFIFO dispatch (opcodes 0x00..0x44),
 *     NOT the display vchan's compact namespace {0x01,0x02,0x04,0x06,0x07}.
 *   - baseNode is 24 bytes at ring start (offset 0x00).
 *     Commands start at ring_base_gpa + 0x40.
 *   - produced/consumed indices (u32 at baseNode+0/+4) are
 *     command counts, NOT byte offsets.
 *
 * WindowServer creates 5 sub-channels per display (Display0)
 * via opcode 0x04 (44-byte descriptor, see
 * CmdDefineChildFIFO-display-vchan-analysis.md).
 * These carry the actual rendering commands:
 *   - 0x20 ExecIndirect2 (inner render/blit/compute opcodes)
 *   - 0x22 SynchronizeResources
 *   - 0x12 DisplaySwapMapping
 *   - See paravirt-re/command-buffer-format.md for format.
 *
 * Uses lagfx_dispatch_inner() for full PGFIFO dispatch per command.
 * Per-channel stamp slot = ch (5..12 = display_index + 5).
 * See waitForStamp-mechanism-summary.md for slot table.
 */

static void lagfx_drain_display_child_rings(lagfx_protocol_t *p,
                                             uint32_t *io_last_stamp) {
    if (!p || !p->dev || !p->dev->desc.shell.read_memory) {
        return;
    }

    for (unsigned ri = 0; ri < LAGFX_MAX_DISPLAY_CHILD_RINGS; ++ri) {
        lagfx_display_child_ring_t *ring = &p->display_child_rings[ri];
        if (!ring->live || ring->ring_base_gpa == 0u) {
            continue;
        }

        uint8_t basenode[64] = {0};
        if (!p->dev->desc.shell.read_memory(
                p->dev->desc.shell.opaque,
                ring->ring_base_gpa, sizeof(basenode), basenode)) {
            LAGFX_TRACE("child_ring[%u]: basenode read failed at "
                       "gpa=0x%llx", ri,
                       (unsigned long long)ring->ring_base_gpa);
            continue;
        }

        uint32_t produced = (uint32_t)basenode[0]
                          | ((uint32_t)basenode[1] << 8)
                          | ((uint32_t)basenode[2] << 16)
                          | ((uint32_t)basenode[3] << 24);
        uint32_t consumed = (uint32_t)basenode[4]
                          | ((uint32_t)basenode[5] << 8)
                          | ((uint32_t)basenode[6] << 16)
                          | ((uint32_t)basenode[7] << 24);
        uint32_t fault    = (uint32_t)basenode[8]
                          | ((uint32_t)basenode[9] << 8)
                          | ((uint32_t)basenode[10] << 16)
                          | ((uint32_t)basenode[11] << 24);

        if (produced <= consumed) {
            continue;
        }

        {
            if (ri < LAGFX_MAX_DISPLAY_CHILD_RINGS) {
                p->log_suppress[ri]++;
            }
            if (p->log_suppress[ri] <= 4) {
                LAGFX_TRACE("child_ring[%u]: FIRST SEEN produced=%u "
                            "consumed=%u fault=%u pending=%u "
                            "ring_base=0x%llx ring_size=0x%llx "
                            "entry_count=%u strides=(%u,%u) "
                            "basenode_hex=%02x%02x%02x%02x %02x%02x%02x%02x "
                            "%02x%02x%02x%02x %02x%02x%02x%02x "
                            "%02x%02x%02x%02x %02x%02x%02x%02x "
                            "%02x%02x%02x%02x %02x%02x%02x%02x",
                            ri, produced, consumed, fault,
                            produced - consumed,
                           (unsigned long long)ring->ring_base_gpa,
                           (unsigned long long)ring->ring_size,
                           ring->entry_count,
                           (unsigned)ring->read_stride,
                           (unsigned)ring->write_stride,
                           basenode[0], basenode[1], basenode[2],
                           basenode[3], basenode[4], basenode[5],
                           basenode[6], basenode[7], basenode[8],
                           basenode[9], basenode[10], basenode[11],
                           basenode[12], basenode[13], basenode[14],
                           basenode[15], basenode[16], basenode[17],
                           basenode[18], basenode[19], basenode[20],
                           basenode[21], basenode[22], basenode[23]);
            }
        }

        LAGFX_LOG("child_ring[%u]: produced=%u consumed=%u pending=%u",
                 ri, produced, consumed, produced - consumed);

        if (ring->entry_count == 0u || ring->ring_size <= 0x40u) {
            LAGFX_TRACE("child_ring[%u]: bad geometry entry_count=%u "
                        "ring_size=0x%llx", ri, ring->entry_count,
                        (unsigned long long)ring->ring_size);
            continue;
        }

        uint64_t data_base = ring->ring_base_gpa + 0x40u;
        uint64_t data_size = ring->ring_size - 0x40u;
        uint32_t entry_size = (uint32_t)(data_size / ring->entry_count);
        if (entry_size < 12u) {
            LAGFX_TRACE("child_ring[%u]: entry_size=%u too small "
                        "(data_size=0x%llx entry_count=%u)",
                        ri, entry_size,
                        (unsigned long long)data_size,
                        ring->entry_count);
            continue;
        }

        uint32_t cur = consumed;
        uint32_t drained = 0;
        while (cur < produced && drained < 128u) {
            uint32_t slot = cur % ring->entry_count;
            uint64_t cmd_gpa = data_base
                             + (uint64_t)slot * (uint64_t)entry_size;

            uint8_t hdr_bytes[12] = {0};
            if (!p->dev->desc.shell.read_memory(
                    p->dev->desc.shell.opaque,
                    cmd_gpa, 12, hdr_bytes)) {
                LAGFX_TRACE("child_ring[%u]: header read failed at "
                            "gpa=0x%llx slot=%u cur=%u",
                            ri, (unsigned long long)cmd_gpa, slot, cur);
                break;
            }

            uint16_t opcode = (uint16_t)(hdr_bytes[0]
                                         | (hdr_bytes[1] << 8));
            uint32_t cmd_len = (uint32_t)hdr_bytes[4]
                              | ((uint32_t)hdr_bytes[5] << 8)
                              | ((uint32_t)hdr_bytes[6] << 16)
                              | ((uint32_t)hdr_bytes[7] << 24);
            uint32_t stamp = (uint32_t)hdr_bytes[8]
                            | ((uint32_t)hdr_bytes[9] << 8)
                            | ((uint32_t)hdr_bytes[10] << 16)
                            | ((uint32_t)hdr_bytes[11] << 24);

            if (cmd_len < 12u || cmd_len > entry_size) {
                LAGFX_TRACE("child_ring[%u]: bad cmd_len=%u at cur=%u "
                            "slot=%u entry_size=%u "
                            "hdr=[%02x %02x %02x %02x %02x %02x "
                            "%02x %02x %02x %02x %02x %02x] — stopping",
                            ri, cmd_len, cur, slot, entry_size,
                            hdr_bytes[0], hdr_bytes[1], hdr_bytes[2],
                            hdr_bytes[3], hdr_bytes[4], hdr_bytes[5],
                            hdr_bytes[6], hdr_bytes[7], hdr_bytes[8],
                            hdr_bytes[9], hdr_bytes[10], hdr_bytes[11]);
                break;
            }

            uint8_t *cmd = (uint8_t *)malloc(cmd_len);
            if (!cmd) {
                LAGFX_TRACE("child_ring[%u]: malloc(%u) failed",
                            ri, cmd_len);
                break;
            }
            if (!p->dev->desc.shell.read_memory(
                    p->dev->desc.shell.opaque,
                    cmd_gpa, cmd_len, cmd)) {
                LAGFX_TRACE("child_ring[%u]: cmd body read failed "
                            "gpa=0x%llx len=%u",
                            ri, (unsigned long long)cmd_gpa, cmd_len);
                free(cmd);
                break;
            }

            LAGFX_TRACE("child_ring[%u] cmd[%u]: opcode=0x%04x (%s) "
                        "len=%u stamp=0x%08x slot=%u cur=%u",
                        ri, drained, opcode,
                        lagfx_opcode_name(opcode),
                        cmd_len, stamp, slot, cur);

            lagfx_cmd_header_t parsed;
            p->extra_stamp_advance = 0u;
            int rc = lagfx_protocol_dispatch_one_no_stamp(
                p, cmd, cmd_len, &parsed);
            (void)rc;

            LAGFX_TRACE("child_ring[%u] cmd[%u]: dispatch rc=%d "
                        "stamp=0x%08x",
                     ri, drained, rc, parsed.stamp);

            uint32_t effective = parsed.stamp
                                 + p->extra_stamp_advance;
            if (effective > *io_last_stamp) {
                *io_last_stamp = effective;
            }

            free(cmd);
            cur++;
            drained++;
        }

        if (drained > 0 && p->dev->desc.shell.write_memory) {
            p->dev->desc.shell.write_memory(
                p->dev->desc.shell.opaque,
                ring->ring_base_gpa + 4u,
                sizeof(cur), &cur);
            LAGFX_TRACE("child_ring[%u]: consumed %u -> %u "
                        "(%u cmd(s) drained)",
                        ri, consumed, cur, drained);
        }

        /* Complete stamp for this channel after draining. This is the
         * critical fix: without this, the kext's waitForStamp loops
         * forever because stamps are never acknowledged. */
        if (drained > 0) {
            uint32_t slot = ri;  /* ring index == slot for display channels */
            lagfx_protocol_complete_stamp_slot(p, slot, *io_last_stamp);
            LAGFX_LOG("child_ring[%u]: completed stamp 0x%08x for slot %u "
                      "(drained=%u cmds)",
                      ri, *io_last_stamp, slot, drained);

            /* Update per-channel highest stamp tracker */
            if (ri < 32) {
                p->per_channel_highest_stamp[ri] = *io_last_stamp;
            }
        }
    }
}

/* === Dispatcher ============================================ */

/* Internal: parse + run the handler. Caller decides what (if any)
 * stamp completion to run on the way out. Returns rc; on parse
 * failure returns negative value and *did_run_handler = 0. */
static int lagfx_dispatch_inner(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes, size_t cmd_len,
                                lagfx_cmd_header_t *out_hdr,
                                int *did_run_handler) {
    *did_run_handler = 0;
    if (!lagfx_protocol_is_valid(p) || !cmd_bytes) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!lagfx_fifo_parse_header(cmd_bytes, cmd_len, out_hdr)) {
        LAGFX_ERR("dispatch: header parse failed (len=%zu)", cmd_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    p->total_cmds_seen += 1;

    const lagfx_op_descriptor_t *desc = lagfx_opcode_lookup(out_hdr->opcode);
    lagfx_op_handler_fn fn = (desc && desc->handler) ? desc->handler
                                                     : lagfx_op_default_handler;

    if (!desc) {
        p->unknown_opcode_count += 1;
    }

    LAGFX_LOG("dispatch: op=0x%04x (%s) stamp=0x%08x arg_count_8b=%u "
              "length=%u payload=%u",
              out_hdr->opcode, lagfx_opcode_name(out_hdr->opcode),
              out_hdr->stamp, (unsigned)out_hdr->arg_count_8b,
              (unsigned)out_hdr->length, (unsigned)out_hdr->payload_size);

    if (desc) {
        if (out_hdr->payload_size < desc->min_payload) {
            LAGFX_TRACE("dispatch: %s payload too small (%u < %u min)",
                        desc->name, (unsigned)out_hdr->payload_size,
                        (unsigned)desc->min_payload);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        if (desc->max_payload != 0 && out_hdr->payload_size > desc->max_payload) {
            LAGFX_TRACE("dispatch: %s payload too large (%u > %u max)",
                        desc->name, (unsigned)out_hdr->payload_size,
                        (unsigned)desc->max_payload);
            /* Fall through — fail-open. */
        }
    }

    *did_run_handler = 1;
    int handler_rc = (int)fn(p, out_hdr);
    if (handler_rc != 0) {
        LAGFX_TRACE("dispatch: op=0x%04x (%s) handler returned error %d "
                    "(stamp=0x%08x)",
                    out_hdr->opcode, lagfx_opcode_name(out_hdr->opcode),
                    handler_rc, out_hdr->stamp);
    }
    return handler_rc;
}

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

/* lagfx_task_translate moved to translate.c. */

/* Per-channel variant — runs the handler but does NOT auto-complete
 * the stamp. The caller (typically the per-channel doorbell handler)
 * is responsible for advancing stamp_cell[ch] + setting the
 * pending_stamps_bitmask bit + raising the IRQ once after draining
 * all cmds in the ring. Returns the handler rc and writes the
 * parsed header to *out_hdr if non-NULL. */
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

/* === MMIO register shadow =================================== */

uint32_t lagfx_protocol_mmio_read(lagfx_protocol_t *p, uint64_t offset) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* MSI-X table range — shell owns it. Return 0, no log spam. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return 0;
    }

    /*
     * Secondary capability / config bank (BAR0+0x1200..0x122c).
     *
     * Per PROTOCOL.md §2.1, mmio-survival-recipe-v2.md, and
     * graphics-attach.md:
     *   0x122c is the CAPABILITY_GATE read by
     *   AppleParavirtGPUControl::start()+0x7c. Must return >= 9 to
     *   select the modern paravirt path. 0 = silent bail, 1..8 =
     *   legacy. Verified RE + M2/M3/M4 live boots at value 9.
     *
     * Other entries (per PROTOCOL.md §2.1 and re-followup-spec-gaps
     * §5.2): _PGDevice ObjC getters backing individual MMIO offsets.
     * Return 0 for unhandled; _PGDevice's ivars are zero-initialised.
     */
    if (offset == 0x122c) {
        return 1;
    }
    if (offset >= 0x1200 && offset < 0x122c) {
        return 0;
    }

    /* 0x100c — _rootFIFO.fifoFaultOffset.
     *
     * Per RE (BAR0-mmio-map.md): "last fault offset; infrequent read;
     * host should hard-return 0." The kext's ISR reads this on
     * fault-path walks; returning read_ptr (our previous behavior)
     * could trigger spurious handleFaultInterrupt processing. */
    if (offset == LAGFX_REG_FIFO_FAULT_OFFSET) {
        LAGFX_TRACE("mmio_read: FIFO_FAULT_OFFSET -> 0");
        return 0u;
    }

    /*
     * 0x102c — fault-pending status.
     *
     * The kext's unified ISR at MSI-X vec 0 reads this register to
     * decide whether to walk the fault queue via handleFaultInterrupt.
     * Non-zero = faults pending. Returning `last_completed_stamp`
     * causes every ISR to spuriously drain the fault queue, which in
     * turn IOLogs each entry and eventually escalates to terminate.
     * Return 0 unconditionally unless we actually have a fault to
     * report; no fault path is wired yet.
     */
    if (offset == 0x102cu) {
        LAGFX_TRACE("mmio_read: 0x102c (fault_status) -> 0");
        return 0u;
    }

    /*
     * 0x1018 — stamp-completion bitmask fed to signalStamps.
     *
     * Bit N = stamp_id N completed since the last ISR read. Kext does
     * xchg-with-0 on this register and passes the prior value to
     * AppleParavirtEventMachine::signalStamps, which iterates set bits
     * via bsf and calls commandWakeup(stamp_id) per bit. commandWakeup
     * reads the actual stamp value from [EM+0x20] in kernel heap — we
     * don't have to provide stamp values via DMA.
     *
     * Return our pending-stamps bitmask and atomically clear it.
     */
    if (offset == LAGFX_REG_STAMP_CELL_2) {
        uint32_t mask = p->pending_stamps_bitmask;
        p->pending_stamps_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1018 (stamp_bitmask) xchg -> 0x%x", mask);
        return mask;
    }

    /*
     * 0x1014 — display-interrupt bitmask fed to signalDisplays.
     *
     * Same pattern as 0x1018, but the mask here targets
     * AppleParavirtDisplayMachine. Bit N = display_id N has a
     * completed transaction.
     */
    if (offset == LAGFX_REG_STAMP_CELL_1) {
        /* xchg-and-clear, mirrors 0x1018 semantics.
         * Bit N = display channel N has a completed transaction. Set by
         * the per-channel doorbell handler for display channels (ch>=5).
         * Per display0-cmd-actual-location.md, the display bitmask is
         * what signalDisplays consumes — separate from the stamp bitmask. */
        uint32_t mask = p->pending_displays_bitmask;
        p->pending_displays_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1014 (display_bitmask) xchg -> 0x%x", mask);
        return mask;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_read: unmapped offset 0x%llx -> 0",
                  (unsigned long long)offset);
        return 0;
    }

    uint32_t value = p->reg[idx];

    /*
     * 0x1034 (BINARY_VERSION / idx 13) — version negotiation loopback.
     *
     * AppleParavirtAccelerator::setupVersion() (kext+0x314a) does:
     *   1. Write preferred version (6) to BAR0+0x34.
     *   2. Read BAR0+0x34 back immediately.
     *   3. If read-back <= 6 and >= 1 → accept (decode cap flags).
     *      If high bit (bit 31) set → "rejected"; retry with host's
     *      max-supported version from low 31 bits.
     *      If still invalid after retry → write 0x80000006 (abort).
     *
     * Our write handler (below) shadows the written value into
     * reg[13], so reading it back returns what the kext wrote.
     * With loopback: kext writes 6, reads 6, cap_a=1 cap_b=0
     * cap_c=1 cap_d=1 subver=1 flag_e=1 flag_f=1 → version 6
     * accepted. No action needed.
     */

    LAGFX_TRACE("mmio_read: off=0x%llx -> 0x%08x",
              (unsigned long long)offset, value);
    return value;
}

void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset,
                               uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    /* MSI-X range — shell's problem. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_write: unmapped offset 0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    /* Shadow first so reads reflect the write even if we short-circuit
     * into a setter probe. Note that 0x101c is documented read-only in
     * the dylib (returns _rootPageNumber); we still shadow the written
     * value so a subsequent read in the test rig can confirm the path
     * was taken. The real hardware behavior on write to 0x101c is also
     * "store to setter-backed ivar" (_PGDevice calls its 1-arg
     * selector), so shadowing is consistent. */
    p->reg[idx] = value;

    LAGFX_TRACE("mmio_write: off=0x%llx val=0x%08x",
              (unsigned long long)offset, value);

    /* Primary-ring MMIO write map (Accelerator::setupCommandRing).
     *
     *   0x1000 W → ring_armed (1=enable). Kick only; doorbell advances
     *              happen via 0x1008.
     *   0x1004 W → ring_size (bytes; observed 0x10000 = 64 KiB).
     *   0x1008 W → write_ptr update. Each write advances write_ptr;
     *              we drain everything in [read_ptr, write_ptr).
     *   0x100c R → read_ptr (guest polls to confirm our progress).
     *              Read handler is elsewhere; we don't write 0x100c.
     *   0x1010 W → page_size (observed 0x1000).
     *   0x101c W → ring_shared_page_pfn (mailbox page; NOT the
     *              command ring). Previously mistagged read-only.
     *   0x1030 W → ring_base_pfn → ring_base_gpa = pfn << 12.
     */
    switch (offset) {
        case LAGFX_REG_STATUS_CONTROL:
            p->ring_armed = (value != 0u);
            LAGFX_TRACE("mmio_write: STATUS_CONTROL ring_armed=%d",
                      (int)p->ring_armed);
            return;
        case 0x1004u:
            p->ring_size = value ? value : 0x10000u;
            LAGFX_TRACE("mmio_write: ring_size=0x%x", p->ring_size);
            return;
        case 0x1008u: {
            uint32_t old_wp = p->write_ptr;
            p->write_ptr = value;
            LAGFX_LOG("doorbell: primary ring wp=0x%x (was 0x%x)",
                      value, old_wp);
            if (p->ring_armed && p->write_ptr != p->read_ptr) {
                size_t drained = lagfx_fifo_drain(p);
                LAGFX_TRACE("mmio_write: doorbell drained %zu cmds", drained);
            }
            return;
        }
        case 0x1010u:
            /* setFifoStart — byte offset of ring within its base page.
             * Observed value 0x1000 on M2 boot: first 4 KiB of the
             * base page is a control/metadata header; command stream
             * starts at base_page + 0x1000. */
            p->ring_start_offset = value;
            p->page_size = 0x1000u;  /* assumed; not read from MMIO */
            p->ring_base_gpa =
                ((uint64_t)p->ring_base_pfn << 12) + p->ring_start_offset;
            LAGFX_TRACE("mmio_write: ring_start_offset=0x%x -> gpa=0x%llx",
                      value, (unsigned long long)p->ring_base_gpa);
            return;
        case 0x101cu:
            p->ring_shared_page_pfn = value;
            LAGFX_TRACE("mmio_write: ring_shared_page_pfn=0x%x", value);
            return;
        case 0x1030u: {
            p->ring_base_pfn = value;
            p->ring_base_gpa =
                ((uint64_t)value << 12) + p->ring_start_offset;
            if (p->ring_size == 0u) {
                p->ring_size = 0x10000u;
            }
            LAGFX_TRACE("mmio_write: ring_base_pfn=0x%x -> gpa=0x%llx size=0x%x",
                      value, (unsigned long long)p->ring_base_gpa,
                      p->ring_size);
            return;
        }
case 0x1020u: {
              /* Per-channel doorbell. Use dispatcher registry for polymorphic
               * routing — channel 0 (primary ring) goes to Channel0Dispatcher,
               * channels 1-4 go to ComputeDispatcher, channels 5+ go to DisplayVchanDispatcher.
               * Unregistered channels automatically route through unknown_dispatcher. */
              unsigned ch = value;
              
              if (ch >= LAGFX_MAX_CHANNELS) {
                  LAGFX_TRACE("doorbell ch=%u: out of range", ch);
                  return;
              }

              /* Set current channel ID for per-channel opcode tracking */
              p->current_chan_id = ch;
              
              /* Look up dispatcher by channel ID — polymorphic dispatch! */
              lagfx_dispatcher_base_t *d = lagfx_dispatcher_lookup(ch);
              if (!d) {
                  LAGFX_WARN("doorbell: no dispatcher for channel %u -> routing to unknown_dispatcher", ch);
                  d = (lagfx_dispatcher_base_t *)unknown_dispatcher_new();
              }

               LAGFX_LOG("doorbell ch=%u: dispatching to %s", ch, d->name);
              
              /* Delegate ring drain and command dispatch to dispatcher */
              uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
              
              /* Channel 0 uses primary ring FIFO — descriptor at +0x400 from shared page.
               * Channels 1+ use per-channel sub-channels — descriptor at +0x400 + 20*(ch-1). */
              uint64_t descr_gpa;
              if (ch == 0) {
                  /* Primary ring: descriptor is the FIFO write pointer itself,
                   * not a separate channel descriptor. Use shared page + 0x400 as base. */
                  descr_gpa = shared_gpa + 0x400u;
              } else {
                  descr_gpa = shared_gpa + 0x400u + 20u * (ch - 1u);
              }
              
              /* Call dispatcher's ring_dispatch method via polymorphic dispatch */
if (ch == 0) {
    channel_0_dispatcher_ring_dispatch((lagfx_channel_0_dispatcher_t *)d, p, descr_gpa, ch);
} else if (ch == 1) {
    channel_1_dispatcher_ring_dispatch((lagfx_channel_1_dispatcher_t *)d, p, descr_gpa, ch);
} else if (ch == 2) {
    channel_2_dispatcher_ring_dispatch((lagfx_channel_2_dispatcher_t *)d, p, descr_gpa, ch);
} else if (ch == 3) {
    channel_3_dispatcher_ring_dispatch((lagfx_channel_3_dispatcher_t *)d, p, descr_gpa, ch);
} else if (ch == 4) {
    channel_4_dispatcher_ring_dispatch((lagfx_channel_4_dispatcher_t *)d, p, descr_gpa, ch);
} else if (ch >= 5) {
    channel_5_plus_dispatcher_ring_dispatch((lagfx_channel_5_plus_dispatcher_t *)d, p, descr_gpa, ch);
} else if (d->name && strstr(d->name, "UnknownDispatcher") != NULL) {
    unknown_dispatcher_ring_dispatch((lagfx_unknown_dispatcher_t *)d, p, descr_gpa, ch);
}
return;
          }
        default: break;
    }
}

/* === Stats accessors ======================================== */

void lagfx_protocol_stats(const lagfx_protocol_t *p,
                          uint64_t *total_cmds_seen_out,
                          uint64_t *total_cmds_completed_out,
                          uint64_t *unknown_opcode_count_out) {
    if (!lagfx_protocol_is_valid(p)) {
        if (total_cmds_seen_out)      *total_cmds_seen_out = 0;
        if (total_cmds_completed_out) *total_cmds_completed_out = 0;
        if (unknown_opcode_count_out) *unknown_opcode_count_out = 0;
        return;
    }
    if (total_cmds_seen_out)      *total_cmds_seen_out = p->total_cmds_seen;
    if (total_cmds_completed_out) *total_cmds_completed_out = p->total_cmds_completed;
    if (unknown_opcode_count_out) *unknown_opcode_count_out = p->unknown_opcode_count;
}

