/*
 * libapplegfx-vulkan — nested Block struct layout regression
 * tests/air2spv-nested-block-offset-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Guards std430 layout of NESTED aggregates inside a [[buffer(n)]] Block
 * (real SkyLight corpus, coverage audit 2026-05-30 v2). A buffer struct
 * whose members are themselves structs (struct-of-struct) or arrays must
 * have Offset decorations on EVERY transitively-reachable struct's members
 * and ArrayStride on every array — not just the top-level Block. Previously
 * `emit_type_struct_block` decorated only the top struct + direct array
 * members, so a nested struct member reached emission undecorated and
 * spirv-val rejected: "Structure id N decorated as Block must be explicitly
 * laid out with Offset decorations."
 *
 * This test parses the translated SPIR-V, finds every struct used (directly
 * or transitively) as a member of a Block-decorated struct, and asserts each
 * such nested struct carries at least one OpMemberDecorate Offset, and every
 * array carries an ArrayStride.
 *
 * Fixture: SimpleTextureLightingVertex (real SkyLight vertex shader; its
 * [[buffer]] arg is a struct { struct { float4[4] } x2 }).
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SPV_MAGIC            0x07230203u
#define OP_DECORATE          71u
#define OP_MEMBER_DECORATE   72u
#define OP_TYPE_ARRAY        28u
#define OP_TYPE_STRUCT       30u
#define DECOR_BLOCK          2u
#define DECOR_ARRAY_STRIDE   6u
#define DECOR_OFFSET         35u

#define MAXID 4096u

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

/* member type ids of each struct (up to 32 members), -1 sentinel */
static int32_t struct_members[MAXID][32];
static int     struct_nmembers[MAXID];
static uint8_t is_struct[MAXID];
static uint8_t is_array[MAXID];
static int32_t array_elem[MAXID];
static uint8_t is_block[MAXID];
static uint8_t has_member_offset[MAXID];
static uint8_t has_array_stride[MAXID];
static uint8_t reachable_in_block[MAXID];

static void mark_reachable(uint32_t id) {
    if (id >= MAXID || reachable_in_block[id]) return;
    reachable_in_block[id] = 1;
    if (is_struct[id]) {
        for (int i = 0; i < struct_nmembers[id]; i++) {
            int32_t m = struct_members[id][i];
            if (m >= 0) mark_reachable((uint32_t)m);
        }
    } else if (is_array[id] && array_elem[id] >= 0) {
        mark_reachable((uint32_t)array_elem[id]);
    }
}

static int test_nested_block(void) {
    const char *cands[] = {
        "tests/fixtures/simpletexlighting_vertex.air.bc",
        "../tests/fixtures/simpletexlighting_vertex.air.bc",
        SRCDIR "/fixtures/simpletexlighting_vertex.air.bc",
        NULL,
    };
    uint8_t *air = NULL; size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: simpletexlighting_vertex.air.bc fixture not found\n"); return 1; }

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

    const uint32_t *w = (const uint32_t *)(const void *)spv;
    size_t nwords = spv_sz / 4u;
    int rc = 0;

    /* Pass 1: collect type structure + decorations. */
    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_TYPE_STRUCT && wc >= 2u) {
            uint32_t id = w[i + 1u];
            if (id < MAXID) {
                is_struct[id] = 1;
                int nm = (int)wc - 2;
                if (nm > 32) nm = 32;
                struct_nmembers[id] = nm;
                for (int k = 0; k < nm; k++) struct_members[id][k] = (int32_t)w[i + 2u + (size_t)k];
            }
        } else if (op == OP_TYPE_ARRAY && wc >= 3u) {
            uint32_t id = w[i + 1u];
            if (id < MAXID) { is_array[id] = 1; array_elem[id] = (int32_t)w[i + 2u]; }
        } else if (op == OP_DECORATE && wc >= 3u) {
            uint32_t id = w[i + 1u], dec = w[i + 2u];
            if (id < MAXID && dec == DECOR_BLOCK) is_block[id] = 1;
            if (id < MAXID && dec == DECOR_ARRAY_STRIDE) has_array_stride[id] = 1;
        } else if (op == OP_MEMBER_DECORATE && wc >= 4u) {
            uint32_t id = w[i + 1u], dec = w[i + 3u];
            if (id < MAXID && dec == DECOR_OFFSET) has_member_offset[id] = 1;
        }
        i += wc;
    }

    /* Pass 2: from each Block struct, mark all transitively-reachable types. */
    int nblocks = 0;
    for (uint32_t id = 0; id < MAXID; id++) {
        if (is_block[id]) {
            nblocks++;
            for (int k = 0; k < struct_nmembers[id]; k++) {
                int32_t mm = struct_members[id][k];
                if (mm >= 0) mark_reachable((uint32_t)mm);
            }
        }
    }
    if (nblocks == 0) {
        printf("FAIL: no Block-decorated struct found — fixture changed?\n");
        free(spv); lagfx_air_module_free(m); free(air); return 1;
    }

    /* Pass 3: every struct reachable inside a Block (and the Block itself)
     * must have member Offsets; every reachable array must have ArrayStride. */
    int n_nested_structs = 0;
    for (uint32_t id = 0; id < MAXID; id++) {
        if (is_block[id] && !has_member_offset[id]) {
            printf("FAIL: Block struct id %u has no member Offset\n", id); rc = 1;
        }
        if (reachable_in_block[id] && is_struct[id]) {
            n_nested_structs++;
            if (!has_member_offset[id]) {
                printf("FAIL: nested struct id %u (in a Block) lacks Offset "
                       "decorations\n", id);
                rc = 1;
            }
        }
        if (reachable_in_block[id] && is_array[id] && !has_array_stride[id]) {
            printf("FAIL: array id %u (in a Block) lacks ArrayStride\n", id);
            rc = 1;
        }
    }

    if (rc == 0)
        printf("PASS: %d Block struct(s), %d nested member-struct(s) all laid "
               "out with Offset/ArrayStride\n", nblocks, n_nested_structs);

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_nested_block();
}
