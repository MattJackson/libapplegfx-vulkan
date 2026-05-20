/*
 * libapplegfx-vulkan — Phase 4 skeleton SPIR-V emitter for air.position
 * src/air2spv/emit_position.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_POSITION_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_POSITION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emit a minimal SPIR-V vertex-shader module that writes a constant
 * vec4(0, 0, 0, 1) to a BuiltIn Position output. Used as the Phase 4
 * reference emitter for air.position — proves the spv_builder scaffold
 * produces a Vulkan-valid module.
 *
 * On success: *out_blob is a malloc'd byte buffer; caller frees with
 * free(). *out_size is the byte count.
 *
 * Returns 0 on success, -1 on OOM. */
int lagfx_air2spv_emit_position_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_POSITION_H */
