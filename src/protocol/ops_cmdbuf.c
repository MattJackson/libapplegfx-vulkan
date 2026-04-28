/*
 * libapplegfx-vulkan — cmdbuf-domain opcode handlers
 * src/protocol/ops_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 real handlers for the command-buffer submission path:
 *
 *   CmdSynchronizeResources (0x22) P0 — implemented. Per
 *     re-followup-spec-gaps.md §4 (HIGH, 90%): payload is
 *     {u32 taskID, u32 count, u32 resource_ids[count]}, total 8+4N
 *     bytes. count=0 is the completion path for `[cmdbuf commit]` on
 *     an empty cmdbuf (plan §6.2).
 *
 *   CmdExecIndirect2 (0x20) P1 — scaffolded. Per
 *     re-followup-spec-gaps.md R2 and phase-1a2-decoder-plan.md §6.2,
 *     empty-list case (count=0) is the alternate completion path for
 *     `[cmdbuf commit]` on an empty cmdbuf; count>0 path is scaffolded
 *     by walking the list and marking implicated child-FIFOs synced
 *     (mirroring CmdSynchronizeResources count>0 behaviour). Real
 *     indirect-exec dispatch is Phase 3.
 */

#include "blit_decoder.h"
#include "opcodes.h"
#include "protocol.h"
#include "render_decoder.h"
#include "state.h"
#include "../common/log.h"
#include "../device.h"
#include "../vulkan/command.h"

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
        /* Test fixture path — no device attached. Silent skip. */
        return;
    }
    struct lagfx_vk_state *vk = p->dev->vk;
    if (!vk) {
        /* Should not happen post-1.B (device_new always populates vk)
         * but be defensive. */
        return;
    }
    lagfx_status_t vst = lagfx_vk_submit_empty(vk);
    if (vst != LAGFX_OK) {
        /* Not fatal for the guest-visible command completion. On Darwin
         * dev hosts with no ICD this will fire on every empty-cmdbuf
         * commit; LAGFX_LOG keeps it at the usual verbosity. */
        LAGFX_LOG("cmdbuf_commit_empty: vk submit skipped/failed "
                  "(opcode=0x%04x stamp=0x%08x status=%d)",
                  opcode, stamp, (int)vst);
    }
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
    if (count > ((uint32_t)hdr->payload_size - 8u) / 4u) {
        LAGFX_WARN("CmdSynchronizeResources: count=%u exceeds payload "
                   "(size=%u)", count, (unsigned)hdr->payload_size);
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
 * Outer payload layout (CONFIRMED from kext-side 0x37 ch=1 capture
 * 2026-04-26; see body comment below for details):
 *
 *     payload[0..3]   u32 task_id
 *     payload[4..7]   u32 invalidate_count          (16B records)
 *     payload[8..11]  u32 resource_count
 *     payload[12..]   invalidate_record[]           (16B each)
 *     ...             16B reserved/middle block
 *     ...             resource_table[]              (16B each:
 *                                                    {u64 host_gpu_addr,
 *                                                     u32 length, u32 _pad})
 *
 * Each resource_table entry's host_gpu_addr is a TASK-VIRTUAL address
 * (translated through the per-task PFN-array from CmdDefineHostTask 0x38)
 * pointing at a per-resource cmdBuf containing
 * PGSerializerCommandSegmentHeader records + nested PGCmdHeader streams.
 *
 * The handler walks each cmdBuf, decodes segment + inner-cmd headers,
 * and currently only emits a 12B PGReplyRenderPipelineStateInfo for
 * InfoDecoder opcode 0x1cc (decodeRenderPipelineStateInfo) so the dylib
 * can finish MetalShader::CopyPipelineState. All other inner opcodes
 * are observation-only (logged for RE follow-up).
 *
 * Empty-list / short-payload cases fall through to the metal-no-op
 * empty-cmdbuf completion path. The outer stamp is signalled
 * unconditionally by the dispatcher.
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

        /* Read the entire cmdBuf into a per-iteration buffer. host_gpu_addr
         * is a TASK-VIRTUAL address (not a literal GPA) — we translate
         * each page through the task's root_page_pfn PFN-array (published
         * by CmdDefineHostTask 0x38). Same pattern as the per-channel
         * ring's PFN-array indirection. Reads page-by-page so that a
         * cmdBuf spanning multiple non-contiguous physical pages still
         * works.
         *
         * Falls back to treating host_gpu_addr as a literal GPA if no
         * host-task is registered (defensive — covers the dylib-emitted
         * 0x20 path on the RootChannel where host_gpu_addr might be a
         * direct GPA). */
        uint8_t *cmdbuf = malloc(length);
        if (!cmdbuf) {
            LAGFX_WARN("  resource[%u]: malloc(%u) failed — skipping",
                       i, length);
            continue;
        }

        bool read_ok = true;
        uint32_t bytes_read = 0;
        while (bytes_read < length) {
            uint64_t cur_dev_addr = host_gpu_addr + bytes_read;
            uint64_t gpa = 0;
            uint64_t run = 0;
            bool translated = lagfx_task_translate(p, task_id, cur_dev_addr,
                                                   &gpa, &run);
            if (!translated) {
                /* Fallback: literal GPA. */
                gpa = cur_dev_addr;
                run = 0x1000u - (cur_dev_addr & 0xfffu);
            }
            uint32_t this_chunk = (uint32_t)((run < (uint64_t)(length - bytes_read))
                                             ? run : (length - bytes_read));
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                gpa, this_chunk,
                                                cmdbuf + bytes_read)) {
                LAGFX_TRACE("  resource[%u]: read_memory failed at gpa=0x%llx "
                          "(dev=0x%llx, %u bytes, translated=%d) — skipping",
                          i, (unsigned long long)gpa,
                          (unsigned long long)cur_dev_addr, this_chunk,
                          translated ? 1 : 0);
                read_ok = false;
                break;
            }
            bytes_read += this_chunk;
        }
        if (!read_ok) {
            free(cmdbuf);
            continue;
        }

        /* Walk PGSerializerCommandSegmentHeader (16 B) records. Per
         * library/state-machines/inner-opcode-format.md. */
        size_t soff = 0;
        unsigned segment_idx = 0;
        while (soff + 16u <= (size_t)length) {
            uint32_t segment_size =
                lagfx_le32(cmdbuf + soff + 0);
            uint32_t prot_options =
                lagfx_le32(cmdbuf + soff + 4);
            uint8_t  encoder_type = cmdbuf[soff + 8];
            uint8_t  final_flag   = cmdbuf[soff + 9];
            uint8_t  reuse_flag   = cmdbuf[soff + 10];

            if (segment_idx == 0u) {
                LAGFX_LOG("    segment[%u]: size=%u prot=0x%x "
                          "encType=%u final=%u reuse=%u "
                          "hdr=%02x%02x%02x%02x %02x%02x%02x%02x "
                          "%02x%02x%02x%02x %02x%02x%02x%02x "
                          "off=%zu taskID=%u",
                          segment_idx, segment_size, prot_options,
                          (unsigned)encoder_type, (unsigned)final_flag,
                          (unsigned)reuse_flag,
                          cmdbuf[soff+0], cmdbuf[soff+1],
                          cmdbuf[soff+2], cmdbuf[soff+3],
                          cmdbuf[soff+4], cmdbuf[soff+5],
                          cmdbuf[soff+6], cmdbuf[soff+7],
                          cmdbuf[soff+8], cmdbuf[soff+9],
                          cmdbuf[soff+10], cmdbuf[soff+11],
                          cmdbuf[soff+12], cmdbuf[soff+13],
                          cmdbuf[soff+14], cmdbuf[soff+15],
                          soff, task_id);
            }

            if (segment_size == 0u
                || segment_size > (uint32_t)((size_t)length - soff - 16u)) {
                LAGFX_WARN("    segment[%u]: bad size — bailing out", segment_idx);
                break;
            }

            /* Walk inner cmds: 8-byte PGCmdHeader { u32 opcode; u32 totalLength }
             * then totalLength-8 bytes payload. */
            size_t ioff = soff + 16u;
            size_t iend = ioff + segment_size;
            unsigned inner_idx = 0;
            while (ioff + 8u <= iend) {
                uint32_t inner_opcode = lagfx_le32(cmdbuf + ioff + 0);
                uint32_t inner_total  = lagfx_le32(cmdbuf + ioff + 4);
                if (inner_total < 8u || ioff + inner_total > iend) {
                    LAGFX_WARN("      inner[%u]: bad totalLength=%u — bailing",
                               inner_idx, inner_total);
                    break;
                }
                size_t ipl_len = (size_t)inner_total - 8u;
                LAGFX_TRACE("      inner[%u]: op=0x%04x totalLen=%u (encType=%u)",
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
                            /* maxTotalThreadsPerThreadgroup=1024 */
                            reply[0]  = 0x00; reply[1]  = 0x04;
                            reply[2]  = 0x00; reply[3]  = 0x00;
                            /* +4..7 pad. threadExecutionWidth=32 @ +8 */
                            reply[8]  = 32;   reply[9]  = 0;
                            reply[10] = 0;    reply[11] = 0;
                            /* +12..15 pad. staticThreadgroupMemoryLength=0 @ +16 */
                            /* +20..23 pad. supportIndirectCommandBuffers=0 @ +24 */
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
                                    /* Fallback: literal GPA. */
                                    target_gpa = target_dev_addr;
                                    target_run = 0x1000u
                                        - (target_dev_addr & 0xfffu);
                                }

                                if (target_run < (uint64_t)reply_size) {
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
                                              "+offset=0x%llx -> gpa=0x%llx %zuB "
                                              "(translated=%d)",
                                              opname, ref, buffer_id,
                                              (unsigned long long)buffer_dev_addr,
                                              buffer_len,
                                              (unsigned long long)reply_offset,
                                              (unsigned long long)target_gpa,
                                              reply_size,
                                              translated ? 1 : 0);
                                } else {
                                    LAGFX_WARN("        %s reply: write_memory "
                                               "failed at gpa=0x%llx",
                                               opname,
                                               (unsigned long long)target_gpa);
                                }
                            }
                        } else {
                            LAGFX_WARN("        %s reply: buffer_id=%u >= "
                                       "resource_count=%u — out of range",
                                       opname, buffer_id, resource_count);
                        }
                    }
                } else if (encoder_type == 2u) {
                    const uint8_t *ipl = cmdbuf + ioff + 8u;
                    int rc = lagfx_render_decoder_dispatch(p, inner_opcode,
                                                          ipl, ipl_len);
                    if (rc != 0) {
                        LAGFX_TRACE("      inner[%u]: render dispatch op=0x%04x "
                                  "returned %d (continuing)",
                                  inner_idx, inner_opcode, rc);
                    }
                } else if (encoder_type == 0u) {
                    {
                        const uint8_t *bpl = cmdbuf + ioff + 8u;
                        int rc = lagfx_blit_decoder_dispatch(p, inner_opcode,
                                                             bpl, ipl_len);
                        if (rc != 0) {
                            LAGFX_TRACE("      inner[%u]: blit dispatch "
                                      "op=0x%04x returned %d (continuing)",
                                      inner_idx, inner_opcode, rc);
                        }
                    }
                } else if (encoder_type == 1u) {
                    LAGFX_TRACE("      inner[%u]: compute op=0x%04x len=%zu "
                              "(no dispatch module)",
                              inner_idx, inner_opcode, ipl_len);
                }

                ioff += inner_total;
                inner_idx += 1;
            }

            if (final_flag == 0u && reuse_flag == 0u) {
                /* End of encoder — drop and look for next encoder
                 * segment. */
            }

            soff += 16u + segment_size;
            segment_idx += 1;
        }

        free(cmdbuf);
    }

    /* Ack the stamp regardless. Inner-opcode handlers will do their
     * actual work (when implemented) before this point — for now the
     * walk is observation-only. */
    lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                        hdr->stamp);
    return LAGFX_HANDLER_OK;
}
