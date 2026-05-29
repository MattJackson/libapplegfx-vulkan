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

/* An already-trusted neighbour, as a sanity anchor that the enum block
 * as a whole is on the right number line. (OpSelect=169 will join here
 * once the SELECT/VSELECT cycle lands its constant.) */
PIN(LAGFX_SPV_OP_DOT, 148);

int main(void) {
    /* All checks are compile-time; reaching here means they passed. */
    return 0;
}
