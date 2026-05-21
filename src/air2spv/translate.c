/*
 * libapplegfx-vulkan — Phase 5 AIR-module → SPIR-V translator (skeleton)
 * src/air2spv/translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * MVP: uses Phase 3 named-metadata helpers to identify the entry-point
 * stage, then dispatches to a reference emitter that produces a
 * constant-output shader of the matching shape. The actual semantic
 * translation of the function body is Phase 5 work that lands in
 * subsequent commits — this skeleton establishes the API surface so
 * op_0x74 (Phase 6) has something to wire against without waiting.
 */

#include "translate.h"
#include "emit_position.h"
#include "emit_render_target.h"

#include "common/log.h"

#include <stdlib.h>

typedef enum {
    LAGFX_TRANS_STAGE_UNKNOWN  = 0,
    LAGFX_TRANS_STAGE_VERTEX   = 1,
    LAGFX_TRANS_STAGE_FRAGMENT = 2,
} lagfx_translate_stage_t;

static lagfx_translate_stage_t discover_stage(const lagfx_air_module_t *m) {
    /* Apple's AIR convention: the metallib's entry-point function is
     * named via one of these well-known METADATA_NAMED_NODE records.
     * Each named-node operands list references the function's
     * description-NODE through one or more metadata-IDs.
     *
     * On triangle, "air.vertex" → NODE → function descriptor that
     * pins the stage. We don't need to walk into the descriptor for
     * the MVP — the named-node's existence is enough. */
    if (lagfx_air_module_named_metadata(m, "air.vertex"))   return LAGFX_TRANS_STAGE_VERTEX;
    if (lagfx_air_module_named_metadata(m, "air.fragment")) return LAGFX_TRANS_STAGE_FRAGMENT;
    if (lagfx_air_module_named_metadata(m, "air.kernel"))   return LAGFX_TRANS_STAGE_UNKNOWN;  /* compute, not supported yet */
    return LAGFX_TRANS_STAGE_UNKNOWN;
}

lagfx_status_t
lagfx_air2spv_translate_module(const lagfx_air_module_t *m,
                                uint8_t                 **out_blob,
                                size_t                   *out_size_bytes) {
    if (!m || !out_blob || !out_size_bytes) return LAGFX_ERR_INVALID_ARG;
    *out_blob = NULL;
    *out_size_bytes = 0u;

    lagfx_translate_stage_t stage = discover_stage(m);
    int rc = -1;
    switch (stage) {
        case LAGFX_TRANS_STAGE_VERTEX:
            LAGFX_TRACE("air2spv: detected vertex stage; emitting reference Position output");
            rc = lagfx_air2spv_emit_position_stub(out_blob, out_size_bytes);
            break;
        case LAGFX_TRANS_STAGE_FRAGMENT:
            LAGFX_TRACE("air2spv: detected fragment stage; emitting reference RenderTarget output");
            rc = lagfx_air2spv_emit_render_target_stub(out_blob, out_size_bytes);
            break;
        default:
            LAGFX_LOG("air2spv: could not discover entry-point stage (no air.vertex / air.fragment NAMED_NODE)");
            return LAGFX_ERR_PROTOCOL;
    }

    if (rc != 0 || !*out_blob) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    return LAGFX_OK;
}
