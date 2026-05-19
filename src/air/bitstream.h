/*
 * libapplegfx-vulkan — LLVM Bitstream reader primitives
 * src/air/bitstream.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The LLVM Bitstream format is a self-describing bit-packed container.
 * https://llvm.org/docs/BitCodeFormat.html#bitstream-format
 *
 * Layout:
 *   [Magic][Blocks...]
 *   Each block: [enter_subblock][block_id][new_abbrev_len][block_size][records...][end_block]
 *   Each record: [abbrev_code][operands...]
 *
 * Abbrev codes:
 *   0 = END_BLOCK
 *   1 = ENTER_SUBBLOCK
 *   2 = DEFINE_ABBREV
 *   3 = UNABBREV_RECORD
 *   4+ = application-defined abbreviations
 *
 * Apple's customization is in (4+) abbreviations inside FUNCTION_BLOCK.
 * Phase 1 only sees abbreviations in module-level blocks, which match
 * upstream LLVM.
 */

#ifndef LIBAPPLEGFX_AIR_BITSTREAM_H
#define LIBAPPLEGFX_AIR_BITSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cursor over a bit-packed byte buffer. */
typedef struct {
    const uint8_t *buf;        /* underlying buffer (caller-owned) */
    size_t         buf_len;    /* length in BYTES */
    size_t         bit_pos;    /* current read position in BITS (always advancing) */
} lagfx_bitstream_t;

/* Initialize cursor at bit position 0. */
static inline void lagfx_bs_init(lagfx_bitstream_t *bs,
                                  const uint8_t *buf, size_t buf_len) {
    bs->buf = buf;
    bs->buf_len = buf_len;
    bs->bit_pos = 0u;
}

/* End-of-stream check (true if no bits remain). */
static inline bool lagfx_bs_at_end(const lagfx_bitstream_t *bs) {
    return bs->bit_pos >= bs->buf_len * 8u;
}

/* Current bit position (for diagnostics + body_offset stashing). */
static inline size_t lagfx_bs_pos(const lagfx_bitstream_t *bs) {
    return bs->bit_pos;
}

/* Seek cursor to absolute bit position. Returns false if out of range. */
static inline bool lagfx_bs_seek(lagfx_bitstream_t *bs, size_t bit_pos) {
    if (bit_pos > bs->buf_len * 8u) return false;
    bs->bit_pos = bit_pos;
    return true;
}

/* Align cursor up to the next 32-bit boundary. Used at block-boundary
 * transitions per LLVM bitstream spec. */
static inline void lagfx_bs_align_32(lagfx_bitstream_t *bs) {
    bs->bit_pos = (bs->bit_pos + 31u) & ~(size_t)31u;
}

/* Read `n` bits (0 < n <= 32) as an unsigned integer.
 * On error (insufficient bits), returns 0 and sets *err = true.
 * On success, *err is left unchanged (caller initializes to false). */
uint32_t lagfx_bs_read_bits(lagfx_bitstream_t *bs, uint32_t n, bool *err);

/* Read a 64-bit value via two 32-bit reads. */
uint64_t lagfx_bs_read_bits_64(lagfx_bitstream_t *bs, uint32_t n, bool *err);

/* Read a Variable Bit-Rate (VBR) integer with `n`-bit chunks.
 * VBR encoding: read `n` bits; if high bit (bit n-1) is set, the lower
 * (n-1) bits are part of the value and we continue reading more chunks;
 * otherwise all `n` bits are the final value. Per LLVM bitstream spec. */
uint32_t lagfx_bs_read_vbr(lagfx_bitstream_t *bs, uint32_t n, bool *err);

/* 64-bit VBR (for large values). */
uint64_t lagfx_bs_read_vbr_64(lagfx_bitstream_t *bs, uint32_t n, bool *err);

/* === Convenience macros ========================================== */

/* Standard LLVM bitstream abbrev IDs (always 2 bits for the standard set,
 * but width grows with DEFINE_ABBREV use). Cursor's CURRENT abbrev width
 * is tracked by the caller (changes with ENTER_SUBBLOCK / new_abbrev_len). */
#define LAGFX_BS_ABBREV_END_BLOCK        0u
#define LAGFX_BS_ABBREV_ENTER_SUBBLOCK   1u
#define LAGFX_BS_ABBREV_DEFINE_ABBREV    2u
#define LAGFX_BS_ABBREV_UNABBREV_RECORD  3u

/* Maximum allowed abbrev width — 8 bits = 256 abbreviations per block.
 * Real bitcode uses 3..6 bits typically. */
#define LAGFX_BS_MAX_ABBREV_WIDTH        8u

#endif /* LIBAPPLEGFX_AIR_BITSTREAM_H */
