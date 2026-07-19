/*
 * libapplegfx-vulkan — fragment colour-output store regression
 * tests/air2spv-fragment-color-store-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Regression guard for the fragment RET → colour-output store bug.
 *
 * emit_inst_ret() handled ONLY the vertex stage: it extracted the
 * returned struct's field 0 and OpStore'd it to the BuiltIn Position.
 * The fragment stage fell through to a bare OpReturn — the function
 * computed its colour constant but NEVER wrote it to the Location-0
 * colour Output variable. The body came out as `OpLabel / OpReturn`
 * with no OpStore, so the fragment colour was undefined and the
 * geometry rendered BLANK (the e2e centre pixel stayed the clear
 * colour instead of red — even though the vertex positions were
 * already correct and spirv-val passed the empty body).
 *
 * Fixture: triangle_fragment (`fragment float4 f() { return
 * float4(1,0,0,1); }`) compiled with `xcrun metal`. Assert the
 * translated fragment emits at least one OpStore (the colour write)
 * and that its target is the OpEntryPoint-declared Output variable.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC      0x07230203u
#define SPV_OP_STORE   62u
#define SPV_OP_ENTRYPT 15u

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

/* Walk the SPIR-V instruction stream. For every OpStore, record the
 * target id (first operand). For the OpEntryPoint, collect the
 * interface var ids (operands after the name). Returns 1 if at least
 * one OpStore targets an interface (Output) variable. */
static int store_targets_interface(const uint8_t *blob, size_t sz,
                                    int *out_n_store) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    *out_n_store = 0;
    if (nwords < 5u) return 0;

    uint32_t iface[32]; size_t n_iface = 0;
    uint32_t stores[64]; size_t n_stores = 0;

    size_t i = 5u;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;
        if (op == SPV_OP_ENTRYPT) {
            /* OpEntryPoint: ExecModel, entry-id, name(string...), iface... */
            /* word layout: [hdr][execmodel][entryid][name...][iface...].
             * Find the end of the literal string (NUL-terminated, packed
             * 4 bytes/word) then the rest are interface ids. */
            size_t p = i + 3u;          /* skip hdr, execmodel, entryid */
            while (p < i + wc) {
                uint32_t word = w[p];
                p++;
                if (((word >> 24) & 0xFFu) == 0u ||
                    (word & 0xFFu) == 0u || ((word >> 8) & 0xFFu) == 0u ||
                    ((word >> 16) & 0xFFu) == 0u) {
                    /* a NUL byte terminates the name in this word */
                    break;
                }
            }
            for (; p < i + wc && n_iface < 32; p++) iface[n_iface++] = w[p];
        } else if (op == SPV_OP_STORE) {
            if (n_stores < 64) stores[n_stores++] = w[i + 1u];
        }
        i += wc;
    }
    *out_n_store = (int)n_stores;
    for (size_t s = 0; s < n_stores; s++)
        for (size_t k = 0; k < n_iface; k++)
            if (stores[s] == iface[k]) return 1;
    return 0;
}

static int test_fragment_color_store(void) {
    const char *cands[] = {
        "tests/fixtures/triangle_fragment.air.bc",
        "../tests/fixtures/triangle_fragment.air.bc",
        SRCDIR "/fixtures/triangle_fragment.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: triangle_fragment.air.bc fixture not found\n"); return 1; }

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

    int rc = 0;
    int n_store = 0;
    int hits_iface = store_targets_interface(spv, spv_sz, &n_store);
    printf("OpStore count=%d, store-targets-Output-interface=%d (%zu spv bytes)\n",
           n_store, hits_iface, spv_sz);
    if (n_store < 1) {
        printf("FAIL: fragment body emitted ZERO OpStore — the RET colour "
               "value was never written to the Location-0 colour output. "
               "Fragment renders blank.\n");
        rc = 1;
    }
    if (!hits_iface) {
        printf("FAIL: no OpStore targets an OpEntryPoint interface (Output) "
               "variable — the fragment colour write regressed.\n");
        rc = 1;
    }
    if (rc == 0)
        printf("PASS: fragment RET stores its colour value to the "
               "Location-0 Output variable\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_fragment_color_store();
}
