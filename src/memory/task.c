/*
 * libapplegfx-vulkan — Linux mach_vm_remap semantics
 * src/memory/task.c — memfd-based task memory management
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Enable memfd_create() on Linux. Requires Linux 5.4+. */
#define _GNU_SOURCE

#include "task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Private task structure. */
struct lagfx_task {
    void *reserved_base;      /* Start of reserved VA range */
    size_t reserved_size;     /* Total reserved size in bytes */
    int memfd;                /* Backing memfd for current mappings, or -1 */
};

/* Fallback memfd_create if not in libc (Linux 5.4+). */
#ifndef __NR_memfd_create
#define __NR_memfd_create 319  /* x86_64 syscall number */
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static int memfd_create_fallback(const char *name, unsigned int flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}

/* Utility: Create an anonymous memfd for use as mapping backing. */
static int task_create_memfd(size_t size) {
    int fd = memfd_create_fallback("lagfx-guest-dma", MFD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "memfd_create failed: %s\n", strerror(errno));
        return -1;
    }

    /* Pre-allocate the memfd to avoid SIGBUS on first access. */
    if (ftruncate(fd, (off_t)size) < 0) {
        fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

lagfx_task_t *lagfx_task_create(size_t vm_size, void **base_out) {
    if (!base_out || vm_size == 0) {
        return NULL;
    }

    /* Reserve the virtual address range with PROT_NONE.
     * This reserves the range without allocating backing pages.
     * Flags:
     *   MAP_PRIVATE  — changes to this range don't affect other processes
     *   MAP_ANONYMOUS — no file backing; just reserve address space */
    void *base = mmap(NULL, vm_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "Failed to reserve %zu byte VA range: %s\n",
                vm_size, strerror(errno));
        return NULL;
    }

    lagfx_task_t *task = malloc(sizeof(*task));
    if (!task) {
        munmap(base, vm_size);
        return NULL;
    }

    task->reserved_base = base;
    task->reserved_size = vm_size;
    task->memfd = -1;

    *base_out = base;
    return task;
}

void lagfx_task_destroy(lagfx_task_t *task) {
    if (!task) {
        return;
    }

    if (task->reserved_base) {
        munmap(task->reserved_base, task->reserved_size);
    }

    if (task->memfd >= 0) {
        close(task->memfd);
    }

    free(task);
}

bool lagfx_task_map_host_memory(lagfx_task_t *task, uint64_t vm_offset,
                                 void *host_addr, uint64_t len,
                                 bool read_only) {
    if (!task || vm_offset + len > task->reserved_size || len == 0) {
        return false;
    }

    /* Create or reuse memfd backing for this mapping. */
    if (task->memfd < 0) {
        task->memfd = task_create_memfd(task->reserved_size);
        if (task->memfd < 0) {
            return false;
        }
    }

    /* Target address in the reserved range. */
    void *target = (char *)task->reserved_base + vm_offset;

    /* Map protection flags. */
    int prot = PROT_READ;
    if (!read_only) {
        prot |= PROT_WRITE;
    }

    /* Map the memfd into the task's reserved range at the fixed offset.
     * Flags:
     *   MAP_FIXED    — place mapping at exact target address
     *   MAP_SHARED   — share backing pages across this and any other
     *                  mappings of the same memfd (future: QEMU aliases)
     *   MAP_ANONYMOUS — needed here to fill gaps initially (or we mmap
     *                    the memfd) */
    void *mapped = mmap(target, (size_t)len, prot,
                         MAP_FIXED | MAP_SHARED, task->memfd,
                         (off_t)vm_offset);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "mmap(MAP_FIXED) at offset %llu failed: %s\n",
                (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    /* If host_addr is provided (e.g., QEMU's guest RAM pointer),
     * copy its contents into the newly mapped range.
     * Future: This could be optimized with page-table tricks to avoid
     * the copy, but for MVP we err on the side of correctness. */
    if (host_addr) {
        memcpy(target, host_addr, (size_t)len);
    }

    return true;
}

bool lagfx_task_unmap(lagfx_task_t *task, uint64_t vm_offset,
                       uint64_t len) {
    if (!task || vm_offset + len > task->reserved_size || len == 0) {
        return false;
    }

    void *target = (char *)task->reserved_base + vm_offset;

    /* Replace the range with fresh PROT_NONE pages using mmap.
     * This overwrites the old mapping at MAP_FIXED. */
    void *result = mmap(target, (size_t)len, PROT_NONE,
                         MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        fprintf(stderr, "mmap(PROT_NONE) unmap at offset %llu failed: %s\n",
                (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    return true;
}
