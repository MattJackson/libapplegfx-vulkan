/*
 * libapplegfx-vulkan — task-VA → GPA translation via the per-task
 *                      multi-level radix page-table
 * src/protocol/translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The kext maintains a per-task 3-level radix tree mapping task-VA
 * pages to host PFNs. The root page (published over CmdDefineHostTask
 * 0x38) carries a small header pointing at the L1 interior node; the
 * kext writes PTEs into the radix pages directly via guest CPU stores
 * (no MMIO, no opcode signal — see
 * paravirt-re/annotated/AppleParavirtPageTable-StorageNode-setEntry.annotated.asm).
 * The host walks those PTEs on every translate.
 *
 * Format reference: paravirt-re/library/state-machines/per-task-page-table.md
 *
 *   Header at (root_page_pfn << 12):
 *     +0x00  u32 L1_pfn         (PFN of first interior node)
 *     +0x04  u32 levels         (= 3 for typical tasks)
 *     +0x08  u64 reserved0
 *     +0x10  u32 reserved1
 *     +0x14  u64 taskHandleHint
 *
 *   Each interior/leaf node = 1 × 4 KiB page = 1024 × u32 PTEs.
 *   PTE format: bits 30..0 PFN, bit 31 is_leaf flag.
 *
 *   Walk:
 *     shift = (levels-1)*10        (= 20 initially for levels=3)
 *     node = L1_pfn
 *     while shift > 0:
 *         idx = (page_idx >> shift) & 0x3ff
 *         pte = u32 at (node<<12) + idx*4
 *         node = pte & 0x7fffffff
 *         shift -= 10
 *     leaf_idx = page_idx & 0x3ff
 *     pte = u32 at (node<<12) + leaf_idx*4
 *     data_pfn = pte & 0x7fffffff
 */

#include "protocol.h"
#include "state.h"
#include "../device.h"
#include "../common/log.h"

bool lagfx_task_translate_radix(lagfx_protocol_t *p, uint32_t task_id,
                                uint64_t dev_addr, uint64_t *out_gpa,
                                uint64_t *out_run_len) {
    if (!lagfx_protocol_is_valid(p) || !out_gpa) {
        return false;
    }
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        return false;
    }
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        return false;
    }
    if (task->root_page_pfn == 0u) {
        return false;
    }

    uint64_t header_gpa = ((uint64_t)task->root_page_pfn << 12);
    uint8_t hdr_bytes[8] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        header_gpa, sizeof(hdr_bytes),
                                        hdr_bytes)) {
        return false;
    }
    uint32_t l1_pfn = (uint32_t)(hdr_bytes[0]
                                 | ((uint32_t)hdr_bytes[1] << 8)
                                 | ((uint32_t)hdr_bytes[2] << 16)
                                 | ((uint32_t)hdr_bytes[3] << 24));
    uint32_t levels = (uint32_t)(hdr_bytes[4]
                                 | ((uint32_t)hdr_bytes[5] << 8)
                                 | ((uint32_t)hdr_bytes[6] << 16)
                                 | ((uint32_t)hdr_bytes[7] << 24));

    if (l1_pfn == 0u || levels == 0u || levels > 4u) {
        return false;
    }

    uint64_t page_idx = dev_addr >> 12;
    uint32_t node_pfn = l1_pfn;
    int32_t shift = (int32_t)((levels - 1u) * 10u);

    while (shift > 0) {
        uint64_t idx = (page_idx >> shift) & 0x3ffu;
        uint64_t pte_gpa = ((uint64_t)node_pfn << 12) + idx * 4u;
        uint32_t pte = 0u;
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            pte_gpa, sizeof(pte), &pte)) {
            return false;
        }
        if (pte == 0u) {
            return false;
        }
        node_pfn = pte & 0x7fffffffu;
        shift -= 10;
    }

    uint64_t leaf_idx = page_idx & 0x3ffu;
    uint64_t leaf_pte_gpa = ((uint64_t)node_pfn << 12) + leaf_idx * 4u;
    uint32_t leaf_pte = 0u;
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        leaf_pte_gpa, sizeof(leaf_pte),
                                        &leaf_pte)) {
        return false;
    }
    if (leaf_pte == 0u) {
        return false;
    }
    uint32_t data_pfn = leaf_pte & 0x7fffffffu;

    uint64_t page_off = dev_addr & 0xfffu;
    *out_gpa = ((uint64_t)data_pfn << 12) + page_off;
    if (out_run_len) {
        *out_run_len = (uint64_t)0x1000u - page_off;
    }
    return true;
}

bool lagfx_task_translate(lagfx_protocol_t *p, uint32_t task_id,
                          uint64_t dev_addr, uint64_t *out_gpa,
                          uint64_t *out_run_len) {
    if (!lagfx_protocol_is_valid(p) || !out_gpa) {
        return false;
    }
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        return false;
    }

    bool radix_ok = lagfx_task_translate_radix(p, task_id, dev_addr, out_gpa, out_run_len);
    if (radix_ok) {
        LAGFX_LOG("task_translate: taskID=%u dev=0x%llx -> radix gpa=0x%llx "
                  "run=%llu", task_id, (unsigned long long)dev_addr,
                  (unsigned long long)(*out_gpa),
                  (unsigned long long)(out_run_len ? *out_run_len : 0));
        return true;
    }

    for (uint32_t i = 0; i < task->va_interval_count; ++i) {
        const lagfx_va_interval_t *iv = &task->va_intervals[i];
        if (dev_addr >= iv->va_base && dev_addr < iv->va_base + iv->length) {
            *out_gpa = iv->gpa_base + (dev_addr - iv->va_base);
            if (out_run_len) {
                *out_run_len = iv->va_base + iv->length - dev_addr;
            }
            LAGFX_LOG("task_translate: taskID=%u dev=0x%llx — interval "
                        "fallback [%u] va_base=0x%llx gpa_base=0x%llx "
                        "length=0x%llx -> gpa=0x%llx run=%llu",
                        task_id, (unsigned long long)dev_addr, i,
                        (unsigned long long)iv->va_base,
                        (unsigned long long)iv->gpa_base,
                        (unsigned long long)iv->length,
                        (unsigned long long)(*out_gpa),
                        (unsigned long long)(out_run_len ? *out_run_len : 0));
            return true;
        }
    }
    return false;
}
