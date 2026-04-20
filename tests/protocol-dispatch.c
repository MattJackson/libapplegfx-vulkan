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
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"
#include "../src/protocol/fifo.h"
#include "../src/protocol/state.h"

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

typedef struct {
    unsigned raise_irq_count;
    uint32_t last_irq_vector;
    unsigned create_task_count;
    unsigned destroy_task_count;
    unsigned map_memory_count;
    unsigned unmap_memory_count;
    unsigned read_memory_count;
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
    (void)gpa; (void)l; (void)d;
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

    /* Stamp cell at MMIO 0x1014 readable. */
    uint32_t cell = lagfx_mmio_read(dev, LAGFX_REG_STAMP_CELL_1);
    CHECK(cell == 0x11110002u,
          "MMIO STAMP_CELL_1 (0x1014) reads last completed stamp");

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

static void test_mmio_setter_candidate_probe(void) {
    fprintf(stdout, "\n--- test: mmio_setter_candidate_probe ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* The real doorbell offset in 0x1004..0x1034 is unknown. For now,
     * writes to any offset in that range land in the setter-candidate
     * probe, which records (offset, value) and bumps a counter. Tests
     * exercise four representative candidates. */
    static const uint64_t candidates[] = {
        0x1004u, 0x1010u, 0x1020u, 0x1034u,
    };
    uint64_t expected_count = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint32_t value = (uint32_t)(0xbeef0000u + (uint32_t)i);
        lagfx_mmio_write(dev, candidates[i], value);
        expected_count += 1;

        CHECK(lagfx_protocol_setter_write_count(p) == expected_count,
              "setter probe count increments");
        CHECK(lagfx_protocol_last_setter_offset(p) == (uint32_t)candidates[i],
              "setter probe records last offset");
        CHECK(lagfx_protocol_last_setter_value(p) == value,
              "setter probe records last value");
    }

    /* 0x101c is inside the candidate range per the spec — it's a
     * setter on the write side (and _rootPageNumber on the read side).
     * Confirm the probe fires on a write there too. */
    lagfx_mmio_write(dev, LAGFX_REG_ROOT_PAGE_NUMBER, 0x12345678u);
    CHECK(lagfx_protocol_last_setter_offset(p) == LAGFX_REG_ROOT_PAGE_NUMBER,
          "write to 0x101c hits the setter probe");

    lagfx_device_free(dev);
}

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
    CHECK(lagfx_protocol_setter_write_count(p) == 0,
          "reset clears setter probe counter");
    /* STATUS_CONTROL register should remain. */
    CHECK(lagfx_mmio_read(dev, LAGFX_REG_STATUS_CONTROL) != 0,
          "reset preserves STATUS_CONTROL register");

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
    test_mmio_setter_candidate_probe();
    test_mmio_status_control_arms_ring();
    test_reset_clears_state();

    fprintf(stdout, "\n=== Summary: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
