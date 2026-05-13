/*
 * libapplegfx-vulkan — M3 stamp-helper unit tests
 * tests/m3-stamp-helpers.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers items 1-3 from the M3/M4 critical-path coverage plan:
 *
 *   1. lagfx_advance_stamp_cell (static in src/protocol/protocol.c) —
 *      monotonic stamp-cell write helper. Not externally callable; we
 *      exercise it indirectly via lagfx_protocol_complete_stamp_slot,
 *      which is its only public caller besides the doorbell drain at
 *      0x1020. Behaviour we assert against the in-memory mirror buffer:
 *        - cur=0, target=5 -> writes 5
 *        - cur=5, target=3 -> writes 6 (max(target, cur+1))
 *        - cur=0, target=0 -> writes 1 (non-zero floor)
 *        - ring_base_pfn==0 -> no write at all
 *        - shell.write_memory==NULL -> no crash, no write
 *        - 5x calls with target=1 -> cell goes 1,2,3,4,5 (monotonic)
 *
 *   2. lagfx_protocol_complete_stamp_slot — public slot-aware completion.
 *        - slot=0 vs slot=5 set the correct bit in pending_stamps_bitmask
 *        - IRQ raised exactly once per call
 *        - cell at FIFO+slot*4 advances monotonically across calls
 *
 *   3. lagfx_protocol_complete_stamp — back-compat wrapper for slot=0.
 *
 * Drives the helpers through the public test-side API by wiring the
 * mock_shell_t's write_memory + read_memory callbacks at a synthetic
 * "ring page" so the cell GPA hits a buffer we own and can introspect.
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

/* === Mock shell — covers the cell-page mirror needed for stamp tests ==
 *
 * We back a synthetic 4KiB "ring base page" at gpa = ring_base_pfn << 12.
 * write_memory writes that fall inside the page mirror into ring_buf so
 * the test can inspect the actual cell value at slot*4. read_memory
 * reads serve from the same mirror so the helper's "load current cell"
 * path returns whatever we last wrote (or whatever the test pre-populated).
 */
typedef struct {
    /* Counters. */
    unsigned raise_irq_count;
    uint32_t last_irq_vector;
    unsigned read_memory_count;
    unsigned write_memory_count;

    /* Last write window (for assertions). */
    uint64_t last_write_gpa;
    uint64_t last_write_len;

    /* Suppress write_memory by setting suppress_writes=1 at runtime —
     * lets us model the shell.write_memory==NULL edge through the same
     * path. */
    int suppress_writes;
    int writes_return_value; /* 0 = false, !=0 = true. Default true. */

    /* Ring base page mirror — a 4KiB blob at ring_pfn<<12. */
    uint32_t ring_base_pfn;
    uint8_t  ring_buf[4096];
} m3_shell_t;

static lagfx_task_t *
m3_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void m3_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool m3_map(void *op, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool m3_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool m3_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    m3_shell_t *m = (m3_shell_t *)op;
    m->read_memory_count++;
    if (m->ring_base_pfn != 0u && d) {
        uint64_t base = (uint64_t)m->ring_base_pfn << 12;
        if (gpa >= base && gpa + l <= base + sizeof(m->ring_buf)) {
            memcpy(d, m->ring_buf + (gpa - base), (size_t)l);
            return true;
        }
    }
    /* Outside the mirror: zero-fill so unrelated reads observe stable
     * defaults. */
    if (d) memset(d, 0, (size_t)l);
    return true;
}
static bool m3_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    m3_shell_t *m = (m3_shell_t *)op;
    m->write_memory_count++;
    m->last_write_gpa = gpa;
    m->last_write_len = l;
    if (m->suppress_writes) {
        return m->writes_return_value != 0;
    }
    if (m->ring_base_pfn != 0u) {
        uint64_t base = (uint64_t)m->ring_base_pfn << 12;
        if (gpa >= base && gpa + l <= base + sizeof(m->ring_buf)) {
            memcpy(m->ring_buf + (gpa - base), s, (size_t)l);
        }
    }
    return m->writes_return_value != 0;
}
static void m3_irq(void *op, uint32_t vec) {
    m3_shell_t *m = (m3_shell_t *)op;
    m->raise_irq_count++;
    m->last_irq_vector = vec;
}

static lagfx_device_t *make_dev(m3_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = m3_create_task;
    d.shell.destroy_task    = m3_destroy_task;
    d.shell.map_memory      = m3_map;
    d.shell.unmap_memory    = m3_unmap;
    d.shell.read_memory     = m3_read;
    d.shell.write_memory    = m3_write;
    d.shell.raise_interrupt = m3_irq;
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) {
        fprintf(stderr, "FATAL: device_new failed: %s\n", err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* Helper: arm ring_base_pfn so the helper has a non-zero base.
 * lagfx_protocol_complete_stamp_slot reads the cell at
 * (ring_base_pfn<<12) + slot*4 — we set ring_base_pfn via the documented
 * MMIO write at 0x1030 (which protocol.c handles by stashing the value
 * into protocol->ring_base_pfn). */
static void arm_ring(lagfx_device_t *dev, m3_shell_t *shell,
                     uint32_t ring_pfn) {
    shell->ring_base_pfn = ring_pfn;  /* Set before MMIO so callbacks can find the page */
    shell->writes_return_value = 1;
    /* MMIO write 0x1030 -> ring_base_pfn (and ring_base_gpa). */
    lagfx_mmio_write(dev, 0x1030u, ring_pfn);
}

static uint32_t read_cell_le32(const m3_shell_t *m, uint32_t slot) {
    const uint8_t *p = m->ring_buf + slot * 4u;
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* === Tests ============================================================ */

/* Exercises lagfx_advance_stamp_cell's "cur=0, target=5 -> writes 5" path
 * via complete_stamp_slot(slot=0, stamp=5). */
static void test_advance_cur0_target5(void) {
    fprintf(stdout, "\n--- test: advance_cur0_target5 ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 1u);
    /* ring_buf is zero (cur=0). target=5 -> max(5, 0+1)=5. */
    lagfx_protocol_complete_stamp_slot(p, 0u, 5u);
    CHECK(read_cell_le32(&shell, 0) == 5u,
          "cur=0, target=5 -> cell becomes 5");
    CHECK(shell.raise_irq_count == 1u,
          "cur=0, target=5: IRQ raised exactly once");
    CHECK(shell.last_irq_vector == 0u,
          "cur=0, target=5: IRQ vec=0 (unified ISR)");
    lagfx_device_free(dev);
}

/* Exercises "cur=5, target=3 -> writes 6 (max(target, cur+1))". */
static void test_advance_cur5_target3(void) {
    fprintf(stdout, "\n--- test: advance_cur5_target3 ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 1u);
    /* Pre-populate the cell with 5 so the helper's read sees cur=5. */
    uint32_t pre = 5u;
    memcpy(shell.ring_buf, &pre, sizeof(pre));
    lagfx_protocol_complete_stamp_slot(p, 0u, 3u);
    /* target=3, cur=5, want=max(3, 5+1)=6. */
    CHECK(read_cell_le32(&shell, 0) == 6u,
          "cur=5, target=3 -> cell becomes 6 (max(target, cur+1))");
    lagfx_device_free(dev);
}

/* Exercises "cur=0, target=0 -> writes 1 (non-zero floor)". */
static void test_advance_cur0_target0(void) {
    fprintf(stdout, "\n--- test: advance_cur0_target0 ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 1u);
    /* Cell is 0, target=0. want=max(0, 0+1)=1. Floor enforces non-zero. */
    lagfx_protocol_complete_stamp_slot(p, 0u, 0u);
    CHECK(read_cell_le32(&shell, 0) == 1u,
          "cur=0, target=0 -> cell becomes 1 (non-zero floor)");
    lagfx_device_free(dev);
}

/* Exercises "ring_base_pfn==0 -> no write" gate. */
static void test_advance_no_ring_base(void) {
    fprintf(stdout, "\n--- test: advance_no_ring_base ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    /* Do NOT arm — ring_base_pfn stays 0. */
    shell.writes_return_value = 1;
    unsigned writes_before = shell.write_memory_count;
    lagfx_protocol_complete_stamp_slot(p, 0u, 7u);
    CHECK(shell.write_memory_count == writes_before,
          "ring_base_pfn==0 -> helper issues NO write_memory call");
    /* IRQ still fires unconditionally (cell write is the gated bit). */
    CHECK(shell.raise_irq_count == 1u,
          "ring_base_pfn==0 -> IRQ still raised (decoupled from cell write)");
    lagfx_device_free(dev);
}

/* Exercises "shell.write_memory==NULL -> no crash" gate. We can't actually
 * NULL-out the descriptor field after device_new without UB; instead
 * we use suppress_writes=1 + writes_return_value=0 to model the path
 * the helper sees on a bad write. */
static void test_advance_write_null_or_fails(void) {
    fprintf(stdout, "\n--- test: advance_write_null_or_fails ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 1u);
    /* Force write_memory to "succeed without effect" — same control-flow
     * as the (write_memory != NULL) precondition: helper still attempts
     * and either skips logging (return false) or logs (return true). The
     * non-crash invariant is the test. */
    shell.suppress_writes = 1;
    shell.writes_return_value = 0;
    lagfx_protocol_complete_stamp_slot(p, 0u, 1u);
    /* No assertion on cell value here (we suppressed the mirror update);
     * the bar is "we got here without crashing and still raised IRQ". */
    CHECK(shell.raise_irq_count == 1u,
          "write_memory failure path completes without crash + raises IRQ");
    lagfx_device_free(dev);
}

/* Exercises the multi-call monotonic invariant: 5 sequential calls with
 * target=1 each -> cell advances 1,2,3,4,5 because of the cur+1 floor. */
static void test_advance_monotonic_5x(void) {
    fprintf(stdout, "\n--- test: advance_monotonic_5x ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 2u);

    for (uint32_t i = 1; i <= 5u; ++i) {
        lagfx_protocol_complete_stamp_slot(p, 0u, /*target*/ 1u);
        char msg[64];
        snprintf(msg, sizeof(msg), "monotonic call #%u: cell == %u", i, i);
        uint32_t got = read_cell_le32(&shell, 0);
        if (got == i) {
            fprintf(stdout, "PASS: %s\n", msg);
            g_pass++;
        } else {
            fprintf(stderr, "FAIL: %s (got %u)\n", msg, got);
            g_fail++;
        }
    }
    CHECK(shell.raise_irq_count == 5u,
          "5 calls -> 5 IRQs raised");
    lagfx_device_free(dev);
}

/* === complete_stamp_slot bit-mask coverage ============================ */

static void test_slot_bitmask_slot0_vs_slot5(void) {
    fprintf(stdout, "\n--- test: slot_bitmask_slot0_vs_slot5 ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 3u);

    /* slot=0 -> bit 0 set in pending_stamps_bitmask. We can read the
     * bitmask via MMIO 0x1018 which xchg-clears it. */
    lagfx_protocol_complete_stamp_slot(p, 0u, 11u);
    uint32_t mask0 = lagfx_mmio_read(dev, 0x1018u);
    CHECK(mask0 == 0x1u, "slot=0 -> pending_stamps_bitmask bit 0 set");

    /* slot=5 -> bit 5. Mask was just cleared by the read above so this
     * is a fresh observation. */
    lagfx_protocol_complete_stamp_slot(p, 5u, 22u);
    uint32_t mask5 = lagfx_mmio_read(dev, 0x1018u);
    CHECK(mask5 == (1u << 5),
          "slot=5 -> pending_stamps_bitmask bit 5 set (xchg-clear semantics)");

    /* IRQ count: 2 calls -> 2 IRQs. */
    CHECK(shell.raise_irq_count == 2u,
          "two complete_stamp_slot calls produced two IRQs");

    /* Each call advanced the per-slot cell. Cell at slot 5 is at offset
     * 5*4 = 20 in ring_buf. */
    CHECK(read_cell_le32(&shell, 0) == 11u,
          "slot=0 cell carries first stamp");
    CHECK(read_cell_le32(&shell, 5) == 22u,
          "slot=5 cell carries second stamp");
    lagfx_device_free(dev);
}

/* Exercises "monotonic per-slot": same slot called twice with backwards
 * stamps still advances. */
static void test_slot_cell_monotonic_per_slot(void) {
    fprintf(stdout, "\n--- test: slot_cell_monotonic_per_slot ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 4u);

    lagfx_protocol_complete_stamp_slot(p, 3u, 100u);
    CHECK(read_cell_le32(&shell, 3) == 100u,
          "slot=3 first call: cell=100");
    /* Backwards stamp must NOT regress; advances to 101. */
    lagfx_protocol_complete_stamp_slot(p, 3u, 50u);
    CHECK(read_cell_le32(&shell, 3) == 101u,
          "slot=3 second call (target<cur): cell=cur+1=101");
    lagfx_device_free(dev);
}

/* === complete_stamp (slot=0) wrapper ================================== */

static void test_complete_stamp_wrapper_routes_to_slot0(void) {
    fprintf(stdout, "\n--- test: complete_stamp_wrapper_routes_to_slot0 ---\n");
    m3_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    arm_ring(dev, &shell, 5u);

    /* state.h declares lagfx_protocol_complete_stamp; we call it via that
     * declaration. The test asserts behaviour matches a slot=0 call to
     * complete_stamp_slot. */
    lagfx_protocol_complete_stamp(p, 42u);
    CHECK(read_cell_le32(&shell, 0) == 42u,
          "complete_stamp wrapper writes slot 0 cell");
    uint32_t mask = lagfx_mmio_read(dev, 0x1018u);
    CHECK(mask == 0x1u,
          "complete_stamp wrapper sets pending_stamps_bitmask bit 0");
    CHECK(shell.raise_irq_count == 1u,
          "complete_stamp wrapper raises IRQ");
    /* last_completed_stamp accessor matches. */
    CHECK(lagfx_protocol_last_completed_stamp(p) == 42u,
          "complete_stamp wrapper updates last_completed_stamp");
    lagfx_device_free(dev);
}

/* === main ============================================================ */

int main(void) {
#ifndef __linux__
    fprintf(stderr, "stamp helpers require Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    fprintf(stdout, "tests/m3-stamp-helpers: starting\n");

    test_advance_cur0_target5();
    test_advance_cur5_target3();
    test_advance_cur0_target0();
    test_advance_no_ring_base();
    test_advance_write_null_or_fails();
    test_advance_monotonic_5x();
    test_slot_bitmask_slot0_vs_slot5();
    test_slot_cell_monotonic_per_slot();
    test_complete_stamp_wrapper_routes_to_slot0();

    fprintf(stdout, "\n=== m3-stamp-helpers: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
