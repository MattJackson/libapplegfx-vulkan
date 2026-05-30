/*
 * libapplegfx-vulkan — clean-room SPIR-V module builder
 * src/air2spv/spv_builder.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal SPIR-V word-by-word emitter sized for Phase 4 reference
 * emitters. Just enough to assemble a valid Vulkan-acceptable vertex
 * shader module:
 *
 *   - 5-word header (magic / version / generator / bound / schema)
 *   - OpCapability / OpMemoryModel / OpEntryPoint
 *   - OpType* for the small handful we need (void / float / vec4 / ptr / fn)
 *   - OpConstant / OpConstantComposite
 *   - OpVariable + OpDecorate BuiltIn Position
 *   - OpFunction / OpLabel / OpStore / OpReturn / OpFunctionEnd
 *
 * The builder hands out monotonically increasing SSA ids and assembles
 * each opcode as one or more 32-bit words appended to a growable buffer.
 *
 * Reference: SPIR-V Specification §2.3 Physical Layout of a SPIR-V
 *   Module and Instruction; opcode constants from §3.32.
 *
 * Out of scope for Phase 4 step 1: optimizer hooks, debug-info ops
 * (OpSource / OpLine / OpName), proper string emission helpers,
 * fragment-stage support. Add as later emitters need them.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_SPV_BUILDER_H
#define LIBAPPLEGFX_AIR2SPV_SPV_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lagfx_spv_builder lagfx_spv_builder_t;

/* === SPIR-V enum subset ============================================ */

/* SPIR-V opcodes we actually emit. See spirv.h in SPIRV-Headers. */
enum {
    LAGFX_SPV_OP_SOURCE                 = 3,
    LAGFX_SPV_OP_EXT_INST_IMPORT        = 11,
    LAGFX_SPV_OP_EXT_INST               = 12,
    LAGFX_SPV_OP_MEMORY_MODEL           = 14,
    LAGFX_SPV_OP_ENTRY_POINT            = 15,
    LAGFX_SPV_OP_EXECUTION_MODE         = 16,
    LAGFX_SPV_OP_CAPABILITY             = 17,
    LAGFX_SPV_OP_TYPE_VOID              = 19,
    LAGFX_SPV_OP_TYPE_INT               = 21,
    LAGFX_SPV_OP_TYPE_BOOL              = 20,
    LAGFX_SPV_OP_TYPE_FLOAT             = 22,
    LAGFX_SPV_OP_TYPE_VECTOR            = 23,
    LAGFX_SPV_OP_TYPE_IMAGE             = 25,
    LAGFX_SPV_OP_TYPE_SAMPLER           = 26,
    LAGFX_SPV_OP_TYPE_SAMPLED_IMAGE     = 27,
    LAGFX_SPV_OP_TYPE_STRUCT            = 30,
    LAGFX_SPV_OP_TYPE_POINTER           = 32,
    LAGFX_SPV_OP_TYPE_FUNCTION          = 33,
    LAGFX_SPV_OP_CONSTANT               = 43,
    LAGFX_SPV_OP_CONSTANT_COMPOSITE     = 44,
    LAGFX_SPV_OP_FUNCTION               = 54,
    LAGFX_SPV_OP_FUNCTION_END           = 56,
    LAGFX_SPV_OP_VARIABLE               = 59,
    LAGFX_SPV_OP_LOAD                   = 61,
    LAGFX_SPV_OP_STORE                  = 62,
    LAGFX_SPV_OP_ACCESS_CHAIN           = 65,
    LAGFX_SPV_OP_IMAGE_READ             = 98,
    LAGFX_SPV_OP_IMAGE_WRITE            = 99,
    LAGFX_SPV_OP_DECORATE               = 71,
    LAGFX_SPV_OP_MEMBER_DECORATE        = 72,
    LAGFX_SPV_OP_VECTOR_SHUFFLE         = 79,
    LAGFX_SPV_OP_COMPOSITE_CONSTRUCT    = 80,
    LAGFX_SPV_OP_COMPOSITE_EXTRACT      = 81,
    LAGFX_SPV_OP_SAMPLED_IMAGE          = 86,
    LAGFX_SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    /* Conversion instructions (SPIR-V §3.32.11) */
    LAGFX_SPV_OP_CONVERT_F_TO_U         = 109,   /* OpConvertFToU: float→unsigned int (§3.32.11) */
    LAGFX_SPV_OP_CONVERT_F_TO_S         = 110,   /* OpConvertFToS: float→signed int (§3.32.11) */
    LAGFX_SPV_OP_CONVERT_S_TO_F         = 111,   /* OpConvertSToF: signed int→float (§3.32.11) */
    LAGFX_SPV_OP_CONVERT_U_TO_F         = 112,   /* OpConvertUToF: unsigned int→float (§3.32.11) */
    LAGFX_SPV_OP_UCONVERT               = 113,   /* OpUConvert: unsigned int conversion (§3.32.11) */
    LAGFX_SPV_OP_SCONVERT               = 114,   /* OpSConvert: signed int conversion (§3.32.11) */
    LAGFX_SPV_OP_F_CONVERT              = 115,   /* OpFConvert: float conversion (§3.32.11) */
    LAGFX_SPV_OP_BITCAST                = 124,   /* OpBitcast: bit-width/pointer cast (§3.32.11) */
    LAGFX_SPV_OP_IADD                   = 128,
    LAGFX_SPV_OP_ISUB                   = 130,   /* OpISub: integer subtraction (SPIR-V §3.32.2) */
    LAGFX_SPV_OP_IMUL                   = 132,   /* OpIMul: integer multiplication (SPIR-V §3.32.2) */
    /* SPIR-V §3.32.2 (Core 1.0) arithmetic/logic opcodes */
    LAGFX_SPV_OP_FNEGATE                = 127,   /* OpFNegate: unary float negation */
    LAGFX_SPV_OP_FADD                   = 129,   /* OpFAdd: floating-point addition */
    LAGFX_SPV_OP_FSUB                   = 131,   /* OpFSub: floating-point subtraction */
    LAGFX_SPV_OP_FMUL                   = 133,   /* OpFMul: floating-point multiplication */
    LAGFX_SPV_OP_UDIV                   = 134,   /* OpUDiv: unsigned integer division */
    LAGFX_SPV_OP_SDIV                   = 135,   /* OpSDiv: signed integer division */
    LAGFX_SPV_OP_FDIV                   = 136,   /* OpFDiv: floating-point division */
    LAGFX_SPV_OP_UMOD                   = 137,   /* OpUMod: unsigned integer modulo */
    LAGFX_SPV_OP_SREM                   = 138,   /* OpSRem: signed integer remainder */
    LAGFX_SPV_OP_SMOD                   = 139,   /* OpSMod: signed integer modulo */
    LAGFX_SPV_OP_FREM                   = 140,   /* OpFRem: floating-point remainder */
    LAGFX_SPV_OP_FMOD                   = 141,   /* OpFMod: floating-point modulo */
    LAGFX_SPV_OP_VECTOR_TIMES_SCALAR    = 142,   /* OpVectorTimesScalar */
    LAGFX_SPV_OP_DOT                    = 148,   /* OpDot: vector dot product (scalar result) */
    /* Relational/comparison opcodes (SPIR-V §3.32.15). Values are the
     * canonical SPIR-V core opcode numbers (verified against
     * spirv/unified1/spirv.h — SpvOp* enumerators). The float ops are a
     * strictly-interleaved Ord/Unord pair sequence 180..191; do NOT
     * eyeball these — every value below is the spec number. */
    LAGFX_SPV_OP_ORDERED                = 162,   /* OpOrdered (§3.32.15) */
    LAGFX_SPV_OP_UNORDERED              = 163,   /* OpUnordered (§3.32.15) */
    LAGFX_SPV_OP_IEQUAL                 = 170,   /* OpIEqual (§3.32.15) */
    LAGFX_SPV_OP_INOT_EQUAL             = 171,   /* OpINotEqual (§3.32.15) */
    LAGFX_SPV_OP_UGREATER_THAN          = 172,   /* OpUGreaterThan (§3.32.15) */
    LAGFX_SPV_OP_SGREATER_THAN          = 173,   /* OpSGreaterThan (§3.32.15) */
    LAGFX_SPV_OP_UGREATER_EQUAL         = 174,   /* OpUGreaterThanEqual (§3.32.15) */
    LAGFX_SPV_OP_SGREATER_EQUAL         = 175,   /* OpSGreaterThanEqual (§3.32.15) */
    LAGFX_SPV_OP_ULESS_THAN             = 176,   /* OpULessThan (§3.32.15) */
    LAGFX_SPV_OP_SLESS_THAN             = 177,   /* OpSLessThan (§3.32.15) */
    LAGFX_SPV_OP_ULESS_EQUAL            = 178,   /* OpULessThanEqual (§3.32.15) */
    LAGFX_SPV_OP_SLESS_EQUAL            = 179,   /* OpSLessThanEqual (§3.32.15) */
    LAGFX_SPV_OP_FORD_EQUAL             = 180,   /* OpFOrdEqual (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_EQUAL           = 181,   /* OpFUnordEqual (§3.32.15) */
    LAGFX_SPV_OP_FORD_NOT_EQUAL         = 182,   /* OpFOrdNotEqual (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_NOT_EQUAL       = 183,   /* OpFUnordNotEqual (§3.32.15) */
    LAGFX_SPV_OP_FORD_LESS_THAN         = 184,   /* OpFOrdLessThan (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_LESS_THAN       = 185,   /* OpFUnordLessThan (§3.32.15) */
    LAGFX_SPV_OP_FORD_GREATER_THAN      = 186,   /* OpFOrdGreaterThan (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_GREATER_THAN    = 187,   /* OpFUnordGreaterThan (§3.32.15) */
    LAGFX_SPV_OP_FORD_LESS_EQUAL        = 188,   /* OpFOrdLessThanEqual (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_LESS_EQUAL      = 189,   /* OpFUnordLessThanEqual (§3.32.15) */
    LAGFX_SPV_OP_FORD_GREATER_EQUAL     = 190,   /* OpFOrdGreaterThanEqual (§3.32.15) */
    LAGFX_SPV_OP_FUNORD_GREATER_EQUAL   = 191,   /* OpFUnordGreaterThanEqual (§3.32.15) */
    LAGFX_SPV_OP_SELECT                 = 169,   /* OpSelect (§3.32.16) */
    /* Composite instructions (SPIR-V §3.32.12). */
    LAGFX_SPV_OP_VECTOR_EXTRACT_DYNAMIC = 77,  /* OpVectorExtractDynamic (§3.32.12) */
    LAGFX_SPV_OP_VECTOR_INSERT_DYNAMIC  = 78,  /* OpVectorInsertDynamic (§3.32.12) */
    /* Shift opcodes (SPIR-V §3.32.14). Canonical SPIR-V numbers:
     * ShiftRightLogical=194, ShiftRightArithmetic=195, ShiftLeftLogical=196.
     * (Pre-2026-05-29 these were rotated-wrong — shl emitted ShiftRightLogical.) */
    LAGFX_SPV_OP_SHIFT_RIGHT_LOGICAL    = 194,   /* OpShiftRightLogical */
    LAGFX_SPV_OP_SHIFT_RIGHT_ARITHMETIC = 195,   /* OpShiftRightArithmetic */
    LAGFX_SPV_OP_SHIFT_LEFT_LOGICAL     = 196,   /* OpShiftLeftLogical */
    /* Bitwise opcodes (SPIR-V §3.32.14). OpBitwiseOr=197, Xor=198, And=199.
     * (Pre-2026-05-29 OR/XOR were 200/201 = OpNot/OpBitFieldInsert.) */
    LAGFX_SPV_OP_BITWISE_OR             = 197,   /* OpBitwiseOr */
    LAGFX_SPV_OP_BITWISE_XOR            = 198,   /* OpBitwiseXor */
    LAGFX_SPV_OP_BITWISE_AND            = 199,   /* OpBitwiseAnd */
    LAGFX_SPV_OP_PHI                    = 245,
    LAGFX_SPV_OP_LOOP_MERGE             = 246,
    LAGFX_SPV_OP_SELECTION_MERGE        = 247,
    LAGFX_SPV_OP_LABEL                  = 248,
    LAGFX_SPV_OP_BRANCH                 = 249,
    LAGFX_SPV_OP_BRANCH_CONDITIONAL     = 250,
    LAGFX_SPV_OP_RETURN                 = 253,
};

/* Capabilities. */
enum {
    LAGFX_SPV_CAPABILITY_SHADER                     = 1,
    /* NOTE: SPIR-V Capability ID 3 is `Tessellation`, NOT "Image" —
     * basic image ops are covered by `Shader` and need no separate
     * capability. (Paid for 2026-05-27: a freshman session erroneously
     * declared `Capability::Image = 3`, which would have requested
     * Tessellation. `StorageImageExtendedFormats` below is the only
     * image-related capability we currently need, and only for storage
     * images with concrete format like Rgba8.) */
    LAGFX_SPV_CAPABILITY_STORAGE_IMAGE_EXTENDED_FORMATS = 49,

    /* Float16 / Int16 / Int8 support (Metal shaders use `half` + i16/i8). */
    LAGFX_SPV_CAPABILITY_FLOAT16   = 9,
    LAGFX_SPV_CAPABILITY_INT64     = 11,
    LAGFX_SPV_CAPABILITY_INT16     = 22,
    LAGFX_SPV_CAPABILITY_INT8      = 39,
};

/* Addressing & memory model. */
enum {
    LAGFX_SPV_ADDRESSING_MODEL_LOGICAL  = 0,
    LAGFX_SPV_MEMORY_MODEL_GLSL450      = 1,
};

/* Execution models for OpEntryPoint. */
enum {
    LAGFX_SPV_EXECUTION_MODEL_VERTEX        = 0,
    LAGFX_SPV_EXECUTION_MODEL_FRAGMENT      = 4,
    LAGFX_SPV_EXECUTION_MODEL_GLCOMPUTE     = 5,
};

/* ExecutionMode — for OpExecutionMode. Fragment stages require
 * OriginUpperLeft (Vulkan convention). Compute requires LocalSize. */
enum {
    LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT = 7,
    LAGFX_SPV_EXECUTION_MODE_LOCAL_SIZE      = 17,
};

/* Storage classes for OpTypePointer / OpVariable. */
enum {
    LAGFX_SPV_STORAGE_UNIFORM_CONSTANT  = 0,  /* opaque types: image/sampler (sampled images) */
    LAGFX_SPV_STORAGE_INPUT             = 1,
    LAGFX_SPV_STORAGE_UNIFORM            = 2,
    LAGFX_SPV_STORAGE_OUTPUT            = 3,
    /* NOTE: SPIR-V Storage Class ID 4 is `Workgroup`, NOT
     * `StorageBuffer`. Storage images in Vulkan compute use
     * `UniformConstant` (id 0); storage buffers use `Uniform` (id 2)
     * pre-SPIR-V 1.3 or `StorageBuffer` (id 12) post-1.3 with the
     * SpvCapabilityStorageBuffer capability. Paid for 2026-05-27. */
    LAGFX_SPV_STORAGE_FUNCTION          = 7,
};

/* OpTypeImage `Dim` operand. */
enum {
    LAGFX_SPV_DIM_2D                    = 1,
};

/* OpTypeImage `ImageFormat` operand — Unknown = no storage-image
 * binding format declared (sampled-only image). Concrete formats required
 * for storage-image OpImageRead/OpImageWrite. */
enum {
    LAGFX_SPV_IMAGE_FORMAT_UNKNOWN      = 0,
    LAGFX_SPV_IMAGE_FORMAT_RGBA8        = 4,
};

/* Decorations. */
enum {
    LAGFX_SPV_DECORATION_BLOCK          = 2,
    LAGFX_SPV_DECORATION_BUILTIN        = 11,
    LAGFX_SPV_DECORATION_LOCATION       = 30,
    LAGFX_SPV_DECORATION_BINDING        = 33,
    LAGFX_SPV_DECORATION_DESCRIPTOR_SET = 34,
    LAGFX_SPV_DECORATION_OFFSET         = 35,
};

/* BuiltIn variants — only the ones Phase 4 currently needs. */
enum {
    LAGFX_SPV_BUILTIN_POSITION          = 0,
    LAGFX_SPV_BUILTIN_VERTEX_INDEX      = 42,

    /* Freshman Task — BuiltIn enum constants (round 2) */
    LAGFX_SPV_BUILTIN_FRAG_COORD        = 15,   /* SpvBuiltInFragCoord */
    LAGFX_SPV_BUILTIN_LAYER             = 9,    /* SpvBuiltInLayer */
    LAGFX_SPV_BUILTIN_VIEWPORT_INDEX    = 10,   /* SpvBuiltInViewportIndex */
    LAGFX_SPV_BUILTIN_GLOBAL_INVOCATION_ID = 28,  /* SpvBuiltInGlobalInvocationId */
};

/* OpFunction `FunctionControl` mask — None is what we want for shaders. */
enum {
    LAGFX_SPV_FUNCTION_CONTROL_NONE     = 0,
};

/* OpSelectionMerge `SelectionControl` mask — None is the default. */
enum {
    LAGFX_SPV_SELECTION_CONTROL_NONE    = 0,
};

/* OpLoopMerge `LoopControl` mask — None is the default. */
enum {
    LAGFX_SPV_LOOP_CONTROL_NONE         = 0,
};

/* GLSL.std.450 extended-instruction-set numbers (subset). Use with
 * OpExtInst after importing the "GLSL.std.450" extension. Full list:
 * https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html */
enum {
    LAGFX_SPV_GLSL_ROUND                = 1,
    LAGFX_SPV_GLSL_FABS                 = 4,
    LAGFX_SPV_GLSL_FLOOR                = 8,
    LAGFX_SPV_GLSL_CEIL                 = 9,
    LAGFX_SPV_GLSL_FRACT                = 10,
    LAGFX_SPV_GLSL_SIN                  = 13,
    LAGFX_SPV_GLSL_COS                  = 14,
    LAGFX_SPV_GLSL_POW                  = 26,
    LAGFX_SPV_GLSL_EXP                  = 27,
    LAGFX_SPV_GLSL_LOG                  = 28,
    LAGFX_SPV_GLSL_SQRT                 = 31,
    LAGFX_SPV_GLSL_INVERSE_SQRT         = 32,
    LAGFX_SPV_GLSL_FMIN                 = 37,
    LAGFX_SPV_GLSL_FMAX                 = 40,
    LAGFX_SPV_GLSL_FCLAMP               = 43,
    LAGFX_SPV_GLSL_FMIX                 = 46,
    LAGFX_SPV_GLSL_STEP                 = 48,
    LAGFX_SPV_GLSL_SMOOTH_STEP          = 49,
    LAGFX_SPV_GLSL_LENGTH               = 66,
    LAGFX_SPV_GLSL_DISTANCE             = 67,
    LAGFX_SPV_GLSL_CROSS                = 68,
    LAGFX_SPV_GLSL_NORMALIZE            = 69,
    LAGFX_SPV_GLSL_REFLECT              = 71,
    /* Round 3: trig + misc (verified vs GLSL.std.450.h, §3.32 GLSL.std.450) */
    LAGFX_SPV_GLSL_TRUNC                = 3,    /* GLSLstd450Trunc */
    LAGFX_SPV_GLSL_FSIGN                = 6,    /* GLSLstd450FSign */
    LAGFX_SPV_GLSL_RADIANS              = 11,   /* GLSLstd450Radians */
    LAGFX_SPV_GLSL_DEGREES              = 12,   /* GLSLstd450Degrees */
    LAGFX_SPV_GLSL_TAN                  = 15,   /* GLSLstd450Tan */
    LAGFX_SPV_GLSL_ASIN                 = 16,   /* GLSLstd450Asin */
    LAGFX_SPV_GLSL_ACOS                 = 17,   /* GLSLstd450Acos */
    LAGFX_SPV_GLSL_ATAN                 = 18,   /* GLSLstd450Atan */
    LAGFX_SPV_GLSL_SINH                 = 19,   /* GLSLstd450Sinh */
    LAGFX_SPV_GLSL_COSH                 = 20,   /* GLSLstd450Cosh */
    LAGFX_SPV_GLSL_TANH                 = 21,   /* GLSLstd450Tanh */
    LAGFX_SPV_GLSL_ATAN2                = 25,   /* GLSLstd450Atan2 */
    LAGFX_SPV_GLSL_EXP2                 = 29,   /* GLSLstd450Exp2 */
    LAGFX_SPV_GLSL_LOG2                 = 30,   /* GLSLstd450Log2 */
    LAGFX_SPV_GLSL_LDEXP                = 53,   /* GLSLstd450Ldexp */
    LAGFX_SPV_GLSL_REFRACT              = 72,   /* GLSLstd450Refract */
};

/* === Builder lifecycle ============================================= */

/* Create a fresh builder with initial capacity (in 32-bit words). The
 * builder owns all word buffers; release with lagfx_spv_builder_free. */
lagfx_spv_builder_t *lagfx_spv_builder_create(uint32_t initial_word_capacity);
void                 lagfx_spv_builder_free(lagfx_spv_builder_t *b);

/* Allocate a fresh SSA id. Ids are monotonically increasing starting
 * at 1 (id 0 is reserved per SPIR-V spec). */
uint32_t lagfx_spv_builder_alloc_id(lagfx_spv_builder_t *b);

/* Append one raw 32-bit word to the module. Returns false on OOM. */
bool lagfx_spv_builder_emit_word(lagfx_spv_builder_t *b, uint32_t w);

/* Emit a complete instruction in one call. `wc_op` is the SPIR-V opcode
 * (low 16 bits); the high 16 bits encode word count and are set
 * automatically as (1 + num_operands). `operands` is a NULL-allowed
 * pointer; pass num_operands=0 if the op has no operands.
 *
 * Returns false on OOM. */
bool lagfx_spv_builder_emit_op(lagfx_spv_builder_t *b,
                                uint32_t opcode,
                                const uint32_t *operands,
                                uint32_t num_operands);

/* Emit an instruction whose payload includes a UTF-8 string operand
 * (used by OpEntryPoint, OpSource, OpName, etc.). The string is packed
 * 4 bytes per word, little-endian, with a trailing NUL aligned up to
 * the next 4-byte boundary (so a 4-char string takes 2 words: ['nul','m','a','i','n'] becomes
 * 'main' + '\0\0\0\0'; the trailing NUL word is mandatory).
 *
 * `prefix_operands` are emitted BEFORE the string; `suffix_operands`
 * AFTER. For OpEntryPoint Vertex %main "main" %iface:
 *   prefix = [VERTEX, main_id], string = "main", suffix = [iface_id...]
 */
bool lagfx_spv_builder_emit_op_string(lagfx_spv_builder_t *b,
                                       uint32_t opcode,
                                       const uint32_t *prefix_operands,
                                       uint32_t        num_prefix,
                                       const char     *str,
                                       const uint32_t *suffix_operands,
                                       uint32_t        num_suffix);

/* === Module finalization =========================================== */

/* Finalize: prepends the 5-word SPIR-V header to the accumulated body,
 * returning a malloc'd buffer that the caller owns. *out_size_bytes is
 * set to the total module size in bytes. Returns NULL on OOM. The
 * builder remains usable afterwards. */
uint8_t *lagfx_spv_builder_finish(const lagfx_spv_builder_t *b,
                                   size_t                   *out_size_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_SPV_BUILDER_H */
