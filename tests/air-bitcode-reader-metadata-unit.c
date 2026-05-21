/*
 * libapplegfx-vulkan — clean-room AIR bitcode reader METADATA tests
 * tests/air-bitcode-reader-metadata-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 3 — module-level METADATA_BLOCK walker tests. Validates that
 * lagfx_air_module_open() decodes triangle's METADATA_STRINGS BLOB +
 * the subsequent NODE / VALUE / NAMED_NODE records byte-exact against
 * `llvm-bcanalyzer --dump`.
 *
 * Ground truth (bcanalyzer 22.1.5 on triangle_vertex.air.bc):
 *   22 strings in METADATA_STRINGS pool (LLVM metadata-IDs 0..21)
 *   11 VALUE records      (metadata-IDs 22..32)
 *   20 NODE records       (metadata-IDs 33..52)
 *    6 NAMED_NODE records (metadata-IDs 53..58)
 *      named: llvm.module.flags, llvm.ident, air.version,
 *             air.language_version, air.compile_options, air.vertex
 *
 * Earlier doc count said "22 STRINGS / 7 NAMED_NODE / ~12 VALUE / ~10
 * NODE"; that was approximate. These tests assert the exact counts.
 */

#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

static const char *kStagingPath = "/Users/mjackson/Developer/staging/triangle_vertex.air.bc";

static int test_triangle_metadata(void) {
    const char *candidates[] = {
        kStagingPath,
        "/tmp/scoping/triangle_vertex.air.bc",
        NULL
    };
    uint8_t *blob = NULL;
    size_t   len  = 0;
    const char *used = NULL;
    for (int i = 0; candidates[i]; i++) {
        blob = slurp(candidates[i], &len);
        if (blob) { used = candidates[i]; break; }
    }
    if (!blob) {
        printf("SKIP: triangle .air.bc fixture not found at staging or /tmp\n");
        return 0;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || !m) {
        printf("FAIL: triangle open st=%d (from %s)\n", (int)st, used);
        free(blob);
        return 1;
    }

    /* === Strings pool === */
    uint32_t nstr = 0;
    const char * const *strs = lagfx_air_module_metadata_strings(m, &nstr);
    if (nstr != 22u) {
        printf("FAIL: expected 22 metadata strings, got %u\n", nstr);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    struct {
        uint32_t    idx;
        const char *expected;
    } spot[] = {
        { 0,  "SDK Version" },
        { 1,  "wchar_size" },
        { 9,  "Apple metal version 32023.883 (metalfe-32023.883)" },
        { 14, "air.position" },
        { 19, "air.vertex_id" },
        { 20, "uint" },
        { 21, "vid" },
    };
    for (size_t i = 0; i < sizeof(spot) / sizeof(spot[0]); i++) {
        const char *got = strs[spot[i].idx];
        if (!got || strcmp(got, spot[i].expected) != 0) {
            printf("FAIL: strings[%u] expected '%s', got '%s'\n",
                   spot[i].idx, spot[i].expected, got ? got : "(null)");
            lagfx_air_module_free(m);
            free(blob);
            return 1;
        }
    }

    /* === Metadata records === */
    uint32_t nmd = 0;
    const lagfx_air_metadata_t *mds = lagfx_air_module_metadata(m, &nmd);
    if (nmd != 37u) {
        printf("FAIL: expected 37 metadata records, got %u\n", nmd);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    uint32_t n_value = 0, n_node = 0, n_named = 0, n_unknown = 0;
    for (uint32_t i = 0; i < nmd; i++) {
        switch (mds[i].kind) {
            case LAGFX_AIR_MD_VALUE:      n_value++;   break;
            case LAGFX_AIR_MD_NODE:       n_node++;    break;
            case LAGFX_AIR_MD_NAMED_NODE: n_named++;   break;
            default:                      n_unknown++; break;
        }
    }
    if (n_value != 11u || n_node != 20u || n_named != 6u || n_unknown != 0u) {
        printf("FAIL: record-kind counts: VALUE=%u (want 11) NODE=%u (want 20) "
               "NAMED_NODE=%u (want 6) UNKNOWN=%u (want 0)\n",
               n_value, n_node, n_named, n_unknown);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    /* The six NAMED_NODE records — bcanalyzer-confirmed names in order. */
    static const char *kNamedExpected[6] = {
        "llvm.module.flags",
        "llvm.ident",
        "air.version",
        "air.language_version",
        "air.compile_options",
        "air.vertex",
    };
    uint32_t named_idx = 0;
    for (uint32_t i = 0; i < nmd; i++) {
        if (mds[i].kind != LAGFX_AIR_MD_NAMED_NODE) continue;
        const char *name = lagfx_air_module_string(m, mds[i].name_offset);
        if (!name || strcmp(name, kNamedExpected[named_idx]) != 0) {
            printf("FAIL: NAMED_NODE[%u] expected '%s', got '%s'\n",
                   named_idx, kNamedExpected[named_idx], name ? name : "(null)");
            lagfx_air_module_free(m);
            free(blob);
            return 1;
        }
        named_idx++;
    }

    /* Spot-check VALUE record [0] (metadata-ID 22) — first VALUE per
     * bcanalyzer is [3, 3] (type_index=3 i.e. i32, value_id=3). */
    if (mds[0].kind != LAGFX_AIR_MD_VALUE || mds[0].num_operands != 2u ||
        mds[0].operands[0] != 3u || mds[0].operands[1] != 3u) {
        printf("FAIL: first VALUE record should be [3, 3], got kind=%d ops=",
               (int)mds[0].kind);
        for (uint32_t j = 0; j < mds[0].num_operands; j++) {
            printf("%u%s", mds[0].operands[j], j + 1 < mds[0].num_operands ? "," : "");
        }
        printf("\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    /* Spot-check first NODE record — bcanalyzer: NODE op0=23 op1=1 op2=24. */
    const lagfx_air_metadata_t *first_node = NULL;
    for (uint32_t i = 0; i < nmd; i++) {
        if (mds[i].kind == LAGFX_AIR_MD_NODE) { first_node = &mds[i]; break; }
    }
    if (!first_node || first_node->num_operands != 3u ||
        first_node->operands[0] != 23u ||
        first_node->operands[1] != 1u  ||
        first_node->operands[2] != 24u) {
        printf("FAIL: first NODE should be [23,1,24]\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    /* The 'air.vertex' NAMED_NODE (last named-node) should reference
     * one operand pointing at the function-description NODE. Bcanalyzer:
     * <NAMED_NODE op0=52/>. Operand 52 is in the metadata-records range
     * (52 - 22 = records[30]) and should be a NODE. */
    const lagfx_air_metadata_t *air_vertex_nn = NULL;
    for (uint32_t i = 0; i < nmd; i++) {
        if (mds[i].kind == LAGFX_AIR_MD_NAMED_NODE) {
            const char *name = lagfx_air_module_string(m, mds[i].name_offset);
            if (name && strcmp(name, "air.vertex") == 0) {
                air_vertex_nn = &mds[i];
                break;
            }
        }
    }
    if (!air_vertex_nn || air_vertex_nn->num_operands != 1u ||
        air_vertex_nn->operands[0] != 52u) {
        printf("FAIL: 'air.vertex' NAMED_NODE should have one operand = 52\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* And metadata-ID 52 should resolve to a NODE record. */
    uint32_t target_idx = 52u - nstr;
    if (target_idx >= nmd || mds[target_idx].kind != LAGFX_AIR_MD_NODE) {
        printf("FAIL: 'air.vertex' target (md-id 52) should be NODE, got kind=%d\n",
               target_idx < nmd ? (int)mds[target_idx].kind : -1);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    /* === Helpers: named-metadata lookup + string-by-id ============= */

    const lagfx_air_metadata_t *air_vertex_via_helper =
        lagfx_air_module_named_metadata(m, "air.vertex");
    if (air_vertex_via_helper != air_vertex_nn) {
        printf("FAIL: lagfx_air_module_named_metadata('air.vertex') returned %p; want %p\n",
               (const void *)air_vertex_via_helper, (const void *)air_vertex_nn);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    if (lagfx_air_module_named_metadata(m, "definitely.not.here") != NULL) {
        printf("FAIL: named_metadata('definitely.not.here') should be NULL\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    /* metadata-ID 14 is 'air.position' in triangle's strings pool. */
    const char *air_pos = lagfx_air_module_metadata_string_by_id(m, 14);
    if (!air_pos || strcmp(air_pos, "air.position") != 0) {
        printf("FAIL: string_by_id(14) expected 'air.position', got '%s'\n",
               air_pos ? air_pos : "(null)");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* metadata-ID >= num_strings should return NULL (it's a record). */
    if (lagfx_air_module_metadata_string_by_id(m, nstr) != NULL) {
        printf("FAIL: string_by_id(%u, == num_strings) should be NULL\n", nstr);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    printf("PASS: triangle METADATA — %u strings, %u records (%u VALUE / %u NODE / %u NAMED_NODE) from %s\n",
           nstr, nmd, n_value, n_node, n_named, used);
    printf("PASS: named-metadata + string-by-id helpers resolve correctly\n");
    lagfx_air_module_free(m);
    free(blob);
    return 0;
}

int main(void) {
    return test_triangle_metadata();
}
