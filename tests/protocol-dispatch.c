/*
 * libapplegfx-vulkan — protocol dispatch unit tests (Phase 1.A.2)
 * tests/protocol-dispatch.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Exercises the opcode dispatcher for the two handlers with real
 * implementations in this phase (CmdNOP, CmdDebug), plus the
 * descriptor-table / header-parse plumbing. No real captured-byte
 * fixtures available in mos/paravirt-re/ yet, so all inputs are
 * synthesized against the wire format in
 * mos/paravirt-re/command-buffer-format.md §2.
 *
 * Drives dispatch through two paths:
 *   1. lagfx_protocol_dispatch_one() directly (fast; tests the
 *      handler jump table + completion path).
 *   2. lagfx_mmio_write(DOORBELL) -> decoder fifo drain (skeleton;
 *      confirms the callback wiring — real drain is stubbed per R1).
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

/* Build a 16-byte PGCommand header at the front of `out` (must
 * have room for LAGFX_CMD_HEADER_BYTES bytes). Returns the
 * total length written (header bytes only; caller appends
 * payload if any). */
static size_t build_header(uint8_t *out, uint8_t opcode, uint8_t flags,
                           uint16_t total_length, uint32_t stamp,
                           uint16_t payload_size) {
    memset(out, 0, LAGFX_CMD_HEADER_BYTES);
    out[0] = opcode;
    out[1] = flags;
    out[2] = (uint8_t)(total_length & 0xffu);
    out[3] = (uint8_t)((total_length >> 8) & 0xffu);
    out[4] = (uint8_t)(stamp & 0xffu);
    out[5] = (uint8_t)((stamp >> 8) & 0xffu);
    out[6] = (uint8_t)((stamp >> 16) & 0xffu);
    out[7] = (uint8_t)((stamp >> 24) & 0xffu);
    /* reserved = 0 (bytes 8..11) */
    out[12] = (uint8_t)(payload_size & 0xffu);
    out[13] = (uint8_t)((payload_size >> 8) & 0xffu);
    /* padding = 0 (bytes 14..15) */
    return LAGFX_CMD_HEADER_BYTES;
}

/* === Tests ================================================ */

static void test_opcode_table_completeness(void) {
    fprintf(stdout, "\n--- test: opcode_table_completeness ---\n");

    /* All 36 opcodes documented per command-buffer-format.md §10. */
    CHECK(lagfx_opcode_table_size() == LAGFX_OPCODE_COUNT,
          "opcode table has expected entry count");

    /* Spot-check each P0 opcode from the brief §4.1. */
    static const uint8_t p0[] = {
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

    /* Unknown opcode returns NULL. */
    CHECK(lagfx_opcode_lookup(0xab) == NULL,
          "unknown opcode lookup returns NULL");

    /* Name fallback prints Unknown(...) — never returns NULL. */
    const char *name = lagfx_opcode_name(0xab);
    CHECK(name != NULL && strstr(name, "Unknown") != NULL,
          "unknown opcode name formats as Unknown(...)");
}

static void test_header_parse(void) {
    fprintf(stdout, "\n--- test: header_parse ---\n");

    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, LAGFX_FLAG_COMPLETION_EXPECTED,
                 /*total_length=*/16, /*stamp=*/0xcafebabeu,
                 /*payload_size=*/0);

    lagfx_cmd_header_t hdr;
    bool ok = lagfx_fifo_parse_header(buf, sizeof(buf), &hdr);
    CHECK(ok, "parse 16-byte NOP header");
    CHECK(hdr.opcode == LAGFX_OP_NOP, "opcode byte");
    CHECK(hdr.flags == LAGFX_FLAG_COMPLETION_EXPECTED, "flags byte");
    CHECK(hdr.length == 16, "length");
    CHECK(hdr.stamp == 0xcafebabeu, "stamp LE decode");
    CHECK(hdr.payload_size == 0, "payload_size");
    CHECK(hdr.payload == NULL, "payload pointer NULL for empty");

    /* A header that claims length < 16 is malformed — reject. */
    uint8_t bad[LAGFX_CMD_HEADER_BYTES];
    build_header(bad, LAGFX_OP_NOP, 0, /*total_length=*/8, 0, 0);
    CHECK(!lagfx_fifo_parse_header(bad, sizeof(bad), &hdr),
          "reject header with length < 16");

    /* Short buffer (< 16 bytes) rejected. */
    CHECK(!lagfx_fifo_parse_header(buf, 10, &hdr),
          "reject buffer shorter than header");
}

static void test_dispatch_nop(void) {
    fprintf(stdout, "\n--- test: dispatch_nop ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    CHECK(p != NULL, "device has decoder attached");

    /* NOP without completion flag — should NOT raise IRQ. */
    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, 0, 16, 0x11110001u, 0);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "NOP no-flag dispatch returns OK");
    CHECK(shell.raise_irq_count == 0, "NOP no-flag: no IRQ raised");

    /* NOP with completion flag — IRQ expected. */
    build_header(buf, LAGFX_OP_NOP, LAGFX_FLAG_COMPLETION_EXPECTED,
                 16, 0x11110002u, 0);
    rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "NOP completion-expected dispatch OK");
    CHECK(shell.raise_irq_count == 1, "NOP completion-expected: IRQ raised");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0x11110002u,
          "fence register carries the completed stamp");

    /* Fence register (0x1020) should read back the stamp too. */
    uint32_t fence = lagfx_mmio_read(dev, LAGFX_REG_FENCE);
    CHECK(fence == 0x11110002u, "MMIO fence register reads last stamp");

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

    /* CmdDebug with a short payload. Total length = 16 + 8 = 24. */
    uint8_t buf[24];
    build_header(buf, LAGFX_OP_DEBUG, LAGFX_FLAG_COMPLETION_EXPECTED,
                 /*total_length=*/24, /*stamp=*/0xdeadbeefu,
                 /*payload_size=*/8);
    for (int i = 0; i < 8; ++i) {
        buf[LAGFX_CMD_HEADER_BYTES + i] = (uint8_t)(0xa0 + i);
    }

    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    CHECK(rc == LAGFX_HANDLER_OK, "Debug dispatch returns OK");
    CHECK(shell.raise_irq_count == 1, "Debug w/ completion flag raised IRQ");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xdeadbeefu,
          "Debug stamp propagated to fence");

    lagfx_device_free(dev);
}

static void test_dispatch_unknown_opcode(void) {
    fprintf(stdout, "\n--- test: dispatch_unknown_opcode ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 0xab is not in the table. Default handler should log and ack. */
    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, /*opcode=*/0xab, LAGFX_FLAG_COMPLETION_EXPECTED,
                 16, 0x22220001u, 0);
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
    static const uint8_t ops[] = {
        LAGFX_OP_GET_DEVICE_INFO,
        LAGFX_OP_DEFINE_TASK2,
        LAGFX_OP_DELETE_TASK,
        LAGFX_OP_DEFINE_CHILD_FIFO,
        LAGFX_OP_DELETE_CHILD_FIFO,
        LAGFX_OP_SYNCHRONIZE_RESOURCES,
        LAGFX_OP_NOP,
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        /* Build a header with whatever minimum payload size the
         * descriptor wants (pad with zeros — stubs don't read it). */
        const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(ops[i]);
        uint16_t payload_size = d ? d->min_payload : 0;
        uint16_t total_length = (uint16_t)(LAGFX_CMD_HEADER_BYTES + payload_size);
        uint8_t buf[256] = {0};
        build_header(buf, ops[i], LAGFX_FLAG_COMPLETION_EXPECTED,
                     total_length, (uint32_t)(0x33330000u + i), payload_size);
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
    CHECK(shell.raise_irq_count == seen, "one IRQ per completion-flagged cmd");

    lagfx_device_free(dev);
}

static void test_mmio_doorbell_triggers_drain(void) {
    fprintf(stdout, "\n--- test: mmio_doorbell_triggers_drain ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Doorbell write through the public MMIO API. The drain itself
     * is a no-op (R1 — ring GPA not discovered); we just confirm the
     * callback wiring records the stamp. */
    lagfx_mmio_write(dev, LAGFX_REG_DOORBELL, 0xbeefcafeu);
    CHECK(lagfx_protocol_last_doorbell_stamp(p) == 0xbeefcafeu,
          "doorbell write recorded via public MMIO path");

    /* Doorbell register reads back the stamp too (shadow). */
    uint32_t rb = lagfx_mmio_read(dev, LAGFX_REG_DOORBELL);
    CHECK(rb == 0xbeefcafeu, "doorbell register shadow readable");

    lagfx_device_free(dev);
}

static void test_reset_clears_state(void) {
    fprintf(stdout, "\n--- test: reset_clears_state ---\n");

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint8_t buf[LAGFX_CMD_HEADER_BYTES];
    build_header(buf, LAGFX_OP_NOP, LAGFX_FLAG_COMPLETION_EXPECTED,
                 16, 0x44440001u, 0);
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

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan protocol dispatch tests ===\n");

    test_opcode_table_completeness();
    test_header_parse();
    test_dispatch_nop();
    test_dispatch_debug();
    test_dispatch_unknown_opcode();
    test_dispatch_routes_to_correct_handler();
    test_mmio_doorbell_triggers_drain();
    test_reset_clears_state();

    fprintf(stdout, "\n=== Summary: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
