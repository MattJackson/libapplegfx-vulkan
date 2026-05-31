/*
 * libapplegfx-vulkan — Object ID resolver for Phase B metallib lookup
 * src/protocol/object_resolver.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Helper API to walk the per-task heap slot table and resolve object IDs
 * to their backing bytes. Cites paravirt-re/library/phase_b_step5_v2_4_MTLB_CONFIRMED.md
 * for the data structure spec (two-level radix walk through type=0x06 slots).
 */

#ifndef LIBAPPLEGFX_PROTOCOL_OBJECT_RESOLVER_H
#define LIBAPPLEGFX_PROTOCOL_OBJECT_RESOLVER_H

#include <stdbool.h>
#include <stdint.h>
#include "state.h"

/* APVObjectType values observed in slot type byte. */
#define LAGFX_APV_TYPE_UNKNOWN_01   0x01u
#define LAGFX_APV_TYPE_UNKNOWN_05   0x05u
#define LAGFX_APV_TYPE_FUNCTION     0x06u  /* function/library; bytes lead to MTLB */
#define LAGFX_APV_TYPE_PIPELINE     0x07u  /* render/compute pipeline state */

/* Look up the metallib bytes for a function objectId (type=0x06 slot).
 * Returns true on success and populates *out_gpa, *out_len with the
 * physical GPA and byte length of the MTLB blob. If out_va is non-NULL it
 * receives the metallib's task-VIRTUAL start address — REQUIRED to read the
 * blob correctly: the metallib is contiguous in the task's virtual address
 * space but its physical (GPA) pages are NOT necessarily contiguous, so a
 * flat read_memory(*out_gpa, *out_len) corrupts everything past the first
 * page boundary. Read via lagfx_task_read_virtual(p, task, *out_va,
 * *out_len, dst) instead. Returns false if:
 *   - task->heap_pfn is 0 (no heap published yet)
 *   - radix-walk fails at any level
 *   - the slot is not live (type=0) or not type=0x06
 *   - the bytes at the resolved GPA don't start with 'MTLB' magic
 */
bool lagfx_lookup_function_bytes(lagfx_protocol_t *p,
                                  const lagfx_task_entry_t *task,
                                  uint32_t func_object_id,
                                  uint64_t *out_gpa,
                                  uint32_t *out_len,
                                  uint64_t *out_va);

/* Generic per-task object-slot resolver: walk slot[object_id] in the task
 * heap and return its type byte + the data it points at (virtual address,
 * and translated GPA — 0 if unmapped). Buffers/textures resolve the same
 * way functions do (lagfx_lookup_function_bytes is the type=0x06 special
 * case that additionally reads the MTLB length); this is the building block
 * for draw-time resource binding (resolve a SetVertexBuffers ref → its data
 * → read page-aware → upload to a VkBuffer). Returns false if heap unpublished
 * or the slot/translate walk fails. */
bool lagfx_resolve_object_data(lagfx_protocol_t *p,
                                const lagfx_task_entry_t *task,
                                uint32_t object_id,
                                uint8_t *out_type,
                                uint64_t *out_data_va,
                                uint64_t *out_data_gpa);

/* Read `len` bytes starting at task-VIRTUAL address `va` into `buf`,
 * translating each guest page (VA->GPA) separately via the per-task radix
 * walk. Use this for any guest buffer larger than a page that is virtually
 * (not necessarily physically) contiguous — e.g. captured metallibs. A flat
 * read_memory of such a buffer reads the wrong physical page after the first
 * boundary. Returns false if any page fails to translate or read. */
bool lagfx_task_read_virtual(lagfx_protocol_t *p,
                              const lagfx_task_entry_t *task,
                              uint64_t va,
                              uint32_t len,
                              uint8_t *buf);

/* Find the function objectIds referenced by a pipeline-state slot.
 * Walks the type=0x07 descriptor at slot[pipeline_object_id].bytes_va
 * and scans for "0x04 XX" tokens. For each XX in the scan, validates
 * that slot[XX].type == 0x06; valid XXs are returned as function refs.
 *
 * On success returns true and populates *out_vertex_ref (always) and
 * *out_fragment_ref (zero if compute pipeline / single function). For
 * a render pipeline both should be non-zero.
 *
 * Returns false if pipeline slot is invalid or no valid function refs
 * are found.
 */
bool lagfx_lookup_pipeline_function_refs(lagfx_protocol_t *p,
                                          const lagfx_task_entry_t *task,
                                          uint32_t pipeline_object_id,
                                          uint8_t *out_vertex_ref,
                                          uint8_t *out_fragment_ref);

#endif /* LIBAPPLEGFX_PROTOCOL_OBJECT_RESOLVER_H */
