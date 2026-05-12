/*
 * libapplegfx-vulkan — CmdDefineTask2 handler (task management)
 * src/handlers/task/task.c
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

lagfx_handler_status_t lagfx_task_define_task2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    
    if (!hdr->payload || hdr->payload_size < 24) {
        LAGFX_WARN("CmdDefineTask2: payload missing or too small (size=%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint64_t root_va = lagfx_le64(hdr->payload + 4);
    uint64_t length = lagfx_le64(hdr->payload + 12);
    (void)lagfx_le32(hdr->payload + 20);

    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (entry) {
        LAGFX_WARN("CmdDefineTask2: duplicate taskID=%u (re-using slot)", task_id);
    } else {
        entry = lagfx_protocol_alloc_task_slot(p);
        if (!entry) {
            LAGFX_WARN("CmdDefineTask2: task table full (max=%u)", LAGFX_MAX_TASKS);
            return LAGFX_HANDLER_ERR_STATE;
        }
    }

    lagfx_task_t *shell_task = NULL;
    void *base_ptr = NULL;
    if (length > 0 && p->dev && p->dev->desc.shell.create_task) {
        shell_task = p->dev->desc.shell.create_task(p->dev->desc.shell.opaque, length, &base_ptr);
        if (!shell_task) {
            LAGFX_WARN("CmdDefineTask2: shell.create_task returned NULL for taskID=%u", task_id);
        }
    } else if (length == 0) {
        LAGFX_LOG("CmdDefineTask2: taskID=%u length=0 — recording slot without shell", task_id);
    }

    entry->id = task_id;
    entry->shell_task = shell_task;
    entry->base_va = root_va ? root_va : (uint64_t)(uintptr_t)base_ptr;
    entry->length = length;
    entry->live = true;

    LAGFX_LOG("CmdDefineTask2: taskID=%u rootVA=0x%llx length=%llu shell_task=%p stamp=0x%08x",
              task_id, (unsigned long long)root_va, (unsigned long long)length, (void *)shell_task, hdr->stamp);

    return LAGFX_HANDLER_OK;
}
