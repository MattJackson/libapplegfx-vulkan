/*
 * libapplegfx-vulkan — M4 task-translate + define_host_task tests
 * tests/m4-task-translate.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers items 4 + 5 from the M3/M4 critical-path coverage plan:
 *
 *   4. lagfx_task_translate (public via state.h):
 *      - Positive: task with root_page_pfn=P, dev_addr=N -> reads PFN
 *        entry, returns gpa correctly.
 *      - Negative: unknown task_id -> false.
 *      - Negative: root_page_pfn=0 -> false.
 *      - Edge: data_pfn at the entry slot is 0 -> false (and the around-
 *        bytes diagnostic is logged; we cannot capture log lines from the
 *        unit-test harness, so we just assert the false return).
 *      - Edge: dev_addr crosses page boundary -> out_run_len reflects
 *        bytes-to-page-end.
 *      - Edge: shell.read_memory fails -> false.
 *
 *   5. lagfx_op_define_host_task handler (CmdDefineHostTask 0x38):
 *      - Parses {task_id, reserved, flags, root_page_pfn} from a
 *        16B payload.
 *      - Allocates a new task slot if task_id unseen; updates
 *        root_page_pfn on existing.
 *      - Min-payload guard: rejects payload < 16B with ERR_SIZE.
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/state.h"
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

/* === Mock shell with a 64KiB heap mirror ============================== */

typedef struct {
    /* Counters. */
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;

    /* Force-fail next read_memory call when set. Cleared after one shot. */
    int next_read_fails;

    /* 64KiB heap mirror covering [heap_gpa, heap_gpa+0x10000). Reads
     * within range are served from heap, writes mirror back. Reads outside
     * the mirror return zeros and succeed. */
    uint64_t heap_gpa;
    uint8_t  heap[0x10000];
} m4_shell_t;

static lagfx_task_t *m4_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void m4_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool m4_map(void *op, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool m4_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool m4_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    m4_shell_t *m = (m4_shell_t *)op;
    m->read_memory_count++;
    if (m->next_read_fails) {
        m->next_read_fails = 0;
        return false;
    }
    if (d) {
        if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + sizeof(m->heap)) {
            memcpy(d, m->heap + (gpa - m->heap_gpa), (size_t)l);
            return true;
        }
        memset(d, 0, (size_t)l);
    }
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

/* === Header / payload writers ========================================= */

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

/* Arrange a synthetic PFN-array for task `task_id` whose root is at
 * (root_pfn<<12) within the heap mirror. Returns nothing — caller is
 * expected to call lagfx_op_define_host_task afterwards (or set up the
 * task directly via DefineTask2). */
static void prep_pfn_array_in_heap(m4_shell_t *m,
                                   uint32_t root_pfn,
                                   const uint32_t *entries,
                                   size_t entry_count) {
    uint64_t root_gpa = (uint64_t)root_pfn << 12;
    /* The heap mirror must cover [root_gpa, root_gpa+entry_count*4). */
    uint64_t end = root_gpa + entry_count * 4u;
    if (root_gpa < m->heap_gpa
        || end > m->heap_gpa + sizeof(m->heap)) {
        fprintf(stderr, "prep_pfn_array_in_heap: root_pfn=0x%x out of mirror "
                "(heap [0x%llx..0x%llx))\n",
                root_pfn,
                (unsigned long long)m->heap_gpa,
                (unsigned long long)(m->heap_gpa + sizeof(m->heap)));
        exit(2);
    }
    uint8_t *p = m->heap + (root_gpa - m->heap_gpa);
    for (size_t i = 0; i < entry_count; ++i) {
        put_le32(p + i * 4u, entries[i]);
    }
}

/* Define a task via the public CmdDefineHostTask handler — exercises
 * item 5 simultaneously. */
static void define_host_task_via_dispatch(lagfx_protocol_t *p,
                                          uint32_t task_id,
                                          uint32_t flags,
                                          uint32_t root_pfn,
                                          uint32_t stamp) {
    uint8_t buf[28];
    build_header(buf, LAGFX_OP_DEFINE_HOST_TASK, /*arg_count_8b=*/0,
                 /*total_length=*/12 + 16u, stamp);
    put_le32(buf + 12 + 0,  task_id);
    put_le32(buf + 12 + 4,  0u);     /* reserved */
    put_le32(buf + 12 + 8,  flags);
    put_le32(buf + 12 + 12, root_pfn);
    int rc = lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
    (void)rc;
}

/* === Item 4 tests: lagfx_task_translate =============================== */

static void test_translate_positive(void) {
    fprintf(stdout, "\n--- test: translate_positive ---\n");
    m4_shell_t shell = {0};
    /* Pick a heap base that contains both root_pfn=0x10's page (0x10000)
     * and data_pfn=0x12's page (0x12000). With heap_gpa=0x10000 and 64KiB
     * mirror, range covers [0x10000..0x20000), which holds PFN 0x10..0x1f. */
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Root PFN-array at root_pfn=0x10. Entry [0]=0x12 (data_pfn), entries
     * [1..N]=0 are fine. */
    uint32_t entries[8] = {0x12u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    prep_pfn_array_in_heap(&shell, /*root_pfn*/ 0x10u, entries, 8);

    /* Register a host task (taskID=7) with root_page_pfn=0x10 via the
     * public 0x38 opcode. */
    define_host_task_via_dispatch(p, 7u, /*flags*/4u, /*root_pfn*/0x10u,
                                  /*stamp*/0xa5a50001u);

    /* Translate dev_addr=0x100 -> page_idx=0, page_off=0x100, data_pfn=0x12,
     * gpa = 0x12000 + 0x100 = 0x12100, run_len = 0x1000-0x100 = 0xf00. */
    uint64_t gpa = 0, run = 0;
    bool ok = lagfx_task_translate(p, 7u, 0x100ull, &gpa, &run);
    CHECK(ok, "positive: lagfx_task_translate returns true");
    CHECK(gpa == 0x12100ull,
          "positive: dev=0x100 -> gpa=0x12100");
    CHECK(run == (0x1000ull - 0x100ull),
          "positive: out_run_len = bytes-to-page-end (0xf00)");

    lagfx_device_free(dev);
}

static void test_translate_unknown_task_id(void) {
    fprintf(stdout, "\n--- test: translate_unknown_task_id ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint64_t gpa = 0xdeadbeefull, run = 0xdeadbeefull;
    bool ok = lagfx_task_translate(p, /*task_id*/ 999u, 0x100ull, &gpa, &run);
    CHECK(!ok, "unknown task_id -> false");
    /* The function should NOT touch out_gpa/run_len on miss in
     * documented behaviour. We don't strictly assert that here — the
     * spec is "returns false; caller falls back to literal GPA". */
    lagfx_device_free(dev);
}

static void test_translate_zero_root_pfn(void) {
    fprintf(stdout, "\n--- test: translate_zero_root_pfn ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Define a host task with root_pfn=0 — translate must refuse. */
    define_host_task_via_dispatch(p, 7u, 4u, /*root_pfn*/0u, 0xa5a50002u);

    uint64_t gpa = 0, run = 0;
    bool ok = lagfx_task_translate(p, 7u, 0x100ull, &gpa, &run);
    CHECK(!ok, "root_page_pfn==0 -> translate returns false");

    lagfx_device_free(dev);
}

static void test_translate_zero_data_pfn(void) {
    fprintf(stdout, "\n--- test: translate_zero_data_pfn ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* PFN-array entry [0]=0 -> translate sees data_pfn==0 -> false +
     * around-bytes diagnostic. */
    uint32_t entries[4] = {0u, 0xaau, 0u, 0u};
    prep_pfn_array_in_heap(&shell, 0x10u, entries, 4);
    define_host_task_via_dispatch(p, 7u, 4u, 0x10u, 0xa5a50003u);

    uint64_t gpa = 0xdeadbeefull, run = 0xdeadbeefull;
    bool ok = lagfx_task_translate(p, 7u, 0x100ull, &gpa, &run);
    CHECK(!ok, "data_pfn==0 -> translate returns false (around-bytes "
               "diagnostic logged)");

    lagfx_device_free(dev);
}

static void test_translate_dev_addr_crosses_page(void) {
    fprintf(stdout, "\n--- test: translate_dev_addr_crosses_page ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Two PFN-array entries: data_pfn[0]=0x12, data_pfn[1]=0x13. */
    uint32_t entries[4] = {0x12u, 0x13u, 0u, 0u};
    prep_pfn_array_in_heap(&shell, 0x10u, entries, 4);
    define_host_task_via_dispatch(p, 7u, 4u, 0x10u, 0xa5a50004u);

    /* dev_addr = 0xfff (last byte of page 0). page_idx=0, off=0xfff,
     * data_pfn=0x12, gpa=0x12fff, run_len = 0x1000-0xfff = 1. */
    uint64_t gpa = 0, run = 0;
    bool ok = lagfx_task_translate(p, 7u, 0x0fffull, &gpa, &run);
    CHECK(ok, "page-crossing dev_addr: translate succeeds");
    CHECK(gpa == 0x12fffull,
          "page-crossing dev_addr: gpa = data_pfn<<12 + page_off");
    CHECK(run == 1u,
          "page-crossing dev_addr: out_run_len = bytes-to-page-end (1)");

    /* dev_addr = 0x1000 (first byte of page 1). page_idx=1, off=0,
     * data_pfn=0x13, gpa=0x13000, run_len = 0x1000. */
    ok = lagfx_task_translate(p, 7u, 0x1000ull, &gpa, &run);
    CHECK(ok, "page-1 dev_addr: translate succeeds");
    CHECK(gpa == 0x13000ull,
          "page-1 dev_addr: gpa picks data_pfn=0x13 from entry[1]");
    CHECK(run == 0x1000ull,
          "page-1 dev_addr: out_run_len = full page (0x1000)");

    lagfx_device_free(dev);
}

static void test_translate_read_memory_fails(void) {
    fprintf(stdout, "\n--- test: translate_read_memory_fails ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t entries[4] = {0x12u, 0u, 0u, 0u};
    prep_pfn_array_in_heap(&shell, 0x10u, entries, 4);
    define_host_task_via_dispatch(p, 7u, 4u, 0x10u, 0xa5a50005u);

    /* Force the next read_memory call to fail. The handler call above
     * doesn't read_memory itself — it only updates the task table —
     * so this fail-flag survives until lagfx_task_translate is called. */
    shell.next_read_fails = 1;
    uint64_t gpa = 0xdeadbeefull, run = 0xdeadbeefull;
    bool ok = lagfx_task_translate(p, 7u, 0x100ull, &gpa, &run);
    CHECK(!ok, "shell.read_memory failure -> translate returns false");

    lagfx_device_free(dev);
}

/* === Item 5 tests: lagfx_op_define_host_task =========================== */

static void test_define_host_task_creates_new(void) {
    fprintf(stdout, "\n--- test: define_host_task_creates_new ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* No prior DefineTask2 — host_task should allocate a fresh slot. */
    define_host_task_via_dispatch(p, /*task_id*/ 13u,
                                  /*flags*/ 4u,
                                  /*root_pfn*/ 0x100u,
                                  /*stamp*/ 0xb0000001u);
    lagfx_task_entry_t *e = lagfx_protocol_find_task(p, 13u);
    CHECK(e != NULL, "DefineHostTask allocated a fresh slot");
    CHECK(e && e->live, "new task slot is live");
    CHECK(e && e->id == 13u, "new task slot id matches");
    CHECK(e && e->root_page_pfn == 0x100u,
          "new task slot root_page_pfn matches payload");

    lagfx_device_free(dev);
}

static void test_define_host_task_updates_existing(void) {
    fprintf(stdout, "\n--- test: define_host_task_updates_existing ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* First DefineHostTask establishes the slot. */
    define_host_task_via_dispatch(p, 9u, 4u, 0x200u, 0xb0000002u);
    lagfx_task_entry_t *e = lagfx_protocol_find_task(p, 9u);
    CHECK(e && e->root_page_pfn == 0x200u,
          "initial DefineHostTask sets root_page_pfn");

    /* Second DefineHostTask with the same task_id but new root_page_pfn
     * must update in place — kext re-emits this opcode periodically. */
    define_host_task_via_dispatch(p, 9u, 4u, 0x300u, 0xb0000003u);
    e = lagfx_protocol_find_task(p, 9u);
    CHECK(e && e->root_page_pfn == 0x300u,
          "subsequent DefineHostTask overwrites root_page_pfn in place");
    /* No new slot was created — same entry. */
    CHECK(e && e->id == 9u, "same slot still id=9 (no duplicate)");

    lagfx_device_free(dev);
}

static void test_define_host_task_min_payload_guard(void) {
    fprintf(stdout, "\n--- test: define_host_task_min_payload_guard ---\n");
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* 12B-payload (header-only) is < 16B and must be rejected. The
     * dispatcher's min_payload check (see opcodes.c entry: min=16) traps
     * before the handler runs — we should still observe ERR_SIZE and
     * the stamp should still signal (fail-open). */
    uint8_t hdr_only[LAGFX_CMD_HEADER_BYTES + 8];
    build_header(hdr_only, LAGFX_OP_DEFINE_HOST_TASK, 0,
                 (uint32_t)sizeof(hdr_only), 0xb0000004u);
    /* fill payload with 8 bytes (payload=8 < 16). */
    for (int i = 0; i < 8; ++i) hdr_only[12 + i] = (uint8_t)i;
    int rc = lagfx_protocol_dispatch_one(p, hdr_only, sizeof(hdr_only));
    CHECK(rc == LAGFX_HANDLER_ERR_SIZE,
          "DefineHostTask payload<16 -> ERR_SIZE");
    /* No task_id was parsed since the handler bailed. */
    CHECK(lagfx_protocol_find_task(p, 0u) == NULL,
          "DefineHostTask short-payload did NOT register a task");
    /* Stamp still signalled (fail-open). */
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xb0000004u,
          "DefineHostTask short-payload still signals stamp (fail-open)");

    lagfx_device_free(dev);
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/m4-task-translate: starting\n");

    /* Item 4. */
    test_translate_positive();
    test_translate_unknown_task_id();
    test_translate_zero_root_pfn();
    test_translate_zero_data_pfn();
    test_translate_dev_addr_crosses_page();
    test_translate_read_memory_fails();

    /* Item 5. */
    test_define_host_task_creates_new();
    test_define_host_task_updates_existing();
    test_define_host_task_min_payload_guard();

    fprintf(stdout, "\n=== m4-task-translate: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
