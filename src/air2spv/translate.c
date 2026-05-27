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
#include "translate_function.h"

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

    /* Phase 5 per-function body translator is opt-in via env until its
     * output is spirv-val-clean for triangle. Default path stays on the
     * legacy reference emitters so 35/40 existing tests don't regress.
     * Set LAGFX_PHASE5_BODY=1 to exercise the new translate_function
     * code path. */
    const char *phase5 = getenv("LAGFX_PHASE5_BODY");
    if (phase5 && phase5[0] == '1') {
        uint32_t n_fns = 0;
        const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &n_fns);
        uint32_t body_fn = (uint32_t)-1;
        for (uint32_t i = 0; i < n_fns; i++) {
            if (!fns[i].is_proto && fns[i].body_offset != 0u) { body_fn = i; break; }
        }
        if (body_fn != (uint32_t)-1 && stage != LAGFX_TRANS_STAGE_UNKNOWN) {
            lagfx_xlate_stage_t xs = (stage == LAGFX_TRANS_STAGE_VERTEX)
                                       ? LAGFX_XLATE_STAGE_VERTEX
                                       : LAGFX_XLATE_STAGE_FRAGMENT;
            LAGFX_TRACE("air2spv: LAGFX_PHASE5_BODY=1 — translating fn[%u]", body_fn);
            lagfx_status_t bst = lagfx_air2spv_translate_function(m, body_fn, xs,
                                                                    out_blob, out_size_bytes);
            if (bst == LAGFX_OK) return LAGFX_OK;
            LAGFX_WARN("air2spv: translate_function failed (st=%d); falling back",
                       (int)bst);
            if (*out_blob) { free(*out_blob); *out_blob = NULL; *out_size_bytes = 0u; }
        }
    }

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
