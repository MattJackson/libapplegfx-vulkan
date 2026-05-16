/*
 * libapplegfx-vulkan — MTLB container parser (metallib)
 * src/metallib/metallib_reader.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "metallib_reader.h"
#include "../common/le.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

struct lagfx_metallib {
    const uint8_t *data;
    size_t         len;

    lagfx_metallib_function_t *funcs;
    size_t                     n_funcs;
    size_t                     funcs_cap;

    char  *names;
    size_t names_len;
    size_t names_cap;

    size_t bitcode_offset;
    size_t bitcode_length;
};

static int copy_into_names(lagfx_metallib_t *ml, const char *str, size_t len) {
    if (len == 0) return 0;

    size_t needed = len + 1; /* include NUL */
    if (needed > ml->names_cap - ml->names_len) {
        size_t new_cap = ml->names_cap * 2;
        if (new_cap < needed) new_cap = needed;
        char *new_names = realloc(ml->names, new_cap);
        if (!new_names) return -1;
        ml->names = new_names;
        ml->names_cap = new_cap;
    }

    memcpy(ml->names + ml->names_len, str, len);
    ml->names[ml->names_len + len] = '\0';
    ml->names_len += needed;
    return 0;
}

static int grow_functions(lagfx_metallib_t *ml) {
    size_t new_cap = ml->funcs_cap == 0 ? 16 : ml->funcs_cap * 2;
    lagfx_metallib_function_t *new_funcs = realloc(ml->funcs, new_cap * sizeof(*ml->funcs));
    if (!new_funcs) return -1;
    ml->funcs = new_funcs;
    ml->funcs_cap = new_cap;
    return 0;
}

lagfx_metallib_t *lagfx_metallib_open(const uint8_t *data, size_t len) {
    if (!data || len < 88) {
        LAGFX_ERR("metallib_reader: data too short (len=%zu, need >= 88)", len);
        return NULL;
    }

    lagfx_metallib_t *ml = calloc(1, sizeof(*ml));
    if (!ml) {
        LAGFX_ERR("metallib_reader: failed to allocate metallib struct");
        return NULL;
    }

    ml->data   = data;
    ml->len    = len;

    /* Validate magic at +0x00 == 'MTLB' (LE u32) */
    uint32_t magic = lagfx_le32(data + 0x00);
    if (magic != 0x424c544d) { /* MTLB in LE */
        LAGFX_ERR("metallib_reader: invalid magic 0x%08x, expected 0x424c544d", magic);
        free(ml);
        return NULL;
    }

    /* Read file_size at +0x10 (u64 LE). Warn if mismatch but continue. */
    uint64_t file_size = lagfx_le64(data + 0x10);
    if (file_size != len) {
        LAGFX_WARN("metallib_reader: file_size (%zu) != input len (%zu)",
                   (size_t)file_size, len);
    }

    /* Read function metadata offset at +0x18 */
    uint64_t fn_meta_offset = lagfx_le64(data + 0x18);
    if (fn_meta_offset < 88 || fn_meta_offset >= len) {
        LAGFX_ERR("metallib_reader: invalid function metadata offset 0x%llx",
                  (unsigned long long)fn_meta_offset);
        free(ml);
        return NULL;
    }

    /* Read bitcode offset at +0x48 */
    uint64_t bitcode_off = lagfx_le64(data + 0x48);
    if (bitcode_off >= len) {
        LAGFX_ERR("metallib_reader: invalid bitcode offset 0x%llx",
                  (unsigned long long)bitcode_off);
        free(ml);
        return NULL;
    }

    /* Read bitcode length at +0x50, cap at remaining data */
    uint64_t bitcode_len = lagfx_le64(data + 0x50);
    size_t max_bitcode_len = len - (size_t)bitcode_off;
    if (bitcode_len > max_bitcode_len) {
        bitcode_len = max_bitcode_len;
    }

    ml->bitcode_offset = (size_t)bitcode_off;
    ml->bitcode_length = (size_t)bitcode_len;

    /* Walk TLV section from fn_meta_offset */
    size_t offset = (size_t)fn_meta_offset;
    size_t stop_at = ml->bitcode_offset;

    ml->funcs_cap = 0;
    ml->funcs     = NULL;
    ml->names     = malloc(4096);
    if (!ml->names) {
        LAGFX_ERR("metallib_reader: failed to allocate names buffer");
        free(ml);
        return NULL;
    }
    ml->names_len   = 0;
    ml->names_cap   = 4096;

    while (offset + 8 <= stop_at && offset < len) {
        /* Check for tag start: need at least 4 bytes for ASCII tag */
        if (offset + 4 > stop_at) break;

        uint32_t tag = lagfx_le32(data + offset);

        /* Validate tag is ASCII (all bytes in printable range) */
        uint8_t *tag_bytes = (uint8_t *)&tag;
        int ascii_tag = 1;
        for (int i = 0; i < 4 && tag_bytes[i] != '\0'; ++i) {
            if (tag_bytes[i] < 32 || tag_bytes[i] > 126) {
                ascii_tag = 0;
                break;
            }
        }

        if (!ascii_tag) break; /* end of TLV section */

        /* Read length field at offset+4 (u16 LE) */
        if (offset + 6 > stop_at) break;
        uint16_t tlv_len = lagfx_le16(data + offset + 4);

        /* Sanity check: tag (4) + len (2) + value must be within bounds */
        if (offset + 6 + tlv_len > stop_at || offset + 6 + tlv_len > len) {
            break;
        }

        const uint8_t *value = data + offset + 6;

        /* Process known tags: NAME, TYPE, HASH */
        int name_done = 0, type_done = 0, hash_done = 0;
        lagfx_metallib_function_t *fn = NULL;

        if (tag == 0x454d414e) { /* 'NAME' in LE = 0x4E414D45 */
            /* Copy string into names buffer */
            if (copy_into_names(ml, (const char *)value, tlv_len) == 0) {
                /* Point funcs[n_funcs].name at it later when we advance n_funcs */
                name_done = 1;
            }
        } else if (tag == 0x45505954) { /* 'TYPE' in LE = 0x54595045 */
            if (tlv_len >= 1 && ml->n_funcs < ml->funcs_cap) {
                fn = &ml->funcs[ml->n_funcs];
                fn->type_code = value[0];
                type_done = 1;
            }
        } else if (tag == 0x48534148) { /* 'HASH' in LE = 0x48415348 */
            if (tlv_len >= 32 && ml->n_funcs < ml->funcs_cap) {
                fn = &ml->funcs[ml->n_funcs];
                memcpy(fn->hash, value, 32);
                hash_done = 1;
            }
        }

        /* Track name pointer when we see it */
        if (name_done && !type_done && !hash_done && ml->n_funcs < ml->funcs_cap) {
            /* Find last string in names buffer */
            size_t last_name_start = 0;
            const char *ptr = ml->names;
            while (*ptr) {
                ptr += strlen(ptr) + 1;
                last_name_start = (size_t)(ptr - ml->names);
            }
            /* The current NAME should be the last one added */
            if (ml->n_funcs < ml->funcs_cap) {
                lagfx_metallib_function_t *fn_tmp = &ml->funcs[ml->n_funcs];
                fn_tmp->name = ml->names + last_name_start;
            }
        }

        /* If we have all three fields, advance n_funcs */
        if (name_done && type_done && hash_done) {
            if (ml->n_funcs >= ml->funcs_cap) {
                if (grow_functions(ml) < 0) {
                    LAGFX_ERR("metallib_reader: failed to grow function array");
                    free(ml->names);
                    free(ml->funcs);
                    free(ml);
                    return NULL;
                }
            }

            /* Set name for this entry (if not already set by NAME processing) */
            if (!ml->funcs[ml->n_funcs].name) {
                size_t last_name_start = 0;
                const char *ptr = ml->names;
                while (*ptr) {
                    ptr += strlen(ptr) + 1;
                    last_name_start = (size_t)(ptr - ml->names);
                }
                ml->funcs[ml->n_funcs].name = ml->names + last_name_start;
            }

            /* Sanity: ensure name is valid */
            if (!ml->funcs[ml->n_funcs].name) {
                LAGFX_ERR("metallib_reader: name not set for function %zu", ml->n_funcs);
                break;
            }

            ml->n_funcs++;
        }

        /* Advance to next TLV entry */
        offset += 6 + tlv_len;

        /* Sentinel check: if tag is not one of NAME/TYPE/HASH and not ASCII, stop */
        if (tag != 0x454d414e && tag != 0x45505954 && tag != 0x48534148) {
            /* Check if this is a known sentinel or end marker */
            /* For now, continue walking until we hit bitcode section */
        }

        /* Stop if we've gone too far past expected function metadata area */
        if (offset > stop_at + 256) break;
    }

    LAGFX_LOG("metallib_reader: parsed mtlb, n_funcs=%zu, bitcode=%zu bytes at +0x%zx",
              ml->n_funcs, ml->bitcode_length, ml->bitcode_offset);

    return ml;
}

size_t lagfx_metallib_list_functions(lagfx_metallib_t *ml,
                                      lagfx_metallib_function_t *out_funcs,
                                      size_t cap) {
    if (!ml || !out_funcs) return 0;

    size_t count = ml->n_funcs;
    if (cap < count) count = cap;

    for (size_t i = 0; i < count; ++i) {
        out_funcs[i] = ml->funcs[i];
    }

    return count;
}

size_t lagfx_metallib_get_bitcode(lagfx_metallib_t *ml,
                                   const char *name,
                                   const uint8_t **out_bytes) {
    if (!ml || !out_bytes || !name) return 0;

    /* MVP: hard-code SimpleVertexShadow lookup by matching first function */
    if (ml->n_funcs == 0) return 0;

    for (size_t i = 0; i < ml->n_funcs; ++i) {
        if (strcmp(ml->funcs[i].name, name) == 0) {
            *out_bytes = ml->data + ml->bitcode_offset;
            return ml->bitcode_length;
        }
    }

    /* Name not found */
    return 0;
}

void lagfx_metallib_close(lagfx_metallib_t *ml) {
    if (!ml) return;
    free(ml->funcs);
    free(ml->names);
    free(ml);
}
