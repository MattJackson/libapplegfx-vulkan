/*
 * libapplegfx-vulkan — misc opcode handlers (NOP, Debug, GetDeviceInfo)
 * src/protocol/ops_misc.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Handlers:
 *
 *   - CmdNOP (0x0e)  — accept, return success.
 *   - CmdDebug (0x0d) — log the debug payload, return success.
 *   - CmdGetDeviceInfo (0x0a) — legacy single-u32 key/value query;
 *     unused in modern macOS-15 boot but kept for completeness.
 *   - CmdGetDeviceInfo2 (0x3a) — M3+ extended TLV reply. Critical
 *     gate for Metal/SkyLight: the kext bulk-copies the 216-byte
 *     APVDeviceInfoStruct to the Metal dylib's _deviceInfo ivar;
 *     every per-Metal capability check (MSAA support, max threads
 *     per threadgroup, SupportsRasterSampleCount, …) reads those
 *     fields. If they're zero, SkyLight aborts in
 *     MetalShader::CopyPipelineState before any draw happens.
 *
 * See paravirt-re/library/state-machines/device-info-tlv-mapping.md
 * for the authoritative tag → field mapping and
 * paravirt-re/classes/AppleParavirtAccelerator-setupDeviceInfo-summary.md
 * for the wire-level handshake (request body is 12 bytes:
 * {kind=0x2a, size_qw=0x200, phys_pfn}; response is a TLV stream
 * at phys_pfn<<12 terminated by a key=0x00 sentinel).
 *
 * The completion stamp + IRQ are raised by the dispatcher AFTER
 * the handler returns, so DMA-writes here are observed by the
 * guest as soon as it services the IRQ.
 */

#include "handlers/handlers.h"
#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../device.h"
#include "../common/log.h"

#include <stdio.h>
#include <string.h>

/* Little-endian readers (guest protocol is LE on x86-64). */
static inline uint32_t lagfx_misc_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

lagfx_handler_status_t lagfx_op_nop(lagfx_protocol_t *p,
                                    const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    LAGFX_LOG("CmdNOP: stamp=0x%08x arg_count_8b=%u length=%u",
              hdr->stamp, (unsigned)hdr->arg_count_8b,
              (unsigned)hdr->length);
    return LAGFX_HANDLER_OK;
}

/* Debug commands carry an opaque payload (debugCode + bytes, per
 * the spec table). We log the first 16 bytes of payload to aid
 * bring-up without spamming multi-KB dumps. */
lagfx_handler_status_t lagfx_op_debug(lagfx_protocol_t *p,
                                      const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    if (hdr->payload && hdr->payload_size > 0) {
        /* Hex-dump up to 16 bytes. */
        char hex[3 * 16 + 1] = {0};
        size_t n = hdr->payload_size < 16 ? hdr->payload_size : 16;
        for (size_t i = 0; i < n; ++i) {
            snprintf(hex + (i * 3), sizeof(hex) - (i * 3),
                     "%02x ", hdr->payload[i]);
        }
        LAGFX_LOG("CmdDebug: stamp=0x%08x arg_count_8b=%u "
                  "payload_size=%u bytes=[%s%s]",
                  hdr->stamp, (unsigned)hdr->arg_count_8b,
                  (unsigned)hdr->payload_size,
                  hex,
                  hdr->payload_size > 16 ? "..." : "");
    } else {
        LAGFX_LOG("CmdDebug: stamp=0x%08x arg_count_8b=%u (no payload)",
                  hdr->stamp, (unsigned)hdr->arg_count_8b);
    }
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdGetDeviceInfo (0x0a) — P0 legacy single-u32 query
 *
 * Pre-M3 opcode for per-key device-info reads. Not used by macOS-15's
 * AppleParavirtGPU.kext (it goes straight to 0x3a), but we keep a
 * non-zero key/value table so any reader of the older protocol sees
 * sensible defaults. Payload: {u32 key, u32 outOffset, u32 flags}.
 * Reply: a single u32 DMA'd to outOffset (interpreted as a GPA).
 *
 * RE: paravirt-re/re-followup-spec-gaps.md §2 (request triple),
 *     pre-refactor src/protocol/ops_device.c (af87e8c~1).
 * =========================================================================== */
lagfx_handler_status_t lagfx_op_get_device_info(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("CmdGetDeviceInfo: payload too small (%u)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t key        = lagfx_misc_le32(hdr->payload + 0);
    uint32_t out_offset = lagfx_misc_le32(hdr->payload + 4);
    uint32_t flags      = lagfx_misc_le32(hdr->payload + 8);

    uint32_t value;
    switch (key) {
        case 0x0: value = 1u; break;                  /* protocol version 1 */
        case 0x1: value = LAGFX_MAX_TASKS; break;     /* maxTasks */
        case 0x2: value = LAGFX_MAX_FIFOS; break;     /* maxFIFOCount */
        case 0x3: value = 0u; break;                  /* deviceFeatureLevel */
        case 0x4: value = 0u; break;                  /* feature flags */
        default:  value = 0u; break;
    }

    LAGFX_LOG("CmdGetDeviceInfo: stamp=0x%08x key=0x%x out_off=0x%x flags=0x%x -> value=0x%x",
              hdr->stamp, key, out_offset, flags, value);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (dev && dev->desc.shell.write_memory && out_offset != 0u) {
        if (!dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                           (uint64_t)out_offset,
                                           sizeof(value), &value)) {
            LAGFX_WARN("CmdGetDeviceInfo: DMA writeback failed (gpa=0x%x)",
                       out_offset);
        }
    }
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdGetDeviceInfo2 (0x3a) — M3 GATE — TLV device-info reply
 *
 * Emitted by AppleParavirtAccelerator::setupDeviceInfo() during the
 * accelerator start() sequence; must complete before registerService()
 * fires and thus before MTLCreateSystemDefaultDevice() returns non-nil.
 *
 * Request payload (12 bytes):
 *   +0  u32 kind          observed 0x2a
 *   +4  u32 resp_qwords   response buffer CAPACITY in 8-byte units (0x200)
 *   +8  u32 resp_pfn      GPA of response buffer >> 12
 *
 * Response: TLV array at (resp_pfn << 12). Each entry is two u32:
 *   { u32 key; u32 value; }
 * Keys 0x01..0x29 = valid fields per device-info-tlv-mapping.md.
 * Key 0x00 = end-of-stream sentinel. Parser at kext vmaddr 0x1455d41d
 * stops on the first key=0 OR after resp_qwords/2 entries.
 *
 * Why this is the SkyLight CopyPipelineState abort gate (2026-05-13):
 *
 *   The 216-byte APVDeviceInfoStruct at accel+0xe68 is bulk-copied
 *   to userspace as the Metal dylib's _deviceInfo ivar. Metal-level
 *   capability methods read those fields directly:
 *
 *     supportsRasterSampleCount:N reads _deviceInfo.MSAASamples
 *     and returns (MSAASamples >= N) after gating against 0x10116.
 *     With MSAASamples=0 this returns NO for ALL N including 1 →
 *     CA::OGL::MetalContext::create_pipeline_state → SkyLight
 *     MetalShader::CopyPipelineState → abort().
 *
 *     newRenderPipelineStateWithDescriptor reads
 *     _deviceInfo.MaxMetalShaderVersion. The parser FORCES 0x20002
 *     when the tag is missing; populateAccelConfig clamps to 0x20007
 *     if < 0x20008 → driver falls back to legacy 2D path, no Metal
 *     compositor draws ever submit.
 *
 *   With a stub reply (the pre-fix state: just LAGFX_HANDLER_OK with
 *   no DMA write), every TLV field is zero, both gates fail, and
 *   SkyLight aborts BEFORE any encType=2 (render) CmdExecIndirect2
 *   inner stream is built. That's exactly the observation in
 *   project_m5_inflight.md (164+ CmdExecIndirect2 events seen, all
 *   encType=0 compute setup, zero encType=4 InfoDecoder traffic).
 *
 * RE sources:
 *   - paravirt-re/library/state-machines/device-info-tlv-mapping.md
 *     (authoritative tag → field mapping with value rationale)
 *   - paravirt-re/classes/AppleParavirtAccelerator-setupDeviceInfo-summary.md
 *     (wire handshake, parseDeviceInfo behavior)
 *   - pre-refactor src/protocol/ops_device.c (af87e8c~1 .. b652199~1)
 *
 * Cost: 42 pair-writes (41 fields + sentinel) = 336 bytes of DMA,
 * vs the 4 KiB response page the kext provisions. Single shell call.
 * =========================================================================== */
lagfx_handler_status_t lagfx_op_get_device_info_2(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("CmdGetDeviceInfo2: payload too small (%u)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t kind        = lagfx_misc_le32(hdr->payload + 0);
    uint32_t resp_qwords = lagfx_misc_le32(hdr->payload + 4);
    uint32_t resp_pfn    = lagfx_misc_le32(hdr->payload + 8);
    uint64_t resp_gpa    = (uint64_t)resp_pfn << 12;

    LAGFX_LOG("CmdGetDeviceInfo2: kind=0x%x resp_qwords=0x%x resp_pfn=0x%x -> gpa=0x%llx",
              kind, resp_qwords, resp_pfn, (unsigned long long)resp_gpa);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.write_memory || resp_pfn == 0u) {
        LAGFX_WARN("CmdGetDeviceInfo2: no write_memory or resp_pfn=0; "
                   "stamping with empty response");
        return LAGFX_HANDLER_OK;
    }

    /*
     * Full TLV table. Each pair = (u32 key, u32 value); sentinel 0x00
     * terminates. Values mirror pre-refactor ops_device.c and the
     * authoritative tag → field mapping. Trailing sentinel ensures
     * parseDeviceInfo halts even if resp_qwords > number of pairs.
     *
     * Load-bearing values (do NOT silently change):
     *   0x01 MSAASamples = 4   — supportsRasterSampleCount:1 gate
     *   0x12 MaxMetalShaderVersion = 0x20008 — modern Metal path
     *   0x0a DeserializerVersion = 0x20008 — matches above
     *   0x25 HostGPUFamily = 0x10005 (Apple5/Intel-like)
     *   0x03-0x05, 0x1d MaxThreadsPerTG = 1024 — divide-by-zero trap
     *
     * Anything else can be tuned later as capability fidelity grows.
     */
    struct { uint32_t key; uint32_t value; } pairs[] = {
        { 0x01, 4          }, /* MSAASamples (max MSAA 4x) */
        { 0x02, 1          }, /* D24S8Supported */
        { 0x03, 1024       }, /* MaxThreadsPerThreadgroupW */
        { 0x04, 1024       }, /* MaxThreadsPerThreadgroupH */
        { 0x05, 1024       }, /* MaxThreadsPerThreadgroupD */
        { 0x06, 32768      }, /* MaxThreadgroupMemoryLength */
        { 0x07, 1          }, /* IsFramebufferReadSupported */
        { 0x08, 1          }, /* IsRGB10A2GammaSupported */
        { 0x09, 0          }, /* SupportsNativeHardwareFP16 (sw lavapipe) */
        { 0x0a, 0x00020008 }, /* DeserializerVersion */
        { 0x0b, 0x7f       }, /* PrimitiveTypeSupport: all standard */
        { 0x0c, 0          }, /* SupportsMultiplaneTextures */
        { 0x0d, 256        }, /* LinearTextureAlignment */
        { 0x0e, 1          }, /* HeapBuffers */
        { 0x0f, 256        }, /* HeapBufferAlignment */
        { 0x10, 1          }, /* HeapTextures */
        { 0x11, 1          }, /* BufferFromIOSurface */
        { 0x12, 0x00020008 }, /* MaxMetalShaderVersion — modern path */
        { 0x13, 0          }, /* SupportsSharedTextures */
        { 0x14, 1          }, /* MaxVertexAmplificationCount */
        { 0x15, 0          }, /* SupportsProgrammableSamplePositions */
        { 0x16, 0          }, /* RasterizationRateLayerCount */
        { 0x17, 0          }, /* TileShaders */
        { 0x18, 0          }, /* ImageBlocks */
        { 0x19, 0          }, /* RasterOrderGroups */
        { 0x1a, 0          }, /* MemoryOrderAtomics */
        { 0x1b, 0          }, /* LargeMRT */
        { 0x1c, 0          }, /* SupportFlags2023 */
        { 0x1d, 1024       }, /* MaxTotalComputeThreadsPerThreadgroup */
        { 0x1e, 32768      }, /* MaxComputeLocalMemorySizes */
        { 0x1f, 32768      }, /* MaxComputeThreadgroupMemory */
        { 0x20, 256        }, /* MaxComputeThreadgroupMemoryAlignmentBytes */
        { 0x21, 0          }, /* SupportFlags2024 */
        { 0x22, 1          }, /* GpuCoreCount */
        { 0x23, 2048       }, /* MaxTextureLayers */
        { 0x24, 0          }, /* MaxPredicatedNestingDepth */
        { 0x25, 0x10005    }, /* HostGPUFamily: Apple5 */
        { 0x26, 1          }, /* ArgumentBuffersTier */
        { 0x27, 1024       }, /* ArgumentBuffersMaxSamplerCount */
        { 0x28, 256        }, /* MinimumLinearTextureAlignment */
        { 0x29, 0          }, /* SupportedTextureWriteRoundingModes */
        { 0x00, 0          }, /* end-of-stream sentinel */
    };
    const size_t n_pairs = sizeof(pairs) / sizeof(pairs[0]);

    /* resp_qwords is in 8-byte units; each pair is 8 bytes, so the
     * capacity in pairs is resp_qwords (one qword == one pair). Clamp
     * defensively. In practice n_pairs=42 vs resp_qwords=0x200=512. */
    size_t emit_pairs =
        (n_pairs <= resp_qwords) ? n_pairs : (size_t)resp_qwords;

    if (!dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                       resp_gpa,
                                       emit_pairs * sizeof(pairs[0]),
                                       pairs)) {
        LAGFX_WARN("CmdGetDeviceInfo2: DMA writeback failed to gpa=0x%llx",
                   (unsigned long long)resp_gpa);
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    LAGFX_LOG("CmdGetDeviceInfo2: wrote %zu TLV pairs to resp page "
              "(MSAA=4, MaxMetalShaderVersion=0x20008, HostGPUFamily=0x10005)",
              emit_pairs);
    return LAGFX_HANDLER_OK;
}
