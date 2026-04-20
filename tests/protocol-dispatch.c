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

    /* Explicit count=0 with taskID. payload=8, total=20. */
    uint8_t zero[20];
    build_header(zero, LAGFX_OP_EXEC_INDIRECT2, 0, 20, 0xd0000002u);
    put_le32(zero + 12, 1u);  /* taskID */
    put_le32(zero + 16, 0u);  /* count=0 */
    rc = lagfx_protocol_dispatch_one(p, zero, sizeof(zero));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdExecIndirect2(count=0) returns OK");
    CHECK(shell.raise_irq_count == 2,
          "CmdExecIndirect2(count=0) raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd0000002u,
          "CmdExecIndirect2(count=0) stamp signalled");

    lagfx_device_free(dev);
}

static void test_exec_indirect2_scaffold(void) {
    fprintf(stdout, "\n--- test: exec_indirect2_scaffold ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Register two child FIFOs. */
    uint8_t def[16];
    build_header(def, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0xd1000001u);
    put_le32(def + 12, 7u);
    lagfx_protocol_dispatch_one(p, def, sizeof(def));
    build_header(def, LAGFX_OP_DEFINE_CHILD_FIFO, 0, 16, 0xd1000002u);
    put_le32(def + 12, 8u);
    lagfx_protocol_dispatch_one(p, def, sizeof(def));

    /* Issue ExecIndirect2 with count=2, IDs [7, 8]. Both FIFOs should
     * be flagged synced as the scaffold side-effect. payload=16, total=28. */
    uint8_t buf[28];
    build_header(buf, LAGFX_OP_EXEC_INDIRECT2, 0,
                 /*total_length=*/28, /*stamp=*/0xd1000003u);
    put_le32(buf + 12, 0u);  /* taskID (unknown; fail-open) */
    put_le32(buf + 16, 2u);  /* count=2 */
    put_le32(buf + 20, 7u);  /* indirect_cmd_id[0] = fifoID 7 */
    put_le32(buf + 24, 8u);  /* indirect_cmd_id[1] = fifoID 8 */

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK,
          "CmdExecIndirect2(count=2) returns OK (scaffolded)");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd1000003u,
          "CmdExecIndirect2(count=2) stamp signalled");

    lagfx_childfifo_entry_t *f7 = lagfx_protocol_find_fifo(p, 7u);
    lagfx_childfifo_entry_t *f8 = lagfx_protocol_find_fifo(p, 8u);
    CHECK(f7 != NULL && f7->synced,
          "CmdExecIndirect2 scaffold marked fifo 7 synced");
    CHECK(f8 != NULL && f8->synced,
          "CmdExecIndirect2 scaffold marked fifo 8 synced");

    /* Overflow: count=10 in a payload too small. */
    uint8_t bad[20];
    build_header(bad, LAGFX_OP_EXEC_INDIRECT2, 0, 20, 0xd1000004u);
    put_le32(bad + 12, 0u);
    put_le32(bad + 16, 10u); /* 10 ids demanded, but only 0 bytes follow */
    rc = lagfx_protocol_dispatch_one(p, bad, sizeof(bad));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "CmdExecIndirect2(count > payload) rejected");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd1000004u,
          "CmdExecIndirect2 size-mismatch still signals stamp");

    lagfx_device_free(dev);
}

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
    test_get_device_info_handler();
    test_task_lifecycle_handler();
    test_child_fifo_lifecycle_handler();
    test_synchronize_resources_handler();
    test_map_memory2_handler();
    test_unmap_memory_handler();
    test_exec_indirect2_empty();
    test_exec_indirect2_scaffold();
    test_metal_no_op_sequence();
    test_task_table_full();
    test_reset_clears_state();

    fprintf(stdout, "\n=== Summary: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
