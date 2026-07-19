/*
 * libapplegfx-vulkan — perf timing helper (M3 Stage 95)
 * src/common/perf.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Monotonic nanosecond clock for the env-gated (LAGFX_PERF) frame-time
 * instrumentation that feeds the lavapipe-vs-GPU-backend decision. Header-only.
 */
#ifndef LAGFX_COMMON_PERF_H
#define LAGFX_COMMON_PERF_H

#include <stdint.h>
#include <time.h>

static inline uint64_t lagfx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif /* LAGFX_COMMON_PERF_H */
