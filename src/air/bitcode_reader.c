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

/* TYPE_BLOCK record codes per llvm/Bitcode/LLVMBitCodes.h. */
enum {
    LAGFX_TYPE_NUMENTRY     = 1,
    LAGFX_TYPE_VOID         = 2,
    LAGFX_TYPE_FLOAT        = 3,
    LAGFX_TYPE_DOUBLE       = 4,
    LAGFX_TYPE_LABEL        = 5,
    LAGFX_TYPE_OPAQUE       = 6,
    LAGFX_TYPE_INTEGER      = 7,
    LAGFX_TYPE_POINTER      = 8,
    LAGFX_TYPE_FUNCTION_OLD = 9,
    LAGFX_TYPE_HALF         = 10,
    LAGFX_TYPE_ARRAY        = 11,
    LAGFX_TYPE_VECTOR       = 12,
    LAGFX_TYPE_METADATA     = 16,
    LAGFX_TYPE_STRUCT_ANON  = 18,
    LAGFX_TYPE_STRUCT_NAME  = 19,
    LAGFX_TYPE_STRUCT_NAMED = 20,
    LAGFX_TYPE_FUNCTION     = 21,
    LAGFX_TYPE_TOKEN        = 22,
    LAGFX_TYPE_BFLOAT       = 23,
};

/* VALUE_SYMTAB_BLOCK record codes per llvm/Bitcode/LLVMBitCodes.h. */
enum {
    LAGFX_VST_CODE_ENTRY     = 1,  /* [valueid, namechar x N] */
    LAGFX_VST_CODE_BBENTRY   = 2,  /* [bbid, namechar x N] — skip for Phase 1 */
    LAGFX_VST_CODE_FNENTRY   = 3,  /* [valueid, body_offset, namechar x N] */
    LAGFX_VST_CODE_COMBINED_ENTRY = 5,
};

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

/* VALUE_SYMTAB_BLOCK entry — maps value_id -> name for functions/globals.
 * Stored in module->symtab_entries[] during MODULE_BLOCK parsing, then
 * linked to functions[] in a post-parse pass. */
typedef struct {
    uint32_t value_id;
    uint32_t body_offset; /* only valid for FNENTRY (code=3) */
    uint32_t name_offset; /* offset into arena, 0 = no name yet */
} lagfx_symtab_entry_t;

struct lagfx_air_module {
    lagfx_arena_t arena;

    /* Copy of the raw bitstream body (after the wrapper magic was
     * stripped). Stored so Phase 2 / Phase 3 can decode FUNCTION_BLOCK
     * bodies on demand without making the caller keep the original
     * blob alive. Offset is into m->arena; length is in BYTES (matching
     * what lagfx_bs_init expects). body_offset on lagfx_air_function_t
     * is a BIT index into this buffer. */
    uint32_t bitstream_arena_offset;
    size_t   bitstream_len_bytes;

    /* Persisted BLOCKINFO_BLOCK contents. ~2 MB; malloc'd out of band
     * rather than placed on the module struct so the struct stays cheap
     * to allocate, and not in the arena because arena_reserve grows the
     * arena's base pointer which would invalidate the embedded abbrev
     * pointers. Phase 2's lagfx_air_function_body_open() needs this so
     * that FUNCTION_BLOCK-targeted DEFINE_ABBREVs from BLOCKINFO are
     * installed when re-entering a function body. */
    lagfx_blockinfo_t *blockinfo;

    /* Optional module-level strings (offsets into arena, 0 = absent). */
    uint32_t triple_offset;
    uint32_t datalayout_offset;
    uint32_t source_filename_offset;

    /* True when the file contains a STRTAB_BLOCK or SYMTAB_BLOCK at the
     * root level. When set, symbol-bearing module records (FUNCTION,
     * GLOBALVAR, ALIAS, IFUNC) prepend two operands [strtab_offset,
     * strtab_size] before the rest of the record schema. Both the
     * bundled triangle and captured macOS metallibs use this layout. */
    bool has_strtab;

    /* STRTAB_BLOCK BLOB contents (raw bytes, no NUL terminators —
     * names are length-delimited via the FUNCTION record's strtab_size
     * operand). Populated when STRTAB_BLOCK is encountered at root.
     * Post-MODULE pass resolves each function's strtab slice into a
     * NUL-terminated arena copy under fn->name_offset. */
    uint32_t strtab_arena_offset;
    size_t   strtab_len_bytes;

    /* Parsed tables (pointers into arena, counts inline). Phase 1
     * populates incrementally; some may be empty for some modules. */
    lagfx_air_type_t            *types;
    uint32_t                     num_types;
    lagfx_air_constant_t        *constants;
    uint32_t                     num_constants;
    lagfx_air_function_t        *functions;
    uint32_t                     num_functions;
    /* Count of module-level GLOBALVAR records. We don't model the
     * globals themselves (Phase 1 ignores their bodies), but LLVM's
     * value enumeration assigns them value-ids BEFORE functions and
     * module constants. Absolute value-id operands in the bitcode
     * (e.g. CST_CODE_AGGREGATE constituents, which are NOT relative-
     * encoded) are numbered in that full space, so consumers must
     * offset their module-value base by this count or every absolute
     * module-constant reference is shifted (off-by-num_globalvars).
     * Paid for by the SkyLight shuffle-mask AGGREGATE bug: a
     * <4 x i32> <0,1,2,undef> mask resolved its constituents to the
     * wrong constants when this was assumed zero. */
    uint32_t                     num_globalvars;
    /* Arena OFFSETS for the three tables above (0 = unset). The table
     * pointers are raw arena addresses and a later arena_reserve() may
     * realloc the arena out from under them (e.g. a CONSTANTS block
     * growing the arena after the function table was cached → stale
     * pointer → use-after-free). We re-derive the pointers from these
     * offsets at each internal mid-parse use and once at finalization
     * (see derive_table_ptrs), mirroring the metadata-strings scheme. */
    uint32_t                     types_off;
    uint32_t                     constants_off;
    uint32_t                     functions_off;
    uint32_t                     param_attr_groups_off;
    lagfx_air_metadata_t        *metadata;
    uint32_t                     num_metadata;
    /* Metadata strings pool. Internally we keep arena OFFSETS for the
     * per-string NUL-terminated byte arrays (offset 0 = absent), since
     * arena_reserve calls may realloc the arena and invalidate raw
     * pointers. After all parsing completes we materialize a parallel
     * const char ** array (metadata_strings) that the public accessor
     * returns; that pointer array is also arena-resident but is built
     * AFTER all other arena_reserves so its pointers stay stable. */
    uint32_t                     metadata_strings_offsets_arena;  /* offset of u32[] */
    uint32_t                     metadata_records_arena;          /* offset of records[] */
    const char                 **metadata_strings;                /* materialized at end */
    uint32_t                     num_metadata_strings;
    lagfx_air_param_attr_group_t*param_attr_groups;
    uint32_t                     num_param_attr_groups;

    /* VALUE_SYMTAB_BLOCK entries. Maps value_id -> name_offset for
     * functions/globals. Phase 1 decodes and stores here, then links
     * to module->functions[] during a post-parse pass. */
    lagfx_symtab_entry_t *symtab_entries;
    uint32_t              num_symtab_entries;
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

/* Re-derive the table pointers from their stored arena offsets. MUST be
 * called after any arena_reserve() that could have realloc'd the arena
 * if a table pointer is about to be dereferenced (the raw pointers go
 * stale on realloc → use-after-free). Offset 0 means "table unset", so
 * the corresponding pointer is left as-is. */
static void derive_table_ptrs(lagfx_air_module_t *m) {
    if (m->types_off) {
        m->types = (lagfx_air_type_t *)(m->arena.base + m->types_off);
        /* Each type's `op` is itself a raw arena pointer — re-derive it
         * from the stored op_offset too (interior staleness). */
        for (uint32_t i = 0; i < m->num_types; i++) {
            m->types[i].op = m->types[i].op_offset
                ? (const uint32_t *)(m->arena.base + m->types[i].op_offset)
                : NULL;
        }
    }
    if (m->constants_off)
        m->constants = (lagfx_air_constant_t *)(m->arena.base + m->constants_off);
    if (m->functions_off)
        m->functions = (lagfx_air_function_t *)(m->arena.base + m->functions_off);
    if (m->param_attr_groups_off) {
        m->param_attr_groups =
            (lagfx_air_param_attr_group_t *)(m->arena.base + m->param_attr_groups_off);
        for (uint32_t i = 0; i < m->num_param_attr_groups; i++) {
            m->param_attr_groups[i].raw = m->param_attr_groups[i].raw_offset
                ? (const uint32_t *)(m->arena.base + m->param_attr_groups[i].raw_offset)
                : NULL;
        }
    }
}

/* Reserve `n` bytes of arena space + align to 4 bytes. Returns the
 * byte offset (always >= 1; 0 reserved for "absent"). Returns 0 on
 * out-of-memory (caller checks). */
static uint32_t arena_reserve(lagfx_arena_t *a, size_t n) {
    /* Align used to 8 bytes. The arena backs structs with pointer
     * members (e.g. lagfx_air_param_attr_group_t::raw), which require
     * 8-byte alignment; a 4-byte alignment let those land on a
     * 4-but-not-8 offset and tripped UBSan ("misaligned address ...
     * requires 8 byte alignment") + SIGSEGV on Linux when a shader with
     * a PARAMATTR_GROUP block was parsed. base is malloc-aligned (>=8),
     * so an 8-multiple offset yields an 8-aligned pointer; u32-array
     * reservations stay 4-aligned (8 is a multiple of 4). */
    size_t aligned = (a->used + 7u) & ~(size_t)7u;
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

uint32_t
lagfx_air_module_num_globalvars(const lagfx_air_module_t *m) {
    return m->num_globalvars;
}

const lagfx_air_metadata_t *
lagfx_air_module_metadata(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_metadata;
    return m->metadata;
}

const char * const *
lagfx_air_module_metadata_strings(const lagfx_air_module_t *m, uint32_t *count) {
    if (count) *count = m->num_metadata_strings;
    return m->metadata_strings;
}

const char *
lagfx_air_module_metadata_string_by_id(const lagfx_air_module_t *m, uint32_t id) {
    if (!m || id >= m->num_metadata_strings || !m->metadata_strings) return NULL;
    return m->metadata_strings[id];
}

const lagfx_air_metadata_t *
lagfx_air_module_named_metadata(const lagfx_air_module_t *m, const char *name) {
    if (!m || !name || !m->metadata) return NULL;
    for (uint32_t i = 0; i < m->num_metadata; i++) {
        const lagfx_air_metadata_t *md = &m->metadata[i];
        if (md->kind != LAGFX_AIR_MD_NAMED_NODE || md->name_offset == 0u) continue;
        const char *md_name = (const char *)(m->arena.base + md->name_offset);
        if (strcmp(md_name, name) == 0) return md;
    }
    return NULL;
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

    /* Step 2a: copy the bitstream body into the arena so Phase 2 / Phase 3
     * decoders can use stashed body_offset/body_length values without
     * the caller keeping `blob` alive. */
    {
        uint32_t bs_off = arena_reserve(&m->arena, body_len);
        if (bs_off == 0u && body_len > 0u) {
            lagfx_air_module_free(m);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        memcpy(m->arena.base + bs_off, blob + body_off, body_len);
        m->bitstream_arena_offset = bs_off;
        m->bitstream_len_bytes = body_len;
    }

    /* Step 3: BLOCKINFO is the first thing in modules with shared
     * abbrev tables. We need to parse it BEFORE entering MODULE so we
     * can install per-target-block-id abbrevs. The blockinfo struct is
     * ~2 MB (32 block-IDs × 64 abbrevs × 64-op tables) so it's heap
     * rather than stack, and persisted on the module so Phase 2 body
     * decoders can also install BLOCKINFO-sourced abbrevs. */
    m->blockinfo = (lagfx_blockinfo_t *)malloc(sizeof(*m->blockinfo));
    if (!m->blockinfo) {
        lagfx_air_module_free(m);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    lagfx_blockinfo_init(m->blockinfo);

    /* Step 3a: STRTAB schema selection.
     *
     * Symbol-bearing module records (FUNCTION, GLOBALVAR, ALIAS, IFUNC)
     * use one of two operand layouts:
     *
     *   STRTAB-bearing (newer):  [strtab_off, strtab_size, type, cc,
     *                             isProto, linkage, paramattr, ...]
     *   Legacy:                  [type, cc, isProto, linkage,
     *                             paramattr, ...]
     *
     * The triangle and every captured macOS metallib in
     * `scratch/captured-metallibs-2026-05-19/` use the STRTAB-bearing
     * layout (verified by manual decode of FUNCTION records: strtab_size
     * always matches the function name length, and isProto sits at the
     * expected schema slot). We default to that schema.
     *
     * A root-level STRTAB_BLOCK/SYMTAB_BLOCK pre-scan was attempted
     * (commit ${PR}) but Apple's MODULE_BLOCK size accounting confuses
     * the lightweight pre-pass: triangle was detected fine, but
     * captured-macOS files have MODULE_BLOCK extending past its
     * self-reported block_size. Rather than ship a half-working
     * detector, we hard-code the schema and revisit if we ever ingest
     * legacy-layout AIR bitcode (none observed in 2 weeks of capture). */
    m->has_strtab = true;
    LAGFX_TRACE("air_bitcode_reader: assuming STRTAB-bearing FUNCTION schema");

    /* Step 4: parse MODULE_BLOCK + its records.
     * Bitstream starts immediately after the 4-byte magic. The first
     * abbrev code (at width 2, the root width) should be
     * ENTER_SUBBLOCK introducing MODULE_BLOCK. */
    lagfx_bitstream_t bs;
    lagfx_bs_init(&bs, blob + body_off, body_len);

    /* Root abbrev width is 2 bits per LLVM Bitstream spec. */
    const uint32_t kRootAbbrevWidth = 2u;

   /* Enter top-level block. Could be IDENTIFICATION_BLOCK first then
     * MODULE_BLOCK — Apple's metallib bitcode starts with IDENT then
     * MODULE. We scan blocks at the root level until we hit MODULE or STRTB. */
    while (!lagfx_bs_at_end(&bs)) {
        bool err = false;
        size_t pos_before_code = lagfx_bs_pos(&bs);
        uint32_t code = lagfx_bs_read_bits(&bs, kRootAbbrevWidth, &err);
        if (err) break;
        if (code != LAGFX_ABBREV_ENTER_SUBBLOCK) {
            /* Root level should only have ENTER_SUBBLOCK at top. Captured
             * macOS metallibs do something funny past MODULE_BLOCK's
             * declared end (related to the OPERAND_BUNDLE_TAGS
             * Apple-flavored abbrev issue — Phase 2.4). Treat as
             * end-of-stream rather than a hard error so the caller still
             * gets the partial module. */
            LAGFX_LOG("air_bitcode_reader: unexpected root abbrev code %u at bit %zu — treating as end-of-stream",
                      code, pos_before_code);
            break;
        }
        /* Seek back to re-enter via the helper. */
        if (!lagfx_bs_seek(&bs, pos_before_code)) {
            lagfx_air_module_free(m);
            return LAGFX_ERR_PROTOCOL;
        }
        lagfx_block_t blk;
        if (!lagfx_block_enter(&bs, kRootAbbrevWidth, NULL, &blk)) {
            LAGFX_ERR("air_bitcode_reader: failed to enter root sub-block");
            lagfx_air_module_free(m);
            return LAGFX_ERR_PROTOCOL;
        }

        /* Parse STRTAB_BLOCK at root level. Stashes the BLOB bytes in
         * the arena so the post-MODULE name-resolution pass can intern
         * NUL-terminated copies for each function's
         * (strtab_offset, strtab_size) pair into the function struct's
         * name_offset.
         *
         * STRTAB_BLOCK has a single STRTAB_BLOB record (code=1) whose
         * payload is the BLOB-encoded byte array. lagfx_block_next_record
         * surfaces BLOBs via rec.blob_data + rec.blob_len when the
         * decoded abbrev includes a BLOB operand. */
        if (blk.block_id == LAGFX_BLK_STRTAB) {
            /* The root walker already called lagfx_block_enter for blk;
             * we iterate STRTAB's records using blk's own context. */
            uint64_t st_scratch[LAGFX_RECORD_MAX_OPS];
            while (lagfx_bs_pos(&bs) < blk.end_pos) {
                lagfx_record_t srec = {0};
                bool s_end = false, s_sub = false, s_da = false;
                uint32_t s_sub_id = 0;
                if (!lagfx_block_next_record(&blk, st_scratch, &srec,
                                              &s_end, &s_sub, &s_da, &s_sub_id)) break;
                if (s_end) break;
                if (s_sub) { lagfx_block_skip(&bs, blk.abbrev_width); continue; }
                if (s_da) continue;
                /* The STRTAB_BLOB record (code 1) carries the whole table
                 * as a BLOB. lagfx_block_next_record surfaces BLOBs via
                 * srec.blob_data + srec.blob_len. Stash the bytes in the
                 * arena and remember the offset for the post-pass that
                 * resolves each function's strtab slice into a NUL-
                 * terminated arena copy. */
                if (srec.blob_data && srec.blob_len > 0u) {
                    uint32_t off = arena_reserve(&m->arena, srec.blob_len);
                    if (off == 0u) {
                        lagfx_air_module_free(m);
                        return LAGFX_ERR_OUT_OF_MEMORY;
                    }
                    memcpy(m->arena.base + off, srec.blob_data, srec.blob_len);
                    m->strtab_arena_offset = off;
                    m->strtab_len_bytes    = srec.blob_len;
                    LAGFX_TRACE("air_bitcode_reader: STRTAB_BLOCK BLOB %u bytes @ arena+%u",
                                (unsigned)srec.blob_len, off);
                }
            }
            (void)lagfx_bs_seek(&bs, blk.end_pos);
            continue;
        }

        if (blk.block_id == LAGFX_BLK_MODULE) {
            /* Parse MODULE_BLOCK records + sub-blocks. */
            uint64_t scratch_ops[LAGFX_RECORD_MAX_OPS];
            /* Cursor into m->functions[] for body-offset assignment as
             * FUNCTION_BLOCK sub-blocks are encountered. LLVM emits one
             * FUNCTION_BLOCK per non-prototype function in
             * declaration order; we walk both lists in lock-step. */
            uint32_t next_body_fn = 0u;
            /* Did the MODULE walker exit cleanly (END_BLOCK seen OR cursor
             * reached blk.end_pos)? If not, the inner-record reader bailed
             * on something we don't yet decode (Apple-flavored abbrevs in
             * captured macOS metallibs — Phase 2.4), and we should stop
             * root-level parsing right after — there's nothing meaningful
             * past the truncation point. */
            bool module_clean_exit = false;
            while (lagfx_bs_pos(&bs) < blk.end_pos) {
                lagfx_record_t rec = {0};
                bool is_end = false, is_subblock = false, is_define_abbrev = false;
                uint32_t sub_id = 0;
                if (!lagfx_block_next_record(&blk, scratch_ops, &rec,
                                              &is_end, &is_subblock,
                                              &is_define_abbrev, &sub_id)) {
                    LAGFX_LOG("air_bitcode_reader: failed to read MODULE record at bit %zu — stopping module parse",
                              lagfx_bs_pos(&bs));
                    break;
                }
                if (is_end) { module_clean_exit = true; break; }
                if (is_subblock) {
                    LAGFX_TRACE("MODULE walker: sub-block id=%u at bit %zu (MODULE end %zu)",
                                sub_id, lagfx_bs_pos(&bs), blk.end_pos);
                    if (sub_id == LAGFX_BLK_BLOCKINFO) {
                        /* Parse BLOCKINFO: SETBID records partition the
                         * following DEFINE_ABBREVs by target block ID. */
                        lagfx_block_t bi;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, NULL, &bi)) {
                            LAGFX_ERR("air_bitcode_reader: failed to enter BLOCKINFO");
                            break;
                        }
                        uint32_t target_block_id = 0xFFFFFFFFu;
                        while (lagfx_bs_pos(&bs) < bi.end_pos) {
                            lagfx_record_t birec = {0};
                            bool bi_end = false, bi_sub = false, bi_da = false;
                            uint32_t bi_sub_id = 0;
                            if (!lagfx_block_next_record(&bi, scratch_ops, &birec,
                                                          &bi_end, &bi_sub, &bi_da,
                                                          &bi_sub_id)) {
                                break;
                            }
                            if (bi_end) break;
                            if (bi_sub) {
                                /* BLOCKINFO shouldn't have sub-blocks. */
                                lagfx_block_skip(&bs, bi.abbrev_width);
                                continue;
                            }
                            if (bi_da) {
                                /* DEFINE_ABBREV inside BLOCKINFO: the
                                 * abbrev got appended to `bi.abbrevs`.
                                 * Move it into blockinfo.per_block[target]. */
                                if (target_block_id < LAGFX_BLOCKINFO_MAX_BLOCK_IDS &&
                                    bi.abbrevs.num_entries > 0u) {
                                    lagfx_abbrev_table_t *dst =
                                        &m->blockinfo->per_block[target_block_id];
                                    if (dst->num_entries < LAGFX_ABBREV_MAX_PER_BLOCK) {
                                        const lagfx_abbrev_t *src_ab =
                                            &bi.abbrevs.entries[bi.abbrevs.num_entries - 1u];
                                        dst->entries[dst->num_entries] = *src_ab;
                                        /* Debug-dump the abbrev pattern at trace level
                                         * so Phase 2.4 OBT RE can compare triangle vs
                                         * viewport per-block-id abbrev installs. */
                                        char pat[256]; size_t off = 0;
                                        for (uint32_t pi = 0; pi < src_ab->num_ops && off < sizeof(pat) - 16; pi++) {
                                            const char *k = "?";
                                            switch (src_ab->ops[pi].kind) {
                                                case LAGFX_ABBREV_OP_LITERAL: k = "LIT"; break;
                                                case LAGFX_ABBREV_OP_FIXED:   k = "FIX"; break;
                                                case LAGFX_ABBREV_OP_VBR:     k = "VBR"; break;
                                                case LAGFX_ABBREV_OP_ARRAY:   k = "ARR"; break;
                                                case LAGFX_ABBREV_OP_CHAR6:   k = "CH6"; break;
                                                case LAGFX_ABBREV_OP_BLOB:    k = "BLB"; break;
                                            }
                                            int n = snprintf(pat + off, sizeof(pat) - off,
                                                             "%s%s(%llu)", pi ? "," : "", k,
                                                             (unsigned long long)src_ab->ops[pi].value_or_width);
                                            if (n < 0) break;
                                            off += (size_t)n;
                                        }
                                        LAGFX_TRACE("BLOCKINFO: target_block=%u abbrev_id=%u (slot=%u) ops=[%s]",
                                                    target_block_id,
                                                    4u + dst->num_entries,
                                                    dst->num_entries, pat);
                                        dst->num_entries++;
                                    }
                                }
                                continue;
                            }
                            /* SETBID record: code 1, op0 = target block id. */
                            if (birec.code == 1u && birec.num_ops >= 1u) {
                                target_block_id = (uint32_t)birec.ops[0];
                            }
                            /* BLOCKNAME (2) / SETRECORDNAME (3): debug
                             * info, ignored. */
                        }
                        continue;
                    }
                    if (sub_id == LAGFX_BLK_CONSTANTS) {
                        /* CONSTANTS_BLOCK: SETTYPE records change the
                         * 'current type', then constant records (INTEGER,
                         * FLOAT, NULL, AGGREGATE, STRING, DATA, ...) emit
                         * constants of that type. Phase 1 collects basic
                         * constants; complex (AGGREGATE, DATA) constants
                         * collected as UNKNOWN with raw bytes for now. */
                        lagfx_block_t cb;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &cb)) break;
                        const uint32_t MAX_CONSTS = 4096u;
                        uint32_t consts_off = arena_reserve(&m->arena,
                                                              sizeof(lagfx_air_constant_t) * MAX_CONSTS);
                        if (consts_off == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        lagfx_air_constant_t *consts = (lagfx_air_constant_t *)(m->arena.base + consts_off);
                        uint32_t num_consts = 0u;
                        uint32_t current_type = 0u;

                        while (lagfx_bs_pos(&bs) < cb.end_pos) {
                            lagfx_record_t crec = {0};
                            bool c_end = false, c_sub = false, c_da = false;
                            uint32_t c_sub_id = 0;
                            if (!lagfx_block_next_record(&cb, scratch_ops, &crec,
                                                          &c_end, &c_sub, &c_da, &c_sub_id)) break;
                            if (c_end) break;
                            if (c_sub) { lagfx_block_skip(&bs, cb.abbrev_width); continue; }
                            if (c_da) continue;
                            if (num_consts >= MAX_CONSTS) break;

                            /* CST_CODE_SETTYPE (1): switches current type. */
                            if (crec.code == 1u) {
                                if (crec.num_ops >= 1u) current_type = (uint32_t)crec.ops[0];
                                continue;
                            }
                            lagfx_air_constant_t *c = &consts[num_consts];
                            c->type_index = current_type;
                            c->payload.bytes.offset = 0;
                            c->payload.bytes.len = 0;
                            switch (crec.code) {
                                case 2u:  /* CST_CODE_NULL */
                                    c->kind = LAGFX_AIR_CONST_NULL; break;
                                case 3u:  /* CST_CODE_UNDEF */
                                    c->kind = LAGFX_AIR_CONST_UNDEF; break;
                                case 4u:  /* CST_CODE_INTEGER */
                                    c->kind = LAGFX_AIR_CONST_INTEGER;
                                    /* Signed VBR: low bit = sign, remaining bits = magnitude. */
                                    if (crec.num_ops >= 1) {
                                        uint64_t raw = crec.ops[0];
                                        int64_t v = (raw & 1) ? -(int64_t)(raw >> 1) : (int64_t)(raw >> 1);
                                        c->payload.i64 = v;
                                    }
                                    break;
                                case 6u:  /* CST_CODE_FLOAT */
                                    c->kind = LAGFX_AIR_CONST_FLOAT;
                                    if (crec.num_ops >= 1) {
                                        union { uint64_t u; double f; } cv;
                                        cv.u = crec.ops[0];
                                        c->payload.f64 = cv.f;
                                    }
                                    break;
                                case 8u:  /* CST_CODE_STRING */
                                case 9u:  /* CST_CODE_CSTRING */
                                {
                                    c->kind = LAGFX_AIR_CONST_STRING;
                                    /* One char per op. */
                                    uint32_t off = arena_reserve(&m->arena, crec.num_ops + 1u);
                                    if (off == 0u) { lagfx_air_module_free(m); return LAGFX_ERR_OUT_OF_MEMORY; }
                                    consts = (lagfx_air_constant_t *)(m->arena.base + consts_off);
                                    c = &consts[num_consts];
                                    for (uint32_t i = 0; i < crec.num_ops; i++) {
                                        m->arena.base[off + i] = (uint8_t)(crec.ops[i] & 0xFFu);
                                    }
                                    m->arena.base[off + crec.num_ops] = 0;
                                    c->payload.bytes.offset = off;
                                    c->payload.bytes.len = crec.num_ops;
                                    break;
                                }
                                case 22u: /* CST_CODE_DATA */
                                    c->kind = LAGFX_AIR_CONST_DATA;
                                    /* Stash op-array via arena. */
                                    {
                                        uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                        for (uint32_t i = 0; i < crec.num_ops; i++) u32_buf[i] = (uint32_t)crec.ops[i];
                                        uint32_t off = arena_intern_u32_array(&m->arena, u32_buf, crec.num_ops);
                                        consts = (lagfx_air_constant_t *)(m->arena.base + consts_off);
                                        c = &consts[num_consts];
                                        c->payload.bytes.offset = off;
                                        c->payload.bytes.len = crec.num_ops * sizeof(uint32_t);
                                    }
                                    break;
                                case 7u:  /* CST_CODE_AGGREGATE */
                                    c->kind = LAGFX_AIR_CONST_AGGREGATE;
                                    {
                                        uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                        for (uint32_t i = 0; i < crec.num_ops; i++) u32_buf[i] = (uint32_t)crec.ops[i];
                                        uint32_t off = arena_intern_u32_array(&m->arena, u32_buf, crec.num_ops);
                                        consts = (lagfx_air_constant_t *)(m->arena.base + consts_off);
                                        c = &consts[num_consts];
                                        c->payload.bytes.offset = off;
                                        c->payload.bytes.len = crec.num_ops * sizeof(uint32_t);
                                    }
                                    break;
                                default:
                                    c->kind = LAGFX_AIR_CONST_UNKNOWN;
                                    break;
                            }
                            num_consts++;
                        }
                        m->constants = consts;
                        m->constants_off = consts_off;
                        m->num_constants = num_consts;
                        LAGFX_TRACE("air_bitcode_reader: CONSTANTS_BLOCK decoded %u constants", num_consts);
                        (void)lagfx_bs_seek(&bs, cb.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_METADATA_KIND ||
                        sub_id == LAGFX_BLK_METADATA_ATTACHMENT) {
                        /* METADATA_KIND_BLOCK + METADATA_ATTACHMENT: walk
                         * past for now. METADATA_KIND maps numeric kind
                         * IDs to names ('dbg', 'tbaa', etc.) that our
                         * translator doesn't yet need; METADATA_ATTACHMENT
                         * binds metadata to instructions (debug info
                         * mostly) which is also not on the Phase 4
                         * critical path. */
                        lagfx_block_t mb;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &mb)) break;
                        while (lagfx_bs_pos(&bs) < mb.end_pos) {
                            lagfx_record_t mrec = {0};
                            bool m_end = false, m_sub = false, m_da = false;
                            uint32_t m_sub_id = 0;
                            if (!lagfx_block_next_record(&mb, scratch_ops, &mrec,
                                                          &m_end, &m_sub, &m_da, &m_sub_id)) break;
                            if (m_end) break;
                            if (m_sub) { lagfx_block_skip(&bs, mb.abbrev_width); continue; }
                            if (m_da) continue;
                        }
                        (void)lagfx_bs_seek(&bs, mb.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_METADATA) {
                        /* METADATA_BLOCK walker. Decodes the records we
                         * need for the AIR semantic model:
                         *   - METADATA_STRINGS (35): packed-VBR6 string
                         *     pool. Each batch contributes a contiguous
                         *     range of LLVM metadata IDs.
                         *   - METADATA_STRING_OLD (1): legacy single
                         *     string-as-char-array record.
                         *   - METADATA_NAME (4): char array; names the
                         *     NEXT NAMED_NODE record.
                         *   - METADATA_VALUE (2): [type_idx, value_id].
                         *   - METADATA_NODE (3) / DISTINCT_NODE (5) /
                         *     OLD_NODE (8): tuple of metadata-IDs.
                         *   - METADATA_NAMED_NODE (10): named tuple;
                         *     takes its name from the preceding NAME.
                         *   - METADATA_INDEX_OFFSET (38) /
                         *     METADATA_INDEX (39): used by LLVM's lazy
                         *     metadata loader; no semantic content,
                         *     skipped.
                         * Any other code lands in metadata[i].kind=UNKNOWN
                         * with raw operands preserved.
                         *
                         * The STRINGS BLOB layout (paid for 2026-05-20
                         * after the failed metadata_block_walker freshman
                         * dispatch — see paravirt-re/library/diag/
                         * metadata_strings_decoder.py for the pinned
                         * encoding):
                         *
                         *   BLOB[0 : strings_offset]   = packed VBR6
                         *                                size table
                         *                                (one chunk per
                         *                                string, LSB-first
                         *                                bit-stream).
                         *   BLOB[strings_offset:]      = concatenated raw
                         *                                string bytes,
                         *                                NO NUL
                         *                                terminators —
                         *                                lengths come
                         *                                from the size
                         *                                table.
                         *
                         * Don't be tempted to read the BLOB as
                         * NUL-terminated bytes; that's the trap that
                         * killed the 2026-05-20 freshman dispatch
                         * (yields 0 strings on triangle).
                         */
                        lagfx_block_t mb;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &mb)) break;

                        const uint32_t MAX_MD_STRINGS = 4096u;
                        const uint32_t MAX_MD_RECORDS = 4096u;

                        uint32_t str_offs_arena = arena_reserve(&m->arena,
                                                     sizeof(uint32_t) * MAX_MD_STRINGS);
                        if (str_offs_arena == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        uint32_t md_arena = arena_reserve(&m->arena,
                                                 sizeof(lagfx_air_metadata_t) * MAX_MD_RECORDS);
                        if (md_arena == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        uint32_t md_num_strings = 0u;
                        uint32_t md_num_records = 0u;
                        uint32_t pending_name_off = 0u;

                        while (lagfx_bs_pos(&bs) < mb.end_pos) {
                            lagfx_record_t mrec = {0};
                            bool m_end = false, m_sub = false, m_da = false;
                            uint32_t m_sub_id = 0;
                            if (!lagfx_block_next_record(&mb, scratch_ops, &mrec,
                                                          &m_end, &m_sub, &m_da, &m_sub_id)) break;
                            if (m_end) break;
                            if (m_sub) { lagfx_block_skip(&bs, mb.abbrev_width); continue; }
                            if (m_da) continue;

                            switch (mrec.code) {
                                case 35u: {  /* METADATA_STRINGS */
                                    if (mrec.num_ops < 2u || mrec.blob_data == NULL ||
                                        mrec.blob_len == 0u) {
                                        LAGFX_WARN("METADATA_STRINGS: malformed (num_ops=%u blob=%p len=%u)",
                                                   mrec.num_ops, (const void *)mrec.blob_data,
                                                   (unsigned)mrec.blob_len);
                                        break;
                                    }
                                    uint32_t batch_num = (uint32_t)mrec.ops[0];
                                    uint32_t size_table_bytes = (uint32_t)mrec.ops[1];
                                    if (size_table_bytes > mrec.blob_len) {
                                        LAGFX_WARN("METADATA_STRINGS: size_table_bytes=%u > blob_len=%u",
                                                   size_table_bytes, (unsigned)mrec.blob_len);
                                        break;
                                    }
                                    /* Copy the BLOB out of the bitstream
                                     * buffer before arena_reserve calls
                                     * potentially realloc the arena (the
                                     * bitstream lives in the arena, so
                                     * blob_data would dangle). */
                                    uint8_t *blob_copy = (uint8_t *)malloc(mrec.blob_len);
                                    if (!blob_copy) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    memcpy(blob_copy, mrec.blob_data, mrec.blob_len);

                                    const uint8_t *st = blob_copy;
                                    const uint8_t *sd = blob_copy + size_table_bytes;
                                    uint32_t sd_remaining = (uint32_t)mrec.blob_len - size_table_bytes;
                                    size_t bit_pos = 0u;
                                    size_t st_total_bits = (size_t)size_table_bytes * 8u;
                                    bool decode_ok = true;

                                    for (uint32_t i = 0; i < batch_num; i++) {
                                        if (md_num_strings >= MAX_MD_STRINGS) {
                                            LAGFX_WARN("METADATA_STRINGS: hit MAX_MD_STRINGS cap %u",
                                                       MAX_MD_STRINGS);
                                            decode_ok = false;
                                            break;
                                        }
                                        /* VBR6: low 5 bits data, bit 5 = continuation. */
                                        uint32_t v = 0u;
                                        uint32_t shift = 0u;
                                        bool overflow = false;
                                        for (;;) {
                                            if (bit_pos + 6u > st_total_bits) {
                                                overflow = true;
                                                break;
                                            }
                                            uint32_t chunk = 0u;
                                            for (uint32_t bi = 0; bi < 6u; bi++) {
                                                uint32_t bit = (uint32_t)((st[(bit_pos + bi) / 8u] >>
                                                                            ((bit_pos + bi) % 8u)) & 1u);
                                                chunk |= bit << bi;
                                            }
                                            v |= (chunk & 0x1Fu) << shift;
                                            shift += 5u;
                                            bit_pos += 6u;
                                            if ((chunk & 0x20u) == 0u) break;
                                        }
                                        if (overflow) {
                                            LAGFX_WARN("METADATA_STRINGS: size-table truncated at string %u/%u",
                                                       i, batch_num);
                                            decode_ok = false;
                                            break;
                                        }
                                        uint32_t slen = v;
                                        if (slen > sd_remaining) {
                                            LAGFX_WARN("METADATA_STRINGS: string %u overruns data: len=%u remaining=%u",
                                                       i, slen, sd_remaining);
                                            decode_ok = false;
                                            break;
                                        }
                                        uint32_t off = arena_reserve(&m->arena, (size_t)slen + 1u);
                                        if (off == 0u) {
                                            free(blob_copy);
                                            lagfx_air_module_free(m);
                                            return LAGFX_ERR_OUT_OF_MEMORY;
                                        }
                                        memcpy(m->arena.base + off, sd, slen);
                                        m->arena.base[off + slen] = 0;
                                        /* Refresh str-offsets array pointer after potential realloc. */
                                        uint32_t *str_offs = (uint32_t *)(m->arena.base + str_offs_arena);
                                        str_offs[md_num_strings++] = off;
                                        sd += slen;
                                        sd_remaining -= slen;
                                    }
                                    free(blob_copy);
                                    LAGFX_TRACE("METADATA_STRINGS: +%u strings (total %u)%s",
                                                batch_num, md_num_strings,
                                                decode_ok ? "" : " [truncated]");
                                    break;
                                }

                                case 1u: {  /* METADATA_STRING_OLD */
                                    if (md_num_strings >= MAX_MD_STRINGS) break;
                                    uint32_t slen = mrec.num_ops;
                                    uint32_t off = arena_reserve(&m->arena, (size_t)slen + 1u);
                                    if (off == 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    for (uint32_t i = 0; i < slen; i++) {
                                        m->arena.base[off + i] = (uint8_t)(mrec.ops[i] & 0xFFu);
                                    }
                                    m->arena.base[off + slen] = 0;
                                    uint32_t *str_offs = (uint32_t *)(m->arena.base + str_offs_arena);
                                    str_offs[md_num_strings++] = off;
                                    break;
                                }

                                case 4u: {  /* METADATA_NAME (char-array; names next NAMED_NODE) */
                                    uint32_t slen = mrec.num_ops;
                                    uint32_t off = arena_reserve(&m->arena, (size_t)slen + 1u);
                                    if (off == 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    for (uint32_t i = 0; i < slen; i++) {
                                        m->arena.base[off + i] = (uint8_t)(mrec.ops[i] & 0xFFu);
                                    }
                                    m->arena.base[off + slen] = 0;
                                    pending_name_off = off;
                                    break;
                                }

                                case 2u:    /* METADATA_VALUE */
                                case 3u:    /* METADATA_NODE */
                                case 5u:    /* METADATA_DISTINCT_NODE */
                                case 8u:    /* METADATA_OLD_NODE (legacy) */
                                case 10u: { /* METADATA_NAMED_NODE */
                                    if (md_num_records >= MAX_MD_RECORDS) {
                                        LAGFX_WARN("METADATA: hit MAX_MD_RECORDS cap %u", MAX_MD_RECORDS);
                                        break;
                                    }
                                    uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                    for (uint32_t i = 0; i < mrec.num_ops; i++) {
                                        u32_buf[i] = (uint32_t)mrec.ops[i];
                                    }
                                    uint32_t op_off = (mrec.num_ops == 0u)
                                        ? 0u
                                        : arena_intern_u32_array(&m->arena, u32_buf, mrec.num_ops);
                                    if (op_off == 0u && mrec.num_ops > 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    /* Refresh records array pointer after potential realloc. */
                                    lagfx_air_metadata_t *md_arr =
                                        (lagfx_air_metadata_t *)(m->arena.base + md_arena);
                                    lagfx_air_metadata_t *md = &md_arr[md_num_records++];
                                    md->name_offset = 0u;
                                    md->operands = (op_off == 0u)
                                        ? NULL
                                        : (const uint32_t *)(m->arena.base + op_off);
                                    md->num_operands = mrec.num_ops;
                                    if (mrec.code == 2u) {
                                        md->kind = LAGFX_AIR_MD_VALUE;
                                    } else if (mrec.code == 10u) {
                                        md->kind = LAGFX_AIR_MD_NAMED_NODE;
                                        md->name_offset = pending_name_off;
                                        pending_name_off = 0u;
                                    } else {
                                        md->kind = LAGFX_AIR_MD_NODE;
                                    }
                                    break;
                                }

                                case 38u:   /* METADATA_INDEX_OFFSET */
                                case 39u:   /* METADATA_INDEX */
                                    /* LLVM lazy-loader bookkeeping; no
                                     * semantic content for our translator. */
                                    break;

                                default: {
                                    /* Unknown record code — preserve raw
                                     * operands under kind=UNKNOWN so
                                     * callers can introspect rather than
                                     * silently dropping. Captured-macOS
                                     * metallibs may use Apple-specific
                                     * codes we haven't mapped yet; the
                                     * UNKNOWN bucket keeps record-id
                                     * accounting honest. */
                                    if (md_num_records >= MAX_MD_RECORDS) break;
                                    uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                    for (uint32_t i = 0; i < mrec.num_ops; i++) {
                                        u32_buf[i] = (uint32_t)mrec.ops[i];
                                    }
                                    uint32_t op_off = (mrec.num_ops == 0u)
                                        ? 0u
                                        : arena_intern_u32_array(&m->arena, u32_buf, mrec.num_ops);
                                    if (op_off == 0u && mrec.num_ops > 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    lagfx_air_metadata_t *md_arr =
                                        (lagfx_air_metadata_t *)(m->arena.base + md_arena);
                                    lagfx_air_metadata_t *md = &md_arr[md_num_records++];
                                    md->kind = LAGFX_AIR_MD_UNKNOWN;
                                    md->name_offset = 0u;
                                    md->operands = (op_off == 0u)
                                        ? NULL
                                        : (const uint32_t *)(m->arena.base + op_off);
                                    md->num_operands = mrec.num_ops;
                                    LAGFX_TRACE("METADATA: unmapped record code %u (num_ops=%u)",
                                                mrec.code, mrec.num_ops);
                                    break;
                                }
                            }
                        }

                        m->metadata_strings_offsets_arena = str_offs_arena;
                        m->metadata_records_arena         = md_arena;
                        m->num_metadata_strings = md_num_strings;
                        m->num_metadata         = md_num_records;
                        /* m->metadata pointer is materialized at the end
                         * of module_open (after STRTAB pass) so it picks
                         * up the final arena base. metadata_strings
                         * (const char **) is materialized there too. */
                        LAGFX_TRACE("METADATA_BLOCK decoded: %u strings, %u records",
                                    md_num_strings, md_num_records);
                        (void)lagfx_bs_seek(&bs, mb.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_PARAMATTR_GROUP) {
                        /* PARAMATTR_GROUP: each ENTRY record has a
                         * group_id (op0) followed by attribute pairs.
                         * Phase 1 stashes raw operand vectors; semantic
                         * decoding deferred. */
                        lagfx_block_t pg;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &pg)) break;
                        const uint32_t MAX_GROUPS = 64u;
                        uint32_t groups_off = arena_reserve(&m->arena,
                                                              sizeof(lagfx_air_param_attr_group_t) * MAX_GROUPS);
                        if (groups_off == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        lagfx_air_param_attr_group_t *groups =
                            (lagfx_air_param_attr_group_t *)(m->arena.base + groups_off);
                        uint32_t num_groups = 0u;
                        while (lagfx_bs_pos(&bs) < pg.end_pos) {
                            lagfx_record_t prec = {0};
                            bool p_end = false, p_sub = false, p_da = false;
                            uint32_t p_sub_id = 0;
                            if (!lagfx_block_next_record(&pg, scratch_ops, &prec,
                                                          &p_end, &p_sub, &p_da, &p_sub_id)) break;
                            if (p_end) break;
                            if (p_sub) { lagfx_block_skip(&bs, pg.abbrev_width); continue; }
                            if (p_da) continue;
                            if (num_groups >= MAX_GROUPS) break;

                            uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                            for (uint32_t i = 0; i < prec.num_ops; i++) {
                                u32_buf[i] = (uint32_t)prec.ops[i];
                            }
                            uint32_t off = arena_intern_u32_array(&m->arena, u32_buf, prec.num_ops);
                            groups = (lagfx_air_param_attr_group_t *)(m->arena.base + groups_off);
                            groups[num_groups].group_id = (prec.num_ops > 0) ? (uint32_t)prec.ops[0] : 0u;
                            groups[num_groups].raw = (off == 0u) ? NULL : (const uint32_t *)(m->arena.base + off);
                            groups[num_groups].raw_offset = off;
                            groups[num_groups].num_raw = prec.num_ops;
                            num_groups++;
                        }
                        m->param_attr_groups = groups;
                        m->param_attr_groups_off = groups_off;
                        m->num_param_attr_groups = num_groups;
                        LAGFX_TRACE("air_bitcode_reader: PARAMATTR_GROUP decoded %u groups", num_groups);
                        (void)lagfx_bs_seek(&bs, pg.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_PARAMATTR) {
                        /* PARAMATTR: each record is an array of group
                         * IDs (referenced by FUNCTION records via the
                         * paramattr_index field). Phase 1 just walks. */
                        lagfx_block_t pa;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &pa)) break;
                        while (lagfx_bs_pos(&bs) < pa.end_pos) {
                            lagfx_record_t prec = {0};
                            bool p_end = false, p_sub = false, p_da = false;
                            uint32_t p_sub_id = 0;
                            if (!lagfx_block_next_record(&pa, scratch_ops, &prec,
                                                          &p_end, &p_sub, &p_da, &p_sub_id)) break;
                            if (p_end) break;
                            if (p_sub) { lagfx_block_skip(&bs, pa.abbrev_width); continue; }
                            if (p_da) continue;
                            /* Phase 1: just walk. */
                        }
                        (void)lagfx_bs_seek(&bs, pa.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_OPERAND_BUNDLE_TAGS) {
                        /* OPERAND_BUNDLE_TAGS contains the standard 8
                         * LLVM bundle tag names ('deopt', 'funclet',
                         * 'gc-transition', etc.). Both fixtures we have
                         * use the same 8 tags in the same order, BUT
                         * Apple's runtime AIR emitter encodes some of
                         * the later records with a per-record bit-
                         * difference that defeats both our generic vbr6
                         * reader AND llvm-bcanalyzer (Phase 2.4 pickup
                         * doc traces the divergence to bit 11 of op 2
                         * in record 3 — see phase2_step4_pickup).
                         *
                         * Phase 2 doesn't need the OBT contents — we
                         * already know the standard tags by name. Skip
                         * the block entirely so the MODULE walker
                         * reaches FUNCTION_BLOCK on captured macOS
                         * metallibs. When OBT decoding is fully RE'd,
                         * swap this back to a record-walking handler. */
                        if (!lagfx_block_skip(&bs, blk.abbrev_width)) {
                            LAGFX_ERR("air_bitcode_reader: failed to skip OPERAND_BUNDLE_TAGS");
                            break;
                        }
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_TYPE) {
                        /* Decode TYPE_BLOCK into m->types[]. */
                        lagfx_block_t tb;
                        if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &tb)) {
                            LAGFX_ERR("air_bitcode_reader: failed to enter TYPE_BLOCK");
                            break;
                        }
                        /* Reserve a working buffer for types — sized by
                         * NUMENTRY record if it comes first, otherwise
                         * grow as needed. For simplicity, hard-cap at
                         * 4096; real metallibs are ~20-30 types. */
                        const uint32_t MAX_TYPES = 4096u;
                        uint32_t types_off = arena_reserve(&m->arena,
                                                            sizeof(lagfx_air_type_t) * MAX_TYPES);
                        if (types_off == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        lagfx_air_type_t *types = (lagfx_air_type_t *)(m->arena.base + types_off);
                        uint32_t num_types = 0u;
                        /* Track pending STRUCT_NAME → STRUCT_NAMED association. */
                        uint32_t pending_name_offset = 0u;

                        while (lagfx_bs_pos(&bs) < tb.end_pos) {
                            lagfx_record_t trec = {0};
                            bool t_end = false, t_sub = false, t_da = false;
                            uint32_t t_sub_id = 0;
                            if (!lagfx_block_next_record(&tb, scratch_ops, &trec,
                                                          &t_end, &t_sub, &t_da,
                                                          &t_sub_id)) {
                                LAGFX_LOG("air_bitcode_reader: TYPE_BLOCK read failed at bit %zu",
                                          lagfx_bs_pos(&bs));
                                break;
                            }
                            if (t_end) break;
                            if (t_sub) { lagfx_block_skip(&bs, tb.abbrev_width); continue; }
                            if (t_da) continue;

                            if (trec.code == LAGFX_TYPE_NUMENTRY) {
                                /* NUMENTRY hints number of types; we just continue. */
                                continue;
                            }

                            if (num_types >= MAX_TYPES) break;
                            lagfx_air_type_t *t = &types[num_types];
                            t->op = NULL;
                            t->num_op = 0u;
                            t->op_offset = 0u;
                            t->name_offset = 0u;

                            switch (trec.code) {
                                case LAGFX_TYPE_VOID:      t->kind = LAGFX_AIR_TYPE_VOID; break;
                                case LAGFX_TYPE_FLOAT:     t->kind = LAGFX_AIR_TYPE_FLOAT; break;
                                case LAGFX_TYPE_DOUBLE:    t->kind = LAGFX_AIR_TYPE_DOUBLE; break;
                                case LAGFX_TYPE_HALF:      t->kind = LAGFX_AIR_TYPE_HALF; break;
                                case LAGFX_TYPE_LABEL:     t->kind = LAGFX_AIR_TYPE_LABEL; break;
                                case LAGFX_TYPE_METADATA:  t->kind = LAGFX_AIR_TYPE_METADATA; break;
                                case LAGFX_TYPE_INTEGER:
                                case LAGFX_TYPE_POINTER:
                                case LAGFX_TYPE_VECTOR:
                                case LAGFX_TYPE_ARRAY:
                                case LAGFX_TYPE_FUNCTION:
                                case LAGFX_TYPE_STRUCT_ANON:
                                case LAGFX_TYPE_STRUCT_NAMED: {
                                    /* These types have operands; intern them
                                     * into the arena. */
                                    static const lagfx_air_type_kind_t kind_map[] = {
                                        [LAGFX_TYPE_INTEGER]      = LAGFX_AIR_TYPE_INTEGER,
                                        [LAGFX_TYPE_POINTER]      = LAGFX_AIR_TYPE_POINTER,
                                        [LAGFX_TYPE_VECTOR]       = LAGFX_AIR_TYPE_VECTOR,
                                        [LAGFX_TYPE_ARRAY]        = LAGFX_AIR_TYPE_ARRAY,
                                        [LAGFX_TYPE_FUNCTION]     = LAGFX_AIR_TYPE_FUNCTION,
                                        [LAGFX_TYPE_STRUCT_ANON]  = LAGFX_AIR_TYPE_STRUCT_ANON,
                                        [LAGFX_TYPE_STRUCT_NAMED] = LAGFX_AIR_TYPE_STRUCT_NAMED,
                                    };
                                    t->kind = kind_map[trec.code];
                                    /* Stash operands as u32 array in arena. */
                                    uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                    for (uint32_t i = 0; i < trec.num_ops; i++) {
                                        u32_buf[i] = (uint32_t)trec.ops[i];
                                    }
                                    uint32_t off = arena_intern_u32_array(&m->arena, u32_buf, trec.num_ops);
                                    if (off == 0u && trec.num_ops > 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    /* WARNING: arena_intern_u32_array may have
                                     * realloc'd m->arena.base, invalidating
                                     * `types` pointer. Refresh from offset. */
                                    types = (lagfx_air_type_t *)(m->arena.base + types_off);
                                    t = &types[num_types];
                                    t->op = (off == 0u) ? NULL : (const uint32_t *)(m->arena.base + off);
                                    t->op_offset = off;
                                    t->num_op = trec.num_ops;
                                    if (trec.code == LAGFX_TYPE_STRUCT_NAMED) {
                                        t->name_offset = pending_name_offset;
                                        pending_name_offset = 0u;
                                    }
                                    break;
                                }
                                case LAGFX_TYPE_STRUCT_NAME: {
                                    /* String operands (one char per op). Stash in arena
                                     * so the NEXT STRUCT_NAMED can reference. */
                                    size_t len = trec.num_ops;
                                    uint32_t off = arena_reserve(&m->arena, len + 1u);
                                    if (off == 0u) {
                                        lagfx_air_module_free(m);
                                        return LAGFX_ERR_OUT_OF_MEMORY;
                                    }
                                    types = (lagfx_air_type_t *)(m->arena.base + types_off);
                                    for (size_t i = 0; i < len; i++) {
                                        m->arena.base[off + i] = (uint8_t)(trec.ops[i] & 0xFFu);
                                    }
                                    m->arena.base[off + len] = 0;
                                    pending_name_offset = off;
                                    continue;  /* don't increment num_types */
                                }
                                default:
                                    t->kind = LAGFX_AIR_TYPE_UNKNOWN;
                                    break;
                            }
                            num_types++;
                        }

                        m->types = types;
                        m->types_off = types_off;
                        m->num_types = num_types;
                        LAGFX_TRACE("air_bitcode_reader: TYPE_BLOCK decoded %u entries",
                                    num_types);
                        (void)lagfx_bs_seek(&bs, tb.end_pos);
                        continue;
                    }

                    if (sub_id == LAGFX_BLK_FUNCTION) {
                         /* FUNCTION_BLOCK: instruction stream for one
                          * function body. Phase 2 will decode; Phase 1
                          * just stashes the bit-offset + length on the
                          * next unassigned non-prototype function. The
                          * stashed offset points AT the ENTER_SUBBLOCK
                          * abbrev code so a Phase 2 reader can position
                          * its bitstream cursor there and call
                          * lagfx_block_enter() directly. */
                         size_t body_start = lagfx_bs_pos(&bs);
                         if (!lagfx_block_skip(&bs, blk.abbrev_width)) {
                             LAGFX_ERR("air_bitcode_reader: failed to skip FUNCTION_BLOCK");
                             break;
                         }
                         size_t body_end = lagfx_bs_pos(&bs);
                         /* m->functions may be stale here: earlier blocks
                          * (constants, metadata, types, …) ran
                          * arena_reserve and could have realloc'd the
                          * arena since the function table was cached. */
                         derive_table_ptrs(m);
                         /* Advance next_body_fn past any prototypes (no
                          * body to attach). */
                         while (next_body_fn < m->num_functions &&
                                m->functions[next_body_fn].is_proto) {
                             next_body_fn++;
                         }
                         if (next_body_fn < m->num_functions) {
                             m->functions[next_body_fn].body_offset = body_start;
                             m->functions[next_body_fn].body_length = body_end - body_start;
                             LAGFX_TRACE("air_bitcode_reader: FUNCTION_BLOCK[%u] body @ bit %zu len %zu",
                                         next_body_fn, body_start, body_end - body_start);
                             next_body_fn++;
                         } else {
                             LAGFX_LOG("air_bitcode_reader: FUNCTION_BLOCK without a matching non-proto declaration (functions=%u)",
                                       m->num_functions);
                         }
                         continue;
                     }

                     if (sub_id == LAGFX_BLK_VALUE_SYMTAB) {
                         /* VALUE_SYMTAB_BLOCK: value symbol table with
                          * function/global names. Records:
                          *   VST_CODE_ENTRY(1): [valueid, namechar x N]
                          *   VST_CODE_BBENTRY(2): [bbid, namechar x N] — skip
                          *   VST_CODE_FNENTRY(3): [valueid, body_offset, namechar x N] */
                         lagfx_block_t vs;
                         if (!lagfx_block_enter(&bs, blk.abbrev_width, m->blockinfo, &vs)) {
                             LAGFX_ERR("air_bitcode_reader: failed to enter VALUE_SYMTAB_BLOCK");
                             break;
                         }

                         /* Reserve symtab entries buffer — hard cap at 4096 for now. */
                         const uint32_t MAX_SYM_TAB = 4096u;
                         uint32_t symtab_off = arena_reserve(&m->arena, sizeof(lagfx_symtab_entry_t) * MAX_SYM_TAB);
                         if (symtab_off == 0u && MAX_SYM_TAB > 0u) {
                             lagfx_air_module_free(m);
                             return LAGFX_ERR_OUT_OF_MEMORY;
                         }
                         lagfx_symtab_entry_t *entries = (lagfx_symtab_entry_t *)(m->arena.base + symtab_off);
                         uint32_t num_entries = 0u;

                         while (lagfx_bs_pos(&bs) < vs.end_pos) {
                             lagfx_record_t vrec = {0};
                             bool v_end = false, v_sub = false, v_da = false;
                             uint32_t v_sub_id = 0;
                             if (!lagfx_block_next_record(&vs, scratch_ops, &vrec,
                                                           &v_end, &v_sub, &v_da, &v_sub_id)) {
                                 break;
                             }
                             if (v_end) break;
                             if (v_sub || v_da) continue;

                             /* Skip BBENTRY — basic block names not needed for Phase 1. */
                             if (vrec.code == LAGFX_VST_CODE_BBENTRY) {
                                 continue;
                             }

                             if (num_entries >= MAX_SYM_TAB) break;
                             lagfx_symtab_entry_t *entry = &entries[num_entries];
                             entry->value_id = 0u;
                             entry->body_offset = 0u;
                             entry->name_offset = 0u;

                             if (vrec.code == LAGFX_VST_CODE_ENTRY) {
                                 /* [valueid, namechar x N] */
                                 if (vrec.num_ops >= 1u) {
                                     entry->value_id = (uint32_t)vrec.ops[0];
                                     size_t len = vrec.num_ops - 1u;
                                     uint32_t off = arena_reserve(&m->arena, len + 1u);
                                     if (off != 0u) {
                                         for (size_t i = 0; i < len; i++) {
                                             m->arena.base[off + i] = (uint8_t)(vrec.ops[i + 1u] & 0xFFu);
                                         }
                                         m->arena.base[off + len] = 0;
                                         entry->name_offset = off;
                                     }
                                 }
                             } else if (vrec.code == LAGFX_VST_CODE_FNENTRY) {
                                 /* [valueid, body_offset, namechar x N] */
                                 if (vrec.num_ops >= 2u) {
                                     entry->value_id = (uint32_t)vrec.ops[0];
                                     entry->body_offset = (uint32_t)vrec.ops[1];
                                     size_t len = vrec.num_ops - 2u;
                                     uint32_t off = arena_reserve(&m->arena, len + 1u);
                                     if (off != 0u) {
                                         for (size_t i = 0; i < len; i++) {
                                             m->arena.base[off + i] = (uint8_t)(vrec.ops[i + 2u] & 0xFFu);
                                         }
                                         m->arena.base[off + len] = 0;
                                         entry->name_offset = off;
                                     }
                                 }
                             }

                             num_entries++;
                         }

                         m->symtab_entries = entries;
                         m->num_symtab_entries = num_entries;
                         LAGFX_TRACE("air_bitcode_reader: VALUE_SYMTAB_BLOCK decoded %u entries", num_entries);
                         (void)lagfx_bs_seek(&bs, vs.end_pos);
                         continue;
                     }

                     /* Other sub-blocks: skip for now. */
                    if (!lagfx_block_skip(&bs, blk.abbrev_width)) {
                        LAGFX_ERR("air_bitcode_reader: failed to skip sub-block id=%u", sub_id);
                        break;
                    }
                    continue;
                }
                if (is_define_abbrev) continue;

                /* Dispatch on record code. */
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
                    case 8u:  /* MODULE_CODE_FUNCTION */
                    {
                        /* Two schemas per llvm/Bitcode/LLVMBitCodes.h:
                         *
                         *  STRTAB-bearing module (has_strtab=true):
                         *    op0 = strtab_offset (name into root STRTAB)
                         *    op1 = strtab_size   (name length)
                         *    op2 = type_index (function type)
                         *    op3 = calling convention
                         *    op4 = isProto (0 = has body, 1 = declaration)
                         *    op5 = linkage
                         *    op6 = paramattr index (1-based; 0 = none)
                         *    op7+ = alignment, section, visibility, ...
                         *
                         *  Legacy / non-STRTAB module:
                         *    op0 = type_index
                         *    op1 = calling convention
                         *    op2 = isProto
                         *    op3 = linkage
                         *    op4 = paramattr index
                         *    op5+ = ...
                         *
                         * Triangle + every captured macOS metallib we have
                         * use the STRTAB schema; the legacy branch is
                         * here for completeness against older bitcode. */
                        const uint32_t off_off = m->has_strtab ? 2u : 0u;
                        if (rec.num_ops < off_off + 3u) break;
                        uint32_t blob_off = arena_reserve(&m->arena, sizeof(lagfx_air_function_t));
                        if (blob_off == 0u) {
                            lagfx_air_module_free(m);
                            return LAGFX_ERR_OUT_OF_MEMORY;
                        }
                        if (m->num_functions == 0u) {
                            m->functions = (lagfx_air_function_t *)(m->arena.base + blob_off);
                            m->functions_off = blob_off;
                        }
                        lagfx_air_function_t *fn =
                            (lagfx_air_function_t *)(m->arena.base + blob_off);
                        /* For STRTAB modules: stash strtab_offset in
                         * name_offset and strtab_size in name_length for
                         * now. Post-MODULE pass will read the STRTAB
                         * BLOB, copy the name bytes into an arena-NUL-
                         * terminated string, and rewrite name_offset to
                         * the interned location. For non-STRTAB modules:
                         * name comes from VALUE_SYMTAB and stays 0 here. */
                        fn->name_offset      = m->has_strtab ? (uint32_t)rec.ops[0] : 0u;
                        fn->name_length      = m->has_strtab ? (uint32_t)rec.ops[1] : 0u;
                        fn->type_index       = (uint32_t)rec.ops[off_off + 0u];
                        /* off_off+1 is calling convention; not stored yet. */
                        fn->is_proto         = (rec.ops[off_off + 2u] != 0u);
                        fn->linkage          = (rec.num_ops > off_off + 3u)
                                                ? (uint32_t)rec.ops[off_off + 3u] : 0u;
                        fn->param_attr_index = (rec.num_ops > off_off + 4u)
                                                ? (uint32_t)rec.ops[off_off + 4u] : 0u;
                        fn->visibility       = (rec.num_ops > off_off + 7u)
                                                ? (uint32_t)rec.ops[off_off + 7u] : 0u;
                        fn->body_offset = 0u;
                        fn->body_length = 0u;
                        m->num_functions++;
                        break;
                    }
                    case LAGFX_MOD_CODE_GLOBALVAR:  /* = 7 */
                        /* We don't model the global's contents, but it
                         * DOES occupy a value-id (assigned before
                         * functions in LLVM's enumeration). Count it so
                         * the absolute-value-id base lines up — see
                         * num_globalvars doc comment. */
                        m->num_globalvars++;
                        break;
                    default:
                        /* Other module-level records (ALIAS, IFUNC,
                         * VSTOFFSET, etc.) — Phase 1 ignores. */
                        break;
                }
            }
            /* MODULE_BLOCK done. ONLY fall back to VALUE_SYMTAB linking
             * for non-STRTAB modules. STRTAB-bearing modules (both our
             * fixtures, and all captured macOS metallibs to date) get
             * their names from the STRTAB BLOB; the post-bs.at_end pass
             * below resolves them. Running the VALUE_SYMTAB linking on
             * a STRTAB module would clobber the strtab_offset stashed
             * in name_offset (since the linker only checks
             * `name_offset == 0` for its first non-proto function with
             * strtab_offset 0). */
            if (!m->has_strtab && m->symtab_entries != NULL &&
                m->num_symtab_entries > 0u) {
                derive_table_ptrs(m);  /* tables may be stale post-realloc */
                for (uint32_t i = 0; i < m->num_symtab_entries; i++) {
                    lagfx_symtab_entry_t *entry = &m->symtab_entries[i];
                    if (entry->name_offset == 0u) continue;
                    /* Match by value_id-equals-function-index. */
                    for (uint32_t j = 0; j < m->num_functions; j++) {
                        lagfx_air_function_t *fn = &m->functions[j];
                        if (fn->name_offset != 0u) continue;
                        if ((uint32_t)entry->value_id == j) {
                            fn->name_offset = entry->name_offset;
                            break;
                        }
                    }
                }
            }
            /* Exit handling. Three paths the MODULE walker can have
             * left in:
             *
             *   (a) END_BLOCK read cleanly (module_clean_exit==true).
             *       block_reader applied a 32-bit alignment after the
             *       END_BLOCK code; cursor is at the next root-level
             *       record. Continue the root loop so STRTAB_BLOCK /
             *       SYMTAB_BLOCK (which come AFTER MODULE in emission
             *       order) are picked up.
             *
             *   (b) Loop-condition false (cursor caught up to declared
             *       end_pos without END_BLOCK). Trust cursor; continue.
             *
             *   (c) Inner record reader bailed (module_clean_exit==false
             *       AND cursor < end_pos). Apple-flavored abbrev hit —
             *       captured macOS metallibs do this inside
             *       OPERAND_BUNDLE_TAGS (Phase 2.4). The bytes past the
             *       cursor are garbage from a root parser's view; stop
             *       root walking. What we already decoded stays valid. */
            if (!module_clean_exit && lagfx_bs_pos(&bs) < blk.end_pos) {
                LAGFX_LOG("air_bitcode_reader: MODULE exited early at bit %zu (declared end %zu) — skipping post-MODULE root blocks",
                          lagfx_bs_pos(&bs), blk.end_pos);
                break;
            }
            continue;
        } else {
             /* Non-MODULE block at root (e.g., IDENTIFICATION_BLOCK).
              * Skip and continue. We already entered it; need to seek
              * past its end. If the seek fails (Apple-inflated
              * block_size overshoots the actual buffer — same family
              * as the MODULE truncation issue, see Phase 2.4 pickup
              * notes), break out gracefully — what we've already
              * decoded stays valid. */
            if (!lagfx_bs_seek(&bs, blk.end_pos)) {
                LAGFX_LOG("air_bitcode_reader: cannot seek past root sub-block id=%u (declared end %zu past buffer) — treating as end-of-stream",
                          blk.block_id, blk.end_pos);
                break;
            }
        }
    }

    /* STRTAB name-resolution post-pass.
     *
     * For STRTAB-bearing modules, each function's name_offset is still
     * a STRTAB-relative offset (set during MODULE_CODE_FUNCTION parsing).
     * Now that STRTAB_BLOCK's BLOB is in the arena, intern a NUL-
     * terminated copy of each function's name slice and rewrite
     * name_offset to point at the interned copy. This makes
     * lagfx_air_module_string(fn->name_offset) return the function
     * name directly.
     *
     * If a function's strtab slice is out of range (because STRTAB
     * wasn't found or the offset is bogus), leave name_offset at 0 to
     * signal "name unknown" — the public string accessor returns NULL
     * for offset 0. */
    if (m->has_strtab) {
        if (m->strtab_arena_offset != 0u && m->strtab_len_bytes > 0u) {
            const uint8_t *strtab_bytes = m->arena.base + m->strtab_arena_offset;
            size_t strtab_len = m->strtab_len_bytes;
            derive_table_ptrs(m);  /* m->functions may be stale post-walk */
            for (uint32_t i = 0; i < m->num_functions; i++) {
                lagfx_air_function_t *fn = &m->functions[i];
                uint32_t st_off = fn->name_offset;  /* currently STRTAB-relative */
                uint32_t st_len = fn->name_length;
                if (st_len == 0u || (size_t)st_off + (size_t)st_len > strtab_len) {
                    fn->name_offset = 0u;
                    continue;
                }
                uint32_t arena_off = arena_reserve(&m->arena, st_len + 1u);
                if (arena_off == 0u) {
                    lagfx_air_module_free(m);
                    return LAGFX_ERR_OUT_OF_MEMORY;
                }
                /* arena_reserve may have realloc'd: refresh strtab_bytes
                 * AND re-derive the function table (fn would be stale). */
                strtab_bytes = m->arena.base + m->strtab_arena_offset;
                derive_table_ptrs(m);
                memcpy(m->arena.base + arena_off, strtab_bytes + st_off, st_len);
                m->arena.base[arena_off + st_len] = 0;
                m->functions[i].name_offset = arena_off;
                LAGFX_TRACE("air_bitcode_reader: STRTAB resolved fn[%u] '%.*s' (arena+%u, len=%u)",
                            i, (int)st_len, (const char *)(m->arena.base + arena_off),
                            arena_off, st_len);
            }
        } else {
            /* STRTAB schema assumed but no STRTAB_BLOCK was loaded
             * (captured macOS metallib whose MODULE walker bailed
             * before reaching STRTAB at root). Clear name_offset on
             * every function so callers don't dereference a stale
             * STRTAB-relative offset as an arena offset. */
            for (uint32_t i = 0; i < m->num_functions; i++) {
                m->functions[i].name_offset = 0u;
            }
        }
    }

    /* Materialize metadata pointers AFTER all arena_reserve calls so
     * the pointer values are stable. Up to this point we've kept
     * arena offsets only; now translate them into the public
     * (const char **) / lagfx_air_metadata_t * surfaces. The metadata
     * walker recorded m->metadata_strings_offsets_arena (offset of the
     * u32[] of string offsets) and m->metadata_records_arena (offset
     * of the records[] array). */
    if (m->num_metadata > 0u) {
        m->metadata = (lagfx_air_metadata_t *)(m->arena.base + m->metadata_records_arena);
    }
    if (m->num_metadata_strings > 0u) {
        /* Allocate the const char ** pointer array in the arena; fill
         * from the offsets table. The pointer array is appended to the
         * arena AFTER STRTAB pass, so its slots stay valid for the
         * module's lifetime (no further arena_reserve happens). */
        size_t ptr_bytes = (size_t)m->num_metadata_strings * sizeof(const char *);
        uint32_t ptr_off = arena_reserve(&m->arena, ptr_bytes);
        if (ptr_off == 0u) {
            lagfx_air_module_free(m);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        /* Refresh after potential realloc. */
        const uint32_t *str_offs =
            (const uint32_t *)(m->arena.base + m->metadata_strings_offsets_arena);
        const char **ptrs = (const char **)(m->arena.base + ptr_off);
        for (uint32_t i = 0; i < m->num_metadata_strings; i++) {
            ptrs[i] = (const char *)(m->arena.base + str_offs[i]);
        }
        m->metadata_strings = ptrs;
    }

    /* Final re-derivation before handing the module to the caller: the
     * metadata-strings pointer-array reserve (and the STRTAB pass) may
     * have realloc'd the arena after the table/metadata pointers were
     * cached, so refresh them all. (m->metadata_strings and its slots
     * were written after the last reserve, so they stay valid.) */
    derive_table_ptrs(m);
    if (m->num_metadata > 0u) {
        m->metadata = (lagfx_air_metadata_t *)(m->arena.base + m->metadata_records_arena);
    }

    *out_module = m;
    return LAGFX_OK;
}

void
lagfx_air_module_free(lagfx_air_module_t *module) {
    if (!module) return;
    free(module->blockinfo);
    arena_free(&module->arena);
    free(module);
}

/* =====================================================================
 * Phase 2 — FUNCTION_BLOCK body decoder
 *
 * Walks one function body block (the bit-range stashed by Phase 2 step 1
 * during MODULE_BLOCK traversal) and produces an in-memory list of
 * lagfx_air_inst_t records. Nested CONSTANTS_BLOCK / METADATA_BLOCK /
 * METADATA_ATTACHMENT_BLOCK sub-blocks are walked-past for now (their
 * contents are function-local and Phase 3 work).
 *
 * Apple-custom abbreviations inside FUNCTION_BLOCK are the long pole
 * here. Triangle's body uses the default LLVM abbrev set + a small
 * number of in-block DEFINE_ABBREVs that our generic abbrev reader
 * already handles; bcanalyzer agrees on 18 records (1 DECLAREBLOCKS +
 * 17 instructions). Captured macOS bodies are expected to need
 * Apple-specific abbrev decoding which lands in subsequent commits.
 * ===================================================================== */

struct lagfx_air_function_body {
    lagfx_arena_t arena;
    uint32_t      num_blocks;
    lagfx_air_inst_t *instructions;
    uint32_t      num_instructions;
    /* Function-local CONSTANTS_BLOCK contents. LLVM stores constants
     * that are referenced only from within this function as a nested
     * block at the start of the FUNCTION_BLOCK. They occupy value-IDs
     * AFTER the function's arguments and BEFORE instruction results.
     * See LLVMBitcodes.h / BitcodeReader::ParseFunctionBody.
     *
     * Stored as an arena BYTE OFFSET, not a raw pointer (B2 fix). The
     * function's instructions are parsed AFTER this constants block and keep
     * growing the arena — every growth realloc's arena.base, which would
     * dangle a raw pointer cached at parse time (use-after-free → SIGSEGV in
     * lagfx_air2spv_translate_function on large fragments like SkyLight's
     * pipeline 0x2f). The offset is resolved against the live arena.base in
     * the accessor, where parsing is already complete and the base is final. */
    uint32_t              local_constants_off;
    uint32_t              num_local_constants;
};

/* Map a raw FUNC_CODE_* record code to our enum. Unknown codes get
 * LAGFX_AIR_INST_UNKNOWN so the consumer can still inspect raw_code. */
static lagfx_air_inst_code_t classify_inst(uint32_t raw) {
    switch (raw) {
        case 1:  return LAGFX_AIR_INST_DECLAREBLOCKS;
        case 2:  return LAGFX_AIR_INST_BINOP;
        case 3:  return LAGFX_AIR_INST_CAST;
        case 4:  return LAGFX_AIR_INST_GEP_OLD;
        case 5:  return LAGFX_AIR_INST_SELECT;
        case 6:  return LAGFX_AIR_INST_EXTRACTELT;
        case 7:  return LAGFX_AIR_INST_INSERTELT;
        case 8:  return LAGFX_AIR_INST_SHUFFLEVEC;
        case 9:  return LAGFX_AIR_INST_CMP;
        case 10: return LAGFX_AIR_INST_RET;
        case 11: return LAGFX_AIR_INST_BR;
        case 12: return LAGFX_AIR_INST_SWITCH;
        case 15: return LAGFX_AIR_INST_UNREACHABLE;
        case 16: return LAGFX_AIR_INST_PHI;
        case 19: return LAGFX_AIR_INST_ALLOCA;
        case 20: return LAGFX_AIR_INST_LOAD;
        case 24: return LAGFX_AIR_INST_STORE_OLD;
        case 26: return LAGFX_AIR_INST_EXTRACTVAL;
        case 27: return LAGFX_AIR_INST_INSERTVAL;
        case 28: return LAGFX_AIR_INST_CMP2;
        case 29: return LAGFX_AIR_INST_VSELECT;
        case 31: return LAGFX_AIR_INST_INDIRECTBR;
        case 33: return LAGFX_AIR_INST_DEBUG_LOC_AGAIN;
        case 34: return LAGFX_AIR_INST_CALL;
        case 35: return LAGFX_AIR_INST_DEBUG_LOC;
        case 36: return LAGFX_AIR_INST_FENCE;
        case 43: return LAGFX_AIR_INST_GEP;
        case 44: return LAGFX_AIR_INST_STORE;
        case 46: return LAGFX_AIR_INST_CMPXCHG;
        case 56: return LAGFX_AIR_INST_UNOP;
        default: return LAGFX_AIR_INST_UNKNOWN;
    }
}

lagfx_status_t
lagfx_air_function_body_open(const lagfx_air_module_t   *module,
                              uint32_t                   fn_idx,
                              lagfx_air_function_body_t **out_body) {
    if (!module || !out_body) return LAGFX_ERR_INVALID_ARG;
    *out_body = NULL;
    if (fn_idx >= module->num_functions) return LAGFX_ERR_INVALID_ARG;
    const lagfx_air_function_t *fn = &module->functions[fn_idx];
    if (fn->is_proto || fn->body_offset == 0u || fn->body_length == 0u) {
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Allocate body + arena. */
    lagfx_air_function_body_t *body =
        (lagfx_air_function_body_t *)calloc(1, sizeof(*body));
    if (!body) return LAGFX_ERR_OUT_OF_MEMORY;
    if (!arena_init(&body->arena, 4u * 1024u)) {
        free(body);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* Position a fresh bitstream at the start of the FUNCTION_BLOCK
     * (the ENTER_SUBBLOCK abbrev code). The stashed body_offset is a
     * bit position within module->bitstream copy. */
    lagfx_bitstream_t bs;
    lagfx_bs_init(&bs, module->arena.base + module->bitstream_arena_offset,
                  module->bitstream_len_bytes);
    if (!lagfx_bs_seek(&bs, fn->body_offset)) {
        lagfx_air_function_body_free(body);
        return LAGFX_ERR_PROTOCOL;
    }

    /* The parent block (MODULE) uses 3-bit abbrev codes. The stashed
     * offset points at the ENTER_SUBBLOCK code itself, so we hand the
     * parent width to block_enter. */
    const uint32_t parent_abbrev_width = 3u;
    lagfx_block_t fb;
    if (!lagfx_block_enter(&bs, parent_abbrev_width, module->blockinfo, &fb)) {
        LAGFX_ERR("function_body: failed to enter FUNCTION_BLOCK at bit %zu",
                  fn->body_offset);
        lagfx_air_function_body_free(body);
        return LAGFX_ERR_PROTOCOL;
    }
    if (fb.block_id != LAGFX_BLK_FUNCTION) {
        LAGFX_ERR("function_body: stashed offset doesn't point at FUNCTION_BLOCK (got id=%u)",
                  fb.block_id);
        lagfx_air_function_body_free(body);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Reserve an instruction array up front; we'll cap at MAX. */
    const uint32_t MAX_INSTS = 4096u;
    uint32_t insts_off = arena_reserve(&body->arena,
                                        sizeof(lagfx_air_inst_t) * MAX_INSTS);
    if (insts_off == 0u) {
        lagfx_air_function_body_free(body);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    body->instructions = (lagfx_air_inst_t *)(body->arena.base + insts_off);
    body->num_instructions = 0u;

    uint64_t scratch_ops[LAGFX_RECORD_MAX_OPS];
    while (lagfx_bs_pos(&bs) < fb.end_pos) {
        lagfx_record_t rec = {0};
        bool is_end = false, is_subblock = false, is_define_abbrev = false;
        uint32_t sub_id = 0;
        if (!lagfx_block_next_record(&fb, scratch_ops, &rec,
                                       &is_end, &is_subblock,
                                       &is_define_abbrev, &sub_id)) {
            LAGFX_LOG("function_body: next_record failed at bit %zu (insts decoded so far=%u)",
                      lagfx_bs_pos(&bs), body->num_instructions);
            /* Bail; what we've decoded so far is still valid. */
            break;
        }
        if (is_end) break;
        if (is_subblock) {
            if (sub_id == LAGFX_BLK_CONSTANTS) {
                /* Function-local CONSTANTS_BLOCK. Same SETTYPE +
                 * record-code shape as the module-level CONSTANTS_BLOCK
                 * (see module-parse path); duplicated here because we
                 * need to write into body->arena, not m->arena, and the
                 * constants populate the function-local value-id slice
                 * (after args, before instruction results). */
                lagfx_block_t cb;
                if (!lagfx_block_enter(&bs, fb.abbrev_width, module->blockinfo, &cb)) {
                    LAGFX_ERR("function_body: failed to enter local CONSTANTS_BLOCK");
                    break;
                }
                const uint32_t MAX_LOCAL_CONSTS = 256u;
                uint32_t lc_off = arena_reserve(&body->arena,
                                                 sizeof(lagfx_air_constant_t) * MAX_LOCAL_CONSTS);
                if (lc_off == 0u) {
                    lagfx_air_function_body_free(body);
                    return LAGFX_ERR_OUT_OF_MEMORY;
                }
                /* instructions pointer must be refreshed after any
                 * arena_reserve since the arena may have realloc'd. */
                body->instructions = (lagfx_air_inst_t *)(body->arena.base + insts_off);
                lagfx_air_constant_t *lc =
                    (lagfx_air_constant_t *)(body->arena.base + lc_off);
                uint32_t lc_count = 0u;
                uint32_t current_type = 0u;

                while (lagfx_bs_pos(&bs) < cb.end_pos) {
                    lagfx_record_t crec = {0};
                    bool c_end = false, c_sub = false, c_da = false;
                    uint32_t c_sub_id = 0;
                    if (!lagfx_block_next_record(&cb, scratch_ops, &crec,
                                                  &c_end, &c_sub, &c_da, &c_sub_id)) break;
                    if (c_end) break;
                    if (c_sub) { lagfx_block_skip(&bs, cb.abbrev_width); continue; }
                    if (c_da) continue;
                    if (lc_count >= MAX_LOCAL_CONSTS) break;

                    if (crec.code == 1u) {
                        /* CST_CODE_SETTYPE */
                        if (crec.num_ops >= 1u) current_type = (uint32_t)crec.ops[0];
                        continue;
                    }
                    lagfx_air_constant_t *c = &lc[lc_count];
                    c->type_index = current_type;
                    c->payload.bytes.offset = 0;
                    c->payload.bytes.len = 0;
                    switch (crec.code) {
                        case 2u:  /* CST_CODE_NULL */
                            c->kind = LAGFX_AIR_CONST_NULL; break;
                        case 3u:  /* CST_CODE_UNDEF */
                            c->kind = LAGFX_AIR_CONST_UNDEF; break;
                        case 4u:  /* CST_CODE_INTEGER */
                            c->kind = LAGFX_AIR_CONST_INTEGER;
                            if (crec.num_ops >= 1) {
                                uint64_t raw = crec.ops[0];
                                int64_t v = (raw & 1) ? -(int64_t)(raw >> 1)
                                                       : (int64_t)(raw >> 1);
                                c->payload.i64 = v;
                            }
                            break;
                        case 6u:  /* CST_CODE_FLOAT */
                            c->kind = LAGFX_AIR_CONST_FLOAT;
                            if (crec.num_ops >= 1) {
                                union { uint64_t u; double f; } cv;
                                cv.u = crec.ops[0];
                                c->payload.f64 = cv.f;
                            }
                            break;
                        case 22u: /* CST_CODE_DATA — vector-of-X literal */
                            c->kind = LAGFX_AIR_CONST_DATA;
                            {
                                uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                for (uint32_t i = 0; i < crec.num_ops; i++)
                                    u32_buf[i] = (uint32_t)crec.ops[i];
                                uint32_t off = arena_intern_u32_array(&body->arena,
                                                                       u32_buf, crec.num_ops);
                                /* Refresh both arrays after potential realloc. */
                                body->instructions =
                                    (lagfx_air_inst_t *)(body->arena.base + insts_off);
                                lc = (lagfx_air_constant_t *)(body->arena.base + lc_off);
                                c = &lc[lc_count];
                                c->payload.bytes.offset = off;
                                c->payload.bytes.len = crec.num_ops * sizeof(uint32_t);
                            }
                            break;
                        case 7u:  /* CST_CODE_AGGREGATE */
                            c->kind = LAGFX_AIR_CONST_AGGREGATE;
                            {
                                uint32_t u32_buf[LAGFX_RECORD_MAX_OPS];
                                for (uint32_t i = 0; i < crec.num_ops; i++)
                                    u32_buf[i] = (uint32_t)crec.ops[i];
                                uint32_t off = arena_intern_u32_array(&body->arena,
                                                                       u32_buf, crec.num_ops);
                                body->instructions =
                                    (lagfx_air_inst_t *)(body->arena.base + insts_off);
                                lc = (lagfx_air_constant_t *)(body->arena.base + lc_off);
                                c = &lc[lc_count];
                                c->payload.bytes.offset = off;
                                c->payload.bytes.len = crec.num_ops * sizeof(uint32_t);
                            }
                            break;
                        default:
                            c->kind = LAGFX_AIR_CONST_UNKNOWN;
                            break;
                    }
                    lc_count++;
                }
                /* Store the OFFSET, not lc (the raw pointer): subsequent
                 * instruction parsing grows + realloc's the arena, which would
                 * dangle lc. Resolved against the final base in the accessor. */
                body->local_constants_off = lc_off;
                body->num_local_constants = lc_count;
                (void)lc;
                LAGFX_TRACE("function_body: local CONSTANTS_BLOCK decoded %u constants", lc_count);
                (void)lagfx_bs_seek(&bs, cb.end_pos);
                continue;
            }
            /* Other nested blocks (METADATA_BLOCK,
             * METADATA_ATTACHMENT_BLOCK, VALUE_SYMTAB) — skip; Phase 3
             * already mines what it needs from module-level. */
            if (!lagfx_block_skip(&bs, fb.abbrev_width)) {
                LAGFX_ERR("function_body: failed to skip nested sub-block id=%u", sub_id);
                break;
            }
            continue;
        }
        if (is_define_abbrev) continue;

        if (body->num_instructions >= MAX_INSTS) {
            LAGFX_WARN("function_body: instruction cap (%u) reached; truncating", MAX_INSTS);
            break;
        }

        /* Stash a copy of the record's operand vector in our arena so
         * the caller can hold the body open after open() returns. */
        uint32_t ops_off = 0u;
        if (rec.num_ops > 0u) {
            ops_off = arena_reserve(&body->arena, rec.num_ops * sizeof(uint64_t));
            if (ops_off == 0u) {
                lagfx_air_function_body_free(body);
                return LAGFX_ERR_OUT_OF_MEMORY;
            }
            /* arena may have realloc'd; refresh instructions pointer. */
            body->instructions = (lagfx_air_inst_t *)(body->arena.base + insts_off);
            memcpy(body->arena.base + ops_off, rec.ops,
                   rec.num_ops * sizeof(uint64_t));
        }

        lagfx_air_inst_t *inst = &body->instructions[body->num_instructions++];
        inst->raw_code = rec.code;
        inst->code     = classify_inst(rec.code);
        inst->ops      = (ops_off == 0u)
                            ? NULL
                            : (const uint64_t *)(body->arena.base + ops_off);
        inst->ops_off  = ops_off;  /* re-resolved post-parse against final base */
        inst->num_ops  = rec.num_ops;

        /* DECLAREBLOCKS sets body->num_blocks. */
        if (inst->code == LAGFX_AIR_INST_DECLAREBLOCKS && rec.num_ops >= 1u) {
            body->num_blocks = (uint32_t)rec.ops[0];
        }
    }

    /* Re-resolve every instruction's operand pointer against the FINAL
     * arena.base. During parsing each inst->ops was cached as base+ops_off,
     * but subsequent instructions' arena_reserve calls realloc the arena and
     * move base — dangling all earlier ops pointers (use-after-free → SIGSEGV
     * in the translator on bodies large enough to realloc, e.g. SkyLight
     * pipelines 0x2f/0x31). body->instructions is refreshed in-loop; the
     * per-instruction ops pointers are not, so fix them up here once, now that
     * the base is final. Same fix class as local_constants_off. */
    for (uint32_t i = 0; i < body->num_instructions; i++) {
        lagfx_air_inst_t *fi = &body->instructions[i];
        fi->ops = fi->ops_off ? (const uint64_t *)(body->arena.base + fi->ops_off)
                              : NULL;
    }

    LAGFX_TRACE("function_body: fn[%u] decoded %u instructions (%u basic blocks)",
                fn_idx, body->num_instructions, body->num_blocks);
    *out_body = body;
    return LAGFX_OK;
}

void
lagfx_air_function_body_free(lagfx_air_function_body_t *body) {
    if (!body) return;
    arena_free(&body->arena);
    free(body);
}

uint32_t
lagfx_air_function_body_num_blocks(const lagfx_air_function_body_t *body) {
    return body ? body->num_blocks : 0u;
}

const lagfx_air_inst_t *
lagfx_air_function_body_instructions(const lagfx_air_function_body_t *body,
                                       uint32_t *count) {
    if (count) *count = body ? body->num_instructions : 0u;
    return body ? body->instructions : NULL;
}

const lagfx_air_constant_t *
lagfx_air_function_body_local_constants(const lagfx_air_function_body_t *body,
                                          uint32_t *count) {
    if (count) *count = body ? body->num_local_constants : 0u;
    if (!body || body->num_local_constants == 0u) {
        return NULL;
    }
    /* Resolve the arena offset against the CURRENT base. Parsing is complete
     * by the time any consumer calls this, so arena.base is final and the
     * returned pointer is stable for the lifetime of the body (B2 fix: a raw
     * pointer cached at parse time dangled after later instruction-parse
     * arena reallocs). */
    return (const lagfx_air_constant_t *)(body->arena.base + body->local_constants_off);
}

const void *
lagfx_air_function_body_payload_ptr(const lagfx_air_function_body_t *body,
                                      uint32_t offset) {
    if (!body || offset == 0u) return NULL;
    if (offset >= body->arena.used) return NULL;
    return (const void *)(body->arena.base + offset);
}
