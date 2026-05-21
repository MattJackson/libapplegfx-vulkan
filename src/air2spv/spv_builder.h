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
    LAGFX_SPV_OP_DECORATE               = 71,
    LAGFX_SPV_OP_MEMBER_DECORATE        = 72,
    LAGFX_SPV_OP_VECTOR_SHUFFLE         = 79,
    LAGFX_SPV_OP_COMPOSITE_CONSTRUCT    = 80,
    LAGFX_SPV_OP_COMPOSITE_EXTRACT      = 81,
    LAGFX_SPV_OP_SAMPLED_IMAGE          = 86,
    LAGFX_SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    LAGFX_SPV_OP_CONVERT_U_TO_F         = 112,
    LAGFX_SPV_OP_FMUL                   = 133,
    LAGFX_SPV_OP_LABEL                  = 248,
    LAGFX_SPV_OP_RETURN                 = 253,
};

/* Capabilities. */
enum {
    LAGFX_SPV_CAPABILITY_SHADER         = 1,
};

/* Addressing & memory model. */
enum {
    LAGFX_SPV_ADDRESSING_MODEL_LOGICAL  = 0,
    LAGFX_SPV_MEMORY_MODEL_GLSL450      = 1,
};

/* Execution models for OpEntryPoint. */
enum {
    LAGFX_SPV_EXECUTION_MODEL_VERTEX    = 0,
    LAGFX_SPV_EXECUTION_MODEL_FRAGMENT  = 4,
};

/* ExecutionMode — for OpExecutionMode. Fragment stages require
 * OriginUpperLeft (Vulkan convention). */
enum {
    LAGFX_SPV_EXECUTION_MODE_ORIGIN_UPPER_LEFT = 7,
};

/* Storage classes for OpTypePointer / OpVariable. */
enum {
    LAGFX_SPV_STORAGE_UNIFORM_CONSTANT  = 0,  /* opaque types: image / sampler */
    LAGFX_SPV_STORAGE_INPUT             = 1,
    LAGFX_SPV_STORAGE_UNIFORM            = 2,
    LAGFX_SPV_STORAGE_OUTPUT            = 3,
    LAGFX_SPV_STORAGE_FUNCTION          = 7,
};

/* OpTypeImage `Dim` operand. */
enum {
    LAGFX_SPV_DIM_2D                    = 1,
};

/* OpTypeImage `ImageFormat` operand — Unknown = no storage-image
 * binding format declared (sampled-only image). */
enum {
    LAGFX_SPV_IMAGE_FORMAT_UNKNOWN      = 0,
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
};

/* OpFunction `FunctionControl` mask — None is what we want for shaders. */
enum {
    LAGFX_SPV_FUNCTION_CONTROL_NONE     = 0,
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
