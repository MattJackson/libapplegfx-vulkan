/*
 * libapplegfx-vulkan — SPIR-V relational/shift/bitwise opcode pinning
 * tests/air2spv-relational-opcodes-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Compile-time guard: pins every comparison (§3.32.15), shift and
 * bitwise (§3.32.14/§3.32.2) opcode constant in spv_builder.h to its
 * canonical SPIR-V core opcode number.
 *
 * WHY THIS EXISTS: a freshman cycle (CMP/CMP2 dispatch) shipped ~22
 * fabricated opcode values (e.g. OpIEqual written as 178 instead of
 * 170; OpINotEqual as 197 = actually OpBitwiseOr) while citing the
 * spec in every comment. The values went undetected because no test
 * fixture exercised CMP, so spirv-val never saw the opcodes. These
 * _Static_assert lines make a wrong opcode value a BUILD failure, no
 * fixture required.
 *
 * The canonical values below are the SpvOp* enumerators from
 * spirv/unified1/spirv.h (Khronos SPIR-V headers). Do NOT "simplify"
 * by deleting these — they are the second independent transcription
 * that cross-checks spv_builder.h.
 */
#include "air2spv/spv_builder.h"

#define PIN(name, val) \
    _Static_assert((name) == (val), #name " must be the canonical SPIR-V opcode " #val)

/* Comparison / relational — SPIR-V §3.32.15 */
PIN(LAGFX_SPV_OP_ORDERED,              162);
PIN(LAGFX_SPV_OP_UNORDERED,            163);
PIN(LAGFX_SPV_OP_IEQUAL,               170);
PIN(LAGFX_SPV_OP_INOT_EQUAL,           171);
PIN(LAGFX_SPV_OP_UGREATER_THAN,        172);
PIN(LAGFX_SPV_OP_SGREATER_THAN,        173);
PIN(LAGFX_SPV_OP_UGREATER_EQUAL,       174);
PIN(LAGFX_SPV_OP_SGREATER_EQUAL,       175);
PIN(LAGFX_SPV_OP_ULESS_THAN,           176);
PIN(LAGFX_SPV_OP_SLESS_THAN,           177);
PIN(LAGFX_SPV_OP_ULESS_EQUAL,          178);
PIN(LAGFX_SPV_OP_SLESS_EQUAL,          179);
PIN(LAGFX_SPV_OP_FORD_EQUAL,           180);
PIN(LAGFX_SPV_OP_FUNORD_EQUAL,         181);
PIN(LAGFX_SPV_OP_FORD_NOT_EQUAL,       182);
PIN(LAGFX_SPV_OP_FUNORD_NOT_EQUAL,     183);
PIN(LAGFX_SPV_OP_FORD_LESS_THAN,       184);
PIN(LAGFX_SPV_OP_FUNORD_LESS_THAN,     185);
PIN(LAGFX_SPV_OP_FORD_GREATER_THAN,    186);
PIN(LAGFX_SPV_OP_FUNORD_GREATER_THAN,  187);
PIN(LAGFX_SPV_OP_FORD_LESS_EQUAL,      188);
PIN(LAGFX_SPV_OP_FUNORD_LESS_EQUAL,    189);
PIN(LAGFX_SPV_OP_FORD_GREATER_EQUAL,   190);
PIN(LAGFX_SPV_OP_FUNORD_GREATER_EQUAL, 191);

/* Shift — SPIR-V §3.32.14 */
PIN(LAGFX_SPV_OP_SHIFT_RIGHT_LOGICAL,    194);
PIN(LAGFX_SPV_OP_SHIFT_RIGHT_ARITHMETIC, 195);
PIN(LAGFX_SPV_OP_SHIFT_LEFT_LOGICAL,     196);

/* Bitwise — SPIR-V §3.32.14 */
PIN(LAGFX_SPV_OP_BITWISE_OR,  197);
PIN(LAGFX_SPV_OP_BITWISE_XOR, 198);
PIN(LAGFX_SPV_OP_BITWISE_AND, 199);

/* Composite instructions (SPIR-V §3.32.16). */
PIN(LAGFX_SPV_OP_SELECT, 169);

/* Composite static index (SPIR-V §3.32.12). */
PIN(LAGFX_SPV_OP_COMPOSITE_EXTRACT, 81);

/* Composite dynamic index — SPIR-V §3.32.12. */
PIN(LAGFX_SPV_OP_VECTOR_EXTRACT_DYNAMIC, 77);
PIN(LAGFX_SPV_OP_VECTOR_INSERT_DYNAMIC,  78);

/* Memory (SPIR-V §3.32.8) */
PIN(LAGFX_SPV_OP_ACCESS_CHAIN, 65);

/* BuiltIn decorations (SPIR-V §3.21) */
PIN(LAGFX_SPV_BUILTIN_FRAG_COORD,           15);
PIN(LAGFX_SPV_BUILTIN_LAYER,                9);
PIN(LAGFX_SPV_BUILTIN_VIEWPORT_INDEX,       10);
PIN(LAGFX_SPV_BUILTIN_GLOBAL_INVOCATION_ID, 28);

/* Capabilities (SPIR-V §3.31) */
PIN(LAGFX_SPV_CAPABILITY_FLOAT16, 9);
PIN(LAGFX_SPV_CAPABILITY_INT64,   11);
PIN(LAGFX_SPV_CAPABILITY_INT16,   22);
PIN(LAGFX_SPV_CAPABILITY_INT8,    39);

/* GLSL.std.450 ext-inst (round 3) — verified vs GLSL.std.450.h */
PIN(LAGFX_SPV_GLSL_TRUNC,    3);
PIN(LAGFX_SPV_GLSL_FSIGN,    6);
PIN(LAGFX_SPV_GLSL_RADIANS,  11);
PIN(LAGFX_SPV_GLSL_DEGREES,  12);
PIN(LAGFX_SPV_GLSL_TAN,      15);
PIN(LAGFX_SPV_GLSL_ASIN,     16);
PIN(LAGFX_SPV_GLSL_ACOS,     17);
PIN(LAGFX_SPV_GLSL_ATAN,     18);
PIN(LAGFX_SPV_GLSL_SINH,     19);
PIN(LAGFX_SPV_GLSL_COSH,     20);
PIN(LAGFX_SPV_GLSL_TANH,     21);
PIN(LAGFX_SPV_GLSL_ATAN2,    25);
PIN(LAGFX_SPV_GLSL_EXP2,     29);
PIN(LAGFX_SPV_GLSL_LOG2,     30);
PIN(LAGFX_SPV_GLSL_LDEXP,    53);
PIN(LAGFX_SPV_GLSL_REFRACT,  72);

/* Arithmetic — SPIR-V §3.32.13. Added 2026-05-30 after a real macOS
 * 15.7.5 fragment shader exposed FREM=137 (actually OpUMod) and
 * UMOD=138 (actually OpSRem) — a fabricated-opcode regression that
 * shipped because nothing pinned these and no fixture hit FRem/UMod.
 * Values are SpvOp* from spirv/unified1/spirv.h. */
PIN(LAGFX_SPV_OP_IADD, 128);
PIN(LAGFX_SPV_OP_FADD, 129);
PIN(LAGFX_SPV_OP_ISUB, 130);
PIN(LAGFX_SPV_OP_FSUB, 131);
PIN(LAGFX_SPV_OP_IMUL, 132);
PIN(LAGFX_SPV_OP_FMUL, 133);
PIN(LAGFX_SPV_OP_UDIV, 134);
PIN(LAGFX_SPV_OP_SDIV, 135);
PIN(LAGFX_SPV_OP_FDIV, 136);
PIN(LAGFX_SPV_OP_UMOD, 137);
PIN(LAGFX_SPV_OP_SREM, 138);
PIN(LAGFX_SPV_OP_SMOD, 139);
PIN(LAGFX_SPV_OP_FREM, 140);
PIN(LAGFX_SPV_OP_FMOD, 141);

/* An already-trusted neighbour, as a sanity anchor that the enum block
 * as a whole is on the right number line. */
PIN(LAGFX_SPV_OP_DOT, 148);

int main(void) {
    /* All checks are compile-time; reaching here means they passed. */
    return 0;
}
