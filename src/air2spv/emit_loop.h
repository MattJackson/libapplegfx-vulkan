/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for structured loops.
 * src/air2spv/emit_loop.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Eleventh reference emitter. Lands Pattern J — structured `for` /
 * `while` loops via OpLoopMerge + Function-storage OpVariable.
 *
 * Output: fragment shader that runs an integer counter from 0..3
 * (incrementing once per iteration) and then writes red to color
 * attachment 0. Loop trip count = 4; the iteration result isn't
 * itself observed in output — the value of this emitter is the
 * STRUCTURE of the loop, not its arithmetic. Real per-intrinsic
 * emitters lowering AIR `br.cond` / `for.body` blocks will copy
 * this scaffolding.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_LOOP_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_LOOP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_loop_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_LOOP_H */
