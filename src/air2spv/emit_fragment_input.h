/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.fragment_input (fragment)
 * src/air2spv/emit_fragment_input.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Fourth reference emitter — fragment-stage INPUT shape. Emits a minimal
 * fragment shader that reads a vec2 from Location 0 (UV coordinates),
 * constructs a vec4(uv.x, uv.y, 0.0, 1.0) using OpCompositeExtract +
 * OpCompositeConstruct, and writes it to the color output at Location 0.
 * Demonstrates:
 *
 *   - ExecutionModel Fragment (not Vertex)
 *   - OpExecutionMode OriginUpperLeft (required by Vulkan)
 *   - OpDecorate Location 0 for both Input and Output variables
 *   - OpLoad, OpCompositeExtract (twice), OpCompositeConstruct pattern
 *
 * Sets the pattern for fragment-input emitters.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_FRAGMENT_INPUT_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_FRAGMENT_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_fragment_input_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_FRAGMENT_INPUT_H */
