/*
 * libapplegfx-vulkan — IOSurface-family opcode handlers (Stage 50%)
 * src/protocol/ops_iosurface.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements real IOSurface lifecycle management for stage 50%
 * (window operations work). Handles opcodes 0x26-0x2a:
 *
 *   0x26 CmdDeleteIOSurfaceBacking2 — delete backing, decrement refcount
 *   0x27 CmdCreateIOSurfaceBacking2 — create VkImage+VkDeviceMemory
 *   0x28 CmdLookupIOSurface        — resolve existing surface by ID
 *   0x29 CmdImportIOSurfaceMachPort — cross-task import, increment refcount
 *   0x2a CmdUnmapIOSurface        — decrement refcount, destroy if zero
 *
 * See phase-4-iosurface-videotoolbox-plan.md §3.2 and
 * stage-30-50-opcode-implementation-plan.md §4.
 */

#include "opcodes.h"
#include "ops_iosurface.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"
#include "../device.h"
#include "../vulkan/iosurface.h"

#include <stdint.h>
#include <string.h>

/* === Little-endian primitives ================================== */

static inline uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t le64(const uint8_t *b) {
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
}

/* === Helper: count references to a host_handle ================ */

static int count_references(lagfx_resource_registry_t *reg,
                            void *host_handle) {
    if (!reg || !host_handle) return 0;
    int count = 0;
    for (uint32_t i = 0; i < reg->count; i++) {
        if (reg->entries[i].host_handle == host_handle) {
            count++;
        }
    }
    return count;
}

/* === Capture state (one per opcode) ========================= */

static lagfx_iosurface_capture_t g_cap_delete  = {0};
static lagfx_iosurface_capture_t g_cap_create  = {0};
static lagfx_iosurface_capture_t g_cap_lookup  = {0};
static lagfx_iosurface_capture_t g_cap_import  = {0};
static lagfx_iosurface_capture_t g_cap_unmap   = {0};

const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_delete(void) {
    return &g_cap_delete;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_create(void) {
    return &g_cap_create;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_lookup(void) {
    return &g_cap_lookup;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_import(void) {
    return &g_cap_import;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_unmap(void) {
    return &g_cap_unmap;
}

void lagfx_ops_iosurface_reset(void) {
    memset(&g_cap_delete, 0, sizeof(g_cap_delete));
    memset(&g_cap_create, 0, sizeof(g_cap_create));
    memset(&g_cap_lookup, 0, sizeof(g_cap_lookup));
    memset(&g_cap_import, 0, sizeof(g_cap_import));
    memset(&g_cap_unmap,  0, sizeof(g_cap_unmap));
}

/* === CmdDeleteIOSurfaceBacking2 (0x26) ======================= */

lagfx_handler_status_t lagfx_op_iosurface_delete_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr) return LAGFX_HANDLER_ERR_INTERNAL;

    g_cap_delete.surface_id = 0;
    g_cap_delete.task_id = 0;
    g_cap_delete.last_stamp = hdr->stamp;
    g_cap_delete.dispatch_count++;
    g_cap_delete.valid = true;
    g_cap_delete.payload_size = hdr->payload_size;
    if (hdr->payload && hdr->payload_size > 0) {
        uint32_t to_copy = hdr->payload_size < LAGFX_IOSURFACE_CAPTURE_MAX_BYTES ?
                          hdr->payload_size : LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
        g_cap_delete.captured_len = to_copy;
        memcpy(g_cap_delete.bytes, hdr->payload, to_copy);
    }

    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDeleteIOSurfaceBacking2: payload too small");
        return LAGFX_HANDLER_OK;
    }

    uint32_t surface_id = le32(hdr->payload);
    g_cap_delete.surface_id = surface_id;

    LAGFX_LOG("CmdDeleteIOSurfaceBacking2: surface_id=0x%x", surface_id);

    /* Assume task_id=0 for single-task; multi-task uses payload task. */
    uint32_t task_id = 0;
    if (hdr->payload_size >= 8) {
        task_id = le32(hdr->payload + 4);
        g_cap_delete.task_id = task_id;
    }

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources,
                                                      surface_id, task_id);
    if (!e || !e->host_handle) {
        LAGFX_WARN("CmdDeleteIOSurfaceBacking2: surface 0x%x not found",
                    surface_id);
        return LAGFX_HANDLER_OK;
    }

    void *host_handle = e->host_handle;
    lagfx_resource_unregister(&p->resources, surface_id, task_id);

    if (count_references(&p->resources, host_handle) == 0) {
        LAGFX_LOG("CmdDeleteIOSurfaceBacking2: destroying VkImage");
        if (p && p->dev && p->dev->vk && p->dev->vk->initialized) {
            lagfx_vk_iosurface_destroy(p->dev->vk,
                                       (lagfx_vk_iosurface_t *)host_handle);
        }
    }

    return LAGFX_HANDLER_OK;
}

/* === CmdCreateIOSurfaceBacking2 (0x27) ======================= */

lagfx_handler_status_t lagfx_op_iosurface_create_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr) return LAGFX_HANDLER_ERR_INTERNAL;

    g_cap_create.surface_id = 0;
    g_cap_create.width = 0;
    g_cap_create.height = 0;
    g_cap_create.pixel_format = 0;
    g_cap_create.bytes_per_row = 0;
    g_cap_create.size = 0;
    g_cap_create.last_stamp = hdr->stamp;
    g_cap_create.dispatch_count++;
    g_cap_create.valid = true;
    g_cap_create.payload_size = hdr->payload_size;
    if (hdr->payload && hdr->payload_size > 0) {
        uint32_t to_copy = hdr->payload_size < LAGFX_IOSURFACE_CAPTURE_MAX_BYTES ?
                          hdr->payload_size : LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
        g_cap_create.captured_len = to_copy;
        memcpy(g_cap_create.bytes, hdr->payload, to_copy);
    }

    if (!hdr->payload || hdr->payload_size < 28) {
        LAGFX_WARN("CmdCreateIOSurfaceBacking2: payload too small (%u < 28)",
                    (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }

    uint32_t surface_id = le32(hdr->payload + 0);
    uint32_t width = le32(hdr->payload + 4);
    uint32_t height = le32(hdr->payload + 8);
    uint32_t pixel_format = le32(hdr->payload + 12);
    uint32_t bytes_per_row = le32(hdr->payload + 16);
    uint64_t size = le64(hdr->payload + 20);

    g_cap_create.surface_id = surface_id;
    g_cap_create.width = width;
    g_cap_create.height = height;
    g_cap_create.pixel_format = pixel_format;
    g_cap_create.bytes_per_row = bytes_per_row;
    g_cap_create.size = size;

    LAGFX_LOG("CmdCreateIOSurfaceBacking2: surface_id=0x%x %ux%u fmt=0x%x "
               "bpr=%u size=%llu", surface_id, width, height, pixel_format,
               bytes_per_row, (unsigned long long)size);

    if (!p || !p->dev || !p->dev->vk || !p->dev->vk->initialized) {
        LAGFX_WARN("CmdCreateIOSurfaceBacking2: vk not initialized");
        return LAGFX_HANDLER_OK;
    }

    lagfx_vk_iosurface_t *ios = NULL;
    lagfx_status_t st = lagfx_vk_iosurface_create(p->dev->vk, width, height,
                                                    pixel_format, &ios);
    if (st != LAGFX_OK || !ios) {
        LAGFX_ERR("CmdCreateIOSurfaceBacking2: failed to create VkImage");
        return LAGFX_HANDLER_OK;
    }

    /* Assume task_id=0 for single-task. */
    uint32_t task_id = 0;
    lagfx_resource_register(&p->resources, surface_id,
                            LAGFX_RESOURCE_TYPE_TEXTURE,
                            task_id, 0u, size);
    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources,
                                                      surface_id, task_id);
    if (e) {
        e->host_handle = ios;
    }

    LAGFX_LOG("CmdCreateIOSurfaceBacking2: VkImage=%p view=%p",
               (void *)ios->image, (void *)ios->view);

    return LAGFX_HANDLER_OK;
}

/* === CmdLookupIOSurface (0x28) =============================== */

lagfx_handler_status_t lagfx_op_iosurface_lookup(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr) return LAGFX_HANDLER_ERR_INTERNAL;

    g_cap_lookup.surface_id = 0;
    g_cap_lookup.last_stamp = hdr->stamp;
    g_cap_lookup.dispatch_count++;
    g_cap_lookup.valid = true;
    g_cap_lookup.payload_size = hdr->payload_size;
    if (hdr->payload && hdr->payload_size > 0) {
        uint32_t to_copy = hdr->payload_size < LAGFX_IOSURFACE_CAPTURE_MAX_BYTES ?
                          hdr->payload_size : LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
        g_cap_lookup.captured_len = to_copy;
        memcpy(g_cap_lookup.bytes, hdr->payload, to_copy);
    }

    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdLookupIOSurface: payload too small");
        return LAGFX_HANDLER_OK;
    }

    uint32_t surface_id = le32(hdr->payload);
    g_cap_lookup.surface_id = surface_id;

    LAGFX_LOG("CmdLookupIOSurface: surface_id=0x%x", surface_id);

    uint32_t task_id = 0;
    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources,
                                                      surface_id, task_id);
    if (!e || !e->host_handle) {
        LAGFX_WARN("CmdLookupIOSurface: surface 0x%x not found", surface_id);
        return LAGFX_HANDLER_OK;
    }

    lagfx_vk_iosurface_t *ios = (lagfx_vk_iosurface_t *)e->host_handle;
    LAGFX_LOG("CmdLookupIOSurface: found VkImage=%p view=%p",
               (void *)ios->image, (void *)ios->view);

    return LAGFX_HANDLER_OK;
}

/* === CmdImportIOSurfaceMachPort (0x29) ======================= */

lagfx_handler_status_t lagfx_op_iosurface_import_mach_port(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr) return LAGFX_HANDLER_ERR_INTERNAL;

    g_cap_import.surface_id = 0;
    g_cap_import.task_id = 0;
    g_cap_import.remote_surface_id = 0;
    g_cap_import.remote_task_id = 0;
    g_cap_import.last_stamp = hdr->stamp;
    g_cap_import.dispatch_count++;
    g_cap_import.valid = true;
    g_cap_import.payload_size = hdr->payload_size;
    if (hdr->payload && hdr->payload_size > 0) {
        uint32_t to_copy = hdr->payload_size < LAGFX_IOSURFACE_CAPTURE_MAX_BYTES ?
                          hdr->payload_size : LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
        g_cap_import.captured_len = to_copy;
        memcpy(g_cap_import.bytes, hdr->payload, to_copy);
    }

    if (!hdr->payload || hdr->payload_size < 16) {
        LAGFX_WARN("CmdImportIOSurfaceMachPort: payload too small (%u < 16)",
                    (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }

    uint32_t local_task_id = le32(hdr->payload + 0);
    uint32_t remote_task_id = le32(hdr->payload + 4);
    uint32_t remote_surface_id = le32(hdr->payload + 8);
    uint32_t local_surface_id = le32(hdr->payload + 12);

    g_cap_import.task_id = local_task_id;
    g_cap_import.surface_id = local_surface_id;
    g_cap_import.remote_task_id = remote_task_id;
    g_cap_import.remote_surface_id = remote_surface_id;

    LAGFX_LOG("CmdImportIOSurfaceMachPort: local_task=%u remote_task=%u "
               "remote_surface=0x%x local_surface=0x%x",
               local_task_id, remote_task_id, remote_surface_id,
               local_surface_id);

    lagfx_resource_entry_t *remote = lagfx_resource_lookup(
        &p->resources, remote_surface_id, remote_task_id);
    if (!remote || !remote->host_handle) {
        LAGFX_WARN("CmdImportIOSurfaceMachPort: remote surface 0x%x not found",
                    remote_surface_id);
        return LAGFX_HANDLER_OK;
    }

    lagfx_resource_register(&p->resources, local_surface_id,
                            LAGFX_RESOURCE_TYPE_TEXTURE,
                            local_task_id, 0u, remote->size);
    lagfx_resource_entry_t *local = lagfx_resource_lookup(
        &p->resources, local_surface_id, local_task_id);
    if (local) {
        local->host_handle = remote->host_handle;
    }

    LAGFX_LOG("CmdImportIOSurfaceMachPort: imported -> VkImage=%p",
               (void *)((lagfx_vk_iosurface_t *)remote->host_handle)->image);

    return LAGFX_HANDLER_OK;
}

/* === CmdUnmapIOSurface (0x2a) ================================ */

lagfx_handler_status_t lagfx_op_iosurface_unmap(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!hdr) return LAGFX_HANDLER_ERR_INTERNAL;

    g_cap_unmap.surface_id = 0;
    g_cap_unmap.task_id = 0;
    g_cap_unmap.last_stamp = hdr->stamp;
    g_cap_unmap.dispatch_count++;
    g_cap_unmap.valid = true;
    g_cap_unmap.payload_size = hdr->payload_size;
    if (hdr->payload && hdr->payload_size > 0) {
        uint32_t to_copy = hdr->payload_size < LAGFX_IOSURFACE_CAPTURE_MAX_BYTES ?
                          hdr->payload_size : LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
        g_cap_unmap.captured_len = to_copy;
        memcpy(g_cap_unmap.bytes, hdr->payload, to_copy);
    }

    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdUnmapIOSurface: payload too small (%u < 8)",
                    (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }

    uint32_t task_id = le32(hdr->payload + 0);
    uint32_t surface_id = le32(hdr->payload + 4);

    g_cap_unmap.task_id = task_id;
    g_cap_unmap.surface_id = surface_id;

    LAGFX_LOG("CmdUnmapIOSurface: task=%u surface_id=0x%x", task_id, surface_id);

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources,
                                                      surface_id, task_id);
    if (!e || !e->host_handle) {
        LAGFX_WARN("CmdUnmapIOSurface: surface 0x%x not found", surface_id);
        return LAGFX_HANDLER_OK;
    }

    void *host_handle = e->host_handle;
    lagfx_resource_unregister(&p->resources, surface_id, task_id);

    if (count_references(&p->resources, host_handle) == 0) {
        LAGFX_LOG("CmdUnmapIOSurface: destroying VkImage");
        if (p && p->dev && p->dev->vk && p->dev->vk->initialized) {
            lagfx_vk_iosurface_destroy(p->dev->vk,
                                       (lagfx_vk_iosurface_t *)host_handle);
        }
    }

    return LAGFX_HANDLER_OK;
}
