/*
 * libapplegfx-vulkan — AIR arg-metadata Metal-index map (unit)
 * tests/air2spv-arg-bindings-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Red-on-revert guard for lagfx_air_arg_bindings(): the authoritative
 * binding→Metal-resource-index map extracted from the entry point's
 * air.vertex / air.fragment per-arg metadata (air.location_index).
 *
 * Ground truth (real 15.7.5 SkyLight login shaders, hand-decoded from the
 * metadata graph and cross-validated against llvm.module.flags):
 *   UberCompositeVertex — 1 buffer arg:  mvp_matrix  = [[buffer(1)]]
 *   Vfx                 — 1 buffer arg:  [[buffer(2)]]
 *   ViewportToNDC       — 3 buffer args: [[buffer(0)]],[[buffer(1)]],[[buffer(2)]]
 *   UberCompositeFragment (both) — texture(0), buffer(0), sampler(0)
 *
 * The Metal index VARIES per shader — this map is what makes draw-time
 * binding correct without the MTXSCAN/skip-one heuristics (the M2v smear:
 * the 64 B mvp at index 2 was uploaded as the stage-in vertex stream).
 */

#include "air/bitcode_reader.h"
#include "air2spv/translate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SRCDIR
#define SRCDIR "tests"
#endif

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

static uint8_t *find_fixture(const char *basename, size_t *len) {
    char p[512];
    const char *dirs[] = { "tests/fixtures", "../tests/fixtures",
                           SRCDIR "/fixtures", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", dirs[i], basename);
        uint8_t *b = slurp(p, len);
        if (b) return b;
    }
    return NULL;
}

typedef struct {
    const char *name, *file;
    size_t      n;
    uint8_t     kind[4];
    int16_t     midx[4];
} exp_t;

static const exp_t g_cases[] = {
    { "UberCompositeVertex", "login_ubercomposite_vert.air.bc",
      1, { 1 },       { 1 } },
    { "Vfx",                 "login_vfx_vert.air.bc",
      1, { 1 },       { 2 } },
    { "ViewportToNDC",       "login_viewporttondc_vert.air.bc",
      3, { 1, 1, 1 }, { 0, 1, 2 } },
    { "UberCompositeFrag_a", "login_ubercomposite_frag_a.air.bc",
      3, { 2, 1, 3 }, { 0, 0, 0 } },
};

int main(void) {
    int fails = 0;
    for (size_t c = 0; c < sizeof(g_cases) / sizeof(g_cases[0]); c++) {
        const exp_t *e = &g_cases[c];
        size_t air_len = 0;
        uint8_t *air = find_fixture(e->file, &air_len);
        if (!air) { printf("FAIL[%s]: fixture missing\n", e->name); fails++; continue; }
        lagfx_air_module_t *m = NULL;
        if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
            printf("FAIL[%s]: module open\n", e->name); free(air); fails++; continue;
        }
        lagfx_air_arg_binding_t ab[8];
        size_t n = lagfx_air_arg_bindings(m, ab, 8);
        if (n != e->n) {
            printf("FAIL[%s]: expected %zu resource args, got %zu\n", e->name, e->n, n);
            fails++;
        } else {
            for (size_t k = 0; k < n; k++) {
                if (ab[k].kind != e->kind[k] || ab[k].metal_index != e->midx[k]) {
                    printf("FAIL[%s]: arg %zu = kind %u idx %d, expected kind %u idx %d\n",
                           e->name, k, ab[k].kind, (int)ab[k].metal_index,
                           e->kind[k], (int)e->midx[k]);
                    fails++;
                }
            }
        }
        lagfx_air_module_free(m);
        free(air);
        if (!fails) printf("ok [%s]\n", e->name);
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("all arg-binding maps correct\n");
    return 0;
}
