/*
 * libapplegfx-vulkan — Linux mach_vm_remap semantics
 * src/memory/task.c — memfd-based task memory management
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * The production target is Linux; on Darwin (used for ad-hoc dev
 * builds) memfd_create is absent and we fall back to mkstemp + unlink
 * so syntax-check / header-only builds still succeed. The fallback
 * is NOT production-quality (no MFD_CLOEXEC guarantees, slower) —
 * callers expecting full semantics must run on Linux.
 */

/* Enable memfd_create() and mremap() on Linux. Requires Linux 5.4+.
 * _GNU_SOURCE may already be injected globally by meson; guard
 * redefinition. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "task.h"

#include "../common/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

/* MREMAP_MAYMOVE / MREMAP_FIXED live in <sys/mman.h> on glibc when
 * _GNU_SOURCE is defined; older headers may omit them. Provide the
 * canonical Linux values as a fallback so the Option-C aliasing path
 * compiles on any reasonably recent glibc. These are kernel ABI
 * constants — stable. */
#ifdef __linux__
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED   2
#endif
#endif

/* Private task structure. */
struct lagfx_task {
    void *reserved_base;      /* Start of reserved VA range */
    size_t reserved_size;     /* Total reserved size in bytes */
    int memfd;                /* Backing memfd for current mappings, or -1 */
};

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* Fallback memfd_create if not in libc (Linux 5.4+). */
#ifdef __linux__
#ifndef __NR_memfd_create
#if defined(__x86_64__)
#define __NR_memfd_create 319
#elif defined(__aarch64__)
#define __NR_memfd_create 279
#else
#define __NR_memfd_create 319  /* best guess */
#endif
#endif

static int memfd_create_fallback(const char *name, unsigned int flags) {
    return (int)syscall(__NR_memfd_create, name, flags);
}
#else
/* Non-Linux fallback: anonymous tmp file. Production must run on Linux. */
static int memfd_create_fallback(const char *name, unsigned int flags) {
    (void)name;
    (void)flags;
    char tmpl[] = "/tmp/lagfx-memfd.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
    }
    return fd;
}
#endif

/* Utility: Create an anonymous memfd for use as mapping backing. */
static int task_create_memfd(size_t size) {
    int fd = memfd_create_fallback("lagfx-guest-dma", MFD_CLOEXEC);
    if (fd < 0) {
        LAGFX_ERR("memfd_create failed: %s", strerror(errno));
        return -1;
    }

    /* Pre-allocate the memfd to avoid SIGBUS on first access. */
    if (ftruncate(fd, (off_t)size) < 0) {
        LAGFX_ERR("ftruncate failed: %s", strerror(errno));
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
        LAGFX_ERR("Failed to reserve %zu byte VA range: %s",
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

/* Fallback path: fill the target VA with memfd-backed pages and copy
 * host_addr contents into them. This breaks post-map coherence
 * (writes by QEMU to host_addr do NOT propagate to the task VA),
 * so it is used only when true aliasing is impossible — the Darwin
 * dev path, or the Linux mremap fallback when the source VMA is
 * MAP_PRIVATE anonymous. Callers get a degraded Phase-1 grade of
 * "ok for read-mostly, wrong for guest-writable DMA". See
 * docs/memory-coherence-audit.md for the full rationale. */
static bool task_map_via_copy(lagfx_task_t *task, uint64_t vm_offset,
                               void *target, void *host_addr,
                               uint64_t len, int prot) {
    if (task->memfd < 0) {
        task->memfd = task_create_memfd(task->reserved_size);
        if (task->memfd < 0) {
            return false;
        }
    }

    /* Always map R/W for the copy — we're about to memcpy into it.
     * If the caller asked for read-only, tighten via mprotect after. */
    void *mapped = mmap(target, (size_t)len, PROT_READ | PROT_WRITE,
                         MAP_FIXED | MAP_SHARED, task->memfd,
                         (off_t)vm_offset);
    if (mapped == MAP_FAILED) {
        LAGFX_ERR("mmap(MAP_FIXED) at offset %llu failed: %s",
                (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    if (host_addr) {
        memcpy(target, host_addr, (size_t)len);
    }

    if (prot != (PROT_READ | PROT_WRITE)) {
        if (mprotect(target, (size_t)len, prot) != 0) {
            LAGFX_ERR("mprotect to caller prot=0x%x failed: %s",
                    prot, strerror(errno));
            return false;
        }
    }

    return true;
}

bool lagfx_task_map_host_memory(lagfx_task_t *task, uint64_t vm_offset,
                                 void *host_addr, uint64_t len,
                                 bool read_only) {
    if (!task || vm_offset + len > task->reserved_size || len == 0) {
        return false;
    }

    /* Target address in the reserved range. */
    void *target = (char *)task->reserved_base + vm_offset;

    /* Map protection flags. */
    int prot = PROT_READ;
    if (!read_only) {
        prot |= PROT_WRITE;
    }

    /* If no host_addr supplied, the caller wants a freshly-zeroed
     * read/writable region inside the task — use the memfd path
     * (no copy needed since there's no source). */
    if (!host_addr) {
        return task_map_via_copy(task, vm_offset, target, NULL, len,
                                  prot);
    }

#ifdef __linux__
    /* === Option C: zero-copy aliasing via mremap duplicate-mapping ========
     *
     * Per docs/memory-coherence-audit.md, the correct semantics for this
     * function are to make `target` alias the same physical pages that
     * `host_addr` points at — not to copy. With a MAP_SHARED source VMA
     * (QEMU's memfd-backed RAMBlock, or the test's MAP_SHARED memfd),
     * `mremap(old_size=0)` duplicates the source mapping into `target` —
     * both pointers then refer to the same physical pages. Guest writes
     * via QEMU are immediately visible at the task VA and vice versa.
     *
     * Preconditions for the duplicate-mapping form:
     *   - old_size == 0  (this is what triggers duplication, not move)
     *   - source VMA carries MAP_SHARED (otherwise EINVAL)
     *   - MREMAP_MAYMOVE | MREMAP_FIXED with new_address == target
     *
     * mremap with MREMAP_FIXED refuses to overlay an existing mapping
     * (unlike mmap MAP_FIXED which silently replaces). We must munmap
     * the PROT_NONE reservation at `target` first to make a hole, then
     * mremap fills it. If mremap fails we restore the hole via fresh
     * PROT_NONE mmap so the reservation isn't permanently punctured.
     *
     * Original size=len form (MOVE) is wrong: it invalidates host_addr
     * at the source. Keep old_size=0 (DUPLICATE). */
    if (munmap(target, (size_t)len) != 0) {
        LAGFX_ERR(
            "lagfx_task_map_host_memory: munmap(target) failed at "
            "offset %llu: %s",
            (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    void *aliased = mremap(host_addr, 0, (size_t)len,
                            MREMAP_MAYMOVE | MREMAP_FIXED, target);
    if (aliased != MAP_FAILED) {
        /* mremap cannot set protection; apply requested prot now.
         * Non-fatal if mprotect fails — caller still has an alias. */
        if (mprotect(target, (size_t)len, prot) != 0) {
            LAGFX_WARN(
                "lagfx_task_map_host_memory: mprotect failed at "
                "offset %llu: %s (continuing with source prot)",
                (unsigned long long)vm_offset, strerror(errno));
        }
        return true;
    }

    int saved_errno = errno;

    /* mremap failed. Restore the PROT_NONE reservation before
     * falling back so the task VA stays intact. */
    void *restored = mmap(target, (size_t)len, PROT_NONE,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (restored == MAP_FAILED) {
        LAGFX_ERR(
            "lagfx_task_map_host_memory: failed to restore PROT_NONE "
            "reservation at offset %llu after mremap failure: %s",
            (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    LAGFX_WARN(
        "lagfx_task_map_host_memory: mremap aliasing failed at offset %llu"
        ": %s — falling back to copy (coherence may not hold)",
        (unsigned long long)vm_offset, strerror(saved_errno));
    return task_map_via_copy(task, vm_offset, target, host_addr, len, prot);
#else
    /* Non-Linux dev path (Darwin / others): no mremap, retain the
     * legacy copy-on-map behaviour. Production runs on Linux; the
     * audit doc covers why this is acceptable for dev builds only. */
    LAGFX_WARN(
        "lagfx_task_map_host_memory: non-Linux host — using"
        " copy-on-map fallback at offset %llu (coherence will"
        " fail post-map; Linux is the production target)",
        (unsigned long long)vm_offset);
    return task_map_via_copy(task, vm_offset, target, host_addr, len,
                              prot);
#endif
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
        LAGFX_ERR("mmap(PROT_NONE) unmap at offset %llu failed: %s",
                (unsigned long long)vm_offset, strerror(errno));
        return false;
    }

    return true;
}

void *lagfx_task_get_base_ptr(const lagfx_task_t *task) {
    if (!task) {
        return NULL;
    }
    return task->reserved_base;
}
