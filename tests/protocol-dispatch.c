/*
 * libapplegfx-vulkan — protocol dispatch unit tests (Phase 1.A.2)
 * tests/protocol-dispatch.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the opcode dispatcher for the two handlers with real
 * implementations in this phase (CmdNOP, CmdDebug), plus the
 * descriptor-table / header-parse plumbing. All inputs are
 * synthesized against the 12-byte on-wire header documented in
 * mos/paravirt-re/re-followup-spec-gaps.md §5.1:
 *
 *    struct lagfx_cmd_header {
 *        uint16_t opcode;        [0..1]
 *        uint16_t arg_count_8b;  [2..3]
 *        uint32_t length;        [4..7]
 *        uint32_t stamp;         [8..11]
 *    };
 *
 * There is no `flags` byte — every command unconditionally signals
 * its stamp on completion, and the dispatcher raises the IRQ
 * unconditionally as well.
 *
 * Drives dispatch through two paths:
 *   1. lagfx_protocol_dispatch_one() directly (fast; tests the
 *      handler jump table + completion path).
 *   2. lagfx_mmio_write(<candidate offset>) -> decoder setter probe
 *      (skeleton; confirms the candidate-range wiring — the real
 *      doorbell offset in 0x1004..0x1034 is still unknown and will
 *      be disambiguated at runtime).
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/display.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"
#include "../src/protocol/fifo.h"
#include "../src/protocol/state.h"
#include "../src/protocol/ops_display.h"
#include "../src/protocol/ops_iosurface.h"

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

/* === Mock shell callbacks ================================= */

/* Test-side scanout capture. When scanout_gpa / scanout_capacity are
 * set, mock_write routes any write_memory call whose range lies within
 * [scanout_gpa, scanout_gpa+scanout_capacity) into scanout_buf at the
 * matching offset. Used by test_metal_clear_color_sequence to observe
 * the M4 GAP #1 DMA writeback that ops_display.c → display.c issues
 * against the guest's scanout VA after CmdDisplayTransaction3
 * fence-waits. */
typedef struct {
    unsigned raise_irq_count;
    uint32_t last_irq_vector;
    unsigned create_task_count;
    unsigned destroy_task_count;
    unsigned map_memory_count;
    unsigned unmap_memory_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
    uint64_t scanout_gpa;       /* 0 = not-capturing */
    uint64_t scanout_capacity;  /* bytes backing scanout_buf */
    uint8_t *scanout_buf;       /* owned by test, NULL-ok */
    uint64_t last_write_gpa;
    uint64_t last_write_len;

    /* Phase M3 plumbing tests: synthetic guest-memory mirror. When
     * `ring_backing` is non-NULL, mock_read routes any read_memory call
     * whose [gpa, gpa+len) fits within [ring_gpa, ring_gpa+ring_capacity)
     * into that buffer. Lets the fifo drain see a real command stream
     * without needing real guest DMA. Likewise mock_write captures all
     * writes into the same region when they overlap it. */
    uint64_t ring_gpa;
    uint64_t ring_capacity;
    uint8_t *ring_backing;

    /* Capture of CmdGetDeviceInfo2 response-page writes: when
     * `devinfo_gpa` is non-zero, mock_write stashes up to
     * `devinfo_capacity` bytes at the matching offset in devinfo_buf. */
    uint64_t devinfo_gpa;
    uint64_t devinfo_capacity;
    uint8_t *devinfo_buf;
    uint64_t devinfo_last_len;
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
    mock_shell_t *m = (mock_shell_t *)op;
    m->map_memory_count++;
    (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool mock_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->unmap_memory_count++;
    (void)t; (void)o; (void)l;
    return true;
}
static bool mock_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->read_memory_count++;
    /* Serve guest-ring reads from the test-owned backing buffer when
     * configured. */
    if (m->ring_backing && m->ring_capacity > 0
        && gpa >= m->ring_gpa
        && gpa + l <= m->ring_gpa + m->ring_capacity
        && d) {
        uint64_t off = gpa - m->ring_gpa;
        memcpy(d, m->ring_backing + off, (size_t)l);
        return true;
    }
    (void)gpa; (void)l; (void)d;
    return true;
}
static bool mock_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->write_memory_count++;
    m->last_write_gpa = gpa;
    m->last_write_len = l;
    if (m->scanout_buf && m->scanout_capacity > 0
        && gpa >= m->scanout_gpa
        && gpa + l <= m->scanout_gpa + m->scanout_capacity) {
        uint64_t off = gpa - m->scanout_gpa;
        memcpy(m->scanout_buf + off, s, (size_t)l);
    }
    if (m->ring_backing && m->ring_capacity > 0
        && gpa >= m->ring_gpa
        && gpa + l <= m->ring_gpa + m->ring_capacity) {
        uint64_t off = gpa - m->ring_gpa;
        memcpy(m->ring_backing + off, s, (size_t)l);
    }
    if (m->devinfo_buf && m->devinfo_capacity > 0
        && gpa >= m->devinfo_gpa
        && gpa + l <= m->devinfo_gpa + m->devinfo_capacity) {
        uint64_t off = gpa - m->devinfo_gpa;
        memcpy(m->devinfo_buf + off, s, (size_t)l);
        m->devinfo_last_len = l;
    }
    return true;
}
static void mock_raise_irq(void *op, uint32_t vec) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->raise_irq_count++;
    m->last_irq_vector = vec;
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
        fprintf(stderr, "FATAL: device_new failed: %s\n", err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* === Command-byte synthesis helpers ======================= */

/* Build a 12-byte header at the front of `out` (must have room for
 * LAGFX_CMD_HEADER_BYTES bytes). All fields are little-endian per
 * the x86-64 LE guest ABI. Returns the number of header bytes
 * written; caller appends arg/tail data if any. */
static size_t build_header(uint8_t *out, uint16_t opcode,
                           uint16_t arg_count_8b,
                           uint32_t total_length, uint32_t stamp) {
    memset(out, 0, LAGFX_CMD_HEADER_BYTES);
    /* opcode @ [0..1] */
    out[0] = (uint8_t)(opcode & 0xffu);
    out[1] = (uint8_t)((opcode >> 8) & 0xffu);
    /* arg_count_8b @ [2..3] */
    out[2] = (uint8_t)(arg_count_8b & 0xffu);
    out[3] = (uint8_t)((arg_count_8b >> 8) & 0xffu);
    /* length @ [4..7] */
    out[4] = (uint8_t)(total_length & 0xffu);
    out[5] = (uint8_t)((total_length >> 8) & 0xffu);
    out[6] = (uint8_t)((total_length >> 16) & 0xffu);
    out[7] = (uint8_t)((total_length >> 24) & 0xffu);
    /* stamp @ [8..11] */
    out[8]  = (uint8_t)(stamp & 0xffu);
    out[9]  = (uint8_t)((stamp >> 8) & 0xffu);
    out[10] = (uint8_t)((stamp >> 16) & 0xffu);
    out[11] = (uint8_t)((stamp >> 24) & 0xffu);
    return LAGFX_CMD_HEADER_BYTES;
}

/* === Tests ================================================ */

static void test_header_size_invariant(void) {
    fprintf(stdout, "\n--- test: header_size_invariant ---\n");
    CHECK(LAGFX_CMD_HEADER_BYTES == 12u,
          "on-wire header is 12 bytes");
    /* Compile-time offsets are asserted in opcodes.h; re-check at
     * runtime for belt-and-braces. */
    lagfx_cmd_header_t h;
    CHECK((uintptr_t)&h.opcode       - (uintptr_t)&h == 0,
          "opcode at offset 0");
    CHECK((uintptr_t)&h.arg_count_8b - (uintptr_t)&h == 2,
          "arg_count_8b at offset 2");
    CHECK((uintptr_t)&h.length       - (uintptr_t)&h == 4,
          "length at offset 4");
    CHECK((uintptr_t)&h.stamp        - (uintptr_t)&h == 8,
          "stamp at offset 8");
}

static void test_opcode_table_completeness(void) {
    fprintf(stdout, "\n--- test: opcode_table_completeness ---\n");

    /* All 36 named opcodes per command-buffer-format.md §10. */
    CHECK(lagfx_opcode_table_size() == LAGFX_OPCODE_COUNT,
          "opcode table has expected entry count");

    /* Spot-check each P0 opcode from the brief §4.1. */
    static const uint16_t p0[] = {
        LAGFX_OP_DEFINE_TASK2, LAGFX_OP_DELETE_TASK,
        LAGFX_OP_DEFINE_CHILD_FIFO, LAGFX_OP_DELETE_CHILD_FIFO,
        LAGFX_OP_GET_DEVICE_INFO, LAGFX_OP_SYNCHRONIZE_RESOURCES,
        LAGFX_OP_NOP,
    };
    for (size_t i = 0; i < sizeof(p0) / sizeof(p0[0]); ++i) {
        const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(p0[i]);
        char msg[64];
        snprintf(msg, sizeof(msg), "P0 opcode 0x%02x in table", p0[i]);
        CHECK(d != NULL, msg);
        if (d) {
            snprintf(msg, sizeof(msg), "P0 opcode 0x%02x priority=P0", p0[i]);
            CHECK(d->priority == LAGFX_PRIO_P0, msg);
        }
    }

    /* CmdDefineChildFIFO now only needs 4 payload bytes, not 16
     * (re-followup-spec-gaps.md §3). */
    const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(LAGFX_OP_DEFINE_CHILD_FIFO);
    CHECK(d != NULL && d->min_payload == 4,
          "CmdDefineChildFIFO min_payload == 4");

    /* CmdSynchronizeResources minimum 8 bytes (empty-list case). */
    d = lagfx_opcode_lookup(LAGFX_OP_SYNCHRONIZE_RESOURCES);
    CHECK(d != NULL && d->min_payload == 8,
          "CmdSynchronizeResources min_payload == 8");

    /* CmdGetDeviceInfo min 12 bytes (3 u32 key triple). */
    d = lagfx_opcode_lookup(LAGFX_OP_GET_DEVICE_INFO);
    CHECK(d != NULL && d->min_payload == 12,
          "CmdGetDeviceInfo min_payload == 12");

    /* Unknown opcode returns NULL. */
    CHECK(lagfx_opcode_lookup(0xabab) == NULL,
          "unknown opcode lookup returns NULL");

    /* Name fallback prints Unknown(...) — never returns NULL. */
    const char *name = lagfx_opcode_name(0xabab);
    CHECK(name != NULL && strstr(name, "Unknown") != NULL,
          "unknown opcode name formats as Unknown(...)");
}

static void test_header_parse(void) {
    fprintf(stdout, "\n--- test: header_parse ---\n");

    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, /*arg_count_8b=*/0,
                 /*total_length=*/12, /*stamp=*/0xcafebabeu);

    lagfx_cmd_header_t hdr;
    bool ok = lagfx_fifo_parse_header(buf, sizeof(buf), &hdr);
    CHECK(ok, "parse 12-byte NOP header");
    CHECK(hdr.opcode == LAGFX_OP_NOP, "opcode u16");
    CHECK(hdr.arg_count_8b == 0, "arg_count_8b");
    CHECK(hdr.length == 12, "length");
    CHECK(hdr.stamp == 0xcafebabeu, "stamp LE decode");
    CHECK(hdr.payload_size == 0, "payload_size derived (length - 12 == 0)");
    CHECK(hdr.payload == NULL, "payload pointer NULL for header-only");

    /* Header with arg_count_8b=2 and length=28 => payload_size=16. */
    uint8_t big[28];
    build_header(big, LAGFX_OP_DEBUG, /*arg_count_8b=*/2,
                 /*total_length=*/28, /*stamp=*/0xdeadbeefu);
    for (int i = 0; i < 16; ++i) big[12 + i] = (uint8_t)(0x10 + i);
    ok = lagfx_fifo_parse_header(big, sizeof(big), &hdr);
    CHECK(ok, "parse 28-byte command header");
    CHECK(hdr.arg_count_8b == 2, "arg_count_8b=2");
    CHECK(hdr.length == 28, "length=28");
    CHECK(hdr.payload_size == 16, "payload_size = length - 12 = 16");
    CHECK(hdr.payload == big + 12, "payload points past header");

    /* A header that claims length < 12 is malformed — reject. */
    uint8_t bad[LAGFX_CMD_HEADER_BYTES];
    build_header(bad, LAGFX_OP_NOP, 0, /*total_length=*/8, 0);
    CHECK(!lagfx_fifo_parse_header(bad, sizeof(bad), &hdr),
          "reject header with length < 12");

    /* Short buffer (< 12 bytes) rejected. */
    CHECK(!lagfx_fifo_parse_header(buf, 10, &hdr),
          "reject buffer shorter than header");
}

static void test_dispatch_nop(void) {
    fprintf(stdout, "\n--- test: dispatch_nop ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    CHECK(p != NULL, "device has decoder attached");

    /* Every command unconditionally signals its stamp → IRQ on every
     * dispatch (no flags gate; see re-followup-spec-gaps.md §5.1). */
    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, 0, 12, 0x11110001u);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "NOP dispatch #1 returns OK");
    CHECK(shell.raise_irq_count == 1, "NOP dispatch #1 raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x11110001u,
          "stamp cell carries first completed stamp");

    /* Second NOP with a different stamp. */
    build_header(buf, LAGFX_OP_NOP, 0, 12, 0x11110002u);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "NOP dispatch #2 returns OK");
    CHECK(shell.raise_irq_count == 2, "NOP dispatch #2 raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x11110002u,
          "stamp cell updated to second stamp");

    /* MMIO 0x1014 is the display-completion bitmask (xchg-and-clear)
     * per A4d (2026-04-24). It is NOT a last-completed-stamp register —
     * the older `STAMP_CELL_1` name is retained as a header alias for
     * the offset only. With no display channels having completed in
     * this test, the mask should read as 0 and stay 0 across repeated
     * reads (xchg semantics on a zero-bit pending mask). */
    uint32_t mask = lagfx_mmio_read(dev, LAGFX_REG_STAMP_CELL_1);
    CHECK(mask == 0u,
          "MMIO 0x1014 (display_bitmask) reads 0 with no displays pending");
    uint32_t mask2 = lagfx_mmio_read(dev, LAGFX_REG_STAMP_CELL_1);
    CHECK(mask2 == 0u,
          "MMIO 0x1014 second read still 0 (xchg-and-clear)");

    /* _rootPageNumber read slot (0x101c) is NOT a stamp register —
     * decoder scaffold hasn't been asked to populate it, so it
     * reads 0 by default here. Just confirm it doesn't carry the
     * stamp. */
    uint32_t rpn = lagfx_mmio_read(dev, LAGFX_REG_ROOT_PAGE_NUMBER);
    CHECK(rpn != 0x11110002u,
          "ROOT_PAGE_NUMBER (0x101c) is not a stamp register");

    /* Counters. */
    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(seen == 2, "total_cmds_seen == 2");
    CHECK(completed == 2, "total_cmds_completed == 2");
    CHECK(unknown == 0, "no unknown opcodes");

    lagfx_device_free(dev);
}

static void test_dispatch_debug(void) {
    fprintf(stdout, "\n--- test: dispatch_debug ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdDebug with a short payload. Total length = 12 + 8 = 20. */
    uint8_t buf[20];
    build_header(buf, LAGFX_OP_DEBUG, /*arg_count_8b=*/1,
                 /*total_length=*/20, /*stamp=*/0xdeadbeefu);
    for (int i = 0; i < 8; ++i) {
        buf[LAGFX_CMD_HEADER_BYTES + i] = (uint8_t)(0xa0 + i);
    }

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "Debug dispatch returns OK");
    CHECK(shell.raise_irq_count == 1, "Debug raised IRQ unconditionally");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xdeadbeefu,
          "Debug stamp propagated to stamp cell");

    lagfx_device_free(dev);
}

static void test_dispatch_unknown_opcode(void) {
    fprintf(stdout, "\n--- test: dispatch_unknown_opcode ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 0x00ab is not in the table. Default handler should log and ack. */
    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, /*opcode=*/0x00abu, 0, 12, 0x22220001u);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "unknown opcode falls through to default handler (fail-open)");
    CHECK(shell.raise_irq_count == 1, "unknown opcode still raises IRQ");

    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(unknown == 1, "unknown_opcode_count bumped");

    lagfx_device_free(dev);
}

static void test_dispatch_routes_to_correct_handler(void) {
    fprintf(stdout, "\n--- test: dispatch_routes_to_correct_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Issue one of every P0 opcode. All are either real (NOP) or
     * stubbed-but-present — must return OK and bump seen counters. */
    static const uint16_t ops[] = {
        LAGFX_OP_GET_DEVICE_INFO,
        LAGFX_OP_DEFINE_TASK2,
        LAGFX_OP_DELETE_TASK,
        LAGFX_OP_DEFINE_CHILD_FIFO,
        LAGFX_OP_DELETE_CHILD_FIFO,
        LAGFX_OP_SYNCHRONIZE_RESOURCES,
        LAGFX_OP_NOP,
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(ops[i]);
        uint16_t payload_bytes = d ? d->min_payload : 0;
        uint32_t total_length = LAGFX_CMD_HEADER_BYTES + payload_bytes;
        uint8_t buf[256] = {0};
        build_header(buf, ops[i], /*arg_count_8b=*/0, total_length,
                     (uint32_t)(0x33330000u + i));
        int rc = lagfx_protocol_dispatch_one(p, buf, total_length);
        char msg[64];
        snprintf(msg, sizeof(msg), "P0 op 0x%02x dispatch OK", ops[i]);
        CHECK(rc == LAGFX_HANDLER_OK, msg);
    }

    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(seen == sizeof(ops) / sizeof(ops[0]),
          "all P0 commands seen");
    CHECK(completed == seen, "all P0 commands completed");
    CHECK(unknown == 0, "no P0 opcode fell through to default");
    CHECK(shell.raise_irq_count == seen,
          "one IRQ per completed command (unconditional)");

    lagfx_device_free(dev);
}

/* test_mmio_setter_candidate_probe was removed alongside the
 * setter-candidate probe state (last_setter_offset / last_setter_value
 * / setter_write_count) — every offset in 0x1004..0x1034 now has a
 * dedicated handler (or is silently ignored), so the probe was dead
 * scaffolding. See m3-prod-readiness-audit.md cleanup item. */

static void test_mmio_status_control_arms_ring(void) {
    fprintf(stdout, "\n--- test: mmio_status_control_arms_ring ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    (void)p;

    /* Master FIFO enable (0x1000) — non-zero arms, zero disarms.
     * Scaffold doesn't actually drain (R1), but the ring_armed flag
     * is observable via the read shadow. */
    lagfx_mmio_write(dev, LAGFX_REG_STATUS_CONTROL, 0x1u);
    uint32_t v = lagfx_mmio_read(dev, LAGFX_REG_STATUS_CONTROL);
    CHECK(v == 0x1u, "STATUS_CONTROL shadowed after arm");

    lagfx_mmio_write(dev, LAGFX_REG_STATUS_CONTROL, 0x0u);
    v = lagfx_mmio_read(dev, LAGFX_REG_STATUS_CONTROL);
    CHECK(v == 0x0u, "STATUS_CONTROL shadowed after disarm");

    lagfx_device_free(dev);
}

/* === P0 handler tests (Phase 1.A.2 — real implementations) ===
 *
 * Each builds a command with the exact payload shape from
 * re-followup-spec-gaps.md + phase-1a2-decoder-plan.md §4.1 and asserts
 * on state mutation. */

/* LE writers for payload synthesis. */
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

static void test_get_device_info_handler(void) {
    fprintf(stdout, "\n--- test: get_device_info_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Request: {u32 key=1, u32 outOff=0x100, u32 flags=0}. Total len=24. */
    uint8_t buf[24];
    build_header(buf, LAGFX_OP_GET_DEVICE_INFO, /*arg_count_8b=*/0,
                 /*total_length=*/24, /*stamp=*/0x51510001u);
    put_le32(buf + 12, 0x1u);    /* keyIndex = 1 (maxTasks) */
    put_le32(buf + 16, 0x100u);  /* outOffset */
    put_le32(buf + 20, 0x0u);    /* flags */

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "GetDeviceInfo(key=maxTasks) returns OK");
    CHECK(shell.raise_irq_count == 1, "GetDeviceInfo raised completion IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x51510001u,
          "GetDeviceInfo stamp carried to cell");

    /* Too-small payload — dispatcher's min_payload check traps before
     * the handler runs; expect ERR_SIZE but stamp still signals. */
    uint8_t short_buf[16];
    build_header(short_buf, LAGFX_OP_GET_DEVICE_INFO, 0,
                 /*total_length=*/16, /*stamp=*/0x51510002u);
    put_le32(short_buf + 12, 0x0u); /* only one u32, not three */
    rc = lagfx_protocol_dispatch_one(p, short_buf, sizeof(short_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "GetDeviceInfo rejects 4-byte payload (< 12 min)");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x51510002u,
          "GetDeviceInfo still signals stamp on size error (fail-open)");

    lagfx_device_free(dev);
}

static void test_task_lifecycle_handler(void) {
    fprintf(stdout, "\n--- test: task_lifecycle_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* CmdDefineTask2: taskID=7, rootVA=0xcafe0000, length=0x100000, reserved=0 */
    uint8_t define_buf[36];
    build_header(define_buf, LAGFX_OP_DEFINE_TASK2, 0,
                 /*total_length=*/36, /*stamp=*/0x70000001u);
    put_le32(define_buf + 12, 7u);           /* taskID */
    put_le64(define_buf + 16, 0xcafe0000ull); /* rootVA */
    put_le64(define_buf + 24, 0x100000ull);   /* length */
    put_le32(define_buf + 32, 0u);           /* reserved */

    int rc = lagfx_protocol_dispatch_one(p, define_buf, sizeof(define_buf));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdDefineTask2 returns OK");
    CHECK(shell.create_task_count == 1,
          "CmdDefineTask2 invoked shell.create_task exactly once");

    /* Confirm the task table has the entry. */
    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, 7u);
    CHECK(entry != NULL, "task table contains taskID=7");
    CHECK(entry && entry->live, "task entry marked live");
    CHECK(entry && entry->length == 0x100000ull,
          "task entry length matches payload");
    CHECK(entry && entry->base_va == 0xcafe0000ull,
          "task entry base_va matches rootVA");

    /* CmdDeleteTask with taskID=7 — should call shell.destroy_task and
     * mark entry !live. */
    uint8_t del_buf[16];
    build_header(del_buf, LAGFX_OP_DELETE_TASK, 0,
                 /*total_length=*/16, /*stamp=*/0x70000002u);
    put_le32(del_buf + 12, 7u);
    rc = lagfx_protocol_dispatch_one(p, del_buf, sizeof(del_buf));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdDeleteTask returns OK");
    CHECK(shell.destroy_task_count == 1,
          "CmdDeleteTask invoked shell.destroy_task exactly once");
    CHECK(lagfx_protocol_find_task(p, 7u) == NULL,
          "task entry removed after DeleteTask");

    /* Deleting a non-existent task returns ERR_STATE but still
     * completes the stamp (fail-open). */
    build_header(del_buf, LAGFX_OP_DELETE_TASK, 0, 16, 0x70000003u);
    put_le32(del_buf + 12, 99u);
    rc = lagfx_protocol_dispatch_one(p, del_buf, sizeof(del_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_STATE,
          "CmdDeleteTask on missing taskID returns ERR_STATE");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x70000003u,
          "CmdDeleteTask stamp still signals on miss");

    lagfx_device_free(dev);
}

static void test_child_fifo_lifecycle_handler(void) {
    fprintf(stdout, "\n--- test: child_fifo_lifecycle_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define two child FIFOs with distinct IDs. */
    uint8_t buf[16];
    build_header(buf, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0x80000001u);
    put_le32(buf + 12, 3u);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "DefineChildFIFO(id=3) returns OK");

    build_header(buf, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0x80000002u);
    put_le32(buf + 12, 5u);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "DefineChildFIFO(id=5) returns OK");

    CHECK(lagfx_protocol_find_fifo(p, 3u) != NULL,
          "fifo table contains id=3");
    CHECK(lagfx_protocol_find_fifo(p, 5u) != NULL,
          "fifo table contains id=5");
    CHECK(lagfx_protocol_find_fifo(p, 4u) == NULL,
          "fifo table does NOT contain id=4 (never defined)");

    /* DeleteChildFIFO(id=3). */
    build_header(buf, LAGFX_OP_DELETE_CHILD_FIFO, 0, 16, 0x80000003u);
    put_le32(buf + 12, 3u);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "DeleteChildFIFO(id=3) returns OK");
    CHECK(lagfx_protocol_find_fifo(p, 3u) == NULL,
          "id=3 removed from fifo table");
    CHECK(lagfx_protocol_find_fifo(p, 5u) != NULL,
          "id=5 still live");

    /* Delete a non-existent ID — ERR_STATE but stamp still signals. */
    build_header(buf, LAGFX_OP_DELETE_CHILD_FIFO, 0, 16, 0x80000004u);
    put_le32(buf + 12, 42u);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_ERR_STATE,
          "DeleteChildFIFO(unknown id) returns ERR_STATE");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x80000004u,
          "DeleteChildFIFO stamp signals on miss");

    lagfx_device_free(dev);
}

static void test_synchronize_resources_handler(void) {
    fprintf(stdout, "\n--- test: synchronize_resources_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Empty list — the metal-no-op completion path. payload = 8 bytes
     * (taskID=0, count=0). Must complete immediately, raise IRQ. */
    uint8_t empty_buf[20];
    build_header(empty_buf, LAGFX_OP_SYNCHRONIZE_RESOURCES, 0,
                 /*total_length=*/20, /*stamp=*/0x90000001u);
    put_le32(empty_buf + 12, 0u); /* taskID */
    put_le32(empty_buf + 16, 0u); /* count=0 */

    int rc = lagfx_protocol_dispatch_one(p, empty_buf, sizeof(empty_buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "SynchronizeResources(count=0) returns OK immediately");
    CHECK(shell.raise_irq_count == 1,
          "SynchronizeResources(count=0) raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x90000001u,
          "SynchronizeResources(count=0) stamp signalled");

    /* Non-empty list — define two child FIFOs first so the handler can
     * find them, then issue a sync that names IDs 10 and 20. Only
     * FIFO with id=10 is registered, so matched=1. */
    uint8_t def_buf[16];
    build_header(def_buf, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0x90000002u);
    put_le32(def_buf + 12, 10u);
    lagfx_protocol_dispatch_one(p, def_buf, sizeof(def_buf));

    uint8_t sync_buf[28];
    build_header(sync_buf, LAGFX_OP_SYNCHRONIZE_RESOURCES, 0,
                 /*total_length=*/28, /*stamp=*/0x90000003u);
    put_le32(sync_buf + 12, 0u);  /* taskID (unknown, fail-open) */
    put_le32(sync_buf + 16, 2u);  /* count=2 */
    put_le32(sync_buf + 20, 10u); /* id[0]=10 (matches) */
    put_le32(sync_buf + 24, 20u); /* id[1]=20 (miss) */
    rc = lagfx_protocol_dispatch_one(p, sync_buf, sizeof(sync_buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "SynchronizeResources(count=2) returns OK (fail-open on unknown taskID)");

    lagfx_childfifo_entry_t *f = lagfx_protocol_find_fifo(p, 10u);
    CHECK(f != NULL && f->synced,
          "fifo id=10 marked synced by SynchronizeResources");

    /* Malformed: count=5 but payload size only covers 2 u32s — reject. */
    uint8_t bad_buf[20];
    build_header(bad_buf, LAGFX_OP_SYNCHRONIZE_RESOURCES, 0,
                 /*total_length=*/20, /*stamp=*/0x90000004u);
    put_le32(bad_buf + 12, 0u);
    put_le32(bad_buf + 16, 5u); /* count=5, but only 8 bytes after */
    rc = lagfx_protocol_dispatch_one(p, bad_buf, sizeof(bad_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "SynchronizeResources(count > payload) rejected");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x90000004u,
          "SynchronizeResources size-mismatch still signals stamp");

    lagfx_device_free(dev);
}

/* === CmdMapMemory2 / CmdUnmapMemory / CmdExecIndirect2 ===
 *
 * P1 handlers implemented in Phase 1.A.2 against command-buffer-format.md
 * §4 and re-followup-spec-gaps.md R2. The map-memory shape is PARTIAL
 * (re-followup did not decode 0x02/0x03); tests fix the on-wire shape
 * to what the current handler expects. */

static void test_map_memory2_handler(void) {
    fprintf(stdout, "\n--- test: map_memory2_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define a task first so the map has a valid taskID to bind to. */
    uint8_t dt[36];
    build_header(dt, LAGFX_OP_DEFINE_TASK2, 0, 36, 0xb0000001u);
    put_le32(dt + 12, 42u);
    put_le64(dt + 16, 0ull);
    put_le64(dt + 24, 0x10000ull);
    put_le32(dt + 32, 0u);
    lagfx_protocol_dispatch_one(p, dt, sizeof(dt));
    CHECK(shell.create_task_count == 1, "task created for map_memory test");

    /* CmdMapMemory2 with one range:
     *   [0..3]   taskID=42
     *   [4..11]  virtualOffset=0x2000
     *   [12..15] readOnly=1
     *   [16..19] rangeCount=1
     *   [20..27] ranges[0].gpa=0xcafed000
     *   [28..35] ranges[0].length=0x1000
     * payload=36 bytes, header=12, total=48 bytes. */
    uint8_t buf[48];
    build_header(buf, LAGFX_OP_MAP_MEMORY2, 0,
                 /*total_length=*/48, /*stamp=*/0xb0000002u);
    put_le32(buf + 12, 42u);              /* taskID */
    put_le64(buf + 16, 0x2000ull);        /* virtualOffset */
    put_le32(buf + 24, 1u);               /* readOnly */
    put_le32(buf + 28, 1u);               /* rangeCount */
    put_le64(buf + 32, 0xcafed000ull);    /* ranges[0].gpa */
    put_le64(buf + 40, 0x1000ull);        /* ranges[0].length */

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdMapMemory2(1 range) returns OK");
    CHECK(shell.map_memory_count == 1,
          "CmdMapMemory2 invoked shell.map_memory exactly once");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xb0000002u,
          "CmdMapMemory2 stamp propagated to cell");

    /* Empty-ranges case (count=0) — still completes, NO shell call. */
    uint8_t empty[32];
    build_header(empty, LAGFX_OP_MAP_MEMORY2, 0,
                 /*total_length=*/32, /*stamp=*/0xb0000003u);
    put_le32(empty + 12, 42u);       /* taskID */
    put_le64(empty + 16, 0x4000ull); /* virtualOffset */
    put_le32(empty + 24, 0u);        /* readOnly */
    put_le32(empty + 28, 0u);        /* rangeCount=0 */
    rc = lagfx_protocol_dispatch_one(p, empty, sizeof(empty));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdMapMemory2(count=0) returns OK");
    CHECK(shell.map_memory_count == 1,
          "CmdMapMemory2(count=0) does not invoke shell.map_memory");

    /* Multi-range: 3 ranges. payload=20 + 3*16 = 68, total=80. */
    uint8_t multi[80];
    build_header(multi, LAGFX_OP_MAP_MEMORY2, 0, 80, 0xb0000004u);
    put_le32(multi + 12, 42u);
    put_le64(multi + 16, 0x10000ull);
    put_le32(multi + 24, 0u);
    put_le32(multi + 28, 3u);
    for (int i = 0; i < 3; ++i) {
        put_le64(multi + 32 + i * 16,     0x100000ull + (uint64_t)i * 0x1000ull);
        put_le64(multi + 32 + i * 16 + 8, 0x1000ull);
    }
    rc = lagfx_protocol_dispatch_one(p, multi, sizeof(multi));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdMapMemory2(3 ranges) returns OK");
    CHECK(shell.map_memory_count == 2,
          "CmdMapMemory2(3 ranges) produced one shell.map_memory call "
          "(batched; callback internally iterates)");

    /* Malformed: rangeCount=5 but payload only holds 1 range — reject. */
    uint8_t bad[36];
    build_header(bad, LAGFX_OP_MAP_MEMORY2, 0, 36, 0xb0000005u);
    put_le32(bad + 12, 42u);
    put_le64(bad + 16, 0ull);
    put_le32(bad + 24, 0u);
    put_le32(bad + 28, 5u);        /* claims 5 ranges */
    put_le64(bad + 32, 0xdeadull); /* only 8 bytes follow (< 1 range) */
    /* Actually we have 4 bytes of room past rangeCount; header padding
     * above only covered the first 4 bytes of a u64 and the overflow
     * check must reject. Total payload = 36 - 12 = 24 bytes; count=5
     * demands 20 + 80 = 100. */
    rc = lagfx_protocol_dispatch_one(p, bad, sizeof(bad));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdMapMemory2(count > payload capacity) rejected as ERR_SIZE");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xb0000005u,
          "CmdMapMemory2 size-mismatch still signals stamp (fail-open)");

    lagfx_device_free(dev);
}

static void test_unmap_memory_handler(void) {
    fprintf(stdout, "\n--- test: unmap_memory_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define a task. */
    uint8_t dt[36];
    build_header(dt, LAGFX_OP_DEFINE_TASK2, 0, 36, 0xc0000001u);
    put_le32(dt + 12, 99u);
    put_le64(dt + 16, 0ull);
    put_le64(dt + 24, 0x10000ull);
    put_le32(dt + 32, 0u);
    lagfx_protocol_dispatch_one(p, dt, sizeof(dt));

    /* CmdUnmapMemory: taskID=99, virtualOffset=0x3000, length=0x2000.
     * payload=20, total=32. */
    uint8_t buf[32];
    build_header(buf, LAGFX_OP_UNMAP_MEMORY, 0, 32, 0xc0000002u);
    put_le32(buf + 12, 99u);
    put_le64(buf + 16, 0x3000ull);
    put_le64(buf + 24, 0x2000ull);

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdUnmapMemory returns OK");
    CHECK(shell.unmap_memory_count == 1,
          "CmdUnmapMemory invoked shell.unmap_memory exactly once");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xc0000002u,
          "CmdUnmapMemory stamp propagated");

    /* Unknown taskID is fail-open: handler still calls shell.unmap_memory
     * with NULL task handle, stamp signals. */
    build_header(buf, LAGFX_OP_UNMAP_MEMORY, 0, 32, 0xc0000003u);
    put_le32(buf + 12, 123u); /* not defined */
    put_le64(buf + 16, 0x0ull);
    put_le64(buf + 24, 0x1000ull);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdUnmapMemory(unknown task) still OK (fail-open)");
    CHECK(shell.unmap_memory_count == 2,
          "CmdUnmapMemory(unknown task) still reaches shell");

    /* Truncated payload — reject. */
    uint8_t short_buf[24];
    build_header(short_buf, LAGFX_OP_UNMAP_MEMORY, 0, 24, 0xc0000004u);
    put_le32(short_buf + 12, 99u);
    /* only 8 bytes remain, not 16 for (u64 vm_off, u64 len). But
     * dispatcher's min_payload check traps it before the handler — 20
     * bytes required, we supplied 12. Expect ERR_SIZE. */
    rc = lagfx_protocol_dispatch_one(p, short_buf, sizeof(short_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdUnmapMemory short payload rejected");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xc0000004u,
          "CmdUnmapMemory short payload still signals stamp");

    lagfx_device_free(dev);
}

static void test_exec_indirect2_empty(void) {
    fprintf(stdout, "\n--- test: exec_indirect2_empty ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Truly empty (header-only) ExecIndirect2 — descriptor has
     * min_payload=0, so even a 12-byte command should dispatch
     * successfully and complete the stamp. This is the fallback that
     * metal-no-op may use in place of CmdSynchronizeResources. */
    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_EXEC_INDIRECT2, 0,
                 /*total_length=*/12, /*stamp=*/0xd0000001u);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdExecIndirect2(header-only) returns OK");
    CHECK(shell.raise_irq_count == 1,
          "CmdExecIndirect2(header-only) raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd0000001u,
          "CmdExecIndirect2(header-only) stamp signalled");

    /* Explicit zero-record outer payload in the M4 layout (per
     * src/protocol/ops_cmdbuf.c §"Outer payload layout"):
     *
     *   +0x00  u32 task_id
     *   +0x04  u32 invalidate_count = 0
     *   +0x08  u32 resource_count   = 0
     *   +0x0c  16B reserved/middle block
     *
     * Total outer payload = 28 bytes; total command = 12 + 28 = 40. */
    uint8_t zero[40] = {0};
    build_header(zero, LAGFX_OP_EXEC_INDIRECT2, 0, 40, 0xd0000002u);
    put_le32(zero + 12, 1u);  /* taskID */
    put_le32(zero + 16, 0u);  /* invalidate_count */
    put_le32(zero + 20, 0u);  /* resource_count */
    /* zero[24..39] left as zeros for the 16B middle/reserved block. */
    rc = lagfx_protocol_dispatch_one(p, zero, sizeof(zero));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdExecIndirect2(count=0) returns OK");
    CHECK(shell.raise_irq_count == 2,
          "CmdExecIndirect2(count=0) raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd0000002u,
          "CmdExecIndirect2(count=0) stamp signalled");

    lagfx_device_free(dev);
}

/* test_exec_indirect2_inner_dispatch_smoke + test_exec_indirect2_unknown_inner
 * were removed alongside the Phase 3.A inner-opcode dispatch scaffold
 * (lagfx_process_inner + per-inner stubs + LAGFX_INNER_* enum +
 * inner_opcodes_* counters). The M4 segment walker in
 * lagfx_op_exec_indirect2 superseded that path; new wire-format coverage
 * lands as part of the M4 work in flight. */

static void test_metal_no_op_sequence(void) {
    fprintf(stdout, "\n--- test: metal_no_op_sequence ---\n");

    /* Mini integration: issue the exact opcode sequence that a successful
     * [cmdbuf commit] on empty cmdbuf is expected to produce per
     * phase-1a2-decoder-plan.md §1.2:
     *
     *   CmdGetDeviceInfo → CmdDefineTask2 → CmdDefineChildFIFO →
     *   CmdSynchronizeResources(count=0) →
     *   CmdDeleteChildFIFO → CmdDeleteTask
     *
     * Also exercises CmdExecIndirect2(count=0) as the alternate empty
     * cmdbuf completion path (per re-followup R2 / plan §6.2).
     *
     * Asserts shell callback counts match end-to-end expectations. */
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 1. CmdGetDeviceInfo(key=0). */
    uint8_t gdi[24];
    build_header(gdi, LAGFX_OP_GET_DEVICE_INFO, 0, 24, 0xe2e00001u);
    put_le32(gdi + 12, 0u);
    put_le32(gdi + 16, 0u);
    put_le32(gdi + 20, 0u);
    lagfx_protocol_dispatch_one(p, gdi, sizeof(gdi));

    /* 2. CmdDefineTask2(taskID=1, length=4KB). */
    uint8_t dt[36];
    build_header(dt, LAGFX_OP_DEFINE_TASK2, 0, 36, 0xe2e00002u);
    put_le32(dt + 12, 1u);
    put_le64(dt + 16, 0u);
    put_le64(dt + 24, 0x1000ull);
    put_le32(dt + 32, 0u);
    lagfx_protocol_dispatch_one(p, dt, sizeof(dt));

    /* 3. CmdDefineChildFIFO(fifoID=1). */
    uint8_t dcf[16];
    build_header(dcf, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0xe2e00003u);
    put_le32(dcf + 12, 1u);
    lagfx_protocol_dispatch_one(p, dcf, sizeof(dcf));

    /* 4. CmdSynchronizeResources(count=0) — the "commit empty cmdbuf"
     *    completion path.
     *
     *    Phase 1 end-to-end note: when a real lagfx_device is attached
     *    (as here — make_dev calls lagfx_device_new), this handler's
     *    count=0 branch also invokes lagfx_vk_submit_empty() on the
     *    device's VkQueue. On Darwin dev hosts with no loadable ICD,
     *    LAGFX_HAVE_VULKAN is unset so the call lands in the no-op
     *    stub that returns LAGFX_OK; on Linux with Mesa lavapipe, a
     *    real fence-gated empty VkSubmit runs end-to-end. This mock
     *    test cannot assert on Vulkan state (no VkDevice introspection
     *    here), but the code path is exercised — a regression in the
     *    decoder→vk wiring would show up as an abort or as additional
     *    LAGFX_LOG spam in test output. */
    uint8_t sync[20];
    build_header(sync, LAGFX_OP_SYNCHRONIZE_RESOURCES, 0, 20, 0xe2e00004u);
    put_le32(sync + 12, 1u); /* taskID=1 (known) */
    put_le32(sync + 16, 0u); /* count=0 */
    lagfx_protocol_dispatch_one(p, sync, sizeof(sync));

    /* 4b. Alternate empty-cmdbuf completion path: CmdExecIndirect2
     *     with count=0. Should complete cleanly, symmetric to 0x22.
     *     (Per re-followup R2 / plan §6.2 this is the documented
     *     alternate path that metal-no-op may take.)
     *
     *     Same Phase 1 end-to-end note as step 4 applies — this path
     *     also drives lagfx_vk_submit_empty() when a device is attached. */
    uint8_t exi[20];
    build_header(exi, LAGFX_OP_EXEC_INDIRECT2, 0, 20, 0xe2e0004au);
    put_le32(exi + 12, 1u);
    put_le32(exi + 16, 0u);
    lagfx_protocol_dispatch_one(p, exi, sizeof(exi));

    /* 5. CmdDeleteChildFIFO(fifoID=1). */
    uint8_t ddf[16];
    build_header(ddf, LAGFX_OP_DELETE_CHILD_FIFO, 0, 16, 0xe2e00005u);
    put_le32(ddf + 12, 1u);
    lagfx_protocol_dispatch_one(p, ddf, sizeof(ddf));

    /* 6. CmdDeleteTask(taskID=1). */
    uint8_t drt[16];
    build_header(drt, LAGFX_OP_DELETE_TASK, 0, 16, 0xe2e00006u);
    put_le32(drt + 12, 1u);
    lagfx_protocol_dispatch_one(p, drt, sizeof(drt));

    /* Shell call tallies. */
    CHECK(shell.create_task_count == 1,
          "metal-no-op sequence: exactly one shell.create_task");
    CHECK(shell.destroy_task_count == 1,
          "metal-no-op sequence: exactly one shell.destroy_task");
    CHECK(shell.raise_irq_count == 7,
          "metal-no-op sequence: one IRQ per command (7 incl. ExecIndirect2)");

    /* Final stamp is DeleteTask's stamp. */
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xe2e00006u,
          "metal-no-op last stamp matches DeleteTask");

    /* Tables empty again after teardown. */
    CHECK(lagfx_protocol_find_task(p, 1u) == NULL,
          "task 1 removed after sequence");
    CHECK(lagfx_protocol_find_fifo(p, 1u) == NULL,
          "fifo 1 removed after sequence");

    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(seen == 7, "metal-no-op: 7 commands seen");
    CHECK(completed == 7, "metal-no-op: 7 commands completed");
    CHECK(unknown == 0, "metal-no-op: no unknown opcodes");

    lagfx_device_free(dev);
}

/* === Phase 2.A display-path handler tests (PARTIAL-layout) ====
 *
 * CmdDisplayAck (0x10), CmdDisplaySwapMapping (0x12),
 * CmdDisplayTransaction3 (0x16) — see phase-2-first-pixel-plan.md §4
 * and src/protocol/ops_display.c for the assumed payload shapes. All
 * three opcodes are layout-PARTIAL (confirmable by runtime capture on
 * a booted VM); tests assert against the current handler's
 * interpretation. If a future RE revision changes the layout, these
 * tests are the canonical place to update.
 *
 * f32-on-wire helper — little-endian IEEE 754. */
static void put_lef32(uint8_t *b, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    put_le32(b, u);
}

static void test_display_ack_handler(void) {
    fprintf(stdout, "\n--- test: display_ack_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 1. Ack against an unknown displayID — fail-open, returns OK,
     *    stamp signalled, no state changes. */
    uint8_t ack[20];
    build_header(ack, LAGFX_OP_DISPLAY_ACK, 0,
                 /*total_length=*/20, /*stamp=*/0xda010001u);
    put_le32(ack + 12, 1u);  /* displayID (not yet registered) */
    put_le32(ack + 16, 99u); /* frameID */

    int rc = lagfx_protocol_dispatch_one(p, ack, sizeof(ack));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayAck(unknown displayID) returns OK (fail-open)");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xda010001u,
          "CmdDisplayAck stamp signalled on unknown displayID");
    CHECK(shell.raise_irq_count == 1,
          "CmdDisplayAck raised completion IRQ on unknown displayID");

    /* 2. Register a display via SwapMapping, submit a transaction,
     *    then ack it — expect transaction_pending=false and
     *    transaction_acked=true on the entry. */
    uint8_t swap[52];
    build_header(swap, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0,
                 /*total_length=*/52, /*stamp=*/0xda010002u);
    put_le32(swap + 12, 1u);          /* displayID */
    put_le32(swap + 16, 7u);          /* mappingID */
    put_le64(swap + 20, 0xfb000000ull); /* bufferVA */
    put_le64(swap + 28, 0x7e9000ull);   /* length (1920*1080*4) */
    put_le32(swap + 36, 1920u);       /* width */
    put_le32(swap + 40, 1080u);       /* height */
    put_le32(swap + 44, 7680u);       /* stride */
    put_le32(swap + 48, 0u);          /* format BGRA8 */
    rc = lagfx_protocol_dispatch_one(p, swap, sizeof(swap));
    CHECK(rc == LAGFX_HANDLER_OK, "pre-ack: DisplaySwapMapping OK");

    uint8_t tx[12 + 12 + 32];
    build_header(tx, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 /*total_length=*/sizeof(tx), /*stamp=*/0xda010003u);
    put_le32(tx + 12, 1u);  /* displayID */
    put_le32(tx + 16, 42u); /* transactionID */
    put_le32(tx + 20, 1u);  /* attachmentCount */
    /* attachment 0: idx=0 load=clear(2) store=store(1) flags=0 rgba=red */
    put_le32(tx + 24,  0u);
    put_le32(tx + 28,  2u);
    put_le32(tx + 32,  1u);
    put_le32(tx + 36,  0u);
    put_lef32(tx + 40, 1.f);
    put_lef32(tx + 44, 0.f);
    put_lef32(tx + 48, 0.f);
    put_lef32(tx + 52, 1.f);
    rc = lagfx_protocol_dispatch_one(p, tx, sizeof(tx));
    CHECK(rc == LAGFX_HANDLER_OK, "pre-ack: DisplayTransaction3 OK");

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 1u);
    CHECK(d != NULL && d->transaction_pending,
          "pre-ack: display has transaction_pending=true");
    CHECK(d != NULL && d->pending_transaction_id == 42u,
          "pre-ack: pending transactionID captured");

    /* 3. Now the ack for frameID=42 (matches pending). */
    build_header(ack, LAGFX_OP_DISPLAY_ACK, 0,
                 /*total_length=*/20, /*stamp=*/0xda010004u);
    put_le32(ack + 12, 1u);
    put_le32(ack + 16, 42u);
    rc = lagfx_protocol_dispatch_one(p, ack, sizeof(ack));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayAck(matching frameID) returns OK");
    d = lagfx_protocol_find_display(p, 1u);
    CHECK(d != NULL && !d->transaction_pending,
          "CmdDisplayAck cleared transaction_pending on match");
    CHECK(d != NULL && d->transaction_acked,
          "CmdDisplayAck set transaction_acked=true on match");

    /* 4. Short payload — reject with ERR_SIZE (the dispatcher's
     *    min_payload check catches it before the handler body). */
    uint8_t short_buf[16];
    build_header(short_buf, LAGFX_OP_DISPLAY_ACK, 0,
                 /*total_length=*/16, /*stamp=*/0xda010005u);
    put_le32(short_buf + 12, 1u);
    rc = lagfx_protocol_dispatch_one(p, short_buf, sizeof(short_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplayAck short payload rejected");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xda010005u,
          "CmdDisplayAck short payload still signals stamp (fail-open)");

    lagfx_device_free(dev);
}

static void test_display_swap_mapping_handler(void) {
    fprintf(stdout, "\n--- test: display_swap_mapping_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Initial swap — auto-register displayID=2. */
    uint8_t swap[52];
    build_header(swap, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0,
                 /*total_length=*/52, /*stamp=*/0xd2020001u);
    put_le32(swap + 12, 2u);          /* displayID */
    put_le32(swap + 16, 1u);          /* mappingID */
    put_le64(swap + 20, 0xabcd0000ull); /* bufferVA */
    put_le64(swap + 28, 0x10000ull);    /* length */
    put_le32(swap + 36, 1920u);
    put_le32(swap + 40, 1080u);
    put_le32(swap + 44, 1920u * 4u);
    put_le32(swap + 48, 0u);

    int rc = lagfx_protocol_dispatch_one(p, swap, sizeof(swap));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplaySwapMapping(displayID=2) returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd2020001u,
          "CmdDisplaySwapMapping stamp propagated");

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 2u);
    CHECK(d != NULL, "display table contains displayID=2 after swap");
    CHECK(d != NULL && d->live, "display entry marked live");
    CHECK(d != NULL && d->mapped, "display entry marked mapped");
    CHECK(d != NULL && d->mapping_id == 1u, "mappingID recorded");
    CHECK(d != NULL && d->buffer_va == 0xabcd0000ull,
          "bufferVA recorded");
    CHECK(d != NULL && d->length == 0x10000ull,
          "length recorded");
    CHECK(d != NULL && d->width == 1920u && d->height == 1080u,
          "geometry recorded (1920x1080)");
    CHECK(d != NULL && d->stride == 7680u,
          "stride recorded (1920*4)");
    CHECK(d != NULL && d->format == 0u,
          "format recorded (BGRA8)");

    /* Second swap on the same display — update, not allocate new slot.
     * Count of live slots must stay at 1. */
    build_header(swap, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0,
                 /*total_length=*/52, /*stamp=*/0xd2020002u);
    put_le32(swap + 12, 2u);
    put_le32(swap + 16, 2u);            /* new mappingID */
    put_le64(swap + 20, 0xdead0000ull); /* new bufferVA */
    put_le64(swap + 28, 0x20000ull);
    put_le32(swap + 36, 1440u);
    put_le32(swap + 40, 900u);
    put_le32(swap + 44, 5760u);
    put_le32(swap + 48, 0u);
    rc = lagfx_protocol_dispatch_one(p, swap, sizeof(swap));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdDisplaySwapMapping(second) OK");

    d = lagfx_protocol_find_display(p, 2u);
    CHECK(d != NULL && d->mapping_id == 2u,
          "second swap updated mappingID");
    CHECK(d != NULL && d->buffer_va == 0xdead0000ull,
          "second swap updated bufferVA");
    CHECK(d != NULL && d->width == 1440u,
          "second swap updated width to 1440");

    /* Count live slots — should be exactly one (no leak). */
    unsigned live = 0;
    for (unsigned i = 0; i < LAGFX_PROTO_MAX_DISPLAYS; ++i) {
        if (p->displays[i].live) live++;
    }
    CHECK(live == 1, "only one display slot live after two swaps on same ID");

    /* Table-full test: swap four distinct displayIDs to fill the table
     * (LAGFX_PROTO_MAX_DISPLAYS=4), then try a fifth — expect
     * ERR_STATE but stamp still signals. */
    for (uint32_t i = 10; i < 10u + LAGFX_PROTO_MAX_DISPLAYS; ++i) {
        build_header(swap, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0, 52,
                     (uint32_t)(0xd2020100u + i));
        put_le32(swap + 12, i);
        put_le32(swap + 16, 0u);
        put_le64(swap + 20, 0ull);
        put_le64(swap + 28, 0ull);
        put_le32(swap + 36, 0u);
        put_le32(swap + 40, 0u);
        put_le32(swap + 44, 0u);
        put_le32(swap + 48, 0u);
        rc = lagfx_protocol_dispatch_one(p, swap, sizeof(swap));
        if (i == 10u + LAGFX_PROTO_MAX_DISPLAYS - 1) {
            /* Fourth unique ID, with slot #2 already live —
             * table is now full. */
            CHECK(rc == LAGFX_HANDLER_ERR_STATE || rc == LAGFX_HANDLER_OK,
                  "display table at/near capacity handled");
        }
    }

    /* Short payload rejected. */
    uint8_t short_buf[32];
    build_header(short_buf, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0,
                 /*total_length=*/32, /*stamp=*/0xd2020fffu);
    put_le32(short_buf + 12, 99u);
    rc = lagfx_protocol_dispatch_one(p, short_buf, sizeof(short_buf));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplaySwapMapping short payload rejected");

    lagfx_device_free(dev);
}

static void test_display_transaction3_handler(void) {
    fprintf(stdout, "\n--- test: display_transaction3_handler ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 1. Transaction with one clear-colour attachment (red).
     *    payload = 12 (hdr) + 12 (disp,tx,count) + 32 (attach) = 56 total. */
    uint8_t tx[12 + 12 + 32];
    build_header(tx, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 /*total_length=*/sizeof(tx), /*stamp=*/0xd3030001u);
    put_le32(tx + 12, 3u);   /* displayID (auto-register) */
    put_le32(tx + 16, 101u); /* transactionID */
    put_le32(tx + 20, 1u);   /* attachmentCount */
    put_le32(tx + 24, 0u);   /* attachmentIndex */
    put_le32(tx + 28, 2u);   /* loadAction = Clear */
    put_le32(tx + 32, 1u);   /* storeAction = Store */
    put_le32(tx + 36, 0u);   /* flags */
    put_lef32(tx + 40, 1.0f); /* R */
    put_lef32(tx + 44, 0.0f); /* G */
    put_lef32(tx + 48, 0.0f); /* B */
    put_lef32(tx + 52, 1.0f); /* A */

    int rc = lagfx_protocol_dispatch_one(p, tx, sizeof(tx));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayTransaction3(1-attach clear) returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd3030001u,
          "CmdDisplayTransaction3 stamp propagated");

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 3u);
    CHECK(d != NULL, "display table contains displayID=3 after txn");
    CHECK(d != NULL && d->live, "display auto-registered");
    CHECK(d != NULL && d->transaction_pending,
          "display marked transaction_pending");
    CHECK(d != NULL && !d->transaction_acked,
          "transaction_acked false until ack arrives");
    CHECK(d != NULL && d->pending_transaction_id == 101u,
          "pending_transaction_id captured");
    CHECK(d != NULL && d->last_attachment_count == 1u,
          "last_attachment_count captured");
    CHECK(d != NULL && d->last_load_action == 2u,
          "last_load_action == 2 (clear)");
    CHECK(d != NULL && d->last_clear_rgba[0] == 1.f,
          "last_clear_rgba[0] = 1.0 (red)");
    CHECK(d != NULL && d->last_clear_rgba[1] == 0.f,
          "last_clear_rgba[1] = 0.0");
    CHECK(d != NULL && d->last_clear_rgba[2] == 0.f,
          "last_clear_rgba[2] = 0.0");
    CHECK(d != NULL && d->last_clear_rgba[3] == 1.f,
          "last_clear_rgba[3] = 1.0 (opaque)");

    /* 2. attachmentCount=0 — valid (header-only txn). */
    uint8_t tx_empty[24];
    build_header(tx_empty, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 /*total_length=*/24, /*stamp=*/0xd3030002u);
    put_le32(tx_empty + 12, 3u);
    put_le32(tx_empty + 16, 102u);
    put_le32(tx_empty + 20, 0u); /* attachmentCount=0 */
    rc = lagfx_protocol_dispatch_one(p, tx_empty, sizeof(tx_empty));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayTransaction3(count=0) returns OK");

    /* 3. Size-mismatch: payload_size doesn't match either shape.
     *    Post-§14.3.3 update the decoder derives the entry count from
     *    payload size (12 + 32*n for legacy, 16 + 44*n for layer
     *    form). A payload of 30 bytes fits neither — decoder errors
     *    rather than risk a ring-pointer desync. */
    uint8_t tx_bad[30];
    build_header(tx_bad, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 /*total_length=*/30, /*stamp=*/0xd3030003u);
    put_le32(tx_bad + 12, 3u);
    put_le32(tx_bad + 16, 103u);
    put_le32(tx_bad + 20, 0u);  /* declared count ignored — size rules */
    /* pad bytes 24..29 with garbage so size != 12+0*32 = 12 */
    memset(tx_bad + 24, 0xaa, 6);
    rc = lagfx_protocol_dispatch_one(p, tx_bad, sizeof(tx_bad));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplayTransaction3(ambiguous size) rejected");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd3030003u,
          "CmdDisplayTransaction3 size-mismatch still signals stamp");

    lagfx_device_free(dev);
}

static void test_metal_clear_color_sequence(void) {
    fprintf(stdout, "\n--- test: metal_clear_color_sequence ---\n");

    /* Phase 2.A mini integration: the full wire sequence for a
     * metal-clear-screen frame, per phase-2-first-pixel-plan.md §4:
     *
     *   CmdDefineTask2      → register task (cmdbuf backing)
     *   CmdMapMemory2       → bind guest range to host memory
     *   CmdDisplaySwapMapping → scanout target
     *   CmdDisplayTransaction3 → clear-colour transaction
     *   CmdDisplayAck       → guest closes the loop
     *   CmdSynchronizeResources(count=0) → commit completion
     *
     * Note: Phase 3's CmdExecIndirect2 nested-draw-stream path
     * (inner-opcode dispatch — see test_exec_indirect2_inner_dispatch_smoke)
     * is NOT exercised here. Phase 2's clear-colour path takes the
     * display transaction route, not the indirect-exec route; Phase 3
     * will introduce a new end-to-end test once the inner-opcode
     * RE spike confirms the real wire format (phase-3-metal-vulkan-plan.md
     * §R3.6).
     *
     * Assertions focus on state-transition observables:
     *   - one shell.create_task
     *   - one shell.map_memory
     *   - one IRQ per command (6 total)
     *   - display table holds one live entry at end
     *   - display's transaction_acked=true after the ack
     *   - clear-colour on the display entry matches (1,0,0,1) */
    mock_shell_t shell = {0};
    /* M4 GAP #1: capture the DMA writeback to the SwapMapping buffer_va
     * (see build_header for swap[] below — bufferVA = 0x1000000). Back
     * it with a 16 KiB scanout buffer sized for the 64x64 BGRA8 render
     * target so we can inspect the pixel that landed at guest GPA. */
    const uint64_t test_scanout_gpa = 0x1000000ull;
    const uint64_t test_scanout_cap = 64ull * 64ull * 4ull;
    uint8_t test_scanout_buf[64u * 64u * 4u];
    memset(test_scanout_buf, 0, sizeof(test_scanout_buf));
    shell.scanout_gpa      = test_scanout_gpa;
    shell.scanout_capacity = test_scanout_cap;
    shell.scanout_buf      = test_scanout_buf;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Phase 2.B: attach a display so lagfx_op_display_transaction3 has
     * a lagfx_display_t to trigger the clear-colour render + readback
     * path against. On a non-Vulkan build this short-circuits to
     * "new_frame_ready=true"; on a Vulkan build it submits a real
     * VkClear + readback. Either way we assert the flag + read_frame
     * semantics below. */
    static const lagfx_display_mode_t modes[] = {
        { 64u, 64u, 60u },
    };
    lagfx_display_descriptor_t disp_desc;
    memset(&disp_desc, 0, sizeof(disp_desc));
    disp_desc.name = "test clear display";
    disp_desc.modes = modes;
    disp_desc.mode_count = 1u;
    char *derr = NULL;
    lagfx_display_t *display =
        lagfx_display_new(dev, &disp_desc, 1u, 1u, &derr);
    CHECK(display != NULL, "Phase 2.B: display attached for clear-color test");
    free(derr);

    /* 1. DefineTask2(taskID=5, length=64K) */
    uint8_t dt[36];
    build_header(dt, LAGFX_OP_DEFINE_TASK2, 0, 36, 0xc10c0001u);
    put_le32(dt + 12, 5u);
    put_le64(dt + 16, 0ull);
    put_le64(dt + 24, 0x10000ull);
    put_le32(dt + 32, 0u);
    lagfx_protocol_dispatch_one(p, dt, sizeof(dt));

    /* 2. MapMemory2(taskID=5, range 0..0x1000) */
    uint8_t mm[48];
    build_header(mm, LAGFX_OP_MAP_MEMORY2, 0, 48, 0xc10c0002u);
    put_le32(mm + 12, 5u);
    put_le64(mm + 16, 0ull);
    put_le32(mm + 24, 0u);
    put_le32(mm + 28, 1u);
    put_le64(mm + 32, 0x100000ull);
    put_le64(mm + 40, 0x1000ull);
    lagfx_protocol_dispatch_one(p, mm, sizeof(mm));

    /* 3. DisplaySwapMapping(displayID=1) */
    uint8_t swap[52];
    build_header(swap, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0, 52, 0xc10c0003u);
    put_le32(swap + 12, 1u);
    put_le32(swap + 16, 1u);
    put_le64(swap + 20, 0x1000000ull);
    put_le64(swap + 28, 0x7e9000ull);
    put_le32(swap + 36, 1920u);
    put_le32(swap + 40, 1080u);
    put_le32(swap + 44, 7680u);
    put_le32(swap + 48, 0u);
    lagfx_protocol_dispatch_one(p, swap, sizeof(swap));

    /* 4. DisplayTransaction3(displayID=1, txID=77, clear red). */
    uint8_t tx[12 + 12 + 32];
    build_header(tx, LAGFX_OP_DISPLAY_TRANSACTION3, 0, sizeof(tx),
                 0xc10c0004u);
    put_le32(tx + 12, 1u);
    put_le32(tx + 16, 77u);
    put_le32(tx + 20, 1u);
    put_le32(tx + 24,  0u);
    put_le32(tx + 28,  2u); /* clear */
    put_le32(tx + 32,  1u);
    put_le32(tx + 36,  0u);
    put_lef32(tx + 40, 1.0f);
    put_lef32(tx + 44, 0.0f);
    put_lef32(tx + 48, 0.0f);
    put_lef32(tx + 52, 1.0f);
    lagfx_protocol_dispatch_one(p, tx, sizeof(tx));

    /* 5. DisplayAck(displayID=1, frameID=77). */
    uint8_t ack[20];
    build_header(ack, LAGFX_OP_DISPLAY_ACK, 0, 20, 0xc10c0005u);
    put_le32(ack + 12, 1u);
    put_le32(ack + 16, 77u);
    lagfx_protocol_dispatch_one(p, ack, sizeof(ack));

    /* 6. SynchronizeResources(count=0) — cmdbuf commit completion. */
    uint8_t sync[20];
    build_header(sync, LAGFX_OP_SYNCHRONIZE_RESOURCES, 0, 20,
                 0xc10c0006u);
    put_le32(sync + 12, 5u);
    put_le32(sync + 16, 0u);
    lagfx_protocol_dispatch_one(p, sync, sizeof(sync));

    /* Shell callback tallies. */
    CHECK(shell.create_task_count == 1,
          "clear-color sequence: one create_task");
    CHECK(shell.map_memory_count == 1,
          "clear-color sequence: one map_memory");
    CHECK(shell.raise_irq_count == 6,
          "clear-color sequence: one IRQ per command (6 total)");

    /* Display state: single live entry, clear=red, acked. */
    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 1u);
    CHECK(d != NULL && d->live, "display 1 live after sequence");
    CHECK(d != NULL && d->mapped,
          "display 1 mapped (SwapMapping landed)");
    CHECK(d != NULL && d->transaction_acked,
          "display 1 transaction_acked after DisplayAck");
    CHECK(d != NULL && !d->transaction_pending,
          "display 1 transaction_pending cleared after ack");
    CHECK(d != NULL && d->last_load_action == 2u,
          "display 1 last_load_action == clear");
    CHECK(d != NULL && d->last_clear_rgba[0] == 1.f
                    && d->last_clear_rgba[1] == 0.f
                    && d->last_clear_rgba[2] == 0.f
                    && d->last_clear_rgba[3] == 1.f,
          "display 1 clear colour == (1,0,0,1) red");

    /* Counters. */
    CHECK(p->display_swaps_applied == 1,
          "display_swaps_applied counter == 1");
    CHECK(p->display_transactions_submitted == 1,
          "display_transactions_submitted counter == 1");
    CHECK(p->display_acks_received == 1,
          "display_acks_received counter == 1");

    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(seen == 6, "clear-color sequence: 6 commands seen");
    CHECK(completed == 6, "clear-color sequence: 6 commands completed");
    CHECK(unknown == 0, "clear-color sequence: no unknown opcodes");

    /* Phase 2.B: DisplayTransaction3 with loadAction=clear must have
     * flipped the display's new_frame_ready flag. read_frame clears it
     * on consumption — second call returns NO_FRAME. On a non-Vulkan
     * build read_frame returns NO_FRAME even on the first call (the
     * flag gets cleared as part of the no-backend path). */
    CHECK(display != NULL && display->new_frame_ready,
          "Phase 2.B: new_frame_ready set after clear transaction");

    /* M4 GAP #1 closure assertion: after CmdDisplayTransaction3
     * fence-waits on the clear, display_submit_clear_color DMAs the
     * readback into shell.write_memory at the SwapMapping-captured
     * scanout GPA. On a non-Vulkan (or no-loadable-ICD) build this
     * path is skipped — so the callback count is only expected to
     * bump when LAGFX_HAVE_VULKAN is set AND the device brought up a
     * live vk instance. We gate the assertion on shell.scanout_buf
     * being populated (i.e. the mock actually captured a write in the
     * expected range). */
#ifdef LAGFX_HAVE_VULKAN
    if (dev->vk && dev->vk->initialized) {
        CHECK(shell.write_memory_count >= 1,
              "M4: DMA writeback fired at least once (scanout VA)");
        CHECK(shell.last_write_gpa == 0x1000000ull,
              "M4: DMA writeback targeted SwapMapping buffer_va");
        /* 64x64 BGRA8 = 16 KiB. */
        CHECK(shell.last_write_len == 64u * 64u * 4u,
              "M4: DMA writeback size == rt width*height*4");
        if (shell.scanout_buf) {
            size_t off = (size_t)32u * (size_t)(64u * 4u)
                       + (size_t)32u * 4u;
            CHECK(shell.scanout_buf[off + 0] == 0u,
                  "M4: scanout GPA pixel (32,32) B == 0");
            CHECK(shell.scanout_buf[off + 1] == 0u,
                  "M4: scanout GPA pixel (32,32) G == 0");
            CHECK(shell.scanout_buf[off + 2] == 255u,
                  "M4: scanout GPA pixel (32,32) R == 255");
            CHECK(shell.scanout_buf[off + 3] == 255u,
                  "M4: scanout GPA pixel (32,32) A == 255");
        }
    } else {
        CHECK(shell.write_memory_count == 0,
              "M4: DMA writeback skipped when no Vulkan ICD");
    }
#else
    CHECK(shell.write_memory_count == 0,
          "M4: DMA writeback skipped on no-Vulkan build");
#endif

    size_t stride_out = 0;
    bool   new_frame  = false;
    /* 64x64 BGRA8 = 16 KiB. */
    uint8_t frame_buf[64u * 64u * 4u];
    lagfx_status_t rf_st = lagfx_display_read_frame(
        display, frame_buf, sizeof(frame_buf), &stride_out, &new_frame);
#ifdef LAGFX_HAVE_VULKAN
    /* With a live Vulkan init the readback should succeed and the
     * centre pixel should be red in BGRA byte order. */
    if (dev->vk && dev->vk->initialized) {
        CHECK(rf_st == LAGFX_OK,
              "Phase 2.B: read_frame returns LAGFX_OK after clear");
        CHECK(new_frame == true,
              "Phase 2.B: new_frame=true on first read after clear");
        CHECK(stride_out == 64u * 4u,
              "Phase 2.B: read_frame stride == width*4");
        size_t off = (size_t)32u * stride_out + (size_t)32u * 4u;
        CHECK(frame_buf[off + 0] == 0u   &&
              frame_buf[off + 1] == 0u   &&
              frame_buf[off + 2] == 255u &&
              frame_buf[off + 3] == 255u,
              "Phase 2.B: centre pixel is red BGRA (0,0,255,255)");
    } else {
        CHECK(rf_st == LAGFX_ERR_NO_FRAME,
              "Phase 2.B: read_frame NO_FRAME with uninit Vulkan");
    }
#else
    CHECK(rf_st == LAGFX_ERR_NO_FRAME,
          "Phase 2.B: read_frame NO_FRAME on no-Vulkan build");
#endif

    /* Second call always returns NO_FRAME (one-shot latch). */
    rf_st = lagfx_display_read_frame(display, frame_buf, sizeof(frame_buf),
                                     &stride_out, &new_frame);
    CHECK(rf_st == LAGFX_ERR_NO_FRAME,
          "Phase 2.B: read_frame NO_FRAME on second call (one-shot latch)");
    CHECK(new_frame == false,
          "Phase 2.B: new_frame_out=false on second call");

    lagfx_display_free(display);
    lagfx_device_free(dev);
}

/* === M3 plumbing tests (Phase 0 follow-up) ================
 *
 * Covers the freshly-landed command-ring wiring:
 *   - lagfx_protocol_mmio_write routing for 0x1000/0x1004/0x1008/
 *     0x1010/0x101c/0x1030 (ring-geometry setters + doorbell).
 *   - lagfx_fifo_drain ring-buffer read loop with wrap.
 *   - CmdGetDeviceInfo2 (0x3a) response-page DMA + actual_count
 *     writeback into the ring header +4.
 *   - MMIO read extension at 0x100c returning 0 (fifoFaultOffset).
 */

static void test_m3_mmio_setter_routing(void) {
    fprintf(stdout, "\n--- test: m3_mmio_setter_routing ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Program ring geometry in the order the real kext does:
     *   0x1004 → ring_size
     *   0x1010 → ring_start_offset (byte offset within base page)
     *   0x1030 → ring_base_pfn  (GPA = (pfn << 12) + start_offset)
     *   0x101c → mailbox page pfn (tracked separately)
     *   0x1000 → arm bit
     */
    lagfx_mmio_write(dev, 0x1004u, 0x10000u);    /* 64 KiB */
    CHECK(p->ring_size == 0x10000u,
          "m3: 0x1004 sets ring_size");

    lagfx_mmio_write(dev, 0x1010u, 0x1000u);     /* byte offset */
    CHECK(p->ring_start_offset == 0x1000u,
          "m3: 0x1010 sets ring_start_offset");
    CHECK(p->page_size == 0x1000u,
          "m3: 0x1010 defaults page_size to 0x1000");

    lagfx_mmio_write(dev, 0x1030u, 0xabcdeu);    /* pfn */
    CHECK(p->ring_base_pfn == 0xabcdeu,
          "m3: 0x1030 sets ring_base_pfn");
    /* GPA = (pfn << 12) + start_offset = 0xabcde000 + 0x1000 = 0xabcdf000 */
    CHECK(p->ring_base_gpa == ((uint64_t)0xabcdeu << 12) + 0x1000u,
          "m3: ring_base_gpa = (pfn << 12) + start_offset");

    lagfx_mmio_write(dev, LAGFX_REG_ROOT_PAGE_NUMBER, 0xfeedu);
    CHECK(p->ring_shared_page_pfn == 0xfeedu,
          "m3: 0x101c sets ring_shared_page_pfn (not base pfn)");

    lagfx_mmio_write(dev, LAGFX_REG_STATUS_CONTROL, 1u);
    CHECK(p->ring_armed == true,
          "m3: 0x1000 arms the ring");

    /* Re-programming 0x1010 AFTER 0x1030 must also recompute the GPA
     * so order-independence holds. */
    lagfx_mmio_write(dev, 0x1010u, 0x2000u);
    CHECK(p->ring_base_gpa == ((uint64_t)0xabcdeu << 12) + 0x2000u,
          "m3: re-write 0x1010 recomputes ring_base_gpa");

    /* 0x100c read returns 0 (fifoFaultOffset — host must hard-return 0
     * per RE to avoid spurious handleFaultInterrupt activation). */
    p->read_ptr = 0x2048u;
    CHECK(lagfx_mmio_read(dev, LAGFX_REG_FIFO_FAULT_OFFSET) == 0u,
          "m3: 0x100c read returns 0 (not read_ptr)");

    lagfx_device_free(dev);
}

static void test_m3_doorbell_advances_write_ptr(void) {
    fprintf(stdout, "\n--- test: m3_doorbell_advances_write_ptr ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Ring not yet armed — doorbell write still moves write_ptr, drain
     * just no-ops. */
    CHECK(p->write_ptr == 0u, "m3: write_ptr starts at 0");
    lagfx_mmio_write(dev, 0x1008u, 0x40u);
    CHECK(p->write_ptr == 0x40u,
          "m3: doorbell write advances p->write_ptr");
    CHECK(shell.read_memory_count == 0,
          "m3: doorbell with disarmed ring does not DMA");

    /* Subsequent write — write_ptr tracks last value. */
    lagfx_mmio_write(dev, 0x1008u, 0xc0u);
    CHECK(p->write_ptr == 0xc0u,
          "m3: doorbell write overwrites with latest value");

    lagfx_device_free(dev);
}

static void test_m3_fifo_drain_dispatches_injected_nop(void) {
    fprintf(stdout, "\n--- test: m3_fifo_drain_dispatches_injected_nop ---\n");

    /* Wire up a synthetic ring: 4 KiB backing, base GPA 0x200000,
     * start_offset 0 (ring sits right at the base GPA for simplicity). */
    const uint64_t ring_gpa = 0x200000ull;
    const uint32_t ring_size = 0x1000u;
    uint8_t ring[0x1000];
    memset(ring, 0, sizeof(ring));

    mock_shell_t shell = {0};
    shell.ring_gpa      = ring_gpa;
    shell.ring_capacity = ring_size;
    shell.ring_backing  = ring;

    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Inject a single NOP (12-byte header, zero payload) at ring[0]. */
    build_header(ring, LAGFX_OP_NOP, /*arg_count_8b=*/0,
                 /*total_length=*/12, /*stamp=*/0xcafec0deu);

    /* Program ring geometry — pfn<<12 == ring_gpa, start_offset == 0. */
    lagfx_mmio_write(dev, 0x1004u, ring_size);
    lagfx_mmio_write(dev, 0x1010u, 0u);
    lagfx_mmio_write(dev, 0x1030u, (uint32_t)(ring_gpa >> 12));
    lagfx_mmio_write(dev, LAGFX_REG_STATUS_CONTROL, 1u);

    CHECK(p->ring_base_gpa == ring_gpa,
          "m3: ring_base_gpa matches synthetic GPA");

    /* Doorbell at 12 triggers the drain. */
    lagfx_mmio_write(dev, 0x1008u, 12u);

    CHECK(p->total_cmds_seen == 1,
          "m3: drain saw the injected NOP");
    CHECK(p->total_cmds_completed == 1,
          "m3: drain completed the injected NOP");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xcafec0deu,
          "m3: injected NOP stamped");
    CHECK(shell.raise_irq_count == 1,
          "m3: drain raised IRQ for completion");
    CHECK(p->read_ptr == 12u,
          "m3: read_ptr advanced past the 12-byte header");
    CHECK(shell.read_memory_count >= 2,
          "m3: drain DMA'd the header + body (>=2 read_memory calls)");

    /* A second doorbell with no new command does not re-dispatch. */
    uint64_t seen_before = p->total_cmds_seen;
    lagfx_mmio_write(dev, 0x1008u, 12u);
    CHECK(p->total_cmds_seen == seen_before,
          "m3: idempotent doorbell (rp==wp) drains nothing");

    lagfx_device_free(dev);
}

static void test_m3_get_device_info2_response_and_count(void) {
    fprintf(stdout, "\n--- test: m3_get_device_info2_response_and_count ---\n");

    /* Ring backing: 4 KiB at 0x300000. Response page: 4 KiB at 0x400000. */
    const uint64_t ring_gpa = 0x300000ull;
    const uint32_t ring_size = 0x1000u;
    const uint64_t resp_gpa = 0x400000ull;
    const uint32_t resp_pfn = (uint32_t)(resp_gpa >> 12);

    uint8_t ring[0x1000];
    uint8_t resp_page[0x1000];
    memset(ring, 0, sizeof(ring));
    memset(resp_page, 0, sizeof(resp_page));

    mock_shell_t shell = {0};
    shell.ring_gpa         = ring_gpa;
    shell.ring_capacity    = ring_size;
    shell.ring_backing     = ring;
    shell.devinfo_gpa      = resp_gpa;
    shell.devinfo_capacity = sizeof(resp_page);
    shell.devinfo_buf      = resp_page;

    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Build a CmdGetDeviceInfo2 in the ring:
     *   header (12) + payload { kind=0x2a, resp_qwords=0x200, resp_pfn }
     * Total length = 24.
     */
    const uint32_t cmd_len = 24u;
    build_header(ring, LAGFX_OP_GET_DEVICE_INFO_2, /*arg_count_8b=*/0,
                 /*total_length=*/cmd_len, /*stamp=*/0x3a3a3a3au);
    put_le32(ring + 12, 0x2au);        /* kind */
    put_le32(ring + 16, 0x200u);       /* resp_qwords */
    put_le32(ring + 20, resp_pfn);     /* resp_pfn */

    /* Program ring + arm + kick. */
    lagfx_mmio_write(dev, 0x1004u, ring_size);
    lagfx_mmio_write(dev, 0x1010u, 0u);
    lagfx_mmio_write(dev, 0x1030u, (uint32_t)(ring_gpa >> 12));
    lagfx_mmio_write(dev, LAGFX_REG_STATUS_CONTROL, 1u);
    lagfx_mmio_write(dev, 0x1008u, cmd_len);

    CHECK(p->total_cmds_seen == 1,
          "m3-devinfo2: drain saw the 0x3a command");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x3a3a3a3au,
          "m3-devinfo2: stamp completed");

    /*
     * Current src/protocol/ops_device.c behavior (A4a, 2026-04-24):
     * CmdGetDeviceInfo2 emits the full TLV superset — 41 {u32 key, u32
     * value} pairs covering tags 0x01..0x29, with tag 0x12 set to
     * 0x00020008 (avoid the kext parser's missing-tag fallback) and the
     * rest zero. Total response payload = 41 * 8 = 328 bytes. */
    const size_t expected_pairs = 41u;
    const size_t expected_resp_bytes = expected_pairs * 8u;
    CHECK(shell.devinfo_last_len == expected_resp_bytes,
          "m3-devinfo2: response page write was 41 pairs (328 bytes)");

    /* Pair #0 → key=0x01, value=0 at resp_page[0..7]. */
    uint32_t pair0_key = (uint32_t)resp_page[0]
                       | ((uint32_t)resp_page[1] << 8)
                       | ((uint32_t)resp_page[2] << 16)
                       | ((uint32_t)resp_page[3] << 24);
    CHECK(pair0_key == 0x01u,
          "m3-devinfo2: pair #0 key == 0x01");

    /* Pair #17 → key=0x12, value=0x00020008 (the version-tag override).
     * Offset = 17 * 8 = 136. */
    uint32_t pair17_key = (uint32_t)resp_page[136]
                        | ((uint32_t)resp_page[137] << 8)
                        | ((uint32_t)resp_page[138] << 16)
                        | ((uint32_t)resp_page[139] << 24);
    uint32_t pair17_val = (uint32_t)resp_page[140]
                        | ((uint32_t)resp_page[141] << 8)
                        | ((uint32_t)resp_page[142] << 16)
                        | ((uint32_t)resp_page[143] << 24);
    CHECK(pair17_key == 0x12u,
          "m3-devinfo2: pair #17 key == 0x12 (version tag)");
    CHECK(pair17_val == 0x00020008u,
          "m3-devinfo2: pair #17 value == 0x00020008 (avoids parser fallback)");

    /* actual_count writeback: with 41 pairs emitted, the handler writes
     * u32=41 to ring header offset +4 (the `length` slot) AFTER dispatch.
     * mock_write captures writes into ring_backing for overlapping ranges,
     * so ring[4..7] now holds the actual_count LE u32. */
    uint32_t actual_count = (uint32_t)ring[4]
                          | ((uint32_t)ring[5] << 8)
                          | ((uint32_t)ring[6] << 16)
                          | ((uint32_t)ring[7] << 24);
    CHECK(actual_count == (uint32_t)expected_pairs,
          "m3-devinfo2: drain wrote actual_count=41 to ring header +4");

    lagfx_device_free(dev);
}

static void test_task_table_full(void) {
    fprintf(stdout, "\n--- test: task_table_full ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Fill the task table to capacity and then overflow. */
    uint8_t buf[36];
    for (unsigned i = 0; i < LAGFX_MAX_TASKS; ++i) {
        build_header(buf, LAGFX_OP_DEFINE_TASK2, 0, 36,
                     (uint32_t)(0xa0000000u + i));
        put_le32(buf + 12, 100u + i);
        put_le64(buf + 16, 0ull);
        put_le64(buf + 24, 0x1000ull);
        put_le32(buf + 32, 0u);
        int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
        CHECK(rc == LAGFX_HANDLER_OK, "DefineTask2 slot alloc");
    }

    /* Overflow attempt. */
    build_header(buf, LAGFX_OP_DEFINE_TASK2, 0, 36, 0xa0000fffu);
    put_le32(buf + 12, 999u);
    put_le64(buf + 16, 0ull);
    put_le64(buf + 24, 0x1000ull);
    put_le32(buf + 32, 0u);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_ERR_STATE,
          "DefineTask2 returns ERR_STATE when table full");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xa0000fffu,
          "DefineTask2 stamp still signals on overflow");

    lagfx_device_free(dev);
}

static void test_reset_clears_state(void) {
    fprintf(stdout, "\n--- test: reset_clears_state ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, 0, 12, 0x44440001u);
    lagfx_protocol_dispatch_one(p, buf, sizeof(buf));

    lagfx_device_reset(dev);

    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(seen == 0, "reset clears total_cmds_seen");
    CHECK(completed == 0, "reset clears total_cmds_completed");
    CHECK(unknown == 0, "reset clears unknown_opcode_count");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0,
          "reset clears last_completed_stamp");
    /* STATUS_CONTROL register should remain. */
    CHECK(lagfx_mmio_read(dev, LAGFX_REG_STATUS_CONTROL) != 0,
          "reset preserves STATUS_CONTROL register");

    lagfx_device_free(dev);
}

/* === §14 M6 gap-closure tests: 0x19/0x1a/0x27/0x28/0x29 =========
 *
 * Covers the log-only stubs added for WindowServer startup (see
 * re-followup-spec-gaps.md §14 punch list items 7 and 10). Each test
 * asserts: (a) dispatch returns OK, (b) stamp propagates to the cell,
 * (c) IRQ raised, (d) the handler-local capture reflects the latest
 * command. The exact payload decoding is best-effort — §14.10 marks
 * 0x19/0x1a as cosmetic for M6, and §14.5 marks 0x27/0x28/0x29 as
 * conjectured — so we assert only fields the decoder advertises.
 * ============================================================== */

static void test_display_compositor_params_handler(void) {
    fprintf(stdout, "\n--- test: display_compositor_params_handler ---\n");

    lagfx_ops_display_reset();
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Arbitrary 20-byte payload: u32 displayID + opaque 16 bytes. */
    uint8_t cmd[12 + 20];
    build_header(cmd, LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS, /*arg_count_8b=*/0,
                 /*total_length=*/sizeof(cmd), /*stamp=*/0x19190001u);
    put_le32(cmd + 12, 7u);  /* displayID */
    for (int i = 0; i < 16; ++i) cmd[16 + i] = (uint8_t)(0xa0 + i);

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayCompositorParameters dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x19190001u,
          "CmdDisplayCompositorParameters stamp propagated");
    CHECK(shell.raise_irq_count >= 1,
          "CmdDisplayCompositorParameters raised IRQ");

    const lagfx_compositor_params_state_t *st =
        lagfx_ops_display_last_compositor_params();
    CHECK(st != NULL && st->valid,
          "compositor-params capture valid after dispatch");
    CHECK(st != NULL && st->dispatch_count == 1u,
          "compositor-params dispatch_count == 1");
    CHECK(st != NULL && st->display_id == 7u,
          "compositor-params display_id decoded");
    CHECK(st != NULL && st->payload_size == 20u,
          "compositor-params payload_size captured");
    CHECK(st != NULL && st->last_stamp == 0x19190001u,
          "compositor-params last_stamp captured");

    lagfx_device_free(dev);
}

static void test_display_set_icc_profile_handler(void) {
    fprintf(stdout, "\n--- test: display_set_icc_profile_handler ---\n");

    lagfx_ops_display_reset();
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 16-byte payload: u32 displayID + u32 profile_size + u64 profile_va. */
    uint8_t cmd[12 + 16];
    build_header(cmd, LAGFX_OP_DISPLAY_SET_ICC_PROFILE, 0,
                 /*total_length=*/sizeof(cmd), /*stamp=*/0x1a1a0001u);
    put_le32(cmd + 12, 3u);             /* displayID */
    put_le32(cmd + 16, 0x1000u);        /* profile_size = 4 KiB */
    put_le64(cmd + 20, 0xdeadbeef0000ull); /* profile_va */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplaySetGuestICCProfile dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x1a1a0001u,
          "CmdDisplaySetGuestICCProfile stamp propagated");
    CHECK(shell.raise_irq_count >= 1,
          "CmdDisplaySetGuestICCProfile raised IRQ");

    const lagfx_icc_profile_state_t *st =
        lagfx_ops_display_last_icc_profile();
    CHECK(st != NULL && st->valid,
          "icc-profile capture valid after dispatch");
    CHECK(st != NULL && st->dispatch_count == 1u,
          "icc-profile dispatch_count == 1");
    CHECK(st != NULL && st->display_id == 3u,
          "icc-profile display_id decoded");
    CHECK(st != NULL && st->profile_size == 0x1000u,
          "icc-profile profile_size decoded");
    CHECK(st != NULL && st->profile_va == 0xdeadbeef0000ull,
          "icc-profile profile_va decoded");
    CHECK(st != NULL && st->payload_size == 16u,
          "icc-profile payload_size captured");
    CHECK(st != NULL && st->last_stamp == 0x1a1a0001u,
          "icc-profile last_stamp captured");

    lagfx_device_free(dev);
}

static void test_iosurface_delete_handler(void) {
    fprintf(stdout, "\n--- test: iosurface_delete_handler ---\n");

    lagfx_ops_iosurface_reset();
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 4-byte payload per §14.5: u32 surface_id. */
    uint8_t cmd[12 + 4];
    build_header(cmd, LAGFX_OP_DELETE_IOSURFACE, 0,
                 /*total_length=*/sizeof(cmd), /*stamp=*/0x27270001u);
    put_le32(cmd + 12, 0xabcd1234u);  /* surface_id */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDeleteIOSurface dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x27270001u,
          "CmdDeleteIOSurface stamp propagated");
    CHECK(shell.raise_irq_count >= 1,
          "CmdDeleteIOSurface raised IRQ");

    /* Opcode routed to the table entry (not the unknown fallback). */
    const lagfx_op_descriptor_t *d =
        lagfx_opcode_lookup(LAGFX_OP_DELETE_IOSURFACE);
    CHECK(d != NULL && d->handler != NULL,
          "0x27 has a registered handler (not default-fallback)");
    CHECK(d != NULL && strcmp(d->name, "CmdDeleteIOSurface") == 0,
          "0x27 registered as CmdDeleteIOSurface");

    const lagfx_iosurface_capture_t *cap =
        lagfx_ops_iosurface_last_delete();
    CHECK(cap != NULL && cap->valid,
          "iosurface-delete capture valid");
    CHECK(cap != NULL && cap->dispatch_count == 1u,
          "iosurface-delete dispatch_count == 1");
    CHECK(cap != NULL && cap->surface_id == 0xabcd1234u,
          "iosurface-delete surface_id decoded");
    CHECK(cap != NULL && cap->last_stamp == 0x27270001u,
          "iosurface-delete last_stamp captured");

    /* Second dispatch bumps the counter. */
    build_header(cmd, LAGFX_OP_DELETE_IOSURFACE, 0,
                 sizeof(cmd), /*stamp=*/0x27270002u);
    put_le32(cmd + 12, 0xfeedface);
    rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK, "CmdDeleteIOSurface second dispatch OK");
    cap = lagfx_ops_iosurface_last_delete();
    CHECK(cap != NULL && cap->dispatch_count == 2u,
          "iosurface-delete dispatch_count == 2 after second");
    CHECK(cap != NULL && cap->surface_id == 0xfeedfaceu,
          "iosurface-delete surface_id updated on second dispatch");

    /* Unknown-opcode counter stays at zero — handler is registered. */
    uint64_t seen, completed, unknown;
    lagfx_protocol_stats(p, &seen, &completed, &unknown);
    CHECK(unknown == 0,
          "CmdDeleteIOSurface does NOT fall through to default handler");

    lagfx_device_free(dev);
}

static void test_iosurface_create_handler(void) {
    fprintf(stdout, "\n--- test: iosurface_create_handler ---\n");

    lagfx_ops_iosurface_reset();
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 28-byte conjectured payload per §14.5 / Phase 4 §3.3:
     *   u32 surface_id, u32 w, u32 h, u32 pixel_format,
     *   u32 bytes_per_row, u64 size. */
    uint8_t cmd[12 + 28];
    build_header(cmd, LAGFX_OP_IOSURFACE_CREATE, 0,
                 /*total_length=*/sizeof(cmd), /*stamp=*/0x28280001u);
    put_le32(cmd + 12, 0x1000u);   /* surface_id */
    put_le32(cmd + 16, 1920u);     /* width */
    put_le32(cmd + 20, 1080u);     /* height */
    put_le32(cmd + 24, 80u);       /* pixel_format = BGRA8Unorm */
    put_le32(cmd + 28, 7680u);     /* bytes_per_row (1920*4) */
    put_le64(cmd + 32, 0x7e9000ull); /* size */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdIOSurfaceCreate dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x28280001u,
          "CmdIOSurfaceCreate stamp propagated");
    CHECK(shell.raise_irq_count >= 1,
          "CmdIOSurfaceCreate raised IRQ");

    const lagfx_op_descriptor_t *d =
        lagfx_opcode_lookup(LAGFX_OP_IOSURFACE_CREATE);
    CHECK(d != NULL && d->handler != NULL,
          "0x28 has a registered handler");
    CHECK(d != NULL && strcmp(d->name, "CmdIOSurfaceCreate") == 0,
          "0x28 registered as CmdIOSurfaceCreate");

    const lagfx_iosurface_capture_t *cap =
        lagfx_ops_iosurface_last_create();
    CHECK(cap != NULL && cap->valid,
          "iosurface-create capture valid");
    CHECK(cap != NULL && cap->dispatch_count == 1u,
          "iosurface-create dispatch_count == 1");
    CHECK(cap != NULL && cap->surface_id == 0x1000u,
          "iosurface-create surface_id decoded");
    CHECK(cap != NULL && cap->width == 1920u,
          "iosurface-create width decoded");
    CHECK(cap != NULL && cap->height == 1080u,
          "iosurface-create height decoded");
    CHECK(cap != NULL && cap->pixel_format == 80u,
          "iosurface-create pixel_format decoded");
    CHECK(cap != NULL && cap->bytes_per_row == 7680u,
          "iosurface-create bytes_per_row decoded");
    CHECK(cap != NULL && cap->size == 0x7e9000ull,
          "iosurface-create size decoded");
    CHECK(cap != NULL && cap->last_stamp == 0x28280001u,
          "iosurface-create last_stamp captured");

    /* Short payload — fail-open: still OK, still stamps, captured for
     * the §14.8 instrumentation pass. */
    uint8_t short_cmd[12 + 4];
    build_header(short_cmd, LAGFX_OP_IOSURFACE_CREATE, 0,
                 sizeof(short_cmd), /*stamp=*/0x28280002u);
    put_le32(short_cmd + 12, 0x2000u);
    rc = lagfx_protocol_dispatch_one(p, short_cmd, sizeof(short_cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdIOSurfaceCreate short payload still OK (fail-open)");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x28280002u,
          "CmdIOSurfaceCreate short payload still stamps");
    cap = lagfx_ops_iosurface_last_create();
    CHECK(cap != NULL && cap->dispatch_count == 2u,
          "iosurface-create dispatch_count incremented on short payload");
    CHECK(cap != NULL && cap->surface_id == 0x2000u,
          "iosurface-create short payload decoded surface_id");

    lagfx_device_free(dev);
}

static void test_iosurface_update_handler(void) {
    fprintf(stdout, "\n--- test: iosurface_update_handler ---\n");

    lagfx_ops_iosurface_reset();
    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 16-byte conjectured payload: u32 surface_id, u32 flags, u64 size. */
    uint8_t cmd[12 + 16];
    build_header(cmd, LAGFX_OP_IOSURFACE_UPDATE, 0,
                 /*total_length=*/sizeof(cmd), /*stamp=*/0x29290001u);
    put_le32(cmd + 12, 0x3000u);     /* surface_id */
    put_le32(cmd + 16, 0x5a5au);     /* flags */
    put_le64(cmd + 20, 0x123456ull); /* size */

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdIOSurfaceUpdate dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x29290001u,
          "CmdIOSurfaceUpdate stamp propagated");
    CHECK(shell.raise_irq_count >= 1,
          "CmdIOSurfaceUpdate raised IRQ");

    const lagfx_op_descriptor_t *d =
        lagfx_opcode_lookup(LAGFX_OP_IOSURFACE_UPDATE);
    CHECK(d != NULL && d->handler != NULL,
          "0x29 has a registered handler");
    CHECK(d != NULL && strcmp(d->name, "CmdIOSurfaceUpdate") == 0,
          "0x29 registered as CmdIOSurfaceUpdate");

    const lagfx_iosurface_capture_t *cap =
        lagfx_ops_iosurface_last_update();
    CHECK(cap != NULL && cap->valid,
          "iosurface-update capture valid");
    CHECK(cap != NULL && cap->dispatch_count == 1u,
          "iosurface-update dispatch_count == 1");
    CHECK(cap != NULL && cap->surface_id == 0x3000u,
          "iosurface-update surface_id decoded");
    CHECK(cap != NULL && cap->flags == 0x5a5au,
          "iosurface-update flags decoded");
    CHECK(cap != NULL && cap->size == 0x123456ull,
          "iosurface-update size decoded");
    CHECK(cap != NULL && cap->last_stamp == 0x29290001u,
          "iosurface-update last_stamp captured");

    lagfx_device_free(dev);
}

static void test_opcode_table_has_iosurface_entries(void) {
    fprintf(stdout, "\n--- test: opcode_table_has_iosurface_entries ---\n");

    /* Spec §14.5 + phase-4 §3.3 punch list: all three IOSurface opcodes
     * and both cosmetic display opcodes (0x19/0x1a) must be registered
     * with non-NULL handlers so the default fallback (which bumps
     * unknown_opcode_count) is bypassed. */
    static const uint16_t ops[] = {
        LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS,
        LAGFX_OP_DISPLAY_SET_ICC_PROFILE,
        LAGFX_OP_DELETE_IOSURFACE,
        LAGFX_OP_IOSURFACE_CREATE,
        LAGFX_OP_IOSURFACE_UPDATE,
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(ops[i]);
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "opcode 0x%02x present in table", ops[i]);
        CHECK(d != NULL, msg);
        if (d) {
            snprintf(msg, sizeof(msg),
                     "opcode 0x%02x has non-NULL handler", ops[i]);
            CHECK(d->handler != NULL, msg);
        }
    }

    /* Count must reflect the +2 net additions for 0x27/0x29 relative
     * to the pre-§14 table (0x28 was renamed, not added). */
    CHECK(lagfx_opcode_table_size() == LAGFX_OPCODE_COUNT,
          "opcode table size matches LAGFX_OPCODE_COUNT");
}

/* === §14 M6 library-side gap closure tests ======================
 *
 * Coverage for the handlers added/revised per
 * re-followup-spec-gaps.md §14 (SwapMapping 32B form, Transaction3
 * layer list, CursorShow, CursorGlyph, SetSharedStatePage).
 * 0x13/0x14/0x17 are invoked directly via their lagfx_op_display_*
 * entry points because the opcode descriptor table still has
 * handler=NULL for those opcodes pending the §14.7 punch-list
 * wire-up.
 * ============================================================== */

static void test_display_swap_mapping_v2_layout(void) {
    fprintf(stdout, "\n--- test: display_swap_mapping_v2_layout ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[LAGFX_CMD_HEADER_BYTES + 32];
    build_header(cmd, LAGFX_OP_DISPLAY_SWAP_MAPPING, 0,
                 sizeof(cmd), 0x12c0de01u);
    put_le32(cmd + 12 + 0x00, 5u);
    put_le64(cmd + 12 + 0x04, 0xdeadbeef00ull);
    put_le32(cmd + 12 + 0x0c, 1920u * 4u);
    put_le32(cmd + 12 + 0x10, 1920u);
    put_le32(cmd + 12 + 0x14, 1080u);
    put_le32(cmd + 12 + 0x18, 80u);
    put_le32(cmd + 12 + 0x1c, 0u);

    /* Dispatcher still gates 0x12 at min_payload=40; call the handler
     * directly to exercise the 32 B §14.3.2 shape. */
    lagfx_cmd_header_t hdr = {
        .opcode       = LAGFX_OP_DISPLAY_SWAP_MAPPING,
        .arg_count_8b = 0,
        .length       = (uint32_t)sizeof(cmd),
        .stamp        = 0x12c0de01u,
        .payload_size = 32,
        .payload      = cmd + LAGFX_CMD_HEADER_BYTES,
    };
    lagfx_handler_status_t rc = lagfx_op_display_swap_mapping(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "SwapMapping 32B §14.3.2 form parses OK");

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 5u);
    CHECK(d != NULL && d->live, "32B form: display entry live");
    CHECK(d != NULL && d->buffer_va == 0xdeadbeef00ull,
          "32B form: bufferVA captured");
    CHECK(d != NULL && d->stride == 1920u * 4u,
          "32B form: stride captured");
    CHECK(d != NULL && d->width == 1920u && d->height == 1080u,
          "32B form: geometry captured");
    CHECK(d != NULL && d->format == 80u,
          "32B form: pixel_format captured (BGRA8Unorm=80)");
    CHECK(d != NULL && d->length == 1920ull * 1080ull * 4ull,
          "32B form: length derived as stride*height");

    lagfx_device_free(dev);
}

static void test_display_transaction3_layer_form(void) {
    fprintf(stdout, "\n--- test: display_transaction3_layer_form ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t cmd[LAGFX_CMD_HEADER_BYTES + 16 + 0x2c];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 sizeof(cmd), 0x14c0de02u);
    uint8_t *pld = cmd + LAGFX_CMD_HEADER_BYTES;
    put_le32(pld + 0x00, 0xcafe0001u);
    put_le32(pld + 0x04, 7u);
    put_le32(pld + 0x08, 1u);
    put_le32(pld + 0x0c, 0x3u);
    put_le32(pld + 0x10, 0xb10c0001u);
    put_le32(pld + 0x14, 0u);
    put_le32(pld + 0x18, 0u);
    put_le32(pld + 0x1c, 1920u);
    put_le32(pld + 0x20, 1080u);
    put_le32(pld + 0x24, 0u);
    put_le32(pld + 0x28, 0u);
    put_le32(pld + 0x2c, 1920u);
    put_le32(pld + 0x30, 1080u);
    put_le32(pld + 0x34, 0x50u);
    put_le32(pld + 0x38, 0u);

    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    CHECK(rc == LAGFX_HANDLER_OK,
          "Transaction3 layer-form parses OK");

    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, 7u);
    CHECK(d != NULL && d->live,
          "layer-form: display 7 auto-registered");
    CHECK(d != NULL && d->pending_transaction_id == 0xcafe0001u,
          "layer-form: transactionID captured");
    CHECK(d != NULL && d->transaction_pending,
          "layer-form: transaction_pending set");
    CHECK(d != NULL && d->last_attachment_count == 1u,
          "layer-form: entry count reused on last_attachment_count");
    CHECK(d != NULL && d->last_load_action == 0u,
          "layer-form: no clear action (composite-only)");

    uint8_t cmd2[LAGFX_CMD_HEADER_BYTES + 16 + 2 * 0x2c];
    memset(cmd2, 0, sizeof(cmd2));
    build_header(cmd2, LAGFX_OP_DISPLAY_TRANSACTION3, 0,
                 sizeof(cmd2), 0x14c0de03u);
    uint8_t *p2 = cmd2 + LAGFX_CMD_HEADER_BYTES;
    put_le32(p2 + 0x00, 0xcafe0002u);
    put_le32(p2 + 0x04, 7u);
    put_le32(p2 + 0x08, 2u);
    put_le32(p2 + 0x0c, 0u);
    put_le32(p2 + 0x10, 0xaaaa0001u);
    put_le32(p2 + 0x10 + 0x2c, 0xbbbb0002u);
    rc = lagfx_protocol_dispatch_one(p, cmd2, sizeof(cmd2));
    CHECK(rc == LAGFX_HANDLER_OK,
          "Transaction3 2-layer parses OK");
    d = lagfx_protocol_find_display(p, 7u);
    CHECK(d != NULL && d->last_attachment_count == 2u,
          "layer-form: 2-layer count recorded");

    lagfx_device_free(dev);
}

static void test_display_cursor_show_handler(void) {
    fprintf(stdout, "\n--- test: display_cursor_show_handler ---\n");
    lagfx_ops_display_reset();

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t pld[16];
    put_le32(pld + 0, 1u);
    pld[4] = 0x40; pld[5] = 0x01;
    pld[6] = 0xf0; pld[7] = 0x00;
    put_le32(pld + 8,  1u);
    put_le32(pld + 12, ((uint32_t)3u << 16) | 5u);

    lagfx_cmd_header_t hdr = {
        .opcode       = LAGFX_OP_DISPLAY_CURSOR_SHOW,
        .arg_count_8b = 0,
        .length       = LAGFX_CMD_HEADER_BYTES + (uint32_t)sizeof(pld),
        .stamp        = 0x13000001u,
        .payload_size = (uint16_t)sizeof(pld),
        .payload      = pld,
    };
    lagfx_handler_status_t rc = lagfx_op_display_cursor_show(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayCursorShow returns OK on well-formed payload");

    const lagfx_cursor_show_state_t *cs =
        lagfx_ops_display_last_cursor_show();
    CHECK(cs->valid, "cursor-show state marked valid");
    CHECK(cs->display_id == 1u, "cursor-show: displayID captured");
    CHECK(cs->x == 320, "cursor-show: x decoded as i16");
    CHECK(cs->y == 240, "cursor-show: y decoded as i16");
    CHECK(cs->visible == 1u, "cursor-show: visible captured");
    CHECK(cs->hot_x == 3u, "cursor-show: hot_x high16 captured");
    CHECK(cs->hot_y == 5u, "cursor-show: hot_y low16 captured");

    pld[4] = 0xf0; pld[5] = 0xff;
    pld[6] = 0x00; pld[7] = 0x00;
    hdr.stamp = 0x13000002u;
    rc = lagfx_op_display_cursor_show(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayCursorShow with negative x returns OK");
    cs = lagfx_ops_display_last_cursor_show();
    CHECK(cs->x == -16, "cursor-show: negative x decoded (sign-extended i16)");

    hdr.payload_size = 8;
    rc = lagfx_op_display_cursor_show(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplayCursorShow short payload rejected");

    lagfx_device_free(dev);
}

static void test_display_cursor_glyph_handler(void) {
    fprintf(stdout, "\n--- test: display_cursor_glyph_handler ---\n");
    lagfx_ops_display_reset();

    const uint64_t glyph_gpa = 0x200000ull;
    const uint32_t glyph_w = 32u, glyph_h = 32u;
    const uint32_t glyph_bpr = glyph_w * 4u;
    const size_t   glyph_bytes = glyph_bpr * glyph_h;
    uint8_t glyph_backing[32u * 32u * 4u];
    for (size_t i = 0; i < sizeof(glyph_backing); ++i) {
        glyph_backing[i] = (uint8_t)(i & 0xffu);
    }

    mock_shell_t shell = {0};
    shell.ring_gpa      = glyph_gpa;
    shell.ring_capacity = sizeof(glyph_backing);
    shell.ring_backing  = glyph_backing;

    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t pld[32];
    put_le32(pld + 0x00, 1u);
    put_le64(pld + 0x04, glyph_gpa);
    put_le32(pld + 0x0c, glyph_w);
    put_le32(pld + 0x10, glyph_h);
    put_le32(pld + 0x14, glyph_bpr);
    put_le32(pld + 0x18, 4u);
    put_le32(pld + 0x1c, 7u);

    lagfx_cmd_header_t hdr = {
        .opcode       = LAGFX_OP_DISPLAY_CURSOR_GLYPH,
        .arg_count_8b = 0,
        .length       = LAGFX_CMD_HEADER_BYTES + (uint32_t)sizeof(pld),
        .stamp        = 0x14000001u,
        .payload_size = (uint16_t)sizeof(pld),
        .payload      = pld,
    };
    lagfx_handler_status_t rc = lagfx_op_display_cursor_glyph(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayCursorGlyph returns OK");

    const lagfx_cursor_glyph_state_t *cg =
        lagfx_ops_display_last_cursor_glyph();
    CHECK(cg->valid, "cursor-glyph state marked valid");
    CHECK(cg->display_id == 1u, "cursor-glyph: displayID captured");
    CHECK(cg->glyph_va == glyph_gpa, "cursor-glyph: glyphVA captured");
    CHECK(cg->width == glyph_w, "cursor-glyph: width captured");
    CHECK(cg->height == glyph_h, "cursor-glyph: height captured");
    CHECK(cg->bytes_per_row == glyph_bpr, "cursor-glyph: bpr captured");
    CHECK(cg->hot_x == 4u, "cursor-glyph: hot_x captured");
    CHECK(cg->hot_y == 7u, "cursor-glyph: hot_y captured");
    CHECK(cg->captured_len == glyph_bytes,
          "cursor-glyph: all pixels captured via shell.read_memory");
    CHECK(memcmp(cg->bytes, glyph_backing, glyph_bytes) == 0,
          "cursor-glyph: captured bytes match guest backing exactly");

    lagfx_ops_display_reset();
    put_le32(pld + 0x0c, 200u);
    put_le32(pld + 0x10, 200u);
    put_le32(pld + 0x14, 200u * 4u);
    hdr.stamp = 0x14000002u;
    rc = lagfx_op_display_cursor_glyph(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplayCursorGlyph oversized glyph still returns OK");
    cg = lagfx_ops_display_last_cursor_glyph();
    CHECK(cg->valid, "oversized glyph: state captured");
    CHECK(cg->captured_len == 0,
          "oversized glyph: pixel DMA skipped (size > cap)");

    hdr.payload_size = 24;
    rc = lagfx_op_display_cursor_glyph(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplayCursorGlyph short payload rejected");

    lagfx_device_free(dev);
}

static void test_display_set_shared_state_page_handler(void) {
    fprintf(stdout, "\n--- test: display_set_shared_state_page_handler ---\n");
    lagfx_ops_display_reset();

    const uint64_t page_gpa = 0x300000ull;
    uint8_t page[0x1000];
    memset(page, 0xaa, sizeof(page));

    mock_shell_t shell = {0};
    shell.scanout_gpa      = page_gpa;
    shell.scanout_capacity = sizeof(page);
    shell.scanout_buf      = page;

    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t pld[8];
    put_le64(pld + 0, page_gpa);

    lagfx_cmd_header_t hdr = {
        .opcode       = LAGFX_OP_DISPLAY_SET_SHARED_PAGE,
        .arg_count_8b = 0,
        .length       = LAGFX_CMD_HEADER_BYTES + (uint32_t)sizeof(pld),
        .stamp        = 0x17000001u,
        .payload_size = (uint16_t)sizeof(pld),
        .payload      = pld,
    };
    lagfx_handler_status_t rc = lagfx_op_display_set_shared_page(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdDisplaySetSharedStatePage returns OK");

    const lagfx_shared_state_t *ss = lagfx_ops_display_shared_state();
    CHECK(ss->installed, "shared-state: installed flag set");
    CHECK(ss->page_va == page_gpa, "shared-state: pageVA recorded");
    CHECK(ss->vblank_counter == 1u,
          "shared-state: first tick fired (counter = 1)");

    uint32_t first_u32 = (uint32_t)page[0]
                       | ((uint32_t)page[1] << 8)
                       | ((uint32_t)page[2] << 16)
                       | ((uint32_t)page[3] << 24);
    CHECK(first_u32 == 1u,
          "mailbox: u32@+0 holds counter=1 after install");
    bool cleared = true;
    for (size_t i = 4; i < 64; ++i) {
        if (page[i] != 0u) { cleared = false; break; }
    }
    CHECK(cleared, "mailbox: first 64 B zeroed");
    CHECK(page[64] == 0xaau,
          "mailbox: bytes past first 64 left untouched");

    CHECK(lagfx_ops_display_tick_vblank(&shell, mock_write),
          "tick_vblank returns true (installed + write_memory)");
    CHECK(lagfx_ops_display_tick_vblank(&shell, mock_write),
          "tick_vblank second call returns true");
    ss = lagfx_ops_display_shared_state();
    CHECK(ss->vblank_counter == 3u,
          "shared-state: counter monotonically advanced to 3");
    first_u32 = (uint32_t)page[0]
              | ((uint32_t)page[1] << 8)
              | ((uint32_t)page[2] << 16)
              | ((uint32_t)page[3] << 24);
    CHECK(first_u32 == 3u,
          "mailbox: DMA-written counter matches shadow (3)");

    CHECK(!lagfx_ops_display_tick_vblank(&shell, NULL),
          "tick_vblank returns false when no write_memory provided");
    ss = lagfx_ops_display_shared_state();
    CHECK(ss->vblank_counter == 4u,
          "shared-state: shadow counter advances even w/o DMA path");

    hdr.payload_size = 4;
    rc = lagfx_op_display_set_shared_page(p, &hdr);
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdDisplaySetSharedStatePage short payload rejected");

    lagfx_device_free(dev);
}

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan protocol dispatch tests ===\n");

    test_header_size_invariant();
    test_opcode_table_completeness();
    test_header_parse();
    test_dispatch_nop();
    test_dispatch_debug();
    test_dispatch_unknown_opcode();
    test_dispatch_routes_to_correct_handler();
    test_mmio_status_control_arms_ring();
    test_get_device_info_handler();
    test_task_lifecycle_handler();
    test_child_fifo_lifecycle_handler();
    test_synchronize_resources_handler();
    test_map_memory2_handler();
    test_unmap_memory_handler();
    test_exec_indirect2_empty();
    test_metal_no_op_sequence();
    test_display_ack_handler();
    test_display_swap_mapping_handler();
    test_display_transaction3_handler();
    test_metal_clear_color_sequence();
    test_m3_mmio_setter_routing();
    test_m3_doorbell_advances_write_ptr();
    test_m3_fifo_drain_dispatches_injected_nop();
    test_m3_get_device_info2_response_and_count();
    test_task_table_full();
    test_reset_clears_state();

    /* §14 M6 gap-closure coverage (0x19/0x1a cosmetic + 0x27/0x28/0x29
     * conjectured IOSurface family). */
    test_display_compositor_params_handler();
    test_display_set_icc_profile_handler();
    test_iosurface_delete_handler();
    test_iosurface_create_handler();
    test_iosurface_update_handler();
    test_opcode_table_has_iosurface_entries();

    /* §14 M6 library-side gap closure: 0x12 32B / 0x16 layer form /
     * 0x13 cursor-show / 0x14 cursor-glyph / 0x17 shared-state page. */
    test_display_swap_mapping_v2_layout();
    test_display_transaction3_layer_form();
    test_display_cursor_show_handler();
    test_display_cursor_glyph_handler();
    test_display_set_shared_state_page_handler();

    fprintf(stdout, "\n=== Summary: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
