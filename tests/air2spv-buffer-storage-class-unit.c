/*
 * libapplegfx-vulkan — buffer storage-class / pointee correctness regression
 * tests/air2spv-buffer-storage-class-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards two REAL macOS-guest SkyLight shaders (captured live from a running
 * macOS 15.7.5 guest) whose `[[buffer(n)]]` args have an UNWRAPPED, non-struct
 * pointee — `device packed_float2*` ([2 x float] element) and `device float4*`
 * (a v4float). Apple does not wrap these in an enclosing struct, so the prior
 * translator:
 *
 *   1. mis-classified them as TEXTURES (the addrspace==1 fallback fired for
 *      any pointee that was not a struct), and
 *   2. when it did treat them as buffers, lowered their GEP/load to a
 *      Function-class OpAccessChain into a StorageBuffer variable, or a direct
 *      OpLoad of v4float through a pointer whose pointee was the Block struct.
 *
 * Both produced spirv-val-INVALID SPIR-V:
 *   - ViewportToNDC: "result pointer storage class and base pointer storage
 *     class in OpAccessChain do not match" (Function vs StorageBuffer).
 *   - ColorFill: "OpLoad Result Type %v4float does not match Pointer %N's type".
 *
 * Fix: a non-struct buffer pointee is modelled as a `{ runtimearray<T> }`
 * StorageBuffer Block; the GEP/direct-load lower to a StorageBuffer
 * OpAccessChain (member 0, then the LLVM indices), and the direct load reads
 * element 0 through that chain.
 *
 * This test asserts, structurally (no external spirv-val needed):
 *   - every [[buffer]] arg becomes a StorageBuffer OpVariable;
 *   - NO OpAccessChain whose base is a StorageBuffer variable has a
 *     Function-class result pointer (the exact ViewportToNDC bug);
 *   - the buffer is actually read (>=1 StorageBuffer OpAccessChain).
 *
 * Fixtures: guest_viewporttondc_vert.air.bc, guest_colorfill_frag.air.bc.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC                    0x07230203u
#define OP_TYPE_POINTER              32u
#define OP_VARIABLE                  59u
#define OP_ACCESS_CHAIN              65u
#define OP_IN_BOUNDS_ACCESS_CHAIN    66u
#define STORAGE_CLASS_FUNCTION       7u
#define STORAGE_CLASS_STORAGE_BUFFER 12u

#define MAX_ID 4096u

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

/* Walk the SPIR-V module and assert the buffer storage-class invariants.
 * Returns 0 on PASS, 1 on FAIL. */
static int check_module(const uint8_t *blob, size_t sz, const char *label) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u || w[0] != SPV_MAGIC) {
        printf("FAIL[%s]: bad SPIR-V header\n", label);
        return 1;
    }

    /* id -> storage class for OpTypePointer and OpVariable results. */
    static uint8_t ptr_sc[MAX_ID];   /* OpTypePointer result-id -> storage class (+1) */
    static uint8_t var_sc[MAX_ID];   /* OpVariable result-id -> storage class (+1) */
    memset(ptr_sc, 0, sizeof(ptr_sc));
    memset(var_sc, 0, sizeof(var_sc));

    int n_sb_var = 0, n_sb_access = 0, rc = 0;

    /* Pass 1: record OpTypePointer + OpVariable storage classes. */
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        if (op == OP_TYPE_POINTER && wc >= 4u) {
            uint32_t id = w[i + 1u], scc = w[i + 2u];
            if (id < MAX_ID) ptr_sc[id] = (uint8_t)(scc + 1u);
        } else if (op == OP_VARIABLE && wc >= 4u) {
            uint32_t id = w[i + 2u], scc = w[i + 3u];
            if (id < MAX_ID) var_sc[id] = (uint8_t)(scc + 1u);
            if (scc == STORAGE_CLASS_STORAGE_BUFFER) n_sb_var++;
        }
        i += wc;
    }

    /* Pass 2: for every OpAccessChain, the result-pointer storage class must
     * equal the base variable's storage class. The exact bug: a Function
     * result pointer based on a StorageBuffer variable. */
    i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 4u) {
            uint32_t res_ty = w[i + 1u];
            uint32_t base   = w[i + 3u];
            uint32_t res_scc  = (res_ty < MAX_ID && ptr_sc[res_ty]) ? ptr_sc[res_ty] - 1u : 0xFFu;
            uint32_t base_scc = (base < MAX_ID && var_sc[base]) ? var_sc[base] - 1u : 0xFFu;
            if (base_scc == STORAGE_CLASS_STORAGE_BUFFER) {
                n_sb_access++;
                if (res_scc != STORAGE_CLASS_STORAGE_BUFFER) {
                    printf("FAIL[%s]: OpAccessChain into StorageBuffer var %%%u has "
                           "result pointer storage class %u (expected %u). The "
                           "storage-class-mismatch bug regressed.\n",
                           label, base, res_scc, STORAGE_CLASS_STORAGE_BUFFER);
                    rc = 1;
                }
            }
        }
        i += wc;
    }

    printf("[%s] StorageBuffer vars=%d, StorageBuffer access-chains=%d (%zu bytes)\n",
           label, n_sb_var, n_sb_access, sz);
    if (n_sb_var < 1) {
        printf("FAIL[%s]: no StorageBuffer OpVariable — the non-struct "
               "[[buffer(n)]] arg was mis-classified (texture) or SKIP'd.\n", label);
        rc = 1;
    }
    if (n_sb_access < 1) {
        printf("FAIL[%s]: no StorageBuffer OpAccessChain — the buffer is never "
               "read through the block (GEP/load not lowered).\n", label);
        rc = 1;
    }
    if (rc == 0)
        printf("PASS[%s]: non-struct buffer reads are StorageBuffer-class, "
               "no Function access chain into a StorageBuffer var.\n", label);
    return rc;
}

static int run_one(const char *name, const char *const *cands) {
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: %s fixture not found\n", name); return 1; }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL[%s]: module open\n", name); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL[%s]: translate\n", name); lagfx_air_module_free(m); free(air); return 1;
    }
    int rc = check_module(spv, spv_sz, name);
    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    const char *vert[] = {
        "tests/fixtures/guest_viewporttondc_vert.air.bc",
        "../tests/fixtures/guest_viewporttondc_vert.air.bc",
        SRCDIR "/fixtures/guest_viewporttondc_vert.air.bc",
        NULL,
    };
    const char *frag[] = {
        "tests/fixtures/guest_colorfill_frag.air.bc",
        "../tests/fixtures/guest_colorfill_frag.air.bc",
        SRCDIR "/fixtures/guest_colorfill_frag.air.bc",
        NULL,
    };
    int rc = 0;
    rc |= run_one("ViewportToNDC(vert)", vert);
    rc |= run_one("ColorFill(frag)", frag);
    return rc;
}
