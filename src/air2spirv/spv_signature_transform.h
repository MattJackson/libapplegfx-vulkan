/*
 * libapplegfx-vulkan — SPV signature transform (Phase 3.C.2 M5)
 * src/air2spirv/spv_signature_transform.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Takes SPIR-V emitted by the LLVM SPIR-V backend (which uses an
 * OpenCL/Kernel calling convention: `%struct fn(%args)` with `Pure`
 * function-control and return-by-value) and rewrites it into a
 * Vulkan-GLSL-style shader (void-returning entry point with Input
 * and Output storage-class globals standing in for each parameter
 * and each return-struct member).
 *
 * This supersedes spv_entrypoint_rewrite in the triangle E2E path:
 * the transform performed here subsumes the metadata edits done by
 * lagfx_spv_rewrite_entry_point (OpCapability / OpEntryPoint /
 * OpExecutionMode injection, CPacked / LinkageAttributes strip)
 * AND also rewrites the function signature so that
 * vkCreateGraphicsPipelines stops SEGFAULT'ing inside Mesa's NIR
 * compiler. See:
 *
 *   - src/air2spirv/spv_entrypoint_rewrite.c  (metadata pass)
 *   - tests/triangle-lavapipe-e2e.c            (USE_APPLE_SPV gate)
 *
 * Scope — what the transform does
 * -------------------------------
 *
 * Given an entry function named `entry_point_name` with type
 *
 *     %fn_t = OpTypeFunction %ret_t %arg0_t %arg1_t ...
 *
 * where %ret_t is either a direct v4float (fragment) or a struct of
 * one-or-more v4float / float members (vertex), and each %argN_t is
 * a scalar integer (vertex_id / instance_id as uint), the rewriter
 * emits a new SPIR-V blob that:
 *
 *   (a) Drops OpCapability Linkage / Kernel.
 *   (b) Ensures OpCapability Shader is present.
 *   (c) Drops OpDecorate / OpMemberDecorate CPacked.
 *   (d) Drops OpDecorate / OpMemberDecorate LinkageAttributes.
 *   (e) Synthesizes an OpTypeVoid, OpTypeFunction %void (no args).
 *   (f) Synthesizes, for each struct member of the return type (or
 *       for the return type itself if non-struct), an OpVariable
 *       in the Output storage class of matching element type, and
 *       decorates it with BuiltIn Position (vertex, member 0) or
 *       Location N (fragment, member N / or the sole return).
 *   (g) Synthesizes, for each function parameter, an OpVariable
 *       in the Input storage class of matching type, and decorates
 *       it with the stage-appropriate BuiltIn (VertexIndex for
 *       vertex-stage uint params).
 *   (h) Injects OpEntryPoint <stage> <fn-id> "<name>" with an
 *       interface list that includes every synthesized Input and
 *       Output variable.
 *   (i) For fragment stage, injects OpExecutionMode OriginUpperLeft.
 *   (j) Rewrites the entry function:
 *         - return-type word changed from %ret_t to %void
 *         - function-type operand changed from %fn_t to %void_fn_t
 *         - FunctionControl word zeroed (drops Pure)
 *         - OpFunctionParameter instructions DROPPED. In their
 *           place, immediately after the function's first OpLabel,
 *           we emit
 *             <orig-param-id> = OpLoad <param-type> <in-var-id>
 *           hijacking the original parameter's result-id so that
 *           all downstream uses continue to resolve correctly.
 *         - Each OpReturnValue <retval> is replaced by
 *             [OpCompositeExtract <member-type> <retval> N] per member
 *             OpStore <out-var-N> <member>
 *             OpReturn
 *           (the OpCompositeExtract is skipped for non-struct
 *           returns; the single value is stored straight through.)
 *
 * Scope — what the transform does NOT do
 * --------------------------------------
 *
 *   - It does NOT try to interpret Metal per-attribute semantics
 *     beyond the two our triangle shader uses (`[[position]]` → BuiltIn
 *     Position, `[[vertex_id]]` → BuiltIn VertexIndex, return-value v4
 *     of fragment → Location 0). Any deeper MSL attribute plumbing is
 *     future work (tracked as FIXME(phase-3c3-attr-plumbing)).
 *   - It does NOT perform general ID-remapping or dead-code elimination.
 *     Unused types / constants carried over from the LLVM output are
 *     left in the blob; Mesa's compiler prunes them.
 *
 * Failure modes
 * -------------
 *
 *   LAGFX_ERR_INVALID_ARG   — NULL inputs, missing magic, entry name
 *                             not found.
 *   LAGFX_ERR_OUT_OF_MEMORY — working buffer allocation failed.
 *   LAGFX_ERR_PROTOCOL      — malformed instruction stream, or shader
 *                             does not fit the supported Apple-shape
 *                             (e.g. return type is neither v4float nor
 *                             struct-of-v4floats).
 */

#ifndef LIBAPPLEGFX_AIR2SPIRV_SPV_SIGNATURE_TRANSFORM_H
#define LIBAPPLEGFX_AIR2SPIRV_SPV_SIGNATURE_TRANSFORM_H

#include "libapplegfx-vulkan.h"
#include "air2spirv/spv_entrypoint_rewrite.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full signature-aware rewrite. Subsumes lagfx_spv_rewrite_entry_point
 * for the triangle E2E path. On success, *out_buf is a malloc'd buffer
 * the caller owns (free() to release).
 *
 *   in_buf, in_len     — SPIR-V input (little-endian).
 *   entry_point_name   — name of the entry function; must match an
 *                        OpName in the module.
 *   stage              — LAGFX_SPV_STAGE_VERTEX or _FRAGMENT.
 *   out_buf, out_len   — receives the rewritten buffer.
 */
lagfx_status_t lagfx_spv_signature_transform(
    const uint8_t *in_buf,
    size_t in_len,
    const char *entry_point_name,
    lagfx_spv_stage_t stage,
    uint8_t **out_buf,
    size_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_SPV_SIGNATURE_TRANSFORM_H */
