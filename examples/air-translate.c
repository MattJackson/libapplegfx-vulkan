/*
 * libapplegfx-vulkan — offline AIR(.air.bc) → SPIR-V translate driver
 * examples/air-translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs the real translator (lagfx_air2spv_translate_module, which calls
 * the per-instruction body walker with a reference-emitter fallback) on
 * a retargeted .air.bc and writes the SPIR-V to argv[2]. Enables OFFLINE
 * testing of real captured macOS shaders through our pipeline — no
 * guest, no noVNC. Pair with `spirv-val <out.spv>` to validate.
 *
 *   usage: air-translate <in.air.bc> <out.spv>
 *   exit:  0 = translated + SPIR-V written; 1 = open/translate failed.
 */
#include "air/bitcode_reader.h"
#include "air2spv/translate.h"
#include "libapplegfx-vulkan.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.air.bc> <out.spv>\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    uint8_t *blob = slurp(argv[1], &len);
    if (!blob) { fprintf(stderr, "error: cannot read '%s'\n", argv[1]); return 1; }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || !m) {
        fprintf(stderr, "error: lagfx_air_module_open st=%d\n", (int)st);
        free(blob);
        return 1;
    }

    uint8_t *spv = NULL;
    size_t spv_len = 0;
    st = lagfx_air2spv_translate_module(m, &spv, &spv_len);
    if (st != LAGFX_OK || !spv) {
        fprintf(stderr, "error: lagfx_air2spv_translate_module st=%d\n", (int)st);
        lagfx_air_module_free(m);
        free(blob);
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(spv, 1, spv_len, out) != spv_len) {
        fprintf(stderr, "error: cannot write '%s'\n", argv[2]);
        if (out) fclose(out);
        free(spv); lagfx_air_module_free(m); free(blob);
        return 1;
    }
    fclose(out);
    printf("translated %s -> %s (%zu SPIR-V bytes)\n", argv[1], argv[2], spv_len);

    free(spv);
    lagfx_air_module_free(m);
    free(blob);
    return 0;
}
