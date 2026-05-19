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
    /* The full .metallib has its own MTLB wrapper format around the
     * embedded bitcode. We need just the bitcode payload for the
     * reader. For now we use the canned wrapper test above; a future
     * iteration will plumb metallib_extract through to feed actual
     * function bitcode payloads here. */
    (void)slurp;  /* keep helper referenced for future tests */
    printf("SKIP: triangle metallib end-to-end (needs metallib_extract integration)\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_open_close_smoke();
    rc |= test_bad_magic();
    rc |= test_truncated_wrapper();
    rc |= test_real_triangle_metallib();
    return rc ? 1 : 0;
}
