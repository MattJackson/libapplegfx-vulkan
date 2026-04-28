/*
 * libapplegfx-vulkan — resource reference registry implementation
 * src/protocol/resource_registry.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "resource_registry.h"
#include "../common/log.h"

#include <string.h>

void lagfx_resource_register(lagfx_resource_registry_t *reg,
                             uint32_t ref,
                             lagfx_resource_type_t type,
                             uint32_t task_id,
                             uint64_t gpu_addr,
                             uint64_t size) {
    if (!reg) {
        return;
    }

    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref
            && reg->entries[i].task_id == task_id) {
            reg->entries[i].type     = type;
            reg->entries[i].gpu_addr = gpu_addr;
            reg->entries[i].size     = size;
            LAGFX_TRACE("resource_register: updated ref=0x%x task=%u "
                         "type=%u gpu_addr=0x%llx size=%llu",
                         ref, task_id, (unsigned)type,
                         (unsigned long long)gpu_addr,
                         (unsigned long long)size);
            return;
        }
    }

    if (reg->count >= LAGFX_MAX_RESOURCES) {
        LAGFX_WARN("resource_register: table full (max=%u) ref=0x%x "
                    "task=%u",
                    LAGFX_MAX_RESOURCES, ref, task_id);
        return;
    }

    lagfx_resource_entry_t *e = &reg->entries[reg->count++];
    e->ref         = ref;
    e->type        = type;
    e->task_id     = task_id;
    e->host_handle = NULL;
    e->gpu_addr    = gpu_addr;
    e->size        = size;

    LAGFX_TRACE("resource_register: inserted ref=0x%x task=%u type=%u "
                 "gpu_addr=0x%llx size=%llu (total=%u)",
                 ref, task_id, (unsigned)type,
                 (unsigned long long)gpu_addr,
                 (unsigned long long)size,
                 reg->count);
}

lagfx_resource_entry_t *lagfx_resource_lookup(lagfx_resource_registry_t *reg,
                                               uint32_t ref,
                                               uint32_t task_id) {
    if (!reg) {
        return NULL;
    }

    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref
            && reg->entries[i].task_id == task_id) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

void lagfx_resource_unregister(lagfx_resource_registry_t *reg,
                               uint32_t ref,
                               uint32_t task_id) {
    if (!reg) {
        return;
    }

    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref
            && reg->entries[i].task_id == task_id) {
            if (i + 1 < reg->count) {
                reg->entries[i] = reg->entries[reg->count - 1];
            }
            memset(&reg->entries[reg->count - 1], 0,
                   sizeof(lagfx_resource_entry_t));
            reg->count--;
            LAGFX_TRACE("resource_unregister: ref=0x%x task=%u "
                         "(remaining=%u)",
                         ref, task_id, reg->count);
            return;
        }
    }

    LAGFX_TRACE("resource_unregister: ref=0x%x task=%u not found",
                 ref, task_id);
}

void lagfx_resource_clear_task(lagfx_resource_registry_t *reg,
                               uint32_t task_id) {
    if (!reg) {
        return;
    }

    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < reg->count; ++read_idx) {
        if (reg->entries[read_idx].task_id != task_id) {
            if (write_idx != read_idx) {
                reg->entries[write_idx] = reg->entries[read_idx];
            }
            write_idx++;
        }
    }

    uint32_t removed = reg->count - write_idx;
    if (removed > 0) {
        memset(&reg->entries[write_idx], 0,
               removed * sizeof(lagfx_resource_entry_t));
    }
    reg->count = write_idx;

    LAGFX_TRACE("resource_clear_task: task=%u removed=%u remaining=%u",
                 task_id, removed, reg->count);
}
