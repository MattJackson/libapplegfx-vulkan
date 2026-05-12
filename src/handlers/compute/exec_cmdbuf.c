/*
 * libapplegfx-vulkan — Command buffer execution handler (opcode 0x18/0x20)
 * src/handlers/compute/exec_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers.h"
#include "common/log.h"

/* Little-endian u32/u64 readers (ring is LE on all hosts). */
static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
}

/* CmdExecIndirect2 outer payload format (confirmed 2026-04-28):
 *   +0x00  u32 task_id
 *   +0x04  u32 descriptor_count (24 B records)
 *   +0x08  u32 resource_count (16 B records)
 *   +0x0c+dc*24  descriptors[descriptor_count]
 *                +0x00  u32 id (resource/object ref ID)
 *                +0x04  u32 flags (0x100=invalidate, 1=sync)
 *                +0x08  u64 reserved (zero)
 *   +res  resources[resource_count]
 *                +0x00  u64 host_gpu_addr (device VA, NOT GPA!)
 *                +0x08  u32 length (cmdbuf size in bytes)
 *                +0x0c  u32 _pad (zero) */

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
    
    /* If payload is exactly 8 bytes, macOS is doing a minimal query.
     * descriptor_count and resource_count are not present — treat as
     * empty command (no descriptors, no resources). */
    if (hdr->payload_size == 8) {
        uint32_t task_id = lagfx_le32(hdr->payload);
        LAGFX_TRACE("CmdExecIndirect2: minimal query payload taskID=%u", task_id);
        return LAGFX_HANDLER_OK;
    }
    
    /* Payload >= 12 bytes: safe to read descriptor_count and resource_count */
    uint32_t task_id       = lagfx_le32(hdr->payload + 0);
    uint32_t descriptor_count = lagfx_le32(hdr->payload + 4);
    uint32_t resource_count   = lagfx_le32(hdr->payload + 8);
    
    LAGFX_LOG("CmdExecIndirect2: taskID=%u desc_count=%u res_count=%u payload_size=%u stamp=0x%08x",
              task_id, descriptor_count, resource_count, (unsigned)hdr->payload_size, hdr->stamp);
    
    /* Validate payload size against descriptor+resource counts. */
    uint32_t min_payload = 12 + 24 * descriptor_count + 16 * resource_count;
    if (hdr->payload_size < min_payload) {
        LAGFX_WARN("CmdExecIndirect2: payload too small for desc=%u res=%u (need %u, have %u)",
                   descriptor_count, resource_count, min_payload, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    /* Look up task. Unknown taskID is fail-open (resource table may still be valid). */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdExecIndirect2: taskID=%u not found (continuing fail-open)", task_id);
    }
    
    /* Validate resource table entries. Each is 16 bytes: u64 host_gpu_addr + u32 length + pad. */
    const uint8_t *res_start = hdr->payload + 12 + 24 * descriptor_count;
    for (uint32_t i = 0; i < resource_count; ++i) {
        const uint8_t *r = res_start + 16 * i;
        uint64_t host_gpu_addr = lagfx_le64(r + 0);
        uint32_t length = lagfx_le32(r + 8);
        
        LAGFX_TRACE("CmdExecIndirect2 resource[%u]: addr=0x%llx len=%u", i,
                    (unsigned long long)host_gpu_addr, length);
    }
    
    /* TODO: For each resource, map host_gpu_addr via task radix tree or VA→GPA table,
     *       then parse inner PGCmdHeader stream and dispatch to render/compute/blit decoders.
     *       This requires the AIR translation runtime for full Metal command execution. */
    
    LAGFX_LOG("CmdExecIndirect2: completed validation (inner cmdbuf parsing TODO)");
    return LAGFX_HANDLER_OK;
}
