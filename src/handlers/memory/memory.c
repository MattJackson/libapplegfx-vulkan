/*
 * libapplegfx-vulkan — Memory mapping handlers (opcode 0x02, 0x03)
 * src/handlers/memory/memory.c
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

lagfx_handler_status_t lagfx_memory_map_memory2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    
    /* CmdMapMemory2 payload (variable):
     *   +0  u32 task_id
     *   +4  u64 virtual_offset (guest VA offset within task)
     *   +12 u32 read_only flag (bit 0 = RO, rest reserved)
     *   +16 u32 range_count
     *   +20 16-byte ranges[range_count]: {u64 gpa, u64 length} */
    if (!hdr->payload || hdr->payload_size < 20) {
        LAGFX_WARN("CmdMapMemory2: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    uint32_t task_id      = lagfx_le32(hdr->payload + 0);
    uint64_t virtual_off  = lagfx_le64(hdr->payload + 4);
    uint32_t read_only    = lagfx_le32(hdr->payload + 12);
    uint32_t range_count  = lagfx_le32(hdr->payload + 16);
    
    /* Overflow-safe size check: each range is 16 bytes. */
    if (range_count > ((uint32_t)hdr->payload_size - 20u) / 16u) {
        LAGFX_WARN("CmdMapMemory2: range_count=%u exceeds payload (size=%u)",
                   range_count, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    /* Look up task. Unknown taskID is fail-open (map may accept NULL shell_task). */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdMapMemory2: taskID=%u not found (continuing fail-open)", task_id);
    }
    
    /* Empty ranges: valid no-op. */
    if (range_count == 0) {
        LAGFX_LOG("CmdMapMemory2: taskID=%u vm_off=0x%llx ro=%u range_count=0",
                  task_id, (unsigned long long)virtual_off, read_only & 1u);
        return LAGFX_HANDLER_OK;
    }
    
    /* Build ranges array for shell callback. */
    enum { LAGFX_MAP_MAX_RANGES = 64 };
    if (range_count > LAGFX_MAP_MAX_RANGES) {
        LAGFX_WARN("CmdMapMemory2: range_count=%u exceeds batch cap %u",
                   range_count, LAGFX_MAP_MAX_RANGES);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    lagfx_physical_range_t ranges[LAGFX_MAP_MAX_RANGES];
    for (uint32_t i = 0; i < range_count; ++i) {
        const uint8_t *r = hdr->payload + 20u + 16u * i;
        ranges[i].guest_physical_address = lagfx_le64(r + 0);
        ranges[i].length                 = lagfx_le64(r + 8);
    }
    
    /* Call shell.map_memory if available. */
    bool ok = true;
    if (p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        lagfx_task_t *shell_task = task ? task->shell_task : NULL;
        if (dev->desc.shell.map_memory) {
            ok = dev->desc.shell.map_memory(
                dev->desc.shell.opaque, shell_task, virtual_off,
                ranges, (size_t)range_count, (read_only & 1u) != 0);
        } else {
            LAGFX_WARN("CmdMapMemory2: no shell.map_memory callback for taskID=%u", task_id);
        }
    }
    
    LAGFX_LOG("CmdMapMemory2: taskID=%u vm_off=0x%llx ro=%u range_count=%u status=%d",
              task_id, (unsigned long long)virtual_off, read_only & 1u,
              range_count, ok ? 0 : -1);
    
    return ok ? LAGFX_HANDLER_OK : LAGFX_HANDLER_ERR_STATE;
}

lagfx_handler_status_t lagfx_memory_unmap_memory(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    
    /* CmdUnmapMemory payload (20 bytes):
     *   +0  u32 task_id
     *   +4  u64 virtual_offset
     *   +12 u64 length */
    if (!hdr->payload || hdr->payload_size < 20) {
        LAGFX_WARN("CmdUnmapMemory: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    uint32_t task_id      = lagfx_le32(hdr->payload + 0);
    uint64_t virtual_off  = lagfx_le64(hdr->payload + 4);
    uint64_t length       = lagfx_le64(hdr->payload + 12);
    
    /* Look up task. Unknown taskID is fail-open. */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdUnmapMemory: taskID=%u not found (continuing fail-open)", task_id);
    }
    
    /* Call shell.unmap_memory if available. */
    bool ok = true;
    if (p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        lagfx_task_t *shell_task = task ? task->shell_task : NULL;
        if (dev->desc.shell.unmap_memory) {
            ok = dev->desc.shell.unmap_memory(
                dev->desc.shell.opaque, shell_task, virtual_off, length);
        } else {
            LAGFX_WARN("CmdUnmapMemory: no shell.unmap_memory callback for taskID=%u", task_id);
        }
    }
    
    LAGFX_LOG("CmdUnmapMemory: taskID=%u vm_off=0x%llx length=%llu status=%d",
              task_id, (unsigned long long)virtual_off, (unsigned long long)length, ok ? 0 : -1);
    
    return ok ? LAGFX_HANDLER_OK : LAGFX_HANDLER_ERR_STATE;
}

