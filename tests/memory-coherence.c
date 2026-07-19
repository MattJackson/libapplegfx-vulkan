/*
 * libapplegfx-vulkan — memory coherence test
 * tests/memory-coherence.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Proves that lagfx_task_map_host_memory aliases the host pointer's
 * pages into the task VA range, rather than copying. Under the
 * aliasing contract (see docs/memory-coherence-audit.md):
 *
 *   - Writes by the "guest" (simulated here as writes through the
 *     original host_ptr after the map call) are immediately visible
 *     in the task VA range.
 *   - Writes by the "host" (via the task VA range) are immediately
 *     visible through the original host_ptr.
 *
 * Under the copy-on-map (incorrect) implementation, both assertions
 * fail: the task VA holds a stale snapshot taken at map time.
 *
 * Platform policy (see tests/meson.build):
 *   - Linux: test() registered; MUST PASS post-fix.
 *   - Darwin / non-Linux: built for syntax checking only (not run).
 *     The library's non-Linux path retains copy-on-map by design
 *     because Darwin is dev-only; running this test there would
 *     correctly fail and make CI noisy.
 *
 * Runtime SKIP policy:
 *   - If memfd_create is unavailable at runtime (ancient kernel), the
 *     test exits 77 (meson's SKIP convention) rather than FAIL.
 *   - If mmap(MAP_SHARED, memfd) at the source — required to match
 *     QEMU's RAMBlock backing semantics — fails, same SKIP path.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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

#include "libapplegfx-vulkan.h"

#define SKIP_EXIT_CODE 77  /* meson convention for skipped tests */

/* Page size used for the simulated "RAMBlock". Must be a multiple of
 * the host page size; 16 KB covers x86_64 (4 KB) and arm64 (16 KB). */
#define PAGE_SIZE_LOCAL 16384

/* Minimal memfd_create wrapper — identical spirit to the one in
 * src/memory/task.c but local to this test to keep the test
 * independent of library internals. */
static int tc_memfd_create(const char *name, unsigned int flags) {
#ifdef __linux__
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, flags);
#elif defined(__x86_64__)
    return (int)syscall(319, name, flags);
#elif defined(__aarch64__)
    return (int)syscall(279, name, flags);
#else
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
#else
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan memory coherence test ===\n");

    /* Step 1: create a MAP_SHARED memfd-backed "host RAM" region.
     * This mimics what QEMU hands us via memory_region_get_ram_ptr
     * when guest RAM is backed by memory-backend-memfd (share=on). */
    const size_t region_size = PAGE_SIZE_LOCAL * 2;

    int memfd = tc_memfd_create("lagfx-coherence-test", 0);
    if (memfd < 0) {
        fprintf(stdout,
                "SKIP: memfd_create unavailable (%s). Coherence test"
                " requires a memfd-like MAP_SHARED source.\n",
                strerror(errno));
        return SKIP_EXIT_CODE;
    }

    if (ftruncate(memfd, (off_t)region_size) < 0) {
        fprintf(stdout, "SKIP: ftruncate failed: %s\n", strerror(errno));
        close(memfd);
        return SKIP_EXIT_CODE;
    }

    void *host_ptr = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, memfd, 0);
    if (host_ptr == MAP_FAILED) {
        fprintf(stdout, "SKIP: mmap(MAP_SHARED, memfd) failed: %s\n",
                strerror(errno));
        close(memfd);
        return SKIP_EXIT_CODE;
    }

    /* Seed the region with sentinel A (0xAA). */
    memset(host_ptr, 0xAA, region_size);

    /* Step 2: create a task and map the host region into it. */
    void *task_base = NULL;
    lagfx_task_t *task = lagfx_task_create(region_size, &task_base);
    if (!task) {
        fprintf(stderr, "FAIL: lagfx_task_create returned NULL\n");
        munmap(host_ptr, region_size);
        close(memfd);
        return 1;
    }

    if (!lagfx_task_map_host_memory(task, 0, host_ptr, region_size,
                                     false)) {
        fprintf(stderr,
                "FAIL: lagfx_task_map_host_memory returned false\n");
        lagfx_task_destroy(task);
        munmap(host_ptr, region_size);
        close(memfd);
        return 1;
    }

    volatile uint8_t *task_view = (volatile uint8_t *)task_base;
    volatile uint8_t *host_view = (volatile uint8_t *)host_ptr;

    int failures = 0;

    /* Assertion 1: post-map, the task view reflects the seeded data.
     * Every path (copy or alias) must satisfy this. */
    if (task_view[0] != 0xAA || task_view[region_size - 1] != 0xAA) {
        fprintf(stderr,
                "FAIL: post-map task view does not reflect seeded"
                " content (task_view[0]=0x%02x,"
                " task_view[end]=0x%02x, expected 0xAA)\n",
                task_view[0], task_view[region_size - 1]);
        failures++;
    } else {
        fprintf(stdout,
                "PASS: post-map task view reflects seeded content\n");
    }

    /* Assertion 2: LOAD-BEARING. Guest writes AFTER map must be
     * visible in the task VA. Under copy-on-map this fails — the
     * memcpy snapshot was taken at map time. */
    memset(host_ptr, 0xBB, region_size);
    __sync_synchronize();  /* total store order between the two views */

    if (task_view[0] != 0xBB || task_view[region_size - 1] != 0xBB) {
        fprintf(stderr,
                "FAIL: COHERENCE BUG — host_ptr write did not"
                " propagate to task view"
                " (task_view[0]=0x%02x, expected 0xBB). This is"
                " the copy-on-map signature; see"
                " docs/memory-coherence-audit.md.\n",
                task_view[0]);
        failures++;
    } else {
        fprintf(stdout,
                "PASS: host_ptr -> task_view coherence (guest"
                " write visible at task VA)\n");
    }

    /* Assertion 3: bidirectional. Writes from the task VA must be
     * visible through host_ptr. */
    memset((void *)task_base, 0xCC, region_size);
    __sync_synchronize();

    if (host_view[0] != 0xCC || host_view[region_size - 1] != 0xCC) {
        fprintf(stderr,
                "FAIL: COHERENCE BUG — task view write did not"
                " propagate to host_ptr"
                " (host_view[0]=0x%02x, expected 0xCC).\n",
                host_view[0]);
        failures++;
    } else {
        fprintf(stdout,
                "PASS: task_view -> host_ptr coherence (host"
                " write visible at guest-facing pointer)\n");
    }

    /* Assertion 4: unmap still works and leaves the task in a clean
     * state (regression safety — fix must not break the unmap path). */
    if (!lagfx_task_unmap(task, 0, region_size)) {
        fprintf(stderr, "FAIL: lagfx_task_unmap returned false\n");
        failures++;
    } else {
        fprintf(stdout, "PASS: lagfx_task_unmap after aliasing\n");
    }

    /* Cleanup. */
    lagfx_task_destroy(task);
    munmap(host_ptr, region_size);
    close(memfd);

    fprintf(stdout, "\n=== Summary ===\n");
    fprintf(stdout, "Failures: %d\n", failures);

    return failures > 0 ? 1 : 0;
}
