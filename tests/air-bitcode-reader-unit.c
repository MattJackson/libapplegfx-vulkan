/*
 * libapplegfx-vulkan — clean-room AIR bitcode reader unit tests
 * tests/air-bitcode-reader-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1 scope: validate that lagfx_air_module_open() accepts a real
 * .air.bc blob (wrapper magic check, bitstream magic check, allocation
 * + free). Body parsing tests come in subsequent commits as per-block
 * decoders land.
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

static int test_open_close_smoke(void) {
    /* Use the bundled triangle vertex bitcode as a known-good fixture.
     * It's checked into tests/fixtures and the extract tool drops the
     * .air.bc next to it; for the in-tree test we use the metallib
     * directly via slurping + magic-bytes-only test. */
    static const uint8_t fake_blob[24] = {
        0xDE, 0xC0, 0x17, 0x0B,  /* wrapper magic */
        0x00, 0x00, 0x00, 0x00,  /* version */
        0x14, 0x00, 0x00, 0x00,  /* body offset = 20 */
        0x04, 0x00, 0x00, 0x00,  /* body length = 4 */
        0xFF, 0xFF, 0xFF, 0xFF,  /* CPU type */
        0x42, 0x43, 0xC0, 0xDE,  /* bitstream magic */
    };
    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(fake_blob, sizeof(fake_blob), &m);
    if (st != LAGFX_OK || m == NULL) {
        printf("FAIL: minimal wrapper smoke (st=%d m=%p)\n", (int)st, (void *)m);
        return 1;
    }
    /* Module-level strings should all be absent (NULL) for this minimal
     * fixture that has only the wrapper. */
    if (lagfx_air_module_triple(m) != NULL) {
        printf("FAIL: expected NULL triple for minimal wrapper\n");
        lagfx_air_module_free(m);
        return 1;
    }
    lagfx_air_module_free(m);
    printf("PASS: minimal wrapper smoke\n");
    return 0;
}

static int test_bad_magic(void) {
    uint8_t bad[24] = {0};
    bad[0] = 0xAB; bad[1] = 0xCD;
    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(bad, sizeof(bad), &m);
    if (st == LAGFX_OK) {
        printf("FAIL: bad-magic should error, got OK\n");
        lagfx_air_module_free(m);
        return 1;
    }
    if (m != NULL) {
        printf("FAIL: bad-magic returned non-NULL module\n");
        lagfx_air_module_free(m);
        return 1;
    }
    printf("PASS: bad-magic rejected (st=%d)\n", (int)st);
    return 0;
}

static int test_truncated_wrapper(void) {
    uint8_t tiny[10] = {0xDE, 0xC0, 0x17, 0x0B};
    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(tiny, sizeof(tiny), &m);
    if (st == LAGFX_OK) {
        printf("FAIL: truncated wrapper should error\n");
        lagfx_air_module_free(m);
        return 1;
    }
    printf("PASS: truncated wrapper rejected (st=%d)\n", (int)st);
    return 0;
}

static int test_real_triangle_metallib(void) {
    /* Locate the extracted triangle bitcode. Tests run from builddir,
     * so source is at ../tests/fixtures. The .air.bc files we want
     * come from running triangle-extract-only on the triangle metallib,
     * which we do once and stash in builddir-relative paths. */
    const char *candidates[] = {
        "/tmp/scoping/triangle_vertex.air.bc",
        "tests/fixtures/triangle_vertex.air.bc",
        "../tests/fixtures/triangle_vertex.air.bc",
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
        printf("SKIP: triangle .air.bc fixture not found (regenerate via "
               "examples/triangle-extract-only)\n");
        return 0;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || m == NULL) {
        printf("FAIL: triangle .air.bc open: st=%d m=%p (from %s)\n",
               (int)st, (void *)m, used);
        free(blob);
        return 1;
    }

    /* Validate the triple — triangle metallib has
     * "air64_v28-apple-macosx<version>". */
    const char *triple = lagfx_air_module_triple(m);
    if (!triple) {
        printf("FAIL: triangle .air.bc missing triple\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    if (strncmp(triple, "air64", 5) != 0) {
        printf("FAIL: triangle .air.bc triple doesn't start with 'air64' (got '%s')\n",
               triple);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* Inspect type table: triangle should have ~19 types per bcanalyzer. */
    uint32_t num_types = 0;
    const lagfx_air_type_t *types = lagfx_air_module_types(m, &num_types);
    printf("PASS: triangle .air.bc parsed (triple='%s' num_types=%u from %s)\n",
           triple, num_types, used);
    if (num_types > 0) {
        printf("       type[0] kind=%d\n", (int)types[0].kind);
        if (num_types > 5) {
            printf("       type[5] kind=%d num_op=%u\n", (int)types[5].kind, types[5].num_op);
        }
    }
    lagfx_air_module_free(m);
    free(blob);
    return 0;
}

static int test_real_macos_metallib(void) {
    /* Try one of the captured macOS metallibs. Like the triangle test
     * above, these live in /tmp/air-bc-dump/ after running
     * triangle-extract-only on a captured .metallib. */
    const char *candidates[] = {
        "/tmp/air-bc-dump/ViewportToNDC.air.bc",
        "../scratch/captured-metallibs-2026-05-19/ViewportToNDC.air.bc",
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
        printf("SKIP: captured macOS .air.bc not found (regenerate via "
               "extract on captured-metallibs-2026-05-19/)\n");
        return 0;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || m == NULL) {
        printf("FAIL: captured macOS .air.bc open: st=%d m=%p (from %s)\n",
               (int)st, (void *)m, used);
        free(blob);
        return 1;
    }

    const char *triple = lagfx_air_module_triple(m);
    if (!triple) {
        printf("FAIL: captured macOS .air.bc missing triple\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* Captured metallibs are 'air64-apple-macosx15.7.0' (no _v28 suffix). */
    if (strncmp(triple, "air64", 5) != 0) {
        printf("FAIL: captured macOS triple doesn't start with 'air64' (got '%s')\n",
               triple);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    uint32_t num_types = 0;
    (void)lagfx_air_module_types(m, &num_types);
    printf("PASS: captured macOS .air.bc parsed (triple='%s' num_types=%u from %s)\n",
           triple, num_types, used);
    lagfx_air_module_free(m);
    free(blob);
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_open_close_smoke();
    rc |= test_bad_magic();
    rc |= test_truncated_wrapper();
    rc |= test_real_triangle_metallib();
    rc |= test_real_macos_metallib();
    return rc ? 1 : 0;
}
