/*
 * libapplegfx-vulkan — internal logging helper
 * src/common/log.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three levels:
 *   LAGFX_ERR   — always emitted
 *   LAGFX_WARN  — always emitted
 *   LAGFX_LOG   — per-task/per-display-event volume (info-level).
 *                 Gated by LAGFX_LOG_LEVEL >= info (default).
 *   LAGFX_TRACE — per-cmd/per-translate/per-mmio volume.
 *                 Gated by LAGFX_LOG_LEVEL >= trace (off by default).
 *
 * Selection (in order of precedence):
 *   LAGFX_LOG_LEVEL=warn|info|trace   — explicit level
 *   LAGFX_LOG=1                       — back-compat: equivalent to info
 *   LAGFX_LOG=0  / unset              — warn-only (prod default)
 *
 * Keep this header dependency-free so every subsystem can include it freely.
 */

#ifndef LIBAPPLEGFX_COMMON_LOG_H
#define LIBAPPLEGFX_COMMON_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAGFX_LOG_LVL_WARN  0
#define LAGFX_LOG_LVL_INFO  1
#define LAGFX_LOG_LVL_TRACE 2

static inline int lagfx_log_level(void) {
    static int cached = -1;
    if (cached == -1) {
        const char *lvl = getenv("LAGFX_LOG_LEVEL");
        if (lvl && lvl[0]) {
            if (strcmp(lvl, "trace") == 0) cached = LAGFX_LOG_LVL_TRACE;
            else if (strcmp(lvl, "info") == 0) cached = LAGFX_LOG_LVL_INFO;
            else cached = LAGFX_LOG_LVL_WARN;
        } else {
            const char *v = getenv("LAGFX_LOG");
            cached = (v && v[0] && strcmp(v, "0") != 0)
                ? LAGFX_LOG_LVL_INFO : LAGFX_LOG_LVL_WARN;
        }
    }
    return cached;
}

static inline const char *lagfx_level_name(void) {
    switch (lagfx_log_level()) {
        case LAGFX_LOG_LVL_TRACE: return "trace";
        case LAGFX_LOG_LVL_INFO:  return "info";
        default:                  return "warn";
    }
}

/* Implementations live in device.c; they route through lagfx_log_internal
 * which writes to /tmp/lagfx.log (with stderr fallback) and flushes after
 * every line. Macros call these so logs survive when QEMU runs as a
 * non-PID-1 process inside docker (where its stderr would otherwise be
 * dropped). */
extern void lagfx_log_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
extern void lagfx_warn_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
extern void lagfx_err_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
extern void lagfx_trace_impl(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Opcode trace support (Task O2) — initialized at first log call from env var.
 * g_trace_opcodes contains up to 8 opcodes; entries beyond are silently ignored.
 * Use lagfx_is_opcode_traced(opcode) to check if an opcode should be traced. */
extern int g_trace_opcodes[8];
extern int g_trace_opcodes_count;

static inline bool lagfx_is_opcode_traced(uint32_t opcode) {
    for (int i = 0; i < g_trace_opcodes_count && i < 8; ++i) {
        if (g_trace_opcodes[i] == (int)opcode) {
            return true;
        }
    }
    return false;
}

/* New macro: logs at INFO level if opcode is in trace list OR global level >= trace.
 * Otherwise no-op. Ready for adoption by handlers/ during Stage 80 debugging. */
#define LAGFX_OPCODE_TRACE(opcode, fmt, ...) \
    do { \
        if (lagfx_log_level() >= LAGFX_LOG_LVL_TRACE || lagfx_is_opcode_traced(opcode)) { \
            lagfx_log_impl(fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LAGFX_LOG(fmt, ...) \
    do { \
        if (lagfx_log_level() >= LAGFX_LOG_LVL_INFO) { \
            lagfx_log_impl(fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LAGFX_TRACE(fmt, ...) \
    do { \
        if (lagfx_log_level() >= LAGFX_LOG_LVL_TRACE) { \
            lagfx_trace_impl(fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define LAGFX_WARN(fmt, ...) lagfx_warn_impl(fmt, ##__VA_ARGS__)
#define LAGFX_ERR(fmt, ...)  lagfx_err_impl(fmt, ##__VA_ARGS__)

#endif /* LIBAPPLEGFX_COMMON_LOG_H */
