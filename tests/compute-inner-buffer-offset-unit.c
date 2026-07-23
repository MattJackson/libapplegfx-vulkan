/*
 * libapplegfx-vulkan — PGCmdSetBufferOffset wire-format unit tests
 * tests/compute-inner-buffer-offset-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Red-on-revert guard for the 0x6f/0x7e SetBufferOffset field order:
 * the wire is [index:u32@0][offset:u64@4] (Apple's
 * decodeSetVertexBufferOffsetWithIterator / compute decodeSetBufferOffset
 * disasm: u32@0 is validated against the buffer-array count and passed as
 * the atIndex: arg; u64@+4 is the offset arg). The pre-fix swapped read
 * ([offset:u64@0][index:u32@8]) turned live commands like
 * {index=1, offset=0x10200} into offset=0x1020000000001 on slot 0 —
 * every subsequent placement read of that slot failed → zero buffer →
 * the Xgc backdrop draws rendered degenerate geometry (the per-pass
 * content-flow blocker, 2026-07-23).
 */

#include "../src/protocol/state.h"
#include "../src/handlers/compute/compute_inner_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; }       \
    else         { fprintf(stdout, "PASS: %s\n", msg); }                 \
} while (0)

static void put_le32(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
}
static void put_le64(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
}

int main(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    if (!p) return 1;
    uint32_t task_id = 3u;
    p->tasks[task_id].live = true;

    /* Pre-bind slot 1 so the offset update lands on a bound slot. */
    p->tasks[task_id].bindings.vertex_buffers[1].ref = 0x24u;
    p->tasks[task_id].bindings.vertex_buffers[1].valid = true;
    p->tasks[task_id].bindings.fragment_buffers[1].ref = 0x24u;
    p->tasks[task_id].bindings.fragment_buffers[1].valid = true;

    /* Live-wire example from the 2026-07-23 boot: index=1, offset=0x10200. */
    uint8_t body[12];
    put_le32(body + 0, 1u);        /* index @0 */
    put_le64(body + 4, 0x10200u);  /* offset @4 */

    int rc = lagfx_compute_inner_dispatch(p, 0u, task_id, 0x7e, body, sizeof(body));
    CHECK(rc == 0, "0x7e SetVertexBufferOffset dispatch succeeds");
    CHECK(p->tasks[task_id].bindings.vertex_buffers[1].offset == 0x10200u,
          "0x7e updates vertex_buffers[index=1].offset from u64@+4");
    CHECK(p->tasks[task_id].bindings.vertex_buffers[0].offset == 0u,
          "0x7e does NOT touch slot 0 (old swapped read wrote garbage there)");
    CHECK(p->tasks[task_id].bindings.vertex_buffers[1].ref == 0x24u
          && p->tasks[task_id].bindings.vertex_buffers[1].valid,
          "0x7e leaves ref/valid alone");

    rc = lagfx_compute_inner_dispatch(p, 0u, task_id, 0x6f, body, sizeof(body));
    CHECK(rc == 0, "0x6f SetFragmentBufferOffset dispatch succeeds");
    CHECK(p->tasks[task_id].bindings.fragment_buffers[1].offset == 0x10200u,
          "0x6f updates fragment_buffers[index=1].offset from u64@+4");
    CHECK(p->tasks[task_id].bindings.fragment_buffers[0].offset == 0u,
          "0x6f does NOT touch slot 0");

    /* The pre-fix parse read u64@0 as offset: for this wire that is
     * 0x0000010200000001 — assert the fixed parse can no longer produce it. */
    CHECK(p->tasks[task_id].bindings.vertex_buffers[1].offset != 0x1020000000001ull,
          "swapped-field regression value absent");

    /* Out-of-range index is rejected without state change. */
    put_le32(body + 0, 0xffffu);
    rc = lagfx_compute_inner_dispatch(p, 0u, task_id, 0x7e, body, sizeof(body));
    CHECK(rc != 0, "0x7e rejects out-of-range index");

    free(p);
    printf("%s\n", g_fail ? "FAILURES" : "ALL PASS");
    return g_fail ? 1 : 0;
}
