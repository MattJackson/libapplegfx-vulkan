/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * OpExtInst GLSL.std.450 demo (Pattern G).
 * src/air2spv/emit_extinst_glsl.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Eighth reference emitter. Lands Pattern G — OpExtInstImport +
 * OpExtInst against the GLSL.std.450 extended instruction set.
 * That single mechanism unlocks every AIR math intrinsic family:
 *   air.sin / air.cos / air.tan / air.atan2 / air.exp / air.log
 *   air.sqrt / air.rsqrt / air.pow
 *   air.fmin / air.fmax / air.clamp / air.mix / air.smoothstep
 *   air.length / air.distance / air.cross / air.normalize / air.reflect
 *   ...and ~30 others.
 *
 * Output: vertex shader that computes
 *   sqrt(vec4(4.0, 9.0, 16.0, 1.0)) -> vec4(2.0, 3.0, 4.0, 1.0)
 * via OpExtInst GLSL.std.450 Sqrt, and writes the result to
 * BuiltIn Position.
 *
 * Validates clean under `spirv-val`.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_EXTINST_GLSL_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_EXTINST_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_extinst_glsl_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_EXTINST_GLSL_H */
