/*
 * libapplegfx-vulkan — shader catalog (Phase 3.C scaffold)
 * src/shaders/catalog.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Stock-shader catalog: SPIR-V blobs embedded in the library at
 * build time, keyed for now on a placeholder kind enum
 * (lagfx_shader_kind_t in the public header). Phase 3.C.2 flips
 * the primary lookup to SHA-256-truncated-64 over the raw AIR
 * bytes submitted by the guest (see
 * paravirt-re/shader-catalog-plan.md §4 and §5).
 *
 * The catalog intentionally does NOT link against Vulkan itself
 * — the lookup returns a (bytes, length) pair which callers hand
 * to vkCreateShaderModule. This keeps the catalog usable from
 * non-Vulkan code paths (log capture, Phase 3.C.2 translator
 * fallback) without pulling <vulkan/vulkan.h> into every TU.
 *
 * See paravirt-re/phase-3-metal-vulkan-plan.md §3.C for Phase-3
 * scope.
 */

#ifndef LIBAPPLEGFX_SHADERS_CATALOG_H
#define LIBAPPLEGFX_SHADERS_CATALOG_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pipeline stage for a per-stage SPIR-V blob. Values are
 * deliberately distinct from VkShaderStageFlagBits so non-Vulkan
 * consumers don't need <vulkan/vulkan.h>. */
typedef enum {
    LAGFX_SHADER_STAGE_VERTEX   = 1,
    LAGFX_SHADER_STAGE_FRAGMENT = 2,
    LAGFX_SHADER_STAGE_COMPUTE  = 3,
} lagfx_shader_stage_t;

/* One per-stage SPIR-V blob inside a catalog entry. */
typedef struct {
    lagfx_shader_stage_t  stage;
    const char           *entry_name;   /* e.g. "main" */
    const uint8_t        *spirv_bytes;  /* pointer into .rodata */
    size_t                spirv_len;
    /* Phase 3.C.2: hash of the AIR blob this SPIR-V is the
     * translation of. Zero until the MSL→AIR build pipeline lands
     * and populates a real hash. */
    uint64_t              air_hash;
} lagfx_shader_blob_t;

/* One catalog record. At Phase 3.C scaffold each entry owns a
 * vertex + fragment blob pair (the primitives we author are all
 * graphics; compute shaders land Phase 3.C.2+). `blobs` is a
 * pointer into the catalog's static blob table rather than an
 * inline array so the struct stays small and append-only. */
typedef struct {
    lagfx_shader_kind_t         kind;        /* catalog key */
    const char                 *debug_name;  /* e.g. "blit" */
    const lagfx_shader_blob_t  *blobs;
    size_t                      blob_count;
} lagfx_shader_entry_t;

/* --- Lookup -----------------------------------------------------
 *
 * Phase 3.C scaffold lookup: by kind enum. Returns a pointer to
 * a static table entry on hit, NULL on miss. The table lives in
 * .rodata so the pointer is valid for the library's lifetime.
 *
 * Phase 3.C.2 will add lagfx_shader_catalog_lookup_by_air(
 *     const uint8_t *air, size_t len, const lagfx_shader_entry_t **out)
 * which computes SHA-256-64 over `air` and scans the table. */
const lagfx_shader_entry_t *lagfx_shader_catalog_lookup(
    lagfx_shader_kind_t kind);

/* Convenience: look up one stage's blob directly. Returns NULL
 * if kind isn't in the catalog OR the requested stage isn't
 * present for this entry. */
const lagfx_shader_blob_t *lagfx_shader_catalog_lookup_stage(
    lagfx_shader_kind_t kind, lagfx_shader_stage_t stage);

/* --- Table iteration --------------------------------------------
 *
 * Exposed primarily for tests. */
size_t lagfx_shader_catalog_count(void);
const lagfx_shader_entry_t *lagfx_shader_catalog_at(size_t i);

/* --- Fallback / capture path ------------------------------------
 *
 * Phase 3.C scaffold: logs the miss + returns false. Phase 3.C.2
 * promotes this to "dump raw AIR bytes to
 * $XDG_STATE_HOME/libapplegfx-vulkan/missed-shaders/<hash>.air"
 * per paravirt-re/shader-catalog-plan.md §7. The log line format
 * matches that plan:
 *
 *   [lagfx warn] shader not in catalog, hash=0x%016llx
 *
 * Returns false so callers propagate "no shader" up the dispatch
 * stack; the translator's policy (skip draw vs abort command
 * buffer) is Phase 3.A's call. */
bool lagfx_shader_catalog_log_miss(const uint8_t *air_bytes,
                                   size_t air_len,
                                   lagfx_shader_stage_t stage);

/* --- Device registration hook -----------------------------------
 *
 * Called from lagfx_device_new to surface a log line ("catalog
 * loaded, N entries") and give future-phase device state a place
 * to hang per-device shader-module caches. At Phase 3.C scaffold
 * time this only logs; Phase 3.E turns it into a real
 * VkShaderEXT pre-creation pass. */
lagfx_status_t lagfx_device_register_shader_catalog(lagfx_device_t *device);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_SHADERS_CATALOG_H */
