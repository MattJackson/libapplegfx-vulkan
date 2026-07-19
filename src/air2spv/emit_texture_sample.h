/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for the
 * separate image+sampler texture-sample shape (Pattern F).
 * src/air2spv/emit_texture_sample.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Seventh reference emitter. Lands Pattern F: a fragment shader that
 * declares a separate 2D `texture2D` + `sampler` (not a combined
 * sampler2D — Metal AIR uses separate-binding convention which maps
 * directly onto Vulkan's two-descriptor model), combines them at use
 * site via OpSampledImage, and samples with implicit LOD using a
 * per-fragment vec2 UV input.
 *
 * Pipeline shape:
 *   Input: vec2 uv (Location 0)
 *   UBO  : (none — texture sampling alone)
 *   Texture:  set 0 binding 0 = 2D image
 *   Sampler:  set 0 binding 1
 *   Output: vec4 color (Location 0)
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 *
 * Why separate Image + Sampler (not combined): Metal's MSL emits
 * `texture2d<float>` and `sampler` as distinct bindings, and AIR's
 * `air.sample_texture` intrinsic takes them as separate operands.
 * Matching that convention in our reference makes the per-intrinsic
 * emitter that lowers air.sample_texture trivial — load the two
 * variables, OpSampledImage them, OpImageSampleImplicitLod.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_TEXTURE_SAMPLE_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_TEXTURE_SAMPLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_texture_sample_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_TEXTURE_SAMPLE_H */
