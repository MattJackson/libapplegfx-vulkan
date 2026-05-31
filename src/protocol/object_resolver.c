/*
 * libapplegfx-vulkan — Object ID resolver for Phase B metallib lookup
 * src/protocol/object_resolver.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of lagfx_lookup_function_bytes() and
 * lagfx_lookup_pipeline_function_refs(). Cites
 * paravirt-re/library/phase_b_step5_v2_4_MTLB_CONFIRMED.md for the
 * data structure spec (two-level radix walk through type=0x06 slots).
 */

#include "object_resolver.h"
#include "handlers/compute/task_translate.h"
#include "device.h"
#include "common/log.h"
#include "common/le.h"

/* Compute slot VA for a given object ID: heap_va + objectId * 12.
 * Cites: phase_b_step5_v2_4_MTLB_CONFIRMED.md line 13. */
static inline uint64_t
slot_va_for(uint64_t heap_pfn, uint32_t object_id) {
    return (heap_pfn << 12) + (uint64_t)object_id * 12ull;
}

/* Read a u64 LE from memory via shell. Returns true on success. */
static bool
read_u64_via_shell(const lagfx_device_descriptor_t *desc, uint64_t gpa, uint64_t *out) {
    uint8_t buf[8] = {0};
    if (!desc->shell.read_memory(desc->shell.opaque, gpa, 8, buf)) {
        return false;
    }
    *out = lagfx_le64(buf);
    return true;
}

/* Read a u32 LE from memory via shell. Returns true on success. */
static bool
read_u32_via_shell(const lagfx_device_descriptor_t *desc, uint64_t gpa, uint32_t *out) {
    uint8_t buf[4] = {0};
    if (!desc->shell.read_memory(desc->shell.opaque, gpa, 4, buf)) {
        return false;
    }
    *out = lagfx_le32(buf);
    return true;
}

/* Read type/flags/bytes_va from a slot GPA. Sets *type_out, *bytes_va_out.
 * Returns true on success. */
static bool
read_slot_fields(const lagfx_device_descriptor_t *desc, uint64_t slot_gpa,
                 uint8_t *type_out, uint64_t *bytes_va_out) {
    uint8_t buf[12] = {0};
    if (!desc->shell.read_memory(desc->shell.opaque, slot_gpa, 12, buf)) {
        return false;
    }
    *type_out = buf[0];
    *bytes_va_out = lagfx_le64(buf + 4);
    return true;
}

bool
lagfx_lookup_function_bytes(lagfx_protocol_t *p,
                            const lagfx_task_entry_t *task,
                            uint32_t func_object_id,
                            uint64_t *out_gpa,
                            uint32_t *out_len,
                            uint64_t *out_va) {
    if (!p || !task || !out_gpa || !out_len) {
        return false;
    }
    if (out_va) *out_va = 0;

    /* Step 1: heap must be published. Cites phase_b_step5_v2_4_MTLB_CONFIRMED.md line 9-10. */
    if (task->heap_pfn == 0u) {
        LAGFX_TRACE("lookup_function_bytes: task_id=%u has no heap_pfn", (unsigned)task->id);
        return false;
    }

    /* Step 2: compute slot VA and translate to GPA. */
    uint64_t slot_va = slot_va_for(task->heap_pfn, func_object_id);
    uint64_t slot_gpa = 0;
    if (!lagfx_task_translate(p, task, slot_va, &slot_gpa)) {
        LAGFX_TRACE("lookup_function_bytes: radix walk failed for slot_va=0x%llx",
                    (unsigned long long)slot_va);
        return false;
    }

    /* Step 3: read slot fields. */
    uint8_t slot_type = 0;
    uint64_t bytes_va = 0;
    if (!read_slot_fields((const lagfx_device_descriptor_t *)&((lagfx_device_t *)p->dev)->desc, slot_gpa, &slot_type, &bytes_va)) {
        LAGFX_TRACE("lookup_function_bytes: failed to read slot at gpa=0x%llx",
                    (unsigned long long)slot_gpa);
        return false;
    }

    /* Step 4: validate type == 0x06. */
    if (slot_type != LAGFX_APV_TYPE_FUNCTION) {
        LAGFX_TRACE("lookup_function_bytes: slot[0x%x] type=0x%02x (expected 0x06)",
                    (unsigned)func_object_id, (unsigned)slot_type);
        return false;
    }

    /* Step 5: translate bytes_va to GPA. */
    uint64_t bytes_gpa = 0;
    if (!lagfx_task_translate(p, task, bytes_va, &bytes_gpa)) {
        LAGFX_TRACE("lookup_function_bytes: radix walk failed for bytes_va=0x%llx",
                    (unsigned long long)bytes_va);
        return false;
    }

    /* Step 6: read first u64 at bytes_gpa → next_va. Cites phase_b_step5_v2_4_MTLB_CONFIRMED.md line 18. */
    uint64_t next_va = 0;
    if (!read_u64_via_shell((const lagfx_device_descriptor_t *)&((lagfx_device_t *)p->dev)->desc, bytes_gpa, &next_va)) {
        LAGFX_TRACE("lookup_function_bytes: failed to read next_va at bytes_gpa=0x%llx",
                    (unsigned long long)bytes_gpa);
        return false;
    }

    if (next_va == 0u) {
        LAGFX_TRACE("lookup_function_bytes: next_va is zero");
        return false;
    }

    /* Step 7: translate next_va to GPA. */
    uint64_t next_gpa = 0;
    if (!lagfx_task_translate(p, task, next_va, &next_gpa)) {
        LAGFX_TRACE("lookup_function_bytes: radix walk failed for next_va=0x%llx",
                    (unsigned long long)next_va);
        return false;
    }

    /* Step 8: read MTLB header (first 24 bytes). Check magic at +0, length at +16. */
    uint8_t mtlb_header[24] = {0};
    lagfx_device_t *dev_for_dma = (lagfx_device_t *)p->dev;
    if (!dev_for_dma->desc.shell.read_memory(dev_for_dma->desc.shell.opaque, next_gpa, 24, mtlb_header)) {
        LAGFX_TRACE("lookup_function_bytes: failed to read MTLB header at gpa=0x%llx",
                    (unsigned long long)next_gpa);
        return false;
    }

    /* Validate 'MTLB' magic. */
    if (mtlb_header[0] != 'M' || mtlb_header[1] != 'T' ||
        mtlb_header[2] != 'L' || mtlb_header[3] != 'B') {
        LAGFX_TRACE("lookup_function_bytes: missing MTLB magic at gpa=0x%llx",
                    (unsigned long long)next_gpa);
        return false;
    }

    /* Read total length from +16 (LE u32). Per V2.4 dump for ref=0x1:
     *   first32 = 4d544c42 01800200 08000081 0f000700 d4220000 00000000 ...
     * Length 0x22d4 = 8916 is at bytes +16..+19 (the 5th u32), not +20. */
    uint32_t mtlb_len = 0;
    if (!read_u32_via_shell((const lagfx_device_descriptor_t *)&((lagfx_device_t *)p->dev)->desc, next_gpa + 16, &mtlb_len)) {
        LAGFX_TRACE("lookup_function_bytes: failed to read length at gpa+16=0x%llx",
                    (unsigned long long)(next_gpa + 16));
        return false;
    }

    if (mtlb_len == 0 || mtlb_len > 16 * 1024 * 1024) {
        /* Sanity check: reject absurdly small or large metallibs. */
        LAGFX_TRACE("lookup_function_bytes: invalid MTLB length=%u", mtlb_len);
        return false;
    }

    *out_gpa = next_gpa;
    *out_len = mtlb_len;
    if (out_va) *out_va = next_va;
    return true;
}

bool
lagfx_task_read_virtual(lagfx_protocol_t *p,
                        const lagfx_task_entry_t *task,
                        uint64_t va,
                        uint32_t len,
                        uint8_t *buf) {
    if (!p || !task || !buf) return false;
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    const uint64_t PAGE = 4096u;
    uint32_t done = 0u;
    while (done < len) {
        uint64_t cur_va   = va + done;
        uint64_t page_off = cur_va & (PAGE - 1u);
        uint64_t avail    = PAGE - page_off;          /* bytes to next page boundary */
        uint32_t chunk    = (avail < (uint64_t)(len - done)) ? (uint32_t)avail : (len - done);
        uint64_t gpa = 0;
        if (!lagfx_task_translate(p, task, cur_va, &gpa)) {
            LAGFX_TRACE("read_virtual: VA->GPA failed at va=0x%llx (off %u/%u)",
                        (unsigned long long)cur_va, done, len);
            return false;
        }
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque, gpa, chunk, buf + done)) {
            LAGFX_TRACE("read_virtual: read_memory failed gpa=0x%llx chunk=%u",
                        (unsigned long long)gpa, chunk);
            return false;
        }
        done += chunk;
    }
    return true;
}

/* Helper: read a single byte via shell. */
static bool
read_byte_via_shell(const lagfx_device_descriptor_t *desc, uint64_t gpa, uint8_t *out) {
    uint8_t buf[1] = {0};
    if (!desc->shell.read_memory(desc->shell.opaque, gpa, 1, buf)) {
        return false;
    }
    *out = buf[0];
    return true;
}

/* Helper: look up a single slot's type byte by object ID. */
static bool
lookup_slot_type(lagfx_protocol_t *p, const lagfx_task_entry_t *task,
                 uint32_t object_id, uint8_t *type_out) {
    if (task->heap_pfn == 0u) {
        return false;
    }

    uint64_t slot_va = slot_va_for(task->heap_pfn, object_id);
    uint64_t slot_gpa = 0;
    if (!lagfx_task_translate(p, task, slot_va, &slot_gpa)) {
        return false;
    }

    return read_byte_via_shell((const lagfx_device_descriptor_t *)&((lagfx_device_t *)p->dev)->desc, slot_gpa, type_out);
}

bool
lagfx_lookup_pipeline_function_refs(lagfx_protocol_t *p,
                                     const lagfx_task_entry_t *task,
                                     uint32_t pipeline_object_id,
                                     uint8_t *out_vertex_ref,
                                     uint8_t *out_fragment_ref) {
    if (!p || !task || !out_vertex_ref || !out_fragment_ref) {
        return false;
    }

    /* Step 1: validate heap is published. */
    if (task->heap_pfn == 0u) {
        LAGFX_TRACE("lookup_pipeline_refs: task_id=%u has no heap_pfn", (unsigned)task->id);
        return false;
    }

    *out_vertex_ref = 0;
    *out_fragment_ref = 0;

    /* Step 2: compute slot VA for pipeline and translate. */
    uint64_t slot_va = slot_va_for(task->heap_pfn, pipeline_object_id);
    uint64_t slot_gpa = 0;
    if (!lagfx_task_translate(p, task, slot_va, &slot_gpa)) {
        LAGFX_TRACE("lookup_pipeline_refs: radix walk failed for pipeline slot[0x%x] va=0x%llx",
                    (unsigned)pipeline_object_id, (unsigned long long)slot_va);
        return false;
    }

    /* Step 3: read slot fields to get bytes_va. */
    uint8_t slot_type = 0;
    uint64_t bytes_va = 0;
    if (!read_slot_fields((const lagfx_device_descriptor_t *)&((lagfx_device_t *)p->dev)->desc, slot_gpa, &slot_type, &bytes_va)) {
        LAGFX_TRACE("lookup_pipeline_refs: failed to read pipeline slot at gpa=0x%llx",
                    (unsigned long long)slot_gpa);
        return false;
    }

    /* Step 4: validate type == 0x07 (pipeline). */
    if (slot_type != LAGFX_APV_TYPE_PIPELINE) {
        LAGFX_TRACE("lookup_pipeline_refs: slot[0x%x] type=0x%02x (expected 0x07)",
                    (unsigned)pipeline_object_id, (unsigned)slot_type);
        return false;
    }

    /* Step 5: translate bytes_va to GPA. */
    uint64_t bytes_gpa = 0;
    if (!lagfx_task_translate(p, task, bytes_va, &bytes_gpa)) {
        LAGFX_TRACE("lookup_pipeline_refs: radix walk failed for pipeline bytes_va=0x%llx",
                    (unsigned long long)bytes_va);
        return false;
    }

    /* Step 6: read descriptor header (128 B covers all observed sizes 0x3c..0x154). */
    uint8_t desc[128] = {0};
    lagfx_device_t *dev_for_dma = (lagfx_device_t *)p->dev;
    if (!dev_for_dma->desc.shell.read_memory(dev_for_dma->desc.shell.opaque, bytes_gpa, 128, desc)) {
        LAGFX_TRACE("lookup_pipeline_refs: failed to read pipeline descriptor at gpa=0x%llx",
                    (unsigned long long)bytes_gpa);
        return false;
    }

    /* Step 7: scan for "0x04 XX" tokens at every byte offset.
     * Cites phase_b_step5_v2_4_MTLB_CONFIRMED.md line 57-58. */
    uint8_t vertex_ref = 0, fragment_ref = 0;
    bool found_vertex = false, found_fragment = false;

    for (uint32_t i = 0; i < 128u && (!found_vertex || !found_fragment); i++) {
        if (desc[i] == 0x04u && i + 1 < 128u) {
            uint8_t candidate = desc[i + 1];
            if (candidate == 0u) {
                continue;  /* Skip null refs */
            }

            /* Validate that slot[candidate] is type=0x06. */
            uint8_t candidate_type = 0;
            if (!lookup_slot_type(p, task, candidate, &candidate_type)) {
                continue;  /* Translation failed or invalid slot */
            }

            if (candidate_type == LAGFX_APV_TYPE_FUNCTION) {
                /* First valid ref is vertex; second (if different) is fragment. */
                if (!found_vertex) {
                    vertex_ref = candidate;
                    found_vertex = true;
                    LAGFX_TRACE("lookup_pipeline_refs: found vertex func ref=0x%x", (unsigned)candidate);
                } else if (candidate != vertex_ref && !found_fragment) {
                    fragment_ref = candidate;
                    found_fragment = true;
                    LAGFX_TRACE("lookup_pipeline_refs: found fragment func ref=0x%x", (unsigned)candidate);
                }
            }
        }
    }

    if (!found_vertex) {
        LAGFX_TRACE("lookup_pipeline_refs: no vertex function ref found for pipeline 0x%x",
                    (unsigned)pipeline_object_id);
        return false;
    }

    /* Compute pipelines may have only one function ref. */
    *out_vertex_ref = vertex_ref;
    *out_fragment_ref = fragment_ref;
    return true;
}
