/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for the
 * Uniform-storage-class buffer-load shape.
 * src/air2spv/emit_buffer_load.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Fifth reference emitter. Lands the Pattern E "read from a Vulkan
 * uniform buffer + use the value in the body" shape that subsequent
 * per-air.buffer / air.buffer_size intrinsic emitters can template.
 *
 * Emits a vertex shader that:
 *   - Declares a Uniform buffer at DescriptorSet 0 Binding 0 wrapping
 *     a single `vec4 factor` member.
 *   - Loads `factor` via OpAccessChain + OpLoad.
 *   - Multiplies the hardcoded position (0,0,0,1) by `factor`.
 *   - Writes the product to BuiltIn Position.
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_BUFFER_LOAD_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_BUFFER_LOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emit a complete SPIR-V module exercising the Uniform buffer-load
 * shape. *out_blob is malloc'd; caller owns it. Returns 0 on success,
 * non-zero on OOM / builder failure. */
int lagfx_air2spv_emit_buffer_load_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_BUFFER_LOAD_H */
