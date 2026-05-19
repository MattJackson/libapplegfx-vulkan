/*
 * libapplegfx-vulkan — LLVM Bitstream abbreviation table
 * src/air/abbrev.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Abbreviations let the bitstream emitter compress common record
 * shapes. A DEFINE_ABBREV pattern specifies a sequence of operand
 * descriptors; subsequent records using that abbrev ID are read by
 * following the pattern instead of the default UNABBREV encoding.
 *
 * Per LLVM Bitstream spec, operand encodings:
 *   Literal: a fixed value baked into the abbrev (decoder emits without reading)
 *   Fixed:   N-bit unsigned integer
 *   VBR:     N-bit variable bit-rate integer
 *   Array:   followed by another operand specifying the element type;
 *            length:vbr6 then length × element-type reads
 *   Char6:   6-bit char encoding (a-z, A-Z, 0-9, ., _)
 *   Blob:    aligned 32-bit length then aligned bytes
 *
 * Abbreviation scope:
 *   - Defined inside a block: usable only in that block instance
 *   - Defined inside BLOCKINFO_BLOCK + SETBID: usable in every block
 *     of that target block ID across the module
 *
 * Abbreviation IDs start at 4 (codes 0-3 are reserved for END_BLOCK,
 * ENTER_SUBBLOCK, DEFINE_ABBREV, UNABBREV_RECORD).
 */

#ifndef LIBAPPLEGFX_AIR_ABBREV_H
#define LIBAPPLEGFX_AIR_ABBREV_H

#include "bitstream.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LAGFX_ABBREV_OP_LITERAL = 0,
    LAGFX_ABBREV_OP_FIXED   = 1,
    LAGFX_ABBREV_OP_VBR     = 2,
    LAGFX_ABBREV_OP_ARRAY   = 3,
    LAGFX_ABBREV_OP_CHAR6   = 4,
    LAGFX_ABBREV_OP_BLOB    = 5,
} lagfx_abbrev_op_kind_t;

typedef struct {
    lagfx_abbrev_op_kind_t kind;
    /* For LITERAL: the literal value emitted at decode time.
     * For FIXED:   the bit width of the operand.
     * For VBR:     the VBR chunk bit count (typically 6 or 8).
     * For ARRAY:   ignored; the NEXT operand in the pattern is the
     *              element type encoding (which can be LITERAL/FIXED/
     *              VBR/CHAR6 but NOT another ARRAY or BLOB).
     * For CHAR6:   ignored (fixed 6-bit width).
     * For BLOB:    ignored (length is vbr6, payload is aligned bytes).
     */
    uint64_t value_or_width;
} lagfx_abbrev_op_t;

/* Maximum operands in a single abbrev pattern. LLVM bitcode emits
 * 5-15 typically; 64 is safe. */
#define LAGFX_ABBREV_MAX_OPS 64u

/* Maximum app-defined abbreviations per block. LLVM bitcode emits
 * up to ~30. 64 is safe. */
#define LAGFX_ABBREV_MAX_PER_BLOCK 64u

typedef struct {
    lagfx_abbrev_op_t ops[LAGFX_ABBREV_MAX_OPS];
    uint32_t          num_ops;
} lagfx_abbrev_t;

/* Per-block abbrev table. Index N corresponds to abbrev ID (N + 4).
 * Locally-defined abbrevs are appended as DEFINE_ABBREV records are
 * read. BLOCKINFO-sourced abbrevs are pre-installed when the block is
 * entered. */
typedef struct {
    lagfx_abbrev_t entries[LAGFX_ABBREV_MAX_PER_BLOCK];
    uint32_t       num_entries;
} lagfx_abbrev_table_t;

/* Read a DEFINE_ABBREV record from the bitstream and append it to the
 * given table. Cursor must be positioned immediately after the
 * DEFINE_ABBREV abbrev code (i.e., the caller has already consumed
 * the 2-bit-or-wider abbrev code = 2). Returns true on success.
 * Returns false on malformed pattern or if the table is full. */
bool lagfx_abbrev_table_define(lagfx_abbrev_table_t *table,
                                lagfx_bitstream_t   *bs);

/* Decode a record using an abbreviation pattern from the table.
 * Cursor must be positioned immediately after the abbrev code (which
 * the caller has read and identified as `abbrev_id` >= 4).
 *
 * `scratch_ops` is the operand buffer (LAGFX_RECORD_MAX_OPS u64 slots).
 *
 * On success:
 *   *out_record_code = the record code (always the first operand by
 *                      convention; LLVM stores record_code as the
 *                      first decoded operand of an abbrev pattern).
 *   *out_num_ops     = number of operand values (NOT including the
 *                      record code).
 *   The first operand-by-convention is the record code itself; the
 *   remaining are the actual record operands.
 *
 *   If the pattern contains a BLOB, the out_blob_data / out_blob_len params
 *   into the underlying bitstream buffer (caller-owned, valid for the
 *   lifetime of the bitstream).
 *
 * Returns false on malformed bitstream or unsupported encoding.
 */
bool lagfx_abbrev_decode_record(const lagfx_abbrev_table_t *table,
                                 uint32_t              abbrev_id,
                                 lagfx_bitstream_t    *bs,
                                 uint64_t             *scratch_ops,
                                 uint32_t              scratch_capacity,
                                 uint32_t             *out_record_code,
                                 uint32_t             *out_num_ops,
                                 const uint8_t       **out_blob_data,
                                 uint32_t             *out_blob_len);

/* Reset table (used when entering a fresh block). */
static inline void lagfx_abbrev_table_reset(lagfx_abbrev_table_t *t) {
    t->num_entries = 0u;
}

/* Copy abbreviations from `src` into `dst`. Used to pre-install
 * BLOCKINFO-sourced abbrevs into a newly-entered block. */
bool lagfx_abbrev_table_copy(lagfx_abbrev_table_t       *dst,
                              const lagfx_abbrev_table_t *src);

#endif /* LIBAPPLEGFX_AIR_ABBREV_H */
