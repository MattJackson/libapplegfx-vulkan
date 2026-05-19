/*
 * libapplegfx-vulkan — clean-room AIR (LLVM) bitcode reader
 * src/air/bitcode_reader.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1: parses the standard LLVM bitcode container portion of an
 * Apple .air.bc file (extracted from a .metallib by metallib_extract)
 * into an in-memory representation. Stops short of FUNCTION_BLOCK body
 * decoding — that's Phase 2 (Apple-aware decoder).
 *
 * Bitcode format reference: https://llvm.org/docs/BitCodeFormat.html
 *
 * What Phase 1 produces:
 *   - Wrapper header (magic, version, payload offset, size)
 *   - IDENTIFICATION block
 *   - MODULE_BLOCK metadata:
 *     - TYPE_BLOCK_ID    -> lagfx_air_module_t::types[]
 *     - PARAMATTR_GROUP  -> lagfx_air_module_t::param_attr_groups[]
 *     - PARAMATTR_BLOCK  -> lagfx_air_module_t::param_attrs[]
 *     - CONSTANTS_BLOCK  -> lagfx_air_module_t::constants[]
 *     - METADATA_KIND_BLOCK -> lagfx_air_module_t::metadata_kinds[]
 *     - METADATA_BLOCK   -> lagfx_air_module_t::metadata[]
 *     - OPERAND_BUNDLE_TAGS_BLOCK -> lagfx_air_module_t::operand_bundle_tags[]
 *     - TRIPLE / DATALAYOUT / SOURCE_FILENAME records -> module strings
 *     - FUNCTION records  -> lagfx_air_module_t::functions[] (declarations only;
 *                            body offset stashed for Phase 2)
 *
 * What Phase 1 does NOT do:
 *   - Decode FUNCTION_BLOCK contents (instruction streams). Function body
 *     offsets are stashed for Phase 2 to consume.
 *   - Resolve metadata cross-references into a usable semantic structure.
 *     We collect raw records; semantic mapping is Phase 3.
 *
 * Memory ownership:
 *   - lagfx_air_module_open() malloc's a single struct + arena for all
 *     string-data + record-data. The caller owns it; free with
 *     lagfx_air_module_free().
 *   - All `const char *` pointers in the public struct point into the
 *     module's internal arena; valid until module_free.
 */

#ifndef LIBAPPLEGFX_AIR_BITCODE_READER_H
#define LIBAPPLEGFX_AIR_BITCODE_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libapplegfx-vulkan.h"  /* for lagfx_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* === Type table ================================================== */

typedef enum {
    LAGFX_AIR_TYPE_VOID         = 0,
    LAGFX_AIR_TYPE_INTEGER      = 1,   /* op0 = bit width */
    LAGFX_AIR_TYPE_FLOAT        = 2,   /* IEEE-754 binary32 */
    LAGFX_AIR_TYPE_DOUBLE       = 3,   /* IEEE-754 binary64 */
    LAGFX_AIR_TYPE_HALF         = 4,   /* IEEE-754 binary16 */
    LAGFX_AIR_TYPE_VECTOR       = 5,   /* op0 = lane count, op1 = element type index */
    LAGFX_AIR_TYPE_POINTER      = 6,   /* op0 = pointee type index, op1 = address space */
    LAGFX_AIR_TYPE_STRUCT_ANON  = 7,   /* op0 = packed, op1..N = field type indices */
    LAGFX_AIR_TYPE_STRUCT_NAMED = 8,   /* op0 = packed, op1..N = field type indices; name in name_offset */
    LAGFX_AIR_TYPE_ARRAY        = 9,   /* op0 = length, op1 = element type index */
    LAGFX_AIR_TYPE_FUNCTION     = 10,  /* op0 = varargs, op1 = return type, op2..N = param types */
    LAGFX_AIR_TYPE_METADATA     = 11,  /* opaque metadata reference type */
    LAGFX_AIR_TYPE_LABEL        = 12,
    LAGFX_AIR_TYPE_UNKNOWN      = 255, /* catch-all for any LLVM type code we haven't decoded yet */
} lagfx_air_type_kind_t;

typedef struct {
    lagfx_air_type_kind_t kind;
    /* Type-specific operands. Interpretation depends on `kind`:
     *   INTEGER: op[0] = bit width
     *   VECTOR:  op[0] = lane count, op[1] = element type index
     *   POINTER: op[0] = pointee type idx, op[1] = address space
     *   STRUCT_ANON: op[0] = packed flag, then op[1..1+num_fields-1] = field type indices
     *   STRUCT_NAMED: same as anon plus `name_offset` set
     *   ARRAY:   op[0] = length, op[1] = element type idx
     *   FUNCTION: op[0] = varargs flag, op[1] = return type idx, op[2..] = param type indices
     */
    const uint32_t *op;
    uint32_t        num_op;
    /* For STRUCT_NAMED: offset into module string arena; for others: 0. */
    uint32_t        name_offset;
} lagfx_air_type_t;

/* === Constant table (subset for Phase 1) ========================= */

typedef enum {
    LAGFX_AIR_CONST_NULL         = 0,
    LAGFX_AIR_CONST_INTEGER      = 1,
    LAGFX_AIR_CONST_FLOAT        = 2,
    LAGFX_AIR_CONST_AGGREGATE    = 3,    /* arrays / structs / vectors of constants */
    LAGFX_AIR_CONST_STRING       = 4,
    LAGFX_AIR_CONST_DATA         = 5,    /* dense data array */
    LAGFX_AIR_CONST_UNDEF        = 6,
    LAGFX_AIR_CONST_GLOBAL       = 7,    /* pointer to a global symbol */
    LAGFX_AIR_CONST_UNKNOWN      = 255,
} lagfx_air_constant_kind_t;

typedef struct {
    lagfx_air_constant_kind_t kind;
    uint32_t type_index;         /* index into module->types[] */
    /* Inline payload for small constants (INTEGER, FLOAT). For larger
     * payloads (STRING, DATA, AGGREGATE) data_offset / data_len index
     * into the module string arena. */
    union {
        int64_t  i64;
        double   f64;
        struct { uint32_t offset; uint32_t len; } bytes;
    } payload;
} lagfx_air_constant_t;

/* === Function declaration ======================================== */

typedef struct {
    uint32_t    name_offset;     /* into module string arena */
    uint32_t    type_index;      /* into module->types[]; must be FUNCTION kind */
    uint32_t    param_attr_index;/* into module->param_attrs[]; 0 = none */
    uint32_t    linkage;         /* LLVM linkage code (raw) */
    uint32_t    visibility;      /* LLVM visibility code (raw) */
    bool        is_proto;        /* true = declaration only; false = has body */
    /* Phase 2 will populate body_offset when we decode FUNCTION_BLOCK
     * instances. Phase 1 leaves this at 0. */
    size_t      body_offset;
    size_t      body_length;
} lagfx_air_function_t;

/* === Metadata record (Phase 1: raw form) ========================= */

typedef enum {
    LAGFX_AIR_MD_STRING         = 0,   /* string operand */
    LAGFX_AIR_MD_NODE           = 1,   /* tuple of metadata refs */
    LAGFX_AIR_MD_NAMED_NODE     = 2,   /* named tuple */
    LAGFX_AIR_MD_VALUE          = 3,   /* (type, value) pair */
    LAGFX_AIR_MD_UNKNOWN        = 255,
} lagfx_air_md_kind_t;

typedef struct {
    lagfx_air_md_kind_t kind;
    uint32_t            name_offset;   /* for NAMED_NODE */
    const uint32_t     *operands;
    uint32_t            num_operands;
} lagfx_air_metadata_t;

/* === Parameter-attribute group =================================== */

typedef struct {
    uint32_t            group_id;
    const uint32_t     *raw;            /* raw record operands; semantic decoding deferred */
    uint32_t            num_raw;
} lagfx_air_param_attr_group_t;

/* === Top-level module ============================================ */

typedef struct lagfx_air_module lagfx_air_module_t;

/* Module accessors (read-only views into the parsed module). */
const lagfx_air_type_t       *lagfx_air_module_types(const lagfx_air_module_t *m, uint32_t *count);
const lagfx_air_constant_t   *lagfx_air_module_constants(const lagfx_air_module_t *m, uint32_t *count);
const lagfx_air_function_t   *lagfx_air_module_functions(const lagfx_air_module_t *m, uint32_t *count);
const lagfx_air_metadata_t   *lagfx_air_module_metadata(const lagfx_air_module_t *m, uint32_t *count);
const lagfx_air_param_attr_group_t *lagfx_air_module_param_attr_groups(const lagfx_air_module_t *m, uint32_t *count);

/* Module-level strings. Returned pointer is into the module's arena;
 * valid until module_free. Returns NULL if not present in this module. */
const char *lagfx_air_module_triple(const lagfx_air_module_t *m);
const char *lagfx_air_module_datalayout(const lagfx_air_module_t *m);
const char *lagfx_air_module_source_filename(const lagfx_air_module_t *m);

/* Resolve a name_offset (returned in lagfx_air_function_t etc.) to a
 * NUL-terminated string in the module arena. Returns NULL on invalid
 * offset. */
const char *lagfx_air_module_string(const lagfx_air_module_t *m, uint32_t offset);

/* === Parse API =================================================== */

/* Parse the standard LLVM bitcode container portion of an AIR `.air.bc`
 * blob. The blob must start with the LLVM Bitcode Wrapper magic
 * (`0xDE 0xC0 0x17 0x0B` in little-endian read order).
 *
 * On success: *out_module is set to a freshly-allocated module struct;
 * caller owns it and must free via lagfx_air_module_free().
 *
 * On failure: *out_module is set to NULL and a LAGFX_ERR_* code is
 * returned. Common errors:
 *   - LAGFX_ERR_INVALID_ARG: NULL inputs or zero-length blob.
 *   - LAGFX_ERR_PROTOCOL: missing wrapper magic or truncated bitstream.
 *   - LAGFX_ERR_OUT_OF_MEMORY: arena allocation failed.
 *
 * NOTE: Phase 1 ignores FUNCTION_BLOCK body contents. Body bit-offsets
 * are recorded in `lagfx_air_function_t::body_offset` so that Phase 2
 * (the Apple-aware function decoder) can find them later.
 *
 * NOTE: This parser is INTENTIONALLY clean-room. It does not link
 * against libLLVM. It does not depend on Apple's LLVM fork. Format
 * details derived from the public LLVM Bitcode Format documentation
 * + empirical comparison against `llvm-bcanalyzer --dump` output for
 * our captured test fixtures.
 */
lagfx_status_t lagfx_air_module_open(const uint8_t      *blob,
                                     size_t              blob_len,
                                     lagfx_air_module_t **out_module);

/* Release a module returned by lagfx_air_module_open. Safe to call
 * with NULL. */
void lagfx_air_module_free(lagfx_air_module_t *module);

/* === Phase 2 — FUNCTION_BLOCK body decoder ======================== */

/* LLVM function-body instruction codes (FUNC_CODE_* in
 * llvm/Bitcode/LLVMBitCodes.h). Stored verbatim on each decoded
 * lagfx_air_inst_t so a translator can dispatch on them. Note that
 * LLVM keeps both legacy and modern variants for GEP/STORE/etc.:
 * the modern variants embed a type index and the legacy variants
 * infer it from pointer operands; both appear in real bitcode and
 * we surface both unmodified. */
typedef enum {
    LAGFX_AIR_INST_UNKNOWN       = 0,   /* sentinel for unrecognized codes */
    LAGFX_AIR_INST_DECLAREBLOCKS = 1,   /* block-count declaration */
    LAGFX_AIR_INST_BINOP         = 2,
    LAGFX_AIR_INST_CAST          = 3,
    LAGFX_AIR_INST_GEP_OLD       = 4,
    LAGFX_AIR_INST_SELECT        = 5,
    LAGFX_AIR_INST_EXTRACTELT    = 6,
    LAGFX_AIR_INST_INSERTELT     = 7,
    LAGFX_AIR_INST_SHUFFLEVEC    = 8,
    LAGFX_AIR_INST_CMP           = 9,
    LAGFX_AIR_INST_RET           = 10,
    LAGFX_AIR_INST_BR            = 11,
    LAGFX_AIR_INST_SWITCH        = 12,
    LAGFX_AIR_INST_UNREACHABLE   = 15,
    LAGFX_AIR_INST_PHI           = 16,
    LAGFX_AIR_INST_ALLOCA        = 19,
    LAGFX_AIR_INST_LOAD          = 20,
    LAGFX_AIR_INST_STORE_OLD     = 24,
    LAGFX_AIR_INST_EXTRACTVAL    = 26,
    LAGFX_AIR_INST_INSERTVAL     = 27,
    LAGFX_AIR_INST_CMP2          = 28,
    LAGFX_AIR_INST_VSELECT       = 29,
    LAGFX_AIR_INST_INDIRECTBR    = 31,
    LAGFX_AIR_INST_DEBUG_LOC_AGAIN = 33,
    LAGFX_AIR_INST_CALL          = 34,
    LAGFX_AIR_INST_DEBUG_LOC     = 35,
    LAGFX_AIR_INST_FENCE         = 36,
    LAGFX_AIR_INST_GEP           = 43,
    LAGFX_AIR_INST_STORE         = 44,
    LAGFX_AIR_INST_CMPXCHG       = 46,
    LAGFX_AIR_INST_UNOP          = 56,
} lagfx_air_inst_code_t;

/* One decoded instruction. The operand interpretation depends on
 * `code` and is the responsibility of the consumer (Phase 3 semantic
 * mapping). `ops` points into the function-body's own arena; valid
 * until lagfx_air_function_body_free. */
typedef struct {
    lagfx_air_inst_code_t code;
    uint32_t              raw_code;     /* original record code (in case it's a value we don't have an enum for) */
    const uint64_t       *ops;
    uint32_t              num_ops;
} lagfx_air_inst_t;

typedef struct lagfx_air_function_body lagfx_air_function_body_t;

/* Decode one function body. `fn_idx` must reference a non-prototype
 * function whose body_offset / body_length were stashed by
 * lagfx_air_module_open (Phase 2 step 1).
 *
 * On success: *out_body owns the decoded instruction stream; caller
 * must free with lagfx_air_function_body_free.
 *
 * Returns:
 *   LAGFX_OK             — decoded; check num_instructions
 *   LAGFX_ERR_INVALID_ARG— bad fn_idx or fn is a prototype
 *   LAGFX_ERR_PROTOCOL   — bitstream malformed or Apple-custom abbrev
 *                          we can't yet decode (Phase 2 evolves)
 */
lagfx_status_t lagfx_air_function_body_open(const lagfx_air_module_t   *module,
                                              uint32_t                   fn_idx,
                                              lagfx_air_function_body_t **out_body);

void lagfx_air_function_body_free(lagfx_air_function_body_t *body);

/* Number of basic blocks declared via the body's DECLAREBLOCKS
 * record (zero if absent). */
uint32_t lagfx_air_function_body_num_blocks(const lagfx_air_function_body_t *body);

/* Decoded instructions in emission order. The pointer is into the
 * body's arena; valid until lagfx_air_function_body_free. */
const lagfx_air_inst_t *lagfx_air_function_body_instructions(
    const lagfx_air_function_body_t *body, uint32_t *count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR_BITCODE_READER_H */
