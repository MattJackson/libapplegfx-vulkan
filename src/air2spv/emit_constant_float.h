/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for
 * OpConstantComposite with multiple distinct float operands.
 * src/air2spv/emit_constant_float.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Sixth reference emitter. Demonstrates building a vec4 const from
 * four DISTINCT scalar float constants (vs. emit_render_target which
 * uses only 0.0 / 1.0). This is the path AIR shaders emitting things
 * like clear-color literals or per-channel scale factors land in.
 *
 * Output: fragment shader writing `vec4(0.2, 0.5, 0.8, 1.0)` to
 * Location 0.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_CONSTANT_FLOAT_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_CONSTANT_FLOAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_constant_float_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_CONSTANT_FLOAT_H */
