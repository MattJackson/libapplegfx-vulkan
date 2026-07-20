/*
 * libapplegfx-vulkan — Phase 5 AIR-module → SPIR-V module translator (skeleton)
 * src/air2spv/translate.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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

/* One resource argument of the module's entry point, in FUNCTION-ARG ORDER
 * (the same order the translator assigns sequential SPIR-V bindings), with
 * its Metal resource index from Apple's own arg metadata.
 *
 * kind: 1 = buffer ([[buffer(n)]]), 2 = texture, 3 = sampler.
 * metal_index: the `air.location_index` of the arg — i.e. the Metal binding
 * slot the guest's SetVertexBuffers/SetFragmentBuffers/... index refers to.
 * -1 if the index could not be resolved.
 *
 * This is THE authoritative binding map: the Metal buffer index VARIES per
 * shader (UberCompositeVertex: mvp_matrix=[[buffer(1)]]; ViewportToNDC:
 * buffers 0/1/2; CA lock-screen composites: matrix at 2) — no fixed
 * convention exists, so slot-guessing heuristics (skip-one, MTXSCAN content
 * signatures) cannot be correct in general. */
typedef struct {
    uint8_t kind;
    int16_t metal_index;
} lagfx_air_arg_binding_t;

/* Walk the entry point's air.vertex / air.fragment per-arg metadata and
 * return the resource args (buffers/textures/samplers) in arg order.
 * Returns the number of entries written (0 on any parse failure — callers
 * MUST treat 0 as "no mapping available" and fall back). */
size_t
lagfx_air_arg_bindings(const lagfx_air_module_t *m,
                        lagfx_air_arg_binding_t  *out,
                        size_t                    max);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_TRANSLATE_H */
