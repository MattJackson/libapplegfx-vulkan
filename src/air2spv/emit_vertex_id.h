/*
 * libapplegfx-vulkan — Phase 4 reference emitter: air.vertex_id input
 * src/air2spv/emit_vertex_id.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Second reference emitter after air.position. Same minimal shape but
 * for an INPUT variable with BuiltIn VertexIndex decoration — the
 * SPIR-V equivalent of Metal's [[vertex_id]] attribute. Sets the
 * pattern for follow-on input-side builtin emitters (instance_id,
 * sample_id, etc.).
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_VERTEX_ID_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_VERTEX_ID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emit a minimal SPIR-V vertex-shader module that READS a 32-bit
 * unsigned integer from a BuiltIn VertexIndex input and writes a
 * vec4 to BuiltIn Position. The vec4's first lane is f32(vertex_id);
 * the other lanes are zero. Demonstrates per-vertex reads of a
 * SPIR-V builtin input + the input-output dataflow pattern that
 * full shaders will use.
 *
 * On success: *out_blob is malloc'd; caller frees with free().
 * *out_size is byte count. Returns 0 on success, -1 on OOM. */
int lagfx_air2spv_emit_vertex_id_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_VERTEX_ID_H */
