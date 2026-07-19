/*
 * libapplegfx-vulkan — LLVM Bitstream abbreviation table
 * src/air/abbrev.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#include "abbrev.h"
#include "../common/log.h"

#include <stdio.h>
#include <string.h>

/* Map 6-bit char encoding back to ASCII per LLVM spec.
 * Bits 0..25 → 'a'..'z'
 * Bits 26..51 → 'A'..'Z'
 * Bits 52..61 → '0'..'9'
 * Bit 62 → '.'
 * Bit 63 → '_'
 */
static inline char char6_decode(uint32_t v) {
    if (v < 26u) return (char)('a' + v);
    if (v < 52u) return (char)('A' + (v - 26u));
    if (v < 62u) return (char)('0' + (v - 52u));
    if (v == 62u) return '.';
    return '_';
}

bool
lagfx_abbrev_table_define(lagfx_abbrev_table_t *table,
                          lagfx_bitstream_t   *bs) {
    if (table->num_entries >= LAGFX_ABBREV_MAX_PER_BLOCK) return false;

    bool err = false;
    uint32_t num_ops = lagfx_bs_read_vbr(bs, 5, &err);
    if (err || num_ops == 0u || num_ops > LAGFX_ABBREV_MAX_OPS) return false;

    lagfx_abbrev_t *ab = &table->entries[table->num_entries];
    ab->num_ops = num_ops;

    for (uint32_t i = 0; i < num_ops; i++) {
        uint32_t is_literal = lagfx_bs_read_bits(bs, 1, &err);
        if (err) return false;
        if (is_literal) {
            ab->ops[i].kind = LAGFX_ABBREV_OP_LITERAL;
            ab->ops[i].value_or_width = lagfx_bs_read_vbr_64(bs, 8, &err);
            if (err) return false;
        } else {
            uint32_t encoding = lagfx_bs_read_bits(bs, 3, &err);
            if (err) return false;
            switch (encoding) {
                case 1u:  /* Fixed */
                    ab->ops[i].kind = LAGFX_ABBREV_OP_FIXED;
                    ab->ops[i].value_or_width = lagfx_bs_read_vbr(bs, 5, &err);
                    if (err) return false;
                    break;
                case 2u:  /* VBR */
                    ab->ops[i].kind = LAGFX_ABBREV_OP_VBR;
                    ab->ops[i].value_or_width = lagfx_bs_read_vbr(bs, 5, &err);
                    if (err) return false;
                    break;
                case 3u:  /* Array */
                    ab->ops[i].kind = LAGFX_ABBREV_OP_ARRAY;
                    ab->ops[i].value_or_width = 0u;
                    /* The NEXT operand specifies the element encoding;
                     * caller reads it on the next loop iteration. */
                    break;
                case 4u:  /* Char6 */
                    ab->ops[i].kind = LAGFX_ABBREV_OP_CHAR6;
                    ab->ops[i].value_or_width = 0u;
                    break;
                case 5u:  /* Blob */
                    ab->ops[i].kind = LAGFX_ABBREV_OP_BLOB;
                    ab->ops[i].value_or_width = 0u;
                    break;
                default:
                    return false;
            }
        }
    }
    table->num_entries++;
    /* Trace: dump the just-installed pattern. Helps Phase 2.4 RE see
     * what shapes captured-macOS DEFINE_ABBREV records take. */
    {
        char pat[256]; size_t off = 0;
        for (uint32_t pi = 0; pi < ab->num_ops && off < sizeof(pat) - 16; pi++) {
            const char *k = "?";
            switch (ab->ops[pi].kind) {
                case LAGFX_ABBREV_OP_LITERAL: k = "LIT"; break;
                case LAGFX_ABBREV_OP_FIXED:   k = "FIX"; break;
                case LAGFX_ABBREV_OP_VBR:     k = "VBR"; break;
                case LAGFX_ABBREV_OP_ARRAY:   k = "ARR"; break;
                case LAGFX_ABBREV_OP_CHAR6:   k = "CH6"; break;
                case LAGFX_ABBREV_OP_BLOB:    k = "BLB"; break;
            }
            int n = snprintf(pat + off, sizeof(pat) - off,
                             "%s%s(%llu)", pi ? "," : "", k,
                             (unsigned long long)ab->ops[pi].value_or_width);
            if (n < 0) break;
            off += (size_t)n;
        }
        LAGFX_TRACE("abbrev_define: abbrev_id=%u (slot=%u) ops=[%s]",
                    4u + table->num_entries - 1u,
                    table->num_entries - 1u, pat);
    }
    return true;
}

bool
lagfx_abbrev_table_copy(lagfx_abbrev_table_t       *dst,
                        const lagfx_abbrev_table_t *src) {
    if (src->num_entries + dst->num_entries > LAGFX_ABBREV_MAX_PER_BLOCK) {
        return false;
    }
    for (uint32_t i = 0; i < src->num_entries; i++) {
        dst->entries[dst->num_entries + i] = src->entries[i];
    }
    dst->num_entries += src->num_entries;
    return true;
}

/* Decode a single operand-value using the given pattern op.
 * For ARRAY ops, this is called per-element and `op` is the element
 * encoding (NOT the ARRAY entry itself). */
static bool
decode_one_value(const lagfx_abbrev_op_t *op,
                 lagfx_bitstream_t       *bs,
                 uint64_t                *out) {
    bool err = false;
    switch (op->kind) {
        case LAGFX_ABBREV_OP_LITERAL:
            *out = op->value_or_width;
            return true;
        case LAGFX_ABBREV_OP_FIXED: {
            uint32_t w = (uint32_t)op->value_or_width;
            if (w == 0u) { *out = 0u; return true; }
            if (w > 32u) {
                /* Read in two halves. */
                uint64_t lo = lagfx_bs_read_bits(bs, 32, &err);
                if (err) return false;
                uint64_t hi = lagfx_bs_read_bits(bs, w - 32u, &err);
                if (err) return false;
                *out = lo | (hi << 32);
                return true;
            }
            *out = lagfx_bs_read_bits(bs, w, &err);
            return !err;
        }
        case LAGFX_ABBREV_OP_VBR: {
            uint32_t w = (uint32_t)op->value_or_width;
            *out = lagfx_bs_read_vbr_64(bs, w, &err);
            return !err;
        }
        case LAGFX_ABBREV_OP_CHAR6: {
            uint32_t v = lagfx_bs_read_bits(bs, 6, &err);
            if (err) return false;
            *out = (uint64_t)char6_decode(v);
            return true;
        }
        default:
            return false;  /* ARRAY/BLOB handled by caller */
    }
}

bool
lagfx_abbrev_decode_record(const lagfx_abbrev_table_t *table,
                            uint32_t              abbrev_id,
                            lagfx_bitstream_t    *bs,
                            uint64_t             *scratch_ops,
                            uint32_t              scratch_capacity,
                            uint32_t             *out_record_code,
                            uint32_t             *out_num_ops,
                            const uint8_t       **out_blob_data,
                            uint32_t             *out_blob_len) {
    if (abbrev_id < 4u) return false;
    uint32_t idx = abbrev_id - 4u;
    if (idx >= table->num_entries) return false;
    const lagfx_abbrev_t *ab = &table->entries[idx];

    *out_blob_data = NULL;
    *out_blob_len = 0u;

    /* Convention: the first abbrev op decodes to the record code.
     * Subsequent ops decode to operands stored in scratch_ops. */
    uint32_t op_count = 0u;
    bool got_code = false;
    uint32_t code = 0u;

    for (uint32_t i = 0; i < ab->num_ops; i++) {
        const lagfx_abbrev_op_t *op = &ab->ops[i];
        if (op->kind == LAGFX_ABBREV_OP_ARRAY) {
            /* Next op is the element type. */
            if (i + 1u >= ab->num_ops) return false;
            const lagfx_abbrev_op_t *elem = &ab->ops[i + 1u];
            bool err = false;
            uint32_t length = lagfx_bs_read_vbr(bs, 6, &err);
            if (err) return false;
            if (op_count + length > scratch_capacity) return false;
            for (uint32_t k = 0; k < length; k++) {
                uint64_t v = 0u;
                if (!decode_one_value(elem, bs, &v)) return false;
                if (!got_code) {
                    code = (uint32_t)v;
                    got_code = true;
                } else {
                    scratch_ops[op_count++] = v;
                }
            }
            i++;  /* consume the element-type op too */
            continue;
        }
        if (op->kind == LAGFX_ABBREV_OP_BLOB) {
            bool err = false;
            size_t pos_before_len = lagfx_bs_pos(bs);
            uint32_t length = lagfx_bs_read_vbr(bs, 6, &err);
            if (err) return false;
            lagfx_bs_align_32(bs);
            size_t pos_after_align = lagfx_bs_pos(bs);
            size_t byte_off = pos_after_align >> 3;
            if (byte_off + length > bs->buf_len) {
                LAGFX_LOG("abbrev BLOB: length=%u at bit %zu (after align %zu) overflows buf_len=%zu",
                          length, pos_before_len, pos_after_align, bs->buf_len);
                return false;
            }
            *out_blob_data = bs->buf + byte_off;
            *out_blob_len = length;
            /* Advance cursor: payload aligned bytes + align to 32 again. */
            bs->bit_pos += (size_t)length * 8u;
            lagfx_bs_align_32(bs);
            LAGFX_TRACE("abbrev BLOB: vbr6@%zu length=%u align→%zu end→%zu",
                        pos_before_len, length, pos_after_align,
                        lagfx_bs_pos(bs));
            continue;
        }
        /* Scalar op: LITERAL / FIXED / VBR / CHAR6. */
        uint64_t v = 0u;
        if (!decode_one_value(op, bs, &v)) return false;
        if (!got_code) {
            code = (uint32_t)v;
            got_code = true;
        } else {
            if (op_count >= scratch_capacity) return false;
            scratch_ops[op_count++] = v;
        }
    }

    if (!got_code) return false;
    *out_record_code = code;
    *out_num_ops = op_count;
    return true;
}
