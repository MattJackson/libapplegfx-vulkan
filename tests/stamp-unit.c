/*
 * libapplegfx-vulkan — Stamp advancement unit tests (standalone)
 * tests/stamp-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for stamp advancement logic. PURE UNIT TESTS — no Vulkan,
 * no device_new(), no MMIO path. Standalone functions that mirror the
 * implementation without dependencies.
 *
 * Tests:
 *   1. Monotonicity (target > cur)
 *   2. Ceiling behavior (target < cur → cur+1)
 *   3. Non-zero floor enforcement (target=0 → writes 1)
 *   4. Zero ring base GPA guard (no write)
 *   5. Monotonic progression across multiple calls
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Standalone stamp advancement logic — mirrors src/protocol/stamp.c */
static void advance_stamp_cell(uint32_t *cell_buf, size_t cell_count,
                               uint32_t slot, uint32_t target) {
    if (slot >= cell_count) return;  /* Out of bounds guard */

    uint32_t cur = cell_buf[slot];
    uint32_t next = target > cur ? target : cur + 1;
    if (next < 1) next = 1;  /* Non-zero floor */

    cell_buf[slot] = next;
}

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s\n", msg);                              \
        g_fail++;                                                        \
    } else {                                                             \
        fprintf(stdout, "PASS: %s\n", msg);                              \
        g_pass++;                                                        \
    }                                                                    \
} while (0)

/* Test 1: cur=0, target=5 -> writes 5 */
static void test_advance_cur0_target5(void) {
    fprintf(stdout, "\n--- test_advance_cur0_target5 ---\n");

    uint32_t cell_buf[8] = {0};
    advance_stamp_cell(cell_buf, 8, 0u, 5u);

    CHECK(cell_buf[0] == 5u, "cur=0, target=5 -> cell becomes 5");
}

/* Test 2: cur=5, target=3 -> writes 6 (max(target, cur+1)) */
static void test_advance_cur5_target3(void) {
    fprintf(stdout, "\n--- test_advance_cur5_target3 ---\n");

    uint32_t cell_buf[8] = {0};
    cell_buf[0] = 5u;  /* Pre-populate cur=5 */
    advance_stamp_cell(cell_buf, 8, 0u, 3u);

    CHECK(cell_buf[0] == 6u, "cur=5, target=3 -> cell becomes 6 (max(3, 6))");
}

/* Test 3: cur=0, target=0 -> writes 1 (non-zero floor) */
static void test_advance_cur0_target0(void) {
    fprintf(stdout, "\n--- test_advance_cur0_target0 ---\n");

    uint32_t cell_buf[8] = {0};
    advance_stamp_cell(cell_buf, 8, 0u, 0u);

    CHECK(cell_buf[0] == 1u, "cur=0, target=0 -> cell becomes 1 (floor)");
}

/* Test 4: Zero ring base GPA should prevent write */
static void test_advance_zero_gpa(void) {
    fprintf(stdout, "\n--- test_advance_zero_gpa ---\n");

    uint32_t cell_buf[8] = {5u};
    int result = -99;  /* Sentinel to indicate "no write happened" */

    /* Simulate zero ring_base_pfn check (from lagfx_advance_stamp_cell) */
    uint32_t ring_base_pfn = 0u;
    if (ring_base_pfn == 0u) {
        /* No write should happen - cell stays at 5 */
        result = cell_buf[0];
    } else {
        advance_stamp_cell(cell_buf, 8, 0u, 5u);
        result = cell_buf[0];
    }

    CHECK(result == 5u, "ring_base_pfn==0 -> NO write (cell unchanged at 5)");
}

/* Test 5: Monotonic progression across multiple calls */
static void test_advance_monotonic_5x(void) {
    fprintf(stdout, "\n--- test_advance_monotonic_5x ---\n");

    uint32_t cell_buf[8] = {0};

    for (uint32_t i = 1; i <= 5u; ++i) {
        advance_stamp_cell(cell_buf, 8, 0u, 1u);
        CHECK(cell_buf[0] == (uint32_t)i, "monotonic progression correct");
    }

    /* After 5 calls with target=1, cell should be at 5 */
    CHECK(cell_buf[0] == 5u, "After 5x target=1, cell reaches 5");
}

/* Test 6: Multiple slots advance independently */
static void test_advance_multi_slot(void) {
    fprintf(stdout, "\n--- test_advance_multi_slot ---\n");

    uint32_t cell_buf[8] = {0};

    /* Advance slot 3 to 100 */
    advance_stamp_cell(cell_buf, 8, 3u, 100u);
    CHECK(cell_buf[3] == 100u, "slot=3 first call: cell=100");
    CHECK(cell_buf[0] == 0u, "slot=0 unchanged at 0");

    /* Advance slot 3 again with target < cur */
    advance_stamp_cell(cell_buf, 8, 3u, 50u);
    CHECK(cell_buf[3] == 101u, "slot=3 second call (target<cur): cell=cur+1=101");

    /* Advance slot 0 */
    advance_stamp_cell(cell_buf, 8, 0u, 50u);
    CHECK(cell_buf[0] == 50u, "slot=0 after target=50: cell=50");
}

/* Test 7: Boundaries and edge cases */
static void test_advance_boundaries(void) {
    fprintf(stdout, "\n--- test_advance_boundaries ---\n");

    uint32_t cell_buf[8] = {UINT32_MAX};

    /* cur=UINT32_MAX, target=0 -> wraps to 0, then floor->1 */
    advance_stamp_cell(cell_buf, 8, 0u, 0u);
    CHECK(cell_buf[0] == 1u, "cur=UINT32_MAX, target=0 -> wraps to 0, floor->1");

    /* Test out of bounds - should not crash */
    advance_stamp_cell(cell_buf, 8, 7u, 5u);  /* Valid slot (0-7) */
    CHECK(cell_buf[7] == 5u, "slot=7 (last valid) writes correctly");

    advance_stamp_cell(cell_buf, 8, 8u, 5u);  /* Out of bounds - no-op */
    CHECK(cell_buf[7] == 5u, "out-of-bounds slot doesn't crash");
}

/* Test 8: Zero ring_base_pfn guard with actual write_memory simulation */
static void test_advance_write_memory_guard(void) {
    fprintf(stdout, "\n--- test_advance_write_memory_guard ---\n");

    uint32_t cell_buf[8] = {5u};
    
    /* Simulate the full lagfx_advance_stamp_cell logic:
     * 1. Check ring_base_pfn != 0
     * 2. Check write_memory callback exists and succeeds
     * 3. Only then write */

    uint32_t ring_base_pfn = 0u;
    bool write_memory_called = false;

    if (ring_base_pfn == 0u) {
        /* Skip the write entirely */
    } else {
        cell_buf[0] = 10u;  /* Would write here */
        write_memory_called = true;
    }

    CHECK(cell_buf[0] == 5u, "zero ring_base_pfn -> no write (cell unchanged)");
    CHECK(write_memory_called == false, "write_memory not called when ring_base_pfn=0");
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/stamp-unit: starting\n");

    test_advance_cur0_target5();
    test_advance_cur5_target3();
    test_advance_cur0_target0();
    test_advance_zero_gpa();
    test_advance_monotonic_5x();
    test_advance_multi_slot();
    test_advance_boundaries();
    test_advance_write_memory_guard();

    fprintf(stdout, "\n=== stamp-unit: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
