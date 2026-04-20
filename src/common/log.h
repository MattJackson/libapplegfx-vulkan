/*
 * libapplegfx-vulkan — internal logging helper
 * src/common/log.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal stderr logger. Envvar LAGFX_LOG=1 to enable verbose
 * traces; errors/warnings are always printed. Keep this header
 * dependency-free so every subsystem can include it freely.
 */

#ifndef LIBAPPLEGFX_COMMON_LOG_H
#define LIBAPPLEGFX_COMMON_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return non-zero if LAGFX_LOG env var is set and not "0"/"". */
static inline int lagfx_log_enabled(void) {
    static int cached = -1;
    if (cached == -1) {
        const char *v = getenv("LAGFX_LOG");
        cached = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

#define LAGFX_LOG(fmt, ...) \
    do { \
        if (lagfx_log_enabled()) { \
            fprintf(stderr, "[lagfx] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define LAGFX_WARN(fmt, ...) \
    fprintf(stderr, "[lagfx warn] " fmt "\n", ##__VA_ARGS__)

#define LAGFX_ERR(fmt, ...) \
    fprintf(stderr, "[lagfx error] " fmt "\n", ##__VA_ARGS__)

#endif /* LIBAPPLEGFX_COMMON_LOG_H */
