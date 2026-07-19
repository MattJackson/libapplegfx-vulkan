/*
 * libapplegfx-vulkan — memory task tests
 * tests/memory-task.c — Unity-style tests for task.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Compile: gcc -o memory-task memory-task.c ../src/memory/task.c -std=c11
 * Run: ./memory-task
 *
 * Tests exercise lagfx_task_create, map_host_memory, unmap, and destroy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Include the task header. Path is relative to tests/ directory. */
#include "../src/memory/task.h"

/* Test counters. */
static int g_tests_passed = 0;
static int g_tests_failed = 0;

/* Macro for simple assertions. */
#define TEST_ASSERT(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stdout, "FAIL: " fmt "\n", ##__VA_ARGS__); \
            g_tests_failed++; \
        } else { \
            g_tests_passed++; \
        } \
    } while (0)

/* Test 1: Basic task creation and destruction. */
static void test_create_destroy(void) {
    fprintf(stdout, "\n--- Test 1: create_destroy ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(4096, &base);

    TEST_ASSERT(task != NULL, "Task creation should succeed");
    TEST_ASSERT(base != NULL, "Base address should be non-NULL");

    /* Try to read/write; should crash (PROT_NONE). */
    /* Skipping intentional SIGSEGV for safety in test harness. */

    lagfx_task_destroy(task);
    fprintf(stdout, "Test 1 complete\n");
}

/* Test 2: Map memory at offset 0. */
static void test_map_at_zero(void) {
    fprintf(stdout, "\n--- Test 2: map_at_zero ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(8192, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    /* Prepare host memory to map. */
    void *host_mem = malloc(1024);
    TEST_ASSERT(host_mem != NULL, "Host memory allocation");

    memset(host_mem, 0x42, 1024);

    bool ok = lagfx_task_map_host_memory(task, 0, host_mem, 1024, false);
    TEST_ASSERT(ok, "Map at offset 0");

    /* Verify the mapped range is readable. */
    uint8_t *mapped = (uint8_t *)base;
    uint8_t first_byte = mapped[0];
    TEST_ASSERT(first_byte == 0x42, "Mapped memory contains expected value");

    free(host_mem);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 2 complete\n");
}

/* Test 3: Map memory at non-zero offset. */
static void test_map_at_offset(void) {
    fprintf(stdout, "\n--- Test 3: map_at_offset ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(16384, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    void *host_mem = malloc(512);
    memset(host_mem, 0xAB, 512);

    /* Map at offset 4096 (page-aligned). */
    bool ok = lagfx_task_map_host_memory(task, 4096, host_mem, 512, false);
    TEST_ASSERT(ok, "Map at offset 4096");

    uint8_t *mapped = (uint8_t *)base + 4096;
    uint8_t byte_at_offset = mapped[0];
    TEST_ASSERT(byte_at_offset == 0xAB,
                "Mapped memory at offset contains expected value");

    free(host_mem);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 3 complete\n");
}

/* Test 4: Read-only mapping. */
static void test_map_readonly(void) {
    fprintf(stdout, "\n--- Test 4: map_readonly ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(8192, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    void *host_mem = malloc(256);
    memset(host_mem, 0xCD, 256);

    bool ok = lagfx_task_map_host_memory(task, 0, host_mem, 256, true);
    TEST_ASSERT(ok, "Map as read-only");

    /* Read should work. */
    uint8_t *mapped = (uint8_t *)base;
    uint8_t byte = mapped[0];
    TEST_ASSERT(byte == 0xCD, "Read-only mapping is readable");

    /* Write would fail (SIGSEGV); skipping intentional crash. */

    free(host_mem);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 4 complete\n");
}

/* Test 5: Unmap range. */
static void test_unmap(void) {
    fprintf(stdout, "\n--- Test 5: unmap ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(8192, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    void *host_mem = malloc(1024);
    memset(host_mem, 0x55, 1024);

    bool ok = lagfx_task_map_host_memory(task, 0, host_mem, 1024, false);
    TEST_ASSERT(ok, "Initial map");

    /* Verify mapped. */
    uint8_t *mapped = (uint8_t *)base;
    TEST_ASSERT(mapped[0] == 0x55, "Mapped before unmap");

    /* Unmap. */
    ok = lagfx_task_unmap(task, 0, 1024);
    TEST_ASSERT(ok, "Unmap succeeds");

    /* Reading unmapped range would SIGSEGV; skipping. */

    free(host_mem);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 5 complete\n");
}

/* Test 6: Multiple disjoint mappings. */
static void test_multiple_mappings(void) {
    fprintf(stdout, "\n--- Test 6: multiple_mappings ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(16384, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    void *host_mem1 = malloc(512);
    void *host_mem2 = malloc(512);
    memset(host_mem1, 0x11, 512);
    memset(host_mem2, 0x22, 512);

    /* Map first range at offset 0. */
    bool ok1 = lagfx_task_map_host_memory(task, 0, host_mem1, 512, false);
    TEST_ASSERT(ok1, "First mapping");

    /* Map second range at offset 4096. */
    bool ok2 = lagfx_task_map_host_memory(task, 4096, host_mem2, 512, false);
    TEST_ASSERT(ok2, "Second mapping");

    /* Verify both. */
    uint8_t *mapped = (uint8_t *)base;
    TEST_ASSERT(mapped[0] == 0x11, "First range content");
    TEST_ASSERT(mapped[4096] == 0x22, "Second range content");

    free(host_mem1);
    free(host_mem2);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 6 complete\n");
}

/* Test 7: Invalid arguments. */
static void test_invalid_args(void) {
    fprintf(stdout, "\n--- Test 7: invalid_args ---\n");

    void *base = NULL;
    lagfx_task_t *task = lagfx_task_create(4096, &base);
    TEST_ASSERT(task != NULL, "Task creation");

    /* Mapping beyond task bounds should fail. */
    void *host_mem = malloc(256);
    bool ok = lagfx_task_map_host_memory(task, 4000, host_mem, 1024, false);
    TEST_ASSERT(!ok, "Map beyond bounds fails");

    /* Zero-length mapping should fail. */
    ok = lagfx_task_map_host_memory(task, 0, host_mem, 0, false);
    TEST_ASSERT(!ok, "Zero-length mapping fails");

    /* Unmap beyond bounds should fail. */
    ok = lagfx_task_unmap(task, 4000, 1024);
    TEST_ASSERT(!ok, "Unmap beyond bounds fails");

    free(host_mem);
    lagfx_task_destroy(task);
    fprintf(stdout, "Test 7 complete\n");
}

/* Test 8: Large task. */
static void test_large_task(void) {
    fprintf(stdout, "\n--- Test 8: large_task ---\n");

    void *base = NULL;
    size_t large_size = 16 * 1024 * 1024;  /* 16 MB */
    lagfx_task_t *task = lagfx_task_create(large_size, &base);
    TEST_ASSERT(task != NULL, "Large task creation (16 MB)");

    if (task) {
        lagfx_task_destroy(task);
    }
    fprintf(stdout, "Test 8 complete\n");
}

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan task memory tests ===\n");

    test_create_destroy();
    test_map_at_zero();
    test_map_at_offset();
    test_map_readonly();
    test_unmap();
    test_multiple_mappings();
    test_invalid_args();
    test_large_task();

    fprintf(stdout, "\n=== Summary ===\n");
    fprintf(stdout, "Passed: %d\n", g_tests_passed);
    fprintf(stdout, "Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
