/*
 * libapplegfx-vulkan — SPV entry-point metadata rewriter (Phase 3.C.2)
 * src/air2spirv/spv_entrypoint_rewrite.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Post-processes SPIR-V blobs emitted by the LLVM SPIR-V backend
 * (stage 4 of src/air2spirv/README.md) to fix up the Vulkan-facing
 * entry-point metadata that LLVM produces in OpenCL/Kernel-calling
 * flavour. Concretely, after running
 *
 *   llc -mtriple=spirv-unknown-vulkan1.3 -filetype=obj in.bc -o out.spv
 *
 * the output contains:
 *
 *   - `OpCapability Linkage` (OpenCL holdover; Vulkan rejects)
 *   - `OpDecorate %fn LinkageAttributes "..." Export` (ditto)
 *   - `OpDecorate %_struct CPacked` (Kernel-capability flavour)
 *   - No `OpEntryPoint` at all; the function is just an exported
 *     OpenCL-style kernel entry, not a Vulkan shader stage.
 *   - No `OpExecutionMode %fn OriginUpperLeft` for fragment.
 *
 * This post-processor rewrites those pieces so `vkCreateShaderModule`
 * accepts the result as a valid Vulkan 1.3 shader. It does NOT
 * transform the function signature (which is still the Apple
 * `%struct fn(%params)` shape rather than `void main(void)` with
 * Output globals) — that transformation is tracked separately as
 * FIXME(phase-3c2-signature-transform) and is what keeps full
 * `vkCreateGraphicsPipelines` short of success on this path today.
 *
 * Scope and non-goals
 * -------------------
 *
 *   - Binary-level instruction edit, no SPIR-V parser library (we
 *     don't link spirv-tools). Each instruction is one word of
 *     (opcode, word_count) followed by `word_count-1` operands.
 *   - Input is assumed to be little-endian SPIR-V (host endianness
 *     for our x86_64 + aarch64 build targets; LLVM emits LE).
 *   - No validation — caller pairs this with a later `spirv-val`
 *     invocation when running the E2E runbook.
 *
 * Call surface
 * ------------
 *
 * A single transform function. Allocates a new buffer (caller owns);
 * input is NOT mutated. Keeps the same return-code shape as the
 * sibling bitcode_retarget API.
 */

#ifndef LIBAPPLEGFX_AIR2SPIRV_SPV_ENTRYPOINT_REWRITE_H
#define LIBAPPLEGFX_AIR2SPIRV_SPV_ENTRYPOINT_REWRITE_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPIR-V magic in host-order (little-endian on x86_64/aarch64). */
#define LAGFX_SPV_MAGIC 0x07230203u

/* Which Vulkan shader stage to emit entry-point metadata for. Matches
 * the SPIR-V ExecutionModel enum values so the rewriter can memcpy
 * straight into an OpEntryPoint operand. */
typedef enum {
    LAGFX_SPV_STAGE_VERTEX   = 0,  /* SpvExecutionModelVertex */
    LAGFX_SPV_STAGE_FRAGMENT = 4,  /* SpvExecutionModelFragment */
} lagfx_spv_stage_t;

/* Read-only check for SPIR-V magic. */
bool lagfx_spv_has_magic(const uint8_t *buf, size_t buf_len);

/* Rewrite the entry-point metadata of a SPIR-V blob so it's Vulkan-
 * valid (caveat: see file header re: signature transform).
 *
 *   in_buf, in_len     — SPIR-V input (little-endian).
 *   entry_point_name   — name of the function to promote to the
 *                        Vulkan entry point. Must match an OpName
 *                        in the input (e.g. "triangle_vertex" for
 *                        our fixtures). The emitted OpEntryPoint
 *                        keeps this name as the Vulkan-side pName —
 *                        callers must pass the same string to
 *                        VkPipelineShaderStageCreateInfo.pName.
 *   stage              — LAGFX_SPV_STAGE_VERTEX or _FRAGMENT.
 *   out_buf, out_len   — receives a malloc'd buffer the caller
 *                        owns (free via free()).
 *
 * Edits performed (in scan order):
 *
 *   - Drops `OpCapability Linkage` (OpenCL-only capability).
 *   - Drops `OpCapability Kernel` (no guest emits it today but we
 *     strip it defensively).
 *   - Ensures `OpCapability Shader` is present; injects if missing.
 *   - Drops `OpDecorate <id> LinkageAttributes "..." <mode>`.
 *   - Drops `OpDecorate <id> CPacked` decorations.
 *   - Drops `OpMemberDecorate` variants of the same two decorations.
 *   - Injects `OpEntryPoint <stage> %fn "<name>"` immediately after
 *     the `OpMemoryModel` instruction. Interface list is empty at
 *     this phase — the signature transform will add Input/Output
 *     IDs when it lands.
 *   - For fragment stage, injects `OpExecutionMode %fn OriginUpperLeft`
 *     after the entry-point instruction.
 *
 * Failure modes:
 *   LAGFX_ERR_INVALID_ARG  — NULL inputs, no SPIR-V magic, input too
 *                            short, entry_point_name NULL / empty,
 *                            or entry_point_name not found among
 *                            OpName instructions.
 *   LAGFX_ERR_OUT_OF_MEMORY — working buffer allocation failed.
 *   LAGFX_ERR_PROTOCOL     — SPIR-V well-formed check failed (e.g.
 *                            a truncated instruction, or word_count
 *                            field reads past end of buffer).
 */
lagfx_status_t lagfx_spv_rewrite_entry_point(
    const uint8_t *in_buf,
    size_t in_len,
    const char *entry_point_name,
    lagfx_spv_stage_t stage,
    uint8_t **out_buf,
    size_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_SPV_ENTRYPOINT_REWRITE_H */
