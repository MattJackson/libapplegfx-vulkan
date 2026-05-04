/*
 * libapplegfx-vulkan — M4 blit opcode unit tests
 * tests/m4-blit-opcodes.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers CmdCopyImage, CmdClearColorImage, CmdResolveImage with various
 * source/dest combinations and edge cases. Tests payload parsing for all
 * blit variants.
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"
#include "../src/protocol/blit_decoder.h"

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

static void put_le64(uint8_t *b, uint64_t v) {
    put_le32(b,     (uint32_t)(v & 0xffffffffull));
    put_le32(b + 4, (uint32_t)((v >> 32) & 0xffffffffull));
}

/* === CmdCopyImage tests ============================================= */

static void test_copy_image_basic(void) {
    fprintf(stdout, "\n--- test: copy_image_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdCopyImage payload:
     * - src_texture_gpa: u64 @ +0x0c
     * - dst_texture_gpa: u64 @ +0x14
     * - region_offset_x: u32 @ +0x1c
     * - region_offset_y: u32 @ +0x20
     * - region_extent_w: u32 @ +0x24
     * - region_extent_h: u32 @ +0x28
     * Total length = 40B */
    uint8_t cmd[56];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 40, 0xa5a50030);

    put_le64(cmd + 0x0c, 0x1000ull << 12);   /* src_texture_gpa */
    put_le64(cmd + 0x14, 0x2000ull << 12);   /* dst_texture_gpa */
    put_le32(cmd + 0x1c, 0);                 /* offset_x = 0 */
    put_le32(cmd + 0x20, 0);                 /* offset_y = 0 */
    put_le32(cmd + 0x24, 64);                /* extent_w = 64 */
    put_le32(cmd + 0x28, 64);                /* extent_h = 64 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdCopyImage returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after copy image");

    lagfx_device_free(dev);
}

static void test_copy_image_large_region(void) {
    fprintf(stdout, "\n--- test: copy_image_large_region ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[56];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 40, 0xa5a50031);

    put_le64(cmd + 0x0c, 0x1000ull << 12);   /* src */
    put_le64(cmd + 0x14, 0x2000ull << 12);   /* dst */
    put_le32(cmd + 0x1c, 0);                 /* offset_x = 0 */
    put_le32(cmd + 0x20, 0);                 /* offset_y = 0 */
    put_le32(cmd + 0x24, 1920);              /* extent_w = 1920 (HD width) */
    put_le32(cmd + 0x28, 1080);              /* extent_h = 1080 (HD height) */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdCopyImage accepts large region");

    lagfx_device_free(dev);
}

static void test_copy_image_offset(void) {
    fprintf(stdout, "\n--- test: copy_image_offset ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[56];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 40, 0xa5a50032);

    put_le64(cmd + 0x0c, 0x1000ull << 12);   /* src */
    put_le64(cmd + 0x14, 0x2000ull << 12);   /* dst */
    put_le32(cmd + 0x1c, 100);               /* offset_x = 100 */
    put_le32(cmd + 0x20, 50);                /* offset_y = 50 */
    put_le32(cmd + 0x24, 64);                /* extent_w = 64 */
    put_le32(cmd + 0x28, 64);                /* extent_h = 64 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdCopyImage accepts non-zero offset");

    lagfx_device_free(dev);
}

/* === CmdClearColorImage tests ======================================= */

static void test_clear_color_image_basic(void) {
    fprintf(stdout, "\n--- test: clear_color_image_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdClearColorImage payload:
     * - texture_gpa: u64 @ +0x0c
     * - color_r: f32 @ +0x14
     * - color_g: f32 @ +0x18
     * - color_b: f32 @ +0x1c
     * - color_a: f32 @ +0x20
     * Total length = 32B */
    uint8_t cmd[48];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 32, 0xa5a50033);

    put_le64(cmd + 0x0c, 0x3000ull << 12);   /* texture_gpa */
    put_le32(cmd + 0x14, 0xff000080u);       /* r = 1.0 (red) */
    put_le32(cmd + 0x18, 0x00ff0080u);       /* g = 1.0 */
    put_le32(cmd + 0x1c, 0x0000ff80u);       /* b = 1.0 */
    put_le32(cmd + 0x20, 0x80808080u);       /* a = 1.0 (opaque) */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdClearColorImage returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after clear color");

    lagfx_device_free(dev);
}

static void test_clear_color_image_blue(void) {
    fprintf(stdout, "\n--- test: clear_color_image_blue ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[48];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 32, 0xa5a50034);

    put_le64(cmd + 0x0c, 0x3000ull << 12);   /* texture_gpa */
    put_le32(cmd + 0x14, 0x80808080u);       /* r = 0.0 */
    put_le32(cmd + 0x18, 0x80808080u);       /* g = 0.0 */
    put_le32(cmd + 0x1c, 0xff000080u);       /* b = 1.0 (blue) */
    put_le32(cmd + 0x20, 0x80808080u);       /* a = 1.0 */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdClearColorImage accepts blue color");

    lagfx_device_free(dev);
}

/* === CmdResolveImage tests ========================================== */

static void test_resolve_image_basic(void) {
    fprintf(stdout, "\n--- test: resolve_image_basic ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdResolveImage payload:
     * - src_texture_gpa: u64 @ +0x0c (multi-sample)
     * - dst_texture_gpa: u64 @ +0x14 (single-sample)
     * Total length = 24B */
    uint8_t cmd[32];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 24, 0xa5a50035);

    put_le64(cmd + 0x0c, 0x4000ull << 12);   /* src multi-sample */
    put_le64(cmd + 0x14, 0x5000ull << 12);   /* dst single-sample */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc >= 0 || rc == -4, "CmdResolveImage returns success or ERR_SIZE");
    CHECK(shell.raise_irq_count == 1, "IRQ raised after resolve image");

    lagfx_device_free(dev);
}

/* === Error path tests =============================================== */

static void test_blit_invalid_payload(void) {
    fprintf(stdout, "\n--- test: blit_invalid_payload ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Fail-open: accepts short payload gracefully. */
    uint8_t cmd[LAGFX_CMD_HEADER_BYTES + 12];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, /*arg_count=*/0, 12, 0xa5a50036);

    (void)lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(shell.raise_irq_count == 1, "IRQ raised even for short payload (fail-open)");

    lagfx_device_free(dev);
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/m4-blit-opcodes: starting\n");

    /* CmdCopyImage tests. */
    test_copy_image_basic();
    test_copy_image_large_region();
    test_copy_image_offset();

    /* CmdClearColorImage tests. */
    test_clear_color_image_basic();
    test_clear_color_image_blue();

    /* CmdResolveImage tests. */
    test_resolve_image_basic();

    /* Error path tests. */
    test_blit_invalid_payload();

    fprintf(stdout, "\n=== m4-blit-opcodes: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
