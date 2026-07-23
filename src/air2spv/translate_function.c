/*
 * libapplegfx-vulkan — Phase 5 per-function AIR → SPIR-V translator
 * src/air2spv/translate_function.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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

/* Sentinel for "AIR type-index unknown / unresolved". MUST NOT be 0:
 * AIR type index 0 is a LEGITIMATE type (it is `float` in the compositor
 * shaders), and the translator historically conflated index 0 with
 * "unknown" — so any float-typed value (type index 0) was treated as
 * untyped, mis-dispatching CMP/SELECT/BINOP. inst_result_air_type[] is
 * initialised to this value; value_air_type_idx and the operand-type
 * resolvers return it when no type is found. UINT32_MAX can never be a
 * real type index (type tables are tiny). */
#define LAGFX_AIR_TYPE_NONE 0xFFFFFFFFu
/* Sentinel result-AIR-type for a value that is a SPIR-V bool (a comparison
 * result). AIR has no bool type index, so CMP records this; emit_air_type
 * maps it to OpTypeBool, and a downstream SELECT over bools resolves its
 * result type correctly (instead of treating "2" as a real type index). */
#define LAGFX_AIR_TYPE_BOOL 0xFFFFFFFEu

/* ===================================================================
 * Helper-function registry (UberShader member-function calls)
 *
 * Design A of KICKOFF-ubershader-function-calls: every non-proto callee
 * reachable from the entry function is translated as a REAL SPIR-V
 * function (OpFunction/OpFunctionParameter/OpReturnValue) into the same
 * module, and call sites emit OpFunctionCall. Helpers are emitted
 * CALLEE-FIRST (post-order over the call graph), so a call is only wired
 * to an already-emitted function — no forward references, and a helper
 * whose body fails to translate is rolled back (builder funcs rewind)
 * with its call sites degrading to the existing typed-undef contract.
 *
 * Parameter kinds (classify_helper_param):
 *   VALUE     — float/half/int/bool scalar or vector; plain by-value param.
 *   TEX/SAMP  — opaque resource pointer; param typed ptr-UniformConstant-
 *               image/sampler; call sites pass the resolved resource
 *               VARIABLE (a memory-object declaration — spirv-val-legal).
 *   THREADPTR — addrspace-0 data pointer (`thread T&`); the callee param
 *               is ptr-Function-T and the call site passes a fresh
 *               Function temp with copy-in/copy-out (interior pointers
 *               are not legal call args under Logical addressing).
 *   Constant/device DATA pointers (`constant float4*` matrices etc.)
 *   need VariablePointersStorageBuffer + OpPtrAccessChain — deferred;
 *   helpers taking one are not registered (call sites stay undef).
 * =================================================================== */

#define LAGFX_MAX_HELPER_FNS    96u
#define LAGFX_MAX_HELPER_PARAMS 8u

enum {
    LAGFX_HPARAM_VALUE = 1,
    LAGFX_HPARAM_TEX,
    LAGFX_HPARAM_SAMP,
    LAGFX_HPARAM_THREADPTR,
    /* constant/device pointer to a VALUE-class element (`constant
     * float4*` colour matrices, `device float*` LUTs): the param is a
     * ptr-StorageBuffer-T; call sites pass the interior pointer DIRECTLY
     * (legal with VariablePointersStorageBuffer), and callee-side p[i]
     * GEPs lower to OpPtrAccessChain (the param's pointer type carries
     * the required ArrayStride). */
    LAGFX_HPARAM_DATABUF,
    /* constant/device pointer to a NAMED DATA STRUCT (`constant
     * GammaLUTs*`): the param type is the SAME cached
     * OpTypePointer(StorageBuffer, <Block-struct>) the entry's
     * [[buffer(n)]] variable is declared with (emit_type_block_ptr —
     * call args match param types by ID), so call sites pass the Block
     * variable (or a tracked same-typed alias) directly. Callee GEPs
     * skip LLVM's leading 0 like any struct buffer base. */
    LAGFX_HPARAM_STRUCTBUF,
};

typedef struct {
    uint32_t fn_idx;      /* module function-table index */
    uint32_t func_id;     /* OpFunction result id (0 = registered-unsupported) */
    uint32_t fnty_id;     /* OpTypeFunction id */
    uint32_t ret_air;     /* AIR return type (LAGFX_AIR_TYPE_NONE = void) */
    uint32_t ret_spv;     /* SPIR-V return type (0 = void) */
    uint8_t  n_params;
    struct {
        uint8_t  kind;         /* LAGFX_HPARAM_* */
        uint32_t air_ty;       /* declared AIR param type */
        uint32_t spv_ty;       /* param's SPIR-V type (value type or ptr type) */
        uint32_t pointee_air;  /* THREADPTR: pointee AIR type */
        uint32_t pointee_spv;  /* THREADPTR: pointee SPIR-V type */
    } param[LAGFX_MAX_HELPER_PARAMS];
    bool     emitted;     /* body translated successfully */
} xlate_helper_fn_t;

typedef struct {
    xlate_helper_fn_t fns[LAGFX_MAX_HELPER_FNS];
    uint32_t n_fns;
    /* Shared module-constant SPIR-V bindings: emitted once (entry ctx),
     * copied into every later function ctx's value map. */
    uint32_t *mod_const_spv;
    int32_t  *mod_const_lit;
    bool     *mod_const_lit_valid;
    uint32_t  n_mod_consts;
    bool      mod_consts_bound;
    /* Planning recursion guard. */
    uint32_t visiting[16];
    uint32_t n_visiting;
    /* VariablePointersStorageBuffer capability emitted (once, on the
     * first DATABUF param registration). */
    bool cap_varptr;
    /* Pointer-type ids already decorated with ArrayStride (dedup —
     * duplicate identical decorations are legal but noisy). */
    uint32_t stride_decorated[16];
    uint32_t n_stride_decorated;
} xlate_helpers_t;

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
    /* Per-value POINTER storage class (stored as class+1; 0 = unset).
     * SPIR-V requires an OpAccessChain result's storage class to match its
     * base's, but chained GEPs (the no-index alias path) and pointer
     * bitcasts bind NEW value ids to a buffer-arg's variable — the id-range
     * heuristic in gep_base_storage_class can't see through them (VfxXgb:
     * StorageBuffer base, Function-class result → spirv-val reject). */
    uint8_t                  *value_storage;

    /* Side-table: int32 literal reverse lookup for SHUFFLEVEC mask resolution. */
    int32_t                  *value_id_to_lit_i32;
    bool                     *value_id_lit_i32_valid;

    /* === Block-dispatch control flow (GOAL-M2aa) ====================
     * General CFG support: bodies that don't match the recognised loop
     * shapes are emitted as ONE structured loop whose header switches on a
     * block-index variable (the classic relooper fallback). Values that
     * cross basic-block boundaries are demoted to Function variables
     * (def-site store, use-site load) because a dispatch case does not
     * dominate its successors; PHIs become variables stored by every
     * predecessor edge. Pointer-typed values are NOT spilled (logical
     * addressing) — cross-block pointer uses degrade to typed undefs. */
    bool                      dispatch_mode;
    uint32_t                  dispatch_cur_bb;
    /* Function-pointer types keyed by VALUE SPIR-V type — for spills whose
     * AIR type is the lossy BOOL sentinel (a v4bool compare result spilled
     * as scalar bool was an OpStore type mismatch). Duplicate OpTypePointer
     * declarations are permitted by spirv-val, but dedup anyway. */
    struct { uint32_t val_ty, ptr_ty; } dispatch_ptr_cache[8];
    uint32_t                  n_dispatch_ptr_cache;
    uint16_t                 *value_def_bb;   /* per value id; 0xFFFF unknown */
    uint32_t                 *spill_var;      /* per value id; 0 = none */
    uint32_t                 *spill_type;     /* value SPIR-V type of the spill */
    uint32_t                 *phi_var;        /* per PHI value id; 0 = none */
    uint32_t                 *phi_type;       /* value SPIR-V type of the phi var */
    uint32_t                 *value_def_inst; /* per value id; ~0u unknown */
    uint32_t                  dispatch_remat_depth;

    /* Side-table: result SPIR-V TYPE id per value (0 = unknown). AIR type
     * tracking (inst_result_air_type) goes dark through default-typed
     * fallback results; consumers that must type-check operands (SPIR-V
     * arithmetic requires operand type == result type) consult this to
     * catch mis-typed resolved operands (Xgc). */
    uint32_t                 *value_spv_type;

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
    uint32_t                  id_color_var;      /* fragment: Location 0 output (== id_color_vars[0]) */

    /* Per-vertex-arg Input variables. A vertex arg is either the
     * vertex_id builtin (integer-typed param) or a stage-in attribute
     * (vec/float param) declared as a Location-decorated Input. Indexed
     * by arg index; capped at LAGFX_MAX_VERTEX_ARGS. */
/* 32: the Xgc login-panel fragment (pipeline 0x31) takes 30 args — its
 * texture/sampler args sit past the old cap of 8 and were silently
 * dropped (no OpTypeImage in the module -> every sample undef -> black). */
#define LAGFX_MAX_VERTEX_ARGS 32u
    uint32_t                  arg_input_var_ids[LAGFX_MAX_VERTEX_ARGS];

    /* Fragment colour outputs. A `fragment float4 f()` has one (Location 0);
     * a struct-returning fragment writes one Location-N Output per struct
     * member (multiple render targets, e.g. YCbCr plane_y/plane_uv). */
    uint32_t                  id_color_vars[LAGFX_MAX_VERTEX_ARGS];
    uint32_t                  num_color_outputs;

    /* Vertex output varyings. A Metal vertex returning a struct
     * {float4 [[position]], varying0, ...} feeds the fragment's stage_in:
     * member 0 drives gl_Position AND every member is also a Location-k Output
     * varying so the fragment's interpolated inputs (e.g. the UV) are fed.
     * Without these the fragment reads unbound inputs → 0 → samples texel(0,0)
     * → black composites. Mirrors the fragment multi-output path. */
    uint32_t                  id_vertex_out_vars[LAGFX_MAX_VERTEX_ARGS];
    uint32_t                  num_vertex_outs;

    /* Fragment/compute resource args: a texture (pointer addrspace 1) or a
     * sampler (pointer addrspace 2) is declared as a UniformConstant
     * OpVariable. arg_resource_var[i] holds that variable's id (0 if arg i
     * is not a resource); the arg's value-id is bound to it so the
     * air.sample_texture_2d handler can OpLoad image/sampler. The opaque
     * type ids are shared across all resource args. */
    uint32_t                  arg_resource_var[LAGFX_MAX_VERTEX_ARGS];
    /* Per-arg resource kind (frag_resource_kind result: 1=tex 2=samp 3=buf,
     * 0=none), cached so GEP can tell a buffer var apart for its storage
     * class. */
    uint8_t                   arg_resource_kind[LAGFX_MAX_VERTEX_ARGS];
    /* Effective-pointee override for a nonstruct `device T*` buffer arg
     * whose body accesses UNANIMOUSLY reinterpret it as a different element
     * type. Metal code like `device uchar *raw` GEP'd with
     * `source_elem_type = float4` models the arg as { runtimearr<uchar> }
     * but indexes it in float4 units — the OpAccessChain result type then
     * mismatches the runtime-array element (spirv-val reject, the VfxXgb
     * login vertex shader). A body prescan collects every GEP source type /
     * direct-LOAD result type on the arg; if they all agree and differ from
     * the declared pointee, the Block is synthesized with the ACCESS type
     * instead (indices are already in access-type units per LLVM GEP
     * semantics). 0 = no override. */
    uint32_t                  arg_pointee_override[LAGFX_MAX_VERTEX_ARGS];
    /* Byte-buffer VIEW variables (descriptor aliasing). Metal code takes a
     * `device uchar *blob` arg and BITCASTs it to several typed views
     * (`device Uniforms*`, `device float4*`, ...) — the VfxXgb login vertex
     * shader. SPIR-V logical form can't reinterpret a { runtimearr<uchar> }
     * Block, but Vulkan permits MULTIPLE OpVariables decorated with the
     * SAME DescriptorSet/Binding, one per view type. The prescan registers
     * a view per (bytebuf arg, cast-dest pointee); the cast emitter binds
     * the cast result to the view's variable, and GEP/LOAD flow through
     * the existing struct-Block / runtimearray paths untouched. */
#define LAGFX_MAX_BYTEBUF_VIEWS 12u
    struct {
        uint8_t  arg;         /* owning buffer arg index */
        uint32_t pointee_ty;  /* AIR type of the view's pointee */
        uint32_t var_id;      /* pre-allocated OpVariable id */
    } bytebuf_view[LAGFX_MAX_BYTEBUF_VIEWS];
    uint32_t                  n_bytebuf_views;
    /* Per-value view index + 1 (0 = none): cast results that must bind to
     * a view variable instead of aliasing the raw byte-buffer variable. */
    uint8_t                  *value_view;
    /* Per-value buffer-arg alias map (arg index + 1; 0 = none), built by
     * prescan_buffer_arg_access_types: pointer BITCAST/ADDRSPACECAST
     * results that alias a buffer arg. Freed with the other side tables. */
    uint8_t                  *arg_alias;
    uint32_t                  id_default_sampler_var; /* constexpr-sampler fallback (0=none) */
    uint32_t                  id_image_t;        /* OpTypeImage 2D float, Sampled=1 */
    uint32_t                  id_sampler_t;      /* OpTypeSampler */
    uint32_t                  id_sampimg_t;      /* OpTypeSampledImage */
    /* Block-decorated struct-type cache (one per [[buffer(n)]] struct). For a
     * non-struct buffer pointee the key is the pointee AIR type. */
    struct { uint32_t air_ty; uint32_t spv_id; } block_cache[LAGFX_MAX_VERTEX_ARGS];
    uint32_t                  block_cache_len;
    /* OpTypeRuntimeArray cache, keyed on element AIR type — backs the
     * `{ runtimearray<T> }` Block synthesized for non-struct `device T*`. */
    struct { uint32_t air_ty; uint32_t spv_id; } rtarr_cache[LAGFX_MAX_VERTEX_ARGS];
    uint32_t                  rtarr_cache_len;
    /* OpTypePointer(StorageBuffer, <Block>) cache, keyed on the Block's
     * pointee AIR type. The entry's buffer-arg variable and any helper
     * STRUCTBUF param must share ONE pointer type id — OpFunctionCall
     * matches arg/param types by ID, and a re-emit would be a duplicate
     * pointer type declaration. */
    struct { uint32_t air_ty; uint32_t spv_id; } blockptr_cache[LAGFX_MAX_VERTEX_ARGS];
    uint32_t                  blockptr_cache_len;

    /* Common SPIR-V type ids (filled lazily). */
    uint32_t                  id_void;
    uint32_t                  id_uchar;          /* OpTypeInt 8 0 */
    uint8_t                   cap_int8;          /* OpCapability Int8 emitted */
    uint8_t                   cap_int16;         /* OpCapability Int16 emitted */
    uint8_t                   cap_float16;       /* OpCapability Float16 emitted */
    uint32_t                  id_uint;           /* OpTypeInt 32 0 */
    uint32_t                  id_int32;          /* OpTypeInt 32 1 — currently unused */
    uint32_t                  id_int64;          /* OpTypeInt 64 1 */
    uint32_t                  id_ulong;          /* OpTypeInt 64 0 */
    uint32_t                  id_char;           /* OpTypeInt 8 1  (B6: was uncached → dup) */
    uint32_t                  id_ushort;         /* OpTypeInt 16 0 (B6) */
    uint32_t                  id_short;          /* OpTypeInt 16 1 (B6) */
    uint32_t                  id_float32;
    uint32_t                  id_half;           /* OpTypeFloat 16 (half) */
    uint32_t                  id_bool;           /* OpTypeBool — comparison results */
    uint32_t                  id_vec2_f;
    uint32_t                  id_vec3_f;
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

    /* Helper-function registry (shared across all function ctxs of one
     * module translation) + per-function role flags. */
    xlate_helpers_t          *helpers;
    bool                      is_helper;
    uint32_t                  helper_ret_air;  /* AIR ret type (helpers) */
    uint32_t                  helper_ret_spv;  /* 0 = void */
    const xlate_helper_fn_t  *helper_self;     /* registry entry (helpers) */

    /* Cached ptr-UniformConstant-image / -sampler types. The resource
     * vars and every helper's tex/samp params MUST share ONE pointer type
     * id each: OpFunctionCall matches argument/parameter types by ID, not
     * structurally (verified against spirv-val). */
    uint32_t                  id_ptr_uc_image;
    uint32_t                  id_ptr_uc_sampler;

    /* Generic OpTypeVector dedup cache. SPIR-V forbids duplicate
     * non-aggregate type declarations, and emit_type_vec is also called
     * directly from the compare/select paths (bool vectors) — those call
     * sites re-emitted the same v4bool per comparison (spirv-val:
     * "Duplicate non-aggregate type declarations are not allowed", the
     * Xgc login fragment shader). Keyed on (elem type id, lanes). */
    struct { uint32_t elem, lanes, id; } vec_cache[32];
    uint32_t                  n_vec_cache;
} xlate_ctx_t;

/* Copy the MODULE-scope mutable state between function contexts. Helper
 * functions are translated with their own ctx (fresh value maps — bitcode
 * value-id spaces are per function) but must SHARE every uniqueness-
 * constrained type/constant cache: re-emitting an OpTypeFloat/Vector/
 * Image/Function from a fresh cache would be a duplicate non-aggregate
 * type declaration (spirv-val reject). Translation is strictly
 * sequential, so state flows copy-in → translate → copy-back. Fields NOT
 * listed here are per-function (value maps, interface vars, dispatch
 * state, arg tables). */
static void copy_module_state(xlate_ctx_t *dst, const xlate_ctx_t *src) {
    dst->spv_type_ids   = src->spv_type_ids;
    dst->num_air_types  = src->num_air_types;
    memcpy(dst->ptr_cache, src->ptr_cache, sizeof(dst->ptr_cache));
    dst->ptr_cache_len  = src->ptr_cache_len;
    memcpy(dst->const_cache, src->const_cache, sizeof(dst->const_cache));
    dst->const_cache_len = src->const_cache_len;
    memcpy(dst->block_cache, src->block_cache, sizeof(dst->block_cache));
    dst->block_cache_len = src->block_cache_len;
    memcpy(dst->rtarr_cache, src->rtarr_cache, sizeof(dst->rtarr_cache));
    dst->rtarr_cache_len = src->rtarr_cache_len;
    memcpy(dst->blockptr_cache, src->blockptr_cache, sizeof(dst->blockptr_cache));
    dst->blockptr_cache_len = src->blockptr_cache_len;
    memcpy(dst->vec_cache, src->vec_cache, sizeof(dst->vec_cache));
    dst->n_vec_cache    = src->n_vec_cache;
    dst->module_val_count = src->module_val_count;
    dst->id_void        = src->id_void;
    dst->id_uchar       = src->id_uchar;
    dst->cap_int8       = src->cap_int8;
    dst->cap_int16      = src->cap_int16;
    dst->cap_float16    = src->cap_float16;
    dst->id_uint        = src->id_uint;
    dst->id_int32       = src->id_int32;
    dst->id_int64       = src->id_int64;
    dst->id_ulong       = src->id_ulong;
    dst->id_char        = src->id_char;
    dst->id_ushort      = src->id_ushort;
    dst->id_short       = src->id_short;
    dst->id_float32     = src->id_float32;
    dst->id_half        = src->id_half;
    dst->id_bool        = src->id_bool;
    dst->id_vec2_f      = src->id_vec2_f;
    dst->id_vec3_f      = src->id_vec3_f;
    dst->id_vec4_f      = src->id_vec4_f;
    dst->id_fn_void     = src->id_fn_void;
    dst->id_glsl        = src->id_glsl;
    dst->id_image_t     = src->id_image_t;
    dst->id_sampler_t   = src->id_sampler_t;
    dst->id_sampimg_t   = src->id_sampimg_t;
    dst->id_ptr_uc_image   = src->id_ptr_uc_image;
    dst->id_ptr_uc_sampler = src->id_ptr_uc_sampler;
    dst->id_default_sampler_var = src->id_default_sampler_var;
    dst->helpers        = src->helpers;
}

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

/* Emit OpTypeFloat 16 (half). */
static uint32_t emit_type_half(xlate_ctx_t *c) {
    if (c->id_half) return c->id_half;
    /* OpTypeFloat 16 requires the Float16 capability. Emit it lazily at the
     * point of use — the multi-section builder routes OpCapability to the
     * preamble regardless of emission order. */
    if (!c->cap_float16) {
        c->cap_float16 = 1u;
        uint32_t cap[] = { LAGFX_SPV_CAPABILITY_FLOAT16 };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY, cap, 1);
    }
    c->id_half = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { c->id_half, 16u };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_FLOAT, ops, 2);
    return c->id_half;
}

/* Emit OpTypeInt with the given width + signedness (0=unsigned, 1=signed). */
static uint32_t emit_type_bool(xlate_ctx_t *c);  /* fwd: i1 -> OpTypeBool */

static uint32_t emit_type_int_w(xlate_ctx_t *c, uint32_t width, uint32_t sign) {
    /* SPIR-V has no 1-bit integer; LLVM i1 is a boolean -> OpTypeBool. */
    if (width == 1u) return emit_type_bool(c);
    if (width == 8u  && sign == 0u && c->id_uchar)   return c->id_uchar;
    if (width == 8u  && sign == 1u && c->id_char)    return c->id_char;   /* B6 */
    if (width == 16u && sign == 0u && c->id_ushort)  return c->id_ushort; /* B6 */
    if (width == 16u && sign == 1u && c->id_short)   return c->id_short;  /* B6 */
    if (width == 32u && sign == 0u && c->id_uint)    return c->id_uint;
    if (width == 32u && sign == 1u && c->id_int32)   return c->id_int32;
    if (width == 64u && sign == 1u && c->id_int64)   return c->id_int64;
    if (width == 64u && sign == 0u && c->id_ulong)   return c->id_ulong;
    /* Narrow ints need a capability. Emit it AT THE POINT OF USE — the
     * multi-section builder routes OpCapability to the preamble regardless
     * of when it is emitted, so we don't need a fragile pre-scan. Guarded
     * by a once-flag (signed-8/16 aren't type-cached, so the bare cache
     * miss above can recur). */
    if (width == 8u && !c->cap_int8) {
        c->cap_int8 = 1u;
        uint32_t cap[] = { LAGFX_SPV_CAPABILITY_INT8 };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY, cap, 1);
    } else if (width == 16u && !c->cap_int16) {
        c->cap_int16 = 1u;
        uint32_t cap[] = { LAGFX_SPV_CAPABILITY_INT16 };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY, cap, 1);
    }
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, width, sign };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_INT, ops, 3);
    if (width == 8u  && sign == 0u) c->id_uchar = id;
    if (width == 8u  && sign == 1u) c->id_char = id;    /* B6 */
    if (width == 16u && sign == 0u) c->id_ushort = id;  /* B6 */
    if (width == 16u && sign == 1u) c->id_short = id;   /* B6 */
    if (width == 32u && sign == 0u) c->id_uint = id;
    if (width == 32u && sign == 1u) c->id_int32 = id;
    if (width == 64u && sign == 1u) c->id_int64 = id;
    if (width == 64u && sign == 0u) c->id_ulong = id;
    return id;
}

/* Emit OpTypeBool (cached). Comparison ops (§3.32.15) produce bool. */
static uint32_t emit_type_bool(xlate_ctx_t *c) {
    if (c->id_bool) return c->id_bool;
    c->id_bool = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { c->id_bool };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_BOOL, ops, 1);
    return c->id_bool;
}

static uint32_t emit_type_vec(xlate_ctx_t *c, uint32_t elem_spv, uint32_t lanes) {
    for (uint32_t i = 0; i < c->n_vec_cache; i++)
        if (c->vec_cache[i].elem == elem_spv && c->vec_cache[i].lanes == lanes)
            return c->vec_cache[i].id;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, elem_spv, lanes };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_VECTOR, ops, 3);
    if (c->n_vec_cache < 32u) {
        c->vec_cache[c->n_vec_cache].elem  = elem_spv;
        c->vec_cache[c->n_vec_cache].lanes = lanes;
        c->vec_cache[c->n_vec_cache].id    = id;
        c->n_vec_cache++;
    }
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

static uint32_t emit_type_vec3_f(xlate_ctx_t *c) {
    if (c->id_vec3_f) return c->id_vec3_f;
    c->id_vec3_f = emit_type_vec(c, emit_type_float32(c), 3u);
    return c->id_vec3_f;
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
    if (air_type_idx == LAGFX_AIR_TYPE_BOOL) return emit_type_bool(c);
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
        case LAGFX_AIR_TYPE_HALF:
            /* IEEE-754 binary16. Needs the Float16 capability, which the
             * builder emits lazily at point-of-use of OpTypeFloat 16. Without
             * this case `half` fell through to `default` → uint32, so a
             * `<2 x half>` vector came out `v2uint` and the f→f convert that
             * targets it (`air.convert.f.v2f16.f.v2f32` → OpFConvert) got a
             * non-float result type → spirv-val "Expected float scalar or
             * vector type as Result Type: FConvert". */
            out = emit_type_half(c); break;
        case LAGFX_AIR_TYPE_INTEGER: {
            uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
            /* LLVM i1 is a boolean — SPIR-V has no 1-bit integer
             * (OpTypeInt 1 is invalid); map it to OpTypeBool. (icmp/fcmp/
             * air.all/air.any results and i1 stack temps flow here.) */
            if (w == 1u) { out = emit_type_bool(c); break; }
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
            } else if (lanes == 3u && elem_spv == c->id_float32 && c->id_vec3_f) {
                out = c->id_vec3_f;
            } else if (lanes == 4u && elem_spv == c->id_float32 && c->id_vec4_f) {
                out = c->id_vec4_f;
            } else {
                out = emit_type_vec(c, elem_spv, lanes);
                if (lanes == 2u && elem_spv == c->id_float32) c->id_vec2_f = out;
                if (lanes == 3u && elem_spv == c->id_float32) c->id_vec3_f = out;
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
            /* LLVM `[0 x T]` (flexible array member / dead artifact chains in
             * SkyLight shaders) — OpTypeArray requires length >= 1
             * (spirv-val: "Length default value must be at least 1", the
             * TvcmXc_Isrc login fragment shader). These arrays are never
             * legitimately indexed at runtime in Function storage; clamp. */
            if (len == 0u) len = 1u;
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
            /* B7: the field arrays + ops[] hold at most 16; the emit below
             * passed the FULL nfields count, so nfields>=17 read OOB ops[]
             * (uninitialized → garbage type / crash on unbounded guest input).
             * Clamp to the array capacity. */
            if (nfields > 16u) nfields = 16u;
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

/* The fragment function's declared return AIR type (LAGFX_AIR_TYPE_NONE if
 * not derivable). */
static uint32_t fragment_return_air_type(xlate_ctx_t *c) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
    if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION &&
        ts[fn_ty].num_op >= 2u)
        return ts[fn_ty].op[1];
    return LAGFX_AIR_TYPE_NONE;
}

/* Number of fragment colour outputs (render targets). A struct return is one
 * Location-N Output per member (YCbCr plane_y/plane_uv); anything else is a
 * single output. */
static uint32_t fragment_output_count(xlate_ctx_t *c) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t ret_ty = fragment_return_air_type(c);
    if (ret_ty < n_types &&
        (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
         ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
        uint32_t nf = ts[ret_ty].num_op > 1u ? ts[ret_ty].num_op - 1u : 0u;
        if (nf == 0u) nf = 1u;
        if (nf > LAGFX_MAX_VERTEX_ARGS) nf = LAGFX_MAX_VERTEX_ARGS;
        return nf;
    }
    return 1u;
}

/* AIR type of colour output `idx`: a struct member type for multi-target, or
 * the whole return type for a single output. */
static uint32_t fragment_output_member_air_type(xlate_ctx_t *c, uint32_t idx) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t ret_ty = fragment_return_air_type(c);
    if (ret_ty < n_types &&
        (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
         ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
        uint32_t nf = ts[ret_ty].num_op > 1u ? ts[ret_ty].num_op - 1u : 0u;
        if (idx < nf) return ts[ret_ty].op[1u + idx];
    }
    return ret_ty;
}

/* SPIR-V type id for fragment colour output `idx`. A Metal fragment can
 * return `float4`, but real SkyLight fragments also return `float2`
 * (SimpleTextureFragmentUV), scalar `float` (ColorFillYCbCr_ChromaOnly), or
 * a struct of those (ColorFillYCbCr: {float, float2} -> two render targets).
 * The output OpVariable AND the RET store must match this, else spirv-val
 * rejects the store with a type mismatch (it previously hardcoded v4float).
 * Falls back to v4float for any non-float result. */
static uint32_t fragment_output_type_spv_idx(xlate_ctx_t *c, uint32_t idx) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t mty = fragment_output_member_air_type(c, idx);
    if (mty < n_types) {
        lagfx_air_type_kind_t k = ts[mty].kind;
        if (k == LAGFX_AIR_TYPE_FLOAT || k == LAGFX_AIR_TYPE_VECTOR)
            return emit_air_type(c, mty);
    }
    return emit_type_vec4_f(c);
}

/* Count of vertex output struct members ({[[position]], varying...} return).
 * 0 for a bare float4 return (just gl_Position, no varyings). */
static uint32_t vertex_output_count(xlate_ctx_t *c) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
    if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION
        && ts[fn_ty].num_op >= 2u) {
        uint32_t ret_ty = ts[fn_ty].op[1];
        if (ret_ty < n_types &&
            (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
             ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
            uint32_t nf = ts[ret_ty].num_op > 1u ? ts[ret_ty].num_op - 1u : 0u;
            if (nf > LAGFX_MAX_VERTEX_ARGS) nf = LAGFX_MAX_VERTEX_ARGS;
            return nf;
        }
    }
    return 0u;
}

/* SPIR-V type id for vertex output varying member `idx`. */
static uint32_t vertex_output_type_spv_idx(xlate_ctx_t *c, uint32_t idx) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
    if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION
        && ts[fn_ty].num_op >= 2u) {
        uint32_t ret_ty = ts[fn_ty].op[1];
        if (ret_ty < n_types &&
            (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
             ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
            uint32_t nf = ts[ret_ty].num_op > 1u ? ts[ret_ty].num_op - 1u : 0u;
            if (idx < nf) {
                uint32_t mty = ts[ret_ty].op[1u + idx];
                if (mty < n_types) {
                    lagfx_air_type_kind_t k = ts[mty].kind;
                    if (k == LAGFX_AIR_TYPE_FLOAT || k == LAGFX_AIR_TYPE_VECTOR)
                        return emit_air_type(c, mty);
                }
            }
        }
    }
    return emit_type_vec4_f(c);
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
            /* i8/i16/i32/i64 are all first-class now (Int8/Int16 caps have
             * been declared since the byte-buffer work; uchar/ushort types
             * emit throughout). Filtering i8 here left uchar CST_CODE_NULL
             * constants unbound — the Xgc mode SWITCH's `case 0` compare was
             * silently dropped, so the REAL runtime mode byte (0) fell to the
             * default = undef-phi = NaN output (offline replay, 2026-07-23).
             * Only i1/i128-style widths still fall back. */
            return !(w == 8u || w == 16u || w == 32u || w == 64u);
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

/* OpConstant of a 32-bit float (cached by bit pattern). Must precede
 * OpFunction per SPIR-V layout — pre-warm in the prologue before body use. */
static uint32_t emit_const_float32(xlate_ctx_t *c, float val) {
    uint32_t bits; memcpy(&bits, &val, sizeof(bits));
    uint32_t ty = emit_type_float32(c);
    uint32_t cached = cache_lookup_const(c, 1, ty, (uint64_t)bits);
    if (cached) return cached;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { ty, id, bits };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
    cache_store_const(c, 1, ty, (uint64_t)bits, id);
    return id;
}

/* OpUndef per SPIR-V type. Used as a placeholder for unresolvable
 * operands (function-local constants pending Phase 5.5 decode). */
static uint32_t emit_undef(xlate_ctx_t *c, uint32_t spv_type) {
    /* OpUndef %void is illegal (spirv-val: "Cannot create undefined values
     * with void type") — it arises when a placeholder is built for a value
     * whose AIR type resolved to void (e.g. a void-returning intrinsic that
     * was nonetheless asked for a result, or an unresolved type in a
     * function-constant-gated stub body). Substitute a uint undef: a void
     * undef is never meaningfully consumed, and this keeps the module valid. */
    if (c->id_void && spv_type == c->id_void)
        spv_type = emit_type_int_w(c, 32u, 0u);
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
static void emit_inst_gep(xlate_ctx_t *c, uint32_t inst_idx,
                           const lagfx_air_inst_t *inst,
                           uint32_t result_value_id, uint32_t next_val_id);
static void emit_inst_cast(xlate_ctx_t *c, uint32_t inst_idx,
                             const lagfx_air_inst_t *inst,
                             uint32_t result_value_id, uint32_t next_val_id);

static uint32_t resolve_value_spv(xlate_ctx_t *c, uint32_t value_id) {
    if (value_id >= c->value_id_capacity) return 0u;
    /* Dispatch mode: a value defined in a DIFFERENT dispatch case does not
     * dominate this one — return a fresh load of its spill variable. Block
     * 0 is emitted in the SPIR-V entry (before the loop) and dominates
     * everything, so its values resolve directly. Unspilled cross-block
     * values (pointers, unknown types, forward refs) resolve to 0 and the
     * caller's typed-undef fallback applies. */
    if (c->dispatch_mode && value_id >= c->inst_id_base && c->value_def_bb) {
        uint16_t db = c->value_def_bb[value_id];
        if (db != 0xFFFFu && db != 0u && (uint32_t)db != c->dispatch_cur_bb) {
            if (c->spill_var && c->spill_var[value_id]) {
                uint32_t id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t ops[] = { c->spill_type[value_id], id,
                                   c->spill_var[value_id] };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
                return id;
            }
            /* A PHI value has no spill — its live value is in its dispatch
             * phi variable (stored by every executed predecessor edge). */
            if (c->phi_var && c->phi_var[value_id] &&
                c->phi_type && c->phi_type[value_id]) {
                uint32_t id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t ops[] = { c->phi_type[value_id], id,
                                   c->phi_var[value_id] };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
                return id;
            }
            /* Pointer-typed defs can't be spilled — RE-MATERIALIZE the
             * access chain in the CURRENT block instead: GEP bases bottom
             * out at entry-dominating variables (allocas / buffer Blocks),
             * and the indices resolve through this same hook (spill loads
             * / recursive remat, depth-capped). The def's value-id map
             * binding is refreshed to the new chain — safe: only the
             * latest-emitted block reads it, earlier blocks already
             * resolved their own copy. */
            if (c->value_def_inst && c->dispatch_remat_depth < 4u &&
                c->value_def_inst[value_id] != ~0u) {
                uint32_t di = c->value_def_inst[value_id];
                const lagfx_air_inst_t *dinst = &c->insts[di];
                if (dinst->code == LAGFX_AIR_INST_GEP ||
                    dinst->code == LAGFX_AIR_INST_GEP_OLD) {
                    c->dispatch_remat_depth++;
                    emit_inst_gep(c, di, dinst, value_id, value_id);
                    c->dispatch_remat_depth--;
                    return c->value_id_to_spv[value_id];
                }
                if (dinst->code == LAGFX_AIR_INST_CAST) {
                    c->dispatch_remat_depth++;
                    emit_inst_cast(c, di, dinst, value_id, value_id);
                    c->dispatch_remat_depth--;
                    return c->value_id_to_spv[value_id];
                }
                LAGFX_TRACE("dispatch: cross-case MISS vid=%u def_bb=%u "
                            "def_inst=%u code=%d raw=%u", value_id, db, di,
                            (int)dinst->code, dinst->raw_code);
            }
            return 0u;
        }
    }
    return c->value_id_to_spv[value_id];
}

static void bind_value_spv(xlate_ctx_t *c, uint32_t value_id, uint32_t spv_id) {
    if (value_id < c->value_id_capacity) {
        c->value_id_to_spv[value_id] = spv_id;
    }
}

/* Pointer storage-class tracking (see the value_storage field doc). */
static void set_value_spv_type(xlate_ctx_t *c, uint32_t value_id, uint32_t ty) {
    if (c->value_spv_type && value_id < c->value_id_capacity)
        c->value_spv_type[value_id] = ty;
}

static uint32_t get_value_spv_type(const xlate_ctx_t *c, uint32_t value_id) {
    if (c->value_spv_type && value_id < c->value_id_capacity)
        return c->value_spv_type[value_id];
    return 0u;
}

static void set_value_storage(xlate_ctx_t *c, uint32_t value_id, uint32_t sc) {
    if (c->value_storage && value_id < c->value_id_capacity)
        c->value_storage[value_id] = (uint8_t)(sc + 1u);
}

static uint32_t get_value_storage(const xlate_ctx_t *c, uint32_t value_id) {
    if (c->value_storage && value_id < c->value_id_capacity
        && c->value_storage[value_id])
        return (uint32_t)c->value_storage[value_id] - 1u;
    return UINT32_MAX;
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

/* Forward decls — defined further below but referenced from the
 * prologue's ALLOCA pre-emission pass. */
static bool inst_produces_value(const xlate_ctx_t *c,
                                const lagfx_air_inst_t *i);
static void set_result_air_type(xlate_ctx_t *c, uint32_t result_value_id,
                                uint32_t air_ty);
static uint32_t call_return_air_type(const xlate_ctx_t *c,
                                     const lagfx_air_inst_t *i);
static uint32_t value_air_type_idx(xlate_ctx_t *c, uint32_t value_id);

/* Classify a vertex arg for interface emission:
 *   VID   — integer scalar → BuiltIn VertexIndex (uint Input).
 *   ATTR  — float/vector    → stage-in attribute (Location-decorated Input).
 *   SKIP  — pointer/struct/etc. (e.g. a [[buffer(n)]] binding) → NOT a
 *           simple Input variable; emitting one yields an invalid
 *           pointer-pointee OpVariable under Logical addressing. Left
 *           unbound (resolves to undef downstream) until buffer bindings
 *           are modelled.
 * (Triangle/clear: uint vertex_id → VID. color_fill/composite_over: vec2
 * attributes → ATTR. Vfx: a buffer pointer → SKIP.) */
typedef enum { LAGFX_VARG_VID, LAGFX_VARG_ATTR, LAGFX_VARG_SKIP } lagfx_varg_kind_t;

static lagfx_varg_kind_t vertex_arg_kind(const xlate_ctx_t *c, uint32_t arg_idx) {
    if (arg_idx >= c->num_args) return LAGFX_VARG_SKIP;
    uint32_t ty = c->arg_air_type_ids[arg_idx];
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (ty >= n) return LAGFX_VARG_SKIP;
    switch (ts[ty].kind) {
        case LAGFX_AIR_TYPE_INTEGER:
            /* Only the vertex stage maps an integer scalar arg to the
             * BuiltIn VertexIndex. A fragment integer input would need a
             * Flat-decorated Location input (not modelled yet) → SKIP. */
            return (c->stage == LAGFX_XLATE_STAGE_VERTEX)
                       ? LAGFX_VARG_VID : LAGFX_VARG_SKIP;
        case LAGFX_AIR_TYPE_FLOAT:
        case LAGFX_AIR_TYPE_VECTOR:  return LAGFX_VARG_ATTR;
        default:                     return LAGFX_VARG_SKIP;
    }
}

/* Classify a pointer arg as a graphics resource. Returns:
 *   1 = texture   (opaque pointee, addrspace 1)
 *   2 = sampler   (opaque pointee, addrspace 2)
 *   3 = buffer    (STRUCT pointee — a `[[buffer(n)]]` constant/device block)
 *   0 = neither.
 *
 * Address space ALONE is ambiguous: a `sampler [[sampler(n)]]` and a
 * `constant T* [[buffer(n)]]` are BOTH addrspace 2. And Apple represents
 * an opaque resource type in TWO ways: an opaque pointee, OR a NAMED struct
 * (`_texture_2d_t`, `_sampler_t`, `_depth_2d_t`, ...). So we disambiguate by
 * the pointee STRUCT NAME first (leading-underscore resource types ->
 * texture/sampler), then fall back to address space for opaque pointees.
 * A pointee struct with a non-resource name (or anonymous, with real data
 * members) is a buffer. */
static int frag_resource_kind(const xlate_ctx_t *c, uint32_t arg_idx) {
    if (arg_idx >= c->num_args) return 0;
    uint32_t ty = c->arg_air_type_ids[arg_idx];
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (ty >= n || ts[ty].kind != LAGFX_AIR_TYPE_POINTER || ts[ty].num_op < 2u)
        return 0;
    uint32_t pointee   = ts[ty].op[0];
    uint32_t addrspace = ts[ty].op[1];
    if (pointee < n && ts[pointee].kind == LAGFX_AIR_TYPE_STRUCT_NAMED) {
        const char *nm = lagfx_air_module_string(c->m, ts[pointee].name_offset);
        if (nm) {
            /* Apple's opaque resource structs (strip a "struct." prefix). */
            const char *p = nm;
            if (strncmp(p, "struct.", 7u) == 0) p += 7u;
            if (strstr(p, "_texture") || strstr(p, "_depth")) return 1; /* texture */
            if (strstr(p, "_sampler")) return 2;                        /* sampler */
        }
        return 3;                    /* named data struct -> buffer */
    }
    if (pointee < n && ts[pointee].kind == LAGFX_AIR_TYPE_STRUCT_ANON)
        return 3;                    /* anonymous data struct -> buffer */
    /* A pointee that decodes to a CONCRETE data type — an array
     * (`device packed_float2*`), a vector (`device float4*`), or a
     * scalar (`device float*`) — is a `[[buffer(n)]]` whose element type
     * happens not to be wrapped in a struct. Real opaque resources
     * (texture/sampler) decode to LAGFX_AIR_TYPE_UNKNOWN (255) because
     * Apple's opaque-pointer AIR carries no concrete pointee. So only an
     * opaque pointee may fall through to the address-space heuristic;
     * a concrete data pointee is unambiguously a buffer. */
    if (pointee < n) {
        switch (ts[pointee].kind) {
            case LAGFX_AIR_TYPE_ARRAY:
            case LAGFX_AIR_TYPE_VECTOR:
            case LAGFX_AIR_TYPE_FLOAT:
            case LAGFX_AIR_TYPE_HALF:
            case LAGFX_AIR_TYPE_DOUBLE:
            case LAGFX_AIR_TYPE_INTEGER:
                return 3;            /* concrete data pointee -> buffer */
            default: break;
        }
    }
    if (addrspace == 1u) return 1;   /* texture (opaque) */
    if (addrspace == 2u) return 2;   /* sampler (opaque) */
    return 0;
}

/* True when the shader has any texture resource arg — the constexpr /
 * in-shader-sampler case. A shader (or one of its HELPER functions, e.g.
 * UberShader::sample_3d_lut, which takes a texture but NO sampler param)
 * may sample with a Metal `constexpr sampler` declared in the body, so
 * the air.sample_* sampler operand never resolves to a bound sampler
 * var. Provide one module-scope default sampler whenever textures exist;
 * if nothing references it, the binding stays statically unused (legal
 * to leave unwritten). */
static bool needs_default_sampler(const xlate_ctx_t *c) {
    uint32_t n = c->num_args < LAGFX_MAX_VERTEX_ARGS
                     ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
    for (uint32_t i = 0; i < n; i++) {
        if (frag_resource_kind(c, i) == 1) return true;
    }
    return false;
}

/* Shared OpTypeImage 2D float (Sampled=1, used with a sampler). */
static uint32_t emit_type_image2d_f(xlate_ctx_t *c) {
    if (c->id_image_t) return c->id_image_t;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, emit_type_float32(c), LAGFX_SPV_DIM_2D,
                       0u /*Depth*/, 0u /*Arrayed*/, 0u /*MS*/,
                       1u /*Sampled=1*/, LAGFX_SPV_IMAGE_FORMAT_UNKNOWN };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_IMAGE, ops, 8);
    c->id_image_t = id;
    return id;
}

static uint32_t emit_type_sampler(xlate_ctx_t *c) {
    if (c->id_sampler_t) return c->id_sampler_t;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_SAMPLER, ops, 1);
    c->id_sampler_t = id;
    return id;
}

static uint32_t emit_type_sampled_image(xlate_ctx_t *c) {
    if (c->id_sampimg_t) return c->id_sampimg_t;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, emit_type_image2d_f(c) };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_SAMPLED_IMAGE, ops, 2);
    c->id_sampimg_t = id;
    return id;
}

/* Single shared ptr-UniformConstant-image2d / -sampler types. Resource
 * variables AND helper-function tex/samp parameters must use the SAME
 * pointer type ids — OpFunctionCall matches arg/param types by ID. */
static uint32_t emit_type_ptr_uc_image(xlate_ctx_t *c) {
    if (c->id_ptr_uc_image) return c->id_ptr_uc_image;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT,
                       emit_type_image2d_f(c) };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
    c->id_ptr_uc_image = id;
    return id;
}

static uint32_t emit_type_ptr_uc_sampler(xlate_ctx_t *c) {
    if (c->id_ptr_uc_sampler) return c->id_ptr_uc_sampler;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT,
                       emit_type_sampler(c) };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
    c->id_ptr_uc_sampler = id;
    return id;
}

static uint32_t lagfx_round_up(uint32_t x, uint32_t a) {
    return (a == 0u) ? x : ((x + a - 1u) / a) * a;
}

/* std430 byte size + alignment of an AIR type. Matches Metal's natural C
 * layout (the AIR `e-p:64:64:64-...` datalayout), so computed Offsets line
 * up with the bytes the host uploads. Returns false for unhandled kinds. */
static bool air_type_size_align(xlate_ctx_t *c, uint32_t ty,
                                uint32_t *size, uint32_t *align) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (ty >= n) return false;
    const lagfx_air_type_t *t = &ts[ty];
    switch (t->kind) {
        case LAGFX_AIR_TYPE_FLOAT: *size = 4u; *align = 4u; return true;
        case LAGFX_AIR_TYPE_HALF: *size = 2u; *align = 2u; return true;
        case LAGFX_AIR_TYPE_DOUBLE: *size = 8u; *align = 8u; return true;
        case LAGFX_AIR_TYPE_INTEGER: {
            uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
            uint32_t b = w / 8u; if (b == 0u) b = 1u;
            *size = b; *align = b; return true;
        }
        case LAGFX_AIR_TYPE_VECTOR: {
            uint32_t lanes = t->num_op >= 1u ? t->op[0] : 4u;
            uint32_t elem  = t->num_op >= 2u ? t->op[1] : 0u;
            uint32_t es, ea; if (!air_type_size_align(c, elem, &es, &ea)) return false;
            *size  = es * lanes;                       /* vec3 = 12 (no pad) */
            *align = (lanes == 2u) ? es * 2u : es * 4u; /* vec3/4 align 4*elem */
            return true;
        }
        case LAGFX_AIR_TYPE_ARRAY: {
            uint32_t len  = t->num_op >= 1u ? t->op[0] : 0u;
            uint32_t elem = t->num_op >= 2u ? t->op[1] : 0u;
            uint32_t es, ea; if (!air_type_size_align(c, elem, &es, &ea)) return false;
            uint32_t stride = lagfx_round_up(es, ea);
            *size = stride * len; *align = ea; return true;
        }
        case LAGFX_AIR_TYPE_STRUCT_ANON:
        case LAGFX_AIR_TYPE_STRUCT_NAMED: {
            uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
            uint32_t off = 0u, maxa = 1u;
            for (uint32_t i = 0; i < nfields; i++) {
                uint32_t ms, ma;
                if (!air_type_size_align(c, t->op[1u + i], &ms, &ma)) return false;
                off = lagfx_round_up(off, ma) + ms;
                if (ma > maxa) maxa = ma;
            }
            *size = lagfx_round_up(off, maxa); *align = maxa; return true;
        }
        default: return false;
    }
}

/* Look up the pre-allocated SPIR-V id for a [[buffer(n)]] block struct
 * (populated by block_struct_decorate in the prologue). 0 if not handled. */
static uint32_t block_struct_spv_id(const xlate_ctx_t *c, uint32_t struct_ty) {
    for (uint32_t i = 0; i < c->block_cache_len; i++)
        if (c->block_cache[i].air_ty == struct_ty) return c->block_cache[i].spv_id;
    return 0u;
}

/* True when a buffer arg's pointee is a CONCRETE data type that is NOT a
 * struct — an array element (`device packed_float2*` -> [2 x float]), a
 * vector (`device float4*`), or a scalar (`device float*`). Apple emits
 * such `[[buffer(n)]]` args with an unwrapped pointee (no enclosing
 * struct). We model them as a single-member Block `{ runtimearray<pointee> }`
 * — an unbounded `device T*` — so the body's GEP/load lowers to a
 * StorageBuffer OpAccessChain. (A struct pointee keeps the existing
 * Block == struct path.) Returns the pointee AIR type id, or 0 if the
 * pointee is a struct / opaque / unmodellable. */
static uint32_t buffer_arg_nonstruct_pointee(xlate_ctx_t *c, uint32_t pointee_ty) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (pointee_ty >= n) return 0u;
    uint32_t ms, ma;
    switch (ts[pointee_ty].kind) {
        case LAGFX_AIR_TYPE_ARRAY:
        case LAGFX_AIR_TYPE_VECTOR:
        case LAGFX_AIR_TYPE_FLOAT:
        case LAGFX_AIR_TYPE_HALF:
        case LAGFX_AIR_TYPE_DOUBLE:
        case LAGFX_AIR_TYPE_INTEGER:
            return air_type_size_align(c, pointee_ty, &ms, &ma) ? pointee_ty : 0u;
        default:
            return 0u;
    }
}

/* Pure check (no emission): can we model this struct as a std430 Block?
 * Used during var allocation (before OpEntryPoint) so an unhandleable
 * buffer never gets a variable / interface entry. Arrays ARE handled (the
 * multi-section builder auto-routes the ArrayStride decoration). A non-struct
 * pointee (array/vector/scalar `device T*`) is also handleable — modelled as
 * a `{ runtimearray<T> }` Block (see buffer_arg_nonstruct_pointee). */
static bool block_struct_handleable(xlate_ctx_t *c, uint32_t struct_ty) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (struct_ty >= n) return false;
    const lagfx_air_type_t *t = &ts[struct_ty];
    if (t->kind != LAGFX_AIR_TYPE_STRUCT_ANON &&
        t->kind != LAGFX_AIR_TYPE_STRUCT_NAMED)
        return buffer_arg_nonstruct_pointee(c, struct_ty) != 0u;
    uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
    if (nfields == 0u || nfields > 32u) return false;
    for (uint32_t i = 0; i < nfields; i++) {
        uint32_t ms, ma;
        if (!air_type_size_align(c, t->op[1u + i], &ms, &ma)) return false;
    }
    return true;
}

/* Pointee struct AIR type of a buffer arg's pointer type (0 if none).
 * A prescanned access-type override (see arg_pointee_override) replaces
 * the declared pointee so every downstream consumer (Block synthesis,
 * runtime-array element, GEP walk) sees the type the body indexes with. */
static uint32_t buffer_arg_struct_ty(const xlate_ctx_t *c, uint32_t arg_idx) {
    if (arg_idx < LAGFX_MAX_VERTEX_ARGS && c->arg_pointee_override[arg_idx])
        return c->arg_pointee_override[arg_idx];
    uint32_t aty = c->arg_air_type_ids[arg_idx];
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    return (aty < n && ts[aty].kind == LAGFX_AIR_TYPE_POINTER && ts[aty].num_op >= 1u)
               ? ts[aty].op[0] : 0u;
}

/* Recursively emit std430 layout decorations for every aggregate REACHABLE
 * from a Block struct member: member Offset on nested structs, ArrayStride
 * on arrays (and arrays-of-structs / structs-of-arrays, to any depth). The
 * top-level Block struct's own Block + member Offsets are emitted by the
 * caller (it owns a locally-allocated sid not in the emit_air_type cache);
 * this walks INTO the field types, whose SPIR-V ids ARE the cached
 * emit_air_type ids. SPIR-V requires that every struct transitively used
 * in a laid-out (StorageBuffer/Uniform) Block carry Offset decorations and
 * every array an ArrayStride, else spirv-val rejects with "Structure ...
 * decorated as Block must be explicitly laid out with Offset decorations."
 * Dedup by AIR type id (a struct shared across members is decorated once;
 * duplicate decorations are legal but noisy). */
static void decorate_nested_layout(xlate_ctx_t *c, uint32_t air_ty,
                                   uint32_t *visited, uint32_t *nvisited) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (air_ty >= n) return;
    for (uint32_t i = 0; i < *nvisited; i++)
        if (visited[i] == air_ty) return;
    if (*nvisited < 64u) visited[(*nvisited)++] = air_ty;
    const lagfx_air_type_t *t = &ts[air_ty];
    if (t->kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
        t->kind == LAGFX_AIR_TYPE_STRUCT_NAMED) {
        uint32_t sid = emit_air_type(c, air_ty);  /* cached id */
        uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
        uint32_t off = 0u;
        for (uint32_t i = 0; i < nfields; i++) {
            uint32_t fty = t->op[1u + i];
            uint32_t ms, ma;
            if (!air_type_size_align(c, fty, &ms, &ma)) return;
            off = lagfx_round_up(off, ma);
            uint32_t md[] = { sid, i, LAGFX_SPV_DECORATION_OFFSET, off };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_MEMBER_DECORATE, md, 4);
            off += ms;
            decorate_nested_layout(c, fty, visited, nvisited);
        }
    } else if (t->kind == LAGFX_AIR_TYPE_ARRAY && t->num_op >= 2u) {
        uint32_t elem = t->op[1];
        uint32_t es, ea;
        if (air_type_size_align(c, elem, &es, &ea)) {
            uint32_t aid = emit_air_type(c, air_ty);  /* cached array id */
            uint32_t stride = lagfx_round_up(es, ea);
            uint32_t as[] = { aid, LAGFX_SPV_DECORATION_ARRAY_STRIDE, stride };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, as, 3);
        }
        decorate_nested_layout(c, elem, visited, nvisited);
    }
}

/* Emit an OpTypeRuntimeArray of `elem_ty` carrying the std430 ArrayStride.
 * Cached on the (elem) AIR type via a dedicated cache so repeated buffer
 * args of the same element type share one runtime-array id. */
static uint32_t emit_type_runtime_array(xlate_ctx_t *c, uint32_t elem_air,
                                        uint32_t elem_spv) {
    for (uint32_t i = 0; i < c->rtarr_cache_len; i++)
        if (c->rtarr_cache[i].air_ty == elem_air) return c->rtarr_cache[i].spv_id;
    uint32_t es, ea;
    if (!air_type_size_align(c, elem_air, &es, &ea)) return 0u;
    uint32_t stride = lagfx_round_up(es, ea);
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, elem_spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_RUNTIME_ARRAY, ops, 2);
    uint32_t dec[] = { id, LAGFX_SPV_DECORATION_ARRAY_STRIDE, stride };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, dec, 3);
    if (c->rtarr_cache_len < LAGFX_MAX_VERTEX_ARGS) {
        c->rtarr_cache[c->rtarr_cache_len].air_ty = elem_air;
        c->rtarr_cache[c->rtarr_cache_len].spv_id = id;
        c->rtarr_cache_len++;
    }
    return id;
}

/* Emit a std430 Block for a [[buffer(n)]] arg whose pointee is a CONCRETE
 * non-struct type (`device float4*`, `device packed_float2*`, ...). Modelled
 * as `{ runtimearray<pointee> }` (member 0 Offset 0) — an unbounded device
 * array, matching Metal's `device T*` semantics. Cached on the pointee AIR
 * type (the same key block_struct_spv_id / the cache use). 0 if unhandled. */
static uint32_t emit_type_nonstruct_block(xlate_ctx_t *c, uint32_t pointee_ty) {
    uint32_t elem_spv = emit_air_type(c, pointee_ty);
    if (!elem_spv) return 0u;
    /* Lay out a nested aggregate pointee (array element / vector is laid out
     * by the runtime-array's own stride, but an array-of-array element needs
     * its inner ArrayStride too). */
    uint32_t laid_out[64]; uint32_t n_laid_out = 0;
    decorate_nested_layout(c, pointee_ty, laid_out, &n_laid_out);

    uint32_t rt = emit_type_runtime_array(c, pointee_ty, elem_spv);
    if (!rt) return 0u;

    uint32_t sid = lagfx_spv_builder_alloc_id(c->b);
    uint32_t sops[] = { sid, rt };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_STRUCT, sops, 2);
    { uint32_t d[] = { sid, LAGFX_SPV_DECORATION_BLOCK };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d, 2); }
    { uint32_t md[] = { sid, 0u, LAGFX_SPV_DECORATION_OFFSET, 0u };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_MEMBER_DECORATE, md, 4); }

    /* StorageBuffer pointers the body's GEP/load will need: to the member
     * runtime-array, to the runtime-array element (pointee), and — if the
     * pointee is itself an array — to its element. */
    (void)emit_type_pointer(c, pointee_ty, elem_spv, LAGFX_SPV_STORAGE_STORAGE_BUFFER);
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (pointee_ty < n && ts[pointee_ty].kind == LAGFX_AIR_TYPE_ARRAY &&
        ts[pointee_ty].num_op >= 2u) {
        uint32_t inner = ts[pointee_ty].op[1];
        (void)emit_type_pointer(c, inner, emit_air_type(c, inner),
                                LAGFX_SPV_STORAGE_STORAGE_BUFFER);
    }

    if (c->block_cache_len < LAGFX_MAX_VERTEX_ARGS) {
        c->block_cache[c->block_cache_len].air_ty = pointee_ty;
        c->block_cache[c->block_cache_len].spv_id = sid;
        c->block_cache_len++;
    }
    return sid;
}

/* Emit a std430 Block struct for a [[buffer(n)]] arg: OpTypeStruct + Block /
 * member-Offset / ArrayStride decorations + StorageBuffer pointers to the
 * field (and array-element) types for the body GEP/AccessChain. The
 * multi-section builder auto-routes the OpType* to the types section and
 * the OpDecorate/OpMemberDecorate to the annotations section, so emitting
 * them together here (in module-vars) is legal — decorations forward-
 * reference the type ids. Cached by AIR struct type; 0 if unhandled. */
static uint32_t emit_type_struct_block(xlate_ctx_t *c, uint32_t struct_ty) {
    uint32_t cached = block_struct_spv_id(c, struct_ty);
    if (cached) return cached;
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (struct_ty >= n) return 0u;
    const lagfx_air_type_t *t = &ts[struct_ty];
    if (t->kind != LAGFX_AIR_TYPE_STRUCT_ANON &&
        t->kind != LAGFX_AIR_TYPE_STRUCT_NAMED) {
        /* Non-struct pointee -> synthesize a { runtimearray<T> } Block. */
        if (buffer_arg_nonstruct_pointee(c, struct_ty))
            return emit_type_nonstruct_block(c, struct_ty);
        return 0u;
    }
    uint32_t nfields = t->num_op > 1u ? t->num_op - 1u : 0u;
    if (nfields == 0u || nfields > 32u) {
        LAGFX_TRACE("block: struct ty %u nfields %u out of range", struct_ty,
                    nfields);
        return 0u;
    }

    uint32_t offsets[33], member_spv[33], off = 0u;
    for (uint32_t i = 0; i < nfields; i++) {
        uint32_t ms, ma;
        if (!air_type_size_align(c, t->op[1u + i], &ms, &ma)) {
            LAGFX_TRACE("block: struct ty %u field %u ty %u kind %d — "
                        "size/align failed", struct_ty, i, t->op[1u + i],
                        t->op[1u + i] < n ? (int)ts[t->op[1u + i]].kind : -1);
            return 0u;
        }
        off = lagfx_round_up(off, ma);
        offsets[i] = off;
        off += ms;
        member_spv[i] = emit_air_type(c, t->op[1u + i]);  /* -> types section */
    }

    uint32_t sid = lagfx_spv_builder_alloc_id(c->b);
    uint32_t sops[34];
    sops[0] = sid;
    for (uint32_t i = 0; i < nfields; i++) sops[1u + i] = member_spv[i];
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_STRUCT, sops, 1u + nfields);

    { uint32_t d[] = { sid, LAGFX_SPV_DECORATION_BLOCK };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d, 2); }
    /* Nested-layout dedup set, seeded with the top-level struct so a
     * member that (cyclically) references it isn't re-decorated. */
    uint32_t laid_out[64]; uint32_t n_laid_out = 0;
    if (n_laid_out < 64u) laid_out[n_laid_out++] = struct_ty;
    for (uint32_t i = 0; i < nfields; i++) {
        uint32_t md[] = { sid, i, LAGFX_SPV_DECORATION_OFFSET, offsets[i] };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_MEMBER_DECORATE, md, 4);
        uint32_t fty = t->op[1u + i];
        /* StorageBuffer pointer to the field (GEP-to-field result). */
        (void)emit_type_pointer(c, fty, member_spv[i], LAGFX_SPV_STORAGE_STORAGE_BUFFER);
        /* Recursively lay out nested structs / arrays (member Offset +
         * ArrayStride to any depth), incl. this field if it's an array. */
        decorate_nested_layout(c, fty, laid_out, &n_laid_out);
        if (fty < n && ts[fty].kind == LAGFX_AIR_TYPE_ARRAY && ts[fty].num_op >= 2u) {
            /* StorageBuffer pointer to the array element (GEP-into-array). */
            uint32_t elem = ts[fty].op[1];
            (void)emit_type_pointer(c, elem, emit_air_type(c, elem),
                                    LAGFX_SPV_STORAGE_STORAGE_BUFFER);
        }
    }
    if (c->block_cache_len < LAGFX_MAX_VERTEX_ARGS) {
        c->block_cache[c->block_cache_len].air_ty = struct_ty;
        c->block_cache[c->block_cache_len].spv_id = sid;
        c->block_cache_len++;
    }
    return sid;
}

/* Cached OpTypePointer(StorageBuffer, <Block>) for a buffer pointee —
 * shared between the entry's [[buffer(n)]] OpVariable declarations and
 * helper STRUCTBUF params (arg/param types must match by ID). Key: the
 * pointee AIR type (struct or non-struct, same key emit_type_struct_block
 * caches its Block under). 0 if the Block itself is unhandled. */
static uint32_t emit_type_block_ptr(xlate_ctx_t *c, uint32_t pointee_ty) {
    for (uint32_t i = 0; i < c->blockptr_cache_len; i++)
        if (c->blockptr_cache[i].air_ty == pointee_ty)
            return c->blockptr_cache[i].spv_id;
    uint32_t block = emit_type_struct_block(c, pointee_ty);
    if (!block) return 0u;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { id, LAGFX_SPV_STORAGE_STORAGE_BUFFER, block };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, ops, 3);
    if (c->blockptr_cache_len < LAGFX_MAX_VERTEX_ARGS) {
        c->blockptr_cache[c->blockptr_cache_len].air_ty = pointee_ty;
        c->blockptr_cache[c->blockptr_cache_len].spv_id = id;
        c->blockptr_cache_len++;
    }
    return id;
}

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

    /* Int8 / Int16 capabilities are emitted lazily at the point of use in
     * emit_type_int_w — the multi-section builder hoists OpCapability into
     * the preamble, so no pre-scan is needed (previously this only scanned
     * CALL results and missed i8 from buffer struct fields / function-
     * constant predicate types). */

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
        uint32_t n_va = c->num_args < LAGFX_MAX_VERTEX_ARGS
                            ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
        /* One Input variable per VID/ATTR vertex arg. A pointer arg is a
         * resource (texture/sampler/buffer) — vertex shaders read a
         * [[buffer(n)]] for their transform matrices etc.; handle it like
         * the fragment stage. */
        for (uint32_t i = 0; i < n_va; i++) {
            lagfx_varg_kind_t k = vertex_arg_kind(c, i);
            if (k != LAGFX_VARG_SKIP) {
                c->arg_input_var_ids[i] = lagfx_spv_builder_alloc_id(c->b);
                if (k == LAGFX_VARG_VID && !c->id_vid_var)
                    c->id_vid_var = c->arg_input_var_ids[i];
            } else {
                int rk = frag_resource_kind(c, i);
                if (rk == 3 && !block_struct_handleable(c, buffer_arg_struct_ty(c, i)))
                    continue;
                if (rk != 0) {
                    c->arg_resource_var[i] = lagfx_spv_builder_alloc_id(c->b);
                    c->arg_resource_kind[i] = (uint8_t)rk;
                }
            }
        }

        /* Vertex output varyings: a struct {[[position]], varying...} return
         * feeds the fragment's stage_in. Alloc a Location-k Output var per
         * member so the fragment's interpolated inputs are fed (else UV=0 →
         * black). 0 for a bare float4 return. */
        c->num_vertex_outs = vertex_output_count(c);
        for (uint32_t k = 0; k < c->num_vertex_outs; k++)
            c->id_vertex_out_vars[k] = lagfx_spv_builder_alloc_id(c->b);

        /* 4. OpEntryPoint Vertex %main "main" %pos <arg inputs + resources> */
        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_VERTEX, c->id_main };
        uint32_t suffix[1u + 3u * LAGFX_MAX_VERTEX_ARGS + LAGFX_MAX_BYTEBUF_VIEWS];
        uint32_t n_iface = 0u;
        suffix[n_iface++] = c->id_position_var;
        for (uint32_t i = 0; i < n_va; i++) {
            if (c->arg_input_var_ids[i]) suffix[n_iface++] = c->arg_input_var_ids[i];
            if (c->arg_resource_var[i])  suffix[n_iface++] = c->arg_resource_var[i];
        }
        for (uint32_t k = 0; k < c->num_vertex_outs; k++)
            suffix[n_iface++] = c->id_vertex_out_vars[k];
        for (uint32_t v = 0; v < c->n_bytebuf_views; v++)
            suffix[n_iface++] = c->bytebuf_view[v].var_id;
        lagfx_spv_builder_emit_op_string(c->b, LAGFX_SPV_OP_ENTRY_POINT,
                                          prefix, 2, "main", suffix, n_iface);
        /* DescriptorSet 0 + sequential Binding for vertex resources. */
        {
            uint32_t binding = 0u;
            for (uint32_t i = 0; i < n_va; i++) {
                if (!c->arg_resource_var[i]) continue;
                uint32_t ds[] = { c->arg_resource_var[i],
                                   LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ds, 3);
                uint32_t bd[] = { c->arg_resource_var[i],
                                   LAGFX_SPV_DECORATION_BINDING, binding };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, bd, 3);
                /* Byte-buffer views alias the SAME descriptor (set 0,
                 * same binding) with a different Block type. */
                for (uint32_t v = 0; v < c->n_bytebuf_views; v++) {
                    if (c->bytebuf_view[v].arg != i) continue;
                    uint32_t vds[] = { c->bytebuf_view[v].var_id,
                                       LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, vds, 3);
                    uint32_t vbd[] = { c->bytebuf_view[v].var_id,
                                       LAGFX_SPV_DECORATION_BINDING, binding };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, vbd, 3);
                }
                binding++;
            }
        }

        /* 5. Decorations: position is BuiltIn Position; each arg is either
         * BuiltIn VertexIndex (VID) or Location N (ATTR). */
        {
            uint32_t ops[] = { c->id_position_var,
                                LAGFX_SPV_DECORATION_BUILTIN,
                                LAGFX_SPV_BUILTIN_POSITION };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
        uint32_t loc = 0u;
        for (uint32_t i = 0; i < n_va; i++) {
            lagfx_varg_kind_t k = vertex_arg_kind(c, i);
            if (k == LAGFX_VARG_VID) {
                uint32_t ops[] = { c->arg_input_var_ids[i],
                                    LAGFX_SPV_DECORATION_BUILTIN,
                                    LAGFX_SPV_BUILTIN_VERTEX_INDEX };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
            } else if (k == LAGFX_VARG_ATTR) {
                uint32_t ops[] = { c->arg_input_var_ids[i],
                                    LAGFX_SPV_DECORATION_LOCATION, loc++ };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
            }
        }
        /* Vertex output varyings: Location k (matches the fragment's Location-k
         * stage_in inputs). Member 0 is also gl_Position (BuiltIn, above). */
        for (uint32_t k = 0; k < c->num_vertex_outs; k++) {
            uint32_t ops[] = { c->id_vertex_out_vars[k],
                                LAGFX_SPV_DECORATION_LOCATION, k };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
    } else /* fragment */ {
        /* One Output variable per colour render target (1 for a bare
         * float/float4 return, N for a struct-of-members multi-target). */
        c->num_color_outputs = fragment_output_count(c);
        for (uint32_t k = 0; k < c->num_color_outputs; k++)
            c->id_color_vars[k] = lagfx_spv_builder_alloc_id(c->b);
        c->id_color_var = c->id_color_vars[0];

        /* Stage-in inputs: each float/vector fragment arg becomes a
         * Location-decorated Input variable (mirrors the vertex ATTR path).
         * Without this, reads of an interpolated input (`in.uv`) resolve to
         * a mistyped OpUndef and break downstream ops (the branch_fragment
         * `FOrdGreaterThan` on an undef-bool). The `[[position]]` arg is
         * modelled as a plain Location input for now rather than BuiltIn
         * FragCoord — correct enough to validate and to read user inputs;
         * FragCoord + metadata-driven location matching is a refinement. */
        uint32_t n_fa = c->num_args < LAGFX_MAX_VERTEX_ARGS
                            ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
        for (uint32_t i = 0; i < n_fa; i++) {
            if (vertex_arg_kind(c, i) == LAGFX_VARG_ATTR)
                c->arg_input_var_ids[i] = lagfx_spv_builder_alloc_id(c->b);
            else {
                int rk = frag_resource_kind(c, i);
                /* Skip a buffer whose struct layout we can't model — it
                 * stays unbound (no var, no interface entry) rather than
                 * emitting a half-modelled block. */
                if (rk == 3 && !block_struct_handleable(c, buffer_arg_struct_ty(c, i)))
                    continue;
                if (rk != 0) {
                    c->arg_resource_var[i] = lagfx_spv_builder_alloc_id(c->b);
                    c->arg_resource_kind[i] = (uint8_t)rk;
                }
            }
        }
        /* A texture-with-no-sampler shader (constexpr sampler) needs one
         * module-scope default sampler so the body's sample has a bound
         * sampler to load. */
        if (needs_default_sampler(c))
            c->id_default_sampler_var = lagfx_spv_builder_alloc_id(c->b);

        uint32_t prefix[] = { LAGFX_SPV_EXECUTION_MODEL_FRAGMENT, c->id_main };
        uint32_t suffix[3u * LAGFX_MAX_VERTEX_ARGS + LAGFX_MAX_BYTEBUF_VIEWS];
        uint32_t n_iface = 0u;
        for (uint32_t k = 0; k < c->num_color_outputs; k++)
            suffix[n_iface++] = c->id_color_vars[k];
        for (uint32_t i = 0; i < n_fa; i++) {
            /* SPIR-V 1.4: list ALL used globals (Input + UniformConstant). */
            if (c->arg_input_var_ids[i]) suffix[n_iface++] = c->arg_input_var_ids[i];
            if (c->arg_resource_var[i])  suffix[n_iface++] = c->arg_resource_var[i];
        }
        if (c->id_default_sampler_var) suffix[n_iface++] = c->id_default_sampler_var;
        for (uint32_t v = 0; v < c->n_bytebuf_views; v++)
            suffix[n_iface++] = c->bytebuf_view[v].var_id;
        lagfx_spv_builder_emit_op_string(c->b, LAGFX_SPV_OP_ENTRY_POINT,
                                          prefix, 2, "main", suffix, n_iface);

        /* OpExecutionMode %main OriginUpperLeft */
        {
            uint32_t ops[] = { c->id_main, LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_EXECUTION_MODE, ops, 2);
        }
        /* OpDecorate %color_k Location k — render target k. */
        for (uint32_t k = 0; k < c->num_color_outputs; k++) {
            uint32_t ops[] = { c->id_color_vars[k], LAGFX_SPV_DECORATION_LOCATION, k };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
        /* OpDecorate each texture/sampler resource DescriptorSet 0 + a
         * sequential Binding (matches the air.texture(n)/sampler(n) order;
         * metadata-driven binding numbers are a refinement). */
        uint32_t binding = 0u;
        for (uint32_t i = 0; i < n_fa; i++) {
            if (!c->arg_resource_var[i]) continue;
            /* The buffer's Block / Offset / ArrayStride decorations are
             * emitted with the OpTypeStruct in module-vars — the
             * multi-section builder auto-routes them to the annotations
             * section, so they don't need to be hand-placed here. */
            uint32_t d_set[] = { c->arg_resource_var[i],
                                  LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d_set, 3);
            uint32_t d_bind[] = { c->arg_resource_var[i],
                                   LAGFX_SPV_DECORATION_BINDING, binding };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d_bind, 3);
            /* Byte-buffer views alias the SAME descriptor (set 0, same
             * binding) with a different Block type. */
            for (uint32_t v = 0; v < c->n_bytebuf_views; v++) {
                if (c->bytebuf_view[v].arg != i) continue;
                uint32_t vds[] = { c->bytebuf_view[v].var_id,
                                   LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, vds, 3);
                uint32_t vbd[] = { c->bytebuf_view[v].var_id,
                                   LAGFX_SPV_DECORATION_BINDING, binding };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, vbd, 3);
            }
            binding++;
        }
        /* The default sampler binds after the texture(s) in the same set. */
        if (c->id_default_sampler_var) {
            uint32_t d_set[] = { c->id_default_sampler_var,
                                  LAGFX_SPV_DECORATION_DESCRIPTOR_SET, 0u };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d_set, 3);
            uint32_t d_bind[] = { c->id_default_sampler_var,
                                   LAGFX_SPV_DECORATION_BINDING, binding++ };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, d_bind, 3);
        }
        /* OpDecorate each stage-in Input %arg Location N (input Location
         * space is separate from the output color's). */
        uint32_t loc = 0u;
        for (uint32_t i = 0; i < n_fa; i++) {
            if (!c->arg_input_var_ids[i]) continue;
            uint32_t ops[] = { c->arg_input_var_ids[i],
                                LAGFX_SPV_DECORATION_LOCATION, loc++ };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE, ops, 3);
        }
    }
}

/* Emit the OpVariable + type for each texture/sampler/buffer resource arg
 * (shared by the vertex and fragment stages) and bind the arg value-id to
 * its variable. DescriptorSet/Binding were decorated in the prologue. */
static void emit_arg_resource_vars(xlate_ctx_t *c, uint32_t n_args) {
    for (uint32_t i = 0; i < n_args; i++) {
        uint32_t rk = c->arg_resource_kind[i];
        if (rk == 0u || !c->arg_resource_var[i]) continue;
        if (rk == 3u) {
            /* [[buffer(n)]]: a StorageBuffer Block variable. The pointer
             * type comes from the shared blockptr cache — helper STRUCTBUF
             * params reuse the same id, and duplicate pointer declarations
             * (two args with one struct type) are invalid anyway. */
            uint32_t ptr_id = emit_type_block_ptr(c, buffer_arg_struct_ty(c, i));
            if (!ptr_id) continue;
            uint32_t var_ops[] = { ptr_id, c->arg_resource_var[i],
                                    LAGFX_SPV_STORAGE_STORAGE_BUFFER };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, var_ops, 3);
            bind_value_spv(c, c->arg_id_base + i, c->arg_resource_var[i]);
            set_value_storage(c, c->arg_id_base + i,
                              LAGFX_SPV_STORAGE_STORAGE_BUFFER);
            /* Track the arg's POINTER type — helper-call STRUCTBUF args
             * match param types by ID against exactly this. */
            set_value_spv_type(c, c->arg_id_base + i, ptr_id);
            /* Aliased byte-buffer VIEW variables: one Block per cast-dest
             * pointee, same descriptor as the raw arg (see bytebuf_view). */
            for (uint32_t v = 0; v < c->n_bytebuf_views; v++) {
                if (c->bytebuf_view[v].arg != i) continue;
                uint32_t vptr = emit_type_block_ptr(
                    c, c->bytebuf_view[v].pointee_ty);
                if (!vptr) continue;
                uint32_t vvar_ops[] = { vptr, c->bytebuf_view[v].var_id,
                                        LAGFX_SPV_STORAGE_STORAGE_BUFFER };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE,
                                          vvar_ops, 3);
            }
            continue;
        }
        /* Shared cached pointer type — helper-fn params must match by ID. */
        uint32_t ptr_id = (rk == 1u) ? emit_type_ptr_uc_image(c)
                                     : emit_type_ptr_uc_sampler(c);
        uint32_t var_ops[] = { ptr_id, c->arg_resource_var[i],
                                LAGFX_SPV_STORAGE_UNIFORM_CONSTANT };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, var_ops, 3);
        bind_value_spv(c, c->arg_id_base + i, c->arg_resource_var[i]);
    }
    /* Default (constexpr) sampler variable: a single immutable OpTypeSampler
     * UniformConstant, shared by every sample that finds no sampler arg. Its
     * DescriptorSet/Binding + interface entry were emitted in the prologue. */
    if (c->id_default_sampler_var) {
        uint32_t ptr_id = emit_type_ptr_uc_sampler(c);
        uint32_t var_ops[] = { ptr_id, c->id_default_sampler_var,
                                LAGFX_SPV_STORAGE_UNIFORM_CONSTANT };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, var_ops, 3);
    }
    if (c->id_image_t) (void)emit_type_sampled_image(c);
}

/* Pre-emit OpVariable for each ALLOCA at the top of the function's entry
 * block, binding the result value-id in the map. translate_body's ALLOCA
 * handler skips re-emission when it sees a pre-bound value-id. Shared by
 * the entry function and every helper function. */
static void emit_alloca_vars(xlate_ctx_t *c) {
    uint32_t alloca_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        bool produces = inst_produces_value(c, inst);
        if (inst->code == LAGFX_AIR_INST_ALLOCA && inst->num_ops >= 1u) {
            uint32_t alloc_air_ty = (uint32_t)inst->ops[0];
            uint32_t alloc_spv   = emit_air_type(c, alloc_air_ty);
            uint32_t ptr_spv     = emit_type_pointer(c, alloc_air_ty, alloc_spv,
                                                      LAGFX_SPV_STORAGE_FUNCTION);
            uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
            uint32_t ops[] = { ptr_spv, result_id, LAGFX_SPV_STORAGE_FUNCTION };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, ops, 3);
            bind_value_spv(c, alloca_val_id, result_id);
            set_result_air_type(c, alloca_val_id, alloc_air_ty);
        }
        if (produces) alloca_val_id++;
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

        /* One Output OpVariable per vertex output varying, typed to the struct
         * member type (float4/float2/...). Fed at RET; read by the fragment's
         * Location-k stage_in input. */
        for (uint32_t k = 0; k < c->num_vertex_outs; k++) {
            uint32_t out_ty = vertex_output_type_spv_idx(c, k);
            uint32_t id_ptr_out = lagfx_spv_builder_alloc_id(c->b);
            uint32_t op_pt[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, out_ty };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, op_pt, 3);
            uint32_t op_var[] = { id_ptr_out, c->id_vertex_out_vars[k],
                                  LAGFX_SPV_STORAGE_OUTPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, op_var, 3);
        }

        /* One Input OpVariable per vertex arg: a uint for vertex_id, or
         * the attribute's own type (vec2f etc.) for a stage-in attribute.
         * The matching Location/BuiltIn decoration was emitted in the
         * prologue; the OpLoad that reads it into the arg value-id is
         * emitted in the entry block (below). */
        uint32_t n_va = c->num_args < LAGFX_MAX_VERTEX_ARGS
                            ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
        for (uint32_t i = 0; i < n_va; i++) {
            lagfx_varg_kind_t k = vertex_arg_kind(c, i);
            if (k == LAGFX_VARG_SKIP) continue;
            uint32_t elem_spv = (k == LAGFX_VARG_VID)
                                  ? emit_type_int_w(c, 32u, 0u)
                                  : emit_air_type(c, c->arg_air_type_ids[i]);
            uint32_t ptr_id = lagfx_spv_builder_alloc_id(c->b);
            uint32_t pt_ops[] = { ptr_id, LAGFX_SPV_STORAGE_INPUT, elem_spv };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, pt_ops, 3);
            uint32_t var_ops[] = { ptr_id, c->arg_input_var_ids[i],
                                    LAGFX_SPV_STORAGE_INPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, var_ops, 3);
        }
        emit_arg_resource_vars(c, n_va);
    } else /* fragment */ {
        /* One Output OpVariable per render target, each typed to its actual
         * member/return type (float / float2 / float4), not a hardcoded
         * vec4. A struct return -> N targets (multi-plane YCbCr). */
        (void)id_v4f;
        for (uint32_t k = 0; k < c->num_color_outputs; k++) {
            uint32_t out_ty = fragment_output_type_spv_idx(c, k);
            uint32_t id_ptr_out = lagfx_spv_builder_alloc_id(c->b);
            uint32_t op_pt[] = { id_ptr_out, LAGFX_SPV_STORAGE_OUTPUT, out_ty };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, op_pt, 3);
            uint32_t op_var[] = { id_ptr_out, c->id_color_vars[k],
                                  LAGFX_SPV_STORAGE_OUTPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, op_var, 3);
        }

        /* One Input OpVariable per stage-in (ATTR) fragment arg, typed to
         * the arg's own float/vector type. The Location decoration was
         * emitted in the prologue; the OpLoad that reads it into the arg
         * value-id is emitted in the entry block (below). */
        uint32_t n_fa = c->num_args < LAGFX_MAX_VERTEX_ARGS
                            ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
        for (uint32_t i = 0; i < n_fa; i++) {
            if (vertex_arg_kind(c, i) != LAGFX_VARG_ATTR) continue;
            uint32_t elem_spv = emit_air_type(c, c->arg_air_type_ids[i]);
            uint32_t ptr_id = lagfx_spv_builder_alloc_id(c->b);
            uint32_t pt_ops[] = { ptr_id, LAGFX_SPV_STORAGE_INPUT, elem_spv };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, pt_ops, 3);
            uint32_t var_ops[] = { ptr_id, c->arg_input_var_ids[i],
                                    LAGFX_SPV_STORAGE_INPUT };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VARIABLE, var_ops, 3);
        }

        emit_arg_resource_vars(c, n_fa);
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
            case LAGFX_AIR_INST_CALL: {
                /* Non-void CALL we don't lower yet (e.g. a texture
                 * sample → struct{vec4,...}): the body binds a typed
                 * OpUndef placeholder, so its result type MUST be
                 * declared here, before OpFunction (SPIR-V layout: all
                 * type and constant decls precede the first function). */
                if (inst_produces_value(c, inst)) {
                    uint32_t ret_ty = call_return_air_type(c, inst);
                    if (ret_ty != 0u) (void)emit_air_type(c, ret_ty);
                }
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
            case LAGFX_AIR_INST_CMP:
            case LAGFX_AIR_INST_CMP2:
            case LAGFX_AIR_INST_SELECT:
            case LAGFX_AIR_INST_VSELECT: {
                /* Comparisons produce a bool result and OpSelect takes a
                 * bool condition. The CMP/SELECT handlers call
                 * emit_type_bool() mid-body, which would emit OpTypeBool
                 * into the function body — SPIR-V requires it before
                 * OpFunction ("OpTypeBool cannot appear in the graph
                 * definitions section"). Pre-emit it here (cached, so the
                 * body call is a no-op). */
                (void)emit_type_bool(c);
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
    (void)emit_type_vec3_f(c);
    (void)emit_type_vec4_f(c);
    /* Pre-warm common constants + OpUndefs the body fallbacks may want.
     * All OpConstant / OpUndef emissions must precede OpFunction per
     * SPIR-V §2.4. */
    (void)emit_const_uint32(c, 0u);
    /* 0.0 / 1.0 float constants for air.fast_saturate -> FClamp(x,0,1). */
    (void)emit_const_float32(c, 0.0f);
    (void)emit_const_float32(c, 1.0f);
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
     * be the first instructions in the function's first block. */
    emit_alloca_vars(c);

    /* Locals splice point: any Function OpVariable the dispatch emitter
     * creates mid-body (spills / phi vars / %blockvar) is inserted HERE at
     * finish() — after the pre-emitted alloca variables, before the first
     * non-variable instruction, keeping SPIR-V's variables-first rule. */
    lagfx_spv_builder_mark_locals(c->b);

    /* For each vertex arg: OpLoad its Input variable into the arg's
     * value-id. The loaded SPIR-V id IS the argument's value (a uint for
     * vertex_id, or the attribute's vec/float type). Must come AFTER all
     * OpVariable instructions in the entry block. */
    {
        uint32_t n_va = c->num_args < LAGFX_MAX_VERTEX_ARGS
                            ? c->num_args : LAGFX_MAX_VERTEX_ARGS;
        for (uint32_t i = 0; i < n_va; i++) {
            lagfx_varg_kind_t k = vertex_arg_kind(c, i);
            if (k == LAGFX_VARG_SKIP) continue;  /* no Input var to load */
            if (!c->arg_input_var_ids[i]) continue;
            uint32_t elem_spv = (k == LAGFX_VARG_VID)
                                  ? emit_type_int_w(c, 32u, 0u)
                                  : emit_air_type(c, c->arg_air_type_ids[i]);
            uint32_t id_load = lagfx_spv_builder_alloc_id(c->b);
            uint32_t ops[] = { elem_spv, id_load, c->arg_input_var_ids[i] };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
            c->arg_spv_ids[i] = id_load;
            bind_value_spv(c, c->arg_id_base + i, id_load);
        }
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

/* Resolve a CALL's return AIR type-index (0 if void / no explicit-type
 * slot / unresolvable). Mirrors emit_inst_call's explicit-type-slot
 * parsing: CALL = [attr, ccinfo, (fnty if ccinfo & 0x8000), callee,
 * args...]; the function type's op[1] is the return type-index. */
static uint32_t call_return_air_type(const xlate_ctx_t *c,
                                     const lagfx_air_inst_t *i) {
    /* Returns LAGFX_AIR_TYPE_NONE on failure (NOT 0 — type index 0 is a
     * valid return type, e.g. float; conflating it with "no type" dropped
     * non-void calls whose return type is index 0). */
    if (i->num_ops < 3u) return LAGFX_AIR_TYPE_NONE;
    uint32_t ccinfo = (uint32_t)i->ops[1];
    if (!(ccinfo & 0x8000)) return LAGFX_AIR_TYPE_NONE;  /* no explicit fnty slot */
    /* CALL_FMF (cc bit 17) inserts a fast-math-flags operand BEFORE the
     * fnty/callee/args (llvm::CallMarkersFlags). Skip it or every slot is
     * off by one. fnty then sits at ops[2 + has_fmf]. */
    uint32_t has_fmf = (ccinfo & (1u << 17)) ? 1u : 0u;
    uint32_t fnty_slot = 2u + has_fmf;
    if (i->num_ops <= fnty_slot) return LAGFX_AIR_TYPE_NONE;
    uint32_t fn_ty_idx = (uint32_t)i->ops[fnty_slot];
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    if (fn_ty_idx >= n_types ||
        ts[fn_ty_idx].kind != LAGFX_AIR_TYPE_FUNCTION ||
        ts[fn_ty_idx].num_op < 2u) {
        return LAGFX_AIR_TYPE_NONE;
    }
    return (uint32_t)ts[fn_ty_idx].op[1];          /* return-type index (may be 0) */
}

/* True iff the AIR type-index denotes void (or is unresolvable). */
static bool air_type_is_void(const xlate_ctx_t *c, uint32_t ty_idx) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    if (ty_idx >= n_types) return true;
    return ts[ty_idx].kind == LAGFX_AIR_TYPE_VOID;
}

/* Record a value-producing instruction's result AIR type, keyed by
 * VALUE-NUMBER offset (result_value_id - inst_id_base) — the SAME index
 * space the operand-type lookups read with. (Blocker A: the array was
 * WRITTEN by inst_idx but READ by value-number; the two diverge as soon
 * as a non-value-producing inst — e.g. a void CALL — sits between
 * producers, so an operand-type lookup read the wrong inst's type. A
 * non-void CALL desynced it the other way: its result took no value-id
 * at all. Keying both sides by value-number makes them agree.) */
static void set_result_air_type(xlate_ctx_t *c, uint32_t result_value_id,
                                uint32_t air_ty) {
    if (result_value_id < c->inst_id_base) return;
    uint32_t idx = result_value_id - c->inst_id_base;
    if (idx < c->num_insts) c->inst_result_air_type[idx] = air_ty;
}

static bool inst_produces_value(const xlate_ctx_t *c,
                                const lagfx_air_inst_t *i) {
    switch (i->code) {
        case LAGFX_AIR_INST_DECLAREBLOCKS:
        case LAGFX_AIR_INST_STORE:
        case LAGFX_AIR_INST_STORE_OLD:
        case LAGFX_AIR_INST_RET:
        case LAGFX_AIR_INST_BR:
        case LAGFX_AIR_INST_SWITCH:
        case LAGFX_AIR_INST_UNREACHABLE:
            return false;
        case LAGFX_AIR_INST_CALL: {
            /* Non-void CALLs (e.g. a texture sample → vec4) ARE value
             * producers in LLVM's relative value numbering, so they MUST
             * consume a value-id here or every downstream relative ref
             * desyncs (blocker A). Void calls (llvm.lifetime.*, and any
             * call with no resolvable explicit return type) do not. */
            uint32_t ret = call_return_air_type(c, i);
            return ret != LAGFX_AIR_TYPE_NONE && !air_type_is_void(c, ret);
        }
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


/* Lower `fptoui/fptosi float -> i1` to OpFOrdNotEqual(x, 0.0) — the SPIR-V
 * dest for i1 is OpTypeBool, which OpConvertFToU rejects ("Expected
 * unsigned int scalar or vector type as Result Type", Xgc). Returns true
 * if applied. Scalar and 2..4-lane vector dests. */
static bool emit_float_to_bool_cmp(xlate_ctx_t *c, uint32_t opval_spv,
                                   uint32_t dest_ty, uint32_t result_value_id) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t lanes = 1u, elem_ty = dest_ty;
    if (dest_ty < n_types && ts[dest_ty].kind == LAGFX_AIR_TYPE_VECTOR &&
        ts[dest_ty].num_op >= 2u) {
        lanes = (uint32_t)ts[dest_ty].op[0];
        elem_ty = ts[dest_ty].op[1];
    }
    if (elem_ty >= n_types || ts[elem_ty].kind != LAGFX_AIR_TYPE_INTEGER ||
        ts[elem_ty].num_op < 1u || ts[elem_ty].op[0] != 1u || lanes > 4u)
        return false;

    uint32_t bool_t = emit_type_bool(c);
    uint32_t res_t  = (lanes > 1u) ? emit_type_vec(c, bool_t, lanes) : bool_t;
    uint32_t zero   = emit_const_float32(c, 0.0f);
    if (lanes > 1u) {
        uint32_t vf = emit_type_vec(c, emit_type_float32(c), lanes);
        uint32_t zv[6]; uint32_t zid = lagfx_spv_builder_alloc_id(c->b);
        zv[0] = vf; zv[1] = zid;
        for (uint32_t k = 0; k < lanes; k++) zv[2+k] = zero;
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT,
                                  zv, 2u+lanes);
        zero = zid;
    }
    uint32_t rid = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { res_t, rid, opval_spv, zero };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FORD_NOT_EQUAL, ops, 4);
    bind_value_spv(c, result_value_id, rid);
    set_result_air_type(c, result_value_id, LAGFX_AIR_TYPE_BOOL);
    set_value_spv_type(c, result_value_id, res_t);
    return true;
}

/* Lower `uitofp/sitofp i1 -> float` (bool to float) to
 * OpSelect(cond, 1.0, 0.0) of the destination float type — SPIR-V bool is
 * not an integer, so OpConvert*ToF rejects it ("Expected input to be int
 * scalar or vector", the Xgc v3bool -> v3float conversion). Returns true
 * if the lowering applied (result bound); false -> caller emits the plain
 * conversion. Handles scalar float32 and 2..4-lane float32 vectors. */
static bool emit_bool_to_float_select(xlate_ctx_t *c, uint32_t opval_id,
                                      uint32_t opval_spv, uint32_t dest_ty,
                                      uint32_t result_value_id) {
    uint32_t src_ty = value_air_type_idx(c, opval_id);
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    bool src_bool = (src_ty == LAGFX_AIR_TYPE_BOOL);
    if (!src_bool && src_ty != LAGFX_AIR_TYPE_NONE && src_ty < n_types) {
        const lagfx_air_type_t *st = &ts[src_ty];
        if (st->kind == LAGFX_AIR_TYPE_VECTOR && st->num_op >= 2u &&
            st->op[1] < n_types)
            st = &ts[st->op[1]];
        src_bool = (st->kind == LAGFX_AIR_TYPE_INTEGER && st->num_op >= 1u &&
                    st->op[0] == 1u);
    }
    if (!src_bool) return false;

    uint32_t lanes = 1u, elem_ty = dest_ty;
    if (dest_ty < n_types && ts[dest_ty].kind == LAGFX_AIR_TYPE_VECTOR &&
        ts[dest_ty].num_op >= 2u) {
        lanes = (uint32_t)ts[dest_ty].op[0];
        elem_ty = ts[dest_ty].op[1];
    }
    if (elem_ty >= n_types || ts[elem_ty].kind != LAGFX_AIR_TYPE_FLOAT ||
        lanes > 4u)
        return false;   /* half / exotic — keep the plain conversion */

    uint32_t dest_spv = emit_air_type(c, dest_ty);
    uint32_t one  = emit_const_float32(c, 1.0f);
    uint32_t zero = emit_const_float32(c, 0.0f);
    if (lanes > 1u) {
        uint32_t one_v[6], zero_v[6];
        uint32_t one_id  = lagfx_spv_builder_alloc_id(c->b);
        uint32_t zero_id = lagfx_spv_builder_alloc_id(c->b);
        one_v[0] = dest_spv;  one_v[1] = one_id;
        zero_v[0] = dest_spv; zero_v[1] = zero_id;
        for (uint32_t k = 0; k < lanes; k++) { one_v[2+k]=one; zero_v[2+k]=zero; }
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, one_v, 2u+lanes);
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, zero_v, 2u+lanes);
        one = one_id; zero = zero_id;
    }
    /* The condition must match the result's lane count. If the source is a
     * scalar bool but the dest is a vector, broadcast the condition. */
    uint32_t cond = opval_spv;
    if (lanes > 1u) {
        uint32_t want_bvec = emit_type_vec(c, emit_type_bool(c), lanes);
        uint32_t have = get_value_spv_type(c, opval_id);
        bool cond_is_vec = (have == want_bvec);
        if (!cond_is_vec && src_ty != LAGFX_AIR_TYPE_NONE &&
            src_ty != LAGFX_AIR_TYPE_BOOL && src_ty < n_types &&
            ts[src_ty].kind == LAGFX_AIR_TYPE_VECTOR)
            cond_is_vec = true;
        if (!cond_is_vec) {
            uint32_t cv[6];
            uint32_t cv_id = lagfx_spv_builder_alloc_id(c->b);
            cv[0] = want_bvec; cv[1] = cv_id;
            for (uint32_t k = 0; k < lanes; k++) cv[2+k] = cond;
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT,
                                      cv, 2u+lanes);
            cond = cv_id;
        }
    }
    uint32_t rid = lagfx_spv_builder_alloc_id(c->b);
    uint32_t sel[] = { dest_spv, rid, cond, one, zero };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECT, sel, 5);
    bind_value_spv(c, result_value_id, rid);
    set_result_air_type(c, result_value_id, dest_ty);
    set_value_spv_type(c, result_value_id, dest_spv);
    return true;
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
    /* The undef fallback must be in the cast's SOURCE type family:
     * FPTRUNC/FPEXT/FPTOUI/FPTOSI take a FLOAT source — an int-typed
     * undef feeds OpFConvert "Expected input to be float scalar or
     * vector" (Xgc). FPEXT to float32 implies a half source (FConvert
     * requires differing widths). */
    uint32_t src_fallback;
    switch (cast_op) {
        case 3: case 4: case 7:                 /* FPTOUI/FPTOSI/FPTRUNC */
            src_fallback = emit_type_float32(c); break;
        case 8:                                 /* FPEXT (to f32) ← half */
            src_fallback = emit_type_half(c);    break;
        default:
            src_fallback = emit_type_int_w(c, 32u, 0u); break;
    }
    uint32_t opval_spv = resolve_or_undef(c, opval_id, src_fallback);

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
            if (emit_float_to_bool_cmp(c, opval_spv, dest_ty, result_value_id))
                return;
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_F_TO_U, ops, 3); }
            break;

        case 4:  /* CAST_FPTOSI → OpConvertFToS (float→signed int) */
            if (emit_float_to_bool_cmp(c, opval_spv, dest_ty, result_value_id))
                return;
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_F_TO_S, ops, 3); }
            break;

        case 5:  /* CAST_UITOFP → OpConvertUToF (unsigned int→float) */
            if (emit_bool_to_float_select(c, opval_id, opval_spv, dest_ty,
                                          result_value_id))
                return;
            if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
            { uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
              uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
              lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONVERT_U_TO_F, ops, 3); }
            break;

        case 6:  /* CAST_SITOFP → OpConvertSToF (signed int→float) */
            /* sitofp i1 true is -1.0 in LLVM; Metal emits uitofp for bool
             * -> 0/1, so the 1.0/0.0 select is the shape that occurs. */
            if (emit_bool_to_float_select(c, opval_id, opval_spv, dest_ty,
                                          result_value_id))
                return;
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
            set_result_air_type(c, result_value_id, dest_ty);
            return;

        case 10: /* CAST_INTTOPTR → alias (int→pointer not representable in SPIR-V) */
            bind_value_spv(c, result_value_id, opval_spv);
            set_result_air_type(c, result_value_id, dest_ty);
            return;

        case 11: /* CAST_BITCAST */
            /* A bitcast between two NUMERIC types (scalar/vector of int or
             * float of the same total bit width — e.g. `as_type<uint>(float)`,
             * `bitcast <2 x float> to <2 x i32>`) is a genuine reinterpret:
             * the result has a DIFFERENT SPIR-V type than the source, so the
             * old "always alias" path left a float vector flowing into an int
             * consumer (extractelement → "Vector component type to be equal
             * to Result Type"). Emit a real OpBitcast for those. POINTER
             * bitcasts (and anything not representable) still alias — SPIR-V
             * Logical addressing has no general pointer reinterpret, and the
             * GEP/access-chain path handles the addressing. The multi-section
             * builder hoists the dest type, so emitting it here is ordering-safe. */
            {
                uint32_t n_types = 0u;
                const lagfx_air_type_t *ts =
                    lagfx_air_module_types(c->m, &n_types);
                int numeric = 0;
                if (dest_ty < n_types) {
                    const lagfx_air_type_t *dt = &ts[dest_ty];
                    if (dt->kind == LAGFX_AIR_TYPE_INTEGER ||
                        dt->kind == LAGFX_AIR_TYPE_FLOAT ||
                        dt->kind == LAGFX_AIR_TYPE_HALF) {
                        numeric = 1;
                    } else if (dt->kind == LAGFX_AIR_TYPE_VECTOR &&
                               dt->num_op >= 2u) {
                        uint32_t el = dt->op[1];
                        if (el < n_types &&
                            (ts[el].kind == LAGFX_AIR_TYPE_INTEGER ||
                             ts[el].kind == LAGFX_AIR_TYPE_FLOAT ||
                             ts[el].kind == LAGFX_AIR_TYPE_HALF)) {
                            numeric = 1;
                        }
                    }
                }
                if (numeric) {
                    uint32_t dest_ty_spv = emit_air_type(c, dest_ty);
                    /* OpBitcast requires EQUAL total bit widths. When the
                     * source's tracked AIR type disagrees (a mis-resolved
                     * operand through pending control flow — Xgc's
                     * `OpBitcast %uint %v4float`), bind an undef of the
                     * dest type instead of emitting an invalid cast. */
                    uint32_t src_ty = value_air_type_idx(c, opval_id);
                    bool width_ok = false;
                    if (src_ty != LAGFX_AIR_TYPE_NONE &&
                        src_ty != LAGFX_AIR_TYPE_BOOL) {
                        uint32_t sw, sa, dw, da;
                        width_ok = air_type_size_align(c, src_ty, &sw, &sa) &&
                                   air_type_size_align(c, dest_ty, &dw, &da) &&
                                   sw == dw;
                    }
                    if (!width_ok) {
                        bind_value_spv(c, result_value_id,
                                       emit_undef(c, dest_ty_spv));
                        set_result_air_type(c, result_value_id, dest_ty);
                        return;
                    }
                    if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t ops[] = { dest_ty_spv, result_spv, opval_spv };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BITCAST, ops, 3);
                    bind_value_spv(c, result_value_id, result_spv);
                    set_result_air_type(c, result_value_id, dest_ty);
                    return;
                }
            }
            /* A registered byte-buffer VIEW binds to its own aliased Block
             * variable (same descriptor set/binding as the raw arg). */
            if (c->value_view && result_value_id < c->value_id_capacity &&
                c->value_view[result_value_id]) {
                uint32_t v = (uint32_t)c->value_view[result_value_id] - 1u;
                if (v < c->n_bytebuf_views) {
                    bind_value_spv(c, result_value_id,
                                   c->bytebuf_view[v].var_id);
                    set_result_air_type(c, result_value_id, dest_ty);
                    set_value_storage(c, result_value_id,
                                      LAGFX_SPV_STORAGE_STORAGE_BUFFER);
                    return;
                }
            }
            /* Pointer / non-numeric bitcast — alias (identical bit
             * representation; downstream uses the aliased id directly).
             * Propagate the operand's pointer storage class so a later
             * GEP through the alias keeps the base's class. */
            bind_value_spv(c, result_value_id, opval_spv);
            set_result_air_type(c, result_value_id, dest_ty);
            {
                uint32_t sc = get_value_storage(c, opval_id);
                if (sc != UINT32_MAX) set_value_storage(c, result_value_id, sc);
            }
            return;

        case 12: /* CAST_ADDRSPACECAST → alias for now */
            if (c->value_view && result_value_id < c->value_id_capacity &&
                c->value_view[result_value_id]) {
                uint32_t v = (uint32_t)c->value_view[result_value_id] - 1u;
                if (v < c->n_bytebuf_views) {
                    bind_value_spv(c, result_value_id,
                                   c->bytebuf_view[v].var_id);
                    set_result_air_type(c, result_value_id, dest_ty);
                    set_value_storage(c, result_value_id,
                                      LAGFX_SPV_STORAGE_STORAGE_BUFFER);
                    return;
                }
            }
            bind_value_spv(c, result_value_id, opval_spv);
            set_result_air_type(c, result_value_id, dest_ty);
            {
                uint32_t sc = get_value_storage(c, opval_id);
                if (sc != UINT32_MAX) set_value_storage(c, result_value_id, sc);
            }
            return;

        default:
            /* Unknown cast opcode — fall back to aliasing */
            bind_value_spv(c, result_value_id, opval_spv);
            set_result_air_type(c, result_value_id, dest_ty);
            return;
    }

    /* Bind the result value-id and record the AIR type for downstream ops */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, dest_ty);
}

static uint32_t buffer_arg_runtimearray_elem(xlate_ctx_t *c,
                                             uint32_t ptr_value_id);

/* Emit an OpFunctionCall to a registered (already-emitted) helper
 * function. Returns true when the call was emitted and the result bound;
 * false to fall through to the typed-undef degradation path.
 *
 * Argument lowering per param kind:
 *   VALUE     — resolve; a KNOWN type mismatch degrades that arg to a
 *               typed undef of the param type (never an ill-typed op).
 *   TEX/SAMP  — the arg must resolve to a bound resource VARIABLE (or a
 *               helper's own resource param, which lives in the same
 *               arg_resource_var table); otherwise fall back to the
 *               default sampler / first bound resource of the kind, and
 *               if none exists degrade the whole call.
 *   THREADPTR — interior pointers are not legal call args under Logical
 *               addressing; pass a fresh Function temp with copy-in
 *               (when the source pointer's type is known to match) and
 *               copy-back after the call (`thread T&` out-params). */
static bool emit_helper_fncall(xlate_ctx_t *c, uint32_t fn_idx,
                               const lagfx_air_inst_t *inst,
                               uint32_t callee_slot_idx,
                               uint32_t result_value_id,
                               uint32_t next_val_id,
                               const char *fn_name) {
    xlate_helpers_t *h = c->helpers;
    if (!h) return false;
    xlate_helper_fn_t *hf = NULL;
    for (uint32_t i = 0; i < h->n_fns; i++) {
        if (h->fns[i].fn_idx == fn_idx) { hf = &h->fns[i]; break; }
    }
    if (!hf || !hf->func_id || !hf->emitted) return false;
    if (inst->num_ops < callee_slot_idx + 1u + hf->n_params) return false;
    /* Void-ness must agree between the registry (declared fn type) and the
     * call site's value numbering, or downstream ids desync. */
    if ((hf->ret_spv == 0u) != (result_value_id == 0u)) return false;

    uint32_t argv[LAGFX_MAX_HELPER_PARAMS];
    struct { uint32_t src, temp, pointee; } cb[LAGFX_MAX_HELPER_PARAMS];
    uint32_t n_cb = 0;

    for (uint32_t i = 0; i < hf->n_params; i++) {
        uint32_t vid = resolve_relative(
            (uint32_t)inst->ops[callee_slot_idx + 1u + i], next_val_id);
        switch (hf->param[i].kind) {
            case LAGFX_HPARAM_VALUE: {
                uint32_t a = resolve_value_spv(c, vid);
                if (a) {
                    uint32_t vt = get_value_spv_type(c, vid);
                    if (vt && vt != hf->param[i].spv_ty) a = 0u;
                }
                if (!a) a = emit_undef(c, hf->param[i].spv_ty);
                argv[i] = a;
                break;
            }
            case LAGFX_HPARAM_TEX:
            case LAGFX_HPARAM_SAMP: {
                uint8_t want = (hf->param[i].kind == LAGFX_HPARAM_TEX) ? 1u : 2u;
                uint32_t a = resolve_value_spv(c, vid);
                bool ok = false;
                uint32_t first = 0u;
                for (uint32_t k = 0; k < c->num_args &&
                         k < LAGFX_MAX_VERTEX_ARGS; k++) {
                    if (c->arg_resource_kind[k] != want ||
                        !c->arg_resource_var[k]) continue;
                    if (!first) first = c->arg_resource_var[k];
                    if (a == c->arg_resource_var[k]) ok = true;
                }
                if (want == 2u && a && a == c->id_default_sampler_var) ok = true;
                if (!ok)
                    a = (want == 2u && c->id_default_sampler_var)
                            ? c->id_default_sampler_var : first;
                if (!a) return false;   /* no resource of the kind — degrade */
                argv[i] = a;
                break;
            }
            case LAGFX_HPARAM_THREADPTR: {
                uint32_t temp = lagfx_spv_builder_alloc_id(c->b);
                if (!lagfx_spv_builder_emit_local_var(c->b, hf->param[i].spv_ty,
                                                      temp))
                    return false;
                uint32_t src = resolve_value_spv(c, vid);
                if (src && get_value_spv_type(c, vid) == hf->param[i].spv_ty &&
                    n_cb < LAGFX_MAX_HELPER_PARAMS) {
                    uint32_t ld = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t lo[] = { hf->param[i].pointee_spv, ld, src };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, lo, 3);
                    uint32_t so[] = { temp, ld };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
                    cb[n_cb].src = src;
                    cb[n_cb].temp = temp;
                    cb[n_cb].pointee = hf->param[i].pointee_spv;
                    n_cb++;
                }
                argv[i] = temp;
                break;
            }
            case LAGFX_HPARAM_DATABUF: {
                /* Interior StorageBuffer pointer, passed directly (legal
                 * with VariablePointersStorageBuffer). The arg's tracked
                 * pointer TYPE must equal the param's exactly (call
                 * arg/param types match by ID); otherwise degrade the
                 * whole call. */
                uint32_t a = resolve_value_spv(c, vid);
                if (a && get_value_spv_type(c, vid) == hf->param[i].spv_ty) {
                    argv[i] = a;
                    break;
                }
                /* Whole `device T*` BUFFER ARG passed as a T* param (the
                 * asg77_sample LUT): the arg is the { runtimearray<T> }
                 * Block VARIABLE — synthesize &block[0][0], which has
                 * exactly the param's interior-pointer type. */
                uint32_t elem = buffer_arg_runtimearray_elem(c, vid);
                if (a && elem && elem == hf->param[i].pointee_air) {
                    uint32_t chain = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t zero = emit_const_uint32(c, 0u);
                    uint32_t ac[] = { hf->param[i].spv_ty, chain, a, zero, zero };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_ACCESS_CHAIN,
                                              ac, 5);
                    argv[i] = chain;
                    break;
                }
                return false;
            }
            case LAGFX_HPARAM_STRUCTBUF: {
                /* The arg must be the Block VARIABLE (an entry buffer arg,
                 * a GEP-0 alias of it, or an enclosing helper's own
                 * STRUCTBUF param) with the shared blockptr type — types
                 * match by ID; otherwise degrade the whole call. */
                uint32_t a = resolve_value_spv(c, vid);
                if (a && get_value_spv_type(c, vid) == hf->param[i].spv_ty) {
                    argv[i] = a;
                    break;
                }
                return false;
            }
            default:
                return false;
        }
    }

    uint32_t res = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[3u + LAGFX_MAX_HELPER_PARAMS];
    ops[0] = hf->ret_spv ? hf->ret_spv : emit_type_void(c);
    ops[1] = res;
    ops[2] = hf->func_id;
    for (uint32_t i = 0; i < hf->n_params; i++) ops[3u + i] = argv[i];
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_CALL, ops,
                              3u + hf->n_params);

    /* Copy-back for thread out-params. */
    for (uint32_t i = 0; i < n_cb; i++) {
        uint32_t ld = lagfx_spv_builder_alloc_id(c->b);
        uint32_t lo[] = { cb[i].pointee, ld, cb[i].temp };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, lo, 3);
        uint32_t so[] = { cb[i].src, ld };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
    }

    if (result_value_id != 0u && hf->ret_spv) {
        bind_value_spv(c, result_value_id, res);
        set_result_air_type(c, result_value_id, hf->ret_air);
        set_value_spv_type(c, result_value_id, hf->ret_spv);
    }
    LAGFX_TRACE("call: helper '%s' fn[%u] → OpFunctionCall (value-id %u)",
                fn_name, fn_idx, result_value_id);
    return true;
}

static void emit_inst_call(xlate_ctx_t *c, uint32_t inst_idx,
                             const lagfx_air_inst_t *inst,
                             uint32_t result_value_id, uint32_t next_val_id) {
    (void)inst_idx;
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
    /* CALL_FMF (cc bit 17, llvm::CallMarkersFlags) inserts a fast-math-
     * flags operand BEFORE fnty/callee/args. Skip it or the callee + every
     * arg slot is off by one (→ callee resolves to a bogus value-id and the
     * whole call gets dropped, breaking downstream type propagation).
     * Metal emits fast-math on nearly every op, so this is the common path. */
    uint32_t has_fmf = (ccinfo & (1u << 17)) ? 1u : 0u;

    /* Determine callee_rel slot index:
     *   [paramattrs, cc, (FMF if bit17), (fnty if bit15), callee, args...] */
    uint32_t callee_slot_idx = 2u + has_fmf + (has_explicit_type ? 1u : 0u);
    if (inst->num_ops <= callee_slot_idx) return;

    uint64_t callee_rel_raw = inst->ops[callee_slot_idx];
    uint32_t callee_rel = (uint32_t)callee_rel_raw;

    /* Resolve callee to absolute value-id. */
    uint32_t callee_id = resolve_relative(callee_rel, next_val_id);

    /* Look up the function name via module-level functions table. The
     * absolute value-id space is [globalvars | functions | module-consts |
     * args | local-consts | insts]; functions begin at value-id
     * n_globalvars (LLVM enumerates globals before functions). So the
     * function-table index is callee_id - n_globalvars, NOT callee_id.
     * (Before GLOBALVAR records were counted, n_globalvars was 0 and the
     * raw id happened to work; now every shader has >=1 globalvar
     * (@llvm.global_ctors), and a constexpr-sampler shader has 2, which is
     * why air.sample_texture_2d resolved past the table and dropped ->
     * the result undef took the global_ctors array type, yielding an
     * invalid zero-length OpTypeArray downstream.) */
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(c->m, &n_fns);
    uint32_t n_globalvars = lagfx_air_module_num_globalvars(c->m);
    if (callee_id < n_globalvars || (callee_id - n_globalvars) >= n_fns) {
        /* Not a function reference — drop the call. */
        LAGFX_TRACE("call: callee_id=%u (globalvars=%u, n_fns=%u) not a fn — drop",
                    callee_id, n_globalvars, n_fns);
        return;
    }
    uint32_t fn_idx = callee_id - n_globalvars;

    const char *fn_name = lagfx_air_module_string(c->m, fns[fn_idx].name_offset);
    if (!fn_name) {
        LAGFX_TRACE("call: no name for fn[%u] — drop", callee_id);
        return;
    }

    /* Resolve the CALL's result AIR type once, up front. The explicit-type
     * slot (ops[2], present when ccinfo & 0x8000) is a FUNCTION type whose
     * op[1] is the return type. We need this BEFORE the intrinsic dispatch
     * so the unrecognized-call path (texture sample etc.) can bind a
     * correctly-typed placeholder for downstream value-numbering. */
    uint32_t result_ty_air = call_return_air_type(c, inst);

   /* Intrinsic dispatch table: air.* fast-math intrinsics → GLSL.std.450
     * instruction numbers (from spv_builder.h LAGFX_SPV_GLSL_* constants).
     * Apple uses air.fast.<x> and air.<x> interchangeably; we match by
     * string prefix so "air.fast.sqrt.f32" matches the Sqrt row.
     * Reference: paravirt-re/library/air_intrinsic_spec docs. */
    static const struct {
        const char *prefix;
        uint32_t    glsl_inst;
        uint8_t     num_args;
    } intrinsic_table[] = {
        /* Unary intrinsics (num_args=1) */
        {"air.fast.sqrt",      LAGFX_SPV_GLSL_SQRT,         1},
        {"air.fast.rsqrt",     LAGFX_SPV_GLSL_INVERSE_SQRT, 1},
        {"air.fast.exp",       LAGFX_SPV_GLSL_EXP,          1},
        {"air.fast.log",       LAGFX_SPV_GLSL_LOG,          1},
        {"air.fast.sin",       LAGFX_SPV_GLSL_SIN,          1},
        {"air.fast.cos",       LAGFX_SPV_GLSL_COS,          1},
        {"air.fast.normalize", LAGFX_SPV_GLSL_NORMALIZE,    1},
        {"air.fast.length",    LAGFX_SPV_GLSL_LENGTH,       1},
        {"air.fast.fabs",      LAGFX_SPV_GLSL_FABS,         1},
        {"air.fast.floor",     LAGFX_SPV_GLSL_FLOOR,        1},
        {"air.fast.ceil",      LAGFX_SPV_GLSL_CEIL,         1},
        {"air.fast.round",     LAGFX_SPV_GLSL_ROUND,        1},
        {"air.fast.fract",     LAGFX_SPV_GLSL_FRACT,        1},
        /* air.precise.* variants (same GLSL insts, num_args=1) */
        {"air.precise.sqrt",   LAGFX_SPV_GLSL_SQRT,         1},
        {"air.precise.rsqrt",  LAGFX_SPV_GLSL_INVERSE_SQRT, 1},
        {"air.precise.exp",    LAGFX_SPV_GLSL_EXP,          1},
        {"air.precise.log",    LAGFX_SPV_GLSL_LOG,          1},
        {"air.precise.sin",    LAGFX_SPV_GLSL_SIN,          1},
        {"air.precise.cos",    LAGFX_SPV_GLSL_COS,          1},
        {"air.precise.normalize", LAGFX_SPV_GLSL_NORMALIZE, 1},
        {"air.precise.length", LAGFX_SPV_GLSL_LENGTH,       1},
        /* Multi-operand intrinsics */
        {"air.fast.fmin",      LAGFX_SPV_GLSL_FMIN,         2},
        {"air.fast.fmax",      LAGFX_SPV_GLSL_FMAX,         2},
        {"air.fast.pow",       LAGFX_SPV_GLSL_POW,          2},
        {"air.fast.distance",  LAGFX_SPV_GLSL_DISTANCE,     2},
        {"air.fast.reflect",   LAGFX_SPV_GLSL_REFLECT,      2},
        {"air.fast.cross",     LAGFX_SPV_GLSL_CROSS,        2},
        {"air.fast.fclamp",    LAGFX_SPV_GLSL_FCLAMP,       3},
        {"air.fast.fmix",      LAGFX_SPV_GLSL_FMIX,         3},
        {"air.fast.step",      LAGFX_SPV_GLSL_STEP,         2},
        {"air.fast.smoothstep", LAGFX_SPV_GLSL_SMOOTH_STEP, 3},
        /* air.precise.* variants for multi-operand intrinsics */
        {"air.precise.fmin",      LAGFX_SPV_GLSL_FMIN,         2},
        {"air.precise.fmax",      LAGFX_SPV_GLSL_FMAX,         2},
        {"air.precise.pow",       LAGFX_SPV_GLSL_POW,          2},
        {"air.precise.distance",  LAGFX_SPV_GLSL_DISTANCE,     2},
        {"air.precise.reflect",   LAGFX_SPV_GLSL_REFLECT,      2},
        {"air.precise.cross",     LAGFX_SPV_GLSL_CROSS,        2},
        {"air.precise.fclamp",    LAGFX_SPV_GLSL_FCLAMP,       3},
        {"air.precise.fmix",      LAGFX_SPV_GLSL_FMIX,         3},
        {"air.precise.step",      LAGFX_SPV_GLSL_STEP,         2},
        {"air.precise.smoothstep", LAGFX_SPV_GLSL_SMOOTH_STEP, 3},
        /* Round 3: trig + misc (GLSL numbers verified vs GLSL.std.450.h). */
        {"air.fast.tan",        LAGFX_SPV_GLSL_TAN,     1},
        {"air.fast.asin",       LAGFX_SPV_GLSL_ASIN,    1},
        {"air.fast.acos",       LAGFX_SPV_GLSL_ACOS,    1},
        {"air.fast.atan",       LAGFX_SPV_GLSL_ATAN,    1},
        {"air.fast.sinh",       LAGFX_SPV_GLSL_SINH,    1},
        {"air.fast.cosh",       LAGFX_SPV_GLSL_COSH,    1},
        {"air.fast.tanh",       LAGFX_SPV_GLSL_TANH,    1},
        {"air.fast.exp2",       LAGFX_SPV_GLSL_EXP2,    1},
        {"air.fast.log2",       LAGFX_SPV_GLSL_LOG2,    1},
        {"air.fast.trunc",      LAGFX_SPV_GLSL_TRUNC,   1},
        {"air.fast.sign",       LAGFX_SPV_GLSL_FSIGN,   1},
        {"air.fast.radians",    LAGFX_SPV_GLSL_RADIANS, 1},
        {"air.fast.degrees",    LAGFX_SPV_GLSL_DEGREES, 1},
        {"air.fast.atan2",      LAGFX_SPV_GLSL_ATAN2,   2},
        {"air.fast_atan2",      LAGFX_SPV_GLSL_ATAN2,   2},   /* REAL Apple name (observed live: air.fast_atan2.f32) */
        {"air.fast.ldexp",      LAGFX_SPV_GLSL_LDEXP,   2},
        {"air.fast.refract",    LAGFX_SPV_GLSL_REFRACT, 3},
        {"air.precise.tan",     LAGFX_SPV_GLSL_TAN,     1},
        {"air.precise.asin",    LAGFX_SPV_GLSL_ASIN,    1},
        {"air.precise.acos",    LAGFX_SPV_GLSL_ACOS,    1},
        {"air.precise.atan",    LAGFX_SPV_GLSL_ATAN,    1},
        {"air.precise.sinh",    LAGFX_SPV_GLSL_SINH,    1},
        {"air.precise.cosh",    LAGFX_SPV_GLSL_COSH,    1},
        {"air.precise.tanh",    LAGFX_SPV_GLSL_TANH,    1},
        {"air.precise.exp2",    LAGFX_SPV_GLSL_EXP2,    1},
        {"air.precise.log2",    LAGFX_SPV_GLSL_LOG2,    1},
        {"air.precise.trunc",   LAGFX_SPV_GLSL_TRUNC,   1},
        {"air.precise.sign",    LAGFX_SPV_GLSL_FSIGN,   1},
        {"air.precise.radians", LAGFX_SPV_GLSL_RADIANS, 1},
        {"air.precise.degrees", LAGFX_SPV_GLSL_DEGREES, 1},
        {"air.precise.atan2",   LAGFX_SPV_GLSL_ATAN2,   2},
        {"air.precise.ldexp",   LAGFX_SPV_GLSL_LDEXP,   2},
        {"air.precise.refract", LAGFX_SPV_GLSL_REFRACT, 3},
        /* ===== REAL Apple naming (verified via llvm-dis of xcrun-metal
         * output, 2026-05-30) =====
         * The entries above use an `air.fast.<op>` / `air.precise.<op>`
         * (dot-separated) convention that does NOT match what Apple's
         * metal compiler actually emits. Real names are `air.<op>` and
         * `air.fast_<op>` / `air.precise_<op>` (underscore). Prefix-match
         * still works because the `.v3f32` / `.f32` type suffix follows.
         * These are the verified math ops from the coverage-audit corpus;
         * extend as more real shaders are run through the translator.
         * (air.dot and air.fast_saturate are NOT GLSL ext-insts — handled
         * as core-op specials below.) */
        {"air.mix",            LAGFX_SPV_GLSL_FMIX,         3},
        {"air.fast_clamp",     LAGFX_SPV_GLSL_FCLAMP,       3},
        {"air.precise_clamp",  LAGFX_SPV_GLSL_FCLAMP,       3},
        {"air.fast_pow",       LAGFX_SPV_GLSL_POW,          2},
        {"air.precise_pow",    LAGFX_SPV_GLSL_POW,          2},
        {"air.fast_rsqrt",     LAGFX_SPV_GLSL_INVERSE_SQRT, 1},
        {"air.precise_rsqrt",  LAGFX_SPV_GLSL_INVERSE_SQRT, 1},
        {"air.fast_sqrt",      LAGFX_SPV_GLSL_SQRT,         1},
        {"air.precise_sqrt",   LAGFX_SPV_GLSL_SQRT,         1},
        {"air.fast_fmin",      LAGFX_SPV_GLSL_FMIN,         2},
        {"air.fast_fmax",      LAGFX_SPV_GLSL_FMAX,         2},
        {"air.fast_floor",     LAGFX_SPV_GLSL_FLOOR,        1},
        {"air.fast_ceil",      LAGFX_SPV_GLSL_CEIL,         1},
        {"air.fast_fract",     LAGFX_SPV_GLSL_FRACT,        1},
        {"air.fast_normalize", LAGFX_SPV_GLSL_NORMALIZE,    1},
        {"air.fast_length",    LAGFX_SPV_GLSL_LENGTH,       1},
        {"air.fast_fabs",      LAGFX_SPV_GLSL_FABS,         1},
        {"air.fast_log2",      LAGFX_SPV_GLSL_LOG2,         1},
        {"air.fast_exp2",      LAGFX_SPV_GLSL_EXP2,         1},
        {"air.fast_log",       LAGFX_SPV_GLSL_LOG,          1},
        {"air.fast_exp",       LAGFX_SPV_GLSL_EXP,          1},
        /* fma(a,b,c) — REAL Apple name air.fma.<ty> (observed live in the
         * Xgc login fragment, f16 + f32 variants). */
        {"air.fma.",           LAGFX_SPV_GLSL_FMA,          3},
        {"air.fast_fma",       LAGFX_SPV_GLSL_FMA,          3},
        /* sign(x) — air.sign.<ty> (the GammaLUTs srgb helpers). */
        {"air.sign.",          LAGFX_SPV_GLSL_FSIGN,        1},
        {"air.fast_sign",      LAGFX_SPV_GLSL_FSIGN,        1},
    };

    uint32_t glsl_inst = 0u;
    for (size_t i = 0; i < sizeof(intrinsic_table) / sizeof(intrinsic_table[0]); i++) {
        if (strncmp(fn_name, intrinsic_table[i].prefix, strlen(intrinsic_table[i].prefix)) == 0) {
            glsl_inst = intrinsic_table[i].glsl_inst;
            break;
        }
    }

    /* === air.convert.* numeric conversions (core SPIR-V, NOT GLSL.std.450) ===
     *
     * Apple lowers Metal scalar/vector numeric casts to a CALL of an
     * `air.convert.<dstkind>.<dsttype>.<srckind>.<srctype>` intrinsic
     * rather than an LLVM `uitofp`/`sitofp`/CAST instruction. E.g.
     * `float2(uint x, uint y)` emits two
     * `air.convert.f.f32.u.i32(i32) -> float` calls. These map to core
     * SPIR-V conversion opcodes, not to OpExtInst GLSL.std.450 — so they
     * must be handled here, BEFORE the unrecognized-call OpUndef fallback
     * (which would otherwise drop the conversion and feed an undefined
     * scalar into the consuming insertelement → garbage gl_Position →
     * triangle not rasterised on strict drivers; the Stage-80 black-screen
     * bug, isolated 2026-05-29). The `kind` field is a single char:
     * 'f'=float, 'u'=unsigned int, 's'=signed int.
     *
     *   dst  src  → opcode
     *    f    u   → OpConvertUToF      f    s   → OpConvertSToF
     *    u    f   → OpConvertFToU      s    f   → OpConvertFToS
     *    f    f   → OpFConvert (f16<->f32)
     *    u/s  u/s → OpUConvert / OpSConvert (int width change)
     */
    if (strncmp(fn_name, "air.convert.", 12u) == 0) {
        const char *q = fn_name + 12u;          /* "<dstkind>.<dsttype>.<srckind>.<srctype>" */
        char dstkind = q[0];
        /* Advance past the dstkind + dsttype fields (two '.') to srckind. */
        const char *r = q;
        int dots = 0;
        while (*r && dots < 2) { if (*r == '.') dots++; r++; }
        char srckind = *r;
        /* srctype string follows srckind after one more '.', e.g.
         * "...u.i1" → src kind 'u', src type "i1". A bool (i1) source cannot
         * feed OpConvertUToF/SToF (SPIR-V bool isn't an int) — `step()`/
         * comparisons lower to `air.convert.f.f32.u.i1(i1)`. Detect it so we
         * can emit OpSelect(cond, 1.0, 0.0) instead. */
        const char *srctype = r;
        while (*srctype && *srctype != '.') srctype++;
        if (*srctype == '.') srctype++;
        int src_is_bool = (srctype[0] == 'i' && srctype[1] == '1' &&
                           (srctype[2] == '\0'));
        int src_is_bool_vec = 0;
        if (!src_is_bool && srctype[0] == 'v') {
            /* vector-of-bool: "v<N>i1" (air.convert.f.v3f32.u.v3i1, Xgc) */
            const char *q = srctype + 1;
            while (*q >= '0' && *q <= '9') q++;
            src_is_bool_vec = (q != srctype + 1 && q[0] == 'i' &&
                               q[1] == '1' && q[2] == '\0');
            src_is_bool |= src_is_bool_vec;
        }

        /* dsttype substring: after "<dstkind>." up to the next '.'. */
        const char *dsttype = (q[0] && q[1] == '.') ? q + 2 : q;
        int dst_is_bool = (dsttype[0] == 'i' && dsttype[1] == '1' &&
                           (dsttype[2] == '.' || dsttype[2] == '\0'));
        if (!dst_is_bool && dsttype[0] == 'v') {
            const char *dq = dsttype + 1;
            while (*dq >= '0' && *dq <= '9') dq++;
            dst_is_bool = (dq != dsttype + 1 && dq[0] == 'i' && dq[1] == '1' &&
                           (dq[2] == '.' || dq[2] == '\0'));
        }

        uint32_t conv_op = 0u;
        if      (dstkind == 'f' && srckind == 'u') conv_op = LAGFX_SPV_OP_CONVERT_U_TO_F;
        else if (dstkind == 'f' && srckind == 's') conv_op = LAGFX_SPV_OP_CONVERT_S_TO_F;
        else if (dstkind == 'u' && srckind == 'f') conv_op = LAGFX_SPV_OP_CONVERT_F_TO_U;
        else if (dstkind == 's' && srckind == 'f') conv_op = LAGFX_SPV_OP_CONVERT_F_TO_S;
        else if (dstkind == 'f' && srckind == 'f') conv_op = LAGFX_SPV_OP_F_CONVERT;
        else if (dstkind == 'u')                   conv_op = LAGFX_SPV_OP_UCONVERT;
        else if (dstkind == 's')                   conv_op = LAGFX_SPV_OP_SCONVERT;

        uint32_t cvt_arg_slot = callee_slot_idx + 1u;
        /* bool(i1) → float: SPIR-V bool is not an integer, so OpConvertUToF/
         * SToF is invalid (spirv-val "Expected input to be int scalar or
         * vector"). Lower to OpSelect(cond, 1.0, 0.0) of the destination
         * float type. `step(edge,x)` and other comparison-to-float casts
         * take this path. */
        if (src_is_bool && dstkind == 'f' && inst->num_ops > cvt_arg_slot &&
            result_value_id != 0u) {
            uint32_t dst_ty_spv = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                    ? emit_air_type(c, result_ty_air)
                                    : emit_type_float32(c);
            uint32_t n_types_c = 0;
            const lagfx_air_type_t *ts_c =
                lagfx_air_module_types(c->m, &n_types_c);
            uint32_t src_lanes = 1u;
            if (src_is_bool_vec && result_ty_air < n_types_c &&
                ts_c[result_ty_air].kind == LAGFX_AIR_TYPE_VECTOR &&
                ts_c[result_ty_air].num_op >= 1u)
                src_lanes = (uint32_t)ts_c[result_ty_air].op[0];
            uint32_t cond_fallback = src_is_bool_vec
                ? emit_type_vec(c, emit_type_bool(c), src_lanes)
                : emit_type_bool(c);
            uint32_t cond = resolve_or_undef(c,
                resolve_relative((uint32_t)inst->ops[cvt_arg_slot], next_val_id),
                cond_fallback);
            uint32_t one = emit_const_float32(c, 1.0f);
            uint32_t zero = emit_const_float32(c, 0.0f);
            /* If the destination is a float vector, the OpSelect needs the
             * 1.0/0.0 (and the condition) broadcast to that width. */
            uint32_t n_types = 0;
            const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
            if (result_ty_air < n_types &&
                ts[result_ty_air].kind == LAGFX_AIR_TYPE_VECTOR &&
                ts[result_ty_air].num_op >= 1u) {
                uint32_t lanes = ts[result_ty_air].op[0];
                if (lanes >= 2u && lanes <= 4u) {
                    uint32_t one_v[6], zero_v[6];
                    uint32_t one_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t zero_id = lagfx_spv_builder_alloc_id(c->b);
                    one_v[0] = dst_ty_spv; one_v[1] = one_id;
                    zero_v[0] = dst_ty_spv; zero_v[1] = zero_id;
                    for (uint32_t k = 0; k < lanes; k++) { one_v[2+k]=one; zero_v[2+k]=zero; }
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, one_v, 2u+lanes);
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, zero_v, 2u+lanes);
                    one = one_id; zero = zero_id;
                    /* OpSelect over a vector result needs a vector
                     * condition; a bool-VECTOR source already is one —
                     * splatting it would build vec-of-vec. */
                    if (!src_is_bool_vec) {
                        uint32_t bvec = emit_type_vec(c, emit_type_bool(c), lanes);
                        uint32_t cv[6];
                        uint32_t cv_id = lagfx_spv_builder_alloc_id(c->b);
                        cv[0] = bvec; cv[1] = cv_id;
                        for (uint32_t k = 0; k < lanes; k++) cv[2+k] = cond;
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, cv, 2u+lanes);
                        cond = cv_id;
                    }
                }
            }
            uint32_t res = resolve_value_spv(c, result_value_id);
            if (!res) res = lagfx_spv_builder_alloc_id(c->b);
            uint32_t sops[] = { dst_ty_spv, res, cond, one, zero };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECT, sops, 5);
            bind_value_spv(c, result_value_id, res);
            set_result_air_type(c, result_value_id, result_ty_air);
            LAGFX_TRACE("call: air.convert '%s' bool→float via OpSelect "
                        "(value-id %u)", fn_name, result_value_id);
            return;
        }
        if (conv_op != 0u && inst->num_ops > cvt_arg_slot &&
            result_value_id != 0u) {
            uint32_t dst_ty_spv = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                    ? emit_air_type(c, result_ty_air)
                                    : emit_type_float32(c);
            uint32_t a_rel  = (uint32_t)inst->ops[cvt_arg_slot];
            uint32_t a_id   = resolve_relative(a_rel, next_val_id);
            /* float -> i1 dest: SPIR-V bool is not an int; lower to
             * OpFOrdNotEqual(x, 0.0) (mirrors the CAST-instruction path). */
            if (dst_is_bool && srckind == 'f' &&
                result_ty_air != LAGFX_AIR_TYPE_NONE) {
                uint32_t x_spv = resolve_or_undef(c, a_id,
                                                  emit_type_float32(c));
                if (emit_float_to_bool_cmp(c, x_spv, result_ty_air,
                                           result_value_id))
                    return;
            }
            uint32_t a_spv  = resolve_or_undef(c, a_id, emit_type_int_w(c, 32u, 0u));
            uint32_t res    = resolve_value_spv(c, result_value_id);
            if (!res) res = lagfx_spv_builder_alloc_id(c->b);
            uint32_t cops[] = { dst_ty_spv, res, a_spv };
            lagfx_spv_builder_emit_op(c->b, conv_op, cops, 3);
            bind_value_spv(c, result_value_id, res);
            set_result_air_type(c, result_value_id, result_ty_air);
            LAGFX_TRACE("call: air.convert '%s' dst=%c src=%c → conv_op=%u "
                        "(value-id %u)", fn_name, dstkind, srckind, conv_op,
                        result_value_id);
            return;
        }
        /* Unhandled convert shape — fall through to the typed-undef path
         * so downstream value-numbering still stays in sync. */
    }

    /* === air.dot.<vty> → OpDot (core SPIR-V, NOT GLSL.std.450) ===
     * Metal `dot(a,b)` lowers to `air.dot.v3f32(<3xfloat>, <3xfloat>)
     * -> float`. OpDot takes two vectors of the SAME float type and
     * yields a SCALAR float (the call's return type). Also reached by
     * normalize(), which expands to a * rsqrt(dot(a,a)). */
    if (strncmp(fn_name, "air.dot.", 8u) == 0 && result_value_id != 0u) {
        uint32_t a0_slot = callee_slot_idx + 1u;
        uint32_t a1_slot = callee_slot_idx + 2u;
        if (inst->num_ops > a1_slot) {
            uint32_t dot_ty = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                ? emit_air_type(c, result_ty_air)
                                : emit_type_float32(c);
            uint32_t a0 = resolve_or_undef(c,
                            resolve_relative((uint32_t)inst->ops[a0_slot], next_val_id),
                            emit_type_vec4_f(c));
            uint32_t a1 = resolve_or_undef(c,
                            resolve_relative((uint32_t)inst->ops[a1_slot], next_val_id),
                            emit_type_vec4_f(c));
            uint32_t res = resolve_value_spv(c, result_value_id);
            if (!res) res = lagfx_spv_builder_alloc_id(c->b);
            uint32_t dops[] = { dot_ty, res, a0, a1 };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DOT, dops, 4);
            bind_value_spv(c, result_value_id, res);
            set_result_air_type(c, result_value_id, result_ty_air);
            LAGFX_TRACE("call: air.dot '%s' → OpDot (value-id %u)",
                        fn_name, result_value_id);
            return;
        }
    }

    /* === air.fast_saturate.<ty> → OpExtInst FClamp(x, 0, 1) ===
     * saturate(x) clamps to [0,1]. Not a GLSL ext-inst by itself; lower to
     * FClamp with synthesized 0/1. For a vector result, splat the scalar
     * 0/1 constants to the result width via OpCompositeConstruct (a body
     * instruction; the scalar constants are pre-warmed in the prologue). */
    if ((strncmp(fn_name, "air.fast_saturate", 17u) == 0 ||
         strncmp(fn_name, "air.precise_saturate", 20u) == 0 ||
         strncmp(fn_name, "air.saturate", 12u) == 0) &&
        result_value_id != 0u && result_ty_air != LAGFX_AIR_TYPE_NONE) {
        uint32_t arg_slot = callee_slot_idx + 1u;
        if (inst->num_ops > arg_slot) {
            uint32_t rty = emit_air_type(c, result_ty_air);
            uint32_t x = resolve_or_undef(c,
                resolve_relative((uint32_t)inst->ops[arg_slot], next_val_id), rty);
            /* 0/1 bounds in the result's SCALAR type: air.saturate.f16
             * (half) needs half constants — f32 bounds on a half x is an
             * OpExtInst operand-type mismatch. */
            uint32_t n_types = 0;
            const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
            bool is_half = false;
            {
                uint32_t elem = result_ty_air;
                if (elem < n_types && ts[elem].kind == LAGFX_AIR_TYPE_VECTOR &&
                    ts[elem].num_op >= 2u)
                    elem = ts[elem].op[1];
                is_half = (elem < n_types && ts[elem].kind == LAGFX_AIR_TYPE_HALF);
            }
            uint32_t c0, c1;
            if (is_half) {
                uint32_t half_t = emit_type_half(c);
                c0 = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { half_t, c0, 0x0000u };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, o, 3); }
                c1 = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { half_t, c1, 0x3C00u /* 1.0h */ };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, o, 3); }
            } else {
                c0 = emit_const_float32(c, 0.0f);
                c1 = emit_const_float32(c, 1.0f);
            }
            /* If the result is a float vector, splat 0/1 to its width. */
            uint32_t lo = c0, hi = c1;
            if (result_ty_air < n_types &&
                ts[result_ty_air].kind == LAGFX_AIR_TYPE_VECTOR &&
                ts[result_ty_air].num_op >= 1u) {
                uint32_t lanes = ts[result_ty_air].op[0];
                if (lanes >= 2u && lanes <= 4u) {
                    uint32_t lo_ops[6], hi_ops[6];
                    lo_ops[0] = rty; hi_ops[0] = rty;
                    uint32_t lo_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t hi_id = lagfx_spv_builder_alloc_id(c->b);
                    lo_ops[1] = lo_id; hi_ops[1] = hi_id;
                    for (uint32_t k = 0; k < lanes; k++) { lo_ops[2+k]=c0; hi_ops[2+k]=c1; }
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, lo_ops, 2u+lanes);
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, hi_ops, 2u+lanes);
                    lo = lo_id; hi = hi_id;
                }
            }
            uint32_t res = resolve_value_spv(c, result_value_id);
            if (!res) res = lagfx_spv_builder_alloc_id(c->b);
            uint32_t o[] = { rty, res, c->id_glsl, LAGFX_SPV_GLSL_FCLAMP, x, lo, hi };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_EXT_INST, o, 7);
            bind_value_spv(c, result_value_id, res);
            set_result_air_type(c, result_value_id, result_ty_air);
            LAGFX_TRACE("call: air.fast_saturate → FClamp(x,0,1) (value-id %u)",
                        result_value_id);
            return;
        }
    }

    /* === air.sample_texture_2d.<vty> → OpImageSampleImplicitLod ===
     * `tex.sample(s, uv)` lowers to
     *   air.sample_texture_2d.v4f32(tex_ptr, samp_ptr, uv, <opts...>)
     * returning { vec4 colour, i8 status }. We OpLoad the image + sampler
     * (bound to UniformConstant vars via the resource-arg setup), combine
     * with OpSampledImage, sample at `uv`, then OpCompositeConstruct the
     * { vec4, i8 } result struct (status byte = undef; only field 0, the
     * colour, is ever extracted). Needs tex + samp + uv operands. */
    /* air.sample_texture_3d shares this path: our textures all realize as
     * 2D images (the tex arg is already declared with the 2D image type),
     * and a Dim2D OpImageSampleImplicitLod accepts a coordinate with
     * extra components — so the 3D LUT samples read a 2D slice instead of
     * binding undef. Approximate, but real data of the right type. */
    if ((strncmp(fn_name, "air.sample_texture_2d", 21u) == 0 ||
         strncmp(fn_name, "air.sample_texture_3d", 21u) == 0) &&
        result_value_id != 0u) {
        uint32_t tex_slot = callee_slot_idx + 1u;
        uint32_t samp_slot = callee_slot_idx + 2u;
        uint32_t uv_slot   = callee_slot_idx + 3u;
        if (inst->num_ops > uv_slot) {
            uint32_t tex_var = resolve_value_spv(c,
                resolve_relative((uint32_t)inst->ops[tex_slot], next_val_id));
            uint32_t samp_var = resolve_value_spv(c,
                resolve_relative((uint32_t)inst->ops[samp_slot], next_val_id));
            /* Both operands must be REAL bound resource variables. A
             * constexpr sampler (or a texture/sampler value that flowed
             * through not-yet-handled control flow — Xgc's undef-pointer
             * sampler) resolves to something else; OpLoad through it is a
             * spirv-val reject ("not a logical pointer"). Fall back to the
             * default sampler, then to the FIRST bound var of the right
             * kind (semantically approximate; valid — matching the
             * pending-control-flow degradation contract). */
            {
                int samp_ok = 0, tex_ok = 0;
                uint32_t first_samp = 0, first_tex = 0;
                for (uint32_t a = 0;
                     a < c->num_args && a < LAGFX_MAX_VERTEX_ARGS; a++) {
                    uint32_t v = c->arg_resource_var[a];
                    if (!v) continue;
                    if (c->arg_resource_kind[a] == 2u) {
                        if (!first_samp) first_samp = v;
                        if (samp_var == v) samp_ok = 1;
                    } else if (c->arg_resource_kind[a] == 1u) {
                        if (!first_tex) first_tex = v;
                        if (tex_var == v) tex_ok = 1;
                    }
                }
                if (!samp_ok)
                    samp_var = c->id_default_sampler_var ? c->id_default_sampler_var
                                                         : first_samp;
                if (!tex_ok)
                    tex_var = first_tex;
            }
            uint32_t uv_spv = resolve_or_undef(c,
                resolve_relative((uint32_t)inst->ops[uv_slot], next_val_id),
                emit_type_vec2_f(c));
            if (tex_var && samp_var) {
                uint32_t img_t  = emit_type_image2d_f(c);
                uint32_t samp_t = emit_type_sampler(c);
                uint32_t simg_t = emit_type_sampled_image(c);
                uint32_t v4f    = emit_type_vec4_f(c);

                uint32_t img_val = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { img_t, img_val, tex_var };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, o, 3); }
                uint32_t samp_val = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { samp_t, samp_val, samp_var };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, o, 3); }
                uint32_t sampled = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { simg_t, sampled, img_val, samp_val };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SAMPLED_IMAGE, o, 4); }
                uint32_t rgba = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { v4f, rgba, sampled, uv_spv };
                  lagfx_spv_builder_emit_op(c->b,
                      LAGFX_SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, o, 4); }

                /* Build the { vec4, i8 } result struct. The struct type +
                 * Int8 are already pre-emitted (the call-return pre-emit
                 * pass). Status byte is undef — never read. */
                uint32_t struct_spv = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                        ? emit_air_type(c, result_ty_air)
                                        : 0u;
                uint32_t res = resolve_value_spv(c, result_value_id);
                if (!res) res = lagfx_spv_builder_alloc_id(c->b);
                if (struct_spv) {
                    uint32_t status = emit_undef(c, emit_type_int_w(c, 8u, 0u));
                    uint32_t o[] = { struct_spv, res, rgba, status };
                    lagfx_spv_builder_emit_op(c->b,
                        LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, o, 4);
                    bind_value_spv(c, result_value_id, res);
                    set_result_air_type(c, result_value_id, result_ty_air);
                } else {
                    /* No struct type: bind the bare vec4 (extractval will
                     * still pass it through if it's field 0). */
                    bind_value_spv(c, result_value_id, rgba);
                }
                LAGFX_TRACE("call: air.sample_texture_2d → OpImageSampleImplicitLod "
                            "(value-id %u)", result_value_id);
                return;
            }
        }
    }

    /* === air.read_texture_2d.v4f32 → OpImageFetch (int coords, Lod 0) ===
     * `tex.read(uint2 xy, lod)` — (tex_ptr, v2i32 coord, i32 lod, i32) →
     * { vec4, i8 }. Fetch needs no sampler; Lod fixed to 0 (mip chains
     * aren't realized host-side). */
    if (strncmp(fn_name, "air.read_texture_2d", 19u) == 0 &&
        result_value_id != 0u) {
        uint32_t tex_slot   = callee_slot_idx + 1u;
        uint32_t coord_slot = callee_slot_idx + 2u;
        if (inst->num_ops > coord_slot) {
            uint32_t tex_var = resolve_value_spv(c,
                resolve_relative((uint32_t)inst->ops[tex_slot], next_val_id));
            /* Same resource-var validity contract as the sample handler. */
            {
                int tex_ok = 0; uint32_t first_tex = 0;
                for (uint32_t a = 0;
                     a < c->num_args && a < LAGFX_MAX_VERTEX_ARGS; a++) {
                    if (c->arg_resource_kind[a] != 1u ||
                        !c->arg_resource_var[a]) continue;
                    if (!first_tex) first_tex = c->arg_resource_var[a];
                    if (tex_var == c->arg_resource_var[a]) tex_ok = 1;
                }
                if (!tex_ok) tex_var = first_tex;
            }
            if (tex_var) {
                uint32_t ivec2 = emit_type_vec(c, emit_type_int_w(c, 32u, 0u), 2u);
                uint32_t coord = resolve_or_undef(c,
                    resolve_relative((uint32_t)inst->ops[coord_slot], next_val_id),
                    ivec2);
                uint32_t img_t = emit_type_image2d_f(c);
                uint32_t v4f   = emit_type_vec4_f(c);
                uint32_t img_val = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { img_t, img_val, tex_var };
                  lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, o, 3); }
                uint32_t rgba = lagfx_spv_builder_alloc_id(c->b);
                { uint32_t o[] = { v4f, rgba, img_val, coord,
                                   0x2u /* ImageOperands Lod */,
                                   emit_const_uint32(c, 0u) };
                  lagfx_spv_builder_emit_op(c->b, 95 /* OpImageFetch */, o, 6); }
                uint32_t struct_spv = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                        ? emit_air_type(c, result_ty_air)
                                        : 0u;
                uint32_t res = resolve_value_spv(c, result_value_id);
                if (!res) res = lagfx_spv_builder_alloc_id(c->b);
                if (struct_spv) {
                    uint32_t status = emit_undef(c, emit_type_int_w(c, 8u, 0u));
                    uint32_t o[] = { struct_spv, res, rgba, status };
                    lagfx_spv_builder_emit_op(c->b,
                        LAGFX_SPV_OP_COMPOSITE_CONSTRUCT, o, 4);
                    bind_value_spv(c, result_value_id, res);
                    set_result_air_type(c, result_value_id, result_ty_air);
                } else {
                    bind_value_spv(c, result_value_id, rgba);
                }
                LAGFX_TRACE("call: air.read_texture_2d → OpImageFetch "
                            "(value-id %u)", result_value_id);
                return;
            }
        }
    }

    /* === air.any.vNi1 / air.all.vNi1 → OpAny / OpAll ===
     * Vector-bool reductions; unhandled they bound an UNDEF i1 that fed
     * EIGHT of Xgc's dispatched branch conditions. One arg (the vNbool). */
    if ((strncmp(fn_name, "air.any.", 8u) == 0 ||
         strncmp(fn_name, "air.all.", 8u) == 0) && result_value_id != 0u) {
        uint32_t a_slot = callee_slot_idx + 1u;
        if (inst->num_ops > a_slot) {
            uint32_t a_id = resolve_relative((uint32_t)inst->ops[a_slot],
                                             next_val_id);
            uint32_t b = emit_type_bool(c);
            /* Lane count from the name suffix "vNi1". */
            uint32_t lanes = 4u;
            const char *vv = strrchr(fn_name, '.');
            if (vv && vv[1] == 'v' && vv[2] >= '2' && vv[2] <= '4')
                lanes = (uint32_t)(vv[2] - '0');
            uint32_t bvec = emit_type_vec(c, b, lanes);
            uint32_t a_spv = resolve_or_undef(c, a_id, bvec);
            uint32_t rid = lagfx_spv_builder_alloc_id(c->b);
            uint32_t o[] = { b, rid, a_spv };
            lagfx_spv_builder_emit_op(c->b,
                fn_name[4] == 'n' /* aNy */ ? LAGFX_SPV_OP_ANY
                                            : LAGFX_SPV_OP_ALL, o, 3);
            bind_value_spv(c, result_value_id, rid);
            set_result_air_type(c, result_value_id, LAGFX_AIR_TYPE_BOOL);
            set_value_spv_type(c, result_value_id, b);
            return;
        }
    }

    /* === air.dfdx / air.dfdy / air.fwidth → OpDPdx/OpDPdy/OpFwidth === */
    if ((strncmp(fn_name, "air.dfdx.", 9u) == 0 ||
         strncmp(fn_name, "air.dfdy.", 9u) == 0 ||
         strncmp(fn_name, "air.fwidth.", 11u) == 0) && result_value_id != 0u &&
        c->stage == LAGFX_XLATE_STAGE_FRAGMENT) {
        uint32_t a_slot = callee_slot_idx + 1u;
        if (inst->num_ops > a_slot &&
            result_ty_air != LAGFX_AIR_TYPE_NONE) {
            uint32_t rt = emit_air_type(c, result_ty_air);
            uint32_t a_id = resolve_relative((uint32_t)inst->ops[a_slot],
                                             next_val_id);
            uint32_t a_spv = resolve_or_undef(c, a_id, rt);
            uint32_t rid = lagfx_spv_builder_alloc_id(c->b);
            uint32_t o[] = { rt, rid, a_spv };
            lagfx_spv_builder_emit_op(c->b,
                fn_name[4] == 'f' ? LAGFX_SPV_OP_FWIDTH
                : fn_name[7] == 'x' ? LAGFX_SPV_OP_DPDX : LAGFX_SPV_OP_DPDY,
                o, 3);
            bind_value_spv(c, result_value_id, rid);
            set_result_air_type(c, result_value_id, result_ty_air);
            set_value_spv_type(c, result_value_id, rt);
            return;
        }
    }

    /* === Registered UberShader helper → OpFunctionCall ===============
     * The callee is a real (non-proto) function in the SAME metallib that
     * was translated as a SPIR-V function (callee-first). Wire the call;
     * on any lowering obstacle fall through to the typed-undef path. */
    if (!glsl_inst && c->helpers &&
        strncmp(fn_name, "air.", 4u) != 0 && strncmp(fn_name, "llvm.", 5u) != 0 &&
        emit_helper_fncall(c, fn_idx, inst, callee_slot_idx,
                           result_value_id, next_val_id, fn_name)) {
        return;
    }

    /* Not an air.* GLSL intrinsic we recognize. Two sub-cases:
     *   - Void calls (llvm.lifetime/debug, and anything inst_produces_value
     *     classified non-producing → result_value_id == 0): drop silently;
     *     they consume no value-id, so downstream numbering is unaffected.
     *   - Non-void calls we don't lower yet (e.g. a texture sample → vec4):
     *     these DID consume a value-id (result_value_id != 0), so we MUST
     *     bind something of the right type or every later relative ref
     *     desyncs (blocker A). Emit a typed OpUndef placeholder. The result
     *     isn't visually correct (no real sample yet) but it is
     *     structurally + type correct, which is what downstream BINOP/etc.
     *     type resolution needs. Real image-op lowering is separate. */
    if (!glsl_inst) {
        if (result_value_id != 0u && !air_type_is_void(c, result_ty_air)) {
            uint32_t ph_ty = (result_ty_air != LAGFX_AIR_TYPE_NONE)
                                ? emit_air_type(c, result_ty_air)
                                : emit_type_vec4_f(c);
            uint32_t ph = emit_undef(c, ph_ty);
            bind_value_spv(c, result_value_id, ph);
            set_result_air_type(c, result_value_id, result_ty_air);
            LAGFX_TRACE("call: unrecognized non-void '%s' — typed undef "
                        "placeholder (value-id %u)", fn_name, result_value_id);
        } else {
            LAGFX_TRACE("call: unrecognized void intrinsic '%s' — drop", fn_name);
        }
        return;
    }

    /* Find the table entry to get num_args. */
    uint32_t entry_num_args = 0u;
    for (size_t i = 0; i < sizeof(intrinsic_table) / sizeof(intrinsic_table[0]); i++) {
        if (strncmp(fn_name, intrinsic_table[i].prefix, strlen(intrinsic_table[i].prefix)) == 0) {
            entry_num_args = intrinsic_table[i].num_args;
            break;
        }
    }

    /* The CALL operand layout after callee_rel is [args...], so we need
     * at least one argument. */
    uint32_t arg_slot_idx = callee_slot_idx + 1u;
    if (inst->num_ops <= arg_slot_idx || entry_num_args == 0u) {
        LAGFX_TRACE("call: no args for '%s' — drop", fn_name);
        return;
    }

    /* result_ty_air was resolved up front. Emit its SPIR-V type (or vec4f
     * if the return type was unresolvable). */
    uint32_t result_spv_type;
    if (result_ty_air != LAGFX_AIR_TYPE_NONE) {
        result_spv_type = emit_air_type(c, result_ty_air);
    } else {
        /* Default to vec4f as per task spec. */
        result_spv_type = emit_type_vec4_f(c);
    }

    /* Result is bound by value-NUMBER (result_value_id from translate_body,
     * = LLVM NextValueNo), not inst_id_base+inst_idx. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpExtInst: [result_type, result_id, ext_set(id_glsl), inst_num, arg1, [arg2, [arg3]]] */
    uint32_t ops[8];  /* up to 3 args + 4 fixed = 7 words; 8 for safety */
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = c->id_glsl;
    ops[3] = glsl_inst;
    uint32_t n_ops = 4u;
    for (uint8_t a = 0; a < entry_num_args; a++) {
        if (arg_slot_idx + a >= inst->num_ops) break;
        uint32_t arg_rel = (uint32_t)inst->ops[arg_slot_idx + a];
        uint32_t arg_id  = resolve_relative(arg_rel, next_val_id);
        uint32_t arg_spv = resolve_or_undef(c, arg_id, result_spv_type);
        ops[n_ops++] = arg_spv;
    }
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_EXT_INST, ops, n_ops);

    /* Bind result and record AIR type for downstream ops (value-number keyed). */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, result_ty_air);
}

/* Storage class for a GEP/AccessChain whose base is `ptr_value_id`. If the
 * base is a [[buffer(n)]] arg (a StorageBuffer Block variable), the result
 * pointer must also be StorageBuffer (SPIR-V requires AccessChain result
 * storage == base storage). Otherwise Function (alloca-backed locals). */
static uint32_t gep_base_storage_class(const xlate_ctx_t *c, uint32_t ptr_value_id) {
    /* Tracked class first — sees through GEP aliases and pointer bitcasts. */
    uint32_t tracked = get_value_storage(c, ptr_value_id);
    if (tracked != UINT32_MAX)
        return tracked;
    if (ptr_value_id >= c->arg_id_base &&
        ptr_value_id < c->arg_id_base + c->num_args) {
        uint32_t a = ptr_value_id - c->arg_id_base;
        if (a < LAGFX_MAX_VERTEX_ARGS && c->arg_resource_kind[a] == 3u)
            return LAGFX_SPV_STORAGE_STORAGE_BUFFER;
    }
    return LAGFX_SPV_STORAGE_FUNCTION;
}

/* If `ptr_value_id` is a [[buffer(n)]] arg whose pointee is a non-struct
 * `device T*` (modelled as a `{ runtimearray<T> }` Block), return the
 * pointee (element) AIR type. 0 otherwise. Such bases need an extra
 * leading member-0 index in their OpAccessChain, and the LLVM GEP's first
 * index is a REAL runtime-array element index (not a to-be-dropped 0). */
static uint32_t buffer_arg_runtimearray_elem(xlate_ctx_t *c, uint32_t ptr_value_id) {
    /* A byte-buffer VIEW binding with a nonstruct pointee is a
     * { runtimearray<T> } Block too (see bytebuf_view doc). */
    if (c->value_view && ptr_value_id < c->value_id_capacity &&
        c->value_view[ptr_value_id]) {
        uint32_t v = (uint32_t)c->value_view[ptr_value_id] - 1u;
        if (v < c->n_bytebuf_views)
            return buffer_arg_nonstruct_pointee(
                c, c->bytebuf_view[v].pointee_ty);
        return 0u;
    }
    if (ptr_value_id < c->arg_id_base ||
        ptr_value_id >= c->arg_id_base + c->num_args)
        return 0u;
    uint32_t a = ptr_value_id - c->arg_id_base;
    if (a >= LAGFX_MAX_VERTEX_ARGS || c->arg_resource_kind[a] != 3u) return 0u;
    uint32_t pointee = buffer_arg_struct_ty(c, a);
    return buffer_arg_nonstruct_pointee(c, pointee);
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
        /* Base pointer unknown — leave the result UNBOUND (consumers fall
         * back to a typed undef VALUE). Binding an OpUndef POINTER here
         * made downstream loads emit `OpLoad %T %undef_ptr` — "Pointer is
         * not a logical pointer", spirv-val reject (the Xgc login panel
         * fragment, whose base flowed through then-unhandled control
         * flow). Keep the result AIR type so GEP+STORE can still type its
         * undef value. */
        set_result_air_type(c, result_value_id, source_ty);
        return;
    }

    /* A non-struct `device T*` buffer base is modelled as a
     * `{ runtimearray<T> }` Block: the AccessChain needs a leading member-0
     * index, and the LLVM GEP's FIRST index is the real runtime-array
     * element index (so it is kept, not dropped). For a struct buffer / a
     * local alloca the first index is pointer-arithmetic and is skipped. */
    uint32_t rtarr_elem  = buffer_arg_runtimearray_elem(c, ptr_id);
    uint32_t first_idx   = rtarr_elem ? 0u : 1u; /* first LLVM index used? */

    /* Helper DATABUF param base (`constant T* p` → `p[i]`): the base is
     * an INTERIOR StorageBuffer pointer, so LLVM's first index is real
     * pointer arithmetic — lower to OpPtrAccessChain (its Element operand
     * IS that first index; the param's pointer type carries the required
     * ArrayStride). Subsequent indices descend like OpAccessChain. */
    bool ptr_arith = false;
    if (c->is_helper && c->helper_self &&
        ptr_id >= c->arg_id_base && ptr_id < c->arg_id_base + c->num_args) {
        uint32_t a = ptr_id - c->arg_id_base;
        if (a < c->helper_self->n_params &&
            c->helper_self->param[a].kind == LAGFX_HPARAM_DATABUF) {
            ptr_arith = true;
            first_idx = 0u;
        }
    }

    /* Walk the AIR type chain to deduce the result pointer's pointee
     * type. For a runtime-array buffer base, start at the element type and
     * walk through ALL indices; otherwise start at source_ty and skip the
     * first (base-array) index. */
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t pointee = source_ty;
    /* Runtime-array buffer base: the FIRST kept LLVM index selects the
     * runtime-array ELEMENT — the pointee stays source_ty for it; only
     * SUBSEQUENT indices descend into the element type. Descending on the
     * element index walked one level too deep (a `GEP v4float, ptr, i64 n`
     * result pointee became float → OpAccessChain result-type mismatch,
     * the VfxU11 vertex shader / 5 login pipelines). */
    for (uint32_t i = (rtarr_elem || ptr_arith) ? first_idx + 1u : first_idx;
         i < n_idx_total; i++) {
        if (pointee >= n_types) break;
        const lagfx_air_type_t *t = &ts[pointee];
        switch (t->kind) {
            case LAGFX_AIR_TYPE_ARRAY:  if (t->num_op >= 2u) pointee = t->op[1]; break;
            case LAGFX_AIR_TYPE_VECTOR: if (t->num_op >= 2u) pointee = t->op[1]; break;
            case LAGFX_AIR_TYPE_STRUCT_ANON:
            case LAGFX_AIR_TYPE_STRUCT_NAMED: {
                /* Resolve the field index operand to a literal constant. */
                uint32_t idx_rel = (uint32_t)inst->ops[3u + i];
                uint32_t idx_id  = resolve_relative(idx_rel, next_val_id);
                int32_t field    = 0;
                if (!resolve_value_lit_i32(c, idx_id, &field) || field < 0) {
                    LAGFX_WARN("gep: struct field index not a known constant — drop");
                    return;
                }
                /* Bounds-check: field type lives at op[1 + field]. */
                if ((uint32_t)(1 + field) >= t->num_op) {
                    LAGFX_WARN("gep: struct field %d out of bounds — drop", field);
                    return;
                }
                pointee = t->op[1u + (uint32_t)field];
                break;
            }
            default: break;
        }
    }

    uint32_t storage = gep_base_storage_class(c, ptr_id);
    uint32_t pointee_spv = emit_air_type(c, pointee);
    uint32_t result_ptr  = emit_type_pointer(c, pointee, pointee_spv, storage);

    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);

    /* Build the operand vector: [result_type, result, base, idx..., ...] */
    uint32_t ops[16];
    ops[0] = result_ptr;
    ops[1] = result_id;
    ops[2] = ptr_spv;
    uint32_t op_count = 3u;
    /* Runtime-array buffer: index into the Block's member 0 first. */
    if (rtarr_elem && op_count < 16u)
        ops[op_count++] = emit_const_uint32(c, 0u);
    for (uint32_t i = first_idx; i < n_idx_total && op_count < 16u; i++) {
        uint32_t idx_rel = (uint32_t)inst->ops[3u + i];
        uint32_t idx_id  = resolve_relative(idx_rel, next_val_id);
        uint32_t idx_spv = resolve_value_spv(c, idx_id);
        if (!idx_spv) {
            /* Unresolvable index — substitute uint 0. */
            idx_spv = emit_const_uint32(c, 0u);
        }
        ops[op_count++] = idx_spv;
    }

    /* If the GEP produced no index operands, SPIR-V OpAccessChain isn't
     * needed — just alias the base. */
    if (op_count == 3u) {
        bind_value_spv(c, result_value_id, ptr_spv);
        set_result_air_type(c, result_value_id, pointee);
        set_value_storage(c, result_value_id, storage);
        set_value_spv_type(c, result_value_id, get_value_spv_type(c, ptr_id));
        return;
    }

    lagfx_spv_builder_emit_op(c->b,
        ptr_arith ? LAGFX_SPV_OP_PTR_ACCESS_CHAIN : LAGFX_SPV_OP_ACCESS_CHAIN,
        ops, op_count);
    bind_value_spv(c, result_value_id, result_id);
    set_result_air_type(c, result_value_id, pointee);
    set_value_storage(c, result_value_id, storage);
    /* Track the result's POINTER type — helper-call DATABUF args match
     * param types by ID against exactly this. */
    set_value_spv_type(c, result_value_id, result_ptr);
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
        set_result_air_type(c, result_value_id, result_ty_air);
        return;
    }
    /* A direct load through a non-struct `device T*` buffer arg (no GEP —
     * `load T, ptr %buf`) reads element 0 of the `{ runtimearray<T> }`
     * Block. The pointer here is the Block VARIABLE itself, so the load
     * type (T) doesn't match the var's pointee (the Block struct); inject
     * an OpAccessChain %var, 0(member), 0(elem) to reach the element. */
    uint32_t rtarr_elem = buffer_arg_runtimearray_elem(c, ptr_id);
    if (rtarr_elem) {
        uint32_t elem_spv = emit_air_type(c, rtarr_elem);
        uint32_t elem_ptr = emit_type_pointer(c, rtarr_elem, elem_spv,
                                              LAGFX_SPV_STORAGE_STORAGE_BUFFER);
        uint32_t chain_id = lagfx_spv_builder_alloc_id(c->b);
        uint32_t zero = emit_const_uint32(c, 0u);
        uint32_t ac[] = { elem_ptr, chain_id, ptr_spv, zero, zero };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_ACCESS_CHAIN, ac, 5);
        ptr_spv = chain_id;
    }
    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[] = { result_spv, result_id, ptr_spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, ops, 3);
    bind_value_spv(c, result_value_id, result_id);
    set_result_air_type(c, result_value_id, result_ty_air);
}

/* Find a module AIR type index for a float vector of `lanes` components.
 * Returns LAGFX_AIR_TYPE_NONE if the module declares no such type. Used to
 * give shuffle/other results a concrete vector AIR type so downstream
 * BINOP/etc. result-type inference doesn't default to vec4. */
static uint32_t find_air_float_vec_type(xlate_ctx_t *c, uint32_t lanes) {
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    for (uint32_t i = 0; i < n_types; i++) {
        if (ts[i].kind == LAGFX_AIR_TYPE_VECTOR && ts[i].num_op >= 2u
            && ts[i].op[0] == lanes) {
            uint32_t elem = ts[i].op[1];
            if (elem < n_types && ts[elem].kind == LAGFX_AIR_TYPE_FLOAT)
                return i;
        }
    }
    return LAGFX_AIR_TYPE_NONE;
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
    /* Result width = number of mask lanes. The mask is a <N x i32>
     * constant, so its component count IS the result vector width. The
     * old code hardcoded 4 lanes / vec4 result, so a 3-lane shuffle (e.g.
     * the scalar->vec3 splat Apple emits for mix(v3,v3,scalar)) produced
     * a v4 that then mismatched the v3 intrinsic operand (spirv-val
     * reject). Default 4 only when the mask is unresolvable. */
    uint32_t shuf_n = 4u;

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
                shuf_n = n;
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
                shuf_n = n;
                for (uint32_t i = 0; i < n; i++) {
                    mask_lanes[i] = raw[i];
                }
            }
        } else if (k->kind == LAGFX_AIR_CONST_NULL) {
            /* zeroinitializer mask (e.g. <N x i32> splat-to-lane-0). The
             * NULL const doesn't carry a width, so derive N from the AIR
             * value type if available; lanes are all 0. */
            uint32_t mty = value_air_type_idx(c, msk_id);
            uint32_t n_types = 0;
            const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
            if (mty < n_types && ts[mty].kind == LAGFX_AIR_TYPE_VECTOR
                && ts[mty].num_op >= 1u) {
                uint32_t n = ts[mty].op[0];
                shuf_n = (n >= 2u && n <= 4u) ? n : 4u;
            }
            for (uint32_t i = 0; i < 4u; i++) mask_lanes[i] = 0u;
        }
    }

    /* Result vector type matches the mask lane count. */
    uint32_t result_ty = (shuf_n == 2u) ? vec2_f
                       : (shuf_n == 3u) ? emit_type_vec3_f(c)
                       : vec4_f;
    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t ops[8] = {
        result_ty, result_id, v1_spv, v2_spv,
        mask_lanes[0], mask_lanes[1], mask_lanes[2], mask_lanes[3],
    };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VECTOR_SHUFFLE, ops, 4u + shuf_n);
    bind_value_spv(c, result_value_id, result_id);
    /* Record the result's vector AIR type (float vecN) so a downstream
     * BINOP (e.g. the `v3 * splat` in normalize()) infers the right result
     * width instead of defaulting to vec4. */
    set_result_air_type(c, result_value_id, find_air_float_vec_type(c, shuf_n));
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
    /* Unresolvable inserted values must fall back to an undef of the
     * MEMBER's type, not vec4 — inserting a v4float undef into a float
     * member is a spirv-val type mismatch (VfxXgb's point_size member,
     * whose value flows through not-yet-handled control flow). */
    uint32_t val_fallback = vec4_f;
    {
        uint32_t n_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
        if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION &&
            ts[fn_ty].num_op >= 2u) {
            uint32_t ret_ty = ts[fn_ty].op[1];
            if (ret_ty < n_types &&
                (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
                 ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED) &&
                1u + field < ts[ret_ty].num_op) {
                val_fallback = emit_air_type(c, ts[ret_ty].op[1u + field]);
            }
        }
    }
    uint32_t val_spv = resolve_or_undef(c, val_id, val_fallback);

    uint32_t result_id = lagfx_spv_builder_alloc_id(c->b);
    /* OpCompositeInsert: [result_type, result, object, composite, idx0, ...] */
    uint32_t ops[] = { struct_id, result_id, val_spv, agg_spv, field };
    lagfx_spv_builder_emit_op(c->b, 82 /* OpCompositeInsert */, ops, 5);
    bind_value_spv(c, result_value_id, result_id);
    set_result_air_type(c, result_value_id, LAGFX_AIR_TYPE_NONE);
}

static void emit_inst_ret(xlate_ctx_t *c, const lagfx_air_inst_t *inst,
                            uint32_t next_val_id) {
    /* Helper functions return their value directly: OpReturnValue for a
     * non-void return type (typed-undef fallback when the RET operand is
     * unresolved or its tracked SPIR-V type mismatches — the degradation
     * contract: typed undefs, never ill-typed ops), OpReturn for void. */
    if (c->is_helper) {
        if (c->helper_ret_spv) {
            uint32_t v = 0u;
            if (inst->num_ops >= 1u) {
                uint32_t val_id = resolve_relative((uint32_t)inst->ops[0],
                                                   next_val_id);
                v = resolve_value_spv(c, val_id);
                if (v) {
                    uint32_t vt = get_value_spv_type(c, val_id);
                    if (vt && vt != c->helper_ret_spv) v = 0u;
                }
            }
            if (!v) v = emit_undef(c, c->helper_ret_spv);
            uint32_t o[] = { v };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN_VALUE, o, 1);
        } else {
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN, NULL, 0);
        }
        return;
    }
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

        /* A Metal vertex returns either a bare `float4` (`vertex float4
         * f()`) or a struct whose first field is the `[[position]]` vec4
         * (`struct VOut { float4 position [[position]]; ... }`). The
         * former arrives as a vec4 value (insertelement chain), the
         * latter as a struct value (insertvalue chain). We must store the
         * vec4 directly in the bare case but OpCompositeExtract field 0
         * in the struct case — extracting field 0 of a bare vec4 yields a
         * SCALAR typed as v4float (spirv-val reject; the scalarmath
         * black-box bug). Decide by the FUNCTION's declared return type
         * (authoritative — the RET operand's tracked type can be NONE,
         * e.g. the demo's untracked insertvalue result). */
        bool is_struct = false;
        {
            uint32_t n_types = 0;
            const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
            uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
            if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION &&
                ts[fn_ty].num_op >= 2u) {
                uint32_t ret_ty = ts[fn_ty].op[1];
                if (ret_ty < n_types &&
                    (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
                     ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
                    is_struct = true;
                }
            }
        }

        uint32_t store_src = val_spv;
        if (is_struct) {
            uint32_t extract_id = lagfx_spv_builder_alloc_id(c->b);
            uint32_t op_ex[] = { vec4_f, extract_id, val_spv, 0u };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, op_ex, 4);
            store_src = extract_id;
        }

        uint32_t op_st[] = { c->id_position_var, store_src };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, op_st, 2);

        /* Store each struct member to its Location-k Output varying so the
         * fragment's stage_in inputs are fed. Member 0 is the position (already
         * → gl_Position; also emitted as Location-0 varying to match the
         * fragment's first stage_in). Without this the fragment's UV varying is
         * unbound → 0 → samples texel(0,0) → black composites. */
        if (is_struct) {
            for (uint32_t k = 0; k < c->num_vertex_outs; k++) {
                uint32_t mem_ty = vertex_output_type_spv_idx(c, k);
                uint32_t mem_id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t op_ex[] = { mem_ty, mem_id, val_spv, k };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, op_ex, 4);
                uint32_t op_vst[] = { c->id_vertex_out_vars[k], mem_id };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, op_vst, 2);
            }
        }
    }

    /* Fragment shader: the returned value IS the colour. A Metal
     * `fragment float4 f()` returns a bare vec4 directly (no struct
     * wrapper), so store the resolved RET operand straight to the
     * Location-0 colour output. Without this the colour output is
     * never written → undefined fragment colour → the geometry
     * renders blank (paid for: triangle_fragment emitted an empty
     * body and the centre pixel stayed the clear colour). */
    if (c->stage == LAGFX_XLATE_STAGE_FRAGMENT && inst->num_ops >= 1u &&
        c->id_color_var) {
        uint32_t val_rel = (uint32_t)inst->ops[0];
        uint32_t val_id  = resolve_relative(val_rel, next_val_id);

        if (c->num_color_outputs > 1u) {
            /* Multi-target: the RET value is a struct (insertvalue chain);
             * extract member k and store it to its Location-k output. */
            uint32_t struct_spv = resolve_value_spv(c, val_id);
            for (uint32_t k = 0; k < c->num_color_outputs; k++) {
                uint32_t mem_ty = fragment_output_type_spv_idx(c, k);
                uint32_t mem_id;
                if (struct_spv) {
                    mem_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t op_ex[] = { mem_ty, mem_id, struct_spv, k };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT,
                                              op_ex, 4);
                } else {
                    mem_id = emit_undef(c, mem_ty);
                }
                uint32_t op_st[] = { c->id_color_vars[k], mem_id };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, op_st, 2);
            }
        } else {
            /* Single target. Fallback-typed to the colour output's actual
             * type (float/float2/float4) so an unresolved RET operand's
             * OpUndef matches the store target. */
            uint32_t out_ty  = fragment_output_type_spv_idx(c, 0u);
            uint32_t val_spv = resolve_or_undef(c, val_id, out_ty);
            /* A single-target fragment can still return a STRUCT wrapper
             * (`struct Out { float4 color [[color(0)]]; }` — the
             * TvcmXc_Isrc login shader): storing the struct into the
             * v4float Output is a spirv-val type mismatch. Decide by the
             * function's declared return type (authoritative, same rule
             * as the vertex path) and extract member 0. */
            {
                uint32_t n_types = 0;
                const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
                uint32_t fn_ty = c->fn ? c->fn->type_index : LAGFX_AIR_TYPE_NONE;
                if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION &&
                    ts[fn_ty].num_op >= 2u) {
                    uint32_t ret_ty = ts[fn_ty].op[1];
                    if (ret_ty < n_types &&
                        (ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
                         ts[ret_ty].kind == LAGFX_AIR_TYPE_STRUCT_NAMED)) {
                        uint32_t extract_id = lagfx_spv_builder_alloc_id(c->b);
                        uint32_t op_ex[] = { out_ty, extract_id, val_spv, 0u };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT,
                                                  op_ex, 4);
                        val_spv = extract_id;
                    }
                }
            }
            uint32_t op_st[] = { c->id_color_var, val_spv };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, op_st, 2);
        }
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

    /* Resolve the LHS operand's AIR type (NONE if unknown).
     * value_air_type_idx covers inst results, args, function-local
     * consts, and module consts in one place — including the local-const
     * range that the old inline lookup missed (which mis-typed float
     * literals as module constants). The RHS type isn't needed: SPIR-V
     * arithmetic result type follows the LHS/result type. */
    uint32_t lhs_ty = value_air_type_idx(c, lhs_id);
    /* An unresolvable LHS (e.g. a PHI not yet handled by control flow)
     * falls back to the RHS operand's type — SPIR-V arithmetic requires
     * BOTH operands to match the result type, and a v4float-default undef
     * next to a resolved scalar float RHS is a spirv-val reject (Xgc). */
    if (lhs_ty == LAGFX_AIR_TYPE_NONE && !resolve_value_spv(c, lhs_id))
        lhs_ty = value_air_type_idx(c, rhs_id);

    /* Default to float if we can't resolve the LHS type. */
    bool is_float = true;
    uint32_t result_ty_air = lhs_ty;
    if (lhs_ty != LAGFX_AIR_TYPE_NONE) {
        uint32_t n_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        if (lhs_ty < n_types) {
            const lagfx_air_type_t *t = &ts[lhs_ty];
            if (t->kind == LAGFX_AIR_TYPE_FLOAT ||
                t->kind == LAGFX_AIR_TYPE_HALF) {
                /* `half` is a floating-point type — `fmul half` must lower to
                 * OpFMul, not OpIMul (spirv-val "Expected int scalar or vector
                 * type as Result Type: IMul"). */
                is_float = true;
            } else if (t->kind == LAGFX_AIR_TYPE_VECTOR) {
                /* A vector is float iff its ELEMENT type is float OR half. A
                 * uintN vector is integer (e.g. `uint4 % 31` → OpUMod, not
                 * FMod); the old code classified every VECTOR as float,
                 * emitting integer ops with a float result type (reject). */
                uint32_t elem = t->num_op >= 2u ? t->op[1] : 0u;
                is_float = (elem < n_types &&
                            (ts[elem].kind == LAGFX_AIR_TYPE_FLOAT ||
                             ts[elem].kind == LAGFX_AIR_TYPE_HALF));
            } else {
                is_float = false;
            }
        }
    }

    /* Determine result SPIR-V type. AIR type unknown → the operands'
     * TRACKED SPIR-V type before the vec4f guess (a scalar-float chain
     * through an untyped value fed FDiv a v4float result type — spirv-val
     * reject, the GammaLUTs helpers). */
    uint32_t result_spv_type;
    if (result_ty_air != LAGFX_AIR_TYPE_NONE) {
        result_spv_type = emit_air_type(c, result_ty_air);
    } else {
        uint32_t tracked = get_value_spv_type(c, lhs_id);
        if (!tracked) tracked = get_value_spv_type(c, rhs_id);
        result_spv_type = tracked ? tracked
                        : is_float ? emit_type_vec4_f(c)
                                   : emit_type_int_w(c, 32u, 0u);
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
    bool use_vector_times_scalar = false;

    /* Boolean logic: LLVM lowers `a && b` / `a || b` / `a ^ b` on i1 to
     * and/or/xor — SPIR-V requires OpLogical* on OpTypeBool operands
     * ("Expected int scalar or vector type as Result Type: BitwiseXor",
     * the Xgc login fragment). Covers the CMP-result BOOL marker, i1, and
     * vectors of i1. */
    bool is_bool = (lhs_ty == LAGFX_AIR_TYPE_BOOL);
    if (!is_bool && lhs_ty != LAGFX_AIR_TYPE_NONE) {
        uint32_t n_types_b = 0;
        const lagfx_air_type_t *tsb = lagfx_air_module_types(c->m, &n_types_b);
        if (lhs_ty < n_types_b) {
            const lagfx_air_type_t *bt = &tsb[lhs_ty];
            if (bt->kind == LAGFX_AIR_TYPE_VECTOR && bt->num_op >= 2u &&
                bt->op[1] < n_types_b)
                bt = &tsb[bt->op[1]];
            is_bool = (bt->kind == LAGFX_AIR_TYPE_INTEGER &&
                       bt->num_op >= 1u && bt->op[0] == 1u);
        }
    }
    if (is_bool && (llvm_binop == 10 || llvm_binop == 11 || llvm_binop == 12)) {
        uint32_t bool_op =
            (llvm_binop == 10) ? LAGFX_SPV_OP_LOGICAL_AND :
            (llvm_binop == 11) ? LAGFX_SPV_OP_LOGICAL_OR
                               : LAGFX_SPV_OP_LOGICAL_NOT_EQUAL; /* xor */
        uint32_t ops[] = { result_spv_type, result_spv, lhs_spv, rhs_spv };
        lagfx_spv_builder_emit_op(c->b, bool_op, ops, 4);
        bind_value_spv(c, result_value_id, result_spv);
        set_result_air_type(c, result_value_id, lhs_ty);
        set_value_spv_type(c, result_value_id, result_spv_type);
        return;
    }

    switch (llvm_binop) {
        case 0:  /* BINOP_ADD  → FAdd / IAdd  */ spv_op = is_float ? LAGFX_SPV_OP_FADD : LAGFX_SPV_OP_IADD; break;
        case 1:  /* BINOP_SUB  → FSub / ISub  */ spv_op = is_float ? LAGFX_SPV_OP_FSUB : LAGFX_SPV_OP_ISUB; break;
        case 2:  /* BINOP_MUL  → FMul / IMul  */ {
            if (!is_float) {
                spv_op = LAGFX_SPV_OP_IMUL;
            } else {
                uint32_t rhs_ty = value_air_type_idx(c, rhs_id);
                uint32_t n_types_local = 0;
                const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types_local);
                
                bool lhs_vec = (lhs_ty != LAGFX_AIR_TYPE_NONE && lhs_ty < n_types_local && 
                                ts[lhs_ty].kind == LAGFX_AIR_TYPE_VECTOR);
                bool rhs_vec = (rhs_ty != LAGFX_AIR_TYPE_NONE && rhs_ty < n_types_local && 
                                ts[rhs_ty].kind == LAGFX_AIR_TYPE_VECTOR);
                bool lhs_float_scalar = (lhs_ty != LAGFX_AIR_TYPE_NONE && lhs_ty < n_types_local && 
                                         ts[lhs_ty].kind == LAGFX_AIR_TYPE_FLOAT);
                bool rhs_float_scalar = (rhs_ty != LAGFX_AIR_TYPE_NONE && rhs_ty < n_types_local && 
                                         ts[rhs_ty].kind == LAGFX_AIR_TYPE_FLOAT);
                
                if ((lhs_vec && rhs_float_scalar) || (rhs_vec && lhs_float_scalar)) {
                    use_vector_times_scalar = true;
                } else {
                    spv_op = LAGFX_SPV_OP_FMUL;
                }
            }
            break;
        }
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

    if (spv_op == 0u && !use_vector_times_scalar) return;

    if (use_vector_times_scalar) {
        uint32_t vec_spv, scal_spv;
        uint32_t n_types_local = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types_local);
        
        bool lhs_vec = (lhs_ty != LAGFX_AIR_TYPE_NONE && lhs_ty < n_types_local && 
                        ts[lhs_ty].kind == LAGFX_AIR_TYPE_VECTOR);
        
        if (lhs_vec) { vec_spv = lhs_spv; scal_spv = rhs_spv; }
        else         { vec_spv = rhs_spv; scal_spv = lhs_spv; }
        
        uint32_t ops[4] = { result_spv_type, result_spv, vec_spv, scal_spv };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VECTOR_TIMES_SCALAR, ops, 4);
        bind_value_spv(c, result_value_id, result_spv);
        set_result_air_type(c, result_value_id, result_ty_air);
        set_value_spv_type(c, result_value_id, result_spv_type);
        return;
    }

    /* Operand/result type consistency: SPIR-V arithmetic requires both
     * operands to BE the result type (shift amounts exempt — any int
     * width). A resolved-but-wrongly-typed operand (mis-resolution through
     * pending control flow; Xgc's `OpFSub %float %float %v4float`) is
     * replaced by an undef of the result type — same degradation contract
     * as an unresolved operand. */
    if (!(llvm_binop >= 7 && llvm_binop <= 9)) {
        uint32_t rt = value_air_type_idx(c, rhs_id);
        if (result_ty_air != LAGFX_AIR_TYPE_NONE &&
            rt != LAGFX_AIR_TYPE_NONE && rt != result_ty_air)
            rhs_spv = emit_undef(c, result_spv_type);
        uint32_t lt = value_air_type_idx(c, lhs_id);
        if (result_ty_air != LAGFX_AIR_TYPE_NONE &&
            lt != LAGFX_AIR_TYPE_NONE && lt != result_ty_air)
            lhs_spv = emit_undef(c, result_spv_type);
        /* SPIR-V-type-level check catches producers whose AIR type went
         * dark (default-typed fallback results). */
        uint32_t rst = get_value_spv_type(c, rhs_id);
        if (rst && rst != result_spv_type)
            rhs_spv = emit_undef(c, result_spv_type);
        uint32_t lst = get_value_spv_type(c, lhs_id);
        if (lst && lst != result_spv_type)
            lhs_spv = emit_undef(c, result_spv_type);
    }


    uint32_t ops[5];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = lhs_spv;
    ops[3] = rhs_spv;
    lagfx_spv_builder_emit_op(c->b, spv_op, ops, 4);

    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, result_ty_air);
    set_value_spv_type(c, result_value_id, result_spv_type);
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

    /* Resolve operand; type from the AIR side table (insts, args, local
     * and module consts), else the tracked SPIR-V type (values whose AIR
     * type is unknown but whose emitted type is — e.g. extractelement
     * feeding fneg in the GammaLUTs helpers), else the vec4f default. */
    uint32_t opval_id = resolve_relative(opval_rel, next_val_id);
    uint32_t ty_air = value_air_type_idx(c, opval_id);
    uint32_t result_spv_type;
    if (ty_air != LAGFX_AIR_TYPE_NONE) {
        result_spv_type = emit_air_type(c, ty_air);
    } else {
        uint32_t tracked = get_value_spv_type(c, opval_id);
        result_spv_type = tracked ? tracked : emit_type_vec4_f(c);
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

    /* Bind the result value-id. AIR type may be NONE (unknown operand) —
     * but the EMITTED SPIR-V type is always known; track it so downstream
     * binops type their result from it instead of the vec4f default. */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, ty_air);
    set_value_spv_type(c, result_value_id, result_spv_type);
}

/* Best-effort AIR type index for a resolved value-id: instruction
 * results, then function args, then module constants. 0 = unknown.
 * Mirrors the operand-type lookup in emit_inst_binop. */
static uint32_t value_air_type_idx(xlate_ctx_t *c, uint32_t value_id) {
    if (value_id >= c->inst_id_base && value_id < c->value_id_capacity) {
        int idx = (int)(value_id - c->inst_id_base);
        if (idx >= 0 && (uint32_t)idx < c->num_insts &&
            c->inst_result_air_type[idx] != LAGFX_AIR_TYPE_NONE) {
            return c->inst_result_air_type[idx];
        }
    }
    if (value_id >= c->arg_id_base) {
        uint32_t a = value_id - c->arg_id_base;
        if (a < c->num_args) return c->arg_air_type_ids[a];
    }
    /* Function-local constants occupy value-ids
     * [arg_id_base + num_args, inst_id_base). This range OVERLAPS the
     * module-const test below (arg_id_base == module_val_count), so it
     * MUST be resolved first — otherwise a local-const operand is read as
     * a module constant → wrong type. (The local FLOAT consts in
     * clear/blit are type index 0 = float; before this, the overlap +
     * the 0-is-unknown bug both hit, dropping/mis-typing the SELECT.) */
    {
        uint32_t lc_base = c->arg_id_base + c->num_args;
        uint32_t n_lc = 0u;
        const lagfx_air_constant_t *lc =
            lagfx_air_function_body_local_constants(c->body, &n_lc);
        if (lc && value_id >= lc_base && value_id < lc_base + n_lc) {
            return lc[value_id - lc_base].type_index;
        }
    }
    if (value_id >= c->module_val_count) {
        uint32_t n = 0u;
        const lagfx_air_constant_t *consts = lagfx_air_module_constants(c->m, &n);
        uint32_t ci = value_id - c->module_val_count;
        if (ci < n) return consts[ci].type_index;
    }
    return LAGFX_AIR_TYPE_NONE;
}

/* Map an LLVM CmpInst::Predicate to its SPIR-V relational opcode
 * (§3.32.15). 0 = no mapping (FCMP_FALSE/FCMP_TRUE or unknown).
 *
 * CmpInst::Predicate values appear on the bitcode wire UNCHANGED — the
 * reader's getDecodedCmpPredicate returns Record[OpNum] directly. This
 * is the documented exception to the bitcode-subcode-vs-IR-enum rule
 * (BINOP/CAST/etc. do remap; CMP does not). Values from
 * llvm/IR/InstrTypes.h enum Predicate. */
static uint32_t cmp_predicate_to_spv_op(int pred) {
    switch (pred) {
        /* Float ordered. */
        case 1:  return LAGFX_SPV_OP_FORD_EQUAL;          /* FCMP_OEQ */
        case 2:  return LAGFX_SPV_OP_FORD_GREATER_THAN;   /* FCMP_OGT */
        case 3:  return LAGFX_SPV_OP_FORD_GREATER_EQUAL;  /* FCMP_OGE */
        case 4:  return LAGFX_SPV_OP_FORD_LESS_THAN;      /* FCMP_OLT */
        case 5:  return LAGFX_SPV_OP_FORD_LESS_EQUAL;     /* FCMP_OLE */
        case 6:  return LAGFX_SPV_OP_FORD_NOT_EQUAL;      /* FCMP_ONE */
        case 7:  return LAGFX_SPV_OP_ORDERED;             /* FCMP_ORD */
        /* Float unordered. */
        case 8:  return LAGFX_SPV_OP_UNORDERED;           /* FCMP_UNO */
        case 9:  return LAGFX_SPV_OP_FUNORD_EQUAL;        /* FCMP_UEQ */
        case 10: return LAGFX_SPV_OP_FUNORD_GREATER_THAN; /* FCMP_UGT */
        case 11: return LAGFX_SPV_OP_FUNORD_GREATER_EQUAL;/* FCMP_UGE */
        case 12: return LAGFX_SPV_OP_FUNORD_LESS_THAN;    /* FCMP_ULT */
        case 13: return LAGFX_SPV_OP_FUNORD_LESS_EQUAL;   /* FCMP_ULE */
        case 14: return LAGFX_SPV_OP_FUNORD_NOT_EQUAL;    /* FCMP_UNE */
        /* Integer equality. */
        case 32: return LAGFX_SPV_OP_IEQUAL;              /* ICMP_EQ */
        case 33: return LAGFX_SPV_OP_INOT_EQUAL;          /* ICMP_NE */
        /* Integer unsigned. */
        case 34: return LAGFX_SPV_OP_UGREATER_THAN;       /* ICMP_UGT */
        case 35: return LAGFX_SPV_OP_UGREATER_EQUAL;      /* ICMP_UGE */
        case 36: return LAGFX_SPV_OP_ULESS_THAN;          /* ICMP_ULT */
        case 37: return LAGFX_SPV_OP_ULESS_EQUAL;         /* ICMP_ULE */
        /* Integer signed. */
        case 38: return LAGFX_SPV_OP_SGREATER_THAN;       /* ICMP_SGT */
        case 39: return LAGFX_SPV_OP_SGREATER_EQUAL;      /* ICMP_SGE */
        case 40: return LAGFX_SPV_OP_SLESS_THAN;          /* ICMP_SLT */
        case 41: return LAGFX_SPV_OP_SLESS_EQUAL;         /* ICMP_SLE */
        default: return 0u;  /* FCMP_FALSE(0)/FCMP_TRUE(15)/unknown */
    }
}

/* ===================================================================
 * CMP / CMP2 handler — LLVM CmpInst → SPIR-V relational ops (§3.32.15)
 *
 * Record layout is the verbatim bitcode record (rec.ops is memcpy'd
 * into inst->ops by lagfx_air_function_body_open), in USE_RELATIVE_IDS
 * form — the SAME shape as BINOP (confirmed against emit_inst_binop):
 *   ops[0] = LHS value (relative ref)
 *   ops[1] = RHS value (relative ref)
 *   ops[2] = predicate (CmpInst::Predicate; IR enum value unchanged)
 *   ops[3] = optional fast-math flags (CMP2/FP only) — ignored
 * There is NO leading explicit-type slot in the common (non-forward-
 * ref) case: getValueTypePair consumes one operand for the LHS and
 * infers RHS's type from it. CMP (code 9, legacy) and CMP2 (code 28,
 * modern) share this layout for our purposes.
 *
 * Result type is a BOOLEAN per §3.32.15: OpTypeBool for scalar
 * operands, or vec<N x bool> matching the operand component count for
 * vector operands — NOT the operand type. */
static void emit_inst_cmp(xlate_ctx_t *c, uint32_t inst_idx,
                          const lagfx_air_inst_t *inst,
                          uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 3u) return;

    uint32_t lhs_rel = (uint32_t)inst->ops[0];
    uint32_t rhs_rel = (uint32_t)inst->ops[1];
    int      pred    = (int)(inst->ops[2]);

    uint32_t spv_op = cmp_predicate_to_spv_op(pred);
    if (spv_op == 0u) {
        LAGFX_TRACE("cmp: predicate %d unmapped (FCMP_FALSE/TRUE?) — drop", pred);
        return;
    }

    uint32_t lhs_id = resolve_relative(lhs_rel, next_val_id);
    uint32_t rhs_id = resolve_relative(rhs_rel, next_val_id);

    /* Result type: bool, or vec<N x bool> if the operands are vectors.
     * Derive the lane count from the LHS operand's AIR type. */
    uint32_t bool_spv = emit_type_bool(c);
    uint32_t result_spv_type = bool_spv;
    uint32_t lhs_ty = value_air_type_idx(c, lhs_id);
    /* Unresolvable LHS (e.g. a PHI pending control-flow support): take the
     * RHS operand's type — both compare operands must match, and a scalar
     * default undef next to a resolved v4float RHS is a spirv-val reject
     * (Xgc: "Expected left and right operands to have the same type"). */
    if (lhs_ty == LAGFX_AIR_TYPE_NONE && !resolve_value_spv(c, lhs_id))
        lhs_ty = value_air_type_idx(c, rhs_id);
    if (lhs_ty != LAGFX_AIR_TYPE_NONE) {
        uint32_t n_types = 0u;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        if (lhs_ty < n_types && ts[lhs_ty].kind == LAGFX_AIR_TYPE_VECTOR) {
            uint32_t lanes = ts[lhs_ty].num_op >= 1u ? (uint32_t)ts[lhs_ty].op[0] : 4u;
            result_spv_type = emit_type_vec(c, bool_spv, lanes);
        }
    }

    /* Resolve operands to SPIR-V ids. The undef fallback must be typed as
     * the OPERAND type (both compare operands share it), NOT the bool
     * result type — otherwise an unresolved RHS (e.g. the literal `0` in
     * `icmp eq i8 %x, 0`, common in function-constant predicate chains)
     * becomes OpUndef %bool and the OpIEqual gets a bool operand
     * (spirv-val: "operands must be int"). Derive it from the LHS AIR type
     * when known. */
    uint32_t operand_spv_type;
    if (lhs_ty != LAGFX_AIR_TYPE_NONE) {
        operand_spv_type = emit_air_type(c, lhs_ty);
    } else {
        /* LHS type unknown: default by the comparison KIND, not bool. LLVM
         * FCmp predicates are 0..15, ICmp 32..41 — a float compare needs
         * float operands (FOrdEqual etc.), an int compare needs int. */
        operand_spv_type = (pred < 32)
                             ? emit_type_float32(c)
                             : emit_type_int_w(c, 32u, 0u);
    }
    uint32_t lhs_spv = resolve_or_undef(c, lhs_id, operand_spv_type);
    uint32_t rhs_spv = resolve_or_undef(c, rhs_id, operand_spv_type);

    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);

    /* Emit: [result_type(bool), result_id, operand1, operand2] (§3.32.15) */
    uint32_t ops[4];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = lhs_spv;
    ops[3] = rhs_spv;
    lagfx_spv_builder_emit_op(c->b, spv_op, ops, 4);

    bind_value_spv(c, result_value_id, result_spv);
    /* Record the SPIR-V-bool sentinel so a downstream SELECT over comparison
     * results resolves a bool result type (emit_air_type maps the sentinel
     * to OpTypeBool). For a vector compare the result is a bool-vector; we
     * still record the scalar-bool sentinel — the select path handles the
     * common scalar predicate chains (function-constant predicates). The
     * PRECISE type (incl. bool-vectors) goes to the SPIR-V-type side table
     * for consumers that must type-check (bool->float select lowering). */
    set_result_air_type(c, result_value_id, LAGFX_AIR_TYPE_BOOL);
    set_value_spv_type(c, result_value_id, result_spv_type);
}

/* CMP2 (code 28, modern) shares CMP's record layout for our purposes
 * (LHS, RHS, predicate, optional fast-math flags). Forward to the
 * shared handler. */
static void emit_inst_cmp2(xlate_ctx_t *c, uint32_t inst_idx,
                             const lagfx_air_inst_t *inst,
                             uint32_t result_value_id, uint32_t next_val_id) {
    emit_inst_cmp(c, inst_idx, inst, result_value_id, next_val_id);
}

/* ===================================================================
 * EXTRACTVAL / INSERTVAL handlers — SPIR-V composite extract/insert
 *
 * EXTRACTVAL: [agg_rel (or agg_type for fresh undef agg), idx0, idx1, ...]
 *   - ops[0] = aggregate value (relative-encoded)
 *   - ops[1..N] = LITERAL constant indices (NOT relative-encoded!)
 *   Result type = walk the aggregate AIR type by the literal indices.
 *
 * SPIR-V OpCompositeExtract: [result_type, result_id, Composite, <literal idx0>, ...]
 * The indices are emitted as literal integer words, copied straight from inst->ops[1..].
 *
 * Result types:
 *   - EXTRACTVAL result = the walked member type (from aggregate by indices)
 *   - INSERTVAL result = the aggregate type itself
 * =================================================================== */

static void emit_inst_extractval(xlate_ctx_t *c, uint32_t inst_idx,
                                  const lagfx_air_inst_t *inst,
                                  uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 2u) return;

    /* ops[0] = aggregate value ref (relative-encoded). */
    uint32_t agg_rel = (uint32_t)inst->ops[0];
    
    /* ops[1..N] = LITERAL constant indices — DO NOT resolve_relative! */
    uint32_t n_indices = inst->num_ops - 1u;
    if (n_indices == 0u) return;

    /* Resolve aggregate value-id. */
    uint32_t agg_id = resolve_relative(agg_rel, next_val_id);

    /* Walk the type by literal indices to get result type. */
    uint32_t n_types = 0u;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    
    uint32_t agg_ty_air = value_air_type_idx(c, agg_id);
    if (agg_ty_air == LAGFX_AIR_TYPE_NONE) {
        LAGFX_WARN("extractval: couldn't resolve aggregate operand type — drop");
        return;
    }

    /* Bounds check the aggregate type. */
    if (agg_ty_air >= n_types) {
        LAGFX_WARN("extractval: aggregate type index out of bounds — drop");
        return;
    }

    const lagfx_air_type_t *ty = &ts[agg_ty_air];
    
    /* Walk the type by each literal index. */
    for (uint32_t i = 0u; i < n_indices; i++) {
        uint32_t idx = (uint32_t)inst->ops[1u + i];

        if (ty->kind == LAGFX_AIR_TYPE_STRUCT_ANON ||
            ty->kind == LAGFX_AIR_TYPE_STRUCT_NAMED) {
            /* STRUCT: op[0] = packed, op[1..N] = field type indices. */
            if (idx >= ty->num_op - 1u) {
                LAGFX_WARN("extractval: struct field index %u out of bounds (fields=%u) — drop",
                           idx, ty->num_op - 1u);
                return;
            }
            uint32_t field_ty = ty->op[1u + idx];
            if (field_ty >= n_types) {
                LAGFX_WARN("extractval: struct field type index out of bounds — drop");
                return;
            }
            ty = &ts[field_ty];
        } else if (ty->kind == LAGFX_AIR_TYPE_ARRAY ||
                   ty->kind == LAGFX_AIR_TYPE_VECTOR) {
            /* ARRAY/VECTOR: op[0] = length/lanes, op[1] = element type. */
            if (ty->num_op < 2u) {
                LAGFX_WARN("extractval: array/vector has no element type — drop");
                return;
            }
            uint32_t elem_ty = ty->op[1];
            if (elem_ty >= n_types) {
                LAGFX_WARN("extractval: array/vector element type index out of bounds — drop");
                return;
            }
            ty = &ts[elem_ty];
        } else {
            /* Non-aggregate type with indices is invalid. */
            LAGFX_WARN("extractval: operand type %u is not a struct/array/vector — drop",
                       ty->kind);
            return;
        }
    }

    uint32_t result_ty_air = (uint32_t)(ty - ts);
    uint32_t result_spv_type = emit_air_type(c, result_ty_air);

    /* Resolve aggregate to SPIR-V id. */
    uint32_t agg_spv = resolve_or_undef(c, agg_id, emit_air_type(c, agg_ty_air));

    /* Allocate result SPIR-V id. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Build OpCompositeExtract operands: [result_type, result_id, composite, idx0, ...] */
    uint32_t ops[16];  /* Max 8 indices + 4 fixed words */
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = agg_spv;
    
    uint32_t n_ops = 3u;
    for (uint32_t i = 0u; i < n_indices && i < 8u; i++) {
        if (n_ops >= 16u) break;
        ops[n_ops++] = (uint32_t)inst->ops[1u + i];
    }
    
    if (n_indices > 8u) {
        LAGFX_WARN("extractval: too many indices (%u > 8), capping — drop", n_indices);
        return;
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_COMPOSITE_EXTRACT, ops, n_ops);

    /* Bind the result value-id and record AIR type. */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, result_ty_air);
}

/* ===================================================================
 * EXTRACTELT / INSERTELT handler — vector element access (SPIR-V §3.32.12)
 *
 * AIR record layout (verbatim bitcode record, USE_RELATIVE_IDS):
 *   EXTRACTELT (raw=6): ops[0] = Vector (rel),  ops[1] = Index (rel)        ; guard num_ops < 2
 *   INSERTELT  (raw=7): ops[0] = Vector (rel),  ops[1] = Element (rel),
 *                       ops[2] = Index (rel)                                 ; guard num_ops < 3
 *
 * SPIR-V OpVectorExtractDynamic (§3.32.12): [result_type(elem), result_id, Vector, Index]
 * SPIR-V OpVectorInsertDynamic (§3.32.12):  [result_type(vec),  result_id, Vector, Element, Index]
 *
 * Result types:
 *   - EXTRACTELT result = the vector's ELEMENT type (ts[vec_ty].op[1])
 *   - INSERTELT result = the VECTOR type itself
 */

static void emit_inst_extractelt(xlate_ctx_t *c, uint32_t inst_idx,
                                   const lagfx_air_inst_t *inst,
                                   uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 2u) return;

    /* Resolve operands. */
    uint32_t vector_rel = (uint32_t)inst->ops[0];
    uint32_t index_rel  = (uint32_t)inst->ops[1];

    uint32_t vector_id = resolve_relative(vector_rel, next_val_id);
    uint32_t index_id  = resolve_relative(index_rel, next_val_id);

    /* Resolve the vector's AIR type to get its element type. */
    uint32_t vec_ty_air = value_air_type_idx(c, vector_id);

    /* Bool-VECTOR operand: a vector compare's tracked AIR type is the
     * lossy scalar-BOOL sentinel, so the AIR walk below can't type it —
     * but the SPIR-V-type side table has the precise vNbool. Emit the
     * extraction with a bool element (this single dropped case fed
     * OpUndef %bool into EIGHT of Xgc's dispatched branch conditions). */
    {
        uint32_t vspv = get_value_spv_type(c, vector_id);
        /* Back-edge operand: unemitted def, but the dispatch pre-pass
         * already typed its spill variable. */
        if (!vspv && c->spill_type && vector_id < c->value_id_capacity)
            vspv = c->spill_type[vector_id];
        if (vspv &&
            (vec_ty_air == LAGFX_AIR_TYPE_NONE ||
             vec_ty_air == LAGFX_AIR_TYPE_BOOL)) {
            uint32_t b = emit_type_bool(c);
            bool is_bvec = false;
            for (uint32_t l = 2; l <= 4 && !is_bvec; l++)
                is_bvec = (vspv == emit_type_vec(c, b, l));
            if (is_bvec) {
                uint32_t vec_spv = resolve_or_undef(c, vector_id, vspv);
                uint32_t idx_spv = resolve_or_undef(c, index_id,
                                                    emit_type_int_w(c, 32u, 0u));
                uint32_t rid = lagfx_spv_builder_alloc_id(c->b);
                uint32_t o[] = { b, rid, vec_spv, idx_spv };
                lagfx_spv_builder_emit_op(c->b,
                    LAGFX_SPV_OP_VECTOR_EXTRACT_DYNAMIC, o, 4);
                bind_value_spv(c, result_value_id, rid);
                set_result_air_type(c, result_value_id, LAGFX_AIR_TYPE_BOOL);
                set_value_spv_type(c, result_value_id, b);
                return;
            }
        }
    }
    if (vec_ty_air == LAGFX_AIR_TYPE_NONE) {
        LAGFX_WARN("extractelt: couldn't resolve vector operand type — drop");
        return;
    }

    /* Check that it's actually a VECTOR type. */
    uint32_t n_types = 0u;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    if (vec_ty_air >= n_types || ts[vec_ty_air].kind != LAGFX_AIR_TYPE_VECTOR) {
        LAGFX_WARN("extractelt: vector operand type is not a VECTOR — drop");
        return;
    }

    /* Element AIR type = vec_ty.op[1] (lanes are op[0], element is op[1]).
     * NOTE: type INDEX 0 is valid (float is commonly type 0) — do NOT use a
     * truthiness check here, or a vec<float> arg drops (the branch_fragment
     * `in.uv.x` extractelement, which left the comparison reading an
     * undef-typed-bool). Validate by bounds, not by != 0. */
    if (ts[vec_ty_air].num_op < 2u) {
        LAGFX_WARN("extractelt: vector type missing element operand — drop");
        return;
    }
    uint32_t elem_ty_air = ts[vec_ty_air].op[1];
    if (elem_ty_air >= n_types) {
        LAGFX_WARN("extractelt: element type index out of range — drop");
        return;
    }

    /* Emit SPIR-V types. */
    uint32_t elem_spv_type = emit_air_type(c, elem_ty_air);
    uint32_t index_spv_type = emit_type_int_w(c, 32u, 0u); /* Index is i32. */

    /* Resolve operands to SPIR-V ids. */
    uint32_t vector_spv = resolve_or_undef(c, vector_id, elem_ty_air ? elem_spv_type : index_spv_type);
    uint32_t index_spv  = resolve_or_undef(c, index_id, index_spv_type);

    /* Allocate result SPIR-V id. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpVectorExtractDynamic: [result_type(elem), result_id, Vector, Index] */
    uint32_t ops[4];
    ops[0] = elem_spv_type;
    ops[1] = result_spv;
    ops[2] = vector_spv;
    ops[3] = index_spv;
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VECTOR_EXTRACT_DYNAMIC, ops, 4);

    /* Bind the result value-id and record AIR type (element type). */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, elem_ty_air);
}

static void emit_inst_insertelt(xlate_ctx_t *c, uint32_t inst_idx,
                                 const lagfx_air_inst_t *inst,
                                 uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 3u) return;

    /* Resolve operands. */
    uint32_t vector_rel = (uint32_t)inst->ops[0];
    uint32_t elem_rel   = (uint32_t)inst->ops[1];
    uint32_t index_rel  = (uint32_t)inst->ops[2];

    uint32_t vector_id = resolve_relative(vector_rel, next_val_id);
    uint32_t elem_id   = resolve_relative(elem_rel, next_val_id);
    uint32_t index_id  = resolve_relative(index_rel, next_val_id);

    /* Resolve the vector's AIR type. */
    uint32_t vec_ty_air = value_air_type_idx(c, vector_id);
    if (vec_ty_air == LAGFX_AIR_TYPE_NONE) {
        LAGFX_WARN("insertelt: couldn't resolve vector operand type — drop");
        return;
    }

    /* Check that it's actually a VECTOR type. */
    uint32_t n_types = 0u;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    if (vec_ty_air >= n_types || ts[vec_ty_air].kind != LAGFX_AIR_TYPE_VECTOR) {
        LAGFX_WARN("insertelt: vector operand type is not a VECTOR — drop");
        return;
    }

    /* Emit SPIR-V types. */
    uint32_t vec_spv_type = emit_air_type(c, vec_ty_air);
    uint32_t elem_spv_type = emit_air_type(c, ts[vec_ty_air].num_op >= 2u ? ts[vec_ty_air].op[1] : 0u);
    uint32_t index_spv_type = emit_type_int_w(c, 32u, 0u); /* Index is i32. */

    /* Resolve operands to SPIR-V ids. */
    uint32_t vector_spv = resolve_or_undef(c, vector_id, vec_spv_type);
    uint32_t elem_spv   = resolve_or_undef(c, elem_id, elem_spv_type);
    uint32_t index_spv  = resolve_or_undef(c, index_id, index_spv_type);

    /* Type-check the inserted element / base vector against the vector's
     * component/vector type — a mis-typed resolved operand (Xgc: v4float
     * inserted into v3float) degrades to a typed undef, matching the
     * unresolved-operand contract. */
    {
        uint32_t est = get_value_spv_type(c, elem_id);
        if (est && est != elem_spv_type)
            elem_spv = emit_undef(c, elem_spv_type);
        uint32_t vst = get_value_spv_type(c, vector_id);
        if (vst && vst != vec_spv_type)
            vector_spv = emit_undef(c, vec_spv_type);
    }

    /* Allocate result SPIR-V id. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpVectorInsertDynamic: [result_type(vec), result_id, Vector, Element, Index] */
    uint32_t ops[5];
    ops[0] = vec_spv_type;
    ops[1] = result_spv;
    ops[2] = vector_spv;
    ops[3] = elem_spv;
    ops[4] = index_spv;
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_VECTOR_INSERT_DYNAMIC, ops, 5);

    /* Bind the result value-id and record AIR type (vector type). */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, vec_ty_air);
}

/* ===================================================================
 * SELECT / VSELECT handler — LLVM select → SPIR-V OpSelect (§3.32.16)
 *
 * Record layout (verbatim bitcode record from lagfx_air_function_body_open):
 *   ops[0] = TrueVal  (relative value ref)
 *   ops[1] = FalseVal (relative value ref)
 *   ops[2] = Cond     (relative value ref)
 * No leading explicit-type slot in the common case; getValueTypePair
 * consumes one operand for TrueVal.
 *
 * SPIR-V OpSelect (§3.32.16) operand order:
 *   [result_type, result_id, Condition, Object_1(true), Object_2(false)]
 *
 * So we MUST reorder from AIR record order (true, false, cond):
 * emit Cond(ops[2]) FIRST into SPIR-V slot 2, then TrueVal(ops[0]),
 * then FalseVal(ops[1]). Do NOT pass them through in record order.
 */
static void emit_inst_select(xlate_ctx_t *c, uint32_t inst_idx,
                              const lagfx_air_inst_t *inst,
                              uint32_t result_value_id, uint32_t next_val_id) {
    if (inst->num_ops < 3u) return;

    uint32_t true_rel  = (uint32_t)inst->ops[0];
    uint32_t false_rel = (uint32_t)inst->ops[1];
    uint32_t cond_rel  = (uint32_t)inst->ops[2];

    /* Resolve all three operands to absolute value-ids. */
    uint32_t true_id  = resolve_relative(true_rel,  next_val_id);
    uint32_t false_id = resolve_relative(false_rel, next_val_id);
    uint32_t cond_id  = resolve_relative(cond_rel,  next_val_id);

    /* Result type: same as the selected values (true/false share a type).
     * Resolve from TrueVal operand's AIR type via value_air_type_idx. */
    uint32_t result_ty_air = value_air_type_idx(c, true_id);
    if (result_ty_air == LAGFX_AIR_TYPE_NONE) {
        /* Fall back to the false operand, then leave unresolved. */
        result_ty_air = value_air_type_idx(c, false_id);
    }
    if (result_ty_air == LAGFX_AIR_TYPE_NONE) {
        LAGFX_WARN("select: couldn't resolve operand type — drop");
        return;
    }
    uint32_t result_spv_type = emit_air_type(c, result_ty_air);

    /* Resolve operands to SPIR-V ids. */
    uint32_t cond_spv   = resolve_or_undef(c, cond_id,  emit_type_bool(c));
    uint32_t true_spv   = resolve_or_undef(c, true_id,  result_spv_type);
    uint32_t false_spv  = resolve_or_undef(c, false_id, result_spv_type);

    /* Allocate result SPIR-V id. */
    uint32_t result_spv = resolve_value_spv(c, result_value_id);
    if (!result_spv) {
        result_spv = lagfx_spv_builder_alloc_id(c->b);
    }

    /* Emit OpSelect: [result_type, result_id, Condition, TrueVal, FalseVal] */
    uint32_t ops[5];
    ops[0] = result_spv_type;
    ops[1] = result_spv;
    ops[2] = cond_spv;
    ops[3] = true_spv;
    ops[4] = false_spv;
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECT, ops, 5);

    /* Bind the result value-id. */
    bind_value_spv(c, result_value_id, result_spv);
    set_result_air_type(c, result_value_id, result_ty_air);
}

/* ===================================================================
 * Structured control flow (multi-basic-block bodies)
 *
 * SPIR-V demands STRUCTURED, reducible control flow: every header has an
 * explicit merge block, and a loop's back-edge must target a dedicated
 * continue block (not the header). LLVM AIR from Metal is reducible, but
 * the per-instruction emission in translate_body is single-basic-block —
 * it emits linearly and drops BR/PHI/SWITCH (they fall through the
 * default case). That is fine for the (vast) majority of compositor
 * shaders, whose translated entry-point function is a single block; but a
 * counted `for`/`while` lowers to a real multi-block loop with PHI nodes,
 * and dropping the terminators leaves loop instructions emitted AFTER the
 * OpReturn ("X must appear in a block").
 *
 * To keep the regression surface minimal we recognise exactly ONE family
 * here — the canonical LLVM "rotated" single loop (the shape `xcrun metal`
 * emits for a counted for/while):
 *
 *   entry:   ... ; br label %H            (unconditional)
 *   merge:   ... ; ret                    (after-loop block)
 *   H:       phi… ; … ; br i1 %c, %merge, %H   (self back-edge)
 *
 * Any body that does NOT match this exact shape falls through to the
 * existing linear path unchanged — so single-block shaders and the one
 * multi-block SkyLight entry (InPlaceColorOrClampEDR, an if/else diamond)
 * keep their current, validated behaviour.
 * =================================================================== */

/* getDecodedSignRotatedValue (llvm/Bitcode/BitcodeCommon-ish): PHI value
 * operands are sign-rotated relative ids. low bit = sign; magnitude in the
 * upper bits. Returns the signed delta to subtract from the PHI's result
 * value-number to recover the incoming value-id. */
static int64_t decode_sign_rotated(uint64_t v) {
    if ((v & 1u) == 0u) return (int64_t)(v >> 1);
    if (v != 1u)        return -(int64_t)(v >> 1);
    return INT64_MIN;   /* the special "INT_MIN" encoding; never seen here */
}

/* A reconstructed basic block: [first, last] inclusive instruction range
 * (last is the terminator). `index` is the LLVM basic-block number a BR
 * operand references. */
typedef struct {
    uint32_t index;       /* LLVM bb number (0-based) */
    uint32_t first_inst;  /* first instruction index in this block */
    uint32_t last_inst;   /* terminator instruction index */
} lagfx_bb_t;

static bool inst_is_terminator(lagfx_air_inst_code_t code) {
    return code == LAGFX_AIR_INST_RET || code == LAGFX_AIR_INST_BR ||
           code == LAGFX_AIR_INST_SWITCH || code == LAGFX_AIR_INST_UNREACHABLE ||
           code == LAGFX_AIR_INST_INDIRECTBR;
}

/* Partition the body into basic blocks (split after each terminator).
 * DECLAREBLOCKS (always inst 0, if present) is folded into block 0.
 * Returns the block count, or 0 if the body isn't cleanly partitionable
 * (e.g. the final instruction isn't a terminator). */
static uint32_t partition_basic_blocks(const xlate_ctx_t *c, lagfx_bb_t *bbs,
                                       uint32_t max_bbs) {
    uint32_t nbb = 0;
    uint32_t start = 0;
    /* Fold a leading DECLAREBLOCKS into block 0's range start. */
    if (c->num_insts > 0u && c->insts[0].code == LAGFX_AIR_INST_DECLAREBLOCKS)
        start = 0; /* keep at 0; the emitter skips DECLAREBLOCKS itself */
    for (uint32_t i = 0; i < c->num_insts; i++) {
        if (inst_is_terminator(c->insts[i].code)) {
            if (nbb >= max_bbs) return 0;
            bbs[nbb].index = nbb;
            bbs[nbb].first_inst = start;
            bbs[nbb].last_inst = i;
            nbb++;
            start = i + 1u;
        }
    }
    /* All instructions must be covered: the last instruction is a
     * terminator (start advanced past num_insts). */
    if (start != c->num_insts) return 0;
    return nbb;
}

/* Plan for the recognised single-loop shape. */
typedef struct {
    bool     valid;
    uint32_t entry_bb;    /* always 0 */
    uint32_t header_bb;   /* the loop block (phis + latch test + self back-edge) */
    uint32_t merge_bb;    /* the after-loop block (ends in RET) */
    /* The header block's conditional BR: which target is the back-edge
     * (== header) and which is the exit (== merge). */
    bool     cond_true_is_exit; /* true: BR cond ? merge : header */
} lagfx_loop_plan_t;

static lagfx_loop_plan_t detect_simple_loop(const xlate_ctx_t *c) {
    lagfx_loop_plan_t plan = {0};
    lagfx_bb_t bbs[8];
    uint32_t nbb = partition_basic_blocks(c, bbs, 8u);
    if (nbb != 3u) return plan;   /* only the 3-block rotated loop today */

    /* Block 0 (entry) must end in an UNCONDITIONAL BR to some block H. */
    const lagfx_air_inst_t *t0 = &c->insts[bbs[0].last_inst];
    if (t0->code != LAGFX_AIR_INST_BR || t0->num_ops != 1u) return plan;
    uint32_t header = (uint32_t)t0->ops[0];
    if (header >= nbb) return plan;

    /* The header block must START with a PHI and END with a CONDITIONAL
     * BR whose two targets are {header (back-edge), other (exit)}. */
    const lagfx_air_inst_t *hfirst = &c->insts[bbs[header].first_inst];
    if (hfirst->code != LAGFX_AIR_INST_PHI) return plan;
    const lagfx_air_inst_t *th = &c->insts[bbs[header].last_inst];
    if (th->code != LAGFX_AIR_INST_BR || th->num_ops != 3u) return plan;
    /* Conditional BR operand layout: [true_bb, false_bb, cond_rel]. */
    uint32_t t_true  = (uint32_t)th->ops[0];
    uint32_t t_false = (uint32_t)th->ops[1];
    uint32_t exit_bb;
    bool cond_true_is_exit;
    if (t_false == header && t_true != header) {
        exit_bb = t_true;  cond_true_is_exit = true;
    } else if (t_true == header && t_false != header) {
        exit_bb = t_false; cond_true_is_exit = false;
    } else {
        return plan;       /* not a self-loop */
    }
    if (exit_bb >= nbb) return plan;

    /* The exit block must END in a RET (the after-loop merge). */
    const lagfx_air_inst_t *te = &c->insts[bbs[exit_bb].last_inst];
    if (te->code != LAGFX_AIR_INST_RET) return plan;

    /* Entry must be block 0 (the other two are header + merge). */
    if (header == 0u || exit_bb == 0u) return plan;

    plan.valid = true;
    plan.entry_bb = 0u;
    plan.header_bb = header;
    plan.merge_bb = exit_bb;
    plan.cond_true_is_exit = cond_true_is_exit;
    return plan;
}

/* Pre-allocate a SPIR-V result id for every value-producing body
 * instruction and bind it in the value map, so that emit_inst_* picks up
 * the pre-bound id (via resolve_value_spv) instead of allocating — this
 * makes FORWARD references resolvable, which a loop's PHI requires (its
 * incoming values are defined later in the same header block). Also
 * records each instruction's result AIR type where statically known
 * (PHI/CALL carry it explicitly; others are filled by their emitters). */
static void preallocate_result_ids(xlate_ctx_t *c) {
    uint32_t next_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (!inst_produces_value(c, inst)) continue;
        if (!resolve_value_spv(c, next_val_id)) {
            uint32_t id = lagfx_spv_builder_alloc_id(c->b);
            bind_value_spv(c, next_val_id, id);
        }
        /* PHI carries its result type as op[0] (an AIR type index). */
        if (inst->code == LAGFX_AIR_INST_PHI && inst->num_ops >= 1u)
            set_result_air_type(c, next_val_id, (uint32_t)inst->ops[0]);
        else if (inst->code == LAGFX_AIR_INST_CALL) {
            uint32_t rt = call_return_air_type(c, inst);
            if (rt != LAGFX_AIR_TYPE_NONE) set_result_air_type(c, next_val_id, rt);
        }
        next_val_id++;
    }
}

/* Emit the non-PHI, non-terminator instructions of a basic block using the
 * existing per-instruction emitters, advancing the value-number counter
 * exactly as the linear path does. *io_val_id is the value-id of the FIRST
 * instruction in [first,last) (caller seeds + reads back). PHI and the
 * terminator are handled by the loop driver, so they are skipped here but
 * still advance the counter when they produce a value (PHI does). */
static void emit_block_body(xlate_ctx_t *c, uint32_t first, uint32_t last_excl,
                            uint32_t *io_val_id) {
    uint32_t next_val_id = *io_val_id;
    for (uint32_t i = first; i < last_excl; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        bool produces = inst_produces_value(c, inst);
        uint32_t result_value_id = produces ? next_val_id : 0u;
        switch (inst->code) {
            case LAGFX_AIR_INST_ALLOCA:    emit_inst_alloca(c, i, inst, result_value_id); break;
            case LAGFX_AIR_INST_CAST:      emit_inst_cast(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_CALL:      emit_inst_call(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_GEP:
            case LAGFX_AIR_INST_GEP_OLD:   emit_inst_gep(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_STORE:
            case LAGFX_AIR_INST_STORE_OLD: emit_inst_store(c, inst, next_val_id); break;
            case LAGFX_AIR_INST_LOAD:      emit_inst_load(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_SHUFFLEVEC:emit_inst_shufflevec(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_INSERTVAL: emit_inst_insertval(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_BINOP:     emit_inst_binop(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_UNOP:      emit_inst_unop(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_CMP:       emit_inst_cmp(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_CMP2:      emit_inst_cmp2(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_SELECT:
            case LAGFX_AIR_INST_VSELECT:   emit_inst_select(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_EXTRACTVAL:emit_inst_extractval(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_EXTRACTELT:emit_inst_extractelt(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_INSERTELT: emit_inst_insertelt(c, i, inst, result_value_id, next_val_id); break;
            case LAGFX_AIR_INST_DECLAREBLOCKS: break;
            case LAGFX_AIR_INST_PHI:       break; /* handled by the driver */
            default:
                LAGFX_TRACE("translate_function(loop): dropping unhandled "
                            "AIR opcode raw=%u (code=%d) at i[%u]",
                            inst->raw_code, (int)inst->code, i);
                break;
        }
        if (produces) next_val_id++;
    }
    *io_val_id = next_val_id;
}

/* The value-id of the instruction at body index `inst_idx` (the value-id a
 * producing instruction at that index would take). Counts value-producers
 * up to (not including) inst_idx. */
static uint32_t value_id_at_inst(const xlate_ctx_t *c, uint32_t inst_idx) {
    uint32_t v = c->inst_id_base;
    for (uint32_t i = 0; i < inst_idx && i < c->num_insts; i++)
        if (inst_produces_value(c, &c->insts[i])) v++;
    return v;
}

/* Emit the header block's PHI nodes as OpPhi. `entry_label` is the SPIR-V
 * label of the predecessor that flows in from the loop's entry edge;
 * `continue_label` is the predecessor for the self back-edge. The LLVM PHI
 * incoming pairs are (value_sign_rotated, bb_index); we map bb_index ==
 * header_bb → continue_label, else → entry_label. */
static void emit_header_phis(xlate_ctx_t *c, const lagfx_loop_plan_t *plan,
                             const lagfx_bb_t *bbs,
                             uint32_t entry_label, uint32_t continue_label) {
    uint32_t hfirst = bbs[plan->header_bb].first_inst;
    for (uint32_t i = hfirst; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst->code != LAGFX_AIR_INST_PHI) break;   /* phis are contiguous at block start */
        if (inst->num_ops < 3u) continue;

        uint32_t phi_valno = value_id_at_inst(c, i);
        uint32_t result_spv = resolve_value_spv(c, phi_valno);
        if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);

        uint32_t ty_air = (uint32_t)inst->ops[0];
        uint32_t result_ty = emit_air_type(c, ty_air);

        /* OpPhi: [result_type, result, (val, parent_label)...]. */
        uint32_t ops[2 + 2 * 8];
        uint32_t n = 0;
        ops[n++] = result_ty;
        ops[n++] = result_spv;
        uint32_t npairs = (inst->num_ops - 1u) / 2u;
        if (npairs > 8u) npairs = 8u;
        for (uint32_t p = 0; p < npairs; p++) {
            uint64_t v_enc = inst->ops[1u + 2u * p];
            uint32_t bb    = (uint32_t)inst->ops[2u + 2u * p];
            int64_t delta  = decode_sign_rotated(v_enc);
            uint32_t inval_id = (uint32_t)((int64_t)phi_valno - delta);
            uint32_t inval_spv = resolve_value_spv(c, inval_id);
            if (!inval_spv) inval_spv = emit_undef(c, result_ty);
            uint32_t parent = (bb == plan->header_bb) ? continue_label : entry_label;
            ops[n++] = inval_spv;
            ops[n++] = parent;
        }
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_PHI, ops, n);

        bind_value_spv(c, phi_valno, result_spv);
        set_result_air_type(c, phi_valno, ty_air);
    }
}

/* Drive emission for the recognised single-loop shape. Emits, in SPIR-V
 * structured order:
 *   entry  (already open): non-phi entry insts ; OpBranch %header
 *   header: OpPhi… ; OpLoopMerge %merge %continue ; OpBranch %body
 *   body:   header block's non-phi insts (up to the latch test) ;
 *           OpBranchConditional %cond %merge %continue
 *   continue: OpBranch %header
 *   merge:  the after-loop block's insts ; (RET → OpReturn)
 */
static lagfx_status_t translate_body_loop(xlate_ctx_t *c,
                                          const lagfx_loop_plan_t *plan) {
    lagfx_bb_t bbs[8];
    uint32_t nbb = partition_basic_blocks(c, bbs, 8u);
    if (nbb != 3u) return LAGFX_ERR_PROTOCOL;

    /* Allocate result ids for all producers up-front (forward refs). */
    preallocate_result_ids(c);

    uint32_t id_header   = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_body     = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_continue = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_merge    = lagfx_spv_builder_alloc_id(c->b);

    /* --- entry block (already open via emit_module_vars_and_function) --- */
    {
        uint32_t v = c->inst_id_base;
        emit_block_body(c, bbs[plan->entry_bb].first_inst,
                        bbs[plan->entry_bb].last_inst, &v);
    }
    { uint32_t ops[] = { id_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- header: phis + OpLoopMerge + branch to body --- */
    { uint32_t ops[] = { id_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    emit_header_phis(c, plan, bbs, c->id_entry_label, id_continue);
    { uint32_t ops[] = { id_merge, id_continue, LAGFX_SPV_LOOP_CONTROL_NONE };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOOP_MERGE, ops, 3); }
    { uint32_t ops[] = { id_body };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- body: header block's non-phi computations + the latch BR --- */
    { uint32_t ops[] = { id_body };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    {
        /* Skip the leading PHIs (emitted in the header). */
        uint32_t hfirst = bbs[plan->header_bb].first_inst;
        uint32_t body_first = hfirst;
        while (body_first <= bbs[plan->header_bb].last_inst &&
               c->insts[body_first].code == LAGFX_AIR_INST_PHI)
            body_first++;
        uint32_t v = value_id_at_inst(c, body_first);
        emit_block_body(c, body_first, bbs[plan->header_bb].last_inst, &v);
    }
    /* Conditional latch BR → merge / continue. */
    {
        const lagfx_air_inst_t *th = &c->insts[bbs[plan->header_bb].last_inst];
        uint32_t cond_valno = value_id_at_inst(c, bbs[plan->header_bb].last_inst);
        uint32_t cond_id = resolve_relative((uint32_t)th->ops[2], cond_valno);
        uint32_t cond_spv = resolve_or_undef(c, cond_id, emit_type_bool(c));
        /* OpBranchConditional %cond <true-target> <false-target>. The LLVM
         * targets are {merge (exit), header (back-edge→continue)}. */
        uint32_t tgt_true  = plan->cond_true_is_exit ? id_merge : id_continue;
        uint32_t tgt_false = plan->cond_true_is_exit ? id_continue : id_merge;
        uint32_t ops[] = { cond_spv, tgt_true, tgt_false };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, ops, 3);
    }

    /* --- continue block: branch back to header --- */
    { uint32_t ops[] = { id_continue };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    { uint32_t ops[] = { id_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- merge block: the after-loop instructions + RET --- */
    { uint32_t ops[] = { id_merge };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    {
        uint32_t mfirst = bbs[plan->merge_bb].first_inst;
        uint32_t mlast  = bbs[plan->merge_bb].last_inst;   /* the RET */
        uint32_t v = value_id_at_inst(c, mfirst);
        emit_block_body(c, mfirst, mlast, &v);
        /* Emit the RET (→ OpReturn / colour store). */
        const lagfx_air_inst_t *ret = &c->insts[mlast];
        uint32_t ret_valno = value_id_at_inst(c, mlast);
        emit_inst_ret(c, ret, ret_valno);
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return LAGFX_OK;
}

/* ===================================================================
 * Guarded rotated loop (the 4-block shape `xcrun metal` emits for a
 * `while (cond) {...}` whose first iteration can be skipped, or a `for`
 * with a runtime trip-count guard). Distinct from the plain rotated
 * loop above in two ways:
 *
 *   1. ENTRY GUARD — block 0 ends in a CONDITIONAL br that either enters
 *      the loop header or jumps straight to the after-loop merge.
 *   2. LOOP-CLOSING PHIs IN THE MERGE — the after-loop block starts with
 *      PHI nodes whose predecessors are {entry guard-skip edge, loop-exit
 *      edge from the header/latch}, selecting the right value depending
 *      on whether the loop ran at all.
 *
 * Canonical shape (exactly 3 partitioned blocks; the entry guard fuses
 * the would-be pre-header into the entry's conditional branch):
 *
 *   entry:  ...; br i1 %c, label %H, label %M     (conditional guard)
 *   H:      phi…; …; br i1 %l, label %H, label %M (self back-edge + exit)
 *   M:      phi…; …; ret                          (loop-closing phis)
 *
 * SPIR-V demands one merge instruction per merge block, so the single
 * LLVM merge block %M is split into a loop-merge landing pad and the
 * selection merge:
 *
 *   entry:     OpSelectionMerge %merge; OpBranchConditional %c %H %merge
 *   H:         OpPhi…; OpLoopMerge %loop_exit %continue; OpBranch %body
 *   body:      <H non-phi insts>; OpBranchConditional %l %continue %loop_exit
 *   continue:  OpBranch %H
 *   loop_exit: OpBranch %merge      (loop's structured merge target)
 *   merge:     OpPhi(entry, loop_exit)…; <M non-phi insts>; OpReturn
 *
 * The merge OpPhi predecessors remap: the LLVM `entry`(%2) incoming →
 * %merge's entry predecessor (the guard-skip edge), and the LLVM
 * `header`(%H) incoming → %loop_exit (the structured loop-exit edge).
 *
 * Any body that does not match this exact shape (e.g. a loop with a real
 * separate pre-header block, an if/else diamond, or a single block) falls
 * through to the existing paths unchanged.
 * =================================================================== */

typedef struct {
    bool     valid;
    uint32_t entry_bb;          /* always 0 — the guard */
    uint32_t header_bb;         /* loop block (phis + latch + self back-edge) */
    uint32_t merge_bb;          /* after-loop block (loop-closing phis + RET) */
    bool     guard_true_is_header; /* entry BR cond ? header : merge  (vs swapped) */
    bool     latch_true_is_header; /* header BR cond ? header(back) : merge(exit) */
} lagfx_guarded_loop_plan_t;

static lagfx_guarded_loop_plan_t detect_guarded_loop(const xlate_ctx_t *c) {
    lagfx_guarded_loop_plan_t plan = {0};
    lagfx_bb_t bbs[8];
    uint32_t nbb = partition_basic_blocks(c, bbs, 8u);
    if (nbb != 3u) return plan;   /* only the fused-preheader 3-block shape */

    /* Block 0 (entry) must end in a CONDITIONAL BR to {header, merge}. */
    const lagfx_air_inst_t *t0 = &c->insts[bbs[0].last_inst];
    if (t0->code != LAGFX_AIR_INST_BR || t0->num_ops != 3u) return plan;
    uint32_t g_true  = (uint32_t)t0->ops[0];
    uint32_t g_false = (uint32_t)t0->ops[1];
    if (g_true >= nbb || g_false >= nbb) return plan;
    if (g_true == 0u || g_false == 0u) return plan;       /* neither targets entry */
    if (g_true == g_false) return plan;

    /* The HEADER is whichever guard target starts with a PHI and ends in a
     * self-looping conditional BR; the MERGE is the other target. Try both
     * assignments (the guard may enter header on the true OR false edge). */
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t header = (attempt == 0) ? g_true  : g_false;
        uint32_t merge  = (attempt == 0) ? g_false : g_true;
        bool guard_true_is_header = (attempt == 0);

        /* Header starts with a PHI. */
        const lagfx_air_inst_t *hfirst = &c->insts[bbs[header].first_inst];
        if (hfirst->code != LAGFX_AIR_INST_PHI) continue;

        /* Header ends in a CONDITIONAL BR whose targets are {header
         * (back-edge), merge (exit)}. */
        const lagfx_air_inst_t *th = &c->insts[bbs[header].last_inst];
        if (th->code != LAGFX_AIR_INST_BR || th->num_ops != 3u) continue;
        uint32_t l_true  = (uint32_t)th->ops[0];
        uint32_t l_false = (uint32_t)th->ops[1];
        bool latch_true_is_header;
        if (l_true == header && l_false == merge)      latch_true_is_header = true;
        else if (l_false == header && l_true == merge) latch_true_is_header = false;
        else continue;       /* not a self-loop exiting to merge */

        /* The merge block must END in a RET. */
        const lagfx_air_inst_t *tm = &c->insts[bbs[merge].last_inst];
        if (tm->code != LAGFX_AIR_INST_RET) continue;

        /* The merge block must START with a loop-closing PHI — that is
         * the distinguishing signal versus the plain rotated loop (whose
         * merge has no phis). Be conservative: require it. */
        const lagfx_air_inst_t *mfirst = &c->insts[bbs[merge].first_inst];
        if (mfirst->code != LAGFX_AIR_INST_PHI) continue;

        plan.valid = true;
        plan.entry_bb = 0u;
        plan.header_bb = header;
        plan.merge_bb = merge;
        plan.guard_true_is_header = guard_true_is_header;
        plan.latch_true_is_header = latch_true_is_header;
        return plan;
    }
    return plan;
}

/* Emit the loop-closing PHIs at the start of the merge block as OpPhi.
 * The LLVM incoming pairs are (value_sign_rotated, bb_index); we map
 * bb_index == entry_bb → entry_pred_label (the guard-skip edge) and
 * bb_index == header_bb → loop_exit_label (the structured loop-exit
 * landing pad). */
static void emit_merge_phis(xlate_ctx_t *c,
                            const lagfx_guarded_loop_plan_t *plan,
                            const lagfx_bb_t *bbs,
                            uint32_t entry_pred_label,
                            uint32_t loop_exit_label) {
    uint32_t mfirst = bbs[plan->merge_bb].first_inst;
    for (uint32_t i = mfirst; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst->code != LAGFX_AIR_INST_PHI) break;   /* phis contiguous at block start */
        if (inst->num_ops < 3u) continue;

        uint32_t phi_valno = value_id_at_inst(c, i);
        uint32_t result_spv = resolve_value_spv(c, phi_valno);
        if (!result_spv) result_spv = lagfx_spv_builder_alloc_id(c->b);

        uint32_t ty_air = (uint32_t)inst->ops[0];
        uint32_t result_ty = emit_air_type(c, ty_air);

        uint32_t ops[2 + 2 * 8];
        uint32_t n = 0;
        ops[n++] = result_ty;
        ops[n++] = result_spv;
        uint32_t npairs = (inst->num_ops - 1u) / 2u;
        if (npairs > 8u) npairs = 8u;
        for (uint32_t p = 0; p < npairs; p++) {
            uint64_t v_enc = inst->ops[1u + 2u * p];
            uint32_t bb    = (uint32_t)inst->ops[2u + 2u * p];
            int64_t delta  = decode_sign_rotated(v_enc);
            uint32_t inval_id = (uint32_t)((int64_t)phi_valno - delta);
            uint32_t inval_spv = resolve_value_spv(c, inval_id);
            if (!inval_spv) inval_spv = emit_undef(c, result_ty);
            uint32_t parent = (bb == plan->header_bb) ? loop_exit_label
                                                      : entry_pred_label;
            ops[n++] = inval_spv;
            ops[n++] = parent;
        }
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_PHI, ops, n);

        bind_value_spv(c, phi_valno, result_spv);
        set_result_air_type(c, phi_valno, ty_air);
    }
}

/* Drive emission for the recognised guarded-loop shape (see the big
 * comment above for the SPIR-V block layout). */
static lagfx_status_t translate_body_guarded_loop(
        xlate_ctx_t *c, const lagfx_guarded_loop_plan_t *plan) {
    lagfx_bb_t bbs[8];
    uint32_t nbb = partition_basic_blocks(c, bbs, 8u);
    if (nbb != 3u) return LAGFX_ERR_PROTOCOL;

    preallocate_result_ids(c);

    uint32_t id_header    = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_body      = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_continue  = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_loop_exit = lagfx_spv_builder_alloc_id(c->b);
    uint32_t id_merge     = lagfx_spv_builder_alloc_id(c->b);

    /* --- entry block (already open): guard insts + selection guard --- */
    {
        uint32_t v = c->inst_id_base;
        emit_block_body(c, bbs[plan->entry_bb].first_inst,
                        bbs[plan->entry_bb].last_inst, &v);
    }
    /* The guard's condition is the entry block's terminator's cond operand
     * (ops[2], relative to the value-id AT the terminator). */
    {
        const lagfx_air_inst_t *t0 = &c->insts[bbs[plan->entry_bb].last_inst];
        uint32_t cond_valno = value_id_at_inst(c, bbs[plan->entry_bb].last_inst);
        uint32_t cond_id = resolve_relative((uint32_t)t0->ops[2], cond_valno);
        uint32_t cond_spv = resolve_or_undef(c, cond_id, emit_type_bool(c));
        { uint32_t ops[] = { id_merge, LAGFX_SPV_SELECTION_CONTROL_NONE };
          lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECTION_MERGE, ops, 2); }
        uint32_t tgt_true  = plan->guard_true_is_header ? id_header : id_merge;
        uint32_t tgt_false = plan->guard_true_is_header ? id_merge  : id_header;
        uint32_t ops[] = { cond_spv, tgt_true, tgt_false };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, ops, 3);
    }

    /* --- header: phis + OpLoopMerge + branch to body --- */
    { uint32_t ops[] = { id_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    {
        /* Reuse the rotated-loop phi emitter: it maps incoming bb ==
         * header_bb → continue_label and anything else → entry_label.
         * The header's entry-edge predecessor is the entry block. */
        lagfx_loop_plan_t lp = {0};
        lp.entry_bb = plan->entry_bb;
        lp.header_bb = plan->header_bb;
        lp.merge_bb = plan->merge_bb;
        emit_header_phis(c, &lp, bbs, c->id_entry_label, id_continue);
    }
    { uint32_t ops[] = { id_loop_exit, id_continue, LAGFX_SPV_LOOP_CONTROL_NONE };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOOP_MERGE, ops, 3); }
    { uint32_t ops[] = { id_body };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- body: header block's non-phi computations + the latch BR --- */
    { uint32_t ops[] = { id_body };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    {
        uint32_t hfirst = bbs[plan->header_bb].first_inst;
        uint32_t body_first = hfirst;
        while (body_first <= bbs[plan->header_bb].last_inst &&
               c->insts[body_first].code == LAGFX_AIR_INST_PHI)
            body_first++;
        uint32_t v = value_id_at_inst(c, body_first);
        emit_block_body(c, body_first, bbs[plan->header_bb].last_inst, &v);
    }
    {
        const lagfx_air_inst_t *th = &c->insts[bbs[plan->header_bb].last_inst];
        uint32_t cond_valno = value_id_at_inst(c, bbs[plan->header_bb].last_inst);
        uint32_t cond_id = resolve_relative((uint32_t)th->ops[2], cond_valno);
        uint32_t cond_spv = resolve_or_undef(c, cond_id, emit_type_bool(c));
        /* OpBranchConditional %cond <back-edge=continue> <exit=loop_exit>. */
        uint32_t tgt_true  = plan->latch_true_is_header ? id_continue : id_loop_exit;
        uint32_t tgt_false = plan->latch_true_is_header ? id_loop_exit : id_continue;
        uint32_t ops[] = { cond_spv, tgt_true, tgt_false };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, ops, 3);
    }

    /* --- continue: branch back to header --- */
    { uint32_t ops[] = { id_continue };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    { uint32_t ops[] = { id_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- loop_exit: the loop's structured merge landing pad → merge --- */
    { uint32_t ops[] = { id_loop_exit };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    { uint32_t ops[] = { id_merge };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, ops, 1); }

    /* --- merge: loop-closing phis + after-loop insts + RET --- */
    { uint32_t ops[] = { id_merge };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, ops, 1); }
    emit_merge_phis(c, plan, bbs, c->id_entry_label, id_loop_exit);
    {
        uint32_t mfirst = bbs[plan->merge_bb].first_inst;
        uint32_t mlast  = bbs[plan->merge_bb].last_inst;   /* the RET */
        /* Skip the leading loop-closing PHIs (emitted above). */
        uint32_t after_phis = mfirst;
        while (after_phis < mlast &&
               c->insts[after_phis].code == LAGFX_AIR_INST_PHI)
            after_phis++;
        uint32_t v = value_id_at_inst(c, after_phis);
        emit_block_body(c, after_phis, mlast, &v);
        const lagfx_air_inst_t *ret = &c->insts[mlast];
        uint32_t ret_valno = value_id_at_inst(c, mlast);
        emit_inst_ret(c, ret, ret_valno);
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return LAGFX_OK;
}

/* ===================================================================
 * Driver
 * =================================================================== */


/* ===================================================================
 * Block-dispatch control flow (GOAL-M2aa) — the general-CFG fallback.
 *
 * Any multi-block body that the recognised single/guarded loop plans do
 * not match is emitted as ONE structured loop:
 *
 *   entry:    OpVariables (allocas + spills + phi vars + %blockvar,
 *             spliced) ; prologue ; bb0 body ; store initial target ;
 *             OpBranch %header
 *   header:   %cur = OpLoad %blockvar ; OpLoopMerge %exit %cont ;
 *             OpBranch %body
 *   body:     OpSelectionMerge %selmerge ; OpSwitch %cur %selmerge
 *             [i -> %bb_i]...
 *   bb_i:     phi loads ; body insts ; terminator -> phi-incoming stores,
 *             store next block id, OpBranch %selmerge (RET -> OpReturn)
 *   selmerge: OpBranch %cont      cont: OpBranch %header
 *   exit:     OpUnreachable       (returns happen inside the cases)
 *
 * LLVM bb0 (the entry block) is emitted BEFORE the loop: it dominates
 * every case, so its values need no spilling. Values defined in cases are
 * spilled to Function variables at the def site (type known then; the
 * builder's locals-splice places the OpVariables in the entry block) and
 * loaded at cross-case uses via the resolve_value_spv hook. PHIs become
 * variables: every predecessor edge stores its incoming value before
 * branching (unconditionally for both targets of a conditional branch —
 * safe: the taken edge's store is always the LAST one executed before the
 * successor's phi load). Kill-switch: LAGFX_DISABLE_BLOCKSWITCH.
 * =================================================================== */

/* Function-storage pointer type for a spill of value type `ty_air`
 * (AIR id or the BOOL sentinel) with SPIR-V type `ty_spv`. Uses the
 * ptr_cache via emit_type_pointer so alloca-created pointer types are
 * shared (duplicate OpTypePointer = spirv-val reject). */
static uint32_t dispatch_ptr_type(xlate_ctx_t *c, uint32_t ty_air,
                                  uint32_t ty_spv) {
    return emit_type_pointer(c, ty_air, ty_spv, LAGFX_SPV_STORAGE_FUNCTION);
}

/* Function-pointer type keyed on the pointee's SPIR-V type id (for values
 * whose AIR type is unknown/lossy — see dispatch_ptr_cache). */
static uint32_t dispatch_ptr_type_spv(xlate_ctx_t *c, uint32_t ty_spv) {
    for (uint32_t i = 0; i < c->n_dispatch_ptr_cache; i++)
        if (c->dispatch_ptr_cache[i].val_ty == ty_spv)
            return c->dispatch_ptr_cache[i].ptr_ty;
    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
    uint32_t o[] = { id, LAGFX_SPV_STORAGE_FUNCTION, ty_spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_POINTER, o, 3);
    if (c->n_dispatch_ptr_cache < 8u) {
        c->dispatch_ptr_cache[c->n_dispatch_ptr_cache].val_ty = ty_spv;
        c->dispatch_ptr_cache[c->n_dispatch_ptr_cache].ptr_ty = id;
        c->n_dispatch_ptr_cache++;
    }
    return id;
}

/* Lazily create the Function variable backing a PHI result. */
static uint32_t dispatch_phi_var(xlate_ctx_t *c, uint32_t phi_value_id,
                                 uint32_t ty_air, uint32_t ty_spv) {
    if (!c->phi_var || phi_value_id >= c->value_id_capacity) return 0u;
    if (c->phi_var[phi_value_id]) return c->phi_var[phi_value_id];
    uint32_t ptrty = dispatch_ptr_type(c, ty_air, ty_spv);
    uint32_t var = lagfx_spv_builder_alloc_id(c->b);
    if (!lagfx_spv_builder_emit_local_var(c->b, ptrty, var)) return 0u;
    c->phi_var[phi_value_id] = var;
    if (c->phi_type) c->phi_type[phi_value_id] = ty_spv;
    return var;
}

/* Spill a just-defined case-block value to a Function variable so other
 * cases can load it. Pointer-typed / unknown-typed values are skipped
 * (their cross-case uses degrade to typed undefs, same as before). */
static void dispatch_spill_result(xlate_ctx_t *c, uint32_t value_id) {
    if (!c->spill_var || value_id >= c->value_id_capacity) return;
    uint32_t spv = c->value_id_to_spv[value_id];
    if (!spv) return;
    if (c->spill_var[value_id]) {
        /* Pre-created by the inference pre-pass (back-edge support): emit
         * the def-site store — guarding the type (an OpStore mismatch is
         * a spirv-val reject; a skipped store just leaves the var undef,
         * the standing degradation contract). */
        uint32_t ty_air2 = value_air_type_idx(c, value_id);
        uint32_t have = get_value_spv_type(c, value_id);
        if (!have && ty_air2 != LAGFX_AIR_TYPE_NONE &&
            ty_air2 != LAGFX_AIR_TYPE_BOOL)
            have = emit_air_type(c, ty_air2);
        if (have && have == c->spill_type[value_id]) {
            uint32_t so[] = { c->spill_var[value_id], spv };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
        }
        return;
    }
    if (get_value_storage(c, value_id) != UINT32_MAX) return; /* pointer */
    uint32_t ty_air = value_air_type_idx(c, value_id);
    uint32_t precise = get_value_spv_type(c, value_id);
    uint32_t ty_spv = 0;
    bool spv_keyed = false;
    if (ty_air == LAGFX_AIR_TYPE_BOOL) {
        /* The BOOL sentinel is lossy (a vector compare records it too);
         * the SPIR-V-type side table carries the precise type. */
        ty_spv = precise ? precise : emit_type_bool(c);
        spv_keyed = true;
    } else if (ty_air != LAGFX_AIR_TYPE_NONE) {
        uint32_t n_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
        if (ty_air < n_types &&
            (ts[ty_air].kind == LAGFX_AIR_TYPE_POINTER ||
             ts[ty_air].kind == LAGFX_AIR_TYPE_VOID))
            return;
        ty_spv = emit_air_type(c, ty_air);
    } else if (precise) {
        ty_spv = precise;
        spv_keyed = true;
    } else {
        return;   /* unknown type — cross-case uses degrade to undef */
    }
    if (!ty_spv) return;
    uint32_t ptrty = spv_keyed ? dispatch_ptr_type_spv(c, ty_spv)
                               : dispatch_ptr_type(c, ty_air, ty_spv);
    uint32_t var = lagfx_spv_builder_alloc_id(c->b);
    if (!lagfx_spv_builder_emit_local_var(c->b, ptrty, var)) return;
    uint32_t so[] = { var, spv };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
    c->spill_var[value_id]  = var;
    c->spill_type[value_id] = ty_spv;
}

/* Store the PHI-incoming values for edge cur_bb -> succ. */
static void dispatch_store_phi_incomings(xlate_ctx_t *c, const lagfx_bb_t *bbs,
                                         uint32_t nbb, uint32_t cur_bb,
                                         uint32_t succ) {
    if (succ >= nbb) return;
    for (uint32_t i = bbs[succ].first_inst; i <= bbs[succ].last_inst &&
             i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst->code != LAGFX_AIR_INST_PHI) break; /* phis lead the block */
        if (inst->num_ops < 3u) continue;
        uint32_t phi_valno = value_id_at_inst(c, i);
        uint32_t ty_air = (uint32_t)inst->ops[0];
        uint32_t ty_spv = emit_air_type(c, ty_air);
        uint32_t var = dispatch_phi_var(c, phi_valno, ty_air, ty_spv);
        if (!var) continue;
        uint32_t npairs = (inst->num_ops - 1u) / 2u;
        for (uint32_t pr = 0; pr < npairs; pr++) {
            uint32_t bb = (uint32_t)inst->ops[2u + 2u * pr];
            if (bb != cur_bb) continue;
            int64_t delta = decode_sign_rotated(inst->ops[1u + 2u * pr]);
            uint32_t inval_id = (uint32_t)((int64_t)phi_valno - delta);
            uint32_t inval = resolve_value_spv(c, inval_id);
            /* The incoming value may itself be a PHI (chained phi networks
             * through pass-through blocks): it has no in-case SSA binding —
             * its live value sits in ITS dispatch phi variable, stored by
             * whichever predecessor executed. Load it. Without this the
             * undef fallback CLOBBERED the receiving phi var with NaN on
             * every pass-through edge — the Xgc login material's final
             * color flowed through exactly such a block chain and emitted
             * NaN at 100% of pixels (offline lavapipe replay, 2026-07-23:
             * F16RT center [7e00 x4]). */
            if (!inval && c->phi_var && inval_id < c->value_id_capacity &&
                c->phi_var[inval_id]) {
                uint32_t lty = (c->phi_type && c->phi_type[inval_id])
                                   ? c->phi_type[inval_id] : ty_spv;
                if (lty == ty_spv) {
                    uint32_t lid = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t lo[] = { lty, lid, c->phi_var[inval_id] };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, lo, 3);
                    inval = lid;
                }
            }
            /* Pass-through edge whose incoming could not be resolved at
             * all: SKIP the store rather than clobber the phi variable —
             * a dominating def-site store already holds the best-known
             * value; overwriting with undef is strictly worse. */
            if (!inval) break;
            uint32_t so[] = { var, inval };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
            break;
        }
    }
}

/* Lower a block terminator: phi-incoming stores for every successor edge,
 * store the next block index into %blockvar, branch to `jump_label`
 * (%header from bb0, %selmerge from a case). RET/UNREACHABLE terminate
 * directly. */
static void dispatch_lower_terminator(xlate_ctx_t *c, const lagfx_bb_t *bbs,
                                      uint32_t nbb, uint32_t cur_bb,
                                      const lagfx_air_inst_t *inst,
                                      uint32_t next_val_id,
                                      uint32_t blockvar, uint32_t jump_label) {
    uint32_t uint_spv = emit_type_int_w(c, 32u, 0u);
    switch (inst->code) {
        case LAGFX_AIR_INST_RET:
            emit_inst_ret(c, inst, next_val_id);   /* stores outputs + OpReturn */
            return;
        case LAGFX_AIR_INST_UNREACHABLE:
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_UNREACHABLE, NULL, 0);
            return;
        case LAGFX_AIR_INST_BR:
            if (inst->num_ops == 1u) {
                uint32_t t = (uint32_t)inst->ops[0];
                dispatch_store_phi_incomings(c, bbs, nbb, cur_bb, t);
                uint32_t so[] = { blockvar, emit_const_uint32(c, t) };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
            } else if (inst->num_ops >= 3u) {
                uint32_t t = (uint32_t)inst->ops[0];
                uint32_t f = (uint32_t)inst->ops[1];
                dispatch_store_phi_incomings(c, bbs, nbb, cur_bb, t);
                if (f != t) dispatch_store_phi_incomings(c, bbs, nbb, cur_bb, f);
                uint32_t cond_id = resolve_relative((uint32_t)inst->ops[2],
                                                    next_val_id);
                if (!resolve_value_spv(c, cond_id)) {
                    uint32_t di2 = (c->value_def_inst &&
                                    cond_id < c->value_id_capacity)
                                       ? c->value_def_inst[cond_id] : ~0u;
                    LAGFX_TRACE("dispatch: UNDEF cond vid=%u def_inst=%u "
                                "code=%d def_bb=%u cur_bb=%u", cond_id, di2,
                                di2 != ~0u ? (int)c->insts[di2].code : -1,
                                (c->value_def_bb && cond_id < c->value_id_capacity)
                                    ? c->value_def_bb[cond_id] : 0xFFFF,
                                cur_bb);
                }
                uint32_t cond = resolve_or_undef(c, cond_id, emit_type_bool(c));
                uint32_t sel = lagfx_spv_builder_alloc_id(c->b);
                uint32_t selo[] = { uint_spv, sel, cond,
                                    emit_const_uint32(c, t),
                                    emit_const_uint32(c, f) };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECT, selo, 5);
                uint32_t so[] = { blockvar, sel };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
            }
            break;
        case LAGFX_AIR_INST_SWITCH: {
            /* [opty, cond_rel, default_bb, (caseval_ABS, casebb)...] */
            if (inst->num_ops < 3u) break;
            uint32_t defbb = (uint32_t)inst->ops[2];
            dispatch_store_phi_incomings(c, bbs, nbb, cur_bb, defbb);
            uint32_t opty = (uint32_t)inst->ops[0];
            uint32_t cond_id = resolve_relative((uint32_t)inst->ops[1],
                                                next_val_id);
            uint32_t cond_ty = emit_air_type(c, opty);
            uint32_t cond = resolve_or_undef(c, cond_id, cond_ty);
            uint32_t next = emit_const_uint32(c, defbb);
            uint32_t ncase = (inst->num_ops - 3u) / 2u;
            for (uint32_t k = 0; k < ncase; k++) {
                uint32_t cval_id = (uint32_t)inst->ops[3u + 2u * k];
                uint32_t cbb     = (uint32_t)inst->ops[4u + 2u * k];
                if (cbb != defbb)
                    dispatch_store_phi_incomings(c, bbs, nbb, cur_bb, cbb);
                uint32_t cval = resolve_value_spv(c, cval_id);
                if (!cval) continue;   /* unresolvable case value: falls to default */
                uint32_t eq = lagfx_spv_builder_alloc_id(c->b);
                uint32_t eqo[] = { emit_type_bool(c), eq, cond, cval };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_IEQUAL, eqo, 4);
                uint32_t sel = lagfx_spv_builder_alloc_id(c->b);
                uint32_t selo[] = { uint_spv, sel, eq,
                                    emit_const_uint32(c, cbb), next };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECT, selo, 5);
                next = sel;
            }
            uint32_t so[] = { blockvar, next };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2);
            break;
        }
        default:
            break;
    }
    uint32_t bo[] = { jump_label };
    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, bo, 1);
}

/* Emit one basic block's leading PHIs (as variable loads) and body
 * instructions through the standard per-inst emitters, spilling each
 * produced value (cases only — bb0 dominates and needs no spills).
 * *io_val_id advances exactly as the linear path's counter. Stops BEFORE
 * the terminator. */
static void dispatch_emit_block(xlate_ctx_t *c, const lagfx_bb_t *bbs,
                                uint32_t bb, uint32_t *io_val_id) {
    uint32_t next_val_id = *io_val_id;
    for (uint32_t i = bbs[bb].first_inst; i < bbs[bb].last_inst &&
             i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        bool produces = inst_produces_value(c, inst);
        uint32_t result_value_id = produces ? next_val_id : 0u;
        if (inst->code == LAGFX_AIR_INST_PHI) {
            if (inst->num_ops >= 1u) {
                uint32_t ty_air = (uint32_t)inst->ops[0];
                uint32_t ty_spv = emit_air_type(c, ty_air);
                uint32_t var = dispatch_phi_var(c, result_value_id, ty_air,
                                                ty_spv);
                if (var) {
                    uint32_t id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t lo[] = { ty_spv, id, var };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, lo, 3);
                    bind_value_spv(c, result_value_id, id);
                    set_result_air_type(c, result_value_id, ty_air);
                    set_value_spv_type(c, result_value_id, ty_spv);
                }
            }
        } else {
            switch (inst->code) {
                case LAGFX_AIR_INST_ALLOCA:    emit_inst_alloca(c, i, inst, result_value_id); break;
                case LAGFX_AIR_INST_CAST:      emit_inst_cast(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_CALL:      emit_inst_call(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_GEP:
                case LAGFX_AIR_INST_GEP_OLD:   emit_inst_gep(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_STORE:
                case LAGFX_AIR_INST_STORE_OLD: emit_inst_store(c, inst, next_val_id); break;
                case LAGFX_AIR_INST_LOAD:      emit_inst_load(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_SHUFFLEVEC:emit_inst_shufflevec(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_INSERTVAL: emit_inst_insertval(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_BINOP:     emit_inst_binop(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_UNOP:      emit_inst_unop(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_CMP:       emit_inst_cmp(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_CMP2:      emit_inst_cmp2(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_SELECT:
                case LAGFX_AIR_INST_VSELECT:   emit_inst_select(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_EXTRACTVAL:emit_inst_extractval(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_EXTRACTELT:emit_inst_extractelt(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_INSERTELT: emit_inst_insertelt(c, i, inst, result_value_id, next_val_id); break;
                case LAGFX_AIR_INST_DECLAREBLOCKS: break;
                default:
                    LAGFX_TRACE("translate_function(dispatch): dropping unhandled "
                                "AIR opcode raw=%u (code=%d) at i[%u]",
                                inst->raw_code, (int)inst->code, i);
                    break;
            }
        }
        if (produces && bb != 0u) dispatch_spill_result(c, result_value_id);
        if (produces) next_val_id++;
    }
    *io_val_id = next_val_id;
}


/* Pre-pass type inference for case-defined values (back-edge support).
 * Loop back edges make a value's USE emit before its DEF — the def-site
 * spill var doesn't exist yet, so those uses degraded to undefs (8/61 of
 * Xgc's branch conditions). Infer result AIR types where the record
 * carries or trivially implies them, pre-create the spill variables, and
 * the resolve hook's forward-ref loads become runtime-correct (dominance
 * in the ORIGINAL CFG guarantees the def's store executes first). */
static void dispatch_precreate_spills(xlate_ctx_t *c, uint32_t nbb) {
    (void)nbb;
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    /* inferred AIR type per value id (NONE = unknown); lanes for BOOL. */
    uint32_t *ity = (uint32_t *)malloc(c->value_id_capacity * sizeof(uint32_t));
    uint8_t  *blanes = (uint8_t *)calloc(c->value_id_capacity, 1u);
    if (!ity || !blanes) { free(ity); free(blanes); return; }
    for (uint32_t i = 0; i < c->value_id_capacity; i++)
        ity[i] = LAGFX_AIR_TYPE_NONE;
    /* Seed args + constants (their types are declared). */
    for (uint32_t a = 0; a < c->num_args; a++)
        ity[c->arg_id_base + a] = c->arg_air_type_ids[a];
    {
        uint32_t nmc = 0;
        const lagfx_air_constant_t *mc = lagfx_air_module_constants(c->m, &nmc);
        uint32_t base = c->module_val_count - nmc;
        for (uint32_t i = 0; i < nmc; i++)
            if (base + i < c->value_id_capacity)
                ity[base + i] = mc[i].type_index;
        uint32_t nlc = 0;
        const lagfx_air_constant_t *lc =
            lagfx_air_function_body_local_constants(c->body, &nlc);
        uint32_t lbase = c->arg_id_base + c->num_args;
        for (uint32_t i = 0; i < nlc && lc; i++)
            if (lbase + i < c->value_id_capacity)
                ity[lbase + i] = lc[i].type_index;
    }

    uint32_t vid = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (!inst_produces_value(c, inst)) continue;
        uint32_t t = LAGFX_AIR_TYPE_NONE; uint8_t lanes = 0;
        switch (inst->code) {
            case LAGFX_AIR_INST_PHI:
                if (inst->num_ops >= 1u) t = (uint32_t)inst->ops[0];
                break;
            case LAGFX_AIR_INST_LOAD:
                if (inst->num_ops >= 2u) t = (uint32_t)inst->ops[1];
                break;
            case LAGFX_AIR_INST_CAST:
                if (inst->num_ops >= 2u) t = (uint32_t)inst->ops[1];
                break;
            case LAGFX_AIR_INST_CALL:
                t = call_return_air_type(c, inst);
                break;
            case LAGFX_AIR_INST_BINOP:
            case LAGFX_AIR_INST_SELECT: {
                uint32_t opnd = resolve_relative((uint32_t)inst->ops[0], vid);
                if (opnd < c->value_id_capacity) t = ity[opnd];
                break;
            }
            case LAGFX_AIR_INST_INSERTELT: {
                uint32_t opnd = resolve_relative((uint32_t)inst->ops[0], vid);
                if (opnd < c->value_id_capacity) t = ity[opnd];
                break;
            }
            case LAGFX_AIR_INST_EXTRACTELT: {
                uint32_t opnd = resolve_relative((uint32_t)inst->ops[0], vid);
                if (opnd < c->value_id_capacity) {
                    if (ity[opnd] < n_types &&
                        ts[ity[opnd]].kind == LAGFX_AIR_TYPE_VECTOR &&
                        ts[ity[opnd]].num_op >= 2u)
                        t = ts[ity[opnd]].op[1];
                    else if (ity[opnd] == LAGFX_AIR_TYPE_BOOL) {
                        t = LAGFX_AIR_TYPE_BOOL; lanes = 1;
                    }
                }
                break;
            }
            case LAGFX_AIR_INST_CMP:
            case LAGFX_AIR_INST_CMP2: {
                t = LAGFX_AIR_TYPE_BOOL; lanes = 1;
                uint32_t opnd = resolve_relative((uint32_t)inst->ops[0], vid);
                if (opnd < c->value_id_capacity && ity[opnd] < n_types &&
                    ts[ity[opnd]].kind == LAGFX_AIR_TYPE_VECTOR &&
                    ts[ity[opnd]].num_op >= 1u)
                    lanes = (uint8_t)ts[ity[opnd]].op[0];
                break;
            }
            default:
                break;
        }
        if (t < n_types &&
            (ts[t].kind == LAGFX_AIR_TYPE_POINTER ||
             ts[t].kind == LAGFX_AIR_TYPE_VOID))
            t = LAGFX_AIR_TYPE_NONE;   /* never spill pointers/void */
        ity[vid] = t; blanes[vid] = lanes;
        vid++;
    }

    /* Pre-create spill vars for CASE-defined (def_bb > 0) inferred values. */
    for (uint32_t v = c->inst_id_base; v < c->value_id_capacity; v++) {
        if (!c->value_def_bb || c->value_def_bb[v] == 0xFFFFu ||
            c->value_def_bb[v] == 0u)
            continue;
        uint32_t t = ity[v];
        if (t == LAGFX_AIR_TYPE_NONE) continue;
        uint32_t ty_spv, ptrty;
        if (t == LAGFX_AIR_TYPE_BOOL) {
            uint32_t b = emit_type_bool(c);
            ty_spv = (blanes[v] > 1u) ? emit_type_vec(c, b, blanes[v]) : b;
            ptrty = dispatch_ptr_type_spv(c, ty_spv);
        } else {
            ty_spv = emit_air_type(c, t);
            if (!ty_spv) continue;
            ptrty = dispatch_ptr_type(c, t, ty_spv);
        }
        uint32_t var = lagfx_spv_builder_alloc_id(c->b);
        if (!lagfx_spv_builder_emit_local_var(c->b, ptrty, var)) break;
        c->spill_var[v]  = var;
        c->spill_type[v] = ty_spv;
    }
    free(ity); free(blanes);
}

/* The dispatch driver. Returns false when the body isn't partitionable
 * (caller falls to the linear path). */
static bool translate_body_dispatch(xlate_ctx_t *c, lagfx_status_t *out_st) {
    /* Partition on the heap — real compositor fragments reach 474 blocks. */
    uint32_t nterm = 0;
    for (uint32_t i = 0; i < c->num_insts; i++)
        if (inst_is_terminator(c->insts[i].code)) nterm++;
    if (nterm < 2u) return false;      /* single block: linear path is fine */
    lagfx_bb_t *bbs = (lagfx_bb_t *)calloc(nterm, sizeof(*bbs));
    if (!bbs) return false;
    uint32_t nbb = 0, start = 0;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        if (inst_is_terminator(c->insts[i].code)) {
            bbs[nbb].index = nbb;
            bbs[nbb].first_inst = start;
            bbs[nbb].last_inst = i;
            nbb++;
            start = i + 1u;
        }
    }
    if (start != c->num_insts || nbb < 2u) { free(bbs); return false; }

    /* Def-block map for every value id (0xFFFF = not a body result).
     * ALLOCAs count as block 0 — their OpVariables are hoisted to the
     * entry block and dominate everything. */
    c->value_def_bb = (uint16_t *)malloc(c->value_id_capacity * sizeof(uint16_t));
    c->spill_var    = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    c->spill_type   = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    c->phi_var      = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    c->phi_type     = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    if (!c->value_def_bb || !c->spill_var || !c->spill_type || !c->phi_var
        || !c->phi_type) {
        free(bbs);
        free(c->value_def_bb); free(c->spill_var);
        free(c->spill_type);   free(c->phi_var);   free(c->phi_type);
        c->value_def_bb = NULL; c->spill_var = NULL;
        c->spill_type = NULL;   c->phi_var = NULL; c->phi_type = NULL;
        return false;
    }
    memset(c->value_def_bb, 0xFF, c->value_id_capacity * sizeof(uint16_t));
    c->value_def_inst = (uint32_t *)malloc(c->value_id_capacity * sizeof(uint32_t));
    if (c->value_def_inst)
        memset(c->value_def_inst, 0xFF, c->value_id_capacity * sizeof(uint32_t));
    {
        uint32_t vid = c->inst_id_base, bb = 0;
        for (uint32_t i = 0; i < c->num_insts; i++) {
            const lagfx_air_inst_t *inst = &c->insts[i];
            if (inst_produces_value(c, inst) && vid < c->value_id_capacity) {
                c->value_def_bb[vid] =
                    (inst->code == LAGFX_AIR_INST_ALLOCA) ? 0u : (uint16_t)bb;
                if (c->value_def_inst) c->value_def_inst[vid] = i;
                vid++;
            }
            if (inst_is_terminator(inst->code)) bb++;
        }
    }

    c->dispatch_mode = true;
    c->dispatch_cur_bb = 0;

    dispatch_precreate_spills(c, nbb);

    /* %blockvar + %itervar (uint, Function) via the locals splice. The
     * iteration counter is a TERMINATION GUARD: while cross-case values
     * still degrade to undefs (unspilled pointers etc.), a branch condition
     * can resolve to garbage and the dispatched CFG can spin forever —
     * live-observed as a vkWaitForFences timeout on the Xgc panel draw
     * followed by a lavapipe SIGSEGV at the next present. The header
     * bails to OpReturn after DISPATCH_MAX_ITERS. */
    uint32_t uint_spv = emit_type_int_w(c, 32u, 0u);
    uint32_t blk_ptr  = dispatch_ptr_type(c, LAGFX_AIR_TYPE_NONE - 2u, uint_spv);
    uint32_t blockvar = lagfx_spv_builder_alloc_id(c->b);
    lagfx_spv_builder_emit_local_var(c->b, blk_ptr, blockvar);
    uint32_t itervar = lagfx_spv_builder_alloc_id(c->b);
    lagfx_spv_builder_emit_local_var(c->b, blk_ptr, itervar);
    { uint32_t so[] = { itervar, emit_const_uint32(c, 0u) };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2); }

    /* Labels. */
    uint32_t lbl_header   = lagfx_spv_builder_alloc_id(c->b);
    uint32_t lbl_body     = lagfx_spv_builder_alloc_id(c->b);
    uint32_t lbl_selmerge = lagfx_spv_builder_alloc_id(c->b);
    uint32_t lbl_cont     = lagfx_spv_builder_alloc_id(c->b);
    uint32_t lbl_exit     = lagfx_spv_builder_alloc_id(c->b);
    uint32_t *lbl_bb = (uint32_t *)calloc(nbb, sizeof(uint32_t));
    if (!lbl_bb) { free(bbs); *out_st = LAGFX_ERR_OUT_OF_MEMORY; return true; }
    for (uint32_t i = 1; i < nbb; i++) lbl_bb[i] = lagfx_spv_builder_alloc_id(c->b);

    /* bb0 body straight into the (already open) entry block. */
    uint32_t val_id = c->inst_id_base;
    dispatch_emit_block(c, bbs, 0, &val_id);
    dispatch_lower_terminator(c, bbs, nbb, 0, &c->insts[bbs[0].last_inst],
                              val_id, blockvar, lbl_header);

    /* header: cur = load blockvar ; iter++ ; OpLoopMerge exit cont ;
     * branch body while iter < DISPATCH_MAX_ITERS else exit (OpReturn). */
    { uint32_t o[] = { lbl_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
    uint32_t cur = lagfx_spv_builder_alloc_id(c->b);
    { uint32_t o[] = { uint_spv, cur, blockvar };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, o, 3); }
    uint32_t it = lagfx_spv_builder_alloc_id(c->b);
    { uint32_t o[] = { uint_spv, it, itervar };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOAD, o, 3); }
    uint32_t itp = lagfx_spv_builder_alloc_id(c->b);
    { uint32_t o[] = { uint_spv, itp, it, emit_const_uint32(c, 1u) };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_IADD, o, 4); }
    { uint32_t so[] = { itervar, itp };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_STORE, so, 2); }
    uint32_t inbound = lagfx_spv_builder_alloc_id(c->b);
    { uint32_t o[] = { emit_type_bool(c), inbound, itp,
                       emit_const_uint32(c, 65536u) };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_ULESS_THAN, o, 4); }
    { uint32_t o[] = { lbl_exit, lbl_cont, 0u /* LoopControl None */ };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LOOP_MERGE, o, 3); }
    { uint32_t o[] = { inbound, lbl_body, lbl_exit };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH_CONDITIONAL, o, 3); }

    /* body: selection merge + switch over the block index. */
    { uint32_t o[] = { lbl_body };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
    { uint32_t o[] = { lbl_selmerge, 0u /* SelectionControl None */ };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SELECTION_MERGE, o, 2); }
    {
        /* OpSwitch: [Selector, Default, (literal, label)...] */
        uint32_t nops = 2u + 2u * (nbb - 1u);
        uint32_t *o = (uint32_t *)malloc(nops * sizeof(uint32_t));
        if (!o) { free(bbs); free(lbl_bb); *out_st = LAGFX_ERR_OUT_OF_MEMORY; return true; }
        o[0] = cur; o[1] = lbl_selmerge;
        for (uint32_t i = 1; i < nbb; i++) {
            o[2u + 2u * (i - 1u)] = i;
            o[3u + 2u * (i - 1u)] = lbl_bb[i];
        }
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_SWITCH, o, nops);
        free(o);
    }

    /* Case blocks. */
    for (uint32_t i = 1; i < nbb; i++) {
        { uint32_t o[] = { lbl_bb[i] };
          lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
        c->dispatch_cur_bb = i;
        dispatch_emit_block(c, bbs, i, &val_id);
        dispatch_lower_terminator(c, bbs, nbb, i, &c->insts[bbs[i].last_inst],
                                  val_id, blockvar, lbl_selmerge);
    }

    /* selmerge -> cont -> header ; exit unreachable (returns are inline). */
    { uint32_t o[] = { lbl_selmerge };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
    { uint32_t o[] = { lbl_cont };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, o, 1); }
    { uint32_t o[] = { lbl_cont };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
    { uint32_t o[] = { lbl_header };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_BRANCH, o, 1); }
    { uint32_t o[] = { lbl_exit };
      lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_LABEL, o, 1); }
    if (c->is_helper && c->helper_ret_spv) {
        /* Non-void helper: the iteration-cap bail must still return a
         * value of the declared type. */
        uint32_t u = emit_undef(c, c->helper_ret_spv);
        uint32_t o[] = { u };
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN_VALUE, o, 1);
    } else {
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN, NULL, 0);
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);

    c->dispatch_mode = false;
    free(bbs); free(lbl_bb);
    *out_st = LAGFX_OK;
    return true;
}

static lagfx_status_t translate_body(xlate_ctx_t *c) {
    /* Structured control flow: if the body matches the recognised single
     * reducible loop, emit multi-basic-block SPIR-V. Otherwise fall
     * through to the linear single-block path (unchanged) — this keeps
     * every single-block shader and the one multi-block SkyLight entry
     * (InPlaceColorOrClampEDR) on their existing, validated route. */
    {
        lagfx_loop_plan_t plan = detect_simple_loop(c);
        if (plan.valid) {
            LAGFX_TRACE("translate_function: recognised single loop "
                        "(header bb=%u merge bb=%u)", plan.header_bb, plan.merge_bb);
            return translate_body_loop(c, &plan);
        }
    }
    {
        lagfx_guarded_loop_plan_t plan = detect_guarded_loop(c);
        if (plan.valid) {
            LAGFX_TRACE("translate_function: recognised guarded loop "
                        "(header bb=%u merge bb=%u)", plan.header_bb, plan.merge_bb);
            return translate_body_guarded_loop(c, &plan);
        }
    }
    /* General multi-block CFG: block-dispatch loop (kill-switch keeps the
     * old linearizing path for A/B). */
    if (!getenv("LAGFX_DISABLE_BLOCKSWITCH")) {
        lagfx_status_t dst = LAGFX_OK;
        if (translate_body_dispatch(c, &dst)) {
            LAGFX_TRACE("translate_function: block-dispatch control flow");
            return dst;
        }
    }

    /* Pass 1: allocate result SPIR-V ids for each value-producing
     * instruction; populate value-id → spv map. */
    uint32_t next_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        if (inst_produces_value(c, inst)) {
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
        bool produces = inst_produces_value(c, inst);
        uint32_t result_value_id = produces ? next_val_id : 0u;

        switch (inst->code) {
            case LAGFX_AIR_INST_ALLOCA:
                emit_inst_alloca(c, i, inst, result_value_id);
                break;
            case LAGFX_AIR_INST_CAST:
                emit_inst_cast(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_CALL:
                emit_inst_call(c, i, inst, result_value_id, next_val_id);
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
            case LAGFX_AIR_INST_CMP:
                emit_inst_cmp(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_CMP2:
                emit_inst_cmp2(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_SELECT:
            case LAGFX_AIR_INST_VSELECT:
                emit_inst_select(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_EXTRACTVAL:
                emit_inst_extractval(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_EXTRACTELT:
                emit_inst_extractelt(c, i, inst, result_value_id, next_val_id);
                break;
            case LAGFX_AIR_INST_INSERTELT:
                emit_inst_insertelt(c, i, inst, result_value_id, next_val_id);
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
     * OpReturn (OpReturnValue undef for a non-void helper) to keep the
     * function valid. */
    if (c->num_insts == 0u ||
        c->insts[c->num_insts - 1u].code != LAGFX_AIR_INST_RET) {
        if (c->is_helper && c->helper_ret_spv) {
            uint32_t u = emit_undef(c, c->helper_ret_spv);
            uint32_t o[] = { u };
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN_VALUE, o, 1);
        } else {
            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_RETURN, NULL, 0);
        }
    }

    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_FUNCTION_END, NULL, 0);
    return LAGFX_OK;
}

/* Body prescan: collect the element type each nonstruct `device T*` buffer
 * arg is ACTUALLY accessed with (GEP source_elem_type / direct-LOAD result
 * type). If every access agrees on one type that differs from the declared
 * pointee, set arg_pointee_override[a] so the Block is synthesized with the
 * access type (see the field doc; the VfxXgb `device uchar*`-as-float4
 * login vertex shader). Runs before emit_prologue — uses only arg/type
 * tables and the decoded instruction stream. */
static void prescan_buffer_arg_access_types(xlate_ctx_t *c) {
    uint32_t access_ty[LAGFX_MAX_VERTEX_ARGS] = {0};
    bool     mixed[LAGFX_MAX_VERTEX_ARGS] = {false};
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);

    /* Which args are raw byte buffers (declared pointee = i8)? Their cast
     * views get their own aliased Block variables (see bytebuf_view doc). */
    bool is_bytebuf[LAGFX_MAX_VERTEX_ARGS] = {false};
    for (uint32_t a = 0; a < c->num_args && a < LAGFX_MAX_VERTEX_ARGS; a++) {
        uint32_t aty = c->arg_air_type_ids[a];
        if (aty >= n_types || ts[aty].kind != LAGFX_AIR_TYPE_POINTER ||
            ts[aty].num_op < 1u)
            continue;
        uint32_t p = ts[aty].op[0];
        is_bytebuf[a] = (p < n_types && ts[p].kind == LAGFX_AIR_TYPE_INTEGER &&
                         ts[p].num_op >= 1u && ts[p].op[0] == 8u);
    }

    /* Alias map: value-id -> (arg index + 1), 0 = not an arg alias. The
     * body reaches a buffer arg through pointer BITCAST chains (and
     * pointer-preserving casts), so track those the same way the emitters'
     * per-value storage-class table does. */
    uint8_t *alias = (uint8_t *)calloc(c->value_id_capacity, 1u);
    uint8_t *vview = (uint8_t *)calloc(c->value_id_capacity, 1u);
    if (!alias || !vview) { free(alias); free(vview); return; }
    for (uint32_t a = 0; a < c->num_args && a < LAGFX_MAX_VERTEX_ARGS; a++)
        alias[c->arg_id_base + a] = (uint8_t)(a + 1u);

    uint32_t next_val_id = c->inst_id_base;
    for (uint32_t i = 0; i < c->num_insts; i++) {
        const lagfx_air_inst_t *inst = &c->insts[i];
        uint32_t ptr_rel = 0, acc = 0;
        if ((inst->code == LAGFX_AIR_INST_GEP ||
             inst->code == LAGFX_AIR_INST_GEP_OLD) && inst->num_ops >= 4u) {
            ptr_rel = (uint32_t)inst->ops[2];
            acc     = (uint32_t)inst->ops[1];   /* source_elem_type */
        } else if (inst->code == LAGFX_AIR_INST_LOAD && inst->num_ops >= 2u) {
            ptr_rel = (uint32_t)inst->ops[0];
            acc     = (uint32_t)inst->ops[1];   /* result type */
        } else if (inst->code == LAGFX_AIR_INST_CAST && inst->num_ops >= 3u) {
            /* Pointer-preserving casts (BITCAST=11 / ADDRSPACECAST=12)
             * alias the source; a cast of a BYTE-BUFFER arg to a typed
             * pointer registers a view. */
            int cast_op = (int)inst->ops[2];
            if (cast_op == 11 || cast_op == 12) {
                uint32_t src = resolve_relative((uint32_t)inst->ops[0],
                                                next_val_id);
                if (src < c->value_id_capacity &&
                    next_val_id < c->value_id_capacity) {
                    alias[next_val_id] = alias[src];
                    vview[next_val_id] = vview[src];
                    uint32_t a = alias[src] ? (uint32_t)alias[src] - 1u
                                            : UINT32_MAX;
                    uint32_t dest_ty = (uint32_t)inst->ops[1];
                    if (a < LAGFX_MAX_VERTEX_ARGS && is_bytebuf[a] &&
                        dest_ty < n_types &&
                        ts[dest_ty].kind == LAGFX_AIR_TYPE_POINTER &&
                        ts[dest_ty].num_op >= 1u) {
                        uint32_t pointee = ts[dest_ty].op[0];
                        bool p_is_i8 = pointee < n_types &&
                            ts[pointee].kind == LAGFX_AIR_TYPE_INTEGER &&
                            ts[pointee].num_op >= 1u && ts[pointee].op[0] == 8u;
                        if (!p_is_i8) {
                            /* Find-or-register the (arg, pointee) view. */
                            uint32_t v;
                            for (v = 0; v < c->n_bytebuf_views; v++)
                                if (c->bytebuf_view[v].arg == a &&
                                    c->bytebuf_view[v].pointee_ty == pointee)
                                    break;
                            if (v == c->n_bytebuf_views &&
                                v < LAGFX_MAX_BYTEBUF_VIEWS) {
                                c->bytebuf_view[v].arg = (uint8_t)a;
                                c->bytebuf_view[v].pointee_ty = pointee;
                                c->bytebuf_view[v].var_id =
                                    lagfx_spv_builder_alloc_id(c->b);
                                c->n_bytebuf_views++;
                                LAGFX_TRACE("translate_function: bytebuf arg "
                                            "%u view pointee air type %u",
                                            a, pointee);
                            }
                            if (v < c->n_bytebuf_views)
                                vview[next_val_id] = (uint8_t)(v + 1u);
                        }
                    }
                }
            }
        }
        if (acc) {
            uint32_t ptr_id = resolve_relative(ptr_rel, next_val_id);
            if (ptr_id < c->value_id_capacity && alias[ptr_id] &&
                !vview[ptr_id]) {
                uint32_t a = (uint32_t)alias[ptr_id] - 1u;
                if (a < LAGFX_MAX_VERTEX_ARGS) {
                    if (access_ty[a] && access_ty[a] != acc) mixed[a] = true;
                    access_ty[a] = acc;
                }
            }
        }
        if (inst_produces_value(c, inst)) next_val_id++;
    }
    c->arg_alias  = alias;   /* kept for the emitters; freed with the
                              * other side tables */
    c->value_view = vview;

    for (uint32_t a = 0; a < c->num_args && a < LAGFX_MAX_VERTEX_ARGS; a++) {
        if (!access_ty[a] || mixed[a]) continue;
        /* Declared pointee (raw, no override yet). */
        uint32_t aty = c->arg_air_type_ids[a];
        if (aty >= n_types || ts[aty].kind != LAGFX_AIR_TYPE_POINTER ||
            ts[aty].num_op < 1u)
            continue;
        uint32_t declared = ts[aty].op[0];
        if (access_ty[a] == declared) continue;
        /* Both the declared pointee and the access type must be modellable
         * nonstruct element types (struct pointees keep the Block==struct
         * path untouched). */
        if (!buffer_arg_nonstruct_pointee(c, declared)) continue;
        if (!buffer_arg_nonstruct_pointee(c, access_ty[a])) continue;
        c->arg_pointee_override[a] = access_ty[a];
        LAGFX_TRACE("translate_function: buffer arg %u pointee override "
                    "air type %u -> %u (unanimous access type)",
                    a, declared, access_ty[a]);
    }
}


/* ===================================================================
 * Per-function context lifecycle
 *
 * Bitcode value-id spaces are PER FUNCTION (module values shared; args /
 * local consts / instruction results per body), so each translated
 * function — the entry and every helper — gets its own value maps. The
 * module-scope type/constant caches are shared via copy_module_state.
 * =================================================================== */

/* Open fn_idx's body and allocate the per-function tables. Requires c->m
 * (and, for cache sharing, copy_module_state already applied). Frees
 * nothing on failure beyond its own allocations — call xlate_ctx_free_fn
 * unconditionally afterwards. */
static lagfx_status_t xlate_ctx_init_fn(xlate_ctx_t *c, uint32_t fn_idx) {
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(c->m, &n_fns);
    if (fn_idx >= n_fns) return LAGFX_ERR_INVALID_ARG;
    c->fn = &fns[fn_idx];
    if (c->fn->is_proto || c->fn->body_offset == 0u) return LAGFX_ERR_INVALID_ARG;

    lagfx_status_t st = lagfx_air_function_body_open(c->m, fn_idx, &c->body);
    if (st != LAGFX_OK || !c->body) return LAGFX_ERR_PROTOCOL;
    c->insts = lagfx_air_function_body_instructions(c->body, &c->num_insts);

    /* Arg count + AIR types from the function-type entry.
     * ops[0] = varargs, ops[1] = return type, ops[2..] = params. */
    {
        uint32_t fn_ty_idx = c->fn->type_index;
        uint32_t nn_types = 0;
        const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &nn_types);
        if (fn_ty_idx < nn_types && ts[fn_ty_idx].kind == LAGFX_AIR_TYPE_FUNCTION) {
            c->num_args = ts[fn_ty_idx].num_op > 2u ? ts[fn_ty_idx].num_op - 2u : 0u;
            if (c->num_args > 0u) {
                c->arg_spv_ids = (uint32_t *)calloc(c->num_args, sizeof(uint32_t));
                c->arg_air_type_ids = (uint32_t *)calloc(c->num_args, sizeof(uint32_t));
                if (!c->arg_spv_ids || !c->arg_air_type_ids)
                    return LAGFX_ERR_OUT_OF_MEMORY;
                for (uint32_t i = 0; i < c->num_args; i++)
                    c->arg_air_type_ids[i] = ts[fn_ty_idx].op[2u + i];
            }
        }
    }

    /* Value-id layout. LLVM's value enumeration assigns ids to global
     * variables BEFORE functions, then module constants; bitcode operands
     * that are NOT relative-encoded (CST_CODE_AGGREGATE constituents,
     * module-const references) index that full list. Then args, then the
     * body's local CONSTANTS_BLOCK, then instruction results. */
    uint32_t n_mod_consts = 0;
    (void)lagfx_air_module_constants(c->m, &n_mod_consts);
    uint32_t mod_val_base = lagfx_air_module_num_globalvars(c->m) + n_fns;
    c->module_val_count = mod_val_base + n_mod_consts;
    c->arg_id_base      = c->module_val_count;
    uint32_t n_local_consts = 0;
    (void)lagfx_air_function_body_local_constants(c->body, &n_local_consts);
    c->local_const_count = n_local_consts;
    c->inst_id_base = c->arg_id_base + c->num_args + c->local_const_count;

    c->value_id_capacity = c->inst_id_base + c->num_insts + 32u;
    c->value_id_to_spv = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    c->value_id_to_lit_i32 = (int32_t *)calloc(c->value_id_capacity, sizeof(int32_t));
    c->value_id_lit_i32_valid = (bool *)calloc(c->value_id_capacity, sizeof(bool));
    c->value_storage = (uint8_t *)calloc(c->value_id_capacity, sizeof(uint8_t));
    c->value_spv_type = (uint32_t *)calloc(c->value_id_capacity, sizeof(uint32_t));
    c->inst_result_air_type = (uint32_t *)calloc(c->num_insts + 1u, sizeof(uint32_t));
    if (!c->value_id_to_spv || !c->value_id_to_lit_i32 ||
        !c->value_id_lit_i32_valid || !c->value_storage ||
        !c->value_spv_type || !c->inst_result_air_type)
        return LAGFX_ERR_OUT_OF_MEMORY;
    /* Init to NONE, not 0: a zero slot would read as "type index 0"
     * (= float), but an unwritten slot means "type unknown". */
    for (uint32_t ti = 0; ti < c->num_insts + 1u; ti++)
        c->inst_result_air_type[ti] = LAGFX_AIR_TYPE_NONE;
    return LAGFX_OK;
}

/* Free the per-function tables + body. Never frees the shared module
 * type table (c->spv_type_ids) or the builder. */
static void xlate_ctx_free_fn(xlate_ctx_t *c) {
    free(c->arg_spv_ids);
    free(c->arg_air_type_ids);
    free(c->value_id_to_spv);
    free(c->value_id_to_lit_i32);
    free(c->value_id_lit_i32_valid);
    free(c->value_storage);
    free(c->arg_alias);
    free(c->value_view);
    free(c->value_spv_type);
    free(c->value_def_bb);
    free(c->spill_var);
    free(c->spill_type);
    free(c->phi_var);
    free(c->phi_type);
    free(c->value_def_inst);
    free(c->inst_result_air_type);
    if (c->body) lagfx_air_function_body_free(c->body);
    c->body = NULL;
}

/* ===================================================================
 * Module / local constant pre-binding
 * =================================================================== */

/* Bind module-level constants into c's value map. The FIRST call (entry
 * ctx) emits the OpConstants and records the ids in the shared helpers
 * struct; later calls (helper ctxs) copy the recorded bindings — the
 * constants themselves live once in the types section. */
static void bind_module_constants(xlate_ctx_t *c) {
    uint32_t n_mod_consts = 0;
    const lagfx_air_constant_t *cs = lagfx_air_module_constants(c->m, &n_mod_consts);
    uint32_t n_fns = 0;
    (void)lagfx_air_module_functions(c->m, &n_fns);
    uint32_t mod_val_base = lagfx_air_module_num_globalvars(c->m) + n_fns;
    xlate_helpers_t *h = c->helpers;

    if (h && h->mod_consts_bound) {
        for (uint32_t i = 0; i < h->n_mod_consts && i < n_mod_consts; i++) {
            if (h->mod_const_spv && h->mod_const_spv[i])
                bind_value_spv(c, mod_val_base + i, h->mod_const_spv[i]);
            if (h->mod_const_lit_valid && h->mod_const_lit_valid[i])
                bind_value_lit_i32(c, mod_val_base + i, h->mod_const_lit[i]);
        }
        return;
    }
    if (h) {
        h->n_mod_consts = n_mod_consts;
        if (n_mod_consts) {
            h->mod_const_spv = (uint32_t *)calloc(n_mod_consts, sizeof(uint32_t));
            h->mod_const_lit = (int32_t *)calloc(n_mod_consts, sizeof(int32_t));
            h->mod_const_lit_valid = (bool *)calloc(n_mod_consts, sizeof(bool));
        }
        h->mod_consts_bound = true;
    }

    for (uint32_t i = 0; i < n_mod_consts; i++) {
        uint32_t vid = mod_val_base + i;
        const lagfx_air_constant_t *k = &cs[i];
        uint32_t spv_id = 0u;
        switch (k->kind) {
            case LAGFX_AIR_CONST_INTEGER: {
                /* Bind i8/i16/i32/i64 integer constants. i8/i16 were once
                 * skipped "pending capabilities" — STALE: the translator has
                 * emitted uchar/ushort types (Int8/Int16 caps) since the
                 * byte-buffer work, but unbound i8 constants made every
                 * SWITCH whose case values are i8 constants (the Xgc login
                 * material's final-composite mode switch, inst 2521: cases
                 * 284/285/288, default = the undef-phi block) drop ALL cases
                 * → always default → NaN placeholder at 100% of pixels
                 * (offline replay, 2026-07-23). i1/i128 still fall back. */
                uint32_t ty_air = k->type_index;
                if (ty_air < c->num_air_types) {
                    const lagfx_air_type_t *t = &((const lagfx_air_type_t *)lagfx_air_module_types(c->m, &(uint32_t){0}))[ty_air];
                    if (t->kind == LAGFX_AIR_TYPE_INTEGER) {
                        uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
                        if (w == 8u || w == 16u) {
                            uint32_t ty_spv = emit_type_int_w(c, w, 0u);
                            spv_id = lagfx_spv_builder_alloc_id(c->b);
                            uint32_t mask = w == 8u ? 0xFFu : 0xFFFFu;
                            uint32_t ops[] = { ty_spv, spv_id,
                                                (uint32_t)(uint64_t)k->payload.i64 & mask };
                            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                            bind_value_lit_i32(c, vid, (int32_t)((uint64_t)k->payload.i64 & mask));
                            if (h && h->mod_const_lit_valid) {
                                h->mod_const_lit[i] = (int32_t)((uint64_t)k->payload.i64 & mask);
                                h->mod_const_lit_valid[i] = true;
                            }
                        } else if (w == 32u) {
                            uint32_t ty_spv = emit_type_int_w(c, 32u, 0u);
                            spv_id = lagfx_spv_builder_alloc_id(c->b);
                            uint32_t ops[] = { ty_spv, spv_id,
                                                (uint32_t)(uint64_t)k->payload.i64 };
                            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                            bind_value_lit_i32(c, vid, (int32_t)(uint64_t)k->payload.i64);
                            if (h && h->mod_const_lit_valid) {
                                h->mod_const_lit[i] = (int32_t)(uint64_t)k->payload.i64;
                                h->mod_const_lit_valid[i] = true;
                            }
                        } else if (w == 64u) {
                            uint32_t ty_spv = emit_type_int_w(c, 64u, 0u);
                            spv_id = lagfx_spv_builder_alloc_id(c->b);
                            uint64_t bits = (uint64_t)k->payload.i64;
                            uint32_t ops[] = { ty_spv, spv_id,
                                                (uint32_t)(bits & 0xFFFFFFFFu),
                                                (uint32_t)(bits >> 32u) };
                            lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 4);
                        }
                    }
                }
                break;
            }
            case LAGFX_AIR_CONST_FLOAT: {
                uint32_t ty_spv = emit_type_float32(c);
                spv_id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t bits = f32_bits_from_double(k->payload.f64);
                uint32_t ops[] = { ty_spv, spv_id, bits };
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                break;
            }
            case LAGFX_AIR_CONST_NULL: {
                /* Zero of the constant's type. Skip if the type tree
                 * recursively contains i8 / i64 / 0-length array — those
                 * would force Int8/Int64 caps or fail SPIR-V validation. */
                uint32_t ty_air = k->type_index;
                if (ty_air < c->num_air_types &&
                    !air_type_requires_extra_cap(c, ty_air, 0)) {
                    uint32_t ty_spv = emit_air_type(c, ty_air);
                    spv_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t ops[] = { ty_spv, spv_id };
                    lagfx_spv_builder_emit_op(c->b, 46 /* OpConstantNull */, ops, 2);
                    /* NULL of an i32 type is the integer literal 0. */
                    const lagfx_air_type_t *tt = &((const lagfx_air_type_t *)lagfx_air_module_types(c->m, &(uint32_t){0}))[ty_air];
                    if (tt->kind == LAGFX_AIR_TYPE_INTEGER &&
                        tt->num_op >= 1u && tt->op[0] == 32u) {
                        bind_value_lit_i32(c, vid, 0);
                        if (h && h->mod_const_lit_valid) {
                            h->mod_const_lit[i] = 0;
                            h->mod_const_lit_valid[i] = true;
                        }
                    }
                }
                break;
            }
            default:
                /* AGGREGATE / DATA / STRING — defer; the operand will
                 * fall back to OpUndef of the expected type. */
                break;
        }
        if (spv_id) {
            bind_value_spv(c, vid, spv_id);
            if (h && h->mod_const_spv) h->mod_const_spv[i] = spv_id;
        }
    }
}

/* Bind the body's function-local constants at value-ids
 * [arg_id_end, arg_id_end + n_local_consts). These are the vec2f data
 * values, shufflevec masks, and other constants the function body
 * references but the module-level constants table doesn't carry. */
static void bind_local_constants(xlate_ctx_t *c) {
    uint32_t n_local_consts = 0;
    const lagfx_air_constant_t *local_consts =
        lagfx_air_function_body_local_constants(c->body, &n_local_consts);
    if (!local_consts) return;
    uint32_t lc_base = c->arg_id_base + c->num_args;
    uint32_t nn_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &nn_types);
    for (uint32_t i = 0; i < n_local_consts; i++) {
        uint32_t vid = lc_base + i;
        const lagfx_air_constant_t *k = &local_consts[i];
        uint32_t spv_id = 0u;
        uint32_t ty_air = k->type_index;
        if (ty_air >= nn_types) continue;
        const lagfx_air_type_t *t = &ts[ty_air];

        switch (k->kind) {
            case LAGFX_AIR_CONST_INTEGER: {
                if (t->kind == LAGFX_AIR_TYPE_INTEGER) {
                    uint32_t w = t->num_op >= 1u ? t->op[0] : 32u;
                    if (w == 8u || w == 16u) {
                        /* i8/i16 were unbound ("pending capabilities" —
                         * stale; uchar/ushort types already emit). Unbound
                         * i8 case values made the Xgc login material's
                         * final-composite SWITCH drop every case → default
                         * = the undef-phi block → NaN output at 100% of
                         * pixels (offline lavapipe replay, 2026-07-23). */
                        uint32_t mask = w == 8u ? 0xFFu : 0xFFFFu;
                        uint32_t ty_spv = emit_type_int_w(c, w, 0u);
                        spv_id = lagfx_spv_builder_alloc_id(c->b);
                        uint32_t ops[] = { ty_spv, spv_id,
                                            (uint32_t)(uint64_t)k->payload.i64 & mask };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                        bind_value_lit_i32(c, vid,
                                           (int32_t)((uint64_t)k->payload.i64 & mask));
                    } else if (w == 32u) {
                        uint32_t ty_spv = emit_type_int_w(c, 32u, 0u);
                        spv_id = lagfx_spv_builder_alloc_id(c->b);
                        uint32_t ops[] = { ty_spv, spv_id,
                                            (uint32_t)(uint64_t)k->payload.i64 };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                    } else if (w == 64u) {
                        uint32_t ty_spv = emit_type_int_w(c, 64u, 0u);
                        spv_id = lagfx_spv_builder_alloc_id(c->b);
                        uint64_t bits = (uint64_t)k->payload.i64;
                        uint32_t ops[] = { ty_spv, spv_id,
                                            (uint32_t)(bits & 0xFFFFFFFFu),
                                            (uint32_t)(bits >> 32u) };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 4);
                    }
                    if (w == 32u) {
                        bind_value_lit_i32(c, vid, (int32_t)(uint64_t)k->payload.i64);
                    }
                }
                break;
            }
            case LAGFX_AIR_CONST_FLOAT: {
                /* Float constants: the parser stored the raw u64 op[0]
                 * bits via union with double. For f32 types, extract the
                 * low 32 bits — they're the original IEEE-754 binary32
                 * pattern. */
                if (t->kind == LAGFX_AIR_TYPE_FLOAT) {
                    uint32_t ty_spv = emit_type_float32(c);
                    uint32_t bits = (uint32_t)(uint64_t)k->payload.i64;
                    spv_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t ops[] = { ty_spv, spv_id, bits };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                } else if (t->kind == LAGFX_AIR_TYPE_HALF) {
                    /* IEEE-754 binary16 scalar constant: OpConstant for
                     * OpTypeFloat 16 takes one 32-bit literal word with
                     * the 16-bit pattern in the low bits. */
                    uint32_t ty_spv = emit_type_half(c);
                    uint32_t bits = (uint32_t)(uint64_t)k->payload.i64 & 0xFFFFu;
                    spv_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t ops[] = { ty_spv, spv_id, bits };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                }
                break;
            }
            case LAGFX_AIR_CONST_NULL: {
                if (air_type_requires_extra_cap(c, ty_air, 0)) break;
                uint32_t ty_spv = emit_air_type(c, ty_air);
                spv_id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t ops[] = { ty_spv, spv_id };
                lagfx_spv_builder_emit_op(c->b, 46 /* OpConstantNull */, ops, 2);
                if (t->kind == LAGFX_AIR_TYPE_INTEGER &&
                    t->num_op >= 1u && t->op[0] == 32u) {
                    bind_value_lit_i32(c, vid, 0);
                }
                break;
            }
            case LAGFX_AIR_CONST_UNDEF:
            case LAGFX_AIR_CONST_UNKNOWN: {
                /* OpUndef of the constant's type. UNKNOWN covers LLVM's
                 * POISON code (26); treat as undef. */
                if (air_type_requires_extra_cap(c, ty_air, 0)) break;
                uint32_t ty_spv = emit_air_type(c, ty_air);
                spv_id = emit_undef(c, ty_spv);
                break;
            }
            case LAGFX_AIR_CONST_DATA: {
                /* DATA records hold a packed array of literal scalar
                 * values (raw u32 per lane) for vector constants. */
                if (t->kind == LAGFX_AIR_TYPE_VECTOR && t->num_op >= 2u) {
                    uint32_t lanes  = t->op[0];
                    uint32_t elem_ai = t->op[1];
                    const lagfx_air_type_t *et =
                        (elem_ai < nn_types) ? &ts[elem_ai] : NULL;
                    if (!et) break;
                    const uint32_t *raw = (const uint32_t *)
                        lagfx_air_function_body_payload_ptr(c->body, k->payload.bytes.offset);
                    if (!raw) break;
                    uint32_t n_words = k->payload.bytes.len / sizeof(uint32_t);
                    if (n_words < lanes) break;
                    uint32_t elem_spv = emit_air_type(c, elem_ai);
                    uint32_t lane_ids[16];
                    if (lanes > 16u) break;
                    for (uint32_t l = 0; l < lanes; l++) {
                        uint32_t lane_id = lagfx_spv_builder_alloc_id(c->b);
                        uint32_t ops[] = { elem_spv, lane_id, raw[l] };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT, ops, 3);
                        lane_ids[l] = lane_id;
                    }
                    uint32_t vec_spv = emit_air_type(c, ty_air);
                    spv_id = lagfx_spv_builder_alloc_id(c->b);
                    uint32_t ops[1 + 1 + 16];
                    ops[0] = vec_spv;
                    ops[1] = spv_id;
                    for (uint32_t l = 0; l < lanes; l++) ops[2u + l] = lane_ids[l];
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT_COMPOSITE,
                                                ops, 2u + lanes);
                }
                break;
            }
            case LAGFX_AIR_CONST_AGGREGATE: {
                /* AGGREGATE: payload bytes are a u32 array of absolute
                 * value-IDs of the component constants. */
                const uint32_t *comps = (const uint32_t *)
                    lagfx_air_function_body_payload_ptr(c->body, k->payload.bytes.offset);
                if (!comps) break;
                uint32_t ncomp = k->payload.bytes.len / sizeof(uint32_t);
                if (ncomp == 0u || ncomp > 16u) break;

                uint32_t vec_spv = emit_air_type(c, ty_air);
                uint32_t elem_ai = 0u;
                if (t->kind == LAGFX_AIR_TYPE_VECTOR && t->num_op >= 2u) {
                    elem_ai = t->op[1];
                } else if (t->kind == LAGFX_AIR_TYPE_ARRAY && t->num_op >= 2u) {
                    elem_ai = t->op[1];
                }
                uint32_t elem_spv_fallback =
                    elem_ai ? emit_air_type(c, elem_ai)
                            : emit_type_int_w(c, 32u, 0u);

                uint32_t comp_ids[16];
                for (uint32_t j = 0; j < ncomp; j++) {
                    uint32_t cid = resolve_value_spv(c, comps[j]);
                    if (!cid) cid = emit_undef(c, elem_spv_fallback);
                    comp_ids[j] = cid;
                }
                spv_id = lagfx_spv_builder_alloc_id(c->b);
                uint32_t ops[1 + 1 + 16];
                ops[0] = vec_spv;
                ops[1] = spv_id;
                for (uint32_t j = 0; j < ncomp; j++) ops[2u + j] = comp_ids[j];
                lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CONSTANT_COMPOSITE,
                                            ops, 2u + ncomp);
                break;
            }
            default:
                break;
        }
        if (spv_id) bind_value_spv(c, vid, spv_id);
    }
}

/* ===================================================================
 * Helper-function planning + emission (KICKOFF-ubershader-function-calls)
 * =================================================================== */

/* Value-class check for helper params / returns: types we can pass and
 * return by value with full fidelity. */
static bool helper_value_type_ok(const xlate_ctx_t *c, uint32_t ty) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (ty >= n) return false;
    switch (ts[ty].kind) {
        case LAGFX_AIR_TYPE_FLOAT:
        case LAGFX_AIR_TYPE_HALF:
            return true;
        case LAGFX_AIR_TYPE_INTEGER:
            return ts[ty].num_op >= 1u && ts[ty].op[0] <= 64u;
        case LAGFX_AIR_TYPE_VECTOR: {
            if (ts[ty].num_op < 2u) return false;
            uint32_t lanes = ts[ty].op[0], elem = ts[ty].op[1];
            if (lanes < 2u || lanes > 4u || elem >= n) return false;
            switch (ts[elem].kind) {
                case LAGFX_AIR_TYPE_FLOAT:
                case LAGFX_AIR_TYPE_HALF:
                    return true;
                case LAGFX_AIR_TYPE_INTEGER:
                    return ts[elem].num_op >= 1u && ts[elem].op[0] <= 64u;
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

/* Classify a helper param's AIR type. Returns a LAGFX_HPARAM_* kind or 0
 * (unsupported). For THREADPTR, *pointee_out receives the pointee. */
static int classify_helper_param(const xlate_ctx_t *c, uint32_t air_ty,
                                 uint32_t *pointee_out) {
    uint32_t n = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n);
    if (air_ty >= n) return 0;
    if (ts[air_ty].kind != LAGFX_AIR_TYPE_POINTER)
        return helper_value_type_ok(c, air_ty) ? LAGFX_HPARAM_VALUE : 0;
    if (ts[air_ty].num_op < 2u) return 0;
    uint32_t pointee   = ts[air_ty].op[0];
    uint32_t addrspace = ts[air_ty].op[1];
    if (pointee < n && ts[pointee].kind == LAGFX_AIR_TYPE_STRUCT_NAMED) {
        const char *nm = lagfx_air_module_string(c->m, ts[pointee].name_offset);
        if (nm) {
            const char *p = nm;
            if (strncmp(p, "struct.", 7u) == 0) p += 7u;
            if (strstr(p, "_texture") || strstr(p, "_depth"))
                return LAGFX_HPARAM_TEX;
            if (strstr(p, "_sampler")) return LAGFX_HPARAM_SAMP;
        }
        /* Named data struct (`constant GammaLUTs*`) — passed as the
         * entry's Block variable itself (shared blockptr type). */
        if (addrspace == 1u || addrspace == 2u) {
            if (pointee_out) *pointee_out = pointee;
            return LAGFX_HPARAM_STRUCTBUF;
        }
        return 0;
    }
    if (pointee < n && ts[pointee].kind == LAGFX_AIR_TYPE_UNKNOWN) {
        /* Opaque resource pointee — texture2d/texture3d (as1) / sampler
         * (as2). texture3d params share the 2D image type: consistent
         * with how the entry declares its texture3d args, so types match
         * (semantically wrong for real 3D sampling — same pre-existing
         * limitation). */
        if (addrspace == 1u) return LAGFX_HPARAM_TEX;
        if (addrspace == 2u) return LAGFX_HPARAM_SAMP;
        return 0;
    }
    if (addrspace == 0u && helper_value_type_ok(c, pointee)) {
        /* `thread T&` — pass a Function temp with copy-in/copy-out. */
        if (pointee_out) *pointee_out = pointee;
        return LAGFX_HPARAM_THREADPTR;
    }
    if ((addrspace == 1u || addrspace == 2u) &&
        helper_value_type_ok(c, pointee)) {
        /* constant/device pointer to a value element — pass through as a
         * StorageBuffer interior pointer (VariablePointersStorageBuffer). */
        if (pointee_out) *pointee_out = pointee;
        return LAGFX_HPARAM_DATABUF;
    }
    return 0;   /* struct-pointee data pointer — deferred */
}

static xlate_helper_fn_t *helpers_find(xlate_helpers_t *h, uint32_t fn_idx) {
    if (!h) return NULL;
    for (uint32_t i = 0; i < h->n_fns; i++)
        if (h->fns[i].fn_idx == fn_idx) return &h->fns[i];
    return NULL;
}

/* Classify + register one helper: emits its OpTypeFunction (deduped —
 * duplicate OpTypeFunction declarations are invalid) and pre-allocates
 * its OpFunction id. An unsupported signature is registered with
 * func_id 0 so the planner doesn't re-visit it. */
static void helpers_register(xlate_ctx_t *c, uint32_t fn_idx) {
    xlate_helpers_t *h = c->helpers;
    if (!h || h->n_fns >= LAGFX_MAX_HELPER_FNS) return;
    if (helpers_find(h, fn_idx)) return;
    xlate_helper_fn_t *hf = &h->fns[h->n_fns++];
    memset(hf, 0, sizeof(*hf));
    hf->fn_idx = fn_idx;

    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(c->m, &n_fns);
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(c->m, &n_types);
    uint32_t fn_ty = fns[fn_idx].type_index;
    if (fn_ty >= n_types || ts[fn_ty].kind != LAGFX_AIR_TYPE_FUNCTION ||
        ts[fn_ty].num_op < 2u)
        return;                                    /* func_id stays 0 */
    uint32_t n_params = ts[fn_ty].num_op - 2u;
    if (n_params > LAGFX_MAX_HELPER_PARAMS) return;
    uint32_t ret_air = ts[fn_ty].op[1];

    if (ret_air < n_types && ts[ret_air].kind == LAGFX_AIR_TYPE_VOID) {
        hf->ret_air = LAGFX_AIR_TYPE_NONE;
        hf->ret_spv = 0u;
    } else if (helper_value_type_ok(c, ret_air)) {
        hf->ret_air = ret_air;
        hf->ret_spv = emit_air_type(c, ret_air);
    } else {
        return;                                    /* unsupported return */
    }

    hf->n_params = (uint8_t)n_params;
    for (uint32_t i = 0; i < n_params; i++) {
        uint32_t air_ty = ts[fn_ty].op[2u + i];
        uint32_t pointee = 0u;
        int kind = classify_helper_param(c, air_ty, &pointee);
        if (!kind) {                               /* unsupported param */
            LAGFX_TRACE("air2spv: helper fn[%u] param %u air_ty %u kind %d "
                        "unsupported — not registered", fn_idx, i, air_ty,
                        air_ty < n_types ? (int)ts[air_ty].kind : -1);
            return;
        }
        hf->param[i].kind   = (uint8_t)kind;
        hf->param[i].air_ty = air_ty;
        switch (kind) {
            case LAGFX_HPARAM_VALUE:
                hf->param[i].spv_ty = emit_air_type(c, air_ty);
                break;
            case LAGFX_HPARAM_TEX:
                hf->param[i].spv_ty = emit_type_ptr_uc_image(c);
                break;
            case LAGFX_HPARAM_SAMP:
                hf->param[i].spv_ty = emit_type_ptr_uc_sampler(c);
                break;
            case LAGFX_HPARAM_THREADPTR: {
                hf->param[i].pointee_air = pointee;
                hf->param[i].pointee_spv = emit_air_type(c, pointee);
                hf->param[i].spv_ty = emit_type_pointer(
                    c, pointee, hf->param[i].pointee_spv,
                    LAGFX_SPV_STORAGE_FUNCTION);
                break;
            }
            case LAGFX_HPARAM_DATABUF: {
                hf->param[i].pointee_air = pointee;
                hf->param[i].pointee_spv = emit_air_type(c, pointee);
                hf->param[i].spv_ty = emit_type_pointer(
                    c, pointee, hf->param[i].pointee_spv,
                    LAGFX_SPV_STORAGE_STORAGE_BUFFER);
                if (!h->cap_varptr) {
                    h->cap_varptr = true;
                    uint32_t cap[] = {
                        LAGFX_SPV_CAPABILITY_VARIABLE_POINTERS_STORAGE_BUFFER };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY,
                                              cap, 1);
                }
                /* OpPtrAccessChain requires ArrayStride on the BASE
                 * pointer type (VUID-StandaloneSpirv-Base-04708); stride =
                 * the element's std430 size. Decorate once per type id. */
                bool done = false;
                for (uint32_t d = 0; d < h->n_stride_decorated; d++)
                    if (h->stride_decorated[d] == hf->param[i].spv_ty)
                        { done = true; break; }
                if (!done) {
                    uint32_t sz = 0, al = 0;
                    if (air_type_size_align(c, pointee, &sz, &al) && sz) {
                        uint32_t dops[] = { hf->param[i].spv_ty,
                                            LAGFX_SPV_DECORATION_ARRAY_STRIDE,
                                            lagfx_round_up(sz, al) };
                        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_DECORATE,
                                                  dops, 3);
                        if (h->n_stride_decorated <
                            sizeof(h->stride_decorated) /
                                sizeof(h->stride_decorated[0]))
                            h->stride_decorated[h->n_stride_decorated++] =
                                hf->param[i].spv_ty;
                    }
                }
                break;
            }
            case LAGFX_HPARAM_STRUCTBUF: {
                hf->param[i].pointee_air = pointee;
                hf->param[i].spv_ty = emit_type_block_ptr(c, pointee);
                if (!hf->param[i].spv_ty) {
                    LAGFX_TRACE("air2spv: helper fn[%u] param %u STRUCTBUF "
                                "pointee %u — Block layout failed", fn_idx,
                                i, pointee);
                    return;
                }
                hf->param[i].pointee_spv = block_struct_spv_id(c, pointee);
                if (!h->cap_varptr) {
                    h->cap_varptr = true;
                    uint32_t cap[] = {
                        LAGFX_SPV_CAPABILITY_VARIABLE_POINTERS_STORAGE_BUFFER };
                    lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_CAPABILITY,
                                              cap, 1);
                }
                break;
            }
        }
    }

    /* OpTypeFunction — dedup against earlier registrations (duplicate
     * non-aggregate type declarations are invalid). */
    uint32_t ret_spv_eff = hf->ret_spv ? hf->ret_spv : emit_type_void(c);
    for (uint32_t e = 0; e + 1u < h->n_fns; e++) {
        const xlate_helper_fn_t *o = &h->fns[e];
        if (!o->func_id || o->n_params != hf->n_params) continue;
        uint32_t o_ret = o->ret_spv ? o->ret_spv : emit_type_void(c);
        if (o_ret != ret_spv_eff) continue;
        bool same = true;
        for (uint32_t i = 0; i < hf->n_params; i++)
            if (o->param[i].spv_ty != hf->param[i].spv_ty) { same = false; break; }
        if (same) { hf->fnty_id = o->fnty_id; break; }
    }
    if (!hf->fnty_id) {
        hf->fnty_id = lagfx_spv_builder_alloc_id(c->b);
        uint32_t ops[2u + LAGFX_MAX_HELPER_PARAMS];
        ops[0] = hf->fnty_id;
        ops[1] = ret_spv_eff;
        for (uint32_t i = 0; i < hf->n_params; i++)
            ops[2u + i] = hf->param[i].spv_ty;
        lagfx_spv_builder_emit_op(c->b, LAGFX_SPV_OP_TYPE_FUNCTION, ops,
                                  2u + hf->n_params);
    }
    hf->func_id = lagfx_spv_builder_alloc_id(c->b);
    {
        const char *nm = lagfx_air_module_string(c->m, fns[fn_idx].name_offset);
        LAGFX_TRACE("air2spv: registered helper fn[%u] '%s' (%u params)",
                    fn_idx, nm ? nm : "?", n_params);
    }
}

/* Mirror emit_inst_call's callee decoding: return the module function-
 * table index a CALL targets, or UINT32_MAX. */
static uint32_t call_callee_fn_index(const xlate_ctx_t *c,
                                     const lagfx_air_inst_t *inst,
                                     uint32_t next_val_id) {
    if (inst->num_ops < 2u) return UINT32_MAX;
    uint32_t ccinfo = (uint32_t)inst->ops[1];
    uint32_t has_fmf = (ccinfo & (1u << 17)) ? 1u : 0u;
    uint32_t callee_slot = 2u + has_fmf + ((ccinfo & 0x8000) ? 1u : 0u);
    if (inst->num_ops <= callee_slot) return UINT32_MAX;
    uint32_t callee_id = resolve_relative((uint32_t)inst->ops[callee_slot],
                                          next_val_id);
    uint32_t n_fns = 0;
    (void)lagfx_air_module_functions(c->m, &n_fns);
    uint32_t n_globalvars = lagfx_air_module_num_globalvars(c->m);
    if (callee_id < n_globalvars || (callee_id - n_globalvars) >= n_fns)
        return UINT32_MAX;
    return callee_id - n_globalvars;
}

static void helpers_plan_fn(xlate_ctx_t *ec, uint32_t fn_idx, int depth);

/* Scan one decoded instruction stream for CALLs to non-proto module
 * functions; plan each callee (post-order: its own callees register
 * FIRST, so registry order is callee-before-caller and emission never
 * needs a forward reference). */
static void helpers_plan_scan(xlate_ctx_t *ec, const lagfx_air_inst_t *insts,
                              uint32_t n_insts, uint32_t inst_id_base,
                              int depth) {
    xlate_helpers_t *h = ec->helpers;
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(ec->m, &n_fns);
    uint32_t entry_fn_idx = (uint32_t)(ec->fn - fns);
    uint32_t next_val = inst_id_base;
    for (uint32_t i = 0; i < n_insts; i++) {
        const lagfx_air_inst_t *inst = &insts[i];
        if (inst->code == LAGFX_AIR_INST_CALL) {
            uint32_t fidx = call_callee_fn_index(ec, inst, next_val);
            if (fidx != UINT32_MAX && fidx != entry_fn_idx &&
                !fns[fidx].is_proto && fns[fidx].body_offset != 0u &&
                !helpers_find(h, fidx)) {
                const char *nm = lagfx_air_module_string(ec->m,
                                                         fns[fidx].name_offset);
                if (nm && strncmp(nm, "air.", 4u) != 0 &&
                    strncmp(nm, "llvm.", 5u) != 0 &&
                    !strstr(nm, "_GLOBAL__sub_I")) {
                    bool visiting = false;
                    for (uint32_t v = 0; v < h->n_visiting; v++)
                        if (h->visiting[v] == fidx) { visiting = true; break; }
                    if (!visiting && depth < 6) {
                        if (h->n_visiting <
                            sizeof(h->visiting) / sizeof(h->visiting[0])) {
                            h->visiting[h->n_visiting++] = fidx;
                            helpers_plan_fn(ec, fidx, depth + 1);
                            h->n_visiting--;
                        }
                        helpers_register(ec, fidx);
                    }
                }
            }
        }
        /* inst_produces_value only consults the module type table via
         * call_return_air_type, so the entry ctx is safe for any body. */
        if (inst_produces_value(ec, inst)) next_val++;
    }
}

/* Open fn_idx's body and plan its callees. */
static void helpers_plan_fn(xlate_ctx_t *ec, uint32_t fn_idx, int depth) {
    lagfx_air_function_body_t *body = NULL;
    if (lagfx_air_function_body_open(ec->m, fn_idx, &body) != LAGFX_OK || !body)
        return;
    uint32_t n_insts = 0;
    const lagfx_air_inst_t *insts = lagfx_air_function_body_instructions(body,
                                                                         &n_insts);
    /* This body's value-id layout (module values shared; args + local
     * consts + results per function). */
    uint32_t n_fns = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(ec->m, &n_fns);
    uint32_t n_types = 0;
    const lagfx_air_type_t *ts = lagfx_air_module_types(ec->m, &n_types);
    uint32_t fn_ty = fns[fn_idx].type_index;
    uint32_t num_args = 0;
    if (fn_ty < n_types && ts[fn_ty].kind == LAGFX_AIR_TYPE_FUNCTION)
        num_args = ts[fn_ty].num_op > 2u ? ts[fn_ty].num_op - 2u : 0u;
    uint32_t n_local = 0;
    (void)lagfx_air_function_body_local_constants(body, &n_local);
    uint32_t inst_id_base = ec->module_val_count + num_args + n_local;

    helpers_plan_scan(ec, insts, n_insts, inst_id_base, depth);
    lagfx_air_function_body_free(body);
}

/* Translate one registered helper into the module as a real SPIR-V
 * function. On body-translation failure the partial function words are
 * rewound and the registry entry stays !emitted (call sites degrade). */
static void translate_one_helper(xlate_ctx_t *ec, uint32_t slot) {
    xlate_helpers_t *h = ec->helpers;
    xlate_helper_fn_t *hf = &h->fns[slot];

    lagfx_spv_funcs_mark_t mark;
    lagfx_spv_builder_funcs_snapshot(ec->b, &mark);

    xlate_ctx_t hc = {0};
    hc.b = ec->b;
    hc.m = ec->m;
    hc.stage = ec->stage;
    hc.is_helper = true;
    copy_module_state(&hc, ec);

    lagfx_status_t st = xlate_ctx_init_fn(&hc, hf->fn_idx);
    if (st != LAGFX_OK) {
        copy_module_state(ec, &hc);
        xlate_ctx_free_fn(&hc);
        return;
    }
    hc.helper_ret_air = hf->ret_air;
    hc.helper_ret_spv = hf->ret_spv;
    hc.helper_self    = hf;

    bind_module_constants(&hc);
    bind_local_constants(&hc);

    /* Function header: OpFunction + OpFunctionParameter* + OpLabel,
     * then the ALLOCA OpVariables and the locals-splice mark. */
    uint32_t retty = hf->ret_spv ? hf->ret_spv : emit_type_void(&hc);
    {
        uint32_t ops[] = { retty, hf->func_id,
                           LAGFX_SPV_FUNCTION_CONTROL_NONE, hf->fnty_id };
        lagfx_spv_builder_emit_op(hc.b, LAGFX_SPV_OP_FUNCTION, ops, 4);
    }
    uint32_t n_hp = hf->n_params;
    if (n_hp > hc.num_args) n_hp = hc.num_args;   /* defensive */
    for (uint32_t i = 0; i < n_hp; i++) {
        uint32_t pid = lagfx_spv_builder_alloc_id(hc.b);
        uint32_t ops[] = { hf->param[i].spv_ty, pid };
        lagfx_spv_builder_emit_op(hc.b, LAGFX_SPV_OP_FUNCTION_PARAMETER, ops, 2);
        uint32_t vid = hc.arg_id_base + i;
        bind_value_spv(&hc, vid, pid);
        if (hc.arg_spv_ids) hc.arg_spv_ids[i] = pid;
        switch (hf->param[i].kind) {
            case LAGFX_HPARAM_VALUE:
                set_value_spv_type(&hc, vid, hf->param[i].spv_ty);
                break;
            case LAGFX_HPARAM_TEX:
                hc.arg_resource_var[i]  = pid;
                hc.arg_resource_kind[i] = 1u;
                set_value_storage(&hc, vid, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT);
                break;
            case LAGFX_HPARAM_SAMP:
                hc.arg_resource_var[i]  = pid;
                hc.arg_resource_kind[i] = 2u;
                set_value_storage(&hc, vid, LAGFX_SPV_STORAGE_UNIFORM_CONSTANT);
                break;
            case LAGFX_HPARAM_THREADPTR:
                set_value_storage(&hc, vid, LAGFX_SPV_STORAGE_FUNCTION);
                set_value_spv_type(&hc, vid, hf->param[i].spv_ty);
                set_result_air_type(&hc, vid, hf->param[i].pointee_air);
                break;
            case LAGFX_HPARAM_DATABUF:
                set_value_storage(&hc, vid, LAGFX_SPV_STORAGE_STORAGE_BUFFER);
                set_value_spv_type(&hc, vid, hf->param[i].spv_ty);
                break;
            case LAGFX_HPARAM_STRUCTBUF:
                /* Struct-Block base: GEPs skip LLVM's leading 0 like any
                 * struct buffer arg (no ptr_arith), so only storage class
                 * and the shared pointer type need tracking. */
                set_value_storage(&hc, vid, LAGFX_SPV_STORAGE_STORAGE_BUFFER);
                set_value_spv_type(&hc, vid, hf->param[i].spv_ty);
                break;
        }
    }
    {
        uint32_t lbl = lagfx_spv_builder_alloc_id(hc.b);
        uint32_t ops[] = { lbl };
        lagfx_spv_builder_emit_op(hc.b, LAGFX_SPV_OP_LABEL, ops, 1);
    }
    emit_alloca_vars(&hc);
    lagfx_spv_builder_mark_locals(hc.b);

    st = translate_body(&hc);
    if (st == LAGFX_OK) {
        hf->emitted = true;
    } else {
        lagfx_spv_builder_funcs_rewind(ec->b, &mark);
        LAGFX_TRACE("air2spv: helper fn[%u] body translation failed (st=%d) "
                    "— rolled back; call sites degrade to typed undef",
                    hf->fn_idx, (int)st);
    }
    copy_module_state(ec, &hc);
    xlate_ctx_free_fn(&hc);
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

    xlate_ctx_t c = {0};
    c.b = lagfx_spv_builder_create(256u);
    if (!c.b) return LAGFX_ERR_OUT_OF_MEMORY;
    c.m = m;
    c.stage = stage;

    lagfx_status_t st = xlate_ctx_init_fn(&c, fn_idx);
    if (st != LAGFX_OK) goto fail;

    /* Module type-id table (shared with every helper ctx). */
    uint32_t n_types = 0;
    (void)lagfx_air_module_types(m, &n_types);
    c.num_air_types = n_types;
    if (n_types > 0u) {
        c.spv_type_ids = (uint32_t *)calloc(n_types, sizeof(uint32_t));
        if (!c.spv_type_ids) goto oom;
    }

    /* Helper-function registry (heap — ~20 KiB). */
    c.helpers = (xlate_helpers_t *)calloc(1u, sizeof(xlate_helpers_t));
    if (!c.helpers) goto oom;

    /* === Emit ============================================================
     * SPIR-V module layout requires:
     *   Capability → ExtInstImport → MemoryModel → EntryPoint →
     *   ExecutionMode → Decorate → Types/Constants/Globals → Functions.
     * The multi-section builder routes each op to its section, so the
     * emission ORDER here is about value-map priming and callee-first
     * function emission, not binary layout. */

    prescan_buffer_arg_access_types(&c);

    emit_prologue(&c);

    /* Module constants: emitted once, bindings recorded for helper ctxs. */
    bind_module_constants(&c);

    /* Plan + translate every non-proto callee reachable from the entry
     * (post-order → callee-first, no forward references). Helper function
     * bodies land in the functions section BEFORE the entry's OpFunction. */
    helpers_plan_scan(&c, c.insts, c.num_insts, c.inst_id_base, 0);
    for (uint32_t i = 0; i < c.helpers->n_fns; i++) {
        if (c.helpers->fns[i].func_id && !c.helpers->fns[i].emitted)
            translate_one_helper(&c, i);
    }
    {
        uint32_t reg = 0, emitted = 0;
        for (uint32_t i = 0; i < c.helpers->n_fns; i++) {
            if (c.helpers->fns[i].func_id) reg++;
            if (c.helpers->fns[i].emitted) emitted++;
        }
        if (c.helpers->n_fns)
            LAGFX_TRACE("air2spv: helpers planned=%u supported=%u emitted=%u",
                        c.helpers->n_fns, reg, emitted);
    }

    /* Entry-function local constants. */
    bind_local_constants(&c);

    emit_module_vars_and_function(&c);

    st = translate_body(&c);
    if (st != LAGFX_OK) goto fail;

    *out_blob = lagfx_spv_builder_finish(c.b, out_size_bytes);
    if (!*out_blob) goto oom;

    lagfx_spv_builder_free(c.b);
    c.b = NULL;
    xlate_ctx_free_fn(&c);
    free(c.spv_type_ids);
    if (c.helpers) {
        free(c.helpers->mod_const_spv);
        free(c.helpers->mod_const_lit);
        free(c.helpers->mod_const_lit_valid);
        free(c.helpers);
    }
    return LAGFX_OK;

oom:
    st = LAGFX_ERR_OUT_OF_MEMORY;
fail:
    if (c.b) lagfx_spv_builder_free(c.b);
    xlate_ctx_free_fn(&c);
    free(c.spv_type_ids);
    if (c.helpers) {
        free(c.helpers->mod_const_spv);
        free(c.helpers->mod_const_lit);
        free(c.helpers->mod_const_lit_valid);
        free(c.helpers);
    }
    return st;
}
