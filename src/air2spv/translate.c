/*
 * libapplegfx-vulkan — Phase 5 AIR-module → SPIR-V translator (skeleton)
 * src/air2spv/translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * MVP: uses Phase 3 named-metadata helpers to identify the entry-point
 * stage, then dispatches to a reference emitter that produces a
 * constant-output shader of the matching shape. The actual semantic
 * translation of the function body is Phase 5 work that lands in
 * subsequent commits — this skeleton establishes the API surface so
 * op_0x74 (Phase 6) has something to wire against without waiting.
 */

#include "translate.h"
#include <string.h>
#include "emit_position.h"
#include "emit_render_target.h"
#include "translate_function.h"

#include "common/log.h"

#include <stdbool.h>
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

/* === Entry-point arg-metadata walk (Metal resource indexes) ========
 *
 * LLVM/AIR metadata ID rules (validated against llvm.module.flags on the
 * login corpus: {Max, 'air.max_device_buffers', 31} resolves exactly):
 *   - NAMED_NODE operands are RAW global metadata IDs.
 *   - NODE operands are global-ID + 1 (0 = null).
 *   - global ID < num_strings  -> strings[id]; else metadata[id-num_strings].
 *   - VALUE records are [type_index, value_id]; value_id enumerates
 *     globalvars, then functions, then module constants — so constant
 *     index = value_id - (num_globalvars + num_functions).
 *   - CST_NULL means integer 0 (LLVM emits i32 0 as CST_CODE_NULL).
 *
 * The entry-point node (air.vertex/air.fragment op) is
 *   [fn VALUE, outputs NODE, args NODE]
 * and each per-arg NODE is a key/value list over the strings pool:
 *   [argidx VALUE, 'air.buffer'|'air.vertex_input'|..., 'air.location_index',
 *    <loc VALUE>, ...] */

static const lagfx_air_metadata_t *
md_record(const lagfx_air_module_t *m, uint32_t global_id) {
    uint32_t nms = 0, nmd = 0;
    (void)lagfx_air_module_metadata_strings(m, &nms);
    const lagfx_air_metadata_t *mds = lagfx_air_module_metadata(m, &nmd);
    if (!mds || global_id < nms || global_id - nms >= nmd) return NULL;
    return &mds[global_id - nms];
}

static const char *
md_string(const lagfx_air_module_t *m, uint32_t global_id) {
    uint32_t nms = 0;
    const char *const *strs = lagfx_air_module_metadata_strings(m, &nms);
    if (!strs || global_id >= nms) return NULL;
    return strs[global_id];
}

/* Resolve a VALUE metadata record to its integer constant, or -1. */
static int64_t
md_value_int(const lagfx_air_module_t *m, const lagfx_air_metadata_t *v) {
    if (!v || v->kind != LAGFX_AIR_MD_VALUE || v->num_operands < 2u) return -1;
    uint32_t value_id = v->operands[1];
    uint32_t ng = lagfx_air_module_num_globalvars(m);
    uint32_t nf = 0;
    (void)lagfx_air_module_functions(m, &nf);
    uint32_t base = ng + nf;
    if (value_id < base) return -1;
    uint32_t nc = 0;
    const lagfx_air_constant_t *cs = lagfx_air_module_constants(m, &nc);
    if (!cs || value_id - base >= nc) return -1;
    const lagfx_air_constant_t *c = &cs[value_id - base];
    if (c->kind == LAGFX_AIR_CONST_INTEGER) return c->payload.i64;
    if (c->kind == LAGFX_AIR_CONST_NULL)    return 0;
    return -1;
}

size_t
lagfx_air_arg_bindings(const lagfx_air_module_t *m,
                        lagfx_air_arg_binding_t  *out,
                        size_t                    max) {
    if (!m || !out || max == 0u) return 0u;
    const lagfx_air_metadata_t *named = lagfx_air_module_named_metadata(m, "air.vertex");
    if (!named) named = lagfx_air_module_named_metadata(m, "air.fragment");
    if (!named || named->num_operands < 1u) return 0u;
    /* Entry-point node: NAMED_NODE ops are raw IDs. */
    const lagfx_air_metadata_t *ep = md_record(m, named->operands[0]);
    if (!ep || ep->kind != LAGFX_AIR_MD_NODE || ep->num_operands < 3u) return 0u;
    /* [fn, outputs, args] — args node ops are ID+1. */
    uint32_t args_id = ep->operands[2];
    if (args_id == 0u) return 0u;
    const lagfx_air_metadata_t *args = md_record(m, args_id - 1u);
    if (!args || args->kind != LAGFX_AIR_MD_NODE) return 0u;

    size_t n_out = 0u;
    for (uint32_t a = 0; a < args->num_operands && n_out < max; a++) {
        if (args->operands[a] == 0u) continue;
        const lagfx_air_metadata_t *arg = md_record(m, args->operands[a] - 1u);
        if (!arg || arg->kind != LAGFX_AIR_MD_NODE) continue;
        uint8_t kind = 0;      /* 1=buffer 2=texture 3=sampler */
        int64_t loc = -1;
        bool is_stagein = false;
        for (uint32_t j = 0; j < arg->num_operands; j++) {
            if (arg->operands[j] == 0u) continue;
            const char *s = md_string(m, arg->operands[j] - 1u);
            if (!s) continue;
            if (strcmp(s, "air.buffer") == 0)            kind = kind ? kind : 1u;
            else if (strcmp(s, "air.texture") == 0)      kind = kind ? kind : 2u;
            else if (strcmp(s, "air.sampler") == 0)      kind = kind ? kind : 3u;
            else if (strcmp(s, "air.vertex_input") == 0
                     || strcmp(s, "air.position") == 0
                     || strcmp(s, "air.vertex_id") == 0
                     || strcmp(s, "air.fragment_input") == 0)
                is_stagein = true;
            else if (strcmp(s, "air.location_index") == 0
                     && j + 1u < arg->num_operands
                     && arg->operands[j + 1u] != 0u) {
                loc = md_value_int(m, md_record(m, arg->operands[j + 1u] - 1u));
            }
        }
        if (is_stagein || kind == 0u) continue;   /* stage-in attr / vid — not a resource */
        out[n_out].kind        = kind;
        out[n_out].metal_index = (loc >= 0 && loc < 0x7fff) ? (int16_t)loc : -1;
        n_out++;
    }
    return n_out;
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

    /* Per-function body translation is the default path for modules with a
     * non-prototype function; the legacy reference emitters remain the
     * fallback when it fails. */
    {
        uint32_t n_fns = 0;
        const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &n_fns);
        uint32_t body_fn = (uint32_t)-1;
        for (uint32_t i = 0; i < n_fns; i++) {
            if (fns[i].is_proto || fns[i].body_offset == 0u) continue;
            /* Skip C++ static-initializer functions (`_GLOBAL__sub_I_*.metal`,
             * void(), section "air.static_init"). Composite SkyLight shaders
             * carry one BEFORE the real entry shader (function-constant init);
             * picking the first body function then translated the static-init
             * (num_args=0, no resources) instead of e.g. UberCompositeFragment
             * → every texture + buffer dropped → black. The real entry shader
             * is the non-static-init body function. */
            const char *nm = lagfx_air_module_string(m, fns[i].name_offset);
            if (nm && (strstr(nm, "_GLOBAL__sub_I") || strstr(nm, "global_ctors")
                       || strstr(nm, ".static_init"))) {
                LAGFX_TRACE("air2spv: skipping static-init fn[%u] '%s'", i, nm);
                continue;
            }
            body_fn = i; break;
        }
        if (body_fn != (uint32_t)-1 && stage != LAGFX_TRANS_STAGE_UNKNOWN) {
            lagfx_xlate_stage_t xs = (stage == LAGFX_TRANS_STAGE_VERTEX)
                                       ? LAGFX_XLATE_STAGE_VERTEX
                                       : LAGFX_XLATE_STAGE_FRAGMENT;
            LAGFX_TRACE("air2spv: translating fn[%u]", body_fn);
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
