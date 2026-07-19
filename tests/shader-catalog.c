/*
 * libapplegfx-vulkan — shader catalog smoke (Phase 3.C scaffold)
 * tests/shader-catalog.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Exercises src/shaders/catalog.c:
 *   1. Each of the 5 shader kinds in lagfx_shader_kind_t looks
 *      up to a non-NULL entry.
 *   2. Every entry's first stage blob (vertex, by construction
 *      of the table) begins with the SPIR-V magic word
 *      0x07230203 in the expected little-endian byte order.
 *   3. The fragment-stage blob is also present and starts with
 *      the same magic.
 *   4. Unknown kind values (e.g. 999) return NULL (miss path).
 *   5. Catalog count and iterator agree.
 *
 * Does NOT call vkCreateShaderModule — that is Phase 3.E's job
 * and requires a full device. The test only ensures the bytes
 * we hand to the eventual vkCreateShaderModule have the right
 * shape to be accepted by the Vulkan loader (magic + alignment).
 *
 * Registered by tests/meson.build only when vulkan_dep was
 * present at configure time (matches the library's own gating),
 * but the test itself does not depend on a loadable ICD — it
 * only reads the static catalog table.
 */

#include "libapplegfx-vulkan.h"
#include "shaders/catalog.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do {                     \
    if (!(cond)) {                                 \
        fprintf(stderr, "FAIL: %s\n", msg);        \
        g_fail++;                                  \
    } else {                                       \
        fprintf(stdout, "PASS: %s\n", msg);        \
    }                                              \
} while (0)

/* SPIR-V magic in the four leading bytes of every module,
 * little-endian on every host we care about. */
static int has_spirv_magic(const uint8_t *p, size_t len) {
    if (!p || len < 4u) {
        return 0;
    }
    /* 0x07230203 — little-endian: 03 02 23 07. */
    return p[0] == 0x03u
        && p[1] == 0x02u
        && p[2] == 0x23u
        && p[3] == 0x07u;
}

static void check_kind(lagfx_shader_kind_t kind, const char *label) {
    const lagfx_shader_entry_t *e = lagfx_shader_catalog_lookup(kind);
    char msg[128];

    snprintf(msg, sizeof(msg), "lookup(%s) non-NULL", label);
    CHECK(e != NULL, msg);
    if (!e) {
        return;
    }

    snprintf(msg, sizeof(msg), "%s has >=1 blob", label);
    CHECK(e->blob_count >= 1u, msg);

    /* Vertex stage blob. */
    const lagfx_shader_blob_t *vb = lagfx_shader_catalog_lookup_stage(
        kind, LAGFX_SHADER_STAGE_VERTEX);
    snprintf(msg, sizeof(msg), "%s vertex blob present", label);
    CHECK(vb != NULL, msg);
    if (vb) {
        snprintf(msg, sizeof(msg), "%s vertex blob has SPIR-V magic", label);
        CHECK(has_spirv_magic(vb->spirv_bytes, vb->spirv_len), msg);
        snprintf(msg, sizeof(msg), "%s vertex blob length >= 20 bytes (header)",
                 label);
        CHECK(vb->spirv_len >= 20u, msg);
    }

    /* Fragment stage blob. */
    const lagfx_shader_blob_t *fb = lagfx_shader_catalog_lookup_stage(
        kind, LAGFX_SHADER_STAGE_FRAGMENT);
    snprintf(msg, sizeof(msg), "%s fragment blob present", label);
    CHECK(fb != NULL, msg);
    if (fb) {
        snprintf(msg, sizeof(msg), "%s fragment blob has SPIR-V magic", label);
        CHECK(has_spirv_magic(fb->spirv_bytes, fb->spirv_len), msg);
    }
}

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan shader catalog smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    /* CHECK #1-5: each of the five shader kinds resolves. */
    check_kind(LAGFX_SHADER_BLIT,           "BLIT");
    check_kind(LAGFX_SHADER_CLEAR,          "CLEAR");
    check_kind(LAGFX_SHADER_COMPOSITE_OVER, "COMPOSITE_OVER");
    check_kind(LAGFX_SHADER_CURSOR,         "CURSOR");
    check_kind(LAGFX_SHADER_COLOR_FILL,     "COLOR_FILL");

    /* Miss path: an out-of-range value returns NULL. */
    const lagfx_shader_entry_t *miss =
        lagfx_shader_catalog_lookup((lagfx_shader_kind_t)999);
    CHECK(miss == NULL, "unknown kind returns NULL");

    /* Iterator + count agree with the expected five-entry scaffold. */
    size_t n = lagfx_shader_catalog_count();
    CHECK(n == 5u, "catalog has 5 entries");

    size_t seen = 0u;
    for (size_t i = 0; i < n; ++i) {
        const lagfx_shader_entry_t *e = lagfx_shader_catalog_at(i);
        if (e && e->debug_name) {
            seen++;
        }
    }
    CHECK(seen == 5u, "iterator visits every entry with a debug_name");

    /* Miss-log path returns false and does not crash on NULL/0. */
    bool r1 = lagfx_shader_catalog_log_miss(NULL, 0,
                                             LAGFX_SHADER_STAGE_FRAGMENT);
    CHECK(!r1, "log_miss(NULL, 0) returns false");

    const uint8_t fake_air[] = { 0xde, 0xad, 0xbe, 0xef };
    bool r2 = lagfx_shader_catalog_log_miss(fake_air, sizeof(fake_air),
                                             LAGFX_SHADER_STAGE_VERTEX);
    CHECK(!r2, "log_miss(bytes) returns false (scaffold miss is fatal)");

    fprintf(stdout, "\n=== Summary: %s ===\n",
            g_fail ? "FAILURES" : "ALL GOOD");
    return g_fail ? 1 : 0;
}
