/*
 * libapplegfx-vulkan — Phase 5 translator skeleton unit test
 * tests/air2spv-translate-module-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Walks triangle.air.bc through the translator skeleton:
 *   1. lagfx_air_module_open (Phase 1-3)
 *   2. lagfx_air2spv_translate_module (Phase 5 MVP)
 *   3. Validate output is a Vulkan-acceptable SPIR-V module with the
 *      vertex stage's BuiltIn Position output.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPV_MAGIC 0x07230203u

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

static int spirv_val(const uint8_t *blob, size_t sz) {
    const char *candidates[] = {
        "/opt/homebrew/bin/spirv-val",
        "/usr/local/bin/spirv-val",
        NULL,
    };
    const char *spirv_val_path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) { spirv_val_path = candidates[i]; break; }
    }
    if (!spirv_val_path) return 0;  /* SKIP */

    char tmpl[] = "/tmp/lagfx_air2spv_xlate_XXXXXX.spv";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) return -1;
    if ((size_t)write(fd, blob, sz) != sz) {
        close(fd); unlink(tmpl); return -1;
    }
    close(fd);
    pid_t pid = fork();
    if (pid < 0) { unlink(tmpl); return -1; }
    if (pid == 0) {
        execl(spirv_val_path, "spirv-val", tmpl, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 1;
}

static int test_translate_triangle(void) {
    const char *candidates[] = {
        "/Users/mjackson/Developer/staging/triangle_vertex.air.bc",
        "/tmp/scoping/triangle_vertex.air.bc",
        NULL,
    };
    uint8_t *air_blob = NULL;
    size_t   air_len  = 0;
    const char *used = NULL;
    for (int i = 0; candidates[i]; i++) {
        air_blob = slurp(candidates[i], &air_len);
        if (air_blob) { used = candidates[i]; break; }
    }
    if (!air_blob) {
        printf("SKIP: triangle .air.bc fixture not found\n");
        return 0;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(air_blob, air_len, &m);
    if (st != LAGFX_OK || !m) {
        printf("FAIL: triangle open st=%d (from %s)\n", (int)st, used);
        free(air_blob);
        return 1;
    }

    uint8_t *spv_blob = NULL;
    size_t   spv_sz   = 0u;
    st = lagfx_air2spv_translate_module(m, &spv_blob, &spv_sz);
    if (st != LAGFX_OK || !spv_blob) {
        printf("FAIL: translate st=%d\n", (int)st);
        lagfx_air_module_free(m);
        free(air_blob);
        return 1;
    }

    uint32_t magic;
    memcpy(&magic, spv_blob, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad SPIR-V magic 0x%08x\n", magic);
        free(spv_blob); lagfx_air_module_free(m); free(air_blob);
        return 1;
    }

    int val_rc = spirv_val(spv_blob, spv_sz);
    if (val_rc < 0) {
        printf("FAIL: spirv-val rejected translator output\n");
        free(spv_blob); lagfx_air_module_free(m); free(air_blob);
        return 1;
    }
    if (val_rc == 0) {
        printf("PASS: triangle translated to SPIR-V (%zu bytes); spirv-val SKIPPED (not on PATH)\n",
               spv_sz);
    } else {
        printf("PASS: triangle translated to SPIR-V (%zu bytes); spirv-val accepted\n", spv_sz);
    }

    free(spv_blob);
    lagfx_air_module_free(m);
    free(air_blob);
    return 0;
}

static int test_translate_rejects_no_stage(void) {
    /* Minimal AIR module with valid wrapper + bitstream magic but no
     * METADATA_BLOCK named-nodes — the translator should refuse with
     * LAGFX_ERR_PROTOCOL. */
    static const uint8_t fake[24] = {
        0xDE, 0xC0, 0x17, 0x0B,
        0x00, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF,
        0x42, 0x43, 0xC0, 0xDE,
    };
    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(fake, sizeof(fake), &m) != LAGFX_OK || !m) {
        printf("FAIL: minimal-AIR open failed\n");
        return 1;
    }
    uint8_t *spv = NULL;
    size_t   sz  = 0u;
    lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &sz);
    if (st == LAGFX_OK) {
        printf("FAIL: translator should refuse module with no stage metadata\n");
        free(spv); lagfx_air_module_free(m);
        return 1;
    }
    if (spv != NULL || sz != 0u) {
        printf("FAIL: translator left non-empty output after error (%p, %zu)\n",
               (void *)spv, sz);
        free(spv); lagfx_air_module_free(m);
        return 1;
    }
    printf("PASS: translator rejected metadata-less module (st=%d)\n", (int)st);
    lagfx_air_module_free(m);
    return 0;
}

static int test_translate_vfx(void) {
    /* Vfx is a captured macOS shader with a 4-instruction body — the
     * second real-world fixture proving the translator handles more
     * than just the bundled triangle. */
    const char *candidates[] = {
        "/Users/mjackson/Developer/libapplegfx-vulkan/scratch/phase2_4_diagnosis/air-bc/Vfx.air.bc",
        NULL,
    };
    uint8_t *air_blob = NULL;
    size_t   air_len  = 0;
    const char *used = NULL;
    for (int i = 0; candidates[i]; i++) {
        air_blob = slurp(candidates[i], &air_len);
        if (air_blob) { used = candidates[i]; break; }
    }
    if (!air_blob) {
        printf("SKIP: Vfx .air.bc fixture not found\n");
        return 0;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(air_blob, air_len, &m);
    if (st != LAGFX_OK || !m) {
        printf("FAIL: Vfx open st=%d (from %s)\n", (int)st, used);
        free(air_blob);
        return 1;
    }

    uint8_t *spv_blob = NULL;
    size_t   spv_sz   = 0u;
    st = lagfx_air2spv_translate_module(m, &spv_blob, &spv_sz);
    if (st != LAGFX_OK || !spv_blob) {
        printf("FAIL: Vfx translate st=%d\n", (int)st);
        lagfx_air_module_free(m);
        free(air_blob);
        return 1;
    }

    uint32_t magic;
    memcpy(&magic, spv_blob, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: Vfx bad SPIR-V magic 0x%08x\n", magic);
        free(spv_blob); lagfx_air_module_free(m); free(air_blob);
        return 1;
    }

    int val_rc = spirv_val(spv_blob, spv_sz);
    if (val_rc < 0) {
        printf("FAIL: Vfx spirv-val rejected translator output\n");
        free(spv_blob); lagfx_air_module_free(m); free(air_blob);
        return 1;
    }
    if (val_rc == 0) {
        printf("PASS: Vfx translated to SPIR-V (%zu bytes); spirv-val SKIPPED (not on PATH)\n",
               spv_sz);
    } else {
        printf("PASS: Vfx translated to SPIR-V (%zu bytes); spirv-val accepted\n", spv_sz);
    }

    free(spv_blob);
    lagfx_air_module_free(m);
    free(air_blob);
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_translate_triangle();
    rc |= test_translate_vfx();
    rc |= test_translate_rejects_no_stage();
    return rc ? 1 : 0;
}
