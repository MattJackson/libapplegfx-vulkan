/*
 * libapplegfx-vulkan — little-endian readers
 * src/common/le.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Per-byte LE readers for guest-protocol payloads. The macOS guest
 * is x86-64 (little-endian), but the readers are arch-neutral
 * byte-shift form so they compile cleanly on any host and don't
 * depend on alignment rules.
 *
 * Eleven-plus TUs previously each redeclared their own static-inline
 * copies under names like read_le16/32, lagfx_le32/64, ch0_read_le32,
 * and lagfx_le{32,64}_local. This header is the single source of
 * truth — keep it dependency-free (only <stdint.h>) so every
 * subsystem can include it freely.
 */

#ifndef LIBAPPLEGFX_COMMON_LE_H
#define LIBAPPLEGFX_COMMON_LE_H

#include <stdint.h>

static inline uint16_t lagfx_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

/* Read a float stored as little-endian u32. Used by ops_display.c
 * for RGBA clear-color payloads. */
static inline float lagfx_lef32(const uint8_t *b) {
    union { uint32_t u; float f; } conv;
    conv.u = lagfx_le32(b);
    return conv.f;
}

#endif /* LIBAPPLEGFX_COMMON_LE_H */
