/*
 * libapplegfx-vulkan — Phase 5 AIR-module → SPIR-V module translator (skeleton)
 * src/air2spv/translate.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 5 entry point. Walks a parsed AIR module (`lagfx_air_module_t *`)
 * and produces a Vulkan-compliant SPIR-V module.
 *
 * Initial MVP: uses Phase 3 metadata helpers to discover the entry
 * point's stage (vertex / fragment), then dispatches to a known-shape
 * reference emitter. Real per-instruction semantic translation lands
 * incrementally as the air2spv emitter library grows.
 *
 * Stage discovery:
 *   - air.vertex      named-metadata present -> vertex shader
 *   - air.fragment    named-metadata present -> fragment shader
 *   - neither         -> error (LAGFX_ERR_PROTOCOL)
 *
 * Output for the MVP:
 *   - Vertex stage  -> emit_position (constant Position output)
 *   - Fragment stage-> emit_render_target (constant red output)
 *
 * Future work:
 *   - Walk function body instructions and dispatch per-opcode.
 *   - Map air.* intrinsic calls to OpExtInst / OpImageSample* / etc.
 *   - Resolve metadata-id operand types from the strings pool.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_TRANSLATE_H
#define LIBAPPLEGFX_AIR2SPV_TRANSLATE_H

#include "air/bitcode_reader.h"
#include "libapplegfx-vulkan.h"  /* lagfx_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Translate an AIR module into a SPIR-V module.
 *
 * On success: *out_blob is malloc'd with the SPIR-V bytes; caller
 * owns it and must free. *out_size_bytes gives the byte length.
 *
 * Returns:
 *   LAGFX_OK             — success
 *   LAGFX_ERR_INVALID_ARG— NULL inputs
 *   LAGFX_ERR_PROTOCOL   — no recognized entry-point stage in the
 *                          module's named metadata
 *   LAGFX_ERR_OUT_OF_MEMORY — emitter ran out of memory
 */
lagfx_status_t
lagfx_air2spv_translate_module(const lagfx_air_module_t *m,
                                uint8_t                 **out_blob,
                                size_t                   *out_size_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_TRANSLATE_H */
