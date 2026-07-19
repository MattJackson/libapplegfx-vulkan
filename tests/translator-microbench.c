/*
 * libapplegfx-vulkan — translator micro-benchmark
 * tests/translator-microbench.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Purpose
 * -------
 * Host-side micro-benchmark of the translator hot path:
 *     MMIO decode -> command dispatch -> (optional) vulkan call build
 *     -> vkCmdDraw / submit
 *
 * No live guest is required. We synthesize N on-wire CmdDisplayTransaction3
 * commands targeting a 1080p BGRA8 "scanout" and feed them through
 * lagfx_protocol_dispatch_one() in a tight loop, then compute
 * commands-per-second and per-command nanoseconds.
 *
 * The executable is intentionally self-contained (one TU, no Vulkan
 * linkage beyond what libapplegfx_vulkan_dep already pulls in). It uses
 * the existing mock-shell plumbing pattern from tests/protocol-dispatch.c
 * but trimmed down to what the translator exercise actually needs.
 *
 * Rationale for why this is the "translator" budget
 * -------------------------------------------------
 * lagfx_protocol_dispatch_one is the pivot point the decoder calls per
 * command after header + min-payload validation. A real guest draw frame
 * at M8 (30fps @ 1080p interactive) ships on the order of ~300-1000
 * commands; this benchmark exaggerates to 10k so the per-command cost is
 * resolvable above clock jitter.
 *
 * Usage
 * -----
 *   $ ./build/tests/translator-microbench [N]
 *
 * N defaults to 10000 (task spec). The program always prints:
 *   - N
 *   - elapsed seconds
 *   - commands/sec
 *   - nanoseconds/command
 *
 * Exit code is 0 on success, 2 on device-new / display-new failure.
 *
 * M8 gate heuristic (printed at end):
 *   < 1 us/cmd  at 10k samples => translator is not the bottleneck.
 *   > 100 us/cmd at 10k samples => profile target.
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/display.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/opcodes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* === Mock shell (minimal) ================================ */

typedef struct {
    uint64_t raise_irq_count;
    uint64_t write_memory_count;
    uint64_t read_memory_count;
} bench_shell_t;

static lagfx_task_t *b_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void b_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool b_map(void *op, lagfx_task_t *t, uint64_t o,
                  const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool b_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool b_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    bench_shell_t *s = (bench_shell_t *)op;
    s->read_memory_count++;
    (void)gpa; (void)l; (void)d;
    return true;
}
static bool b_write(void *op, uint64_t gpa, uint64_t l, const void *src) {
    bench_shell_t *s = (bench_shell_t *)op;
    s->write_memory_count++;
    (void)gpa; (void)l; (void)src;
    return true;
}
static void b_raise_irq(void *op, uint32_t vec) {
    bench_shell_t *s = (bench_shell_t *)op;
    s->raise_irq_count++;
    (void)vec;
}

static lagfx_device_t *make_dev(bench_shell_t *shell) {
    lagfx_device_descriptor_t d;
    memset(&d, 0, sizeof(d));
    d.shell.opaque          = shell;
    d.shell.create_task     = b_create_task;
    d.shell.destroy_task    = b_destroy_task;
    d.shell.map_memory      = b_map;
    d.shell.unmap_memory    = b_unmap;
    d.shell.read_memory     = b_read;
    d.shell.write_memory    = b_write;
    d.shell.raise_interrupt = b_raise_irq;
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) {
        fprintf(stderr, "FATAL: device_new failed: %s\n",
                err ? err : "(no err)");
        free(err);
        exit(2);
    }
    return dev;
}

/* === Command-byte synthesis =============================== */

static void put_le32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)((v >> 8) & 0xffu);
    b[2] = (uint8_t)((v >> 16) & 0xffu);
    b[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static void put_lef32(uint8_t *b, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    put_le32(b, u);
}

static size_t build_header(uint8_t *out, uint16_t opcode,
                           uint32_t total_length, uint32_t stamp) {
    memset(out, 0, LAGFX_CMD_HEADER_BYTES);
    out[0] = (uint8_t)(opcode & 0xffu);
    out[1] = (uint8_t)((opcode >> 8) & 0xffu);
    /* arg_count_8b @ [2..3] = 0 */
    put_le32(out + 4, total_length);
    put_le32(out + 8, stamp);
    return LAGFX_CMD_HEADER_BYTES;
}

/* Build a CmdDisplayTransaction3 carrying one clear-color attachment
 * (legacy 12+32 shape per ops_display.c). Matches the shape the Phase
 * 2.A clear-color test drives. */
static size_t build_tx3_1080p(uint8_t *out, uint32_t display_id,
                              uint32_t transaction_id, uint32_t stamp) {
    const size_t total = 12 /* hdr */ + 12 /* disp,tx,count */ + 32;
    build_header(out, LAGFX_OP_DISPLAY_TRANSACTION3, (uint32_t)total, stamp);
    put_le32(out + 12, display_id);
    put_le32(out + 16, transaction_id);
    put_le32(out + 20, 1u);          /* attachmentCount */
    put_le32(out + 24, 0u);          /* attachmentIndex */
    put_le32(out + 28, 2u);          /* loadAction = Clear */
    put_le32(out + 32, 1u);          /* storeAction = Store */
    put_le32(out + 36, 0u);          /* flags */
    put_lef32(out + 40, 1.0f);       /* R */
    put_lef32(out + 44, 0.0f);       /* G */
    put_lef32(out + 48, 0.0f);       /* B */
    put_lef32(out + 52, 1.0f);       /* A */
    return total;
}

/* Build a CmdDisplaySwapMapping describing a 1920x1080 BGRA8 surface —
 * queued once at setup so subsequent tx3 commands target a real
 * display entry. */
static size_t build_swap_1080p(uint8_t *out, uint32_t display_id,
                               uint32_t mapping_id, uint32_t stamp) {
    const size_t total = 52;
    build_header(out, LAGFX_OP_DISPLAY_SWAP_MAPPING, (uint32_t)total, stamp);
    put_le32(out + 12, display_id);
    put_le32(out + 16, mapping_id);
    put_le64(out + 20, 0x1000000ull);          /* bufferVA */
    put_le64(out + 28, 1920ull * 1080ull * 4ull);
    put_le32(out + 36, 1920u);
    put_le32(out + 40, 1080u);
    put_le32(out + 44, 1920u * 4u);
    put_le32(out + 48, 0u);                    /* BGRA8 */
    return total;
}

/* === Timing ============================================== */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* === Main ================================================ */

int main(int argc, char **argv) {
    size_t N = 10000;
    bool attach_display = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-display") == 0) {
            attach_display = false;
            continue;
        }
        long v = strtol(argv[i], NULL, 10);
        if (v > 0) N = (size_t)v;
    }

    bench_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;

    /* Attach a display so the tx3 clear-color path triggers the full
     * vulkan submit (when LAGFX_HAVE_VULKAN is on). --no-display
     * skips this so we measure just the decoder (MMIO + dispatch +
     * header parse + state updates), without the per-frame vulkan
     * VkClear + readback that the display path triggers. */
    lagfx_display_t *display = NULL;
    if (attach_display) {
        static const lagfx_display_mode_t modes[] = {
            { 1920u, 1080u, 60u },
        };
        lagfx_display_descriptor_t disp_desc;
        memset(&disp_desc, 0, sizeof(disp_desc));
        disp_desc.name       = "microbench 1080p";
        disp_desc.modes      = modes;
        disp_desc.mode_count = 1u;
        char *derr = NULL;
        display = lagfx_display_new(dev, &disp_desc, 1u, 1u, &derr);
        if (!display) {
            fprintf(stderr, "WARN: display_new failed (%s); continuing "
                    "without render side-effect.\n",
                    derr ? derr : "(no err)");
            free(derr);
        }
    }

    /* One-time SwapMapping so display entry exists. */
    uint8_t swap[64];
    size_t swap_len = build_swap_1080p(swap, 1u, 1u, 0x5A000001u);
    (void)lagfx_protocol_dispatch_one(p, swap, swap_len);

    /* Pre-build a single tx3 command buffer; the stamp is patched
     * per-iteration so handlers that notice stamp progress still see
     * unique values without rebuilding the whole payload every time. */
    uint8_t tx[56];
    size_t tx_len = build_tx3_1080p(tx, 1u, 0u, 0x0u);

    /* Silence LAGFX_WARN/LAGFX_LOG fprintf-to-stderr calls during the
     * timed loop — the library is chatty about pending-stamp queue
     * fullness (the decoder's stamp queue drains on MMIO 0x1014 reads,
     * which a real guest would do but this synthetic driver does not),
     * and the fprintf formatting cost would otherwise dominate the
     * measurement. We re-open stderr after the loop for the report
     * block below. */
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    /* Warmup: run a short burst so instruction caches are hot, branch
     * predictors have trained on the dispatch switch, and any lazy
     * one-shot init inside the display/vulkan backend has completed.
     * We also drain the pending-stamp queue via an MMIO 0x1014 read
     * each iteration so the library's "queue full" warn path doesn't
     * fire — a real guest performs this read on every IRQ. */
    const size_t warmup = N / 10 > 256 ? N / 10 : 256;
    for (size_t i = 0; i < warmup; ++i) {
        put_le32(tx + 16, (uint32_t)i);           /* transactionID */
        put_le32(tx + 8,  (uint32_t)(0x77000000u + i));  /* stamp */
        (void)lagfx_protocol_dispatch_one(p, tx, tx_len);
        (void)lagfx_protocol_mmio_read(p, LAGFX_REG_STAMP_CELL_1);
    }

    /* Timed loop. */
    uint64_t t0_irq = shell.raise_irq_count;
    double t0 = now_sec();
    for (size_t i = 0; i < N; ++i) {
        put_le32(tx + 16, (uint32_t)(i + 1));
        put_le32(tx + 8,  (uint32_t)(0xB0000000u + i));
        (void)lagfx_protocol_dispatch_one(p, tx, tx_len);
        /* Guest-side stamp consumption: advances the pending-stamp
         * queue, mirroring what a real IRQ handler would do. Included
         * in the timing so "cost per command" reflects the full
         * guest<->host round trip the decoder participates in. */
        (void)lagfx_protocol_mmio_read(p, LAGFX_REG_STAMP_CELL_1);
    }
    double t1 = now_sec();

    /* Restore stderr for the report. */
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    uint64_t irq_delta = shell.raise_irq_count - t0_irq;

    double elapsed = t1 - t0;
    double cps = (elapsed > 0.0) ? ((double)N / elapsed) : 0.0;
    double ns_per_cmd = (N > 0) ? (elapsed * 1e9 / (double)N) : 0.0;

    /* Report. */
    printf("translator-microbench\n");
    printf("  mode          : %s\n",
           attach_display ? "decoder+vulkan" : "decoder-only");
    printf("  N             : %zu\n", N);
    printf("  elapsed       : %.6f s\n", elapsed);
    printf("  throughput    : %.0f cmds/sec\n", cps);
    printf("  per-cmd       : %.1f ns (%.3f us)\n",
           ns_per_cmd, ns_per_cmd / 1000.0);
    printf("  irqs raised   : %llu (expected %zu)\n",
           (unsigned long long)irq_delta, N);
    printf("  write_memory  : %llu\n",
           (unsigned long long)shell.write_memory_count);
    printf("  read_memory   : %llu\n",
           (unsigned long long)shell.read_memory_count);

    /* M8 gate heuristic for operator convenience. */
    if (ns_per_cmd < 1000.0) {
        printf("  verdict       : <1us/cmd -- M8-safe (translator not "
               "the bottleneck at 30fps 1080p)\n");
    } else if (ns_per_cmd > 100000.0) {
        printf("  verdict       : >100us/cmd -- profile target\n");
    } else {
        printf("  verdict       : between 1us and 100us/cmd -- "
               "feasible but worth watching\n");
    }

    lagfx_device_free(dev);
    return 0;
}
