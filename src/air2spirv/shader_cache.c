/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "air2spirv/shader_cache.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
/* Non-Apple fallback: FNV-1a 64-bit → 256-bit key by quadruple-hashing
 * with different seeds. The cache only needs deterministic, collision-
 * resistant-enough keys — NOT cryptographic SHA-256. FNV-1a is dead
 * simple (no length-padding gotchas, no message-schedule bugs) and the
 * cache key space is ≤ a few hundred entries per session. Cross-host
 * cache transfer isn't a feature we support, so the hash function
 * being non-canonical is fine. */
typedef struct {
    uint64_t h[4];  /* four independent FNV-1a accumulators with different seeds */
} sha256_ctx_t;  /* name kept for callsite compatibility */

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->h[0] = 0xcbf29ce484222325ull;  /* FNV-1a 64 offset basis */
    ctx->h[1] = 0xcbf29ce484222324ull;  /* off-by-one variants for independence */
    ctx->h[2] = 0xcbf29ce484222326ull;
    ctx->h[3] = 0xcbf29ce484222323ull;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *in = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        ctx->h[0] = (ctx->h[0] ^ in[i]) * 0x100000001b3ull;
        ctx->h[1] = (ctx->h[1] ^ (in[i] + 1)) * 0x100000001b3ull;
        ctx->h[2] = (ctx->h[2] ^ (in[i] + 2)) * 0x100000001b3ull;
        ctx->h[3] = (ctx->h[3] ^ (in[i] + 3)) * 0x100000001b3ull;
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]) {
    for (int k = 0; k < 4; ++k) {
        uint64_t v = ctx->h[k];
        for (int b = 0; b < 8; ++b) {
            out[k*8 + b] = (uint8_t)((v >> (56 - 8*b)) & 0xff);
        }
    }
}

#endif


typedef struct {
    uint8_t key[32];
    lagfx_shader_translation_t value;
    uint64_t lru_counter;
    int used;
} cache_entry_t;

struct lagfx_shader_cache {
    size_t max_entries;
    size_t count;
    cache_entry_t *entries;
    uint64_t next_lru;
    size_t hits;
    size_t misses;
    size_t evictions;
};


static void make_key(const uint8_t *metallib_data, size_t metallib_len,
                      const char *function_name, lagfx_shader_stage_t stage,
                      uint8_t key[32]) {
#if defined(__APPLE__)
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, metallib_data, (CC_LONG)metallib_len);

    const char *fn = function_name ? function_name : "";
    CC_SHA256_Update(&c, fn, (CC_LONG)strlen(fn));
    
    uint8_t stage_byte = (uint8_t)stage;
    CC_SHA256_Update(&c, &stage_byte, 1);

    CC_SHA256_Final(key, &c);
#else
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, metallib_data, metallib_len);

    const char *fn = function_name ? function_name : "";
    sha256_update(&ctx, fn, strlen(fn));
    
    uint8_t stage_byte = (uint8_t)stage;
    sha256_update(&ctx, &stage_byte, 1);

    sha256_final(&ctx, key);
#endif
}

static int find_entry(lagfx_shader_cache_t *cache, const uint8_t *key) {
    size_t i;
    for (i = 0; i < cache->count; ++i) {
        if (memcmp(cache->entries[i].key, key, 32) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t find_lru_entry(lagfx_shader_cache_t *cache) {
    size_t idx = 0;
    uint64_t min_lru = cache->entries[0].lru_counter;
    size_t i;
    for (i = 1; i < cache->count; ++i) {
        if (cache->entries[i].lru_counter < min_lru) {
            min_lru = cache->entries[i].lru_counter;
            idx = i;
        }
    }
    return idx;
}

lagfx_shader_cache_t *lagfx_shader_cache_new(size_t max_entries) {
    if (max_entries == 0) {
        max_entries = 64;
    }

    lagfx_shader_cache_t *cache = (lagfx_shader_cache_t *)calloc(1, sizeof(lagfx_shader_cache_t));
    if (!cache) {
        return NULL;
    }

    cache->entries = (cache_entry_t *)calloc(max_entries, sizeof(cache_entry_t));
    if (!cache->entries) {
        free(cache);
        return NULL;
    }

    cache->max_entries = max_entries;
    cache->count = 0;
    cache->next_lru = 1;

    return cache;
}

lagfx_status_t lagfx_shader_cache_get(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      lagfx_shader_translation_t *out) {
    if (!cache || !metallib_data || !function_name || !out) {
        return LAGFX_ERR_INVALID_ARG;
    }

    uint8_t key[32];
    make_key(metallib_data, metallib_len, function_name, stage, key);

    int idx = find_entry(cache, key);
    if (idx < 0) {
        cache->misses++;
        return LAGFX_ERR_NOT_FOUND;
    }

    cache->hits++;
    
    cache->entries[idx].used = 1;
    cache->entries[idx].lru_counter = cache->next_lru++;

    memcpy(out, &cache->entries[idx].value, sizeof(lagfx_shader_translation_t));
    return LAGFX_OK;
}

lagfx_status_t lagfx_shader_cache_put(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      const lagfx_shader_translation_t *translation) {
    if (!cache || !metallib_data || !function_name || !translation || !translation->spv_bytes) {
        return LAGFX_ERR_INVALID_ARG;
    }

    uint8_t key[32];
    make_key(metallib_data, metallib_len, function_name, stage, key);

    int existing = find_entry(cache, key);
    
    if (existing >= 0) {
        /* Replace existing entry */
        free((void *)cache->entries[existing].value.spv_bytes);
        memcpy(cache->entries[existing].key, key, 32);
        cache->entries[existing].value.spv_bytes = translation->spv_bytes;
        cache->entries[existing].value.spv_len = translation->spv_len;
        cache->entries[existing].value.stage = translation->stage;
        cache->entries[existing].lru_counter = cache->next_lru++;
        cache->entries[existing].used = 1;
    } else if (cache->count >= cache->max_entries) {
        /* Evict LRU and insert */
        size_t lru_idx = find_lru_entry(cache);
        free((void *)cache->entries[lru_idx].value.spv_bytes);
        memcpy(cache->entries[lru_idx].key, key, 32);
        cache->entries[lru_idx].value.spv_bytes = translation->spv_bytes;
        cache->entries[lru_idx].value.spv_len = translation->spv_len;
        cache->entries[lru_idx].value.stage = translation->stage;
        cache->entries[lru_idx].lru_counter = cache->next_lru++;
        cache->entries[lru_idx].used = 1;
        cache->evictions++;
    } else {
        /* Insert into new slot */
        size_t insert_idx = cache->count;
        memcpy(cache->entries[insert_idx].key, key, 32);
        cache->entries[insert_idx].value.spv_bytes = translation->spv_bytes;
        cache->entries[insert_idx].value.spv_len = translation->spv_len;
        cache->entries[insert_idx].value.stage = translation->stage;
        cache->entries[insert_idx].lru_counter = cache->next_lru++;
        cache->entries[insert_idx].used = 1;
        cache->count++;
    }

   return LAGFX_OK;
}

void lagfx_shader_cache_close(lagfx_shader_cache_t *cache) {
    if (!cache) {
        return;
    }

    size_t i;
    for (i = 0; i < cache->count; ++i) {
        free((void *)cache->entries[i].value.spv_bytes);
    }

    free(cache->entries);
    free(cache);
}

void lagfx_shader_cache_stats(const lagfx_shader_cache_t *cache,
                              lagfx_shader_cache_stats_t *out) {
    if (!cache || !out) {
        return;
    }

    out->entries = cache->count;
    out->hits = cache->hits;
    out->misses = cache->misses;
    out->evictions = cache->evictions;
}
