/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter:
 * if/else via OpSelectionMerge + OpBranchConditional + OpPhi.
 * src/air2spv/emit_control_flow.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Tenth reference emitter. Lands Pattern I — structured control
 * flow. Required for any AIR shader with a non-trivial body
 * (early-out, alpha cutoff, branch-on-uniform, etc.).
 *
 * Output: fragment shader that reads vec2 uv, picks red if uv.x > 0.5
 * else blue, writes to color attachment 0. Demonstrates:
 *   - OpFOrdGreaterThan comparison producing a bool
 *   - OpSelectionMerge to mark the merge block (required by SPIR-V
 *     structured-control-flow rules)
 *   - OpBranchConditional + OpBranch
 *   - OpPhi to select between branch-local results without spilling
 *     to a Function-local variable
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_CONTROL_FLOW_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_CONTROL_FLOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_control_flow_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_CONTROL_FLOW_H */
