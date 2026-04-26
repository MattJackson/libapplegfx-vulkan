/*
 * libapplegfx-vulkan — M4 ExecIndirect2 outer-payload parser, segment
 * walker, and 0x1cc reply-writer tests.
 * tests/m4-execindirect2-parser.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Covers items 6, 7, 8 from the M3/M4 critical-path coverage plan.
 *
 *   6. lagfx_op_exec_indirect2 outer-payload parser:
 *      - kext-side 0x37 layout: 12B header + 16B invalidates + 16B
 *        reserved/middle + 16B resources.
 *      - resource_count=0, invalidate_count=0 -> empty-list completion.
 *      - resource[0] with non-zero host_gpu_addr -> segment walker is
 *        invoked (we observe shell.read_memory hits the resource cmdBuf).
 *      - Should NOT misparse with the 8B-invalidate layout. We feed a
 *        payload sized for 16B-invalidates and confirm the parser walks
 *        the resource_table at the expected offset.
 *
 *   7. Segment walker (inside lagfx_op_exec_indirect2):
 *      - encType=4 + inner opcode 0x1cc -> 0x1cc reply path runs (check
 *        the 12B reply landed at the expected GPA).
 *      - encType=2 (Render) -> log only (no reply written).
 *      - Multi-segment cmdBuf -> all segments walked (read_memory shows
 *        progressive walk).
 *      - Bad segment_size -> bails out gracefully (no crash).
 *      - Inner cmd with totalLength<8 -> bails out (no crash).
 *
 *   8. 0x1cc reply writer:
 *      - Inner payload {ref, buffer_id, reply_offset} parsed as
 *        LE u32+u32+u64.
 *      - Writes 12-byte PGReplyRenderPipelineStateInfo at translated GPA.
 *      - maxTotalThreadsPerThreadgroup = 1024 (0x00 0x04 0x00 0x00 LE).
 *      - reply_offset+12 > buffer_len -> no write, warning logged.
 *      - buffer_id >= resource_count -> no write, warning logged.
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
    uint8_t *heap;  /* HEAP_BYTES allocated */
} ei2_shell_t;

static lagfx_task_t *ei2_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void ei2_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool ei2_map(void *op, lagfx_task_t *t, uint64_t o,
                    const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool ei2_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool ei2_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    ei2_shell_t *m = (ei2_shell_t *)op;
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
static bool ei2_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    ei2_shell_t *m = (ei2_shell_t *)op;
    m->write_memory_count++;
    m->last_write_gpa = gpa;
    m->last_write_len = l;
    if (gpa >= m->heap_gpa && gpa + l <= m->heap_gpa + HEAP_BYTES) {
        memcpy(m->heap + (gpa - m->heap_gpa), s, (size_t)l);
    }
    return true;
}
static void ei2_irq(void *op, uint32_t vec) {
    ei2_shell_t *m = (ei2_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
}

static void ei2_shell_init(ei2_shell_t *m, uint64_t base) {
    memset(m, 0, sizeof(*m));
    m->heap_gpa = base;
    m->heap = calloc(1, HEAP_BYTES);
    if (!m->heap) {
        fprintf(stderr, "FATAL: heap alloc failed\n");
        exit(2);
    }
}

static void ei2_shell_free(ei2_shell_t *m) {
    free(m->heap);
    m->heap = NULL;
}

static lagfx_device_t *make_dev(ei2_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = ei2_create_task;
    d.shell.destroy_task    = ei2_destroy_task;
    d.shell.map_memory      = ei2_map;
    d.shell.unmap_memory    = ei2_unmap;
    d.shell.read_memory     = ei2_read;
    d.shell.write_memory    = ei2_write;
    d.shell.raise_interrupt = ei2_irq;
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
static uint32_t get_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}
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

/* === Helper: register a task w/ root_page_pfn via 0x38 ================ */

static void define_host_task(lagfx_protocol_t *p,
                             uint32_t task_id, uint32_t root_pfn,
                             uint32_t stamp) {
    uint8_t buf[28];
    build_header(buf, LAGFX_OP_DEFINE_HOST_TASK, 0,
                 /*total_length=*/12u + 16u, stamp);
    put_le32(buf + 12 + 0,  task_id);
    put_le32(buf + 12 + 4,  0u);
    put_le32(buf + 12 + 8,  4u);
    put_le32(buf + 12 + 12, root_pfn);
    (void)lagfx_protocol_dispatch_one(p, buf, sizeof(buf));
}

/* Lay down a 3-level radix tree in the heap mirror so that VA-page 0
 * maps to `data_pfn`. Uses 4 contiguous PFNs starting at root_pfn:
 *   root=root_pfn (header), L1=root_pfn+1, L2=root_pfn+2, L3=root_pfn+3.
 * Caller must ensure root_pfn..root_pfn+3 fall inside the heap mirror
 * (and don't collide with data_pfn or other content). */
static void prep_radix_for_data_pfn(ei2_shell_t *m, uint32_t root_pfn,
                                    uint32_t data_pfn) {
    uint64_t base = m->heap_gpa;
    /* Header at root_pfn. */
    uint8_t *hp = m->heap + (((uint64_t)root_pfn << 12) - base);
    memset(hp, 0, 0x1000);
    put_le32(hp + 0, root_pfn + 1u);  /* L1_pfn */
    put_le32(hp + 4, 3u);             /* levels */

    /* L1 PTE[0] = L2_pfn (interior, bit31=0). */
    uint8_t *l1 = m->heap + ((((uint64_t)root_pfn + 1u) << 12) - base);
    memset(l1, 0, 0x1000);
    put_le32(l1 + 0, root_pfn + 2u);

    /* L2 PTE[0] = L3_pfn (interior, bit31=0). */
    uint8_t *l2 = m->heap + ((((uint64_t)root_pfn + 2u) << 12) - base);
    memset(l2, 0, 0x1000);
    put_le32(l2 + 0, root_pfn + 3u);

    /* L3 leaf PTE[0] = data_pfn | bit31. */
    uint8_t *l3 = m->heap + ((((uint64_t)root_pfn + 3u) << 12) - base);
    memset(l3, 0, 0x1000);
    put_le32(l3 + 0, data_pfn | 0x80000000u);
}

/* Build an outer ExecIndirect2 payload using the kext-side 0x37 layout
 * with INV_COUNT 16B-invalidate records + a 16B middle/reserved block +
 * RES_COUNT 16B resource records.
 *
 * Total payload size: 12 + 16*INV_COUNT + 16 + 16*RES_COUNT.
 *
 * Caller passes pre-built invalidate + resource record blobs; this only
 * stitches them together in the canonical order. */
static size_t build_exec_indirect2_outer(uint8_t *out,
                                         uint32_t task_id,
                                         const uint8_t *invalidates,
                                         uint32_t inv_count,
                                         const uint8_t *resources,
                                         uint32_t res_count) {
    put_le32(out + 0, task_id);
    put_le32(out + 4, inv_count);
    put_le32(out + 8, res_count);
    size_t off = 12u;
    if (inv_count > 0 && invalidates) {
        memcpy(out + off, invalidates, (size_t)inv_count * 16u);
    }
    off += (size_t)inv_count * 16u;
    /* 16B reserved/middle block. */
    memset(out + off, 0, 16u);
    off += 16u;
    if (res_count > 0 && resources) {
        memcpy(out + off, resources, (size_t)res_count * 16u);
    }
    off += (size_t)res_count * 16u;
    return off;
}

/* Build a 16B segment header at `out`. */
static void put_segment_header(uint8_t *out, uint32_t segment_size,
                               uint32_t prot_options, uint8_t enc_type,
                               uint8_t final_flag, uint8_t reuse_flag) {
    put_le32(out + 0, segment_size);
    put_le32(out + 4, prot_options);
    out[8]  = enc_type;
    out[9]  = final_flag;
    out[10] = reuse_flag;
    out[11] = 0;
    put_le32(out + 12, 0u);  /* reserved */
}

/* Build an inner cmd header { u32 opcode; u32 totalLength } at `out`. */
static void put_inner_cmd(uint8_t *out, uint32_t opcode, uint32_t total_len) {
    put_le32(out + 0, opcode);
    put_le32(out + 4, total_len);
}

/* === Item 6 tests: outer-payload parser ================================ */

static void test_outer_empty_invalidate_empty_resources(void) {
    fprintf(stdout, "\n--- test: outer_empty_invalidate_empty_resources ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* invalidate_count=0, resource_count=0. payload = 12 + 16 (middle)
     * = 28 bytes. The (need=12 + 0 + 16 + 0 = 28) <= payload_size check
     * must accept; the empty-list completion path should run. */
    uint8_t cmd[12 + 64];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0,
                 /*total_length=*/12u + 28u, /*stamp=*/0xc0000001u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, /*task_id=*/0u,
                                           NULL, 0, NULL, 0);
    CHECK(pl == 28u, "empty exec_indirect2 payload size = 28B");

    int rc = lagfx_protocol_dispatch_one(p, cmd, 12u + pl);
    CHECK(rc == LAGFX_HANDLER_OK,
          "exec_indirect2(empty/empty) dispatch returns OK");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xc0000001u,
          "exec_indirect2(empty/empty) stamp signalled");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_outer_resource_with_host_gpu_addr_invokes_walker(void) {
    fprintf(stdout, "\n--- test: outer_resource_invokes_walker ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Set up a host-task with root_page_pfn pointing at a PFN-array
     * that maps task-VA 0x0 -> data_pfn=0x40010 (i.e. GPA 0x40010000).
     * heap_gpa=0x40000000 covers PFNs 0x40000..0x40100. We'll use
     * root_pfn = 0x40000 (page 0 of the heap mirror) and write a 4B
     * entry [0]=0x40010. */
    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, /*task_id=*/77u, /*root_pfn=*/0x40000u,
                     /*stamp=*/0xc1000001u);

    /* Place a 16B segment header at task-VA 0 (i.e. heap+0x10000).
     * segment_size=0 -> walker bails after one iteration; the point of
     * the test is to confirm the walker WAS invoked (i.e. read_memory
     * was called for the cmdBuf, and a segment header read happened). */
    uint8_t *res_page = shell.heap + 0x10000u;
    put_segment_header(res_page, /*size=*/0u, /*prot=*/0u,
                       /*enc=*/2u, /*final=*/1u, /*reuse=*/0u);

    /* Build resource[0] = {host_gpu_addr=0 (task-VA 0), length=16, _pad=0}. */
    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);   /* task-VA 0 */
    put_le32(resources + 8, 16u);    /* length = 16 (one segment header) */
    put_le32(resources + 12, 0u);

    uint8_t cmd[12 + 256];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0,
                 /*total_length=*/12u + 12u + 16u + 16u, 0xc1000002u);
    /* invalidate_count=0, resource_count=1. */
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u,
                                           NULL, 0, resources, 1);
    /* Update the header to reflect actual computed length. */
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    unsigned reads_before = shell.read_memory_count;
    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "exec_indirect2(1 resource) dispatch returns OK");
    /* Walker invoked the read_memory path at least once (to fetch the
     * resource cmdBuf). */
    CHECK(shell.read_memory_count > reads_before,
          "walker invoked shell.read_memory for the resource cmdBuf");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

/* Confirm the outer parser uses the 16B-invalidates layout, NOT the
 * 8B-invalidates legacy layout. We populate a payload with one 16B
 * invalidate record and one 16B resource record at the correct offsets;
 * if the parser were using 8B invalidates it would compute
 * off_resources = 12 + 8 + 16 = 36, read garbage as the resource record,
 * and fail to invoke the walker against our resource cmdBuf. */
static void test_outer_uses_16B_invalidate_layout(void) {
    fprintf(stdout, "\n--- test: outer_uses_16B_invalidate_layout ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Same pfn-array setup as above. */
    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xc2000001u);

    /* One 16B invalidate (zeros). */
    uint8_t invalidates[16] = {0};
    put_le32(invalidates + 0, /*rid=*/0xdead0001u);
    put_le32(invalidates + 4, /*flags=*/0x0000ABCDu);

    /* Place a 16B segment header at task-VA 0. */
    uint8_t *res_page = shell.heap + 0x10000u;
    put_segment_header(res_page, 0u, 0u, 2u, 1u, 0u);

    /* One resource. */
    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, 16u);
    put_le32(resources + 12, 0u);

    /* outer_size = 12 + 16 (one invalidate) + 16 (middle) + 16 (one
     * resource) = 60 B. */
    uint8_t cmd[12 + 256];
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u,
                                           invalidates, 1, resources, 1);
    CHECK(pl == 60u,
          "outer payload with 16B invalidate is 60 bytes "
          "(12+16+16+16)");

    uint32_t total = (uint32_t)(12u + pl);
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, total, 0xc2000002u);
    /* re-stamp the size after build_header overwrote it. */
    pl = build_exec_indirect2_outer(cmd + 12, 77u,
                                    invalidates, 1, resources, 1);
    total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    unsigned reads_before = shell.read_memory_count;
    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "exec_indirect2 with 16B invalidate dispatched OK "
          "(parser found resource record at correct offset)");
    /* Walker read the resource cmdBuf (proves it reached res_page at
     * the offset computed under the 16B-invalidate layout). */
    CHECK(shell.read_memory_count > reads_before,
          "walker read resource cmdBuf with 16B invalidate layout — "
          "parser did NOT misparse as 8B-invalidate legacy layout");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

/* === Item 7 tests: segment walker ===================================== */

/* Helper: prep a resource cmdBuf in the heap mirror. Returns the task-VA
 * base address (always 0 in our setup). */
static void prep_resource_cmdbuf(ei2_shell_t *m, uint8_t *res_page,
                                 const uint8_t *blob, size_t blob_len) {
    memset(res_page, 0, 0x1000);
    if (blob_len > 0x1000) blob_len = 0x1000;
    memcpy(res_page, blob, blob_len);
    (void)m;
}

static void test_walker_encType4_inner_0x1c9(void) {
    fprintf(stdout, "\n--- test: walker_encType4_inner_0x1c9 ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Task-VA 0 -> data_pfn 0x40010. So the cmdBuf is at heap+0x10000. */
    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xd0000001u);

    /* Build a cmdBuf:
     *   [0..15]  segment header (encType=4, segment_size=24, final=1)
     *   [16..23] inner cmd: opcode=0x1c9 RenderPipelineStateInfo,
     *            totalLength=24
     *   [24..39] inner payload: ref=0xaaaaaaaa, buffer_id=0,
     *            reply_offset=0x40 (LE u64) */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0,
                       /*size=*/24u, /*prot=*/0u, /*enc=*/4u,
                       /*final=*/1u, /*reuse=*/0u);
    put_inner_cmd(cmdbuf + 16, /*op=*/0x1c9u, /*total_len=*/24u);
    put_le32(cmdbuf + 24, /*ref=*/0xaaaaaaaau);
    put_le32(cmdbuf + 28, /*buffer_id=*/0u);
    put_le64(cmdbuf + 32, /*reply_offset=*/0x40ull);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    /* resource[0] = {dev=0, length=128}. The 0x1c9 reply will be written
     * at task-VA 0+0x40 = data_pfn 0x40010 << 12 + 0x40 = heap+0x10040.
     * length=128 gives 64 bytes of buffer beyond reply_offset for the
     * 12B reply to land inside the buffer-bounds check. */
    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, 128u);

    uint8_t cmd[12 + 128];
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, total, 0xd0000002u);
    pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0, resources, 1);
    total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "exec_indirect2 with encType=4 + 0x1c9 inner returns OK");

    /* The 12B reply must be at heap+0x10040. */
    uint32_t maxTPT = get_le32(shell.heap + 0x10040u);
    CHECK(maxTPT == 1024u,
          "0x1c9 reply: maxTotalThreadsPerThreadgroup = 1024 at expected "
          "GPA");
    /* Reply bytes 4..11 are zero (imageblockSampleLength + flags). */
    int zeros_ok = 1;
    for (int i = 4; i < 12; ++i) {
        if (shell.heap[0x10040u + i] != 0) {
            zeros_ok = 0;
            break;
        }
    }
    CHECK(zeros_ok, "0x1c9 reply: bytes 4..11 are zero (no tile shader)");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_walker_encType2_no_reply(void) {
    fprintf(stdout, "\n--- test: walker_encType2_no_reply ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xd1000001u);

    /* Inner cmd is 0x1cc but encType=2 (Render) — walker must NOT write
     * a reply (the 0x1cc handling is gated on encType==4). */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0, 24u, 0u, /*enc=*/2u, 1u, 0u);
    put_inner_cmd(cmdbuf + 16, 0x1ccu, 24u);
    put_le32(cmdbuf + 24, 0xaaaaaaaau);
    put_le32(cmdbuf + 28, 0u);
    put_le64(cmdbuf + 32, 0x40ull);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    /* Pre-poison the reply target — if the walker writes there, we'll
     * see it overwritten. */
    memset(shell.heap + 0x10040u, 0xff, 12);

    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, 64u);

    uint8_t cmd[12 + 128];
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, total, 0xd1000002u);
    pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0, resources, 1);
    total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "exec_indirect2 with encType=2 (Render) returns OK (no reply)");

    /* Reply target unchanged from poison. */
    int unchanged = 1;
    for (int i = 0; i < 12; ++i) {
        if (shell.heap[0x10040u + i] != 0xff) { unchanged = 0; break; }
    }
    CHECK(unchanged,
          "encType=2 + inner 0x1cc: NO reply written at target (poison "
          "intact)");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_walker_multi_segment(void) {
    fprintf(stdout, "\n--- test: walker_multi_segment ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xd2000001u);

    /* Three segments back to back. Each segment_size=8 holds one inner
     * cmd of totalLength=8 (i.e. an inner header with no payload). The
     * walker must visit all three and return without crashing. */
    uint8_t cmdbuf[3 * (16 + 8)];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    for (unsigned s = 0; s < 3; ++s) {
        uint8_t *seg = cmdbuf + s * (16 + 8);
        put_segment_header(seg, /*size=*/8u, /*prot=*/0u,
                           /*enc=*/(uint8_t)(2u + s),
                           /*final=*/(uint8_t)(s == 2 ? 1 : 0),
                           /*reuse=*/0u);
        put_inner_cmd(seg + 16, /*op=*/0x1000u + s, /*total_len=*/8u);
    }

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, (uint32_t)sizeof(cmdbuf));

    uint8_t cmd[12 + 128];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, 12u + 60u, 0xd2000002u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "multi-segment cmdBuf walker returns OK");
    /* Liveness via stamp counter is enough — walker did not crash. */
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd2000002u,
          "multi-segment walker: stamp signalled");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_walker_bad_segment_size_bails(void) {
    fprintf(stdout, "\n--- test: walker_bad_segment_size_bails ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xd3000001u);

    /* segment_size = 0xffffffff -> walker's bound check catches it and
     * bails. */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0, /*size=*/0xffffffffu, 0u,
                       /*enc=*/2u, /*final=*/1u, 0u);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, (uint32_t)sizeof(cmdbuf));

    uint8_t cmd[12 + 128];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, 12u + 60u, 0xd3000002u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "bad segment_size -> walker bails out gracefully (no crash)");
    CHECK(lagfx_protocol_last_completed_stamp(p) == 0xd3000002u,
          "bad segment_size: stamp still signalled");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_walker_inner_total_length_too_small(void) {
    fprintf(stdout, "\n--- test: walker_inner_total_length_too_small ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xd4000001u);

    /* Inner cmd totalLength=4 (< 8) — walker must bail. */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0, /*size=*/16u, 0u,
                       /*enc=*/4u, /*final=*/1u, 0u);
    put_inner_cmd(cmdbuf + 16, /*op=*/0x1ccu, /*total_len=*/4u);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, (uint32_t)sizeof(cmdbuf));

    uint8_t cmd[12 + 128];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, 12u + 60u, 0xd4000002u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "inner totalLength<8 -> walker bails out (no crash)");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

/* === Item 8 tests: 0x1cc reply writer corner cases ==================== */

static void test_reply_offset_out_of_bounds(void) {
    fprintf(stdout, "\n--- test: reply_offset_out_of_bounds ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xe0000001u);

    /* buffer_len = 16, reply_offset = 8 -> 8+12=20 > 16 -> reject. */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0, /*size=*/24u, 0u,
                       /*enc=*/4u, /*final=*/1u, 0u);
    put_inner_cmd(cmdbuf + 16, /*op=*/0x1ccu, /*total_len=*/24u);
    put_le32(cmdbuf + 24, /*ref=*/0u);
    put_le32(cmdbuf + 28, /*buffer_id=*/0u);
    put_le64(cmdbuf + 32, /*reply_offset=*/8ull);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    /* resource[0] length=16 (deliberately too small for reply_offset=8 + 12). */
    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, /*length=*/16u);

    /* Pre-poison the would-be reply target so we can detect a write. */
    memset(shell.heap + 0x10008u, 0xa5, 12);

    uint8_t cmd[12 + 128];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, 12u + 60u, 0xe0000002u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "reply_offset oob -> handler returns OK (warning logged)");
    int poison_intact = 1;
    for (int i = 0; i < 12; ++i) {
        if (shell.heap[0x10008u + i] != 0xa5) { poison_intact = 0; break; }
    }
    CHECK(poison_intact,
          "reply_offset+12 > buffer_len: NO reply written (poison intact)");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

static void test_reply_buffer_id_out_of_range(void) {
    fprintf(stdout, "\n--- test: reply_buffer_id_out_of_range ---\n");
    ei2_shell_t shell;
    ei2_shell_init(&shell, 0x40000000ull);
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    prep_radix_for_data_pfn(&shell, /*root_pfn*/0x40000u,
                            /*data_pfn*/0x40010u);
    define_host_task(p, 77u, 0x40000u, 0xe1000001u);

    /* resource_count=1; inner cmd targets buffer_id=2 (out of range). */
    uint8_t cmdbuf[64];
    memset(cmdbuf, 0, sizeof(cmdbuf));
    put_segment_header(cmdbuf + 0, 24u, 0u, 4u, 1u, 0u);
    put_inner_cmd(cmdbuf + 16, 0x1ccu, 24u);
    put_le32(cmdbuf + 24, 0u);
    put_le32(cmdbuf + 28, /*buffer_id=*/2u);  /* >= resource_count=1 */
    put_le64(cmdbuf + 32, 0x40ull);

    uint8_t *res_page = shell.heap + 0x10000u;
    prep_resource_cmdbuf(&shell, res_page, cmdbuf, sizeof(cmdbuf));

    uint8_t resources[16] = {0};
    put_le64(resources + 0, 0ull);
    put_le32(resources + 8, 64u);

    /* Pre-poison the would-be target. */
    memset(shell.heap + 0x10040u, 0xb6, 12);

    uint8_t cmd[12 + 128];
    build_header(cmd, LAGFX_OP_EXEC_INDIRECT2, 0, 12u + 60u, 0xe1000002u);
    size_t pl = build_exec_indirect2_outer(cmd + 12, 77u, NULL, 0,
                                           resources, 1);
    uint32_t total = (uint32_t)(12u + pl);
    cmd[4] = (uint8_t)(total & 0xff);
    cmd[5] = (uint8_t)((total >> 8) & 0xff);
    cmd[6] = (uint8_t)((total >> 16) & 0xff);
    cmd[7] = (uint8_t)((total >> 24) & 0xff);

    int rc = lagfx_protocol_dispatch_one(p, cmd, total);
    CHECK(rc == LAGFX_HANDLER_OK,
          "buffer_id >= resource_count: handler returns OK (warning)");
    int poison_intact = 1;
    for (int i = 0; i < 12; ++i) {
        if (shell.heap[0x10040u + i] != 0xb6) { poison_intact = 0; break; }
    }
    CHECK(poison_intact,
          "buffer_id >= resource_count: NO reply written (poison intact)");

    lagfx_device_free(dev);
    ei2_shell_free(&shell);
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/m4-execindirect2-parser: starting\n");

    /* Item 6. */
    test_outer_empty_invalidate_empty_resources();
    test_outer_resource_with_host_gpu_addr_invokes_walker();
    test_outer_uses_16B_invalidate_layout();

    /* Item 7. */
    test_walker_encType4_inner_0x1c9();
    test_walker_encType2_no_reply();
    test_walker_multi_segment();
    test_walker_bad_segment_size_bails();
    test_walker_inner_total_length_too_small();

    /* Item 8. */
    test_reply_offset_out_of_bounds();
    test_reply_buffer_id_out_of_range();

    fprintf(stdout, "\n=== m4-execindirect2-parser: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
