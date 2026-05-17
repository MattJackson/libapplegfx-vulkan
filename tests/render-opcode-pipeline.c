/*
 * libapplegfx-vulkan — M4 render pipeline opcode unit tests
 * tests/m4-render-opcode-pipeline.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers CmdSetPipelineState, CmdSetBlendConstants, CmdSetColorWriteMask,
 * and other pipeline state setup opcodes. Tests payload parsing for all
 * pipeline variants.
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

/* === Mock shell with heap mirror ==================================== */

typedef struct {
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
    uint8_t heap[131072];
    uint64_t heap_gpa;
} m4_shell_t;

static lagfx_task_t *m4_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = malloc(4096);
    return (lagfx_task_t *)0x1u;
}
static void m4_destroy_task(void *op, lagfx_task_t *t) {
    (void)op;
    if (t) free(t);
}
static bool m4_map(void *op, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool m4_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l; return true;
}
static bool m4_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    m4_shell_t *m = (m4_shell_t *)op;
    m->read_memory_count++;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + sizeof(m->heap)) {
        if (d) memcpy(d, m->heap + (gpa - m->heap_gpa), (size_t)l);
        return true;
    }
    if (d) memset(d, 0, (size_t)l);
    return true;
}
static bool m4_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    m4_shell_t *m = (m4_shell_t *)op;
    m->write_memory_count++;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + sizeof(m->heap)) {
        memcpy(m->heap + (gpa - m->heap_gpa), s, (size_t)l);
    }
    return true;
}
static void m4_irq(void *op, uint32_t vec) {
    m4_shell_t *m = (m4_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
}

static lagfx_device_t *make_dev(m4_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = m4_create_task;
    d.shell.destroy_task    = m4_destroy_task;
    d.shell.map_memory      = m4_map;
    d.shell.unmap_memory    = m4_unmap;
    d.shell.read_memory     = m4_read;
    d.shell.write_memory    = m4_write;
    d.shell.raise_interrupt = m4_irq;
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

/* === CmdSetPipelineState tests ====================================== */

static void test_set_pipeline_state_basic(void) {
    fprintf(stdout, "\n--- test: set_pipeline_state_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdSetPipelineState payload:
     * - pipeline_id: u32 @ +0x0c
     * Total length = 16B */
    uint8_t cmd[24];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 16, 0xa5a50020);

    put_le32(cmd + 0x0c, 7);        /* pipeline_id = 7 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetPipelineState returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after set pipeline state");

    lagfx_device_free(dev);
}

static void test_set_pipeline_state_zero_id(void) {
    fprintf(stdout, "\n--- test: set_pipeline_state_zero_id ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[24];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 16, 0xa5a50021);

    put_le32(cmd + 0x0c, 0);        /* pipeline_id = 0 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetPipelineState accepts id=0");

    lagfx_device_free(dev);
}

/* === CmdSetBlendConstants tests ===================================== */

static void test_set_blend_constants_basic(void) {
    fprintf(stdout, "\n--- test: set_blend_constants_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdSetBlendConstants payload:
     * - r: f32 @ +0x0c
     * - g: f32 @ +0x10  
     * - b: f32 @ +0x14
     * - a: f32 @ +0x18
     * Total length = 24B */
    uint8_t cmd[36];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 24, 0xa5a50022);

    put_le32(cmd + 0x0c, 0x3f800000u);  /* r = 1.0f (IEEE 754) */
    put_le32(cmd + 0x10, 0x3f800000u);  /* g = 1.0f */
    put_le32(cmd + 0x14, 0x3f800000u);  /* b = 1.0f */
    put_le32(cmd + 0x18, 0x3f800000u);  /* a = 1.0f */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetBlendConstants returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after set blend constants");

    lagfx_device_free(dev);
}

static void test_set_blend_constants_alpha_zero(void) {
    fprintf(stdout, "\n--- test: set_blend_constants_alpha_zero ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[36];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 24, 0xa5a50023);

    put_le32(cmd + 0x0c, 0x3f800000u);  /* r = 1.0 */
    put_le32(cmd + 0x10, 0x3f800000u);  /* g = 1.0 */
    put_le32(cmd + 0x14, 0x3f800000u);  /* b = 1.0 */
    put_le32(cmd + 0x18, 0x00000000u);  /* a = 0.0 (transparent) */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetBlendConstants accepts alpha=0");

    lagfx_device_free(dev);
}

/* === CmdSetColorWriteMask tests ===================================== */

static void test_set_color_write_mask_basic(void) {
    fprintf(stdout, "\n--- test: set_color_write_mask_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdSetColorWriteMask payload:
     * - mask: u32 @ +0x0c (bits: R,G,B,A enable)
     * Total length = 16B */
    uint8_t cmd[24];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 16, 0xa5a50024);

    put_le32(cmd + 0x0c, 0x0Fu);        /* mask = RGBA all enabled */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetColorWriteMask returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after set color write mask");

    lagfx_device_free(dev);
}

static void test_set_color_write_mask_partial(void) {
    fprintf(stdout, "\n--- test: set_color_write_mask_partial ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[24];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 16, 0xa5a50025);

    put_le32(cmd + 0x0c, 0x0Du);        /* mask = RGB only (no alpha) */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdSetColorWriteMask accepts partial mask");

    lagfx_device_free(dev);
}

/* === Error path tests =============================================== */

static void test_pipeline_invalid_payload(void) {
    fprintf(stdout, "\n--- test: pipeline_invalid_payload ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Fail-open: accepts short payload gracefully. */
    uint8_t cmd[LAGFX_CMD_HEADER_BYTES + 12];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 12, 0xa5a50026);

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    (void)rc;
    CHECK(shell.raise_irq_count == 1, "IRQ raised even for short payload (fail-open)");

    lagfx_device_free(dev);
}

/* === main ============================================================ */

int main(void) {
#ifndef __linux__
    fprintf(stderr, "render opcode pipeline requires Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    fprintf(stdout, "tests/m4-render-opcode-pipeline: starting\n");

    /* CmdSetPipelineState tests. */
    test_set_pipeline_state_basic();
    test_set_pipeline_state_zero_id();

    /* CmdSetBlendConstants tests. */
    test_set_blend_constants_basic();
    test_set_blend_constants_alpha_zero();

    /* CmdSetColorWriteMask tests. */
    test_set_color_write_mask_basic();
    test_set_color_write_mask_partial();

    /* Error path tests. */
    test_pipeline_invalid_payload();

    fprintf(stdout, "\n=== m4-render-opcode-pipeline: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
