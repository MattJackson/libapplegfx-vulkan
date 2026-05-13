/*
 * libapplegfx-vulkan — M4 per-channel ring drain (BAR0+0x1020) tests
 * tests/m4-doorbell-drain.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers item 9 from the M3/M4 critical-path coverage plan: per-channel
 * ring drain in protocol.c case 0x1020.
 *
 *   - ch=1..4 walks the ring with lagfx_protocol_dispatch_one_no_stamp.
 *   - ch=5..7 keeps the existing setupSharedState path.
 *   - PFN-array indirection: page0[0] vs the +0x1000 fallback.
 *   - Multiple cmds in one drain -> all dispatched, last_stamp captured.
 *
 * Strategy: build a synthetic shared page with a 20B descriptor at
 * +0x400+20*(ch-1) describing the ring, plus a synthetic ring page
 * containing a PFN-array indirection that points to a data page holding
 * one or more 12B-header cmds. Then issue MMIO writes against
 * BAR0+0x1020 to trigger the drain and assert the side effects.
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

/* === Mock shell with a 1MiB heap mirror =============================== */

#define HEAP_BYTES (1u << 20)

typedef struct {
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
    uint64_t last_write_gpa;
    uint64_t last_write_len;

    uint64_t heap_gpa;
    uint8_t *heap;
} db_shell_t;

static lagfx_task_t *db_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void db_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool db_map(void *op, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool db_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool db_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    db_shell_t *m = (db_shell_t *)op;
    m->read_memory_count++;
    if (d) {
        if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + HEAP_BYTES) {
            memcpy(d, m->heap + (gpa - m->heap_gpa), (size_t)l);
            return true;
        }
        memset(d, 0, (size_t)l);
    }
    return true;
}
static bool db_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    db_shell_t *m = (db_shell_t *)op;
    m->write_memory_count++;
    m->last_write_gpa = gpa;
    m->last_write_len = l;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + HEAP_BYTES) {
        memcpy(m->heap + (gpa - m->heap_gpa), s, (size_t)l);
    }
    return true;
}
static void db_irq(void *op, uint32_t vec) {
    db_shell_t *m = (db_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
}

static void db_shell_init(db_shell_t *m, uint64_t base) {
    memset(m, 0, sizeof(*m));
    m->heap_gpa = base;
    m->heap = calloc(1, HEAP_BYTES);
    if (!m->heap) {
        fprintf(stderr, "FATAL: heap alloc failed\n");
        exit(2);
    }
}
static void db_shell_free(db_shell_t *m) {
    free(m->heap);
    m->heap = NULL;
}

static lagfx_device_t *make_dev(db_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = db_create_task;
    d.shell.destroy_task    = db_destroy_task;
    d.shell.map_memory      = db_map;
    d.shell.unmap_memory    = db_unmap;
    d.shell.read_memory     = db_read;
    d.shell.write_memory    = db_write;
    d.shell.raise_interrupt = db_irq;
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) {
        fprintf(stderr, "FATAL: device_new failed: %s\n", err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* === Layout helpers =================================================== */

static void put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8)  & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
}
static uint32_t get_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* Place a 20B descriptor at shared_pfn<<12 + 0x400 + 20*(ch-1) with the
 * verified field order { write_ptr, read_ptr, mid, chan_id, ring_pfn }. */
static void place_descr(db_shell_t *m, uint32_t shared_pfn, unsigned ch,
                        uint32_t write_ptr, uint32_t read_ptr,
                        uint32_t mid, uint32_t chan_id, uint32_t ring_pfn) {
    uint64_t descr_gpa = (uint64_t)shared_pfn * 0x1000ull
                         + 0x400u + 20ull * (ch - 1u);
    uint8_t *p = m->heap + (descr_gpa - m->heap_gpa);
    put_le32(p + 0,  write_ptr);
    put_le32(p + 4,  read_ptr);
    put_le32(p + 8,  mid);
    put_le32(p + 12, chan_id);
    put_le32(p + 16, ring_pfn);
}

/* Build a 12B cmd header in `out`. */
static void put_cmd_header(uint8_t *out, uint16_t opcode, uint16_t arg_count_8b,
                           uint32_t total_length, uint32_t stamp) {
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
}

/* Arm the protocol's MMIO ring-state so the doorbell handler has a
 * shared_pfn + ring_base_pfn. */
static void arm_doorbell_state(lagfx_device_t *dev,
                               uint32_t shared_pfn,
                               uint32_t ring_base_pfn) {
    /* 0x101c -> ring_shared_page_pfn. */
    lagfx_mmio_write(dev, 0x101cu, shared_pfn);
    /* 0x1030 -> ring_base_pfn (so stamp_cell writes have a base too). */
    lagfx_mmio_write(dev, 0x1030u, ring_base_pfn);
    /* 0x1010 -> ring_start_offset = 0x1000 (default). */
    lagfx_mmio_write(dev, 0x1010u, 0x1000u);
}

/* === Item 9 tests ===================================================== */

/* ch=1..4 walks the ring via dispatch_one_no_stamp. Single cmd with NOP
 * opcode. Use page0 PFN-array indirection (page0[0] = data_pfn). */
static void test_doorbell_ch1_walks_ring_one_cmd(void) {
    fprintf(stdout, "\n--- test: doorbell_ch1_walks_ring_one_cmd ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn   = 0x40001u;
    uint32_t ring_pfn     = 0x40002u;
    uint32_t data_pfn     = 0x40003u;
    uint32_t ring_base_pfn= 0x40004u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* page0[0] = data_pfn (PFN-array indirection). */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page, data_pfn);

    /* Place one 12B NOP cmd at data_pfn<<12 + read_ptr=0. */
    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page,
                   /*opcode=*/LAGFX_OP_NOP,
                   /*arg_count_8b=*/0,
                   /*total_length=*/12u,
                   /*stamp=*/0xfeed0001u);

    /* Descriptor: write_ptr=12, read_ptr=0, chan_id=ch=1, ring_pfn=ring_pfn. */
    place_descr(&shell, shared_pfn, /*ch=*/1u,
                /*write_ptr=*/12u, /*read_ptr=*/0u,
                /*mid=*/0u, /*chan_id=*/1u, ring_pfn);

    uint64_t seen_before = 0, comp_before = 0, unk_before = 0;
    lagfx_protocol_stats(p, &seen_before, &comp_before, &unk_before);

    /* Doorbell: write ch=1 to 0x1020. */
    lagfx_mmio_write(dev, 0x1020u, 1u);

    uint64_t seen_after = 0, comp_after = 0, unk_after = 0;
    lagfx_protocol_stats(p, &seen_after, &comp_after, &unk_after);

    CHECK(seen_after - seen_before == 1u,
          "ch=1 doorbell: dispatch_one_no_stamp invoked once");
    /* dispatch_one_no_stamp does NOT auto-complete the stamp, so
     * total_cmds_completed should NOT have advanced. */
    CHECK(comp_after == comp_before,
          "ch=1 doorbell: dispatch_one_no_stamp did NOT auto-complete stamp");

    /* The drain advances descr.read_ptr to write_ptr. */
    uint64_t descr_gpa = (uint64_t)shared_pfn * 0x1000ull + 0x400u;
    uint32_t new_rp = get_le32(shell.heap + (descr_gpa - shell.heap_gpa) + 4u);
    CHECK(new_rp == 12u,
          "ch=1 doorbell: descr.read_ptr advanced 0->12");

    /* Bit ch=1 set in pending_stamps_bitmask. */
    uint32_t mask = lagfx_mmio_read(dev, 0x1018u);
    CHECK((mask & (1u << 1)) != 0u,
          "ch=1 doorbell: pending_stamps_bitmask bit 1 set");

    /* IRQ fired at least once. */
    CHECK(shell.raise_irq_count >= 1u,
          "ch=1 doorbell: raise_interrupt invoked at least once");

    /* Stamp cell at FIFO+ch*4. */
    uint64_t cell_gpa = (uint64_t)ring_base_pfn * 0x1000ull + 1u * 4u;
    uint32_t cell = get_le32(shell.heap + (cell_gpa - shell.heap_gpa));
    CHECK(cell == 0xfeed0001u,
          "ch=1 doorbell: stamp_cell[1] := last cmd's stamp (0xfeed0001)");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* Multi-cmd walk: 3 cmds. last_stamp captured = third cmd's stamp. */
static void test_doorbell_ch2_walks_multi_cmd(void) {
    fprintf(stdout, "\n--- test: doorbell_ch2_walks_multi_cmd ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn = 0x40001u;
    uint32_t ring_pfn = 0x40005u;
    uint32_t data_pfn = 0x40006u;
    uint32_t ring_base_pfn = 0x40007u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* PFN-array. */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page, data_pfn);

    /* Three back-to-back 12B NOPs at data_pfn page. */
    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page +  0, LAGFX_OP_NOP, 0, 12u, 0xa1111111u);
    put_cmd_header(data_page + 12, LAGFX_OP_NOP, 0, 12u, 0xa2222222u);
    put_cmd_header(data_page + 24, LAGFX_OP_NOP, 0, 12u, 0xa3333333u);

    place_descr(&shell, shared_pfn, /*ch=*/2u,
                /*write_ptr=*/36u, /*read_ptr=*/0u,
                /*mid=*/0u, /*chan_id=*/2u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 2u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after - seen_before == 3u,
          "ch=2 doorbell: 3 cmds dispatched in one drain");

    /* descr.read_ptr -> write_ptr=36. */
    uint64_t descr_gpa = (uint64_t)shared_pfn * 0x1000ull + 0x400u
                         + 20u * (2u - 1u);
    uint32_t new_rp = get_le32(shell.heap + (descr_gpa - shell.heap_gpa) + 4u);
    CHECK(new_rp == 36u,
          "ch=2 doorbell: descr.read_ptr advanced 0->36");

    /* Stamp cell carries the LAST cmd's stamp (per implementation). */
    uint64_t cell_gpa = (uint64_t)ring_base_pfn * 0x1000ull + 2u * 4u;
    uint32_t cell = get_le32(shell.heap + (cell_gpa - shell.heap_gpa));
    CHECK(cell == 0xa3333333u,
          "ch=2 doorbell: stamp_cell[2] := LAST cmd's stamp");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* page0[0]=0 means the kext hasn't published a data PFN yet — the drain
 * must bail rather than read from a probabilistic +0x1000 location. The
 * old fallback (ring_gpa + 0x1000) only worked when the kernel allocator
 * happened to give physically-contiguous pages; it broke on fragmented
 * heaps (cf. paravirt-re/library/state-machines/per-channel-ring-pfn-array.md).
 *
 * Walker behavior on page0[0]=0: read_memory of cmd header bails (PFN=0
 * sentinel), warning logged, drain advances read_ptr to write_ptr to
 * unblock the kext, NO cmd dispatched. */
static void test_doorbell_ch3_bails_when_page0_zero(void) {
    fprintf(stdout, "\n--- test: doorbell_ch3_bails_when_page0_zero ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn = 0x40001u;
    uint32_t ring_pfn   = 0x40010u;
    uint32_t ring_base_pfn = 0x40020u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* page0[0] = 0 -> walker bails (no dispatch). */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page, 0u);

    place_descr(&shell, shared_pfn, /*ch=*/3u,
                12u, 0u, 0u, 3u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 3u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after - seen_before == 0u,
          "ch=3 doorbell with page0[0]=0: walker bails, NO cmd dispatched");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* ch=5 must follow the existing setupSharedState path, NOT the per-channel
 * ring-drain branch. We assert that:
 *  - exactly zero seen_after-seen_before increment from dispatch_one_no_stamp
 *    (the ch=5..7 branch doesn't dispatch via the protocol jump table);
 *  - descr.read_ptr is still advanced to write_ptr (the SS path also does
 *    this);
 *  - pending_displays_bitmask gets bit (ch-5) set (display IRQ path).
 */
static void test_doorbell_ch5_keeps_setupSharedState_path(void) {
    fprintf(stdout, "\n--- test: doorbell_ch5_keeps_ss_path ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn = 0x40001u;
    uint32_t ring_pfn   = 0x40030u;
    uint32_t ss_pfn     = 0x40040u;
    uint32_t ring_base_pfn = 0x40050u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* page0[0] = data_pfn */
    uint32_t data_pfn = 0x40031u;
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page, data_pfn);

    /* Place a cmd with a 12B header + 8B payload (display_index=0,
     * ss_pfn=0x40040). Total length=20, write_ptr=20. */
    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page, /*opcode=*/0x01u, 0,
                   /*total_length=*/20u, /*stamp=*/0xdd550001u);
    put_le32(data_page + 12, /*display_index=*/0u);
    put_le32(data_page + 16, /*ss_pfn=*/ss_pfn);

    place_descr(&shell, shared_pfn, /*ch=*/5u,
                /*write_ptr=*/20u, /*read_ptr=*/0u,
                /*mid=*/0u, /*chan_id=*/5u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);

    lagfx_mmio_write(dev, 0x1020u, 5u);

    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    /* The ch=5 branch does NOT call dispatch_one_no_stamp; it only reads
     * the cmd header for display_index/ss_pfn and writes shared-state
     * fields. So total_cmds_seen should NOT advance. */
    CHECK(seen_after == seen_before,
          "ch=5 doorbell: did NOT route through dispatch_one_no_stamp "
          "(setupSharedState path preserved)");

    /* descr.read_ptr advanced to write_ptr (handled by the SS branch too). */
    uint64_t descr_gpa = (uint64_t)shared_pfn * 0x1000ull + 0x400u
                         + 20u * (5u - 1u);
    uint32_t new_rp = get_le32(shell.heap + (descr_gpa - shell.heap_gpa) + 4u);
    CHECK(new_rp == 20u, "ch=5 doorbell: descr.read_ptr advanced 0->20");

    /* Display bitmask bit (ch-5)=0 should NOT be set for setupSharedState
     * (opcode 0x01) — display-online is deferred to the vblank timer. */
    uint32_t disp_mask = lagfx_mmio_read(dev, 0x1014u);
    CHECK((disp_mask & 0x1u) == 0u,
          "ch=5 doorbell: pending_displays_bitmask bit 0 NOT set "
          "(deferred to vblank timer for setupSharedState)");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* ch=6: same setupSharedState path. Display bit 1 set. */
static void test_doorbell_ch6_keeps_ss_path(void) {
    fprintf(stdout, "\n--- test: doorbell_ch6_keeps_ss_path ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    (void)p;

    uint32_t shared_pfn    = 0x40001u;
    uint32_t ring_pfn      = 0x40060u;
    uint32_t data_pfn      = 0x40061u;
    uint32_t ss_pfn        = 0x40070u;
    uint32_t ring_base_pfn = 0x40080u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    put_le32(shell.heap + (ring_page_gpa - shell.heap_gpa), data_pfn);

    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page, /*opcode=*/0x01u, 0, 20u, 0xdd660001u);
    put_le32(data_page + 12, /*display_index=*/1u);
    put_le32(data_page + 16, /*ss_pfn=*/ss_pfn);

    place_descr(&shell, shared_pfn, /*ch=*/6u,
                20u, 0u, 0u, 6u, ring_pfn);

    lagfx_mmio_write(dev, 0x1020u, 6u);

    uint32_t disp_mask = lagfx_mmio_read(dev, 0x1014u);
    CHECK((disp_mask & (1u << 1)) == 0u,
          "ch=6 doorbell: pending_displays_bitmask bit 1 NOT set "
          "(deferred to vblank timer for setupSharedState)");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* === Edge-case coverage (item 9 — guest-controlled drain inputs) ==== */

/* PFN-walk that crosses a page boundary: place a 12B header at offset
 * 0xff8 so 8 bytes live in PFN-array entry 0 and the remaining 4 bytes
 * live in entry 1. Walker must stitch the read across two PFN lookups
 * and dispatch a single NOP. */
static void test_doorbell_ch1_pfn_walk_crosses_page_boundary(void) {
    fprintf(stdout, "\n--- test: doorbell_ch1_pfn_walk_crosses_page_boundary ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn    = 0x40001u;
    uint32_t ring_pfn      = 0x40090u;
    uint32_t data_pfn_a    = 0x40091u;
    uint32_t data_pfn_b    = 0x40092u;
    uint32_t ring_base_pfn = 0x40093u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* PFN-array: page0[0] = data_pfn_a, page0[1] = data_pfn_b. */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page + 0, data_pfn_a);
    put_le32(ring_page + 4, data_pfn_b);

    /* Build a 12B NOP header in scratch then write the first 8 bytes to
     * data_pfn_a + 0xff8 and the remaining 4 bytes to data_pfn_b + 0. */
    uint8_t hdr[12];
    put_cmd_header(hdr, /*opcode=*/LAGFX_OP_NOP, /*arg_count_8b=*/0,
                   /*total_length=*/12u, /*stamp=*/0xc0c00001u);
    uint8_t *page_a = shell.heap
        + ((uint64_t)data_pfn_a * 0x1000ull - shell.heap_gpa);
    uint8_t *page_b = shell.heap
        + ((uint64_t)data_pfn_b * 0x1000ull - shell.heap_gpa);
    memcpy(page_a + 0xff8u, hdr + 0, 8u);
    memcpy(page_b + 0u,     hdr + 8, 4u);

    /* read_ptr=0xff8, write_ptr=0x1004 (one 12B cmd straddling the
     * page boundary at 0x1000). chan_id=ch=1. */
    place_descr(&shell, shared_pfn, /*ch=*/1u,
                /*write_ptr=*/0x1004u, /*read_ptr=*/0xff8u,
                /*mid=*/0u, /*chan_id=*/1u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 1u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after - seen_before == 1u,
          "ch=1 doorbell with cmd straddling PFN boundary: 1 cmd dispatched");

    /* descr.read_ptr -> write_ptr=0x1004. */
    uint64_t descr_gpa = (uint64_t)shared_pfn * 0x1000ull + 0x400u;
    uint32_t new_rp = get_le32(shell.heap + (descr_gpa - shell.heap_gpa) + 4u);
    CHECK(new_rp == 0x1004u,
          "ch=1 doorbell straddle: descr.read_ptr advanced 0xff8->0x1004");

    /* Stamp cell carries the cmd's stamp. */
    uint64_t cell_gpa = (uint64_t)ring_base_pfn * 0x1000ull + 1u * 4u;
    uint32_t cell = get_le32(shell.heap + (cell_gpa - shell.heap_gpa));
    CHECK(cell == 0xc0c00001u,
          "ch=1 doorbell straddle: stamp_cell[1] := stitched cmd's stamp");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* page0[0]=valid_pfn but page0[1]=0: header read straddling the page
 * boundary hits the pte_pfn==0 sentinel during the SECOND PFN lookup
 * (mid-walk, not at the entry point). Walker must bail without
 * dispatching, and read_ptr stays at the unprocessed offset. */
static void test_doorbell_ch1_pte_pfn_zero_mid_walk(void) {
    fprintf(stdout, "\n--- test: doorbell_ch1_pte_pfn_zero_mid_walk ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn    = 0x40001u;
    uint32_t ring_pfn      = 0x400a0u;
    uint32_t data_pfn_a    = 0x400a1u;
    uint32_t ring_base_pfn = 0x400a3u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* PFN-array: page0[0]=data_pfn_a (valid), page0[1]=0 (sentinel). */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    uint8_t *ring_page = shell.heap + (ring_page_gpa - shell.heap_gpa);
    put_le32(ring_page + 0, data_pfn_a);
    put_le32(ring_page + 4, 0u);

    /* Pre-populate the first 8 bytes of the header at data_pfn_a+0xff8.
     * The walker will read those 8 bytes, then look up page0[1] for the
     * remaining 4 bytes — and find pte_pfn=0, so it bails. */
    uint8_t hdr[12];
    put_cmd_header(hdr, LAGFX_OP_NOP, 0, 12u, 0xbadbad01u);
    uint8_t *page_a = shell.heap
        + ((uint64_t)data_pfn_a * 0x1000ull - shell.heap_gpa);
    memcpy(page_a + 0xff8u, hdr + 0, 8u);

    place_descr(&shell, shared_pfn, /*ch=*/1u,
                /*write_ptr=*/0x1004u, /*read_ptr=*/0xff8u,
                /*mid=*/0u, /*chan_id=*/1u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 1u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after - seen_before == 0u,
          "ch=1 doorbell with pte_pfn=0 mid-walk: walker bails, NO cmd dispatched");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* Oversized cmd_len in the on-wire header triggers the `cmd_len >
 * (write_ptr - cur_rp)` rejection in protocol.c (line ~978). Header
 * claims 0x10000 but only 12 bytes are advertised by write_ptr.
 * Walker must reject WITHOUT dispatching. */
static void test_doorbell_ch1_oversized_cmd_len_rejected(void) {
    fprintf(stdout, "\n--- test: doorbell_ch1_oversized_cmd_len_rejected ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn    = 0x40001u;
    uint32_t ring_pfn      = 0x400b0u;
    uint32_t data_pfn      = 0x400b1u;
    uint32_t ring_base_pfn = 0x400b2u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    put_le32(shell.heap + (ring_page_gpa - shell.heap_gpa), data_pfn);

    /* Header advertises total_length=0x10000 (64 KiB) but write_ptr is
     * only 12 — only the header is actually published. Walker must
     * reject the bogus length and not dispatch. */
    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page, /*opcode=*/LAGFX_OP_NOP, 0,
                   /*total_length=*/0x10000u, /*stamp=*/0xdeadbe01u);

    place_descr(&shell, shared_pfn, /*ch=*/1u,
                /*write_ptr=*/12u, /*read_ptr=*/0u,
                /*mid=*/0u, /*chan_id=*/1u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 1u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after == seen_before,
          "ch=1 doorbell with cmd_len > (write_ptr-cur_rp): rejected, NO dispatch");

    /* Stamp cell should NOT have been written (no successful dispatch). */
    uint64_t cell_gpa = (uint64_t)ring_base_pfn * 0x1000ull + 1u * 4u;
    uint32_t cell = get_le32(shell.heap + (cell_gpa - shell.heap_gpa));
    CHECK(cell == 0u,
          "ch=1 doorbell oversized cmd_len: stamp_cell[1] not advanced");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* cmd_len that fits within write_ptr but exceeds the 64 KiB bounce
 * buffer (line ~995 in protocol.c). Header advertises cmd_len=65540
 * with write_ptr=65540 — passes the (write_ptr - cur_rp) guard but
 * trips the bounce-buffer overflow rejection. Walker must bail. */
static void test_doorbell_ch1_cmd_len_exceeds_bounce_buffer(void) {
    fprintf(stdout, "\n--- test: doorbell_ch1_cmd_len_exceeds_bounce_buffer ---\n");
    db_shell_t shell;
    db_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    uint32_t shared_pfn    = 0x40001u;
    uint32_t ring_pfn      = 0x400c0u;
    uint32_t data_pfn      = 0x400c1u;
    uint32_t ring_base_pfn = 0x400c2u;
    arm_doorbell_state(dev, shared_pfn, ring_base_pfn);

    /* page0[0] = data_pfn (only the header lookup needs to succeed —
     * the body read is rejected before it runs). */
    uint64_t ring_page_gpa = (uint64_t)ring_pfn * 0x1000ull;
    put_le32(shell.heap + (ring_page_gpa - shell.heap_gpa), data_pfn);

    /* Header advertises cmd_len=65540 (0x10004) — 4 bytes past the
     * 65536-byte bounce buffer. write_ptr=65540 so the
     * (write_ptr-cur_rp) guard at line ~978 passes. */
    uint8_t *data_page = shell.heap
        + ((uint64_t)data_pfn * 0x1000ull - shell.heap_gpa);
    put_cmd_header(data_page, /*opcode=*/LAGFX_OP_NOP, 0,
                   /*total_length=*/0x10004u, /*stamp=*/0xdeadbe02u);

    place_descr(&shell, shared_pfn, /*ch=*/1u,
                /*write_ptr=*/0x10004u, /*read_ptr=*/0u,
                /*mid=*/0u, /*chan_id=*/1u, ring_pfn);

    uint64_t seen_before = 0;
    lagfx_protocol_stats(p, &seen_before, NULL, NULL);
    lagfx_mmio_write(dev, 0x1020u, 1u);
    uint64_t seen_after = 0;
    lagfx_protocol_stats(p, &seen_after, NULL, NULL);

    CHECK(seen_after == seen_before,
          "ch=1 doorbell with cmd_len > bounce buffer: rejected, NO dispatch");

    /* Stamp cell should NOT have been written. */
    uint64_t cell_gpa = (uint64_t)ring_base_pfn * 0x1000ull + 1u * 4u;
    uint32_t cell = get_le32(shell.heap + (cell_gpa - shell.heap_gpa));
    CHECK(cell == 0u,
          "ch=1 doorbell bounce-overflow: stamp_cell[1] not advanced");

    lagfx_device_free(dev);
    db_shell_free(&shell);
}

/* === main ============================================================ */

int main(void) {
#ifndef __linux__
    fprintf(stderr, "doorbell drain requires Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    fprintf(stdout, "tests/m4-doorbell-drain: starting\n");

    test_doorbell_ch1_walks_ring_one_cmd();
    test_doorbell_ch2_walks_multi_cmd();
    test_doorbell_ch3_bails_when_page0_zero();
    test_doorbell_ch5_keeps_setupSharedState_path();
    test_doorbell_ch6_keeps_ss_path();

    /* Edge-case coverage for guest-controlled drain inputs. */
    test_doorbell_ch1_pfn_walk_crosses_page_boundary();
    test_doorbell_ch1_pte_pfn_zero_mid_walk();
    test_doorbell_ch1_oversized_cmd_len_rejected();
    test_doorbell_ch1_cmd_len_exceeds_bounce_buffer();

    fprintf(stdout, "\n=== m4-doorbell-drain: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
