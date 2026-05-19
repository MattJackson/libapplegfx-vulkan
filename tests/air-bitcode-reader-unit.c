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
    uint32_t num_funcs = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &num_funcs);
    printf("PASS: triangle .air.bc parsed (triple='%s' num_types=%u num_funcs=%u from %s)\n",
           triple, num_types, num_funcs, used);
    if (num_types > 0) {
        printf("       type[0] kind=%d\n", (int)types[0].kind);
        if (num_types > 5) {
            printf("       type[5] kind=%d num_op=%u\n", (int)types[5].kind, types[5].num_op);
        }
    }
    /* Triangle has 3 FUNCTION declarations: one body ("triangle_vertex")
     * and two intrinsic prototypes. Phase 2 step 1 added STRTAB-aware
     * record parsing + body_offset stashing; verify both are correct. */
    if (num_funcs != 3) {
        printf("FAIL: expected num_funcs=3, got %u\n", num_funcs);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    uint32_t protos = 0, with_body = 0;
    for (uint32_t i = 0; i < num_funcs; i++) {
        if (fns[i].is_proto) protos++;
        else with_body++;
        printf("       fn[%u] type_index=%u is_proto=%d body_offset=%zu body_length=%zu\n",
               i, fns[i].type_index, (int)fns[i].is_proto,
               fns[i].body_offset, fns[i].body_length);
    }
    if (with_body != 1 || protos != 2) {
        printf("FAIL: triangle expected 1 body + 2 protos, got %u + %u\n",
               with_body, protos);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* The one non-proto must have a populated body_offset/body_length. */
    for (uint32_t i = 0; i < num_funcs; i++) {
        if (!fns[i].is_proto && (fns[i].body_offset == 0 || fns[i].body_length == 0)) {
            printf("FAIL: fn[%u] non-proto missing body_offset/length\n", i);
            lagfx_air_module_free(m);
            free(blob);
            return 1;
        }
    }
    uint32_t num_pag = 0;
    (void)lagfx_air_module_param_attr_groups(m, &num_pag);
    uint32_t num_consts = 0;
    (void)lagfx_air_module_constants(m, &num_consts);
    printf("       paramattr_groups=%u  constants=%u\n", num_pag, num_consts);
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
    uint32_t num_funcs = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &num_funcs);
    printf("PASS: captured macOS .air.bc parsed (triple='%s' num_types=%u num_funcs=%u from %s)\n",
           triple, num_types, num_funcs, used);
    /* ViewportToNDC is a compiled shader: 1 FUNCTION declaration with a
     * body (no intrinsic prototypes — those would appear if the shader
     * called any air.* helpers). */
    if (num_funcs != 1) {
        printf("FAIL: expected num_funcs=1 for ViewportToNDC, got %u\n", num_funcs);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    printf("       fn[0] type_index=%u is_proto=%d linkage=%u body_offset=%zu body_length=%zu\n",
           fns[0].type_index, (int)fns[0].is_proto, fns[0].linkage,
           fns[0].body_offset, fns[0].body_length);
    if (fns[0].is_proto) {
        printf("FAIL: ViewportToNDC fn[0] should have a body (is_proto=0)\n");
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }
    /* body_offset stashing for ViewportToNDC requires the MODULE walker
     * to successfully traverse the OPERAND_BUNDLE_TAGS_BLOCK that sits
     * BEFORE FUNCTION_BLOCK in Apple's emission order. That block uses
     * Apple-custom abbrev encoding — bcanalyzer dies there too, see
     * tail of /tmp/viewport_dump.txt with "Invalid abbrev number".
     * Tracked under Phase 2 step 4. Triangle's MODULE order puts
     * OPERAND_BUNDLE_TAGS AFTER FUNCTION_BLOCK, so triangle works. */
    if (fns[0].body_offset != 0) {
        printf("       NOTE: body_offset stash works for ViewportToNDC — Phase 2.4 progress\n");
    } else {
        printf("       NOTE: body_offset=0 expected (Apple OPERAND_BUNDLE_TAGS decode pending — Phase 2.4)\n");
    }
    lagfx_air_module_free(m);
    free(blob);
    return 0;
}

static int test_file_mode(const char *filepath) {
    size_t len = 0;
    uint8_t *blob = slurp(filepath, &len);
    if (!blob) {
        printf("ERROR: failed to open '%s'\n", filepath);
        return 1;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || m == NULL) {
        printf("ERROR: open failed (st=%d)\n", (int)st);
        free(blob);
        return 1;
    }

    const char *triple = lagfx_air_module_triple(m);
    uint32_t num_types = 0;
    (void)lagfx_air_module_types(m, &num_types);
    uint32_t num_funcs = 0;
    (void)lagfx_air_module_functions(m, &num_funcs);
    uint32_t num_pag = 0;
    (void)lagfx_air_module_param_attr_groups(m, &num_pag);
    uint32_t num_consts = 0;
    (void)lagfx_air_module_constants(m, &num_consts);

    const char *dl = lagfx_air_module_datalayout(m);
    printf("triple='%s' datalayout='%s' num_types=%u num_funcs=%u paramattr_groups=%u constants=%u\n",
           triple ? triple : "(none)", dl ? dl : "(none)",
           num_types, num_funcs, num_pag, num_consts);

    lagfx_air_module_free(m);
    free(blob);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--file") == 0) {
        return test_file_mode(argv[2]);
    }

    int rc = 0;
    rc |= test_open_close_smoke();
    rc |= test_bad_magic();
    rc |= test_truncated_wrapper();
    rc |= test_real_triangle_metallib();
    rc |= test_real_macos_metallib();
    return rc ? 1 : 0;
}
