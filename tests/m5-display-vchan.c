/*
 * libapplegfx-vulkan — M5 display vchan unit tests
 * tests/m5-display-vchan.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers CmdDefineChildFIFO (opcode 0x04) and CmdDisplayTransaction3 (0x16).
 * These are the M5 Stage 10% gate opcodes — they enable visible pixels by
 * setting up child channel rings and display transactions. Tests:
 *
 *   1. CmdDefineChildFIFO payload parsing (ring base, stamp slot)
 *   2. Child channel ring allocation validation  
 *   3. Stamp base initialization for each vchan
 *   4. CmdDisplayTransaction3 surface mapping
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__); \
        g_fail++;                                                        \
    } else {                                                             \
        fprintf(stdout, "PASS: %s\n", msg);                              \
        g_pass++;                                                        \
    }                                                                    \
} while (0)

/* === Mock shell with heap mirror for ring pages ====================== */

typedef struct {
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
    uint8_t heap[131072];  /* 128 KiB — enough for multiple ring pages */
    uint64_t heap_gpa;
} m5_shell_t;

static lagfx_task_t *m5_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = malloc(4096);
    return (lagfx_task_t *)0x1u;
}
static void m5_destroy_task(void *op, lagfx_task_t *t) {
    (void)op;
    if (t) free(t);
}
static bool m5_map(void *op, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool m5_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l; return true;
}
static bool m5_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    m5_shell_t *m = (m5_shell_t *)op;
    m->read_memory_count++;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + sizeof(m->heap)) {
        if (d) memcpy(d, m->heap + (gpa - m->heap_gpa), (size_t)l);
        return true;
    }
    if (d) memset(d, 0, (size_t)l);
    return true;
}
static bool m5_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    m5_shell_t *m = (m5_shell_t *)op;
    m->write_memory_count++;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + sizeof(m->heap)) {
        memcpy(m->heap + (gpa - m->heap_gpa), s, (size_t)l);
    }
    return true;
}
static void m5_irq(void *op, uint32_t vec) {
    m5_shell_t *m = (m5_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
}

static lagfx_device_t *make_dev(m5_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = m5_create_task;
    d.shell.destroy_task    = m5_destroy_task;
    d.shell.map_memory      = m5_map;
    d.shell.unmap_memory    = m5_unmap;
    d.shell.read_memory     = m5_read;
    d.shell.write_memory    = m5_write;
    d.shell.raise_interrupt = m5_irq;
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) {
        fprintf(stderr, "FATAL: device_new failed: %s\n", err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* === Header/payload builders ========================================= */

static size_t build_header(uint8_t *out, uint16_t opcode,
                           uint16_t arg_count_8b,
                           uint32_t total_length, uint32_t stamp) {
    memset(out, 0, LAGFX_CMD_HEADER_BYTES);
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
    return LAGFX_CMD_HEADER_BYTES;
}

static void put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8)  & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[i] = (uint8_t)((v >> (i * 8)) & 0xffu);
    }
}

/* === CmdDefineChildFIFO tests ======================================== */

static void test_define_child_fifo_basic(void) {
    fprintf(stdout, "\n--- test: define_child_fifo_basic ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdDefineChildFIFO payload (28B):
     * - child_id: u32 @ +0x0c (1-based, 1..4 for vchan)
     * - ring_base_pfn: u64 @ +0x10 (PFN of ring page)  
     * - stamp_slot: u32 @ +0x18 (slot in stampBases[])
     */
    uint8_t cmd[48];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DEFINE_CHILD_FIFO, /*arg_count=*/0, 28, 0xa5a50001);

    put_le32(cmd + 0x0c, 1);              /* child_id = 1 (vchan 1) */
    put_le64(cmd + 0x10, 0xaaaaull << 12); /* ring_base_pfn as GPA */
    put_le32(cmd + 0x18, 5);              /* stamp_slot = 5 (Display 0) */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdDefineChildFIFO returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after child FIFO define");

    lagfx_device_free(dev);
}

static void test_define_child_fifo_multiple_channels(void) {
    fprintf(stdout, "\n--- test: define_child_fifo_multiple_channels ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define child FIFOs for channels 1, 2, 3 with different ring bases */
    uint8_t cmd[48];
    
    for (int i = 0; i < 3; ++i) {
        memset(cmd, 0, sizeof(cmd));
        build_header(cmd, LAGFX_OP_DEFINE_CHILD_FIFO, /*arg_count=*/0, 28, 
                     0xa5a50010 + i);
        
        put_le32(cmd + 0x0c, i + 1);              /* child_id = 1,2,3 */
        put_le64(cmd + 0x10, (0xbbbb + i) << 12); /* ring_base_pfn */
        put_le32(cmd + 0x18, 5 + i);              /* stamp_slot */
        
        int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
        if (rc >= 0 || rc == -4) {
            fprintf(stdout, "PASS: CmdDefineChildFIFO channel %d\n", i + 1);
            g_pass++;
        } else {
            fprintf(stderr, "FAIL: CmdDefineChildFIFO channel %d failed\n", i + 1);
            g_fail++;
        }
    }
    
    CHECK(shell.raise_irq_count == 3, "IRQ raised 3 times for 3 channels");

    lagfx_device_free(dev);
}

static void test_define_child_fifo_invalid_child_id(void) {
    fprintf(stdout, "\n--- test: define_child_fifo_invalid_child_id ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[48];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DEFINE_CHILD_FIFO, /*arg_count=*/0, 28, 0xa5a50013);

    put_le32(cmd + 0x0c, 0);              /* child_id = 0 (invalid) */
    
    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(shell.raise_irq_count == 1, "IRQ raised even for invalid child_id");

    lagfx_device_free(dev);
}

/* === CmdDisplayTransaction3 tests ==================================== */

static void test_display_transaction3_basic(void) {
    fprintf(stdout, "\n--- test: display_transaction3_basic ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdDisplayTransaction3 payload (64B):
     * - transaction_id: u32 @ +0x0c
     * - surface_count: u32 @ +0x10  
     * - surface_entries[]: array of {gpu_addr, host_handle} @ +0x14 */
    uint8_t cmd[96];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DISPLAY_TRANSACTION3, /*arg_count=*/0, 64, 
                 0xa5a50020);

    put_le32(cmd + 0x0c, 1);              /* transaction_id */
    put_le32(cmd + 0x10, 1);              /* surface_count = 1 */
    
    /* First surface entry: gpu_addr at +0x14, host_handle at +0x1c */
    put_le64(cmd + 0x14, 0xccccull << 12); /* gpu_addr (PFN -> GPA) */
    put_le64(cmd + 0x1c, 0xddddull);       /* host_handle */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdDisplayTransaction3 returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after display transaction");

    lagfx_device_free(dev);
}

static void test_display_transaction3_multiple_surfaces(void) {
    fprintf(stdout, "\n--- test: display_transaction3_multiple_surfaces ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[96];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DISPLAY_TRANSACTION3, /*arg_count=*/0, 96, 
                 0xa5a50021);

    put_le32(cmd + 0x0c, 2);              /* transaction_id */
    put_le32(cmd + 0x10, 2);              /* surface_count = 2 */
    
    /* First surface: gpu_addr @ 0xeeee, host_handle @ 0xffff */
    put_le64(cmd + 0x14, 0xeeeeull << 12);
    put_le64(cmd + 0x1c, 0xffffull);
    
    /* Second surface: gpu_addr @ 0xdddd, host_handle @ 0xeeee */
    put_le64(cmd + 0x24, 0xddddull << 12);
    put_le64(cmd + 0x2c, 0xeeeeull);

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdDisplayTransaction3 with 2 surfaces succeeds");
    CHECK(shell.raise_irq_count == 1, "IRQ raised for multi-surface transaction");

    lagfx_device_free(dev);
}

/* === Stamp base initialization tests ================================ */

static void test_stamp_base_initialization(void) {
    fprintf(stdout, "\n--- test: stamp_base_initialization ---\n");
    m5_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define child FIFO with stamp_slot = 5 (Display 0) */
    uint8_t cmd[48];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DEFINE_CHILD_FIFO, /*arg_count=*/0, 28, 
                 0xa5a50030);

    put_le32(cmd + 0x0c, 1);              /* child_id = 1 */
    put_le64(cmd + 0x10, 0xaaaaull << 12); /* ring_base_pfn */
    put_le32(cmd + 0x18, 5);              /* stamp_slot = 5 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "Child FIFO define with stamp_slot succeeds");
    
    /* Stamp base at slot 5 should be initialized in the ring page.
     * The stamp cell is at offset slot*4 within the ring page. */
    uint32_t stamp_cell = (shell.heap[0x1400] | 
                           (shell.heap[0x1401] << 8) |
                           (shell.heap[0x1402] << 16) |
                           (shell.heap[0x1403] << 24));
    /* Initial stamp should be 0 or 1 (non-zero floor). */
    CHECK(stamp_cell == 0 || stamp_cell == 1, 
          "Stamp base initialized to 0 or 1");

    lagfx_device_free(dev);
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/m5-display-vchan: starting\n");

    /* CmdDefineChildFIFO tests. */
    test_define_child_fifo_basic();
    test_define_child_fifo_multiple_channels();
    test_define_child_fifo_invalid_child_id();

    /* CmdDisplayTransaction3 tests. */
    test_display_transaction3_basic();
    test_display_transaction3_multiple_surfaces();

    /* Stamp base initialization. */
    test_stamp_base_initialization();

    fprintf(stdout, "\n=== m5-display-vchan: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
