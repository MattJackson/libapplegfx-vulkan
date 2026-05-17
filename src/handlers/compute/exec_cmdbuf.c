/*
 * libapplegfx-vulkan — Command buffer execution handler (opcode 0x20/0x37)
 * src/handlers/compute/exec_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * CmdExecIndirect2 outer-payload handler + Stage-20% inner-PGCmdHeader
 * observation walker. The outer payload layout is RE-confirmed
 * 2026-04-28 (see paravirt-re M4-inner-opcode-implementation-guide.md
 * §1.1). The inner walker translates each resource's task-virtual
 * cmdbuf address via the per-task radix tree
 * (paravirt-re/library/state-machines/per-task-page-table.md), reads
 * the 8-byte PGSerializerCommandSegmentHeader (inner-opcode-format.md
 * §"Top-level structure"), and on encType=2 (render) reports the
 * first PGCmdHeader so Stage 20% (RenderPassDescriptor sighting) can
 * be validated against /tmp/lagfx.log.
 *
 * Stage 20% requirement (per reference_m5_validation.md):
 *   grep -E "RenderPassDescriptor|0x1a:" /tmp/lagfx.log → ≥ 1 match
 *
 * The walker is observation-only — it does NOT execute Metal
 * commands. Real rendering lives in render_opcodes.c (not compiled
 * into the current build; Stage 30+ work). Stamp is signalled at the
 * end so the kext's waitForStamp() doesn't park.
 */

#include "../handlers.h"
#include "info_replies.h"
#include "render_inner_ops.h"
#include "blit_inner_ops.h"
#include "compute_inner_ops.h"
#include "common/le.h"
#include "common/log.h"

#include <stdint.h>
#include <stdio.h>

/* === Per-task radix-tree translator ============================== */

/* Translate a task-virtual address to GPA via the 3-level radix tree
 * rooted at task->root_page_pfn. Returns true on success.
 *
 * See paravirt-re/library/state-machines/per-task-page-table.md
 * for the wire format. PTE layout:
 *
 *   bits 30..0  next-PFN (interior nodes) OR data-PFN (leaf nodes)
 *   bit  31     is_leaf — 0 for interior, 1 for leaf
 *
 * An interior-level PTE with bit 31 set means the walk terminates
 * early — the PTE directly identifies a data page covering the
 * remainder of the address. This is the "huge page" / compact-map
 * case. The remaining low bits of dev_addr become the in-page offset
 * scaled to the level reached.
 */
static bool task_translate(lagfx_protocol_t *p,
                            const lagfx_task_entry_t *task,
                            uint64_t dev_addr,
                            uint64_t *out_gpa) {
    if (!task || task->root_page_pfn == 0u) {
        return false;
    }
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        return false;
    }

    uint64_t header_gpa = task->root_page_pfn << 12;
    uint8_t header[8];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      header_gpa, 8, header)) {
        return false;
    }
    uint32_t l1_pfn = lagfx_le32(header + 0);
    uint32_t levels = lagfx_le32(header + 4);
    if (l1_pfn == 0u || levels == 0u || levels > 4u) {
        return false;
    }

    uint64_t page_idx = dev_addr >> 12;
    uint32_t node_pfn = l1_pfn;
    int shift = (int)(levels - 1u) * 10;

    while (shift > 0) {
        uint64_t idx = (page_idx >> shift) & 0x3ffu;
        uint64_t pte_gpa = ((uint64_t)node_pfn << 12) + idx * 4u;
        uint8_t pte_buf[4];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          pte_gpa, 4, pte_buf)) {
            return false;
        }
        uint32_t pte = lagfx_le32(pte_buf);
        if (pte == 0u) {
            return false;
        }
        if (pte & 0x80000000u) {
            /* Early-leaf: this interior-level PTE directly identifies
             * a data page covering the remaining address range. The
             * in-page offset spans the bits not yet consumed by the
             * walk: (10*shift) lower index bits + the 12-bit byte
             * offset. Mask out is_leaf before composing the GPA. */
            uint32_t data_pfn = pte & 0x7fffffffu;
            uint64_t offset_mask = ((uint64_t)1 << (shift + 12)) - 1u;
            uint64_t page_offset = dev_addr & offset_mask;
            *out_gpa = ((uint64_t)data_pfn << 12) | page_offset;
            return true;
        }
        node_pfn = pte & 0x7fffffffu;
        shift -= 10;
    }

    /* Leaf */
    uint64_t leaf_idx = page_idx & 0x3ffu;
    uint64_t leaf_pte_gpa = ((uint64_t)node_pfn << 12) + leaf_idx * 4u;
    uint8_t leaf_buf[4];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      leaf_pte_gpa, 4, leaf_buf)) {
        return false;
    }
    uint32_t leaf_pte = lagfx_le32(leaf_buf);
    if (leaf_pte == 0u) {
        return false;
    }
    uint32_t data_pfn = leaf_pte & 0x7fffffffu;
    uint64_t page_offset = dev_addr & 0xfffu;
    *out_gpa = ((uint64_t)data_pfn << 12) | page_offset;
    return true;
}

/* === Inner segment + PGCmdHeader walker ==========================
 *
 * Per inner-opcode-format.md:
 *   - 8-byte PGSerializerCommandSegmentHeader:
 *       +0 u32 segmentSize, +4 u8 encoderType, +5 u8 reuseFlag,
 *       +6 u8 keepFlag, +7 u8 pad
 *   - For render segments (encType=2), the first PGCmdHeader at
 *     segment+8 is canonically 0x1a (RENDER_DESCRIBE_RENDER_PASS).
 *   - PGCmdHeader is 8 bytes: u32 opcode + u32 totalLengthBytes.
 *
 * Encoder-type dispatch:
 *   0 / 1  — Compute. Currently TRACE only; the live channel_compute
 *            drainer already accepts the outer 0x20/0x37 and the
 *            compute inner-opcode table is forward-port material
 *            (Stage 30+). For now the per-opcode TRACE is sufficient.
 *   2      — Render. Dispatched into render_inner_ops_dispatch (this
 *            is the canonical Stage 20% path; emits the 0x1a
 *            RenderPassDescriptor sighting line).
 *   4      — Blit. Dispatched into blit_inner_ops_dispatch when
 *            available; until then logged + absorbed.
 *   5      — Protection-options preamble. The kext writes
 *            `(uint64_t protectionOptions, PGSerializerCommandSegmentHeader)`
 *            after this header; the segment locator already handles
 *            the recursive header read at the cursor boundary, so
 *            walker just logs and continues.
 *
 * InfoDecoder reply dispatch (encType=4 + opcode 0x1c2..0x1d0) is
 * handled inline below; those reply contracts cross-cut blit + render
 * (encoder_type=4 is overloaded with InfoDecoder for queries).
 */

#define LAGFX_INNER_OP_RENDER_DESCRIBE_RENDER_PASS  0x1au

/* Walk one segment's PGCmdHeader stream. Returns number of inner
 * commands observed. Stops on first malformed header.
 *
 * Threaded args (for InfoDecoder reply dispatch on encType=4, opcodes
 * 0x1c2..0x1d0): the outer CmdExecIndirect2 resource_table base +
 * count, and the resolved task. These let info_replies.c resolve
 * buffer_id → buffer_dev_addr → reply target GPA. NULL/0 disables
 * info-reply dispatch (observation-only fallback). */
static size_t inner_walk_segment(lagfx_protocol_t *p,
                                  uint8_t encoder_type,
                                  const uint8_t *segment_bytes,
                                  size_t segment_len,
                                  uint32_t stamp,
                                  const uint8_t *outer_resources,
                                  uint32_t resource_count,
                                  const lagfx_task_entry_t *task) {
    if (segment_len < 8u || segment_bytes == NULL) return 0;

    size_t off = 0;
    size_t observed = 0;
    while (off + 8u <= segment_len) {
        uint32_t opcode      = lagfx_le32(segment_bytes + off + 0);
        uint32_t total_len   = lagfx_le32(segment_bytes + off + 4);
        if (total_len < 8u || total_len > segment_len - off) {
            LAGFX_TRACE("inner_walk: bad PGCmdHeader at off=%zu opcode=0x%x "
                        "total_len=%u — stop", off, opcode, total_len);
            break;
        }

        /* Inner opcode key: most inner opcodes pack into the low byte
         * (Render/Compute/Blit ranges all <= 0xa6), but the Info family
         * occupies 0x1c2..0x1d0 which require the low 16 bits. Use the
         * full u32 for the InfoDecoder check, low byte for Stage 20. */
        uint32_t opcode_full  = opcode;
        uint8_t  opcode_byte  = (uint8_t)(opcode & 0xffu);

        LAGFX_TRACE("inner_walk: encType=%u opcode=0x%04x (byte=0x%02x) len=%u off=%zu",
                    (unsigned)encoder_type, (unsigned)(opcode_full & 0xffffu),
                    (unsigned)opcode_byte, total_len, off);

        const uint8_t *body = segment_bytes + off + 8u;
        size_t body_len = total_len - 8u;
        (void)stamp;  /* dispatchers may need the stamp later */

       switch (encoder_type) {
            case 0u:
            case 1u:
                /* Compute — Stage 30 dispatch. The descriptor table in
                 * compute_inner_ops.c covers the 15 encType=0 opcodes
                 * observed live (2026-05-14 empirical sweep). All
                 * handlers are currently parse-and-trace stubs; Task 6
                 * of memory/stage30_freshman_queue.md promotes one to
                 * a real Vulkan dispatch. */
                uint32_t task_id = task ? task->id : 0xffffffffu;
                lagfx_compute_inner_dispatch(p, encoder_type, task_id,
                                               opcode_full & 0xffffu,
                                               body, body_len);
                break;

            case 2u:
                /* Render — dispatch into the 96-entry render inner-op
                 * table (render_inner_ops.c). opcode 0x1a emits the
                 * canonical Stage 20% sighting line. */
                lagfx_render_inner_dispatch(p, opcode_full & 0xffffu, body, body_len);
                break;

            case 4u: {
                /* Blit OR InfoDecoder (encType=4 is overloaded).
                 *
                 * InfoDecoder opcode space is 0x1c2..0x1d0 — these
                 * carry pipeline-state / host-resource info queries
                 * that need replies (info_replies.c). Without a
                 * non-zero reply, SkyLight aborts in
                 * MetalShader::CopyPipelineState. */
                uint16_t op16 = (uint16_t)(opcode_full & 0xffffu);
                if (op16 >= 0x1c2u && op16 <= 0x1d0u) {
                    lagfx_info_dispatch(p, op16, body, body_len,
                                        outer_resources, resource_count,
                                        task, task_translate);
                } else {
                    /* Real blit dispatch — 24-entry table at 0x12c..0x143
                     * plus extended low-range (0x001..0x07d). */
                    lagfx_blit_inner_dispatch(p, op16, body, body_len);
                }
                break;
            }

            case 5u:
                /* Protection-options preamble. The segment locator
                 * has already advanced past the recursive header read
                 * at the cursor boundary; this case fires only if a
                 * preamble shows up mid-stream, which would be a wire
                 * format violation. Log + absorb. */
                LAGFX_TRACE("inner_walk: protection-options preamble mid-stream "
                            "(encType=5 op=0x%04x)", opcode_full & 0xffffu);
                break;

            default:
                LAGFX_WARN("inner_walk: unknown encType=%u op=0x%04x len=%u",
                           (unsigned)encoder_type, opcode_full & 0xffffu, total_len);
                break;
        }

        observed++;
        off += total_len;
    }
    return observed;
}

/* Locate a segment header in `cmdbuf`. Per inner-opcode-format.md
 * §"The 8-byte gotcha", live-traffic segments start either at +0 or
 * at +8 within the resource's cmdbuf (the kext sometimes prefixes
 * 8 bytes of metadata). We try +0 first; if segmentSize is invalid
 * we fall back to +8.
 *
 * Returns the offset of the segment header on success, SIZE_MAX on
 * failure.
 */
static size_t locate_segment_header(const uint8_t *cmdbuf, size_t length) {
    static const size_t candidates[] = { 0u, 8u };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        size_t off = candidates[i];
        if (off + 8u > length) continue;
        uint32_t segment_size = lagfx_le32(cmdbuf + off + 0);
        uint8_t  encoder_type = cmdbuf[off + 4];
        if (segment_size == 0u || segment_size > length - off) continue;
        /* Valid encoder types per inner-opcode-format.md:
         *   0 / 1 = compute, 2 = render, 4 = blit, 5 = protection preamble. */
        if (encoder_type != 0u && encoder_type != 1u && encoder_type != 2u
            && encoder_type != 4u && encoder_type != 5u) {
            continue;
        }
        return off;
    }
    return (size_t)-1;
}

/* Read a resource's cmdbuf from guest memory and walk its segments.
 *
 * Stage 30: page-walked translation per
 * paravirt-re/library/state-machines/per-task-page-table.md. A
 * cmdbuf spanning more than one 4 KiB page is the common case once
 * macOS starts submitting real render passes — `desc_count > 0`
 * payloads, command stream + resource_table laid out across multiple
 * task-VA pages. The previous walker translated the first page only
 * and treated the rest as a contiguous DMA from `first_gpa`; if the
 * radix maps the next task-VA page to a NON-contiguous physical
 * page, the second-page region of the read buffer contains data
 * from the wrong page and inner_walk_segment fabricates garbage
 * "inner opcodes" out of it.
 *
 * Fix: walk task_translate per page span, chain reads into the
 * scratch buffer. Modelled on
 * channel_compute_dispatcher.c:ring_resolve_data_gpa's per-page
 * chunked read, except the GPA source is the radix tree rather
 * than the per-channel ring's PFN-array.
 *
 * Translation falls back to identity when the task is unknown
 * (pre-2026-05-09 mode when 0x38 wasn't firing); identity treats
 * `host_gpu_addr` as a flat GPA and lets shell.read_memory cope.
 */
static void exec_walk_resource(lagfx_protocol_t *p,
                                 const lagfx_task_entry_t *task,
                                 uint64_t host_gpu_addr,
                                 uint32_t length,
                                 uint32_t stamp,
                                 const uint8_t *outer_resources,
                                 uint32_t resource_count) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) return;
    if (length == 0u || length > (1u << 22)) {
        LAGFX_TRACE("exec_walk_resource: bad length=%u — skip", length);
        return;
    }

    /* Read up to scratch_exec bytes, chunked per page so the radix
     * tree can map each task-VA page to whichever GPA the kext put
     * it at. */
    uint8_t *buf = p->scratch_exec;
    uint32_t buf_size = LAGFX_MAX_RING_READ;
    uint32_t to_read = length < buf_size ? length : buf_size;

    uint32_t bytes_read = 0;
    while (bytes_read < to_read) {
        uint64_t cur_va = host_gpu_addr + bytes_read;
        uint64_t cur_gpa = 0;
        bool translated = false;
        if (task) {
            translated = task_translate(p, task, cur_va, &cur_gpa);
        }
        if (!translated) {
            cur_gpa = cur_va;  /* fallback: VA == GPA */
            if (bytes_read == 0u) {
                LAGFX_TRACE("exec_walk_resource: task translate unavailable; "
                            "using identity gpa=0x%llx",
                            (unsigned long long)cur_gpa);
            }
        }

        /* Chunk = remainder of the current 4 KiB page (or remaining
         * bytes to read, whichever is smaller). */
        uint32_t off_in_page = (uint32_t)(cur_gpa & 0xfffu);
        uint32_t chunk = 0x1000u - off_in_page;
        if (chunk > to_read - bytes_read) {
            chunk = to_read - bytes_read;
        }

        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          cur_gpa, chunk,
                                          buf + bytes_read)) {
            LAGFX_TRACE("exec_walk_resource: read_memory failed at gpa=0x%llx "
                        "(chunk %u, off=%u)",
                        (unsigned long long)cur_gpa, chunk, bytes_read);
            if (bytes_read == 0u) {
                return;  /* nothing salvageable */
            }
            /* Partial read — fall through and walk what we have. */
            break;
        }
        bytes_read += chunk;
    }
    to_read = bytes_read;
    if (to_read == 0u) return;

    /* One-time hexdump of first CmdExecIndirect2 resource buffer bytes (buf). */
    static int s_resource_buf_dumped = 0;
    if (!s_resource_buf_dumped && to_read > 0) {
        s_resource_buf_dumped = 1;
        size_t n = to_read < 256 ? to_read : 256;
        LAGFX_LOG("=== first CmdExecIndirect2 resource buffer hexdump (len=%u, showing %zu) ===",
                  to_read, n);
        char hex[3*16+1] = {0};
        for (size_t i = 0; i < n; i += 16) {
            size_t row = (i + 16 <= n) ? 16 : (n - i);
            for (size_t j = 0; j < row; ++j) {
                snprintf(hex + j*3, 4, "%02x ", buf[i+j]);
            }
            LAGFX_LOG("  +0x%04zx: %s", i, hex);
        }
    }

    size_t seg_off = locate_segment_header(buf, to_read);
    if (seg_off == (size_t)-1) {
        LAGFX_TRACE("exec_walk_resource: no valid segment header "
                    "in first %u bytes", to_read);
        return;
    }

    uint32_t segment_size = lagfx_le32(buf + seg_off + 0);
    uint8_t  encoder_type = buf[seg_off + 4];
    LAGFX_LOG("exec_walk: segment seg_off=%zu seg_size=%u encType=%u",
              seg_off, segment_size, (unsigned)encoder_type);

    /* Walk the inner stream after the 8-byte segment header. */
    size_t inner_off = seg_off + 8u;
    if (inner_off >= to_read) return;
    size_t inner_len = segment_size > 8u ? segment_size - 8u : 0u;
    if (inner_len > to_read - inner_off) inner_len = to_read - inner_off;

    size_t inner_count = inner_walk_segment(p, encoder_type,
                                              buf + inner_off,
                                              inner_len, stamp,
                                              outer_resources,
                                              resource_count, task);
    LAGFX_LOG("exec_walk: encType=%u inner_cmds_observed=%zu",
              (unsigned)encoder_type, inner_count);
}

lagfx_handler_status_t lagfx_compute_exec_cmdbuf(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* macOS may send minimal 8-byte CmdExecIndirect2 payloads for
     * query/capability-checking mode. These contain just task_id and no
     * descriptor/resource data. Accept these gracefully instead of
     * reading out-of-bounds garbage as descriptor_count. */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_TRACE("CmdExecIndirect2: empty payload (size=%u)", hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }

    /* If payload is exactly 8 bytes, macOS is doing a minimal query. */
    if (hdr->payload_size == 8) {
        uint32_t task_id = lagfx_le32(hdr->payload);
        LAGFX_TRACE("CmdExecIndirect2: minimal query payload taskID=%u", task_id);
        return LAGFX_HANDLER_OK;
    }

    /* Payload >= 12 bytes: safe to read descriptor_count and resource_count */
    uint32_t task_id           = lagfx_le32(hdr->payload + 0);
    uint32_t descriptor_count  = lagfx_le32(hdr->payload + 4);
    uint32_t resource_count    = lagfx_le32(hdr->payload + 8);

    LAGFX_LOG("CmdExecIndirect2: taskID=%u desc_count=%u res_count=%u payload_size=%u stamp=0x%08x",
              task_id, descriptor_count, resource_count, (unsigned)hdr->payload_size, hdr->stamp);

    /* Outer payload size guard: 12 + dc*24 + rc*16 must fit. */
    uint32_t min_payload = 12u + 24u * descriptor_count + 16u * resource_count;
    if (hdr->payload_size < min_payload) {
        LAGFX_WARN("CmdExecIndirect2: payload too small for desc=%u res=%u (need %u, have %u)",
                   descriptor_count, resource_count, min_payload, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Resolve task — fail-open if not found (resource table may still
     * be readable via identity-VA mapping per 2026-05-09 finding). */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_TRACE("CmdExecIndirect2: taskID=%u not found (fail-open)", task_id);
    }

    /* Walk each resource's cmdbuf for the Stage-20 inner observation
     * AND InfoDecoder reply dispatch. Pass the outer resource_table
     * base + count so info_replies.c can resolve buffer_id → target
     * GPA for pipeline-state info queries. */
    const uint8_t *res_start = hdr->payload + 12u + 24u * descriptor_count;
    for (uint32_t i = 0; i < resource_count; ++i) {
        const uint8_t *r = res_start + 16u * i;
        uint64_t host_gpu_addr = lagfx_le64(r + 0);
        uint32_t res_length    = lagfx_le32(r + 8);

        LAGFX_TRACE("CmdExecIndirect2 resource[%u]: addr=0x%llx len=%u",
                    i, (unsigned long long)host_gpu_addr, res_length);

        exec_walk_resource(p, task, host_gpu_addr, res_length, hdr->stamp,
                            res_start, resource_count);
    }

    LAGFX_LOG("CmdExecIndirect2: completed taskID=%u (observation-only)",
              task_id);

    /* Complete stamp to raise IRQ and advance ring state.
     *
     * The drain loop in channel_*_dispatcher.c ALSO calls
     * complete_stamp_slot with the channel's slot (matching its
     * chan_id), so this is belt-and-suspenders for test paths that
     * drive lagfx_compute_exec_cmdbuf directly without going through
     * a drain.
     *
     * The slot picked here must match the channel that issued the
     * exec. For root-channel-issued execs (chan_id=0, dylib path)
     * the slot is SLOT_ROOT_CHANNEL=0. For compute-vchan-issued
     * (chan 1..4) the slot tracks chan_id (SLOT_COMPUTE_N). Use
     * current_chan_id captured by the door dispatcher — the previous
     * hardcoded `0` was wrong on the compute vchan path (would
     * double-raise slot 0 instead of the actual compute slot). */
    if (p && p->dev && hdr->stamp != 0) {
        uint32_t slot = (uint32_t)p->current_chan_id;
        if (slot >= LAGFX_MAX_CHANNELS) slot = SLOT_ROOT_CHANNEL;
        lagfx_protocol_complete_stamp_slot(p, slot, hdr->stamp);
    }

    return LAGFX_HANDLER_OK;
}
