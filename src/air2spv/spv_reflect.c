/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * libapplegfx-vulkan — SPIR-V descriptor-binding reflection
 * src/air2spv/spv_reflect.c
 *
 * Copyright © 2026 Matthew Jackson
 */
#include "spv_reflect.h"

#include "common/le.h"

#include <stdlib.h>

#define SPV_MAGIC                0x07230203u
#define OP_DECORATE              71u
#define OP_TYPE_IMAGE            25u
#define OP_TYPE_SAMPLER          26u
#define OP_TYPE_SAMPLED_IMAGE    27u
#define OP_TYPE_STRUCT           30u
#define OP_TYPE_POINTER          32u
#define OP_VARIABLE              59u
#define OP_TYPE_FLOAT            22u
#define OP_TYPE_VECTOR           23u
#define DECOR_BINDING            33u
#define DECOR_DESCRIPTOR_SET     34u
#define DECOR_LOCATION           30u
#define STORAGE_UNIFORM_CONSTANT 0u
#define STORAGE_INPUT            1u
#define STORAGE_UNIFORM          2u
#define STORAGE_STORAGE_BUFFER   12u

/* Bounded id-indexed scratch. SPIR-V ids are dense and small for our
 * translator output; cap generously and ignore anything past it. */
#define MAXID 8192u

size_t lagfx_spv_reflect_bindings(const uint8_t *spv, size_t spv_len,
                                  lagfx_spv_binding_t *out, size_t cap) {
    if (!spv || spv_len < 20u) return 0;
    const uint8_t *p = spv;
    if (lagfx_le32(p) != SPV_MAGIC) return 0;
    size_t nwords = spv_len / 4u;
    size_t total = 0; /* declared up front: the OOM goto must not skip it */

    /* Use calloc'd arrays sized to MAXID, allocated on the heap to keep
     * the stack small. */
    uint8_t  *has_set = NULL, *has_bind = NULL, *is_var = NULL;
    uint32_t *dec_set = NULL, *dec_bind = NULL;
    uint32_t *var_ptr = NULL, *var_sc = NULL;         /* OpVariable: ptr type id, storage class */
    uint32_t *ptr_pointee = NULL;                     /* OpTypePointer: pointee id */
    uint8_t  *is_image = NULL, *is_sampler = NULL, *is_struct = NULL, *is_sampled_image = NULL;

    size_t sz = MAXID;
    has_set = calloc(sz, 1); has_bind = calloc(sz, 1); is_var = calloc(sz, 1);
    dec_set = calloc(sz, sizeof(uint32_t)); dec_bind = calloc(sz, sizeof(uint32_t));
    var_ptr = calloc(sz, sizeof(uint32_t)); var_sc = calloc(sz, sizeof(uint32_t));
    ptr_pointee = calloc(sz, sizeof(uint32_t));
    is_image = calloc(sz, 1); is_sampler = calloc(sz, 1); is_struct = calloc(sz, 1);
    is_sampled_image = calloc(sz, 1);
    if (!has_set || !has_bind || !is_var || !dec_set || !dec_bind || !var_ptr ||
        !var_sc || !ptr_pointee || !is_image || !is_sampler || !is_struct ||
        !is_sampled_image) {
        goto done; /* OOM -> report 0 */
    }

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (i + wc > nwords) break;
        #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
        switch (op) {
            case OP_DECORATE:
                if (wc >= 4u) {
                    uint32_t id = W(1), dec = W(2);
                    if (id < MAXID) {
                        if (dec == DECOR_DESCRIPTOR_SET) { has_set[id] = 1; dec_set[id] = W(3); }
                        else if (dec == DECOR_BINDING)   { has_bind[id] = 1; dec_bind[id] = W(3); }
                    }
                }
                break;
            case OP_TYPE_IMAGE:
                if (wc >= 2u && W(1) < MAXID) is_image[W(1)] = 1;
                break;
            case OP_TYPE_SAMPLER:
                if (wc >= 2u && W(1) < MAXID) is_sampler[W(1)] = 1;
                break;
            case OP_TYPE_SAMPLED_IMAGE:
                if (wc >= 2u && W(1) < MAXID) is_sampled_image[W(1)] = 1;
                break;
            case OP_TYPE_STRUCT:
                if (wc >= 2u && W(1) < MAXID) is_struct[W(1)] = 1;
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && W(1) < MAXID) ptr_pointee[W(1)] = W(3);
                break;
            case OP_VARIABLE:
                if (wc >= 4u && W(2) < MAXID) {
                    is_var[W(2)] = 1; var_ptr[W(2)] = W(1); var_sc[W(2)] = W(3);
                }
                break;
            default: break;
        }
        #undef W
        i += wc;
    }

    /* Emit a binding for every variable carrying BOTH set + binding. */
    for (uint32_t id = 0; id < MAXID; id++) {
        if (!is_var[id] || !has_set[id] || !has_bind[id]) continue;
        uint32_t ptr = var_ptr[id];
        uint32_t pointee = (ptr < MAXID) ? ptr_pointee[ptr] : 0u;
        lagfx_spv_binding_kind_t kind = LAGFX_SPV_BINDING_UNKNOWN;
        if (pointee < MAXID) {
            if (is_image[pointee] || is_sampled_image[pointee])
                kind = LAGFX_SPV_BINDING_SAMPLED_IMAGE;
            else if (is_sampler[pointee])
                kind = LAGFX_SPV_BINDING_SAMPLER;
            else if (is_struct[pointee] && var_sc[id] == STORAGE_STORAGE_BUFFER)
                kind = LAGFX_SPV_BINDING_STORAGE_BUFFER;
        }
        if (kind == LAGFX_SPV_BINDING_UNKNOWN) continue;
        if (total < cap) {
            out[total].set = dec_set[id];
            out[total].binding = dec_bind[id];
            out[total].kind = kind;
        }
        total++;
    }

    /* Insertion-sort the emitted prefix by (set, binding) for a stable,
     * caller-friendly order (descriptor-set layout binding arrays want
     * ascending bindings). Only the prefix actually written is sorted. */
    size_t n = total < cap ? total : cap;
    for (size_t a = 1; a < n; a++) {
        lagfx_spv_binding_t key = out[a];
        size_t b = a;
        while (b > 0 &&
               (out[b - 1].set > key.set ||
                (out[b - 1].set == key.set && out[b - 1].binding > key.binding))) {
            out[b] = out[b - 1];
            b--;
        }
        out[b] = key;
    }

done:
    free(has_set); free(has_bind); free(is_var);
    free(dec_set); free(dec_bind); free(var_ptr); free(var_sc);
    free(ptr_pointee); free(is_image); free(is_sampler); free(is_struct);
    free(is_sampled_image);
    return total;
}

size_t lagfx_spv_reflect_vertex_inputs(const uint8_t *spv, size_t spv_len,
                                       lagfx_spv_vertex_input_t *out, size_t cap) {
    if (!spv || spv_len < 20u) return 0;
    if (lagfx_le32(spv) != SPV_MAGIC) return 0;
    size_t nwords = spv_len / 4u;
    size_t total = 0;

    uint8_t  *has_loc = calloc(MAXID, 1);
    uint32_t *dec_loc = calloc(MAXID, sizeof(uint32_t));
    uint8_t  *is_var  = calloc(MAXID, 1);
    uint32_t *var_ptr = calloc(MAXID, sizeof(uint32_t));
    uint32_t *var_sc  = calloc(MAXID, sizeof(uint32_t));
    uint32_t *ptr_pointee = calloc(MAXID, sizeof(uint32_t));
    uint8_t  *vec_comp = calloc(MAXID, 1);   /* OpTypeVector: component count */
    if (!has_loc || !dec_loc || !is_var || !var_ptr || !var_sc || !ptr_pointee || !vec_comp) {
        goto done;
    }

    for (size_t i = 5u; i < nwords; ) {
        uint32_t hdr = lagfx_le32(spv + i * 4u);
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u || i + wc > nwords) break;
        #define W(n) lagfx_le32(spv + (i + (n)) * 4u)
        switch (op) {
            case OP_DECORATE:
                if (wc >= 4u && W(1) < MAXID && W(2) == DECOR_LOCATION) {
                    has_loc[W(1)] = 1; dec_loc[W(1)] = W(3);
                }
                break;
            case OP_TYPE_POINTER:
                if (wc >= 4u && W(1) < MAXID) ptr_pointee[W(1)] = W(3);
                break;
            case OP_TYPE_VECTOR:
                /* W(1)=result id, W(3)=component count (W(2)=component type). */
                if (wc >= 4u && W(1) < MAXID) {
                    uint32_t c = W(3); vec_comp[W(1)] = (uint8_t)(c > 4u ? 4u : c);
                }
                break;
            case OP_TYPE_FLOAT:
                /* A scalar float type as a vertex input → 1 component. Mark it
                 * via vec_comp=1 so a non-vector float attribute still resolves. */
                if (wc >= 2u && W(1) < MAXID) vec_comp[W(1)] = 1u;
                break;
            case OP_VARIABLE:
                if (wc >= 4u && W(2) < MAXID) {
                    is_var[W(2)] = 1; var_ptr[W(2)] = W(1); var_sc[W(2)] = W(3);
                }
                break;
            default: break;
        }
        #undef W
        i += wc;
    }

    for (uint32_t id = 0; id < MAXID; id++) {
        if (!is_var[id] || var_sc[id] != STORAGE_INPUT || !has_loc[id]) continue;
        uint32_t ptr = var_ptr[id];
        uint32_t pointee = (ptr < MAXID) ? ptr_pointee[ptr] : 0u;
        uint32_t comp = (pointee < MAXID) ? vec_comp[pointee] : 0u;
        if (comp == 0u) comp = 4u; /* default to vec4 if the type wasn't a vector/float */
        if (total < cap) {
            out[total].location = dec_loc[id];
            out[total].components = comp;
        }
        total++;
    }

    /* Sort the written prefix by location. */
    size_t n = total < cap ? total : cap;
    for (size_t a = 1; a < n; a++) {
        lagfx_spv_vertex_input_t key = out[a];
        size_t b = a;
        while (b > 0 && out[b - 1].location > key.location) { out[b] = out[b - 1]; b--; }
        out[b] = key;
    }

done:
    free(has_loc); free(dec_loc); free(is_var); free(var_ptr); free(var_sc);
    free(ptr_pointee); free(vec_comp);
    return total;
}
