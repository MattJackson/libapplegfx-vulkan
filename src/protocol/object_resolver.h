/*
 * libapplegfx-vulkan — Object ID resolver for Phase B metallib lookup
 * src/protocol/object_resolver.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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

/*
 * Collect ALL function-typed refs from the pipeline descriptor (full length,
 * any order), up to `cap`, into out_refs; returns the count. The caller binds
 * each stage from whichever ref's metallib contains that stage — robust to the
 * fragment-ref-first ordering and >128-byte descriptors that broke the
 * two-ref vertex/fragment split above. Returns 0 on any failure.
 */
size_t lagfx_lookup_pipeline_function_ref_list(lagfx_protocol_t *p,
                                               const lagfx_task_entry_t *task,
                                               uint32_t pipeline_object_id,
                                               uint8_t *out_refs, size_t cap);

/*
 * Pure, guest-memory-free descriptor token scan used by the list lookup above.
 * For each `0x04 XX` tag in desc[0..limit), collects the distinct nonzero XX
 * (first-seen order) for which is_function(ctx, XX) is true, up to cap. Exposed
 * for unit testing the fragment-ref-first / >128-byte-vertex ordering fix.
 */
size_t lagfx_scan_descriptor_function_refs(const uint8_t *desc, uint32_t limit,
                                           bool (*is_function)(void *ctx, uint8_t ref),
                                           void *ctx, uint8_t *out, size_t cap);

/*
 * Decode the vertex-buffer-layout stride from a render pipeline's serialized
 * MTLVertexDescriptor (the PSO blob). The blob is a `04 <u32 value> <tag:u8>`
 * token stream; a MTLVertexBufferLayout stride is `04 <stride> 02`. Returns the
 * decoded stride (bytes) or 0 if the pipeline has no vertex descriptor / can't
 * be read. Ground truth: pso 0x2c/0x23 (CoreAnimation composites) = 48, pso 0x14
 * (fullscreen-quad) = 24 — the round8(sum-of-attr-sizes) heuristic gets these
 * wrong and smears the geometry.
 */
/* GOAL-M2z: per-attribute {MTLVertexFormat, offset, bufferIndex} decoded from
 * the PSO's serialized MTLVertexDescriptor. Entry pattern in the blob:
 * `01 04 <fmt u32> 02 04 <off u32> 03 04 <bufidx u32>` (keys: 01=format,
 * 02=offset, 03=bufferIndex; 0x04 = u32 type tag). Ground truth: caret/9patch
 * pipes carry fmt 31 (Float4) @0, 29 (Float2) @16, 9 (UChar4Normalized) @32 —
 * the rgba8 color our SFLOAT binding read as NaN (invisible fills).
 * Returns the number of attributes found (<= cap), sorted by offset. */
typedef struct { uint32_t fmt, off, bufidx; } lagfx_pso_vtx_attr_t;
uint32_t lagfx_parse_pso_vertex_attrs(lagfx_protocol_t *p,
                                      const lagfx_task_entry_t *task,
                                      uint32_t pipeline_object_id,
                                      lagfx_pso_vtx_attr_t *out, uint32_t cap);

uint32_t lagfx_parse_pso_vertex_stride(lagfx_protocol_t *p,
                                        const lagfx_task_entry_t *task,
                                        uint32_t pipeline_object_id);

#endif /* LIBAPPLEGFX_PROTOCOL_OBJECT_RESOLVER_H */
