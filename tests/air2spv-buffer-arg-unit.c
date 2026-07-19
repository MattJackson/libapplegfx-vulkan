/*
 * libapplegfx-vulkan — [[buffer(n)]] argument (StorageBuffer Block) regression
 * tests/air2spv-buffer-arg-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Guards Metal constant/device BUFFER argument support (coverage audit
 * 2026-05-30, real SkyLight corpus). A `[[buffer(n)]]` arg is a struct
 * pointer (addrspace 1/2) accessed via getelementptr + load. Previously the
 * arg was SKIP'd (textures/samplers handled, buffers not), so the GEP base
 * was unbound and the translator synthesized an illegal mid-body
 * OpTypePointer Function. Now a buffer arg becomes a StorageBuffer Block
 * variable (std430 member Offsets, DescriptorSet 0 + Binding, in
 * OpEntryPoint's interface), the GEP lowers to OpAccessChain in the
 * StorageBuffer storage class, and the load reads the real buffer.
 *
 * Distinguishing buffers from samplers matters: both are addrspace 2 — the
 * disambiguator is the pointee (a buffer points at a data STRUCT; a
 * texture/sampler at an opaque type).
 *
 * Fixture: InPlaceSover (real SkyLight fragment, `constant FragmentArgs* [[
 * buffer(0)]]` with a float4) compiled by Apple's metalfe.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC                   0x07230203u
#define OP_VARIABLE                 59u
#define OP_ACCESS_CHAIN             65u
#define STORAGE_CLASS_STORAGE_BUFFER 12u

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

static void count(const uint8_t *blob, size_t sz, int *n_sb_var, int *n_access) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *n_sb_var = 0; *n_access = 0;
    if (nwords < 5u) return;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_VARIABLE && wc >= 4u && w[i + 3u] == STORAGE_CLASS_STORAGE_BUFFER)
            (*n_sb_var)++;
        else if (op == OP_ACCESS_CHAIN)
            (*n_access)++;
        i += wc;
    }
}

static int test_buffer_arg(void) {
    const char *cands[] = {
        "tests/fixtures/inplacesover_buffer.air.bc",
        "../tests/fixtures/inplacesover_buffer.air.bc",
        SRCDIR "/fixtures/inplacesover_buffer.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: inplacesover_buffer.air.bc fixture not found\n"); return 1; }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open\n"); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL: translate\n"); lagfx_air_module_free(m); free(air); return 1;
    }
    uint32_t magic; memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad magic 0x%08x\n", magic);
        free(spv); lagfx_air_module_free(m); free(air); return 1;
    }

    int rc = 0, n_sb = 0, n_ac = 0;
    count(spv, spv_sz, &n_sb, &n_ac);
    printf("StorageBuffer OpVariable count=%d, OpAccessChain count=%d (%zu spv bytes)\n",
           n_sb, n_ac, spv_sz);

    if (n_sb < 1) {
        printf("FAIL: no StorageBuffer OpVariable — the [[buffer(n)]] arg was "
               "not modelled as a buffer block (mis-classified as sampler, or "
               "SKIP'd).\n");
        rc = 1;
    }
    if (n_ac < 1) {
        printf("FAIL: no OpAccessChain — the buffer GEP did not lower into the "
               "block.\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: [[buffer(n)]] arg lowers to a StorageBuffer Block + "
               "AccessChain\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_buffer_arg();
}
