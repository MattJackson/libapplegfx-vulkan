/*
 * libapplegfx-vulkan — Linux mach_vm_remap semantics
 * src/memory/task.h — private header for task memory management
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * === Design Notes ===============================================
 *
 * This module implements the Darwin "task" concept (reserved VA range)
 * using Linux memfd_create(2) + mmap(2) with MAP_FIXED.
 *
 * On macOS, mach_vm_remap() remaps guest-physical pages into a
 * task's reserved address range. On Linux, we achieve this by:
 *
 * 1. Reserve a VA range via mmap(NULL, size, PROT_NONE, MAP_PRIVATE)
 *    This range is "free" but reserved—we own it, no backing.
 *
 * 2. When aliasing host memory (QEMU's guest RAM pointers):
 *    - Create a memfd (anonymous in-memory file)
 *    - mmap() the memfd read-write into our process' global VA space
 *    - mmap(..., MAP_FIXED | MAP_SHARED) the memfd into task range
 *
 *    This creates two mappings of the SAME physical pages:
 *    - Global: [0x...] → pages, used for QEMU's page table lookups
 *    - Task:   [reserved+offset] → SAME pages, for guest DMA
 *
 * === Key Design Decisions =======================================
 *
 * Q: Why memfd_create + MAP_FIXED, not mremap()?
 * A: mremap(MREMAP_MAYMOVE) unpredictably relocates pages, breaking
 *    QEMU's memory_region invariants. MAP_FIXED is explicit: we map
 *    the SAME fd into a specific VA, preserving physical identity.
 *
 * Q: Why MAP_PRIVATE for reserved range, not MAP_SHARED?
 * A: The reserved range starts empty (PROT_NONE). Only aliases get
 *    MAP_SHARED to the memfd backing. This enforces that unmapping
 *    (mprotect PROT_NONE) doesn't affect siblings.
 *
 * Q: How to alias pages QEMU already owns?
 * A: QEMU passes us raw host pointers (via memory_region_get_ram_ptr).
 *    We do NOT mmap() those directly (we don't own them). Instead,
 *    we create memfd mappings that the guest can then address, and
 *    rely on page-table tricks (not implemented yet) or copy-on-write
 *    semantics to keep them in sync. For now: guest DMA ranges
 *    allocate fresh memfd-backed storage, separate from QEMU's RAMBlock.
 *
 * === Future Enhancements ========================================
 *
 * - Page table shadowing for zero-copy aliasing of QEMU-owned pages
 * - Transparent huge pages (THP) alignment hints
 * - Prctl(PR_SET_THP_DISABLE) per-task policy
 * ------------------------------------------------------------- */

#ifndef LIBAPPLEGFX_TASK_H
#define LIBAPPLEGFX_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque task handle. Implementation details fully hidden. */
typedef struct lagfx_task lagfx_task_t;

/* Reserve a contiguous virtual-address range of vm_size bytes.
 * On success, writes the reserved base address to *base_out and
 * returns an opaque task handle. On failure, returns NULL.
 *
 * The reserved range is initially PROT_NONE (not readable/writable).
 * Callers subsequently populate it via lagfx_task_map_host_memory(). */
lagfx_task_t *lagfx_task_create(size_t vm_size, void **base_out);

/* Release the task, unmapping the reserved range and closing any
 * associated memfds. Safe to call on NULL. */
void lagfx_task_destroy(lagfx_task_t *task);

/* Map host-process memory into the task's reserved range.
 *
 * Parameters:
 *   task        — handle from lagfx_task_create()
 *   vm_offset   — byte offset into the task's reserved range
 *   host_addr   — host pointer to memory to alias
 *   len         — number of bytes to map
 *   read_only   — if true, map read-only; else read-write
 *
 * Returns true on success, false on failure (e.g., offset+len exceeds
 * task size, or system mmap() call failed).
 *
 * Design: We DO NOT directly mmap() the host_addr pointer (which may
 * be in use elsewhere). Instead, this creates a fresh memfd and maps
 * it into the task range at vm_offset. Callers must ensure the
 * mapped memory is properly initialized (or use read_memory callback
 * to fetch guest RAM on-demand). */
bool lagfx_task_map_host_memory(lagfx_task_t *task, uint64_t vm_offset,
                                 void *host_addr, uint64_t len,
                                 bool read_only);

/* Unmaps a range within the task, replacing it with fresh PROT_NONE
 * pages. This is the counterpart to map_host_memory() for cleanup.
 *
 * Parameters:
 *   task      — handle from lagfx_task_create()
 *   vm_offset — byte offset into task's reserved range
 *   len       — number of bytes to unmap
 *
 * Returns true on success, false on error. */
bool lagfx_task_unmap(lagfx_task_t *task, uint64_t vm_offset,
                       uint64_t len);

#endif /* LIBAPPLEGFX_TASK_H */
