/*
 * libapplegfx-vulkan — LLVM Bitstream block-level traversal
 * src/air/block_reader.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "block_reader.h"

#include <stddef.h>

/* === Block entry / skip ========================================== */

/* Read an ENTER_SUBBLOCK payload at the current cursor (the abbrev
 * code byte must already have been consumed by the caller).
 *
 * Payload:
 *   blockid:vbr8
 *   newAbbrevWidth:vbr4 (minimum 1, typically 3..6)
 *   <align32>
 *   blockSize:32 (32-bit-word count)
 *
 * On success populates `out` with new abbrev width + block_id + end
 * position. Cursor is left at the first record/sub-block inside the
 * new block. */
static bool read_enter_subblock_payload(lagfx_bitstream_t *bs,
                                         lagfx_block_t     *out) {
    bool err = false;
    uint32_t blockid = lagfx_bs_read_vbr(bs, 8, &err);
    if (err) return false;
    uint32_t new_abbrev_width = lagfx_bs_read_vbr(bs, 4, &err);
    if (err) return false;
    if (new_abbrev_width == 0u || new_abbrev_width > LAGFX_BS_MAX_ABBREV_WIDTH) {
        return false;
    }
    lagfx_bs_align_32(bs);
    uint32_t block_size_words = lagfx_bs_read_bits(bs, 32, &err);
    if (err) return false;

    out->bs = bs;
    out->abbrev_width = new_abbrev_width;
    out->block_id = blockid;
    /* block_size is the number of 32-bit words BETWEEN the size field
     * and the END_BLOCK (exclusive of END_BLOCK itself per spec).
     * The cursor is currently at the start of the block contents. */
    out->end_pos = lagfx_bs_pos(bs) + (size_t)block_size_words * 32u;
    return true;
}

bool
lagfx_block_enter(lagfx_bitstream_t *bs,
                  uint32_t           parent_abbrev_width,
                  lagfx_block_t     *out_block) {
    bool err = false;
    uint32_t code = lagfx_bs_read_bits(bs, parent_abbrev_width, &err);
    if (err) return false;
    if (code != LAGFX_ABBREV_ENTER_SUBBLOCK) return false;
    return read_enter_subblock_payload(bs, out_block);
}

bool
lagfx_block_skip(lagfx_bitstream_t *bs,
                  uint32_t           parent_abbrev_width) {
    lagfx_block_t blk;
    if (!lagfx_block_enter(bs, parent_abbrev_width, &blk)) return false;
    /* Jump straight to end_pos + align to 32 bits. */
    if (!lagfx_bs_seek(bs, blk.end_pos)) return false;
    return true;
}

/* === DEFINE_ABBREV payload skipper ================================ */

/* Skip a DEFINE_ABBREV record payload. Format (per spec):
 *   numops:vbr5
 *   ...numops operand descriptors...
 *
 * Each operand descriptor:
 *   isLiteral:fixed1
 *   if isLiteral:
 *     value:vbr8
 *   else:
 *     encoding:fixed3
 *     if encoding ∈ {Fixed, VBR}:
 *       width:vbr5
 *
 * Phase 1 doesn't yet REGISTER abbreviations (so we can't decode
 * application-defined abbrev codes), but we have to consume the
 * DEFINE_ABBREV bytes correctly to stay aligned with the bitstream.
 *
 * Encoding codes per spec:
 *   1 = Fixed   (width follows)
 *   2 = VBR     (width follows)
 *   3 = Array   (next operand is the element type)
 *   4 = Char6   (no width; 6-bit char encoding)
 *   5 = Blob    (no width; aligned to 32 bits, length follows)
 */
static bool skip_define_abbrev(lagfx_bitstream_t *bs) {
    bool err = false;
    uint32_t num_ops = lagfx_bs_read_vbr(bs, 5, &err);
    if (err) return false;
    for (uint32_t i = 0; i < num_ops; i++) {
        uint32_t is_literal = lagfx_bs_read_bits(bs, 1, &err);
        if (err) return false;
        if (is_literal) {
            (void)lagfx_bs_read_vbr(bs, 8, &err);
            if (err) return false;
        } else {
            uint32_t encoding = lagfx_bs_read_bits(bs, 3, &err);
            if (err) return false;
            if (encoding == 1u || encoding == 2u) {  /* Fixed or VBR */
                (void)lagfx_bs_read_vbr(bs, 5, &err);
                if (err) return false;
            }
            /* Array (3), Char6 (4), Blob (5): no width operand. */
        }
    }
    return true;
}

/* === Record reader ================================================ */

bool
lagfx_block_next_record(lagfx_block_t  *block,
                         uint64_t       *scratch_ops,
                         lagfx_record_t *out_record,
                         bool           *out_is_end,
                         bool           *out_is_subblock,
                         bool           *out_is_define_abbrev,
                         uint32_t       *out_subblock_peek_id) {
    *out_is_end = false;
    *out_is_subblock = false;
    *out_is_define_abbrev = false;
    out_record->code = 0u;
    out_record->ops = NULL;
    out_record->num_ops = 0u;
    out_record->blob_data = NULL;
    out_record->blob_len = 0u;

    /* Early end-of-block detection — if cursor is at end_pos. */
    if (lagfx_bs_pos(block->bs) >= block->end_pos) {
        *out_is_end = true;
        return true;
    }

    bool err = false;
    uint32_t code = lagfx_bs_read_bits(block->bs, block->abbrev_width, &err);
    if (err) return false;

    if (code == LAGFX_ABBREV_END_BLOCK) {
        /* Align cursor up to 32 bits per spec. */
        lagfx_bs_align_32(block->bs);
        *out_is_end = true;
        return true;
    }

    if (code == LAGFX_ABBREV_ENTER_SUBBLOCK) {
        /* Peek block ID for the caller's dispatcher, but don't consume
         * the rest of the ENTER_SUBBLOCK payload yet. We've already
         * consumed the abbrev code; the next thing is blockid:vbr8.
         * We have to read that to give the caller a useful peek, but
         * we ALSO have to give the caller the option to skip or enter,
         * which means seeking back. Solution: stash the position
         * before the vbr8 read so we can seek back. */
        size_t save_pos = lagfx_bs_pos(block->bs);
        uint32_t blockid = lagfx_bs_read_vbr(block->bs, 8, &err);
        if (err) return false;
        /* Seek back so the caller's enter/skip can re-read the
         * blockid as if from scratch. We do NOT re-emit the abbrev
         * code byte, so the caller must use a helper that starts at
         * "payload begins here". Both lagfx_block_enter() and
         * lagfx_block_skip() above expect the abbrev code at the
         * cursor; to make their contract uniform, we instead seek
         * back to BEFORE the abbrev code we consumed. */
        if (!lagfx_bs_seek(block->bs, save_pos - block->abbrev_width)) return false;
        *out_is_subblock = true;
        if (out_subblock_peek_id) *out_subblock_peek_id = blockid;
        return true;
    }

    if (code == LAGFX_ABBREV_DEFINE_ABBREV) {
        if (!skip_define_abbrev(block->bs)) return false;
        *out_is_define_abbrev = true;
        return true;
    }

    if (code == LAGFX_ABBREV_UNABBREV_RECORD) {
        /* Read record code:vbr6 + numOps:vbr6 + each op:vbr6. */
        uint32_t record_code = lagfx_bs_read_vbr(block->bs, 6, &err);
        if (err) return false;
        uint32_t num_ops = lagfx_bs_read_vbr(block->bs, 6, &err);
        if (err) return false;
        if (num_ops > LAGFX_RECORD_MAX_OPS) return false;
        for (uint32_t i = 0; i < num_ops; i++) {
            scratch_ops[i] = lagfx_bs_read_vbr_64(block->bs, 6, &err);
            if (err) return false;
        }
        out_record->code = record_code;
        out_record->ops = scratch_ops;
        out_record->num_ops = num_ops;
        return true;
    }

    /* Application-defined abbreviation (code >= 4). Phase 1 doesn't
     * yet register/replay abbreviations, so we can't decode these.
     * For the module-level blocks we care about, almost all data is
     * UNABBREV_RECORD (the LLVM bitcode emitter uses DEFINE_ABBREV
     * for size compression but always falls back to UNABBREV for
     * fields that don't fit the abbreviation pattern).
     *
     * For now: signal as an error so the caller knows it hit one.
     * Future work: implement abbrev table replay. */
    return false;
}
