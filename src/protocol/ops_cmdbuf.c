/*
 * libapplegfx-vulkan — cmdbuf-domain opcode handlers
 * src/protocol/ops_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Handles command-buffer submission opcodes:
 *   - CmdExecIndirect2 (0x20): walks per-resource cmdBuf segments
 *     containing inner render/blit/compute opcodes.
 *   - CmdSynchronizeResources (0x22): marks resources synced.
 *
 * RE context (paravirt-re/):
 *   - M4-inner-opcode-implementation-guide.md: 0x20 outer payload
 *     {task_id, descriptor_count, resource_count, ...}
 *     CONFIRMED 2026-04-28 from live kext-side 0x37 capture.
 *   - classes/ directory: PGSerializerCommandSegmentHeader format
 *   - Inner opcodes: render_opcodes.c dispatch table + RE analysis
 *     in library/state-machines/render-decoder-handlers.tsv
 *   - InfoDecoder opcodes (0x1c2..0x1d0):
 *     library/state-machines/info-decoder-replies.tsv
 *     library/journey/info-decoder-opcodes.md
 *   - Render decoder (encType=2): stage 30%+ gap — all handlers
 *     in render_opcodes.c are stubs (return 0) except viewport,
 *     scissor, blend_color, pipeline_state, draw_primitives.
 *   - Blit/compute decoders: scaffolded, observation-only.
 *
 * Stage 30%+ implementation status (see CLAUDE.md):
 *   - Stage 10% blocked by ABBA deadlock (WindowServer vs DisplayPipe)
 *   - Stage 20% (cursor + static UI): cursor wired but deadlock blocks
 *   - Stage 30%+ (actual rendering): render opcodes need real impl
 *
 * WindowServer command buffer hang (BLOCKER):
 *   - After deadlock resolved, WS may hang in command buffer submission
 *     via IOAccelClient2::commit_commands().
 *   - Guest expects GPU work to complete + stamp to advance.
 *   - If render opcodes are stubs (return 0), the guest's
 *     MTLCommandBuffer waitUntilCompleted may never return because
 *     the GPU "work" never actually runs — but stamps DO signal.
 *   - The real issue: guest may poll on IOAccelShared2 completion
 *     tokens that require actual GPU progress (not just stamp ack).
 *   - See journey/opcodes-0x35-0x36-0x39.md for related
 *     opcodes that fire during command buffer submission.
 *
 * CmdExecIndirect2: what the guest expects:
 *   - Guest sends cmdBuf segments with inner render/blit/compute opcodes.
 *   - Each segment's work should execute on the GPU (via Vulkan).
 *   - When complete: stamp advances + IRQ fires.
 *   - With stub handlers: stamps still signal, but no GPU work occurs.
 *   - Guest may detect "GPU hang" if work doesn't visibly progress.
 *
 * Priority for stage 30%+:
 *   1. Render opcodes (inner 0x20, encoder_type=2):
 *      PGDeserializerRenderDecoder handlers — needed for actual drawing.
 *      See render_opcodes.c for stub vs real status per opcode.
 *   2. Blit opcodes (encoder_type=4): buffer copies, texture blits.
 *   3. Compute opcodes (encoder_type=0/1): compute pipeline dispatch.
 *   4. InfoDecoder replies: mostly done (stubs return defaults).
 *
 * RE references:
 *   - paravirt-re/journey/opcodes-0x35-0x36-0x39.md
 *   - paravirt-re/library/state-machines/render-decoder-handlers.tsv
 *   - paravirt-re/M4-inner-opcode-implementation-guide.md
 */

#include "blit_decoder.h"
#include "compute_decoder.h"
#include "opcodes.h"
#include "protocol.h"
#include "render_decoder.h"
#include "state.h"
#include "render_opcodes.h"
#include "render_pass.h"
#include "../common/log.h"
#include "../device.h"
#include "../display.h"
#include "../memory/task.h"
#include "../vulkan/command.h"
#include "../translate/render_encoder.h"

#include <stdlib.h>
#include <string.h>

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* Phase 1 end-to-end glue: the empty-cmdbuf completion paths (0x22 and
 * 0x20 with count=0) represent the guest saying "commit this cmdbuf with
 * no GPU work". On real hardware Apple still drives the GPU's submission
 * pipeline — fence, wait, completion interrupt. We mirror that by driving
 * a fence-gated VkQueueSubmit of an empty command buffer via the
 * Phase 1.B.2 helper lagfx_vk_submit_empty().
 *
 * This is additive: the decoder's own completion path (stamp writeback +
 * shell IRQ) already ran by the time the handler returns. The vk submit
 * exercises the Vulkan queue round-trip so we can observe an end-to-end
 * success even though no rendering occurred. The call is guarded so:
 *
 *   - Tests that pass NULL device (pre-Phase-1.B fixtures) skip silently.
 *   - Builds without Vulkan (LAGFX_HAVE_VULKAN unset) land in the
 *     command.c no-op stub, which returns LAGFX_OK.
 *   - Runs where the Vulkan state exists but didn't initialise (Darwin
 *     dev host with no loadable ICD) return LAGFX_ERR_VULKAN_INIT from
 *     inside lagfx_vk_submit_empty; we log and carry on — the decoder's
 *     own completion path is correctness-critical, not this.
 */
static void lagfx_cmdbuf_commit_empty_vk_submit(lagfx_protocol_t *p,
                                                 uint16_t opcode,
                                                 uint32_t stamp) {
    if (!p || !p->dev) {
        return;
    }
    struct lagfx_vk_state *vk = p->dev->vk;
    if (!vk) {
        return;
    }
    lagfx_status_t vst = lagfx_vk_submit_empty(vk);
    if (vst != LAGFX_OK) {
        LAGFX_LOG("cmdbuf_commit_empty: vk submit skipped/failed "
                  "(opcode=0x%04x stamp=0x%08x status=%d)",
                  opcode, stamp, (int)vst);
    }
}

static void lagfx_render_encoder_try_begin(lagfx_protocol_t *p) {
#ifdef LAGFX_HAVE_VULKAN
    if (!p->dev || !p->dev->vk || !p->dev->vk->initialized) {
        return;
    }
    struct lagfx_vk_state *vk = p->dev->vk;
    lagfx_status_t st = lagfx_vk_begin_frame(vk);
    if (st != LAGFX_OK) {
        LAGFX_WARN("render_encoder_try_begin: begin_frame failed (%d)",
                   (int)st);
        return;
    }
    const lagfx_render_pass_desc_t *desc = lagfx_render_pass_desc_get();
    if (!desc) {
        LAGFX_WARN("render_encoder_try_begin: no render pass descriptor");
        return;
    }
    float clear[4] = {0};
    if (desc->color_attachment_count > 0) {
        for (unsigned c = 0; c < 4; c++) {
            clear[c] = (float)desc->colors[0].clear_color[c];
        }
    }
    VkCommandBuffer cmd = lagfx_vk_get_cmd_buf(vk);
    st = lagfx_translate_render_begin(vk, &p->render_enc,
        cmd, VK_NULL_HANDLE,
        (uint32_t)desc->render_target_width,
        (uint32_t)desc->render_target_height,
        clear, NULL, 0);
    if (st != LAGFX_OK) {
        LAGFX_WARN("render_encoder_try_begin: render_begin failed (%d)",
                   (int)st);
    }
#else
    (void)p;
#endif
}

/* ===========================================================================
 * CmdSynchronizeResources (0x22) — P0
 *
 * Request layout (re-followup-spec-gaps.md §4.3, HIGH 90%):
 *   payload[0..3]            u32 taskID
 *   payload[4..7]            u32 count
 *   payload[8..(8 + 4*count)] u32 resource_ids[count]
 *
 * Total payload bytes: 8 + 4*count. Minimum: 8 bytes (count=0, empty
 * list — the `[cmdbuf commit]` on empty cmdbuf path).
 *
 * Semantics (scaffold):
 *   - count=0: instant completion (dispatcher signals stamp).
 *   - count>0: for each ID, scan the fifo table and mark synced=true
 *     if present. This is a Phase 1.A.2 placeholder; full per-resource
 *     barrier tracking lands in Phase 3. The taskID is validated
 *     against the task table — unknown taskID is a warning but does
 *     not fail (fail-open).
 *
 * Overflow guard: we require payload_size >= 8 + 4*count, mirroring the
 * dylib's `cmpq %r13, %r15; ja fault` check at disasm line 71267–71268.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_synchronize_resources(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdSynchronizeResources: payload missing or too small "
                   "(size=%u, need >= 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint32_t count   = lagfx_le32(hdr->payload + 4);

    /* Overflow-safe size check: 8 + 4*count must fit within payload_size. */
    if (count > 0u && count > ((uint32_t)hdr->payload_size - 8u) / 4u) {
        char pay_hex[65];
        for (unsigned i = 0; i < hdr->payload_size && i < 64u; ++i) {
            snprintf(pay_hex + i*2, 3, "%02x", hdr->payload[i]);
        }
        pay_hex[hdr->payload_size < 64u ? hdr->payload_size * 2 : 128] = '\0';
        LAGFX_WARN("CmdSynchronizeResources: count=%u exceeds payload "
                   "(size=%u) taskID=%u hex=%s",
                   count, (unsigned)hdr->payload_size, task_id, pay_hex);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Empty list — pure completion vehicle. Matches metal-no-op. Drive
     * the Vulkan queue round-trip so the guest's empty-cmdbuf commit
     * becomes an end-to-end VkSubmit on the host. See helper comment
     * above for the guard rationale. */
    if (count == 0) {
        LAGFX_LOG("CmdSynchronizeResources: taskID=%u count=0 "
                  "(empty-list completion) stamp=0x%08x",
                  task_id, hdr->stamp);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_SYNCHRONIZE_RESOURCES,
                                            hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Non-empty: validate taskID against the task table. Unknown taskID
     * is logged but not fatal — fail-open per spec §6. */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdSynchronizeResources: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    /* Walk the id list and mark matching child-FIFO entries synced.
     * Phase 1.A.2 uses child-FIFO IDs as a crude stand-in for "resource
     * IDs" — real resources include textures/buffers/heaps which we do
     * not yet track; full resource table lands in Phase 2/3. */
    unsigned matched = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rid = lagfx_le32(hdr->payload + 8u + 4u * i);
        lagfx_childfifo_entry_t *fifo = lagfx_protocol_find_fifo(p, rid);
        if (fifo) {
            fifo->synced = true;
            matched++;
        }
    }

    LAGFX_LOG("CmdSynchronizeResources: taskID=%u count=%u matched=%u "
              "stamp=0x%08x",
              task_id, count, matched, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdExecIndirect2 (0x20 / kext-side 0x37) — M4 segment walker.
 *
 * WHY THIS MATTERS FOR WS HANG:
 *   WindowServer sends these to submit GPU work (rendering/blits/compute).
 *   If the guest's IOAccelClient2::commit_commands() waits for
 *   completion tokens that require actual GPU progress (not just
 *   stamp acks), the guest will hang here. Stage 30%+ render
 *   opcode implementations are needed so the GPU actually does work.
 *
 * RE confirmed (paravirt-re/M4-inner-opcode-implementation-guide.md §1.1,
 *     command-buffer-format.md):
 *   Kext emits 0x37 on per-channel rings (ch 1..4).
 *   Dylib emits 0x20 on RootChannel.
 *   Both carry the same outer payload format.
 *   CONFIRMED 2026-04-28 from live kext-side 0x37 ch=1 capture.
 *
 * Outer payload (CONFIRMED from kext-side 0x37 ch=1 capture):
 *     payload[0..3]   u32 task_id
 *     payload[4..7]   u32 descriptor_count   (24 B records, NOT 16 B)
 *     payload[8..11]  u32 resource_count
 *     payload[12..]   descriptor[descriptor_count]  (24 B each:
 *                      {u32 id, u32 flags, u64 reserved})
 *                      flags: 0x100=invalidate, 1=sync, 0=reference
 *     ...             resource_table[]  (16 B each:
 *                      {u64 host_gpu_addr, u32 length, u32 _pad})
 *
* Each resource_table entry's host_gpu_addr is a TASK-VIRTUAL address
 * (translated through the per-task PFN-array from CmdDefineHostTask 0x38).
 * It points at a per-resource cmdBuf containing:
    *   - Segment header: 16B metadata block, inner commands start at +16
 *     encoderType(1 @ +4: 0=compute, 1=compute-alt, 2=render, 4=blit),
 *     finalFlag(1 @ +5), reuseFlag(1 @ +6), pad(1 @ +7)
 *   - Nested PGCmdHeader streams (8 B: opcode, totalLength)
 * See paravirt-re/library/state-machines/segment-header-wire-format.md for 
 * segment header format analysis.

**CRITICAL:** Live guest traffic from macOS 15.7.5 shows encoderType at +0x04, NOT
+0x08 as originally claimed by framework binary RE. Invalid values (0x1a, 0x2c) appear
at +0x08 while valid encoder types (0=compute, 2=render) are found at +0x04. This 
document has been updated to reflect the actual wire format observed in live traffic.
 *
* Inner opcode families by encoder_type:
 *   - encType=0 (compute): lagfx_compute_decoder_dispatch()
 *     PGDeserializerComputeDecoder — compute pipeline dispatch.
 *     Scaffolded in compute_decoder.c — observation-only for now.
 *   - encType=1 (compute-alt): same as encType=0, alternate init path
 *   - encType=2 (render):  lagfx_render_decoder_dispatch()
 *     PGDeserializerRenderDecoder in render_opcodes.c.
 *     Most handlers are STUBS (return 0) — stage 30%+ gap.
 *     See render_opcodes.c header comment for stub vs real status.
 *   - encType=4 (blit):    lagfx_blit_decoder_dispatch()
 *     PGDeserializerBlitDecoder — buffer copies, texture blits.
 *     Scaffolded in blit_decoder.c — observation-only for now.
 *     Reply with sane defaults so SkyLight doesn't abort.
 *     See paravirt-re/library/state-machines/info-decoder-replies.tsv
 *
 * Stage 30%+ work (see CLAUDE.md):
 *   - Render opcodes (encType=2): Need real implementations
 *     for actual Metal drawing to appear.
 *   - Blit opcodes (encType=4): buffer copies, texture blits.
 *   - Compute opcodes (encType=0/1): compute pipeline dispatch.
 *
 * Empty-list case falls through to metal-no-op completion path.
 * The outer stamp is signalled by the dispatcher.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_exec_indirect2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* No payload or < 8 bytes: treat as empty-list completion (the
     * fallback metal-no-op path an implementation might choose instead
     * of CmdSynchronizeResources). Spec descriptor sets min_payload=0,
     * so this branch is valid. */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_TRACE("CmdExecIndirect2: stamp=0x%08x payload_size=%u "
                  "(empty-list completion — metal-no-op alternate path)",
                  hdr->stamp, (unsigned)hdr->payload_size);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                            hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Outer payload layout — CONFIRMED 2026-04-28 from live capture
     * of kext-side opcode 0x37 on ch=1 (per-channel exec ring):
     *
     *   +0x00  u32 task_id
     *   +0x04  u32 descriptor_count   (24 B records, NOT 16 B)
     *   +0x08  u32 resource_count
     *   +0x0c  descriptor[descriptor_count]  (24 B each: {u32 id,
     *          u32 flags, u64 reserved}) — flags 0x100=invalidate,
     *          1=sync, 0=reference
     *   +0x0c+dc*24  resource[resource_count]  (16 B each:
     *          {u64 host_gpu_addr, u32 length, u32 _pad})
     *
     * Previous M4 guide's 16-byte invalidate records + 16-byte
     * mid-section was WRONG — live capture clearly shows 24-byte
     * records with no mid-section.  The "16B mid" was actually the
     * last 8 bytes of one record + first 8 bytes of the next. */
    uint32_t task_id           = lagfx_le32(hdr->payload + 0);
    uint32_t descriptor_count  = lagfx_le32(hdr->payload + 4);
    uint32_t resource_count    = lagfx_le32(hdr->payload + 8);

    size_t off_descriptors = 12u;
    size_t off_resources   = off_descriptors
                             + (size_t)descriptor_count * 24u;
    size_t end_resources   = off_resources + (size_t)resource_count * 16u;

    if (end_resources > (size_t)hdr->payload_size) {
        LAGFX_WARN("CmdExecIndirect2: outer payload too small "
                   "(taskID=%u descriptor_count=%u resource_count=%u "
                   "need=%zu have=%u)",
                   task_id, descriptor_count, resource_count,
                   end_resources, (unsigned)hdr->payload_size);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                            hdr->stamp);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    if (descriptor_count == 0 && resource_count == 0) {
        LAGFX_TRACE("CmdExecIndirect2: taskID=%u empty (no descriptors, "
                  "no resources) stamp=0x%08x",
                  task_id, hdr->stamp);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                            hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    LAGFX_WARN("CmdExecIndirect2: processing taskID=%u descriptor_count=%u resource_count=%u",
              task_id, descriptor_count, resource_count);

    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdExecIndirect2: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    LAGFX_TRACE("CmdExecIndirect2: taskID=%u descriptor_count=%u "
              "resource_count=%u payload_size=%u stamp=0x%08x",
              task_id, descriptor_count, resource_count,
              (unsigned)hdr->payload_size, hdr->stamp);

    /* RE diagnostic: hex-dump full payload so we can see what's
     * actually on the wire and confirm or refute the task_id/ic/rc
     * layout for kext-side opcode 0x37. */
    {
        size_t n = (hdr->payload_size < 256u) ? hdr->payload_size : 256u;
        char buf[1024];
        size_t pos = 0;
        for (size_t i = 0; i + 3 < n && pos < 900; i += 4) {
            uint32_t v = lagfx_le32(hdr->payload + i);
            int w = snprintf(buf + pos, sizeof(buf) - pos,
                             "+%02zx=%08x ", i, v);
            if (w > 0) pos += (size_t)w;
        }
        LAGFX_LOG("  CmdExecIndirect2 dump: %s", buf);
    }

    for (uint32_t i = 0; i < descriptor_count; ++i) {
        const uint8_t *rec = hdr->payload + off_descriptors + (size_t)i * 24u;
        uint32_t rid   = lagfx_le32(rec + 0);
        uint32_t flags = lagfx_le32(rec + 4);
        LAGFX_TRACE("  descriptor[%u]: rid=0x%08x flags=0x%08x",
                  i, rid, flags);
    }

    /* Walk resource_table — each {host_gpu_addr, length} points at a
     * per-resource cmdBuf in IOAccelResource2 backing memory. For
     * each, read the cmdBuf via shell.read_memory and walk the
     * segment headers within. */
    for (uint32_t i = 0; i < resource_count; ++i) {
        const uint8_t *rec = hdr->payload + off_resources + (size_t)i * 16u;
        uint64_t host_gpu_addr =
            (uint64_t)lagfx_le32(rec + 0) |
            ((uint64_t)lagfx_le32(rec + 4) << 32);
        uint32_t length = lagfx_le32(rec + 8);
        uint32_t pad    = lagfx_le32(rec + 12);
        LAGFX_WARN("  resource[%u]: host_gpu_addr=0x%llx length=%u pad=0x%08x",
                  i, (unsigned long long)host_gpu_addr, length, pad);

        if (length == 0u || length > (1u << 22) /* 4 MiB cap */
            || p->dev == NULL || p->dev->desc.shell.read_memory == NULL) {
            continue;
        }

        /* Read the entire cmdBuf into a per-iteration buffer.
         *
         * host_gpu_addr is a TASK-VIRTUAL address. On real Apple hardware,
         * the host reads this data from the task's mapped VA window (the
         * zero-copy alias populated by mapMemory callbacks). We try that
         * path first: if the task has a shell_task with a populated VA
         * window, read directly from (base + host_gpu_addr) — no radix
         * tree walk, no dma_memory_read, just a host-side memcpy from
         * aliased pages that share physical backing with the guest.
         *
         * If the task VA window is not available (no shell_task, or the
         * range hasn't been mapped yet), fall back to the radix-tree
         * translate + dma_memory_read path. */
        uint8_t *cmdbuf = malloc(length);
        if (!cmdbuf) {
            LAGFX_WARN("  resource[%u]: malloc(%u) failed — skipping",
                       i, length);
            continue;
        }

        bool read_ok = false;
        if (task && task->shell_task) {
            void *task_base = lagfx_task_get_base_ptr(task->shell_task);
            if (task_base
                && host_gpu_addr + length <= task->length) {
                const uint8_t *src =
                    (const uint8_t *)task_base + host_gpu_addr;
                memcpy(cmdbuf, src, length);
                read_ok = true;
                LAGFX_LOG("  resource[%u]: read %u bytes from task VA "
                          "window (base=%p + offset=0x%llx)",
                          i, length, task_base,
                          (unsigned long long)host_gpu_addr);

                uint32_t first_word = lagfx_le32(cmdbuf);
                if (first_word == 0u) {
                    LAGFX_WARN("  resource[%u]: task VA window read "
                               "returned all-zero first word — pages "
                               "may not be mapped yet, trying DMA "
                               "fallback",
                               i);
                    read_ok = false;
                }
            }
        }

        if (!read_ok) {
            read_ok = true;
            uint32_t bytes_read = 0;
            while (bytes_read < length) {
                uint64_t cur_dev_addr = host_gpu_addr + bytes_read;
                uint64_t gpa = 0;
                uint64_t run = 0;
                bool translated = lagfx_task_translate(p, task_id, cur_dev_addr,
                                                       &gpa, &run);
                if (!translated) {
                    LAGFX_WARN("  resource[%u]: task-VA -> GPA translation failed "
                               "(dev_addr=0x%llx taskID=%u) — skipping segment",
                               i, (unsigned long long)cur_dev_addr, task_id);
                    read_ok = false;
                    break;
                }
                uint32_t this_chunk = (uint32_t)((run < (uint64_t)(length - bytes_read))
                                                 ? run : (length - bytes_read));
                if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                    gpa, this_chunk,
                                                    cmdbuf + bytes_read)) {
                    LAGFX_TRACE("  resource[%u]: read_memory failed at gpa=0x%llx "
                              "(dev=0x%llx, %u bytes) — skipping",
                              i, (unsigned long long)gpa,
                              (unsigned long long)cur_dev_addr, this_chunk);
                    read_ok = false;
                    break;
                }
                bytes_read += this_chunk;
            }
        }
        if (!read_ok) {
            free(cmdbuf);
            continue;
        }

        {
            size_t dump_n = (length < 512u) ? length : 512u;
            char dump_buf[4096];
            size_t dpos = 0;
            for (size_t di = 0; di < dump_n && dpos < 3900; di++) {
                int w = snprintf(dump_buf + dpos, sizeof(dump_buf) - dpos,
                                 "%02x", cmdbuf[di]);
                if (w > 0) dpos += (size_t)w;
                if ((di & 15) == 15 && di + 1 < dump_n) {
                    w = snprintf(dump_buf + dpos, sizeof(dump_buf) - dpos,
                                 "\n              ");
                    if (w > 0) dpos += (size_t)w;
                }
            }
        LAGFX_WARN("  resource[%u] first_8bytes: %02x%02x%02x%02x %02x%02x%02x%02x encType_at_8=0x%02x",
                    i, cmdbuf[0], cmdbuf[1], cmdbuf[2], cmdbuf[3],
                    cmdbuf[4], cmdbuf[5], cmdbuf[6], cmdbuf[7],
                    cmdbuf[8]);
    }

    size_t soff = 0;
        unsigned segment_idx = 0;
        /* Probe for segment header start: try both offset 0 and 8.
         * At each candidate, check if bytes 0-3 form a valid segmentSize
         * that fits within the buffer (size <= remaining space). */
        uint32_t probe_size_at_0 = lagfx_le32(cmdbuf + soff + 0);
        size_t seg_off[2] = { 0u, 8u };
        size_t best_off = (size_t)-1; /* Invalid sentinel */
        for (int i = 0; i < 2; i++) {
            size_t off = seg_off[i];
            if (soff + off + 16u > (size_t)length) continue;
            uint32_t seg_size = lagfx_le32(cmdbuf + soff + off + 0);
            /* Valid if: non-zero, fits in buffer. Don't validate encoderType here since
             * byte+8 might be first inner opcode (if off=8) or garbage. */
            if (seg_size == 0 || seg_size > (uint32_t)((size_t)length - soff - off)) {
                LAGFX_WARN("      probe off=%zu: seg_size=%u length=%zu soff=%zu FAIL",
                          off, seg_size, (size_t)length, soff);
                continue;
            }
            best_off = off;
            break; /* Prefer offset 0 if valid */
        }
        size_t segment_start_offset = best_off;
        
        /* If no valid segment header found, skip this resource. */
        if (segment_start_offset == (size_t)-1) {
            LAGFX_WARN("    No valid segment header found — skipping resource");
            free(cmdbuf);
            continue;
        }

         while (soff + segment_start_offset + 16u <= (size_t)length) {
              uint32_t segment_size =
                  lagfx_le32(cmdbuf + soff + segment_start_offset + 0);
           /* Segment header format: size(4) at +0, encoderType(1) at +4.
            * CONFIRMED from live macOS 15.7.5 traffic: encoderType is at byte +4, NOT +8.
            * Framework binary RE incorrectly claimed +0x08; live traffic shows invalid values (0x1a, 0x2c) there. */
              uint8_t  encoder_type = cmdbuf[soff + segment_start_offset + 4];

            LAGFX_WARN("    segment[%u]: seg_header_bytes_0to7=%02x%02x%02x%02x %02x%02x%02x%02x encType=0x%02x",
                      segment_idx, cmdbuf[soff + segment_start_offset],
                      cmdbuf[soff + segment_start_offset + 1],
                      cmdbuf[soff + segment_start_offset + 2],
                      cmdbuf[soff + segment_start_offset + 3],
                      cmdbuf[soff + segment_start_offset + 4],
                      cmdbuf[soff + segment_start_offset + 5],
                      cmdbuf[soff + segment_start_offset + 6],
                      cmdbuf[soff + segment_start_offset + 7],
                      encoder_type);

            if (segment_idx == 0u) {
                LAGFX_LOG("    segment[%u]: size=%u "
                          "encType=%u probe_size_at_0=0x%x resource_len=%u segment_start_off=%zu "
                          "hdr_bytes_0to7=%02x%02x%02x%02x %02x%02x%02x%02x "
                          "off=%zu taskID=%u",
                          segment_idx, segment_size, (unsigned)encoder_type,
                          probe_size_at_0, (unsigned)length, segment_start_offset,
                          cmdbuf[soff+segment_start_offset+0],
                          cmdbuf[soff+segment_start_offset+1],
                          cmdbuf[soff+segment_start_offset+2],
                          cmdbuf[soff+segment_start_offset+3],
                          cmdbuf[soff+segment_start_offset+4],
                          cmdbuf[soff+segment_start_offset+5],
                          cmdbuf[soff+segment_start_offset+6],
                          cmdbuf[soff+segment_start_offset+7],
                          soff, task_id);
            }

          if (segment_size == 0u
                   || segment_size > (uint32_t)((size_t)length - soff - segment_start_offset)) {
                 LAGFX_WARN("    segment[%u]: bad size — bailing out", segment_idx);
                 break;
             }

            /* Handle encoderType == 5: setProtectionOptions PREAMBLE.
             * When [+0x04] == 5, the framework reads an extra uint64_t protectionOptions,
             * then re-reads a fresh 8-byte segment header from that position and uses it. */
            bool render_begin_pending;
            size_t ioff;

            if (encoder_type == 5u) {
                 /* Read protectionOptions uint64_t from bytes +8..+15. */
                 uint64_t protection_options =
                     (uint64_t)lagfx_le32(cmdbuf + soff + segment_start_offset + 8) |
                     ((uint64_t)lagfx_le32(cmdbuf + soff + segment_start_offset + 12) << 32);

                 /* Re-read segment header from bytes +8 onward. */
                 uint32_t new_segment_size = lagfx_le32(cmdbuf + soff + segment_start_offset + 8);
                 encoder_type = cmdbuf[soff + segment_start_offset + 12]; /* [+0x04] of new header */

                 LAGFX_WARN("    segment[%u]: encType=5 preamble — protectionOptions=0x%llx, "
                            "new_header_size=%u new_encType=0x%02x (advancing by 8B)",
                            segment_idx, (unsigned long long)protection_options,
                            new_segment_size, encoder_type);

                 /* Update segment boundaries to use the re-read header. */
                 if (new_segment_size == 0u || new_segment_size > (uint32_t)((size_t)length - soff - segment_start_offset - 8)) {
                     LAGFX_WARN("    segment[%u]: preamble new size invalid — bailing", segment_idx);
                     break;
                 }

                /* Inner stream now starts at +16 from original offset (8B preamble + 8B new header). */
                   render_begin_pending =
                      ((encoder_type == 4u || encoder_type == 2u || encoder_type == 0u)
                       && !p->render_enc.in_pass);

                /* Inner command stream starts at offset +16 from segment_start (after 8B preamble + 8B new header). */
                   ioff = soff + segment_start_offset + 16u;

                 LAGFX_WARN("    segment[%u]: inner_stream_start=ioff=%zu encType=%u (post-preamble)",
                           segment_idx, ioff, encoder_type);
             } else {
                /* Walk inner cmds: 8-byte PGCmdHeader { u32 opcode; u32 totalLength }
                  * Inner stream starts at offset +8 from segment_start (after 8B header). */
                 render_begin_pending =
                    ((encoder_type == 4u || encoder_type == 2u || encoder_type == 0u)
                     && !p->render_enc.in_pass);

               /* Inner command stream starts at offset +8 from segment_start (after full 8B header).
              * Header layout: size(4 @ +0), encType(1 @ +4), reuseFlag(1 @ +5), keepFlag(1 @ +6), pad(1 @ +7). */
               ioff = soff + segment_start_offset + 8u;

             LAGFX_WARN("    segment[%u]: inner_stream_start=ioff=%zu encType=%u",
                       segment_idx, ioff, encoder_type);
            }
            size_t iend = soff + segment_size;
            unsigned inner_idx = 0;
           while (ioff + 8u <= iend) {
                LAGFX_WARN("    segment[%u]: byte at ioff=%zu = %02x%02x%02x%02x",
                          segment_idx, ioff, cmdbuf[ioff], cmdbuf[ioff+1],
                          cmdbuf[ioff+2], cmdbuf[ioff+3]);
                uint32_t inner_opcode = lagfx_le32(cmdbuf + ioff + 0);
                uint32_t inner_total  = lagfx_le32(cmdbuf + ioff + 4);
                LAGFX_WARN("      inner[%u]: raw[+0..+7]=%02x%02x%02x%02x %02x%02x%02x%02x op=0x%04x totalLen=%u",
                          inner_idx, cmdbuf[ioff], cmdbuf[ioff+1], cmdbuf[ioff+2], cmdbuf[ioff+3],
                          cmdbuf[ioff+4], cmdbuf[ioff+5], cmdbuf[ioff+6], cmdbuf[ioff+7],
                          inner_opcode, inner_total);
                if (inner_total < 8u || ioff + inner_total > iend) {
                    LAGFX_WARN("      inner[%u]: bad totalLength=%u — bailing",
                                inner_idx, inner_total);
                    break;
                }
                size_t ipl_len = (size_t)inner_total - 8u;
                LAGFX_WARN("      inner[%u]: op=0x%04x totalLen=%u (encType=%u)",
                          inner_idx, inner_opcode, inner_total,
                          (unsigned)encoder_type);

                /* InfoDecoder (encType=4) opcode 0x1c2..0x1d0 family.
                 *
                 * Every Info-class opcode (except 0x1c5, no reply) follows
                 * a common wire shape:
                 *
                 *   inner_payload[+0..+3]   u32 ref          (resource ID)
                 *   inner_payload[+4..+7]   u32 buffer_id    (resolved against
                 *                                             outer resource_table[])
                 *   inner_payload[+8..+15]  u64 reply_offset (within resolved buffer)
                 *   inner_payload[+16..]    per-opcode args  (size varies)
                 *
                 * For most ops {ref, buffer_id, reply_offset} sit at the very
                 * start. Exceptions:
                 *   - 0x1ca / 0x1cb: 24B MTLSize between ref and buffer_id
                 *     ({u32 ref, MTLSize(24B), u32 buffer_id, u64 reply_offset}).
                 *   - 0x1cc:        {u32 ref, u32 objectType, u32 buffer_id, u64 reply_offset}.
                 *
                 * Stubs reply with sane defaults (see per-opcode comments
                 * below) so future Metal clients hitting these don't read
                 * zeros and abort. Real implementations are a Phase 3 task.
                 *
                 * Refs:
                 *   - paravirt-re/library/state-machines/info-decoder-replies.tsv
                 *   - paravirt-re/library/journey/info-decoder-opcodes.md
                 *
                 * Helper macro used below for shell write + log/skip on
                 * page-cross or short run. */
                if (encoder_type == 4u
                    && inner_opcode >= 0x1c2u && inner_opcode <= 0x1d0u
                    && p->dev != NULL
                    && p->dev->desc.shell.write_memory != NULL) {
                    const uint8_t *ipl = cmdbuf + ioff + 8u;

                    /* Decode common {ref, buffer_id, reply_offset} triplet.
                     * Layout-specific opcodes (0x1ca/0x1cb/0x1cc) re-decode
                     * inside their case below. */
                    uint32_t ref          = 0;
                    uint32_t buffer_id    = 0;
                    uint64_t reply_offset = 0;
                    bool     have_triplet = false;
                    size_t   reply_size   = 0;
                    uint8_t  reply[32]    = {0};
                    const char *opname    = "(info-stub)";

                    switch (inner_opcode) {
                    case 0x1c2u:  /* ComputePipelineStateInfo — body 0x10 reply 0x1c */
                        if (ipl_len >= 16u) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            have_triplet = true;
                            reply_size   = 0x1c;
                            opname       = "0x1c2 ComputePipelineStateInfo";
                            /* Reply struct (28 B):
                             *   +0x00 u32 maxTotalThreadsPerThreadgroup
                             *   +0x04 u32 pad
                             *   +0x08 u32 threadExecutionWidth
                             *   +0x0c u32 pad
                             *   +0x10 u32 staticThreadgroupMemoryLength
                             *   +0x14 u32 pad
                             *   +0x18 u8  supportIndirectCommandBuffers
                             *   +0x19   pad[3]
                             * Per info-decoder-opcodes.md CONFIRMED layout.
                             * threadExecutionWidth MUST be non-zero —
                             * SkyLight divides by this value when
                             * computing threadgroup grid dimensions. */
                            /* maxTotalThreadsPerThreadgroup=1024 */
                            reply[0]  = 0x00; reply[1]  = 0x04;
                            reply[2]  = 0x00; reply[3]  = 0x00;
                            /* threadExecutionWidth=32 @ +8 */
                            reply[8]  = 32;   reply[9]  = 0;
                            reply[10] = 0;    reply[11] = 0;
                            /* staticThreadgroupMemoryLength=0 @ +16 */
                            /* supportIndirectCommandBuffers=0 @ +24 */
                        }
                        break;
                    case 0x1c3u:  /* HeapTextureDescriptorSizeAndAlign — body 0x2c reply 0x10 */
                        if (ipl_len >= 0x2cu) {
                            /* descriptor at +0..+0x1f, then buffer_id @ +0x20,
                             * reply_offset @ +0x24. ref slot is implicit
                             * (descriptor-only — no resource ref). Use 0. */
                            ref          = 0;
                            buffer_id    = lagfx_le32(ipl + 0x20);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 0x24) |
                                ((uint64_t)lagfx_le32(ipl + 0x28) << 32);
                            have_triplet = true;
                            reply_size   = 0x10;
                            opname       = "0x1c3 HeapTextureSizeAndAlign";
                            /* MTLSizeAndAlign { NSUInteger size; NSUInteger align; }
                             * size=4096, align=4096 (one page). */
                            reply[0] = 0x00; reply[1] = 0x10; reply[2] = 0; reply[3] = 0;
                            reply[4] = 0;    reply[5] = 0;    reply[6] = 0; reply[7] = 0;
                            reply[8] = 0x00; reply[9] = 0x10; reply[10] = 0; reply[11] = 0;
                            reply[12]= 0;    reply[13]= 0;    reply[14] = 0; reply[15] = 0;
                        }
                        break;
                    case 0x1c4u:  /* GetRasterizationRateMapInfo — body 0x18 reply 0x14+N*0x4 */
                        if (ipl_len >= 0x18u) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            have_triplet = true;
                            reply_size   = 0x14;  /* fixed header only */
                            opname       = "0x1c4 RasterizationRateMapInfo";
                            /* All zeros: zero screenSize, zero physicalGranularity,
                             * zero zoom buckets. Per-layer trailer not stubbed —
                             * guest will read zeros for each layer's physicalSize. */
                            LAGFX_LOG("        0x1c4 reply: variable trailer "
                                      "(per-layer physicalSize) not yet stubbed");
                        }
                        break;
                    case 0x1c5u:  /* CopyRasterizationRateParameterBuffer — no reply */
                        opname     = "0x1c5 CopyRasterizationRateParameterBuffer";
                        reply_size = 0;
                        LAGFX_LOG("        %s: no-op (no reply)", opname);
                        break;
                    case 0x1c6u:  /* MapPhysicalToScreenCoordinates — body 0x1c reply 0x8 */
                    case 0x1c7u:  /* MapScreenToPhysicalCoordinates  — body 0x1c reply 0x8 */
                        if (ipl_len >= 0x1cu) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            have_triplet = true;
                            reply_size   = 0x8;
                            opname = (inner_opcode == 0x1c6u)
                                ? "0x1c6 MapPhysicalToScreen"
                                : "0x1c7 MapScreenToPhysical";
                            /* Pass-through: copy MTLCoordinate2D @ +0x14
                             * (8B = {float x, float y}) to the reply slot. */
                            memcpy(reply, ipl + 0x14, 8);
                        }
                        break;
                    case 0x1c8u:  /* MapPhysicalToScreenCoordinatesMultiple — N*0x8 */
                        if (ipl_len >= 0x20u) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            opname       = "0x1c8 MapPhysicalToScreenMultiple";
                            /* Reply is purely variable (count × 8B). Stub
                             * with header-only no-op — the guest will read
                             * zeros for every coordinate, but at least we
                             * resolved the buffer. Don't write anything to
                             * avoid scribbling beyond the reply slot. */
                            LAGFX_LOG("        %s: variable-only reply (count*8B) "
                                      "not yet stubbed (ref=0x%x buffer_id=%u "
                                      "reply_offset=0x%llx)",
                                      opname, ref, buffer_id,
                                      (unsigned long long)reply_offset);
                            reply_size = 0;
                        }
                        break;
                    case 0x1c9u:  /* RenderPipelineStateInfo — body 0x10 reply 0xc */
                        if (ipl_len >= 16u) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            have_triplet = true;
                            reply_size   = 0xc;
                            opname       = "0x1c9 RenderPipelineStateInfo";
                            /* maxTotalThreadsPerThreadgroup=1024, rest zero. */
                            reply[0] = 0x00; reply[1] = 0x04;
                            reply[2] = 0x00; reply[3] = 0x00;
                        }
                        break;
                    case 0x1cau:  /* RenderPipelineImageBlockMemoryLength — body 0x28 reply 0x4 */
                    case 0x1cbu:  /* ComputePipelineImageBlockMemoryLength — body 0x28 reply 0x4 */
                        if (ipl_len >= 0x28u) {
                            ref          = lagfx_le32(ipl + 0);
                            /* 24B MTLSize at +4..+0x1b, then buffer_id @ +0x1c,
                             * reply_offset @ +0x20. */
                            buffer_id    = lagfx_le32(ipl + 0x1c);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 0x20) |
                                ((uint64_t)lagfx_le32(ipl + 0x24) << 32);
                            have_triplet = true;
                            reply_size   = 0x4;
                            opname = (inner_opcode == 0x1cau)
                                ? "0x1ca RenderPipelineImageBlockMemoryLength"
                                : "0x1cb ComputePipelineImageBlockMemoryLength";
                            /* u32 length = 0 (no imageblock memory). */
                        }
                        break;
                    case 0x1ccu:  /* ObjectUniqueIdentifier — body 0x14 reply 0x8 */
                        if (ipl_len >= 0x14u) {
                            ref          = lagfx_le32(ipl + 0);
                            /* +4 u32 objectType (1=texture, 9=buffer, 10=other) */
                            buffer_id    = lagfx_le32(ipl + 8);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 0xc) |
                                ((uint64_t)lagfx_le32(ipl + 0x10) << 32);
                            have_triplet = true;
                            reply_size   = 0x8;
                            opname       = "0x1cc ObjectUniqueIdentifier";
                            /* Echo ref as low 32 bits of u64 MTLResourceID;
                             * upper 32 bits 0. Stable per-resource identifier
                             * is the goal — using ref makes it deterministic. */
                            reply[0] = (uint8_t)(ref);
                            reply[1] = (uint8_t)(ref >> 8);
                            reply[2] = (uint8_t)(ref >> 16);
                            reply[3] = (uint8_t)(ref >> 24);
                            /* +4..7 already zero. */
                        }
                        break;
                    case 0x1cdu:  /* BufferHostResourceInfo  — body 0x10 reply 0x8 (zeros) */
                    case 0x1ceu:  /* TextureHostResourceInfo — body 0x10 reply 0x10 (zeros) */
                    case 0x1cfu:  /* HeapHostResourceInfo    — body 0x10 reply 0x8 (zeros) */
                    case 0x1d0u:  /* SamplerStateHostResourceInfo — body 0x10 reply 0x10 (zeros) */
                        if (ipl_len >= 16u) {
                            ref          = lagfx_le32(ipl + 0);
                            buffer_id    = lagfx_le32(ipl + 4);
                            reply_offset = (uint64_t)lagfx_le32(ipl + 8) |
                                ((uint64_t)lagfx_le32(ipl + 12) << 32);
                            have_triplet = true;
                            switch (inner_opcode) {
                            case 0x1cdu:
                                reply_size = 0x8;
                                opname     = "0x1cd BufferHostResourceInfo";
                                break;
                            case 0x1ceu:
                                reply_size = 0x10;
                                opname     = "0x1ce TextureHostResourceInfo";
                                break;
                            case 0x1cfu:
                                reply_size = 0x8;
                                opname     = "0x1cf HeapHostResourceInfo";
                                break;
                            default: /* 0x1d0 */
                                reply_size = 0x10;
                                opname     = "0x1d0 SamplerStateHostResourceInfo";
                                break;
                            }
                            /* reply[] already zero-initialised. */
                        }
                        break;
                    default:
                        /* Should be unreachable given the outer range check. */
                        break;
                    }

                    /* Common reply-target resolution + write. Skip for
                     * 0x1c5 (no reply) and the 0x1c8 variable-only path. */

                    /* Defensive: ensure 0x1c2 ComputePipelineStateInfo
                     * threadExecutionWidth (+8..+11) is never zero.
                     * A zero value causes SIGFPE in SkyLight when it
                     * computes threadgroup grid dimensions. */
                    if (inner_opcode == 0x1c2u && reply_size >= 12u) {
                        uint32_t tew = (uint32_t)reply[8]
                                     | ((uint32_t)reply[9] << 8)
                                     | ((uint32_t)reply[10] << 16)
                                     | ((uint32_t)reply[11] << 24);
                        if (tew == 0u) {
                            LAGFX_WARN("        0x1c2: threadExecutionWidth=0, "
                                       "forcing 32 (divide-by-zero guard)");
                            reply[8] = 32; reply[9] = 0;
                            reply[10] = 0; reply[11] = 0;
                        }
                    }

                    if (have_triplet && reply_size > 0u) {
                        if (buffer_id < resource_count) {
                            const uint8_t *brec = hdr->payload + off_resources
                                                  + (size_t)buffer_id * 16u;
                            uint64_t buffer_dev_addr =
                                (uint64_t)lagfx_le32(brec + 0) |
                                ((uint64_t)lagfx_le32(brec + 4) << 32);
                            uint32_t buffer_len = lagfx_le32(brec + 8);

                            if (reply_offset + (uint64_t)reply_size
                                    > (uint64_t)buffer_len) {
                                LAGFX_WARN("        %s reply: offset=0x%llx + "
                                           "%zu > buffer_len=%u — out of bounds",
                                           opname,
                                           (unsigned long long)reply_offset,
                                           reply_size, buffer_len);
                            } else {
                                /* Translate buffer_dev_addr + reply_offset
                                 * through the task's PFN-array. Reply sizes
                                 * are <= 32B and 4-byte aligned; in practice
                                 * never crosses a page. */
                                uint64_t target_dev_addr =
                                    buffer_dev_addr + reply_offset;
                                uint64_t target_gpa = 0;
                                uint64_t target_run = 0;
                                bool translated = lagfx_task_translate(
                                    p, task_id, target_dev_addr,
                                    &target_gpa, &target_run);

                                if (!translated) {
                                    LAGFX_WARN("        %s reply: task-VA -> GPA "
                                               "translation failed "
                                               "(dev_addr=0x%llx taskID=%u "
                                               "buffer_id=%u reply_offset=0x%llx "
                                               "buffer_dev_addr=0x%llx "
                                               "reply_size=%zu) — skipping reply",
                                               opname,
                                               (unsigned long long)target_dev_addr,
                                               task_id, buffer_id,
                                               (unsigned long long)reply_offset,
                                               (unsigned long long)buffer_dev_addr,
                                               reply_size);
                                } else if (target_run < (uint64_t)reply_size) {
                                    LAGFX_WARN("        %s reply: target run=%llu "
                                               "< %zu — would cross page (skip)",
                                               opname,
                                               (unsigned long long)target_run,
                                               reply_size);
                                } else if (p->dev->desc.shell.write_memory(
                                                p->dev->desc.shell.opaque,
                                                target_gpa,
                                                (uint32_t)reply_size, reply)) {
                                    LAGFX_LOG("        %s reply: ref=0x%x "
                                              "buffer_id=%u (dev=0x%llx len=%u) "
                                              "+offset=0x%llx -> gpa=0x%llx %zuB",
                                              opname, ref, buffer_id,
                                              (unsigned long long)buffer_dev_addr,
                                              buffer_len,
                                              (unsigned long long)reply_offset,
                                              (unsigned long long)target_gpa,
                                              reply_size);
                                } else {
                                    LAGFX_WARN("        %s reply: write_memory "
                                               "failed at gpa=0x%llx "
                                               "(taskID=%u buffer_id=%u "
                                               "reply_offset=0x%llx "
                                               "buffer_dev_addr=0x%llx "
                                               "reply_size=%zu)",
                                               opname,
                                               (unsigned long long)target_gpa,
                                               task_id, buffer_id,
                                               (unsigned long long)reply_offset,
                                               (unsigned long long)buffer_dev_addr,
                                               reply_size);
                                }
                            }
                       } else {
                            LAGFX_WARN("        %s reply: buffer_id=%u >= "
                                       "resource_count=%u — out of range",
                                       opname, buffer_id, resource_count);
                        }
                    }

                 LAGFX_WARN("    segment[%u]: dispatching inner op=0x%04x encType=%u",
                           segment_idx, inner_opcode, encoder_type);
                } /* end if (have_triplet && reply_size > 0u) */

                /* Dispatch render/blit/compute opcodes for ALL segments.
                 * This is separate from InfoDecoder reply handling above. */
                if (encoder_type == 2u) {
                    /* Render encoder (encType=2): Metal drawing commands.
                     * PGDeserializerRenderDecoder handlers are ALL STUBS
                     * (stage 30%+ gap). Without real implementations,
                     * the guest renders nothing — login screen shows
                     * but no window content draws.
                     *
                     * 0x1a = beginRender (triggers render_encoder_try_begin)
                     * All other render opcodes need real handlers.
                     */
                    const uint8_t *ipl = cmdbuf + ioff + 8u;
                    int rc = lagfx_render_decoder_dispatch(p, inner_opcode,
                                                          ipl, ipl_len);
                    if (rc != 0) {
                        LAGFX_TRACE("      inner[%u]: render dispatch op=0x%04x "
                                  "returned %d (continuing)",
                                  inner_idx, inner_opcode, rc);
                    }
                    if (render_begin_pending
                        && inner_opcode == 0x1au) {
                        render_begin_pending = false;
                        lagfx_render_encoder_try_begin(p);
                    }
                } else if (encoder_type == 4u) {
                    /* Blit encoder (encType=4): buffer copies,
                      * texture blits. PGDeserializerBlitDecoder is
                      * scaffolded — observation-only for now.
                      */
                    const uint8_t *bpl = cmdbuf + ioff + 8u;
                    int rc = lagfx_blit_decoder_dispatch(p, inner_opcode,
                                                         bpl, ipl_len);
                    if (rc != 0) {
                        LAGFX_TRACE("      inner[%u]: blit dispatch "
                                  "op=0x%04x returned %d (continuing)",
                                  inner_idx, inner_opcode, rc);
                    }
             } else if (encoder_type == 0u || encoder_type == 1u) {
                    /* Compute or compute-like segment: try compute decoder first, fall back to render. */
                    int rc = lagfx_compute_decoder_dispatch(p, inner_opcode, cmdbuf + ioff + 8u, ipl_len);

                    /* macOS sometimes sends render opcodes in encType=0 segments (e.g., 0x1a RENDER_DESCRIBE_RENDER_PASS).
                     * Force try render decoder if opcode looks like a known render op. */
                    int force_try_render = 0;
                    static const char *opnames[] = {
                        "0x00", "0x01", "0x02", "0x03", "0x04", "0x05", "0x06", "0x07",
                        "0x08", "0x09", "0x0a", "0x0b", "0x0c", "0x0d", "0x0e", "0x0f",
                        "0x10", "0x11", "0x12", "0x13", "0x14", "0x15", "0x16", "0x17",
                        "0x18", "0x19", "0x1a", "0x1b", "0x1c", "0x1d", "0x1e", "0x1f"
                    };
                    const char *opname = (inner_opcode < 32u) ? opnames[inner_opcode] : "???";

                    if (encoder_type == 0u || encoder_type == 1u) {
                        /* Render pass descriptor ops (0x1a-0x24), SET_RENDER_PIPELINE_STATE (0x74), etc. */
                        if ((inner_opcode >= 0x1au && inner_opcode <= 0x24u)
                            || inner_opcode == 0x74u || inner_opcode == 0x75u
                            || inner_opcode == 0x65u || inner_opcode == 0x66u) {
                            force_try_render = 1;
                        }
                    }

                    if (rc != LAGFX_HANDLER_OK || force_try_render) {
                        /* Fallback: treat as render for encType=0/1 (some macOS paths use compute path for render ops). */
                        int rc2 = lagfx_render_decoder_dispatch(p, inner_opcode, cmdbuf + ioff + 8u, ipl_len);

                        if (rc2 != LAGFX_HANDLER_OK) {
                            LAGFX_WARN("        %s: encoder_type=%u compute rc=%d fallback rc=%d%s — skipping",
                                       opname, encoder_type, rc, rc2, force_try_render ? " (forced)" : "");
                            LAGFX_TRACE("      inner[%u]: compute+render dispatch both failed",
                                        inner_idx);
                        } else if (force_try_render) {
                            /* Render opcode handled by render decoder even though it arrived in encType=0 segment. */
                            LAGFX_LOG("        %s: encoder_type=%u render fallback succeeded", opname, encoder_type);
                        }
                    }
                } else if (encoder_type == 2u) {
                    /* Render segment: always try render decoder first. */
                    int rc = lagfx_render_decoder_dispatch(p, inner_opcode, cmdbuf + ioff + 8u, ipl_len);

                    if (rc != LAGFX_HANDLER_OK) {
                        /* Fallback: treat as compute for encType=2 (rare macOS paths). */
                        int rc2 = lagfx_compute_decoder_dispatch(p, inner_opcode, cmdbuf + ioff + 8u, ipl_len);

                        if (rc2 != LAGFX_HANDLER_OK) {
                            static const char *opnames[] = {
                                "0x00", "0x01", "0x02", "0x03", "0x04", "0x05", "0x06", "0x07",
                                "0x08", "0x09", "0x0a", "0x0b", "0x0c", "0x0d", "0x0e", "0x0f",
                                "0x10", "0x11", "0x12", "0x13", "0x14", "0x15", "0x16", "0x17",
                                "0x18", "0x19", "0x1a", "0x1b", "0x1c", "0x1d", "0x1e", "0x1f"
                            };
                            const char *opname = (inner_opcode < 32u) ? opnames[inner_opcode] : "???";

                            LAGFX_WARN("        %s: encoder_type=%u render rc=%d fallback rc=%d — skipping",
                                       opname, encoder_type, rc, rc2);
                            LAGFX_TRACE("      inner[%u]: render+compute dispatch both failed",
                                        inner_idx);
                        }
                    }
                } /* end if/else dispatch based on encoder_type */

                ioff += inner_total;
                inner_idx += 1;
            }

            if (encoder_type == 2u) {
                if (p->render_enc.in_pass) {
                    lagfx_translate_render_end(&p->render_enc);
                }
#ifdef LAGFX_HAVE_VULKAN
                if (p->dev && p->dev->vk) {
                    lagfx_vk_end_frame(p->dev->vk);
                }
#endif
            }

            soff += segment_size;
            segment_idx += 1;
        }

        free(cmdbuf);
    }

    /* Ack the stamp regardless. Per IOAccelChannel2::incrementStamp the
     * kext bumps the per-slot counter by exactly +1 per submitCommands
     * and writes that value as header.stamp. The host writes
     * header.stamp back to the stamp cell via the monotonic floor
     * max(target, cur+1), which naturally produces the correct sequence.
     * No extra_stamp_advance is needed — one command = one stamp. */

    /* Ack the stamp regardless. Inner-opcode handlers will do their
     * actual work (when implemented) before this point — for now the
     * walk is observation-only. */
    lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                        hdr->stamp);
    return LAGFX_HANDLER_OK;
}
