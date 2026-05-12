/*
 * libapplegfx-vulkan — Memory management handlers
 * src/handlers/memory/memory.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../device.h"
#include "../common/log.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)lagfx_le32(b) | ((uint64_t)lagfx_le32(b + 4) << 32);
}

lagfx_handler_status_t lagfx_memory_map_memory2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    
    /* TODO: implement memory mapping */
    (void)hdr;
    LAGFX_TRACE("CmdMapMemory2: stub");
    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_memory_map_memory_immediate(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    
    /* Minimum trailer: task_id (4) + vaBase (8) + vaLength (8) = 20 bytes */
    if (!hdr->payload || hdr->payload_size < 20) {
        LAGFX_WARN("CmdMapMemoryImmediate: payload missing or too small "
                   "(size=%u, need >= 20)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint64_t va_base = (uint64_t)lagfx_le32(hdr->payload + 4) |
                       ((uint64_t)lagfx_le32(hdr->payload + 8) << 32);
    uint64_t va_length = (uint64_t)lagfx_le32(hdr->payload + 12) |
                         ((uint64_t)lagfx_le32(hdr->payload + 16) << 32);

    LAGFX_LOG("CmdMapMemoryImmediate: taskID=%u vaBase=0x%llx length=%llu stamp=0x%08x",
              task_id, (unsigned long long)va_base, (unsigned long long)va_length, hdr->stamp);

    /* TODO: Register mapping in host-side address space for translation */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_memory_unmap_memory(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    
    /* TODO: implement memory unmapping */
    (void)hdr;
    LAGFX_TRACE("CmdUnmapMemory: stub");
    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_memory_define_child_fifo(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    
    /* TODO: implement child FIFO creation */
    (void)hdr;
    LAGFX_TRACE("CmdDefineChildFIFO: stub");
    return LAGFX_HANDLER_OK;
}
