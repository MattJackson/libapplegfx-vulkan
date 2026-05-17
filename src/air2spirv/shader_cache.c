/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "air2spirv/shader_cache.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buflen;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->buflen = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *in = (const uint8_t *)data;
    while (len > 0) {
        size_t copy = (64 - ctx->buflen < len) ? (64 - ctx->buflen) : len;
        memcpy(ctx->buf + ctx->buflen, in, copy);
        ctx->buflen += copy;
        in += copy;
        len -= copy;
        if (ctx->buflen == 64) {
            uint32_t w[64];
            size_t i;

            for (i = 0; i < 16; ++i) {
                w[i] = ((uint32_t)ctx->buf[i*4]<<24)|((uint32_t)ctx->buf[i*4+1]<<16)|
                       ((uint32_t)ctx->buf[i*4+2]<<8)|(uint32_t)ctx->buf[i*4+3];
            }

            for (i = 16; i < 64; ++i) {
                #define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
                #define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
                #define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))
                w[i] = SIG0(w[i-15]) + w[i-7] + SIG1(w[i-2]) + w[i-16];
            }

            uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
            uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], hh = ctx->h[7];

            static const uint32_t k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab125f0c,
                0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,
                0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,
                0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,
                0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,
                0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,
                0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,
                0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
            };

            #define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
            #define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
            #define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
            #define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))

            for (i = 0; i < 64; ++i) {
                uint32_t t1 = hh + EP1(e) + CH(e,f,g) + k[i] + w[i&15];
                uint32_t t2 = EP0(a) + MAJ(a,b,c);
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }

            ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
            ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += hh;

            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]) {
    uint32_t i;

    ctx->buf[ctx->buflen++] = 0x80;

    if (ctx->buflen > 56) {
        while (ctx->buflen < 64) sha256_update(ctx, "", 1);
    } else {
        while (ctx->buflen < 56) sha256_update(ctx, "", 1);
    }

    for (i = 0; i < 8; ++i) {
        uint32_t v = ctx->h[i];
        out[i*4+0] = (v>>24)&0xff; out[i*4+1] = (v>>16)&0xff;
        out[i*4+2] = (v>>8)&0xff;  out[i*4+3] = v&0xff;
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
