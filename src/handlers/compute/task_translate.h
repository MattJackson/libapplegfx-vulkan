/*
 * libapplegfx-vulkan — Per-task radix tree address translator
 * src/handlers/compute/task_translate.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exports lagfx_task_translate() for VA→GPA translation via the
 * per-task radix tree (rooted at task->root_page_pfn). Used by Phase B
 * step 5 diagnostic to walk task-VA heap[+0x1000 + ref*12] → GPA.
 *
 * Cites: paravirt-re/library/state-machines/per-task-page-table.md
 */

#ifndef LAGFX_HANDLERS_COMPUTE_TASK_TRANSLATE_H
#define LAGFX_HANDLERS_COMPUTE_TASK_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>
#include "protocol/state.h"

/* Translate a task-virtual address to GPA via the 3-level radix tree
 * rooted at task->root_page_pfn. Returns true on success.
 *
 * See paravirt-re/library/state-machines/per-task-page-table.md
 * for the wire format. PTE layout:
 *
 *   bits 30..0  next-PFN (interior nodes) OR data-PFN (leaf nodes)
 *   bit  31     is_leaf — 0 for interior, 1 for leaf
 *
 * An interior-level PTE with bit 31 set means the walk terminates
 * early — the PTE directly identifies a data page covering the
 * remainder of the address. This is the "huge page" / compact-map
 * case. The remaining low bits of dev_addr become the in-page offset
 * scaled to the level reached.
 */
bool lagfx_task_translate(lagfx_protocol_t *p,
                          const lagfx_task_entry_t *task,
                          uint64_t dev_addr,
                          uint64_t *out_gpa);

#endif /* LAGFX_HANDLERS_COMPUTE_TASK_TRANSLATE_H */
