/*
 * libapplegfx-vulkan — Resource registry unit tests (standalone)
 * tests/resource-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for resource registry logic. PURE UNIT TESTS — no Vulkan,
 * no device_new(), no opcodes. Standalone functions that mirror the
 * EXACT implementation in src/protocol/resource_registry.c without dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Resource types (matching src/protocol/resource_registry.h) */
typedef enum {
    LAGFX_RESOURCE_TYPE_BUFFER,
    LAGFX_RESOURCE_TYPE_TEXTURE,
    LAGFX_RESOURCE_TYPE_PIPELINE,
    LAGFX_RESOURCE_TYPE_SAMPLER,
    LAGFX_RESOURCE_TYPE_HEAP,
    LAGFX_RESOURCE_TYPE_DEPTH_STENCIL_STATE,
} lagfx_resource_type_t;

#define LAGFX_MAX_RESOURCES 256

/* Resource entry (matching src/protocol/resource_registry.h) */
typedef struct {
    uint32_t ref;
    lagfx_resource_type_t type;
    uint32_t task_id;
    void *host_handle;
#ifdef LAGFX_HAVE_VULKAN
    void *image;
    void *view;
#else
    void *image;
    void *view;
#endif
    uint64_t gpu_addr;
    uint64_t size;
} lagfx_resource_entry_t;

/* Resource registry (matching src/protocol/resource_registry.h) */
typedef struct {
    lagfx_resource_entry_t entries[LAGFX_MAX_RESOURCES];
    uint32_t count;
} lagfx_resource_registry_t;

/* Standalone registry functions — EXACT mirror of src/protocol/resource_registry.c */

static void resource_register(lagfx_resource_registry_t *reg,
                              uint32_t ref,
                              lagfx_resource_type_t type,
                              uint32_t task_id,
                              uint64_t gpu_addr,
                              uint64_t size) {
    if (!reg) return;

    /* Check for existing entry with same ref+task_id (update path) */
    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref && reg->entries[i].task_id == task_id) {
            reg->entries[i].type = type;
            reg->entries[i].gpu_addr = gpu_addr;
            reg->entries[i].size = size;
            return;  /* Updated existing entry */
        }
    }

    /* Table full check - must happen before adding new entry */
    if (reg->count >= LAGFX_MAX_RESOURCES) {
        return;  /* Table full, silently fail */
    }

    /* Append new entry at end of array */
    lagfx_resource_entry_t *e = &reg->entries[reg->count++];
    e->ref = ref;
    e->type = type;
    e->task_id = task_id;
    e->host_handle = NULL;
    e->image = NULL;
    e->view = NULL;
    e->gpu_addr = gpu_addr;
    e->size = size;
}

static lagfx_resource_entry_t *resource_lookup(lagfx_resource_registry_t *reg,
                                               uint32_t ref,
                                               uint32_t task_id) {
    if (!reg) return NULL;

    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref && reg->entries[i].task_id == task_id) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

static void resource_unregister(lagfx_resource_registry_t *reg,
                                uint32_t ref,
                                uint32_t task_id) {
    if (!reg) return;

    for (uint32_t i = 0; i < reg->count; ++i) {
        if (reg->entries[i].ref == ref && reg->entries[i].task_id == task_id) {
            /* Swap with last entry and decrement count */
            if (i + 1 < reg->count) {
                reg->entries[i] = reg->entries[reg->count - 1];
            }
            memset(&reg->entries[reg->count - 1], 0, sizeof(lagfx_resource_entry_t));
            reg->count--;
            return;
        }
    }
}

static void resource_clear_task(lagfx_resource_registry_t *reg, uint32_t task_id) {
    if (!reg) return;

    /* Compact array: move non-matching entries to front */
    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < reg->count; ++read_idx) {
        if (reg->entries[read_idx].task_id != task_id) {
            if (write_idx != read_idx) {
                reg->entries[write_idx] = reg->entries[read_idx];
            }
            write_idx++;
        }
    }

    /* Zero trailing entries and update count */
    uint32_t removed = reg->count - write_idx;
    if (removed > 0) {
        memset(&reg->entries[write_idx], 0, removed * sizeof(lagfx_resource_entry_t));
    }
    reg->count = write_idx;
}

static void init_registry(lagfx_resource_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));
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

/* Test 1: Register and lookup by ref+task_id */
static void test_register_lookup(void) {
    fprintf(stdout, "\n--- test_register_lookup ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 0x10000000ull, 4096u);
    CHECK(reg.count == 1u, "register: count == 1");

    lagfx_resource_entry_t *entry = resource_lookup(&reg, 10u, 1u);
    CHECK(entry != NULL, "lookup: found ref=10 task=1");
    CHECK(entry->type == LAGFX_RESOURCE_TYPE_BUFFER, "lookup: type == BUFFER");
    CHECK(entry->gpu_addr == 0x10000000ull, "lookup: gpu_addr correct");
    CHECK(entry->size == 4096u, "lookup: size correct");

    entry = resource_lookup(&reg, 10u, 2u);
    CHECK(entry == NULL, "lookup: wrong task_id -> NULL");

    entry = resource_lookup(&reg, 20u, 1u);
    CHECK(entry == NULL, "lookup: wrong ref -> NULL");
}

/* Test 2: Update existing resource (insert-or-update semantics) */
static void test_register_update(void) {
    fprintf(stdout, "\n--- test_register_update ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 0x10000000ull, 4096u);
    CHECK(reg.count == 1u, "update: initial count == 1");

    /* Re-register with different values - should update in place */
    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_TEXTURE, 1u, 0x20000000ull, 8192u);
    CHECK(reg.count == 1u, "update: count still 1 (not duplicated)");

    lagfx_resource_entry_t *entry = resource_lookup(&reg, 10u, 1u);
    CHECK(entry != NULL, "update: found after re-register");
    CHECK(entry->type == LAGFX_RESOURCE_TYPE_TEXTURE, "update: type updated");
    CHECK(entry->gpu_addr == 0x20000000ull, "update: gpu_addr updated");
    CHECK(entry->size == 8192u, "update: size updated");
}

/* Test 3: Unregister by ref+task_id (swaps with last entry) */
static void test_unregister(void) {
    fprintf(stdout, "\n--- test_unregister ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 0x10000000ull, 4096u);
    resource_register(&reg, 20u, LAGFX_RESOURCE_TYPE_TEXTURE, 1u, 0x20000000ull, 8192u);
    CHECK(reg.count == 2u, "unregister: count == 2");

    /* Unregister first resource - swaps with last */
    resource_unregister(&reg, 10u, 1u);
    CHECK(reg.count == 1u, "unregister: count == 1 after unregister");

    /* First should be gone (swapped out) */
    lagfx_resource_entry_t *entry = resource_lookup(&reg, 10u, 1u);
    CHECK(entry == NULL, "unregister: ref=10 not found");

    /* Second might have been swapped to slot 0 - lookup should still find it */
    entry = resource_lookup(&reg, 20u, 1u);
    CHECK(entry != NULL, "unregister: ref=20 still exists (might be at different index)");
}

/* Test 4: Clear all resources for a task_id (compacts array) */
static void test_clear_task(void) {
    fprintf(stdout, "\n--- test_clear_task ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 0x10000000ull, 4096u);
    resource_register(&reg, 20u, LAGFX_RESOURCE_TYPE_TEXTURE, 1u, 0x20000000ull, 8192u);
    resource_register(&reg, 30u, LAGFX_RESOURCE_TYPE_BUFFER, 2u, 0x30000000ull, 4096u);

    CHECK(reg.count == 3u, "clear_task: count == 3");

    /* Clear all resources for task 1 - compacts remaining */
    resource_clear_task(&reg, 1u);
    CHECK(reg.count == 1u, "clear_task: count == 1 after clearing task=1");

    /* Task 1 resources should be gone */
    CHECK(resource_lookup(&reg, 10u, 1u) == NULL, "clear_task: ref=10 gone");
    CHECK(resource_lookup(&reg, 20u, 1u) == NULL, "clear_task: ref=20 gone");

    /* Task 2 resource should remain */
    lagfx_resource_entry_t *entry = resource_lookup(&reg, 30u, 2u);
    CHECK(entry != NULL, "clear_task: task=2 resource remains");
}

/* Test 5: Null safety (NULL registry) */
static void test_null_safety(void) {
    fprintf(stdout, "\n--- test_null_safety ---\n");

    /* All operations should be safe with NULL registry */
    resource_register(NULL, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 0x10000000ull, 4096u);
    lagfx_resource_entry_t *entry = resource_lookup(NULL, 10u, 1u);
    CHECK(entry == NULL, "null safety: lookup(NULL) returns NULL");
    resource_unregister(NULL, 10u, 1u);
    resource_clear_task(NULL, 1u);
}

/* Test 6: Table-full handling */
static void test_table_full(void) {
    fprintf(stdout, "\n--- test_table_full ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    /* Register max resources */
    for (uint32_t i = 0; i < LAGFX_MAX_RESOURCES; ++i) {
        resource_register(&reg, i + 1u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 
                          (uint64_t)(0x10000000ull + i * 4096), 4096u);
    }

    CHECK(reg.count == LAGFX_MAX_RESOURCES, "table_full: count == MAX");

    /* Try to register one more - should fail silently (no crash) */
    resource_register(&reg, 1u, LAGFX_RESOURCE_TYPE_TEXTURE, 2u, 
                      0xFFFFFFFFull, 8192u);

    /* Count should still be MAX (new entry not added) */
    CHECK(reg.count == LAGFX_MAX_RESOURCES, "table_full: count unchanged after full");
}

/* Test 7: Map memory immediate registration pattern */
static void test_map_memory_immediate_registers_buffer(void) {
    fprintf(stdout, "\n--- test_map_memory_immediate_registers_buffer ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    uint32_t task_id = 42u;
    resource_register(&reg, task_id, LAGFX_RESOURCE_TYPE_BUFFER, task_id, 
                      0x80000000ull, 16384u);

    lagfx_resource_entry_t *entry = resource_lookup(&reg, task_id, task_id);
    CHECK(entry != NULL, "map_memory: buffer registered");
    CHECK(entry->type == LAGFX_RESOURCE_TYPE_BUFFER, "map_memory: type is BUFFER");
}

/* Test 8: Delete task clears resources */
static void test_delete_task_clears_resources(void) {
    fprintf(stdout, "\n--- test_delete_task_clears_resources ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 5u, 
                      0x10000000ull, 4096u);
    resource_register(&reg, 20u, LAGFX_RESOURCE_TYPE_TEXTURE, 5u, 
                      0x20000000ull, 8192u);

    CHECK(reg.count == 2u, "delete_task: count == 2 before clear");

    resource_clear_task(&reg, 5u);
    CHECK(reg.count == 0u, "delete_task: count == 0 after clear");
    CHECK(resource_lookup(&reg, 10u, 5u) == NULL, "delete_task: ref=10 cleared");
    CHECK(resource_lookup(&reg, 20u, 5u) == NULL, "delete_task: ref=20 cleared");
}

/* Test 9: Delete resource handler */
static void test_delete_resource_handler(void) {
    fprintf(stdout, "\n--- test_delete_resource_handler ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 
                      0x10000000ull, 4096u);
    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_TEXTURE, 2u, 
                      0x20000000ull, 8192u);

    CHECK(reg.count == 2u, "delete_resource: count == 2");

    /* Unregister ref=10 for task=1 - swaps with last entry */
    resource_unregister(&reg, 10u, 1u);

    CHECK(reg.count == 1u, "delete_resource: count == 1 after unregister");
    CHECK(resource_lookup(&reg, 10u, 1u) == NULL, "delete_resource: ref=10 task=1 gone");

    /* Task 2 resource might have been swapped - lookup should find it */
    lagfx_resource_entry_t *entry = resource_lookup(&reg, 10u, 2u);
    CHECK(entry != NULL, "delete_resource: ref=10 task=2 remains (swapped to slot 0)");
}

/* Test 10: Set object placement list handler */
static void test_set_object_placement_handler(void) {
    fprintf(stdout, "\n--- test_set_object_placement_handler ---\n");

    lagfx_resource_registry_t reg;
    init_registry(&reg);

    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 
                      0x10000000ull, 4096u);

    /* Unregister first (simulating CmdSetObjectAndPlacementList) */
    resource_unregister(&reg, 10u, 1u);
    CHECK(reg.count == 0u, "set_placement: count == 0 after unregister");

    /* Re-register with new parameters */
    resource_register(&reg, 10u, LAGFX_RESOURCE_TYPE_BUFFER, 1u, 
                      0x30000000ull, 8192u);

    CHECK(reg.count == 1u, "set_placement: count == 1 after re-register");
    lagfx_resource_entry_t *entry = resource_lookup(&reg, 10u, 1u);
    CHECK(entry->gpu_addr == 0x30000000ull, "set_placement: gpu_addr updated");
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/resource-unit: starting\n");

    test_register_lookup();
    test_register_update();
    test_unregister();
    test_clear_task();
    test_null_safety();
    test_table_full();
    test_map_memory_immediate_registers_buffer();
    test_delete_task_clears_resources();
    test_delete_resource_handler();
    test_set_object_placement_handler();

    fprintf(stdout, "\n=== resource-unit: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
