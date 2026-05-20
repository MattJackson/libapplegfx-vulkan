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
#define LAGFX_SPV_GENERATOR  0u           /* unregistered generator id */
#define LAGFX_SPV_SCHEMA     0u

struct lagfx_spv_builder {
    uint32_t *words;
    uint32_t  num_words;
    uint32_t  cap_words;
    uint32_t  next_id;   /* next SSA id to hand out; spec reserves 0, start at 1 */
};

lagfx_spv_builder_t *
lagfx_spv_builder_create(uint32_t initial_word_capacity) {
    if (initial_word_capacity < 16u) initial_word_capacity = 16u;
    lagfx_spv_builder_t *b = (lagfx_spv_builder_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->words = (uint32_t *)malloc(initial_word_capacity * sizeof(uint32_t));
    if (!b->words) { free(b); return NULL; }
    b->cap_words = initial_word_capacity;
    b->num_words = 0u;
    b->next_id = 1u;
    return b;
}

void
lagfx_spv_builder_free(lagfx_spv_builder_t *b) {
    if (!b) return;
    free(b->words);
    free(b);
}

uint32_t
lagfx_spv_builder_alloc_id(lagfx_spv_builder_t *b) {
    return b->next_id++;
}

static bool grow_to(lagfx_spv_builder_t *b, uint32_t needed) {
    if (b->cap_words >= needed) return true;
    uint32_t new_cap = b->cap_words * 2u;
    while (new_cap < needed) new_cap *= 2u;
    uint32_t *nw = (uint32_t *)realloc(b->words, new_cap * sizeof(uint32_t));
    if (!nw) return false;
    b->words = nw;
    b->cap_words = new_cap;
    return true;
}

bool
lagfx_spv_builder_emit_word(lagfx_spv_builder_t *b, uint32_t w) {
    if (!grow_to(b, b->num_words + 1u)) return false;
    b->words[b->num_words++] = w;
    return true;
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
    if (!grow_to(b, b->num_words + word_count)) return false;
    b->words[b->num_words++] = header;
    for (uint32_t i = 0; i < num_operands; i++) {
        b->words[b->num_words++] = operands[i];
    }
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
    if (!grow_to(b, b->num_words + word_count)) return false;

    uint32_t header = (word_count << 16) | (opcode & 0xFFFFu);
    b->words[b->num_words++] = header;
    for (uint32_t i = 0; i < num_prefix; i++) {
        b->words[b->num_words++] = prefix_operands[i];
    }
    /* Pack the string. */
    for (uint32_t w = 0; w < str_words; w++) {
        uint32_t word = 0u;
        for (uint32_t byte = 0; byte < 4u; byte++) {
            size_t idx = (size_t)w * 4u + byte;
            uint8_t c = (idx < slen) ? (uint8_t)str[idx] : 0u;
            word |= ((uint32_t)c) << (byte * 8u);
        }
        b->words[b->num_words++] = word;
    }
    for (uint32_t i = 0; i < num_suffix; i++) {
        b->words[b->num_words++] = suffix_operands[i];
    }
    return true;
}

uint8_t *
lagfx_spv_builder_finish(const lagfx_spv_builder_t *b, size_t *out_size_bytes) {
    /* Header is 5 words: magic, version, generator, bound, schema.
     * Bound must be > every id used, so set to next_id (which is the
     * id that WOULD be handed out next — exactly the right bound). */
    uint32_t bound = b->next_id;
    size_t total_words = 5u + b->num_words;
    size_t total_bytes = total_words * sizeof(uint32_t);
    uint8_t *out = (uint8_t *)malloc(total_bytes);
    if (!out) return NULL;
    uint32_t *p = (uint32_t *)out;
    p[0] = LAGFX_SPV_MAGIC;
    p[1] = LAGFX_SPV_VERSION_10;
    p[2] = LAGFX_SPV_GENERATOR;
    p[3] = bound;
    p[4] = LAGFX_SPV_SCHEMA;
    memcpy(p + 5, b->words, b->num_words * sizeof(uint32_t));
    if (out_size_bytes) *out_size_bytes = total_bytes;
    return out;
}
