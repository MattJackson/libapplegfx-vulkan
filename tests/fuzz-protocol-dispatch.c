/*
 * libapplegfx-vulkan — libFuzzer harness for lagfx_protocol_dispatch_one
 * tests/fuzz-protocol-dispatch.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Feeds libFuzzer-supplied bytes directly into the protocol dispatcher.
 * Catches the next cmd_len=0xFFFFFFFF-class bug at PR time, not in prod.
 *
 * Build:
 *   meson setup buildfuzz -Dfuzz=enabled -Db_sanitize=address,undefined
 *   meson compile -C buildfuzz fuzz-protocol-dispatch
 *   ./buildfuzz/tests/fuzz-protocol-dispatch -max_total_time=300 \
 *       paravirt-re/traces  (seed corpus)
 *
 * Each input represents the bytes of one ring-buffer command. The
 * dispatcher walks the 12-byte header, allocates the payload, runs
 * the matching opcode handler, and signals stamp completion. ASAN
 * + UBSAN catch any out-of-bounds reads, integer overflows, or
 * use-after-frees the dispatcher might trip.
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Mock shell — accept everything, return zeros, never block. The fuzz
 * target is the dispatcher's parsing + handler invocation, NOT the
 * shell's behavior. */
static lagfx_task_t *fuzz_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void fuzz_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool fuzz_map(void *op, lagfx_task_t *t, uint64_t o,
                     const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool fuzz_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l; return true;
}
static bool fuzz_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    (void)op; (void)gpa;
    if (d) memset(d, 0, (size_t)l);
    return true;
}
static bool fuzz_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    (void)op; (void)gpa; (void)l; (void)s; return true;
}
static void fuzz_irq(void *op, uint32_t vec) { (void)op; (void)vec; }

static lagfx_device_t *fuzz_dev = NULL;

static void fuzz_init(void) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.create_task     = fuzz_create_task;
    d.shell.destroy_task    = fuzz_destroy_task;
    d.shell.map_memory      = fuzz_map;
    d.shell.unmap_memory    = fuzz_unmap;
    d.shell.read_memory     = fuzz_read;
    d.shell.write_memory    = fuzz_write;
    d.shell.raise_interrupt = fuzz_irq;
    char *err = NULL;
    fuzz_dev = lagfx_device_new(&d, &err);
    free(err);
    if (!fuzz_dev) abort();
}

/* libFuzzer entry point. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!fuzz_dev) fuzz_init();
    lagfx_protocol_t *p = (lagfx_protocol_t *)fuzz_dev->protocol_state;
    /* dispatch_one returns negative rc on size/parse error; we ignore
     * the rc — the goal is to crash the harness on any UAF / OOB /
     * UB the dispatcher trips, not to assert opcode-specific behavior. */
    (void)lagfx_protocol_dispatch_one(p, data, size);
    return 0;
}
