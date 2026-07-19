/*
 * libapplegfx-vulkan — per-channel ring helpers
 * src/dispatchers/ring_common.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implementation notes live in ring_common.h. This file is the
 * single source of truth for the FIFORingDescriptor read,
 * PFN-array resolve, and read_ptr write-back used by every
 * per-channel dispatcher. Both channel_compute_dispatcher.c and
 * channel_display_dispatcher.c previously carried near-identical
 * 75-line copies of these three functions.
 */

#include "ring_common.h"

#include "../common/le.h"
#include "../common/log.h"
#include "../device.h"
#include "../protocol/state.h"

bool lagfx_ring_fifo_descriptor_read(lagfx_protocol_t *p,
                                      uint32_t chan_id,
                                      uint64_t *out_desc_gpa,
                                      uint32_t *out_write_ptr,
                                      uint32_t *out_read_ptr,
                                      uint64_t *out_page0_gpa) {
    if (!p || !out_desc_gpa || !out_write_ptr || !out_read_ptr
        || !out_page0_gpa) {
        return false;
    }
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("ring_fifo_descriptor_read: no shell.read_memory callback");
        return false;
    }
    if (p->ring_shared_page_pfn == 0u) {
        LAGFX_TRACE("ring_fifo_descriptor_read: ring_shared_page_pfn=0 — kext "
                    "hasn't published shared page yet");
        return false;
    }
    if (chan_id == 0u) {
        /* Root channel uses primary-ring geometry, not a per-channel
         * descriptor; callers must dispatch directly to
         * channel_0_dispatcher_drain. */
        return false;
    }

    uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
    uint64_t desc_gpa   = shared_gpa
                        + LAGFX_RING_FIFO_DESC_BASE_OFFSET
                        + (uint64_t)(chan_id - 1u) * LAGFX_RING_FIFO_DESC_BYTES;

    uint8_t desc[LAGFX_RING_FIFO_DESC_BYTES];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      desc_gpa,
                                      LAGFX_RING_FIFO_DESC_BYTES,
                                      desc)) {
        LAGFX_WARN("ring_fifo_descriptor_read: read failed at 0x%llx (chan=%u)",
                   (unsigned long long)desc_gpa, chan_id);
        return false;
    }

    uint32_t write_ptr = lagfx_le32(desc + 0x00);
    uint32_t read_ptr  = lagfx_le32(desc + 0x04);
    uint32_t desc_chan = lagfx_le32(desc + 0x0c);
    uint32_t ring_pfn  = lagfx_le32(desc + 0x10);

    if (ring_pfn == 0u) {
        LAGFX_TRACE("ring_fifo_descriptor_read: ch=%u ring_pfn=0 — descriptor "
                    "not yet initialised", chan_id);
        return false;
    }
    if (desc_chan != chan_id) {
        LAGFX_WARN("ring_fifo_descriptor_read: ch=%u descriptor chan_id field "
                   "mismatch (got %u) — proceeding with caller's id",
                   chan_id, desc_chan);
    }

    *out_desc_gpa  = desc_gpa;
    *out_write_ptr = write_ptr;
    *out_read_ptr  = read_ptr;
    *out_page0_gpa = (uint64_t)ring_pfn << 12;
    return true;
}

bool lagfx_ring_resolve_data_gpa(lagfx_protocol_t *p,
                                  uint64_t page0_gpa,
                                  uint32_t ring_size,
                                  uint32_t offset,
                                  uint64_t *out_data_gpa) {
    if (!p || !out_data_gpa) return false;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) return false;

    uint32_t off_mod = (ring_size != 0u) ? (offset % ring_size) : offset;
    uint32_t page_idx = off_mod >> 12;

    uint8_t pfn_buf[4];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      page0_gpa + page_idx * 4u,
                                      4, pfn_buf)) {
        LAGFX_WARN("ring_resolve_data_gpa: PFN-array read failed at 0x%llx",
                   (unsigned long long)(page0_gpa + page_idx * 4u));
        return false;
    }
    uint32_t data_pfn = lagfx_le32(pfn_buf);
    if (data_pfn == 0u) {
        LAGFX_WARN("ring_resolve_data_gpa: PFN-array entry[%u]=0 — ring page "
                   "not mapped at off=0x%x", page_idx, offset);
        return false;
    }
    *out_data_gpa = ((uint64_t)data_pfn << 12) + (off_mod & 0xfffu);
    return true;
}

void lagfx_ring_publish_read_ptr(lagfx_protocol_t *p,
                                  uint64_t desc_gpa,
                                  uint32_t read_ptr) {
    if (!p) return;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.write_memory) return;

    /* Write to +0x04. NOT +0x00 — that's the kext's write_ptr field
     * (FIFORingDescriptor.md "Bug pattern: clobbering write_ptr"). */
    if (!dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                       desc_gpa + 0x04u,
                                       sizeof(read_ptr),
                                       &read_ptr)) {
        LAGFX_WARN("ring_publish_read_ptr: write failed at 0x%llx",
                   (unsigned long long)(desc_gpa + 0x04u));
    }
}

bool lagfx_ring_read_bytes(lagfx_protocol_t *p,
                            uint64_t page0_gpa,
                            uint32_t ring_size,
                            uint32_t offset,
                            uint32_t len,
                            uint8_t *out) {
    if (!p || !out || len == 0u) return false;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) return false;

    uint32_t done = 0;
    while (done < len) {
        uint64_t chunk_gpa = 0;
        if (!lagfx_ring_resolve_data_gpa(p, page0_gpa, ring_size,
                                          offset + done, &chunk_gpa)) {
            return false;
        }
        uint32_t chunk_off_in_page = (uint32_t)(chunk_gpa & 0xfffu);
        uint32_t chunk_len = 0x1000u - chunk_off_in_page;
        if (chunk_len > len - done) chunk_len = len - done;
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          chunk_gpa, chunk_len, out + done)) {
            LAGFX_WARN("ring_read_bytes: chunk DMA failed at gpa=0x%llx",
                       (unsigned long long)chunk_gpa);
            return false;
        }
        done += chunk_len;
    }
    return true;
}
