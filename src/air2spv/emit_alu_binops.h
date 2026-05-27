/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter
 * (Binary ALU ops: OpFAdd, OpFSub, OpFDiv, OpDot).
 * src/air2spv/emit_alu_binops.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_ALU_BINOPS_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_ALU_BINOPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_alu_binops_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_ALU_BINOPS_H */
