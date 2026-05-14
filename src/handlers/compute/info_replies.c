/*
 * libapplegfx-vulkan — InfoDecoder reply handlers (inner opcodes 0x1c2..0x1d0)
 * src/handlers/compute/info_replies.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Restoration of the InfoDecoder reply path that was lost in the
 * 2026-05-12 dispatcher refactor (commit b652199 deleted
 * src/protocol/ops_cmdbuf.c, which carried 0x1c2..0x1d0 reply stubs).
 * Source-of-truth references:
 *
 *   - paravirt-re/library/state-machines/info-decoder-replies.tsv
 *     (RE-verified opcode → body/reply sizes, 15 entries)
 *   - paravirt-re/library/journey/info-decoder-opcodes.md
 *     (per-opcode field offsets + Metal selector mapping)
 *
 * Why this matters (Stage 20% blocker, 2026-05-13 evening):
 *
 *   The SkyLight compositor in the guest is aborting in
 *   MetalShader::CopyPipelineState → MetalCompositeSimpleColorLayer →
 *   MetalShader::CompilePipelineState. Pre-abort, SkyLight queries the
 *   Metal driver for pipeline-state capabilities via the Info-class
 *   inner opcodes 0x1c2 (ComputePipelineStateInfo) and 0x1c9
 *   (RenderPipelineStateInfo). Pre-refactor, ops_cmdbuf.c wrote
 *   non-zero replies (maxTotalThreadsPerThreadgroup=1024,
 *   threadExecutionWidth=32) into the buffer the guest had registered.
 *
 *   After the refactor the new exec_cmdbuf.c walker only OBSERVES the
 *   inner stream and never writes replies. SkyLight reads zeros for
 *   threadExecutionWidth, divides-by-zero when computing a threadgroup
 *   grid, and aborts. No encType=2 (render) segment ever appears.
 *
 *   This file restores the pre-refactor replies, RE-cross-checked
 *   against the TSV + journey doc. Where the journey doc only gives
 *   sizes (not field offsets) we use the documented size with all
 *   zeros and mark the case with TODO. The defensive guard on 0x1c2
 *   threadExecutionWidth survives — it was the load-bearing fix.
 *
 * Reply-buffer model (per journey/info-decoder-opcodes.md §"Reply
 * buffer model"):
 *
 *   For every Info-class opcode except 0x1c5 (no reply):
 *
 *     1. Inner payload carries {u32 ref, u32 buffer_id, u64
 *        reply_offset, ...args}. (Exceptions: 0x1c3 buffer_id at
 *        +0x20, 0x1ca/0x1cb at +0x1c with MTLSize between, 0x1cc with
 *        u32 objectType between ref and buffer_id.)
 *
 *     2. buffer_id is the INDEX into the outer CmdExecIndirect2
 *        resource_table[] (16-byte entries {u64 host_gpu_addr, u32
 *        length, u32 pad}). The host_gpu_addr is a task-virtual
 *        address.
 *
 *     3. Reply target GPA = task_translate(buffer_dev_addr +
 *        reply_offset) via the per-task radix tree at task->root_page_pfn.
 *
 *     4. Write exactly reply_size bytes via shell.write_memory.
 *
 *   Reply sizes are <= 32B and 4-byte-aligned; in practice they never
 *   cross a 4 KiB page, so a single translate + write is sufficient.
 */

#include "info_replies.h"
#include "common/le.h"
#include "common/log.h"

#include <string.h>

void lagfx_info_dispatch(lagfx_protocol_t *p,
                          uint32_t inner_opcode,
                          const uint8_t *body,
                          size_t body_len,
                          const uint8_t *outer_resources,
                          uint32_t resource_count,
                          const lagfx_task_entry_t *task,
                          lagfx_info_translate_fn translate) {
    if (!p || !p->dev || body == NULL) {
        return;
    }
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev->desc.shell.write_memory) {
        LAGFX_WARN("info_reply: no shell.write_memory — cannot reply to 0x%04x",
                   (unsigned)inner_opcode);
        return;
    }

    /* Common decode + per-opcode reply build. The values below mirror
     * pre-refactor ops_cmdbuf.c (af87e8c~1 .. b652199~1). See
     * info-decoder-opcodes.md per-opcode sections for field offsets. */
    uint32_t   ref          = 0;
    uint32_t   buffer_id    = 0;
    uint64_t   reply_offset = 0;
    bool       have_triplet = false;
    size_t     reply_size   = 0;
    uint8_t    reply[32]    = {0};
    const char *opname      = "(info-stub)";

    switch (inner_opcode) {
    case 0x1c2u:
        /* RE: info-decoder-opcodes.md §0x1c2 — ComputePipelineStateInfo
         * body=0x10, reply=0x1c. Reply struct:
         *   +0x00 u32 maxTotalThreadsPerThreadgroup
         *   +0x04 u32 pad
         *   +0x08 u32 threadExecutionWidth         (DIVIDE-BY-ZERO TRAP)
         *   +0x0c u32 pad
         *   +0x10 u32 staticThreadgroupMemoryLength
         *   +0x14 u32 pad
         *   +0x18 u8  supportIndirectCommandBuffers
         *   +0x19..0x1b pad
         */
        if (body_len >= 16u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            have_triplet = true;
            reply_size   = 0x1cu;
            opname       = "0x1c2 ComputePipelineStateInfo";
            /* maxTotalThreadsPerThreadgroup = 1024 (0x400) */
            lagfx_put_le32(reply + 0x00, 1024u);
            /* threadExecutionWidth = 32 (canonical SIMD width) */
            lagfx_put_le32(reply + 0x08, 32u);
            /* staticThreadgroupMemoryLength = 0, supportIndirectCommandBuffers = 0 */
        }
        break;

    case 0x1c3u:
        /* RE: info-decoder-opcodes.md §0x1c3 — HeapTextureDescriptorSizeAndAlign
         * body=0x2c (32B descriptor + u32 buffer_id at +0x20 + u64 reply_offset @ +0x24).
         * reply=0x10 = MTLSizeAndAlign { NSUInteger size; NSUInteger align; }.
         * Defaulting to 4096/4096 (one page). // TODO: verify against
         * kext expectations — kext may compute against texture descriptor. */
        if (body_len >= 0x2cu) {
            ref          = 0;  /* no resource ref in this opcode */
            buffer_id    = lagfx_le32(body + 0x20);
            reply_offset = lagfx_le64(body + 0x24);
            have_triplet = true;
            reply_size   = 0x10u;
            opname       = "0x1c3 HeapTextureSizeAndAlign";
            /* MTLSizeAndAlign { NSUInteger size; NSUInteger align; }.
             * NSUInteger is u64 on x86_64 macOS — the original
             * pre-refactor implementation wrote only the low 16 bits
             * of each field, which happens to round-trip 4096 = 0x1000
             * but would lose any value > 0xffff. Writing the full
             * 8-byte field is correct AND more defensive. */
            lagfx_put_le64(reply + 0x00, 4096u);
            lagfx_put_le64(reply + 0x08, 4096u);
        }
        break;

    case 0x1c4u:
        /* RE: info-decoder-opcodes.md §0x1c4 — GetRasterizationRateMapInfo
         * body=0x18, reply=0x14+N*0x4. We stub fixed 0x14 header zeros;
         * per-layer trailer not yet implemented. // TODO: per-layer
         * physicalSize trailer requires layerCount loop. */
        if (body_len >= 0x18u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            have_triplet = true;
            reply_size   = 0x14u;
            opname       = "0x1c4 RasterizationRateMapInfo";
            /* all zero — guest reads zero screenSize / physicalGranularity */
        }
        break;

    case 0x1c5u:
        /* RE: info-decoder-opcodes.md §0x1c5 — CopyRasterizationRateParameterBuffer
         * No reply; guest only registers buffer copy intent. */
        opname     = "0x1c5 CopyRasterizationRateParameterBuffer";
        reply_size = 0;
        LAGFX_LOG("info_reply: %s — no-op (no reply)", opname);
        return;

    case 0x1c6u:
    case 0x1c7u:
        /* RE: info-decoder-opcodes.md §0x1c6/0x1c7 — Map(Physical|Screen)ToScreenCoordinates
         * body=0x1c, reply=0x8 (MTLCoordinate2D = {float x; float y}).
         * Pass-through: echo the input coordinate @ +0x14. */
        if (body_len >= 0x1cu) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            have_triplet = true;
            reply_size   = 0x8u;
            opname = (inner_opcode == 0x1c6u)
                ? "0x1c6 MapPhysicalToScreen"
                : "0x1c7 MapScreenToPhysical";
            memcpy(reply, body + 0x14, 8);
        }
        break;

    case 0x1c8u:
        /* RE: info-decoder-opcodes.md §0x1c8 — MapPhysicalToScreenCoordinatesMultiple
         * body=0x20+N*0x8 (count at +0x18), reply=N*0x8 (variable, NOT
         * stubbed). Log and continue — guest reads zeros for every
         * sample-point coord, but at least we know it was requested. */
        if (body_len >= 0x20u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            opname       = "0x1c8 MapPhysicalToScreenMultiple";
            LAGFX_LOG("info_reply: %s ref=0x%x buffer_id=%u reply_offset=0x%llx — "
                      "variable reply not stubbed",
                      opname, ref, buffer_id, (unsigned long long)reply_offset);
        }
        return;  /* no reply emitted */

    case 0x1c9u:
        /* RE: info-decoder-opcodes.md §0x1c9 — RenderPipelineStateInfo
         * body=0x10, reply=0xc. Reply struct:
         *   +0x00 u32 maxTotalThreadsPerThreadgroup
         *   +0x04 u32 imageblockSampleLength
         *   +0x08 u8  threadgroupSizeMatchesTileSize
         *   +0x09 u8  supportIndirectCommandBuffers
         *   +0x0a..0x0b pad
         */
        if (body_len >= 16u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            have_triplet = true;
            reply_size   = 0xcu;
            opname       = "0x1c9 RenderPipelineStateInfo";
            /* maxTotalThreadsPerThreadgroup = 1024 — Apple GPUs advertise
             * 1024 as the canonical render threadgroup ceiling. */
            lagfx_put_le32(reply + 0x00, 1024u);
            /* imageblockSampleLength = 0, threadgroupSizeMatchesTileSize=0,
             * supportIndirectCommandBuffers=0 */
        }
        break;

    case 0x1cau:
    case 0x1cbu:
        /* RE: info-decoder-opcodes.md §0x1ca/0x1cb —
         * (Render|Compute)PipelineImageBlockMemoryLength
         * body=0x28: u32 ref @ +0, MTLSize (3*u64=24B) @ +4..+0x1b,
         * u32 buffer_id @ +0x1c, u64 reply_offset @ +0x20.
         * reply=0x4 (u32 length). 0 = no imageblock memory. */
        if (body_len >= 0x28u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 0x1c);
            reply_offset = lagfx_le64(body + 0x20);
            have_triplet = true;
            reply_size   = 0x4u;
            opname = (inner_opcode == 0x1cau)
                ? "0x1ca RenderPipelineImageBlockMemoryLength"
                : "0x1cb ComputePipelineImageBlockMemoryLength";
            /* u32 = 0 (no imageblock memory) */
        }
        break;

    case 0x1ccu:
        /* RE: info-decoder-opcodes.md §0x1cc — ObjectUniqueIdentifier
         * body=0x14: u32 ref @ +0, u32 objectType @ +4, u32 buffer_id
         * @ +8, u64 reply_offset @ +0xc. reply=0x8 (MTLResourceID = u64).
         * Echo ref into low 32 bits for a stable per-resource id. */
        if (body_len >= 0x14u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 8);
            reply_offset = lagfx_le64(body + 0xc);
            have_triplet = true;
            reply_size   = 0x8u;
            opname       = "0x1cc ObjectUniqueIdentifier";
            /* MTLResourceID = u64; echo ref into low 32 bits for a
             * stable per-resource id. High 32 bits stay zero. */
            lagfx_put_le32(reply + 0, ref);
        }
        break;

    case 0x1cdu:
    case 0x1ceu:
    case 0x1cfu:
    case 0x1d0u:
        /* RE: info-decoder-opcodes.md §0x1cd..0x1d0 — *HostResourceInfo
         * family. body=0x10 {u32 ref, u32 buffer_id, u64 reply_offset}.
         * Reply sizes vary (8 / 16 / 8 / 16) — all zeros (no host
         * gpuAddress / gpuResourceID published yet). // TODO: when we
         * implement Metal-side resources, populate gpuResourceID +
         * gpuAddress for buffer/texture/heap/sampler. */
        if (body_len >= 16u) {
            ref          = lagfx_le32(body + 0);
            buffer_id    = lagfx_le32(body + 4);
            reply_offset = lagfx_le64(body + 8);
            have_triplet = true;
            switch (inner_opcode) {
            case 0x1cdu:
                reply_size = 0x8u;
                opname     = "0x1cd BufferHostResourceInfo";
                break;
            case 0x1ceu:
                reply_size = 0x10u;
                opname     = "0x1ce TextureHostResourceInfo";
                break;
            case 0x1cfu:
                reply_size = 0x8u;
                opname     = "0x1cf HeapHostResourceInfo";
                break;
            default: /* 0x1d0 */
                reply_size = 0x10u;
                opname     = "0x1d0 SamplerStateHostResourceInfo";
                break;
            }
            /* reply[] all zero (default) */
        }
        break;

    default:
        /* Caller should restrict to 0x1c2..0x1d0, but be defensive. */
        LAGFX_TRACE("info_reply: opcode 0x%04x outside InfoDecoder range",
                    (unsigned)inner_opcode);
        return;
    }

    /* Defensive: never let 0x1c2 ship threadExecutionWidth=0 — SkyLight
     * divides by it. The case above already writes 32; this is the
     * belt-and-suspenders check that survived from pre-refactor. */
    if (inner_opcode == 0x1c2u && reply_size >= 12u) {
        uint32_t tew = lagfx_le32(reply + 8);
        if (tew == 0u) {
            LAGFX_WARN("info_reply: 0x1c2 threadExecutionWidth=0 — forcing 32");
            lagfx_put_le32(reply + 8, 32u);
        }
    }

    if (!have_triplet || reply_size == 0u) {
        LAGFX_TRACE("info_reply: %s — body_len=%zu too small or no-reply opcode",
                    opname, body_len);
        return;
    }

    /* Resolve buffer_id against outer resource_table */
    if (buffer_id >= resource_count) {
        LAGFX_WARN("info_reply: %s buffer_id=%u >= resource_count=%u — skip reply",
                   opname, buffer_id, resource_count);
        return;
    }
    if (!outer_resources) {
        LAGFX_WARN("info_reply: %s no resource_table available — skip reply",
                   opname);
        return;
    }
    const uint8_t *brec = outer_resources + (size_t)buffer_id * 16u;
    uint64_t buffer_dev_addr = lagfx_le64(brec + 0);
    uint32_t buffer_len      = lagfx_le32(brec + 8);

    if (reply_offset + (uint64_t)reply_size > (uint64_t)buffer_len) {
        LAGFX_WARN("info_reply: %s reply_offset=0x%llx + %zu > buffer_len=%u — skip",
                   opname, (unsigned long long)reply_offset,
                   reply_size, buffer_len);
        return;
    }

    /* Translate via per-task radix tree. Fall back to identity if no
     * task or translation fails (pre-2026-05-09 mode when 0x38 wasn't
     * firing; harmless because identity-VA pages tend to land in valid
     * GPA in single-task macOS boot). */
    uint64_t target_dev_addr = buffer_dev_addr + reply_offset;
    uint64_t target_gpa = 0;
    bool translated = false;
    if (task && translate) {
        translated = translate(p, task, target_dev_addr, &target_gpa);
    }
    if (!translated) {
        target_gpa = target_dev_addr;
        LAGFX_TRACE("info_reply: %s translate failed — using identity gpa=0x%llx",
                    opname, (unsigned long long)target_gpa);
    }

    if (dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                      target_gpa,
                                      (uint64_t)reply_size,
                                      reply)) {
        LAGFX_LOG("info_reply: %s ref=0x%x buffer_id=%u "
                  "buffer_dev=0x%llx len=%u reply_offset=0x%llx "
                  "-> gpa=0x%llx %zuB",
                  opname, ref, buffer_id,
                  (unsigned long long)buffer_dev_addr, buffer_len,
                  (unsigned long long)reply_offset,
                  (unsigned long long)target_gpa, reply_size);
    } else {
        LAGFX_WARN("info_reply: %s write_memory failed at gpa=0x%llx (reply_size=%zu)",
                   opname, (unsigned long long)target_gpa, reply_size);
    }
}
