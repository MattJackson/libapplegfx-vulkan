/*
 * libapplegfx-vulkan — per-channel ring helpers
 * src/dispatchers/ring_common.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The compute (ch 1..4) and display (ch 5+) dispatchers each have
 * their own command ring, but the ring-descriptor lookup, the PFN-
 * array indirection that resolves a byte offset to a data-page GPA,
 * and the write-back of the drained read_ptr into the descriptor are
 * structurally identical. Pre-refactor each dispatcher carried its
 * own ~75 LOC copy.
 *
 * RE refs:
 *   - state-machines/FIFORingDescriptor.md — 20-byte descriptor at
 *     shared_control + 0x400 + 20 * (chan_id - 1).
 *   - state-machines/per-channel-ring-pfn-array.md — ring_pfn<<12
 *     is a u32 PFN-array (page 0); data lives at
 *     page0[(off%ring_size) >> 12] << 12 + (off & 0xfff). Descriptor
 *     write_ptr/read_ptr are ABSOLUTE monotonic byte counters; take
 *     modulo BEFORE computing the page index.
 *
 * The functions below are the single source of truth for that
 * pattern. Call them from any drain loop that follows the standard
 * FIFORingDescriptor layout.
 */

#ifndef LAGFX_DISPATCHERS_RING_COMMON_H
#define LAGFX_DISPATCHERS_RING_COMMON_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

/* Read the 20-byte FIFORingDescriptor for `chan_id` from the shared
 * control page registered at BAR0+0x101c.
 *
 * On success returns true and populates:
 *   *out_desc_gpa  — descriptor's GPA (so the caller can write back
 *                    the advanced read_ptr via the publish helper).
 *   *out_write_ptr — guest write pointer (absolute monotonic).
 *   *out_read_ptr  — host read pointer (absolute monotonic).
 *   *out_page0_gpa — `ring_pfn << 12`. This is the PFN-ARRAY page;
 *                    actual command bytes live at
 *                    page0[(off % ring_size) >> 12] << 12 + offset
 *                    mod 0x1000. Resolve via ring_resolve_data_gpa.
 *
 * Returns false (and logs at TRACE/WARN as appropriate) when:
 *   - shell.read_memory is missing,
 *   - ring_shared_page_pfn is zero (kext hasn't published yet),
 *   - chan_id == 0 (root channel uses primary-ring geometry, not
 *     a per-channel descriptor),
 *   - the descriptor DMA fails, or
 *   - the descriptor's ring_pfn is zero (uninitialised slot).
 *
 * A descriptor chan_id mismatch is logged at WARN but does NOT
 * fail the lookup — the caller's id wins because the doorbell route
 * is authoritative.
 */
bool lagfx_ring_fifo_descriptor_read(lagfx_protocol_t *p,
                                      uint32_t chan_id,
                                      uint64_t *out_desc_gpa,
                                      uint32_t *out_write_ptr,
                                      uint32_t *out_read_ptr,
                                      uint64_t *out_page0_gpa);

/* Resolve a byte offset within the ring to its data-page GPA via
 * page0's u32 PFN-array. Returns false on DMA failure or on a zero
 * PFN entry (unmapped). On success *out_data_gpa is the absolute GPA
 * of the byte at `offset` within the ring.
 *
 * `ring_size` is the convention for the channel (DisplayPipes use
 * 0x1000, VirtualChannels use 0x10000). A zero ring_size means
 * "treat offset as already modular" — used by callers that have
 * pre-modded their offset.
 */
bool lagfx_ring_resolve_data_gpa(lagfx_protocol_t *p,
                                  uint64_t page0_gpa,
                                  uint32_t ring_size,
                                  uint32_t offset,
                                  uint64_t *out_data_gpa);

/* Publish the host's drained position to descriptor + 0x04 (the
 * read_ptr field). NEVER writes +0x00 — that's the kext's write_ptr
 * (FIFORingDescriptor.md "Bug pattern: clobbering write_ptr"). */
void lagfx_ring_publish_read_ptr(lagfx_protocol_t *p,
                                  uint64_t desc_gpa,
                                  uint32_t read_ptr);

/* FIFORingDescriptor wire size (in bytes). Exported for tests. */
#define LAGFX_RING_FIFO_DESC_BYTES        20u
#define LAGFX_RING_FIFO_DESC_BASE_OFFSET  0x400u

#endif /* LAGFX_DISPATCHERS_RING_COMMON_H */
