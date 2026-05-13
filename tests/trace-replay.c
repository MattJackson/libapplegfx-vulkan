/*
 * libapplegfx-vulkan — MMIO trace replay harness
 * tests/trace-replay.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression harness for the protocol decoder's MMIO front door. We
 * capture guest-side MMIO traces from live boots (e.g.
 *   docker logs mos-docker-macos-1 | grep apple_gfx_
 * ) and replay them through lagfx_protocol_mmio_{read,write} against
 * a mock-device decoder state. The goal is to catch behavioural drift
 * in the decoder as we iterate: a trace that "works" today should keep
 * working tomorrow.
 *
 * Trace grammar is deliberately trivial (parsed in under 50 lines):
 *
 *    # comment — ignored, as is any blank line
 *    W <offset> <value>   # 32-bit MMIO write
 *    R <offset>           # 32-bit MMIO read (value discarded)
 *    R <offset> <value>   # read with expected value — fails if mismatch
 *
 * Offsets and values accept 0x-prefixed hex or plain decimal.
 *
 * Assertions:
 *   - No panics/crashes during replay.
 *   - Every expected-value read matches.
 *   - A post-replay invariant check: after processing the init-phase
 *     trace shipped alongside this file, the decoder's ring_base_gpa
 *     must equal (ring_base_pfn << 12) + ring_start_offset, matching
 *     the computation in src/protocol/protocol.c for W 0x1030 + W 0x1010.
 *     For the shipped m2-m3-init-2026-04-21.trace this is
 *     0x23eb6000 + 0x1000 = 0x23eb7000.
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/protocol.h"
#include "../src/protocol/state.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
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

/* === Mock shell callbacks ===================================
 *
 * Minimal — the init-phase trace doesn't exercise ring drain (the
 * trace covers the setup writes that precede the first doorbell),
 * but we still supply callbacks so any stray DMA path taken by the
 * decoder has somewhere to land. */
typedef struct {
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
} mock_shell_t;

static lagfx_task_t *mock_create_task(void *op, uint64_t sz, void **out) {
    (void)op; (void)sz;
    if (out) *out = (void *)0xbeef0000u;
    return (lagfx_task_t *)0x1u;
}
static void mock_destroy_task(void *op, lagfx_task_t *t) { (void)op; (void)t; }
static bool mock_map(void *op, lagfx_task_t *t, uint64_t o,
                     const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)op; (void)t; (void)o; (void)r; (void)c; (void)ro;
    return true;
}
static bool mock_unmap(void *op, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)op; (void)t; (void)o; (void)l;
    return true;
}
static bool mock_read(void *op, uint64_t gpa, uint64_t l, void *d) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->read_memory_count++;
    /* Zero-fill — decoder reads during the init trace hit the shared
     * page, which is unpopulated in this fixture. */
    if (d) memset(d, 0, (size_t)l);
    (void)gpa;
    return true;
}
static bool mock_write(void *op, uint64_t gpa, uint64_t l, const void *s) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->write_memory_count++;
    (void)gpa; (void)l; (void)s;
    return true;
}
static void mock_raise_irq(void *op, uint32_t vec) {
    mock_shell_t *m = (mock_shell_t *)op;
    m->raise_irq_count++;
    (void)vec;
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
    d.shell.write_memory    = mock_write;
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

/* === Trace parser =========================================== */

/* strtoull with 0/0x prefix autodetect, tolerant of trailing ws. */
static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0) return false;
    if (end == s) return false;
    /* Any trailing chars must be whitespace. */
    while (*end) {
        if (!isspace((unsigned char)*end)) return false;
        end++;
    }
    *out = (uint64_t)v;
    return true;
}

/* Stats the harness reports at the end. */
typedef struct {
    unsigned writes;
    unsigned reads;
    unsigned comments;
    unsigned blanks;
    unsigned read_expectations_checked;
    unsigned read_expectations_failed;
} trace_stats_t;

/* Replay one line. Returns true on successful parse+dispatch, false on
 * syntax error. Missing expected-value mismatches are counted but do
 * not abort — we want to surface them all. */
static bool replay_line(lagfx_protocol_t *p,
                        const char *line,
                        unsigned lineno,
                        trace_stats_t *st) {
    /* Strip leading ws. */
    while (*line && isspace((unsigned char)*line)) line++;
    if (!*line) { st->blanks++; return true; }
    if (*line == '#') { st->comments++; return true; }

    char op = (char)toupper((unsigned char)*line);
    if (op != 'W' && op != 'R') {
        fprintf(stderr, "trace:%u: bad opcode '%c' (expected W or R)\n",
                lineno, *line);
        return false;
    }
    line++;
    while (*line && isspace((unsigned char)*line)) line++;

    /* Tokenise remainder. */
    char buf[256];
    size_t n = strlen(line);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = 0;
    /* Strip trailing newline / ws. */
    while (n && isspace((unsigned char)buf[n-1])) buf[--n] = 0;
    /* Strip inline comment. */
    char *hash = strchr(buf, '#');
    if (hash) {
        *hash = 0;
        size_t m = strlen(buf);
        while (m && isspace((unsigned char)buf[m-1])) buf[--m] = 0;
    }

    char *saveptr = NULL;
    char *tok_off = strtok_r(buf, " \t", &saveptr);
    char *tok_val = strtok_r(NULL, " \t", &saveptr);
    char *tok_extra = strtok_r(NULL, " \t", &saveptr);

    if (!tok_off) {
        fprintf(stderr, "trace:%u: missing offset\n", lineno);
        return false;
    }
    if (tok_extra) {
        fprintf(stderr, "trace:%u: unexpected extra token '%s'\n",
                lineno, tok_extra);
        return false;
    }

    uint64_t offset = 0;
    if (!parse_u64(tok_off, &offset)) {
        fprintf(stderr, "trace:%u: bad offset '%s'\n", lineno, tok_off);
        return false;
    }

    if (op == 'W') {
        if (!tok_val) {
            fprintf(stderr, "trace:%u: W missing value\n", lineno);
            return false;
        }
        uint64_t value = 0;
        if (!parse_u64(tok_val, &value)) {
            fprintf(stderr, "trace:%u: bad value '%s'\n", lineno, tok_val);
            return false;
        }
        lagfx_protocol_mmio_write(p, offset, (uint32_t)value);
        st->writes++;
        return true;
    }

    /* R — value token, if present, is an expected value. */
    uint32_t got = lagfx_protocol_mmio_read(p, offset);
    st->reads++;
    if (tok_val) {
        uint64_t expected = 0;
        if (!parse_u64(tok_val, &expected)) {
            fprintf(stderr, "trace:%u: bad expected value '%s'\n",
                    lineno, tok_val);
            return false;
        }
        st->read_expectations_checked++;
        if ((uint32_t)expected != got) {
            fprintf(stderr,
                    "trace:%u: R 0x%llx expected 0x%08x got 0x%08x\n",
                    lineno, (unsigned long long)offset,
                    (uint32_t)expected, got);
            st->read_expectations_failed++;
        }
    }
    return true;
}

static bool replay_file(lagfx_protocol_t *p, const char *path,
                        trace_stats_t *st) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return false;
    }
    char line[512];
    unsigned lineno = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (!replay_line(p, line, lineno, st)) {
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

/* === Test entry ============================================= */

int main(int argc, char **argv) {
#ifndef __linux__
    fprintf(stderr, "trace replay requires Linux (Vulkan lavapipe); skipping on %s\n", 
            sizeof(__APPLE__) ? "macOS" : "unknown");
    return 77;
#endif

    const char *trace_path = NULL;
    if (argc >= 2) {
        trace_path = argv[1];
    } else {
        /* Default — meson runs tests from the build directory, but
         * LAGFX_TRACE_PATH is passed through as a compile-time macro
         * so the harness can locate its companion fixture. */
#ifdef LAGFX_TRACE_REPLAY_DEFAULT
        trace_path = LAGFX_TRACE_REPLAY_DEFAULT;
#else
        trace_path = "traces/m2-m3-init-2026-04-21.trace";
#endif
    }

    fprintf(stdout, "trace-replay: replaying %s\n", trace_path);

    mock_shell_t shell = {0};
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    CHECK(p != NULL, "device has decoder attached");

    trace_stats_t st = {0};
    bool parsed_clean = replay_file(p, trace_path, &st);
    CHECK(parsed_clean, "trace parsed without syntax errors");
    CHECK(st.read_expectations_failed == 0,
          "all read expectations matched (0 mismatches)");

    fprintf(stdout,
            "trace-replay: %u writes, %u reads (%u expected), "
            "%u comments, %u blanks\n",
            st.writes, st.reads, st.read_expectations_checked,
            st.comments, st.blanks);

    /* Decoder-state invariants post-replay. These are specific to the
     * init-phase trace the task shipped alongside this harness; when a
     * user supplies a different trace via argv[1], we downgrade the
     * strict assertions to informational prints so the harness remains
     * useful as a generic regression driver. */
    bool is_shipped_init_trace =
        (argc < 2) ||
        (strstr(trace_path, "m2-m3-init") != NULL);

    if (is_shipped_init_trace) {
        /* Writes we expect to have observed:
         *   0x1030 0x23eb6    -> ring_base_pfn
         *   0x1010 0x1000     -> ring_start_offset
         *   0x1004 0x10000    -> ring_size
         *   0x1000 0x1        -> ring_armed
         * And the computed ring_base_gpa = (0x23eb6 << 12) + 0x1000
         *   = 0x23eb6000 + 0x1000 = 0x23eb7000.
         */
        CHECK(p->ring_base_pfn == 0x23eb6u,
              "ring_base_pfn == 0x23eb6 (from W 0x1030)");
        CHECK(p->ring_start_offset == 0x1000u,
              "ring_start_offset == 0x1000 (from W 0x1010)");
        CHECK(p->ring_size == 0x10000u,
              "ring_size == 0x10000 (from W 0x1004)");
        CHECK(p->ring_base_gpa == 0x23eb7000ull,
              "ring_base_gpa == 0x23eb7000 (pfn<<12 + start_offset)");
        CHECK(p->ring_armed,
              "ring_armed after W 0x1000 0x1 kick");
    } else {
        fprintf(stdout,
                "trace-replay: non-shipped trace, post-state:"
                " ring_base_pfn=0x%x ring_start_offset=0x%x"
                " ring_size=0x%x ring_base_gpa=0x%llx armed=%d\n",
                p->ring_base_pfn, p->ring_start_offset,
                p->ring_size,
                (unsigned long long)p->ring_base_gpa,
                (int)p->ring_armed);
    }

    lagfx_device_free(dev);

    fprintf(stdout, "\ntrace-replay: %d passed, %d failed\n",
            g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
