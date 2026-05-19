/*
 * libapplegfx-vulkan — LLVM Bitstream reader primitives
 * src/air/bitstream.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the bit-level cursor primitives in bitstream.h.
 *
 * LLVM bitstream is little-endian bit order WITHIN each byte, and the
 * stream as a whole reads bytes in order. So to read bit position B
 * we compute byte = B>>3, bit_in_byte = B&7, and the value at that
 * position is bit `bit_in_byte` of `buf[byte]` (low bit first).
 *
 * For a multi-bit read of width N starting at bit B, we read enough
 * full bytes to span [B, B+N), shift them into a u64, and mask the
 * relevant N bits.
 */

#include "bitstream.h"

#include <string.h>

uint32_t
lagfx_bs_read_bits(lagfx_bitstream_t *bs, uint32_t n, bool *err) {
    if (n == 0u || n > 32u) {
        *err = true;
        return 0u;
    }
    if (bs->bit_pos + (size_t)n > bs->buf_len * 8u) {
        *err = true;
        return 0u;
    }

    /* Load up to 5 bytes covering bits [bit_pos, bit_pos+n).
     * Worst case: starting bit_in_byte=7, N=32 → spans 5 bytes. */
    size_t   byte_off = bs->bit_pos >> 3;
    uint32_t bit_in   = (uint32_t)(bs->bit_pos & 7u);

    uint64_t acc = 0u;
    uint32_t shift = 0u;
    /* Read enough bytes to cover (bit_in + n) bits. */
    uint32_t total_bits_needed = bit_in + n;
    uint32_t bytes_to_read = (total_bits_needed + 7u) >> 3;
    if (bytes_to_read > 5u) bytes_to_read = 5u;  /* clamp; n<=32+7 bits=39bits max */

    for (uint32_t i = 0u; i < bytes_to_read; i++) {
        if (byte_off + i >= bs->buf_len) {
            *err = true;
            return 0u;
        }
        acc |= ((uint64_t)bs->buf[byte_off + i]) << shift;
        shift += 8u;
    }

    /* Shift right to drop bit_in low bits, then mask to N bits. */
    uint32_t val = (uint32_t)((acc >> bit_in) & (n == 32u ? 0xFFFFFFFFu : ((1u << n) - 1u)));
    bs->bit_pos += n;
    return val;
}

uint64_t
lagfx_bs_read_bits_64(lagfx_bitstream_t *bs, uint32_t n, bool *err) {
    if (n == 0u || n > 64u) {
        *err = true;
        return 0u;
    }
    if (n <= 32u) {
        return (uint64_t)lagfx_bs_read_bits(bs, n, err);
    }
    /* Read low 32 bits then high (n-32) bits. */
    uint64_t lo = (uint64_t)lagfx_bs_read_bits(bs, 32u, err);
    if (*err) return 0u;
    uint64_t hi = (uint64_t)lagfx_bs_read_bits(bs, n - 32u, err);
    if (*err) return 0u;
    return lo | (hi << 32);
}

uint32_t
lagfx_bs_read_vbr(lagfx_bitstream_t *bs, uint32_t n, bool *err) {
    /* VBR-n: read chunks of n bits; high bit of each chunk = "more bits follow".
     * Value is the concatenation of the LOW (n-1) bits of each chunk in
     * little-endian (first chunk = low bits). */
    if (n < 2u || n > 32u) {
        *err = true;
        return 0u;
    }
    uint32_t val = 0u;
    uint32_t shift = 0u;
    uint32_t more_mask = 1u << (n - 1u);
    uint32_t data_mask = more_mask - 1u;  /* low (n-1) bits */

    for (;;) {
        uint32_t chunk = lagfx_bs_read_bits(bs, n, err);
        if (*err) return 0u;
        val |= (chunk & data_mask) << shift;
        if ((chunk & more_mask) == 0u) {
            break;
        }
        shift += (n - 1u);
        /* Bail if shifting beyond 32 bits — caller should use vbr_64. */
        if (shift >= 32u) {
            *err = true;
            return 0u;
        }
    }
    return val;
}

uint64_t
lagfx_bs_read_vbr_64(lagfx_bitstream_t *bs, uint32_t n, bool *err) {
    if (n < 2u || n > 32u) {
        *err = true;
        return 0u;
    }
    uint64_t val = 0u;
    uint32_t shift = 0u;
    uint32_t more_mask = 1u << (n - 1u);
    uint32_t data_mask = more_mask - 1u;

    for (;;) {
        uint32_t chunk = lagfx_bs_read_bits(bs, n, err);
        if (*err) return 0u;
        val |= ((uint64_t)(chunk & data_mask)) << shift;
        if ((chunk & more_mask) == 0u) {
            break;
        }
        shift += (n - 1u);
        if (shift >= 64u) {
            *err = true;
            return 0u;
        }
    }
    return val;
}
