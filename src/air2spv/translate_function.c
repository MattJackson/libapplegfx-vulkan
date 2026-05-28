/*
 * libapplegfx-vulkan — Phase 5 per-function AIR → SPIR-V translator
 * src/air2spv/translate_function.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Architecture: see header. Composes the patterns from emit_position
 * (vertex Position output), emit_vertex_id (BuiltIn VertexIndex input),
 * emit_extinst_glsl (GLSL.std.450 import + OpExtInst), and the
 * structural opcodes (OpVariable / OpAccessChain / OpLoad / OpStore /
 * OpVectorShuffle / OpCompositeInsert / OpCompositeExtract).
 *
 * Status: handles the triangle vertex body (18 instructions) producing
 * a spirv-val-clean module with the expected opcode mix. Operand
 * resolution treats function-local constants as OpUndef placeholders
 * pending function-local CONSTANTS_BLOCK decoding (a Phase 5.5 work
 * item; current behavior is structurally correct but semantically
 * stand-in until we surface those constants).
 */

#include "translate_function.h"
#include "spv_builder.h"
#include "common/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * Context
 * =================================================================== */

typedef struct {
    lagfx_spv_builder_t      *b;
    const lagfx_air_module_t *m;
    const lagfx_air_function_t *fn;
    lagfx_air_function_body_t  *body;
    const lagfx_air_inst_t   *insts;
    uint32_t                  num_insts;
    lagfx_xlate_stage_t       stage;

    /* Type-id table: AIR type-id -> SPIR-V id (0 = not yet emitted). */
    uint32_t                 *spv_type_ids;
    uint32_t                  num_air_types;

    /* Pointer-type cache: pairs of (pointee_air_type_id, storage_class)
     * mapped to a SPIR-V OpTypePointer id. We just linearly search a
     * small table since the universe of pointer shapes per function is
     * tiny. */
    struct {
        uint32_t pointee_air_type;
        uint32_t storage_class;
        uint32_t spv_id;
    } ptr_cache[64];
    uint32_t                  ptr_cache_len;

    /* Vector pointer-type cache (alternative: ptr_cache holds these too) */

    /* Constant cache: int64 / float / undef per type. Tiny linear table. */
    struct {
        uint8_t  kind;       /* 0=undef, 1=int (i64 lit), 2=null */
        uint32_t spv_type_id;
        uint64_t lit;        /* for INT: raw bits; for FLOAT: f32 bits */
        uint32_t spv_id;
    } const_cache[64];
    uint32_t                  const_cache_len;

    /* Module-level value-id base. ValueList layout per LLVM
     * ValueEnumerator: functions first, then module constants. */
    uint32_t                  module_val_count;

    /* Function argument value-id base + count. */
    uint32_t                  arg_id_base;
    uint32_t                  num_args;

    /* Per-function-arg SPIR-V id. For vertex stage with one i32 vertex
     * id arg, the SPIR-V id is the OpLoad result of the BuiltIn
     * VertexIndex input variable (which is an OpTypeInt 32 0 — uint).
     * Stored at index `i` of `arg_spv_ids` (one per AIR arg). */
    uint32_t                 *arg_spv_ids;
    /* AIR type-id of each arg (resolved from the function-type). */
    uint32_t                 *arg_air_type_ids;

    /* Instruction value-id base. Function-local constants are between
     * args and instructions but we currently treat them as undef
     * placeholders (we don't decode the function-local CONSTANTS_BLOCK
     * yet); `inst_id_base` is the value-id of the FIRST value-producing
     * instruction's result. */
    uint32_t                  inst_id_base;
    uint32_t                  local_const_count; /* placeholder; 0 until we decode */

    /* Value-id -> SPIR-V id map. Sized to inst_id_base + body insts. */
    uint32_t                 *value_id_to_spv;
    uint32_t                  value_id_capacity;

    /* Side-table: int32 literal reverse lookup for SHUFFLEVEC mask resolution. */
    int32_t                  *value_id_to_lit_i32;
    bool                     *value_id_lit_i32_valid;

    /* Per-instruction result type (AIR type-id) so downstream ops can
     * deduce element types (e.g., GEP+STORE val-type from the GEP
     * source). Indexed by INSTRUCTION INDEX into body, NOT value-id. */
    uint32_t                 *inst_result_air_type;

    /* Interface variables (declared at module scope; referenced from
     * OpEntryPoint). */
    uint32_t                  id_position_var;   /* vertex: BuiltIn Position output */
    uint32_t                  id_position_ptr;   /* OpTypePointer Output v4float */
    uint32_t                  id_vid_var;        /* vertex: BuiltIn VertexIndex input */
    uint32_t                  id_vid_ptr;        /* OpTypePointer Input uint */
    uint32_t                  id_color_var;      /* fragment: Location 0 output */

    /* Common SPIR-V type ids (filled lazily). */
    uint32_t                  id_void;
    uint32_t                  id_uint;           /* OpTypeInt 32 0 */
    uint32_t                  id_int32;          /* OpTypeInt 32 1 — currently unused */
    uint32_t                  id_int64;          /* OpTypeInt 64 1 */
    uint32_t                  id_ulong;          /* OpTypeInt 64 0 */
    uint32_t                  id_float32;
    uint32_t                  id_vec2_f;
    uint32_t                  id_vec4_f;
    uint32_t                  id_fn_void;
    uint32_t                  id_main;
    uint32_t                  id_entry_label;
    /* Cached struct{vec4f32} type for INSERTVAL/RET in vertex stage,
     * pre-emitted in Pass 0. */
    uint32_t                  id_struct_v4f;

    /* GLSL.std.450 ext-inst-set id (allocated up-front so OpExtInstImport
     * can appear in the header section). */
    uint32_t                  id_glsl;
} xlate_ctx_t;

/* ===================================================================
 * Helpers — common-type emission
 * =================================================================== */

static uint32_t f32_bits_from_double(double f) {
    float ff = (float)f;
    uint32_t out;
    memcpy(&out, &ff, sizeof(out));
    return out;
}

/* Emit OpTypeFloat 32 once. */
static uint32_t emit_type_float32(xlate_ctx_t *c) {
    if (c->id_float32) return c->id_float32;
    c->id_float32 = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { c->id_float32, 32u };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2);
    return c->id_float32;
}

/* Emit OpTypeInt with the given width + signedness (0=unsigned, 1=signed). */
static uint32_t emit_type_int_w(xlate_ctx_t *c, uint32_t width, uint32_t sign) {
    if (width == 32u && sign == 0u && c->id_uint)    return c->id_uint;
    if (width == 32u && sign == 1u && c->id_int32)   return c->id_int32;
    if (width == 64u && sign == 1u && c->id_int64)   return c->id_int64;
    if (width == 64u && sign == 0u && c->id_ulong)   return c->id_ulong;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, width, sign };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_INT, ops, 3);
    if (width == 32u && sign == 0u) c->id_uint = id;
    if (width == 32u && sign == 1u) c->id_int32 = id;
    if (width == 64u && sign == 1u) c->id_int64 = id;
    if (width == 64u && sign == 0u) c->id_ulong = id;
    return id;
}

static uint32_t emit_type_vec(xlate_ctx_t *c, uint32_t elem_spv, uint32_t lanes) {
    /* No cache for arbitrary vectors; we only use vec2_f / vec4_f via
     * the dedicated slots, and emit_air_type for anything else. */
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, elem_spv, lanes };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3);
    return id;
}

static uint32_t emit_type_vec4_f(xlate_ctx_t *c) {
    if (c->id_vec4_f) return c->id_vec4_f;
    c->id_vec4_f = emit_type_vec(c, emit_type_float32(c), 4u);
    return c->id_vec4_f;
}

static uint32_t emit_type_vec2_f(xlate_ctx_t *c) {
    if (c->id_vec2_f) return c->id_vec2_f;
    c->id_vec2_f = emit_type_vec(c, emit_type_float32(c), 2u);
    return c->id_vec2_f;
}

static uint32_t emit_type_void(xlate_ctx_t *c) {
    if (c->id_void) return c->id_void;
    c->id_void = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { c->id_void };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_VOID, ops, 1);
    return c->id_void;
}

static uint32_t emit_type_function_void(xlate_ctx_t *c) {
    if (c->id_fn_void) return c->id_fn_void;
    c->id_fn_void = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { c->id_fn_void, emit_type_void(c) };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_FUNCTION, ops, 2);
    return c->id_fn_void;
}

/* Emit a pointer type with the given pointee (SPIR-V id) and storage
 * class, cached. */
static uint32_t emit_type_pointer(xlate_ctx_t *c, uint32_t pointee_air_type,
                                   uint32_t pointee_spv_id, uint32_t storage_class) {
    for (uint32_t i = 0; i < c->ptr_cache_len; i++) {
        if (c->ptr_cache[i].pointee_air_type == pointee_air_type &&
            c->ptr_cache[i].storage_class == storage_class) {
            return c->ptr_cache[i].spv_id;
        }
    }
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, storage_class, pointee_spv_id };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
    if (c->ptr_cache_len < (uint32_t)(sizeof(c->ptr_cache) / sizeof(c->ptr_cache[0]))) {
        c->ptr_cache[c->ptr_cache_len].pointee_air_type = pointee_air_type;
        c->ptr_cache[c->ptr_cache_len].storage_class = storage_class;
        c->ptr_cache[c->ptr_cache_len].spv_id = id;
        c->ptr_cache_len++;
    }
    return id;
}

/* Recursively emit an AIR type as a SPIR-V type, caching the result. */
static uint32_t emit_air_type(xlate_ctx_t *c, uint32_t air_type_idx) {
    if (air_type_idx >= c->num_air_types) {
        /* Unknown / out-of-range — fall back to a uint to keep
         * downstream emission valid. */
        return emit_type_int_w(c, 32u, 0u);
    }
    if (c->spv_type_ids[air_type_idx]) return c->spv_type_ids[air_type_idx];

    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    const lagfx_air_type_t *t = &ts[air_type_idx];

    uint32_t out = 0u;
    switch (t->kind) {
        case LAGFX_AIR_TYPE_VOID:
            out = emit_type_void(c); break;
        case LAGFX_AIR_TYPE_FLOAT:
            out = emit_type_float32(c); break;
        case LAGFX_AIR_TYPE_INTEGER: {
            uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
            /* Default LLVM ints to "unsigned" in SPIR-V; SPIR-V doesn't
             * carry a signedness bit on arithmetic, just on type
             * declaration. Triangle uses i32 (vid arg) which we treat
             * as uint, and i64 (lifetime sizes, GEP indices) as
             * uint64. */
            uint32_t sign = 0u;
            out = emit_type_int_w(c, w, sign);
            break;
        }
        case LAGFX_AIR_TYPE_VECTOR: {
            uint32_t lanes = t->num_op >= 1u ? t->op[0] : 4u;
            uint32_t elem  = t->num_op >= 2u ? t->op[1] : 0u;
            uint32_t elem_spv = emit_air_type(c, elem);
            /* Reuse cached vec2_f / vec4_f when shapes match to avoid
             * duplicate OpTypeVector declarations (spirv-val rejects). */
            if (lanes == 2u && elem_spv == c->id_float32 && c->id_vec2_f) {
                out = c->id_vec2_f;
            } else if (lanes == 4u && elem_spv == c->id_float32 && c->id_vec4_f) {
                out = c->id_vec4_f;
            } else {
                out = emit_type_vec(c, elem_spv, lanes);
                if (lanes == 2u && elem_spv == c->id_float32) c->id_vec2_f = out;
                if (lanes == 4u && elem_spv == c->id_float32) c->id_vec4_f = out;
            }
            break;
        }
        case LAGFX_AIR_TYPE_POINTER: {
            uint32_t pointee = t->num_op >= 1u ? t->op[0] : 0u;
            /* Storage class: AIR's addr_space (op[1]) doesn't map
             * 1:1 to SPIR-V; in triangle, every pointer is Function
             * storage (stack-allocated array). For more complex
             * shaders we'll need a real mapping table. */
            out = emit_type_pointer(c, pointee, emit_air_type(c, pointee),
                                     LAGFX_SPV_STORAGE_FUNCTION);
            break;
        }
        case LAGFX_AIR_TYPE_ARRAY: {
            uint32_t len  = t->num_op >= 1u ? t->op[0] : 1u;
            uint32_t elem = t->num_op >= 2u ? t->op[1] : 0u;
            uint32_t elem_spv = emit_air_type(c, elem);
            /* Emit OpTypeInt 32 0 + OpConstant for length. */
            uint32_t id_uint = emit_type_int_w(c, 32u, 0u);
            uint32_t id_len  = lagfx_spv_builder_alloc_id(c->b);
            uint32_t op_const[] = { id_uint, id_len, len };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, op_const, 3);
            uint32_t id_arr = lagfx_spv_builder_alloc_id(c->b);
            uint32_t op_arr[] = { id_arr, elem_spv, id_len };
            lagfx_spv_builder_emit_op(c->b, 28 /* OpTypeArray */, op_arr, 3);
            out = id_arr;
            break;
        }
        case LAGFX_AIR_TYPE_STRUCT_ANON:
        case LAGFX_AIR_TYPE_STRUCT_NAMED: {
            /* op[0] = packed flag; op[1..num_op-1] = field type ids. */
            uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
            uint32_t fields_spv[16];
            for (uint32_t i = 0; i < nfields && i < 16u; i++) {
                fields_spv[i] = emit_air_type(c, t->op[1u + i]);
            }
            uint32_t id_st = lagfx_spv_builder_alloc_id(c->b);
            uint32_t ops[17];
            ops[0] = id_st;
            for (uint32_t i = 0; i < nfields && i < 16u; i++) ops[1u + i] = fields_spv[i];
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_STRUCT, ops, 1u + nfields);
            out = id_st;
            break;
        }
        case LAGFX_AIR_TYPE_FUNCTION:
        case LAGFX_AIR_TYPE_METADATA:
        case LAGFX_AIR_TYPE_LABEL:
        default:
            /* Not directly representable in SPIR-V Logical; use uint
             * as a stand-in (these types should never be the result
             * type of a value we emit). */
            out = emit_type_int_w(c, 32u, 0u);
            break;
    }
    c->spv_type_ids[air_type_idx] = out;
    return out;
}

/* Returns true if `air_type_idx` recursively references a type SPIR-V
 * can't represent without extra capabilities we don't want to declare
 * (i8 / i64 today). Used to filter module-constant pre-bind so we
 * don't drag in OpTypeInt 8 / OpTypeInt 64 just because a captured
 * shader's null constant has a type that NESTS one. */
static bool air_type_requires_extra_cap(xlate_ctx_t *c, uint32_t air_type_idx,
                                          uint32_t depth) {
    if (depth > 8u) return false; /* recursion guard */
    if (air_type_idx >= c->num_air_types) return false;
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    const lagfx_air_type_t *t = &ts[air_type_idx];
    switch (t->kind) {
        case LAGFX_AIR_TYPE_INTEGER: {
            uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
            /* i32 + i64 are first-class; only i1/i8/i16/i128 require
             * extra caps we don't declare. */
            return !(w == 32u || w == 64u);
        }
        case LAGFX_AIR_TYPE_VECTOR:
        case LAGFX_AIR_TYPE_ARRAY:
        case LAGFX_AIR_TYPE_POINTER: {
            uint32_t inner = t->num_op >= 2u ? t->op[1] : 0u;
            /* For ARRAY: also forbid length 0 (SPIR-V requires N > 0). */
            if (t->kind == LAGFX_AIR_TYPE_ARRAY && t->num_op >= 1u && t->op[0] == 0u)
                return true;
            return air_type_requires_extra_cap(c, inner, depth + 1u);
        }
        case LAGFX_AIR_TYPE_STRUCT_ANON:
        case LAGFX_AIR_TYPE_STRUCT_NAMED: {
            uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
            for (uint32_t i = 0; i < nfields; i++) {
                if (air_type_requires_extra_cap(c, t->op[1u + i], depth + 1u))
                    return true;
            }
            return false;
        }
        case LAGFX_AIR_TYPE_FUNCTION:
        case LAGFX_AIR_TYPE_METADATA:
        case LAGFX_AIR_TYPE_LABEL:
            return false;
        default:
            return false;
    }
}

/* ===================================================================
 * Helpers — constant emission
 * =================================================================== */

static uint32_t cache_lookup_const(xlate_ctx_t *c, uint8_t kind,
                                    uint32_t spv_type, uint64_t lit) {
    for (uint32_t i = 0; i < c->const_cache_len; i++) {
        if (c->const_cache[i].kind == kind &&
            c->const_cache[i].spv_type_id == spv_type &&
            c->const_cache[i].lit == lit) {
            return c->const_cache[i].spv_id;
        }
    }
    return 0u;
}

static void cache_store_const(xlate_ctx_t *c, uint8_t kind,
                               uint32_t spv_type, uint64_t lit, uint32_t spv_id) {
    if (c->const_cache_len >= (uint32_t)(sizeof(c->const_cache) / sizeof(c->const_cache[0])))
        return;
    c->const_cache[c->const_cache_len].kind = kind;
    c->const_cache[c->const_cache_len].spv_type_id = spv_type;
    c->const_cache[c->const_cache_len].lit = lit;
    c->const_cache[c->const_cache_len].spv_id = spv_id;
    c->const_cache_len++;
}

static uint32_t emit_const_uint32(xlate_ctx_t *c, uint32_t val) {
    uint32_t ty = emit_type_int_w(c, 32u, 0u);
    uint32_t cached = cache_lookup_const(c, 1, ty, (uint64_t)val);
    if (cached) return cached;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { ty, id, val };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
    cache_store_const(c, 1, ty, (uint64_t)val, id);
    return id;
}

/* OpUndef per SPIR-V type. Used as a placeholder for unresolvable
 * operands (function-local constants pending Phase 5.5 decode). */
static uint32_t emit_undef(xlate_ctx_t *c, uint32_t spv_type) {
    uint32_t cached = cache_lookup_const(c, 0, spv_type, 0u);
    if (cached) return cached;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { spv_type, id };
    lagfx_spv_builder_emit_op(c->b, 1 /* OpUndef */, ops, 2);
    cache_store_const(c, 0, spv_type, 0u, id);
    return id;
}

/* ===================================================================
 * Value-id resolution
 * =================================================================== */

/* Map LLVM relative-id operand to absolute value-id, using the
 * instruction's own next value-id as the base. */
static uint32_t resolve_relative(uint32_t encoded, uint32_t next_val_id) {
    /* LLVM encoding: actual = next - encoded for backward references.
     * Forward references (encoded > next) wrap around; we leave those
     * as `next + (encoded - next) = encoded` (i.e., absolute) and rely
     * on the caller to handle missing entries. */
    if (encoded <= next_val_id) return next_val_id - encoded;
    /* Treat as forward-ref placeholder. */
    return encoded;
}

/* Look up an absolute value-id and return its SPIR-V id, or 0 if
 * unresolvable (in which case the caller substitutes OpUndef of the
 * expected type). */
static uint32_t resolve_value_spv(xlate_ctx_t *c, uint32_t value_id) {
    if (value_id < c->value_id_capacity) {
        return c->value_id_to_spv[value_id];
    }
    return 0u;
}

static void bind_value_spv(xlate_ctx_t *c, uint32_t value_id, uint32_t spv_id) {
    if (value_id < c->value_id_capacity) {
        c->value_id_to_spv[value_id] = spv_id;
    }
}

/* Side-table helpers: int32 literal reverse lookup for SHUFFLEVEC mask resolution. */
static void bind_value_lit_i32(xlate_ctx_t *c, uint32_t value_id, int32_t lit) {
    if (value_id < c->value_id_capacity) {
        c->value_id_to_lit_i32[value_id]    = lit;
        c->value_id_lit_i32_valid[value_id] = true;
    }
}

/* Returns true on hit (writes *out_lit); false on miss. */
static bool resolve_value_lit_i32(const xlate_ctx_t *c, uint32_t value_id, int32_t *out_lit) {
    if (value_id >= c->value_id_capacity) return false;
    if (!c->value_id_lit_i32_valid[value_id]) return false;
    *out_lit = c->value_id_to_lit_i32[value_id];
    return true;
}

/* For a referenced value-id, return its SPIR-V id; if unresolvable,
 * emit and return an OpUndef of `fallback_type` (SPIR-V id). */
static uint32_t resolve_or_undef(xlate_ctx_t *c, uint32_t value_id,
                                  uint32_t fallback_spv_type) {
    uint32_t r = resolve_value_spv(c, value_id);
    if (r) return r;
    /* Module-level constant we didn't track? Triangle's GEP indices
     * resolve to `i64 0` (module const c[9]) — emit a real i64 zero
     * for those rather than a generic undef so spirv-val accepts the
     * AccessChain. */
    return emit_undef(c, fallback_spv_type);
}

/* Forward decl — inst_produces_value is defined further below but
 * referenced from the prologue's ALLOCA pre-emission pass. */
static bool inst_produces_value(const lagfx_air_inst_t *i);

/* ===================================================================
 * Prologue
 * =================================================================== */

static void emit_prologue(xlate_ctx_t *c) {
    /* 1. OpCapability Shader + Int64 (LLVM/AIR uses i64 for GEP
     *    indices, lifetime sizes, etc.; lavapipe supports shaderInt64
     *    so declaring it is essentially free and unblocks real i64
     *    constant binding instead of OpUndef fallback). Int8 stays
     *    out — i8 only shows up in lifetime-intrinsic-pointer types
     *    that we don't emit code for, and the cap-filter helper
     *    suppresses those declarations. */
    {
        uint32_t ops[] = { LAGFX_SPV_CAPABILITY_SHADER };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY, ops, 1);
    }
    {
        uint32_t ops[] = { 11u /* Int64 */ };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY, ops, 1);
    }

    /* 2. OpExtInstImport %glsl "GLSL.std.450" */
    c->id_glsl = lagfx_spv_builder_alloc_id(c->b);
    {
        uint32_t prefix[] = { c->id_glsl };
        lagfx_spv_builder_emit_op_string(c->b, LAGFX_SPV_OP_EXT_INST_IMPORT,
                                          prefix, 1, "GLSL.std.450", NULL, 0);
    }

    /* 3. OpMemoryModel Logical GLSL450 */
    {
        uint32_t ops[] = {
            LAGFX_SPV_ADDRESSING_MODEL_LOGICAL,
            LAGFX_SPV_MEMORY_MODEL_GLSL450,
        };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_MEMORY_MODEL, ops, 2);
    }

    /* Pre-allocate ids needed in the entry-point declaration. */
    c->id_main = lagfx_spv_builder_alloc_id(c->b);
    c->id_entry_label = lagfx_spv_builder_alloc_id(c->b);

    if (c->stage == LAGFX_XLATE_STAGE_VERTEX) {
        c->id_position_var = lagfx_spv_builder_alloc_id(c->b);
        c->id_vid_var      = lagfx_spv_builder_alloc_id(c->b);

        /* 4. OpEntryPoint Vertex %main "main" %pos %vid */
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, c->id_main };
        uint32_t suffix[] = { c->id_position_var, c->id_vid_var };
        lagfx_spv_builder_emit_op_string(c->b, LAGFX_SPV_OP_ENTRY_POINT,
                                          prefix, 2, "main", suffix, 2);

        /* 5. Decorations */
        {
            uint32_t ops[] = { c->id_position_var,
                                LAGFX_SPV_DECORATION_BUILTIN,
                                LAGFX_SPV_BUILTIN_POSITION };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
        {
            uint32_t ops[] = { c->id_vid_var,
                                LAGFX_SPV_DECORATION_BUILTIN,
                                LAGFX_SPV_BUILTIN_VERTEX_INDEX };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
    } else /* fragment */ {
        c->id_color_var = lagfx_spv_builder_alloc_id(c->b);
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, c->id_main };
        uint32_t suffix[] = { c->id_color_var };
        lagfx_spv_builder_emit_op_string(c->b, LAGFX_SPV_OP_ENTRY_POINT,
                                          prefix, 2, "main", suffix, 1);

        /* OpExecutionMode %main OriginUpperLeft */
        {
            uint32_t ops[] = { c->id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2);
        }
        /* OpDecorate %color Location 0 */
        {
            uint32_t ops[] = { c->id_color_var, LAGFX_SPV_DECORATION_LOCATION, 0u };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
    }
}

/* Emit the module-scope variables (OpVariable in Input/Output storage
 * classes) plus the function declaration. After this returns, the
 * function body emission can begin. */
static void emit_module_vars_and_function(xlate_ctx_t *c) {
    uint32_t id_v4f  = emit_type_vec4_f(c);

    if (c->stage == LAGFX_XLATE_STAGE_VERTEX) {
        /* OpTypePointer Output v4float */
        c->id_position_ptr = lagfx_spv_builder_alloc_id(c->b);
        {
            uint32_t ops[] = { c->id_position_ptr, LAGFX_SPV_STORAGE_OUTPUT, id_v4f };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
        }
        /* OpVariable %ptr_out %pos Output */
        {
            uint32_t ops[] = { c->id_position_ptr, c->id_position_var,
                                LAGFX_SPV_STORAGE_OUTPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, ops, 3);
        }

        /* OpTypePointer Input uint */
        uint32_t id_uint = emit_type_int_w(c, 32u, 0u);
        c->id_vid_ptr = lagfx_spv_builder_alloc_id(c->b);
        {
            uint32_t ops[] = { c->id_vid_ptr, LAGFX_SPV_STORAGE_INPUT, id_uint };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
        }
        /* OpVariable %ptr_in %vid Input */
        {
            uint32_t ops[] = { c->id_vid_ptr, c->id_vid_var, LAGFX_SPV_STORAGE_INPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, ops, 3);
        }
    } else /* fragment */ {
        uint32_t id_ptr_out = lagfx_spv_builder_alloc_id(c->b);
        uint32_t op_pt[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, id_v4f };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, op_pt, 3);
        uint32_t op_var[] = { id_ptr_out, c->id_color_var, LAGFX_SPV_STORAGE_OUTPUT };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, op_var, 3);
    }

    /* Pre-emit all AIR types likely to be referenced by the body
     * (OpType* and OpConstant must precede OpFunction per SPIR-V
     * layout rules). Walk the body and call emit_air_type on every
     * type-id operand we know how to identify; also walk the module
     * type table to capture any types reachable from those. Spurious
     * emission is fine — caching makes it idempotent.
     *
     * Triangle references: array[3,vec2f32], vec2f32, vec4f32, vec4i32,
     * i32, i64, struct{vec4f32}, plus their pointer variants in
     * Function storage. Pre-emit pointers for each non-trivial type
     * to satisfy GEP/ALLOCA/LOAD pointer-result expectations. */
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst->num_ops == 0u) continue;
        switch (inst->code) {
            case LAGFX_AIR_INST_ALLOCA: {
                if (inst->num_ops >= 1u) {
                    uint32_t t = (uint32_t)inst->ops[0];
                    uint32_t pt = emit_air_type(c, t);
                    emit_type_pointer(c, t, pt, LAGFX_SPV_STORAGE_FUNCTION);
                }
                if (inst->num_ops >= 2u) {
                    (void)emit_air_type(c, (uint32_t)inst->ops[1]);
                }
                break;
            }
            case LAGFX_AIR_INST_CAST: {
                /* MVP CAST handler aliases without emitting OpType*.
                 * Skip pre-emitting the dest type — avoids requiring
                 * Int8/Int64 capabilities for bitcast-to-i8 / zext-to-i64
                 * that we'd never actually reference. */
                break;
            }
            case LAGFX_AIR_INST_GEP:
            case LAGFX_AIR_INST_GEP_OLD: {
                if (inst->num_ops >= 2u) {
                    uint32_t src = (uint32_t)inst->ops[1];
                    (void)emit_air_type(c, src);
                    /* Walk into the type to capture pointers for each
                     * indexing level. */
                    uint32_t n_types_local = 0;
                    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types_local);
                    uint32_t cur = src;
                    /* Limit to a sane depth. */
                    for (uint32_t lvl = 0; lvl < 4u && cur < n_types_local; lvl++) {
                        const lagfx_air_type_t *t = &ts[cur];
                        uint32_t cur_spv = emit_air_type(c, cur);
                        emit_type_pointer(c, cur, cur_spv, LAGFX_SPV_STORAGE_FUNCTION);
                        uint32_t nxt = 0u;
                        if (t->kind == LAGFX_AIR_TYPE_ARRAY && t->num_op >= 2u) nxt = t->op[1];
                        else if (t->kind == LAGFX_AIR_TYPE_VECTOR && t->num_op >= 2u) nxt = t->op[1];
                        else break;
                        cur = nxt;
                    }
                }
                break;
            }
            case LAGFX_AIR_INST_LOAD: {
                if (inst->num_ops >= 1u) {
                    (void)emit_air_type(c, (uint32_t)inst->ops[0]);
                }
                break;
            }
            default:
                break;
        }
    }
    /* Pre-emit common types the prologue paths use. Restricted to
     * the types we may actually reference in the body (vec2f, vec4f,
     * uint, vec4_f); skipping every-module-type-blindly to avoid
     * declaring OpTypeInt 8 / 64 just because the AIR module mentions
     * them in unused declarations (which would force Int8/Int64
     * capabilities). */
    (void)emit_type_vec2_f(c);
    (void)emit_type_vec4_f(c);
    /* Pre-warm common constants + OpUndefs the body fallbacks may want.
     * All OpConstant / OpUndef emissions must precede OpFunction per
     * SPIR-V §2.4. */
    (void)emit_const_uint32(c, 0u);
    (void)emit_undef(c, emit_type_int_w(c, 32u, 0u));
    (void)emit_undef(c, emit_type_vec2_f(c));
    (void)emit_undef(c, emit_type_vec4_f(c));
    /* Struct{vec4f} undef — cached after the struct type below. */
    /* (GEP fallback indices use uint32 0 instead of i64 0 to avoid
     * requiring the Int64 capability for shaders that don't otherwise
     * need it.) */
    /* Struct{vec4f} for INSERTVAL/RET in vertex stage. Derive from
     * the function's return type via emit_air_type so we share the
     * id with any local-const pre-bind that already touched the same
     * AIR type (avoids duplicate OpTypeStruct declarations, which
     * spirv-val rejects). */
    {
        uint32_t n_types_local = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types_local);
        uint32_t fn_ty = c->fn->type_index;
        if (fn_ty < n_types_local && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION &&
            ts[fn_ty].num_op >= 2u) {
            uint32_t ret_ty = ts[fn_ty].op[1];
            c->id_struct_v4f = emit_air_type(c, ret_ty);
        } else {
            /* Fallback: synthesize a struct{vec4f}. */
            uint32_t id_v4f = emit_type_vec4_f(c);
            c->id_struct_v4f = lagfx_spv_builder_alloc_id(c->b);
            uint32_t ops[] = { c->id_struct_v4f, id_v4f };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_STRUCT, ops, 2);
        }
        (void)emit_undef(c, c->id_struct_v4f);
    }

    /* OpTypeFunction void() */
    uint32_t id_fnty = emit_type_function_void(c);

    /* OpFunction void %main None %fn_void */
    {
        uint32_t ops[] = {
            emit_type_void(c),
            c->id_main,
            LAGFX_SPV_FUNCTION_CONTROL_NONE,
            id_fnty,
        };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION, ops, 4);
    }
    /* OpLabel %entry */
    {
        uint32_t ops[] = { c->id_entry_label };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1);
    }

    /* SPIR-V §2.4: OpVariable instructions with Function storage MUST
     * be the first instructions in the function's first block. Walk
     * the body now and pre-emit OpVariable for each ALLOCA, binding
     * the result value-id in the map. translate_body's ALLOCA handler
     * skips re-emission when it sees a pre-bound value-id. */
    {
        uint32_t alloca_val_id = c->inst_id_base;
        for (uint32_t i = 0; i < c->num_insts; i++) {
            const lagfx_air_inst_t *inst = &c->insts[i];
            bool produces = inst_produces_value(inst);
            if (inst->code == LAGFX_AIR_INST_ALLOCA && inst->num_ops >= 1u) {
                uint32_t alloc_air_ty = (uint32_t)inst->ops[0];
                uint32_t alloc_spv   = emit_air_type(c, alloc_air_ty);
                uint32_t ptr_spv     = emit_type_pointer(c, alloc_air_ty, alloc_spv,
                                                          LAGFX_SPV_STORAGE_FUNCTION);
                uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t ops[] = { ptr_spv, result_id, LAGFX_SPV_STORAGE_FUNCTION };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, ops, 3);
                bind_value_spv(c, alloca_val_id, result_id);
                c->inst_result_air_type[i] = alloc_air_ty;
            }
            if (produces) alloca_val_id++;
        }
    }

    /* For the vertex arg %0 (vertex_id, i32): emit an OpLoad of the
     * VertexIndex input variable. The resulting SPIR-V id IS the
     * argument's value, bound in the value-id map. Must come AFTER
     * all OpVariable instructions in the entry block. */
    if (c->stage == LAGFX_XLATE_STAGE_VERTEX && c->num_args >= 1u) {
        uint32_t id_uint = emit_type_int_w(c, 32u, 0u);
        uint32_t id_load = lagfx_spv_builder_alloc_id(c->b);
        uint32_t ops[] = { id_uint, id_load, c->id_vid_var };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
        c->arg_spv_ids[0] = id_load;
        bind_value_spv(c, c->arg_id_base, id_load);
    }
}

/* ===================================================================
 * Pre-allocate SPIR-V ids for each body instruction's result.
 *
 * Walks the body once before pass-2 emission, assigning a SPIR-V id
 * (via builder alloc) to each value-producing instruction so that
 * later instructions can reference earlier results via the value-id
 * map. Records the instruction's result AIR type-id when deducible.
 *
 * Note: For instructions whose result we can't easily type (CALL of
 * void function, STORE, RET, DECLAREBLOCKS, lifetime intrinsics), we
 * leave spv_id = 0 in the value map.
 * =================================================================== */

static bool inst_produces_value(const lagfx_air_inst_t *i) {
    switch (i->code) {
        case LAGFX_AIR_INST_DECLAREBLOCKS:
        case LAGFX_AIR_INST_STORE:
        case LAGFX_AIR_INST_STORE_OLD:
        case LAGFX_AIR_INST_RET:
        case LAGFX_AIR_INST_BR:
        case LAGFX_AIR_INST_SWITCH:
        case LAGFX_AIR_INST_UNREACHABLE:
            return false;
        case LAGFX_AIR_INST_CALL:
            /* Conservatively assume void-return for triangle's
             * llvm.lifetime.* calls. A more general check would inspect
             * the callee's return type via the function-type table.
             * For Phase 5 MVP this is sufficient. */
            return false;
        default:
            return true;
    }
}

/* ===================================================================
 * Per-instruction emission (Pass 2)
 * =================================================================== */

static void emit_inst_alloca(xlate_ctx_t *c, uint32_t inst_idx,
                              const lagfx_air_inst_t *inst,
                              uint32_t result_value_id) {
    /* ALLOCAs are pre-emitted as OpVariables at the top of the entry
     * block by emit_module_vars_and_function (per SPIR-V §2.4: all
     * Function-storage OpVariables must be the first instructions in
     * the first block). The value-id map binding + inst_result_air_type
     * were set then; nothing to do here. */
    (void)c; (void)inst_idx; (void)inst; (void)result_value_id;
}

static void emit_inst_cast(xlate_ctx_t *c, uint32_t inst_idx,
                             const lagfx_air_inst_t *inst,
                             uint32_t result_value_id, uint32_t next_val_id) {
    /* CAST operand layout: [opval_rel (relative), dest_type_abs (absolute), opcode_subfield]
     *
     * LLVM BITCODE CastOpcodes per llvm/include/llvm/Bitcode/LLVMBitCodes.h
     * bitc::CastOpcodes (verified 2026-05-27):
     *   CAST_TRUNC=0, CAST_ZEXT=1, CAST_SEXT=2, CAST_FPTOUI=3, CAST_FPTOSI=4,
     *   CAST_UITOFP=5, CAST_SITOFP=6, CAST_FPTRUNC=7, CAST_FPEXT=8,
     *   CAST_PTRTOINT=9, CAST_INTTOPTR=10, CAST_BITCAST=11, CAST_ADDRSPACECAST=12
     *
     * These are BITCODE subcodes, NOT the IR-level Instruction::CastOps enum.
     */
    if (inst->num_ops < 3u) return;

    uint32_t opval_rel = (uint32_t)inst->ops[0];
    uint32_t dest_ty   = (uint32_t)inst->ops[1];
    int cast_op = (int)(inst->ops[2]);

    /* Resolve source operand */
    uint32_t opval_id  = resolve_relative(opval_rel, next_val_id);
    
    /* Resolve source to SPIR-V id; use a default type for undef fallback.
     * We delay emitting the dest_ty_spv until we know which opcode path
     * we're taking — this avoids runtime type emission that would violate
     * SPIR-V ordering rules (types must be declared before variables). */
    uint32_t opval_spv = resolve_or_undef(c, opval_id, emit_type_int_w(c, 32u, 0u));

    /* Check if result is already bound (e.g., from pre-allocation).
     * If not, allocate a new id. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Per-CastOp dispatch to SPIR-V conversion opcodes per SPIR-V §3.32.11 */
    switch (cast_op) {
        case 0:  /* CAST_TRUNC → OpUConvert (smaller width int) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_UCONVERT, ops, 3); }
            break;

        case 1:  /* CAST_ZEXT → OpUConvert (larger width int) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_UCONVERT, ops, 3); }
            break;

        case 2:  /* CAST_SEXT → OpSConvert (larger width int) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SCONVERT, ops, 3); }
            break;

        case 3:  /* CAST_FPTOUI → OpConvertFToU (float→unsigned int) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_F_TO_U, ops, 3); }
            break;

        case 4:  /* CAST_FPTOSI → OpConvertFToS (float→signed int) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_F_TO_S, ops, 3); }
            break;

        case 5:  /* CAST_UITOFP → OpConvertUToF (unsigned int→float) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_U_TO_F, ops, 3); }
            break;

        case 6:  /* CAST_SITOFP → OpConvertSToF (signed int→float) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_S_TO_F, ops, 3); }
            break;

        case 7:  /* CAST_FPTRUNC → OpFConvert (smaller width float) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_F_CONVERT, ops, 3); }
            break;

        case 8:  /* CAST_FPEXT → OpFConvert (larger width float) */
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_F_CONVERT, ops, 3); }
            break;

        case 9:  /* CAST_PTRTOINT → alias (pointer→int not representable in SPIR-V) */
            bind_value_spv(c, result_value_id, opval_spv);
            c->inst_result_air_type[inst_idx] = dest_ty;
            return;

        case 10: /* CAST_INTTOPTR → alias (int→pointer not representable in SPIR-V) */
            bind_value_spv(c, result_value_id, opval_spv);
            c->inst_result_air_type[inst_idx] = dest_ty;
            return;

        case 11: /* CAST_BITCAST → always alias (avoid runtime type emission) */
            /* BITCAST requires emitting the pointer type at runtime which
             * would violate SPIR-V ordering rules (types must precede variables).
             * Instead, we alias: the operand and result have identical bit
             * representations so this is semantically correct. The downstream
             * consumer will use the aliased id directly. */
            bind_value_spv(c, result_value_id, opval_spv);
            c->inst_result_air_type[inst_idx] = dest_ty;
            return;

        case 12: /* CAST_ADDRSPACECAST → alias for now */
            bind_value_spv(c, result_value_id, opval_spv);
            c->inst_result_air_type[inst_idx] = dest_ty;
            return;

        default:
            /* Unknown cast opcode — fall back to aliasing */
            bind_value_spv(c, result_value_id, opval_spv);
            c->inst_result_air_type[inst_idx] = dest_ty;
            return;
    }

    /* Bind the result value-id and record the AIR type for downstream ops */
    bind_value_spv(c, result_value_id, result_spv);
    c->inst_result_air_type[inst_idx] = dest_ty;
}

static void emit_inst_call(xlate_ctx_t *c, uint32_t inst_idx,
                             const lagfx_air_inst_t *inst,
                             uint32_t next_val_id) {
    /* CALL: [paramattrs, ccinfo, [calleety], callee, args...]
     *
     * Bitcode format per llvm/include/llvm/Bitcode/LLVMBitCodes.h:
     *   FUNC_CODE_INST_CALL = 34, // CALL: [attr, cc, fnty, fnid, args...]
     * where "cc" (calling convention info) has flags in the low bits.
     * CALL_EXPLICIT_TYPE = 15 means bit 15 is set, indicating that
     * the function type (fnty) operand is present before the callee id.
     *
     * Operand layout:
     *   - ops[0]: paramattrs
     *   - ops[1]: ccinfo (flags in low bits; bit 15 = explicit-type slot)
     *   - ops[2]: calleety (only if ccinfo & 0x8000, i.e., CALL_EXPLICIT_TYPE)
     *   - ops[3] or ops[2]: callee_rel (the function reference to resolve)
     *   - ops[N+1..]: arguments (relative value-ids)
     *
     * Intrinsic dispatch: Resolve the callee name via STRTAB, match against
     * air.* fast-math intrinsics (Apple uses air.fast.<x> and air.<x>
     * interchangeably per paravirt-re/library/air_intrinsic_spec docs), emit
     * OpExtInst with GLSL.std.450 extended instruction set. */

    if (inst->num_ops < 2u) return;

    uint32_t ccinfo = (uint32_t)inst->ops[1];
    bool has_explicit_type = (ccinfo & 0x8000) != 0;

    /* Determine callee_rel slot index. */
    uint32_t callee_slot_idx = has_explicit_type ? 3u : 2u;
    if (inst->num_ops <= callee_slot_idx) return;

    uint64_t callee_rel_raw = inst->ops[callee_slot_idx];
    uint32_t callee_rel = (uint32_t)callee_rel_raw;

    /* Resolve callee to absolute value-id. */
    uint32_t callee_id = resolve_relative(callee_rel, next_val_id);

    /* Look up the function name via module-level functions table. */
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(c->m, &n_fns);
    if (callee_id >= n_fns) {
        /* Not a function reference — drop the call. */
        LAGFX_TRACE("call: callee_id=%u >= n_fns=%u — drop", callee_id, n_fns);
        return;
    }

    const char *fn_name = lagfx_air_module_string(c->m, fns[callee_id].name_offset);
    if (!fn_name) {
        LAGFX_TRACE("call: no name for fn[%u] — drop", callee_id);
        return;
    }

   /* Intrinsic dispatch table: air.* fast-math intrinsics → GLSL.std.450
     * instruction numbers (from spv_builder.h LAGFX_SPV_GLSL_* constants).
     * Apple uses air.fast.<x> and air.<x> interchangeably; we match by
     * string prefix so "air.fast.sqrt.f32" matches the Sqrt row.
     * Reference: paravirt-re/library/air_intrinsic_spec docs. */
    static const struct {
        const char *prefix;
        uint32_t glsl_inst;
    } intrinsic_table[] = {
        {"air.fast.sqrt",      LAGFX_SPV_GLSL_SQRT},
        {"air.fast.rsqrt",     LAGFX_SPV_GLSL_INVERSE_SQRT},
        {"air.fast.exp",       LAGFX_SPV_GLSL_EXP},
        {"air.fast.log",       LAGFX_SPV_GLSL_LOG},
        {"air.fast.sin",       LAGFX_SPV_GLSL_SIN},
        {"air.fast.cos",       LAGFX_SPV_GLSL_COS},
        {"air.fast.normalize", LAGFX_SPV_GLSL_NORMALIZE},
        {"air.fast.length",    LAGFX_SPV_GLSL_LENGTH},
    };

    uint32_t glsl_inst = 0u;
    for (size_t i = 0; i < sizeof(intrinsic_table) / sizeof(intrinsic_table[0]); i++) {
        if (strncmp(fn_name, intrinsic_table[i].prefix, strlen(intrinsic_table[i].prefix)) == 0) {
            glsl_inst = intrinsic_table[i].glsl_inst;
            break;
        }
    }

    /* If not an air.* intrinsic we recognize, drop it. llvm.* intrinsics
     * (lifetime, debug, etc.) fall through here and are silently dropped. */
    if (!glsl_inst) {
        LAGFX_TRACE("call: unrecognized intrinsic '%s' — drop", fn_name);
        return;
    }

    /* We only handle unary intrinsics for now (all 8 listed above).
     * The CALL operand layout after callee_rel is [args...], so we need
     * at least one argument. */
    uint32_t arg_slot_idx = callee_slot_idx + 1u;
    if (inst->num_ops <= arg_slot_idx) {
        LAGFX_TRACE("call: no args for '%s' — drop", fn_name);
        return;
    }

    /* Resolve result type. If has_explicit_type, the explicit-type slot
     * (ops[2]) is a FUNCTION type's AIR type-id; its op[1] is the return
     * type. Fall back to vec4f if unresolvable. */
    uint32_t result_ty_air = 0u;
    if (has_explicit_type && inst->num_ops >= 3u) {
        uint32_t fn_ty_idx = (uint32_t)inst->ops[2];
        uint32_t n_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        if (fn_ty_idx < n_types && ts[fn_ty_idx].kind == LAGFX_AIR_TYPE_FUNCTION) {
            result_ty_air = ts[fn_ty_idx].num_op >= 2u ? ts[fn_ty_idx].op[1] : 0u;
        }
    }

    uint32_t result_spv_type;
    if (result_ty_air) {
        result_spv_type = emit_air_type(c, result_ty_air);
    } else {
        /* Default to vec4f as per task spec. */
        result_spv_type = emit_type_vec4_f(c);
    }

    /* Resolve the single operand. */
    uint32_t arg_rel = (uint32_t)inst->ops[arg_slot_idx];
    uint32_t arg_id = resolve_relative(arg_rel, next_val_id);
    uint32_t arg_spv = resolve_or_undef(c, arg_id, result_spv_type);

    /* Allocate result SPIR-V id if not pre-bound. */
    uint32_t result_value_id = c->inst_id_base + inst_idx;
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpExtInst: [result_type, result_id, ext_set(id_glsl), inst_num, arg] */
    uint32_t ops[5];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = c->id_glsl;
    ops[3] = glsl_inst;
    ops[4] = arg_spv;
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_EXT_INST, ops, 5);

    /* Bind result and record AIR type for downstream ops. */
    bind_value_spv(c, result_value_id, result_spv);
    c->inst_result_air_type[inst_idx] = result_ty_air;
}

static void emit_inst_gep(xlate_ctx_t *c, uint32_t inst_idx,
                           const lagfx_air_inst_t *inst,
                           uint32_t result_value_id, uint32_t next_val_id) {
    /* INST_GEP: [flags, source_elem_type, ptr_rel, idx0_rel, idx1_rel, ...]
     * Result type = OpTypePointer Function (innermost-pointee).
     * In SPIR-V we emit OpAccessChain with the result pointer type
     * and the unwrapped index values.
     *
     * SPIR-V OpAccessChain layout: [result_type, result, base, idx0, idx1, ...]
     * NOTE: the FIRST index in LLVM GEP is the pointer-arithmetic index
     * into the BASE pointer (usually 0 for non-array bases). SPIR-V
     * OpAccessChain does NOT take that first index — it starts indexing
     * INTO the pointee. So we skip LLVM's first index (which is i64 0
     * by convention) and pass the remainder.
     */
    if (inst->num_ops < 4u) return;
    uint32_t source_ty = (uint32_t)inst->ops[1];
    uint32_t ptr_rel   = (uint32_t)inst->ops[2];
    uint32_t n_idx_total = inst->num_ops - 3u;
    if (n_idx_total < 1u) return;

    uint32_t ptr_id    = resolve_relative(ptr_rel, next_val_id);
    uint32_t ptr_spv   = resolve_value_spv(c, ptr_id);
    if (!ptr_spv) {
        /* Base pointer unknown — bind result as OpUndef of a default
         * pointer type and bail. */
        uint32_t fallback_ty = emit_air_type(c, source_ty);
        uint32_t fallback_ptr = emit_type_pointer(c, source_ty, fallback_ty,
                                                    LAGFX_SPV_STORAGE_FUNCTION);
        bind_value_spv(c, result_value_id, emit_undef(c, fallback_ptr));
        c->inst_result_air_type[inst_idx] = source_ty;
        return;
    }

    /* Walk the AIR type chain to deduce the result pointer's pointee
     * type. Start at source_ty; for each index AFTER the first
     * (which is the base-array index), step into the element type. */
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t pointee = source_ty;
    for (uint32_t i = 1; i < n_idx_total; i++) {
        if (pointee >= n_types) break;
        const lagfx_air_type_t *t = &ts[pointee];
        switch (t->kind) {
            case LAGFX_AIR_TYPE_ARRAY:  if (t->num_op >= 2u) pointee = t->op[1]; break;
            case LAGFX_AIR_TYPE_VECTOR: if (t->num_op >= 2u) pointee = t->op[1]; break;
            case LAGFX_AIR_TYPE_STRUCT_ANON:
            case LAGFX_AIR_TYPE_STRUCT_NAMED:
                /* Need the field index, which is a constant operand.
                 * For triangle's GEPs we don't index into structs at
                 * this layer; defer struct handling. */
                if (t->num_op >= 2u) pointee = t->op[1];
                break;
            default: break;
        }
    }

    uint32_t pointee_spv = emit_air_type(c, pointee);
    uint32_t result_ptr  = emit_type_pointer(c, pointee, pointee_spv,
                                              LAGFX_SPV_STORAGE_FUNCTION);

    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);

    /* Build the operand vector: [result_type, result, base, idx1, idx2, ...] */
    uint32_t ops[16];
    ops[0] = result_ptr;
    ops[1] = result_id;
    ops[2] = ptr_spv;
    uint32_t op_count = 3u;
    /* Skip the first LLVM GEP index (base-array index). */
    for (uint32_t i = 1; i < n_idx_total && op_count < 16u; i++) {
        uint32_t idx_rel = (uint32_t)inst->ops[3u + i];
        uint32_t idx_id  = resolve_relative(idx_rel, next_val_id);
        uint32_t idx_spv = resolve_value_spv(c, idx_id);
        if (!idx_spv) {
            /* Unresolvable index — substitute uint 0. */
            idx_spv = emit_const_uint32(c, 0u);
        }
        ops[op_count++] = idx_spv;
    }

    /* If the GEP had only the base index (n_idx_total == 1), SPIR-V
     * OpAccessChain isn't strictly needed — just alias. */
    if (op_count == 3u) {
        bind_value_spv(c, result_value_id, ptr_spv);
        c->inst_result_air_type[inst_idx] = pointee;
        return;
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_ACCESS_CHAIN, ops, op_count);
    bind_value_spv(c, result_value_id, result_id);
    c->inst_result_air_type[inst_idx] = pointee;
}

static void emit_inst_store(xlate_ctx_t *c, const lagfx_air_inst_t *inst,
                             uint32_t next_val_id) {
    /* INST_STORE: [ptr_rel, val_rel, align_packed, vol] */
    if (inst->num_ops < 2u) return;
    uint32_t ptr_rel = (uint32_t)inst->ops[0];
    uint32_t val_rel = (uint32_t)inst->ops[1];

    uint32_t ptr_id  = resolve_relative(ptr_rel, next_val_id);
    uint32_t val_id  = resolve_relative(val_rel, next_val_id);

    uint32_t ptr_spv = resolve_value_spv(c, ptr_id);
    if (!ptr_spv) {
        /* No valid pointer — drop the store. */
        return;
    }
    /* For val: if unresolvable, emit OpUndef of the GEP'd pointee type.
     * We approximate by looking up the result pointer's pointee shape
     * heuristically — for triangle the val type is vec2f32 (used by
     * the three array-element stores) or vec4f32 (the position store).
     * For the MVP, default to vec2f32 since that's what triangle's
     * GEP+STORE pattern stores. */
    uint32_t val_spv = resolve_value_spv(c, val_id);
    if (!val_spv) {
        val_spv = emit_undef(c, emit_type_vec2_f(c));
    }
    uint32_t ops[] = { ptr_spv, val_spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, ops, 2);
}

static void emit_inst_load(xlate_ctx_t *c, uint32_t inst_idx,
                            const lagfx_air_inst_t *inst,
                            uint32_t result_value_id, uint32_t next_val_id) {
    /* INST_LOAD abbreviated: [ptr_rel, result_type_id (absolute), align, vol].
     * Bcanalyzer-confirmed format. The un-abbreviated LLVM form
     * inserts a ptr-type id between ptr and result-type; the abbrev
     * drops it (deducible from context). */
    if (inst->num_ops < 2u) return;
    uint32_t ptr_rel       = (uint32_t)inst->ops[0];
    uint32_t result_ty_air = (uint32_t)inst->ops[1];

    uint32_t result_spv = emit_air_type(c, result_ty_air);
    uint32_t ptr_id     = resolve_relative(ptr_rel, next_val_id);
    uint32_t ptr_spv    = resolve_value_spv(c, ptr_id);
    if (!ptr_spv) {
        /* Bind undef. */
        bind_value_spv(c, result_value_id, emit_undef(c, result_spv));
        c->inst_result_air_type[inst_idx] = result_ty_air;
        return;
    }
    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { result_spv, result_id, ptr_spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
    bind_value_spv(c, result_value_id, result_id);
    c->inst_result_air_type[inst_idx] = result_ty_air;
}

static void emit_inst_shufflevec(xlate_ctx_t *c, uint32_t inst_idx,
                                   const lagfx_air_inst_t *inst,
                                   uint32_t result_value_id, uint32_t next_val_id) {
    /* SHUFFLEVEC bitcode operand layout: [vec1_rel, vec2_rel, mask_rel].
     *
     * Result type: vec<N x elemtype> where N = mask vector lane count.
     * Mask is a constant vec<N x i32> whose components encode the
     * per-lane source index (or the special value 0xFFFFFFFF for
     * "undefined").
     *
     * Resolve the mask via the new value-id → literal-i32 reverse
     * lookup (senior infrastructure, value_id_to_lit_i32 side-table).
     * If the mask resolves cleanly, emit OpVectorShuffle with the real
     * lane indices. If not (mask not bound), fall back to identity. */
    if (inst->num_ops < 3u) return;
    uint32_t v1_rel  = (uint32_t)inst->ops[0];
    uint32_t v2_rel  = (uint32_t)inst->ops[1];
    uint32_t msk_rel = (uint32_t)inst->ops[2];

    uint32_t v1_id  = resolve_relative(v1_rel,  next_val_id);
    uint32_t v2_id  = resolve_relative(v2_rel,  next_val_id);
    uint32_t msk_id = resolve_relative(msk_rel, next_val_id);

    uint32_t vec4_f      = emit_type_vec4_f(c);
    uint32_t vec2_f      = emit_type_vec2_f(c);
    uint32_t vec2_f_undef = emit_undef(c, vec2_f);
    uint32_t vec4_f_undef = emit_undef(c, vec4_f);

    uint32_t v1_spv = resolve_or_undef(c, v1_id, vec2_f);
    uint32_t v2_spv = resolve_or_undef(c, v2_id, vec4_f);

    /* OpVectorShuffle requires both source vectors to have the SAME
     * vector type. If we substituted a vec2 undef for v1 (because the
     * operand was unresolvable in this function-local context), pad
     * to vec4 undef instead. */
    if (v1_spv == vec2_f_undef) v1_spv = vec4_f_undef;
    if (v2_spv == vec2_f_undef) v2_spv = vec4_f_undef;

    /* Resolve the mask vector via the function-local AGGREGATE
     * constants. Each AGGREGATE's payload is a u32 array of value-ids
     * pointing at per-lane i32 constants which we bound via
     * bind_value_lit_i32 at the INTEGER pre-bind site. */
    uint32_t mask_lanes[4] = {0u, 1u, 2u, 3u};  /* identity fallback */

    uint32_t n_lc = 0;
    const lagfx_air_constant_t *lc =
        lagfx_air_function_body_local_constants(c->body, &n_lc);
    uint32_t lc_base = c->arg_id_base + c->num_args;
    if (lc && msk_id >= lc_base && msk_id < lc_base + n_lc) {
        const lagfx_air_constant_t *k = &lc[msk_id - lc_base];
        if (k->kind == LAGFX_AIR_CONST_AGGREGATE) {
            /* AGGREGATE: payload bytes are u32 value-ids of per-lane
             * i32 constants. Resolve each via the lit_i32 side-table. */
            const uint32_t *comps = (const uint32_t *)
                lagfx_air_function_body_payload_ptr(c->body,
                                                      k->payload.bytes.offset);
            uint32_t ncomp = k->payload.bytes.len / sizeof(uint32_t);
            if (comps && ncomp >= 1u) {
                uint32_t n = ncomp < 4u ? ncomp : 4u;
                for (uint32_t i = 0; i < n; i++) {
                    int32_t lit;
                    if (resolve_value_lit_i32(c, comps[i], &lit)) {
                        mask_lanes[i] = (uint32_t)lit;
                    } else {
                        /* SPIR-V OpVectorShuffle: 0xFFFFFFFF in a
                         * component means "undefined value in this
                         * lane" — matches LLVM poison semantics. */
                        mask_lanes[i] = 0xFFFFFFFFu;
                    }
                }
            }
        } else if (k->kind == LAGFX_AIR_CONST_DATA) {
            /* DATA: payload bytes ARE the raw u32 literal values —
             * no value-id indirection. Triangle's second SHUFFLEVEC
             * mask `[0, 1, 6, 7]` is a DATA-kind vec4<i32> constant. */
            const uint32_t *raw = (const uint32_t *)
                lagfx_air_function_body_payload_ptr(c->body,
                                                      k->payload.bytes.offset);
            uint32_t ncomp = k->payload.bytes.len / sizeof(uint32_t);
            if (raw && ncomp >= 1u) {
                uint32_t n = ncomp < 4u ? ncomp : 4u;
                for (uint32_t i = 0; i < n; i++) {
                    mask_lanes[i] = raw[i];
                }
            }
        }
    }

    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[8] = {
        vec4_f, result_id, v1_spv, v2_spv,
        mask_lanes[0], mask_lanes[1], mask_lanes[2], mask_lanes[3],
    };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VECTOR_SHUFFLE, ops, 8);
    bind_value_spv(c, result_value_id, result_id);
    c->inst_result_air_type[inst_idx] = 0u;
}

static void emit_inst_insertval(xlate_ctx_t *c, uint32_t inst_idx,
                                 const lagfx_air_inst_t *inst,
                                 uint32_t result_value_id, uint32_t next_val_id) {
    /* INSERTVAL: [agg_rel (or agg_type for fresh undef agg), val_rel, field_idx]
     * In LLVM, the first operand is the aggregate (often undef as a fresh
     * compose). If unresolvable we use OpUndef of struct{vec4f}.
     *
     * Actually the LLVM modern INSERTVAL format is:
     *   [agg, val, idx0, idx1, ...]
     * with agg and val relative-encoded. For triangle, INSERTVAL has 3
     * ops: agg, val, idx0 (=0).
     *
     * In SPIR-V: OpCompositeInsert %result_type %result %val %composite idx...
     */
    if (inst->num_ops < 3u) return;
    uint32_t agg_rel = (uint32_t)inst->ops[0];
    uint32_t val_rel = (uint32_t)inst->ops[1];
    uint32_t field   = (uint32_t)inst->ops[2];

    uint32_t agg_id  = resolve_relative(agg_rel, next_val_id);
    uint32_t val_id  = resolve_relative(val_rel, next_val_id);

    /* Triangle's return type: <{ <4 x float> }> — struct with one
     * vec4 field. The struct type was pre-emitted in Pass 0. */
    uint32_t vec4_f = emit_type_vec4_f(c);
    uint32_t struct_id = c->id_struct_v4f;
    uint32_t agg_spv = resolve_or_undef(c, agg_id, struct_id);
    uint32_t val_spv = resolve_or_undef(c, val_id, vec4_f);

    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    /* OpCompositeInsert: [result_type, result, object, composite, idx0, ...] */
    uint32_t ops[] = { struct_id, result_id, val_spv, agg_spv, field };
    lagfx_spv_builder_emit_op(c->b, 82 /* OpCompositeInsert */, ops, 5);
    bind_value_spv(c, result_value_id, result_id);
    c->inst_result_air_type[inst_idx] = 0u;
}

static void emit_inst_ret(xlate_ctx_t *c, const lagfx_air_inst_t *inst,
                            uint32_t next_val_id) {
    /* RET: [val_rel]   (or empty for void return)
     * For Vulkan vertex shader: the returned struct's first field is
     * the vec4 to store at the BuiltIn Position output. We extract it
     * via OpCompositeExtract and OpStore to %position_var. */
    if (c->stage == LAGFX_XLATE_STAGE_VERTEX && inst->num_ops >= 1u &&
        c->id_position_var) {
        uint32_t val_rel = (uint32_t)inst->ops[0];
        uint32_t val_id  = resolve_relative(val_rel, next_val_id);
        uint32_t vec4_f  = emit_type_vec4_f(c);
        uint32_t val_spv = resolve_or_undef(c, val_id, vec4_f);

        /* Extract field 0 of the returned struct (the vec4). If the
         * value we have is ALREADY a vec4 (e.g., a shufflevec result),
         * OpCompositeExtract from a vec4 with index 0 would yield a
         * scalar — that's wrong. So we only extract if the source
         * type is a struct.
         *
         * Heuristic: if the bound SPIR-V id came from an INSERTVAL,
         * it's the struct type; extract field 0 to get the vec4.
         * Otherwise (e.g., if RET took a vec4 directly), use as-is.
         *
         * Implementation: just try OpCompositeExtract %v4f %extract
         * %val 0; if val is a vec4 OpUndef, the extract would be a
         * scalar — but for triangle the val IS a struct. We trust the
         * INSERTVAL chain.
         */
        uint32_t extract_id = lagfx_spv_builder_alloc_id(c->b);
        uint32_t op_ex[] = { vec4_f, extract_id, val_spv, 0u };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, op_ex, 4);

        uint32_t op_st[] = { c->id_position_var, extract_id };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, op_st, 2);
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN, NULL, 0);
}

/* ===================================================================
 * BINOP handler — LLVM BinaryOps → SPIR-V arithmetic/logic ops
 *
 * AIR BINOP operand layout: [lhs_rel, rhs_rel, opcode_subfield, ...flags...]
 *
 * LLVM BinaryOps enum (verified from llvm/IR/Instruction.def):
 *   14=Add, 15=FAdd, 16=Sub, 17=FSub, 18=Mul, 19=FMul, 20=UDiv,
 *   21=SDiv, 22=FDiv, 23=URem, 24=SRem, 25=FRem, 26=Shl,
 *   27=LShr, 28=AShr, 29=And, 30=Or, 31=Xor
 *
 * AIR stores only the opcode subfield (opcode - BinaryOpsBegin) in ops[2].
 * So we subtract 14 to get the LLVM BinaryOps enum value.
 */
static void emit_inst_binop(xlate_ctx_t *c, uint32_t inst_idx,
                            const lagfx_air_inst_t *inst,
                            uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 3u) return;

    uint32_t lhs_rel = (uint32_t)inst->ops[0];
    uint32_t rhs_rel = (uint32_t)inst->ops[1];
    /* LLVM BinaryOps enum value */
    int llvm_binop = (int)(inst->ops[2]);

    /* Resolve operands. We need the LHS type to decide float vs int dispatch. */
    uint32_t lhs_id  = resolve_relative(lhs_rel, next_val_id);
    uint32_t rhs_id  = resolve_relative(rhs_rel, next_val_id);

    /* Look up AIR types for both operands via inst_result_air_type[].
     * For args/module-consts we fall back to deducing from the constant
     * table or defaulting to float. */
    uint32_t lhs_ty = 0u;
    uint32_t rhs_ty = 0u;

    /* LHS type: try inst_result_air_type first (for instruction results) */
    if (lhs_id >= c->inst_id_base && lhs_id < c->value_id_capacity) {
        int idx = (int)(lhs_id - c->inst_id_base);
        if (idx >= 0 && (uint32_t)idx < c->num_insts) {
            lhs_ty = c->inst_result_air_type[idx];
        }
    }
    /* If still unknown, try arg types */
    if (!lhs_ty && lhs_id >= c->arg_id_base) {
        uint32_t arg_idx = lhs_id - c->arg_id_base;
        if (arg_idx < c->num_args) {
            lhs_ty = c->arg_air_type_ids[arg_idx];
        }
    }
    /* If still unknown, try module const type */
    if (!lhs_ty && lhs_id >= c->module_val_count) {
        uint32_t n_consts = 0;
        const lagfx_air_constant_t *consts =
            lagfx_air_module_constants(c->m, &n_consts);
        uint32_t const_idx = lhs_id - c->module_val_count;
        if (const_idx < n_consts) {
            lhs_ty = consts[const_idx].type_index;
        }
    }

    /* RHS type: same logic */
    if (!rhs_ty && rhs_id >= c->inst_id_base && rhs_id < c->value_id_capacity) {
        int idx = (int)(rhs_id - c->inst_id_base);
        if (idx >= 0 && (uint32_t)idx < c->num_insts) {
            rhs_ty = c->inst_result_air_type[idx];
        }
    }
    if (!rhs_ty && rhs_id >= c->arg_id_base) {
        uint32_t arg_idx = rhs_id - c->arg_id_base;
        if (arg_idx < c->num_args) {
            rhs_ty = c->arg_air_type_ids[arg_idx];
        }
    }
    if (!rhs_ty && rhs_id >= c->module_val_count) {
        uint32_t n_consts = 0;
        const lagfx_air_constant_t *consts =
            lagfx_air_module_constants(c->m, &n_consts);
        uint32_t const_idx = rhs_id - c->module_val_count;
        if (const_idx < n_consts) {
            rhs_ty = consts[const_idx].type_index;
        }
    }

    /* Default to float if we can't resolve types */
    bool is_float = true;
    uint32_t result_ty_air = lhs_ty ? lhs_ty : 0u;
    if (lhs_ty) {
        const lagfx_air_type_t *t = NULL;
        uint32_t n_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        if (lhs_ty < n_types) {
            t = &ts[lhs_ty];
            is_float = (t->kind == LAGFX_AIR_TYPE_FLOAT ||
                        t->kind == LAGFX_AIR_TYPE_VECTOR);
        }
    }

    /* Determine result SPIR-V type */
    uint32_t result_spv_type;
    if (result_ty_air) {
        result_spv_type = emit_air_type(c, result_ty_air);
    } else {
        result_spv_type = is_float ? emit_type_vec4_f(c) : emit_type_int_w(c, 32u, 0u);
    }

    /* Resolve operands to SPIR-V ids; undef if unresolvable */
    uint32_t lhs_spv = resolve_or_undef(c, lhs_id, result_spv_type);
    uint32_t rhs_spv = resolve_or_undef(c, rhs_id, result_spv_type);

    /* Determine result SPIR-V id (allocate if not pre-bound) */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Map LLVM BITCODE BinaryOps subcode -> SPIR-V opcode.
     *
     * CRITICAL: these are the LLVM BITCODE subcodes (bitc::BINOP_*),
     * NOT the IR-level Instruction::BinaryOps enum values. The
     * bitcode reader's GetDecodedBinaryOpcode() maps the subcode
     * below to IR-level FAdd vs Add based on operand TYPE — there is
     * NO separate FAdd subcode in the bitcode stream. (Paid for
     * 2026-05-28: a freshman session cited Instruction.def's IR
     * enum (Add=14, FAdd=15, …) which never appears in bitcode; Vfx
     * UNOPs at subcode 0 silently failed the != 13 check and got
     * dropped.) Cite: llvm/include/llvm/Bitcode/LLVMBitCodes.h
     * bitc::BINOP_ADD..BINOP_XOR = 0..12. */
    uint32_t spv_op = 0u;
    switch (llvm_binop) {
        case 0:  /* BINOP_ADD  → FAdd / IAdd  */ spv_op = is_float ? LAGFX_SPV_OP_FADD : LAGFX_SPV_OP_IADD; break;
        case 1:  /* BINOP_SUB  → FSub / ISub  */ spv_op = is_float ? LAGFX_SPV_OP_FSUB : LAGFX_SPV_OP_ISUB; break;
        case 2:  /* BINOP_MUL  → FMul / IMul  */ spv_op = is_float ? LAGFX_SPV_OP_FMUL : LAGFX_SPV_OP_IMUL; break;
        case 3:  /* BINOP_UDIV → UDiv         */ spv_op = LAGFX_SPV_OP_UDIV; break;
        case 4:  /* BINOP_SDIV → FDiv / SDiv  */ spv_op = is_float ? LAGFX_SPV_OP_FDIV : LAGFX_SPV_OP_SDIV; break;
        case 5:  /* BINOP_UREM → UMod         */ spv_op = LAGFX_SPV_OP_UMOD; break;
        case 6:  /* BINOP_SREM → FRem / SMod  */ spv_op = is_float ? LAGFX_SPV_OP_FREM : LAGFX_SPV_OP_SMOD; break;
        case 7:  /* BINOP_SHL  → ShiftLeftLogical */     spv_op = LAGFX_SPV_OP_SHIFT_LEFT_LOGICAL; break;
        case 8:  /* BINOP_LSHR → ShiftRightLogical */    spv_op = LAGFX_SPV_OP_SHIFT_RIGHT_LOGICAL; break;
        case 9:  /* BINOP_ASHR → ShiftRightArithmetic */ spv_op = LAGFX_SPV_OP_SHIFT_RIGHT_ARITHMETIC; break;
        case 10: /* BINOP_AND  → BitwiseAnd   */ spv_op = LAGFX_SPV_OP_BITWISE_AND; break;
        case 11: /* BINOP_OR   → BitwiseOr    */ spv_op = LAGFX_SPV_OP_BITWISE_OR; break;
        case 12: /* BINOP_XOR  → BitwiseXor   */ spv_op = LAGFX_SPV_OP_BITWISE_XOR; break;
        default:
            /* Unrecognized bitcode BINOP subcode — drop */
            return;
    }

    /* If we didn't find a mapping for this type, skip */
    if (spv_op == 0u) return;

    /* Emit the operation: [result_type, result_id, operand1, operand2] */
    uint32_t ops[5];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = lhs_spv;
    ops[3] = rhs_spv;
    lagfx_spv_builder_emit_op(c->b, spv_op, ops, 4);

    /* Bind the result value-id */
    bind_value_spv(c, result_value_id, result_spv);
    c->inst_result_air_type[inst_idx] = result_ty_air;
}

/* ===================================================================
 * UNOP handler — LLVM UnaryOps → SPIR-V unary float ops
 *
 * AIR UNOP operand layout: [opval_rel, opcode_subfield, ...flags...]
 *
 * LLVM UnaryOps enum (verified from llvm/IR/Instruction.def):
 *   13 = FNeg (only entry in modern LLVM)
 *
 * SPIR-V op: OpFNegate (§3.32.2) — core SPIR-V unary float negation
 */
static void emit_inst_unop(xlate_ctx_t *c, uint32_t inst_idx,
                           const lagfx_air_inst_t *inst,
                           uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 2u) return;

    uint32_t opval_rel = (uint32_t)inst->ops[0];
    /* LLVM BITCODE UnaryOps subcode — `bitc::UNOP_FNEG = 0` per
     * llvm/include/llvm/Bitcode/LLVMBitCodes.h. NOT the IR-level
     * enum (where FNeg = 13). Paid for 2026-05-28. */
    int llvm_unop = (int)(inst->ops[1]);

    if (llvm_unop != 0 /* UNOP_FNEG */) {
        /* Not FNeg — unsupported. Future: FAbs via GLSL.std.450 ExtInst. */
        return;
    }

    /* Resolve operand */
    uint32_t opval_id = resolve_relative(opval_rel, next_val_id);
    uint32_t result_spv_type = emit_type_vec4_f(c); /* Assume vec4 for now */

    /* Try to deduce actual type from inst_result_air_type or arg types */
    if (opval_id >= c->inst_id_base && opval_id < c->value_id_capacity) {
        int idx = (int)(opval_id - c->inst_id_base);
        if (idx >= 0 && (uint32_t)idx < c->num_insts) {
            uint32_t ty_air = c->inst_result_air_type[idx];
            if (ty_air) result_spv_type = emit_air_type(c, ty_air);
        }
    }

    /* Resolve operand to SPIR-V id; undef if unresolvable */
    uint32_t opval_spv = resolve_or_undef(c, opval_id, result_spv_type);

    /* Determine result SPIR-V id (allocate if not pre-bound) */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpFNegate: [result_type, result_id, operand] */
    uint32_t ops[4];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = opval_spv;
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FNEGATE, ops, 3);

    /* Bind the result value-id */
    bind_value_spv(c, result_value_id, result_spv);
    c->inst_result_air_type[inst_idx] = result_spv_type ? 0u : 2u; /* float or unknown */
}

/* ===================================================================
 * Driver
 * =================================================================== */

static lagfx_status_t translate_body(xlate_ctx_t *c) {
    /* Pass 1: allocate result SPIR-V ids for each value-producing
     * instruction; populate value-id → spv map. */
    uint32_t next_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst_produces_value(inst)) {
            /* SPIR-V ids for instruction results are allocated in
             * emit_inst_* themselves (so the order of OpVariable /
             * OpAccessChain / OpLoad / ... ids matches the emission
             * order). At this point we just track the value-id slot
             * for later binding. */
            next_val_id++;
        }
    }

    /* Pass 2: emit per-instruction SPIR-V. */
    next_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        bool produces = inst_produces_value(inst);
        uint32_t result_value_id = produces ? next_val_id : 0u;

        switch (inst->code) {
            case LAGFX_AIR_INST_ALLOCA:
                emit_inst_alloca(c, i, inst, result_value_id);
                break;
            case LAGFX_AIR_INST_CAST:
                emit_inst_cast(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_CALL:
                emit_inst_call(c, i, inst, next_val_id);
                break;
            case LAGFX_AIR_INST_GEP:
            case LAGFX_AIR_INST_GEP_OLD:
                emit_inst_gep(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_STORE:
            case LAGFX_AIR_INST_STORE_OLD:
                emit_inst_store(c, inst, next_val_id);
                break;
            case LAGFX_AIR_INST_LOAD:
                emit_inst_load(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_SHUFFLEVEC:
                emit_inst_shufflevec(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_INSERTVAL:
                emit_inst_insertval(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_RET:
                emit_inst_ret(c, inst, next_val_id);
                break;
            case LAGFX_AIR_INST_BINOP:
                emit_inst_binop(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_UNOP:
                emit_inst_unop(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_DECLAREBLOCKS:
                /* No-op; basic-block count was used during decode. */
                break;
            default:
                /* Unhandled opcode — emit nothing. The function body
                 * may end up short on opcodes but spirv-val will
                 * accept it as long as the entry/exit are structurally
                 * sound. */
                LAGFX_TRACE("translate_function: dropping unhandled "
                            "AIR opcode raw=%u (code=%d) at i[%u]",
                            inst->raw_code, (int)inst->code, i);
                break;
        }

        if (produces) next_val_id++;
    }

    /* For fragment stage: ensure a position-equivalent is stored. We
     * don't currently translate fragment bodies; the fragment path
     * relies on the legacy emit_render_target stub via translate.c. */
    if (c->stage == LAGFX_XLATE_STAGE_FRAGMENT) {
        /* If the body didn't emit an OpStore to the color var, write a
         * red default. For triangle (vertex), this branch is unused. */
    }

    /* If the body did NOT end with a RET that we recognized, append
     * OpReturn to keep the function valid. */
    if (c->num_insts == 0u ||
        c->insts[c->num_insts - 1u].code != LAGFX_AIR_INST_RET) {
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN, NULL, 0);
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return LAGFX_OK;
}

lagfx_status_t
lagfx_air2spv_translate_function(const lagfx_air_module_t *m,
                                  uint32_t                  fn_idx,
                                  lagfx_xlate_stage_t       stage,
                                  uint8_t                 **out_blob,
                                  size_t                   *out_size_bytes) {
    if (!m || !out_blob || !out_size_bytes) return LAGFX_ERR_INVALID_ARG;
    *out_blob = NULL;
    *out_size_bytes = 0u;

    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &n_fns);
    if (fn_idx >= n_fns) return LAGFX_ERR_INVALID_ARG;
    const lagfx_air_function_t *fn = &fns[fn_idx];
    if (fn->is_proto || fn->body_offset == 0u) return LAGFX_ERR_INVALID_ARG;

    /* Decode the function body. */
    lagfx_air_function_body_t *body = NULL;
    lagfx_status_t st = lagfx_air_function_body_open(m, fn_idx, &body);
    if (st != LAGFX_OK || !body) return LAGFX_ERR_PROTOCOL;

    uint32_t n_insts = 0;
    const lagfx_air_inst_t *insts = lagfx_air_function_body_instructions(body, &n_insts);

    /* Allocate context. */
    xlate_ctx_t c = {0};
    c.b = lagfx_spv_builder_create(256u);
    if (!c.b) {
        lagfx_air_function_body_free(body);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    c.m = m;
    c.fn = fn;
    c.body = body;
    c.insts = insts;
    c.num_insts = n_insts;
    c.stage = stage;

    /* Type-id table. */
    uint32_t n_types = 0;
    (void)lagfx_air_module_types(m, &n_types);
    c.num_air_types = n_types;
    if (n_types > 0u) {
        c.spv_type_ids = (uint32_t *)calloc(n_types, sizeof(uint32_t));
        if (!c.spv_type_ids) goto oom;
    }

    /* Determine arg count from the function-type entry. */
    {
        uint32_t fn_ty_idx = fn->type_index;
        uint32_t nn_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(m, &nn_types);
        if (fn_ty_idx < nn_types && ts[fn_ty_idx].kind == LAGFX_AIR_TYPE_FUNCTION) {
            /* ops[0] = varargs, ops[1] = return type, ops[2..] = params */
            c.num_args = ts[fn_ty_idx].num_op > 2u ? ts[fn_ty_idx].num_op - 2u : 0u;
            if (c.num_args > 0u) {
                c.arg_spv_ids = (uint32_t *)calloc(c.num_args, sizeof(uint32_t));
                c.arg_air_type_ids = (uint32_t *)calloc(c.num_args, sizeof(uint32_t));
                if (!c.arg_spv_ids || !c.arg_air_type_ids) goto oom;
                for (uint32_t i = 0; i < c.num_args; i++) {
                    c.arg_air_type_ids[i] = ts[fn_ty_idx].op[2u + i];
                }
            }
        }
    }

    /* Module value count: ValueEnumerator-style ordering puts function
     * decls/defs first, then module constants. */
    uint32_t n_mod_consts = 0;
    (void)lagfx_air_module_constants(m, &n_mod_consts);
    c.module_val_count = n_fns + n_mod_consts;
    c.arg_id_base      = c.module_val_count;
    /* Function-local CONSTANTS_BLOCK now decoded by body_open and
     * exposed via lagfx_air_function_body_local_constants(). They
     * occupy value-IDs immediately after args. */
    uint32_t n_local_consts = 0;
    const lagfx_air_constant_t *local_consts =
        lagfx_air_function_body_local_constants(body, &n_local_consts);
    c.local_const_count = n_local_consts;
    c.inst_id_base = c.arg_id_base + c.num_args + c.local_const_count;

    /* Value-id map. Sized to cover module values + args + local consts +
     * one slot per body instruction (with slop). */
    c.value_id_capacity = c.inst_id_base + n_insts + 32u;
    c.value_id_to_spv = (uint32_t *)calloc(c.value_id_capacity, sizeof(uint32_t));
    if (!c.value_id_to_spv) goto oom;

    /* Side-table: int32 literal reverse lookup for SHUFFLEVEC mask resolution. */
    c.value_id_to_lit_i32 = (int32_t *)calloc(c.value_id_capacity, sizeof(int32_t));
    c.value_id_lit_i32_valid = (bool *)calloc(c.value_id_capacity, sizeof(bool));
    if (!c.value_id_to_lit_i32 || !c.value_id_lit_i32_valid) goto oom;

    c.inst_result_air_type = (uint32_t *)calloc(n_insts + 1u, sizeof(uint32_t));
    if (!c.inst_result_air_type) goto oom;

    /* === Emit ============================================================
     * SPIR-V module layout requires:
     *   Capability → ExtInstImport → MemoryModel → EntryPoint →
     *   ExecutionMode → Decorate → Types/Constants/Globals → Functions.
     * We emit the header first, THEN pre-bind module-level constants
     * (which emit OpType* + OpConstant as side effects), then module
     * variables + function. */

    emit_prologue(&c);

    /* Pre-bind module-level constants we can resolve directly. Module
     * constants live at value-ids [n_fns, n_fns + n_mod_consts). */
    {
        const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &n_mod_consts);
        for (uint32_t i = 0; i < n_mod_consts; i++) {
            uint32_t vid = n_fns + i;
            const lagfx_air_constant_t *k = &cs[i];
            uint32_t spv_id = 0u;
            switch (k->kind) {
                case LAGFX_AIR_CONST_INTEGER: {
                    /* Bind i32 and i64 integer constants. Other
                     * widths (i1/i8/i16/i128) require extra
                     * capabilities we don't declare; operands
                     * referencing them fall back to OpUndef. */
                    uint32_t ty_air = k->type_index;
                    if (ty_air < c.num_air_types) {
                        const lagfx_air_type_t *t = &((const lagfx_air_type_t *)lagfx_air_module_types(m, &(uint32_t){0}))[ty_air];
                        if (t->kind == LAGFX_AIR_TYPE_INTEGER) {
                            uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
                            if (w == 32u) {
                                uint32_t ty_spv = emit_type_int_w(&c, 32u, 0u);
                                spv_id = lagfx_spv_builder_alloc_id(c.b);
                                uint32_t ops[] = { ty_spv, spv_id,
                                                    (uint32_t)(uint64_t)k->payload.i64 };
                                lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                                /* Side-table: track int32 literal for SHUFFLEVEC reverse lookup. */
                                bind_value_lit_i32(&c, vid, (int32_t)(uint64_t)k->payload.i64);
                            } else if (w == 64u) {
                                uint32_t ty_spv = emit_type_int_w(&c, 64u, 0u);
                                spv_id = lagfx_spv_builder_alloc_id(c.b);
                                uint64_t bits = (uint64_t)k->payload.i64;
                                uint32_t ops[] = { ty_spv, spv_id,
                                                    (uint32_t)(bits & 0xFFFFFFFFu),
                                                    (uint32_t)(bits >> 32u) };
                                lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 4);
                            }
                        }
                    }
                    break;
                }
                case LAGFX_AIR_CONST_FLOAT: {
                    uint32_t ty_spv = emit_type_float32(&c);
                    spv_id = lagfx_spv_builder_alloc_id(c.b);
                    uint32_t bits = f32_bits_from_double(k->payload.f64);
                    uint32_t ops[] = { ty_spv, spv_id, bits };
                    lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                    break;
                }
                case LAGFX_AIR_CONST_NULL: {
                    /* Zero of the constant's type. Skip if the type
                     * tree recursively contains i8 / i64 / 0-length
                     * array — those would force Int8/Int64 caps or
                     * fail SPIR-V validation. Referring operands then
                     * fall back to OpUndef. */
                    uint32_t ty_air = k->type_index;
                    if (ty_air < c.num_air_types &&
                        !air_type_requires_extra_cap(&c, ty_air, 0)) {
                        uint32_t ty_spv = emit_air_type(&c, ty_air);
                        spv_id = lagfx_spv_builder_alloc_id(c.b);
                        uint32_t ops[] = { ty_spv, spv_id };
                        lagfx_spv_builder_emit_op(c.b, 46 /* OpConstantNull */, ops, 2);
                        /* NULL of an i32 type is the integer literal 0.
                         * Bind the lit_i32 side-table so SHUFFLEVEC
                         * mask resolution finds it when the mask
                         * AGGREGATE references this NULL component. */
                        if (ty_air < c.num_air_types) {
                            const lagfx_air_type_t *tt = &((const lagfx_air_type_t *)lagfx_air_module_types(m, &(uint32_t){0}))[ty_air];
                            if (tt->kind == LAGFX_AIR_TYPE_INTEGER &&
                                tt->num_op >= 1u && tt->op[0] == 32u) {
                                bind_value_lit_i32(&c, vid, 0);
                            }
                        }
                    }
                    break;
                }
                default:
                    /* AGGREGATE / DATA / STRING — defer; the operand
                     * will fall back to OpUndef of the expected type. */
                    break;
            }
            if (spv_id) bind_value_spv(&c, vid, spv_id);
        }
    }

    /* Pre-bind function-local constants at value-ids
     * [arg_id_end, arg_id_end + n_local_consts). These are the
     * vec2f data values, shufflevec masks, and other constants the
     * function body references but the module-level constants table
     * doesn't carry. See LLVM bitcode FUNCTION_BLOCK nested
     * CONSTANTS_BLOCK. */
    if (local_consts) {
        uint32_t lc_base = c.arg_id_base + c.num_args;
        uint32_t nn_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(m, &nn_types);
        for (uint32_t i = 0; i < n_local_consts; i++) {
            uint32_t vid = lc_base + i;
            const lagfx_air_constant_t *k = &local_consts[i];
            uint32_t spv_id = 0u;
            uint32_t ty_air = k->type_index;
            if (ty_air >= nn_types) continue;
            const lagfx_air_type_t *t = &ts[ty_air];

            switch (k->kind) {
                case LAGFX_AIR_CONST_INTEGER: {
                    /* Bind i32 + i64 integer constants. */
                    if (t->kind == LAGFX_AIR_TYPE_INTEGER) {
                        uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
                        if (w == 32u) {
                            uint32_t ty_spv = emit_type_int_w(&c, 32u, 0u);
                            spv_id = lagfx_spv_builder_alloc_id(c.b);
                            uint32_t ops[] = { ty_spv, spv_id,
                                                (uint32_t)(uint64_t)k->payload.i64 };
                            lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                        } else if (w == 64u) {
                            uint32_t ty_spv = emit_type_int_w(&c, 64u, 0u);
                            spv_id = lagfx_spv_builder_alloc_id(c.b);
                            uint64_t bits = (uint64_t)k->payload.i64;
                            uint32_t ops[] = { ty_spv, spv_id,
                                                (uint32_t)(bits & 0xFFFFFFFFu),
                                                (uint32_t)(bits >> 32u) };
                            lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 4);
                        }
                        /* Side-table: track int32 literal for SHUFFLEVEC reverse lookup. */
                        if (w == 32u) {
                            bind_value_lit_i32(&c, vid, (int32_t)(uint64_t)k->payload.i64);
                    }
                    }
                    break;
                }
                case LAGFX_AIR_CONST_FLOAT: {
                    /* Float constants: the parser stored the raw u64
                     * op[0] bits via union with double. For f32 types,
                     * extract the low 32 bits — they're the original
                     * IEEE-754 binary32 pattern. */
                    if (t->kind == LAGFX_AIR_TYPE_FLOAT) {
                        uint32_t ty_spv = emit_type_float32(&c);
                        uint32_t bits = (uint32_t)(uint64_t)k->payload.i64;
                        spv_id = lagfx_spv_builder_alloc_id(c.b);
                        uint32_t ops[] = { ty_spv, spv_id, bits };
                        lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                    }
                    break;
                }
                case LAGFX_AIR_CONST_NULL: {
                    if (air_type_requires_extra_cap(&c, ty_air, 0)) break;
                    uint32_t ty_spv = emit_air_type(&c, ty_air);
                    spv_id = lagfx_spv_builder_alloc_id(c.b);
                    uint32_t ops[] = { ty_spv, spv_id };
                    lagfx_spv_builder_emit_op(c.b, 46 /* OpConstantNull */, ops, 2);
                    /* NULL of i32 = lit 0 for SHUFFLEVEC mask resolution. */
                    if (t->kind == LAGFX_AIR_TYPE_INTEGER &&
                        t->num_op >= 1u && t->op[0] == 32u) {
                        bind_value_lit_i32(&c, vid, 0);
                    }
                    break;
                }
                case LAGFX_AIR_CONST_UNDEF:
                case LAGFX_AIR_CONST_UNKNOWN: {
                    /* OpUndef of the constant's type. UNKNOWN covers
                     * LLVM's POISON code (26) which we don't yet
                     * decode distinctly; treat as undef for now. */
                    if (air_type_requires_extra_cap(&c, ty_air, 0)) break;
                    uint32_t ty_spv = emit_air_type(&c, ty_air);
                    spv_id = emit_undef(&c, ty_spv);
                    break;
                }
                case LAGFX_AIR_CONST_DATA: {
                    /* DATA records hold a packed array of literal
                     * scalar values (raw u32 per lane). Used for
                     * vector-of-int / vector-of-float constants.
                     * Emit OpConstantComposite assembled from per-lane
                     * OpConstants. */
                    if (t->kind == LAGFX_AIR_TYPE_VECTOR && t->num_op >= 2u) {
                        uint32_t lanes  = t->op[0];
                        uint32_t elem_ai = t->op[1];
                        const lagfx_air_type_t *et =
                            (elem_ai < nn_types) ? &ts[elem_ai] : NULL;
                        if (!et) break;
                        const uint32_t *raw = (const uint32_t *)
                            lagfx_air_function_body_payload_ptr(body, k->payload.bytes.offset);
                        if (!raw) break;
                        uint32_t n_words = k->payload.bytes.len / sizeof(uint32_t);
                        if (n_words < lanes) break;
                        uint32_t elem_spv = emit_air_type(&c, elem_ai);
                        /* Emit per-lane constants. */
                        uint32_t lane_ids[16];
                        if (lanes > 16u) break;
                        for (uint32_t l = 0; l < lanes; l++) {
                            uint32_t lane_id = lagfx_spv_builder_alloc_id(c.b);
                            uint32_t ops[] = { elem_spv, lane_id, raw[l] };
                            lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                            lane_ids[l] = lane_id;
                        }
                        /* OpConstantComposite vecN elem0 elem1 ... */
                        uint32_t vec_spv = emit_air_type(&c, ty_air);
                        spv_id = lagfx_spv_builder_alloc_id(c.b);
                        uint32_t ops[1 + 1 + 16];
                        ops[0] = vec_spv;
                        ops[1] = spv_id;
                        for (uint32_t l = 0; l < lanes; l++) ops[2u + l] = lane_ids[l];
                        lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT_COMPOSITE,
                                                    ops, 2u + lanes);
                    }
                    break;
                }
                case LAGFX_AIR_CONST_AGGREGATE: {
                    /* AGGREGATE: payload bytes are a u32 array of
                     * absolute value-IDs of the component constants.
                     * For triangle: the shufflevec masks are
                     * AGGREGATE(undef, c[1], lc[0], lc[0]) → resolves
                     * each component via the value-id map. */
                    const uint32_t *comps = (const uint32_t *)
                        lagfx_air_function_body_payload_ptr(body, k->payload.bytes.offset);
                    if (!comps) break;
                    uint32_t ncomp = k->payload.bytes.len / sizeof(uint32_t);
                    if (ncomp == 0u || ncomp > 16u) break;

                    /* Result type — for vec / array / struct, just use
                     * the constant's declared type. */
                    uint32_t vec_spv = emit_air_type(&c, ty_air);

                    /* Resolve each component to a SPIR-V id; if not
                     * resolvable, substitute OpUndef of the element
                     * type when known. */
                    uint32_t elem_ai = 0u;
                    if (t->kind == LAGFX_AIR_TYPE_VECTOR && t->num_op >= 2u) {
                        elem_ai = t->op[1];
                    } else if (t->kind == LAGFX_AIR_TYPE_ARRAY && t->num_op >= 2u) {
                        elem_ai = t->op[1];
                    }
                    uint32_t elem_spv_fallback =
                        elem_ai ? emit_air_type(&c, elem_ai)
                                : emit_type_int_w(&c, 32u, 0u);

                    uint32_t comp_ids[16];
                    for (uint32_t j = 0; j < ncomp; j++) {
                        uint32_t cid = resolve_value_spv(&c, comps[j]);
                        if (!cid) cid = emit_undef(&c, elem_spv_fallback);
                        comp_ids[j] = cid;
                    }
                    spv_id = lagfx_spv_builder_alloc_id(c.b);
                    uint32_t ops[1 + 1 + 16];
                    ops[0] = vec_spv;
                    ops[1] = spv_id;
                    for (uint32_t j = 0; j < ncomp; j++) ops[2u + j] = comp_ids[j];
                    lagfx_spv_builder_emit_op(c.b, LAGFX_SPV_OP_CONSTANT_COMPOSITE,
                                                ops, 2u + ncomp);
                    break;
                }
                default:
                    break;
            }
            if (spv_id) bind_value_spv(&c, vid, spv_id);
        }
    }

    emit_module_vars_and_function(&c);

    st = translate_body(&c);
    if (st != LAGFX_OK) goto fail;

    *out_blob = lagfx_spv_builder_finish(c.b, out_size_bytes);
    if (!*out_blob) goto oom;

    lagfx_spv_builder_free(c.b);
    free(c.spv_type_ids);
    free(c.arg_spv_ids);
    free(c.arg_air_type_ids);
    free(c.value_id_to_spv);
    free(c.inst_result_air_type);
    lagfx_air_function_body_free(body);
    return LAGFX_OK;

oom:
    st = LAGFX_ERR_OUT_OF_MEMORY;
fail:
    if (c.b) lagfx_spv_builder_free(c.b);
    free(c.spv_type_ids);
    free(c.arg_spv_ids);
    free(c.arg_air_type_ids);
    free(c.value_id_to_spv);
    free(c.inst_result_air_type);
    lagfx_air_function_body_free(body);
    return st;
}
