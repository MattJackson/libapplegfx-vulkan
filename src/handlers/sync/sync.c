/*
 * libapplegfx-vulkan — Sync and Display handlers
 * src/handlers/sync/sync.c + src/handlers/display/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers.h"
#include "common/le.h"
#include "common/log.h"

lagfx_handler_status_t lagfx_sync_synchronize_resources(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdSynchronizeResources payload (variable):
     *   +0  u32 task_id
     *   +4  u32 range_count (8-byte records: {u64 gpa, u64 length}) */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdSynchronizeResources: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id   = lagfx_le32(hdr->payload + 0);
    uint32_t range_count = lagfx_le32(hdr->payload + 4);

    /* Validate: each range is 8 bytes. */
    if (range_count > ((uint32_t)hdr->payload_size - 8u) / 8u) {
        LAGFX_WARN("CmdSynchronizeResources: range_count=%u exceeds payload", range_count);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    LAGFX_LOG("CmdSynchronizeResources: taskID=%u range_count=%u stamp=0x%08x",
              task_id, range_count, hdr->stamp);

    /* TODO: Flush host cache for each {gpa, length} range in guest memory.
     *       This ensures guest sees all writes before stamp completes. */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_swap_mapping(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplaySwapMapping payload (40 bytes):
     *   +0  u32 display_id
     *   +4  u32 scanout_length
     *   +8  u64 scanout_gpa (guest VA of scanout buffer)
     *   +16 u32 framebuffer_width
     *   +20 u32 framebuffer_height
     *   +24 u32 format (pixel format enum)
     *   +28 u32 stride (bytes per row)
     *   +32 u64 scanout_offset (offset within scanout buffer)
     *   +40 ... (optional extra fields, 40-byte minimum) */
    if (!hdr->payload || hdr->payload_size < 40) {
        LAGFX_WARN("CmdDisplaySwapMapping: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id   = lagfx_le32(hdr->payload + 0);
    uint32_t scanout_len  = lagfx_le32(hdr->payload + 4);
    uint64_t scanout_gpa  = lagfx_le64(hdr->payload + 8);
    uint32_t fb_width     = lagfx_le32(hdr->payload + 16);
    uint32_t fb_height    = lagfx_le32(hdr->payload + 20);
    uint32_t format       = lagfx_le32(hdr->payload + 24);
    uint32_t stride       = lagfx_le32(hdr->payload + 28);
    uint64_t scanout_off  = lagfx_le64(hdr->payload + 32);

    LAGFX_LOG("CmdDisplaySwapMapping: display_id=%u scanout_gpa=0x%llx len=%u "
              "fb=%ux%d format=0x%x stride=%u offset=0x%llx",
              display_id, (unsigned long long)scanout_gpa, scanout_len,
              fb_width, fb_height, format, stride, (unsigned long long)scanout_off);

    /* Store scanout buffer info for use by vchan_display_submit. The kext
      * may send arg2=0 in vchan_display_submit if it expects us to use the
      * last-seen swap mapping (common pattern when scanout buffer is stable). */
    if (display_id < LAGFX_MAX_DISPLAYS) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        if (dev && dev->displays[display_id]) {
            lagfx_display_t *disp = dev->displays[display_id];
            disp->scanout_gpa  = scanout_gpa;
            disp->scanout_length = scanout_len;
            disp->scanout_width  = fb_width;
            disp->scanout_height = fb_height;
            disp->scanout_valid  = true;
            LAGFX_LOG("CmdDisplaySwapMapping: stored for display[%u] gpa=0x%llx len=%u %ux%u",
                      display_id, (unsigned long long)scanout_gpa, scanout_len,
                      fb_width, fb_height);
        } else {
            LAGFX_WARN("CmdDisplaySwapMapping: display[%u] not found — info stored but no handler",
                      display_id);
        }
    }

    return LAGFX_HANDLER_OK;
}
