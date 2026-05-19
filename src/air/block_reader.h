/*
 * libapplegfx-vulkan — LLVM Bitstream block-level traversal
 * src/air/block_reader.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Block structure (per LLVM Bitstream spec):
 *
 *   <ENTER_SUBBLOCK abbrev=1>
 *     <blockID:vbr8>
 *     <newAbbrevLen:vbr4>
 *     <align32>
 *     <blockSize:32>       (in 32-bit words)
 *     ...records and sub-blocks...
 *   <END_BLOCK abbrev=0>   (followed by align32)
 *
 * Records inside a block:
 *   <abbrev_code:abbrev_width_bits>
 *   if abbrev_code == 2 (DEFINE_ABBREV):
 *     <numops:vbr5>
 *     ...numops operand descriptors...
 *   if abbrev_code == 3 (UNABBREV_RECORD):
 *     <recordCode:vbr6>
 *     <numOps:vbr6>
 *     ...numOps operands, each vbr6...
 *   if abbrev_code >= 4: application-defined abbreviation (looked up in table)
 *
 * Phase 1 supports UNABBREV_RECORD fully, and a subset of DEFINE_ABBREV
 * patterns sufficient for the module-level blocks we see in real
 * captures. FUNCTION_BLOCK uses Apple-specific custom abbrevs; Phase 2
 * handles those.
 */

#ifndef LIBAPPLEGFX_AIR_BLOCK_READER_H
#define LIBAPPLEGFX_AIR_BLOCK_READER_H

#include "bitstream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-record decoded view. UNABBREV_RECORD always lands here with raw
 * operand values. Defined-abbrev records also decode into this shape;
 * the abbrev table records the per-operand types so the dispatcher can
 * fill in the values uniformly. */
typedef struct {
    uint32_t code;          /* application-defined record code */
    const uint64_t *ops;    /* operand values; pointer into per-block scratch */
    uint32_t num_ops;
    /* For blob/string operands inlined as byte arrays, blob_data is
     * non-NULL and blob_len gives the byte count. Otherwise blob_data
     * is NULL and string-like operands appear as one-byte-per-op in
     * `ops` (less efficient but legal LLVM encoding). */
    const uint8_t *blob_data;
    size_t         blob_len;
} lagfx_record_t;

/* Predeclared LLVM Bitstream standard abbrev codes (always available
 * regardless of the per-block abbrev table). */
#define LAGFX_ABBREV_END_BLOCK        0u
#define LAGFX_ABBREV_ENTER_SUBBLOCK   1u
#define LAGFX_ABBREV_DEFINE_ABBREV    2u
#define LAGFX_ABBREV_UNABBREV_RECORD  3u

/* Standard LLVM block IDs we encounter in Phase 1. */
#define LAGFX_BLK_BLOCKINFO            0u
#define LAGFX_BLK_MODULE               8u
#define LAGFX_BLK_PARAMATTR            9u
#define LAGFX_BLK_PARAMATTR_GROUP     10u
#define LAGFX_BLK_CONSTANTS           11u
#define LAGFX_BLK_FUNCTION            12u
#define LAGFX_BLK_IDENTIFICATION      13u
#define LAGFX_BLK_VALUE_SYMTAB        14u
#define LAGFX_BLK_METADATA            15u
#define LAGFX_BLK_METADATA_ATTACHMENT 16u
#define LAGFX_BLK_TYPE                17u
#define LAGFX_BLK_USELIST             18u
#define LAGFX_BLK_OPERAND_BUNDLE_TAGS 21u
#define LAGFX_BLK_METADATA_KIND       22u
#define LAGFX_BLK_STRTAB              23u
#define LAGFX_BLK_SYMTAB              24u

/* Maximum operands we expect in any single record. LLVM allows
 * unbounded but the real-world max for module-level blocks is in
 * the hundreds (struct types with many fields, paramattr groups).
 * 1024 is a safe ceiling. */
#define LAGFX_RECORD_MAX_OPS  1024u

/* Per-block context. The caller initializes for the outermost
 * MODULE_BLOCK; recursive entries push/pop via lagfx_block_enter()
 * and lagfx_block_exit() to manage the abbreviation width stack. */
typedef struct {
    lagfx_bitstream_t *bs;     /* underlying cursor (caller-owned) */
    uint32_t abbrev_width;     /* current bits-per-abbrev-code (set by ENTER_SUBBLOCK) */
    uint32_t block_id;         /* block ID we entered (debugging) */
    size_t   end_pos;          /* absolute bit position where this block ends */
} lagfx_block_t;

/* Enter the next sub-block at the current cursor position.
 *
 * Cursor must be positioned at an ENTER_SUBBLOCK abbrev code. Reads:
 *   ENTER_SUBBLOCK code (abbrev_width bits) -- caller already consumed if want_id check
 *   blockid:vbr8
 *   newAbbrevWidth:vbr4
 *   <align to 32 bits>
 *   blockSize:32 (in 32-bit words)
 *
 * After this call, the cursor is positioned at the first record/
 * sub-block within the new block, ready to be parsed.
 *
 * Returns false on protocol error or if `expected_id` is non-zero and
 * the block ID doesn't match. Caller should typically peek at the
 * block ID before deciding whether to enter or skip.
 */
bool lagfx_block_enter(lagfx_bitstream_t *bs,
                        uint32_t           parent_abbrev_width,
                        lagfx_block_t     *out_block);

/* Skip over a sub-block. Cursor must be positioned at an
 * ENTER_SUBBLOCK code. Advances the cursor to immediately after the
 * matching END_BLOCK. */
bool lagfx_block_skip(lagfx_bitstream_t *bs,
                       uint32_t           parent_abbrev_width);

/* Read the next record OR detect the END_BLOCK.
 *
 * On EOF / END_BLOCK: returns true, sets *out_is_end = true.
 * On nested ENTER_SUBBLOCK: returns true, sets *out_is_subblock = true.
 *   The caller can then either lagfx_block_enter() or lagfx_block_skip().
 *   In this case the abbrev code has been consumed; the cursor is at
 *   the position immediately after.
 * On DEFINE_ABBREV: returns true, sets *out_is_define_abbrev = true.
 *   Phase 1 currently SKIPS abbreviation definitions (treats their
 *   contents as opaque); future iterations register them into a per-
 *   block abbrev table.
 * On UNABBREV_RECORD: returns true, sets *out_record populated with
 *   code + ops. The ops point into `scratch_ops` (caller-owned, must
 *   be at least LAGFX_RECORD_MAX_OPS u64s).
 * On any error: returns false.
 *
 * If the current abbrev_code is >= 4 (application-defined), Phase 1
 * currently returns an error — we don't yet decode DEFINE_ABBREV
 * patterns. This is sufficient for many small module-level blocks
 * but will need to be extended for blocks that use DEFINE_ABBREV
 * extensively (CONSTANTS_BLOCK, METADATA_BLOCK).
 */
bool lagfx_block_next_record(lagfx_block_t  *block,
                              uint64_t       *scratch_ops,
                              lagfx_record_t *out_record,
                              bool           *out_is_end,
                              bool           *out_is_subblock,
                              bool           *out_is_define_abbrev,
                              uint32_t       *out_subblock_peek_id /* if subblock: block ID */);

#endif /* LIBAPPLEGFX_AIR_BLOCK_READER_H */
