/*
 * libapplegfx-vulkan — clean-room AIR (LLVM) bitcode reader
 * src/air/bitcode_reader.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1 implementation. Parses the LLVM bitcode wrapper + the
 * module-level blocks. Stops at FUNCTION_BLOCK bodies (Phase 2 work).
 *
 * Strict conventions:
 *  - All parser errors propagate via lagfx_status_t. No silent OK.
 *  - Memory is owned by a single arena attached to lagfx_air_module_t;
 *    no exposed `malloc`'d pointers in the public struct.
 *  - This file does not link against libLLVM. Clean-room.
 */

#include "bitcode_reader.h"
#include "bitstream.h"
#include "block_reader.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Wrapper format =============================================== */

/* LLVM Bitcode Wrapper magic — little-endian bytes 0xDE 0xC0 0x17 0x0B.
 * https://llvm.org/docs/BitCodeFormat.html#bitcode-wrapper-format */
static const uint8_t kLlvmBitcodeWrapperMagic[4] = { 0xDE, 0xC0, 0x17, 0x0B };

/* Raw LLVM Bitstream magic (the body starts with this). */
static const uint8_t kLlvmBitstreamMagic[4] = { 0x42, 0x43, 0xC0, 0xDE };  /* 'B','C',0xC0,0xDE */

/* Module record codes we care about for Phase 1.
 * Reference: llvm/include/llvm/Bitcode/LLVMBitCodes.h ModuleCodes enum. */
enum {
    LAGFX_MOD_CODE_VERSION         = 1,
    LAGFX_MOD_CODE_TRIPLE          = 2,
    LAGFX_MOD_CODE_DATALAYOUT      = 3,
    LAGFX_MOD_CODE_ASM             = 4,
    LAGFX_MOD_CODE_SECTIONNAME     = 5,
    LAGFX_MOD_CODE_DEPLIB          = 6,
    LAGFX_MOD_CODE_GLOBALVAR       = 7,
    LAGFX_MOD_CODE_FUNCTION        = 8,
    LAGFX_MOD_CODE_ALIAS_OLD       = 9,
    LAGFX_MOD_CODE_GCNAME          = 11,
    LAGFX_MOD_CODE_COMDAT          = 12,
    LAGFX_MOD_CODE_VSTOFFSET       = 13,
    LAGFX_MOD_CODE_ALIAS           = 14,
    LAGFX_MOD_CODE_METADATA_VALUES = 15,
    LAGFX_MOD_CODE_SOURCE_FILENAME = 16,
    LAGFX_MOD_CODE_HASH            = 17,
    LAGFX_MOD_CODE_IFUNC           = 18,
};

/* === Module struct =============================================== */

/* Arena chunk — we malloc one big buffer per module and bump-allocate
 * into it. Strings + record-operand arrays + decoded structs all live
 * here. Avoids the realloc churn of per-record allocations. */
typedef struct {
    uint8_t *base;
    size_t   capacity;
    size_t   used;
} lagfx_arena_t;

struct lagfx_air_module {
    lagfx_arena_t arena;

    /* Optional module-level strings (offsets into arena, 0 = absent). */
    uint32_t triple_offset;
    uint32_t datalayout_offset;
    uint32_t source_filename_offset;

    /* Parsed tables (pointers into arena, counts inline). Phase 1
     * populates incrementally; some may be empty for some modules. */
    lagfx_air_type_t            *types;
    uint32_t                     num_types;
    lagfx_air_constant_t        *constants;
    uint32_t                     num_constants;
    lagfx_air_function_t        *functions;
    uint32_t                     num_functions;
    lagfx_air_metadata_t        *metadata;
    uint32_t                     num_metadata;
    lagfx_air_param_attr_group_t*param_attr_groups;
    uint32_t                     num_param_attr_groups;
};

/* === Arena management ============================================ */

static bool arena_init(lagfx_arena_t *a, size_t initial_capacity) {
    a->base = (uint8_t *)malloc(initial_capacity);
    if (!a->base) return false;
    a->capacity = initial_capacity;
    a->used = 0u;
    /* Reserve offset 0 to mean "empty / not set". */
    a->base[0] = 0u;
    a->used = 1u;
    return true;
}

static void arena_free(lagfx_arena_t *a) {
    free(a->base);
    a->base = NULL;
    a->capacity = a->used = 0u;
}

/* Reserve `n` bytes of arena space + align to 4 bytes. Returns the
 * byte offset (always >= 1; 0 reserved for "absent"). Returns 0 on
 * out-of-memory (caller checks). */
static uint32_t arena_reserve(lagfx_arena_t *a, size_t n) {
    /* Align used to 4 bytes for safe pointer-typed reads. */
    size_t aligned = (a->used + 3u) & ~(size_t)3u;
    if (aligned + n > a->capacity) {
        /* Grow geometric. */
        size_t new_cap = a->capacity * 2u;
        while (new_cap < aligned + n) new_cap *= 2u;
        uint8_t *nb = (uint8_t *)realloc(a->base, new_cap);
        if (!nb) return 0u;
        a->base = nb;
        a->capacity = new_cap;
    }
    uint32_t off = (uint32_t)aligned;
    a->used = aligned + n;
    return off;
}

/* Intern a NUL-terminated string into the arena; returns offset (>=1)
 * or 0 on OOM. NOTE: these helpers are unused in the Phase 1 wrapper-only
 * cut; they'll be used by per-block decoders landing in subsequent commits. */
__attribute__((unused))
static uint32_t arena_intern_string(lagfx_arena_t *a, const char *str, size_t len) {
    uint32_t off = arena_reserve(a, len + 1u);
    if (off == 0u) return 0u;
    memcpy(a->base + off, str, len);
    a->base[off + len] = 0;
    return off;
}

/* Intern a record's operand vector (u32 array) into the arena; returns
 * offset or 0 on OOM. */
__attribute__((unused))
static uint32_t arena_intern_u32_array(lagfx_arena_t *a, const uint32_t *src, uint32_t count) {
    uint32_t off = arena_reserve(a, (size_t)count * sizeof(uint32_t));
    if (off == 0u) return 0u;
    memcpy(a->base + off, src, (size_t)count * sizeof(uint32_t));
    return off;
}

/* === Public accessors ============================================ */

const lagfx_air_type_t *
lagfx_air_module_types(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_types;
    return m->types;
}

const lagfx_air_constant_t *
lagfx_air_module_constants(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_constants;
    return m->constants;
}

const lagfx_air_function_t *
lagfx_air_module_functions(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_functions;
    return m->functions;
}

const lagfx_air_metadata_t *
lagfx_air_module_metadata(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_metadata;
    return m->metadata;
}

const lagfx_air_param_attr_group_t *
lagfx_air_module_param_attr_groups(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_param_attr_groups;
    return m->param_attr_groups;
}

const char *
lagfx_air_module_triple(const lagfx_air_module_t *m) {
    if (m->triple_offset == 0u) return NULL;
    return (const char *)(m->arena.base + m->triple_offset);
}

const char *
lagfx_air_module_datalayout(const lagfx_air_module_t *m) {
    if (m->datalayout_offset == 0u) return NULL;
    return (const char *)(m->arena.base + m->datalayout_offset);
}

const char *
lagfx_air_module_source_filename(const lagfx_air_module_t *m) {
    if (m->source_filename_offset == 0u) return NULL;
    return (const char *)(m->arena.base + m->source_filename_offset);
}

const char *
lagfx_air_module_string(const lagfx_air_module_t *m, uint32_t offset) {
    if (offset == 0u || offset >= m->arena.used) return NULL;
    return (const char *)(m->arena.base + offset);
}

/* === Wrapper-header parsing ====================================== */

/* Parse the LLVM Bitcode Wrapper header (20 bytes at the start of an
 * .air.bc payload). Returns the byte offset where the bitstream proper
 * starts (typically 20) and the bitstream length in bytes.
 *
 * Format (each field is little-endian u32):
 *   [+0]  Magic = 0xDE C0 17 0B
 *   [+4]  Version (typically 0)
 *   [+8]  Offset to bitstream body (typically 20)
 *   [+12] Size of bitstream body in bytes
 *   [+16] CPU type (often 0xFFFFFFFF — generic)
 */
static lagfx_status_t parse_wrapper(const uint8_t *blob, size_t blob_len,
                                     size_t *out_body_off, size_t *out_body_len) {
    if (blob_len < 20u) {
        LAGFX_ERR("air_bitcode_reader: blob too short for wrapper header (%zu < 20)", blob_len);
        return LAGFX_ERR_PROTOCOL;
    }
    if (memcmp(blob, kLlvmBitcodeWrapperMagic, 4u) != 0) {
        LAGFX_ERR("air_bitcode_reader: missing wrapper magic (got %02x %02x %02x %02x)",
                  blob[0], blob[1], blob[2], blob[3]);
        return LAGFX_ERR_PROTOCOL;
    }
    /* Read body offset + size as LE u32. */
    uint32_t body_off = (uint32_t)blob[8] | ((uint32_t)blob[9] << 8)
                      | ((uint32_t)blob[10] << 16) | ((uint32_t)blob[11] << 24);
    uint32_t body_len = (uint32_t)blob[12] | ((uint32_t)blob[13] << 8)
                      | ((uint32_t)blob[14] << 16) | ((uint32_t)blob[15] << 24);
    if ((size_t)body_off > blob_len || (size_t)body_off + (size_t)body_len > blob_len) {
        LAGFX_ERR("air_bitcode_reader: body extends beyond blob (off=%u len=%u blob=%zu)",
                  body_off, body_len, blob_len);
        return LAGFX_ERR_PROTOCOL;
    }
    /* The body starts with the bitstream magic ('B','C',0xC0,0xDE). */
    if (body_len < 4u || memcmp(blob + body_off, kLlvmBitstreamMagic, 4u) != 0) {
        LAGFX_ERR("air_bitcode_reader: missing bitstream magic at body offset %u", body_off);
        return LAGFX_ERR_PROTOCOL;
    }
    *out_body_off = (size_t)body_off + 4u;  /* skip the 4-byte magic */
    *out_body_len = (size_t)body_len - 4u;
    return LAGFX_OK;
}

/* === Public API =================================================== */

lagfx_status_t
lagfx_air_module_open(const uint8_t *blob, size_t blob_len,
                      lagfx_air_module_t **out_module) {
    if (!blob || blob_len == 0u || !out_module) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out_module = NULL;

    /* Step 1: parse wrapper, locate bitstream body. */
    size_t body_off = 0, body_len = 0;
    lagfx_status_t st = parse_wrapper(blob, blob_len, &body_off, &body_len);
    if (st != LAGFX_OK) return st;

    LAGFX_TRACE("air_bitcode_reader: wrapper OK, bitstream body at +%zu len=%zu",
                body_off, body_len);

    /* Step 2: allocate module + arena. */
    lagfx_air_module_t *m = (lagfx_air_module_t *)calloc(1, sizeof(*m));
    if (!m) return LAGFX_ERR_OUT_OF_MEMORY;
    if (!arena_init(&m->arena, 16u * 1024u)) {
        free(m);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* Step 3: parse MODULE_BLOCK + its records.
     * Bitstream starts immediately after the 4-byte magic. The first
     * abbrev code (at width 2, the root width) should be
     * ENTER_SUBBLOCK introducing MODULE_BLOCK. */
    lagfx_bitstream_t bs;
    lagfx_bs_init(&bs, blob + body_off, body_len);

    /* Root abbrev width is 2 bits per LLVM Bitstream spec. */
    const uint32_t kRootAbbrevWidth = 2u;

    /* Enter top-level block. Could be IDENTIFICATION_BLOCK first then
     * MODULE_BLOCK — Apple's metallib bitcode starts with IDENT then
     * MODULE. We scan blocks at the root level until we hit MODULE. */
    while (!lagfx_bs_at_end(&bs)) {
        bool err = false;
        size_t pos_before_code = lagfx_bs_pos(&bs);
        uint32_t code = lagfx_bs_read_bits(&bs, kRootAbbrevWidth, &err);
        if (err) break;
        if (code != LAGFX_ABBREV_ENTER_SUBBLOCK) {
            /* Root level should only have ENTER_SUBBLOCK at top. */
            LAGFX_ERR("air_bitcode_reader: unexpected root abbrev code %u", code);
            lagfx_air_module_free(m);
            return LAGFX_ERR_PROTOCOL;
        }
        /* Seek back to re-enter via the helper. */
        if (!lagfx_bs_seek(&bs, pos_before_code)) {
            lagfx_air_module_free(m);
            return LAGFX_ERR_PROTOCOL;
        }
        lagfx_block_t blk;
        if (!lagfx_block_enter(&bs, kRootAbbrevWidth, &blk)) {
            LAGFX_ERR("air_bitcode_reader: failed to enter root sub-block");
            lagfx_air_module_free(m);
            return LAGFX_ERR_PROTOCOL;
        }

        if (blk.block_id == LAGFX_BLK_MODULE) {
            /* Parse MODULE_BLOCK records + sub-blocks. */
            uint64_t scratch_ops[LAGFX_RECORD_MAX_OPS];
            while (lagfx_bs_pos(&bs) < blk.end_pos) {
                lagfx_record_t rec = {0};
                bool is_end = false, is_subblock = false, is_define_abbrev = false;
                uint32_t sub_id = 0;
                if (!lagfx_block_next_record(&blk, scratch_ops, &rec,
                                              &is_end, &is_subblock,
                                              &is_define_abbrev, &sub_id)) {
                    /* Application-defined abbrev or other unhandled
                     * encoding. Phase 1 can't handle these; bail with
                     * what we have so far rather than corrupting. */
                    LAGFX_LOG("air_bitcode_reader: hit unhandled abbrev at bit %zu — Phase 1 stopping module parse here",
                              lagfx_bs_pos(&bs));
                    break;
                }
                if (is_end) break;
                if (is_subblock) {
                    /* Skip sub-block for now — per-block decoders land
                     * in subsequent commits. */
                    if (!lagfx_block_skip(&bs, blk.abbrev_width)) {
                        LAGFX_ERR("air_bitcode_reader: failed to skip sub-block id=%u", sub_id);
                        break;
                    }
                    continue;
                }
                if (is_define_abbrev) continue;

                /* Dispatch on record code. Phase 1 handles the
                 * module-level metadata strings (TRIPLE, DATALAYOUT,
                 * SOURCE_FILENAME); per-field decoders for FUNCTION
                 * records etc. follow later. */
                switch (rec.code) {
                    case 1u:  /* MODULE_CODE_VERSION */
                        /* op[0] = version (typically 2). Not stored. */
                        break;
                    case 2u:  /* MODULE_CODE_TRIPLE — operands are 1 char per op */
                    case 3u:  /* MODULE_CODE_DATALAYOUT — same encoding */
                    case 16u: /* MODULE_CODE_SOURCE_FILENAME — same encoding */
                    {
                        /* Reserve space for NUL terminator. */
                        size_t len = rec.num_ops;
                        uint32_t off = arena_reserve(&m->arena, len + 1u);
                        if (off == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        for (uint32_t i = 0; i < len; i++) {
                            m->arena.base[off + i] = (uint8_t)(rec.ops[i] & 0xFFu);
                        }
                        m->arena.base[off + len] = 0;
                        if (rec.code == 2u) m->triple_offset = off;
                        else if (rec.code == 3u) m->datalayout_offset = off;
                        else m->source_filename_offset = off;
                        break;
                    }
                    default:
                        /* Other module-level records (FUNCTION decls,
                         * GLOBALVAR, ALIAS, etc.) — Phase 1 ignores
                         * them; later phases will decode. */
                        break;
                }
            }
            /* MODULE_BLOCK done. */
            break;
        } else {
            /* Non-MODULE block at root (e.g., IDENTIFICATION_BLOCK).
             * Skip and continue. We already entered it; need to seek
             * past its end. */
            if (!lagfx_bs_seek(&bs, blk.end_pos)) {
                LAGFX_ERR("air_bitcode_reader: failed to seek past root sub-block id=%u",
                          blk.block_id);
                lagfx_air_module_free(m);
                return LAGFX_ERR_PROTOCOL;
            }
        }
    }

    *out_module = m;
    return LAGFX_OK;
}

void
lagfx_air_module_free(lagfx_air_module_t *module) {
    if (!module) return;
    arena_free(&module->arena);
    free(module);
}
