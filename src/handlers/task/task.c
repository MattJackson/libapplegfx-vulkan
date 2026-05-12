/*
 * libapplegfx-vulkan — Task management handlers (opcode 0x00, 0x01)
 * src/handlers/task/task.c
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

lagfx_handler_status_t lagfx_task_define_task2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    
    /* CmdDefineTask2 payload (24 bytes):
     *   +0  u32 task_id
     *   +4  u64 root_va (page-table base VA, or shell-returned ptr low bits)
     *   +12 u64 length (task backing size in bytes; zero is valid for bring-up)
     *   +20 u32 reserved (always zero) */
    if (!hdr->payload || hdr->payload_size < 24) {
        LAGFX_WARN("CmdDefineTask2: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    uint32_t task_id   = lagfx_le32(hdr->payload + 0);
    uint64_t root_va   = lagfx_le64(hdr->payload + 4);
    uint64_t length    = lagfx_le64(hdr->payload + 12);
    uint32_t reserved  = lagfx_le32(hdr->payload + 20);
    
    (void)reserved;
    
    /* Look up or allocate task slot. Re-use slot if duplicate. */
    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        entry = lagfx_protocol_alloc_task_slot(p);
        if (!entry) {
            LAGFX_WARN("CmdDefineTask2: task table full (max=%u)", LAGFX_MAX_TASKS);
            return LAGFX_HANDLER_ERR_STATE;
        }
    }
    
    /* Call shell.create_task if callback available and length > 0. */
    lagfx_task_t *shell_task = NULL;
    void         *base_ptr   = NULL;
    if (length > 0 && p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev->desc.shell.create_task) {
            shell_task = dev->desc.shell.create_task(
                dev->desc.shell.opaque, length, &base_ptr);
            if (!shell_task) {
                LAGFX_WARN("CmdDefineTask2: shell.create_task returned NULL for taskID=%u", task_id);
            }
        } else {
            LAGFX_WARN("CmdDefineTask2: no shell.create_task callback for taskID=%u", task_id);
        }
    } else if (length == 0) {
        LAGFX_LOG("CmdDefineTask2: taskID=%u length=0 — recording slot without backing", task_id);
    }
    
    entry->id         = task_id;
    entry->shell_task = shell_task;
    /* Prefer guest root_va; fall back to shell-returned ptr low bits if root_va==0. */
    entry->base_va    = root_va ? root_va : (uint64_t)(uintptr_t)base_ptr;
    entry->length     = length;
    entry->live       = true;
    
    LAGFX_LOG("CmdDefineTask2: taskID=%u rootVA=0x%llx length=%llu shell_task=%p stamp=0x%08x",
              task_id, (unsigned long long)root_va, (unsigned long long)length,
              (void *)shell_task, hdr->stamp);
    
    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_task_delete_task(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    
    /* CmdDeleteTask payload (4 bytes): u32 task_id */
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDeleteTask: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    
    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    
    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        LAGFX_WARN("CmdDeleteTask: taskID=%u not found", task_id);
        return LAGFX_HANDLER_ERR_STATE;
    }
    
    /* Call shell.destroy_task if available. */
    if (entry->shell_task && p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev->desc.shell.destroy_task) {
            dev->desc.shell.destroy_task(dev->desc.shell.opaque, entry->shell_task);
        }
    }
    
    LAGFX_LOG("CmdDeleteTask: taskID=%u stamp=0x%08x", task_id, hdr->stamp);
    
    /* Clear task resources and mark slot free. */
    lagfx_resource_clear_task(&p->resources, task_id);
    memset(entry, 0, sizeof(*entry));
    entry->live = false;
    
    return LAGFX_HANDLER_OK;
}
