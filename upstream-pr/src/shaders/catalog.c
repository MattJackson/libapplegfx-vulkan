/*
 * libapplegfx-vulkan — shader catalog (Phase 3.C scaffold)
 * src/shaders/catalog.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements the runtime shader catalog described in
 * the internal spec and
 * the internal spec The catalog is
 * a static table of (lagfx_shader_kind_t, stage, SPIR-V blob)
 * records, one per stock shader we author in src/shaders/msl/.
 *
 * Phase 3.C scaffold choices (ADR-shaped summary):
 *   - Key: lagfx_shader_kind_t enum, not AIR-byte hash. Phase 3.C.2
 *     adds the hash-based path once the command-buffer decoder
 *     isolates the opcode that carries AIR blobs
 *     (the internal spec FIXME).
 *   - Storage: SPIR-V bytes embedded in .rodata via the bin2c-style
 *     generator at src/shaders/embed_spirv.py, consumed via the
 *     #include below.
 *   - Failure mode: lookup returns NULL on miss; callers log + skip
 *     the draw per the "fail-soft" policy in shader-catalog-plan.md
 *     §7. The runtime AIR→SPIR-V translator that would normally
 *     cover misses is explicitly deferred (§9).
 *
 * This file has no Vulkan dependency. The VkShaderModule creation
 * happens in src/vulkan/ when Phase 3.E wires the pipeline path;
 * the catalog only ships the bytes.
 */

#include "shaders/catalog.h"
#include "common/log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Generated at build time from the five .spv files in
 * src/shaders/spv/. See src/shaders/meson.build for the
 * custom_target that produces this file and
 * src/shaders/embed_spirv.py for the generator. */
#include "catalog_embedded.inc"

/* --- Per-entry blob arrays -------------------------------------
 *
 * Each catalog entry points at a fixed-size table of blobs
 * (vertex + fragment for every stock graphics shader at this
 * scaffold stage). Phase 3.D's compute shaders (#4+#5 of the
 * metallib-analysis.md inventory) will append a third slot.
 *
 * The blob length field is initialised via sizeof() on the
 * embedded array so the table can live entirely in .rodata
 * (no post-init mutation; writing to .rodata SIGBUSes on
 * Darwin and SIGSEGVs on Linux). sizeof(X[]) is a compile-time
 * constant because the array definition precedes this point
 * through the #include above. */
#define LAGFX_BLOB(stage_, name_, ident_) \
    { (stage_), "main", (ident_), sizeof(ident_), 0 }

static const lagfx_shader_blob_t g_blobs_blit[] = {
    LAGFX_BLOB(LAGFX_SHADER_STAGE_VERTEX,   "blit.vert",
               lagfx_spirv_blit_vert),
    LAGFX_BLOB(LAGFX_SHADER_STAGE_FRAGMENT, "blit.frag",
               lagfx_spirv_blit_frag),
};

static const lagfx_shader_blob_t g_blobs_clear[] = {
    LAGFX_BLOB(LAGFX_SHADER_STAGE_VERTEX,   "clear.vert",
               lagfx_spirv_clear_vert),
    LAGFX_BLOB(LAGFX_SHADER_STAGE_FRAGMENT, "clear.frag",
               lagfx_spirv_clear_frag),
};

static const lagfx_shader_blob_t g_blobs_composite_over[] = {
    LAGFX_BLOB(LAGFX_SHADER_STAGE_VERTEX,   "composite_over.vert",
               lagfx_spirv_composite_over_vert),
    LAGFX_BLOB(LAGFX_SHADER_STAGE_FRAGMENT, "composite_over.frag",
               lagfx_spirv_composite_over_frag),
};

static const lagfx_shader_blob_t g_blobs_cursor[] = {
    LAGFX_BLOB(LAGFX_SHADER_STAGE_VERTEX,   "cursor.vert",
               lagfx_spirv_cursor_vert),
    LAGFX_BLOB(LAGFX_SHADER_STAGE_FRAGMENT, "cursor.frag",
               lagfx_spirv_cursor_frag),
};

static const lagfx_shader_blob_t g_blobs_color_fill[] = {
    LAGFX_BLOB(LAGFX_SHADER_STAGE_VERTEX,   "color_fill.vert",
               lagfx_spirv_color_fill_vert),
    LAGFX_BLOB(LAGFX_SHADER_STAGE_FRAGMENT, "color_fill.frag",
               lagfx_spirv_color_fill_frag),
};

#undef LAGFX_BLOB

/* --- Catalog table ---------------------------------------------
 *
 * Append-only. The kind enum values are defined by
 * include/libapplegfx-vulkan.h; the ordering in this table is
 * independent (lookup is linear). */
static const lagfx_shader_entry_t g_catalog[] = {
    { LAGFX_SHADER_BLIT,            "blit",
      g_blobs_blit,            sizeof(g_blobs_blit) / sizeof(g_blobs_blit[0]) },
    { LAGFX_SHADER_CLEAR,           "clear",
      g_blobs_clear,           sizeof(g_blobs_clear) / sizeof(g_blobs_clear[0]) },
    { LAGFX_SHADER_COMPOSITE_OVER,  "composite_over",
      g_blobs_composite_over,  sizeof(g_blobs_composite_over) / sizeof(g_blobs_composite_over[0]) },
    { LAGFX_SHADER_CURSOR,          "cursor",
      g_blobs_cursor,          sizeof(g_blobs_cursor) / sizeof(g_blobs_cursor[0]) },
    { LAGFX_SHADER_COLOR_FILL,      "color_fill",
      g_blobs_color_fill,      sizeof(g_blobs_color_fill) / sizeof(g_blobs_color_fill[0]) },
};

/* Number of entries in the catalog. 5 today; grows as we author
 * more shaders per metallib-analysis.md §Shader inventory. */
static const size_t g_catalog_len =
    sizeof(g_catalog) / sizeof(g_catalog[0]);

/* --- Public API ------------------------------------------------- */

const lagfx_shader_entry_t *lagfx_shader_catalog_lookup(
    lagfx_shader_kind_t kind) {
    for (size_t i = 0; i < g_catalog_len; ++i) {
        if (g_catalog[i].kind == kind) {
            return &g_catalog[i];
        }
    }
    return NULL;
}

const lagfx_shader_blob_t *lagfx_shader_catalog_lookup_stage(
    lagfx_shader_kind_t kind, lagfx_shader_stage_t stage) {
    const lagfx_shader_entry_t *e = lagfx_shader_catalog_lookup(kind);
    if (!e) {
        return NULL;
    }
    for (size_t i = 0; i < e->blob_count; ++i) {
        if (e->blobs[i].stage == stage) {
            return &e->blobs[i];
        }
    }
    return NULL;
}

size_t lagfx_shader_catalog_count(void) {
    return g_catalog_len;
}

const lagfx_shader_entry_t *lagfx_shader_catalog_at(size_t i) {
    if (i >= g_catalog_len) {
        return NULL;
    }
    return &g_catalog[i];
}

/* Lightweight hash used for the miss-logging side-car. This is
 * NOT cryptographic SHA-256; Phase 3.C.2 swaps this helper for
 * SHA-256-truncated-64 once we own the hash transport end-to-end.
 * FNV-1a 64-bit is good enough for the scaffold (collisions are
 * observable in logs but don't affect correctness because the
 * catalog isn't keyed on the hash yet). */
static uint64_t fnv1a_64(const uint8_t *p, size_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool lagfx_shader_catalog_log_miss(const uint8_t *air_bytes,
                                   size_t air_len,
                                   lagfx_shader_stage_t stage) {
    uint64_t h = 0;
    if (air_bytes && air_len > 0) {
        h = fnv1a_64(air_bytes, air_len);
    }
    /* The format string matches the internal spec
     * §7 literally so log-grep workflows stay stable across
     * Phase 3.C -> 3.C.2. */
    LAGFX_WARN("shader not in catalog, hash=0x%016llx (len=%zu stage=%u) "
               "[FIXME(phase-3c-air2spirv)]",
               (unsigned long long)h, air_len, (unsigned)stage);
    (void)air_bytes;
    return false;
}

lagfx_status_t lagfx_device_register_shader_catalog(lagfx_device_t *device) {
    if (!device) {
        return LAGFX_ERR_INVALID_ARG;
    }
    LAGFX_LOG("shader catalog: registered %zu stock shaders for dev=%p",
              g_catalog_len, (void *)device);
    /* Phase 3.E: pre-create VkShaderEXT for each stage here. At
     * Phase 3.C scaffold time we only log — the device struct
     * doesn't yet carry a shader-module cache. */
    return LAGFX_OK;
}
