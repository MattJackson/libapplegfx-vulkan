/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * Unary float ops (OpFNegate + OpExtInst FAbs).
 * src/air2spv/emit_unop.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Eleventh reference emitter. Demonstrates two unary float patterns
 * in a single fragment shader:
 *   - OpFNegate (SPIR-V core, raw_code=56) for air.fast_math_negate
 *   - OpExtInst FAbs (GLSL.std.450 #4) for air.fabs
 * Combined with OpFAdd so spirv-val sees both results used.
 *
 * Output: fragment shader that computes on vec4(-0.4, 0.3, -0.2, 1.0):
 *   neg = OpFNegate in    -> (0.4, -0.3, 0.2, -1.0)
 *   abs = OpExtInst FAbs in -> (0.4, 0.3, 0.2, 1.0)
 *   sum = OpFAdd neg abs  -> (0.8, 0.0, 0.4, 0.0)
 *   OpStore %color_out sum
 *
 * Validates clean under `spirv-val`.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_UNOP_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_UNOP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_unop_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_UNOP_H */
