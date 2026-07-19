/*
 * libapplegfx-vulkan — AIR to SPIR-V translation API (Phase 3.C.2 M5)
 * src/air2spirv/shader_translate.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Runtime translator API that glues the offline MSL→SPIR-V pipeline
 * (metallib_extract + bitcode_retarget + spv_signature_transform) into a
 * single in-process call suitable for the SetRenderPipelineState (0x70)
 * inner-op handler.
 *
 * This is Stage 65 of the M5 progress scale per
 * paravirt-re/library/offline-pipeline-map-2026-05-16.md §"Gap Analysis".
 * The underlying stages are:
 *
 *   1. metallib_extract — find named function's LLVM Bitcode in MTLB blob
 *      (lagfx_metallib_extract_functions at metallib_extract.h:92)
 *   2. bitcode_retarget — rewrite triple to spir64-unknown-vulkan1.3
 *      (lagfx_bitcode_retarget_to_spirv at bitcode_retarget.h:95)
 *   3. llc lowering — NOT YET IMPLEMENTED; currently returns error
 *      (see offline-pipeline-map-2026-05-16.md §"Stage 4 (llc lowering)")
 *   4. spv_signature_transform — Vulkan GLSL-style rewrite
 *      (lagfx_spv_signature_transform at spv_signature_transform.h:123)
 *
 * The llc step is the current blocker for Stage 65 — this API returns
 * LAGFX_ERR_NOT_IMPLEMENTED until stage 4 infrastructure lands.
 */

#ifndef LIBAPPLEGFX_AIR2SPIRV_SHADER_TRANSLATE_H
#define LIBAPPLEGFX_AIR2SPIRV_SHADER_TRANSLATE_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Shader stage enum for SPIR-V output. Mirrors LAGFX_SPV_STAGE_* from
 * spv_entrypoint_rewrite.h but uses simpler naming for the top-level API.
 */
typedef enum {
    LAGFX_SHADER_STAGE_VERTEX = 0,      /* SpvExecutionModelVertex */
    LAGFX_SHADER_STAGE_FRAGMENT = 4,    /* SpvExecutionModelFragment */
    LAGFX_SHADER_STAGE_COMPUTE = 3,     /* SpvExecutionModelKernel (reserved) */
} lagfx_shader_stage_t;

/**
 * Result structure for a successful translation. Contains SPIR-V bytes
 * valid until lagfx_shader_translate_free() is called.
 *
 * Ownership:
 *   - spv_bytes points to malloc'd memory owned by the caller.
 *   - Caller MUST call lagfx_shader_translate_free() when done.
 *   - Safe to pass zero-initialised struct (no-op).
 */
typedef struct {
    const uint8_t *spv_bytes;           /* SPIR-V bytes; NULL on failure */
    size_t         spv_len;             /* Byte count of spv_bytes */
    lagfx_shader_stage_t stage;         /* Shader stage the SPIR-V targets */
} lagfx_shader_translation_t;

/**
 * Translate one named function from a metallib blob to Vulkan-ready SPIR-V.
 *
 * This is the runtime entry point for Stage 65 (offline pipeline wiring). It
 * performs all translation steps in-process except llc lowering, which is not
 * yet implemented and returns LAGFX_ERR_NOT_IMPLEMENTED.
 *
 * Steps performed:
 *   1. metallib_extract — lagfx_metallib_extract_functions() at
 *      metallib_extract.h:92–97 to find the named function's LLVM Bitcode.
 *   2. bitcode_retarget — lagfx_bitcode_retarget_to_spirv() at
 *      bitcode_retarget.h:95–99 to rewrite the AIR triple.
 *   3. llc lowering — NOT YET IMPLEMENTED; returns LAGFX_ERR_NOT_IMPLEMENTED.
 *      See offline-pipeline-map-2026-05-16.md §"Stage 4 (llc lowering)".
 *   4. spv_signature_transform — lagfx_spv_signature_transform() at
 *      spv_signature_transform.h:123–129 to produce Vulkan-ready SPIR-V.
 *
 * Error semantics:
 *   - LAGFX_OK                 — translation complete; out->spv_bytes valid.
 *   - LAGFX_ERR_INVALID_ARG    — NULL inputs, invalid metallib, function not found.
 *   - LAGFX_ERR_NOT_IMPLEMENTED — llc step missing (current Stage 65 blocker).
 *   - LAGFX_ERR_OUT_OF_MEMORY  — allocation failure during rewrite.
 *   - LAGFX_ERR_PROTOCOL       — malformed metallib or SPIR-V structure.
 *
 * Logging:
 *   - Errors logged via LAGFX_ERR() from src/common/log.h.
 *   - Success not logged by default; use LAGFX_LOG_LEVEL=info for diagnostics.
 *
 * Thread safety:
 *   - Not internally synchronised. Caller must serialise if multiple threads
 *     call this against shared metallib buffers or the same function name.
 *
 * @param metallib_data  pointer to in-memory metallib blob (MTLB magic required)
 * @param metallib_len   length of the blob in bytes
 * @param function_name  NUL-terminated function name (e.g., "triangle_vertex")
 * @param stage          shader stage (vertex/fragment/compute) — ignored for now,
 *                       defaults to vertex if unknown.
 * @param out            populated on success; untouched on failure except spv_bytes
 *                       set to NULL and spv_len to 0.
 *
 * Cited from offline-pipeline-map-2026-05-16.md §"Pipeline diagram".
 */
lagfx_status_t lagfx_shader_translate_run(
    const uint8_t *metallib_data,
    size_t metallib_len,
    const char *function_name,
    lagfx_shader_stage_t stage,
    lagfx_shader_translation_t *out);

/**
 * Release the SPIR-V bytes produced by lagfx_shader_translate_run().
 *
 * After this call, translation->spv_bytes is invalid. The struct itself may be
 * reused by passing it to another _run() call. Safe to call with a zero-
 * initialised struct (no-op).
 *
 * This mirrors the "caller owns malloc'd buffer" contract from the underlying
 * APIs: bitcode_retarget.h:95–99 and spv_signature_transform.h:123–129 both
 * document that callers must free() the resulting buffers.
 */
void lagfx_shader_translate_free(lagfx_shader_translation_t *translation);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_SHADER_TRANSLATE_H */
