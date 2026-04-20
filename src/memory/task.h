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

/* The lagfx_task_t typedef and lagfx_task_create/_destroy/_map_host_memory/_unmap
 * declarations now live in the public header. This file remains as a
 * private entry point for any future in-tree internal helpers (e.g.
 * debugging introspection, stats) without exposing them to consumers.
 *
 * Include the public API here so internal TUs still get the full type. */

#include "libapplegfx-vulkan.h"

#endif /* LIBAPPLEGFX_TASK_H */
