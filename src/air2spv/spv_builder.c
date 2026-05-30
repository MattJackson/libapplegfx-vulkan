/*
 * libapplegfx-vulkan — SPIR-V module builder
 * src/air2spv/spv_builder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "spv_builder.h"

#include <stdlib.h>
#include <string.h>

#define LAGFX_SPV_MAGIC      0x07230203u
#define LAGFX_SPV_VERSION_10 0x00010000u  /* version 1.0 */
/* SPIR-V 1.4: enables scalar-bool OpSelect over vector operands (Metal's
 * `cond ? vec : vec` ternary), avoiding a bool-vector splat. Backward
 * compatible with 1.0 constructs; the only globals we emit are Input/Output
 * (already listed in OpEntryPoint, satisfying the stricter 1.4 interface
 * rule). The deploy target (container llvmpipe, Vulkan 1.3) accepts <=1.6. */
#define LAGFX_SPV_VERSION_14 0x00010400u  /* version 1.4 */
#define LAGFX_SPV_GENERATOR  0u           /* unregistered generator id */
#define LAGFX_SPV_SCHEMA     0u

/* SPIR-V requires a strict module section order: capabilities / extensions /
 * ext-inst-imports / memory-model / entry-points / execution-modes / debug,
 * then ANNOTATIONS (decorations), then TYPES+CONSTANTS+global-VARIABLES, then
 * FUNCTIONS. The translator, however, emits in a convenient order and often
 * materialises a type/constant lazily WHILE translating a function body
 * (e.g. a GEP result pointer, a splat constant). To keep that ergonomic
 * without scattering type-defs into the function bodies (spirv-val: "OpType*
 * cannot appear in the graph definitions section"), the builder routes every
 * instruction into one of four section streams by opcode and concatenates
 * them in spec order at finish time. So a type emitted mid-body is
 * automatically hoisted into the types section. */
enum { SEC_CAPS = 0, SEC_PREAMBLE, SEC_ANNOT, SEC_TYPES, SEC_FUNCS, SEC_COUNT };

typedef struct { uint32_t *w; uint32_t n; uint32_t cap; } spv_section;

struct lagfx_spv_builder {
    spv_section sec[SEC_COUNT];
    uint32_t    next_id;  /* next SSA id; spec reserves 0, start at 1 */
};

/* Classify an opcode (+ operands for the context-sensitive ones) into a
 * section. OpVariable is a global (types section) unless Function-storage
 * (a local, stays in the body). OpUndef is valid at module scope, so hoist
 * it to the types section too. */
static int section_of(uint32_t opcode, const uint32_t *operands, uint32_t n) {
    switch (opcode) {
        case 17u: /* OpCapability — must precede everything; own section so a
                   * lazily-emitted capability still lands before OpEntryPoint */
        case 10u: /* OpExtension */
            return SEC_CAPS;
        case 11u: /* OpExtInstImport */
        case 14u: /* OpMemoryModel */
        case 15u: /* OpEntryPoint */
        case 16u: /* OpExecutionMode */
        case 331u:/* OpExecutionModeId */
        case 5u:  /* OpName */
        case 6u:  /* OpMemberName */
        case 7u:  /* OpString */
        case 8u:  /* OpLine */
            return SEC_PREAMBLE;
        case 71u: /* OpDecorate */
        case 72u: /* OpMemberDecorate */
        case 73u: /* OpDecorationGroup */
        case 74u: /* OpGroupDecorate */
        case 75u: /* OpGroupMemberDecorate */
            return SEC_ANNOT;
        case 59u: /* OpVariable: [result-type][result][storage-class] */
            return (n >= 3u && operands[2] != LAGFX_SPV_STORAGE_FUNCTION)
                       ? SEC_TYPES : SEC_FUNCS;
        case 1u:  /* OpUndef — module-scope-legal; hoist with the types */
            return SEC_TYPES;
        default:
            /* OpType* (19..39) and OpConstant / OpSpecConstant (41..52). */
            if ((opcode >= 19u && opcode <= 39u) ||
                (opcode >= 41u && opcode <= 52u))
                return SEC_TYPES;
            return SEC_FUNCS;
    }
}

lagfx_spv_builder_t *
lagfx_spv_builder_create(uint32_t initial_word_capacity) {
    if (initial_word_capacity < 16u) initial_word_capacity = 16u;
    lagfx_spv_builder_t *b = (lagfx_spv_builder_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    for (int s = 0; s < SEC_COUNT; s++) {
        b->sec[s].w = (uint32_t *)malloc(initial_word_capacity * sizeof(uint32_t));
        if (!b->sec[s].w) {
            for (int k = 0; k < s; k++) free(b->sec[k].w);
            free(b); return NULL;
        }
        b->sec[s].cap = initial_word_capacity;
        b->sec[s].n = 0u;
    }
    b->next_id = 1u;
    return b;
}

void
lagfx_spv_builder_free(lagfx_spv_builder_t *b) {
    if (!b) return;
    for (int s = 0; s < SEC_COUNT; s++) free(b->sec[s].w);
    free(b);
}

uint32_t
lagfx_spv_builder_alloc_id(lagfx_spv_builder_t *b) {
    return b->next_id++;
}

static bool sec_grow(spv_section *sec, uint32_t needed) {
    if (sec->cap >= needed) return true;
    uint32_t new_cap = sec->cap ? sec->cap * 2u : 16u;
    while (new_cap < needed) new_cap *= 2u;
    uint32_t *nw = (uint32_t *)realloc(sec->w, new_cap * sizeof(uint32_t));
    if (!nw) return false;
    sec->w = nw; sec->cap = new_cap;
    return true;
}

static bool sec_push(spv_section *sec, uint32_t w) {
    if (!sec_grow(sec, sec->n + 1u)) return false;
    sec->w[sec->n++] = w;
    return true;
}

bool
lagfx_spv_builder_emit_word(lagfx_spv_builder_t *b, uint32_t w) {
    /* Raw word append — routed to the function/body stream (only caller
     * context that uses raw words). */
    return sec_push(&b->sec[SEC_FUNCS], w);
}

bool
lagfx_spv_builder_emit_op(lagfx_spv_builder_t *b,
                            uint32_t opcode,
                            const uint32_t *operands,
                            uint32_t num_operands) {
    /* SPIR-V instruction header: high 16 bits = word count (1 + N for
     * operands), low 16 bits = opcode. */
    uint32_t word_count = 1u + num_operands;
    uint32_t header = (word_count << 16) | (opcode & 0xFFFFu);
    spv_section *sec = &b->sec[section_of(opcode, operands, num_operands)];
    if (!sec_grow(sec, sec->n + word_count)) return false;
    sec->w[sec->n++] = header;
    for (uint32_t i = 0; i < num_operands; i++) sec->w[sec->n++] = operands[i];
    return true;
}

bool
lagfx_spv_builder_emit_op_string(lagfx_spv_builder_t *b,
                                   uint32_t opcode,
                                   const uint32_t *prefix_operands,
                                   uint32_t        num_prefix,
                                   const char     *str,
                                   const uint32_t *suffix_operands,
                                   uint32_t        num_suffix) {
    /* String packing: 4 chars per 32-bit word, little-endian, with a
     * NUL terminator and zero padding to the next 4-byte boundary. */
    size_t slen = str ? strlen(str) : 0u;
    uint32_t str_words = (uint32_t)((slen / 4u) + 1u);  /* +1 always for terminator */

    uint32_t word_count = 1u + num_prefix + str_words + num_suffix;
    /* The only string-bearing op we emit is OpEntryPoint (preamble). */
    spv_section *sec = &b->sec[section_of(opcode, prefix_operands, num_prefix)];
    if (!sec_grow(sec, sec->n + word_count)) return false;

    uint32_t header = (word_count << 16) | (opcode & 0xFFFFu);
    sec->w[sec->n++] = header;
    for (uint32_t i = 0; i < num_prefix; i++) sec->w[sec->n++] = prefix_operands[i];
    /* Pack the string. */
    for (uint32_t w = 0; w < str_words; w++) {
        uint32_t word = 0u;
        for (uint32_t byte = 0; byte < 4u; byte++) {
            size_t idx = (size_t)w * 4u + byte;
            uint8_t c = (idx < slen) ? (uint8_t)str[idx] : 0u;
            word |= ((uint32_t)c) << (byte * 8u);
        }
        sec->w[sec->n++] = word;
    }
    for (uint32_t i = 0; i < num_suffix; i++) sec->w[sec->n++] = suffix_operands[i];
    return true;
}

uint8_t *
lagfx_spv_builder_finish(const lagfx_spv_builder_t *b, size_t *out_size_bytes) {
    /* Header is 5 words: magic, version, generator, bound, schema.
     * Bound must be > every id used, so set to next_id (which is the
     * id that WOULD be handed out next — exactly the right bound). */
    uint32_t bound = b->next_id;
    size_t body_words = 0u;
    for (int s = 0; s < SEC_COUNT; s++) body_words += b->sec[s].n;
    size_t total_words = 5u + body_words;
    size_t total_bytes = total_words * sizeof(uint32_t);
    uint8_t *out = (uint8_t *)malloc(total_bytes);
    if (!out) return NULL;
    uint32_t *p = (uint32_t *)out;
    p[0] = LAGFX_SPV_MAGIC;
    p[1] = LAGFX_SPV_VERSION_14;
    p[2] = LAGFX_SPV_GENERATOR;
    p[3] = bound;
    p[4] = LAGFX_SPV_SCHEMA;
    /* Concatenate the section streams in SPIR-V module order. */
    uint32_t *dst = p + 5;
    for (int s = 0; s < SEC_COUNT; s++) {
        memcpy(dst, b->sec[s].w, b->sec[s].n * sizeof(uint32_t));
        dst += b->sec[s].n;
    }
    if (out_size_bytes) *out_size_bytes = total_bytes;
    return out;
}
