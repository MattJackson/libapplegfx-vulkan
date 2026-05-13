/*
 * libapplegfx-vulkan — gpu_cores / thread_count plumbing test
 * tests/gpu-cores.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Verifies the descriptor field `thread_count` is translated into the
 * process env var `LP_NUM_THREADS` by lagfx_device_new, per the spec
 * at paravirt-re/gpu-cores-implementation-spec.md. Covers:
 *   - thread_count = N (>0)  -> LP_NUM_THREADS == "N"
 *   - thread_count = 0       -> LP_NUM_THREADS unchanged (unset path)
 *   - thread_count = 1       -> works (single-threaded lavapipe)
 *
 * NOTE: Mesa only READS LP_NUM_THREADS at ICD init time, so this test
 * exercises the plumbing from descriptor to env var, not the ICD
 * behavior downstream. For the latter, see LP_DEBUG=perf.
 */

#include "libapplegfx-vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
    } \
} while (0)

/* Minimal no-op shell callbacks. */
static lagfx_task_t *cb_create_task(void *o, uint64_t sz, void **out) {
    (void)o; (void)sz; (void)out; return NULL;
}
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)a; (void)t; (void)o; (void)l; return true;
}
static bool cb_read(void *a, uint64_t gpa, uint64_t l, void *d) {
    (void)a; (void)gpa; (void)l; (void)d; return true;
}
static void cb_irq(void *a, uint32_t v) { (void)a; (void)v; }

static void init_desc(lagfx_device_descriptor_t *desc) {
    memset(desc, 0, sizeof(*desc));
    desc->shell.opaque          = (void *)0xdeadbeefu;
    desc->shell.create_task     = cb_create_task;
    desc->shell.destroy_task    = cb_destroy_task;
    desc->shell.map_memory      = cb_map;
    desc->shell.unmap_memory    = cb_unmap;
    desc->shell.read_memory     = cb_read;
    desc->shell.raise_interrupt = cb_irq;
}

static int test_thread_count_sets_env(uint32_t n, const char *expected) {
    lagfx_device_descriptor_t desc;
    init_desc(&desc);
    desc.thread_count = n;

    /* Reset env var so we observe only this device's effect. */
    unsetenv("LP_NUM_THREADS");

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&desc, &err);
    CHECK(dev != NULL, "device_new with non-zero thread_count succeeds");
    CHECK(err == NULL, "no error on success");

    const char *v = getenv("LP_NUM_THREADS");
    CHECK(v != NULL, "LP_NUM_THREADS is set after device_new(thread_count>0)");
    if (v) {
        if (strcmp(v, expected) == 0) {
            fprintf(stdout, "PASS: LP_NUM_THREADS == \"%s\"\n", expected);
        } else {
            fprintf(stderr,
                    "FAIL: LP_NUM_THREADS is \"%s\", expected \"%s\"\n",
                    v, expected);
            g_fail++;
        }
    }

    lagfx_device_free(dev);
    unsetenv("LP_NUM_THREADS");
    return 0;
}

static int test_thread_count_zero_preserves_env(void) {
    lagfx_device_descriptor_t desc;
    init_desc(&desc);
    desc.thread_count = 0;  /* unset sentinel */

    /* Pre-seed an existing value; a zero thread_count must not clobber it. */
    setenv("LP_NUM_THREADS", "preset-42", 1);

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&desc, &err);
    CHECK(dev != NULL, "device_new with thread_count==0 succeeds");
    CHECK(err == NULL, "no error on success");

    const char *v = getenv("LP_NUM_THREADS");
    CHECK(v != NULL && strcmp(v, "preset-42") == 0,
          "thread_count==0 leaves existing LP_NUM_THREADS untouched");

    lagfx_device_free(dev);
    unsetenv("LP_NUM_THREADS");
    return 0;
}

static int test_thread_count_one(void) {
    /* Spec edge case: gpu_cores=1 must work (single-threaded lavapipe). */
    lagfx_device_descriptor_t desc;
    init_desc(&desc);
    desc.thread_count = 1;

    unsetenv("LP_NUM_THREADS");

    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&desc, &err);
    CHECK(dev != NULL, "device_new with thread_count==1 succeeds");

    const char *v = getenv("LP_NUM_THREADS");
    CHECK(v != NULL && strcmp(v, "1") == 0, "LP_NUM_THREADS == \"1\"");

    lagfx_device_free(dev);
    unsetenv("LP_NUM_THREADS");
    return 0;
}

int main(void) {
#ifndef __linux__
    fprintf(stderr, "gpu cores requires Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    fprintf(stdout, "=== libapplegfx-vulkan gpu_cores plumbing ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    test_thread_count_sets_env(4, "4");
    test_thread_count_sets_env(32, "32");
    test_thread_count_zero_preserves_env();
    test_thread_count_one();

    fprintf(stdout, "\n=== Summary: %s ===\n",
            g_fail ? "FAILURES" : "ALL GOOD");
    return g_fail ? 1 : 0;
}
