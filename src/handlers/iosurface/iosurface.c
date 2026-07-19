/*
 * libapplegfx-vulkan — IOSurface outer-FIFO opcode handlers
 * src/handlers/iosurface/iosurface.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * RE: paravirt-re/library/state-machines/PGFIFO-sub-channel-opcode-table.md
 *     re-followup-spec-gaps.md §14.5 (conjectured wire layouts),
 *     pre-refactor src/protocol/ops_iosurface.c at b652199~1
 *     (stranded in dead-code-to-revive/protocol/ until 2026-05-13).
 *
 * Implementation status:
 *   - Wire parsing is preserved from the pre-refactor handlers.
 *   - Resource-registry register/lookup/unregister calls are kept
 *     so cross-task IOSurface ID → host metadata mapping survives.
 *   - The Vulkan VkImage allocation path (lagfx_vk_iosurface_create
 *     and the cap_counters mirror) is NOT reinstated: the post-
 *     dispatcher state.h doesn't carry the per-opcode capture struct
 *     and the resource_entry host_handle is repurposed for the
 *     SetFragmentTextures auto-create path in the upcoming Stage 30
 *     render-encoder reintroduction. Each handler carries a Stage 30
 *     TODO marker for the VkImage allocator.
 *   - All ops fail-open (return OK on missing surface / short payload)
 *     to match the pre-refactor semantics and avoid stalling the
 *     guest in waitForStamp.
 */

#include "iosurface.h"

#include "common/le.h"
#include "common/log.h"
#include "../../device.h"
#include "../../vulkan/iosurface.h"

#include <stdint.h>
#include <string.h>

/* === 0x26 — CmdDeleteIOSurfaceBacking2 =========================== *
 *
 * Payload (pre-refactor):
 *   +0  u32 surface_id
 *   +4  u32 task_id      (optional; defaults to 0 if absent)
 */
lagfx_handler_status_t lagfx_iosurface_delete_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDeleteIOSurfaceBacking2: payload too small (%u)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }
    uint32_t surface_id = lagfx_le32(hdr->payload + 0);
    uint32_t task_id    = (hdr->payload_size >= 8) ? lagfx_le32(hdr->payload + 4) : 0u;
    LAGFX_LOG("CmdDeleteIOSurfaceBacking2: surface_id=0x%x task=%u stamp=0x%08x",
              surface_id, task_id, hdr->stamp);

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources, surface_id, task_id);
    if (!e) {
        LAGFX_TRACE("CmdDeleteIOSurfaceBacking2: surface 0x%x not in registry", surface_id);
        return LAGFX_HANDLER_OK;
    }
#ifdef LAGFX_HAVE_VULKAN
    /* Stage 30 — release backing VkImage. Refcount-aware via
     * lagfx_vk_iosurface_release so Import-aliased backings don't
     * double-free. */
    if (e->host_handle && p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev->vk) {
            lagfx_vk_iosurface_release(dev->vk,
                (lagfx_vk_iosurface_t *)e->host_handle);
        }
        e->host_handle = NULL;
        e->image = VK_NULL_HANDLE;
        e->view  = VK_NULL_HANDLE;
    }
#endif
    lagfx_resource_unregister(&p->resources, surface_id, task_id);
    return LAGFX_HANDLER_OK;
}

/* === 0x27 — CmdCreateIOSurfaceBacking2 =========================== *
 *
 * Payload (pre-refactor):
 *   +0   u32 surface_id
 *   +4   u32 width
 *   +8   u32 height
 *   +12  u32 pixel_format
 *   +16  u32 bytes_per_row
 *   +20  u64 size
 *
 * Short payloads (< 28 bytes) are MACOS-OBSERVED capability probes:
 * macOS sometimes sends only a surface_id to ask "do you know
 * about this id?" before issuing a real create. Reject those at
 * the size gate so the tracker doesn't end up holding entries with
 * RE-conjectured dimensions; the kext will follow up with a full
 * payload once the userspace IOSurface_create call lands.
 *
 * RE: pre-refactor lagfx_op_iosurface_create_backing2 at
 * b652199~1:src/protocol/ops_iosurface.c defaulted these probes to
 * 1920x1080 BGRA. That worked by accident — macOS 15.7.5 happens
 * to only register surfaces that exact size — but masked any drift
 * in the wire format. Fail loud instead.
 */
lagfx_handler_status_t lagfx_iosurface_create_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    if (!hdr->payload || hdr->payload_size < 28u) {
        LAGFX_WARN("CmdCreateIOSurfaceBacking2: payload too small (%u, need 28) "
                   "— treating as capability probe; no registry update",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t surface_id   = lagfx_le32(hdr->payload + 0);
    uint32_t width        = lagfx_le32(hdr->payload + 4);
    uint32_t height       = lagfx_le32(hdr->payload + 8);
    uint32_t pixel_format = lagfx_le32(hdr->payload + 12);
    uint32_t bytes_per_row = lagfx_le32(hdr->payload + 16);
    uint64_t size          = lagfx_le64(hdr->payload + 20);

    LAGFX_LOG("CmdCreateIOSurfaceBacking2: surface_id=0x%x %ux%u fmt=0x%x bpr=%u size=%llu",
              surface_id, width, height, pixel_format, bytes_per_row,
              (unsigned long long)size);

    /* Register in the resource registry — task_id 0 for single-task,
     * matching the pre-refactor convention. */
    uint32_t task_id = 0u;
    lagfx_resource_register(&p->resources, surface_id,
                            LAGFX_RESOURCE_TYPE_TEXTURE,
                            task_id, 0u, size);

#ifdef LAGFX_HAVE_VULKAN
    /* Stage 30 — allocate backing VkImage. Stash on the registry
     * entry's host_handle so render-target / blit / sample paths
     * can resolve surface_id → VkImage in O(1). Refcount starts at
     * 1; Import (0x29) bumps; Delete/Unmap call _release.
     *
     * Failure modes:
     *  - vk state not yet initialized (very-early-boot ordering):
     *    log + leave host_handle NULL. The next 0x28 Lookup will
     *    auto-retry via a follow-up patch. For now the registry
     *    entry is still in place so dispatchers don't fail-open
     *    on missing surface.
     *  - width or height 0: fail-loud — that's a wire-format bug
     *    we want to surface, not silently allocate a degenerate
     *    backing.
     *  - vkCreateImage / vkAllocateMemory failure: the create
     *    function already LAGFX_ERRs; we leave host_handle NULL. */
    if (p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev->vk) {
            lagfx_vk_iosurface_t *ios = NULL;
            lagfx_status_t st = lagfx_vk_iosurface_create(dev->vk,
                width, height, pixel_format, &ios);
            if (st == LAGFX_OK && ios) {
                lagfx_resource_entry_t *e =
                    lagfx_resource_lookup(&p->resources, surface_id, task_id);
                if (e) {
                    /* If an earlier registration left a backing in
                     * place, release it first so we don't leak. */
                    if (e->host_handle) {
                        lagfx_vk_iosurface_release(dev->vk,
                            (lagfx_vk_iosurface_t *)e->host_handle);
                    }
                    e->host_handle = ios;
                    e->image = ios->image;
                    e->view  = ios->view;
                    LAGFX_LOG("CmdCreateIOSurfaceBacking2: VkImage allocated "
                              "for surface_id=0x%x → handle=%p",
                              surface_id, (void *)ios);
                } else {
                    /* Registry full or other lookup failure — undo the alloc */
                    lagfx_vk_iosurface_destroy(dev->vk, ios);
                }
            }
        } else {
            LAGFX_LOG("CmdCreateIOSurfaceBacking2: vk state not ready for "
                      "surface_id=0x%x; backing deferred to first Lookup",
                      surface_id);
        }
    }
#endif
    return LAGFX_HANDLER_OK;
}

/* === 0x28 — CmdLookupIOSurface =================================== *
 *
 * Payload (pre-refactor):
 *   +0   u32 surface_id
 *   +4   u32 flags
 *   +8   u64 size
 *
 * If the surface isn't in the registry and the payload carries a
 * non-zero size, pre-refactor auto-created a backing on first lookup.
 * That path depends on the Vulkan allocator; for now we register a
 * tracker entry so the second lookup succeeds.
 */
lagfx_handler_status_t lagfx_iosurface_lookup(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdLookupIOSurface: payload too small (%u)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }
    uint32_t surface_id = lagfx_le32(hdr->payload + 0);
    uint32_t flags      = (hdr->payload_size >= 8)  ? lagfx_le32(hdr->payload + 4) : 0u;
    uint64_t size       = (hdr->payload_size >= 16) ? lagfx_le64(hdr->payload + 8) : 0u;

    LAGFX_LOG("CmdLookupIOSurface: surface_id=0x%x flags=0x%x size=%llu stamp=0x%08x",
              surface_id, flags, (unsigned long long)size, hdr->stamp);

    uint32_t task_id = 0u;
    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources, surface_id, task_id);
    if (!e && size > 0u && size <= ((uint64_t)4096u * 4096u * 4u)) {
        LAGFX_LOG("CmdLookupIOSurface: auto-registering surface 0x%x size=%llu "
                  "(guest didn't send 0x27 Create first)",
                  surface_id, (unsigned long long)size);
        lagfx_resource_register(&p->resources, surface_id,
                                LAGFX_RESOURCE_TYPE_TEXTURE,
                                task_id, 0u, size);
        /* TODO: Stage 30 — allocate a backing VkImage and bind to
         * host_handle so subsequent render-target uses can resolve. */
    } else if (!e) {
        LAGFX_TRACE("CmdLookupIOSurface: surface 0x%x not found, size=%llu — skip auto-create",
                    surface_id, (unsigned long long)size);
    }
    return LAGFX_HANDLER_OK;
}

/* === 0x29 — CmdImportIOSurfaceMachPort =========================== *
 *
 * Payload (pre-refactor):
 *   +0   u32 local_task_id
 *   +4   u32 remote_task_id
 *   +8   u32 remote_surface_id
 *   +12  u32 local_surface_id
 *
 * Cross-task import: copy the remote (task, surface_id) entry's
 * size + host_handle into a new local entry. */
lagfx_handler_status_t lagfx_iosurface_import_mach_port(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    if (!hdr->payload || hdr->payload_size < 16) {
        LAGFX_WARN("CmdImportIOSurfaceMachPort: payload too small (%u < 16)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }
    uint32_t local_task_id     = lagfx_le32(hdr->payload + 0);
    uint32_t remote_task_id    = lagfx_le32(hdr->payload + 4);
    uint32_t remote_surface_id = lagfx_le32(hdr->payload + 8);
    uint32_t local_surface_id  = lagfx_le32(hdr->payload + 12);

    LAGFX_LOG("CmdImportIOSurfaceMachPort: local_task=%u remote_task=%u "
              "remote_surface=0x%x local_surface=0x%x",
              local_task_id, remote_task_id, remote_surface_id, local_surface_id);

    lagfx_resource_entry_t *remote =
        lagfx_resource_lookup(&p->resources, remote_surface_id, remote_task_id);
    if (!remote) {
        LAGFX_WARN("CmdImportIOSurfaceMachPort: remote surface 0x%x (task %u) not found",
                   remote_surface_id, remote_task_id);
        return LAGFX_HANDLER_OK;
    }

    lagfx_resource_register(&p->resources, local_surface_id,
                            LAGFX_RESOURCE_TYPE_TEXTURE,
                            local_task_id, 0u, remote->size);
    lagfx_resource_entry_t *local =
        lagfx_resource_lookup(&p->resources, local_surface_id, local_task_id);
    if (local) {
        /* Aliased host_handle — both entries point to the same backing.
         * Retain the backing so Delete/Unmap on either entry only frees
         * when the last reference is gone. */
        local->host_handle = remote->host_handle;
#ifdef LAGFX_HAVE_VULKAN
        local->image = remote->image;
        local->view  = remote->view;
        if (remote->host_handle) {
            lagfx_vk_iosurface_retain(
                (lagfx_vk_iosurface_t *)remote->host_handle);
        }
#endif
    }
    return LAGFX_HANDLER_OK;
}

/* === 0x2a — CmdUnmapIOSurface ==================================== *
 *
 * Payload (pre-refactor):
 *   +0   u32 task_id
 *   +4   u32 surface_id
 */
lagfx_handler_status_t lagfx_iosurface_unmap(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) return LAGFX_HANDLER_ERR_INTERNAL;
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdUnmapIOSurface: payload too small (%u < 8)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }
    uint32_t task_id    = lagfx_le32(hdr->payload + 0);
    uint32_t surface_id = lagfx_le32(hdr->payload + 4);
    LAGFX_LOG("CmdUnmapIOSurface: task=%u surface_id=0x%x stamp=0x%08x",
              task_id, surface_id, hdr->stamp);

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources, surface_id, task_id);
    if (!e) {
        LAGFX_TRACE("CmdUnmapIOSurface: surface 0x%x not in registry", surface_id);
        return LAGFX_HANDLER_OK;
    }
#ifdef LAGFX_HAVE_VULKAN
    /* Stage 30 — release backing VkImage, refcount-aware (mirrors
     * CmdDeleteIOSurfaceBacking2). */
    if (e->host_handle && p->dev) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev->vk) {
            lagfx_vk_iosurface_release(dev->vk,
                (lagfx_vk_iosurface_t *)e->host_handle);
        }
        e->host_handle = NULL;
        e->image = VK_NULL_HANDLE;
        e->view  = VK_NULL_HANDLE;
    }
#endif
    lagfx_resource_unregister(&p->resources, surface_id, task_id);
    return LAGFX_HANDLER_OK;
}
