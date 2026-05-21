/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for OpVectorShuffle.
 * src/air2spv/emit_vector_shuffle.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Ninth reference emitter. Lands Pattern H — OpVectorShuffle for
 * Metal/HLSL-style vector swizzles (.xyz, .rrr, .bgra, etc.).
 *
 * Output: fragment shader that takes the constant vec4(0.2, 0.5, 0.8, 1.0)
 * and shuffles it to vec4(1.0, 0.8, 0.5, 0.2) (i.e., .wzyx — reversed)
 * via OpVectorShuffle, writing the result to Location 0.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_VECTOR_SHUFFLE_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_VECTOR_SHUFFLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_vector_shuffle_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_VECTOR_SHUFFLE_H */
