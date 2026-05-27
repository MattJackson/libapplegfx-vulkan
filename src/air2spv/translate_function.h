/*
 * libapplegfx-vulkan — Phase 5 per-function AIR → SPIR-V translator
 * src/air2spv/translate_function.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Translates one AIR function body (a decoded FUNCTION_BLOCK) into a
 * complete Vulkan-compliant SPIR-V module. Composes the per-pattern
 * lessons from the Phase 4 reference emitters (emit_position,
 * emit_extinst_glsl, emit_vector_shuffle, …) into a per-AIR-record
 * dispatch.
 *
 * Two-pass shape:
 *   - Pass 1: classify instructions, build the SSA-id map sizing,
 *     identify the interface (output vars, input builtins), enumerate
 *     types referenced.
 *   - Pass 2: emit SPIR-V per AIR record, looking up operand SPIR-V ids
 *     from the map.
 *
 * Operand value-id resolution follows LLVM's bitcode rules:
 *   - Module values live at IDs 0..(num_functions + num_module_consts - 1)
 *     in ValueEnumerator order: functions first, then module constants.
 *   - Function arguments take the next N IDs.
 *   - Function-local CONSTANTS_BLOCK adds K IDs after args (NOT decoded
 *     by the reader yet — operands referencing these resolve to OpUndef
 *     placeholders for now; tracked in [[task_function_local_consts]]).
 *   - Each value-producing instruction adds the next ID.
 *
 * Many AIR opcodes encode operand value-ids relative to the next
 * instruction's value-id (`actual = next_inst_val - encoded`). The
 * resolver handles both forms.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H
#define LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H

#include "air/bitcode_reader.h"
#include "libapplegfx-vulkan.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAGFX_XLATE_STAGE_VERTEX   = 0,
    LAGFX_XLATE_STAGE_FRAGMENT = 1,
} lagfx_xlate_stage_t;

/* Translate the body of `m->functions[fn_idx]` into a SPIR-V module.
 *
 * The function MUST have a body (is_proto == false; body_offset != 0).
 * The caller is responsible for selecting the entry-point function
 * (typically: the one whose name appears in the air.vertex /
 * air.fragment named-metadata's target NODE).
 *
 * On success: *out_blob is a malloc'd SPIR-V buffer; caller frees with
 * free(). *out_size_bytes is the byte length.
 *
 * Returns:
 *   LAGFX_OK              — translated; blob spirv-val-clean (subject
 *                           to Phase 5 maturity)
 *   LAGFX_ERR_INVALID_ARG — bad inputs; fn has no body
 *   LAGFX_ERR_PROTOCOL    — body decode failed or contains opcodes the
 *                           current translator cannot handle without
 *                           losing structural validity
 *   LAGFX_ERR_OUT_OF_MEMORY — allocation failure
 */
lagfx_status_t lagfx_air2spv_translate_function(
    const lagfx_air_module_t *m,
    uint32_t                  fn_idx,
    lagfx_xlate_stage_t       stage,
    uint8_t                 **out_blob,
    size_t                   *out_size_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_TRANSLATE_FUNCTION_H */
