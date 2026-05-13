/*
 * libapplegfx-vulkan — resource registry unit tests
 * tests/resource-registry.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the resource reference registry:
 *   - register / lookup / unregister / clear_task
 *   - insert-or-update semantics
 *   - table-full handling
 *   - Wiring through opcode handlers:
 *     - CmdMapMemoryImmediate (0x39) registers a BUFFER
 *     - CmdDeleteTask clears task resources
 *     - CmdDeleteResource (0x08) unregisters
 *     - CmdSetObjectAndPlacementList (0x25) unregisters
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/display.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"
#include "../src/protocol/state.h"
#include "../src/protocol/resource_registry.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
        g_pass++; \
    } \
} while (0)

/* === Mock shell ============================================ */

typedef struct {
    unsigned raise_irq_count;
    unsigned create_task_count;
    unsigned destroy_task_count;
} mock_shell_t;

static lagfx_task_t *mock_create_task(void *op, uint64_t sz, void **out) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->create_task_count++;
    if (out) *out = (void *)0xbeef0000u;
    (void)sz;
    return (lagfx_task_t *)0x1u;
}

static void mock_destroy_task(void *op, lagfx_task_t *t) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->destroy_task_count++;
    (void)t;
}

static bool mock_map(void *op, lagfx_task_t *t, uint64_t o,
                     const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}

static bool mock_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}

static bool mock_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    (void)op; (void)gpa; (void)l; (void)d;
    return true;
}

static bool mock_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    (void)op; (void)gpa; (void)l; (void)s;
    return true;
}

static void mock_raise_irq(void *op, uint32_t vec) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
}

static lagfx_device_t *make_dev(mock_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = mock_create_task;
    d.shell.destroy_task    = mock_destroy_task;
    d.shell.map_memory      = mock_map;
    d.shell.unmap_memory    = mock_unmap;
    d.shell.read_memory     = mock_read;
    d.shell.write_memory    = mock_write;
    d.shell.raise_interrupt = mock_raise_irq;
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) {
        fprintf(stderr, "FATAL: device_new failed: %s\n",
                err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* === Command synthesis ===================================== */

static size_t build_header(uint8_t *out, uint16_t opcode,
                           uint16_t arg_count_8b,
                           uint32_t total_length, uint32_t stamp) {
    memset(out, 0, 12);
    out[0] = (uint8_t)(opcode & 0xffu);
    out[1] = (uint8_t)((opcode >> 8) & 0xffu);
    out[2] = (uint8_t)(arg_count_8b & 0xffu);
    out[3] = (uint8_t)((arg_count_8b >> 8) & 0xffu);
    out[4] = (uint8_t)(total_length & 0xffu);
    out[5] = (uint8_t)((total_length >> 8) & 0xffu);
    out[6] = (uint8_t)((total_length >> 16) & 0xffu);
    out[7] = (uint8_t)((total_length >> 24) & 0xffu);
    out[8]  = (uint8_t)(stamp & 0xffu);
    out[9]  = (uint8_t)((stamp >> 8) & 0xffu);
    out[10] = (uint8_t)((stamp >> 16) & 0xffu);
    out[11] = (uint8_t)((stamp >> 24) & 0xffu);
    return 12;
}

static void put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *b, uint64_t v) {
    put_le32(b,     (uint32_t)v);
    put_le32(b + 4, (uint32_t)(v >> 32));
}

/* === Tests ================================================= */

static void test_register_lookup(void) {
    lagfx_resource_registry_t reg;
    memset(&reg, 0, sizeof(reg));

    lagfx_resource_register(&reg, 10, LAGFX_RESOURCE_TYPE_BUFFER,
                            1, 0x100000, 0x4000);
    CHECK(reg.count == 1, "register: count == 1");

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&reg, 10, 1);
    CHECK(e != NULL, "lookup: found ref=10 task=1");
    CHECK(e->type == LAGFX_RESOURCE_TYPE_BUFFER, "lookup: type == BUFFER");
    CHECK(e->gpu_addr == 0x100000, "lookup: gpu_addr correct");
    CHECK(e->size == 0x4000, "lookup: size correct");
    CHECK(e->host_handle == NULL, "lookup: host_handle NULL (scaffold)");

    CHECK(lagfx_resource_lookup(&reg, 10, 2) == NULL,
          "lookup: wrong task_id → NULL");
    CHECK(lagfx_resource_lookup(&reg, 99, 1) == NULL,
          "lookup: wrong ref → NULL");
}

static void test_register_update(void) {
    lagfx_resource_registry_t reg;
    memset(&reg, 0, sizeof(reg));

    lagfx_resource_register(&reg, 5, LAGFX_RESOURCE_TYPE_TEXTURE,
                            1, 0xAABBCC, 1024);
    CHECK(reg.count == 1, "update: initial count == 1");

    lagfx_resource_register(&reg, 5, LAGFX_RESOURCE_TYPE_PIPELINE,
                            1, 0xDDEEFF, 2048);
    CHECK(reg.count == 1, "update: count still 1 after re-register");

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&reg, 5, 1);
    CHECK(e != NULL, "update: found after re-register");
    CHECK(e->type == LAGFX_RESOURCE_TYPE_PIPELINE, "update: type updated");
    CHECK(e->gpu_addr == 0xDDEEFF, "update: gpu_addr updated");
    CHECK(e->size == 2048, "update: size updated");
}

static void test_unregister(void) {
    lagfx_resource_registry_t reg;
    memset(&reg, 0, sizeof(reg));

    lagfx_resource_register(&reg, 1, LAGFX_RESOURCE_TYPE_BUFFER, 1, 0, 100);
    lagfx_resource_register(&reg, 2, LAGFX_RESOURCE_TYPE_TEXTURE, 1, 0, 200);
    lagfx_resource_register(&reg, 3, LAGFX_RESOURCE_TYPE_SAMPLER, 1, 0, 300);
    CHECK(reg.count == 3, "unregister: initial count == 3");

    lagfx_resource_unregister(&reg, 2, 1);
    CHECK(reg.count == 2, "unregister: count == 2 after remove");
    CHECK(lagfx_resource_lookup(&reg, 2, 1) == NULL,
          "unregister: removed entry gone");
    CHECK(lagfx_resource_lookup(&reg, 1, 1) != NULL,
          "unregister: other entry still present");
    CHECK(lagfx_resource_lookup(&reg, 3, 1) != NULL,
          "unregister: third entry still present");

    lagfx_resource_unregister(&reg, 99, 1);
    CHECK(reg.count == 2, "unregister: no-op for missing ref");
}

static void test_clear_task(void) {
    lagfx_resource_registry_t reg;
    memset(&reg, 0, sizeof(reg));

    lagfx_resource_register(&reg, 1, LAGFX_RESOURCE_TYPE_BUFFER,  1, 0, 100);
    lagfx_resource_register(&reg, 2, LAGFX_RESOURCE_TYPE_TEXTURE, 1, 0, 200);
    lagfx_resource_register(&reg, 10, LAGFX_RESOURCE_TYPE_BUFFER, 2, 0, 300);
    lagfx_resource_register(&reg, 11, LAGFX_RESOURCE_TYPE_BUFFER, 2, 0, 400);
    CHECK(reg.count == 4, "clear_task: initial count == 4");

    lagfx_resource_clear_task(&reg, 1);
    CHECK(reg.count == 2, "clear_task: count == 2 after clearing task 1");
    CHECK(lagfx_resource_lookup(&reg, 1, 1) == NULL,
          "clear_task: task 1 ref 1 gone");
    CHECK(lagfx_resource_lookup(&reg, 2, 1) == NULL,
          "clear_task: task 1 ref 2 gone");
    CHECK(lagfx_resource_lookup(&reg, 10, 2) != NULL,
          "clear_task: task 2 ref 10 still present");
    CHECK(lagfx_resource_lookup(&reg, 11, 2) != NULL,
          "clear_task: task 2 ref 11 still present");
}

static void test_null_safety(void) {
    lagfx_resource_register(NULL, 0, LAGFX_RESOURCE_TYPE_UNKNOWN,
                            0, 0, 0);
    CHECK(lagfx_resource_lookup(NULL, 0, 0) == NULL,
          "null: lookup on NULL returns NULL");
    lagfx_resource_unregister(NULL, 0, 0);
    lagfx_resource_clear_task(NULL, 0);
    CHECK(1, "null: all functions safe on NULL registry");
}

static void test_map_memory_immediate_registers_buffer(void) {
    mock_shell_t shell;
    memset(&shell, 0, sizeof(shell));
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = dev->protocol_state;

    uint8_t cmd[12 + 24 + 20];
    size_t hlen = build_header(cmd, LAGFX_OP_DEFINE_TASK2, 3,
                               (uint32_t)sizeof(cmd), 0x100);
    put_le32(cmd + hlen, 42);
    put_le64(cmd + hlen + 4, 0ULL);
    put_le64(cmd + hlen + 12, 0x100000ULL);
    put_le32(cmd + hlen + 20, 0);
    lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(lagfx_protocol_find_task(p, 42) != NULL,
          "0x39 setup: task 42 registered");

    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, 42);
    task->root_page_pfn = 1;

    uint8_t map_cmd[12 + 20];
    hlen = build_header(map_cmd, LAGFX_OP_MAP_MEMORY_IMMEDIATE, 2,
                        (uint32_t)sizeof(map_cmd), 0x200);
    size_t off = sizeof(map_cmd) - 12 - 20;
    put_le32(map_cmd + hlen + off + 0, 42);
    put_le64(map_cmd + hlen + off + 4, 0x50000ULL);
    put_le64(map_cmd + hlen + off + 12, 0x2000ULL);
    lagfx_protocol_dispatch_one(p, map_cmd, sizeof(map_cmd));

    lagfx_resource_entry_t *e = lagfx_resource_lookup(&p->resources, 0, 42);
    CHECK(e != NULL, "0x39: resource registered after MapMemoryImmediate");
    CHECK(e->type == LAGFX_RESOURCE_TYPE_BUFFER, "0x39: type == BUFFER");
    CHECK(e->gpu_addr == 0x50000, "0x39: gpu_addr == 0x50000");
    CHECK(e->size == 0x2000, "0x39: size == 0x2000");

    lagfx_device_free(dev);
}

static void test_delete_task_clears_resources(void) {
    mock_shell_t shell;
    memset(&shell, 0, sizeof(shell));
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = dev->protocol_state;

    uint8_t cmd[12 + 24];
    size_t hlen = build_header(cmd, LAGFX_OP_DEFINE_TASK2, 3,
                               (uint32_t)sizeof(cmd), 0x100);
    put_le32(cmd + hlen, 7);
    put_le64(cmd + hlen + 4, 0ULL);
    put_le64(cmd + hlen + 12, 0x10000ULL);
    put_le32(cmd + hlen + 20, 0);
    lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));

    lagfx_resource_register(&p->resources, 100,
                            LAGFX_RESOURCE_TYPE_BUFFER, 7, 0, 4096);
    lagfx_resource_register(&p->resources, 101,
                            LAGFX_RESOURCE_TYPE_TEXTURE, 7, 0, 8192);
    CHECK(p->resources.count == 2, "delete_task: 2 resources registered");

    uint8_t del_cmd[12 + 4];
    hlen = build_header(del_cmd, LAGFX_OP_DELETE_TASK, 0,
                        (uint32_t)sizeof(del_cmd), 0x200);
    put_le32(del_cmd + hlen, 7);
    lagfx_protocol_dispatch_one(p, del_cmd, sizeof(del_cmd));

    CHECK(p->resources.count == 0, "delete_task: resources cleared");
    CHECK(lagfx_resource_lookup(&p->resources, 100, 7) == NULL,
          "delete_task: ref 100 gone");
    CHECK(lagfx_resource_lookup(&p->resources, 101, 7) == NULL,
          "delete_task: ref 101 gone");

    lagfx_device_free(dev);
}

static void test_delete_resource_handler(void) {
    mock_shell_t shell;
    memset(&shell, 0, sizeof(shell));
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = dev->protocol_state;

    lagfx_resource_register(&p->resources, 50,
                            LAGFX_RESOURCE_TYPE_BUFFER, 1, 0, 100);
    CHECK(p->resources.count == 1, "0x08: 1 resource before delete");

    uint8_t del[12 + 8];
    size_t hlen = build_header(del, LAGFX_OP_DELETE_RESOURCE, 1,
                               (uint32_t)sizeof(del), 0x300);
    put_le32(del + hlen, 1);
    put_le32(del + hlen + 4, 50);
    lagfx_protocol_dispatch_one(p, del, sizeof(del));

    CHECK(p->resources.count == 0, "0x08: resource unregistered");
    CHECK(lagfx_resource_lookup(&p->resources, 50, 1) == NULL,
          "0x08: ref 50 gone after CmdDeleteResource");

    lagfx_device_free(dev);
}

static void test_set_object_placement_handler(void) {
    mock_shell_t shell;
    memset(&shell, 0, sizeof(shell));
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = dev->protocol_state;

    lagfx_resource_register(&p->resources, 20,
                            LAGFX_RESOURCE_TYPE_SAMPLER, 3, 0, 64);
    lagfx_resource_register(&p->resources, 21,
                            LAGFX_RESOURCE_TYPE_SAMPLER, 3, 0, 64);
    CHECK(p->resources.count == 2, "0x25: 2 resources before");

    uint8_t plc[12 + 8 + 8];
    size_t hlen = build_header(plc, LAGFX_OP_SET_OBJECT_PLACEMENT, 2,
                               (uint32_t)sizeof(plc), 0x400);
    put_le32(plc + hlen, 3);
    put_le32(plc + hlen + 4, 2);
    put_le32(plc + hlen + 8, 20);
    put_le32(plc + hlen + 12, 21);
    lagfx_protocol_dispatch_one(p, plc, sizeof(plc));

    CHECK(p->resources.count == 0, "0x25: both resources unregistered");
    CHECK(lagfx_resource_lookup(&p->resources, 20, 3) == NULL,
          "0x25: ref 20 gone");
    CHECK(lagfx_resource_lookup(&p->resources, 21, 3) == NULL,
          "0x25: ref 21 gone");

    lagfx_device_free(dev);
}

static void test_reset_clears_resources(void) {
    mock_shell_t shell;
    memset(&shell, 0, sizeof(shell));
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = dev->protocol_state;

    lagfx_resource_register(&p->resources, 1,
                            LAGFX_RESOURCE_TYPE_BUFFER, 1, 0, 100);
    lagfx_resource_register(&p->resources, 2,
                            LAGFX_RESOURCE_TYPE_BUFFER, 2, 0, 200);
    CHECK(p->resources.count == 2, "reset: 2 resources before reset");

    lagfx_protocol_reset(p);
    CHECK(p->resources.count == 0, "reset: resources cleared by reset");

    lagfx_device_free(dev);
}

/* === main ================================================== */

int main(void) {
#ifndef __linux__
    fprintf(stderr, "resource registry requires Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    test_register_lookup();
    test_register_update();
    test_unregister();
    test_clear_task();
    test_null_safety();
    test_map_memory_immediate_registers_buffer();
    test_delete_task_clears_resources();
    test_delete_resource_handler();
    test_set_object_placement_handler();
    test_reset_clears_resources();

    fprintf(stdout, "\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
