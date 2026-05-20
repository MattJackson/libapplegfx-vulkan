/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.render_target (fragment)
 * src/air2spv/emit_render_target.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Third reference emitter — fragment-stage shape. Emits a minimal
 * fragment shader that writes a constant vec4(1, 0, 0, 1) (red) to
 * the color attachment at location 0. Demonstrates:
 *
 *   - ExecutionModel Fragment (not Vertex)
 *   - OpExecutionMode OriginUpperLeft (required by Vulkan)
 *   - OpDecorate Location 0 (not BuiltIn)
 *
 * Sets the pattern for follow-on fragment-stage emitters.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_RENDER_TARGET_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_RENDER_TARGET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_render_target_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_RENDER_TARGET_H */
